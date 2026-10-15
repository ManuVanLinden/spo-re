/* weak_runtime.c -- WEAK default definitions of every [runtime] symbol that the translated code, the
 * HLE shims and this loader reference, so the whole image links before runtime/core and
 * runtime/win32 exist. A strong definition anywhere in the link replaces the default.
 *
 * Policy: a default that cannot be correct without the real runtime reports "<symbol>: not provided
 * by runtime/core" and aborts the first time it runs. Defaults that are already the right development
 * behaviour (fault reports, 80-bit conversions, a bump allocator for runtime-owned guest data) are
 * real implementations. tools/link/build.py lists which defaults survive in a given link
 * (build/sporenative/link-report.json, "weak_defaults_in_use"). */
#include <stdio.h>
#include <stdlib.h>
#include <string.h>
#include <sys/mman.h>

#include "hle.h"
#include "sg_image.h"

#define WEAK __attribute__((weak))

static SG_NORETURN SG_COLD void missing(const char *sym, sg_ctx *ctx) {
    fprintf(stderr, "spore: %s: not provided by runtime/core (weak default in runtime/image)", sym);
    if (ctx) fprintf(stderr, "; guest esp=0x%08x [esp]=0x%08x eip=0x%08x", ctx->esp,
                     ctx->mem ? sg_ld32(ctx->mem, ctx->esp) : 0u, ctx->eip);
    fprintf(stderr, "\n");
    abort();
}

WEAK SG_THREAD_LOCAL sg_thread *sg_self;

/* ---- reports that are already the intended development behaviour ------------------------- */

WEAK void sg_return_mismatch(sg_ctx *ctx, uint8_t *mem, uint32_t expected) {
    (void)mem;
    fprintf(stderr, "spore: return-address mismatch: expected 0x%08x, callee returned to 0x%08x (esp=0x%08x)\n",
            expected, ctx->eip, ctx->esp);
    abort();
}
WEAK void sg_unhandled_insn(sg_ctx *ctx, uint32_t va) {
    fprintf(stderr, "spore: untranslated instruction at guest 0x%08x (esp=0x%08x)\n", va, ctx->esp);
    abort();
}
WEAK void sg_int3(sg_ctx *ctx, uint32_t va) {
    fprintf(stderr, "spore: int3 reached at guest 0x%08x (esp=0x%08x)\n", va, ctx->esp);
    abort();
}
WEAK void sg_divide_error(sg_ctx *ctx, uint32_t va) {
    /* Hardware faults abort with a report for now. */
    fprintf(stderr, "spore: divide error (#DE) at guest 0x%08x\n", va);
    (void)ctx;
    abort();
}
WEAK void sg_misaligned_atomic(sg_ctx *ctx, uint32_t a) {
    fprintf(stderr, "spore: misaligned locked access at guest 0x%08x (esp=0x%08x)\n", a, ctx->esp);
    abort();
}
WEAK void sg_bad_resume(sg_ctx *ctx) {
    fprintf(stderr, "spore: bad EH resume: no resume point 0x%08x in this landing\n", ctx->eip);
    abort();
}

/* ---- x87 80-bit memory operands (no m80 operands exist in .text [observed]; kept correct anyway) */

#ifndef SG_X87_F80
WEAK sg_f80 sg_ldf80(const uint8_t *mem, uint32_t a) {
    uint64_t mant;
    uint16_t se;
    int e;
    memcpy(&mant, mem + a, 8);
    memcpy(&se, mem + a + 8, 2);
    e = se & 0x7fff;
    if (e == 0 && mant == 0) return (se & 0x8000) ? -0.0 : 0.0;
    if (e == 0x7fff) return (mant << 1) ? __builtin_nan("") : ((se & 0x8000) ? -__builtin_inf() : __builtin_inf());
    {
        double v = __builtin_ldexp((double)mant, e - 16383 - 63);
        return (se & 0x8000) ? -v : v;
    }
}
WEAK void sg_stf80(uint8_t *mem, uint32_t a, sg_f80 v) {
    uint64_t mant = 0;
    uint16_t se = __builtin_signbit(v) ? 0x8000 : 0;
    if (__builtin_isnan(v)) { se |= 0x7fff; mant = 0xc000000000000000ull; }
    else if (__builtin_isinf(v)) { se |= 0x7fff; mant = 0x8000000000000000ull; }
    else if (v != 0.0) {
        int e;
        double f = __builtin_frexp(__builtin_fabs(v), &e); /* v = f * 2^e, f in [0.5, 1) */
        mant = (uint64_t)__builtin_ldexp(f, 64);
        se |= (uint16_t)(e - 1 + 16383);
    }
    memcpy(mem + a, &mant, 8);
    memcpy(mem + a + 8, &se, 2);
}
#endif

