/* mt_d3d9.mm -- the Metal back end behind the guest Direct3D 9 vtables (d3d9metal_int.h).
 *
 * Host object = our own object (mt_obj): there is no native D3D9 underneath, so identity is structural
 * (GetSurfaceLevel twice returns the same child; Get* returns what Set* stored). The IDirect3D9 adapter
 * queries are the null device's handlers (d3d9null_d3d9_handler), so the identity, caps, formats and modes
 * are the same profile as the null device (DEVIATIONS 3.1-3.4). CreateDevice makes a Metal device
 * on the system default MTLDevice.
 *
 * Containment as the null device: level surfaces/volumes count on their texture, the back buffer on its
 * swap chain, the implicit swap chain and the auto depth surface on the device. An object the game releases
 * while it is still bound to the device (texture stage, stream, indices, render target, depth, shaders,
 * declaration) stays alive, unreachable from the guest, until the binding changes: D3D9's runtime holds an
 * internal reference on bound objects, and draws keep using them.
 *
 * Every state setter stores into mt_state (the pipeline reads it at draw time) and sets the dirty bit. Draws are
 * stubs here (counted, D3D_OK) unless the pipeline provides the slot (mt_pipeline_handler). Clear of
 * the whole target is a Clear load action; anything narrower is the pipeline's. */
#import <Metal/Metal.h>
#import <QuartzCore/CAMetalLayer.h>
#include <dlfcn.h>
#include <mach/mach.h>
#include <pthread.h>
#include <unistd.h>

#include "mt_priv.h"

d3d9metal_stats g_mst;
int64_t g_live_kind[MT_K_NKIND];
int g_mt_trace;
int mt_verbose;
static int g_loaded;
static uint64_t g_next_id = 1;
static void (*p_stall_beat)(void);
static void (*p_stall_fault)(const char *);
static void (*p_stall_reporter)(void (*)(int));
static volatile int g_lost;
static char g_watch_line[1024];

extern "C" int d3d9metal_loaded(void) { return g_loaded; }
extern "C" void d3d9metal_get_stats(d3d9metal_stats *s) { *s = g_mst; }

/* ---- weak hooks (pipeline/ provides the strong ones) --------------------------------------------------- */
extern "C" __attribute__((weak)) sg_com_handler *mt_pipeline_handler(unsigned slot) { (void)slot; return NULL; }
extern "C" __attribute__((weak)) void mt_pl_device_created(mt_dev *d) { (void)d; }
extern "C" __attribute__((weak)) void mt_pl_device_reset(mt_dev *d) { (void)d; }
extern "C" __attribute__((weak)) void mt_pl_device_destroyed(mt_dev *d) { (void)d; }
extern "C" __attribute__((weak)) void mt_pl_object_freed(mt_obj *o) { (void)o; }
extern "C" __attribute__((weak)) void mt_pl_frame_end(mt_dev *d) { (void)d; }
extern "C" __attribute__((weak)) void mt_pl_shader_created(mt_obj *o) { (void)o; }

/* ---- objects ------------------------------------------------------------------------------------------- */
typedef struct mt_extra { /* device-layer private state (mt_dev.devpriv) */
    uint8_t gamma[1536];
    uint32_t material[17];
    uint32_t lights[16][26], light_set[16], light_on[16];
    uint32_t clip_status[6];
    uint32_t palette, swvp, cursor;
    float npatch;
    int recording;
} mt_extra;
#define X(d) ((mt_extra *)(d)->devpriv)

static void mt_destroy(sg_com_obj *o);
const sg_com_class mt_class = {"d3d9metal", mt_destroy, NULL};
static int is_res(int k) { return k >= MT_K_TEX && k <= MT_K_IB; }

extern "C" uint32_t mt_guest_of(mt_obj *o) { return o ? sg_com_obj_guest(&o->com, NULL) : 0; }
extern "C" mt_obj *mt_from_guest(const uint8_t *mem, uint32_t g, int kind) {
    sg_com_obj *o = g ? sg_com_lookup(mem, g) : NULL;
    if (!o || o->cls != &mt_class) return NULL;
    if (kind >= 0 && ((mt_obj *)o)->kind != kind) return NULL;
    return (mt_obj *)o;
}
extern "C" void mt_kid_add(mt_obj *p, mt_obj *k) {
    if (p->nkids == p->capkids) {
        p->capkids = p->capkids ? p->capkids * 2 : 8;
        p->kids = (mt_obj **)realloc(p->kids, p->capkids * sizeof *p->kids);
    }
    p->kids[p->nkids++] = k;
}
extern "C" mt_obj *mt_new(int kind, mt_dev *dev, mt_obj *owner) {
    mt_obj *o = (mt_obj *)calloc(1, kind == MT_K_DEV ? sizeof(mt_dev) : sizeof(mt_obj));
    if (!o) return NULL;
    if (sg_com_obj_init(&o->com, &mt_class)) {
        free(o);
        return NULL;
    }
    o->kind = kind;
    o->dev = dev;
    o->uid = g_next_id++;
    if (owner) {
        o->com.refs = 0;
        o->com.owner = &owner->com;
        o->parent = owner;
        mt_kid_add(owner, o);
    }
    if (!sg_com_obj_expose(&o->com, &mt_iface[kind])) {
        sg_com_obj_retire(&o->com);
        free(o);
        return NULL;
    }
    g_live_kind[kind]++;
    if (kind != MT_K_D3D && kind != MT_K_DEV) {
        nst()->creates++;
        nst()->live_resources++;
    }
    return o;
}
extern "C" uint32_t mt_put_new(sg_com_call *c, uint32_t out, mt_obj *o, uint32_t err) {
    if (out) sg_st32(MEM, out, mt_guest_of(o));
    return o ? D3D_OK : err;
}

/* Is o (or one of its children) bound to its device's state? */
static int bound_one(mt_dev *d, mt_obj *o) {
    mt_state *s = &d->st;
    for (int i = 0; i < MT_NSAMPLERS; i++) if (s->tex[i] == o) return 1;
    for (int i = 0; i < MT_NSTREAMS; i++) if (s->streams[i].vb == o) return 1;
    for (int i = 0; i < MT_NRT; i++) if (s->rt[i] == o) return 1;
    return s->ib == o || s->ds == o || s->vs == o || s->ps == o || s->vdecl == o;
}
static int is_bound(mt_obj *o) {
    mt_dev *d = o->dev;
    if (!d || !d->alive || o->kind == MT_K_DEV || o->kind == MT_K_D3D) return 0;
    if (bound_one(d, o)) return 1;
    for (unsigned i = 0; i < o->nkids; i++)
        if (bound_one(d, o->kids[i])) return 1;
    return 0;
}
#define ZOMBIE(o) ((o)->zombie)
extern "C" void mt_obj_free(mt_obj *o) {
    for (unsigned i = 0; i < o->nkids; i++) mt_obj_free(o->kids[i]);
    free(o->kids);
    o->kids = NULL;
    o->nkids = 0;
    if (!ZOMBIE(o)) mt_pl_object_freed(o);
    if (o->dev && o->dev->occ_active == o) o->dev->occ_active = NULL;
    if (o->dev && o->dev->alive && o->kind == MT_K_SURF) { /* the open pass may name it: end it, forget it */
        mt_dev *d = o->dev;
        int hit = d->pass_req_ds == o || d->pass_ds == o;
        for (int i = 0; i < MT_NRT; i++) hit |= d->pass_req_rt[i] == o || d->pass_rt[i] == o;
        if (hit) {
            mt_pass_end(d);
            memset(d->pass_req_rt, 0, sizeof d->pass_req_rt);
            memset(d->pass_rt, 0, sizeof d->pass_rt);
            d->pass_req_ds = d->pass_ds = NULL;
        }
    }
    mt_res_release_metal(o);
    if (o->shadow) {
        sg_com_gfree(o->shadow);
        nst()->resource_bytes -= o->bytes;
        g_mst.shadow_bytes -= o->bytes;
        o->shadow = 0;
    }
    free(o->locks);
    free(o->decl);
    free(o->code);
    mt_private_free_all(o);
    if (o->kind < MT_K_NKIND) g_live_kind[o->kind]--;
    if (o->kind != MT_K_D3D && o->kind != MT_K_DEV) nst()->live_resources--;
    if (o->com.handle) sg_com_obj_retire(&o->com);
    free(o);
}
/* A binding slot changes from `old`: a released object that was only alive because it was bound goes now. */
static void unbound(mt_obj *old) {
    if (!old) return;
    mt_obj *top = old;
    while (top->parent && top->parent->kind != MT_K_DEV && top->parent->kind != MT_K_SC && ZOMBIE(top->parent)) top = top->parent;
    if (ZOMBIE(top) && !is_bound(top)) mt_obj_free(top);
}
static void retire_records(mt_obj *o) {
    for (unsigned i = 0; i < o->nkids; i++) retire_records(o->kids[i]);
    if (o->com.handle) sg_com_obj_retire(&o->com);
}
static void dev_free(mt_dev *d);
static void mt_destroy(sg_com_obj *co) {
    mt_obj *o = (mt_obj *)co;
    if (o->kind == MT_K_DEV) {
        dev_free((mt_dev *)o);
        return;
    }
    if (o->kind == MT_K_D3D) {
        g_live_kind[MT_K_D3D]--;
        sg_com_obj_retire(co);
        free(o);
        return;
    }
    if (is_bound(o)) { /* D3D9 keeps bound objects alive: unreachable for the guest, freed when unbound */
        mt_pl_object_freed(o);
        o->zombie = 1;
        for (unsigned i = 0; i < o->nkids; i++) o->kids[i]->zombie = 1;
        retire_records(o);
        return;
    }
    mt_obj_free(o);
}

