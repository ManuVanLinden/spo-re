/* crt_scan.c -- msvcr90 sscanf/swscanf rules over guest memory.
 *
 * Rules (docs/design/hle-crt.md §4.3 cites the sources):
 *  - Size prefixes h, l, ll, I64, L (and w); h/l on c/s/C/S choose narrow/wide storage; in
 *    sscanf c/s store narrow and C/S wide, the reverse in swscanf.
 *  - %[...] sets accept ranges in either order: %[z-a] == %[a-z]; a leading ']' is a member.
 *  - %Nc stores N characters without a terminator; %n stores the units consumed.
 *  - Floating point: decimal only. "inf", "nan" and hex floats are not accepted before
 *    VS2015 (they were added with the UCRT's parser), and at most 17 significant digits count.
 *  - Integers accumulate with wrap-around, not saturation [inf: old input.c had no overflow
 *    check]; %i follows 0x / 0 prefixes; %p reads hex.
 *  - Returns the number of assigned fields, or EOF (-1) when the input ends before the first
 *    conversion.
 */
#include <string.h>

#include "hle_imports.h"

#define A(i) SG_ARG(i)

typedef struct {
    const uint8_t *mem;
    uint32_t a;
    int wide;
} src;
static unsigned at(const src *s, uint32_t i) {
    return s->wide ? sg_ld16(s->mem, s->a + 2u * i) : s->mem[(uint32_t)(s->a + i)];
}
static int is_ws(unsigned c) { return c == ' ' || (c >= 9 && c <= 13); }

static unsigned get_c(const void *s, size_t i) { return (unsigned char)((const char *)s)[i]; }

enum { SZ_SHORT = 1, SZ_LONG = 2, SZ_I64 = 4, SZ_WIDE = 8, SZ_NARROW = 16, SZ_DOUBLE = 32 };

