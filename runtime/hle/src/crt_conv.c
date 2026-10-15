/* crt_conv.c -- number <-> text in msvcr90's (pre-2015) style, and the shims built on it.
 *
 * Old-CRT behaviours reproduced (docs/design/hle-crt.md section 4 cites the sources):
 *  - Formatting generates 17 significant digits, then zeros: printf("%.0f", 2^80) printed
 *    1208925819614629200000000. Rounding to the requested precision is done on that digit
 *    string, half-up (the CRT's _fptostr).
 *  - inf/nan are formatted as the digit strings "1#INF", "1#IND", "1#QNAN", "1#SNAN", which go
 *    through the same rounding: %.2f of +inf is "1.#J".
 *  - strtod accepts [ws][sign][digits][.digits][{d|D|e|E}[sign]digits]; no inf/nan/hex; it
 *    considers at most 17 significant digits.
 *  - atoi/atol accumulate with wrap-around (no overflow check) [inf]; strtol saturates.
 */
#include <math.h>
#include <stdio.h>
#include <stdlib.h>
#include <string.h>

#include "hle_imports.h"

#define A(i) SG_ARG(i)

/* ---- digit generation ---------------------------------------------------------------- */

static int special_digits(double v, char *buf, size_t cap, int *neg) {
    uint64_t b;
    memcpy(&b, &v, 8);
    *neg = (int)(b >> 63);
    if (((b >> 52) & 0x7ff) != 0x7ff) return 0;
    uint64_t m = b & 0xfffffffffffffull;
    const char *s;
    if (!m) s = "1#INF";
    else if (m == 0x8000000000000ull && *neg) s = "1#IND"; /* x86 "indefinite" (default NaN) */
    else if (m & 0x8000000000000ull) s = "1#QNAN";
    else s = "1#SNAN";
    snprintf(buf, cap, "%s", s);
    return 1;
}

int hle_fltout(double v, int ndigits, int count_from_point, char *buf, size_t cap, int *neg) {
    char d17[40], src[48];
    int decpt, n, i, len;
    if (cap < 2) return 0;
    if (special_digits(v, src, sizeof src, neg)) {
        decpt = 1;
    } else if (v == 0.0) {
        /* zero: one digit "0" at decpt 1 so %e prints e+000 and %f prints 0.xxx */
        snprintf(src, sizeof src, "0");
        decpt = 1;
    } else {
        int e;
        snprintf(d17, sizeof d17, "%.16e", fabs(v)); /* correctly rounded 17 significant digits */
        src[0] = d17[0];
        memcpy(src + 1, d17 + 2, 16);
        src[17] = 0;
        e = atoi(strchr(d17, 'e') + 1);
        decpt = e + 1;
    }
    n = count_from_point ? ndigits + decpt : ndigits;
    if (n < 0) { buf[0] = 0; return decpt; }
    if ((size_t)n + 2 > cap) n = (int)cap - 2;
    len = (int)strlen(src);
    /* buf[0] is a guard for the carry; digits start at buf + 1 */
    buf[0] = '0';
    for (i = 0; i < n; i++) buf[1 + i] = i < len ? src[i] : '0';
    buf[1 + n] = 0;
    if (n < len && src[n] >= '5') { /* _fptostr: round half up on the digit string */
        char *p = buf + n;
        while (p > buf && *p == '9') *p-- = '0';
        ++*p;
    }
    if (buf[0] != '0') {
        /* carry out of the first digit: the value became 10^k. Significant-digit mode keeps n
         * digits ("1" then zeros); fixed mode gains one integer digit (n + 1 digits). */
        decpt++;
        buf[count_from_point ? n + 1 : n] = 0;
        return decpt;
    }
    memmove(buf, buf + 1, (size_t)n + 1);
    return decpt;
}

/* ---- parsing ----------------------------------------------------------------------- */

static int is_ws(unsigned c) { return c == ' ' || (c >= 9 && c <= 13); }

