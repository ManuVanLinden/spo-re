/*
 * sg_recomp.h -- helpers that the translator (recomp/) emits calls to, on top of spore_guest.h.
 *
 * spore_guest.h is the ABI: context, memory access, calls, returns, dispatch, landings.
 * This header adds only instruction semantics that are too long to inline as literal C in the
 * generated code: lazy-flag evaluation on a function-local record, x87 stack arithmetic with
 * precision control (the guest control word's PC field), SSE/MMX lane operations, string instructions and
 * division checks. Everything here is static inline except the few [runtime] hooks at the end.
 *
 * No game code and no game bytes. Plain C11, clang.
 *
 * Build switches:
 *   SG_X87_PC_EMULATE (default 1)  round fadd/fsub/fmul/fdiv/fsqrt results to float while the
 *                                  x87 precision-control field says 24-bit
 *   SGR_FLAGS_AT_EXIT  (default 0) write the function-local flags record back to ctx->fl at every
 *                                  return and tail call. Used by the fixture harness to compare
 *                                  flags; the ABI itself treats flags as dead at boundaries (D4)
 */
#ifndef SG_RECOMP_H
#define SG_RECOMP_H

#include "spore_guest.h"
#include <math.h>

#ifndef SG_X87_PC_EMULATE
#define SG_X87_PC_EMULATE 1
#endif
#ifndef SGR_FLAGS_AT_EXIT
#define SGR_FLAGS_AT_EXIT 0
#endif

/* ------------------------------------------------------------------------------------------ */
/* Flags: generated code keeps the lazy record in a local `lf` and evaluates it with the        */
/* header's own evaluators (one source of truth). With a constant `op` clang folds the switch.  */
/* ------------------------------------------------------------------------------------------ */

#define SGR_FL(kind, size, res, dst, src) \
    ((sg_lazyflags){SG_FOP((kind), (size)), (uint32_t)(res), (uint32_t)(dst), (uint32_t)(src)})
#define SGR_FLC(kind, size, res, dst, src, cin) \
    ((sg_lazyflags){SG_FOP((kind), (size)) | ((cin) ? SG_FOP_CIN : 0u), (uint32_t)(res), (uint32_t)(dst), (uint32_t)(src)})
#define SGR_FLE(bits) ((sg_lazyflags){SG_FOP_EFLAGS, (uint32_t)(bits) & SG_ARITH_FLAGS, 0u, 0u})

SG_INLINE int sgr_cond(sg_lazyflags f, unsigned cc) { sg_ctx t; t.fl = f; return sg_cond(&t, cc); }
SG_INLINE uint32_t sgr_cf(sg_lazyflags f) { sg_ctx t; t.fl = f; return (uint32_t)sg_flag_cf(&t); }
SG_INLINE uint32_t sgr_eflags(sg_lazyflags f) { sg_ctx t; t.fl = f; t.sysfl = 0; return sg_get_eflags(&t) & SG_ARITH_FLAGS; }
/* Replace the flags in `mask` with the bits in `val`, keeping the others (rol/ror/bt/bsr...). */
SG_INLINE sg_lazyflags sgr_merge(sg_lazyflags f, uint32_t mask, uint32_t val) {
    return SGR_FLE((sgr_eflags(f) & ~mask) | (val & mask));
}
SG_INLINE uint32_t sgr_szp(uint32_t r, unsigned bits) { /* SF ZF PF of a result */
    uint32_t m = bits == 32 ? 0xffffffffu : ((1u << bits) - 1u);
    return ((r & m) == 0 ? SG_ZF : 0u) | ((r >> (bits - 1u)) & 1u ? SG_SF : 0u) | (sg_parity8(r) ? SG_PF : 0u);
}

#if SGR_FLAGS_AT_EXIT
#define SGR_EXIT_FLAGS() (ctx->fl = lf)
#else
#define SGR_EXIT_FLAGS() ((void)0)
#endif

/* Partial registers by name (ModRM numbering). */
#define SGR_SET8(n, hi, v) do { if (hi) SG_SET_R8H(ctx, n, v); else SG_SET_R8L(ctx, n, v); } while (0)

/* ------------------------------------------------------------------------------------------ */
/* Shifts and rotates whose flag effects do not fit the header's record kinds                   */
/* ------------------------------------------------------------------------------------------ */