/* ---- IDirect3D9 ------------------------------------------------------------------------------------------ */
static uint32_t d_shared(sg_com_call *c) { /* the null device's adapter-profile handlers */
    sg_com_handler *h = d3d9null_d3d9_handler(c->m->slot);
    return h ? h(c) : D3DERR_INVALIDCALL;
}
/* D3DPRESENT_PARAMETERS defaults, as the null device: written back to the guest's struct. */
static uint32_t read_pp(sg_com_call *c, uint32_t p, uint32_t focus, uint32_t pp[D3DPP_DWORDS]) {
    for (int i = 0; i < D3DPP_DWORDS; i++) pp[i] = sg_ld32(MEM, p + 4u * i);
    if (!pp[PP_HWND]) pp[PP_HWND] = focus;
    if (pp[PP_WINDOWED]) {
        if (!pp[PP_W] || !pp[PP_H]) {
            uint32_t cw = 0, ch = 0;
            if (sg_host_client_size(pp[PP_HWND], &cw, &ch) || !cw || !ch) cw = 1024, ch = 768;
            if (!pp[PP_W]) pp[PP_W] = cw;
            if (!pp[PP_H]) pp[PP_H] = ch;
        }
        if (pp[PP_FMT] == D3DFMT_UNKNOWN) pp[PP_FMT] = D3DFMT_X8R8G8B8;
        if (pp[PP_REFRESH]) return D3DERR_INVALIDCALL;
    } else if (!pp[PP_W] || !pp[PP_H] || pp[PP_FMT] == D3DFMT_UNKNOWN) {
        return D3DERR_INVALIDCALL;
    }
    if (!pp[PP_COUNT]) pp[PP_COUNT] = 1;
    if (pp[PP_COUNT] > 3 || pp[PP_SWAP] < 1 || pp[PP_SWAP] > 5) return D3DERR_INVALIDCALL;
    if (pp[PP_FMT] != D3DFMT_A8R8G8B8 && pp[PP_FMT] != D3DFMT_X8R8G8B8 && !mt_format_of(pp[PP_FMT])) return D3DERR_NOTAVAILABLE;
    for (int i = 0; i < D3DPP_DWORDS; i++) sg_st32(MEM, p + 4u * i, pp[i]);
    return D3D_OK;
}
static void set_fpu_single(sg_ctx *ctx, uint32_t flags) { /* DEVIATIONS 1.5: D3D9 without FPU_PRESERVE sets 24-bit */
    if (!(flags & D3DCREATE_FPU_PRESERVE)) ctx->fpu.cw = (uint16_t)((ctx->fpu.cw & ~0x0f3fu) | 0x003fu);
}
/* A swap chain with its back buffer (a render-target surface, child of the swap chain). */
static mt_obj *swapchain_new(mt_dev *d, const uint32_t pp[D3DPP_DWORDS], mt_obj *owner, uint32_t *err) {
    mt_obj *s = mt_new(MT_K_SC, d, owner), *bb = NULL;
    if (!s) {
        *err = SG_E_OUTOFMEMORY;
        return NULL;
    }
    memcpy(s->pp, pp, sizeof s->pp);
    s->sc_hwnd = pp[PP_HWND];
    *err = mt_texture_new(d, MT_K_SURF, pp[PP_W], pp[PP_H], 1, 1, D3DUSAGE_RENDERTARGET, pp[PP_FMT], D3DPOOL_DEFAULT, pp[PP_MS],
                          pp[PP_MSQ], &bb);
    if (!bb) {
        if (owner) { /* owner's kid list: drop it again */
            owner->nkids--;
            s->com.owner = NULL;
        }
        mt_obj_free(s);
        return NULL;
    }
    /* re-parent the back buffer under the swap chain: its count delegates to it */
    bb->com.refs = 0;
    bb->com.owner = &s->com;
    bb->parent = s;
    bb->lockable = (pp[PP_FLAGS] & 1) != 0; /* D3DPRESENTFLAG_LOCKABLE_BACKBUFFER */
    mt_kid_add(s, bb);
    s->backbuf = bb;
    return s;
}
static uint32_t make_default_targets(mt_dev *d) {
    uint32_t err = D3D_OK;
    d->sc = swapchain_new(d, d->pp, &d->o, &err);
    if (!d->sc) return err;
    if (d->pp[PP_AUTODS]) {
        mt_obj *ds = NULL;
        err = mt_texture_new(d, MT_K_SURF, d->pp[PP_W], d->pp[PP_H], 1, 1, D3DUSAGE_DEPTHSTENCIL, d->pp[PP_DSFMT], D3DPOOL_DEFAULT,
                             d->pp[PP_MS], d->pp[PP_MSQ], &ds);
        if (!ds) return err;
        ds->com.refs = 0;
        ds->com.owner = &d->o.com;
        ds->parent = &d->o;
        mt_kid_add(&d->o, ds);
        d->autods = ds;
    }
    return D3D_OK;
}
static void free_default_targets(mt_dev *d) {
    unsigned j = 0;
    for (unsigned i = 0; i < d->o.nkids; i++) {
        mt_obj *k = d->o.kids[i];
        if (k == d->sc || k == d->autods) mt_obj_free(k);
        else d->o.kids[j++] = k;
    }
    d->o.nkids = j;
    d->sc = d->autods = NULL;
}
static void viewport_full(mt_dev *d, uint32_t w, uint32_t h) {
    d->st.vp.x = d->st.vp.y = 0;
    d->st.vp.w = w, d->st.vp.h = h;
    d->st.vp.minz = 0.0f, d->st.vp.maxz = 1.0f;
    d->st.scissor[0] = d->st.scissor[1] = 0;
    d->st.scissor[2] = w, d->st.scissor[3] = h;
    d->st.dirty |= MT_DIRTY_VIEWPORT | MT_DIRTY_SCISSOR;
}
/* D3D9's state after CreateDevice/Reset (the null device's table, d3d9_device.c dev_reset_state). */
static void reset_state(mt_dev *d) {
    static const struct { uint16_t s; uint32_t v; } rs_defaults[] = {
        {7, 1}, {8, 3}, {9, 2}, {14, 1}, {16, 1}, {19, 2}, {20, 1}, {22, 3}, {23, 4}, {25, 8}, {37, 0x3f800000},
        {38, 0x3f800000}, {53, 1}, {54, 1}, {55, 1}, {56, 8}, {58, 0xffffffff}, {59, 0xffffffff}, {60, 0xffffffff},
        {136, 1}, {137, 1}, {141, 1}, {142, 1}, {145, 1}, {146, 2}, {154, 0x3f800000}, {155, 0x3f800000},
        {158, 0x3f800000}, {161, 1}, {162, 0xffffffff}, {165, 0xbaadcafe}, {166, 0x46000000}, {168, 0xf}, {171, 1},
        {172, 3}, {173, 1}, {178, 0x3f800000}, {179, 0x3f800000}, {182, 0x3f800000}, {186, 1}, {187, 1}, {188, 1},
        {189, 8}, {190, 0xf}, {191, 0xf}, {192, 0xf}, {193, 0xffffffff}, {207, 2}, {208, 1}, {209, 1}};
    mt_state *s = &d->st;
    mt_obj *old[MT_NSAMPLERS + MT_NSTREAMS + MT_NRT + 6];
    unsigned n = 0;
    for (int i = 0; i < MT_NSAMPLERS; i++) old[n++] = s->tex[i];
    for (int i = 0; i < MT_NSTREAMS; i++) old[n++] = s->streams[i].vb;
    for (int i = 0; i < MT_NRT; i++) old[n++] = s->rt[i];
    old[n++] = s->ib, old[n++] = s->ds, old[n++] = s->vs, old[n++] = s->ps, old[n++] = s->vdecl;
    memset(s, 0, sizeof *s);
    for (unsigned i = 0; i < sizeof rs_defaults / sizeof rs_defaults[0]; i++) s->rs[rs_defaults[i].s] = rs_defaults[i].v;
    s->rs[7] = d->pp[PP_AUTODS] ? 1 : 0;
    for (int t = 0; t < MT_NSTAGES; t++) {
        s->tss[t][1] = t ? 1 : 4; /* COLOROP: MODULATE on stage 0, DISABLE after */
        s->tss[t][2] = 2;
        s->tss[t][4] = t ? 1 : 2; /* ALPHAOP */
        s->tss[t][5] = 2;
        s->tss[t][11] = (uint32_t)t; /* TEXCOORDINDEX */
    }
    for (int i = 0; i < MT_NSAMPLERS; i++) {
        s->ss[i][1] = s->ss[i][2] = s->ss[i][3] = 1; /* WRAP */
        s->ss[i][5] = s->ss[i][6] = 1;              /* POINT */
        s->ss[i][10] = 1;                           /* MAXANISOTROPY */
    }
    for (int i = 0; i < MT_NSTREAMS; i++) s->streams[i].freq = 1;
    for (int t = 0; t < 512; t++)
        for (int i = 0; i < 4; i++) s->transforms[t][i * 5] = 1.0f;
    s->rt[0] = d->sc ? d->sc->backbuf : NULL;
    s->ds = d->autods;
    viewport_full(d, d->pp[PP_W], d->pp[PP_H]);
    s->dirty = MT_DIRTY_ALL;
    d->in_scene = 0;
    if (X(d)) {
        X(d)->recording = 0;
        for (int i = 0; i < 256; i++) {
            uint8_t *g = X(d)->gamma;
            g[2 * i] = g[512 + 2 * i] = g[1024 + 2 * i] = (uint8_t)i;
            g[2 * i + 1] = g[513 + 2 * i] = g[1025 + 2 * i] = (uint8_t)i;
        }
    }
    for (unsigned i = 0; i < n; i++) unbound(old[i]);
}
static uint32_t d_create_device(sg_com_call *c) { /* (adapter, devtype, focus, flags, pp, ppDevice) */
    uint32_t adapter = ARG(0), devtype = ARG(1), focus = ARG(2), flags = ARG(3), ppa = ARG(4), out = ARG(5), pp[D3DPP_DWORDS], err;
    if (!out || !ppa) return D3DERR_INVALIDCALL;
    sg_st32(MEM, out, 0);
    if (adapter != 0) return D3DERR_INVALIDCALL;
    if (devtype != D3DDEVTYPE_HAL) return D3DERR_NOTAVAILABLE;
    {
        uint32_t vp = flags & 0xe0u; /* exactly one of SOFTWARE/HARDWARE/MIXED; the game asks for MIXED 0x80 */
        if (vp != 0x20u && vp != 0x40u && vp != 0x80u) return D3DERR_INVALIDCALL;
    }
    if (!focus && !sg_ld32(MEM, ppa + 4u * PP_HWND)) return D3DERR_INVALIDCALL;
    err = read_pp(c, ppa, focus, pp);
    if (err) return err;
    mt_dev *d = (mt_dev *)mt_new(MT_K_DEV, NULL, NULL);
    if (!d) return SG_E_OUTOFMEMORY;
    d->o.dev = d;
    d->devpriv = calloc(1, sizeof(mt_extra));
    d->mtl = MTLCreateSystemDefaultDevice();
    d->queue = d->mtl ? [d->mtl newCommandQueueWithMaxCommandBufferCount:64] : nil;
    if (!d->mtl || !d->queue) {
        MT_LOG("no Metal device or command queue\n");
        free(d->devpriv);
        sg_com_obj_retire(&d->o.com);
        free(d);
        g_live_kind[MT_K_DEV]--;
        return D3DERR_NOTAVAILABLE;
    }
    d->alive = 1;
    d->d3d = O;
    d->adapter = adapter, d->devtype = devtype, d->focus = focus, d->flags = flags;
    memcpy(d->pp, pp, sizeof pp);
    mt_device_metal_init(d);
    err = make_default_targets(d);
    if (err) {
        MT_LOG("CreateDevice: implicit targets failed: 0x%08x\n", err);
        sg_com_obj_release(&d->o.com);
        return err;
    }
    sg_com_obj_addref(&O->com);
    reset_state(d);
    set_fpu_single(c->ctx, flags);
    mt_pl_device_created(d);
    sg_st32(MEM, out, mt_guest_of(&d->o));
    MT_LOG("CreateDevice(flags 0x%x, %ux%u fmt %u, %s, depth %u, interval 0x%x, hwnd 0x%x) = 0x%08x on %s\n", flags, pp[PP_W],
           pp[PP_H], pp[PP_FMT], pp[PP_WINDOWED] ? "windowed" : "fullscreen requested, windowed", pp[PP_AUTODS] ? pp[PP_DSFMT] : 0,
           pp[PP_INTERVAL], pp[PP_HWND], mt_guest_of(&d->o), d->mtl.name.UTF8String);
    return D3D_OK;
}
static void dev_free(mt_dev *d) {
    mt_obj *d3d = d->d3d;
    mt_pl_device_destroyed(d);
    d->sc = d->sc; /* the implicit objects are kids: freed below */
    for (int i = 0; i < MT_NSAMPLERS; i++) d->st.tex[i] = NULL;
    /* bound zombies go with the state */
    mt_obj *old[MT_NSAMPLERS + MT_NSTREAMS + MT_NRT + 6];
    unsigned n = 0;
    for (int i = 0; i < MT_NSTREAMS; i++) old[n++] = d->st.streams[i].vb;
    for (int i = 0; i < MT_NRT; i++) old[n++] = d->st.rt[i];
    old[n++] = d->st.ib, old[n++] = d->st.ds, old[n++] = d->st.vs, old[n++] = d->st.ps, old[n++] = d->st.vdecl;
    memset(&d->st, 0, sizeof d->st);
    for (unsigned i = 0; i < n; i++) unbound(old[i]);
    for (unsigned i = 0; i < d->o.nkids; i++) mt_obj_free(d->o.kids[i]);
    free(d->o.kids);
    d->o.kids = NULL;
    d->o.nkids = 0;
    d->sc = d->autods = NULL;
    mt_device_metal_free(d);
    d->alive = 0;
    g_live_kind[MT_K_DEV]--;
    sg_com_obj_retire(&d->o.com);
    if (d3d) sg_com_obj_release(&d3d->com);
    /* the struct stays allocated: resources the game still holds keep a pointer to it (as the null device) */
}

