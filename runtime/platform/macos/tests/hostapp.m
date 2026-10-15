/*
 * hostapp.m - the platform layer's test app: a "Canvas" window driven exactly like the game's main loop, with no
 * game code. Our own code only.
 *
 *   sphost --script tests/input-script.txt --log out.log
 *       Test mode: replays the reference script as real NSEvents posted to the AppKit queue
 *       ([NSApp postEvent:atStart:NO]) and writes the WM_* log; compare with tests/expected-wm.log.
 *   sphost --display-test [--retina | --no-fullscreen], --safe-area-test [--retina], --text-test
 *       The window tests further down; each prints its pass/fail counts.
 *   sphost --interactive [--seconds N] [--tone] [--quiet]
 *       Opens the window, runs the 0x8482f0-style pump with a 16 ms frame, logs input messages to
 *       stderr, clears the Metal layer to a colour that follows the input, posts WM_USER+0x100 from
 *       a worker thread once a second, and optionally plays the fake-DirectSound test tone.
 */
#import <AppKit/AppKit.h>
#import <CoreGraphics/CoreGraphics.h>
#import <QuartzCore/CAMetalLayer.h>

#include <math.h>
#include <pthread.h>
#include <stdatomic.h>
#include <time.h>

#include "fake_dac.h"
#include "script_runner.h"
#include "sp_macos.h"

static sp_hwnd g_h;
static double g_ts = SP_MAC_SYNTH_TS;
static NSEventModifierFlags g_mods;

static NSWindow *nswin(void) { return [NSApp windowWithWindowNumber:sp_mac_window_number(g_h)]; }
static double next_ts(void) { g_ts += 0.001; return g_ts; }

/* ---- script driver: build real NSEvents ---------------------------------------------------------- */
static NSString *str16(const uint16_t *c, int n) { return [NSString stringWithCharacters:(const unichar *)c length:(NSUInteger)n]; }

static void post(NSEvent *e) { if (e) [NSApp postEvent:e atStart:NO]; else fprintf(stderr, "event creation failed\n"); }

static void d_key(void *c, int action, uint16_t kvk, const uint16_t *ch, int n, const uint16_t *ig, int ni, uint32_t mods)
{
    NSEvent *e = [NSEvent keyEventWithType:action == SP_KEY_UP ? NSEventTypeKeyUp : NSEventTypeKeyDown
                                  location:NSZeroPoint modifierFlags:g_mods timestamp:next_ts()
                              windowNumber:sp_mac_window_number(g_h) context:nil
                                characters:str16(ch, n) charactersIgnoringModifiers:str16(ig, ni)
                                 isARepeat:action == SP_KEY_REPEAT keyCode:kvk];
    post(e);
}

static void d_flags(void *c, uint16_t kvk, int on)
{
    NSEventModifierFlags gen = 0, dev = 0;
    switch (kvk) {
    case 0x38: gen = NSEventModifierFlagShift; dev = 0x02; break;
    case 0x3C: gen = NSEventModifierFlagShift; dev = 0x04; break;
    case 0x3B: gen = NSEventModifierFlagControl; dev = 0x01; break;
    case 0x3E: gen = NSEventModifierFlagControl; dev = 0x2000; break;
    case 0x3A: gen = NSEventModifierFlagOption; dev = 0x20; break;
    case 0x3D: gen = NSEventModifierFlagOption; dev = 0x40; break;
    case 0x37: gen = NSEventModifierFlagCommand; dev = 0x08; break;
    case 0x36: gen = NSEventModifierFlagCommand; dev = 0x10; break;
    case 0x39: gen = NSEventModifierFlagCapsLock; break;
    }
    if (on) g_mods |= gen | dev;
    else {
        g_mods &= ~dev;
        if (kvk == 0x39 || !(g_mods & 0x217F & ~dev)) g_mods &= ~gen;   /* generic bit clears with the last key */
    }
    NSEvent *e = [NSEvent keyEventWithType:NSEventTypeFlagsChanged location:NSZeroPoint modifierFlags:g_mods
                                 timestamp:next_ts() windowNumber:sp_mac_window_number(g_h) context:nil
                                characters:@"" charactersIgnoringModifiers:@"" isARepeat:NO keyCode:kvk];
    post(e);
}

static void d_mouse(void *c, int kind, int button, int32_t x, int32_t y, int clicks)
{
    NSWindow *w = nswin();
    NSView *v = w.contentView;
    NSPoint p = [v convertPoint:NSMakePoint(x + 0.5, v.bounds.size.height - y - 0.5) toView:nil];
    NSEventType t;
    switch (kind) {
    case SR_MOUSE_MOVE: t = NSEventTypeMouseMoved; break;
    case SR_MOUSE_DRAG: t = button == SP_MB_RIGHT ? NSEventTypeRightMouseDragged : button == SP_MB_MIDDLE ? NSEventTypeOtherMouseDragged : NSEventTypeLeftMouseDragged; break;
    case SR_MOUSE_DOWN: t = button == SP_MB_RIGHT ? NSEventTypeRightMouseDown : button == SP_MB_MIDDLE ? NSEventTypeOtherMouseDown : NSEventTypeLeftMouseDown; break;
    default: t = button == SP_MB_RIGHT ? NSEventTypeRightMouseUp : button == SP_MB_MIDDLE ? NSEventTypeOtherMouseUp : NSEventTypeLeftMouseUp; break;
    }
    NSEvent *e = [NSEvent mouseEventWithType:t location:p modifierFlags:g_mods timestamp:next_ts()
                                windowNumber:w.windowNumber context:nil eventNumber:0
                                  clickCount:(kind == SR_MOUSE_DOWN || kind == SR_MOUSE_UP) ? clicks : 0
                                    pressure:(kind == SR_MOUSE_DOWN || kind == SR_MOUSE_DRAG) ? 1.0f : 0.0f];
    if (e) sp_mac_note_synthetic_point(e.timestamp, p.x, p.y);   /* the point meant (sp_window.m) */
    post(e);
}

