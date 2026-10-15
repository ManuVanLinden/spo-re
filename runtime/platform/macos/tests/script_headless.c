/* script_headless.c - replays the reference script straight into sp_input_* (no AppKit).
 * Checks the portable queue/input core; tests/hostapp.m checks the same script through NSEvents. */
#include "script_runner.h"

#include <stdlib.h>
#include <string.h>

static sp_hwnd g_h;
static int32_t g_ox = 100, g_oy = 200, g_w = 640, g_hh = 480;

static void c2s(void *c, sp_hwnd h, int32_t *x, int32_t *y) { (void)c; (void)h; *x += g_ox; *y += g_oy; }
static void s2c(void *c, sp_hwnd h, int32_t *x, int32_t *y) { (void)c; (void)h; *x -= g_ox; *y -= g_oy; }
static void csz(void *c, sp_hwnd h, int32_t *w, int32_t *hh) { (void)c; (void)h; *w = g_w; *hh = g_hh; }

static void d_key(void *c, int action, uint16_t kvk, const uint16_t *ch, int n, const uint16_t *ig, int ni, uint32_t mods)
{ (void)c; (void)mods; sp_input_key(kvk, action, ch, n, ig, ni); }
static void d_flags(void *c, uint16_t kvk, int on)
{ (void)c; if (kvk == 0x39) sp_input_capslock(on); else sp_input_modifier(kvk, on); }
static void d_mouse(void *c, int kind, int button, int32_t x, int32_t y, int clicks)
{
    (void)c;
    if (kind == SR_MOUSE_MOVE || kind == SR_MOUSE_DRAG) sp_input_mouse_move(g_h, x, y);
    else sp_input_mouse_button(g_h, button, kind == SR_MOUSE_DOWN, x, y, clicks);
}
static void d_wheel(void *c, int lines) { (void)c; sp_input_wheel(g_h, lines * WHEEL_DELTA); }
static void d_resize(void *c, int32_t w, int32_t h) { (void)c; g_w = w; g_hh = h; sp_window_rect_changed(g_h, g_ox, g_oy, w, h); }
static void d_activate(void *c, int on) { (void)c; sp_window_activate(g_h, on, 1); }
static void d_close(void *c) { (void)c; sp_window_close_request(g_h); }
static void d_show(void *c) { (void)c; sp_show_window(g_h, 1, 1); sp_update_window(g_h); }

int main(int argc, char **argv)
{
    const char *script = argc > 1 ? argv[1] : "tests/input-script.txt";
    const char *out = argc > 2 ? argv[2] : NULL;
    FILE *log = out ? fopen(out, "w") : stdout;
    sp_host_hooks hooks = {0};
    hooks.client_to_screen = c2s; hooks.screen_to_client = s2c; hooks.get_client_size = csz;
    sp_platform_init(&hooks, NULL);
    g_h = sp_window_register(sr_wndproc, NULL, SP_STYLE_DBLCLKS);
    sp_window_rect_changed(g_h, g_ox, g_oy, g_w, g_hh);     /* initial rect, silent before show */
    sr_driver drv = {NULL, d_key, d_flags, d_mouse, d_wheel, d_resize, d_activate, d_close, d_show, NULL};
    sr_set_log(log, &drv);
    int pumps = sr_run_script(script, g_h);
    fprintf(stderr, "headless: %d pump passes, %u frames\n", pumps, sr_frames());
    if (out) fclose(log);
    return pumps < 0;
}