/* ---- device ---------------------------------------------------------------------------------------------- */
#define D ((mt_dev *)c->obj)
#define ST (D->st)
static uint32_t ok0(sg_com_call *c) { return D3D_OK; }
static uint32_t dv_test_coop(sg_com_call *c) { return g_lost ? D3DERR_DEVICELOST : D3D_OK; }
static uint32_t dv_texmem(sg_com_call *c) { return 768u << 20; } /* the 8800 GTX profile, as the null device */
static uint32_t dv_evict(sg_com_call *c) { return D3D_OK; }
static uint32_t dv_get_d3d(sg_com_call *c) {
    if (!ARG(0)) return D3DERR_INVALIDCALL;
    sg_com_obj_addref(&D->d3d->com);
    sg_st32(MEM, ARG(0), mt_guest_of(D->d3d));
    return D3D_OK;
}
static uint32_t dv_caps(sg_com_call *c) {
    if (!ARG(0)) return D3DERR_INVALIDCALL;
    d3d9_fill_caps(MEM + ARG(0), D->devtype);
    return D3D_OK;
}
static void put_mode(sg_com_call *c, uint32_t p, const uint32_t *pp) {
    uint32_t w, h, hz;
    d3d9_current_mode(&w, &h, &hz);
    if (pp && !pp[PP_WINDOWED]) w = pp[PP_W], h = pp[PP_H], hz = pp[PP_REFRESH] ? pp[PP_REFRESH] : hz;
    sg_st32(MEM, p, w);
    sg_st32(MEM, p + 4, h);
    sg_st32(MEM, p + 8, hz);
    sg_st32(MEM, p + 12, D3DFMT_X8R8G8B8);
}
static uint32_t dv_display_mode(sg_com_call *c) {
    if (ARG(0) != 0 || !ARG(1)) return D3DERR_INVALIDCALL;
    put_mode(c, ARG(1), D->pp);
    return D3D_OK;
}
static uint32_t dv_creation(sg_com_call *c) {
    if (!ARG(0)) return D3DERR_INVALIDCALL;
    sg_st32(MEM, ARG(0), D->adapter);
    sg_st32(MEM, ARG(0) + 4, D->devtype);
    sg_st32(MEM, ARG(0) + 8, D->focus);
    sg_st32(MEM, ARG(0) + 12, D->flags);
    return D3D_OK;
}
static uint32_t dv_cursor_pos(sg_com_call *c) { return 0; }
static uint32_t dv_show_cursor(sg_com_call *c) { uint32_t o = X(D)->cursor; X(D)->cursor = ARG(0) != 0; return o; }
static uint32_t dv_add_swapchain(sg_com_call *c) { /* (pp, IDirect3DSwapChain9**) */
    uint32_t pp[D3DPP_DWORDS], err;
    if (!ARG(0) || !ARG(1)) return D3DERR_INVALIDCALL;
    sg_st32(MEM, ARG(1), 0);
    err = read_pp(c, ARG(0), D->focus, pp);
    if (err) return err;
    mt_obj *s = swapchain_new(D, pp, NULL, &err);
    if (!s) return err;
    sg_st32(MEM, ARG(1), mt_guest_of(s));
    return D3D_OK;
}
static uint32_t dv_get_swapchain(sg_com_call *c) {
    if (!ARG(1)) return D3DERR_INVALIDCALL;
    if (ARG(0) != 0 || !D->sc) {
        sg_st32(MEM, ARG(1), 0);
        return D3DERR_INVALIDCALL;
    }
    sg_com_obj_addref(&D->sc->com);
    sg_st32(MEM, ARG(1), mt_guest_of(D->sc));
    return D3D_OK;
}
static uint32_t dv_num_swapchains(sg_com_call *c) { return 1; }
static uint32_t dv_reset(sg_com_call *c) { /* (pp) */
    uint32_t pp[D3DPP_DWORDS], err;
    if (!ARG(0)) return D3DERR_INVALIDCALL;
    err = read_pp(c, ARG(0), D->focus, pp);
    if (err) return err;
    mt_flush(D, 1);
    mt_obj *old[MT_NRT + 1];
    for (int i = 0; i < MT_NRT; i++) old[i] = D->st.rt[i], D->st.rt[i] = NULL; /* the implicit targets go now */
    old[MT_NRT] = D->st.ds;
    D->st.ds = NULL;
    D->pass_req_ds = NULL;
    memset(D->pass_req_rt, 0, sizeof D->pass_req_rt);
    for (int i = 0; i <= MT_NRT; i++) unbound(old[i]);
    free_default_targets(D);
    memcpy(D->pp, pp, sizeof pp);
    err = make_default_targets(D);
    if (err) {
        MT_LOG("Reset(%ux%u): implicit targets failed: 0x%08x\n", pp[PP_W], pp[PP_H], err);
        return err;
    }
    reset_state(D);
    set_fpu_single(c->ctx, D->flags);
    mt_pl_device_reset(D);
    MT_LOG("Reset(%ux%u fmt %u, %s, depth %u, interval 0x%x)\n", pp[PP_W], pp[PP_H], pp[PP_FMT], pp[PP_WINDOWED] ? "windowed" : "fullscreen requested",
           pp[PP_AUTODS] ? pp[PP_DSFMT] : 0, pp[PP_INTERVAL]);
    return D3D_OK;
}
static uint32_t dv_present(sg_com_call *c) { /* (src, dst, hwnd, dirty) */
    uint32_t hwnd = ARG(2) ? ARG(2) : D->pp[PP_HWND] ? D->pp[PP_HWND] : D->focus;
    return mt_present(c, D, D->sc, hwnd);
}
static uint32_t dv_back_buffer(sg_com_call *c) { /* (swapchain, index, type, IDirect3DSurface9**) */
    if (!ARG(3)) return D3DERR_INVALIDCALL;
    if (ARG(0) != 0 || ARG(1) >= (D->pp[PP_COUNT] ? D->pp[PP_COUNT] : 1) || !D->sc || !D->sc->backbuf) {
        sg_st32(MEM, ARG(3), 0);
        return D3DERR_INVALIDCALL;
    }
    sg_com_obj_addref(&D->sc->backbuf->com);
    sg_st32(MEM, ARG(3), mt_guest_of(D->sc->backbuf));
    return D3D_OK;
}
static uint32_t dv_raster(sg_com_call *c) {
    if (!ARG(1)) return D3DERR_INVALIDCALL;
    sg_st32(MEM, ARG(1), 0);
    sg_st32(MEM, ARG(1) + 4, 0);
    return D3D_OK;
}
static uint32_t dv_set_gamma(sg_com_call *c) { /* stored only: the game's gamma ramp is not applied (DEVIATIONS 3.18) */
    if (ARG(2)) memcpy(X(D)->gamma, MEM + ARG(2), sizeof X(D)->gamma);
    return 0;
}
static uint32_t dv_get_gamma(sg_com_call *c) {
    if (ARG(1)) memcpy(MEM + ARG(1), X(D)->gamma, sizeof X(D)->gamma);
    return 0;
}
static uint32_t dv_create_texture(sg_com_call *c) { /* (w, h, levels, usage, fmt, pool, ppTex, pShared) */
    mt_obj *t = NULL;
    if (!ARG(6) || ARG(7)) return D3DERR_INVALIDCALL;
    uint32_t err = mt_texture_new(D, MT_K_TEX, ARG(0), ARG(1), 1, ARG(2), ARG(3), ARG(4), ARG(5), 0, 0, &t);
    return mt_put_new(c, ARG(6), t, err);
}
static uint32_t dv_create_volume(sg_com_call *c) { /* (w, h, d, levels, usage, fmt, pool, pp, shared) */
    mt_obj *t = NULL;
    if (!ARG(7) || ARG(8)) return D3DERR_INVALIDCALL;
    uint32_t err = mt_texture_new(D, MT_K_VOLTEX, ARG(0), ARG(1), ARG(2), ARG(3), ARG(4), ARG(5), ARG(6), 0, 0, &t);
    return mt_put_new(c, ARG(7), t, err);
}
static uint32_t dv_create_cube(sg_com_call *c) { /* (edge, levels, usage, fmt, pool, pp, shared) */
    mt_obj *t = NULL;
    if (!ARG(5) || ARG(6)) return D3DERR_INVALIDCALL;
    uint32_t err = mt_texture_new(D, MT_K_CUBE, ARG(0), ARG(0), 1, ARG(1), ARG(2), ARG(3), ARG(4), 0, 0, &t);
    return mt_put_new(c, ARG(5), t, err);
}
static uint32_t dv_create_vb(sg_com_call *c) { /* (len, usage, fvf, pool, pp, shared) */
    mt_obj *b = NULL;
    if (!ARG(4) || ARG(5)) return D3DERR_INVALIDCALL;
    uint32_t err = mt_buffer_new(D, MT_K_VB, ARG(0), ARG(1), ARG(2), ARG(3), &b);
    return mt_put_new(c, ARG(4), b, err);
}
static uint32_t dv_create_ib(sg_com_call *c) { /* (len, usage, fmt, pool, pp, shared) */
    mt_obj *b = NULL;
    if (!ARG(4) || ARG(5)) return D3DERR_INVALIDCALL;
    uint32_t err = mt_buffer_new(D, MT_K_IB, ARG(0), ARG(1), ARG(2), ARG(3), &b);
    return mt_put_new(c, ARG(4), b, err);
}
static uint32_t dv_create_rt(sg_com_call *c) { /* (w, h, fmt, ms, msq, lockable, pp, shared) */
    mt_obj *s = NULL;
    if (!ARG(6) || ARG(7)) return D3DERR_INVALIDCALL;
    if (d3d9_check_format(D3DFMT_X8R8G8B8, D3DUSAGE_RENDERTARGET, D3DRTYPE_SURFACE, ARG(2))) return mt_put_new(c, ARG(6), NULL, D3DERR_NOTAVAILABLE);
    uint32_t err = mt_texture_new(D, MT_K_SURF, ARG(0), ARG(1), 1, 1, D3DUSAGE_RENDERTARGET, ARG(2), D3DPOOL_DEFAULT, ARG(3), ARG(4), &s);
    if (s) s->lockable = ARG(5) != 0;
    return mt_put_new(c, ARG(6), s, err);
}
static uint32_t dv_create_ds(sg_com_call *c) { /* (w, h, fmt, ms, msq, discard, pp, shared) */
    mt_obj *s = NULL;
    if (!ARG(6) || ARG(7)) return D3DERR_INVALIDCALL;
    if (d3d9_check_format(D3DFMT_X8R8G8B8, D3DUSAGE_DEPTHSTENCIL, D3DRTYPE_SURFACE, ARG(2))) return mt_put_new(c, ARG(6), NULL, D3DERR_NOTAVAILABLE);
    uint32_t err = mt_texture_new(D, MT_K_SURF, ARG(0), ARG(1), 1, 1, D3DUSAGE_DEPTHSTENCIL, ARG(2), D3DPOOL_DEFAULT, ARG(3), ARG(4), &s);
    return mt_put_new(c, ARG(6), s, err);
}
static uint32_t dv_create_offscreen(sg_com_call *c) { /* (w, h, fmt, pool, pp, shared) */
    mt_obj *s = NULL;
    if (!ARG(4) || ARG(5) || ARG(3) == D3DPOOL_MANAGED) return D3DERR_INVALIDCALL;
    uint32_t err = mt_texture_new(D, MT_K_SURF, ARG(0), ARG(1), 1, 1, 0, ARG(2), ARG(3), 0, 0, &s);
    return mt_put_new(c, ARG(4), s, err);
}
static uint32_t dv_set_rt(sg_com_call *c) { /* (index, surface) */
    mt_obj *s = NULL;
    if (ARG(0) >= MT_NRT || (ARG(0) == 0 && !ARG(1))) return D3DERR_INVALIDCALL;
    if (ARG(1)) {
        s = mt_from_guest(MEM, ARG(1), MT_K_SURF);
        if (!s) return D3DERR_INVALIDCALL;
        if (!(s->usage & D3DUSAGE_RENDERTARGET)) return D3DERR_INVALIDCALL;
    }
    mt_obj *old = ST.rt[ARG(0)];
    ST.rt[ARG(0)] = s;
    ST.dirty |= MT_DIRTY_TARGETS;
    if (ARG(0) == 0) viewport_full(D, s->w, s->h);
    if (old != s) unbound(old);
    return D3D_OK;
}
static uint32_t out_obj(sg_com_call *c, uint32_t out, mt_obj *o) {
    if (o) sg_com_obj_addref(&o->com);
    sg_st32(MEM, out, mt_guest_of(o));
    return D3D_OK;
}
static uint32_t dv_get_rt(sg_com_call *c) { /* (index, IDirect3DSurface9**) */
    if (ARG(0) >= MT_NRT || !ARG(1)) return D3DERR_INVALIDCALL;
    out_obj(c, ARG(1), ST.rt[ARG(0)]);
    return ST.rt[ARG(0)] ? D3D_OK : D3DERR_NOTFOUND;
}
static uint32_t dv_set_ds(sg_com_call *c) {
    mt_obj *s = NULL, *old = ST.ds;
    if (ARG(0) && !(s = mt_from_guest(MEM, ARG(0), MT_K_SURF))) return D3DERR_INVALIDCALL;
    ST.ds = s;
    ST.dirty |= MT_DIRTY_TARGETS;
    if (old != s) unbound(old);
    return D3D_OK;
}
static uint32_t dv_get_ds(sg_com_call *c) {
    if (!ARG(0)) return D3DERR_INVALIDCALL;
    out_obj(c, ARG(0), ST.ds);
    return ST.ds ? D3D_OK : D3DERR_NOTFOUND;
}
static uint32_t dv_begin(sg_com_call *c) {
    if (D->in_scene) return D3DERR_INVALIDCALL;
    D->in_scene = 1;
    return D3D_OK;
}
static uint32_t dv_end(sg_com_call *c) {
    if (!D->in_scene) return D3DERR_INVALIDCALL;
    D->in_scene = 0;
    return D3D_OK;
}
/* Clear(count, rects, flags, color, z, stencil): the default handles a clear of the whole target
 * (no rects, viewport and scissor covering it) with Clear load actions; anything narrower is the pipeline's and
 * is logged once here. */
