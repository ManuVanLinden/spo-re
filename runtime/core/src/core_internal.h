/* core_internal.h -- private declarations shared by the files in runtime/core/src */
#ifndef SG_CORE_INTERNAL_H
#define SG_CORE_INTERNAL_H

#include <pthread.h>
#include <setjmp.h>
#include <stdio.h>
#include <stdlib.h>
#include <string.h>

#include "sg_core.h"

/* Windows error codes the shims set. */
enum {
    WERR_SUCCESS = 0, WERR_INVALID_HANDLE = 6, WERR_NOT_ENOUGH_MEMORY = 8, WERR_INVALID_PARAMETER = 87,
    WERR_ALREADY_EXISTS = 183, WERR_NOT_OWNER = 288, WERR_TOO_MANY_POSTS = 298
};
enum { WAIT_OBJ_0 = 0, WAIT_ABANDONED_0 = 0x80, WAIT_IO_COMPL = 0xc0, WAIT_TIMED_OUT = 0x102,
       WAIT_FAIL = 0xffffffffu, STILL_ACTIVE_CODE = 259, INFINITE_MS = 0xffffffffu };

static inline void core_set_last_error(sg_ctx *ctx, uint32_t v) {
    if (ctx->fs_base) sg_st32(ctx->mem, ctx->fs_base + SG_TEB_LAST_ERROR, v);
}

/* ---- per-thread host state (sg_thread.host) ---------------------------------------------- */

#define CORE_MAX_INFLIGHT 32
typedef struct core_inflight {   /* a caught C++ exception whose catch block is running */
    uint32_t obj, ti;
    uint32_t guard;               /* guest address of its catch-guard registration */
    int done;
} core_inflight;

struct core_kobj;
typedef struct core_apc { uint32_t fn, data; struct core_apc *next; } core_apc;

typedef struct core_thread {
    pthread_t pt;
    int host_started;             /* a pthread runs this guest thread (0 for attached threads) */
    pthread_cond_t cv;            /* waits (under the kernel-object lock) */
    struct core_kobj *obj;        /* the thread's kernel object (signalled at exit) */
    int suspend;                  /* suspend count; the pthread waits while > 0 */
    uint32_t entry, param;
    uint32_t stack_alloc, stack_alloc_size; /* guest allocation holding guard + stack */
    uint32_t tls_expansion;       /* guest page of the 1,024 expansion slots, 0 until first use */
    int priority;
    char name[64];
    /* waits */
    const struct core_kobj *const *wait_set;
    unsigned wait_n;
    struct core_thread *wait_next, *wait_prev; /* list of waiting threads */
    int waiting;
    core_apc *apc_head, *apc_tail;
    /* ExitThread */
    jmp_buf exit_jb;
    int exit_armed;
    uint32_t exit_code;
    /* exception handling (runtime-core.md 7) */
    core_inflight inflight[CORE_MAX_INFLIGHT];
    unsigned ninflight;
    uint32_t dispatch_obj;        /* object of the C++ dispatch in progress (rethrow detection) */
    sg_cleanup *cleanup;          /* host cleanup records, innermost first */
    struct core_thread *all_next, *all_prev; /* registry of live threads (TLS clearing) */
    sg_thread *self;
} core_thread;

static inline core_thread *core_th(const sg_thread *t) { return (core_thread *)t->host; }
static inline core_thread *core_cur(sg_ctx *ctx) { return ctx->thread ? core_th(ctx->thread) : NULL; }

/* core_teb.c */
uint32_t core_teb_alloc(sg_thread *t);
void core_teb_free(sg_thread *t);
void core_tls_thread_init(sg_thread *t);
void core_threads_register(core_thread *h);
void core_threads_unregister(core_thread *h);
extern pthread_mutex_t core_threads_lock;
extern core_thread *core_threads_all;

/* core_mem.c */
uint32_t core_pages_alloc(uint32_t size);           /* via sg_core_vm_alloc */
void core_pages_free(uint32_t a, uint32_t size);

/* core_thread.c */
void core_kobj_init(void);
void core_thread_init_ctx(sg_thread *t);
uint32_t core_new_tid(void);
void core_set_host_thread_name(const char *name);

/* core_eh.c */
void core_eh_register_thunks(void);
void core_run_cleanups_below(sg_ctx *ctx, const void *host_limit);
SG_NORETURN void core_terminate(sg_ctx *ctx, const char *why);

/* core_call.c */
extern int core_fault_handlers_installed;
void core_thunks_init(void);
extern uint32_t core_thunk_catch_guard;

#endif
