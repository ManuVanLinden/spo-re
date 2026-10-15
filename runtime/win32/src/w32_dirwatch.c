/* w32_dirwatch.c -- directory watching (design doc section 16): ReadDirectoryChangesW
 * (synchronous and overlapped), FindFirstChangeNotificationA/W, FindNextChangeNotification,
 * FindCloseChangeNotification.
 *
 * What the game does [obs: disassembly 0x92f8c0-0x92fbc7]: a watcher thread opens
 * the folder with CreateFileW(FILE_FLAG_BACKUP_SEMANTICS | FILE_FLAG_OVERLAPPED) (0x92fdc8),
 * creates an auto-reset event (CreateEventA(0,0,0,0) at 0x92f903) as OVERLAPPED.hEvent, calls
 * ReadDirectoryChangesW(h, buf, 0x2000, subtree, filter, &n, &ov, NULL) through the pointer it got
 * from GetProcAddress (0x1668f20), waits on {event, stop event} with WaitForMultipleObjectsEx
 * (0x92f97c), walks the FILE_NOTIFY_INFORMATION records itself (no GetOverlappedResult), maps the
 * actions (1/5 -> 2, 2/4 -> 1, 3 -> 8 or 0x28) for its callback, and loops; on stop it calls
 * CancelIo and closes the event. The fallback (0x92fb20) uses FindFirstChangeNotificationW +
 * WaitForMultipleObjects + FindNextChangeNotification.
 *
 * Back end: FSEvents with file-level events (kFSEventStreamCreateFlagFileEvents) as the trigger,
 * and a snapshot diff of the watched tree to produce the Win32 actions. Why FSEvents rather than
 * kqueue: kqueue's EVFILT_VNODE on a directory reports only that its entry list changed, not a
 * write to an existing file, so it needs one descriptor per file and per subdirectory for a
 * subtree watch; FSEvents watches a whole subtree with one stream and reports content changes.
 * Why a snapshot diff: FSEvents coalesces flags (a created-then-written file arrives as one event
 * with both bits) and reports renames as two unpaired paths, while Win32 needs ADDED, REMOVED,
 * MODIFIED and paired RENAMED_OLD_NAME/RENAMED_NEW_NAME records. Comparing (inode, size, mtime,
 * mode) per entry gives exactly those, renames paired by inode within a directory as NTFS does.
 * CoreServices/CoreFoundation are loaded with dlopen, so the image link needs no framework flag;
 * if they cannot be loaded the watcher polls every 250 ms instead.
 *
 * Kernel objects: the change-notification handle and OVERLAPPED.hEvent are the core's events (they
 * must work with WaitForMultipleObjects). This file reaches them only through w32_dirwatch_sync,
 * whose default is the core event API (sg_core_event_create/set/reset and sg_handle_close; weak
 * stand-ins here, because runtime/core is linked into the game binary but not into the w32 unit
 * tests, which install their own implementation).
 */
#include <dirent.h>
#include <dispatch/dispatch.h>
#include <dlfcn.h>
#include <errno.h>
#include <fcntl.h>
#include <limits.h>
#include <pthread.h>
#include <stdio.h>
#include <stdlib.h>
#include <string.h>
#include <sys/stat.h>
#include <time.h>
#include <unistd.h>

#include "w32_int.h"
#include "w32_dirwatch.h"

/* ---- the event back end ------------------------------------------------------------------------ */

/* The default is the core event API (runtime/core/include/sg_core.h section 5): ordinary SG_H_EVENT handles the guest waits on with
 * WaitForSingleObject/WaitForMultipleObjectsEx and closes with CloseHandle, set and reset
 * directly from any host thread (the FSEvents queue) with no guest context, stack or lock of ours.
 * The prototypes repeat sg_core.h's (runtime/win32 does not include the core's headers).
 *
 * Weak stand-ins: runtime/core's strong definitions replace them when the core is linked (the
 * game binary, and build/core/dirwatch's test); without it (the w32 unit tests, which install
 * their own operations) they report "no core": create returns 0, which first_change turns into
 * an error. Mach-O has no weak undefined references to static symbols, hence weak definitions. */
uint32_t sg_core_event_create(int manual_reset, int initial_state);
int sg_core_event_set(uint32_t h);
int sg_core_event_reset(uint32_t h);
int sg_handle_close(uint32_t h);
__attribute__((weak)) uint32_t sg_core_event_create(int manual_reset, int initial_state) {
    (void)manual_reset;
    (void)initial_state;
    return 0;
}
__attribute__((weak)) int sg_core_event_set(uint32_t h) { (void)h; return 0; }
__attribute__((weak)) int sg_core_event_reset(uint32_t h) { (void)h; return 0; }
__attribute__((weak)) int sg_handle_close(uint32_t h) { (void)h; return 0; }

static uint32_t core_event_create(int manual) { return sg_core_event_create(manual, 0); }

