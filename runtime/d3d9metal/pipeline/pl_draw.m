/* pl_draw.m -- pipeline state and draws of the Metal back end (docs: runtime/d3d9metal/pipeline/pl_int.h).
 *
 * The device layer (src/) stores every D3D9 setter into mt_state (d3d9metal_int.h); this file turns that state into Metal at draw
 * time and implements the four draw calls and Clear (the slots mt_pipeline_handler hands to the device vtable):
 *
 *   render pipeline states  cached by pl_pso_key: vertex shader, pixel shader (or the fixed-function stand-in),
 *                           vertex declaration + the strides, step rates and null streams it is fed from,
 *                           blend state, colour write masks, the attachment formats (colour x4, depth, stencil),
 *                           sample count and the alpha-test function (a function constant of the fragment)
 *   depth-stencil states    cached by a packed 64-bit key
 *   sampler states          cached by a packed 64-bit key (address, filters, anisotropy, border class, max mip)
 *   per-encoder state       viewport (+0.5 px: D3D9 pixel centres), scissor, cull, fill, depth bias, stencil
 *                           reference, blend colour, buffers, textures, samplers, constants -- set only when they
 *                           change; everything is re-sent when the device layer opens a new encoder
 *   constants               setVertexBytes/setFragmentBytes of the range the shader reads (MT_BUF_CONST_F/I/B)
 *                           plus the driver block (MT_BUF_DRIVER)
 *   topology                triangle fans become indexed lists (scratch index buffer); indexed strips whose index
 *                           range holds Metal's restart value (0xFFFF / 0xFFFFFFFF) are re-issued with 32-bit
 *                           indices, because Metal always restarts strips there and D3D9 has no restart
 *   UP draws                vertex and index data copied into the device's per-frame scratch ring; afterwards stream 0
 *                           and (for DrawIndexedPrimitiveUP) the indices are unset, as D3D9 does
 *   Clear                   a whole-target clear becomes the next pass's load action (mt_pass_clear); one limited
 *                           by the viewport, scissor or rectangles is a quad draw with its own pipeline
 */
#import <Metal/Metal.h>
#include <math.h>
#include <stdio.h>
#include <stdlib.h>
#include <string.h>

#include "d3d9metal_int.h"
#include "d3d9_slots.h"
#include "pl_int.h"
#include "pl_shader.h"
#include "../shader/d3d9msl.h"

/* The numeric Metal values pl_maps.c returns. */
_Static_assert(MTLBlendFactorBlendColor == 11 && MTLBlendFactorOneMinusSource1Color == 16, "MTLBlendFactor");
_Static_assert(MTLBlendOperationMax == 4, "MTLBlendOperation");
_Static_assert(MTLColorWriteMaskRed == 8 && MTLColorWriteMaskAlpha == 1, "MTLColorWriteMask");
_Static_assert(MTLCompareFunctionAlways == 7 && MTLCompareFunctionLessEqual == 3, "MTLCompareFunction");
_Static_assert(MTLStencilOperationDecrementWrap == 7, "MTLStencilOperation");
_Static_assert(MTLCullModeFront == 1 && MTLCullModeBack == 2, "MTLCullMode");
_Static_assert(MTLTriangleFillModeLines == 1, "MTLTriangleFillMode");
_Static_assert(MTLSamplerAddressModeRepeat == 2 && MTLSamplerAddressModeClampToBorderColor == 5 &&
               MTLSamplerAddressModeMirrorClampToEdge == 1, "MTLSamplerAddressMode");
_Static_assert(MTLSamplerMipFilterLinear == 2, "MTLSamplerMipFilter");
_Static_assert(MTLSamplerBorderColorOpaqueWhite == 2, "MTLSamplerBorderColor");
_Static_assert(MTLVertexFormatUChar4Normalized_BGRA == 42 && MTLVertexFormatFloat4 == 31 && MTLVertexFormatHalf4 == 27 &&
               MTLVertexFormatShort2Normalized == 22 && MTLVertexFormatUShort4Normalized == 21 &&
               MTLVertexFormatInt1010102Normalized == 40, "MTLVertexFormat");
_Static_assert(MTLPrimitiveTypeTriangleStrip == 4, "MTLPrimitiveType");
_Static_assert(D3D9MSL_BUF_CONST_F == MT_BUF_CONST_F && D3D9MSL_BUF_CONST_I == MT_BUF_CONST_I &&
               D3D9MSL_BUF_CONST_B == MT_BUF_CONST_B && D3D9MSL_BUF_AUX == MT_BUF_DRIVER &&
               D3D9MSL_BUF_STREAM0 == MT_BUF_STREAM0, "shader and device binding layouts agree");
_Static_assert(sizeof(d3d9msl_aux) == 592, "SgAux");

#define PL_DUMMY_VB 30          /* buffer index of the zero buffer that feeds unfed vertex inputs */
#ifndef PL_HALFPIXEL_VIEWPORT
#define PL_HALFPIXEL_VIEWPORT 1 /* D3D9 pixel centres: shift the viewport by +0.5 px (D3D9 rasterises at pixel centres); 0 if the translated shaders do it */
#endif

/* ---- statistics -------------------------------------------------------------------------------------- */
typedef struct pl_stats {
    uint64_t draws, prims, skipped, pso_new, pso_fail, dss_new, smp_new, fans, restart_promotions, up_bytes;
    uint64_t clears_load, clears_quad, encoders, const_bytes;
} pl_stats;
static pl_stats g_st;
/* SPORE_METAL_RESTART_FIX=0 turns off the 0xFFFF strip promotion (diagnosis only: Metal then cuts the strip) */
static int restart_fix(void) {
    static int v = -1;
    if (v < 0) { const char *e = getenv("SPORE_METAL_RESTART_FIX"); v = !(e && e[0] == '0'); }
    return v;
}

/* ---- a small open-addressing map: fixed-size keys -> retained Metal object ---------------------------- */
typedef struct pl_map {
    uint32_t cap, n, klen;
    uint8_t *keys;
    uint64_t *hash;
    void **val;              /* +1 objects, or (void *)1 for "failed, do not retry" */
} pl_map;

static uint64_t fnv(const void *p, size_t n) {
    const uint8_t *s = (const uint8_t *)p;
    uint64_t h = 1469598103934665603ull;
    for (size_t i = 0; i < n; i++) { h ^= s[i]; h *= 1099511628211ull; }
    return h | 1;            /* 0 marks an empty slot */
}
static void map_init(pl_map *m, uint32_t klen) {
    memset(m, 0, sizeof *m);
    m->klen = klen;
}
static void **map_slot(pl_map *m, const void *k, uint64_t h, int insert);
static void map_grow(pl_map *m) {
    pl_map o = *m;
    m->cap = o.cap ? o.cap * 2 : 64;
    m->n = 0;
    m->keys = calloc(m->cap, m->klen);
    m->hash = calloc(m->cap, sizeof(uint64_t));
    m->val = calloc(m->cap, sizeof(void *));
    for (uint32_t i = 0; i < o.cap; i++)
        if (o.hash[i]) *map_slot(m, o.keys + (size_t)i * o.klen, o.hash[i], 1) = o.val[i];
    free(o.keys); free(o.hash); free(o.val);
}
static void **map_slot(pl_map *m, const void *k, uint64_t h, int insert) {
    if (insert && (m->n + 1) * 4 > m->cap * 3) map_grow(m);
    if (!m->cap) return NULL;
    for (uint32_t i = (uint32_t)h & (m->cap - 1);; i = (i + 1) & (m->cap - 1)) {
        if (!m->hash[i]) {
            if (!insert) return NULL;
            m->hash[i] = h;
            memcpy(m->keys + (size_t)i * m->klen, k, m->klen);
            m->n++;
            return &m->val[i];
        }
        if (m->hash[i] == h && !memcmp(m->keys + (size_t)i * m->klen, k, m->klen)) return &m->val[i];
    }
}
static void map_free(pl_map *m) {
    for (uint32_t i = 0; i < m->cap; i++)
        if (m->hash[i] && m->val[i] && m->val[i] != (void *)1) [(id)m->val[i] release];
    free(m->keys); free(m->hash); free(m->val);
    memset(m, 0, sizeof *m);
}
/* Removes every entry whose key holds the 64-bit id at byte offset `off` (a freed shader or declaration). */
static void map_drop_id(pl_map *m, size_t off, uint64_t oid, int nids) {
    if (!m->cap) return;
    int any = 0;
    for (uint32_t i = 0; i < m->cap; i++) {
        if (!m->hash[i]) continue;
        const uint8_t *k = m->keys + (size_t)i * m->klen;
        for (int j = 0; j < nids; j++) {
            uint64_t v;
            memcpy(&v, k + off + 8 * j, 8);
            if (v == oid) { any = 1; break; }
        }
    }
    if (!any) return;
    pl_map o = *m;
    map_init(m, o.klen);
    for (uint32_t i = 0; i < o.cap; i++) {
        if (!o.hash[i]) continue;
        const uint8_t *k = o.keys + (size_t)i * o.klen;
        int hit = 0;
        for (int j = 0; j < nids; j++) {
            uint64_t v;
            memcpy(&v, k + off + 8 * j, 8);
            if (v == oid) hit = 1;
        }
        if (hit) { if (o.val[i] && o.val[i] != (void *)1) [(id)o.val[i] release]; }
        else *map_slot(m, k, o.hash[i], 1) = o.val[i];
    }
    free(o.keys); free(o.hash); free(o.val);
}

/* ---- keys ------------------------------------------------------------------------------------------- */
typedef struct pl_pso_key {
    uint64_t vs, ps, decl;            /* mt_obj.uid (never reused); ps 0 = fixed-function stand-in */
    uint32_t fmt[MT_NRT];             /* MTLPixelFormat of each colour attachment (0 = none) */
    uint32_t depth_fmt, stencil_fmt;
    uint16_t stride[MT_NSTREAMS];     /* stride of each stream the declaration reads */
    uint8_t rate[MT_NSTREAMS];        /* 0 per vertex, n per instance (INSTANCEDATA | n) */
    uint16_t null_streams;            /* declaration streams with no buffer (fed from the zero buffer) */
    uint8_t samples, blend, sep;
    uint8_t src, dst, op, srca, dsta, opa;
    uint8_t wmask[MT_NRT];
    uint8_t alpha_func;               /* 8 = off */
    uint8_t ff;                       /* fixed-function stand-in variant (ps == 0) */
    uint8_t pad[4];
} pl_pso_key;

