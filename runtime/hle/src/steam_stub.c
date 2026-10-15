/* steam_stub.c -- the three steam_api imports, returning what GOG's stand-in returns
 * (docs/design/win32-layer.md section 22.1).
 *
 * Source of every value: GOG's own 15 KB stand-in game/SporebinEP1/steam_api.dll (export
 * directory name "steam_api_sr3.dll", image base 0x3b400000), disassembled with Capstone.
 * Nothing from Valve's library or the Steam client was looked at.
 *   SteamAPI_Init     0x3b401110  mov al,1; ret            -> true (cdecl, no arguments)
 *   SteamAPI_Shutdown 0x3b4023c0  ret                      -> nothing
 *   SteamApps         0x3b401990  mov eax,0x3b404244; ret  -> a static object whose vptr is the
 *                     vtable 0x3b4032e8 (8 slots, all thiscall):
 *     +0x00 0x3b401110  -> 1 (al)
 *     +0x04 0x3b401120  -> 1 only if gog.ini [Config] LowViolence is "1/yes/true/enabled"; else 0
 *     +0x08 0x3b4022c0  -> 0 (al)
 *     +0x0c 0x3b4022c0  -> 0 (al)
 *     +0x10 0x3b401140  -> language string: gog.ini [Config] Language, else an LCID table, else
 *     +0x14 0x3b401140     "english"
 *     +0x18 0x3b401150 (u32 id, ret 4) -> 1 if id == 0xd7be, else the DLC check below
 *     +0x1c 0x3b401180 (u32 id, ret 4) -> DLC check: ids outside its 19-entry table -> 1; ids in
 *                     it -> gog.ini [EnableDLC] <name> if present, else 1 (no table entry has a
 *                     path, so the file-existence branch is never taken)
 * The Spore folder has no gog.ini [obs], so every INI read takes its "absent" branch: the values
 * below are the stand-in's values for this install. The game calls only slot +0x18, from
 * 0x6b4ed0 (one SteamApps call site), with an id read from property 0x7634d70 (0x686c15).
 * The stand-in sets only al in the bool returns; we return the full 0/1 in eax.
 * Language (+0x10/+0x14) is not called by the game; we return "english" without the LCID
 * table [inf: the stand-in's default on an en-US system]. */
#include <string.h>

#include "hle.h"
#include "hle_imports.h"

static uint32_t apps_obj; /* guest address of the object; its vtable follows it */

static void m_true(sg_ctx *SG_RESTRICT ctx, uint8_t *mem) { ctx->eax = 1; SG_RET(0); }
static void m_false(sg_ctx *SG_RESTRICT ctx, uint8_t *mem) { ctx->eax = 0; SG_RET(0); }
static void m_language(sg_ctx *SG_RESTRICT ctx, uint8_t *mem) {
    static uint32_t s;
    if (!s) {
        uint32_t a = hle_guest_alloc(8, 4);
        if (a) memcpy(mem + a, "english", 8);
        s = a;
    }
    ctx->eax = s;
    SG_RET(0);
}
static void m_subscribed_app(sg_ctx *SG_RESTRICT ctx, uint8_t *mem) { /* id == 0xd7be or DLC check: 1 */
    hle_log(HLE_LOG_INFO, "steam_api: SteamApps()+0x18(%u) -> 1", SG_ARG(0));
    ctx->eax = 1;
    SG_RET(4);
}
static void m_dlc(sg_ctx *SG_RESTRICT ctx, uint8_t *mem) {
    hle_log(HLE_LOG_INFO, "steam_api: SteamApps()+0x1c(%u) -> 1", SG_ARG(0));
    ctx->eax = 1;
    SG_RET(4);
}

uint32_t hle_steam_apps_object(uint8_t *mem); /* for tests */
uint32_t hle_steam_apps_object(uint8_t *mem) {
    static sg_func *const methods[8] = {m_true, m_false, m_false, m_false, m_language, m_language,
                                        m_subscribed_app, m_dlc};
    static const char *const names[8] = {"steam_apps+00", "steam_apps+04", "steam_apps+08", "steam_apps+0c",
                                         "steam_apps+10", "steam_apps+14", "steam_apps+18", "steam_apps+1c"};
    static int lock;
    while (__atomic_exchange_n(&lock, 1, __ATOMIC_ACQUIRE)) {
    }
    if (!apps_obj) {
        /* object (vptr) at +0, vtable (8 slots + a NULL terminator as in the stand-in) at +16 */
        uint32_t o = hle_guest_alloc(16 + 4 * 12, 16);
        if (o) {
            sg_st32(mem, o, o + 16);
            for (unsigned i = 0; i < 8; i++) sg_st32(mem, o + 16 + 4 * i, sg_thunk_register(methods[i], names[i]));
            __atomic_store_n(&apps_obj, o, __ATOMIC_RELEASE);
        }
    }
    __atomic_store_n(&lock, 0, __ATOMIC_RELEASE);
    return apps_obj;
}

SG_IMPORT(steam_api, SteamAPI_Init) {
    hle_log(HLE_LOG_INFO, "steam_api: SteamAPI_Init -> 1 (GOG stand-in)");
    ctx->eax = 1;
    SG_RET(0);
}

SG_IMPORT(steam_api, SteamAPI_Shutdown) { SG_RET(0); }

SG_IMPORT(steam_api, SteamApps) {
    ctx->eax = hle_steam_apps_object(mem);
    SG_RET(0);
}