SG_INLINE uint32_t sgr_rol(uint32_t v, unsigned c, unsigned bits) {
    uint32_t m = bits == 32 ? 0xffffffffu : ((1u << bits) - 1u);
    v &= m; c %= bits;
    return c ? ((v << c) | (v >> (bits - c))) & m : v;
}
SG_INLINE uint32_t sgr_ror(uint32_t v, unsigned c, unsigned bits) {
    uint32_t m = bits == 32 ? 0xffffffffu : ((1u << bits) - 1u);
    v &= m; c %= bits;
    return c ? ((v >> c) | (v << (bits - c))) & m : v;
}
/* rcl/rcr: rotate through carry. count already masked to 5 bits; returns result, *cf updated. */
SG_INLINE uint32_t sgr_rcl(uint32_t v, unsigned c, unsigned bits, uint32_t *cf) {
    uint64_t m = (1ull << bits) - 1u, x = ((uint64_t)(v & m)) | ((uint64_t)(*cf & 1u) << bits);
    unsigned n = c % (bits + 1u), i;
    for (i = 0; i < n; i++) x = ((x << 1) | (x >> bits)) & ((m << 1) | 1u);
    *cf = (uint32_t)(x >> bits) & 1u;
    return (uint32_t)(x & m);
}
SG_INLINE uint32_t sgr_rcr(uint32_t v, unsigned c, unsigned bits, uint32_t *cf) {
    uint64_t m = (1ull << bits) - 1u, x = ((uint64_t)(v & m)) | ((uint64_t)(*cf & 1u) << bits);
    unsigned n = c % (bits + 1u), i;
    for (i = 0; i < n; i++) x = ((x >> 1) | ((x & 1u) << bits)) & ((m << 1) | 1u);
    *cf = (uint32_t)(x >> bits) & 1u;
    return (uint32_t)(x & m);
}

/* ------------------------------------------------------------------------------------------ */
/* Division (#DE is explicit: arm64 does not trap, runtime-abi.md 4.6)                          */
/* ------------------------------------------------------------------------------------------ */

SG_INLINE void sgr_div32(sg_ctx *ctx, uint32_t d, uint32_t va) {
    uint64_t n = ((uint64_t)ctx->edx << 32) | ctx->eax, q;
    if (SG_UNLIKELY(d == 0)) sg_divide_error(ctx, va);
    q = n / d;
    if (SG_UNLIKELY(q > 0xffffffffull)) sg_divide_error(ctx, va);
    ctx->edx = (uint32_t)(n % d); ctx->eax = (uint32_t)q;
}
SG_INLINE void sgr_idiv32(sg_ctx *ctx, uint32_t d, uint32_t va) {
    int64_t n = (int64_t)(((uint64_t)ctx->edx << 32) | ctx->eax), q;
    if (SG_UNLIKELY(d == 0)) sg_divide_error(ctx, va);
    if (SG_UNLIKELY(n == INT64_MIN && (int32_t)d == -1)) sg_divide_error(ctx, va);
    q = n / (int32_t)d;
    if (SG_UNLIKELY(q > INT32_MAX || q < INT32_MIN)) sg_divide_error(ctx, va);
    ctx->edx = (uint32_t)(int32_t)(n % (int32_t)d); ctx->eax = (uint32_t)(int32_t)q;
}
SG_INLINE void sgr_div16(sg_ctx *ctx, uint32_t d, uint32_t va) {
    uint32_t n = ((uint32_t)SG_R16(ctx, SG_EDX) << 16) | SG_R16(ctx, SG_EAX), q;
    d &= 0xffffu;
    if (SG_UNLIKELY(d == 0)) sg_divide_error(ctx, va);
    q = n / d;
    if (SG_UNLIKELY(q > 0xffffu)) sg_divide_error(ctx, va);
    SG_SET_R16(ctx, SG_EDX, n % d); SG_SET_R16(ctx, SG_EAX, q);
}
SG_INLINE void sgr_idiv16(sg_ctx *ctx, uint32_t d, uint32_t va) {
    int32_t n = (int32_t)(((uint32_t)SG_R16(ctx, SG_EDX) << 16) | SG_R16(ctx, SG_EAX)), q, s = (int16_t)d;
    if (SG_UNLIKELY(s == 0)) sg_divide_error(ctx, va);
    q = n / s;
    if (SG_UNLIKELY(q > 32767 || q < -32768)) sg_divide_error(ctx, va);
    SG_SET_R16(ctx, SG_EDX, (uint16_t)(n % s)); SG_SET_R16(ctx, SG_EAX, (uint16_t)q);
}
SG_INLINE void sgr_div8(sg_ctx *ctx, uint32_t d, uint32_t va) {
    uint32_t n = SG_R16(ctx, SG_EAX), q;
    d &= 0xffu;
    if (SG_UNLIKELY(d == 0)) sg_divide_error(ctx, va);
    q = n / d;
    if (SG_UNLIKELY(q > 0xffu)) sg_divide_error(ctx, va);
    SG_SET_R16(ctx, SG_EAX, ((n % d) << 8) | q);
}
SG_INLINE void sgr_idiv8(sg_ctx *ctx, uint32_t d, uint32_t va) {
    int32_t n = (int16_t)SG_R16(ctx, SG_EAX), s = (int8_t)d, q;
    if (SG_UNLIKELY(s == 0)) sg_divide_error(ctx, va);
    q = n / s;
    if (SG_UNLIKELY(q > 127 || q < -128)) sg_divide_error(ctx, va);
    SG_SET_R16(ctx, SG_EAX, ((uint32_t)(uint8_t)(n % s) << 8) | (uint8_t)q);
}

