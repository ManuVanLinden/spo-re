/* mt_resources.mm -- the Metal back end's resources: formats, textures, surfaces, volumes, vertex
 * and index buffers, their guest Lock shadows, uploads and readback, UpdateSurface/UpdateTexture,
 * GetRenderTargetData, StretchRect, ColorFill, queries, and the command-buffer / render-pass plumbing
 * that the pipeline draws through (d3d9metal_int.h).
 *
 * Lock memory stays in the guest window (runtime-abi 8.1, DEVIATIONS 3.9): every
 * lockable resource has a guest shadow laid out by the null device's layout code (d3d9null_layout), the
 * authoritative copy of whatever the CPU wrote. Unlock sends the locked region to Metal:
 *   - textures: replaceRegion when no command buffer in flight uses the texture, else a staged blit on the
 *     current command buffer (ordered with the draws as the guest issued them);
 *   - buffers: memcpy into the shared MTLBuffer, or, when a command buffer in flight uses it and the lock
 *     was not NOOVERWRITE, a rename to a fresh MTLBuffer filled from the whole shadow.
 * Contents the GPU wrote (render targets, depth, blit destinations) are read back into the shadow on a
 * Lock that needs them; that commits and waits.
 *
 * Objective-C++ without ARC: the Metal objects in mt_obj/mt_dev are +1 references owned by the struct. */
#import <Metal/Metal.h>
#import <QuartzCore/CAMetalLayer.h>
#include <pthread.h>
#include <unistd.h>

#include "mt_priv.h"

/* ---- formats ----------------------------------------------------------------------------------------- */
enum { SZ = MTLTextureSwizzleZero, S1 = MTLTextureSwizzleOne, SR = MTLTextureSwizzleRed, SG = MTLTextureSwizzleGreen,
       SB = MTLTextureSwizzleBlue, SA = MTLTextureSwizzleAlpha };
#define ID4 {SR, SG, SB, SA}
#define C MT_FMT_COLOR
#define RT MT_FMT_RENDERABLE
static const mt_format k_formats[] = {
    {D3DFMT_A8R8G8B8, MTLPixelFormatBGRA8Unorm, C | RT, ID4, 4, 4, 0, 0},
    {D3DFMT_X8R8G8B8, MTLPixelFormatBGRA8Unorm, C | RT | MT_FMT_ALPHA1, {SR, SG, SB, S1}, 4, 4, 0, 0},
    {D3DFMT_R5G6B5, MTLPixelFormatB5G6R5Unorm, C | RT, ID4, 2, 2, 0, 0},
    {D3DFMT_X1R5G5B5, MTLPixelFormatBGR5A1Unorm, C | RT | MT_FMT_ALPHA1, {SR, SG, SB, S1}, 2, 2, 0, 0},
    {D3DFMT_A1R5G5B5, MTLPixelFormatBGR5A1Unorm, C | RT, ID4, 2, 2, 0, 0},
    {D3DFMT_A4R4G4B4, MTLPixelFormatBGRA8Unorm, C | MT_FMT_CONVERT, ID4, 2, 4, 0, 0},
    {D3DFMT_X4R4G4B4, MTLPixelFormatBGRA8Unorm, C | MT_FMT_CONVERT | MT_FMT_ALPHA1, {SR, SG, SB, S1}, 2, 4, 0, 0},
    {D3DFMT_A8, MTLPixelFormatA8Unorm, C, ID4, 1, 1, 0, 0},
    {D3DFMT_A2B10G10R10, MTLPixelFormatRGB10A2Unorm, C | RT, ID4, 4, 4, 0, 0},
    {D3DFMT_A8B8G8R8, MTLPixelFormatRGBA8Unorm, C | RT, ID4, 4, 4, 0, 0},
    {D3DFMT_X8B8G8R8, MTLPixelFormatRGBA8Unorm, C | RT | MT_FMT_ALPHA1, {SR, SG, SB, S1}, 4, 4, 0, 0},
    {D3DFMT_G16R16, MTLPixelFormatRG16Unorm, C | RT, {SR, SG, S1, S1}, 4, 4, 0, 0},
    {D3DFMT_A2R10G10B10, MTLPixelFormatBGR10A2Unorm, C | RT, ID4, 4, 4, 0, 0},
    {D3DFMT_A16B16G16R16, MTLPixelFormatRGBA16Unorm, C | RT, ID4, 8, 8, 0, 0},
    {D3DFMT_L8, MTLPixelFormatR8Unorm, C, {SR, SR, SR, S1}, 1, 1, 0, 0},
    {D3DFMT_A8L8, MTLPixelFormatRG8Unorm, C, {SR, SR, SR, SG}, 2, 2, 0, 0},
    {D3DFMT_L16, MTLPixelFormatR16Unorm, C, {SR, SR, SR, S1}, 2, 2, 0, 0},
    {D3DFMT_V8U8, MTLPixelFormatRG8Snorm, C, {SR, SG, S1, S1}, 2, 2, 0, 0},
    {D3DFMT_Q8W8V8U8, MTLPixelFormatRGBA8Snorm, C, ID4, 4, 4, 0, 0},
    {D3DFMT_V16U16, MTLPixelFormatRG16Snorm, C, {SR, SG, S1, S1}, 4, 4, 0, 0},
    {D3DFMT_R16F, MTLPixelFormatR16Float, C | RT | MT_FMT_FLOAT, {SR, S1, S1, S1}, 2, 2, 0, 0},
    {D3DFMT_G16R16F, MTLPixelFormatRG16Float, C | RT | MT_FMT_FLOAT, {SR, SG, S1, S1}, 4, 4, 0, 0},
    {D3DFMT_A16B16G16R16F, MTLPixelFormatRGBA16Float, C | RT | MT_FMT_FLOAT, ID4, 8, 8, 0, 0},
    {D3DFMT_R32F, MTLPixelFormatR32Float, C | RT | MT_FMT_FLOAT, {SR, S1, S1, S1}, 4, 4, 0, 0},
    {D3DFMT_G32R32F, MTLPixelFormatRG32Float, C | RT | MT_FMT_FLOAT, {SR, SG, S1, S1}, 8, 8, 0, 0},
    {D3DFMT_A32B32G32R32F, MTLPixelFormatRGBA32Float, C | RT | MT_FMT_FLOAT, ID4, 16, 16, 0, 0},
    {D3DFMT_DXT1, MTLPixelFormatBC1_RGBA, C | MT_FMT_BLOCK, ID4, 0, 0, 8, 0},
    {D3DFMT_DXT2, MTLPixelFormatBC2_RGBA, C | MT_FMT_BLOCK, ID4, 0, 0, 16, 0},
    {D3DFMT_DXT3, MTLPixelFormatBC2_RGBA, C | MT_FMT_BLOCK, ID4, 0, 0, 16, 0},
    {D3DFMT_DXT4, MTLPixelFormatBC3_RGBA, C | MT_FMT_BLOCK, ID4, 0, 0, 16, 0},
    {D3DFMT_DXT5, MTLPixelFormatBC3_RGBA, C | MT_FMT_BLOCK, ID4, 0, 0, 16, 0},
    /* depth: Apple GPUs have no 24-bit depth; D24 formats become 32-bit float depth (DEVIATIONS 3.15) */
    {D3DFMT_D16, MTLPixelFormatDepth16Unorm, MT_FMT_DEPTH, {SR, SR, SR, S1}, 2, 2, 0, 0},
    {D3DFMT_D16_LOCKABLE, MTLPixelFormatDepth16Unorm, MT_FMT_DEPTH, {SR, SR, SR, S1}, 2, 2, 0, 0},
    {D3DFMT_D32F_LOCKABLE, MTLPixelFormatDepth32Float, MT_FMT_DEPTH, {SR, SR, SR, S1}, 4, 4, 0, 0},
    {D3DFMT_D32, MTLPixelFormatDepth32Float, MT_FMT_DEPTH | MT_FMT_CONVERT, {SR, SR, SR, S1}, 4, 4, 0, 0},
    {D3DFMT_D24X8, MTLPixelFormatDepth32Float, MT_FMT_DEPTH | MT_FMT_CONVERT, {SR, SR, SR, S1}, 4, 4, 0, 0},
    {D3DFMT_D24S8, MTLPixelFormatDepth32Float_Stencil8, MT_FMT_DEPTH | MT_FMT_STENCIL | MT_FMT_CONVERT, {SR, SR, SR, S1}, 4, 8, 0, 0},
    {D3DFMT_D24FS8, MTLPixelFormatDepth32Float_Stencil8, MT_FMT_DEPTH | MT_FMT_STENCIL | MT_FMT_CONVERT, {SR, SR, SR, S1}, 4, 8, 0, 0},
    {D3DFMT_D24X4S4, MTLPixelFormatDepth32Float_Stencil8, MT_FMT_DEPTH | MT_FMT_STENCIL | MT_FMT_CONVERT, {SR, SR, SR, S1}, 4, 8, 0, 0},
    {D3DFMT_D15S1, MTLPixelFormatDepth32Float_Stencil8, MT_FMT_DEPTH | MT_FMT_STENCIL | MT_FMT_CONVERT, {SR, SR, SR, S1}, 2, 8, 0, 0},
};
#undef C
#undef RT
extern "C" const mt_format *mt_format_of(uint32_t f) {
    for (unsigned i = 0; i < sizeof k_formats / sizeof k_formats[0]; i++)
        if (k_formats[i].d3dfmt == f) return &k_formats[i];
    return NULL;
}
static int swz_identity(const mt_format *f) { return f->swz[0] == SR && f->swz[1] == SG && f->swz[2] == SB && f->swz[3] == SA; }
/* Guest-layout pixels -> Metal-layout pixels for MT_FMT_CONVERT colour formats (n pixels). */
static void conv_up(const mt_format *f, const uint8_t *s, uint8_t *d, uint32_t n) {
    switch (f->d3dfmt) {
    case D3DFMT_A4R4G4B4: case D3DFMT_X4R4G4B4:
        for (uint32_t i = 0; i < n; i++) {
            uint16_t v = (uint16_t)(s[2 * i] | s[2 * i + 1] << 8);
            d[4 * i + 0] = (uint8_t)((v & 15) * 17);
            d[4 * i + 1] = (uint8_t)(((v >> 4) & 15) * 17);
            d[4 * i + 2] = (uint8_t)(((v >> 8) & 15) * 17);
            d[4 * i + 3] = f->d3dfmt == D3DFMT_X4R4G4B4 ? 255 : (uint8_t)(((v >> 12) & 15) * 17);
        }
        break;
    default: memset(d, 0, (size_t)n * f->mtl_bpp); break; /* depth uploads: not supported (logged) */
    }
}
static void conv_down(const mt_format *f, const uint8_t *s, uint8_t *d, uint32_t n) {
    switch (f->d3dfmt) {
    case D3DFMT_A4R4G4B4: case D3DFMT_X4R4G4B4:
        for (uint32_t i = 0; i < n; i++) {
            uint16_t v = (uint16_t)((s[4 * i] >> 4) | (s[4 * i + 1] >> 4) << 4 | (s[4 * i + 2] >> 4) << 8 | (s[4 * i + 3] >> 4) << 12);
            d[2 * i] = (uint8_t)v, d[2 * i + 1] = (uint8_t)(v >> 8);
        }
        break;
    case D3DFMT_D24X8: case D3DFMT_D24S8: case D3DFMT_D24FS8: case D3DFMT_D24X4S4: case D3DFMT_D32:
        /* 32-bit float depth (and padding) -> D24 in the high 24 bits, stencil 0 */
        for (uint32_t i = 0; i < n; i++) {
            float z;
            memcpy(&z, s + (size_t)i * f->mtl_bpp, 4);
            uint32_t v = f->d3dfmt == D3DFMT_D32 ? (uint32_t)(z * 4294967295.0) : (uint32_t)(z * 16777215.0f + 0.5f) << 8;
            memcpy(d + 4 * i, &v, 4);
        }
        break;
    default: memset(d, 0, (size_t)n * f->guest_bpp); break;
    }
}

