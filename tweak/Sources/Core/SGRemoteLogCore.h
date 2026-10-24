// The transport and crash writer behind IPLOG builds (see SGRemoteLog.h). Plain C on purpose: the
// signal handler runs while the app is broken, so it may not touch Objective-C, malloc or locks, and
// the same file is compiled on a PC by harness/remotelog against the real desktop console.
//
// Wire format, one record per line, UTF-8, fields split by TAB:
//   !HELLO  app  version  mod  device  os  pid
//   !LOG    epoch-ms  message          (\n \r \\ escaped in the message)
//   !CRASH-BEGIN  id  kind  summary    followed by raw lines up to
//   !CRASH-END    id
#pragma once
#include <stddef.h>

#ifdef __cplusplus
extern "C" {
#endif

// Where a crash report waits for the next launch when the console cannot be reached, and where the
// app's foreground/background state lives. Call before anything else.
void sgrl_configure(const char *crashPath, const char *statePath);

// Starts the connection thread. An empty host waits for sgrl_set_host (Bonjour). `hello` is the
// whole !HELLO record without the newline.
void sgrl_start(const char *host, int port, const char *hello);
void sgrl_set_host(const char *host, int port);

// One log message. Sent at once when the console is connected, kept (newest 500) while it is not.
void sgrl_log(const char *message, size_t length);

// Signal handlers for SIGSEGV SIGBUS SIGILL SIGFPE SIGABRT SIGTRAP SIGSYS, chained to whoever had
// them. Safe to call again: it takes them back when somebody else installed theirs on top.
void sgrl_install_signal_handlers(void);

// A crash report by hand (uncaught NSException, abnormal exit of the last run). begin returns 0 when
// another crash is already being written. Between begin and end, text adds raw lines.
int  sgrl_crash_begin(const char *kind, const char *summary);
void sgrl_crash_text(const char *text);
void sgrl_crash_recent(void);   // the last log lines of this run
void sgrl_crash_end(void);
int  sgrl_crash_reported(void);

// "launching", "foreground", "inactive", "background", "exit"; also written to the state file.
void sgrl_set_state(const char *state);

#ifdef __cplusplus
}
#endif
