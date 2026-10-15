/* core_eh.c -- the MSVC x86 exception-handling runtime (docs/design/runtime-core.md section 7):
 * SEH dispatch over the guest fs:[0] chain, RtlUnwind, __CxxFrameHandler3 over guest FuncInfo
 * tables, the catch guard, _CxxThrowException (throw and rethrow), _except_handler3,
 * _except_handler4_common, _setjmp3/longjmp, RaiseException, and sg_eh_resume into the
 * translator's landings (runtime-abi 10.3).
 *
 * Every structure lives in guest memory with the documented MSVC x86 layout (field offsets are
 * spelled out next to each access; tools/eh/fh3_model.py is the executable reference this file
 * follows). Funclets, filters, handlers and copy constructors are guest code called through
 * sg_core_call. */
#include "core_internal.h"

/* ---- guest layouts --------------------------------------------------------------------------- */
/* EXCEPTION_RECORD: +0 code, +4 flags, +8 record, +12 address, +16 nparams, +20 info[15] */
#define REC_SIZE 0x50u
#define CTXREC_SIZE 0x2ccu
/* CONTEXT (x86) integer fields */
#define CX_FLAGS 0x00u
#define CX_EDI 0x9cu
#define CX_ESI 0xa0u
#define CX_EBX 0xa4u
#define CX_EDX 0xa8u
#define CX_ECX 0xacu
#define CX_EAX 0xb0u
#define CX_EBP 0xb4u
#define CX_EIP 0xb8u
#define CX_EFLAGS 0xc0u
#define CX_ESP 0xc4u
/* FuncInfo: +0 magic, +4 maxState, +8 pUnwindMap, +12 nTryBlocks, +16 pTryBlockMap, +20 nIPMap,
 * +24 pIPMap, +28 pESTypeList, +32 EHFlags.  UnwindMapEntry {toState, action} (8 bytes).
 * TryBlockMapEntry {tryLow, tryHigh, catchHigh, nCatches, pHandlerArray} (20 bytes).
 * HandlerType {adjectives, pType, dispCatchObj, addressOfHandler} (16 bytes).
 * ThrowInfo {attributes, pmfnUnwind, pForwardCompat, pCatchableTypeArray}.
 * CatchableTypeArray {n, CatchableType*[n]}.
 * CatchableType {properties, pType, PMD{mdisp, pdisp, vdisp}, sizeOrOffset, copyFunction}.
 * TypeDescriptor {pVFTable, spare, name[]}. */
#define HT_IsConst 1u
#define HT_IsVolatile 2u
#define HT_IsUnaligned 4u
#define HT_IsReference 8u
#define CT_IsSimpleType 1u
#define CT_ByReferenceOnly 2u
#define CT_HasVirtualBase 4u
#define TI_IsConst 1u
#define TI_IsVolatile 2u
#define TI_IsUnaligned 4u
/* _JUMP_BUFFER: +0 Ebp, +4 Ebx, +8 Edi, +12 Esi, +16 Esp, +20 Eip, +24 Registration,
 * +28 TryLevel, +32 Cookie ('VC20'), +36 UnwindFunc, +40 UnwindData[6] */
#define JB_VC20 0x56433230u

enum { DISP_CONTINUE_EXECUTION = 0, DISP_CONTINUE_SEARCH = 1, DISP_NESTED = 2, DISP_COLLIDED = 3 };

static inline int32_t s32(uint32_t v) { return (int32_t)v; }
static inline uint32_t L(sg_ctx *ctx, uint32_t a) { return sg_ld32(ctx->mem, a); }
static inline void S(sg_ctx *ctx, uint32_t a, uint32_t v) { sg_st32(ctx->mem, a, v); }
static inline uint32_t fs0(sg_ctx *ctx) { return L(ctx, ctx->fs_base + SG_TEB_EXCEPTION_LIST); }
static inline void set_fs0(sg_ctx *ctx, uint32_t v) { S(ctx, ctx->fs_base + SG_TEB_EXCEPTION_LIST, v); }

uint32_t core_thunk_catch_guard;
#define SLOT_CATCH_GUARD 3u

void (*sg_core_unhandled_hook)(sg_ctx *ctx, uint32_t rec);
const sg_resume_point *sg_core_resume_table;
uint32_t sg_core_resume_table_count;

/* ---- helpers ------------------------------------------------------------------------------------ */

/* Calls a funclet the way msvcr90's _CallSettingFrame does: ebp = the parent frame, callee-saved
 * registers restored afterwards. Returns eax (a catch funclet's continuation, a filter's verdict). */
static uint32_t call_funclet(sg_ctx *ctx, uint32_t fn, uint32_t frame_ebp) {
    uint32_t ebx = ctx->ebx, esi = ctx->esi, edi = ctx->edi, ebp = ctx->ebp, r;
    ctx->ebp = frame_ebp;
    r = sg_core_call(ctx, fn, SG_CC_CDECL, 0, NULL, SG_CORE_CALL_FUNCLET);
    ctx->ebx = ebx; ctx->esi = esi; ctx->edi = edi; ctx->ebp = ebp;
    return r;
}

