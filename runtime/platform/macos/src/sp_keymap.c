/*
 * sp_keymap.c - macOS virtual key codes (kVK_*, HIToolbox Events.h) to Windows virtual-key codes,
 * set-1 scan codes and the extended-key flag.
 *
 * The mapping is positional (US ANSI layout), which is what the scan code means on Windows. The
 * VK for letter keys on Windows depends on the active layout; with a US layout it equals the
 * positional value used here. Layout-aware VKs (AZERTY etc.) are listed as remaining work.
 *
 * Sources: kVK_* values from Carbon HIToolbox/Events.h; scan codes and VK codes from the
 * Microsoft "Keyboard Input" / "Virtual-Key Codes" documentation and the USB HID to PS/2 scan
 * code translation table (Microsoft, "Keyboard Scan Code Specification", 2000).
 */
#include "sp_platform.h"

#include <stdio.h>
#include <string.h>

typedef struct { uint8_t vk, scan, ext; } kent;

/* Indexed by kVK (0x00-0x7F). vk == 0 means "no Windows key". */
static const kent g_map[128] = {
    [0x00] = {'A', 0x1E, 0}, [0x01] = {'S', 0x1F, 0}, [0x02] = {'D', 0x20, 0}, [0x03] = {'F', 0x21, 0},
    [0x04] = {'H', 0x23, 0}, [0x05] = {'G', 0x22, 0}, [0x06] = {'Z', 0x2C, 0}, [0x07] = {'X', 0x2D, 0},
    [0x08] = {'C', 0x2E, 0}, [0x09] = {'V', 0x2F, 0},
    [0x0A] = {VK_OEM_102, 0x56, 0},                       /* kVK_ISO_Section */
    [0x0B] = {'B', 0x30, 0}, [0x0C] = {'Q', 0x10, 0}, [0x0D] = {'W', 0x11, 0}, [0x0E] = {'E', 0x12, 0},
    [0x0F] = {'R', 0x13, 0}, [0x10] = {'Y', 0x15, 0}, [0x11] = {'T', 0x14, 0},
    [0x12] = {'1', 0x02, 0}, [0x13] = {'2', 0x03, 0}, [0x14] = {'3', 0x04, 0}, [0x15] = {'4', 0x05, 0},
    [0x16] = {'6', 0x07, 0}, [0x17] = {'5', 0x06, 0},
    [0x18] = {VK_OEM_PLUS, 0x0D, 0},                      /* = */
    [0x19] = {'9', 0x0A, 0}, [0x1A] = {'7', 0x08, 0},
    [0x1B] = {VK_OEM_MINUS, 0x0C, 0},
    [0x1C] = {'8', 0x09, 0}, [0x1D] = {'0', 0x0B, 0},
    [0x1E] = {VK_OEM_6, 0x1B, 0},                         /* ] */
    [0x1F] = {'O', 0x18, 0}, [0x20] = {'U', 0x16, 0},
    [0x21] = {VK_OEM_4, 0x1A, 0},                         /* [ */
    [0x22] = {'I', 0x17, 0}, [0x23] = {'P', 0x19, 0},
    [0x24] = {VK_RETURN, 0x1C, 0},
    [0x25] = {'L', 0x26, 0}, [0x26] = {'J', 0x24, 0},
    [0x27] = {VK_OEM_7, 0x28, 0},                         /* ' */
    [0x28] = {'K', 0x25, 0},
    [0x29] = {VK_OEM_1, 0x27, 0},                         /* ; */
    [0x2A] = {VK_OEM_5, 0x2B, 0},                         /* \ */
    [0x2B] = {VK_OEM_COMMA, 0x33, 0},
    [0x2C] = {VK_OEM_2, 0x35, 0},                         /* / */
    [0x2D] = {'N', 0x31, 0}, [0x2E] = {'M', 0x32, 0},
    [0x2F] = {VK_OEM_PERIOD, 0x34, 0},
    [0x30] = {VK_TAB, 0x0F, 0},
    [0x31] = {VK_SPACE, 0x39, 0},
    [0x32] = {VK_OEM_3, 0x29, 0},                         /* ` */
    [0x33] = {VK_BACK, 0x0E, 0},                          /* kVK_Delete = Backspace */
    [0x35] = {VK_ESCAPE, 0x01, 0},
    /* 0x36-0x3E modifiers: handled by modifier_info() below */
    [0x39] = {VK_CAPITAL, 0x3A, 0},
    [0x40] = {VK_F1 + 16, 0x68, 0},                       /* F17 */
    [0x41] = {VK_DECIMAL, 0x53, 0},
    [0x43] = {VK_MULTIPLY, 0x37, 0},
    [0x45] = {VK_ADD, 0x4E, 0},
    [0x47] = {VK_CLEAR, 0x4C, 0},                         /* Keypad Clear sits where NumLock is */
    [0x48] = {VK_VOLUME_UP, 0x30, 1},
    [0x49] = {VK_VOLUME_DOWN, 0x2E, 1},
    [0x4A] = {VK_VOLUME_MUTE, 0x20, 1},
    [0x4B] = {VK_DIVIDE, 0x35, 1},
    [0x4C] = {VK_RETURN, 0x1C, 1},                        /* keypad Enter */
    [0x4E] = {VK_SUBTRACT, 0x4A, 0},
    [0x4F] = {VK_F1 + 17, 0x69, 0},                       /* F18 */
    [0x50] = {VK_F1 + 18, 0x6A, 0},                       /* F19 */
    [0x51] = {VK_OEM_NEC_EQUAL, 0x59, 0},                 /* keypad = */
    [0x52] = {VK_NUMPAD0 + 0, 0x52, 0}, [0x53] = {VK_NUMPAD0 + 1, 0x4F, 0},
    [0x54] = {VK_NUMPAD0 + 2, 0x50, 0}, [0x55] = {VK_NUMPAD0 + 3, 0x51, 0},
    [0x56] = {VK_NUMPAD0 + 4, 0x4B, 0}, [0x57] = {VK_NUMPAD0 + 5, 0x4C, 0},
    [0x58] = {VK_NUMPAD0 + 6, 0x4D, 0}, [0x59] = {VK_NUMPAD0 + 7, 0x47, 0},
    [0x5A] = {VK_F1 + 19, 0x6B, 0},                       /* F20 */
    [0x5B] = {VK_NUMPAD0 + 8, 0x48, 0}, [0x5C] = {VK_NUMPAD0 + 9, 0x49, 0},
    [0x5D] = {VK_OEM_5, 0x7D, 0},                         /* JIS Yen */
    [0x5E] = {VK_OEM_102, 0x73, 0},                       /* JIS Underscore (Ro) */
    [0x5F] = {VK_SEPARATOR, 0x7E, 0},                     /* JIS keypad comma */
    [0x60] = {VK_F1 + 4, 0x3F, 0},                        /* F5 */
    [0x61] = {VK_F1 + 5, 0x40, 0},                        /* F6 */
    [0x62] = {VK_F1 + 6, 0x41, 0},                        /* F7 */
    [0x63] = {VK_F1 + 2, 0x3D, 0},                        /* F3 */
    [0x64] = {VK_F1 + 7, 0x42, 0},                        /* F8 */
    [0x65] = {VK_F1 + 8, 0x43, 0},                        /* F9 */
    [0x66] = {VK_OEM_ATTN, 0x3A, 0},                      /* JIS Eisu */
    [0x67] = {VK_F1 + 10, 0x57, 0},                       /* F11 */
    [0x68] = {VK_KANA, 0x70, 0},                          /* JIS Kana */
    [0x69] = {VK_F1 + 12, 0x64, 0},                       /* F13 */
    [0x6A] = {VK_F1 + 15, 0x67, 0},                       /* F16 */
    [0x6B] = {VK_F1 + 13, 0x65, 0},                       /* F14 */
    [0x6D] = {VK_F1 + 9, 0x44, 0},                        /* F10 */
    [0x6E] = {VK_APPS, 0x5D, 1},                          /* contextual menu key */
    [0x6F] = {VK_F1 + 11, 0x58, 0},                       /* F12 */
    [0x71] = {VK_F1 + 14, 0x66, 0},                       /* F15 */
    [0x72] = {VK_INSERT, 0x52, 1},                        /* Help sits where Insert is */
    [0x73] = {VK_HOME, 0x47, 1},
    [0x74] = {VK_PRIOR, 0x49, 1},
    [0x75] = {VK_DELETE, 0x53, 1},                        /* forward delete */
    [0x76] = {VK_F1 + 3, 0x3E, 0},                        /* F4 */
    [0x77] = {VK_END, 0x4F, 1},
    [0x78] = {VK_F1 + 1, 0x3C, 0},                        /* F2 */
    [0x79] = {VK_NEXT, 0x51, 1},
    [0x7A] = {VK_F1 + 0, 0x3B, 0},                        /* F1 */
    [0x7B] = {VK_LEFT, 0x4B, 1},
    [0x7C] = {VK_RIGHT, 0x4D, 1},
    [0x7D] = {VK_DOWN, 0x50, 1},
    [0x7E] = {VK_UP, 0x48, 1},
};