static uint32_t dv_clear(sg_com_call *c) {
    uint32_t n = ARG(0), rects = ARG(1), flags = ARG(2), col = ARG(3), st = ARG(5);
    float z = SG_COM_ARGF(c, 4);
    if (n && !rects) return D3DERR_INVALIDCALL;
    if (!ST.ds) flags &= ~6u;
    if (!flags) return D3D_OK;
    mt_obj *rt0 = ST.rt[0];
    int full = !n && rt0 && ST.vp.x == 0 && ST.vp.y == 0 && ST.vp.w >= rt0->w && ST.vp.h >= rt0->h &&
               (!ST.rs[174] /* SCISSORTESTENABLE */ || (ST.scissor[0] == 0 && ST.scissor[1] == 0 && ST.scissor[2] >= rt0->w && ST.scissor[3] >= rt0->h));
    if (!full) {
        static int once;
        if (!once++) MT_LOG("Clear of part of the target (rects %u, viewport %ux%u+%u+%u): not done without the pipeline package\n", n,
                            ST.vp.w, ST.vp.h, ST.vp.x, ST.vp.y);
        return D3D_OK;
    }
    float rgba[4] = {((col >> 16) & 255) / 255.0f, ((col >> 8) & 255) / 255.0f, (col & 255) / 255.0f, (col >> 24) / 255.0f};
    if (z < 0) z = 0;
    if (z > 1) z = 1;
    mt_pass_clear(D, flags, rgba, z, st & 0xff);
    return D3D_OK;
}
static float *transform_slot(mt_dev *d, uint32_t s) { return s < 512 ? d->st.transforms[s] : NULL; }
static uint32_t dv_set_transform(sg_com_call *c) {
    float *t = transform_slot(D, ARG(0));
    if (!ARG(1)) return D3DERR_INVALIDCALL;
    if (t) memcpy(t, MEM + ARG(1), 64);
    ST.dirty |= MT_DIRTY_TRANSFORM;
    return D3D_OK;
}
static uint32_t dv_get_transform(sg_com_call *c) {
    float *t = transform_slot(D, ARG(0));
    if (!ARG(1) || !t) return D3DERR_INVALIDCALL;
    memcpy(MEM + ARG(1), t, 64);
    return D3D_OK;
}
static uint32_t dv_mul_transform(sg_com_call *c) {
    float *t = transform_slot(D, ARG(0)), a[16], b[16], r[16];
    if (!ARG(1) || !t) return D3DERR_INVALIDCALL;
    memcpy(a, t, 64);
    memcpy(b, MEM + ARG(1), 64);
    for (int i = 0; i < 4; i++)
        for (int j = 0; j < 4; j++) r[i * 4 + j] = a[i * 4] * b[j] + a[i * 4 + 1] * b[4 + j] + a[i * 4 + 2] * b[8 + j] + a[i * 4 + 3] * b[12 + j];
    memcpy(t, r, 64);
    ST.dirty |= MT_DIRTY_TRANSFORM;
    return D3D_OK;
}
static uint32_t dv_set_vp(sg_com_call *c) { /* D3DVIEWPORT9 {X, Y, Width, Height, MinZ, MaxZ} */
    if (!ARG(0)) return D3DERR_INVALIDCALL;
    memcpy(&ST.vp, MEM + ARG(0), 24);
    ST.dirty |= MT_DIRTY_VIEWPORT;
    return D3D_OK;
}
static uint32_t dv_get_vp(sg_com_call *c) {
    if (!ARG(0)) return D3DERR_INVALIDCALL;
    memcpy(MEM + ARG(0), &ST.vp, 24);
    return D3D_OK;
}
#define XSTORE(field, n) do { if (!ARG(0)) return D3DERR_INVALIDCALL; memcpy(X(D)->field, MEM + ARG(0), n); return D3D_OK; } while (0)
#define XLOAD(field, n) do { if (!ARG(0)) return D3DERR_INVALIDCALL; memcpy(MEM + ARG(0), X(D)->field, n); return D3D_OK; } while (0)
static uint32_t dv_set_material(sg_com_call *c) { XSTORE(material, 68); }
static uint32_t dv_get_material(sg_com_call *c) { XLOAD(material, 68); }
static uint32_t dv_set_light(sg_com_call *c) {
    if (!ARG(1) || ARG(0) >= 16) return ARG(1) ? D3D_OK : D3DERR_INVALIDCALL;
    memcpy(X(D)->lights[ARG(0)], MEM + ARG(1), 104);
    X(D)->light_set[ARG(0)] = 1;
    return D3D_OK;
}
static uint32_t dv_get_light(sg_com_call *c) {
    if (!ARG(1) || ARG(0) >= 16 || !X(D)->light_set[ARG(0)]) return D3DERR_INVALIDCALL;
    memcpy(MEM + ARG(1), X(D)->lights[ARG(0)], 104);
    return D3D_OK;
}
static uint32_t dv_light_enable(sg_com_call *c) {
    if (ARG(0) < 16) X(D)->light_on[ARG(0)] = ARG(1) != 0, X(D)->light_set[ARG(0)] = 1;
    return D3D_OK;
}
static uint32_t dv_get_light_enable(sg_com_call *c) {
    if (!ARG(1) || ARG(0) >= 16 || !X(D)->light_set[ARG(0)]) return D3DERR_INVALIDCALL;
    sg_st32(MEM, ARG(1), X(D)->light_on[ARG(0)]);
    return D3D_OK;
}
static uint32_t dv_set_clip(sg_com_call *c) {
    if (ARG(0) >= 6 || !ARG(1)) return D3DERR_INVALIDCALL;
    memcpy(ST.clip[ARG(0)], MEM + ARG(1), 16);
    ST.dirty |= MT_DIRTY_RS;
    return D3D_OK;
}
static uint32_t dv_get_clip(sg_com_call *c) {
    if (ARG(0) >= 6 || !ARG(1)) return D3DERR_INVALIDCALL;
    memcpy(MEM + ARG(1), ST.clip[ARG(0)], 16);
    return D3D_OK;
}
static uint32_t dv_set_rs(sg_com_call *c) {
    if (ARG(0) < MT_NRS && ST.rs[ARG(0)] != ARG(1)) {
        ST.rs[ARG(0)] = ARG(1);
        ST.dirty |= MT_DIRTY_RS;
    }
    return D3D_OK;
}
static uint32_t dv_get_rs(sg_com_call *c) {
    if (!ARG(1) || ARG(0) >= MT_NRS) return D3DERR_INVALIDCALL;
    sg_st32(MEM, ARG(1), ST.rs[ARG(0)]);
    return D3D_OK;
}
/* State blocks: the game never calls them (d3d9_used in d3d9null/src/d3d9_slots.h); objects exist, Capture/Apply do
 * nothing, as on the null device. */
