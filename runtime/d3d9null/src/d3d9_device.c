/* d3d9_device.c -- the null IDirect3DDevice9 and its resources. Every method the game calls
 * succeeds and draws nothing; state is stored and returned; resources have real sizes, pitches and
 * descs, and Lock hands out guest memory (allocated on the first lock). */
#include <stdio.h>
#include <time.h>

#include "d3d9_int.h"

#define ARG(i) SG_COM_ARG(c, i)
#define MEM c->mem
#define M(S, Name, fn) {#Name, S_##S##_##Name, 4 + 4 * A_##S##_##Name, fn}

/* ---- objects --------------------------------------------------------------------------------- */
#define MAX_LEVELS 16
typedef struct dev dev;
typedef struct res res;
struct res {
    sg_com_obj com;
    dev *dev;
    uint32_t rtype, usage, format, pool, fvf, ms, msq;
    uint32_t w, h, d, levels, faces;
    uint32_t lvl_off[MAX_LEVELS], lvl_pitch[MAX_LEVELS], lvl_slice[MAX_LEVELS];
    uint32_t face_bytes, bytes;
    uint32_t mem;            /* guest backing (lazy) */
    res *parent;             /* container of a level surface / volume */
    uint32_t face, level;
    res **subs;              /* container: faces * levels children, created on demand */
    uint32_t lod, priority, filter;
    uint32_t locks;
    uint8_t *blob;           /* shader bytecode, declaration elements */
    uint32_t blob_len, count;
    uint32_t qtype;
    uint32_t pp[D3DPP_DWORDS]; /* swap chain */
    res *bb;                 /* swap chain back buffer */
    sg_com_iface *iface;
};

struct dev {
    sg_com_obj com;
    d3d9_obj *d3d;
    int alive;
    uint32_t adapter, devtype, focus, flags;
    uint32_t pp[D3DPP_DWORDS];
    res *sc, *autods;
    uint32_t rt[4], ds;
    uint32_t rs[256];
    uint32_t tss[8][33];
    uint32_t ss[21][14];
    uint32_t tex[21];
    struct { uint32_t vb, off, stride, freq; } streams[16];
    uint32_t ib, vdecl, vs, ps, fvf;
    uint32_t vp[6], scissor[4];
    float transforms[512][16];
    float vsf[256 * 4], psf[224 * 4];
    int32_t vsi[16 * 4], psi[16 * 4];
    uint32_t vsb[16], psb[16];
    uint8_t gamma[1536];
    float clip[6][4];
    uint32_t material[17];
    uint32_t lights[16][26], light_set[16], light_on[16];
    uint32_t clip_status[6];
    uint32_t palette, swvp, cursor;
    float npatch;
    int in_scene, recording;
    uint64_t frames;
};

static const sg_com_class res_class, dev_class;
static void dev_reset_state(dev *d);

static uint32_t guest_of(void *o) { return o ? sg_com_obj_guest(&((res *)o)->com, NULL) : 0; }
/* Writes an object to an out pointer with a reference, or NULL if it is gone. */
static void out_obj(sg_com_call *c, uint32_t out, uint32_t guest) {
    sg_com_obj *o = guest ? sg_com_lookup(MEM, guest) : NULL;
    if (!out) return;
    if (o) sg_com_obj_addref(o);
    sg_st32(MEM, out, o ? guest : 0);
}
static uint32_t dev_guest(dev *d) { return d && d->alive ? sg_com_obj_guest(&d->com, NULL) : 0; }

static res *res_new(dev *d, sg_com_iface *iface, uint32_t rtype) {
    res *r = calloc(1, sizeof *r);
    if (!r) return NULL;
    if (sg_com_obj_init(&r->com, &res_class)) {
        free(r);
        return NULL;
    }
    r->dev = d;
    r->rtype = rtype;
    r->iface = iface;
    r->filter = 2; /* D3DTEXF_LINEAR */
    if (!sg_com_obj_expose(&r->com, iface)) {
        sg_com_obj_retire(&r->com);
        free(r);
        return NULL;
    }
    d3d9_stats.creates++;
    d3d9_stats.live_resources++;
    return r;
}
static void res_free(res *r) {
    if (r->subs) {
        for (uint32_t i = 0; i < r->faces * r->levels; i++)
            if (r->subs[i]) res_free(r->subs[i]);
        free(r->subs);
    }
    if (r->bb) res_free(r->bb);
    if (r->mem && !r->parent) {
        sg_com_gfree(r->mem);
        d3d9_stats.resource_bytes -= r->bytes;
    }
    free(r->blob);
    if (r->com.handle) sg_com_obj_retire(&r->com);
    d3d9_stats.live_resources--;
    free(r);
}
static void res_destroy(sg_com_obj *o) { res_free((res *)o); }
static const sg_com_class res_class = {"IDirect3DResource9", res_destroy, NULL};

/* Level layout: sizes, pitches (4-aligned rows; block rows for DXT), 16-aligned level offsets.
 * Exported for the Metal back end's guest-memory Lock shadows. */
int d3d9null_layout(uint32_t fmt, uint32_t w, uint32_t h, uint32_t d, uint32_t levels, uint32_t faces,
                    uint32_t *lvl_off, uint32_t *lvl_pitch, uint32_t *lvl_slice, uint32_t *face_bytes, uint32_t *bytes) {
    unsigned block, bits = d3d9_fmt_bits(fmt, &block);
    uint64_t off = 0;
    if (!bits) bits = 32; /* unknown format: assume 32 bpp so Lock still gives a sane pitch */
    for (uint32_t l = 0; l < levels; l++) {
        uint32_t lw = w >> l ? w >> l : 1, lh = h >> l ? h >> l : 1, ld = d >> l ? d >> l : 1;
        uint32_t pitch, rows;
        if (block) {
            pitch = ((lw + 3) / 4) * block;
            rows = (lh + 3) / 4;
        } else {
            pitch = (uint32_t)(((uint64_t)lw * bits / 8 + 3) & ~3ull);
            rows = lh;
        }
        lvl_off[l] = (uint32_t)off;
        lvl_pitch[l] = pitch;
        lvl_slice[l] = pitch * rows;
        off += ((uint64_t)pitch * rows * ld + 15) & ~15ull;
        if (off > 0x40000000ull) return -1;
    }
    *face_bytes = (uint32_t)off;
    if ((uint64_t)off * faces > 0x40000000ull) return -1;
    *bytes = (uint32_t)off * faces;
    return 0;
}
static int res_layout(res *r) {
    return d3d9null_layout(r->format, r->w, r->h, r->d, r->levels, r->faces, r->lvl_off, r->lvl_pitch, r->lvl_slice,
                           &r->face_bytes, &r->bytes);
}
static uint32_t full_chain(uint32_t w, uint32_t h, uint32_t d) {
    uint32_t m = w > h ? w : h, n = 1;
    if (d > m) m = d;
    while (m > 1) m >>= 1, n++;
    return n;
}
/* Guest backing of a container (or standalone surface), allocated on first use. */
static uint32_t res_backing(res *r) {
    res *top = r->parent ? r->parent : r;
    if (!top->mem && top->bytes) {
        top->mem = sg_com_galloc(top->bytes);
        if (top->mem) d3d9_stats.resource_bytes += top->bytes;
    }
    return top->mem;
}

static res *texture_new(dev *d, sg_com_iface *iface, uint32_t rtype, uint32_t w, uint32_t h, uint32_t depth,
                        uint32_t levels, uint32_t usage, uint32_t fmt, uint32_t pool, uint32_t faces, uint32_t *err) {
    res *r;
    uint32_t maxl = full_chain(w, h, depth);
    *err = D3DERR_INVALIDCALL;
    if (!w || !h || !depth || w > 16384 || h > 16384 || depth > 2048) return NULL;
    if (usage & D3DUSAGE_AUTOGENMIPMAP) {
        if (levels > 1) return NULL;
        levels = 1;
    }
    if (!levels || levels > maxl) levels = maxl;
    if (levels > MAX_LEVELS) levels = MAX_LEVELS;
    if (pool > D3DPOOL_SCRATCH) return NULL;
    if ((usage & (D3DUSAGE_RENDERTARGET | D3DUSAGE_DEPTHSTENCIL)) && pool != D3DPOOL_DEFAULT) return NULL;
    if (pool != D3DPOOL_SCRATCH && rtype != D3DRTYPE_SURFACE &&
        d3d9_check_format(D3DFMT_X8R8G8B8, usage & ~D3DUSAGE_AUTOGENMIPMAP, rtype, fmt) == D3DERR_NOTAVAILABLE) {
        *err = D3DERR_NOTAVAILABLE;
        return NULL;
    }
    r = res_new(d, iface, rtype);
    if (!r) {
        *err = SG_E_OUTOFMEMORY;
        return NULL;
    }
    r->w = w, r->h = h, r->d = depth, r->levels = levels, r->faces = faces;
    r->usage = usage, r->format = fmt, r->pool = pool;
    if (res_layout(r)) {
        res_free(r);
        *err = D3DERR_OUTOFVIDEOMEMORY;
        return NULL;
    }
    *err = D3D_OK;
    return r;
}

/* Child surface (or volume) for (face, level) of a container; its references count on the container. */
static res *sub_of(res *t, uint32_t face, uint32_t level) {
    uint32_t i = face * t->levels + level;
    res *s;
    if (!t->subs) t->subs = calloc((size_t)t->faces * t->levels, sizeof *t->subs);
    if (!t->subs) return NULL;
    if (t->subs[i]) return t->subs[i];
    s = res_new(t->dev, t->rtype == D3DRTYPE_VOLUMETEXTURE ? &d3d9_iface_volume : &d3d9_iface_surface,
                t->rtype == D3DRTYPE_VOLUMETEXTURE ? D3DRTYPE_VOLUME : D3DRTYPE_SURFACE);
    if (!s) return NULL;
    s->com.refs = 0;
    s->com.owner = &t->com;
    s->parent = t;
    s->face = face, s->level = level;
    s->format = t->format, s->usage = t->usage, s->pool = t->pool;
    s->w = t->w >> level ? t->w >> level : 1;
    s->h = t->h >> level ? t->h >> level : 1;
    s->d = t->d >> level ? t->d >> level : 1;
    s->levels = 1, s->faces = 1;
    t->subs[i] = s;
    return s;
}

