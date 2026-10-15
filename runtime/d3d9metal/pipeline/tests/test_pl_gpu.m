/* test_pl_gpu.m -- the GPU test of the pipeline package against a STUB device (no device-layer code): this file
 * implements the d3d9metal_int.h functions src/ provides (command buffer, pass, scratch, resource accessors) in the
 * simplest way, builds mt_dev/mt_obj by hand, sets D3D9 state the way src/'s setters store it, calls the pipeline's
 * slot handlers (Clear, DrawPrimitive, DrawIndexedPrimitive, the two UP draws) with arguments laid out as on the
 * guest stack, reads the 64x64 target back and checks pixels. Covers: half-pixel edges, triangle fans (plain,
 * indexed, UP), DrawIndexedPrimitiveUP, a 16-bit strip through index 0xFFFF (Metal primitive restart), blending,
 * whole and partial Clear (viewport, rectangles, depth), scissor, alpha test, culling, colour write mask, UBYTE4 and
 * SHORT2 inputs, an unfed vertex input, stencil, instancing, the dummy texture for an unbound sampler, the
 * sampler/PSO caches. Our own shaders (tests/shaders, .asm); no game code or data.
 * Usage: test_pl_gpu [out.ppm]
 *        test_pl_gpu --pairs DIR...   pipeline states only (no draw): for every <n>.pair file (u32 vs length, vs
 *        bytecode, u32 ps length, ps bytecode) translate both shaders, synthesise a declaration for the VS inputs
 *        cycling through every D3DDECLTYPE our DeclTypes caps allow, vary blend/alpha test/targets, and build the
 *        pipeline state. Used on shader pairs extracted from the game (compile only, never drawn). */
#import <Metal/Metal.h>
#include <math.h>
#include <stdarg.h>
#include <stdio.h>
#include <stdlib.h>
#include <string.h>

#include "d3d9metal_int.h"
#include "d3d9_slots.h"
#include "../pl_int.h"
#include "../pl_shader.h"
#include "../../shader/d3d9msl.h"
#include "pl_test_shaders.h"

/* ---- the stub device (what src/ provides, minimally) ------------------------------------------------------- */
int mt_verbose = 0;
static id<MTLBuffer> g_scr;
static uint32_t g_scr_off;
static uint64_t g_passes;

MT_ID(MTLCommandBuffer) mt_cmd(mt_dev *d) {
    if (!d->cmd) d->cmd = [[d->queue commandBuffer] retain];
    return d->cmd;
}
void mt_pass_end(mt_dev *d) {
    if (d->enc) { [d->enc endEncoding]; [d->enc release]; d->enc = nil; }
}
MT_ID(MTLRenderCommandEncoder) mt_pass_encoder(mt_dev *d) {
    int same = d->enc && d->pass_ds == d->st.ds;
    for (int i = 0; i < MT_NRT; i++) if (d->pass_rt[i] != d->st.rt[i]) same = 0;
    if (same) return d->enc;
    mt_pass_end(d);
    MTLRenderPassDescriptor *rp = [MTLRenderPassDescriptor renderPassDescriptor];
    for (int i = 0; i < MT_NRT; i++) {
        d->pass_rt[i] = d->st.rt[i];
        if (!d->st.rt[i]) continue;
        rp.colorAttachments[i].texture = d->st.rt[i]->tex;
        rp.colorAttachments[i].storeAction = MTLStoreActionStore;
        if (d->pending_clear & 1) {
            rp.colorAttachments[i].loadAction = MTLLoadActionClear;
            rp.colorAttachments[i].clearColor = MTLClearColorMake(d->clear_rgba[0], d->clear_rgba[1], d->clear_rgba[2], d->clear_rgba[3]);
        } else rp.colorAttachments[i].loadAction = MTLLoadActionLoad;
    }
    d->pass_ds = d->st.ds;
    if (d->st.ds) {
        rp.depthAttachment.texture = d->st.ds->tex;
        rp.depthAttachment.storeAction = MTLStoreActionStore;
        rp.depthAttachment.loadAction = (d->pending_clear & 2) ? MTLLoadActionClear : MTLLoadActionLoad;
        rp.depthAttachment.clearDepth = d->clear_z;
        rp.stencilAttachment.texture = d->st.ds->tex;
        rp.stencilAttachment.storeAction = MTLStoreActionStore;
        rp.stencilAttachment.loadAction = (d->pending_clear & 4) ? MTLLoadActionClear : MTLLoadActionLoad;
        rp.stencilAttachment.clearStencil = d->clear_stencil;
    }
    d->pending_clear = 0;
    d->pass_w = d->st.rt[0] ? d->st.rt[0]->w : d->st.ds->w;
    d->pass_h = d->st.rt[0] ? d->st.rt[0]->h : d->st.ds->h;
    d->enc = [[mt_cmd(d) renderCommandEncoderWithDescriptor:rp] retain];
    g_passes++;
    return d->enc;
}
int mt_pass_clear(mt_dev *d, uint32_t flags, const float rgba[4], float z, uint32_t stencil) {
    mt_pass_end(d);
    d->pending_clear = flags;
    memcpy(d->clear_rgba, rgba, 16);
    d->clear_z = z;
    d->clear_stencil = stencil;
    return 0;
}
MT_ID(MTLBlitCommandEncoder) mt_blit(mt_dev *d) { mt_pass_end(d); return [mt_cmd(d) blitCommandEncoder]; }
void mt_flush(mt_dev *d, int wait) {
    if (d->pending_clear) mt_pass_encoder(d);
    mt_pass_end(d);
    if (!d->cmd) return;
    [d->cmd commit];
    if (wait) [d->cmd waitUntilCompleted];
    if (d->cmd.error) fprintf(stderr, "command buffer error: %s\n", d->cmd.error.localizedDescription.UTF8String);
    [d->cmd release];
    d->cmd = nil;
    mt_pl_frame_end(d);
    g_scr_off = 0;
}
void mt_wait_serial(mt_dev *d, uint64_t s) { (void)s; mt_flush(d, 1); }
MT_ID(MTLBuffer) mt_scratch(mt_dev *d, uint32_t size, uint32_t align, uint32_t *off, void **host) {
    (void)d;
    g_scr_off = (g_scr_off + align - 1) & ~(align - 1);
    if (g_scr_off + size > g_scr.length) return nil;
    *off = g_scr_off;
    *host = (uint8_t *)g_scr.contents + g_scr_off;
    g_scr_off += size;
    return g_scr;
}
void mt_mark_used(mt_dev *d, mt_obj *r) { (void)d; (void)r; }
MT_ID(MTLBuffer) mt_buffer_of(mt_dev *d, mt_obj *o) { (void)d; return o->buf; }
MT_ID(MTLTexture) mt_texture_of(mt_dev *d, mt_obj *o) { (void)d; return o->view ? o->view : o->tex; }
MT_ID(MTLTexture) mt_surface_target(mt_obj *s, uint32_t *slice, uint32_t *level) { *slice = 0; *level = 0; return s->tex; }
MT_ID(MTLBuffer) mt_pass_visibility(mt_dev *d) { (void)d; return nil; }
static d3d9null_stats g_nstats;
d3d9null_stats *d3d9null_stats_ptr(void) { return &g_nstats; }