typedef struct pl_dev {
    pl_map pso, dss, smp, clr;
    pl_pso_key last_key;
    id<MTLRenderPipelineState> last_pso;    /* +0 (owned by the map) */
    id<MTLLibrary> util;                    /* clear + fixed-function stand-ins */
    id<MTLBuffer> zero;                     /* 256 zero bytes: unfed vertex inputs */
    id<MTLTexture> dummy[5];                /* 1x1 (0,0,0,1) 2D, cube, volume (index PL_ST_*) */
    /* the encoder the cached bindings below belong to (+1, so its address cannot be reused while cached) */
    id<MTLRenderCommandEncoder> enc;
    id<MTLRenderPipelineState> b_pso;
    id<MTLDepthStencilState> b_dss;
    int b_cull, b_fill;
    float b_bias[3];
    uint32_t b_sref[2];
    float b_bc[4];
    MTLViewport b_vp;
    MTLScissorRect b_sc;
    id b_vbuf[32];
    uint32_t b_voff[32];
    id b_ftex[16], b_vtex[4], b_fsmp[16], b_vsmp[4];
    uint64_t b_vs_id, b_ps_id;              /* shaders the constants were uploaded for */
    int b_vs_nf, b_ps_nf;
    d3d9msl_aux b_aux[2];                   /* last driver block sent: [0] vertex, [1] fragment */
    int b_aux_ok[2];
    int ff_warned;
} pl_dev;

static pl_dev *P(mt_dev *d) { return (pl_dev *)d->pl; }

static void log_once(int *flag, const char *what) {
    if (*flag) return;
    *flag = 1;
    MT_LOG("pipeline: %s\n", what);
}
static int w_novs, w_nodecl, w_link, w_srgbw, w_dtype, w_stride, w_vrange, w_point, w_wrap, w_clip;
static unsigned n_linkmiss;

/* ---- utility shaders (clear quad, fixed-function pixel stand-ins) ------------------------------------- */
static const char *k_util_msl =
    "#include <metal_stdlib>\n"
    "using namespace metal;\n"
    "struct ClrO { float4 p [[position]]; };\n"
    "vertex ClrO pl_clear_vs(uint vid [[vertex_id]], constant float4 &z [[buffer(0)]]) {\n"
    "    float2 uv = float2((vid << 1) & 2, vid & 2);\n"
    "    ClrO o; o.p = float4(uv * 2.0f - 1.0f, z.x, 1.0f); return o; }\n"
    "struct C1 { float4 c0 [[color(0)]]; };\n"
    "struct C2 { float4 c0 [[color(0)]]; float4 c1 [[color(1)]]; };\n"
    "struct C3 { float4 c0 [[color(0)]]; float4 c1 [[color(1)]]; float4 c2 [[color(2)]]; };\n"
    "struct C4 { float4 c0 [[color(0)]]; float4 c1 [[color(1)]]; float4 c2 [[color(2)]]; float4 c3 [[color(3)]]; };\n"
    "fragment C1 pl_clear_fs1(constant float4 &c [[buffer(0)]]) { C1 o; o.c0 = c; return o; }\n"
    "fragment C2 pl_clear_fs2(constant float4 &c [[buffer(0)]]) { C2 o; o.c0 = c; o.c1 = c; return o; }\n"
    "fragment C3 pl_clear_fs3(constant float4 &c [[buffer(0)]]) { C3 o; o.c0 = c; o.c1 = c; o.c2 = c; return o; }\n"
    "fragment C4 pl_clear_fs4(constant float4 &c [[buffer(0)]]) { C4 o; o.c0 = c; o.c1 = c; o.c2 = c; o.c3 = c; return o; }\n"
    "fragment void pl_clear_fs0() {}\n"
    /* fixed-function pixel stand-ins (D3D9 with no pixel shader: stage 0 MODULATE of texture and diffuse);
     * variants: with/without color0, with/without a 2D texture on stage 0 */
    "constant uint pl_alpha_func [[function_constant(0)]];\n"
    "constant bool pl_alpha_set = is_function_constant_defined(pl_alpha_func);\n"
    "struct PlAux { float alphaRef; float pad[3]; float4 fogColor; float4 fogParams; float4 rtSize; };\n"
    "static bool pl_alpha(float a, float r) { uint f = pl_alpha_set ? pl_alpha_func : 8u;\n"
    "    float qa = rint(saturate(a) * 255.0f), qr = rint(saturate(r) * 255.0f);\n"
    "    switch (f) { case 1u: return false; case 2u: return qa < qr; case 3u: return qa == qr; case 4u: return qa <= qr;\n"
    "    case 5u: return qa > qr; case 6u: return qa != qr; case 7u: return qa >= qr; default: return true; } }\n"
    "struct FfC { float4 c [[user(color0)]]; };\n"
    "struct FfCT { float4 c [[user(color0)]]; float4 t [[user(texcoord0)]]; };\n"
    "struct FfT { float4 t [[user(texcoord0)]]; };\n"
    "fragment float4 pl_ff_none(constant PlAux &aux [[buffer(3)]]) { float4 r = float4(1.0f); if (!pl_alpha(r.w, aux.alphaRef)) discard_fragment(); return r; }\n"
    "fragment float4 pl_ff_c(FfC i [[stage_in]], constant PlAux &aux [[buffer(3)]]) { float4 r = i.c; if (!pl_alpha(r.w, aux.alphaRef)) discard_fragment(); return r; }\n"
    "fragment float4 pl_ff_t(FfT i [[stage_in]], texture2d<float> t0 [[texture(0)]], sampler s0 [[sampler(0)]], constant PlAux &aux [[buffer(3)]]) {\n"
    "    float4 r = t0.sample(s0, i.t.xy); if (!pl_alpha(r.w, aux.alphaRef)) discard_fragment(); return r; }\n"
    "fragment float4 pl_ff_ct(FfCT i [[stage_in]], texture2d<float> t0 [[texture(0)]], sampler s0 [[sampler(0)]], constant PlAux &aux [[buffer(3)]]) {\n"
    "    float4 r = i.c * t0.sample(s0, i.t.xy); if (!pl_alpha(r.w, aux.alphaRef)) discard_fragment(); return r; }\n";

static id<MTLLibrary> util_lib(mt_dev *d) {
    pl_dev *p = P(d);
    if (!p->util) {
        NSError *e = nil;
        MTLCompileOptions *o = [[MTLCompileOptions alloc] init];
        p->util = [d->mtl newLibraryWithSource:[NSString stringWithUTF8String:k_util_msl] options:o error:&e];
        [o release];
        if (!p->util) MT_LOG("pipeline: utility shaders failed to compile: %s\n", e ? e.localizedDescription.UTF8String : "?");
    }
    return p->util;
}

/* ---- device hooks ------------------------------------------------------------------------------------ */
static id<MTLTexture> make_dummy(mt_dev *d, MTLTextureType type) {
    MTLTextureDescriptor *td = [[MTLTextureDescriptor alloc] init];
    td.textureType = type;
    td.pixelFormat = MTLPixelFormatRGBA8Unorm;
    td.width = td.height = td.depth = 1;
    td.usage = MTLTextureUsageShaderRead;
    td.storageMode = MTLStorageModeShared;
    id<MTLTexture> t = [d->mtl newTextureWithDescriptor:td];
    [td release];
    static const uint8_t black[4] = {0, 0, 0, 255};   /* an unbound sampler reads (0,0,0,1) */
    int slices = type == MTLTextureTypeCube ? 6 : 1;
    for (int s = 0; s < slices; s++)
        [t replaceRegion:MTLRegionMake3D(0, 0, 0, 1, 1, 1) mipmapLevel:0 slice:s withBytes:black bytesPerRow:4 bytesPerImage:4];
    return t;
}

void mt_pl_device_created(mt_dev *d) {
    if (d->pl) return;
    pl_dev *p = calloc(1, sizeof *p);
    d->pl = p;
    map_init(&p->pso, sizeof(pl_pso_key));
    map_init(&p->dss, 8);
    map_init(&p->smp, 8);
    map_init(&p->clr, 32);
    p->zero = [d->mtl newBufferWithLength:256 options:MTLResourceStorageModeShared];
    memset(p->zero.contents, 0, 256);
    p->dummy[PL_ST_2D] = make_dummy(d, MTLTextureType2D);
    p->dummy[PL_ST_CUBE] = make_dummy(d, MTLTextureTypeCube);
    p->dummy[PL_ST_VOLUME] = make_dummy(d, MTLTextureType3D);
}

static void enc_forget(pl_dev *p) {
    if (p->enc) [p->enc release];
    p->enc = nil;
    p->b_pso = nil; p->b_dss = nil;
    p->b_cull = p->b_fill = -1;
    memset(p->b_vbuf, 0, sizeof p->b_vbuf);
    memset(p->b_ftex, 0, sizeof p->b_ftex); memset(p->b_vtex, 0, sizeof p->b_vtex);
    memset(p->b_fsmp, 0, sizeof p->b_fsmp); memset(p->b_vsmp, 0, sizeof p->b_vsmp);
    p->b_vs_id = p->b_ps_id = 0;
    p->b_aux_ok[0] = p->b_aux_ok[1] = 0;
    memset(&p->b_vp, 0, sizeof p->b_vp); p->b_vp.width = -1;
    memset(&p->b_sc, 0, sizeof p->b_sc); p->b_sc.width = (NSUInteger)-1;
    p->b_bias[0] = NAN; p->b_sref[0] = p->b_sref[1] = 0xffffffffu; p->b_bc[0] = NAN;
}

void mt_pl_device_reset(mt_dev *d) {
    pl_dev *p = P(d);
    if (!p) return;
    enc_forget(p);
}

void mt_pl_device_destroyed(mt_dev *d) {
    pl_dev *p = P(d);
    if (!p) return;
    pl_pso_archive_save(0);
    enc_forget(p);
    map_free(&p->pso); map_free(&p->dss); map_free(&p->smp); map_free(&p->clr);
    [p->util release]; [p->zero release];
    for (int i = 0; i < 5; i++) [p->dummy[i] release];
    free(p);
    d->pl = NULL;
}

/* A sampling texture's sRGB view, kept in the texture object's private slot. */
typedef struct pl_texpriv { id<MTLTexture> base, srgb; } pl_texpriv;

void mt_pl_object_freed(mt_obj *o) {
    mt_dev *d = o->dev;
    pl_dev *p = d ? P(d) : NULL;
    if ((o->kind == MT_K_VS || o->kind == MT_K_PS) && o->pl) pl_sh_free(o);
    if ((o->kind == MT_K_TEX || o->kind == MT_K_CUBE || o->kind == MT_K_VOLTEX) && o->pl) {
        pl_texpriv *t = (pl_texpriv *)o->pl;
        [t->base release]; [t->srgb release];
        free(t);
        o->pl = NULL;
    }
    if (p && (o->kind == MT_K_VS || o->kind == MT_K_PS || o->kind == MT_K_VDECL)) {
        /* drop the pipelines built from it: the ids are never reused, so this only frees memory */
        map_drop_id(&p->pso, offsetof(pl_pso_key, vs), o->uid, 3);
        p->last_pso = nil;
        memset(&p->last_key, 0, sizeof p->last_key);
    }
}

void mt_pl_frame_end(mt_dev *d) {
    pl_dev *p = P(d);
    if (p) enc_forget(p);       /* the frame's encoders are gone */
    pl_sh_frame_end();          /* shaders created in this frame form their batches */
}

/* CreateVertexShader/CreatePixelShader (mt_d3d9.mm) */
void mt_pl_shader_created(mt_obj *o) {
    if (o && o->dev) pl_sh_created(o->dev, o);
}

