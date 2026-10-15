/*
 * spore_guest.h -- the contract between translated guest code, the runtime and the HLE shims.
 *
 * Spec: docs/design/runtime-abi.md (read that first; section numbers below refer to it).
 * Version: SG_ABI_VERSION below (first version 2026-10-12); changes are logged in the doc's change log.
 *
 * Rules for this file:
 *   - Plain C11 (clang/gcc). No game code, no game bytes. Host is little-endian, 64-bit
 *     (arm64 macOS is the target; x86-64 Linux works for host-only tests).
 *   - Everything the generated C needs is here, as macros or static inline functions.
 *     Things the runtime implements are declared `extern` and marked [runtime].
 *   - Guest addresses are always uint32_t. A host pointer to guest memory is only ever
 *     formed as (mem + guest_addr) inside this header's helpers.
 */
#ifndef SPORE_GUEST_H
#define SPORE_GUEST_H

#include <stddef.h>
#include <stdint.h>
#include <string.h>

#ifdef __cplusplus
extern "C" {
#endif

#define SG_ABI_VERSION 1u /* bump on any change to sg_ctx layout or call protocol */

/* ------------------------------------------------------------------------------------------ */
/* 0. Compiler glue                                                                            */
/* ------------------------------------------------------------------------------------------ */

#if defined(__GNUC__) || defined(__clang__)
#define SG_INLINE static inline __attribute__((always_inline))
#define SG_NOINLINE __attribute__((noinline))
#define SG_NORETURN __attribute__((noreturn))
#define SG_LIKELY(x) __builtin_expect(!!(x), 1)
#define SG_UNLIKELY(x) __builtin_expect(!!(x), 0)
#define SG_RESTRICT __restrict__
#define SG_COLD __attribute__((cold))
#else
#error "spore_guest.h needs clang or gcc"
#endif

#if defined(__clang__) && __has_attribute(musttail)
#define SG_MUSTTAIL __attribute__((musttail))
#else
#define SG_MUSTTAIL /* gcc: rely on -O2 sibling-call optimisation */
#endif

#ifdef __cplusplus
#define SG_STATIC_ASSERT(c, m) static_assert(c, m)
#define SG_THREAD_LOCAL thread_local /* hand-written C++ overrides include this header too */
#else
#define SG_STATIC_ASSERT(c, m) _Static_assert(c, m)
#define SG_THREAD_LOCAL _Thread_local
#endif

/* ------------------------------------------------------------------------------------------ */
/* 1. Guest address space (doc section 3)                                                      */
/* ------------------------------------------------------------------------------------------ */

/* The window: 4 GiB of host virtual address space reserved at a host base `mem`, plus a
 * guard tail so a multi-byte access at 0xFFFFFFFx cannot leave the reservation. */
#define SG_WINDOW_SIZE 0x100000000ull
#define SG_WINDOW_GUARD 0x10000ull /* bytes reserved (PROT_NONE) past 4 GiB */

/* Fixed regions [obs: image facts read from the exe's PE headers; others are our choice]. */
#define SG_NULL_GUARD_END 0x00010000u /* [0, 64K) never mapped: null derefs fault */
#define SG_IMAGE_BASE 0x00400000u     /* PE preferred base; image is never relocated */
#define SG_IMAGE_SIZE 0x0190e000u     /* SizeOfImage -> image ends at 0x01d0e000 */
#define SG_TEXT_START 0x00401000u
#define SG_TEXT_END 0x013cbf2au       /* .text virtual end (exclusive) */
#define SG_IAT_START 0x013cc000u      /* IAT, 0x80c bytes, filled with thunk addresses at load */
#define SG_IAT_END 0x013cc80cu
#define SG_USER_LIMIT 0x80000000u     /* exe is not LARGE_ADDRESS_AWARE: all guest allocations < 2 GiB */
#define SG_HOST_PAGE 0x4000u          /* arm64 macOS page: the unit of every host mprotect (image-linker.md 9.1) */
#define SG_IMAGE_MAPPED_END 0x01d10000u /* image end rounded up to a host page: the guest heap starts here */

/* Runtime-owned regions at the top of the 2 GiB user space (doc 3.2). */
#define SG_THUNK_BASE 0x7fe00000u  /* host thunks: one 16-byte slot per host entry point */
#define SG_THUNK_SLOT 16u
#define SG_THUNK_LIMIT 0x7ff00000u /* 65,536 slots */
#define SG_TEB_REGION 0x7ff00000u  /* per-thread TEBs (4 KiB each) + the PEB */
#define SG_TEB_REGION_END 0x7ffe0000u
#define SG_PEB_ADDR 0x7ffdf000u    /* one fake PEB (zero-filled, a few fields set) */

/* Well-known thunk slots (slot index * 16 + SG_THUNK_BASE). */
#define SG_THUNK_ADDR(slot) (SG_THUNK_BASE + (uint32_t)(slot) * SG_THUNK_SLOT)
#define SG_RET_TO_HOST SG_THUNK_ADDR(0)   /* return address pushed by sg_call_guest */
#define SG_THREAD_EXIT SG_THUNK_ADDR(1)   /* return address at the base of a new thread's stack */
#define SG_THUNK_UNIMPL SG_THUNK_ADDR(2)  /* IAT value for imports we never resolve */
#define SG_THUNK_FIRST_FREE 16u           /* slots 0..15 reserved for the runtime */

/* ------------------------------------------------------------------------------------------ */
/* 2. CPU context (doc section 4)                                                              */
/* ------------------------------------------------------------------------------------------ */

/* x86 register numbers, as in ModRM encoding. */
enum { SG_EAX = 0, SG_ECX, SG_EDX, SG_EBX, SG_ESP, SG_EBP, SG_ESI, SG_EDI };

/* Lazy arithmetic flags (doc 4.2). `op` says how CF/PF/AF/ZF/SF/OF are derived from
 * res/dst/src; SG_FOP_EFLAGS means `res` holds them already materialised (EFLAGS bit layout). */
typedef struct sg_lazyflags {
    uint32_t op;  /* SG_FOP_* | (size << SG_FOP_SIZE_SHIFT) | (carry_in << SG_FOP_CIN_SHIFT) */
    uint32_t res; /* result, zero-extended to 32 bits (or materialised flags) */
    uint32_t dst; /* first operand before the operation */
    uint32_t src; /* second operand / shift count / multiplier high half (see 4.2) */
} sg_lazyflags;

/* x87 floating-point unit (doc 4.4). Registers are indexed physically like the real FPU:
 * ST(i) is st[(top + i) & 7]. Values are host doubles unless SG_X87_F80 is defined
 * (only where code needs real 80-bit values). */
#ifdef SG_X87_F80
typedef struct { uint64_t mant; uint16_t sexp; uint16_t pad[3]; } sg_f80; /* soft-float */
#else
typedef double sg_f80;
#endif

typedef struct sg_x87 {
    sg_f80 st[8];
    uint32_t top; /* 0..7; ST(0) = st[top] */
    uint16_t cw;  /* control word; reset value 0x027f; precision/rounding: doc 4.4 */
    uint16_t sw;  /* status word WITHOUT the TOP field (bits 11..13 are synthesised from `top`) */
    uint16_t tag; /* 2 bits per physical register: 00 valid, 01 zero, 10 special, 11 empty */
    uint16_t pad0;
    uint32_t pad1;
} sg_x87;

typedef union sg_xmm {
    float f32[4];
    uint32_t u32[4];
    uint64_t u64[2];
    double f64[2];
    uint8_t u8[16];
} sg_xmm;

struct sg_thread; /* opaque to generated code; see section 9 */

typedef struct sg_ctx {
    /* --- integer state: keep first, hot ---------------------------------------- */
    union {
        uint32_t r[8]; /* indexed by SG_EAX..SG_EDI */
        struct { uint32_t eax, ecx, edx, ebx, esp, ebp, esi, edi; };
    };
    sg_lazyflags fl;   /* CF PF AF ZF SF OF */
    uint32_t eip;      /* written by SG_RET (popped address) and by the runtime; see 5.3 */
    uint32_t fs_base;  /* guest address of this thread's TEB */
    uint32_t sysfl;    /* non-arithmetic EFLAGS bits that pushfd/popfd round-trip: DF (always 0
                        * [obs: no std/cld in .text]), AC and ID (CPUID probe at 0x1168750) */
    uint32_t pad0;
    /* --- SSE ------------------------------------------------------------------- */
    sg_xmm xmm[8];
    uint32_t mxcsr;    /* reset value 0x1f80 */
    uint32_t pad1;
    /* --- x87 / MMX ---------------------------------------------------------- */
    sg_x87 fpu;
    uint64_t mm[8];    /* MMX: NOT aliased onto st[] (doc 4.5) */
    /* --- host-side links (not guest-visible) ----------------------------------- */
    uint8_t *mem;               /* window base, same value as the `mem` argument */
    struct sg_thread *thread;   /* owning thread */
    void *eh_top;               /* innermost sg_landing on this thread (section 10) */
    uint64_t scratch[4];        /* for runtime helpers (e.g. 64-bit muldiv temporaries) */
} sg_ctx;

SG_STATIC_ASSERT(offsetof(sg_ctx, eax) == 0, "eax at 0");
SG_STATIC_ASSERT(offsetof(sg_ctx, esp) == 16, "esp at 16");
SG_STATIC_ASSERT(offsetof(sg_ctx, fl) == 32, "flags at 32");
SG_STATIC_ASSERT(offsetof(sg_ctx, eip) == 48, "eip at 48");
SG_STATIC_ASSERT(offsetof(sg_ctx, xmm) == 64, "xmm at 64, 16-aligned");
SG_STATIC_ASSERT(offsetof(sg_ctx, fpu) == 200, "fpu at 200");

/* Partial-register access. 8-bit high registers (ah, ch, dh, bh) are bits 8..15 of r[0..3]. */
#define SG_R8L(ctx, n) ((uint8_t)((ctx)->r[(n)]))
#define SG_R8H(ctx, n) ((uint8_t)((ctx)->r[(n)] >> 8))
#define SG_R16(ctx, n) ((uint16_t)((ctx)->r[(n)]))
#define SG_SET_R8L(ctx, n, v) ((ctx)->r[(n)] = ((ctx)->r[(n)] & 0xffffff00u) | (uint8_t)(v))
#define SG_SET_R8H(ctx, n, v) ((ctx)->r[(n)] = ((ctx)->r[(n)] & 0xffff00ffu) | ((uint32_t)(uint8_t)(v) << 8))
#define SG_SET_R16(ctx, n, v) ((ctx)->r[(n)] = ((ctx)->r[(n)] & 0xffff0000u) | (uint16_t)(v))

/* ------------------------------------------------------------------------------------------ */
/* 3. Lazy flags (doc 4.2)                                                                     */
/* ------------------------------------------------------------------------------------------ */

enum sg_fop {
    SG_FOP_EFLAGS = 0, /* res = materialised EFLAGS (only CF PF AF ZF SF OF bits are meaningful) */
    SG_FOP_ADD,        /* res = dst + src (+ cin for ADC) */
    SG_FOP_SUB,        /* res = dst - src (- cin for SBB); also CMP, NEG (dst = 0) */
    SG_FOP_LOGIC,      /* AND/OR/XOR/TEST: CF = OF = 0, AF = 0 */
    SG_FOP_INC,        /* res = dst + 1; CF preserved: old CF kept in the CIN bit */
    SG_FOP_DEC,        /* res = dst - 1; CF preserved: old CF kept in the CIN bit */
    SG_FOP_SHL,        /* res = dst << src, src = masked count (1..31) */
    SG_FOP_SHR,        /* res = dst >> src (logical) */
    SG_FOP_SAR,        /* res = dst >> src (arithmetic) */
    SG_FOP_MUL,        /* MUL/IMUL: CF = OF = (src != 0); src = 1 if high half is significant */
};
#define SG_FOP_KIND_MASK 0xffu
#define SG_FOP_SIZE_SHIFT 8   /* size code: 0 = 8-bit, 1 = 16-bit, 2 = 32-bit */
#define SG_FOP_CIN_SHIFT 16
#define SG_FOP(kind, size) ((uint32_t)(kind) | ((uint32_t)(size) << SG_FOP_SIZE_SHIFT))
#define SG_FOP_CIN (1u << SG_FOP_CIN_SHIFT)

/* EFLAGS bit positions. */
#define SG_CF 0x0001u
#define SG_PF 0x0004u
#define SG_AF 0x0010u
#define SG_ZF 0x0040u
#define SG_SF 0x0080u
#define SG_DF 0x0400u
#define SG_OF 0x0800u
#define SG_AC 0x00040000u
#define SG_ID 0x00200000u
#define SG_ARITH_FLAGS (SG_CF | SG_PF | SG_AF | SG_ZF | SG_SF | SG_OF)
#define SG_SYS_FLAGS (SG_DF | SG_AC | SG_ID)

/* Record helpers used by the translator when flags are live beyond the defining block. */
SG_INLINE void sg_setfl(sg_ctx *ctx, uint32_t op, uint32_t res, uint32_t dst, uint32_t src) {
    ctx->fl.op = op; ctx->fl.res = res; ctx->fl.dst = dst; ctx->fl.src = src;
}

SG_INLINE uint32_t sg_fl_bits(uint32_t op) { return 8u << ((op >> SG_FOP_SIZE_SHIFT) & 3u); }
SG_INLINE uint32_t sg_fl_mask(uint32_t op) {
    uint32_t b = sg_fl_bits(op);
    return b == 32 ? 0xffffffffu : ((1u << b) - 1u);
}
SG_INLINE uint32_t sg_fl_sign(uint32_t op) { return 1u << (sg_fl_bits(op) - 1u); }

SG_INLINE int sg_flag_cf(const sg_ctx *ctx) {
    const sg_lazyflags *f = &ctx->fl;
    uint32_t m = sg_fl_mask(f->op), cin = (f->op >> SG_FOP_CIN_SHIFT) & 1u;
    switch (f->op & SG_FOP_KIND_MASK) {
    case SG_FOP_EFLAGS: return (int)(f->res & SG_CF);
    case SG_FOP_ADD: { uint32_t r = f->res & m, d = f->dst & m; return cin ? r <= d : r < d; }
    case SG_FOP_SUB: { uint32_t d = f->dst & m, s = f->src & m; return cin ? d <= s : d < s; }
    case SG_FOP_LOGIC: return 0;
    case SG_FOP_INC: case SG_FOP_DEC: return (int)cin;
    case SG_FOP_SHL: /* count > operand size: undefined on hardware; we return 0 */
        return f->src <= sg_fl_bits(f->op) ? (int)(((uint64_t)f->dst >> (sg_fl_bits(f->op) - f->src)) & 1u) : 0;
    case SG_FOP_SHR: return (int)(((f->dst & m) >> ((f->src - 1u) & 31u)) & 1u);
    case SG_FOP_SAR: {
        uint32_t b = sg_fl_bits(f->op);
        int32_t sd = (int32_t)(f->dst << (32u - b)) >> (32u - b); /* sign-extend to 32 */
        uint32_t c = f->src - 1u; if (c > 31u) c = 31u;
        return (int)(((uint32_t)(sd >> c)) & 1u);
    }
    case SG_FOP_MUL: return f->src != 0;
    }
    return 0;
}

SG_INLINE int sg_flag_zf(const sg_ctx *ctx) {
    const sg_lazyflags *f = &ctx->fl;
    if ((f->op & SG_FOP_KIND_MASK) == SG_FOP_EFLAGS) return (f->res & SG_ZF) != 0;
    return (f->res & sg_fl_mask(f->op)) == 0;
}

SG_INLINE int sg_flag_sf(const sg_ctx *ctx) {
    const sg_lazyflags *f = &ctx->fl;
    if ((f->op & SG_FOP_KIND_MASK) == SG_FOP_EFLAGS) return (f->res & SG_SF) != 0;
    return (f->res & sg_fl_sign(f->op)) != 0;
}

SG_INLINE int sg_parity8(uint32_t v) { return !__builtin_parity(v & 0xffu); } /* PF=1 when even */

SG_INLINE int sg_flag_pf(const sg_ctx *ctx) {
    const sg_lazyflags *f = &ctx->fl;
    if ((f->op & SG_FOP_KIND_MASK) == SG_FOP_EFLAGS) return (f->res & SG_PF) != 0;
    return sg_parity8(f->res);
}

SG_INLINE int sg_flag_af(const sg_ctx *ctx) {
    const sg_lazyflags *f = &ctx->fl;
    switch (f->op & SG_FOP_KIND_MASK) {
    case SG_FOP_EFLAGS: return (f->res & SG_AF) != 0;
    case SG_FOP_ADD: case SG_FOP_SUB: return ((f->dst ^ f->src ^ f->res) >> 4) & 1u;
    case SG_FOP_INC: return (f->res & 0xfu) == 0;
    case SG_FOP_DEC: return (f->res & 0xfu) == 0xfu;
    default: return 0; /* LOGIC: 0; shifts/MUL: undefined on hardware, we return 0 */
    }
}

SG_INLINE int sg_flag_of(const sg_ctx *ctx) {
    const sg_lazyflags *f = &ctx->fl;
    uint32_t sb = sg_fl_sign(f->op), m = sg_fl_mask(f->op);
    switch (f->op & SG_FOP_KIND_MASK) {
    case SG_FOP_EFLAGS: return (f->res & SG_OF) != 0;
    case SG_FOP_ADD: return ((~(f->dst ^ f->src) & (f->dst ^ f->res)) & sb) != 0;
    case SG_FOP_SUB: return (((f->dst ^ f->src) & (f->dst ^ f->res)) & sb) != 0;
    case SG_FOP_LOGIC: return 0;
    case SG_FOP_INC: return (f->res & m) == sb;
    case SG_FOP_DEC: return (f->res & m) == sb - 1u;
    case SG_FOP_SHL: /* defined for count 1; hardware-observed formula used for all counts */
        return (((f->res & sb) != 0) ^ sg_flag_cf(ctx)) != 0;
    case SG_FOP_SHR: return f->src == 1u ? (f->dst & sb) != 0 : 0;
    case SG_FOP_SAR: return 0;
    case SG_FOP_MUL: return f->src != 0;
    }
    return 0;
}

/* Condition codes, numbered as in Jcc/SETcc/CMOVcc (low nibble of the opcode). */
SG_INLINE int sg_cond(const sg_ctx *ctx, unsigned cc) {
    int r;
    switch (cc >> 1) {
    case 0: r = sg_flag_of(ctx); break;                                   /* O  */
    case 1: r = sg_flag_cf(ctx); break;                                   /* B  */
    case 2: r = sg_flag_zf(ctx); break;                                   /* E  */
    case 3: r = sg_flag_cf(ctx) || sg_flag_zf(ctx); break;                /* BE */
    case 4: r = sg_flag_sf(ctx); break;                                   /* S  */
    case 5: r = sg_flag_pf(ctx); break;                                   /* P  */
    case 6: r = sg_flag_sf(ctx) != sg_flag_of(ctx); break;                /* L  */
    default: r = sg_flag_zf(ctx) || (sg_flag_sf(ctx) != sg_flag_of(ctx)); /* LE */
    }
    return (cc & 1u) ? !r : r;
}

/* Materialise (pushfd, lahf across blocks) and load (popfd, sahf). Bit 1 is always set. */
SG_INLINE uint32_t sg_get_eflags(const sg_ctx *ctx) {
    return 0x2u | (sg_flag_cf(ctx) ? SG_CF : 0) | (sg_flag_pf(ctx) ? SG_PF : 0) |
           (sg_flag_af(ctx) ? SG_AF : 0) | (sg_flag_zf(ctx) ? SG_ZF : 0) |
           (sg_flag_sf(ctx) ? SG_SF : 0) | (sg_flag_of(ctx) ? SG_OF : 0) | ctx->sysfl |
           0x0200u /* IF */;
}
SG_INLINE void sg_set_eflags(sg_ctx *ctx, uint32_t v) {
    ctx->fl.op = SG_FOP_EFLAGS; ctx->fl.res = v & SG_ARITH_FLAGS; ctx->fl.dst = ctx->fl.src = 0;
    ctx->sysfl = v & SG_SYS_FLAGS;
}

/* ------------------------------------------------------------------------------------------ */
/* 4. Guest memory access (doc 3.3)                                                            */
/* ------------------------------------------------------------------------------------------ */

/* All accesses go through these. Unaligned access is legal (arm64 and x86 both allow it for
 * plain loads/stores). 32-bit address arithmetic must already be done in uint32_t. */
#define SG_HOSTPTR(mem, a) ((void *)((mem) + (uint32_t)(a)))

SG_INLINE uint8_t sg_ld8(const uint8_t *mem, uint32_t a) { return mem[a]; }
SG_INLINE uint16_t sg_ld16(const uint8_t *mem, uint32_t a) { uint16_t v; __builtin_memcpy(&v, mem + a, 2); return v; }
SG_INLINE uint32_t sg_ld32(const uint8_t *mem, uint32_t a) { uint32_t v; __builtin_memcpy(&v, mem + a, 4); return v; }
SG_INLINE uint64_t sg_ld64(const uint8_t *mem, uint32_t a) { uint64_t v; __builtin_memcpy(&v, mem + a, 8); return v; }
SG_INLINE float sg_ldf32(const uint8_t *mem, uint32_t a) { float v; __builtin_memcpy(&v, mem + a, 4); return v; }
SG_INLINE double sg_ldf64(const uint8_t *mem, uint32_t a) { double v; __builtin_memcpy(&v, mem + a, 8); return v; }
SG_INLINE void sg_st8(uint8_t *mem, uint32_t a, uint8_t v) { mem[a] = v; }
SG_INLINE void sg_st16(uint8_t *mem, uint32_t a, uint16_t v) { __builtin_memcpy(mem + a, &v, 2); }
SG_INLINE void sg_st32(uint8_t *mem, uint32_t a, uint32_t v) { __builtin_memcpy(mem + a, &v, 4); }
SG_INLINE void sg_st64(uint8_t *mem, uint32_t a, uint64_t v) { __builtin_memcpy(mem + a, &v, 8); }
SG_INLINE void sg_stf32(uint8_t *mem, uint32_t a, float v) { __builtin_memcpy(mem + a, &v, 4); }
SG_INLINE void sg_stf64(uint8_t *mem, uint32_t a, double v) { __builtin_memcpy(mem + a, &v, 8); }
SG_INLINE void sg_ld128(const uint8_t *mem, uint32_t a, sg_xmm *x) { __builtin_memcpy(x, mem + a, 16); }
SG_INLINE void sg_st128(uint8_t *mem, uint32_t a, const sg_xmm *x) { __builtin_memcpy(mem + a, x, 16); }

/* 80-bit x87 memory operands (fld/fstp tbyte): conversion lives in the runtime. [runtime] */
extern sg_f80 sg_ldf80(const uint8_t *mem, uint32_t a);
extern void sg_stf80(uint8_t *mem, uint32_t a, sg_f80 v);

/* Ordered accesses for the barrier policy. The default policy (doc 3.4) is that plain
 * accesses are relaxed and only these are ordered. Natural alignment is required for the
 * atomic forms; a misaligned locked access traps to sg_misaligned_atomic(). */
SG_INLINE uint32_t sg_ld32_acq(const uint8_t *mem, uint32_t a) {
    return __atomic_load_n((const uint32_t *)(const void *)(mem + a), __ATOMIC_ACQUIRE);
}
SG_INLINE void sg_st32_rel(uint8_t *mem, uint32_t a, uint32_t v) {
    __atomic_store_n((uint32_t *)(void *)(mem + a), v, __ATOMIC_RELEASE);
}
/* docs/design/memory-ordering.md: ordered accesses of every width for the translator's
 * memory-ordering policy. Unlike sg_ld32_acq/sg_st32_rel above, these accept any alignment, as
 * plain x86 accesses do. A naturally aligned access is one acquire load (ldapr on Apple
 * Silicon, RCpc) or one release store (stlr). A misaligned one, which ldapr/stlr cannot do, is
 * a plain access through a volatile pointer followed by an acquire fence (load) or preceded by
 * a release fence (store): the "half barrier" FEX uses. 128-bit accesses are never single-copy
 * atomic on x86 either and always take the fence form. Every form is also a compiler barrier
 * for the access itself: it is never merged, hoisted out of a loop or deleted. */
typedef uint16_t __attribute__((aligned(1), may_alias)) sg__u16u;
typedef uint32_t __attribute__((aligned(1), may_alias)) sg__u32u;
typedef uint64_t __attribute__((aligned(1), may_alias)) sg__u64u;
SG_INLINE void sg_fence_acq(void) { __atomic_thread_fence(__ATOMIC_ACQUIRE); }
SG_INLINE void sg_fence_rel(void) { __atomic_thread_fence(__ATOMIC_RELEASE); }
SG_INLINE uint8_t sg_ld8_acq_u(const uint8_t *mem, uint32_t a) {
    return __atomic_load_n(mem + a, __ATOMIC_ACQUIRE);
}
SG_INLINE uint16_t sg_ld16_acq_u(const uint8_t *mem, uint32_t a) {
    uint16_t v;
    if (SG_LIKELY(!(a & 1u))) return __atomic_load_n((const uint16_t *)(const void *)(mem + a), __ATOMIC_ACQUIRE);
    v = *(const volatile sg__u16u *)(const void *)(mem + a);
    sg_fence_acq();
    return v;
}
SG_INLINE uint32_t sg_ld32_acq_u(const uint8_t *mem, uint32_t a) {
    uint32_t v;
    if (SG_LIKELY(!(a & 3u))) return __atomic_load_n((const uint32_t *)(const void *)(mem + a), __ATOMIC_ACQUIRE);
    v = *(const volatile sg__u32u *)(const void *)(mem + a);
    sg_fence_acq();
    return v;
}
SG_INLINE uint64_t sg_ld64_acq_u(const uint8_t *mem, uint32_t a) {
    uint64_t v;
    if (SG_LIKELY(!(a & 7u))) return __atomic_load_n((const uint64_t *)(const void *)(mem + a), __ATOMIC_ACQUIRE);
    v = *(const volatile sg__u64u *)(const void *)(mem + a);
    sg_fence_acq();
    return v;
}
SG_INLINE float sg_ldf32_acq_u(const uint8_t *mem, uint32_t a) {
    uint32_t u = sg_ld32_acq_u(mem, a); float f; __builtin_memcpy(&f, &u, 4); return f;
}
SG_INLINE double sg_ldf64_acq_u(const uint8_t *mem, uint32_t a) {
    uint64_t u = sg_ld64_acq_u(mem, a); double f; __builtin_memcpy(&f, &u, 8); return f;
}
SG_INLINE void sg_ld128_acq_u(const uint8_t *mem, uint32_t a, sg_xmm *x) {
    const volatile sg__u64u *p = (const volatile sg__u64u *)(const void *)(mem + a);
    uint64_t lo = p[0], hi = p[1];
    __builtin_memcpy((uint8_t *)x, &lo, 8);
    __builtin_memcpy((uint8_t *)x + 8, &hi, 8);
    sg_fence_acq();
}
SG_INLINE void sg_st8_rel_u(uint8_t *mem, uint32_t a, uint8_t v) {
    __atomic_store_n(mem + a, v, __ATOMIC_RELEASE);
}
SG_INLINE void sg_st16_rel_u(uint8_t *mem, uint32_t a, uint16_t v) {
    if (SG_LIKELY(!(a & 1u))) { __atomic_store_n((uint16_t *)(void *)(mem + a), v, __ATOMIC_RELEASE); return; }
    sg_fence_rel();
    *(volatile sg__u16u *)(void *)(mem + a) = v;
}
SG_INLINE void sg_st32_rel_u(uint8_t *mem, uint32_t a, uint32_t v) {
    if (SG_LIKELY(!(a & 3u))) { __atomic_store_n((uint32_t *)(void *)(mem + a), v, __ATOMIC_RELEASE); return; }
    sg_fence_rel();
    *(volatile sg__u32u *)(void *)(mem + a) = v;
}
SG_INLINE void sg_st64_rel_u(uint8_t *mem, uint32_t a, uint64_t v) {
    if (SG_LIKELY(!(a & 7u))) { __atomic_store_n((uint64_t *)(void *)(mem + a), v, __ATOMIC_RELEASE); return; }
    sg_fence_rel();
    *(volatile sg__u64u *)(void *)(mem + a) = v;
}
SG_INLINE void sg_stf32_rel_u(uint8_t *mem, uint32_t a, float v) {
    uint32_t u; __builtin_memcpy(&u, &v, 4); sg_st32_rel_u(mem, a, u);
}
SG_INLINE void sg_stf64_rel_u(uint8_t *mem, uint32_t a, double v) {
    uint64_t u; __builtin_memcpy(&u, &v, 8); sg_st64_rel_u(mem, a, u);
}
SG_INLINE void sg_st128_rel_u(uint8_t *mem, uint32_t a, const sg_xmm *x) {
    volatile sg__u64u *p = (volatile sg__u64u *)(void *)(mem + a);
    uint64_t lo, hi;
    __builtin_memcpy(&lo, (const uint8_t *)x, 8);
    __builtin_memcpy(&hi, (const uint8_t *)x + 8, 8);
    sg_fence_rel();
    p[0] = lo;
    p[1] = hi;
}
extern SG_NORETURN SG_COLD void sg_misaligned_atomic(sg_ctx *ctx, uint32_t a); /* [runtime] */
/* Misaligned locked accesses (seen on the way to the first frame: `lock cmpxchg8b` on a 4-aligned stack slot at
 * 0x68c1eb). x86 performs them as split locks. When the n bytes lie inside one 16-byte block they
 * are done here with a 128-bit compare-and-swap of the whole block (inlined ldaxp/stlxp on arm64),
 * which is atomic against every other locked access to those bytes; an access that crosses a
 * 16-byte boundary uses a process-wide spin lock (below). sg__ma_cas returns 1 on success, else
 * stores the current value in *expected. */
/* An access that crosses a 16-byte boundary (seen: cmpxchg8b at stack offset 0xc, 0x68c130, run 052)
 * takes one process-wide spin lock instead. It is atomic against every other crossing access, which
 * is all the same address can ever be; it is not atomic against an aligned access to an overlapping
 * neighbour, a case x86 code does not rely on [inf]. The lock is a weak definition, so every
 * translation unit shares one. */
__attribute__((weak)) volatile int sg__ma_spin;
SG_COLD static inline __attribute__((noinline)) int sg__ma_cas_locked(uint8_t *mem, uint32_t a, unsigned n,
                                                                       uint64_t *expected, uint64_t v) {
    uint64_t now = 0;
    int ok;
    while (__atomic_exchange_n(&sg__ma_spin, 1, __ATOMIC_ACQUIRE)) { }
    __builtin_memcpy(&now, mem + a, n);
    ok = now == *expected;
    if (ok) __builtin_memcpy(mem + a, &v, n);
    else *expected = now;
    __atomic_store_n(&sg__ma_spin, 0, __ATOMIC_RELEASE);
    __atomic_thread_fence(__ATOMIC_SEQ_CST);
    return ok;
}
SG_COLD static inline __attribute__((noinline)) int sg__ma_cas(sg_ctx *ctx, uint8_t *mem, uint32_t a, unsigned n,
                                                                uint64_t *expected, uint64_t v) {
    uint32_t off = a & 15u;
    unsigned __int128 *blk, cur, nv;
    uint64_t now = 0;
    (void)ctx;
    if (off + n > 16u) return sg__ma_cas_locked(mem, a, n, expected, v);
    blk = (unsigned __int128 *)(void *)(mem + (a - off));
    cur = __atomic_load_n(blk, __ATOMIC_SEQ_CST);
    for (;;) {
        __builtin_memcpy(&now, (const uint8_t *)&cur + off, n);
        if (now != *expected) { *expected = now; return 0; }
        nv = cur;
        __builtin_memcpy((uint8_t *)&nv + off, &v, n);
        if (__atomic_compare_exchange_n(blk, &cur, nv, 0, __ATOMIC_SEQ_CST, __ATOMIC_SEQ_CST)) return 1;
    }
}
#define SG_MISALIGNED(a, n) SG_UNLIKELY((a) & ((n) - 1u))
#define SG_CHECK_ALIGN(ctx, a, n) \
    do { if (SG_UNLIKELY((a) & ((n) - 1u))) sg_misaligned_atomic((ctx), (a)); } while (0)
SG_INLINE uint32_t sg_lock_xadd32(sg_ctx *ctx, uint8_t *mem, uint32_t a, uint32_t v) {
    if (SG_MISALIGNED(a, 4u)) {
        uint64_t e = sg_ld32(mem, a);
        while (!sg__ma_cas(ctx, mem, a, 4u, &e, (uint32_t)(e + v))) { }
        return (uint32_t)e;
    }
    return __atomic_fetch_add((uint32_t *)(void *)(mem + a), v, __ATOMIC_SEQ_CST);
}
SG_INLINE uint32_t sg_xchg32(sg_ctx *ctx, uint8_t *mem, uint32_t a, uint32_t v) {
    if (SG_MISALIGNED(a, 4u)) {
        uint64_t e = sg_ld32(mem, a);
        while (!sg__ma_cas(ctx, mem, a, 4u, &e, v)) { }
        return (uint32_t)e;
    }
    return __atomic_exchange_n((uint32_t *)(void *)(mem + a), v, __ATOMIC_SEQ_CST);
}
/* lock cmpxchg: returns 1 on success (ZF=1); on failure *expected receives the current value. */
SG_INLINE int sg_lock_cmpxchg32(sg_ctx *ctx, uint8_t *mem, uint32_t a, uint32_t *expected, uint32_t v) {
    if (SG_MISALIGNED(a, 4u)) {
        uint64_t e = *expected;
        int ok = sg__ma_cas(ctx, mem, a, 4u, &e, v);
        *expected = (uint32_t)e;
        return ok;
    }
    return __atomic_compare_exchange_n((uint32_t *)(void *)(mem + a), expected, v, 0,
                                       __ATOMIC_SEQ_CST, __ATOMIC_SEQ_CST);
}
SG_INLINE int sg_lock_cmpxchg64(sg_ctx *ctx, uint8_t *mem, uint32_t a, uint64_t *expected, uint64_t v) {
    if (SG_MISALIGNED(a, 8u)) return sg__ma_cas(ctx, mem, a, 8u, expected, v);
    return __atomic_compare_exchange_n((uint64_t *)(void *)(mem + a), expected, v, 0,
                                       __ATOMIC_SEQ_CST, __ATOMIC_SEQ_CST);
}

/* fs: segment (TEB) access. fs is the only segment override in the image [obs]. */
#define SG_FS(ctx, off) ((uint32_t)((ctx)->fs_base + (uint32_t)(off)))

/* ------------------------------------------------------------------------------------------ */
/* 5. Translated functions, calls and returns (doc section 5)                                  */
/* ------------------------------------------------------------------------------------------ */

/* Every guest entry point -- translated function, import shim, COM-method thunk, runtime
 * helper -- has this one host type. All arguments and results travel in ctx and the guest
 * stack, so custom (LTCG) conventions, thiscall, stdcall and cdecl need no signatures. */
typedef void sg_func(sg_ctx *ctx, uint8_t *mem);

/* Host symbol for the translated function whose guest entry is `va`, written as exactly 8
 * lower-case hex digits WITHOUT 0x: SG_FUNC(005a9040) defines sg_f_005a9040. */
#define SG_FN_NAME(va) sg_f_##va
#define SG_FUNC(va) void SG_FN_NAME(va)(sg_ctx *SG_RESTRICT ctx, uint8_t *mem)
#define SG_DECL(va) extern void SG_FN_NAME(va)(sg_ctx *ctx, uint8_t *mem)

/* Push / pop on the guest stack (the guest stack lives in the window; doc 5.2). */
#define SG_PUSH32(v) do { uint32_t v_ = (uint32_t)(v); ctx->esp -= 4u; sg_st32(mem, ctx->esp, v_); } while (0)
#define SG_POP32(lv) do { (lv) = sg_ld32(mem, ctx->esp); ctx->esp += 4u; } while (0)

/* Return-address check (doc 5.3). After a host call returns, ctx->eip holds the address the
 * callee's `ret` popped. It must equal the caller's fall-through address. */
#ifndef SG_CHECK_RETURNS
#define SG_CHECK_RETURNS 1
#endif
extern SG_COLD void sg_return_mismatch(sg_ctx *ctx, uint8_t *mem, uint32_t expected); /* [runtime] */
#if SG_CHECK_RETURNS
#define SG_AFTER_CALL(ra) do { if (SG_UNLIKELY(ctx->eip != (uint32_t)(ra))) sg_return_mismatch(ctx, mem, (ra)); } while (0)
#else
#define SG_AFTER_CALL(ra) do { } while (0)
#endif

/* call rel32 to a known translated function. `ra` = guest address of the next instruction. */
#define SG_CALL(fn, ra) do { SG_PUSH32(ra); fn(ctx, mem); SG_AFTER_CALL(ra); } while (0)

/* call r/m32. The target is evaluated BEFORE the push (x86 order: `call [esp+4]` reads the
 * operand with the old esp). */
#define SG_CALL_IND(target_expr, ra) do { \
        uint32_t t_ = (uint32_t)(target_expr); SG_PUSH32(ra); \
        sg_lookup(ctx, mem, t_)(ctx, mem); SG_AFTER_CALL(ra); } while (0)