/* ---- command buffers, passes, scratch, deferred release --------------------------------------------- */
static inline uint64_t completed_of(mt_dev *d) { return __atomic_load_n(&d->completed, __ATOMIC_ACQUIRE); }
static void set_completed(mt_dev *d, uint64_t s) {
    uint64_t c = completed_of(d);
    while (c < s && !__atomic_compare_exchange_n(&d->completed, &c, s, 0, __ATOMIC_ACQ_REL, __ATOMIC_ACQUIRE)) {
    }
}
extern "C" MT_ID(MTLCommandBuffer) mt_cmd(mt_dev *d) {
    if (!d->cmd) {
        id<MTLCommandBuffer> cb = [d->queue commandBuffer];
        if (!cb) {
            MT_LOG("commandBuffer returned nil\n");
            return nil;
        }
        d->cmd = [cb retain];
        uint64_t serial = d->serial;
        mt_dev *dev = d;
        [cb addCompletedHandler:^(id<MTLCommandBuffer> b) {
          if (b.status == MTLCommandBufferStatusError) {
              char what[300];
              __atomic_add_fetch(&g_mst.gpu_errors, 1, __ATOMIC_RELAXED);
              snprintf(what, sizeof what, "Metal command buffer %llu failed: %s (code %ld)", (unsigned long long)serial,
                       b.error ? b.error.localizedDescription.UTF8String : "?", b.error ? (long)b.error.code : 0L);
              mt_device_lost(dev, what);
          }
          set_completed(dev, serial);
        }];
        g_mst.cmdbufs++;
    }
    return d->cmd;
}
extern "C" void mt_pass_end(mt_dev *d) {
    if (d->enc) {
        [d->enc endEncoding];
        [d->enc release];
        d->enc = nil;
    }
    if (d->blit) {
        [d->blit endEncoding];
        [d->blit release];
        d->blit = nil;
    }
}
extern "C" MT_ID(MTLBlitCommandEncoder) mt_blit(mt_dev *d) {
    if (d->enc) mt_pass_end(d);
    if (!d->blit) {
        id<MTLCommandBuffer> cb = mt_cmd(d);
        if (!cb) return nil;
        d->blit = [[cb blitCommandEncoder] retain];
        g_mst.blits++;
    }
    return d->blit;
}
extern "C" void mt_reap(mt_dev *d) {
    uint64_t done = completed_of(d);
    unsigned j = 0;
    for (unsigned i = 0; i < d->ngrave; i++) {
        if (d->graveyard[i].serial <= done) [(id)d->graveyard[i].obj release];
        else d->graveyard[j++] = d->graveyard[i];
    }
    d->ngrave = j;
}
extern "C" void mt_flush(mt_dev *d, int wait) {
    mt_pass_end(d);
    if (d->cmd) {
        id<MTLCommandBuffer> cb = d->cmd;
        uint64_t serial = d->serial;
        d->cmd = nil;
        d->serial++;
        [cb commit];
        if (wait) {
            uint64_t t0 = mt_now_ns();
            [cb waitUntilCompleted];
            set_completed(d, serial);
            g_mst.waits++;
            g_mst.wait_ns += mt_now_ns() - t0;
        }
        if (d->last_cmd) [d->last_cmd release];
        d->last_cmd = cb; /* keeps the reference */
    }
    mt_reap(d);
}
extern "C" void mt_wait_serial(mt_dev *d, uint64_t serial) {
    if (serial <= completed_of(d)) return;
    if (d->cmd && serial >= d->serial) {
        mt_flush(d, 1);
        return;
    }
    if (d->last_cmd) {
        uint64_t t0 = mt_now_ns();
        [d->last_cmd waitUntilCompleted];
        set_completed(d, d->serial - 1);
        g_mst.waits++;
        g_mst.wait_ns += mt_now_ns() - t0;
    }
    mt_reap(d);
}
extern "C" void mt_retire(mt_dev *d, void *obj) {
    if (!obj) return;
    if (!d || !d->alive || !d->queue) {
        [(id)obj release];
        return;
    }
    if (d->ngrave == d->capgrave) {
        d->capgrave = d->capgrave ? d->capgrave * 2 : 256;
        d->graveyard = (decltype(d->graveyard))realloc(d->graveyard, d->capgrave * sizeof *d->graveyard);
    }
    d->graveyard[d->ngrave].obj = obj;
    d->graveyard[d->ngrave].serial = d->serial;
    d->ngrave++;
}
#define SCRATCH_CHUNK (8u << 20)
extern "C" MT_ID(MTLBuffer) mt_scratch(mt_dev *d, uint32_t size, uint32_t align, uint32_t *offset, void **host) {
    if (!align) align = 16;
    mt_cmd(d);
    if (size > SCRATCH_CHUNK / 2) { /* a dedicated buffer, released when this command buffer completes */
        id<MTLBuffer> b = [d->mtl newBufferWithLength:size options:MTLResourceStorageModeShared];
        if (!b) return nil;
        mt_retire(d, b);
        *offset = 0;
        *host = b.contents;
        return b;
    }
    uint32_t off = (d->scratch_off + align - 1) & ~(align - 1);
    unsigned i = d->scratch_idx;
    if (!d->scratch[i] || off + size > SCRATCH_CHUNK) {
        i = (i + 1) % 3;
        if (d->scratch[i] && d->scratch_serial[i] >= d->serial) { /* used by the current command buffer: replace it */
            mt_retire(d, d->scratch[i]);
            d->scratch[i] = nil;
        } else if (d->scratch[i] && d->scratch_serial[i] > completed_of(d)) {
            mt_wait_serial(d, d->scratch_serial[i]);
        }
        if (!d->scratch[i]) d->scratch[i] = [d->mtl newBufferWithLength:SCRATCH_CHUNK options:MTLResourceStorageModeShared];
        if (!d->scratch[i]) return nil;
        d->scratch_idx = i;
        off = 0;
    }
    d->scratch_serial[i] = d->serial;
    d->scratch_off = off + size;
    *offset = off;
    *host = (uint8_t *)d->scratch[i].contents + off;
    return d->scratch[i];
}

/* ---- surfaces as attachments, passes --------------------------------------------------------------- */
extern "C" mt_obj *mt_top(mt_obj *s, uint32_t *face, uint32_t *level) {
    if (s->parent && (s->parent->kind == MT_K_TEX || s->parent->kind == MT_K_CUBE || s->parent->kind == MT_K_VOLTEX)) {
        if (face) *face = s->face;
        if (level) *level = s->level;
        return s->parent;
    }
    if (face) *face = 0;
    if (level) *level = 0;
    return s;
}
extern "C" MT_ID(MTLTexture) mt_surface_target(mt_obj *surf, uint32_t *slice, uint32_t *level) {
    uint32_t f, l;
    mt_obj *t;
    if (!surf) return nil;
    t = mt_top(surf, &f, &l);
    if (slice) *slice = f;
    if (level) *level = l;
    return t->tex;
}
extern "C" void mt_mark_used(mt_dev *d, mt_obj *r) {
    if (!r) return;
    r->used_serial = d->serial;
    if (r->parent && r->parent->kind != MT_K_DEV && r->parent->kind != MT_K_SC) r->parent->used_serial = d->serial;
}
extern "C" void mt_gpu_wrote(mt_obj *t) {
    mt_obj *top = mt_top(t, NULL, NULL);
    top->gpu_dirty = 1;
    if (top->usage & D3DUSAGE_AUTOGENMIPMAP) top->mips_dirty = 1;
}
extern "C" MT_ID(MTLBuffer) mt_pass_visibility(mt_dev *d) {
    if (!d->vis) {
        d->vis_slots = 4096;
        d->vis = [d->mtl newBufferWithLength:8u * d->vis_slots options:MTLResourceStorageModeShared];
        memset(d->vis.contents, 0, 8u * d->vis_slots);
    }
    return d->vis;
}
static void begin_pass(mt_dev *d, uint32_t clear_flags) {
    MTLRenderPassDescriptor *rp = [MTLRenderPassDescriptor renderPassDescriptor];
    uint32_t w = 0, h = 0, n = 0;
    mt_pass_end(d);
    id<MTLCommandBuffer> cb = mt_cmd(d);
    if (!cb) return;
    memcpy(d->pass_req_rt, d->st.rt, sizeof d->pass_req_rt);
    d->pass_req_ds = d->st.ds;
    for (int i = 0; i < MT_NRT; i++) {
        mt_obj *s = d->st.rt[i];
        uint32_t slice = 0, level = 0;
        id<MTLTexture> t = s ? mt_surface_target(s, &slice, &level) : nil;
        d->pass_rt[i] = t ? s : NULL;
        if (!t) continue;
        MTLRenderPassColorAttachmentDescriptor *a = rp.colorAttachments[i];
        a.texture = t;
        if (t.textureType == MTLTextureType3D) a.depthPlane = slice;
        else a.slice = slice;
        a.level = level;
        a.loadAction = (clear_flags & 1) ? MTLLoadActionClear : MTLLoadActionLoad;
        a.clearColor = MTLClearColorMake(d->clear_rgba[0], d->clear_rgba[1], d->clear_rgba[2], d->clear_rgba[3]);
        a.storeAction = MTLStoreActionStore;
        if (!n++) w = lvl_dim(s->w, 0), h = lvl_dim(s->h, 0);
        mt_mark_used(d, s);
        mt_gpu_wrote(s);
    }
    d->pass_ds = NULL;
    if (d->st.ds) {
        mt_obj *s = d->st.ds;
        uint32_t slice = 0, level = 0;
        id<MTLTexture> t = mt_surface_target(s, &slice, &level);
        /* D3D9 allows a depth buffer larger than the colour target; Metal draws the intersection. A smaller
         * one is invalid in D3D9 too: it is dropped (logged once). */
        if (t && n && (lvl_dim(s->w, 0) < w || lvl_dim(s->h, 0) < h)) {
            static int once;
            if (!once++) MT_LOG("depth surface %ux%u smaller than the render target %ux%u: drawn without depth\n", s->w, s->h, w, h);
            t = nil;
        }
        if (t) {
            const mt_format *f = s->mf;
            d->pass_ds = s;
            rp.depthAttachment.texture = t;
            rp.depthAttachment.slice = slice;
            rp.depthAttachment.level = level;
            rp.depthAttachment.loadAction = (clear_flags & 2) ? MTLLoadActionClear : MTLLoadActionLoad;
            rp.depthAttachment.clearDepth = d->clear_z;
            rp.depthAttachment.storeAction = MTLStoreActionStore;
            if (f && (f->flags & MT_FMT_STENCIL)) {
                rp.stencilAttachment.texture = t;
                rp.stencilAttachment.slice = slice;
                rp.stencilAttachment.level = level;
                rp.stencilAttachment.loadAction = (clear_flags & 4) ? MTLLoadActionClear : MTLLoadActionLoad;
                rp.stencilAttachment.clearStencil = d->clear_stencil;
                rp.stencilAttachment.storeAction = MTLStoreActionStore;
            }
            if (!n) w = s->w, h = s->h;
            mt_mark_used(d, s);
            mt_gpu_wrote(s);
        }
    }
    if (!n && !d->pass_ds) { /* nothing to draw into (a SYSTEMMEM target?): a 1x1 pass without attachments */
        rp.renderTargetWidth = 1;
        rp.renderTargetHeight = 1;
        rp.defaultRasterSampleCount = 1;
    }
    if (d->vis) rp.visibilityResultBuffer = d->vis;
    d->pass_w = w ? w : 1;
    d->pass_h = h ? h : 1;
    d->enc = [[cb renderCommandEncoderWithDescriptor:rp] retain];
    if (!d->enc) MT_LOG("renderCommandEncoderWithDescriptor returned nil\n");
    else if (d->occ_active) mt_occlusion_pass_begun(d);
    if (g_mt_trace >= 2) MT_LOG("pass begun: %ux%u, %u colour, depth %s, clear 0x%x\n", d->pass_w, d->pass_h, n, d->pass_ds ? "yes" : "no", clear_flags);
}
static int pass_matches(mt_dev *d) {
    for (int i = 0; i < MT_NRT; i++)
        if (d->st.rt[i] != d->pass_req_rt[i]) return 0;
    return d->st.ds == d->pass_req_ds;
}
extern "C" MT_ID(MTLRenderCommandEncoder) mt_pass_encoder(mt_dev *d) {
    if (d->enc && pass_matches(d)) return d->enc;
    begin_pass(d, 0);
    return d->enc;
}
extern "C" int mt_pass_clear(mt_dev *d, uint32_t flags, const float rgba[4], float z, uint32_t stencil) {
    if (rgba) memcpy(d->clear_rgba, rgba, sizeof d->clear_rgba);
    d->clear_z = z;
    d->clear_stencil = stencil;
    begin_pass(d, flags);
    g_mst.clears++;
    return 0;
}

