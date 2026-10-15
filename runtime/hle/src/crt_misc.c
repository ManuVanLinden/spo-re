/* crt_misc.c -- msvcr90 qsort (calls a guest comparator).
 *
 * qsort is not stable, so the order of equal elements depends on the algorithm. This is the
 * msvcr80/90 algorithm as published with the CRT sources (non-recursive quicksort with an
 * explicit stack, median of three, CUTOFF 8, and a selection "shortsort" below it), written
 * from its published description [inf: not yet checked against the binary; the game's 7
 * qsort uses would only notice with comparators that report ties].
 */
#include <stdio.h>
#include <stdlib.h>
#include <string.h>

#include "hle_imports.h"

#define A(i) SG_ARG(i)
#define CUTOFF 8

typedef struct {
    sg_ctx *ctx;
    uint8_t *mem;
    uint32_t comp;
} qs;

static int cmp(qs *q, uint32_t a, uint32_t b) {
    uint32_t args[2] = {a, b};
    return (int32_t)sg_call_guest(q->ctx, q->comp, SG_CC_CDECL, 2, args);
}
static void swap(qs *q, uint32_t a, uint32_t b, uint32_t w) {
    uint8_t *m = q->mem;
    if (a == b) return;
    while (w--) { uint8_t t = m[a]; m[a++] = m[b]; m[b++] = t; }
}
static void shortsort(qs *q, uint32_t lo, uint32_t hi, uint32_t w) {
    while (hi > lo) {
        uint32_t max = lo, p;
        for (p = lo + w; p <= hi; p += w)
            if (cmp(q, p, max) > 0) max = p;
        swap(q, max, hi, w);
        hi -= w;
    }
}

void hle_qsort(sg_ctx *ctx, uint8_t *mem, uint32_t base, uint32_t num, uint32_t w, uint32_t comp) {
    qs q = {ctx, mem, comp};
    uint32_t lo, hi, mid, loguy, higuy, size, lostk[64], histk[64];
    int sp = 0;
    if (num < 2 || !w) return;
    lo = base;
    hi = base + w * (num - 1);
recurse:
    size = (hi - lo) / w + 1;
    if (size <= CUTOFF) {
        shortsort(&q, lo, hi, w);
    } else {
        mid = lo + (size / 2) * w;
        if (cmp(&q, lo, mid) > 0) swap(&q, lo, mid, w);
        if (cmp(&q, lo, hi) > 0) swap(&q, lo, hi, w);
        if (cmp(&q, mid, hi) > 0) swap(&q, mid, hi, w);
        loguy = lo;
        higuy = hi;
        for (;;) {
            if (mid > loguy) {
                do { loguy += w; } while (loguy < mid && cmp(&q, loguy, mid) <= 0);
            }
            if (mid <= loguy) {
                do { loguy += w; } while (loguy <= hi && cmp(&q, loguy, mid) <= 0);
            }
            do { higuy -= w; } while (higuy > mid && cmp(&q, higuy, mid) > 0);
            if (higuy < loguy) break;
            swap(&q, loguy, higuy, w);
            if (mid == higuy) mid = loguy;
        }
        higuy += w;
        if (mid < higuy) {
            do { higuy -= w; } while (higuy > mid && cmp(&q, higuy, mid) == 0);
        }
        if (mid >= higuy) {
            do { higuy -= w; } while (higuy > lo && cmp(&q, higuy, mid) == 0);
        }
        if (higuy - lo >= hi - loguy) {
            if (lo < higuy) { lostk[sp] = lo; histk[sp] = higuy; ++sp; }
            if (loguy < hi) { lo = loguy; goto recurse; }
        } else {
            if (loguy < hi) { lostk[sp] = loguy; histk[sp] = hi; ++sp; }
            if (lo < higuy) { hi = higuy; goto recurse; }
        }
    }
    if (--sp >= 0) {
        lo = lostk[sp];
        hi = histk[sp];
        goto recurse;
    }
}

/* void qsort(void *base, size_t num, size_t width, int (__cdecl *comp)(const void *, const void *)) */
SG_IMPORT(msvcr90, qsort) {
    uint32_t base = A(0), num = A(1), w = A(2), comp = A(3);
    uint32_t esp = ctx->esp;
    if ((!base && num) || (num && !w) || !comp) {
        hle_invalid_parameter(ctx, "qsort", "NULL base/comparator or zero width");
        hle_set_errno(mem, MSVC_EINVAL);
        SG_RET(0);
    }
    hle_qsort(ctx, mem, base, num, w, comp);
    ctx->esp = esp; /* sg_call_guest restores esp; keep the shim frame exact regardless */
    SG_RET(0);
}

/* ---- locale queries ---------------------------------------------------------------------
 * [obs] The game calls setlocale for LC_COLLATE, LC_MONETARY, LC_NUMERIC and LC_TIME (1, 3, 4,
 * 5) with one name built at 0x87fa90, treats NULL as failure, then calls _setmbcp(-4
 * = _MB_CP_LOCALE) at 0x87fb3f; it never sets LC_CTYPE. We accept and report the names but
 * keep the "C" rules, which match an English locale's (decimal point '.', English month
 * names). A non-English user locale would differ: a recorded deviation (hle-crt.md). */
static uint32_t locale_va[6];
static char locale_host[6][128] = {"C", "C", "C", "C", "C", "C"};

static uint32_t guest_copy(uint8_t *mem, uint32_t *slot, const char *s) {
    size_t n = strlen(s);
    if (n > 127) n = 127;
    if (!*slot) *slot = hle_guest_alloc(128, 4);
    memcpy(mem + *slot, s, n);
    mem[*slot + (uint32_t)n] = 0;
    return *slot;
}

