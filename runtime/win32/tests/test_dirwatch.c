/* test_dirwatch.c -- ReadDirectoryChangesW and the change-notification functions on our own
 * temporary directories: create, rename, modify, delete, subtree, buffering between calls,
 * overflow, cancel, close, the synchronous form, and the game's call pattern (0x92f8c0).
 *
 * runtime/core is not linked into this binary, so the events behind the handles come from a
 * small pthread implementation installed through w32_dirwatch_sync; the game binary uses the core's
 * events through the same interface. */
#include "harness.h"
#include "w32_dirwatch.h"
#include "w32_int.h"

#include <pthread.h>
#include <stdlib.h>
#include <sys/stat.h>
#include <time.h>
#include <unistd.h>

/* ---- fake events ---------------------------------------------------------------------------- */

#define NEV 64
static struct { int used, manual, signalled; } evs[NEV];
static pthread_mutex_t ev_lock = PTHREAD_MUTEX_INITIALIZER;
static pthread_cond_t ev_cv = PTHREAD_COND_INITIALIZER;
static int closes;

static uint32_t ev_handle(int i) { return 0x7000u + 4u * (uint32_t)i; }
static int ev_index(uint32_t h) {
    int i = (int)((h - 0x7000u) / 4u);
    return h >= 0x7000u && i < NEV && evs[i].used ? i : -1;
}
static uint32_t fake_create(int manual) {
    pthread_mutex_lock(&ev_lock);
    for (int i = 1; i < NEV; i++)
        if (!evs[i].used) {
            evs[i].used = 1; evs[i].manual = manual; evs[i].signalled = 0;
            pthread_mutex_unlock(&ev_lock);
            return ev_handle(i);
        }
    pthread_mutex_unlock(&ev_lock);
    return 0;
}
static int fake_set(uint32_t h) {
    pthread_mutex_lock(&ev_lock);
    int i = ev_index(h);
    if (i >= 0) { evs[i].signalled = 1; pthread_cond_broadcast(&ev_cv); }
    pthread_mutex_unlock(&ev_lock);
    return i >= 0;
}
static int fake_reset(uint32_t h) {
    pthread_mutex_lock(&ev_lock);
    int i = ev_index(h);
    if (i >= 0) evs[i].signalled = 0;
    pthread_mutex_unlock(&ev_lock);
    return i >= 0;
}
static int fake_close(uint32_t h) {
    pthread_mutex_lock(&ev_lock);
    int i = ev_index(h);
    if (i >= 0) { evs[i].used = 0; closes++; }
    pthread_mutex_unlock(&ev_lock);
    return i >= 0;
}
/* WaitForSingleObject: 1 if signalled within ms (auto-reset events are consumed) */
static int wait_ev(uint32_t h, int ms) {
    struct timespec dl;
    clock_gettime(CLOCK_REALTIME, &dl);
    dl.tv_sec += ms / 1000;
    dl.tv_nsec += (long)(ms % 1000) * 1000000L;
    if (dl.tv_nsec >= 1000000000L) { dl.tv_sec++; dl.tv_nsec -= 1000000000L; }
    pthread_mutex_lock(&ev_lock);
    int i = ev_index(h), r = 0;
    while (i >= 0 && !evs[i].signalled)
        if (pthread_cond_timedwait(&ev_cv, &ev_lock, &dl)) break;
    if (i >= 0 && evs[i].signalled) { r = 1; if (!evs[i].manual) evs[i].signalled = 0; }
    pthread_mutex_unlock(&ev_lock);
    return r;
}

/* ---- host helpers ------------------------------------------------------------------------------ */

static char wdir[1200];
static void hpath(char *out, size_t cap, const char *rel) { snprintf(out, cap, "%s/%s", wdir, rel); }
static void hwrite(const char *rel, const char *text) {
    char p[1400];
    hpath(p, sizeof p, rel);
    FILE *f = fopen(p, "a");
    if (f) { fputs(text, f); fclose(f); }
}
static void hmkdir(const char *rel) { char p[1400]; hpath(p, sizeof p, rel); mkdir(p, 0755); }
static void hrename(const char *a, const char *b) {
    char p[1400], q[1400];
    hpath(p, sizeof p, a);
    hpath(q, sizeof q, b);
    rename(p, q);
}
static void hunlink(const char *rel) { char p[1400]; hpath(p, sizeof p, rel); unlink(p); }
static void msleep(int ms) { struct timespec ts = {ms / 1000, (long)(ms % 1000) * 1000000L}; nanosleep(&ts, NULL); }

/* ---- FILE_NOTIFY_INFORMATION records ------------------------------------------------------------ */

