/* test_main.c -- harness for the runtime-core fixture tests (see fixture.h). */
#include <stdarg.h>

#include "core_internal.h"
#include "fixture.h"

void test_events(void); /* test_events.c */
void test_stall(void);  /* test_stall.c */
void test_memorder(int assert_relaxed); /* test_memorder.c */

int t_pass, t_fail;
const char *t_name = "";
uint8_t *M;
sg_ctx *C;

/* ---- what the image linker generates: dispatch table, miss hook, resume points -------- */

uint32_t sg_dispatch_bucket[SG_DISPATCH_BUCKETS + 1];
sg_dispatch_entry sg_dispatch_entries[512];
static unsigned n_entries;
sg_resume_point sg_resume_points[1];
const uint32_t sg_resume_point_count = 0;

static sg_func *miss(sg_ctx *ctx, uint8_t *mem, uint32_t a) {
    (void)mem;
    sg_core_fatal(ctx, "dispatch miss at 0x%08x", a);
}
sg_miss_hook_fn sg_miss_hook = miss;

static void rebuild_buckets(void) {
    uint32_t b, i = 0;
    for (b = 0; b <= SG_DISPATCH_BUCKETS; b++) {
        uint32_t lo = SG_TEXT_START + 16u * b;
        while (i < n_entries && sg_dispatch_entries[i].guest < lo) i++;
        sg_dispatch_bucket[b] = i;
    }
}

void fx_register(uint32_t guest, sg_func *fn) {
    unsigned j;
    for (j = 0; j < n_entries; j++)
        if (sg_dispatch_entries[j].guest == guest) { sg_dispatch_entries[j].fn = fn; return; }
    for (j = n_entries; j > 0 && sg_dispatch_entries[j - 1].guest > guest; j--) sg_dispatch_entries[j] = sg_dispatch_entries[j - 1];
    sg_dispatch_entries[j].guest = guest;
    sg_dispatch_entries[j].flags = SG_DE_FUNC;
    sg_dispatch_entries[j].fn = fn;
    n_entries++;
    rebuild_buckets();
}

static sg_resume_point rp[128];
static unsigned n_rp;
void fx_resume_point(uint32_t resume, uint32_t owner) {
    unsigned j;
    for (j = 0; j < n_rp; j++)
        if (rp[j].resume == resume) return;
    for (j = n_rp; j > 0 && rp[j - 1].resume > resume; j--) rp[j] = rp[j - 1];
    rp[j].resume = resume;
    rp[j].func = owner;
    n_rp++;
    sg_core_resume_table = rp;
    sg_core_resume_table_count = n_rp;
}

/* ---- guest data --------------------------------------------------------------------------------- */

static uint32_t data_top = FX_DATA;
uint32_t fx_alloc(uint32_t n) {
    uint32_t a = data_top;
    data_top = (data_top + n + 15u) & ~15u;
    if (data_top > FX_DATA + 0x400000u) { fprintf(stderr, "fixture data exhausted\n"); abort(); }
    memset(M + a, 0, n);
    return a;
}
uint32_t fx_words(unsigned n, ...) {
    va_list ap;
    uint32_t a = fx_alloc(4u * (n ? n : 1u)), i;
    va_start(ap, n);
    for (i = 0; i < n; i++) sg_st32(M, a + 4 * i, va_arg(ap, uint32_t));
    va_end(ap);
    return a;
}
uint32_t fx_str(const char *s) {
    uint32_t n = (uint32_t)strlen(s) + 1, a = fx_alloc(n);
    memcpy(M + a, s, n);
    return a;
}
void fx_data_reset(void) { data_top = FX_DATA; }

/* ---- guest calls ------------------------------------------------------------------------------- */

static uint32_t ra_next = 0x00ff0000u;
uint32_t gcall(sg_ctx *ctx, sg_func *fn, int cdecl_, unsigned n, const uint32_t *args) {
    uint8_t *mem = ctx->mem;
    uint32_t esp0 = ctx->esp, ra = __atomic_add_fetch(&ra_next, 4u, __ATOMIC_RELAXED);
    unsigned k;
    for (k = n; k-- > 0;) SG_PUSH32(args[k]);
    SG_PUSH32(ra);
    fn(ctx, mem);
    if (ctx->eip != ra) sg_core_fatal(ctx, "gcall: returned to 0x%08x, expected 0x%08x", ctx->eip, ra);
    if (cdecl_) {
        if (ctx->esp != esp0 - 4u * n) sg_core_fatal(ctx, "gcall: cdecl callee left esp off by %d", (int)(ctx->esp - (esp0 - 4u * n)));
        ctx->esp = esp0;
    } else if (ctx->esp != esp0) {
        sg_core_fatal(ctx, "gcall: stdcall callee left esp off by %d", (int)(ctx->esp - esp0));
    }
    return ctx->eax;
}

