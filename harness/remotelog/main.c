// The phone's crash reporter (tweak/Sources/Core/SGRemoteLogCore.c) run on a PC against the real
// console server; tools/logconsole/test/remotelog.test.js drives it.
//   remotelog-test <host> <port> <crash-file> <state-file> <segv|abort|trap|exit|nullcall|report-then-segv>
#include "Core/SGRemoteLogCore.h"
#include <signal.h>
#include <stdio.h>
#include <stdlib.h>
#include <string.h>
#include <unistd.h>

static void boom(int *p) { *p = 42; }

int main(int argc, char **argv) {
    const char *mode;
    if (argc < 6) { fprintf(stderr, "usage: %s host port crash-file state-file mode\n", argv[0]); return 2; }
    mode = argv[5];
    sgrl_configure(argv[3], argv[4]);
    sgrl_start(argv[1], atoi(argv[2]), "!HELLO\tcom.test.app\t1.0\t0.0.1-test\tiPhone-test\t26.0\t1234");
    sgrl_install_signal_handlers();
    sgrl_set_state("foreground");
    sgrl_log("first line", 10);
    { const char *m = "multi\nline \\ message"; sgrl_log(m, strlen(m)); }
    { const char *m = "error: something failed"; sgrl_log(m, strlen(m)); }
    if (!strcmp(mode, "report-then-segv")) {
        // The report about the previous run must not stop this run's own crash from being reported.
        if (sgrl_crash_begin("abnormal-exit", "previous run ended without a crash report")) { sgrl_crash_text("earlier run"); sgrl_crash_end(); }
        mode = "segv";
    }
    usleep(400 * 1000);   // let the connection thread come up
    sgrl_log("about to crash", 14);
    if (!strcmp(mode, "segv")) boom((int *)0x10);
    else if (!strcmp(mode, "abort")) abort();
    else if (!strcmp(mode, "trap")) __builtin_trap();
    else if (!strcmp(mode, "nullcall")) ((void (*)(void))0)();
    usleep(300 * 1000);
    return 0;
}
