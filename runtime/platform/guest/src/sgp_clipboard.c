/*
 * sgp_clipboard.c -- USER32 clipboard shims over NSPasteboard and the clipboard-viewer notification.
 * See sgp_clipboard.h for what the game does with them and how the pieces connect.
 *
 * Windows behaviour kept:
 *   - SetClipboardData copies the data out of the GlobalAlloc block (the block is the "system's" from then on;
 *     we leave it allocated, as the previous shim did, so a caller that passes some other pointer is safe).
 *   - GetClipboardData returns a GMEM_MOVEABLE block (GlobalAlloc; a handle is the pointer, w32_heap.c) sized
 *     exactly to the data plus its terminator, so GlobalSize works as the game expects (0x92b020). The block
 *     stays valid until the next GetClipboardData of the same format or the next EmptyClipboard.
 *   - CF_TEXT (1) is served from and written to the Unicode text, Latin-1 narrowed ('?' above U+00FF).
 *   - SetClipboardViewer returns the previous viewer and queues a first WM_DRAWCLIPBOARD (Windows sends one
 *     when a viewer joins the chain); ChangeClipboardChain unlinks.
 * Differences (docs/DEVIATIONS.md 4.9): WM_DRAWCLIPBOARD is posted (not sent) up to SPORE_CLIPBOARD_POLL_MS
 * after another application changes the pasteboard; delayed rendering (SetClipboardData(fmt, NULL)) fails.
 *
 * Environment: SPORE_CLIPBOARD=private keeps the clipboard inside the process (never touches the Mac's);
 * SPORE_CLIPBOARD=mac forces the pasteboard even with the headless back end (tests);
 * SPORE_CLIPBOARD_POLL_MS (default 250, 0 = no viewer notifications).
 *
 * No game code.
 */
#include <stdlib.h>
#include <time.h>

#include "sgp_int.h"
#include "sgp_clipboard.h"

#define CF_TEXT 1u
#define CF_UNICODETEXT 13u
#define WM_DRAWCLIPBOARD 0x308u
#define GMEM_MOVEABLE 0x2u
#define MAX_TEXT_UNITS (1u << 20)

/* ---- weak host stand-ins: "no pasteboard" (sp_clipboard.m not linked) ------------------------- */
__attribute__((weak)) long sp_mac_pb_seq(void) { return -1; }
__attribute__((weak)) int sp_mac_pb_clear(void) { return 0; }
__attribute__((weak)) int sp_mac_pb_put(uint32_t fmt, const void *p, uint32_t n) { (void)fmt; (void)p; (void)n; return 0; }
__attribute__((weak)) uint32_t sp_mac_pb_get(uint32_t fmt, void *buf, uint32_t cap) { (void)fmt; (void)buf; (void)cap; return 0; }
__attribute__((weak)) int sp_mac_pb_has(uint32_t fmt) { (void)fmt; return 0; }
__attribute__((weak)) void sp_mac_pb_use_named(const char *name) { (void)name; }

/* ---- the store: NSPasteboard ('m'), in-process ('p') or the back end's text hooks ('l') ------- */
typedef struct { uint32_t fmt; uint8_t *p; uint32_t n; } clip_item;
#define MAX_ITEMS 16
static clip_item items[MAX_ITEMS];
static unsigned n_items;
static long mem_seq;
static char mode;

static char pick_mode(void) {
    const char *e = getenv("SPORE_CLIPBOARD");
    sgp_ensure();
    if (e && !strcmp(e, "private")) return 'p';
    if (e && !strcmp(e, "mac") && sp_mac_pb_seq() >= 0) return 'm';
    if (sgp_be == &sgp_backend_headless) return 'p';
    if (sp_mac_pb_seq() >= 0) return 'm';
    return 'l';
}

char sgp_clip_mode(void) {
    if (!mode) {
        mode = pick_mode();
        SGP_LOG("clipboard: %s", mode == 'm' ? "NSPasteboard" : mode == 'p' ? "in-process store" :
                "back end text hooks (no viewer notifications)");
    }
    return mode;
}

static clip_item *mem_find(uint32_t fmt) {
    unsigned i;
    for (i = 0; i < n_items; i++)
        if (items[i].fmt == fmt) return &items[i];
    return NULL;
}
static void mem_clear(void) {
    unsigned i;
    for (i = 0; i < n_items; i++) free(items[i].p);
    n_items = 0;
    mem_seq++;
}
static int mem_put(uint32_t fmt, const void *p, uint32_t n) {
    clip_item *it = mem_find(fmt);
    uint8_t *c = malloc(n ? n : 1);
    if (!c) return 0;
    memcpy(c, p, n);
    if (!it) {
        if (n_items == MAX_ITEMS) { free(c); return 0; }
        it = &items[n_items++];
        it->fmt = fmt;
    } else {
        free(it->p);
    }
    it->p = c;
    it->n = n;
    mem_seq++;
    return 1;
}
static uint32_t mem_get(uint32_t fmt, void *buf, uint32_t cap) {
    clip_item *it = mem_find(fmt);
    if (!it) return 0;
    if (buf) memcpy(buf, it->p, it->n < cap ? it->n : cap);
    return it->n;
}

