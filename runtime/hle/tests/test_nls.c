/* MultiByteToWideChar / WideCharToMultiByte / GetACP. */
#include "harness.h"

#define CP_ACP 0
#define CP_UTF8 65001
#define MB_ERR_INVALID_CHARS 8

void test_nls(void) {
    uint32_t w = gbuf(64), n = gbuf(64), used = gbuf(4);
    uint32_t u8 = gstr("h\xc3\xa9llo \xe2\x82\xac \xf0\x9f\x98\x80"); /* hello with e-acute, euro, U+1F600 */

    CHECK_EQ(CALL0(sg_imp_kernel32_GetACP, 0), 1252);

    /* size query including the NUL (-1), then conversion */
    CHECK_EQ(CALL(sg_imp_kernel32_MultiByteToWideChar, 24, CP_UTF8, 0, u8, (uint32_t)-1, 0, 0), 11);
    CHECK_EQ(CALL(sg_imp_kernel32_MultiByteToWideChar, 24, CP_UTF8, 0, u8, (uint32_t)-1, w, 32), 11);
    CHECK_EQ(sg_ld16(M, w + 2), 0xe9);
    CHECK_EQ(sg_ld16(M, w + 12), 0x20ac);
    CHECK_EQ(sg_ld16(M, w + 16), 0xd83d);
    CHECK_EQ(sg_ld16(M, w + 18), 0xde00);
    CHECK_EQ(sg_ld16(M, w + 20), 0);
    /* explicit length: no NUL */
    CHECK_EQ(CALL(sg_imp_kernel32_MultiByteToWideChar, 24, CP_UTF8, 0, u8, 3, w, 32), 2);
    /* too small */
    CHECK_EQ(CALL(sg_imp_kernel32_MultiByteToWideChar, 24, CP_UTF8, 0, u8, (uint32_t)-1, w, 4), 0);
    CHECK_EQ(hle_get_last_error(&T, M), 122);
    /* invalid UTF-8 */
    CHECK_EQ(CALL(sg_imp_kernel32_MultiByteToWideChar, 24, CP_UTF8, MB_ERR_INVALID_CHARS, gstr("a\xff" "b"), (uint32_t)-1, w, 32), 0);
    CHECK_EQ(hle_get_last_error(&T, M), 1113);
    CHECK_EQ(CALL(sg_imp_kernel32_MultiByteToWideChar, 24, CP_UTF8, 0, gstr("a\xff" "b"), (uint32_t)-1, w, 32), 4);
    CHECK_EQ(sg_ld16(M, w + 2), 0xfffd);
    CHECK_EQ(CALL(sg_imp_kernel32_MultiByteToWideChar, 24, CP_UTF8, 1 /* MB_PRECOMPOSED */, u8, (uint32_t)-1, w, 32), 0);
    CHECK_EQ(hle_get_last_error(&T, M), 1004);
    CHECK_EQ(CALL(sg_imp_kernel32_MultiByteToWideChar, 24, CP_UTF8, 0, u8, 0, w, 32), 0);
    CHECK_EQ(hle_get_last_error(&T, M), 87);
    /* the ANSI code page is 1252 */
    CHECK_EQ(CALL(sg_imp_kernel32_MultiByteToWideChar, 24, CP_ACP, 0, gstr("\x80\xe9"), (uint32_t)-1, w, 32), 3);
    CHECK_EQ(sg_ld16(M, w), 0x20ac);
    CHECK_EQ(sg_ld16(M, w + 2), 0xe9);

    /* back to UTF-8 */
    CALL(sg_imp_kernel32_MultiByteToWideChar, 24, CP_UTF8, 0, u8, (uint32_t)-1, w, 32);
    CHECK_EQ(CALL(sg_imp_kernel32_WideCharToMultiByte, 32, CP_UTF8, 0, w, (uint32_t)-1, 0, 0, 0, 0), 16);
    CHECK_EQ(CALL(sg_imp_kernel32_WideCharToMultiByte, 32, CP_UTF8, 0, w, (uint32_t)-1, n, 64, 0, 0), 16);
    CHECK_STR(hstr(n), hstr(u8));
    CHECK_EQ(CALL(sg_imp_kernel32_WideCharToMultiByte, 32, CP_UTF8, 0, w, (uint32_t)-1, n, 64, 0, used), 0);
    CHECK_EQ(hle_get_last_error(&T, M), 87); /* UTF-8 forbids lpUsedDefaultChar */
    CHECK_EQ(CALL(sg_imp_kernel32_WideCharToMultiByte, 32, CP_UTF8, 0, w, (uint32_t)-1, n, 3, 0, 0), 0);
    CHECK_EQ(hle_get_last_error(&T, M), 122);
    /* 1252 with an unmappable character */
    {
        static const uint16_t s[] = {'a', 0x20ac, 0x101, 0};
        uint32_t ws = gwstr16(s);
        CHECK_EQ(CALL(sg_imp_kernel32_WideCharToMultiByte, 32, 1252, 0, ws, (uint32_t)-1, n, 64, 0, used), 4);
        CHECK(!memcmp(M + n, "a\x80?", 4));
        CHECK_EQ(sg_ld32(M, used), 1);
    }
    /* unpaired surrogate -> U+FFFD */
    {
        static const uint16_t s[] = {0xd800, 'x', 0};
        CHECK_EQ(CALL(sg_imp_kernel32_WideCharToMultiByte, 32, CP_UTF8, 0, gwstr16(s), (uint32_t)-1, n, 64, 0, 0), 5);
        CHECK(!memcmp(M + n, "\xef\xbf\xbdx", 5));
    }
}