/* ---- state -> Metal objects ------------------------------------------------------------------------- */
static uint32_t fmt_of(id<MTLTexture> t) { return t ? (uint32_t)t.pixelFormat : 0; }
static int fmt_no_alpha(mt_obj *surf) { return surf && surf->mf && (surf->mf->flags & MT_FMT_ALPHA1); }
static int fmt_has_stencil(uint32_t f) {
    return f == MTLPixelFormatDepth32Float_Stencil8 || f == MTLPixelFormatStencil8 ||
           f == MTLPixelFormatX32_Stencil8
#if !TARGET_OS_IPHONE
           || f == MTLPixelFormatDepth24Unorm_Stencil8 || f == MTLPixelFormatX24_Stencil8
#endif
        ;
}
static int fmt_has_depth(uint32_t f) {
    return f == MTLPixelFormatDepth32Float_Stencil8 || f == MTLPixelFormatDepth32Float || f == MTLPixelFormatDepth16Unorm
#if !TARGET_OS_IPHONE
           || f == MTLPixelFormatDepth24Unorm_Stencil8
#endif
        ;
}

static id<MTLDepthStencilState> get_dss(mt_dev *d, int has_depth, int has_stencil) {
    const uint32_t *rs = d->st.rs;
    uint64_t k = 0;
    int zen = has_depth && rs[PL_RS_ZENABLE] != 0;
    if (zen) k |= 1ull | ((uint64_t)(rs[PL_RS_ZWRITEENABLE] != 0) << 1) | ((uint64_t)pl_compare(rs[PL_RS_ZFUNC]) << 2);
    if (has_stencil && rs[PL_RS_STENCILENABLE]) {
        int two = rs[PL_RS_TWOSIDEDSTENCILMODE] != 0;
        k |= 1ull << 5;
        k |= (uint64_t)pl_stencil_op(rs[PL_RS_STENCILFAIL]) << 6 | (uint64_t)pl_stencil_op(rs[PL_RS_STENCILZFAIL]) << 9 |
             (uint64_t)pl_stencil_op(rs[PL_RS_STENCILPASS]) << 12 | (uint64_t)pl_compare(rs[PL_RS_STENCILFUNC]) << 15;
        uint32_t bf = two ? rs[PL_RS_CCW_STENCILFAIL] : rs[PL_RS_STENCILFAIL];
        uint32_t bz = two ? rs[PL_RS_CCW_STENCILZFAIL] : rs[PL_RS_STENCILZFAIL];
        uint32_t bp = two ? rs[PL_RS_CCW_STENCILPASS] : rs[PL_RS_STENCILPASS];
        uint32_t bc = two ? rs[PL_RS_CCW_STENCILFUNC] : rs[PL_RS_STENCILFUNC];
        k |= (uint64_t)pl_stencil_op(bf) << 18 | (uint64_t)pl_stencil_op(bz) << 21 | (uint64_t)pl_stencil_op(bp) << 24 |
             (uint64_t)pl_compare(bc) << 27;
        k |= (uint64_t)(rs[PL_RS_STENCILMASK] & 0xff) << 32 | (uint64_t)(rs[PL_RS_STENCILWRITEMASK] & 0xff) << 40;
    }
    pl_dev *p = P(d);
    void **slot = map_slot(&p->dss, &k, fnv(&k, 8), 1);
    if (!*slot) {
        MTLDepthStencilDescriptor *dd = [[MTLDepthStencilDescriptor alloc] init];
        if (k & 1) {
            dd.depthCompareFunction = (MTLCompareFunction)((k >> 2) & 7);
            dd.depthWriteEnabled = (k >> 1) & 1;
        } else {
            dd.depthCompareFunction = MTLCompareFunctionAlways;
            dd.depthWriteEnabled = NO;
        }
        if (k & (1ull << 5)) {
            MTLStencilDescriptor *f = [[MTLStencilDescriptor alloc] init], *b = [[MTLStencilDescriptor alloc] init];
            f.stencilFailureOperation = (MTLStencilOperation)((k >> 6) & 7);
            f.depthFailureOperation = (MTLStencilOperation)((k >> 9) & 7);
            f.depthStencilPassOperation = (MTLStencilOperation)((k >> 12) & 7);
            f.stencilCompareFunction = (MTLCompareFunction)((k >> 15) & 7);
            b.stencilFailureOperation = (MTLStencilOperation)((k >> 18) & 7);
            b.depthFailureOperation = (MTLStencilOperation)((k >> 21) & 7);
            b.depthStencilPassOperation = (MTLStencilOperation)((k >> 24) & 7);
            b.stencilCompareFunction = (MTLCompareFunction)((k >> 27) & 7);
            f.readMask = b.readMask = (uint32_t)(k >> 32) & 0xff;
            f.writeMask = b.writeMask = (uint32_t)(k >> 40) & 0xff;
            dd.frontFaceStencil = f;   /* front = clockwise = D3D9's normal stencil state */
            dd.backFaceStencil = b;    /* counter-clockwise: the CCW_* states in two-sided mode */
            [f release]; [b release];
        }
        *slot = [d->mtl newDepthStencilStateWithDescriptor:dd];
        [dd release];
        g_st.dss_new++;
    }
    return (id<MTLDepthStencilState>)*slot;
}

/* The Metal sampler for guest sampler s; `info` receives what Metal samplers cannot hold (the translator's SgSampler:
 * a border colour Metal cannot express, the LOD bias). */
/* D3D9-era hardware filters a cube map inside one face (no blending across face edges). Metal filters cube
 * maps seamlessly, and the default stays seamless (tests/conformance.cpp case cube.face_edge reports
 * the blended value). SPORE_METAL_CUBE_FACE_FILTER=1 filters per face as D3D9 does, through Metal's private sampler
 * property forceSeamsOnCubemapFiltering (not in the public headers, so set only when the descriptor answers to it,
 * else logged once), with the cube's address modes forced to CLAMP, since per-face filtering clamps at the face
 * edge. */
static int cube_face_filter(void) {
    static int v = -1;
    if (v < 0) { const char *e = getenv("SPORE_METAL_CUBE_FACE_FILTER"); v = e && e[0] == '1'; }
    return v;
}
static id<MTLSamplerState> get_sampler(mt_dev *d, unsigned s, d3d9msl_sampler *info) {
    const uint32_t *ss = d->st.ss[s];
    int cube = cube_face_filter() && s < MT_NSAMPLERS && d->st.tex[s] && d->st.tex[s]->kind == MT_K_CUBE;
    uint32_t maxa = ss[PL_SAMP_MAXANISOTROPY];
    /* anisotropy only with an ANISOTROPIC MIN filter, a mip filter, and a texture that has more
     * than one mip level; anything else gets one bilinear tap. The other rule (anisotropy whenever MIN or MAG is
     * ANISOTROPIC) would catch the game's post-process passes, which set ANISOTROPIC (RenderWare's flush,
     * 0x11f1a30) with MAXANISOTROPY 8 on single-level render targets, where an anisotropic tap gathers more of a minified source than the single
     * bilinear tap D3D9 drivers take (tests/conformance.cpp case sampler.aniso_single_level).
     * SPORE_METAL_ANISO_RULE=old selects that other rule. */
    static int old_rule = -1;
    if (old_rule < 0) { const char *e = getenv("SPORE_METAL_ANISO_RULE"); old_rule = e && (!strcmp(e, "old") || !strcmp(e, "wave8")); }
    mt_obj *bt = s < MT_NSAMPLERS ? d->st.tex[s] : NULL;
    int multimip = bt && bt->tex && bt->tex.mipmapLevelCount > 1;
    int want_aniso = old_rule ? (ss[PL_SAMP_MINFILTER] == 3 || ss[PL_SAMP_MAGFILTER] == 3)
                              : (ss[PL_SAMP_MINFILTER] == 3 && ss[PL_SAMP_MIPFILTER] != 0 && multimip);
    int aniso = want_aniso ? (int)(maxa < 1 ? 1 : maxa > 16 ? 16 : maxa) : 1;
    int exact = 1;
    uint32_t border = 0, au = ss[PL_SAMP_ADDRESSU], av = ss[PL_SAMP_ADDRESSV], aw = ss[PL_SAMP_ADDRESSW];
    if (cube) au = av = aw = 3;   /* CLAMP for per-face filtering (Metal ignores a cube map's address mode in either filtering mode) */
    if (au == 4 || av == 4 || aw == 4) {
        border = pl_border(ss[PL_SAMP_BORDERCOLOR], &exact);
        if (!exact) {
            /* the shader substitutes the colour outside [0,1]; the Metal sampler clamps to the edge there */
            pl_argb_to_float(ss[PL_SAMP_BORDERCOLOR], info->border);
            if (au == 4) { info->flags |= D3D9MSL_SAMP_BORDER_U; au = 3; }
            if (av == 4) { info->flags |= D3D9MSL_SAMP_BORDER_V; av = 3; }
            if (aw == 4) { info->flags |= D3D9MSL_SAMP_BORDER_W; aw = 3; }
            border = 0;
        }
    }
    memcpy(&info->mip_bias, &ss[PL_SAMP_MIPMAPLODBIAS], 4);
    uint32_t maxmip = ss[PL_SAMP_MAXMIPLEVEL];
    /* IDirect3DBaseTexture9::SetLOD (MANAGED textures; the game calls it ~1,000 times in a Creature Creator
     * session) also limits the most detailed level sampled. The device layer stores it (t_set_lod). It acts as the base level of the sampled view, so here it joins MAXMIPLEVEL in the
     * sampler's lodMinClamp: the larger of the two wins, as on D3D9. */
    if (s < MT_NSAMPLERS && d->st.tex[s] && d->st.tex[s]->lod > maxmip) maxmip = d->st.tex[s]->lod;
    if (maxmip > 15) maxmip = 15;
    uint64_t k = (uint64_t)pl_address(au) | (uint64_t)pl_address(av) << 3 |
                 (uint64_t)pl_address(aw) << 6 | (uint64_t)pl_minmag(ss[PL_SAMP_MAGFILTER]) << 9 |
                 (uint64_t)pl_minmag(ss[PL_SAMP_MINFILTER]) << 10 | (uint64_t)pl_mip(ss[PL_SAMP_MIPFILTER]) << 11 |
                 (uint64_t)aniso << 13 | (uint64_t)border << 18 | (uint64_t)maxmip << 20 | (uint64_t)cube << 24;
    pl_dev *p = P(d);
    void **slot = map_slot(&p->smp, &k, fnv(&k, 8), 1);
    if (!*slot) {
        MTLSamplerDescriptor *sd = [[MTLSamplerDescriptor alloc] init];
        sd.sAddressMode = (MTLSamplerAddressMode)(k & 7);
        sd.tAddressMode = (MTLSamplerAddressMode)((k >> 3) & 7);
        sd.rAddressMode = (MTLSamplerAddressMode)((k >> 6) & 7);
        sd.magFilter = (MTLSamplerMinMagFilter)((k >> 9) & 1);
        sd.minFilter = (MTLSamplerMinMagFilter)((k >> 10) & 1);
        sd.mipFilter = (MTLSamplerMipFilter)((k >> 11) & 3);
        sd.maxAnisotropy = (NSUInteger)((k >> 13) & 31);
        sd.borderColor = (MTLSamplerBorderColor)((k >> 18) & 3);
        sd.lodMinClamp = (float)((k >> 20) & 15);       /* D3DSAMP_MAXMIPLEVEL: the largest (most detailed) level used */
        sd.normalizedCoordinates = YES;
        if ((k >> 24) & 1) {
            static int warned;
            if ([sd respondsToSelector:@selector(setForceSeamsOnCubemapFiltering:)]) [sd setValue:@YES forKey:@"forceSeamsOnCubemapFiltering"];
            else if (!warned++) MT_LOG("pipeline: this Metal has no per-face cube filtering; cube maps are filtered seamlessly\n");
        }
        *slot = [d->mtl newSamplerStateWithDescriptor:sd];
        [sd release];
        g_st.smp_new++;
    }
    return (id<MTLSamplerState>)*slot;
}

