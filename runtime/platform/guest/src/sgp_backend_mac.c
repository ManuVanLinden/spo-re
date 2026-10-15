/* sgp_backend_mac.c -- the AppKit back end of the guest window layer: thin calls into
 * runtime/platform/macos/src/sp_window.m (sp_macos.h). Linking this file makes it the default
 * (strong sgp_default_backend); SPORE_HEADLESS=1 still selects the headless one. Also the desktop display
 * mode the D3D9 adapter reports (sg_host_display_mode: Retina, the notch). */
#include <stdio.h>
#include <stdlib.h>

#include "sgp_int.h"
#include "sp_macos.h"

static int mac_init(void) { return sp_mac_app_init(0); }

static void mac_attach(sp_hwnd h, const char *title, int32_t x, int32_t y, int32_t w, int32_t hh) {
    sp_mac_attach_window(h, title && *title ? title : "Spore", x, y, w, hh);
    /* The CGWindowID, for `screencapture -l <id>` screenshots of the window */
    fprintf(stderr, "platform: window 0x%x on screen as macOS window %d, client %dx%d\n", (unsigned)h,
            sp_mac_window_number(h), (int)w, (int)hh);
    sp_mac_set_client_rect(h, x, y, w, hh); /* clamp into the visible frame (the game asks for (0,0)) */
}
static void mac_detach(sp_hwnd h) { sp_mac_detach_window(h); }
static void mac_rect(sp_hwnd h, int attached, int32_t x, int32_t y, int32_t w, int32_t hh) {
    if (attached) sp_mac_set_client_rect(h, x, y, w, hh);
    else sp_window_rect_changed(h, x, y, w, hh); /* not on screen yet: recorded for the first show */
}
static int mac_show(sp_hwnd h, int show, int activate) {
    if (!show) { sp_mac_order_out(h); return 0; }
    if (activate) { sp_mac_make_key(h); return 1; } /* windowDidBecomeKey sends the activation messages */
    sp_mac_order_front(h);
    return 0;
}
static int32_t mac_caption(void) { return sp_mac_caption_height(); }
/* sp_mac_set_cursor_pos decides: the scripted point while a script drives the mouse, otherwise a
 * real warp only while one of our windows is key. */
static void mac_setpos(int32_t x, int32_t y) { (void)sp_mac_set_cursor_pos(x, y); }
static void *mac_layer(sp_hwnd h) { return sp_mac_metal_layer(h); }
static int mac_fullscreen(sp_hwnd h, int on) { return sp_mac_set_fullscreen(h, on); }

static const sgp_backend mac_backend = {
    "AppKit", mac_init, mac_attach, mac_detach, mac_rect, mac_show, sp_mac_set_title, sp_mac_is_miniaturized,
    mac_caption, sp_mac_screen_info, sp_mac_cursor_pos, mac_setpos, sp_mac_show_cursor, sp_mac_set_cursor_shape,
    sp_mac_clip_get, sp_mac_clip_set, sp_mac_message_box, sp_mac_double_click_ms, mac_layer, mac_fullscreen,
};

/* The desktop display mode the D3D9 adapter reports (runtime/d3d9null's weak default is
 * SPORE_DISPLAY_MODE, else 1920x1080@60, and stays so for the test binaries that have no window).
 * DEVIATIONS 3.47: it is the primary display's size in guest units (sp_mac_screen_info): points at
 * 60 Hz by default (for example 2056x1329), its pixels at the panel's rate with
 * SPORE_RETINA=1, so the game's resolution list (every enumerated mode from its minimum up to this
 * desktop size, built at 0x7ccf00; the adapter's list ends with the desktop size itself) reaches the
 * display's own size and a window of that size is drawn 1:1.
 * DEVIATIONS 4.12: with SPORE_FULLSCREEN_SAFE_AREA on and a notch (non-zero insets), the size is the
 * safe area's (2056x1291 points, 4112x2582 pixels on a 2056x1329-point display).
 * SPORE_DISPLAY_MODE=WxH[@Hz] overrides everything (=1920x1080@60 gives the fixed mode the test binaries use);
 * SPORE_HEADLESS=1 (no window, a 1024x768 pretend screen) keeps 1920x1080@60. */
int sg_host_display_mode(uint32_t *w, uint32_t *h, uint32_t *hz) {
    const char *s = getenv("SPORE_DISPLAY_MODE"), *hl = getenv("SPORE_HEADLESS");
    unsigned a = 0, b = 0, r = 60;
    if (s && sscanf(s, "%ux%u@%u", &a, &b, &r) >= 2 && a >= 640 && b >= 480) {
        *w = a, *h = b, *hz = r ? r : 60;
        return 0;
    }
    if (!(hl && *hl && *hl != '0')) {
        int retina = sp_mac_guest_scale() != 1.0;
        int32_t sw = 0, sh = 0, f = 60;
        sp_mac_screen_info(&sw, &sh, NULL, &f, NULL, NULL);
        if (sw >= 640 && sh >= 480) {
            *w = (uint32_t)sw, *h = (uint32_t)sh, *hz = retina && f > 0 ? (uint32_t)f : 60;
            return 0;
        }
    }
    *w = 1920, *h = 1080, *hz = 60;
    return 0;
}

const sgp_backend *sgp_default_backend(void) { return &mac_backend; }
