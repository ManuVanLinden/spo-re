/* w32_crt.c -- the msvcr90 start-up and shut-down imports the CRT entry 0x11e11e1 uses
 * (design doc section 4).
 *
 *   pre_c_init 0x11e1100: __set_app_type(2), _encode_pointer(-1) into __onexitbegin/end,
 *                          *__p__fmode() = _fmode, *__p__commode() = _commode,
 *                          reads _adjust_fdiv through its IAT slot, __setusermatherr(0x11e1585)
 *   _initterm_e(0x13eb09c, 0x13eb0ac), _initterm(0x13cc80c, 0x13eb098)
 *   __getmainargs(&argc 0x16f261c, &argv 0x16f2624, &envp 0x16f2620, _dowildcard, &startinfo)
 *   atexit 0x11e078a: __onexitbegin decodes to -1 in an exe, so it calls msvcr90's _onexit;
 *                          the __dllonexit branch is for DLLs [obs: 0x11e07a8, 0x11e116c]
 *   exit / _cexit after WinMain returns (0x11e1070, 0x11e10d3)
 * _amsg_exit, _encode_pointer/_decode_pointer, _controlfp_s, _configthreadlocale and
 * _crt_debugger_hook are runtime/hle's and are not replaced here.
 */
#include <pthread.h>
#include <stdio.h>
#include <stdlib.h>
#include <string.h>

#include "w32_int.h"

volatile uint32_t w32_initterm_done, w32_initterm_current;
void (*w32_initterm_hook)(uint32_t index, uint32_t fn);

static void default_exit(int code) {
    fflush(NULL);
    exit(code);
}
void (*w32_exit_hook)(int code) = default_exit;

static uint32_t fmode_va, commode_va, app_type, usermatherr;
static pthread_mutex_t onexit_lock = PTHREAD_MUTEX_INITIALIZER;
static uint32_t *onexit_tab;
static unsigned onexit_n, onexit_cap;
#define NLOCKS 64
static pthread_mutex_t crt_locks[NLOCKS];
static pthread_once_t locks_once = PTHREAD_ONCE_INIT;

static void init_locks(void) {
    pthread_mutexattr_t a;
    pthread_mutexattr_init(&a);
    pthread_mutexattr_settype(&a, PTHREAD_MUTEX_RECURSIVE);
    for (int i = 0; i < NLOCKS; i++) pthread_mutex_init(&crt_locks[i], &a);
    pthread_mutexattr_destroy(&a);
}

void w32_crt_init(void) {
    fmode_va = hle_guest_alloc(4, 4);    /* int _fmode = 0 (text unless the program says) */
    commode_va = hle_guest_alloc(4, 4);  /* int _commode = 0 */
    pthread_mutex_lock(&onexit_lock);
    free(onexit_tab);
    onexit_tab = NULL;
    onexit_n = onexit_cap = 0;
    pthread_mutex_unlock(&onexit_lock);
    w32_initterm_done = w32_initterm_current = 0;
    pthread_once(&locks_once, init_locks);
}

uint32_t w32_crt_fmode(void) { return fmode_va ? sg_ld32(w32_mem, fmode_va) : 0; }

/* ---- start-up ---------------------------------------------------------------------- */

W32_IMPORT(msvcr90, __set_app_type) { app_type = SG_ARG(0); SG_RET(0); }
W32_IMPORT(msvcr90, __p__fmode) { SG_RETURN_CDECL(fmode_va); }
W32_IMPORT(msvcr90, __p__commode) { SG_RETURN_CDECL(commode_va); }
W32_IMPORT(msvcr90, __setusermatherr) { usermatherr = SG_ARG(0); SG_RET(0); } /* never called back: our libm shims do not raise _matherr */

static uint32_t run_table(sg_ctx *ctx, uint8_t *mem, uint32_t b, uint32_t e, int check) {
    hle_log(HLE_LOG_INFO, "_initterm%s(0x%08x, 0x%08x): %u slots", check ? "_e" : "", b, e, (e - b) / 4);
    for (uint32_t p = b; p < e; p += 4) {
        uint32_t fn = sg_ld32(mem, p);
        if (!fn) continue;
        w32_initterm_current = fn;
        if (w32_initterm_hook) w32_initterm_hook(w32_initterm_done, fn);
        uint32_t r = sg_call_guest(ctx, fn, SG_CC_CDECL, 0, NULL);
        w32_initterm_done++;
        if (check && r) {
            hle_log(HLE_LOG_ERROR, "_initterm_e: initializer 0x%08x returned %u", fn, r);
            w32_initterm_current = 0;
            return r;
        }
    }
    w32_initterm_current = 0;
    return 0;
}
W32_IMPORT(msvcr90, _initterm) {
    uint32_t b = SG_ARG(0), e = SG_ARG(1);
    run_table(ctx, mem, b, e, 0);
    SG_RET(0);
}
W32_IMPORT(msvcr90, _initterm_e) {
    uint32_t b = SG_ARG(0), e = SG_ARG(1);
    SG_RETURN_CDECL(run_table(ctx, mem, b, e, 1));
}

