/* crt_format.c -- msvcr90 printf rules (and user32 wsprintf) over guest memory.
 *
 * Rules, with the documentation they come from (full citations in docs/design/hle-crt.md §4):
 *  - Spec: %[flags][width][.precision][{h|l|ll|I|I32|I64}]type (VS2008 "Format Specification
 *    Fields"). Size prefixes accumulate; 'w' = wide; 'L' accepted for double; 'F'/'N' are the
 *    old far/near modifiers and are skipped (VS2015 change history). No hh/j/z/t: those
 *    letters are unknown type characters.
 *  - "If a percent sign is followed by a character that has no meaning as a format field, the
 *    character is copied" (VS2008). So "%1$s" (positional, only in _printf_p) prints "$s" and
 *    "%zu" prints "zu"; %F (VS2015+) is a size modifier here, not a conversion.
 *  - c/s are narrow and C/S wide in printf; the reverse in wprintf; h forces narrow, l/w wide.
 *  - NULL %s/%S prints "(null)". %p prints 8 upper-case hex digits. Exponents have at least 3
 *    digits (e+000) unless _set_output_format, which the game does not import.
 *  - %n is disabled by default: the invalid-parameter handler runs (we log; returns -1).
 *  - Floating point: 17 significant digits then zeros; inf/nan as 1.#INF, 1.#IND, 1.#QNAN,
 *    1.#SNAN, rounded like digits (%.2f -> 1.#J). %a/%A default precision 6, and zero
 *    padding goes before "0x" (VS2015 change history lists both as old behaviour).
 *  - wsprintf (user32): flags '-', '#', '0' only; no floating point; output capped at 1024
 *    characters including the terminator (Win32 docs).
 */
#include <math.h>
#include <stdio.h>
#include <string.h>

#include "hle_imports.h"

#define A(i) SG_ARG(i)

enum {
    FL_LEFT = 1, FL_SIGN = 2, FL_SIGNSP = 4, FL_ALT = 8, FL_ZERO = 16,
    FL_SHORT = 32, FL_LONG = 64, FL_I64 = 128, FL_WIDE = 256, FL_NARROW = 512
};

typedef struct {
    const uint8_t *mem;
    uint32_t a;
    int wide;
} fstr;
static unsigned fch(const fstr *f, uint32_t i) {
    return f->wide ? sg_ld16(f->mem, f->a + 2u * i) : f->mem[(uint32_t)(f->a + i)];
}

static uint32_t va32(hle_va *v) { uint32_t x = sg_ld32(v->mem, v->ap); v->ap += 4; return x; }
static uint64_t va64(hle_va *v) { uint64_t x = sg_ld64(v->mem, v->ap); v->ap += 8; return x; }
static double vaf64(hle_va *v) { double x = sg_ldf64(v->mem, v->ap); v->ap += 8; return x; }

typedef struct {
    hle_obuf *o;
    int err;
} emit;

static void put(emit *e, unsigned u) { hle_obuf_put(e->o, u); }
static void pad(emit *e, unsigned ch, int n) { while (n-- > 0) put(e, ch); }

/* Writes prefix+body with width/justification/zero padding as msvcr90's _output does. */
static void field(emit *e, int flags, int width, const char *prefix, const unsigned *body, int blen) {
    int plen = (int)strlen(prefix), fill = width - plen - blen, i;
    if (!(flags & (FL_LEFT | FL_ZERO))) pad(e, ' ', fill);
    for (i = 0; i < plen; i++) put(e, (unsigned char)prefix[i]);
    if ((flags & FL_ZERO) && !(flags & FL_LEFT)) pad(e, '0', fill);
    for (i = 0; i < blen; i++) put(e, body[i]);
    if (flags & FL_LEFT) pad(e, ' ', fill);
}
static void field_ascii(emit *e, int flags, int width, const char *prefix, const char *body) {
    unsigned tmp[512];
    int n = 0;
    while (body[n] && n < 512) { tmp[n] = (unsigned char)body[n]; n++; }
    field(e, flags, width, prefix, tmp, n);
}

/* ---- integers ------------------------------------------------------------------------ */

