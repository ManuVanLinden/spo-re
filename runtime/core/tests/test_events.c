/* test_events.c -- the core event API for other runtime packages (sg_core.h section 5,
 * added 2026-10-12). It was first tested only in the guest platform fixture. Covers: auto and manual reset, initial state, timeouts, a guest worker
 * thread blocked in WaitForSingleObject / WaitForMultipleObjects woken by a host thread that is
 * not a guest thread (FSEvents queue, CoreAudio render thread), host-thread waits woken by the
 * guest SetEvent shim, two host waiters on a manual event, sg_core_event_wait on a guest thread,
 * guest ResetEvent/CloseHandle on API handles, non-event and closed handles, and a host-thread
 * stress of 2,000 sets each consumed exactly once. */
#include <pthread.h>
#include <sched.h>
#include <sys/time.h>
#include <unistd.h>

#include "core_internal.h"
#include "fixture.h"

#define S(name) SGC_SHIM(kernel32, name);
S(CreateThread) S(GetExitCodeThread) S(CloseHandle) S(WaitForSingleObject) S(WaitForMultipleObjects) S(SetEvent)
S(ResetEvent) S(CreateEventA) S(CreateSemaphoreA) S(DuplicateHandle)
#undef S

enum { K_W1 = 300, K_W2, K_W3 };
#define TCALL(fn, ...) gcall(ctx, fn, 0, sizeof((uint32_t[]){__VA_ARGS__}) / 4, (uint32_t[]){__VA_ARGS__})

static uint32_t g_ev, g_ev2, g_out, g_count;
static volatile uint32_t ev_host; /* for the host threads */

static double now_ms(void) {
    struct timeval tv;
    gettimeofday(&tv, NULL);
    return tv.tv_sec * 1000.0 + tv.tv_usec / 1000.0;
}

/* W1(timeout): WaitForSingleObject(g_ev, timeout); returns the wait result */
static void fx_w1(sg_ctx *ctx, uint8_t *mem) {
    ctx->eax = TCALL(sgc_kernel32_WaitForSingleObject, g_ev, SG_ARG(0));
    SG_RET(4);
}
/* W2: WaitForMultipleObjects({g_ev, g_ev2}, any, 5000); returns the index */
static void fx_w2(sg_ctx *ctx, uint8_t *mem) {
    ctx->eax = TCALL(sgc_kernel32_WaitForMultipleObjects, 2u, g_out, 0u, 5000u);
    SG_RET(4);
}
/* W3(n): takes the auto-reset g_ev n times (5 s each), counting in g_count */
static void fx_w3(sg_ctx *ctx, uint8_t *mem) {
    uint32_t n = SG_ARG(0), i, ok = 0;
    for (i = 0; i < n; i++) {
        if (TCALL(sgc_kernel32_WaitForSingleObject, g_ev, 5000u) != 0) break;
        ok++;
        __atomic_store_n((uint32_t *)(void *)(mem + g_count), ok, __ATOMIC_SEQ_CST);
    }
    ctx->eax = ok;
    SG_RET(4);
}

static uint32_t create(uint32_t entry, uint32_t param) {
    uint32_t tid_va = fx_alloc(4);
    return GSTD(sgc_kernel32_CreateThread, 0u, 0u, entry, param, 0u, tid_va);
}
static uint32_t join(uint32_t h) {
    uint32_t code_va = fx_alloc(4);
    CHECK_EQ(GSTD(sgc_kernel32_WaitForSingleObject, h, 10000u), 0);
    CHECK_EQ(GSTD(sgc_kernel32_GetExitCodeThread, h, code_va), 1);
    CHECK_EQ(GSTD(sgc_kernel32_CloseHandle, h), 1);
    return sg_ld32(M, code_va);
}

static void *host_setter(void *arg) { /* a plain pthread: no guest context, no TEB */
    usleep((useconds_t)(uintptr_t)arg);
    return (void *)(uintptr_t)sg_core_event_set(ev_host);
}
static void *host_waiter(void *arg) {
    (void)arg;
    return (void *)(uintptr_t)sg_core_event_wait(ev_host, 5000);
}
static uint32_t count(void) { return __atomic_load_n((uint32_t *)(void *)(M + g_count), __ATOMIC_SEQ_CST); }