/* ---- resource objects -------------------------------------------------------------------------------- */
static uint32_t full_chain(uint32_t w, uint32_t h, uint32_t d) {
    uint32_t m = w > h ? w : h, n = 1;
    if (d > m) m = d;
    while (m > 1) m >>= 1, n++;
    return n;
}
static void set_layout(mt_obj *o) {
    if (d3d9null_layout(o->fmt, o->w, o->h, o->d, o->levels, o->faces, o->lvl_off, o->lvl_pitch, o->lvl_slice, &o->face_bytes, &o->bytes))
        o->bytes = 0;
}
static int metal_backed(uint32_t pool) { return pool == D3DPOOL_DEFAULT || pool == D3DPOOL_MANAGED; }
/* The Metal texture of a texture/cube/volume/standalone surface. */
static int make_texture(mt_dev *d, mt_obj *t) {
    const mt_format *f = t->mf;
    MTLTextureDescriptor *td;
    uint32_t mlevels = t->levels;
    if (!f || !metal_backed(t->pool) || !d->mtl) return 0;
    if (t->usage & D3DUSAGE_AUTOGENMIPMAP) mlevels = full_chain(t->w, t->h, t->d);
    td = [[MTLTextureDescriptor alloc] init];
    td.pixelFormat = (MTLPixelFormat)f->mtl;
    td.width = t->w;
    td.height = t->h;
    td.depth = 1;
    td.mipmapLevelCount = mlevels;
    td.textureType = t->kind == MT_K_CUBE ? MTLTextureTypeCube : t->kind == MT_K_VOLTEX ? MTLTextureType3D : MTLTextureType2D;
    if (t->kind == MT_K_VOLTEX) td.depth = t->d;
    MTLTextureUsage u = MTLTextureUsageShaderRead;
    int target = (t->usage & (D3DUSAGE_RENDERTARGET | D3DUSAGE_DEPTHSTENCIL)) || (f->flags & MT_FMT_DEPTH) ||
                 (t->kind == MT_K_SURF && t->pool == D3DPOOL_DEFAULT && (f->flags & MT_FMT_RENDERABLE));
    if (target || (t->usage & D3DUSAGE_AUTOGENMIPMAP)) u |= MTLTextureUsageRenderTarget;
    if (!swz_identity(f)) u |= MTLTextureUsagePixelFormatView;
    td.usage = u;
    td.storageMode = (target || (f->flags & MT_FMT_DEPTH)) ? MTLStorageModePrivate
                     : d->mtl.hasUnifiedMemory                ? MTLStorageModeShared
                                                              : MTLStorageModeManaged;
    t->tex = [d->mtl newTextureWithDescriptor:td];
    [td release];
    if (!t->tex) {
        MT_LOG("newTextureWithDescriptor failed: kind %d %ux%ux%u levels %u fmt %u usage 0x%x pool %u\n", t->kind, t->w, t->h, t->d,
               mlevels, t->fmt, t->usage, t->pool);
        return -1;
    }
    if (!swz_identity(f)) {
        MTLTextureSwizzleChannels sw = MTLTextureSwizzleChannelsMake((MTLTextureSwizzle)f->swz[0], (MTLTextureSwizzle)f->swz[1],
                                                                     (MTLTextureSwizzle)f->swz[2], (MTLTextureSwizzle)f->swz[3]);
        NSUInteger slices = t->kind == MT_K_CUBE ? 6 : 1;
        t->view = [t->tex newTextureViewWithPixelFormat:t->tex.pixelFormat textureType:t->tex.textureType
                                                 levels:NSMakeRange(0, mlevels) slices:NSMakeRange(0, slices) swizzle:sw];
    }
    if (!t->view) t->view = [t->tex retain];
    t->metal_bytes = t->tex.allocatedSize;
    g_mst.metal_bytes += t->metal_bytes;
    if (g_mt_trace >= 1 && t->bytes >= (4u << 20))
        MT_LOG("big texture: kind %d %ux%ux%u levels %u fmt %u usage 0x%x pool %u: %.1f MB\n", t->kind, t->w, t->h, t->d, t->levels,
               t->fmt, t->usage, t->pool, t->bytes / 1048576.0);
    return 0;
}
extern "C" uint32_t mt_texture_new(mt_dev *d, int kind, uint32_t w, uint32_t h, uint32_t depth, uint32_t levels, uint32_t usage,
                                   uint32_t fmt, uint32_t pool, uint32_t ms, uint32_t msq, mt_obj **out) {
    uint32_t rtype = kind == MT_K_TEX ? D3DRTYPE_TEXTURE : kind == MT_K_CUBE ? D3DRTYPE_CUBETEXTURE
                     : kind == MT_K_VOLTEX ? D3DRTYPE_VOLUMETEXTURE : D3DRTYPE_SURFACE;
    uint32_t maxl = full_chain(w, h, depth);
    mt_obj *t;
    *out = NULL;
    if (!w || !h || !depth || w > 16384 || h > 16384 || depth > 2048) return D3DERR_INVALIDCALL;
    if (usage & D3DUSAGE_AUTOGENMIPMAP) {
        if (levels > 1) return D3DERR_INVALIDCALL;
        levels = 1;
    }
    if (!levels || levels > maxl) levels = maxl;
    if (levels > MT_MAXLV) levels = MT_MAXLV;
    if (pool > D3DPOOL_SCRATCH) return D3DERR_INVALIDCALL;
    if ((usage & (D3DUSAGE_RENDERTARGET | D3DUSAGE_DEPTHSTENCIL)) && pool != D3DPOOL_DEFAULT) return D3DERR_INVALIDCALL;
    if (pool != D3DPOOL_SCRATCH && rtype != D3DRTYPE_SURFACE &&
        d3d9_check_format(D3DFMT_X8R8G8B8, usage & ~D3DUSAGE_AUTOGENMIPMAP, rtype, fmt) == D3DERR_NOTAVAILABLE)
        return D3DERR_NOTAVAILABLE;
    if (ms > 1) { /* the game never asks for multisampling (its measured D3D9 usage); single-sampled (DEVIATIONS 3.17) */
        static int once;
        if (!once++) MT_LOG("multisample type %u requested: created single-sampled\n", ms);
    }
    t = mt_new(kind, d, NULL);
    if (!t) return SG_E_OUTOFMEMORY;
    t->w = w, t->h = h, t->d = depth, t->levels = levels, t->faces = kind == MT_K_CUBE ? 6 : 1;
    t->usage = usage, t->fmt = fmt, t->pool = pool, t->ms = ms, t->msq = msq;
    t->mf = mt_format_of(fmt);
    t->autogen_filter = 2; /* D3DTEXF_LINEAR */
    t->shadow_valid = 1;
    set_layout(t);
    if (!t->bytes) {
        mt_obj_free(t);
        return D3DERR_OUTOFVIDEOMEMORY;
    }
    if (!t->mf && metal_backed(pool)) {
        static int once;
        if (!once++ || g_mt_trace >= 1) MT_LOG("format %u has no Metal format: kind %d %ux%u pool %u kept in guest memory only\n", fmt, kind, w, h, pool);
    }
    if (make_texture(d, t) < 0) {
        mt_obj_free(t);
        return D3DERR_OUTOFVIDEOMEMORY;
    }
    *out = t;
    return D3D_OK;
}
extern "C" uint32_t mt_buffer_new(mt_dev *d, int kind, uint32_t len, uint32_t usage, uint32_t fmt_or_fvf, uint32_t pool, mt_obj **out) {
    mt_obj *b;
    *out = NULL;
    if (!len || len > 0x10000000u || pool > D3DPOOL_SCRATCH) return D3DERR_INVALIDCALL;
    if (kind == MT_K_IB && fmt_or_fvf != D3DFMT_INDEX16 && fmt_or_fvf != D3DFMT_INDEX32) return D3DERR_INVALIDCALL;
    b = mt_new(kind, d, NULL);
    if (!b) return SG_E_OUTOFMEMORY;
    b->w = len, b->h = b->d = b->levels = b->faces = 1;
    b->usage = usage, b->pool = pool;
    if (kind == MT_K_IB) b->fmt = fmt_or_fvf;
    else b->fvf = fmt_or_fvf, b->fmt = D3DFMT_VERTEXDATA;
    b->bytes = (len + 15) & ~15u;
    b->shadow_valid = 1;
    *out = b;
    return D3D_OK;
}
extern "C" void mt_res_release_metal(mt_obj *o) {
    mt_dev *d = o->dev;
    if (o->metal_bytes) g_mst.metal_bytes -= o->metal_bytes;
    o->metal_bytes = 0;
    if (o->view) mt_retire(d, o->view);
    if (o->tex) mt_retire(d, o->tex);
    if (o->buf) mt_retire(d, o->buf);
    for (unsigned i = 0; i < o->nspare; i++) mt_retire(d, o->spare[i].buf);
    o->view = nil, o->tex = nil, o->buf = nil, o->nspare = 0;
}
extern "C" uint32_t mt_shadow_of(mt_obj *t) {
    if (!t->shadow && t->bytes) {
        t->shadow = sg_com_galloc(t->bytes);
        if (t->shadow) {
            nst()->resource_bytes += t->bytes;
            g_mst.shadow_bytes += t->bytes;
        }
    }
    return t->shadow;
}
/* Level child of a container, created on demand; its references count on the container. */
extern "C" mt_obj *mt_sub(mt_obj *t, uint32_t face, uint32_t level) {
    for (unsigned i = 0; i < t->nkids; i++)
        if (t->kids[i]->face == face && t->kids[i]->level == level) return t->kids[i];
    mt_obj *s = mt_new(t->kind == MT_K_VOLTEX ? MT_K_VOL : MT_K_SURF, t->dev, t);
    if (!s) return NULL;
    s->face = face, s->level = level;
    s->fmt = t->fmt, s->usage = t->usage, s->pool = t->pool, s->mf = t->mf;
    s->w = lvl_dim(t->w, level), s->h = lvl_dim(t->h, level), s->d = lvl_dim(t->d, level);
    s->levels = 1, s->faces = 1;
    return s;
}

