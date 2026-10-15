/* sgp_window.c -- USER32 window classes, windows, geometry, metrics and monitors.
 *
 * Call sites [obs, data/imports.json and a disassembly of SporeApp.exe]:
 *   0x849c56..0x849d6e  "Canvas" class (CS_DBLCLKS, wndproc 0x849310), CreateWindowExW(exstyle,
 *                       atom, NULL, style, CW_USEDEFAULT x4, parent, 0, hinst, 0), LoadCursorA,
 *                       SetWindowLongW(GWL_USERDATA), ShowWindow(SW_HIDE), UpdateWindow, SetTimer
 *   0x848fe0..0x849088  centring: SystemParametersInfoA(SPI_GETWORKAREA), GetSystemMetrics(0/1),
 *                       IntersectRect, then 0x848530: AdjustWindowRectEx + MoveWindow
 *   0x8490c0..0x849179  "fullscreen": style := WS_POPUP|WS_SYSMENU, SetWindowPos(HWND_TOPMOST,
 *                       0, 0, w, h, SWP_FRAMECHANGED), ShowWindow(SW_MAXIMIZE). Here: a borderless
 *                       window over the current display, the client w x h shown letterboxed; no
 *                       display-mode switch (sync_fullscreen, sp_mac_set_fullscreen).
 *   0x92b2ee            CreateWindowExA for EAClipboard's never-shown window.
 *   0xb63e17/0xb63ef7   debug dialogs (off the normal path).
 */
#include <stdlib.h>

#include "sgp_int.h"

enum { GWL_WNDPROC = -4, GWL_HINSTANCE = -6, GWL_HWNDPARENT = -8, GWL_ID = -12, GWL_STYLE = -16, GWL_EXSTYLE = -20,
       GWL_USERDATA = -21 };
#define WS_VISIBLE 0x10000000u
#define CW_USEDEFAULT 0x80000000u
#define ERROR_INVALID_WINDOW_HANDLE 1400u
#define ERROR_CLASS_ALREADY_EXISTS 1410u
#define ERROR_CANNOT_FIND_WND_CLASS 1407u
#define ERROR_INVALID_PARAMETER 87u
#define DESKTOP_HWND 0x00010000u
#define MONITOR_HANDLE 0x00e20001u

/* ---- classes ------------------------------------------------------------------------------------ */
static uint16_t next_atom = 0xc000;

/* WNDCLASSEXW (48): cbSize, style, lpfnWndProc, cbClsExtra, cbWndExtra, hInstance, hIcon, hCursor,
 * hbrBackground, lpszMenuName, lpszClassName, hIconSm. WNDCLASSA (40) is the same minus cbSize
 * and hIconSm. */
static uint32_t register_class(sg_ctx *ctx, uint8_t *mem, uint32_t base, int ansi) {
    uint32_t nm = sg_ld32(mem, base + 36);
    char name[64];
    sgp_class *c = NULL;
    unsigned i;
    if (nm < 0x10000u) { hle_set_last_error(ctx, mem, ERROR_INVALID_PARAMETER); return 0; }
    if (ansi) sgp_astr(mem, nm, name, sizeof name);
    else sgp_wstr_utf8(mem, nm, name, sizeof name);
    sgp_lower(name);
    if (sgp_class_find_name(name)) { hle_set_last_error(ctx, mem, ERROR_CLASS_ALREADY_EXISTS); return 0; }
    for (i = 0; i < SGP_MAX_CLASS && !c; i++)
        if (!sgp_classes[i].used) c = &sgp_classes[i];
    if (!c) { hle_set_last_error(ctx, mem, 8); return 0; }
    memset(c, 0, sizeof *c);
    c->used = 1;
    c->atom = next_atom++;
    snprintf(c->name, sizeof c->name, "%s", name);
    c->style = sg_ld32(mem, base + 0);
    c->wndproc = sg_ld32(mem, base + 4);
    c->extra_bytes = sg_ld32(mem, base + 12);
    c->hinst = sg_ld32(mem, base + 16);
    c->hcursor = sg_ld32(mem, base + 24);
    c->hbrush = sg_ld32(mem, base + 28);
    c->ansi = ansi;
    SGP_LOG("RegisterClass%s(\"%s\", style 0x%x, wndproc 0x%08x, extra %u) = 0x%04x", ansi ? "A" : "ExW", c->name,
            c->style, c->wndproc, c->extra_bytes, c->atom);
    return c->atom;
}

