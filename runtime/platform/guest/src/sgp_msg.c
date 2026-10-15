/* sgp_msg.c -- USER32 message loop, painting, timers, keyboard, mouse and cursor, hooks,
 * clipboard and message boxes.
 *
 * The pump [obs, from the disassembly]: 0x8482f0 calls InvalidateRect(hwnd, NULL, FALSE),
 * then PeekMessageW(PM_REMOVE) / TranslateMessage / DispatchMessageW until the queue is empty;
 * the last message of each pass is WM_PAINT, whose handler 0x848420 brackets the frame
 * (message 0x1ee100a) with BeginPaint/EndPaint. sp_queue.c synthesises WM_PAINT while the window
 * is visible and invalid, after posted and input messages, so every pass ends with one frame.
 * PeekMessageA drains key messages in the IME code (0x84a0e3). Guest window procedures run
 * through sgp_call_wndproc (an sg_cleanup barrier), always from inside one of these shims. */
#include <stdarg.h>
#include <stdio.h>
#include <stdlib.h>

#include "sgp_int.h"
#include "sgp_clipboard.h"

/* ---- input trace (SPORE_INPUT_TRACE=1; sgp_int.h) ----------------------------------------------- */
int sgp_input_trace_on(void) {
    static int on = -1;
    if (on < 0) {
        const char *e = getenv("SPORE_INPUT_TRACE");
        on = e && *e && *e != '0';
    }
    return on;
}

void sgp_input_trace(const char *fmt, ...) {
    static unsigned lines;
    va_list ap;
    if (lines >= 200000u) return;
    if (++lines == 200000u) { fprintf(stderr, "in: trace limit reached\n"); return; }
    fprintf(stderr, "in: t=%u ", (unsigned)sp_tick_ms());
    va_start(ap, fmt);
    vfprintf(stderr, fmt, ap);
    va_end(ap);
    fputc('\n', stderr);
}

#define ERROR_INVALID_WINDOW_HANDLE 1400u

/* ---- host close requests ------------------------------------------------------------------------ */
/* Called by runtime/platform/macos's sp_window.m for the title-bar close button, Cmd-Q and the app's
 * Quit (h 0). During a timed run (SPORE_RUN_SECONDS set to a positive number, sgp_run.c) the request
 * is logged and dropped, so an accidental click or Cmd-Q does not end a measurement; otherwise a
 * window's request becomes WM_CLOSE (the game decides) and Quit proceeds. Messages the game
 * sends or posts itself are not affected. Returns 1 if the request was delivered. */
int sgp_host_close_request(sp_hwnd h, const char *source);
int sgp_host_close_request(sp_hwnd h, const char *source) {
    const char *e = getenv("SPORE_RUN_SECONDS");
    if (e && strtod(e, NULL) > 0) {
        fprintf(stderr, "platform: close request (%s, window 0x%x) dropped: timed run (SPORE_RUN_SECONDS=%s)\n",
                source ? source : "host", (unsigned)h, e);
        return 0;
    }
    if (h) sp_window_close_request(h);
    return 1;
}

/* ---- timers with TIMERPROC ---------------------------------------------------------------------- */
typedef struct tproc { uint32_t h, id, proc; } tproc;
static tproc tprocs[16];

static uint32_t timer_proc(uint32_t h, uint32_t id) {
    unsigned i;
    for (i = 0; i < 16; i++)
        if (tprocs[i].proc && tprocs[i].h == h && tprocs[i].id == id) return tprocs[i].proc;
    return 0;
}

SG_IMPORT(user32, SetTimer) { /* (hwnd, id, elapse, TIMERPROC) */
    uint32_t h = SG_ARG(0), id = SG_ARG(1), el = SG_ARG(2), proc = SG_ARG(3), r;
    static uint32_t next_id = 0x7000;
    unsigned i;
    sgp_ensure();
    if (h && !sgp_win_find(h)) { hle_set_last_error(ctx, mem, ERROR_INVALID_WINDOW_HANDLE); SG_RETURN_STDCALL(0, 16); }
    if (!h) id = next_id++;
    r = sp_set_timer(h, id, el);
    if (r && proc) {
        for (i = 0; i < 16; i++)
            if (!tprocs[i].proc || (tprocs[i].h == h && tprocs[i].id == id)) { tprocs[i] = (tproc){h, id, proc}; break; }
    }
    SGP_LOG("SetTimer(0x%x, %u, %u ms, proc 0x%x) = %u", h, id, el, proc, r);
    SG_RETURN_STDCALL(r, 16);
}