w32_dirwatch_ops w32_dirwatch_sync = {core_event_create, sg_core_event_set, sg_core_event_reset, sg_handle_close};

/* ---- snapshots ------------------------------------------------------------------------------- */

enum { FILE_NOTIFY_CHANGE_FILE_NAME = 0x1, FILE_NOTIFY_CHANGE_DIR_NAME = 0x2, FILE_NOTIFY_CHANGE_ATTRIBUTES = 0x4,
       FILE_NOTIFY_CHANGE_SIZE = 0x8, FILE_NOTIFY_CHANGE_LAST_WRITE = 0x10, FILE_NOTIFY_CHANGE_LAST_ACCESS = 0x20,
       FILE_NOTIFY_CHANGE_CREATION = 0x40, FILE_NOTIFY_CHANGE_SECURITY = 0x100, FILE_NOTIFY_ALL = 0x17f };
enum { FILE_ACTION_ADDED = 1, FILE_ACTION_REMOVED = 2, FILE_ACTION_MODIFIED = 3, FILE_ACTION_RENAMED_OLD_NAME = 4,
       FILE_ACTION_RENAMED_NEW_NAME = 5 };
#define STATUS_PENDING 0x00000103u
#define STATUS_NOTIFY_ENUM_DIR 0x0000010Cu
#define STATUS_CANCELLED 0xC0000120u
#define ERROR_NOTIFY_ENUM_DIR 1022
#define ERROR_IO_PENDING 997

typedef struct dw_entry {
    char *rel;  /* relative host path, '/' separators */
    uint64_t ino;
    int64_t size, mtime_ns, btime_ns;
    uint32_t mode, uid;
    int isdir;
} dw_entry;

typedef struct dw_snap { dw_entry *e; size_t n, cap; } dw_snap;

static int hidden_name(const char *n) {
    return !strcmp(n, ".") || !strcmp(n, "..") || !strcmp(n, ".DS_Store") || !strncmp(n, "._", 2);
}

static void snap_add(dw_snap *s, const char *rel, const struct stat *st) {
    if (s->n == s->cap) {
        s->cap = s->cap ? 2 * s->cap : 64;
        s->e = (dw_entry *)realloc(s->e, s->cap * sizeof *s->e);
    }
    dw_entry *e = &s->e[s->n++];
    e->rel = strdup(rel);
    e->ino = st->st_ino;
    e->size = st->st_size;
    e->mtime_ns = (int64_t)st->st_mtimespec.tv_sec * 1000000000 + st->st_mtimespec.tv_nsec;
    e->btime_ns = (int64_t)st->st_birthtimespec.tv_sec * 1000000000 + st->st_birthtimespec.tv_nsec;
    e->mode = st->st_mode;
    e->uid = st->st_uid;
    e->isdir = S_ISDIR(st->st_mode);
}

static void snap_scan(dw_snap *s, const char *root, const char *rel, int subtree, int depth) {
    char path[PATH_MAX];
    snprintf(path, sizeof path, "%s%s%s", root, *rel ? "/" : "", rel);
    DIR *d = opendir(path);
    if (!d) return;
    struct dirent *de;
    while ((de = readdir(d))) {
        if (hidden_name(de->d_name)) continue;
        char r[PATH_MAX], p[PATH_MAX];
        struct stat st;
        snprintf(r, sizeof r, "%s%s%s", rel, *rel ? "/" : "", de->d_name);
        snprintf(p, sizeof p, "%s/%s", root, r);
        if (lstat(p, &st)) continue;
        snap_add(s, r, &st);
        if (subtree && S_ISDIR(st.st_mode) && depth < 64) snap_scan(s, root, r, subtree, depth + 1);
    }
    closedir(d);
}

static int entry_cmp(const void *a, const void *b) {
    return strcmp(((const dw_entry *)a)->rel, ((const dw_entry *)b)->rel);
}

static void snap_take(dw_snap *s, const char *root, int subtree) {
    memset(s, 0, sizeof *s);
    snap_scan(s, root, "", subtree, 0);
    if (s->n) qsort(s->e, s->n, sizeof *s->e, entry_cmp);
}

static void snap_free(dw_snap *s) {
    for (size_t i = 0; i < s->n; i++) free(s->e[i].rel);
    free(s->e);
    memset(s, 0, sizeof *s);
}

/* ---- changes ------------------------------------------------------------------------------- */

typedef struct dw_change { uint32_t action; char *rel; struct dw_change *next; } dw_change;
typedef struct dw_list { dw_change *head, *tail; size_t n; } dw_list;

static void list_add(dw_list *l, uint32_t action, const char *rel) {
    dw_change *c = (dw_change *)calloc(1, sizeof *c);
    c->action = action;
    c->rel = strdup(rel);
    if (l->tail) l->tail->next = c; else l->head = c;
    l->tail = c;
    l->n++;
}
static void list_free(dw_list *l) {
    for (dw_change *c = l->head, *nx; c; c = nx) { nx = c->next; free(c->rel); free(c); }
    memset(l, 0, sizeof *l);
}

