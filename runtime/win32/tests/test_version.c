/* test_version.c -- VerQueryValue over StringFileInfo/VarFileInfo: a version block built
 * here, and the real exe's resource when the GOG files are present (read at run time from the
 * Spore folder; nothing from it is stored). */
#include "harness.h"
#include "w32_dirwatch.h"
#include "w32_int.h"

#include <stdlib.h>

/* ---- a small VS_VERSIONINFO writer --------------------------------------------------------- */

static uint32_t wr_key(uint32_t at, const char *k) {
    uint32_t p = at + 6;
    for (; *k; k++, p += 2) sg_st16(M, p, (uint8_t)*k);
    sg_st16(M, p, 0);
    return (p + 2 + 3) & ~3u;
}
/* text node with a value (or a block when v == NULL); returns its end (4-aligned) */
static uint32_t wr_str(uint32_t at, const char *k, const char *v) {
    uint32_t p = wr_key(at, k), n = 0;
    if (v) { for (; v[n]; n++) sg_st16(M, p + 2 * n, (uint8_t)v[n]); sg_st16(M, p + 2 * n, 0); n++; }
    uint32_t end = p + 2 * n;
    sg_st16(M, at, (uint16_t)(end - at));
    sg_st16(M, at + 2, (uint16_t)n);
    sg_st16(M, at + 4, 1);
    return (end + 3) & ~3u;
}
static void close_node(uint32_t at, uint32_t end) { sg_st16(M, at, (uint16_t)(end - at)); }

static uint32_t build_block(uint32_t b) {
    memset(M + b, 0, 1024);
    /* root */
    uint32_t p = wr_key(b, "VS_VERSION_INFO");
    sg_st16(M, b + 2, 0x34);
    sg_st16(M, b + 4, 0);
    sg_st32(M, p, 0xfeef04bdu);
    sg_st32(M, p + 8, 0x00010002);
    sg_st32(M, p + 12, 0x00030004);
    p += 0x34;
    uint32_t sfi = p;
    p = wr_key(sfi, "StringFileInfo");
    sg_st16(M, sfi + 4, 1);
    uint32_t tab = p;
    p = wr_key(tab, "040904b0");
    sg_st16(M, tab + 4, 1);
    p = wr_str(p, "CompanyName", "Test Co");
    p = wr_str(p, "FileVersion", "1.2.3.4");
    p = wr_str(p, "Comments", "caf\xe9"); /* Latin-1 e-acute as a UTF-16 unit */
    close_node(tab, p);
    close_node(sfi, p);
    uint32_t vfi = p;
    p = wr_key(vfi, "VarFileInfo");
    sg_st16(M, vfi + 4, 1);
    uint32_t var = p;
    p = wr_key(var, "Translation");
    sg_st32(M, p, 0x04b00409);
    sg_st16(M, var + 2, 4);
    sg_st16(M, var + 4, 0);
    p += 4;
    close_node(var, p);
    close_node(vfi, p);
    close_node(b, p);
    return p - b;
}