SG_IMPORT(user32, RegisterClassExW) {
    uint32_t wc = SG_ARG(0);
    if (!wc || sg_ld32(mem, wc) != 48u) { hle_set_last_error(ctx, mem, ERROR_INVALID_PARAMETER); SG_RETURN_STDCALL(0, 4); }
    SG_RETURN_STDCALL(register_class(ctx, mem, wc + 4, 0), 4);
}

SG_IMPORT(user32, RegisterClassA) {
    uint32_t wc = SG_ARG(0);
    if (!wc) { hle_set_last_error(ctx, mem, ERROR_INVALID_PARAMETER); SG_RETURN_STDCALL(0, 4); }
    SG_RETURN_STDCALL(register_class(ctx, mem, wc, 1), 4);
}

static sgp_class *class_from_arg(uint8_t *mem, uint32_t a, int ansi) {
    char name[64];
    if (a < 0x10000u) return sgp_class_find_atom((uint16_t)a);
    if (ansi) sgp_astr(mem, a, name, sizeof name);
    else sgp_wstr_utf8(mem, a, name, sizeof name);
    sgp_lower(name);
    return sgp_class_find_name(name);
}

SG_IMPORT(user32, UnregisterClassA) {
    sgp_class *c = class_from_arg(mem, SG_ARG(0), 1);
    if (!c) { hle_set_last_error(ctx, mem, ERROR_CANNOT_FIND_WND_CLASS); SG_RETURN_STDCALL(0, 8); }
    c->used = 0;
    SG_RETURN_STDCALL(1, 8);
}

/* ---- windows ------------------------------------------------------------------------------------- */
static int32_t bridge(void *user, sp_hwnd h, uint32_t msg, uint32_t w, int32_t l) {
    sgp_win *win = (sgp_win *)user;
    if (!win->used || win->h != h) return 0;
    return sgp_call_wndproc(win->wndproc, h, msg, w, l);
}

static uint32_t create_window(sg_ctx *ctx, uint8_t *mem, int ansi) {
    uint32_t exstyle = SG_ARG(0), cls = SG_ARG(1), title = SG_ARG(2), style = SG_ARG(3);
    int32_t x = (int32_t)SG_ARG(4), y = (int32_t)SG_ARG(5), w = (int32_t)SG_ARG(6), hh = (int32_t)SG_ARG(7);
    uint32_t parent = SG_ARG(8), menu = SG_ARG(9), hinst = SG_ARG(10);
    sgp_class *c;
    sgp_win *win = NULL;
    sp_hwnd h;
    int32_t top, sw, sh;
    sp_rect work;
    unsigned i;
    if (sgp_ensure()) { hle_set_last_error(ctx, mem, 1400); return 0; }
    c = class_from_arg(mem, cls, ansi);
    if (!c) { hle_set_last_error(ctx, mem, ERROR_CANNOT_FIND_WND_CLASS); return 0; }
    for (i = 0; i < SGP_MAX_WIN; i++) {
        if (sgp_wins[i].used && !sp_is_window(sgp_wins[i].h)) sgp_wins[i].used = 0;
        if (!sgp_wins[i].used && !win) win = &sgp_wins[i];
    }
    if (!win) { hle_set_last_error(ctx, mem, 8); return 0; }
    memset(win, 0, sizeof *win);
    h = sp_window_register(bridge, win, c->style);
    if (!h) { hle_set_last_error(ctx, mem, 8); return 0; }
    win->used = 1;
    win->h = h;
    win->wndproc = c->wndproc;
    win->style = style & ~WS_VISIBLE;
    win->exstyle = exstyle;
    win->parent = parent;
    win->menu = menu;
    win->hinst = hinst;
    win->atom = c->atom;
    win->ansi = ansi;
    win->class_style = c->style;
    win->toplevel = !(style & 0x40000000u); /* not WS_CHILD */
    if (ansi) sgp_astr(mem, title, win->title, sizeof win->title);
    else sgp_wstr_utf8(mem, title, win->title, sizeof win->title);
    /* Initial rect: CW_USEDEFAULT gives a centred 800x600 client area. x/y/w/h are the outer
     * window rect (frame model: caption on top only). Recorded silently while hidden. */
    top = sgp_frame_top(style, exstyle);
    sgp_be->screen(&sw, &sh, &work, NULL, NULL, NULL);
    if ((uint32_t)w == CW_USEDEFAULT) { w = 800; hh = 600 + top; }
    if ((uint32_t)x == CW_USEDEFAULT) { x = work.left + (work.right - work.left - w) / 2; y = work.top + (work.bottom - work.top - hh) / 2; }
    sp_window_rect_changed(h, x, y + top, w, hh - top);
    SGP_LOG("CreateWindowEx%s(class 0x%04x \"%s\", \"%s\", style 0x%08x, ex 0x%x, %d,%d %dx%d) = 0x%08x",
            ansi ? "A" : "W", c->atom, c->name, win->title, style, exstyle, x, y, w, hh, h);
    /* Creation messages (WM_NCCREATE/WM_CREATE ...) are not synthesised: the game sets
     * GWL_USERDATA after CreateWindowEx returns, so its window procedure forwards every creation
     * message to DefWindowProc (platform-macos.md section 2). WS_VISIBLE shows it now. */
    if (style & WS_VISIBLE) {
        sgp_be->attach(h, win->title, x, y + top, w, hh - top);
        win->attached = 1;
        win->style |= WS_VISIBLE;
        sp_show_window(h, 1, !sgp_be->show(h, 1, 1));
    }
    return h;
}

