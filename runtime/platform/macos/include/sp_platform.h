/*
 * sp_platform.h - the macOS platform layer: window messages, input and key state.
 *
 * Layering
 *   sp_queue.c   portable C: windows table, posted/input/sent queues, PeekMessageW semantics,
 *                WM_PAINT/WM_TIMER/WM_QUIT synthesis, TranslateMessage, DispatchMessageW,
 *                SendMessageW (same and cross thread), DefWindowProcW subset, key-state tables.
 *   sp_input.c   portable C: macOS key codes -> VK + scan code + WM_KEY* lParam, mouse -> WM_MOUSE*.
 *   sp_keymap.c  portable C: the kVK -> (VK, scan code, extended) table.
 *   sp_window.m  AppKit: NSWindow + CAMetalLayer view, event drain (the pump_events hook),
 *                window-state -> WM_* (activation, size, move, close), coordinate mapping.
 *
 * Threading
 *   - Window procedures run only on the thread that owns the queue (the main thread), as on
 *     Windows. sp_peek_message must be called on that thread.
 *   - sp_post_message and sp_send_message may be called from any thread. A cross-thread send
 *     blocks until the window thread processes it inside sp_peek_message (Win32 semantics).
 *
 * Guest boundary
 *   The window procedure is a host callback (sp_wndproc_fn). The runtime installs one that calls
 *   the guest WndProc (0x849310) with sg_call_guest; that is legal because every call into it is
 *   made from inside a shim (PeekMessageW, DispatchMessageW, SendMessageW, DefWindowProcW,
 *   UpdateWindow, ShowWindow), which satisfies runtime-abi.md section 7.3. Messages whose lParam
 *   is a pointer (WM_GETMINMAXINFO, WM_SIZING) go through the scratch area (sp_set_scratch) so
 *   the lParam is a guest address.
 */
#ifndef SP_PLATFORM_H
#define SP_PLATFORM_H

#include <stddef.h>
#include <stdint.h>
#include "sp_win32.h"

