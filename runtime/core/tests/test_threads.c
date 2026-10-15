/* test_threads.c -- guest threads, handles, events, semaphores, mutexes, critical sections, waits,
 * APCs, ExitThread, per-thread TLS and TEB, exceptions on a worker thread, and a message-passing
 * check of the "every sync shim is a full barrier" rule (runtime-core.md 6.4).
 * Thread entry points are fixture functions with the stdcall shape `DWORD WINAPI f(void *)`. */
#include <sys/time.h>
#include <unistd.h>

#include "core_internal.h"
#include "fixture.h"

#define S(name) SGC_SHIM(kernel32, name);
S(CreateThread) S(ResumeThread) S(GetExitCodeThread) S(GetCurrentThread) S(GetCurrentThreadId) S(GetCurrentProcess)
S(InitializeCriticalSection) S(InitializeCriticalSectionAndSpinCount) S(DeleteCriticalSection) S(EnterCriticalSection)
S(LeaveCriticalSection) S(TryEnterCriticalSection) S(CreateEventA) S(SetEvent) S(ResetEvent) S(CreateSemaphoreA)
S(ReleaseSemaphore) S(CreateMutexA) S(ReleaseMutex) S(CloseHandle) S(DuplicateHandle) S(WaitForSingleObject)
S(WaitForMultipleObjects) S(WaitForMultipleObjectsEx) S(Sleep) S(SleepEx) S(QueueUserAPC) S(ExitThread)
S(TlsAlloc) S(TlsGetValue) S(TlsSetValue)
#undef S
SGC_SHIM(msvcr90, _beginthreadex);

enum { K_E1 = 200, K_E2, K_E3, K_E4, K_E5, K_E6, K_E7, K_APC, K_E8, K_E8HELP, K_E10, K_E11, K_E12, K_CATCH_A = 20,
       K_THROW = 3 };

#define TCALL(fn, ...) gcall(ctx, fn, 0, sizeof((uint32_t[]){__VA_ARGS__}) / 4, (uint32_t[]){__VA_ARGS__})
#define TCALL0(fn) gcall(ctx, fn, 0, 0, NULL)
static uint32_t g_flag;
static void flag_set(uint8_t *mem, uint32_t v) { __atomic_store_n((uint32_t *)(void *)(mem + g_flag), v, __ATOMIC_SEQ_CST); }
static uint32_t g_counter;
static uint32_t counter(void) { return __atomic_load_n((uint32_t *)(void *)(M + g_counter), __ATOMIC_SEQ_CST); }
static uint32_t gle(sg_ctx *ctx) { return sg_ld32(ctx->mem, ctx->fs_base + SG_TEB_LAST_ERROR); }

static uint32_t g_tls_idx, g_cs, g_ev[2], g_sem, g_mtx, g_area, g_results;
static int g_apc_tid, g_apc_data, g_sleep_ret; /* written by one thread, read after a join */