SG_IMPORT(user32, KillTimer) {
    uint32_t h = SG_ARG(0), id = SG_ARG(1);
    unsigned i;
    for (i = 0; i < 16; i++)
        if (tprocs[i].h == h && tprocs[i].id == id) tprocs[i].proc = 0;
    SG_RETURN_STDCALL(sp_kill_timer(h, id), 8);
}

/* ---- the queue ---------------------------------------------------------------------------------- */
static void put_msg(uint8_t *mem, uint32_t a, const sp_msg *m) {
    sg_st32(mem, a + 0, m->hwnd);
    sg_st32(mem, a + 4, m->message);
    sg_st32(mem, a + 8, m->wParam);
    sg_st32(mem, a + 12, (uint32_t)m->lParam);
    sg_st32(mem, a + 16, m->time);
    sg_st32(mem, a + 20, (uint32_t)m->pt.x);
    sg_st32(mem, a + 24, (uint32_t)m->pt.y);
}

static void get_msg(const uint8_t *mem, uint32_t a, sp_msg *m) {
    m->hwnd = sg_ld32(mem, a + 0);
    m->message = sg_ld32(mem, a + 4);
    m->wParam = sg_ld32(mem, a + 8);
    m->lParam = (int32_t)sg_ld32(mem, a + 12);
    m->time = sg_ld32(mem, a + 16);
    m->pt.x = (int32_t)sg_ld32(mem, a + 20);
    m->pt.y = (int32_t)sg_ld32(mem, a + 24);
}

static void paint_cadence(void) {
    unsigned i;
    uint32_t now, period;
    if (sgp_paint_hz <= 0) return;
    now = sp_tick_ms();
    period = 1000u / (uint32_t)sgp_paint_hz;
    for (i = 0; i < SGP_MAX_WIN; i++) {
        sgp_win *w = &sgp_wins[i];
        if (w->used && sp_window_visible(w->h) && (uint32_t)(now - w->last_paint_ms) >= period) sp_invalidate_rect(w->h, 0);
    }
}

static int peek(sg_ctx *ctx, uint8_t *mem, int ansi) {
    uint32_t out = SG_ARG(0), filt = SG_ARG(1), mn = SG_ARG(2), mx = SG_ARG(3), flags = SG_ARG(4);
    sp_msg m;
    int r;
    static int warned;
    if (sgp_ensure()) return 0;
    if (!sp_is_owner_thread()) {
        if (!warned++) hle_log(HLE_LOG_WARN, "PeekMessage%s on a thread that does not own the windows: no messages",
                               ansi ? "A" : "W");
        return 0;
    }
    sgp_run_check();
    sgp_stat_peek_calls++;
    paint_cadence();
    sgp_clip_poll(); /* WM_DRAWCLIPBOARD when another application changed the pasteboard */
    memset(&m, 0, sizeof m);
    r = sp_peek_message(&m, filt, mn, mx, flags);
    if (!r) return 0;
    if (flags & 1u) { /* PM_REMOVE: count each message once */
        sgp_stat_msgs++;
        if (m.message == WM_PAINT) sgp_stat_paints++;
        if (m.message == WM_TIMER) sgp_stat_timers++;
    }
    if (m.message == WM_TIMER && !m.lParam) m.lParam = (int32_t)timer_proc(m.hwnd, m.wParam);
    if (ansi && (m.message == WM_CHAR || m.message == WM_SYSCHAR) && m.wParam >= 0x100) m.wParam = '?';
    if (out) put_msg(mem, out, &m);
    if (m.message >= 0x200 && m.message <= 0x20e)
        SGP_INTRACE("%s%s %s client (%d,%d) wParam 0x%x pt (%d,%d)", flags & 1u ? "" : "peek-noremove ",
                    ansi ? "PeekMessageA" : "PeekMessageW", sp_message_name(m.message), (int)(int16_t)(m.lParam & 0xffff),
                    (int)(int16_t)((uint32_t)m.lParam >> 16), m.wParam, m.pt.x, m.pt.y);
    SGP_TRACE("PeekMessage%s -> hwnd 0x%x msg 0x%x (%s) w 0x%x l 0x%x", ansi ? "A" : "W", m.hwnd, m.message,
              sp_message_name(m.message), m.wParam, (uint32_t)m.lParam);
    return 1;
}

