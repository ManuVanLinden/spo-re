/* hle_core.c -- import installation, name resolution, logging, policies, per-thread CRT state. */
#include <stdio.h>
#include <stdlib.h>
#include <string.h>
#include <strings.h>

#include "hle_imports.h"

int hle_log_level = HLE_LOG_WARN;
int hle_unimpl_policy = HLE_UNIMPL_ABORT;

static void default_sink(int level, const char *line) {
    static const char *const tag[] = {"error", "warn", "info", "trace"};
    fprintf(stderr, "[hle %s] %s\n", tag[level & 3], line);
}
void (*hle_log_sink)(int level, const char *line) = default_sink;

void hle_log(int level, const char *fmt, ...) {
    char line[512];
    va_list ap;
    if (level > hle_log_level) return;
    va_start(ap, fmt);
    vsnprintf(line, sizeof line, fmt, ap);
    va_end(ap);
    hle_log_sink(level, line);
}

/* Environment overrides, read once (constructor: before any shim can run). */
__attribute__((constructor)) static void hle_env(void) {
    const char *s = getenv("SPORE_HLE_UNIMPL");
    if (s && !strcmp(s, "continue")) hle_unimpl_policy = HLE_UNIMPL_CONTINUE;
    s = getenv("SPORE_HLE_LOG");
    if (s) hle_log_level = atoi(s);
}

/* ---- unimplemented / invalid parameter ------------------------------------------------- */

static void default_result(sg_ctx *ctx, unsigned ret) {
    switch (ret) {
    case HLE_RET_I64: ctx->eax = ctx->edx = 0; break;
    case HLE_RET_F64: sg_fpu_push(ctx, 0.0); break;
    case HLE_RET_X87_UNARY: break;                      /* leave ST(0) as is */
    case HLE_RET_X87_BINARY: (void)sg_fpu_pop(ctx); break; /* ST(1) op ST(0): pop one */
    default: ctx->eax = 0; break;
    }
}

void hle_unimplemented(sg_ctx *ctx, uint8_t *mem, unsigned i) {
    static unsigned char seen[1024];
    const sg_import *imp = &hle_imports[i];
    uint32_t ra = sg_ld32(mem, ctx->esp);
    if (i < sizeof seen && !__atomic_exchange_n(&seen[i], 1, __ATOMIC_RELAXED))
        hle_log(HLE_LOG_ERROR, "unimplemented: %s!%s%s (called from guest 0x%08x)", imp->dll,
                imp->name ? imp->name : "#", imp->name ? "" : "ordinal", ra);
    if (hle_unimpl_policy == HLE_UNIMPL_ABORT) {
        fprintf(stderr, "hle: aborting on unimplemented import %s (set SPORE_HLE_UNIMPL=continue)\n",
                hle_import_info[i].symbol);
        abort();
    }
    default_result(ctx, hle_import_info[i].ret);
}

void hle_unimplemented_dynamic(sg_ctx *ctx, uint8_t *mem, unsigned i) {
    static unsigned char seen[256];
    const hle_dynamic *d = &hle_dynamics[i];
    if (i < sizeof seen && !__atomic_exchange_n(&seen[i], 1, __ATOMIC_RELAXED))
        hle_log(HLE_LOG_ERROR, "unimplemented: %s!%s (dynamic; called from guest 0x%08x)", d->module, d->name,
                sg_ld32(mem, ctx->esp));
    if (hle_unimpl_policy == HLE_UNIMPL_ABORT) abort();
    default_result(ctx, d->ret);
}

void hle_invalid_parameter(sg_ctx *ctx, const char *func, const char *what) {
    (void)ctx;
    hle_log(HLE_LOG_ERROR, "invalid parameter in %s: %s (msvcr90 would call _invoke_watson)", func, what);
    if (hle_unimpl_policy == HLE_UNIMPL_ABORT) abort();
}

/* ---- loader and GetProcAddress back end ------------------------------------------------ */

static uint32_t acmdln_va, adjust_fdiv_va;

unsigned hle_install_imports(uint8_t *mem) {
    unsigned i, n = 0;
    for (i = 0; i < hle_import_count; i++) {
        sg_import *imp = &hle_imports[i];
        if (imp->cc == HLE_CC_DATA) {
            /* Data imports: the IAT slot holds the variable's address. */
            if (imp->name && !strcmp(imp->name, "_acmdln")) {
                if (!acmdln_va) acmdln_va = hle_guest_alloc(4, 4); /* char *_acmdln, set by the startup layer */
                imp->guest = acmdln_va;
            } else {
                if (!adjust_fdiv_va) adjust_fdiv_va = hle_guest_alloc(4, 4); /* int _adjust_fdiv = 0 (no FDIV bug) */
                imp->guest = adjust_fdiv_va;
            }
        } else {
            imp->guest = sg_thunk_register(imp->fn, hle_import_info[i].symbol);
        }
        sg_st32(mem, imp->iat, imp->guest);
        n++;
    }
    return n;
}

/* "KERNEL32.dll", "kernel32", "Kernel32.DLL" -> "kernel32" */
static void mod_stem(const char *m, char *out, size_t cap) {
    size_t i, n;
    const char *slash = strrchr(m, '\\');
    if (slash) m = slash + 1;
    n = strlen(m);
    if (n > 4 && !strcasecmp(m + n - 4, ".dll")) n -= 4;
    for (i = 0; i < n && i + 1 < cap; i++) out[i] = (char)(m[i] >= 'A' && m[i] <= 'Z' ? m[i] + 32 : m[i]);
    out[i] = 0;
    /* xinput: the game tries xinput9_1_0 then xinput1_1; both resolve to the same shims. */
    if (!strcmp(out, "xinput1_1") || !strcmp(out, "xinput1_3")) strcpy(out, "xinput9_1_0");
}