/* E1(param): TLS round trip on its own TEB, the __declspec(thread) block, then return param*2 */
static void fx_e1(sg_ctx *ctx, uint8_t *mem) {
    uint32_t p = SG_ARG(0), blk;
    TCALL(sgc_kernel32_TlsSetValue, g_tls_idx, p);
    sg_st32(mem, g_results + 0, TCALL(sgc_kernel32_TlsGetValue, g_tls_idx));
    blk = sg_ld32(mem, sg_ld32(mem, ctx->fs_base + SG_TEB_TLS_POINTER));
    sg_st32(mem, g_results + 4, sg_ld32(mem, blk + 4));  /* template word 1 */
    sg_st32(mem, blk + 4, 0x77777777u);                  /* private to this thread */
    sg_st32(mem, g_results + 8, sg_ld32(mem, ctx->fs_base + SG_TEB_SELF) == ctx->fs_base);
    sg_st32(mem, g_results + 12, TCALL0(sgc_kernel32_GetCurrentThreadId));
    sg_st32(mem, g_results + 16, sg_ld32(mem, ctx->fs_base + SG_TEB_EXCEPTION_LIST));
    ctx->eax = p * 2u;
    SG_RET(4);
}
/* E2: sets a flag (used to show that a suspended thread does not run) */
static void fx_e2(sg_ctx *ctx, uint8_t *mem) {
    sg_st32(mem, g_flag, SG_ARG(0));
    ctx->eax = 5;
    SG_RET(4);
}
/* E3: 20,000 locked increments with plain loads and stores, recursion inside */
static void fx_e3(sg_ctx *ctx, uint8_t *mem) {
    int i;
    for (i = 0; i < 20000; i++) {
        TCALL(sgc_kernel32_EnterCriticalSection, g_cs);
        if ((i & 63) == 0) TCALL(sgc_kernel32_EnterCriticalSection, g_cs);
        sg_st32(mem, g_counter, sg_ld32(mem, g_counter) + 1u);
        if ((i & 63) == 0) TCALL(sgc_kernel32_LeaveCriticalSection, g_cs);
        TCALL(sgc_kernel32_LeaveCriticalSection, g_cs);
    }
    ctx->eax = 0;
    SG_RET(4);
}
/* E4: TryEnter must fail while main holds the lock, then succeed after the flag */
static void fx_e4(sg_ctx *ctx, uint8_t *mem) {
    uint32_t r1 = TCALL(sgc_kernel32_TryEnterCriticalSection, g_cs), r2 = 0;
    flag_set(mem, 1);
    while (__atomic_load_n((uint32_t *)(void *)(mem + g_flag), __ATOMIC_SEQ_CST) != 2) usleep(100);
    r2 = TCALL(sgc_kernel32_TryEnterCriticalSection, g_cs);
    if (r2) TCALL(sgc_kernel32_LeaveCriticalSection, g_cs);
    ctx->eax = r1 * 10u + r2;
    SG_RET(4);
}
/* E5(i): wait on event g_ev[0], then count */
static void fx_e5(sg_ctx *ctx, uint8_t *mem) {
    uint32_t r = TCALL(sgc_kernel32_WaitForSingleObject, g_ev[0], 0xffffffffu);
    __atomic_add_fetch((uint32_t *)(void *)(mem + g_counter), 1u, __ATOMIC_SEQ_CST);
    ctx->eax = r;
    SG_RET(4);
}
/* E6: semaphore producer: 1,000 single releases */
static void fx_e6(sg_ctx *ctx, uint8_t *mem) {
    int i;
    uint32_t bad = 0;
    for (i = 0; i < 1000; i++) {
        while (!TCALL(sgc_kernel32_ReleaseSemaphore, g_sem, 1u, 0u)) {
            if (gle(ctx) != 298) { bad++; break; }
            TCALL(sgc_kernel32_Sleep, 0u);
        }
    }
    ctx->eax = bad;
    SG_RET(4);
}
/* E7: alertable sleep; an APC must run here and end the sleep with WAIT_IO_COMPLETION */
static void fx_e7(sg_ctx *ctx, uint8_t *mem) {
    flag_set(mem, 1);
    g_sleep_ret = (int)TCALL(sgc_kernel32_SleepEx, 0xffffffffu, 1u);
    ctx->eax = 0;
    SG_RET(4);
}
static void fx_apc(sg_ctx *ctx, uint8_t *mem) {
    g_apc_tid = (int)TCALL0(sgc_kernel32_GetCurrentThreadId);
    g_apc_data = (int)SG_ARG(0);
    SG_RET(4);
}
/* E8: ExitThread(77) from a nested call; nothing after it runs */
static void fx_e8help(sg_ctx *ctx, uint8_t *mem) {
    TCALL(sgc_kernel32_ExitThread, 77u);
    sg_st32(mem, g_flag, 0xbad);
    SG_RET(0);
}
static void fx_e8(sg_ctx *ctx, uint8_t *mem) {
    SG_CALL(fx_e8help, FX_FN(K_E8) + 0x10u);
    sg_st32(mem, g_flag, 0xbad);
    ctx->eax = 1;
    SG_RET(4);
}
/* E10: C++ throw and catch on a worker thread (catcher A from test_eh.c) */
static void fx_e10(sg_ctx *ctx, uint8_t *mem) {
    SG_CALL_IND(FX_FN(K_CATCH_A), FX_FN(K_E10) + 0x10u);
    sg_st32(mem, g_results, sg_ld32(mem, ctx->fs_base));
    SG_RET(4);
}
/* E11: message passing writer: 64 plain stores, then SetEvent; wait for the ack */
static void fx_e11(sg_ctx *ctx, uint8_t *mem) {
    uint32_t round, i;
    for (round = 1; round <= 500; round++) {
        for (i = 0; i < 64; i++) sg_st32(mem, g_area + 4 * i, round * 1000u + i);
        TCALL(sgc_kernel32_SetEvent, g_ev[0]);
        TCALL(sgc_kernel32_WaitForSingleObject, g_ev[1], 0xffffffffu);
    }
    ctx->eax = 0;
    SG_RET(4);
}
/* E12: wait on a mutex owned by main (timeout), try to release it (not owner), then wait */
static void fx_e12(sg_ctx *ctx, uint8_t *mem) {
    uint32_t a = TCALL(sgc_kernel32_WaitForSingleObject, g_mtx, 0u);
    uint32_t b = TCALL(sgc_kernel32_ReleaseMutex, g_mtx), e = gle(ctx), c;
    flag_set(mem, 1);
    c = TCALL(sgc_kernel32_WaitForSingleObject, g_mtx, 5000u);
    if (c == 0) TCALL(sgc_kernel32_ReleaseMutex, g_mtx);
    sg_st32(mem, g_results, a);
    sg_st32(mem, g_results + 4, b);
    sg_st32(mem, g_results + 8, e);
    sg_st32(mem, g_results + 12, c);
    SG_RET(4);
}

