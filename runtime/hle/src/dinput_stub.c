/* dinput_stub.c -- DirectInput8Create with no game controllers attached
 * (docs/design/win32-layer.md 22.4). WEAK: the platform layer's GameController support replaces it.
 *
 * [obs] The gamepad manager 0x8d2180 (under config slot +0x14 0x7e6850) first polls XInput slots
 * 0..3; when none answers (ERROR_DEVICE_NOT_CONNECTED, user_prewindow.c) it calls
 * DirectInput8Create(GetModuleHandleA(NULL), 0x800, IID 0x14f8f74, &obj+0x364, NULL) at 0x8d2232
 * and then IDirectInput8::EnumDevices (slot +0x10) with DI8DEVCLASS_GAMECTRL (4), callback
 * 0x8d2130 and DIEDFL_ATTACHEDONLY (1), and counts the devices the callback saw (+0x368).
 * This object answers like a PC with no joystick: EnumDevices succeeds and calls nothing.
 * IDirectInput8 vtable (11 stdcall methods, `this` first): QueryInterface, AddRef, Release,
 * CreateDevice, EnumDevices, GetDeviceStatus, RunControlPanel, Initialize, FindDevice,
 * EnumDevicesBySemantics, ConfigureDevices. */
#include "hle.h"
#include "hle_imports.h"

#define DI_OK 0u
#define E_NOINTERFACE 0x80004002u
#define E_NOTIMPL 0x80004001u
#define E_POINTER 0x80004003u
#define DIERR_DEVICENOTREG 0x80040154u

static uint32_t di_obj;
static int di_refs;

static void m_qi(sg_ctx *SG_RESTRICT ctx, uint8_t *mem) {
    if (SG_ARG(2)) sg_st32(mem, SG_ARG(2), 0);
    SG_RETURN_STDCALL(E_NOINTERFACE, 12);
}
static void m_addref(sg_ctx *SG_RESTRICT ctx, uint8_t *mem) {
    SG_RETURN_STDCALL((uint32_t)__atomic_add_fetch(&di_refs, 1, __ATOMIC_SEQ_CST), 4);
}
static void m_release(sg_ctx *SG_RESTRICT ctx, uint8_t *mem) { /* the record is never freed (static, reused) */
    int r = __atomic_sub_fetch(&di_refs, 1, __ATOMIC_SEQ_CST);
    SG_RETURN_STDCALL(r < 0 ? 0u : (uint32_t)r, 4);
}
static void m_create_device(sg_ctx *SG_RESTRICT ctx, uint8_t *mem) { /* (this, rguid, ppDevice, pUnkOuter) */
    if (SG_ARG(2)) sg_st32(mem, SG_ARG(2), 0);
    SG_RETURN_STDCALL(DIERR_DEVICENOTREG, 16);
}
static void m_enum_devices(sg_ctx *SG_RESTRICT ctx, uint8_t *mem) { /* (this, type, cb, ref, flags): none */
    hle_log(HLE_LOG_INFO, "IDirectInput8::EnumDevices(type %u, flags 0x%x): no devices", SG_ARG(1), SG_ARG(4));
    SG_RETURN_STDCALL(DI_OK, 20);
}
static void m_device_status(sg_ctx *SG_RESTRICT ctx, uint8_t *mem) { SG_RETURN_STDCALL(DIERR_DEVICENOTREG, 8); }
static void m_ret_notimpl_8(sg_ctx *SG_RESTRICT ctx, uint8_t *mem) { SG_RETURN_STDCALL(E_NOTIMPL, 12); } /* RunControlPanel(this,hwnd,flags) */
static void m_initialize(sg_ctx *SG_RESTRICT ctx, uint8_t *mem) { SG_RETURN_STDCALL(DI_OK, 12); }      /* (this,hinst,ver) */
static void m_find_device(sg_ctx *SG_RESTRICT ctx, uint8_t *mem) { SG_RETURN_STDCALL(DIERR_DEVICENOTREG, 16); }
static void m_enum_semantics(sg_ctx *SG_RESTRICT ctx, uint8_t *mem) { SG_RETURN_STDCALL(DI_OK, 24); } /* none */
static void m_configure(sg_ctx *SG_RESTRICT ctx, uint8_t *mem) { SG_RETURN_STDCALL(E_NOTIMPL, 20); }

uint32_t hle_dinput8_object(uint8_t *mem); /* for tests */
uint32_t hle_dinput8_object(uint8_t *mem) {
    static sg_func *const m[11] = {m_qi, m_addref, m_release, m_create_device, m_enum_devices, m_device_status,
                                   m_ret_notimpl_8, m_initialize, m_find_device, m_enum_semantics, m_configure};
    static int lock;
    while (__atomic_exchange_n(&lock, 1, __ATOMIC_ACQUIRE)) {
    }
    if (!di_obj) {
        uint32_t o = hle_guest_alloc(16 + 4 * 12, 16);
        if (o) {
            sg_st32(mem, o, o + 16);
            for (unsigned i = 0; i < 11; i++) sg_st32(mem, o + 16 + 4 * i, sg_thunk_register(m[i], "IDirectInput8 (no devices)"));
            __atomic_store_n(&di_obj, o, __ATOMIC_RELEASE);
        }
    }
    __atomic_store_n(&lock, 0, __ATOMIC_RELEASE);
    return di_obj;
}

/* HRESULT DirectInput8Create(HINSTANCE, DWORD dwVersion, REFIID, LPVOID *ppvOut, LPUNKNOWN) */
__attribute__((weak)) SG_IMPORT(dinput8, DirectInput8Create) {
    uint32_t out = SG_ARG(3), o;
    if (!out) SG_RETURN_STDCALL(E_POINTER, 20);
    o = hle_dinput8_object(mem);
    sg_st32(mem, out, o);
    if (o) __atomic_add_fetch(&di_refs, 1, __ATOMIC_SEQ_CST);
    hle_log(HLE_LOG_INFO, "DirectInput8Create(version 0x%x) = 0x%08x (no game controllers)", SG_ARG(1), o);
    SG_RETURN_STDCALL(o ? DI_OK : 0x8007000eu /* E_OUTOFMEMORY */, 20);
}
