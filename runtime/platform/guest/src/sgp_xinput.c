/* sgp_xinput.c -- XInput over the GameController framework.
 *
 * The game resolves the four imports with LoadLibraryA("xinput9_1_0.dll") + GetProcAddress (0x8d342d-0x8d3484)
 * and only ever calls XInputGetState: the manager's init (0x8d2180) probes users 0-3 once (any answer skips
 * DirectInput8), then marks all four slots as XInput users and polls each with XInputGetState every frame
 * (0x8d2920), treating a non-zero result as "disconnected" and doing its own dead zone on the sticks. So a pad
 * plugged in after start-up is picked up without any re-initialisation.
 *
 * The state lives in runtime/platform/macos (sp_gamepad_core.c: four users, a lock, packet numbers; the
 * GameController back end sp_gamepad.m feeds it). Headless builds have no back end: every user is empty and the
 * imports answer ERROR_DEVICE_NOT_CONNECTED. SPORE_GAMEPAD=0 does the same in the game. */
#include "sgp_int.h"
#include "sp_macos.h"

#define ERROR_SUCCESS_ 0u
#define ERROR_BAD_ARGUMENTS 160u
#define ERROR_DEVICE_NOT_CONNECTED 1167u
#define XINPUT_DEVTYPE_GAMEPAD 1u
#define XINPUT_FLAG_GAMEPAD 1u

/* ---- XInput (dynamic: GetProcAddress on xinput9_1_0.dll) ----------------------------------------- */

/* DWORD XInputGetState(DWORD dwUserIndex, XINPUT_STATE *pState); XINPUT_STATE = { DWORD dwPacketNumber;
 * XINPUT_GAMEPAD { WORD wButtons; BYTE bLeftTrigger, bRightTrigger; SHORT sThumbLX, LY, RX, RY; } }, 16 bytes.
 * An empty user leaves *pState untouched. */
SG_IMPORT(xinput9_1_0, XInputGetState) {
    uint32_t user = SG_ARG(0), p = SG_ARG(1), packet = 0;
    sp_pad_xstate x;
    sp_pad_start();
    if (user >= SP_PAD_USERS || !p) SG_RETURN_STDCALL(ERROR_BAD_ARGUMENTS, 8);
    if (sp_pad_get_state((int)user, &packet, &x)) SG_RETURN_STDCALL(ERROR_DEVICE_NOT_CONNECTED, 8);
    sg_st32(mem, p, packet);
    sg_st16(mem, p + 4, x.buttons);
    mem[p + 6] = x.lt;
    mem[p + 7] = x.rt;
    sg_st16(mem, p + 8, (uint16_t)x.lx);
    sg_st16(mem, p + 10, (uint16_t)x.ly);
    sg_st16(mem, p + 12, (uint16_t)x.rx);
    sg_st16(mem, p + 14, (uint16_t)x.ry);
    SG_RETURN_STDCALL(ERROR_SUCCESS_, 8);
}

/* DWORD XInputSetState(DWORD dwUserIndex, XINPUT_VIBRATION *pVibration); { WORD wLeftMotorSpeed (low
 * frequency), wRightMotorSpeed (high frequency) }. Rumble goes to the pad's haptics when it has them (queued
 * on the back end; never blocks); otherwise accepted and dropped, as a pad without motors would. */
SG_IMPORT(xinput9_1_0, XInputSetState) {
    uint32_t user = SG_ARG(0), p = SG_ARG(1);
    sp_pad_start();
    if (user >= SP_PAD_USERS || !p) SG_RETURN_STDCALL(ERROR_BAD_ARGUMENTS, 8);
    if (sp_pad_set_vibration((int)user, sg_ld16(mem, p), sg_ld16(mem, p + 2)))
        SG_RETURN_STDCALL(ERROR_DEVICE_NOT_CONNECTED, 8);
    SG_RETURN_STDCALL(ERROR_SUCCESS_, 8);
}

/* DWORD XInputGetCapabilities(DWORD dwUserIndex, DWORD dwFlags, XINPUT_CAPABILITIES *pCaps);
 * { BYTE Type; BYTE SubType; WORD Flags; XINPUT_GAMEPAD Gamepad; XINPUT_VIBRATION Vibration; }, 20 bytes.
 * Gamepad holds the buttons the profile has, 0xFF for each trigger and 0xFFC0 per stick axis (the resolution
 * mask a wired Xbox 360 pad reports); Vibration 0xFFFF per motor when the pad has haptics. */
SG_IMPORT(xinput9_1_0, XInputGetCapabilities) {
    uint32_t user = SG_ARG(0), flags = SG_ARG(1), p = SG_ARG(2);
    sp_pad_caps c;
    sp_pad_start();
    if (user >= SP_PAD_USERS || (flags & ~XINPUT_FLAG_GAMEPAD) || !p) SG_RETURN_STDCALL(ERROR_BAD_ARGUMENTS, 12);
    if (sp_pad_get_caps((int)user, &c)) SG_RETURN_STDCALL(ERROR_DEVICE_NOT_CONNECTED, 12);
    memset(mem + p, 0, 20);
    mem[p] = XINPUT_DEVTYPE_GAMEPAD;
    mem[p + 1] = c.subtype ? c.subtype : 1;
    sg_st16(mem, p + 4, c.buttons & 0xf3ffu);
    mem[p + 6] = c.triggers ? 0xff : 0;
    mem[p + 7] = c.triggers ? 0xff : 0;
    for (int i = 0; i < 4; i++) sg_st16(mem, p + 8 + 2 * (uint32_t)i, c.sticks ? 0xffc0u : 0);
    sg_st16(mem, p + 16, c.rumble ? 0xffffu : 0);
    sg_st16(mem, p + 18, c.rumble ? 0xffffu : 0);
    SG_RETURN_STDCALL(ERROR_SUCCESS_, 12);
}

/* DWORD XInputGetDSoundAudioDeviceGuids(DWORD dwUserIndex, GUID *pRender, GUID *pCapture): the headset on
 * the pad. No pad has one here: a connected user gets GUID_NULL for both (as Windows reports a pad without a
 * headset), an empty user ERROR_DEVICE_NOT_CONNECTED. Strong over runtime/hle's weak stub. */
SG_IMPORT(xinput9_1_0, XInputGetDSoundAudioDeviceGuids) {
    uint32_t user = SG_ARG(0), r = SG_ARG(1), c = SG_ARG(2);
    sp_pad_start();
    if (user >= SP_PAD_USERS) SG_RETURN_STDCALL(ERROR_BAD_ARGUMENTS, 12);
    if (sp_pad_get_state((int)user, NULL, NULL)) SG_RETURN_STDCALL(ERROR_DEVICE_NOT_CONNECTED, 12);
    if (r) memset(mem + r, 0, 16);
    if (c) memset(mem + c, 0, 16);
    SG_RETURN_STDCALL(ERROR_SUCCESS_, 12);
}
