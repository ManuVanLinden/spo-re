/* d3d9metal_int.h -- the internal interface of the Metal back end of the guest Direct3D 9.
 *
 * The device layer (src/: device, resources, locks, readback, present) provides it to the pipeline
 * (pipeline/: state objects, vertex descriptors, draws, clear) and the shader library (shader/: d3dbc ->
 * MSL). RULE: this header is only ever ADDED to; nothing is renamed or removed once it exists.
 *
 * Build model (runtime/d3d9metal/Makefile; tools/link/build.py for the game):
 *   - every .m/.mm in the package compiles with -fno-objc-arc (manual retain/release). Metal objects held
 *     in the structs below are +1 references owned by the struct; release them in the matching free path.
 *   - in Objective-C/C++ the Metal fields are typed (id<MTLTexture> etc.); in plain C they are void *.
 *     The layout is identical (one pointer each).
 *   - every handler runs under the one device mutex (d3d9null_device_lock(); DEVIATIONS 3.12), so nothing
 *     here is thread-safe on its own and nothing needs to be.
 *
 * Object model: one guest COM record per host object (sg_com_obj first in every struct, as the null
 * device). Containment as in D3D9 and the null device: level surfaces/volumes count on their
 * texture, back buffers on their swap chain, the implicit swap chain and the auto depth surface on the
 * device.
 *
 * Frame model:
 *   one MTLCommandBuffer at a time per device ("the current command buffer", created on demand, serial
 *   numbered). Uploads (Unlock of a texture, UpdateSurface/UpdateTexture, StretchRect, ColorFill) are blit
 *   commands on the current command buffer and end the open render encoder first, so they are ordered with
 *   draws exactly as the guest issued them. A render encoder is open only between mt_pass_encoder() and the
 *   next mt_pass_end() (called by the device layer for uploads, target changes, readback and Present). Present ends the
 *   pass, draws the back buffer into the layer's drawable, presents, commits and starts nothing (the next
 *   call creates the next command buffer). Readback (Lock of a render target, GetRenderTargetData,
 *   occlusion/event query results) commits and waits.
 */
#ifndef D3D9METAL_INT_H
#define D3D9METAL_INT_H

#include <stdint.h>
#include <pthread.h>

#include "sg_com.h"
#include "d3d9null.h"

#ifdef __OBJC__
#import <Metal/Metal.h>
#import <QuartzCore/CAMetalLayer.h>
#define MT_ID(proto) id<proto>
#define MT_OBJ(cls) cls *
#else
#define MT_ID(proto) void *
#define MT_OBJ(cls) void *
#endif

