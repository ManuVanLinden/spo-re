/* test_readback.c -- the null device's readback test run against the Metal back end, with the window check
 * made for a CAMetalLayer we present into ourselves. The draws need the pipeline package; with the device
 * layer's stubs only the clear, the copies and the readback are real.
 * The test: render-target readback entirely through the guest vtables (the way
 * translated code calls them): clear a render target to a colour, draw a textured quad with the conformance
 * test's SM3 shaders (vs_basic, ps_tex; our own bytecode), the texture written through LockRect into the guest
 * shadow (whole level, then a dirty rectangle), the vertices through a DISCARD Lock of a dynamic vertex
 * buffer, plus a DrawPrimitiveUP quad from guest memory; read the pixels back with GetRenderTargetData
 * into a SYSTEMMEM surface, LockRect(READONLY) it and check them in guest memory. Then copy the target to
 * the back buffer, Present into the window, and read the back buffer back too.
 * Usage: test_readback <outdir>   (writes readback.ppm) */
#include <stdlib.h>
#include <unistd.h>

#include "com_harness.h"
#include "../src/d3d9_int.h"
#include "d3d9metal.h"
#include "test_shaders.h" /* $(OUT)/gen: tests/shaders/*.asm assembled by tests/gen_shaders.py */

void hle_stop_check(sg_ctx *ctx, uint8_t *mem, const char *import) { (void)ctx; (void)mem; (void)import; }
extern void sg_imp_d3d9_Direct3DCreate9(sg_ctx *ctx, uint8_t *mem);
void test_app_init(void);
void test_window_size(int w, int h);
void test_window_close(void);
void test_pump(void);

#define W 256
#define CLEAR 0xff336699u
static const uint32_t QC[4] = {0xffff0000u, 0xff00ff00u, 0xff0000ffu, 0xffffffffu}; /* TL red, TR green, BL blue, BR white */

static uint32_t f2u(float f) { uint32_t u; memcpy(&u, &f, 4); return u; }
static uint32_t put_blob(const uint32_t *p, size_t n) {
    uint32_t a = gscratch((uint32_t)(n * 4));
    for (size_t i = 0; i < n; i++) gst(a + 4 * (uint32_t)i, p[i]);
    return a;
}
/* one vertex: x y z w u v color (28 bytes) */
static void put_vtx(uint32_t a, float x, float y, float u, float v) {
    gst(a, f2u(x)); gst(a + 4, f2u(y)); gst(a + 8, f2u(0.5f)); gst(a + 12, f2u(1.0f));
    gst(a + 16, f2u(u)); gst(a + 20, f2u(v)); gst(a + 24, 0xffffffffu);
}
static void put_quad(uint32_t a, float x0, float y0, float x1, float y1) { /* triangle strip */
    put_vtx(a, x0, y0, 0, 0);
    put_vtx(a + 28, x1, y0, 1, 0);
    put_vtx(a + 56, x0, y1, 0, 1);
    put_vtx(a + 84, x1, y1, 1, 1);
}
static uint32_t px(uint32_t bits, uint32_t pitch, int x, int y) { return gld(bits + (uint32_t)y * pitch + 4u * (uint32_t)x); }