SG_IMPORT(user32, PeekMessageW) {
    SGP_ENTER();
    int r = peek(ctx, mem, 0);
    SGP_LEAVE();
    SG_RETURN_STDCALL(r, 20);
}

SG_IMPORT(user32, PeekMessageA) {
    SGP_ENTER();
    int r = peek(ctx, mem, 1);
    SGP_LEAVE();
    SG_RETURN_STDCALL(r, 20);
}

SG_IMPORT(user32, TranslateMessage) {
    sp_msg m;
    if (!SG_ARG(0)) SG_RETURN_STDCALL(0, 4);
    get_msg(mem, SG_ARG(0), &m);
    SG_RETURN_STDCALL(sp_translate_message(&m), 4);
}

SG_IMPORT(user32, DispatchMessageW) {
    sp_msg m;
    int32_t r = 0;
    uint32_t proc;
    if (!SG_ARG(0)) SG_RETURN_STDCALL(0, 4);
    get_msg(mem, SG_ARG(0), &m);
    sgp_stat_dispatched++;
    SGP_ENTER();
    if (m.message == WM_TIMER && m.lParam && (proc = timer_proc(m.hwnd, m.wParam)) == (uint32_t)m.lParam) {
        uint32_t a[4] = {m.hwnd, WM_TIMER, m.wParam, sp_tick_ms()};
        sgp_guest_call(ctx, proc, 4, a, "TIMERPROC");
    } else {
        r = sp_dispatch_message(&m);
    }
    SGP_LEAVE();
    SG_RETURN_STDCALL((uint32_t)r, 4);
}

SG_IMPORT(user32, PostMessageA) {
    uint32_t h = SG_ARG(0);
    int ok;
    sgp_ensure();
    if (h && !sgp_win_find(h)) { hle_set_last_error(ctx, mem, ERROR_INVALID_WINDOW_HANDLE); SG_RETURN_STDCALL(0, 16); }
    ok = sp_post_message(h, SG_ARG(1), SG_ARG(2), (int32_t)SG_ARG(3));
    if (!ok) hle_set_last_error(ctx, mem, 1816u); /* ERROR_NOT_ENOUGH_QUOTA */
    SG_RETURN_STDCALL(ok, 16);
}

SG_IMPORT(user32, SendMessageA) {
    uint32_t h = SG_ARG(0), msg = SG_ARG(1), w = SG_ARG(2);
    int32_t l = (int32_t)SG_ARG(3), r;
    if (!sgp_win_find(h)) { hle_set_last_error(ctx, mem, ERROR_INVALID_WINDOW_HANDLE); SG_RETURN_STDCALL(0, 16); }
    SGP_ENTER();
    r = sp_send_message(h, msg, w, l);
    SGP_LEAVE();
    SG_RETURN_STDCALL((uint32_t)r, 16);
}

SG_IMPORT(user32, PostQuitMessage) {
    sgp_ensure();
    sp_post_quit_message((int32_t)SG_ARG(0));
    SG_RETURN_STDCALL(0, 4);
}

static void def_proc(sg_ctx *ctx, uint8_t *mem) {
    uint32_t h = SG_ARG(0), msg = SG_ARG(1), w = SG_ARG(2);
    int32_t l = (int32_t)SG_ARG(3), r;
    SGP_ENTER();
    r = sp_def_window_proc(h, msg, w, l);
    SGP_LEAVE();
    SG_RETURN_STDCALL((uint32_t)r, 16);
}
SG_IMPORT(user32, DefWindowProcW) { def_proc(ctx, mem); }
SG_IMPORT(user32, DefWindowProcA) { def_proc(ctx, mem); }

/* ---- painting ------------------------------------------------------------------------------------ */
SG_IMPORT(user32, BeginPaint) {
    uint32_t h = SG_ARG(0), ps = SG_ARG(1), hdc;
    sp_paintstruct p;
    sgp_win *w = sgp_win_find(h);
    if (!w) SG_RETURN_STDCALL(0, 8);
    SGP_ENTER();
    hdc = sp_begin_paint(h, &p);
    SGP_LEAVE();
    w->last_paint_ms = sp_tick_ms();
    if (ps) memcpy(mem + ps, &p, sizeof p); /* PAINTSTRUCT: x86 layout, 64 bytes */
    SG_RETURN_STDCALL(hdc, 8);
}