/* msvcr90 parse_cmdline (stdargv.c): the program name ends at the closing quote (no escapes);
 * in arguments 2n backslashes + '"' give n backslashes and toggle quoting, 2n+1 give n and a
 * literal '"', and "" inside quotes is a literal '"'. Writes NUL-separated arguments into
 * out (if not NULL) and returns the count; *chars receives the bytes needed. */
static unsigned parse_cmdline(const char *p, char *out, size_t *chars) {
    unsigned argc = 0;
    size_t n = 0;
    int inquote = 0;
#define EMIT(c) do { char c_ = (char)(c); if (out) out[n] = c_; n++; } while (0)
    argc++;
    if (*p == '"') {
        p++;
        while (*p && *p != '"') EMIT(*p++);
        if (*p == '"') p++;
    } else {
        while (*p && *p != ' ' && *p != '\t') EMIT(*p++);
    }
    EMIT(0);
    for (;;) {
        while (*p == ' ' || *p == '\t') p++;
        if (!*p) break;
        argc++;
        for (;;) {
            int copy = 1;
            unsigned slashes = 0;
            while (*p == '\\') { p++; slashes++; }
            if (*p == '"') {
                if (slashes % 2 == 0) {
                    if (inquote && p[1] == '"') p++;
                    else { copy = 0; inquote = !inquote; }
                }
                slashes /= 2;
            }
            while (slashes--) EMIT('\\');
            if (!*p || (!inquote && (*p == ' ' || *p == '\t'))) break;
            if (copy) EMIT(*p);
            p++;
        }
        EMIT(0);
    }
#undef EMIT
    *chars = n;
    return argc;
}

/* Builds a guest char*[] + strings block (one allocation, as msvcr90 does). */
static uint32_t build_vector(uint8_t *mem, unsigned count, const char *strings, size_t chars) {
    uint32_t block = w32_malloc((count + 1) * 4u + (uint32_t)chars);
    if (!block) return 0;
    uint32_t s = block + (count + 1) * 4u;
    memcpy(mem + s, strings, chars);
    for (unsigned i = 0; i < count; i++) {
        sg_st32(mem, block + 4 * i, s);
        s += (uint32_t)strlen(strings) + 1;
        strings += strlen(strings) + 1;
    }
    sg_st32(mem, block + 4 * count, 0);
    return block;
}

/* Command line in code page 1252 (the "A" view). */
static void ansi_cmdline(char *out, size_t cap) {
    const char *s = w32_command_line();
    size_t n = 0;
    uint32_t cp;
    while ((cp = w32_utf8_decode(&s)) && n + 1 < cap) out[n++] = (char)w32_unicode_to_cp1252(cp);
    out[n] = 0;
}

int w32_getmainargs(uint8_t *mem, uint32_t *argc_out, uint32_t *argv_out, uint32_t *envp_out) {
    char cmd[8192];
    size_t chars;
    ansi_cmdline(cmd, sizeof cmd);
    unsigned argc = parse_cmdline(cmd, NULL, &chars);
    char *buf = (char *)malloc(chars + 1);
    parse_cmdline(cmd, buf, &chars);
    uint32_t argv = build_vector(mem, argc, buf, chars);
    free(buf);
    /* environment: NAME=VALUE strings from the w32 environment block */
    unsigned envc = 0;
    size_t need = w32_env_block(NULL, 0, &envc);
    char *eb = (char *)malloc(need + 1);
    w32_env_block(eb, need + 1, &envc);
    /* the block is UTF-8; convert each string to 1252 in place (never longer) */
    {
        size_t o = 0;
        const char *s = eb;
        while ((size_t)(s - eb) < need) {
            if (!*s) { eb[o++] = 0; s++; continue; }
            uint32_t cp = w32_utf8_decode(&s);
            eb[o++] = (char)w32_unicode_to_cp1252(cp);
        }
        need = o;
    }
    uint32_t envp = build_vector(mem, envc, eb, need);
    free(eb);
    if (!argv || !envp) return -1;
    *argc_out = argc;
    *argv_out = argv;
    *envp_out = envp;
    return 0;
}