/* ret / ret N: pop the return address into eip, release N bytes of arguments, host-return. */
#define SG_RET(n) do { ctx->eip = sg_ld32(mem, ctx->esp); ctx->esp += 4u + (uint32_t)(n); return; } while (0)

/* jmp to another function's entry (tail call / thunk): host tail call, no push. */
#define SG_TAILCALL(fn) do { SG_MUSTTAIL return fn(ctx, mem); } while (0)
#define SG_TAILCALL_IND(target_expr) do { uint32_t t_ = (uint32_t)(target_expr); \
        sg_func *f_ = sg_lookup(ctx, mem, t_); SG_MUSTTAIL return f_(ctx, mem); } while (0)

/* ------------------------------------------------------------------------------------------ */
/* 6. Dispatch table (doc section 6)                                                           */
/* ------------------------------------------------------------------------------------------ */

/* Generated by the image linker. Entries are sorted by guest address. For every
 * 16-byte bucket b of .text, sg_dispatch_bucket[b] is the index of the first entry with
 * guest >= SG_TEXT_START + 16*b; bucket b's entries are [bucket[b], bucket[b+1]).
 * [obs] 97.4% of the 91,124 known entries are 16-aligned and no bucket holds more than 3. */
typedef struct sg_dispatch_entry {
    uint32_t guest;
    uint32_t flags; /* SG_DE_* */
    sg_func *fn;
} sg_dispatch_entry;
enum { SG_DE_FUNC = 1u, SG_DE_SECONDARY = 2u /* mid-function entry (shared tail) */ };