SG_IMPORT(user32, CreateWindowExW) {
    hle_stop_check(ctx, mem, "CreateWindowExW"); /* the stop before the first window (win32-layer.md 22.4): keep it first */
    SGP_ENTER();
    uint32_t r = create_window(ctx, mem, 0);
    SGP_LEAVE();
    SG_RETURN_STDCALL(r, 48);
}

SG_IMPORT(user32, CreateWindowExA) {
    hle_stop_check(ctx, mem, "CreateWindowExA");
    SGP_ENTER();
    uint32_t r = create_window(ctx, mem, 1);
    SGP_LEAVE();
    SG_RETURN_STDCALL(r, 48);
}

SG_IMPORT(user32, DestroyWindow) {
    uint32_t h = SG_ARG(0);
    sgp_win *w = sgp_win_find(h);
    int ok;
    if (!w) { hle_set_last_error(ctx, mem, ERROR_INVALID_WINDOW_HANDLE); SG_RETURN_STDCALL(0, 4); }
    SGP_ENTER();
    ok = sp_destroy_window(h);
    if (w->attached) sgp_be->detach(h);
    w->used = 0;
    SGP_LEAVE();
    SG_RETURN_STDCALL(ok, 4);
}

/* ---- showing --------------------------------------------------------------------------------- */
enum { SW_HIDE = 0, SW_SHOWNORMAL = 1, SW_SHOWMINIMIZED = 2, SW_MAXIMIZE = 3, SW_SHOWNOACTIVATE = 4, SW_SHOW = 5,
       SW_MINIMIZE = 6, SW_SHOWMINNOACTIVE = 7, SW_SHOWNA = 8, SW_RESTORE = 9, SW_SHOWDEFAULT = 10 };

/* A top-level WS_POPUP window without a caption is the game's fullscreen window (0x8490c0). Its
 * style takes effect when it is first shown and at each SetWindowPos(SWP_FRAMECHANGED), as on Windows. */
#define WS_POPUP 0x80000000u
#define WS_CAPTION 0x00C00000u
static void sync_fullscreen(sgp_win *w) {
    int want;
    if (!w->attached || !w->toplevel || !sgp_be->set_fullscreen) return;
    want = (w->style & WS_POPUP) && (w->style & WS_CAPTION) != WS_CAPTION;
    sgp_be->set_fullscreen(w->h, want);
}

static void show_window(sgp_win *w, int show, int activate) {
    int32_t x, y, cw, ch;
    if (show && !w->attached) {
        sp_get_last_client(w->h, &x, &y, &cw, &ch);
        sgp_be->attach(w->h, w->title, x, y, cw, ch);
        w->attached = 1;
        sync_fullscreen(w);
    }
    if (show) w->style |= WS_VISIBLE;
    else w->style &= ~WS_VISIBLE;
    if (show) {
        int host = sgp_be->show(w->h, 1, activate);
        const char *f = getenv("SPORE_FORCE_ACTIVE");
        sp_show_window(w->h, 1, activate && !host);
        /* macOS may refuse to make a background-launched app key, so the window never activates
         * and the game never draws. SPORE_FORCE_ACTIVE=1 activates the guest window on show anyway. */
        if (activate && host && f && *f && *f != '0' && sp_get_active() != w->h) sp_window_activate(w->h, 1, 1);
    } else {
        sp_show_window(w->h, 0, 0);
        if (w->attached) sgp_be->show(w->h, 0, 0);
    }
}