/* The texture to bind for sampler s (pixel 0..15, or MT_SAMPLER_VTX0 + k) given the type the shader declares. */
static id<MTLTexture> resolve_texture(mt_dev *d, unsigned s, int want, mt_obj **used, d3d9msl_sampler *info) {
    pl_dev *p = P(d);
    mt_obj *o = d->st.tex[s];
    *used = NULL;
    if (want < PL_ST_2D || want > PL_ST_VOLUME) want = PL_ST_2D;
    if (!o) return p->dummy[want];
    id<MTLTexture> t = mt_texture_of(d, o);
    if (!t) return p->dummy[want];
    MTLTextureType tt = t.textureType;
    int have = tt == MTLTextureTypeCube ? PL_ST_CUBE : tt == MTLTextureType3D ? PL_ST_VOLUME : PL_ST_2D;
    if (have != want) return p->dummy[want];         /* a mismatched bind would fault on the GPU */
    *used = o;
    if (o->mf && (o->mf->flags & MT_FMT_DEPTH)) info->flags |= D3D9MSL_SAMP_DEPTH_CMP;  /* shadow-map compare */
    if (d->st.ss[s][PL_SAMP_SRGBTEXTURE]) {
        MTLPixelFormat sf = MTLPixelFormatInvalid;
        switch (t.pixelFormat) {
        case MTLPixelFormatBGRA8Unorm: sf = MTLPixelFormatBGRA8Unorm_sRGB; break;
        case MTLPixelFormatRGBA8Unorm: sf = MTLPixelFormatRGBA8Unorm_sRGB; break;
        case MTLPixelFormatBC1_RGBA: sf = MTLPixelFormatBC1_RGBA_sRGB; break;
        case MTLPixelFormatBC2_RGBA: sf = MTLPixelFormatBC2_RGBA_sRGB; break;
        case MTLPixelFormatBC3_RGBA: sf = MTLPixelFormatBC3_RGBA_sRGB; break;
        default: break;
        }
        if (sf != MTLPixelFormatInvalid && (t.usage & MTLTextureUsagePixelFormatView)) {
            pl_texpriv *tp = (pl_texpriv *)o->pl;
            if (!tp) { tp = calloc(1, sizeof *tp); o->pl = tp; }
            if (tp->base != t) {
                [tp->base release]; [tp->srgb release];
                tp->base = [t retain];
                tp->srgb = [t newTextureViewWithPixelFormat:sf];
            }
            if (tp->srgb) return tp->srgb;
        }
        info->flags |= D3D9MSL_SAMP_SRGB;    /* no sRGB view: the shader decodes after filtering */
    }
    return t;
}

/* ---- vertex descriptor ------------------------------------------------------------------------------- */
typedef struct pl_elem { uint16_t stream, offset; uint8_t type, method, usage, index; } pl_elem;

/* The declaration element feeding (usage, index), or NULL. */
static const pl_elem *find_elem(mt_obj *decl, uint8_t usage, uint8_t index) {
    const pl_elem *e = (const pl_elem *)decl->decl;
    for (uint32_t i = 0; i < decl->decl_count; i++)
        if (e[i].usage == usage && e[i].index == index && e[i].type != PL_DT_UNUSED) return &e[i];
    /* D3D9 matches POSITION to POSITIONT for a programmable shader's dcl_position */
    if (usage == PL_U_POSITION && index == 0)
        for (uint32_t i = 0; i < decl->decl_count; i++)
            if (e[i].usage == PL_U_POSITIONT && e[i].index == 0) return &e[i];
    return NULL;
}

/* SPORE_METAL_DECL_CENSUS=1: log each (D3DDECLTYPE, usage, usage index) the first time a pipeline uses it --
 * the census of declaration types the game really builds (RenderWare copies them from asset data, 0x11f2c30). */
static void decl_census(mt_obj *decl) {
    static int on = -1;
    static uint8_t seen[18][14][16];
    if (on < 0) { const char *e = getenv("SPORE_METAL_DECL_CENSUS"); on = e && e[0] == '1'; }
    if (!on) return;
    const pl_elem *e = (const pl_elem *)decl->decl;
    for (uint32_t i = 0; i < decl->decl_count; i++) {
        if (e[i].type > 17 || e[i].usage > 13 || e[i].index > 15 || seen[e[i].type][e[i].usage][e[i].index]) continue;
        seen[e[i].type][e[i].usage][e[i].index] = 1;
        uint32_t sz, vf = pl_vertex_format(e[i].type, &sz);
        MT_LOG("decl census: type %u usage %u index %u -> MTLVertexFormat %u\n", e[i].type, e[i].usage, e[i].index, vf);
    }
}

static MTLVertexDescriptor *build_vdesc(mt_dev *d, const pl_pso_key *k, pl_sh *vs, mt_obj *decl) {
    decl_census(decl);
    MTLVertexDescriptor *v = [MTLVertexDescriptor vertexDescriptor];
    int dummy = 0;
    uint32_t used = 0;
    for (unsigned i = 0; i < vs->nin; i++) {
        unsigned reg = vs->in[i].reg;
        const pl_elem *e = find_elem(decl, vs->in[i].usage, vs->in[i].index);
        uint32_t size = 0, vf = e ? pl_vertex_format(e->type, &size) : 0;
        if (e && !vf) log_once(&w_dtype, "a declaration type with no Metal vertex format (UDEC3/DEC3N): fed zeros");
        if (!e || !vf || e->stream >= MT_NSTREAMS || (k->null_streams >> e->stream & 1)) {
            v.attributes[reg].format = MTLVertexFormatFloat4;
            v.attributes[reg].offset = 0;
            v.attributes[reg].bufferIndex = PL_DUMMY_VB;
            dummy = 1;
            continue;
        }
        v.attributes[reg].format = (MTLVertexFormat)vf;
        v.attributes[reg].offset = e->offset;
        v.attributes[reg].bufferIndex = MT_BUF_STREAM(e->stream);
        used |= 1u << e->stream;
    }
    for (unsigned s = 0; s < MT_NSTREAMS; s++) {
        if (!(used >> s & 1)) continue;
        MTLVertexBufferLayoutDescriptor *l = v.layouts[MT_BUF_STREAM(s)];
        if (k->stride[s] == 0) {
            /* stride 0: every vertex reads the same element; Metal wants a non-zero stride */
            l.stride = 16;
            l.stepFunction = MTLVertexStepFunctionConstant;
            l.stepRate = 0;
        } else {
            if (k->stride[s] & 3) log_once(&w_stride, "a vertex stride that is not a multiple of 4 (Metal requires it)");
            l.stride = k->stride[s];
            if (k->rate[s]) {
                l.stepFunction = MTLVertexStepFunctionPerInstance;
                l.stepRate = k->rate[s];
            }
        }
    }
    if (dummy) {
        MTLVertexBufferLayoutDescriptor *l = v.layouts[PL_DUMMY_VB];
        l.stride = 16;
        l.stepFunction = MTLVertexStepFunctionConstant;
        l.stepRate = 0;
    }
    return v;
}

/* ---- pipeline states ---------------------------------------------------------------------------------- */
static id<MTLRenderPipelineState> get_pso(mt_dev *d, const pl_pso_key *k, pl_sh *vs, pl_sh *ps, mt_obj *decl) {
    pl_dev *p = P(d);
    if (p->last_pso && !memcmp(k, &p->last_key, sizeof *k)) return p->last_pso;
    void **slot = map_slot(&p->pso, k, fnv(k, sizeof *k), 1);
    if (!*slot) {
        @autoreleasepool {
            MTLRenderPipelineDescriptor *pd = [[MTLRenderPipelineDescriptor alloc] init];
            pd.vertexFunction = vs->fn;
            id<MTLFunction> ff = nil;
            if (ps) pd.fragmentFunction = pl_sh_fragment(d, ps, k->alpha_func);
            else {
                static const char *names[4] = {"pl_ff_none", "pl_ff_c", "pl_ff_t", "pl_ff_ct"};
                id<MTLLibrary> u = util_lib(d);
                /* the alpha-test function constant is always given (8 = ALWAYS = off) */
                MTLFunctionConstantValues *cv = [[MTLFunctionConstantValues alloc] init];
                uint32_t f = k->alpha_func;
                [cv setConstantValue:&f type:MTLDataTypeUInt atIndex:0];
                ff = [u newFunctionWithName:[NSString stringWithUTF8String:names[k->ff & 3]] constantValues:cv error:nil];
                [cv release];
                pd.fragmentFunction = ff;
            }
            pd.vertexDescriptor = build_vdesc(d, k, vs, decl);
            if (ps)
                for (unsigned i = 0; i < ps->nout; i++)
                    if (strncmp(ps->out[i], "position", 8) && !pl_sh_writes(vs, ps->out[i]) && n_linkmiss++ < 20)
                        MT_LOG("pipeline: pixel shader input %s is not written by the vertex shader (vs %llx ps %llx)\n",
                               ps->out[i], (unsigned long long)k->vs, (unsigned long long)k->ps);
            for (int i = 0; i < MT_NRT; i++) {
                MTLRenderPipelineColorAttachmentDescriptor *a = pd.colorAttachments[i];
                a.pixelFormat = (MTLPixelFormat)k->fmt[i];
                if (!k->fmt[i]) continue;
                a.writeMask = k->wmask[i];
                if (k->blend) {
                    a.blendingEnabled = YES;
                    a.sourceRGBBlendFactor = (MTLBlendFactor)(k->src >> 1);
                    a.destinationRGBBlendFactor = (MTLBlendFactor)(k->dst >> 1);
                    a.rgbBlendOperation = (MTLBlendOperation)k->op;
                    a.sourceAlphaBlendFactor = (MTLBlendFactor)(k->srca >> 1);
                    a.destinationAlphaBlendFactor = (MTLBlendFactor)(k->dsta >> 1);
                    a.alphaBlendOperation = (MTLBlendOperation)k->opa;
                }
            }
            pd.depthAttachmentPixelFormat = (MTLPixelFormat)k->depth_fmt;
            pd.stencilAttachmentPixelFormat = (MTLPixelFormat)k->stencil_fmt;
            pd.rasterSampleCount = k->samples ? k->samples : 1;
            NSError *e = nil;
            int from_archive = 0;   /* the pipeline archive (pl_shader.m) */
            id<MTLRenderPipelineState> pso = pl_pso_create(d, pd, &from_archive, (void **)&e);
            if (!pso) {
                g_st.pso_fail++;
                if (g_st.pso_fail <= 20)
                    MT_LOG("pipeline: pipeline state failed (vs %llx ps %llx): %s\n", (unsigned long long)k->vs,
                           (unsigned long long)k->ps, e ? e.localizedDescription.UTF8String : "?");
                *slot = (void *)1;
            } else {
                *slot = pso;
                g_st.pso_new++;
            }
            [ff release];
            [pd release];
        }
    }
    if (*slot == (void *)1) return nil;
    p->last_key = *k;
    p->last_pso = (id<MTLRenderPipelineState>)*slot;
    return p->last_pso;
}

