/*
 * sp_window.m - AppKit host for the platform layer: NSWindow + CAMetalLayer, NSEvent -> WM_*,
 * window-state notifications -> WM_*, and the event drain behind PeekMessageW.
 * See include/sp_macos.h and docs/design/platform-macos.md.
 * SPORE_FULLSCREEN_SAFE_AREA (DEVIATIONS 4.12, platform-macos.md section 11.6): when on, the
 * fullscreen letterbox is fitted inside the display's safe area (below a MacBook notch) and the screen size
 * the game sees (sp_mac_screen_info) is the safe area's; the window still covers the whole display.
 */
#import <AppKit/AppKit.h>
#import <Metal/Metal.h>
#import <QuartzCore/QuartzCore.h>

#include <dlfcn.h>
#include <math.h>
#include <stdatomic.h>

#include "sp_macos.h"

@interface SPView : NSView <NSTextInputClient>
@property(nonatomic, strong) CAMetalLayer *metalLayer;
@property(nonatomic) sp_hwnd hwnd;
@end

typedef struct spwin {
    sp_hwnd h;
    __unsafe_unretained NSWindow *win;   /* kept alive by g_keep */
    __unsafe_unretained SPView *view;
    int buttons_down;                    /* translated buttons held (drag routing) */
    double wheel_acc;
    /* fullscreen and the client mapping (section 11 of docs/design/platform-macos.md). */
    int fs;                              /* borderless window over its display */
    NSRect saved_frame;                  /* the titled frame to go back to (fs only) */
    int32_t lw, lh;                      /* client size the game sees, guest units */
    NSRect content;                      /* where that client is shown, view points, top-left origin */
} spwin;

#define MAXW 8
static spwin g_w[MAXW];
static NSMutableArray *g_keep;
static int g_test_mode;
static int g_activated;
/* runtime/core's stall watchdog, when linked (the game); the test apps have no core, so look it up. */
static void stall_suspend(int on) {
    static void (*fn)(int);
    static int looked;
    if (!looked) { fn = (void (*)(int))dlsym(RTLD_DEFAULT, "sg_core_stall_suspend"); looked = 1; }
    if (fn) fn(on);
}
static _Atomic int g_waiting;
static id<MTLDevice> g_dev;
static id<MTLCommandQueue> g_q;
static int g_cursor_count;
static int g_cursor_hidden;            /* NSCursor hide applied (released on focus loss / Esc) */
static const short SP_WAKE_SUBTYPE = 0x5350;

static spwin *by_hwnd(sp_hwnd h) { for (int i = 0; i < MAXW; i++) if (g_w[i].h == h && h) return &g_w[i]; return NULL; }
static spwin *by_nswin(NSWindow *w) { for (int i = 0; i < MAXW; i++) if (g_w[i].h && g_w[i].win == w) return &g_w[i]; return NULL; }

static CGFloat primary_height(void) { return NSScreen.screens.count ? NSScreen.screens[0].frame.size.height : 0; }

/* ---- guest units, fullscreen and the client mapping -----------------------------------------------
 * Guest units are what the game's coordinates (client sizes, screen points, mouse messages) count in.
 * They are points (k = 1), or with SPORE_RETINA=1 the primary display's pixels (k = its backing scale), so
 * a game window of W x H guest units is W/k x H/k points and its client is drawn 1:1 into the drawable.
 * Windowed: the client is the whole content view, lw x lh = view size x k. Fullscreen (a borderless window
 * over the display): lw x lh is the size the game asked for (its resolution) and `content` is that size
 * fitted at its aspect inside the screen (letterbox), unless the presenter cannot letterbox (any but Metal,
 * e.g. SPORE_D3D9=null, or sp_mac_set_letterbox(0)), where it is the whole view (stretched). The CAMetalLayer carries the content rectangle for the presenter under the
 * key "sporeContentRect" (normalised, top-left origin; absent = the whole drawable). */
static int g_retina = -1, g_fs_allowed = -1, g_fs_esc = -1, g_letterbox = -1;
static int env_on(const char *n, int dflt)
{
    const char *e = getenv(n);
    return e && *e ? (*e != '0') : dflt;
}
static void read_env(void)
{
    if (g_retina >= 0) return;
    const char *b = getenv("SPORE_D3D9");
    int metal = !b || !*b || !strcmp(b, "metal");        /* the Metal presenter (the default) letterboxes */
    g_retina = env_on("SPORE_RETINA", 0);
    /* a presenter other than Metal keeps the titled window unless SPORE_FULLSCREEN=1 (it stretches, so the
     * mapping stretches) */
    g_fs_allowed = env_on("SPORE_FULLSCREEN", metal);
    g_fs_esc = env_on("SPORE_FULLSCREEN_ESC", 0);
    if (g_letterbox < 0) g_letterbox = metal;
}
static double gk(void)
{
    read_env();
    if (!g_retina) return 1.0;
    CGFloat sc = NSScreen.screens.count ? NSScreen.screens[0].backingScaleFactor : 1.0;
    return sc > 0 ? sc : 1.0;
}

static NSScreen *screen_of(spwin *s) { return s->win.screen ?: (NSScreen.screens.count ? NSScreen.screens[0] : nil); }
static NSRect screen_frame_of(spwin *s)
{
    NSScreen *sc = screen_of(s);
    return sc ? sc.frame : NSMakeRect(0, 0, 1024, 768);
}

/* ---- the notch (DEVIATIONS 4.12) -----------------------------------------------------------------
 * SPORE_FULLSCREEN_SAFE_AREA: unset or 0 = off (the whole display); 1 = the display's
 * safeAreaInsets (a MacBook's notch strip at the top; all zero on a display without one, so nothing
 * changes there); "top[,bottom,left,right]" in points = those insets on any display (tests). Read on every
 * use (a getenv), so a test can switch it within one process. */
static NSEdgeInsets safe_insets(NSScreen *sc)
{
    NSEdgeInsets z = NSEdgeInsetsMake(0, 0, 0, 0);
    const char *e = getenv("SPORE_FULLSCREEN_SAFE_AREA");
    if (!e || !*e || !strcmp(e, "0")) return z;
    if (!strcmp(e, "1")) {
        if (sc) { if (@available(macOS 12.0, *)) return sc.safeAreaInsets; }
        return z;
    }
    double t = 0, b = 0, l = 0, r = 0;
    if (sscanf(e, "%lf,%lf,%lf,%lf", &t, &b, &l, &r) < 1 || t < 0 || b < 0 || l < 0 || r < 0) return z;
    return NSEdgeInsetsMake(t, l, b, r);
}
/* The safe area of a display of `full` points (top-left origin, points); the whole display when the insets
 * are zero or would leave less than 64 points. */
static NSRect safe_rect(NSSize full, NSEdgeInsets in)
{
    double w = full.width - in.left - in.right, h = full.height - in.top - in.bottom;
    if (w < 64 || h < 64) return NSMakeRect(0, 0, full.width, full.height);
    return NSMakeRect(in.left, in.top, w, h);
}

/* Recomputes s->content (and s->lw/lh when windowed) from the view, and tells the layer. */
static void layout(spwin *s)
{
    NSView *v = (NSView *)s->view;
    NSSize b = v.bounds.size;
    double k = gk();
    if (!s->fs) {
        s->lw = (int32_t)lround(b.width * k);
        s->lh = (int32_t)lround(b.height * k);
        s->content = NSMakeRect(0, 0, b.width, b.height);
    } else if (!g_letterbox || s->lw < 1 || s->lh < 1) {
        s->content = NSMakeRect(0, 0, b.width, b.height);
    } else {
        /* fit at the client's aspect inside the safe area (the whole view unless
         * SPORE_FULLSCREEN_SAFE_AREA is on and the display has a notch), snapped to whole drawable pixels */
        double px = s->win.backingScaleFactor > 0 ? s->win.backingScaleFactor : 1.0;
        NSRect a = safe_rect(b, safe_insets(screen_of(s)));
        double ax = round(a.origin.x * px), ay = round(a.origin.y * px);
        double W = round(a.size.width * px), H = round(a.size.height * px);
        double f = fmin(W / s->lw, H / s->lh);
        double cw = fmin(W, round(s->lw * f)), ch = fmin(H, round(s->lh * f));
        double ox = ax + floor((W - cw) / 2), oy = ay + floor((H - ch) / 2);
        s->content = NSMakeRect(ox / px, oy / px, cw / px, ch / px);
    }
    CAMetalLayer *l = ((SPView *)s->view).metalLayer;
    int whole = s->content.origin.x == 0 && s->content.origin.y == 0 && s->content.size.width == b.width &&
                s->content.size.height == b.height;
    if (whole || b.width <= 0 || b.height <= 0) [l setValue:nil forKey:@"sporeContentRect"];
    else
        [l setValue:[NSValue valueWithRect:NSMakeRect(s->content.origin.x / b.width, s->content.origin.y / b.height,
                                                      s->content.size.width / b.width, s->content.size.height / b.height)]
             forKey:@"sporeContentRect"];
}

