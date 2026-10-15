/* k32_nls.c -- MultiByteToWideChar / WideCharToMultiByte / GetACP over UTF-8 and code page 1252.
 *
 * [obs] The game's MultiByteToWideChar/WideCharToMultiByte calls pass CP_UTF8 (0xfde9) as an
 * immediate at 0x9317cc, 0x931dc0, 0x9395f2, 0x931837, 0x93801c; the two others (0x93cdd0,
 * 0x93ce29) take the code page from a register, fed by GetACP (0x93c956..). So UTF-8 and the
 * ANSI code page are what matter; GetACP reports 1252.
 *
 * Win32 contract (MultiByteToWideChar / WideCharToMultiByte reference pages):
 *  - cbMultiByte / cchWideChar == -1: the input is NUL-terminated and the NUL is converted
 *    and counted; 0 is ERROR_INVALID_PARAMETER.
 *  - output size 0: return the required size, write nothing.
 *  - output too small: return 0, ERROR_INSUFFICIENT_BUFFER (122).
 *  - MB_ERR_INVALID_CHARS (8): an invalid sequence fails with ERROR_NO_UNICODE_TRANSLATION
 *    (1113); without it, invalid UTF-8 becomes U+FFFD (Vista and later).
 *  - CP_UTF8 accepts only dwFlags 0 or MB_ERR_INVALID_CHARS (ERROR_INVALID_FLAGS, 1004), and
 *    WideCharToMultiByte requires lpDefaultChar and lpUsedDefaultChar NULL for it.
 * Deviation: no "best fit" mapping for 1252 (Windows maps e.g. U+0101 to 'a'); unmapped
 * characters become the default character '?'.
 */
#include <stdlib.h>
#include <string.h>

#include "hle_imports.h"

#define A(i) SG_ARG(i)

unsigned hle_acp = 1252;

static const uint16_t cp1252_hi[32] = {
    0x20ac, 0x0081, 0x201a, 0x0192, 0x201e, 0x2026, 0x2020, 0x2021, 0x02c6, 0x2030, 0x0160, 0x2039, 0x0152, 0x008d, 0x017d, 0x008f,
    0x0090, 0x2018, 0x2019, 0x201c, 0x201d, 0x2022, 0x2013, 0x2014, 0x02dc, 0x2122, 0x0161, 0x203a, 0x0153, 0x009d, 0x017e, 0x0178};

static unsigned resolve_cp(unsigned cp) {
    if (cp == HLE_CP_ACP || cp == HLE_CP_THREAD_ACP) return hle_acp;
    if (cp == HLE_CP_OEMCP) return 0; /* OEM 437 not provided */
    return cp;
}

long hle_mb_to_utf16(unsigned cp, const uint8_t *s, size_t n, uint16_t *d, size_t cap, int *invalid) {
    size_t i = 0, o = 0;
    cp = resolve_cp(cp);
    *invalid = 0;
#define OUT(u) do { if (d && o < cap) d[o] = (uint16_t)(u); o++; } while (0)
    if (cp == HLE_CP_1252 || cp == HLE_CP_ASCII) {
        for (; i < n; i++) {
            uint8_t b = s[i];
            if (cp == HLE_CP_ASCII && b >= 0x80) { *invalid = 1; OUT(b & 0x7f); continue; }
            OUT(b >= 0x80 && b < 0xa0 ? cp1252_hi[b - 0x80] : b);
        }
        return (long)o;
    }
    if (cp != HLE_CP_UTF8) return -1;
    while (i < n) {
        uint8_t b = s[i];
        uint32_t cpv, min;
        int len, k;
        if (b < 0x80) { OUT(b); i++; continue; }
        if (b >= 0xc2 && b <= 0xdf) { len = 2; cpv = b & 0x1f; min = 0x80; }
        else if (b >= 0xe0 && b <= 0xef) { len = 3; cpv = b & 0x0f; min = 0x800; }
        else if (b >= 0xf0 && b <= 0xf4) { len = 4; cpv = b & 0x07; min = 0x10000; }
        else { *invalid = 1; OUT(0xfffd); i++; continue; }
        for (k = 1; k < len; k++) {
            if (i + (size_t)k >= n || (s[i + (size_t)k] & 0xc0) != 0x80) break;
            cpv = (cpv << 6) | (s[i + (size_t)k] & 0x3f);
        }
        if (k < len || cpv < min || cpv > 0x10ffff || (cpv >= 0xd800 && cpv < 0xe000)) {
            /* maximal invalid prefix becomes one U+FFFD [inf: Windows 10 behaviour] */
            *invalid = 1;
            OUT(0xfffd);
            i += (size_t)(k < len ? k : len);
            continue;
        }
        if (cpv >= 0x10000) { cpv -= 0x10000; OUT(0xd800 | (cpv >> 10)); OUT(0xdc00 | (cpv & 0x3ff)); }
        else OUT(cpv);
        i += (size_t)len;
    }
    return (long)o;
#undef OUT
}