/* ---- uploads and readback ------------------------------------------------------------------------------ */
/* Sends a region of guest-layout data to (face, level) of t's Metal texture at (x, y, z), w x h x depth texels. */
static int upload(mt_dev *d, mt_obj *t, uint32_t face, uint32_t level, uint32_t x, uint32_t y, uint32_t z, uint32_t w, uint32_t h,
                  uint32_t depth, const uint8_t *src, uint32_t spitch, uint32_t sslice) {
    const mt_format *f = t->mf;
    uint32_t rows, rowbytes, mrowbytes;
    if (!t->tex || !f || !w || !h || !depth) return 0;
    if (f->flags & MT_FMT_DEPTH) {
        static int once;
        if (!once++) MT_LOG("upload into a depth format (%u) is not supported: ignored\n", t->fmt);
        return 0;
    }
    if (f->block) {
        uint32_t lw = lvl_dim(t->w, level), lh = lvl_dim(t->h, level);
        /* a block region: whole blocks, clamped to the level */
        uint32_t bx0 = x / 4, by0 = y / 4, bx1 = (x + w + 3) / 4, by1 = (y + h + 3) / 4;
        x = bx0 * 4, y = by0 * 4;
        w = bx1 * 4 - x, h = by1 * 4 - y;
        if (x + w > lw) w = lw - x;
        if (y + h > lh) h = lh - y;
        rows = by1 - by0;
        rowbytes = mrowbytes = (bx1 - bx0) * f->block;
    } else {
        rows = h;
        rowbytes = w * f->guest_bpp;
        mrowbytes = w * f->mtl_bpp;
    }
    uint64_t t0 = mt_now_ns();
    uint8_t *conv = NULL;
    const uint8_t *data = src;
    uint32_t dpitch = spitch, dslice = sslice;
    if (f->flags & MT_FMT_CONVERT) {
        conv = (uint8_t *)malloc((size_t)mrowbytes * rows * depth);
        if (!conv) return -1;
        for (uint32_t zz = 0; zz < depth; zz++)
            for (uint32_t r = 0; r < rows; r++)
                conv_up(f, src + (size_t)zz * sslice + (size_t)r * spitch, conv + ((size_t)zz * rows + r) * mrowbytes, w);
        data = conv, dpitch = mrowbytes, dslice = mrowbytes * rows;
    }
    int busy = t->used_serial > completed_of(d) || t->tex.storageMode == MTLStorageModePrivate;
    MTLRegion reg = MTLRegionMake3D(x, y, t->kind == MT_K_VOLTEX ? z : 0, w, h, t->kind == MT_K_VOLTEX ? depth : 1);
    if (!busy) {
        [t->tex replaceRegion:reg mipmapLevel:level slice:t->kind == MT_K_CUBE ? face : 0 withBytes:data bytesPerRow:dpitch
                bytesPerImage:t->kind == MT_K_VOLTEX ? dslice : 0];
        g_mst.direct_uploads++;
    } else {
        uint32_t off = 0, total = mrowbytes * rows * depth;
        void *host = NULL;
        id<MTLBuffer> sb = mt_scratch(d, total, 16, &off, &host);
        if (!sb) {
            free(conv);
            return -1;
        }
        for (uint32_t zz = 0; zz < depth; zz++)
            for (uint32_t r = 0; r < rows; r++)
                memcpy((uint8_t *)host + ((size_t)zz * rows + r) * mrowbytes, data + (size_t)zz * dslice + (size_t)r * dpitch, mrowbytes);
        id<MTLBlitCommandEncoder> be = mt_blit(d);
        [be copyFromBuffer:sb sourceOffset:off sourceBytesPerRow:mrowbytes sourceBytesPerImage:mrowbytes * rows
                sourceSize:reg.size toTexture:t->tex destinationSlice:t->kind == MT_K_CUBE ? face : 0 destinationLevel:level
         destinationOrigin:reg.origin];
        mt_mark_used(d, t);
        g_mst.staged_uploads++;
    }
    free(conv);
    g_mst.bytes_out += (uint64_t)rowbytes * rows * depth;
    g_mst.copy_ns += mt_now_ns() - t0;
    if (level == 0 && (t->usage & D3DUSAGE_AUTOGENMIPMAP)) t->mips_dirty = 1;
    if (g_mt_trace >= 2)
        MT_LOG("upload kind %d fmt %u face %u level %u (%u,%u,%u) %ux%ux%u %s\n", t->kind, t->fmt, face, level, x, y, z, w, h, depth,
               busy ? "staged" : "direct");
    return 0;
}
/* Box x0,y0,x1,y1,z0,z1 of (face, level) of t from its own shadow. */
extern "C" int mt_upload_region(mt_dev *d, mt_obj *t, uint32_t face, uint32_t level, const uint32_t box[6]) {
    uint32_t base = t->shadow;
    unsigned block = t->mf ? t->mf->block : 0, bpp = t->mf ? t->mf->guest_bpp : 4;
    if (!base || !t->tex) return 0;
    base += face * t->face_bytes + t->lvl_off[level] + box[4] * t->lvl_slice[level];
    if (block) base += (box[1] / 4) * t->lvl_pitch[level] + (box[0] / 4) * block;
    else base += box[1] * t->lvl_pitch[level] + box[0] * bpp;
    return upload(d, t, face, level, box[0], box[1], box[4], box[2] - box[0], box[3] - box[1], box[5] - box[4], sg_com_mem + base,
                  t->lvl_pitch[level], t->lvl_slice[level]);
}
extern "C" int mt_upload_all(mt_dev *d, mt_obj *t) {
    if (!t->shadow) return 0;
    for (uint32_t f = 0; f < t->faces; f++)
        for (uint32_t l = 0; l < t->levels; l++) {
            uint32_t box[6] = {0, 0, lvl_dim(t->w, l), lvl_dim(t->h, l), 0, lvl_dim(t->d, l)};
            if (mt_upload_region(d, t, f, l, box)) return -1;
        }
    return 0;
}
/* (face, level) of t's Metal texture into guest memory at dst (guest layout, pitch dpitch, slice dslice). */
static int readback(mt_dev *d, mt_obj *t, uint32_t face, uint32_t level, uint8_t *dst, uint32_t dpitch, uint32_t dslice) {
    const mt_format *f = t->mf;
    uint32_t lw = lvl_dim(t->w, level), lh = lvl_dim(t->h, level), ld = t->kind == MT_K_VOLTEX ? lvl_dim(t->d, level) : 1;
    uint32_t rows = f && f->block ? (lh + 3) / 4 : lh;
    uint32_t mrow = f ? (f->block ? ((lw + 3) / 4) * f->block : lw * f->mtl_bpp) : 0;
    if (!t->tex || !f) return -1;
    uint64_t t0 = mt_now_ns();
    uint8_t *tmp = (uint8_t *)malloc((size_t)mrow * rows * ld);
    if (!tmp) return -1;
    if (t->tex.storageMode != MTLStorageModePrivate && !(f->flags & MT_FMT_DEPTH)) {
        if (t->used_serial > completed_of(d)) mt_wait_serial(d, t->used_serial);
        [t->tex getBytes:tmp bytesPerRow:mrow bytesPerImage:mrow * rows fromRegion:MTLRegionMake3D(0, 0, 0, lw, lh, ld) mipmapLevel:level
                   slice:t->kind == MT_K_CUBE ? face : 0];
    } else {
        id<MTLBuffer> rb = [d->mtl newBufferWithLength:(NSUInteger)mrow * rows * ld options:MTLResourceStorageModeShared];
        if (!rb) {
            free(tmp);
            return -1;
        }
        id<MTLBlitCommandEncoder> be = mt_blit(d);
        MTLBlitOption opt = MTLBlitOptionNone;
        if ((f->flags & MT_FMT_DEPTH) && (f->flags & MT_FMT_STENCIL)) opt = MTLBlitOptionDepthFromDepthStencil;
        uint32_t row = (f->flags & MT_FMT_STENCIL) ? lw * 4 : mrow; /* the depth plane alone: 4 bytes per texel */
        [be copyFromTexture:t->tex sourceSlice:t->kind == MT_K_CUBE ? face : 0 sourceLevel:level sourceOrigin:MTLOriginMake(0, 0, 0)
                 sourceSize:MTLSizeMake(lw, lh, ld) toBuffer:rb destinationOffset:0 destinationBytesPerRow:row
       destinationBytesPerImage:row * rows options:opt];
        mt_mark_used(d, t);
        mt_flush(d, 1);
        if (f->flags & MT_FMT_STENCIL) { /* spread to the 8-byte Metal layout conv_down expects */
            const uint8_t *s = (const uint8_t *)rb.contents;
            for (uint32_t i = 0; i < lw * rows * ld; i++) memcpy(tmp + 8 * (size_t)i, s + 4 * (size_t)i, 4);
        } else {
            memcpy(tmp, rb.contents, (size_t)mrow * rows * ld);
        }
        [rb release];
    }
    for (uint32_t z = 0; z < ld; z++)
        for (uint32_t r = 0; r < rows; r++) {
            const uint8_t *s = tmp + ((size_t)z * rows + r) * mrow;
            uint8_t *o = dst + (size_t)z * dslice + (size_t)r * dpitch;
            if (f->flags & MT_FMT_CONVERT) conv_down(f, s, o, lw);
            else memcpy(o, s, mrow);
        }
    free(tmp);
    g_mst.readbacks++;
    g_mst.bytes_in += (uint64_t)mrow * rows * ld;
    g_mst.copy_ns += mt_now_ns() - t0;
    return 0;
}
extern "C" int mt_readback_all(mt_dev *d, mt_obj *t) {
    uint32_t base = mt_shadow_of(t);
    if (!base || !t->tex) return -1;
    for (uint32_t f = 0; f < t->faces; f++)
        for (uint32_t l = 0; l < t->levels; l++)
            if (readback(d, t, f, l, sg_com_mem + base + f * t->face_bytes + t->lvl_off[l], t->lvl_pitch[l], t->lvl_slice[l])) return -1;
    t->gpu_dirty = 0;
    return 0;
}
static void gen_mips(mt_dev *d, mt_obj *t) {
    if (!t->tex || t->tex.mipmapLevelCount < 2) {
        t->mips_dirty = 0;
        return;
    }
    id<MTLBlitCommandEncoder> be = mt_blit(d);
    [be generateMipmapsForTexture:t->tex];
    mt_mark_used(d, t);
    t->mips_dirty = 0;
}
extern "C" MT_ID(MTLTexture) mt_texture_of(mt_dev *d, mt_obj *t) {
    if (!t) return nil;
    if (t->upload_pending) {
        t->upload_pending = 0;
        mt_upload_all(d, t);
    }
    if (t->mips_dirty) gen_mips(d, t);
    return t->view;
}
extern "C" MT_ID(MTLBuffer) mt_buffer_of(mt_dev *d, mt_obj *b) {
    if (!b) return nil;
    if (!b->buf && d->mtl) {
        uint32_t base = mt_shadow_of(b);
        b->buf = [d->mtl newBufferWithLength:b->bytes options:MTLResourceStorageModeShared];
        if (b->buf && base) memcpy(b->buf.contents, sg_com_mem + base, b->w);
        b->metal_bytes = b->bytes;
        g_mst.metal_bytes += b->bytes;
    }
    return b->buf;
}

/* ---- locks -------------------------------------------------------------------------------------------- */
static mt_lockrec *lock_push(mt_obj *t) {
    if (t->nlocks == t->caplocks) {
        t->caplocks = t->caplocks ? t->caplocks * 2 : 4;
        t->locks = (mt_lockrec *)realloc(t->locks, t->caplocks * sizeof *t->locks);
    }
    return &t->locks[t->nlocks++];
}
static int lock_pop(mt_obj *t, uint32_t face, uint32_t level, mt_lockrec *out) {
    for (unsigned i = t->nlocks; i-- > 0;)
        if (t->locks[i].face == face && t->locks[i].level == level) {
            *out = t->locks[i];
            t->locks[i] = t->locks[--t->nlocks];
            return 1;
        }
    return 0;
}
static void read_box(uint8_t *mem, uint32_t p, int is_box, uint32_t *box) {
    box[0] = sg_ld32(mem, p), box[1] = sg_ld32(mem, p + 4), box[2] = sg_ld32(mem, p + 8), box[3] = sg_ld32(mem, p + 12);
    box[4] = is_box ? sg_ld32(mem, p + 16) : 0;
    box[5] = is_box ? sg_ld32(mem, p + 20) : 1;
}
/* D3D9's lockability (DEVIATIONS 3.10): DEFAULT-pool textures only when DYNAMIC; render targets only when
 * created lockable (or the back buffer with D3DPRESENTFLAG_LOCKABLE_BACKBUFFER); depth only in the
 * lockable depth formats. */