typedef struct rec { uint32_t action; char name[256]; } rec;
static rec got[256];
static int ngot;

static int parse(uint32_t buf, uint32_t bytes) {
    uint32_t off = 0;
    int n = 0;
    if (!bytes) return 0;
    for (;;) {
        uint32_t next = sg_ld32(M, buf + off), act = sg_ld32(M, buf + off + 4), nb = sg_ld32(M, buf + off + 8);
        if (ngot < 256) {
            rec *r = &got[ngot++];
            r->action = act;
            uint32_t k = 0;
            for (uint32_t i = 0; i < nb / 2 && k < sizeof r->name - 1; i++) {
                uint16_t c = sg_ld16(M, buf + off + 12 + 2 * i);
                r->name[k++] = c < 0x80 ? (char)c : '?';
            }
            r->name[k] = 0;
        }
        n++;
        if (!next || (next & 3) || off + next >= bytes) break;
        off += next;
    }
    return n;
}
static int has(uint32_t action, const char *name) {
    for (int i = 0; i < ngot; i++) if (got[i].action == action && !strcmp(got[i].name, name)) return 1;
    return 0;
}
static int index_of(uint32_t action, const char *name) {
    for (int i = 0; i < ngot; i++) if (got[i].action == action && !strcmp(got[i].name, name)) return i;
    return -1;
}
static void dump(const char *what) {
    fprintf(stderr, "   %s: %d record(s):", what, ngot);
    for (int i = 0; i < ngot; i++) fprintf(stderr, " %u:%s", got[i].action, got[i].name);
    fprintf(stderr, "\n");
}

/* The game's loop: arm an overlapped read, wait on the event, parse, re-arm, until `done` says
 * the expected records are all there or `ms` passes. Returns 1 when done. */
static uint32_t gh, gbufa, gov, gev;
static int arm(uint32_t len) {
    return CALL(w32_imp_kernel32_ReadDirectoryChangesW, 32, gh, gbufa, len, 1, 0x17f & ~0x20, gbuf(4), gov, 0) == 1;
}
static int collect(int (*done)(void), int ms) {
    ngot = 0;
    for (int waited = 0; waited < ms; waited += 100) {
        if (wait_ev(gev, 100)) {
            CHECK_EQ(sg_ld32(M, gov), 0);
            parse(gbufa, sg_ld32(M, gov + 4));
            if (!arm(0x2000)) return 0;
        }
        if (done()) return 1;
    }
    return done();
}
static int d_add(void) { return has(1, "a.txt"); }
static int d_ren(void) { return has(4, "a.txt") && has(5, "b.txt"); }
static int d_mod(void) { return has(3, "b.txt"); }
static int d_sub(void) { return has(1, "sub") && has(1, "sub\\c.txt"); }
static int d_subren(void) { return has(4, "sub") && has(5, "sub2"); }
static int d_del(void) { return has(2, "b.txt") && has(2, "sub2\\c.txt"); }
static int d_buffered(void) { return has(1, "d.txt") && has(1, "e.txt"); }

/* synchronous reader thread target */
static uint32_t sync_h;
static void *writer_main(void *arg) {
    (void)arg;
    msleep(200);
    hwrite("sync.txt", "s");
    msleep(5000);
    w32_dirwatch_close(sync_h); /* unblocks the reader if the change was never seen */
    return NULL;
}

