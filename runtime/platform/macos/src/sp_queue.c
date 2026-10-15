/*
 * sp_queue.c - the Win32 message queue for the macOS platform layer (portable C, pthreads).
 *
 * Retrieval order in sp_peek_message follows the PeekMessage documentation (Microsoft Learn,
 * "PeekMessageW function", Remarks): sent messages, posted messages, input (hardware) messages,
 * then WM_QUIT once nothing else is queued, then WM_PAINT, then WM_TIMER. WM_PAINT is generated
 * while a window's update region is non-empty and is not removed by PM_REMOVE; BeginPaint or
 * ValidateRect clear it. WM_QUIT is returned regardless of the message filter.
 *
 * The game drives frames from WM_PAINT: its pump
 * 0x8482f0 calls InvalidateRect(hwnd, NULL, FALSE) and then drains PeekMessageW, so each pass
 * ends with exactly one WM_PAINT after all posted and input messages.
 */
#include "sp_internal.h"

#include <pthread.h>
#include <stdio.h>
#include <stdlib.h>
#include <string.h>
#include <time.h>

#define MAX_WINDOWS 8
#define MAX_TIMERS 32
#define POSTED_LIMIT 10000   /* Windows' default per-queue limit for posted messages */
#define INPUT_LIMIT 4096

typedef struct win {
    int used;
    sp_hwnd h;
    sp_wndproc_fn proc;
    void *user;
    uint32_t style;
    int visible, invalid, erase;
    int32_t cx, cy, cw, ch;   /* last client rect reported (screen origin, size) */
    int track_leave;
} win;

typedef struct sent {
    sp_hwnd h; uint32_t msg, w; int32_t l;
    int32_t result; int done;
    struct sent *next;
} sent;

typedef struct timer { int used; sp_hwnd h; uint32_t id, elapse, due; } timer;

typedef struct deque { sp_qent *v; uint32_t head, n, cap; } deque;   /* live items: v[head .. head+n) */

static pthread_mutex_t g_lock = PTHREAD_MUTEX_INITIALIZER;
static pthread_cond_t  g_sent_cv = PTHREAD_COND_INITIALIZER;
static pthread_t g_owner;
static int g_inited;
static sp_host_hooks g_hooks;
static sp_input_config g_cfg = {SP_CMD_AS_CONTROL, 1, 1};
static win g_win[MAX_WINDOWS];
static timer g_timers[MAX_TIMERS];
static deque g_posted, g_input;
static sent *g_sent_head, *g_sent_tail;
static int g_quit_pending; static int32_t g_quit_code;
static sp_hwnd g_focus, g_active, g_capture;
static int g_paint_needs_visible;   /* hidden windows get no WM_PAINT, as on Windows */

/* last key-down retrieved, for TranslateMessage */
static sp_qent g_last_keydown; static int g_have_last_keydown;

/* delivery stack (window thread only) */
#define MAX_DEPTH 64
static int g_depth; static char g_kind[MAX_DEPTH];

/* scratch area for pointer-carrying messages */
static uint8_t *g_scratch_host; static uint32_t g_scratch_guest, g_scratch_size, g_scratch_top;

uint8_t sp__async[256], sp__sync[256], sp__async_pressed[256];

/* ---- helpers ------------------------------------------------------------------------------ */
static uint32_t default_now_ms(void)
{
    return (uint32_t)(clock_gettime_nsec_np(CLOCK_UPTIME_RAW) / 1000000ull);
}
uint32_t sp_tick_ms(void) { return g_hooks.now_ms ? g_hooks.now_ms(g_hooks.ctx) : default_now_ms(); }

int sp_is_owner_thread(void) { return g_inited && pthread_equal(pthread_self(), g_owner); }

static win *find_win(sp_hwnd h)
{
    if (!h) return NULL;
    for (int i = 0; i < MAX_WINDOWS; i++) if (g_win[i].used && g_win[i].h == h) return &g_win[i];
    return NULL;
}