void test_events(void) {
    static int setup;
    pthread_t t, t2;
    void *res;
    uint32_t h, m, th;
    double t0, dt;
    if (!setup) {
        setup = 1;
        fx_register(FX_FN(K_W1), fx_w1);
        fx_register(FX_FN(K_W2), fx_w2);
        fx_register(FX_FN(K_W3), fx_w3);
        g_out = fx_alloc(8);
        g_count = fx_alloc(4);
    }

    /* 1. creation, states, the handle table */
    t_name = "events.basic";
    h = sg_core_event_create(0, 0);
    CHECK(h != 0, "event create");
    CHECK_EQ(sg_handle_type(h), SG_H_EVENT);
    CHECK_EQ(sg_core_event_wait(h, 0), 0x102);
    CHECK_EQ(sg_core_event_set(h), 1);
    CHECK_EQ(sg_core_event_set(h), 1);                  /* setting a set event is not counted */
    CHECK_EQ(sg_core_event_wait(h, 0), 0);
    CHECK_EQ(sg_core_event_wait(h, 0), 0x102);          /* auto-reset: one take */
    m = sg_core_event_create(1, 1);                     /* manual, initially signalled */
    CHECK_EQ(sg_core_event_wait(m, 0), 0);
    CHECK_EQ(sg_core_event_wait(m, 0), 0);
    CHECK_EQ(GSTD(sgc_kernel32_WaitForSingleObject, m, 0u), 0); /* the guest sees the same object */
    CHECK_EQ(GSTD(sgc_kernel32_ResetEvent, m), 1);              /* guest reset of an API handle */
    CHECK_EQ(sg_core_event_wait(m, 0), 0x102);
    CHECK_EQ(sg_core_event_set(m), 1);
    CHECK_EQ(sg_core_event_reset(m), 1);
    CHECK_EQ(GSTD(sgc_kernel32_WaitForSingleObject, m, 0u), 0x102u);
    /* the auto-reset event from the API and from CreateEventA behave alike */
    {
        uint32_t g = GSTD(sgc_kernel32_CreateEventA, 0u, 0u, 1u, 0u);
        CHECK_EQ(sg_core_event_wait(g, 0), 0);          /* initial state 1, taken */
        CHECK_EQ(sg_core_event_wait(g, 0), 0x102);
        CHECK_EQ(sg_core_event_set(g), 1);              /* the API on a CreateEventA handle */
        CHECK_EQ(GSTD(sgc_kernel32_WaitForSingleObject, g, 0u), 0);
        CHECK_EQ(GSTD(sgc_kernel32_CloseHandle, g), 1);
    }

    /* 2. timeouts on the host path (main is a guest thread, so use a plain pthread for that) */
    t_name = "events.timeout";
    t0 = now_ms();
    CHECK_EQ(sg_core_event_wait(h, 100), 0x102);        /* guest thread: sg_core_wait */
    dt = now_ms() - t0;
    CHECK(dt >= 95.0 && dt < 400.0, "guest-thread 100 ms wait took %.1f ms", dt);
    ev_host = h;                                        /* a host waiter, set from this guest thread */
    pthread_create(&t, NULL, host_waiter, NULL);
    usleep(30000);
    CHECK_EQ(sg_core_event_set(h), 1);
    pthread_join(t, &res);
    CHECK_EQ((uintptr_t)res, 0);

    /* 3. a guest worker blocked in WaitForSingleObject, woken by a host thread */
    t_name = "events.guest-woken-by-host";
    g_ev = sg_core_event_create(0, 0);
    th = create(FX_FN(K_W1), 5000u);
    usleep(30000);
    ev_host = g_ev;
    t0 = now_ms();
    pthread_create(&t, NULL, host_setter, (void *)(uintptr_t)1000);
    pthread_join(t, &res);
    CHECK_EQ((uintptr_t)res, 1);
    CHECK_EQ(join(th), 0);                              /* WAIT_OBJECT_0, not the 5 s timeout */
    dt = now_ms() - t0;
    CHECK(dt < 1000.0, "guest wake took %.1f ms", dt);
    CHECK_EQ(sg_core_event_wait(g_ev, 0), 0x102);       /* the worker took it (auto-reset) */
    /* a guest timeout while nobody sets */
    th = create(FX_FN(K_W1), 50u);
    CHECK_EQ(join(th), 0x102);

    /* 4. WaitForMultipleObjects(any) on a worker: the host sets the second handle */
    t_name = "events.wait-multiple";
    g_ev2 = sg_core_event_create(1, 0);
    sg_st32(M, g_out, g_ev);
    sg_st32(M, g_out + 4, g_ev2);
    th = create(FX_FN(K_W2), 0u);
    usleep(30000);
    ev_host = g_ev2;
    pthread_create(&t, NULL, host_setter, (void *)(uintptr_t)1000);
    pthread_join(t, NULL);
    CHECK_EQ(join(th), 1);
    CHECK_EQ(sg_core_event_wait(g_ev2, 0), 0);          /* manual: still set after the wake */

    /* 5. host-thread waits woken by the guest SetEvent shim, two waiters on a manual event */
    t_name = "events.host-waiters";
    ev_host = h;
    pthread_create(&t, NULL, host_waiter, NULL);
    usleep(30000);
    CHECK_EQ(GSTD(sgc_kernel32_SetEvent, h), 1);
    pthread_join(t, &res);
    CHECK_EQ((uintptr_t)res, 0);
    CHECK_EQ(sg_core_event_reset(m), 1);
    ev_host = m;
    pthread_create(&t, NULL, host_waiter, NULL);
    pthread_create(&t2, NULL, host_waiter, NULL);
    usleep(30000);
    CHECK_EQ(GSTD(sgc_kernel32_SetEvent, m), 1);
    pthread_join(t, &res);
    CHECK_EQ((uintptr_t)res, 0);
    pthread_join(t2, &res);
    CHECK_EQ((uintptr_t)res, 0);

    /* 6. stress: 2,000 host sets, each consumed exactly once by a guest worker (auto-reset) */
    t_name = "events.stress";
    sg_st32(M, g_count, 0);
    th = create(FX_FN(K_W3), 2000u);
    {
        int i, spins = 0, ok = 1;
        t0 = now_ms();
        for (i = 0; i < 2000; i++) {
            uint32_t before = count();
            ok &= sg_core_event_set(g_ev) == 1;
            while (count() == before && spins < 50000000) spins++, sched_yield();
        }
        CHECK(ok, "a host set failed");
        CHECK_EQ(join(th), 2000);
        printf("  events: 2000 host set -> guest wake round trips in %.1f ms\n", now_ms() - t0);
    }
    CHECK_EQ(sg_core_event_wait(g_ev, 0), 0x102);

    /* 7. wrong and closed handles */
    t_name = "events.handles";
    {
        uint32_t sem = GSTD(sgc_kernel32_CreateSemaphoreA, 0u, 1u, 1u, 0u);
        CHECK_EQ(sg_core_event_set(sem), 0);            /* not an event */
        CHECK_EQ(sg_core_event_reset(sem), 0);
        CHECK_EQ(sg_core_event_wait(sem, 0), 0);        /* guest thread: sg_core_wait accepts any waitable */
        CHECK_EQ(GSTD(sgc_kernel32_CloseHandle, sem), 1);
        CHECK_EQ(sg_core_event_set(0), 0);
        CHECK_EQ(sg_core_event_set(0x12345678u), 0);
    }
    {
        /* a duplicated handle is the same object; closing one keeps the other */
        uint32_t out = fx_alloc(4);
        CHECK_EQ(GSTD(sgc_kernel32_DuplicateHandle, 0xffffffffu, m, 0xffffffffu, out, 0u, 0u, 2u), 1);
        CHECK_EQ(sg_handle_close(m), 1);
        CHECK_EQ(sg_core_event_reset(sg_ld32(M, out)), 1);
        CHECK_EQ(sg_core_event_set(sg_ld32(M, out)), 1);
        CHECK_EQ(GSTD(sgc_kernel32_WaitForSingleObject, sg_ld32(M, out), 0u), 0);
        CHECK_EQ(GSTD(sgc_kernel32_CloseHandle, sg_ld32(M, out)), 1);
    }
    CHECK_EQ(GSTD(sgc_kernel32_CloseHandle, h), 1);     /* the guest closes an API handle */
    CHECK_EQ(sg_core_event_set(h), 0);
    CHECK_EQ(sg_core_event_reset(h), 0);
    CHECK_EQ(sg_core_event_wait(h, 0), 0xffffffffu);
    ev_host = h;                                        /* closed, on the host path */
    pthread_create(&t, NULL, host_waiter, NULL);
    pthread_join(t, &res);
    CHECK_EQ((uintptr_t)res, 0xffffffffu);
    CHECK_EQ(sg_handle_close(g_ev), 1);
    CHECK_EQ(sg_handle_close(g_ev2), 1);
    CHECK_EQ(sg_handle_close(g_ev), 0);
}
