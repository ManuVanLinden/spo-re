/* w32_file.c -- KERNEL32 files, directories, find handles, folders and volumes, over the
 * path layer (w32_path.c). Design doc section 5.
 *
 * All guest handles of this layer live in one table (W32_HANDLE_BASE + 4*k). Reads and writes
 * go straight to host file descriptors (no buffering here; msvcr90 stdio buffers above us,
 * and EA's file layer reads in large blocks). Sharing modes are not enforced.
 */
#include <dirent.h>
#include <errno.h>
#include <fcntl.h>
#include <pthread.h>
#include <stdio.h>
#include <stdlib.h>
#include <string.h>
#include <sys/mount.h>
#include <sys/stat.h>
#include <unistd.h>

#include "w32_int.h"
#include "w32_dirwatch.h"

/* ---- handle table ------------------------------------------------------------------------ */

#define MAX_HANDLES 4096
typedef struct hslot { enum w32_htype type; void *obj; } hslot;
static hslot htab[MAX_HANDLES];
static pthread_mutex_t hlock = PTHREAD_MUTEX_INITIALIZER;
static int std_handles_enabled;
static uint32_t std_h[3];

static int foreign_default(sg_ctx *ctx, uint32_t h) {
    static int logged;
    (void)ctx;
    if (!logged++) hle_log(HLE_LOG_WARN, "CloseHandle(0x%08x): not a file-layer handle and no owner registered", h);
    return 1;
}
int (*w32_close_foreign)(sg_ctx *ctx, uint32_t handle) = foreign_default;

uint32_t w32_handle_new(enum w32_htype type, void *obj) {
    pthread_mutex_lock(&hlock);
    for (unsigned i = 0; i < MAX_HANDLES; i++)
        if (htab[i].type == W32_H_FREE) {
            htab[i].type = type;
            htab[i].obj = obj;
            pthread_mutex_unlock(&hlock);
            return W32_HANDLE_BASE + 4u * (i + 1);
        }
    pthread_mutex_unlock(&hlock);
    return 0;
}

static int hindex(uint32_t h) {
    if (h <= W32_HANDLE_BASE || h > W32_HANDLE_BASE + 4u * MAX_HANDLES || (h & 3)) return -1;
    return (int)((h - W32_HANDLE_BASE) / 4 - 1);
}
int w32_handle_is_ours(uint32_t h) {
    int i = hindex(h);
    return i >= 0 && htab[i].type != W32_H_FREE;
}
void *w32_handle_get(uint32_t h, enum w32_htype type) {
    int i = hindex(h);
    void *o = NULL;
    if (i < 0) return NULL;
    pthread_mutex_lock(&hlock);
    if (htab[i].type == type) o = htab[i].obj;
    pthread_mutex_unlock(&hlock);
    return o;
}
void *w32_handle_take(uint32_t h, enum w32_htype *type) {
    int i = hindex(h);
    void *o = NULL;
    if (i < 0) return NULL;
    pthread_mutex_lock(&hlock);
    if (htab[i].type != W32_H_FREE) {
        o = htab[i].obj;
        *type = htab[i].type;
        htab[i].type = W32_H_FREE;
        htab[i].obj = NULL;
    }
    pthread_mutex_unlock(&hlock);
    return o;
}

/* Per-host-path I/O accounting (docs/design/win32-layer.md 22): opens, reads, writes and bytes,
 * aggregated over all handles of a path. w32_io_report() prints it; runtime/core's boot
 * reporter calls it (found by dlsym) at a stop and at exit. Cheap: a few adds per call. */
typedef struct w32_iostat {
    char host[W32_PATH_CAP];
    uint64_t size, opens, reads, rbytes, writes, wbytes, max_end;
    int created;
} w32_iostat;
#define MAX_IOSTAT 1024
static w32_iostat iostat[MAX_IOSTAT];
static unsigned n_iostat;
static pthread_mutex_t iolock = PTHREAD_MUTEX_INITIALIZER;

static w32_iostat *iostat_for(const char *host, int created) {
    w32_iostat *r = NULL;
    struct stat st;
    pthread_mutex_lock(&iolock);
    for (unsigned i = 0; i < n_iostat && !r; i++)
        if (!strcmp(iostat[i].host, host)) r = &iostat[i];
    if (!r && n_iostat < MAX_IOSTAT) {
        r = &iostat[n_iostat++];
        snprintf(r->host, sizeof r->host, "%s", host);
        r->created = created;
    }
    if (r) {
        r->opens++;
        if (!stat(host, &st)) r->size = (uint64_t)st.st_size;
    }
    pthread_mutex_unlock(&iolock);
    return r;
}

void w32_io_report(FILE *out); /* also called by runtime/core via dlsym */
void w32_io_report(FILE *out) {
    pthread_mutex_lock(&iolock);
    fprintf(out, "io: %u files opened through CreateFile\n", n_iostat);
    for (unsigned i = 0; i < n_iostat; i++) {
        w32_iostat *r = &iostat[i];
        fprintf(out, "io: %-7s size %llu opens %llu reads %llu (%llu bytes, furthest %llu) writes %llu (%llu bytes) %s\n",
                r->created ? "created" : "existed", (unsigned long long)r->size, (unsigned long long)r->opens,
                (unsigned long long)r->reads, (unsigned long long)r->rbytes, (unsigned long long)r->max_end,
                (unsigned long long)r->writes, (unsigned long long)r->wbytes, r->host);
    }
    pthread_mutex_unlock(&iolock);
}

typedef struct wfile {
    int fd;
    int can_read, can_write, is_dir, delete_on_close;
    char host[W32_PATH_CAP];
    w32_iostat *io;
    int64_t skip_intro_at; /* SPORE_SKIP_INTRO: file offset served as 0, or -1 (w32_skipintro.c) */
} wfile;

typedef struct fentry { char *name; struct stat st; } fentry;
typedef struct wfind { fentry *e; size_t n, i; } wfind;

static wfile *std_file(int fd) {
    wfile *f = (wfile *)calloc(1, sizeof *f);
    f->fd = fd;
    f->skip_intro_at = -1;
    f->can_read = fd == 0;
    f->can_write = fd != 0;
    return f;
}

void w32_files_init(const w32_config *cfg) {
    const char *e = getenv("SPORE_W32_STDHANDLES");
    std_handles_enabled = cfg->std_handles || (e && atoi(e));
    for (int i = 0; i < 3; i++) std_h[i] = 0;
    if (std_handles_enabled)
        for (int i = 0; i < 3; i++) std_h[i] = w32_handle_new(W32_H_STD, std_file(i));
}

/* Returns the host fd for a file handle usable for I/O (files and std handles). */
static wfile *io_file(uint32_t h) {
    wfile *f = (wfile *)w32_handle_get(h, W32_H_FILE);
    return f ? f : (wfile *)w32_handle_get(h, W32_H_STD);
}