static int dq_push(deque *d, const sp_qent *e, uint32_t limit)
{
    if (d->n >= limit) return 0;
    if (d->head + d->n == d->cap) {
        if (d->head > d->cap / 2) {                     /* plenty of room at the front: compact */
            memmove(d->v, d->v + d->head, d->n * sizeof d->v[0]);
            d->head = 0;
        } else {
            uint32_t nc = d->cap ? d->cap * 2 : 64;
            sp_qent *nv = realloc(d->v, nc * sizeof *nv);
            if (!nv) return 0;
            d->v = nv; d->cap = nc;
        }
    }
    d->v[d->head + d->n++] = *e;
    return 1;
}
static sp_qent *dq_at(deque *d, uint32_t i) { return &d->v[d->head + i]; }
static void dq_erase(deque *d, uint32_t i)
{
    if (i == 0) { d->head++; d->n--; if (!d->n) d->head = 0; return; }
    memmove(dq_at(d, i), dq_at(d, i + 1), (d->n - i - 1) * sizeof d->v[0]);
    d->n--;
}

static int match(const sp_msg *m, sp_hwnd filt, uint32_t mn, uint32_t mx)
{
    if (filt == (sp_hwnd)-1) { if (m->hwnd) return 0; }
    else if (filt && m->hwnd != filt) return 0;
    if (mn == 0 && mx == 0) return 1;
    return m->message >= mn && m->message <= mx;
}
static int range_has(uint32_t msg, uint32_t mn, uint32_t mx)
{
    return (mn == 0 && mx == 0) || (msg >= mn && msg <= mx);
}

void sp__client_to_screen(sp_hwnd h, int32_t *x, int32_t *y)
{
    if (g_hooks.client_to_screen) g_hooks.client_to_screen(g_hooks.ctx, h, x, y);
}
void sp__screen_to_client(sp_hwnd h, int32_t *x, int32_t *y)
{
    if (g_hooks.screen_to_client) g_hooks.screen_to_client(g_hooks.ctx, h, x, y);
}
void sp_client_to_screen(sp_hwnd h, int32_t *x, int32_t *y) { sp__client_to_screen(h, x, y); }
void sp_screen_to_client(sp_hwnd h, int32_t *x, int32_t *y) { sp__screen_to_client(h, x, y); }
const sp_input_config *sp__cfg(void) { return &g_cfg; }
sp_hwnd sp__key_target(void) { return g_focus ? g_focus : g_active; }
int sp__window_dblclks(sp_hwnd h) { win *w = find_win(h); return w && (w->style & SP_STYLE_DBLCLKS); }

/* ---- setup ------------------------------------------------------------------------------- */
void sp_platform_init(const sp_host_hooks *hooks, const sp_input_config *cfg)
{
    pthread_mutex_lock(&g_lock);
    memset(g_win, 0, sizeof g_win);
    memset(g_timers, 0, sizeof g_timers);
    g_posted.n = g_input.n = 0; g_posted.head = g_input.head = 0;
    g_sent_head = g_sent_tail = NULL;
    g_quit_pending = 0;
    g_focus = g_active = g_capture = 0;
    g_have_last_keydown = 0;
    g_depth = 0;
    if (hooks) g_hooks = *hooks; else memset(&g_hooks, 0, sizeof g_hooks);
    if (cfg) g_cfg = *cfg;
    g_owner = pthread_self();
    g_inited = 1;
    pthread_mutex_unlock(&g_lock);
    sp__input_reset_state();
    memset(sp__async, 0, sizeof sp__async);
    memset(sp__sync, 0, sizeof sp__sync);
    sp_input_init_toggles(0);
}

void sp_platform_shutdown(void)
{
    pthread_mutex_lock(&g_lock);
    free(g_posted.v); free(g_input.v);
    memset(&g_posted, 0, sizeof g_posted); memset(&g_input, 0, sizeof g_input);
    g_inited = 0;
    pthread_mutex_unlock(&g_lock);
}

void sp_set_scratch(void *host, uint32_t guest_addr, uint32_t size)
{
    g_scratch_host = host; g_scratch_guest = guest_addr; g_scratch_size = size; g_scratch_top = 0;
}
void *sp_scratch_host(uint32_t a)
{
    if (!g_scratch_host || a < g_scratch_guest || a - g_scratch_guest >= g_scratch_size) return NULL;
    return g_scratch_host + (a - g_scratch_guest);
}