double hle_strtod_units(unsigned (*get)(const void *s, size_t i), const void *s, size_t *consumed, int *range) {
    size_t i = 0, end;
    char buf[64];
    int bn = 0, sig = 0, dexp = 0, any = 0, neg = 0;
    double r;
    *range = 0;
    while (is_ws(get(s, i))) i++;
    if (get(s, i) == '+' || get(s, i) == '-') neg = get(s, i++) == '-';
    /* mantissa: keep the first 17 significant digits, count the rest into the exponent */
    while (get(s, i) == '0') { i++; any = 1; }
    while (get(s, i) >= '0' && get(s, i) <= '9') {
        if (sig < 17) buf[bn++] = (char)get(s, i), sig++;
        else dexp++;
        i++; any = 1;
    }
    if (get(s, i) == '.') {
        size_t j = i + 1;
        int fany = 0;
        while (get(s, j) == '0' && sig == 0) { j++; dexp--; fany = 1; }
        while (get(s, j) >= '0' && get(s, j) <= '9') {
            if (sig < 17) { buf[bn++] = (char)get(s, j); sig++; dexp--; }
            j++; fany = 1;
        }
        if (any || fany) { i = j; any = 1; }
    }
    if (!any) { *consumed = 0; return 0.0; }
    end = i;
    {
        unsigned c = get(s, i);
        if (c == 'e' || c == 'E' || c == 'd' || c == 'D') {
            size_t j = i + 1;
            int eneg = 0;
            long ev = 0;
            if (get(s, j) == '+' || get(s, j) == '-') eneg = get(s, j++) == '-';
            if (get(s, j) >= '0' && get(s, j) <= '9') {
                while (get(s, j) >= '0' && get(s, j) <= '9') {
                    if (ev < 100000) ev = ev * 10 + (long)(get(s, j) - '0');
                    j++;
                }
                dexp += (int)(eneg ? -ev : ev);
                end = j;
            }
        }
    }
    *consumed = end;
    if (!bn) return neg ? -0.0 : 0.0;
    snprintf(buf + bn, sizeof buf - (size_t)bn, "e%d", dexp);
    r = strtod(buf, NULL);
    if (isinf(r)) { *range = 1; r = HUGE_VAL; }
    else if (r == 0.0) *range = 1; /* underflow: msvcr90 returns 0 and sets ERANGE */
    return neg ? -r : r;
}

/* strtol family over guest narrow or wide strings. `base` as in C; returns the parsed value
 * with C saturation rules for (un)signed 32/64-bit; *end_units = units consumed (0 if none). */
typedef struct { const uint8_t *mem; uint32_t a; int wide; } gstr;
static unsigned gs(const gstr *g, size_t i) {
    return g->wide ? sg_ld16(g->mem, g->a + 2u * (uint32_t)i) : g->mem[(uint32_t)(g->a + i)];
}

static uint64_t parse_int(const gstr *g, int base, int is_unsigned, int bits, size_t *end, int *range) {
    size_t i = 0;
    int neg = 0, any = 0;
    uint64_t v = 0, lim = bits == 64 ? ~0ull : 0xffffffffull, cut;
    *range = 0;
    while (is_ws(gs(g, i))) i++;
    if (gs(g, i) == '+' || gs(g, i) == '-') neg = gs(g, i++) == '-';
    if ((base == 0 || base == 16) && gs(g, i) == '0' && (gs(g, i + 1) | 32) == 'x') {
        i += 2; base = 16;
    } else if (base == 0) {
        base = gs(g, i) == '0' ? 8 : 10;
    }
    if (base < 2 || base > 36) { *end = 0; return 0; }
    cut = lim / (uint64_t)base;
    for (;; i++) {
        unsigned c = gs(g, i), d;
        if (c >= '0' && c <= '9') d = c - '0';
        else if ((c | 32) >= 'a' && (c | 32) <= 'z') d = (c | 32) - 'a' + 10;
        else break;
        if (d >= (unsigned)base) break;
        any = 1;
        if (v > cut || (v == cut && d > lim % (uint64_t)base)) *range = 1;
        else v = v * (uint64_t)base + d;
    }
    if (!any) { *end = 0; return 0; } /* msvcr90: no digits -> endptr = nptr (even after "0x") [inf] */
    *end = i;
    if (is_unsigned) {
        if (*range) return lim;
        return neg ? (uint64_t)(-(int64_t)v) & lim : v;
    }
    {
        uint64_t smax = lim >> 1;
        if (*range || (!neg && v > smax) || (neg && v > smax + 1)) {
            *range = 1;
            return neg ? (smax + 1) & lim : smax;
        }
        return neg ? (uint64_t)(-(int64_t)v) & lim : v;
    }
}