void test_dirwatch(void) {
    w32_dirwatch_ops saved = w32_dirwatch_sync;
    w32_dirwatch_sync = (w32_dirwatch_ops){fake_create, fake_set, fake_reset, fake_close};
    snprintf(wdir, sizeof wdir, "%s/docs/watch", t_root);
    mkdir(wdir, 0755);
    uint32_t gpath = gwstr("C:\\Users\\Player\\Documents\\watch");

    /* ---- FindFirstChangeNotificationW / FindNext / FindClose (fallback path, 0x92fb20) -------- */
    uint32_t n0 = w32_dirwatch_count();
    uint32_t h = CALL(w32_imp_kernel32_FindFirstChangeNotificationW, 12, gpath, 0, 0x1 /* FILE_NAME */);
    CHECK(h != 0xffffffffu && ev_index(h) >= 0);
    CHECK_EQ(w32_dirwatch_count(), n0 + 1);
    CHECK(!wait_ev(h, 150));
    hwrite("n1.txt", "x");
    CHECK(wait_ev(h, 3000));
    CHECK(wait_ev(h, 10)); /* manual reset: stays signalled */
    CHECK_EQ(CALL(w32_imp_kernel32_FindNextChangeNotification, 4, h), 1);
    CHECK(!wait_ev(h, 10));
    hwrite("n1.txt", "more"); /* a write is not a FILE_NAME change */
    CHECK(!wait_ev(h, 400));
    hmkdir("nsub");
    msleep(200);
    CALL(w32_imp_kernel32_FindNextChangeNotification, 4, h); /* a folder is not a FILE_NAME change either */
    hwrite("nsub/inner.txt", "x"); /* not watched without the subtree flag */
    CHECK(!wait_ev(h, 400));
    hunlink("n1.txt");
    CHECK(wait_ev(h, 3000));
    int c0 = closes;
    CHECK_EQ(CALL(w32_imp_kernel32_FindCloseChangeNotification, 4, h), 1);
    CHECK_EQ(closes, c0 + 1);
    CHECK_EQ(w32_dirwatch_count(), n0);
    CHECK_EQ(CALL(w32_imp_kernel32_FindNextChangeNotification, 4, h), 0);
    CHECK_EQ(last_error(), 6);
    /* subtree + LAST_WRITE */
    h = CALL(w32_imp_kernel32_FindFirstChangeNotificationA, 12, gstr("C:\\Users\\Player\\Documents\\watch"), 1, 0x10);
    CHECK(h != 0xffffffffu);
    hwrite("nsub/inner.txt", "y");
    CHECK(wait_ev(h, 3000));
    CALL(w32_imp_kernel32_FindCloseChangeNotification, 4, h);
    /* errors */
    CHECK_EQ(CALL(w32_imp_kernel32_FindFirstChangeNotificationW, 12, gwstr("C:\\no\\such\\dir"), 0, 1), 0xffffffffu);
    CHECK_EQ(last_error(), 3);
    CHECK_EQ(CALL(w32_imp_kernel32_FindFirstChangeNotificationW, 12, gpath, 0, 0), 0xffffffffu);
    CHECK_EQ(last_error(), 87);
    hwrite("plain.txt", "x");
    CHECK_EQ(CALL(w32_imp_kernel32_FindFirstChangeNotificationW, 12, gwstr("C:\\Users\\Player\\Documents\\watch\\plain.txt"), 0, 1), 0xffffffffu);
    CHECK_EQ(last_error(), 267);
    hunlink("plain.txt");

    /* ---- ReadDirectoryChangesW, overlapped, as the game uses it (0x92f8c0) --------------------- */
    gh = CALL(w32_imp_kernel32_CreateFileW, 28, gpath, 1 /* FILE_LIST_DIRECTORY */, 7, 0, 3 /* OPEN_EXISTING */,
              0x02000000u | 0x40000000u /* BACKUP_SEMANTICS | OVERLAPPED */, 0);
    CHECK(gh != 0xffffffffu);
    gev = fake_create(0); /* CreateEventA(0, FALSE, FALSE, 0) at 0x92f903 */
    gov = gbuf(20);
    sg_st32(M, gov + 16, gev);
    gbufa = gbuf(0x2000);
    CHECK(arm(0x2000));
    CHECK_EQ(sg_ld32(M, gov), 0x103); /* STATUS_PENDING */
    CHECK(!wait_ev(gev, 200));

    hwrite("a.txt", "1");
    CHECK(collect(d_add, 3000));
    hrename("a.txt", "b.txt");
    if (!collect(d_ren, 3000)) { CHECK(0); dump("rename"); }
    else {
        int o = index_of(4, "a.txt"), n = index_of(5, "b.txt");
        CHECK_EQ(n, o + 1); /* RENAMED_OLD_NAME immediately followed by RENAMED_NEW_NAME */
    }
    msleep(20);
    hwrite("b.txt", "22");
    if (!collect(d_mod, 3000)) { CHECK(0); dump("modify"); }
    else CHECK(!has(1, "b.txt") && !has(2, "b.txt"));
    hmkdir("sub");
    hwrite("sub/c.txt", "c");
    if (!collect(d_sub, 3000)) { CHECK(0); dump("subtree"); }
    else CHECK(index_of(1, "sub") < index_of(1, "sub\\c.txt"));
    hrename("sub", "sub2");
    if (!collect(d_subren, 3000)) { CHECK(0); dump("folder rename"); }
    else CHECK(!has(1, "sub2\\c.txt") && !has(2, "sub\\c.txt")); /* only the folder is reported */
    hunlink("b.txt");
    hunlink("sub2/c.txt");
    if (!collect(d_del, 3000)) { CHECK(0); dump("delete"); }

    /* changes between calls are kept: wait for completion, do not re-arm, change, then arm */
    hwrite("x0.txt", "x");
    CHECK(wait_ev(gev, 3000));
    ngot = 0;
    parse(gbufa, sg_ld32(M, gov + 4));
    CHECK(has(1, "x0.txt"));
    hwrite("d.txt", "d");
    hwrite("e.txt", "e");
    msleep(500);
    ngot = 0;
    CHECK(arm(0x2000));
    if (wait_ev(gev, 3000)) parse(gbufa, sg_ld32(M, gov + 4));
    if (!d_buffered()) {
        CHECK(arm(0x2000));
        if (wait_ev(gev, 3000)) parse(gbufa, sg_ld32(M, gov + 4));
    }
    CHECK(d_buffered());

    /* a buffer too small for one record: STATUS_NOTIFY_ENUM_DIR with 0 bytes */
    CHECK(arm(16));
    hwrite("a-rather-long-file-name.txt", "x");
    CHECK(wait_ev(gev, 3000));
    CHECK_EQ(sg_ld32(M, gov), 0x10c);
    CHECK_EQ(sg_ld32(M, gov + 4), 0);

    /* CancelIo (w32_dirwatch_cancel): STATUS_CANCELLED, event set */
    CHECK(arm(0x2000));
    msleep(100);
    w32_dirwatch_cancel(gh);
    CHECK(wait_ev(gev, 100));
    CHECK_EQ(sg_ld32(M, gov), 0xC0000120u);
    /* errors */
    CHECK_EQ(CALL(w32_imp_kernel32_ReadDirectoryChangesW, 32, 0x1234, gbufa, 0x2000, 1, 1, 0, gov, 0), 0);
    CHECK_EQ(last_error(), 6);
    CHECK_EQ(CALL(w32_imp_kernel32_ReadDirectoryChangesW, 32, gh, gbufa, 0x2000, 1, 0, 0, gov, 0), 0);
    CHECK_EQ(last_error(), 87);
    CHECK_EQ(CALL(w32_imp_kernel32_ReadDirectoryChangesW, 32, gh, gbufa + 2, 0x100, 1, 1, 0, gov, 0), 0);
    CHECK_EQ(last_error(), 87);
    CHECK_EQ(CALL(w32_imp_kernel32_ReadDirectoryChangesW, 32, gh, gbufa, 0x100, 1, 1, 0, gov, register_callback(NULL)), 0);
    CHECK_EQ(last_error(), 50);
    uint32_t fh = CALL(w32_imp_kernel32_CreateFileW, 28, gwstr("C:\\Users\\Player\\Documents\\watch\\d.txt"), 0x80000000u, 7, 0, 3, 0, 0);
    CHECK_EQ(CALL(w32_imp_kernel32_ReadDirectoryChangesW, 32, fh, gbufa, 0x100, 1, 1, 0, gov, 0), 0);
    CHECK_EQ(last_error(), 6);
    CALL(w32_imp_kernel32_CloseHandle, 4, fh);
    /* close ends the watch */
    unsigned before = w32_dirwatch_count();
    w32_dirwatch_close(gh);
    CHECK_EQ(w32_dirwatch_count(), before - 1);
    CALL(w32_imp_kernel32_CloseHandle, 4, gh);
    fake_close(gev);

    /* ---- synchronous ReadDirectoryChangesW ------------------------------------------------------- */
    sync_h = CALL(w32_imp_kernel32_CreateFileW, 28, gpath, 1, 7, 0, 3, 0x02000000u, 0);
    /* the first call starts recording, so start the watch with an overlapped call and cancel it */
    uint32_t ov2 = gbuf(20), pbytes = gbuf(4);
    CHECK_EQ(CALL(w32_imp_kernel32_ReadDirectoryChangesW, 32, sync_h, gbufa, 0x2000, 0, 1, 0, ov2, 0), 1);
    w32_dirwatch_cancel(sync_h);
    pthread_t th;
    pthread_create(&th, NULL, writer_main, NULL);
    CHECK_EQ(CALL(w32_imp_kernel32_ReadDirectoryChangesW, 32, sync_h, gbufa, 0x2000, 0, 1, pbytes, 0, 0), 1);
    ngot = 0;
    parse(gbufa, sg_ld32(M, pbytes));
    CHECK(has(1, "sync.txt"));
    pthread_cancel(th);
    pthread_join(th, NULL);
    w32_dirwatch_close(sync_h);
    CALL(w32_imp_kernel32_CloseHandle, 4, sync_h);
    CHECK_EQ(w32_dirwatch_count(), n0);

    w32_dirwatch_sync = saved;
}