/* thiscall with up to 2 stack arguments; callee-saved registers restored. */
static void call_member(sg_ctx *ctx, uint32_t fn, uint32_t self, unsigned nargs, uint32_t a1, uint32_t a2) {
    uint32_t args[3] = {self, a1, a2};
    uint32_t ebx = ctx->ebx, esi = ctx->esi, edi = ctx->edi, ebp = ctx->ebp;
    sg_core_call(ctx, fn, SG_CC_THISCALL, nargs + 1u, args, SG_CORE_CALL_FUNCLET);
    ctx->ebx = ebx; ctx->esi = esi; ctx->edi = edi; ctx->ebp = ebp;
}

static uint32_t call_handler(sg_ctx *ctx, uint32_t rec, uint32_t rn, uint32_t ctxrec, uint32_t dc) {
    uint32_t args[4] = {rec, rn, ctxrec, dc};
    uint32_t ebx = ctx->ebx, esi = ctx->esi, edi = ctx->edi, ebp = ctx->ebp, r;
    r = sg_core_call(ctx, L(ctx, rn + 4), SG_CC_CDECL, 4, args, SG_CORE_CALL_FUNCLET);
    ctx->ebx = ebx; ctx->esi = esi; ctx->edi = edi; ctx->ebp = ebp;
    return r;
}

static int rn_valid(sg_ctx *ctx, uint32_t rn) {
    const sg_thread *t = ctx->thread;
    if (rn & 3u) return 0;
    if (!t) return 1;
    return rn >= t->stack_lo && rn + 8u <= t->stack_hi;
}

/* Reserves `n` bytes (16-aligned) on the guest stack below esp and returns its address. */
static uint32_t stack_alloc(sg_ctx *ctx, uint32_t n) {
    ctx->esp = (ctx->esp - n) & ~15u;
    memset(ctx->mem + ctx->esp, 0, n);
    return ctx->esp;
}

static uint32_t build_record(sg_ctx *ctx, uint32_t code, uint32_t flags, uint32_t addr, unsigned n, const uint32_t *p) {
    uint32_t rec = stack_alloc(ctx, REC_SIZE), i;
    if (n > 15) n = 15;
    S(ctx, rec + 0, code);
    S(ctx, rec + 4, flags);
    S(ctx, rec + 8, 0);
    S(ctx, rec + 12, addr);
    S(ctx, rec + 16, n);
    for (i = 0; i < n; i++) S(ctx, rec + 20 + 4 * i, p[i]);
    return rec;
}

static uint32_t build_context(sg_ctx *ctx, uint32_t eip) {
    uint32_t esp = ctx->esp, c = stack_alloc(ctx, CTXREC_SIZE);
    S(ctx, c + CX_FLAGS, 0x00010007u); /* CONTEXT_FULL (i386) */
    S(ctx, c + CX_EDI, ctx->edi);
    S(ctx, c + CX_ESI, ctx->esi);
    S(ctx, c + CX_EBX, ctx->ebx);
    S(ctx, c + CX_EDX, ctx->edx);
    S(ctx, c + CX_ECX, ctx->ecx);
    S(ctx, c + CX_EAX, ctx->eax);
    S(ctx, c + CX_EBP, ctx->ebp);
    S(ctx, c + CX_EIP, eip);
    S(ctx, c + CX_EFLAGS, sg_get_eflags(ctx));
    S(ctx, c + CX_ESP, esp);
    return c;
}

static const char *cxx_type_name(sg_ctx *ctx, uint32_t ti, char *buf, size_t cap) {
    uint32_t cta, ct, td, i;
    buf[0] = 0;
    if (!ti) return buf;
    cta = L(ctx, ti + 12);
    if (!cta || !L(ctx, cta)) return buf;
    ct = L(ctx, cta + 4);
    td = ct ? L(ctx, ct + 4) : 0;
    if (!td) return buf;
    for (i = 0; i + 1 < cap; i++) {
        buf[i] = (char)ctx->mem[td + 8 + i];
        if (!buf[i]) break;
    }
    buf[cap - 1] = 0;
    return buf;
}

static SG_NORETURN void unhandled(sg_ctx *ctx, uint32_t rec, const char *why) {
    char tn[128];
    uint32_t code = L(ctx, rec), n = L(ctx, rec + 16);
    tn[0] = 0;
    if (code == SG_EXC_CXX && n >= 3) cxx_type_name(ctx, L(ctx, rec + 28), tn, sizeof tn);
    sg_core_log(0, "unhandled guest exception 0x%08x%s%s%s at 0x%08x (%s), thread %s", code, tn[0] ? " (C++ " : "", tn,
                tn[0] ? ")" : "", L(ctx, rec + 12), why, ctx->thread ? sg_core_thread_name(ctx->thread) : "?");
    sg_core_backtrace(ctx, 0);
    if (sg_core_unhandled_hook) sg_core_unhandled_hook(ctx, rec);
    sg_core_fatal(ctx, "unhandled guest exception 0x%08x", code);
}

SG_NORETURN void core_terminate(sg_ctx *ctx, const char *why) {
    sg_core_fatal(ctx, "terminate(): %s", why);
}

/* ---- dispatch and unwind ---------------------------------------------------------------------- */