static void parent_of(const char *rel, char *out, size_t cap) {
    snprintf(out, cap, "%s", rel);
    char *s = strrchr(out, '/');
    if (s) *s = 0; else out[0] = 0;
}
static int under(const char *rel, const char *dir) {
    size_t n = strlen(dir);
    return !strncmp(rel, dir, n) && rel[n] == '/';
}

/* Which FILE_NOTIFY_CHANGE_* classes changed between two states of one entry. */
static uint32_t modified_classes(const dw_entry *a, const dw_entry *b) {
    uint32_t m = 0;
    if (a->size != b->size && !b->isdir) m |= FILE_NOTIFY_CHANGE_SIZE;
    if (a->mtime_ns != b->mtime_ns) m |= FILE_NOTIFY_CHANGE_LAST_WRITE;
    if ((a->mode & 0222) != (b->mode & 0222)) m |= FILE_NOTIFY_CHANGE_ATTRIBUTES; /* READONLY */
    if ((a->mode & 07777) != (b->mode & 07777) || a->uid != b->uid) m |= FILE_NOTIFY_CHANGE_SECURITY;
    if (a->btime_ns != b->btime_ns) m |= FILE_NOTIFY_CHANGE_CREATION;
    return m;
}

/* Diff two sorted snapshots into Win32 actions, filtered. Order: removals (deepest first),
 * rename pairs, additions (parents first), modifications. */
static void snap_diff(const dw_snap *o, const dw_snap *n, uint32_t filter, dw_list *out) {
    size_t i = 0, j = 0;
    const dw_entry **rem = (const dw_entry **)calloc(o->n + 1, sizeof *rem), **add = (const dw_entry **)calloc(n->n + 1, sizeof *add);
    size_t nr = 0, na = 0;
    dw_list mods = {0};
    while (i < o->n || j < n->n) {
        int c = i == o->n ? 1 : j == n->n ? -1 : strcmp(o->e[i].rel, n->e[j].rel);
        if (c < 0) rem[nr++] = &o->e[i++];
        else if (c > 0) add[na++] = &n->e[j++];
        else {
            const dw_entry *a = &o->e[i++], *b = &n->e[j++];
            if (a->ino != b->ino || a->isdir != b->isdir) { rem[nr++] = a; add[na++] = b; continue; }
            if (modified_classes(a, b) & filter) list_add(&mods, FILE_ACTION_MODIFIED, b->rel);
        }
    }
    /* pair renames: same inode, same parent directory */
    dw_list renames = {0};
    for (size_t r = 0; r < nr; r++) {
        if (!rem[r]) continue;
        char pr[PATH_MAX], pa[PATH_MAX];
        parent_of(rem[r]->rel, pr, sizeof pr);
        for (size_t a = 0; a < na; a++) {
            if (!add[a] || add[a]->ino != rem[r]->ino) continue;
            parent_of(add[a]->rel, pa, sizeof pa);
            if (strcmp(pr, pa)) continue;
            uint32_t cls = rem[r]->isdir ? FILE_NOTIFY_CHANGE_DIR_NAME : FILE_NOTIFY_CHANGE_FILE_NAME;
            if (cls & filter) {
                list_add(&renames, FILE_ACTION_RENAMED_OLD_NAME, rem[r]->rel);
                list_add(&renames, FILE_ACTION_RENAMED_NEW_NAME, add[a]->rel);
            }
            if (rem[r]->isdir) { /* the children moved with it: Windows reports only the folder */
                for (size_t k = 0; k < nr; k++) if (rem[k] && under(rem[k]->rel, rem[r]->rel)) rem[k] = NULL;
                for (size_t k = 0; k < na; k++) if (add[k] && under(add[k]->rel, add[a]->rel)) add[k] = NULL;
            }
            rem[r] = NULL;
            add[a] = NULL;
            break;
        }
    }
    for (size_t r = nr; r-- > 0;) /* reverse path order: children before their folder */
        if (rem[r] && ((rem[r]->isdir ? FILE_NOTIFY_CHANGE_DIR_NAME : FILE_NOTIFY_CHANGE_FILE_NAME) & filter))
            list_add(out, FILE_ACTION_REMOVED, rem[r]->rel);
    for (dw_change *c = renames.head; c; c = c->next) list_add(out, c->action, c->rel);
    for (size_t a = 0; a < na; a++)
        if (add[a] && ((add[a]->isdir ? FILE_NOTIFY_CHANGE_DIR_NAME : FILE_NOTIFY_CHANGE_FILE_NAME) & filter))
            list_add(out, FILE_ACTION_ADDED, add[a]->rel);
    for (dw_change *c = mods.head; c; c = c->next) list_add(out, c->action, c->rel);
    list_free(&renames);
    list_free(&mods);
    free(rem);
    free(add);
}