static void fmt_int(emit *e, unsigned type, int flags, int width, int prec, hle_va *va, int wsprintf_rules) {
    uint64_t u;
    int neg = 0, is_signed = (type == 'd' || type == 'i');
    unsigned radix = type == 'o' ? 8 : (type == 'x' || type == 'X' || type == 'p') ? 16 : 10;
    const char *dig = (type == 'X' || type == 'p') ? "0123456789ABCDEF" : "0123456789abcdef";
    char body[80], prefix[4] = "";
    int n = 0, i, nonzero;
    char tmp[72];

    if (flags & FL_I64) u = va64(va);
    else {
        uint32_t x = va32(va);
        if (flags & FL_SHORT) u = is_signed ? (uint64_t)(int64_t)(int16_t)x : (uint16_t)x;
        else u = is_signed ? (uint64_t)(int64_t)(int32_t)x : x;
    }
    if (is_signed && (int64_t)u < 0) { neg = 1; u = 0 - u; }
    if (!(flags & FL_I64) && !is_signed) u &= (flags & FL_SHORT) ? 0xffffu : 0xffffffffu;
    nonzero = u != 0;
    if (type == 'p') { prec = 8; flags &= ~FL_ALT; }
    if (prec < 0) prec = 1;
    else flags &= ~FL_ZERO; /* a precision turns off zero padding */
    if (wsprintf_rules && prec == 0) prec = 1;
    for (i = 0; u; u /= radix) tmp[i++] = dig[u % radix];
    while (i < prec && i < 64) tmp[i++] = '0';
    if (type == 'o' && (flags & FL_ALT) && (i == 0 || tmp[i - 1] != '0')) tmp[i++] = '0';
    for (n = 0; n < i; n++) body[n] = tmp[i - 1 - n];
    body[n] = 0;
    if (is_signed) {
        if (neg) strcpy(prefix, "-");
        else if (flags & FL_SIGN) strcpy(prefix, "+");
        else if (flags & FL_SIGNSP) strcpy(prefix, " ");
    } else if (radix == 16 && type != 'p' && (flags & FL_ALT) && nonzero) {
        /* '#': 0x/0X before a non-zero value [inf: msvcr90 prints plain "0" for zero] */
        prefix[0] = '0';
        prefix[1] = type == 'X' ? 'X' : 'x';
        prefix[2] = 0;
    }
    field_ascii(e, flags, width, prefix, body);
}

/* ---- floating point --------------------------------------------------------------------- */

static int put_exp(char *p, int x, unsigned ech) {
    int n = 0;
    p[n++] = (char)ech;
    p[n++] = x < 0 ? '-' : '+';
    if (x < 0) x = -x;
    n += snprintf(p + n, 8, "%03d", x);
    return n;
}

static void fmt_float(emit *e, unsigned type, int flags, int width, int prec, hle_va *va) {
    double v = vaf64(va);
    char d[400], body[480], prefix[4] = "";
    int neg = 0, decpt, n = 0, i, lower = type == 'e' || type == 'f' || type == 'g' || type == 'a';
    unsigned t = lower ? type : type + 32;

    if (t == 'a') {
        /* hex float, old CRT: default precision 6; zero padding lands before "0x" */
        uint64_t b;
        int ex, p = prec < 0 ? 6 : prec;
        uint64_t m;
        memcpy(&b, &v, 8);
        neg = (int)(b >> 63);
        ex = (int)((b >> 52) & 0x7ff);
        m = b & 0xfffffffffffffull;
        if (ex == 0x7ff) { /* specials use the decimal spellings */
            decpt = hle_fltout(v, p, 1, d, sizeof d, &neg);
            snprintf(body, sizeof body, "1.%s", d + 1);
        } else {
            int lead = ex ? 1 : 0, e2 = ex ? ex - 1023 : (m ? -1022 : 0);
            if (p < 13) { /* round to p hex digits, half to even [inf] */
                int sh = 4 * (13 - p);
                uint64_t full = ((uint64_t)lead << 52) | m, half = 1ull << (sh - 1), rem = full & ((1ull << sh) - 1);
                full >>= sh;
                if (rem > half || (rem == half && (full & 1))) full++;
                lead = (int)(full >> (4 * p));
                m = full & ((p ? (1ull << (4 * p)) : 1ull) - 1);
                if (!p) m = 0;
                m <<= sh;
                if (lead == 2) { lead = 1; e2++; } /* 1.fff... rounded up to 2.0 */
            }
            n = snprintf(body, sizeof body, "%s%d", t == type ? "0x" : "0X", lead);
            if (p || (flags & FL_ALT)) body[n++] = '.';
            for (i = 0; i < p; i++) {
                unsigned dd = i < 13 ? (unsigned)((m >> (48 - 4 * i)) & 0xf) : 0;
                body[n++] = (char)(dd < 10 ? '0' + dd : (t == type ? 'a' : 'A') + dd - 10);
            }
            n += snprintf(body + n, sizeof body - (size_t)n, "%c%+d", t == type ? 'p' : 'P', e2);
        }
    } else {
        int p = prec < 0 ? 6 : prec, gstyle_e = 0;
        if (t == 'g') {
            if (p == 0) p = 1;
            decpt = hle_fltout(v, p, 0, d, sizeof d, &neg);
            gstyle_e = (decpt - 1 < -4 || decpt - 1 >= p);
        }
        if (t == 'e' || (t == 'g' && gstyle_e)) {
            int ep = t == 'g' ? p - 1 : p;
            if (t == 'e') decpt = hle_fltout(v, ep + 1, 0, d, sizeof d, &neg);
            body[n++] = d[0];
            if (ep > 0 || (flags & FL_ALT)) body[n++] = '.';
            for (i = 1; i <= ep; i++) body[n++] = d[i] ? d[i] : '0';
            if (t == 'g' && !(flags & FL_ALT)) { /* strip trailing zeros */
                while (n > 1 && body[n - 1] == '0') n--;
                if (body[n - 1] == '.') n--;
            }
            n += put_exp(body + n, (d[0] == '0' && v == 0.0) ? 0 : decpt - 1, lower ? 'e' : 'E');
        } else {
            int fp = t == 'g' ? p - decpt : p, dl;
            if (t == 'f') decpt = hle_fltout(v, fp, 1, d, sizeof d, &neg);
            dl = (int)strlen(d);
            if (decpt <= 0) body[n++] = '0';
            else for (i = 0; i < decpt; i++) body[n++] = i < dl ? d[i] : '0';
            if (fp > 0 || (flags & FL_ALT)) body[n++] = '.';
            for (i = 0; i < fp && n < 470; i++) {
                int k = decpt + i;
                body[n++] = (k >= 0 && k < dl) ? d[k] : '0';
            }
            if (t == 'g' && !(flags & FL_ALT)) {
                int has_point = memchr(body, '.', (size_t)n) != NULL;
                if (has_point) {
                    while (body[n - 1] == '0') n--;
                    if (body[n - 1] == '.') n--;
                }
            }
        }
        body[n] = 0;
    }
    body[n ? n : (int)strlen(body)] = 0;
    if (neg) strcpy(prefix, "-");
    else if (flags & FL_SIGN) strcpy(prefix, "+");
    else if (flags & FL_SIGNSP) strcpy(prefix, " ");
    field_ascii(e, flags, width, prefix, body);
}

