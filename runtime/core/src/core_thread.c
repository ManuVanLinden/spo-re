/* core_thread.c -- guest threads over pthreads, the handle table, kernel objects (events,
 * semaphores, mutexes, threads), waits, APCs, Sleep, critical sections, and their shims.
 *
 * Memory ordering (runtime-core.md 6.4, required of the runtime whatever the translator's policy): every
 * synchronization shim is a full barrier. All kernel-object state is guarded by one mutex (ko_lock), and each shim also issues a sequentially consistent fence on entry and
 * exit, so plain guest stores before a release/set/leave are visible to plain guest loads after
 * the matching wait/enter on another thread, whatever the translator's access policy. */
#include <errno.h>
#include <sched.h>
#include <signal.h>
#include <sys/time.h>
#include <time.h>
#include <unistd.h>

#include "core_internal.h"

SG_THREAD_LOCAL sg_thread *sg_self;

/* ---- kernel objects and the handle table --------------------------------------------------- */

typedef struct core_kobj {
    int type;            /* SG_H_* or an external tag */
    int refs;            /* open handles + internal references (a running thread holds one) */
    char *name;          /* named objects share one namespace (Windows: per session) */
    struct core_kobj *name_next;
    /* event */
    int manual, signaled;
    /* semaphore */
    int32_t count, max;
    /* mutex */
    core_thread *owner;
    int recursion;
    /* thread */
    sg_thread *thread;
    int exited;
    uint32_t exit_code;
    /* external */
    void *ext;
    void (*close)(void *);
} core_kobj;

static pthread_mutex_t ko_lock = PTHREAD_MUTEX_INITIALIZER;
#define MAX_HANDLES 16384u
#define HANDLE_BASE 0x100u
static core_kobj *handles[MAX_HANDLES];
static unsigned handle_hint;
static core_kobj *named;
static core_thread *waiting_threads; /* threads blocked in core_wait */

void core_kobj_init(void) {}

static uint32_t handle_put_locked(core_kobj *o) {
    unsigned i, k;
    for (k = 0; k < MAX_HANDLES; k++) {
        i = (handle_hint + k) % MAX_HANDLES;
        if (!handles[i]) {
            handles[i] = o;
            o->refs++;
            handle_hint = i + 1;
            return HANDLE_BASE + 4u * i;
        }
    }
    return 0;
}

static core_kobj *handle_get_locked(uint32_t h) {
    uint32_t i;
    if (h < HANDLE_BASE || (h & 3u)) return NULL;
    i = (h - HANDLE_BASE) / 4u;
    return i < MAX_HANDLES ? handles[i] : NULL;
}

static void kobj_unref_locked(core_kobj *o, void (**ext_close)(void *), void **ext_obj, sg_thread **free_thread) {
    if (--o->refs > 0) return;
    if (o->name) {
        core_kobj **p;
        for (p = &named; *p; p = &(*p)->name_next)
            if (*p == o) { *p = o->name_next; break; }
        free(o->name);
    }
    if (o->type >= SG_H_EXTERNAL && o->close) { *ext_close = o->close; *ext_obj = o->ext; }
    if (o->type == SG_H_THREAD) *free_thread = o->thread;
    free(o);
}

static void free_thread_struct(sg_thread *t) {
    core_thread *h;
    if (!t) return;
    h = core_th(t);
    pthread_cond_destroy(&h->cv);
    free(h);
    free(t);
}

static void kobj_unref(core_kobj *o) {
    void (*c)(void *) = NULL;
    void *e = NULL;
    sg_thread *ft = NULL;
    pthread_mutex_lock(&ko_lock);
    kobj_unref_locked(o, &c, &e, &ft);
    pthread_mutex_unlock(&ko_lock);
    if (c) c(e);
    free_thread_struct(ft);
}

static core_kobj *kobj_new(int type) {
    core_kobj *o = calloc(1, sizeof *o);
    if (o) o->type = type;
    return o;
}

uint32_t sg_handle_alloc_external(int tag, void *obj, void (*close)(void *)) {
    core_kobj *o = kobj_new(tag < SG_H_EXTERNAL ? SG_H_EXTERNAL : tag);
    uint32_t h;
    if (!o) return 0;
    o->ext = obj;
    o->close = close;
    pthread_mutex_lock(&ko_lock);
    h = handle_put_locked(o);
    pthread_mutex_unlock(&ko_lock);
    if (!h) free(o);
    return h;
}

void *sg_handle_external(uint32_t h, int tag) {
    core_kobj *o;
    void *r = NULL;
    pthread_mutex_lock(&ko_lock);
    o = handle_get_locked(h);
    if (o && o->type == tag) r = o->ext;
    pthread_mutex_unlock(&ko_lock);
    return r;
}

int sg_handle_type(uint32_t h) {
    core_kobj *o;
    int t;
    if (h == SG_PSEUDO_CURRENT_THREAD) return SG_H_THREAD;
    pthread_mutex_lock(&ko_lock);
    o = handle_get_locked(h);
    t = o ? o->type : SG_H_NONE;
    pthread_mutex_unlock(&ko_lock);
    return t;
}

int sg_handle_close(uint32_t h) {
    core_kobj *o;
    void (*c)(void *) = NULL;
    void *e = NULL;
    sg_thread *ft = NULL;
    if (h == SG_PSEUDO_CURRENT_THREAD || h == SG_PSEUDO_CURRENT_PROCESS) return 1; /* no-op on Windows */
    pthread_mutex_lock(&ko_lock);
    o = handle_get_locked(h);
    if (o) {
        handles[(h - HANDLE_BASE) / 4u] = NULL;
        kobj_unref_locked(o, &c, &e, &ft);
    }
    pthread_mutex_unlock(&ko_lock);
    if (c) c(e);
    free_thread_struct(ft);
    return o != NULL;
}

/* Finds or creates a named object. *existed set when the name was already in use (then the
 * types must match, else NULL with ERROR_INVALID_HANDLE as Windows does). Caller holds ko_lock. */
static core_kobj *named_lookup_locked(const char *name, int type, int *existed) {
    core_kobj *o;
    *existed = 0;
    if (!name) return NULL;
    for (o = named; o; o = o->name_next)
        if (!strcmp(o->name, name)) {
            *existed = 1;
            return o->type == type ? o : NULL;
        }
    return NULL;
}

static void named_add_locked(core_kobj *o, const char *name) {
    o->name = strdup(name);
    o->name_next = named;
    named = o;
}