static int modifier_info(uint16_t kvk, const sp_input_config *cfg, sp_keyinfo *o)
{
    int win = cfg && cfg->cmd_mapping == SP_CMD_AS_WIN;
    switch (kvk) {
    case 0x38: *o = (sp_keyinfo){VK_SHIFT, VK_LSHIFT, 0x2A, 0}; return 1;      /* kVK_Shift */
    case 0x3C: *o = (sp_keyinfo){VK_SHIFT, VK_RSHIFT, 0x36, 0}; return 1;      /* kVK_RightShift */
    case 0x3B: *o = (sp_keyinfo){VK_CONTROL, VK_LCONTROL, 0x1D, 0}; return 1;  /* kVK_Control */
    case 0x3E: *o = (sp_keyinfo){VK_CONTROL, VK_RCONTROL, 0x1D, 1}; return 1;  /* kVK_RightControl */
    case 0x3A: *o = (sp_keyinfo){VK_MENU, VK_LMENU, 0x38, 0}; return 1;        /* kVK_Option */
    case 0x3D: *o = (sp_keyinfo){VK_MENU, VK_RMENU, 0x38, 1}; return 1;        /* kVK_RightOption */
    case 0x37:                                                                 /* kVK_Command */
        *o = win ? (sp_keyinfo){VK_LWIN, VK_LWIN, 0x5B, 1} : (sp_keyinfo){VK_CONTROL, VK_LCONTROL, 0x1D, 0};
        return 1;
    case 0x36:                                                                 /* kVK_RightCommand */
        *o = win ? (sp_keyinfo){VK_RWIN, VK_RWIN, 0x5C, 1} : (sp_keyinfo){VK_CONTROL, VK_RCONTROL, 0x1D, 1};
        return 1;
    default: return 0;
    }
}

