/* harness.c -- guest window, the runtime entry points libhle needs, and the test driver. */
#include "harness.h"
#include "w32_int.h"

#include <stdlib.h>
#include <sys/mman.h>
#include <sys/stat.h>
#include <unistd.h>

sg_ctx T;
uint8_t *M;
int t_fail, t_pass;
char t_root[1024];

/* ---- stand-ins for the runtime core (thunks, callbacks) ------------------------------- */

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

#define CB_BASE 0x00500000u
static host_cb callbacks[256];
static unsigned ncallbacks;
uint32_t register_callback(host_cb cb) {
    callbacks[ncallbacks] = cb;
    return CB_BASE + 16u * ncallbacks++; /* fake guest code addresses (never executed) */
}

/* runtime-abi 7.3 semantics for host callbacks standing in for guest code. */
uint32_t sg_call_guest(sg_ctx *ctx, uint32_t target, enum sg_cc cc, unsigned nargs, const uint32_t *args) {
    uint32_t esp0 = ctx->esp, r;
    unsigned i = (target - CB_BASE) / 16u;
    uint8_t *mem = ctx->mem;
    for (unsigned k = nargs; k-- > 0;) { ctx->esp -= 4; sg_st32(mem, ctx->esp, args[k]); }
    ctx->esp -= 4;
    sg_st32(mem, ctx->esp, SG_RET_TO_HOST);
    if (i >= ncallbacks || (target - CB_BASE) % 16u) { fprintf(stderr, "sg_call_guest: unknown target 0x%08x\n", target); abort(); }
    r = callbacks[i](ctx, nargs, args);
    (void)cc;
    ctx->eax = r;
    ctx->esp = esp0;
    return r;
}

/* ---- set-up ---------------------------------------------------------------------------- */

static void rm_rf(const char *p) {
    char cmd[2048];
    snprintf(cmd, sizeof cmd, "rm -rf '%s'", p);
    if (system(cmd)) fprintf(stderr, "warning: %s failed\n", cmd);
}

static w32_config cfg;
static char game[1100], sup[1100], docs[1100], tmpd[1100], regp[1100];

static void make_tree(void) {
    char cwd[900], p[1100];
    if (!getcwd(cwd, sizeof cwd)) abort();
    snprintf(t_root, sizeof t_root, "%s/test-root", cwd);
    rm_rf(t_root);
    mkdir(t_root, 0755);
    snprintf(game, sizeof game, "%s/game", t_root);
    mkdir(game, 0755);
    snprintf(p, sizeof p, "%s/game/SporebinEP1", t_root);
    mkdir(p, 0755);
    snprintf(sup, sizeof sup, "%s/support", t_root);
    snprintf(docs, sizeof docs, "%s/docs", t_root);
    snprintf(tmpd, sizeof tmpd, "%s/tmp", t_root);
    snprintf(regp, sizeof regp, "%s/support/registry.json", t_root);
    memset(&cfg, 0, sizeof cfg);
    cfg.game_dir = game;
    cfg.support_dir = sup;
    cfg.documents_dir = docs;
    cfg.temp_dir = tmpd;
    cfg.registry_file = regp;
    cfg.args = "-ep1 -dataDir:\"C:\\My Data\\\" x";
}

/* Fresh scratch tree, paths, environment and registry; the VM, heaps and stdio stay. */
void harness_reset(void) {
    make_tree();
    if (w32_paths_init(&cfg)) { fprintf(stderr, "paths init failed\n"); exit(2); }
    w32_env_init(&cfg);
    if (w32_reg_load(regp, 0)) { fprintf(stderr, "registry init failed\n"); exit(2); }
}