/* ---- waiting --------------------------------------------------------------------------------- */

static int ko_ready(const core_kobj *o, const core_thread *self) {
    switch (o->type) {
    case SG_H_EVENT: return o->signaled;
    case SG_H_SEMAPHORE: return o->count > 0;
    case SG_H_MUTEX: return o->owner == NULL || o->owner == self;
    case SG_H_THREAD: return o->exited;
    default: return 1; /* files and other external objects: signalled (no async I/O here) */
    }
}

static void ko_acquire(core_kobj *o, core_thread *self) {
    switch (o->type) {
    case SG_H_EVENT: if (!o->manual) o->signaled = 0; break;
    case SG_H_SEMAPHORE: o->count--; break;
    case SG_H_MUTEX: o->owner = self; o->recursion++; break;
    default: break;
    }
}

/* Wakes every waiting thread whose wait set contains o (or all if o is NULL). Holds ko_lock. */
static pthread_cond_t host_wait_cv = PTHREAD_COND_INITIALIZER; /* sg_core_event_wait on non-guest threads */
static int host_waiters;
static void ko_wake_locked(const core_kobj *o) {
    core_thread *w;
    if (host_waiters) pthread_cond_broadcast(&host_wait_cv);
    for (w = waiting_threads; w; w = w->wait_next) {
        unsigned i;
        int hit = (o == NULL);
        for (i = 0; !hit && i < w->wait_n; i++) hit = (w->wait_set[i] == o);
        if (hit) pthread_cond_signal(&w->cv);
    }
}

static void deadline_from_ms(struct timespec *ts, uint32_t ms) {
    struct timeval tv;
    gettimeofday(&tv, NULL);
    ts->tv_sec = tv.tv_sec + ms / 1000u;
    ts->tv_nsec = (long)tv.tv_usec * 1000 + (long)(ms % 1000u) * 1000000L;
    if (ts->tv_nsec >= 1000000000L) { ts->tv_sec++; ts->tv_nsec -= 1000000000L; }
}

static int ts_passed(const struct timespec *ts) {
    struct timeval tv;
    gettimeofday(&tv, NULL);
    return tv.tv_sec > ts->tv_sec || (tv.tv_sec == ts->tv_sec && (long)tv.tv_usec * 1000 >= ts->tv_nsec);
}

static void run_apcs(sg_ctx *ctx, core_thread *self) {
    for (;;) {
        core_apc *a;
        uint32_t arg;
        pthread_mutex_lock(&ko_lock);
        a = self->apc_head;
        if (a) {
            self->apc_head = a->next;
            if (!self->apc_head) self->apc_tail = NULL;
        }
        pthread_mutex_unlock(&ko_lock);
        if (!a) return;
        arg = a->data;
        sg_call_guest(ctx, a->fn, SG_CC_STDCALL, 1, &arg); /* PAPCFUNC: VOID NTAPI (ULONG_PTR) */
        free(a);
    }
}

uint32_t sg_core_wait(sg_ctx *ctx, unsigned n, const uint32_t *hs, int wait_all, uint32_t timeout_ms, int alertable) {
    core_thread *self = core_cur(ctx);
    core_kobj *objs[64];
    struct timespec dl;
    uint32_t result = WAIT_TIMED_OUT;
    unsigned i;
    int apc = 0;
    if (n > 64 || !self) {
        core_set_last_error(ctx, WERR_INVALID_PARAMETER);
        return WAIT_FAIL;
    }
    sg_core_full_barrier();
    pthread_mutex_lock(&ko_lock);
    for (i = 0; i < n; i++) {
        objs[i] = hs[i] == SG_PSEUDO_CURRENT_THREAD ? self->obj : handle_get_locked(hs[i]);
        if (!objs[i]) {
            pthread_mutex_unlock(&ko_lock);
            core_set_last_error(ctx, WERR_INVALID_HANDLE);
            return WAIT_FAIL;
        }
    }
    for (i = 0; i < n; i++) objs[i]->refs++; /* keep them alive if closed while we wait */
    if (timeout_ms != INFINITE_MS) deadline_from_ms(&dl, timeout_ms);
    for (;;) {
        if (n && wait_all) {
            for (i = 0; i < n && ko_ready(objs[i], self); i++) {}
            if (i == n) {
                for (i = 0; i < n; i++) ko_acquire(objs[i], self);
                result = WAIT_OBJ_0;
                break;
            }
        } else {
            for (i = 0; i < n; i++)
                if (ko_ready(objs[i], self)) {
                    ko_acquire(objs[i], self);
                    result = WAIT_OBJ_0 + i;
                    break;
                }
            if (i < n) break;
        }
        if (alertable && self->apc_head) { apc = 1; result = WAIT_IO_COMPL; break; }
        if (timeout_ms == 0 || (timeout_ms != INFINITE_MS && ts_passed(&dl))) { result = WAIT_TIMED_OUT; break; }
        /* block */
        self->wait_set = (const core_kobj *const *)objs;
        self->wait_n = n;
        self->waiting = 1 | (alertable ? 2 : 0);
        self->wait_prev = NULL;
        self->wait_next = waiting_threads;
        if (waiting_threads) waiting_threads->wait_prev = self;
        waiting_threads = self;
        if (timeout_ms == INFINITE_MS) pthread_cond_wait(&self->cv, &ko_lock);
        else pthread_cond_timedwait(&self->cv, &ko_lock, &dl);
        if (self->wait_prev) self->wait_prev->wait_next = self->wait_next; else waiting_threads = self->wait_next;
        if (self->wait_next) self->wait_next->wait_prev = self->wait_prev;
        self->wait_next = self->wait_prev = NULL;
        self->waiting = 0;
        self->wait_set = NULL;
        self->wait_n = 0;
    }
    pthread_mutex_unlock(&ko_lock);
    for (i = 0; i < n; i++) kobj_unref(objs[i]);
    if (apc) run_apcs(ctx, self);
    sg_core_full_barrier();
    return result;
}

uint32_t sg_core_sleep(sg_ctx *ctx, uint32_t ms, int alertable) {
    if (alertable) {
        uint32_t r = sg_core_wait(ctx, 0, NULL, 0, ms, 1);
        return r == WAIT_IO_COMPL ? WAIT_IO_COMPL : 0;
    }
    sg_core_full_barrier();
    if (ms == 0) sched_yield();
    else if (ms == INFINITE_MS) for (;;) pause();
    else {
        struct timespec ts = {(time_t)(ms / 1000u), (long)(ms % 1000u) * 1000000L};
        while (nanosleep(&ts, &ts) && errno == EINTR) {}
    }
    sg_core_full_barrier();
    return 0;
}