#ifdef __cplusplus
extern "C" {
#endif

/* ---- result codes and the enums the pipeline needs (values from the public d3d9types.h documentation) ------ */
#ifndef D3D_OK
#define D3D_OK 0u
#endif
#define MT_D3DERR_INVALIDCALL 0x8876086cu
#define MT_D3DERR_NOTAVAILABLE 0x8876086au
#define MT_D3DERR_DEVICELOST 0x88760868u
#define MT_D3DERR_OUTOFVIDEOMEMORY 0x8876017cu
#define MT_E_OUTOFMEMORY 0x8007000eu

/* object kinds (mt_obj.kind) */
enum mt_kind {
    MT_K_D3D, MT_K_DEV, MT_K_SC, MT_K_TEX, MT_K_CUBE, MT_K_VOLTEX, MT_K_SURF, MT_K_VOL, MT_K_VB, MT_K_IB,
    MT_K_VDECL, MT_K_VS, MT_K_PS, MT_K_QUERY, MT_K_SB, MT_K_NKIND
};

/* ---- limits ---------------------------------------------------------------------------------------- */
#define MT_MAXLV 16          /* mip levels */
#define MT_NSTREAMS 16       /* vertex streams */
#define MT_NSAMPLERS 21      /* 0..15 pixel samplers, 16..20 = D3DDMAPSAMPLER (256) and D3DVERTEXTEXTURESAMPLER0..3 (257..260) */
#define MT_SAMPLER_DMAP 16
#define MT_SAMPLER_VTX0 17   /* vertex texture sampler k (D3DVERTEXTEXTURESAMPLER0 + k) is index 17 + k */
#define MT_NSS 14            /* sampler states 0..13 (D3DSAMP_ADDRESSU = 1 .. D3DSAMP_DMAPOFFSET = 13) */
#define MT_NTSS 33           /* texture stage states 0..32 per stage */
#define MT_NSTAGES 8
#define MT_NRS 256           /* render states 0..255 (D3DRS_* values index directly) */
#define MT_VS_NCONSTF 256
#define MT_PS_NCONSTF 224
#define MT_NCONSTI 16
#define MT_NCONSTB 16
#define MT_NRT 4

/* ---- Metal binding layout shared with the translated shaders (shader/d3d9msl.h matches these) ---------- *
 * Buffer argument table, both stages:                                                                     */
#define MT_BUF_CONST_F 0      /* float4 c[256] (vertex) / c[224] (fragment): the stage's float constants */
#define MT_BUF_CONST_I 1      /* int4 i[16] */
#define MT_BUF_CONST_B 2      /* uint b[16] (one 32-bit word per bool register, 0 or 1) */
#define MT_BUF_DRIVER 3       /* per-draw driver constants (half-pixel offset, viewport, alpha test, fog,
                               * per-sampler info); layout owned by shader/ and pipeline/ (shader/d3d9msl.h) */
#define MT_BUF_STREAM0 14     /* vertex stream k is buffer index MT_BUF_STREAM0 + k (14..29) */
#define MT_BUF_STREAM(k) (MT_BUF_STREAM0 + (k))
/* Textures and samplers: fragment texture/sampler index s = pixel sampler s (0..15); vertex
 * texture/sampler index k = vertex texture sampler k (0..3, guest sampler 257 + k). */
#define MT_TEX_PS(s) (s)
#define MT_TEX_VS(k) (k)

/* ---- formats --------------------------------------------------------------------------------------- */
enum {
    MT_FMT_COLOR = 1u,        /* sampleable colour format */
    MT_FMT_RENDERABLE = 2u,   /* can be a render target */
    MT_FMT_DEPTH = 4u,
    MT_FMT_STENCIL = 8u,
    MT_FMT_BLOCK = 16u,       /* BC (DXT) 4x4 blocks */
    MT_FMT_CONVERT = 32u,     /* the guest layout differs from the Metal one: upload/readback convert */
    MT_FMT_ALPHA1 = 64u,      /* X-channel format: alpha reads as 1 through the swizzle */
    MT_FMT_FLOAT = 128u
};
typedef struct mt_format {
    uint32_t d3dfmt;
    uint32_t mtl;            /* MTLPixelFormat */
    uint32_t flags;          /* MT_FMT_* */
    uint8_t swz[4];          /* MTLTextureSwizzle for r,g,b,a of the sampling view (MTLTextureSwizzleRed = 2 ...) */
    uint8_t guest_bpp;       /* bytes per pixel in the guest layout (0 for block formats) */
    uint8_t mtl_bpp;         /* bytes per pixel in the Metal layout (0 for block formats) */
    uint8_t block;           /* bytes per 4x4 block (DXT), else 0 */
    uint8_t pad;
} mt_format;
/* The format record for a D3DFORMAT, or NULL if the Metal back end cannot create it. */
const mt_format *mt_format_of(uint32_t d3dfmt);

/* ---- objects --------------------------------------------------------------------------------------- */
typedef struct mt_dev mt_dev;
typedef struct mt_obj mt_obj;

typedef struct mt_lockrec {   /* one outstanding Lock (device layer internal) */
    uint32_t face, level, flags;
    uint32_t x0, y0, x1, y1, z0, z1; /* the locked region in texels (buffers: x0..x1 = byte range) */
} mt_lockrec;