/* ---- helpers ------------------------------------------------------------------------------ */

typedef struct resolved {
    char full[W32_PATH_CAP];
    char host[W32_PATH_CAP];
    int exists, parent_exists;
    struct stat st;
} resolved;

/* Resolves a guest path argument. Returns 0 or a Win32 error. */
static uint32_t resolve(const uint8_t *mem, uint32_t a, int wide, resolved *r) {
    uint32_t e = w32_arg_path(mem, a, wide, r->full, sizeof r->full);
    if (e) return e;
    if (w32_path_to_host(r->full, r->host, sizeof r->host, &r->exists, &r->parent_exists)) return W32_ERROR_PATH_NOT_FOUND;
    if (r->exists && stat(r->host, &r->st)) r->exists = 0;
    return 0;
}

static uint32_t attrs_of(const struct stat *st, const char *name) {
    uint32_t a = 0;
    if (S_ISDIR(st->st_mode)) a |= 0x10;            /* FILE_ATTRIBUTE_DIRECTORY */
    else a |= 0x20;                                  /* FILE_ATTRIBUTE_ARCHIVE, as NTFS files usually are */
    if (!(st->st_mode & S_IWUSR)) a |= 0x1;          /* READONLY */
    (void)name;
    return a;
}

static void put_time(uint8_t *mem, uint32_t at, struct timespec ts) { sg_st64(mem, at, w32_filetime_from_unix(ts.tv_sec, ts.tv_nsec)); }

static int skip_host_name(const char *n) {
    return !strcmp(n, ".DS_Store") || !strncmp(n, "._", 2); /* macOS metadata, invisible on Windows */
}

/* ---- CreateFile ------------------------------------------------------------------------- */

enum { GENERIC_READ = 0x80000000u, GENERIC_WRITE = 0x40000000u, GENERIC_ALL = 0x10000000u,
       FILE_READ_DATA = 1, FILE_WRITE_DATA = 2, FILE_APPEND_DATA = 4 };
enum { CREATE_NEW = 1, CREATE_ALWAYS = 2, OPEN_EXISTING = 3, OPEN_ALWAYS = 4, TRUNCATE_EXISTING = 5 };
enum { FLAG_BACKUP_SEMANTICS = 0x02000000u, FLAG_DELETE_ON_CLOSE = 0x04000000u };

uint32_t w32_create_file(const uint8_t *mem, uint32_t name, int wide, uint32_t access, uint32_t disp,
                         uint32_t flags, uint32_t *err) {
    resolved r;
    uint32_t e = resolve(mem, name, wide, &r);
    *err = 0;
    if (e) { *err = e == W32_ERROR_BAD_PATHNAME ? W32_ERROR_ACCESS_DENIED : e; return W32_INVALID_HANDLE; }
    int rd = (access & (GENERIC_READ | GENERIC_ALL | FILE_READ_DATA)) != 0;
    int wr = (access & (GENERIC_WRITE | GENERIC_ALL | FILE_WRITE_DATA | FILE_APPEND_DATA)) != 0;
    if (!r.parent_exists) { *err = W32_ERROR_PATH_NOT_FOUND; return W32_INVALID_HANDLE; }
    if (r.exists && S_ISDIR(r.st.st_mode)) {
        if (!(flags & FLAG_BACKUP_SEMANTICS) || disp != OPEN_EXISTING) { *err = W32_ERROR_ACCESS_DENIED; return W32_INVALID_HANDLE; }
        int fd = open(r.host, O_RDONLY | O_DIRECTORY | O_CLOEXEC);
        if (fd < 0) { *err = w32_err_from_errno(errno); return W32_INVALID_HANDLE; }
        wfile *f = (wfile *)calloc(1, sizeof *f);
        f->fd = fd;
        f->is_dir = 1;
        f->skip_intro_at = -1;
        snprintf(f->host, sizeof f->host, "%s", r.host);
        uint32_t h = w32_handle_new(W32_H_FILE, f);
        if (!h) { close(fd); free(f); *err = W32_ERROR_TOO_MANY_OPEN_FILES; return W32_INVALID_HANDLE; }
        return h;
    }
    int of = O_CLOEXEC;
    uint32_t ok_err = 0;
    switch (disp) {
    case CREATE_NEW:
        if (r.exists) { *err = W32_ERROR_FILE_EXISTS; return W32_INVALID_HANDLE; }
        of |= O_CREAT | O_EXCL;
        break;
    case CREATE_ALWAYS:
        of |= O_CREAT | O_TRUNC;
        if (r.exists) ok_err = W32_ERROR_ALREADY_EXISTS;
        break;
    case OPEN_EXISTING:
        if (!r.exists) { *err = W32_ERROR_FILE_NOT_FOUND; return W32_INVALID_HANDLE; }
        break;
    case OPEN_ALWAYS:
        of |= O_CREAT;
        if (r.exists) ok_err = W32_ERROR_ALREADY_EXISTS;
        break;
    case TRUNCATE_EXISTING:
        if (!r.exists) { *err = W32_ERROR_FILE_NOT_FOUND; return W32_INVALID_HANDLE; }
        if (!wr) { *err = W32_ERROR_INVALID_PARAMETER; return W32_INVALID_HANDLE; }
        of |= O_TRUNC;
        break;
    default:
        *err = W32_ERROR_INVALID_PARAMETER;
        return W32_INVALID_HANDLE;
    }
    /* creating or truncating needs a writable host descriptor even for a read-only guest handle */
    if (wr || (of & (O_CREAT | O_TRUNC))) of |= O_RDWR;
    else of |= O_RDONLY;
    int fd = open(r.host, of, 0644);
    if (fd < 0 && (of & O_RDWR) && !wr && errno == EACCES && !(of & (O_CREAT | O_TRUNC))) fd = open(r.host, O_RDONLY | O_CLOEXEC);
    if (fd < 0) { *err = errno == EEXIST ? W32_ERROR_FILE_EXISTS : w32_err_from_errno(errno); return W32_INVALID_HANDLE; }
    wfile *f = (wfile *)calloc(1, sizeof *f);
    f->fd = fd;
    f->can_read = rd || !wr;
    f->can_write = wr;
    f->delete_on_close = (flags & FLAG_DELETE_ON_CLOSE) != 0;
    snprintf(f->host, sizeof f->host, "%s", r.host);
    f->io = iostat_for(r.host, !r.exists);
    f->skip_intro_at = (r.exists && !wr) ? w32_skip_intro_offset(fd, r.host) : -1;
    uint32_t h = w32_handle_new(W32_H_FILE, f);
    if (!h) { close(fd); free(f); *err = W32_ERROR_TOO_MANY_OPEN_FILES; return W32_INVALID_HANDLE; }
    *err = ok_err;
    return h;
}

