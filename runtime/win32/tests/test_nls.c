/* test_nls.c -- GetSystemDefaultLCID, GetLocaleInfo, CompareString, LCMapString. */
#include "harness.h"
#include "w32_dirwatch.h"
#include "w32_int.h"

#include <stdlib.h>

static uint32_t cmp(uint32_t lcid, uint32_t flags, const char *a, int32_t na, const char *b, int32_t nb) {
    return CALL(w32_imp_kernel32_CompareStringW, 24, lcid, flags, gwstr(a), (uint32_t)na, gwstr(b), (uint32_t)nb);
}

/* sort key of s (host copy) */
static uint8_t *key(uint32_t flags, const char *s, uint32_t *len) {
    uint32_t src = gwstr(s);
    uint32_t n = CALL(w32_imp_kernel32_LCMapStringW, 24, 0x409, 0x400 | flags, src, 0xffffffffu, 0, 0);
    uint32_t dst = gbuf(n + 4);
    uint32_t m = CALL(w32_imp_kernel32_LCMapStringW, 24, 0x409, 0x400 | flags, src, 0xffffffffu, dst, n);
    if (m != n) { fprintf(stderr, "FAIL sort key size %u vs %u\n", m, n); t_fail++; }
    uint8_t *k = (uint8_t *)malloc(n);
    memcpy(k, M + dst, n);
    *len = n;
    return k;
}