SG_INLINE uint32_t sgr_bsr(uint32_t v) { return 31u - (uint32_t)__builtin_clz(v); } /* v != 0 */
SG_INLINE uint32_t sgr_bsf(uint32_t v) { return (uint32_t)__builtin_ctz(v); }

/* ------------------------------------------------------------------------------------------ */
/* String instructions (DF is always 0 [obs], so all run forward)                                */
/* ------------------------------------------------------------------------------------------ */

/* rep movs: element by element, so overlapping forward copies replicate as on x86. */
SG_INLINE void sgr_rep_movs(sg_ctx *ctx, uint8_t *mem, unsigned sz, int rep) {
    uint32_t n = rep ? ctx->ecx : 1u, s = ctx->esi, d = ctx->edi, bytes = n * sz;
    if (!n) return;
    if (d - s >= bytes) {
        /* destination below the source or disjoint: a forward copy equals memmove */
        memmove(mem + d, mem + s, bytes);
    } else {
        /* destination inside the source: x86 reads and writes one element at a time */
        uint32_t i; uint64_t e;
        for (i = 0; i < n; i++) { __builtin_memcpy(&e, mem + s + i * sz, sz); __builtin_memcpy(mem + d + i * sz, &e, sz); }
    }
    ctx->esi = s + bytes; ctx->edi = d + bytes;
    if (rep) ctx->ecx = 0;
}
SG_INLINE void sgr_rep_stos(sg_ctx *ctx, uint8_t *mem, unsigned sz, int rep) {
    uint32_t n = rep ? ctx->ecx : 1u, d = ctx->edi, i, v = ctx->eax;
    if (sz == 1) memset(mem + d, (int)(v & 0xffu), n);
    else for (i = 0; i < n; i++) __builtin_memcpy(mem + d + i * sz, &v, sz);
    ctx->edi = d + n * sz;
    if (rep) ctx->ecx = 0;
}
/* repe cmps / cmps: compares [esi] with [edi]; flags are those of the last compare (SUB). */
SG_INLINE void sgr_repe_cmps(sg_ctx *ctx, uint8_t *mem, unsigned sz, int rep, sg_lazyflags *lf) {
    unsigned size = sz == 1 ? 0u : sz == 2 ? 1u : 2u;
    if (rep && ctx->ecx == 0) return;
    for (;;) {
        uint32_t a = 0, b = 0;
        __builtin_memcpy(&a, mem + ctx->esi, sz); __builtin_memcpy(&b, mem + ctx->edi, sz);
        ctx->esi += sz; ctx->edi += sz;
        *lf = SGR_FL(SG_FOP_SUB, size, a - b, a, b);
        if (!rep) return;
        if (--ctx->ecx == 0 || a != b) return;
    }
}

/* ------------------------------------------------------------------------------------------ */
/* x87. Values are host doubles (precision control below). The stack is physical in ctx->fpu;   */
/* every instruction updates it through these helpers ("x87 as runtime helper calls"). Keeping  */
/* the stack in C locals, where a static stack-depth analysis allows it, is a later step.       */
/* ------------------------------------------------------------------------------------------ */

#define SGR_ST(i) SG_ST(ctx, i)
#define SGR_X87_C0 0x0100u
#define SGR_X87_C1 0x0200u
#define SGR_X87_C2 0x0400u
#define SGR_X87_C3 0x4000u

/* Precision control, derived from the guest control word: PC = cw bits 8..9 (0 = 24-bit). */
#if SG_X87_PC_EMULATE
/* 24-bit precision control rounds the significand to 24 bits but keeps the extended exponent range:
 * a plain (float) cast would overflow to inf above FLT_MAX and lose bits below FLT_MIN (found at
 * 0xa7c8b6 dividing by a float denormal under PC=24: x87 gives 4.7e42, the cast gave inf). */
SG_INLINE double sgr_pc24(double v) {
    double a = fabs(v);
    int e;
    if (a >= 0x1p-126 && a < 0x1p127) return (double)(float)v; /* float-normal range: the cast is exact rounding */
    if (a == 0.0 || a != a || a == __builtin_inf()) return v;
    v = frexp(v, &e); /* 0.5 <= |v| < 1: the cast rounds to 24 bits, nearest-even */
    return ldexp((double)(float)v, e);
}
#define SG_X87_PC(ctx, v) ((((ctx)->fpu.cw >> 8) & 3u) == 0u ? sgr_pc24((double)(v)) : (double)(v))
#else
#define SG_X87_PC(ctx, v) ((double)(v))
#endif