/* Blend factors with "destination alpha" read as 1 on a target without alpha. The key stores factor << 1. */
static void fill_blend(mt_dev *d, pl_pso_key *k) {
    const uint32_t *rs = d->st.rs;
    if (!rs[PL_RS_ALPHABLENDENABLE]) return;
    uint32_t src = rs[PL_RS_SRCBLEND], dst = rs[PL_RS_DESTBLEND], srca, dsta;
    pl_blend_both(src, &src, &dst);
    if (rs[PL_RS_SEPARATEALPHABLENDENABLE]) {
        srca = rs[PL_RS_SRCBLENDALPHA]; dsta = rs[PL_RS_DESTBLENDALPHA];
        pl_blend_both(srca, &srca, &dsta);
        k->opa = (uint8_t)pl_blend_op(rs[PL_RS_BLENDOPALPHA]);
    } else {
        srca = src; dsta = dst;
        k->opa = (uint8_t)pl_blend_op(rs[PL_RS_BLENDOP]);
    }
    int noalpha = fmt_no_alpha(d->pass_rt[0]);   /* D3D9 has one blend state; rt0 decides (MRT formats match in practice) */
    k->blend = 1;
    k->sep = rs[PL_RS_SEPARATEALPHABLENDENABLE] != 0;
    k->src = (uint8_t)(pl_blend_factor(src, noalpha) << 1);
    k->dst = (uint8_t)(pl_blend_factor(dst, noalpha) << 1);
    k->op = (uint8_t)pl_blend_op(rs[PL_RS_BLENDOP]);
    k->srca = (uint8_t)(pl_blend_factor(srca, noalpha) << 1);
    k->dsta = (uint8_t)(pl_blend_factor(dsta, noalpha) << 1);
}

/* ---- the draw ------------------------------------------------------------------------------------------ */
typedef struct pl_draw_args {
    uint32_t prim, count, instances, first;     /* MTLPrimitiveType; vertex or index count; first vertex (non-indexed) */
    int indexed;
    id<MTLBuffer> ibuf;
    uint32_t ioff, isz;
    int32_t base_vertex;
    /* UP: stream 0 replaced by scratch data */
    id<MTLBuffer> up_buf;
    uint32_t up_off, up_stride;
    int up;
} pl_draw_args;

static void set_consts(mt_dev *d, id<MTLRenderCommandEncoder> enc, pl_sh *vs, pl_sh *ps, mt_obj *vso, mt_obj *pso_obj) {
    pl_dev *p = P(d);
    mt_state *st = &d->st;
    /* vertex */
    int nf = vs->rel ? MT_VS_NCONSTF : vs->nconst_f;
    if (nf < 1) nf = 1;
    if (nf > MT_VS_NCONSTF) nf = MT_VS_NCONSTF;
    if ((st->dirty & MT_DIRTY_VS_CONST) || p->b_vs_id != vso->uid || p->b_vs_nf != nf) {
        [enc setVertexBytes:st->vsf length:(NSUInteger)nf * 16 atIndex:MT_BUF_CONST_F];
        [enc setVertexBytes:st->vsi length:sizeof st->vsi atIndex:MT_BUF_CONST_I];
        [enc setVertexBytes:st->vsb length:sizeof st->vsb atIndex:MT_BUF_CONST_B];
        g_st.const_bytes += (uint64_t)nf * 16 + sizeof st->vsi + sizeof st->vsb;
        p->b_vs_id = vso->uid;
        p->b_vs_nf = nf;
    }
    /* pixel */
    uint64_t pid = pso_obj ? pso_obj->uid : 0;
    if (ps) {
        int pf = ps->rel ? MT_PS_NCONSTF : ps->nconst_f;
        if (pf < 1) pf = 1;
        if (pf > MT_PS_NCONSTF) pf = MT_PS_NCONSTF;
        if ((st->dirty & MT_DIRTY_PS_CONST) || p->b_ps_id != pid || p->b_ps_nf != pf) {
            [enc setFragmentBytes:st->psf length:(NSUInteger)pf * 16 atIndex:MT_BUF_CONST_F];
            [enc setFragmentBytes:st->psi length:sizeof st->psi atIndex:MT_BUF_CONST_I];
            [enc setFragmentBytes:st->psb length:sizeof st->psb atIndex:MT_BUF_CONST_B];
            g_st.const_bytes += (uint64_t)pf * 16 + sizeof st->psi + sizeof st->psb;
            p->b_ps_id = pid;
            p->b_ps_nf = pf;
        }
    }
    st->dirty &= ~(uint32_t)(MT_DIRTY_VS_CONST | MT_DIRTY_PS_CONST);
}

/* The driver block per stage (the translator's SgAux). samp: per-sampler flags/border/bias computed at bind time. */
static void set_aux(mt_dev *d, id<MTLRenderCommandEncoder> enc, int stage, const d3d9msl_sampler *samp, unsigned nsamp) {
    pl_dev *p = P(d);
    mt_state *st = &d->st;
    d3d9msl_aux a;
    memset(&a, 0, sizeof a);
    a.alpha_ref = (float)(st->rs[PL_RS_ALPHAREF] & 0xff) / 255.0f;
    pl_argb_to_float(st->rs[PL_RS_FOGCOLOR], a.fog_color);
    memcpy(&a.fog_params[0], &st->rs[PL_RS_FOGSTART], 4);
    memcpy(&a.fog_params[1], &st->rs[PL_RS_FOGEND], 4);
    memcpy(&a.fog_params[2], &st->rs[PL_RS_FOGDENSITY], 4);
    a.fog_params[3] = st->rs[PL_RS_FOGENABLE] ? (float)st->rs[PL_RS_FOGTABLEMODE] : 0.0f;
    mt_obj *rt0 = d->pass_rt[0];
    float w = rt0 ? (float)rt0->w : (float)d->pass_w, h = rt0 ? (float)rt0->h : (float)d->pass_h;
    a.rt_size[0] = w; a.rt_size[1] = h; a.rt_size[2] = w > 0 ? 1.0f / w : 0; a.rt_size[3] = h > 0 ? 1.0f / h : 0;
    /* pos_fixup stays 0: the half-pixel shift is in the viewport (PL_HALFPIXEL_VIEWPORT) */
    if (!PL_HALFPIXEL_VIEWPORT && st->vp.w && st->vp.h) { a.pos_fixup[0] = 1.0f / (float)st->vp.w; a.pos_fixup[1] = -1.0f / (float)st->vp.h; }
    if (samp) memcpy(a.samp, samp, sizeof(d3d9msl_sampler) * nsamp);
    if (p->b_aux_ok[stage] && !memcmp(&a, &p->b_aux[stage], sizeof a)) return;
    if (stage == 0) [enc setVertexBytes:&a length:sizeof a atIndex:MT_BUF_DRIVER];
    else [enc setFragmentBytes:&a length:sizeof a atIndex:MT_BUF_DRIVER];
    p->b_aux[stage] = a;
    p->b_aux_ok[stage] = 1;
}

static int g_dry;   /* mt_pl_prepare: stop after the pipeline state (no draw) */

