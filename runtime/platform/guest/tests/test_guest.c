/* test_guest.c -- the guest-facing imports driven by a hand-written window
 * procedure and pump in translated-code style (functions taking sg_ctx that read their arguments
 * from the guest stack and call imports with SG_CALL), replaying the reference input script
 * (runtime/platform/macos/tests/input-script.txt). The WM_* log must equal the hand-written expected log, so the guest path (CreateWindowExW,
 * ShowWindow, UpdateWindow, PeekMessageW, TranslateMessage, DispatchMessageW, DefWindowProcW,
 * BeginPaint/EndPaint, GetKeyState, GetAsyncKeyState, ScreenToClient, PostQuitMessage, SetWindowPos)
 * behaves exactly as the platform layer did when called directly. Plus unit checks of the other
 * imports, the registration, the stop before the first window and the core event API. No game code. */
#include <pthread.h>
#include <stdlib.h>
#include <sys/wait.h>
#include <time.h>
#include <unistd.h>

#include "fixture.h"
#include "script_runner.h"
#include <math.h>
#include "sp_macos.h"

/* The AppKit functions the GUI smoke calls; weak stand-ins so the headless binary (no sp_window.m)
 * links too. The real ones come from libsgp.a in guest_gui. */
__attribute__((weak)) int sp_mac_is_fullscreen(sp_hwnd h) { (void)h; return -1; }
__attribute__((weak)) void sp_mac_set_letterbox(int on) { (void)on; }
__attribute__((weak)) void sp_mac_content_rect(sp_hwnd h, double out[4]) { (void)h; out[0] = out[1] = out[2] = out[3] = 0; }
__attribute__((weak)) void sp_mac_screen_info(int32_t *w, int32_t *h, sp_rect *work, int32_t *hz, int32_t *mm_w, int32_t *mm_h) {
    (void)work; (void)hz; (void)mm_w; (void)mm_h; *w = *h = 0;
}
/* The notch's safe area */
__attribute__((weak)) int sp_mac_screen_safe_area(double out[4]) { (void)out; return 0; }
__attribute__((weak)) int sg_host_display_mode(uint32_t *w, uint32_t *h, uint32_t *hz) { *w = *h = *hz = 0; return -1; }
__attribute__((weak)) int sp_mac_post_mouse(sp_hwnd h, int kind, int button, int32_t x, int32_t y, int clicks) {
    (void)h; (void)kind; (void)button; (void)x; (void)y; (void)clicks; return 0;
}

#define WNDPROC_VA FX_FN(1)
#define PUMP_VA FX_FN(2)
#define RA(fn, k) ((fn) + 0x10u + 5u * (k)) /* fall-through addresses inside the fake functions */

static FILE *g_log;
static unsigned g_ime_chars; /* WM_IME_CHAR seen by the guest window procedure */
static const char *g_compare = "compare_logs.sh";
static unsigned g_frames;
static uint32_t g_hwnd;

static int screen_lparam(uint32_t msg) {
    return msg == WM_NCHITTEST || msg == WM_MOUSEWHEEL || msg == WM_CONTEXTMENU || msg == WM_MOVE;
}
static int key_msg(uint32_t msg) {
    return msg == WM_KEYDOWN || msg == WM_KEYUP || msg == WM_SYSKEYDOWN || msg == WM_SYSKEYUP;
}

/* ---- the guest window procedure: LRESULT CALLBACK (HWND, UINT, WPARAM, LPARAM), ret 16 ---------- */
static void guest_wndproc(sg_ctx *SG_RESTRICT ctx, uint8_t *mem) {
    const uint32_t F = WNDPROC_VA;
    uint32_t h = SG_ARG(0), msg = SG_ARG(1), w = SG_ARG(2), l = SG_ARG(3), r = 0;
    int depth, i;
    char kind;
    sp_delivery_info(&depth, &kind); /* test instrumentation: nesting and D/S, as the expected log */
    if (g_log) {
        for (i = 0; i < depth; i++) fputs("  ", g_log);
        fprintf(g_log, "%c %s w=0x%08X l=", kind, sp_message_name(msg), w);
        if (screen_lparam(msg)) {
            uint32_t pt = (ctx->esp -= 8); /* POINT on the guest stack, ScreenToClient(h, &pt) */
            sg_st32(mem, pt, (uint32_t)(int32_t)(int16_t)(l & 0xffff));
            sg_st32(mem, pt + 4, (uint32_t)(int32_t)(int16_t)(l >> 16));
            SG_PUSH32(pt);
            SG_PUSH32(h);
            SG_CALL(sg_imp_user32_ScreenToClient, RA(F, 0));
            fprintf(g_log, "scr(%d,%d)\n", (int32_t)sg_ld32(mem, pt), (int32_t)sg_ld32(mem, pt + 4));
            ctx->esp += 8;
        } else {
            fprintf(g_log, "0x%08X\n", l);
        }
    }
    switch (msg) {
    case WM_PAINT: {
        uint32_t ps = (ctx->esp -= 64); /* PAINTSTRUCT local */
        SG_PUSH32(ps);
        SG_PUSH32(h);
        SG_CALL(sg_imp_user32_BeginPaint, RA(F, 1));
        g_frames++;
        SG_PUSH32(ps);
        SG_PUSH32(h);
        SG_CALL(sg_imp_user32_EndPaint, RA(F, 2));
        ctx->esp += 64;
        r = 0;
        break;
    }
    case WM_ERASEBKGND: r = 1; break;
    case 0x286: g_ime_chars++; goto def; /* WM_IME_CHAR: counted, then DefWindowProc as usual */
    case WM_SETCURSOR: r = 1; break;
    case WM_CLOSE:
        SG_PUSH32(0);
        SG_CALL(sg_imp_user32_PostQuitMessage, RA(F, 3));
        r = 0;
        break;
    default:
    def:
        SG_PUSH32(l);
        SG_PUSH32(w);
        SG_PUSH32(msg);
        SG_PUSH32(h);
        SG_CALL(sg_imp_user32_DefWindowProcW, RA(F, 4));
        r = ctx->eax;
        break;
    }
    if (key_msg(msg) && g_log) {
        uint32_t ks, as;
        SG_PUSH32(w);
        SG_CALL(sg_imp_user32_GetKeyState, RA(F, 5));
        ks = ctx->eax;
        SG_PUSH32(w);
        SG_CALL(sg_imp_user32_GetAsyncKeyState, RA(F, 6));
        as = ctx->eax;
        for (i = 0; i < depth + 1; i++) fputs("  ", g_log);
        fprintf(g_log, "= ks(0x%02X)=0x%04X async=%d\n", w, (uint16_t)ks, (as & 0x8000) != 0);
    }
    ctx->eax = r;
    SG_RET(16);
}

/* ---- the guest pump, shaped like 0x8482f0: BOOL __stdcall pump(HWND), ret 4 ---------------------- */
static void guest_pump(sg_ctx *SG_RESTRICT ctx, uint8_t *mem) {
    const uint32_t F = PUMP_VA;
    uint32_t h = SG_ARG(0), m = (ctx->esp -= 28); /* MSG local */
    SG_PUSH32(0);
    SG_PUSH32(0);
    SG_PUSH32(h);
    SG_CALL(sg_imp_user32_InvalidateRect, RA(F, 0));
    for (int guard = 0; guard < 10000; guard++) {
        SG_PUSH32(1); /* PM_REMOVE */
        SG_PUSH32(0);
        SG_PUSH32(0);
        SG_PUSH32(0);
        SG_PUSH32(m);
        SG_CALL(sg_imp_user32_PeekMessageW, RA(F, 1));
        if (!ctx->eax) break;
        if (sg_ld32(mem, m + 4) == WM_QUIT) {
            if (g_log) fprintf(g_log, "Q WM_QUIT w=0x%08X l=0x%08X\n", sg_ld32(mem, m + 8), sg_ld32(mem, m + 12));
            ctx->esp += 28;
            ctx->eax = 0;
            SG_RET(4);
        }
        SG_PUSH32(m);
        SG_CALL(sg_imp_user32_TranslateMessage, RA(F, 2));
        SG_PUSH32(m);
        SG_CALL(sg_imp_user32_DispatchMessageW, RA(F, 3));
    }
    ctx->esp += 28;
    ctx->eax = 1;
    SG_RET(4);
}

/* ---- script driver: host events go straight to the platform layer (as the AppKit drain does);
 * window operations go through the guest imports -------------------------------------------------- */