/* The guest address of (x, y, z) in level `l` of face `f`; 0 if the backing cannot be allocated. */
static uint32_t texel_addr(res *t, uint32_t f, uint32_t l, uint32_t x, uint32_t y, uint32_t z) {
    unsigned block, bits = d3d9_fmt_bits(t->format, &block);
    uint32_t base = res_backing(t);
    if (!base) return 0;
    if (!bits) bits = 32;
    base += f * t->face_bytes + t->lvl_off[l] + z * t->lvl_slice[l];
    if (block) return base + (y / 4) * t->lvl_pitch[l] + (x / 4) * block;
    return base + y * t->lvl_pitch[l] + x * (bits / 8);
}
/* D3DLOCKED_RECT {Pitch, pBits} for (face, level) of t, honouring an optional RECT. */
static uint32_t lock_rect(sg_com_call *c, res *t, uint32_t face, uint32_t level, uint32_t plr, uint32_t prect) {
    uint32_t x = 0, y = 0, a;
    if (!plr || level >= t->levels || face >= t->faces) return D3DERR_INVALIDCALL;
    if (prect) {
        x = sg_ld32(MEM, prect);
        y = sg_ld32(MEM, prect + 4);
    }
    a = texel_addr(t, face, level, x, y, 0);
    if (!a) {
        sg_st32(MEM, plr, 0);
        sg_st32(MEM, plr + 4, 0);
        return D3DERR_OUTOFVIDEOMEMORY;
    }
    sg_st32(MEM, plr, t->lvl_pitch[level]);
    sg_st32(MEM, plr + 4, a);
    t->locks++;
    d3d9_stats.locks++;
    return D3D_OK;
}
static uint32_t lock_box(sg_com_call *c, res *t, uint32_t level, uint32_t plb, uint32_t pbox) {
    uint32_t x = 0, y = 0, z = 0, a;
    if (!plb || level >= t->levels) return D3DERR_INVALIDCALL;
    if (pbox) {
        x = sg_ld32(MEM, pbox);
        y = sg_ld32(MEM, pbox + 4);
        z = sg_ld32(MEM, pbox + 16);
    }
    a = texel_addr(t, 0, level, x, y, z);
    if (!a) return D3DERR_OUTOFVIDEOMEMORY;
    sg_st32(MEM, plb, t->lvl_pitch[level]);
    sg_st32(MEM, plb + 4, t->lvl_slice[level]);
    sg_st32(MEM, plb + 8, a);
    t->locks++;
    d3d9_stats.locks++;
    return D3D_OK;
}
static void put_surface_desc(sg_com_call *c, uint32_t p, uint32_t fmt, uint32_t usage, uint32_t pool, uint32_t ms,
                             uint32_t msq, uint32_t w, uint32_t h) {
    uint32_t v[8] = {fmt, D3DRTYPE_SURFACE, usage, pool, ms, msq, w, h};
    for (int i = 0; i < 8; i++) sg_st32(MEM, p + 4u * i, v[i]);
}

/* ---- IDirect3DResource9 / IDirect3DBaseTexture9 common methods ------------------------------- */
#define R ((res *)c->obj)
static uint32_t r_get_device(sg_com_call *c) {
    uint32_t g = dev_guest(R->dev);
    if (!ARG(0)) return D3DERR_INVALIDCALL;
    out_obj(c, ARG(0), g);
    return g ? D3D_OK : D3DERR_INVALIDCALL;
}
static uint32_t r_set_private(sg_com_call *c) { return D3D_OK; }
static uint32_t r_get_private(sg_com_call *c) { return D3DERR_NOTFOUND; }
static uint32_t r_free_private(sg_com_call *c) { return D3DERR_NOTFOUND; }
static uint32_t r_set_priority(sg_com_call *c) { uint32_t o = R->priority; R->priority = ARG(0); return o; }
static uint32_t r_get_priority(sg_com_call *c) { return R->priority; }
static uint32_t r_preload(sg_com_call *c) { return 0; }
static uint32_t r_get_type(sg_com_call *c) { return R->rtype; }
static uint32_t t_set_lod(sg_com_call *c) {
    uint32_t o = R->lod;
    if (R->pool == D3DPOOL_MANAGED) R->lod = ARG(0) < R->levels ? ARG(0) : R->levels - 1;
    return o;
}
static uint32_t t_get_lod(sg_com_call *c) { return R->lod; }
static uint32_t t_level_count(sg_com_call *c) { return R->levels; }
static uint32_t t_set_filter(sg_com_call *c) { R->filter = ARG(0); return D3D_OK; }
static uint32_t t_get_filter(sg_com_call *c) { return R->filter; }
static uint32_t t_gen_mips(sg_com_call *c) { return 0; }
#define RESOURCE_METHODS(S)                                                                                     \
    SG_COM_IUNKNOWN, M(S, GetDevice, r_get_device), M(S, SetPrivateData, r_set_private),                       \
        M(S, GetPrivateData, r_get_private), M(S, FreePrivateData, r_free_private),                            \
        M(S, SetPriority, r_set_priority), M(S, GetPriority, r_get_priority), M(S, PreLoad, r_preload),        \
        M(S, GetType, r_get_type)
#define BASETEX_METHODS(S)                                                                                      \
    RESOURCE_METHODS(S), M(S, SetLOD, t_set_lod), M(S, GetLOD, t_get_lod), M(S, GetLevelCount, t_level_count), \
        M(S, SetAutoGenFilterType, t_set_filter), M(S, GetAutoGenFilterType, t_get_filter),                    \
        M(S, GenerateMipSubLevels, t_gen_mips)

/* ---- IDirect3DTexture9 ----------------------------------------------------------------------- */
static uint32_t tex_level_desc(sg_com_call *c) { /* (level, D3DSURFACE_DESC*) */
    uint32_t l = ARG(0);
    if (l >= R->levels || !ARG(1)) return D3DERR_INVALIDCALL;
    put_surface_desc(c, ARG(1), R->format, R->usage, R->pool, 0, 0, R->w >> l ? R->w >> l : 1, R->h >> l ? R->h >> l : 1);
    return D3D_OK;
}
static uint32_t tex_surface_level(sg_com_call *c) { /* (level, IDirect3DSurface9**) */
    res *s;
    if (ARG(0) >= R->levels || !ARG(1)) return D3DERR_INVALIDCALL;
    s = sub_of(R, 0, ARG(0));
    if (!s) return SG_E_OUTOFMEMORY;
    sg_com_obj_addref(&s->com);
    sg_st32(MEM, ARG(1), guest_of(s));
    return D3D_OK;
}
static uint32_t tex_lock(sg_com_call *c) { return lock_rect(c, R, 0, ARG(0), ARG(1), ARG(2)); } /* (level, lr, rect, flags) */
static uint32_t tex_unlock(sg_com_call *c) { return ARG(0) < R->levels ? D3D_OK : D3DERR_INVALIDCALL; }
static uint32_t tex_dirty(sg_com_call *c) { return D3D_OK; }
static const sg_com_method texture_methods[] = {
    BASETEX_METHODS(Texture), M(Texture, GetLevelDesc, tex_level_desc), M(Texture, GetSurfaceLevel, tex_surface_level),
    M(Texture, LockRect, tex_lock), M(Texture, UnlockRect, tex_unlock), M(Texture, AddDirtyRect, tex_dirty)};

/* ---- IDirect3DCubeTexture9 ------------------------------------------------------------------- */
static uint32_t cube_surface(sg_com_call *c) { /* (face, level, IDirect3DSurface9**) */
    res *s;
    if (ARG(0) >= 6 || ARG(1) >= R->levels || !ARG(2)) return D3DERR_INVALIDCALL;
    s = sub_of(R, ARG(0), ARG(1));
    if (!s) return SG_E_OUTOFMEMORY;
    sg_com_obj_addref(&s->com);
    sg_st32(MEM, ARG(2), guest_of(s));
    return D3D_OK;
}
static uint32_t cube_lock(sg_com_call *c) { return lock_rect(c, R, ARG(0), ARG(1), ARG(2), ARG(3)); } /* (face, level, lr, rect, flags) */
static uint32_t cube_unlock(sg_com_call *c) { return ARG(0) < 6 && ARG(1) < R->levels ? D3D_OK : D3DERR_INVALIDCALL; }
static const sg_com_method cube_methods[] = {
    BASETEX_METHODS(CubeTexture), M(CubeTexture, GetLevelDesc, tex_level_desc),
    M(CubeTexture, GetCubeMapSurface, cube_surface), M(CubeTexture, LockRect, cube_lock),
    M(CubeTexture, UnlockRect, cube_unlock), M(CubeTexture, AddDirtyRect, tex_dirty)};

/* ---- IDirect3DVolumeTexture9 / IDirect3DVolume9 ---------------------------------------------- */
static void put_volume_desc(sg_com_call *c, uint32_t p, res *t, uint32_t l) {
    uint32_t v[7] = {t->format, D3DRTYPE_VOLUME, t->usage, t->pool, t->w >> l ? t->w >> l : 1, t->h >> l ? t->h >> l : 1,
                     t->d >> l ? t->d >> l : 1};
    for (int i = 0; i < 7; i++) sg_st32(MEM, p + 4u * i, v[i]);
}
static uint32_t vt_level_desc(sg_com_call *c) {
    if (ARG(0) >= R->levels || !ARG(1)) return D3DERR_INVALIDCALL;
    put_volume_desc(c, ARG(1), R, ARG(0));
    return D3D_OK;
}
static uint32_t vt_volume_level(sg_com_call *c) {
    res *s;
    if (ARG(0) >= R->levels || !ARG(1)) return D3DERR_INVALIDCALL;
    s = sub_of(R, 0, ARG(0));
    if (!s) return SG_E_OUTOFMEMORY;
    sg_com_obj_addref(&s->com);
    sg_st32(MEM, ARG(1), guest_of(s));
    return D3D_OK;
}
static uint32_t vt_lock(sg_com_call *c) { return lock_box(c, R, ARG(0), ARG(1), ARG(2)); } /* (level, lb, box, flags) */
static const sg_com_method volumetex_methods[] = {
    BASETEX_METHODS(VolumeTexture), M(VolumeTexture, GetLevelDesc, vt_level_desc),
    M(VolumeTexture, GetVolumeLevel, vt_volume_level), M(VolumeTexture, LockBox, vt_lock),
    M(VolumeTexture, UnlockBox, tex_unlock), M(VolumeTexture, AddDirtyBox, tex_dirty)};