long hle_utf16_to_mb(unsigned cp, const uint16_t *s, size_t n, uint8_t *d, size_t cap, int *used_default, uint8_t defc) {
    size_t i = 0, o = 0;
    cp = resolve_cp(cp);
    *used_default = 0;
#define OUT(b) do { if (d && o < cap) d[o] = (uint8_t)(b); o++; } while (0)
    if (cp == HLE_CP_1252 || cp == HLE_CP_ASCII) {
        for (; i < n; i++) {
            uint16_t u = s[i];
            int j, found = -1;
            if (u < 0x80 || (cp == HLE_CP_1252 && u >= 0xa0 && u <= 0xff)) { OUT(u); continue; }
            if (cp == HLE_CP_1252)
                for (j = 0; j < 32; j++)
                    if (cp1252_hi[j] == u) { found = 0x80 + j; break; }
            if (found >= 0) OUT(found);
            else { *used_default = 1; OUT(defc); }
        }
        return (long)o;
    }
    if (cp != HLE_CP_UTF8) return -1;
    for (; i < n; i++) {
        uint32_t c = s[i];
        if (c >= 0xd800 && c < 0xdc00 && i + 1 < n && s[i + 1] >= 0xdc00 && s[i + 1] < 0xe000) {
            c = 0x10000 + ((c - 0xd800) << 10) + (s[i + 1] - 0xdc00u);
            i++;
        } else if (c >= 0xd800 && c < 0xe000) {
            c = 0xfffd; /* unpaired surrogate */
            *used_default = 1;
        }
        if (c < 0x80) OUT(c);
        else if (c < 0x800) { OUT(0xc0 | (c >> 6)); OUT(0x80 | (c & 0x3f)); }
        else if (c < 0x10000) { OUT(0xe0 | (c >> 12)); OUT(0x80 | ((c >> 6) & 0x3f)); OUT(0x80 | (c & 0x3f)); }
        else { OUT(0xf0 | (c >> 18)); OUT(0x80 | ((c >> 12) & 0x3f)); OUT(0x80 | ((c >> 6) & 0x3f)); OUT(0x80 | (c & 0x3f)); }
    }
    return (long)o;
#undef OUT
}

enum { ERROR_INVALID_PARAMETER = 87, ERROR_INSUFFICIENT_BUFFER = 122, ERROR_INVALID_FLAGS = 1004,
       ERROR_NO_UNICODE_TRANSLATION = 1113, MB_ERR_INVALID_CHARS = 8 };

