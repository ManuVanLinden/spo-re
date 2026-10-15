/* test_dragdrop.c -- OLE drag and drop through the guest layer, host-only (no AppKit, no game code).
 *
 * A fake IDropTarget (host functions at fixture guest addresses, as the game's vtable 0x140a08c would be) is
 * registered through the ole32 entries of the generated import table (so the constructor in sgp_dragdrop.c that puts
 * ours there is checked too). Drops are driven two ways: sgp_dnd_deliver directly, and through the host queue
 * (this file stands in for sp_dragdrop.m: sp_mac_dnd_attach/poll/event_free) by calling the notify function
 * RegisterDragDrop handed to the host. The fake target does what the game's does (QueryGetData, GetData,
 * DragQueryFileW count / size / copy, ReleaseStgMedium in DragLeave) and records the call order; the test
 * checks the DROPFILES bytes in the HGLOBAL, the Windows paths and the DragQueryFileW results.
 *
 * Files: the game's own cards from build/harness/sandbox/x/Documents/My Spore Creations/y/z.png (read in place, never
 * copied), or two empty .png files in a temporary folder when there are none. runtime/win32 is emulated here:
 * Documents -> the first card's sandbox Documents folder, and w32_path_mount is recorded. */
#include <glob.h>
#include <limits.h>
#include <stdlib.h>
#include <unistd.h>

#include "fixture.h"
#include "sp_macos.h"

uint32_t sgp_dnd_deliver(sg_ctx *ctx, const sp_dnd_event *e);
int sgp_dnd_windows_path(const char *host, char *win, size_t cap);
int sgp_dnd_live_objects(void);

/* ---- runtime/win32 stand-ins: GlobalAlloc family and the path API (found by dlsym) ------------------- */
static unsigned g_allocs, g_frees, g_locks, g_unlocks;
SG_IMPORT(kernel32, GlobalAlloc) {
    uint32_t a = hle_guest_alloc(SG_ARG(1) ? SG_ARG(1) : 1, 16);
    g_allocs++;
    SG_RETURN_STDCALL(a, 8);
}
SG_IMPORT(kernel32, GlobalFree) { g_frees++; SG_RETURN_STDCALL(0, 4); }
SG_IMPORT(kernel32, GlobalLock) { g_locks++; SG_RETURN_STDCALL(SG_ARG(0), 4); }
SG_IMPORT(kernel32, GlobalUnlock) { g_unlocks++; SG_RETURN_STDCALL(1, 4); }

static char docs_host[PATH_MAX];
static char mounted_win[256], mounted_host[PATH_MAX];
static unsigned n_mounts;
const char *w32_folder(int f) {
    switch (f) {
    case 0: return "C:\\Spore";
    case 3: return "C:\\Users\\Player\\Documents";
    case 4: return "C:\\Users\\Player\\AppData\\Roaming";
    case 8: return "C:\\Users\\Player\\AppData\\Local\\Temp";
    default: return "";
    }
}
int w32_path_to_host(const char *win, char *host, size_t cap, int *exists, int *parent_exists) {
    if (!strcmp(win, "C:\\Users\\Player\\Documents") && *docs_host) { snprintf(host, cap, "%s", docs_host); return 0; }
    if (!strcmp(win, "C:\\")) { snprintf(host, cap, "/nonexistent-drive-c"); return 0; }
    return -1;
}
int w32_path_mount(const char *win, const char *host) {
    snprintf(mounted_win, sizeof mounted_win, "%s", win);
    snprintf(mounted_host, sizeof mounted_host, "%s", host);
    n_mounts++;
    return 0;
}

/* ---- the fake IDropTarget -------------------------------------------------------------------------- */
#define DT_BASE 200
static char calls[1024];
static uint32_t target, t_fmt, t_med, t_buf, t_refs, enter_answer = 1;
static int t_count_seen, t_entered;
static char t_names[4][300];
static uint32_t t_len0, t_short_ret;
static uint16_t t_short[8];
static uint32_t t_qgd_ok, t_qgd_text, t_getdata;
static uint8_t t_dropfiles[2048];
static uint32_t t_dropfiles_len;