SG_IMPORT(user32, EndPaint) {
    sp_paintstruct p;
    if (SG_ARG(1)) memcpy(&p, mem + SG_ARG(1), sizeof p);
    sp_end_paint(SG_ARG(0), &p);
    SG_RETURN_STDCALL(1, 8);
}

SG_IMPORT(user32, InvalidateRect) { /* (hwnd, rect, erase): whole client area */
    uint32_t h = SG_ARG(0);
    if (h && !sgp_win_find(h)) SG_RETURN_STDCALL(0, 12);
    sp_invalidate_rect(h, SG_ARG(2) != 0);
    SG_RETURN_STDCALL(1, 12);
}

SG_IMPORT(user32, ValidateRect) {
    uint32_t h = SG_ARG(0);
    if (h && !sgp_win_find(h)) SG_RETURN_STDCALL(0, 8);
    sp_validate_rect(h);
    SG_RETURN_STDCALL(1, 8);
}

SG_IMPORT(user32, GetDC) { SG_RETURN_STDCALL(SP_FAKE_HDC, 4); }
SG_IMPORT(user32, ReleaseDC) { SG_RETURN_STDCALL(1, 8); }
SG_IMPORT(user32, FillRect) { SG_RETURN_STDCALL(1, 12); } /* GDI background fill: nothing to draw under Metal */

/* ---- keyboard ------------------------------------------------------------------------------------ */
SG_IMPORT(user32, GetKeyState) { SG_RETURN_STDCALL((uint32_t)(int32_t)sp_get_key_state((int)(SG_ARG(0) & 0xff)), 4); }
SG_IMPORT(user32, GetAsyncKeyState) {
    SG_RETURN_STDCALL((uint32_t)(int32_t)sp_get_async_key_state((int)(SG_ARG(0) & 0xff)), 4);
}
SG_IMPORT(user32, GetKeyboardState) {
    uint8_t ks[256];
    if (!SG_ARG(0)) SG_RETURN_STDCALL(0, 4);
    sp_get_keyboard_state(ks);
    memcpy(mem + SG_ARG(0), ks, 256);
    SG_RETURN_STDCALL(1, 4);
}
/* US layout, en-US (0x849c08 keeps the low word as the language for GetLocaleInfoW). A
 * layout-aware VK map is remaining work (platform-macos.md section 4). */
SG_IMPORT(user32, GetKeyboardLayout) { SG_RETURN_STDCALL(0x04090409u, 4); }

/* ---- mouse and cursor ---------------------------------------------------------------------------- */
SG_IMPORT(user32, SetCapture) {
    uint32_t h = SG_ARG(0), old;
    if (!sgp_win_find(h)) SG_RETURN_STDCALL(0, 4);
    SGP_ENTER();
    old = sp_set_capture(h);
    SGP_LEAVE();
    SG_RETURN_STDCALL(old, 4);
}
SG_IMPORT(user32, ReleaseCapture) {
    int r;
    SGP_ENTER();
    r = sp_release_capture();
    SGP_LEAVE();
    SG_RETURN_STDCALL(r, 0);
}

SG_IMPORT(user32, GetCursorPos) {
    int32_t x, y;
    if (!SG_ARG(0)) SG_RETURN_STDCALL(0, 4);
    sgp_ensure();
    sgp_be->get_cursor_pos(&x, &y);
    sg_st32(mem, SG_ARG(0), (uint32_t)x);
    sg_st32(mem, SG_ARG(0) + 4, (uint32_t)y);
    SGP_INTRACE("GetCursorPos -> screen (%d,%d) from 0x%x", x, y, sg_ld32(mem, ctx->esp));
    SG_RETURN_STDCALL(1, 4);
}

/* Warps only while one of our windows is active: after focus loss (or Esc, which releases the
 * cursor in sp_window.m) the game cannot pull the pointer back.
 * As on Windows, moving the cursor produces a WM_MOUSEMOVE at the new position
 * for the window that has the capture or, failing that, the active window when the point is inside
 * its client area. Spore takes mouse deltas between consecutive WM_MOUSEMOVEs (0x848830) and warps
 * the cursor back during camera drags (message 0x501 -> SetCursorPos at 0x849905); without that
 * message the first move after each warp was measured from the pre-warp point. AppKit's warp makes no
 * event, so the message is made here. Positions equal to the last reported one make none
 * (sp_input_mouse_move). */