#define SG_DISPATCH_BUCKETS (((SG_TEXT_END - SG_TEXT_START) + 15u) / 16u + 1u)
#ifndef SG_DISPATCH_CONST
#define SG_DISPATCH_CONST const /* host tests that build tables at run time define it empty */
#endif
extern SG_DISPATCH_CONST uint32_t sg_dispatch_bucket[];       /* SG_DISPATCH_BUCKETS + 1 entries */
extern SG_DISPATCH_CONST sg_dispatch_entry sg_dispatch_entries[];
extern sg_func *sg_thunk_fn[];                     /* (SG_THUNK_LIMIT-SG_THUNK_BASE)/16 slots */

/* Miss hook: called for any guest code address with no translation. Returns the function to
 * run (dev builds: logs the address, appends it to the miss list and aborts; a future
 * interpreter can return a trampoline). Replaceable at run time. [runtime] */
typedef sg_func *(*sg_miss_hook_fn)(sg_ctx *ctx, uint8_t *mem, uint32_t guest);
extern sg_miss_hook_fn sg_miss_hook;

SG_INLINE sg_func *sg_lookup(sg_ctx *ctx, uint8_t *mem, uint32_t a) {
    uint32_t o = a - SG_TEXT_START;
    if (SG_LIKELY(o < SG_TEXT_END - SG_TEXT_START)) {
        uint32_t b = o >> 4, i = sg_dispatch_bucket[b], e = sg_dispatch_bucket[b + 1];
        for (; i < e; i++)
            if (sg_dispatch_entries[i].guest == a) return sg_dispatch_entries[i].fn;
    } else if (a - SG_THUNK_BASE < SG_THUNK_LIMIT - SG_THUNK_BASE && !(a & (SG_THUNK_SLOT - 1u))) {
        sg_func *f = sg_thunk_fn[(a - SG_THUNK_BASE) / SG_THUNK_SLOT];
        if (SG_LIKELY(f != 0)) return f;
    }
    return sg_miss_hook(ctx, mem, a);
}

