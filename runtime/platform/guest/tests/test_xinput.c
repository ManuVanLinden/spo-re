/* test_xinput.c -- the four XInput imports as the game reaches them (stdcall through the fixture, as
 * translated code calls a GetProcAddress'd pointer), over a fake back end that injects pads into the users of
 * runtime/platform/macos/src/sp_gamepad_core.c. Checks the guest XINPUT_STATE / XINPUT_CAPABILITIES bytes,
 * packet numbers, empty users, bad arguments, rumble forwarding and the headset GUIDs. No game code. */
#include "fixture.h"
#include "sp_macos.h"

static int dev[5];
static int rum_calls;
static uint16_t rum_lo, rum_hi;
static void fake_rumble(void *d, uint16_t lo, uint16_t hi) { (void)d; rum_calls++; rum_lo = lo; rum_hi = hi; }

static uint32_t get_state(uint32_t user, uint32_t p) { return GSTD(sg_imp_xinput9_1_0_XInputGetState, user, p); }

int main(void) {
    if (fx_init()) { fprintf(stderr, "fixture init failed\n"); return 2; }
    uint32_t st = hle_guest_alloc(16, 4), caps = hle_guest_alloc(20, 4), vib = hle_guest_alloc(4, 4);
    uint32_t g1 = hle_guest_alloc(16, 4), g2 = hle_guest_alloc(16, 4);
    sp_pad_caps c;
    sp_pad_input in;

    /* no back end in this binary: every user is empty */
    memset(M + st, 0xab, 16);
    for (uint32_t u = 0; u < 4; u++) CHECK_EQ(get_state(u, st), 1167);
    CHECK_EQ(M[st], 0xab);                                          /* left untouched */
    CHECK_EQ(get_state(4, st), 160);                                /* ERROR_BAD_ARGUMENTS */
    CHECK_EQ(get_state(0, 0), 160);
    CHECK_EQ(GSTD(sg_imp_xinput9_1_0_XInputGetCapabilities, 0, 1, caps), 1167);
    CHECK_EQ(GSTD(sg_imp_xinput9_1_0_XInputGetCapabilities, 0, 2, caps), 160);
    CHECK_EQ(GSTD(sg_imp_xinput9_1_0_XInputSetState, 0, vib), 1167);
    CHECK_EQ(GSTD(sg_imp_xinput9_1_0_XInputGetDSoundAudioDeviceGuids, 0, g1, g2), 1167);
    CHECK_EQ(GSTD(sg_imp_xinput9_1_0_XInputGetDSoundAudioDeviceGuids, 9, g1, g2), 160);
    /* the four imports resolve to ours through GetProcAddress's table (the fourth was a weak stub) */
    {
        unsigned found = 0;
        for (unsigned i = 0; i < hle_dynamic_count; i++)
            if (!strcmp(hle_dynamics[i].module, "xinput9_1_0.dll") && !strncmp(hle_dynamics[i].name, "XInput", 6)) {
                const char *n = hle_dynamics[i].name;
                sg_func *want = !strcmp(n, "XInputGetState") ? sg_imp_xinput9_1_0_XInputGetState
                              : !strcmp(n, "XInputSetState") ? sg_imp_xinput9_1_0_XInputSetState
                              : !strcmp(n, "XInputGetCapabilities") ? sg_imp_xinput9_1_0_XInputGetCapabilities
                              : sg_imp_xinput9_1_0_XInputGetDSoundAudioDeviceGuids;
                CHECK(hle_dynamics[i].fn == want, "%s is ours", n);
                found++;
            }
        CHECK_EQ(found, 4);
        CHECK(hle_resolve("xinput9_1_0.dll", "XInputGetDSoundAudioDeviceGuids", 0) != 0, "GetProcAddress finds the fourth");
    }

    /* fake back end: pads on users 0 and 2 (user 1's pad left) */
    memset(&c, 0, sizeof c);
    c.subtype = 1;
    c.buttons = 0xf3ffu & ~SP_PAD_BACK;
    c.triggers = c.sticks = 1;
    c.rumble = 1;
    sp_pad_set_rumble_handler(fake_rumble);
    CHECK_EQ(sp_pad_connect(&dev[0], &c), 0);
    CHECK_EQ(sp_pad_connect(&dev[1], &c), 1);
    c.rumble = 0;
    CHECK_EQ(sp_pad_connect(&dev[2], &c), 2);
    CHECK_EQ(sp_pad_disconnect(&dev[1]), 1);

    memset(&in, 0, sizeof in);
    in.buttons = SP_PAD_A | SP_PAD_DPAD_UP | SP_PAD_RIGHT_SHOULDER;
    in.lt = 1.0f; in.rt = 0.25f;
    in.lx = -1.0f; in.ly = 1.0f; in.rx = 0.5f; in.ry = -0.25f;
    CHECK_EQ(sp_pad_update(&dev[0], &in), 1);
    memset(M + st, 0xab, 16);
    CHECK_EQ(get_state(0, st), 0);
    uint32_t pk = sg_ld32(M, st);
    CHECK_EQ(sg_ld16(M, st + 4), 0x1201u);
    CHECK_EQ(M[st + 6], 255);
    CHECK_EQ(M[st + 7], 64);                                        /* 63.75 -> 64 */
    CHECK_EQ((int16_t)sg_ld16(M, st + 8), -32768);
    CHECK_EQ((int16_t)sg_ld16(M, st + 10), 32767);
    CHECK_EQ((int16_t)sg_ld16(M, st + 12), 16384);
    CHECK_EQ((int16_t)sg_ld16(M, st + 14), -8192);
    /* same state polled again (every frame): same packet; a change: a new one */
    CHECK_EQ(get_state(0, st), 0);
    CHECK_EQ(sg_ld32(M, st), pk);
    CHECK_EQ(sp_pad_update(&dev[0], &in), 0);
    CHECK_EQ(get_state(0, st), 0);
    CHECK_EQ(sg_ld32(M, st), pk);
    in.buttons = 0;
    sp_pad_update(&dev[0], &in);
    CHECK_EQ(get_state(0, st), 0);
    CHECK(sg_ld32(M, st) != pk, "packet changed with the state");
    CHECK_EQ(sg_ld16(M, st + 4), 0);
    /* the empty user between two pads, and the pad at rest on user 2 */
    CHECK_EQ(get_state(1, st), 1167);
    CHECK_EQ(get_state(3, st), 1167);
    memset(M + st, 0xab, 16);
    CHECK_EQ(get_state(2, st), 0);
    CHECK(sg_ld16(M, st + 4) == 0 && M[st + 6] == 0 && sg_ld32(M, st + 8) == 0 && sg_ld32(M, st + 12) == 0, "user 2 at rest");

    /* capabilities: 20 bytes */
    memset(M + caps, 0xab, 20);
    CHECK_EQ(GSTD(sg_imp_xinput9_1_0_XInputGetCapabilities, 0, 1, caps), 0);
    CHECK_EQ(M[caps], 1);                                           /* XINPUT_DEVTYPE_GAMEPAD */
    CHECK_EQ(M[caps + 1], 1);                                       /* XINPUT_DEVSUBTYPE_GAMEPAD */
    CHECK_EQ(sg_ld16(M, caps + 2), 0);
    CHECK_EQ(sg_ld16(M, caps + 4), 0xf3dfu);                        /* no BACK on this profile */
    CHECK(M[caps + 6] == 0xff && M[caps + 7] == 0xff, "triggers");
    CHECK(sg_ld16(M, caps + 8) == 0xffc0u && sg_ld16(M, caps + 14) == 0xffc0u, "sticks");
    CHECK(sg_ld16(M, caps + 16) == 0xffff && sg_ld16(M, caps + 18) == 0xffff, "motors on a pad with haptics");
    CHECK_EQ(GSTD(sg_imp_xinput9_1_0_XInputGetCapabilities, 2, 0, caps), 0);
    CHECK(sg_ld16(M, caps + 16) == 0 && sg_ld16(M, caps + 18) == 0, "no motors without haptics");

    /* rumble */
    sg_st16(M, vib, 0x4000); sg_st16(M, vib + 2, 0xc000);
    rum_calls = 0;
    CHECK_EQ(GSTD(sg_imp_xinput9_1_0_XInputSetState, 0, vib), 0);
    CHECK(rum_calls == 1 && rum_lo == 0x4000 && rum_hi == 0xc000, "rumble forwarded (%d calls)", rum_calls);
    CHECK_EQ(GSTD(sg_imp_xinput9_1_0_XInputSetState, 0, vib), 0);
    CHECK_EQ(rum_calls, 1);
    CHECK_EQ(GSTD(sg_imp_xinput9_1_0_XInputSetState, 2, vib), 0);    /* accepted, nothing to drive */
    CHECK_EQ(rum_calls, 1);
    CHECK_EQ(GSTD(sg_imp_xinput9_1_0_XInputSetState, 1, vib), 1167);

    /* headset GUIDs: none, GUID_NULL */
    memset(M + g1, 0xab, 16);
    memset(M + g2, 0xab, 16);
    CHECK_EQ(GSTD(sg_imp_xinput9_1_0_XInputGetDSoundAudioDeviceGuids, 0, g1, g2), 0);
    CHECK(sg_ld32(M, g1) == 0 && sg_ld32(M, g1 + 12) == 0 && sg_ld32(M, g2) == 0 && sg_ld32(M, g2 + 12) == 0, "GUID_NULL");

    /* hot-unplug: the next poll says disconnected */
    sp_pad_disconnect(&dev[0]);
    CHECK_EQ(get_state(0, st), 1167);
    sp_pad_reset();
    printf("xinput tests: %d passed, %d failed\n", t_pass, t_fail);
    return t_fail ? 1 : 0;
}