static void d_wheel(void *c, int lines)
{
    CGEventRef ev = CGEventCreateScrollWheelEvent(NULL, kCGScrollEventUnitLine, 1, lines);
    CGEventSetTimestamp(ev, (CGEventTimestamp)(next_ts() * 1e9));
    NSEvent *e = [NSEvent eventWithCGEvent:ev];
    CFRelease(ev);
    post(e);
}

static void d_resize(void *c, int32_t w, int32_t h) { sp_mac_set_client_size(g_h, w, h); }
static void d_activate(void *c, int on) { sp_window_activate(g_h, on, 1); }
static void d_close(void *c) { [nswin() performClose:nil]; }   /* -> windowShouldClose: -> WM_CLOSE */
static void d_show(void *c)
{
    [nswin() orderFront:nil];
    sp_show_window(g_h, 1, 1);
    sp_update_window(g_h);
}

static unsigned g_frame;
static void d_frame(void *c, sp_hwnd h)
{
    g_frame++;
    float t = (float)g_frame;
    sp_mac_present_clear(h, 0.5f + 0.5f * sinf(t * 0.3f), 0.3f, 0.5f + 0.5f * cosf(t * 0.2f));
}

/* ---- interactive mode ---------------------------------------------------------------------------- */
static int g_quiet;
static float g_col[3] = {0.1f, 0.2f, 0.3f};
static _Atomic int g_run_poster = 1;

static int32_t live_proc(void *user, sp_hwnd h, uint32_t msg, uint32_t w, int32_t l)
{
    switch (msg) {
    case WM_PAINT: {
        sp_paintstruct ps;
        sp_begin_paint(h, &ps);
        sp_mac_present_clear(h, g_col[0], g_col[1], g_col[2]);
        sp_end_paint(h, &ps);
        return 0;
    }
    case WM_ERASEBKGND: return 1;
    case WM_SETCURSOR: return 1;
    case WM_CLOSE: sp_post_quit_message(0); return 0;
    case WM_NCHITTEST: case WM_MOUSEMOVE: break;
    default:
        if (!g_quiet)
            fprintf(stderr, "%-16s w=0x%08X l=0x%08X%s\n", sp_message_name(msg), w, (uint32_t)l,
                    (msg >= WM_KEYFIRST && msg <= WM_KEYLAST) ? "" : "");
        if (msg == WM_KEYDOWN || msg == WM_SYSKEYDOWN) { g_col[0] = (w & 7) / 7.0f; g_col[1] = ((w >> 3) & 7) / 7.0f; }
        if (msg == WM_CHAR && !g_quiet) fprintf(stderr, "   char U+%04X  GetKeyState(SHIFT)=%04X CONTROL=%04X MENU=%04X\n", w,
                                               (uint16_t)sp_get_key_state(VK_SHIFT), (uint16_t)sp_get_key_state(VK_CONTROL),
                                               (uint16_t)sp_get_key_state(VK_MENU));
        break;
    }
    if (msg == WM_MOUSEMOVE) { g_col[2] = (float)(l & 0xFFFF) / 800.0f; }
    return sp_def_window_proc(h, msg, w, l);
}

static void *poster(void *arg)
{
    for (uint32_t i = 0; atomic_load(&g_run_poster); i++) {
        sp_post_message(g_h, WM_USER + 0x100, i, 0);
        for (int k = 0; k < 10 && atomic_load(&g_run_poster); k++) { struct timespec ts = {0, 100000000}; nanosleep(&ts, NULL); }
    }
    return NULL;
}

static double now_ms(void) { return (double)clock_gettime_nsec_np(CLOCK_UPTIME_RAW) / 1e6; }