/* Every guest-visible object. Fields below `kind` are used by the kinds noted. */
struct mt_obj {
    sg_com_obj com;
    int kind;                 /* enum mt_kind */
    mt_dev *dev;              /* the device (NULL for IDirect3D9 and for the device itself... the device's own is itself) */
    mt_obj *parent;           /* container of a level surface/volume; swap chain of a back buffer; device of
                               * the implicit swap chain and auto depth surface */
    mt_obj **kids;            /* children created on demand (level surfaces, back buffers) */
    unsigned nkids, capkids;
    uint64_t uid;             /* unique, never reused: a cache key that survives address reuse (named uid: `id` would hide the ObjC type) */

    /* ---- resources (TEX, CUBE, VOLTEX, SURF, VOL, VB, IB) ---- */
    uint32_t fmt, usage, pool, fvf;            /* D3D9 description (buffers: fmt = INDEX16/32 or VERTEXDATA) */
    uint32_t w, h, d, levels, faces;           /* buffers: w = size in bytes */
    uint32_t ms, msq;                          /* multisample type/quality (surfaces) */
    uint32_t face, level;                      /* level surface/volume: where in the parent */
    const mt_format *mf;                       /* textures/surfaces; NULL for buffers */
    /* guest Lock shadow (runtime-abi 8.1): the null device's layout (d3d9null_layout) */
    uint32_t lvl_off[MT_MAXLV], lvl_pitch[MT_MAXLV], lvl_slice[MT_MAXLV], face_bytes, bytes;
    uint32_t shadow;                           /* guest address of the shadow, 0 until first needed */
    mt_lockrec *locks;
    unsigned nlocks, caplocks;
    uint32_t lod, priority, autogen_filter;
    int shadow_valid;                          /* shadow holds the current contents (CPU-written resources) */
    int gpu_dirty;                             /* the GPU wrote it since the shadow was filled (RTs, depth) */
    int mips_dirty;                            /* AUTOGENMIPMAP: level 0 changed; regenerate before sampling */
    /* Metal objects */
    MT_ID(MTLTexture) tex;                     /* the texture (for a level surface: the parent's; see mt_surface_view) */
    MT_ID(MTLTexture) view;                    /* sampling view with the format swizzle (or == tex) */
    MT_ID(MTLBuffer) buf;                      /* VB/IB: the current Metal buffer (renamed on DISCARD in flight) */
    uint64_t used_serial;                      /* last command buffer serial that read or wrote it (mt_mark_used) */

    /* ---- vertex declaration ---- */
    uint8_t *decl;                             /* D3DVERTEXELEMENT9 array as the guest gave it (8 bytes each, END included) */
    uint32_t decl_count;                       /* elements before D3DDECL_END */
    /* ---- shaders ---- */
    uint8_t *code;                             /* bytecode copy */
    uint32_t code_len;                         /* bytes, end token included */
    uint64_t code_hash;                        /* FNV-1a 64 of the bytecode (the shader cache key) */
    /* ---- query ---- */
    uint32_t qtype;
    int q_state;                               /* 0 idle, 1 begun, 2 issued (end), result pending */
    uint64_t q_serial;                         /* command buffer serial the result waits for */
    uint64_t q_result;
    uint32_t q_slot;                           /* occlusion: offset in the device's visibility buffer (the pipeline writes it) */
    /* ---- swap chain ---- */
    uint32_t sc_hwnd;
    uint32_t pp[14];                           /* D3DPRESENT_PARAMETERS as stored */
    mt_obj *backbuf;                           /* the swap chain's back buffer (one; BackBufferCount > 1 shares it) */

    /* ---- private slots for the pipeline and the shader library. The device layer never touches them
     * except to call mt_pl_object_freed() before the object goes. */
    void *pl;
    void *pl2;