static uint32_t create_file_shim(sg_ctx *ctx, uint8_t *mem, int wide) {
    /* (lpFileName, dwDesiredAccess, dwShareMode, lpSecurityAttributes, dwCreationDisposition,
     *  dwFlagsAndAttributes, hTemplateFile) */
    uint32_t err, h = w32_create_file(mem, SG_ARG(0), wide, SG_ARG(1), SG_ARG(4), SG_ARG(5), &err);
    if (hle_log_level >= HLE_LOG_INFO) {
        char n[W32_PATH_CAP];
        if (wide) w32_wstr_get(mem, SG_ARG(0), n, sizeof n); else w32_astr_get(mem, SG_ARG(0), n, sizeof n);
        hle_log(HLE_LOG_INFO, "CreateFile%c(\"%s\", 0x%x, disp %u) = 0x%08x (error %u)", wide ? 'W' : 'A', n,
                SG_ARG(1), SG_ARG(4), h, err);
    }
    hle_set_last_error(ctx, mem, err);
    return h;
}
W32_IMPORT(kernel32, CreateFileA) { SG_RETURN_STDCALL(create_file_shim(ctx, mem, 0), 28); }
W32_IMPORT(kernel32, CreateFileW) { SG_RETURN_STDCALL(create_file_shim(ctx, mem, 1), 28); }

/* ---- read / write / seek ------------------------------------------------------------------ */

static int rw_common(sg_ctx *ctx, uint8_t *mem, int is_write) {
    uint32_t h = SG_ARG(0), buf = SG_ARG(1), n = SG_ARG(2), pdone = SG_ARG(3), ovl = SG_ARG(4);
    wfile *f = io_file(h);
    if (pdone) sg_st32(mem, pdone, 0);
    if (!f) { hle_set_last_error(ctx, mem, W32_ERROR_INVALID_HANDLE); return 0; }
    if (f->is_dir) { hle_set_last_error(ctx, mem, W32_ERROR_INVALID_FUNCTION); return 0; }
    if (is_write ? !f->can_write : !f->can_read) { hle_set_last_error(ctx, mem, W32_ERROR_ACCESS_DENIED); return 0; }
    if (n && !w32_vm_is_committed(buf, n)) { hle_set_last_error(ctx, mem, W32_ERROR_NOACCESS); return 0; }
    off_t off = -1;
    if (ovl) off = (off_t)((uint64_t)sg_ld32(mem, ovl + 8) | ((uint64_t)sg_ld32(mem, ovl + 12) << 32));
    off_t start = off >= 0 ? off : (!is_write && f->skip_intro_at >= 0 ? lseek(f->fd, 0, SEEK_CUR) : -1);
    uint32_t done = 0;
    while (done < n) {
        ssize_t k;
        if (is_write) k = off >= 0 ? pwrite(f->fd, mem + buf + done, n - done, off + done) : write(f->fd, mem + buf + done, n - done);
        else k = off >= 0 ? pread(f->fd, mem + buf + done, n - done, off + done) : read(f->fd, mem + buf + done, n - done);
        if (k < 0) {
            if (errno == EINTR) continue;
            if (pdone) sg_st32(mem, pdone, done);
            hle_set_last_error(ctx, mem, w32_err_from_errno(errno));
            return 0;
        }
        if (!k) break; /* end of file: success with fewer bytes */
        done += (uint32_t)k;
    }
    if (pdone) sg_st32(mem, pdone, done);
    /* SPORE_SKIP_INTRO: the intro-movie property's value byte reads as false (w32_skipintro.c) */
    if (!is_write && f->skip_intro_at >= 0 && start >= 0 && f->skip_intro_at >= start &&
        f->skip_intro_at < start + (off_t)done && mem[buf + (uint32_t)(f->skip_intro_at - start)] == 1)
        mem[buf + (uint32_t)(f->skip_intro_at - start)] = 0;
    if (f->io) {
        off_t end = off >= 0 ? off + done : lseek(f->fd, 0, SEEK_CUR);
        pthread_mutex_lock(&iolock);
        if (is_write) { f->io->writes++; f->io->wbytes += done; }
        else { f->io->reads++; f->io->rbytes += done; }
        if (end > 0 && (uint64_t)end > f->io->max_end) f->io->max_end = (uint64_t)end;
        pthread_mutex_unlock(&iolock);
    }
    if (hle_log_level >= HLE_LOG_TRACE)
        hle_log(HLE_LOG_TRACE, "%s(0x%08x, %u bytes%s) = %u: %s", is_write ? "WriteFile" : "ReadFile", h, n,
                ovl ? ", overlapped" : "", done, f->host);
    if (ovl) { sg_st32(mem, ovl, 0); sg_st32(mem, ovl + 4, done); } /* Internal = STATUS_SUCCESS, InternalHigh = bytes */
    if (ovl && !is_write && !done && n) { hle_set_last_error(ctx, mem, W32_ERROR_HANDLE_EOF); return 0; }
    return 1;
}
W32_IMPORT(kernel32, ReadFile) { SG_RETURN_STDCALL(rw_common(ctx, mem, 0), 20); }
W32_IMPORT(kernel32, WriteFile) { SG_RETURN_STDCALL(rw_common(ctx, mem, 1), 20); }

W32_IMPORT(kernel32, SetFilePointer) {
    uint32_t h = SG_ARG(0), lo = SG_ARG(1), phi = SG_ARG(2), method = SG_ARG(3);
    wfile *f = io_file(h);
    if (!f) { hle_set_last_error(ctx, mem, W32_ERROR_INVALID_HANDLE); SG_RETURN_STDCALL(W32_INVALID_SET_FILE_POINTER, 16); }
    int64_t dist = phi ? (int64_t)(((uint64_t)sg_ld32(mem, phi) << 32) | lo) : (int64_t)(int32_t)lo;
    int64_t base;
    struct stat st;
    switch (method) {
    case 0: base = 0; break;
    case 1: base = lseek(f->fd, 0, SEEK_CUR); break;
    case 2: base = fstat(f->fd, &st) ? 0 : st.st_size; break;
    default: hle_set_last_error(ctx, mem, W32_ERROR_INVALID_PARAMETER); SG_RETURN_STDCALL(W32_INVALID_SET_FILE_POINTER, 16);
    }
    int64_t pos = base + dist;
    if (pos < 0) { hle_set_last_error(ctx, mem, W32_ERROR_NEGATIVE_SEEK); SG_RETURN_STDCALL(W32_INVALID_SET_FILE_POINTER, 16); }
    if (lseek(f->fd, pos, SEEK_SET) < 0) {
        hle_set_last_error(ctx, mem, w32_err_from_errno(errno));
        SG_RETURN_STDCALL(W32_INVALID_SET_FILE_POINTER, 16);
    }
    if (phi) sg_st32(mem, phi, (uint32_t)((uint64_t)pos >> 32));
    hle_set_last_error(ctx, mem, 0);
    SG_RETURN_STDCALL((uint32_t)pos, 16);
}