/* ---- runtime/core entry points: absent until it links ------------------------------- */

WEAK void sg_cpuid(sg_ctx *ctx) { missing("sg_cpuid", ctx); }
WEAK void sg_rdtsc(sg_ctx *ctx) { missing("sg_rdtsc", ctx); }
WEAK void sg_set_mxcsr(sg_ctx *ctx, uint32_t v) { (void)v; missing("sg_set_mxcsr", ctx); }
WEAK void sg_fxsave(sg_ctx *ctx, uint8_t *mem, uint32_t a) { (void)mem; (void)a; missing("sg_fxsave", ctx); }
WEAK uint32_t sg_call_guest(sg_ctx *ctx, uint32_t target, enum sg_cc cc, unsigned nargs, const uint32_t *args) {
    (void)target; (void)cc; (void)nargs; (void)args;
    missing("sg_call_guest", ctx);
}
WEAK void sg_eh_resume(sg_ctx *ctx, uint32_t target) { (void)target; missing("sg_eh_resume", ctx); }
WEAK void sg_seh_raise(sg_ctx *ctx, uint32_t code, uint32_t flags, unsigned nparams, const uint32_t *params) {
    (void)code; (void)flags; (void)nparams; (void)params;
    missing("sg_seh_raise", ctx);
}
WEAK sg_thread *sg_thread_create(uint32_t entry, uint32_t param, uint32_t stack_bytes, int suspended) {
    (void)entry; (void)param; (void)stack_bytes; (void)suspended;
    missing("sg_thread_create", NULL);
}
/* sg_com_host, sg_com_wrap and sg_com_vtable (runtime-abi 8.1) are defined by runtime/com,
 * which tools/link/build.py always links; their weak aborts were removed here. */

/* ---- runtime-owned guest data before the guest heap (runtime/win32) exists ------------------------- */

/* A bump allocator in [0x7f000000, 0x7fe00000), just below the thunk region. Only used when no strong
 * hle_guest_alloc is linked; the real heap replaces it. Single-threaded use at start-up. */
#define BUMP_LO 0x7f000000u
#define BUMP_HI SG_THUNK_BASE
WEAK uint32_t hle_guest_alloc(uint32_t size, uint32_t align) {
    static uint32_t next = BUMP_LO, committed = BUMP_LO;
    uint32_t a, end;
    if (!align) align = 1;
    a = (next + align - 1u) & ~(align - 1u);
    end = a + size;
    if (end < a || end > BUMP_HI) return 0;
    if (end > committed) {
        uint32_t c = (end + SG_HOST_PAGE - 1u) & ~(SG_HOST_PAGE - 1u);
        if (sg_image_window_commit(committed, c, PROT_READ | PROT_WRITE)) return 0;
        committed = c;
    }
    next = end;
    memset(sg_image_mem + a, 0, size);
    return a;
}

/* ---- start-up hooks ------------------------------------------------------------------------ */

unsigned sg_image_hooks_absent;
WEAK void sg_core_startup(uint8_t *mem) { (void)mem; sg_image_hooks_absent |= 1u; }
WEAK void sg_win32_startup(uint8_t *mem) { (void)mem; sg_image_hooks_absent |= 2u; }
WEAK int sg_boot(uint8_t *mem, uint32_t entry, int argc, char **argv) {
    (void)mem; (void)argc; (void)argv;
    sg_image_hooks_absent |= 4u;
    fprintf(stderr, "spore: no boot path linked (sg_boot comes from runtime/core or the boot diagnostics); "
                    "entry 0x%08x not called\n", entry);
    return 3;
}
