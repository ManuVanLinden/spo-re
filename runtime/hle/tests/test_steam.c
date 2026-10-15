/* steam_api stand-in stubs: the values GOG's stand-in steam_api.dll returns with no
 * gog.ini, and the object/vtable shape the game's one SteamApps call site (0x6b4ed0) uses. */
#include "harness.h"

uint32_t hle_steam_apps_object(uint8_t *mem);

static uint32_t call_method(uint32_t obj, unsigned slot, unsigned cleanup, unsigned n, const uint32_t *args) {
    uint32_t vt = sg_ld32(M, obj), fn = sg_ld32(M, vt + 4 * slot);
    sg_func *f = (fn - SG_THUNK_BASE) / SG_THUNK_SLOT < (SG_THUNK_LIMIT - SG_THUNK_BASE) / SG_THUNK_SLOT ? sg_thunk_fn[(fn - SG_THUNK_BASE) / SG_THUNK_SLOT] : NULL;
    CHECK(f != NULL);
    if (!f) return 0xdeadu;
    T.ecx = obj; /* thiscall */
    return call_shim(f, cleanup, n, args, "SteamApps method");
}

void test_steam(void) {
    uint32_t obj, a;
    CHECK_EQ(CALL0(sg_imp_steam_api_SteamAPI_Init, 0), 1);
    T.eax = 0x1234;
    CALL0(sg_imp_steam_api_SteamAPI_Shutdown, 0);
    obj = CALL0(sg_imp_steam_api_SteamApps, 0);
    CHECK(obj != 0);
    CHECK_EQ(CALL0(sg_imp_steam_api_SteamApps, 0), obj); /* one static object, as in the stand-in */
    CHECK(sg_ld32(M, obj) != 0);
    CHECK_EQ(sg_ld32(M, sg_ld32(M, obj) + 4 * 8), 0); /* 8 slots, then NULL */
    CHECK_EQ(call_method(obj, 0, 0, 0, NULL), 1);
    CHECK_EQ(call_method(obj, 1, 0, 0, NULL), 0); /* LowViolence: no gog.ini -> 0 */
    CHECK_EQ(call_method(obj, 2, 0, 0, NULL), 0);
    CHECK_EQ(call_method(obj, 3, 0, 0, NULL), 0);
    a = call_method(obj, 4, 0, 0, NULL);
    CHECK_STR(hstr(a), "english");
    CHECK_EQ(call_method(obj, 5, 0, 0, NULL), a);
    CHECK_EQ(call_method(obj, 6, 4, 1, (uint32_t[]){0xd7be}), 1);
    CHECK_EQ(call_method(obj, 6, 4, 1, (uint32_t[]){17390}), 1);   /* any id outside the DLC table */
    CHECK_EQ(call_method(obj, 6, 4, 1, (uint32_t[]){55380}), 1);   /* in the table, no gog.ini */
    CHECK_EQ(call_method(obj, 7, 4, 1, (uint32_t[]){24720}), 1);
}
