/* test_dirwatch_core.c -- runtime/win32's directory watcher over the REAL core events.
 *
 * runtime/win32's own suite (tests/test_dirwatch.c) cannot link runtime/core, so it installs fake
 * events through w32_dirwatch_sync. This binary links libw32 + libcore + libhle the way the game
 * binary does and leaves w32_dirwatch_sync at its default: sg_core_event_create/set/reset and
 * sg_handle_close (sg_core.h section 5). It then drives the game's two call patterns from a
 * guest thread, waiting with the core's own WaitForSingleObject / WaitForMultipleObjectsEx shims:
 *   - FindFirstChangeNotificationA / FindNextChangeNotification / FindCloseChangeNotification
 *     (the fallback at 0x92fb20);
 *   - CreateEventA(0,0,0,0) as OVERLAPPED.hEvent, CreateFileW(BACKUP_SEMANTICS|OVERLAPPED),
 *     ReadDirectoryChangesW overlapped, WaitForMultipleObjectsEx {event, stop}, CancelIo,
 *     CloseHandle (the watcher thread at 0x92f8c0).
 * The FSEvents dispatch queue (a host thread that is not a guest thread) sets the events.
 * Our own temporary directory only; no game code or game bytes. */
#include <stdio.h>
#include <stdlib.h>
#include <string.h>
#include <sys/stat.h>
#include <time.h>
#include <unistd.h>

#include "hle.h"
#include "sg_core.h"
#include "w32.h"
#include "w32_dirwatch.h"
#include "w32_shims.h"

/* ---- what the image linker provides (patterned on runtime/core/tests/test_main.c) ------------- */
uint32_t sg_dispatch_bucket[SG_DISPATCH_BUCKETS + 1];
sg_dispatch_entry sg_dispatch_entries[1];
sg_resume_point sg_resume_points[1];
const uint32_t sg_resume_point_count = 0;
static sg_func *miss(sg_ctx *ctx, uint8_t *mem, uint32_t a) {
    (void)mem;
    sg_core_fatal(ctx, "dispatch miss at 0x%08x", a);
}
sg_miss_hook_fn sg_miss_hook = miss;