/* ---- calling window procedures ------------------------------------------------------------ */
static int32_t call_proc(sp_hwnd h, char kind, uint32_t msg, uint32_t w, int32_t l)
{
    win *wp = find_win(h);
    if (!wp || !wp->proc) return 0;
    sp_wndproc_fn proc = wp->proc; void *user = wp->user;
    if (g_depth < MAX_DEPTH) g_kind[g_depth] = kind;
    g_depth++;
    int32_t r = proc(user, h, msg, w, l);
    g_depth--;
    return r;
}

void sp_delivery_info(int *depth, char *kind)
{
    int d = g_depth - 1;
    if (depth) *depth = d < 0 ? 0 : d;
    if (kind) *kind = (d >= 0 && d < MAX_DEPTH) ? g_kind[d] : '?';
}

static void process_sent(void)
{
    for (;;) {
        pthread_mutex_lock(&g_lock);
        sent *s = g_sent_head;
        if (s) { g_sent_head = s->next; if (!g_sent_head) g_sent_tail = NULL; }
        pthread_mutex_unlock(&g_lock);
        if (!s) return;
        int32_t r = call_proc(s->h, 'S', s->msg, s->w, s->l);
        pthread_mutex_lock(&g_lock);
        s->result = r; s->done = 1;
        pthread_cond_broadcast(&g_sent_cv);
        pthread_mutex_unlock(&g_lock);
    }
}

int32_t sp_send_message(sp_hwnd h, uint32_t msg, uint32_t w, int32_t l)
{
    if (sp_is_owner_thread()) return call_proc(h, 'S', msg, w, l);
    if (!find_win(h)) return 0;
    sent s = {h, msg, w, l, 0, 0, NULL};
    pthread_mutex_lock(&g_lock);
    if (g_sent_tail) g_sent_tail->next = &s; else g_sent_head = &s;
    g_sent_tail = &s;
    pthread_mutex_unlock(&g_lock);
    if (g_hooks.wake) g_hooks.wake(g_hooks.ctx);
    pthread_mutex_lock(&g_lock);
    while (!s.done) pthread_cond_wait(&g_sent_cv, &g_lock);
    pthread_mutex_unlock(&g_lock);
    return s.result;
}

/* ---- windows ----------------------------------------------------------------------------- */
sp_hwnd sp_window_register(sp_wndproc_fn proc, void *user, uint32_t style)
{
    pthread_mutex_lock(&g_lock);
    for (int i = 0; i < MAX_WINDOWS; i++) {
        if (!g_win[i].used) {
            memset(&g_win[i], 0, sizeof g_win[i]);
            g_win[i].used = 1;
            g_win[i].h = 0x00010001u + 2u * (uint32_t)i;
            g_win[i].proc = proc; g_win[i].user = user; g_win[i].style = style;
            g_win[i].invalid = 1; g_win[i].erase = 1;     /* a new window starts invalid */
            sp_hwnd h = g_win[i].h;
            pthread_mutex_unlock(&g_lock);
            return h;
        }
    }
    pthread_mutex_unlock(&g_lock);
    return 0;
}

void sp_window_unregister(sp_hwnd h)
{
    pthread_mutex_lock(&g_lock);
    win *w = find_win(h);
    if (w) w->used = 0;
    if (g_focus == h) g_focus = 0;
    if (g_active == h) g_active = 0;
    if (g_capture == h) g_capture = 0;
    for (int i = 0; i < MAX_TIMERS; i++) if (g_timers[i].used && g_timers[i].h == h) g_timers[i].used = 0;
    pthread_mutex_unlock(&g_lock);
}

int sp_is_window(sp_hwnd h) { return find_win(h) != NULL; }

static void set_focus(sp_hwnd h)
{
    if (g_focus == h) return;
    sp_hwnd old = g_focus;
    g_focus = h;
    if (old) sp_send_message(old, WM_KILLFOCUS, h, 0);
    if (h) sp_send_message(h, WM_SETFOCUS, old, 0);
}
sp_hwnd sp_get_focus(void) { return g_focus; }
sp_hwnd sp_set_focus(sp_hwnd h)
{
    sp_hwnd old = g_focus;
    if (h && !find_win(h)) return 0;
    set_focus(h);
    return old;
}
sp_hwnd sp_get_active(void) { return g_active; }
int sp_window_visible(sp_hwnd h) { win *w = find_win(h); return w && w->visible; }
int sp_window_invalid(sp_hwnd h) { win *w = find_win(h); return w && w->invalid; }
void sp_set_paint_needs_visible(int on) { g_paint_needs_visible = on; }

