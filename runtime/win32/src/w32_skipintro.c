/* w32_skipintro.c -- SPORE_SKIP_INTRO=1: the title movie's own "off" switch, served by the file layer.
 *
 * The title sequence plays the intro movie only when the application property 0xC14680EF is true: the
 * title screen's movie step (0xdeabd0, reached from 0xdeb060) reads it from the app property list
 * [0x15fd918] and, when it is false, takes the path the game already has for "no movie" (0xdea510 closes
 * the title layout, phase 6, message 0x66f8f25), the same end state the movie's natural end reaches after
 * the GGE_intro zoom (0xdea92f). The property is data, not code: it is the bool `true` in the application
 * properties resource (.prop type 0x00B1B104, group 0, instance 0x08EA246B) of the shipped packages, and
 * the exe reads the id at one place only (0xdeabf6).
 *
 * With the switch on, each package the game opens for reading is checked once: its DBPF index is read,
 * the application properties resource is located, decompressed (RefPack) with a record of where each
 * output byte came from, and the property record `C1 46 80 EF 00 01 00 00 01` (id, type bool, no flags,
 * true) is found exactly once. When its value byte is a literal of the compressed stream that no later
 * back-reference copies (true in all three shipped copies), the file offset of that literal is stored on
 * the handle, and ReadFile returns 0 instead of 1 at that one offset. Nothing else in any file changes;
 * the files on disk are never written. Any other shape (property absent, value not a lone literal,
 * damaged index) leaves the package untouched and logs why.
 *
 * Default off (unset or "0"): no package is inspected and every read is the file's bytes.
 * DEVIATIONS 2.14; docs/design/win32-layer.md §7.6.
 */
#include <errno.h>
#include <pthread.h>
#include <stdint.h>
#include <stdlib.h>
#include <string.h>
#include <strings.h>
#include <sys/stat.h>
#include <unistd.h>

#include "w32_int.h"

#define PROP_TYPE 0x00B1B104u
#define APP_PROPS_GROUP 0u
#define APP_PROPS_INSTANCE 0x08EA246Bu
#define MAX_INDEX (64u << 20)      /* the largest shipped index is a few MB */
#define MAX_RESOURCE (1u << 20)    /* the application properties are about 5 KB */

static const uint8_t k_record[8] = { 0xC1, 0x46, 0x80, 0xEF, 0x00, 0x01, 0x00, 0x00 }; /* id, bool, flags 0 */

int w32_skip_intro_enabled(void) {
    const char *e = getenv("SPORE_SKIP_INTRO");
    return e && *e && strcmp(e, "0") != 0;
}

static int read_at(int fd, void *buf, size_t n, off_t off) {
    size_t done = 0;
    while (done < n) {
        ssize_t k = pread(fd, (uint8_t *)buf + done, n - done, off + (off_t)done);
        if (k < 0 && errno == EINTR) continue;
        if (k <= 0) return -1;
        done += (size_t)k;
    }
    return 0;
}

static uint32_t le32(const uint8_t *p) { return p[0] | p[1] << 8 | p[2] << 16 | (uint32_t)p[3] << 24; }

/* RefPack decode that records, for each output byte, the input index of its literal or -1 when it was
 * produced by a back-reference; `copied[k]` is set when output byte k is the source of a later copy.
 * Returns the output length, or -1 on a malformed stream. */
