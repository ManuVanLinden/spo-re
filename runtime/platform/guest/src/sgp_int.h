/* sgp_int.h -- state shared by the runtime/platform/guest sources. Not a public interface. */
#ifndef SGP_INT_H
#define SGP_INT_H

#include <stdio.h>
#include <string.h>

#include "sgp.h"
#include "hle.h"
#include "hle_imports.h"

#include "sg_core.h"

/* ---- windows and classes -------------------------------------------------------------------- */
#define SGP_MAX_WIN 8
#define SGP_MAX_CLASS 32
#define SGP_EXTRA 16 /* cbWndExtra words kept */

typedef struct sgp_class {
    int used;
    uint16_t atom;
    char name[64];       /* lower-case ASCII for comparison */
    uint32_t wndproc, style, extra_bytes, hcursor, hbrush, hinst;
    int ansi;
} sgp_class;

typedef struct sgp_win {
    int used;
    uint32_t h;          /* sp_hwnd, also the guest HWND */
    uint32_t wndproc, style, exstyle, userdata, id, hinst, parent, menu;
    uint16_t atom;
    int ansi, attached, toplevel;
    uint32_t extra[SGP_EXTRA];
    char title[256];
    uint32_t class_style;
    uint32_t last_paint_ms;
} sgp_win;

extern sgp_win sgp_wins[SGP_MAX_WIN];
extern sgp_class sgp_classes[SGP_MAX_CLASS];
extern const sgp_backend *sgp_be;
extern int sgp_caption_h;          /* frame model: caption height in points, no side borders */

sgp_win *sgp_win_find(uint32_t h);  /* live window (also checks sp_is_window) */
sgp_class *sgp_class_find_name(const char *lower);
sgp_class *sgp_class_find_atom(uint16_t atom);

/* Frame model shared by AdjustWindowRectEx, GetWindowRect, SetWindowPos and MoveWindow. */
int32_t sgp_frame_top(uint32_t style, uint32_t exstyle);

/* ---- the ctx a shim was entered with (callbacks into guest code use it) -------------------- */
extern SG_THREAD_LOCAL sg_ctx *sgp_tl_ctx;
#define SGP_ENTER() sg_ctx *sgp_saved_ctx_ = sgp_tl_ctx; sgp_tl_ctx = ctx
#define SGP_LEAVE() (sgp_tl_ctx = sgp_saved_ctx_)
/* Calls guest code (stdcall) inside an sg_cleanup barrier (runtime-abi.md 10.4). */
uint32_t sgp_guest_call(sg_ctx *ctx, uint32_t proc, unsigned n, const uint32_t *args, const char *what);
/* Calls another package's import shim (stdcall) by export name through the generated import table, as
 * translated code would (GlobalAlloc for clipboard data). Returns eax. */
uint32_t sgp_call_import(sg_ctx *ctx, const char *name, unsigned n, const uint32_t *args);

/* ---- guest memory helpers --------------------------------------------------------------------- */
/* UTF-16 (guest) -> UTF-8 (host), NUL-terminated, truncating. */
size_t sgp_wstr_utf8(const uint8_t *mem, uint32_t a, char *out, size_t cap);
size_t sgp_astr(const uint8_t *mem, uint32_t a, char *out, size_t cap);   /* CP1252-ish -> UTF-8 */
void sgp_lower(char *s);
uint32_t sgp_pseudo_handle(void);   /* 0x00e1xxxx, distinct from runtime/hle's 0x00e0xxxx */

/* runtime/hle hle_boot_stops.c: the stop before the first window (SPORE_STOP_AT); real CreateWindowExW/A call it first. */
void hle_stop_check(sg_ctx *ctx, uint8_t *mem, const char *import);

#define SGP_LOG(...) hle_log(HLE_LOG_INFO, __VA_ARGS__)
#define SGP_TRACE(...) hle_log(HLE_LOG_TRACE, __VA_ARGS__)

/* SPORE_INPUT_TRACE=1 logs the cursor imports (GetCursorPos, SetCursorPos, ScreenToClient,
 * ClientToScreen) with the guest return address, and every mouse message PeekMessage returns, as
 * "in: t=<ms> ..." lines on stderr (at most 200,000 lines). */
int sgp_input_trace_on(void);
#define SGP_INTRACE(...) do { if (sgp_input_trace_on()) sgp_input_trace(__VA_ARGS__); } while (0)
void sgp_input_trace(const char *fmt, ...) __attribute__((format(printf, 1, 2)));

/* sgp_run.c: SPORE_RUN_SECONDS and the timed-run counters */
void sgp_run_check(void);
extern uint64_t sgp_stat_peek_calls, sgp_stat_msgs, sgp_stat_paints, sgp_stat_dispatched, sgp_stat_timers;
#endif
