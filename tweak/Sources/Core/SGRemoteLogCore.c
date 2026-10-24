// See SGRemoteLogCore.h. Compiled only into IPLOG builds (make IPLOG=1).
#if SG_IPLOG

#include "SGRemoteLogCore.h"
#include <arpa/inet.h>
#include <errno.h>
#include <execinfo.h>
#include <fcntl.h>
#include <netdb.h>
#include <netinet/in.h>
#include <netinet/tcp.h>
#include <poll.h>
#include <pthread.h>
#include <signal.h>
#include <stdint.h>
#include <stdlib.h>
#include <string.h>
#include <sys/socket.h>
#include <sys/time.h>
#include <sys/types.h>
#include <time.h>
#include <unistd.h>
#ifdef __APPLE__
#include <mach-o/dyld.h>
#include <mach-o/loader.h>
#include <mach/mach.h>
#include <sys/ucontext.h>
#endif

enum { kBacklogMax = 500, kMessageMax = 16 * 1024, kRecentN = 40, kRecentLen = 200, kFramesMax = 64 };

static pthread_mutex_t sg_lock = PTHREAD_MUTEX_INITIALIZER;
static volatile int sg_fd = -1;          // the connected socket, -1 when there is none
static char sg_host[128];
static int sg_port = 9000;
static unsigned sg_hostGeneration;       // bumped whenever the host changes, so a connection to the old one is dropped
static char sg_hello[512];
static char sg_crashPath[512], sg_statePath[512];
static char sg_state[24] = "launching";
static time_t sg_startTime;

static char *sg_backlog[kBacklogMax];
static int sg_backlogHead, sg_backlogCount;

static char sg_recent[kRecentN][kRecentLen];
static volatile unsigned sg_recentNext;

// ---------------------------------------------------------------------------------------------
// Small formatters that need no libc state, for the signal handler.

static size_t fmt_u(char *out, unsigned long long v) {
    char tmp[24];
    size_t n = 0, i;
    do { tmp[n++] = (char)('0' + v % 10); v /= 10; } while (v);
    for (i = 0; i < n; i++) out[i] = tmp[n - 1 - i];
    return n;
}

static size_t fmt_hex(char *out, unsigned long long v, int width) {
    static const char digits[] = "0123456789abcdef";
    size_t n = 0;
    int i;
    out[n++] = '0';
    out[n++] = 'x';
    for (i = (width - 1) * 4; i >= 0; i -= 4) out[n++] = digits[(v >> i) & 0xF];
    return n;
}

static void sleep_ms(int ms) {
    struct timespec ts = { ms / 1000, (long)(ms % 1000) * 1000000L };
    nanosleep(&ts, NULL);
}

static int write_all(int fd, const char *p, size_t n, int isSocket) {
    while (n) {
        ssize_t r = isSocket ? send(fd, p, n, 0) : write(fd, p, n);
        if (r > 0) { p += r; n -= (size_t)r; continue; }
        if (r < 0 && errno == EINTR) continue;
        if (r < 0 && isSocket && (errno == EAGAIN || errno == EWOULDBLOCK)) {
            struct pollfd pf = { fd, POLLOUT, 0 };
            if (poll(&pf, 1, 300) <= 0) return 0;
            continue;
        }
        return 0;
    }
    return 1;
}

// ---------------------------------------------------------------------------------------------
// Log records.

static size_t escape(char *out, size_t room, const char *msg, size_t len) {
    size_t n = 0, i;
    for (i = 0; i < len && n + 2 < room; i++) {
        char c = msg[i];
        if (c == '\n') { out[n++] = '\\'; out[n++] = 'n'; }
        else if (c == '\r') { out[n++] = '\\'; out[n++] = 'r'; }
        else if (c == '\\') { out[n++] = '\\'; out[n++] = '\\'; }
        else out[n++] = c;
    }
    return n;
}

