/* crt_string.c -- msvcr90 memory, string, ctype and UTF-16 wcs* shims ("C" locale rules).
 *
 * All pointers are guest addresses; every byte access goes through `mem`. Behaviour follows
 * the msvcr90 sources' documented contracts; differences from the C standard are noted.
 * [obs] The game never sets LC_CTYPE (setlocale calls use categories 1, 3, 4, 5 at
 * 0x87fab3..0x87faeb), so the "C" locale paths below are the ones msvcr90 takes.
 */
#include <locale.h>
#include <string.h>
#include <wctype.h>
#include <xlocale.h>

#include "hle_imports.h"

#define A(i) SG_ARG(i)
#define B(a) (mem[(uint32_t)(a)])
#define W(a) sg_ld16(mem, (uint32_t)(a))

/* ---- ctype tables ------------------------------------------------------------------- */

unsigned hle_ctype(int c) {
    if (c < -1 || c > 255) return 0;
    if (c == -1 || c >= 128) return 0; /* EOF and the upper half: nothing in the "C" locale */
    if (c < 0x20 || c == 0x7f) return HLE_CONTROL | ((c >= 9 && c <= 13) ? HLE_SPACE : 0);  /* tab: no _BLANK, else isprint('\t') would be true */
    if (c == ' ') return HLE_SPACE | HLE_BLANK;
    if (c >= '0' && c <= '9') return HLE_DIGIT | HLE_HEX;
    if (c >= 'A' && c <= 'Z') return HLE_UPPER | (c <= 'F' ? HLE_HEX : 0);
    if (c >= 'a' && c <= 'z') return HLE_LOWER | (c <= 'f' ? HLE_HEX : 0);
    return HLE_PUNCT;
}

static locale_t host_utf8(void) {
    static locale_t l;
    if (!l) l = newlocale(LC_CTYPE_MASK, "en_US.UTF-8", 0);
    return l;
}

unsigned hle_wctype(unsigned c) {
    unsigned r;
    c &= 0xffffu;
    if (c < 128) {
        r = hle_ctype((int)c);
        if (r & (HLE_UPPER | HLE_LOWER)) r |= 0x100; /* wide table carries C1_ALPHA */
        return r;
    }
    if (c < 0xa0) return HLE_CONTROL;
    if (c < 0x100) { /* Latin-1, as GetStringTypeW classifies it [inf] */
        if (c == 0xa0) return HLE_SPACE | HLE_BLANK;
        if (c == 0xaa || c == 0xb5 || c == 0xba) return HLE_LOWER | 0x100;
        if (c == 0xb2 || c == 0xb3 || c == 0xb9) return HLE_DIGIT | HLE_PUNCT;
        if (c < 0xc0 || c == 0xd7 || c == 0xf7) return HLE_PUNCT;
        return (c < 0xdf ? HLE_UPPER : HLE_LOWER) | 0x100;
    }
    if (c >= 0xd800 && c < 0xe000) return 0; /* surrogate halves: no class */
    {
        locale_t l = host_utf8();
        wint_t w = (wint_t)c;
        r = 0;
        if (iswupper_l(w, l)) r |= HLE_UPPER;
        if (iswlower_l(w, l)) r |= HLE_LOWER;
        if (iswalpha_l(w, l)) r |= 0x100;
        if (iswdigit_l(w, l)) r |= HLE_DIGIT;
        if (iswspace_l(w, l)) r |= HLE_SPACE;
        if (iswpunct_l(w, l)) r |= HLE_PUNCT;
        if (iswcntrl_l(w, l)) r |= HLE_CONTROL;
        if (iswblank_l(w, l)) r |= HLE_BLANK;
        return r;
    }
}

#define CTYPE_SHIM(fn, mask) \
    SG_IMPORT(msvcr90, fn) { SG_RETURN_CDECL(hle_ctype((int)A(0)) & (mask)); }
