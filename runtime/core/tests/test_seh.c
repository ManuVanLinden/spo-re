/* test_seh.c -- SEH fixtures: _except_handler3, _except_handler4_common (scope table XOR cookie,
 * the cookie check), __finally during local and global unwind, RaiseException (continuable,
 * noncontinuable, SetThreadName), _setjmp3/longjmp across frames with destructors, host cleanup
 * records and barriers, and the fatal paths of sg_eh_resume.
 *
 * SEH frame shape (MSVC x86):
 *   push ebp; mov ebp,esp; push trylevel; push scopetable(^cookie); push handler; push fs:[0];
 *   mov fs:[0],esp; sub esp,N; mov [ebp-18h],esp     rn = ebp-10h, trylevel at [ebp-4],
 *   EXCEPTION_POINTERS* at [ebp-14h] */
#include "core_internal.h"
#include "fixture.h"

SGC_SHIM(msvcr90, __CxxFrameHandler3);
SGC_SHIM(msvcr90, _CxxThrowException);
SGC_SHIM(msvcr90, _except_handler3);
SGC_SHIM(msvcr90, _except_handler4_common);
SGC_SHIM(msvcr90, _setjmp3);
SGC_SHIM(msvcr90, longjmp);
SGC_SHIM(kernel32, RaiseException);

enum {
    K_EH3THUNK = 100, K_EH4LOCAL, K_CHECK, K_FILT0, K_FILT1, K_FIN0, K_FIN1, K_RAISER, K_SEH_A = 110, K_SEH_B,
    K_SJ = 120, K_LJ, K_SHIMCB = 130,
    /* from test_eh.c */
    K_CATCH_A = 20, K_CFN_A = 30, K_LEAF1 = 40, K_UNW0 = 50, K_THROW = 3, K_STUB0 = 4, K_STUB3 = 7
};
#define CONT_OFF 0x80u

static uint32_t g_cookie_va, g_cookie_checks, g_cookie_bad;
static int32_t g_filter_ret[2];
static uint32_t g_raise_code, g_raise_flags;
static int g_corrupt;

struct sehcfg {
    int eh4;
    uint32_t table;          /* scope table (plain; the frame stores it XOR the cookie for EH4) */
    int nenter;              /* trylevels entered before the call, in order */
    int32_t levels[4];
    uint32_t callee;         /* 0: call the raiser */
    uint32_t ebp;
};
static struct sehcfg g_seh[2];

/* the exe's `jmp [IAT]` thunk for _except_handler3 */
static void fx_eh3thunk(sg_ctx *ctx, uint8_t *mem) { SG_TAILCALL(sgc_msvcr90__except_handler3); }
/* the exe-local _except_handler4: forwards to _except_handler4_common(&cookie, check, rec, rn, ctx, dc) */
static void fx_eh4local(sg_ctx *ctx, uint8_t *mem) {
    uint32_t a0 = SG_ARG(0), a1 = SG_ARG(1), a2 = SG_ARG(2), a3 = SG_ARG(3);
    SG_PUSH32(a3); SG_PUSH32(a2); SG_PUSH32(a1); SG_PUSH32(a0);
    SG_PUSH32(FX_FN(K_CHECK));
    SG_PUSH32(g_cookie_va);
    SG_CALL(sgc_msvcr90__except_handler4_common, FX_FN(K_EH4LOCAL) + 0x10u);
    ctx->esp += 24u;
    SG_RET(0);
}
/* __security_check_cookie: cookie in ecx (fastcall) */
static void fx_check(sg_ctx *ctx, uint8_t *mem) {
    g_cookie_checks++;
    if (ctx->ecx != sg_ld32(mem, g_cookie_va)) g_cookie_bad++;
    SG_RET(0);
}
static uint32_t xp_code(sg_ctx *ctx, uint8_t *mem) {
    uint32_t xp = sg_ld32(mem, ctx->ebp - 0x14u);
    return xp ? sg_ld32(mem, sg_ld32(mem, xp)) : 0;
}
static void fx_filt0(sg_ctx *ctx, uint8_t *mem) {
    fx_log(EV_FILTER, ctx->ebp, xp_code(ctx, mem));
    if (g_corrupt) sg_st32(mem, ctx->ebp - 0x20u, sg_ld32(mem, ctx->ebp - 0x20u) ^ 1u); /* smash the EH cookie */
    ctx->eax = (uint32_t)g_filter_ret[0];
    SG_RET(0);
}
static void fx_filt1(sg_ctx *ctx, uint8_t *mem) {
    fx_log(EV_FILTER, ctx->ebp, xp_code(ctx, mem));
    ctx->eax = (uint32_t)g_filter_ret[1];
    SG_RET(0);
}
static void fx_fin0(sg_ctx *ctx, uint8_t *mem) { fx_log(EV_FINALLY, ctx->ebp, 0); SG_RET(0); }
static void fx_fin1(sg_ctx *ctx, uint8_t *mem) { fx_log(EV_FINALLY, ctx->ebp, 1); SG_RET(0); }
static void fx_raiser(sg_ctx *ctx, uint8_t *mem) {
    SG_PUSH32(0); SG_PUSH32(0); SG_PUSH32(g_raise_flags); SG_PUSH32(g_raise_code);
    SG_CALL(sgc_kernel32_RaiseException, FX_FN(K_RAISER) + 0x10u);
    fx_log(EV_MISC, 0xc0, 0); /* RaiseException returned (continued) */
    SG_RET(0);
}

