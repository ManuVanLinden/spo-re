/* harness.c -- guest window, fake runtime entry points, and the test driver. */
#include "harness.h"

#include <stdlib.h>
#include <sys/mman.h>

sg_ctx T;
uint8_t *M;
int t_fail, t_pass;

static uint32_t heap_top = G_HEAP;

/* ---- runtime entry points the shims use (the real runtime core provides these) ---------- */

uint32_t hle_guest_alloc(uint32_t size, uint32_t align) {
    uint32_t a;
    if (!align) align = 1;
    heap_top = (heap_top + align - 1) & ~(align - 1);
    a = heap_top;
    heap_top += size;
    if (heap_top > G_HEAP + 0x04000000u) { fprintf(stderr, "test heap exhausted\n"); abort(); }
    memset(M + a, 0, size);
    return a;
}

sg_func *sg_thunk_fn[(SG_THUNK_LIMIT - SG_THUNK_BASE) / SG_THUNK_SLOT];
static unsigned next_slot = SG_THUNK_FIRST_FREE;
uint32_t sg_thunk_register(sg_func *fn, const char *debug_name) {
    (void)debug_name;
    sg_thunk_fn[next_slot] = fn;
    return SG_THUNK_ADDR(next_slot++);
}

void sg_misaligned_atomic(sg_ctx *ctx, uint32_t a) {
    (void)ctx;
    fprintf(stderr, "misaligned atomic at 0x%08x\n", a);
    abort();
}

static host_cb callbacks[64];
static unsigned ncallbacks;
uint32_t register_callback(host_cb cb) {
    callbacks[ncallbacks] = cb;
    return 0x00500000u + 16u * ncallbacks++; /* fake guest code addresses */
}

/* Mimics the runtime's sg_call_guest for host callbacks standing in for guest code: pushes
 * the arguments and SG_RET_TO_HOST, "runs" the callee (cdecl: it leaves the stack), then the
 * caller removes the arguments, as runtime-abi 7.3 specifies. */
uint32_t sg_call_guest(sg_ctx *ctx, uint32_t target, enum sg_cc cc, unsigned nargs, const uint32_t *args) {
    uint32_t esp0 = ctx->esp, r;
    unsigned i = (target - 0x00500000u) / 16u;
    uint8_t *mem = ctx->mem;
    for (unsigned k = nargs; k-- > 0;) { ctx->esp -= 4; sg_st32(mem, ctx->esp, args[k]); }
    ctx->esp -= 4;
    sg_st32(mem, ctx->esp, SG_RET_TO_HOST);
    if (i >= ncallbacks) { fprintf(stderr, "sg_call_guest: unknown target 0x%08x\n", target); abort(); }
    r = callbacks[i](ctx, nargs, args);
    (void)cc;
    ctx->eax = r;
    ctx->esp = esp0;
    return r;
}

/* ---- window -------------------------------------------------------------------------- */

static void map_rw(uint32_t lo, uint32_t hi) {
    if (mprotect(M + lo, hi - lo, PROT_READ | PROT_WRITE)) { perror("mprotect"); exit(2); }
}

void harness_init(void) {
    void *p = mmap(NULL, SG_WINDOW_SIZE + SG_WINDOW_GUARD, PROT_NONE, MAP_ANON | MAP_PRIVATE | MAP_NORESERVE, -1, 0);
    if (p == MAP_FAILED) { perror("mmap"); exit(2); }
    M = (uint8_t *)p;
    map_rw(SG_IMAGE_BASE, SG_IMAGE_BASE + SG_IMAGE_SIZE); /* empty image area: the IAT lives here */
    map_rw(G_HEAP, G_HEAP + 0x04000000u);
    map_rw(G_STACK_TOP - 0x00100000u, G_STACK_TOP);
    map_rw(SG_THUNK_BASE, SG_THUNK_BASE + 0x00100000u);
    map_rw(G_TEB, G_TEB + 0x1000u);
    memset(&T, 0, sizeof T);
    T.esp = G_STACK_TOP - 64;
    T.ebx = 0x11111111u; T.esi = 0x22222222u; T.edi = 0x33333333u; T.ebp = 0x44444444u;
    T.fs_base = G_TEB;
    T.fpu.cw = 0x027f;
    T.fpu.tag = 0xffff;
    T.mxcsr = 0x1f80;
    T.mem = M;
    sg_st32(M, G_TEB + SG_TEB_SELF, G_TEB);
}

uint32_t call_shim(sg_func *fn, unsigned cleanup, unsigned n, const uint32_t *args, const char *what) {
    uint32_t esp0 = T.esp, ebx = T.ebx, esi = T.esi, edi = T.edi, ebp = T.ebp;
    for (unsigned k = n; k-- > 0;) { T.esp -= 4; sg_st32(M, T.esp, args[k]); }
    T.esp -= 4;
    sg_st32(M, T.esp, G_RET);
    T.eip = 0;
    fn(&T, M);
    if (T.eip != G_RET || T.esp != esp0 - 4u * n + cleanup || T.ebx != ebx || T.esi != esi || T.edi != edi ||
        T.ebp != ebp) {
        t_fail++;
        fprintf(stderr, "FAIL stack discipline in %s: eip=0x%08x esp delta=%d (want %d)\n", what, T.eip,
                (int)(T.esp - esp0), (int)cleanup - 4 * (int)n);
    }
    T.esp = esp0; /* cdecl: the caller removes the arguments */
    return T.eax;
}

/* ---- guest data helpers ------------------------------------------------------------------- */

uint32_t gstr(const char *s) {
    uint32_t n = (uint32_t)strlen(s) + 1, a = hle_guest_alloc(n, 1);
    memcpy(M + a, s, n);
    return a;
}
uint32_t gwstr(const char *s) {
    uint32_t n = (uint32_t)strlen(s) + 1, a = hle_guest_alloc(2 * n, 2);
    for (uint32_t i = 0; i < n; i++) sg_st16(M, a + 2 * i, (uint8_t)s[i]);
    return a;
}
uint32_t gwstr16(const uint16_t *s) {
    uint32_t n = 0, a;
    while (s[n]) n++;
    a = hle_guest_alloc(2 * (n + 1), 2);
    for (uint32_t i = 0; i <= n; i++) sg_st16(M, a + 2 * i, s[i]);
    return a;
}
uint32_t gbuf(uint32_t n) { return hle_guest_alloc(n, 16); }
uint32_t gdouble(double d, uint32_t *lo, uint32_t *hi) {
    uint64_t b;
    memcpy(&b, &d, 8);
    *lo = (uint32_t)b;
    *hi = (uint32_t)(b >> 32);
    return 0;
}
const char *hstr(uint32_t a) { return (const char *)(M + a); }
int wstreq(uint32_t a, const char *s) {
    uint32_t i = 0;
    for (;; i++) {
        uint16_t u = sg_ld16(M, a + 2 * i);
        if (u != (uint8_t)s[i]) return 0;
        if (!u) return 1;
    }
}

int main(void) {
    harness_init();
    hle_unimpl_policy = HLE_UNIMPL_CONTINUE;
    test_table();
    test_string();
    test_conv();
    test_format();
    test_scan();
    test_math();
    test_time();
    test_nls();
    test_misc();
    test_net();
    test_steam();
    test_boot_stops();
    test_shell(); /* ShellExecuteW */
    printf("hle tests: %d passed, %d failed\n", t_pass, t_fail);
    return t_fail ? 1 : 0;
}