/* Host screen rectangle (points, top-left origin) of the view's content rect. */
static NSRect content_screen_pt(spwin *s)
{
    NSView *v = (NSView *)s->view;
    NSRect r = [s->win convertRectToScreen:[v convertRect:v.bounds toView:nil]];
    return NSMakeRect(r.origin.x + s->content.origin.x, primary_height() - (r.origin.y + r.size.height) + s->content.origin.y,
                      s->content.size.width, s->content.size.height);
}

/* Client-area rectangle in Windows screen coordinates (guest units, top-left origin). Fullscreen: the
 * display's origin, as a Windows fullscreen window at (0,0) of its monitor. */
static void client_screen_rect(spwin *s, int32_t *x, int32_t *y, int32_t *w, int32_t *h)
{
    double k = gk();
    if (s->fs) {
        NSRect f = screen_frame_of(s);
        *x = (int32_t)lround(f.origin.x * k);
        *y = (int32_t)lround((primary_height() - (f.origin.y + f.size.height)) * k);
    } else {
        NSRect r = content_screen_pt(s);
        *x = (int32_t)lround(r.origin.x * k);
        *y = (int32_t)lround(r.origin.y * k);
    }
    *w = s->lw;
    *h = s->lh;
}

static void report_rect(spwin *s)
{
    int32_t x, y, w, h;
    layout(s);
    client_screen_rect(s, &x, &y, &w, &h);
    sp_window_rect_changed(s->h, x, y, w, h);
}

/* View point (AppKit, bottom-left origin) <-> client point (guest units, top-left origin). */
static void view_to_client(spwin *s, NSPoint p, double *cx, double *cy)
{
    NSView *v = (NSView *)s->view;
    double vx = p.x - s->content.origin.x, vy = v.bounds.size.height - p.y - s->content.origin.y;
    double sx = s->content.size.width > 0 ? s->lw / s->content.size.width : 1.0;
    double sy = s->content.size.height > 0 ? s->lh / s->content.size.height : 1.0;
    *cx = sx == 1.0 ? vx : vx * sx;
    *cy = sy == 1.0 ? vy : vy * sy;
}
static NSPoint client_to_view(spwin *s, double cx, double cy)
{
    NSView *v = (NSView *)s->view;
    double sx = s->lw > 0 ? s->content.size.width / s->lw : 1.0, sy = s->lh > 0 ? s->content.size.height / s->lh : 1.0;
    double vx = s->content.origin.x + (sx == 1.0 ? cx : cx * sx), vy = s->content.origin.y + (sy == 1.0 ? cy : cy * sy);
    return NSMakePoint(vx, v.bounds.size.height - vy);
}

/* ---- host hooks ---------------------------------------------------------------------------- */
static void hk_c2s(void *c, sp_hwnd h, int32_t *x, int32_t *y)
{
    spwin *s = by_hwnd(h); if (!s) return;
    int32_t cx, cy, cw, ch; client_screen_rect(s, &cx, &cy, &cw, &ch);
    *x += cx; *y += cy;
}
static void hk_s2c(void *c, sp_hwnd h, int32_t *x, int32_t *y)
{
    spwin *s = by_hwnd(h); if (!s) return;
    int32_t cx, cy, cw, ch; client_screen_rect(s, &cx, &cy, &cw, &ch);
    *x -= cx; *y -= cy;
}
static void hk_size(void *c, sp_hwnd h, int32_t *w, int32_t *hh)
{
    spwin *s = by_hwnd(h); if (!s) return;
    *w = s->lw; *hh = s->lh;
}
static void hk_wake(void *c)
{
    if (!atomic_load(&g_waiting)) return;
    NSEvent *e = [NSEvent otherEventWithType:NSEventTypeApplicationDefined location:NSZeroPoint modifierFlags:0
                                   timestamp:0 windowNumber:0 context:nil subtype:SP_WAKE_SUBTYPE data1:0 data2:0];
    [NSApp postEvent:e atStart:NO];   /* documented as safe from secondary threads */
}
static void hk_destroy(void *c, sp_hwnd h)
{
    spwin *s = by_hwnd(h); if (!s) return;
    NSWindow *w = s->win;
    s->h = 0;
    [w orderOut:nil];
    [g_keep removeObject:w];
}
static void drain(void *c);

/* ---- text input (docs/design/platform-macos.md section 12) ---------------------------------------------
 * The core (sp_textinput.inc) is compiled into a binary by the guest layer's sgp_imm.c or by the test app.
 * These weak stand-ins keep a binary without it linking, with keys going straight to translate_key. */
__attribute__((weak)) int sp_ti_wants_key(uint16_t kvk, int cmd_or_ctrl) { (void)kvk; (void)cmd_or_ctrl; return 0; }
__attribute__((weak)) void sp_ti_key_begin(void) {}
__attribute__((weak)) int sp_ti_key_end(uint16_t kvk, int action, const uint16_t *chars, int n)
{ (void)kvk; (void)action; (void)chars; (void)n; return 0; }
__attribute__((weak)) void sp_ti_set_marked(const uint16_t *s, int n, int sel_loc, int sel_len)
{ (void)s; (void)n; (void)sel_loc; (void)sel_len; }
__attribute__((weak)) void sp_ti_insert(const uint16_t *s, int n) { (void)s; (void)n; }
__attribute__((weak)) void sp_ti_unmark(void) {}
__attribute__((weak)) int sp_ti_marked(uint16_t *out, int max, int *sel_loc, int *sel_len)
{ (void)out; (void)max; if (sel_loc) *sel_loc = 0; if (sel_len) *sel_len = 0; return 0; }
__attribute__((weak)) void sp_ti_set_source(int kind, const char *id) { (void)kind; (void)id; }
__attribute__((weak)) void sp_ti_set_discard_hook(void (*fn)(void)) { (void)fn; }

static int g_ti_test;                     /* synthetic key events go through the input context (text test) */
static NSString *g_ti_source;
static __weak NSTextInputContext *g_ti_active;
void sp_mac_set_text_test(int on) { g_ti_test = on; }

static NSString *ti_string(id string)     /* NSString or NSAttributedString */
{
    if ([string isKindOfClass:[NSAttributedString class]]) return ((NSAttributedString *)string).string;
    return [string isKindOfClass:[NSString class]] ? string : @"";
}
static int ti_utf16(NSString *s, uint16_t *out, int max)
{
    NSUInteger n = s.length;
    if (n > (NSUInteger)max) n = (NSUInteger)max;
    [s getCharacters:(unichar *)out range:NSMakeRange(0, n)];
    return (int)n;
}

/* ---- the content view ------------------------------------------------------------------------ */

@implementation SPView
- (instancetype)initWithFrame:(NSRect)f
{
    if ((self = [super initWithFrame:f])) {
        _metalLayer = [CAMetalLayer layer];
        _metalLayer.device = g_dev;
        _metalLayer.pixelFormat = MTLPixelFormatBGRA8Unorm;
        _metalLayer.framebufferOnly = YES;
        self.layer = _metalLayer;          /* layer-hosting view */
        self.wantsLayer = YES;
        NSTrackingArea *ta = [[NSTrackingArea alloc] initWithRect:NSZeroRect
            options:NSTrackingMouseEnteredAndExited | NSTrackingActiveInKeyWindow | NSTrackingInVisibleRect
              owner:self userInfo:nil];
        [self addTrackingArea:ta];
    }
    return self;
}
- (BOOL)acceptsFirstResponder { return YES; }
- (BOOL)acceptsFirstMouse:(NSEvent *)e { return YES; }
- (BOOL)isOpaque { return YES; }
- (void)updateDrawableSize
{
    CGFloat sc = self.window ? self.window.backingScaleFactor : 1.0;
    _metalLayer.contentsScale = sc;
    _metalLayer.drawableSize = CGSizeMake(self.bounds.size.width * sc, self.bounds.size.height * sc);
}
- (void)setFrameSize:(NSSize)s { [super setFrameSize:s]; [self updateDrawableSize]; }
- (void)viewDidChangeBackingProperties { [self updateDrawableSize]; }
/* Input is translated in the drain loop, not here: these stay no-ops so AppKit still gets the
 * events for window activation and title-bar handling without double translation. */
- (void)mouseDown:(NSEvent *)e {}
- (void)rightMouseDown:(NSEvent *)e {}
- (void)otherMouseDown:(NSEvent *)e {}
- (void)keyDown:(NSEvent *)e {}
- (void)mouseExited:(NSEvent *)e { if (!g_test_mode) sp_input_mouse_exited(_hwnd); }

/* NSTextInputClient. Called from interpretKeyEvents: in the drain (route -> ti_route), and by the
 * input method on its own (candidate window clicks, focus changes). The game has no document: the only
 * text is the marked text, and committed text leaves as messages. */
