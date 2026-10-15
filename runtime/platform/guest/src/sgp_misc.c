/* sgp_misc.c -- GDI32 and DirectInput8 (IMM32 is in sgp_imm.c, XInput in sgp_xinput.c), the shim list for the
 * registration check, and window lookups for other packages.
 *
 * GDI32 [obs]: 0x75daad..0x75dae4 GetMonitorInfoA(MONITORINFOEX) -> CreateDCA(device, device) ->
 * GetDeviceCaps(HORZSIZE 4, VERTSIZE 6) for the physical aspect ratio; 0x11e1d2e CreateDCA("DISPLAY")
 * + ExtEscape(0x7306, ...) is a vendor driver query, answered "not supported" (0), as a driver
 * without it does. IMM32: no IME (ImmGetContext NULL); text input through NSTextInputClient is
 * remaining work. XInput: no pad (ERROR_DEVICE_NOT_CONNECTED). DirectInput8: no game controllers
 * (runtime/hle dinput_stub.c's object, reused). */
#include "sgp_int.h"

/* ---- GDI32 ------------------------------------------------------------------------------------- */
SG_IMPORT(gdi32, CreateSolidBrush) { SG_RETURN_STDCALL(sgp_pseudo_handle(), 4); }
SG_IMPORT(gdi32, DeleteObject) { SG_RETURN_STDCALL(SG_ARG(0) != 0, 4); }
SG_IMPORT(gdi32, CreateDCA) {
    char drv[64];
    sgp_astr(mem, SG_ARG(0), drv, sizeof drv);
    SGP_LOG("CreateDCA(\"%s\") = 0x%x", drv, SP_FAKE_HDC + 0x10u);
    SG_RETURN_STDCALL(SP_FAKE_HDC + 0x10u, 16);
}
SG_IMPORT(gdi32, DeleteDC) { SG_RETURN_STDCALL(1, 4); }
SG_IMPORT(gdi32, ExtEscape) {
    SGP_LOG("ExtEscape(0x%x, escape 0x%x, in %u, out %u) = 0 (not supported)", SG_ARG(0), SG_ARG(1), SG_ARG(2), SG_ARG(4));
    SG_RETURN_STDCALL(0, 24);
}
SG_IMPORT(gdi32, GetDeviceCaps) {
    int32_t i = (int32_t)SG_ARG(1), w, h, hz, mmw, mmh, r;
    sgp_ensure();
    sgp_be->screen(&w, &h, NULL, &hz, &mmw, &mmh);
    switch (i) {
    case 4: r = mmw; break;            /* HORZSIZE (mm) */
    case 6: r = mmh; break;            /* VERTSIZE */
    case 8: case 118: r = w; break;    /* HORZRES, DESKTOPHORZRES */
    case 10: case 117: r = h; break;   /* VERTRES, DESKTOPVERTRES */
    case 12: r = 32; break;            /* BITSPIXEL */
    case 14: r = 1; break;             /* PLANES */
    case 88: case 90: r = 96; break;   /* LOGPIXELSX/Y */
    case 116: r = hz; break;           /* VREFRESH */
    default: r = 0; break;
    }
    SGP_LOG("GetDeviceCaps(%d) = %d", i, r);
    SG_RETURN_STDCALL((uint32_t)r, 8);
}

/* ---- DirectInput8 ------------------------------------------------------------------------------- */
uint32_t hle_dinput8_object(uint8_t *mem); /* runtime/hle dinput_stub.c: IDirectInput8 with no devices */
SG_IMPORT(dinput8, DirectInput8Create) { /* (hinst, version, riid, ppvOut, punkOuter) */
    uint32_t out = SG_ARG(3), o, ref = 0;
    if (!out) SG_RETURN_STDCALL(0x80004003u /* E_POINTER */, 20);
    o = hle_dinput8_object(mem);
    sg_st32(mem, out, o);
    if (o) { /* AddRef through the object's own vtable slot 1, as a caller would */
        uint32_t a[1] = {o};
        SGP_ENTER();
        ref = sgp_guest_call(ctx, sg_ld32(mem, sg_ld32(mem, o) + 4), 1, a, "IDirectInput8::AddRef");
        SGP_LEAVE();
    }
    SGP_LOG("DirectInput8Create(version 0x%x) = 0x%08x, refs %u (no game controllers)", SG_ARG(1), o, ref);
    SG_RETURN_STDCALL(o ? 0u : 0x8007000eu, 20);
}

/* ---- KERNEL32 SetThreadExecutionState (0x849d7f, right after the window's SetTimer) -------------
 * Display/system sleep requests belong with the window layer. Records the state and returns the
 * previous one, as Windows does; a macOS power assertion (IOPMAssertion) is remaining work. Not in
 * this package's DLL list: if runtime/win32 or runtime/core ever defines it too, delete this one. */
SG_IMPORT(kernel32, SetThreadExecutionState) {
    static uint32_t state = 0x80000000u; /* ES_CONTINUOUS */
    uint32_t prev = state, f = SG_ARG(0);
    if (f & 0x80000000u) state = f;
    SGP_LOG("SetThreadExecutionState(0x%x) = 0x%x", f, prev);
    SG_RETURN_STDCALL(prev, 4);
}

/* ---- lookups for other packages ------------------------------------------------------------------ */
uint32_t sgp_main_window(void) {
    unsigned i;
    for (i = 0; i < SGP_MAX_WIN; i++)
        if (sgp_wins[i].used && sgp_wins[i].attached && sgp_wins[i].toplevel) return sgp_wins[i].h;
    for (i = 0; i < SGP_MAX_WIN; i++)
        if (sgp_wins[i].used && sgp_wins[i].toplevel && !sgp_wins[i].ansi) return sgp_wins[i].h;
    return 0;
}

void *sgp_metal_layer(uint32_t h) {
    sgp_win *w = sgp_win_find(h);
    return w && w->attached && sgp_be ? sgp_be->metal_layer(h) : NULL;
}

/* ---- the shim list (sgp.h) ----------------------------------------------------------------------- */
#define SGP_SHIM(dll, name, bytes) {"sg_imp_" #dll "_" #name, sg_imp_##dll##_##name, bytes},
const sgp_shim sgp_shims[] = {
#include "sgp_list.h"
};
const unsigned sgp_shim_count = sizeof sgp_shims / sizeof sgp_shims[0];
