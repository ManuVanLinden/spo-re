/*
 * sp_input.c - host keyboard and mouse events -> queued WM_* input messages.
 *
 * Keyboard lParam (Microsoft Learn, "WM_KEYDOWN", "WM_SYSKEYDOWN", "Keystroke Messages"):
 *   bits 0-15 repeat count (always 1 here), 16-23 scan code, 24 extended key,
 *   29 context code (Alt held; set on WM_SYS* messages), 30 previous key state, 31 transition.
 * Message choice: WM_SYSKEYDOWN/UP when Alt is held without Ctrl, and for F10; otherwise
 * WM_KEYDOWN/UP. Releasing Alt sends WM_SYSKEYUP if no other key was pressed while it was held,
 * otherwise WM_KEYUP (the menu-activation rule) [inf: matches Spy++ traces; not checked on a PC].
 * Characters: TranslateMessage posts WM_CHAR (or WM_SYSCHAR under Alt) with the key-down's lParam.
 * The characters come from the host event (NSEvent.characters) with Windows rules for Enter,
 * Backspace, Tab, Escape and Ctrl+letter applied here, because macOS reports different code
 * points for those (Backspace 0x7F, keypad Enter 0x03, Cmd+C "c").
 *
 * Mouse: client coordinates in lParam, MK_* flags in wParam. A button event at a position
 * different from the last reported one is preceded by WM_MOUSEMOVE, as a real mouse would be.
 * Even click counts become WM_*BUTTONDBLCLK when the class has CS_DBLCLKS (the game's "Canvas"
 * class does, style 8 at 0x849cc1). WM_MOUSEWHEEL carries screen coordinates in lParam.
 */
#include "sp_internal.h"

#include <string.h>

static uint8_t g_phys[256];          /* physical-key counts per specific VK (Cmd and Ctrl may share) */
static int g_alt_used;               /* a key was pressed while Alt was held */
static sp_hwnd g_cur_hwnd;
static int32_t g_cur_x = -1, g_cur_y = -1;

void sp__input_reset_state(void)
{
    for (int i = 0; i < 256; i++) {
        sp__async[i] &= 0x01;
        sp__sync[i] &= 0x01;
        sp__async_pressed[i] = 0;
    }
    memset(g_phys, 0, sizeof g_phys);
    g_alt_used = 0;
}

void sp_input_reset(void) { sp__input_reset_state(); }

void sp_input_init_toggles(int caps_on)
{
    /* macOS keyboards have no Num Lock: report it on so the keypad produces digits. */
    sp__async[VK_NUMLOCK] |= 1; sp__sync[VK_NUMLOCK] |= 1;
    if (caps_on) { sp__async[VK_CAPITAL] |= 1; sp__sync[VK_CAPITAL] |= 1; }
    else { sp__async[VK_CAPITAL] &= (uint8_t)~1; sp__sync[VK_CAPITAL] &= (uint8_t)~1; }
}

static int down(int vk) { return (sp__async[vk] & 0x80) != 0; }

static int filter_char(uint16_t c)
{
    if (c >= 0xF700 && c <= 0xF8FF) return 0;   /* AppKit function-key code points */
    if (c < 0x20 || c == 0x7F) return 0;         /* control codes come only from the rules below */
    return 1;
}

static int compute_chars(const sp_keyinfo *k, int ctrl, int alt, const uint16_t *chars, int n,
                         const uint16_t *nomod, int nn, uint16_t out[4])
{
    switch (k->vk) {
    case VK_RETURN: out[0] = ctrl ? 0x0A : 0x0D; return 1;
    case VK_BACK:   out[0] = ctrl ? 0x7F : 0x08; return 1;
    case VK_TAB:    if (alt || ctrl) return 0; out[0] = 0x09; return 1;
    case VK_ESCAPE: if (alt) return 0; out[0] = 0x1B; return 1;
    default: break;
    }
    if (ctrl && !alt) {
        if (k->vk >= 'A' && k->vk <= 'Z') { out[0] = (uint16_t)(k->vk - 0x40); return 1; }
        switch (k->vk) {
        case VK_OEM_4: out[0] = 0x1B; return 1;
        case VK_OEM_5: out[0] = 0x1C; return 1;
        case VK_OEM_6: out[0] = 0x1D; return 1;
        case VK_SPACE: out[0] = 0x20; return 1;
        default: return 0;
        }
    }
    const uint16_t *src = chars; int sn = n;
    if (alt && sp__cfg()->option_is_alt && nomod && nn > 0) { src = nomod; sn = nn; }
    int o = 0;
    for (int i = 0; i < sn && o < 4; i++) if (filter_char(src[i])) out[o++] = src[i];
    return o;
}

