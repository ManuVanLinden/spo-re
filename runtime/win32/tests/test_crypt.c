/* test_crypt.c -- advapi32 CryptAcquireContextW / CryptGenRandom / CryptReleaseContext (w32_crypt.c). */
#include "harness.h"
#include "w32_int.h"

#include <string.h>

#include "hle_imports.h"

void test_crypt(void) {
    uint32_t ph = gbuf(4), buf = gbuf(64), buf2 = gbuf(64);
    CHECK_EQ(CALL(sg_imp_advapi32_CryptAcquireContextW, 20, ph, 0, 0, 1 /* PROV_RSA_FULL */, 0xf0000000u), 1);
    CHECK(sg_ld32(M, ph) != 0);
    CHECK_EQ(CALL(sg_imp_advapi32_CryptAcquireContextW, 20, 0, 0, 0, 1, 0), 0);
    CHECK_EQ(last_error(), 87);
    uint32_t h = sg_ld32(M, ph);
    CHECK_EQ(CALL(sg_imp_advapi32_CryptGenRandom, 12, h, 64, buf), 1);
    CHECK_EQ(CALL(sg_imp_advapi32_CryptGenRandom, 12, h, 64, buf2), 1);
    CHECK(memcmp(M + buf, M + buf2, 64) != 0);          /* two draws differ */
    int nonzero = 0;
    for (int i = 0; i < 64; i++) nonzero += M[buf + i] != 0;
    CHECK(nonzero > 32);                                  /* filled, not left zero */
    CHECK_EQ(CALL(sg_imp_advapi32_CryptGenRandom, 12, h, 0, buf), 1);
    CHECK_EQ(CALL(sg_imp_advapi32_CryptGenRandom, 12, h, 8, 0), 0);
    CHECK_EQ(last_error(), 87);
    CHECK_EQ(CALL(sg_imp_advapi32_CryptReleaseContext, 8, h, 0), 1);
}
