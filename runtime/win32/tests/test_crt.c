/* test_crt.c -- the msvcr90 start-up imports: __getmainargs, _initterm(_e), _onexit,
 * __dllonexit, exit/_cexit, _lock, and the small pre_c_init imports. */
#include "harness.h"
#include "w32_int.h"

#include <setjmp.h>
#include <stdlib.h>

static int order[64], norder;
static uint32_t cb_record(sg_ctx *ctx, unsigned n, const uint32_t *a) { (void)ctx; (void)n; (void)a; order[norder++] = 1; return 0; }
static uint32_t cb_a(sg_ctx *ctx, unsigned n, const uint32_t *a) { (void)ctx; (void)n; (void)a; order[norder++] = 'a'; return 0; }
static uint32_t cb_b(sg_ctx *ctx, unsigned n, const uint32_t *a) { (void)ctx; (void)n; (void)a; order[norder++] = 'b'; return 0; }
static uint32_t cb_fail(sg_ctx *ctx, unsigned n, const uint32_t *a) { (void)ctx; (void)n; (void)a; order[norder++] = 'f'; return 27; }
static uint32_t late_fn;
/* an atexit handler that registers another one while exit runs (msvcr90 then runs it too) */
static uint32_t cb_reg(sg_ctx *ctx, unsigned n, const uint32_t *a) {
    (void)ctx; (void)n; (void)a;
    order[norder++] = 'r';
    w32_onexit_add(late_fn);
    return 0;
}
static uint32_t cb_late(sg_ctx *ctx, unsigned n, const uint32_t *a) { (void)ctx; (void)n; (void)a; order[norder++] = 'l'; return 0; }

static jmp_buf exit_jb;
static int exit_code = -1;
static void test_exit_hook(int code) { exit_code = code; longjmp(exit_jb, 1); }

/* argv as a list joined by '|' */
static const char *argv_of(const char *args) {
    static char out[1024];
    w32_config c;
    memset(&c, 0, sizeof c);
    c.args = args;
    w32_env_init(&c);
    uint32_t argc, argv, envp;
    out[0] = 0;
    if (w32_getmainargs(M, &argc, &argv, &envp)) return "(fail)";
    for (uint32_t i = 1; i < argc; i++) {
        if (i > 1) strcat(out, "|");
        strcat(out, (const char *)M + sg_ld32(M, argv + 4 * i));
    }
    if (sg_ld32(M, argv + 4 * argc) != 0) return "(no NULL terminator)";
    return out;
}