void harness_init(void) {
    make_tree();
    void *w = mmap(NULL, SG_WINDOW_SIZE + SG_WINDOW_GUARD, PROT_NONE, MAP_ANON | MAP_PRIVATE | MAP_NORESERVE, -1, 0);
    if (w == MAP_FAILED) { perror("mmap"); exit(2); }
    M = (uint8_t *)w;
    if (w32_init(M, &cfg)) { fprintf(stderr, "w32_init failed\n"); exit(2); }
    /* the loader's job in the real runtime: make the image range accessible (holds the IAT) */
    if (mprotect(M + SG_IMAGE_BASE, (SG_IMAGE_SIZE + 0x3fffu) & ~0x3fffu, PROT_READ | PROT_WRITE)) { perror("mprotect"); exit(2); }
    uint32_t teb = w32_vm_alloc_runtime(SG_TEB_REGION, 0x1000, W32_PAGE_READWRITE);
    uint32_t err, stack = w32_vm_alloc(0, 0x100000, W32_MEM_RESERVE | W32_MEM_COMMIT, W32_PAGE_READWRITE, &err);
    if (!teb || !stack) { fprintf(stderr, "harness: TEB/stack allocation failed\n"); exit(2); }
    memset(&T, 0, sizeof T);
    T.esp = stack + 0x100000 - 64;
    T.ebx = 0x11111111u; T.esi = 0x22222222u; T.edi = 0x33333333u; T.ebp = 0x44444444u;
    T.fs_base = teb;
    T.fpu.cw = 0x027f;
    T.fpu.tag = 0xffff;
    T.mxcsr = 0x1f80;
    T.mem = M;
    sg_st32(M, teb + SG_TEB_SELF, teb);
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
    T.esp = esp0;
    return T.eax;
}

uint32_t gstr(const char *s) {
    uint32_t n = w32_utf8_units(s, 0) + 1, a = hle_guest_alloc(n, 1);
    int fits;
    w32_put_str(M, a, n, s, 0, &fits);
    return a;
}
uint32_t gwstr(const char *s) {
    uint32_t n = w32_utf8_units(s, 1) + 1, a = hle_guest_alloc(2 * n, 2);
    int fits;
    w32_put_str(M, a, n, s, 1, &fits);
    return a;
}
uint32_t gbuf(uint32_t n) { return hle_guest_alloc(n, 16); }
const char *hwstr(uint32_t a) {
    static char bufs[8][2048];
    static int k;
    char *b = bufs[k++ & 7];
    w32_wstr_get(M, a, b, 2048);
    return b;
}
uint32_t last_error(void) { return sg_ld32(M, T.fs_base + SG_TEB_LAST_ERROR); }

int main(int argc, char **argv) {
    int bench = argc > 1 && !strcmp(argv[1], "--bench");
    hle_unimpl_policy = HLE_UNIMPL_CONTINUE;
    if (!getenv("SPORE_HLE_LOG")) hle_log_level = HLE_LOG_ERROR;
    harness_init();
    if (bench) { bench_heap(); return 0; }
    int last = 0;
#define GROUP(name) do { if (strcmp(name, "vm")) fprintf(stderr, "   %d checks\n", t_pass + t_fail - last); last = t_pass + t_fail; fprintf(stderr, "-- %s\n", name); } while (0)
    GROUP("vm");
    test_vm();
    harness_reset();
    GROUP("heap");
    test_heap();
    harness_reset();
    GROUP("path");
    test_path();
    harness_reset();
    GROUP("reg");
    test_reg();
    harness_reset();
    GROUP("file");
    test_file();
    harness_reset();
    GROUP("skipintro"); /* SPORE_SKIP_INTRO */
    { void test_skipintro(void); test_skipintro(); }
    harness_reset();
    GROUP("crt");
    test_crt();
    harness_reset();
    GROUP("module");
    test_module();
    /* directory watching, NLS, process, version strings */
    void test_nls(void), test_process(void), test_version(void), test_dirwatch(void);
    harness_reset();
    GROUP("nls");
    test_nls();
    harness_reset();
    GROUP("process");
    test_process();
    harness_reset();
    GROUP("version");
    test_version();
    harness_reset();
    GROUP("dirwatch");
    test_dirwatch();
    { void test_crypt(void); puts("[crypt]"); test_crypt(); }
    fprintf(stderr, "   %d checks\n", t_pass + t_fail - last);
    printf("w32 tests: %d passed, %d failed\n", t_pass, t_fail);
    return t_fail ? 1 : 0;
}