CTYPE_SHIM(isupper, HLE_UPPER)
CTYPE_SHIM(isdigit, HLE_DIGIT)
CTYPE_SHIM(isspace, HLE_SPACE)
CTYPE_SHIM(isalnum, HLE_ALPHA | HLE_DIGIT)
CTYPE_SHIM(isalpha, HLE_ALPHA)
CTYPE_SHIM(isxdigit, HLE_HEX)
CTYPE_SHIM(isprint, HLE_UPPER | HLE_LOWER | HLE_DIGIT | HLE_PUNCT | HLE_BLANK)

/* isprint uses msvcr90's mask _BLANK|_PUNCT|_ALPHA|_DIGIT; only ' ' has _BLANK here. */
SG_IMPORT(msvcr90, tolower) { SG_RETURN_CDECL((int32_t)hle_ascii_tolower(A(0))); }
SG_IMPORT(msvcr90, toupper) { SG_RETURN_CDECL((int32_t)hle_ascii_toupper(A(0))); }
SG_IMPORT(msvcr90, towlower) { uint32_t c = A(0) & 0xffffu; SG_RETURN_CDECL(hle_ascii_tolower(c)); }
SG_IMPORT(msvcr90, towupper) { uint32_t c = A(0) & 0xffffu; SG_RETURN_CDECL(hle_ascii_toupper(c)); }
SG_IMPORT(msvcr90, iswctype) {
    uint32_t c = A(0) & 0xffffu, m = A(1) & 0xffffu;
    if (c == 0xffffu) SG_RETURN_CDECL(0); /* WEOF */
    SG_RETURN_CDECL(hle_wctype(c) & m);
}

/* ---- memory ------------------------------------------------------------------------- */

/* msvcr90's x86 memcpy shares memmove's overlap-safe code [inf]; use memmove for both. */
SG_IMPORT(msvcr90, memcpy) {
    uint32_t d = A(0), s = A(1), n = A(2);
    if (n) memmove(mem + d, mem + s, n);
    SG_RETURN_CDECL(d);
}
SG_IMPORT(msvcr90, memmove) {
    uint32_t d = A(0), s = A(1), n = A(2);
    if (n) memmove(mem + d, mem + s, n);
    SG_RETURN_CDECL(d);
}
SG_IMPORT(msvcr90, memset) {
    uint32_t d = A(0), n = A(2);
    if (n) memset(mem + d, (int)(A(1) & 0xff), n);
    SG_RETURN_CDECL(d);
}
SG_IMPORT(msvcr90, memchr) {
    uint32_t s = A(0), c = A(1) & 0xff, n = A(2), i;
    for (i = 0; i < n; i++)
        if (B(s + i) == c) SG_RETURN_CDECL(s + i);
    SG_RETURN_CDECL(0);
}

/* ---- narrow strings ----------------------------------------------------------------- */

