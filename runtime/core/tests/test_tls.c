/* test_tls.c -- TEB layout, TlsAlloc/TlsFree/TlsGetValue/TlsSetValue (inline and expansion slots),
 * the image TLS block, and misc runtime entry points (cpuid, f80, sg_call_guest checks). */
#include <math.h>

#include "core_internal.h"
#include "fixture.h"

SGC_SHIM(kernel32, TlsAlloc);
SGC_SHIM(kernel32, TlsFree);
SGC_SHIM(kernel32, TlsGetValue);
SGC_SHIM(kernel32, TlsSetValue);

void test_tls(void) {
    uint32_t teb = C->fs_base, idx[80], i;
    sg_thread *t = sg_self;

    t_name = "tls.teb";
    CHECK_EQ(sg_ld32(M, teb + SG_TEB_EXCEPTION_LIST), SG_END_OF_CHAIN);
    CHECK_EQ(sg_ld32(M, teb + SG_TEB_SELF), teb);
    CHECK_EQ(sg_ld32(M, teb + SG_TEB_STACK_BASE), t->stack_hi);
    CHECK_EQ(sg_ld32(M, teb + SG_TEB_STACK_LIMIT), t->stack_lo);
    CHECK_EQ(sg_ld32(M, teb + SG_TEB_CLIENT_TID), t->tid);
    CHECK_EQ(sg_ld32(M, teb + SG_TEB_PEB), SG_PEB_ADDR);
    CHECK_EQ(sg_ld32(M, SG_PEB_ADDR + 8), SG_IMAGE_BASE);
    CHECK_EQ(t->stack_hi - t->stack_lo, 0x100000u);
    CHECK(teb >= SG_TEB_REGION && teb < SG_PEB_ADDR, "TEB 0x%08x outside the TEB region", teb);
    {
        uint32_t arr = sg_ld32(M, teb + SG_TEB_TLS_POINTER), blk = sg_ld32(M, arr);
        CHECK_EQ(blk, t->tls_block);
        CHECK_EQ(sg_ld32(M, blk + 0), 0x11111111u);
        CHECK_EQ(sg_ld32(M, blk + 4), 0x22222222u);
        CHECK_EQ(sg_ld32(M, blk + 8), 0x33333333u);
        CHECK_EQ(sg_ld32(M, blk + 12), 0); /* zero fill */
    }
    /* thunk region holds int3 bytes and is read-only data for the guest */
    CHECK_EQ(M[SG_THUNK_BASE + 0x1234], 0xcc);

    t_name = "tls.slots";
    sg_st32(M, teb + SG_TEB_LAST_ERROR, 5);
    for (i = 0; i < 80; i++) idx[i] = GSTD0(sgc_kernel32_TlsAlloc);
    for (i = 0; i < 80; i++) CHECK(idx[i] < SG_TLS_SLOTS_TOTAL, "TlsAlloc %u failed", i);
    CHECK(idx[79] >= SG_TLS_SLOTS_INLINE, "80 allocations should reach the expansion slots");
    CHECK_EQ(GSTD(sgc_kernel32_TlsGetValue, idx[0]), 0);
    CHECK_EQ(sg_ld32(M, teb + SG_TEB_LAST_ERROR), 0); /* success clears the last error */
    for (i = 0; i < 80; i++) CHECK_EQ(GSTD(sgc_kernel32_TlsSetValue, idx[i], 0x1000u + i), 1);
    for (i = 0; i < 80; i++) CHECK_EQ(GSTD(sgc_kernel32_TlsGetValue, idx[i]), 0x1000u + i);
    CHECK(sg_ld32(M, teb + SG_TEB_TLS_EXPANSION) != 0, "expansion block not allocated");
    CHECK_EQ(sg_tls_get(C, M, idx[79]), 0x1000u + 79u); /* the header's fast path agrees */
    /* free clears the value; reallocation sees 0 */
    CHECK_EQ(GSTD(sgc_kernel32_TlsFree, idx[3]), 1);
    CHECK_EQ(GSTD(sgc_kernel32_TlsFree, idx[3]), 0);
    CHECK_EQ(GSTD(sgc_kernel32_TlsFree, idx[70]), 1);
    {
        uint32_t a = GSTD0(sgc_kernel32_TlsAlloc), b = GSTD0(sgc_kernel32_TlsAlloc);
        CHECK_EQ(a, idx[3]);
        CHECK_EQ(GSTD(sgc_kernel32_TlsGetValue, a), 0);
        CHECK_EQ(b, idx[70]);
        CHECK_EQ(GSTD(sgc_kernel32_TlsGetValue, b), 0);
    }
    /* invalid index */
    CHECK_EQ(GSTD(sgc_kernel32_TlsGetValue, 5000u), 0);
    CHECK_EQ(sg_ld32(M, teb + SG_TEB_LAST_ERROR), 87);
    CHECK_EQ(GSTD(sgc_kernel32_TlsSetValue, 1088u, 1u), 0);
    for (i = 0; i < 80; i++) GSTD(sgc_kernel32_TlsFree, idx[i]);
}

/* ---- misc: cpuid, f80 conversion, sg_call_guest checks, thunk registration ------------------- */