static int pump_hook(sp_hwnd h) {
    if (g_log) fputs("-- pump\n", g_log);
    return (int)GSTD(guest_pump, h);
}
static void d_key(void *c, int action, uint16_t kvk, const uint16_t *ch, int n, const uint16_t *ig, int ni, uint32_t mods) {
    sp_input_key(kvk, action, ch, n, ig, ni);
}
static void d_flags(void *c, uint16_t kvk, int on) {
    if (kvk == 0x39) sp_input_capslock(on);
    else sp_input_modifier(kvk, on);
}
static void d_mouse(void *c, int kind, int button, int32_t x, int32_t y, int clicks) {
    if (kind == SR_MOUSE_MOVE || kind == SR_MOUSE_DRAG) sp_input_mouse_move(g_hwnd, x, y);
    else sp_input_mouse_button(g_hwnd, button, kind == SR_MOUSE_DOWN, x, y, clicks);
}
static void d_wheel(void *c, int lines) { sp_input_wheel(g_hwnd, lines * WHEEL_DELTA); }
static void d_resize(void *c, int32_t w, int32_t h) { /* outer size: client + caption (frame model) */
    GSTD(sg_imp_user32_SetWindowPos, g_hwnd, 0, 0, 0, (uint32_t)w, (uint32_t)(h + sgp_caption_h), 0x2u | 0x4u);
}
static void d_activate(void *c, int on) { sp_window_activate(g_hwnd, on, 1); }
static void d_close(void *c) { sp_window_close_request(g_hwnd); }
static void d_show(void *c) {
    GSTD(sg_imp_user32_ShowWindow, g_hwnd, 5 /* SW_SHOW */);
    GSTD(sg_imp_user32_UpdateWindow, g_hwnd);
}

static uint32_t wstr(const char *s) {
    uint32_t n = (uint32_t)strlen(s) + 1, a = hle_guest_alloc(2 * n, 4), i;
    for (i = 0; i < n; i++) sg_st16(M, a + 2 * i, (uint8_t)s[i]);
    return a;
}

static uint32_t register_canvas(void) {
    uint32_t wc = hle_guest_alloc(48, 4);
    sg_st32(M, wc + 0, 48);
    sg_st32(M, wc + 4, SP_STYLE_DBLCLKS);
    sg_st32(M, wc + 8, WNDPROC_VA);
    sg_st32(M, wc + 28, GSTD(sg_imp_user32_LoadCursorA, 0, 32512));
    sg_st32(M, wc + 32, GSTD(sg_imp_gdi32_CreateSolidBrush, 0));
    sg_st32(M, wc + 40, wstr("Canvas"));
    return GSTD(sg_imp_user32_RegisterClassExW, wc);
}

static int run_script(const char *script, const char *expected) {
    char out[512], cmd[1200];
    uint32_t atom, rect = hle_guest_alloc(16, 4);
    int rc;
    snprintf(out, sizeof out, "guest-wm.log");
    g_log = fopen(out, "w");
    atom = register_canvas();
    CHECK(atom >= 0xc000, "RegisterClassExW atom 0x%x", atom);
    /* As the game at 0x849cfd, but positioned: client 640x480 at (100,200), WS_OVERLAPPEDWINDOW. */
    g_hwnd = GSTD(sg_imp_user32_CreateWindowExW, 0, atom, wstr("fixture"), 0x00CF0000u, 100, 200 - (uint32_t)sgp_caption_h,
                  640, 480 + (uint32_t)sgp_caption_h, 0, 0, 0x400000, 0);
    CHECK(g_hwnd != 0, "CreateWindowExW");
    GSTD(sg_imp_user32_SetWindowLongW, g_hwnd, (uint32_t)-21, 0x12345678u); /* GWL_USERDATA, as 0x849d18 */
    CHECK_EQ(GSTD(sg_imp_user32_GetWindowLongA, g_hwnd, (uint32_t)-21), 0x12345678u);
    CHECK_EQ(GSTD(sg_imp_user32_GetClientRect, g_hwnd, rect), 1);
    CHECK_EQ(sg_ld32(M, rect + 8), 640);
    CHECK_EQ(sg_ld32(M, rect + 12), 480);
    CHECK_EQ(GSTD(sg_imp_user32_IsWindowVisible, g_hwnd), 0);
    {
        sr_driver drv = {NULL, d_key, d_flags, d_mouse, d_wheel, d_resize, d_activate, d_close, d_show, NULL};
        int pumps;
        sr_set_log(g_log, &drv);
        sr_pump_hook = pump_hook;
        pumps = sr_run_script(script, g_hwnd);
        fclose(g_log);
        g_log = NULL;
        printf("guest replay: %d pump passes, %u WM_PAINT frames through BeginPaint/EndPaint\n", pumps, g_frames);
        CHECK(pumps == 19, "pump passes %d", pumps);
        CHECK(g_frames >= 19, "frames %u", g_frames);
    }
    snprintf(cmd, sizeof cmd, "bash '%s' '%s' '%s'", g_compare, expected, out);
    rc = system(cmd);
    CHECK(rc == 0, "guest WM_* log differs from %s", expected);
    return rc;
}

/* ---- unit checks ------------------------------------------------------------------------------ */
static void test_geometry(void) {
    uint32_t r = hle_guest_alloc(16, 4), h2;
    /* AdjustWindowRectEx then GetWindowRect agree (frame model: caption only) */
    sg_st32(M, r, 0); sg_st32(M, r + 4, 0); sg_st32(M, r + 8, 800); sg_st32(M, r + 12, 600);
    CHECK_EQ(GSTD(sg_imp_user32_AdjustWindowRectEx, r, 0x00CF0000u, 0, 0), 1);
    CHECK_EQ((int32_t)sg_ld32(M, r + 4), -sgp_caption_h);
    CHECK_EQ(GSTD(sg_imp_user32_AdjustWindowRectEx, r, 0x80080000u, 0, 0), 1); /* WS_POPUP: no caption */
    CHECK_EQ((int32_t)sg_ld32(M, r + 4), -sgp_caption_h);
    h2 = GSTD(sg_imp_user32_CreateWindowExW, 0, wstr("Canvas"), 0, 0x00CF0000u, 0x80000000u, 0x80000000u, 0x80000000u,
              0x80000000u, 0, 0, 0, 0);
    CHECK(h2 != 0, "CreateWindowExW by class name with CW_USEDEFAULT");
    GSTD(sg_imp_user32_GetClientRect, h2, r);
    CHECK_EQ(sg_ld32(M, r + 8), 800);
    CHECK_EQ(sg_ld32(M, r + 12), 600);
    CHECK_EQ(GSTD(sg_imp_user32_MoveWindow, h2, 10, 20, 300, 200 + (uint32_t)sgp_caption_h, 0), 1);
    GSTD(sg_imp_user32_GetWindowRect, h2, r);
    CHECK_EQ(sg_ld32(M, r), 10);
    CHECK_EQ(sg_ld32(M, r + 4), 20);
    CHECK_EQ(sg_ld32(M, r + 8), 310);
    CHECK_EQ(sg_ld32(M, r + 12), 220 + (uint32_t)sgp_caption_h);
    GSTD(sg_imp_user32_GetClientRect, h2, r);
    CHECK_EQ(sg_ld32(M, r + 8), 300);
    CHECK_EQ(sg_ld32(M, r + 12), 200);
    /* GWL_STYLE round trip; hidden windows get no WM_PAINT */
    CHECK_EQ(GSTD(sg_imp_user32_GetWindowLongA, h2, (uint32_t)-16), 0x00CF0000u);
    GSTD(sg_imp_user32_SetWindowLongA, h2, (uint32_t)-16, 0x80080000u);
    CHECK_EQ(GSTD(sg_imp_user32_GetWindowLongA, h2, (uint32_t)-16), 0x80080000u);
    CHECK(sp_window_invalid(h2) && !sp_window_visible(h2), "new hidden window is invalid but hidden");
    {
        uint32_t m = hle_guest_alloc(28, 4), got = 0;
        while (GSTD(sg_imp_user32_PeekMessageW, m, h2, 0, 0, 1)) got |= sg_ld32(M, m + 4) == WM_PAINT;
        CHECK(!got, "hidden window got WM_PAINT");
    }
    CHECK_EQ(GSTD(sg_imp_user32_DestroyWindow, h2), 1);
    CHECK_EQ(GSTD(sg_imp_user32_IsWindowVisible, h2), 0);
    /* metrics, work area, monitor */
    CHECK_EQ(GSTD(sg_imp_user32_GetSystemMetrics, 0), 1024);
    CHECK_EQ(GSTD(sg_imp_user32_GetSystemMetrics, 1), 768);
    CHECK_EQ(GSTD(sg_imp_user32_SystemParametersInfoA, 0x30, 0, r, 0), 1);
    CHECK_EQ(sg_ld32(M, r + 4), 25);
    {
        uint32_t mi = hle_guest_alloc(72, 4);
        sg_st32(M, mi, 72);
        CHECK_EQ(GSTD(sg_imp_user32_GetMonitorInfoA, GSTD(sg_imp_user32_MonitorFromWindow, g_hwnd, 2), mi), 1);
        CHECK_EQ(sg_ld32(M, mi + 12), 1024);
        CHECK(!strcmp((char *)M + mi + 40, "\\\\.\\DISPLAY1"), "szDevice");
        uint32_t dc = GSTD(sg_imp_gdi32_CreateDCA, mi + 40, mi + 40, 0, 0);
        CHECK(dc != 0, "CreateDCA");
        CHECK_EQ(GSTD(sg_imp_gdi32_GetDeviceCaps, dc, 4), 271);
        CHECK_EQ(GSTD(sg_imp_gdi32_GetDeviceCaps, dc, 6), 203);
        CHECK_EQ(GSTD(sg_imp_gdi32_ExtEscape, dc, 0x7306, 0x50, 0, 0x140, 0), 0);
        CHECK_EQ(GSTD(sg_imp_gdi32_DeleteDC, dc), 1);
    }
    /* IntersectRect */
    {
        uint32_t a = hle_guest_alloc(16, 4), b = hle_guest_alloc(16, 4);
        sg_st32(M, a, 0); sg_st32(M, a + 4, 0); sg_st32(M, a + 8, 100); sg_st32(M, a + 12, 100);
        sg_st32(M, b, 50); sg_st32(M, b + 4, 60); sg_st32(M, b + 8, 200); sg_st32(M, b + 12, 80);
        CHECK_EQ(GSTD(sg_imp_user32_IntersectRect, r, a, b), 1);
        CHECK_EQ(sg_ld32(M, r), 50); CHECK_EQ(sg_ld32(M, r + 4), 60); CHECK_EQ(sg_ld32(M, r + 8), 100); CHECK_EQ(sg_ld32(M, r + 12), 80);
    }
}