SG_INLINE void sgr_fpush(sg_ctx *ctx, double v) { sg_fpu_push(ctx, v); }
SG_INLINE double sgr_fpop(sg_ctx *ctx) { return sg_fpu_pop(ctx); }
SG_INLINE void sgr_fxch(sg_ctx *ctx, unsigned i) { double t = SGR_ST(0); SGR_ST(0) = SGR_ST(i); SGR_ST(i) = t; }

/* fcom family: C3 C2 C0 = 000 (>), 001 (<), 100 (=), 111 (unordered). C1 cleared. */
SG_INLINE void sgr_fcom(sg_ctx *ctx, double a, double b) {
    uint16_t c = a > b ? 0u : a < b ? SGR_X87_C0 : a == b ? SGR_X87_C3 : (SGR_X87_C0 | SGR_X87_C2 | SGR_X87_C3);
    ctx->fpu.sw = (uint16_t)((ctx->fpu.sw & ~(SGR_X87_C0 | SGR_X87_C1 | SGR_X87_C2 | SGR_X87_C3)) | c);
}
/* fcomi/fucomi and comiss/ucomiss: ZF PF CF = 000 (>), 001 (<), 100 (=), 111 (unordered); OF SF AF = 0. */
SG_INLINE uint32_t sgr_fcmp_eflags(double a, double b) {
    return a > b ? 0u : a < b ? SG_CF : a == b ? SG_ZF : (SG_ZF | SG_PF | SG_CF);
}
/* fnstsw: status word with TOP synthesised from ctx->fpu.top (runtime-abi.md 4.4). */
SG_INLINE uint16_t sgr_fnstsw(const sg_ctx *ctx) {
    return (uint16_t)((ctx->fpu.sw & ~0x3800u) | ((ctx->fpu.top & 7u) << 11));
}
/* Round to integer under the control word's RC field (frndint, fist/fistp). The host runs in
 * round-to-nearest-even, which is RC=0. */
SG_INLINE double sgr_frnd(const sg_ctx *ctx, double v) {
    switch ((ctx->fpu.cw >> 10) & 3u) {
    case 0: return nearbyint(v);
    case 1: return floor(v);
    case 2: return ceil(v);
    default: return trunc(v);
    }
}
/* fist/fistp: out of range or NaN gives the "integer indefinite" value. */
SG_INLINE uint32_t sgr_fist32(const sg_ctx *ctx, double v) {
    double r = sgr_frnd(ctx, v);
    return (r >= -2147483648.0 && r < 2147483648.0) ? (uint32_t)(int32_t)r : 0x80000000u;
}
SG_INLINE uint16_t sgr_fist16(const sg_ctx *ctx, double v) {
    double r = sgr_frnd(ctx, v);
    return (r >= -32768.0 && r < 32768.0) ? (uint16_t)(int16_t)r : (uint16_t)0x8000u;
}
SG_INLINE uint64_t sgr_fist64(const sg_ctx *ctx, double v) {
    double r = sgr_frnd(ctx, v);
    return (r >= -9223372036854775808.0 && r < 9223372036854775808.0) ? (uint64_t)(int64_t)r : 0x8000000000000000ull;
}
/* Loads of float/double memory operands. */
SG_INLINE double sgr_ldf32(const uint8_t *mem, uint32_t a) { return (double)sg_ldf32(mem, a); }
/* Stores round the double to float with the host's rounding (nearest; RC is nearest outside the
 * truncation windows the game sets around fistp only). */
SG_INLINE void sgr_stf32(uint8_t *mem, uint32_t a, double v) { sg_stf32(mem, a, (float)v); }
/* ordered forms (docs/design/memory-ordering.md): emitted for accesses the memory-ordering policy orders */
SG_INLINE double sgr_ldf32_acq(const uint8_t *mem, uint32_t a) { return (double)sg_ldf32_acq_u(mem, a); }
SG_INLINE void sgr_stf32_rel(uint8_t *mem, uint32_t a, double v) { sg_stf32_rel_u(mem, a, (float)v); }

/* Transcendentals. fptan pushes 1.0; fpatan/fyl2x pop.
 * fsin/fcos/fsincos/fptan accept |x| < 2^63 only: a finite operand outside that range leaves ST(0)
 * unchanged (fptan/fsincos push nothing) and sets C2; in range C2 is cleared (Intel SDM; the plain libm
 * call was found wrong at 0xa7c8be). Infinities still give NaN, as on hardware. */