    /* ---- uploads, buffer renaming, queries, private data and lifetime ---- */
    int upload_pending;                        /* MANAGED: AddDirtyRect / NO_DIRTY_UPDATE regions not uploaded yet
                                                * (mt_texture_of uploads the whole resource from the shadow) */
    int lockable;                              /* CreateRenderTarget(Lockable) / lockable back buffer */
    uint64_t metal_bytes;                      /* bytes of Metal memory this object created */
    struct { MT_ID(MTLBuffer) buf; uint64_t serial; } spare[4]; /* VB/IB: retired Metal buffers for renaming */
    unsigned nspare;
    uint32_t q_slots[16];                      /* occlusion: visibility slots (one per render pass the query spans) */
    unsigned q_nslots;
    uint32_t dirty[6];                         /* MANAGED: union of dirty regions x0,y0,x1,y1,z0,z1 (level 0) */
    void *pdata;                               /* SetPrivateData list */
    int zombie;                                /* released by the guest while bound: freed when unbound */
};

/* ---- the stored-state block (src/ stores every setter; pipeline/ reads it at draw time) ------------- */
enum {                                     /* mt_state.dirty bits, set by src/'s setters, cleared by pipeline/ */
    MT_DIRTY_RS = 1u << 0,                 /* any render state */
    MT_DIRTY_SS = 1u << 1,                 /* any sampler state */
    MT_DIRTY_TEX = 1u << 2,                /* any texture binding */
    MT_DIRTY_VDECL = 1u << 3,
    MT_DIRTY_STREAMS = 1u << 4,            /* stream source, offset, stride or frequency */
    MT_DIRTY_IB = 1u << 5,
    MT_DIRTY_VIEWPORT = 1u << 6,
    MT_DIRTY_SCISSOR = 1u << 7,
    MT_DIRTY_VS = 1u << 8,
    MT_DIRTY_PS = 1u << 9,
    MT_DIRTY_VS_CONST = 1u << 10,          /* vs float/int/bool constants */
    MT_DIRTY_PS_CONST = 1u << 11,
    MT_DIRTY_TARGETS = 1u << 12,           /* render targets or depth-stencil */
    MT_DIRTY_TSS = 1u << 13,
    MT_DIRTY_FVF = 1u << 14,
    MT_DIRTY_TRANSFORM = 1u << 15,
    MT_DIRTY_ALL = 0xffffffffu
};
typedef struct mt_stream {
    mt_obj *vb;                            /* MT_K_VB or NULL */
    uint32_t offset, stride;
    uint32_t freq;                         /* SetStreamSourceFreq value as given (D3DSTREAMSOURCE_INDEXEDDATA etc.) */
} mt_stream;
typedef struct mt_viewport {
    uint32_t x, y, w, h;
    float minz, maxz;
} mt_viewport;
typedef struct mt_state {
    uint32_t dirty;                        /* MT_DIRTY_* */
    uint32_t rs[MT_NRS];                   /* D3DRS_* by value, D3D9 defaults after CreateDevice/Reset */
    uint32_t ss[MT_NSAMPLERS][MT_NSS];     /* D3DSAMP_* per sampler (index as MT_NSAMPLERS explains) */
    uint32_t tss[MT_NSTAGES][MT_NTSS];     /* D3DTSS_* per stage */
    mt_obj *tex[MT_NSAMPLERS];             /* TEX, CUBE or VOLTEX (no reference held; cleared on free) */
    mt_obj *vdecl;                         /* MT_K_VDECL */
    uint32_t fvf;
    mt_stream streams[MT_NSTREAMS];
    mt_obj *ib;                            /* MT_K_IB */
    mt_viewport vp;
    uint32_t scissor[4];                   /* left, top, right, bottom */
    mt_obj *vs, *ps;                       /* MT_K_VS / MT_K_PS or NULL */
    float vsf[MT_VS_NCONSTF * 4];
    float psf[MT_PS_NCONSTF * 4];
    int32_t vsi[MT_NCONSTI * 4], psi[MT_NCONSTI * 4];
    uint32_t vsb[MT_NCONSTB], psb[MT_NCONSTB];
    uint32_t vsf_hi, psf_hi;               /* 1 + highest float register ever set (upload bound) */
    mt_obj *rt[MT_NRT];                    /* render target surfaces (MT_K_SURF), rt[0] never NULL after CreateDevice */
    mt_obj *ds;                            /* depth-stencil surface or NULL */
    float transforms[512][16];             /* SetTransform (stored for Get only; the game uses shaders) */
    float clip[6][4];
} mt_state;