/* ------------------------------------------------------------------------------------------ */
/* 7. Host shims called from the guest (imports, COM methods; doc section 7)                   */
/* ------------------------------------------------------------------------------------------ */

/* A shim is an sg_func. On entry: [esp] = return address, args at esp+4, esp+8, ...
 * It sets eax (and edx for 64-bit results, or pushes ST(0) for float/double results),
 * must not change ebx/esi/edi/ebp, and finishes with SG_RET(n) where n is the stdcall
 * argument byte count (0 for cdecl). This is identical to a translated function's exit. */
#define SG_ARG(i) sg_ld32(mem, ctx->esp + 4u + 4u * (uint32_t)(i))
#define SG_ARG_F32(i) sg_ldf32(mem, ctx->esp + 4u + 4u * (uint32_t)(i))
#define SG_ARG_F64(i) sg_ldf64(mem, ctx->esp + 4u + 4u * (uint32_t)(i)) /* uses slots i, i+1 */
#define SG_ARG_U64(i) sg_ld64(mem, ctx->esp + 4u + 4u * (uint32_t)(i))  /* uses slots i, i+1 */
#define SG_ARG_PTR(i) SG_HOSTPTR(mem, SG_ARG(i)) /* only after a NULL check on SG_ARG(i) */

/* Declares an import shim. Naming: sg_imp_<DLL stem lower-case>_<export name or ordN>. */
#define SG_IMPORT(dll, name) void sg_imp_##dll##_##name(sg_ctx *SG_RESTRICT ctx, uint8_t *mem)
#define SG_RETURN_STDCALL(eaxval, nbytes) do { ctx->eax = (uint32_t)(eaxval); SG_RET(nbytes); } while (0)
#define SG_RETURN_CDECL(eaxval) do { ctx->eax = (uint32_t)(eaxval); SG_RET(0); } while (0)
#define SG_RETURN_U64(v, nbytes) do { uint64_t v_ = (uint64_t)(v); ctx->eax = (uint32_t)v_; \
        ctx->edx = (uint32_t)(v_ >> 32); SG_RET(nbytes); } while (0)