static int interactive(double seconds, int tone)
{
    if (sp_mac_app_init(1)) { fprintf(stderr, "Metal device unavailable\n"); return 1; }
    uint8_t *scratch = calloc(1, 4096);
    sp_set_scratch(scratch, 0x7FF00000u, 4096);
    g_h = sp_mac_create_window("platform test (no game code)", INT32_MIN, 0, 800, 600, live_proc, NULL, SP_STYLE_DBLCLKS);
    [nswin() makeKeyAndOrderFront:nil];
    sp_show_window(g_h, 1, 0);
    sp_update_window(g_h);
    fake_dac dac; int dac_on = 0;
    if (tone) { dac_on = fake_dac_start(&dac, sp_coreaudio_backend(), 440.0, 0.05, 50.0f) == 0; if (!dac_on) fprintf(stderr, "audio failed\n"); }
    pthread_t pt; pthread_create(&pt, NULL, poster, NULL);
    double start = now_ms(), pump_ms = 0, worst = 0;
    unsigned frames = 0; int running = 1;
    while (running) {
        double f0 = now_ms();
        /* the game's pump 0x8482f0 */
        sp_invalidate_rect(g_h, 0);
        sp_msg m;
        while (sp_peek_message(&m, 0, 0, 0, PM_REMOVE)) {
            if (m.message == WM_QUIT) { running = 0; break; }
            sp_translate_message(&m);
            sp_dispatch_message(&m);
        }
        double f1 = now_ms();
        pump_ms += f1 - f0; if (f1 - f0 > worst) worst = f1 - f0;
        frames++;
        if (seconds > 0 && f1 - start > seconds * 1000.0 && running) sp_window_close_request(g_h);
        /* frame pacing like 0xf477d0: sleep the rest of a 16 ms target (SleepEx) */
        double left = 16.0 - (now_ms() - f0);
        if (left > 0) { struct timespec ts = {0, (long)(left * 1e6)}; nanosleep(&ts, NULL); }
    }
    double el = (now_ms() - start) / 1000.0;
    atomic_store(&g_run_poster, 0);
    pthread_join(pt, NULL);
    if (dac_on) {
        sp_dsbuf_stats st; sp_dsbuf_get_stats(dac.buf, &st);
        fprintf(stderr, "audio: %llu callbacks, %llu underruns, Dac latency %.1f ms\n",
                (unsigned long long)st.callbacks, (unsigned long long)st.underruns, dac.latency_ms);
        fake_dac_stop(&dac);
    }
    printf("interactive: %u pump passes in %.2f s (%.1f /s); pump+frame time avg %.3f ms, worst %.3f ms\n",
           frames, el, frames / el, pump_ms / frames, worst);
    return 0;
}

static int script_mode(const char *script, const char *logpath)
{
    if (sp_mac_app_init(0)) { fprintf(stderr, "Metal device unavailable\n"); return 1; }
    sp_mac_set_test_mode(1);
    uint8_t *scratch = calloc(1, 4096);
    sp_set_scratch(scratch, 0x7FF00000u, 4096);
    g_h = sp_mac_create_window("scripted input test", 200, 200, 640, 480, sr_wndproc, NULL, SP_STYLE_DBLCLKS);
    FILE *log = fopen(logpath, "w");
    if (!log) { perror(logpath); return 1; }
    sr_driver drv = {NULL, d_key, d_flags, d_mouse, d_wheel, d_resize, d_activate, d_close, d_show, d_frame};
    sr_set_log(log, &drv);
    double t0 = now_ms();
    int pumps = sr_run_script(script, g_h);
    fclose(log);
    printf("script: %d pump passes, %u frames, %.0f ms\n", pumps, sr_frames(), now_ms() - t0);
    return pumps < 0;
}

/* ---- display test: fullscreen transitions, the letterbox mapping, resize, Retina guest units -------
 *   sphost --display-test [--retina]
 * A window appears for about two seconds and covers the display for part of it. Test mode: the user's
 * real input is ignored; the keys and mouse points are synthetic NSEvents through the normal drain. */
static int g_dt_pass, g_dt_fail;
#define DCHECK(c, ...) do { if (c) g_dt_pass++; else { g_dt_fail++; fprintf(stderr, "FAIL %s:%d ", __FILE__, __LINE__); \
    fprintf(stderr, __VA_ARGS__); fprintf(stderr, "\n"); } } while (0)
static uint32_t g_dt_sizes, g_dt_size_l, g_dt_move_l, g_dt_moves, g_dt_keys;
static int32_t dt_proc(void *user, sp_hwnd h, uint32_t msg, uint32_t w, int32_t l)
{
    if (msg == WM_SIZE) g_dt_sizes++, g_dt_size_l = (uint32_t)l;
    if (msg == WM_MOUSEMOVE) g_dt_moves++, g_dt_move_l = (uint32_t)l;
    if (msg == WM_KEYDOWN || msg == WM_KEYUP) g_dt_keys++;
    if (msg == WM_PAINT) {
        sp_paintstruct ps;
        sp_begin_paint(h, &ps);
        sp_mac_present_clear(h, 0.2f, 0.4f, 0.6f);
        sp_end_paint(h, &ps);
        return 0;
    }
    return sp_def_window_proc(h, msg, w, l);
}
static void dt_pump(int passes)
{
    for (int i = 0; i < passes; i++) {
        sp_invalidate_rect(g_h, 0);
        sp_msg m;
        while (sp_peek_message(&m, 0, 0, 0, PM_REMOVE)) { sp_translate_message(&m); sp_dispatch_message(&m); }
        struct timespec ts = {0, 16000000};
        nanosleep(&ts, NULL);
    }
}
static void dt_cmd_ctrl_f(void)
{
    NSEventModifierFlags f = NSEventModifierFlagCommand | NSEventModifierFlagControl | 0x08 | 0x01;
    for (int up = 0; up < 2; up++)
        post([NSEvent keyEventWithType:up ? NSEventTypeKeyUp : NSEventTypeKeyDown location:NSZeroPoint modifierFlags:f
                             timestamp:next_ts() windowNumber:sp_mac_window_number(g_h) context:nil characters:@"f"
           charactersIgnoringModifiers:@"f" isARepeat:NO keyCode:0x03]);
}
static uint32_t lp(int x, int y) { return ((uint32_t)(uint16_t)y << 16) | (uint16_t)x; }
static int dt_move_maps(int x, int y)
{
    g_dt_moves = 0, g_dt_move_l = 0xffffffffu;
    sp_mac_post_mouse(g_h, 0, 0, x, y, 0);
    dt_pump(2);
    return g_dt_moves >= 1 && g_dt_move_l == lp(x, y);
}