static void fx_bad_stdcall(sg_ctx *ctx, uint8_t *mem) { ctx->eax = 1; SG_RET(0); /* should pop 8 */ }
static void fx_clobber(sg_ctx *ctx, uint8_t *mem) { ctx->ebx = 0x1234; SG_RET(0); }
static void fx_wrong_ret(sg_ctx *ctx, uint8_t *mem) { ctx->esp += 4; ctx->eip = 0x1234; }
static void fx_thiscall(sg_ctx *ctx, uint8_t *mem) { ctx->eax = ctx->ecx + SG_ARG(0) * 10u; SG_RET(4); }
static void fx_fastcall(sg_ctx *ctx, uint8_t *mem) { ctx->eax = ctx->ecx * 100u + ctx->edx * 10u + SG_ARG(0); SG_RET(4); }

void test_misc(void) {
    t_name = "misc.cpuid";
    C->eax = 0;
    sg_cpuid(C);
    CHECK_EQ(C->ebx, 0x756e6547u);
    C->eax = 1;
    sg_cpuid(C);
    CHECK((C->edx & (1u << 25)) && (C->edx & (1u << 26)) && (C->edx & (1u << 23)), "SSE/SSE2/MMX bits");
    CHECK_EQ(C->ecx & 1u, 0); /* no SSE3 */
    {
        uint32_t lo, hi;
        uint64_t a, b;
        sg_rdtsc(C); lo = C->eax; hi = C->edx; a = ((uint64_t)hi << 32) | lo;
        sg_rdtsc(C); b = ((uint64_t)C->edx << 32) | C->eax;
        CHECK(b >= a, "rdtsc went backwards");
    }

    t_name = "misc.f80";
    {
        uint32_t a = fx_alloc(16);
        static const double vals[] = {0.0, -0.0, 1.0, -2.5, 3.141592653589793, 1e300, -1e-300, 5e-324, INFINITY};
        unsigned i;
        for (i = 0; i < sizeof vals / sizeof vals[0]; i++) {
            sg_stf80(M, a, vals[i]);
            CHECK(sg_ldf80(M, a) == vals[i] && signbit(sg_ldf80(M, a)) == signbit(vals[i]), "f80 round trip %g", vals[i]);
        }
        /* 1.0 as an x87 tbyte: mantissa 0x8000000000000000, exponent 0x3fff */
        sg_stf80(M, a, 1.0);
        CHECK_EQ(sg_ld64(M, a), 0x8000000000000000ull);
        CHECK_EQ(sg_ld16(M, a + 8), 0x3fffu);
        sg_stf80(M, a, NAN);
        CHECK(isnan(sg_ldf80(M, a)), "NaN round trip");
    }

    t_name = "misc.call-guest";
    fx_register(FX_FN(300), fx_bad_stdcall);
    fx_register(FX_FN(301), fx_clobber);
    fx_register(FX_FN(302), fx_wrong_ret);
    fx_register(FX_FN(303), fx_thiscall);
    fx_register(FX_FN(304), fx_fastcall);
    {
        uint32_t args[3] = {7, 3, 4};
        CHECK_EQ(sg_call_guest(C, FX_FN(303), SG_CC_THISCALL, 2, args), 37);
        CHECK_EQ(sg_call_guest(C, FX_FN(304), SG_CC_FASTCALL, 3, args), 734);
        CHECK(EXPECT_FATAL(sg_call_guest(C, FX_FN(300), SG_CC_STDCALL, 2, args)), "stdcall clean-up mismatch");
        CHECK(EXPECT_FATAL(sg_call_guest(C, FX_FN(301), SG_CC_CDECL, 0, NULL)), "callee-saved clobber");
        CHECK(EXPECT_FATAL(sg_call_guest(C, FX_FN(302), SG_CC_CDECL, 0, NULL)), "wrong return address");
        CHECK(EXPECT_FATAL(sg_call_guest(C, 0x004ff010u, SG_CC_CDECL, 0, NULL)), "dispatch miss");
        CHECK(EXPECT_FATAL(sg_call_guest(C, SG_THUNK_UNIMPL, SG_CC_CDECL, 0, NULL)), "SG_THUNK_UNIMPL");
    }
    t_name = "misc.thunks";
    {
        uint32_t a = sg_thunk_register(fx_thiscall, "fixture_thiscall"), b;
        uint32_t args[2] = {5, 6};
        CHECK(a >= SG_THUNK_ADDR(SG_THUNK_FIRST_FREE) && a < SG_THUNK_LIMIT, "thunk 0x%08x", a);
        CHECK(!strcmp(sg_thunk_name(a), "fixture_thiscall"), "thunk name");
        CHECK_EQ(sg_call_guest(C, a, SG_CC_THISCALL, 2, args), 65);
        b = sg_thunk_register(fx_thiscall, "again");
        CHECK_EQ(b, a + SG_THUNK_SLOT);
    }
    t_name = "misc.shim-table";
    CHECK(sg_core_shim_lookup("sg_imp_kernel32_TlsGetValue") == sgc_kernel32_TlsGetValue, "lookup");
    CHECK(sg_core_shim_lookup("sg_imp_kernel32_Nope") == NULL, "lookup of unknown");
    CHECK(sg_core_shim_count >= 50, "%u shims", sg_core_shim_count);
}