static void dispatch(sg_ctx *ctx, uint32_t rec, uint32_t ctxrec) {
    uint32_t rn = fs0(ctx);
    while (rn != SG_END_OF_CHAIN) {
        uint32_t d;
        if (!rn_valid(ctx, rn)) {
            S(ctx, rec + 4, L(ctx, rec + 4) | SG_EXC_STACK_INVALID);
            unhandled(ctx, rec, "registration outside the stack");
        }
        d = call_handler(ctx, rec, rn, ctxrec, 0);
        if (d == DISP_CONTINUE_EXECUTION) {
            if (L(ctx, rec + 4) & SG_EXC_NONCONTINUABLE)
                unhandled(ctx, rec, "a handler continued a noncontinuable exception");
            return;
        }
        if (d != DISP_CONTINUE_SEARCH && d != DISP_NESTED)
            sg_core_fatal(ctx, "exception handler 0x%08x returned disposition %u", L(ctx, rn + 4), d);
        rn = L(ctx, rn);
    }
    unhandled(ctx, rec, "end of the fs:[0] chain");
}

void sg_seh_raise(sg_ctx *ctx, uint32_t code, uint32_t flags, unsigned nparams, const uint32_t *params) {
    uint32_t esp0 = ctx->esp, ra = sg_ld32(ctx->mem, ctx->esp), ctxrec, rec;
    ctxrec = build_context(ctx, ra);
    rec = build_record(ctx, code, flags & SG_EXC_NONCONTINUABLE, ra, nparams, params);
    dispatch(ctx, rec, ctxrec);
    ctx->esp = esp0; /* continued (only continuable exceptions get here) */
}

void sg_seh_unwind(sg_ctx *ctx, uint32_t target, uint32_t rec) {
    uint32_t esp0 = ctx->esp, rn, flags;
    if (!rec) rec = build_record(ctx, SG_STATUS_UNWIND, 0, 0, 0, NULL);
    flags = L(ctx, rec + 4) | SG_EXC_UNWINDING | (target == SG_END_OF_CHAIN ? SG_EXC_EXIT_UNWIND : 0u);
    S(ctx, rec + 4, flags);
    rn = fs0(ctx);
    while (rn != target && rn != SG_END_OF_CHAIN) {
        uint32_t next;
        if (target != SG_END_OF_CHAIN && rn > target)
            sg_core_fatal(ctx, "unwind target 0x%08x is not on the fs:[0] chain (reached 0x%08x)", target, rn);
        if (!rn_valid(ctx, rn)) sg_core_fatal(ctx, "unwind: registration 0x%08x outside the stack", rn);
        call_handler(ctx, rec, rn, 0, 0);
        next = L(ctx, rn);
        set_fs0(ctx, next);
        rn = next;
    }
    S(ctx, rec + 4, L(ctx, rec + 4) & ~(SG_EXC_UNWINDING | SG_EXC_EXIT_UNWIND));
    ctx->esp = esp0;
}

/* ---- landings (runtime-abi 10.3) and host cleanup records (10.4) -------------------------------- */

static const sg_resume_point *resume_lookup(uint32_t target) {
    const sg_resume_point *t = sg_core_resume_table ? sg_core_resume_table : sg_resume_points;
    uint32_t n = sg_core_resume_table ? sg_core_resume_table_count : sg_resume_point_count, lo = 0, hi = n;
    while (lo < hi) {
        uint32_t mid = lo + (hi - lo) / 2;
        if (t[mid].resume < target) lo = mid + 1; else hi = mid;
    }
    return lo < n && t[lo].resume == target ? &t[lo] : NULL;
}

uint32_t sg_core_resume_owner(uint32_t target) {
    const sg_resume_point *p = resume_lookup(target);
    return p ? p->func : 0;
}

void sg_cleanup_push(sg_ctx *ctx, sg_cleanup *c) {
    core_thread *h = core_cur(ctx);
    if (!h) return;
    c->prev = h->cleanup;
    h->cleanup = c;
}

void sg_cleanup_pop(sg_ctx *ctx, sg_cleanup *c) {
    core_thread *h = core_cur(ctx);
    if (!h) return;
    if (h->cleanup != c) sg_core_fatal(ctx, "sg_cleanup_pop: record %p is not innermost", (void *)c);
    h->cleanup = c->prev;
}

void core_run_cleanups_below(sg_ctx *ctx, const void *limit) {
    core_thread *h = core_cur(ctx);
    if (!h) return;
    while (h->cleanup && (!limit || (uintptr_t)h->cleanup < (uintptr_t)limit)) {
        sg_cleanup *c = h->cleanup;
        h->cleanup = c->prev;
        if (c->barrier)
            sg_core_fatal(ctx, "guest unwinding crosses the barrier \"%s\" (runtime-abi 10.4)", c->what ? c->what : "?");
        if (c->fn) c->fn(c);
    }
}