static int display_test(int retina, int no_fs)
{
    if (retina) setenv("SPORE_RETINA", "1", 1);
    else unsetenv("SPORE_RETINA");
    unsetenv("SPORE_D3D9");                               /* the default presenter (Metal) */
    unsetenv("SPORE_FULLSCREEN_ESC");
    if (no_fs) setenv("SPORE_FULLSCREEN", "0", 1);
    else unsetenv("SPORE_FULLSCREEN");
    unsetenv("SPORE_FULLSCREEN_SAFE_AREA");                /* off (the default) */
    if (sp_mac_app_init(0)) { fprintf(stderr, "Metal device unavailable\n"); return 1; }
    sp_mac_set_test_mode(1);
    sp_mac_set_letterbox(1);
    uint8_t *scratch = calloc(1, 4096);
    sp_set_scratch(scratch, 0x7FF00000u, 4096);
    double k = sp_mac_guest_scale();
    g_h = sp_mac_create_window("display test (no game code)", 200, 200, 640, 480, dt_proc, NULL, SP_STYLE_DBLCLKS);
    NSWindow *w = nswin();
    if (getenv("SPHOST_KEY")) {                           /* SPHOST_KEY=1: make it key (takes the focus) */
        [NSApp activateIgnoringOtherApps:YES];
        [w makeKeyAndOrderFront:nil];
    } else {
        [w orderFront:nil];
    }
    sp_show_window(g_h, 1, 0);
    dt_pump(3);
    NSRect sf = w.screen ? w.screen.frame : NSScreen.screens[0].frame;
    double cr[4];
    int32_t cx, cy, cw, ch;
    printf("display test%s: guest scale %.1f, backing scale %.1f, screen %.0fx%.0f pt\n", retina ? " (SPORE_RETINA=1)" : "", k,
           w.backingScaleFactor, sf.size.width, sf.size.height);

    /* 1. windowed: the client is the view in guest units */
    sp_get_last_client(g_h, &cx, &cy, &cw, &ch);
    NSSize vb = w.contentView.bounds.size;
    DCHECK(cw == 640 && ch == 480, "client 640x480 guest units, got %dx%d", cw, ch);
    DCHECK(fabs(vb.width - 640 / k) < 0.01 && fabs(vb.height - 480 / k) < 0.01, "view %.0fx%.0f pt = client / %.1f", vb.width, vb.height, k);
    DCHECK(sp_mac_caption_height() == (int32_t)lround((w.frame.size.height - vb.height) * k), "caption in guest units (%d)",
           sp_mac_caption_height());
    DCHECK(dt_move_maps(101, 51), "windowed mouse (101,51) -> 0x%08x", g_dt_move_l);
    DCHECK(dt_move_maps(639, 479), "windowed mouse (639,479) -> 0x%08x", g_dt_move_l);
    if (retina) {
        CAMetalLayer *l = (__bridge CAMetalLayer *)sp_mac_metal_layer(g_h);
        DCHECK(fabs(l.drawableSize.width - 640 * w.backingScaleFactor / k) < 0.5, "drawable %.0f px = client (1:1 at this scale)",
               l.drawableSize.width);
    }

    if (no_fs) {                                         /* SPORE_FULLSCREEN=0: the toggle does nothing */
        NSRect f0 = w.frame;
        g_dt_keys = 0;
        dt_cmd_ctrl_f();
        dt_pump(4);
        DCHECK(sp_mac_is_fullscreen(g_h) == 0 && NSEqualRects(w.frame, f0), "SPORE_FULLSCREEN=0: no fullscreen");
        DCHECK(sp_mac_set_fullscreen(g_h, 1) == 0, "SPORE_FULLSCREEN=0: the game's request refused too");
        DCHECK(g_dt_keys == 0, "the toggle is still not delivered (%u)", g_dt_keys);
        sp_destroy_window(g_h);
        dt_pump(1);
        printf("display test (SPORE_FULLSCREEN=0): %d passed, %d failed\n", g_dt_pass, g_dt_fail);
        return g_dt_fail ? 1 : 0;
    }

    /* 2. resize (as a live resize ends): WM_SIZE with the new client in guest units */
    g_dt_sizes = 0;
    sp_mac_set_client_size(g_h, 400, 300);                /* points */
    dt_pump(2);
    sp_get_last_client(g_h, &cx, &cy, &cw, &ch);
    DCHECK(g_dt_sizes >= 1 && g_dt_size_l == lp((int)lround(400 * k), (int)lround(300 * k)), "WM_SIZE %dx%d, got 0x%08x (%u)",
           (int)lround(400 * k), (int)lround(300 * k), g_dt_size_l, g_dt_sizes);
    DCHECK(cw == (int32_t)lround(400 * k) && ch == (int32_t)lround(300 * k), "client after resize %dx%d", cw, ch);
    NSRect before = w.frame;

    /* 3. Cmd-Ctrl-F: borderless over the display, the client size kept (no WM_SIZE), letterboxed */
    g_dt_sizes = 0, g_dt_keys = 0;
    dt_cmd_ctrl_f();
    dt_pump(6);
    sp_mac_content_rect(g_h, cr);
    DCHECK(sp_mac_is_fullscreen(g_h) == 1, "fullscreen after Cmd-Ctrl-F");
    DCHECK(NSEqualRects(w.frame, w.screen.frame), "window frame = its screen's frame (%.0fx%.0f)", w.frame.size.width, w.frame.size.height);
    DCHECK(w.styleMask == NSWindowStyleMaskBorderless, "borderless (style 0x%lx)", (unsigned long)w.styleMask);
    DCHECK(g_dt_sizes == 0, "no WM_SIZE: the game keeps its client (%u)", g_dt_sizes);
    DCHECK(g_dt_keys == 0, "the toggle key is not delivered to the game (%u key messages)", g_dt_keys);
    sp_get_last_client(g_h, &cx, &cy, &cw, &ch);
    DCHECK(cw == (int32_t)lround(400 * k) && ch == (int32_t)lround(300 * k), "client kept %dx%d", cw, ch);
    DCHECK(cx == (int32_t)lround(sf.origin.x * k) && cy == 0, "client origin = the display's origin (%d,%d)", cx, cy);
    DCHECK(fabs(cr[2] / cr[3] - 4.0 / 3.0) < 0.01, "content keeps 4:3 (%.1fx%.1f at %.1f,%.1f)", cr[2], cr[3], cr[0], cr[1]);
    DCHECK(fabs(cr[2] - sf.size.width) < 1 || fabs(cr[3] - sf.size.height) < 1, "content fills one dimension");
    if (w.isKeyWindow) {
        DCHECK((NSApp.presentationOptions & NSApplicationPresentationHideMenuBar) != 0, "menu bar hidden while key");
    } else {
        printf("  (window not key: the presentation options are applied on activation; not checked)\n");
    }
    printf("  fullscreen: client %dx%d shown at %.1f,%.1f %.1fx%.1f pt\n", cw, ch, cr[0], cr[1], cr[2], cr[3]);
    DCHECK(dt_move_maps(0, 0), "fullscreen mouse (0,0) -> 0x%08x", g_dt_move_l);
    DCHECK(dt_move_maps(cw / 2, ch / 2), "fullscreen mouse centre -> 0x%08x", g_dt_move_l);
    DCHECK(dt_move_maps(cw - 1, ch - 1), "fullscreen mouse corner -> 0x%08x", g_dt_move_l);
    {
        CAMetalLayer *l = (__bridge CAMetalLayer *)sp_mac_metal_layer(g_h);
        NSValue *v = [l valueForKey:@"sporeContentRect"];
        int bars = fabs(cr[2] - sf.size.width) >= 1 || fabs(cr[3] - sf.size.height) >= 1;
        DCHECK(bars ? v != nil : v == nil, "layer carries the content rectangle for the presenter (%s)", v ? "set" : "absent");
        if (v) {
            NSRect n = v.rectValue;
            DCHECK(fabs(n.origin.x * sf.size.width - cr[0]) < 0.01 && fabs(n.size.width * sf.size.width - cr[2]) < 0.01,
                   "normalised rect matches (%.4f %.4f)", n.origin.x, n.size.width);
        }
    }

    /* 4. the game sets its resolution while fullscreen: the client follows, still letterboxed */
    g_dt_sizes = 0;
    sp_mac_set_client_rect(g_h, 0, 0, (int32_t)lround(1280 * k), (int32_t)lround(720 * k));
    dt_pump(2);
    sp_mac_content_rect(g_h, cr);
    DCHECK(g_dt_sizes >= 1 && g_dt_size_l == lp((int)lround(1280 * k), (int)lround(720 * k)), "WM_SIZE 16:9 client (0x%08x)", g_dt_size_l);
    DCHECK(fabs(cr[2] / cr[3] - 16.0 / 9.0) < 0.01 && NSEqualRects(w.frame, w.screen.frame), "16:9 content, still over the display");

    /* 5. a presenter that cannot letterbox (any but Metal): the content is the whole view, the mapping stretches */
    sp_mac_set_letterbox(0);
    sp_mac_set_client_rect(g_h, 0, 0, 1000, 500);
    dt_pump(2);
    sp_mac_content_rect(g_h, cr);
    DCHECK(cr[0] == 0 && cr[1] == 0 && fabs(cr[2] - sf.size.width) < 0.01, "stretch: the whole view");
    DCHECK(dt_move_maps(999, 499) && dt_move_maps(500, 250), "stretched mapping exact (0x%08x)", g_dt_move_l);
    DCHECK([(__bridge CAMetalLayer *)sp_mac_metal_layer(g_h) valueForKey:@"sporeContentRect"] == nil, "no content rect on the layer");
    sp_mac_set_letterbox(1);
    sp_mac_set_client_rect(g_h, 0, 0, (int32_t)lround(400 * k), (int32_t)lround(300 * k));

    /* 6. Cmd-Ctrl-F again: the titled window comes back where it was, with its client */
    g_dt_keys = 0;
    dt_cmd_ctrl_f();
    dt_pump(6);
    sp_get_last_client(g_h, &cx, &cy, &cw, &ch);
    DCHECK(sp_mac_is_fullscreen(g_h) == 0, "windowed after the second Cmd-Ctrl-F");
    DCHECK(w.styleMask & NSWindowStyleMaskTitled, "titled again");
    DCHECK(NSEqualRects(w.frame, before), "frame restored (%.0f,%.0f %.0fx%.0f)", w.frame.origin.x, w.frame.origin.y, w.frame.size.width,
           w.frame.size.height);
    DCHECK(cw == (int32_t)lround(400 * k) && ch == (int32_t)lround(300 * k), "client %dx%d", cw, ch);
    DCHECK(NSApp.presentationOptions == NSApplicationPresentationDefault, "menu bar and Dock back");
    DCHECK(dt_move_maps(10, 20), "windowed mapping again (0x%08x)", g_dt_move_l);
    DCHECK(g_dt_keys == 0, "no key messages from the toggle (%u)", g_dt_keys);

    sp_destroy_window(g_h);
    dt_pump(1);
    printf("display test%s: %d passed, %d failed\n", retina ? " (SPORE_RETINA=1)" : "", g_dt_pass, g_dt_fail);
    return g_dt_fail ? 1 : 0;
}

