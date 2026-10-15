/* test_reg.c -- the registry store, its GOG seed, and the advapi32 shims as the game calls them. */
#include "harness.h"
#include "w32_int.h"

#include <ctype.h>
#include <stdlib.h>

/* RegOpenKeyExW(root, subkey) -> handle or 0 */
static uint32_t open_w(uint32_t root, const char *sub) {
    uint32_t out = gbuf(4);
    uint32_t r = CALL(w32_imp_advapi32_RegOpenKeyExW, 20, root, gwstr(sub), 0, 1 /* KEY_QUERY_VALUE */, out);
    return r ? 0 : sg_ld32(M, out);
}

/* The DWORD read of 0x6ab6f0: type query with NULL data and NULL size, then a 4-byte read. */
static int read_dword_like_game(uint32_t root, const char *sub, const char *name, uint32_t *v) {
    uint32_t h = open_w(root, sub), t = gbuf(4), d = gbuf(4), cb = gbuf(4);
    if (!h) return 0;
    int ok = 0;
    if (!CALL(w32_imp_advapi32_RegQueryValueExW, 24, h, gwstr(name), 0, t, 0, 0) && sg_ld32(M, t) == 4) {
        sg_st32(M, cb, 4);
        if (!CALL(w32_imp_advapi32_RegQueryValueExW, 24, h, gwstr(name), 0, t, d, cb)) { *v = sg_ld32(M, d); ok = 1; }
    }
    CALL(w32_imp_advapi32_RegCloseKey, 4, h);
    return ok;
}

static const char *read_sz_w(uint32_t root, const char *sub, const char *name) {
    static char out[512];
    uint32_t h = open_w(root, sub), t = gbuf(4), cb = gbuf(4), d = gbuf(1024);
    out[0] = 0;
    if (!h) return "(no key)";
    sg_st32(M, cb, 1024);
    uint32_t r = CALL(w32_imp_advapi32_RegQueryValueExW, 24, h, gwstr(name), 0, t, d, cb);
    CALL(w32_imp_advapi32_RegCloseKey, 4, h);
    if (r) return "(no value)";
    if (sg_ld32(M, t) != W32_REG_SZ) return "(not REG_SZ)";
    snprintf(out, sizeof out, "%s", hwstr(d));
    return out;
}