/* ---- threads ------------------------------------------------------------------------------------ */

static uint32_t tid_next = 0x0f00u;
uint32_t core_new_tid(void) { return __atomic_add_fetch(&tid_next, 4u, __ATOMIC_RELAXED); }

void core_set_host_thread_name(const char *name) {
#ifdef __APPLE__
    pthread_setname_np(name);
#else
    pthread_setname_np(pthread_self(), name);
#endif
}

const char *sg_core_thread_name(const sg_thread *t) {
    const core_thread *h = t ? core_th(t) : NULL;
    return h && h->name[0] ? h->name : "?";
}

void core_thread_init_ctx(sg_thread *t) {
    sg_ctx *c = &t->ctx;
    unsigned i;
    memset(c, 0, sizeof *c);
    c->mem = sg_core_mem;
    c->thread = t;
    c->fs_base = t->teb;
    c->esp = t->stack_hi;
    c->fpu.cw = 0x027f;
    c->fpu.tag = 0xffff;
    c->mxcsr = 0x1f80;
    for (i = 0; i < 8; i++) c->fpu.st[i] = 0;
    c->fl.op = SG_FOP_EFLAGS;
}

#define DEFAULT_STACK 0x100000u
#define STACK_GUARD 0x10000u

/* Allocates sg_thread + host state, guest stack, TEB and the thread kernel object. */
static sg_thread *thread_alloc(uint32_t stack_bytes) {
    sg_thread *t = calloc(1, sizeof *t);
    core_thread *h = calloc(1, sizeof *h);
    uint32_t size, a;
    if (!t || !h) goto fail;
    t->host = h;
    h->self = t;
    pthread_cond_init(&h->cv, NULL);
    size = stack_bytes ? stack_bytes : DEFAULT_STACK;
    if (size < 0x10000u) size = 0x10000u;
    size = (size + 0xffffu) & ~0xffffu;
    a = core_pages_alloc(size + STACK_GUARD);
    if (!a) goto fail;
    sg_window_protect(a, STACK_GUARD, 0); /* guard below the stack: overflow faults */
    h->stack_alloc = a;
    h->stack_alloc_size = size + STACK_GUARD;
    t->stack_lo = a + STACK_GUARD;
    t->stack_hi = a + STACK_GUARD + size;
    t->tid = core_new_tid();
    if (!core_teb_alloc(t)) goto fail_stack;
    core_thread_init_ctx(t);
    h->obj = kobj_new(SG_H_THREAD);
    if (!h->obj) goto fail_teb;
    h->obj->thread = t;
    h->obj->refs = 1; /* held by the running thread */
    snprintf(h->name, sizeof h->name, "thread %u", t->tid);
    return t;
fail_teb:
    core_teb_free(t);
fail_stack:
    core_pages_free(h->stack_alloc, h->stack_alloc_size);
fail:
    if (h) { pthread_cond_destroy(&h->cv); free(h); }
    free(t);
    return NULL;
}

static void thread_release_guest(sg_thread *t) {
    core_thread *h = core_th(t);
    core_threads_unregister(h);
    core_teb_free(t);
    if (h->stack_alloc) core_pages_free(h->stack_alloc, h->stack_alloc_size);
    h->stack_alloc = 0;
}

/* Marks the thread exited, wakes joiners, drops the thread's own reference. After this the
 * sg_thread may be freed by another thread closing the last handle. */
static void thread_finish(sg_thread *t, uint32_t code) {
    core_thread *h = core_th(t);
    core_kobj *o = h->obj;
    unsigned i;
    sg_core_full_barrier();
    pthread_mutex_lock(&ko_lock);
    /* abandon mutexes still owned (Windows: WAIT_ABANDONED; we release them and log) */
    for (i = 0; i < MAX_HANDLES; i++) {
        core_kobj *m = handles[i];
        if (m && m->type == SG_H_MUTEX && m->owner == h) {
            m->owner = NULL;
            m->recursion = 0;
            ko_wake_locked(m);
        }
    }
    o->exited = 1;
    o->exit_code = code;
    ko_wake_locked(o);
    pthread_mutex_unlock(&ko_lock);
    sg_self = NULL;
    thread_release_guest(t);
    kobj_unref(o);
}

static void *thread_main(void *arg) {
    sg_thread *t = arg;
    core_thread *h = core_th(t);
    sg_ctx *ctx = &t->ctx;
    uint8_t *mem = ctx->mem;
    uint32_t code;
    stack_t ss;
    /* alternate signal stack so the fault reporter runs even after a host stack overflow
     * (only when the runtime's fault handlers are installed; sanitizers keep their own) */
    ss.ss_sp = core_fault_handlers_installed ? malloc(65536) : NULL;
    ss.ss_size = 65536;
    ss.ss_flags = 0;
    if (ss.ss_sp) sigaltstack(&ss, NULL);
    sg_self = t;
    core_set_host_thread_name(h->name);
    pthread_mutex_lock(&ko_lock);
    while (h->suspend > 0) pthread_cond_wait(&h->cv, &ko_lock);
    pthread_mutex_unlock(&ko_lock);
    sg_core_full_barrier();
    ctx->esp = t->stack_hi;
    SG_PUSH32(h->param);
    SG_PUSH32(SG_THREAD_EXIT);
    if (!setjmp(h->exit_jb)) {
        h->exit_armed = 1;
        sg_lookup(ctx, mem, h->entry)(ctx, mem);
        if (ctx->eip != SG_THREAD_EXIT)
            sg_core_fatal(ctx, "thread entry 0x%08x returned to 0x%08x, not SG_THREAD_EXIT", h->entry, ctx->eip);
        if (ctx->esp != t->stack_hi)
            sg_core_log(1, "thread entry 0x%08x left esp 0x%08x (stdcall entry should pop its argument)", h->entry,
                        ctx->esp);
        code = ctx->eax;
    } else {
        code = h->exit_code;
    }
    h->exit_armed = 0;
    thread_finish(t, code);
    if (ss.ss_sp) {
        ss.ss_flags = SS_DISABLE;
        sigaltstack(&ss, NULL);
        free(ss.ss_sp);
    }
    return NULL;
}