static void note(const char *s) {
    if (calls[0]) strncat(calls, " ", sizeof calls - strlen(calls) - 1);
    strncat(calls, s, sizeof calls - strlen(calls) - 1);
}

static sg_func *find_import(const char *name) {
    for (unsigned i = 0; i < hle_import_count; i++)
        if (hle_imports[i].name && !strcmp(hle_imports[i].name, name)) return hle_imports[i].fn;
    return NULL;
}
static uint32_t dqf(uint32_t h, uint32_t i, uint32_t buf, uint32_t cch) {
    return GSTD(find_import("DragQueryFileW"), h, i, buf, cch);
}

static void dt_qi(sg_ctx *ctx, uint8_t *mem) { note("QI"); SG_RETURN_STDCALL(0x80004002u, 12); }
static void dt_addref(sg_ctx *ctx, uint8_t *mem) { note("AddRef"); SG_RETURN_STDCALL(++t_refs, 4); }
static void dt_release(sg_ctx *ctx, uint8_t *mem) { note("Release"); SG_RETURN_STDCALL(--t_refs, 4); }
/* DragEnter(this, pDataObj, grfKeyState, pt.x, pt.y, pdwEffect), as 0x6bb8b0 */
static void dt_enter(sg_ctx *ctx, uint8_t *mem) {
    uint32_t dobj = SG_ARG(1), peff = SG_ARG(5), hr, n, i;
    char buf[64];
    snprintf(buf, sizeof buf, "DragEnter(%d,%d)", (int)SG_ARG(3), (int)SG_ARG(4));
    note(buf);
    t_entered++;
    sg_st16(mem, t_fmt, 15); /* CF_HDROP */
    sg_st32(mem, t_fmt + 4, 0);
    sg_st32(mem, t_fmt + 8, 1);
    sg_st32(mem, t_fmt + 12, 0xffffffffu);
    sg_st32(mem, t_fmt + 16, 1);
    hr = sg_call_guest_method(ctx, dobj, 5, 1, &t_fmt);         /* QueryGetData */
    t_qgd_ok = hr;
    sg_st16(mem, t_fmt, 13); /* CF_UNICODETEXT: not offered */
    t_qgd_text = sg_call_guest_method(ctx, dobj, 5, 1, &t_fmt);
    sg_st16(mem, t_fmt, 15);
    {
        uint32_t a[2] = {t_fmt, t_med};
        t_getdata = sg_call_guest_method(ctx, dobj, 3, 2, a); /* GetData */
    }
    if (!t_getdata && sg_ld32(mem, t_med) == 1 && !sg_ld32(mem, t_med + 8)) {
        uint32_t h = sg_ld32(mem, t_med + 4), off = sg_ld32(mem, h);
        uint32_t end = off;
        while (end + 4 < sizeof t_dropfiles && (sg_ld16(mem, h + end) || sg_ld16(mem, h + end + 2))) end += 2;
        t_dropfiles_len = end + 4;
        memcpy(t_dropfiles, mem + h, t_dropfiles_len);
        n = dqf(h, 0xffffffffu, t_buf, 0x208);
        t_count_seen = (int)n;
        t_len0 = dqf(h, 0, 0, 0);
        for (i = 0; i < n && i < 4; i++) {
            uint32_t k = dqf(h, i, t_buf, 0x208), j;
            for (j = 0; j < k && j < 299; j++) t_names[i][j] = (char)(sg_ld16(mem, t_buf + 2 * j) < 128 ? sg_ld16(mem, t_buf + 2 * j) : '?');
            t_names[i][j] = 0;
        }
        t_short_ret = dqf(h, 0, t_buf, 5);
        for (i = 0; i < 6; i++) t_short[i] = sg_ld16(mem, t_buf + 2 * i);
    }
    sg_st32(mem, peff, enter_answer && !t_getdata ? 1u : 0u);
    SG_RETURN_STDCALL(0, 24);
}
/* DragOver(this, grfKeyState, pt.x, pt.y, pdwEffect), as 0x6bb980 */
static void dt_over(sg_ctx *ctx, uint8_t *mem) {
    char buf[64];
    snprintf(buf, sizeof buf, "DragOver(%d,%d)", (int)SG_ARG(2), (int)SG_ARG(3));
    note(buf);
    sg_st32(mem, SG_ARG(4), enter_answer ? 1u : 0u);
    SG_RETURN_STDCALL(0, 20);
}
/* DragLeave(this), as 0x6bba10: releases the medium from DragEnter */
static void dt_leave(sg_ctx *ctx, uint8_t *mem) {
    note("DragLeave");
    GSTD(find_import("ReleaseStgMedium"), t_med);
    SG_RETURN_STDCALL(0, 4);
}
/* Drop(this, pDataObj, grfKeyState, pt.x, pt.y, pdwEffect), as 0x6bba40: answers DROPEFFECT_MOVE */
static void dt_drop(sg_ctx *ctx, uint8_t *mem) {
    char buf[64];
    snprintf(buf, sizeof buf, "Drop(%d,%d)", (int)SG_ARG(3), (int)SG_ARG(4));
    note(buf);
    sg_st32(mem, SG_ARG(5), 2);
    SG_RETURN_STDCALL(0, 24);
}

