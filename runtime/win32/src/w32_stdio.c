/* w32_stdio.c -- msvcr90 stdio over the path layer (design doc section 5.6).
 *
 * A guest FILE is a 32-byte msvcr90 record (_ptr, _cnt, _base, _flag, _file, _charbuf,
 * _bufsiz, _tmpfname). The game reaches streams only through imported functions (no getc/putc
 * macros: those are not imported), so the record carries just _flag and _file; the state is
 * host-side. Text mode translates "\n" <-> "\r\n" and stops reading at Ctrl-Z, as msvcr90
 * does; the default mode follows _fmode. stdout/stderr of this GUI process have no console;
 * what the game prints there goes to the HLE log (level INFO).
 */
#include <errno.h>
#include <fcntl.h>
#include <pthread.h>
#include <stdio.h>
#include <stdlib.h>
#include <string.h>
#include <sys/stat.h>
#include <unistd.h>

#include "w32_int.h"

enum { IOREAD = 0x1, IOWRT = 0x2, IOEOF = 0x10, IOERR = 0x20, IORW = 0x80 };
#define IOB_ENTRIES 20
#define MAX_STREAMS 512
#define RBUF 4096

typedef struct stream {
    uint32_t guest;
    int fd;          /* -1 for the console-less std streams */
    int std;         /* 1 stdout, 2 stderr */
    int text, can_read, can_write;
    int eof, err;
    unsigned char rbuf[RBUF];
    size_t rpos, rlen;
    char line[1024]; size_t linelen; /* std stream line assembly for the log */
} stream;

static stream *streams[MAX_STREAMS];
static pthread_mutex_t st_lock = PTHREAD_MUTEX_INITIALIZER;
static uint32_t iob_va;

static stream *find_stream(uint32_t g) {
    stream *s = NULL;
    pthread_mutex_lock(&st_lock);
    for (int i = 0; i < MAX_STREAMS; i++)
        if (streams[i] && streams[i]->guest == g) { s = streams[i]; break; }
    pthread_mutex_unlock(&st_lock);
    return s;
}

static void sync_flags(stream *s) {
    uint32_t f = (s->can_read && s->can_write) ? IORW : s->can_read ? IOREAD : IOWRT;
    if (s->eof) f |= IOEOF;
    if (s->err) f |= IOERR;
    sg_st32(w32_mem, s->guest + 12, f);
}

static stream *new_stream(uint32_t guest, int fd) {
    stream *s = (stream *)calloc(1, sizeof *s);
    s->guest = guest;
    s->fd = fd;
    pthread_mutex_lock(&st_lock);
    for (int i = 0; i < MAX_STREAMS; i++)
        if (!streams[i]) {
            streams[i] = s;
            pthread_mutex_unlock(&st_lock);
            sg_st32(w32_mem, guest + 16, (uint32_t)(i + 3)); /* _file: a plausible small descriptor */
            return s;
        }
    pthread_mutex_unlock(&st_lock);
    free(s);
    return NULL;
}

void w32_stdio_init(void) {
    pthread_mutex_lock(&st_lock);
    for (int i = 0; i < MAX_STREAMS; i++) { free(streams[i]); streams[i] = NULL; }
    pthread_mutex_unlock(&st_lock);
    iob_va = hle_guest_alloc(32 * IOB_ENTRIES, 16);
    for (int i = 0; i < 3; i++) {
        stream *s = new_stream(iob_va + 32u * (uint32_t)i, -1);
        s->std = i;
        s->can_read = i == 0;
        s->can_write = i != 0;
        s->text = 1;
        sync_flags(s);
        sg_st32(w32_mem, s->guest + 16, 0xfffffffeu); /* _file = _NO_CONSOLE_FILENO (-2) */
    }
}

