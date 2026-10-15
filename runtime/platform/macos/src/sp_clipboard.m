/*
 * sp_clipboard.m -- NSPasteboard for the USER32 clipboard shims (runtime/platform/guest/src/sgp_clipboard.c,
 * whose header declares these functions and describes the mapping).
 *
 *   format 13 (CF_UNICODETEXT)  <->  NSPasteboardTypeString (UTF-16 units, no terminator)
 *   any other format n          <->  "org.spore-native.clipboard.cf<n>" carrying the raw bytes
 *
 * The change count is NSPasteboard.changeCount; the guest polls it from the pump to post WM_DRAWCLIPBOARD.
 * Plain AppKit calls, safe from the main thread (where the game pumps). Compiled with -fobjc-arc.
 * Our own code; no game code.
 */
#import <AppKit/AppKit.h>
#include <stdint.h>
#include <string.h>

long sp_mac_pb_seq(void);
int sp_mac_pb_clear(void);
int sp_mac_pb_put(uint32_t fmt, const void *p, uint32_t n);
uint32_t sp_mac_pb_get(uint32_t fmt, void *buf, uint32_t cap);
int sp_mac_pb_has(uint32_t fmt);
void sp_mac_pb_use_named(const char *name);

static NSPasteboard *named;

static NSPasteboard *pb(void) { return named ? named : NSPasteboard.generalPasteboard; }

static NSPasteboardType type_for(uint32_t fmt)
{
    if (fmt == 13) return NSPasteboardTypeString;
    return [NSString stringWithFormat:@"org.spore-native.clipboard.cf%u", fmt];
}

void sp_mac_pb_use_named(const char *name)
{
    @autoreleasepool {
        if (named) [named releaseGlobally];
        named = name ? [NSPasteboard pasteboardWithName:[NSString stringWithUTF8String:name]] : nil;
    }
}

long sp_mac_pb_seq(void)
{
    @autoreleasepool { return (long)pb().changeCount; }
}

int sp_mac_pb_clear(void)
{
    @autoreleasepool { [pb() clearContents]; return 1; }
}

int sp_mac_pb_put(uint32_t fmt, const void *p, uint32_t n)
{
    @autoreleasepool {
        NSPasteboard *b = pb();
        NSPasteboardType t = type_for(fmt);
        BOOL ok;
        if (fmt == 13) {
            NSString *s = [NSString stringWithCharacters:(const unichar *)p length:n / 2];
            ok = [b setString:s forType:t];
            if (!ok) { [b addTypes:@[t] owner:nil]; ok = [b setString:s forType:t]; }
        } else {
            NSData *d = [NSData dataWithBytes:p length:n];
            ok = [b setData:d forType:t];
            if (!ok) { [b addTypes:@[t] owner:nil]; ok = [b setData:d forType:t]; }
        }
        return ok ? 1 : 0;
    }
}

uint32_t sp_mac_pb_get(uint32_t fmt, void *buf, uint32_t cap)
{
    @autoreleasepool {
        NSPasteboard *b = pb();
        if (fmt == 13) {
            NSString *s = [b stringForType:NSPasteboardTypeString];
            NSUInteger units;
            if (!s) return 0;
            units = s.length;
            if (units > 0x7fffffffu / 2) units = 0x7fffffffu / 2;
            if (buf && cap) {
                NSUInteger k = cap / 2 < units ? cap / 2 : units;
                [s getCharacters:(unichar *)buf range:NSMakeRange(0, k)];
            }
            return (uint32_t)(units * 2);
        } else {
            NSData *d = [b dataForType:type_for(fmt)];
            uint32_t n;
            if (!d) return 0;
            n = d.length > 0xffffffffu ? 0xffffffffu : (uint32_t)d.length;
            if (buf && cap) memcpy(buf, d.bytes, n < cap ? n : cap);
            return n;
        }
    }
}

int sp_mac_pb_has(uint32_t fmt)
{
    @autoreleasepool {
        NSPasteboard *b = pb();
        if (fmt == 13) {
            NSString *s = [b stringForType:NSPasteboardTypeString];
            return s && s.length ? 1 : 0;
        }
        return [b availableTypeFromArray:@[type_for(fmt)]] ? 1 : 0;
    }
}