W32_IMPORT(msvcr90, __getmainargs) {
    /* (int *argc, char ***argv, char ***envp, int dowildcard, _startupinfo *) */
    uint32_t pargc = SG_ARG(0), pargv = SG_ARG(1), penvp = SG_ARG(2), argc, argv, envp;
    if (SG_ARG(3)) hle_log(HLE_LOG_WARN, "__getmainargs: wildcard expansion requested; not done");
    if (w32_getmainargs(mem, &argc, &argv, &envp)) SG_RETURN_CDECL(-1);
    sg_st32(mem, pargc, argc);
    sg_st32(mem, pargv, argv);
    sg_st32(mem, penvp, envp);
    hle_log(HLE_LOG_INFO, "__getmainargs: argc=%u", argc);
    SG_RETURN_CDECL(0);
}

/* ---- atexit / _onexit ------------------------------------------------------------------- */

uint32_t w32_onexit_add(uint32_t fn) {
    pthread_mutex_lock(&onexit_lock);
    if (onexit_n == onexit_cap) {
        unsigned nc = onexit_cap ? onexit_cap * 2 : 256;
        uint32_t *nt = (uint32_t *)realloc(onexit_tab, nc * 4);
        if (!nt) { pthread_mutex_unlock(&onexit_lock); return 0; }
        onexit_tab = nt;
        onexit_cap = nc;
    }
    onexit_tab[onexit_n++] = fn;
    pthread_mutex_unlock(&onexit_lock);
    return fn;
}
unsigned w32_onexit_count(void) { return onexit_n; }

W32_IMPORT(msvcr90, _onexit) { SG_RETURN_CDECL(w32_onexit_add(SG_ARG(0))); }
W32_IMPORT(msvcr90, atexit) { SG_RETURN_CDECL(w32_onexit_add(SG_ARG(0)) ? 0 : -1); } /* not imported by this exe */

/* __dllonexit(func, &begin, &end): a module-private table in guest memory, grown with
 * realloc; begin/end hold _encode_pointer'ed values (identity in runtime/hle's shims). */
W32_IMPORT(msvcr90, __dllonexit) {
    uint32_t fn = SG_ARG(0), pb = SG_ARG(1), pe = SG_ARG(2);
    uint32_t b = sg_ld32(mem, pb), e = sg_ld32(mem, pe);
    if (!b || b == 0xffffffffu) {
        b = w32_malloc(32 * 4);
        if (!b) SG_RETURN_CDECL(0);
        e = b;
    }
    if (e < b) SG_RETURN_CDECL(0);
    uint32_t used = e - b, size = w32_msize(b);
    if (size < used + 4) {
        uint32_t grow = size > 512 * 4 ? 512 * 4 : size;
        uint32_t nb = w32_realloc(b, size + (grow ? grow : 16));
        if (!nb) nb = w32_realloc(b, size + 16);
        if (!nb) SG_RETURN_CDECL(0);
        b = nb;
        e = nb + used;
    }
    sg_st32(mem, e, fn);
    e += 4;
    sg_st32(mem, pb, b);
    sg_st32(mem, pe, e);
    SG_RETURN_CDECL(fn);
}

static void run_onexit(sg_ctx *ctx) {
    for (;;) {
        uint32_t fn;
        pthread_mutex_lock(&onexit_lock);
        if (!onexit_n) { pthread_mutex_unlock(&onexit_lock); break; }
        fn = onexit_tab[--onexit_n];
        pthread_mutex_unlock(&onexit_lock);
        if (fn) sg_call_guest(ctx, fn, SG_CC_CDECL, 0, NULL); /* handlers may register more */
    }
}

extern void w32_stdio_flush_all(void);

static SG_NORETURN void finish(int code) {
    w32_exit_hook(code);
    fprintf(stderr, "w32: exit hook returned; aborting\n");
    abort();
}

W32_IMPORT(msvcr90, exit) {
    int code = (int)SG_ARG(0);
    hle_log(HLE_LOG_INFO, "exit(%d): %u atexit handlers", code, onexit_n);
    run_onexit(ctx);
    w32_stdio_flush_all();
    finish(code);
}
W32_IMPORT(msvcr90, _exit) {
    hle_log(HLE_LOG_INFO, "_exit(%d)", (int)SG_ARG(0));
    w32_stdio_flush_all();
    finish((int)SG_ARG(0));
}
W32_IMPORT(msvcr90, _cexit) {
    run_onexit(ctx);
    w32_stdio_flush_all();
    SG_RET(0);
}

/* ---- _lock / _unlock ------------------------------------------------------------------- */

W32_IMPORT(msvcr90, _lock) {
    uint32_t n = SG_ARG(0);
    pthread_once(&locks_once, init_locks);
    if (n < NLOCKS) pthread_mutex_lock(&crt_locks[n]);
    else hle_log(HLE_LOG_ERROR, "_lock(%u): out of range", n);
    SG_RET(0);
}
W32_IMPORT(msvcr90, _unlock) {
    uint32_t n = SG_ARG(0);
    if (n < NLOCKS) pthread_mutex_unlock(&crt_locks[n]);
    SG_RET(0);
}