void sg_eh_resume(sg_ctx *ctx, uint32_t target) {
    uint32_t owner = sg_core_resume_owner(target);
    sg_landing *l;
    if (!owner) sg_core_fatal(ctx, "sg_eh_resume: 0x%08x is not a resume point", target);
    for (l = (sg_landing *)ctx->eh_top; l; l = l->prev)
        if (l->func == owner && l->frame_esp > ctx->esp) break;
    if (!l)
        sg_core_fatal(ctx, "sg_eh_resume: no live landing of 0x%08x for resume point 0x%08x (esp 0x%08x)", owner,
                      target, ctx->esp);
    core_run_cleanups_below(ctx, l);
    ctx->eh_top = l;
    ctx->eip = target;
    sg_core_full_barrier();
    SG_HOST_LONGJMP(l->jb);
}

/* ---- C++: __CxxFrameHandler3 (runtime-core.md 7.2) ---------------------------------------------- */

static uint32_t adjust_pointer(sg_ctx *ctx, uint32_t obj, uint32_t ct) {
    int32_t mdisp = s32(L(ctx, ct + 8)), pdisp = s32(L(ctx, ct + 12)), vdisp = s32(L(ctx, ct + 16));
    uint32_t p = obj + (uint32_t)mdisp;
    if (pdisp >= 0) {
        uint32_t vbtable = L(ctx, obj + (uint32_t)pdisp);
        p += (uint32_t)s32(L(ctx, vbtable + (uint32_t)vdisp)) + (uint32_t)pdisp;
    }
    return p;
}

static int td_name_eq(sg_ctx *ctx, uint32_t a, uint32_t b) {
    return !strcmp((const char *)ctx->mem + a + 8, (const char *)ctx->mem + b + 8);
}

/* TypeMatch (frame.cpp): HandlerType ht against CatchableType ct of ThrowInfo ti. */
static int type_match(sg_ctx *ctx, uint32_t ht, uint32_t ct, uint32_t ti) {
    uint32_t adj = L(ctx, ht), ptype = L(ctx, ht + 4), cptype = L(ctx, ct + 4), props = L(ctx, ct), attr = L(ctx, ti);
    if (ptype == 0 || ctx->mem[ptype + 8] == 0) return 1; /* catch (...) */
    if (ptype != cptype && !td_name_eq(ctx, ptype, cptype)) return 0;
    if ((props & CT_ByReferenceOnly) && !(adj & HT_IsReference)) return 0;
    if ((attr & TI_IsConst) && !(adj & HT_IsConst)) return 0;
    if ((attr & TI_IsUnaligned) && !(adj & HT_IsUnaligned)) return 0;
    if ((attr & TI_IsVolatile) && !(adj & HT_IsVolatile)) return 0;
    return 1;
}

static void build_catch_object(sg_ctx *ctx, uint32_t rn, uint32_t ht, uint32_t ct, uint32_t obj) {
    uint32_t adj = L(ctx, ht), ptype = L(ctx, ht + 4), dest, props, size, copy;
    int32_t disp = s32(L(ctx, ht + 8));
    if (ptype == 0 || ctx->mem[ptype + 8] == 0 || disp == 0) return;
    dest = rn + 12u + (uint32_t)disp;
    props = L(ctx, ct);
    size = L(ctx, ct + 20);
    copy = L(ctx, ct + 24);
    if (adj & HT_IsReference) {
        S(ctx, dest, adjust_pointer(ctx, obj, ct));
    } else if (props & CT_IsSimpleType) {
        memmove(ctx->mem + dest, ctx->mem + obj, size);
        if (size == 4 && L(ctx, dest)) S(ctx, dest, adjust_pointer(ctx, L(ctx, dest), ct)); /* pointer types */
    } else if (!copy) {
        memmove(ctx->mem + dest, ctx->mem + adjust_pointer(ctx, obj, ct), size);
    } else if (props & CT_HasVirtualBase) {
        call_member(ctx, copy, dest, 2, adjust_pointer(ctx, obj, ct), 1);
    } else {
        call_member(ctx, copy, dest, 1, adjust_pointer(ctx, obj, ct), 0);
    }
}

/* __FrameUnwindToState: run unwind actions from the frame's current state down to `target`,
 * storing each new state before calling its action, as MSVC does. */
static void unwind_to_state(sg_ctx *ctx, uint32_t rn, uint32_t fi, int32_t target) {
    int32_t max = s32(L(ctx, fi + 4)), cur = s32(L(ctx, rn + 8));
    uint32_t umap = L(ctx, fi + 8);
    while (cur != target && cur != -1) {
        int32_t to;
        uint32_t act;
        if (cur < 0 || cur >= max)
            sg_core_fatal(ctx, "FH3: state %d out of range (maxState %d, FuncInfo 0x%08x)", cur, max, fi);
        to = s32(L(ctx, umap + 8u * (uint32_t)cur));
        act = L(ctx, umap + 8u * (uint32_t)cur + 4u);
        S(ctx, rn + 8, (uint32_t)to);
        if (act) call_funclet(ctx, act, rn + 12u);
        cur = to;
    }
    S(ctx, rn + 8, (uint32_t)target);
}

static void destroy_object(sg_ctx *ctx, uint32_t obj, uint32_t ti) {
    uint32_t dtor = L(ctx, ti + 4);
    if (dtor && obj) call_member(ctx, dtor, obj, 0, 0, 0);
}