static int timer_seen;
static void timer_wndproc(sg_ctx *SG_RESTRICT ctx, uint8_t *mem) {
    if (SG_ARG(1) == WM_TIMER) timer_seen++;
    ctx->eax = 0;
    SG_RET(16);
}
static int tp_calls;
static void timer_proc(sg_ctx *SG_RESTRICT ctx, uint8_t *mem) { tp_calls++; SG_RET(16); }

static void test_queue_misc(void) {
    uint32_t m = hle_guest_alloc(28, 4), wc = hle_guest_alloc(40, 4), h;
    int i;
    fx_register(FX_FN(3), timer_wndproc);
    fx_register(FX_FN(4), timer_proc);
    /* RegisterClassA + CreateWindowExA: EAClipboard's pattern (never shown) */
    sg_st32(M, wc + 4, FX_FN(3));
    {
        uint32_t nm = hle_guest_alloc(32, 4);
        strcpy((char *)M + nm, "EAClipboardClass");
        sg_st32(M, wc + 36, nm);
        CHECK(GSTD(sg_imp_user32_RegisterClassA, wc) >= 0xc000, "RegisterClassA");
        CHECK_EQ(GSTD(sg_imp_user32_RegisterClassA, wc), 0); /* already exists */
        h = GSTD(sg_imp_user32_CreateWindowExA, 0, nm, 0, 0, 0, 0, 0, 0, 0, 0, 0, 0);
        CHECK(h != 0, "CreateWindowExA");
    }
    /* SetTimer -> WM_TIMER through DispatchMessageW; TIMERPROC called by DispatchMessageW */
    CHECK_EQ(GSTD(sg_imp_user32_SetTimer, h, 7, 10, 0), 7);
    CHECK(GSTD(sg_imp_user32_SetTimer, 0, 0, 10, FX_FN(4)) != 0, "SetTimer with TIMERPROC");
    usleep(30000);
    for (i = 0; i < 50 && GSTD(sg_imp_user32_PeekMessageW, m, 0, 0, 0, 1); i++) {
        GSTD(sg_imp_user32_TranslateMessage, m);
        GSTD(sg_imp_user32_DispatchMessageW, m);
    }
    CHECK(timer_seen >= 1, "WM_TIMER reached the guest window procedure (%d)", timer_seen);
    CHECK(tp_calls >= 1, "TIMERPROC called (%d)", tp_calls);
    CHECK_EQ(GSTD(sg_imp_user32_KillTimer, h, 7), 1);
    /* PostMessageA from another thread, SendMessageA on the window thread */
    timer_seen = 0;
    CHECK_EQ(GSTD(sg_imp_user32_PostMessageA, h, WM_TIMER, 9, 0), 1);
    CHECK_EQ(GSTD(sg_imp_user32_PeekMessageW, m, 0, 0, 0, 1), 1);
    CHECK_EQ(sg_ld32(M, m + 4), WM_TIMER);
    GSTD(sg_imp_user32_DispatchMessageW, m);
    CHECK_EQ(timer_seen, 1);
    GSTD(sg_imp_user32_SendMessageA, h, WM_TIMER, 1, 0);
    CHECK_EQ(timer_seen, 2);
    CHECK_EQ(GSTD(sg_imp_user32_DestroyWindow, h), 1);
    /* keyboard state, layout */
    CHECK_EQ(GSTD(sg_imp_user32_GetKeyboardLayout, 0), 0x04090409u);
    CHECK_EQ(GSTD(sg_imp_user32_GetKeyboardState, hle_guest_alloc(256, 4)), 1);
    /* clipboard (headless store) */
    {
        uint32_t s = wstr("hello");
        CHECK_EQ(GSTD(sg_imp_user32_OpenClipboard, 0), 1);
        CHECK_EQ(GSTD(sg_imp_user32_EmptyClipboard), 1);
        CHECK_EQ(GSTD(sg_imp_user32_SetClipboardData, 13, s), s);
        CHECK_EQ(GSTD(sg_imp_user32_CloseClipboard), 1);
        CHECK_EQ(GSTD(sg_imp_user32_IsClipboardFormatAvailable, 13), 1);
    }
    /* cursor and hooks */
    CHECK_EQ((int32_t)GSTD(sg_imp_user32_ShowCursor, 0), -1);
    CHECK_EQ((int32_t)GSTD(sg_imp_user32_ShowCursor, 1), 0);
    CHECK_EQ(GSTD(sg_imp_user32_SetCursor, 0x00e30000u + 32512), 0);
    CHECK(GSTD(sg_imp_user32_SetWindowsHookExA, 13, FX_FN(4), 0x400000, 0) != 0, "hook");
    CHECK_EQ(GSTD(sg_imp_user32_GetCursorPos, m), 1);
    /* IMM32: a context per window (test_imm); XInput: no pad; DirectInput8: no devices */
    CHECK_EQ(GSTD(sg_imp_imm32_ImmGetContext, g_hwnd), 0x494D0000u | (g_hwnd & 0xFFFF));
    CHECK_EQ(GSTD(sg_imp_imm32_ImmGetContext, 0), 0);
    CHECK_EQ(GSTD(sg_imp_xinput9_1_0_XInputGetState, 0, m), 1167);
    {
        uint32_t out = hle_guest_alloc(4, 4), o;
        CHECK_EQ(GSTD(sg_imp_dinput8_DirectInput8Create, 0x400000, 0x800, 0, out, 0), 0);
        o = sg_ld32(M, out);
        CHECK(o != 0, "IDirectInput8 object");
        /* EnumDevices (slot +0x10) through the object's vtable: guest call, finds nothing */
        uint32_t a[5] = {o, 4, FX_FN(4), 0, 1};
        CHECK_EQ(sg_call_guest(C, sg_ld32(M, sg_ld32(M, o) + 0x10), SG_CC_STDCALL, 5, a), 0);
        CHECK_EQ(tp_calls >= 1, 1);
    }
    /* user object flags (CRT interactive check) */
    {
        uint32_t f = hle_guest_alloc(12, 4), need = hle_guest_alloc(4, 4);
        CHECK_EQ(GSTD(sg_imp_user32_GetUserObjectInformationW, GSTD0(sg_imp_user32_GetProcessWindowStation), 1, f, 12, need), 1);
        CHECK_EQ(sg_ld32(M, f + 8), 1);
    }
}

/* Every USER32/GDI32/IMM32/DINPUT8 import of the generated table (and the XInput dynamics) is ours. */
static void test_registration(void) {
    unsigned bad = 99, n = sgp_register_shims(&bad), i, k, ours = 0, other = 0;
    CHECK_EQ(n, sgp_shim_count);
    CHECK_EQ(bad, 0);
    for (i = 0; i < hle_import_count; i++) {
        const char *d = hle_imports[i].dll;
        if (strcmp(d, "USER32.dll") && strcmp(d, "GDI32.dll") && strcmp(d, "IMM32.dll") && strcmp(d, "DINPUT8.dll")) continue;
        for (k = 0; k < sgp_shim_count && sgp_shims[k].fn != hle_imports[i].fn; k++) {}
        if (k < sgp_shim_count) ours++;
        else {
            other++;
            CHECK(!strncmp(hle_imports[i].name, "wsprintf", 8), "%s!%s is not implemented here", d, hle_imports[i].name);
        }
    }
    printf("registration: %u imports checked, %u problems; table USER32/GDI32/IMM32/DINPUT8: %u ours, %u elsewhere (wsprintfA/W in runtime/hle)\n",
           n, bad, ours, other);
    CHECK_EQ(ours, 106);
    CHECK_EQ(other, 2);
    CHECK_EQ(hle_resolve("xinput9_1_0.dll", "XInputGetState", 0) != 0, 1);
}

