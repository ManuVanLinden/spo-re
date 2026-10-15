// test_d3d9msl -- tests of the run-time shader translator (our own shaders only; no game data).
//
//   test_d3d9msl CACHEDIR [BLOBDIR]
//
// 1. API and error handling on hand-encoded token streams (parse errors, SM1, predication, bad entry).
// 2. Reflection of small shaders (constants used, inputs, outputs, samplers).
// 3. With BLOBDIR (*.bin bytecode of our fixtures, written by crosscheck.py): every blob is translated by the C
//    library in this process and compiled with the system Metal compiler (MTLMathModeSafe), every function
//    specialised with the alpha-test function constant; vertex/fragment pairs are not linked here.
// 4. GPU render checks of the D3D9 conventions the translator emits: the per-sampler state block (border
//    colour, sRGB decode, LOD bias for texld and texldl, depth compare), texldp, texkill, the alpha-test
//    function constant, and the half-pixel position fixup.
// 5. The compile cache: miss, hit with identical output, a different option set is a different key, a
//    corrupted entry is a miss.
// Build: see ../Makefile.
#import <Foundation/Foundation.h>
#import <Metal/Metal.h>
#pragma clang diagnostic ignored "-Wdeprecated-declarations"  // fastMathEnabled on macOS < 15
#include <dirent.h>
#include <math.h>
#include "d3d9msl.h"

static int g_pass, g_fail;
#define CHECK(c, ...) do { if (c) g_pass++; else { g_fail++; printf("FAIL %s:%d ", __FILE__, __LINE__); printf(__VA_ARGS__); printf("\n"); } } while (0)

// assembled from tools/shaders/d3dbc.py (our own shaders):
// vs_3_0 dcl_position v0; dcl_texcoord v1; dcl_position o0; dcl_texcoord o1; mov o0, v0; mov o1, v1
static const uint32_t VS_PASS[] = {0xfffe0300, 0x0200001f, 0x80000000, 0x900f0000, 0x0200001f, 0x80000005, 0x900f0001, 0x0200001f, 0x80000000, 0xe00f0000, 0x0200001f, 0x80000005, 0xe00f0001, 0x02000001, 0xe00f0000, 0x90e40000, 0x02000001, 0xe00f0001, 0x90e40001, 0x0000ffff};
// ps_3_0 dcl_texcoord v0; dcl_2d s0; texld r0, v0, s0; mov oC0, r0
static const uint32_t PS_TEX[] = {0xffff0300, 0x0200001f, 0x80000005, 0x900f0000, 0x0200001f, 0x90000000, 0xa00f0800, 0x03000042, 0x800f0000, 0x90e40000, 0xa0e40800, 0x02000001, 0x800f0800, 0x80e40000, 0x0000ffff};
// the same with texldp
static const uint32_t PS_TEXP[] = {0xffff0300, 0x0200001f, 0x80000005, 0x900f0000, 0x0200001f, 0x90000000, 0xa00f0800, 0x03010042, 0x800f0000, 0x90e40000, 0xa0e40800, 0x02000001, 0x800f0800, 0x80e40000, 0x0000ffff};
// the same with texldl
static const uint32_t PS_TEXL[] = {0xffff0300, 0x0200001f, 0x80000005, 0x900f0000, 0x0200001f, 0x90000000, 0xa00f0800, 0x0300005f, 0x800f0000, 0x90e40000, 0xa0e40800, 0x02000001, 0x800f0800, 0x80e40000, 0x0000ffff};
// ps_2_0 dcl t0; def c0, 1, 0, 0, 1; texkill t0; mov oC0, c0
static const uint32_t PS_KILL[] = {0xffff0200, 0x0200001f, 0x80000000, 0xb00f0000, 0x05000051, 0xa00f0000, 0x3f800000, 0x00000000, 0x00000000, 0x3f800000, 0x01000041, 0xb00f0000, 0x02000001, 0x800f0800, 0xa0e40000, 0x0000ffff};
#define N(a) (sizeof(a) / sizeof((a)[0]))

static id<MTLDevice> g_dev;
static id<MTLCommandQueue> g_q;

static MTLCompileOptions *copts(void) {
    MTLCompileOptions *o = [MTLCompileOptions new];
    if (@available(macOS 15.0, *)) o.mathMode = MTLMathModeSafe;
    else o.fastMathEnabled = NO;
    return o;
}