static void remember(const char *msg, size_t len) {
    struct timeval tv;
    struct tm tmv;
    char line[kRecentLen];
    size_t n = 0, i;
    unsigned slot;
    gettimeofday(&tv, NULL);
    localtime_r(&tv.tv_sec, &tmv);
    n = (size_t)strftime(line, sizeof line, "%H:%M:%S", &tmv);
    line[n++] = '.';
    line[n++] = (char)('0' + (tv.tv_usec / 100000) % 10);
    line[n++] = (char)('0' + (tv.tv_usec / 10000) % 10);
    line[n++] = (char)('0' + (tv.tv_usec / 1000) % 10);
    line[n++] = ' ';
    for (i = 0; i < len && n + 1 < sizeof line; i++) line[n++] = (msg[i] == '\n' || msg[i] == '\r' || msg[i] == '\t') ? ' ' : msg[i];
    line[n] = 0;
    slot = sg_recentNext % kRecentN;
    memcpy(sg_recent[slot], line, n + 1);
    sg_recentNext++;
}

// Sends under the lock. A connection that cannot take it is shut down; the connection thread then
// notices and starts over.
static int send_locked(const char *data, size_t n) {
    int fd = sg_fd;
    if (fd < 0) return 0;
    if (!write_all(fd, data, n, 1)) {
        shutdown(fd, SHUT_RDWR);
        return 0;
    }
    return 1;
}

void sgrl_log(const char *message, size_t length) {
    struct timeval tv;
    char *rec;
    size_t room, n;
    if (!message) return;
    if (length > kMessageMax) length = kMessageMax;
    gettimeofday(&tv, NULL);
    room = length * 2 + 64;
    rec = (char *)malloc(room);
    if (!rec) return;
    n = 0;
    memcpy(rec, "!LOG\t", 5); n = 5;
    n += fmt_u(rec + n, (unsigned long long)tv.tv_sec * 1000ull + (unsigned long long)tv.tv_usec / 1000ull);
    rec[n++] = '\t';
    n += escape(rec + n, room - n - 1, message, length);
    rec[n++] = '\n';

    pthread_mutex_lock(&sg_lock);
    remember(message, length);
    if (!send_locked(rec, n)) {
        // Keep it for the next connection, dropping the oldest when full.
        int slot = (sg_backlogHead + sg_backlogCount) % kBacklogMax;
        if (sg_backlogCount == kBacklogMax) {
            free(sg_backlog[sg_backlogHead]);
            sg_backlog[sg_backlogHead] = NULL;
            sg_backlogHead = (sg_backlogHead + 1) % kBacklogMax;
            sg_backlogCount--;
            slot = (sg_backlogHead + sg_backlogCount) % kBacklogMax;
        }
        sg_backlog[slot] = rec;
        sg_backlogCount++;
        pthread_mutex_unlock(&sg_lock);
        return;
    }
    pthread_mutex_unlock(&sg_lock);
    free(rec);
}

// ---------------------------------------------------------------------------------------------
// Connection thread.

static int dial(const char *host, int port, int timeoutMs) {
    struct addrinfo hints, *res = NULL, *ai;
    char service[16];
    int fd = -1;
    memset(&hints, 0, sizeof hints);
    hints.ai_family = AF_UNSPEC;
    hints.ai_socktype = SOCK_STREAM;
    service[fmt_u(service, (unsigned long long)port)] = 0;
    if (getaddrinfo(host, service, &hints, &res) != 0) return -1;
    for (ai = res; ai && fd < 0; ai = ai->ai_next) {
        int s = socket(ai->ai_family, ai->ai_socktype, ai->ai_protocol);
        int flags, one = 1;
        if (s < 0) continue;
        flags = fcntl(s, F_GETFL, 0);
        fcntl(s, F_SETFL, flags | O_NONBLOCK);
#ifdef SO_NOSIGPIPE
        setsockopt(s, SOL_SOCKET, SO_NOSIGPIPE, &one, sizeof one);
#endif
        setsockopt(s, IPPROTO_TCP, TCP_NODELAY, &one, sizeof one);
        setsockopt(s, SOL_SOCKET, SO_KEEPALIVE, &one, sizeof one);
        if (connect(s, ai->ai_addr, ai->ai_addrlen) != 0 && errno != EINPROGRESS) { close(s); continue; }
        {
            struct pollfd pf = { s, POLLOUT, 0 };
            int err = 0;
            socklen_t len = sizeof err;
            if (poll(&pf, 1, timeoutMs) > 0 && getsockopt(s, SOL_SOCKET, SO_ERROR, &err, &len) == 0 && err == 0) fd = s;
            else close(s);
        }
    }
    freeaddrinfo(res);
    return fd;
}

