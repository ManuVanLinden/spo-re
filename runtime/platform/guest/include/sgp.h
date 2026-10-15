/*
 * sgp.h -- the guest-facing window, input and message-loop imports: USER32, GDI32, IMM32,
 * XInput and DirectInput8 as strong SG_IMPORT definitions over runtime/platform/macos.
 *
 * Linking: every import here is a strong `sg_imp_<dll>_<name>` (SG_IMPORT). runtime/hle's
 * generated stubs and its hand-written placeholders (user_prewindow.c, hle_boot_stops.c, dinput_stub.c) are
 * weak, so these replace them at link time for direct SG_CALL(sg_imp_...) call sites and in
 * hle_imports[] (whose .fn fields name the same symbols). sgp_register_shims() checks that and
 * the clean-up bytes against the generated import table (docs/design/platform-macos.md section 10).
 *
 * Back ends: the imports talk to the portable message queue (sp_queue.c) plus a small back end
 * for what only the host knows (windows on screen, cursor, clipboard, screen size). Two exist:
 *   headless  (sgp_backend.c)      no AppKit: windows are queue entries; tests and SPORE_HEADLESS=1
 *   AppKit    (sgp_backend_mac.c)  NSWindow + CAMetalLayer through sp_macos.h (the game binary)
 * The AppKit file defines the strong sgp_default_backend(); without it the headless one is used.
 *
 * Threads: sg_boot runs the guest main thread on the process main thread (runtime/core
 * core_boot.c: sg_core_attach_main on the caller of main()), which is the AppKit main thread.
 * The first window import made on that thread initialises the queue and AppKit there; the pump
 * (PeekMessageW/A) drains NSEvents on every call. No [NSApp run] (sp_macos.h).
 *
 * No game code. Plain C11 (and one Objective-C file in runtime/platform/macos).
 */
#ifndef SGP_H
#define SGP_H

#include <stdint.h>

#include "spore_guest.h"
#include "sp_platform.h"

#ifdef __cplusplus
extern "C" {
#endif

/* ---- back end ------------------------------------------------------------------------------ */
typedef struct sgp_backend {
    const char *name;
    int (*init)(void);                               /* on the window thread; calls sp_platform_init */
    /* Host side of a top-level window, created on its first ShowWindow (windows that are never
     * shown, such as EAClipboard's, never get one). The sp_hwnd is already registered; the client
     * rect is in Windows screen points. */
    void (*attach)(sp_hwnd h, const char *title_utf8, int32_t x, int32_t y, int32_t w, int32_t hh);
    void (*detach)(sp_hwnd h);
    /* Moves/sizes the client area; must end in sp_window_rect_changed (WM_MOVE/WM_SIZE). The host
     * may clamp it (never fullscreen, never off screen). */
    void (*set_client_rect)(sp_hwnd h, int attached, int32_t x, int32_t y, int32_t w, int32_t hh);
    /* Orders the host window in or out. Returns 1 if the host delivers activation itself
     * (AppKit: windowDidBecomeKey), 0 if the caller must (headless). */
    int (*show)(sp_hwnd h, int show, int activate);
    void (*set_title)(sp_hwnd h, const char *utf8);
    int (*is_iconic)(sp_hwnd h);
    int32_t (*caption_height)(void);                 /* title bar height in points */
    void (*screen)(int32_t *w, int32_t *h, sp_rect *work, int32_t *refresh_hz, int32_t *mm_w, int32_t *mm_h);
    void (*get_cursor_pos)(int32_t *x, int32_t *y);  /* screen points */
    void (*set_cursor_pos)(int32_t x, int32_t y);    /* ignored unless one of our windows is active */
    int (*show_cursor)(int show);                    /* returns the display counter */
    void (*set_cursor_shape)(uint32_t idc);          /* IDC_* (32512 arrow ...), 0 = none */
    /* Clipboard as UTF-16 text. get returns units written (0: empty); set returns 1 on success. */
    uint32_t (*clip_get)(uint16_t *buf, uint32_t cap);
    int (*clip_set)(const uint16_t *s, uint32_t n);
    int (*message_box)(const char *text, const char *caption, uint32_t type); /* returns IDOK etc. */
    uint32_t (*double_click_ms)(void);
    void *(*metal_layer)(sp_hwnd h);
    /* The window's style asks for fullscreen (WS_POPUP without a caption, the game's path at
     * 0x8490c0) or not. NULL: no fullscreen (headless). Returns 1 if the host window is now in that state. */
    int (*set_fullscreen)(sp_hwnd h, int on);
} sgp_backend;

extern const sgp_backend sgp_backend_headless;
const sgp_backend *sgp_default_backend(void);       /* weak default: headless */
/* Selects the back end before the first window import (tests). NULL = default. */
void sgp_set_backend(const sgp_backend *b);
const sgp_backend *sgp_backend_in_use(void);

/* ---- set-up -------------------------------------------------------------------------------- */
/* Initialises on first use (the caller becomes the window thread). Safe to call more than once. 0 on success. */
int sgp_ensure(void);
/* Checks every import this package defines against the generated import table: the entry's fn must be our
 * function and its clean-up bytes must agree. Returns the number checked; *bad gets the number
 * that disagree (logged). Call after the image linker filled hle_imports (sg_core_startup time
 * or any time later). */
unsigned sgp_register_shims(unsigned *bad);
/* For tests: the list of (symbol, function, clean-up bytes) this package defines. */
typedef struct sgp_shim { const char *symbol; sg_func *fn; uint32_t cleanup; } sgp_shim;
extern const sgp_shim sgp_shims[];
extern const unsigned sgp_shim_count;

/* ---- guest window procedures --------------------------------------------------------------- */
/* Calls the guest window procedure `proc` (stdcall, 4 args) on the current guest thread inside an
 * sg_cleanup barrier (runtime-abi.md 10.4: a guest exception that tries to unwind through
 * DispatchMessage aborts with a diagnostic). Fatal if the thread is not a guest thread. */
int32_t sgp_call_wndproc(uint32_t proc, uint32_t hwnd, uint32_t msg, uint32_t wparam, int32_t lparam);
/* Optional trace of every guest window-procedure call (tests): kind 'D' dispatched, 'S' sent. */
extern void (*sgp_wndproc_trace)(uint32_t hwnd, uint32_t msg, uint32_t wparam, int32_t lparam, int depth, char kind);

/* Frame cadence: when > 0, a visible window that has not been painted for 1000/hz ms is
 * invalidated by the pump (env SPORE_PAINT_HZ). 0 (default): WM_PAINT only when dirty, as on
 * Windows (the game invalidates on every pump pass, see sgp_msg.c). */
extern int sgp_paint_hz;

/* Window lookups for other packages (the D3D9 back end: the CAMetalLayer of the game window). */
uint32_t sgp_main_window(void);                      /* first top-level window created, 0 if none */
void *sgp_metal_layer(uint32_t hwnd);                /* CAMetalLayer * or NULL (headless) */

#ifdef __cplusplus
}
#endif
#endif /* SGP_H */
