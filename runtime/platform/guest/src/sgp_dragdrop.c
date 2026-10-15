/* sgp_dragdrop.c -- OLE drag and drop of files onto the game window (docs/design/platform-macos.md,
 * docs/DEVIATIONS.md 4.x "drag and drop").
 *
 * [obs] App init 0x6bbb30 calls OleInitialize(NULL), builds an IDropTarget (vtable 0x140a08c: QI, AddRef,
 * Release, DragEnter 0x6bb8b0, DragOver 0x6bb980, DragLeave 0x6bba10, Drop 0x6bba40) holding a FORMATETC
 * {CF_HDROP, NULL, DVASPECT_CONTENT, -1, TYMED_HGLOBAL} at +0x10 and a STGMEDIUM at +0x24, then
 * CoLockObjectExternal(target, TRUE, FALSE) and RegisterDragDrop(hwnd, target). DragEnter asks the data object
 * QueryGetData then GetData for that format and, through 0x6bbc60, reads the names with DragQueryFileW(hdrop,
 * 0xFFFFFFFF) and DragQueryFileW(hdrop, i, buf, 0x208) (a 260-WCHAR stack buffer, so every path we hand out is
 * shorter than MAX_PATH); DragOver answers DROPEFFECT_COPY; Drop answers DROPEFFECT_MOVE; DragLeave calls
 * ReleaseStgMedium on the medium from DragEnter. Each of Enter/Drop/Leave posts a message to the game with the
 * file list (0x24ce124 / 0x24ce123 / 0x411e1e8d).
 *
 * What this file provides:
 *   - ole32 OleInitialize, OleUninitialize, CoLockObjectExternal, RegisterDragDrop, RevokeDragDrop,
 *     ReleaseStgMedium, as strong sg_imp_ole32_* over runtime/hle/src/ole_stub.c's six weak ones (see HLE_OLE32_WEAK
 *     below). Nothing is removed there. SPORE_DRAGDROP=0 makes
 *     RegisterDragDrop keep nothing, as ole_stub.c does.
 *   - shell32 DragQueryFileW (strong over the generated weak stub; the game imports no DragFinish).
 *   - a guest IDataObject (vtable of thunks from sg_thunk_register, 16-byte records like runtime/com's, which this
 *     package does not link: the guest suite links libcore and libhle only) offering CF_HDROP on TYMED_HGLOBAL.
 *     GetData allocates a fresh HGLOBAL with the game's own GlobalAlloc (runtime/win32), holding a DROPFILES
 *     {pFiles 20, pt client point, fNC 0, fWide 1} and the Windows paths, double-NUL terminated; the caller
 *     owns it (pUnkForRelease NULL), as with the shell's data object.
 *   - delivery: runtime/platform/macos's sp_dragdrop.m queues host events and calls our notify function on the
 *     main thread while it is inside the event drain (so inside PeekMessage or another shim of the game
 *     thread, the same place window procedures are called from); we drain the queue there and call the
 *     registered IDropTarget's DragEnter/DragOver/DragLeave/Drop through sgp_guest_call. sgp_dnd_deliver is the
 *     same entry for tests.
 *   - paths: the Windows view of each dropped file. A file inside a mounted folder (Documents, Temp, AppData,
 *     the install root, drive C:) gets its natural path; any other file gets a mount of its folder at
 *     C:\Users\Player\Dropped\<k> (eight slots, reused round robin). Only .png files are offered; names Windows
 *     cannot hold, and paths of MAX_PATH or more, are left out. Without runtime/win32 (host tests) the view is
 *     Wine's Z:\ drive. */
#include <dlfcn.h>
#include <limits.h>
#include <stdlib.h>
#include <string.h>
#include <strings.h>

#include "sgp_int.h"
#include "sp_macos.h"

/* ---- constants ------------------------------------------------------------------------------------- */
#define S_OK_ 0x00000000u
#define S_FALSE_ 0x00000001u
#define E_NOTIMPL_ 0x80004001u
#define E_NOINTERFACE_ 0x80004002u
#define E_POINTER_ 0x80004003u
#define E_OUTOFMEMORY_ 0x8007000eu
#define E_INVALIDARG_ 0x80070057u
#define DV_E_FORMATETC_ 0x80040064u
#define DV_E_LINDEX_ 0x80040068u
#define DV_E_TYMED_ 0x80040069u
#define DV_E_DVASPECT_ 0x8004006bu
#define OLE_E_ADVISENOTSUPPORTED_ 0x80040003u
#define DATA_S_SAMEFORMATETC_ 0x00040130u
#define DRAGDROP_E_NOTREGISTERED_ 0x80040100u
#define DRAGDROP_E_ALREADYREGISTERED_ 0x80040101u
#define DRAGDROP_E_INVALIDHWND_ 0x80040102u
#define CF_HDROP_ 15u
#define TYMED_HGLOBAL_ 1u
#define TYMED_FILE_ 2u
#define TYMED_ISTREAM_ 4u
#define TYMED_ISTORAGE_ 8u
#define DVASPECT_CONTENT_ 1u
#define GHND_ 0x42u /* GMEM_MOVEABLE | GMEM_ZEROINIT */
#define DROPEFFECT_COPY_ 1u
#define DND_MAX_PATH 260u      /* MAX_PATH, the NUL included */
#define DND_MAX_FILES 64
#define DND_MAX_OBJ 8
#define DND_MAX_REG 4
#define DND_MAGIC 0x32345057u  /* an arbitrary tag value */