static long st_seq(void) {
    switch (sgp_clip_mode()) {
    case 'm': return sp_mac_pb_seq();
    case 'p': return mem_seq;
    default: return -1;
    }
}
static void st_clear(void) {
    switch (sgp_clip_mode()) {
    case 'm': sp_mac_pb_clear(); break;
    case 'p': mem_clear(); break;
    default: mem_clear(); break;   /* 'l': the next text set replaces the host text */
    }
}
static int st_put(uint32_t fmt, const void *p, uint32_t n) {
    switch (sgp_clip_mode()) {
    case 'm': return sp_mac_pb_put(fmt, p, n);
    case 'l':
        if (fmt == CF_UNICODETEXT) return sgp_be->clip_set((const uint16_t *)p, n / 2);
        return mem_put(fmt, p, n);
    default: return mem_put(fmt, p, n);
    }
}
/* Returns the size in bytes; copies min(size, cap) into buf. */
static uint32_t st_get(uint32_t fmt, void *buf, uint32_t cap) {
    switch (sgp_clip_mode()) {
    case 'm': return sp_mac_pb_get(fmt, buf, cap);
    case 'l':
        if (fmt == CF_UNICODETEXT) {
            static uint16_t t[65536];
            uint32_t n = sgp_be->clip_get(t, 65535) * 2u;
            if (buf) memcpy(buf, t, n < cap ? n : cap);
            return n;
        }
        return mem_get(fmt, buf, cap);
    default: return mem_get(fmt, buf, cap);
    }
}
static int st_has(uint32_t fmt) {
    switch (sgp_clip_mode()) {
    case 'm': return sp_mac_pb_has(fmt);
    default: return st_get(fmt, NULL, 0) > 0;
    }
}

/* ---- state ------------------------------------------------------------------------------- */
static int clip_open;
static uint32_t clip_owner;            /* hwnd given to OpenClipboard */
static uint32_t viewer;                /* the one viewer we track (the chain beyond it is the game's) */
static long seen_seq = -2, own_seq = -2;
static uint32_t next_poll_ms;
static int poll_ms = -1;
static struct { uint32_t fmt, h; } handles[8];   /* blocks returned by GetClipboardData */

static int poll_interval(void) {
    if (poll_ms < 0) {
        const char *e = getenv("SPORE_CLIPBOARD_POLL_MS");
        poll_ms = e && *e ? atoi(e) : 250;
        if (poll_ms < 0) poll_ms = 0;
    }
    return poll_ms;
}

static void note_own_change(void) { own_seq = st_seq(); }

static void free_handles(sg_ctx *ctx, int all, uint32_t fmt) {
    unsigned i;
    for (i = 0; i < sizeof handles / sizeof handles[0]; i++) {
        if (!handles[i].h || (!all && handles[i].fmt != fmt)) continue;
        {
            uint32_t a[1] = {handles[i].h};
            sgp_call_import(ctx, "GlobalFree", 1, a);
        }
        handles[i].h = 0;
    }
}

static uint32_t guest_alloc(sg_ctx *ctx, uint32_t n) {
    uint32_t a[2] = {GMEM_MOVEABLE, n};
    return sgp_call_import(ctx, "GlobalAlloc", 2, a);
}

void sgp_clip_reset(void) {
    unsigned i;
    viewer = 0;
    seen_seq = own_seq = -2;
    next_poll_ms = 0;
    poll_ms = -1;
    clip_open = 0;
    for (i = 0; i < sizeof handles / sizeof handles[0]; i++) handles[i].h = 0;
    for (i = 0; i < n_items; i++) free(items[i].p);
    n_items = 0;
    mode = 0;
}

/* ---- the shims --------------------------------------------------------------------------- */
SGP_CLIP_FN(OpenClipboard) {
    clip_open++;
    clip_owner = SG_ARG(0);
    SG_RETURN_STDCALL(1, 4);
}

SGP_CLIP_FN(CloseClipboard) {
    if (clip_open > 0) clip_open--;
    note_own_change();
    SG_RETURN_STDCALL(1, 0);
}

SGP_CLIP_FN(EmptyClipboard) {
    sgp_ensure();
    SGP_ENTER();
    free_handles(ctx, 1, 0);
    SGP_LEAVE();
    st_clear();
    note_own_change();
    SG_RETURN_STDCALL(1, 0);
}

SGP_CLIP_FN(IsClipboardFormatAvailable) {
    uint32_t f = SG_ARG(0);
    sgp_ensure();
    SG_RETURN_STDCALL(st_has(f == CF_TEXT ? CF_UNICODETEXT : f) ? 1 : 0, 4);
}

