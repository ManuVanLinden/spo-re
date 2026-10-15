/* test_clipboard.c -- the USER32 clipboard shims (sgp_clipboard.c) as the game uses them (EAClipboard,
 * 0x92ac90 copy, 0x92b020 paste on WM_DRAWCLIPBOARD), over the in-process store and over a NAMED NSPasteboard
 * (never the general one, so the user's clipboard is untouched).
 *
 *   text round trip: OpenClipboard(0), EmptyClipboard, GlobalAlloc(GMEM_MOVEABLE) + SetClipboardData(13),
 *     CloseClipboard; IsClipboardFormatAvailable(13/1), GetClipboardData(13) with GlobalSize = (n + 1) * 2,
 *     GetClipboardData(1) Latin-1 narrowed;
 *   a registered-style format (0xC0DE): bytes round trip, GlobalSize exact; an absent format is absent;
 *   the viewer: SetClipboardViewer returns the previous viewer and queues one WM_DRAWCLIPBOARD; our own
 *     copy is not echoed; a change made outside the shims (another application, simulated through the host
 *     functions) reaches the viewer window through the pump; ChangeClipboardChain unlinks.
 *
 * Built by runtime/platform/guest/tests/clipboard.mk against a copy of sgp_msg.c
 * -- the shims forward to sgp_clipboard.c and the pump polls. No game code. */
#include <stdlib.h>
#include <unistd.h>

#include "fixture.h"
#include "sgp_clipboard.h"

/* The headless binary has no sp_window.m; the test does not need these. */
__attribute__((weak)) int sp_mac_is_fullscreen(sp_hwnd h) { (void)h; return -1; }

/* ---- a tiny GlobalAlloc family (the real one is runtime/win32; GMEM_MOVEABLE handle = pointer) ----- */
#define NBLK 256
static struct { uint32_t p, n; int live; } blk[NBLK];
static unsigned frees;
static int blk_find(uint32_t p) {
    int i;
    for (i = 0; i < NBLK; i++) if (blk[i].live && blk[i].p == p) return i;
    return -1;
}
SG_IMPORT(kernel32, GlobalAlloc) {
    uint32_t n = SG_ARG(1), p = hle_guest_alloc(n ? n : 1, 8);
    int i;
    for (i = 0; i < NBLK && blk[i].live; i++) {}
    if (i < NBLK) { blk[i].p = p; blk[i].n = n; blk[i].live = 1; }
    SG_RETURN_STDCALL(p, 8);
}
SG_IMPORT(kernel32, GlobalSize) { int i = blk_find(SG_ARG(0)); SG_RETURN_STDCALL(i < 0 ? 0 : blk[i].n, 4); }
SG_IMPORT(kernel32, GlobalLock) { SG_RETURN_STDCALL(SG_ARG(0), 4); }
SG_IMPORT(kernel32, GlobalUnlock) { SG_RETURN_STDCALL(0, 4); }
SG_IMPORT(kernel32, GlobalFree) {
    int i = blk_find(SG_ARG(0));
    if (i >= 0) { blk[i].live = 0; frees++; }
    SG_RETURN_STDCALL(i < 0 ? SG_ARG(0) : 0, 4);
}
#define GALLOC(n) GSTD(sg_imp_kernel32_GlobalAlloc, 2, (n))
#define GSIZE(h) GSTD(sg_imp_kernel32_GlobalSize, (h))

/* ---- the viewer window: a guest WNDPROC counting WM_DRAWCLIPBOARD ------------------------------- */
#define VIEWER_PROC FX_FN(7)
static unsigned draws, chains;
static void viewer_proc(sg_ctx *SG_RESTRICT ctx, uint8_t *mem) {
    uint32_t msg = SG_ARG(1);
    if (msg == 0x308) draws++;
    if (msg == 0x30d) chains++;
    SG_RETURN_STDCALL(0, 16);
}
static uint32_t astr(const char *s) {
    uint32_t n = (uint32_t)strlen(s) + 1, a = hle_guest_alloc(n, 4);
    memcpy(M + a, s, n);
    return a;
}
static uint32_t make_viewer(void) {
    static uint32_t atom;
    if (!atom) {
        uint32_t wc = hle_guest_alloc(40, 4);
        memset(M + wc, 0, 40);
        sg_st32(M, wc + 4, VIEWER_PROC);
        sg_st32(M, wc + 12, 4);
        sg_st32(M, wc + 36, astr("EAClipboardClass"));
        atom = GSTD(sg_imp_user32_RegisterClassA, wc);
    }
    /* As 0x92b2ee: never shown, CW_USEDEFAULT everywhere. */
    return GSTD(sg_imp_user32_CreateWindowExA, 0, astr("EAClipboardClass"), astr("EAClipboard"), 0, 0x80000000u,
                0x80000000u, 0x80000000u, 0x80000000u, 0, 0, 0x400000, 0);
}
static void pump(void) {
    uint32_t m = hle_guest_alloc(28, 4);
    int k;
    for (k = 0; k < 3; k++) {
        usleep(3000); /* SPORE_CLIPBOARD_POLL_MS=1 */
        while (GSTD(sg_imp_user32_PeekMessageW, m, 0, 0, 0, 1)) GSTD(sg_imp_user32_DispatchMessageW, m);
    }
}