static int inflight_find(core_thread *h, uint32_t guard) {
    unsigned i;
    for (i = h->ninflight; i-- > 0;)
        if (h->inflight[i].guard == guard) return (int)i;
    return -1;
}

static void inflight_remove(core_thread *h, int i) {
    unsigned k;
    for (k = (unsigned)i; k + 1 < h->ninflight; k++) h->inflight[k] = h->inflight[k + 1];
    h->ninflight--;
}

/* The catch guard (runtime-core.md 7.3): registered above the catching frame while its catch block
 * runs. Search: continue (the catching frame is next). Unwind (an exception or longjmp is leaving
 * the catch block): destroy the caught object unless that very object is being rethrown. */
static void catch_guard_fn(sg_ctx *ctx, uint8_t *mem) {
    uint32_t rec = SG_ARG(0), rn = SG_ARG(1);
    core_thread *h = core_cur(ctx);
    if (h && (L(ctx, rec + 4) & (SG_EXC_UNWINDING | SG_EXC_EXIT_UNWIND))) {
        int i = inflight_find(h, rn);
        if (i >= 0 && !h->inflight[i].done) {
            core_inflight e = h->inflight[i];
            h->inflight[i].done = 1;
            inflight_remove(h, i);
            if (h->dispatch_obj != e.obj) destroy_object(ctx, e.obj, e.ti);
        }
    }
    SG_RETURN_CDECL(DISP_CONTINUE_SEARCH);
}

static SG_NORETURN void catch_it(sg_ctx *ctx, uint32_t rec, uint32_t rn, uint32_t fi, uint32_t ht, uint32_t ct,
                                 uint32_t obj, uint32_t ti, int32_t lo, int32_t hi) {
    core_thread *h = core_cur(ctx);
    uint32_t guard, cont, handler = L(ctx, ht + 12);
    int i;
    if (!h) sg_core_fatal(ctx, "C++ catch on a thread without runtime state");
    /* 1. the catch object, while the thrown object is still alive in the thrower's frame */
    build_catch_object(ctx, rn, ht, ct, obj);
    /* 2. global unwind of every frame above this one, 3. local unwind to the try's low state */
    sg_seh_unwind(ctx, rn, rec);
    unwind_to_state(ctx, rn, fi, lo);
    /* 4. the catch block runs in state tryHigh + 1 */
    S(ctx, rn + 8, (uint32_t)(hi + 1));
    /* 5. catch guard + call the catch funclet */
    if (h->ninflight >= CORE_MAX_INFLIGHT) sg_core_fatal(ctx, "too many nested catch blocks");
    guard = stack_alloc(ctx, 16);
    S(ctx, guard + 0, fs0(ctx));
    S(ctx, guard + 4, core_thunk_catch_guard);
    S(ctx, guard + 8, rn);
    S(ctx, guard + 12, 0);
    set_fs0(ctx, guard);
    h->inflight[h->ninflight].obj = obj;
    h->inflight[h->ninflight].ti = ti;
    h->inflight[h->ninflight].guard = guard;
    h->inflight[h->ninflight].done = 0;
    h->ninflight++;
    cont = call_funclet(ctx, handler, rn + 12u);
    /* 6. the catch block finished normally: pop the guard, destroy the exception object */
    set_fs0(ctx, L(ctx, guard));
    i = inflight_find(h, guard);
    if (i >= 0) {
        inflight_remove(h, i);
        destroy_object(ctx, obj, ti);
    }
    /* 7. _JumpToContinuation */
    ctx->esp = L(ctx, rn - 4u);
    ctx->ebp = rn + 12u;
    set_fs0(ctx, rn);
    sg_eh_resume(ctx, cont);
}

static uint32_t frame_handler3(sg_ctx *ctx, uint32_t rec, uint32_t rn, uint32_t fi) {
    uint32_t magic = L(ctx, fi), ntry, ptry, flags = L(ctx, rec + 4), obj, ti, cta, ncta, t;
    int32_t max, state;
    if (magic < 0x19930520u || magic > 0x19930522u)
        sg_core_fatal(ctx, "__CxxFrameHandler3: bad FuncInfo 0x%08x (magic 0x%08x)", fi, magic);
    max = s32(L(ctx, fi + 4));
    ntry = L(ctx, fi + 12);
    ptry = L(ctx, fi + 16);
    if (flags & (SG_EXC_UNWINDING | SG_EXC_EXIT_UNWIND)) {
        if (max > 0 && s32(L(ctx, rn + 8)) != -1) unwind_to_state(ctx, rn, fi, -1);
        return DISP_CONTINUE_SEARCH;
    }
    if (ntry == 0) return DISP_CONTINUE_SEARCH;
    if (L(ctx, rec) != SG_EXC_CXX || L(ctx, rec + 16) != 3 || L(ctx, rec + 20) < 0x19930520u ||
        L(ctx, rec + 20) > 0x19930522u)
        return DISP_CONTINUE_SEARCH; /* not C++; no _set_se_translator in this exe */
    obj = L(ctx, rec + 24);
    ti = L(ctx, rec + 28);
    if (!ti) core_terminate(ctx, "C++ exception record without ThrowInfo");
    cta = L(ctx, ti + 12);
    ncta = L(ctx, cta);
    state = s32(L(ctx, rn + 8));
    for (t = 0; t < ntry; t++) {
        uint32_t e = ptry + 20u * t, c, nc = L(ctx, e + 12), parr = L(ctx, e + 16);
        int32_t lo = s32(L(ctx, e)), hi = s32(L(ctx, e + 4));
        if (state < lo || state > hi) continue;
        for (c = 0; c < nc; c++) {
            uint32_t ht = parr + 16u * c, k;
            for (k = 0; k < ncta; k++) {
                uint32_t ct = L(ctx, cta + 4u + 4u * k);
                if (type_match(ctx, ht, ct, ti)) catch_it(ctx, rec, rn, fi, ht, ct, obj, ti, lo, hi);
            }
        }
    }
    return DISP_CONTINUE_SEARCH;
}

