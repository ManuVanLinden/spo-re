/* sgp_core.c -- set-up, back-end selection, the guest window-procedure bridge, window and class
 * tables, the headless back end, and the registration check (docs/design/platform-macos.md
 * section 10). No game code. */
#include <pthread.h>
#include <stdlib.h>
#include <time.h>

#include "sgp_int.h"

sgp_win sgp_wins[SGP_MAX_WIN];
sgp_class sgp_classes[SGP_MAX_CLASS];
const sgp_backend *sgp_be;
int sgp_caption_h = 22;
int sgp_paint_hz;
SG_THREAD_LOCAL sg_ctx *sgp_tl_ctx;
void (*sgp_wndproc_trace)(uint32_t, uint32_t, uint32_t, int32_t, int, char);

static const sgp_backend *chosen;
static int inited;
static pthread_mutex_t init_lock = PTHREAD_MUTEX_INITIALIZER;

/* ---- headless back end -------------------------------------------------------------------------- */
static int32_t hl_cursor_x = 400, hl_cursor_y = 300, hl_cursor_count;
static uint16_t hl_clip[1024];
static uint32_t hl_clip_n;

static void hl_c2s(void *c, sp_hwnd h, int32_t *x, int32_t *y) {
    int32_t cx = 0, cy = 0;
    sp_get_last_client(h, &cx, &cy, NULL, NULL);
    *x += cx;
    *y += cy;
}
static void hl_s2c(void *c, sp_hwnd h, int32_t *x, int32_t *y) {
    int32_t cx = 0, cy = 0;
    sp_get_last_client(h, &cx, &cy, NULL, NULL);
    *x -= cx;
    *y -= cy;
}
static void hl_size(void *c, sp_hwnd h, int32_t *w, int32_t *hh) { sp_get_last_client(h, NULL, NULL, w, hh); }
static int hl_init(void) {
    sp_host_hooks hk;
    memset(&hk, 0, sizeof hk);
    hk.client_to_screen = hl_c2s;
    hk.screen_to_client = hl_s2c;
    hk.get_client_size = hl_size;
    sp_platform_init(&hk, NULL);
    return 0;
}
static void hl_attach(sp_hwnd h, const char *t, int32_t x, int32_t y, int32_t w, int32_t hh) {}
static void hl_detach(sp_hwnd h) {}
static void hl_rect(sp_hwnd h, int attached, int32_t x, int32_t y, int32_t w, int32_t hh) {
    if (x == INT32_MIN) x = (1024 - w) / 2;
    if (y == INT32_MIN) y = (768 - hh) / 2;
    sp_window_rect_changed(h, x, y, w, hh);
}
static int hl_show(sp_hwnd h, int show, int activate) { return 0; }
static void hl_title(sp_hwnd h, const char *s) {}
static int hl_iconic(sp_hwnd h) { return 0; }
static int32_t hl_caption(void) { return 22; }
static void hl_screen(int32_t *w, int32_t *h, sp_rect *work, int32_t *hz, int32_t *mmw, int32_t *mmh) {
    if (w) *w = 1024;
    if (h) *h = 768;
    if (work) { work->left = 0; work->top = 25; work->right = 1024; work->bottom = 768; }
    if (hz) *hz = 60;
    if (mmw) *mmw = 271; /* 1024 px at 96 dpi */
    if (mmh) *mmh = 203;
}
static void hl_getpos(int32_t *x, int32_t *y) {
    uint32_t h = sgp_main_window();
    if (h) { sp_get_cursor_client(h, x, y); sp_client_to_screen(h, x, y); return; } /* last mouse event */
    *x = hl_cursor_x;
    *y = hl_cursor_y;
}
static void hl_setpos(int32_t x, int32_t y) { hl_cursor_x = x; hl_cursor_y = y; }
static int hl_showc(int show) { return hl_cursor_count += show ? 1 : -1; }
static void hl_shape(uint32_t idc) {}
static uint32_t hl_clip_get(uint16_t *b, uint32_t cap) {
    uint32_t n = hl_clip_n < cap ? hl_clip_n : cap;
    memcpy(b, hl_clip, 2u * n);
    return n;
}
static int hl_clip_set(const uint16_t *s, uint32_t n) {
    if (n > 1024) n = 1024;
    memcpy(hl_clip, s, 2u * n);
    hl_clip_n = n;
    return 1;
}
static int hl_mbox(const char *text, const char *cap, uint32_t type) {
    hle_log(HLE_LOG_WARN, "MessageBox(\"%s\", \"%s\", 0x%x) -> default button (headless)", text, cap, type);
    return (type & 0xf) == 4 ? 6 /* IDYES */ : 1 /* IDOK */;
}
static uint32_t hl_dbl(void) { return 500; }
static void *hl_layer(sp_hwnd h) { return NULL; }