static uint32_t sub_container(sg_com_call *c) { /* (REFIID, void**) */
    uint32_t g = 0;
    sg_guid iid;
    if (!ARG(0) || !ARG(1)) return D3DERR_INVALIDCALL;
    iid = sg_guid_load(MEM, ARG(0));
    if (R->parent && (sg_guid_eq(&iid, &R->parent->iface->iid) || sg_guid_eq(&iid, &SG_IID_IUnknown)))
        g = guest_of(R->parent);
    else if (!R->parent && R->dev && sg_guid_eq(&iid, &d3d9_iface_device.iid))
        g = dev_guest(R->dev);
    sg_st32(MEM, ARG(1), 0);
    if (!g) return SG_E_NOINTERFACE;
    out_obj(c, ARG(1), g);
    return D3D_OK;
}
static uint32_t vol_desc(sg_com_call *c) {
    if (!ARG(0)) return D3DERR_INVALIDCALL;
    put_volume_desc(c, ARG(0), R->parent, R->level);
    return D3D_OK;
}
static uint32_t vol_lock(sg_com_call *c) { return lock_box(c, R->parent, R->level, ARG(0), ARG(1)); }
static uint32_t ok0(sg_com_call *c) { return D3D_OK; }
static const sg_com_method volume_methods[] = {
    SG_COM_IUNKNOWN, M(Volume, GetDevice, r_get_device), M(Volume, SetPrivateData, r_set_private),
    M(Volume, GetPrivateData, r_get_private), M(Volume, FreePrivateData, r_free_private),
    M(Volume, GetContainer, sub_container), M(Volume, GetDesc, vol_desc), M(Volume, LockBox, vol_lock),
    M(Volume, UnlockBox, ok0)};

/* ---- IDirect3DSurface9 ----------------------------------------------------------------------- */
static uint32_t surf_desc(sg_com_call *c) {
    if (!ARG(0)) return D3DERR_INVALIDCALL;
    put_surface_desc(c, ARG(0), R->format, R->usage, R->pool, R->ms, R->msq, R->w, R->h);
    return D3D_OK;
}
static uint32_t surf_lock(sg_com_call *c) { /* (lr, rect, flags) */
    if (R->parent) return lock_rect(c, R->parent, R->face, R->level, ARG(0), ARG(1));
    return lock_rect(c, R, 0, 0, ARG(0), ARG(1));
}
static uint32_t surf_get_dc(sg_com_call *c) {
    if (ARG(0)) sg_st32(MEM, ARG(0), 0);
    return D3DERR_INVALIDCALL;
}
static const sg_com_method surface_methods[] = {
    RESOURCE_METHODS(Surface), M(Surface, GetContainer, sub_container), M(Surface, GetDesc, surf_desc),
    M(Surface, LockRect, surf_lock), M(Surface, UnlockRect, ok0), M(Surface, GetDC, surf_get_dc),
    M(Surface, ReleaseDC, surf_get_dc)};

static res *surface_new(dev *d, uint32_t w, uint32_t h, uint32_t fmt, uint32_t usage, uint32_t pool, uint32_t ms,
                        uint32_t msq, uint32_t *err) {
    res *s = texture_new(d, &d3d9_iface_surface, D3DRTYPE_SURFACE, w, h, 1, 1, usage, fmt, pool, 1, err);
    if (s) s->ms = ms, s->msq = msq;
    return s;
}

/* ---- vertex and index buffers ---------------------------------------------------------------- */
static uint32_t buf_lock(sg_com_call *c) { /* (offset, size, void **ppb, flags) */
    uint32_t off = ARG(0), size = ARG(1), base;
    if (!ARG(2)) return D3DERR_INVALIDCALL;
    if (off > R->w || (size && size > R->w - off)) {
        sg_st32(MEM, ARG(2), 0);
        return D3DERR_INVALIDCALL;
    }
    base = res_backing(R);
    if (!base) {
        sg_st32(MEM, ARG(2), 0);
        return D3DERR_OUTOFVIDEOMEMORY;
    }
    sg_st32(MEM, ARG(2), base + off);
    R->locks++;
    d3d9_stats.locks++;
    return D3D_OK;
}
static uint32_t vb_desc(sg_com_call *c) {
    uint32_t v[6] = {D3DFMT_VERTEXDATA, D3DRTYPE_VERTEXBUFFER, R->usage, R->pool, R->w, R->fvf};
    if (!ARG(0)) return D3DERR_INVALIDCALL;
    for (int i = 0; i < 6; i++) sg_st32(MEM, ARG(0) + 4u * i, v[i]);
    return D3D_OK;
}
static uint32_t ib_desc(sg_com_call *c) {
    uint32_t v[5] = {R->format, D3DRTYPE_INDEXBUFFER, R->usage, R->pool, R->w};
    if (!ARG(0)) return D3DERR_INVALIDCALL;
    for (int i = 0; i < 5; i++) sg_st32(MEM, ARG(0) + 4u * i, v[i]);
    return D3D_OK;
}
static const sg_com_method vb_methods[] = {RESOURCE_METHODS(VertexBuffer), M(VertexBuffer, Lock, buf_lock),
                                           M(VertexBuffer, Unlock, ok0), M(VertexBuffer, GetDesc, vb_desc)};
static const sg_com_method ib_methods[] = {RESOURCE_METHODS(IndexBuffer), M(IndexBuffer, Lock, buf_lock),
                                           M(IndexBuffer, Unlock, ok0), M(IndexBuffer, GetDesc, ib_desc)};

static res *buffer_new(dev *d, sg_com_iface *iface, uint32_t rtype, uint32_t len, uint32_t usage, uint32_t fmt_or_fvf,
                       uint32_t pool, uint32_t *err) {
    res *b;
    *err = D3DERR_INVALIDCALL;
    if (!len || len > 0x10000000u || pool > D3DPOOL_SCRATCH) return NULL;
    if (rtype == D3DRTYPE_INDEXBUFFER && fmt_or_fvf != D3DFMT_INDEX16 && fmt_or_fvf != D3DFMT_INDEX32) return NULL;
    b = res_new(d, iface, rtype);
    if (!b) {
        *err = SG_E_OUTOFMEMORY;
        return NULL;
    }
    b->w = len, b->h = b->d = b->levels = b->faces = 1;
    b->usage = usage, b->pool = pool;
    if (rtype == D3DRTYPE_INDEXBUFFER) b->format = fmt_or_fvf;
    else b->fvf = fmt_or_fvf, b->format = D3DFMT_VERTEXDATA;
    b->bytes = (len + 15) & ~15u;
    *err = D3D_OK;
    return b;
}

/* ---- declarations, shaders, queries, state blocks, swap chains -------------------------------- */
static uint32_t blob_get(sg_com_call *c) { /* GetDeclaration(elems, UINT *num) / GetFunction(data, UINT *size) */
    uint32_t data = ARG(0), np = ARG(1);
    if (!np) return D3DERR_INVALIDCALL;
    if (R->count) { /* declaration: count elements of 8 bytes */
        if (data) memcpy(MEM + data, R->blob, R->count * 8u);
        sg_st32(MEM, np, R->count);
        return D3D_OK;
    }
    if (data) {
        if (sg_ld32(MEM, np) < R->blob_len) return D3DERR_INVALIDCALL;
        memcpy(MEM + data, R->blob, R->blob_len);
    }
    sg_st32(MEM, np, R->blob_len);
    return D3D_OK;
}
static const sg_com_method vdecl_methods[] = {SG_COM_IUNKNOWN, M(VertexDeclaration, GetDevice, r_get_device),
                                              M(VertexDeclaration, GetDeclaration, blob_get)};
static const sg_com_method vs_methods[] = {SG_COM_IUNKNOWN, M(VertexShader, GetDevice, r_get_device),
                                           M(VertexShader, GetFunction, blob_get)};
static const sg_com_method ps_methods[] = {SG_COM_IUNKNOWN, M(PixelShader, GetDevice, r_get_device),
                                           M(PixelShader, GetFunction, blob_get)};

static uint32_t query_size(uint32_t t) {
    switch (t) {
    case D3DQUERYTYPE_EVENT: case D3DQUERYTYPE_OCCLUSION: case D3DQUERYTYPE_TIMESTAMPDISJOINT: return 4;
    case D3DQUERYTYPE_TIMESTAMP: case D3DQUERYTYPE_TIMESTAMPFREQ: return 8;
    default: return 0;
    }
}
static uint32_t q_type(sg_com_call *c) { return R->qtype; }
static uint32_t q_size(sg_com_call *c) { return query_size(R->qtype); }
static uint32_t q_issue(sg_com_call *c) { R->locks++; return D3D_OK; }
static uint32_t q_get_data(sg_com_call *c) { /* (data, size, flags): everything has completed */
    uint32_t p = ARG(0), n = ARG(1), need = query_size(R->qtype);
    uint64_t v = 0;
    struct timespec ts;
    if (!p || !n) return D3D_OK;
    if (n < need) return D3DERR_INVALIDCALL;
    switch (R->qtype) {
    case D3DQUERYTYPE_EVENT: v = 1; break;                 /* TRUE: the GPU reached the event */
    case D3DQUERYTYPE_OCCLUSION: v = 0; break;             /* nothing was drawn */
    case D3DQUERYTYPE_TIMESTAMPDISJOINT: v = 0; break;
    case D3DQUERYTYPE_TIMESTAMPFREQ: v = 1000000000ull; break;
    case D3DQUERYTYPE_TIMESTAMP:
        clock_gettime(CLOCK_MONOTONIC, &ts);
        v = (uint64_t)ts.tv_sec * 1000000000ull + (uint64_t)ts.tv_nsec;
        break;
    }
    memcpy(MEM + p, &v, need);
    return D3D_OK;
}
static const sg_com_method query_methods[] = {SG_COM_IUNKNOWN, M(Query, GetDevice, r_get_device), M(Query, GetType, q_type),
                                              M(Query, GetDataSize, q_size), M(Query, Issue, q_issue),
                                              M(Query, GetData, q_get_data)};