/* Import table entry, generated by the HLE generator from data/imports.json. The loader writes
 * `guest` (a thunk-region address) into the IAT slot `iat`. */
typedef struct sg_import {
    const char *dll;     /* "KERNEL32.dll" */
    const char *name;    /* export name, or NULL when imported by ordinal */
    uint16_t ordinal;
    uint8_t cc;          /* SG_CC_* */
    uint8_t argbytes;    /* stdcall argument bytes (0 for cdecl / unknown) */
    uint32_t iat;        /* guest address of the IAT slot */
    uint32_t guest;      /* thunk address assigned at startup */
    sg_func *fn;         /* host shim */
} sg_import;
enum sg_cc { SG_CC_CDECL = 0, SG_CC_STDCALL = 1, SG_CC_THISCALL = 2, SG_CC_FASTCALL = 3, SG_CC_X87 = 4 /* _CI* */ };

/* Registers a host function and returns its guest-callable thunk address. Thread-safe. [runtime] */
extern uint32_t sg_thunk_register(sg_func *fn, const char *debug_name);

/* x87 helpers for shims that take/return values on the FPU stack (_CI*, floor, D3DX...). */
SG_INLINE void sg_fpu_push(sg_ctx *ctx, sg_f80 v) {
    ctx->fpu.top = (ctx->fpu.top - 1u) & 7u;
    ctx->fpu.st[ctx->fpu.top] = v;
    ctx->fpu.tag &= (uint16_t)~(3u << (2u * ctx->fpu.top));
}
SG_INLINE sg_f80 sg_fpu_pop(sg_ctx *ctx) {
    sg_f80 v = ctx->fpu.st[ctx->fpu.top];
    ctx->fpu.tag |= (uint16_t)(3u << (2u * ctx->fpu.top));
    ctx->fpu.top = (ctx->fpu.top + 1u) & 7u;
    return v;
}
#define SG_ST(ctx, i) ((ctx)->fpu.st[((ctx)->fpu.top + (uint32_t)(i)) & 7u])