/* ---- watchers ------------------------------------------------------------------------------ */

enum { DW_NOTIFY = 1, DW_READ = 2 };

typedef struct dirwatch {
    int kind;
    uint32_t handle;      /* DW_NOTIFY: the event handle given to the guest; DW_READ: the directory handle */
    void *file_obj;       /* DW_READ: the file-layer object behind the handle (detects reuse) */
    char root[PATH_MAX];
    int subtree;
    uint32_t filter;
    dw_snap snap;
    pthread_mutex_t lock;
    pthread_cond_t cv;
    dw_list queue;        /* DW_READ: changes not yet delivered */
    int armed;            /* DW_READ: a ReadDirectoryChangesW call has started recording */
    /* DW_READ: the outstanding overlapped request */
    int pending;
    uint32_t buf, buflen, ov, ev;
    int sync_waiters;
    /* back end */
    void *stream;
    dispatch_queue_t q;
    pthread_t poller;
    int polling, stop;
    struct dirwatch *next;
} dirwatch;

static pthread_mutex_t list_lock = PTHREAD_MUTEX_INITIALIZER;
static dirwatch *watchers;
unsigned w32_dirwatch_latency_ms = 30;

/* FSEvents through dlopen */
typedef void (*fse_cb)(const void *stream, void *info, size_t n, void *paths, const uint32_t flags[], const uint64_t ids[]);
typedef struct { long version; void *info; void *retain, *release, *desc; } fse_context;
static struct {
    int ok;
    void *(*str_create)(void *alloc, const char *path);
    void *(*array_create)(void *alloc, const void **vals, long n, const void *callbacks);
    void (*release)(const void *);
    const void *array_callbacks;
    void *(*create)(void *alloc, fse_cb cb, fse_context *ctx, void *paths, uint64_t since, double latency, uint32_t flags);
    void (*set_queue)(void *stream, dispatch_queue_t q);
    unsigned char (*start)(void *stream);
    void (*stop)(void *stream);
    void (*invalidate)(void *stream);
    void (*stream_release)(void *stream);
} fse;
static pthread_once_t fse_once = PTHREAD_ONCE_INIT;

static void fse_load(void) {
    if (getenv("SPORE_W32_DIRWATCH_POLL")) return; /* force the polling back end (tests) */
    void *cf = dlopen("/System/Library/Frameworks/CoreFoundation.framework/CoreFoundation", RTLD_NOW | RTLD_LOCAL);
    void *cs = dlopen("/System/Library/Frameworks/CoreServices.framework/CoreServices", RTLD_NOW | RTLD_LOCAL);
    if (!cf || !cs) return;
    *(void **)&fse.str_create = dlsym(cf, "CFStringCreateWithFileSystemRepresentation");
    *(void **)&fse.array_create = dlsym(cf, "CFArrayCreate");
    *(void **)&fse.release = dlsym(cf, "CFRelease");
    fse.array_callbacks = dlsym(cf, "kCFTypeArrayCallBacks");
    *(void **)&fse.create = dlsym(cs, "FSEventStreamCreate");
    *(void **)&fse.set_queue = dlsym(cs, "FSEventStreamSetDispatchQueue");
    *(void **)&fse.start = dlsym(cs, "FSEventStreamStart");
    *(void **)&fse.stop = dlsym(cs, "FSEventStreamStop");
    *(void **)&fse.invalidate = dlsym(cs, "FSEventStreamInvalidate");
    *(void **)&fse.stream_release = dlsym(cs, "FSEventStreamRelease");
    fse.ok = fse.str_create && fse.array_create && fse.release && fse.array_callbacks && fse.create && fse.set_queue &&
             fse.start && fse.stop && fse.invalidate && fse.stream_release;
}

/* Writes queued changes into the guest buffer as FILE_NOTIFY_INFORMATION records. Returns the
 * bytes used (0 when nothing fits: the caller reports STATUS_NOTIFY_ENUM_DIR). Delivered records
 * leave the queue; records that do not fit stay for the next call (Windows would drop them all
 * and report an overflow; keeping them is kinder to the game's loop). */