static const sg_com_method stateblock_methods[] = {SG_COM_IUNKNOWN, M(StateBlock, GetDevice, r_get_device),
                                                   M(StateBlock, Capture, ok0), M(StateBlock, Apply, ok0)};

static void dev_present(dev *d, uint32_t hwnd) {
    d->frames++;
    d3d9_stats.frames++;
    sg_host_present(hwnd ? hwnd : d->pp[PP_HWND] ? d->pp[PP_HWND] : d->focus, d->frames);
}
static uint32_t sc_present(sg_com_call *c) { /* (src, dst, hwnd, dirty, flags) */
    if (R->dev) dev_present(R->dev, ARG(2) ? ARG(2) : R->pp[PP_HWND]);
    return D3D_OK;
}
static uint32_t sc_back_buffer(sg_com_call *c) { /* (index, type, IDirect3DSurface9**) */
    if (!ARG(2)) return D3DERR_INVALIDCALL;
    if (ARG(0) != 0 || !R->bb) {
        sg_st32(MEM, ARG(2), 0);
        return D3DERR_INVALIDCALL;
    }
    sg_com_obj_addref(&R->bb->com);
    sg_st32(MEM, ARG(2), guest_of(R->bb));
    return D3D_OK;
}
static uint32_t sc_raster(sg_com_call *c) {
    if (!ARG(0)) return D3DERR_INVALIDCALL;
    sg_st32(MEM, ARG(0), 0);
    sg_st32(MEM, ARG(0) + 4, 0);
    return D3D_OK;
}
static uint32_t sc_display_mode(sg_com_call *c) {
    uint32_t w, h, hz;
    if (!ARG(0)) return D3DERR_INVALIDCALL;
    d3d9_current_mode(&w, &h, &hz);
    if (!R->pp[PP_WINDOWED]) w = R->pp[PP_W], h = R->pp[PP_H], hz = R->pp[PP_REFRESH] ? R->pp[PP_REFRESH] : hz;
    sg_st32(MEM, ARG(0), w);
    sg_st32(MEM, ARG(0) + 4, h);
    sg_st32(MEM, ARG(0) + 8, hz);
    sg_st32(MEM, ARG(0) + 12, D3DFMT_X8R8G8B8);
    return D3D_OK;
}
static uint32_t sc_pp(sg_com_call *c) {
    if (!ARG(0)) return D3DERR_INVALIDCALL;
    for (int i = 0; i < D3DPP_DWORDS; i++) sg_st32(MEM, ARG(0) + 4u * i, R->pp[i]);
    return D3D_OK;
}
static const sg_com_method swapchain_methods[] = {
    SG_COM_IUNKNOWN, M(SwapChain, Present, sc_present), M(SwapChain, GetFrontBufferData, ok0),
    M(SwapChain, GetBackBuffer, sc_back_buffer), M(SwapChain, GetRasterStatus, sc_raster),
    M(SwapChain, GetDisplayMode, sc_display_mode), M(SwapChain, GetDevice, r_get_device),
    M(SwapChain, GetPresentParameters, sc_pp)};

/* Fills in defaults the way D3D9 does and writes them back to the guest's struct. */
static uint32_t read_pp(sg_com_call *c, uint32_t p, uint32_t focus, uint32_t pp[D3DPP_DWORDS]) {
    uint32_t w, h, hz;
    for (int i = 0; i < D3DPP_DWORDS; i++) pp[i] = sg_ld32(MEM, p + 4u * i);
    d3d9_current_mode(&w, &h, &hz);
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
    for (int i = 0; i < D3DPP_DWORDS; i++) sg_st32(MEM, p + 4u * i, pp[i]);
    return D3D_OK;
}
static res *swapchain_new(dev *d, const uint32_t pp[D3DPP_DWORDS], uint32_t *err) {
    res *s = res_new(d, &d3d9_iface_swapchain, 0);
    if (!s) {
        *err = SG_E_OUTOFMEMORY;
        return NULL;
    }
    memcpy(s->pp, pp, sizeof s->pp);
    s->bb = surface_new(d, pp[PP_W], pp[PP_H], pp[PP_FMT], D3DUSAGE_RENDERTARGET, D3DPOOL_DEFAULT, pp[PP_MS], pp[PP_MSQ], err);
    if (!s->bb) {
        res_free(s);
        return NULL;
    }
    s->bb->com.refs = 0;
    s->bb->com.owner = &s->com;
    return s;
}