SG_IMPORT(user32, ShowWindow) {
    uint32_t h = SG_ARG(0), cmd = SG_ARG(1);
    sgp_win *w = sgp_win_find(h);
    int was;
    if (!w) SG_RETURN_STDCALL(0, 8);
    SGP_ENTER();
    was = sp_window_visible(h);
    switch (cmd) {
    case SW_HIDE: show_window(w, 0, 0); break;
    case SW_SHOWMINIMIZED: case SW_MINIMIZE: case SW_SHOWMINNOACTIVE:
        SGP_LOG("ShowWindow(0x%x, %u): minimise ignored", h, cmd);
        break;
    case SW_SHOWNOACTIVATE: case SW_SHOWNA: show_window(w, 1, 0); break;
    case SW_MAXIMIZE: /* no zoom; a popup window is fullscreen already (sync_fullscreen) */
        show_window(w, 1, 1);
        sync_fullscreen(w);
        break;
    default: show_window(w, 1, 1); break;
    }
    SGP_LEAVE();
    SGP_LOG("ShowWindow(0x%x, %u) = %d", h, cmd, was);
    SG_RETURN_STDCALL(was ? 1 : 0, 8);
}

SG_IMPORT(user32, UpdateWindow) {
    uint32_t h = SG_ARG(0);
    if (!sgp_win_find(h)) SG_RETURN_STDCALL(0, 4);
    SGP_ENTER();
    sp_update_window(h);
    SGP_LEAVE();
    SG_RETURN_STDCALL(1, 4);
}

SG_IMPORT(user32, IsWindowVisible) { SG_RETURN_STDCALL(sgp_win_find(SG_ARG(0)) && sp_window_visible(SG_ARG(0)), 4); }
SG_IMPORT(user32, IsIconic) {
    uint32_t h = SG_ARG(0);
    SG_RETURN_STDCALL(sgp_win_find(h) && sgp_be->is_iconic(h), 4);
}
SG_IMPORT(user32, IsZoomed) { SG_RETURN_STDCALL(0, 4); } /* never maximised (see SW_MAXIMIZE) */

/* ---- geometry ----------------------------------------------------------------------------------- */
static void put_rect(uint8_t *mem, uint32_t a, int32_t l, int32_t t, int32_t r, int32_t b) {
    sg_st32(mem, a, (uint32_t)l);
    sg_st32(mem, a + 4, (uint32_t)t);
    sg_st32(mem, a + 8, (uint32_t)r);
    sg_st32(mem, a + 12, (uint32_t)b);
}

static void move_window(sgp_win *w, int32_t x, int32_t y, int32_t cx, int32_t cy, int move, int size) {
    int32_t ox, oy, ow, oh, top = sgp_frame_top(w->style, w->exstyle);
    sp_get_last_client(w->h, &ox, &oy, &ow, &oh);
    if (move) { ox = x; oy = y + top; }
    if (size) { ow = cx; oh = cy - top; }
    if (ow < 0) ow = 0;
    if (oh < 0) oh = 0;
    sgp_be->set_client_rect(w->h, w->attached, ox, oy, ow, oh);
}

SG_IMPORT(user32, SetWindowPos) {
    uint32_t h = SG_ARG(0), after = SG_ARG(1), flags = SG_ARG(6);
    int32_t x = (int32_t)SG_ARG(2), y = (int32_t)SG_ARG(3), cx = (int32_t)SG_ARG(4), cy = (int32_t)SG_ARG(5);
    sgp_win *w = sgp_win_find(h);
    if (!w) { hle_set_last_error(ctx, mem, ERROR_INVALID_WINDOW_HANDLE); SG_RETURN_STDCALL(0, 28); }
    SGP_ENTER();
    SGP_LOG("SetWindowPos(0x%x, after 0x%x, %d,%d %dx%d, flags 0x%x)", h, after, x, y, cx, cy, flags);
    if (flags & 0x20) sync_fullscreen(w); /* SWP_FRAMECHANGED: the style set with SetWindowLong applies */
    if (!(flags & 0x1) || !(flags & 0x2)) move_window(w, x, y, cx, cy, !(flags & 0x2), !(flags & 0x1));
    if (flags & 0x40) show_window(w, 1, !(flags & 0x10)); /* SWP_SHOWWINDOW, SWP_NOACTIVATE */
    if (flags & 0x80) show_window(w, 0, 0);               /* SWP_HIDEWINDOW */
    SGP_LEAVE();
    SG_RETURN_STDCALL(1, 28);
}

SG_IMPORT(user32, MoveWindow) {
    uint32_t h = SG_ARG(0);
    int32_t x = (int32_t)SG_ARG(1), y = (int32_t)SG_ARG(2), cx = (int32_t)SG_ARG(3), cy = (int32_t)SG_ARG(4);
    sgp_win *w = sgp_win_find(h);
    if (!w) { hle_set_last_error(ctx, mem, ERROR_INVALID_WINDOW_HANDLE); SG_RETURN_STDCALL(0, 24); }
    SGP_ENTER();
    SGP_LOG("MoveWindow(0x%x, %d,%d %dx%d, repaint %u)", h, x, y, cx, cy, SG_ARG(5));
    move_window(w, x, y, cx, cy, 1, 1);
    if (SG_ARG(5)) sp_invalidate_rect(h, 1);
    SGP_LEAVE();
    SG_RETURN_STDCALL(1, 24);
}