void sp_window_activate(sp_hwnd h, int active, int app_change)
{
    if (!find_win(h)) return;
    /* Activation changes are rare and decide whether the game renders; always log them. */
    {   /* SPORE_FORCE_ACTIVE=1: the guest window stays active while the Mac window is not key, so an
         * unattended or background run keeps rendering (the game stops drawing frames while inactive,
         * docs/design/platform-macos.md 10.11). The Mac side (cursor release, key state) still follows the key window. */
        const char *f = getenv("SPORE_FORCE_ACTIVE");
        int force = f && *f && *f != '0';
        fprintf(stderr, "platform: window 0x%x %s%s at %u ms%s\n", (unsigned)h, active ? "activated" : "deactivated",
                app_change ? " (app)" : "", (unsigned)sp_tick_ms(), !active && force ? " (ignored: SPORE_FORCE_ACTIVE)" : "");
        if (!active && force) { sp__input_reset_state(); return; }
        if (active && force && g_active == h) return;
    }
    if (active) {
        g_active = h;
        if (app_change) sp_send_message(h, WM_ACTIVATEAPP, 1, 0);
        sp_send_message(h, WM_NCACTIVATE, 1, 0);
        sp_send_message(h, WM_ACTIVATE, WA_ACTIVE, 0);   /* DefWindowProc sets the focus */
    } else {
        sp_send_message(h, WM_NCACTIVATE, 0, 0);
        sp_send_message(h, WM_ACTIVATE, WA_INACTIVE, 0);
        if (app_change) sp_send_message(h, WM_ACTIVATEAPP, 0, 0);
        if (g_active == h) g_active = 0;
        if (g_focus == h) { g_focus = 0; sp_send_message(h, WM_KILLFOCUS, 0, 0); }
        /* Keys released while another app has focus never reach us: forget them (state only). */
        sp__input_reset_state();
    }
}

void sp_show_window(sp_hwnd h, int show, int activate)
{
    win *w = find_win(h);
    if (!w) return;
    if (!!w->visible == !!show) return;
    w->visible = show;
    sp_send_message(h, WM_SHOWWINDOW, show ? 1 : 0, 0);
    if (show && activate) sp_window_activate(h, 1, 1);
}

void sp_update_window(sp_hwnd h)
{
    win *w = find_win(h);
    if (w && w->invalid && (w->visible || !g_paint_needs_visible)) sp_send_message(h, WM_PAINT, 0, 0);
}

int sp_destroy_window(sp_hwnd h)
{
    if (!find_win(h)) return 0;
    sp_send_message(h, WM_DESTROY, 0, 0);
    if (g_hooks.destroy_window) g_hooks.destroy_window(g_hooks.ctx, h);
    sp_window_unregister(h);
    return 1;
}

void sp_get_last_client(sp_hwnd h, int32_t *x, int32_t *y, int32_t *cw, int32_t *ch)
{
    win *w = find_win(h);
    if (!w) return;
    if (x) *x = w->cx; if (y) *y = w->cy; if (cw) *cw = w->cw; if (ch) *ch = w->ch;
}

void sp_window_rect_changed(sp_hwnd h, int32_t x, int32_t y, int32_t cw, int32_t ch)
{
    win *w = find_win(h);
    if (!w) return;
    int moved = (x != w->cx || y != w->cy), sized = (cw != w->cw || ch != w->ch);
    w->cx = x; w->cy = y; w->cw = cw; w->ch = ch;
    if (!w->visible) return;                 /* before ShowWindow: record silently */
    if (moved) sp_send_message(h, WM_MOVE, 0, sp__makelparam(x, y));
    if (sized) {
        sp_send_message(h, WM_SIZE, SIZE_RESTORED, sp__makelparam(cw, ch));
        sp_invalidate_rect(h, 1);
    }
}

