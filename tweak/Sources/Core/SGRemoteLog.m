#if SG_IPLOG

#import "SGRemoteLog.h"
#import "SGRemoteLogCore.h"
#import <UIKit/UIKit.h>
#import <arpa/inet.h>
#import <netinet/in.h>
#import <os/log.h>
#import <sys/utsname.h>

#ifndef SG_IPLOG_HOST
#define SG_IPLOG_HOST ""
#endif
#ifndef SG_IPLOG_PORT
#define SG_IPLOG_PORT 9000
#endif
#ifndef SG_VERSION
#define SG_VERSION "dev"
#endif

// Nothing in this file may call SGLog: it would come straight back here.
static void note(const char *text) {
    os_log_with_type(OS_LOG_DEFAULT, OS_LOG_TYPE_DEFAULT, "[spotifyglass] iplog: %{public}s", text);
}

void SGRemoteLogSend(NSString *text) {
    const char *utf8 = text.UTF8String;
    if (utf8) sgrl_log(utf8, strlen(utf8));
}

// --- uncaught exceptions ----------------------------------------------------------------------

static NSUncaughtExceptionHandler *sgPreviousExceptionHandler;

static void sgExceptionHandler(NSException *exception) {
    NSString *summary = [NSString stringWithFormat:@"%@: %@", exception.name, exception.reason ?: @"(no reason)"];
    if (sgrl_crash_begin("exception", summary.UTF8String)) {
        NSMutableString *body = [NSMutableString string];
        [body appendFormat:@"exception: %@\nreason: %@\n", exception.name, exception.reason ?: @"(none)"];
        if (exception.userInfo.count) [body appendFormat:@"userInfo: %@\n", exception.userInfo];
        [body appendFormat:@"thread: %@\ncall stack (%lu frames):\n", NSThread.isMainThread ? @"main" : @"background",
         (unsigned long)exception.callStackSymbols.count];
        for (NSString *frame in exception.callStackSymbols) [body appendFormat:@"  %@\n", frame];
        sgrl_crash_text(body.UTF8String);
        sgrl_crash_recent();
        sgrl_crash_end();
    }
    if (sgPreviousExceptionHandler && sgPreviousExceptionHandler != sgExceptionHandler) sgPreviousExceptionHandler(exception);
}

// The app and other tweaks install theirs too; ours takes its place back, keeping theirs to call.
static void sgInstallHandlers(void) {
    NSUncaughtExceptionHandler *current = NSGetUncaughtExceptionHandler();
    if (current != sgExceptionHandler) {
        sgPreviousExceptionHandler = current;
        NSSetUncaughtExceptionHandler(sgExceptionHandler);
    }
    sgrl_install_signal_handlers();
}

// --- finding the console -----------------------------------------------------------------------

@interface SGRemoteLogBrowser : NSObject <NSNetServiceBrowserDelegate, NSNetServiceDelegate>
- (void)start;
@end

@implementation SGRemoteLogBrowser {
    NSNetServiceBrowser *_browser;
    NSMutableArray<NSNetService *> *_services;
}

- (void)start {
    _services = [NSMutableArray array];
    _browser = [NSNetServiceBrowser new];
    _browser.delegate = self;
    [_browser searchForServicesOfType:@"_taurus-log._tcp." inDomain:@"local."];
}

- (void)netServiceBrowser:(NSNetServiceBrowser *)browser didFindService:(NSNetService *)service moreComing:(BOOL)moreComing {
    [_services addObject:service];
    service.delegate = self;
    [service resolveWithTimeout:10];
}

- (void)netServiceBrowser:(NSNetServiceBrowser *)browser didRemoveService:(NSNetService *)service moreComing:(BOOL)moreComing {
    service.delegate = nil;
    [_services removeObject:service];
}

- (void)netServiceBrowser:(NSNetServiceBrowser *)browser didNotSearch:(NSDictionary<NSString *, NSNumber *> *)error {
    note("Bonjour browse failed, trying again in 5 s");
    _browser.delegate = nil;
    [_browser stop];
    __weak SGRemoteLogBrowser *weakSelf = self;
    dispatch_after(dispatch_time(DISPATCH_TIME_NOW, 5 * NSEC_PER_SEC), dispatch_get_main_queue(), ^{ [weakSelf start]; });
}

- (void)netServiceDidResolveAddress:(NSNetService *)service {
    for (NSData *data in service.addresses) {
        if (data.length < sizeof(struct sockaddr_in)) continue;
        const struct sockaddr_in *sin = data.bytes;
        if (sin->sin_family != AF_INET) continue;
        char text[INET_ADDRSTRLEN];
        if (!inet_ntop(AF_INET, &sin->sin_addr, text, sizeof text)) continue;
        sgrl_set_host(text, (int)service.port);
        note(text);
        return;
    }
}

- (void)netService:(NSNetService *)service didNotResolve:(NSDictionary<NSString *, NSNumber *> *)error {
    __weak NSNetService *weak = service;
    dispatch_after(dispatch_time(DISPATCH_TIME_NOW, 3 * NSEC_PER_SEC), dispatch_get_main_queue(), ^{
        if (weak && weak.delegate) [weak resolveWithTimeout:10];
    });
}