static int lockable(mt_obj *t, mt_obj *surf) {
    if (t->pool != D3DPOOL_DEFAULT) return 1;
    if (t->usage & D3DUSAGE_DEPTHSTENCIL) return t->fmt == D3DFMT_D16_LOCKABLE || t->fmt == D3DFMT_D32F_LOCKABLE;
    if (t->usage & D3DUSAGE_RENDERTARGET) return t->kind == MT_K_SURF && (t->lockable || (surf && surf->lockable));
    if (t->kind == MT_K_SURF) return 1; /* offscreen plain DEFAULT surface */
    return (t->usage & D3DUSAGE_DYNAMIC) != 0;
}
/* LockRect/LockBox on (face, level) of container (or standalone surface) t. */
static uint32_t lock_common(sg_com_call *c, mt_obj *t, mt_obj *surf, uint32_t face, uint32_t level, uint32_t prect, int is_box,
                            uint32_t flags, uint32_t *addr, uint32_t *pitch, uint32_t *slice) {
    const mt_format *f = t->mf;
    unsigned block = 0, bits = d3d9null_fmt_bits(t->fmt, &block);
    uint32_t lw = lvl_dim(t->w, level), lh = lvl_dim(t->h, level), ld = lvl_dim(t->d, level);
    uint32_t box[6] = {0, 0, lw, lh, 0, ld}, base, a;
    if (level >= t->levels || face >= t->faces) return D3DERR_INVALIDCALL;
    if (!lockable(t, surf)) return D3DERR_INVALIDCALL;
    if (!bits) bits = 32;
    if (prect) {
        read_box(MEM, prect, is_box, box);
        if (box[2] > lw) box[2] = lw;
        if (box[3] > lh) box[3] = lh;
        if (box[5] > ld) box[5] = ld;
        if (box[0] > box[2]) box[0] = box[2];
        if (box[1] > box[3]) box[1] = box[3];
        if (box[4] > box[5]) box[4] = box[5];
    }
    base = mt_shadow_of(t);
    if (!base) return D3DERR_OUTOFVIDEOMEMORY;
    if (t->gpu_dirty && t->tex) {
        mt_dev *d = t->dev;
        if (flags & D3DLOCK_DISCARD) t->gpu_dirty = 0; /* contents undefined: the shadow is as good */
        else if (d && d->alive) mt_readback_all(d, t);
    }
    base += face * t->face_bytes + t->lvl_off[level];
    if (block) a = base + box[4] * t->lvl_slice[level] + (box[1] / 4) * t->lvl_pitch[level] + (box[0] / 4) * block;
    else a = base + box[4] * t->lvl_slice[level] + box[1] * t->lvl_pitch[level] + box[0] * (bits / 8);
    (void)f;
    mt_lockrec *L = lock_push(t);
    L->face = face, L->level = level, L->flags = flags;
    L->x0 = box[0], L->y0 = box[1], L->x1 = box[2], L->y1 = box[3], L->z0 = box[4], L->z1 = box[5];
    g_mst.locks++;
    nst()->locks++;
    *addr = a;
    *pitch = t->lvl_pitch[level];
    if (slice) *slice = t->lvl_slice[level];
    return D3D_OK;
}
static void dirty_union(mt_obj *t, const uint32_t *b, uint32_t level) {
    uint32_t s[6] = {b[0] << level, b[1] << level, b[2] << level, b[3] << level, b[4] << level, b[5] << level};
    if (!t->upload_pending) memcpy(t->dirty, s, sizeof s);
    else
        for (int i = 0; i < 6; i++) t->dirty[i] = (i & 1) == 0 && i < 4 ? (s[i] < t->dirty[i] ? s[i] : t->dirty[i])
                                                                        : (s[i] > t->dirty[i] ? s[i] : t->dirty[i]);
    t->upload_pending = 1;
}
static uint32_t unlock_common(mt_obj *t, uint32_t face, uint32_t level) {
    mt_lockrec L;
    if (level >= t->levels || face >= t->faces) return D3DERR_INVALIDCALL;
    if (!lock_pop(t, face, level, &L)) return D3D_OK; /* D3D9 tolerates an unmatched Unlock on these paths */
    g_mst.unlocks++;
    if (L.flags & D3DLOCK_READONLY) return D3D_OK;
    if (!t->tex) return D3D_OK; /* SYSTEMMEM / SCRATCH: the shadow is the resource */
    if (L.flags & D3DLOCK_NO_DIRTY_UPDATE) {
        uint32_t b[6] = {L.x0, L.y0, L.x1, L.y1, L.z0, L.z1};
        dirty_union(t, b, level);
        return D3D_OK;
    }
    mt_dev *d = t->dev;
    if (!d || !d->alive) return D3D_OK;
    uint32_t box[6] = {L.x0, L.y0, L.x1, L.y1, L.z0, L.z1};
    mt_upload_region(d, t, face, level, box);
    return D3D_OK;
}

/* ---- resource interface methods ------------------------------------------------------------------- */
extern "C" uint32_t mt_r_get_device(sg_com_call *c) {
    mt_dev *d = O->dev;
    if (!ARG(0)) return D3DERR_INVALIDCALL;
    if (!d || !d->alive) {
        sg_st32(MEM, ARG(0), 0);
        return D3DERR_INVALIDCALL;
    }
    sg_com_obj_addref(&d->o.com);
    sg_st32(MEM, ARG(0), mt_guest_of(&d->o));
    return D3D_OK;
}
/* Private data: kept per object in a small list (D3DSPD_IUNKNOWN is refused: a guest IUnknown would need
 * an AddRef through the guest vtable). */
struct mt_pdata { sg_guid g; uint32_t size; uint8_t *data; mt_pdata *next; };
static mt_pdata **pdata_list(mt_obj *o) { return (mt_pdata **)&o->pdata; }
static uint32_t r_set_private(sg_com_call *c) {
    if (!ARG(0)) return D3DERR_INVALIDCALL;
    if (ARG(3) & 1) return SG_E_NOTIMPL;
    sg_guid g = sg_guid_load(MEM, ARG(0));
    mt_pdata **l = pdata_list(O), *p;
    for (p = *l; p; p = p->next)
        if (sg_guid_eq(&p->g, &g)) break;
    if (!p) {
        p = (mt_pdata *)calloc(1, sizeof *p);
        p->g = g;
        p->next = *l;
        *l = p;
    }
    free(p->data);
    p->size = ARG(2);
    p->data = (uint8_t *)malloc(p->size ? p->size : 1);
    if (ARG(1) && p->size) memcpy(p->data, MEM + ARG(1), p->size);
    return D3D_OK;
}
static uint32_t r_get_private(sg_com_call *c) { /* (guid, data, DWORD *size) */
    if (!ARG(0) || !ARG(2)) return D3DERR_INVALIDCALL;
    sg_guid g = sg_guid_load(MEM, ARG(0));
    for (mt_pdata *p = *pdata_list(O); p; p = p->next)
        if (sg_guid_eq(&p->g, &g)) {
            uint32_t have = sg_ld32(MEM, ARG(2));
            sg_st32(MEM, ARG(2), p->size);
            if (!ARG(1)) return D3D_OK;
            if (have < p->size) return D3DERR_MOREDATA;
            memcpy(MEM + ARG(1), p->data, p->size);
            return D3D_OK;
        }
    return D3DERR_NOTFOUND;
}
static uint32_t r_free_private(sg_com_call *c) {
    if (!ARG(0)) return D3DERR_INVALIDCALL;
    sg_guid g = sg_guid_load(MEM, ARG(0));
    for (mt_pdata **pp = pdata_list(O); *pp; pp = &(*pp)->next)
        if (sg_guid_eq(&(*pp)->g, &g)) {
            mt_pdata *p = *pp;
            *pp = p->next;
            free(p->data);
            free(p);
            return D3D_OK;
        }
    return D3DERR_NOTFOUND;
}
extern "C" void mt_private_free_all(mt_obj *o) {
    mt_pdata **l = pdata_list(o);
    while (*l) {
        mt_pdata *p = *l;
        *l = p->next;
        free(p->data);
        free(p);
    }
}
static uint32_t r_set_priority(sg_com_call *c) { uint32_t o = O->priority; O->priority = ARG(0); return o; }
static uint32_t r_get_priority(sg_com_call *c) { return O->priority; }
static uint32_t r_preload(sg_com_call *c) {
    if (O->dev && O->dev->alive && O->tex) mt_texture_of(O->dev, O);
    return 0;
}
static uint32_t r_get_type(sg_com_call *c) {
    switch (O->kind) {
    case MT_K_TEX: return D3DRTYPE_TEXTURE;
    case MT_K_CUBE: return D3DRTYPE_CUBETEXTURE;
    case MT_K_VOLTEX: return D3DRTYPE_VOLUMETEXTURE;
    case MT_K_SURF: return D3DRTYPE_SURFACE;
    case MT_K_VOL: return D3DRTYPE_VOLUME;
    case MT_K_VB: return D3DRTYPE_VERTEXBUFFER;
    case MT_K_IB: return D3DRTYPE_INDEXBUFFER;
    default: return 0;
    }
}
static uint32_t t_set_lod(sg_com_call *c) {
    uint32_t o = O->lod;
    if (O->pool == D3DPOOL_MANAGED) O->lod = ARG(0) < O->levels ? ARG(0) : O->levels - 1;
    return o;
}
static uint32_t t_get_lod(sg_com_call *c) { return O->lod; }
static uint32_t t_level_count(sg_com_call *c) { return O->levels; }
static uint32_t t_set_filter(sg_com_call *c) {
    if (!ARG(0)) return D3DERR_INVALIDCALL;
    O->autogen_filter = ARG(0);
    return D3D_OK;
}
static uint32_t t_get_filter(sg_com_call *c) { return O->autogen_filter; }
static uint32_t t_gen_mips(sg_com_call *c) {
    if ((O->usage & D3DUSAGE_AUTOGENMIPMAP) && O->dev && O->dev->alive) {
        if (O->upload_pending) mt_texture_of(O->dev, O);
        gen_mips(O->dev, O);
    }
    return 0;
}
#define RESOURCE_METHODS(S)                                                                                     \
    SG_COM_IUNKNOWN, M(S, GetDevice, mt_r_get_device), M(S, SetPrivateData, r_set_private),                    \
        M(S, GetPrivateData, r_get_private), M(S, FreePrivateData, r_free_private),                            \
        M(S, SetPriority, r_set_priority), M(S, GetPriority, r_get_priority), M(S, PreLoad, r_preload),        \
        M(S, GetType, r_get_type)
#define BASETEX_METHODS(S)                                                                                      \
    RESOURCE_METHODS(S), M(S, SetLOD, t_set_lod), M(S, GetLOD, t_get_lod), M(S, GetLevelCount, t_level_count), \
        M(S, SetAutoGenFilterType, t_set_filter), M(S, GetAutoGenFilterType, t_get_filter),                    \
        M(S, GenerateMipSubLevels, t_gen_mips)

static void put_surface_desc(sg_com_call *c, uint32_t p, uint32_t fmt, uint32_t usage, uint32_t pool, uint32_t ms, uint32_t msq,
                             uint32_t w, uint32_t h) {
    uint32_t v[8] = {fmt, D3DRTYPE_SURFACE, usage, pool, ms, msq, w, h};
    for (int i = 0; i < 8; i++) sg_st32(MEM, p + 4u * i, v[i]);
}
static uint32_t put_lr(sg_com_call *c, uint32_t plr, uint32_t r, uint32_t pitch, uint32_t a) {
    if (r) {
        sg_st32(MEM, plr, 0);
        sg_st32(MEM, plr + 4, 0);
        return r;
    }
    sg_st32(MEM, plr, pitch);
    sg_st32(MEM, plr + 4, a);
    return D3D_OK;
}
static uint32_t tex_level_desc(sg_com_call *c) { /* (level, D3DSURFACE_DESC*) */
    uint32_t l = ARG(0);
    if (l >= O->levels || !ARG(1)) return D3DERR_INVALIDCALL;
    put_surface_desc(c, ARG(1), O->fmt, O->usage, O->pool, 0, 0, lvl_dim(O->w, l), lvl_dim(O->h, l));
    return D3D_OK;
}
static uint32_t tex_surface_level(sg_com_call *c) { /* (level, IDirect3DSurface9**) */
    mt_obj *s;
    if (ARG(0) >= O->levels || !ARG(1)) return D3DERR_INVALIDCALL;
    s = mt_sub(O, 0, ARG(0));
    if (!s) return SG_E_OUTOFMEMORY;
    sg_com_obj_addref(&s->com);
    sg_st32(MEM, ARG(1), mt_guest_of(s));
    return D3D_OK;
}
static uint32_t tex_lock(sg_com_call *c) { /* (level, lr, rect, flags) */
    uint32_t a = 0, p = 0;
    if (!ARG(1)) return D3DERR_INVALIDCALL;
    return put_lr(c, ARG(1), lock_common(c, O, NULL, 0, ARG(0), ARG(2), 0, ARG(3), &a, &p, NULL), p, a);
}
static uint32_t tex_unlock(sg_com_call *c) { return unlock_common(O, 0, ARG(0)); }
static uint32_t tex_dirty(sg_com_call *c) { /* (RECT*): NULL = everything */
    uint32_t b[6] = {0, 0, O->w, O->h, 0, 1};
    if (ARG(0)) read_box(MEM, ARG(0), 0, b);
    if (O->tex && O->shadow) dirty_union(O, b, 0);
    return D3D_OK;
}