void sp_window_set_minimized(sp_hwnd h, int minimized)
{
    win *w = find_win(h);
    if (!w) return;
    if (minimized) sp_send_message(h, WM_SIZE, SIZE_MINIMIZED, 0);
    else { sp_send_message(h, WM_SIZE, SIZE_RESTORED, sp__makelparam(w->cw, w->ch)); sp_invalidate_rect(h, 1); }
}

void sp_window_close_request(sp_hwnd h) { if (find_win(h)) sp_send_message(h, WM_CLOSE, 0, 0); }

void sp_window_enter_size_move(sp_hwnd h, int enter)
{
    if (find_win(h)) sp_send_message(h, enter ? WM_ENTERSIZEMOVE : WM_EXITSIZEMOVE, 0, 0);
}

void sp_window_display_change(sp_hwnd h, int bpp, int sw, int sh)
{
    if (find_win(h)) sp_send_message(h, WM_DISPLAYCHANGE, (uint32_t)bpp, sp__makelparam(sw, sh));
}

int sp_window_get_minmax(sp_hwnd h, sp_minmaxinfo *io)
{
    if (!find_win(h) || !g_scratch_host || g_scratch_top + sizeof *io > g_scratch_size) return 0;
    uint32_t off = g_scratch_top;
    g_scratch_top += (uint32_t)((sizeof *io + 15) & ~15u);
    memcpy(g_scratch_host + off, io, sizeof *io);
    sp_send_message(h, WM_GETMINMAXINFO, 0, (int32_t)(g_scratch_guest + off));
    memcpy(io, g_scratch_host + off, sizeof *io);
    g_scratch_top = off;
    return 1;
}

/* ---- painting and timers ------------------------------------------------------------------ */
void sp_invalidate_rect(sp_hwnd h, int erase)
{
    pthread_mutex_lock(&g_lock);
    for (int i = 0; i < MAX_WINDOWS; i++) {
        if (!g_win[i].used || (h && g_win[i].h != h)) continue;
        g_win[i].invalid = 1;
        if (erase) g_win[i].erase = 1;
    }
    pthread_mutex_unlock(&g_lock);
}

void sp_validate_rect(sp_hwnd h)
{
    pthread_mutex_lock(&g_lock);
    win *w = find_win(h);
    if (w) { w->invalid = 0; w->erase = 0; }
    pthread_mutex_unlock(&g_lock);
}

uint32_t sp_begin_paint(sp_hwnd h, sp_paintstruct *ps)
{
    win *w = find_win(h);
    if (!w) return 0;
    pthread_mutex_lock(&g_lock);
    int erase = w->erase;
    w->invalid = 0; w->erase = 0;
    pthread_mutex_unlock(&g_lock);
    int32_t erased = 0;
    if (erase) erased = sp_send_message(h, WM_ERASEBKGND, SP_FAKE_HDC, 0);
    if (ps) {
        memset(ps, 0, sizeof *ps);
        ps->hdc = SP_FAKE_HDC;
        ps->fErase = erase && !erased;
        ps->rcPaint.right = w->cw; ps->rcPaint.bottom = w->ch;
    }
    return SP_FAKE_HDC;
}
void sp_end_paint(sp_hwnd h, const sp_paintstruct *ps) { (void)h; (void)ps; }

uint32_t sp_set_timer(sp_hwnd h, uint32_t id, uint32_t elapse)
{
    if (elapse < 10) elapse = 10;           /* USER_TIMER_MINIMUM */
    uint32_t now = sp_tick_ms();
    pthread_mutex_lock(&g_lock);
    timer *free_slot = NULL;
    for (int i = 0; i < MAX_TIMERS; i++) {
        timer *t = &g_timers[i];
        if (t->used && t->h == h && t->id == id) { t->elapse = elapse; t->due = now + elapse; pthread_mutex_unlock(&g_lock); return id ? id : 1; }
        if (!t->used && !free_slot) free_slot = t;
    }
    if (free_slot) *free_slot = (timer){1, h, id, elapse, now + elapse};
    pthread_mutex_unlock(&g_lock);
    return free_slot ? (id ? id : 1) : 0;   /* nonzero on success, also for id 0 (SetTimer at 0x849d6e) */
}