/* The stop before the first window survives: with SPORE_STOP_AT=window, CreateWindowExW stops (exit 0) before creating. */
static void test_boot_stop(void) {
    pid_t p;
    fflush(NULL);
    p = fork();
    int st = 0;
    if (p == 0) {
        setenv("SPORE_STOP_AT", "window", 1);
        freopen("/dev/null", "w", stderr);
        GSTD(sg_imp_user32_CreateWindowExW, 0, 0xc000, 0, 0, 0, 0, 10, 10, 0, 0, 0, 0);
        _exit(3);
    }
    waitpid(p, &st, 0);
    CHECK(WIFEXITED(st) && WEXITSTATUS(st) == 0, "SPORE_STOP_AT=window: CreateWindowExW should stop with exit 0 (status 0x%x)", st);
}

/* ---- the core event API (sg_core.h) ----------------------------------------------------------- */
static uint32_t ev_h;
static void *setter(void *arg) {
    usleep(20000);
    sg_core_event_set(ev_h); /* from a host thread that is not a guest thread (FSEvents / CoreAudio) */
    return arg;
}
static void *host_waiter(void *arg) {
    return (void *)(uintptr_t)sg_core_event_wait(ev_h, 5000);
}
static void test_events(void) {
    pthread_t t, t2;
    void *res;
    uint32_t h = sg_core_event_create(0, 0), m;
    CHECK(h != 0, "event create");
    CHECK_EQ(sg_handle_type(h), SG_H_EVENT);
    CHECK_EQ(sg_core_event_wait(h, 0), 0x102);         /* not signalled */
    CHECK_EQ(sg_core_event_set(h), 1);
    CHECK_EQ(sg_core_event_wait(h, 0), 0);             /* auto-reset: taken ... */
    CHECK_EQ(sg_core_event_wait(h, 0), 0x102);         /* ... and reset */
    ev_h = h;                                          /* guest thread waits, host thread sets */
    pthread_create(&t, NULL, setter, NULL);
    CHECK_EQ(sg_core_event_wait(h, 5000), 0);
    pthread_join(t, NULL);
    pthread_create(&t, NULL, host_waiter, NULL);       /* host thread waits, guest import sets */
    usleep(20000);
    CHECK_EQ(GSTD(sg_core_shim_lookup("sg_imp_kernel32_SetEvent"), h), 1);
    pthread_join(t, &res);
    CHECK_EQ((uintptr_t)res, 0);
    m = sg_core_event_create(1, 1);                    /* manual reset stays signalled */
    CHECK_EQ(sg_core_event_wait(m, 0), 0);
    CHECK_EQ(sg_core_event_wait(m, 0), 0);
    CHECK_EQ(sg_core_event_reset(m), 1);
    CHECK_EQ(sg_core_event_wait(m, 10), 0x102);
    ev_h = m;                                          /* two host waiters on a manual event */
    pthread_create(&t, NULL, host_waiter, NULL);
    pthread_create(&t2, NULL, host_waiter, NULL);
    usleep(20000);
    sg_core_event_set(m);
    pthread_join(t, &res);
    CHECK_EQ((uintptr_t)res, 0);
    pthread_join(t2, &res);
    CHECK_EQ((uintptr_t)res, 0);
    CHECK_EQ(GSTD(sg_core_shim_lookup("sg_imp_kernel32_WaitForSingleObject"), m, 0), 0); /* guest wait */
    CHECK_EQ(sg_handle_close(h), 1);
    CHECK_EQ(sg_handle_close(m), 1);
    CHECK_EQ(sg_core_event_set(h), 0);                 /* closed */
    CHECK_EQ(sg_core_event_wait(h, 0), 0xffffffffu);
}

/* ---- host close requests in timed runs (a timed test run is not ended by a stray close) ---------- */
int sgp_host_close_request(sp_hwnd h, const char *source); /* sgp_msg.c; called by sp_window.m */
static int close_seen;
static void close_wndproc(sg_ctx *SG_RESTRICT ctx, uint8_t *mem) {
    const uint32_t F = FX_FN(6);
    uint32_t h = SG_ARG(0), msg = SG_ARG(1), w = SG_ARG(2), l = SG_ARG(3);
    if (msg == WM_CLOSE) {
        close_seen++;
        ctx->eax = 0; /* the game decides; this one keeps the window */
    } else {
        SG_PUSH32(l); SG_PUSH32(w); SG_PUSH32(msg); SG_PUSH32(h);
        SG_CALL(sg_imp_user32_DefWindowProcW, RA(F, 0));
    }
    SG_RET(16);
}
static void close_pump(void) {
    uint32_t m = hle_guest_alloc(28, 4);
    for (int i = 0; i < 50 && GSTD(sg_imp_user32_PeekMessageW, m, 0, 0, 0, 1); i++) GSTD(sg_imp_user32_DispatchMessageW, m);
}
static void test_close_requests(void) {
    uint32_t wc = hle_guest_alloc(40, 4), nm = hle_guest_alloc(32, 4), h;
    char *saved = getenv("SPORE_RUN_SECONDS") ? strdup(getenv("SPORE_RUN_SECONDS")) : NULL;
    fx_register(FX_FN(6), close_wndproc);
    strcpy((char *)M + nm, "CloseTestClass");
    sg_st32(M, wc + 4, FX_FN(6));
    sg_st32(M, wc + 36, nm);
    CHECK(GSTD(sg_imp_user32_RegisterClassA, wc) >= 0xc000, "RegisterClassA");
    h = GSTD(sg_imp_user32_CreateWindowExA, 0, nm, 0, 0, 0, 0, 0, 0, 0, 0, 0, 0);
    CHECK(h != 0, "CreateWindowExA");
    /* a normal run honours the close button, Cmd-Q and Quit */
    unsetenv("SPORE_RUN_SECONDS");
    close_seen = 0;
    CHECK_EQ(sgp_host_close_request(h, "close button"), 1);
    close_pump();
    CHECK_EQ(close_seen, 1);
    CHECK_EQ(sgp_host_close_request(h, "Cmd-Q"), 1);
    close_pump();
    CHECK_EQ(close_seen, 2);
    CHECK_EQ(sgp_host_close_request(0, "Quit"), 1);          /* outside a timed run Quit is delivered */
    CHECK_EQ(close_seen, 2);
    /* a timed run logs and drops them */
    setenv("SPORE_RUN_SECONDS", "120", 1);
    CHECK_EQ(sgp_host_close_request(h, "close button"), 0);
    CHECK_EQ(sgp_host_close_request(h, "Cmd-Q"), 0);
    CHECK_EQ(sgp_host_close_request(0, "Quit"), 0);          /* applicationShouldTerminate: cancel */
    close_pump();
    CHECK_EQ(close_seen, 2);
    CHECK_EQ(GSTD(sg_imp_user32_IsWindowVisible, h), 0); /* still a window (hidden, never shown) */
    /* the game's own WM_CLOSE is not a host request: still delivered in a timed run */
    GSTD(sg_imp_user32_PostMessageA, h, WM_CLOSE, 0, 0);
    close_pump();
    CHECK_EQ(close_seen, 3);
    setenv("SPORE_RUN_SECONDS", "0", 1);                     /* not a timed run */
    CHECK_EQ(sgp_host_close_request(h, "Cmd-Q"), 1);
    close_pump();
    CHECK_EQ(close_seen, 4);
    if (saved) { setenv("SPORE_RUN_SECONDS", saved, 1); free(saved); } else unsetenv("SPORE_RUN_SECONDS");
    CHECK_EQ(GSTD(sg_imp_user32_DestroyWindow, h), 1);
    close_pump();
}

/* ---- AppKit smoke (make check-gui): the real back end, a real NSWindow for about a second ------- */
static int gui_paints;
static uint32_t gui_sizes, gui_last_size, gui_moves_seen[8], gui_nmoves;
static void gui_wndproc(sg_ctx *SG_RESTRICT ctx, uint8_t *mem) {
    const uint32_t F = FX_FN(5);
    uint32_t h = SG_ARG(0), msg = SG_ARG(1), w = SG_ARG(2), l = SG_ARG(3);
    if (msg == WM_SIZE) gui_sizes++, gui_last_size = l;
    if (msg == WM_MOUSEMOVE && gui_nmoves < 8) gui_moves_seen[gui_nmoves++] = l;
    if (msg == WM_PAINT) {
        uint32_t ps = (ctx->esp -= 64);
        SG_PUSH32(ps); SG_PUSH32(h);
        SG_CALL(sg_imp_user32_BeginPaint, RA(F, 0));
        gui_paints++;
        SG_PUSH32(ps); SG_PUSH32(h);
        SG_CALL(sg_imp_user32_EndPaint, RA(F, 1));
        ctx->esp += 64;
        ctx->eax = 0;
    } else {
        SG_PUSH32(l); SG_PUSH32(w); SG_PUSH32(msg); SG_PUSH32(h);
        SG_CALL(sg_imp_user32_DefWindowProcW, RA(F, 2));
    }
    SG_RET(16);
}