static uint32_t do_draw(mt_dev *d, pl_draw_args *a) {
    pl_dev *p = P(d);
    mt_state *st = &d->st;
    if (!p) return D3D_OK;
    if (!a->count) return D3D_OK;
    mt_obj *vso = st->vs, *pso_obj = st->ps, *decl = st->vdecl;
    if (!vso) { log_once(&w_novs, "a draw with no vertex shader (fixed-function vertex processing) is skipped"); g_st.skipped++; return D3D_OK; }
    if (!decl) { log_once(&w_nodecl, "a draw with no vertex declaration (FVF) is skipped"); g_st.skipped++; return D3D_OK; }
    pl_sh *vs = pl_sh_get(d, vso);
    if (!vs) { g_st.skipped++; return g_dry ? 2 : D3D_OK; }
    pl_sh *ps = pso_obj ? pl_sh_get(d, pso_obj) : NULL;
    if (pso_obj && !ps) { g_st.skipped++; return g_dry ? 3 : D3D_OK; }
    if (st->rs[PL_RS_SRGBWRITEENABLE]) log_once(&w_srgbw, "D3DRS_SRGBWRITEENABLE is ignored (linear writes)");
    if (st->rs[PL_RS_CLIPPLANEENABLE]) log_once(&w_clip, "user clip planes are ignored");
    if (st->rs[PL_RS_POINTSPRITEENABLE]) log_once(&w_point, "point sprites are not emulated");
    for (int w = 0; w < 8; w++)
        if (st->rs[w < 8 ? PL_RS_WRAP0 + w : 0]) { log_once(&w_wrap, "D3DRS_WRAP0..7 cylindrical wrapping is ignored"); break; }

    /* 1. everything that may end the render pass comes first: textures, buffers */
    id<MTLTexture> ftex[16], vtex[4];
    mt_obj *fused[16], *vused[4];
    d3d9msl_sampler finfo[16], vinfo[4];
    memset(finfo, 0, sizeof finfo);
    memset(vinfo, 0, sizeof vinfo);
    uint16_t fmask = 0;
    int ff = 0;
    if (ps) fmask = ps->smp_mask;
    else {
        /* fixed-function stand-in: stage 0 texture if one is bound and the VS writes texcoord0 */
        int has_c = pl_sh_writes(vs, "color0"), has_t = pl_sh_writes(vs, "texcoord0") && st->tex[0];
        ff = (has_c ? 1 : 0) | (has_t ? 2 : 0);
        if (has_t) fmask = 1;
        if (!p->ff_warned) { p->ff_warned = 1; MT_LOG("pipeline: a draw with no pixel shader uses the fixed-function stand-in\n"); }
    }
    for (unsigned s = 0; s < 16; s++)
        if (fmask >> s & 1) ftex[s] = resolve_texture(d, s, ps ? ps->smp_type[s] : PL_ST_2D, &fused[s], &finfo[s]);
    uint16_t vmask = vs->smp_mask & 0xf;
    for (unsigned k = 0; k < 4; k++)
        if (vmask >> k & 1) vtex[k] = resolve_texture(d, MT_SAMPLER_VTX0 + k, vs->smp_type[k], &vused[k], &vinfo[k]);

    pl_pso_key key;
    memset(&key, 0, sizeof key);
    key.vs = vso->uid;
    key.ps = pso_obj ? pso_obj->uid : 0;
    key.decl = decl->uid;
    key.ff = (uint8_t)ff;
    id<MTLBuffer> vbuf[MT_NSTREAMS];
    uint32_t voff[MT_NSTREAMS];
    uint32_t smask = 0;
    {
        const pl_elem *e = (const pl_elem *)decl->decl;
        for (uint32_t i = 0; i < decl->decl_count; i++)
            if (e[i].stream < MT_NSTREAMS && e[i].type != PL_DT_UNUSED) smask |= 1u << e[i].stream;
    }
    for (unsigned s = 0; s < MT_NSTREAMS; s++) {
        if (!(smask >> s & 1)) continue;
        mt_stream *sr = &st->streams[s];
        if (s == 0 && a->up) {
            vbuf[0] = a->up_buf; voff[0] = a->up_off;
            key.stride[0] = (uint16_t)a->up_stride;
            continue;
        }
        if (!sr->vb) { key.null_streams |= (uint16_t)(1u << s); vbuf[s] = nil; continue; }
        vbuf[s] = mt_buffer_of(d, sr->vb);
        if (!vbuf[s]) { key.null_streams |= (uint16_t)(1u << s); continue; }
        voff[s] = sr->offset;
        key.stride[s] = (uint16_t)sr->stride;
        if (a->indexed && (sr->freq & PL_SSF_INSTANCEDATA)) {
            uint32_t r = sr->freq & 0x3fffffffu;
            key.rate[s] = (uint8_t)(r ? (r > 255 ? 255 : r) : 1);
        }
    }

    /* 2. the encoder */
    id<MTLRenderCommandEncoder> enc = mt_pass_encoder(d);
    if (!enc) { g_st.skipped++; return D3D_OK; }
    if (enc != p->enc) {
        enc_forget(p);
        p->enc = [enc retain];
        [enc setFrontFacingWinding:MTLWindingClockwise];
        [enc setVertexBuffer:p->zero offset:0 atIndex:PL_DUMMY_VB];
        g_st.encoders++;
    }

    /* 3. pipeline key: attachments, blend, masks, alpha test */
    uint32_t nrt = 0;
    for (int i = 0; i < MT_NRT; i++) {
        mt_obj *rt = d->pass_rt[i];
        if (!rt) continue;
        uint32_t sl, lv;
        id<MTLTexture> t = mt_surface_target(rt, &sl, &lv);
        key.fmt[i] = fmt_of(t);
        if (t && !key.samples) key.samples = (uint8_t)t.sampleCount;
        static const int wm_rs[4] = {PL_RS_COLORWRITEENABLE, PL_RS_COLORWRITEENABLE1, PL_RS_COLORWRITEENABLE2, PL_RS_COLORWRITEENABLE3};
        uint32_t m = st->rs[wm_rs[i]];
        if (ps && !(ps->ps_out_mask >> i & 1)) m = 0;     /* an output the shader does not write stays untouched */
        if (!ps && i > 0) m = 0;
        key.wmask[i] = (uint8_t)pl_color_write_mask(m);
        nrt = (uint32_t)i + 1;
    }
    (void)nrt;
    int has_depth = 0, has_stencil = 0;
    if (d->pass_ds) {
        uint32_t sl, lv;
        id<MTLTexture> t = mt_surface_target(d->pass_ds, &sl, &lv);
        uint32_t f = fmt_of(t);
        if (fmt_has_depth(f)) { key.depth_fmt = f; has_depth = 1; }
        if (fmt_has_stencil(f)) { key.stencil_fmt = f; has_stencil = 1; }
        if (t && !key.samples) key.samples = (uint8_t)t.sampleCount;
    }
    if (!key.samples) key.samples = 1;
    fill_blend(d, &key);
    key.alpha_func = st->rs[PL_RS_ALPHATESTENABLE] ? (uint8_t)(st->rs[PL_RS_ALPHAFUNC] >= 1 && st->rs[PL_RS_ALPHAFUNC] <= 8 ? st->rs[PL_RS_ALPHAFUNC] : 8) : 8;
    /* a pixel shader input the vertex shader does not write fails pipeline creation: report it once */
    id<MTLRenderPipelineState> pso = get_pso(d, &key, vs, ps, decl);
    if (!pso) { log_once(&w_link, "a pipeline could not be built (see the pipeline state message); its draws are skipped"); g_st.skipped++; return g_dry ? 1 : D3D_OK; }
    if (g_dry) return 0;

    /* 4. encoder state */
    if (pso != p->b_pso) { [enc setRenderPipelineState:pso]; p->b_pso = pso; }
    id<MTLDepthStencilState> dss = get_dss(d, has_depth, has_stencil);
    if (dss != p->b_dss) { [enc setDepthStencilState:dss]; p->b_dss = dss; }
    int cull = (int)pl_cull(st->rs[PL_RS_CULLMODE]);
    if (cull != p->b_cull) { [enc setCullMode:(MTLCullMode)cull]; p->b_cull = cull; }
    int fill = (int)pl_fill(st->rs[PL_RS_FILLMODE]);
    if (fill != p->b_fill) { [enc setTriangleFillMode:(MTLTriangleFillMode)fill]; p->b_fill = fill; }
    float bias, slope;
    memcpy(&bias, &st->rs[PL_RS_DEPTHBIAS], 4);
    memcpy(&slope, &st->rs[PL_RS_SLOPESCALEDEPTHBIAS], 4);
    /* D3D9's DEPTHBIAS is in depth units; Metal's constant is in units of the depth format's resolution.
     * For the 24-bit depth the game asks for, one unit is 2^-24; Depth32Float's unit equals that for z in
     * [0.5, 1) (DEVIATIONS: smaller z gets a smaller bias). */
    float mb = bias * 16777216.0f;
    if (mb != p->b_bias[0] || slope != p->b_bias[1]) {
        [enc setDepthBias:mb slopeScale:slope clamp:0];
        p->b_bias[0] = mb; p->b_bias[1] = slope;
    }
    if (has_stencil) {
        uint32_t ref = st->rs[PL_RS_STENCILREF] & 0xff;
        if (ref != p->b_sref[0]) { [enc setStencilReferenceValue:ref]; p->b_sref[0] = ref; }
    }
    if (key.blend) {
        float bc[4];
        pl_argb_to_float(st->rs[PL_RS_BLENDFACTOR], bc);
        if (memcmp(bc, p->b_bc, sizeof bc)) { [enc setBlendColorRed:bc[0] green:bc[1] blue:bc[2] alpha:bc[3]]; memcpy(p->b_bc, bc, sizeof bc); }
    }
    MTLViewport vp;
    const double hp = PL_HALFPIXEL_VIEWPORT ? 0.5 : 0.0;
    vp.originX = st->vp.x + hp; vp.originY = st->vp.y + hp;
    vp.width = st->vp.w; vp.height = st->vp.h;
    vp.znear = st->vp.minz; vp.zfar = st->vp.maxz;
    if (memcmp(&vp, &p->b_vp, sizeof vp)) { [enc setViewport:vp]; p->b_vp = vp; }
    MTLScissorRect sc = {0, 0, d->pass_w, d->pass_h};
    if (st->rs[PL_RS_SCISSORTESTENABLE]) {
        int32_t l = (int32_t)st->scissor[0], t = (int32_t)st->scissor[1], r = (int32_t)st->scissor[2], b = (int32_t)st->scissor[3];
        if (l < 0) l = 0;
        if (t < 0) t = 0;
        if (r > (int32_t)d->pass_w) r = (int32_t)d->pass_w;
        if (b > (int32_t)d->pass_h) b = (int32_t)d->pass_h;
        if (r <= l || b <= t) { g_st.skipped++; return D3D_OK; }      /* empty scissor: nothing is drawn */
        sc.x = (NSUInteger)l; sc.y = (NSUInteger)t; sc.width = (NSUInteger)(r - l); sc.height = (NSUInteger)(b - t);
    }
    /* the viewport also bounds what is drawn (D3D9 clips to it; Metal only maps to it) */
    {
        int64_t vx0 = st->vp.x, vy0 = st->vp.y, vx1 = (int64_t)st->vp.x + st->vp.w, vy1 = (int64_t)st->vp.y + st->vp.h;
        int64_t x0 = (int64_t)sc.x > vx0 ? (int64_t)sc.x : vx0, y0 = (int64_t)sc.y > vy0 ? (int64_t)sc.y : vy0;
        int64_t x1 = (int64_t)(sc.x + sc.width) < vx1 ? (int64_t)(sc.x + sc.width) : vx1;
        int64_t y1 = (int64_t)(sc.y + sc.height) < vy1 ? (int64_t)(sc.y + sc.height) : vy1;
        if (x1 <= x0 || y1 <= y0) { g_st.skipped++; return D3D_OK; }
        sc.x = (NSUInteger)x0; sc.y = (NSUInteger)y0; sc.width = (NSUInteger)(x1 - x0); sc.height = (NSUInteger)(y1 - y0);
    }
    if (memcmp(&sc, &p->b_sc, sizeof sc)) { [enc setScissorRect:sc]; p->b_sc = sc; }

    for (unsigned s = 0; s < MT_NSTREAMS; s++) {
        if (!(smask >> s & 1) || !vbuf[s]) continue;
        unsigned bi = MT_BUF_STREAM(s);
        if (p->b_vbuf[bi] != vbuf[s]) { [enc setVertexBuffer:vbuf[s] offset:voff[s] atIndex:bi]; p->b_vbuf[bi] = vbuf[s]; p->b_voff[bi] = voff[s]; }
        else if (p->b_voff[bi] != voff[s]) { [enc setVertexBufferOffset:voff[s] atIndex:bi]; p->b_voff[bi] = voff[s]; }
        if (!(s == 0 && a->up)) mt_mark_used(d, st->streams[s].vb);
    }
    for (unsigned s = 0; s < 16; s++) {
        if (!(fmask >> s & 1)) continue;
        if (p->b_ftex[s] != ftex[s]) { [enc setFragmentTexture:ftex[s] atIndex:MT_TEX_PS(s)]; p->b_ftex[s] = ftex[s]; }
        id<MTLSamplerState> sm = get_sampler(d, s, &finfo[s]);
        if (p->b_fsmp[s] != sm) { [enc setFragmentSamplerState:sm atIndex:MT_TEX_PS(s)]; p->b_fsmp[s] = sm; }
        if (fused[s]) mt_mark_used(d, fused[s]);
    }
    for (unsigned k = 0; k < 4; k++) {
        if (!(vmask >> k & 1)) continue;
        if (p->b_vtex[k] != vtex[k]) { [enc setVertexTexture:vtex[k] atIndex:MT_TEX_VS(k)]; p->b_vtex[k] = vtex[k]; }
        id<MTLSamplerState> sm = get_sampler(d, MT_SAMPLER_VTX0 + k, &vinfo[k]);
        if (p->b_vsmp[k] != sm) { [enc setVertexSamplerState:sm atIndex:MT_TEX_VS(k)]; p->b_vsmp[k] = sm; }
        if (vused[k]) mt_mark_used(d, vused[k]);
    }
    set_consts(d, enc, vs, ps, vso, pso_obj);
    set_aux(d, enc, 0, vinfo, 4);
    set_aux(d, enc, 1, finfo, 16);

    /* 5. draw */
    if (a->indexed)
        [enc drawIndexedPrimitives:(MTLPrimitiveType)a->prim indexCount:a->count
                         indexType:a->isz == 2 ? MTLIndexTypeUInt16 : MTLIndexTypeUInt32 indexBuffer:a->ibuf
                 indexBufferOffset:a->ioff instanceCount:a->instances ? a->instances : 1 baseVertex:a->base_vertex baseInstance:0];
    else
        [enc drawPrimitives:(MTLPrimitiveType)a->prim vertexStart:a->first vertexCount:a->count
              instanceCount:a->instances ? a->instances : 1];
    g_st.draws++;
    return D3D_OK;
}