W32_IMPORT(kernel32, GetFileSize) {
    uint32_t h = SG_ARG(0), phi = SG_ARG(1);
    wfile *f = io_file(h);
    struct stat st;
    if (!f || fstat(f->fd, &st)) { hle_set_last_error(ctx, mem, W32_ERROR_INVALID_HANDLE); SG_RETURN_STDCALL(0xffffffffu, 8); }
    if (phi) sg_st32(mem, phi, (uint32_t)((uint64_t)st.st_size >> 32));
    hle_set_last_error(ctx, mem, 0);
    SG_RETURN_STDCALL((uint32_t)st.st_size, 8);
}

W32_IMPORT(kernel32, SetEndOfFile) {
    wfile *f = io_file(SG_ARG(0));
    if (!f || f->is_dir) { hle_set_last_error(ctx, mem, W32_ERROR_INVALID_HANDLE); SG_RETURN_STDCALL(0, 4); }
    if (!f->can_write) { hle_set_last_error(ctx, mem, W32_ERROR_ACCESS_DENIED); SG_RETURN_STDCALL(0, 4); }
    off_t pos = lseek(f->fd, 0, SEEK_CUR);
    if (pos < 0 || ftruncate(f->fd, pos)) { hle_set_last_error(ctx, mem, w32_err_from_errno(errno)); SG_RETURN_STDCALL(0, 4); }
    SG_RETURN_STDCALL(1, 4);
}

W32_IMPORT(kernel32, FlushFileBuffers) {
    wfile *f = io_file(SG_ARG(0));
    if (!f) { hle_set_last_error(ctx, mem, W32_ERROR_INVALID_HANDLE); SG_RETURN_STDCALL(0, 4); }
    SG_RETURN_STDCALL(1, 4); /* writes are unbuffered here; no fsync (slow, and not needed) */
}

W32_IMPORT(kernel32, GetFileType) {
    uint32_t h = SG_ARG(0);
    if (w32_handle_get(h, W32_H_STD)) SG_RETURN_STDCALL(2, 4); /* FILE_TYPE_CHAR */
    if (w32_handle_get(h, W32_H_FILE)) SG_RETURN_STDCALL(1, 4); /* FILE_TYPE_DISK */
    hle_set_last_error(ctx, mem, W32_ERROR_INVALID_HANDLE);
    SG_RETURN_STDCALL(0, 4);
}

W32_IMPORT(kernel32, GetStdHandle) {
    uint32_t n = SG_ARG(0);
    if (n < 0xfffffff4u || n > 0xfffffff6u) { hle_set_last_error(ctx, mem, W32_ERROR_INVALID_HANDLE); SG_RETURN_STDCALL(W32_INVALID_HANDLE, 4); }
    SG_RETURN_STDCALL(std_h[0xfffffff6u - n], 4); /* NULL for a GUI process without a console */
}

static void close_obj(enum w32_htype t, void *o) {
    if (t == W32_H_FILE || t == W32_H_STD) {
        wfile *f = (wfile *)o;
        if (t == W32_H_FILE) close(f->fd);
        if (f->delete_on_close) unlink(f->host);
        free(f);
    } else if (t == W32_H_FIND) {
        wfind *fd = (wfind *)o;
        for (size_t i = 0; i < fd->n; i++) free(fd->e[i].name);
        free(fd->e);
        free(fd);
    }
}

/* Closes one of this layer's handles: 1, or 0 with ERROR_INVALID_HANDLE. Never forwards, so it
 * can serve as the core's sg_core_close_hook while w32_close_foreign points back at the core. */
int w32_close_own_handle(sg_ctx *ctx, uint32_t h) {
    enum w32_htype t = W32_H_FREE;
    if (!w32_handle_is_ours(h)) {
        if (ctx) hle_set_last_error(ctx, ctx->mem, W32_ERROR_INVALID_HANDLE);
        return 0;
    }
    if (w32_handle_get(h, W32_H_STD)) return 1; /* std handles stay */
    w32_dirwatch_close(h); /* a directory being watched (w32_dirwatch.c): end the watch first */
    void *o = w32_handle_take(h, &t);
    close_obj(t, o);
    if (t == W32_H_FIND) { /* FindClose, not CloseHandle, owns these: Windows fails (we still free it) */
        if (ctx) hle_set_last_error(ctx, ctx->mem, W32_ERROR_INVALID_HANDLE);
        return 0;
    }
    return 1;
}

W32_IMPORT(kernel32, CloseHandle) {
    uint32_t h = SG_ARG(0);
    if (w32_handle_is_ours(h)) SG_RETURN_STDCALL(w32_close_own_handle(ctx, h), 4);
    SG_RETURN_STDCALL(w32_close_foreign(ctx, h), 4);
}

W32_IMPORT(kernel32, DeviceIoControl) {
    /* Only the volume-extents query (0x6b8240) uses it, on \\.\X: which CreateFile refuses. */
    hle_set_last_error(ctx, mem, W32_ERROR_INVALID_FUNCTION);
    SG_RETURN_STDCALL(0, 32);
}
/* I/O is synchronous here except ReadDirectoryChangesW (w32_dirwatch.c) [doc: CancelIo] */
W32_IMPORT(kernel32, CancelIo) {
    w32_dirwatch_cancel(SG_ARG(0));
    SG_RETURN_STDCALL(1, 4);
}

/* ---- names: delete, move, copy, attributes, directories -------------------------------- */

static int fail(sg_ctx *ctx, uint8_t *mem, uint32_t e) { hle_set_last_error(ctx, mem, e); return 0; }
static uint32_t missing_err(const resolved *r) { return r->parent_exists ? W32_ERROR_FILE_NOT_FOUND : W32_ERROR_PATH_NOT_FOUND; }

static int delete_file(sg_ctx *ctx, uint8_t *mem, int wide) {
    resolved r;
    uint32_t e = resolve(mem, SG_ARG(0), wide, &r);
    if (e) return fail(ctx, mem, e);
    if (!r.exists) return fail(ctx, mem, missing_err(&r));
    if (S_ISDIR(r.st.st_mode)) return fail(ctx, mem, W32_ERROR_ACCESS_DENIED);
    if (unlink(r.host)) return fail(ctx, mem, w32_err_from_errno(errno));
    return 1;
}
W32_IMPORT(kernel32, DeleteFileA) { SG_RETURN_STDCALL(delete_file(ctx, mem, 0), 4); }
W32_IMPORT(kernel32, DeleteFileW) { SG_RETURN_STDCALL(delete_file(ctx, mem, 1), 4); }