sg_thread *sg_thread_create(uint32_t entry, uint32_t param, uint32_t stack_bytes, int suspended) {
    sg_thread *t;
    core_thread *h;
    pthread_attr_t at;
    int rc;
    if (!sg_core_mem) return NULL;
    t = thread_alloc(stack_bytes);
    if (!t) return NULL;
    h = core_th(t);
    h->entry = entry;
    h->param = param;
    h->suspend = suspended ? 1 : 0;
    h->host_started = 1;
    core_threads_register(h);
    sg_core_full_barrier(); /* thread creation is a full barrier */
    pthread_attr_init(&at);
    pthread_attr_setstacksize(&at, 8u << 20);
    pthread_attr_setdetachstate(&at, PTHREAD_CREATE_DETACHED);
    rc = pthread_create(&h->pt, &at, thread_main, t);
    pthread_attr_destroy(&at);
    if (rc) {
        thread_release_guest(t);
        free(h->obj);
        pthread_cond_destroy(&h->cv);
        free(h);
        free(t);
        return NULL;
    }
    return t;
}

sg_thread *sg_core_attach_main(uint32_t stack_bytes) {
    sg_thread *t = thread_alloc(stack_bytes);
    if (!t) return NULL;
    core_threads_register(core_th(t));
    snprintf(core_th(t)->name, sizeof core_th(t)->name, "%s", sg_self ? "attached" : "main");
    sg_self = t;
    return t;
}

void sg_core_detach_thread(sg_thread *t) {
    if (sg_self == t) sg_self = NULL;
    thread_finish(t, 0);
}

uint32_t sg_core_thread_handle(sg_thread *t) {
    uint32_t h;
    pthread_mutex_lock(&ko_lock);
    h = handle_put_locked(core_th(t)->obj);
    pthread_mutex_unlock(&ko_lock);
    return h;
}

int sg_core_resume_thread(sg_thread *t) {
    core_thread *h = core_th(t);
    int prev;
    sg_core_full_barrier();
    pthread_mutex_lock(&ko_lock);
    prev = h->suspend;
    if (h->suspend > 0 && --h->suspend == 0) pthread_cond_signal(&h->cv);
    pthread_mutex_unlock(&ko_lock);
    return prev;
}

uint32_t sg_core_join(sg_thread *t) {
    core_kobj *o = core_th(t)->obj;
    uint32_t code;
    pthread_mutex_lock(&ko_lock);
    o->refs++;
    /* a host-side join: poll with a short sleep (tests and shutdown only) */
    while (!o->exited) {
        pthread_mutex_unlock(&ko_lock);
        usleep(200);
        pthread_mutex_lock(&ko_lock);
    }
    code = o->exit_code;
    pthread_mutex_unlock(&ko_lock);
    kobj_unref(o);
    return code;
}

void sg_core_exit_thread(sg_ctx *ctx, uint32_t code) {
    core_thread *h = core_cur(ctx);
    if (!h || !h->exit_armed) sg_core_fatal(ctx, "ExitThread(%u) on a thread not started by sg_thread_create", code);
    core_run_cleanups_below(ctx, NULL);
    ctx->eh_top = NULL;
    h->exit_code = code;
    longjmp(h->exit_jb, 1);
}

/* Resolves a thread handle (or the pseudo handle) to its sg_thread. */
static sg_thread *thread_from_handle(sg_ctx *ctx, uint32_t hv) {
    core_kobj *o;
    sg_thread *t = NULL;
    if (hv == SG_PSEUDO_CURRENT_THREAD) return ctx->thread;
    pthread_mutex_lock(&ko_lock);
    o = handle_get_locked(hv);
    if (o && o->type == SG_H_THREAD) t = o->thread;
    pthread_mutex_unlock(&ko_lock);
    return t;
}

/* ---- critical sections (guest CRITICAL_SECTION, 24 bytes) ---------------------------------------
 *   +0 DebugInfo (0xFFFFFFFF)   +4 LockCount (-1 free, -2 held)   +8 RecursionCount
 *   +12 OwningThread (tid)       +16 LockSemaphore (our waiter count)  +20 SpinCount
 * Ownership is a CAS on OwningThread; contended waiters park on one of 256 host mutex/condvar
 * buckets chosen by address. The waiter count is incremented before the waiter's CAS and read
 * by the releaser after its store, both sequentially consistent, so no wake-up is lost. */
#define CS_DEBUG 0u
#define CS_LOCKCOUNT 4u
#define CS_RECURSION 8u
#define CS_OWNER 12u
#define CS_WAITERS 16u
#define CS_SPIN 20u
#define NBUCKETS 256u
static struct { pthread_mutex_t m; pthread_cond_t c; } cs_bucket[NBUCKETS];
static pthread_once_t cs_once = PTHREAD_ONCE_INIT;
static void cs_init_buckets(void) {
    unsigned i;
    for (i = 0; i < NBUCKETS; i++) {
        pthread_mutex_init(&cs_bucket[i].m, NULL);
        pthread_cond_init(&cs_bucket[i].c, NULL);
    }
}
static unsigned cs_hash(uint32_t a) { return ((a >> 3) ^ (a >> 11)) & (NBUCKETS - 1u); }
static uint32_t *cs_word(uint8_t *mem, uint32_t cs, uint32_t off) { return (uint32_t *)(void *)(mem + cs + off); }

static int cs_try(sg_ctx *ctx, uint8_t *mem, uint32_t cs, uint32_t tid) {
    uint32_t expect = 0;
    if (__atomic_load_n(cs_word(mem, cs, CS_OWNER), __ATOMIC_RELAXED) == tid) {
        sg_st32(mem, cs + CS_RECURSION, sg_ld32(mem, cs + CS_RECURSION) + 1u);
        return 1;
    }
    if (__atomic_compare_exchange_n(cs_word(mem, cs, CS_OWNER), &expect, tid, 0, __ATOMIC_SEQ_CST, __ATOMIC_SEQ_CST)) {
        sg_st32(mem, cs + CS_RECURSION, 1);
        sg_st32(mem, cs + CS_LOCKCOUNT, 0xfffffffeu);
        return 1;
    }
    (void)ctx;
    return 0;
}

static void cs_check(sg_ctx *ctx, uint32_t cs, const char *fn) {
    if (!cs || (cs & 3u)) sg_core_fatal(ctx, "%s: bad CRITICAL_SECTION pointer 0x%08x", fn, cs);
}