- (BOOL)hasMarkedText { return sp_ti_marked(NULL, 0, NULL, NULL) > 0; }
- (NSRange)markedRange
{
    int n = sp_ti_marked(NULL, 0, NULL, NULL);
    return n > 0 ? NSMakeRange(0, (NSUInteger)n) : NSMakeRange(NSNotFound, 0);
}
- (NSRange)selectedRange
{
    int loc = 0, len = 0;
    int n = sp_ti_marked(NULL, 0, &loc, &len);
    return n > 0 ? NSMakeRange((NSUInteger)loc, (NSUInteger)len) : NSMakeRange(0, 0);
}
- (void)setMarkedText:(id)string selectedRange:(NSRange)sel replacementRange:(NSRange)rep
{
    uint16_t b[256];
    int n = ti_utf16(ti_string(string), b, 256);
    sp_ti_set_marked(b, n, sel.location == NSNotFound ? n : (int)sel.location, (int)sel.length);
}
- (void)unmarkText { sp_ti_unmark(); }
- (NSArray<NSAttributedStringKey> *)validAttributesForMarkedText { return @[]; }
- (NSAttributedString *)attributedSubstringForProposedRange:(NSRange)r actualRange:(NSRangePointer)actual
{
    uint16_t b[256];
    int n = sp_ti_marked(b, 256, NULL, NULL);
    if (r.location == NSNotFound || r.location >= (NSUInteger)n) return nil;
    if (r.location + r.length > (NSUInteger)n) r.length = (NSUInteger)n - r.location;
    if (actual) *actual = r;
    return [[NSAttributedString alloc] initWithString:[NSString stringWithCharacters:(unichar *)b + r.location
                                                                              length:r.length]];
}
- (void)insertText:(id)string replacementRange:(NSRange)rep
{
    uint16_t b[256];
    int n = ti_utf16(ti_string(string), b, 256);
    sp_ti_insert(b, n);
}
- (void)insertText:(id)string { [self insertText:string replacementRange:NSMakeRange(NSNotFound, 0)]; }
- (NSUInteger)characterIndexForPoint:(NSPoint)p { return NSNotFound; }
/* Where the IM puts its candidate window: the game draws its composition at its own caret, which it does not
 * tell IMM32 (no ImmSetCompositionWindow import), so the window opens under the mouse pointer, which is
 * where the player last clicked a text field [inf]. */
- (NSRect)firstRectForCharacterRange:(NSRange)r actualRange:(NSRangePointer)actual
{
    NSPoint m = [NSEvent mouseLocation];
    if (actual) *actual = r;
    return NSMakeRect(m.x, m.y - 24, 1, 24);
}
- (void)doCommandBySelector:(SEL)sel {}   /* key bindings (arrows, Return ...): the game gets the key itself */
@end

/* A borderless window must say it can become key and main, or it gets no keyboard. */
@interface SPWindow : NSWindow
@end
@implementation SPWindow
- (BOOL)canBecomeKeyWindow { return YES; }
- (BOOL)canBecomeMainWindow { return YES; }
@end

static const NSWindowStyleMask SP_TITLED_STYLE = NSWindowStyleMaskTitled | NSWindowStyleMaskClosable |
                                                 NSWindowStyleMaskMiniaturizable | NSWindowStyleMaskResizable;

/* Menu bar and Dock hidden while a fullscreen window of ours is key (they apply only while the app is
 * active, so Cmd-Tab gives them back). */
static void apply_presentation(void)
{
    int want = 0;
    for (int i = 0; i < MAXW; i++)
        if (g_w[i].h && g_w[i].fs && g_w[i].win.isKeyWindow) want = 1;
    NSApplicationPresentationOptions o = want ? (NSApplicationPresentationHideDock | NSApplicationPresentationHideMenuBar)
                                              : NSApplicationPresentationDefault;
    if (NSApp.presentationOptions != o) {
        @try { NSApp.presentationOptions = o; } @catch (NSException *ex) { (void)ex; }
    }
}

static void fs_fit(spwin *s)    /* the window over its whole display */
{
    NSRect f = screen_frame_of(s);
    if (!NSEqualRects(s->win.frame, f)) [s->win setFrame:f display:YES];
}

/* ---- host close requests ------------------------------------------------------------------------ */
/* The title-bar close button, Cmd-Q and the app's Quit (menu, Dock) go through
 * sgp_host_close_request, which runtime/platform/guest defines (sgp_msg.c: during a timed run,
 * SPORE_RUN_SECONDS set, it logs and drops the request; otherwise it is WM_CLOSE). This weak default,
 * used where the guest layer is not linked (this package's own tests), always delivers the request. Returns 1
 * if the request was delivered. */
int sgp_host_close_request(sp_hwnd h, const char *source);
__attribute__((weak)) int sgp_host_close_request(sp_hwnd h, const char *source)
{
    (void)source;
    sp_window_close_request(h);
    return 1;
}

/* ---- window delegate ----------------------------------------------------------------------------- */
@interface SPDelegate : NSObject <NSWindowDelegate, NSApplicationDelegate>
@end

@implementation SPDelegate
- (BOOL)windowShouldClose:(NSWindow *)w
{
    spwin *s = by_nswin(w);
    if (s) sgp_host_close_request(s->h, "close button");   /* the game decides; DestroyWindow closes it */
    return NO;
}
/* Quit from the app menu or the Dock: honoured immediately, except in a timed run, where
 * sgp_host_close_request logs and drops it. */
- (NSApplicationTerminateReply)applicationShouldTerminate:(NSApplication *)app
{
    (void)app;
    return sgp_host_close_request(0, "Quit") ? NSTerminateNow : NSTerminateCancel; /* h 0: no WM_CLOSE */
}
- (void)windowDidResize:(NSNotification *)n { spwin *s = by_nswin(n.object); if (s) report_rect(s); }
- (void)windowDidMove:(NSNotification *)n { spwin *s = by_nswin(n.object); if (s) report_rect(s); }
- (void)windowDidChangeScreen:(NSNotification *)n
{
    spwin *s = by_nswin(n.object);
    if (s && s->fs) { fs_fit(s); report_rect(s); }
}
- (void)windowDidChangeBackingProperties:(NSNotification *)n
{
    spwin *s = by_nswin(n.object);
    if (s) report_rect(s);   /* the letterbox snaps to pixels; Retina guest units follow the scale */
}
- (void)windowDidBecomeKey:(NSNotification *)n
{
    spwin *s = by_nswin(n.object);
    apply_presentation();
    if (!s || g_test_mode || g_activated) return;
    g_activated = 1;
    if (g_cursor_count < 0 && !g_cursor_hidden) { [NSCursor hide]; g_cursor_hidden = 1; }
    stall_suspend(0);
    sp_window_activate(s->h, 1, 1);
}
- (void)windowDidResignKey:(NSNotification *)n
{
    spwin *s = by_nswin(n.object);
    apply_presentation();
    if (!s || g_test_mode || !g_activated) return;
    g_activated = 0;
    sp_mac_cursor_release();
    sp_release_capture();                 /* Windows drops the capture on deactivation too */
    stall_suspend(1);                     /* the game stops drawing while inactive: no stall report while away */
    sp_window_activate(s->h, 0, 1);
}
- (void)windowWillStartLiveResize:(NSNotification *)n { spwin *s = by_nswin(n.object); if (s) sp_window_enter_size_move(s->h, 1); }
- (void)windowDidEndLiveResize:(NSNotification *)n { spwin *s = by_nswin(n.object); if (s) sp_window_enter_size_move(s->h, 0); }
- (NSSize)windowWillResize:(NSWindow *)w toSize:(NSSize)fs
{
    spwin *s = by_nswin(w);
    if (!s) return fs;
    sp_minmaxinfo mmi = {{0, 0}, {0, 0}, {0, 0}, {1, 1}, {100000, 100000}};
    if (s->fs || !sp_window_get_minmax(s->h, &mmi)) return fs;
    NSRect cr = [w contentRectForFrameRect:(NSRect){NSZeroPoint, fs}];
    double k = gk();                                     /* the track sizes are in guest units */
    cr.size.width = fmax(cr.size.width, mmi.ptMinTrackSize.x / k);
    cr.size.height = fmax(cr.size.height, mmi.ptMinTrackSize.y / k);
    cr.size.width = fmin(cr.size.width, mmi.ptMaxTrackSize.x / k);
    cr.size.height = fmin(cr.size.height, mmi.ptMaxTrackSize.y / k);
    return [w frameRectForContentRect:cr].size;
}
- (void)windowDidMiniaturize:(NSNotification *)n { spwin *s = by_nswin(n.object); if (s) sp_window_set_minimized(s->h, 1); }
- (void)windowDidDeminiaturize:(NSNotification *)n { spwin *s = by_nswin(n.object); if (s) sp_window_set_minimized(s->h, 0); }
- (void)screensChanged:(NSNotification *)n
{
    NSScreen *sc0 = NSScreen.screens.count ? NSScreen.screens[0] : nil;
    NSSize sz = sc0 ? safe_rect(sc0.frame.size, safe_insets(sc0)).size : NSZeroSize;   /* as sp_mac_screen_info */
    double k = gk();
    for (int i = 0; i < MAXW; i++) {
        if (g_w[i].h && g_w[i].fs) { fs_fit(&g_w[i]); report_rect(&g_w[i]); }
        if (g_w[i].h) sp_window_display_change(g_w[i].h, 32, (int)lround(sz.width * k), (int)lround(sz.height * k));
    }
}
@end

