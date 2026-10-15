/* _CI* x87 helpers, floor/ceil/modf/ldexp, and the FP control words. */
#include <math.h>

#include "harness.h"

static uint32_t LO(double d) { uint32_t l, h; gdouble(d, &l, &h); return l; }
static uint32_t HI(double d) { uint32_t l, h; gdouble(d, &l, &h); return h; }
#define D(x) LO(x), HI(x)

void test_math(void) {
    uint32_t top0 = T.fpu.top, p = gbuf(8);

    /* unary: ST0 = f(ST0), stack depth unchanged */
    sg_fpu_push(&T, 2.0);
    CALL0(sg_imp_msvcr90__CIsqrt, 0);
    CHECK(SG_ST(&T, 0) == sqrt(2.0));
    CHECK_EQ(T.fpu.top, (top0 - 1) & 7);
    CALL0(sg_imp_msvcr90__CIlog, 0);
    CHECK(SG_ST(&T, 0) == log(sqrt(2.0)));
    SG_ST(&T, 0) = 0.5;
    CALL0(sg_imp_msvcr90__CIacos, 0);
    CHECK(SG_ST(&T, 0) == acos(0.5));
    sg_fpu_pop(&T);

    /* binary: base in ST1, exponent in ST0; one pop [obs pattern: fld 10.0; fild n; call _CIpow] */
    sg_fpu_push(&T, 10.0);
    sg_fpu_push(&T, 3.0);
    CALL0(sg_imp_msvcr90__CIpow, 0);
    CHECK(SG_ST(&T, 0) == 1000.0);
    CHECK_EQ(T.fpu.top, (top0 - 1) & 7);
    sg_fpu_pop(&T);
    sg_fpu_push(&T, 7.5);
    sg_fpu_push(&T, 2.0);
    CALL0(sg_imp_msvcr90__CIfmod, 0);
    CHECK(SG_ST(&T, 0) == 1.5);
    sg_fpu_pop(&T);
    CHECK_EQ(T.fpu.top, top0);

    /* cdecl double arguments, result pushed */
    CALL(sg_imp_msvcr90_floor, 0, D(-1.5));
    CHECK(sg_fpu_pop(&T) == -2.0);
    CALL(sg_imp_msvcr90_ceil, 0, D(-1.5));
    CHECK(sg_fpu_pop(&T) == -1.0);
    CALL(sg_imp_msvcr90_modf, 0, D(3.25), p);
    CHECK(sg_fpu_pop(&T) == 0.25 && sg_ldf64(M, p) == 3.0);
    CALL(sg_imp_msvcr90_ldexp, 0, D(1.0), 10);
    CHECK(sg_fpu_pop(&T) == 1024.0);
    CHECK(CALL(sg_imp_msvcr90__isnan, 0, D(NAN)) == 1 && CALL(sg_imp_msvcr90__isnan, 0, D(1.0)) == 0);
    CHECK(CALL(sg_imp_msvcr90__finite, 0, D(1.0)) == 1 && CALL(sg_imp_msvcr90__finite, 0, D(INFINITY)) == 0);
    CHECK_EQ(T.fpu.top, top0);

    /* control words. Reset state: x87 CW 0x027f, MXCSR 0x1f80. The _control87 reference's
     * example prints 0x0009001f for the default and 0x000a001f after setting _PC_24. */
    CHECK_EQ(CALL(sg_imp_msvcr90__controlfp, 0, 0, 0), 0x0009001fu);
    CHECK_EQ(CALL(sg_imp_msvcr90__controlfp, 0, 0x20000 /* _PC_24 */, 0x30000 /* _MCW_PC */), 0x000a001fu);
    CHECK_EQ(T.fpu.cw, 0x007f);
    CHECK_EQ(T.mxcsr, 0x1f80); /* PC is x87-only */
    CHECK_EQ(CALL(sg_imp_msvcr90__controlfp, 0, 0x300 /* _RC_CHOP */, 0x300 /* _MCW_RC */), 0x000a031fu);
    CHECK_EQ(T.fpu.cw, 0x0c7f);
    CHECK_EQ(T.mxcsr, 0x7f80); /* both units on x86 */
    CHECK_EQ(CALL(sg_imp_msvcr90__controlfp, 0, 0, 0x300), 0x000a001fu);
    /* _controlfp leaves the denormal mask alone; _control87 changes it */
    CALL(sg_imp_msvcr90__controlfp, 0, 0x10 /* _EM_INVALID only */, 0x8001f /* _MCW_EM */);
    CHECK_EQ(T.fpu.cw & 0x3f, 0x01 | 0x02); /* IM and DM still set */
    CALL(sg_imp_msvcr90__control87, 0, 0x8001f, 0x8001f);
    CHECK_EQ(T.fpu.cw & 0x3f, 0x3f);
    CALL(sg_imp_msvcr90__control87, 0, 0x1f, 0x8001f); /* unmask denormal */
    CHECK_EQ(T.fpu.cw & 0x3f, 0x3d);
    CHECK_EQ(T.mxcsr & 0x1f80, 0x1e80);
    CALL(sg_imp_msvcr90__control87, 0, 0x8001f, 0x8001f);
    /* _controlfp_s as the CRT start-up uses it [obs 0x11e156a: (NULL, _PC_53, _MCW_PC)] */
    CHECK_EQ(CALL(sg_imp_msvcr90__controlfp_s, 0, 0, 0x10000, 0x30000), 0);
    CHECK_EQ(T.fpu.cw, 0x027f);
    CHECK_EQ(CALL(sg_imp_msvcr90__controlfp_s, 0, p, 0, 0), 0);
    CHECK_EQ(sg_ld32(M, p), 0x0009001fu);
    CHECK_EQ(CALL(sg_imp_msvcr90__controlfp_s, 0, p, 0, 0x80000000u), MSVC_EINVAL);
    CHECK_EQ(hle_x87cw_to_msvc(0x027f, 0x1f80), 0x0009001fu);
    CHECK(hle_x87cw_to_msvc(0x027f, 0x1f80 | 0x6000) & 0x80000000u); /* units disagree: _EM_AMBIGUOUS */
}
