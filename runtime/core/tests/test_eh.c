/* test_eh.c -- C++ exception fixtures: __CxxFrameHandler3, _CxxThrowException, the catch guard,
 * unwinding, landings. Mirrors tools/eh/test_fh3_model.py against the real runtime.
 *
 * Fixture "translated functions" follow the MSVC x86 frame shapes:
 *   FH3 frame:  push ebp; mov ebp,esp; push -1; push stub; push fs:[0]; mov fs:[0],esp;
 *               sub esp,N; mov [ebp-10h],esp        rn = ebp-0Ch, state at [ebp-4]
 * and the translator's emission pattern for landing owners (SG_LANDING_BEGIN ... SG_RESUME_DISPATCH). */
#include "core_internal.h"
#include "fixture.h"

SGC_SHIM(msvcr90, __CxxFrameHandler3);
SGC_SHIM(msvcr90, _CxxThrowException);

/* ---- addresses ---------------------------------------------------------------------------------- */
enum {
    K_DTOR = 1, K_COPY, K_THROW, K_STUB0, K_STUB1, K_STUB2, K_STUB3, K_STUB4, K_STUB5, K_STUB6, K_STUB7,
    K_CATCH_A = 20, K_CATCH_B, K_CATCH_C, K_CFN_A = 30, K_CFN_B, K_CFN_C, K_LEAF1 = 40, K_LEAF2, K_LEAF3,
    K_UNW0 = 50, K_REC = 70, K_RECFN, K_COPY2
};
#define CONT_OFF 0x80u

/* ---- shared fixture state ------------------------------------------------------------------------- */
static uint32_t g_ti_err, g_ti_other, g_ti_err_byval, g_td_err, g_td_other;
static uint32_t g_throw_ti, g_throw_val;
static uint32_t stub_fi[8];

struct catcher {
    uint32_t callee;   /* guest function called inside the try (0: throw inline) */
    uint32_t fi;       /* FuncInfo */
    int32_t disp;      /* dispCatchObj of its handler */
    int mode;          /* catch funclet: 0 return, 1 rethrow, 2 throw a new object */
    uint32_t new_ti, new_val;
    uint32_t ebp;      /* frame ebp seen at entry */
};
static struct catcher g_cat[3];
struct leaf { uint32_t callee; int nstates; uint32_t ebp; };
static struct leaf g_leaf[3];

/* ---- exception class: dtor and copy constructor (thiscall) ---------------------------------------- */
static void fx_dtor(sg_ctx *ctx, uint8_t *mem) {
    fx_log(EV_DTOR, ctx->ecx, sg_ld32(mem, ctx->ecx));
    SG_RET(0);
}
static void fx_copy(sg_ctx *ctx, uint8_t *mem) {
    uint32_t dest = ctx->ecx, src = SG_ARG(0);
    memmove(mem + dest, mem + src, 8);
    fx_log(EV_COPY, dest, src);
    SG_RET(4);
}
static void fx_copy_vb(sg_ctx *ctx, uint8_t *mem) { /* copy constructor of a class with a virtual base */
    uint32_t dest = ctx->ecx, src = SG_ARG(0);
    memmove(mem + dest, mem + src, 8);
    fx_log(EV_COPY, dest, SG_ARG(1));
    SG_RET(8);
}

/* A function that builds an exception object in its own frame and throws it (cdecl, no args). */
static void fx_thrower(sg_ctx *ctx, uint8_t *mem) {
    uint32_t obj;
    SG_PUSH32(ctx->ebp);
    ctx->ebp = ctx->esp;
    ctx->esp -= 16u;
    obj = ctx->esp;
    sg_st32(mem, obj, g_throw_val);
    sg_st32(mem, obj + 4, 0xabcd0000u);
    fx_log(EV_THROW, obj, g_throw_val);
    SG_PUSH32(g_throw_ti);
    SG_PUSH32(obj);
    SG_CALL(sgc_msvcr90__CxxThrowException, FX_FN(K_THROW) + 0x20u);
    sg_core_fatal(ctx, "_CxxThrowException returned");
}