/* ---- characters and strings ------------------------------------------------------------ */

/* Converts a UTF-16 unit for narrow output in the "C" locale (wctomb: 0..255 map to the byte;
 * anything else fails with EILSEQ [inf]). */
static int narrow_unit(unsigned u, unsigned *out) {
    if (u > 0xff) return 0;
    *out = u;
    return 1;
}

static void fmt_str(emit *e, int src_wide, int out_wide, int flags, int width, int prec, uint32_t s,
                    const uint8_t *mem, int counted_struct) {
    unsigned buf[1024], *body = buf;
    int n = 0, cap = 1024;
    uint32_t len = 0xffffffffu;
    static const char nul[] = "(null)";
    if (counted_struct && s) { /* %Z: ANSI_STRING / UNICODE_STRING {USHORT Length, MaximumLength; PCHAR Buffer} */
        len = sg_ld16(mem, s) / (src_wide ? 2u : 1u);
        s = sg_ld32(mem, s + 4);
    }
    if (!s) {
        int i;
        for (i = 0; nul[i] && (prec < 0 || i < prec); i++) buf[n++] = (unsigned char)nul[i];
        field(e, flags & ~FL_ZERO, width, "", buf, n);
        return;
    }
    for (uint32_t i = 0; i < len && (prec < 0 || n < prec); i++) {
        unsigned u = src_wide ? sg_ld16(mem, s + 2u * i) : mem[(uint32_t)(s + i)];
        if (!u && len == 0xffffffffu) break;
        if (src_wide && !out_wide && !narrow_unit(u, &u)) { e->err = 1; break; }
        if (n == cap) { /* long strings: flush what we have as an unpadded prefix */
            int j;
            for (j = 0; j < n; j++) put(e, body[j]);
            width -= n;
            n = 0;
        }
        body[n++] = u;
    }
    field(e, flags & ~FL_ZERO, width, "", body, n);
}

/* ---- the engine ---------------------------------------------------------------------------- */

