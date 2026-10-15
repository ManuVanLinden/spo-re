/* core_boot_diag.c -- boot diagnostics up to WinMain, and the stop before the first window.
 *
 * sg_boot (core_boot.c) calls sg_boot_diag_install() before running the PE entry. It
 *   - counts static constructors through runtime/win32's _initterm hook (found by dlsym, so the
 *     core still links without runtime/win32),
 *   - installs a SIGABRT/SIGSEGV/SIGBUS/SIGILL/SIGTRAP/SIGALRM reporter that prints the constructor
 *     count, the current constructor, the last guest return eip and the host backtrace (whose
 *     sg_f_XXXXXXXX frames are the guest call stack), then chains to the previous handler,
 *   - arms a watchdog (SPORE_WATCHDOG seconds, default 0 = off).
 * sg_boot_winmain stands in for the CRT's `call WinMain` at 0x11e105c in the boot harness build
 * (tools/harness/prep_src.py).
 * With SPORE_STOP_AT_WINMAIN set it reports that WinMain was reached and exits 0;
 * otherwise it forwards to WinMain 0xf489e0 through dispatch. */
#include <dlfcn.h>
#include <execinfo.h>
#include <signal.h>
#include <time.h>
#include <unistd.h>

#include "core_internal.h"

#define BOOT_WINMAIN 0x00f489e0u
#define BOOT_INITTERM_E_COUNT 3u /* non-null slots of the _initterm_e table 0x13eb09c-0x13eb0ac [observed] */

static volatile uint32_t *ctor_done, *ctor_current;
static struct sigaction prev_sa[32];
static struct timespec t_start;

static void put(const char *s) { (void)!write(2, s, strlen(s)); }

static void progress(char *buf, size_t n) {
    uint32_t d = ctor_done ? *ctor_done : 0, c = ctor_current ? *ctor_current : 0;
    snprintf(buf, n,
             "boot: initializers run %u (_initterm_e %u, _initterm static constructors %u of 31266); current 0x%08x; "
             "last ret eip=0x%08x esp=0x%08x ebp=0x%08x\n",
             d, d < BOOT_INITTERM_E_COUNT ? d : BOOT_INITTERM_E_COUNT, d > BOOT_INITTERM_E_COUNT ? d - BOOT_INITTERM_E_COUNT : 0, c,
             sg_self ? sg_self->ctx.eip : 0, sg_self ? sg_self->ctx.esp : 0, sg_self ? sg_self->ctx.ebp : 0);
}

static void report(int sig) {
    char buf[320];
    void *bt[64];
    int n;
    snprintf(buf, sizeof buf, "\nboot: stop on signal %d (%s)\n", sig, sig == SIGALRM ? "watchdog" : strsignal(sig));
    put(buf);
    progress(buf, sizeof buf);
    put(buf);
    n = backtrace(bt, 64);
    put("boot: host backtrace (sg_f_XXXXXXXX frames are guest functions):\n");
    backtrace_symbols_fd(bt, n, 2);
}

static void on_signal(int sig, siginfo_t *si, void *uc) {
    struct sigaction *p = &prev_sa[sig & 31];
    report(sig);
    if (sig != SIGALRM && (p->sa_flags & SA_SIGINFO) && p->sa_sigaction) {
        p->sa_sigaction(sig, si, uc);
        return;
    }
    signal(sig, SIG_DFL);
    if (sig == SIGALRM) sig = SIGKILL;
    raise(sig);
}

static void on_initterm(uint32_t index, uint32_t fn) {
    static int every = -1;
    if (every < 0) {
        const char *e = getenv("SPORE_BOOT_PROGRESS");
        every = e ? atoi(e) : 0;
    }
    if (every > 0 && index % (uint32_t)every == 0) fprintf(stderr, "boot: initializer %u: 0x%08x\n", index, fn);
}

static int past_winmain;

static double ms_since_start(void) {
    struct timespec t;
    clock_gettime(CLOCK_MONOTONIC, &t);
    return (double)(t.tv_sec - t_start.tv_sec) * 1e3 + (double)(t.tv_nsec - t_start.tv_nsec) / 1e6;
}

/* runtime/win32's per-file I/O table (w32_file.c), if linked. */
static void io_report(void) {
    void (*rep)(FILE *) = (void (*)(FILE *))dlsym(RTLD_DEFAULT, "w32_io_report");
    if (rep) rep(stderr);
}