/* ---- the test ----------------------------------------------------------------------------------------------- */
static int pass, fail;
#define CHECK(c, ...) do { if (c) pass++; else { fail++; fprintf(stderr, "%s:%d: FAIL: ", __FILE__, __LINE__); fprintf(stderr, __VA_ARGS__); fputc('\n', stderr); } } while (0)

enum { W = 64 };
static mt_dev D;
static uint8_t *MEM;
static sg_ctx CTX;
static uint64_t next_id = 1;
static mt_obj RT, DS;

static mt_obj *new_obj(int kind) { mt_obj *o = calloc(1, sizeof *o); o->kind = kind; o->dev = &D; o->uid = next_id++; return o; }
static mt_obj *shader(int kind, const uint32_t *t, size_t n) {
    mt_obj *o = new_obj(kind);
    o->code = malloc(4 * n); memcpy(o->code, t, 4 * n);
    o->code_len = (uint32_t)(4 * n);
    o->code_hash = d3d9msl_hash(t, n);
    return o;
}
/* D3DVERTEXELEMENT9: stream, offset, type, method, usage, index */
static mt_obj *decl(const uint16_t (*e)[6], int n) {
    mt_obj *o = new_obj(MT_K_VDECL);
    o->decl = calloc(n + 1, 8);
    for (int i = 0; i < n; i++) {
        uint8_t *p = o->decl + 8 * i;
        memcpy(p, &e[i][0], 2); memcpy(p + 2, &e[i][1], 2);
        p[4] = (uint8_t)e[i][2]; p[5] = (uint8_t)e[i][3]; p[6] = (uint8_t)e[i][4]; p[7] = (uint8_t)e[i][5];
    }
    uint8_t *end = o->decl + 8 * n; end[0] = 0xff; end[4] = 17;
    o->decl_count = (uint32_t)n;
    return o;
}
static mt_obj *buffer(int kind, const void *data, uint32_t len) {
    mt_obj *o = new_obj(kind);
    o->buf = [D.mtl newBufferWithBytes:data length:len options:MTLResourceStorageModeShared];
    o->w = len;
    return o;
}
/* calls a pipeline handler with stdcall arguments laid out as on the guest stack */
static uint32_t call(unsigned slot, int n, ...) {
    sg_com_handler *h = mt_pipeline_handler(slot);
    if (!h) { fprintf(stderr, "no handler for slot %u\n", slot); exit(1); }
    CTX.esp = 0x1000;
    va_list ap; va_start(ap, n);
    for (int i = 0; i < n; i++) { uint32_t v = va_arg(ap, uint32_t); memcpy(MEM + 0x1008 + 4 * i, &v, 4); }
    va_end(ap);
    sg_com_call c = {&CTX, MEM, 0, (sg_com_obj *)&D, NULL, NULL};
    return h(&c);
}
static uint32_t f2u(float f) { uint32_t u; memcpy(&u, &f, 4); return u; }
static uint32_t pix[W * W];
static void readback(void) {
    mt_flush(&D, 1);
    [RT.tex getBytes:pix bytesPerRow:4 * W fromRegion:MTLRegionMake2D(0, 0, W, W) mipmapLevel:0];
}
static uint32_t px(int x, int y) { return pix[y * W + x] ; }  /* BGRA8 little-endian = 0xAARRGGBB */
static void clear_all(uint32_t argb) { call(S_Device_Clear, 6, 0u, 0u, 7u, argb, f2u(1.0f), 0u); }
static void defaults(void) {
    mt_state *st = &D.st;
    memset(st->rs, 0, sizeof st->rs);
    st->rs[PL_RS_ZENABLE] = 0; st->rs[PL_RS_ZWRITEENABLE] = 1; st->rs[PL_RS_ZFUNC] = 4; st->rs[PL_RS_CULLMODE] = 1;
    st->rs[PL_RS_FILLMODE] = 3; st->rs[PL_RS_SRCBLEND] = 2; st->rs[PL_RS_DESTBLEND] = 1; st->rs[PL_RS_BLENDOP] = 1;
    st->rs[PL_RS_ALPHAFUNC] = 8; st->rs[PL_RS_STENCILFUNC] = 8; st->rs[PL_RS_STENCILMASK] = 0xffffffff;
    st->rs[PL_RS_STENCILWRITEMASK] = 0xffffffff; st->rs[PL_RS_STENCILFAIL] = st->rs[PL_RS_STENCILZFAIL] = st->rs[PL_RS_STENCILPASS] = 1;
    st->rs[PL_RS_COLORWRITEENABLE] = 0xf; st->rs[PL_RS_COLORWRITEENABLE1] = 0xf;
    st->rs[PL_RS_COLORWRITEENABLE2] = 0xf; st->rs[PL_RS_COLORWRITEENABLE3] = 0xf;
    for (int s = 0; s < MT_NSAMPLERS; s++) {
        st->ss[s][PL_SAMP_ADDRESSU] = st->ss[s][PL_SAMP_ADDRESSV] = st->ss[s][PL_SAMP_ADDRESSW] = 1;
        st->ss[s][PL_SAMP_MAGFILTER] = st->ss[s][PL_SAMP_MINFILTER] = 1; st->ss[s][PL_SAMP_MAXANISOTROPY] = 1;
    }
    st->vp = (mt_viewport){0, 0, W, W, 0, 1};
}