/* Reached by a guest stub `mov eax, FuncInfo; jmp __CxxFrameHandler3`: FuncInfo in eax, the
 * four cdecl handler arguments on the guest stack. */
SGC_SHIM(msvcr90, __CxxFrameHandler3) {
    uint32_t fi = ctx->eax, rec = SG_ARG(0), rn = SG_ARG(1);
    uint32_t ebx = ctx->ebx, esi = ctx->esi, edi = ctx->edi, ebp = ctx->ebp, r;
    r = frame_handler3(ctx, rec, rn, fi);
    ctx->ebx = ebx; ctx->esi = esi; ctx->edi = edi; ctx->ebp = ebp;
    SG_RETURN_CDECL(r);
}

SGC_SHIM(msvcr90, _CxxThrowException) {
    uint32_t obj = SG_ARG(0), ti = SG_ARG(1), p[3];
    core_thread *h = core_cur(ctx);
    if (!obj && !ti) { /* throw; */
        if (!h || !h->ninflight) core_terminate(ctx, "rethrow with no exception being handled");
        obj = h->inflight[h->ninflight - 1].obj;
        ti = h->inflight[h->ninflight - 1].ti;
    }
    p[0] = SG_EXC_CXX_MAGIC1;
    p[1] = obj;
    p[2] = ti;
    if (h) h->dispatch_obj = obj;
    sg_seh_raise(ctx, SG_EXC_CXX, SG_EXC_NONCONTINUABLE, 3, p);
    core_terminate(ctx, "C++ exception dispatch returned");
}

/* ---- SEH: _except_handler3 / _except_handler4_common (runtime-core.md 7.4) ---------------------- */

typedef struct seh_kind {
    uint32_t table;   /* scope table (decoded) */
    uint32_t first;   /* byte offset of record 0: 0 (EH3) or 16 (EH4 header) */
    int32_t outside;  /* outermost level: -1 (EH3) or -2 (EH4) */
    uint32_t checkfn; /* EH4: guest __security_check_cookie (cookie in ecx), else 0 */
    int eh4;
} seh_kind;

static void scope(sg_ctx *ctx, const seh_kind *k, int32_t lvl, int32_t *enc, uint32_t *filt, uint32_t *hand) {
    uint32_t a = k->table + k->first + 12u * (uint32_t)lvl;
    *enc = s32(L(ctx, a));
    *filt = L(ctx, a + 4);
    *hand = L(ctx, a + 8);
}

/* _local_unwind2/_EH4_LocalUnwind: run __finally handlers from the current level to `stop`. */
static void seh_local_unwind(sg_ctx *ctx, uint32_t rn, const seh_kind *k, int32_t stop) {
    int32_t lvl = s32(L(ctx, rn + 12));
    while (lvl != stop && lvl != k->outside && lvl != -1) {
        int32_t enc;
        uint32_t filt, hand;
        if (lvl < 0) sg_core_fatal(ctx, "SEH local unwind: bad try level %d at frame 0x%08x", lvl, rn);
        scope(ctx, k, lvl, &enc, &filt, &hand);
        S(ctx, rn + 12, (uint32_t)enc);
        if (filt == 0) call_funclet(ctx, hand, rn + 0x10u);
        lvl = enc;
    }
}

/* ValidateLocalCookies: the GS cookie (if any) and the EH cookie, through the guest check. */
static void eh4_check_cookies(sg_ctx *ctx, uint32_t rn, const seh_kind *k) {
    uint32_t fp = rn + 0x10u, t = k->table;
    int32_t gs = s32(L(ctx, t)), gsx = s32(L(ctx, t + 4)), ec = s32(L(ctx, t + 8)), ecx = s32(L(ctx, t + 12));
    if (!k->checkfn) return;
    if (gs != -2) {
        uint32_t v = L(ctx, fp + (uint32_t)gs) ^ (fp + (uint32_t)gsx);
        call_member(ctx, k->checkfn, v, 0, 0, 0);
    }
    {
        uint32_t v = L(ctx, fp + (uint32_t)ec) ^ (fp + (uint32_t)ecx);
        call_member(ctx, k->checkfn, v, 0, 0, 0);
    }
}