static void real_exe(void) {
    const char *gd = getenv("SPORE_GAME_DIR");
    char path[2048];
    snprintf(path, sizeof path, "%s/SporebinEP1/SporeApp.exe", gd ? gd : "../../game");
    FILE *f = fopen(path, "rb");
    if (!f) { fprintf(stderr, "   (real exe not found at %s: skipped)\n", path); return; }
    uint8_t hdr[0x1000];
    if (fread(hdr, 1, sizeof hdr, f) != sizeof hdr) { fclose(f); return; }
    uint32_t pe = hdr[0x3c] | hdr[0x3d] << 8, nsec = hdr[pe + 6] | hdr[pe + 7] << 8, opt = hdr[pe + 20] | hdr[pe + 21] << 8;
    memcpy(M + SG_IMAGE_BASE, hdr, sizeof hdr);
    int loaded = 0;
    for (uint32_t i = 0; i < nsec; i++) {
        const uint8_t *s = hdr + pe + 24 + opt + 40 * i;
        if (memcmp(s, ".rsrc", 6)) continue;
        uint32_t va = (s[12] | s[13] << 8 | s[14] << 16 | (uint32_t)s[15] << 24);
        uint32_t rs = s[16] | s[17] << 8 | s[18] << 16 | (uint32_t)s[19] << 24;
        uint32_t rp = s[20] | s[21] << 8 | s[22] << 16 | (uint32_t)s[23] << 24;
        if (va + rs > SG_IMAGE_SIZE) break;
        fseek(f, rp, SEEK_SET);
        loaded = fread(M + SG_IMAGE_BASE + va, 1, rs, f) == rs;
    }
    fclose(f);
    CHECK(loaded);
    if (!loaded) return;
    uint32_t name = gstr("SporeApp.exe"), h = gbuf(4);
    uint32_t size = CALL(w32_imp_version_GetFileVersionInfoSizeA, 8, name, h);
    CHECK(size > 1300);
    uint32_t blk = gbuf(size);
    CHECK_EQ(CALL(w32_imp_version_GetFileVersionInfoA, 16, name, 0, size, blk), 1);
    uint32_t ptr, len, err;
    CHECK_EQ(w32_ver_query(M, blk, "\\StringFileInfo\\040904b0\\FileVersion", 1, &ptr, &len, &err), 1);
    CHECK_STR(hwstr(ptr), "3.1.0.29");
    CHECK_EQ(len, 9);
    CHECK_EQ(w32_ver_query(M, blk, "\\StringFileInfo\\040904B0\\OriginalFilename", 0, &ptr, &len, &err), 1);
    CHECK_STR((const char *)M + ptr, "SporeApp.exe");
    CHECK(ptr >= blk + sg_ld16(M, blk) && ptr + len <= blk + size); /* the ANSI copy lies in the spare area */
    CHECK_EQ(w32_ver_query(M, blk, "\\VarFileInfo\\Translation", 0, &ptr, &len, &err), 1);
    CHECK_EQ(len, 4);
    CHECK_EQ(sg_ld32(M, ptr), 0x04b00409);
    const char *names[] = {"CompanyName", "FileDescription", "FileVersion", "InternalName", "LegalCopyright",
                           "LegalTrademarks", "OriginalFilename", "ProductName", "ProductVersion", "SpecialBuild"};
    int found = 0;
    for (unsigned i = 0; i < sizeof names / sizeof names[0]; i++) {
        char sub[128];
        snprintf(sub, sizeof sub, "\\StringFileInfo\\040904b0\\%s", names[i]);
        found += w32_ver_query(M, blk, sub, 1, &ptr, &len, &err) && len > 1;
    }
    CHECK_EQ(found, 10);
    /* the root still comes from the shim */
    uint32_t pp = gbuf(4), pl = gbuf(4);
    CHECK_EQ(CALL(w32_imp_version_VerQueryValueW, 16, blk, gwstr("\\"), pp, pl), 1);
    CHECK_EQ(sg_ld32(M, sg_ld32(M, pp) + 8), 0x00030001); /* 3.1.0.29: dwFileVersionMS */
    memset(M + SG_IMAGE_BASE, 0, sizeof hdr);
}

void test_version(void) {
    uint32_t b = gbuf(2048);
    uint32_t total = build_block(b);
    CHECK(total > 200);
    uint32_t ptr, len, err;
    CHECK_EQ(w32_ver_query(M, b, "\\", 1, &ptr, &len, &err), 1);
    CHECK_EQ(ptr, b + 40);
    CHECK_EQ(len, 0x34);
    CHECK_EQ(w32_ver_query(M, b, "\\StringFileInfo\\040904b0\\FileVersion", 1, &ptr, &len, &err), 1);
    CHECK_STR(hwstr(ptr), "1.2.3.4");
    CHECK_EQ(len, 8); /* characters, with the NUL, as stored */
    CHECK_EQ(w32_ver_query(M, b, "\\stringfileinfo\\040904B0\\companyname", 1, &ptr, &len, &err), 1);
    CHECK_STR(hwstr(ptr), "Test Co");
    CHECK_EQ(w32_ver_query(M, b, "StringFileInfo\\\\040904b0\\Comments", 1, &ptr, &len, &err), 1);
    CHECK_STR(hwstr(ptr), "café");
    /* A: 1252 copy after the block */
    CHECK_EQ(w32_ver_query(M, b, "\\StringFileInfo\\040904b0\\Comments", 0, &ptr, &len, &err), 1);
    CHECK_EQ(M[ptr + 3], 0xe9);
    CHECK_EQ(M[ptr + 4], 0);
    CHECK(ptr >= b + total);
    CHECK_EQ(w32_ver_query(M, b, "\\VarFileInfo\\Translation", 1, &ptr, &len, &err), 1);
    CHECK_EQ(len, 4);
    CHECK_EQ(sg_ld32(M, ptr), 0x04b00409);
    CHECK_EQ(w32_ver_query(M, b, "\\StringFileInfo", 1, &ptr, &len, &err), 1);
    CHECK_EQ(len, 0);
    CHECK_EQ(w32_ver_query(M, b, "\\StringFileInfo\\040904b0\\ProductName", 1, &ptr, &len, &err), 0);
    CHECK_EQ(err, 1813);
    CHECK_EQ(w32_ver_query(M, b, "\\StringFileInfo\\040704b0\\FileVersion", 1, &ptr, &len, &err), 0);
    real_exe();
}