static int copy_host(const char *src, const char *dst, int fail_if_exists) {
    int in = open(src, O_RDONLY | O_CLOEXEC), out;
    char buf[65536];
    if (in < 0) return errno;
    out = open(dst, O_WRONLY | O_CREAT | O_CLOEXEC | (fail_if_exists ? O_EXCL : O_TRUNC), 0644);
    if (out < 0) { int e = errno; close(in); return e; }
    for (;;) {
        ssize_t k = read(in, buf, sizeof buf);
        if (k < 0 && errno == EINTR) continue;
        if (k <= 0) break;
        if (write(out, buf, (size_t)k) != k) { int e = errno; close(in); close(out); return e ? e : EIO; }
    }
    close(in);
    close(out);
    return 0;
}

static int move_file(sg_ctx *ctx, uint8_t *mem, int wide, uint32_t flags) {
    resolved s, d;
    uint32_t e;
    if ((e = resolve(mem, SG_ARG(0), wide, &s))) return fail(ctx, mem, e);
    if (!SG_ARG(1)) return fail(ctx, mem, W32_ERROR_INVALID_PARAMETER); /* delete-on-reboot: unsupported */
    if ((e = resolve(mem, SG_ARG(1), wide, &d))) return fail(ctx, mem, e);
    if (!s.exists) return fail(ctx, mem, missing_err(&s));
    if (!d.parent_exists) return fail(ctx, mem, W32_ERROR_PATH_NOT_FOUND);
    if (d.exists) {
        if (!(flags & 1) || S_ISDIR(d.st.st_mode)) return fail(ctx, mem, W32_ERROR_ALREADY_EXISTS);
        if (!strcmp(s.host, d.host)) return 1;
    }
    if (rename(s.host, d.host)) {
        if (errno == EXDEV && (flags & 2) && !S_ISDIR(s.st.st_mode)) {
            int ce = copy_host(s.host, d.host, 0);
            if (ce) return fail(ctx, mem, w32_err_from_errno(ce));
            unlink(s.host);
            return 1;
        }
        return fail(ctx, mem, w32_err_from_errno(errno));
    }
    return 1;
}
W32_IMPORT(kernel32, MoveFileA) { SG_RETURN_STDCALL(move_file(ctx, mem, 0, 0), 8); }
W32_IMPORT(kernel32, MoveFileExW) { SG_RETURN_STDCALL(move_file(ctx, mem, 1, SG_ARG(2)), 12); }

W32_IMPORT(kernel32, CopyFileW) {
    resolved s, d;
    uint32_t e;
    int failx = SG_ARG(2) != 0, r = 0;
    if ((e = resolve(mem, SG_ARG(0), 1, &s)) || (e = resolve(mem, SG_ARG(1), 1, &d))) { fail(ctx, mem, e); SG_RETURN_STDCALL(0, 12); }
    if (!s.exists) { fail(ctx, mem, missing_err(&s)); SG_RETURN_STDCALL(0, 12); }
    if (S_ISDIR(s.st.st_mode)) { fail(ctx, mem, W32_ERROR_ACCESS_DENIED); SG_RETURN_STDCALL(0, 12); }
    if (!d.parent_exists) { fail(ctx, mem, W32_ERROR_PATH_NOT_FOUND); SG_RETURN_STDCALL(0, 12); }
    if (d.exists && failx) { fail(ctx, mem, W32_ERROR_FILE_EXISTS); SG_RETURN_STDCALL(0, 12); }
    int ce = copy_host(s.host, d.host, 0);
    if (ce) fail(ctx, mem, w32_err_from_errno(ce));
    else r = 1;
    SG_RETURN_STDCALL(r, 12);
}

uint32_t w32_get_attributes(const uint8_t *mem, uint32_t a, int wide, uint32_t *err) {
    resolved r;
    uint32_t e = resolve(mem, a, wide, &r);
    *err = 0;
    if (e) { *err = e; return W32_INVALID_FILE_ATTRIBUTES; }
    if (!r.exists) { *err = missing_err(&r); return W32_INVALID_FILE_ATTRIBUTES; }
    const char *slash = strrchr(r.host, '/');
    return attrs_of(&r.st, slash ? slash + 1 : r.host);
}
static void log_attrs(const uint8_t *mem, uint32_t a, int wide, uint32_t at, uint32_t err) {
    char n[W32_PATH_CAP];
    if (hle_log_level < HLE_LOG_INFO) return;
    if (wide) w32_wstr_get(mem, a, n, sizeof n); else w32_astr_get(mem, a, n, sizeof n);
    hle_log(HLE_LOG_INFO, "GetFileAttributes%c(\"%s\") = 0x%08x (error %u)", wide ? 'W' : 'A', n, at, err);
}
W32_IMPORT(kernel32, GetFileAttributesW) {
    uint32_t err, a = w32_get_attributes(mem, SG_ARG(0), 1, &err);
    log_attrs(mem, SG_ARG(0), 1, a, err);
    if (a == W32_INVALID_FILE_ATTRIBUTES) hle_set_last_error(ctx, mem, err);
    SG_RETURN_STDCALL(a, 4);
}
W32_IMPORT(kernel32, GetFileAttributesA) {
    uint32_t err, a = w32_get_attributes(mem, SG_ARG(0), 0, &err);
    log_attrs(mem, SG_ARG(0), 0, a, err);
    if (a == W32_INVALID_FILE_ATTRIBUTES) hle_set_last_error(ctx, mem, err);
    SG_RETURN_STDCALL(a, 4);
}

W32_IMPORT(kernel32, SetFileAttributesW) {
    resolved r;
    uint32_t e = resolve(mem, SG_ARG(0), 1, &r), at = SG_ARG(1);
    if (e) { fail(ctx, mem, e); SG_RETURN_STDCALL(0, 8); }
    if (!r.exists) { fail(ctx, mem, missing_err(&r)); SG_RETURN_STDCALL(0, 8); }
    mode_t m = r.st.st_mode & 07777;
    m = (at & 1) ? (m & ~(mode_t)0222) : (m | S_IWUSR);
    if (chmod(r.host, m)) { fail(ctx, mem, w32_err_from_errno(errno)); SG_RETURN_STDCALL(0, 8); }
    SG_RETURN_STDCALL(1, 8);
}