int hle_module_known(const char *module) {
    char want[64], have[64];
    unsigned i;
    mod_stem(module, want, sizeof want);
    for (i = 0; i < hle_import_count; i++) {
        mod_stem(hle_imports[i].dll, have, sizeof have);
        if (!strcmp(want, have)) return 1;
    }
    for (i = 0; i < hle_dynamic_count; i++) {
        mod_stem(hle_dynamics[i].module, have, sizeof have);
        if (!strcmp(want, have) && hle_dynamics[i].fn) return 1;
    }
    return 0;
}

uint32_t hle_resolve(const char *module, const char *name, unsigned ordinal) {
    char want[64], have[64];
    unsigned i;
    mod_stem(module, want, sizeof want);
    if (name) {
        for (i = 0; i < hle_dynamic_count; i++) {
            hle_dynamic *d = &hle_dynamics[i];
            mod_stem(d->module, have, sizeof have);
            if (strcmp(want, have) || strcmp(name, d->name)) continue;
            if (!d->fn) {
                hle_log(HLE_LOG_INFO, "GetProcAddress(%s, %s): deliberately absent (%s)", module, name, d->reason);
                return 0;
            }
            if (!d->guest) {
                /* a name that is also a static import shares that import's thunk */
                unsigned k;
                for (k = 0; k < hle_import_count; k++)
                    if (hle_imports[k].fn == d->fn) {
                        if (!hle_imports[k].guest) hle_imports[k].guest = sg_thunk_register(d->fn, hle_import_info[k].symbol);
                        d->guest = hle_imports[k].guest;
                        break;
                    }
                if (!d->guest) d->guest = sg_thunk_register(d->fn, d->name);
            }
            return d->guest;
        }
    }
    for (i = 0; i < hle_import_count; i++) {
        sg_import *imp = &hle_imports[i];
        if (imp->cc == HLE_CC_DATA) continue;
        mod_stem(imp->dll, have, sizeof have);
        if (strcmp(want, have)) continue;
        if (name ? (imp->name && !strcmp(name, imp->name)) : (imp->ordinal == ordinal && ordinal)) {
            if (!imp->guest) imp->guest = sg_thunk_register(imp->fn, hle_import_info[i].symbol);
            return imp->guest;
        }
    }
    hle_log(HLE_LOG_WARN, "GetProcAddress(%s, %s%u): unknown name, returning NULL", module,
            name ? name : "#", name ? 0u : ordinal);
    return 0;
}

/* ---- guest strings ------------------------------------------------------------------- */

uint32_t hle_strlen(const uint8_t *mem, uint32_t a) {
    uint32_t n = 0;
    while (mem[(uint32_t)(a + n)]) n++;
    return n;
}
uint32_t hle_wcslen(const uint8_t *mem, uint32_t a) {
    uint32_t n = 0;
    while (sg_ld16(mem, a + 2u * n)) n++;
    return n;
}
size_t hle_get_str(const uint8_t *mem, uint32_t a, char *dst, size_t cap) {
    size_t i = 0;
    if (!cap) return 0;
    for (; i + 1 < cap && mem[(uint32_t)(a + i)]; i++) dst[i] = (char)mem[(uint32_t)(a + i)];
    dst[i] = 0;
    return i;
}

/* ---- output buffer ------------------------------------------------------------------ */

void hle_obuf_put(hle_obuf *o, unsigned unit) {
    size_t w = o->wide ? 2 : 1;
    if (o->n == o->cap) {
        size_t nc = o->cap ? o->cap * 2 : 256;
        uint8_t *np = (uint8_t *)realloc(o->p, nc * w);
        if (!np) abort();
        o->p = np;
        o->cap = nc;
    }
    if (o->wide) {
        uint16_t u = (uint16_t)unit;
        memcpy(o->p + 2 * o->n, &u, 2);
    } else {
        o->p[o->n] = (uint8_t)unit;
    }
    o->n++;
}
void hle_obuf_free(hle_obuf *o) { free(o->p); o->p = 0; o->n = o->cap = 0; }

/* ---- per-thread CRT state ----------------------------------------------------------- */

#define HLE_CVTBUFSIZE 352u /* msvcr90 _CVTBUFSIZE is 309 + 40 = 349; rounded up */

static _Thread_local hle_tls tls_state;

hle_tls *hle_thread(void) {
    hle_tls *t = &tls_state;
    if (!t->block) {
        uint32_t b = hle_guest_alloc(16 + 48 + HLE_CVTBUFSIZE, 16);
        if (!b) {
            fprintf(stderr, "hle: guest allocation for thread CRT state failed\n");
            abort();
        }
        t->block = b;
        t->errno_va = b;
        t->doserrno_va = b + 4;
        t->tm_va = b + 16;
        t->cvt_va = b + 64;
        t->rand_seed = 1; /* msvcr90: _tiddata._holdrand starts at 1 on every thread */
    }
    return t;
}
void hle_set_errno(uint8_t *mem, int e) { sg_st32(mem, hle_thread()->errno_va, (uint32_t)e); }
int hle_get_errno(const uint8_t *mem) { return (int)sg_ld32(mem, hle_thread()->errno_va); }