static void gui_pump(uint32_t m, int passes) {
    for (int i = 0; i < passes; i++) {
        while (GSTD(sg_imp_user32_PeekMessageW, m, 0, 0, 0, 1)) {
            GSTD(sg_imp_user32_TranslateMessage, m);
            GSTD(sg_imp_user32_DispatchMessageW, m);
        }
        usleep(16000);
    }
}

/* The game's fullscreen path (0x8490c0) and back, through the imports, on the real back end. The
 * window covers the display for about half a second. */
static void gui_fullscreen(uint32_t h, uint32_t r, uint32_t m) {
    int32_t sw = 0, sh = 0;
    double cr[4];
    sp_mac_screen_info(&sw, &sh, NULL, NULL, NULL, NULL);
    sp_mac_set_letterbox(1);
    gui_sizes = 0;
    /* SetWindowLong(GWL_STYLE, WS_POPUP|WS_SYSMENU), SetWindowPos(HWND_TOPMOST, 0,0, w,h, FRAMECHANGED),
     * ShowWindow(SW_MAXIMIZE), ShowWindow(SW_SHOWNORMAL); 1280x960 is 4:3, so a 16:10 or 16:9 display
     * letterboxes it at the sides */
    GSTD(sg_imp_user32_SetWindowLongW, h, (uint32_t)-16, 0x80080000u);
    CHECK_EQ(GSTD(sg_imp_user32_SetWindowPos, h, 0xffffffffu, 0, 0, 1280, 960, 0x20), 1);
    GSTD(sg_imp_user32_ShowWindow, h, 3);
    GSTD(sg_imp_user32_ShowWindow, h, 1);
    gui_pump(m, 15);
    CHECK_EQ(sp_mac_is_fullscreen(h), 1);
    GSTD(sg_imp_user32_GetClientRect, h, r);
    CHECK_EQ(sg_ld32(M, r + 8), 1280);                     /* the game's resolution, not the display's */
    CHECK_EQ(sg_ld32(M, r + 12), 960);
    CHECK(gui_sizes >= 1 && gui_last_size == ((960u << 16) | 1280u), "WM_SIZE 1280x960 (%u sizes, last 0x%x)", gui_sizes,
          gui_last_size);
    GSTD(sg_imp_user32_GetWindowRect, h, r);
    CHECK_EQ(sg_ld32(M, r), 0);                            /* a popup at the display's origin, no caption */
    CHECK_EQ(sg_ld32(M, r + 4), 0);
    sp_mac_content_rect(h, cr);
    printf("fullscreen: display %dx%d pt, client 1280x960 shown at %.1f,%.1f %.1fx%.1f pt\n", sw, sh, cr[0], cr[1], cr[2], cr[3]);
    CHECK(fabs(cr[2] / cr[3] - 1280.0 / 960.0) < 0.01, "letterbox keeps the aspect (%.4f)", cr[2] / cr[3]);
    CHECK(fabs(cr[2] - sw) < 1.0 || fabs(cr[3] - sh) < 1.0, "fills one dimension of the display");
    CHECK(fabs(cr[0] * 2 + cr[2] - sw) < 1.0 && fabs(cr[1] * 2 + cr[3] - sh) < 1.0, "centred");
    /* a mouse point in the client maps through the letterbox: client (640,480) and (10,950) */
    gui_nmoves = 0;
    sp_mac_post_mouse(h, 0, 0, 640, 480, 0);
    gui_pump(m, 3);                                        /* (consecutive moves coalesce) */
    sp_mac_post_mouse(h, 0, 0, 10, 950, 0);
    gui_pump(m, 3);
    {
        int a = 0, b = 0;
        for (uint32_t i = 0; i < gui_nmoves; i++) {
            if (gui_moves_seen[i] == ((480u << 16) | 640u)) a = 1;
            if (gui_moves_seen[i] == ((950u << 16) | 10u)) b = 1;
        }
        CHECK(a && b, "WM_MOUSEMOVE at the client points through the letterbox (%u moves)", gui_nmoves);
    }
    /* GetCursorPos/ScreenToClient of the scripted point give the client point back (read before the next
     * pump, since a real mouse event over the window hands the pointer back to the user) */
    sp_mac_post_mouse(h, 0, 0, 10, 950, 0);
    GSTD(sg_imp_user32_GetCursorPos, r);
    GSTD(sg_imp_user32_ScreenToClient, h, r);
    CHECK(sg_ld32(M, r) == 10 && sg_ld32(M, r + 4) == 950, "GetCursorPos + ScreenToClient = (10,950), got (%d,%d)",
          (int)sg_ld32(M, r), (int)sg_ld32(M, r + 4));
    gui_pump(m, 1);
    /* the game's way back: the windowed style, SetWindowPos(FRAMECHANGED) with the window size */
    GSTD(sg_imp_user32_SetWindowLongW, h, (uint32_t)-16, 0x00cb0000u);
    CHECK_EQ(GSTD(sg_imp_user32_SetWindowPos, h, 0xfffffffeu, 100, 100, 800, 600 + (uint32_t)sgp_caption_h, 0x20), 1);
    gui_pump(m, 10);
    CHECK_EQ(sp_mac_is_fullscreen(h), 0);
    GSTD(sg_imp_user32_GetClientRect, h, r);
    CHECK_EQ(sg_ld32(M, r + 8), 800);
    CHECK_EQ(sg_ld32(M, r + 12), 600);
    sp_mac_content_rect(h, cr);
    CHECK(cr[0] == 0 && cr[1] == 0 && cr[2] == 800 && cr[3] == 600, "windowed: the whole view (%.0fx%.0f)", cr[2], cr[3]);
}

/* SPORE_FULLSCREEN_SAFE_AREA=38 (a MacBook notch's height, simulated on any display): the screen the imports report
 * and the desktop mode the D3D9 adapter reports lose the strip; the game's fullscreen at the safe area's
 * size fills the area below it exactly, and the mouse maps there. Off again: the whole display. */