void sgc_cs_enter(sg_ctx *ctx, uint8_t *mem, uint32_t cs) {
    uint32_t tid = ctx->thread ? ctx->thread->tid : 1;
    unsigned b;
    int spins;
    cs_check(ctx, cs, "EnterCriticalSection");
    if (cs_try(ctx, mem, cs, tid)) return;
    for (spins = 0; spins < 64; spins++) {
#if defined(__aarch64__)
        __asm__ __volatile__("yield");
#endif
        if (cs_try(ctx, mem, cs, tid)) return;
    }
    pthread_once(&cs_once, cs_init_buckets);
    b = cs_hash(cs);
    pthread_mutex_lock(&cs_bucket[b].m);
    __atomic_add_fetch(cs_word(mem, cs, CS_WAITERS), 1u, __ATOMIC_SEQ_CST);
    while (!cs_try(ctx, mem, cs, tid)) pthread_cond_wait(&cs_bucket[b].c, &cs_bucket[b].m);
    __atomic_sub_fetch(cs_word(mem, cs, CS_WAITERS), 1u, __ATOMIC_SEQ_CST);
    pthread_mutex_unlock(&cs_bucket[b].m);
}

void sgc_cs_leave(sg_ctx *ctx, uint8_t *mem, uint32_t cs) {
    uint32_t tid = ctx->thread ? ctx->thread->tid : 1, r, owner;
    cs_check(ctx, cs, "LeaveCriticalSection");
    owner = __atomic_load_n(cs_word(mem, cs, CS_OWNER), __ATOMIC_RELAXED);
    if (owner != tid) {
        sg_core_log(1, "LeaveCriticalSection(0x%08x) by tid %u, owner %u: ignored", cs, tid, owner);
        return;
    }
    r = sg_ld32(mem, cs + CS_RECURSION) - 1u;
    sg_st32(mem, cs + CS_RECURSION, r);
    if (r) return;
    sg_st32(mem, cs + CS_LOCKCOUNT, 0xffffffffu);
    __atomic_store_n(cs_word(mem, cs, CS_OWNER), 0u, __ATOMIC_SEQ_CST);
    if (__atomic_load_n(cs_word(mem, cs, CS_WAITERS), __ATOMIC_SEQ_CST)) {
        unsigned b = cs_hash(cs);
        pthread_once(&cs_once, cs_init_buckets);
        pthread_mutex_lock(&cs_bucket[b].m);
        pthread_cond_broadcast(&cs_bucket[b].c);
        pthread_mutex_unlock(&cs_bucket[b].m);
    }
}

static void cs_init(uint8_t *mem, uint32_t cs, uint32_t spin) {
    sg_st32(mem, cs + CS_DEBUG, 0xffffffffu);
    sg_st32(mem, cs + CS_LOCKCOUNT, 0xffffffffu);
    sg_st32(mem, cs + CS_RECURSION, 0);
    sg_st32(mem, cs + CS_OWNER, 0);
    sg_st32(mem, cs + CS_WAITERS, 0);
    sg_st32(mem, cs + CS_SPIN, spin & 0x00ffffffu);
}

SGC_SHIM(kernel32, InitializeCriticalSection) {
    uint32_t cs = SG_ARG(0);
    cs_check(ctx, cs, "InitializeCriticalSection");
    cs_init(mem, cs, 0);
    sg_core_full_barrier();
    SG_RET(4);
}
SGC_SHIM(kernel32, InitializeCriticalSectionAndSpinCount) {
    uint32_t cs = SG_ARG(0);
    cs_check(ctx, cs, "InitializeCriticalSectionAndSpinCount");
    cs_init(mem, cs, SG_ARG(1));
    sg_core_full_barrier();
    SG_RETURN_STDCALL(1, 8);
}
SGC_SHIM(kernel32, DeleteCriticalSection) {
    uint32_t cs = SG_ARG(0);
    if (cs && __atomic_load_n(cs_word(mem, cs, CS_OWNER), __ATOMIC_RELAXED) &&
        __atomic_load_n(cs_word(mem, cs, CS_WAITERS), __ATOMIC_RELAXED))
        sg_core_log(1, "DeleteCriticalSection(0x%08x) with waiters", cs);
    sg_core_full_barrier();
    SG_RET(4);
}
SGC_SHIM(kernel32, EnterCriticalSection) {
    sgc_cs_enter(ctx, mem, SG_ARG(0));
    SG_RET(4);
}
SGC_SHIM(kernel32, LeaveCriticalSection) {
    sgc_cs_leave(ctx, mem, SG_ARG(0));
    SG_RET(4);
}
SGC_SHIM(kernel32, TryEnterCriticalSection) {
    uint32_t cs = SG_ARG(0);
    cs_check(ctx, cs, "TryEnterCriticalSection");
    SG_RETURN_STDCALL(cs_try(ctx, mem, cs, ctx->thread ? ctx->thread->tid : 1), 4);
}

/* ---- thread shims ------------------------------------------------------------------------------- */

static uint32_t create_thread_common(sg_ctx *ctx, uint32_t stack, uint32_t start, uint32_t param, uint32_t flags,
                                     uint32_t tid_out) {
    sg_thread *t;
    uint32_t h;
    if (!start) {
        core_set_last_error(ctx, WERR_INVALID_PARAMETER);
        return 0;
    }
    t = sg_thread_create(start, param, stack, 1);
    if (!t) {
        core_set_last_error(ctx, WERR_NOT_ENOUGH_MEMORY);
        return 0;
    }
    h = sg_core_thread_handle(t);
    if (tid_out) sg_st32(ctx->mem, tid_out, t->tid);
    sg_core_log(2, "thread %u created: entry 0x%08x param 0x%08x stack %u%s", t->tid, start, param, stack,
                (flags & 4u) ? " (suspended)" : "");
    if (!(flags & 4u)) sg_core_resume_thread(t); /* CREATE_SUSPENDED = 4 */
    return h;
}

SGC_SHIM(kernel32, CreateThread) {
    /* (lpThreadAttributes, dwStackSize, lpStartAddress, lpParameter, dwCreationFlags, lpThreadId) */
    uint32_t h = create_thread_common(ctx, SG_ARG(1), SG_ARG(2), SG_ARG(3), SG_ARG(4), SG_ARG(5));
    SG_RETURN_STDCALL(h, 24);
}