int sp_kill_timer(sp_hwnd h, uint32_t id)
{
    pthread_mutex_lock(&g_lock);
    for (int i = 0; i < MAX_TIMERS; i++)
        if (g_timers[i].used && g_timers[i].h == h && g_timers[i].id == id) { g_timers[i].used = 0; pthread_mutex_unlock(&g_lock); return 1; }
    pthread_mutex_unlock(&g_lock);
    return 0;
}

/* ---- capture -------------------------------------------------------------------------------- */
sp_hwnd sp_set_capture(sp_hwnd h)
{
    sp_hwnd old = g_capture;
    g_capture = h;
    if (old && old != h) sp_send_message(old, WM_CAPTURECHANGED, 0, (int32_t)h);
    return old;
}
int sp_release_capture(void)
{
    sp_hwnd old = g_capture;
    g_capture = 0;
    if (old) sp_send_message(old, WM_CAPTURECHANGED, 0, 0);
    return 1;
}
sp_hwnd sp_get_capture(void) { return g_capture; }

int sp_track_mouse_leave(sp_hwnd h)
{
    win *w = find_win(h);
    if (!w) return 0;
    w->track_leave = 1;
    return 1;
}
void sp_input_mouse_exited(sp_hwnd h)
{
    win *w = find_win(h);
    if (!w || !w->track_leave) return;
    w->track_leave = 0;
    sp_qent e; memset(&e, 0, sizeof e);
    e.m.hwnd = h; e.m.message = WM_MOUSELEAVE; e.m.time = sp_tick_ms();
    sp__enqueue_input(&e);
}

/* ---- the queue ------------------------------------------------------------------------------ */
int sp_post_message(sp_hwnd h, uint32_t msg, uint32_t w, int32_t l)
{
    if (h && !find_win(h)) return 0;
    sp_qent e; memset(&e, 0, sizeof e);
    e.m.hwnd = h; e.m.message = msg; e.m.wParam = w; e.m.lParam = l; e.m.time = sp_tick_ms();
    pthread_mutex_lock(&g_lock);
    int ok = dq_push(&g_posted, &e, POSTED_LIMIT);
    pthread_mutex_unlock(&g_lock);
    if (ok && !sp_is_owner_thread() && g_hooks.wake) g_hooks.wake(g_hooks.ctx);
    return ok;
}

void sp_post_quit_message(int32_t code)
{
    pthread_mutex_lock(&g_lock);
    g_quit_pending = 1; g_quit_code = code;
    pthread_mutex_unlock(&g_lock);
}

void sp__enqueue_input(const sp_qent *e)
{
    pthread_mutex_lock(&g_lock);
    if (e->m.message == WM_MOUSEMOVE && g_input.n) {
        sp_qent *t = dq_at(&g_input, g_input.n - 1);
        if (t->m.message == WM_MOUSEMOVE && t->m.hwnd == e->m.hwnd && t->m.wParam == e->m.wParam) {
            *t = *e;                         /* Windows keeps only the latest mouse move */
            pthread_mutex_unlock(&g_lock);
            return;
        }
    }
    dq_push(&g_input, e, INPUT_LIMIT);
    pthread_mutex_unlock(&g_lock);
}

uint32_t sp_queue_depth(int which)
{
    pthread_mutex_lock(&g_lock);
    uint32_t n = which ? g_input.n : g_posted.n;
    pthread_mutex_unlock(&g_lock);
    return n;
}

static void on_input_retrieved(const sp_qent *e)
{
    if (e->key_change) sp__keystate_apply(sp__sync, e->key_vk, e->key_vk_specific, e->key_change > 0);
    uint32_t m = e->m.message;
    if (m == WM_KEYDOWN || m == WM_SYSKEYDOWN) { g_last_keydown = *e; g_have_last_keydown = 1; }
    if (e->is_mouse && !g_capture && g_cfg.send_nchittest) {
        int32_t sx = (int16_t)(e->m.lParam & 0xFFFF), sy = (int16_t)((uint32_t)e->m.lParam >> 16);
        sp__client_to_screen(e->m.hwnd, &sx, &sy);
        int32_t ht = sp_send_message(e->m.hwnd, WM_NCHITTEST, 0, sp__makelparam(sx, sy));
        sp_send_message(e->m.hwnd, WM_SETCURSOR, e->m.hwnd, sp__makelparam(ht, (int32_t)m));
    }
}