int hle_format(sg_ctx *ctx, uint8_t *mem, hle_obuf *out, uint32_t fmt, int fmt_wide, hle_va *va,
               enum hle_fmt_flavor flavor) {
    fstr f = {mem, fmt, fmt_wide};
    emit e = {out, 0};
    size_t start = out->n;
    uint32_t i = 0;
    int ws = flavor == HLE_FMT_WSPRINTF;
    (void)ctx;
    for (;;) {
        unsigned c = fch(&f, i++), t;
        int flags = 0, width = 0, prec = -1;
        if (!c) break;
        if (c != '%') { put(&e, c); continue; }
        /* flags */
        for (;; i++) {
            c = fch(&f, i);
            if (c == '-') flags |= FL_LEFT;
            else if (c == '0') flags |= FL_ZERO;
            else if (c == '#') flags |= FL_ALT;
            else if (!ws && c == '+') flags |= FL_SIGN;
            else if (!ws && c == ' ') flags |= FL_SIGNSP;
            else break;
        }
        /* width */
        if (!ws && c == '*') {
            width = (int32_t)va32(va);
            if (width < 0) { flags |= FL_LEFT; width = -width; }
            c = fch(&f, ++i);
        } else {
            while (c >= '0' && c <= '9') { width = width * 10 + (int)(c - '0'); c = fch(&f, ++i); }
        }
        /* precision */
        if (c == '.') {
            prec = 0;
            c = fch(&f, ++i);
            if (!ws && c == '*') {
                prec = (int32_t)va32(va);
                if (prec < 0) prec = -1;
                c = fch(&f, ++i);
            } else {
                while (c >= '0' && c <= '9') { prec = prec * 10 + (int)(c - '0'); c = fch(&f, ++i); }
            }
        }
        /* size prefixes (accumulate, as msvcr90 did) */
        for (;;) {
            if (c == 'h') flags |= FL_SHORT | FL_NARROW;
            else if (c == 'l') {
                if (!ws && fch(&f, i + 1) == 'l') { flags |= FL_I64; i++; }
                else flags |= FL_LONG | FL_WIDE;
            } else if (c == 'w' && !ws) flags |= FL_WIDE;
            else if (c == 'L' && !ws) { /* long double == double */ }
            else if ((c == 'F' || c == 'N') && !ws) { /* far/near: ignored */ }
            else if (c == 'I') {
                if (fch(&f, i + 1) == '6' && fch(&f, i + 2) == '4') { flags |= FL_I64; i += 2; }
                else if (fch(&f, i + 1) == '3' && fch(&f, i + 2) == '2') { i += 2; }
                /* plain I: pointer-sized = 32-bit on x86 */
            } else break;
            c = fch(&f, ++i);
        }
        if (!c) break; /* format ended inside a specification: nothing more is printed [inf] */
        i++;
        t = c;
        switch (t) {
        case 'd': case 'i': case 'u': case 'o': case 'x': case 'X':
            fmt_int(&e, t, flags, width, prec, va, ws);
            break;
        case 'p':
            fmt_int(&e, t, flags, width, prec, va, ws);
            break;
        case 'e': case 'E': case 'f': case 'g': case 'G': case 'a': case 'A':
            if (ws) { put(&e, t); break; }
            fmt_float(&e, t, flags, width, prec, va);
            break;
        case 'c': case 'C': {
            /* which width does the argument have? */
            int wide_arg = (t == 'C') ^ fmt_wide;
            unsigned u = va32(va);
            if (flags & FL_NARROW) wide_arg = 0;
            if (flags & FL_WIDE) wide_arg = 1;
            u = wide_arg ? (u & 0xffffu) : (u & 0xffu);
            if (ws && (flags & (FL_NARROW | FL_WIDE)) && u == 0) break; /* wsprintf: hc/lc of 0 is ignored */
            if (wide_arg && !fmt_wide && !narrow_unit(u, &u)) { e.err = 1; break; }
            field(&e, flags & ~FL_ZERO, width, "", &u, 1);
            break;
        }
        case 's': case 'S': case 'Z': {
            int wide_arg = (t == 'S') ^ fmt_wide;
            if (t == 'Z') wide_arg = !!(flags & FL_WIDE); /* %Z ANSI_STRING, %wZ UNICODE_STRING */
            else {
                if (flags & FL_NARROW) wide_arg = 0;
                if (flags & FL_WIDE) wide_arg = 1;
            }
            if (ws && t == 'Z') { put(&e, t); break; }
            fmt_str(&e, wide_arg, fmt_wide, flags, width, prec, va32(va), mem, t == 'Z');
            break;
        }
        case 'n':
            if (ws) { put(&e, t); break; }
            (void)va32(va);
            hle_invalid_parameter(ctx, "printf", "%n is disabled (_set_printf_count_output not called)");
            return -1;
        default:
            put(&e, t); /* unknown type character: copied (VS2008 docs); "%%" lands here too */
            break;
        }
        if (e.err) break;
    }
    if (e.err) return -1;
    return (int)(out->n - start);
}

/* ---- shims -------------------------------------------------------------------------------- */