static uint32_t dv_create_sb(sg_com_call *c) {
    if (!ARG(1) || ARG(0) < 1 || ARG(0) > 3) return D3DERR_INVALIDCALL;
    return mt_put_new(c, ARG(1), mt_new(MT_K_SB, D, NULL), SG_E_OUTOFMEMORY);
}
static uint32_t dv_begin_sb(sg_com_call *c) {
    if (X(D)->recording) return D3DERR_INVALIDCALL;
    X(D)->recording = 1;
    return D3D_OK;
}
static uint32_t dv_end_sb(sg_com_call *c) {
    if (!ARG(0) || !X(D)->recording) return D3DERR_INVALIDCALL;
    X(D)->recording = 0;
    return mt_put_new(c, ARG(0), mt_new(MT_K_SB, D, NULL), SG_E_OUTOFMEMORY);
}
static uint32_t dv_set_clip_status(sg_com_call *c) { XSTORE(clip_status, 24); }
static uint32_t dv_get_clip_status(sg_com_call *c) { XLOAD(clip_status, 24); }
static int sampler_index(uint32_t s) {
    if (s < 16) return (int)s;
    if (s >= 256 && s <= 260) return (int)(s - 240); /* DMAP 256 -> 16, vertex 257..260 -> 17..20 */
    return -1;
}
static uint32_t dv_get_texture(sg_com_call *c) {
    int i = sampler_index(ARG(0));
    if (i < 0 || !ARG(1)) return D3DERR_INVALIDCALL;
    out_obj(c, ARG(1), ST.tex[i]);
    return D3D_OK;
}
static uint32_t dv_set_texture(sg_com_call *c) {
    int i = sampler_index(ARG(0));
    mt_obj *t = NULL;
    if (i < 0) return D3DERR_INVALIDCALL;
    if (ARG(1)) {
        t = mt_from_guest(MEM, ARG(1), -1);
        if (!t || (t->kind != MT_K_TEX && t->kind != MT_K_CUBE && t->kind != MT_K_VOLTEX)) return D3DERR_INVALIDCALL;
    }
    mt_obj *old = ST.tex[i];
    if (old == t) return D3D_OK;
    ST.tex[i] = t;
    ST.dirty |= MT_DIRTY_TEX;
    unbound(old);
    return D3D_OK;
}
static uint32_t dv_get_tss(sg_com_call *c) {
    if (ARG(0) >= MT_NSTAGES || ARG(1) >= MT_NTSS || !ARG(2)) return D3DERR_INVALIDCALL;
    sg_st32(MEM, ARG(2), ST.tss[ARG(0)][ARG(1)]);
    return D3D_OK;
}
static uint32_t dv_set_tss(sg_com_call *c) {
    if (ARG(0) < MT_NSTAGES && ARG(1) < MT_NTSS) ST.tss[ARG(0)][ARG(1)] = ARG(2), ST.dirty |= MT_DIRTY_TSS;
    return D3D_OK;
}
static uint32_t dv_get_ss(sg_com_call *c) {
    int i = sampler_index(ARG(0));
    if (i < 0 || ARG(1) >= MT_NSS || !ARG(2)) return D3DERR_INVALIDCALL;
    sg_st32(MEM, ARG(2), ST.ss[i][ARG(1)]);
    return D3D_OK;
}
static uint32_t dv_set_ss(sg_com_call *c) {
    int i = sampler_index(ARG(0));
    if (i < 0) return D3DERR_INVALIDCALL;
    if (ARG(1) < MT_NSS && ST.ss[i][ARG(1)] != ARG(2)) ST.ss[i][ARG(1)] = ARG(2), ST.dirty |= MT_DIRTY_SS;
    return D3D_OK;
}
static uint32_t dv_validate(sg_com_call *c) {
    if (ARG(0)) sg_st32(MEM, ARG(0), 1);
    return D3D_OK;
}
static uint32_t dv_set_palette(sg_com_call *c) { return ARG(1) ? D3D_OK : D3DERR_INVALIDCALL; }
static uint32_t dv_get_palette(sg_com_call *c) {
    if (!ARG(1)) return D3DERR_INVALIDCALL;
    memset(MEM + ARG(1), 0, 1024);
    return D3D_OK;
}
static uint32_t dv_set_cur_palette(sg_com_call *c) { X(D)->palette = ARG(0); return D3D_OK; }
static uint32_t dv_get_cur_palette(sg_com_call *c) {
    if (!ARG(0)) return D3DERR_INVALIDCALL;
    sg_st32(MEM, ARG(0), X(D)->palette);
    return D3D_OK;
}
static uint32_t dv_set_scissor(sg_com_call *c) {
    if (!ARG(0)) return D3DERR_INVALIDCALL;
    memcpy(ST.scissor, MEM + ARG(0), 16);
    ST.dirty |= MT_DIRTY_SCISSOR;
    return D3D_OK;
}
static uint32_t dv_get_scissor(sg_com_call *c) {
    if (!ARG(0)) return D3DERR_INVALIDCALL;
    memcpy(MEM + ARG(0), ST.scissor, 16);
    return D3D_OK;
}
static uint32_t dv_set_swvp(sg_com_call *c) { X(D)->swvp = ARG(0) != 0; return D3D_OK; }
static uint32_t dv_get_swvp(sg_com_call *c) { return X(D)->swvp; }
static uint32_t dv_set_npatch(sg_com_call *c) { X(D)->npatch = SG_COM_ARGF(c, 0); return D3D_OK; }
static uint32_t dv_get_npatch(sg_com_call *c) {
    sg_fpu_push(c->ctx, (sg_f80)X(D)->npatch);
    return 0;
}
/* Draw stubs (the pipeline replaces them): counted like the null device's, nothing drawn. */
static uint32_t stub_draw(uint32_t prims) {
    static int once;
    if (!once++) MT_LOG("draws are stubs in this build (no pipeline package linked): counted, not drawn\n");
    nst()->draws++;
    nst()->primitives += prims;
    g_mst.draws_stubbed++;
    return D3D_OK;
}
static uint32_t dv_draw(sg_com_call *c) { return stub_draw(ARG(2)); }
static uint32_t dv_draw_indexed(sg_com_call *c) { return stub_draw(ARG(5)); }
static uint32_t dv_draw_up(sg_com_call *c) { /* (type, count, data, stride): D3D9 unbinds stream 0 afterwards */
    if (!ARG(2)) return D3DERR_INVALIDCALL;
    mt_obj *old = ST.streams[0].vb;
    ST.streams[0].vb = NULL, ST.streams[0].offset = ST.streams[0].stride = 0;
    ST.dirty |= MT_DIRTY_STREAMS;
    unbound(old);
    return stub_draw(ARG(1));
}
static uint32_t dv_draw_indexed_up(sg_com_call *c) { /* (type, min, num, count, idx, idxfmt, data, stride): also the indices */
    if (!ARG(4) || !ARG(6)) return D3DERR_INVALIDCALL;
    mt_obj *o1 = ST.streams[0].vb, *o2 = ST.ib;
    ST.streams[0].vb = NULL, ST.streams[0].offset = ST.streams[0].stride = 0;
    ST.ib = NULL;
    ST.dirty |= MT_DIRTY_STREAMS | MT_DIRTY_IB;
    unbound(o1);
    unbound(o2);
    return stub_draw(ARG(3));
}
static uint32_t dv_process(sg_com_call *c) { return ARG(3) ? D3D_OK : D3DERR_INVALIDCALL; } /* not used by the game */
static uint32_t dv_create_vdecl(sg_com_call *c) { /* (D3DVERTEXELEMENT9*, IDirect3DVertexDeclaration9**) */
    uint32_t p = ARG(0), n = 0;
    if (!p || !ARG(1)) return D3DERR_INVALIDCALL;
    while (n < 65 && sg_ld16(MEM, p + 8u * n) != 0xff) n++; /* D3DDECL_END has Stream 0xFF */
    if (n >= 65) return D3DERR_INVALIDCALL;
    mt_obj *v = mt_new(MT_K_VDECL, D, NULL);
    if (!v) return mt_put_new(c, ARG(1), NULL, SG_E_OUTOFMEMORY);
    v->decl = (uint8_t *)malloc((n + 1) * 8u);
    memcpy(v->decl, MEM + p, (n + 1) * 8u);
    v->decl_count = n;
    return mt_put_new(c, ARG(1), v, D3D_OK);
}
static uint32_t dv_set_vdecl(sg_com_call *c) {
    mt_obj *v = NULL, *old = ST.vdecl;
    if (ARG(0) && !(v = mt_from_guest(MEM, ARG(0), MT_K_VDECL))) return D3DERR_INVALIDCALL;
    if (v == old) return D3D_OK;
    ST.vdecl = v;
    ST.dirty |= MT_DIRTY_VDECL;
    unbound(old);
    return D3D_OK;
}
static uint32_t dv_get_vdecl(sg_com_call *c) {
    if (!ARG(0)) return D3DERR_INVALIDCALL;
    return out_obj(c, ARG(0), ST.vdecl);
}
static uint32_t dv_set_fvf(sg_com_call *c) { ST.fvf = ARG(0); ST.dirty |= MT_DIRTY_FVF; return D3D_OK; }
static uint32_t dv_get_fvf(sg_com_call *c) {
    if (!ARG(0)) return D3DERR_INVALIDCALL;
    sg_st32(MEM, ARG(0), ST.fvf);
    return D3D_OK;
}
static uint32_t shader_len(const uint8_t *mem, uint32_t p) {
    uint32_t i = 1, n = 1u << 18, v = sg_ld32(mem, p);
    if ((v & 0xfffe0000u) != 0xfffe0000u) return 0; /* 0xFFFE vs, 0xFFFF ps */
    while (i < n) {
        uint32_t t = sg_ld32(mem, p + 4u * i);
        if (t == 0x0000ffffu) return 4u * (i + 1);
        if ((t & 0xffffu) == 0xfffeu) i += 1 + (t >> 16);
        else i++;
    }
    return 0;
}
static uint64_t fnv1a(const uint8_t *p, uint32_t n) {
    uint64_t h = 0xcbf29ce484222325ull;
    for (uint32_t i = 0; i < n; i++) h = (h ^ p[i]) * 0x100000001b3ull;
    return h;
}
/* The bytecode is kept and handed to the pipeline, which compiles it on a worker. A version token of the wrong
 * kind (a ps blob to CreateVertexShader) is refused, as D3D9 does. */