static void (*prev_exit)(int);
static void on_exit_hook(int code) {
    char buf[320];
    fprintf(stderr, "boot: guest exit(%d) %s WinMain after %.1f ms\n", code, past_winmain ? "after entering" : "before",
            ms_since_start());
    progress(buf, sizeof buf);
    put(buf);
    io_report();
    fflush(NULL);
    if (prev_exit) prev_exit(code);
    _exit(code);
}

/* Stop before the first window (docs/design/platform-macos.md 10.12): runtime/hle's hle_stop_check calls this at the first
 * CreateWindowExW/A or Direct3DCreate9 when SPORE_STOP_AT=window. Reports and exits 0. */
static void on_stop(sg_ctx *ctx, uint8_t *mem, const char *import) {
    void *bt[64];
    int n;
    fprintf(stderr, "\nboot: REACHED stop point %s (SPORE_STOP_AT=%s): called from guest 0x%08x, esp=0x%08x, %.1f ms after start\n",
            import, getenv("SPORE_STOP_AT"), sg_ld32(mem, ctx->esp), ctx->esp, ms_since_start());
    fflush(stderr);
    n = backtrace(bt, 64);
    put("boot: host backtrace (sg_f_XXXXXXXX frames are guest functions):\n");
    backtrace_symbols_fd(bt, n, 2);
    io_report();
    fflush(NULL);
    _exit(0);
}

void sg_boot_diag_install(void) {
    static const int sigs[] = {SIGABRT, SIGSEGV, SIGBUS, SIGILL, SIGTRAP, SIGFPE, SIGALRM};
    struct sigaction sa;
    void **hook;
    const char *w;
    ctor_done = dlsym(RTLD_DEFAULT, "w32_initterm_done");
    ctor_current = dlsym(RTLD_DEFAULT, "w32_initterm_current");
    hook = dlsym(RTLD_DEFAULT, "w32_initterm_hook");
    if (hook) *hook = (void *)on_initterm;
    hook = dlsym(RTLD_DEFAULT, "w32_exit_hook");
    if (hook) {
        prev_exit = (void (*)(int))*hook;
        *hook = (void *)on_exit_hook;
    }
    memset(&sa, 0, sizeof sa);
    sa.sa_sigaction = on_signal;
    sa.sa_flags = SA_SIGINFO | SA_ONSTACK;
    sigemptyset(&sa.sa_mask);
    for (unsigned i = 0; i < sizeof sigs / sizeof sigs[0]; i++) sigaction(sigs[i], &sa, &prev_sa[sigs[i] & 31]);
    hook = dlsym(RTLD_DEFAULT, "hle_stop_hook");
    if (hook) *hook = (void *)on_stop;
    clock_gettime(CLOCK_MONOTONIC, &t_start);
    w = getenv("SPORE_WATCHDOG");
    if (w && atoi(w) > 0) alarm((unsigned)atoi(w));
}

void sg_boot_winmain(sg_ctx *ctx, uint8_t *mem) {
    if (getenv("SPORE_STOP_AT_WINMAIN")) {
        char buf[320];
        uint32_t esp = ctx->esp, cl = sg_ld32(mem, esp + 12);
        struct timespec t;
        char line[129];
        unsigned i;
        for (i = 0; i < sizeof line - 1 && cl && mem[cl + i]; i++) line[i] = (char)mem[cl + i];
        line[i] = 0;
        clock_gettime(CLOCK_MONOTONIC, &t);
        fprintf(stderr, "\nboot: REACHED WinMain 0x%08x: hInstance=0x%08x hPrev=0x%08x lpCmdLine=0x%08x nShow=%u\n", BOOT_WINMAIN,
                sg_ld32(mem, esp + 4), sg_ld32(mem, esp + 8), cl, sg_ld32(mem, esp + 16));
        fprintf(stderr, "boot: lpCmdLine \"%s\"; entry to WinMain took %.1f ms\n", line,
                (double)(t.tv_sec - t_start.tv_sec) * 1e3 + (double)(t.tv_nsec - t_start.tv_nsec) / 1e6);
        progress(buf, sizeof buf);
        put(buf);
        fflush(NULL);
        _exit(0);
    }
    past_winmain = 1;
    sg_lookup(ctx, mem, BOOT_WINMAIN)(ctx, mem);
}