typedef struct { float x, y, z, w, u, v; uint32_t c; } Vtx;
static void quad(Vtx *q, float x0, float y0, float x1, float y1, float z, uint32_t c) { /* strip order */
    Vtx a[4] = {{x0 - .5f, y0 - .5f, z, 1, 0, 0, c}, {x1 - .5f, y0 - .5f, z, 1, 1, 0, c},
                {x0 - .5f, y1 - .5f, z, 1, 0, 1, c}, {x1 - .5f, y1 - .5f, z, 1, 1, 1, c}};
    memcpy(q, a, sizeof a);
}
static int near(uint32_t a, uint32_t b, int tol) {
    for (int k = 0; k < 32; k += 8) { int d = (int)((a >> k) & 255) - (int)((b >> k) & 255); if (d < -tol || d > tol) return 0; }
    return 1;
}

extern int mt_pl_prepare(mt_dev *d);
#include <dirent.h>
/* D3DDECLTYPEs per usage, all within the DeclTypes caps (0x30f) and the base types */
static uint8_t pick_type(unsigned usage, unsigned n) {
    static const uint8_t pos[] = {2, 3, 10, 16, 7}, bw[] = {8, 3, 2, 4, 0, 1}, bi[] = {5, 4}, nrm[] = {2, 8, 10, 4, 16},
                         tc[] = {1, 15, 9, 3, 10, 16, 0, 2, 6, 7}, col[] = {4, 8, 3, 16};
#define P(a) a[n % sizeof a]
    switch (usage) {
    case 0: case 9: return P(pos);
    case 1: return P(bw);
    case 2: return P(bi);
    case 3: case 6: case 7: return P(nrm);
    case 5: return P(tc);
    case 10: return P(col);
    default: return 3;
    }
#undef P
}
static unsigned type_size(uint8_t t) { uint32_t sz; pl_vertex_format(t, &sz); return sz; }

static int run_pairs(int argc, char **argv) {
    unsigned npairs = 0, ok = 0, fail_vs = 0, fail_ps = 0, fail_pso = 0, types_used = 0;
    mt_obj *rt1 = NULL;
    for (int ai = 2; ai < argc; ai++) {
        DIR *dir = opendir(argv[ai]);
        if (!dir) { perror(argv[ai]); continue; }
        struct dirent *de;
        while ((de = readdir(dir))) {
            if (!strstr(de->d_name, ".pair")) continue;
            char path[1024];
            snprintf(path, sizeof path, "%s/%s", argv[ai], de->d_name);
            FILE *f = fopen(path, "rb");
            if (!f) continue;
            static uint8_t buf[1 << 20];
            size_t len = fread(buf, 1, sizeof buf, f);
            fclose(f);
            uint32_t vl, pl;
            memcpy(&vl, buf, 4); memcpy(&pl, buf + 4 + vl, 4);
            if (8 + (size_t)vl + pl > len) continue;
            @autoreleasepool {
            mt_obj *vs = shader(MT_K_VS, (const uint32_t *)(buf + 4), vl / 4);
            mt_obj *ps = shader(MT_K_PS, (const uint32_t *)(buf + 8 + vl), pl / 4);
            pl_sh *vsh = pl_sh_get(&D, vs);
            unsigned n = npairs++;
            if (!vsh) { fail_vs++; continue; }
            /* declaration: every VS input, the type cycling with the pair number; texcoords in stream 1 on odd pairs */
            uint16_t el[16][6];
            unsigned ne = 0, off[2] = {0, 0};
            for (unsigned i = 0; i < vsh->nin && ne < 16; i++) {
                uint8_t t = pick_type(vsh->in[i].usage, n + i);
                types_used |= 1u << t;
                unsigned st = (vsh->in[i].usage == 5 && (n & 1)) ? 1 : 0;
                el[ne][0] = (uint16_t)st; el[ne][1] = (uint16_t)off[st]; el[ne][2] = t; el[ne][3] = 0;
                el[ne][4] = vsh->in[i].usage; el[ne][5] = vsh->in[i].index;
                off[st] += type_size(t);
                ne++;
            }
            mt_obj *dc = decl((const uint16_t (*)[6])el, (int)ne);
            mt_obj *vb0 = new_obj(MT_K_VB), *vb1 = new_obj(MT_K_VB);
            vb0->buf = g_scr; vb1->buf = g_scr;
            D.st.streams[0] = (mt_stream){vb0, 0, off[0] ? off[0] : 4, 1};
            D.st.streams[1] = (mt_stream){off[1] ? vb1 : NULL, 0, off[1], 1};
            D.st.vs = vs; D.st.ps = ps; D.st.vdecl = dc;
            D.st.rs[PL_RS_ALPHABLENDENABLE] = n % 2;
            D.st.rs[PL_RS_SRCBLEND] = 5; D.st.rs[PL_RS_DESTBLEND] = 6;
            D.st.rs[PL_RS_ALPHATESTENABLE] = n % 3 == 0; D.st.rs[PL_RS_ALPHAFUNC] = 7;
            /* every fourth pair renders to two targets */
            if (!rt1) { rt1 = new_obj(MT_K_SURF); rt1->tex = RT.tex; rt1->w = rt1->h = W; }
            D.st.rt[1] = n % 4 == 3 ? rt1 : NULL;
            int r = mt_pl_prepare(&D);
            if (r == 0) ok++;
            else if (r == 2) fail_vs++;
            else if (r == 3) fail_ps++;
            else { fail_pso++; if (fail_pso <= 10) fprintf(stderr, "pipeline failed: %s\n", path); }
            D.st.rt[1] = NULL;
            mt_flush(&D, 1);
            }
        }
        closedir(dir);
    }
    uint64_t s[14], cc, cf, cns;
    extern void d3d9metal_pl_stats(uint64_t out[14]);
    extern void d3d9metal_pl_shader_stats(uint64_t *, uint64_t *, uint64_t *);
    d3d9metal_pl_stats(s);
    d3d9metal_pl_shader_stats(&cc, &cf, &cns);
    printf("pairs: %u, pipeline states built %u, vs failed %u, ps failed %u, pso failed %u; declaration types used 0x%05x; "
           "shaders compiled %llu (failed %llu) in %.1f ms\n", npairs, ok, fail_vs, fail_ps, fail_pso, types_used,
           (unsigned long long)cc, (unsigned long long)cf, cns / 1e6);
    return ok == npairs ? 0 : 1;
}