static int create_dir(sg_ctx *ctx, uint8_t *mem, int wide) {
    resolved r;
    uint32_t e = resolve(mem, SG_ARG(0), wide, &r);
    if (e) return fail(ctx, mem, e);
    if (r.exists) return fail(ctx, mem, W32_ERROR_ALREADY_EXISTS);
    if (!r.parent_exists) return fail(ctx, mem, W32_ERROR_PATH_NOT_FOUND);
    if (mkdir(r.host, 0755)) return fail(ctx, mem, w32_err_from_errno(errno));
    return 1;
}
W32_IMPORT(kernel32, CreateDirectoryA) { SG_RETURN_STDCALL(create_dir(ctx, mem, 0), 8); }
W32_IMPORT(kernel32, CreateDirectoryW) { SG_RETURN_STDCALL(create_dir(ctx, mem, 1), 8); }

static int remove_dir(sg_ctx *ctx, uint8_t *mem, int wide) {
    resolved r;
    uint32_t e = resolve(mem, SG_ARG(0), wide, &r);
    if (e) return fail(ctx, mem, e);
    if (!r.exists) return fail(ctx, mem, missing_err(&r));
    if (!S_ISDIR(r.st.st_mode)) return fail(ctx, mem, W32_ERROR_DIRECTORY);
    if (rmdir(r.host)) {
        /* a folder holding only invisible macOS metadata counts as empty */
        if (errno == ENOTEMPTY) {
            DIR *d = opendir(r.host);
            struct dirent *de;
            int only_meta = d != NULL;
            while (d && (de = readdir(d)))
                if (strcmp(de->d_name, ".") && strcmp(de->d_name, "..") && !skip_host_name(de->d_name)) only_meta = 0;
            if (d) closedir(d);
            if (only_meta) {
                char p[W32_PATH_CAP * 2];
                d = opendir(r.host);
                while (d && (de = readdir(d)))
                    if (skip_host_name(de->d_name)) { snprintf(p, sizeof p, "%s/%s", r.host, de->d_name); unlink(p); }
                if (d) closedir(d);
                if (!rmdir(r.host)) return 1;
            }
            return fail(ctx, mem, W32_ERROR_DIR_NOT_EMPTY);
        }
        return fail(ctx, mem, w32_err_from_errno(errno));
    }
    return 1;
}
W32_IMPORT(kernel32, RemoveDirectoryA) { SG_RETURN_STDCALL(remove_dir(ctx, mem, 0), 4); }
W32_IMPORT(kernel32, RemoveDirectoryW) { SG_RETURN_STDCALL(remove_dir(ctx, mem, 1), 4); }

/* ---- FindFirstFile ------------------------------------------------------------------------ */

static int fentry_cmp(const void *a, const void *b) {
    const fentry *x = (const fentry *)a, *y = (const fentry *)b;
    int dx = !strcmp(x->name, ".") ? 0 : !strcmp(x->name, "..") ? 1 : 2;
    int dy = !strcmp(y->name, ".") ? 0 : !strcmp(y->name, "..") ? 1 : 2;
    if (dx != dy) return dx - dy;
    return w32_name_casecmp(x->name, y->name);
}

/* Builds the match list. Returns 0 or a Win32 error. */
uint32_t w32_find_open(const char *raw, wfind **out) {
    char dirpart[W32_PATH_CAP], pat[W32_PATH_CAP], full[W32_PATH_CAP], host[W32_PATH_CAP];
    const char *sep = NULL;
    int ex = 0;
    struct stat st;
    for (const char *p = raw; *p; p++)
        if (*p == '\\' || *p == '/') sep = p;
    if (sep) {
        size_t n = (size_t)(sep - raw) + 1;
        if (n >= sizeof dirpart) return W32_ERROR_FILENAME_EXCED_RANGE;
        memcpy(dirpart, raw, n);
        dirpart[n] = 0;
        snprintf(pat, sizeof pat, "%s", sep + 1);
    } else if (raw[0] && raw[1] == ':') {
        snprintf(dirpart, sizeof dirpart, "%.2s", raw);
        snprintf(pat, sizeof pat, "%s", raw + 2);
    } else {
        snprintf(dirpart, sizeof dirpart, ".");
        snprintf(pat, sizeof pat, "%s", raw);
    }
    if (!pat[0]) return W32_ERROR_FILE_NOT_FOUND;
    if (w32_path_full(dirpart, full, sizeof full) < 0) return W32_ERROR_PATH_NOT_FOUND;
    if (w32_path_to_host(full, host, sizeof host, &ex, NULL) || !ex || stat(host, &st) || !S_ISDIR(st.st_mode))
        return W32_ERROR_PATH_NOT_FOUND;
    int is_root = strlen(full) <= 3;
    DIR *d = opendir(host);
    if (!d) return w32_err_from_errno(errno);
    wfind *f = (wfind *)calloc(1, sizeof *f);
    size_t cap = 0;
    struct dirent *de;
    while ((de = readdir(d))) {
        const char *n = de->d_name;
        if (is_root && (!strcmp(n, ".") || !strcmp(n, ".."))) continue;
        if (skip_host_name(n) || !w32_wildcard_match(pat, n)) continue;
        if (f->n == cap) { cap = cap ? cap * 2 : 32; f->e = (fentry *)realloc(f->e, cap * sizeof *f->e); }
        char p[W32_PATH_CAP * 2];
        snprintf(p, sizeof p, "%s/%s", host, n);
        if (stat(p, &f->e[f->n].st)) continue; /* dangling link */
        f->e[f->n].name = strdup(n);
        f->n++;
    }
    closedir(d);
    if (!f->n) { free(f->e); free(f); return W32_ERROR_FILE_NOT_FOUND; }
    qsort(f->e, f->n, sizeof *f->e, fentry_cmp);
    *out = f;
    return 0;
}

static void fill_find(uint8_t *mem, uint32_t p, const fentry *e, int wide) {
    memset(mem + p, 0, wide ? 592u : 320u);
    sg_st32(mem, p, attrs_of(&e->st, e->name));
    put_time(mem, p + 4, e->st.st_birthtimespec);
    put_time(mem, p + 12, e->st.st_atimespec);
    put_time(mem, p + 20, e->st.st_mtimespec);
    uint64_t size = S_ISDIR(e->st.st_mode) ? 0 : (uint64_t)e->st.st_size;
    sg_st32(mem, p + 28, (uint32_t)(size >> 32));
    sg_st32(mem, p + 32, (uint32_t)size);
    w32_put_str_trunc(mem, p + 44, W32_MAX_PATH, e->name, wide); /* cFileName; cAlternateFileName stays empty */
}

