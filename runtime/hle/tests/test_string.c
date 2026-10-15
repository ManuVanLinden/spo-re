/* Memory, string, ctype and wcs* shims. */
#include "harness.h"

void test_string(void) {
    uint32_t a = gstr("hello world"), b = gbuf(64), r;
    int32_t s;

    /* memory */
    r = CALL(sg_imp_msvcr90_memcpy, 0, b, a, 12);
    CHECK_EQ(r, b);
    CHECK_STR(hstr(b), "hello world");
    CALL(sg_imp_msvcr90_memmove, 0, b + 2, b, 5); /* overlapping */
    CHECK_STR(hstr(b), "hehelloorld");
    CALL(sg_imp_msvcr90_memset, 0, b, 0x141, 3); /* value truncated to a byte */
    CHECK(!memcmp(M + b, "AAAe", 4));
    CHECK_EQ(CALL(sg_imp_msvcr90_memchr, 0, a, 'w', 11), a + 6);
    CHECK_EQ(CALL(sg_imp_msvcr90_memchr, 0, a, 'w', 6), 0);

    /* comparisons */
    CHECK_EQ((int32_t)CALL(sg_imp_msvcr90_strcmp, 0, gstr("abc"), gstr("abd")), -1);
    CHECK_EQ((int32_t)CALL(sg_imp_msvcr90_strcmp, 0, gstr("b"), gstr("abc")), 1);
    CHECK_EQ((int32_t)CALL(sg_imp_msvcr90_strcmp, 0, gstr("same"), gstr("same")), 0);
    CHECK_EQ((int32_t)CALL(sg_imp_msvcr90_strcmp, 0, gstr("\xe9"), gstr("e")), 1); /* unsigned bytes */
    CHECK_EQ((int32_t)CALL(sg_imp_msvcr90_strncmp, 0, gstr("abcX"), gstr("abcY"), 3), 0);
    CHECK((int32_t)CALL(sg_imp_msvcr90_strncmp, 0, gstr("abcX"), gstr("abcY"), 4) < 0);
    CHECK_EQ((int32_t)CALL(sg_imp_msvcr90__stricmp, 0, gstr("HeLLo"), gstr("hello")), 0);
    /* folding is to lower case: '_' (0x5f) sorts after '[' (0x5b) but before 'a' */
    CHECK((int32_t)CALL(sg_imp_msvcr90__stricmp, 0, gstr("A_"), gstr("a[")) > 0);
    CHECK((int32_t)CALL(sg_imp_msvcr90__stricmp, 0, gstr("_"), gstr("A")) < 0);
    CHECK((int32_t)CALL(sg_imp_msvcr90__stricmp, 0, gstr("\xc9"), gstr("\xe9")) != 0); /* "C" locale: ASCII only */
    CHECK_EQ((int32_t)CALL(sg_imp_msvcr90__strnicmp, 0, gstr("ABCdef"), gstr("abcXYZ"), 3), 0);

    /* copies */
    b = gbuf(16);
    memset(M + b, 'Z', 16);
    CALL(sg_imp_msvcr90_strncpy, 0, b, gstr("ab"), 5);
    CHECK(!memcmp(M + b, "ab\0\0\0Z", 6)); /* pads with NULs to n, no more */
    CALL(sg_imp_msvcr90_strncpy, 0, b, gstr("abcdefgh"), 3);
    CHECK(!memcmp(M + b, "abc\0", 4) && M[b + 3] == 0);
    b = gbuf(16);
    memcpy(M + b, "abc", 4);
    CALL(sg_imp_msvcr90_strncat, 0, b, gstr("defgh"), 2);
    CHECK_STR(hstr(b), "abcde");

    /* searching */
    a = gstr("a/b/c");
    CHECK_EQ(CALL(sg_imp_msvcr90_strchr, 0, a, '/'), a + 1);
    CHECK_EQ(CALL(sg_imp_msvcr90_strrchr, 0, a, '/'), a + 3);
    CHECK_EQ(CALL(sg_imp_msvcr90_strchr, 0, a, 0), a + 5);
    CHECK_EQ(CALL(sg_imp_msvcr90_strchr, 0, a, 'z'), 0);
    CHECK_EQ(CALL(sg_imp_msvcr90_strstr, 0, a, gstr("b/c")), a + 2);
    CHECK_EQ(CALL(sg_imp_msvcr90_strstr, 0, a, gstr("")), a);
    CHECK_EQ(CALL(sg_imp_msvcr90_strpbrk, 0, a, gstr("cb")), a + 2);
    CHECK_EQ(CALL(sg_imp_msvcr90_strspn, 0, gstr("aaab"), gstr("a")), 3);
    CHECK_EQ(CALL(sg_imp_msvcr90_strcspn, 0, gstr("abc,d"), gstr(",;")), 3);
    a = gstr("MiXeD");
    CALL(sg_imp_msvcr90__strlwr, 0, a);
    CHECK_STR(hstr(a), "mixed");

    /* ctype, "C" locale */
    CHECK(CALL(sg_imp_msvcr90_isalpha, 0, 'q'));
    CHECK(!CALL(sg_imp_msvcr90_isalpha, 0, 0xe9)); /* upper half is unclassified in "C" */
    CHECK(!CALL(sg_imp_msvcr90_isalpha, 0, (uint32_t)-23)); /* sign-extended char */
    CHECK(CALL(sg_imp_msvcr90_isspace, 0, '\t') && CALL(sg_imp_msvcr90_isspace, 0, '\v'));
    CHECK(!CALL(sg_imp_msvcr90_isprint, 0, '\t'));
    CHECK(CALL(sg_imp_msvcr90_isprint, 0, ' '));
    CHECK(CALL(sg_imp_msvcr90_isxdigit, 0, 'F') && !CALL(sg_imp_msvcr90_isxdigit, 0, 'g'));
    CHECK(CALL(sg_imp_msvcr90_isalnum, 0, '7') && !CALL(sg_imp_msvcr90_isalnum, 0, '_'));
    CHECK(CALL(sg_imp_msvcr90_isupper, 0, 'Q') && !CALL(sg_imp_msvcr90_isupper, 0, 'q'));
    CHECK(!CALL(sg_imp_msvcr90_isdigit, 0, (uint32_t)-1)); /* EOF */
    CHECK_EQ(CALL(sg_imp_msvcr90_tolower, 0, 'Q'), 'q');
    CHECK_EQ(CALL(sg_imp_msvcr90_tolower, 0, 0xc9), 0xc9);
    CHECK_EQ(CALL(sg_imp_msvcr90_toupper, 0, 'z'), 'Z');
    CHECK_EQ(CALL(sg_imp_msvcr90_towlower, 0, 'A'), 'a');
    CHECK_EQ(CALL(sg_imp_msvcr90_towlower, 0, 0xc9), 0xc9); /* "C" locale: __ascii_towlower */
    CHECK_EQ(CALL(sg_imp_msvcr90_towupper, 0, 0x3b1), 0x3b1);
    CHECK(CALL(sg_imp_msvcr90_iswctype, 0, 0xe9, HLE_ALPHA) != 0); /* wide table covers Latin-1 */
    CHECK(CALL(sg_imp_msvcr90_iswctype, 0, 0x3b1, HLE_LOWER) != 0); /* Greek alpha, via host Unicode data */
    CHECK(CALL(sg_imp_msvcr90_iswctype, 0, '5', HLE_DIGIT) != 0);
    CHECK_EQ(CALL(sg_imp_msvcr90_iswctype, 0, 0xffff, 0xffff), 0); /* WEOF */

    /* UTF-16 */
    a = gwstr("Path\\To\\File.TXT");
    CHECK_EQ(CALL(sg_imp_msvcr90_wcschr, 0, a, '\\'), a + 8);
    CHECK_EQ(CALL(sg_imp_msvcr90_wcsrchr, 0, a, '\\'), a + 14);
    CHECK_EQ(CALL(sg_imp_msvcr90_wcsstr, 0, a, gwstr("File")), a + 16);
    CHECK_EQ(CALL(sg_imp_msvcr90_wcscspn, 0, a, gwstr(".\\")), 4);
    CHECK_EQ((int32_t)CALL(sg_imp_msvcr90__wcsicmp, 0, a, gwstr("path\\to\\file.txt")), 0);
    CHECK((int32_t)CALL(sg_imp_msvcr90__wcsicmp, 0, gwstr("a"), gwstr("B")) < 0);
    CHECK_EQ((int32_t)CALL(sg_imp_msvcr90__wcsnicmp, 0, gwstr("ABCx"), gwstr("abcy"), 3), 0);
    CHECK((int32_t)CALL(sg_imp_msvcr90_wcsncmp, 0, gwstr("abc"), gwstr("abd"), 3) < 0);
    b = gbuf(32);
    CALL(sg_imp_msvcr90_wcsncpy, 0, b, gwstr("xy"), 4);
    CHECK(wstreq(b, "xy") && sg_ld16(M, b + 6) == 0);
    b = gbuf(32);
    sg_st16(M, b, 'a'); sg_st16(M, b + 2, 'b');
    CALL(sg_imp_msvcr90_wcsncat, 0, b, gwstr("cdef"), 2);
    CHECK(wstreq(b, "abcd"));
    a = gwstr("MiXeD");
    CALL(sg_imp_msvcr90__wcslwr, 0, a);
    CHECK(wstreq(a, "mixed"));
    (void)s;
}