/* ------------------------------------------------------------------------------------------ */
/* 8. Calling guest code from the host (callbacks; doc 7.3)                                   */
/* ------------------------------------------------------------------------------------------ */

/* Runs guest code at `target` on the current thread's context and guest stack, as if called
 * by a `call` instruction whose return address is SG_RET_TO_HOST. Arguments are pushed right
 * to left. For SG_CC_THISCALL args[0] goes in ecx, the rest on the stack. For cdecl the
 * runtime removes the arguments afterwards; for stdcall/thiscall it checks the callee did.
 * Returns eax (edx is left in ctx->edx; a float result is left in ST(0)).
 * Volatile guest registers (eax, ecx, edx, flags) are clobbered; the callee preserves
 * ebx/esi/edi/ebp per the Win32 ABI and sg_call_guest verifies esp/ebp/ebx/esi/edi in dev
 * builds. Only legal while the thread is inside a shim (or at thread start). [runtime] */
extern uint32_t sg_call_guest(sg_ctx *ctx, uint32_t target, enum sg_cc cc, unsigned nargs,
                              const uint32_t *args);

/* Calls method `slot` of a guest-memory COM object (vtable of guest code addresses);
 * COM methods are stdcall with `this` as the first stack argument. */
SG_INLINE uint32_t sg_call_guest_method(sg_ctx *ctx, uint32_t obj, unsigned slot, unsigned nargs,
                                         const uint32_t *args_after_this) {
    uint32_t a[16];
    unsigned i;
    uint32_t vt = sg_ld32(ctx->mem, obj), fn = sg_ld32(ctx->mem, vt + 4u * slot);
    a[0] = obj;
    for (i = 0; i < nargs && i < 15u; i++) a[i + 1] = args_after_this[i];
    return sg_call_guest(ctx, fn, SG_CC_STDCALL, nargs + 1u, a);
}