static long refpack_track(const uint8_t *src, size_t n, uint8_t *out, size_t cap, int32_t *origin, uint8_t *copied) {
    if (n < 2 || src[1] != 0xFB) return -1;
    size_t i = 2, o = 0;
    int big = src[0] & 0x80;
    size_t w = big ? 4 : 3;
    if (src[0] & 0x01) i += w;
    i += w; /* uncompressed size (big endian); the loop below is bounded by cap instead */
    while (i < n) {
        uint32_t b0 = src[i], plain, cnt, off;
        if (b0 < 0x80) {
            if (i + 2 > n) return -1;
            uint32_t b1 = src[i + 1]; i += 2;
            plain = b0 & 3; cnt = ((b0 >> 2) & 7) + 3; off = ((b0 & 0x60) << 3) + b1 + 1;
        } else if (b0 < 0xC0) {
            if (i + 3 > n) return -1;
            uint32_t b1 = src[i + 1], b2 = src[i + 2]; i += 3;
            plain = b1 >> 6; cnt = (b0 & 0x3F) + 4; off = ((b1 & 0x3F) << 8) + b2 + 1;
        } else if (b0 < 0xE0) {
            if (i + 4 > n) return -1;
            uint32_t b1 = src[i + 1], b2 = src[i + 2], b3 = src[i + 3]; i += 4;
            plain = b0 & 3; cnt = ((b0 & 0x0C) << 6) + b3 + 5; off = ((b0 & 0x10) << 12) + (b1 << 8) + b2 + 1;
        } else if (b0 < 0xFC) {
            plain = ((b0 & 0x1F) << 2) + 4; cnt = 0; off = 0; i += 1;
        } else {
            plain = b0 & 3; cnt = 0; off = 0; i += 1;
        }
        if (i + plain > n || o + plain + cnt > cap) return -1;
        for (uint32_t k = 0; k < plain; k++) { out[o] = src[i + k]; origin[o] = (int32_t)(i + k); o++; }
        i += plain;
        if (cnt && off > o) return -1;
        for (uint32_t k = 0; k < cnt; k++) {
            size_t from = o - off;
            copied[from] = 1;
            out[o] = out[from];
            origin[o] = -1;
            o++;
        }
        if (b0 >= 0xFC) break;
    }
    return (long)o;
}

/* Finds the value byte of the property inside a resource. For a compressed resource the byte must be a
 * literal never copied; *why gets the reason on failure. Returns the offset within the stored bytes, or -1. */
static long locate_in_resource(const uint8_t *raw, size_t n, int compressed, size_t memsize, const char **why) {
    uint8_t *dec = NULL, *copied = NULL;
    int32_t *origin = NULL;
    const uint8_t *buf = raw;
    size_t len = n;
    long res = -1;
    if (compressed) {
        size_t cap = memsize ? memsize : MAX_RESOURCE;
        if (cap > MAX_RESOURCE) { *why = "resource too large"; return -1; }
        dec = (uint8_t *)malloc(cap);
        origin = (int32_t *)malloc(cap * sizeof *origin);
        copied = (uint8_t *)calloc(cap, 1);
        if (!dec || !origin || !copied) { *why = "out of memory"; goto out; }
        long got = refpack_track(raw, n, dec, cap, origin, copied);
        if (got < 0) { *why = "RefPack stream not understood"; goto out; }
        buf = dec;
        len = (size_t)got;
    }
    long at = -1;
    for (size_t k = 0; k + sizeof k_record < len; k++) {
        if (memcmp(buf + k, k_record, sizeof k_record)) continue;
        if (at >= 0) { *why = "property record found twice"; goto out; }
        at = (long)(k + sizeof k_record);
    }
    if (at < 0) { *why = "property record not found"; goto out; }
    if (buf[at] != 1) { *why = buf[at] ? "property value not 1" : "property already false"; goto out; }
    if (!compressed) { res = at; goto out; }
    if (origin[at] < 0) { *why = "value byte comes from a back-reference"; goto out; }
    if (copied[at]) { *why = "value byte is copied later in the stream"; goto out; }
    res = origin[at];
out:
    free(dec);
    free(origin);
    free(copied);
    return res;
}

/* Scans an open package. Returns the absolute file offset of the byte to serve as 0, or -1 when the package
 * has no application properties resource or its shape is not the one described above. */
