/* test_skipintro.c -- SPORE_SKIP_INTRO=1 (w32_skipintro.c): the intro-movie property in a package's
 * application properties reads as false through ReadFile, and nothing else changes. The packages here are
 * synthetic (built below); no game data. */
#include "harness.h"
#include "w32_int.h"

#include <stdlib.h>
#include <sys/stat.h>

#define GR 0x80000000u
#define INV 0xffffffffu

static uint32_t open_r(const char *p) { return CALL(w32_imp_kernel32_CreateFileW, 28, gwstr(p), GR, 1, 0, 3, 0x80, 0); }

/* The decompressed application properties: a count, an unrelated uint32 record, the intro record (id
 * 0xC14680EF, bool, flags 0, true), then a second bool record that is true as well. 24 + 12 + 12 bytes. */
static const uint8_t k_props[] = {
    0x00, 0x00, 0x00, 0x03,
    0x01, 0x02, 0x03, 0x04, 0x00, 0x0a, 0x00, 0x00, 0x00, 0x00, 0x00, 0x07,
    0xC1, 0x46, 0x80, 0xEF, 0x00, 0x01, 0x00, 0x00, 0x01,       /* value at 24 */
    0x05, 0x06, 0x07, 0x08, 0x00, 0x01, 0x00, 0x00, 0x01, 0x00, 0x00,       /* 9 + 2 bytes of padding */
    0x11, 0x22, 0x33, 0x44, 0x55, 0x66, 0x77, 0x88, 0x99, 0xaa, 0xbb, 0xcc,
};
#define VALUE_AT 24

/* RefPack: 0xE0+ literal runs (4..112 bytes, multiples of 4); `copy_value` appends a 3-byte back-reference
 * that copies the value byte (so the value is no longer a lone literal); 0xFC ends. Returns the length and
 * the stream offset of the value literal in *lit. */
static size_t refpack(uint8_t *out, int copy_value, size_t *lit, size_t *memsize) {
    size_t n = sizeof k_props, o = 0;
    size_t total = n + (copy_value ? 3 : 0);
    out[o++] = 0x10; out[o++] = 0xFB;
    out[o++] = (uint8_t)(total >> 16); out[o++] = (uint8_t)(total >> 8); out[o++] = (uint8_t)total;
    out[o++] = (uint8_t)(0xE0 + (n - 4) / 4); /* one literal run with every byte */
    *lit = o + VALUE_AT;
    memcpy(out + o, k_props, n); o += n;
    if (copy_value) { out[o++] = 0x00; out[o++] = (uint8_t)(n - VALUE_AT - 1); } /* cnt 3, offset n - VALUE_AT */
    out[o++] = 0xFC;
    *memsize = total;
    return o;
}

/* A DBPF 3.0 file: 96-byte header, a filler resource, the properties resource, the index (the unk word
 * constant; 7 words per entry). Returns the absolute file offset of the value byte. */
static long write_package(const char *rel, int compressed, int copy_value, uint32_t instance) {
    uint8_t res[256], file[1024];
    size_t lit = VALUE_AT, memsize = sizeof k_props, rn;
    if (compressed) rn = refpack(res, copy_value, &lit, &memsize);
    else { memcpy(res, k_props, sizeof k_props); rn = sizeof k_props; }
    memset(file, 0, sizeof file);
    memcpy(file, "DBPF", 4);
    file[4] = 3;
    size_t filler = 96, props = 96 + 16, idx = props + rn;
    memcpy(file + filler, "\x01\x00\x00\x00\x01\x00\x0a\x00\x00\x00\x00\x00\x00\x00\x00\x01", 16);
    memcpy(file + props, res, rn);
    uint32_t words[2 + 2 * 7] = {
        4, 0, /* flags: the unk word is constant (0); type and group per entry */
        0x00B1B104u, 0, 0x12345678u, (uint32_t)filler, 16u | 0x80000000u, 16, 0x00010000u,
        0x00B1B104u, 0, instance, (uint32_t)props, (uint32_t)rn | 0x80000000u, (uint32_t)memsize,
        compressed ? 0x0001FFFFu : 0x00010000u,
    };
    /* each entry: type, group, instance, offset, size (bit 31 set), memsize, compression word and flags */
    memcpy(file + idx, words, sizeof words);
    uint32_t count = 2, isize = sizeof words, ioff = (uint32_t)idx;
    memcpy(file + 0x24, &count, 4);
    memcpy(file + 0x2C, &isize, 4);
    memcpy(file + 0x40, &ioff, 4);
    char p[1200];
    snprintf(p, sizeof p, "%s/%s", t_root, rel);
    FILE *f = fopen(p, "wb");
    fwrite(file, 1, idx + sizeof words, f);
    fclose(f);
    return (long)(props + lit);
}

/* Reads the whole file through the guest in chunks of `step` bytes (0: one read, overlapped at offset 0). */
static size_t read_all(uint32_t h, uint32_t buf, uint32_t step) {
    uint32_t n = gbuf(4), done = 0;
    if (!step) {
        uint32_t ovl = gbuf(20);
        memset(M + ovl, 0, 20);
        CHECK_EQ(CALL(w32_imp_kernel32_ReadFile, 20, h, buf, 1024, n, ovl), 1);
        return sg_ld32(M, n);
    }
    for (;;) {
        CHECK_EQ(CALL(w32_imp_kernel32_ReadFile, 20, h, buf + done, step, n, 0), 1);
        if (!sg_ld32(M, n)) break;
        done += sg_ld32(M, n);
    }
    return done;
}

