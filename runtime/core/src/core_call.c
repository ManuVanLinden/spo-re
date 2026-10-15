/* core_call.c -- thunk region registry, host->guest calls, fatal reports, logging, faults. */
#include <signal.h>
#include <stdarg.h>
#include <unistd.h>

#include "core_internal.h"

/* ---- logging -------------------------------------------------------------------------------- */

int sg_core_log_level = 1;
static void default_sink(int level, const char *line) {
    static const char *const tag[] = {"error", "warn", "info", "trace"};
    fprintf(stderr, "[core %s] %s\n", tag[level & 3], line);
}
void (*sg_core_log_sink)(int level, const char *line) = default_sink;

void sg_core_log(int level, const char *fmt, ...) {
    char line[768];
    va_list ap;
    if (level > sg_core_log_level) return;
    va_start(ap, fmt);
    vsnprintf(line, sizeof line, fmt, ap);
    va_end(ap);
    sg_core_log_sink(level, line);
}

/* ---- thunk registry (runtime-abi 7.1) ------------------------------------------------------ */

#define NSLOTS ((SG_THUNK_LIMIT - SG_THUNK_BASE) / SG_THUNK_SLOT)
sg_func *sg_thunk_fn[NSLOTS];
static const char *thunk_names[NSLOTS];
static unsigned thunk_next = SG_THUNK_FIRST_FREE;
static pthread_mutex_t thunk_lock = PTHREAD_MUTEX_INITIALIZER;

uint32_t sg_thunk_register(sg_func *fn, const char *debug_name) {
    unsigned s;
    pthread_mutex_lock(&thunk_lock);
    if (thunk_next >= NSLOTS) {
        pthread_mutex_unlock(&thunk_lock);
        fprintf(stderr, "sg_thunk_register: thunk region full\n");
        abort();
    }
    s = thunk_next++;
    thunk_names[s] = debug_name;
    __atomic_store_n(&sg_thunk_fn[s], fn, __ATOMIC_RELEASE);
    pthread_mutex_unlock(&thunk_lock);
    return SG_THUNK_ADDR(s);
}

const char *sg_thunk_name(uint32_t a) {
    if (a - SG_THUNK_BASE >= SG_THUNK_LIMIT - SG_THUNK_BASE || (a & (SG_THUNK_SLOT - 1))) return NULL;
    return thunk_names[(a - SG_THUNK_BASE) / SG_THUNK_SLOT];
}

static void slot_ret_to_host(sg_ctx *ctx, uint8_t *mem) {
    (void)mem;
    sg_core_fatal(ctx, "guest code called SG_RET_TO_HOST (0x%08x) as a function", SG_RET_TO_HOST);
}
static void slot_thread_exit(sg_ctx *ctx, uint8_t *mem) {
    (void)mem;
    sg_core_exit_thread(ctx, ctx->eax); /* jumped to: behave like returning from the entry */
}
static void slot_unimpl(sg_ctx *ctx, uint8_t *mem) {
    sg_core_fatal(ctx, "call to SG_THUNK_UNIMPL (unresolved import) from guest 0x%08x", sg_ld32(mem, ctx->esp));
}

static void set_slot(unsigned s, sg_func *fn, const char *name) {
    sg_thunk_fn[s] = fn;
    thunk_names[s] = name;
}

void core_thunks_init(void) {
    set_slot(0, slot_ret_to_host, "SG_RET_TO_HOST");
    set_slot(1, slot_thread_exit, "SG_THREAD_EXIT");
    set_slot(2, slot_unimpl, "SG_THUNK_UNIMPL");
}

/* ---- host -> guest calls (runtime-abi 7.3) ------------------------------------------------- */

int sg_core_checks = 1;