static uint32_t fill_buffer(dirwatch *w, uint8_t *mem, uint32_t buf, uint32_t cap, int *overflow) {
    uint32_t off = 0, last = 0;
    int any = 0;
    *overflow = 0;
    while (w->queue.head) {
        dw_change *c = w->queue.head;
        char win[PATH_MAX];
        snprintf(win, sizeof win, "%s", c->rel);
        for (char *p = win; *p; p++) if (*p == '/') *p = '\\';
        uint32_t units = w32_utf8_units(win, 1), rec = (12 + 2 * units + 3) & ~3u;
        if (off + 12 + 2 * units > cap) { if (!any) *overflow = 1; break; }
        if (any) sg_st32(mem, buf + last, off - last);
        sg_st32(mem, buf + off, 0);
        sg_st32(mem, buf + off + 4, c->action);
        sg_st32(mem, buf + off + 8, 2 * units);
        /* UTF-16, not NUL-terminated */
        const char *q = win;
        uint32_t at = buf + off + 12, cp;
        while ((cp = w32_utf8_decode(&q))) {
            if (cp >= 0x10000) {
                cp -= 0x10000;
                sg_st16(mem, at, (uint16_t)(0xd800 + (cp >> 10)));
                sg_st16(mem, at + 2, (uint16_t)(0xdc00 + (cp & 0x3ff)));
                at += 4;
            } else { sg_st16(mem, at, (uint16_t)cp); at += 2; }
        }
        last = off;
        off += rec;
        any = 1;
        w->queue.head = c->next;
        if (!w->queue.head) w->queue.tail = NULL;
        w->queue.n--;
        free(c->rel);
        free(c);
    }
    if (*overflow) { list_free(&w->queue); return 0; }
    return any ? (off > cap ? cap : off) : 0;
}

/* Completes the outstanding overlapped request if there is something to report. Holds w->lock. */
static void try_complete_locked(dirwatch *w) {
    if (!w->pending || !w->queue.head) return;
    uint8_t *mem = w32_mem;
    int overflow;
    uint32_t n = fill_buffer(w, mem, w->buf, w->buflen, &overflow);
    sg_st32(mem, w->ov + 4, n);
    sg_st32(mem, w->ov, overflow ? STATUS_NOTIFY_ENUM_DIR : 0);
    w->pending = 0;
    uint32_t ev = w->ev;
    __atomic_thread_fence(__ATOMIC_SEQ_CST);
    if (ev) w32_dirwatch_sync.event_set(ev);
}

/* Rescans and records what changed. Runs on the watcher's queue or poll thread. */
static void rescan(dirwatch *w) {
    dw_snap now;
    snap_take(&now, w->root, w->subtree);
    pthread_mutex_lock(&w->lock);
    if (w->stop) { pthread_mutex_unlock(&w->lock); snap_free(&now); return; }
    dw_list ch = {0};
    snap_diff(&w->snap, &now, w->filter, &ch);
    snap_free(&w->snap);
    w->snap = now;
    if (ch.n) {
        hle_log(HLE_LOG_TRACE, "dirwatch %s: %zu change(s)", w->root, ch.n);
        if (w->kind == DW_NOTIFY) {
            w32_dirwatch_sync.event_set(w->handle);
        } else if (w->armed) {
            /* bound the backlog like Windows' buffer would; past it, report an overflow */
            for (dw_change *c = ch.head; c; c = c->next) list_add(&w->queue, c->action, c->rel);
            try_complete_locked(w);
            if (w->sync_waiters) pthread_cond_broadcast(&w->cv);
        }
    }
    list_free(&ch);
    pthread_mutex_unlock(&w->lock);
}

static void fse_callback(const void *stream, void *info, size_t n, void *paths, const uint32_t flags[], const uint64_t ids[]) {
    (void)stream; (void)n; (void)paths; (void)flags; (void)ids;
    rescan((dirwatch *)info);
}

static void *poll_main(void *arg) {
    dirwatch *w = (dirwatch *)arg;
    for (;;) {
        struct timespec ts = {0, 250 * 1000000L};
        nanosleep(&ts, NULL);
        pthread_mutex_lock(&w->lock);
        int stop = w->stop;
        pthread_mutex_unlock(&w->lock);
        if (stop) return NULL;
        rescan(w);
    }
}

static int backend_start(dirwatch *w) {
    pthread_once(&fse_once, fse_load);
    if (fse.ok) {
        char qn[64];
        snprintf(qn, sizeof qn, "spore.w32.dirwatch.%p", (void *)w);
        w->q = dispatch_queue_create(qn, DISPATCH_QUEUE_SERIAL);
        void *s = fse.str_create(NULL, w->root);
        const void *vals[1] = {s};
        void *arr = fse.array_create(NULL, vals, 1, fse.array_callbacks);
        fse_context c = {0, w, NULL, NULL, NULL};
        /* kFSEventStreamCreateFlagNoDefer 0x2 | WatchRoot 0x4 | FileEvents 0x10; since now */
        w->stream = fse.create(NULL, fse_callback, &c, arr, 0xFFFFFFFFFFFFFFFFull, w32_dirwatch_latency_ms / 1000.0, 0x2 | 0x4 | 0x10);
        fse.release(arr);
        fse.release(s);
        if (w->stream) {
            fse.set_queue(w->stream, w->q);
            if (fse.start(w->stream)) return 0;
            fse.invalidate(w->stream);
            fse.stream_release(w->stream);
            w->stream = NULL;
        }
        dispatch_release(w->q);
        w->q = NULL;
        hle_log(HLE_LOG_WARN, "dirwatch %s: FSEvents stream failed; polling", w->root);
    }
    if (pthread_create(&w->poller, NULL, poll_main, w)) return -1;
    w->polling = 1;
    return 0;
}