static uint32_t create_shader(sg_com_call *c, int ps) {
    uint32_t len;
    if (!ARG(0) || !ARG(1)) return D3DERR_INVALIDCALL;
    len = shader_len(MEM, ARG(0));
    if (!len || ((sg_ld32(MEM, ARG(0)) >> 16) == 0xffff) != ps) return mt_put_new(c, ARG(1), NULL, D3DERR_INVALIDCALL);
    mt_obj *s = mt_new(ps ? MT_K_PS : MT_K_VS, D, NULL);
    if (!s) return mt_put_new(c, ARG(1), NULL, SG_E_OUTOFMEMORY);
    s->code = (uint8_t *)malloc(len);
    memcpy(s->code, MEM + ARG(0), len);
    s->code_len = len;
    s->code_hash = fnv1a(s->code, len);
    g_mst.shader_creates++;
    mt_pl_shader_created(s);   /* translated and compiled on a worker from here (pl_shader.m) */
    return mt_put_new(c, ARG(1), s, D3D_OK);
}
static uint32_t dv_create_vs(sg_com_call *c) { return create_shader(c, 0); }
static uint32_t dv_create_ps(sg_com_call *c) { return create_shader(c, 1); }
static uint32_t set_shader(sg_com_call *c, mt_obj **slot, int kind, uint32_t bit) {
    mt_obj *s = NULL, *old = *slot;
    if (ARG(0) && !(s = mt_from_guest(MEM, ARG(0), kind))) return D3DERR_INVALIDCALL;
    if (s == old) return D3D_OK;
    *slot = s;
    ST.dirty |= bit;
    unbound(old);
    return D3D_OK;
}
static uint32_t dv_set_vs(sg_com_call *c) { return set_shader(c, &ST.vs, MT_K_VS, MT_DIRTY_VS); }
static uint32_t dv_set_ps(sg_com_call *c) { return set_shader(c, &ST.ps, MT_K_PS, MT_DIRTY_PS); }
static uint32_t dv_get_vs(sg_com_call *c) {
    if (!ARG(0)) return D3DERR_INVALIDCALL;
    return out_obj(c, ARG(0), ST.vs);
}
static uint32_t dv_get_ps(sg_com_call *c) {
    if (!ARG(0)) return D3DERR_INVALIDCALL;
    return out_obj(c, ARG(0), ST.ps);
}
/* Shader constants: (start, data, count) in units of 4 floats / 4 ints / 1 BOOL. */
static uint32_t consts(sg_com_call *c, void *shadow, uint32_t max, uint32_t unit, int set, uint32_t bit, uint32_t *hi) {
    uint32_t start = ARG(0), p = ARG(1), n = ARG(2);
    if (!p || start > max || n > max - start) return D3DERR_INVALIDCALL;
    if (set) {
        memcpy((uint8_t *)shadow + start * unit, MEM + p, n * unit);
        ST.dirty |= bit;
        if (hi && start + n > *hi) *hi = start + n;
    } else {
        memcpy(MEM + p, (uint8_t *)shadow + start * unit, n * unit);
    }
    return D3D_OK;
}
static uint32_t dv_set_vsf(sg_com_call *c) { return consts(c, ST.vsf, MT_VS_NCONSTF, 16, 1, MT_DIRTY_VS_CONST, &ST.vsf_hi); }
static uint32_t dv_get_vsf(sg_com_call *c) { return consts(c, ST.vsf, MT_VS_NCONSTF, 16, 0, 0, NULL); }
static uint32_t dv_set_vsi(sg_com_call *c) { return consts(c, ST.vsi, MT_NCONSTI, 16, 1, MT_DIRTY_VS_CONST, NULL); }
static uint32_t dv_get_vsi(sg_com_call *c) { return consts(c, ST.vsi, MT_NCONSTI, 16, 0, 0, NULL); }
static uint32_t dv_set_vsb(sg_com_call *c) { return consts(c, ST.vsb, MT_NCONSTB, 4, 1, MT_DIRTY_VS_CONST, NULL); }
static uint32_t dv_get_vsb(sg_com_call *c) { return consts(c, ST.vsb, MT_NCONSTB, 4, 0, 0, NULL); }
static uint32_t dv_set_psf(sg_com_call *c) { return consts(c, ST.psf, MT_PS_NCONSTF, 16, 1, MT_DIRTY_PS_CONST, &ST.psf_hi); }
static uint32_t dv_get_psf(sg_com_call *c) { return consts(c, ST.psf, MT_PS_NCONSTF, 16, 0, 0, NULL); }
static uint32_t dv_set_psi(sg_com_call *c) { return consts(c, ST.psi, MT_NCONSTI, 16, 1, MT_DIRTY_PS_CONST, NULL); }
static uint32_t dv_get_psi(sg_com_call *c) { return consts(c, ST.psi, MT_NCONSTI, 16, 0, 0, NULL); }
static uint32_t dv_set_psb(sg_com_call *c) { return consts(c, ST.psb, MT_NCONSTB, 4, 1, MT_DIRTY_PS_CONST, NULL); }
static uint32_t dv_get_psb(sg_com_call *c) { return consts(c, ST.psb, MT_NCONSTB, 4, 0, 0, NULL); }
static uint32_t dv_set_stream(sg_com_call *c) { /* (stream, vb, offset, stride) */
    mt_obj *vb = NULL;
    if (ARG(0) >= MT_NSTREAMS) return D3DERR_INVALIDCALL;
    if (ARG(1) && !(vb = mt_from_guest(MEM, ARG(1), MT_K_VB))) return D3DERR_INVALIDCALL;
    mt_stream *s = &ST.streams[ARG(0)];
    mt_obj *old = s->vb;
    s->vb = vb, s->offset = ARG(2), s->stride = ARG(3);
    ST.dirty |= MT_DIRTY_STREAMS;
    if (old != vb) unbound(old);
    return D3D_OK;
}
static uint32_t dv_get_stream(sg_com_call *c) { /* (stream, ppVB, pOffset, pStride) */
    if (ARG(0) >= MT_NSTREAMS || !ARG(1) || !ARG(2) || !ARG(3)) return D3DERR_INVALIDCALL;
    out_obj(c, ARG(1), ST.streams[ARG(0)].vb);
    sg_st32(MEM, ARG(2), ST.streams[ARG(0)].offset);
    sg_st32(MEM, ARG(3), ST.streams[ARG(0)].stride);
    return D3D_OK;
}
static uint32_t dv_set_freq(sg_com_call *c) {
    if (ARG(0) >= MT_NSTREAMS) return D3DERR_INVALIDCALL;
    ST.streams[ARG(0)].freq = ARG(1);
    ST.dirty |= MT_DIRTY_STREAMS;
    return D3D_OK;
}
static uint32_t dv_get_freq(sg_com_call *c) {
    if (ARG(0) >= MT_NSTREAMS || !ARG(1)) return D3DERR_INVALIDCALL;
    sg_st32(MEM, ARG(1), ST.streams[ARG(0)].freq);
    return D3D_OK;
}
static uint32_t dv_set_indices(sg_com_call *c) {
    mt_obj *ib = NULL, *old = ST.ib;
    if (ARG(0) && !(ib = mt_from_guest(MEM, ARG(0), MT_K_IB))) return D3DERR_INVALIDCALL;
    ST.ib = ib;
    ST.dirty |= MT_DIRTY_IB;
    if (old != ib) unbound(old);
    return D3D_OK;
}
static uint32_t dv_get_indices(sg_com_call *c) {
    if (!ARG(0)) return D3DERR_INVALIDCALL;
    return out_obj(c, ARG(0), ST.ib);
}
static uint32_t dv_patch(sg_com_call *c) { return D3D_OK; }

static const sg_com_method device_methods[] = {
    SG_COM_IUNKNOWN,
    M(Device, TestCooperativeLevel, dv_test_coop), M(Device, GetAvailableTextureMem, dv_texmem),
    M(Device, EvictManagedResources, dv_evict), M(Device, GetDirect3D, dv_get_d3d), M(Device, GetDeviceCaps, dv_caps),
    M(Device, GetDisplayMode, dv_display_mode), M(Device, GetCreationParameters, dv_creation),
    M(Device, SetCursorProperties, ok0), M(Device, SetCursorPosition, dv_cursor_pos), M(Device, ShowCursor, dv_show_cursor),
    M(Device, CreateAdditionalSwapChain, dv_add_swapchain), M(Device, GetSwapChain, dv_get_swapchain),
    M(Device, GetNumberOfSwapChains, dv_num_swapchains), M(Device, Reset, dv_reset), M(Device, Present, dv_present),
    M(Device, GetBackBuffer, dv_back_buffer), M(Device, GetRasterStatus, dv_raster), M(Device, SetDialogBoxMode, ok0),
    M(Device, SetGammaRamp, dv_set_gamma), M(Device, GetGammaRamp, dv_get_gamma),
    M(Device, CreateTexture, dv_create_texture), M(Device, CreateVolumeTexture, dv_create_volume),
    M(Device, CreateCubeTexture, dv_create_cube), M(Device, CreateVertexBuffer, dv_create_vb),
    M(Device, CreateIndexBuffer, dv_create_ib), M(Device, CreateRenderTarget, dv_create_rt),
    M(Device, CreateDepthStencilSurface, dv_create_ds), M(Device, UpdateSurface, mt_dv_update_surface),
    M(Device, UpdateTexture, mt_dv_update_texture), M(Device, GetRenderTargetData, mt_dv_rt_data),
    M(Device, GetFrontBufferData, mt_dv_front_data), M(Device, StretchRect, mt_dv_stretch), M(Device, ColorFill, mt_dv_color_fill),
    M(Device, CreateOffscreenPlainSurface, dv_create_offscreen), M(Device, SetRenderTarget, dv_set_rt),
    M(Device, GetRenderTarget, dv_get_rt), M(Device, SetDepthStencilSurface, dv_set_ds),
    M(Device, GetDepthStencilSurface, dv_get_ds), M(Device, BeginScene, dv_begin), M(Device, EndScene, dv_end),
    M(Device, Clear, dv_clear), M(Device, SetTransform, dv_set_transform), M(Device, GetTransform, dv_get_transform),
    M(Device, MultiplyTransform, dv_mul_transform), M(Device, SetViewport, dv_set_vp), M(Device, GetViewport, dv_get_vp),
    M(Device, SetMaterial, dv_set_material), M(Device, GetMaterial, dv_get_material), M(Device, SetLight, dv_set_light),
    M(Device, GetLight, dv_get_light), M(Device, LightEnable, dv_light_enable), M(Device, GetLightEnable, dv_get_light_enable),
    M(Device, SetClipPlane, dv_set_clip), M(Device, GetClipPlane, dv_get_clip), M(Device, SetRenderState, dv_set_rs),
    M(Device, GetRenderState, dv_get_rs), M(Device, CreateStateBlock, dv_create_sb), M(Device, BeginStateBlock, dv_begin_sb),
    M(Device, EndStateBlock, dv_end_sb), M(Device, SetClipStatus, dv_set_clip_status),
    M(Device, GetClipStatus, dv_get_clip_status), M(Device, GetTexture, dv_get_texture), M(Device, SetTexture, dv_set_texture),
    M(Device, GetTextureStageState, dv_get_tss), M(Device, SetTextureStageState, dv_set_tss),
    M(Device, GetSamplerState, dv_get_ss), M(Device, SetSamplerState, dv_set_ss), M(Device, ValidateDevice, dv_validate),
    M(Device, SetPaletteEntries, dv_set_palette), M(Device, GetPaletteEntries, dv_get_palette),
    M(Device, SetCurrentTexturePalette, dv_set_cur_palette), M(Device, GetCurrentTexturePalette, dv_get_cur_palette),
    M(Device, SetScissorRect, dv_set_scissor), M(Device, GetScissorRect, dv_get_scissor),
    M(Device, SetSoftwareVertexProcessing, dv_set_swvp), M(Device, GetSoftwareVertexProcessing, dv_get_swvp),
    M(Device, SetNPatchMode, dv_set_npatch), M(Device, GetNPatchMode, dv_get_npatch), M(Device, DrawPrimitive, dv_draw),
    M(Device, DrawIndexedPrimitive, dv_draw_indexed), M(Device, DrawPrimitiveUP, dv_draw_up),
    M(Device, DrawIndexedPrimitiveUP, dv_draw_indexed_up), M(Device, ProcessVertices, dv_process),
    M(Device, CreateVertexDeclaration, dv_create_vdecl), M(Device, SetVertexDeclaration, dv_set_vdecl),
    M(Device, GetVertexDeclaration, dv_get_vdecl), M(Device, SetFVF, dv_set_fvf), M(Device, GetFVF, dv_get_fvf),
    M(Device, CreateVertexShader, dv_create_vs), M(Device, SetVertexShader, dv_set_vs), M(Device, GetVertexShader, dv_get_vs),
    M(Device, SetVertexShaderConstantF, dv_set_vsf), M(Device, GetVertexShaderConstantF, dv_get_vsf),
    M(Device, SetVertexShaderConstantI, dv_set_vsi), M(Device, GetVertexShaderConstantI, dv_get_vsi),
    M(Device, SetVertexShaderConstantB, dv_set_vsb), M(Device, GetVertexShaderConstantB, dv_get_vsb),
    M(Device, SetStreamSource, dv_set_stream), M(Device, GetStreamSource, dv_get_stream),
    M(Device, SetStreamSourceFreq, dv_set_freq), M(Device, GetStreamSourceFreq, dv_get_freq),
    M(Device, SetIndices, dv_set_indices), M(Device, GetIndices, dv_get_indices), M(Device, CreatePixelShader, dv_create_ps),
    M(Device, SetPixelShader, dv_set_ps), M(Device, GetPixelShader, dv_get_ps),
    M(Device, SetPixelShaderConstantF, dv_set_psf), M(Device, GetPixelShaderConstantF, dv_get_psf),
    M(Device, SetPixelShaderConstantI, dv_set_psi), M(Device, GetPixelShaderConstantI, dv_get_psi),
    M(Device, SetPixelShaderConstantB, dv_set_psb), M(Device, GetPixelShaderConstantB, dv_get_psb),
    M(Device, DrawRectPatch, dv_patch), M(Device, DrawTriPatch, dv_patch), M(Device, DeletePatch, dv_patch),
    M(Device, CreateQuery, mt_dv_create_query),
};
extern "C" sg_com_handler *mt_device_handler(unsigned slot) {
    for (unsigned i = 0; i < SG_COM_N(device_methods); i++)
        if (device_methods[i].slot == slot) return device_methods[i].fn;
    return NULL;
}

