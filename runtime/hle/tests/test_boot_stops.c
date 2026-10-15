/* The shims the boot needs up to the first window: pre-window USER/GDI/XInput placeholders, DirectInput8 with no devices,
 * the keyboard layout, and the stop-point predicate (the NLS shims are runtime/win32's and tested there). */
#include <stdlib.h>

#include "harness.h"

uint32_t hle_dinput8_object(uint8_t *mem);
int hle_stop_wanted(const char *import);

static sg_func *thunk_fn(uint32_t a) {
    uint32_t i = (a - SG_THUNK_BASE) / SG_THUNK_SLOT;
    return i < (SG_THUNK_LIMIT - SG_THUNK_BASE) / SG_THUNK_SLOT ? sg_thunk_fn[i] : NULL;
}

void test_boot_stops(void) {
    uint32_t b, wc, o, vt, cls, h1, h2;
    /* XInput: no pad */
    b = gbuf(64);
    CHECK_EQ(CALL(sg_imp_xinput9_1_0_XInputGetState, 8, 0, b), 1167);
    CHECK_EQ(CALL(sg_imp_xinput9_1_0_XInputSetState, 8, 3, b), 1167);
    CHECK_EQ(CALL(sg_imp_xinput9_1_0_XInputGetCapabilities, 12, 0, 1, b), 1167);
    /* keyboard layout and locale */
    CHECK_EQ(CALL(sg_imp_user32_GetKeyboardLayout, 4, 0), 0x04090409u);
    /* pseudo handles */
    h1 = CALL(sg_imp_gdi32_CreateSolidBrush, 4, 0x41235f);
    h2 = CALL(sg_imp_user32_LoadCursorA, 8, 0, 0x7f00);
    CHECK(h1 && h2 && h1 != h2);
    CHECK(CALL(sg_imp_user32_LoadIconA, 8, 0x400000, 1) != 0);
    /* RegisterClassExW */
    wc = gbuf(48);
    cls = gwstr("Canvas");
    sg_st32(M, wc, 48);
    sg_st32(M, wc + 4, 8);
    sg_st32(M, wc + 8, 0x849310);
    sg_st32(M, wc + 40, cls);
    o = CALL(sg_imp_user32_RegisterClassExW, 4, wc);
    CHECK(o >= 0xc000u);
    CHECK_EQ(CALL(sg_imp_user32_RegisterClassExW, 4, wc), 0); /* already exists */
    sg_st32(M, wc, 40);
    CHECK_EQ(CALL(sg_imp_user32_RegisterClassExW, 4, wc), 0); /* bad cbSize */
    /* DirectInput8: object with EnumDevices that finds nothing */
    b = gbuf(4);
    CHECK_EQ(CALL(sg_imp_dinput8_DirectInput8Create, 20, 0x400000, 0x800, gbuf(16), b, 0), 0);
    o = sg_ld32(M, b);
    CHECK(o == hle_dinput8_object(M));
    vt = sg_ld32(M, o);
    CHECK(thunk_fn(sg_ld32(M, vt + 0x10)) != NULL);
    if (thunk_fn(sg_ld32(M, vt + 0x10))) {
        CHECK_EQ(call_shim(thunk_fn(sg_ld32(M, vt + 0x10)), 20, 5, (uint32_t[]){o, 4, 0x8d2130, 0, 1}, "EnumDevices"), 0);
        CHECK_EQ(call_shim(thunk_fn(sg_ld32(M, vt + 0x08)), 4, 1, (uint32_t[]){o}, "Release"), 0);
    }
    CHECK_EQ(CALL(sg_imp_dinput8_DirectInput8Create, 20, 0x400000, 0x800, gbuf(16), 0, 0), 0x80004003u);
    /* stop predicate */
    unsetenv("SPORE_STOP_AT");
    CHECK(!hle_stop_wanted("CreateWindowExW"));
    setenv("SPORE_STOP_AT", "window", 1);
    CHECK(hle_stop_wanted("CreateWindowExW") && hle_stop_wanted("CreateWindowExA") && hle_stop_wanted("Direct3DCreate9"));
    CHECK(!hle_stop_wanted("RegisterClassExW"));
    setenv("SPORE_STOP_AT", "Direct3DCreate9", 1);
    CHECK(hle_stop_wanted("Direct3DCreate9") && !hle_stop_wanted("CreateWindowExW"));
    unsetenv("SPORE_STOP_AT");
}
