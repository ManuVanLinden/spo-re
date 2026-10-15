/* com_harness.c -- see com_harness.h. Stands in for runtime/image and runtime/win32 in host tests. */
#define SG_DISPATCH_CONST /* writable, all zero: no translated code in these tests */
#include "com_harness.h"

#include <stdlib.h>
#include <sys/mman.h>

sg_ctx T;
uint8_t *M;
int t_fail, t_pass;

uint32_t sg_dispatch_bucket[SG_DISPATCH_BUCKETS + 1];
sg_dispatch_entry sg_dispatch_entries[1];
static sg_func *miss(sg_ctx *ctx, uint8_t *mem, uint32_t a) {
    (void)mem;
    fprintf(stderr, "harness: dispatch miss at 0x%08x (esp 0x%08x)\n", a, ctx->esp);
    abort();
}
sg_miss_hook_fn sg_miss_hook = miss;
void sg_return_mismatch(sg_ctx *ctx, uint8_t *mem, uint32_t expected) {
    (void)mem;
    fprintf(stderr, "harness: return mismatch: expected 0x%08x, got 0x%08x\n", expected, ctx->eip);
    abort();
}

#define HEAP_LO 0x20000000u
#define HEAP_SIZE 0x10000000u
static uint32_t heap_top = HEAP_LO;
uint32_t hle_guest_alloc(uint32_t size, uint32_t align) {
    uint32_t a;
    if (!align) align = 1;
    heap_top = (heap_top + align - 1) & ~(align - 1);
    a = heap_top;
    heap_top += size;
    if (heap_top > HEAP_LO + HEAP_SIZE) { fprintf(stderr, "test heap exhausted\n"); abort(); }
    memset(M + a, 0, size);
    return a;
}
uint32_t gscratch(uint32_t size) { return hle_guest_alloc(size, 16); }
uint32_t gld(uint32_t a) { return sg_ld32(M, a); }
void gst(uint32_t a, uint32_t v) { sg_st32(M, a, v); }

sg_func *sg_thunk_fn[(SG_THUNK_LIMIT - SG_THUNK_BASE) / SG_THUNK_SLOT];
static unsigned next_slot = SG_THUNK_FIRST_FREE;
uint32_t sg_thunk_register(sg_func *fn, const char *debug_name) {
    (void)debug_name;
    sg_thunk_fn[next_slot] = fn;
    return SG_THUNK_ADDR(next_slot++);
}

static void map_rw(uint32_t lo, uint32_t hi) {
    if (mprotect(M + lo, hi - lo, PROT_READ | PROT_WRITE)) { perror("mprotect"); exit(2); }
}
void harness_init(void) {
    void *p = mmap(NULL, SG_WINDOW_SIZE + SG_WINDOW_GUARD, PROT_NONE, MAP_ANON | MAP_PRIVATE | MAP_NORESERVE, -1, 0);
    if (p == MAP_FAILED) { perror("mmap"); exit(2); }
    M = (uint8_t *)p;
    map_rw(HEAP_LO, HEAP_LO + HEAP_SIZE);
    map_rw(G_STACK_TOP - 0x00100000u, G_STACK_TOP);
    map_rw(SG_THUNK_BASE, SG_THUNK_LIMIT);
    memset(M + SG_THUNK_BASE, 0xcc, SG_THUNK_LIMIT - SG_THUNK_BASE);
    memset(&T, 0, sizeof T);
    T.mem = M;
    T.esp = G_STACK_TOP - 64;
    sg_com_init(M);
}

/* The shape of `push args...; push obj; mov ecx,[obj]; call [ecx+4*slot]` in translated code. */
static void translated_caller(sg_ctx *SG_RESTRICT ctx, uint8_t *mem, uint32_t obj, unsigned slot, unsigned n, const uint32_t *args) {
    for (unsigned i = n; i-- > 0;) SG_PUSH32(args[i]);
    SG_PUSH32(obj);
    ctx->ecx = sg_ld32(mem, obj);
    SG_CALL_IND(sg_ld32(mem, ctx->ecx + 4u * slot), 0x00401234u);
}
uint32_t gcallv(uint32_t obj, unsigned slot, unsigned n, const uint32_t *args) {
    uint32_t esp0 = T.esp;
    T.ebx = 0x1111; T.esi = 0x2222; T.edi = 0x3333; T.ebp = 0x4444;
    translated_caller(&T, M, obj, slot, n, args);
    if (T.esp != esp0) {
        t_fail++;
        fprintf(stderr, "gcall: slot %u: callee left esp off by %d bytes\n", slot, (int)(T.esp - esp0));
        T.esp = esp0;
    }
    if (T.ebx != 0x1111 || T.esi != 0x2222 || T.edi != 0x3333 || T.ebp != 0x4444) {
        t_fail++;
        fprintf(stderr, "gcall: slot %u: callee changed a preserved register\n", slot);
    }
    return T.eax;
}
uint32_t gcall(uint32_t obj, unsigned slot, unsigned n, ...) {
    uint32_t a[16];
    va_list ap;
    va_start(ap, n);
    for (unsigned i = 0; i < n && i < 16; i++) a[i] = va_arg(ap, uint32_t);
    va_end(ap);
    return gcallv(obj, slot, n, a);
}
int harness_report(const char *name) {
    printf("%s: %d passed, %d failed\n", name, t_pass, t_fail);
    return t_fail ? 1 : 0;
}