const sgp_backend sgp_backend_headless = {
    "headless", hl_init, hl_attach, hl_detach, hl_rect, hl_show, hl_title, hl_iconic, hl_caption, hl_screen,
    hl_getpos, hl_setpos, hl_showc, hl_shape, hl_clip_get, hl_clip_set, hl_mbox, hl_dbl, hl_layer,
    NULL, /* set_fullscreen: no fullscreen headless */
};

__attribute__((weak)) const sgp_backend *sgp_default_backend(void) { return &sgp_backend_headless; }

void sgp_set_backend(const sgp_backend *b) { chosen = b; }
const sgp_backend *sgp_backend_in_use(void) { return sgp_be; }

int sgp_ensure(void) {
    int r = 0;
    if (__atomic_load_n(&inited, __ATOMIC_ACQUIRE)) return 0;
    pthread_mutex_lock(&init_lock);
    if (!inited) {
        const char *e = getenv("SPORE_HEADLESS"), *hz = getenv("SPORE_PAINT_HZ");
        sgp_be = chosen ? chosen : (e && *e && *e != '0') ? &sgp_backend_headless : sgp_default_backend();
        if (!pthread_main_np())
            hle_log(HLE_LOG_WARN, "platform: first window import on a thread that is not the process main thread");
        r = sgp_be->init();
        sp_set_paint_needs_visible(1);
        sgp_caption_h = sgp_be->caption_height();
        if (hz) sgp_paint_hz = atoi(hz);
        {
            unsigned bad = 0, n = sgp_register_shims(&bad); /* the table is installed by now */
            if (bad) hle_log(HLE_LOG_ERROR, "platform: %u of %u imports are not wired to this package", bad, n);
        }
        hle_log(HLE_LOG_INFO, "platform: %s back end%s (caption %d pt, paint cadence %d Hz)", sgp_be->name,
                r ? " FAILED to initialise" : "", sgp_caption_h, sgp_paint_hz);
        __atomic_store_n(&inited, 1, __ATOMIC_RELEASE);
    }
    pthread_mutex_unlock(&init_lock);
    return r;
}

/* ---- guest window procedures --------------------------------------------------------------------- */
static void barrier_noop(sg_cleanup *c) { (void)c; }

uint32_t sgp_guest_call(sg_ctx *ctx, uint32_t proc, unsigned n, const uint32_t *args, const char *what) {
    sg_cleanup c = {NULL, barrier_noop, 1, what};
    uint32_t r;
    sg_cleanup_push(ctx, &c);
    r = sg_call_guest(ctx, proc, SG_CC_STDCALL, n, args);
    sg_cleanup_pop(ctx, &c);
    return r;
}

int32_t sgp_call_wndproc(uint32_t proc, uint32_t hwnd, uint32_t msg, uint32_t wparam, int32_t lparam) {
    sg_ctx *ctx = sgp_tl_ctx ? sgp_tl_ctx : (sg_self ? &sg_self->ctx : NULL);
    uint32_t args[4] = {hwnd, msg, wparam, (uint32_t)lparam};
    if (!ctx) {
        hle_log(HLE_LOG_ERROR, "window procedure 0x%08x for msg 0x%x called on a thread with no guest context", proc,
                msg);
        abort();
    }
    if (!proc) return 0;
    if (sgp_wndproc_trace) {
        int depth;
        char kind;
        sp_delivery_info(&depth, &kind);
        sgp_wndproc_trace(hwnd, msg, wparam, lparam, depth, kind);
    }
    return (int32_t)sgp_guest_call(ctx, proc, 4, args, "guest window procedure (DispatchMessage/SendMessage)");
}

/* ---- tables --------------------------------------------------------------------------------------- */
sgp_win *sgp_win_find(uint32_t h) {
    unsigned i;
    if (!h) return NULL;
    for (i = 0; i < SGP_MAX_WIN; i++)
        if (sgp_wins[i].used && sgp_wins[i].h == h) {
            if (!sp_is_window(h)) { sgp_wins[i].used = 0; return NULL; } /* destroyed by DefWindowProc(WM_CLOSE) */
            return &sgp_wins[i];
        }
    return NULL;
}

sgp_class *sgp_class_find_name(const char *lower) {
    unsigned i;
    for (i = 0; i < SGP_MAX_CLASS; i++)
        if (sgp_classes[i].used && !strcmp(sgp_classes[i].name, lower)) return &sgp_classes[i];
    return NULL;
}

sgp_class *sgp_class_find_atom(uint16_t atom) {
    unsigned i;
    for (i = 0; i < SGP_MAX_CLASS; i++)
        if (sgp_classes[i].used && sgp_classes[i].atom == atom) return &sgp_classes[i];
    return NULL;
}

int32_t sgp_frame_top(uint32_t style, uint32_t exstyle) {
    (void)exstyle;
    return (style & 0x00C00000u) == 0x00C00000u ? sgp_caption_h : 0; /* WS_CAPTION; no side borders on macOS */
}