void test_reg(void) {
    uint32_t v = 0;

    /* the GOG values, as the game reads them */
    CHECK_STR(read_sz_w(W32_HKLM, "Software\\Electronic Arts\\SPORE", "PlayerDir"), "My Spore Creations");
    CHECK_STR(read_sz_w(W32_HKLM, "Software\\Electronic Arts\\SPORE", "AppDir"), "Spore");
    CHECK_STR(read_sz_w(W32_HKLM, "SOFTWARE\\Electronic Arts\\SPORE", "DataDir"), "C:\\Spore\\Data");
    CHECK_STR(read_sz_w(W32_HKLM, "SOFTWARE\\Electronic Arts\\SPORE", "Locale"), "en-us");
    CHECK_STR(read_sz_w(W32_HKLM, "SOFTWARE\\Electronic Arts\\spore_EP1", "DataDir"), "C:\\Spore\\DataEP1\\");
    CHECK(read_dword_like_game(W32_HKLM, "SOFTWARE\\Electronic Arts\\spore_EP1", "AddOnID", &v));
    CHECK_EQ(v, 2);
    CHECK(read_dword_like_game(W32_HKLM, "SOFTWARE\\Electronic Arts\\spore_EP1", "PackID", &v));
    CHECK_EQ(v, 128448390);
    CHECK(read_dword_like_game(W32_HKLM, "SOFTWARE\\Electronic Arts\\spore Creepy and Cute Parts Pack", "AddOnID", &v));
    CHECK_EQ(v, 1);
    CHECK(read_dword_like_game(W32_HKLM, "SOFTWARE\\Electronic Arts\\SPORE", "installcompleted", &v));
    CHECK_EQ(v, 1);
    CHECK(!read_dword_like_game(W32_HKLM, "SOFTWARE\\Electronic Arts\\SPORE", "appdir", &v)); /* a string, not a DWORD */
    /* the 32-bit view alias */
    CHECK_STR(read_sz_w(W32_HKLM, "SOFTWARE\\WOW6432Node\\Electronic Arts\\SPORE", "locale"), "en-us");
    /* absent things */
    CHECK_EQ(open_w(W32_HKLM, "SOFTWARE\\Electronic Arts\\EA Games\\SPORE(TM)\\ergc"), 0);
    CHECK(open_w(W32_HKLM, "SOFTWARE\\Electronic Arts\\EA Games\\SPORE(TM)") != 0); /* empty key exists */
    CHECK_STR(read_sz_w(W32_HKLM, "SOFTWARE\\Electronic Arts\\SPORE", "ProductKey"), "(no value)");

    /* the include filter of 0x687bd0 over RegEnumKeyExW: lower-case each subkey, keep names
     * containing one of six strings [obs] */
    static const char *const inc[] = {"spore_bp", "spore creepy and cute parts pack", "spore(tm) creepy & cute parts pack",
                                      "spore bp", "spore_ep", "test"};
    uint32_t ea = open_w(W32_HKLM, "SOFTWARE\\Electronic Arts");
    CHECK(ea != 0);
    uint32_t name = gbuf(512), cch = gbuf(4);
    char seen[8][128];
    int n = 0, matched = 0;
    for (uint32_t i = 0;; i++) {
        sg_st32(M, cch, 256);
        uint32_t r = CALL(w32_imp_advapi32_RegEnumKeyExW, 32, ea, i, name, cch, 0, 0, 0, 0);
        if (r == W32_ERROR_NO_MORE_ITEMS) break;
        CHECK_EQ(r, 0);
        if (r || n == 8) break;
        snprintf(seen[n], sizeof seen[n], "%s", hwstr(name));
        CHECK_EQ(sg_ld32(M, cch), strlen(seen[n]));
        char low[128];
        for (size_t k = 0; k <= strlen(seen[n]); k++) low[k] = (char)tolower((unsigned char)seen[n][k]);
        for (int k = 0; k < 6; k++) if (strstr(low, inc[k])) { matched++; break; }
        n++;
    }
    CHECK_EQ(n, 4); /* ea games, spore, spore Creepy and Cute Parts Pack, spore_EP1 (sorted) */
    CHECK_STR(seen[0], "ea games");
    CHECK_STR(seen[1], "spore");
    CHECK_STR(seen[2], "spore Creepy and Cute Parts Pack");
    CHECK_STR(seen[3], "spore_EP1");
    CHECK_EQ(matched, 2); /* both add-on keys pass, the base "spore" key does not */
    sg_st32(M, cch, 3);
    CHECK_EQ(CALL(w32_imp_advapi32_RegEnumKeyExW, 32, ea, 1, name, cch, 0, 0, 0, 0), W32_ERROR_MORE_DATA);
    CHECK_EQ(CALL(w32_imp_advapi32_RegCloseKey, 4, ea), 0);
    CHECK_EQ(CALL(w32_imp_advapi32_RegCloseKey, 4, ea), W32_ERROR_INVALID_HANDLE);

    /* sizes and MORE_DATA */
    uint32_t h = open_w(W32_HKLM, "SOFTWARE\\Electronic Arts\\SPORE"), t = gbuf(4), cb = gbuf(4), d = gbuf(64);
    CHECK_EQ(CALL(w32_imp_advapi32_RegQueryValueExW, 24, h, gwstr("appdir"), 0, t, 0, cb), 0);
    CHECK_EQ(sg_ld32(M, cb), 12); /* "Spore" + NUL in UTF-16 */
    sg_st32(M, cb, 4);
    CHECK_EQ(CALL(w32_imp_advapi32_RegQueryValueExW, 24, h, gwstr("appdir"), 0, t, d, cb), W32_ERROR_MORE_DATA);
    CHECK_EQ(sg_ld32(M, cb), 12);
    CHECK_EQ(CALL(w32_imp_advapi32_RegQueryValueExW, 24, h, gwstr("nothere"), 0, t, d, cb), W32_ERROR_FILE_NOT_FOUND);
    CHECK_EQ(CALL(w32_imp_advapi32_RegQueryValueExW, 24, h, gwstr("appdir"), 0, t, d, 0), W32_ERROR_INVALID_PARAMETER);
    /* the ANSI view */
    uint32_t ha = gbuf(4);
    CHECK_EQ(CALL(w32_imp_advapi32_RegOpenKeyExA, 20, W32_HKLM, gstr("software\\electronic arts\\spore"), 0, 1, ha), 0);
    sg_st32(M, cb, 64);
    CHECK_EQ(CALL(w32_imp_advapi32_RegQueryValueExA, 24, sg_ld32(M, ha), gstr("PLAYERDIR"), 0, t, d, cb), 0);
    CHECK_STR((const char *)M + d, "My Spore Creations");
    CHECK_EQ(sg_ld32(M, cb), 19);

    /* the network check's write (0x94c48b): RegSetValueExA on HKCU Internet Settings */
    uint32_t hk = gbuf(4);
    CHECK_EQ(CALL(w32_imp_advapi32_RegOpenKeyExA, 20, W32_HKCU,
                  gstr("Software\\Microsoft\\Windows\\CurrentVersion\\Internet Settings"), 0, 0x20006, hk), 0);
    uint32_t one = gbuf(4);
    sg_st32(M, one, 1);
    CHECK_EQ(CALL(w32_imp_advapi32_RegSetValueExA, 24, sg_ld32(M, hk), gstr("EnableAutodial"), 0, W32_REG_DWORD, one, 4), 0);
    sg_st32(M, cb, 4);
    CHECK_EQ(CALL(w32_imp_advapi32_RegQueryValueExA, 24, sg_ld32(M, hk), gstr("enableautodial"), 0, t, d, cb), 0);
    CHECK_EQ(sg_ld32(M, d), 1);
    CHECK_EQ(CALL(w32_imp_advapi32_RegSetValueExA, 24, sg_ld32(M, hk), gstr("Text"), 0, W32_REG_SZ, gstr("café"), 5), 0);

    /* store: the seed file was written; a save and reload round-trips the edits */
    char path[1100];
    snprintf(path, sizeof path, "%s/support/registry.json", t_root);
    FILE *f = fopen(path, "rb");
    CHECK(f != NULL);
    if (f) fclose(f);
    CHECK_EQ(w32_reg_save(), 0);
    CHECK_EQ(w32_reg_load(path, 0), 0);
    uint32_t hu = 0, ty = 0, len = 64;
    uint8_t buf[64];
    CHECK_EQ(w32_reg_open(W32_HKCU, "Software\\Microsoft\\Windows\\CurrentVersion\\Internet Settings", 0, &hu), 0);
    CHECK_EQ(w32_reg_query(hu, "EnableAutodial", &ty, buf, &len, 0), 0);
    CHECK_EQ(ty, W32_REG_DWORD);
    CHECK_EQ(buf[0], 1);
    len = 64;
    CHECK_EQ(w32_reg_query(hu, "text", &ty, buf, &len, 1), 0); /* UTF-16 view of the 1252 text */
    CHECK_EQ(len, 10);
    CHECK_EQ(buf[6], 0xe9);
    len = 64;
    CHECK_EQ(w32_reg_query(hu, "text", &ty, buf, &len, 0), 0);
    CHECK_EQ(buf[3], 0xe9);
    w32_reg_close(hu);

    /* a damaged store is reported, not half-loaded silently */
    f = fopen(path, "wb");
    fputs("{\"format\": 1, \"keys\": {\"HKLM\\\\x\": {\"a\": ", f);
    fclose(f);
    CHECK_EQ(w32_reg_load(path, 0), -1);
    CHECK_STR(read_sz_w(W32_HKLM, "SOFTWARE\\Electronic Arts\\SPORE", "appdir"), "Spore"); /* seed in memory */
    CHECK_EQ(w32_reg_save(), -1); /* and the damaged file is left alone */
    /* missing file: re-seeded */
    remove(path);
    CHECK_EQ(w32_reg_load(path, 0), 0);
    CHECK_STR(read_sz_w(W32_HKLM, "SOFTWARE\\Electronic Arts\\SPORE", "playerdir"), "My Spore Creations");

    /* the seed itself parses and carries {app} */
    char *seed = w32_reg_seed_json("D:\\Games\\Spore");
    CHECK(strstr(seed, "D:\\\\Games\\\\Spore\\\\DataEP1\\\\") != NULL);
    free(seed);
}