int sg_host_display_mode(uint32_t *w, uint32_t *h, uint32_t *hz);
static void gui_safe_area(uint32_t h, uint32_t r, uint32_t m) {
    int32_t sw0, sh0, sw, sh;
    uint32_t dw, dh, dhz;
    double cr[4];
    unsetenv("SPORE_FULLSCREEN_SAFE_AREA");
    unsetenv("SPORE_DISPLAY_MODE");
    sp_mac_screen_info(&sw0, &sh0, NULL, NULL, NULL, NULL);
    sg_host_display_mode(&dw, &dh, &dhz);
    /* DEVIATIONS 3.47: the desktop mode is the main display's size in points at 60 Hz (was 1920x1080) */
    CHECK(dw == (uint32_t)sw0 && dh == (uint32_t)sh0 && dhz == 60, "switch off, points: desktop = the display %dx%d@60 (%ux%u@%u)",
          sw0, sh0, dw, dh, dhz);
    printf("desktop mode, points: %ux%u@%u (display %dx%d)\n", dw, dh, dhz, sw0, sh0);
    setenv("SPORE_DISPLAY_MODE", "1920x1080@60", 1);       /* the fixed 1920x1080@60 wins */
    sg_host_display_mode(&dw, &dh, &dhz);
    CHECK(dw == 1920 && dh == 1080 && dhz == 60, "SPORE_DISPLAY_MODE=1920x1080@60 (%ux%u@%u)", dw, dh, dhz);
    unsetenv("SPORE_DISPLAY_MODE");
    setenv("SPORE_HEADLESS", "1", 1);                      /* no window: the fixed 1920x1080@60 */
    sg_host_display_mode(&dw, &dh, &dhz);
    CHECK(dw == 1920 && dh == 1080 && dhz == 60, "SPORE_HEADLESS=1: desktop 1920x1080@60 (%ux%u@%u)", dw, dh, dhz);
    unsetenv("SPORE_HEADLESS");
    setenv("SPORE_FULLSCREEN_SAFE_AREA", "38", 1);
    sp_mac_screen_info(&sw, &sh, NULL, NULL, NULL, NULL);
    CHECK(sw == sw0 && sh == sh0 - 38, "screen %dx%d = display %dx%d less 38", sw, sh, sw0, sh0);
    CHECK_EQ(GSTD(sg_imp_user32_GetSystemMetrics, 1), (uint32_t)sh);
    sg_host_display_mode(&dw, &dh, &dhz);
    CHECK(dw == (uint32_t)sw && dh == (uint32_t)sh && dhz == 60, "desktop mode = the safe area (%ux%u@%u)", dw, dh, dhz);
    printf("safe area 38: screen %dx%d (display %dx%d), desktop mode %ux%u@%u\n", sw, sh, sw0, sh0, dw, dh, dhz);
    GSTD(sg_imp_user32_SetWindowLongW, h, (uint32_t)-16, 0x80080000u);
    CHECK_EQ(GSTD(sg_imp_user32_SetWindowPos, h, 0xffffffffu, 0, 0, (uint32_t)sw, (uint32_t)sh, 0x20), 1);
    GSTD(sg_imp_user32_ShowWindow, h, 3);
    GSTD(sg_imp_user32_ShowWindow, h, 1);
    gui_pump(m, 15);
    CHECK_EQ(sp_mac_is_fullscreen(h), 1);
    GSTD(sg_imp_user32_GetClientRect, h, r);
    CHECK(sg_ld32(M, r + 8) == (uint32_t)sw && sg_ld32(M, r + 12) == (uint32_t)sh, "client = the safe area's size");
    GSTD(sg_imp_user32_GetWindowRect, h, r);
    CHECK(sg_ld32(M, r) == 0 && sg_ld32(M, r + 4) == 0, "window at the screen origin");
    sp_mac_content_rect(h, cr);
    printf("safe area 38: client %dx%d shown at %.1f,%.1f %.1fx%.1f pt\n", sw, sh, cr[0], cr[1], cr[2], cr[3]);
    CHECK(fabs(cr[0]) < 0.01 && fabs(cr[1] - 38) < 0.01 && fabs(cr[2] - sw) < 0.01 && fabs(cr[3] - sh) < 0.01,
          "content = the area below the strip (%.1f,%.1f %.1fx%.1f)", cr[0], cr[1], cr[2], cr[3]);
    sp_mac_post_mouse(h, 0, 0, sw - 1, sh - 1, 0);
    GSTD(sg_imp_user32_GetCursorPos, r);
    GSTD(sg_imp_user32_ScreenToClient, h, r);
    CHECK(sg_ld32(M, r) == (uint32_t)(sw - 1) && sg_ld32(M, r + 4) == (uint32_t)(sh - 1), "bottom-right corner maps back (%d,%d)",
          (int)sg_ld32(M, r), (int)sg_ld32(M, r + 4));
    sp_mac_post_mouse(h, 0, 0, 0, 0, 0);
    GSTD(sg_imp_user32_GetCursorPos, r);
    GSTD(sg_imp_user32_ScreenToClient, h, r);
    CHECK(sg_ld32(M, r) == 0 && sg_ld32(M, r + 4) == 0, "top-left corner maps back (%d,%d)", (int)sg_ld32(M, r), (int)sg_ld32(M, r + 4));
    gui_pump(m, 1);
    unsetenv("SPORE_FULLSCREEN_SAFE_AREA");
    sp_mac_screen_info(&sw, &sh, NULL, NULL, NULL, NULL);
    CHECK(sw == sw0 && sh == sh0, "off again: the whole display");
    GSTD(sg_imp_user32_SetWindowLongW, h, (uint32_t)-16, 0x00cb0000u);
    CHECK_EQ(GSTD(sg_imp_user32_SetWindowPos, h, 0xfffffffeu, 100, 100, 800, 600 + (uint32_t)sgp_caption_h, 0x20), 1);
    gui_pump(m, 10);
    CHECK_EQ(sp_mac_is_fullscreen(h), 0);
}

static int gui_smoke(void) {
    uint32_t wc = hle_guest_alloc(48, 4), r = hle_guest_alloc(16, 4), m = hle_guest_alloc(28, 4), atom, h;
    double t0;
    struct timespec ts;
    int passes = 0;
    unsetenv("SPORE_D3D9");                                /* the default presenter, fullscreen allowed */
    unsetenv("SPORE_FULLSCREEN");
    unsetenv("SPORE_RETINA");
    unsetenv("SPORE_FULLSCREEN_SAFE_AREA");                /* off (the default) */
    fx_register(FX_FN(5), gui_wndproc);
    sg_st32(M, wc, 48);
    sg_st32(M, wc + 4, SP_STYLE_DBLCLKS);
    sg_st32(M, wc + 8, FX_FN(5));
    sg_st32(M, wc + 40, wstr("CanvasGui"));
    atom = GSTD(sg_imp_user32_RegisterClassExW, wc);
    /* the game's sequence: hidden CW_USEDEFAULT window, then a centred MoveWindow, then show */
    h = GSTD(sg_imp_user32_CreateWindowExW, 0, atom, 0, 0x00CF0000u, 0x80000000u, 0x80000000u, 0x80000000u, 0x80000000u, 0, 0, 0, 0);
    CHECK(h != 0, "CreateWindowExW (AppKit)");
    printf("AppKit back end: %s, caption %d pt\n", sgp_backend_in_use()->name, sgp_caption_h);
    GSTD(sg_imp_user32_SetWindowTextW, h, wstr("guest window smoke test"));
    GSTD(sg_imp_user32_ShowWindow, h, 0);
    GSTD(sg_imp_user32_UpdateWindow, h);
    GSTD(sg_imp_user32_MoveWindow, h, 200, 150, 640, 480 + (uint32_t)sgp_caption_h, 1);
    GSTD(sg_imp_user32_ShowWindow, h, 5);
    CHECK(sgp_metal_layer(h) != NULL, "CAMetalLayer for the shown window");
    clock_gettime(CLOCK_MONOTONIC, &ts);
    t0 = ts.tv_sec + ts.tv_nsec / 1e9;
    for (;;) { /* the game's pump, ~60 passes/s, for one second */
        GSTD(sg_imp_user32_InvalidateRect, h, 0, 0);
        while (GSTD(sg_imp_user32_PeekMessageW, m, 0, 0, 0, 1)) {
            GSTD(sg_imp_user32_TranslateMessage, m);
            GSTD(sg_imp_user32_DispatchMessageW, m);
        }
        passes++;
        usleep(16000);
        clock_gettime(CLOCK_MONOTONIC, &ts);
        if (ts.tv_sec + ts.tv_nsec / 1e9 - t0 > 1.0) break;
    }
    GSTD(sg_imp_user32_GetClientRect, h, r);
    printf("AppKit smoke: %d pump passes, %d WM_PAINT in 1 s, client %ux%u, visible %u\n", passes, gui_paints,
           sg_ld32(M, r + 8), sg_ld32(M, r + 12), GSTD(sg_imp_user32_IsWindowVisible, h));
    CHECK(gui_paints >= passes - 2, "WM_PAINT every pass (%d of %d)", gui_paints, passes);
    CHECK_EQ(sg_ld32(M, r + 8), 640);
    CHECK_EQ(sg_ld32(M, r + 12), 480);
    CHECK_EQ(GSTD(sg_imp_user32_IsWindowVisible, h), 1);
    gui_fullscreen(h, r, m);
    gui_safe_area(h, r, m);
    CHECK_EQ(GSTD(sg_imp_user32_DestroyWindow, h), 1);
    while (GSTD(sg_imp_user32_PeekMessageW, m, 0, 0, 0, 1)) GSTD(sg_imp_user32_DispatchMessageW, m);
    printf("guest platform GUI smoke: %d passed, %d failed\n", t_pass, t_fail);
    return t_fail ? 1 : 0;
}

/* SetCursorPos makes a WM_MOUSEMOVE at the new position, as Windows does, for the
 * active window when the point is inside its client area, or for the capture window anywhere. */