/* IDropTarget slots */
enum { DT_QI, DT_ADDREF, DT_RELEASE, DT_ENTER, DT_OVER, DT_LEAVE, DT_DROP };

/* ---- the host side's weak defaults (sp_dragdrop.m has the real ones) ------------------------------ */
__attribute__((weak)) int sp_mac_dnd_attach(sp_hwnd h, sp_dnd_notify_fn fn, void *user) { return 0; }
__attribute__((weak)) void sp_mac_dnd_detach(sp_hwnd h) {}
__attribute__((weak)) int sp_mac_dnd_poll(sp_dnd_event *out) { return 0; }
__attribute__((weak)) void sp_mac_dnd_event_free(sp_dnd_event *e) {}

/* ---- state ------------------------------------------------------------------------------------------ */
typedef struct dnd_obj {
    int used;
    uint32_t refs;
    uint32_t guest;            /* 16-byte record {vtbl, DND_MAGIC, index, 0}, kept for reuse */
    int nfiles;
    uint16_t *wfiles;          /* the names, each NUL-terminated, then one more NUL */
    uint32_t wunits;           /* units in wfiles, both final NULs included */
    int32_t cx, cy;            /* client point for DROPFILES.pt */
} dnd_obj;

typedef struct dnd_reg { uint32_t hwnd, target; } dnd_reg;

static dnd_obj objs[DND_MAX_OBJ];
static dnd_reg regs[DND_MAX_REG];
static uint32_t do_vtbl;       /* IDataObject guest vtable */
static uint32_t scratch;       /* guest DWORD for pdwEffect */
__attribute__((unused)) static int ole_inits; /* the ole32 entries are under HLE_OLE32_WEAK */
static struct { int active; uint32_t hwnd, target, dobj, eff; } sess; /* eff: the target's last answer */
static int delivering;
static uint64_t stat_events, stat_drops;

int sgp_dnd_enabled(void) {
    const char *e = getenv("SPORE_DRAGDROP");
    return !(e && *e == '0');
}

/* ---- Windows view of a host path --------------------------------------------------------------------- */
typedef const char *(*w32_folder_fn)(int);
typedef int (*w32_to_host_fn)(const char *, char *, size_t, int *, int *);
typedef int (*w32_mount_fn)(const char *, const char *);
static struct { int looked; w32_folder_fn folder; w32_to_host_fn to_host; w32_mount_fn mount; } w32;
static char drop_dirs[8][PATH_MAX];
static unsigned drop_next;

static void w32_lookup(void) {
    if (w32.looked) return;
    w32.looked = 1;
    w32.folder = (w32_folder_fn)dlsym(RTLD_DEFAULT, "w32_folder");
    w32.to_host = (w32_to_host_fn)dlsym(RTLD_DEFAULT, "w32_path_to_host");
    w32.mount = (w32_mount_fn)dlsym(RTLD_DEFAULT, "w32_path_mount");
}

static int win_name_ok(const char *name) {
    const char *p;
    if (!*name) return 0;
    for (p = name; *p; p++)
        if ((unsigned char)*p < 0x20 || strchr("\\:*?\"<>|", *p)) return 0;
    return 1;
}

/* rest: a host path tail with '/' separators; appended to root with '\'. 0 if it fits under MAX_PATH. */
static int join_win(char *out, size_t cap, const char *root, const char *rest) {
    size_t n = (size_t)snprintf(out, cap, "%s%s%s", root, (*root && root[strlen(root) - 1] == '\\') ? "" : "\\", rest);
    char *p;
    if (n >= cap) return -1;
    for (p = out; *p; p++) if (*p == '/') *p = '\\';
    return 0;
}

static uint32_t utf8_units(const char *s); /* below */

/* enum w32_folder (runtime/win32/include/w32.h): the mounted folders, most specific first */
static const int mounted_folders[] = {3 /* DOCUMENTS */, 8 /* TEMP */, 4 /* APPDATA */, 0 /* INSTALL */};