/* ---- the handlers ------------------------------------------------------------------------------------- */
static mt_dev *dev_of(sg_com_call *c) { return (mt_dev *)c->obj; }
/* the run report's counters, shared with the null device (d3d9null_stats_ptr): every draw call and its primitives */
static void count_draw(uint32_t prims) {
    d3d9null_stats *s = d3d9null_stats_ptr();
    if (s) { s->draws++; s->primitives += prims; }
}

static uint32_t h_draw_primitive(sg_com_call *c) {
    mt_dev *d = dev_of(c);
    uint32_t pt = SG_COM_ARG(c, 0), start = SG_COM_ARG(c, 1), n = SG_COM_ARG(c, 2);
    count_draw(n);
    pl_draw_args a;
    memset(&a, 0, sizeof a);
    int fan;
    if (!pl_topology(pt, n, &a.prim, &a.count, &fan)) return MT_D3DERR_INVALIDCALL;
    if (!n) return D3D_OK;
    g_st.prims += n;
    @autoreleasepool {
        if (fan) {
            uint32_t off;
            void *host;
            id<MTLBuffer> b = mt_scratch(d, 12 * n, 4, &off, &host);
            if (!b) return D3D_OK;
            pl_fan_to_list(n, NULL, 4, start, (uint32_t *)host);
            a.indexed = 1; a.ibuf = b; a.ioff = off; a.isz = 4;
            g_st.fans++;
        } else a.first = start;
        return do_draw(d, &a);
    }
}

/* The index data a DrawIndexedPrimitive reads, as the current Metal buffer of the IB (shared storage). */
static uint32_t h_draw_indexed(sg_com_call *c) {
    mt_dev *d = dev_of(c);
    uint32_t pt = SG_COM_ARG(c, 0);
    int32_t base = (int32_t)SG_COM_ARG(c, 1);
    uint32_t start = SG_COM_ARG(c, 4), n = SG_COM_ARG(c, 5);
    count_draw(n);
    pl_draw_args a;
    memset(&a, 0, sizeof a);
    int fan;
    if (!pl_topology(pt, n, &a.prim, &a.count, &fan)) return MT_D3DERR_INVALIDCALL;
    if (!n) return D3D_OK;
    mt_obj *ib = d->st.ib;
    if (!ib) return MT_D3DERR_INVALIDCALL;
    g_st.prims += n;
    @autoreleasepool {
        id<MTLBuffer> buf = mt_buffer_of(d, ib);
        if (!buf) return D3D_OK;
        uint32_t isz = ib->fmt == PL_D3DFMT_INDEX32 ? 4 : 2;
        uint32_t nidx = fan ? n + 2 : a.count;
        if ((uint64_t)(start + nidx) * isz > buf.length) {
            log_once(&w_vrange, "an indexed draw reads past its index buffer: clamped");
            if ((uint64_t)start * isz >= buf.length) return D3D_OK;
            nidx = (uint32_t)(buf.length / isz) - start;
            if (fan) { if (nidx < 3) return D3D_OK; n = nidx - 2; a.count = 3 * n; }
            else a.count = nidx;
        }
        const uint8_t *src = buf.storageMode == MTLStorageModePrivate ? NULL : (const uint8_t *)buf.contents + (size_t)start * isz;
        a.indexed = 1;
        a.base_vertex = base;
        a.instances = 1;
        if (d->st.streams[0].freq & PL_SSF_INDEXEDDATA) {
            uint32_t ni = d->st.streams[0].freq & 0x3fffffffu;
            a.instances = ni ? ni : 1;
        }
        if (fan && src) {
            uint32_t off;
            void *host;
            id<MTLBuffer> b = mt_scratch(d, 12 * n, 4, &off, &host);
            if (!b) return D3D_OK;
            pl_fan_to_list(n, src, isz, 0, (uint32_t *)host);
            a.ibuf = b; a.ioff = off; a.isz = 4;
            g_st.fans++;
            mt_mark_used(d, ib);
        } else if ((pt == PL_PT_TRIANGLESTRIP || pt == PL_PT_LINESTRIP) && isz == 2 && src && restart_fix() && pl_has_restart(src, 2, nidx)) {
            /* Metal restarts a strip at 0xFFFF; D3D9 draws vertex 65535 (+ base). Re-issue as 32-bit indices. */
            uint32_t off;
            void *host;
            id<MTLBuffer> b = mt_scratch(d, 4 * nidx, 4, &off, &host);
            if (!b) return D3D_OK;
            const uint16_t *s16 = (const uint16_t *)src;
            for (uint32_t i = 0; i < nidx; i++) ((uint32_t *)host)[i] = s16[i];
            a.ibuf = b; a.ioff = off; a.isz = 4;
            g_st.restart_promotions++;
            mt_mark_used(d, ib);
        } else {
            a.ibuf = buf; a.ioff = start * isz; a.isz = isz;
            mt_mark_used(d, ib);
        }
        return do_draw(d, &a);
    }
}

static uint32_t h_draw_primitive_up(sg_com_call *c) {
    mt_dev *d = dev_of(c);
    uint32_t pt = SG_COM_ARG(c, 0), n = SG_COM_ARG(c, 1), data = SG_COM_ARG(c, 2), stride = SG_COM_ARG(c, 3);
    count_draw(n);
    pl_draw_args a;
    memset(&a, 0, sizeof a);
    int fan;
    if (!pl_topology(pt, n, &a.prim, &a.count, &fan) || !data) return MT_D3DERR_INVALIDCALL;
    uint32_t r = D3D_OK;
    if (n) {
        g_st.prims += n;
        @autoreleasepool {
            uint32_t nv = fan ? n + 2 : a.count, bytes = nv * stride, off;
            void *host;
            id<MTLBuffer> b = mt_scratch(d, bytes ? bytes : 4, 16, &off, &host);
            if (b) {
                memcpy(host, c->mem + data, bytes);
                g_st.up_bytes += bytes;
                a.up = 1; a.up_buf = b; a.up_off = off; a.up_stride = stride;
                if (fan) {
                    uint32_t ioff;
                    void *ih;
                    id<MTLBuffer> ib = mt_scratch(d, 12 * n, 4, &ioff, &ih);
                    if (ib) {
                        pl_fan_to_list(n, NULL, 4, 0, (uint32_t *)ih);
                        a.indexed = 1; a.ibuf = ib; a.ioff = ioff; a.isz = 4;
                        g_st.fans++;
                        r = do_draw(d, &a);
                    }
                } else r = do_draw(d, &a);
            }
        }
    }
    /* D3D9: after a UP draw stream 0 is unset */
    d->st.streams[0].vb = NULL; d->st.streams[0].offset = 0; d->st.streams[0].stride = 0;
    d->st.dirty |= MT_DIRTY_STREAMS;
    return r;
}

static uint32_t h_draw_indexed_up(sg_com_call *c) {
    mt_dev *d = dev_of(c);
    uint32_t pt = SG_COM_ARG(c, 0), minv = SG_COM_ARG(c, 1), nverts = SG_COM_ARG(c, 2), n = SG_COM_ARG(c, 3);
    uint32_t idata = SG_COM_ARG(c, 4), ifmt = SG_COM_ARG(c, 5), vdata = SG_COM_ARG(c, 6), stride = SG_COM_ARG(c, 7);
    count_draw(n);
    pl_draw_args a;
    memset(&a, 0, sizeof a);
    int fan;
    if (!pl_topology(pt, n, &a.prim, &a.count, &fan) || !idata || !vdata) return MT_D3DERR_INVALIDCALL;
    uint32_t isz = ifmt == PL_D3DFMT_INDEX32 ? 4 : 2, r = D3D_OK;
    if (n) {
        g_st.prims += n;
        @autoreleasepool {
            uint32_t vbytes = (minv + nverts) * stride, voff, ioff;
            void *vh, *ih;
            id<MTLBuffer> vb = mt_scratch(d, vbytes ? vbytes : 4, 16, &voff, &vh);
            uint32_t nidx = fan ? n + 2 : a.count;
            /* indices always go to 32 bits here: fans become lists, and 0xFFFF must not restart a strip */
            uint32_t outn = fan ? 3 * n : nidx;
            id<MTLBuffer> ib = mt_scratch(d, 4 * outn, 4, &ioff, &ih);
            if (vb && ib) {
                memcpy(vh, c->mem + vdata, vbytes);
                const uint8_t *src = c->mem + idata;
                if (fan) { pl_fan_to_list(n, src, isz, 0, (uint32_t *)ih); g_st.fans++; }
                else if (isz == 2) for (uint32_t i = 0; i < nidx; i++) ((uint32_t *)ih)[i] = ((const uint16_t *)src)[i];
                else memcpy(ih, src, 4 * nidx);
                g_st.up_bytes += vbytes + isz * nidx;
                a.up = 1; a.up_buf = vb; a.up_off = voff; a.up_stride = stride;
                a.indexed = 1; a.ibuf = ib; a.ioff = ioff; a.isz = 4;
                r = do_draw(d, &a);
            }
        }
    }
    /* D3D9: after DrawIndexedPrimitiveUP stream 0 and the indices are unset */
    d->st.streams[0].vb = NULL; d->st.streams[0].offset = 0; d->st.streams[0].stride = 0;
    d->st.ib = NULL;
    d->st.dirty |= MT_DIRTY_STREAMS | MT_DIRTY_IB;
    return r;
}

/* ---- Clear ------------------------------------------------------------------------------------------- */
typedef struct pl_clr_key { uint32_t fmt[MT_NRT], depth_fmt, stencil_fmt, samples, flags; } pl_clr_key;