static void drain(void *ctx) { (void)ctx; }

static void backend_stop(dirwatch *w) {
    pthread_mutex_lock(&w->lock);
    w->stop = 1;
    pthread_cond_broadcast(&w->cv);
    pthread_mutex_unlock(&w->lock);
    if (w->stream) {
        fse.stop(w->stream);
        fse.invalidate(w->stream);
        dispatch_sync_f(w->q, NULL, drain); /* no callback is running after this */
        fse.stream_release(w->stream);
        dispatch_release(w->q);
        w->stream = NULL;
    }
    if (w->polling) { pthread_join(w->poller, NULL); w->polling = 0; }
}

static dirwatch *watch_new(int kind, const char *host_dir, int subtree, uint32_t filter) {
    dirwatch *w = (dirwatch *)calloc(1, sizeof *w);
    if (!realpath(host_dir, w->root)) snprintf(w->root, sizeof w->root, "%s", host_dir);
    w->kind = kind;
    w->subtree = subtree;
    w->filter = filter;
    pthread_mutex_init(&w->lock, NULL);
    pthread_cond_init(&w->cv, NULL);
    snap_take(&w->snap, w->root, subtree);
    if (backend_start(w)) {
        snap_free(&w->snap);
        free(w);
        return NULL;
    }
    return w;
}

static void watch_free(dirwatch *w) {
    backend_stop(w);
    /* wake synchronous readers: they see stop and fail */
    pthread_mutex_lock(&w->lock);
    while (w->sync_waiters) {
        pthread_cond_broadcast(&w->cv);
        pthread_mutex_unlock(&w->lock);
        sched_yield();
        pthread_mutex_lock(&w->lock);
    }
    pthread_mutex_unlock(&w->lock);
    snap_free(&w->snap);
    list_free(&w->queue);
    pthread_mutex_destroy(&w->lock);
    pthread_cond_destroy(&w->cv);
    free(w);
}

static dirwatch *find_watch(int kind, uint32_t h, int unlink) {
    pthread_mutex_lock(&list_lock);
    dirwatch **pp = &watchers, *w = NULL;
    for (; *pp; pp = &(*pp)->next)
        if ((*pp)->kind == kind && (*pp)->handle == h) {
            w = *pp;
            if (unlink) *pp = w->next;
            break;
        }
    pthread_mutex_unlock(&list_lock);
    return w;
}

static void add_watch(dirwatch *w) {
    pthread_mutex_lock(&list_lock);
    w->next = watchers;
    watchers = w;
    pthread_mutex_unlock(&list_lock);
}

unsigned w32_dirwatch_count(void) {
    unsigned n = 0;
    pthread_mutex_lock(&list_lock);
    for (dirwatch *w = watchers; w; w = w->next) n++;
    pthread_mutex_unlock(&list_lock);
    return n;
}

/* ---- FindFirstChangeNotification ------------------------------------------------------------ */

/* FindFirstChangeNotificationA/W(lpPathName, bWatchSubtree, dwNotifyFilter) [doc]: a waitable
 * handle that is signalled when a change matching the filter happens; INVALID_HANDLE_VALUE
 * with ERROR_PATH_NOT_FOUND / ERROR_DIRECTORY / ERROR_INVALID_PARAMETER otherwise. The handle is
 * a manual-reset core event, so WaitForSingleObject/WaitForMultipleObjects work unchanged; it
 * stays signalled until FindNextChangeNotification. */
static uint32_t first_change(sg_ctx *ctx, uint8_t *mem, int wide) {
    char full[W32_PATH_CAP], host[W32_PATH_CAP];
    uint32_t filter = SG_ARG(2), err;
    int ex = 0;
    if (!filter || (filter & ~FILE_NOTIFY_ALL)) { hle_set_last_error(ctx, mem, W32_ERROR_INVALID_PARAMETER); return 0xffffffffu; }
    if ((err = w32_arg_path(mem, SG_ARG(0), wide, full, sizeof full))) { hle_set_last_error(ctx, mem, err); return 0xffffffffu; }
    struct stat st;
    if (w32_path_to_host(full, host, sizeof host, &ex, NULL) || !ex) { hle_set_last_error(ctx, mem, W32_ERROR_PATH_NOT_FOUND); return 0xffffffffu; }
    if (stat(host, &st) || !S_ISDIR(st.st_mode)) { hle_set_last_error(ctx, mem, W32_ERROR_DIRECTORY); return 0xffffffffu; }
    uint32_t ev = w32_dirwatch_sync.event_create(1);
    if (!ev) {
        hle_log(HLE_LOG_ERROR, "FindFirstChangeNotification(%s): no event back end (runtime core not linked?)", full);
        hle_set_last_error(ctx, mem, W32_ERROR_NOT_ENOUGH_MEMORY);
        return 0xffffffffu;
    }
    dirwatch *w = watch_new(DW_NOTIFY, host, SG_ARG(1) != 0, filter);
    if (!w) { w32_dirwatch_sync.handle_close(ev); hle_set_last_error(ctx, mem, W32_ERROR_NOT_ENOUGH_MEMORY); return 0xffffffffu; }
    w->handle = ev;
    add_watch(w);
    hle_log(HLE_LOG_INFO, "FindFirstChangeNotification(\"%s\", subtree %u, filter 0x%x) -> 0x%x (%s)", full, SG_ARG(1),
            filter, ev, w->stream ? "FSEvents" : "polling");
    return ev;
}
W32_IMPORT(kernel32, FindFirstChangeNotificationW) { SG_RETURN_STDCALL(first_change(ctx, mem, 1), 12); }
W32_IMPORT(kernel32, FindFirstChangeNotificationA) { SG_RETURN_STDCALL(first_change(ctx, mem, 0), 12); }