static SPDelegate *g_delegate;

/* ---- event translation --------------------------------------------------------------------------- */
static int is_input(NSEventType t)
{
    switch (t) {
    case NSEventTypeKeyDown: case NSEventTypeKeyUp: case NSEventTypeFlagsChanged:
    case NSEventTypeLeftMouseDown: case NSEventTypeLeftMouseUp: case NSEventTypeRightMouseDown:
    case NSEventTypeRightMouseUp: case NSEventTypeOtherMouseDown: case NSEventTypeOtherMouseUp:
    case NSEventTypeMouseMoved: case NSEventTypeLeftMouseDragged: case NSEventTypeRightMouseDragged:
    case NSEventTypeOtherMouseDragged: case NSEventTypeScrollWheel:
        return 1;
    default: return 0;
    }
}

static int utf16_of(NSString *s, uint16_t *out, int max)
{
    if (!s) return 0;
    NSUInteger n = s.length;
    if (n > (NSUInteger)max) n = (NSUInteger)max;
    [s getCharacters:(unichar *)out range:NSMakeRange(0, n)];
    return (int)n;
}

/* Device-dependent modifier bits (IOKit NX_DEVICE*KEYMASK) tell left from right. */
static uint32_t device_bit(uint16_t kvk)
{
    switch (kvk) {
    case 0x38: return 0x0002; case 0x3C: return 0x0004;   /* L/R shift */
    case 0x3B: return 0x0001; case 0x3E: return 0x2000;   /* L/R control */
    case 0x3A: return 0x0020; case 0x3D: return 0x0040;   /* L/R option */
    case 0x37: return 0x0008; case 0x36: return 0x0010;   /* L/R command */
    default: return 0;
    }
}
static NSEventModifierFlags generic_bit(uint16_t kvk)
{
    switch (kvk) {
    case 0x38: case 0x3C: return NSEventModifierFlagShift;
    case 0x3B: case 0x3E: return NSEventModifierFlagControl;
    case 0x3A: case 0x3D: return NSEventModifierFlagOption;
    case 0x37: case 0x36: return NSEventModifierFlagCommand;
    default: return 0;
    }
}

static void translate_key(NSEvent *e)
{
    uint16_t ch[8], ig[8];
    int n = utf16_of(e.characters, ch, 8), ni = utf16_of(e.charactersIgnoringModifiers, ig, 8);
    int action = e.type == NSEventTypeKeyUp ? SP_KEY_UP : (e.isARepeat ? SP_KEY_REPEAT : SP_KEY_DOWN);
    sp_input_key(e.keyCode, action, ch, n, ig, ni);
}

/* Offer a key-down to the input context. Returns 1 when the text-input core emitted the key's
 * messages (a dead key, composed text, or a key the input method took); 0 = today's path. In test mode,
 * and for synthetic keys (SPORE_INPUT_SCRIPT runs), keys bypass it (the reference-script replay stays byte for byte
 * whatever the keyboard layout) unless the text test turned it on. */
static int ti_route(spwin *s, NSEvent *e)
{
    if ((g_test_mode || e.timestamp >= SP_MAC_SYNTH_TS) && !g_ti_test) return 0;   /* scripted keys: no text input */
    NSTextInputContext *ctx = s->view.inputContext;
    NSString *src = ctx.selectedKeyboardInputSource;    /* the input source can change between keys */
    if (src && ![src isEqualToString:g_ti_source]) {
        g_ti_source = src;
        /* input methods have "inputmethod" in their id (com.apple.inputmethod.Kotoeri..., com.google.inputmethod...);
         * everything else is a keyboard layout (com.apple.keylayout.US, third-party .keylayout files) */
        sp_ti_set_source([src rangeOfString:@"inputmethod" options:NSCaseInsensitiveSearch].location != NSNotFound
                             ? SP_TI_IM : SP_TI_LAYOUT, src.UTF8String);
    }
    NSEventModifierFlags f = e.modifierFlags;
    if (!sp_ti_wants_key(e.keyCode, (f & (NSEventModifierFlagCommand | NSEventModifierFlagControl)) != 0)) return 0;
    if (s->win.firstResponder != s->view) [s->win makeFirstResponder:s->view];
    /* Activate the view's context once (AppKit does it when it dispatches keys itself, which the drain does
     * not; in the background currentInputContext stays nil, so compare with the one we activated). */
    if (ctx && ctx != g_ti_active && NSTextInputContext.currentInputContext != ctx) [ctx activate];
    g_ti_active = ctx;
    sp_ti_key_begin();
    [s->view interpretKeyEvents:@[ e ]];
    uint16_t ch[8];
    int n = utf16_of(e.characters, ch, 8);
    return sp_ti_key_end(e.keyCode, e.isARepeat ? SP_KEY_REPEAT : SP_KEY_DOWN, ch, n);
}

static void ti_discard(void)
{
    spwin *s = by_hwnd(sp_get_focus());
    if (!s) for (int i = 0; i < MAXW; i++) if (g_w[i].h) { s = &g_w[i]; break; }
    if (s) [s->view.inputContext discardMarkedText];
}

static void translate_flags(NSEvent *e)
{
    uint16_t kvk = e.keyCode;
    NSEventModifierFlags f = e.modifierFlags;
    if (kvk == 0x39) { sp_input_capslock((f & NSEventModifierFlagCapsLock) != 0); return; }
    uint32_t dev = device_bit(kvk);
    if (!dev) return;                                    /* Fn and unknown keys */
    int down;
    if (f & 0x217F) down = (f & dev) != 0;               /* device bits present */
    else down = (f & generic_bit(kvk)) != 0;
    sp_input_modifier(kvk, down);
}

/* Synthetic mouse events (once an intermittent one-point miss in check-gui): AppKit does not return a posted mouse
 * event's location exactly. Posting window point (120.5, 419.5) came back as (119.87, 420.14) or
 * (119.56, 419.82), varying from run to run by up to 0.7 point, so floor() gave 119 in about half the
 * runs. eventNumber cannot carry the point (only 16 bits survive), so the poster records the window
 * location it meant under the event's unique synthetic timestamp, and mouse_point uses that. Real
 * events are not affected. */
#define SP_SYN_RING 256
static struct { double ts; NSPoint p; int button; } g_syn[SP_SYN_RING];
static unsigned g_syn_next;

void sp_mac_note_synthetic_point(double ts, double wx, double wy)
{
    g_syn[g_syn_next % SP_SYN_RING].ts = ts;
    g_syn[g_syn_next % SP_SYN_RING].p = NSMakePoint(wx, wy);
    g_syn[g_syn_next % SP_SYN_RING].button = -1;
    g_syn_next++;
}

static int syn_find(NSEvent *e)
{
    if (e.timestamp >= SP_MAC_SYNTH_TS)
        for (unsigned i = 0; i < SP_SYN_RING; i++)
            if (g_syn[i].ts != 0 && fabs(g_syn[i].ts - e.timestamp) < 2e-4) return (int)i;
    return -1;
}

static NSPoint event_location(NSEvent *e)
{
    int i = syn_find(e);
    return i >= 0 ? g_syn[i].p : e.locationInWindow;
}

/* The mouse button of an "other" (middle) button event: NSEvent's constructor cannot set buttonNumber,
 * so sp_mac_post_mouse records the button it meant. */
static NSInteger event_button(NSEvent *e)
{
    int i = syn_find(e);
    return i >= 0 && g_syn[i].button >= 0 ? g_syn[i].button : e.buttonNumber;
}

static int mouse_point(spwin *s, NSEvent *e, int32_t *x, int32_t *y)
{
    NSView *v = (NSView *)s->view;
    NSPoint p = [v convertPoint:event_location(e) fromView:nil];
    double cx, cy;
    view_to_client(s, p, &cx, &cy);
    *x = (int32_t)floor(cx);
    *y = (int32_t)floor(cy);
    return NSPointInRect(p, v.bounds) && cx >= 0 && cy >= 0 && cx < s->lw && cy < s->lh;   /* not on a letterbox bar */
}

static void translate_mouse(spwin *s, NSEvent *e)
{
    int32_t x, y;
    int inside = mouse_point(s, e, &x, &y);
    int button = -1, isdown = 0;
    switch (e.type) {
    case NSEventTypeLeftMouseDown: button = SP_MB_LEFT; isdown = 1; break;
    case NSEventTypeLeftMouseUp: button = SP_MB_LEFT; break;
    case NSEventTypeRightMouseDown: button = SP_MB_RIGHT; isdown = 1; break;
    case NSEventTypeRightMouseUp: button = SP_MB_RIGHT; break;
    case NSEventTypeOtherMouseDown: if (event_button(e) == 2) { button = SP_MB_MIDDLE; isdown = 1; } break;
    case NSEventTypeOtherMouseUp: if (event_button(e) == 2) button = SP_MB_MIDDLE; break;
    default: break;
    }
    if (button < 0) {                                    /* move or drag */
        if (inside || s->buttons_down || sp_get_capture() == s->h) sp_input_mouse_move(s->h, x, y);
        return;
    }
    if (isdown) {
        if (!inside) return;                             /* title bar etc.: AppKit's business */
        s->buttons_down |= 1 << button;
    } else {
        if (!(s->buttons_down & (1 << button))) return;
        s->buttons_down &= ~(1 << button);
    }
    sp_input_mouse_button(s->h, button, isdown, x, y, (int)e.clickCount);
}