SG_IMPORT(user32, GetClientRect) {
    uint32_t h = SG_ARG(0), r = SG_ARG(1);
    int32_t x, y, w, hh;
    if (!r) SG_RETURN_STDCALL(0, 8);
    if (h == DESKTOP_HWND) {
        sgp_ensure();
        sgp_be->screen(&w, &hh, NULL, NULL, NULL, NULL);
        put_rect(mem, r, 0, 0, w, hh);
        SG_RETURN_STDCALL(1, 8);
    }
    if (!sgp_win_find(h)) { hle_set_last_error(ctx, mem, ERROR_INVALID_WINDOW_HANDLE); SG_RETURN_STDCALL(0, 8); }
    sp_get_last_client(h, &x, &y, &w, &hh);
    put_rect(mem, r, 0, 0, w, hh);
    SG_RETURN_STDCALL(1, 8);
}

SG_IMPORT(user32, GetWindowRect) {
    uint32_t h = SG_ARG(0), r = SG_ARG(1);
    int32_t x, y, w, hh, top;
    sgp_win *win;
    if (!r) SG_RETURN_STDCALL(0, 8);
    if (h == DESKTOP_HWND) {
        sgp_ensure();
        sgp_be->screen(&w, &hh, NULL, NULL, NULL, NULL);
        put_rect(mem, r, 0, 0, w, hh);
        SG_RETURN_STDCALL(1, 8);
    }
    win = sgp_win_find(h);
    if (!win) { hle_set_last_error(ctx, mem, ERROR_INVALID_WINDOW_HANDLE); SG_RETURN_STDCALL(0, 8); }
    sp_get_last_client(h, &x, &y, &w, &hh);
    top = sgp_frame_top(win->style, win->exstyle);
    put_rect(mem, r, x, y - top, x + w, y + hh);
    SG_RETURN_STDCALL(1, 8);
}

/* BOOL AdjustWindowRectEx(LPRECT, DWORD style, BOOL menu, DWORD exstyle) */
SG_IMPORT(user32, AdjustWindowRectEx) {
    uint32_t r = SG_ARG(0), style = SG_ARG(1), ex = SG_ARG(3);
    if (!r) SG_RETURN_STDCALL(0, 16);
    sgp_ensure();
    sg_st32(mem, r + 4, sg_ld32(mem, r + 4) - (uint32_t)sgp_frame_top(style, ex));
    SG_RETURN_STDCALL(1, 16);
}

SG_IMPORT(user32, ClientToScreen) {
    uint32_t h = SG_ARG(0), p = SG_ARG(1);
    int32_t x, y;
    if (!p || !sgp_win_find(h)) SG_RETURN_STDCALL(0, 8);
    x = (int32_t)sg_ld32(mem, p);
    y = (int32_t)sg_ld32(mem, p + 4);
    sp_client_to_screen(h, &x, &y);
    SGP_INTRACE("ClientToScreen(0x%x) (%d,%d) -> (%d,%d) from 0x%x", h, (int)sg_ld32(mem, p), (int)sg_ld32(mem, p + 4), x,
                y, sg_ld32(mem, ctx->esp));
    sg_st32(mem, p, (uint32_t)x);
    sg_st32(mem, p + 4, (uint32_t)y);
    SG_RETURN_STDCALL(1, 8);
}

SG_IMPORT(user32, ScreenToClient) {
    uint32_t h = SG_ARG(0), p = SG_ARG(1);
    int32_t x, y;
    if (!p || !sgp_win_find(h)) SG_RETURN_STDCALL(0, 8);
    x = (int32_t)sg_ld32(mem, p);
    y = (int32_t)sg_ld32(mem, p + 4);
    sp_screen_to_client(h, &x, &y);
    SGP_INTRACE("ScreenToClient(0x%x) (%d,%d) -> (%d,%d) from 0x%x", h, (int)sg_ld32(mem, p), (int)sg_ld32(mem, p + 4), x,
                y, sg_ld32(mem, ctx->esp));
    sg_st32(mem, p, (uint32_t)x);
    sg_st32(mem, p + 4, (uint32_t)y);
    SG_RETURN_STDCALL(1, 8);
}