static uint32_t cube_surface(sg_com_call *c) { /* (face, level, IDirect3DSurface9**) */
    mt_obj *s;
    if (ARG(0) >= 6 || ARG(1) >= O->levels || !ARG(2)) return D3DERR_INVALIDCALL;
    s = mt_sub(O, ARG(0), ARG(1));
    if (!s) return SG_E_OUTOFMEMORY;
    sg_com_obj_addref(&s->com);
    sg_st32(MEM, ARG(2), mt_guest_of(s));
    return D3D_OK;
}
static uint32_t cube_lock(sg_com_call *c) { /* (face, level, lr, rect, flags) */
    uint32_t a = 0, p = 0;
    if (!ARG(2)) return D3DERR_INVALIDCALL;
    return put_lr(c, ARG(2), lock_common(c, O, NULL, ARG(0), ARG(1), ARG(3), 0, ARG(4), &a, &p, NULL), p, a);
}
static uint32_t cube_unlock(sg_com_call *c) { return unlock_common(O, ARG(0), ARG(1)); }
static uint32_t cube_dirty(sg_com_call *c) { /* (face, RECT*) */
    uint32_t b[6] = {0, 0, O->w, O->h, 0, 1};
    if (ARG(1)) read_box(MEM, ARG(1), 0, b);
    if (O->tex && O->shadow) dirty_union(O, b, 0);
    return D3D_OK;
}

static void put_volume_desc(sg_com_call *c, uint32_t p, mt_obj *t, uint32_t l) {
    uint32_t v[7] = {t->fmt, D3DRTYPE_VOLUME, t->usage, t->pool, lvl_dim(t->w, l), lvl_dim(t->h, l), lvl_dim(t->d, l)};
    for (int i = 0; i < 7; i++) sg_st32(MEM, p + 4u * i, v[i]);
}
static uint32_t vt_level_desc(sg_com_call *c) {
    if (ARG(0) >= O->levels || !ARG(1)) return D3DERR_INVALIDCALL;
    put_volume_desc(c, ARG(1), O, ARG(0));
    return D3D_OK;
}
static uint32_t vt_volume_level(sg_com_call *c) {
    mt_obj *s;
    if (ARG(0) >= O->levels || !ARG(1)) return D3DERR_INVALIDCALL;
    s = mt_sub(O, 0, ARG(0));
    if (!s) return SG_E_OUTOFMEMORY;
    sg_com_obj_addref(&s->com);
    sg_st32(MEM, ARG(1), mt_guest_of(s));
    return D3D_OK;
}
static uint32_t put_lb(sg_com_call *c, uint32_t plb, uint32_t r, uint32_t pitch, uint32_t slice, uint32_t a) {
    if (r) return r;
    sg_st32(MEM, plb, pitch);
    sg_st32(MEM, plb + 4, slice);
    sg_st32(MEM, plb + 8, a);
    return D3D_OK;
}
static uint32_t vt_lock(sg_com_call *c) { /* (level, lb, box, flags) */
    uint32_t a = 0, p = 0, s = 0;
    if (!ARG(1)) return D3DERR_INVALIDCALL;
    uint32_t r = lock_common(c, O, NULL, 0, ARG(0), ARG(2), 1, ARG(3), &a, &p, &s);
    return put_lb(c, ARG(1), r, p, s, a);
}
static uint32_t vt_unlock(sg_com_call *c) { return unlock_common(O, 0, ARG(0)); }
static uint32_t vt_dirty(sg_com_call *c) {
    uint32_t b[6] = {0, 0, O->w, O->h, 0, O->d};
    if (ARG(0)) read_box(MEM, ARG(0), 1, b);
    if (O->tex && O->shadow) dirty_union(O, b, 0);
    return D3D_OK;
}

static uint32_t sub_container(sg_com_call *c) { /* (REFIID, void**) */
    uint32_t g = 0;
    sg_guid iid;
    if (!ARG(0) || !ARG(1)) return D3DERR_INVALIDCALL;
    iid = sg_guid_load(MEM, ARG(0));
    mt_obj *p = O->parent;
    sg_st32(MEM, ARG(1), 0);
    if (p && p->kind != MT_K_DEV && (sg_guid_eq(&iid, &mt_iface[p->kind].iid) || sg_guid_eq(&iid, &SG_IID_IUnknown)))
        g = mt_guest_of(p);
    else if (O->dev && O->dev->alive && sg_guid_eq(&iid, &mt_iface[MT_K_DEV].iid) && (!p || p->kind == MT_K_DEV))
        g = mt_guest_of(&O->dev->o);
    if (!g) return SG_E_NOINTERFACE;
    sg_com_obj *o = sg_com_lookup(MEM, g);
    if (o) sg_com_obj_addref(o);
    sg_st32(MEM, ARG(1), g);
    return D3D_OK;
}
static uint32_t vol_desc(sg_com_call *c) {
    if (!ARG(0)) return D3DERR_INVALIDCALL;
    put_volume_desc(c, ARG(0), O->parent, O->level);
    return D3D_OK;
}
static uint32_t vol_lock(sg_com_call *c) { /* (lb, box, flags) */
    uint32_t a = 0, p = 0, s = 0;
    if (!ARG(0)) return D3DERR_INVALIDCALL;
    uint32_t r = lock_common(c, O->parent, NULL, 0, O->level, ARG(1), 1, ARG(2), &a, &p, &s);
    return put_lb(c, ARG(0), r, p, s, a);
}
static uint32_t vol_unlock(sg_com_call *c) { return unlock_common(O->parent, 0, O->level); }

static uint32_t surf_desc(sg_com_call *c) {
    if (!ARG(0)) return D3DERR_INVALIDCALL;
    put_surface_desc(c, ARG(0), O->fmt, O->usage, O->pool, O->ms, O->msq, O->w, O->h);
    return D3D_OK;
}
static uint32_t surf_lock(sg_com_call *c) { /* (lr, rect, flags) */
    uint32_t f, l, a = 0, p = 0;
    if (!ARG(0)) return D3DERR_INVALIDCALL;
    mt_obj *t = mt_top(O, &f, &l);
    return put_lr(c, ARG(0), lock_common(c, t, O, f, l, ARG(1), 0, ARG(2), &a, &p, NULL), p, a);
}
static uint32_t surf_unlock(sg_com_call *c) {
    uint32_t f, l;
    mt_obj *t = mt_top(O, &f, &l);
    return unlock_common(t, f, l);
}
static uint32_t surf_get_dc(sg_com_call *c) { /* DEVIATIONS 3.5, as the null device */
    if (ARG(0)) sg_st32(MEM, ARG(0), 0);
    return D3DERR_INVALIDCALL;
}

/* vertex and index buffers */
static int g_ib_slack = -1;
static uint32_t buf_lock(sg_com_call *c) { /* (offset, size, void **ppb, flags) */
    uint32_t off = ARG(0), size = ARG(1), flags = ARG(3), base;
    if (!ARG(2)) return D3DERR_INVALIDCALL;
    if (off > O->w || (size && size > O->w - off)) {
        sg_st32(MEM, ARG(2), 0);
        return D3DERR_INVALIDCALL;
    }
    /* DEVIATIONS 3.6: Spore's terrain brushes lock a dynamic INDEX16 buffer with the index
     * count where the byte count goes and write twice that. Windows maps the buffer directly, so the
     * overrun lands; our Unlock uploads the locked range only, so a sized INDEX16 lock is widened to twice
     * its size (to the buffer end at most). The guest sees the same pointer. SPORE_METAL_IB_SLACK=0: off. */
    if (O->kind == MT_K_IB && O->fmt == D3DFMT_INDEX16 && size && !(flags & D3DLOCK_READONLY)) {
        if (g_ib_slack < 0) {
            const char *e = getenv("SPORE_METAL_IB_SLACK");
            g_ib_slack = !(e && *e == '0');
        }
        if (g_ib_slack) size = size <= (O->w - off) / 2 ? 2 * size : O->w - off;
    }
    base = mt_shadow_of(O);
    if (!base) {
        sg_st32(MEM, ARG(2), 0);
        return D3DERR_OUTOFVIDEOMEMORY;
    }
    if (!size) size = O->w - off;
    mt_lockrec *L = lock_push(O);
    L->face = 0, L->level = 0, L->flags = flags, L->x0 = off, L->x1 = off + size, L->y0 = L->z0 = 0, L->y1 = L->z1 = 1;
    g_mst.locks++;
    nst()->locks++;
    sg_st32(MEM, ARG(2), base + off);
    return D3D_OK;
}
static id<MTLBuffer> fresh_buffer(mt_dev *d, mt_obj *b) {
    uint64_t done = completed_of(d);
    for (unsigned i = 0; i < b->nspare; i++)
        if (b->spare[i].serial <= done) {
            id<MTLBuffer> r = b->spare[i].buf;
            b->spare[i] = b->spare[--b->nspare];
            return r;
        }
    id<MTLBuffer> r = [d->mtl newBufferWithLength:b->bytes options:MTLResourceStorageModeShared];
    if (r) {
        b->metal_bytes += b->bytes;
        g_mst.metal_bytes += b->bytes;
    }
    return r;
}
static uint32_t buf_unlock(sg_com_call *c) {
    mt_lockrec L;
    mt_obj *b = O;
    mt_dev *d = b->dev;
    if (!lock_pop(b, 0, 0, &L)) return D3D_OK;
    g_mst.unlocks++;
    if ((L.flags & D3DLOCK_READONLY) || !d || !d->alive || !d->mtl || !b->shadow) return D3D_OK;
    if (!b->buf) { /* first use: the buffer is made from the whole shadow */
        mt_buffer_of(d, b);
        return D3D_OK;
    }
    uint64_t t0 = mt_now_ns();
    if (b->used_serial > completed_of(d) && !(L.flags & D3DLOCK_NOOVERWRITE)) {
        /* in flight and the guest may change what the GPU still reads: rename */
        id<MTLBuffer> nb = fresh_buffer(d, b);
        if (nb) {
            if (b->nspare == 4) {
                mt_retire(d, b->spare[0].buf);
                /* the retired copy leaves the "metal" count too (without this it grew ~95 MB/s in the
                 * Creature Creator while MTLDevice.currentAllocatedSize stayed flat) */
                b->metal_bytes -= b->bytes;
                g_mst.metal_bytes -= b->bytes;
                b->spare[0] = b->spare[--b->nspare];
            }
            b->spare[b->nspare].buf = b->buf;
            b->spare[b->nspare].serial = b->used_serial;
            b->nspare++;
            b->buf = nb;
            memcpy(nb.contents, sg_com_mem + b->shadow, b->w);
            g_mst.renames++;
            g_mst.bytes_out += b->w;
            g_mst.copy_ns += mt_now_ns() - t0;
            return D3D_OK;
        }
    }
    memcpy((uint8_t *)b->buf.contents + L.x0, sg_com_mem + b->shadow + L.x0, L.x1 - L.x0);
    g_mst.bytes_out += L.x1 - L.x0;
    g_mst.copy_ns += mt_now_ns() - t0;
    return D3D_OK;
}
static uint32_t vb_desc(sg_com_call *c) {
    uint32_t v[6] = {D3DFMT_VERTEXDATA, D3DRTYPE_VERTEXBUFFER, O->usage, O->pool, O->w, O->fvf};
    if (!ARG(0)) return D3DERR_INVALIDCALL;
    for (int i = 0; i < 6; i++) sg_st32(MEM, ARG(0) + 4u * i, v[i]);
    return D3D_OK;
}
static uint32_t ib_desc(sg_com_call *c) {
    uint32_t v[5] = {O->fmt, D3DRTYPE_INDEXBUFFER, O->usage, O->pool, O->w};
    if (!ARG(0)) return D3DERR_INVALIDCALL;
    for (int i = 0; i < 5; i++) sg_st32(MEM, ARG(0) + 4u * i, v[i]);
    return D3D_OK;
}