static void send_pending_crash(void) {
    char buf[8192];
    int f, ok = 1;
    ssize_t r;
    if (!sg_crashPath[0]) return;
    f = open(sg_crashPath, O_RDONLY);
    if (f < 0) return;
    while ((r = read(f, buf, sizeof buf)) > 0) {
        if (!send_locked(buf, (size_t)r)) { ok = 0; break; }
    }
    close(f);
    if (ok) unlink(sg_crashPath);
}

static void flush_backlog(void) {
    while (sg_backlogCount > 0) {
        char *rec = sg_backlog[sg_backlogHead];
        if (rec && !send_locked(rec, strlen(rec))) return;
        free(rec);
        sg_backlog[sg_backlogHead] = NULL;
        sg_backlogHead = (sg_backlogHead + 1) % kBacklogMax;
        sg_backlogCount--;
    }
}

static void *connector(void *unused) {
    int backoff = 700;
    (void)unused;
    for (;;) {
        char host[sizeof sg_host];
        int port, fd;
        unsigned generation;
        pthread_mutex_lock(&sg_lock);
        memcpy(host, sg_host, sizeof host);
        port = sg_port;
        generation = sg_hostGeneration;
        pthread_mutex_unlock(&sg_lock);
        if (!host[0]) { sleep_ms(500); continue; }

        fd = dial(host, port, 3000);
        if (fd < 0) {
            sleep_ms(backoff);
            if (backoff < 4000) backoff *= 2;
            continue;
        }
        backoff = 700;

        pthread_mutex_lock(&sg_lock);
        sg_fd = fd;
        {
            char hello[sizeof sg_hello + 2];
            size_t n = strlen(sg_hello);
            memcpy(hello, sg_hello, n);
            hello[n++] = '\n';
            send_locked(hello, n);
        }
        send_pending_crash();
        flush_backlog();
        pthread_mutex_unlock(&sg_lock);

        for (;;) {
            struct pollfd pf = { fd, POLLIN, 0 };
            int r = poll(&pf, 1, 1000);
            if (r > 0) {
                char sink[256];
                ssize_t n = recv(fd, sink, sizeof sink, 0);
                if (n == 0 || (n < 0 && errno != EAGAIN && errno != EINTR)) break;
            }
            pthread_mutex_lock(&sg_lock);
            r = (sg_hostGeneration != generation);
            pthread_mutex_unlock(&sg_lock);
            if (r) break;
        }
        pthread_mutex_lock(&sg_lock);
        sg_fd = -1;
        close(fd);
        pthread_mutex_unlock(&sg_lock);
        sleep_ms(300);
    }
    return NULL;
}

void sgrl_set_host(const char *host, int port) {
    pthread_mutex_lock(&sg_lock);
    if (host && strcmp(host, sg_host) != 0) {
        strncpy(sg_host, host, sizeof sg_host - 1);
        sg_host[sizeof sg_host - 1] = 0;
        sg_hostGeneration++;
    }
    if (port > 0) sg_port = port;
    pthread_mutex_unlock(&sg_lock);
}

void sgrl_configure(const char *crashPath, const char *statePath) {
    if (crashPath) { strncpy(sg_crashPath, crashPath, sizeof sg_crashPath - 1); }
    if (statePath) { strncpy(sg_statePath, statePath, sizeof sg_statePath - 1); }
    if (!sg_startTime) sg_startTime = time(NULL);
}

void sgrl_start(const char *host, int port, const char *hello) {
    static int started;
    pthread_t thread;
    pthread_attr_t attr;
    if (started) return;
    started = 1;
    if (hello) { strncpy(sg_hello, hello, sizeof sg_hello - 1); }
    sgrl_set_host(host ? host : "", port);
    pthread_attr_init(&attr);
    pthread_attr_setdetachstate(&attr, PTHREAD_CREATE_DETACHED);
    pthread_create(&thread, &attr, connector, NULL);
    pthread_attr_destroy(&attr);
}

void sgrl_set_state(const char *state) {
    int f;
    if (!state) return;
    strncpy(sg_state, state, sizeof sg_state - 1);
    if (!sg_statePath[0]) return;
    f = open(sg_statePath, O_WRONLY | O_CREAT | O_TRUNC, 0644);
    if (f < 0) return;
    write_all(f, state, strlen(state), 0);
    close(f);
}

// ---------------------------------------------------------------------------------------------
// Crash reports. From begin to end nothing here allocates, so a signal handler may use it.