@end

// --- start -------------------------------------------------------------------------------------

static NSString *supportFile(NSString *name) {
    NSString *dir = [NSSearchPathForDirectoriesInDomains(NSApplicationSupportDirectory, NSUserDomainMask, YES).firstObject
                     stringByAppendingPathComponent:@"spoti.pw"];
    [NSFileManager.defaultManager createDirectoryAtPath:dir withIntermediateDirectories:YES attributes:nil error:nil];
    return [dir stringByAppendingPathComponent:name];
}

static NSString *clean(NSString *text) {
    return [[text ?: @"?" stringByReplacingOccurrencesOfString:@"\t" withString:@" "] stringByReplacingOccurrencesOfString:@"\n" withString:@" "];
}

// The last run left "launching" or "foreground" behind: neither the signal handlers nor the exception
// handler got to say anything, and the app was never sent to the background or quit.
static void reportAbnormalExit(NSString *previousState) {
    NSString *log = [NSString stringWithContentsOfFile:supportFile(@"diagnostics.log") encoding:NSUTF8StringEncoding error:nil];
    NSArray<NSString *> *lines = [log componentsSeparatedByString:@"\n"];
    NSUInteger from = lines.count > 41 ? lines.count - 41 : 0;
    NSMutableString *body = [NSMutableString stringWithFormat:
        @"The previous run ended while the app was %@, and no signal or exception was caught.\n"
        @"Usual causes: killed for memory (jetsam), killed by the watchdog because the main thread was blocked, "
        @"a crash inside another crash handler, or the phone restarted.\n"
        @"The system keeps its own record: Settings > Privacy & Security > Analytics & Improvements > Analytics Data "
        @"(JetsamEvent-... or Spotify-...ips).\n"
        @"last lines of the diagnostics log:\n", previousState];
    for (NSUInteger i = from; i < lines.count; i++) if (lines[i].length) [body appendFormat:@"  %@\n", lines[i]];
    if (sgrl_crash_begin("abnormal-exit", "previous run ended without a crash report (killed or hung?)")) {
        sgrl_crash_text(body.UTF8String);
        sgrl_crash_end();
    }
}

__attribute__((constructor)) static void SGRemoteLogInit(void) {
    @autoreleasepool {
        NSString *statePath = supportFile(@"iplog-state"), *crashPath = supportFile(@"iplog-crash.txt");
        // A pile-up of reports nobody collected is not worth carrying.
        NSDictionary *attrs = [NSFileManager.defaultManager attributesOfItemAtPath:crashPath error:nil];
        if ([attrs fileSize] > 256 * 1024) [NSFileManager.defaultManager removeItemAtPath:crashPath error:nil];
        sgrl_configure(crashPath.fileSystemRepresentation, statePath.fileSystemRepresentation);

        NSString *previous = [NSString stringWithContentsOfFile:statePath encoding:NSUTF8StringEncoding error:nil];
        if ([previous isEqualToString:@"launching"] || [previous isEqualToString:@"foreground"]) reportAbnormalExit(previous);
        sgrl_set_state("launching");

        struct utsname un;
        uname(&un);
        NSBundle *bundle = NSBundle.mainBundle;
        NSString *hello = [NSString stringWithFormat:@"!HELLO\t%@\t%@\t%@\t%@\t%@\t%d",
                           clean(bundle.bundleIdentifier), clean([bundle objectForInfoDictionaryKey:@"CFBundleShortVersionString"]),
                           @SG_VERSION, clean(@(un.machine)), clean(UIDevice.currentDevice.systemVersion), (int)getpid()];
        sgrl_start(SG_IPLOG_HOST, SG_IPLOG_PORT, hello.UTF8String);
        sgInstallHandlers();

        if (!SG_IPLOG_HOST[0]) {
            static SGRemoteLogBrowser *browser;
            dispatch_async(dispatch_get_main_queue(), ^{
                browser = [SGRemoteLogBrowser new];
                [browser start];
            });
        }

        NSNotificationCenter *center = NSNotificationCenter.defaultCenter;
        [center addObserverForName:UIApplicationDidBecomeActiveNotification object:nil queue:nil usingBlock:^(NSNotification *n) {
            sgrl_set_state("foreground");
            sgInstallHandlers();
        }];
        [center addObserverForName:UIApplicationWillResignActiveNotification object:nil queue:nil usingBlock:^(NSNotification *n) {
            sgrl_set_state("inactive");
        }];
        [center addObserverForName:UIApplicationDidEnterBackgroundNotification object:nil queue:nil usingBlock:^(NSNotification *n) {
            sgrl_set_state("background");
        }];
        [center addObserverForName:UIApplicationWillTerminateNotification object:nil queue:nil usingBlock:^(NSNotification *n) {
            sgrl_set_state("exit");
        }];
        // Spotify sets its own handlers some time after launch.
        for (NSNumber *delay in @[@1, @4, @12]) {
            dispatch_after(dispatch_time(DISPATCH_TIME_NOW, (int64_t)(delay.doubleValue * NSEC_PER_SEC)), dispatch_get_main_queue(), ^{
                sgInstallHandlers();
            });
        }
        note("started");
    }
}

#endif