static void translate_scroll(spwin *s, NSEvent *e)
{
    /* + = content moves down = wheel turned away from the user, the sign WM_MOUSEWHEEL uses.
     * scrollingDeltaY already follows the user's scroll-direction preference. Classic wheels
     * report lines (one notch = 1.0 = WHEEL_DELTA); trackpads report points, 40 points per notch
     * [inf: a comfortable rate, not a Windows rule]. Only whole notches are sent, because 2008-era
     * code often divides the delta by 120. */
    double d = e.hasPreciseScrollingDeltas ? e.scrollingDeltaY * (WHEEL_DELTA / 40.0) : e.scrollingDeltaY * WHEEL_DELTA;
    s->wheel_acc += d;
    int32_t notches = (int32_t)(s->wheel_acc / WHEEL_DELTA);
    if (!notches) return;
    s->wheel_acc -= notches * WHEEL_DELTA;
    sp_input_wheel(s->h, notches * WHEEL_DELTA);
}

/* Scripted input: while a script drives the mouse, GetCursorPos reports the scripted point.
 * Any real mouse event ends that (the user has the mouse again), and SetCursorPos moves the scripted
 * point too, so a game that warps and reads back (Spore's camera drag) stays consistent. */
static int g_cursor_scripted;
static int32_t g_script_x, g_script_y;
static int g_script_buttons;                 /* scripted buttons held (bit per SP_MB_*), for drags */
/* The uptime of the last real cursor warp. A hardware mouse event stamped before
 * the warp but read after it still carries the pre-warp location; turning it into WM_MOUSEMOVE would
 * undo the warp in the game's delta (Spore takes deltas between consecutive WM_MOUSEMOVEs). */
static double g_warp_uptime;

static int is_move(NSEventType t)
{
    return t == NSEventTypeMouseMoved || t == NSEventTypeLeftMouseDragged || t == NSEventTypeRightMouseDragged ||
           t == NSEventTypeOtherMouseDragged;
}

static void route(NSEvent *e)
{
    NSEventType t = e.type;
    sp_keyinfo command_key;
    if (t == NSEventTypeApplicationDefined && e.subtype == SP_WAKE_SUBTYPE) return;
    int synth = e.timestamp >= SP_MAC_SYNTH_TS;
    if (!synth && g_cursor_scripted && is_input(t) && t != NSEventTypeKeyDown && t != NSEventTypeKeyUp &&
        t != NSEventTypeFlagsChanged)
        g_cursor_scripted = 0;                           /* a real mouse event: the real pointer again */
    if (g_test_mode && is_input(t) && !synth) return;    /* keep the scripted log deterministic */
    NSWindow *nw = e.window;
    if (!nw && g_test_mode && synth) for (int i = 0; i < MAXW; i++) if (g_w[i].h) { nw = g_w[i].win; break; }
    spwin *s = nw ? by_nswin(nw) : NULL;
    if (!s || !is_input(t)) { [NSApp sendEvent:e]; return; }
    switch (t) {
    case NSEventTypeKeyDown:
        if ((e.modifierFlags & NSEventModifierFlagCommand) &&
            sp_keymap_lookup(e.keyCode, NULL, &command_key) && command_key.vk == 'Q') { /* Cmd+Q in the selected layout */
            sgp_host_close_request(s->h, "Cmd-Q");
            return;
        }
        /* fallthrough */
    case NSEventTypeKeyUp:
        if ((e.modifierFlags & NSEventModifierFlagCommand) && (e.modifierFlags & NSEventModifierFlagControl) &&
            e.keyCode == 0x03) {                         /* Cmd-Ctrl-F: fullscreen on/off, the game is not told */
            if (t == NSEventTypeKeyDown && !e.isARepeat) sp_mac_set_fullscreen(s->h, !s->fs);
            return;
        }
        if (t == NSEventTypeKeyDown && e.keyCode == 0x35) {
            sp_mac_cursor_release();                     /* Esc: cursor back */
            read_env();
            if (g_fs_esc && s->fs) sp_mac_set_fullscreen(s->h, 0);   /* opt-in: Esc also leaves fullscreen */
        }
        if (t == NSEventTypeKeyDown && ti_route(s, e)) return;   /* dead keys and input methods */
        translate_key(e); return;     /* not forwarded: AppKit drops Cmd key-ups */
    case NSEventTypeFlagsChanged: translate_flags(e); return;
    case NSEventTypeScrollWheel: translate_scroll(s, e); return;
    default:
        if (!synth && is_move(t) && e.timestamp < g_warp_uptime) {   /* stale: from before the last warp */
            [NSApp sendEvent:e];
            return;
        }
        translate_mouse(s, e);
        if (!synth) [NSApp sendEvent:e];                 /* activation clicks, title bar, resize */
        return;
    }
}

static void drain(void *c)
{
    @autoreleasepool {
        for (;;) {
            NSEvent *e = [NSApp nextEventMatchingMask:NSEventMaskAny untilDate:[NSDate distantPast]
                                               inMode:NSDefaultRunLoopMode dequeue:YES];
            if (!e) break;
            route(e);
        }
    }
}

/* ---- public API -------------------------------------------------------------------------------- */
int sp_mac_app_init(int activate)
{
    @autoreleasepool {
        /* Holding a key must auto-repeat (the game moves on held WASD), not open macOS's press-and-hold accent
         * picker, which the text-input path (NSTextInputClient) would otherwise trigger.
         * Dead keys (Option-e then e) do not use press-and-hold and keep working. Per process; nothing is
         * written to the user's defaults. SPORE_PRESS_AND_HOLD=1 keeps macOS's behaviour. */
        {
            const char *pah = getenv("SPORE_PRESS_AND_HOLD");
            if (!(pah && *pah == '1'))
                [[NSUserDefaults standardUserDefaults] registerDefaults:@{@"ApplePressAndHoldEnabled": @NO}];
        }
        [NSApplication sharedApplication];
        [NSApp setActivationPolicy:NSApplicationActivationPolicyRegular];
        NSMenu *bar = [NSMenu new], *appm = [NSMenu new];
        NSMenuItem *it = [NSMenuItem new];
        [bar addItem:it];
        [appm addItemWithTitle:@"Quit" action:@selector(terminate:) keyEquivalent:@"q"];
        it.submenu = appm;
        NSApp.mainMenu = bar;
        [NSApp finishLaunching];
        if (activate) [NSApp activateIgnoringOtherApps:YES];
        g_keep = [NSMutableArray new];
        g_delegate = [SPDelegate new];
        NSApp.delegate = g_delegate;
        [[NSNotificationCenter defaultCenter] addObserver:g_delegate selector:@selector(screensChanged:)
                                                     name:NSApplicationDidChangeScreenParametersNotification object:nil];
        g_dev = MTLCreateSystemDefaultDevice();
        g_q = [g_dev newCommandQueue];
    }
    sp_ti_set_discard_hook(ti_discard);                  /* ImmNotifyIME / ImmSetOpenStatus reach the IM */
    sp_host_hooks hk = {0};
    hk.pump_events = drain; hk.wake = hk_wake;
    hk.client_to_screen = hk_c2s; hk.screen_to_client = hk_s2c; hk.get_client_size = hk_size;
    hk.destroy_window = hk_destroy;
    sp_platform_init(&hk, NULL);
    return g_dev ? 0 : -1;
}

/* Creates the NSWindow for an sp_hwnd already in the queue's window table (the guest layer attaches it on the
 * window's first ShowWindow). x, y: client top-left in Windows screen points, INT32_MIN centres. */