int64_t w32_skip_intro_scan(int fd, const char *host) {
    uint8_t h[96];
    if (read_at(fd, h, sizeof h, 0) || memcmp(h, "DBPF", 4)) return -1;
    uint32_t count = le32(h + 0x24), isize = le32(h + 0x2C), ioff = le32(h + 0x40);
    if (!count || isize < 4 || isize > MAX_INDEX) return -1;
    uint8_t *idx = (uint8_t *)malloc(isize);
    if (!idx) return -1;
    int64_t result = -1;
    const char *why = NULL;
    if (read_at(fd, idx, isize, ioff)) { why = "index unreadable"; goto done; }
    {
        uint32_t flags = le32(idx), p = 4, cst[3] = { 0, 0, 0 };
        for (int b = 0; b < 3; b++)
            if (flags & (1u << b)) {
                if (p + 4 > isize) { why = "index truncated"; goto done; }
                cst[b] = le32(idx + p); p += 4;
            }
        for (uint32_t e = 0; e < count; e++) {
            uint32_t f[3];
            for (int b = 0; b < 3; b++) {
                if (flags & (1u << b)) f[b] = cst[b];
                else { if (p + 4 > isize) { why = "index truncated"; goto done; } f[b] = le32(idx + p); p += 4; }
            }
            if (p + 20 > isize) { why = "index truncated"; goto done; }
            uint32_t inst = le32(idx + p), off = le32(idx + p + 4), size = le32(idx + p + 8) & 0x7FFFFFFFu,
                     memsize = le32(idx + p + 12);
            int compressed = (idx[p + 16] | idx[p + 17] << 8) == 0xFFFF;
            p += 20;
            if (f[0] != PROP_TYPE || f[1] != APP_PROPS_GROUP || inst != APP_PROPS_INSTANCE) continue;
            if (!size || size > MAX_RESOURCE) { why = "resource size out of range"; goto done; }
            uint8_t *raw = (uint8_t *)malloc(size);
            if (!raw) { why = "out of memory"; goto done; }
            if (read_at(fd, raw, size, off)) { free(raw); why = "resource unreadable"; goto done; }
            long at = locate_in_resource(raw, size, compressed, memsize, &why);
            free(raw);
            if (at >= 0) result = (int64_t)off + at;
            goto done;
        }
    }
done:
    free(idx);
    if (result >= 0)
        hle_log(HLE_LOG_INFO, "skip intro: %s: intro-movie property served false at offset 0x%llx", host,
                (unsigned long long)result);
    else if (why)
        hle_log(HLE_LOG_WARN, "skip intro: %s: application properties left unchanged (%s)", host, why);
    return result;
}

/* The cache: one scan per package file (device, inode, size and modification time), shared by every handle. */
#define MAX_CACHE 64
static struct { dev_t dev; ino_t ino; off_t size; struct timespec mtime; int64_t at; } cache[MAX_CACHE];
static unsigned n_cache;
static pthread_mutex_t cache_lock = PTHREAD_MUTEX_INITIALIZER;

int64_t w32_skip_intro_offset(int fd, const char *host) {
    if (!w32_skip_intro_enabled()) return -1;
    size_t n = strlen(host);
    if (n < 8 || strcasecmp(host + n - 8, ".package")) return -1;
    struct stat st;
    if (fstat(fd, &st)) return -1;
    pthread_mutex_lock(&cache_lock);
    for (unsigned i = 0; i < n_cache; i++)
        if (cache[i].dev == st.st_dev && cache[i].ino == st.st_ino && cache[i].size == st.st_size && cache[i].mtime.tv_sec == st.st_mtimespec.tv_sec &&
            cache[i].mtime.tv_nsec == st.st_mtimespec.tv_nsec) {
            int64_t at = cache[i].at;
            pthread_mutex_unlock(&cache_lock);
            return at;
        }
    pthread_mutex_unlock(&cache_lock);
    int64_t at = w32_skip_intro_scan(fd, host);
    pthread_mutex_lock(&cache_lock);
    unsigned k = n_cache < MAX_CACHE ? n_cache++ : (unsigned)(st.st_size % MAX_CACHE);
    cache[k].dev = st.st_dev;
    cache[k].ino = st.st_ino;
    cache[k].size = st.st_size;
    cache[k].mtime = st.st_mtimespec;
    cache[k].at = at;
    pthread_mutex_unlock(&cache_lock);
    return at;
}

void w32_skip_intro_reset(void) {
    pthread_mutex_lock(&cache_lock);
    n_cache = 0;
    pthread_mutex_unlock(&cache_lock);
}