/* ------------------------------------------------------------------------------------------ */
/* 9. Host COM objects that live in guest memory (doc section 8)                               */
/* ------------------------------------------------------------------------------------------ */

/* Guest-visible layout of every object the runtime hands to the guest (IDirect3D9,
 * IDirect3DDevice9, textures, IDirectSound, ...):
 *     +0  vptr   -> guest address of a guest-memory vtable (array of thunk addresses)
 *     +4  handle -> index into the runtime's host object table (host private)
 *     +8  magic  -> SG_COM_MAGIC, checked in dev builds
 *     +12 reserved
 * Objects are 16 bytes, 16-aligned, allocated from the window by the runtime. One host object
 * exposing several interfaces gets one such record per interface, sharing the handle. */
#define SG_COM_MAGIC 0x4d4f4353u /* 'SCOM' */
typedef struct sg_com_guest {
    uint32_t vptr;
    uint32_t handle;
    uint32_t magic;
    uint32_t reserved;
} sg_com_guest;
SG_STATIC_ASSERT(sizeof(sg_com_guest) == 16, "guest COM record is 16 bytes");

/* Host side: lookup from a guest `this`. [runtime] */
extern void *sg_com_host(sg_ctx *ctx, uint32_t guest_this);
/* Creates the guest record for host object `host` using vtable `vtbl_guest` (built once per
 * interface with sg_com_vtable). Returns the guest address. [runtime] */
extern uint32_t sg_com_wrap(sg_ctx *ctx, void *host, uint32_t vtbl_guest);
/* Builds a guest vtable of `n` thunk addresses for the given host methods. [runtime] */
extern uint32_t sg_com_vtable(sg_ctx *ctx, sg_func *const *methods, unsigned n, const char *iface);

/* ------------------------------------------------------------------------------------------ */
/* 10. Threads, TEB and TLS (doc section 9)                                                    */
/* ------------------------------------------------------------------------------------------ */

/* x86 TEB offsets (Windows layout; the game reads 0x0, 0x4, 0x8, 0x18, 0x2c [obs]). */
#define SG_TEB_EXCEPTION_LIST 0x000u /* fs:[0], SEH registration chain head */
#define SG_TEB_STACK_BASE 0x004u      /* highest address + 1 of the guest stack */
#define SG_TEB_STACK_LIMIT 0x008u     /* lowest committed address */
#define SG_TEB_SELF 0x018u            /* guest address of the TEB itself */
#define SG_TEB_CLIENT_PID 0x020u
#define SG_TEB_CLIENT_TID 0x024u
#define SG_TEB_TLS_POINTER 0x02cu     /* -> array of __declspec(thread) block pointers */
#define SG_TEB_PEB 0x030u
#define SG_TEB_LAST_ERROR 0x034u      /* GetLastError/SetLastError live here */
#define SG_TEB_TLS_SLOTS 0xe10u       /* TlsSlots[64] */
#define SG_TEB_TLS_EXPANSION 0xf94u   /* -> 1024 further slots, allocated on first use */
#define SG_TEB_SIZE 0x1000u
#define SG_TLS_SLOTS_INLINE 64u
#define SG_TLS_SLOTS_TOTAL 1088u

/* Image TLS directory facts [obs: read from the exe's PE TLS directory]. The loader stores 0 at the index
 * variable and gives every thread a copy of the 12-byte template at TLS_POINTER[0]. */
#define SG_IMAGE_TLS_INDEX_VA 0x016f2618u
#define SG_IMAGE_TLS_TEMPLATE 0x0171f000u
#define SG_IMAGE_TLS_TEMPLATE_SIZE 12u