static void emit_key(const sp_keyinfo *k, int action, const uint16_t *chars, int n,
                     const uint16_t *nomod, int nn)
{
    sp_hwnd h = sp__key_target();
    int is_alt_key = (k->vk == VK_MENU);
    int alt = down(VK_MENU) || (is_alt_key && action != SP_KEY_UP);
    int ctrl = down(VK_CONTROL);
    sp_qent e; memset(&e, 0, sizeof e);
    e.m.hwnd = h;
    e.m.time = sp_tick_ms();
    int32_t px = g_cur_x < 0 ? 0 : g_cur_x, py = g_cur_y < 0 ? 0 : g_cur_y;
    if (h) sp__client_to_screen(h, &px, &py);
    e.m.pt.x = px; e.m.pt.y = py;
    e.key_vk = k->vk; e.key_vk_specific = k->vk_specific;
    uint32_t lp = 1u | ((uint32_t)k->scan << 16) | ((uint32_t)k->ext << 24);

    if (action == SP_KEY_UP) {
        int sys;
        if (is_alt_key) { sys = !g_alt_used; g_alt_used = 0; }
        else sys = alt && !ctrl;
        if (alt && !is_alt_key) lp |= 1u << 29;
        lp |= (1u << 30) | (1u << 31);
        e.m.message = sys ? WM_SYSKEYUP : WM_KEYUP;
        e.key_change = -1;
        sp__keystate_apply(sp__async, k->vk, k->vk_specific, 0);
    } else {
        int prev = down(k->vk_specific ? k->vk_specific : k->vk) || action == SP_KEY_REPEAT;
        int sys = (alt && !ctrl) || k->vk == VK_F1 + 9;   /* F10 is a system key */
        if (alt) lp |= 1u << 29;
        if (prev) lp |= 1u << 30;
        e.m.message = sys ? WM_SYSKEYDOWN : WM_KEYDOWN;
        e.charmsg = (alt && !ctrl) ? WM_SYSCHAR : WM_CHAR;
        e.nchars = (uint8_t)compute_chars(k, ctrl, alt, chars, n, nomod, nn, e.chars);
        e.key_change = prev ? 0 : +1;
        if (!is_alt_key && alt) g_alt_used = 1;
        if (is_alt_key && !prev) g_alt_used = 0;
        if (!prev) sp__keystate_apply(sp__async, k->vk, k->vk_specific, 1);
    }
    e.m.wParam = k->vk;
    e.m.lParam = (int32_t)lp;
    if (h) sp__enqueue_input(&e);
}

void sp_input_key(uint16_t kvk, int action, const uint16_t *chars, int n, const uint16_t *nomod, int nn)
{
    sp_keyinfo k;
    if (!sp_keymap_lookup(kvk, sp__cfg(), &k)) return;
    if (k.vk == VK_CAPITAL) return;              /* Caps Lock arrives through sp_input_capslock */
    if (k.vk_specific != k.vk) { sp_input_modifier(kvk, action != SP_KEY_UP); return; }
    if (action == SP_KEY_REPEAT && !down(k.vk)) action = SP_KEY_DOWN;
    emit_key(&k, action, chars, n, nomod, nn);
}

void sp_input_modifier(uint16_t kvk, int is_down)
{
    sp_keyinfo k;
    if (!sp_keymap_lookup(kvk, sp__cfg(), &k)) return;
    uint8_t *c = &g_phys[k.vk_specific];
    if (is_down) {
        if ((*c)++ != 0) return;                 /* already held through another physical key */
        emit_key(&k, SP_KEY_DOWN, NULL, 0, NULL, 0);
    } else {
        if (*c == 0) return;
        if (--(*c) != 0) return;
        emit_key(&k, SP_KEY_UP, NULL, 0, NULL, 0);
    }
}