SG_IMPORT(user32, IntersectRect) {
    uint32_t d = SG_ARG(0), a = SG_ARG(1), b = SG_ARG(2);
    int32_t l, t, r, bo;
    if (!d || !a || !b) SG_RETURN_STDCALL(0, 12);
    l = (int32_t)sg_ld32(mem, a);
    t = (int32_t)sg_ld32(mem, a + 4);
    r = (int32_t)sg_ld32(mem, a + 8);
    bo = (int32_t)sg_ld32(mem, a + 12);
    if ((int32_t)sg_ld32(mem, b) > l) l = (int32_t)sg_ld32(mem, b);
    if ((int32_t)sg_ld32(mem, b + 4) > t) t = (int32_t)sg_ld32(mem, b + 4);
    if ((int32_t)sg_ld32(mem, b + 8) < r) r = (int32_t)sg_ld32(mem, b + 8);
    if ((int32_t)sg_ld32(mem, b + 12) < bo) bo = (int32_t)sg_ld32(mem, b + 12);
    if (l >= r || t >= bo) { put_rect(mem, d, 0, 0, 0, 0); SG_RETURN_STDCALL(0, 12); }
    put_rect(mem, d, l, t, r, bo);
    SG_RETURN_STDCALL(1, 12);
}

/* ---- window longs and text --------------------------------------------------------------------- */
static uint32_t get_long(sgp_win *w, int32_t idx, int *ok) {
    *ok = 1;
    switch (idx) {
    case GWL_WNDPROC: return w->wndproc;
    case GWL_HINSTANCE: return w->hinst;
    case GWL_HWNDPARENT: return w->parent;
    case GWL_ID: return w->id;
    case GWL_STYLE: return w->style | (sp_window_visible(w->h) ? WS_VISIBLE : 0);
    case GWL_EXSTYLE: return w->exstyle;
    case GWL_USERDATA: return w->userdata;
    default:
        if (idx >= 0 && (idx & 3) == 0 && idx / 4 < SGP_EXTRA) return w->extra[idx / 4];
        *ok = 0;
        return 0;
    }
}

static uint32_t set_long(sgp_win *w, int32_t idx, uint32_t v, int *ok) {
    uint32_t old = get_long(w, idx, ok);
    if (!*ok) return 0;
    switch (idx) {
    case GWL_WNDPROC: w->wndproc = v; break; /* subclassing: no CallWindowProc import, so unexpected */
    case GWL_HINSTANCE: w->hinst = v; break;
    case GWL_HWNDPARENT: w->parent = v; break;
    case GWL_ID: w->id = v; break;
    case GWL_STYLE: w->style = v & ~WS_VISIBLE; break; /* takes effect at the next SetWindowPos (FRAMECHANGED) */
    case GWL_EXSTYLE: w->exstyle = v; break;
    case GWL_USERDATA: w->userdata = v; break;
    default: w->extra[idx / 4] = v; break;
    }
    SGP_LOG("SetWindowLong(0x%x, %d, 0x%08x) = 0x%08x", w->h, idx, v, old);
    return old;
}

static void set_long_shim(sg_ctx *ctx, uint8_t *mem) {
    sgp_win *w = sgp_win_find(SG_ARG(0));
    int ok = 0;
    uint32_t r = w ? set_long(w, (int32_t)SG_ARG(1), SG_ARG(2), &ok) : 0;
    hle_set_last_error(ctx, mem, w ? (ok ? 0 : 1413u /* ERROR_INVALID_INDEX */) : ERROR_INVALID_WINDOW_HANDLE);
    SG_RETURN_STDCALL(r, 12);
}
SG_IMPORT(user32, SetWindowLongW) { set_long_shim(ctx, mem); }
SG_IMPORT(user32, SetWindowLongA) { set_long_shim(ctx, mem); }

SG_IMPORT(user32, GetWindowLongA) {
    sgp_win *w = sgp_win_find(SG_ARG(0));
    int ok = 0;
    uint32_t r = w ? get_long(w, (int32_t)SG_ARG(1), &ok) : 0;
    if (!w || !ok) hle_set_last_error(ctx, mem, w ? 1413u : ERROR_INVALID_WINDOW_HANDLE);
    SG_RETURN_STDCALL(r, 8);
}

SG_IMPORT(user32, SetWindowTextW) {
    sgp_win *w = sgp_win_find(SG_ARG(0));
    if (!w) SG_RETURN_STDCALL(0, 8);
    sgp_wstr_utf8(mem, SG_ARG(1), w->title, sizeof w->title);
    if (w->attached) sgp_be->set_title(w->h, w->title);
    SGP_LOG("SetWindowTextW(0x%x, \"%s\")", w->h, w->title);
    SG_RETURN_STDCALL(1, 8);
}