static void store_end(uint8_t *mem, uint32_t endp, uint32_t s, size_t units, int wide) {
    if (endp) sg_st32(mem, endp, s + (uint32_t)(units * (wide ? 2 : 1)));
}

#define STRTOX(fn, wide, uns, bits)                                                 \
    SG_IMPORT(msvcr90, fn) {                                                        \
        gstr g = {mem, A(0), wide};                                                 \
        size_t end; int range;                                                      \
        uint64_t v = parse_int(&g, (int)A(2), uns, bits, &end, &range);             \
        store_end(mem, A(1), A(0), end, wide);                                      \
        if (range) hle_set_errno(mem, MSVC_ERANGE);                                 \
        if (bits == 64) SG_RETURN_U64(v, 0);                                        \
        SG_RETURN_CDECL((uint32_t)v);                                               \
    }
STRTOX(strtol, 0, 0, 32)
STRTOX(strtoul, 0, 1, 32)
STRTOX(_strtoui64, 0, 1, 64)
STRTOX(wcstol, 1, 0, 32)
STRTOX(wcstoul, 1, 1, 32)

/* atol/atoi/_wtol: [ws][sign]digits, accumulated in 32 bits with wrap-around [inf]. */
static uint32_t ato32(const gstr *g) {
    size_t i = 0;
    int neg = 0;
    uint32_t v = 0;
    while (is_ws(gs(g, i))) i++;
    if (gs(g, i) == '+' || gs(g, i) == '-') neg = gs(g, i++) == '-';
    while (gs(g, i) >= '0' && gs(g, i) <= '9') v = v * 10u + (gs(g, i++) - '0');
    return neg ? 0u - v : v;
}
SG_IMPORT(msvcr90, atoi) { gstr g = {mem, A(0), 0}; SG_RETURN_CDECL(ato32(&g)); }
SG_IMPORT(msvcr90, atol) { gstr g = {mem, A(0), 0}; SG_RETURN_CDECL(ato32(&g)); }
SG_IMPORT(msvcr90, _wtol) { gstr g = {mem, A(0), 1}; SG_RETURN_CDECL(ato32(&g)); }

static unsigned get_g(const void *s, size_t i) { return gs((const gstr *)s, i); }

static double guest_strtod(uint8_t *mem, uint32_t s, int wide, uint32_t endp) {
    gstr g = {mem, s, wide};
    size_t used;
    int range;
    double r = hle_strtod_units(get_g, &g, &used, &range);
    store_end(mem, endp, s, used, wide);
    if (range) hle_set_errno(mem, MSVC_ERANGE);
    return r;
}
/* atof/strtod/wcstod return in ST(0) (x87 effect (0, +1)). */
SG_IMPORT(msvcr90, atof) { sg_fpu_push(ctx, guest_strtod(mem, A(0), 0, 0)); SG_RET(0); }
SG_IMPORT(msvcr90, strtod) { sg_fpu_push(ctx, guest_strtod(mem, A(0), 0, A(1))); SG_RET(0); }
SG_IMPORT(msvcr90, wcstod) { sg_fpu_push(ctx, guest_strtod(mem, A(0), 1, A(1))); SG_RET(0); }