static void test_set_cursor_pos(void) {
    uint32_t m = hle_guest_alloc(28, 4), r = hle_guest_alloc(16, 4), h, got = 0, last_l = 0;
    int i;
    h = GSTD(sg_imp_user32_CreateWindowExW, 0, wstr("Canvas"), 0, 0x10CF0000u, 40, 50, 400, 300 + (uint32_t)sgp_caption_h,
             0, 0, 0, 0);
    CHECK(h != 0, "visible window");
    GSTD(sg_imp_user32_ShowWindow, h, 5);
    GSTD(sg_imp_user32_SetActiveWindow, h);
    CHECK_EQ(GSTD(sg_imp_user32_GetActiveWindow), h);
    while (GSTD(sg_imp_user32_PeekMessageW, m, 0, 0, 0, 1)) GSTD(sg_imp_user32_DispatchMessageW, m);
    sg_st32(M, r, 30); sg_st32(M, r + 4, 40);
    GSTD(sg_imp_user32_ClientToScreen, h, r);
    CHECK_EQ(GSTD(sg_imp_user32_SetCursorPos, sg_ld32(M, r), sg_ld32(M, r + 4)), 1);
    for (i = 0; i < 20 && GSTD(sg_imp_user32_PeekMessageW, m, 0, 0, 0, 1); i++)
        if (sg_ld32(M, m + 4) == 0x200 && sg_ld32(M, m) == h) { got++; last_l = sg_ld32(M, m + 12); }
    CHECK_EQ(got, 1);
    CHECK_EQ(last_l, (40u << 16) | 30u);
    /* the same point again: no second message */
    GSTD(sg_imp_user32_SetCursorPos, sg_ld32(M, r), sg_ld32(M, r + 4));
    for (got = 0, i = 0; i < 20 && GSTD(sg_imp_user32_PeekMessageW, m, 0, 0, 0, 1); i++)
        if (sg_ld32(M, m + 4) == 0x200) got++;
    CHECK_EQ(got, 0);
    /* outside the client area: none without capture, one with it */
    GSTD(sg_imp_user32_SetCursorPos, 2000, 2000);
    for (got = 0, i = 0; i < 20 && GSTD(sg_imp_user32_PeekMessageW, m, 0, 0, 0, 1); i++)
        if (sg_ld32(M, m + 4) == 0x200) got++;
    CHECK_EQ(got, 0);
    GSTD(sg_imp_user32_SetCapture, h);
    GSTD(sg_imp_user32_SetCursorPos, 2001, 2001);
    for (got = 0, i = 0; i < 20 && GSTD(sg_imp_user32_PeekMessageW, m, 0, 0, 0, 1); i++)
        if (sg_ld32(M, m + 4) == 0x200) got++;
    CHECK_EQ(got, 1);
    GSTD(sg_imp_user32_ReleaseCapture);
    CHECK_EQ(GSTD(sg_imp_user32_DestroyWindow, h), 1);
}

/* ---- text input and IMM32 ------------------------------------------------------------------------
 * The view's NSTextInputClient calls are made directly (sp_ti_*), as interpretKeyEvents: would make them;
 * the messages are read back through PeekMessageW/TranslateMessage and the strings through the imports. */