static void log_std(stream *s, const uint8_t *p, size_t n) {
    for (size_t i = 0; i < n; i++) {
        if (p[i] == '\n' || s->linelen == sizeof s->line - 1) {
            s->line[s->linelen] = 0;
            hle_log(HLE_LOG_INFO, "[guest %s] %s", s->std == 1 ? "stdout" : "stderr", s->line);
            s->linelen = 0;
            if (p[i] == '\n') continue;
        }
        if (p[i] != '\r') s->line[s->linelen++] = (char)p[i];
    }
}

/* Drops read-ahead so the host offset equals the logical position. */
static void drop_readahead(stream *s) {
    if (s->fd >= 0 && s->rlen > s->rpos) lseek(s->fd, -(off_t)(s->rlen - s->rpos), SEEK_CUR);
    s->rpos = s->rlen = 0;
}

static size_t raw_write(stream *s, const uint8_t *p, size_t n) {
    if (s->fd < 0) { if (s->std) log_std(s, p, n); return n; }
    drop_readahead(s);
    size_t done = 0;
    while (done < n) {
        ssize_t k = write(s->fd, p + done, n - done);
        if (k < 0 && errno == EINTR) continue;
        if (k <= 0) { s->err = 1; break; }
        done += (size_t)k;
    }
    return done;
}

/* Writes n bytes of user data; returns how many were consumed. */
static size_t s_write(stream *s, const uint8_t *p, size_t n) {
    if (!s->can_write) { s->err = 1; return 0; }
    if (!s->text) return raw_write(s, p, n);
    uint8_t tmp[8192];
    size_t used = 0, i = 0;
    while (i < n) {
        size_t t = 0;
        size_t start = i;
        while (i < n && t + 2 <= sizeof tmp) {
            if (p[i] == '\n') tmp[t++] = '\r';
            tmp[t++] = p[i++];
        }
        if (raw_write(s, tmp, t) != t) return used;
        used += i - start;
    }
    return used;
}

static int fill(stream *s) {
    if (s->fd < 0) return 0;
    ssize_t k;
    do k = read(s->fd, s->rbuf, RBUF); while (k < 0 && errno == EINTR);
    if (k < 0) { s->err = 1; return 0; }
    s->rpos = 0;
    s->rlen = (size_t)k;
    return k > 0;
}

/* Next user byte, or -1 at end of file. */
static int s_getc(stream *s) {
    for (;;) {
        if (s->rpos == s->rlen && !fill(s)) { s->eof = 1; return -1; }
        int c = s->rbuf[s->rpos++];
        if (!s->text) return c;
        if (c == 0x1a) { s->rpos--; s->eof = 1; return -1; } /* Ctrl-Z ends a text stream */
        if (c != '\r') return c;
        if (s->rpos == s->rlen && !fill(s)) return '\r';
        if (s->rbuf[s->rpos] == '\n') { s->rpos++; return '\n'; }
        return '\r';
    }
}

static size_t s_read(stream *s, uint8_t *p, size_t n) {
    if (!s->can_read) { s->err = 1; return 0; }
    size_t got = 0;
    if (!s->text) {
        while (got < n) {
            if (s->rpos == s->rlen) {
                if (n - got >= RBUF) { /* large reads bypass the buffer */
                    ssize_t k = s->fd < 0 ? 0 : read(s->fd, p + got, n - got);
                    if (k < 0 && errno == EINTR) continue;
                    if (k < 0) { s->err = 1; break; }
                    if (!k) { s->eof = 1; break; }
                    got += (size_t)k;
                    continue;
                }
                if (!fill(s)) { s->eof = 1; break; }
            }
            size_t k = s->rlen - s->rpos;
            if (k > n - got) k = n - got;
            memcpy(p + got, s->rbuf + s->rpos, k);
            s->rpos += k;
            got += k;
        }
        return got;
    }
    while (got < n) {
        int c = s_getc(s);
        if (c < 0) break;
        p[got++] = (uint8_t)c;
    }
    return got;
}

/* ---- fopen and friends ------------------------------------------------------------------ */