/* ---- swap chains, declarations, shaders, state blocks ------------------------------------------------- */
static uint32_t sc_present(sg_com_call *c) { /* (src, dst, hwnd, dirty, flags) */
    mt_dev *d = O->dev;
    if (!d || !d->alive) return D3DERR_INVALIDCALL;
    return mt_present(c, d, O, ARG(2) ? ARG(2) : O->sc_hwnd);
}
static uint32_t sc_back_buffer(sg_com_call *c) { /* (index, type, IDirect3DSurface9**) */
    if (!ARG(2)) return D3DERR_INVALIDCALL;
    if (ARG(0) >= (O->pp[PP_COUNT] ? O->pp[PP_COUNT] : 1) || !O->backbuf) {
        sg_st32(MEM, ARG(2), 0);
        return D3DERR_INVALIDCALL;
    }
    sg_com_obj_addref(&O->backbuf->com);
    sg_st32(MEM, ARG(2), mt_guest_of(O->backbuf));
    return D3D_OK;
}
static uint32_t sc_raster(sg_com_call *c) {
    if (!ARG(0)) return D3DERR_INVALIDCALL;
    sg_st32(MEM, ARG(0), 0);
    sg_st32(MEM, ARG(0) + 4, 0);
    return D3D_OK;
}
static uint32_t sc_display_mode(sg_com_call *c) {
    if (!ARG(0)) return D3DERR_INVALIDCALL;
    put_mode(c, ARG(0), O->pp);
    return D3D_OK;
}
static uint32_t sc_front_data(sg_com_call *c) {
    mt_dev *d = O->dev;
    if (!d || !d->alive) return D3DERR_INVALIDCALL;
    return D3D_OK; /* not used by the game; the destination is left as it is (as the null device) */
}
static uint32_t sc_pp(sg_com_call *c) {
    if (!ARG(0)) return D3DERR_INVALIDCALL;
    for (int i = 0; i < D3DPP_DWORDS; i++) sg_st32(MEM, ARG(0) + 4u * i, O->pp[i]);
    return D3D_OK;
}
static const sg_com_method swapchain_methods[] = {
    SG_COM_IUNKNOWN, M(SwapChain, Present, sc_present), M(SwapChain, GetFrontBufferData, sc_front_data),
    M(SwapChain, GetBackBuffer, sc_back_buffer), M(SwapChain, GetRasterStatus, sc_raster),
    M(SwapChain, GetDisplayMode, sc_display_mode), M(SwapChain, GetDevice, mt_r_get_device),
    M(SwapChain, GetPresentParameters, sc_pp)};
static uint32_t blob_get(sg_com_call *c) { /* GetDeclaration(elems, UINT *num) / GetFunction(data, UINT *size) */
    uint32_t data = ARG(0), np = ARG(1);
    if (!np) return D3DERR_INVALIDCALL;
    if (O->kind == MT_K_VDECL) {
        uint32_t n = O->decl_count + 1;
        if (data) memcpy(MEM + data, O->decl, n * 8u);
        sg_st32(MEM, np, n);
        return D3D_OK;
    }
    if (data) {
        if (sg_ld32(MEM, np) < O->code_len) return D3DERR_INVALIDCALL;
        memcpy(MEM + data, O->code, O->code_len);
    }
    sg_st32(MEM, np, O->code_len);
    return D3D_OK;
}
static const sg_com_method vdecl_methods[] = {SG_COM_IUNKNOWN, M(VertexDeclaration, GetDevice, mt_r_get_device),
                                              M(VertexDeclaration, GetDeclaration, blob_get)};
static const sg_com_method vs_methods[] = {SG_COM_IUNKNOWN, M(VertexShader, GetDevice, mt_r_get_device), M(VertexShader, GetFunction, blob_get)};
static const sg_com_method ps_methods[] = {SG_COM_IUNKNOWN, M(PixelShader, GetDevice, mt_r_get_device), M(PixelShader, GetFunction, blob_get)};
static const sg_com_method stateblock_methods[] = {SG_COM_IUNKNOWN, M(StateBlock, GetDevice, mt_r_get_device),
                                                   M(StateBlock, Capture, ok0), M(StateBlock, Apply, ok0)};
static const sg_com_method d3d9_methods[] = {
    SG_COM_IUNKNOWN,
    M(D3D9, RegisterSoftwareDevice, d_shared), M(D3D9, GetAdapterCount, d_shared), M(D3D9, GetAdapterIdentifier, d_shared),
    M(D3D9, GetAdapterModeCount, d_shared), M(D3D9, EnumAdapterModes, d_shared), M(D3D9, GetAdapterDisplayMode, d_shared),
    M(D3D9, CheckDeviceType, d_shared), M(D3D9, CheckDeviceFormat, d_shared), M(D3D9, CheckDeviceMultiSampleType, d_shared),
    M(D3D9, CheckDepthStencilMatch, d_shared), M(D3D9, CheckDeviceFormatConversion, d_shared), M(D3D9, GetDeviceCaps, d_shared),
    M(D3D9, GetAdapterMonitor, d_shared), M(D3D9, CreateDevice, d_create_device),
};

/* ---- interface descriptors: the null device's slot layout, names and IIDs --------------------------------- */
#define IFACE(name, S, s, methods) {name, {0, 0, 0, {0}}, S##_NSLOTS, methods, SG_COM_N(methods), s##_names, s##_args, D3DERR_INVALIDCALL, NULL, 0, 0}
#define IFACE_N(name, S, s, methods, n) {name, {0, 0, 0, {0}}, S##_NSLOTS, methods, 0, s##_names, s##_args, D3DERR_INVALIDCALL, NULL, 0, 0}
sg_com_iface mt_iface[MT_K_NKIND] = {
    IFACE("IDirect3D9", D3D9, d3d9, d3d9_methods),
    IFACE("IDirect3DDevice9", DEVICE, device, device_methods),
    IFACE("IDirect3DSwapChain9", SWAPCHAIN, swapchain, swapchain_methods),
    IFACE_N("IDirect3DTexture9", TEXTURE, texture, mt_texture_methods, 0),
    IFACE_N("IDirect3DCubeTexture9", CUBETEXTURE, cubetexture, mt_cube_methods, 0),
    IFACE_N("IDirect3DVolumeTexture9", VOLUMETEXTURE, volumetexture, mt_volumetex_methods, 0),
    IFACE_N("IDirect3DSurface9", SURFACE, surface, mt_surface_methods, 0),
    IFACE_N("IDirect3DVolume9", VOLUME, volume, mt_volume_methods, 0),
    IFACE_N("IDirect3DVertexBuffer9", VERTEXBUFFER, vertexbuffer, mt_vb_methods, 0),
    IFACE_N("IDirect3DIndexBuffer9", INDEXBUFFER, indexbuffer, mt_ib_methods, 0),
    IFACE("IDirect3DVertexDeclaration9", VERTEXDECLARATION, vertexdeclaration, vdecl_methods),
    IFACE("IDirect3DVertexShader9", VERTEXSHADER, vertexshader, vs_methods),
    IFACE("IDirect3DPixelShader9", PIXELSHADER, pixelshader, ps_methods),
    IFACE_N("IDirect3DQuery9", QUERY, query, mt_query_methods, 0),
    IFACE("IDirect3DStateBlock9", STATEBLOCK, stateblock, stateblock_methods),
};
static void ifaces_init(void) {
    sg_com_iface *nulls[MT_K_NKIND] = {&d3d9_iface_d3d9, &d3d9_iface_device, &d3d9_iface_swapchain, &d3d9_iface_texture,
                                       &d3d9_iface_cube, &d3d9_iface_volumetex, &d3d9_iface_surface, &d3d9_iface_volume,
                                       &d3d9_iface_vb, &d3d9_iface_ib, &d3d9_iface_vdecl, &d3d9_iface_vs, &d3d9_iface_ps,
                                       &d3d9_iface_query, &d3d9_iface_stateblock};
    const unsigned counts[MT_K_NKIND] = {0, 0, 0, mt_n_texture_methods, mt_n_cube_methods, mt_n_volumetex_methods, mt_n_surface_methods,
                                         mt_n_volume_methods, mt_n_vb_methods, mt_n_ib_methods, 0, 0, 0, mt_n_query_methods, 0};
    for (int k = 0; k < MT_K_NKIND; k++) {
        mt_iface[k].iid = nulls[k]->iid;
        if (counts[k]) mt_iface[k].nmethods = counts[k];
        mt_iface[k].lock = d3d9null_device_lock(); /* the one device mutex (DEVIATIONS 3.12) */
    }
    /* the pipeline's handlers replace the device layer's slot by slot */
    sg_com_method *dm = (sg_com_method *)calloc(SG_COM_N(device_methods) + DEVICE_NSLOTS, sizeof *dm);
    unsigned n = 0, over = 0;
    int have[DEVICE_NSLOTS] = {0};
    for (unsigned i = 0; i < SG_COM_N(device_methods); i++) {
        dm[n] = device_methods[i];
        have[dm[n].slot] = 1;
        sg_com_handler *h = mt_pipeline_handler(dm[n].slot);
        if (h) dm[n].fn = h, over++;
        n++;
    }
    for (unsigned s = 3; s < DEVICE_NSLOTS; s++) { /* a slot src/ leaves to the default but pipeline/ implements */
        sg_com_handler *h = have[s] ? NULL : mt_pipeline_handler(s);
        if (h) dm[n++] = (sg_com_method){device_names[s], (uint16_t)s, (uint16_t)(4 + 4 * device_args[s]), h}, over++;
    }
    mt_iface[MT_K_DEV].methods = dm;
    mt_iface[MT_K_DEV].nmethods = n;
    if (over || mt_verbose) MT_LOG("device vtable: %u slots from the pipeline package, %u from the device layer\n", over, n - over);
}

/* ---- the call ring, the watch line, device loss and the stall report ----- */
struct ring_ent { uint16_t kind, meth; uint32_t self, a[4]; uint64_t frame; };
static ring_ent g_ring[256];
static uint32_t g_ring_n;
static sg_com_method *g_mcopy[MT_K_NKIND];
static sg_com_handler **g_morig[MT_K_NKIND];
static int g_ring_on = 1;
/* Every guest call runs here: an autorelease pool around the handler (the game's threads have none, and
 * Metal hands out autoreleased objects), and the call ring the stall report prints. */