SG_IMPORT(user32, SetCursorPos) {
    int32_t x = (int32_t)SG_ARG(0), y = (int32_t)SG_ARG(1);
    sp_hwnd act;
    sgp_ensure();
    act = sp_get_active();
    if (act) {
        sp_hwnd h = sp_get_capture() ? sp_get_capture() : act;
        int32_t cx = x, cy = y, lx, ly, cw = 0, ch = 0;
        sgp_be->set_cursor_pos(x, y);
        sp_screen_to_client(h, &cx, &cy);
        sp_get_last_client(h, &lx, &ly, &cw, &ch);
        if (sp_get_capture() == h || (cx >= 0 && cy >= 0 && cx < cw && cy < ch)) sp_input_mouse_move(h, cx, cy);
        SGP_INTRACE("SetCursorPos(%d,%d) from 0x%x: client (%d,%d) of 0x%x (%dx%d)", x, y, sg_ld32(mem, ctx->esp),
                    cx, cy, (unsigned)h, cw, ch);
    } else {
        SGP_INTRACE("SetCursorPos(%d,%d) from 0x%x: ignored, no active window", x, y, sg_ld32(mem, ctx->esp));
    }
    SG_RETURN_STDCALL(1, 8);
}

SG_IMPORT(user32, ShowCursor) {
    sgp_ensure();
    SG_RETURN_STDCALL((uint32_t)sgp_be->show_cursor(SG_ARG(0) != 0), 4);
}

/* Cursor handles: system cursors 0x00e30000 + IDC_*; loaded images are pseudo handles shown as
 * the arrow (the game's own cursor art is drawn by its UI or is remaining work). */
#define SYS_CURSOR(idc) (0x00e30000u + (idc))
static uint32_t cur_cursor;
SG_IMPORT(user32, LoadCursorA) {
    uint32_t inst = SG_ARG(0), name = SG_ARG(1);
    SG_RETURN_STDCALL(!inst && name < 0x10000u ? SYS_CURSOR(name) : sgp_pseudo_handle(), 8);
}
SG_IMPORT(user32, LoadIconA) { SG_RETURN_STDCALL(sgp_pseudo_handle(), 8); }
SG_IMPORT(user32, LoadImageW) { /* (hinst, name, type, cx, cy, flags) */
    char nm[260] = "";
    uint32_t h = sgp_pseudo_handle();
    if (SG_ARG(1) >= 0x10000u) sgp_wstr_utf8(mem, SG_ARG(1), nm, sizeof nm);
    SGP_LOG("LoadImageW(type %u, \"%s\", %ux%u, flags 0x%x) = 0x%x (pseudo handle)", SG_ARG(2), nm, SG_ARG(3), SG_ARG(4),
            SG_ARG(5), h);
    SG_RETURN_STDCALL(h, 24);
}
SG_IMPORT(user32, DestroyCursor) { SG_RETURN_STDCALL(1, 4); }
SG_IMPORT(user32, GetIconInfo) { /* ICONINFO: fIcon, xHotspot, yHotspot, hbmMask, hbmColor */
    uint32_t ii = SG_ARG(1);
    if (!ii) SG_RETURN_STDCALL(0, 8);
    sg_st32(mem, ii, 0);
    sg_st32(mem, ii + 4, 0);
    sg_st32(mem, ii + 8, 0);
    sg_st32(mem, ii + 12, sgp_pseudo_handle());
    sg_st32(mem, ii + 16, sgp_pseudo_handle());
    SG_RETURN_STDCALL(1, 8);
}
SG_IMPORT(user32, SetCursor) {
    uint32_t h = SG_ARG(0), old = cur_cursor;
    sgp_ensure();
    cur_cursor = h;
    if (h != old) sgp_be->set_cursor_shape(!h ? 0 : (h & 0xffff0000u) == 0x00e30000u ? h & 0xffffu : 32512u);
    SG_RETURN_STDCALL(old, 4);
}
SG_IMPORT(user32, GetDoubleClickTime) { sgp_ensure(); SG_RETURN_STDCALL(sgp_be->double_click_ms(), 0); }