static uint32_t find_first(sg_ctx *ctx, uint8_t *mem, int wide) {
    char raw[W32_PATH_CAP * 2];
    uint32_t a = SG_ARG(0), data = SG_ARG(1);
    wfind *f = NULL;
    if (!a || !data) { hle_set_last_error(ctx, mem, W32_ERROR_INVALID_PARAMETER); return W32_INVALID_HANDLE; }
    if (wide) w32_wstr_get(mem, a, raw, sizeof raw);
    else w32_astr_get(mem, a, raw, sizeof raw);
    uint32_t e = w32_find_open(raw, &f);
    hle_log(HLE_LOG_INFO, "FindFirstFile%c(\"%s\") = %s (%zu entries)", wide ? 'W' : 'A', raw, e ? "fail" : "ok",
            f ? f->n : (size_t)0);
    if (e) { hle_set_last_error(ctx, mem, e); return W32_INVALID_HANDLE; }
    uint32_t h = w32_handle_new(W32_H_FIND, f);
    if (!h) { close_obj(W32_H_FIND, f); hle_set_last_error(ctx, mem, W32_ERROR_TOO_MANY_OPEN_FILES); return W32_INVALID_HANDLE; }
    fill_find(mem, data, &f->e[0], wide);
    f->i = 1;
    hle_set_last_error(ctx, mem, 0);
    return h;
}
W32_IMPORT(kernel32, FindFirstFileA) { SG_RETURN_STDCALL(find_first(ctx, mem, 0), 8); }
W32_IMPORT(kernel32, FindFirstFileW) { SG_RETURN_STDCALL(find_first(ctx, mem, 1), 8); }

static int find_next(sg_ctx *ctx, uint8_t *mem, int wide) {
    wfind *f = (wfind *)w32_handle_get(SG_ARG(0), W32_H_FIND);
    if (!f) return fail(ctx, mem, W32_ERROR_INVALID_HANDLE);
    if (f->i >= f->n) return fail(ctx, mem, W32_ERROR_NO_MORE_FILES);
    fill_find(mem, SG_ARG(1), &f->e[f->i++], wide);
    return 1;
}
W32_IMPORT(kernel32, FindNextFileA) { SG_RETURN_STDCALL(find_next(ctx, mem, 0), 8); }
W32_IMPORT(kernel32, FindNextFileW) { SG_RETURN_STDCALL(find_next(ctx, mem, 1), 8); }

W32_IMPORT(kernel32, FindClose) {
    enum w32_htype t = W32_H_FREE;
    uint32_t h = SG_ARG(0);
    if (!w32_handle_get(h, W32_H_FIND)) { fail(ctx, mem, W32_ERROR_INVALID_HANDLE); SG_RETURN_STDCALL(0, 4); }
    close_obj(W32_H_FIND, w32_handle_take(h, &t));
    SG_RETURN_STDCALL(1, 4);
}

/* ---- path and folder queries ------------------------------------------------------------- */

/* The "returns length, or required size including the NUL" contract shared by
 * GetCurrentDirectory, GetTempPath, GetFullPathName, GetWindowsDirectory and friends. */
static uint32_t put_or_need(uint8_t *mem, uint32_t buf, uint32_t size, const char *s, int wide) {
    int fits;
    uint32_t r = w32_put_str(mem, buf, size, s, wide, &fits);
    return r;
}

static uint32_t full_path_name(sg_ctx *ctx, uint8_t *mem, int wide) {
    /* (lpFileName, nBufferLength, lpBuffer, lpFilePart) */
    char raw[W32_PATH_CAP * 2], full[W32_PATH_CAP];
    uint32_t size = SG_ARG(1), buf = SG_ARG(2), part = SG_ARG(3);
    if (!SG_ARG(0)) { hle_set_last_error(ctx, mem, W32_ERROR_INVALID_PARAMETER); return 0; }
    if (wide) w32_wstr_get(mem, SG_ARG(0), raw, sizeof raw);
    else w32_astr_get(mem, SG_ARG(0), raw, sizeof raw);
    if (!raw[0] || w32_path_full(raw, full, sizeof full) < 0) { hle_set_last_error(ctx, mem, W32_ERROR_INVALID_NAME); return 0; }
    int fits;
    uint32_t r = w32_put_str(mem, buf, size, full, wide, &fits);
    if (fits && part) {
        const char *last = strrchr(full, '\\');
        uint32_t off = last && last[1] ? w32_utf8_units(full, wide) - w32_utf8_units(last + 1, wide) : 0;
        sg_st32(mem, part, last && last[1] ? buf + off * (wide ? 2u : 1u) : 0);
    }
    return r;
}
W32_IMPORT(kernel32, GetFullPathNameA) { SG_RETURN_STDCALL(full_path_name(ctx, mem, 0), 16); }
W32_IMPORT(kernel32, GetFullPathNameW) { SG_RETURN_STDCALL(full_path_name(ctx, mem, 1), 16); }

W32_IMPORT(kernel32, GetLongPathNameW) {
    /* (lpszShortPath, lpszLongPath, cchBuffer): no 8.3 names exist here, so the long name is
     * the input as given, once we know it exists. */
    char raw[W32_PATH_CAP * 2];
    resolved r;
    uint32_t e = resolve(mem, SG_ARG(0), 1, &r);
    if (e) { fail(ctx, mem, e); SG_RETURN_STDCALL(0, 12); }
    if (!r.exists) { fail(ctx, mem, missing_err(&r)); SG_RETURN_STDCALL(0, 12); }
    w32_wstr_get(mem, SG_ARG(0), raw, sizeof raw);
    SG_RETURN_STDCALL(put_or_need(mem, SG_ARG(1), SG_ARG(2), raw, 1), 12);
}

W32_IMPORT(kernel32, GetTempPathW) {
    char t[W32_PATH_CAP];
    snprintf(t, sizeof t, "%s\\", w32_folder(W32_F_TEMP));
    SG_RETURN_STDCALL(put_or_need(mem, SG_ARG(1), SG_ARG(0), t, 1), 8);
}
W32_IMPORT(kernel32, GetCurrentDirectoryA) {
    SG_RETURN_STDCALL(put_or_need(mem, SG_ARG(1), SG_ARG(0), w32_path_cwd(), 0), 8);
}
W32_IMPORT(kernel32, GetWindowsDirectoryW) {
    SG_RETURN_STDCALL(put_or_need(mem, SG_ARG(0), SG_ARG(1), w32_folder(W32_F_WINDOWS), 1), 8);
}
W32_IMPORT(kernel32, GetSystemDirectoryW) {
    SG_RETURN_STDCALL(put_or_need(mem, SG_ARG(0), SG_ARG(1), w32_folder(W32_F_SYSTEM), 1), 8);
}