int sp_mac_attach_window(sp_hwnd hw, const char *title, int32_t x, int32_t y, int32_t w, int32_t h)
{
    int slot = -1;
    if (by_hwnd(hw)) return 1;
    for (int i = 0; i < MAXW; i++) if (!g_w[i].h) { slot = i; break; }
    if (slot < 0) return 0;
    @autoreleasepool {
        double k = gk();                                 /* guest units -> points */
        if (k != 1.0) {
            if (x != INT32_MIN) x = (int32_t)lround(x / k), y = (int32_t)lround(y / k);
            w = (int32_t)lround(w / k), h = (int32_t)lround(h / k);
            if (w < 1) w = 1;
            if (h < 1) h = 1;
        }
        NSRect cr = NSMakeRect(0, 0, w, h);
        NSWindow *win = [[SPWindow alloc] initWithContentRect:cr styleMask:SP_TITLED_STYLE
                                                      backing:NSBackingStoreBuffered defer:NO];
        win.releasedWhenClosed = NO;
        win.title = [NSString stringWithUTF8String:title ? title : "Spore"] ?: @"Spore";
        win.acceptsMouseMovedEvents = YES;
        win.delegate = g_delegate;
        SPView *v = [[SPView alloc] initWithFrame:cr];
        win.contentView = v;
        [win makeFirstResponder:v];                      /* the view's input context is the window's */
        if (x == INT32_MIN) {
            [win center];
            /* Snap to a whole-point origin. [center] can leave a half-point origin, and AppKit then rounds
             * event locations in screen space, which moved synthetic pixel-centre points across a pixel
             * boundary ((120,60) was logged as (119,59)). With an integral origin the
             * view<->window conversions are exact and floor() in mouse_point() is right. */
            NSRect fr = win.frame;
            fr.origin.x = floor(fr.origin.x);
            fr.origin.y = floor(fr.origin.y);
            [win setFrame:fr display:NO];
        } else {
            NSRect fr = [win frameRectForContentRect:NSMakeRect(x, primary_height() - y - h, w, h)];
            [win setFrame:fr display:NO];
        }
        [g_keep addObject:win];
        g_w[slot] = (spwin){hw, win, v, 0, 0, 0, NSZeroRect, 0, 0, NSZeroRect};
        v.hwnd = hw;
        [v updateDrawableSize];
        report_rect(&g_w[slot]);                         /* silent while the window is not shown */
        return 1;
    }
}

void sp_mac_detach_window(sp_hwnd h) { hk_destroy(NULL, h); }

int32_t sp_mac_caption_height(void)
{
    NSRect fr = [NSWindow frameRectForContentRect:NSMakeRect(0, 0, 100, 100)
                                        styleMask:NSWindowStyleMaskTitled | NSWindowStyleMaskClosable |
                                                  NSWindowStyleMaskMiniaturizable | NSWindowStyleMaskResizable];
    return (int32_t)lround((fr.size.height - 100) * gk());   /* guest units */
}

sp_hwnd sp_mac_create_window(const char *title, int32_t x, int32_t y, int32_t w, int32_t h,
                             sp_wndproc_fn proc, void *user, uint32_t style)
{
    sp_hwnd hw = sp_window_register(proc, user, style);
    if (!hw) return 0;
    if (!sp_mac_attach_window(hw, title, x, y, w, h)) { sp_window_unregister(hw); return 0; }
    return hw;
}

void *sp_mac_metal_layer(sp_hwnd h) { spwin *s = by_hwnd(h); return s ? (__bridge void *)((SPView *)s->view).metalLayer : NULL; }
int sp_mac_window_number(sp_hwnd h) { spwin *s = by_hwnd(h); return s ? (int)s->win.windowNumber : 0; }
double sp_mac_backing_scale(sp_hwnd h) { spwin *s = by_hwnd(h); return s ? s->win.backingScaleFactor : 1.0; }
void sp_mac_order_front(sp_hwnd h) { spwin *s = by_hwnd(h); if (s) [s->win orderFront:nil]; }
void sp_mac_set_test_mode(int on) { g_test_mode = on; }

void sp_mac_set_client_size(sp_hwnd h, int32_t w, int32_t hh)
{
    spwin *s = by_hwnd(h); if (!s) return;
    NSRect cr = [s->win contentRectForFrameRect:s->win.frame];
    cr.origin.y += cr.size.height - hh;                  /* keep the top edge */
    cr.size = NSMakeSize(w, hh);
    [s->win setFrame:[s->win frameRectForContentRect:cr] display:YES];
}

void sp_mac_present_clear(sp_hwnd h, float r, float g, float b)
{
    spwin *s = by_hwnd(h); if (!s || !g_q) return;
    @autoreleasepool {
        id<CAMetalDrawable> d = [((SPView *)s->view).metalLayer nextDrawable];
        if (!d) return;
        MTLRenderPassDescriptor *rp = [MTLRenderPassDescriptor renderPassDescriptor];
        rp.colorAttachments[0].texture = d.texture;
        rp.colorAttachments[0].loadAction = MTLLoadActionClear;
        rp.colorAttachments[0].storeAction = MTLStoreActionStore;
        rp.colorAttachments[0].clearColor = MTLClearColorMake(r, g, b, 1.0);
        id<MTLCommandBuffer> cb = [g_q commandBuffer];
        [[cb renderCommandEncoderWithDescriptor:rp] endEncoding];
        [cb presentDrawable:d];
        [cb commit];
    }
}

void sp_mac_wait_message(uint32_t timeout_ms)
{
    if (sp_queue_depth(0) || sp_queue_depth(1)) return;
    atomic_store(&g_waiting, 1);
    @autoreleasepool {
        NSEvent *e = [NSApp nextEventMatchingMask:NSEventMaskAny
                                        untilDate:[NSDate dateWithTimeIntervalSinceNow:timeout_ms / 1000.0]
                                           inMode:NSDefaultRunLoopMode dequeue:YES];
        atomic_store(&g_waiting, 0);
        if (e) route(e);
    }
}

/* SetCursorPos. While a script drives the mouse, only the scripted point moves: the script owns the
 * pointer and the user's real cursor is never warped (otherwise a script run in a key window would warp the
 * real cursor, and in a window that was not key the scripted point would not move, so Spore's
 * warp-and-read-back camera drag would see a fixed cursor). Otherwise the real cursor is warped
 * only while one of our windows is key. Returns 1 if the cursor (real or scripted) moved. */
/* Host screen points (top-left origin) <-> guest screen coordinates. Uniform (x k) unless a window
 * of ours is fullscreen, whose client mapping then applies (its display's origin is the guest origin). */
static spwin *fs_win(void) { for (int i = 0; i < MAXW; i++) if (g_w[i].h && g_w[i].fs) return &g_w[i]; return NULL; }
static void host_to_guest(double hx, double hy, double *gx, double *gy)
{
    spwin *s = fs_win();
    if (s && s->content.size.width > 0 && s->content.size.height > 0) {
        NSRect c = content_screen_pt(s);
        int32_t ox, oy, w, h;
        client_screen_rect(s, &ox, &oy, &w, &h);
        *gx = ox + (hx - c.origin.x) * s->lw / c.size.width;
        *gy = oy + (hy - c.origin.y) * s->lh / c.size.height;
        return;
    }
    double k = gk();
    *gx = hx * k, *gy = hy * k;
}
static void guest_to_host(double gx, double gy, double *hx, double *hy)
{
    spwin *s = fs_win();
    if (s && s->lw > 0 && s->lh > 0) {
        NSRect c = content_screen_pt(s);
        int32_t ox, oy, w, h;
        client_screen_rect(s, &ox, &oy, &w, &h);
        *hx = c.origin.x + (gx - ox) * c.size.width / s->lw;
        *hy = c.origin.y + (gy - oy) * c.size.height / s->lh;
        return;
    }
    double k = gk();
    *hx = gx / k, *hy = gy / k;
}
/* The last real warp: GetCursorPos returns its guest point exactly while the pointer has not moved from
 * it, so a warp-and-read-back (Spore's camera drag) has no rounding drift when guest units are not points. */
static int g_warp_valid;
static int32_t g_warp_gx, g_warp_gy;
static double g_warp_hx, g_warp_hy;

int sp_mac_set_cursor_pos(int32_t x, int32_t y)
{
    if (g_cursor_scripted) { g_script_x = x; g_script_y = y; return 1; }
    if (!g_activated) return 0;
    double hx, hy;
    guest_to_host(x, y, &hx, &hy);
    g_warp_valid = 1, g_warp_gx = x, g_warp_gy = y, g_warp_hx = hx, g_warp_hy = hy;
    CGWarpMouseCursorPosition(CGPointMake(hx, hy));      /* CG global coords are top-left based too */
    CGAssociateMouseAndMouseCursorPosition(true);        /* avoid the post-warp input suppression */
    g_warp_uptime = NSProcessInfo.processInfo.systemUptime;   /* NSEvent.timestamp's clock */
    return 1;
}

int sp_mac_show_cursor(int show)
{
    /* ShowCursor keeps a display counter; the cursor is visible while it is >= 0. The cursor is only
     * hidden while one of our windows is key, and sp_mac_cursor_release shows it again. */
    g_cursor_count += show ? 1 : -1;
    if (g_cursor_count < 0 && !g_cursor_hidden && g_activated) { [NSCursor hide]; g_cursor_hidden = 1; }
    else if (g_cursor_count >= 0 && g_cursor_hidden) { [NSCursor unhide]; g_cursor_hidden = 0; }
    return g_cursor_count;
}

void sp_mac_cursor_release(void)
{
    if (g_cursor_hidden) { [NSCursor unhide]; g_cursor_hidden = 0; }
    CGAssociateMouseAndMouseCursorPosition(true);
}

int sp_mac_is_active(void) { return g_activated; }

/* ---- window services for the guest layer --------------------------------------------------------- */
static NSRect visible_frame_win(void)   /* primary screen's visible frame in Windows coordinates */
{
    NSScreen *sc = NSScreen.screens.count ? NSScreen.screens[0] : nil;
    if (!sc) return NSMakeRect(0, 0, 1024, 768);
    NSRect v = sc.visibleFrame;
    return NSMakeRect(v.origin.x, primary_height() - (v.origin.y + v.size.height), v.size.width, v.size.height);
}

