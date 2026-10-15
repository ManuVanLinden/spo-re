/* qsort (guest comparator through sg_call_guest), rand, errno, pointer encoding. */
#include <stdlib.h>

#include "harness.h"

static unsigned ncmp;
static uint32_t cmp_int(sg_ctx *ctx, unsigned nargs, const uint32_t *args) {
    (void)ctx; (void)nargs;
    int32_t a = (int32_t)sg_ld32(M, args[0]), b = (int32_t)sg_ld32(M, args[1]);
    ncmp++;
    return (uint32_t)(a < b ? -1 : a > b);
}
/* compares only the key (high 16 bits); the low half records the original position */
static uint32_t cmp_key(sg_ctx *ctx, unsigned nargs, const uint32_t *args) {
    (void)ctx; (void)nargs;
    uint32_t a = sg_ld32(M, args[0]) >> 16, b = sg_ld32(M, args[1]) >> 16;
    return (uint32_t)(a < b ? -1 : a > b);
}

void test_misc(void) {
    uint32_t base = gbuf(4 * 1000), cmp = register_callback(cmp_int), r;
    int i, ok = 1;

    srand(7);
    for (i = 0; i < 1000; i++) sg_st32(M, base + 4u * (uint32_t)i, (uint32_t)(rand() % 200 - 100));
    CALL(sg_imp_msvcr90_qsort, 0, base, 1000, 4, cmp);
    for (i = 1; i < 1000; i++)
        if ((int32_t)sg_ld32(M, base + 4u * (uint32_t)i) < (int32_t)sg_ld32(M, base + 4u * (uint32_t)(i - 1))) ok = 0;
    CHECK(ok);
    CHECK(ncmp > 1000);
    CALL(sg_imp_msvcr90_qsort, 0, base, 1, 4, cmp); /* trivial sizes */
    CALL(sg_imp_msvcr90_qsort, 0, base, 0, 4, cmp);
    {
        /* ties: 12 elements, keys 0..2. The result must be sorted by key; the order of equal
         * keys is whatever the msvcr90 algorithm produces (recorded here as a regression value). */
        uint32_t b2 = gbuf(48), k = register_callback(cmp_key);
        static const uint32_t keys[12] = {2, 0, 1, 2, 0, 1, 2, 0, 1, 2, 0, 1};
        char order[64];
        int n = 0;
        for (i = 0; i < 12; i++) sg_st32(M, b2 + 4u * (uint32_t)i, keys[i] << 16 | (uint32_t)i);
        CALL(sg_imp_msvcr90_qsort, 0, b2, 12, 4, k);
        ok = 1;
        for (i = 0; i < 12; i++) {
            uint32_t v = sg_ld32(M, b2 + 4u * (uint32_t)i);
            if (i && (v >> 16) < (sg_ld32(M, b2 + 4u * (uint32_t)(i - 1)) >> 16)) ok = 0;
            n += snprintf(order + n, sizeof order - (size_t)n, "%x", v & 0xffff);
        }
        CHECK(ok);
        printf("  qsort tie order (12 elements, keys 2,0,1,...): %s\n", order);
    }

    /* rand: msvcr90 LCG from seed 1 */
    {
        static const uint32_t want[5] = {41, 18467, 6334, 26500, 19169};
        for (i = 0; i < 5; i++) CHECK_EQ(CALL0(sg_imp_msvcr90_rand, 0), want[i]);
    }

    /* errno lives in guest memory, per thread */
    r = CALL0(sg_imp_msvcr90__errno, 0);
    hle_set_errno(M, MSVC_ERANGE);
    CHECK_EQ(sg_ld32(M, r), 34);
    CHECK_EQ(CALL(sg_imp_msvcr90__decode_pointer, 0, CALL(sg_imp_msvcr90__encode_pointer, 0, 0x1234)), 0x1234);

    /* locale */
    r = CALL(sg_imp_msvcr90_setlocale, 0, 0, 0);
    CHECK_STR(hstr(r), "C");
    r = CALL(sg_imp_msvcr90_setlocale, 0, 4, gstr("English_United States.1252"));
    CHECK_STR(hstr(r), "English_United States.1252");
    r = CALL(sg_imp_msvcr90_setlocale, 0, 0, 0);
    CHECK(strstr(hstr(r), "LC_NUMERIC=English_United States.1252") != NULL);
    CHECK_EQ(CALL(sg_imp_msvcr90_setlocale, 0, 9, 0), 0);
    CHECK_EQ(CALL(sg_imp_msvcr90__setmbcp, 0, (uint32_t)-4), 0);
    CHECK_EQ(CALL(sg_imp_msvcr90_getenv, 0, gstr("JPEGMEM")), 0);

    /* last error and Interlocked* */
    CALL(sg_imp_kernel32_SetLastError, 4, 1234);
    CHECK_EQ(CALL0(sg_imp_kernel32_GetLastError, 0), 1234);
    CHECK_EQ(sg_ld32(M, G_TEB + SG_TEB_LAST_ERROR), 1234);
    {
        uint32_t v = gbuf(4);
        sg_st32(M, v, 5);
        CHECK_EQ(CALL(sg_imp_kernel32_InterlockedIncrement, 4, v), 6);
        CHECK_EQ(CALL(sg_imp_kernel32_InterlockedDecrement, 4, v), 5);
        CHECK_EQ(CALL(sg_imp_kernel32_InterlockedExchangeAdd, 8, v, 10), 5);
        CHECK_EQ(CALL(sg_imp_kernel32_InterlockedExchange, 8, v, 99), 15);
        CHECK_EQ(CALL(sg_imp_kernel32_InterlockedCompareExchange, 12, v, 7, 98), 99); /* no swap */
        CHECK_EQ(sg_ld32(M, v), 99);
        CHECK_EQ(CALL(sg_imp_kernel32_InterlockedCompareExchange, 12, v, 7, 99), 99); /* swap */
        CHECK_EQ(sg_ld32(M, v), 7);
    }
}