static id<MTLFunction> fn_of(const uint32_t *t, size_t n, const char *entry, uint32_t alpha_func, d3d9msl_reflection *refl) {
    d3d9msl_result r;
    d3d9msl_options o = {entry, 0};
    if (d3d9msl_translate_opts(t, n, &o, &r)) { printf("translate %s: %s\n", entry, r.error); return nil; }
    if (refl) *refl = r.refl;
    NSError *err = nil;
    id<MTLLibrary> lib = [g_dev newLibraryWithSource:@(r.msl) options:copts() error:&err];
    d3d9msl_result_free(&r);
    if (!lib) { printf("compile %s: %s\n", entry, err.localizedDescription.UTF8String); return nil; }
    MTLFunctionConstantValues *cv = [MTLFunctionConstantValues new];
    [cv setConstantValue:&alpha_func type:MTLDataTypeUInt atIndex:D3D9MSL_FC_ALPHA_FUNC];
    id<MTLFunction> f = [lib newFunctionWithName:@(entry) constantValues:cv error:&err];
    if (!f) printf("function %s: %s\n", entry, err.localizedDescription.UTF8String);
    return f;
}

typedef struct { float pos[4], tc[4]; } vert;

// Draw a 4-vertex strip into an 8x8 RGBA32Float target cleared to 0; returns the pixels (8*8*4 floats).
static int draw(const uint32_t *ps, size_t nps, uint32_t alpha_func, const vert *v, id<MTLTexture> tex,
                id<MTLSamplerState> smp, const d3d9msl_aux *vaux, const d3d9msl_aux *paux, float *out) {
    static int seq;
    char ve[32], pe[32];
    snprintf(ve, sizeof ve, "vs_%d", ++seq);
    snprintf(pe, sizeof pe, "ps_%d", seq);
    id<MTLFunction> vf = fn_of(VS_PASS, N(VS_PASS), ve, 8, NULL), pf = fn_of(ps, nps, pe, alpha_func, NULL);
    if (!vf || !pf) return -1;
    MTLVertexDescriptor *vd = [MTLVertexDescriptor vertexDescriptor];
    for (int a = 0; a < 2; a++) {
        vd.attributes[a].format = MTLVertexFormatFloat4;
        vd.attributes[a].offset = 16 * a;
        vd.attributes[a].bufferIndex = D3D9MSL_BUF_STREAM0;
    }
    vd.layouts[D3D9MSL_BUF_STREAM0].stride = sizeof(vert);
    MTLRenderPipelineDescriptor *pd = [MTLRenderPipelineDescriptor new];
    pd.vertexFunction = vf;
    pd.fragmentFunction = pf;
    pd.vertexDescriptor = vd;
    pd.colorAttachments[0].pixelFormat = MTLPixelFormatRGBA32Float;
    NSError *err = nil;
    id<MTLRenderPipelineState> pso = [g_dev newRenderPipelineStateWithDescriptor:pd error:&err];
    if (!pso) { printf("pipeline: %s\n", err.localizedDescription.UTF8String); return -1; }
    MTLTextureDescriptor *td = [MTLTextureDescriptor texture2DDescriptorWithPixelFormat:MTLPixelFormatRGBA32Float width:8 height:8 mipmapped:NO];
    td.usage = MTLTextureUsageRenderTarget;
    td.storageMode = MTLStorageModeShared;
    id<MTLTexture> rt = [g_dev newTextureWithDescriptor:td];
    MTLRenderPassDescriptor *rp = [MTLRenderPassDescriptor renderPassDescriptor];
    rp.colorAttachments[0].texture = rt;
    rp.colorAttachments[0].loadAction = MTLLoadActionClear;
    rp.colorAttachments[0].clearColor = MTLClearColorMake(0, 0, 0, 0);
    rp.colorAttachments[0].storeAction = MTLStoreActionStore;
    id<MTLCommandBuffer> cb = [g_q commandBuffer];
    id<MTLRenderCommandEncoder> enc = [cb renderCommandEncoderWithDescriptor:rp];
    [enc setRenderPipelineState:pso];
    static float zeros[256 * 4];
    [enc setVertexBytes:zeros length:4096 atIndex:D3D9MSL_BUF_CONST_F];
    [enc setVertexBytes:zeros length:256 atIndex:D3D9MSL_BUF_CONST_I];
    [enc setVertexBytes:zeros length:64 atIndex:D3D9MSL_BUF_CONST_B];
    [enc setVertexBytes:vaux length:sizeof *vaux atIndex:D3D9MSL_BUF_AUX];
    [enc setFragmentBytes:zeros length:224 * 16 atIndex:D3D9MSL_BUF_CONST_F];
    [enc setFragmentBytes:zeros length:256 atIndex:D3D9MSL_BUF_CONST_I];
    [enc setFragmentBytes:zeros length:64 atIndex:D3D9MSL_BUF_CONST_B];
    [enc setFragmentBytes:paux length:sizeof *paux atIndex:D3D9MSL_BUF_AUX];
    [enc setVertexBytes:v length:4 * sizeof(vert) atIndex:D3D9MSL_BUF_STREAM0];
    if (tex) [enc setFragmentTexture:tex atIndex:0];
    if (smp) [enc setFragmentSamplerState:smp atIndex:0];
    [enc drawPrimitives:MTLPrimitiveTypeTriangleStrip vertexStart:0 vertexCount:4];
    [enc endEncoding];
    [cb commit];
    [cb waitUntilCompleted];
    if (cb.error) { printf("command buffer: %s\n", cb.error.localizedDescription.UTF8String); return -1; }
    [rt getBytes:out bytesPerRow:8 * 16 fromRegion:MTLRegionMake2D(0, 0, 8, 8) mipmapLevel:0];
    return 0;
}