int hle_scan(uint8_t *mem, uint32_t input, int wide, uint32_t fmt, hle_va *va) {
    src in = {mem, input, wide}, f = {mem, fmt, wide};
    uint32_t ip = 0, fp = 0;
    int assigned = 0, attempted = 0;
    for (;;) {
        unsigned c = at(&f, fp);
        if (!c) break;
        if (is_ws(c)) {
            while (is_ws(at(&f, fp))) fp++;
            while (is_ws(at(&in, ip))) ip++;
            continue;
        }
        if (c != '%' || at(&f, fp + 1) == '%') {
            if (c == '%') fp++;
            if (!at(&in, ip)) return attempted ? assigned : -1;
            if (at(&in, ip) != c) break;
            ip++; fp++;
            continue;
        }
        fp++;
        {
            int suppress = 0, width = 0, size = 0;
            unsigned t;
            uint32_t dst = 0;
            if (at(&f, fp) == '*') { suppress = 1; fp++; }
            while (at(&f, fp) >= '0' && at(&f, fp) <= '9') width = width * 10 + (int)(at(&f, fp++) - '0');
            for (;;) {
                unsigned m = at(&f, fp);
                if (m == 'h') size |= SZ_SHORT | SZ_NARROW;
                else if (m == 'l') {
                    if (at(&f, fp + 1) == 'l') { size |= SZ_I64; fp++; }
                    else size |= SZ_LONG | SZ_WIDE | SZ_DOUBLE;
                } else if (m == 'L') size |= SZ_DOUBLE;
                else if (m == 'w') size |= SZ_WIDE;
                else if (m == 'I') {
                    if (at(&f, fp + 1) == '6' && at(&f, fp + 2) == '4') { size |= SZ_I64; fp += 2; }
                    else if (at(&f, fp + 1) == '3' && at(&f, fp + 2) == '2') fp += 2;
                } else if (m == 'F' || m == 'N') { /* far/near: ignored */ }
                else break;
                fp++;
            }
            t = at(&f, fp++);
            if (!t) break;
            if (t == 'n') {
                if (!suppress) {
                    dst = sg_ld32(mem, va->ap); va->ap += 4;
                    if (size & SZ_SHORT) sg_st16(mem, dst, (uint16_t)ip);
                    else if (size & SZ_I64) sg_st64(mem, dst, ip);
                    else sg_st32(mem, dst, ip);
                }
                continue;
            }
            if (t != 'c' && t != 'C' && t != '[') {
                while (is_ws(at(&in, ip))) ip++;
            }
            if (!at(&in, ip)) return attempted ? assigned : -1;
            attempted = 1;
            if (!suppress) { dst = sg_ld32(mem, va->ap); va->ap += 4; }
            if (width <= 0) width = -1;

            switch (t) {
            case 'c': case 'C': case 's': case 'S': case '[': {
                int store_wide = ((t == 'C' || t == 'S') ^ wide);
                unsigned char set[65536 / 8];
                int neg = 0, n = 0;
                if (size & SZ_NARROW) store_wide = 0;
                if (size & SZ_WIDE) store_wide = 1;
                if (t == '[') {
                    unsigned prev = 0, m;
                    int first = 1;
                    memset(set, 0, sizeof set);
                    if (at(&f, fp) == '^') { neg = 1; fp++; }
                    for (;; first = 0) {
                        m = at(&f, fp);
                        if (!m) break;
                        if (m == ']' && !first) { fp++; break; }
                        if (m == '-' && prev && at(&f, fp + 1) != ']' && at(&f, fp + 1)) {
                            unsigned hi = at(&f, fp + 1), lo = prev, x;
                            if (lo > hi) { x = lo; lo = hi; hi = x; }
                            for (x = lo; x <= hi; x++) set[x >> 3] |= (unsigned char)(1u << (x & 7));
                            fp += 2;
                            prev = 0;
                            continue;
                        }
                        set[m >> 3] |= (unsigned char)(1u << (m & 7));
                        prev = m;
                        fp++;
                    }
                }
                if ((t == 'c' || t == 'C') && width < 0) width = 1;
                for (; width < 0 || n < width; n++) {
                    unsigned u = at(&in, ip);
                    if (!u) break;
                    if ((t == 's' || t == 'S') && is_ws(u)) break;
                    if (t == '[') {
                        int member = (set[(u & 0xffff) >> 3] >> (u & 7)) & 1;
                        if (member == neg) break;
                    }
                    if (!suppress) {
                        if (store_wide) sg_st16(mem, dst + 2u * (uint32_t)n, (uint16_t)u);
                        else mem[(uint32_t)(dst + (uint32_t)n)] = (uint8_t)(u > 0xff ? '?' : u);
                    }
                    ip++;
                }
                if (!n) return assigned;
                if (!suppress) {
                    if (t != 'c' && t != 'C') {
                        if (store_wide) sg_st16(mem, dst + 2u * (uint32_t)n, 0);
                        else mem[(uint32_t)(dst + (uint32_t)n)] = 0;
                    }
                    assigned++;
                }
                break;
            }
            case 'd': case 'i': case 'o': case 'u': case 'x': case 'X': case 'p': {
                unsigned base = t == 'o' ? 8 : (t == 'x' || t == 'X' || t == 'p') ? 16 : t == 'i' ? 0 : 10;
                uint64_t v = 0;
                int neg = 0, any = 0, used = 0;
#define TAKE() (ip++, used++)
#define ROOM() (width < 0 || used < width)
                if (ROOM() && (at(&in, ip) == '-' || at(&in, ip) == '+')) { neg = at(&in, ip) == '-'; TAKE(); }
                if (ROOM() && at(&in, ip) == '0' && (base == 0 || base == 16)) {
                    TAKE(); any = 1;
                    if (ROOM() && (at(&in, ip) | 32) == 'x') { TAKE(); base = 16; any = 0; }
                    else if (base == 0) base = 8;
                }
                if (base == 0) base = 10;
                while (ROOM()) {
                    unsigned u = at(&in, ip), d;
                    if (u >= '0' && u <= '9') d = u - '0';
                    else if ((u | 32) >= 'a' && (u | 32) <= 'f') d = (u | 32) - 'a' + 10;
                    else break;
                    if (d >= base) break;
                    v = v * base + d; /* wraps, like the old CRT */
                    TAKE(); any = 1;
                }
#undef TAKE
#undef ROOM
                if (!any) return assigned;
                if (neg) v = 0 - v;
                if (!suppress) {
                    if (size & SZ_I64) sg_st64(mem, dst, v);
                    else if ((size & SZ_SHORT) && t != 'p') sg_st16(mem, dst, (uint16_t)v);
                    else sg_st32(mem, dst, (uint32_t)v);
                    assigned++;
                }
                break;
            }
            case 'e': case 'E': case 'f': case 'g': case 'G': {
                char buf[400];
                int n = 0, digits = 0;
                size_t used;
                int range;
                double d;
#define ROOM() ((width < 0 || n < width) && n < 390)
                if (ROOM() && (at(&in, ip) == '-' || at(&in, ip) == '+')) buf[n++] = (char)at(&in, ip++);
                while (ROOM() && at(&in, ip) >= '0' && at(&in, ip) <= '9') { buf[n++] = (char)at(&in, ip++); digits++; }
                if (ROOM() && at(&in, ip) == '.') {
                    buf[n++] = (char)at(&in, ip++);
                    while (ROOM() && at(&in, ip) >= '0' && at(&in, ip) <= '9') { buf[n++] = (char)at(&in, ip++); digits++; }
                }
                if (digits && ROOM() && (at(&in, ip) | 32) == 'e') {
                    int save_n = n;
                    uint32_t save_ip = ip;
                    buf[n++] = (char)at(&in, ip++);
                    if (ROOM() && (at(&in, ip) == '-' || at(&in, ip) == '+')) buf[n++] = (char)at(&in, ip++);
                    if (ROOM() && at(&in, ip) >= '0' && at(&in, ip) <= '9') {
                        while (ROOM() && at(&in, ip) >= '0' && at(&in, ip) <= '9') buf[n++] = (char)at(&in, ip++);
                    } else { n = save_n; ip = save_ip; }
                }
#undef ROOM
                buf[n] = 0;
                if (!digits) return assigned;
                d = hle_strtod_units(get_c, buf, &used, &range);
                if (!suppress) {
                    if (size & SZ_DOUBLE) sg_stf64(mem, dst, d);
                    else sg_stf32(mem, dst, (float)d);
                    assigned++;
                }
                break;
            }
            default:
                /* unknown conversion: treated as a literal to match [inf] */
                if (at(&in, ip) != t) return assigned;
                ip++;
                break;
            }
        }
    }
    return assigned;
}

SG_IMPORT(msvcr90, sscanf) {
    hle_va va = {mem, ctx->esp + 12};
    if (!A(0) || !A(1)) { hle_invalid_parameter(ctx, "sscanf", "NULL string"); hle_set_errno(mem, MSVC_EINVAL); SG_RETURN_CDECL(-1); }
    SG_RETURN_CDECL(hle_scan(mem, A(0), 0, A(1), &va));
}
SG_IMPORT(msvcr90, swscanf) {
    hle_va va = {mem, ctx->esp + 12};
    if (!A(0) || !A(1)) { hle_invalid_parameter(ctx, "swscanf", "NULL string"); hle_set_errno(mem, MSVC_EINVAL); SG_RETURN_CDECL(-1); }
    SG_RETURN_CDECL(hle_scan(mem, A(0), 1, A(1), &va));
}