SGP_CLIP_FN(GetClipboardData) {
    uint32_t f = SG_ARG(0), src = f == CF_TEXT ? CF_UNICODETEXT : f, n, h = 0, i, slot;
    uint8_t *buf;
    sgp_ensure();
    n = st_get(src, NULL, 0);
    if (!n) SG_RETURN_STDCALL(0, 4);
    buf = malloc(n);
    if (!buf) SG_RETURN_STDCALL(0, 4);
    n = st_get(src, buf, n);
    SGP_ENTER();
    free_handles(ctx, 0, f);
    if (src == CF_UNICODETEXT) {
        uint32_t units = n / 2;
        h = guest_alloc(ctx, f == CF_TEXT ? units + 1 : 2 * units + 2);
        if (h) {
            for (i = 0; i <= units; i++) {
                uint16_t u = i < units ? (uint16_t)(buf[2 * i] | buf[2 * i + 1] << 8) : 0;
                if (f == CF_TEXT) mem[h + i] = (uint8_t)(u < 0x100 ? u : '?');
                else sg_st16(mem, h + 2 * i, u);
            }
        }
    } else {
        h = guest_alloc(ctx, n);
        if (h) memcpy(mem + h, buf, n);
    }
    SGP_LEAVE();
    free(buf);
    if (h) {
        for (slot = 0; slot < sizeof handles / sizeof handles[0] && handles[slot].h; slot++) {}
        if (slot < sizeof handles / sizeof handles[0]) { handles[slot].fmt = f; handles[slot].h = h; }
    }
    SG_RETURN_STDCALL(h, 4);
}

SGP_CLIP_FN(SetClipboardData) {
    uint32_t f = SG_ARG(0), h = SG_ARG(1), n;
    int ok = 0;
    static int warned;
    sgp_ensure();
    if (!h) {
        if (!warned++) hle_log(HLE_LOG_WARN, "SetClipboardData(%u, NULL): delayed rendering is not supported", f);
        SG_RETURN_STDCALL(0, 8);
    }
    if (f == CF_UNICODETEXT || f == CF_TEXT) {
        uint8_t *t;
        uint32_t i;
        for (n = 0; n < MAX_TEXT_UNITS && (f == CF_TEXT ? mem[h + n] : sg_ld16(mem, h + 2 * n)); n++) {}
        t = malloc(2u * n + 2u);
        if (t) {
            for (i = 0; i < n; i++) {
                uint16_t u = f == CF_TEXT ? mem[h + i] : sg_ld16(mem, h + 2 * i);
                t[2 * i] = (uint8_t)u;
                t[2 * i + 1] = (uint8_t)(u >> 8);
            }
            ok = st_put(CF_UNICODETEXT, t, 2 * n);
            free(t);
        }
    } else {
        uint32_t a[1] = {h};
        SGP_ENTER();
        n = sgp_call_import(ctx, "GlobalSize", 1, a);
        SGP_LEAVE();
        ok = n ? st_put(f, mem + h, n) : 0;
    }
    note_own_change();
    SGP_TRACE("SetClipboardData(%u, 0x%x) -> %s", f, h, ok ? "ok" : "failed");
    SG_RETURN_STDCALL(ok ? h : 0, 8);
}

SGP_CLIP_FN(SetClipboardViewer) {
    uint32_t h = SG_ARG(0), prev = viewer;
    sgp_ensure();
    viewer = h;
    seen_seq = st_seq();
    /* Windows sends the new viewer a WM_DRAWCLIPBOARD; posted here so it arrives from the pump. */
    if (h && poll_interval() > 0) sp_post_message(h, WM_DRAWCLIPBOARD, 0, 0);
    SGP_LOG("SetClipboardViewer(0x%x): previous 0x%x, pasteboard polled every %d ms", h, prev, poll_interval());
    SG_RETURN_STDCALL(prev, 4);
}

SGP_CLIP_FN(ChangeClipboardChain) {
    uint32_t remove = SG_ARG(0), next = SG_ARG(1);
    if (remove && remove == viewer) viewer = next;
    SG_RETURN_STDCALL(1, 8);
}

static uint32_t mono_ms(void) {  /* real time: the tests' message clock (sp_tick_ms) may be scripted */
    struct timespec ts;
    clock_gettime(CLOCK_MONOTONIC, &ts);
    return (uint32_t)((uint64_t)ts.tv_sec * 1000u + (uint64_t)ts.tv_nsec / 1000000u);
}

void sgp_clip_poll(void) {
    uint32_t now;
    long seq;
    if (!viewer || poll_interval() <= 0) return;
    now = mono_ms();
    if (next_poll_ms && (int32_t)(now - next_poll_ms) < 0) return;
    next_poll_ms = now + (uint32_t)poll_ms;
    seq = st_seq();
    if (seq < 0 || seq == seen_seq) return;
    seen_seq = seq;
    if (seq == own_seq) return;   /* our own copy: Windows' echo is ignored by the game's guard anyway */
    if (!sgp_win_find(viewer)) { viewer = 0; return; }
    SGP_TRACE("clipboard: pasteboard changed (seq %ld), WM_DRAWCLIPBOARD to 0x%x", seq, viewer);
    sp_post_message(viewer, WM_DRAWCLIPBOARD, 0, 0);
}