static uint32_t ring_tramp(sg_com_call *c) {
    unsigned k = (unsigned)(c->iface - mt_iface);
    uint32_t r = SG_E_FAIL;
    if (k < MT_K_NKIND && g_mcopy[k]) {
        unsigned m = (unsigned)(c->m - g_mcopy[k]);
        if (g_ring_on) {
            ring_ent *e = &g_ring[g_ring_n++ & 255u];
            e->kind = (uint16_t)k, e->meth = (uint16_t)m, e->self = c->self, e->frame = g_mst.frames;
            for (int i = 0; i < 4; i++) e->a[i] = SG_COM_ARG(c, i);
        }
        @autoreleasepool {
            r = g_morig[k][m](c);
        }
    }
    return r;
}
static void ring_install(void) {
    const char *s = getenv("SPORE_METAL_RING");
    if (s && *s == '0') g_ring_on = 0;
    for (int k = 0; k < MT_K_NKIND; k++) {
        unsigned n = mt_iface[k].nmethods;
        g_mcopy[k] = (sg_com_method *)calloc(n, sizeof *g_mcopy[k]);
        g_morig[k] = (sg_com_handler **)calloc(n, sizeof *g_morig[k]);
        if (!g_mcopy[k] || !g_morig[k]) { g_mcopy[k] = NULL; continue; }
        for (unsigned i = 0; i < n; i++) {
            g_mcopy[k][i] = mt_iface[k].methods[i];
            g_morig[k][i] = g_mcopy[k][i].fn;
            g_mcopy[k][i].fn = ring_tramp;
        }
        mt_iface[k].methods = g_mcopy[k];
    }
}
static void ring_dump(int fd, unsigned last) {
    uint32_t n = g_ring_n, from = n > last ? n - last : 0;
    dprintf(fd, "d3d9metal: the last %u D3D9 calls (oldest first; this, then the first 4 stack arguments):\n", n - from);
    for (uint32_t i = from; i < n; i++) {
        const ring_ent *e = &g_ring[i & 255u];
        const sg_com_iface *f = &mt_iface[e->kind];
        dprintf(fd, "  #%u frame %llu %s::%s this 0x%08x args 0x%x 0x%x 0x%x 0x%x\n", i, (unsigned long long)e->frame, f->name,
                e->meth < f->nmethods ? f->methods[e->meth].name : "?", e->self, e->a[0], e->a[1], e->a[2], e->a[3]);
    }
}
static uint64_t rss_bytes(uint64_t *footprint) {
    task_vm_info_data_t vi;
    mach_msg_type_number_t n = TASK_VM_INFO_COUNT;
    if (task_info(mach_task_self(), TASK_VM_INFO, (task_info_t)&vi, &n) != KERN_SUCCESS) return 0;
    if (footprint) *footprint = vi.phys_footprint;
    return vi.resident_size;
}
/* SPORE_METAL_WATCH=N: every N seconds (from Present) one line: live objects by kind, shadow and Metal
 * memory, RSS and footprint, copies, renames, waits, command buffers. */
extern "C" int mt_present_times(double *p50_ms, double *p90_ms, double *max_ms, double *mean_ms, uint64_t *metal_alloc);
static void watch_line(void) {
    uint64_t fp = 0, rss = rss_bytes(&fp), alloc = 0;
    double fr = g_mst.frames ? (double)g_mst.frames : 1.0, p50, p90, pmax, pmean;
    static uint64_t t0;
    if (!t0) t0 = mt_now_ns();
    int np = mt_present_times(&p50, &p90, &pmax, &pmean, &alloc);
    int k = snprintf(g_watch_line, sizeof g_watch_line,
             "t=%.0f s present-to-present n %d mean %.2f p50 %.2f p90 %.2f max %.2f ms; device allocated %.1f MB; ",
             (mt_now_ns() - t0) / 1e9, np, pmean, p50, p90, pmax, alloc / 1048576.0);
    if (k < 0 || k >= (int)sizeof g_watch_line) k = 0;
    snprintf(g_watch_line + k, sizeof g_watch_line - k,
             "frame %llu: live tex %lld cube %lld voltex %lld surf %lld vb %lld ib %lld query %lld vs %lld ps %lld vdecl %lld; "
             "shadow %.1f MB, metal %.1f MB; rss %.0f MB, footprint %.0f MB; locks %llu, out %.1f KB/frame, in %.1f KB/frame, "
             "copy %.1f us/frame; renames %llu, waits %llu (%.1f ms), cmdbufs %llu, staged %llu, direct %llu, readbacks %llu, "
             "stub draws %llu, query S_FALSE %llu, drawables nil %llu, gpu errors %llu",
             (unsigned long long)g_mst.frames, (long long)g_live_kind[MT_K_TEX], (long long)g_live_kind[MT_K_CUBE],
             (long long)g_live_kind[MT_K_VOLTEX], (long long)g_live_kind[MT_K_SURF], (long long)g_live_kind[MT_K_VB],
             (long long)g_live_kind[MT_K_IB], (long long)g_live_kind[MT_K_QUERY], (long long)g_live_kind[MT_K_VS],
             (long long)g_live_kind[MT_K_PS], (long long)g_live_kind[MT_K_VDECL], g_mst.shadow_bytes / 1048576.0,
             g_mst.metal_bytes / 1048576.0, rss / 1048576.0, fp / 1048576.0, (unsigned long long)g_mst.locks,
             g_mst.bytes_out / fr / 1024.0, g_mst.bytes_in / fr / 1024.0, g_mst.copy_ns / fr / 1000.0,
             (unsigned long long)g_mst.renames, (unsigned long long)g_mst.waits, g_mst.wait_ns / 1e6,
             (unsigned long long)g_mst.cmdbufs, (unsigned long long)g_mst.staged_uploads, (unsigned long long)g_mst.direct_uploads,
             (unsigned long long)g_mst.readbacks, (unsigned long long)g_mst.draws_stubbed, (unsigned long long)g_mst.query_waits,
             (unsigned long long)g_mst.drawables_nil, (unsigned long long)g_mst.gpu_errors);
}
static void pl_stats_lines(void);
extern "C" void mt_watch_tick(int force) {
    static double every = -1;
    static uint64_t last;
    if (every < 0) {
        const char *s = getenv("SPORE_METAL_WATCH");
        every = s && *s ? atof(s) : 0;
    }
    if (every <= 0 && !force) return;
    uint64_t t = mt_now_ns();
    if (!force && last && (t - last) < (uint64_t)(every * 1e9)) return;
    last = t;
    watch_line();
    MT_LOG("watch %s\n", g_watch_line);
    pl_stats_lines();
}
/* Weak defaults, overridden by the pipeline package's strong definitions (pl_draw.m, pl_shader.m). Mach-O's static
 * linker needs a definition for a weak reference, so the device layer built alone (make test without WITH_PIPELINE) links these;
 * a NULL d3d9metal_pl_stats_names means "no pipeline linked". */
extern "C" __attribute__((weak)) void d3d9metal_pl_stats(uint64_t out[14]) { memset(out, 0, 14 * sizeof *out); }
extern "C" __attribute__((weak)) const char *d3d9metal_pl_stats_names = NULL;
extern "C" __attribute__((weak)) void d3d9metal_pl_shader_stats(uint64_t *a, uint64_t *b, uint64_t *c) { *a = *b = *c = 0; }
extern "C" __attribute__((weak)) int d3d9metal_pl_async_stats(uint64_t v[14]) { (void)v; return 0; }
static void pl_stats_lines(void) { /* the pipeline's counters by name, and the shader compiles */
    if (d3d9metal_pl_stats_names) {
        uint64_t v[14];
        char line[600];
        int k = 0;
        const char *n = d3d9metal_pl_stats_names;
        d3d9metal_pl_stats(v);
        for (int i = 0; i < 14 && *n && k < (int)sizeof line - 40; i++) {
            const char *e = strchr(n, ' ');
            int len = e ? (int)(e - n) : (int)strlen(n);
            k += snprintf(line + k, sizeof line - k, "%s%.*s %llu", i ? ", " : "", len, n, (unsigned long long)v[i]);
            n += len + (e ? 1 : 0);
        }
        MT_LOG("pipeline stats: %s\n", line);
    }
    if (d3d9metal_pl_stats_names) {
        uint64_t a = 0, b = 0, c = 0;
        d3d9metal_pl_shader_stats(&a, &b, &c);
        MT_LOG("shader stats: %llu compiles, %llu failed, %.3f s at draws (compiling or waiting on the render thread)\n",
               (unsigned long long)a, (unsigned long long)b, c / 1e9);
        uint64_t v[14];
        if (d3d9metal_pl_async_stats(v)) /* created, queued, batches, batch_shaders, solo, raced, waited, wait, max, worker time, archive */
            MT_LOG("shader async: %llu created, %llu batches (%llu shaders, %.1f s on workers), %llu compiled alone at a draw, "
                   "%llu raced, %llu waits (%.1f ms, longest %.1f ms); pipeline archive %llu hits, %llu misses, %llu added, %llu writes\n",
                   (unsigned long long)v[0], (unsigned long long)v[2], (unsigned long long)v[3], v[9] / 1e9, (unsigned long long)v[4],
                   (unsigned long long)v[5], (unsigned long long)v[6], v[7] / 1e6, v[8] / 1e6, (unsigned long long)v[10],
                   (unsigned long long)v[11], (unsigned long long)v[12], (unsigned long long)v[13]);
    }
}
static void watch_atexit(void) {
    const char *s = getenv("SPORE_METAL_WATCH");
    if ((s && *s) || getenv("SPORE_COM_STATS")) mt_watch_tick(1);
}
static void stall_report(int fd) {
    dprintf(fd, "d3d9metal: device %s; frames %llu; last watch line: %s\n", g_lost ? "LOST" : "not reported lost",
            (unsigned long long)g_mst.frames, g_watch_line[0] ? g_watch_line : "(none; set SPORE_METAL_WATCH)");
    ring_dump(fd, 48);
}
extern "C" void mt_device_lost(mt_dev *d, const char *what) { /* from a command buffer's completion handler */
    (void)d;
    if (__atomic_exchange_n(&g_lost, 1, __ATOMIC_ACQ_REL)) return;
    MT_LOG("%s after %llu frames\n", what, (unsigned long long)g_mst.frames);
    if (p_stall_fault) p_stall_fault(what);
    else MT_LOG("no stall watchdog in this binary: continuing; TestCooperativeLevel now reports DEVICELOST\n");
}
extern "C" void mt_stall_beat(void) {
    if (p_stall_beat) p_stall_beat();
}

/* ---- entry ----------------------------------------------------------------------------------------------- */
extern "C" uint32_t d3d9metal_create(sg_ctx *ctx, uint8_t *mem, uint32_t sdk) {
    static int inited;
    (void)ctx;
    sg_com_init(mem);
    if (!inited) {
        g_mt_trace = getenv("SPORE_METAL_VERBOSE") ? atoi(getenv("SPORE_METAL_VERBOSE")) : 0;
        if (getenv("SPORE_METAL_VERBOSE") && !g_mt_trace) g_mt_trace = 1;
        mt_verbose = g_mt_trace;
        id<MTLDevice> probe = MTLCreateSystemDefaultDevice();
        if (!probe) {
            MT_LOG("no Metal device\n");
            return 0;
        }
        MT_LOG("Metal device %s (unified memory %d, BC %d)\n", probe.name.UTF8String, (int)probe.hasUnifiedMemory,
               (int)probe.supportsBCTextureCompression);
        [probe release];
        *(void **)&p_stall_beat = dlsym(RTLD_DEFAULT, "sg_core_stall_beat");
        *(void **)&p_stall_fault = dlsym(RTLD_DEFAULT, "sg_core_stall_fault");
        *(void **)&p_stall_reporter = dlsym(RTLD_DEFAULT, "sg_core_stall_set_reporter");
        ifaces_init();
        ring_install();
        if (p_stall_reporter) p_stall_reporter(stall_report);
        atexit(watch_atexit);
        inited = 1;
        g_loaded = 1;
    }
    if (sdk != 32 && sdk != (32u | 0x80000000u)) MT_LOG("Direct3DCreate9(SDK version %u): expected 32; continuing\n", sdk);
    pthread_mutex_lock(d3d9null_device_lock());
    mt_obj *o = mt_new(MT_K_D3D, NULL, NULL);
    pthread_mutex_unlock(d3d9null_device_lock());
    if (!o) return 0;
    MT_LOG("Direct3DCreate9(%u) = 0x%08x (Metal; NVIDIA GeForce 8800 GTX profile, INST hidden)\n", sdk, mt_guest_of(o));
    return mt_guest_of(o);
}