/* ---- integer -> text ---------------------------------------------------------------- */

static uint32_t xtoa(uint8_t *mem, uint32_t val, uint32_t buf, uint32_t radix, int is_signed, int wide) {
    char tmp[40];
    int n = 0, neg = 0, i;
    uint32_t pos = buf;
    if (radix < 2 || radix > 36) radix = 10;
    if (is_signed && radix == 10 && (int32_t)val < 0) { neg = 1; val = 0u - val; }
    do { uint32_t d = val % radix; tmp[n++] = (char)(d < 10 ? '0' + d : 'a' + d - 10); val /= radix; } while (val);
    if (neg) tmp[n++] = '-';
    for (i = n - 1; i >= 0; i--) {
        if (wide) { sg_st16(mem, pos, (uint16_t)tmp[i]); pos += 2; }
        else mem[pos++] = (uint8_t)tmp[i];
    }
    if (wide) sg_st16(mem, pos, 0); else mem[pos] = 0;
    return buf;
}
SG_IMPORT(msvcr90, _ltoa) { SG_RETURN_CDECL(xtoa(mem, A(0), A(1), A(2), 1, 0)); }
SG_IMPORT(msvcr90, _ultoa) { SG_RETURN_CDECL(xtoa(mem, A(0), A(1), A(2), 0, 0)); }
SG_IMPORT(msvcr90, _ltow) { SG_RETURN_CDECL(xtoa(mem, A(0), A(1), A(2), 1, 1)); }
SG_IMPORT(msvcr90, _ultow) { SG_RETURN_CDECL(xtoa(mem, A(0), A(1), A(2), 0, 1)); }

/* ---- _ecvt / _fcvt (per-thread static buffer, as msvcr90) ----------------------------- */

static uint32_t cvt(uint8_t *mem, double v, int count, int fixed, uint32_t decp, uint32_t signp) {
    char d[400];
    int neg, decpt, i;
    uint32_t out = hle_thread()->cvt_va;
    if (count < 0) count = 0;
    if (count > 340) count = 340;
    if (v == 0.0 && !isnan(v)) { /* zero: `count` zeros (fixed: count after the point), decpt 0 [inf] */
        for (i = 0; i < count; i++) d[i] = '0';
        d[count] = 0;
        decpt = 0;
        neg = signbit(v) != 0;
    } else {
        decpt = hle_fltout(v, count, fixed, d, sizeof d, &neg);
    }
    for (i = 0; d[i]; i++) mem[out + (uint32_t)i] = (uint8_t)d[i];
    mem[out + (uint32_t)i] = 0;
    if (decp) sg_st32(mem, decp, (uint32_t)decpt);
    if (signp) sg_st32(mem, signp, (uint32_t)neg);
    return out;
}
SG_IMPORT(msvcr90, _ecvt) { SG_RETURN_CDECL(cvt(mem, SG_ARG_F64(0), (int)A(2), 0, A(3), A(4))); }
SG_IMPORT(msvcr90, _fcvt) { SG_RETURN_CDECL(cvt(mem, SG_ARG_F64(0), (int)A(2), 1, A(3), A(4))); }

/* ---- small CRT state ---------------------------------------------------------------- */

/* rand: msvcr90's LCG on the per-thread seed (starts at 1; the game imports no srand). */
SG_IMPORT(msvcr90, rand) {
    hle_tls *t = hle_thread();
    t->rand_seed = t->rand_seed * 214013u + 2531011u;
    SG_RETURN_CDECL((t->rand_seed >> 16) & 0x7fffu);
}
SG_IMPORT(msvcr90, _errno) { SG_RETURN_CDECL(hle_thread()->errno_va); }
/* msvcr90 XORs with a per-process cookie; values only ever round-trip, so identity suffices. */
SG_IMPORT(msvcr90, _encode_pointer) { SG_RETURN_CDECL(A(0)); }
SG_IMPORT(msvcr90, _decode_pointer) { SG_RETURN_CDECL(A(0)); }