int sgp_dnd_windows_path(const char *host, char *win, size_t cap) {
    char real[PATH_MAX], hr[PATH_MAX], h[PATH_MAX], best_root[PATH_MAX] = "";
    const char *name, *best_rest = NULL;
    size_t bestlen = 0, i;
    if (!realpath(host, real)) return -1;
    name = strrchr(real, '/');
    name = name ? name + 1 : real;
    if (!win_name_ok(name)) return -1;
    w32_lookup();
    if (w32.folder && w32.to_host && w32.mount) {
        const char *roots[sizeof mounted_folders / sizeof mounted_folders[0] + 1];
        for (i = 0; i < sizeof mounted_folders / sizeof mounted_folders[0]; i++) roots[i] = w32.folder(mounted_folders[i]);
        roots[i] = "C:\\";
        for (i = 0; i < sizeof roots / sizeof roots[0]; i++) {
            size_t l;
            if (!roots[i] || !*roots[i] || w32.to_host(roots[i], h, sizeof h, NULL, NULL) || !realpath(h, hr)) continue;
            l = strlen(hr);
            if (l > bestlen && !strncmp(real, hr, l) && real[l] == '/') {
                bestlen = l;
                best_rest = real + l + 1;
                snprintf(best_root, sizeof best_root, "%s", roots[i]);
            }
        }
        if (best_rest) {
            const char *c;
            int ok = 1;
            /* every component must be a name Windows can hold */
            for (c = best_rest; *c && ok;) {
                char comp[NAME_MAX + 1];
                size_t k = strcspn(c, "/");
                if (k > NAME_MAX) { ok = 0; break; }
                memcpy(comp, c, k);
                comp[k] = 0;
                ok = win_name_ok(comp);
                c += k;
                while (*c == '/') c++;
            }
            if (ok && !join_win(win, cap, best_root, best_rest) && utf8_units(win) < DND_MAX_PATH) return 0;
        }
        /* outside every mount (or too long): mount its folder at C:\Users\Player\Dropped\<k> */
        {
            char dir[PATH_MAX], root[64];
            unsigned k;
            snprintf(dir, sizeof dir, "%.*s", (int)(name - real - 1), real);
            if (!*dir) snprintf(dir, sizeof dir, "/");
            for (k = 0; k < 8 && strcmp(drop_dirs[k], dir); k++) {}
            if (k == 8) {
                k = drop_next++ % 8u;
                snprintf(drop_dirs[k], sizeof drop_dirs[k], "%s", dir);
                snprintf(root, sizeof root, "C:\\Users\\Player\\Dropped\\%u", k);
                if (w32.mount(root, dir)) return -1;
                SGP_LOG("dragdrop: %s mounted at %s", dir, root);
            }
            snprintf(root, sizeof root, "C:\\Users\\Player\\Dropped\\%u", k);
            if (join_win(win, cap, root, name) || utf8_units(win) >= DND_MAX_PATH) return -1;
            return 0;
        }
    }
    /* no runtime/win32: Wine's view of the host root */
    if (join_win(win, cap, "Z:", real + 1) || utf8_units(win) >= DND_MAX_PATH) return -1;
    return 0;
}

/* ---- UTF-8 -> UTF-16 ---------------------------------------------------------------------------------- */
static uint32_t utf8_next(const char **ps) {
    const unsigned char *s = (const unsigned char *)*ps;
    uint32_t c = *s++;
    int n = 0;
    if (c >= 0xf0 && c < 0xf8) c &= 7, n = 3;
    else if (c >= 0xe0) c &= 15, n = 2;
    else if (c >= 0xc0) c &= 31, n = 1;
    else if (c >= 0x80) c = 0xfffd;
    while (n-- > 0) {
        if ((*s & 0xc0) != 0x80) { c = 0xfffd; break; }
        c = (c << 6) | (*s++ & 0x3fu);
    }
    *ps = (const char *)s;
    return c;
}
static uint32_t utf8_to_utf16(const char *s, uint16_t *out, uint32_t cap) {
    uint32_t n = 0;
    while (*s) {
        uint32_t c = utf8_next(&s);
        if (c >= 0x10000) {
            c -= 0x10000;
            if (out && n + 1 < cap) out[n] = (uint16_t)(0xd800 + (c >> 10)), out[n + 1] = (uint16_t)(0xdc00 + (c & 0x3ff));
            n += 2;
        } else {
            if (out && n < cap) out[n] = (uint16_t)c;
            n++;
        }
    }
    return n;
}
static uint32_t utf8_units(const char *s) { return utf8_to_utf16(s, NULL, 0); }

static int is_png(const char *p) {
    size_t n = strlen(p);
    return n > 4 && !strcasecmp(p + n - 4, ".png");
}

/* ---- the IDataObject --------------------------------------------------------------------------------- */
static dnd_obj *obj_of(const uint8_t *mem, uint32_t self) {
    uint32_t k;
    if (!self || sg_ld32(mem, self + 4) != DND_MAGIC) return NULL;
    k = sg_ld32(mem, self + 8);
    return k < DND_MAX_OBJ && objs[k].used && objs[k].guest == self ? &objs[k] : NULL;
}

static void obj_free(dnd_obj *o) {
    free(o->wfiles);
    o->wfiles = NULL;
    o->used = 0;
}