/* ---- queries ------------------------------------------------------------------------------------------ */
static uint32_t query_size(uint32_t t) {
    switch (t) {
    case D3DQUERYTYPE_EVENT: case D3DQUERYTYPE_OCCLUSION: case D3DQUERYTYPE_TIMESTAMPDISJOINT: return 4;
    case D3DQUERYTYPE_TIMESTAMP: case D3DQUERYTYPE_TIMESTAMPFREQ: return 8;
    default: return 0;
    }
}
extern "C" uint32_t mt_dv_create_query(sg_com_call *c) { /* (type, IDirect3DQuery9**); NULL out = support probe */
    mt_dev *d = (mt_dev *)c->obj;
    if (!query_size(ARG(0))) { /* VCACHE etc.: not available, as on the null device */
        if (ARG(1)) sg_st32(MEM, ARG(1), 0);
        return D3DERR_NOTAVAILABLE;
    }
    if (!ARG(1)) return D3D_OK;
    mt_obj *q = mt_new(MT_K_QUERY, d, NULL);
    if (q) q->qtype = ARG(0);
    return mt_put_new(c, ARG(1), q, SG_E_OUTOFMEMORY);
}
static uint32_t occ_slot(mt_dev *d) {
    mt_pass_visibility(d);
    uint32_t s = d->vis_next++ % d->vis_slots;
    ((uint64_t *)d->vis.contents)[s] = 0;
    return s * 8;
}
/* A render pass began while an occlusion query is active: count into a fresh slot of that query. */
extern "C" void mt_occlusion_pass_begun(mt_dev *d) {
    mt_obj *q = d->occ_active;
    if (!q || !d->enc) return;
    if (q->q_nslots < 16) q->q_slots[q->q_nslots++] = occ_slot(d);
    [d->enc setVisibilityResultMode:MTLVisibilityResultModeCounting offset:q->q_slots[q->q_nslots - 1]];
}
static uint32_t q_type(sg_com_call *c) { return O->qtype; }
static uint32_t q_size(sg_com_call *c) { return query_size(O->qtype); }
static uint32_t q_issue(sg_com_call *c) { /* (flags) */
    mt_obj *q = O;
    mt_dev *d = q->dev;
    if (!d || !d->alive) return D3DERR_INVALIDCALL;
    if (q->qtype == D3DQUERYTYPE_OCCLUSION) {
        if (ARG(0) & D3DISSUE_BEGIN) {
            if (d->occ_active && d->occ_active != q) d->occ_active->q_state = 0;
            q->q_nslots = 0;
            q->q_state = 1;
            d->occ_active = q;
            /* the pass must have the visibility buffer: begin a new one if the open pass was made without it */
            if (d->enc && !d->vis) {
                mt_pass_visibility(d);
                mt_pass_end(d);
            }
            mt_pass_visibility(d);
            if (d->enc) mt_occlusion_pass_begun(d);
        }
        if (ARG(0) & D3DISSUE_END) {
            if (d->occ_active == q) {
                if (d->enc) [d->enc setVisibilityResultMode:MTLVisibilityResultModeDisabled offset:0];
                d->occ_active = NULL;
            }
            q->q_state = 2;
            q->q_serial = d->cmd ? d->serial : d->serial - 1;
        }
        return D3D_OK;
    }
    if (ARG(0) & D3DISSUE_END) {
        q->q_state = 2;
        q->q_serial = d->cmd ? d->serial : d->serial - 1; /* nothing recorded since the last commit: done when that is */
    }
    return D3D_OK;
}
static uint32_t q_get_data(sg_com_call *c) { /* (data, size, flags) */
    mt_obj *q = O;
    mt_dev *d = q->dev;
    uint32_t p = ARG(0), n = ARG(1), need = query_size(q->qtype);
    uint64_t v = 0;
    if (p && n && n < need) return D3DERR_INVALIDCALL;
    if (!d || !d->alive) return D3DERR_DEVICELOST;
    if (q->qtype == D3DQUERYTYPE_EVENT || q->qtype == D3DQUERYTYPE_OCCLUSION) {
        if (q->q_state == 1) return SG_S_FALSE; /* begun, not ended */
        if (q->q_state == 2 && q->q_serial > completed_of(d)) {
            if ((ARG(2) & D3DGETDATA_FLUSH) && d->cmd && q->q_serial >= d->serial) mt_flush(d, 0);
            /* The game's frame limiter spins on this (0x11f8190); a short sleep per S_FALSE. */
            struct timespec ts = {0, 20000};
            g_mst.query_waits++;
            nanosleep(&ts, NULL);
            if (q->q_serial > completed_of(d)) return SG_S_FALSE;
        }
        if (q->qtype == D3DQUERYTYPE_EVENT) v = 1;
        else
            for (unsigned i = 0; i < q->q_nslots; i++) v += ((uint64_t *)d->vis.contents)[q->q_slots[i] / 8];
    } else if (q->qtype == D3DQUERYTYPE_TIMESTAMPFREQ) {
        v = 1000000000ull;
    } else if (q->qtype == D3DQUERYTYPE_TIMESTAMP) {
        v = mt_now_ns();
    }
    if (p && n) memcpy(MEM + p, &v, need);
    return D3D_OK;
}

const sg_com_method mt_texture_methods[] = {BASETEX_METHODS(Texture), M(Texture, GetLevelDesc, tex_level_desc),
                                            M(Texture, GetSurfaceLevel, tex_surface_level), M(Texture, LockRect, tex_lock),
                                            M(Texture, UnlockRect, tex_unlock), M(Texture, AddDirtyRect, tex_dirty)};
const sg_com_method mt_cube_methods[] = {BASETEX_METHODS(CubeTexture), M(CubeTexture, GetLevelDesc, tex_level_desc),
                                         M(CubeTexture, GetCubeMapSurface, cube_surface), M(CubeTexture, LockRect, cube_lock),
                                         M(CubeTexture, UnlockRect, cube_unlock), M(CubeTexture, AddDirtyRect, cube_dirty)};
const sg_com_method mt_volumetex_methods[] = {BASETEX_METHODS(VolumeTexture), M(VolumeTexture, GetLevelDesc, vt_level_desc),
                                              M(VolumeTexture, GetVolumeLevel, vt_volume_level), M(VolumeTexture, LockBox, vt_lock),
                                              M(VolumeTexture, UnlockBox, vt_unlock), M(VolumeTexture, AddDirtyBox, vt_dirty)};
const sg_com_method mt_surface_methods[] = {RESOURCE_METHODS(Surface), M(Surface, GetContainer, sub_container),
                                            M(Surface, GetDesc, surf_desc), M(Surface, LockRect, surf_lock),
                                            M(Surface, UnlockRect, surf_unlock), M(Surface, GetDC, surf_get_dc),
                                            M(Surface, ReleaseDC, surf_get_dc)};
const sg_com_method mt_volume_methods[] = {SG_COM_IUNKNOWN, M(Volume, GetDevice, mt_r_get_device), M(Volume, SetPrivateData, r_set_private),
                                           M(Volume, GetPrivateData, r_get_private), M(Volume, FreePrivateData, r_free_private),
                                           M(Volume, GetContainer, sub_container), M(Volume, GetDesc, vol_desc),
                                           M(Volume, LockBox, vol_lock), M(Volume, UnlockBox, vol_unlock)};
const sg_com_method mt_vb_methods[] = {RESOURCE_METHODS(VertexBuffer), M(VertexBuffer, Lock, buf_lock),
                                       M(VertexBuffer, Unlock, buf_unlock), M(VertexBuffer, GetDesc, vb_desc)};
const sg_com_method mt_ib_methods[] = {RESOURCE_METHODS(IndexBuffer), M(IndexBuffer, Lock, buf_lock),
                                       M(IndexBuffer, Unlock, buf_unlock), M(IndexBuffer, GetDesc, ib_desc)};
const sg_com_method mt_query_methods[] = {SG_COM_IUNKNOWN, M(Query, GetDevice, mt_r_get_device), M(Query, GetType, q_type),
                                          M(Query, GetDataSize, q_size), M(Query, Issue, q_issue), M(Query, GetData, q_get_data)};
const unsigned mt_n_texture_methods = SG_COM_N(mt_texture_methods), mt_n_cube_methods = SG_COM_N(mt_cube_methods),
               mt_n_volumetex_methods = SG_COM_N(mt_volumetex_methods), mt_n_surface_methods = SG_COM_N(mt_surface_methods),
               mt_n_volume_methods = SG_COM_N(mt_volume_methods), mt_n_vb_methods = SG_COM_N(mt_vb_methods),
               mt_n_ib_methods = SG_COM_N(mt_ib_methods), mt_n_query_methods = SG_COM_N(mt_query_methods);