SG_INLINE int sgr_ftrig_ok(sg_ctx *ctx, double v) {
    int out = isfinite(v) && fabs(v) >= 9223372036854775808.0;
    ctx->fpu.sw = (uint16_t)((ctx->fpu.sw & ~SGR_X87_C2) | (out ? SGR_X87_C2 : 0u));
    return !out;
}
SG_INLINE void sgr_fsin(sg_ctx *ctx) { if (sgr_ftrig_ok(ctx, SGR_ST(0))) SGR_ST(0) = sin(SGR_ST(0)); }
SG_INLINE void sgr_fcos(sg_ctx *ctx) { if (sgr_ftrig_ok(ctx, SGR_ST(0))) SGR_ST(0) = cos(SGR_ST(0)); }
SG_INLINE void sgr_fsincos(sg_ctx *ctx) {
    double v = SGR_ST(0);
    if (sgr_ftrig_ok(ctx, v)) { SGR_ST(0) = sin(v); sgr_fpush(ctx, cos(v)); }
}
SG_INLINE void sgr_fptan(sg_ctx *ctx) { if (sgr_ftrig_ok(ctx, SGR_ST(0))) { SGR_ST(0) = tan(SGR_ST(0)); sgr_fpush(ctx, 1.0); } }
SG_INLINE void sgr_fpatan(sg_ctx *ctx) { double y = SGR_ST(1), x = SGR_ST(0); sgr_fpop(ctx); SGR_ST(0) = atan2(y, x); }
SG_INLINE void sgr_fyl2x(sg_ctx *ctx) { double y = SGR_ST(1), x = SGR_ST(0); sgr_fpop(ctx); SGR_ST(0) = y * log2(x); }
SG_INLINE void sgr_f2xm1(sg_ctx *ctx) { SGR_ST(0) = exp2(SGR_ST(0)) - 1.0; }
SG_INLINE void sgr_fscale(sg_ctx *ctx) { SGR_ST(0) = ldexp(SGR_ST(0), (int)fmax(fmin(trunc(SGR_ST(1)), 100000.0), -100000.0)); }

/* x87 constants (rounded to double). */
#define SGR_LG2 0.30102999566398119521  /* log10(2) */
#define SGR_LN2 0.69314718055994530942  /* ln(2)    */
#define SGR_L2E 1.44269504088896340736  /* log2(e)  */
#define SGR_L2T 3.32192809488736234787  /* log2(10) */
#define SGR_PI 3.14159265358979323846

/* ------------------------------------------------------------------------------------------ */
/* SSE                                                                                          */
/* ------------------------------------------------------------------------------------------ */

/* cvtss2si / cvttss2si / cvttsd2si: out of range or NaN gives 0x80000000 ("integer indefinite"). */
SG_INLINE uint32_t sgr_cvt_f2i(double v, int truncate) {
    double r = truncate ? trunc(v) : nearbyint(v); /* MXCSR RC is round-to-nearest [obs, fp-model 6] */
    return (r >= -2147483648.0 && r < 2147483648.0) ? (uint32_t)(int32_t)r : 0x80000000u;
}
/* minss/maxss/minps/maxps: the second operand is returned when either is NaN or both are zero. */
SG_INLINE float sgr_minf(float a, float b) { return a < b ? a : b; }
SG_INLINE float sgr_maxf(float a, float b) { return a > b ? a : b; }
/* cmpps/cmpss predicates 0..7: eq lt le unord neq nlt nle ord. Returns an all-ones mask. */
SG_INLINE uint32_t sgr_fcmpmask(float a, float b, unsigned p) {
    int r;
    switch (p & 7u) {
    case 0: r = a == b; break;
    case 1: r = a < b; break;
    case 2: r = a <= b; break;
    case 3: r = a != a || b != b; break;
    case 4: r = !(a == b); break;
    case 5: r = !(a < b); break;
    case 6: r = !(a <= b); break;
    default: r = !(a != a || b != b); break;
    }
    return r ? 0xffffffffu : 0u;
}
/* rcp/rsqrt: computed exactly. Real CPUs return a 12-bit approximation (Intel and AMD differ),
 * so results differ in the low mantissa bits; no exact emulation is attempted. */
SG_INLINE float sgr_rcpf(float a) { return 1.0f / a; }
SG_INLINE float sgr_rsqrtf(float a) { return 1.0f / sqrtf(a); }

/* [runtime] MXCSR -> host FPCR (FZ from FTZ|DAZ, rounding mode). */
extern void sg_set_mxcsr(sg_ctx *ctx, uint32_t v);
/* [runtime] fxsave: writes a plausible 512-byte image (only the CPU probe at 0x4f8194 uses it). */
extern void sg_fxsave(sg_ctx *ctx, uint8_t *mem, uint32_t a);

/* ------------------------------------------------------------------------------------------ */
/* MMX and SSE2 integer lanes. 64-bit MMX values; xmm forms apply the same helper per half.     */
/* ------------------------------------------------------------------------------------------ */