static uint32_t format_ok(const uint8_t *mem, uint32_t fmt) {
    if (!fmt) return E_INVALIDARG_;
    if (sg_ld16(mem, fmt) != CF_HDROP_) return DV_E_FORMATETC_;
    if (sg_ld32(mem, fmt + 8) != DVASPECT_CONTENT_) return DV_E_DVASPECT_;
    if (sg_ld32(mem, fmt + 12) != 0xffffffffu) return DV_E_LINDEX_;
    if (!(sg_ld32(mem, fmt + 16) & TYMED_HGLOBAL_)) return DV_E_TYMED_;
    return S_OK_;
}

static void do_qi(sg_ctx *ctx, uint8_t *mem) {
    static const uint8_t iid_unknown[16] = {0, 0, 0, 0, 0, 0, 0, 0, 0xc0, 0, 0, 0, 0, 0, 0, 0x46};
    static const uint8_t iid_dataobj[16] = {0x0e, 1, 0, 0, 0, 0, 0, 0, 0xc0, 0, 0, 0, 0, 0, 0, 0x46};
    uint32_t self = SG_ARG(0), riid = SG_ARG(1), ppv = SG_ARG(2);
    dnd_obj *o = obj_of(mem, self);
    if (!ppv) SG_RETURN_STDCALL(E_POINTER_, 12);
    sg_st32(mem, ppv, 0);
    if (!o || !riid) SG_RETURN_STDCALL(E_INVALIDARG_, 12);
    if (memcmp(mem + riid, iid_unknown, 16) && memcmp(mem + riid, iid_dataobj, 16)) SG_RETURN_STDCALL(E_NOINTERFACE_, 12);
    o->refs++;
    sg_st32(mem, ppv, self);
    SG_RETURN_STDCALL(S_OK_, 12);
}
static void do_addref(sg_ctx *ctx, uint8_t *mem) {
    dnd_obj *o = obj_of(mem, SG_ARG(0));
    SG_RETURN_STDCALL(o ? ++o->refs : 0, 4);
}
static void do_release(sg_ctx *ctx, uint8_t *mem) {
    dnd_obj *o = obj_of(mem, SG_ARG(0));
    uint32_t r = 0;
    if (o && o->refs) {
        r = --o->refs;
        if (!r) obj_free(o);
    }
    SG_RETURN_STDCALL(r, 4);
}
static uint32_t make_hdrop(sg_ctx *ctx, uint8_t *mem, const dnd_obj *o) {
    uint32_t size = 20u + 2u * o->wunits, a[1], h, p, i;
    {
        uint32_t ga[2] = {GHND_, size};
        h = sgp_call_import(ctx, "GlobalAlloc", 2, ga);
    }
    if (!h) return 0;
    a[0] = h;
    p = sgp_call_import(ctx, "GlobalLock", 1, a);
    if (!p) p = h;
    sg_st32(mem, p + 0, 20u);                    /* pFiles */
    sg_st32(mem, p + 4, (uint32_t)o->cx);        /* pt */
    sg_st32(mem, p + 8, (uint32_t)o->cy);
    sg_st32(mem, p + 12, 0);                     /* fNC */
    sg_st32(mem, p + 16, 1);                     /* fWide */
    for (i = 0; i < o->wunits; i++) sg_st16(mem, p + 20 + 2 * i, o->wfiles[i]);
    sgp_call_import(ctx, "GlobalUnlock", 1, a);
    return h;
}
static void do_getdata(sg_ctx *ctx, uint8_t *mem) {
    uint32_t self = SG_ARG(0), fmt = SG_ARG(1), med = SG_ARG(2), r, h;
    dnd_obj *o = obj_of(mem, self);
    if (!o || !med) SG_RETURN_STDCALL(E_INVALIDARG_, 12);
    r = format_ok(mem, fmt);
    if (r) SG_RETURN_STDCALL(r, 12);
    h = make_hdrop(ctx, mem, o);
    if (!h) SG_RETURN_STDCALL(E_OUTOFMEMORY_, 12);
    sg_st32(mem, med + 0, TYMED_HGLOBAL_);
    sg_st32(mem, med + 4, h);
    sg_st32(mem, med + 8, 0);
    SGP_TRACE("dragdrop: IDataObject::GetData(CF_HDROP) -> HGLOBAL 0x%x, %d files", h, o->nfiles);
    SG_RETURN_STDCALL(S_OK_, 12);
}
static void do_getdatahere(sg_ctx *ctx, uint8_t *mem) { SG_RETURN_STDCALL(E_NOTIMPL_, 12); }
static void do_querygetdata(sg_ctx *ctx, uint8_t *mem) {
    dnd_obj *o = obj_of(mem, SG_ARG(0));
    SG_RETURN_STDCALL(o ? format_ok(mem, SG_ARG(1)) : E_INVALIDARG_, 8);
}
static void do_canonical(sg_ctx *ctx, uint8_t *mem) {
    uint32_t in = SG_ARG(1), out = SG_ARG(2), i;
    if (!out) SG_RETURN_STDCALL(E_POINTER_, 12);
    if (in) for (i = 0; i < 20; i += 4) sg_st32(mem, out + i, sg_ld32(mem, in + i));
    sg_st32(mem, out + 4, 0); /* ptd */
    SG_RETURN_STDCALL(DATA_S_SAMEFORMATETC_, 12);
}
static void do_setdata(sg_ctx *ctx, uint8_t *mem) { SG_RETURN_STDCALL(E_NOTIMPL_, 16); }
static void do_enumformat(sg_ctx *ctx, uint8_t *mem) {
    if (SG_ARG(2)) sg_st32(mem, SG_ARG(2), 0);
    SG_RETURN_STDCALL(E_NOTIMPL_, 12);
}
static void do_dadvise(sg_ctx *ctx, uint8_t *mem) { SG_RETURN_STDCALL(OLE_E_ADVISENOTSUPPORTED_, 20); }
static void do_dunadvise(sg_ctx *ctx, uint8_t *mem) { SG_RETURN_STDCALL(OLE_E_ADVISENOTSUPPORTED_, 8); }
static void do_enumdadvise(sg_ctx *ctx, uint8_t *mem) {
    if (SG_ARG(1)) sg_st32(mem, SG_ARG(1), 0);
    SG_RETURN_STDCALL(OLE_E_ADVISENOTSUPPORTED_, 8);
}

