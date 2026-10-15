/*
 * sp_macos.h - the AppKit side of the platform layer (src/sp_window.m).
 *
 * The game's main thread runs WinMain, so it is the AppKit main thread: sp_mac_app_init() sets up
 * NSApplication without calling -run, and every sp_peek_message() drains pending NSEvents
 * (nextEventMatchingMask:untilDate:distantPast) and translates them into queued WM_* input. The
 * game never blocks in GetMessage (it imports only PeekMessageW/A), so AppKit is serviced exactly
 * as often as the game pumps; while the game sleeps in SleepEx between frames, events wait in
 * the AppKit queue, as they would in the Win32 queue.
 */
#ifndef SP_MACOS_H
#define SP_MACOS_H

#include "sp_platform.h"

#ifdef __cplusplus
extern "C" {
#endif

int      sp_mac_app_init(int activate);          /* NSApplication + menu; installs the queue hooks */
/* CreateWindowExW + RegisterClassExW core: an NSWindow whose content view hosts a CAMetalLayer.
 * x, y: top-left of the client area in Windows screen coordinates (origin top-left of the
 * primary display, in points); pass INT32_MIN to centre. */
sp_hwnd  sp_mac_create_window(const char *title_utf8, int32_t x, int32_t y, int32_t w, int32_t h,
                              sp_wndproc_fn proc, void *user, uint32_t class_style);
void    *sp_mac_metal_layer(sp_hwnd);            /* CAMetalLayer * (bridged, not retained) */
void     sp_mac_set_client_size(sp_hwnd, int32_t w, int32_t h);   /* keeps the top-left corner */
void     sp_mac_order_front(sp_hwnd);
int      sp_mac_window_number(sp_hwnd);
/* PNG of the window: 1 = window-server image, 2 = AppKit drawing of the frame, 0 = failed. */
int      sp_mac_snapshot(sp_hwnd, const char *path);
double   sp_mac_backing_scale(sp_hwnd);
void     sp_mac_present_clear(sp_hwnd, float r, float g, float b); /* one Metal frame */
/* Block until an event or a cross-thread post arrives, or the timeout passes (MsgWait-style). */
void     sp_mac_wait_message(uint32_t timeout_ms);
/* Test mode: hardware input is dropped and only synthetic events (timestamp >= SP_MAC_SYNTH_TS)
 * are translated; AppKit focus notifications are ignored (the script drives activation). */
/* 1e9 s (31 years) of uptime: real NSEvent timestamps are seconds since boot, and a Mac that has
 * been up for more than 10.4 days would pass a 900,000 s marker, and every real event would be taken
 * as synthetic. */
#define SP_MAC_SYNTH_TS 1.0e9
void     sp_mac_set_test_mode(int on);
/* Scripted input (SPORE_INPUT_SCRIPT, sgp_run.c): post one synthetic mouse event at client
 * point (x, y) of h to the AppKit queue, so it goes through the same locationInWindow -> client
 * mapping as a real event. kind 0 = move (a drag while a scripted button is held), 1 = button down,
 * 2 = button up; button 0 left, 1 right, 2 middle.
 * From then on GetCursorPos reports the scripted point (screen coordinates) instead of the real
 * pointer, so the user's mouse does not disturb the script. Main thread only; 1 = posted. */
int      sp_mac_post_mouse(sp_hwnd, int kind, int button, int32_t x, int32_t y, int clicks);
/* Scripted keys: post one synthetic key event (kind 1 = down, 2 = up) with macOS virtual key code kvk and
 * the character ch (0 for none) to the AppKit queue; it takes the same translate_key path as a real key.
 * Main thread only; 1 = posted. */
int      sp_mac_post_key(sp_hwnd, int kind, uint16_t kvk, uint16_t ch);
/* Scripted wheel: post one synthetic scroll-wheel event of `notches` line notches (+ = away). */
int      sp_mac_post_wheel(sp_hwnd, int32_t x, int32_t y, int notches);
/* Cursor services: SetCursorPos (warp), ShowCursor counter. */
int      sp_mac_set_cursor_pos(int32_t screen_x, int32_t screen_y);  /* 1 if the (real or scripted) cursor moved */
/* A poster of synthetic mouse events records the window location (locationInWindow) it meant,
 * under the event's timestamp; the translation uses it instead of AppKit's inexact round trip. */
void     sp_mac_note_synthetic_point(double timestamp, double window_x, double window_y);
int      sp_mac_show_cursor(int show);
/* For the guest USER32 layer (runtime/platform/guest). */
int      sp_mac_attach_window(sp_hwnd, const char *title_utf8, int32_t x, int32_t y, int32_t w, int32_t h);
void     sp_mac_detach_window(sp_hwnd);
int32_t  sp_mac_caption_height(void);            /* title-bar height of our window style, points */
void     sp_mac_cursor_release(void);            /* show the cursor again (focus loss, Esc) */
int      sp_mac_is_active(void);                 /* one of our windows is key */
/* Client rect in Windows screen coordinates (guest units); clamped into the primary screen's visible
 * frame (never off screen); INT32_MIN x/y centres. Reports WM_MOVE/WM_SIZE. In fullscreen only the
 * size counts: it becomes the client size the game sees, shown letterboxed over the display. */
void     sp_mac_set_client_rect(sp_hwnd, int32_t x, int32_t y, int32_t w, int32_t h);
/* Guest units and fullscreen (docs/design/platform-macos.md section 11). Guest units are points, or the primary display's
 * pixels with SPORE_RETINA=1 (sp_mac_guest_scale() = 1 or the backing scale); every coordinate the
 * game sees (client sizes, screen and mouse points, metrics) is in them.
 * Fullscreen: a borderless window over the window's current display, Dock and menu bar hidden while it is
 * key; never a display-mode switch. The client size the game sees is kept; the client is drawn fitted at
 * its aspect (letterbox) and mouse points are mapped back. Cmd-Ctrl-F toggles it from the keyboard (the
 * game is not told); SPORE_FULLSCREEN=0 refuses it (a titled window instead); SPORE_FULLSCREEN_ESC=1 makes
 * Esc leave it too. Returns 1 if the window is now in the requested state. */
int      sp_mac_set_fullscreen(sp_hwnd, int on);
int      sp_mac_is_fullscreen(sp_hwnd);
/* Whether fullscreen letterboxes (default: unless SPORE_D3D9 names a presenter other than metal, which
 * stretch the back buffer over the whole drawable, so the mouse mapping stretches too). Tests. */
void     sp_mac_set_letterbox(int on);
/* Where the client is shown: x, y, w, h in view points, top-left origin. Tests. */
void     sp_mac_content_rect(sp_hwnd, double out[4]);
double   sp_mac_guest_scale(void);
void     sp_mac_order_out(sp_hwnd);
void     sp_mac_make_key(sp_hwnd);
void     sp_mac_set_title(sp_hwnd, const char *utf8);
int      sp_mac_is_miniaturized(sp_hwnd);
void     sp_mac_screen_info(int32_t *w, int32_t *h, sp_rect *work, int32_t *hz, int32_t *mm_w, int32_t *mm_h);
/* DEVIATIONS 4.12: SPORE_FULLSCREEN_SAFE_AREA (0/unset off; 1 the display's safeAreaInsets;
 * "top[,bottom,left,right]" points for tests). Fullscreen letterboxes inside the safe area, and
 * sp_mac_screen_info reports the safe area's size. Returns 1 when that applies to the primary display (the
 * switch is on and its insets are not zero); out (may be NULL) gets top, bottom, left, right in points. */
int      sp_mac_screen_safe_area(double out[4]);
void     sp_mac_cursor_pos(int32_t *x, int32_t *y);   /* screen points, top-left origin */
void     sp_mac_set_cursor_shape(uint32_t idc);
uint32_t sp_mac_clip_get(uint16_t *buf, uint32_t cap);
int      sp_mac_clip_set(const uint16_t *s, uint32_t n);
int      sp_mac_message_box(const char *text, const char *caption, uint32_t type);
uint32_t sp_mac_double_click_ms(void);

/* ---- game controllers (src/sp_gamepad_core.c portable C; src/sp_gamepad.m GameController back end) ----
 * Four XInput users. A pad takes the lowest free user when it connects and keeps it until it disconnects.
 * The back end pushes host input (sp_pad_update, any thread); XInputGetState reads a snapshot under a lock
 * (sp_pad_get_state, cheap, main thread every frame). The packet number changes only when the mapped state
 * does. sp_pad_start() starts the GameController back end once (when linked; SPORE_GAMEPAD=0 refuses). */
#define SP_PAD_USERS 4
/* XINPUT_GAMEPAD wButtons bits */
#define SP_PAD_DPAD_UP 0x0001u
#define SP_PAD_DPAD_DOWN 0x0002u
#define SP_PAD_DPAD_LEFT 0x0004u
#define SP_PAD_DPAD_RIGHT 0x0008u
#define SP_PAD_START 0x0010u
#define SP_PAD_BACK 0x0020u
#define SP_PAD_LEFT_THUMB 0x0040u
#define SP_PAD_RIGHT_THUMB 0x0080u
#define SP_PAD_LEFT_SHOULDER 0x0100u
#define SP_PAD_RIGHT_SHOULDER 0x0200u
#define SP_PAD_A 0x1000u
#define SP_PAD_B 0x2000u
#define SP_PAD_X 0x4000u
#define SP_PAD_Y 0x8000u
typedef struct sp_pad_input {     /* host values: sticks -1..1 with +y up, triggers 0..1 */
    float lx, ly, rx, ry, lt, rt;
    uint16_t buttons;             /* SP_PAD_* */
} sp_pad_input;
typedef struct sp_pad_xstate {    /* XINPUT_GAMEPAD, field for field */
    uint16_t buttons;
    uint8_t lt, rt;
    int16_t lx, ly, rx, ry;
} sp_pad_xstate;
typedef struct sp_pad_caps {
    uint8_t subtype;              /* XINPUT_DEVSUBTYPE_* (1 = gamepad) */
    uint16_t buttons;             /* SP_PAD_* bits the profile has */
    uint8_t triggers, sticks;     /* 1 when present */
    uint8_t rumble;               /* 1 when the controller has haptics */
    char name[64];
} sp_pad_caps;
typedef void (*sp_pad_rumble_fn)(void *dev, uint16_t low, uint16_t high);
void sp_pad_map(const sp_pad_input *in, sp_pad_xstate *out);
int  sp_pad_connect(void *dev, const sp_pad_caps *caps);      /* user 0-3, or -1 when all four are taken */
int  sp_pad_disconnect(void *dev);                            /* the user it had, or -1 */
int  sp_pad_user_of(void *dev);
int  sp_pad_update(void *dev, const sp_pad_input *in);        /* 1 if the mapped state changed */
int  sp_pad_get_state(int user, uint32_t *packet, sp_pad_xstate *out);   /* 0, or -1 for no pad */
int  sp_pad_get_caps(int user, sp_pad_caps *out);
int  sp_pad_set_vibration(int user, uint16_t low, uint16_t high);        /* 0, or -1 for no pad */
void sp_pad_set_rumble_handler(sp_pad_rumble_fn fn);
void sp_pad_start(void);
void sp_pad_reset(void);                                      /* tests: empty all users */
/* sp_gamepad.m (weak in the core, so headless builds have no back end) */
void sp_mac_gamepad_start(void);

/* src/sp_open.m: open an http: or https: URL in the default browser with NSWorkspace; 1 = opened (or
 * queued to the main thread when called off it). Anything else returns 0 and opens nothing. The HLE's
 * ShellExecuteW finds it with dlsym. sp_mac_open_url_check does every check without opening. */
int      sp_mac_open_url(const char *utf8);
int      sp_mac_open_url_check(const char *utf8);

/* src/sp_dragdrop.m: files dragged onto the game view. sp_mac_dnd_attach registers the view of h for
 * file URLs (now, or when its NSWindow first appears) and remembers fn: an NSDraggingDestination category on the
 * view queues one event per AppKit callback (only .png files are offered; a drag with none is refused and queues
 * nothing) and then calls fn on the main thread, which drains the queue with sp_mac_dnd_poll and returns the
 * DROPEFFECT of the last event it delivered (0 refuses the drag; ~0u = nothing delivered, keep the default copy).
 * runtime/platform/guest/src/sgp_dragdrop.c turns the events into IDropTarget calls; it has weak defaults of these
 * four, so headless builds link without AppKit. Points are client points of h in guest units. */
enum { SP_DND_ENTER = 1, SP_DND_OVER = 2, SP_DND_LEAVE = 3, SP_DND_DROP = 4 };
typedef struct sp_dnd_event {
    int kind;                     /* SP_DND_* */
    sp_hwnd hwnd;
    int32_t x, y;                 /* client point, guest units */
    uint32_t keys;                /* MK_LBUTTON 1, MK_SHIFT 4, MK_CONTROL 8, MK_ALT 0x20 */
    int nfiles;                   /* ENTER and DROP: the .png files, UTF-8 host paths (malloc'd) */
    char **files;
} sp_dnd_event;
typedef uint32_t (*sp_dnd_notify_fn)(void *user);
int      sp_mac_dnd_attach(sp_hwnd h, sp_dnd_notify_fn fn, void *user);   /* 1 registered, 2 pending, 0 no host */
void     sp_mac_dnd_detach(sp_hwnd h);
int      sp_mac_dnd_poll(sp_dnd_event *out);                              /* 1 = an event was taken */
void     sp_mac_dnd_event_free(sp_dnd_event *e);

/* Text input (src/sp_textinput.inc + the NSTextInputClient methods of the game view in src/sp_window.m): dead keys
 * and input methods. The core is compiled once per binary by #include from runtime/platform/guest/src/sgp_imm.c
 * (and tests/hostapp.m); sp_window.m has weak stand-ins, so binaries without the core get plain key events.
 * SPORE_TEXT_INPUT=0 turns it all off (no IMM context, plain key events); SPORE_TEXT_INPUT_TRACE=1 logs it. */
enum { SP_TI_LAYOUT = 0, SP_TI_IM = 1 };
enum { SP_GCS_COMPSTR = 0x8, SP_GCS_COMPATTR = 0x10, SP_GCS_COMPCLAUSE = 0x20, SP_GCS_CURSORPOS = 0x80,
       SP_GCS_DELTASTART = 0x100, SP_GCS_RESULTSTR = 0x800, SP_GCS_RESULTCLAUSE = 0x1000 };
enum { SP_NI_COMPOSITIONSTR = 0x15, SP_CPS_COMPLETE = 1, SP_CPS_CONVERT = 2, SP_CPS_REVERT = 3, SP_CPS_CANCEL = 4 };
int      sp_ti_enabled(void);
void     sp_ti_set_source(int kind, const char *id);     /* the current input source; a new one re-opens the IME */
void     sp_ti_force_kind(int kind);                     /* tests: SP_TI_LAYOUT/IM, -1 = follow the source */
void     sp_ti_set_discard_hook(void (*fn)(void));       /* the view's [inputContext discardMarkedText] */
void     sp_ti_reset(void);                              /* tests */
int      sp_ti_wants_key(uint16_t kvk, int cmd_or_ctrl); /* offer this key-down to the input context? */
void     sp_ti_key_begin(void);                          /* before interpretKeyEvents: */
int      sp_ti_key_end(uint16_t kvk, int action, const uint16_t *chars, int n);  /* 1 = key-down emitted here */
void     sp_ti_set_marked(const uint16_t *s, int n, int sel_loc, int sel_len);   /* setMarkedText: */
void     sp_ti_insert(const uint16_t *s, int n);         /* insertText: */
void     sp_ti_unmark(void);                             /* unmarkText: commits the marked text */
int      sp_ti_marked(uint16_t *out, int max, int *sel_loc, int *sel_len);       /* marked length */
uint32_t sp_ti_context(sp_hwnd h);                       /* ImmGetContext: a pseudo HIMC, 0 when off */
int      sp_ti_open_status(void);
int      sp_ti_set_open(int open);
uint32_t sp_ti_conversion(void);
int      sp_ti_notify(uint32_t action, uint32_t index, uint32_t value);
int32_t  sp_ti_comp_string(uint32_t index, void *buf, uint32_t bytes);           /* ImmGetCompositionStringW */
int      sp_ti_def_window_proc(sp_hwnd h, uint32_t msg, uint32_t w, int32_t l, int32_t *result);
void     sp_mac_set_text_test(int on);                   /* tests: synthetic keys go through the input context */

#ifdef __cplusplus
}
#endif
#endif