/* strcmp: msvcr90's x86 assembly returns -1, 0 or +1 [inf]. */
SG_IMPORT(msvcr90, strcmp) {
    uint32_t a = A(0), b = A(1);
    for (;; a++, b++) {
        uint8_t x = B(a), y = B(b);
        if (x != y) SG_RETURN_CDECL(x < y ? -1 : 1);
        if (!x) SG_RETURN_CDECL(0);
    }
}
/* strncmp: difference of the first mismatching bytes (as unsigned char). */
SG_IMPORT(msvcr90, strncmp) {
    uint32_t a = A(0), b = A(1), n = A(2), i;
    for (i = 0; i < n; i++) {
        uint8_t x = B(a + i), y = B(b + i);
        if (x != y) SG_RETURN_CDECL((int32_t)x - (int32_t)y);
        if (!x) break;
    }
    SG_RETURN_CDECL(0);
}
/* _stricmp/_strnicmp in the "C" locale fold A-Z to lower case and return the difference. */
SG_IMPORT(msvcr90, _stricmp) {
    uint32_t a = A(0), b = A(1);
    if (!a || !b) { hle_invalid_parameter(ctx, "_stricmp", "NULL string"); hle_set_errno(mem, MSVC_EINVAL); SG_RETURN_CDECL(0x7fffffff); }
    for (;; a++, b++) {
        int x = (int)hle_ascii_tolower(B(a)), y = (int)hle_ascii_tolower(B(b));
        if (x != y || !x) SG_RETURN_CDECL(x - y);
    }
}
SG_IMPORT(msvcr90, _strnicmp) {
    uint32_t a = A(0), b = A(1), n = A(2), i;
    if (!n) SG_RETURN_CDECL(0);
    if (!a || !b) { hle_invalid_parameter(ctx, "_strnicmp", "NULL string"); hle_set_errno(mem, MSVC_EINVAL); SG_RETURN_CDECL(0x7fffffff); }
    for (i = 0; i < n; i++) {
        int x = (int)hle_ascii_tolower(B(a + i)), y = (int)hle_ascii_tolower(B(b + i));
        if (x != y || !x) SG_RETURN_CDECL(x - y);
    }
    SG_RETURN_CDECL(0);
}
SG_IMPORT(msvcr90, strncpy) {
    uint32_t d = A(0), s = A(1), n = A(2), i = 0;
    for (; i < n && B(s + i); i++) B(d + i) = B(s + i);
    for (; i < n; i++) B(d + i) = 0;
    SG_RETURN_CDECL(d);
}
SG_IMPORT(msvcr90, strncat) {
    uint32_t d = A(0), s = A(1), n = A(2), e = d + hle_strlen(mem, d), i = 0;
    for (; i < n && B(s + i); i++) B(e + i) = B(s + i);
    B(e + i) = 0;
    SG_RETURN_CDECL(d);
}
SG_IMPORT(msvcr90, strchr) {
    uint32_t s = A(0);
    uint8_t c = (uint8_t)A(1);
    for (;; s++) {
        if (B(s) == c) SG_RETURN_CDECL(s);
        if (!B(s)) SG_RETURN_CDECL(0);
    }
}
SG_IMPORT(msvcr90, strrchr) {
    uint32_t s = A(0), r = 0;
    uint8_t c = (uint8_t)A(1);
    for (;; s++) {
        if (B(s) == c) r = s;
        if (!B(s)) SG_RETURN_CDECL(r);
    }
}
SG_IMPORT(msvcr90, strstr) {
    uint32_t h = A(0), n = A(1), nl = hle_strlen(mem, n);
    if (!nl) SG_RETURN_CDECL(h);
    for (; B(h); h++)
        if (B(h) == B(n) && !memcmp(mem + h, mem + n, nl)) SG_RETURN_CDECL(h);
    SG_RETURN_CDECL(0);
}
static int in_set(const uint8_t *mem, uint32_t set, uint8_t c) {
    for (; B(set); set++)
        if (B(set) == c) return 1;
    return 0;
}
SG_IMPORT(msvcr90, strpbrk) {
    uint32_t s = A(0), set = A(1);
    for (; B(s); s++)
        if (in_set(mem, set, B(s))) SG_RETURN_CDECL(s);
    SG_RETURN_CDECL(0);
}
SG_IMPORT(msvcr90, strspn) {
    uint32_t s = A(0), set = A(1), n = 0;
    while (B(s + n) && in_set(mem, set, B(s + n))) n++;
    SG_RETURN_CDECL(n);
}
SG_IMPORT(msvcr90, strcspn) {
    uint32_t s = A(0), set = A(1), n = 0;
    while (B(s + n) && !in_set(mem, set, B(s + n))) n++;
    SG_RETURN_CDECL(n);
}
SG_IMPORT(msvcr90, _strlwr) {
    uint32_t s = A(0), p;
    for (p = s; B(p); p++) B(p) = (uint8_t)hle_ascii_tolower(B(p));
    SG_RETURN_CDECL(s);
}

/* ---- UTF-16 strings (wchar_t is 2 bytes in the guest) ------------------------------- */