static int vtbl_init(uint8_t *mem) {
    static sg_func *const fns[12] = {do_qi, do_addref, do_release, do_getdata, do_getdatahere, do_querygetdata,
                                     do_canonical, do_setdata, do_enumformat, do_dadvise, do_dunadvise, do_enumdadvise};
    static const char *const names[12] = {
        "IDataObject::QueryInterface", "IDataObject::AddRef", "IDataObject::Release", "IDataObject::GetData",
        "IDataObject::GetDataHere", "IDataObject::QueryGetData", "IDataObject::GetCanonicalFormatEtc",
        "IDataObject::SetData", "IDataObject::EnumFormatEtc", "IDataObject::DAdvise", "IDataObject::DUnadvise",
        "IDataObject::EnumDAdvise"};
    unsigned i;
    if (do_vtbl) return 0;
    do_vtbl = hle_guest_alloc(4u * 12u, 16);
    scratch = hle_guest_alloc(16, 16);
    if (!do_vtbl || !scratch) return -1;
    for (i = 0; i < 12; i++) sg_st32(mem, do_vtbl + 4u * i, sg_thunk_register(fns[i], names[i]));
    return 0;
}

/* A data object (one reference, the caller's) for the .png files among host paths. 0 if none is usable. */
uint32_t sgp_dnd_data_object(uint8_t *mem, char *const *files, int n, int32_t cx, int32_t cy) {
    uint16_t *w;
    uint32_t units = 0, cap = 1;
    int i, k, used = 0;
    dnd_obj *o = NULL;
    if (vtbl_init(mem)) return 0;
    for (k = 0; k < DND_MAX_OBJ; k++) if (!objs[k].used) { o = &objs[k]; break; }
    if (!o) { hle_log(HLE_LOG_WARN, "dragdrop: more than %d live data objects", DND_MAX_OBJ); return 0; }
    cap = (uint32_t)(n < DND_MAX_FILES ? n : DND_MAX_FILES) * DND_MAX_PATH + 1u;
    w = malloc(2u * cap);
    if (!w) return 0;
    for (i = 0; i < n && used < DND_MAX_FILES; i++) {
        char win[1024];
        if (!is_png(files[i])) { SGP_LOG("dragdrop: %s is not a .png, not offered", files[i]); continue; }
        if (sgp_dnd_windows_path(files[i], win, sizeof win)) {
            hle_log(HLE_LOG_WARN, "dragdrop: %s has no Windows path under MAX_PATH, not offered", files[i]);
            continue;
        }
        units += utf8_to_utf16(win, w + units, cap - units);
        w[units++] = 0;
        used++;
        SGP_LOG("dragdrop: offering %s as %s", files[i], win);
    }
    if (!used) { free(w); return 0; }
    w[units++] = 0;
    if (!o->guest) {
        o->guest = hle_guest_alloc(16, 16);
        if (!o->guest) { free(w); return 0; }
    }
    o->used = 1;
    o->refs = 1;
    o->nfiles = used;
    o->wfiles = w;
    o->wunits = units;
    o->cx = cx;
    o->cy = cy;
    sg_st32(mem, o->guest + 0, do_vtbl);
    sg_st32(mem, o->guest + 4, DND_MAGIC);
    sg_st32(mem, o->guest + 8, (uint32_t)k);
    sg_st32(mem, o->guest + 12, 0);
    return o->guest;
}

int sgp_dnd_live_objects(void) {
    int k, n = 0;
    for (k = 0; k < DND_MAX_OBJ; k++) n += objs[k].used;
    return n;
}