/* ---- safe-area test: SPORE_FULLSCREEN_SAFE_AREA (the notch) ----------------------------------------
 *   sphost --safe-area-test [--retina]
 * Explicit insets (38 points at the top, a MacBook notch's height) on whatever display the test runs on:
 * the screen size the game sees loses them, the fullscreen letterbox sits below them, centred in the rest,
 * and the mouse maps through it; off again, everything is the whole display. The window covers the display
 * for about a second. */
static int dt_near(double a, double b, double tol) { return fabs(a - b) <= tol; }
static int safe_area_test(int retina)
{
    if (retina) setenv("SPORE_RETINA", "1", 1);
    else unsetenv("SPORE_RETINA");
    unsetenv("SPORE_D3D9");
    unsetenv("SPORE_FULLSCREEN");
    unsetenv("SPORE_FULLSCREEN_ESC");
    setenv("SPORE_FULLSCREEN_SAFE_AREA", "38", 1);
    if (sp_mac_app_init(0)) { fprintf(stderr, "Metal device unavailable\n"); return 1; }
    sp_mac_set_test_mode(1);
    sp_mac_set_letterbox(1);
    uint8_t *scratch = calloc(1, 4096);
    sp_set_scratch(scratch, 0x7FF00000u, 4096);
    double k = sp_mac_guest_scale();
    NSRect pf = NSScreen.screens[0].frame;                /* the primary display: sp_mac_screen_info's */
    int32_t sw, sh, offw, offh;
    double ins[4], cr[4];

    /* 1. the screen the game sees: the safe area (38 pt less high: 38 x k guest units) */
    sp_mac_screen_info(&sw, &sh, NULL, NULL, NULL, NULL);
    DCHECK(sp_mac_screen_safe_area(ins) == 1 && ins[0] == 38 && ins[1] == 0 && ins[2] == 0 && ins[3] == 0,
           "safe area on, insets 38,0,0,0 (%.0f,%.0f,%.0f,%.0f)", ins[0], ins[1], ins[2], ins[3]);
    DCHECK(sw == (int32_t)lround(pf.size.width * k), "screen width unchanged (%d)", sw);
    DCHECK(sh == (int32_t)lround(pf.size.height * k) - (int32_t)lround(38 * k), "screen height %d = display - %d", sh,
           (int)lround(38 * k));
    int32_t mmw, mmh, offmmw, offmmh;
    sp_mac_screen_info(NULL, NULL, NULL, NULL, &mmw, &mmh);
    setenv("SPORE_FULLSCREEN_SAFE_AREA", "0", 1);
    sp_mac_screen_info(NULL, NULL, NULL, NULL, &offmmw, &offmmh);
    DCHECK(mmw == offmmw && abs(mmh - (int32_t)lround(offmmh * (pf.size.height - 38) / pf.size.height)) <= 1,
           "physical size follows the safe area (%dx%d mm of %dx%d)", mmw, mmh, offmmw, offmmh);
    sp_mac_screen_info(&offw, &offh, NULL, NULL, NULL, NULL);
    DCHECK(offw == (int32_t)lround(pf.size.width * k) && offh == (int32_t)lround(pf.size.height * k) &&
           sp_mac_screen_safe_area(NULL) == 0, "switch 0: the whole display (%dx%d)", offw, offh);
    unsetenv("SPORE_FULLSCREEN_SAFE_AREA");
    sp_mac_screen_info(&offw, &offh, NULL, NULL, NULL, NULL);
    DCHECK(offh == (int32_t)lround(pf.size.height * k), "switch unset: the whole display (%d)", offh);
    setenv("SPORE_FULLSCREEN_SAFE_AREA", "1", 1);         /* the display's own insets (zero without a notch) */
    {
        int on = sp_mac_screen_safe_area(ins);
        sp_mac_screen_info(&offw, &offh, NULL, NULL, NULL, NULL);
        printf("  SPORE_FULLSCREEN_SAFE_AREA=1 on this display: insets %.0f,%.0f,%.0f,%.0f (%s), screen %dx%d\n", ins[0], ins[1],
               ins[2], ins[3], on ? "a notch" : "none", offw, offh);
        DCHECK(offh == (int32_t)lround((pf.size.height - ins[0] - ins[1]) * k), "switch 1: the display's safe area (%d)", offh);
    }
    setenv("SPORE_FULLSCREEN_SAFE_AREA", "38", 1);
    printf("safe-area test%s: guest scale %.1f, primary display %.0fx%.0f pt, screen seen %dx%d\n", retina ? " (SPORE_RETINA=1)" : "",
           k, pf.size.width, pf.size.height, sw, sh);

    g_h = sp_mac_create_window("safe-area test (no game code)", 200, 200, 640, 480, dt_proc, NULL, SP_STYLE_DBLCLKS);
    NSWindow *w = nswin();
    [w orderFront:nil];
    sp_show_window(g_h, 1, 0);
    dt_pump(3);
    sp_mac_content_rect(g_h, cr);
    DCHECK(cr[0] == 0 && cr[1] == 0 && dt_near(cr[2], 640 / k, 0.01), "windowed: the whole view, unaffected (%.1f,%.1f)", cr[0], cr[1]);

    /* 2. fullscreen at the display's own aspect (-r:2056x1329 on a 2056x1329-point display): below the strip, centred */
    DCHECK(sp_mac_set_fullscreen(g_h, 1) == 1, "fullscreen");
    NSRect sf = w.screen.frame;
    double px = w.backingScaleFactor > 0 ? w.backingScaleFactor : 1, tol = 1.0 / px + 0.001;
    double aw = sf.size.width, ah = sf.size.height - 38; /* the safe area, view points from y = 38 */
    DCHECK(NSEqualRects(w.frame, sf), "the window still covers the whole display");
    sp_mac_set_client_rect(g_h, 0, 0, (int32_t)lround(sf.size.width * k), (int32_t)lround(sf.size.height * k));
    dt_pump(2);
    sp_mac_content_rect(g_h, cr);
    printf("  display aspect: client %ldx%ld shown at %.1f,%.1f %.1fx%.1f pt\n", lround(sf.size.width * k), lround(sf.size.height * k),
           cr[0], cr[1], cr[2], cr[3]);
    DCHECK(cr[1] >= 38 - 0.001 && cr[1] + cr[3] <= sf.size.height + 0.001, "content below the 38 pt strip (y %.2f)", cr[1]);
    DCHECK(dt_near(cr[3], ah, tol) && dt_near((cr[0] * 2 + cr[2]), aw, tol * 2), "fills the safe height, centred across (%.2f)",
           cr[0]);
    DCHECK(fabs(cr[2] / cr[3] - sf.size.width / sf.size.height) < 0.005, "aspect kept (%.4f)", cr[2] / cr[3]);
    {
        int32_t cw = (int32_t)lround(sf.size.width * k), ch = (int32_t)lround(sf.size.height * k);
        DCHECK(dt_move_maps(0, 0), "mouse at the content's top-left -> 0x%08x", g_dt_move_l);
        DCHECK(dt_move_maps(cw - 1, 0), "mouse at the top-right -> 0x%08x", g_dt_move_l);
        DCHECK(dt_move_maps(0, ch - 1), "mouse at the bottom-left -> 0x%08x", g_dt_move_l);
        DCHECK(dt_move_maps(cw - 1, ch - 1), "mouse at the bottom-right -> 0x%08x", g_dt_move_l);
    }
    {
        NSValue *v = [(__bridge CAMetalLayer *)sp_mac_metal_layer(g_h) valueForKey:@"sporeContentRect"];
        NSRect n = v ? v.rectValue : NSZeroRect;
        DCHECK(v && dt_near(n.origin.y * sf.size.height, cr[1], 0.01) && dt_near(n.size.height * sf.size.height, cr[3], 0.01),
               "the layer's content rectangle starts below the strip (%.4f %.4f)", n.origin.y, n.size.height);
    }

    /* 3. 16:9: centred in the area below the strip, not in the whole display */
    sp_mac_set_client_rect(g_h, 0, 0, (int32_t)lround(1280 * k), (int32_t)lround(720 * k));
    dt_pump(2);
    sp_mac_content_rect(g_h, cr);
    printf("  16:9: shown at %.1f,%.1f %.1fx%.1f pt\n", cr[0], cr[1], cr[2], cr[3]);
    DCHECK(cr[1] >= 38 - 0.001, "16:9 below the strip (y %.2f)", cr[1]);
    DCHECK(dt_near(cr[0] * 2 + cr[2], aw, tol * 2) && dt_near((cr[1] - 38) * 2 + cr[3], ah, tol * 2), "16:9 centred in the safe area");
    DCHECK(dt_move_maps((int)lround(1280 * k) - 1, (int)lround(720 * k) - 1) && dt_move_maps(0, 0), "16:9 corners map");

    /* 4. the safe area's own size (the below-the-notch preset): exactly the safe area */
    sp_mac_set_client_rect(g_h, 0, 0, (int32_t)lround(aw * k), (int32_t)lround(ah * k));
    dt_pump(2);
    sp_mac_content_rect(g_h, cr);
    DCHECK(dt_near(cr[0], 0, 0.001) && dt_near(cr[1], 38, 0.001) && dt_near(cr[2], aw, 0.001) && dt_near(cr[3], ah, 0.001),
           "below-the-notch size fills the safe area 1:1 (%.1f,%.1f %.1fx%.1f)", cr[0], cr[1], cr[2], cr[3]);
    {
        int32_t cx, cy, cw, ch;
        sp_get_last_client(g_h, &cx, &cy, &cw, &ch);
        DCHECK(cx == (int32_t)lround(sf.origin.x * k) && cw == (int32_t)lround(aw * k) && ch == (int32_t)lround(ah * k),
               "client %dx%d at the display origin (%d,%d)", cw, ch, cx, cy);
        DCHECK(dt_move_maps(cw - 1, ch - 1) && dt_move_maps(1, 1), "corners map (0x%08x)", g_dt_move_l);
    }

    /* 5. a presenter that cannot letterbox (any but Metal): the whole view (the strip is drawn on) */
    sp_mac_set_letterbox(0);
    sp_mac_set_client_rect(g_h, 0, 0, 1000, 500);
    dt_pump(2);
    sp_mac_content_rect(g_h, cr);
    DCHECK(cr[0] == 0 && cr[1] == 0 && dt_near(cr[3], sf.size.height, 0.01), "no letterbox: the whole view (%.1f)", cr[1]);
    sp_mac_set_letterbox(1);

    /* 6. switched off at run time: the letterbox uses the whole display again */
    setenv("SPORE_FULLSCREEN_SAFE_AREA", "0", 1);
    sp_mac_set_client_rect(g_h, 0, 0, (int32_t)lround(1280 * k), (int32_t)lround(720 * k));
    dt_pump(2);
    sp_mac_content_rect(g_h, cr);
    DCHECK(dt_near(cr[1] * 2 + cr[3], sf.size.height, tol * 2), "off: centred in the whole display (y %.2f)", cr[1]);
    setenv("SPORE_FULLSCREEN_SAFE_AREA", "38", 1);

    DCHECK(sp_mac_set_fullscreen(g_h, 0) == 1, "windowed again");
    dt_pump(3);
    sp_mac_content_rect(g_h, cr);
    DCHECK(cr[0] == 0 && cr[1] == 0, "windowed: the whole view");
    sp_destroy_window(g_h);
    dt_pump(1);
    unsetenv("SPORE_FULLSCREEN_SAFE_AREA");
    printf("safe-area test%s: %d passed, %d failed\n", retina ? " (SPORE_RETINA=1)" : "", g_dt_pass, g_dt_fail);
    return g_dt_fail ? 1 : 0;
}