void sp_mac_set_client_rect(sp_hwnd h, int32_t x, int32_t y, int32_t w, int32_t hh)
{
    spwin *s = by_hwnd(h); if (!s) return;
    @autoreleasepool {
        if (s->fs) {                                     /* the game's resolution, shown over the display */
            s->lw = w < 1 ? 1 : w;
            s->lh = hh < 1 ? 1 : hh;
            fs_fit(s);
            report_rect(s);
            return;
        }
        double k = gk();                                 /* guest units -> points */
        if (k != 1.0) {
            if (x != INT32_MIN) x = (int32_t)lround(x / k);
            if (y != INT32_MIN) y = (int32_t)lround(y / k);
            w = (int32_t)lround(w / k), hh = (int32_t)lround(hh / k);
        }
        /* Never larger than the visible screen and never off it: the game asks for fullscreen-sized
         * windows at (0,0); we keep a titled window inside the visible frame (no screen takeover). */
        NSRect vis = visible_frame_win();
        NSRect probe = [s->win frameRectForContentRect:NSMakeRect(0, 0, 100, 100)];
        CGFloat bar = probe.size.height - 100;
        if (w > vis.size.width) w = (int32_t)vis.size.width;
        if (hh > vis.size.height - bar) hh = (int32_t)(vis.size.height - bar);
        if (w < 1) w = 1;
        if (hh < 1) hh = 1;
        if (x == INT32_MIN || x < vis.origin.x || x + w > vis.origin.x + vis.size.width)
            x = (int32_t)(vis.origin.x + (vis.size.width - w) / 2);
        if (y == INT32_MIN || y - bar < vis.origin.y || y + hh > vis.origin.y + vis.size.height)
            y = (int32_t)(vis.origin.y + bar + (vis.size.height - bar - hh) / 2);
        NSRect cr = NSMakeRect(x, primary_height() - y - hh, w, hh);
        [s->win setFrame:[s->win frameRectForContentRect:cr] display:YES];
        report_rect(s);
    }
}

/* Fullscreen as a borderless window over the window's current display (never a display-mode
 * switch). The client size the game sees does not change on the way in: the game's own fullscreen path
 * sets it right after (SetWindowPos with its resolution), and Cmd-Ctrl-F keeps the windowed client, shown
 * letterboxed. On the way out the titled window comes back where it was, with the client the game had. */
int sp_mac_set_fullscreen(sp_hwnd h, int on)
{
    spwin *s = by_hwnd(h);
    if (!s) return 0;
    read_env();
    on = on ? 1 : 0;
    if (on && !g_fs_allowed) return 0;
    if (on == s->fs) return 1;
    @autoreleasepool {
        BOOL key = s->win.isKeyWindow;
        if (on) {
            layout(s);                                   /* lw/lh: the current client */
            s->saved_frame = s->win.frame;
            s->fs = 1;
            s->win.styleMask = NSWindowStyleMaskBorderless;
            s->win.collectionBehavior = NSWindowCollectionBehaviorFullScreenNone;
            fs_fit(s);
        } else {
            int32_t lw = s->lw, lh = s->lh;
            s->fs = 0;
            s->win.styleMask = SP_TITLED_STYLE;
            s->win.collectionBehavior = NSWindowCollectionBehaviorDefault;
            NSRect f = s->saved_frame;
            if (f.size.width > 0 && f.size.height > 0) {
                [s->win setFrame:f display:YES];
                /* the client the game had, inside the visible frame */
                double k = gk();
                NSRect cr = [s->win contentRectForFrameRect:s->win.frame];
                if (lround(cr.size.width * k) != lw || lround(cr.size.height * k) != lh) {
                    int32_t x, y, w, hh;
                    client_screen_rect(s, &x, &y, &w, &hh);
                    sp_mac_set_client_rect(h, x, y, lw, lh);
                }
            }
        }
        if (key && !s->win.isKeyWindow) [s->win makeKeyAndOrderFront:nil];   /* a style change can drop key */
        apply_presentation();
        report_rect(s);
        fprintf(stderr, "platform: window 0x%x %s, client %dx%d shown in %.0fx%.0f pt\n", (unsigned)h,
                on ? "fullscreen (borderless, over the display)" : "windowed", (int)s->lw, (int)s->lh, s->content.size.width,
                s->content.size.height);
    }
    return 1;
}
int sp_mac_is_fullscreen(sp_hwnd h) { spwin *s = by_hwnd(h); return s ? s->fs : 0; }
void sp_mac_set_letterbox(int on) { g_letterbox = on ? 1 : 0; }
void sp_mac_content_rect(sp_hwnd h, double out[4])
{
    spwin *s = by_hwnd(h);
    NSRect r = s ? s->content : NSZeroRect;
    out[0] = r.origin.x, out[1] = r.origin.y, out[2] = r.size.width, out[3] = r.size.height;
}
double sp_mac_guest_scale(void) { return gk(); }

void sp_mac_order_out(sp_hwnd h) { spwin *s = by_hwnd(h); if (s) [s->win orderOut:nil]; }
void sp_mac_make_key(sp_hwnd h)
{
    spwin *s = by_hwnd(h); if (!s) return;
    [NSApp activateIgnoringOtherApps:YES];
    [s->win makeKeyAndOrderFront:nil];
}
void sp_mac_set_title(sp_hwnd h, const char *utf8)
{
    spwin *s = by_hwnd(h); if (!s) return;
    s->win.title = [NSString stringWithUTF8String:utf8 ? utf8 : ""] ?: @"";
}
int sp_mac_is_miniaturized(sp_hwnd h) { spwin *s = by_hwnd(h); return s ? (int)s->win.miniaturized : 0; }

int sp_mac_screen_safe_area(double out[4])
{
    NSScreen *sc = NSScreen.screens.count ? NSScreen.screens[0] : nil;
    NSEdgeInsets in = safe_insets(sc);
    if (out) out[0] = in.top, out[1] = in.bottom, out[2] = in.left, out[3] = in.right;
    if (!sc) return 0;
    NSSize full = sc.frame.size;
    NSRect a = safe_rect(full, in);
    return a.size.width != full.width || a.size.height != full.height;
}

void sp_mac_screen_info(int32_t *w, int32_t *h, sp_rect *work, int32_t *hz, int32_t *mm_w, int32_t *mm_h)
{
    NSScreen *sc = NSScreen.screens.count ? NSScreen.screens[0] : nil;
    /* With SPORE_FULLSCREEN_SAFE_AREA the screen is the safe area (below the notch); the work area
     * stays the real visible frame, which is below the notch already */
    NSSize full = sc ? sc.frame.size : NSMakeSize(1024, 768);
    NSSize sz = sc ? safe_rect(full, safe_insets(sc)).size : full;
    double k = gk();                                     /* guest units (pixels with SPORE_RETINA=1) */
    if (w) *w = (int32_t)lround(sz.width * k);
    if (h) *h = (int32_t)lround(sz.height * k);
    if (work) {
        NSRect v = visible_frame_win();
        work->left = (int32_t)lround(v.origin.x * k); work->top = (int32_t)lround(v.origin.y * k);
        work->right = (int32_t)lround((v.origin.x + v.size.width) * k);
        work->bottom = (int32_t)lround((v.origin.y + v.size.height) * k);
    }
    if (hz) {
        NSInteger f = 60;
        if (@available(macOS 12.0, *)) { if (sc) f = sc.maximumFramesPerSecond; }
        *hz = (int32_t)(f > 0 ? f : 60);
    }
    /* the physical size of what is reported (with the notch switch: the safe area's share of the panel), so the aspect the
     * game derives from HORZSIZE/VERTSIZE (0x75da80) matches the screen size it sees: square pixels */
    CGSize mm = CGDisplayScreenSize(CGMainDisplayID());
    double fw = full.width > 0 ? sz.width / full.width : 1.0, fh = full.height > 0 ? sz.height / full.height : 1.0;
    if (mm_w) *mm_w = mm.width > 0 ? (int32_t)lround(mm.width * fw) : (int32_t)lround(sz.width * 25.4 / 96.0);
    if (mm_h) *mm_h = mm.height > 0 ? (int32_t)lround(mm.height * fh) : (int32_t)lround(sz.height * 25.4 / 96.0);
}


