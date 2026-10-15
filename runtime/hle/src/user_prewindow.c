/* user_prewindow.c -- the USER32/GDI32/XInput calls the game makes before its first
 * CreateWindowExW (docs/design/win32-layer.md 22.4). All WEAK: the macOS platform layer
 * replaces them with real ones. Until then they are honest minimal versions:
 *   - XInputGetState/SetState/GetCapabilities: no controller -> ERROR_DEVICE_NOT_CONNECTED (1167),
 *     which is what XInput returns on a PC with no pad (polled from 0x8d21fc).
 *   - GetKeyboardLayout(thread): HKL 0x04090409 (US layout, en-US); 0x849c08 keeps its low word as
 *     the language for GetLocaleInfoW(LOCALE_IDEFAULTANSICODEPAGE).
 *   - CreateSolidBrush, LoadCursorA, LoadIconA: distinct non-NULL pseudo handles (0x00e0xxxx),
 *     enough for the WNDCLASSEXW at 0x849c56..0x849cb3; nothing draws with them before the window exists.
 *   - RegisterClassExW: records the class name and window procedure (the game's is 0x849310,
 *     class L"Canvas") and returns a class atom (0xc000 + n), as Windows does for a new class. */
#include <string.h>

#include "hle.h"
#include "hle_imports.h"

#define ERROR_DEVICE_NOT_CONNECTED 1167u
#define PSEUDO_HANDLE_BASE 0x00e00000u

static uint32_t next_pseudo = PSEUDO_HANDLE_BASE;
static uint32_t pseudo(void) { return __atomic_add_fetch(&next_pseudo, 4u, __ATOMIC_RELAXED); }

__attribute__((weak)) SG_IMPORT(xinput9_1_0, XInputGetState) { SG_RETURN_STDCALL(ERROR_DEVICE_NOT_CONNECTED, 8); }
__attribute__((weak)) SG_IMPORT(xinput9_1_0, XInputSetState) { SG_RETURN_STDCALL(ERROR_DEVICE_NOT_CONNECTED, 8); }
__attribute__((weak)) SG_IMPORT(xinput9_1_0, XInputGetCapabilities) { SG_RETURN_STDCALL(ERROR_DEVICE_NOT_CONNECTED, 12); }

__attribute__((weak)) SG_IMPORT(user32, GetKeyboardLayout) { SG_RETURN_STDCALL(0x04090409u, 4); }

__attribute__((weak)) SG_IMPORT(gdi32, CreateSolidBrush) {
    uint32_t h = pseudo();
    hle_log(HLE_LOG_INFO, "CreateSolidBrush(0x%06x) = 0x%08x (pseudo handle)", SG_ARG(0), h);
    SG_RETURN_STDCALL(h, 4);
}
__attribute__((weak)) SG_IMPORT(user32, LoadCursorA) {
    uint32_t h = pseudo();
    hle_log(HLE_LOG_INFO, "LoadCursorA(0x%08x, 0x%08x) = 0x%08x (pseudo handle)", SG_ARG(0), SG_ARG(1), h);
    SG_RETURN_STDCALL(h, 8);
}
__attribute__((weak)) SG_IMPORT(user32, LoadIconA) {
    uint32_t h = pseudo();
    hle_log(HLE_LOG_INFO, "LoadIconA(0x%08x, 0x%08x) = 0x%08x (pseudo handle)", SG_ARG(0), SG_ARG(1), h);
    SG_RETURN_STDCALL(h, 8);
}

/* WNDCLASSEXW (x86): cbSize @0, style @4, lpfnWndProc @8, cbClsExtra @12, cbWndExtra @16,
 * hInstance @20, hIcon @24, hCursor @28, hbrBackground @32, lpszMenuName @36,
 * lpszClassName @40, hIconSm @44; 48 bytes. */
typedef struct hle_wndclass { char name[64]; uint32_t wndproc, style; } hle_wndclass;
hle_wndclass hle_classes[32];
unsigned hle_class_count;

__attribute__((weak)) SG_IMPORT(user32, RegisterClassExW) {
    uint32_t wc = SG_ARG(0), nm;
    hle_wndclass *c;
    unsigned i, n;
    if (!wc || sg_ld32(mem, wc) != 48u || hle_class_count >= 32) {
        hle_set_last_error(ctx, mem, 87); /* ERROR_INVALID_PARAMETER */
        SG_RETURN_STDCALL(0, 4);
    }
    c = &hle_classes[hle_class_count];
    memset(c, 0, sizeof *c);
    nm = sg_ld32(mem, wc + 40);
    if (nm >= 0x10000u) /* a string; < 0x10000 is an atom */
        for (i = 0; i < sizeof c->name - 1 && sg_ld16(mem, nm + 2 * i); i++) {
            uint16_t u = sg_ld16(mem, nm + 2 * i);
            c->name[i] = u < 0x80 ? (char)u : '?';
        }
    for (n = 0; n < hle_class_count; n++)
        if (c->name[0] && !strcmp(hle_classes[n].name, c->name)) {
            hle_set_last_error(ctx, mem, 1410); /* ERROR_CLASS_ALREADY_EXISTS */
            SG_RETURN_STDCALL(0, 4);
        }
    c->wndproc = sg_ld32(mem, wc + 8);
    c->style = sg_ld32(mem, wc + 4);
    hle_class_count++;
    hle_log(HLE_LOG_INFO, "RegisterClassExW(\"%s\", style 0x%x, wndproc 0x%08x) = 0x%04x", c->name, c->style, c->wndproc,
            0xc000u + hle_class_count - 1);
    SG_RETURN_STDCALL(0xc000u + hle_class_count - 1, 4);
}