/* Fast TlsGetValue for shims: two loads, no locks. */
SG_INLINE uint32_t sg_tls_get(sg_ctx *ctx, uint8_t *mem, uint32_t slot) {
    if (SG_LIKELY(slot < SG_TLS_SLOTS_INLINE))
        return sg_ld32(mem, ctx->fs_base + SG_TEB_TLS_SLOTS + 4u * slot);
    uint32_t x = sg_ld32(mem, ctx->fs_base + SG_TEB_TLS_EXPANSION);
    return x ? sg_ld32(mem, x + 4u * (slot - SG_TLS_SLOTS_INLINE)) : 0u;
}

/* Per-thread runtime state. Generated code only touches ctx; shims may use the rest. */
typedef struct sg_thread {
    sg_ctx ctx;              /* first member: (sg_thread *)ctx is legal */
    uint32_t teb;            /* == ctx.fs_base */
    uint32_t stack_lo, stack_hi; /* guest stack [lo, hi); a PROT_NONE guard page sits below lo */
    uint32_t tls_block;      /* this thread's copy of the image TLS template */
    uint32_t tid;            /* Windows-style thread id given to the guest */
    void *host;              /* runtime-private (pthread, APC queue, wait objects ...) */
} sg_thread;

extern SG_THREAD_LOCAL sg_thread *sg_self; /* current thread; NULL on foreign host threads [runtime] */

/* Creates a guest thread that will run `entry(param)` (stdcall, 1 arg) on a new guest stack
 * of `stack_bytes` (0 = image default 1 MiB). Backs _beginthreadex/CreateThread. [runtime] */
extern sg_thread *sg_thread_create(uint32_t entry, uint32_t param, uint32_t stack_bytes, int suspended);

/* ------------------------------------------------------------------------------------------ */
/* 11. Exception-handling hand-off (doc section 10)                                            */
/* ------------------------------------------------------------------------------------------ */

/* Guest SEH/C++ EH state is entirely in guest memory (fs:[0] chain, FuncInfo, frames).
 * Translated code needs host help only to RESUME in the middle of a function whose host frame
 * is further up the host stack: catch continuations, __except blocks, and the return from
 * _setjmp3 after longjmp. Functions containing such resume points register a landing. */
#include <setjmp.h>
#ifndef SG_HOST_SETJMP
#if defined(__x86_64__) || defined(__i386__)
/* x86: clang's lightweight builtins (5 words, no signal mask). */
#define SG_HOST_SETJMP(jb) __builtin_setjmp(jb)
#define SG_HOST_LONGJMP(jb) __builtin_longjmp(jb, 1)
typedef void *sg_host_jmpbuf[5];
#else
/* arm64: Apple clang rejects __builtin_setjmp ("not supported for the current target"), so use the
 * libc pair that skips the signal mask. Costlier (22 GPRs plus the callee-saved FP registers), but
 * paid only at entry to the 49 landing owners. [2026-10-12] */
#define SG_HOST_SETJMP(jb) _setjmp(jb)
#define SG_HOST_LONGJMP(jb) _longjmp(jb, 1)
typedef jmp_buf sg_host_jmpbuf;
#endif
#endif

typedef struct sg_landing {
    struct sg_landing *prev;
    uint32_t func;      /* guest entry of the translated function that owns the landing */
    uint32_t frame_esp; /* guest esp at entry (address of the return-address slot) */
    sg_host_jmpbuf jb;
} sg_landing;

/* In a function with resume points:
 *     SG_FUNC(0x...) {
 *         SG_LANDING_BEGIN(0x...);          // first statement
 *         ...                               // body; every exit uses SG_LANDING_RET / _TAIL
 *       SG_RESUME_DISPATCH:
 *         switch (ctx->eip) { case 0x...: goto L_...; ... default: sg_bad_resume(ctx); }
 *     }
 * The runtime sets ctx->eip/esp/ebp/ebx/esi/edi/eax, then longjmps to the landing. */
#define SG_LANDING_BEGIN(va) \
    sg_landing land_; land_.prev = (sg_landing *)ctx->eh_top; land_.func = (va); \
    land_.frame_esp = ctx->esp; ctx->eh_top = &land_; \
    if (SG_HOST_SETJMP(land_.jb)) goto sg_resume_dispatch_
#define SG_RESUME_DISPATCH sg_resume_dispatch_
#define SG_LANDING_POP() (ctx->eh_top = land_.prev)
#define SG_LANDING_RET(n) do { SG_LANDING_POP(); SG_RET(n); } while (0)
/* A landing function never host-tail-calls: its frame must be gone before the callee runs. */
#define SG_LANDING_TAIL(fn) do { SG_LANDING_POP(); fn(ctx, mem); return; } while (0)

extern SG_NORETURN SG_COLD void sg_bad_resume(sg_ctx *ctx); /* [runtime] */

/* Resume-point table, generated with the dispatch table: every guest address at which a
 * landing function can be re-entered (catch continuations, __except bodies, the return
 * address of each `call _setjmp3`), sorted by `resume`. [obs] 11 catch handlers in 11 try
 * blocks + 28 _setjmp3 call sites + 10 __except frames (runtime-abi.md 10.3). */
typedef struct sg_resume_point { uint32_t resume; uint32_t func; } sg_resume_point;
extern SG_DISPATCH_CONST sg_resume_point sg_resume_points[];
extern const uint32_t sg_resume_point_count;

/* Called by the runtime's __CxxFrameHandler3 / _except_handler3/4 / longjmp after it has
 * unwound guest state and loaded esp/ebp/ebx/esi/edi/eax into ctx: continue at guest address
 * `target`. Picks the innermost landing whose func owns `target` (sg_resume_points) and
 * whose frame_esp is above the new esp, unlinks every inner landing, sets ctx->eip and
 * longjmps into it. [runtime] */
extern SG_NORETURN void sg_eh_resume(sg_ctx *ctx, uint32_t target);

/* Raises a guest exception (RaiseException, _CxxThrowException, hardware faults later):
 * builds EXCEPTION_RECORD/CONTEXT on the guest stack and walks the fs:[0] chain, calling each
 * handler with sg_call_guest(cdecl, 4 args). Returns only for continuable exceptions. [runtime] */
extern void sg_seh_raise(sg_ctx *ctx, uint32_t code, uint32_t flags, unsigned nparams,
                         const uint32_t *params);

/* ------------------------------------------------------------------------------------------ */
/* 12. CPU intrinsics and misc runtime entry points                                            */
/* ------------------------------------------------------------------------------------------ */

extern void sg_cpuid(sg_ctx *ctx);  /* reads eax/ecx, writes eax..edx: a fixed SSE-class CPU [runtime] */
extern void sg_rdtsc(sg_ctx *ctx);  /* edx:eax from the host counter [runtime] */
extern SG_NORETURN SG_COLD void sg_unhandled_insn(sg_ctx *ctx, uint32_t va); /* translator gap [runtime] */
extern SG_NORETURN SG_COLD void sg_int3(sg_ctx *ctx, uint32_t va);         /* reached padding [runtime] */
/* div/idiv by zero or quotient overflow (#DE on x86; arm64 does not trap, so the translator
 * tests explicitly and calls this; it raises EXCEPTION_INT_DIVIDE_BY_ZERO/OVERFLOW). [runtime] */
extern SG_NORETURN SG_COLD void sg_divide_error(sg_ctx *ctx, uint32_t va);

#ifdef __cplusplus
}
#endif
#endif /* SPORE_GUEST_H */