uint32_t sg_core_call(sg_ctx *ctx, uint32_t target, enum sg_cc cc, unsigned nargs, const uint32_t *args, int mode) {
    uint8_t *mem = ctx->mem;
    uint32_t esp0 = ctx->esp, ebx = ctx->ebx, esi = ctx->esi, edi = ctx->edi, ebp = ctx->ebp;
    unsigned first = 0, k;
    uint32_t stack_bytes;
    if (cc == SG_CC_THISCALL && nargs > 0) { ctx->ecx = args[0]; first = 1; }
    if (cc == SG_CC_FASTCALL) {
        if (nargs > 0) { ctx->ecx = args[0]; first = 1; }
        if (nargs > 1) { ctx->edx = args[1]; first = 2; }
    }
    for (k = nargs; k-- > first;) {
        ctx->esp -= 4u;
        sg_st32(mem, ctx->esp, args[k]);
    }
    stack_bytes = 4u * (nargs - first);
    ctx->esp -= 4u;
    sg_st32(mem, ctx->esp, SG_RET_TO_HOST);
    sg_lookup(ctx, mem, target)(ctx, mem);
    if (SG_UNLIKELY(ctx->eip != SG_RET_TO_HOST))
        sg_core_fatal(ctx, "sg_call_guest(0x%08x): returned to 0x%08x instead of SG_RET_TO_HOST", target, ctx->eip);
    if (cc == SG_CC_CDECL || cc == SG_CC_X87) {
        if (sg_core_checks && ctx->esp != esp0 - stack_bytes)
            sg_core_fatal(ctx, "sg_call_guest(0x%08x, cdecl): callee left esp 0x%08x, expected 0x%08x", target,
                          ctx->esp, esp0 - stack_bytes);
    } else if (sg_core_checks && ctx->esp != esp0) {
        sg_core_fatal(ctx, "sg_call_guest(0x%08x, callee-cleans, %u stack bytes): esp 0x%08x, expected 0x%08x",
                      target, stack_bytes, ctx->esp, esp0);
    }
    ctx->esp = esp0;
    if (mode == SG_CORE_CALL_CHECKED && sg_core_checks &&
        (ctx->ebx != ebx || ctx->esi != esi || ctx->edi != edi || ctx->ebp != ebp))
        sg_core_fatal(ctx, "sg_call_guest(0x%08x): callee-saved register changed (ebx %08x->%08x esi %08x->%08x "
                      "edi %08x->%08x ebp %08x->%08x)", target, ebx, ctx->ebx, esi, ctx->esi, edi, ctx->edi, ebp,
                      ctx->ebp);
    return ctx->eax;
}

uint32_t sg_call_guest(sg_ctx *ctx, uint32_t target, enum sg_cc cc, unsigned nargs, const uint32_t *args) {
    return sg_core_call(ctx, target, cc, nargs, args, SG_CORE_CALL_CHECKED);
}

/* ---- fatal reports --------------------------------------------------------------------------- */

void (*sg_core_fatal_hook)(sg_ctx *ctx, const char *msg);

static int in_stack(const sg_ctx *ctx, uint32_t a, uint32_t n) {
    const sg_thread *t = ctx->thread;
    if (!t) return 0;
    return a >= t->stack_lo && a + n <= t->stack_hi && a + n > a;
}

void sg_core_backtrace(sg_ctx *ctx, int level) {
    uint32_t ebp = ctx->ebp;
    unsigned i;
    sg_core_log(level, "  eip %08x  [esp] %08x", ctx->eip, in_stack(ctx, ctx->esp, 4) ? sg_ld32(ctx->mem, ctx->esp) : 0);
    for (i = 0; i < 32 && in_stack(ctx, ebp, 8); i++) {
        uint32_t ra = sg_ld32(ctx->mem, ebp + 4), next = sg_ld32(ctx->mem, ebp);
        sg_core_log(level, "  #%-2u ebp %08x  ret %08x%s%s", i, ebp, ra, sg_thunk_name(ra) ? "  " : "",
                    sg_thunk_name(ra) ? sg_thunk_name(ra) : "");
        if (next <= ebp) break;
        ebp = next;
    }
}