uint32_t w32_fopen(uint8_t *mem, const char *winpath, const char *mode, int *msvc_errno) {
    char full[W32_PATH_CAP], host[W32_PATH_CAP];
    int ex = 0, pex = 0, of = O_CLOEXEC, rd = 0, wr = 0, text = -1;
    *msvc_errno = 0;
    switch (mode[0]) {
    case 'r': rd = 1; break;
    case 'w': wr = 1; of |= O_CREAT | O_TRUNC; break;
    case 'a': wr = 1; of |= O_CREAT | O_APPEND; break;
    default: *msvc_errno = MSVC_EINVAL; return 0;
    }
    for (const char *m = mode + 1; *m; m++) {
        if (*m == '+') rd = wr = 1;
        else if (*m == 't') text = 1;
        else if (*m == 'b') text = 0;
        else if (*m == ',') { hle_log(HLE_LOG_WARN, "fopen: encoding flags in \"%s\" ignored", mode); break; }
    }
    if (text < 0) text = (w32_crt_fmode() & 0x8000) ? 0 : 1; /* _O_BINARY in _fmode */
    of |= rd && wr ? O_RDWR : wr ? O_WRONLY : O_RDONLY;
    if (w32_path_full(winpath, full, sizeof full) < 0 ||
        w32_path_to_host(full, host, sizeof host, &ex, &pex)) { *msvc_errno = MSVC_ENOENT; return 0; }
    if (!pex || (mode[0] == 'r' && !ex)) { *msvc_errno = MSVC_ENOENT; return 0; }
    int fd = open(host, of, 0644);
    if (fd < 0) { *msvc_errno = w32_msvc_errno_from_errno(errno); return 0; }
    uint32_t g = hle_guest_alloc(32, 16);
    stream *s = g ? new_stream(g, fd) : NULL;
    if (!s) { close(fd); *msvc_errno = MSVC_EMFILE; return 0; }
    s->text = text;
    s->can_read = rd;
    s->can_write = wr;
    sync_flags(s);
    hle_log(HLE_LOG_INFO, "fopen(\"%s\", \"%s\") -> %s", winpath, mode, host);
    return g;
}

/* Guest FILE records are not reused: msvcr90 recycles its slots, but a stale pointer to a
 * closed stream then fails cleanly here instead of aliasing a new one. */
int w32_fclose(uint32_t g) {
    stream *s = find_stream(g);
    if (!s || s->std) return -1;
    pthread_mutex_lock(&st_lock);
    for (int i = 0; i < MAX_STREAMS; i++)
        if (streams[i] == s) streams[i] = NULL;
    pthread_mutex_unlock(&st_lock);
    int r = s->fd >= 0 && close(s->fd) ? -1 : 0;
    sg_st32(w32_mem, g + 12, 0);
    free(s);
    return r;
}

void w32_stdio_flush_all(void) {
    pthread_mutex_lock(&st_lock);
    for (int i = 0; i < MAX_STREAMS; i++)
        if (streams[i] && streams[i]->std && streams[i]->linelen) log_std(streams[i], (const uint8_t *)"\n", 1);
    pthread_mutex_unlock(&st_lock);
}

static uint32_t guest_path_arg(uint8_t *mem, uint32_t a, int wide, char *out, size_t cap) {
    if (!a) return 0;
    if (wide) w32_wstr_get(mem, a, out, cap);
    else w32_astr_get(mem, a, out, cap);
    return 1;
}

W32_IMPORT(msvcr90, fopen) {
    char path[W32_PATH_CAP * 2], mode[32];
    int e;
    if (!guest_path_arg(mem, SG_ARG(0), 0, path, sizeof path) || !SG_ARG(1)) { hle_set_errno(mem, MSVC_EINVAL); SG_RETURN_CDECL(0); }
    hle_get_str(mem, SG_ARG(1), mode, sizeof mode);
    uint32_t g = w32_fopen(mem, path, mode, &e);
    if (!g) hle_set_errno(mem, e);
    hle_log(HLE_LOG_INFO, "fopen(\"%s\", \"%s\") = 0x%08x (errno %d)", path, mode, g, g ? 0 : e);
    SG_RETURN_CDECL(g);
}