/* ---- relations, focus, activation --------------------------------------------------------------- */
SG_IMPORT(user32, GetAncestor) { /* (hwnd, GA_PARENT 1 / GA_ROOT 2 / GA_ROOTOWNER 3) */
    uint32_t h = SG_ARG(0), how = SG_ARG(1);
    sgp_win *w = sgp_win_find(h);
    if (!w) SG_RETURN_STDCALL(0, 8);
    SG_RETURN_STDCALL(how == 1 ? (w->toplevel ? DESKTOP_HWND : w->parent) : h, 8);
}
SG_IMPORT(user32, GetMenu) { SG_RETURN_STDCALL(0, 4); }
SG_IMPORT(user32, GetDesktopWindow) { SG_RETURN_STDCALL(DESKTOP_HWND, 0); }
SG_IMPORT(user32, GetForegroundWindow) { SG_RETURN_STDCALL(sp_get_active(), 0); }
SG_IMPORT(user32, GetActiveWindow) { SG_RETURN_STDCALL(sp_get_active(), 0); }
SG_IMPORT(user32, SetActiveWindow) {
    uint32_t h = SG_ARG(0), old = sp_get_active();
    if (!sgp_win_find(h)) SG_RETURN_STDCALL(0, 4);
    SGP_ENTER();
    if (old != h) {
        if (old) sp_window_activate(old, 0, 0);
        sp_window_activate(h, 1, 0);
    }
    SGP_LEAVE();
    SG_RETURN_STDCALL(old, 4);
}
SG_IMPORT(user32, SetFocus) {
    uint32_t h = SG_ARG(0), old;
    if (h && !sgp_win_find(h)) SG_RETURN_STDCALL(0, 4);
    SGP_ENTER();
    old = sp_set_focus(h);
    SGP_LEAVE();
    SG_RETURN_STDCALL(old, 4);
}
SG_IMPORT(user32, GetWindowThreadProcessId) {
    uint32_t pid = SG_ARG(1);
    /* The windows belong to the guest main thread; the TEB holds its ids (ClientId at +0x20/+0x24). */
    uint32_t tid = sg_self ? sg_ld32(mem, sg_self->ctx.fs_base + 0x24) : 0;
    if (pid) sg_st32(mem, pid, sg_self ? sg_ld32(mem, sg_self->ctx.fs_base + 0x20) : 0);
    SG_RETURN_STDCALL(sgp_win_find(SG_ARG(0)) ? tid : 0, 8);
}

/* ---- metrics, system parameters, monitors -------------------------------------------------------- */
SG_IMPORT(user32, GetSystemMetrics) {
    int32_t i = (int32_t)SG_ARG(0), sw, sh, r;
    sp_rect work;
    sgp_ensure();
    sgp_be->screen(&sw, &sh, &work, NULL, NULL, NULL);
    switch (i) {
    case 0: case 78: r = sw; break;                         /* SM_CXSCREEN, SM_CXVIRTUALSCREEN */
    case 1: case 79: r = sh; break;
    case 4: r = sgp_caption_h; break;                       /* SM_CYCAPTION */
    case 5: case 6: case 7: case 8: case 32: case 33: r = 0; break; /* borders and frames: none on macOS */
    case 11: case 12: r = 32; break;                        /* icons */
    case 13: case 14: r = 32; break;                        /* cursors */
    case 16: r = work.right - work.left; break;             /* SM_CXFULLSCREEN */
    case 17: r = work.bottom - work.top - sgp_caption_h; break;
    case 19: case 75: r = 1; break;                         /* mouse, wheel present */
    case 23: r = 0; break;                                  /* buttons not swapped */
    case 36: case 37: r = 4; break;                         /* double-click rectangle */
    case 43: r = 3; break;                                  /* mouse buttons */
    case 76: case 77: r = 0; break;                         /* virtual screen origin */
    case 80: r = 1; break;                                  /* SM_CMONITORS */
    case 81: r = 1; break;                                  /* SM_SAMEDISPLAYFORMAT */
    default: r = 0; break;                                  /* SM_REMOTESESSION and the rest */
    }
    SGP_TRACE("GetSystemMetrics(%d) = %d", i, r);
    SG_RETURN_STDCALL((uint32_t)r, 4);
}