#ifdef __cplusplus
extern "C" {
#endif

typedef int32_t (*sp_wndproc_fn)(void *user, sp_hwnd hwnd, uint32_t msg, uint32_t wparam, int32_t lparam);

/* Hooks the host (AppKit layer or a headless test) provides. Any member may be NULL. */
typedef struct sp_host_hooks {
    void    *ctx;
    void    (*pump_events)(void *ctx);                       /* drain host events (main thread) */
    void    (*wake)(void *ctx);                              /* called after a cross-thread post */
    void    (*client_to_screen)(void *ctx, sp_hwnd, int32_t *x, int32_t *y);
    void    (*screen_to_client)(void *ctx, sp_hwnd, int32_t *x, int32_t *y);
    void    (*get_client_size)(void *ctx, sp_hwnd, int32_t *w, int32_t *h);
    uint32_t (*now_ms)(void *ctx);                           /* GetTickCount; NULL = monotonic */
    void    (*destroy_window)(void *ctx, sp_hwnd);           /* host side of DestroyWindow */
} sp_host_hooks;

/* Keyboard mapping options. */
enum sp_cmd_mapping { SP_CMD_AS_CONTROL = 0, SP_CMD_AS_WIN = 1 };
typedef struct sp_input_config {
    int cmd_mapping;      /* default SP_CMD_AS_CONTROL: Cmd+C reaches the game as Ctrl+C */
    int option_is_alt;    /* default 1: Option = Alt; WM_SYSCHAR uses the unmodified character */
    int send_nchittest;   /* default 1: WM_NCHITTEST + WM_SETCURSOR before each mouse message */
} sp_input_config;

/* ---- setup ------------------------------------------------------------------------------- */
void     sp_platform_init(const sp_host_hooks *hooks, const sp_input_config *cfg); /* on the window thread */
void     sp_platform_shutdown(void);
int      sp_is_owner_thread(void);
uint32_t sp_tick_ms(void);                                   /* GetTickCount */

/* Scratch area in guest memory for pointer-carrying messages: host view + guest address. */
void     sp_set_scratch(void *host, uint32_t guest_addr, uint32_t size);
void    *sp_scratch_host(uint32_t guest_addr);               /* guest address -> host pointer */

/* ---- windows ----------------------------------------------------------------------------- */
#define SP_STYLE_DBLCLKS 0x8                                 /* CS_DBLCLKS, the "Canvas" class style */
sp_hwnd  sp_window_register(sp_wndproc_fn proc, void *user, uint32_t class_style); /* CreateWindowExW core */
void     sp_window_unregister(sp_hwnd);
int      sp_is_window(sp_hwnd);
void     sp_show_window(sp_hwnd, int show, int activate);    /* WM_SHOWWINDOW (+ activation) */
void     sp_update_window(sp_hwnd);                          /* UpdateWindow: send WM_PAINT if invalid */
int      sp_destroy_window(sp_hwnd);                         /* DestroyWindow: WM_DESTROY */
/* Window-state notifications from the host (all send messages synchronously). */
void     sp_window_rect_changed(sp_hwnd, int32_t client_x_screen, int32_t client_y_screen,
                                int32_t client_w, int32_t client_h); /* WM_MOVE then WM_SIZE, as Wine */
void     sp_window_set_minimized(sp_hwnd, int minimized);    /* WM_SIZE(SIZE_MINIMIZED/RESTORED) */
void     sp_window_activate(sp_hwnd, int active, int app_change); /* WM_ACTIVATEAPP/NCACTIVATE/ACTIVATE */
void     sp_window_close_request(sp_hwnd);                   /* WM_CLOSE */
void     sp_window_enter_size_move(sp_hwnd, int enter);      /* WM_ENTERSIZEMOVE / WM_EXITSIZEMOVE */
int      sp_window_get_minmax(sp_hwnd, sp_minmaxinfo *io);   /* WM_GETMINMAXINFO through scratch */
void     sp_window_display_change(sp_hwnd, int bpp, int w, int h); /* WM_DISPLAYCHANGE */
void     sp_get_last_client(sp_hwnd, int32_t *x, int32_t *y, int32_t *w, int32_t *h);

/* ---- the message queue ------------------------------------------------------------------- */
int      sp_post_message(sp_hwnd, uint32_t msg, uint32_t wparam, int32_t lparam); /* any thread */
void     sp_post_quit_message(int32_t code);
int      sp_peek_message(sp_msg *out, sp_hwnd filter, uint32_t min, uint32_t max, uint32_t flags);
int      sp_translate_message(const sp_msg *m);
int32_t  sp_dispatch_message(const sp_msg *m);
int32_t  sp_send_message(sp_hwnd, uint32_t msg, uint32_t wparam, int32_t lparam); /* any thread */
int32_t  sp_def_window_proc(sp_hwnd, uint32_t msg, uint32_t wparam, int32_t lparam);
uint32_t sp_queue_depth(int which);                          /* 0 posted, 1 input (tests) */
/* Delivery info for tracing: nesting depth (0 = outermost) and kind 'D' (dispatched) or 'S' (sent). */
void     sp_delivery_info(int *depth, char *kind);

/* Painting. */
#define SP_FAKE_HDC 0x00DC0001u
void     sp_invalidate_rect(sp_hwnd, int erase);             /* whole client area */
void     sp_validate_rect(sp_hwnd);
uint32_t sp_begin_paint(sp_hwnd, sp_paintstruct *ps);        /* sends WM_ERASEBKGND if pending */
void     sp_end_paint(sp_hwnd, const sp_paintstruct *ps);

/* Timers (WM_TIMER is synthesised after WM_PAINT, as on Windows). */
uint32_t sp_set_timer(sp_hwnd, uint32_t id, uint32_t elapse_ms);
int      sp_kill_timer(sp_hwnd, uint32_t id);

/* Capture and mouse-leave tracking. */
sp_hwnd  sp_set_capture(sp_hwnd);
int      sp_release_capture(void);
sp_hwnd  sp_get_capture(void);
int      sp_track_mouse_leave(sp_hwnd);                      /* TrackMouseEvent(TME_LEAVE) */
sp_hwnd  sp_get_focus(void);
sp_hwnd  sp_set_focus(sp_hwnd);                             /* SetFocus: WM_KILLFOCUS/WM_SETFOCUS; returns old */
sp_hwnd  sp_get_active(void);                               /* GetActiveWindow */
int      sp_window_visible(sp_hwnd);
int      sp_window_invalid(sp_hwnd);
/* When on, a hidden window gets no WM_PAINT from the queue or UpdateWindow (Windows
 * behaviour). Default off, which the reference input script was written with. */
void     sp_set_paint_needs_visible(int on);

/* ---- input (called by the host event layer on the window thread) -------------------------- */
enum sp_key_action { SP_KEY_DOWN = 0, SP_KEY_UP = 1, SP_KEY_REPEAT = 2 };
void     sp_input_key(uint16_t mac_keycode, int action, const uint16_t *chars, int nchars,
                      const uint16_t *chars_nomod, int nchars_nomod);
void     sp_input_modifier(uint16_t mac_keycode, int down);   /* flagsChanged for Shift/Ctrl/Opt/Cmd */
void     sp_input_capslock(int on);                          /* flagsChanged for Caps Lock */
void     sp_input_init_toggles(int caps_on);                 /* NumLock is always reported on */
void     sp_input_reset(void);                               /* focus lost: all keys and buttons up */
enum sp_mouse_button { SP_MB_LEFT = 0, SP_MB_RIGHT = 1, SP_MB_MIDDLE = 2 };
void     sp_input_mouse_move(sp_hwnd, int32_t x, int32_t y);  /* client coordinates */
void     sp_input_mouse_button(sp_hwnd, int button, int down, int32_t x, int32_t y, int clicks);
void     sp_input_wheel(sp_hwnd, int32_t delta);              /* WHEEL_DELTA units, + = away from user */
void     sp_input_mouse_exited(sp_hwnd);                     /* WM_MOUSELEAVE if tracked */
void     sp_get_cursor_client(sp_hwnd, int32_t *x, int32_t *y);
void     sp_client_to_screen(sp_hwnd, int32_t *x, int32_t *y);  /* ClientToScreen */
void     sp_screen_to_client(sp_hwnd, int32_t *x, int32_t *y);  /* ScreenToClient */

/* Key-state tables (256 entries each, bit 0x80 down, bit 0x01 toggled). */
int16_t  sp_get_key_state(int vk);           /* GetKeyState: synchronous with retrieved messages */
int16_t  sp_get_async_key_state(int vk);     /* GetAsyncKeyState: state at event-translation time */
void     sp_get_keyboard_state(uint8_t out[256]);
void     sp_set_keyboard_state(const uint8_t in[256]);

/* ---- key map ----------------------------------------------------------------------------- */
typedef struct sp_keyinfo {
    uint8_t vk;          /* wParam of WM_KEY* (generic VK_SHIFT/VK_CONTROL/VK_MENU for modifiers) */
    uint8_t vk_specific; /* VK_LSHIFT etc. for modifiers, else equal to vk */
    uint8_t scan;        /* set-1 scan code */
    uint8_t ext;         /* extended-key flag (lParam bit 24) */
} sp_keyinfo;
int      sp_keymap_lookup(uint16_t mac_keycode, const sp_input_config *cfg, sp_keyinfo *out);
const char *sp_message_name(uint32_t msg);   /* "WM_KEYDOWN", "WM_USER+0x100", "MSG_0x1234" */

#ifdef __cplusplus
}
#endif
#endif /* SP_PLATFORM_H */