/* ---- the device -------------------------------------------------------------------------------------- */
struct mt_dev {
    mt_obj o;                              /* kind MT_K_DEV; o.dev == this device */
    mt_obj *d3d;                           /* the IDirect3D9 that created it (holds a reference) */
    int alive, lost;
    uint32_t adapter, devtype, focus, flags;
    uint32_t pp[14];
    mt_obj *sc;                            /* the implicit swap chain */
    mt_obj *autods;                        /* the auto depth-stencil surface, or NULL */

    MT_ID(MTLDevice) mtl;
    MT_ID(MTLCommandQueue) queue;
    MT_ID(MTLCommandBuffer) cmd;           /* current command buffer or nil (mt_cmd() creates it) */
    MT_ID(MTLRenderCommandEncoder) enc;    /* open render encoder or nil */
    MT_ID(MTLBlitCommandEncoder) blit;     /* open blit encoder or nil (mt_blit() opens; mt_pass_end() closes) */
    MT_OBJ(CAMetalLayer) layer;            /* the layer frames present into (retained), or nil before the window exists */
    uint64_t serial;                       /* serial of the current (or next) command buffer; starts at 1 */
    volatile uint64_t completed;           /* highest serial whose command buffer has completed */
    uint64_t frames;
    int in_scene;

    /* the render pass the open encoder draws into (valid while enc != nil) */
    mt_obj *pass_rt[MT_NRT], *pass_ds;
    uint32_t pass_w, pass_h;
    /* a clear requested for the next pass (mt_pass_clear); consumed when the pass begins */
    uint32_t pending_clear;                /* D3DCLEAR_TARGET 1 | ZBUFFER 2 | STENCIL 4 */
    float clear_rgba[4];
    float clear_z;
    uint32_t clear_stencil;

    /* per-frame scratch ring (UP draws, constants): see mt_scratch */
    MT_ID(MTLBuffer) scratch[3];
    uint32_t scratch_size, scratch_off, scratch_idx;
    uint64_t scratch_serial[3];

    /* occlusion queries: one visibility buffer per command buffer, the pipeline sets it on the pass descriptor
     * through mt_pass_visibility(); slots of 8 bytes */
    MT_ID(MTLBuffer) vis;
    uint32_t vis_slots, vis_next;

    mt_state st;

    /* resources freed while a command buffer may still use their Metal objects: released when
     * `completed` passes their serial */
    struct { void *obj; uint64_t serial; } *graveyard;
    unsigned ngrave, capgrave;

    void *pl;                              /* pipeline/'s per-device data (PSO cache etc.) */
    void *sh;                              /* shader/'s per-device data (MTLLibrary cache) */

    /* ---- queries, the back end's own utility pipelines, present and pass state ---- */
    mt_obj *occ_active;                    /* the occlusion query between Issue(BEGIN) and Issue(END), or NULL;
                                            * src/ sets the visibility mode on every pass it begins */
    MT_ID(MTLLibrary) util_lib;            /* src/'s own MSL (present, StretchRect, ColorFill) */
    void *util_pso[16];                    /* id<MTLRenderPipelineState> by (kind, pixel format), src/ internal */
    uint32_t util_key[16];
    MT_ID(MTLSamplerState) samp_point, samp_linear;
    uint64_t last_present_ns;
    uint32_t present_hwnd;                 /* HWND of the last Present */
    MT_ID(MTLCommandBuffer) last_cmd;      /* the last committed command buffer (retained), for waits */
    void *devpriv;                         /* device-layer private data (lights, material, gamma, ...) */
    mt_obj *pass_req_rt[MT_NRT], *pass_req_ds; /* st.rt/st.ds when the open pass began (pass_rt/pass_ds: attached) */
};