/* UTF-16 text in a fresh GlobalAlloc block, as 0x92ac90 builds it. */
static uint32_t gtext(const uint16_t *u, uint32_t n) {
    uint32_t h = GALLOC(2 * n + 2), i;
    for (i = 0; i < n; i++) sg_st16(M, h + 2 * i, u[i]);
    sg_st16(M, h + 2 * n, 0);
    return h;
}

static void copy_text(const uint16_t *u, uint32_t n) {
    uint32_t h;
    CHECK_EQ(GSTD(sg_imp_user32_OpenClipboard, 0), 1);
    CHECK_EQ(GSTD0(sg_imp_user32_EmptyClipboard), 1);
    h = gtext(u, n);
    CHECK_EQ(GSTD(sg_imp_user32_SetClipboardData, 13, h), h);
    CHECK_EQ(GSTD0(sg_imp_user32_CloseClipboard), 1);
}

static void round_trip(const char *label) {
    static const uint16_t txt[] = {'h', 0xe9, 'l', 'l', 'o', ' ', 0x2713, ' ', 0xd83d, 0xde00}; /* hello, check, emoji */
    const uint32_t n = sizeof txt / 2;
    uint32_t h, i, ok;
    uint8_t blob[300];
    fprintf(stderr, "-- %s (mode %c)\n", label, sgp_clip_mode());
    copy_text(txt, n);
    CHECK_EQ(GSTD(sg_imp_user32_IsClipboardFormatAvailable, 13), 1);
    CHECK_EQ(GSTD(sg_imp_user32_IsClipboardFormatAvailable, 1), 1);
    /* paste, as 0x92b020 */
    CHECK_EQ(GSTD(sg_imp_user32_OpenClipboard, 0), 1);
    h = GSTD(sg_imp_user32_GetClipboardData, 13);
    CHECK(h != 0, "%s: GetClipboardData(13)", label);
    CHECK_EQ(GSIZE(h), 2 * n + 2);
    for (ok = 1, i = 0; h && i < n; i++) ok &= sg_ld16(M, h + 2 * i) == txt[i];
    CHECK(ok && h && sg_ld16(M, h + 2 * n) == 0, "%s: text read back", label);
    h = GSTD(sg_imp_user32_GetClipboardData, 1);
    CHECK(h && !memcmp(M + h, "h\xe9llo ? ??", n + 1), "%s: CF_TEXT narrowed", label);
    CHECK_EQ(GSTD0(sg_imp_user32_CloseClipboard), 1);
    /* a second paste frees the first block of that format */
    {
        unsigned f0 = frees;
        GSTD(sg_imp_user32_OpenClipboard, 0);
        h = GSTD(sg_imp_user32_GetClipboardData, 13);
        GSTD0(sg_imp_user32_CloseClipboard);
        CHECK(h && frees == f0 + 1, "%s: previous GetClipboardData block freed (%u)", label, frees - f0);
    }
    /* empty text */
    copy_text(txt, 0);
    CHECK_EQ(GSTD(sg_imp_user32_IsClipboardFormatAvailable, 13), 0);
    /* a registered-style format carries bytes */
    for (i = 0; i < sizeof blob; i++) blob[i] = (uint8_t)(i * 7 + 3);
    GSTD(sg_imp_user32_OpenClipboard, 0);
    GSTD0(sg_imp_user32_EmptyClipboard);
    h = GALLOC(sizeof blob);
    memcpy(M + h, blob, sizeof blob);
    CHECK_EQ(GSTD(sg_imp_user32_SetClipboardData, 0xC0DE, h), h);
    h = gtext((const uint16_t[]){'c', 'a', 'r', 'd'}, 4);
    CHECK_EQ(GSTD(sg_imp_user32_SetClipboardData, 13, h), h); /* a second format in the same session */
    GSTD0(sg_imp_user32_CloseClipboard);
    CHECK_EQ(GSTD(sg_imp_user32_IsClipboardFormatAvailable, 0xC0DE), 1);
    CHECK_EQ(GSTD(sg_imp_user32_IsClipboardFormatAvailable, 0xC0DF), 0);
    CHECK_EQ(GSTD(sg_imp_user32_IsClipboardFormatAvailable, 13), 1);
    GSTD(sg_imp_user32_OpenClipboard, 0);
    h = GSTD(sg_imp_user32_GetClipboardData, 0xC0DE);
    CHECK(h && GSIZE(h) == sizeof blob && !memcmp(M + h, blob, sizeof blob), "%s: format 0xC0DE bytes", label);
    CHECK_EQ(GSTD(sg_imp_user32_GetClipboardData, 0xC0DF), 0);
    h = GSTD(sg_imp_user32_GetClipboardData, 13);
    CHECK(h && sg_ld16(M, h) == 'c' && sg_ld16(M, h + 8) == 0, "%s: text beside the private format", label);
    GSTD0(sg_imp_user32_CloseClipboard);
    /* delayed rendering is refused */
    GSTD(sg_imp_user32_OpenClipboard, 0);
    CHECK_EQ(GSTD(sg_imp_user32_SetClipboardData, 13, 0), 0);
    GSTD0(sg_imp_user32_CloseClipboard);
}