/* Handler stubs: mov eax, FuncInfo; jmp __CxxFrameHandler3 */
#define STUB(i) static void fx_stub##i(sg_ctx *ctx, uint8_t *mem) { ctx->eax = stub_fi[i]; SG_TAILCALL(sgc_msvcr90___CxxFrameHandler3); }
STUB(0) STUB(1) STUB(2) STUB(3) STUB(4) STUB(5) STUB(6) STUB(7)

/* Unwind funclets: log their index and the frame ebp they were given. */
static int32_t g_unw_state[8]; /* the frame's state while each unwind funclet runs */
#define UNW(i) static void fx_unw##i(sg_ctx *ctx, uint8_t *mem) { \
        g_unw_state[i] = (int32_t)sg_ld32(mem, ctx->ebp - 4u); fx_log(EV_UNWIND, i, ctx->ebp); SG_RET(0); }
UNW(0) UNW(1) UNW(2) UNW(3) UNW(4) UNW(5) UNW(6) UNW(7)

#define PROLOG_FH3(stub, locals) do { \
        SG_PUSH32(ctx->ebp); ctx->ebp = ctx->esp; SG_PUSH32(0xffffffffu); SG_PUSH32(stub); \
        SG_PUSH32(sg_ld32(mem, SG_FS(ctx, 0))); sg_st32(mem, SG_FS(ctx, 0), ctx->esp); \
        ctx->esp -= (locals); sg_st32(mem, ctx->ebp - 0x10u, ctx->esp); } while (0)
#define STATE(s) sg_st32(mem, ctx->ebp - 4u, (uint32_t)(int32_t)(s))
#define EPILOG_FH3() do { sg_st32(mem, SG_FS(ctx, 0), sg_ld32(mem, ctx->ebp - 0xcu)); ctx->esp = ctx->ebp; \
        SG_POP32(ctx->ebp); } while (0)