SG_INLINE int32_t sgr_sat16(int32_t v) { return v > 32767 ? 32767 : v < -32768 ? -32768 : v; }
SG_INLINE int32_t sgr_sat8(int32_t v) { return v > 127 ? 127 : v < -128 ? -128 : v; }
SG_INLINE int32_t sgr_satu8(int32_t v) { return v > 255 ? 255 : v < 0 ? 0 : v; }
SG_INLINE int32_t sgr_satu16(int32_t v) { return v > 65535 ? 65535 : v < 0 ? 0 : v; }

#define SGR_LANES(name, T, N, expr)                                                    \
    SG_INLINE uint64_t name(uint64_t x, uint64_t y) {                                  \
        T a[N], b[N], r[N]; unsigned i;                                                \
        __builtin_memcpy(a, &x, 8); __builtin_memcpy(b, &y, 8);                        \
        for (i = 0; i < N; i++) r[i] = (T)(expr);                                      \
        __builtin_memcpy(&x, r, 8); return x;                                          \
    }
SGR_LANES(sgr_paddb, uint8_t, 8, a[i] + b[i])
SGR_LANES(sgr_paddw, uint16_t, 4, a[i] + b[i])
SGR_LANES(sgr_paddd, uint32_t, 2, a[i] + b[i])
SGR_LANES(sgr_paddq, uint64_t, 1, a[i] + b[i])
SGR_LANES(sgr_psubb, uint8_t, 8, a[i] - b[i])
SGR_LANES(sgr_psubw, uint16_t, 4, a[i] - b[i])
SGR_LANES(sgr_psubd, uint32_t, 2, a[i] - b[i])
SGR_LANES(sgr_psubq, uint64_t, 1, a[i] - b[i])
SGR_LANES(sgr_paddsb, int8_t, 8, sgr_sat8((int32_t)a[i] + b[i]))
SGR_LANES(sgr_paddsw, int16_t, 4, sgr_sat16((int32_t)a[i] + b[i]))
SGR_LANES(sgr_psubsb, int8_t, 8, sgr_sat8((int32_t)a[i] - b[i]))
SGR_LANES(sgr_psubsw, int16_t, 4, sgr_sat16((int32_t)a[i] - b[i]))
SGR_LANES(sgr_paddusb, uint8_t, 8, sgr_satu8((int32_t)a[i] + b[i]))
SGR_LANES(sgr_paddusw, uint16_t, 4, sgr_satu16((int32_t)a[i] + b[i]))
SGR_LANES(sgr_psubusb, uint8_t, 8, sgr_satu8((int32_t)a[i] - b[i]))
SGR_LANES(sgr_psubusw, uint16_t, 4, sgr_satu16((int32_t)a[i] - b[i]))
SGR_LANES(sgr_pmullw, int16_t, 4, (int32_t)a[i] * b[i])
SGR_LANES(sgr_pmulhw, int16_t, 4, ((int32_t)a[i] * b[i]) >> 16)
SGR_LANES(sgr_pmulhuw, uint16_t, 4, ((uint32_t)a[i] * b[i]) >> 16)
SGR_LANES(sgr_pminub, uint8_t, 8, a[i] < b[i] ? a[i] : b[i])
SGR_LANES(sgr_pmaxub, uint8_t, 8, a[i] > b[i] ? a[i] : b[i])
SGR_LANES(sgr_pminsw, int16_t, 4, a[i] < b[i] ? a[i] : b[i])
SGR_LANES(sgr_pmaxsw, int16_t, 4, a[i] > b[i] ? a[i] : b[i])
SGR_LANES(sgr_pavgb, uint8_t, 8, ((uint32_t)a[i] + b[i] + 1u) >> 1)
SGR_LANES(sgr_pavgw, uint16_t, 4, ((uint32_t)a[i] + b[i] + 1u) >> 1)
SGR_LANES(sgr_pcmpeqb, uint8_t, 8, a[i] == b[i] ? 0xffu : 0u)
SGR_LANES(sgr_pcmpeqw, uint16_t, 4, a[i] == b[i] ? 0xffffu : 0u)
SGR_LANES(sgr_pcmpeqd, uint32_t, 2, a[i] == b[i] ? 0xffffffffu : 0u)
SGR_LANES(sgr_pcmpgtb, int8_t, 8, a[i] > b[i] ? -1 : 0)
SGR_LANES(sgr_pcmpgtw, int16_t, 4, a[i] > b[i] ? -1 : 0)
SGR_LANES(sgr_pcmpgtd, int32_t, 2, a[i] > b[i] ? -1 : 0)
SG_INLINE uint64_t sgr_pand(uint64_t a, uint64_t b) { return a & b; }
SG_INLINE uint64_t sgr_pandn(uint64_t a, uint64_t b) { return ~a & b; }
SG_INLINE uint64_t sgr_por(uint64_t a, uint64_t b) { return a | b; }
SG_INLINE uint64_t sgr_pxor(uint64_t a, uint64_t b) { return a ^ b; }
SG_INLINE uint64_t sgr_pmaddwd(uint64_t x, uint64_t y) {
    int16_t a[4], b[4]; uint32_t r[2];
    __builtin_memcpy(a, &x, 8); __builtin_memcpy(b, &y, 8);
    r[0] = (uint32_t)((int32_t)a[0] * b[0] + (int32_t)a[1] * b[1]);
    r[1] = (uint32_t)((int32_t)a[2] * b[2] + (int32_t)a[3] * b[3]);
    __builtin_memcpy(&x, r, 8); return x;
}
/* Shifts by a count (immediate or register); counts above the lane width zero (or sign-fill). */
#define SGR_PSHIFT(name, T, N, W, expr)                                                \
    SG_INLINE uint64_t name(uint64_t x, uint64_t cnt) {                                \
        T a[N]; unsigned i; uint64_t c = cnt;                                          \
        __builtin_memcpy(a, &x, 8);                                                    \
        for (i = 0; i < N; i++) a[i] = (T)(expr);                                      \
        __builtin_memcpy(&x, a, 8); return x;                                          \
    }