/* ---- IDirect3DDevice9 -------------------------------------------------------------------------- */
#define D ((dev *)c->obj)
static uint32_t dv_test_coop(sg_com_call *c) { return D3D_OK; }
static uint32_t dv_texmem(sg_com_call *c) { return 768u << 20; } /* 768 MB, a GeForce 8800 GTX */
static uint32_t dv_get_d3d(sg_com_call *c) {
    if (!ARG(0)) return D3DERR_INVALIDCALL;
    out_obj(c, ARG(0), sg_com_obj_guest(&D->d3d->com, NULL));
    return D3D_OK;
}
static uint32_t dv_caps(sg_com_call *c) {
    if (!ARG(0)) return D3DERR_INVALIDCALL;
    d3d9_fill_caps(MEM + ARG(0), D->devtype);
    return D3D_OK;
}
static uint32_t dv_display_mode(sg_com_call *c) { /* (swapchain, D3DDISPLAYMODE*) */
    uint32_t w, h, hz;
    if (ARG(0) != 0 || !ARG(1)) return D3DERR_INVALIDCALL;
    d3d9_current_mode(&w, &h, &hz);
    if (!D->pp[PP_WINDOWED]) w = D->pp[PP_W], h = D->pp[PP_H], hz = D->pp[PP_REFRESH] ? D->pp[PP_REFRESH] : hz;
    sg_st32(MEM, ARG(1), w);
    sg_st32(MEM, ARG(1) + 4, h);
    sg_st32(MEM, ARG(1) + 8, hz);
    sg_st32(MEM, ARG(1) + 12, D3DFMT_X8R8G8B8);
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
static uint32_t dv_show_cursor(sg_com_call *c) { uint32_t o = D->cursor; D->cursor = ARG(0) != 0; return o; }
static uint32_t dv_add_swapchain(sg_com_call *c) { /* (pp, IDirect3DSwapChain9**) */
    uint32_t pp[D3DPP_DWORDS], err;
    res *s;
    if (!ARG(0) || !ARG(1)) return D3DERR_INVALIDCALL;
    sg_st32(MEM, ARG(1), 0);
    err = read_pp(c, ARG(0), D->focus, pp);
    if (err) return err;
    s = swapchain_new(D, pp, &err);
    if (!s) return err;
    sg_st32(MEM, ARG(1), guest_of(s));
    return D3D_OK;
}
static uint32_t dv_get_swapchain(sg_com_call *c) {
    if (!ARG(1)) return D3DERR_INVALIDCALL;
    if (ARG(0) != 0) {
        sg_st32(MEM, ARG(1), 0);
        return D3DERR_INVALIDCALL;
    }
    out_obj(c, ARG(1), guest_of(D->sc));
    return D3D_OK;
}
static uint32_t dv_num_swapchains(sg_com_call *c) { return 1; }

static uint32_t make_default_targets(dev *d) {
    uint32_t err = D3D_OK;
    res *sc = swapchain_new(d, d->pp, &err);
    if (!sc) return err;
    sc->com.refs = 0;
    sc->com.owner = &d->com;
    d->sc = sc;
    if (d->pp[PP_AUTODS]) {
        res *ds = surface_new(d, d->pp[PP_W], d->pp[PP_H], d->pp[PP_DSFMT], D3DUSAGE_DEPTHSTENCIL, D3DPOOL_DEFAULT,
                              d->pp[PP_MS], d->pp[PP_MSQ], &err);
        if (!ds) return err;
        ds->com.refs = 0;
        ds->com.owner = &d->com;
        d->autods = ds;
    }
    return D3D_OK;
}
static void set_fpu_single(sg_ctx *ctx, uint32_t flags) {
    if (!(flags & D3DCREATE_FPU_PRESERVE)) ctx->fpu.cw = (uint16_t)((ctx->fpu.cw & ~0x0f3fu) | 0x003fu); /* as D3D9 does: single precision, round to nearest, exceptions masked */
}
static uint32_t dv_reset(sg_com_call *c) { /* (pp) */
    uint32_t pp[D3DPP_DWORDS], err;
    if (!ARG(0)) return D3DERR_INVALIDCALL;
    err = read_pp(c, ARG(0), D->focus, pp);
    if (err) return err;
    if (D->sc) res_free(D->sc);
    if (D->autods) res_free(D->autods);
    D->sc = NULL;
    D->autods = NULL;
    memcpy(D->pp, pp, sizeof pp);
    err = make_default_targets(D);
    if (err) return err;
    dev_reset_state(D);
    set_fpu_single(c->ctx, D->flags);
    return D3D_OK;
}
static uint32_t dv_present(sg_com_call *c) { /* (src, dst, hwnd, dirty) */
    dev_present(D, ARG(2));
    return D3D_OK;
}
static uint32_t dv_back_buffer(sg_com_call *c) { /* (swapchain, index, type, IDirect3DSurface9**) */
    if (!ARG(3)) return D3DERR_INVALIDCALL;
    if (ARG(0) != 0 || ARG(1) != 0 || !D->sc) {
        sg_st32(MEM, ARG(3), 0);
        return D3DERR_INVALIDCALL;
    }
    out_obj(c, ARG(3), guest_of(D->sc->bb));
    return D3D_OK;
}
static uint32_t dv_raster(sg_com_call *c) {
    if (!ARG(1)) return D3DERR_INVALIDCALL;
    sg_st32(MEM, ARG(1), 0);
    sg_st32(MEM, ARG(1) + 4, 0);
    return D3D_OK;
}
static uint32_t dv_set_gamma(sg_com_call *c) { /* (swapchain, flags, D3DGAMMARAMP*) -> void */
    if (ARG(2)) memcpy(D->gamma, MEM + ARG(2), sizeof D->gamma);
    return 0;
}
static uint32_t dv_get_gamma(sg_com_call *c) {
    if (ARG(1)) memcpy(MEM + ARG(1), D->gamma, sizeof D->gamma);
    return 0;
}
static uint32_t put_new(sg_com_call *c, uint32_t out, res *r, uint32_t err) {
    if (out) sg_st32(MEM, out, r ? guest_of(r) : 0);
    return r ? D3D_OK : err;
}
static uint32_t dv_create_texture(sg_com_call *c) { /* (w, h, levels, usage, fmt, pool, ppTex, pShared) */
    uint32_t err;
    res *t;
    if (!ARG(6) || ARG(7)) return D3DERR_INVALIDCALL;
    t = texture_new(D, &d3d9_iface_texture, D3DRTYPE_TEXTURE, ARG(0), ARG(1), 1, ARG(2), ARG(3), ARG(4), ARG(5), 1, &err);
    return put_new(c, ARG(6), t, err);
}
static uint32_t dv_create_volume(sg_com_call *c) { /* (w, h, d, levels, usage, fmt, pool, pp, shared) */
    uint32_t err;
    res *t;
    if (!ARG(7) || ARG(8)) return D3DERR_INVALIDCALL;
    t = texture_new(D, &d3d9_iface_volumetex, D3DRTYPE_VOLUMETEXTURE, ARG(0), ARG(1), ARG(2), ARG(3), ARG(4), ARG(5),
                    ARG(6), 1, &err);
    return put_new(c, ARG(7), t, err);
}
static uint32_t dv_create_cube(sg_com_call *c) { /* (edge, levels, usage, fmt, pool, pp, shared) */
    uint32_t err;
    res *t;
    if (!ARG(5) || ARG(6)) return D3DERR_INVALIDCALL;
    t = texture_new(D, &d3d9_iface_cube, D3DRTYPE_CUBETEXTURE, ARG(0), ARG(0), 1, ARG(1), ARG(2), ARG(3), ARG(4), 6, &err);
    return put_new(c, ARG(5), t, err);
}
static uint32_t dv_create_vb(sg_com_call *c) { /* (len, usage, fvf, pool, pp, shared) */
    uint32_t err;
    res *b;
    if (!ARG(4) || ARG(5)) return D3DERR_INVALIDCALL;
    b = buffer_new(D, &d3d9_iface_vb, D3DRTYPE_VERTEXBUFFER, ARG(0), ARG(1), ARG(2), ARG(3), &err);
    return put_new(c, ARG(4), b, err);
}
static uint32_t dv_create_ib(sg_com_call *c) { /* (len, usage, fmt, pool, pp, shared) */
    uint32_t err;
    res *b;
    if (!ARG(4) || ARG(5)) return D3DERR_INVALIDCALL;
    b = buffer_new(D, &d3d9_iface_ib, D3DRTYPE_INDEXBUFFER, ARG(0), ARG(1), ARG(2), ARG(3), &err);
    return put_new(c, ARG(4), b, err);
}
static uint32_t dv_create_rt(sg_com_call *c) { /* (w, h, fmt, ms, msq, lockable, pp, shared) */
    uint32_t err;
    res *s;
    if (!ARG(6) || ARG(7)) return D3DERR_INVALIDCALL;
    if (d3d9_check_format(D3DFMT_X8R8G8B8, D3DUSAGE_RENDERTARGET, D3DRTYPE_SURFACE, ARG(2))) return put_new(c, ARG(6), NULL, D3DERR_NOTAVAILABLE);
    s = surface_new(D, ARG(0), ARG(1), ARG(2), D3DUSAGE_RENDERTARGET, D3DPOOL_DEFAULT, ARG(3), ARG(4), &err);
    return put_new(c, ARG(6), s, err);
}
static uint32_t dv_create_ds(sg_com_call *c) { /* (w, h, fmt, ms, msq, discard, pp, shared) */
    uint32_t err;
    res *s;
    if (!ARG(6) || ARG(7)) return D3DERR_INVALIDCALL;
    if (d3d9_check_format(D3DFMT_X8R8G8B8, D3DUSAGE_DEPTHSTENCIL, D3DRTYPE_SURFACE, ARG(2))) return put_new(c, ARG(6), NULL, D3DERR_NOTAVAILABLE);
    s = surface_new(D, ARG(0), ARG(1), ARG(2), D3DUSAGE_DEPTHSTENCIL, D3DPOOL_DEFAULT, ARG(3), ARG(4), &err);
    return put_new(c, ARG(6), s, err);
}
static uint32_t dv_create_offscreen(sg_com_call *c) { /* (w, h, fmt, pool, pp, shared) */
    uint32_t err;
    res *s;
    if (!ARG(4) || ARG(5) || ARG(3) == D3DPOOL_MANAGED) return D3DERR_INVALIDCALL;
    s = surface_new(D, ARG(0), ARG(1), ARG(2), 0, ARG(3), 0, 0, &err);
    return put_new(c, ARG(4), s, err);
}
static uint32_t dv_obj_args_ok(sg_com_call *c, unsigned a, unsigned b) {
    return (ARG(a) && sg_com_lookup(MEM, ARG(a))) && (b == 99 || (ARG(b) && sg_com_lookup(MEM, ARG(b)))) ? D3D_OK : D3DERR_INVALIDCALL;
}
static uint32_t dv_update_surface(sg_com_call *c) { return dv_obj_args_ok(c, 0, 2); }
static uint32_t dv_update_texture(sg_com_call *c) { return dv_obj_args_ok(c, 0, 1); }
static uint32_t dv_rt_data(sg_com_call *c) { return dv_obj_args_ok(c, 0, 1); } /* leaves the destination as it is */
static uint32_t dv_front_data(sg_com_call *c) { return dv_obj_args_ok(c, 1, 99); }
static uint32_t dv_stretch(sg_com_call *c) { return dv_obj_args_ok(c, 0, 2); }
static uint32_t dv_color_fill(sg_com_call *c) { return dv_obj_args_ok(c, 0, 99); }

static void viewport_full(dev *d, uint32_t w, uint32_t h) {
    float one = 1.0f;
    d->vp[0] = d->vp[1] = 0;
    d->vp[2] = w, d->vp[3] = h;
    d->vp[4] = 0;
    memcpy(&d->vp[5], &one, 4);
    d->scissor[0] = d->scissor[1] = 0;
    d->scissor[2] = w, d->scissor[3] = h;
}
static uint32_t dv_set_rt(sg_com_call *c) { /* (index, surface) */
    res *s;
    if (ARG(0) >= 4 || (ARG(0) == 0 && !ARG(1))) return D3DERR_INVALIDCALL;
    s = ARG(1) ? (res *)sg_com_lookup(MEM, ARG(1)) : NULL;
    if (ARG(1) && !s) return D3DERR_INVALIDCALL;
    D->rt[ARG(0)] = ARG(1);
    if (ARG(0) == 0) viewport_full(D, s->w, s->h);
    return D3D_OK;
}
static uint32_t dv_get_rt(sg_com_call *c) { /* (index, IDirect3DSurface9**) */
    if (ARG(0) >= 4 || !ARG(1)) return D3DERR_INVALIDCALL;
    out_obj(c, ARG(1), D->rt[ARG(0)]);
    return sg_ld32(MEM, ARG(1)) ? D3D_OK : D3DERR_NOTFOUND;
}
static uint32_t dv_set_ds(sg_com_call *c) { D->ds = ARG(0); return D3D_OK; }
static uint32_t dv_get_ds(sg_com_call *c) {
    if (!ARG(0)) return D3DERR_INVALIDCALL;
    out_obj(c, ARG(0), D->ds);
    return sg_ld32(MEM, ARG(0)) ? D3D_OK : D3DERR_NOTFOUND;
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
static uint32_t dv_clear(sg_com_call *c) { return ARG(0) && !ARG(1) ? D3DERR_INVALIDCALL : D3D_OK; }
static float *transform_slot(dev *d, uint32_t s) { return s < 512 ? d->transforms[s] : NULL; }
static uint32_t dv_set_transform(sg_com_call *c) {
    float *t = transform_slot(D, ARG(0));
    if (!ARG(1)) return D3DERR_INVALIDCALL;
    if (t) memcpy(t, MEM + ARG(1), 64);
    return D3D_OK;
}
static uint32_t dv_get_transform(sg_com_call *c) {
    float *t = transform_slot(D, ARG(0));
    if (!ARG(1) || !t) return D3DERR_INVALIDCALL;
    memcpy(MEM + ARG(1), t, 64);
    return D3D_OK;
}
static uint32_t dv_mul_transform(sg_com_call *c) { /* M = M * arg */
    float *t = transform_slot(D, ARG(0)), a[16], b[16], r[16];
    if (!ARG(1) || !t) return D3DERR_INVALIDCALL;
    memcpy(a, t, 64);
    memcpy(b, MEM + ARG(1), 64);
    for (int i = 0; i < 4; i++)
        for (int j = 0; j < 4; j++) r[i * 4 + j] = a[i * 4] * b[j] + a[i * 4 + 1] * b[4 + j] + a[i * 4 + 2] * b[8 + j] + a[i * 4 + 3] * b[12 + j];
    memcpy(t, r, 64);
    return D3D_OK;
}
#define STORE(field, n) do { if (!ARG(0)) return D3DERR_INVALIDCALL; memcpy(D->field, MEM + ARG(0), n); return D3D_OK; } while (0)
#define LOAD(field, n) do { if (!ARG(0)) return D3DERR_INVALIDCALL; memcpy(MEM + ARG(0), D->field, n); return D3D_OK; } while (0)
static uint32_t dv_set_vp(sg_com_call *c) { STORE(vp, 24); }
static uint32_t dv_get_vp(sg_com_call *c) { LOAD(vp, 24); }
static uint32_t dv_set_material(sg_com_call *c) { STORE(material, 68); }
static uint32_t dv_get_material(sg_com_call *c) { LOAD(material, 68); }
static uint32_t dv_set_light(sg_com_call *c) { /* (index, D3DLIGHT9*) */
    if (!ARG(1) || ARG(0) >= 16) return ARG(1) ? D3D_OK : D3DERR_INVALIDCALL;
    memcpy(D->lights[ARG(0)], MEM + ARG(1), 104);
    D->light_set[ARG(0)] = 1;
    return D3D_OK;
}
static uint32_t dv_get_light(sg_com_call *c) {
    if (!ARG(1) || ARG(0) >= 16 || !D->light_set[ARG(0)]) return D3DERR_INVALIDCALL;
    memcpy(MEM + ARG(1), D->lights[ARG(0)], 104);
    return D3D_OK;
}
static uint32_t dv_light_enable(sg_com_call *c) {
    if (ARG(0) < 16) D->light_on[ARG(0)] = ARG(1) != 0, D->light_set[ARG(0)] = 1;
    return D3D_OK;
}
static uint32_t dv_get_light_enable(sg_com_call *c) {
    if (!ARG(1) || ARG(0) >= 16 || !D->light_set[ARG(0)]) return D3DERR_INVALIDCALL;
    sg_st32(MEM, ARG(1), D->light_on[ARG(0)]);
    return D3D_OK;
}
static uint32_t dv_set_clip(sg_com_call *c) {
    if (ARG(0) >= 6 || !ARG(1)) return D3DERR_INVALIDCALL;
    memcpy(D->clip[ARG(0)], MEM + ARG(1), 16);
    return D3D_OK;
}
static uint32_t dv_get_clip(sg_com_call *c) {
    if (ARG(0) >= 6 || !ARG(1)) return D3DERR_INVALIDCALL;
    memcpy(MEM + ARG(1), D->clip[ARG(0)], 16);
    return D3D_OK;
}
static uint32_t dv_set_rs(sg_com_call *c) {
    if (ARG(0) < 256) D->rs[ARG(0)] = ARG(1);
    return D3D_OK;
}
static uint32_t dv_get_rs(sg_com_call *c) {
    if (!ARG(1) || ARG(0) >= 256) return D3DERR_INVALIDCALL;
    sg_st32(MEM, ARG(1), D->rs[ARG(0)]);
    return D3D_OK;
}
static uint32_t dv_create_sb(sg_com_call *c) { /* (type, IDirect3DStateBlock9**) */
    res *s;
    if (!ARG(1) || ARG(0) < 1 || ARG(0) > 3) return D3DERR_INVALIDCALL;
    s = res_new(D, &d3d9_iface_stateblock, 0);
    return put_new(c, ARG(1), s, SG_E_OUTOFMEMORY);
}
static uint32_t dv_begin_sb(sg_com_call *c) {
    if (D->recording) return D3DERR_INVALIDCALL;
    D->recording = 1;
    return D3D_OK;
}
static uint32_t dv_end_sb(sg_com_call *c) {
    res *s;
    if (!ARG(0) || !D->recording) return D3DERR_INVALIDCALL;
    D->recording = 0;
    s = res_new(D, &d3d9_iface_stateblock, 0);
    return put_new(c, ARG(0), s, SG_E_OUTOFMEMORY);
}
static uint32_t dv_set_clip_status(sg_com_call *c) { STORE(clip_status, 24); }
static uint32_t dv_get_clip_status(sg_com_call *c) { LOAD(clip_status, 24); }
static int sampler_index(uint32_t s) {
    if (s < 16) return (int)s;
    if (s >= 256 && s <= 260) return (int)(s - 240);
    return -1;
}
static uint32_t dv_get_texture(sg_com_call *c) {
    int i = sampler_index(ARG(0));
    if (i < 0 || !ARG(1)) return D3DERR_INVALIDCALL;
    out_obj(c, ARG(1), D->tex[i]);
    return D3D_OK;
}
static uint32_t dv_set_texture(sg_com_call *c) {
    int i = sampler_index(ARG(0));
    if (i < 0) return D3DERR_INVALIDCALL;
    D->tex[i] = ARG(1);
    return D3D_OK;
}
static uint32_t dv_get_tss(sg_com_call *c) {
    if (ARG(0) >= 8 || ARG(1) >= 33 || !ARG(2)) return D3DERR_INVALIDCALL;
    sg_st32(MEM, ARG(2), D->tss[ARG(0)][ARG(1)]);
    return D3D_OK;
}
static uint32_t dv_set_tss(sg_com_call *c) {
    if (ARG(0) < 8 && ARG(1) < 33) D->tss[ARG(0)][ARG(1)] = ARG(2);
    return D3D_OK;
}
static uint32_t dv_get_ss(sg_com_call *c) {
    int i = sampler_index(ARG(0));
    if (i < 0 || ARG(1) >= 14 || !ARG(2)) return D3DERR_INVALIDCALL;
    sg_st32(MEM, ARG(2), D->ss[i][ARG(1)]);
    return D3D_OK;
}
static uint32_t dv_set_ss(sg_com_call *c) {
    int i = sampler_index(ARG(0));
    if (i < 0) return D3DERR_INVALIDCALL;
    if (ARG(1) < 14) D->ss[i][ARG(1)] = ARG(2);
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
static uint32_t dv_set_cur_palette(sg_com_call *c) { D->palette = ARG(0); return D3D_OK; }
static uint32_t dv_get_cur_palette(sg_com_call *c) {
    if (!ARG(0)) return D3DERR_INVALIDCALL;
    sg_st32(MEM, ARG(0), D->palette);
    return D3D_OK;
}
static uint32_t dv_set_scissor(sg_com_call *c) { STORE(scissor, 16); }
static uint32_t dv_get_scissor(sg_com_call *c) { LOAD(scissor, 16); }
static uint32_t dv_set_swvp(sg_com_call *c) { D->swvp = ARG(0) != 0; return D3D_OK; }
static uint32_t dv_get_swvp(sg_com_call *c) { return D->swvp; }
static uint32_t dv_set_npatch(sg_com_call *c) { D->npatch = SG_COM_ARGF(c, 0); return D3D_OK; }
static uint32_t dv_get_npatch(sg_com_call *c) { /* returns a float: in ST(0) */
    sg_fpu_push(c->ctx, (sg_f80)D->npatch);
    return 0;
}
static uint32_t prim_count(sg_com_call *c, uint32_t n) {
    d3d9_stats.draws++;
    d3d9_stats.primitives += n;
    return D3D_OK;
}
static uint32_t dv_draw(sg_com_call *c) { return prim_count(c, ARG(2)); }              /* (type, start, count) */
static uint32_t dv_draw_indexed(sg_com_call *c) { return prim_count(c, ARG(5)); }      /* (type, base, min, num, start, count) */
static uint32_t dv_draw_up(sg_com_call *c) { /* (type, count, data, stride): unbinds stream 0 */
    D->streams[0].vb = 0;
    D->streams[0].off = D->streams[0].stride = 0;
    return ARG(2) ? prim_count(c, ARG(1)) : D3DERR_INVALIDCALL;
}
static uint32_t dv_draw_indexed_up(sg_com_call *c) { /* (type, min, num, count, idx, idxfmt, data, stride) */
    D->streams[0].vb = 0;
    D->streams[0].off = D->streams[0].stride = 0;
    D->ib = 0;
    return ARG(4) && ARG(6) ? prim_count(c, ARG(3)) : D3DERR_INVALIDCALL;
}
static uint32_t dv_process(sg_com_call *c) { return ARG(3) ? D3D_OK : D3DERR_INVALIDCALL; }
static uint32_t dv_create_vdecl(sg_com_call *c) { /* (D3DVERTEXELEMENT9*, IDirect3DVertexDeclaration9**) */
    uint32_t p = ARG(0), n = 0;
    res *r;
    if (!p || !ARG(1)) return D3DERR_INVALIDCALL;
    while (n < 65 && sg_ld16(MEM, p + 8u * n) != 0xff) n++; /* D3DDECL_END has Stream 0xFF */
    if (n >= 65) return D3DERR_INVALIDCALL;
    n++;
    r = res_new(D, &d3d9_iface_vdecl, 0);
    if (!r) return put_new(c, ARG(1), NULL, SG_E_OUTOFMEMORY);
    r->blob = malloc(n * 8u);
    memcpy(r->blob, MEM + p, n * 8u);
    r->count = n;
    return put_new(c, ARG(1), r, D3D_OK);
}
static uint32_t dv_set_vdecl(sg_com_call *c) { D->vdecl = ARG(0); return D3D_OK; }
static uint32_t dv_get_vdecl(sg_com_call *c) {
    if (!ARG(0)) return D3DERR_INVALIDCALL;
    out_obj(c, ARG(0), D->vdecl);
    return D3D_OK;
}
static uint32_t dv_set_fvf(sg_com_call *c) { D->fvf = ARG(0); return D3D_OK; }
static uint32_t dv_get_fvf(sg_com_call *c) {
    if (!ARG(0)) return D3DERR_INVALIDCALL;
    sg_st32(MEM, ARG(0), D->fvf);
    return D3D_OK;
}
/* Bytecode length: version token, then instructions and comments up to the end token 0x0000FFFF. */
static uint32_t shader_len(const uint8_t *mem, uint32_t p) {
    uint32_t i = 1, n = 1u << 18;
    uint32_t v = sg_ld32(mem, p);
    if ((v & 0xfffe0000u) != 0xfffe0000u) return 0; /* 0xFFFE vs, 0xFFFF ps */
    while (i < n) {
        uint32_t t = sg_ld32(mem, p + 4u * i);
        if (t == 0x0000ffffu) return 4u * (i + 1);
        if ((t & 0xffffu) == 0xfffeu) i += 1 + (t >> 16); /* comment */
        else i++;
    }
    return 0;
}
static uint32_t create_shader(sg_com_call *c, sg_com_iface *iface) { /* (DWORD *function, IDirect3D*Shader9**) */
    uint32_t len;
    res *r;
    if (!ARG(0) || !ARG(1)) return D3DERR_INVALIDCALL;
    len = shader_len(MEM, ARG(0));
    if (!len) return put_new(c, ARG(1), NULL, D3DERR_INVALIDCALL);
    r = res_new(D, iface, 0);
    if (!r) return put_new(c, ARG(1), NULL, SG_E_OUTOFMEMORY);
    r->blob = malloc(len);
    memcpy(r->blob, MEM + ARG(0), len);
    r->blob_len = len;
    return put_new(c, ARG(1), r, D3D_OK);
}
static uint32_t dv_create_vs(sg_com_call *c) { return create_shader(c, &d3d9_iface_vs); }
static uint32_t dv_create_ps(sg_com_call *c) { return create_shader(c, &d3d9_iface_ps); }
static uint32_t dv_set_vs(sg_com_call *c) { D->vs = ARG(0); return D3D_OK; }
static uint32_t dv_set_ps(sg_com_call *c) { D->ps = ARG(0); return D3D_OK; }
static uint32_t dv_get_vs(sg_com_call *c) {
    if (!ARG(0)) return D3DERR_INVALIDCALL;
    out_obj(c, ARG(0), D->vs);
    return D3D_OK;
}
static uint32_t dv_get_ps(sg_com_call *c) {
    if (!ARG(0)) return D3DERR_INVALIDCALL;
    out_obj(c, ARG(0), D->ps);
    return D3D_OK;
}
/* Shader constants: (start, data, count) in units of 4 floats / 4 ints / 1 BOOL (4 bytes). */
static uint32_t consts(sg_com_call *c, void *shadow, uint32_t max, uint32_t unit, int set) {
    uint32_t start = ARG(0), p = ARG(1), n = ARG(2);
    if (!p || start > max || n > max - start) return D3DERR_INVALIDCALL;
    if (set) memcpy((uint8_t *)shadow + start * unit, MEM + p, n * unit);
    else memcpy(MEM + p, (uint8_t *)shadow + start * unit, n * unit);
    return D3D_OK;
}
static uint32_t dv_set_vsf(sg_com_call *c) { return consts(c, D->vsf, 256, 16, 1); }
static uint32_t dv_get_vsf(sg_com_call *c) { return consts(c, D->vsf, 256, 16, 0); }
static uint32_t dv_set_vsi(sg_com_call *c) { return consts(c, D->vsi, 16, 16, 1); }
static uint32_t dv_get_vsi(sg_com_call *c) { return consts(c, D->vsi, 16, 16, 0); }
static uint32_t dv_set_vsb(sg_com_call *c) { return consts(c, D->vsb, 16, 4, 1); }
static uint32_t dv_get_vsb(sg_com_call *c) { return consts(c, D->vsb, 16, 4, 0); }
static uint32_t dv_set_psf(sg_com_call *c) { return consts(c, D->psf, 224, 16, 1); }
static uint32_t dv_get_psf(sg_com_call *c) { return consts(c, D->psf, 224, 16, 0); }
static uint32_t dv_set_psi(sg_com_call *c) { return consts(c, D->psi, 16, 16, 1); }
static uint32_t dv_get_psi(sg_com_call *c) { return consts(c, D->psi, 16, 16, 0); }
static uint32_t dv_set_psb(sg_com_call *c) { return consts(c, D->psb, 16, 4, 1); }
static uint32_t dv_get_psb(sg_com_call *c) { return consts(c, D->psb, 16, 4, 0); }
static uint32_t dv_set_stream(sg_com_call *c) { /* (stream, vb, offset, stride) */
    if (ARG(0) >= 16) return D3DERR_INVALIDCALL;
    D->streams[ARG(0)].vb = ARG(1);
    D->streams[ARG(0)].off = ARG(2);
    D->streams[ARG(0)].stride = ARG(3);
    return D3D_OK;
}
static uint32_t dv_get_stream(sg_com_call *c) { /* (stream, ppVB, pOffset, pStride) */
    if (ARG(0) >= 16 || !ARG(1) || !ARG(2) || !ARG(3)) return D3DERR_INVALIDCALL;
    out_obj(c, ARG(1), D->streams[ARG(0)].vb);
    sg_st32(MEM, ARG(2), D->streams[ARG(0)].off);
    sg_st32(MEM, ARG(3), D->streams[ARG(0)].stride);
    return D3D_OK;
}
static uint32_t dv_set_freq(sg_com_call *c) {
    if (ARG(0) >= 16) return D3DERR_INVALIDCALL;
    D->streams[ARG(0)].freq = ARG(1);
    return D3D_OK;
}
static uint32_t dv_get_freq(sg_com_call *c) {
    if (ARG(0) >= 16 || !ARG(1)) return D3DERR_INVALIDCALL;
    sg_st32(MEM, ARG(1), D->streams[ARG(0)].freq);
    return D3D_OK;
}
static uint32_t dv_set_indices(sg_com_call *c) { D->ib = ARG(0); return D3D_OK; }
static uint32_t dv_get_indices(sg_com_call *c) {
    if (!ARG(0)) return D3DERR_INVALIDCALL;
    out_obj(c, ARG(0), D->ib);
    return D3D_OK;
}
static uint32_t dv_patch(sg_com_call *c) { return D3D_OK; }
static uint32_t dv_create_query(sg_com_call *c) { /* (type, IDirect3DQuery9**); NULL out = support probe */
    res *q;
    if (!query_size(ARG(0))) {
        if (ARG(1)) sg_st32(MEM, ARG(1), 0);
        return D3DERR_NOTAVAILABLE;
    }
    if (!ARG(1)) return D3D_OK;
    q = res_new(D, &d3d9_iface_query, 0);
    if (q) q->qtype = ARG(0);
    return put_new(c, ARG(1), q, SG_E_OUTOFMEMORY);
}

static const sg_com_method device_methods[] = {
    SG_COM_IUNKNOWN,
    M(Device, TestCooperativeLevel, dv_test_coop), M(Device, GetAvailableTextureMem, dv_texmem),
    M(Device, EvictManagedResources, ok0), M(Device, GetDirect3D, dv_get_d3d), M(Device, GetDeviceCaps, dv_caps),
    M(Device, GetDisplayMode, dv_display_mode), M(Device, GetCreationParameters, dv_creation),
    M(Device, SetCursorProperties, ok0), M(Device, SetCursorPosition, dv_cursor_pos), M(Device, ShowCursor, dv_show_cursor),
    M(Device, CreateAdditionalSwapChain, dv_add_swapchain), M(Device, GetSwapChain, dv_get_swapchain),
    M(Device, GetNumberOfSwapChains, dv_num_swapchains), M(Device, Reset, dv_reset), M(Device, Present, dv_present),
    M(Device, GetBackBuffer, dv_back_buffer), M(Device, GetRasterStatus, dv_raster), M(Device, SetDialogBoxMode, ok0),
    M(Device, SetGammaRamp, dv_set_gamma), M(Device, GetGammaRamp, dv_get_gamma),
    M(Device, CreateTexture, dv_create_texture), M(Device, CreateVolumeTexture, dv_create_volume),
    M(Device, CreateCubeTexture, dv_create_cube), M(Device, CreateVertexBuffer, dv_create_vb),
    M(Device, CreateIndexBuffer, dv_create_ib), M(Device, CreateRenderTarget, dv_create_rt),
    M(Device, CreateDepthStencilSurface, dv_create_ds), M(Device, UpdateSurface, dv_update_surface),
    M(Device, UpdateTexture, dv_update_texture), M(Device, GetRenderTargetData, dv_rt_data),
    M(Device, GetFrontBufferData, dv_front_data), M(Device, StretchRect, dv_stretch), M(Device, ColorFill, dv_color_fill),
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
    M(Device, CreateQuery, dv_create_query),
};

static void dev_reset_state(dev *d) {
    static const struct { uint16_t s; uint32_t v; } rs_defaults[] = {
        {7, 1}, {8, 3}, {9, 2}, {14, 1}, {16, 1}, {19, 2}, {20, 1}, {22, 3}, {23, 4}, {25, 8}, {37, 0x3f800000},
        {38, 0x3f800000}, {53, 1}, {54, 1}, {55, 1}, {56, 8}, {58, 0xffffffff}, {59, 0xffffffff}, {60, 0xffffffff},
        {136, 1}, {137, 1}, {141, 1}, {142, 1}, {145, 1}, {146, 2}, {154, 0x3f800000}, {155, 0x3f800000},
        {158, 0x3f800000}, {161, 1}, {162, 0xffffffff}, {165, 0xbaadcafe}, {166, 0x46000000}, {168, 0xf}, {171, 1},
        {172, 3}, {173, 1}, {178, 0x3f800000}, {179, 0x3f800000}, {182, 0x3f800000}, {186, 1}, {187, 1}, {188, 1},
        {189, 8}, {190, 0xf}, {191, 0xf}, {192, 0xf}, {193, 0xffffffff}, {207, 2}, {208, 1}, {209, 1}};
    memset(d->rs, 0, sizeof d->rs);
    for (unsigned i = 0; i < sizeof rs_defaults / sizeof rs_defaults[0]; i++) d->rs[rs_defaults[i].s] = rs_defaults[i].v;
    d->rs[7] = d->pp[PP_AUTODS] ? 1 : 0;
    memset(d->tss, 0, sizeof d->tss);
    for (int s = 0; s < 8; s++) {
        d->tss[s][1] = s ? 1 : 4;   /* COLOROP: MODULATE on stage 0, DISABLE after */
        d->tss[s][2] = 2;           /* COLORARG1 TEXTURE */
        d->tss[s][3] = 0;           /* COLORARG2 CURRENT */
        d->tss[s][4] = s ? 1 : 2;   /* ALPHAOP */
        d->tss[s][5] = 2;
        d->tss[s][6] = 0;
        d->tss[s][11] = (uint32_t)s; /* TEXCOORDINDEX */
    }
    memset(d->ss, 0, sizeof d->ss);
    for (int s = 0; s < 21; s++) {
        d->ss[s][1] = d->ss[s][2] = d->ss[s][3] = 1; /* WRAP */
        d->ss[s][5] = d->ss[s][6] = 1;              /* POINT */
        d->ss[s][10] = 1;
    }
    memset(d->tex, 0, sizeof d->tex);
    memset(d->streams, 0, sizeof d->streams);
    for (int s = 0; s < 16; s++) d->streams[s].freq = 1;
    d->ib = d->vdecl = d->vs = d->ps = d->fvf = 0;
    memset(d->transforms, 0, sizeof d->transforms);
    for (int t = 0; t < 512; t++)
        for (int i = 0; i < 4; i++) d->transforms[t][i * 5] = 1.0f;
    memset(d->rt, 0, sizeof d->rt);
    d->rt[0] = d->sc && d->sc->bb ? guest_of(d->sc->bb) : 0;
    d->ds = guest_of(d->autods);
    viewport_full(d, d->pp[PP_W], d->pp[PP_H]);
    d->in_scene = d->recording = 0;
    for (int i = 0; i < 256; i++) {
        d->gamma[2 * i] = d->gamma[512 + 2 * i] = d->gamma[1024 + 2 * i] = (uint8_t)i;
        d->gamma[2 * i + 1] = d->gamma[513 + 2 * i] = d->gamma[1025 + 2 * i] = (uint8_t)i;
    }
}

static void dev_destroy(sg_com_obj *o) {
    dev *d = (dev *)o;
    d->alive = 0;
    if (d->sc) res_free(d->sc);
    if (d->autods) res_free(d->autods);
    d->sc = d->autods = NULL;
    sg_com_obj_retire(o);
    sg_com_obj_release(&d->d3d->com);
    /* the dev struct stays allocated: resources the game still holds keep a pointer to it */
}
static const sg_com_class dev_class = {"IDirect3DDevice9", dev_destroy, NULL};

uint32_t d3d9_create_device(sg_com_call *c, d3d9_obj *d3d, uint32_t adapter, uint32_t devtype, uint32_t focus,
                            uint32_t flags, uint32_t ppa, uint32_t out) {
    uint32_t pp[D3DPP_DWORDS], err;
    dev *d;
    if (!out || !ppa) return D3DERR_INVALIDCALL;
    sg_st32(MEM, out, 0);
    if (adapter != 0) return D3DERR_INVALIDCALL;
    if (devtype != D3DDEVTYPE_HAL) return D3DERR_NOTAVAILABLE;
    /* exactly one of SOFTWARE 0x20 / HARDWARE 0x40 / MIXED 0x80 _VERTEXPROCESSING (a mask of 0x70 would
     * reject the game's MIXED 0x80) */
    {
        uint32_t vp = flags & 0xe0u;
        if (vp != 0x20u && vp != 0x40u && vp != 0x80u) return D3DERR_INVALIDCALL;
    }
    if (!focus && !sg_ld32(MEM, ppa + 4u * PP_HWND)) return D3DERR_INVALIDCALL;
    err = read_pp(c, ppa, focus, pp);
    if (err) return err;
    d = calloc(1, sizeof *d);
    if (!d || sg_com_obj_init(&d->com, &dev_class)) {
        free(d);
        return SG_E_OUTOFMEMORY;
    }
    d->alive = 1;
    d->d3d = d3d;
    d->adapter = adapter, d->devtype = devtype, d->focus = focus, d->flags = flags;
    d->cursor = 0;
    memcpy(d->pp, pp, sizeof pp);
    if (!sg_com_obj_expose(&d->com, &d3d9_iface_device) || (err = make_default_targets(d)) != D3D_OK) {
        sg_com_obj_retire(&d->com);
        free(d);
        return err ? err : SG_E_OUTOFMEMORY;
    }
    sg_com_obj_addref(&d3d->com);
    dev_reset_state(d);
    set_fpu_single(c->ctx, flags);
    sg_st32(MEM, out, dev_guest(d));
    if (sg_com_trace >= 1)
        fprintf(stderr, "d3d9: CreateDevice(flags 0x%x, %ux%u fmt %u, %s, depth %u, interval 0x%x, hwnd 0x%x) = 0x%08x\n",
                flags, pp[PP_W], pp[PP_H], pp[PP_FMT], pp[PP_WINDOWED] ? "windowed" : "fullscreen",
                pp[PP_AUTODS] ? pp[PP_DSFMT] : 0, pp[PP_INTERVAL], pp[PP_HWND], dev_guest(d));
    return D3D_OK;
}

/* ---- ID3DXBuffer ------------------------------------------------------------------------------- */
static uint32_t xb_ptr(sg_com_call *c) { return res_backing(R); }
static uint32_t xb_size(sg_com_call *c) { return R->w; }
static const sg_com_method d3dxbuf_methods[] = {SG_COM_IUNKNOWN, M(D3DXBuffer, GetBufferPointer, xb_ptr),
                                                M(D3DXBuffer, GetBufferSize, xb_size)};
uint32_t d3d9null_d3dx_buffer(uint32_t size, uint32_t *data) {
    res *b = res_new(NULL, &d3d9_iface_d3dxbuf, 0);
    if (!b) return 0;
    b->w = size, b->bytes = size ? size : 1;
    if (!res_backing(b)) {
        res_free(b);
        return 0;
    }
    if (data) *data = b->mem;
    return guest_of(b);
}

/* ---- interfaces -------------------------------------------------------------------------------- */
/* IIDs checked (2026-10-12) field by field against the DirectX SDK (June 2010) Include/d3d9.h
 * and Include/d3dx9core.h (Microsoft; mirror github.com/apitrace/dxsdk) and Wine's d3d9.h/d3dx9core.h
 * (the mingw-w64 directx headers): all 16 (these 15 and IDirect3D9 in
 * d3d9_adapter.c) match both. IDirect3DResource9 {05eec05d-...} and IDirect3DBaseTexture9
 * {580ca87e-...} are not answered by QueryInterface; no D3D IID occurs in the exe's bytes. */
#define IFACE(var, name, S, s, methods, d1, d2, d3, ...)                                                        \
    sg_com_iface var = {name, {d1, d2, d3, {__VA_ARGS__}}, S##_NSLOTS, methods, SG_COM_N(methods), s##_names, s##_args, \
                        D3DERR_INVALIDCALL, &d3d9_lock, 0, 0}
IFACE(d3d9_iface_device, "IDirect3DDevice9", DEVICE, device, device_methods, 0xd0223b96, 0xbf7a, 0x43fd, 0x92, 0xbd, 0xa4, 0x3b, 0x0d, 0x82, 0xb9, 0xeb);
IFACE(d3d9_iface_swapchain, "IDirect3DSwapChain9", SWAPCHAIN, swapchain, swapchain_methods, 0x794950f2, 0xadfc, 0x458a, 0x90, 0x5e, 0x10, 0xa1, 0x0b, 0x0b, 0x50, 0x3b);
IFACE(d3d9_iface_texture, "IDirect3DTexture9", TEXTURE, texture, texture_methods, 0x85c31227, 0x3de5, 0x4f00, 0x9b, 0x3a, 0xf1, 0x1a, 0xc3, 0x8c, 0x18, 0xb5);
IFACE(d3d9_iface_cube, "IDirect3DCubeTexture9", CUBETEXTURE, cubetexture, cube_methods, 0xfff32f81, 0xd953, 0x473a, 0x92, 0x23, 0x93, 0xd6, 0x52, 0xab, 0xa9, 0x3f);
IFACE(d3d9_iface_volumetex, "IDirect3DVolumeTexture9", VOLUMETEXTURE, volumetexture, volumetex_methods, 0x2518526c, 0xe789, 0x4111, 0xa7, 0xb9, 0x47, 0xef, 0x32, 0x8d, 0x13, 0xe6);
IFACE(d3d9_iface_surface, "IDirect3DSurface9", SURFACE, surface, surface_methods, 0x0cfbaf3a, 0x9ff6, 0x429a, 0x99, 0xb3, 0xa2, 0x79, 0x6a, 0xf8, 0xb8, 0x9b);
IFACE(d3d9_iface_volume, "IDirect3DVolume9", VOLUME, volume, volume_methods, 0x24f416e6, 0x1f67, 0x4aa7, 0xb8, 0x8e, 0xd3, 0x3f, 0x6f, 0x31, 0x28, 0xa1);
IFACE(d3d9_iface_vb, "IDirect3DVertexBuffer9", VERTEXBUFFER, vertexbuffer, vb_methods, 0xb64bb1b5, 0xfd70, 0x4df6, 0xbf, 0x91, 0x19, 0xd0, 0xa1, 0x24, 0x55, 0xe3);
IFACE(d3d9_iface_ib, "IDirect3DIndexBuffer9", INDEXBUFFER, indexbuffer, ib_methods, 0x7c9dd65e, 0xd3f7, 0x4529, 0xac, 0xee, 0x78, 0x58, 0x30, 0xac, 0xde, 0x35);
IFACE(d3d9_iface_vdecl, "IDirect3DVertexDeclaration9", VERTEXDECLARATION, vertexdeclaration, vdecl_methods, 0xdd13c59c, 0x36fa, 0x4098, 0xa8, 0xfb, 0xc7, 0xed, 0x39, 0xdc, 0x85, 0x46);
IFACE(d3d9_iface_vs, "IDirect3DVertexShader9", VERTEXSHADER, vertexshader, vs_methods, 0xefc5557e, 0x6265, 0x4613, 0x8a, 0x94, 0x43, 0x85, 0x78, 0x89, 0xeb, 0x36);
IFACE(d3d9_iface_ps, "IDirect3DPixelShader9", PIXELSHADER, pixelshader, ps_methods, 0x6d3bdbdc, 0x5b02, 0x4415, 0xb8, 0x52, 0xce, 0x5e, 0x8b, 0xcc, 0xb2, 0x89);
IFACE(d3d9_iface_query, "IDirect3DQuery9", QUERY, query, query_methods, 0xd9771460, 0xa695, 0x4f26, 0xbb, 0xd3, 0x27, 0xb8, 0x40, 0xb5, 0x41, 0xcc);
IFACE(d3d9_iface_stateblock, "IDirect3DStateBlock9", STATEBLOCK, stateblock, stateblock_methods, 0xb07c4fe5, 0x310d, 0x4ba8, 0xa2, 0x3c, 0x4f, 0x0f, 0x20, 0x6f, 0x21, 0x8b);
IFACE(d3d9_iface_d3dxbuf, "ID3DXBuffer", D3DXBUFFER, d3dxbuffer, d3dxbuf_methods, 0x8ba5fb08, 0x5195, 0x40e2, 0xac, 0x58, 0x0d, 0x98, 0x9c, 0x3a, 0x01, 0x02);