/* ---- catchers: one try block (state 0) around a call; landing owners --------------------------------- */
#define CATCHER(X, k, stubk)                                                                              \
    static void fx_catcher##X(sg_ctx *ctx, uint8_t *mem) {                                                \
        SG_LANDING_BEGIN(FX_FN(K_CATCH_##X));                                                             \
        PROLOG_FH3(FX_FN(stubk), 0x40u);                                                                  \
        g_cat[k].ebp = ctx->ebp;                                                                          \
        sg_st32(mem, ctx->ebp - 0x14u, 0xc0de0000u | (k));                                                \
        STATE(0);                                                                                         \
        if (g_cat[k].callee) {                                                                            \
            SG_CALL_IND(g_cat[k].callee, FX_FN(K_CATCH_##X) + 0x10u);                                     \
        } else {                                                                                          \
            uint32_t obj = ctx->ebp - 0x30u;                                                              \
            sg_st32(mem, obj, g_throw_val);                                                               \
            fx_log(EV_THROW, obj, g_throw_val);                                                           \
            SG_PUSH32(g_throw_ti);                                                                        \
            SG_PUSH32(obj);                                                                               \
            SG_CALL(sgc_msvcr90__CxxThrowException, FX_FN(K_CATCH_##X) + 0x20u);                          \
        }                                                                                                 \
        STATE(-1);                                                                                        \
        fx_log(EV_MISC, ctx->ebp, 0);                                                                     \
        ctx->eax = 0;                                                                                     \
        goto out;                                                                                         \
    cont:                                                                                                 \
        STATE(-1);                                                                                        \
        fx_log(EV_AFTER, ctx->ebp, ctx->esp);                                                             \
        ctx->eax = 1;                                                                                     \
    out:                                                                                                  \
        EPILOG_FH3();                                                                                     \
        SG_LANDING_RET(0);                                                                                \
    SG_RESUME_DISPATCH:                                                                                   \
        switch (ctx->eip) {                                                                               \
        case FX_FN(K_CATCH_##X) + CONT_OFF: goto cont;                                                    \
        default: sg_bad_resume(ctx);                                                                      \
        }                                                                                                 \
    }
CATCHER(A, 0, K_STUB0)
CATCHER(B, 1, K_STUB1)
CATCHER(C, 2, K_STUB2)

/* Catch funclets: ebp = the catcher's frame. */
static void catch_body(sg_ctx *ctx, uint8_t *mem, int k, uint32_t cont) {
    struct catcher *c = &g_cat[k];
    uint32_t slot = sg_ld32(mem, ctx->ebp + (uint32_t)c->disp);
    fx_log(EV_CATCH, ctx->ebp, slot);
    if (c->mode == 1) {
        fx_log(EV_RETHROW, 0, 0);
        SG_PUSH32(0);
        SG_PUSH32(0);
        SG_CALL(sgc_msvcr90__CxxThrowException, FX_FN(K_CFN_A + k) + 0x10u);
    } else if (c->mode == 2) {
        STATE(2); /* a local of the catch block is constructed */
        g_throw_ti = c->new_ti;
        g_throw_val = c->new_val;
        SG_CALL(fx_thrower, FX_FN(K_CFN_A + k) + 0x20u);
    }
    ctx->eax = cont;
}
static void fx_cfnA(sg_ctx *ctx, uint8_t *mem) { catch_body(ctx, mem, 0, FX_FN(K_CATCH_A) + CONT_OFF); SG_RET(0); }
static void fx_cfnB(sg_ctx *ctx, uint8_t *mem) { catch_body(ctx, mem, 1, FX_FN(K_CATCH_B) + CONT_OFF); SG_RET(0); }
static void fx_cfnC(sg_ctx *ctx, uint8_t *mem) { catch_body(ctx, mem, 2, FX_FN(K_CATCH_C) + CONT_OFF); SG_RET(0); }

/* ---- leaf frames: FH3 frames with no try block, one state per object ------------------------------- */
#define LEAF(X, k, stubk)                                                                                 \
    static void fx_leaf##X(sg_ctx *ctx, uint8_t *mem) {                                                   \
        int i;                                                                                            \
        PROLOG_FH3(FX_FN(stubk), 0x20u);                                                                  \
        g_leaf[k].ebp = ctx->ebp;                                                                         \
        for (i = 0; i < g_leaf[k].nstates; i++) STATE(i);                                                 \
        SG_CALL_IND(g_leaf[k].callee, FX_FN(K_LEAF1 + k) + 0x10u);                                        \
        STATE(-1);                                                                                        \
        EPILOG_FH3();                                                                                     \
        SG_RET(0);                                                                                        \
    }
LEAF(1, 0, K_STUB3)
LEAF(2, 1, K_STUB4)
LEAF(3, 2, K_STUB5)

/* ---- recursion: R(n) catches in every instance with n > 0; R(0) throws outside its try --------------- */
static void fx_rec(sg_ctx *ctx, uint8_t *mem) {
    uint32_t n;
    SG_LANDING_BEGIN(FX_FN(K_REC));
    n = SG_ARG(0);
    PROLOG_FH3(FX_FN(K_STUB6), 0x40u);
    sg_st32(mem, ctx->ebp - 0x14u, n);
    if (n == 0) {
        g_throw_ti = g_ti_err;
        g_throw_val = 0x777;
        SG_CALL(fx_thrower, FX_FN(K_REC) + 0x08u);
    }
    STATE(0);
    SG_PUSH32(n - 1u);
    SG_CALL(fx_rec, FX_FN(K_REC) + 0x10u);
    ctx->esp += 4u;
    STATE(-1);
    fx_log(EV_MISC, ctx->ebp, sg_ld32(mem, ctx->ebp - 0x14u));
    goto out;
cont:
    STATE(-1);
    fx_log(EV_AFTER, ctx->ebp, sg_ld32(mem, ctx->ebp - 0x14u));
out:
    ctx->eax = sg_ld32(mem, ctx->ebp - 0x14u) + 100u;
    EPILOG_FH3();
    SG_LANDING_RET(0);
SG_RESUME_DISPATCH:
    switch (ctx->eip) {
    case FX_FN(K_REC) + CONT_OFF: goto cont;
    default: sg_bad_resume(ctx);
    }
}
static void fx_recfn(sg_ctx *ctx, uint8_t *mem) {
    fx_log(EV_CATCH, ctx->ebp, sg_ld32(mem, ctx->ebp - 0x14u));
    ctx->eax = FX_FN(K_REC) + CONT_OFF;
    SG_RET(0);
}

/* ---- table builders ------------------------------------------------------------------------------------ */
static uint32_t typedesc(const char *name) {
    uint32_t n = (uint32_t)strlen(name) + 1, a = fx_alloc(8 + n);
    sg_st32(M, a, 0x7e1f0000u);
    memcpy(M + a + 8, name, n);
    return a;
}
static uint32_t throwinfo(uint32_t td, uint32_t props, uint32_t copy) {
    uint32_t ct = fx_words(7, props, td, 0u, 0xffffffffu, 0u, 8u, copy);
    uint32_t cta = fx_words(2, 1u, ct);
    return fx_words(4, 0u, FX_FN(K_DTOR), 0u, cta);
}
/* unwind: n entries of {toState, action}; one try block {lo, hi, chi} with one handler. */
static uint32_t funcinfo(unsigned n, const int32_t *to, const uint32_t *act, int ntry, int32_t lo, int32_t hi,
                         int32_t chi, uint32_t adj, uint32_t td, int32_t disp, uint32_t handler) {
    uint32_t umap = n ? fx_alloc(8 * n) : 0, ptry = 0, i;
    for (i = 0; i < n; i++) {
        sg_st32(M, umap + 8 * i, (uint32_t)to[i]);
        sg_st32(M, umap + 8 * i + 4, act[i]);
    }
    if (ntry) {
        uint32_t ht = fx_words(4, adj, td, (uint32_t)disp, handler);
        ptry = fx_words(5, (uint32_t)lo, (uint32_t)hi, (uint32_t)chi, 1u, ht);
    }
    return fx_words(9, 0x19930522u, n, umap, (uint32_t)ntry, ptry, 0u, 0u, 0u, 1u);
}
static uint32_t catcher_fi(int k, uint32_t adj, uint32_t td, int32_t disp) {
    static const int32_t to[3] = {-1, -1, 1};
    uint32_t act[3] = {0, 0, FX_FN(K_UNW0 + 5)};
    g_cat[k].disp = disp;
    return funcinfo(3, to, act, 1, 0, 0, 2, adj, td, disp, FX_FN(K_CFN_A + k));
}
static uint32_t leaf_fi(int nstates, int first_unw) {
    int32_t to[8];
    uint32_t act[8];
    int i;
    for (i = 0; i < nstates; i++) {
        to[i] = i - 1;
        act[i] = FX_FN(K_UNW0 + first_unw + i);
    }
    return funcinfo((unsigned)nstates, to, act, 0, 0, 0, 0, 0, 0, 0, 0);
}

static void setup(void) {
    static int done;
    unsigned i;
    if (done) return;
    done = 1;
    fx_register(FX_FN(K_DTOR), fx_dtor);
    fx_register(FX_FN(K_COPY), fx_copy);
    fx_register(FX_FN(K_COPY2), fx_copy_vb);
    fx_register(FX_FN(K_THROW), fx_thrower);
    {
        static sg_func *const stubs[8] = {fx_stub0, fx_stub1, fx_stub2, fx_stub3, fx_stub4, fx_stub5, fx_stub6, fx_stub7};
        static sg_func *const unws[8] = {fx_unw0, fx_unw1, fx_unw2, fx_unw3, fx_unw4, fx_unw5, fx_unw6, fx_unw7};
        for (i = 0; i < 8; i++) {
            fx_register(FX_FN(K_STUB0 + i), stubs[i]);
            fx_register(FX_FN(K_UNW0 + i), unws[i]);
        }
    }
    fx_register(FX_FN(K_CATCH_A), fx_catcherA);
    fx_register(FX_FN(K_CATCH_B), fx_catcherB);
    fx_register(FX_FN(K_CATCH_C), fx_catcherC);
    fx_register(FX_FN(K_CFN_A), fx_cfnA);
    fx_register(FX_FN(K_CFN_B), fx_cfnB);
    fx_register(FX_FN(K_CFN_C), fx_cfnC);
    fx_register(FX_FN(K_LEAF1), fx_leaf1);
    fx_register(FX_FN(K_LEAF2), fx_leaf2);
    fx_register(FX_FN(K_LEAF3), fx_leaf3);
    fx_register(FX_FN(K_REC), fx_rec);
    fx_register(FX_FN(K_RECFN), fx_recfn);
    fx_resume_point(FX_FN(K_CATCH_A) + CONT_OFF, FX_FN(K_CATCH_A));
    fx_resume_point(FX_FN(K_CATCH_B) + CONT_OFF, FX_FN(K_CATCH_B));
    fx_resume_point(FX_FN(K_CATCH_C) + CONT_OFF, FX_FN(K_CATCH_C));
    fx_resume_point(FX_FN(K_REC) + CONT_OFF, FX_FN(K_REC));
    g_td_err = typedesc(".?AVcError@Fixture@@");
    g_td_other = typedesc(".?AVcOther@Fixture@@");
    g_ti_err = throwinfo(g_td_err, 0, FX_FN(K_COPY));
    g_ti_other = throwinfo(g_td_other, 0, FX_FN(K_COPY));
    g_ti_err_byval = throwinfo(typedesc(".?AVcError@Fixture@@"), 4u /* CT_HasVirtualBase */, FX_FN(K_COPY2));
}

/* Runs `entry` from the host as a cdecl call and checks the thread's EH state afterwards. */
static uint32_t run(uint32_t entry) {
    uint32_t fs = sg_ld32(M, C->fs_base), esp = C->esp, r;
    r = sg_call_guest(C, entry, SG_CC_CDECL, 0, NULL);
    CHECK_EQ(sg_ld32(M, C->fs_base), fs);
    CHECK_EQ(C->esp, esp);
    CHECK(C->eh_top == NULL, "landing chain not empty after the call");
    CHECK_EQ(core_th(sg_self)->ninflight, 0);
    return r;
}

static void reset_case(const char *name) {
    t_name = name;
    fx_log_reset();
    memset(g_cat, 0, sizeof g_cat);
    memset(g_leaf, 0, sizeof g_leaf);
    g_throw_ti = g_ti_err;
    g_throw_val = 0x1234;
}

#define KINDS(...) ((const int[]){__VA_ARGS__}), (sizeof((const int[]){__VA_ARGS__}) / sizeof(int))
#define CHECK_LOG(...) do { if (!fx_log_kinds(KINDS(__VA_ARGS__))) { CHECK(0, "event log differs:"); fx_log_dump(); } else t_pass++; } while (0)

/* used by test_seh.c */
void test_eh_prepare_catcher(uint32_t callee) {
    setup();
    memset(g_cat, 0, sizeof g_cat);
    stub_fi[0] = catcher_fi(0, 8u | 1u, g_td_err, -0x20);
    g_cat[0].callee = callee;
    g_throw_ti = g_ti_err;
    g_throw_val = 0x1234;
}
void test_eh_prepare_leaf(uint32_t callee, int nstates) {
    setup();
    g_leaf[0].callee = callee;
    g_leaf[0].nstates = nstates;
    stub_fi[3] = leaf_fi(nstates, 0);
}

void test_eh(void) {
    uint32_t r;
    setup();

    /* 1. catch in the same function (const cError&) */
    reset_case("eh.same-function");
    stub_fi[0] = g_cat[0].fi = catcher_fi(0, 8u | 1u, g_td_err, -0x20);
    r = run(FX_FN(K_CATCH_A));
    CHECK_EQ(r, 1);
    CHECK_LOG(EV_THROW, EV_CATCH, EV_DTOR, EV_AFTER);
    CHECK_EQ(fx_log_buf[1].a, g_cat[0].ebp);              /* funclet ebp = parent frame */
    CHECK_EQ(fx_log_buf[1].b, fx_log_buf[0].a);           /* reference to the thrown object */
    CHECK_EQ(fx_log_buf[2].a, fx_log_buf[0].a);           /* that object destroyed after the catch */
    CHECK_EQ(fx_log_buf[3].b, g_cat[0].ebp - 0x4cu); /* esp = [ebp-10h] */

    /* 2. unwinding three intermediate frames, innermost first */
    reset_case("eh.three-frames");
    stub_fi[0] = catcher_fi(0, 8u | 1u, g_td_err, -0x20);
    g_cat[0].callee = FX_FN(K_LEAF1);
    g_leaf[0].callee = FX_FN(K_LEAF2); g_leaf[0].nstates = 2; stub_fi[3] = leaf_fi(2, 0);
    g_leaf[1].callee = FX_FN(K_LEAF3); g_leaf[1].nstates = 1; stub_fi[4] = leaf_fi(1, 2);
    g_leaf[2].callee = FX_FN(K_THROW); g_leaf[2].nstates = 1; stub_fi[5] = leaf_fi(1, 3);
    r = run(FX_FN(K_CATCH_A));
    CHECK_EQ(r, 1);
    CHECK_LOG(EV_THROW, EV_UNWIND, EV_UNWIND, EV_UNWIND, EV_UNWIND, EV_CATCH, EV_DTOR, EV_AFTER);
    if (fx_nlog == 8) {
        CHECK_EQ(fx_log_buf[1].a, 3); CHECK_EQ(fx_log_buf[1].b, g_leaf[2].ebp);
        CHECK_EQ(fx_log_buf[2].a, 2); CHECK_EQ(fx_log_buf[2].b, g_leaf[1].ebp);
        CHECK_EQ(fx_log_buf[3].a, 1); CHECK_EQ(fx_log_buf[3].b, g_leaf[0].ebp);
        CHECK_EQ(fx_log_buf[4].a, 0); CHECK_EQ(fx_log_buf[4].b, g_leaf[0].ebp);
        /* MSVC stores the next state before calling each action */
        CHECK_EQ(g_unw_state[3], -1); CHECK_EQ(g_unw_state[2], -1);
        CHECK_EQ(g_unw_state[1], 0); CHECK_EQ(g_unw_state[0], -1);
    }

    /* 3. a type mismatch passes to the outer handler; the inner catcher's frame is unwound */
    reset_case("eh.mismatch");
    stub_fi[0] = catcher_fi(0, 8u | 1u, g_td_err, -0x20);
    stub_fi[1] = catcher_fi(1, 8u | 1u, g_td_other, -0x20);
    g_cat[0].callee = FX_FN(K_CATCH_B);
    g_cat[1].callee = FX_FN(K_LEAF2);
    g_leaf[1].callee = FX_FN(K_THROW); g_leaf[1].nstates = 1; stub_fi[4] = leaf_fi(1, 2);
    r = run(FX_FN(K_CATCH_A));
    CHECK_EQ(r, 1);
    CHECK_LOG(EV_THROW, EV_UNWIND, EV_CATCH, EV_DTOR, EV_AFTER);
    CHECK_EQ(fx_log_buf[2].a, g_cat[0].ebp);

    /* 4. catch (...) catches another type */
    reset_case("eh.catch-all");
    stub_fi[0] = catcher_fi(0, 0, 0, -0x20);
    g_cat[0].callee = FX_FN(K_THROW);
    g_throw_ti = g_ti_other;
    r = run(FX_FN(K_CATCH_A));
    CHECK_EQ(r, 1);
    CHECK_LOG(EV_THROW, EV_CATCH, EV_DTOR, EV_AFTER);

    /* 5. catch by value: the copy constructor builds the catch object in the catcher's frame */
    reset_case("eh.by-value");
    stub_fi[0] = catcher_fi(0, 0, g_td_err, -0x24);
    g_cat[0].callee = FX_FN(K_THROW);
    g_throw_val = 0x4242;
    r = run(FX_FN(K_CATCH_A));
    CHECK_EQ(r, 1);
    CHECK_LOG(EV_THROW, EV_COPY, EV_CATCH, EV_DTOR, EV_AFTER);
    if (fx_nlog == 5) {
        CHECK_EQ(fx_log_buf[1].a, g_cat[0].ebp - 0x24u); /* dest = rn + 0x0C + disp */
        CHECK_EQ(fx_log_buf[1].b, fx_log_buf[0].a);      /* src = the thrown object */
        CHECK_EQ(fx_log_buf[2].b, 0x4242);               /* catch object holds the copied value */
    }
    /* 5b. a class with a virtual base: the copy constructor gets the extra "1" argument */
    reset_case("eh.by-value-vbase");
    stub_fi[0] = catcher_fi(0, 0, g_td_err, -0x24);
    g_cat[0].callee = FX_FN(K_THROW);
    g_throw_ti = g_ti_err_byval;
    r = run(FX_FN(K_CATCH_A));
    CHECK_EQ(r, 1);
    CHECK_LOG(EV_THROW, EV_COPY, EV_CATCH, EV_DTOR, EV_AFTER);
    if (fx_nlog == 5) CHECK_EQ(fx_log_buf[1].b, 1);

    /* 6. a new throw from inside a catch block: each object destroyed exactly once, MSVC order */
    reset_case("eh.throw-from-catch");
    stub_fi[0] = catcher_fi(0, 8u | 1u, g_td_err, -0x20);
    stub_fi[1] = catcher_fi(1, 8u | 1u, g_td_err, -0x20);
    g_cat[0].callee = FX_FN(K_CATCH_B);
    g_cat[1].callee = FX_FN(K_THROW);
    g_cat[1].mode = 2; g_cat[1].new_ti = g_ti_err; g_cat[1].new_val = 0x5678;
    r = run(FX_FN(K_CATCH_A));
    CHECK_EQ(r, 1);
    /* X thrown, B catches, Y thrown, guard destroys X, B's catch local, A catches Y, Y destroyed */
    CHECK_LOG(EV_THROW, EV_CATCH, EV_THROW, EV_DTOR, EV_UNWIND, EV_CATCH, EV_DTOR, EV_AFTER);
    if (fx_nlog == 8) {
        CHECK_EQ(fx_log_buf[3].a, fx_log_buf[0].a);  /* X */
        CHECK_EQ(fx_log_buf[4].a, 5);                /* B's catch-scope local */
        CHECK_EQ(fx_log_buf[4].b, g_cat[1].ebp);
        CHECK_EQ(g_unw_state[5], 1);
        CHECK_EQ(fx_log_buf[5].a, g_cat[0].ebp);
        CHECK_EQ(fx_log_buf[5].b, fx_log_buf[2].a);  /* A catches Y by reference */
        CHECK_EQ(fx_log_buf[6].a, fx_log_buf[2].a);  /* Y destroyed after A's catch */
    }

    /* 7. rethrow: the same object reaches the outer catch and is destroyed once */
    reset_case("eh.rethrow");
    stub_fi[0] = catcher_fi(0, 8u | 1u, g_td_err, -0x20);
    stub_fi[1] = catcher_fi(1, 8u | 1u, g_td_err, -0x20);
    g_cat[0].callee = FX_FN(K_CATCH_B);
    g_cat[1].callee = FX_FN(K_THROW);
    g_cat[1].mode = 1;
    r = run(FX_FN(K_CATCH_A));
    CHECK_EQ(r, 1);
    CHECK_LOG(EV_THROW, EV_CATCH, EV_RETHROW, EV_CATCH, EV_DTOR, EV_AFTER);
    if (fx_nlog == 6) {
        CHECK_EQ(fx_log_buf[3].b, fx_log_buf[0].a);
        CHECK_EQ(fx_log_buf[4].a, fx_log_buf[0].a);
    }

    /* 8. an unhandled exception runs no destructors and is reported */
    reset_case("eh.unhandled");
    g_leaf[0].callee = FX_FN(K_THROW); g_leaf[0].nstates = 2; stub_fi[3] = leaf_fi(2, 0);
    {
        int fatal = EXPECT_FATAL(sg_call_guest(C, FX_FN(K_LEAF1), SG_CC_CDECL, 0, NULL));
        CHECK(fatal, "unhandled exception did not reach sg_core_fatal");
        CHECK(strstr(fx_fatal_msg, "unhandled guest exception 0xe06d7363") != NULL, "message: %s", fx_fatal_msg);
        C->eh_top = NULL;
    }
    CHECK_LOG(EV_THROW);

    /* 9. recursion: the catch resumes in the right instance (R(1) of R(3)->R(2)->R(1)->R(0)) */
    reset_case("eh.recursion");
    stub_fi[6] = funcinfo(1, (const int32_t[]){-1}, (const uint32_t[]){0}, 1, 0, 0, 0, 8u | 1u, g_td_err, -0x20,
                          FX_FN(K_RECFN));
    {
        uint32_t arg = 3;
        uint32_t fs = sg_ld32(M, C->fs_base), esp = C->esp;
        r = sg_call_guest(C, FX_FN(K_REC), SG_CC_CDECL, 1, &arg);
        CHECK_EQ(r, 103);
        CHECK_EQ(sg_ld32(M, C->fs_base), fs);
        CHECK_EQ(C->esp, esp);
        CHECK(C->eh_top == NULL, "landing chain not empty");
    }
    CHECK_LOG(EV_THROW, EV_CATCH, EV_DTOR, EV_AFTER, EV_MISC, EV_MISC);
    if (fx_nlog == 6) {
        CHECK_EQ(fx_log_buf[1].b, 1);     /* caught in R(1) */
        CHECK_EQ(fx_log_buf[3].b, 1);     /* resumed in R(1)'s frame */
        CHECK_EQ(fx_log_buf[1].a, fx_log_buf[3].a);
        CHECK_EQ(fx_log_buf[4].b, 2);     /* R(2) and R(3) return normally */
        CHECK_EQ(fx_log_buf[5].b, 3);
    }

    /* 10. the same catcher used twice in a row and nested in another thread's absence: state reset */
    reset_case("eh.repeat");
    stub_fi[0] = catcher_fi(0, 8u | 1u, g_td_err, -0x20);
    g_cat[0].callee = FX_FN(K_THROW);
    {
        int i, ok = 1;
        for (i = 0; i < 1000; i++) ok &= run(FX_FN(K_CATCH_A)) == 1;
        CHECK(ok, "repeated catch failed");
        CHECK_EQ(fx_nlog, 256); /* log saturates; the point is no leak of landings/in-flight entries */
    }

    /* 11. no handler matches and a const-qualified throw is refused by a non-const handler */
    reset_case("eh.const-mismatch");
    {
        uint32_t ti_const = throwinfo(g_td_err, 0, FX_FN(K_COPY));
        sg_st32(M, ti_const, 1u); /* TI_IsConst */
        stub_fi[0] = catcher_fi(0, 8u /* reference, not const */, g_td_err, -0x20);
        stub_fi[1] = catcher_fi(1, 8u | 1u, g_td_err, -0x20);
        g_cat[1].callee = FX_FN(K_CATCH_A);
        g_cat[0].callee = FX_FN(K_THROW);
        g_throw_ti = ti_const;
        r = run(FX_FN(K_CATCH_B));
        CHECK_EQ(r, 1);
        CHECK_LOG(EV_THROW, EV_CATCH, EV_DTOR, EV_AFTER);
        if (fx_nlog == 4) CHECK_EQ(fx_log_buf[1].a, g_cat[1].ebp);
    }
}