SGR_PSHIFT(sgr_psllw, uint16_t, 4, 16, c > 15 ? 0 : a[i] << c)
SGR_PSHIFT(sgr_pslld, uint32_t, 2, 32, c > 31 ? 0 : a[i] << c)
SGR_PSHIFT(sgr_psllq, uint64_t, 1, 64, c > 63 ? 0 : a[i] << c)
SGR_PSHIFT(sgr_psrlw, uint16_t, 4, 16, c > 15 ? 0 : a[i] >> c)
SGR_PSHIFT(sgr_psrld, uint32_t, 2, 32, c > 31 ? 0 : a[i] >> c)
SGR_PSHIFT(sgr_psrlq, uint64_t, 1, 64, c > 63 ? 0 : a[i] >> c)
SGR_PSHIFT(sgr_psraw, int16_t, 4, 16, a[i] >> (c > 15 ? 15 : c))
SGR_PSHIFT(sgr_psrad, int32_t, 2, 32, a[i] >> (c > 31 ? 31 : c))

/* Packs and unpacks work on two 64-bit halves (lo = destination, hi = source for MMX). */
SG_INLINE uint64_t sgr_packsswb(uint64_t x, uint64_t y) {
    int16_t a[8]; int8_t r[8]; unsigned i; __builtin_memcpy(a, &x, 8); __builtin_memcpy(a + 4, &y, 8);
    for (i = 0; i < 8; i++) r[i] = (int8_t)sgr_sat8(a[i]);
    __builtin_memcpy(&x, r, 8); return x;
}
SG_INLINE uint64_t sgr_packuswb(uint64_t x, uint64_t y) {
    int16_t a[8]; uint8_t r[8]; unsigned i; __builtin_memcpy(a, &x, 8); __builtin_memcpy(a + 4, &y, 8);
    for (i = 0; i < 8; i++) r[i] = (uint8_t)sgr_satu8(a[i]);
    __builtin_memcpy(&x, r, 8); return x;
}
SG_INLINE uint64_t sgr_packssdw(uint64_t x, uint64_t y) {
    int32_t a[4]; int16_t r[4]; unsigned i; __builtin_memcpy(a, &x, 8); __builtin_memcpy(a + 2, &y, 8);
    for (i = 0; i < 4; i++) r[i] = (int16_t)sgr_sat16(a[i]);
    __builtin_memcpy(&x, r, 8); return x;
}
/* Interleave the low (or high) halves of two 64-bit values: element size e bytes. */
SG_INLINE uint64_t sgr_unpck(uint64_t x, uint64_t y, unsigned e, int high) {
    uint8_t a[8], b[8], r[8]; unsigned i, j, n = 4u / e; /* elements taken from each */
    __builtin_memcpy(a, &x, 8); __builtin_memcpy(b, &y, 8);
    for (i = 0; i < n; i++)
        for (j = 0; j < e; j++) {
            r[2 * i * e + j] = a[(high ? 4u : 0u) + i * e + j];
            r[2 * i * e + e + j] = b[(high ? 4u : 0u) + i * e + j];
        }
    __builtin_memcpy(&x, r, 8); return x;
}
SG_INLINE uint64_t sgr_pshufw(uint64_t x, unsigned imm) {
    uint16_t a[4], r[4]; unsigned i; __builtin_memcpy(a, &x, 8);
    for (i = 0; i < 4; i++) r[i] = a[(imm >> (2 * i)) & 3u];
    __builtin_memcpy(&x, r, 8); return x;
}