static struct { char buf[8192]; size_t n; int file; } W = { {0}, 0, -1 };
static char sg_crashId[48];
static volatile int sg_crashBusy, sg_crashDone;
static int sg_crashFatal;   // 0 for a report about the last run, which must not shut this run's reporting out

static void w_flush(void) {
    int s;
    if (!W.n) return;
    if (W.file >= 0) write_all(W.file, W.buf, W.n, 0);
    s = sg_fd;
    if (s >= 0) write_all(s, W.buf, W.n, 1);
    W.n = 0;
}

static void w_mem(const char *p, size_t n) {
    while (n) {
        size_t room = sizeof W.buf - W.n, c;
        if (!room) { w_flush(); continue; }
        c = n < room ? n : room;
        memcpy(W.buf + W.n, p, c);
        W.n += c;
        p += c;
        n -= c;
    }
}

static void w_str(const char *s) { w_mem(s, strlen(s)); }

// A field of a record line: no tabs or line breaks.
static void w_field(const char *s) {
    size_t i;
    if (!s) return;
    for (i = 0; s[i] && i < 400; i++) {
        char c = (s[i] == '\t' || s[i] == '\n' || s[i] == '\r') ? ' ' : s[i];
        w_mem(&c, 1);
    }
}

static void w_dec(unsigned long long v) { char t[24]; w_mem(t, fmt_u(t, v)); }
static void w_hex(unsigned long long v, int width) { char t[24]; w_mem(t, fmt_hex(t, v, width)); }

int sgrl_crash_begin(const char *kind, const char *summary) {
    size_t n = 0;
    if (__sync_lock_test_and_set(&sg_crashBusy, 1)) return 0;
    sg_crashFatal = !(kind && strcmp(kind, "abnormal-exit") == 0);
    W.n = 0;
    W.file = sg_crashPath[0] ? open(sg_crashPath, O_WRONLY | O_CREAT | O_APPEND, 0644) : -1;
    n += fmt_u(sg_crashId + n, (unsigned long long)time(NULL));
    sg_crashId[n++] = '-';
    n += fmt_u(sg_crashId + n, (unsigned long long)getpid());
    sg_crashId[n] = 0;
    w_str("!CRASH-BEGIN\t");
    w_str(sg_crashId);
    w_str("\t");
    w_field(kind);
    w_str("\t");
    w_field(summary);
    w_str("\n");
    return 1;
}

void sgrl_crash_text(const char *text) {
    size_t n;
    if (!text) return;
    n = strlen(text);
    w_mem(text, n);
    if (n && text[n - 1] != '\n') w_str("\n");
}

void sgrl_crash_recent(void) {
    unsigned next = sg_recentNext, count = next < kRecentN ? next : kRecentN, i;
    w_str("last log lines of this run:\n");
    if (!count) w_str("  (none)\n");
    for (i = 0; i < count; i++) {
        unsigned idx = (next - count + i) % kRecentN;
        w_str("  ");
        w_str(sg_recent[idx]);
        w_str("\n");
    }
}

void sgrl_crash_end(void) {
    w_str("!CRASH-END\t");
    w_str(sg_crashId);
    w_str("\n");
    w_flush();
    if (W.file >= 0) { close(W.file); W.file = -1; }
    if (!sg_crashFatal) { sg_crashBusy = 0; return; }
    sg_crashDone = 1;
    sgrl_set_state("crash");
}

int sgrl_crash_reported(void) { return sg_crashDone; }

// ---------------------------------------------------------------------------------------------
// Signals.

static struct sigaction sg_prev[32];
static char sg_altStack[64 * 1024];

static const char *signal_name(int sig) {
    switch (sig) {
        case SIGSEGV: return "SIGSEGV";
        case SIGBUS: return "SIGBUS";
        case SIGILL: return "SIGILL";
        case SIGFPE: return "SIGFPE";
        case SIGABRT: return "SIGABRT";
        case SIGTRAP: return "SIGTRAP";
        case SIGSYS: return "SIGSYS";
        default: return "SIGNAL";
    }
}

