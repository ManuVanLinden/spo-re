/* w32_util.c -- strings, error mapping, time conversion and the fatal path. */
#include <errno.h>
#include <stdio.h>
#include <stdlib.h>
#include <string.h>

#include "w32_int.h"

uint8_t *w32_mem;

void w32_fatal(const char *fmt, ...) {
    va_list ap;
    fprintf(stderr, "w32 fatal: ");
    va_start(ap, fmt);
    vfprintf(stderr, fmt, ap);
    va_end(ap);
    fputc('\n', stderr);
    abort();
}

size_t w32_utf8_encode(uint32_t cp, char *o) {
    if (cp < 0x80) { o[0] = (char)cp; return 1; }
    if (cp < 0x800) { o[0] = (char)(0xc0 | (cp >> 6)); o[1] = (char)(0x80 | (cp & 0x3f)); return 2; }
    if (cp < 0x10000) {
        o[0] = (char)(0xe0 | (cp >> 12)); o[1] = (char)(0x80 | ((cp >> 6) & 0x3f)); o[2] = (char)(0x80 | (cp & 0x3f));
        return 3;
    }
    o[0] = (char)(0xf0 | (cp >> 18)); o[1] = (char)(0x80 | ((cp >> 12) & 0x3f));
    o[2] = (char)(0x80 | ((cp >> 6) & 0x3f)); o[3] = (char)(0x80 | (cp & 0x3f));
    return 4;
}

uint32_t w32_utf8_decode(const char **sp) {
    const uint8_t *s = (const uint8_t *)*sp;
    uint32_t c = s[0], n, cp;
    if (!c) return 0;
    if (c < 0x80) { *sp += 1; return c; }
    if ((c & 0xe0) == 0xc0) { n = 1; cp = c & 0x1f; }
    else if ((c & 0xf0) == 0xe0) { n = 2; cp = c & 0x0f; }
    else if ((c & 0xf8) == 0xf0) { n = 3; cp = c & 0x07; }
    else { *sp += 1; return 0xfffd; }
    for (uint32_t i = 1; i <= n; i++) {
        if ((s[i] & 0xc0) != 0x80) { *sp += i; return 0xfffd; }
        cp = (cp << 6) | (s[i] & 0x3f);
    }
    *sp += n + 1;
    return cp;
}

/* Code page 1252 bytes 0x80..0x9f (others map to themselves). */
static const uint16_t cp1252_hi[32] = {
    0x20ac, 0x0081, 0x201a, 0x0192, 0x201e, 0x2026, 0x2020, 0x2021, 0x02c6, 0x2030, 0x0160, 0x2039, 0x0152, 0x008d,
    0x017d, 0x008f, 0x0090, 0x2018, 0x2019, 0x201c, 0x201d, 0x2022, 0x2013, 0x2014, 0x02dc, 0x2122, 0x0161, 0x203a,
    0x0153, 0x009d, 0x017e, 0x0178};
uint32_t w32_cp1252_to_unicode(uint8_t b) { return (b >= 0x80 && b < 0xa0) ? cp1252_hi[b - 0x80] : b; }
int w32_unicode_to_cp1252(uint32_t cp) {
    if (cp < 0x80 || (cp >= 0xa0 && cp < 0x100)) return (int)cp;
    for (int i = 0; i < 32; i++)
        if (cp1252_hi[i] == cp) return 0x80 + i;
    return '?';
}

size_t w32_wstr_get(const uint8_t *mem, uint32_t a, char *out, size_t cap) {
    size_t n = 0;
    char tmp[4];
    if (!cap) return 0;
    for (uint32_t i = 0;; i++) {
        uint32_t u = sg_ld16(mem, a + 2 * i);
        if (!u) break;
        if (u >= 0xd800 && u < 0xdc00) {
            uint32_t v = sg_ld16(mem, a + 2 * (i + 1));
            if (v >= 0xdc00 && v < 0xe000) { u = 0x10000 + ((u - 0xd800) << 10) + (v - 0xdc00); i++; }
            else u = 0xfffd;
        } else if (u >= 0xdc00 && u < 0xe000) {
            u = 0xfffd;
        }
        size_t k = w32_utf8_encode(u, tmp);
        if (n + k + 1 > cap) break;
        memcpy(out + n, tmp, k);
        n += k;
    }
    out[n] = 0;
    return n;
}