/* ---- the host queue stand-in (sp_dragdrop.m) --------------------------------------------------------- */
static sp_dnd_notify_fn host_fn;
static void *host_user;
static sp_dnd_event q[8];
static int qn, qhead, freed;
int sp_mac_dnd_attach(sp_hwnd h, sp_dnd_notify_fn fn, void *user) { host_fn = fn; host_user = user; return 1; }
void sp_mac_dnd_detach(sp_hwnd h) { host_fn = NULL; }
int sp_mac_dnd_poll(sp_dnd_event *out) {
    if (qhead >= qn) return 0;
    *out = q[qhead++];
    return 1;
}
void sp_mac_dnd_event_free(sp_dnd_event *e) { freed++; }

static int32_t wndproc(void *u, sp_hwnd h, uint32_t m, uint32_t w, int32_t l) { return 0; }

static int find_cards(char out[2][PATH_MAX]) {
    char pat[PATH_MAX], cwd[PATH_MAX];
    glob_t g;
    size_t i;
    int n = 0;
    const char *root = getenv("SPORE_REPO");
    if (!root) root = getcwd(cwd, sizeof cwd) ? cwd : ".";
    snprintf(pat, sizeof pat, "%s/build/harness/sandbox/*/Documents/My Spore Creations/*/*.png", root);
    if (glob(pat, 0, NULL, &g)) return 0;
    for (i = 0; i < g.gl_pathc && n < 2; i++) {
        /* the second card comes from another sandbox, so it is outside the emulated Documents mount */
        if (n == 1) {
            const char *a = strstr(out[0], "/sandbox/"), *b = strstr(g.gl_pathv[i], "/sandbox/");
            size_t la = strcspn(a + 9, "/"), lb = strcspn(b + 9, "/");
            if (la == lb && !strncmp(a, b, 9 + la)) continue;
        }
        if (!realpath(g.gl_pathv[i], out[n])) continue;
        n++;
    }
    globfree(&g);
    return n;
}