static uint32_t seh_frame(sg_ctx *ctx, uint32_t rec, uint32_t rn, uint32_t ctxrec, const seh_kind *k) {
    uint32_t ebp = rn + 0x10u, flags = L(ctx, rec + 4), esp0 = ctx->esp, xp;
    int32_t lvl;
    if (k->eh4) eh4_check_cookies(ctx, rn, k);
    if (flags & (SG_EXC_UNWINDING | SG_EXC_EXIT_UNWIND)) {
        seh_local_unwind(ctx, rn, k, k->outside);
        return DISP_CONTINUE_SEARCH;
    }
    xp = stack_alloc(ctx, 16); /* EXCEPTION_POINTERS {rec, context}, for GetExceptionInformation */
    S(ctx, xp, rec);
    S(ctx, xp + 4, ctxrec);
    S(ctx, rn - 4u, xp);
    lvl = s32(L(ctx, rn + 12));
    while (lvl != k->outside && lvl != -1) {
        int32_t enc, r;
        uint32_t filt, hand;
        if (lvl < 0) sg_core_fatal(ctx, "SEH: bad try level %d at frame 0x%08x", lvl, rn);
        scope(ctx, k, lvl, &enc, &filt, &hand);
        if (filt) {
            r = s32(call_funclet(ctx, filt, ebp));
            if (r < 0) { ctx->esp = esp0; return DISP_CONTINUE_EXECUTION; }
            if (r > 0) {
                sg_seh_unwind(ctx, rn, k->eh4 ? rec : 0);
                seh_local_unwind(ctx, rn, k, lvl);
                S(ctx, rn + 12, (uint32_t)enc);
                if (k->eh4) eh4_check_cookies(ctx, rn, k);
                ctx->ebp = ebp;
                ctx->esp = L(ctx, rn - 8u);
                set_fs0(ctx, rn);
                sg_eh_resume(ctx, hand);
            }
        }
        lvl = enc;
    }
    ctx->esp = esp0;
    return DISP_CONTINUE_SEARCH;
}

SGC_SHIM(msvcr90, _except_handler3) {
    uint32_t rec = SG_ARG(0), rn = SG_ARG(1), ctxrec = SG_ARG(2), r;
    uint32_t ebx = ctx->ebx, esi = ctx->esi, edi = ctx->edi, ebp = ctx->ebp;
    seh_kind k = {L(ctx, rn + 8), 0, -1, 0, 0};
    r = seh_frame(ctx, rec, rn, ctxrec, &k);
    ctx->ebx = ebx; ctx->esi = esi; ctx->edi = edi; ctx->ebp = ebp;
    SG_RETURN_CDECL(r);
}

SGC_SHIM(msvcr90, _except_handler4_common) {
    /* (PUINT_PTR CookiePointer, PCOOKIE_CHECK CookieCheckFunction, rec, rn, ctxrec, dc) */
    uint32_t cookie = SG_ARG(0) ? L(ctx, SG_ARG(0)) : 0, rec = SG_ARG(2), rn = SG_ARG(3), ctxrec = SG_ARG(4), r;
    uint32_t ebx = ctx->ebx, esi = ctx->esi, edi = ctx->edi, ebp = ctx->ebp;
    seh_kind k = {L(ctx, rn + 8) ^ cookie, 16, -2, SG_ARG(1), 1};
    r = seh_frame(ctx, rec, rn, ctxrec, &k);
    ctx->ebx = ebx; ctx->esi = esi; ctx->edi = edi; ctx->ebp = ebp;
    SG_RETURN_CDECL(r);
}

/* ---- setjmp / longjmp (runtime-core.md 7.5) ------------------------------------------------------ */

SGC_SHIM(msvcr90, _setjmp3) {
    /* cdecl (jmp_buf, count, ...). esp at entry points at the return address. */
    uint32_t buf = SG_ARG(0), count = SG_ARG(1), reg = fs0(ctx);
    S(ctx, buf + 0, ctx->ebp);
    S(ctx, buf + 4, ctx->ebx);
    S(ctx, buf + 8, ctx->edi);
    S(ctx, buf + 12, ctx->esi);
    S(ctx, buf + 16, ctx->esp);
    S(ctx, buf + 20, sg_ld32(mem, ctx->esp));
    S(ctx, buf + 24, reg);
    /* The cookie and UnwindFunc fields are written for count 0 too (both inside the 64-byte
     * jmp_buf), so longjmp never reads uninitialised bytes; with UnwindFunc 0 it then skips the
     * local unwind, which is a no-op for all 28 count-0 sites anyway (runtime-core.md 7.5). */
    S(ctx, buf + 32, JB_VC20);
    S(ctx, buf + 36, 0);
    if (count == 0) {
        S(ctx, buf + 28, reg == SG_END_OF_CHAIN ? 0xffffffffu : L(ctx, reg + 12));
    } else {
        /* count >= 1: UnwindFunc, then TryLevel, then UnwindData (C++ /EH setjmp; unused here) */
        uint32_t i;
        S(ctx, buf + 36, SG_ARG(2));
        S(ctx, buf + 28, count >= 2 ? SG_ARG(3) : (reg == SG_END_OF_CHAIN ? 0xffffffffu : L(ctx, reg + 12)));
        for (i = 2; i < count && i < 8; i++) S(ctx, buf + 40 + 4 * (i - 2), SG_ARG(2 + i));
    }
    SG_RETURN_CDECL(0);
}

