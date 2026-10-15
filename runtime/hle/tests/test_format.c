/* printf family (msvcr90 rules) and wsprintf. Expected strings follow the VS2008/VS2010
 * reference pages and the VS2015 change history's description of the old behaviour. */
#include <math.h>

#include "harness.h"

static uint32_t out;

/* sprintf(out, fmt, ...) with raw 32-bit slots; returns the result and checks the text. */
#define SPF(want, fmt, ...) do { \
        int32_t r_ = (int32_t)CALL(sg_imp_msvcr90_sprintf, 0, out, gstr(fmt), ##__VA_ARGS__); \
        CHECK_STR(hstr(out), want); CHECK_EQ(r_, (int32_t)strlen(want)); } while (0)

static uint32_t LO(double d) { uint32_t l, h; gdouble(d, &l, &h); return l; }
static uint32_t HI(double d) { uint32_t l, h; gdouble(d, &l, &h); return h; }
static double bits(uint64_t b) { double d; memcpy(&d, &b, 8); return d; }
#define D(x) LO(x), HI(x)

void test_format(void) {
    out = gbuf(1024);

    /* integers, flags, width, precision */
    SPF("42|   42|42   |00042|+42| 42", "%d|%5d|%-5d|%05d|%+d|% d", 42, 42, 42, 42, 42, 42);
    SPF("-7|-0007|  -007", "%i|%05d|%6.3d", (uint32_t)-7, (uint32_t)-7, (uint32_t)-7);
    SPF("ff FF 0xff 0XFF 0377 377", "%x %X %#x %#X %#o %o", 255, 255, 255, 255, 255, 255);
    SPF("0|0|", "%#x|%#o|%.0d", 0, 0, 0);
    SPF("4294967295 37777777777", "%u %o", 0xffffffffu, 0xffffffffu);
    SPF("9029 32768 -32768", "%hd %hu %hd", 0x12345, 0xffff8000u, 0x8000);
    SPF("-1 81985529216486895 123456789abcdef", "%I64d %I64u %I64x", 0xffffffffu, 0xffffffffu,
        0x89abcdefu, 0x01234567u, 0x89abcdefu, 0x01234567u);
    SPF("-1|ffffffffffffffff", "%lld|%llx", 0xffffffffu, 0xffffffffu, 0xffffffffu, 0xffffffffu);
    SPF("7 8", "%I32d %Id", 7, 8);
    SPF("0012FF7C", "%p", 0x0012ff7cu);
    SPF("7    |  3.142", "%*d|%*.*f", (uint32_t)-5, 7, 7, 3, D(3.14159265));

    /* characters and strings: c/s narrow, C/S wide in printf; h/l force */
    SPF("abcd", "%c%C%hc%lc", 'a', 'b', 'c', 'd');
    SPF("n|w|n|w|w", "%s|%S|%hs|%ls|%ws", gstr("n"), gwstr("w"), gstr("n"), gwstr("w"), gwstr("w"));
    SPF("(null)|(null)", "%s|%S", 0, 0);
    SPF("abc|ab    |  xyz", "%.3s|%-6.2s|%5s", gstr("abcdef"), gstr("abcdef"), gstr("xyz"));
    {
        /* %Z: ANSI_STRING {USHORT Length, MaximumLength; PCHAR Buffer} */
        uint32_t as = gbuf(8), txt = gstr("counted!");
        sg_st16(M, as, 7); sg_st16(M, as + 2, 9); sg_st32(M, as + 4, txt);
        SPF("counted", "%Z", as);
    }

    /* unknown type characters are copied; F/N are ignored size modifiers; no hh/j/z/t */
    SPF("%", "%%");
    SPF("$s", "%1$s", gstr("x"));      /* positional args only in _printf_p */
    SPF("zu", "%zu", 5);
    SPF("jd", "%jd", 5);
    SPF("511", "%hhd", 0x1ff);          /* hh == h: short */
    SPF("7", "%Fd", 7);
    SPF("q", "%q");

    /* floating point */
    SPF("1.000000e+000", "%e", D(1.0));
    SPF("1.234568E+004", "%E", D(12345.678));
    SPF("1.000000e+100|1.000000e-300", "%e|%e", D(1e100), D(1e-300));
    SPF("0.000000e+000", "%e", D(0.0));
    SPF("0.500000|1|3|0.3", "%f|%.0f|%.0f|%.1f", D(0.5), D(0.5), D(2.5), D(0.25)); /* half-up on digits */
    SPF("1208925819614629200000000", "%.0f", D(pow(2.0, 80)));  /* VS2015 change history example */
    SPF("0.10000000000000001000", "%.20f", D(0.1));               /* 17 digits, then zeros */
    SPF("0.0001|1e-005|1.23457e+008|100000|1e+006", "%g|%g|%g|%g|%g", D(0.0001), D(0.00001), D(123456789.0),
        D(100000.0), D(1000000.0));
    SPF("0|0.5|10", "%g|%g|%g", D(0.0), D(0.5), D(9.9999999));
    SPF("3.10000|3.", "%#g|%#.0f", D(3.1), D(3.0));
    SPF("     3.142|3.14e+000 |", "%10.3f|%-10.2e|", D(3.14159), D(3.14159));
    SPF(" -0.0|+1.5|-1.500000", "%5.1f|%+.1f|%f", D(-0.0), D(1.5), D(-1.5));
    SPF("1.#INF00|-1.#INF00|1.#J|1.$|1", "%f|%f|%.2f|%.1f|%.0f", D(INFINITY), D(-INFINITY), D(INFINITY),
        D(INFINITY), D(INFINITY));
    SPF("1.#INF00e+000|1.#INF", "%e|%g", D(INFINITY), D(INFINITY));
    SPF("-1.#IND00|1.#QNAN0|1.#SNAN0", "%f|%f|%f", D(bits(0xfff8000000000000ull)), D(bits(0x7ff8000000000000ull)),
        D(bits(0x7ff0000000000001ull)));
    SPF("-1.#IND", "%g", D(bits(0xfff8000000000000ull)));
    SPF("0x1.000000p+0|0X1.800000P+1|0x1.0p-1", "%a|%A|%.1a", D(1.0), D(3.0), D(0.5));
    SPF("1.000000|1.000000", "%lf|%Lf", D(1.0), D(1.0));

    /* %n is rejected (invalid parameter; continue policy returns -1) */
    CHECK_EQ((int32_t)CALL(sg_imp_msvcr90_sprintf, 0, out, gstr("ab%n"), gbuf(4)), -1);

    /* vsprintf: a va_list is a guest pointer to the argument slots */
    {
        uint32_t ap = gbuf(16);
        sg_st32(M, ap, 5);
        sg_st32(M, ap + 4, gstr("five"));
        CHECK_EQ(CALL(sg_imp_msvcr90_vsprintf, 0, out, gstr("%d=%s"), ap), 6);
        CHECK_STR(hstr(out), "5=five");
    }

    /* _snprintf / _vsnprintf truncation rules (VS2008 reference) */
    {
        uint32_t b = gbuf(16), ap = gbuf(8);
        memset(M + b, 'X', 16);
        CHECK_EQ((int32_t)CALL(sg_imp_msvcr90__snprintf, 0, b, 5, gstr("hi")), 2);
        CHECK(!memcmp(M + b, "hi\0", 3));
        memset(M + b, 'X', 16);
        CHECK_EQ((int32_t)CALL(sg_imp_msvcr90__snprintf, 0, b, 5, gstr("hello")), 5);
        CHECK(!memcmp(M + b, "helloX", 6)); /* len == count: no terminator */
        memset(M + b, 'X', 16);
        CHECK_EQ((int32_t)CALL(sg_imp_msvcr90__snprintf, 0, b, 5, gstr("%s!"), gstr("hello")), -1);
        CHECK(!memcmp(M + b, "helloX", 6)); /* len > count: count chars, negative result */
        sg_st32(M, ap, 123);
        CHECK_EQ((int32_t)CALL(sg_imp_msvcr90__vsnprintf, 0, b, 16, gstr("<%d>"), ap), 5);
        CHECK_STR(hstr(b), "<123>");
    }

    /* printf goes to the HLE log and returns the count */
    CHECK_EQ(CALL(sg_imp_msvcr90_printf, 0, gstr("x=%d\n"), 3), 4);

    /* wsprintf: no floats, no '+'/' ' flags, '#' gives 0x */
    CHECK_EQ(CALL(sg_imp_user32_wsprintfA, 0, out, gstr("%d|%#x|%+d|%05u|%-3c|%ls|%f"), 5, 255, 9, (uint32_t)'q',
                  gwstr("W"), 0), 23);
    CHECK_STR(hstr(out), "5|0xff|+d|00009|q  |W|f");
    CHECK_EQ(CALL(sg_imp_user32_wsprintfW, 0, out, gwstr("%s-%hs-%S-%d"), gwstr("wide"), gstr("nar"), gstr("S"), 4), 12);
    CHECK(wstreq(out, "wide-nar-S-4"));
}