/* xmm-wide wrappers: apply a 64-bit lane op to both halves. */
#define SGR_X2(fn, d, s) do { sg_xmm s_ = (s); (d).u64[0] = fn((d).u64[0], s_.u64[0]); \
        (d).u64[1] = fn((d).u64[1], s_.u64[1]); } while (0)

/* 128-bit packs/unpacks on xmm. */
SG_INLINE sg_xmm sgr_xpack(sg_xmm d, sg_xmm s, uint64_t (*fn)(uint64_t, uint64_t)) {
    sg_xmm r; r.u64[0] = fn(d.u64[0], d.u64[1]); r.u64[1] = fn(s.u64[0], s.u64[1]); return r;
}
SG_INLINE sg_xmm sgr_xunpck(sg_xmm d, sg_xmm s, unsigned e, int high) {
    sg_xmm r; uint64_t a = d.u64[high ? 1 : 0], b = s.u64[high ? 1 : 0];
    if (e == 8) { r.u64[0] = a; r.u64[1] = b; return r; }
    r.u64[0] = sgr_unpck(a, b, e, 0); r.u64[1] = sgr_unpck(a, b, e, 1); return r;
}
SG_INLINE sg_xmm sgr_pshufd(sg_xmm s, unsigned imm) {
    sg_xmm r; unsigned i; for (i = 0; i < 4; i++) r.u32[i] = s.u32[(imm >> (2 * i)) & 3u]; return r;
}
SG_INLINE sg_xmm sgr_shufps(sg_xmm d, sg_xmm s, unsigned imm) {
    sg_xmm r;
    r.u32[0] = d.u32[imm & 3u]; r.u32[1] = d.u32[(imm >> 2) & 3u];
    r.u32[2] = s.u32[(imm >> 4) & 3u]; r.u32[3] = s.u32[(imm >> 6) & 3u];
    return r;
}
SG_INLINE sg_xmm sgr_psrldq(sg_xmm s, unsigned n) {
    sg_xmm r; unsigned i; for (i = 0; i < 16; i++) r.u8[i] = (i + n < 16u) ? s.u8[i + n] : 0u; return r;
}
SG_INLINE sg_xmm sgr_pslldq(sg_xmm s, unsigned n) {
    sg_xmm r; unsigned i; for (i = 0; i < 16; i++) r.u8[i] = (i >= n) ? s.u8[i - n] : 0u; return r;
}
SG_INLINE uint32_t sgr_movmskps(sg_xmm s) {
    return (s.u32[0] >> 31) | ((s.u32[1] >> 31) << 1) | ((s.u32[2] >> 31) << 2) | ((s.u32[3] >> 31) << 3);
}
SG_INLINE uint32_t sgr_pmovmskb64(uint64_t x) {
    uint32_t r = 0; unsigned i; for (i = 0; i < 8; i++) r |= (uint32_t)((x >> (8 * i + 7)) & 1u) << i; return r;
}
SG_INLINE sg_xmm sgr_xmm_zero(void) { sg_xmm r; r.u64[0] = r.u64[1] = 0; return r; }
SG_INLINE sg_xmm sgr_ldx(const uint8_t *mem, uint32_t a) { sg_xmm r; sg_ld128(mem, a, &r); return r; }
SG_INLINE sg_xmm sgr_ldx32(const uint8_t *mem, uint32_t a) { sg_xmm r = sgr_xmm_zero(); r.u32[0] = sg_ld32(mem, a); return r; }
SG_INLINE sg_xmm sgr_ldx64(const uint8_t *mem, uint32_t a) { sg_xmm r = sgr_xmm_zero(); r.u64[0] = sg_ld64(mem, a); return r; }
SG_INLINE sg_xmm sgr_ldx_acq(const uint8_t *mem, uint32_t a) { sg_xmm r; sg_ld128_acq_u(mem, a, &r); return r; }
SG_INLINE sg_xmm sgr_ldx32_acq(const uint8_t *mem, uint32_t a) { sg_xmm r = sgr_xmm_zero(); r.u32[0] = sg_ld32_acq_u(mem, a); return r; }
SG_INLINE sg_xmm sgr_ldx64_acq(const uint8_t *mem, uint32_t a) { sg_xmm r = sgr_xmm_zero(); r.u64[0] = sg_ld64_acq_u(mem, a); return r; }
SG_INLINE sg_xmm sgr_x64(uint64_t v) { sg_xmm r; r.u64[0] = v; r.u64[1] = 0; return r; }

/* cvtpi2ps / cvtps2pi / cvttps2dq */
SG_INLINE uint64_t sgr_cvtps2pi(sg_xmm s, int truncate) {
    return (uint64_t)sgr_cvt_f2i(s.f32[0], truncate) | ((uint64_t)sgr_cvt_f2i(s.f32[1], truncate) << 32);
}

#endif /* SG_RECOMP_H */