W32_IMPORT(msvcr90, tmpfile) {
    char host[W32_PATH_CAP], full[W32_PATH_CAP];
    snprintf(full, sizeof full, "%s", w32_folder(W32_F_TEMP));
    if (w32_path_to_host(full, host, sizeof host, NULL, NULL)) SG_RETURN_CDECL(0);
    strncat(host, "/w32tmpXXXXXX", sizeof host - strlen(host) - 1);
    int fd = mkstemp(host);
    if (fd < 0) { hle_set_errno(mem, w32_msvc_errno_from_errno(errno)); SG_RETURN_CDECL(0); }
    unlink(host); /* deleted when closed, as msvcr90's tmpfile is */
    uint32_t g = hle_guest_alloc(32, 16);
    stream *s = g ? new_stream(g, fd) : NULL;
    if (!s) { close(fd); SG_RETURN_CDECL(0); }
    s->can_read = s->can_write = 1;
    sync_flags(s);
    SG_RETURN_CDECL(g);
}

W32_IMPORT(msvcr90, fclose) { SG_RETURN_CDECL(w32_fclose(SG_ARG(0))); }
W32_IMPORT(msvcr90, __iob_func) { SG_RETURN_CDECL(iob_va); }

W32_IMPORT(msvcr90, fread) {
    uint32_t buf = SG_ARG(0), size = SG_ARG(1), count = SG_ARG(2);
    stream *s = find_stream(SG_ARG(3));
    uint64_t total = (uint64_t)size * count;
    if (!s || !size || !count) SG_RETURN_CDECL(0);
    if (total > 0x7fffffffu || !w32_vm_is_committed(buf, (uint32_t)total)) { hle_set_errno(mem, MSVC_EINVAL); SG_RETURN_CDECL(0); }
    size_t got = s_read(s, mem + buf, (size_t)total);
    sync_flags(s);
    SG_RETURN_CDECL((uint32_t)(got / size));
}

W32_IMPORT(msvcr90, fwrite) {
    uint32_t buf = SG_ARG(0), size = SG_ARG(1), count = SG_ARG(2);
    stream *s = find_stream(SG_ARG(3));
    uint64_t total = (uint64_t)size * count;
    if (!s || !size || !count) SG_RETURN_CDECL(0);
    if (total > 0x7fffffffu) { hle_set_errno(mem, MSVC_EINVAL); SG_RETURN_CDECL(0); }
    size_t put = s_write(s, mem + buf, (size_t)total);
    sync_flags(s);
    SG_RETURN_CDECL((uint32_t)(put / size));
}

W32_IMPORT(msvcr90, fputs) {
    stream *s = find_stream(SG_ARG(1));
    uint32_t a = SG_ARG(0), n = hle_strlen(mem, a);
    if (!s) SG_RETURN_CDECL(-1);
    int r = s_write(s, mem + a, n) == n ? 0 : -1;
    sync_flags(s);
    SG_RETURN_CDECL(r);
}

W32_IMPORT(msvcr90, fgets) {
    uint32_t buf = SG_ARG(0), n = SG_ARG(1);
    stream *s = find_stream(SG_ARG(2));
    if (!s || (int32_t)n <= 0) SG_RETURN_CDECL(0);
    uint32_t i = 0;
    while (i + 1 < n) {
        int c = s_getc(s);
        if (c < 0) break;
        mem[buf + i++] = (uint8_t)c;
        if (c == '\n') break;
    }
    mem[buf + i] = 0;
    sync_flags(s);
    SG_RETURN_CDECL(i ? buf : 0);
}

static int64_t s_tell(stream *s) {
    if (s->fd < 0) return -1;
    off_t p = lseek(s->fd, 0, SEEK_CUR);
    return p < 0 ? -1 : (int64_t)p - (int64_t)(s->rlen - s->rpos);
}