SG_IMPORT(user32, SystemParametersInfoA) {
    uint32_t act = SG_ARG(0), pv = SG_ARG(2);
    sp_rect work;
    sgp_ensure();
    switch (act) {
    case 0x30: /* SPI_GETWORKAREA */
        sgp_be->screen(NULL, NULL, &work, NULL, NULL, NULL);
        if (pv) put_rect(mem, pv, work.left, work.top, work.right, work.bottom);
        break;
    case 0x68: if (pv) sg_st32(mem, pv, 3); break;              /* SPI_GETWHEELSCROLLLINES */
    case 0x101a: case 0x5e: case 0x101c: case 0x10: case 0x72: /* cursor shadow, trails, sonar, screensaver */
        if (pv) sg_st32(mem, pv, 0);
        break;
    default: break;                                             /* the SETs (0x101b, 0x5d, 0x101d): accepted */
    }
    SGP_TRACE("SystemParametersInfoA(0x%x, %u, 0x%x)", act, SG_ARG(1), pv);
    SG_RETURN_STDCALL(1, 16);
}

SG_IMPORT(user32, MonitorFromWindow) { SG_RETURN_STDCALL(MONITOR_HANDLE, 8); }

/* MONITORINFO (40) / MONITORINFOEXA (72: + szDevice[32]); the game passes 0x48 at 0x75daa5. */
SG_IMPORT(user32, GetMonitorInfoA) {
    uint32_t mi = SG_ARG(1), cb;
    int32_t sw, sh;
    sp_rect work;
    if (!mi) SG_RETURN_STDCALL(0, 8);
    cb = sg_ld32(mem, mi);
    if (cb != 40 && cb != 72) SG_RETURN_STDCALL(0, 8);
    sgp_ensure();
    sgp_be->screen(&sw, &sh, &work, NULL, NULL, NULL);
    put_rect(mem, mi + 4, 0, 0, sw, sh);
    put_rect(mem, mi + 20, work.left, work.top, work.right, work.bottom);
    sg_st32(mem, mi + 36, 1); /* MONITORINFOF_PRIMARY */
    if (cb == 72) {
        static const char dev[] = "\\\\.\\DISPLAY1";
        memset(mem + mi + 40, 0, 32);
        memcpy(mem + mi + 40, dev, sizeof dev);
    }
    SG_RETURN_STDCALL(1, 8);
}

/* BOOL EnumDisplayMonitors(HDC, LPCRECT clip, MONITORENUMPROC, LPARAM): one monitor. The callback
 * is guest code: BOOL CALLBACK (HMONITOR, HDC, LPRECT, LPARAM). */
SG_IMPORT(user32, EnumDisplayMonitors) {
    static uint32_t rect;
    uint32_t proc = SG_ARG(2), lp = SG_ARG(3), hdc = SG_ARG(0);
    int32_t sw, sh;
    if (!proc) SG_RETURN_STDCALL(0, 16);
    sgp_ensure();
    sgp_be->screen(&sw, &sh, NULL, NULL, NULL, NULL);
    if (!rect) rect = hle_guest_alloc(16, 16);
    if (!rect) SG_RETURN_STDCALL(0, 16);
    put_rect(mem, rect, 0, 0, sw, sh);
    {
        uint32_t args[4] = {MONITOR_HANDLE, hdc, rect, lp};
        SGP_ENTER();
        sgp_guest_call(ctx, proc, 4, args, "EnumDisplayMonitors callback");
        SGP_LEAVE();
    }
    SG_RETURN_STDCALL(1, 16);
}

/* ---- TrackMouseEvent (the window procedure's WM_MOUSEMOVE path, 0x847c80) -------------------------
 * BOOL TrackMouseEvent(LPTRACKMOUSEEVENT): {cbSize, dwFlags, hwndTrack, dwHoverTime}. TME_LEAVE (2)
 * arms one WM_MOUSELEAVE through the platform layer; TME_CANCEL (0x80000000) with TME_LEAVE disarms
 * it; TME_HOVER (1, WM_MOUSEHOVER) is accepted but not delivered [dec]; TME_QUERY (0x40000000) is
 * answered as "nothing tracked". */
SG_IMPORT(user32, TrackMouseEvent) {
    uint32_t p = SG_ARG(0), fl, h;
    if (!p || sg_ld32(mem, p) != 16) SG_RETURN_STDCALL(0, 4);
    fl = sg_ld32(mem, p + 4), h = sg_ld32(mem, p + 8);
    if (fl & 0x40000000u) {
        sg_st32(mem, p + 4, 0);
        SG_RETURN_STDCALL(1, 4);
    }
    if (!sgp_win_find(h)) SG_RETURN_STDCALL(0, 4);
    if ((fl & 2u) && !(fl & 0x80000000u)) sp_track_mouse_leave(h);
    SG_RETURN_STDCALL(1, 4);
}