/* FindNextChangeNotification(hChangeHandle) [doc]: re-arms the handle (resets the event). */
W32_IMPORT(kernel32, FindNextChangeNotification) {
    dirwatch *w = find_watch(DW_NOTIFY, SG_ARG(0), 0);
    if (!w) { hle_set_last_error(ctx, mem, W32_ERROR_INVALID_HANDLE); SG_RETURN_STDCALL(0, 4); }
    pthread_mutex_lock(&w->lock);
    w32_dirwatch_sync.event_reset(w->handle);
    pthread_mutex_unlock(&w->lock);
    SG_RETURN_STDCALL(1, 4);
}

/* FindCloseChangeNotification(hChangeHandle) [doc]: stops watching and closes the handle. */
W32_IMPORT(kernel32, FindCloseChangeNotification) {
    dirwatch *w = find_watch(DW_NOTIFY, SG_ARG(0), 1);
    if (!w) { hle_set_last_error(ctx, mem, W32_ERROR_INVALID_HANDLE); SG_RETURN_STDCALL(0, 4); }
    uint32_t h = w->handle;
    watch_free(w);
    w32_dirwatch_sync.handle_close(h);
    SG_RETURN_STDCALL(1, 4);
}

/* ---- ReadDirectoryChangesW ------------------------------------------------------------------- */

/* The file layer's object for a directory handle starts with { int fd; int can_read, can_write,
 * is_dir; ... } (w32_file.c, struct wfile). Only fd and is_dir are read here. */
typedef struct wfile_head { int fd; int can_read, can_write, is_dir; } wfile_head;

static dirwatch *read_watch(uint32_t hdir, void *obj) {
    dirwatch *w = find_watch(DW_READ, hdir, 0);
    if (w && w->file_obj != obj) { /* the handle value was reused for another directory */
        find_watch(DW_READ, hdir, 1);
        watch_free(w);
        w = NULL;
    }
    return w;
}

/* ReadDirectoryChangesW(hDirectory, lpBuffer, nBufferLength, bWatchSubtree, dwNotifyFilter,
 * lpBytesReturned, lpOverlapped, lpCompletionRoutine) [doc: ReadDirectoryChangesW]:
 *   - the first call starts recording changes for the handle; later calls get what happened in
 *     between (the watch, its subtree flag and filter are fixed by the first call, as Windows'
 *     buffer is);
 *   - synchronous: blocks until there is something to report, fills the buffer, returns TRUE with
 *     *lpBytesReturned; 0 bytes means overflow (ERROR_NOTIFY_ENUM_DIR);
 *   - overlapped: returns TRUE at once; OVERLAPPED.Internal is STATUS_PENDING and hEvent is reset;
 *     on completion Internal = 0 (or STATUS_NOTIFY_ENUM_DIR), InternalHigh = bytes, hEvent set;
 *   - completion routines are not supported (FALSE, ERROR_NOT_SUPPORTED; the game passes NULL).
 * Errors: ERROR_INVALID_HANDLE (not a directory handle), ERROR_INVALID_PARAMETER (filter 0 or
 * unknown bits, buffer not DWORD-aligned), ERROR_NOACCESS (buffer not committed). */