W32_IMPORT(msvcr90, ftell) {
    stream *s = find_stream(SG_ARG(0));
    int64_t p = s ? s_tell(s) : -1;
    if (p < 0) hle_set_errno(mem, MSVC_EBADF);
    if (p > 0x7fffffff) { hle_set_errno(mem, MSVC_EINVAL); p = -1; }
    SG_RETURN_CDECL((uint32_t)p);
}

W32_IMPORT(msvcr90, fseek) {
    stream *s = find_stream(SG_ARG(0));
    int32_t off = (int32_t)SG_ARG(1);
    uint32_t origin = SG_ARG(2);
    if (!s || s->fd < 0 || origin > 2) { hle_set_errno(mem, MSVC_EINVAL); SG_RETURN_CDECL(-1); }
    int64_t target;
    if (origin == 1) target = s_tell(s) + off;
    else if (origin == 2) { struct stat st; fstat(s->fd, &st); target = st.st_size + off; }
    else target = off;
    if (target < 0) { hle_set_errno(mem, MSVC_EINVAL); SG_RETURN_CDECL(-1); }
    s->rpos = s->rlen = 0;
    if (lseek(s->fd, target, SEEK_SET) < 0) { hle_set_errno(mem, MSVC_EINVAL); SG_RETURN_CDECL(-1); }
    s->eof = 0;
    sync_flags(s);
    SG_RETURN_CDECL(0);
}

W32_IMPORT(msvcr90, fflush) {
    uint32_t g = SG_ARG(0);
    if (!g) { w32_stdio_flush_all(); SG_RETURN_CDECL(0); }
    stream *s = find_stream(g);
    if (!s) SG_RETURN_CDECL(-1);
    if (s->can_read && s->rlen) drop_readahead(s); /* a flush between reads and writes, as C requires */
    SG_RETURN_CDECL(0);
}

W32_IMPORT(msvcr90, feof) { stream *s = find_stream(SG_ARG(0)); SG_RETURN_CDECL(s && s->eof ? IOEOF : 0); }
W32_IMPORT(msvcr90, ferror) { stream *s = find_stream(SG_ARG(0)); SG_RETURN_CDECL(s && s->err ? IOERR : 0); }

static int32_t do_vfprintf(sg_ctx *ctx, uint8_t *mem, uint32_t g, uint32_t fmt, uint32_t ap) {
    stream *s = find_stream(g);
    if (!s || !fmt) { hle_set_errno(mem, MSVC_EINVAL); return -1; }
    hle_obuf o = {0};
    hle_va va = {mem, ap};
    int n = hle_format(ctx, mem, &o, fmt, 0, &va, HLE_FMT_CRT);
    if (n >= 0 && s_write(s, o.p, o.n) != o.n) n = -1;
    hle_obuf_free(&o);
    sync_flags(s);
    return n;
}
W32_IMPORT(msvcr90, fprintf) { SG_RETURN_CDECL(do_vfprintf(ctx, mem, SG_ARG(0), SG_ARG(1), ctx->esp + 12)); }
W32_IMPORT(msvcr90, vfprintf) { SG_RETURN_CDECL(do_vfprintf(ctx, mem, SG_ARG(0), SG_ARG(1), SG_ARG(2))); }

/* ---- _stat64i32, current directory ------------------------------------------------------- */