/* ---- hooks: the WH_KEYBOARD_LL Alt+Tab trap (0x7e60b7) is not needed on macOS ---------------------- */
SG_IMPORT(user32, SetWindowsHookExA) {
    SGP_LOG("SetWindowsHookExA(type %d, proc 0x%x): accepted, never called", (int32_t)SG_ARG(0), SG_ARG(1));
    SG_RETURN_STDCALL(sgp_pseudo_handle(), 16);
}
SG_IMPORT(user32, UnhookWindowsHookEx) { SG_RETURN_STDCALL(1, 4); }
SG_IMPORT(user32, CallNextHookEx) { SG_RETURN_STDCALL(0, 16); }

/* ---- clipboard: runtime/platform/guest/src/sgp_clipboard.c (NSPasteboard, WM_DRAWCLIPBOARD) ---------------- */
SG_IMPORT(user32, OpenClipboard) { SG_TAILCALL(sgp_clip_OpenClipboard); }
SG_IMPORT(user32, CloseClipboard) { SG_TAILCALL(sgp_clip_CloseClipboard); }
SG_IMPORT(user32, EmptyClipboard) { SG_TAILCALL(sgp_clip_EmptyClipboard); }
SG_IMPORT(user32, IsClipboardFormatAvailable) { SG_TAILCALL(sgp_clip_IsClipboardFormatAvailable); }
SG_IMPORT(user32, GetClipboardData) { SG_TAILCALL(sgp_clip_GetClipboardData); }
SG_IMPORT(user32, SetClipboardData) { SG_TAILCALL(sgp_clip_SetClipboardData); }
SG_IMPORT(user32, SetClipboardViewer) { SG_TAILCALL(sgp_clip_SetClipboardViewer); }
SG_IMPORT(user32, ChangeClipboardChain) { SG_TAILCALL(sgp_clip_ChangeClipboardChain); }

/* ---- message boxes, dialogs, window station ------------------------------------------------------ */
static void message_box(sg_ctx *ctx, uint8_t *mem, int ansi) {
    char text[1024], cap[256];
    uint32_t type = SG_ARG(3);
    int r;
    if (ansi) { sgp_astr(mem, SG_ARG(1), text, sizeof text); sgp_astr(mem, SG_ARG(2), cap, sizeof cap); }
    else { sgp_wstr_utf8(mem, SG_ARG(1), text, sizeof text); sgp_wstr_utf8(mem, SG_ARG(2), cap, sizeof cap); }
    hle_log(HLE_LOG_WARN, "MessageBox%s(\"%s\", \"%s\", 0x%x)", ansi ? "A" : "W", text, cap, type);
    {   /* SPORE_MSGBOX=auto answers with the default button without a modal NSAlert, so unattended
         * runs are not blocked by an error box. */
        const char *e = getenv("SPORE_MSGBOX");
        if (e && !strcmp(e, "auto")) {
            r = (type & 0xf) == 4 || (type & 0xf) == 3 ? 6 /* IDYES */ : 1 /* IDOK */;
            hle_log(HLE_LOG_WARN, "MessageBox -> %d (SPORE_MSGBOX=auto)", r);
            SG_RETURN_STDCALL((uint32_t)r, 16);
        }
    }
    sgp_ensure();
    r = sgp_be->message_box(text, cap, type);
    SG_RETURN_STDCALL((uint32_t)r, 16);
}
SG_IMPORT(user32, MessageBoxA) { message_box(ctx, mem, 1); }
SG_IMPORT(user32, MessageBoxW) { message_box(ctx, mem, 0); }
SG_IMPORT(user32, IsDlgButtonChecked) { SG_RETURN_STDCALL(0, 8); }
SG_IMPORT(user32, CheckDlgButton) { SG_RETURN_STDCALL(1, 12); }
SG_IMPORT(user32, GetProcessWindowStation) { SG_RETURN_STDCALL(0x00e20010u, 0); }
/* (h, UOI_FLAGS 1, pv, len, needed): USEROBJECTFLAGS{fInherit, fReserved, dwFlags = WSF_VISIBLE}:
 * the CRT's "interactive window station" test before a message box. */
SG_IMPORT(user32, GetUserObjectInformationW) {
    uint32_t idx = SG_ARG(1), pv = SG_ARG(2), len = SG_ARG(3), need = SG_ARG(4);
    if (need) sg_st32(mem, need, 12);
    if (idx != 1 || !pv || len < 12) { hle_set_last_error(ctx, mem, 122u); SG_RETURN_STDCALL(0, 20); }
    sg_st32(mem, pv, 0);
    sg_st32(mem, pv + 4, 0);
    sg_st32(mem, pv + 8, 1);
    SG_RETURN_STDCALL(1, 20);
}