int sp_peek_message(sp_msg *out, sp_hwnd filt, uint32_t mn, uint32_t mx, uint32_t flags)
{
    if (!sp_is_owner_thread()) { fprintf(stderr, "sp_peek_message: not the owner thread\n"); return 0; }
    if (g_hooks.pump_events) g_hooks.pump_events(g_hooks.ctx);
    process_sent();
    int remove = flags & PM_REMOVE;
    sp_qent e;

    pthread_mutex_lock(&g_lock);
    for (uint32_t i = 0; i < g_posted.n; i++) {
        if (!match(&dq_at(&g_posted, i)->m, filt, mn, mx)) continue;
        e = *dq_at(&g_posted, i);
        if (remove) dq_erase(&g_posted, i);
        pthread_mutex_unlock(&g_lock);
        *out = e.m;
        return 1;
    }
    for (uint32_t i = 0; i < g_input.n; i++) {
        if (!match(&dq_at(&g_input, i)->m, filt, mn, mx)) continue;
        e = *dq_at(&g_input, i);
        if (remove) dq_erase(&g_input, i);
        pthread_mutex_unlock(&g_lock);
        if (remove) on_input_retrieved(&e);
        *out = e.m;
        return 1;
    }
    uint32_t now = sp_tick_ms();
    if (g_quit_pending) {
        memset(out, 0, sizeof *out);
        out->message = WM_QUIT; out->wParam = (uint32_t)g_quit_code; out->time = now;
        if (remove) g_quit_pending = 0;
        pthread_mutex_unlock(&g_lock);
        return 1;
    }
    if (range_has(WM_PAINT, mn, mx)) {
        for (int i = 0; i < MAX_WINDOWS; i++) {
            win *w = &g_win[i];
            if (!w->used || !w->invalid || (g_paint_needs_visible && !w->visible) || (filt && filt != (sp_hwnd)-1 && filt != w->h) || filt == (sp_hwnd)-1) continue;
            memset(out, 0, sizeof *out);
            out->hwnd = w->h; out->message = WM_PAINT; out->time = now;
            pthread_mutex_unlock(&g_lock);
            return 1;
        }
    }
    if (range_has(WM_TIMER, mn, mx)) {
        for (int i = 0; i < MAX_TIMERS; i++) {
            timer *t = &g_timers[i];
            if (!t->used || (int32_t)(now - t->due) < 0 || (filt && filt != t->h)) continue;
            memset(out, 0, sizeof *out);
            out->hwnd = t->h; out->message = WM_TIMER; out->wParam = t->id; out->time = now;
            if (remove) t->due = now + t->elapse;
            pthread_mutex_unlock(&g_lock);
            return 1;
        }
    }
    pthread_mutex_unlock(&g_lock);
    return 0;
}

int sp_translate_message(const sp_msg *m)
{
    if (m->message != WM_KEYDOWN && m->message != WM_SYSKEYDOWN) return 0;
    if (!g_have_last_keydown) return 1;
    const sp_qent *k = &g_last_keydown;
    if (k->m.message != m->message || k->m.wParam != m->wParam || k->m.lParam != m->lParam || k->m.hwnd != m->hwnd)
        return 1;
    g_have_last_keydown = 0;
    for (int i = 0; i < k->nchars; i++) {
        sp_qent c; memset(&c, 0, sizeof c);
        c.m = k->m; c.m.message = k->charmsg; c.m.wParam = k->chars[i];
        pthread_mutex_lock(&g_lock);
        dq_push(&g_posted, &c, POSTED_LIMIT);
        pthread_mutex_unlock(&g_lock);
    }
    return 1;
}

int32_t sp_dispatch_message(const sp_msg *m)
{
    if (!m->hwnd) return 0;
    return call_proc(m->hwnd, 'D', m->message, m->wParam, m->lParam);
}