int main(int argc, char **argv) {
    @autoreleasepool {
    setenv("SPORE_D3D9MSL_CACHE", "off", 0);
    D.o.kind = MT_K_DEV; D.o.dev = &D;
    D.mtl = MTLCreateSystemDefaultDevice();
    if (!D.mtl) { printf("no Metal device\n"); return 2; }
    D.queue = [D.mtl newCommandQueue];
    MEM = calloc(1, 4 << 20);
    CTX.mem = MEM;
    g_scr = [D.mtl newBufferWithLength:16 << 20 options:MTLResourceStorageModeShared];
    MTLTextureDescriptor *td = [MTLTextureDescriptor texture2DDescriptorWithPixelFormat:MTLPixelFormatBGRA8Unorm width:W height:W mipmapped:NO];
    td.usage = MTLTextureUsageRenderTarget | MTLTextureUsageShaderRead; td.storageMode = MTLStorageModeShared;
    RT.kind = MT_K_SURF; RT.tex = [D.mtl newTextureWithDescriptor:td]; RT.w = RT.h = W; RT.uid = next_id++;
    MTLTextureDescriptor *dd = [MTLTextureDescriptor texture2DDescriptorWithPixelFormat:MTLPixelFormatDepth32Float_Stencil8 width:W height:W mipmapped:NO];
    dd.usage = MTLTextureUsageRenderTarget; dd.storageMode = MTLStorageModePrivate;
    DS.kind = MT_K_SURF; DS.tex = [D.mtl newTextureWithDescriptor:dd]; DS.w = DS.h = W; DS.uid = next_id++;
    D.st.rt[0] = &RT; D.st.ds = &DS;
    mt_pl_device_created(&D);
    defaults();
    if (argc > 1 && !strcmp(argv[1], "--bench")) {
        /* CPU cost per draw: 20,000 small draws, alternating blend (two pipeline states) and a constant
         * update every draw, as a game frame does; command buffers of 2,000 draws */
        mt_obj *bvs = shader(MT_K_VS, k_vs_pos, sizeof k_vs_pos / 4), *bps = shader(MT_K_PS, k_ps_col, sizeof k_ps_col / 4);
        static const uint16_t be[3][6] = {{0, 0, 3, 0, 0, 0}, {0, 16, 1, 0, 5, 0}, {0, 24, 4, 0, 10, 0}};
        Vtx bq[4];
        quad(bq, 0, 0, 4, 4, 0.5f, 0xffffffff);
        D.st.vs = bvs; D.st.ps = bps; D.st.vdecl = decl(be, 3);
        D.st.streams[0] = (mt_stream){buffer(MT_K_VB, bq, sizeof bq), 0, sizeof(Vtx), 1};
        float c0b[4] = {2.0f / W, -2.0f / W, -1, 1};
        memcpy(D.st.vsf, c0b, 16);
        call(S_Device_DrawPrimitive, 3, 5u, 0u, 2u);
        mt_flush(&D, 1);
        uint64_t t0 = clock_gettime_nsec_np(CLOCK_UPTIME_RAW), enc = 0;
        for (int i = 0; i < 20000; i++) {
            D.st.rs[PL_RS_ALPHABLENDENABLE] = i & 1; D.st.rs[PL_RS_SRCBLEND] = 5; D.st.rs[PL_RS_DESTBLEND] = 6;
            D.st.vsf[4] = (float)i; D.st.vsf_hi = 2; D.st.dirty |= MT_DIRTY_VS_CONST;
            call(S_Device_DrawPrimitive, 3, 5u, 0u, 2u);
            if (i % 2000 == 1999) { uint64_t a = clock_gettime_nsec_np(CLOCK_UPTIME_RAW); mt_flush(&D, 1); t0 += clock_gettime_nsec_np(CLOCK_UPTIME_RAW) - a; }
        }
        enc = clock_gettime_nsec_np(CLOCK_UPTIME_RAW) - t0;
        printf("bench: 20000 draws, %.2f us of CPU per draw (encoding, excluding GPU waits)\n", enc / 20000.0 / 1000.0);
        mt_pl_device_destroyed(&D);
        return 0;
    }
    if (argc > 1 && !strcmp(argv[1], "--pairs")) { int r = run_pairs(argc, argv); mt_pl_device_destroyed(&D); return r; }

    mt_obj *vs = shader(MT_K_VS, k_vs_pos, sizeof k_vs_pos / 4), *ps = shader(MT_K_PS, k_ps_col, sizeof k_ps_col / 4);
    mt_obj *pst = shader(MT_K_PS, k_ps_tex, sizeof k_ps_tex / 4), *vsu = shader(MT_K_VS, k_vs_ubyte, sizeof k_vs_ubyte / 4);
    static const uint16_t eb[3][6] = {{0, 0, 3, 0, 0, 0}, {0, 16, 1, 0, 5, 0}, {0, 24, 4, 0, 10, 0}};
    mt_obj *dbasic = decl(eb, 3);
    static const uint16_t enocol[2][6] = {{0, 0, 3, 0, 0, 0}, {0, 16, 1, 0, 5, 0}};
    mt_obj *dnocol = decl(enocol, 2);
    float c0[4] = {2.0f / W, -2.0f / W, -1, 1};
    memcpy(D.st.vsf, c0, 16); D.st.vsf_hi = 1;
    D.st.vs = vs; D.st.ps = ps; D.st.vdecl = dbasic;
    D.st.dirty = MT_DIRTY_ALL;

    /* 1. whole clear (load action) + a quad with exact half-pixel edges */
    clear_all(0xffff0000);
    Vtx q[4];
    quad(q, 0, 0, 32, 32, 0.5f, 0xff00ff00);
    mt_obj *vb = buffer(MT_K_VB, q, sizeof q);
    D.st.streams[0] = (mt_stream){vb, 0, sizeof(Vtx), 1};
    CHECK(call(S_Device_DrawPrimitive, 3, 5u, 0u, 2u) == 0, "DrawPrimitive");
    readback();
    CHECK(px(5, 5) == 0xff00ff00, "quad inside %08x", px(5, 5));
    CHECK(px(31, 31) == 0xff00ff00 && px(32, 5) == 0xffff0000 && px(5, 32) == 0xffff0000,
          "half-pixel edges: (31,31) %08x (32,5) %08x (5,32) %08x", px(31, 31), px(32, 5), px(5, 32));
    CHECK(px(60, 60) == 0xffff0000, "clear colour %08x", px(60, 60));

    /* 2. triangle fans: plain (vertices 0..3 as a fan: TL TR BR BL) */
    Vtx fan[4];
    quad(q, 32, 0, 64, 32, 0.5f, 0xff0000ff);
    fan[0] = q[0]; fan[1] = q[1]; fan[2] = q[3]; fan[3] = q[2];
    mt_obj *vbf = buffer(MT_K_VB, fan, sizeof fan);
    D.st.streams[0].vb = vbf;
    CHECK(call(S_Device_DrawPrimitive, 3, 6u, 0u, 2u) == 0, "fan");
    /* indexed fan */
    quad(q, 0, 32, 32, 64, 0.5f, 0xffffff00);
    Vtx fan2[4] = {q[0], q[1], q[3], q[2]};
    mt_obj *vbf2 = buffer(MT_K_VB, fan2, sizeof fan2);
    uint16_t fidx[4] = {0, 1, 2, 3};
    mt_obj *ibf = buffer(MT_K_IB, fidx, sizeof fidx); ibf->fmt = PL_D3DFMT_INDEX16;
    D.st.streams[0].vb = vbf2; D.st.ib = ibf;
    CHECK(call(S_Device_DrawIndexedPrimitive, 6, 6u, 0u, 0u, 4u, 0u, 2u) == 0, "indexed fan");
    /* UP fan */
    quad(q, 32, 32, 64, 64, 0.5f, 0xff00ffff);
    Vtx fan3[4] = {q[0], q[1], q[3], q[2]};
    memcpy(MEM + 0x10000, fan3, sizeof fan3);
    CHECK(call(S_Device_DrawPrimitiveUP, 4, 6u, 2u, 0x10000u, (uint32_t)sizeof(Vtx)) == 0, "UP fan");
    CHECK(D.st.streams[0].vb == NULL && D.st.streams[0].stride == 0, "stream 0 unset after a UP draw");
    readback();
    CHECK(px(48, 16) == 0xff0000ff, "fan %08x", px(48, 16));
    CHECK(px(16, 48) == 0xffffff00, "indexed fan %08x", px(16, 48));
    CHECK(px(48, 48) == 0xff00ffff, "UP fan %08x", px(48, 48));
    CHECK(px(33, 1) == 0xff0000ff && px(62, 30) == 0xff0000ff, "both fan triangles %08x %08x", px(33, 1), px(62, 30));

    /* 3. DrawIndexedPrimitiveUP list + 16-bit strip through index 0xFFFF */
    clear_all(0xff000000);
    quad(q, 0, 0, 16, 16, 0.5f, 0xffff00ff);
    memcpy(MEM + 0x10000, q, sizeof q);
    uint16_t li[6] = {0, 1, 2, 2, 1, 3};
    memcpy(MEM + 0x11000, li, sizeof li);
    CHECK(call(S_Device_DrawIndexedPrimitiveUP, 8, 4u, 0u, 4u, 2u, 0x11000u, 101u, 0x10000u, (uint32_t)sizeof(Vtx)) == 0, "DIPUP");
    CHECK(D.st.ib == NULL, "indices unset after DIPUP");
    Vtx *big = calloc(65536, sizeof(Vtx));
    quad(q, 16, 16, 32, 32, 0.5f, 0xff808080);
    big[0] = q[0]; big[1] = q[1]; big[65535] = q[2]; big[2] = q[3];
    mt_obj *vbig = buffer(MT_K_VB, big, 65536 * sizeof(Vtx));
    free(big);
    uint16_t si[4] = {0, 1, 0xffff, 2};
    mt_obj *ibs = buffer(MT_K_IB, si, sizeof si); ibs->fmt = PL_D3DFMT_INDEX16;
    D.st.streams[0] = (mt_stream){vbig, 0, sizeof(Vtx), 1}; D.st.ib = ibs;
    CHECK(call(S_Device_DrawIndexedPrimitive, 6, 5u, 0u, 0u, 65536u, 0u, 2u) == 0, "strip with 0xFFFF");
    readback();
    CHECK(px(8, 8) == 0xffff00ff, "DIPUP %08x", px(8, 8));
    CHECK(px(17, 30) == 0xff808080 && px(30, 17) == 0xff808080, "strip through vertex 65535: %08x %08x", px(17, 30), px(30, 17));

    /* 4. blending SRCALPHA/INVSRCALPHA, alpha 0x80 green over red */
    clear_all(0xffff0000);
    D.st.streams[0] = (mt_stream){vb, 0, sizeof(Vtx), 1};
    quad(q, 0, 0, 64, 64, 0.5f, 0x8000ff00);
    memcpy(vb->buf.contents, q, sizeof q);
    D.st.rs[PL_RS_ALPHABLENDENABLE] = 1; D.st.rs[PL_RS_SRCBLEND] = 5; D.st.rs[PL_RS_DESTBLEND] = 6;
    call(S_Device_DrawPrimitive, 3, 5u, 0u, 2u);
    readback();
    CHECK(near(px(10, 10) & 0xffffff, 0x7f8000, 2) && near(px(10, 10) >> 24, 0xbf, 2), "blend %08x (want ~bf7f8000)", px(10, 10));
    D.st.rs[PL_RS_ALPHABLENDENABLE] = 0;

    /* 5. partial clears: viewport-limited and rectangles; depth clear through the quad path */
    clear_all(0xff000000);
    D.st.vp = (mt_viewport){8, 8, 16, 16, 0, 1};
    call(S_Device_Clear, 6, 0u, 0u, 1u, 0xff0000ffu, f2u(1.0f), 0u);
    D.st.vp = (mt_viewport){0, 0, W, W, 0, 1};
    int32_t rects[8] = {40, 40, 48, 48, 50, 0, 64, 4};
    memcpy(MEM + 0x12000, rects, sizeof rects);
    call(S_Device_Clear, 6, 2u, 0x12000u, 1u, 0xff00ff00u, f2u(1.0f), 0u);
    readback();
    CHECK(px(8, 8) == 0xff0000ff && px(23, 23) == 0xff0000ff && px(7, 8) == 0xff000000 && px(24, 23) == 0xff000000,
          "viewport clear %08x %08x %08x %08x", px(8, 8), px(23, 23), px(7, 8), px(24, 23));
    CHECK(px(40, 40) == 0xff00ff00 && px(47, 47) == 0xff00ff00 && px(48, 47) == 0xff000000 && px(55, 2) == 0xff00ff00 && px(55, 5) == 0xff000000,
          "rect clear %08x %08x %08x %08x %08x", px(40, 40), px(47, 47), px(48, 47), px(55, 2), px(55, 5));

    /* 6. depth: whole clear to 1, quad-path clear of the left half to 0.25, draw z=0.5 LESS -> only right half */
    clear_all(0xff000000);
    D.st.vp = (mt_viewport){0, 0, 32, 64, 0, 1};
    call(S_Device_Clear, 6, 0u, 0u, 2u, 0u, f2u(0.25f), 0u);
    D.st.vp = (mt_viewport){0, 0, W, W, 0, 1};
    D.st.rs[PL_RS_ZENABLE] = 1; D.st.rs[PL_RS_ZFUNC] = 2;
    quad(q, 0, 0, 64, 64, 0.5f, 0xffffffff);
    memcpy(vb->buf.contents, q, sizeof q);
    call(S_Device_DrawPrimitive, 3, 5u, 0u, 2u);
    D.st.rs[PL_RS_ZENABLE] = 0;
    readback();
    CHECK(px(10, 10) == 0xff000000 && px(50, 10) == 0xffffffff, "depth clear+test %08x %08x", px(10, 10), px(50, 10));

    /* 7. scissor and colour write mask */
    clear_all(0xff000000);
    D.st.rs[PL_RS_SCISSORTESTENABLE] = 1;
    D.st.scissor[0] = 10; D.st.scissor[1] = 10; D.st.scissor[2] = 20; D.st.scissor[3] = 20;
    D.st.rs[PL_RS_COLORWRITEENABLE] = 2 | 8;   /* green + alpha */
    call(S_Device_DrawPrimitive, 3, 5u, 0u, 2u);
    D.st.rs[PL_RS_SCISSORTESTENABLE] = 0; D.st.rs[PL_RS_COLORWRITEENABLE] = 0xf;
    readback();
    CHECK(px(10, 10) == 0xff00ff00 && px(19, 19) == 0xff00ff00 && px(20, 19) == 0xff000000 && px(9, 10) == 0xff000000,
          "scissor + write mask %08x %08x %08x %08x", px(10, 10), px(19, 19), px(20, 19), px(9, 10));

    /* 8. alpha test: alpha 0x40, ref 0x80: GREATER draws nothing, LESS draws */
    clear_all(0xff000000);
    quad(q, 0, 0, 32, 64, 0.5f, 0x40ffffff);
    memcpy(vb->buf.contents, q, sizeof q);
    D.st.rs[PL_RS_ALPHATESTENABLE] = 1; D.st.rs[PL_RS_ALPHAREF] = 0x80; D.st.rs[PL_RS_ALPHAFUNC] = 5;
    call(S_Device_DrawPrimitive, 3, 5u, 0u, 2u);
    quad(q, 32, 0, 64, 64, 0.5f, 0x40ffffff);
    mt_obj *vb2 = buffer(MT_K_VB, q, sizeof q);
    D.st.streams[0].vb = vb2;
    D.st.rs[PL_RS_ALPHAFUNC] = 2;
    call(S_Device_DrawPrimitive, 3, 5u, 0u, 2u);
    D.st.rs[PL_RS_ALPHATESTENABLE] = 0;
    D.st.streams[0].vb = vb;
    readback();
    CHECK(px(10, 10) == 0xff000000 && (px(50, 10) & 0xffffff) == 0xffffff, "alpha test %08x %08x", px(10, 10), px(50, 10));

    /* 9. culling: default CCW culls counter-clockwise; a strip quad's first triangle (TL TR BL) is clockwise */
    clear_all(0xff000000);
    D.st.rs[PL_RS_CULLMODE] = 3;
    quad(q, 0, 0, 32, 32, 0.5f, 0xffffffff);
    memcpy(vb->buf.contents, q, sizeof q);
    call(S_Device_DrawPrimitive, 3, 4u, 0u, 1u);                /* TRIANGLELIST of TL TR BL: clockwise on screen */
    Vtx ccw[3] = {q[0], q[2], q[1]};
    for (int i = 0; i < 3; i++) { ccw[i].x += 32; }
    mt_obj *vb3 = buffer(MT_K_VB, ccw, sizeof ccw);
    D.st.streams[0].vb = vb3;
    call(S_Device_DrawPrimitive, 3, 4u, 0u, 1u);
    D.st.rs[PL_RS_CULLMODE] = 1;
    D.st.streams[0].vb = vb;
    readback();
    CHECK(px(4, 4) == 0xffffffff && px(36, 4) == 0xff000000, "cull CCW: cw drawn %08x, ccw culled %08x", px(4, 4), px(36, 4));

    /* 10. UBYTE4 + SHORT2 inputs; an unfed colour input (zeros) */
    clear_all(0xff000000);
    struct { int16_t x, y; uint8_t b[4]; } uv[4] = {{0, 0, {255, 128, 0, 255}}, {16, 0, {255, 128, 0, 255}},
                                                     {0, 16, {255, 128, 0, 255}}, {16, 16, {255, 128, 0, 255}}};
    mt_obj *vbu = buffer(MT_K_VB, uv, sizeof uv);
    static const uint16_t eu[2][6] = {{0, 0, 6, 0, 0, 0}, {0, 4, 5, 0, 2, 0}};
    mt_obj *du = decl(eu, 2);
    float c1[4] = {1.0f / 255, 0, 0, 0};
    memcpy(D.st.vsf + 4, c1, 16); D.st.dirty |= MT_DIRTY_VS_CONST;
    D.st.vs = vsu; D.st.vdecl = du; D.st.streams[0] = (mt_stream){vbu, 0, 8, 1};
    call(S_Device_DrawPrimitive, 3, 5u, 0u, 2u);
    D.st.vs = vs; D.st.vdecl = dnocol; D.st.streams[0] = (mt_stream){vb, 0, sizeof(Vtx), 1};
    quad(q, 32, 32, 64, 64, 0.5f, 0xffffffff);
    memcpy(vb->buf.contents, q, sizeof q);
    call(S_Device_DrawPrimitive, 3, 5u, 0u, 2u);
    D.st.vdecl = dbasic;
    readback();
    /* UBYTE4 (255,128,0,255) /255 -> rgba (1, 0.502, 0, 1) = 0xffff8000 */
    CHECK(near(px(4, 4), 0xffff8000, 1), "UBYTE4/SHORT2 %08x", px(4, 4));
    CHECK(px(40, 40) == 0x00000000, "unfed colour input reads zeros %08x", px(40, 40));

    /* 11. stencil: whole clear stencil 0; draw left half writing ref 1 (ALWAYS/REPLACE, colour masked);
     *     then a full quad with EQUAL 1 -> only the left half */
    clear_all(0xff000000);
    D.st.rs[PL_RS_STENCILENABLE] = 1; D.st.rs[PL_RS_STENCILFUNC] = 8; D.st.rs[PL_RS_STENCILPASS] = 3; D.st.rs[PL_RS_STENCILREF] = 1;
    D.st.rs[PL_RS_COLORWRITEENABLE] = 0;
    quad(q, 0, 0, 32, 64, 0.5f, 0xffffffff);
    memcpy(vb->buf.contents, q, sizeof q);
    call(S_Device_DrawPrimitive, 3, 5u, 0u, 2u);
    D.st.rs[PL_RS_COLORWRITEENABLE] = 0xf; D.st.rs[PL_RS_STENCILFUNC] = 3; D.st.rs[PL_RS_STENCILPASS] = 1;
    quad(q, 0, 0, 64, 64, 0.5f, 0xff00ff00);
    memcpy(vb2->buf.contents, q, sizeof q);
    D.st.streams[0].vb = vb2;
    call(S_Device_DrawPrimitive, 3, 5u, 0u, 2u);
    D.st.rs[PL_RS_STENCILENABLE] = 0;
    D.st.streams[0].vb = vb;
    readback();
    CHECK(px(10, 10) == 0xff00ff00 && px(50, 10) == 0xff000000, "stencil %08x %08x", px(10, 10), px(50, 10));

    /* 12. instancing: corners (stream 0) x 4 instances (stream 1: offset + colour), INDEXEDDATA|4 / INSTANCEDATA|1 */
    clear_all(0xff000000);
    Vtx corners[4];
    quad(corners, 0, 0, 8, 8, 0.5f, 0);
    mt_obj *vbc = buffer(MT_K_VB, corners, sizeof corners);
    struct { float x, y; uint32_t c; } inst[4] = {{0, 0, 0xffff0000}, {16, 0, 0xff00ff00}, {32, 0, 0xff0000ff}, {48, 0, 0xffffffff}};
    mt_obj *vbi = buffer(MT_K_VB, inst, sizeof inst);
    /* position from stream 0, colour from stream 1 (per instance); the VS adds no offset, so every instance draws
     * at the same place: the last instance's colour wins -- checks the per-instance step and the instance count */
    static const uint16_t ei[3][6] = {{0, 0, 3, 0, 0, 0}, {0, 16, 1, 0, 5, 0}, {1, 8, 4, 0, 10, 0}};
    mt_obj *dinst = decl(ei, 3);
    uint16_t qi[6] = {0, 1, 2, 2, 1, 3};
    mt_obj *ibq = buffer(MT_K_IB, qi, sizeof qi); ibq->fmt = PL_D3DFMT_INDEX16;
    D.st.vdecl = dinst;
    D.st.streams[0] = (mt_stream){vbc, 0, sizeof(Vtx), PL_SSF_INDEXEDDATA | 4};
    D.st.streams[1] = (mt_stream){vbi, 0, 12, PL_SSF_INSTANCEDATA | 1};
    D.st.ib = ibq;
    call(S_Device_DrawIndexedPrimitive, 6, 4u, 0u, 0u, 4u, 0u, 2u);
    D.st.streams[0].freq = 1; D.st.streams[1] = (mt_stream){0};
    D.st.vdecl = dbasic; D.st.streams[0] = (mt_stream){vb, 0, sizeof(Vtx), 1};
    readback();
    CHECK(px(4, 4) == 0xffffffff, "instancing: last instance's colour %08x", px(4, 4));

    /* 13. an unbound sampler reads the (0,0,0,1) dummy */
    clear_all(0xffff0000);
    D.st.ps = pst;
    quad(q, 0, 0, 16, 16, 0.5f, 0xffffffff);
    memcpy(vb->buf.contents, q, sizeof q);
    call(S_Device_DrawPrimitive, 3, 5u, 0u, 2u);
    D.st.ps = ps;
    readback();
    CHECK(px(4, 4) == 0xff000000, "unbound sampler %08x", px(4, 4));

    /* 14. SetLOD (a MANAGED texture's most detailed level) and MAXMIPLEVEL limit the level sampled; the
     * larger wins. A 4x4 texture with three levels (green, blue, red) magnified on a 16x16 quad samples level 0
     * unless one of them says otherwise. */
    {
        MTLTextureDescriptor *mtd = [MTLTextureDescriptor texture2DDescriptorWithPixelFormat:MTLPixelFormatBGRA8Unorm width:4 height:4 mipmapped:YES];
        id<MTLTexture> mt = [D.mtl newTextureWithDescriptor:mtd];
        const uint32_t lc[3] = {0xff00ff00u, 0xff0000ffu, 0xffff0000u};
        for (int l = 0; l < 3; l++) {
            uint32_t b[16]; int w = 4 >> l;
            for (int i = 0; i < 16; i++) b[i] = lc[l];
            [mt replaceRegion:MTLRegionMake2D(0, 0, w, w) mipmapLevel:l withBytes:b bytesPerRow:4 * w];
        }
        mt_obj *to = new_obj(MT_K_TEX);
        to->tex = mt; to->w = to->h = 4; to->d = 1; to->levels = 3; to->pool = 1 /* MANAGED */;
        D.st.tex[0] = to;
        D.st.ss[0][PL_SAMP_MIPFILTER] = 1; /* POINT */
        const struct { uint32_t lod, maxmip, want; const char *what; } lc2[4] = {
            {0, 0, 0xff00ff00u, "level 0"}, {1, 0, 0xff0000ffu, "SetLOD(1)"}, {0, 2, 0xffff0000u, "MAXMIPLEVEL 2"},
            {2, 1, 0xffff0000u, "SetLOD(2) over MAXMIPLEVEL 1"}};
        for (int k = 0; k < 4; k++) {
            to->lod = lc2[k].lod;
            D.st.ss[0][PL_SAMP_MAXMIPLEVEL] = lc2[k].maxmip;
            D.st.dirty |= MT_DIRTY_SS;
            clear_all(0xff000000);
            D.st.ps = pst;
            quad(q, 0, 0, 16, 16, 0.5f, 0xffffffff);
            memcpy(vb->buf.contents, q, sizeof q);
            call(S_Device_DrawPrimitive, 3, 5u, 0u, 2u);
            D.st.ps = ps;
            readback();
            CHECK(px(8, 8) == lc2[k].want, "%s: %08x, expected %08x", lc2[k].what, px(8, 8), lc2[k].want);
        }
        D.st.tex[0] = NULL;
        D.st.ss[0][PL_SAMP_MIPFILTER] = 0; D.st.ss[0][PL_SAMP_MAXMIPLEVEL] = 0;
    }

    if (argc > 1) {
        FILE *f = fopen(argv[1], "wb");
        if (f) { fprintf(f, "P6 %d %d 255\n", W, W); for (int i = 0; i < W * W; i++) { uint8_t c[3] = {(uint8_t)(pix[i] >> 16), (uint8_t)(pix[i] >> 8), (uint8_t)pix[i]}; fwrite(c, 1, 3, f); } fclose(f); }
    }
    uint64_t s[14];
    extern void d3d9metal_pl_stats(uint64_t out[14]);
    d3d9metal_pl_stats(s);
    printf("pipeline stats: draws %llu skipped %llu pso %llu (fail %llu) dss %llu samplers %llu fans %llu restart %llu "
           "up_bytes %llu clears load/quad %llu/%llu encoders %llu passes %llu\n",
           s[0], s[2], s[3], s[4], s[5], s[6], s[7], s[8], s[9], s[10], s[11], s[12], (unsigned long long)g_passes);
    CHECK(s[2] == 0, "no draw skipped (%llu)", s[2]);
    CHECK(s[4] == 0, "no pipeline failed (%llu)", s[4]);
    mt_pl_device_destroyed(&D);
    }
    printf("test_pl_gpu: %d passed, %d failed\n", pass, fail);
    return fail != 0;
}