int main(void) {
    char cards[2][PATH_MAX], win[1024], expect0[1024], expect1[1024];
    char *files[3];
    sp_dnd_event e;
    uint32_t vt, r, eff, h;
    int ncards;
    if (fx_init()) { fprintf(stderr, "fixture init failed\n"); return 2; }
    sgp_set_backend(&sgp_backend_headless);
    sgp_ensure();
    h = sp_window_register(wndproc, NULL, 0);
    CHECK(h != 0, "window");

    /* files */
    ncards = find_cards(cards);
    if (ncards < 2) {
        char d[] = "/tmp/spore-dnd-XXXXXX";
        if (!mkdtemp(d)) return 2;
        snprintf(cards[0], PATH_MAX, "%s/Documents/My Spore Creations/Creatures/A card.png", d);
        snprintf(cards[1], PATH_MAX, "%s/elsewhere/B.png", d);
        snprintf(win, sizeof win, "mkdir -p '%s/Documents/My Spore Creations/Creatures' '%s/elsewhere' && touch '%s' '%s'",
                 d, d, cards[0], cards[1]);
        if (system(win)) return 2;
        printf("no sandbox cards: using empty stand-ins under %s\n", d);
    } else {
        printf("cards: %s\n       %s\n", cards[0], cards[1]);
    }
    {
        char *p = strstr(cards[0], "/Documents/My Spore Creations/");
        snprintf(docs_host, sizeof docs_host, "%.*s/Documents", (int)(p - cards[0]), cards[0]);
        snprintf(expect0, sizeof expect0, "C:\\Users\\Player\\Documents\\%s", p + strlen("/Documents/"));
        for (p = expect0; *p; p++) if (*p == '/') *p = '\\';
        p = strrchr(cards[1], '/');
        snprintf(expect1, sizeof expect1, "C:\\Users\\Player\\Dropped\\0\\%s", p + 1);
    }

    /* the Windows view */
    CHECK(!sgp_dnd_windows_path(cards[0], win, sizeof win) && !strcmp(win, expect0), "path 0: %s, want %s", win, expect0);
    CHECK(!sgp_dnd_windows_path(cards[1], win, sizeof win) && !strcmp(win, expect1), "path 1: %s, want %s", win, expect1);
    CHECK_EQ(n_mounts, 1);
    CHECK(!strcmp(mounted_win, "C:\\Users\\Player\\Dropped\\0"), "mounted %s", mounted_win);
    CHECK(!strncmp(mounted_host, cards[1], strlen(mounted_host)) && cards[1][strlen(mounted_host)] == '/', "mount host %s",
          mounted_host);
    CHECK(!sgp_dnd_windows_path(cards[1], win, sizeof win) && n_mounts == 1, "the folder's mount is reused");
    {
        char longp[PATH_MAX];
        snprintf(longp, sizeof longp, "%s/%0250d.png", docs_host, 7); /* does not exist: realpath fails */
        CHECK(sgp_dnd_windows_path(longp, win, sizeof win) != 0, "a missing file has no Windows path");
    }

    /* the fake target */
    vt = hle_guest_alloc(7 * 4, 16);
    target = hle_guest_alloc(16, 16);
    t_fmt = hle_guest_alloc(32, 16);
    t_med = hle_guest_alloc(16, 16);
    t_buf = hle_guest_alloc(0x210 * 2, 16);
    {
        sg_func *fns[7] = {dt_qi, dt_addref, dt_release, dt_enter, dt_over, dt_leave, dt_drop};
        for (int k = 0; k < 7; k++) {
            fx_register(FX_FN(DT_BASE + k), fns[k]);
            sg_st32(M, vt + 4u * k, FX_FN(DT_BASE + k));
        }
    }
    sg_st32(M, target, vt);

    /* ole32 through the table, as 0x6bbb30 calls it */
    CHECK_EQ(GSTD(find_import("OleInitialize"), 0), 0);
    CHECK_EQ(GSTD(find_import("OleInitialize"), 0), 1);   /* S_FALSE: already */
    GSTD0(find_import("OleUninitialize"));
    CHECK_EQ(GSTD(find_import("CoLockObjectExternal"), target, 1, 0), 0);
    CHECK_EQ(GSTD(find_import("RegisterDragDrop"), 0x7777u, target), 0x80040102u); /* not a window */
    CHECK_EQ(GSTD(find_import("RegisterDragDrop"), h, 0), 0x80070057u);
    CHECK_EQ(GSTD(find_import("RegisterDragDrop"), h, target), 0);
    CHECK_EQ(t_refs, 1);
    CHECK(host_fn != NULL, "the host got a notify function");
    CHECK_EQ(GSTD(find_import("RegisterDragDrop"), h, target), 0x80040101u); /* already registered */

    /* 1: enter with two cards and a text file, move, drop (sgp_dnd_deliver) */
    calls[0] = 0;
    files[0] = cards[0], files[1] = "/etc/hosts", files[2] = cards[1];
    e = (sp_dnd_event){SP_DND_ENTER, h, 10, 20, 1, 3, files};
    eff = sgp_dnd_deliver(C, &e);
    CHECK_EQ(eff, 1);
    CHECK_EQ(t_qgd_ok, 0);
    CHECK_EQ(t_qgd_text, 0x80040064u);
    CHECK_EQ(t_getdata, 0);
    CHECK_EQ(t_count_seen, 2);
    CHECK_EQ(t_len0, strlen(expect0));
    CHECK(!strcmp(t_names[0], expect0) || ncards < 2 || strchr(expect0, 0x80), "name 0: %s", t_names[0]);
    CHECK(!strcmp(t_names[1], expect1), "name 1: %s want %s", t_names[1], expect1);
    CHECK_EQ(t_short_ret, 4);
    CHECK(t_short[0] == 'C' && t_short[1] == ':' && t_short[2] == '\\' && t_short[3] == 'U' && t_short[4] == 0, "cch 5");
    CHECK_EQ(dqf(sg_ld32(M, t_med + 4), 2, t_buf, 0x208), 0); /* past the end */
    {
        /* DROPFILES: pFiles 20, pt = client point, fNC 0, fWide 1, the names, double NUL */
        uint32_t *hd = (uint32_t *)t_dropfiles, off = 20, k;
        CHECK(hd[0] == 20 && hd[1] == 10 && hd[2] == 20 && hd[3] == 0 && hd[4] == 1, "DROPFILES header %u %u %u %u %u", hd[0],
              hd[1], hd[2], hd[3], hd[4]);
        for (k = 0; k < 2; k++) {
            const char *want = k ? expect1 : expect0;
            uint32_t j, n = (uint32_t)strlen(want), same = 1;
            for (j = 0; j < n; j++) same &= (t_dropfiles[off + 2 * j] | t_dropfiles[off + 2 * j + 1] << 8) == (uint8_t)want[j];
            CHECK(same || (unsigned char)want[0] >= 0x80, "DROPFILES name %u", k);
            CHECK(!t_dropfiles[off + 2 * n] && !t_dropfiles[off + 2 * n + 1], "NUL after name %u", k);
            off += 2 * (n + 1);
        }
        CHECK(off + 2 == t_dropfiles_len && !t_dropfiles[off] && !t_dropfiles[off + 1], "double NUL at %u (len %u)", off,
              t_dropfiles_len);
    }
    e = (sp_dnd_event){SP_DND_OVER, h, 11, 21, 1, 0, NULL};
    CHECK_EQ(sgp_dnd_deliver(C, &e), 1);
    e = (sp_dnd_event){SP_DND_DROP, h, 12, 22, 0, 3, files};
    CHECK_EQ(sgp_dnd_deliver(C, &e), 2);
    /* the headless host's screen is the client (no window frame), so screen point = client point */
    CHECK(!strcmp(calls, "DragEnter(10,20) DragOver(11,21) Drop(12,22)"), "calls: %s", calls);
    CHECK_EQ(sgp_dnd_live_objects(), 0);

    /* 2: through the host queue and notify: enter, leave (the target releases the medium) */
    calls[0] = 0;
    g_frees = 0;
    files[0] = cards[1];
    q[0] = (sp_dnd_event){SP_DND_ENTER, h, 5, 6, 1, 1, files};
    q[1] = (sp_dnd_event){SP_DND_OVER, h, 7, 8, 1, 0, NULL};
    q[2] = (sp_dnd_event){SP_DND_LEAVE, h, 0, 0, 0, 0, NULL};
    qn = 3, qhead = 0, freed = 0;
    r = host_fn(host_user);
    CHECK_EQ(r, 0); /* the last event (DragLeave) answers nothing */
    CHECK_EQ(freed, 3);
    CHECK_EQ(t_count_seen, 1);
    CHECK(!strcmp(t_names[0], expect1), "queued name: %s", t_names[0]);
    CHECK(!strcmp(calls, "DragEnter(5,6) DragOver(7,8) DragLeave"), "calls: %s", calls);
    CHECK_EQ(g_frees, 1);  /* ReleaseStgMedium -> GlobalFree */
    CHECK_EQ(sg_ld32(M, t_med), 0); /* and the medium is zeroed */
    CHECK_EQ(sgp_dnd_live_objects(), 0);

    /* 3: only a text file: nothing is offered, the target is not called */
    calls[0] = 0;
    files[0] = "/etc/hosts";
    e = (sp_dnd_event){SP_DND_ENTER, h, 1, 1, 1, 1, files};
    CHECK_EQ(sgp_dnd_deliver(C, &e), 0);
    e = (sp_dnd_event){SP_DND_DROP, h, 1, 1, 0, 1, files};
    CHECK_EQ(sgp_dnd_deliver(C, &e), 0);
    CHECK(!calls[0], "calls: %s", calls);

    /* 4: a target that refuses (DROPEFFECT_NONE) gets DragLeave instead of Drop, as DoDragDrop does */
    calls[0] = 0;
    enter_answer = 0;
    files[0] = cards[0];
    e = (sp_dnd_event){SP_DND_ENTER, h, 3, 4, 1, 1, files};
    CHECK_EQ(sgp_dnd_deliver(C, &e), 0);
    e = (sp_dnd_event){SP_DND_DROP, h, 3, 4, 0, 1, files};
    CHECK_EQ(sgp_dnd_deliver(C, &e), 0);
    CHECK(!strcmp(calls, "DragEnter(3,4) DragLeave"), "calls: %s", calls);
    enter_answer = 1;

    /* 5: a drop with no enter first (the view was registered mid-drag): Enter, then Drop */
    calls[0] = 0;
    e = (sp_dnd_event){SP_DND_DROP, h, 9, 9, 0, 1, files};
    CHECK_EQ(sgp_dnd_deliver(C, &e), 2);
    CHECK(!strcmp(calls, "DragEnter(9,9) Drop(9,9)"), "calls: %s", calls);

    /* IDataObject odds and ends: QI for IUnknown and IDataObject, refcount, an unknown IID */
    {
        uint32_t d = 0, out = hle_guest_alloc(16, 16), iid = hle_guest_alloc(16, 16), a[2];
        files[0] = cards[0];
        extern uint32_t sgp_dnd_data_object(uint8_t *, char *const *, int, int32_t, int32_t);
        d = sgp_dnd_data_object(M, files, 1, 0, 0);
        CHECK(d != 0, "data object");
        memset(M + iid, 0, 16);
        M[iid + 8] = 0xc0, M[iid + 15] = 0x46;
        a[0] = iid, a[1] = out;
        CHECK_EQ(sg_call_guest_method(C, d, 0, 2, a), 0);       /* IUnknown */
        CHECK_EQ(sg_ld32(M, out), d);
        M[iid] = 0x0e, M[iid + 1] = 1;
        CHECK_EQ(sg_call_guest_method(C, d, 0, 2, a), 0);       /* IDataObject */
        M[iid] = 0x22;
        CHECK_EQ(sg_call_guest_method(C, d, 0, 2, a), 0x80004002u);
        CHECK_EQ(sg_call_guest_method(C, d, 2, 0, NULL), 2);    /* 3 refs -> 2 */
        CHECK_EQ(sg_call_guest_method(C, d, 2, 0, NULL), 1);
        CHECK_EQ(sg_call_guest_method(C, d, 2, 0, NULL), 0);
        CHECK_EQ(sgp_dnd_live_objects(), 0);
    }

    /* revoke */
    calls[0] = 0;
    CHECK_EQ(GSTD(find_import("RevokeDragDrop"), h), 0);
    CHECK_EQ(t_refs, 0);
    CHECK(!strcmp(calls, "Release"), "calls: %s", calls);
    CHECK_EQ(GSTD(find_import("RevokeDragDrop"), h), 0x80040100u);
    CHECK(host_fn == NULL, "detached");
    printf("drag and drop tests: %d passed, %d failed\n", t_pass, t_fail);
    return t_fail ? 1 : 0;
}