SGC_SHIM(msvcr90, _beginthreadex) {
    /* cdecl (security, stack_size, start_address, arglist, initflag, thrdaddr); start is
     * unsigned (__stdcall *)(void *). Returns the handle, 0 on failure (errno EAGAIN/EINVAL). */
    uint32_t h = create_thread_common(ctx, SG_ARG(1), SG_ARG(2), SG_ARG(3), SG_ARG(4), SG_ARG(5));
    SG_RETURN_CDECL(h);
}

SGC_SHIM(msvcr90, _endthreadex) { sg_core_exit_thread(ctx, SG_ARG(0)); }
SGC_SHIM(kernel32, ExitThread) { sg_core_exit_thread(ctx, SG_ARG(0)); }

SGC_SHIM(kernel32, ResumeThread) {
    sg_thread *t = thread_from_handle(ctx, SG_ARG(0));
    if (!t) {
        core_set_last_error(ctx, WERR_INVALID_HANDLE);
        SG_RETURN_STDCALL(0xffffffffu, 4);
    }
    SG_RETURN_STDCALL((uint32_t)sg_core_resume_thread(t), 4);
}

SGC_SHIM(kernel32, GetExitCodeThread) {
    uint32_t hv = SG_ARG(0), out = SG_ARG(1), code;
    core_kobj *o;
    if (hv == SG_PSEUDO_CURRENT_THREAD) {
        if (out) sg_st32(mem, out, STILL_ACTIVE_CODE);
        SG_RETURN_STDCALL(1, 8);
    }
    pthread_mutex_lock(&ko_lock);
    o = handle_get_locked(hv);
    code = o && o->type == SG_H_THREAD ? (o->exited ? o->exit_code : STILL_ACTIVE_CODE) : 0;
    pthread_mutex_unlock(&ko_lock);
    if (!o || o->type != SG_H_THREAD) {
        core_set_last_error(ctx, WERR_INVALID_HANDLE);
        SG_RETURN_STDCALL(0, 8);
    }
    if (out) sg_st32(mem, out, code);
    SG_RETURN_STDCALL(1, 8);
}

SGC_SHIM(kernel32, GetCurrentThread) { SG_RETURN_STDCALL(SG_PSEUDO_CURRENT_THREAD, 0); }
SGC_SHIM(kernel32, GetCurrentThreadId) { SG_RETURN_STDCALL(ctx->thread ? ctx->thread->tid : 0, 0); }
SGC_SHIM(kernel32, GetCurrentProcess) { SG_RETURN_STDCALL(SG_PSEUDO_CURRENT_PROCESS, 0); }
SGC_SHIM(kernel32, GetCurrentProcessId) { SG_RETURN_STDCALL(0x1000u, 0); }

/* Priorities: stored and reported back; macOS schedules by QoS, so nothing else happens. */
SGC_SHIM(kernel32, SetThreadPriority) {
    sg_thread *t = thread_from_handle(ctx, SG_ARG(0));
    if (!t) { core_set_last_error(ctx, WERR_INVALID_HANDLE); SG_RETURN_STDCALL(0, 8); }
    core_th(t)->priority = (int)SG_ARG(1);
    SG_RETURN_STDCALL(1, 8);
}
SGC_SHIM(kernel32, GetThreadPriority) {
    sg_thread *t = thread_from_handle(ctx, SG_ARG(0));
    if (!t) { core_set_last_error(ctx, WERR_INVALID_HANDLE); SG_RETURN_STDCALL(0x7fffffffu, 4); }
    SG_RETURN_STDCALL((uint32_t)core_th(t)->priority, 4);
}
static uint32_t cpu_mask(void) {
    long n = sysconf(_SC_NPROCESSORS_ONLN);
    if (n < 1) n = 1;
    if (n > 32) n = 32;
    return n == 32 ? 0xffffffffu : ((1u << n) - 1u);
}
SGC_SHIM(kernel32, SetThreadAffinityMask) { SG_RETURN_STDCALL(SG_ARG(1) ? cpu_mask() : 0, 8); }
SGC_SHIM(kernel32, SetThreadIdealProcessor) { SG_RETURN_STDCALL(0, 8); }
SGC_SHIM(kernel32, SetPriorityClass) { SG_RETURN_STDCALL(1, 8); }
SGC_SHIM(kernel32, GetPriorityClass) { SG_RETURN_STDCALL(0x20u /* NORMAL_PRIORITY_CLASS */, 4); }
SGC_SHIM(kernel32, SetProcessAffinityMask) { SG_RETURN_STDCALL(1, 8); }
SGC_SHIM(kernel32, GetProcessAffinityMask) {
    uint32_t p = SG_ARG(1), s = SG_ARG(2), m = cpu_mask();
    if (p) sg_st32(mem, p, m);
    if (s) sg_st32(mem, s, m);
    SG_RETURN_STDCALL(1, 12);
}

/* ---- sync object shims -------------------------------------------------------------------------- */

static int guest_name(uint8_t *mem, uint32_t a, char *buf, size_t cap) {
    size_t i;
    if (!a) return 0;
    for (i = 0; i + 1 < cap; i++) {
        buf[i] = (char)mem[a + i];
        if (!buf[i]) break;
    }
    buf[cap - 1] = 0;
    return 1;
}

static uint32_t create_object(sg_ctx *ctx, int type, uint32_t name_va, core_kobj *proto) {
    char name[260];
    int has = guest_name(ctx->mem, name_va, name, sizeof name), existed = 0;
    core_kobj *o = NULL;
    uint32_t h = 0;
    sg_core_full_barrier();
    pthread_mutex_lock(&ko_lock);
    if (has) o = named_lookup_locked(name, type, &existed);
    if (existed && !o) {
        pthread_mutex_unlock(&ko_lock);
        core_set_last_error(ctx, WERR_INVALID_HANDLE);
        return 0;
    }
    if (!o) {
        o = kobj_new(type);
        if (!o) { pthread_mutex_unlock(&ko_lock); core_set_last_error(ctx, WERR_NOT_ENOUGH_MEMORY); return 0; }
        o->manual = proto->manual;
        o->signaled = proto->signaled;
        o->count = proto->count;
        o->max = proto->max;
        o->owner = proto->owner;
        o->recursion = proto->recursion;
        if (has) named_add_locked(o, name);
    }
    h = handle_put_locked(o);
    pthread_mutex_unlock(&ko_lock);
    core_set_last_error(ctx, existed ? WERR_ALREADY_EXISTS : WERR_SUCCESS);
    return h;
}

