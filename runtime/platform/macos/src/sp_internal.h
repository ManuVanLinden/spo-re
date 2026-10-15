/* sp_internal.h - state shared between sp_queue.c and sp_input.c. Not a public interface. */
#ifndef SP_INTERNAL_H
#define SP_INTERNAL_H

#include "sp_platform.h"

/* One queued message plus what the host knew when it was generated. */
typedef struct sp_qent {
    sp_msg   m;
    uint16_t chars[4];     /* characters TranslateMessage will post for this key-down */
    uint8_t  nchars;
    uint16_t charmsg;      /* WM_CHAR or WM_SYSCHAR */
    uint8_t  key_vk;       /* generic VK whose state changes when the message is retrieved */
    uint8_t  key_vk_specific;
    int8_t   key_change;   /* +1 down, -1 up, 0 no change (repeat, mouse move) */
    uint8_t  is_mouse;     /* send WM_NCHITTEST/WM_SETCURSOR on retrieval */
} sp_qent;

void  sp__enqueue_input(const sp_qent *e);       /* window thread; coalesces WM_MOUSEMOVE */
const sp_input_config *sp__cfg(void);
sp_hwnd sp__key_target(void);
int   sp__window_dblclks(sp_hwnd h);
void  sp__client_to_screen(sp_hwnd h, int32_t *x, int32_t *y);
void  sp__screen_to_client(sp_hwnd h, int32_t *x, int32_t *y);

/* Key-state tables. g_async is updated when the host event is translated, g_sync when the
 * message is retrieved with PM_REMOVE (Win32's "synchronous" key state). */
extern uint8_t sp__async[256];
extern uint8_t sp__sync[256];
extern uint8_t sp__async_pressed[256];          /* GetAsyncKeyState bit 0 */
void  sp__keystate_apply(uint8_t *t, int vk, int vk_specific, int down);
void  sp__input_reset_state(void);

static inline int32_t sp__makelparam(int32_t lo, int32_t hi)
{
    return (int32_t)(((uint32_t)(uint16_t)lo) | ((uint32_t)(uint16_t)hi << 16));
}

#endif