/* ---- functions the device layer provides to the pipeline ----------------------------------------- */
/* The current command buffer (created on demand). */
MT_ID(MTLCommandBuffer) mt_cmd(mt_dev *d);
/* The render encoder for the current targets (st.rt / st.ds), beginning a pass if none is open or the
 * targets changed since it began (load actions: Load, or Clear for a pending mt_pass_clear). The
 * viewport and scissor are NOT set: the pipeline sets them on the returned encoder. */
MT_ID(MTLRenderCommandEncoder) mt_pass_encoder(mt_dev *d);
/* Ends the open render or blit encoder, if any. */
void mt_pass_end(mt_dev *d);
/* A clear of the whole current targets: ends the open pass and begins a new one on the current targets
 * with Clear load actions for the given flags (D3DCLEAR_TARGET 1, ZBUFFER 2, STENCIL 4), so the clear
 * happens even if nothing is drawn after it; the next mt_pass_encoder() returns that pass. A Clear limited
 * by viewport/scissor/rects is the pipeline's (a quad draw). Returns 0. */
int mt_pass_clear(mt_dev *d, uint32_t flags, const float rgba[4], float z, uint32_t stencil);
/* A blit encoder on the current command buffer (ends the render pass). */
MT_ID(MTLBlitCommandEncoder) mt_blit(mt_dev *d);
/* Commits the current command buffer; wait != 0 waits for it to complete. */
void mt_flush(mt_dev *d, int wait);
/* Waits until command buffer `serial` has completed (commits the current one first if needed). */
void mt_wait_serial(mt_dev *d, uint64_t serial);

/* Per-frame scratch memory for UP draws and constant uploads: `size` bytes aligned to `align` (<= 256),
 * valid until the command buffer that uses it completes. Returns the buffer (not retained) and writes
 * the offset and the host pointer to write through. */
MT_ID(MTLBuffer) mt_scratch(mt_dev *d, uint32_t size, uint32_t align, uint32_t *offset, void **host);

/* Records that the current command buffer uses resource r (VB, IB, texture, surface): the pipeline calls it for
 * every resource a draw binds, so Lock/Unlock renames or waits correctly. */
void mt_mark_used(mt_dev *d, mt_obj *r);
/* The Metal buffer of a VB/IB as it must be bound now (uploads pending Unlocks first). */
MT_ID(MTLBuffer) mt_buffer_of(mt_dev *d, mt_obj *vb_or_ib);
/* The sampling texture of a TEX/CUBE/VOLTEX (format swizzle applied; auto-mipmaps regenerated if
 * dirty, which may end the render pass: call it before mt_pass_encoder()). NULL if none. */
MT_ID(MTLTexture) mt_texture_of(mt_dev *d, mt_obj *tex);
/* A surface as a render pass attachment: the Metal texture, slice (cube face) and level. */
MT_ID(MTLTexture) mt_surface_target(mt_obj *surf, uint32_t *slice, uint32_t *level);
/* The visibility result buffer for occlusion queries (created on demand); the pipeline puts it on the pass. */
MT_ID(MTLBuffer) mt_pass_visibility(mt_dev *d);
/* A guest object pointer -> our object of `kind` (or any kind if kind < 0), NULL if not ours. */
mt_obj *mt_from_guest(const uint8_t *mem, uint32_t guest, int kind);
uint32_t mt_guest_of(mt_obj *o);

/* The device layer's own handler for an IDirect3DDevice9 slot (d3d9_slots.h S_Device_*): what the
 * pipeline calls to keep the stored state when it overrides a slot. NULL for a slot src/ does not implement. */
sg_com_handler *mt_device_handler(unsigned slot);

/* Logging (SPORE_METAL_VERBOSE >= 1) and the trace level. */
extern int mt_verbose;
#define MT_LOG(...) fprintf(stderr, "d3d9metal: " __VA_ARGS__)