/* ---- strings ------------------------------------------------------------------------------------- */
static size_t put_utf8(char *out, size_t cap, size_t o, uint32_t c) {
    char b[4];
    size_t n, i;
    if (c < 0x80) { b[0] = (char)c; n = 1; }
    else if (c < 0x800) { b[0] = (char)(0xC0 | c >> 6); b[1] = (char)(0x80 | (c & 0x3F)); n = 2; }
    else if (c < 0x10000) { b[0] = (char)(0xE0 | c >> 12); b[1] = (char)(0x80 | ((c >> 6) & 0x3F)); b[2] = (char)(0x80 | (c & 0x3F)); n = 3; }
    else { b[0] = (char)(0xF0 | c >> 18); b[1] = (char)(0x80 | ((c >> 12) & 0x3F)); b[2] = (char)(0x80 | ((c >> 6) & 0x3F)); b[3] = (char)(0x80 | (c & 0x3F)); n = 4; }
    if (o + n >= cap) return o;
    for (i = 0; i < n; i++) out[o + i] = b[i];
    return o + n;
}

size_t sgp_wstr_utf8(const uint8_t *mem, uint32_t a, char *out, size_t cap) {
    size_t o = 0, i;
    if (!cap) return 0;
    if (a)
        for (i = 0; i < 4096; i++) {
            uint32_t u = sg_ld16(mem, a + 2u * (uint32_t)i);
            if (!u) break;
            if (u >= 0xD800 && u < 0xDC00) {
                uint32_t v = sg_ld16(mem, a + 2u * (uint32_t)(i + 1));
                if (v >= 0xDC00 && v < 0xE000) { u = 0x10000 + ((u - 0xD800) << 10) + (v - 0xDC00); i++; }
            }
            size_t n = put_utf8(out, cap, o, u);
            if (n == o) break;
            o = n;
        }
    out[o] = 0;
    return o;
}

size_t sgp_astr(const uint8_t *mem, uint32_t a, char *out, size_t cap) {
    size_t o = 0, i;
    if (!cap) return 0;
    if (a)
        for (i = 0; i < 4096; i++) {
            uint8_t c = mem[a + i];
            if (!c) break;
            size_t n = put_utf8(out, cap, o, c); /* Latin-1 for bytes >= 0x80 (close to CP1252) */
            if (n == o) break;
            o = n;
        }
    out[o] = 0;
    return o;
}

void sgp_lower(char *s) {
    for (; *s; s++)
        if (*s >= 'A' && *s <= 'Z') *s = (char)(*s + 32);
}

uint32_t sgp_pseudo_handle(void) {
    static uint32_t next = 0x00e10000u;
    return __atomic_add_fetch(&next, 4u, __ATOMIC_RELAXED);
}

/* ---- registration check --------------------------------------------------------------------------- */
unsigned sgp_register_shims(unsigned *bad) {
    unsigned i, k, n = 0, b = 0;
    for (k = 0; k < sgp_shim_count; k++) {
        const sgp_shim *s = &sgp_shims[k];
        int found = 0;
        for (i = 0; i < hle_import_count; i++) {
            if (hle_imports[i].fn == s->fn) {
                found = 1;
                if (hle_imports[i].argbytes != s->cleanup) {
                    hle_log(HLE_LOG_ERROR, "platform: %s cleans %u bytes, the import table says %u", s->symbol, s->cleanup,
                            hle_imports[i].argbytes);
                    b++;
                }
            }
        }
        for (i = 0; i < hle_dynamic_count; i++)
            if (hle_dynamics[i].fn == s->fn) {
                found = 1;
                if (hle_dynamics[i].argbytes != s->cleanup) {
                    hle_log(HLE_LOG_ERROR, "platform: %s cleans %u bytes, the dynamic import table says %u", s->symbol,
                            s->cleanup, hle_dynamics[i].argbytes);
                    b++;
                }
            }
        if (!found) {
            hle_log(HLE_LOG_ERROR, "platform: %s is not reachable from the import table", s->symbol);
            b++;
        }
        n++;
    }
    if (bad) *bad = b;
    hle_log(HLE_LOG_INFO, "platform: %u imports checked against the table, %u problems", n, b);
    return n;
}

uint32_t sgp_call_import(sg_ctx *ctx, const char *name, unsigned n, const uint32_t *args) {
    uint8_t *mem = ctx->mem;
    uint32_t esp0 = ctx->esp, eip0 = ctx->eip;
    unsigned i, k;
    for (i = 0; i < hle_import_count; i++)
        if (hle_imports[i].name && !strcmp(hle_imports[i].name, name) && hle_imports[i].fn) break;
    if (i == hle_import_count) {
        hle_log(HLE_LOG_ERROR, "platform: import %s not in the table", name);
        return 0;
    }
    for (k = n; k-- > 0;) { ctx->esp -= 4; sg_st32(mem, ctx->esp, args[k]); }
    ctx->esp -= 4;
    sg_st32(mem, ctx->esp, SG_RET_TO_HOST);
    hle_imports[i].fn(ctx, mem);
    ctx->esp = esp0; /* stdcall shims pop their own arguments; restore regardless */
    ctx->eip = eip0;
    return ctx->eax;
}
