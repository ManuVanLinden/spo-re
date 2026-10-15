/* thunks.c -- the thunk region (runtime-abi 7.1). sg_thunk_fn and sg_thunk_register are WEAK here:
 * if runtime/core defines its own, the linker takes those and these become unused. */
#include <pthread.h>
#include <stdio.h>
#include <stdlib.h>
#include <string.h>
#include <sys/mman.h>

#include "sg_image.h"

#define NSLOTS ((SG_THUNK_LIMIT - SG_THUNK_BASE) / SG_THUNK_SLOT)

__attribute__((weak)) sg_func *sg_thunk_fn[NSLOTS];
const char *sg_image_thunk_names[NSLOTS];
unsigned sg_thunk_count = SG_THUNK_FIRST_FREE;
static pthread_mutex_t thunk_lock = PTHREAD_MUTEX_INITIALIZER;

__attribute__((weak)) uint32_t sg_thunk_register(sg_func *fn, const char *debug_name) {
    uint32_t slot;
    pthread_mutex_lock(&thunk_lock);
    if (sg_thunk_count >= NSLOTS) {
        pthread_mutex_unlock(&thunk_lock);
        fprintf(stderr, "image: thunk region full (%u slots)\n", (unsigned)NSLOTS);
        abort();
    }
    slot = sg_thunk_count++;
    sg_thunk_fn[slot] = fn;
    sg_image_thunk_names[slot] = debug_name;
    pthread_mutex_unlock(&thunk_lock);
    return SG_THUNK_ADDR(slot);
}

/* Slot 2: the IAT value for imports nobody resolves. Logs and aborts. */
static void thunk_unimpl(sg_ctx *ctx, uint8_t *mem) {
    fprintf(stderr, "spore: call to SG_THUNK_UNIMPL from guest 0x%08x\n", sg_ld32(mem, ctx->esp));
    abort();
}
/* Slots 0 and 1 are reached through sg_lookup only if guest code jumps to them; sg_call_guest and the
 * thread start code (runtime/core) normally consume them by comparing eip after the call returns. */
static void thunk_ret_to_host(sg_ctx *ctx, uint8_t *mem) {
    (void)mem;
    fprintf(stderr, "spore: guest code dispatched to SG_RET_TO_HOST (eip 0x%08x); runtime/core must handle it\n", ctx->eip);
    abort();
}
static void thunk_thread_exit(sg_ctx *ctx, uint8_t *mem) {
    (void)mem;
    fprintf(stderr, "spore: guest thread returned to SG_THREAD_EXIT (eax 0x%08x) without runtime/core\n", ctx->eax);
    abort();
}

void sg_thunks_init(uint8_t *mem) {
    if (sg_image_window_commit(SG_THUNK_BASE, SG_THUNK_LIMIT, PROT_READ | PROT_WRITE)) abort();
    if (mem[SG_THUNK_BASE] != 0xcc) memset(mem + SG_THUNK_BASE, 0xcc, SG_THUNK_LIMIT - SG_THUNK_BASE);
    sg_image_window_commit(SG_THUNK_BASE, SG_THUNK_LIMIT, PROT_READ);
    if (!sg_thunk_fn[0]) { sg_thunk_fn[0] = thunk_ret_to_host; sg_image_thunk_names[0] = "SG_RET_TO_HOST"; }
    if (!sg_thunk_fn[1]) { sg_thunk_fn[1] = thunk_thread_exit; sg_image_thunk_names[1] = "SG_THREAD_EXIT"; }
    if (!sg_thunk_fn[2]) { sg_thunk_fn[2] = thunk_unimpl; sg_image_thunk_names[2] = "SG_THUNK_UNIMPL"; }
}