/* ---- device-level copies ------------------------------------------------------------------------------ */
static mt_obj *surf_arg(sg_com_call *c, uint32_t g) {
    mt_obj *o = mt_from_guest(MEM, g, -1);
    return o && (o->kind == MT_K_SURF) ? o : NULL;
}
static void rect_or_full(sg_com_call *c, uint32_t prect, mt_obj *s, uint32_t r[4]) {
    if (prect) {
        r[0] = sg_ld32(MEM, prect), r[1] = sg_ld32(MEM, prect + 4), r[2] = sg_ld32(MEM, prect + 8), r[3] = sg_ld32(MEM, prect + 12);
    } else {
        r[0] = r[1] = 0, r[2] = s->w, r[3] = s->h;
    }
}
static uint8_t *level_ptr(mt_obj *t, uint32_t face, uint32_t level) {
    uint32_t base = mt_shadow_of(t);
    return base ? sg_com_mem + base + face * t->face_bytes + t->lvl_off[level] : NULL;
}
/* UpdateSurface(src SYSTEMMEM, srect, dst DEFAULT, dpoint) */
extern "C" uint32_t mt_dv_update_surface(sg_com_call *c) {
    mt_dev *d = (mt_dev *)c->obj;
    mt_obj *s = surf_arg(c, ARG(0)), *t = surf_arg(c, ARG(2));
    uint32_t sf, sl, df, dl, r[4], dx = 0, dy = 0;
    if (!s || !t) return D3DERR_INVALIDCALL;
    mt_obj *st = mt_top(s, &sf, &sl), *dt = mt_top(t, &df, &dl);
    if (s->pool != D3DPOOL_SYSTEMMEM || t->pool != D3DPOOL_DEFAULT || s->fmt != t->fmt) return D3DERR_INVALIDCALL;
    rect_or_full(c, ARG(1), s, r);
    if (ARG(3)) dx = sg_ld32(MEM, ARG(3)), dy = sg_ld32(MEM, ARG(3) + 4);
    if (r[2] > s->w || r[3] > s->h || r[0] >= r[2] || r[1] >= r[3] || dx + (r[2] - r[0]) > t->w || dy + (r[3] - r[1]) > t->h)
        return D3DERR_INVALIDCALL;
    uint8_t *src = level_ptr(st, sf, sl);
    if (!src || !dt->tex) return D3D_OK;
    unsigned block = st->mf ? st->mf->block : 0, bpp = st->mf ? st->mf->guest_bpp : 4;
    src += block ? (r[1] / 4) * st->lvl_pitch[sl] + (r[0] / 4) * block : r[1] * st->lvl_pitch[sl] + r[0] * bpp;
    upload(d, dt, df, dl, dx, dy, 0, r[2] - r[0], r[3] - r[1], 1, src, st->lvl_pitch[sl], st->lvl_slice[sl]);
    return D3D_OK;
}
/* UpdateTexture(src SYSTEMMEM texture, dst DEFAULT texture): every level of dst from the matching src level */
extern "C" uint32_t mt_dv_update_texture(sg_com_call *c) {
    mt_dev *d = (mt_dev *)c->obj;
    mt_obj *s = mt_from_guest(MEM, ARG(0), -1), *t = mt_from_guest(MEM, ARG(1), -1);
    if (!s || !t || s->kind != t->kind || (s->kind != MT_K_TEX && s->kind != MT_K_CUBE && s->kind != MT_K_VOLTEX)) return D3DERR_INVALIDCALL;
    if (s->pool != D3DPOOL_SYSTEMMEM || t->pool != D3DPOOL_DEFAULT || s->fmt != t->fmt) return D3DERR_INVALIDCALL;
    uint32_t skip = 0;
    while (skip < s->levels && lvl_dim(s->w, skip) > t->w) skip++;
    if (skip >= s->levels || lvl_dim(s->w, skip) != t->w || lvl_dim(s->h, skip) != t->h) return D3DERR_INVALIDCALL;
    uint32_t n = (t->usage & D3DUSAGE_AUTOGENMIPMAP) ? 1 : t->levels;
    if (s->levels - skip < n) return D3DERR_INVALIDCALL;
    if (!t->tex || !mt_shadow_of(s)) return D3D_OK;
    for (uint32_t f = 0; f < t->faces; f++)
        for (uint32_t l = 0; l < n; l++) {
            uint32_t sl = l + skip;
            upload(d, t, f, l, 0, 0, 0, lvl_dim(t->w, l), lvl_dim(t->h, l), t->kind == MT_K_VOLTEX ? lvl_dim(t->d, l) : 1,
                   level_ptr(s, f, sl), s->lvl_pitch[sl], s->lvl_slice[sl]);
        }
    s->upload_pending = 0;
    t->gpu_dirty = 1; /* the shadow of t (if any) no longer matches */
    if (t->usage & D3DUSAGE_AUTOGENMIPMAP) t->mips_dirty = 1;
    return D3D_OK;
}
/* GetRenderTargetData(src render target, dst SYSTEMMEM surface of the same size and format) */
extern "C" uint32_t mt_dv_rt_data(sg_com_call *c) {
    mt_dev *d = (mt_dev *)c->obj;
    mt_obj *s = surf_arg(c, ARG(0)), *t = surf_arg(c, ARG(1));
    uint32_t sf, sl, df, dl;
    if (!s || !t) return D3DERR_INVALIDCALL;
    if (t->pool != D3DPOOL_SYSTEMMEM || s->w != t->w || s->h != t->h || s->fmt != t->fmt) return D3DERR_INVALIDCALL;
    mt_obj *st = mt_top(s, &sf, &sl), *dt = mt_top(t, &df, &dl);
    if (!st->tex) return D3DERR_INVALIDCALL;
    uint8_t *dst = level_ptr(dt, df, dl);
    if (!dst) return D3DERR_OUTOFVIDEOMEMORY;
    if (st->upload_pending) mt_texture_of(d, st);
    if (readback(d, st, sf, sl, dst, dt->lvl_pitch[dl], dt->lvl_slice[dl])) return D3DERR_INVALIDCALL;
    return D3D_OK;
}
extern "C" uint32_t mt_dv_front_data(sg_com_call *c) { /* (swapchain, dst): the last presented back buffer */
    mt_dev *d = (mt_dev *)c->obj;
    mt_obj *t = surf_arg(c, ARG(1));
    if (!t || ARG(0) != 0 || t->pool != D3DPOOL_SYSTEMMEM || !d->sc || !d->sc->backbuf) return D3DERR_INVALIDCALL;
    mt_obj *bb = d->sc->backbuf;
    uint32_t df, dl;
    mt_obj *dt = mt_top(t, &df, &dl);
    if (t->w != bb->w || t->h != bb->h || !bb->tex) return D3DERR_INVALIDCALL;
    uint8_t *dst = level_ptr(dt, df, dl);
    if (!dst) return D3DERR_OUTOFVIDEOMEMORY;
    readback(d, bb, 0, 0, dst, dt->lvl_pitch[dl], dt->lvl_slice[dl]);
    if (t->fmt == D3DFMT_A8R8G8B8)
        for (uint32_t y = 0; y < t->h; y++)
            for (uint32_t x = 0; x < t->w; x++) dst[y * dt->lvl_pitch[dl] + 4 * x + 3] = 0xff;
    return D3D_OK;
}
/* StretchRect(src, srect, dst, drect, filter) with D3D9's rules (DEVIATIONS 3.10) */
extern "C" uint32_t mt_dv_stretch(sg_com_call *c) {
    mt_dev *d = (mt_dev *)c->obj;
    mt_obj *s = surf_arg(c, ARG(0)), *t = surf_arg(c, ARG(2));
    uint32_t sr[4], dr[4], sf, sl, df, dl;
    if (!s || !t || s == t) return D3DERR_INVALIDCALL;
    if (s->pool != D3DPOOL_DEFAULT || t->pool != D3DPOOL_DEFAULT) return D3DERR_INVALIDCALL;
    mt_obj *st = mt_top(s, &sf, &sl), *dt = mt_top(t, &df, &dl);
    if (!st->tex || !dt->tex) return D3DERR_INVALIDCALL;
    rect_or_full(c, ARG(1), s, sr);
    rect_or_full(c, ARG(3), t, dr);
    if (sr[0] >= sr[2] || sr[1] >= sr[3] || sr[2] > s->w || sr[3] > s->h || dr[0] >= dr[2] || dr[1] >= dr[3] || dr[2] > t->w || dr[3] > t->h)
        return D3DERR_INVALIDCALL;
    int depth = s->mf && (s->mf->flags & MT_FMT_DEPTH);
    if (depth) { /* whole surfaces of the same size and format only, as D3D9 */
        if (ARG(1) || ARG(3) || s->w != t->w || s->h != t->h || s->fmt != t->fmt) return D3DERR_INVALIDCALL;
    }
    if (st->upload_pending) mt_texture_of(d, st);
    int same = sr[2] - sr[0] == dr[2] - dr[0] && sr[3] - sr[1] == dr[3] - dr[1] && s->fmt == t->fmt && st->tex.pixelFormat == dt->tex.pixelFormat;
    if (same || depth) {
        id<MTLBlitCommandEncoder> be = mt_blit(d);
        [be copyFromTexture:st->tex sourceSlice:sf sourceLevel:sl sourceOrigin:MTLOriginMake(sr[0], sr[1], 0)
                 sourceSize:MTLSizeMake(sr[2] - sr[0], sr[3] - sr[1], 1) toTexture:dt->tex destinationSlice:df destinationLevel:dl
          destinationOrigin:MTLOriginMake(dr[0], dr[1], 0)];
    } else {
        if (!dt->mf || !(dt->mf->flags & MT_FMT_RENDERABLE) || !(dt->tex.usage & MTLTextureUsageRenderTarget)) return D3DERR_INVALIDCALL;
        if (mt_util_draw(d, MT_UTIL_COPY, s, sr, dt->tex, df, dl, dt->tex.pixelFormat, dr, lvl_dim(dt->w, dl), lvl_dim(dt->h, dl),
                         ARG(4) == 2 /* D3DTEXF_LINEAR */, NULL))
            return D3DERR_INVALIDCALL;
    }
    mt_mark_used(d, st);
    mt_mark_used(d, dt);
    mt_gpu_wrote(t);
    return D3D_OK;
}
/* ColorFill(surface, rect, D3DCOLOR): DEFAULT-pool surfaces */
extern "C" uint32_t mt_dv_color_fill(sg_com_call *c) {
    mt_dev *d = (mt_dev *)c->obj;
    mt_obj *s = surf_arg(c, ARG(0));
    uint32_t r[4], f, l, col = ARG(2);
    if (!s || s->pool != D3DPOOL_DEFAULT || (s->usage & D3DUSAGE_DEPTHSTENCIL)) return D3DERR_INVALIDCALL;
    mt_obj *t = mt_top(s, &f, &l);
    if (!t->tex) return D3DERR_INVALIDCALL;
    rect_or_full(c, ARG(1), s, r);
    if (r[0] >= r[2] || r[1] >= r[3] || r[2] > s->w || r[3] > s->h) return D3DERR_INVALIDCALL;
    float rgba[4] = {((col >> 16) & 255) / 255.0f, ((col >> 8) & 255) / 255.0f, (col & 255) / 255.0f, (col >> 24) / 255.0f};
    if (t->mf && (t->mf->flags & MT_FMT_RENDERABLE) && (t->tex.usage & MTLTextureUsageRenderTarget)) {
        if (mt_util_draw(d, MT_UTIL_FILL, NULL, NULL, t->tex, f, l, t->tex.pixelFormat, r, lvl_dim(t->w, l), lvl_dim(t->h, l), 0, rgba))
            return D3DERR_INVALIDCALL;
    } else if (t->mf && t->mf->guest_bpp == 4 && !(t->mf->flags & MT_FMT_CONVERT)) { /* a non-renderable 32-bit surface */
        uint32_t w = r[2] - r[0], h = r[3] - r[1];
        uint32_t *px = (uint32_t *)malloc((size_t)w * h * 4);
        for (uint32_t i = 0; i < w * h; i++) px[i] = col;
        upload(d, t, f, l, r[0], r[1], 0, w, h, 1, (const uint8_t *)px, w * 4, w * h * 4);
        free(px);
    } else {
        return D3DERR_INVALIDCALL;
    }
    mt_mark_used(d, t);
    mt_gpu_wrote(s);
    return D3D_OK;
}

/* ---- per-device Metal setup ---------------------------------------------------------------------------- */
extern "C" void mt_device_metal_init(mt_dev *d) {
    d->serial = 1;
    d->completed = 0;
    MTLSamplerDescriptor *sd = [[MTLSamplerDescriptor alloc] init];
    sd.sAddressMode = sd.tAddressMode = MTLSamplerAddressModeClampToEdge;
    sd.minFilter = sd.magFilter = MTLSamplerMinMagFilterNearest;
    d->samp_point = [d->mtl newSamplerStateWithDescriptor:sd];
    sd.minFilter = sd.magFilter = MTLSamplerMinMagFilterLinear;
    d->samp_linear = [d->mtl newSamplerStateWithDescriptor:sd];
    [sd release];
}
extern "C" void mt_device_metal_free(mt_dev *d) {
    mt_pass_end(d);
    mt_flush(d, 1);
    if (d->last_cmd) [d->last_cmd waitUntilCompleted];
    set_completed(d, d->serial);
    mt_reap(d);
    free(d->graveyard);
    d->graveyard = NULL;
    d->ngrave = d->capgrave = 0;
    for (int i = 0; i < 3; i++) {
        [d->scratch[i] release];
        d->scratch[i] = nil;
    }
    for (int i = 0; i < 16; i++)
        if (d->util_pso[i]) {
            [(id)d->util_pso[i] release];
            d->util_pso[i] = NULL;
        }
    [d->vis release], d->vis = nil;
    [d->util_lib release], d->util_lib = nil;
    [d->samp_point release], d->samp_point = nil;
    [d->samp_linear release], d->samp_linear = nil;
    [d->last_cmd release], d->last_cmd = nil;
    [d->layer release], d->layer = nil;
    [d->queue release], d->queue = nil;
    [d->mtl release], d->mtl = nil;
}
