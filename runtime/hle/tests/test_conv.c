/* Number parsing and integer/float -> text conversions. */
#include <math.h>
#include <stdlib.h>

#include "harness.h"

static double pop_st0(void) { return sg_fpu_pop(&T); }

void test_conv(void) {
    uint32_t s, endp = gbuf(4), lo, hi, dec = gbuf(4), sign = gbuf(4), r;
    double d;

    /* strtol family */
    s = gstr("  -0x1Az");
    CHECK_EQ((int32_t)CALL(sg_imp_msvcr90_strtol, 0, s, endp, 0), -26);
    CHECK_EQ(sg_ld32(M, endp), s + 7);
    s = gstr("0777");
    CHECK_EQ(CALL(sg_imp_msvcr90_strtol, 0, s, endp, 0), 0777);
    hle_set_errno(M, 0);
    s = gstr("99999999999");
    CHECK_EQ(CALL(sg_imp_msvcr90_strtol, 0, s, endp, 10), 0x7fffffff);
    CHECK_EQ(hle_get_errno(M), MSVC_ERANGE);
    CHECK_EQ(sg_ld32(M, endp), s + 11);
    CHECK_EQ(CALL(sg_imp_msvcr90_strtol, 0, gstr("-99999999999"), 0, 10), 0x80000000u);
    CHECK_EQ(CALL(sg_imp_msvcr90_strtoul, 0, gstr("-1"), 0, 10), 0xffffffffu);
    CHECK_EQ(CALL(sg_imp_msvcr90_strtoul, 0, gstr("ffffffff"), 0, 16), 0xffffffffu);
    s = gstr("0x");
    CHECK_EQ(CALL(sg_imp_msvcr90_strtol, 0, s, endp, 16), 0);
    CHECK_EQ(sg_ld32(M, endp), s); /* no digits: endptr = nptr */
    s = gstr("xyz");
    CHECK_EQ(CALL(sg_imp_msvcr90_strtol, 0, s, endp, 10), 0);
    CHECK_EQ(sg_ld32(M, endp), s);
    CALL(sg_imp_msvcr90__strtoui64, 0, gstr("18446744073709551615"), 0, 10);
    CHECK(T.eax == 0xffffffffu && T.edx == 0xffffffffu);
    CALL(sg_imp_msvcr90__strtoui64, 0, gstr("0x123456789"), 0, 0);
    CHECK(T.eax == 0x23456789u && T.edx == 1);
    CHECK_EQ((int32_t)CALL(sg_imp_msvcr90_wcstol, 0, gwstr(" -42"), 0, 10), -42);
    s = gwstr("12ab");
    CHECK_EQ(CALL(sg_imp_msvcr90_wcstoul, 0, s, endp, 10), 12);
    CHECK_EQ(sg_ld32(M, endp), s + 4);

    /* atoi/atol: wrap-around accumulation */
    CHECK_EQ((int32_t)CALL(sg_imp_msvcr90_atoi, 0, gstr("  +123abc")), 123);
    CHECK_EQ((int32_t)CALL(sg_imp_msvcr90_atol, 0, gstr("-2147483648")), (int32_t)0x80000000u);
    CHECK_EQ(CALL(sg_imp_msvcr90_atoi, 0, gstr("4294967297")), 1);
    CHECK_EQ((int32_t)CALL(sg_imp_msvcr90__wtol, 0, gwstr("-77")), -77);

    /* strtod rules: d/D exponents, no inf/nan/hex, 17 significant digits */
    CALL(sg_imp_msvcr90_atof, 0, gstr("1.5e3"));
    CHECK(pop_st0() == 1500.0);
    s = gstr("1.5d2xyz");
    CALL(sg_imp_msvcr90_strtod, 0, s, endp);
    CHECK(pop_st0() == 150.0);
    CHECK_EQ(sg_ld32(M, endp), s + 5);
    s = gstr("inf");
    CALL(sg_imp_msvcr90_strtod, 0, s, endp);
    CHECK(pop_st0() == 0.0);
    CHECK_EQ(sg_ld32(M, endp), s);
    s = gstr("0x10");
    CALL(sg_imp_msvcr90_strtod, 0, s, endp);
    CHECK(pop_st0() == 0.0);
    CHECK_EQ(sg_ld32(M, endp), s + 1); /* "0" parsed, stops at 'x' */
    s = gstr("1e+");
    CALL(sg_imp_msvcr90_strtod, 0, s, endp);
    CHECK(pop_st0() == 1.0);
    CHECK_EQ(sg_ld32(M, endp), s + 1);
    s = gstr(".5");
    CALL(sg_imp_msvcr90_strtod, 0, s, endp);
    CHECK(pop_st0() == 0.5);
    s = gstr(".");
    CALL(sg_imp_msvcr90_strtod, 0, s, endp);
    CHECK(pop_st0() == 0.0);
    CHECK_EQ(sg_ld32(M, endp), s);
    CALL(sg_imp_msvcr90_atof, 0, gstr("0.123456789012345678"));
    d = pop_st0();
    CHECK(d == strtod("0.12345678901234567", NULL)); /* the 18th digit is ignored */
    hle_set_errno(M, 0);
    CALL(sg_imp_msvcr90_strtod, 0, gstr("1e999"), 0);
    CHECK(isinf(pop_st0()) && hle_get_errno(M) == MSVC_ERANGE);
    CALL(sg_imp_msvcr90_strtod, 0, gstr("-1e-999"), 0);
    d = pop_st0();
    CHECK(d == 0.0);
    CALL(sg_imp_msvcr90_wcstod, 0, gwstr(" -2.25"), 0);
    CHECK(pop_st0() == -2.25);
    CHECK_EQ(T.fpu.top, 0); /* every push was popped */

    /* integer -> text */
    s = gbuf(40);
    CHECK_EQ(CALL(sg_imp_msvcr90__ltoa, 0, (uint32_t)-12, s, 10), s);
    CHECK_STR(hstr(s), "-12");
    CALL(sg_imp_msvcr90__ltoa, 0, (uint32_t)-255, s, 16);
    CHECK_STR(hstr(s), "ffffff01"); /* non-decimal radix: unsigned */
    CALL(sg_imp_msvcr90__ultoa, 0, 0xffffffffu, s, 2);
    CHECK_EQ(strlen(hstr(s)), 32);
    CALL(sg_imp_msvcr90__ltow, 0, 1234, s, 10);
    CHECK(wstreq(s, "1234"));
    CALL(sg_imp_msvcr90__ultow, 0, 255, s, 16);
    CHECK(wstreq(s, "ff"));

    /* _fcvt / _ecvt: the examples in the VS2008 reference pages */
    gdouble(3.1415926535, &lo, &hi);
    r = CALL(sg_imp_msvcr90__fcvt, 0, lo, hi, 7, dec, sign);
    CHECK_STR(hstr(r), "31415927");
    CHECK_EQ(sg_ld32(M, dec), 1);
    CHECK_EQ(sg_ld32(M, sign), 0);
    r = CALL(sg_imp_msvcr90__ecvt, 0, lo, hi, 10, dec, sign);
    CHECK_STR(hstr(r), "3141592654");
    CHECK_EQ(sg_ld32(M, dec), 1);
    gdouble(-0.00125, &lo, &hi);
    r = CALL(sg_imp_msvcr90__fcvt, 0, lo, hi, 4, dec, sign);
    CHECK_STR(hstr(r), "13"); /* 0.0013 -> digits "13", decpt -2 */
    CHECK_EQ((int32_t)sg_ld32(M, dec), -2);
    CHECK_EQ(sg_ld32(M, sign), 1);
    gdouble(99.96, &lo, &hi);
    r = CALL(sg_imp_msvcr90__ecvt, 0, lo, hi, 3, dec, sign);
    CHECK_STR(hstr(r), "100"); /* carry out of the first digit */
    CHECK_EQ(sg_ld32(M, dec), 3);

    /* the digit generator directly */
    {
        char buf[64];
        int neg, dp;
        dp = hle_fltout(1.0 / 0.0, 2, 1, buf, sizeof buf, &neg);
        CHECK_STR(buf, "1#J"); /* "1#INF" rounded to 2 places after the point */
        CHECK_EQ(dp, 1);
        dp = hle_fltout(0.5, 0, 1, buf, sizeof buf, &neg);
        CHECK_STR(buf, "1"); /* half-up on the digit string */
        CHECK_EQ(dp, 1);
        dp = hle_fltout(pow(2.0, 80), 0, 1, buf, sizeof buf, &neg);
        CHECK_STR(buf, "1208925819614629200000000");
    }
}