void sg_core_fatal(sg_ctx *ctx, const char *fmt, ...) {
    char msg[768];
    va_list ap;
    va_start(ap, fmt);
    vsnprintf(msg, sizeof msg, fmt, ap);
    va_end(ap);
    if (sg_core_fatal_hook) sg_core_fatal_hook(ctx, msg);
    sg_core_log(0, "FATAL: %s", msg);
    if (ctx) {
        sg_core_log(0, "  thread %s tid %u", ctx->thread ? sg_core_thread_name(ctx->thread) : "?",
                    ctx->thread ? ctx->thread->tid : 0);
        sg_core_log(0, "  eax %08x ecx %08x edx %08x ebx %08x esp %08x ebp %08x esi %08x edi %08x eip %08x",
                    ctx->eax, ctx->ecx, ctx->edx, ctx->ebx, ctx->esp, ctx->ebp, ctx->esi, ctx->edi, ctx->eip);
        sg_core_backtrace(ctx, 0);
    }
    abort();
}

/* ---- ABI [runtime] entry points the generated code references ------------------------------- */

void sg_return_mismatch(sg_ctx *ctx, uint8_t *mem, uint32_t expected) {
    (void)mem;
    sg_core_fatal(ctx, "return-address check: returned to 0x%08x, caller expected 0x%08x", ctx->eip, expected);
}
void sg_misaligned_atomic(sg_ctx *ctx, uint32_t a) {
    sg_core_fatal(ctx, "misaligned locked access at guest 0x%08x", a);
}
void sg_bad_resume(sg_ctx *ctx) {
    sg_core_fatal(ctx, "landing dispatch has no label for resume address 0x%08x", ctx->eip);
}
void sg_unhandled_insn(sg_ctx *ctx, uint32_t va) {
    sg_core_fatal(ctx, "untranslated instruction at guest 0x%08x", va);
}
void sg_int3(sg_ctx *ctx, uint32_t va) {
    sg_core_fatal(ctx, "int3 reached at guest 0x%08x", va);
}
void sg_divide_error(sg_ctx *ctx, uint32_t va) {
    /* Hardware faults are reported and abort; no guest SEH conversion. */
    sg_core_fatal(ctx, "integer divide error (#DE) at guest 0x%08x", va);
}

/* ---- host faults ------------------------------------------------------------------------------ */

static void fault_handler(int sig, siginfo_t *si, void *uc) {
    char buf[256];
    uintptr_t a = (uintptr_t)si->si_addr, b = (uintptr_t)sg_core_mem;
    sg_ctx *ctx = sg_self ? &sg_self->ctx : NULL;
    int n;
    (void)uc;
    if (b && a >= b && a < b + SG_WINDOW_SIZE + SG_WINDOW_GUARD)
        n = snprintf(buf, sizeof buf, "\nspore-native: signal %d at guest address 0x%08llx (thread %s, last eip 0x%08x)\n",
                     sig, (unsigned long long)(a - b), ctx ? sg_core_thread_name(ctx->thread) : "host",
                     ctx ? ctx->eip : 0);
    else
        n = snprintf(buf, sizeof buf, "\nspore-native: signal %d at host address %p (thread %s, last eip 0x%08x)\n", sig,
                     si->si_addr, ctx ? sg_core_thread_name(ctx->thread) : "host", ctx ? ctx->eip : 0);
    if (n > 0) (void)!write(2, buf, (size_t)n);
    if (ctx) sg_core_backtrace(ctx, 0); /* not async-signal-safe, but we abort right after */
    signal(SIGABRT, SIG_DFL);
    abort();
}

int core_fault_handlers_installed;

void sg_core_install_fault_handlers(void) {
    struct sigaction sa;
    stack_t ss;
    core_fault_handlers_installed = 1;
    ss.ss_sp = malloc(65536); /* this thread's alternate stack; guest threads get their own */
    ss.ss_size = 65536;
    ss.ss_flags = 0;
    if (ss.ss_sp) sigaltstack(&ss, NULL);
    memset(&sa, 0, sizeof sa);
    sa.sa_sigaction = fault_handler;
    sa.sa_flags = SA_SIGINFO | SA_ONSTACK;
    sigemptyset(&sa.sa_mask);
    sigaction(SIGBUS, &sa, NULL);
    sigaction(SIGSEGV, &sa, NULL);
    sigaction(SIGILL, &sa, NULL);
    sigaction(SIGFPE, &sa, NULL);
}
