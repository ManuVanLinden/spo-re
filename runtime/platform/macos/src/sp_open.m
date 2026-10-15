/* sp_open.m -- open a web link in the user's default browser.
 *
 * The HLE's ShellExecuteW (runtime/hle/src/shell_stub.c) filters the game's call (verb "open", an
 * http: or https: URL with a host, printable ASCII) and then finds this function with
 * dlsym(RTLD_DEFAULT, "sp_mac_open_url"), so the HLE test binary links without AppKit. The checks are
 * repeated here so nothing but an http(s) URL can reach NSWorkspace from this entry point either.
 *
 * Threading: the game calls ShellExecuteW on its main thread, which is the AppKit main thread
 * (platform-macos.md section 1), so -openURL: runs directly. From any other thread the open is queued
 * to the main queue, which the game's PeekMessage pump drains, and success means "a browser is
 * registered for the URL". */
#import <AppKit/AppKit.h>

#include "sp_macos.h"

int sp_mac_open_url_check(const char *utf8) {
    @autoreleasepool {
        NSString *s;
        NSURL *u;
        NSString *scheme;
        if (!utf8 || !*utf8) return 0;
        s = [NSString stringWithUTF8String:utf8];
        if (!s) return 0;
        u = [NSURL URLWithString:s];
        if (!u) return 0;
        scheme = u.scheme.lowercaseString;
        if (!([scheme isEqualToString:@"http"] || [scheme isEqualToString:@"https"])) return 0;
        if (u.host.length == 0) return 0;
        return [[NSWorkspace sharedWorkspace] URLForApplicationToOpenURL:u] != nil;
    }
}

int sp_mac_open_url(const char *utf8) {
    @autoreleasepool {
        NSURL *u;
        if (!sp_mac_open_url_check(utf8)) return 0;
        u = [NSURL URLWithString:[NSString stringWithUTF8String:utf8]];
        if ([NSThread isMainThread]) return [[NSWorkspace sharedWorkspace] openURL:u] ? 1 : 0;
        dispatch_async(dispatch_get_main_queue(), ^{
            [[NSWorkspace sharedWorkspace] openURL:u];
        });
        return 1;
    }
}