static id<MTLRenderPipelineState> clear_pso(mt_dev *d, const pl_clr_key *k) {
    pl_dev *p = P(d);
    void **slot = map_slot(&p->clr, k, fnv(k, sizeof *k), 1);
    if (!*slot) {
        @autoreleasepool {
            id<MTLLibrary> u = util_lib(d);
            int n = 0;
            for (int i = 0; i < MT_NRT; i++) if (k->fmt[i]) n = i + 1;
            if (!(k->flags & PL_CLEAR_TARGET)) n = 0;
            MTLRenderPipelineDescriptor *pd = [[MTLRenderPipelineDescriptor alloc] init];
            id<MTLFunction> vf = [u newFunctionWithName:@"pl_clear_vs"];
            id<MTLFunction> ff = [u newFunctionWithName:[NSString stringWithFormat:@"pl_clear_fs%d", n]];
            pd.vertexFunction = vf;
            pd.fragmentFunction = ff;
            for (int i = 0; i < MT_NRT; i++) {
                pd.colorAttachments[i].pixelFormat = (MTLPixelFormat)k->fmt[i];
                if (k->fmt[i]) pd.colorAttachments[i].writeMask = (k->flags & PL_CLEAR_TARGET) ? MTLColorWriteMaskAll : MTLColorWriteMaskNone;
            }
            pd.depthAttachmentPixelFormat = (MTLPixelFormat)k->depth_fmt;
            pd.stencilAttachmentPixelFormat = (MTLPixelFormat)k->stencil_fmt;
            pd.rasterSampleCount = k->samples ? k->samples : 1;
            NSError *e = nil;
            id pso = [d->mtl newRenderPipelineStateWithDescriptor:pd error:&e];
            if (!pso) MT_LOG("pipeline: clear pipeline failed: %s\n", e ? e.localizedDescription.UTF8String : "?");
            *slot = pso ? (void *)pso : (void *)1;
            [vf release]; [ff release]; [pd release];
        }
    }
    return *slot == (void *)1 ? nil : (id<MTLRenderPipelineState>)*slot;
}

static uint32_t h_clear(sg_com_call *c) {
    mt_dev *d = dev_of(c);
    mt_state *st = &d->st;
    uint32_t count = SG_COM_ARG(c, 0), rects = SG_COM_ARG(c, 1), flags = SG_COM_ARG(c, 2), color = SG_COM_ARG(c, 3);
    float z = SG_COM_ARGF(c, 4);
    uint32_t stencil = SG_COM_ARG(c, 5);
    if (!d->st.ds) flags &= ~(PL_CLEAR_ZBUFFER | PL_CLEAR_STENCIL);
    if (!(flags & (PL_CLEAR_TARGET | PL_CLEAR_ZBUFFER | PL_CLEAR_STENCIL))) return D3D_OK;
    if (rects == 0) count = 0;
    float rgba[4];
    pl_argb_to_float(color, rgba);
    mt_obj *rt0 = st->rt[0];
    uint32_t tw = rt0 ? rt0->w : 0, th = rt0 ? rt0->h : 0;
    if (!rt0 && st->ds) { tw = st->ds->w; th = st->ds->h; }
    /* the area D3D9 clears: viewport, scissor (when enabled), rectangles */
    int64_t x0 = st->vp.x, y0 = st->vp.y, x1 = (int64_t)st->vp.x + st->vp.w, y1 = (int64_t)st->vp.y + st->vp.h;
    if (st->rs[PL_RS_SCISSORTESTENABLE]) {
        if ((int64_t)(int32_t)st->scissor[0] > x0) x0 = (int32_t)st->scissor[0];
        if ((int64_t)(int32_t)st->scissor[1] > y0) y0 = (int32_t)st->scissor[1];
        if ((int64_t)(int32_t)st->scissor[2] < x1) x1 = (int32_t)st->scissor[2];
        if ((int64_t)(int32_t)st->scissor[3] < y1) y1 = (int32_t)st->scissor[3];
    }
    if (x0 < 0) x0 = 0;
    if (y0 < 0) y0 = 0;
    if (x1 > tw) x1 = tw;
    if (y1 > th) y1 = th;
    if (x1 <= x0 || y1 <= y0) return D3D_OK;
    int whole = !count && x0 == 0 && y0 == 0 && x1 == tw && y1 == th;
    /* a depth surface larger than the target would be cleared beyond the D3D9 area by a load action */
    if (whole && (flags & (PL_CLEAR_ZBUFFER | PL_CLEAR_STENCIL)) && st->ds && (st->ds->w != tw || st->ds->h != th) && rt0) whole = 0;
    if (whole) {
        g_st.clears_load++;
        return (uint32_t)mt_pass_clear(d, flags, rgba, z, stencil & 0xff);
    }
    @autoreleasepool {
        pl_dev *p = P(d);
        id<MTLRenderCommandEncoder> enc = mt_pass_encoder(d);
        if (!enc) return D3D_OK;
        if (enc != p->enc) {
            enc_forget(p);
            p->enc = [enc retain];
            [enc setFrontFacingWinding:MTLWindingClockwise];
            [enc setVertexBuffer:p->zero offset:0 atIndex:PL_DUMMY_VB];
        }
        pl_clr_key k;
        memset(&k, 0, sizeof k);
        for (int i = 0; i < MT_NRT; i++) {
            if (!d->pass_rt[i]) continue;
            uint32_t sl, lv;
            id<MTLTexture> t = mt_surface_target(d->pass_rt[i], &sl, &lv);
            k.fmt[i] = fmt_of(t);
            if (t && !k.samples) k.samples = (uint32_t)t.sampleCount;
        }
        int hd = 0, hs = 0;
        if (d->pass_ds) {
            uint32_t sl, lv;
            id<MTLTexture> t = mt_surface_target(d->pass_ds, &sl, &lv);
            uint32_t f = fmt_of(t);
            if (fmt_has_depth(f)) { k.depth_fmt = f; hd = 1; }
            if (fmt_has_stencil(f)) { k.stencil_fmt = f; hs = 1; }
            if (t && !k.samples) k.samples = (uint32_t)t.sampleCount;
        }
        k.flags = flags & (PL_CLEAR_TARGET | (hd ? PL_CLEAR_ZBUFFER : 0) | (hs ? PL_CLEAR_STENCIL : 0));
        id<MTLRenderPipelineState> pso = clear_pso(d, &k);
        if (!pso) return D3D_OK;
        [enc setRenderPipelineState:pso];
        /* depth: always pass, write when ZBUFFER; stencil: always pass, replace when STENCIL */
        uint64_t dk = (1ull << 63) | (k.flags & 6);   /* real keys use bits 0..47 */
        void **slot = map_slot(&p->dss, &dk, fnv(&dk, 8), 1);
        if (!*slot) {
            MTLDepthStencilDescriptor *dd = [[MTLDepthStencilDescriptor alloc] init];
            dd.depthCompareFunction = MTLCompareFunctionAlways;
            dd.depthWriteEnabled = (k.flags & PL_CLEAR_ZBUFFER) != 0;
            if (k.flags & PL_CLEAR_STENCIL) {
                MTLStencilDescriptor *s = [[MTLStencilDescriptor alloc] init];
                s.stencilCompareFunction = MTLCompareFunctionAlways;
                s.depthStencilPassOperation = MTLStencilOperationReplace;
                s.stencilFailureOperation = s.depthFailureOperation = MTLStencilOperationReplace;
                s.readMask = s.writeMask = 0xff;
                dd.frontFaceStencil = s; dd.backFaceStencil = s;
                [s release];
            }
            *slot = [d->mtl newDepthStencilStateWithDescriptor:dd];
            [dd release];
        }
        [enc setDepthStencilState:(id<MTLDepthStencilState>)*slot];
        if (k.flags & PL_CLEAR_STENCIL) [enc setStencilReferenceValue:stencil & 0xff];
        [enc setCullMode:MTLCullModeNone];
        [enc setTriangleFillMode:MTLTriangleFillModeFill];
        [enc setDepthBias:0 slopeScale:0 clamp:0];
        MTLViewport vp = {0, 0, (double)d->pass_w, (double)d->pass_h, 0, 1};
        [enc setViewport:vp];
        float zv[4] = {z < 0 ? 0 : z > 1 ? 1 : z, 0, 0, 0};
        [enc setVertexBytes:zv length:16 atIndex:0];
        [enc setFragmentBytes:rgba length:16 atIndex:0];
        uint32_t nr = count ? count : 1;
        for (uint32_t i = 0; i < nr; i++) {
            int64_t a0 = x0, b0 = y0, a1 = x1, b1 = y1;
            if (count) {
                const uint8_t *r = c->mem + rects + 16 * i;
                int32_t rx0, ry0, rx1, ry1;
                memcpy(&rx0, r, 4); memcpy(&ry0, r + 4, 4); memcpy(&rx1, r + 8, 4); memcpy(&ry1, r + 12, 4);
                if (rx0 > a0) a0 = rx0;
                if (ry0 > b0) b0 = ry0;
                if (rx1 < a1) a1 = rx1;
                if (ry1 < b1) b1 = ry1;
                if (a1 <= a0 || b1 <= b0) continue;
            }
            if (a1 > d->pass_w) a1 = d->pass_w;
            if (b1 > d->pass_h) b1 = d->pass_h;
            if (a1 <= a0 || b1 <= b0) continue;
            MTLScissorRect sc = {(NSUInteger)a0, (NSUInteger)b0, (NSUInteger)(a1 - a0), (NSUInteger)(b1 - b0)};
            [enc setScissorRect:sc];
            [enc drawPrimitives:MTLPrimitiveTypeTriangle vertexStart:0 vertexCount:3];
        }
        g_st.clears_quad++;
        /* the clear replaced pipeline, depth state, viewport, scissor and buffer 0: re-send them on the next draw */
        enc_forget(p);
        p->enc = [enc retain];
        [enc setFrontFacingWinding:MTLWindingClockwise];
        [enc setVertexBuffer:p->zero offset:0 atIndex:PL_DUMMY_VB];
    }
    return D3D_OK;
}

/* Test and tooling hook: resolves everything a non-indexed triangle-list draw with the current state needs and
 * builds (or finds) its pipeline state, without drawing. 0 = ready; 1 = pipeline state failed; 2 = vertex shader
 * not translated/compiled; 3 = pixel shader not translated/compiled; 4 = no vertex shader or declaration. */
int mt_pl_prepare(mt_dev *d) {
    if (!d->st.vs || !d->st.vdecl) return 4;
    pl_draw_args a;
    memset(&a, 0, sizeof a);
    a.prim = 3; a.count = 3;
    g_dry = 1;
    uint32_t r;
    @autoreleasepool { r = do_draw(d, &a); }
    g_dry = 0;
    return (int)r;
}

/* ---- the slot table ----------------------------------------------------------------------------------- */
sg_com_handler *mt_pipeline_handler(unsigned slot) {
    switch (slot) {
    case S_Device_Clear: return h_clear;
    case S_Device_DrawPrimitive: return h_draw_primitive;
    case S_Device_DrawIndexedPrimitive: return h_draw_indexed;
    case S_Device_DrawPrimitiveUP: return h_draw_primitive_up;
    case S_Device_DrawIndexedPrimitiveUP: return h_draw_indexed_up;
    default: return NULL;
    }
}

/* Statistics for tests and the watch line (SPORE_METAL_WATCH). */
void d3d9metal_pl_stats(uint64_t out[14]) {
    const uint64_t *s = (const uint64_t *)&g_st;
    for (int i = 0; i < 14; i++) out[i] = s[i];
}
const char *d3d9metal_pl_stats_names = "draws prims skipped pso_new pso_fail dss_new smp_new fans restart_promotions up_bytes "
                                       "clears_load clears_quad encoders const_bytes";