uint32_t w32_read_dir_changes(sg_ctx *ctx, uint8_t *mem, uint32_t hdir, uint32_t buf, uint32_t len, uint32_t subtree,
                              uint32_t filter, uint32_t pbytes, uint32_t ov, uint32_t routine) {
    wfile_head *f = (wfile_head *)w32_handle_get(hdir, W32_H_FILE);
    if (!f || !f->is_dir) { hle_set_last_error(ctx, mem, W32_ERROR_INVALID_HANDLE); return 0; }
    if (!filter || (filter & ~FILE_NOTIFY_ALL) || (buf & 3) || !len) {
        hle_set_last_error(ctx, mem, W32_ERROR_INVALID_PARAMETER);
        return 0;
    }
    if (routine) {
        hle_log(HLE_LOG_WARN, "ReadDirectoryChangesW: completion routines are not supported");
        hle_set_last_error(ctx, mem, W32_ERROR_NOT_SUPPORTED);
        return 0;
    }
    if (!w32_vm_is_committed(buf, len) || (ov && !w32_vm_is_committed(ov, 20)) || (!ov && pbytes && !w32_vm_is_committed(pbytes, 4))) {
        hle_set_last_error(ctx, mem, W32_ERROR_NOACCESS);
        return 0;
    }
    dirwatch *w = read_watch(hdir, f);
    if (!w) {
        char path[PATH_MAX];
        if (fcntl(f->fd, F_GETPATH, path) == -1) { hle_set_last_error(ctx, mem, W32_ERROR_INVALID_HANDLE); return 0; }
        w = watch_new(DW_READ, path, subtree != 0, filter);
        if (!w) { hle_set_last_error(ctx, mem, W32_ERROR_NOT_ENOUGH_MEMORY); return 0; }
        w->handle = hdir;
        w->file_obj = f;
        add_watch(w);
        hle_log(HLE_LOG_INFO, "ReadDirectoryChangesW(0x%x \"%s\", subtree %u, filter 0x%x, %s) (%s)", hdir, w->root, subtree,
                filter, ov ? "overlapped" : "synchronous", w->stream ? "FSEvents" : "polling");
    }
    pthread_mutex_lock(&w->lock);
    w->armed = 1;
    if (ov) {
        if (w->pending) hle_log(HLE_LOG_WARN, "ReadDirectoryChangesW(0x%x): a request is already pending; replaced", hdir);
        uint32_t ev = sg_ld32(mem, ov + 16) & ~1u;
        sg_st32(mem, ov, STATUS_PENDING);
        sg_st32(mem, ov + 4, 0);
        if (ev) w32_dirwatch_sync.event_reset(ev);
        w->pending = 1;
        w->buf = buf;
        w->buflen = len;
        w->ov = ov;
        w->ev = ev;
        try_complete_locked(w);
        pthread_mutex_unlock(&w->lock);
        return 1;
    }
    w->sync_waiters++;
    while (!w->queue.head && !w->stop) pthread_cond_wait(&w->cv, &w->lock);
    w->sync_waiters--;
    if (w->stop) {
        pthread_mutex_unlock(&w->lock);
        hle_set_last_error(ctx, mem, W32_ERROR_INVALID_HANDLE); /* operation aborted by a close */
        return 0;
    }
    int overflow;
    uint32_t n = fill_buffer(w, mem, buf, len, &overflow);
    pthread_mutex_unlock(&w->lock);
    if (pbytes) sg_st32(mem, pbytes, n);
    if (overflow) hle_set_last_error(ctx, mem, ERROR_NOTIFY_ENUM_DIR);
    return 1;
}
W32_IMPORT(kernel32, ReadDirectoryChangesW) {
    SG_RETURN_STDCALL(w32_read_dir_changes(ctx, mem, SG_ARG(0), SG_ARG(1), SG_ARG(2), SG_ARG(3), SG_ARG(4), SG_ARG(5), SG_ARG(6), SG_ARG(7)), 32);
}

/* ReadDirectoryChangesW is a dynamic import (GetProcAddress, 0x130029a), so the generator's
 * override list (imports only) does not cover it: the strong definition is here. */
#if W32_OVERRIDE_HLE_STUBS
SG_IMPORT(kernel32, ReadDirectoryChangesW) { SG_TAILCALL(w32_imp_kernel32_ReadDirectoryChangesW); }
#endif

/* CancelIo back end (w32_file.c calls it):
 * cancels the outstanding request on a directory handle: Internal = STATUS_CANCELLED, 0 bytes,
 * event set [doc: CancelIo]. The watch keeps recording. */
void w32_dirwatch_cancel(uint32_t hdir) {
    dirwatch *w = find_watch(DW_READ, hdir, 0);
    if (!w) return;
    pthread_mutex_lock(&w->lock);
    if (w->pending) {
        sg_st32(w32_mem, w->ov + 4, 0);
        sg_st32(w32_mem, w->ov, STATUS_CANCELLED);
        w->pending = 0;
        if (w->ev) w32_dirwatch_sync.event_set(w->ev);
    }
    pthread_mutex_unlock(&w->lock);
}

/* CloseHandle back end for directory handles (same patch): ends the watch. A pending overlapped
 * request is completed as cancelled first, as closing a handle cancels its I/O. */
void w32_dirwatch_close(uint32_t hdir) {
    w32_dirwatch_cancel(hdir);
    dirwatch *w = find_watch(DW_READ, hdir, 1);
    if (w) watch_free(w);
}