static int t_pass, t_fail;
#define CHECK(c) do { if (c) t_pass++; else { t_fail++; fprintf(stderr, "FAIL %s:%d: %s\n", __FILE__, __LINE__, #c); } } while (0)
#define CHECK_EQ(a, b) do { unsigned long long a_ = (unsigned long long)(a), b_ = (unsigned long long)(b); \
    if (a_ == b_) t_pass++; else { t_fail++; fprintf(stderr, "FAIL %s:%d: %s = 0x%llx, want 0x%llx\n", __FILE__, __LINE__, #a, a_, b_); } } while (0)

static uint8_t *M;
static sg_ctx *C;

/* Calls a shim as translated code does (stdcall: args, return address, callee pops). */
static uint32_t gcall(sg_func *fn, unsigned n, const uint32_t *args) {
    sg_ctx *ctx = C;
    uint8_t *mem = ctx->mem;
    uint32_t esp0 = C->esp;
    const uint32_t ra = 0x00401234u;
    for (unsigned k = n; k-- > 0;) SG_PUSH32(args[k]);
    SG_PUSH32(ra);
    fn(C, mem);
    if (C->eip != ra || C->esp != esp0) {
        t_fail++;
        fprintf(stderr, "FAIL stack discipline: eip 0x%08x esp delta %d\n", C->eip, (int)(C->esp - esp0));
        C->esp = esp0;
    }
    return C->eax;
}
#define G(fn, ...) gcall((sg_func *)(fn), sizeof((uint32_t[]){__VA_ARGS__}) / 4, (uint32_t[]){__VA_ARGS__})
static sg_func *core(const char *name) {
    sg_func *f = sg_core_shim_lookup(name);
    if (!f) { fprintf(stderr, "no core shim %s\n", name); exit(2); }
    return f;
}

static uint32_t gstr(const char *s, int wide) {
    uint32_t n = (uint32_t)strlen(s) + 1, a = hle_guest_alloc(wide ? 2 * n : n, 4);
    for (uint32_t i = 0; i < n; i++) {
        if (wide) sg_st16(M, a + 2 * i, (uint8_t)s[i]);
        else sg_st8(M, a + i, (uint8_t)s[i]);
    }
    return a;
}

static char root[1024], wdir[1200];
static void hwrite(const char *rel, const char *text) {
    char p[1400];
    snprintf(p, sizeof p, "%s/%s", wdir, rel);
    FILE *f = fopen(p, "a");
    if (f) { fputs(text, f); fclose(f); }
}
static double now_ms(void) {
    struct timespec ts;
    clock_gettime(CLOCK_MONOTONIC, &ts);
    return ts.tv_sec * 1e3 + ts.tv_nsec / 1e6;
}

/* FILE_NOTIFY_INFORMATION: 1 if (action, name) is among the records */
static int has_record(uint32_t buf, uint32_t bytes, uint32_t action, const char *name) {
    uint32_t off = 0;
    while (bytes) {
        uint32_t next = sg_ld32(M, buf + off), act = sg_ld32(M, buf + off + 4), nb = sg_ld32(M, buf + off + 8);
        char s[256];
        uint32_t k = 0;
        for (uint32_t i = 0; i < nb / 2 && k < sizeof s - 1; i++) s[k++] = (char)sg_ld16(M, buf + off + 12 + 2 * i);
        s[k] = 0;
        if (act == action && !strcmp(s, name)) return 1;
        if (!next || off + next >= bytes) break;
        off += next;
    }
    return 0;
}

int main(void) {
    hle_unimpl_policy = HLE_UNIMPL_CONTINUE;
    if (!getenv("SPORE_HLE_LOG")) hle_log_level = HLE_LOG_ERROR;
    char cwd[900];
    if (!getcwd(cwd, sizeof cwd)) return 2;
    snprintf(root, sizeof root, "%s/dirwatch-root", cwd);
    char cmd[1200];
    snprintf(cmd, sizeof cmd, "rm -rf '%s' && mkdir -p '%s/game/SporebinEP1' '%s/docs/watch' '%s/support' '%s/tmp'", root, root,
             root, root, root);
    if (system(cmd)) return 2;
    snprintf(wdir, sizeof wdir, "%s/docs/watch", root);
    char game[1100], sup[1100], docs[1100], tmpd[1100], reg[1100];
    snprintf(game, sizeof game, "%s/game", root);
    snprintf(sup, sizeof sup, "%s/support", root);
    snprintf(docs, sizeof docs, "%s/docs", root);
    snprintf(tmpd, sizeof tmpd, "%s/tmp", root);
    snprintf(reg, sizeof reg, "%s/support/registry.json", root);
    w32_config cfg;
    memset(&cfg, 0, sizeof cfg);
    cfg.game_dir = game;
    cfg.support_dir = sup;
    cfg.documents_dir = docs;
    cfg.temp_dir = tmpd;
    cfg.registry_file = reg;

    /* the game binary's start-up order (runtime/image/src/main.c): window, core, win32, core start-up */
    M = sg_window_reserve();
    if (!M) { fprintf(stderr, "cannot reserve the window\n"); return 2; }
    sg_core_init(M);
    if (w32_init(M, &cfg)) { fprintf(stderr, "w32_init failed\n"); return 2; }
    sg_core_startup(M); /* CloseHandle of core handles through runtime/win32: w32_close_foreign */
    sg_thread *t = sg_core_attach_main(0x100000u);
    if (!t) { fprintf(stderr, "no guest main thread\n"); return 2; }
    C = &t->ctx;

    sg_func *WaitForSingleObject = core("sg_imp_kernel32_WaitForSingleObject");
    sg_func *WaitForMultipleObjectsEx = core("sg_imp_kernel32_WaitForMultipleObjectsEx");
    sg_func *CreateEventA = core("sg_imp_kernel32_CreateEventA");
    sg_func *SetEvent = core("sg_imp_kernel32_SetEvent");

    /* 0. the default operations are the core's, not the weak stand-ins */
    CHECK(w32_dirwatch_sync.event_set == sg_core_event_set);
    CHECK(w32_dirwatch_sync.event_reset == sg_core_event_reset);
    CHECK(w32_dirwatch_sync.handle_close == sg_handle_close);
    {
        uint32_t h = w32_dirwatch_sync.event_create(1);
        CHECK(h != 0);
        CHECK_EQ(sg_handle_type(h), SG_H_EVENT);
        CHECK_EQ(w32_dirwatch_sync.handle_close(h), 1);
    }

    /* 1. FindFirstChangeNotificationA: the handle is a core manual-reset event */
    uint32_t gpath = gstr("C:\\Users\\Player\\Documents\\watch", 0);
    uint32_t h = G(w32_imp_kernel32_FindFirstChangeNotificationA, gpath, 0, 0x1 /* FILE_NAME */);
    CHECK(h != 0xffffffffu);
    CHECK_EQ(sg_handle_type(h), SG_H_EVENT);
    CHECK_EQ(G(WaitForSingleObject, h, 100), 0x102);
    double t0 = now_ms();
    hwrite("n1.txt", "x");
    CHECK_EQ(G(WaitForSingleObject, h, 5000), 0); /* guest thread woken by the FSEvents queue */
    fprintf(stderr, "  FindFirstChangeNotification: signalled %.0f ms after the write\n", now_ms() - t0);
    CHECK_EQ(G(WaitForSingleObject, h, 0), 0);    /* manual reset: stays signalled */
    CHECK_EQ(G(w32_imp_kernel32_FindNextChangeNotification, h), 1);
    CHECK_EQ(G(WaitForSingleObject, h, 0), 0x102); /* reset through sg_core_event_reset */
    hwrite("n2.txt", "y");
    CHECK_EQ(G(WaitForSingleObject, h, 5000), 0);
    CHECK_EQ(G(w32_imp_kernel32_FindCloseChangeNotification, h), 1);
    CHECK_EQ(sg_handle_type(h), 0);               /* closed through sg_handle_close */
    CHECK_EQ(G(WaitForSingleObject, h, 0), 0xffffffffu);

    /* 2. the watcher thread's pattern (0x92f8c0) with a guest CreateEventA event */
    uint32_t ev = G(CreateEventA, 0, 0, 0, 0);   /* auto-reset, as at 0x92f903 */
    uint32_t stop = G(CreateEventA, 0, 1, 0, 0);
    CHECK_EQ(sg_handle_type(ev), SG_H_EVENT);
    uint32_t wpath = gstr("C:\\Users\\Player\\Documents\\watch", 1);
    uint32_t dir = G(w32_imp_kernel32_CreateFileW, wpath, 0x1 /* FILE_LIST_DIRECTORY */, 7, 0, 3 /* OPEN_EXISTING */,
                     0x02000000u | 0x40000000u /* BACKUP_SEMANTICS | OVERLAPPED */, 0);
    CHECK(dir != 0xffffffffu);
    uint32_t buf = hle_guest_alloc(0x2000, 4), ov = hle_guest_alloc(20, 4), nbytes = hle_guest_alloc(4, 4);
    uint32_t pair = hle_guest_alloc(8, 4);
    sg_st32(M, ov + 16, ev);
    sg_st32(M, pair, ev);
    sg_st32(M, pair + 4, stop);
    CHECK_EQ(G(w32_imp_kernel32_ReadDirectoryChangesW, dir, buf, 0x2000, 1, 0x17f & ~0x20u, nbytes, ov, 0), 1);
    CHECK_EQ(sg_ld32(M, ov), 0x103);              /* STATUS_PENDING */
    CHECK_EQ(G(WaitForMultipleObjectsEx, 2, pair, 0, 100, 1), 0x102);
    int found = 0;
    t0 = now_ms();
    hwrite("a.txt", "hello");
    for (int i = 0; i < 20 && !found; i++) {
        uint32_t r = G(WaitForMultipleObjectsEx, 2, pair, 0, 5000, 1);
        CHECK_EQ(r, 0);                           /* the event, not the stop event */
        if (r != 0) break;
        CHECK_EQ(sg_ld32(M, ov), 0);
        found = has_record(buf, sg_ld32(M, ov + 4), 1 /* ADDED */, "a.txt");
        if (!found) CHECK_EQ(G(w32_imp_kernel32_ReadDirectoryChangesW, dir, buf, 0x2000, 1, 0x17f & ~0x20u, nbytes, ov, 0), 1);
    }
    CHECK(found);
    fprintf(stderr, "  ReadDirectoryChangesW: ADDED a.txt seen %.0f ms after the write\n", now_ms() - t0);
    CHECK_EQ(G(WaitForSingleObject, ev, 0), 0x102); /* auto-reset: taken by the wait */
    /* re-arm, then stop the way the game does: SetEvent(stop), CancelIo, CloseHandle */
    CHECK_EQ(G(w32_imp_kernel32_ReadDirectoryChangesW, dir, buf, 0x2000, 1, 0x17f & ~0x20u, nbytes, ov, 0), 1);
    CHECK_EQ(G(SetEvent, stop), 1);
    CHECK_EQ(G(WaitForMultipleObjectsEx, 2, pair, 0, 1000, 1), 1);
    CHECK_EQ(G(w32_imp_kernel32_CancelIo, dir), 1);
    CHECK_EQ(sg_ld32(M, ov), 0xC0000120u);        /* STATUS_CANCELLED, and the event set by the core API */
    CHECK_EQ(G(WaitForSingleObject, ev, 0), 0);
    unsigned live = w32_dirwatch_count();
    CHECK_EQ(G(w32_imp_kernel32_CloseHandle, dir), 1);
    CHECK_EQ(w32_dirwatch_count(), live - 1);
    CHECK_EQ(G(w32_imp_kernel32_CloseHandle, ev), 1);   /* runtime/win32's CloseHandle forwards to the core */
    CHECK_EQ(sg_handle_type(ev), 0);
    CHECK_EQ(G(w32_imp_kernel32_CloseHandle, stop), 1);
    CHECK_EQ(w32_dirwatch_count(), 0);

    printf("dirwatch over core events: %d passed, %d failed\n", t_pass, t_fail);
    snprintf(cmd, sizeof cmd, "rm -rf '%s'", root);
    if (!t_fail && system(cmd)) fprintf(stderr, "warning: cleanup failed\n");
    return t_fail ? 1 : 0;
}
