/* crt_math.c -- _CI* x87 helpers, floor/ceil/modf/ldexp, _isnan/_finite, FP control words.
 *
 * Calling glue [obs from the x87 depth analysis]:
 *   _CIsqrt/_CIsin/_CIcos/_CIasin/_CIacos/_CIlog: ST0 = f(ST0)               (SG_CC_X87)
 *   _CIpow/_CIfmod:                              ST1 = f(ST1, ST0), pop       (SG_CC_X87)
 *   floor/ceil/modf/ldexp: cdecl double args on the guest stack, result pushed to ST0.
 * msvcr90 computes these in SSE2 double precision on SSE2 machines, so host
 * libm double is the reference and the x87 precision-control field does not apply.
 *
 * _control87/_controlfp map between msvcr90's abstract control word and the x87 CW / MXCSR
 * (constants from the _control87 reference; on x86 both units are written).
 */
#include <math.h>

#include "hle_imports.h"

#define A(i) SG_ARG(i)

#define CI_UNARY(name, expr)                         \
    SG_IMPORT(msvcr90, name) {                       \
        double x = SG_ST(ctx, 0);                    \
        SG_ST(ctx, 0) = (expr);                      \
        SG_RET(0);                                   \
    }
CI_UNARY(_CIsqrt, sqrt(x))
CI_UNARY(_CIsin, sin(x))
CI_UNARY(_CIcos, cos(x))
CI_UNARY(_CIasin, asin(x))
CI_UNARY(_CIacos, acos(x))
CI_UNARY(_CIlog, log(x))

/* Base in ST(1), exponent/divisor in ST(0) [obs: 0x92da3a `fld 10.0; fild n; call _CIpow`]. */
SG_IMPORT(msvcr90, _CIpow) {
    double y = sg_fpu_pop(ctx);
    SG_ST(ctx, 0) = pow(SG_ST(ctx, 0), y);
    SG_RET(0);
}
SG_IMPORT(msvcr90, _CIfmod) {
    double y = sg_fpu_pop(ctx);
    SG_ST(ctx, 0) = fmod(SG_ST(ctx, 0), y);
    SG_RET(0);
}

SG_IMPORT(msvcr90, floor) { sg_fpu_push(ctx, floor(SG_ARG_F64(0))); SG_RET(0); }
SG_IMPORT(msvcr90, ceil) { sg_fpu_push(ctx, ceil(SG_ARG_F64(0))); SG_RET(0); }
SG_IMPORT(msvcr90, modf) {
    double ip, f = modf(SG_ARG_F64(0), &ip);
    if (A(2)) sg_stf64(mem, A(2), ip);
    sg_fpu_push(ctx, f);
    SG_RET(0);
}
SG_IMPORT(msvcr90, ldexp) {
    double r = ldexp(SG_ARG_F64(0), (int32_t)A(2));
    if (isinf(r) && !isinf(SG_ARG_F64(0))) hle_set_errno(mem, MSVC_ERANGE);
    sg_fpu_push(ctx, r);
    SG_RET(0);
}
SG_IMPORT(msvcr90, _isnan) { SG_RETURN_CDECL(isnan(SG_ARG_F64(0)) ? 1 : 0); }
SG_IMPORT(msvcr90, _finite) { SG_RETURN_CDECL(isfinite(SG_ARG_F64(0)) ? 1 : 0); }

/* ---- control words --------------------------------------------------------------------- */

enum {
    EM_INEXACT = 0x1, EM_UNDERFLOW = 0x2, EM_OVERFLOW = 0x4, EM_ZERODIVIDE = 0x8, EM_INVALID = 0x10,
    EM_DENORMAL = 0x80000, MCW_EM = 0x8001f, MCW_RC = 0x300, MCW_PC = 0x30000, MCW_IC = 0x40000,
    MCW_DN = 0x3000000, DN_FLUSH = 0x1000000, EM_AMBIGUOUS = (int)0x80000000u
};

/* x87 CW exception-mask bits: IM DM ZM OM UM PM = bits 0..5; MXCSR has the same order at bit 7. */
static uint32_t masks_to_msvc(unsigned m6) {
    uint32_t r = 0;
    if (m6 & 0x01) r |= EM_INVALID;
    if (m6 & 0x02) r |= EM_DENORMAL;
    if (m6 & 0x04) r |= EM_ZERODIVIDE;
    if (m6 & 0x08) r |= EM_OVERFLOW;
    if (m6 & 0x10) r |= EM_UNDERFLOW;
    if (m6 & 0x20) r |= EM_INEXACT;
    return r;
}
static unsigned msvc_to_masks(uint32_t w) {
    return ((w & EM_INVALID) ? 0x01u : 0) | ((w & EM_DENORMAL) ? 0x02u : 0) | ((w & EM_ZERODIVIDE) ? 0x04u : 0) |
           ((w & EM_OVERFLOW) ? 0x08u : 0) | ((w & EM_UNDERFLOW) ? 0x10u : 0) | ((w & EM_INEXACT) ? 0x20u : 0);
}
/* RC encodings: x87/MXCSR 0 near, 1 down, 2 up, 3 chop; msvc 0, 0x100, 0x200, 0x300. */