/* ---- calls into the registered IDropTarget ----------------------------------------------------------- */
static uint32_t target_call(sg_ctx *ctx, uint32_t target, unsigned slot, unsigned n, const uint32_t *args,
                            const char *what) {
    uint32_t a[8], fn = sg_ld32(ctx->mem, sg_ld32(ctx->mem, target) + 4u * slot);
    unsigned i;
    a[0] = target;
    for (i = 0; i < n; i++) a[i + 1] = args[i];
    return sgp_guest_call(ctx, fn, n + 1, a, what);
}
__attribute__((unused)) static void com_release(sg_ctx *ctx, uint32_t obj) {
    if (obj) target_call(ctx, obj, DT_RELEASE, 0, NULL, "IUnknown::Release (OLE drag and drop)");
}

static dnd_reg *reg_find(uint32_t hwnd) {
    int i;
    for (i = 0; i < DND_MAX_REG; i++)
        if (regs[i].target && (regs[i].hwnd == hwnd || !hwnd)) return &regs[i];
    return NULL;
}

static uint32_t enter(sg_ctx *ctx, const sp_dnd_event *e, uint32_t target, uint32_t x, uint32_t y) {
    uint8_t *mem = ctx->mem;
    uint32_t dobj = sgp_dnd_data_object(mem, e->files, e->nfiles, e->x, e->y), hr, eff;
    if (!dobj) return 0;
    sess.active = 1, sess.hwnd = e->hwnd, sess.target = target, sess.dobj = dobj;
    sg_st32(mem, scratch, DROPEFFECT_COPY_);
    {
        uint32_t a[5] = {dobj, e->keys, x, y, scratch};
        hr = target_call(ctx, target, DT_ENTER, 5, a, "IDropTarget::DragEnter");
    }
    eff = hr ? 0 : sg_ld32(mem, scratch);
    sess.eff = eff;
    SGP_LOG("dragdrop: DragEnter(%d files, %d,%d) = 0x%x, effect %u", objs[sg_ld32(mem, dobj + 8)].nfiles, (int)x, (int)y,
            hr, eff);
    return eff;
}
static void end_session(sg_ctx *ctx) {
    uint32_t d = sess.dobj;
    sess.active = 0, sess.dobj = 0;
    if (d) {
        /* our reference; the target AddRefs if it keeps the object (the game does not) */
        dnd_obj *o = obj_of(ctx->mem, d);
        if (o && o->refs && !--o->refs) obj_free(o);
    }
}

/* One host event -> IDropTarget calls. Returns the DROPEFFECT the target answered (0 = refused). */
uint32_t sgp_dnd_deliver(sg_ctx *ctx, const sp_dnd_event *e) {
    uint8_t *mem = ctx->mem;
    dnd_reg *r = reg_find(e->hwnd);
    int32_t sx = e->x, sy = e->y;
    uint32_t eff = 0, hr;
    stat_events++;
    if (!r) return 0;
    if (vtbl_init(mem)) return 0;
    sp_client_to_screen(r->hwnd, &sx, &sy); /* POINTL in screen coordinates; the game calls ScreenToClient */
    if (sess.active && sess.target != r->target) {
        target_call(ctx, sess.target, DT_LEAVE, 0, NULL, "IDropTarget::DragLeave");
        end_session(ctx);
    }
    switch (e->kind) {
    case SP_DND_ENTER:
        if (sess.active) {
            target_call(ctx, r->target, DT_LEAVE, 0, NULL, "IDropTarget::DragLeave");
            end_session(ctx);
        }
        eff = enter(ctx, e, r->target, (uint32_t)sx, (uint32_t)sy);
        break;
    case SP_DND_OVER:
        if (!sess.active) {
            if (!e->nfiles) return 0;
            if (!enter(ctx, e, r->target, (uint32_t)sx, (uint32_t)sy) && !sess.active) return 0;
        }
        sg_st32(mem, scratch, DROPEFFECT_COPY_);
        {
            uint32_t a[4] = {e->keys, (uint32_t)sx, (uint32_t)sy, scratch};
            hr = target_call(ctx, r->target, DT_OVER, 4, a, "IDropTarget::DragOver");
        }
        eff = hr ? 0 : sg_ld32(mem, scratch);
        sess.eff = eff;
        SGP_TRACE("dragdrop: DragOver(%d,%d) = 0x%x, effect %u", sx, sy, hr, eff);
        break;
    case SP_DND_LEAVE:
        if (!sess.active) return 0;
        target_call(ctx, r->target, DT_LEAVE, 0, NULL, "IDropTarget::DragLeave");
        SGP_LOG("dragdrop: DragLeave");
        end_session(ctx);
        break;
    case SP_DND_DROP:
        if (!sess.active) {
            if (!e->nfiles) return 0;
            enter(ctx, e, r->target, (uint32_t)sx, (uint32_t)sy);
            if (!sess.active) return 0;
        }
        if (!sess.eff) { /* as DoDragDrop: a target that answered DROPEFFECT_NONE gets DragLeave, not Drop */
            target_call(ctx, r->target, DT_LEAVE, 0, NULL, "IDropTarget::DragLeave");
            SGP_LOG("dragdrop: drop on a target that refused it: DragLeave");
            end_session(ctx);
            return 0;
        }
        sg_st32(mem, scratch, DROPEFFECT_COPY_);
        {
            uint32_t a[5] = {sess.dobj, e->keys, (uint32_t)sx, (uint32_t)sy, scratch};
            hr = target_call(ctx, r->target, DT_DROP, 5, a, "IDropTarget::Drop");
        }
        eff = hr ? 0 : sg_ld32(mem, scratch);
        stat_drops++;
        SGP_LOG("dragdrop: Drop(%d,%d) = 0x%x, effect %u", sx, sy, hr, eff);
        end_session(ctx);
        break;
    default:
        return 0;
    }
    return eff;
}