size_t w32_astr_get(const uint8_t *mem, uint32_t a, char *out, size_t cap) {
    size_t n = 0;
    char tmp[4];
    if (!cap) return 0;
    for (uint32_t i = 0;; i++) {
        uint8_t b = mem[(uint32_t)(a + i)];
        if (!b) break;
        size_t k = w32_utf8_encode(w32_cp1252_to_unicode(b), tmp);
        if (n + k + 1 > cap) break;
        memcpy(out + n, tmp, k);
        n += k;
    }
    out[n] = 0;
    return n;
}

uint32_t w32_utf8_units(const char *s, int wide) {
    uint32_t n = 0, cp;
    while ((cp = w32_utf8_decode(&s))) n += (wide && cp >= 0x10000) ? 2 : 1;
    return n;
}

static uint32_t put_units(uint8_t *mem, uint32_t dst, uint32_t max_units, const char *s, int wide) {
    uint32_t n = 0, cp;
    while ((cp = w32_utf8_decode(&s))) {
        if (wide) {
            if (cp >= 0x10000) {
                if (n + 2 > max_units) break;
                cp -= 0x10000;
                sg_st16(mem, dst + 2 * n, (uint16_t)(0xd800 + (cp >> 10)));
                sg_st16(mem, dst + 2 * n + 2, (uint16_t)(0xdc00 + (cp & 0x3ff)));
                n += 2;
                continue;
            }
            if (n + 1 > max_units) break;
            sg_st16(mem, dst + 2 * n, (uint16_t)cp);
        } else {
            if (n + 1 > max_units) break;
            mem[(uint32_t)(dst + n)] = (uint8_t)w32_unicode_to_cp1252(cp);
        }
        n++;
    }
    return n;
}

uint32_t w32_put_str_trunc(uint8_t *mem, uint32_t dst, uint32_t cap, const char *s, int wide) {
    if (!cap) return 0;
    uint32_t n = put_units(mem, dst, cap - 1, s, wide);
    if (wide) sg_st16(mem, dst + 2 * n, 0);
    else mem[(uint32_t)(dst + n)] = 0;
    return n;
}

uint32_t w32_put_str(uint8_t *mem, uint32_t dst, uint32_t cap, const char *s, int wide, int *fits) {
    uint32_t need = w32_utf8_units(s, wide);
    if (need + 1 > cap || !dst) { if (fits) *fits = 0; return need + 1; }
    put_units(mem, dst, need, s, wide);
    if (wide) sg_st16(mem, dst + 2 * need, 0);
    else mem[(uint32_t)(dst + need)] = 0;
    if (fits) *fits = 1;
    return need;
}

uint32_t w32_err_from_errno(int e) {
    switch (e) {
    case 0: return 0;
    case ENOENT: return W32_ERROR_FILE_NOT_FOUND;
    case ENOTDIR: return W32_ERROR_PATH_NOT_FOUND;
    case EACCES: case EPERM: case EROFS: case EISDIR: return W32_ERROR_ACCESS_DENIED;
    case EEXIST: return W32_ERROR_ALREADY_EXISTS;
    case EMFILE: case ENFILE: return W32_ERROR_TOO_MANY_OPEN_FILES;
    case ENOSPC: return W32_ERROR_DISK_FULL;
    case ENOTEMPTY: return W32_ERROR_DIR_NOT_EMPTY;
    case ENAMETOOLONG: return W32_ERROR_INVALID_NAME;
    case EBADF: return W32_ERROR_INVALID_HANDLE;
    case EINVAL: return W32_ERROR_INVALID_PARAMETER;
    case ENOMEM: return W32_ERROR_NOT_ENOUGH_MEMORY;
    case EXDEV: return W32_ERROR_NOT_SAME_DEVICE;
    default: return W32_ERROR_ACCESS_DENIED;
    }
}

int w32_msvc_errno_from_errno(int e) {
    switch (e) {
    case ENOENT: case ENOTDIR: return MSVC_ENOENT;
    case EACCES: case EPERM: case EROFS: case EISDIR: return MSVC_EACCES;
    case EEXIST: return MSVC_EEXIST;
    case EMFILE: case ENFILE: return MSVC_EMFILE;
    case ENOSPC: return MSVC_ENOSPC;
    case EBADF: return MSVC_EBADF;
    case ENOMEM: return MSVC_ENOMEM;
    case ENOTEMPTY: return MSVC_ENOTEMPTY;
    case ENAMETOOLONG: return MSVC_ENAMETOOLONG;
    default: return MSVC_EINVAL;
    }
}

/* FILETIME: 100 ns ticks since 1601-01-01 UTC; 11644473600 s between 1601 and 1970. */
uint64_t w32_filetime_from_unix(int64_t sec, long nsec) {
    return (uint64_t)(sec + 11644473600ll) * 10000000ull + (uint64_t)(nsec / 100);
}