void test_crt(void) {
    /* __getmainargs with the harness's command line: -ep1 -dataDir:"C:\My Data\" x */
    uint32_t pc = gbuf(4), pv = gbuf(4), pe = gbuf(4), si = gbuf(4);
    CHECK_EQ(CALL(w32_imp_msvcr90___getmainargs, 0, pc, pv, pe, 0, si), 0);
    CHECK_EQ(sg_ld32(M, pc), 3);
    uint32_t argv = sg_ld32(M, pv);
    CHECK_STR((const char *)M + sg_ld32(M, argv), "C:\\Spore\\SporebinEP1\\SporeApp.exe");
    CHECK_STR((const char *)M + sg_ld32(M, argv + 4), "-ep1");
    CHECK_STR((const char *)M + sg_ld32(M, argv + 8), "-dataDir:C:\\My Data\" x"); /* \" is a literal quote */
    CHECK_EQ(sg_ld32(M, argv + 12), 0);
    uint32_t envp = sg_ld32(M, pe);
    int found_os = 0, nenv = 0;
    for (; sg_ld32(M, envp + 4 * nenv); nenv++)
        if (!strcmp((const char *)M + sg_ld32(M, envp + 4 * nenv), "OS=Windows_NT")) found_os = 1;
    CHECK(found_os);
    CHECK(nenv >= 15);

    /* msvcr90 parse_cmdline rules (the "Parsing C command-line arguments" table) */
    CHECK_STR(argv_of("\"abc\" d e"), "abc|d|e");
    CHECK_STR(argv_of("a\\\\b d\"e f\"g h"), "a\\\\b|de fg|h");
    CHECK_STR(argv_of("a\\\\\\\"b c d"), "a\\\"b|c|d");
    CHECK_STR(argv_of("a\\\\\\\\\"b c\" d e"), "a\\\\b c|d|e");
    CHECK_STR(argv_of("a\"b\"\" c d"), "ab\" c d");
    CHECK_STR(argv_of("   spaced\targs  "), "spaced|args");
    CHECK_STR(argv_of("\"\" empty"), "|empty");
    CHECK_STR(argv_of("-locale:fr-fr -windowed"), "-locale:fr-fr|-windowed");
    CHECK_STR(argv_of("café"), "caf\xe9"); /* the ANSI view is code page 1252 */
    harness_reset();

    /* pre_c_init's imports */
    CALL(w32_imp_msvcr90___set_app_type, 0, 2);
    uint32_t fm = CALL0(w32_imp_msvcr90___p__fmode, 0), cm = CALL0(w32_imp_msvcr90___p__commode, 0);
    CHECK(fm && cm && fm != cm);
    CHECK_EQ(sg_ld32(M, fm), 0);
    CALL(w32_imp_msvcr90___setusermatherr, 0, 0x11e1585);

    /* _initterm: every non-NULL entry, in order; _initterm_e stops at the first non-zero */
    uint32_t a = register_callback(cb_a), b = register_callback(cb_b), f = register_callback(cb_fail);
    uint32_t tab = gbuf(32);
    sg_st32(M, tab, a); sg_st32(M, tab + 4, 0); sg_st32(M, tab + 8, b); sg_st32(M, tab + 12, a);
    norder = 0;
    uint32_t before = w32_initterm_done;
    CALL(w32_imp_msvcr90__initterm, 0, tab, tab + 16);
    CHECK_EQ(norder, 3);
    CHECK(order[0] == 'a' && order[1] == 'b' && order[2] == 'a');
    CHECK_EQ(w32_initterm_done - before, 3);
    CHECK_EQ(w32_initterm_current, 0);
    sg_st32(M, tab + 4, f);
    norder = 0;
    CHECK_EQ(CALL(w32_imp_msvcr90__initterm_e, 0, tab, tab + 16), 27);
    CHECK_EQ(norder, 2);
    sg_st32(M, tab + 4, 0);
    CHECK_EQ(CALL(w32_imp_msvcr90__initterm_e, 0, tab, tab + 16), 0);
    CHECK_EQ(CALL(w32_imp_msvcr90__initterm, 0, tab, tab), 0 * 0); /* empty range */

    /* _onexit + _cexit: LIFO; handlers registered during exit run too */
    late_fn = register_callback(cb_late);
    uint32_t r = register_callback(cb_record), rg = register_callback(cb_reg);
    CHECK_EQ(CALL(w32_imp_msvcr90__onexit, 0, a), a);
    CHECK_EQ(CALL(w32_imp_msvcr90__onexit, 0, rg), rg);
    CHECK_EQ(CALL(w32_imp_msvcr90__onexit, 0, b), b);
    norder = 0;
    CALL0(w32_imp_msvcr90__cexit, 0);
    CHECK_EQ(norder, 4);
    CHECK(order[0] == 'b' && order[1] == 'r' && order[2] == 'l' && order[3] == 'a');
    CHECK_EQ(w32_onexit_count(), 0);

    /* exit(code): runs the table, then the exit hook (here: back to the test) */
    CALL(w32_imp_msvcr90__onexit, 0, r);
    void (*saved)(int) = w32_exit_hook;
    w32_exit_hook = test_exit_hook;
    norder = 0;
    if (!setjmp(exit_jb)) {
        uint32_t args[1] = {3};
        call_shim(w32_imp_msvcr90_exit, 0, 1, args, "exit");
        CHECK(0); /* not reached */
    }
    CHECK_EQ(exit_code, 3);
    CHECK_EQ(norder, 1);
    exit_code = -1;
    CALL(w32_imp_msvcr90__onexit, 0, r);
    norder = 0;
    if (!setjmp(exit_jb)) {
        uint32_t args[1] = {5};
        call_shim(w32_imp_msvcr90__exit, 0, 1, args, "_exit");
    }
    CHECK_EQ(exit_code, 5);
    CHECK_EQ(norder, 0); /* _exit runs no handlers */
    w32_exit_hook = saved;
    norder = 0;
    CALL0(w32_imp_msvcr90__cexit, 0); /* the one left by _exit */
    CHECK_EQ(norder, 1);

    /* __dllonexit: a module table in guest memory that grows */
    uint32_t pb = gbuf(4), pend = gbuf(4);
    sg_st32(M, pb, 0xffffffffu);
    sg_st32(M, pend, 0xffffffffu);
    for (uint32_t i = 0; i < 100; i++) CHECK_EQ(CALL(w32_imp_msvcr90___dllonexit, 0, 0x500000u + i, pb, pend), 0x500000u + i);
    uint32_t tb = sg_ld32(M, pb), te = sg_ld32(M, pend);
    CHECK_EQ(te - tb, 400);
    CHECK_EQ(sg_ld32(M, tb + 4 * 57), 0x500000u + 57);
    CHECK(w32_msize(tb) >= 400);

    /* _lock / _unlock nest (recursive), and other threads would block */
    CALL(w32_imp_msvcr90__lock, 0, 8);
    CALL(w32_imp_msvcr90__lock, 0, 8);
    CALL(w32_imp_msvcr90__unlock, 0, 8);
    CALL(w32_imp_msvcr90__unlock, 0, 8);
    CHECK(1);
}