/* char *setlocale(int category, const char *locale): categories 0 (LC_ALL) .. 5. */
SG_IMPORT(msvcr90, setlocale) {
    int32_t cat = (int32_t)A(0);
    uint32_t loc = A(1);
    char name[128];
    int i;
    if (cat < 0 || cat > 5) { hle_invalid_parameter(ctx, "setlocale", "bad category"); SG_RETURN_CDECL(0); }
    if (loc) {
        hle_get_str(mem, loc, name, sizeof name);
        if (!name[0]) snprintf(name, sizeof name, "English_United States.1252"); /* "": the user default */
        for (i = cat ? cat : 0; i <= (cat ? cat : 5); i++) snprintf(locale_host[i], sizeof locale_host[i], "%s", name);
        hle_log(HLE_LOG_INFO, "setlocale(%d, \"%s\"): accepted; formatting keeps the C rules", cat, name);
    }
    if (cat == 0) {
        int same = 1;
        for (i = 2; i <= 5; i++) same &= !strcmp(locale_host[1], locale_host[i]);
        if (!same) {
            snprintf(name, sizeof name, "LC_COLLATE=%s;LC_CTYPE=%s;LC_MONETARY=%s;LC_NUMERIC=%s;LC_TIME=%s",
                     locale_host[1], locale_host[2], locale_host[3], locale_host[4], locale_host[5]);
            SG_RETURN_CDECL(guest_copy(mem, &locale_va[0], name));
        }
        SG_RETURN_CDECL(guest_copy(mem, &locale_va[0], locale_host[1]));
    }
    SG_RETURN_CDECL(guest_copy(mem, &locale_va[cat], locale_host[cat]));
}
/* int _setmbcp(int cp): 0 on success. With code page 1252 there are no lead bytes. */
SG_IMPORT(msvcr90, _setmbcp) { SG_RETURN_CDECL(0); }
SG_IMPORT(msvcr90, _ismbblead) { SG_RETURN_CDECL(0); }
/* int _configthreadlocale(int type): returns the previous setting (_DISABLE_PER_THREAD_LOCALE = 2
 * initially); 0 queries; -1 is _ENABLE_PER_THREAD_LOCALE_GLOBAL... per the CRT reference. */
SG_IMPORT(msvcr90, _configthreadlocale) {
    static int32_t mode = 2;
    int32_t t = (int32_t)A(0), prev = mode;
    if (t == 1 || t == 2) mode = t;
    else if (t != 0 && t != -1) { hle_invalid_parameter(ctx, "_configthreadlocale", "bad type"); SG_RETURN_CDECL(-1); }
    SG_RETURN_CDECL(prev);
}

/* getenv: the game reads JPEGMEM (libjpeg, 0x863b82) and a few OpenSSL variables; none is set
 * on a normal Windows install, so every lookup returns NULL [obs for JPEGMEM; inf for the rest]. */
SG_IMPORT(msvcr90, getenv) {
    char name[128];
    hle_get_str(mem, A(0), name, sizeof name);
    hle_log(HLE_LOG_INFO, "getenv(\"%s\") -> NULL", name);
    SG_RETURN_CDECL(0);
}

/* ---- fatal paths: report and stop ------------------------------------------------------- */

static void fatal(sg_ctx *ctx, uint8_t *mem, const char *what) {
    hle_log(HLE_LOG_ERROR, "guest called %s (return address 0x%08x)", what, sg_ld32(mem, ctx->esp));
    fprintf(stderr, "hle: guest called %s\n", what);
    abort();
}
SG_IMPORT(msvcr90, _purecall) { fatal(ctx, mem, "_purecall (R6025 pure virtual function call)"); SG_RET(0); }
SG_IMPORT(msvcr90, abort) { fatal(ctx, mem, "abort"); SG_RET(0); }
SG_IMPORT(msvcr90, _invoke_watson) { fatal(ctx, mem, "_invoke_watson (invalid parameter)"); SG_RET(0); }
SG_IMPORT(msvcr90, _amsg_exit) {
    char m[64];
    snprintf(m, sizeof m, "_amsg_exit(%u) (runtime error R60%02u)", A(0), A(0));
    fatal(ctx, mem, m);
    SG_RET(0);
}
SG_IMPORT(msvcr90, _crt_debugger_hook) { SG_RET(0); }

/* ---- kernel32: last error and interlocked operations (pure memory) ---------------------- */

SG_IMPORT(kernel32, GetLastError) { SG_RETURN_STDCALL(hle_get_last_error(ctx, mem), 0); }
SG_IMPORT(kernel32, SetLastError) { hle_set_last_error(ctx, mem, A(0)); SG_RET(4); }
/* Interlocked*: full barriers (x86 lock prefix); the target must be 4-aligned on Windows too. */
SG_IMPORT(kernel32, InterlockedIncrement) { SG_RETURN_STDCALL(sg_lock_xadd32(ctx, mem, A(0), 1) + 1, 4); }
SG_IMPORT(kernel32, InterlockedDecrement) { SG_RETURN_STDCALL(sg_lock_xadd32(ctx, mem, A(0), (uint32_t)-1) - 1, 4); }
SG_IMPORT(kernel32, InterlockedExchange) { SG_RETURN_STDCALL(sg_xchg32(ctx, mem, A(0), A(1)), 8); }
SG_IMPORT(kernel32, InterlockedExchangeAdd) { SG_RETURN_STDCALL(sg_lock_xadd32(ctx, mem, A(0), A(1)), 8); }
/* LONG InterlockedCompareExchange(LONG volatile *dst, LONG exchange, LONG comparand) */
SG_IMPORT(kernel32, InterlockedCompareExchange) {
    uint32_t expected = A(2);
    sg_lock_cmpxchg32(ctx, mem, A(0), &expected, A(1));
    SG_RETURN_STDCALL(expected, 12);
}