static void quad(vert *v, float x0, float y0, float x1, float y1, float u0, float v0, float u1, float v1, float z, float w) {
    float P[4][4] = {{x0, y0, 0, 1}, {x1, y0, 0, 1}, {x0, y1, 0, 1}, {x1, y1, 0, 1}};
    float T[4][2] = {{u0, v0}, {u1, v0}, {u0, v1}, {u1, v1}};
    for (int i = 0; i < 4; i++) {
        memcpy(v[i].pos, P[i], 16);
        v[i].tc[0] = T[i][0] * w;
        v[i].tc[1] = T[i][1] * w;
        v[i].tc[2] = z * w;
        v[i].tc[3] = w;
    }
}

static int near4(const float *a, const float *b) {
    for (int k = 0; k < 4; k++)
        if (fabsf(a[k] - b[k]) > 2e-3f) return 0;
    return 1;
}
static float s2l(float c) { return c <= 0.04045f ? c / 12.92f : powf((c + 0.055f) / 1.055f, 2.4f); }

static void gpu_tests(void) {
    // 4x4 RGBA8 texture, three mip levels: level 0 texel (x,y) = (64x, 64y, 128, 255); level 1 green; level 2 blue
    MTLTextureDescriptor *td = [MTLTextureDescriptor texture2DDescriptorWithPixelFormat:MTLPixelFormatRGBA8Unorm width:4 height:4 mipmapped:YES];
    id<MTLTexture> tex = [g_dev newTextureWithDescriptor:td];
    uint8_t l0[16 * 4], l1[4 * 4], l2[4];
    for (int y = 0; y < 4; y++)
        for (int x = 0; x < 4; x++) {
            uint8_t *p = &l0[(y * 4 + x) * 4];
            p[0] = (uint8_t)(64 * x); p[1] = (uint8_t)(64 * y); p[2] = 128; p[3] = 255;
        }
    for (int i = 0; i < 4; i++) { l1[i * 4] = 0; l1[i * 4 + 1] = 255; l1[i * 4 + 2] = 0; l1[i * 4 + 3] = 255; }
    l2[0] = 0; l2[1] = 0; l2[2] = 255; l2[3] = 255;
    [tex replaceRegion:MTLRegionMake2D(0, 0, 4, 4) mipmapLevel:0 withBytes:l0 bytesPerRow:16];
    [tex replaceRegion:MTLRegionMake2D(0, 0, 2, 2) mipmapLevel:1 withBytes:l1 bytesPerRow:8];
    [tex replaceRegion:MTLRegionMake2D(0, 0, 1, 1) mipmapLevel:2 withBytes:l2 bytesPerRow:4];
    MTLSamplerDescriptor *sd = [MTLSamplerDescriptor new];
    sd.minFilter = sd.magFilter = MTLSamplerMinMagFilterNearest;
    sd.mipFilter = MTLSamplerMipFilterNearest;
    sd.sAddressMode = sd.tAddressMode = MTLSamplerAddressModeClampToEdge;
    id<MTLSamplerState> smp = [g_dev newSamplerStateWithDescriptor:sd];
    // full-target quad, texcoords -0.5..1.5: pixel i (2..5) samples texel i-2, pixels 0,1,6,7 are outside [0,1]
    vert v[4];
    quad(v, -1, 1, 1, -1, -0.5f, -0.5f, 1.5f, 1.5f, 0.3f, 1);
    d3d9msl_aux va, pa;
    memset(&va, 0, sizeof va);
    memset(&pa, 0, sizeof pa);
    float px[8 * 8 * 4];
#define PX(x, y) (&px[((y) * 8 + (x)) * 4])
#define T0(x, y) ((float[4]){64 * (x) / 255.0f, 64 * (y) / 255.0f, 128 / 255.0f, 1})
    if (!draw(PS_TEX, N(PS_TEX), 8, v, tex, smp, &va, &pa, px)) {
        CHECK(near4(PX(3, 4), T0(1, 2)), "texld nearest: pixel (3,4) %g %g %g %g", PX(3, 4)[0], PX(3, 4)[1], PX(3, 4)[2], PX(3, 4)[3]);
        CHECK(near4(PX(0, 0), T0(0, 0)), "texld clamp-to-edge outside");
    } else CHECK(0, "draw texld");
    // border colour the Metal sampler cannot hold
    pa.samp[0].flags = D3D9MSL_SAMP_BORDER_U | D3D9MSL_SAMP_BORDER_V;
    float bc[4] = {0.25f, 0.5f, 0.75f, 0.125f};
    memcpy(pa.samp[0].border, bc, 16);
    if (!draw(PS_TEX, N(PS_TEX), 8, v, tex, smp, &va, &pa, px)) {
        CHECK(near4(PX(0, 3), bc), "border: u outside");
        CHECK(near4(PX(3, 7), bc), "border: v outside");
        CHECK(near4(PX(3, 3), T0(1, 1)), "border: inside unchanged");
    } else CHECK(0, "draw border");
    pa.samp[0].flags = D3D9MSL_SAMP_BORDER_U;
    if (!draw(PS_TEX, N(PS_TEX), 8, v, tex, smp, &va, &pa, px)) {
        CHECK(near4(PX(3, 7), T0(1, 3)), "border on U only: v outside clamps");
        CHECK(near4(PX(7, 3), bc), "border on U only: u outside");
    } else CHECK(0, "draw border U");
    // sRGB read
    pa.samp[0].flags = D3D9MSL_SAMP_SRGB;
    if (!draw(PS_TEX, N(PS_TEX), 8, v, tex, smp, &va, &pa, px)) {
        float e[4] = {s2l(64 / 255.0f), s2l(128 / 255.0f), s2l(128 / 255.0f), 1};
        CHECK(near4(PX(3, 4), e), "sRGB decode: %g %g %g %g vs %g %g %g", PX(3, 4)[0], PX(3, 4)[1], PX(3, 4)[2], PX(3, 4)[3], e[0], e[1], e[2]);
    } else CHECK(0, "draw srgb");
    // LOD bias (texld) and texldl with and without bias
    pa.samp[0].flags = 0;
    float green[4] = {0, 1, 0, 1}, blue[4] = {0, 0, 1, 1};
    pa.samp[0].mip_bias = 1.0f;
    if (!draw(PS_TEX, N(PS_TEX), 8, v, tex, smp, &va, &pa, px)) CHECK(near4(PX(3, 3), green), "texld bias 1 -> level 1");
    pa.samp[0].mip_bias = 2.0f;
    if (!draw(PS_TEX, N(PS_TEX), 8, v, tex, smp, &va, &pa, px)) CHECK(near4(PX(3, 3), blue), "texld bias 2 -> level 2");
    pa.samp[0].mip_bias = 0.0f;
    if (!draw(PS_TEX, N(PS_TEX), 8, v, tex, smp, &va, &pa, px)) CHECK(near4(PX(3, 3), T0(1, 1)), "texld bias 0 -> level 0");
    quad(v, -1, 1, 1, -1, -0.5f, -0.5f, 1.5f, 1.5f, 0.3f, 1);
    for (int i = 0; i < 4; i++) v[i].tc[3] = 1.0f;  // texldl LOD = w
    if (!draw(PS_TEXL, N(PS_TEXL), 8, v, tex, smp, &va, &pa, px)) CHECK(near4(PX(3, 3), green), "texldl lod 1");
    pa.samp[0].mip_bias = 1.0f;
    if (!draw(PS_TEXL, N(PS_TEXL), 8, v, tex, smp, &va, &pa, px)) CHECK(near4(PX(3, 3), blue), "texldl lod 1 + bias 1");
    pa.samp[0].mip_bias = 0.0f;
    // texldp: coordinates multiplied by w = 2
    quad(v, -1, 1, 1, -1, -0.5f, -0.5f, 1.5f, 1.5f, 0.3f, 2);
    if (!draw(PS_TEXP, N(PS_TEXP), 8, v, tex, smp, &va, &pa, px)) CHECK(near4(PX(3, 4), T0(1, 2)), "texldp");
    // depth compare: R32Float texture, depth at texel x = 0.125 + 0.25x, reference z = 0.3 (texldp: 0.6/2)
    {
        MTLTextureDescriptor *dd = [MTLTextureDescriptor texture2DDescriptorWithPixelFormat:MTLPixelFormatR32Float width:4 height:4 mipmapped:NO];
        id<MTLTexture> dt = [g_dev newTextureWithDescriptor:dd];
        float dv[16];
        for (int i = 0; i < 16; i++) dv[i] = 0.125f + 0.25f * (float)(i % 4);
        [dt replaceRegion:MTLRegionMake2D(0, 0, 4, 4) mipmapLevel:0 withBytes:dv bytesPerRow:16];
        pa.samp[0].flags = D3D9MSL_SAMP_DEPTH_CMP;
        float one[4] = {1, 1, 1, 1}, zero[4] = {0, 0, 0, 0};
        quad(v, -1, 1, 1, -1, -0.5f, -0.5f, 1.5f, 1.5f, 0.3f, 1);
        if (!draw(PS_TEX, N(PS_TEX), 8, v, dt, smp, &va, &pa, px)) {
            CHECK(near4(PX(2, 3), zero), "depth compare: 0.3 > 0.125 -> 0");
            CHECK(near4(PX(3, 3), one), "depth compare: 0.3 <= 0.375 -> 1");
        } else CHECK(0, "draw depth");
        quad(v, -1, 1, 1, -1, -0.5f, -0.5f, 1.5f, 1.5f, 0.3f, 2);
        if (!draw(PS_TEXP, N(PS_TEXP), 8, v, dt, smp, &va, &pa, px)) {
            CHECK(near4(PX(2, 3), zero) && near4(PX(3, 3), one), "depth compare with texldp (ref z/w)");
        } else CHECK(0, "draw depth p");
        pa.samp[0].flags = 0;
    }
    // alpha test through the function constant: LESS against 0.5 with alpha 1 discards, GREATEREQUAL keeps
    quad(v, -1, 1, 1, -1, -0.5f, -0.5f, 1.5f, 1.5f, 0.3f, 1);
    pa.alpha_ref = 0.5f;
    if (!draw(PS_TEX, N(PS_TEX), 2, v, tex, smp, &va, &pa, px)) CHECK(PX(3, 3)[3] == 0.0f, "alpha test LESS discards");
    if (!draw(PS_TEX, N(PS_TEX), 7, v, tex, smp, &va, &pa, px)) CHECK(PX(3, 3)[3] == 1.0f, "alpha test GREATEREQUAL keeps");
    pa.alpha_ref = 0;
    // half-pixel fixup: a D3D9 quad over window x,y in [-0.5, 3.5] covers exactly pixels 0..3 on Windows
    {
        float x0 = -0.5f / 4 - 1, x1 = 3.5f / 4 - 1, y0 = 1 + 0.5f / 4, y1 = 1 - 3.5f / 4;
        quad(v, x0, y0, x1, y1, 0, 0, 1, 1, 0, 1);
        int cov_fix = 0, cov_nofix = 0, exact = 1;
        va.pos_fixup[0] = 1.0f / 8;
        va.pos_fixup[1] = -1.0f / 8;
        if (!draw(PS_KILL, N(PS_KILL), 8, v, NULL, NULL, &va, &pa, px)) {
            for (int y = 0; y < 8; y++)
                for (int x = 0; x < 8; x++) {
                    int in = PX(x, y)[0] == 1.0f;
                    cov_fix += in;
                    if (in != (x < 4 && y < 4)) exact = 0;
                }
        }
        memset(va.pos_fixup, 0, sizeof va.pos_fixup);
        if (!draw(PS_KILL, N(PS_KILL), 8, v, NULL, NULL, &va, &pa, px))
            for (int i = 0; i < 64; i++) cov_nofix += px[i * 4] == 1.0f;
        CHECK(cov_fix == 16 && exact, "half-pixel fixup: %d pixels covered (want pixels 0..3 x 0..3)", cov_fix);
        CHECK(cov_nofix == 9, "without the fixup the same quad covers %d pixels (Metal rules: 9)", cov_nofix);
        // texkill: negative texcoord kills every pixel
        quad(v, -1, 1, 1, -1, -1, -1, -0.5f, -0.5f, 0, 1);
        int left = 0;
        if (!draw(PS_KILL, N(PS_KILL), 8, v, NULL, NULL, &va, &pa, px))
            for (int i = 0; i < 64; i++) left += px[i * 4] != 0.0f;
        CHECK(left == 0, "texkill: %d pixels survived", left);
    }
}