/* DefWindowProc's IME part (an unhandled WM_IME_COMPOSITION result becomes WM_IME_CHAR, an unhandled
 * WM_IME_CHAR becomes WM_CHAR) lives with the text-input state (sp_textinput.inc); this weak default is used
 * by binaries without it. */
__attribute__((weak)) int sp_ti_def_window_proc(sp_hwnd h, uint32_t msg, uint32_t w, int32_t l, int32_t *result)
{
    (void)h; (void)msg; (void)w; (void)l; (void)result;
    return 0;
}

int32_t sp_def_window_proc(sp_hwnd h, uint32_t msg, uint32_t w, int32_t l)
{
    win *wp = find_win(h);
    int32_t ime_r;
    if ((msg == 0x010F || msg == 0x0286) && sp_ti_def_window_proc(h, msg, w, l, &ime_r)) return ime_r;
    switch (msg) {
    case WM_NCHITTEST: {
        int32_t x = (int16_t)(l & 0xFFFF), y = (int16_t)((uint32_t)l >> 16);
        sp__screen_to_client(h, &x, &y);
        int32_t cw = wp ? wp->cw : 0, ch = wp ? wp->ch : 0;
        if (g_hooks.get_client_size) g_hooks.get_client_size(g_hooks.ctx, h, &cw, &ch);
        return (x >= 0 && y >= 0 && x < cw && y < ch) ? HTCLIENT : HTNOWHERE;
    }
    case WM_SETCURSOR: return 1;
    case WM_ACTIVATE:
        if ((w & 0xFFFF) != WA_INACTIVE && !(w >> 16)) set_focus(h);
        return 0;
    case WM_NCACTIVATE: return 1;
    case WM_RBUTTONUP: {
        int32_t x = (int16_t)(l & 0xFFFF), y = (int16_t)((uint32_t)l >> 16);
        sp__client_to_screen(h, &x, &y);
        sp_send_message(h, WM_CONTEXTMENU, h, sp__makelparam(x, y));
        return 0;
    }
    case WM_CLOSE: sp_destroy_window(h); return 0;
    case WM_PAINT: { sp_paintstruct ps; sp_begin_paint(h, &ps); sp_end_paint(h, &ps); return 0; }
    case WM_ERASEBKGND: return 1;
    default: return 0;
    }
}

/* ---- key state ------------------------------------------------------------------------------ */
static int sibling(int specific)
{
    switch (specific) {
    case VK_LSHIFT: return VK_RSHIFT; case VK_RSHIFT: return VK_LSHIFT;
    case VK_LCONTROL: return VK_RCONTROL; case VK_RCONTROL: return VK_LCONTROL;
    case VK_LMENU: return VK_RMENU; case VK_RMENU: return VK_LMENU;
    default: return 0;
    }
}

static void set_one(uint8_t *t, int vk, int down)
{
    if (down) { if (!(t[vk] & 0x80)) t[vk] ^= 0x01; t[vk] |= 0x80; }   /* toggle on each press */
    else t[vk] &= (uint8_t)~0x80;
}

void sp__keystate_apply(uint8_t *t, int vk, int vk_specific, int down)
{
    if (vk_specific && vk_specific != vk) {
        set_one(t, vk_specific, down);
        int sib = sibling(vk_specific);
        set_one(t, vk, down || (sib && (t[sib] & 0x80)));
    } else {
        set_one(t, vk, down);
    }
    if (t == sp__async && down) sp__async_pressed[vk] = sp__async_pressed[vk_specific ? vk_specific : vk] = 1;
}

int16_t sp_get_key_state(int vk)
{
    if (vk < 0 || vk > 255) return 0;
    return (int16_t)(int8_t)(sp__sync[vk] & 0x81);
}

int16_t sp_get_async_key_state(int vk)
{
    if (vk < 0 || vk > 255) return 0;
    int16_t r = (sp__async[vk] & 0x80) ? (int16_t)0x8000 : 0;
    if (sp__async_pressed[vk]) { r |= 1; sp__async_pressed[vk] = 0; }
    return r;
}

void sp_get_keyboard_state(uint8_t out[256]) { memcpy(out, sp__sync, 256); }
void sp_set_keyboard_state(const uint8_t in[256]) { memcpy(sp__sync, in, 256); }