void test_nls(void) {
    /* LCIDs */
    CHECK_EQ(CALL0(w32_imp_kernel32_GetSystemDefaultLCID, 0), 0x409);
    CHECK_EQ(CALL0(w32_imp_kernel32_GetUserDefaultLCID, 0), 0x409);

    /* GetLocaleInfoW: the game's query (0x847f11): LOCALE_IDEFAULTANSICODEPAGE into 0x80 units */
    uint32_t buf = gbuf(256);
    CHECK_EQ(CALL(w32_imp_kernel32_GetLocaleInfoW, 16, 0x409, 0x1004, buf, 0x80), 5);
    CHECK_STR(hwstr(buf), "1252");
    CHECK_EQ(CALL(w32_imp_kernel32_GetLocaleInfoW, 16, 0x409, 0x1004, 0, 0), 5); /* size query */
    CHECK_EQ(CALL(w32_imp_kernel32_GetLocaleInfoW, 16, 0x409, 0x1004, buf, 3), 0);
    CHECK_EQ(last_error(), 122);
    const struct { uint32_t lcid; const char *cp; } cps[] = {
        {0x419, "1251"}, {0x411, "932"}, {0x412, "949"}, {0x404, "950"}, {0x804, "936"}, {0x415, "1250"},
        {0x408, "1253"}, {0x40c, "1252"}, {0x407, "1252"}, {0x41e, "874"}, {0x400, "1252"}, {0, "1252"}};
    for (unsigned i = 0; i < sizeof cps / sizeof cps[0]; i++) {
        CALL(w32_imp_kernel32_GetLocaleInfoW, 16, cps[i].lcid, 0x1004, buf, 0x80);
        CHECK_STR(hwstr(buf), cps[i].cp);
    }
    CHECK_EQ(CALL(w32_imp_kernel32_GetLocaleInfoW, 16, 0x409, 0x000b, buf, 0x80), 4);
    CHECK_STR(hwstr(buf), "437");
    CHECK_EQ(CALL(w32_imp_kernel32_GetLocaleInfoW, 16, 0x409, 0x20001004u, buf, 2), 2); /* LOCALE_RETURN_NUMBER */
    CHECK_EQ(sg_ld32(M, buf), 1252);
    CHECK_EQ(CALL(w32_imp_kernel32_GetLocaleInfoW, 16, 0x409, 0x20000001u, buf, 2), 2);
    CHECK_EQ(sg_ld32(M, buf), 0x409);
    const struct { uint32_t type; const char *v; } vals[] = {
        {0x5c, "en-US"}, {0x59, "en"}, {0x5a, "US"}, {0x0e, "."}, {0x0f, ","}, {0x1f, "M/d/yyyy"}, {0x2a, "Monday"},
        {0x30, "Sunday"}, {0x44, "Jan"}, {0x43, "December"}, {0x14, "$"}, {0x01, "0409"}, {0x1001, "English"},
        {0x02, "English (United States)"}, {0x80000000u | 0x1d, "/"}};
    for (unsigned i = 0; i < sizeof vals / sizeof vals[0]; i++) {
        CALL(w32_imp_kernel32_GetLocaleInfoW, 16, 0x400, vals[i].type, buf, 0x80);
        CHECK_STR(hwstr(buf), vals[i].v);
    }
    CHECK_EQ(CALL(w32_imp_kernel32_GetLocaleInfoW, 16, 0x409, 0x7777, buf, 0x80), 0);
    CHECK_EQ(last_error(), 1004);
    CHECK_EQ(CALL(w32_imp_kernel32_GetLocaleInfoA, 16, 0x409, 0x1004, buf, 0x80), 5);
    CHECK_STR((const char *)M + buf, "1252");

    /* CompareStringW: CSTR_LESS_THAN 1, EQUAL 2, GREATER 3 */
    CHECK_EQ(cmp(0x409, 0, "abc", -1, "abd", -1), 1);
    CHECK_EQ(cmp(0x409, 0, "abd", -1, "abc", -1), 3);
    CHECK_EQ(cmp(0x409, 0, "abc", -1, "abc", -1), 2);
    CHECK_EQ(cmp(0x409, 0, "a", -1, "A", -1), 1);            /* lower case first */
    CHECK_EQ(cmp(0x409, 1, "a", -1, "A", -1), 2);            /* NORM_IGNORECASE */
    CHECK_EQ(cmp(0x409, 1, "Spore", -1, "SPORE", -1), 2);
    CHECK_EQ(cmp(0x409, 0, "apple", -1, "Banana", -1), 1);   /* linguistic, not ordinal ('B' < 'a') */
    CHECK_EQ(cmp(0x409, 0, "résumé", -1, "resume", -1), 3);
    CHECK_EQ(cmp(0x409, 2, "résumé", -1, "resume", -1), 2);  /* NORM_IGNORENONSPACE */
    CHECK_EQ(cmp(0x409, 2, "Resume", -1, "resume", -1), 3);  /* ... case still counts */
    CHECK_EQ(cmp(0x409, 3, "Résumé", -1, "resume", -1), 2);
    CHECK_EQ(cmp(0x409, 4, "a b!", -1, "ab", -1), 2);        /* NORM_IGNORESYMBOLS */
    CHECK_EQ(cmp(0x409, 0, "coop", -1, "co-op", -1), 1);     /* word sort: hyphen breaks ties only */
    CHECK_EQ(cmp(0x409, 0, "co-op", -1, "coop", -1), 3);
    CHECK_EQ(cmp(0x409, 0, "co-op", -1, "cop", -1), 1);
    CHECK_EQ(cmp(0x409, 0x1000, "co-op", -1, "coop", -1), 1); /* SORT_STRINGSORT: hyphen is a symbol */
    CHECK_EQ(cmp(0x409, 0, "abcdef", 3, "abc", -1), 2);      /* counted length */
    CHECK_EQ(cmp(0x409, 0, "", -1, "a", -1), 1);
    CHECK_EQ(cmp(0x7f, 1, "Data", -1, "DATA", -1), 2);       /* LOCALE_INVARIANT */
    CHECK_EQ(cmp(0x409, 0, "file10", -1, "file9", -1), 1);
    CHECK_EQ(CALL(w32_imp_kernel32_CompareStringW, 24, 0x409, 0, 0, 0xffffffffu, gwstr("a"), 0xffffffffu), 0);
    CHECK_EQ(last_error(), 87);
    CHECK_EQ(cmp(0x409, 0x40000000u, "a", -1, "a", -1), 0);
    CHECK_EQ(last_error(), 1004);
    /* CompareStringOrdinal (not imported): code units */
    CHECK_EQ(CALL(w32_imp_kernel32_CompareStringOrdinal, 20, gwstr("apple"), 0xffffffffu, gwstr("Banana"), 0xffffffffu, 0), 3);
    CHECK_EQ(CALL(w32_imp_kernel32_CompareStringOrdinal, 20, gwstr("abc"), 0xffffffffu, gwstr("ABC"), 0xffffffffu, 1), 2);

    /* LCMapStringW: the game's pattern (0x6b8f55): size query, then the mapping */
    uint32_t src = gwstr("HeLLo ÄÖ Ω");
    CHECK_EQ(CALL(w32_imp_kernel32_LCMapStringW, 24, 0x400, 0x100, src, 10, 0, 0), 10);
    uint32_t dst = gbuf(64);
    CHECK_EQ(CALL(w32_imp_kernel32_LCMapStringW, 24, 0x400, 0x100, src, 10, dst, 32), 10);
    sg_st16(M, dst + 20, 0);
    CHECK_STR(hwstr(dst), "hello äö ω");
    CHECK_EQ(CALL(w32_imp_kernel32_LCMapStringW, 24, 0x409, 0x200, gwstr("straße"), 0xffffffffu, dst, 32), 7); /* NUL counted */
    CHECK_STR(hwstr(dst), "STRAßE"); /* one-to-one mapping: no SS expansion */
    CHECK_EQ(CALL(w32_imp_kernel32_LCMapStringW, 24, 0x409, 0x200, gwstr("abc"), 0xffffffffu, dst, 3), 0);
    CHECK_EQ(last_error(), 122);
    CHECK_EQ(CALL(w32_imp_kernel32_LCMapStringW, 24, 0x409, 0x300, gwstr("abc"), 0xffffffffu, dst, 32), 0);
    CHECK_EQ(last_error(), 1004);
    CHECK_EQ(CALL(w32_imp_kernel32_LCMapStringW, 24, 0x409, 0x800, gwstr("A"), 1, dst, 32), 1); /* LCMAP_BYTEREV */
    CHECK_EQ(sg_ld16(M, dst), 0x4100);
    CHECK_EQ(CALL(w32_imp_kernel32_LCMapStringA, 24, 0x409, 0x100, gstr("ABC"), 0xffffffffu, dst, 32), 4);
    CHECK_STR((const char *)M + dst, "abc");

    /* sort keys order like CompareStringW */
    static const char *const words[] = {"a", "A", "b", "B", "ab", "co-op", "coop", "cop", "résumé", "resume",
                                        "Resume", "file10", "file9", "", "zebra", "Ångström"};
    const uint32_t flagsets[] = {0, 1, 2, 3, 0x1000};
    int agree = 1;
    for (unsigned f = 0; f < sizeof flagsets / sizeof flagsets[0]; f++)
        for (unsigned i = 0; i < sizeof words / sizeof words[0]; i++)
            for (unsigned j = 0; j < sizeof words / sizeof words[0]; j++) {
                uint32_t la, lb;
                uint8_t *ka = key(flagsets[f], words[i], &la), *kb = key(flagsets[f], words[j], &lb);
                int m = memcmp(ka, kb, la < lb ? la : lb);
                if (!m) m = (int)la - (int)lb;
                m = m < 0 ? 1 : m > 0 ? 3 : 2;
                uint32_t c = cmp(0x409, flagsets[f], words[i], -1, words[j], -1);
                if ((uint32_t)m != c) {
                    agree = 0;
                    fprintf(stderr, "sort key/compare disagree: flags 0x%x \"%s\" vs \"%s\": key %d compare %u\n",
                            flagsets[f], words[i], words[j], m, c);
                }
                free(ka);
                free(kb);
            }
    CHECK(agree);
}