#include "text_test.m"   /* sphost --text-test */

int main(int argc, char **argv)
{
    @autoreleasepool {
        const char *script = NULL, *logp = "gui-wm.log";
        double seconds = 0; int tone = 0, inter = 0;
        for (int i = 1; i < argc; i++) {
            if (!strcmp(argv[i], "--script") && i + 1 < argc) script = argv[++i];
            else if (!strcmp(argv[i], "--log") && i + 1 < argc) logp = argv[++i];
            else if (!strcmp(argv[i], "--interactive")) inter = 1;
            else if (!strcmp(argv[i], "--seconds") && i + 1 < argc) seconds = atof(argv[++i]);
            else if (!strcmp(argv[i], "--tone")) tone = 1;
            else if (!strcmp(argv[i], "--quiet")) g_quiet = 1;
            else if (!strcmp(argv[i], "--text-test")) return text_test();
            else if (!strcmp(argv[i], "--safe-area-test")) return safe_area_test(i + 1 < argc && !strcmp(argv[i + 1], "--retina"));
            else if (!strcmp(argv[i], "--display-test"))
                return display_test(i + 1 < argc && !strcmp(argv[i + 1], "--retina"), i + 1 < argc && !strcmp(argv[i + 1], "--no-fullscreen"));
        }
        if (script) return script_mode(script, logp);
        if (inter) return interactive(seconds, tone);
        fprintf(stderr, "usage: sphost --script FILE [--log FILE] | --interactive [--seconds N] [--tone] [--quiet]\n");
        return 2;
    }
}