void sp_input_capslock(int on)
{
    sp_keyinfo k = {VK_CAPITAL, VK_CAPITAL, 0x3A, 0};
    (void)on;   /* every flagsChanged for Caps Lock is one press: down + up, toggling the state */
    emit_key(&k, SP_KEY_DOWN, NULL, 0, NULL, 0);
    emit_key(&k, SP_KEY_UP, NULL, 0, NULL, 0);
}

/* ---- mouse --------------------------------------------------------------------------------- */
static uint32_t mk_flags(void)
{
    uint32_t f = 0;
    if (down(VK_LBUTTON)) f |= MK_LBUTTON;
    if (down(VK_RBUTTON)) f |= MK_RBUTTON;
    if (down(VK_MBUTTON)) f |= MK_MBUTTON;
    if (down(VK_SHIFT)) f |= MK_SHIFT;
    if (down(VK_CONTROL)) f |= MK_CONTROL;
    return f;
}

static void mouse_msg(sp_hwnd h, uint32_t msg, uint32_t w, int32_t x, int32_t y, int vk, int change)
{
    sp_qent e; memset(&e, 0, sizeof e);
    e.m.hwnd = h; e.m.message = msg; e.m.wParam = w;
    e.m.lParam = sp__makelparam(x, y);
    e.m.time = sp_tick_ms();
    int32_t sx = x, sy = y;
    sp__client_to_screen(h, &sx, &sy);
    e.m.pt.x = sx; e.m.pt.y = sy;
    e.is_mouse = 1;
    e.key_vk = (uint8_t)vk; e.key_vk_specific = (uint8_t)vk; e.key_change = (int8_t)change;
    sp__enqueue_input(&e);
}

void sp_input_mouse_move(sp_hwnd h, int32_t x, int32_t y)
{
    if (!h) return;
    if (h == g_cur_hwnd && x == g_cur_x && y == g_cur_y) return;
    g_cur_hwnd = h; g_cur_x = x; g_cur_y = y;
    mouse_msg(h, WM_MOUSEMOVE, mk_flags(), x, y, 0, 0);
}

void sp_input_mouse_button(sp_hwnd h, int button, int is_down, int32_t x, int32_t y, int clicks)
{
    static const int vks[3] = {VK_LBUTTON, VK_RBUTTON, VK_MBUTTON};
    static const uint32_t downs[3] = {WM_LBUTTONDOWN, WM_RBUTTONDOWN, WM_MBUTTONDOWN};
    if (!h || button < 0 || button > 2) return;
    sp_input_mouse_move(h, x, y);
    int vk = vks[button];
    uint32_t msg = downs[button];
    if (is_down) {
        if (clicks >= 2 && (clicks % 2) == 0 && sp__window_dblclks(h)) msg += 2;   /* DBLCLK */
        sp__keystate_apply(sp__async, vk, vk, 1);
    } else {
        msg += 1;
        sp__keystate_apply(sp__async, vk, vk, 0);
    }
    mouse_msg(h, msg, mk_flags(), x, y, vk, is_down ? +1 : -1);
}

void sp_input_wheel(sp_hwnd h, int32_t delta)
{
    if (!h || !delta) return;
    int32_t sx = g_cur_x < 0 ? 0 : g_cur_x, sy = g_cur_y < 0 ? 0 : g_cur_y;
    sp__client_to_screen(h, &sx, &sy);
    sp_qent e; memset(&e, 0, sizeof e);
    e.m.hwnd = h; e.m.message = WM_MOUSEWHEEL;
    e.m.wParam = (mk_flags() & 0xFFFF) | ((uint32_t)(uint16_t)(int16_t)delta << 16);
    e.m.lParam = sp__makelparam(sx, sy);
    e.m.time = sp_tick_ms();
    e.m.pt.x = sx; e.m.pt.y = sy;
    sp__enqueue_input(&e);
}

void sp_get_cursor_client(sp_hwnd h, int32_t *x, int32_t *y)
{
    (void)h;
    if (x) *x = g_cur_x < 0 ? 0 : g_cur_x;
    if (y) *y = g_cur_y < 0 ? 0 : g_cur_y;
}