/* SHGetFolderPathW(hwnd, csidl, hToken, dwFlags, pszPath[MAX_PATH]) */
static const char *csidl_path(uint32_t csidl, char *tmp, size_t cap) {
    switch (csidl & 0xff) {
    case 0x05: return w32_folder(W32_F_DOCUMENTS);        /* CSIDL_PERSONAL */
    case 0x1a: return w32_folder(W32_F_APPDATA);          /* CSIDL_APPDATA */
    case 0x1c: return w32_folder(W32_F_LOCAL_APPDATA);    /* CSIDL_LOCAL_APPDATA */
    case 0x23: return w32_folder(W32_F_COMMON_APPDATA);   /* CSIDL_COMMON_APPDATA */
    case 0x2e: return w32_folder(W32_F_COMMON_DOCUMENTS); /* CSIDL_COMMON_DOCUMENTS */
    case 0x24: return w32_folder(W32_F_WINDOWS);          /* CSIDL_WINDOWS */
    case 0x25: return w32_folder(W32_F_SYSTEM);           /* CSIDL_SYSTEM */
    case 0x28: return w32_folder(W32_F_PROFILE);          /* CSIDL_PROFILE */
    case 0x26: return "C:\\Program Files";                /* CSIDL_PROGRAM_FILES */
    case 0x10: case 0x00: snprintf(tmp, cap, "%s\\Desktop", w32_folder(W32_F_PROFILE)); return tmp;
    case 0x27: snprintf(tmp, cap, "%s\\Pictures", w32_folder(W32_F_PROFILE)); return tmp;
    case 0x0d: snprintf(tmp, cap, "%s\\Music", w32_folder(W32_F_PROFILE)); return tmp;
    case 0x0e: snprintf(tmp, cap, "%s\\Videos", w32_folder(W32_F_PROFILE)); return tmp;
    default: return NULL;
    }
}
W32_IMPORT(shell32, SHGetFolderPathW) {
    char tmp[W32_PATH_CAP], host[W32_PATH_CAP];
    uint32_t csidl = SG_ARG(1), out = SG_ARG(4);
    const char *p = csidl_path(csidl, tmp, sizeof tmp);
    hle_log(HLE_LOG_INFO, "SHGetFolderPathW(0x%x) = %s", csidl, p ? p : "(unknown)");
    if (!p || !out) SG_RETURN_STDCALL(0x80070057u, 20); /* E_INVALIDARG */
    int ex = 0;
    if (!w32_path_to_host(p, host, sizeof host, &ex, NULL) && !ex) {
        if (csidl & 0x8000) mkdir(host, 0755); /* CSIDL_FLAG_CREATE */
        else if (stat(host, &(struct stat){0})) mkdir(host, 0755); /* the standard folders exist on Windows */
    }
    w32_put_str_trunc(mem, out, W32_MAX_PATH, p, 1);
    SG_RETURN_STDCALL(0, 20); /* S_OK */
}

/* ---- volumes ---------------------------------------------------------------------------- */

#define VOLUME_SERIAL 0x53504f52u /* fixed, so IDs derived from it are stable between runs */

static int is_c_root(const uint8_t *mem, uint32_t a, int wide) {
    char r[16];
    if (!a) return (w32_path_cwd()[0] | 32) == 'c';
    if (wide) w32_wstr_get(mem, a, r, sizeof r); else w32_astr_get(mem, a, r, sizeof r);
    return (r[0] | 32) == 'c' && r[1] == ':' && (r[2] == '\\' || r[2] == '/') && !r[3];
}

static int volume_info(sg_ctx *ctx, uint8_t *mem, int wide) {
    /* (lpRootPathName, lpVolumeNameBuffer, nVolumeNameSize, lpVolumeSerialNumber,
     *  lpMaximumComponentLength, lpFileSystemFlags, lpFileSystemNameBuffer, nFileSystemNameSize) */
    if (!is_c_root(mem, SG_ARG(0), wide)) return fail(ctx, mem, W32_ERROR_PATH_NOT_FOUND);
    if (SG_ARG(1)) w32_put_str_trunc(mem, SG_ARG(1), SG_ARG(2), "", wide);
    if (SG_ARG(3)) sg_st32(mem, SG_ARG(3), VOLUME_SERIAL);
    if (SG_ARG(4)) sg_st32(mem, SG_ARG(4), 255);
    if (SG_ARG(5)) sg_st32(mem, SG_ARG(5), 0x000700ffu); /* NTFS-like: case preserved, Unicode, ACLs ... */
    if (SG_ARG(6)) w32_put_str_trunc(mem, SG_ARG(6), SG_ARG(7), "NTFS", wide);
    return 1;
}
W32_IMPORT(kernel32, GetVolumeInformationA) { SG_RETURN_STDCALL(volume_info(ctx, mem, 0), 32); }
W32_IMPORT(kernel32, GetVolumeInformationW) { SG_RETURN_STDCALL(volume_info(ctx, mem, 1), 32); }

W32_IMPORT(kernel32, GetVolumePathNameW) {
    char full[W32_PATH_CAP];
    if (w32_arg_path(mem, SG_ARG(0), 1, full, sizeof full)) { fail(ctx, mem, W32_ERROR_INVALID_NAME); SG_RETURN_STDCALL(0, 12); }
    char root[4] = {full[0], ':', '\\', 0};
    if (SG_ARG(2) < 4) { fail(ctx, mem, W32_ERROR_FILENAME_EXCED_RANGE); SG_RETURN_STDCALL(0, 12); }
    w32_put_str_trunc(mem, SG_ARG(1), SG_ARG(2), root, 1);
    SG_RETURN_STDCALL(1, 12);
}

W32_IMPORT(kernel32, GetDriveTypeA) { SG_RETURN_STDCALL(is_c_root(mem, SG_ARG(0), 0) ? 3 : 1, 4); } /* FIXED / NO_ROOT_DIR */
W32_IMPORT(kernel32, GetLogicalDrives) { SG_RETURN_STDCALL(1u << 2, 0); }                      /* C: only */

W32_IMPORT(kernel32, GetDiskFreeSpaceExA) {
    /* (lpDirectoryName, lpFreeBytesAvailableToCaller, lpTotalNumberOfBytes, lpTotalNumberOfFreeBytes) */
    char full[W32_PATH_CAP], host[W32_PATH_CAP];
    struct statfs sf;
    const char *dir = w32_path_cwd();
    if (SG_ARG(0) && !w32_arg_path(mem, SG_ARG(0), 0, full, sizeof full)) dir = full;
    if (w32_path_to_host(dir, host, sizeof host, NULL, NULL) || statfs(host, &sf)) {
        fail(ctx, mem, W32_ERROR_PATH_NOT_FOUND);
        SG_RETURN_STDCALL(0, 16);
    }
    uint64_t bs = sf.f_bsize;
    if (SG_ARG(1)) sg_st64(mem, SG_ARG(1), sf.f_bavail * bs);
    if (SG_ARG(2)) sg_st64(mem, SG_ARG(2), sf.f_blocks * bs);
    if (SG_ARG(3)) sg_st64(mem, SG_ARG(3), sf.f_bfree * bs);
    SG_RETURN_STDCALL(1, 16);
}