typedef struct { uint32_t msg, w, l; } ti_rec;
static int ti_pump(ti_rec *out, int max) {
    uint32_t m = hle_guest_alloc(28, 4);
    int n = 0;
    while (n < max && GSTD(sg_imp_user32_PeekMessageW, m, 0, 0, 0, 1)) {
        uint32_t msg = sg_ld32(M, m + 4);
        if (msg == WM_PAINT || msg == WM_TIMER) { GSTD(sg_imp_user32_DispatchMessageW, m); continue; }
        out[n].msg = msg, out[n].w = sg_ld32(M, m + 8), out[n].l = sg_ld32(M, m + 12);
        n++;
        GSTD(sg_imp_user32_TranslateMessage, m);
    }
    return n;
}
static int ti_has(const ti_rec *r, int n, uint32_t msg, uint32_t w, uint32_t l) {
    for (int i = 0; i < n; i++) if (r[i].msg == msg && r[i].w == w && r[i].l == l) return 1;
    return 0;
}
static int ti_count(const ti_rec *r, int n, uint32_t msg) {
    int c = 0;
    for (int i = 0; i < n; i++) c += r[i].msg == msg;
    return c;
}
static void ti_key(uint16_t kvk, int marked, const uint16_t *s, int sn, const uint16_t *chars, int cn, int *emitted) {
    sp_ti_key_begin();
    if (marked) sp_ti_set_marked(s, sn, sn, 0); else if (s) sp_ti_insert(s, sn);
    *emitted = sp_ti_key_end(kvk, SP_KEY_DOWN, chars, cn);
    if (!*emitted) sp_input_key(kvk, SP_KEY_DOWN, chars, cn, chars, cn);   /* the caller's path: translate_key */
    sp_input_key(kvk, SP_KEY_UP, chars, cn, chars, cn);
}
static void test_imm(void) {
    ti_rec r[64];
    int n, em;
    uint32_t himc = GSTD(sg_imp_imm32_ImmGetContext, g_hwnd), buf = hle_guest_alloc(64, 4);
    const uint32_t START = 0x10D, END = 0x10E, COMP = 0x10F, CHAR = WM_CHAR, PROC = 0xE5;
    const uint32_t LCOMP = 0x1B8, LRES = 0x1800;
    int checks0 = t_pass + t_fail;
    sp_set_focus(g_hwnd);
    ti_pump(r, 64);
    sp_ti_reset();
    CHECK(himc != 0, "ImmGetContext gives a context");
    CHECK_EQ(GSTD(sg_imp_imm32_ImmGetContext, g_hwnd), himc);
    CHECK_EQ(GSTD(sg_imp_imm32_ImmReleaseContext, g_hwnd, himc), 1);
    /* a keyboard layout: the IME is "closed", as Windows reports for a plain layout */
    sp_ti_force_kind(SP_TI_LAYOUT);
    CHECK_EQ(GSTD(sg_imp_imm32_ImmGetOpenStatus, himc), 0);
    {   /* plain typing: the inserted text is NSEvent.characters, so today's path (WM_KEYDOWN + WM_CHAR 'a') */
        const uint16_t a[1] = {'a'};
        ti_key(0x00, 0, a, 1, a, 1, &em);
        n = ti_pump(r, 64);
        CHECK(!em, "plain 'a' keeps today's path");
        CHECK(n == 3 && r[0].msg == WM_KEYDOWN && r[0].w == 'A' && r[1].msg == CHAR && r[1].w == 'a' && r[2].msg == WM_KEYUP,
              "plain 'a': KEYDOWN CHAR KEYUP (%d messages)", n);
    }
    {   /* dead key: Option-e marks U+00B4, e commits U+00E9 */
        const uint16_t acute[1] = {0xB4}, e_[1] = {'e'}, eacute[1] = {0xE9};
        sp_input_modifier(0x3A, 1);
        ti_key(0x0E, 1, acute, 1, NULL, 0, &em);
        sp_input_modifier(0x3A, 0);
        CHECK(em, "Option-e: the core emits the dead key's key-down");
        CHECK_EQ(GSTD(sg_imp_imm32_ImmGetCompositionStringW, himc, 8, 0, 0), 0);   /* not an IME composition */
        n = ti_pump(r, 64);
        CHECK(ti_has(r, n, WM_SYSKEYDOWN, 'E', r[1].l) && !ti_count(r, n, WM_SYSCHAR) && !ti_count(r, n, CHAR),
              "Option-e: WM_SYSKEYDOWN 'E' and no character (%d messages)", n);
        CHECK(!ti_count(r, n, START) && !ti_count(r, n, COMP), "a dead key sends no WM_IME_*");
        ti_key(0x0E, 0, eacute, 1, e_, 1, &em);
        n = ti_pump(r, 64);
        CHECK(em && n == 3 && r[0].msg == WM_KEYDOWN && r[0].w == 'E' && r[1].msg == CHAR && r[1].w == 0xE9 &&
              r[1].l == r[0].l && r[2].msg == WM_KEYUP, "then e: WM_KEYDOWN 'E', WM_CHAR 0xE9, WM_KEYUP (%d)", n);
    }
    {   /* a character outside the BMP: a surrogate pair is two WM_CHARs, as Windows */
        const uint16_t grin[2] = {0xD83D, 0xDE00}, a[1] = {'a'};
        ti_key(0x00, 0, grin, 2, a, 1, &em);
        n = ti_pump(r, 64);
        CHECK(em && n == 4 && r[1].msg == CHAR && r[1].w == 0xD83D && r[2].msg == CHAR && r[2].w == 0xDE00,
              "U+1F600: WM_CHAR 0xD83D then 0xDE00 (%d)", n);
    }
    /* an input method: Japanese "ka" -> か, committed with Return */
    sp_ti_force_kind(SP_TI_IM);
    CHECK_EQ(GSTD(sg_imp_imm32_ImmGetOpenStatus, himc), 1);
    {
        uint32_t conv = hle_guest_alloc(8, 4);
        CHECK_EQ(GSTD(sg_imp_imm32_ImmGetConversionStatus, himc, conv, conv + 4), 1);
        CHECK_EQ(sg_ld32(M, conv), 1);   /* IME_CMODE_NATIVE */
    }
    {
        const uint16_t k[1] = {'k'}, a[1] = {'a'}, ka[1] = {0x304B}, ret[1] = {0x0D};
        CHECK(sp_ti_wants_key(0x28, 0), "an open input method takes letter keys");
        CHECK(!sp_ti_wants_key(0x24, 0) && !sp_ti_wants_key(0x7B, 0) && !sp_ti_wants_key(0x35, 0),
              "not composing: Return, arrows and Esc stay the game's");
        CHECK(!sp_ti_wants_key(0x28, 1), "Cmd/Ctrl chords stay the game's");
        ti_key(0x28, 1, k, 1, k, 1, &em);
        n = ti_pump(r, 64);
        CHECK(em && n >= 4 && r[0].msg == WM_KEYDOWN && r[0].w == PROC && r[1].msg == START && r[2].msg == COMP &&
              r[2].w == 'k' && r[2].l == LCOMP && !ti_count(r, n, CHAR), "k: VK_PROCESSKEY, STARTCOMPOSITION, COMPOSITION(GCS_COMPSTR...) (%d)", n);
        CHECK_EQ(GSTD(sg_imp_imm32_ImmGetCompositionStringW, himc, 8, 0, 0), 2);
        ti_key(0x00, 1, ka, 1, a, 1, &em);
        n = ti_pump(r, 64);
        CHECK(em && ti_count(r, n, START) == 0 && ti_has(r, n, COMP, 0x304B, LCOMP), "a: COMPOSITION only (%d)", n);
        CHECK_EQ(GSTD(sg_imp_imm32_ImmGetCompositionStringW, himc, 8, 0, 0), 2);
        CHECK_EQ(GSTD(sg_imp_imm32_ImmGetCompositionStringW, himc, 8, buf, 64), 2);
        CHECK_EQ(sg_ld16(M, buf), 0x304B);
        CHECK_EQ(GSTD(sg_imp_imm32_ImmGetCompositionStringW, himc, 0x10, buf, 64), 1);   /* GCS_COMPATTR */
        CHECK_EQ(M[buf], 0);                                                             /* ATTR_INPUT */
        CHECK_EQ(GSTD(sg_imp_imm32_ImmGetCompositionStringW, himc, 0x20, buf, 64), 8);   /* GCS_COMPCLAUSE */
        CHECK(sg_ld32(M, buf) == 0 && sg_ld32(M, buf + 4) == 1, "one clause");
        CHECK_EQ(GSTD(sg_imp_imm32_ImmGetCompositionStringW, himc, 0x80, 0, 0), 1);     /* GCS_CURSORPOS */
        CHECK(sp_ti_wants_key(0x24, 0), "composing: Return goes to the input method");
        sp_ti_key_begin();
        sp_ti_insert(ka, 1);
        em = sp_ti_key_end(0x24, SP_KEY_DOWN, ret, 1);
        n = ti_pump(r, 64);
        CHECK(em && n == 3 && r[0].msg == WM_KEYDOWN && r[0].w == PROC && r[1].msg == COMP && r[1].w == 0x304B &&
              r[1].l == LRES && r[2].msg == END, "Return: VK_PROCESSKEY, COMPOSITION(GCS_RESULTSTR), ENDCOMPOSITION (%d)", n);
        CHECK_EQ(GSTD(sg_imp_imm32_ImmGetCompositionStringW, himc, 0x800, 0, 0), 2);
        CHECK_EQ(GSTD(sg_imp_imm32_ImmGetCompositionStringW, himc, 0x800, buf, 64), 2);
        CHECK_EQ(sg_ld16(M, buf), 0x304B);
        CHECK_EQ(GSTD(sg_imp_imm32_ImmGetCompositionStringW, himc, 8, 0, 0), 0);
        CHECK_EQ((int32_t)GSTD(sg_imp_imm32_ImmGetCompositionStringW, himc, 0x4000, buf, 64), -2);
        CHECK_EQ((int32_t)GSTD(sg_imp_imm32_ImmGetCompositionStringW, 0x1234, 8, buf, 64), -2);
        sp_input_key(0x24, SP_KEY_UP, ret, 1, ret, 1);
        ti_pump(r, 64);
    }
    {   /* DefWindowProc: an unhandled result becomes WM_IME_CHAR, an unhandled WM_IME_CHAR a WM_CHAR */
        int32_t res;
        g_ime_chars = 0;
        CHECK(sp_ti_def_window_proc(g_hwnd, COMP, 0x304B, (int32_t)LRES, &res), "DefWindowProc handles the result");
        CHECK_EQ(g_ime_chars, 1);
        n = ti_pump(r, 64);
        int patched = ti_has(r, n, CHAR, 0x304B, 1);
        CHECK(sp_ti_def_window_proc(g_hwnd, 0x286, 0x304B, 1, &res), "WM_IME_CHAR");
        n = ti_pump(r, 64);
        CHECK(ti_has(r, n, CHAR, 0x304B, 1), "WM_IME_CHAR -> WM_CHAR 0x304B");
        printf("imm: sp_def_window_proc %s the IME hook (WM_IME_CHAR -> WM_CHAR)\n",
               patched ? "has" : "does not have yet");
    }
    {   /* ImmNotifyIME: cancel and complete */
        const uint16_t ni[3] = {0x306B, 0x307B, 0x3093};
        sp_ti_set_marked(ni, 3, 3, 0);
        n = ti_pump(r, 64);
        CHECK(n == 2 && r[0].msg == START && r[1].msg == COMP, "marked text outside a key: START, COMPOSITION (%d)", n);
        CHECK_EQ(GSTD(sg_imp_imm32_ImmNotifyIME, himc, 0x15, 4, 0), 1);   /* CPS_CANCEL */
        n = ti_pump(r, 64);
        CHECK(n == 2 && r[0].msg == COMP && r[0].w == 0 && r[0].l == 0 && r[1].msg == END, "cancel: COMPOSITION(0,0), END (%d)", n);
        CHECK_EQ(sp_ti_marked(NULL, 0, NULL, NULL), 0);
        CHECK_EQ(GSTD(sg_imp_imm32_ImmNotifyIME, himc, 0x15, 4, 0), 0);   /* nothing to cancel */
        sp_ti_set_marked(ni, 3, 1, 2);   /* converted: clause 1-2 selected */
        CHECK_EQ(GSTD(sg_imp_imm32_ImmGetCompositionStringW, himc, 0x10, buf, 64), 3);
        CHECK(M[buf] == 2 && M[buf + 1] == 1 && M[buf + 2] == 1, "attributes CONVERTED, TARGET_CONVERTED x2");
        CHECK_EQ(GSTD(sg_imp_imm32_ImmNotifyIME, himc, 0x15, 1, 0), 1);   /* CPS_COMPLETE */
        n = ti_pump(r, 64);
        CHECK(n == 4 && r[2].msg == COMP && r[2].l == LRES && r[3].msg == END, "complete: COMPOSITION(GCS_RESULTSTR), END (%d)", n);
        CHECK_EQ(GSTD(sg_imp_imm32_ImmGetCompositionStringW, himc, 0x800, buf, 4), 4);   /* truncated to the buffer */
        CHECK_EQ(GSTD(sg_imp_imm32_ImmGetCompositionStringW, himc, 0x800, buf, 64), 6);
        CHECK(sg_ld16(M, buf) == 0x306B && sg_ld16(M, buf + 4) == 0x3093, "result にほん");
        CHECK_EQ(GSTD(sg_imp_imm32_ImmNotifyIME, himc, 0x10, 0, 0), 0);   /* NI_OPENCANDIDATE: the IM's own */
        CHECK_EQ(GSTD(sg_imp_imm32_ImmGetCandidateListW, himc, 0, 0, 0), 0);
    }
    {   /* the game closes the IME when no text field wants it (0x84ba1a): composition cancelled, keys plain */
        const uint16_t k[1] = {'k'};
        sp_ti_set_marked(k, 1, 1, 0);
        CHECK_EQ(GSTD(sg_imp_imm32_ImmSetOpenStatus, himc, 0), 1);
        n = ti_pump(r, 64);
        CHECK(n == 4 && r[0].msg == START && r[2].msg == COMP && r[2].l == 0 && r[3].msg == END, "close: cancelled (%d)", n);
        CHECK_EQ(GSTD(sg_imp_imm32_ImmGetOpenStatus, himc), 0);
        CHECK(!sp_ti_wants_key(0x28, 0), "closed: letter keys take today's path");
        sp_ti_set_source(SP_TI_IM, "test.another.inputmethod");
        CHECK_EQ(GSTD(sg_imp_imm32_ImmGetOpenStatus, himc), 1);   /* choosing an input source opens it */
        CHECK_EQ(GSTD(sg_imp_imm32_ImmSetOpenStatus, himc, 1), 1);
    }
    sp_ti_force_kind(-1);
    sp_ti_reset();
    printf("imm: %d text-input checks\n", t_pass + t_fail - checks0);
}

int main(int argc, char **argv) {
    const char *script = argc > 1 ? argv[1] : "input-script.txt";
    const char *expected = argc > 2 ? argv[2] : "expected-wm.log";
    if (argc > 3) g_compare = argv[3];
    if (fx_init()) { fprintf(stderr, "fixture init failed\n"); return 2; }
    if (argc > 1 && !strcmp(argv[1], "--gui")) return gui_smoke();
    fx_register(WNDPROC_VA, guest_wndproc);
    fx_register(PUMP_VA, guest_pump);
    sgp_set_backend(&sgp_backend_headless);
    test_registration();
    run_script(script, expected);
    test_geometry();
    test_queue_misc();
    test_set_cursor_pos();
    test_boot_stop();
    test_events();
    test_close_requests();
    test_imm();
    printf("guest platform tests: %d passed, %d failed\n", t_pass, t_fail);
    return t_fail ? 1 : 0;
}