static void host_bytes(const char *rel, uint8_t *out, size_t *n) {
    char p[1200];
    snprintf(p, sizeof p, "%s/%s", t_root, rel);
    FILE *f = fopen(p, "rb");
    *n = fread(out, 1, 1024, f);
    fclose(f);
}

/* Opens `win`, reads it whole with each chunking and checks: the byte at `flip` reads 0 (or 1 when flip
 * is -1, i.e. nothing changes) and every other byte equals the host file. */
static void check_file(const char *win, const char *rel, long value_at, int expect_flip) {
    uint8_t host[1024];
    size_t hn;
    host_bytes(rel, host, &hn);
    static const uint32_t steps[] = { 0, 1, 7, 4096 };
    for (unsigned s = 0; s < sizeof steps / sizeof *steps; s++) {
        uint32_t h = open_r(win), buf = gbuf(1024);
        CHECK(h != INV);
        CHECK_EQ(read_all(h, buf, steps[s]), hn);
        int others_same = 1;
        for (size_t k = 0; k < hn; k++)
            if ((long)k != value_at && M[buf + k] != host[k]) others_same = 0;
        CHECK(others_same);
        if (value_at >= 0) CHECK_EQ(M[buf + value_at], expect_flip ? 0 : 1);
        CHECK_EQ(CALL(w32_imp_kernel32_CloseHandle, 4, h), 1);
    }
}

void test_skipintro(void) {
    char p[1200];
    snprintf(p, sizeof p, "%s/game/Data", t_root); mkdir(p, 0755);
    long v_comp = write_package("game/Data/Comp.package", 1, 0, 0x08EA246Bu);
    long v_plain = write_package("game/Data/Plain.package", 0, 0, 0x08EA246Bu);
    long v_copied = write_package("game/Data/Copied.package", 1, 1, 0x08EA246Bu);
    long v_other = write_package("game/Data/Other.package", 1, 0, 0x08EA246Cu); /* another instance */
    long v_txt = write_package("game/Data/Comp.bin", 1, 0, 0x08EA246Bu);         /* not a .package */

    /* off: unset, then "0" */
    unsetenv("SPORE_SKIP_INTRO");
    w32_skip_intro_reset();
    CHECK(!w32_skip_intro_enabled());
    check_file("..\\Data\\Comp.package", "game/Data/Comp.package", v_comp, 0);
    setenv("SPORE_SKIP_INTRO", "0", 1);
    CHECK(!w32_skip_intro_enabled());
    check_file("..\\Data\\Comp.package", "game/Data/Comp.package", v_comp, 0);

    /* on */
    setenv("SPORE_SKIP_INTRO", "1", 1);
    w32_skip_intro_reset();
    CHECK(w32_skip_intro_enabled());
    check_file("..\\Data\\Comp.package", "game/Data/Comp.package", v_comp, 1);
    check_file("..\\data\\COMP.PACKAGE", "game/Data/Comp.package", v_comp, 1); /* cached, any case */
    check_file("..\\Data\\Plain.package", "game/Data/Plain.package", v_plain, 1);
    /* the value literal is copied by a later back-reference: flipping it would change two bytes, so no */
    check_file("..\\Data\\Copied.package", "game/Data/Copied.package", v_copied, 0);
    /* another resource instance, and a file that is not a package: unchanged */
    check_file("..\\Data\\Other.package", "game/Data/Other.package", v_other, 0);
    check_file("..\\Data\\Comp.bin", "game/Data/Comp.bin", v_txt, 0);

    /* the decompressed resource as the game's decoder sees it: only the intro value differs */
    {
        uint32_t h = open_r("..\\Data\\Comp.package"), buf = gbuf(1024);
        read_all(h, buf, 0);
        /* the stream's single literal run starts after the 16-byte filler and the 6-byte RefPack header */
        const uint8_t *lit = M + buf + 96 + 16 + 6;
        int diff = 0;
        for (size_t k = 0; k < sizeof k_props; k++)
            if (lit[k] != k_props[k]) diff += k == VALUE_AT ? 0 : 1;
        CHECK_EQ(diff, 0);
        CHECK_EQ(lit[VALUE_AT], 0);
        CHECK_EQ(lit[VALUE_AT + 9], 1); /* the next bool record is untouched */
        CHECK_EQ(CALL(w32_imp_kernel32_CloseHandle, 4, h), 1);
    }

    /* a package opened for writing is never altered */
    {
        uint32_t h = CALL(w32_imp_kernel32_CreateFileW, 28, gwstr("..\\Data\\Comp.package"), GR | 0x40000000u, 1, 0, 3, 0x80, 0);
        uint32_t buf = gbuf(1024);
        CHECK(h != INV);
        read_all(h, buf, 0);
        CHECK_EQ(M[buf + v_comp], 1);
        CHECK_EQ(CALL(w32_imp_kernel32_CloseHandle, 4, h), 1);
    }
    unsetenv("SPORE_SKIP_INTRO");
    w32_skip_intro_reset();
}