SG_IMPORT(msvcr90, wcsncmp) {
    uint32_t a = A(0), b = A(1), n = A(2), i;
    for (i = 0; i < n; i++) {
        uint16_t x = W(a + 2 * i), y = W(b + 2 * i);
        if (x != y) SG_RETURN_CDECL((int32_t)x - (int32_t)y);
        if (!x) break;
    }
    SG_RETURN_CDECL(0);
}
SG_IMPORT(msvcr90, _wcsicmp) {
    uint32_t a = A(0), b = A(1);
    if (!a || !b) { hle_invalid_parameter(ctx, "_wcsicmp", "NULL string"); hle_set_errno(mem, MSVC_EINVAL); SG_RETURN_CDECL(0x7fffffff); }
    for (;; a += 2, b += 2) {
        int x = (int)hle_ascii_tolower(W(a)), y = (int)hle_ascii_tolower(W(b));
        if (x != y || !x) SG_RETURN_CDECL(x - y);
    }
}
SG_IMPORT(msvcr90, _wcsnicmp) {
    uint32_t a = A(0), b = A(1), n = A(2), i;
    if (!n) SG_RETURN_CDECL(0);
    if (!a || !b) { hle_invalid_parameter(ctx, "_wcsnicmp", "NULL string"); hle_set_errno(mem, MSVC_EINVAL); SG_RETURN_CDECL(0x7fffffff); }
    for (i = 0; i < n; i++) {
        int x = (int)hle_ascii_tolower(W(a + 2 * i)), y = (int)hle_ascii_tolower(W(b + 2 * i));
        if (x != y || !x) SG_RETURN_CDECL(x - y);
    }
    SG_RETURN_CDECL(0);
}
SG_IMPORT(msvcr90, wcsncpy) {
    uint32_t d = A(0), s = A(1), n = A(2), i = 0;
    for (; i < n && W(s + 2 * i); i++) sg_st16(mem, d + 2 * i, W(s + 2 * i));
    for (; i < n; i++) sg_st16(mem, d + 2 * i, 0);
    SG_RETURN_CDECL(d);
}
SG_IMPORT(msvcr90, wcsncat) {
    uint32_t d = A(0), s = A(1), n = A(2), e = d + 2 * hle_wcslen(mem, d), i = 0;
    for (; i < n && W(s + 2 * i); i++) sg_st16(mem, e + 2 * i, W(s + 2 * i));
    sg_st16(mem, e + 2 * i, 0);
    SG_RETURN_CDECL(d);
}
SG_IMPORT(msvcr90, wcschr) {
    uint32_t s = A(0);
    uint16_t c = (uint16_t)A(1);
    for (;; s += 2) {
        if (W(s) == c) SG_RETURN_CDECL(s);
        if (!W(s)) SG_RETURN_CDECL(0);
    }
}
SG_IMPORT(msvcr90, wcsrchr) {
    uint32_t s = A(0), r = 0;
    uint16_t c = (uint16_t)A(1);
    for (;; s += 2) {
        if (W(s) == c) r = s;
        if (!W(s)) SG_RETURN_CDECL(r);
    }
}
SG_IMPORT(msvcr90, wcsstr) {
    uint32_t h = A(0), n = A(1), nl = hle_wcslen(mem, n);
    if (!nl) SG_RETURN_CDECL(h);
    for (; W(h); h += 2)
        if (!memcmp(mem + h, mem + n, 2u * nl)) SG_RETURN_CDECL(h);
    SG_RETURN_CDECL(0);
}
SG_IMPORT(msvcr90, wcscspn) {
    uint32_t s = A(0), set = A(1), n = 0;
    for (; W(s + 2 * n); n++) {
        uint32_t p;
        for (p = set; W(p); p += 2)
            if (W(p) == W(s + 2 * n)) SG_RETURN_CDECL(n);
    }
    SG_RETURN_CDECL(n);
}
SG_IMPORT(msvcr90, _wcslwr) {
    uint32_t s = A(0), p;
    for (p = s; W(p); p += 2) sg_st16(mem, p, (uint16_t)hle_ascii_tolower(W(p)));
    SG_RETURN_CDECL(s);
}