int main(int argc, char **argv) {
    const char *outdir = argc > 1 ? argv[1] : ".";
    sg_ctx *ctx = &T;
    uint8_t *mem;
    harness_init();
    mem = M;
    test_app_init();
    test_window_size(W, W);
    if (!getenv("SPORE_D3D9")) setenv("SPORE_D3D9", "metal", 1);
    setenv("SPORE_D3D9_FPS", "0", 1);

    SG_PUSH32(32);
    SG_CALL(sg_imp_d3d9_Direct3DCreate9, 0x011f84f3u);
    uint32_t d3d = T.eax;
    CHECK(d3d != 0);
    CHECK(d3d9metal_loaded());
    uint32_t pp = gscratch(56), out = gscratch(4), out2 = gscratch(4);
    uint32_t ppv[14] = {W, W, D3DFMT_A8R8G8B8, 1, 0, 0, 1 /* DISCARD */, 0x00010001, 1, 0, 0, 0, 0, 0x80000000u};
    for (int i = 0; i < 14; i++) gst(pp + 4 * i, ppv[i]);
    CHECK_EQ(gcall(d3d, S_D3D9_CreateDevice, 6, 0, D3DDEVTYPE_HAL, 0x00010001, 0x80 /* MIXED, as the game */, pp, out), D3D_OK);
    uint32_t dev = gld(out);
    CHECK(dev != 0);

    /* texture: 8x8 ARGB, quadrants written through the guest shadow */
    CHECK_EQ(gcall(dev, S_Device_CreateTexture, 8, 8, 8, 1, 0, D3DFMT_A8R8G8B8, D3DPOOL_MANAGED, out, 0), D3D_OK);
    uint32_t tex = gld(out), lr = gscratch(8), rect = gscratch(16);
    CHECK_EQ(gcall(tex, S_Texture_LockRect, 4, 0, lr, 0, 0), D3D_OK);
    uint32_t tp = gld(lr), tb = gld(lr + 4);
    CHECK_EQ(tp, 32);
    CHECK(tb >= 0x20000000u && tb < 0x30000000u); /* inside the guest window (the test heap) */
    for (int y = 0; y < 8; y++)
        for (int x = 0; x < 8; x++) gst(tb + y * tp + 4 * x, (y >= 4 && x >= 4) ? 0xff808080u : QC[(y >= 4) * 2 + (x >= 4)]);
    CHECK_EQ(gcall(tex, S_Texture_UnlockRect, 1, 0), D3D_OK);
    /* the bottom-right quadrant again, through a dirty rectangle: only that region is copied */
    gst(rect, 4); gst(rect + 4, 4); gst(rect + 8, 8); gst(rect + 12, 8);
    CHECK_EQ(gcall(tex, S_Texture_LockRect, 4, 0, lr, rect, 0), D3D_OK);
    uint32_t rb = gld(lr + 4);
    CHECK_EQ(rb, tb + 4 * tp + 16);
    for (int y = 0; y < 4; y++)
        for (int x = 0; x < 4; x++) gst(rb + y * tp + 4 * x, QC[3]);
    d3d9metal_stats s0;
    d3d9metal_get_stats(&s0);
    CHECK_EQ(gcall(tex, S_Texture_UnlockRect, 1, 0), D3D_OK);
    d3d9metal_stats s1;
    d3d9metal_get_stats(&s1);
    CHECK_EQ(s1.bytes_out - s0.bytes_out, 4 * 16); /* 4 rows of 16 bytes, not the whole level */

    /* render target texture, cleared */
    CHECK_EQ(gcall(dev, S_Device_CreateTexture, 8, W, W, 1, D3DUSAGE_RENDERTARGET, D3DFMT_A8R8G8B8, D3DPOOL_DEFAULT, out, 0), D3D_OK);
    uint32_t rtt = gld(out);
    CHECK_EQ(gcall(rtt, S_Texture_GetSurfaceLevel, 2, 0, out), D3D_OK);
    uint32_t rts = gld(out);
    CHECK_EQ(gcall(rtt, S_Texture_GetSurfaceLevel, 2, 0, out2), D3D_OK);
    CHECK_EQ(gld(out2), rts);                        /* identity: the same guest record twice */
    CHECK_EQ(gcall(rtt, 1, 0), 4);                   /* a level's references count on its texture (1 + 2 levels + this) */
    gcall(rtt, 2, 0);
    gcall(rts, 2, 0);
    CHECK_EQ(gcall(dev, S_Device_GetRenderTarget, 2, 0, out), D3D_OK);
    uint32_t bb = gld(out);                          /* the implicit back buffer */
    CHECK_EQ(gcall(dev, S_Device_GetBackBuffer, 4, 0, 0, 0, out2), D3D_OK);
    CHECK_EQ(gld(out2), bb);
    gcall(bb, 2, 0);
    gcall(bb, 2, 0);
    CHECK_EQ(gcall(dev, S_Device_SetRenderTarget, 2, 0, rts), D3D_OK);
    CHECK_EQ(gcall(dev, S_Device_GetRenderTarget, 2, 0, out), D3D_OK);
    CHECK_EQ(gld(out), rts);
    gcall(rts, 2, 0);

    /* SM3 shaders, declaration, state */
    uint32_t vsb = put_blob(k_vs_basic, sizeof k_vs_basic / 4), psb = put_blob(k_ps_tex, sizeof k_ps_tex / 4);
    CHECK_EQ(gcall(dev, S_Device_CreateVertexShader, 2, vsb, out), D3D_OK);
    uint32_t vs = gld(out);
    CHECK_EQ(gcall(dev, S_Device_CreatePixelShader, 2, psb, out), D3D_OK);
    uint32_t ps = gld(out);
    uint32_t decl = gscratch(32);
    gst(decl, 0x00000000u); gst(decl + 4, 0x00000003u);        /* stream 0 +0 FLOAT4 POSITION */
    gst(decl + 8, 0x00100000u); gst(decl + 12, 0x00050001u);   /* +16 FLOAT2 TEXCOORD */
    gst(decl + 16, 0x00180000u); gst(decl + 20, 0x000a0004u);  /* +24 D3DCOLOR COLOR */
    gst(decl + 24, 0x000000ffu); gst(decl + 28, 0x00000011u);  /* D3DDECL_END */
    CHECK_EQ(gcall(dev, S_Device_CreateVertexDeclaration, 2, decl, out), D3D_OK);
    uint32_t vd = gld(out);
    uint32_t c0 = gscratch(16);
    gst(c0, f2u(2.0f / W)); gst(c0 + 4, f2u(-2.0f / W)); gst(c0 + 8, f2u(-1.0f)); gst(c0 + 12, f2u(1.0f));

    /* dynamic vertex buffer: DISCARD lock into the guest shadow, copied out on Unlock */
    CHECK_EQ(gcall(dev, S_Device_CreateVertexBuffer, 6, 4 * 28, D3DUSAGE_DYNAMIC | 0x8 /* WRITEONLY */, 0, D3DPOOL_DEFAULT, out, 0), D3D_OK);
    uint32_t vb = gld(out), pb = gscratch(4);
    CHECK_EQ(gcall(vb, S_VertexBuffer_Lock, 4, 0, 0, pb, D3DLOCK_DISCARD), D3D_OK);
    put_quad(gld(pb), 0, 0, 192, 192);
    CHECK_EQ(gcall(vb, S_VertexBuffer_Unlock, 0), D3D_OK);
    uint32_t up = gscratch(4 * 28);
    put_quad(up, 200, 200, 240, 240);

    CHECK_EQ(gcall(dev, S_Device_BeginScene, 0), D3D_OK);
    CHECK_EQ(gcall(dev, S_Device_Clear, 6, 0, 0, 1 /* TARGET */, CLEAR, f2u(1.0f), 0), D3D_OK);
    CHECK_EQ(gcall(dev, S_Device_SetRenderState, 2, 7 /* ZENABLE */, 0), D3D_OK);
    CHECK_EQ(gcall(dev, S_Device_SetRenderState, 2, 22 /* CULLMODE */, 1 /* NONE */), D3D_OK);
    CHECK_EQ(gcall(dev, S_Device_SetVertexDeclaration, 1, vd), D3D_OK);
    CHECK_EQ(gcall(dev, S_Device_SetVertexShader, 1, vs), D3D_OK);
    CHECK_EQ(gcall(dev, S_Device_SetPixelShader, 1, ps), D3D_OK);
    CHECK_EQ(gcall(dev, S_Device_SetVertexShaderConstantF, 3, 0, c0, 1), D3D_OK);
    CHECK_EQ(gcall(dev, S_Device_SetTexture, 2, 0, tex), D3D_OK);
    CHECK_EQ(gcall(dev, S_Device_GetTexture, 2, 0, out), D3D_OK);
    CHECK_EQ(gld(out), tex);
    gcall(tex, 2, 0);
    CHECK_EQ(gcall(dev, S_Device_SetSamplerState, 3, 0, 5 /* MAGFILTER */, 1 /* POINT */), D3D_OK);
    CHECK_EQ(gcall(dev, S_Device_SetSamplerState, 3, 0, 6 /* MINFILTER */, 1), D3D_OK);
    CHECK_EQ(gcall(dev, S_Device_SetStreamSource, 4, 0, vb, 0, 28), D3D_OK);
    CHECK_EQ(gcall(dev, S_Device_DrawPrimitive, 3, 5 /* TRIANGLESTRIP */, 0, 2), D3D_OK);
    CHECK_EQ(gcall(dev, S_Device_DrawPrimitiveUP, 4, 5, 2, up, 28), D3D_OK);
    CHECK_EQ(gcall(dev, S_Device_EndScene, 0), D3D_OK);

    /* readback into guest memory */
    CHECK_EQ(gcall(dev, S_Device_CreateOffscreenPlainSurface, 6, W, W, D3DFMT_A8R8G8B8, D3DPOOL_SYSTEMMEM, out, 0), D3D_OK);
    uint32_t off = gld(out);
    CHECK_EQ(gcall(dev, S_Device_GetRenderTargetData, 2, rts, off), D3D_OK);
    CHECK_EQ(gcall(off, S_Surface_LockRect, 3, lr, 0, D3DLOCK_READONLY), D3D_OK);
    uint32_t pitch = gld(lr), bits = gld(lr + 4);
    CHECK_EQ(pitch, W * 4);
    CHECK(bits >= 0x20000000u && bits < 0x30000000u);
    int bad = 0;
    struct { int x, y; uint32_t want; } pts[] = {
        {20, 20, QC[0]},   {150, 20, QC[1]},  {20, 150, QC[2]},  {150, 150, QC[3]}, /* the VB quad, texture quadrants */
        {220, 100, CLEAR}, {100, 220, CLEAR}, {250, 5, CLEAR},                     /* outside: the clear colour */
        {205, 205, QC[0]}, {235, 205, QC[1]}, {205, 235, QC[2]}, {235, 235, QC[3]}, /* the DrawPrimitiveUP quad */
    };
    for (unsigned i = 0; i < sizeof pts / sizeof pts[0]; i++) {
        uint32_t got = px(bits, pitch, pts[i].x, pts[i].y);
        if (got != pts[i].want) {
            bad++;
            fprintf(stderr, "pixel (%d,%d) = 0x%08x, expected 0x%08x\n", pts[i].x, pts[i].y, got, pts[i].want);
        }
    }
    CHECK_EQ(bad, 0);
    /* a full-image tally: every pixel is one of the five colours */
    unsigned counts[6] = {0};
    for (int y = 0; y < W; y++)
        for (int x = 0; x < W; x++) {
            uint32_t v = px(bits, pitch, x, y);
            int k = 5;
            for (int q = 0; q < 4; q++) if (v == QC[q]) k = q;
            if (v == CLEAR) k = 4;
            counts[k]++;
        }
    fprintf(stderr, "readback tally: red %u green %u blue %u white %u clear %u other %u\n", counts[0], counts[1], counts[2], counts[3],
            counts[4], counts[5]);
    CHECK_EQ(counts[5], 0);
    CHECK_EQ(counts[0] + counts[1] + counts[2] + counts[3], 192 * 192 + 40 * 40);
    {
        char path[1024];
        snprintf(path, sizeof path, "%s/readback.ppm", outdir);
        FILE *f = fopen(path, "wb");
        if (f) {
            fprintf(f, "P6 %d %d 255\n", W, W);
            for (int y = 0; y < W; y++)
                for (int x = 0; x < W; x++) {
                    uint32_t v = px(bits, pitch, x, y);
                    uint8_t rgb[3] = {(uint8_t)(v >> 16), (uint8_t)(v >> 8), (uint8_t)v};
                    fwrite(rgb, 1, 3, f);
                }
            fclose(f);
        }
    }
    CHECK_EQ(gcall(off, S_Surface_UnlockRect, 0), D3D_OK);

    /* to the window: target -> back buffer, Present, and the back buffer read back */
    CHECK_EQ(gcall(dev, S_Device_GetBackBuffer, 4, 0, 0, 0, out), D3D_OK);
    bb = gld(out);
    CHECK_EQ(gcall(dev, S_Device_StretchRect, 5, rts, 0, bb, 0, 1 /* POINT */), D3D_OK);
    for (int i = 0; i < 30; i++) {
        CHECK_EQ(gcall(dev, S_Device_Present, 4, 0, 0, 0, 0), D3D_OK);
        test_pump();
    }
    CHECK_EQ(gcall(dev, S_Device_GetRenderTargetData, 2, bb, off), D3D_OK);
    CHECK_EQ(gcall(off, S_Surface_LockRect, 3, lr, 0, D3DLOCK_READONLY), D3D_OK);
    bits = gld(lr + 4);
    CHECK_EQ(px(bits, pitch, 20, 20), QC[0]);
    CHECK_EQ(px(bits, pitch, 235, 235), QC[3]);
    CHECK_EQ(px(bits, pitch, 220, 100), CLEAR);
    CHECK_EQ(gcall(off, S_Surface_UnlockRect, 0), D3D_OK);
    gcall(bb, 2, 0);
    {
        d3d9metal_wininfo wi;
        d3d9metal_window_info(d3d9metal_host_layer(0x00010001), &wi);
        fprintf(stderr, "window: %.0fx%.0f pt, scale %.1f, drawable %.0fx%.0f, content layer %s, %d subviews\n", wi.w_pt, wi.h_pt,
                wi.scale, wi.drawable_w, wi.drawable_h, wi.is_content_layer ? "CAMetalLayer" : "other", wi.subviews);
        CHECK(wi.is_content_layer);
        CHECK_EQ(wi.subviews, 0); /* we present into the content view's own CAMetalLayer, no subview */
    }
    /* TEST_READBACK_HOLD=N keeps the window up N seconds (to look at it) */
    if (getenv("TEST_READBACK_HOLD")) sleep((unsigned)atoi(getenv("TEST_READBACK_HOLD")));

    /* TEST_READBACK_UNBOUND (opt-in): a texture-sampling pixel shader drawn with sampler 0 unbound. D3D9 reads
     * (0,0,0,1) there. */
    if (getenv("TEST_READBACK_UNBOUND")) {
        CHECK_EQ(gcall(dev, S_Device_SetRenderTarget, 2, 0, rts), D3D_OK);
        CHECK_EQ(gcall(dev, S_Device_BeginScene, 0), D3D_OK);
        CHECK_EQ(gcall(dev, S_Device_Clear, 6, 0, 0, 1, CLEAR, f2u(1.0f), 0), D3D_OK);
        /* TEST_READBACK_UNBOUND=bound is the control: the same draw with the texture bound */
        CHECK_EQ(gcall(dev, S_Device_SetTexture, 2, 0, strcmp(getenv("TEST_READBACK_UNBOUND"), "bound") ? 0 : tex), D3D_OK);
        /* DrawPrimitiveUP left stream 0 unset (D3D9 semantics); bind it again for this draw */
        CHECK_EQ(gcall(dev, S_Device_SetStreamSource, 4, 0, vb, 0, 28), D3D_OK);
        CHECK_EQ(gcall(dev, S_Device_DrawPrimitive, 3, 5, 0, 2), D3D_OK);
        CHECK_EQ(gcall(dev, S_Device_EndScene, 0), D3D_OK);
        for (int i = 0; i < 3; i++) CHECK_EQ(gcall(dev, S_Device_Present, 4, 0, 0, 0, 0), D3D_OK);
        fprintf(stderr, "unbound: drew; TestCooperativeLevel = 0x%08x\n", gcall(dev, S_Device_TestCooperativeLevel, 0));
        CHECK_EQ(gcall(dev, S_Device_GetRenderTargetData, 2, rts, off), D3D_OK);
        CHECK_EQ(gcall(off, S_Surface_LockRect, 3, lr, 0, D3DLOCK_READONLY), D3D_OK);
        bits = gld(lr + 4);
        fprintf(stderr, "unbound: pixel (20,20) = 0x%08x (D3D9: 0xff000000 for (0,0,0,1)), outside 0x%08x\n",
                px(bits, pitch, 20, 20), px(bits, pitch, 220, 100));
        CHECK_EQ(gcall(off, S_Surface_UnlockRect, 0), D3D_OK);
        CHECK_EQ(gcall(dev, S_Device_SetRenderTarget, 2, 0, bb), D3D_OK);
    }
    /* TEST_READBACK_SAMPLERS=N (opt-in): N distinct sampler states, one draw each (the sampler cache under many states). Then the device
     * state and a readback. */
    if (getenv("TEST_READBACK_SAMPLERS")) {
        int n = atoi(getenv("TEST_READBACK_SAMPLERS")), drawn = 0;
        CHECK_EQ(gcall(dev, S_Device_SetRenderTarget, 2, 0, rts), D3D_OK);
        CHECK_EQ(gcall(dev, S_Device_SetTexture, 2, 0, tex), D3D_OK);
        CHECK_EQ(gcall(dev, S_Device_SetStreamSource, 4, 0, vb, 0, 28), D3D_OK);
        for (int i = 0; i < n; i++) {
            if (i % 100 == 0) CHECK_EQ(gcall(dev, S_Device_BeginScene, 0), D3D_OK);
            gcall(dev, S_Device_SetSamplerState, 3, 0, 1 /* ADDRESSU */, 1 + i % 5);
            gcall(dev, S_Device_SetSamplerState, 3, 0, 2 /* ADDRESSV */, 1 + (i / 5) % 5);
            gcall(dev, S_Device_SetSamplerState, 3, 0, 3 /* ADDRESSW */, 1 + (i / 25) % 5);
            gcall(dev, S_Device_SetSamplerState, 3, 0, 9 /* MAXMIPLEVEL */, (i / 125) % 16);
            gcall(dev, S_Device_SetSamplerState, 3, 0, 10 /* MAXANISOTROPY */, 1 + (i / 2000) % 16);
            if (gcall(dev, S_Device_DrawPrimitive, 3, 5, 0, 2) == D3D_OK) drawn++;
            if (i % 100 == 99 || i == n - 1) {
                CHECK_EQ(gcall(dev, S_Device_EndScene, 0), D3D_OK);
                gcall(dev, S_Device_Present, 4, 0, 0, 0, 0);
            }
        }
        fprintf(stderr, "samplers: %d draws with distinct sampler states; TestCooperativeLevel = 0x%08x\n", drawn,
                gcall(dev, S_Device_TestCooperativeLevel, 0));
        CHECK_EQ(gcall(dev, S_Device_GetRenderTargetData, 2, rts, off), D3D_OK);
        CHECK_EQ(gcall(off, S_Surface_LockRect, 3, lr, 0, D3DLOCK_READONLY), D3D_OK);
        fprintf(stderr, "samplers: readback done, pixel (20,20) = 0x%08x\n", px(gld(lr + 4), pitch, 20, 20));
        CHECK_EQ(gcall(off, S_Surface_UnlockRect, 0), D3D_OK);
        CHECK_EQ(gcall(dev, S_Device_SetRenderTarget, 2, 0, bb), D3D_OK);
    }

    /* release everything; the guest-visible objects all go */
    unsigned live_before_release = sg_com_live_objects();
    CHECK_EQ(gcall(off, 2, 0), 0);
    CHECK_EQ(gcall(vb, 2, 0), 0);
    CHECK_EQ(gcall(vd, 2, 0), 0);
    CHECK_EQ(gcall(vs, 2, 0), 0);
    CHECK_EQ(gcall(ps, 2, 0), 0);
    CHECK_EQ(gcall(dev, S_Device_SetTexture, 2, 0, 0), D3D_OK);
    CHECK_EQ(gcall(tex, 2, 0), 0);
    CHECK_EQ(gcall(dev, S_Device_SetRenderTarget, 2, 0, bb), D3D_OK);
    CHECK_EQ(gcall(rts, 2, 0), 1);                   /* the first GetSurfaceLevel reference: the texture's count */
    CHECK_EQ(gcall(rtt, 2, 0), 0);
    CHECK_EQ(gcall(dev, 2, 0), 0);
    CHECK_EQ(gcall(d3d, 2, 0), 0);
    d3d9metal_get_stats(&s1);
    fprintf(stderr, "d3d9metal: %llu locks, %llu bytes out, %llu bytes in, %llu frames, %u live COM objects before the releases, %u after\n",
            (unsigned long long)s1.locks, (unsigned long long)s1.bytes_out, (unsigned long long)s1.bytes_in,
            (unsigned long long)s1.frames, live_before_release, sg_com_live_objects());
    test_window_close();
    return harness_report("runtime/d3d9metal readback");
}