static uint32_t g_foreign_closed;
static int fake_w32_close(sg_ctx *ctx, uint32_t h) { (void)ctx; g_foreign_closed = h; return h == 0x00a00004u; }

static uint32_t create(uint32_t entry, uint32_t param, uint32_t flags, uint32_t *tid) {
    uint32_t tid_va = fx_alloc(4), h = GSTD(sgc_kernel32_CreateThread, 0u, 0u, entry, param, flags, tid_va);
    if (tid) *tid = sg_ld32(M, tid_va);
    return h;
}
static uint32_t join(uint32_t h) {
    uint32_t code_va = fx_alloc(4), w = GSTD(sgc_kernel32_WaitForSingleObject, h, 10000u);
    CHECK_EQ(w, 0);
    CHECK_EQ(GSTD(sgc_kernel32_GetExitCodeThread, h, code_va), 1);
    CHECK_EQ(GSTD(sgc_kernel32_CloseHandle, h), 1);
    return sg_ld32(M, code_va);
}
static double now_ms(void) {
    struct timeval tv;
    gettimeofday(&tv, NULL);
    return tv.tv_sec * 1000.0 + tv.tv_usec / 1000.0;
}

void test_threads(void) {
    static int setup;
    uint32_t h, tid, r;
    if (!setup) {
        setup = 1;
        fx_register(FX_FN(K_E1), fx_e1);
        fx_register(FX_FN(K_E2), fx_e2);
        fx_register(FX_FN(K_E3), fx_e3);
        fx_register(FX_FN(K_E4), fx_e4);
        fx_register(FX_FN(K_E5), fx_e5);
        fx_register(FX_FN(K_E6), fx_e6);
        fx_register(FX_FN(K_E7), fx_e7);
        fx_register(FX_FN(K_APC), fx_apc);
        fx_register(FX_FN(K_E8), fx_e8);
        fx_register(FX_FN(K_E8HELP), fx_e8help);
        fx_register(FX_FN(K_E10), fx_e10);
        fx_register(FX_FN(K_E11), fx_e11);
        fx_register(FX_FN(K_E12), fx_e12);
        g_results = fx_alloc(64);
        g_counter = fx_alloc(4);
        g_flag = fx_alloc(4);
        g_cs = fx_alloc(24);
        g_area = fx_alloc(256);
    }

    /* 1. CreateThread: TLS, TEB and __declspec(thread) are per thread; exit code; handles */
    t_name = "threads.create";
    g_tls_idx = GSTD0(sgc_kernel32_TlsAlloc);
    GSTD(sgc_kernel32_TlsSetValue, g_tls_idx, 0x1111u);
    h = create(FX_FN(K_E1), 21, 0, &tid);
    CHECK(h != 0, "CreateThread failed");
    CHECK_EQ(join(h), 42);
    CHECK_EQ(sg_ld32(M, g_results + 0), 21);           /* its own slot */
    CHECK_EQ(sg_ld32(M, g_results + 4), 0x22222222u);  /* template copied */
    CHECK_EQ(sg_ld32(M, g_results + 8), 1);            /* fs:[18h] = TEB */
    CHECK_EQ(sg_ld32(M, g_results + 12), tid);
    CHECK(tid != C->thread->tid, "same tid as main");
    CHECK_EQ(sg_ld32(M, g_results + 16), SG_END_OF_CHAIN); /* a new thread's fs:[0] */
    CHECK_EQ(GSTD(sgc_kernel32_TlsGetValue, g_tls_idx), 0x1111u); /* main's slot untouched */
    CHECK_EQ(sg_ld32(M, sg_ld32(M, sg_ld32(M, C->fs_base + SG_TEB_TLS_POINTER)) + 4), 0x22222222u);
    CHECK_EQ(GSTD(sgc_kernel32_CloseHandle, h), 0);    /* already closed */
    CHECK_EQ(sg_ld32(M, C->fs_base + SG_TEB_LAST_ERROR), 6);

    /* 2. _beginthreadex with CREATE_SUSPENDED: nothing runs until ResumeThread */
    t_name = "threads.suspended";
    sg_st32(M, g_flag, 0);
    {
        uint32_t tid_va = fx_alloc(4);
        h = GCDECL(sgc_msvcr90__beginthreadex, 0u, 0x10000u, FX_FN(K_E2), 9u, 4u, tid_va);
        CHECK(h != 0, "_beginthreadex failed");
        CHECK(sg_ld32(M, tid_va) != 0, "thrdaddr not written");
        usleep(50000);
        CHECK_EQ(sg_ld32(M, g_flag), 0);
        CHECK_EQ(GSTD(sgc_kernel32_ResumeThread, h), 1);
        CHECK_EQ(join(h), 5);
        CHECK_EQ(sg_ld32(M, g_flag), 9);
    }

    /* 3. critical sections: 8 threads x 20,000 increments of a plain counter, with recursion */
    t_name = "threads.critical-section";
    GSTD(sgc_kernel32_InitializeCriticalSectionAndSpinCount, g_cs, 4000u);
    sg_st32(M, g_counter, 0);
    {
        uint32_t hs = fx_alloc(32), i;
        double t0 = now_ms();
        for (i = 0; i < 8; i++) sg_st32(M, hs + 4 * i, create(FX_FN(K_E3), i, 0, NULL));
        CHECK_EQ(GSTD(sgc_kernel32_WaitForMultipleObjects, 8u, hs, 1u, 30000u), 0);
        for (i = 0; i < 8; i++) GSTD(sgc_kernel32_CloseHandle, sg_ld32(M, hs + 4 * i));
        CHECK_EQ(sg_ld32(M, g_counter), 160000u);
        printf("  critical section: 8 threads x 20000 lock/unlock pairs in %.1f ms\n", now_ms() - t0);
    }
    CHECK_EQ(sg_ld32(M, g_cs + 12), 0); /* OwningThread */
    CHECK_EQ(sg_ld32(M, g_cs + 8), 0);  /* RecursionCount */
    /* TryEnter from another thread while main holds it */
    GSTD(sgc_kernel32_EnterCriticalSection, g_cs);
    sg_st32(M, g_flag, 0);
    h = create(FX_FN(K_E4), 0, 0, NULL);
    while (__atomic_load_n((uint32_t *)(void *)(M + g_flag), __ATOMIC_SEQ_CST) != 1) usleep(100);
    GSTD(sgc_kernel32_LeaveCriticalSection, g_cs);
    __atomic_store_n((uint32_t *)(void *)(M + g_flag), 2u, __ATOMIC_SEQ_CST);
    CHECK_EQ(join(h), 1); /* first TryEnter 0, second 1 */
    GSTD(sgc_kernel32_DeleteCriticalSection, g_cs);

    /* 4. events: auto-reset wakes exactly one waiter per SetEvent; manual-reset stays signalled */
    t_name = "threads.events";
    g_ev[0] = GSTD(sgc_kernel32_CreateEventA, 0u, 0u, 0u, 0u);
    sg_st32(M, g_counter, 0);
    {
        uint32_t h1 = create(FX_FN(K_E5), 1, 0, NULL), h2 = create(FX_FN(K_E5), 2, 0, NULL);
        usleep(30000);
        CHECK_EQ(counter(), 0);
        GSTD(sgc_kernel32_SetEvent, g_ev[0]);
        usleep(50000);
        CHECK_EQ(counter(), 1);
        GSTD(sgc_kernel32_SetEvent, g_ev[0]);
        CHECK_EQ(join(h1), 0);
        CHECK_EQ(join(h2), 0);
        CHECK_EQ(counter(), 2);
        CHECK_EQ(GSTD(sgc_kernel32_WaitForSingleObject, g_ev[0], 0u), 0x102u); /* reset by the wake */
    }
    GSTD(sgc_kernel32_CloseHandle, g_ev[0]);
    g_ev[0] = GSTD(sgc_kernel32_CreateEventA, 0u, 1u, 0u, 0u); /* manual */
    sg_st32(M, g_counter, 0);
    {
        uint32_t h1 = create(FX_FN(K_E5), 1, 0, NULL), h2 = create(FX_FN(K_E5), 2, 0, NULL);
        usleep(20000);
        GSTD(sgc_kernel32_SetEvent, g_ev[0]);
        join(h1);
        join(h2);
        CHECK_EQ(counter(), 2);
        CHECK_EQ(GSTD(sgc_kernel32_WaitForSingleObject, g_ev[0], 0u), 0);
        GSTD(sgc_kernel32_ResetEvent, g_ev[0]);
        CHECK_EQ(GSTD(sgc_kernel32_WaitForSingleObject, g_ev[0], 0u), 0x102u);
    }
    /* timeouts and WaitForMultipleObjects any/all */
    {
        double t0 = now_ms(), dt;
        uint32_t arr = fx_alloc(8);
        r = GSTD(sgc_kernel32_WaitForSingleObject, g_ev[0], 100u);
        dt = now_ms() - t0;
        CHECK_EQ(r, 0x102u);
        CHECK(dt >= 95.0 && dt < 400.0, "100 ms timeout took %.1f ms", dt);
        g_ev[1] = GSTD(sgc_kernel32_CreateEventA, 0u, 1u, 0u, 0u);
        sg_st32(M, arr, g_ev[0]);
        sg_st32(M, arr + 4, g_ev[1]);
        GSTD(sgc_kernel32_SetEvent, g_ev[1]);
        CHECK_EQ(GSTD(sgc_kernel32_WaitForMultipleObjects, 2u, arr, 0u, 0u), 1);
        CHECK_EQ(GSTD(sgc_kernel32_WaitForMultipleObjects, 2u, arr, 1u, 30u), 0x102u);
        GSTD(sgc_kernel32_SetEvent, g_ev[0]);
        CHECK_EQ(GSTD(sgc_kernel32_WaitForMultipleObjects, 2u, arr, 1u, 0u), 0);
        CHECK_EQ(GSTD(sgc_kernel32_WaitForMultipleObjects, 0u, arr, 1u, 0u), 0xffffffffu);
        sg_st32(M, arr + 4, 0x12345678u);
        CHECK_EQ(GSTD(sgc_kernel32_WaitForMultipleObjects, 2u, arr, 0u, 0u), 0xffffffffu);
        CHECK_EQ(sg_ld32(M, C->fs_base + SG_TEB_LAST_ERROR), 6);
        GSTD(sgc_kernel32_CloseHandle, g_ev[0]);
        GSTD(sgc_kernel32_CloseHandle, g_ev[1]);
    }

    /* 5. semaphores: 1,000 releases consumed one by one; the maximum is enforced */
    t_name = "threads.semaphore";
    g_sem = GSTD(sgc_kernel32_CreateSemaphoreA, 0u, 0u, 4u, 0u);
    h = create(FX_FN(K_E6), 0, 0, NULL);
    {
        int i, ok = 1;
        for (i = 0; i < 1000; i++) ok &= GSTD(sgc_kernel32_WaitForSingleObject, g_sem, 5000u) == 0;
        CHECK(ok, "a semaphore wait failed");
    }
    CHECK_EQ(join(h), 0);
    {
        uint32_t prev = fx_alloc(4);
        CHECK_EQ(GSTD(sgc_kernel32_ReleaseSemaphore, g_sem, 3u, prev), 1);
        CHECK_EQ(sg_ld32(M, prev), 0);
        CHECK_EQ(GSTD(sgc_kernel32_ReleaseSemaphore, g_sem, 2u, prev), 0);
        CHECK_EQ(sg_ld32(M, C->fs_base + SG_TEB_LAST_ERROR), 298);
        CHECK_EQ(GSTD(sgc_kernel32_CreateSemaphoreA, 0u, 5u, 4u, 0u), 0);
    }
    GSTD(sgc_kernel32_CloseHandle, g_sem);

    /* 6. mutexes: named, recursive, owner-only release, waiting threads */
    t_name = "threads.mutex";
    {
        uint32_t name = fx_str("SporeFixtureMutex"), m2;
        g_mtx = GSTD(sgc_kernel32_CreateMutexA, 0u, 1u, name);
        CHECK(g_mtx != 0, "CreateMutexA failed");
        CHECK_EQ(sg_ld32(M, C->fs_base + SG_TEB_LAST_ERROR), 0);
        m2 = GSTD(sgc_kernel32_CreateMutexA, 0u, 0u, name);
        CHECK_EQ(sg_ld32(M, C->fs_base + SG_TEB_LAST_ERROR), 183); /* ERROR_ALREADY_EXISTS */
        CHECK_EQ(GSTD(sgc_kernel32_WaitForSingleObject, m2, 0u), 0); /* recursive: same object, owned */
        CHECK_EQ(GSTD(sgc_kernel32_ReleaseMutex, m2), 1);
        sg_st32(M, g_flag, 0);
        h = create(FX_FN(K_E12), 0, 0, NULL);
        while (__atomic_load_n((uint32_t *)(void *)(M + g_flag), __ATOMIC_SEQ_CST) != 1) usleep(100);
        usleep(10000);
        CHECK_EQ(GSTD(sgc_kernel32_ReleaseMutex, g_mtx), 1);
        join(h);
        CHECK_EQ(sg_ld32(M, g_results + 0), 0x102u);
        CHECK_EQ(sg_ld32(M, g_results + 4), 0);
        CHECK_EQ(sg_ld32(M, g_results + 8), 288);  /* ERROR_NOT_OWNER */
        CHECK_EQ(sg_ld32(M, g_results + 12), 0);
        CHECK_EQ(GSTD(sgc_kernel32_ReleaseMutex, g_mtx), 0);
        GSTD(sgc_kernel32_CloseHandle, m2);
        GSTD(sgc_kernel32_CloseHandle, g_mtx);
        /* the name is gone with the last handle */
        g_mtx = GSTD(sgc_kernel32_CreateMutexA, 0u, 0u, name);
        CHECK_EQ(sg_ld32(M, C->fs_base + SG_TEB_LAST_ERROR), 0);
        GSTD(sgc_kernel32_CloseHandle, g_mtx);
    }

    /* 7. QueueUserAPC runs the APC on the target thread inside its alertable SleepEx */
    t_name = "threads.apc";
    sg_st32(M, g_flag, 0);
    g_apc_tid = 0;
    h = create(FX_FN(K_E7), 0, 0, &tid);
    while (__atomic_load_n((uint32_t *)(void *)(M + g_flag), __ATOMIC_SEQ_CST) != 1) usleep(100);
    usleep(20000);
    CHECK_EQ(GSTD(sgc_kernel32_QueueUserAPC, FX_FN(K_APC), h, 0x99u), 1);
    join(h);
    CHECK_EQ((uint32_t)g_apc_tid, tid);
    CHECK_EQ(g_apc_data, 0x99);
    CHECK_EQ(g_sleep_ret, 0xc0);

    /* 8. ExitThread from a nested call */
    t_name = "threads.exit-thread";
    sg_st32(M, g_flag, 0);
    h = create(FX_FN(K_E8), 0, 0, NULL);
    CHECK_EQ(join(h), 77);
    CHECK_EQ(sg_ld32(M, g_flag), 0);

    /* 9. a C++ exception thrown and caught on a worker thread */
    t_name = "threads.eh-worker";
    {
        extern void test_eh_prepare_catcher(uint32_t callee);
        test_eh_prepare_catcher(FX_FN(K_THROW));
        h = create(FX_FN(K_E10), 0, 0, NULL);
        CHECK_EQ(join(h), 1); /* eax of the catcher's continuation path */
        CHECK_EQ(sg_ld32(M, g_results), SG_END_OF_CHAIN);
        CHECK_EQ(sg_ld32(M, C->fs_base), SG_END_OF_CHAIN);
    }

    /* 10. message passing through SetEvent/Wait with plain guest stores: 500 rounds x 64 words */
    t_name = "threads.message-passing";
    g_ev[0] = GSTD(sgc_kernel32_CreateEventA, 0u, 0u, 0u, 0u);
    g_ev[1] = GSTD(sgc_kernel32_CreateEventA, 0u, 0u, 0u, 0u);
    h = create(FX_FN(K_E11), 0, 0, NULL);
    {
        uint32_t round, i, bad = 0;
        for (round = 1; round <= 500; round++) {
            GSTD(sgc_kernel32_WaitForSingleObject, g_ev[0], 0xffffffffu);
            for (i = 0; i < 64; i++) bad += sg_ld32(M, g_area + 4 * i) != round * 1000u + i;
            GSTD(sgc_kernel32_SetEvent, g_ev[1]);
        }
        CHECK_EQ(bad, 0);
    }
    join(h);
    GSTD(sgc_kernel32_CloseHandle, g_ev[0]);
    GSTD(sgc_kernel32_CloseHandle, g_ev[1]);

    /* 11. DuplicateHandle of the pseudo handle gives a real, waitable handle to this thread */
    t_name = "threads.duplicate";
    {
        uint32_t out = fx_alloc(4), code = fx_alloc(4), hp = GSTD0(sgc_kernel32_GetCurrentProcess);
        CHECK_EQ(GSTD(sgc_kernel32_DuplicateHandle, hp, GSTD0(sgc_kernel32_GetCurrentThread), hp, out, 0u, 0u, 2u), 1);
        h = sg_ld32(M, out);
        CHECK(h >= 0x100 && h != 0xfffffffeu, "duplicate handle 0x%x", h);
        CHECK_EQ(GSTD(sgc_kernel32_GetExitCodeThread, h, code), 1);
        CHECK_EQ(sg_ld32(M, code), 259);
        CHECK_EQ(GSTD(sgc_kernel32_WaitForSingleObject, h, 0u), 0x102u);
        CHECK_EQ(GSTD(sgc_kernel32_CloseHandle, h), 1);
    }

    /* 12. 64 threads started and joined; the registry and the stack/TEB pools return to idle */
    t_name = "threads.many";
    {
        uint32_t hs = fx_alloc(256), i, before = sg_core_thread_count();
        for (i = 0; i < 64; i++) sg_st32(M, hs + 4 * i, create(FX_FN(K_E2), i + 100, 0, NULL));
        CHECK_EQ(GSTD(sgc_kernel32_WaitForMultipleObjects, 64u, hs, 1u, 30000u), 0);
        for (i = 0; i < 64; i++) GSTD(sgc_kernel32_CloseHandle, sg_ld32(M, hs + 4 * i));
        usleep(20000);
        CHECK_EQ(sg_core_thread_count(), before);
        for (i = 0; i < 3; i++) { /* reuse of freed stacks and TEBs */
            h = create(FX_FN(K_E2), 1, 0, NULL);
            CHECK_EQ(join(h), 5);
        }
    }
    t_name = "threads.external-handle";
    {
        static int closed;
        static int obj = 42;
        uint32_t hx;
        closed = 0;
        hx = sg_handle_alloc_external(SG_H_EXTERNAL + 3, &obj, (void (*)(void *))(void *)0);
        CHECK(sg_handle_external(hx, SG_H_EXTERNAL + 3) == &obj, "external lookup");
        CHECK(sg_handle_external(hx, SG_H_EXTERNAL + 4) == NULL, "wrong tag must fail");
        CHECK_EQ(sg_handle_type(hx), SG_H_EXTERNAL + 3);
        CHECK_EQ(GSTD(sgc_kernel32_WaitForSingleObject, hx, 0u), 0);
        CHECK_EQ(GSTD(sgc_kernel32_CloseHandle, hx), 1);
        CHECK_EQ(sg_handle_type(hx), SG_H_NONE);
        (void)closed;
    }
    t_name = "threads.close-hooks";
    {
        /* CloseHandle on another package's handle goes to sg_core_close_hook; runtime/win32's CloseHandle
         * reaches core objects through sg_core_close_foreign */
        uint32_t ev = GSTD(sgc_kernel32_CreateEventA, 0u, 0u, 0u, 0u);
        CHECK_EQ(GSTD(sgc_kernel32_CloseHandle, 0x00a00004u), 0);
        CHECK_EQ(sg_ld32(M, C->fs_base + SG_TEB_LAST_ERROR), 6);
        sg_core_close_hook = fake_w32_close;
        CHECK_EQ(GSTD(sgc_kernel32_CloseHandle, 0x00a00004u), 1);
        CHECK_EQ(g_foreign_closed, 0x00a00004u);
        sg_core_close_hook = NULL;
        CHECK_EQ(sg_core_close_foreign(C, ev), 1);
        CHECK_EQ(sg_core_close_foreign(C, ev), 0);
    }
}
