/* test_window.m -- host test stand-in for the game window:
 * an NSWindow whose content view hosts a CAMetalLayer, as runtime/platform/macos's SPView does, returned
 * for every guest HWND. It is ordered front without being made key, so the user's keyboard focus stays
 * where it is; test_window_close orders it out. */
#import <AppKit/AppKit.h>
#import <Metal/Metal.h>
#import <QuartzCore/CAMetalLayer.h>

#include "d3d9metal.h"

@interface TWView : NSView
@end
@implementation TWView
- (instancetype)initWithFrame:(NSRect)f {
    if ((self = [super initWithFrame:f])) {
        CAMetalLayer *l = [CAMetalLayer layer];
        l.device = MTLCreateSystemDefaultDevice();
        l.pixelFormat = MTLPixelFormatBGRA8Unorm;
        self.layer = l; /* layer-hosting, as SPView */
        self.wantsLayer = YES;
    }
    return self;
}
- (void)updateDrawable {
    CGFloat sc = self.window ? self.window.backingScaleFactor : 1.0;
    ((CAMetalLayer *)self.layer).contentsScale = sc;
    ((CAMetalLayer *)self.layer).drawableSize = CGSizeMake(self.bounds.size.width * sc, self.bounds.size.height * sc);
}
- (void)viewDidMoveToWindow { [self updateDrawable]; }
@end

static NSWindow *g_win;
static int g_w = 512, g_h = 256, g_nowindow;
void test_window_size(int w, int h) { g_w = w, g_h = h; }
void test_window_none(int none) { g_nowindow = none; } /* the device before the game's first ShowWindow */
void test_app_init(void) {
    @autoreleasepool {
        [NSApplication sharedApplication];
        [NSApp setActivationPolicy:NSApplicationActivationPolicyAccessory];
        [NSApp finishLaunching];
    }
}
void *test_window_ns(void) { return (__bridge void *)g_win; }
void *d3d9metal_host_layer(uint32_t hwnd) {
    (void)hwnd;
    if (g_nowindow) return NULL;
    if (!g_win) {
        @autoreleasepool {
            NSRect r = NSMakeRect(80, 80, g_w, g_h);
            g_win = [[NSWindow alloc] initWithContentRect:r styleMask:NSWindowStyleMaskTitled backing:NSBackingStoreBuffered defer:NO];
            g_win.releasedWhenClosed = NO;
            g_win.title = @"d3d9metal test";
            g_win.contentView = [[TWView alloc] initWithFrame:NSMakeRect(0, 0, g_w, g_h)];
            [(TWView *)g_win.contentView updateDrawable];
            [g_win orderFrontRegardless];
        }
    }
    return (__bridge void *)g_win.contentView.layer;
}
void test_pump(void) {
    @autoreleasepool {
        NSEvent *e;
        while ((e = [NSApp nextEventMatchingMask:NSEventMaskAny untilDate:[NSDate distantPast] inMode:NSDefaultRunLoopMode dequeue:YES]))
            [NSApp sendEvent:e];
    }
}
void test_window_close(void) {
    if (g_win) [g_win orderOut:nil];
}