static void copy_out(uint8_t *mem, uint32_t dst, const hle_obuf *o, size_t n) {
    if (o->wide) memcpy(mem + dst, o->p, 2 * n);
    else memcpy(mem + dst, o->p, n);
}
static void put_nul(uint8_t *mem, uint32_t dst, int wide, size_t at) {
    if (wide) sg_st16(mem, dst + 2u * (uint32_t)at, 0);
    else mem[(uint32_t)(dst + at)] = 0;
}

static int32_t do_sprintf(sg_ctx *ctx, uint8_t *mem, uint32_t buf, uint32_t fmt, uint32_t ap) {
    hle_obuf o = {0, 0, 0, 0};
    hle_va va = {mem, ap};
    int r;
    if (!buf || !fmt) {
        hle_invalid_parameter(ctx, "sprintf", "NULL buffer or format");
        hle_set_errno(mem, MSVC_EINVAL);
        return -1;
    }
    r = hle_format(ctx, mem, &o, fmt, 0, &va, HLE_FMT_CRT);
    if (r >= 0) { copy_out(mem, buf, &o, o.n); put_nul(mem, buf, 0, o.n); }
    hle_obuf_free(&o);
    return r;
}
SG_IMPORT(msvcr90, sprintf) { SG_RETURN_CDECL(do_sprintf(ctx, mem, A(0), A(1), ctx->esp + 12)); }
SG_IMPORT(msvcr90, vsprintf) { SG_RETURN_CDECL(do_sprintf(ctx, mem, A(0), A(1), A(2))); }

/* _snprintf: len < count: copy + NUL, return len; len == count: copy, no NUL, return len;
 * len > count: copy count units, no NUL, return a negative value (VS2008 docs). */
static int32_t do_snprintf(sg_ctx *ctx, uint8_t *mem, uint32_t buf, uint32_t count, uint32_t fmt, uint32_t ap) {
    hle_obuf o = {0, 0, 0, 0};
    hle_va va = {mem, ap};
    int r;
    if ((!buf && count) || !fmt) {
        hle_invalid_parameter(ctx, "_snprintf", "NULL buffer or format");
        hle_set_errno(mem, MSVC_EINVAL);
        return -1;
    }
    r = hle_format(ctx, mem, &o, fmt, 0, &va, HLE_FMT_CRT);
    if (r >= 0) {
        if (o.n < count) { copy_out(mem, buf, &o, o.n); put_nul(mem, buf, 0, o.n); }
        else if (o.n == count) copy_out(mem, buf, &o, o.n);
        else { copy_out(mem, buf, &o, count); r = -1; }
    }
    hle_obuf_free(&o);
    return r;
}
SG_IMPORT(msvcr90, _snprintf) { SG_RETURN_CDECL(do_snprintf(ctx, mem, A(0), A(1), A(2), ctx->esp + 16)); }
SG_IMPORT(msvcr90, _vsnprintf) { SG_RETURN_CDECL(do_snprintf(ctx, mem, A(0), A(1), A(2), A(3))); }

/* printf: the game is a GUI process with no console; stdout output goes to the HLE log. */
SG_IMPORT(msvcr90, printf) {
    hle_obuf o = {0, 0, 0, 0};
    hle_va va = {mem, ctx->esp + 8};
    int r = hle_format(ctx, mem, &o, A(0), 0, &va, HLE_FMT_CRT);
    if (r > 0) hle_log(HLE_LOG_INFO, "guest stdout: %.*s", (int)o.n, (const char *)o.p);
    hle_obuf_free(&o);
    SG_RETURN_CDECL(r);
}

/* wsprintfA/W: cdecl; output capped at 1024 units including the terminator [inf: the cap
 * truncates silently and returns the stored count]. */
static int32_t do_wsprintf(sg_ctx *ctx, uint8_t *mem, int wide) {
    hle_obuf o = {0, 0, 0, wide};
    hle_va va = {mem, ctx->esp + 12};
    uint32_t buf = A(0);
    int r = hle_format(ctx, mem, &o, A(1), wide, &va, HLE_FMT_WSPRINTF);
    size_t n = o.n > 1023 ? 1023 : o.n;
    if (r < 0) n = 0;
    copy_out(mem, buf, &o, n);
    put_nul(mem, buf, wide, n);
    hle_obuf_free(&o);
    return (int32_t)n;
}
SG_IMPORT(user32, wsprintfA) { SG_RETURN_CDECL(do_wsprintf(ctx, mem, 0)); }
SG_IMPORT(user32, wsprintfW) { SG_RETURN_CDECL(do_wsprintf(ctx, mem, 1)); }