int sp_keymap_lookup(uint16_t kvk, const sp_input_config *cfg, sp_keyinfo *out)
{
    if (kvk >= 128) return 0;
    if (modifier_info(kvk, cfg, out)) return 1;
    const kent *e = &g_map[kvk];
    if (!e->vk) return 0;
    out->vk = e->vk;
    out->vk_specific = e->vk;
    out->scan = e->scan;
    out->ext = e->ext;
    return 1;
}

const char *sp_message_name(uint32_t m)
{
    static _Thread_local char buf[24];
    switch (m) {
#define N(x) case x: return #x;
    N(WM_NULL) N(WM_CREATE) N(WM_DESTROY) N(WM_MOVE) N(WM_SIZE) N(WM_ACTIVATE) N(WM_SETFOCUS)
    N(WM_KILLFOCUS) N(WM_PAINT) N(WM_CLOSE) N(WM_QUIT) N(WM_ERASEBKGND) N(WM_SHOWWINDOW)
    N(WM_SETTINGCHANGE) N(WM_ACTIVATEAPP) N(WM_SETCURSOR) N(WM_GETMINMAXINFO) N(WM_INPUTLANGCHANGE)
    N(WM_CONTEXTMENU) N(WM_DISPLAYCHANGE) N(WM_NCHITTEST) N(WM_NCACTIVATE) N(WM_KEYDOWN) N(WM_KEYUP)
    N(WM_CHAR) N(WM_DEADCHAR) N(WM_SYSKEYDOWN) N(WM_SYSKEYUP) N(WM_SYSCHAR) N(WM_TIMER) N(WM_MENUCHAR)
    N(WM_MOUSEMOVE) N(WM_LBUTTONDOWN) N(WM_LBUTTONUP) N(WM_LBUTTONDBLCLK) N(WM_RBUTTONDOWN)
    N(WM_RBUTTONUP) N(WM_RBUTTONDBLCLK) N(WM_MBUTTONDOWN) N(WM_MBUTTONUP) N(WM_MBUTTONDBLCLK)
    N(WM_MOUSEWHEEL) N(WM_SIZING) N(WM_CAPTURECHANGED) N(WM_ENTERSIZEMOVE) N(WM_EXITSIZEMOVE)
    N(WM_MOUSELEAVE)
#undef N
    default: break;
    }
    if (m >= WM_USER && m < 0x8000) snprintf(buf, sizeof buf, "WM_USER+0x%X", (unsigned)(m - WM_USER));
    else snprintf(buf, sizeof buf, "MSG_0x%04X", (unsigned)m);
    return buf;
}