/* ---- hooks pipeline/ implements; src/ has weak defaults ---------------------------------------------- */
/* The handler pipeline/ provides for an IDirect3DDevice9 slot, or NULL to keep src/'s. Asked once per slot
 * when the device vtable is built. src/'s defaults: Draw* and Clear log once and return D3D_OK (stubs);
 * every setter stores into mt_state and sets the dirty bit. */
sg_com_handler *mt_pipeline_handler(unsigned slot);
void mt_pl_device_created(mt_dev *d);         /* after CreateDevice (Metal device and state ready) */
void mt_pl_device_reset(mt_dev *d);           /* after Reset (state back to defaults, new implicit targets) */
void mt_pl_device_destroyed(mt_dev *d);       /* before the device's Metal objects go */
void mt_pl_object_freed(mt_obj *o);           /* before any object (shader, vdecl, resource) is freed */
void mt_pl_frame_end(mt_dev *d);              /* in Present, after the frame's command buffer is committed */
void mt_pl_shader_created(mt_obj *o);         /* after CreateVertexShader/CreatePixelShader made the object */

/* ---- entry ------------------------------------------------------------------------------------------ */
/* Creates the Metal IDirect3D9 (one reference); the guest address, or 0 if Metal is unavailable.
 * SPORE_D3D9=metal selects it (runtime/d3d9null's Direct3DCreate9). */
uint32_t d3d9metal_create(sg_ctx *ctx, uint8_t *mem, uint32_t sdk_version);

/* ---- notes for the pipeline and the selection in runtime/d3d9null --------------------------------------
 *  - DrawPrimitiveUP / DrawIndexedPrimitiveUP: D3D9 leaves stream 0 (and, for the indexed one, the index
 *    buffer) unset afterwards. src/'s stubs do it (and release a binding the guest had already freed:
 *    see "zombie"); an override should do the same through the stored state, e.g. by calling
 *    mt_device_handler(slot) after its draw, or by clearing st.streams[0] / st.ib and setting the dirty bits.
 *  - The draw statistics the null suite checks (d3d9null_stats_ptr()->draws / ->primitives) are counted by
 *    src/'s stubs; an override should count them too (runtime/d3d9null/tests/test_d3d9.c lines 337-338).
 *  - Objects the guest releases while bound are kept (mt_obj.zombie = 1) until the binding changes, as D3D9's
 *    internal reference does; the guest record is already retired, the Metal objects stay valid. Bindings in
 *    mt_state are therefore always live objects.
 *  - mt_texture_of() and mt_buffer_of() may end the render pass (a staged upload or mipmap generation is a
 *    blit): call them for every resource of a draw before mt_pass_encoder(). mt_scratch() never ends it.
 *  - Depth formats: D24S8/D24FS8/D24X4S4/D15S1 are Depth32Float_Stencil8, D24X8/D32/D32F_LOCKABLE are
 *    Depth32Float, D16 is Depth16Unorm (mt_format_of; MT_FMT_DEPTH). A depth texture the game samples (raster
 *    type 10, CreateTexture with D3DUSAGE_DEPTHSTENCIL) is a shadow map: on the NVIDIA profile D3D9 compares
 *    (hardware PCF), so its sampler needs a compare function (shader/ and pipeline/).
 *  - Occlusion queries are src/'s: a pass begun while one is active gets the visibility buffer and its own
 *    counting slot (mt_occlusion_pass_begun); the pipeline does nothing for them.
 *  - The format view (mt_obj.view) carries the D3D9 sampling swizzle (X8 alpha 1, L8 rrr1, A8L8 rrrg, G16R16
 *    rg11, R16F/R32F r111, V8U8 uv11); render into mt_obj.tex, sample mt_texture_of().
 *  - Selection: runtime/d3d9null's Direct3DCreate9 calls d3d9metal_create (weak there) for SPORE_D3D9=metal
 *    or unset.
 */

#ifdef __cplusplus
}
#endif
#endif