SGC_SHIM(kernel32, CreateEventA) {
    /* (lpEventAttributes, bManualReset, bInitialState, lpName) */
    core_kobj p;
    memset(&p, 0, sizeof p);
    p.manual = SG_ARG(1) != 0;
    p.signaled = SG_ARG(2) != 0;
    SG_RETURN_STDCALL(create_object(ctx, SG_H_EVENT, SG_ARG(3), &p), 16);
}

static uint32_t event_op(sg_ctx *ctx, uint32_t hv, int op /* 1 set, 0 reset, 2 pulse */) {
    core_kobj *o;
    sg_core_full_barrier();
    pthread_mutex_lock(&ko_lock);
    o = handle_get_locked(hv);
    if (!o || o->type != SG_H_EVENT) {
        pthread_mutex_unlock(&ko_lock);
        core_set_last_error(ctx, WERR_INVALID_HANDLE);
        return 0;
    }
    if (op) { o->signaled = 1; ko_wake_locked(o); }
    else o->signaled = 0;
    pthread_mutex_unlock(&ko_lock);
    sg_core_full_barrier();
    return 1;
}
/* ---- the event API for other runtime packages (sg_core.h section 5) -------------------------- */

uint32_t sg_core_event_create(int manual_reset, int initial_state) {
    core_kobj *o = kobj_new(SG_H_EVENT);
    uint32_t h;
    if (!o) return 0;
    o->manual = manual_reset != 0;
    o->signaled = initial_state != 0;
    sg_core_full_barrier();
    pthread_mutex_lock(&ko_lock);
    h = handle_put_locked(o);
    pthread_mutex_unlock(&ko_lock);
    if (!h) free(o);
    return h;
}

static int event_op_host(uint32_t hv, int set) {
    core_kobj *o;
    sg_core_full_barrier();
    pthread_mutex_lock(&ko_lock);
    o = handle_get_locked(hv);
    if (!o || o->type != SG_H_EVENT) { pthread_mutex_unlock(&ko_lock); return 0; }
    if (set) { o->signaled = 1; ko_wake_locked(o); }
    else o->signaled = 0;
    pthread_mutex_unlock(&ko_lock);
    sg_core_full_barrier();
    return 1;
}
int sg_core_event_set(uint32_t h) { return event_op_host(h, 1); }
int sg_core_event_reset(uint32_t h) { return event_op_host(h, 0); }

uint32_t sg_core_event_wait(uint32_t hv, uint32_t timeout_ms) {
    core_kobj *o;
    struct timespec dl;
    uint32_t r;
    if (sg_self) return sg_core_wait(&sg_self->ctx, 1, &hv, 0, timeout_ms, 0);
    sg_core_full_barrier();
    pthread_mutex_lock(&ko_lock);
    o = handle_get_locked(hv);
    if (!o || o->type != SG_H_EVENT) { pthread_mutex_unlock(&ko_lock); return WAIT_FAIL; }
    o->refs++;
    if (timeout_ms != INFINITE_MS) deadline_from_ms(&dl, timeout_ms);
    for (;;) {
        if (o->signaled) { if (!o->manual) o->signaled = 0; r = WAIT_OBJ_0; break; }
        if (timeout_ms == 0 || (timeout_ms != INFINITE_MS && ts_passed(&dl))) { r = WAIT_TIMED_OUT; break; }
        host_waiters++;
        if (timeout_ms == INFINITE_MS) pthread_cond_wait(&host_wait_cv, &ko_lock);
        else pthread_cond_timedwait(&host_wait_cv, &ko_lock, &dl);
        host_waiters--;
    }
    pthread_mutex_unlock(&ko_lock);
    kobj_unref(o);
    sg_core_full_barrier();
    return r;
}

SGC_SHIM(kernel32, SetEvent) { SG_RETURN_STDCALL(event_op(ctx, SG_ARG(0), 1), 4); }
SGC_SHIM(kernel32, ResetEvent) { SG_RETURN_STDCALL(event_op(ctx, SG_ARG(0), 0), 4); }

SGC_SHIM(kernel32, CreateSemaphoreA) {
    /* (lpSemaphoreAttributes, lInitialCount, lMaximumCount, lpName) */
    core_kobj p;
    int32_t init = (int32_t)SG_ARG(1), max = (int32_t)SG_ARG(2);
    if (max <= 0 || init < 0 || init > max) {
        core_set_last_error(ctx, WERR_INVALID_PARAMETER);
        SG_RETURN_STDCALL(0, 16);
    }
    memset(&p, 0, sizeof p);
    p.count = init;
    p.max = max;
    SG_RETURN_STDCALL(create_object(ctx, SG_H_SEMAPHORE, SG_ARG(3), &p), 16);
}

SGC_SHIM(kernel32, ReleaseSemaphore) {
    uint32_t hv = SG_ARG(0), prev_out = SG_ARG(2);
    int32_t n = (int32_t)SG_ARG(1), prev;
    core_kobj *o;
    sg_core_full_barrier();
    pthread_mutex_lock(&ko_lock);
    o = handle_get_locked(hv);
    if (!o || o->type != SG_H_SEMAPHORE) {
        pthread_mutex_unlock(&ko_lock);
        core_set_last_error(ctx, WERR_INVALID_HANDLE);
        SG_RETURN_STDCALL(0, 12);
    }
    if (n <= 0 || o->count > o->max - n) {
        pthread_mutex_unlock(&ko_lock);
        core_set_last_error(ctx, n <= 0 ? WERR_INVALID_PARAMETER : WERR_TOO_MANY_POSTS);
        SG_RETURN_STDCALL(0, 12);
    }
    prev = o->count;
    o->count += n;
    ko_wake_locked(o);
    pthread_mutex_unlock(&ko_lock);
    if (prev_out) sg_st32(mem, prev_out, (uint32_t)prev);
    sg_core_full_barrier();
    SG_RETURN_STDCALL(1, 12);
}

SGC_SHIM(kernel32, CreateMutexA) {
    /* (lpMutexAttributes, bInitialOwner, lpName) */
    core_kobj p;
    memset(&p, 0, sizeof p);
    if (SG_ARG(1)) { p.owner = core_cur(ctx); p.recursion = 1; }
    {
        uint32_t h = create_object(ctx, SG_H_MUTEX, SG_ARG(2), &p);
        SG_RETURN_STDCALL(h, 12);
    }
}

