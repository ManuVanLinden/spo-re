/* sscanf / swscanf (msvcr90 rules). */
#include "harness.h"

void test_scan(void) {
    uint32_t a = gbuf(4), b = gbuf(4), c = gbuf(8), s1 = gbuf(64), s2 = gbuf(64);
    float f;
    double d;

    CHECK_EQ(CALL(sg_imp_msvcr90_sscanf, 0, gstr("42 0x1f -010"), gstr("%d %x %i"), a, b, c), 3);
    CHECK_EQ(sg_ld32(M, a), 42);
    CHECK_EQ(sg_ld32(M, b), 31);
    CHECK_EQ((int32_t)sg_ld32(M, c), -8); /* %i: leading 0 = octal */
    CHECK_EQ(CALL(sg_imp_msvcr90_sscanf, 0, gstr("-9223372036854775807"), gstr("%I64d"), c), 1);
    CHECK(sg_ld64(M, c) == (uint64_t)-9223372036854775807LL);
    CHECK_EQ(CALL(sg_imp_msvcr90_sscanf, 0, gstr("4294967297"), gstr("%d"), a), 1);
    CHECK_EQ(sg_ld32(M, a), 1); /* wraps, no saturation */
    sg_st32(M, a, 0xaaaaaaaau);
    CHECK_EQ(CALL(sg_imp_msvcr90_sscanf, 0, gstr("-2"), gstr("%hd"), a), 1);
    CHECK_EQ(sg_ld32(M, a), 0xaaaafffeu); /* only 16 bits stored */

    /* strings, sets, counted chars */
    CHECK_EQ(CALL(sg_imp_msvcr90_sscanf, 0, gstr("  hello world"), gstr("%s %s"), s1, s2), 2);
    CHECK_STR(hstr(s1), "hello");
    CHECK_STR(hstr(s2), "world");
    memset(M + s1, 'X', 8);
    CHECK_EQ(CALL(sg_imp_msvcr90_sscanf, 0, gstr("abcdef"), gstr("%3c"), s1), 1);
    CHECK(!memcmp(M + s1, "abcX", 4)); /* no terminator */
    CHECK_EQ(CALL(sg_imp_msvcr90_sscanf, 0, gstr("key=val,rest"), gstr("%[a-z]=%[^,]"), s1, s2), 2);
    CHECK_STR(hstr(s1), "key");
    CHECK_STR(hstr(s2), "val");
    CHECK_EQ(CALL(sg_imp_msvcr90_sscanf, 0, gstr("zyx1"), gstr("%[z-a]"), s1), 1); /* reversed range */
    CHECK_STR(hstr(s1), "zyx");
    CHECK_EQ(CALL(sg_imp_msvcr90_sscanf, 0, gstr("]]a"), gstr("%[]]"), s1), 1);
    CHECK_STR(hstr(s1), "]]");
    CHECK_EQ(CALL(sg_imp_msvcr90_sscanf, 0, gstr("wide"), gstr("%S"), s1), 1); /* %S: wide store in sscanf */
    CHECK(wstreq(s1, "wide"));
    CHECK_EQ(CALL(sg_imp_msvcr90_sscanf, 0, gstr("wide"), gstr("%ls"), s1), 1);
    CHECK(wstreq(s1, "wide"));
    CHECK_EQ(CALL(sg_imp_msvcr90_sscanf, 0, gstr("abcdef"), gstr("%4s"), s1), 1);
    CHECK_STR(hstr(s1), "abcd");

    /* floating point: decimal only */
    CHECK_EQ(CALL(sg_imp_msvcr90_sscanf, 0, gstr("1.5 -2.25e1"), gstr("%f %lf"), a, c), 2);
    f = sg_ldf32(M, a);
    d = sg_ldf64(M, c);
    CHECK(f == 1.5f && d == -22.5);
    CHECK_EQ(CALL(sg_imp_msvcr90_sscanf, 0, gstr("inf"), gstr("%f"), a), 0);
    CHECK_EQ(CALL(sg_imp_msvcr90_sscanf, 0, gstr("nan"), gstr("%lf"), c), 0);
    CHECK_EQ(CALL(sg_imp_msvcr90_sscanf, 0, gstr("3e"), gstr("%lf%s"), c, s1), 2);
    CHECK(sg_ldf64(M, c) == 3.0);
    CHECK_STR(hstr(s1), "e");

    /* suppression, %n, literals, EOF */
    CHECK_EQ(CALL(sg_imp_msvcr90_sscanf, 0, gstr("10 20"), gstr("%*d %d%n"), a, b), 1);
    CHECK_EQ(sg_ld32(M, a), 20);
    CHECK_EQ(sg_ld32(M, b), 5);
    CHECK_EQ(CALL(sg_imp_msvcr90_sscanf, 0, gstr("v1.2"), gstr("v%d.%d"), a, b), 2);
    CHECK_EQ(CALL(sg_imp_msvcr90_sscanf, 0, gstr("x1"), gstr("v%d"), a), 0);
    CHECK_EQ((int32_t)CALL(sg_imp_msvcr90_sscanf, 0, gstr(""), gstr("%d"), a), -1);
    CHECK_EQ((int32_t)CALL(sg_imp_msvcr90_sscanf, 0, gstr("   "), gstr("%d"), a), -1);
    CHECK_EQ((int32_t)CALL(sg_imp_msvcr90_sscanf, 0, gstr("5"), gstr("%d %d"), a, b), 1);
    CHECK_EQ(CALL(sg_imp_msvcr90_sscanf, 0, gstr("100%"), gstr("%d%%"), a), 1);

    /* swscanf: s is wide, hs/S narrow */
    CHECK_EQ(CALL(sg_imp_msvcr90_swscanf, 0, gwstr("name 7 nar"), gwstr("%s %d %hs"), s1, a, s2), 3);
    CHECK(wstreq(s1, "name"));
    CHECK_EQ(sg_ld32(M, a), 7);
    CHECK_STR(hstr(s2), "nar");
    CHECK_EQ(CALL(sg_imp_msvcr90_swscanf, 0, gwstr("0.25"), gwstr("%f"), a), 1);
    CHECK(sg_ldf32(M, a) == 0.25f);
}