/* ---- log ---------------------------------------------------------------------------------------- */

fx_event fx_log_buf[256];
unsigned fx_nlog;
static pthread_mutex_t log_lock = PTHREAD_MUTEX_INITIALIZER;
void fx_log(int ev, uint32_t a, uint32_t b) {
    pthread_mutex_lock(&log_lock);
    if (fx_nlog < 256) { fx_log_buf[fx_nlog].ev = ev; fx_log_buf[fx_nlog].a = a; fx_log_buf[fx_nlog].b = b; fx_nlog++; }
    pthread_mutex_unlock(&log_lock);
}
void fx_log_reset(void) { fx_nlog = 0; }
void fx_log_dump(void) {
    static const char *const names[] = {"none", "throw", "catch", "dtor", "copy", "unwind", "after", "filter",
                                        "finally", "except", "rethrow", "cookie", "misc", "cleanup"};
    unsigned i;
    for (i = 0; i < fx_nlog; i++)
        fprintf(stderr, "    %-8s %08x %08x\n", names[fx_log_buf[i].ev], fx_log_buf[i].a, fx_log_buf[i].b);
}
int fx_log_kinds(const int *kinds, unsigned n) {
    unsigned i;
    if (fx_nlog != n) return 0;
    for (i = 0; i < n; i++)
        if (fx_log_buf[i].ev != kinds[i]) return 0;
    return 1;
}

/* ---- fatal capture ------------------------------------------------------------------------------ */

jmp_buf fx_fatal_jb;
int fx_fatal_armed;
char fx_fatal_msg[512];
static void fatal_hook(sg_ctx *ctx, const char *msg) {
    (void)ctx;
    snprintf(fx_fatal_msg, sizeof fx_fatal_msg, "%s", msg);
    if (fx_fatal_armed && sg_self && C && &sg_self->ctx == C) {
        core_thread *h = core_th(sg_self);
        h->ninflight = 0;
        h->cleanup = NULL;
        longjmp(fx_fatal_jb, 1);
    }
}

static void quiet_sink(int level, const char *line) {
    if (level <= 0 && !fx_fatal_armed) fprintf(stderr, "[core] %s\n", line);
}

int main(int argc, char **argv) {
    const char *only = argc > 1 ? argv[1] : NULL;
    sg_thread *t;
    M = sg_window_reserve();
    if (!M) { fprintf(stderr, "cannot reserve the window\n"); return 2; }
    sg_core_init(M);
    sg_core_log_sink = quiet_sink;
    sg_core_fatal_hook = fatal_hook;
    if (sg_window_protect(FX_DATA, 0x400000u, 3)) { fprintf(stderr, "cannot commit fixture data\n"); return 2; }
    /* a fake image TLS template inside the fixture data: 12 bytes */
    {
        uint32_t idx = fx_alloc(4), tmpl = fx_words(3, 0x11111111u, 0x22222222u, 0x33333333u);
        sg_st32(M, idx, 0xdeadbeefu);
        sg_core_image_tls(idx, tmpl, 12, 4);
        CHECK_EQ(sg_ld32(M, idx), 0);
    }
    t = sg_core_attach_main(0);
    C = &t->ctx;
    if (!only || !strcmp(only, "eh")) test_eh();
    if (!only || !strcmp(only, "seh")) test_seh();
    if (!only || !strcmp(only, "tls")) test_tls();
    if (!only || !strcmp(only, "threads")) test_threads();
    if (!only || !strcmp(only, "misc")) test_misc();
    if (!only || !strcmp(only, "events")) test_events();
    if (!only || !strcmp(only, "stall")) test_stall();
    if (!only || !strcmp(only, "memorder")) test_memorder(0);
    if (only && !strcmp(only, "memorder-relaxed")) test_memorder(1);
    if (only && !strcmp(only, "bench")) test_bench();
    printf("runtime core tests: %d passed, %d failed\n", t_pass, t_fail);
    return t_fail ? 1 : 0;
}