static void api_tests(void) {
    d3d9msl_result r;
    CHECK(d3d9msl_translate(VS_PASS, N(VS_PASS), &r) == 0 && r.msl && strstr(r.msl, "vertex sg_main_out sg_main("), "default entry");
    CHECK(r.refl.stage == 0 && r.refl.ninputs == 2 && r.refl.noutputs == 2 && r.refl.position_fixup, "VS reflection");
    CHECK(r.refl.inputs[1].usage == 5 && r.refl.inputs[1].attribute == 1, "VS input texcoord at attribute 1");
    CHECK(r.refl.outputs[0].kind == D3D9MSL_OUT_POSITION && r.refl.outputs[1].kind == D3D9MSL_OUT_USER &&
          !strcmp(r.refl.outputs[1].semantic, "texcoord0"), "VS outputs");
    CHECK(d3d9msl_length(VS_PASS, 1000) == N(VS_PASS), "length");
    d3d9msl_result_free(&r);
    CHECK(d3d9msl_translate(PS_TEX, N(PS_TEX), &r) == 0 && r.refl.sampler_mask == 1 && r.refl.sampler_type[0] == 2 &&
          r.refl.alpha_test && r.refl.inputs[0].usage == 5, "PS reflection");
    d3d9msl_result_free(&r);
    CHECK(d3d9msl_translate(PS_KILL, N(PS_KILL), &r) == 0 && r.refl.uses_texkill && r.refl.defs == 1 &&
          r.refl.c_max == 0 && r.refl.c_used[0] == 0, "ps_2_0 texkill, def'd constant not counted as used");
    d3d9msl_result_free(&r);
    // vs_3_0 mov r0, c1 ; mov o0? -> o0 written without dcl is refused
    static const uint32_t NODCL[] = {0xFFFE0300, 0x02000001, 0xE00F0000, 0xA0E40001, 0x0000FFFF};
    CHECK(d3d9msl_translate(NODCL, N(NODCL), &r) == D3D9MSL_EUNSUPPORTED && strstr(r.error, "without dcl"), "o0 without dcl: %s", r.error);
    static const uint32_t MOV[] = {0xFFFE0300, 0x02000001, 0x800F0000, 0xA0E40001, 0x0000FFFF};
    CHECK(d3d9msl_translate(MOV, N(MOV), &r) == 0 && (r.refl.c_used[0] == 2u) && r.refl.c_max == 1, "c1 used");
    d3d9msl_result_free(&r);
    CHECK(d3d9msl_translate(MOV, 0, &r) == D3D9MSL_EPARSE, "empty");
    CHECK(d3d9msl_translate(MOV, 4, &r) == D3D9MSL_EPARSE && strstr(r.error, "no end token"), "no end: %s", r.error);
    static const uint32_t BADVER[] = {0x12340300, 0x0000FFFF};
    CHECK(d3d9msl_translate(BADVER, 2, &r) == D3D9MSL_EPARSE, "bad version");
    static const uint32_t SM1[] = {0xFFFE0101, 0x0000FFFF};
    CHECK(d3d9msl_translate(SM1, 2, &r) == D3D9MSL_EUNSUPPORTED, "vs_1_1 refused");
    static const uint32_t UNK[] = {0xFFFE0300, 0x00000063, 0x0000FFFF};
    CHECK(d3d9msl_translate(UNK, 3, &r) == D3D9MSL_EPARSE && strstr(r.error, "unknown opcode"), "unknown opcode");
    static const uint32_t SHORT[] = {0xFFFE0300, 0x03000001, 0x800F0000, 0xA0E40001, 0x0000FFFF};
    CHECK(d3d9msl_translate(SHORT, N(SHORT), &r) == D3D9MSL_EPARSE, "length field mismatch");
    // predicated mov (bit 28), predicate register p0 (type 19): refused as unsupported
    static const uint32_t PRED[] = {0xFFFE0300, 0x13000001, 0x800F0000, 0xB0E01000, 0xA0E40001, 0x0000FFFF};
    int rc = d3d9msl_translate(PRED, N(PRED), &r);
    CHECK(rc == D3D9MSL_EUNSUPPORTED && strstr(r.error, "predication"), "predication refused: %d %s", rc, r.error);
    d3d9msl_options bad = {"9bad", 0};
    CHECK(d3d9msl_translate_opts(MOV, N(MOV), &bad, &r) == D3D9MSL_EARG, "bad entry name");
    d3d9msl_options plain = {"p", D3D9MSL_OPT_PLAIN};
    CHECK(d3d9msl_translate_opts(PS_TEX, N(PS_TEX), &plain, &r) == D3D9MSL_EUNSUPPORTED, "plain PS refused");
    d3d9msl_options flat = {"f", D3D9MSL_OPT_FLAT | D3D9MSL_OPT_FOG_VERTEX};
    static const uint32_t PS2COL[] = {0xFFFF0200, 0x0200001F, 0x80000000, 0x900F0000, 0x02000001, 0x800F0800, 0x90E40000, 0x0000FFFF};
    CHECK(d3d9msl_translate_opts(PS2COL, N(PS2COL), &flat, &r) == 0 && r.refl.inputs[0].flat && r.refl.fog_input &&
          strstr(r.msl, "[[user(color0), flat]]") && strstr(r.msl, "aux.fogColor"), "flat + vertex fog");
    d3d9msl_result_free(&r);
    char *js = NULL;
    if (d3d9msl_translate(PS_TEX, N(PS_TEX), &r) == 0) js = d3d9msl_reflection_json(&r.refl, r.entry);
    CHECK(js && strstr(js, "\"samplers\": {\"0\": \"2d\"}"), "reflection JSON");
    free(js);
    d3d9msl_result_free(&r);
    CHECK(sizeof(d3d9msl_aux) == 592 && sizeof(d3d9msl_sampler) == 32, "SgAux layout size %zu", sizeof(d3d9msl_aux));
}