SGC_SHIM(msvcr90, longjmp) {
    uint32_t buf = SG_ARG(0), val = SG_ARG(1), reg = L(ctx, buf + 24), p = buf;
    core_thread *h = core_cur(ctx);
    if (h) h->dispatch_obj = 0;
    if (reg != fs0(ctx)) {
        uint32_t esp0 = ctx->esp, rec = build_record(ctx, SG_STATUS_LONGJUMP, 0, 0, 1, &p);
        sg_seh_unwind(ctx, reg, rec);
        ctx->esp = esp0;
    }
    if (L(ctx, buf + 32) == JB_VC20 && L(ctx, buf + 36)) {
        uint32_t a = buf;
        sg_core_log(1, "longjmp: calling UnwindFunc 0x%08x (untested path)", L(ctx, buf + 36));
        sg_core_call(ctx, L(ctx, buf + 36), SG_CC_CDECL, 1, &a, SG_CORE_CALL_FUNCLET);
    } else if (reg != SG_END_OF_CHAIN && sg_lookup(ctx, mem, L(ctx, reg + 4)) == sgc_msvcr90__except_handler3) {
        seh_kind k = {L(ctx, reg + 8), 0, -1, 0, 0};
        seh_local_unwind(ctx, reg, &k, s32(L(ctx, buf + 28)));
    }
    ctx->ebp = L(ctx, buf + 0);
    ctx->ebx = L(ctx, buf + 4);
    ctx->edi = L(ctx, buf + 8);
    ctx->esi = L(ctx, buf + 12);
    ctx->esp = L(ctx, buf + 16) + 4u; /* saved esp points at _setjmp3's return address */
    ctx->eax = val ? val : 1u;
    set_fs0(ctx, reg);
    sg_eh_resume(ctx, L(ctx, buf + 20));
}

/* ---- the other EH-related imports ------------------------------------------------------------------ */

SGC_SHIM(kernel32, RaiseException) {
    /* (dwExceptionCode, dwExceptionFlags, nNumberOfArguments, lpArguments) */
    uint32_t code = SG_ARG(0), flags = SG_ARG(1), n = SG_ARG(2), args = SG_ARG(3), p[15], i;
    core_thread *h = core_cur(ctx);
    if (n > 15) n = 15;
    for (i = 0; i < n; i++) p[i] = args ? sg_ld32(mem, args + 4 * i) : 0;
    if (code == SG_EXC_SET_THREAD_NAME) {
        /* THREADNAME_INFO {dwType 0x1000, szName, dwThreadID, dwFlags}: swallowed (the debugger's thread-naming convention) */
        if (n >= 3 && p[0] == 0x1000u && p[1]) {
            char name[64];
            core_thread *t = NULL, *w;
            for (i = 0; i + 1 < sizeof name; i++) {
                name[i] = (char)mem[p[1] + i];
                if (!name[i]) break;
            }
            name[sizeof name - 1] = 0;
            if (p[2] == 0xffffffffu || (ctx->thread && p[2] == ctx->thread->tid)) t = h;
            else {
                pthread_mutex_lock(&core_threads_lock);
                for (w = core_threads_all; w; w = w->all_next)
                    if (w->self->tid == p[2]) { t = w; break; }
                if (t) snprintf(t->name, sizeof t->name, "%s", name);
                pthread_mutex_unlock(&core_threads_lock);
                t = NULL; /* named; host name only for the current thread */
            }
            if (t) {
                snprintf(t->name, sizeof t->name, "%s", name);
                core_set_host_thread_name(name);
            }
            sg_core_log(2, "SetThreadName(%u) = \"%s\"", p[2], name);
        }
        SG_RET(16);
    }
    if (h) h->dispatch_obj = 0;
    sg_seh_raise(ctx, code, flags, n, p);
    SG_RET(16);
}

SGC_SHIM(msvcr90, _XcptFilter) {
    sg_core_log(0, "_XcptFilter: exception 0x%08x reached the entry point's filter; continuing the search", SG_ARG(0));
    SG_RETURN_CDECL(0); /* EXCEPTION_CONTINUE_SEARCH: the chain ends and the runtime reports it */
}

static uint32_t unhandled_filter;
SGC_SHIM(kernel32, SetUnhandledExceptionFilter) {
    uint32_t prev = unhandled_filter;
    unhandled_filter = SG_ARG(0); /* recorded, never called (runtime-abi 10.5) */
    SG_RETURN_STDCALL(prev, 4);
}
SGC_SHIM(kernel32, UnhandledExceptionFilter) { SG_RETURN_STDCALL(0, 4); }
SGC_SHIM(msvcr90, _terminate) { core_terminate(ctx, "terminate() called by guest code"); }

void core_eh_register_thunks(void) {
    sg_thunk_fn[SLOT_CATCH_GUARD] = catch_guard_fn;
    core_thunk_catch_guard = SG_THUNK_ADDR(SLOT_CATCH_GUARD);
}