static int stat_common(uint8_t *mem, uint32_t pathp, int wide, uint32_t out) {
    char raw[W32_PATH_CAP * 2], full[W32_PATH_CAP], host[W32_PATH_CAP];
    int ex = 0;
    struct stat st;
    if (!pathp || !out) { hle_set_errno(mem, MSVC_EINVAL); return -1; }
    guest_path_arg(mem, pathp, wide, raw, sizeof raw);
    size_t n = strlen(raw);
    /* msvcr90 rejects a trailing separator except on a drive root */
    if (!n || ((raw[n - 1] == '\\' || raw[n - 1] == '/') && !(n == 3 && raw[1] == ':'))) { hle_set_errno(mem, MSVC_ENOENT); return -1; }
    if (w32_path_full(raw, full, sizeof full) < 0 || w32_path_to_host(full, host, sizeof host, &ex, NULL) || !ex ||
        stat(host, &st)) {
        hle_set_errno(mem, MSVC_ENOENT);
        return -1;
    }
    uint32_t mode = S_ISDIR(st.st_mode) ? 0x4000u | 0x40u : 0x8000u; /* _S_IFDIR + exec, _S_IFREG */
    mode |= 0x100u;                                                   /* _S_IREAD */
    if (st.st_mode & S_IWUSR) mode |= 0x80u;                          /* _S_IWRITE */
    const char *dot = strrchr(full, '.');
    if (dot && (!strcasecmp(dot, ".exe") || !strcasecmp(dot, ".com") || !strcasecmp(dot, ".bat") || !strcasecmp(dot, ".cmd")))
        mode |= 0x40u;
    mode |= (mode & 0700u) >> 3 | (mode & 0700u) >> 6; /* msvcr90 copies the user bits to group and other */
    memset(mem + out, 0, 48);
    sg_st32(mem, out, 2);                       /* st_dev: drive C: (0 = A:) */
    sg_st16(mem, out + 6, (uint16_t)mode);      /* st_mode */
    sg_st16(mem, out + 8, 1);                   /* st_nlink */
    sg_st32(mem, out + 16, 2);                  /* st_rdev */
    sg_st32(mem, out + 20, (uint32_t)st.st_size);
    sg_st64(mem, out + 24, (uint64_t)st.st_atime);
    sg_st64(mem, out + 32, (uint64_t)st.st_mtime);
    sg_st64(mem, out + 40, (uint64_t)st.st_birthtime);
    return 0;
}
static int stat_logged(uint8_t *mem, uint32_t a, int wide, uint32_t out) {
    int r = stat_common(mem, a, wide, out);
    if (hle_log_level >= HLE_LOG_INFO) {
        char n[W32_PATH_CAP];
        guest_path_arg(mem, a, wide, n, sizeof n);
        hle_log(HLE_LOG_INFO, "_%sstat64i32(\"%s\") = %d", wide ? "w" : "", a ? n : "(null)", r);
    }
    return r;
}
W32_IMPORT(msvcr90, _stat64i32) { SG_RETURN_CDECL(stat_logged(mem, SG_ARG(0), 0, SG_ARG(1))); }
W32_IMPORT(msvcr90, _wstat64i32) { SG_RETURN_CDECL(stat_logged(mem, SG_ARG(0), 1, SG_ARG(1))); }

static uint32_t wgetcwd_common(uint8_t *mem, uint32_t buf, int32_t maxlen) {
    const char *cwd = w32_path_cwd();
    uint32_t need = w32_utf8_units(cwd, 1) + 1;
    if (!buf) {
        uint32_t n = (uint32_t)maxlen > need ? (uint32_t)maxlen : need;
        buf = w32_malloc(2 * n);
        if (!buf) { hle_set_errno(mem, MSVC_ENOMEM); return 0; }
        maxlen = (int32_t)n;
    }
    if (maxlen <= 0) { hle_set_errno(mem, MSVC_EINVAL); return 0; }
    if ((uint32_t)maxlen < need) { hle_set_errno(mem, MSVC_ERANGE); return 0; }
    w32_put_str_trunc(mem, buf, (uint32_t)maxlen, cwd, 1);
    return buf;
}
W32_IMPORT(msvcr90, _wgetcwd) { SG_RETURN_CDECL(wgetcwd_common(mem, SG_ARG(0), (int32_t)SG_ARG(1))); }
W32_IMPORT(msvcr90, _getdrive) { SG_RETURN_CDECL((uint32_t)((w32_path_cwd()[0] | 32) - 'a' + 1)); }
W32_IMPORT(msvcr90, _wgetdcwd) {
    uint32_t drive = SG_ARG(0);
    if (drive && drive != 3) { hle_set_errno(mem, MSVC_EACCES); SG_RETURN_CDECL(0); }
    SG_RETURN_CDECL(wgetcwd_common(mem, SG_ARG(1), (int32_t)SG_ARG(2)));
}
