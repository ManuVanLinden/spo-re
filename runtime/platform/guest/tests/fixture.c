/* fixture.c -- what the image linker would provide for a host-only test of the guest window
 * layer: the guest window, a run-time dispatch table for our own fixture functions, a bump guest
 * allocator standing in for runtime/win32's heap, and the guest main thread. Patterned on
 * runtime/core/tests/test_main.c. No game code or game bytes. */
#include <pthread.h>
#include <stdlib.h>

#include "fixture.h"

int t_pass, t_fail;
uint8_t *M;
sg_ctx *C;

uint32_t sg_dispatch_bucket[SG_DISPATCH_BUCKETS + 1];
sg_dispatch_entry sg_dispatch_entries[64];
static unsigned n_entries;
sg_resume_point sg_resume_points[1];
const uint32_t sg_resume_point_count = 0;

static sg_func *miss(sg_ctx *ctx, uint8_t *mem, uint32_t a) {
    (void)mem;
    sg_core_fatal(ctx, "dispatch miss at 0x%08x", a);
}
sg_miss_hook_fn sg_miss_hook = miss;

void fx_register(uint32_t guest, sg_func *fn) {
    unsigned j, i = 0;
    uint32_t b;
    for (j = n_entries; j > 0 && sg_dispatch_entries[j - 1].guest > guest; j--) sg_dispatch_entries[j] = sg_dispatch_entries[j - 1];
    sg_dispatch_entries[j].guest = guest;
    sg_dispatch_entries[j].flags = SG_DE_FUNC;
    sg_dispatch_entries[j].fn = fn;
    n_entries++;
    for (b = 0; b <= SG_DISPATCH_BUCKETS; b++) {
        uint32_t lo = SG_TEXT_START + 16u * b;
        while (i < n_entries && sg_dispatch_entries[i].guest < lo) i++;
        sg_dispatch_bucket[b] = i;
    }
}

/* runtime/win32's guest heap stands in: a bump allocator in the fixture data region. */
static uint32_t data_top = FX_DATA;
static pthread_mutex_t alloc_lock = PTHREAD_MUTEX_INITIALIZER;
uint32_t hle_guest_alloc(uint32_t size, uint32_t align) {
    uint32_t a;
    if (align < 16) align = 16;
    pthread_mutex_lock(&alloc_lock);
    a = (data_top + align - 1) & ~(align - 1);
    data_top = a + size;
    pthread_mutex_unlock(&alloc_lock);
    if (data_top > FX_DATA + 0x400000u) { fprintf(stderr, "fixture data exhausted\n"); abort(); }
    memset(M + a, 0, size);
    return a;
}

static uint32_t ra_next = 0x00ff0000u;
uint32_t gcall(sg_ctx *ctx, sg_func *fn, unsigned n, const uint32_t *args) {
    uint8_t *mem = ctx->mem;
    uint32_t esp0 = ctx->esp, ra = (ra_next += 4u);
    unsigned k;
    for (k = n; k-- > 0;) SG_PUSH32(args[k]);
    SG_PUSH32(ra);
    fn(ctx, mem);
    if (ctx->eip != ra) { fprintf(stderr, "gcall: returned to 0x%08x, expected 0x%08x\n", ctx->eip, ra); abort(); }
    if (ctx->esp != esp0) { fprintf(stderr, "gcall: stdcall callee left esp off by %d\n", (int)(ctx->esp - esp0)); abort(); }
    return ctx->eax;
}

int fx_init(void) {
    sg_thread *t;
    M = sg_window_reserve();
    if (!M) return -1;
    sg_core_init(M);
    if (sg_window_protect(FX_DATA, 0x400000u, 3)) return -1;
    t = sg_core_attach_main(0);
    if (!t) return -1;
    C = &t->ctx;
    /* the IAT lives in the image's .rdata (0x013cc000..): commit it so hle_install_imports can fill it */
    if (sg_window_protect(0x013c0000u, 0x20000u, 3)) return -1;
    hle_install_imports(M);
    return 0;
}