static void viewer_checks(const char *label, int external) {
    uint32_t v = make_viewer(), v2 = make_viewer();
    static const uint16_t t[] = {'x', 'y'};
    CHECK(v != 0 && v2 != 0, "%s: viewer windows", label);
    draws = 0;
    CHECK_EQ(GSTD(sg_imp_user32_SetClipboardViewer, v), 0);
    pump();
    CHECK_EQ(draws, 1);                       /* Windows' first WM_DRAWCLIPBOARD */
    copy_text(t, 2);
    pump();
    CHECK_EQ(draws, 1);                       /* our own copy is not echoed */
    if (external) {
        /* Another application writes the pasteboard (host functions, not the shims). */
        sp_mac_pb_clear();
        sp_mac_pb_put(13, (const uint16_t[]){'e', 'x', 't'}, 6);
        pump();
        CHECK_EQ(draws, 2);
        pump();
        CHECK_EQ(draws, 2);                   /* once per change */
        {
            uint32_t h;
            GSTD(sg_imp_user32_OpenClipboard, 0);
            h = GSTD(sg_imp_user32_GetClipboardData, 13);
            CHECK(h && sg_ld16(M, h) == 'e' && sg_ld16(M, h + 4) == 't' && GSIZE(h) == 8, "%s: outside text pasted", label);
            GSTD0(sg_imp_user32_CloseClipboard);
        }
    }
    /* a second viewer gets the first back; unlinking restores it */
    CHECK_EQ(GSTD(sg_imp_user32_SetClipboardViewer, v2), v);
    CHECK_EQ(GSTD(sg_imp_user32_ChangeClipboardChain, v2, v), 1);
    draws = 0;
    pump();
    CHECK_EQ(draws, 1);                       /* v2's first message (queued at SetClipboardViewer) */
    GSTD(sg_imp_user32_ChangeClipboardChain, v, 0);
    GSTD(sg_imp_user32_DestroyWindow, v);
    GSTD(sg_imp_user32_DestroyWindow, v2);
}

int main(int argc, char **argv) {
    int mac = argc > 1 && !strcmp(argv[1], "--mac");
    setenv("SPORE_CLIPBOARD_POLL_MS", "1", 1);
    if (fx_init()) { fprintf(stderr, "fixture init failed\n"); return 2; }
    fx_register(VIEWER_PROC, viewer_proc);
    sgp_set_backend(&sgp_backend_headless);
    sgp_clip_reset();
    CHECK_EQ(sgp_clip_mode(), 'p');
    round_trip("in-process store");
    viewer_checks("in-process store", 0);
    if (mac) {
        /* A private named pasteboard: the user's general pasteboard is never read or written. */
        sp_mac_pb_use_named("org.spore-native.clipboard-test");
        setenv("SPORE_CLIPBOARD", "mac", 1);
        sgp_clip_reset();
        CHECK_EQ(sgp_clip_mode(), 'm');
        round_trip("NSPasteboard (named)");
        {   /* the text is on the pasteboard as a plain string */
            uint16_t b[8];
            uint32_t n = sp_mac_pb_get(13, b, sizeof b);
            CHECK(n == 8 && b[0] == 'c' && b[3] == 'd', "pasteboard string 'card' (%u bytes)", n);
        }
        viewer_checks("NSPasteboard (named)", 1);
        sp_mac_pb_clear();
        sp_mac_pb_use_named(NULL);
    }
    printf("clipboard tests%s: %d passed, %d failed\n", mac ? " (+NSPasteboard)" : "", t_pass, t_fail);
    return t_fail ? 1 : 0;
}