static const char *signal_meaning(int sig) {
    switch (sig) {
        case SIGSEGV: return "invalid memory access (bad pointer, freed object, stack overflow)";
        case SIGBUS: return "misaligned or unmapped memory access";
        case SIGILL: return "illegal instruction (Swift runtime trap, __builtin_trap, failed assertion)";
        case SIGFPE: return "arithmetic error";
        case SIGABRT: return "abort() (uncaught exception, failed assert, corrupted heap)";
        case SIGTRAP: return "trap (Swift fatalError/precondition, force unwrap of nil, array out of range)";
        case SIGSYS: return "bad system call";
        default: return "";
    }
}

#ifdef __APPLE__
static int image_for(uintptr_t pc, uintptr_t *base, const char **name) {
    uint32_t i, count = _dyld_image_count();
    for (i = 0; i < count; i++) {
        const struct mach_header_64 *mh = (const struct mach_header_64 *)_dyld_get_image_header(i);
        intptr_t slide = _dyld_get_image_vmaddr_slide(i);
        const struct load_command *lc;
        uint32_t c;
        if (!mh || mh->magic != MH_MAGIC_64) continue;
        lc = (const struct load_command *)(mh + 1);
        for (c = 0; c < mh->ncmds; c++) {
            if (lc->cmd == LC_SEGMENT_64) {
                const struct segment_command_64 *seg = (const struct segment_command_64 *)lc;
                uintptr_t start = (uintptr_t)seg->vmaddr + (uintptr_t)slide;
                if (seg->vmsize && pc >= start && pc < start + (uintptr_t)seg->vmsize) {
                    const char *path = _dyld_get_image_name(i), *slash;
                    *base = (uintptr_t)mh;
                    *name = path ? path : "?";
                    if (path) { slash = strrchr(path, '/'); if (slash) *name = slash + 1; }
                    return 1;
                }
            }
            lc = (const struct load_command *)((const char *)lc + lc->cmdsize);
        }
    }
    return 0;
}
#endif

#if defined(__APPLE__) && defined(__arm64__)
// Return addresses carry pointer authentication bits above the address.
#define SG_ADDRESS_MASK 0x0000007FFFFFFFFFull
static int gather_frames(void *ctx, uintptr_t *out, int max) {
    ucontext_t *uc = (ucontext_t *)ctx;
    arm_thread_state64_t *ss;
    uintptr_t pc, lr, fp, lo, hi, firstRet = 0;
    pthread_t self = pthread_self();
    int n = 0;
    if (!uc || !uc->uc_mcontext) return 0;
    ss = (arm_thread_state64_t *)&uc->uc_mcontext->__ss;
    pc = (uintptr_t)arm_thread_state64_get_pc(*ss) & SG_ADDRESS_MASK;
    lr = (uintptr_t)arm_thread_state64_get_lr(*ss) & SG_ADDRESS_MASK;
    fp = (uintptr_t)arm_thread_state64_get_fp(*ss);
    hi = (uintptr_t)pthread_get_stackaddr_np(self);
    lo = hi - (uintptr_t)pthread_get_stacksize_np(self);
    out[n++] = pc;
    if (fp && fp >= lo && fp + 16 <= hi && !(fp & 7)) firstRet = ((uintptr_t *)fp)[1] & SG_ADDRESS_MASK;
    // lr is the caller when the fault hit before the frame was built; otherwise it repeats the first return address.
    if (lr && lr != firstRet) out[n++] = lr;
    while (fp && n < max && fp >= lo && fp + 16 <= hi && !(fp & 7)) {
        uintptr_t next = ((uintptr_t *)fp)[0], ret = ((uintptr_t *)fp)[1] & SG_ADDRESS_MASK;
        if (!ret) break;
        out[n++] = ret;
        if (next <= fp) break;
        fp = next;
    }
    return n;
}
#else
// Fallback for the PC harness: the first frames belong to the handler itself.
static int gather_frames(void *ctx, uintptr_t *out, int max) {
    void *raw[kFramesMax];
    int i, n = backtrace(raw, max);
    (void)ctx;
    for (i = 0; i < n; i++) out[i] = (uintptr_t)raw[i];
    return n;
}
#endif