static uint32_t x87_word(uint16_t cw) {
    static const uint32_t pc[4] = {0x20000 /* 24 */, 0x20000 /* reserved */, 0x10000 /* 53 */, 0 /* 64 */};
    return masks_to_msvc(cw & 0x3f) | ((uint32_t)((cw >> 10) & 3) << 8) | pc[(cw >> 8) & 3] |
           ((cw & 0x1000) ? MCW_IC : 0);
}
static uint32_t sse_word(uint32_t mx) {
    return masks_to_msvc((mx >> 7) & 0x3f) | ((uint32_t)((mx >> 13) & 3) << 8) |
           ((mx & 0x8040) ? DN_FLUSH : 0);
}

uint32_t hle_x87cw_to_msvc(uint16_t cw, uint32_t mxcsr) {
    uint32_t x = x87_word(cw), s = sse_word(mxcsr);
    uint32_t r = x | (s & MCW_DN);
    if ((x & (MCW_EM | MCW_RC)) != (s & (MCW_EM | MCW_RC))) r |= (uint32_t)EM_AMBIGUOUS;
    return r;
}

void hle_msvc_to_hw(uint32_t w, uint16_t *cw, uint32_t *mxcsr) {
    unsigned pcbits = (w & MCW_PC) == 0x20000 ? 0u : (w & MCW_PC) == 0x10000 ? 2u : 3u;
    *cw = (uint16_t)((*cw & ~0x1f3fu) | msvc_to_masks(w) | (((w >> 8) & 3u) << 10) | (pcbits << 8) |
                     ((w & MCW_IC) ? 0x1000u : 0));
    *mxcsr = (*mxcsr & ~0xffc0u) | (msvc_to_masks(w) << 7) | (((w >> 8) & 3u) << 13) |
             ((w & DN_FLUSH) ? 0x8040u : 0);
}

/* fpcntrl = (fpcntrl & ~mask) | (new & mask), applied to both units (x86 rules). */
static uint32_t control(sg_ctx *ctx, uint32_t nw, uint32_t mask) {
    uint32_t cur = hle_x87cw_to_msvc(ctx->fpu.cw, ctx->mxcsr) & ~(uint32_t)EM_AMBIGUOUS;
    if (mask) {
        uint32_t w = (cur & ~mask) | (nw & mask);
        hle_msvc_to_hw(w, &ctx->fpu.cw, &ctx->mxcsr);
        cur = hle_x87cw_to_msvc(ctx->fpu.cw, ctx->mxcsr);
    }
    return hle_x87cw_to_msvc(ctx->fpu.cw, ctx->mxcsr) | (cur & (uint32_t)EM_AMBIGUOUS);
}

SG_IMPORT(msvcr90, _control87) { SG_RETURN_CDECL(control(ctx, A(0), A(1))); }
/* _controlfp never changes the denormal-operand exception mask. */
SG_IMPORT(msvcr90, _controlfp) { SG_RETURN_CDECL(control(ctx, A(0), A(1) & ~(uint32_t)EM_DENORMAL)); }
/* errno_t _controlfp_s(unsigned *current, unsigned new, unsigned mask): invalid mask bits ->
 * invalid parameter, EINVAL. [obs] CRT start-up calls it as (NULL, _PC_53, _MCW_PC) at 0x11e156a. */
SG_IMPORT(msvcr90, _controlfp_s) {
    uint32_t cur = A(0), nw = A(1), mask = A(2), v;
    if (mask & ~(uint32_t)(MCW_EM | MCW_RC | MCW_PC | MCW_IC | MCW_DN)) {
        hle_invalid_parameter(ctx, "_controlfp_s", "invalid mask");
        if (cur) sg_st32(mem, cur, control(ctx, 0, 0));
        SG_RETURN_CDECL(MSVC_EINVAL);
    }
    v = control(ctx, nw, mask & ~(uint32_t)EM_DENORMAL);
    if (cur) sg_st32(mem, cur, v);
    SG_RETURN_CDECL(0);
}