int sp_mac_post_mouse(sp_hwnd h, int kind, int button, int32_t x, int32_t y, int clicks)
{
    static double ts = SP_MAC_SYNTH_TS;
    spwin *s = by_hwnd(h);
    if (!s || !s->win) return 0;
    @autoreleasepool {
        NSView *v = s->win.contentView;
        NSPoint p = [v convertPoint:client_to_view(s, x + 0.5, y + 0.5) toView:nil];
        NSEventType t;
        if (button < 0 || button > 2) return 0;
        if (kind == 1) t = button == 2 ? NSEventTypeOtherMouseDown : button == 1 ? NSEventTypeRightMouseDown : NSEventTypeLeftMouseDown;
        else if (kind == 2) t = button == 2 ? NSEventTypeOtherMouseUp : button == 1 ? NSEventTypeRightMouseUp : NSEventTypeLeftMouseUp;
        else if (g_script_buttons & 1) t = NSEventTypeLeftMouseDragged;     /* a move with a button held */
        else if (g_script_buttons & 2) t = NSEventTypeRightMouseDragged;
        else if (g_script_buttons & 4) t = NSEventTypeOtherMouseDragged;
        else t = NSEventTypeMouseMoved;
        ts += 0.001;
        NSEvent *e = [NSEvent mouseEventWithType:t location:p modifierFlags:0 timestamp:ts
                                    windowNumber:s->win.windowNumber context:nil eventNumber:0
                                      clickCount:kind ? clicks : 0 pressure:kind == 1 ? 1.0f : 0.0f];
        if (!e) return 0;
        sp_mac_note_synthetic_point(e.timestamp, p.x, p.y);
        g_syn[(g_syn_next - 1) % SP_SYN_RING].button = button;
        if (kind == 1) g_script_buttons |= 1 << button;
        else if (kind == 2) g_script_buttons &= ~(1 << button);
        g_cursor_scripted = 1;
        int32_t ox, oy, cw, ch;
        client_screen_rect(s, &ox, &oy, &cw, &ch);       /* the client point in guest screen coordinates */
        g_script_x = ox + x;
        g_script_y = oy + y;
        [NSApp postEvent:e atStart:NO];
        return 1;
    }
}

int sp_mac_post_key(sp_hwnd h, int kind, uint16_t kvk, uint16_t ch)
{
    static double ts = SP_MAC_SYNTH_TS + 5.0e8;          /* synthetic, apart from the mouse stamps */
    spwin *s = by_hwnd(h);
    if (!s || !s->win || (kind != 1 && kind != 2)) return 0;
    @autoreleasepool {
        NSString *c = ch ? [NSString stringWithCharacters:&ch length:1] : @"";
        ts += 0.001;
        NSEvent *e = [NSEvent keyEventWithType:kind == 1 ? NSEventTypeKeyDown : NSEventTypeKeyUp location:NSZeroPoint
                                 modifierFlags:0 timestamp:ts windowNumber:s->win.windowNumber context:nil
                                    characters:c charactersIgnoringModifiers:c isARepeat:NO keyCode:kvk];
        if (!e) return 0;
        [NSApp postEvent:e atStart:NO];
        return 1;
    }
}

int sp_mac_post_wheel(sp_hwnd h, int32_t x, int32_t y, int notches)
{
    spwin *s = by_hwnd(h);
    if (!s || !s->win || !notches) return 0;
    @autoreleasepool {
        NSView *v = s->win.contentView;
        NSPoint p = [v convertPoint:client_to_view(s, x + 0.5, y + 0.5) toView:nil];
        NSPoint sp = [s->win convertPointToScreen:p];
        CGEventRef ce = CGEventCreateScrollWheelEvent(NULL, kCGScrollEventUnitLine, 1, notches);
        if (!ce) return 0;
        CGEventSetLocation(ce, CGPointMake(sp.x, primary_height() - sp.y));
        NSEvent *e = [NSEvent eventWithCGEvent:ce];
        CFRelease(ce);
        if (!e) return 0;
        translate_scroll(s, e);                          /* straight to the game: no window routing needed */
        return 1;
    }
}

void sp_mac_cursor_pos(int32_t *x, int32_t *y)
{
    if (g_cursor_scripted) { *x = g_script_x; *y = g_script_y; return; }
    NSPoint p = [NSEvent mouseLocation];                 /* bottom-left origin, points */
    double hx = p.x, hy = primary_height() - p.y, gx, gy;
    if (g_warp_valid && fabs(hx - g_warp_hx) < 0.25 && fabs(hy - g_warp_hy) < 0.25) {
        *x = g_warp_gx, *y = g_warp_gy;                  /* still where SetCursorPos put it */
        return;
    }
    host_to_guest(hx, hy, &gx, &gy);
    *x = (int32_t)floor(gx);
    *y = (int32_t)floor(gy);
}

void sp_mac_set_cursor_shape(uint32_t idc)
{
    NSCursor *c;
    switch (idc) {
    case 0: return;                                      /* SetCursor(NULL): ShowCursor decides */
    case 32513: c = NSCursor.IBeamCursor; break;         /* IDC_IBEAM */
    case 32515: c = NSCursor.crosshairCursor; break;     /* IDC_CROSS */
    case 32644: case 32645: c = NSCursor.resizeLeftRightCursor; break;
    case 32649: c = NSCursor.pointingHandCursor; break;  /* IDC_HAND */
    default: c = NSCursor.arrowCursor; break;            /* IDC_ARROW and the game's own cursors */
    }
    [c set];
}

uint32_t sp_mac_clip_get(uint16_t *buf, uint32_t cap)
{
    @autoreleasepool {
        NSString *s = [NSPasteboard.generalPasteboard stringForType:NSPasteboardTypeString];
        if (!s || !cap) return 0;
        NSUInteger n = s.length < cap - 1 ? s.length : cap - 1;
        [s getCharacters:(unichar *)buf range:NSMakeRange(0, n)];
        buf[n] = 0;
        return (uint32_t)n;
    }
}

int sp_mac_clip_set(const uint16_t *u, uint32_t n)
{
    @autoreleasepool {
        NSString *s = [NSString stringWithCharacters:(const unichar *)u length:n];
        [NSPasteboard.generalPasteboard clearContents];
        return [NSPasteboard.generalPasteboard setString:s forType:NSPasteboardTypeString] ? 1 : 0;
    }
}

int sp_mac_message_box(const char *text, const char *caption, uint32_t type)
{
    /* MB_OK = 0, MB_OKCANCEL = 1, MB_YESNO = 4. Main thread only; others log and return the default. */
    if (![NSThread isMainThread]) return (type & 0xf) == 4 ? 7 /* IDNO */ : 1 /* IDOK */;
    @autoreleasepool {
        NSAlert *a = [NSAlert new];
        a.messageText = [NSString stringWithUTF8String:caption ? caption : ""] ?: @"";
        a.informativeText = [NSString stringWithUTF8String:text ? text : ""] ?: @"";
        switch (type & 0xf) {
        case 1: [a addButtonWithTitle:@"OK"]; [a addButtonWithTitle:@"Cancel"]; break;
        case 4: [a addButtonWithTitle:@"Yes"]; [a addButtonWithTitle:@"No"]; break;
        default: [a addButtonWithTitle:@"OK"]; break;
        }
        NSModalResponse r = [a runModal];
        int first = r == NSAlertFirstButtonReturn;
        switch (type & 0xf) {
        case 1: return first ? 1 : 2;   /* IDOK / IDCANCEL */
        case 4: return first ? 6 : 7;   /* IDYES / IDNO */
        default: return 1;
        }
    }
}

uint32_t sp_mac_double_click_ms(void) { return (uint32_t)lround(NSEvent.doubleClickInterval * 1000.0); }

/* ---- window snapshot (docs/design/platform-macos.md 10.12) --------------------------------------
 * Writes a PNG of the window. First tries the window server's own picture of this process's window
 * (CGWindowListCreateImage, looked up at run time because the macOS 15 SDK marks it unavailable);
 * if that is refused, AppKit draws the frame view (title bar and content) into a bitmap, which
 * shows the window chrome but not CAMetalLayer pixels. Returns 1 for the window-server image, 2 for
 * the AppKit drawing, 0 on failure. Main thread only. */
int sp_mac_snapshot(sp_hwnd h, const char *path)
{
    spwin *s = by_hwnd(h);
    if (!s || !path) return 0;
    @autoreleasepool {
        NSData *png = nil;
        int how = 0;
        typedef CGImageRef (*cgwl_fn)(CGRect, uint32_t, uint32_t, uint32_t);
        cgwl_fn f = (cgwl_fn)dlsym(RTLD_DEFAULT, "CGWindowListCreateImage");
        if (f) {
            /* kCGWindowListOptionIncludingWindow 8, kCGWindowImageBoundsIgnoreFraming 1 | NominalResolution 16 */
            CGImageRef img = f(CGRectNull, 8, (uint32_t)s->win.windowNumber, 1 | 16);
            if (img && CGImageGetWidth(img) > 1) {
                NSBitmapImageRep *rep = [[NSBitmapImageRep alloc] initWithCGImage:img];
                png = [rep representationUsingType:NSBitmapImageFileTypePNG properties:@{}];
                how = 1;
            }
            if (img) CGImageRelease(img);
        }
        if (!png) {
            NSView *frame = s->win.contentView.superview ? s->win.contentView.superview : s->win.contentView;
            NSBitmapImageRep *rep = [frame bitmapImageRepForCachingDisplayInRect:frame.bounds];
            if (rep) {
                [frame cacheDisplayInRect:frame.bounds toBitmapImageRep:rep];
                png = [rep representationUsingType:NSBitmapImageFileTypePNG properties:@{}];
                how = 2;
            }
        }
        if (!png || ![png writeToFile:[NSString stringWithUTF8String:path] atomically:YES]) return 0;
        return how;
    }
}