#define SEHFN(X, k)                                                                                       \
    static void fx_seh##X(sg_ctx *ctx, uint8_t *mem) {                                                    \
        struct sehcfg *s = &g_seh[k];                                                                     \
        int i;                                                                                            \
        SG_LANDING_BEGIN(FX_FN(K_SEH_##X));                                                               \
        SG_PUSH32(ctx->ebp); ctx->ebp = ctx->esp;                                                         \
        SG_PUSH32(s->eh4 ? 0xfffffffeu : 0xffffffffu);                                                    \
        SG_PUSH32(s->eh4 ? (s->table ^ sg_ld32(mem, g_cookie_va)) : s->table);                            \
        SG_PUSH32(s->eh4 ? FX_FN(K_EH4LOCAL) : FX_FN(K_EH3THUNK));                                        \
        SG_PUSH32(sg_ld32(mem, SG_FS(ctx, 0))); sg_st32(mem, SG_FS(ctx, 0), ctx->esp);                    \
        ctx->esp -= 0x40u; sg_st32(mem, ctx->ebp - 0x18u, ctx->esp);                                      \
        sg_st32(mem, ctx->ebp - 0x20u, sg_ld32(mem, g_cookie_va) ^ ctx->ebp); /* EH cookie */             \
        s->ebp = ctx->ebp;                                                                                \
        for (i = 0; i < s->nenter; i++) sg_st32(mem, ctx->ebp - 4u, (uint32_t)s->levels[i]);              \
        if (s->callee) { SG_CALL_IND(s->callee, FX_FN(K_SEH_##X) + 0x10u); }                              \
        else { SG_CALL(fx_raiser, FX_FN(K_SEH_##X) + 0x10u); }                                            \
        sg_st32(mem, ctx->ebp - 4u, s->eh4 ? 0xfffffffeu : 0xffffffffu);                                  \
        ctx->eax = 0;                                                                                     \
        goto out;                                                                                         \
    except_body:                                                                                          \
        ctx->esp = sg_ld32(mem, ctx->ebp - 0x18u); /* mov esp, [ebp-18h] */                               \
        fx_log(EV_EXCEPT, ctx->ebp, sg_ld32(mem, ctx->ebp - 4u));                                         \
        sg_st32(mem, ctx->ebp - 4u, s->eh4 ? 0xfffffffeu : 0xffffffffu);                                  \
        ctx->eax = 1;                                                                                     \
    out:                                                                                                  \
        sg_st32(mem, SG_FS(ctx, 0), sg_ld32(mem, ctx->ebp - 0x10u));                                      \
        ctx->esp = ctx->ebp; SG_POP32(ctx->ebp);                                                          \
        SG_LANDING_RET(0);                                                                                \
    SG_RESUME_DISPATCH:                                                                                   \
        switch (ctx->eip) {                                                                               \
        case FX_FN(K_SEH_##X) + CONT_OFF: goto except_body;                                               \
        default: sg_bad_resume(ctx);                                                                      \
        }                                                                                                 \
    }
SEHFN(A, 0)
SEHFN(B, 1)

/* setjmp owner: jmp_buf at [ebp-50h]; resume point = the return address of `call _setjmp3`. */
static uint32_t g_lj_val, g_sj_ebp, g_sj_esp_after;
static void fx_sj(sg_ctx *ctx, uint8_t *mem) {
    SG_LANDING_BEGIN(FX_FN(K_SJ));
    SG_PUSH32(ctx->ebp); ctx->ebp = ctx->esp;
    ctx->esp -= 0x60u;
    SG_PUSH32(ctx->ebx); SG_PUSH32(ctx->esi);
    ctx->ebx = 0xb0b0b0b0u; ctx->esi = 0x51515151u;
    g_sj_ebp = ctx->ebp;
    SG_PUSH32(0);
    SG_PUSH32(ctx->ebp - 0x50u);
    SG_CALL(sgc_msvcr90__setjmp3, FX_FN(K_SJ) + CONT_OFF);
L_after_setjmp:
    ctx->esp += 8u;
    g_sj_esp_after = ctx->esp;
    if (ctx->eax == 0) {
        fx_log(EV_MISC, 0, ctx->esp);
        ctx->ebx = 0; ctx->esi = 0; /* clobbered by the code between setjmp and longjmp */
        SG_CALL_IND(FX_FN(K_LEAF1), FX_FN(K_SJ) + 0x20u);
        ctx->eax = 0xdead;
    } else {
        fx_log(EV_AFTER, ctx->eax, ctx->esp);
        fx_log(EV_MISC, ctx->ebx, ctx->esi);
    }
    SG_POP32(ctx->esi); SG_POP32(ctx->ebx);
    ctx->esp = ctx->ebp; SG_POP32(ctx->ebp);
    SG_LANDING_RET(0);
SG_RESUME_DISPATCH:
    switch (ctx->eip) {
    case FX_FN(K_SJ) + CONT_OFF: goto L_after_setjmp;
    default: sg_bad_resume(ctx);
    }
}
static void fx_lj(sg_ctx *ctx, uint8_t *mem) {
    SG_PUSH32(g_lj_val);
    SG_PUSH32(g_sj_ebp - 0x50u);
    SG_CALL(sgc_msvcr90_longjmp, FX_FN(K_LJ) + 0x10u);
    sg_core_fatal(ctx, "longjmp returned");
}

/* A "shim" that calls guest code while holding a host resource (runtime-abi 10.4). */
static int g_barrier;
static void cleanup_fn(sg_cleanup *c) { (void)c; fx_log(EV_CLEANUP, 0, 0); }
static void fx_shimcb(sg_ctx *ctx, uint8_t *mem) {
    sg_cleanup c;
    memset(&c, 0, sizeof c);
    c.fn = cleanup_fn;
    c.barrier = g_barrier;
    c.what = "fixture DispatchMessage";
    sg_cleanup_push(ctx, &c);
    sg_call_guest(ctx, FX_FN(K_THROW), SG_CC_CDECL, 0, NULL);
    sg_cleanup_pop(ctx, &c);
    SG_RET(0);
}

/* scope tables */
static uint32_t eh3_table(unsigned n, const int32_t *enc, const uint32_t *filt, const uint32_t *hand) {
    uint32_t t = fx_alloc(12 * n), i;
    for (i = 0; i < n; i++) {
        sg_st32(M, t + 12 * i, (uint32_t)enc[i]);
        sg_st32(M, t + 12 * i + 4, filt[i]);
        sg_st32(M, t + 12 * i + 8, hand[i]);
    }
    return t;
}
static uint32_t eh4_table(unsigned n, const int32_t *enc, const uint32_t *filt, const uint32_t *hand) {
    uint32_t t = fx_alloc(16 + 12 * n), i;
    sg_st32(M, t + 0, 0xfffffffeu);         /* GSCookieOffset: none */
    sg_st32(M, t + 4, 0);
    sg_st32(M, t + 8, (uint32_t)-0x20);     /* EHCookieOffset */
    sg_st32(M, t + 12, 0);                  /* EHCookieXOROffset */
    for (i = 0; i < n; i++) {
        sg_st32(M, t + 16 + 12 * i, (uint32_t)enc[i]);
        sg_st32(M, t + 16 + 12 * i + 4, filt[i]);
        sg_st32(M, t + 16 + 12 * i + 8, hand[i]);
    }
    return t;
}

static void reset_case(const char *n) {
    t_name = n;
    fx_log_reset();
    memset(g_seh, 0, sizeof g_seh);
    g_filter_ret[0] = g_filter_ret[1] = 1;
    g_raise_code = 0xe0001234u;
    g_raise_flags = 0;
    g_cookie_checks = g_cookie_bad = 0;
}

#define KINDS(...) ((const int[]){__VA_ARGS__}), (sizeof((const int[]){__VA_ARGS__}) / sizeof(int))
#define CHECK_LOG(...) do { if (!fx_log_kinds(KINDS(__VA_ARGS__))) { CHECK(0, "event log differs:"); fx_log_dump(); } else t_pass++; } while (0)

static uint32_t run(uint32_t entry) {
    uint32_t fs = sg_ld32(M, C->fs_base), esp = C->esp, r;
    r = sg_call_guest(C, entry, SG_CC_CDECL, 0, NULL);
    CHECK_EQ(sg_ld32(M, C->fs_base), fs);
    CHECK_EQ(C->esp, esp);
    CHECK(C->eh_top == NULL, "landing chain not empty after the call");
    return r;
}

void test_seh(void) {
    uint32_t r;
    static int setup;
    if (!setup) {
        setup = 1;
        fx_register(FX_FN(K_EH3THUNK), fx_eh3thunk);
        fx_register(FX_FN(K_EH4LOCAL), fx_eh4local);
        fx_register(FX_FN(K_CHECK), fx_check);
        fx_register(FX_FN(K_FILT0), fx_filt0);
        fx_register(FX_FN(K_FILT1), fx_filt1);
        fx_register(FX_FN(K_FIN0), fx_fin0);
        fx_register(FX_FN(K_FIN1), fx_fin1);
        fx_register(FX_FN(K_RAISER), fx_raiser);
        fx_register(FX_FN(K_SEH_A), fx_sehA);
        fx_register(FX_FN(K_SEH_B), fx_sehB);
        fx_register(FX_FN(K_SJ), fx_sj);
        fx_register(FX_FN(K_LJ), fx_lj);
        fx_register(FX_FN(K_SHIMCB), fx_shimcb);
        fx_resume_point(FX_FN(K_SEH_A) + CONT_OFF, FX_FN(K_SEH_A));
        fx_resume_point(FX_FN(K_SEH_B) + CONT_OFF, FX_FN(K_SEH_B));
        fx_resume_point(FX_FN(K_SJ) + CONT_OFF, FX_FN(K_SJ));
        g_cookie_va = fx_words(1, 0x5a5a1234u);
    }

    /* 1. _except_handler3: filter, then resume in the __except body */
    reset_case("seh.eh3-except");
    g_seh[0].table = eh3_table(1, (const int32_t[]){-1}, (const uint32_t[]){FX_FN(K_FILT0)},
                               (const uint32_t[]){FX_FN(K_SEH_A) + CONT_OFF});
    g_seh[0].nenter = 1; g_seh[0].levels[0] = 0;
    r = run(FX_FN(K_SEH_A));
    CHECK_EQ(r, 1);
    CHECK_LOG(EV_FILTER, EV_EXCEPT);
    if (fx_nlog == 2) {
        CHECK_EQ(fx_log_buf[0].a, g_seh[0].ebp);   /* filter ebp = frame ebp (rn + 10h) */
        CHECK_EQ(fx_log_buf[0].b, 0xe0001234u);    /* GetExceptionCode through [ebp-14h] */
        CHECK_EQ(fx_log_buf[1].a, g_seh[0].ebp);
        CHECK_EQ(fx_log_buf[1].b, 0xffffffffu);    /* trylevel = enclosing level */
    }

    /* 2. EH4: nested __try/__finally inside __try/__except; the local unwind runs the inner
     *    __finally before the __except body; cookies are checked through the guest function */
    reset_case("seh.eh4-nested");
    g_seh[0].eh4 = 1;
    g_seh[0].table = eh4_table(2, (const int32_t[]){-2, 0}, (const uint32_t[]){FX_FN(K_FILT0), 0},
                               (const uint32_t[]){FX_FN(K_SEH_A) + CONT_OFF, FX_FN(K_FIN1)});
    g_seh[0].nenter = 2; g_seh[0].levels[0] = 0; g_seh[0].levels[1] = 1;
    r = run(FX_FN(K_SEH_A));
    CHECK_EQ(r, 1);
    CHECK_LOG(EV_FILTER, EV_FINALLY, EV_EXCEPT);
    if (fx_nlog == 3) {
        CHECK_EQ(fx_log_buf[1].a, g_seh[0].ebp);
        CHECK_EQ(fx_log_buf[2].b, 0xfffffffeu);
    }
    CHECK(g_cookie_checks >= 2, "EH cookie checked %u times", g_cookie_checks);
    CHECK_EQ(g_cookie_bad, 0);

    /* 3. a C++ throw passes an EH3 filter that declines and an EH4 __finally (model test 11) */
    reset_case("seh.cxx-through-seh");
    {
        extern void test_eh_prepare_catcher(uint32_t callee);
        test_eh_prepare_catcher(FX_FN(K_SEH_A));
    }
    g_filter_ret[0] = 0;
    g_seh[0].table = eh3_table(1, (const int32_t[]){-1}, (const uint32_t[]){FX_FN(K_FILT0)},
                               (const uint32_t[]){FX_FN(K_SEH_A) + CONT_OFF});
    g_seh[0].nenter = 1; g_seh[0].levels[0] = 0; g_seh[0].callee = FX_FN(K_SEH_B);
    g_seh[1].eh4 = 1;
    g_seh[1].table = eh4_table(1, (const int32_t[]){-2}, (const uint32_t[]){0}, (const uint32_t[]){FX_FN(K_FIN0)});
    g_seh[1].nenter = 1; g_seh[1].levels[0] = 0; g_seh[1].callee = FX_FN(K_THROW);
    r = run(FX_FN(K_CATCH_A));
    CHECK_EQ(r, 1);
    CHECK_LOG(EV_THROW, EV_FILTER, EV_FINALLY, EV_CATCH, EV_DTOR, EV_AFTER);
    if (fx_nlog == 6) {
        CHECK_EQ(fx_log_buf[1].b, 0xe06d7363u);
        CHECK_EQ(fx_log_buf[2].a, g_seh[1].ebp);
    }
    CHECK_EQ(g_cookie_bad, 0);

    /* 4. a continuable exception and a filter returning -1: RaiseException returns */
    reset_case("seh.continue-execution");
    g_filter_ret[0] = -1;
    g_seh[0].table = eh3_table(1, (const int32_t[]){-1}, (const uint32_t[]){FX_FN(K_FILT0)},
                               (const uint32_t[]){FX_FN(K_SEH_A) + CONT_OFF});
    g_seh[0].nenter = 1; g_seh[0].levels[0] = 0;
    r = run(FX_FN(K_SEH_A));
    CHECK_EQ(r, 0);
    CHECK_LOG(EV_FILTER, EV_MISC);

    /* 5. a noncontinuable exception and a filter returning -1: fatal */
    reset_case("seh.noncontinuable");
    g_filter_ret[0] = -1;
    g_raise_flags = 1;
    g_seh[0].table = eh3_table(1, (const int32_t[]){-1}, (const uint32_t[]){FX_FN(K_FILT0)},
                               (const uint32_t[]){FX_FN(K_SEH_A) + CONT_OFF});
    g_seh[0].nenter = 1; g_seh[0].levels[0] = 0;
    CHECK(EXPECT_FATAL(sg_call_guest(C, FX_FN(K_SEH_A), SG_CC_CDECL, 0, NULL)), "noncontinuable continue not fatal");

    /* 6. SetThreadName (0x406D1388) is swallowed and names the thread */
    reset_case("seh.set-thread-name");
    {
        uint32_t info = fx_words(4, 0x1000u, fx_str("fixture-main"), 0xffffffffu, 0u);
        uint32_t ret = GSTD(sgc_kernel32_RaiseException, 0x406d1388u, 0u, 4u, info);
        (void)ret;
        CHECK(!strcmp(sg_core_thread_name(sg_self), "fixture-main"), "name %s", sg_core_thread_name(sg_self));
    }

    /* 7. setjmp/longjmp across a frame with destructors; longjmp(buf, 0) returns 1 */
    {
        extern void test_eh_prepare_leaf(uint32_t callee, int nstates);
        int pass;
        for (pass = 0; pass < 2; pass++) {
            reset_case(pass ? "seh.longjmp-7" : "seh.longjmp-0");
            g_lj_val = pass ? 7 : 0;
            test_eh_prepare_leaf(FX_FN(K_LJ), 2);
            r = run(FX_FN(K_SJ));
            CHECK_EQ(r, pass ? 7 : 1);
            CHECK_LOG(EV_MISC, EV_UNWIND, EV_UNWIND, EV_AFTER, EV_MISC);
            if (fx_nlog == 5) {
                CHECK_EQ(fx_log_buf[1].a, 1);
                CHECK_EQ(fx_log_buf[2].a, 0);
                CHECK_EQ(fx_log_buf[3].b, fx_log_buf[0].b);   /* esp restored */
                CHECK_EQ(fx_log_buf[4].a, 0xb0b0b0b0u);       /* ebx, esi restored */
                CHECK_EQ(fx_log_buf[4].b, 0x51515151u);
            }
        }
    }

    /* 8. a host cleanup record between the throw and the catch runs at the resume */
    reset_case("seh.cleanup-record");
    {
        extern void test_eh_prepare_catcher(uint32_t callee);
        test_eh_prepare_catcher(FX_FN(K_SHIMCB));
        g_barrier = 0;
        r = run(FX_FN(K_CATCH_A));
        CHECK_EQ(r, 1);
        CHECK_LOG(EV_THROW, EV_CATCH, EV_DTOR, EV_CLEANUP, EV_AFTER);
        CHECK(core_th(sg_self)->cleanup == NULL, "cleanup chain not empty");
        /* 8b. ... unless it is a barrier: unwinding through it is fatal */
        reset_case("seh.barrier");
        test_eh_prepare_catcher(FX_FN(K_SHIMCB));
        g_barrier = 1;
        CHECK(EXPECT_FATAL(sg_call_guest(C, FX_FN(K_CATCH_A), SG_CC_CDECL, 0, NULL)), "barrier not fatal");
        CHECK(strstr(fx_fatal_msg, "barrier") != NULL, "message: %s", fx_fatal_msg);
        g_barrier = 0;
    }

    /* 9. sg_eh_resume to an address that is not a resume point, or with no live landing */
    reset_case("seh.bad-resume");
    CHECK(EXPECT_FATAL(sg_eh_resume(C, 0x00412345u)), "non-resume target not fatal");
    CHECK(EXPECT_FATAL(sg_eh_resume(C, FX_FN(K_SEH_A) + CONT_OFF)), "resume without a landing not fatal");
    CHECK(strstr(fx_fatal_msg, "no live landing") != NULL, "message: %s", fx_fatal_msg);

    /* 10. a corrupted EH cookie is passed to the guest check function as a mismatch */
    reset_case("seh.cookie-mismatch");
    g_seh[0].eh4 = 1;
    g_seh[0].table = eh4_table(1, (const int32_t[]){-2}, (const uint32_t[]){FX_FN(K_FILT0)},
                               (const uint32_t[]){FX_FN(K_SEH_A) + CONT_OFF});
    g_seh[0].nenter = 1; g_seh[0].levels[0] = 0;
    g_corrupt = 1;
    r = run(FX_FN(K_SEH_A));
    g_corrupt = 0;
    CHECK_EQ(r, 1);
    CHECK_EQ(g_cookie_bad, 1); /* the check after the filter sees the smashed cookie */
}