static void cache_tests(const char *dir) {
    char d[1024];
    snprintf(d, sizeof d, "%s/cache-test", dir);
    CHECK(d3d9msl_cache_init(d) == 0, "cache init %s", d);
    char key[17];
    d3d9msl_options o = {"c_entry", 0}, o2 = {"c_entry", D3D9MSL_OPT_FLAT};
    d3d9msl_cache_key(PS_TEX, N(PS_TEX), &o, key);
    char path[1200];
    snprintf(path, sizeof path, "%s/%s.d3d9msl", d, key);
    unlink(path);
    d3d9msl_result a, b, c;
    int hit = -1;
    CHECK(d3d9msl_translate_cached(PS_TEX, N(PS_TEX), &o, &a, &hit) == 0 && hit == 0, "first lookup is a miss");
    CHECK(d3d9msl_translate_cached(PS_TEX, N(PS_TEX), &o, &b, &hit) == 0 && hit == 1, "second lookup hits");
    CHECK(a.msl && b.msl && a.msl_len == b.msl_len && !strcmp(a.msl, b.msl) && !memcmp(&a.refl, &b.refl, sizeof a.refl) &&
          !strcmp(b.entry, "c_entry"), "hit returns identical MSL and reflection");
    char key2[17];
    d3d9msl_cache_key(PS_TEX, N(PS_TEX), &o2, key2);
    CHECK(strcmp(key, key2) != 0, "options change the key");
    FILE *f = fopen(path, "r+b");
    if (f) { fseek(f, 40, SEEK_SET); fputc(0x5a, f); fclose(f); }  // inside the stored bytecode
    CHECK(d3d9msl_translate_cached(PS_TEX, N(PS_TEX), &o, &c, &hit) == 0 && hit == 0 && !strcmp(c.msl, a.msl),
          "corrupted entry is a miss and is rewritten");
    d3d9msl_result_free(&a);
    d3d9msl_result_free(&b);
    d3d9msl_result_free(&c);
    CHECK(d3d9msl_translate_cached(PS_TEX, N(PS_TEX), &o, &c, &hit) == 0 && hit == 1, "rewritten entry hits");
    d3d9msl_result_free(&c);
    d3d9msl_cache_stats s;
    d3d9msl_cache_get_stats(&s);
    printf("cache: %s hits %llu misses %llu stores %llu errors %llu\n", d, (unsigned long long)s.hits,
           (unsigned long long)s.misses, (unsigned long long)s.stores, (unsigned long long)s.errors);
}