/* The host's notify: drains the queue on the main thread, from inside the event drain. */
__attribute__((unused)) static uint32_t host_notify(void *user) {
    sg_ctx *ctx = sgp_tl_ctx ? sgp_tl_ctx : (sg_self ? &sg_self->ctx : NULL);
    sp_dnd_event e;
    uint32_t eff = ~0u;
    (void)user;
    if (!ctx || !sp_is_owner_thread() || delivering) return ~0u; /* stays queued for the next callback */
    delivering = 1;
    while (sp_mac_dnd_poll(&e)) {
        eff = sgp_dnd_deliver(ctx, &e);
        sp_mac_dnd_event_free(&e);
    }
    delivering = 0;
    return eff;
}

/* Translated code calls sg_imp_ole32_* directly (SG_CALL at 0x6bbbad and the other sites), so ours must be those
 * symbols. ole_stub.c's are weak when hle.h defines HLE_OLE32_WEAK; without it these stay out (the link would have two definitions) and the game
 * keeps ole_stub.c's no-drop behaviour. The tests compile this file with -DHLE_OLE32_WEAK=1 into their own link. */
#ifdef HLE_OLE32_WEAK
/* ---- ole32 ---------------------------------------------------------------------------------------------- */
static void ole_initialize(sg_ctx *ctx, uint8_t *mem) {
    int n = __atomic_fetch_add(&ole_inits, 1, __ATOMIC_SEQ_CST);
    SGP_LOG("OleInitialize(0x%x) = %s", SG_ARG(0), n ? "S_FALSE" : "S_OK");
    SG_RETURN_STDCALL(n ? S_FALSE_ : S_OK_, 4);
}
static void ole_uninitialize(sg_ctx *ctx, uint8_t *mem) {
    __atomic_fetch_sub(&ole_inits, 1, __ATOMIC_SEQ_CST);
    SG_RETURN_STDCALL(0, 0);
}
/* The target lives as long as the process; the game AddRefs it itself. Kept a no-op as in ole_stub.c. */
static void co_lock_object_external(sg_ctx *ctx, uint8_t *mem) {
    SGP_LOG("CoLockObjectExternal(0x%x, %u, %u) = S_OK", SG_ARG(0), SG_ARG(1), SG_ARG(2));
    SG_RETURN_STDCALL(S_OK_, 12);
}
static void register_drag_drop(sg_ctx *ctx, uint8_t *mem) {
    uint32_t hwnd = SG_ARG(0), target = SG_ARG(1), r = S_OK_;
    int i, slot = -1, host = 0;
    if (!sgp_dnd_enabled()) { /* SPORE_DRAGDROP=0: as ole_stub.c, a PC on which no drop arrives */
        SGP_LOG("RegisterDragDrop(hwnd 0x%x, target 0x%x) = S_OK (SPORE_DRAGDROP=0: no drops)", hwnd, target);
        SG_RETURN_STDCALL(S_OK_, 8);
    }
    SGP_ENTER();
    if (!target) r = E_INVALIDARG_;
    else if (!hwnd || !sp_is_window(hwnd)) r = DRAGDROP_E_INVALIDHWND_;
    else {
        for (i = 0; i < DND_MAX_REG; i++) {
            if (regs[i].target && regs[i].hwnd == hwnd) { r = DRAGDROP_E_ALREADYREGISTERED_; break; }
            if (!regs[i].target && slot < 0) slot = i;
        }
        if (!r && slot < 0) r = E_OUTOFMEMORY_;
    }
    if (!r) {
        regs[slot].hwnd = hwnd;
        regs[slot].target = target;
        target_call(ctx, target, DT_ADDREF, 0, NULL, "IDropTarget::AddRef (RegisterDragDrop)");
        host = sp_mac_dnd_attach(hwnd, host_notify, NULL);
    }
    hle_log(HLE_LOG_INFO, "RegisterDragDrop(hwnd 0x%x, target 0x%x) = 0x%x (%s)", hwnd, target, r,
            r ? "refused" : host == 1 ? "file drops from the Finder are delivered" :
            host == 2 ? "registered when the window appears" : "no AppKit host: no drops");
    SGP_LEAVE();
    SG_RETURN_STDCALL(r, 8);
}
static void revoke_drag_drop(sg_ctx *ctx, uint8_t *mem) {
    uint32_t hwnd = SG_ARG(0);
    int i;
    SGP_ENTER();
    for (i = 0; i < DND_MAX_REG; i++)
        if (regs[i].target && regs[i].hwnd == hwnd) {
            uint32_t t = regs[i].target;
            if (sess.active && sess.target == t) end_session(ctx);
            regs[i].target = regs[i].hwnd = 0;
            sp_mac_dnd_detach(hwnd);
            com_release(ctx, t);
            SGP_LOG("RevokeDragDrop(hwnd 0x%x) = S_OK", hwnd);
            SGP_LEAVE();
            SG_RETURN_STDCALL(S_OK_, 4);
        }
    SGP_LEAVE();
    SG_RETURN_STDCALL(DRAGDROP_E_NOTREGISTERED_, 4);
}
/* STGMEDIUM {tymed, union, pUnkForRelease}: with pUnkForRelease, Release it; otherwise free what the union holds
 * (HGLOBAL: GlobalFree; IStream/IStorage: Release). TYMED_FILE would delete the file on Windows: logged, not done. */
