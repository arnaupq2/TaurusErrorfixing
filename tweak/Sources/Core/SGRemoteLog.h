// IPLOG builds (make IPLOG=1, or the "iplog" input of the GitHub build) stream every SGLog line to
// the Taurus Log Console running on a PC (tools/logconsole), and report crashes to it: caught
// signals, uncaught exceptions, and a run that ended without either (killed for memory, watchdog).
// Without the flag nothing here is compiled and SGRemoteLogSend is empty.
//
// Where the console is: IPLOG_HOST=1.2.3.4 pins an address at build time; left empty, the phone finds
// the console on the local network through Bonjour (_taurus-log._tcp). Port 9000 (IPLOG_PORT).
#import <Foundation/Foundation.h>

#if SG_IPLOG
void SGRemoteLogSend(NSString *text);
#else
#define SGRemoteLogSend(text) ((void)0)
#endif