static void blob_tests(const char *blobdir) {
    DIR *dp = opendir(blobdir);
    if (!dp) { CHECK(0, "cannot open %s", blobdir); return; }
    struct dirent *de;
    int n = 0, ok = 0;
    double tt = 0, tc = 0;
    while ((de = readdir(dp))) {
        size_t l = strlen(de->d_name);
        if (l < 5 || strcmp(de->d_name + l - 4, ".bin")) continue;
        // fixture names are <fixture>.o<flags>.bin; each gets translated with those flags
        unsigned flags = 0;
        const char *o = strstr(de->d_name, ".o");
        if (o) flags = (unsigned)atoi(o + 2);
        char path[2048], entry[64];
        snprintf(path, sizeof path, "%s/%s", blobdir, de->d_name);
        snprintf(entry, sizeof entry, "f%d", n);
        NSData *data = [NSData dataWithContentsOfFile:@(path)];
        if (!data) continue;
        n++;
        d3d9msl_options op = {entry, flags};
        d3d9msl_result r;
        NSDate *t0 = [NSDate date];
        int rc = d3d9msl_translate_opts(data.bytes, data.length / 4, &op, &r);
        tt += -[t0 timeIntervalSinceNow];
        if (rc) {
            if ((flags & D3D9MSL_OPT_PLAIN) && rc == D3D9MSL_EUNSUPPORTED) { ok++; continue; }  // expected refusal
            CHECK(0, "%s: %s", de->d_name, r.error);
            continue;
        }
        NSError *err = nil;
        t0 = [NSDate date];
        id<MTLLibrary> lib = [g_dev newLibraryWithSource:@(r.msl) options:copts() error:&err];
        int good = lib != nil;
        if (lib && !(flags & D3D9MSL_OPT_PLAIN)) {
            MTLFunctionConstantValues *cv = [MTLFunctionConstantValues new];
            uint32_t fa = 7;
            [cv setConstantValue:&fa type:MTLDataTypeUInt atIndex:0];
            good = [lib newFunctionWithName:@(entry) constantValues:cv error:&err] != nil;
        }
        tc += -[t0 timeIntervalSinceNow];
        CHECK(good, "%s: Metal compile: %s", de->d_name, err.localizedDescription.UTF8String);
        ok += good;
        d3d9msl_result_free(&r);
    }
    closedir(dp);
    printf("fixtures: %d/%d translated by the C library and compiled (translate %.1f us each, compile %.1f ms each)\n",
           ok, n, n ? tt * 1e6 / n : 0.0, n ? tc * 1e3 / n : 0.0);
    CHECK(n > 0, "no fixture blobs in %s", blobdir);
}

int main(int argc, char **argv) {
    @autoreleasepool {
        if (argc < 2) { fprintf(stderr, "usage: test_d3d9msl CACHEDIR [BLOBDIR]\n"); return 2; }
        g_dev = MTLCreateSystemDefaultDevice();
        if (!g_dev) { printf("no Metal device\n"); return 2; }
        g_q = [g_dev newCommandQueue];
        api_tests();
        cache_tests(argv[1]);
        if (argc > 2) blob_tests(argv[2]);
        gpu_tests();
        printf("%d passed, %d failed\n", g_pass, g_fail);
        return g_fail ? 1 : 0;
    }
}