SGC_SHIM(kernel32, ReleaseMutex) {
    core_kobj *o;
    core_thread *self = core_cur(ctx);
    sg_core_full_barrier();
    pthread_mutex_lock(&ko_lock);
    o = handle_get_locked(SG_ARG(0));
    if (!o || o->type != SG_H_MUTEX) {
        pthread_mutex_unlock(&ko_lock);
        core_set_last_error(ctx, WERR_INVALID_HANDLE);
        SG_RETURN_STDCALL(0, 4);
    }
    if (o->owner != self) {
        pthread_mutex_unlock(&ko_lock);
        core_set_last_error(ctx, WERR_NOT_OWNER);
        SG_RETURN_STDCALL(0, 4);
    }
    if (--o->recursion == 0) { o->owner = NULL; ko_wake_locked(o); }
    pthread_mutex_unlock(&ko_lock);
    sg_core_full_barrier();
    SG_RETURN_STDCALL(1, 4);
}

int (*sg_core_close_hook)(sg_ctx *ctx, uint32_t h);

int sg_core_close_foreign(sg_ctx *ctx, uint32_t h) {
    if (sg_handle_close(h)) return 1;
    core_set_last_error(ctx, WERR_INVALID_HANDLE);
    return 0;
}

SGC_SHIM(kernel32, CloseHandle) {
    uint32_t hv = SG_ARG(0);
    if (sg_handle_close(hv)) SG_RETURN_STDCALL(1, 4);
    if (sg_core_close_hook) SG_RETURN_STDCALL((uint32_t)sg_core_close_hook(ctx, hv), 4); /* e.g. runtime/win32 files */
    core_set_last_error(ctx, WERR_INVALID_HANDLE);
    SG_RETURN_STDCALL(0, 4);
}

SGC_SHIM(kernel32, DuplicateHandle) {
    /* (hSourceProcess, hSource, hTargetProcess, lpTargetHandle, dwDesiredAccess, bInheritHandle, dwOptions) */
    uint32_t src = SG_ARG(1), out = SG_ARG(3), opt = SG_ARG(6), h = 0;
    core_kobj *o;
    pthread_mutex_lock(&ko_lock);
    if (src == SG_PSEUDO_CURRENT_THREAD) o = core_cur(ctx) ? core_cur(ctx)->obj : NULL;
    else o = handle_get_locked(src);
    if (o) h = handle_put_locked(o);
    pthread_mutex_unlock(&ko_lock);
    if (!o || (!h && src != SG_PSEUDO_CURRENT_PROCESS)) {
        if (src == SG_PSEUDO_CURRENT_PROCESS) h = SG_PSEUDO_CURRENT_PROCESS;
        else {
            core_set_last_error(ctx, WERR_INVALID_HANDLE);
            SG_RETURN_STDCALL(0, 28);
        }
    }
    if (out) sg_st32(mem, out, h);
    if (opt & 1u) sg_handle_close(src); /* DUPLICATE_CLOSE_SOURCE */
    SG_RETURN_STDCALL(1, 28);
}

SGC_SHIM(kernel32, WaitForSingleObject) {
    uint32_t hv = SG_ARG(0);
    SG_RETURN_STDCALL(sg_core_wait(ctx, 1, &hv, 0, SG_ARG(1), 0), 8);
}
SGC_SHIM(kernel32, WaitForSingleObjectEx) {
    uint32_t hv = SG_ARG(0);
    SG_RETURN_STDCALL(sg_core_wait(ctx, 1, &hv, 0, SG_ARG(1), SG_ARG(2) != 0), 12);
}
static uint32_t wait_multiple(sg_ctx *ctx, uint8_t *mem, uint32_t n, uint32_t arr, int all, uint32_t ms, int alertable) {
    uint32_t hs[64], i;
    if (n == 0 || n > 64 || !arr) {
        core_set_last_error(ctx, WERR_INVALID_PARAMETER);
        return WAIT_FAIL;
    }
    for (i = 0; i < n; i++) hs[i] = sg_ld32(mem, arr + 4u * i);
    return sg_core_wait(ctx, n, hs, all, ms, alertable);
}
SGC_SHIM(kernel32, WaitForMultipleObjects) {
    SG_RETURN_STDCALL(wait_multiple(ctx, mem, SG_ARG(0), SG_ARG(1), SG_ARG(2) != 0, SG_ARG(3), 0), 16);
}
SGC_SHIM(kernel32, WaitForMultipleObjectsEx) {
    SG_RETURN_STDCALL(wait_multiple(ctx, mem, SG_ARG(0), SG_ARG(1), SG_ARG(2) != 0, SG_ARG(3), SG_ARG(4) != 0), 20);
}

SGC_SHIM(kernel32, Sleep) {
    sg_core_sleep(ctx, SG_ARG(0), 0);
    SG_RET(4);
}
SGC_SHIM(kernel32, SleepEx) { SG_RETURN_STDCALL(sg_core_sleep(ctx, SG_ARG(0), SG_ARG(1) != 0), 8); }
SGC_SHIM(kernel32, SwitchToThread) {
    sg_core_full_barrier();
    sched_yield();
    SG_RETURN_STDCALL(1, 0);
}

SGC_SHIM(kernel32, QueueUserAPC) {
    /* (pfnAPC, hThread, dwData) */
    sg_thread *t = thread_from_handle(ctx, SG_ARG(1));
    core_apc *a;
    core_thread *h;
    if (!t || !SG_ARG(0)) {
        core_set_last_error(ctx, WERR_INVALID_HANDLE);
        SG_RETURN_STDCALL(0, 12);
    }
    a = calloc(1, sizeof *a);
    if (!a) SG_RETURN_STDCALL(0, 12);
    a->fn = SG_ARG(0);
    a->data = SG_ARG(2);
    h = core_th(t);
    sg_core_full_barrier();
    pthread_mutex_lock(&ko_lock);
    if (h->obj->exited) {
        pthread_mutex_unlock(&ko_lock);
        free(a);
        core_set_last_error(ctx, WERR_INVALID_HANDLE);
        SG_RETURN_STDCALL(0, 12);
    }
    if (h->apc_tail) h->apc_tail->next = a; else h->apc_head = a;
    h->apc_tail = a;
    if (h->waiting & 2) pthread_cond_signal(&h->cv);
    pthread_mutex_unlock(&ko_lock);
    SG_RETURN_STDCALL(1, 12);
}
