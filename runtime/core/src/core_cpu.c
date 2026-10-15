/* core_cpu.c -- CPU intrinsics the translator calls (runtime-abi 4.6, translator.md 6.2):
 * cpuid, rdtsc, 80-bit x87 memory operands, ldmxcsr, fxsave. */
#include <math.h>
#include <time.h>
#if defined(__APPLE__)
#include <mach/mach_time.h>
#endif

#include "core_internal.h"

/* A fixed SSE2-class CPU (the game's cpuid results select MMX/SSE/SSE2 paths). No SSE3, one logical processor per package; the job-thread count comes from
 * GetSystemInfo, not cpuid. */
#define CPUID_EDX_1 ((1u << 0) | (1u << 4) | (1u << 5) | (1u << 8) | (1u << 15) | (1u << 19) | (1u << 23) | \
                     (1u << 24) | (1u << 25) | (1u << 26))
static const char brand[48] = "spore-native SSE2 CPU @ 3.00GHz";

void sg_cpuid(sg_ctx *ctx) {
    uint32_t leaf = ctx->eax;
    ctx->eax = ctx->ebx = ctx->ecx = ctx->edx = 0;
    switch (leaf) {
    case 0:
        ctx->eax = 1;
        ctx->ebx = 0x756e6547u; /* "Genu" */
        ctx->edx = 0x49656e69u; /* "ineI" */
        ctx->ecx = 0x6c65746eu; /* "ntel" */
        break;
    case 1:
        ctx->eax = 0x000006fbu; /* family 6, model 15, stepping 11 */
        ctx->ebx = 0x00010800u; /* clflush 8 qwords, 1 logical processor */
        ctx->ecx = 0;
        ctx->edx = CPUID_EDX_1;
        break;
    case 0x80000000u:
        ctx->eax = 0x80000004u;
        break;
    case 0x80000002u: case 0x80000003u: case 0x80000004u: {
        const uint8_t *b = (const uint8_t *)brand + 16u * (leaf - 0x80000002u);
        memcpy(&ctx->eax, b, 4);
        memcpy(&ctx->ebx, b + 4, 4);
        memcpy(&ctx->ecx, b + 8, 4);
        memcpy(&ctx->edx, b + 12, 4);
        break;
    }
    default: break;
    }
}

/* Time-stamp counter at a nominal 3 GHz from the host monotonic clock. */
void sg_rdtsc(sg_ctx *ctx) {
    uint64_t ns;
#if defined(__APPLE__)
    static mach_timebase_info_data_t tb;
    if (!tb.denom) mach_timebase_info(&tb);
    ns = mach_absolute_time() * tb.numer / tb.denom;
#else
    struct timespec ts;
    clock_gettime(CLOCK_MONOTONIC, &ts);
    ns = (uint64_t)ts.tv_sec * 1000000000ull + (uint64_t)ts.tv_nsec;
#endif
    ns *= 3u;
    ctx->eax = (uint32_t)ns;
    ctx->edx = (uint32_t)(ns >> 32);
}

/* 80-bit extended <-> host double (runtime-abi.md 4.4: x87 values are host doubles). */
sg_f80 sg_ldf80(const uint8_t *mem, uint32_t a) {
#ifdef SG_X87_F80
    sg_f80 v;
    memset(&v, 0, sizeof v);
    v.mant = sg_ld64(mem, a);
    v.sexp = sg_ld16(mem, a + 8);
    return v;
#else
    uint64_t m = sg_ld64(mem, a);
    uint16_t se = sg_ld16(mem, a + 8);
    int e = se & 0x7fff;
    double v;
    if (e == 0x7fff) v = (m << 1) ? __builtin_nan("") : __builtin_inf();
    else if (e == 0 && m == 0) v = 0.0;
    else v = ldexp((double)m, (e ? e : 1) - 16383 - 63); /* rounds to double (nearest) */
    return (se & 0x8000) ? -v : v;
#endif
}

void sg_stf80(uint8_t *mem, uint32_t a, sg_f80 v) {
#ifdef SG_X87_F80
    sg_st64(mem, a, v.mant);
    sg_st16(mem, a + 8, v.sexp);
#else
    uint64_t m = 0;
    uint16_t se = 0;
    if (isnan(v)) { m = 0xc000000000000000ull; se = 0x7fff; }
    else if (isinf(v)) { m = 0x8000000000000000ull; se = 0x7fff; }
    else if (v != 0.0) {
        int e;
        double f = frexp(fabs(v), &e); /* |v| = f * 2^e, 0.5 <= f < 1 */
        m = (uint64_t)ldexp(f, 64);    /* exact: a double has 53 significant bits */
        se = (uint16_t)(e - 1 + 16383);
    }
    if (signbit(v)) se |= 0x8000;
    sg_st64(mem, a, m);
    sg_st16(mem, a + 8, se);
#endif
}

/* ldmxcsr (runtime-abi.md 4.5): FTZ or DAZ -> FPCR.FZ; RC -> FPCR.RMode. Exceptions stay masked. */
void sg_set_mxcsr(sg_ctx *ctx, uint32_t v) {
    ctx->mxcsr = v;
#if defined(__aarch64__)
    {
        uint64_t fpcr;
        static const uint64_t rmode[4] = {0u, 2u, 1u, 3u}; /* x86 RC: near, down, up, zero -> RN, RM, RP, RZ */
        __asm__ __volatile__("mrs %0, fpcr" : "=r"(fpcr));
        fpcr &= ~((1ull << 24) | (3ull << 22));
        if (v & 0x8040u) fpcr |= 1ull << 24;
        fpcr |= rmode[(v >> 13) & 3u] << 22;
        __asm__ __volatile__("msr fpcr, %0" : : "r"(fpcr));
    }
#endif
}

/* fxsave m512: FCW, FSW (with TOP), abridged FTW, MXCSR, MXCSR_MASK, ST0-7, XMM0-7. */
void sg_fxsave(sg_ctx *ctx, uint8_t *mem, uint32_t a) {
    unsigned i;
    uint8_t ftw = 0;
    memset(mem + a, 0, 512);
    sg_st16(mem, a + 0, ctx->fpu.cw);
    sg_st16(mem, a + 2, (uint16_t)((ctx->fpu.sw & ~0x3800u) | ((ctx->fpu.top & 7u) << 11)));
    for (i = 0; i < 8; i++)
        if (((ctx->fpu.tag >> (2 * i)) & 3u) != 3u) ftw |= (uint8_t)(1u << i);
    mem[a + 4] = ftw;
    sg_st32(mem, a + 24, ctx->mxcsr);
    sg_st32(mem, a + 28, 0xffffu); /* MXCSR_MASK: DAZ supported */
    for (i = 0; i < 8; i++) sg_stf80(mem, a + 32 + 16 * i, SG_ST(ctx, i));
    for (i = 0; i < 8; i++) sg_st128(mem, a + 160 + 16 * i, &ctx->xmm[i]);
}