static void write_signal_report(int sig, siginfo_t *info, void *ctx) {
    uintptr_t frames[kFramesMax];
    int i, n;
    char summary[96];
    size_t s = 0;
    const char *name = signal_name(sig);
    time_t now = time(NULL);

    memcpy(summary, name, strlen(name));
    s = strlen(name);
    summary[s++] = ' ';
    summary[s++] = '(';
    s += fmt_u(summary + s, (unsigned long long)sig);
    summary[s++] = ')';
    summary[s] = 0;

    if (!sgrl_crash_begin("signal", summary)) return;
    w_str("signal: "); w_str(name); w_str(" - "); w_str(signal_meaning(sig)); w_str("\n");
    if (info) {
        w_str("code: "); w_dec((unsigned long long)(info->si_code < 0 ? 0 : info->si_code));
        w_str("  fault address: "); w_hex((unsigned long long)(uintptr_t)info->si_addr, 16); w_str("\n");
    }
    w_str("thread: ");
#ifdef __APPLE__
    w_str(pthread_main_np() ? "main" : "background");
#else
    w_str("unknown");
#endif
    w_str("\nstate: "); w_str(sg_state);
    w_str("\nuptime: "); w_dec((unsigned long long)(sg_startTime && now >= sg_startTime ? now - sg_startTime : 0)); w_str(" s\n");

    n = gather_frames(ctx, frames, kFramesMax);
    w_str("backtrace ("); w_dec((unsigned long long)n); w_str(" frames):\n");
    for (i = 0; i < n; i++) {
        w_str("  #"); w_dec((unsigned long long)i); w_str("  "); w_hex(frames[i], 16);
#ifdef __APPLE__
        {
            uintptr_t base = 0;
            const char *image = NULL;
            if (image_for(frames[i], &base, &image)) {
                w_str("  "); w_str(image);
                w_str(" + "); w_hex(frames[i] - base, 8);
                w_str("  (image at "); w_hex(base, 16); w_str(")");
            }
        }
#endif
        w_str("\n");
    }
#ifdef __APPLE__
    w_str("symbolicate: atos -arch arm64 -o <binary> -l <image at> <address>\n");
#endif
    sgrl_crash_recent();
    sgrl_crash_end();
}

static void sig_handler(int sig, siginfo_t *info, void *ctx) {
    struct sigaction dfl, prev;
    int waited = 0;
    // A fault in here must kill the process, not loop.
    memset(&dfl, 0, sizeof dfl);
    dfl.sa_handler = SIG_DFL;
    sigemptyset(&dfl.sa_mask);
    sigaction(sig, &dfl, NULL);

    if (!sg_crashDone) {
        if (sg_crashBusy) {
            // Another thread is writing a report; give it a moment rather than racing it.
            while (!sg_crashDone && waited < 30) { sleep_ms(100); waited++; }
        } else {
            write_signal_report(sig, info, ctx);
        }
    }

    prev = sg_prev[sig];
    if (prev.sa_flags & SA_SIGINFO) {
        if (prev.sa_sigaction) prev.sa_sigaction(sig, info, ctx);
    } else if (prev.sa_handler != SIG_DFL && prev.sa_handler != SIG_IGN) {
        prev.sa_handler(sig);
    }
    // Returning re-runs the fault (or lets abort() finish) under the default action.
}

void sgrl_install_signal_handlers(void) {
    static const int sigs[] = { SIGSEGV, SIGBUS, SIGILL, SIGFPE, SIGABRT, SIGTRAP, SIGSYS };
    static int warmed;
    stack_t old;
    size_t i;
    if (!warmed) {
        void *tmp[2];
        backtrace(tmp, 2);   // the first call loads the unwinder; do it now, not inside a crash
        warmed = 1;
    }
    if (sigaltstack(NULL, &old) == 0 && (old.ss_flags & SS_DISABLE)) {
        stack_t ss;
        ss.ss_sp = sg_altStack;
        ss.ss_size = sizeof sg_altStack;
        ss.ss_flags = 0;
        sigaltstack(&ss, NULL);
    }
    for (i = 0; i < sizeof sigs / sizeof sigs[0]; i++) {
        struct sigaction cur, mine;
        int sig = sigs[i];
        if (sigaction(sig, NULL, &cur) != 0) continue;
        if ((cur.sa_flags & SA_SIGINFO) && cur.sa_sigaction == sig_handler) continue;
        sg_prev[sig] = cur;
        memset(&mine, 0, sizeof mine);
        mine.sa_sigaction = sig_handler;
        mine.sa_flags = SA_SIGINFO | SA_ONSTACK;
        sigemptyset(&mine.sa_mask);
        sigaction(sig, &mine, NULL);
    }
}

#endif /* SG_IPLOG */