/* int MultiByteToWideChar(UINT cp, DWORD flags, LPCSTR src, int cb, LPWSTR dst, int cch) */
SG_IMPORT(kernel32, MultiByteToWideChar) {
    uint32_t cp = A(0), flags = A(1), src = A(2), dst = A(4);
    int32_t cb = (int32_t)A(3), cch = (int32_t)A(5);
    long need, r;
    int invalid;
    uint16_t *tmp;
    if (!src || cb == 0 || cch < 0 || (cch && !dst) || (dst && dst == src)) {
        hle_set_last_error(ctx, mem, ERROR_INVALID_PARAMETER);
        SG_RETURN_STDCALL(0, 24);
    }
    if (resolve_cp(cp) == HLE_CP_UTF8 && (flags & ~(uint32_t)MB_ERR_INVALID_CHARS)) {
        hle_set_last_error(ctx, mem, ERROR_INVALID_FLAGS);
        SG_RETURN_STDCALL(0, 24);
    }
    if (cb < 0) cb = (int32_t)hle_strlen(mem, src) + 1;
    need = hle_mb_to_utf16(cp, mem + src, (size_t)cb, NULL, 0, &invalid);
    if (need < 0) {
        hle_log(HLE_LOG_WARN, "MultiByteToWideChar: code page %u not supported", cp);
        hle_set_last_error(ctx, mem, ERROR_INVALID_PARAMETER);
        SG_RETURN_STDCALL(0, 24);
    }
    if (invalid && (flags & MB_ERR_INVALID_CHARS)) {
        hle_set_last_error(ctx, mem, ERROR_NO_UNICODE_TRANSLATION);
        SG_RETURN_STDCALL(0, 24);
    }
    if (!cch) SG_RETURN_STDCALL(need, 24);
    tmp = (uint16_t *)malloc((size_t)need * 2 + 2);
    if (!tmp) abort();
    r = hle_mb_to_utf16(cp, mem + src, (size_t)cb, tmp, (size_t)need, &invalid);
    if (r > cch) {
        memcpy(mem + dst, tmp, (size_t)cch * 2); /* Windows fills what fits before failing */
        free(tmp);
        hle_set_last_error(ctx, mem, ERROR_INSUFFICIENT_BUFFER);
        SG_RETURN_STDCALL(0, 24);
    }
    memcpy(mem + dst, tmp, (size_t)r * 2);
    free(tmp);
    SG_RETURN_STDCALL(r, 24);
}

/* int WideCharToMultiByte(UINT cp, DWORD flags, LPCWSTR src, int cch, LPSTR dst, int cb,
 *                         LPCSTR defchar, LPBOOL used_default) */
SG_IMPORT(kernel32, WideCharToMultiByte) {
    uint32_t cp = A(0), src = A(2), dst = A(4), defp = A(6), usedp = A(7);
    int32_t cch = (int32_t)A(3), cb = (int32_t)A(5);
    long need, r;
    int used;
    uint8_t defc = defp ? mem[defp] : '?';
    uint16_t *in;
    uint8_t *tmp;
    if (!src || cch == 0 || cb < 0 || (cb && !dst)) {
        hle_set_last_error(ctx, mem, ERROR_INVALID_PARAMETER);
        SG_RETURN_STDCALL(0, 32);
    }
    if (resolve_cp(cp) == HLE_CP_UTF8 && (defp || usedp)) {
        hle_set_last_error(ctx, mem, ERROR_INVALID_PARAMETER);
        SG_RETURN_STDCALL(0, 32);
    }
    if (cch < 0) cch = (int32_t)hle_wcslen(mem, src) + 1;
    in = (uint16_t *)malloc((size_t)cch * 2);
    if (!in) abort();
    memcpy(in, mem + src, (size_t)cch * 2); /* the guest pointer may be unaligned */
    need = hle_utf16_to_mb(cp, in, (size_t)cch, NULL, 0, &used, defc);
    if (need < 0) {
        free(in);
        hle_log(HLE_LOG_WARN, "WideCharToMultiByte: code page %u not supported", cp);
        hle_set_last_error(ctx, mem, ERROR_INVALID_PARAMETER);
        SG_RETURN_STDCALL(0, 32);
    }
    if (usedp) sg_st32(mem, usedp, (uint32_t)used);
    if (!cb) { free(in); SG_RETURN_STDCALL(need, 32); }
    tmp = (uint8_t *)malloc((size_t)need + 1);
    if (!tmp) abort();
    r = hle_utf16_to_mb(cp, in, (size_t)cch, tmp, (size_t)need, &used, defc);
    free(in);
    if (r > cb) {
        memcpy(mem + dst, tmp, (size_t)cb);
        free(tmp);
        hle_set_last_error(ctx, mem, ERROR_INSUFFICIENT_BUFFER);
        SG_RETURN_STDCALL(0, 32);
    }
    memcpy(mem + dst, tmp, (size_t)r);
    free(tmp);
    SG_RETURN_STDCALL(r, 32);
}

SG_IMPORT(kernel32, GetACP) { SG_RETURN_STDCALL(hle_acp, 0); }