static void release_stg_medium(sg_ctx *ctx, uint8_t *mem) {
    uint32_t m = SG_ARG(0), tymed, u, unk;
    SGP_ENTER();
    if (m) {
        tymed = sg_ld32(mem, m), u = sg_ld32(mem, m + 4), unk = sg_ld32(mem, m + 8);
        if (unk) com_release(ctx, unk);
        else if (tymed == TYMED_HGLOBAL_ && u) {
            uint32_t a[1] = {u};
            sgp_call_import(ctx, "GlobalFree", 1, a);
        } else if ((tymed == TYMED_ISTREAM_ || tymed == TYMED_ISTORAGE_) && u) com_release(ctx, u);
        else if (tymed == TYMED_FILE_) hle_log(HLE_LOG_WARN, "ReleaseStgMedium(TYMED_FILE): file left in place");
        sg_st32(mem, m, 0);
        sg_st32(mem, m + 4, 0);
        sg_st32(mem, m + 8, 0);
    }
    SGP_LEAVE();
    SG_RETURN_STDCALL(0, 4);
}

SG_IMPORT(ole32, OleInitialize) { ole_initialize(ctx, mem); }
SG_IMPORT(ole32, OleUninitialize) { ole_uninitialize(ctx, mem); }
SG_IMPORT(ole32, CoLockObjectExternal) { co_lock_object_external(ctx, mem); }
SG_IMPORT(ole32, RegisterDragDrop) { register_drag_drop(ctx, mem); }
SG_IMPORT(ole32, RevokeDragDrop) { revoke_drag_drop(ctx, mem); }
SG_IMPORT(ole32, ReleaseStgMedium) { release_stg_medium(ctx, mem); }
#endif

/* ---- shell32 ------------------------------------------------------------------------------------------- */
/* UINT DragQueryFileW(HDROP, UINT iFile, LPWSTR lpszFile, UINT cch): 0xFFFFFFFF -> the count; NULL buffer ->
 * the length of name iFile (no NUL); else copies at most cch-1 units and a NUL and returns the units copied. */
SG_IMPORT(shell32, DragQueryFileW) {
    uint32_t hdrop = SG_ARG(0), idx = SG_ARG(1), buf = SG_ARG(2), cch = SG_ARG(3), r = 0, p, a[1];
    SGP_ENTER();
    if (!hdrop) goto out;
    a[0] = hdrop;
    p = sgp_call_import(ctx, "GlobalLock", 1, a);
    if (!p) p = hdrop;
    {
        uint32_t off = sg_ld32(mem, p), wide = sg_ld32(mem, p + 16), q = p + off, unit = wide ? 2u : 1u, count = 0;
        for (;;) {
            uint32_t len = 0;
            while ((wide ? sg_ld16(mem, q + 2 * len) : mem[q + len]) && len < 32768u) len++;
            if (!len) break;
            if (idx != 0xffffffffu && count == idx) {
                if (!buf) r = len;
                else if (cch) {
                    uint32_t n = len < cch - 1 ? len : cch - 1, i;
                    for (i = 0; i < n; i++) sg_st16(mem, buf + 2 * i, wide ? sg_ld16(mem, q + 2 * i) : mem[q + i]);
                    sg_st16(mem, buf + 2 * n, 0);
                    r = n;
                }
                break;
            }
            count++;
            q += (len + 1) * unit;
        }
        if (idx == 0xffffffffu) r = count;
    }
    sgp_call_import(ctx, "GlobalUnlock", 1, a);
out:
    SGP_TRACE("DragQueryFileW(0x%x, %d, 0x%x, %u) = %u", hdrop, (int)idx, buf, cch, r);
    SGP_LEAVE();
    SG_RETURN_STDCALL(r, 16);
}

void sgp_dnd_stats(uint64_t *events, uint64_t *drops) {
    if (events) *events = stat_events;
    if (drops) *drops = stat_drops;
}
