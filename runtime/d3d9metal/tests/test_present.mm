/* test_present.mm -- the window, present and resource test for the Metal back end, through the guest
 * vtables (the way translated code calls them) plus a few looks inside (d3d9metal_int.h) where the guest
 * cannot see the effect (buffer renaming, the INDEX16 slack, generated mipmaps):
 *   - a device on the test window; the window's content CAMetalLayer is presented into, no subview;
 *   - 120 frames of full-target Clear + Present: Present returns promptly, every frame gets a drawable,
 *     present-to-present intervals (vsync) reported;
 *   - GetRenderTargetData of the back buffer after Clear; ColorFill of a rectangle;
 *   - StretchRect: same size (blit), scaled (linear), into SYSTEMMEM (refused, DEVIATIONS 3.10);
 *   - texture formats through LockRect -> StretchRect into an A8R8G8B8 target -> readback: A8R8G8B8,
 *     X8R8G8B8, R5G6B5, A1R5G5B5, A4R4G4B4, L8, A8L8, A8, DXT1, DXT5 (sampling swizzles and conversions);
 *   - vertex buffer DISCARD while in flight (renamed, the GPU's copy intact), NOOVERWRITE (in place), the
 *     INDEX16 lock slack; AUTOGENMIPMAP; EVENT and OCCLUSION queries; Reset to a new size; release all.
 * Usage: test_present <outdir> */
#import <Metal/Metal.h>
#include <math.h>
#include <stdlib.h>
#include <unistd.h>

extern "C" {
#include "com_harness.h"
#include "../../d3d9null/src/d3d9_int.h"
#include "d3d9metal_int.h"
#include "d3d9metal.h"
void hle_stop_check(sg_ctx *ctx, uint8_t *mem, const char *import) { (void)ctx; (void)mem; (void)import; }
void sg_imp_d3d9_Direct3DCreate9(sg_ctx *ctx, uint8_t *mem);
void test_app_init(void);
void test_window_size(int w, int h);
void test_window_close(void);
void test_window_none(int none);
void test_pump(void);
int mt_readback_all(mt_dev *d, mt_obj *t); /* device-layer internal (src/mt_priv.h) */
int mt_util_draw(mt_dev *d, int kind, mt_obj *src, const uint32_t srect[4], id<MTLTexture> dst, uint32_t dslice, uint32_t dlevel,
                 uint32_t dfmt, const uint32_t drect[4], uint32_t dw, uint32_t dh, int linear, const float rgba[4]);
enum { MT_UTIL_COPY_OPAQUE_ = 2, MT_UTIL_PRESENT_ = 3 }; /* src/mt_priv.h */
int mt_present_dest(void *layer, uint32_t dw, uint32_t dh, uint32_t out[4]);  /* src/mt_present.mm */
int mt_present_blit(mt_dev *d, mt_obj *bb, void *layer, id<MTLTexture> dt);
}

static uint32_t f2u(float f) { uint32_t u; memcpy(&u, &f, 4); return u; }
static uint64_t now_ns(void) {
    struct timespec ts;
    clock_gettime(CLOCK_MONOTONIC, &ts);
    return (uint64_t)ts.tv_sec * 1000000000ull + (uint64_t)ts.tv_nsec;
}
static int cmp_u64(const void *a, const void *b) {
    uint64_t x = *(const uint64_t *)a, y = *(const uint64_t *)b;
    return x < y ? -1 : x > y;
}
static uint32_t g_dev, g_off, g_lr;
/* Reads pixel (x, y) of an A8R8G8B8 render-target surface through GetRenderTargetData. */
static uint32_t rt_pixel(uint32_t rts, uint32_t off, int x, int y) {
    CHECK_EQ(gcall(g_dev, S_Device_GetRenderTargetData, 2, rts, off), D3D_OK);
    CHECK_EQ(gcall(off, S_Surface_LockRect, 3, g_lr, 0, D3DLOCK_READONLY), D3D_OK);
    uint32_t v = gld(gld(g_lr + 4) + (uint32_t)y * gld(g_lr) + 4u * (uint32_t)x);
    gcall(off, S_Surface_UnlockRect, 0);
    return v;
}
static int near_argb(uint32_t a, uint32_t b, int tol) {
    for (int s = 0; s < 32; s += 8) {
        int d = (int)((a >> s) & 255) - (int)((b >> s) & 255);
        if (d > tol || d < -tol) return 0;
    }
    return 1;
}

int main(int argc, char **argv) {
    const char *outdir = argc > 1 ? argv[1] : ".";
    sg_ctx *ctx = &T;
    uint8_t *mem;
    (void)outdir;
    harness_init();
    mem = M;
    test_app_init();
    test_window_size(320, 240);
    if (!getenv("SPORE_D3D9")) setenv("SPORE_D3D9", "metal", 1);
    setenv("SPORE_D3D9_FPS", "60", 1);
    unsigned live0 = sg_com_live_objects();

    SG_PUSH32(32);
    SG_CALL(sg_imp_d3d9_Direct3DCreate9, 0x011f84f3u);
    uint32_t d3d = T.eax;
    CHECK(d3d != 0);
    CHECK(d3d9metal_loaded());
    uint32_t pp = gscratch(56), out = gscratch(4), out2 = gscratch(4);
    uint32_t ppv[14] = {320, 240, D3DFMT_A8R8G8B8, 1, 0, 0, 3 /* COPY */, 0x00010001, 1, 1, D3DFMT_D24S8, 0, 0, 0x80000000u};
    for (int i = 0; i < 14; i++) gst(pp + 4 * i, ppv[i]);
    T.fpu.cw = 0x027f;
    CHECK_EQ(gcall(d3d, S_D3D9_CreateDevice, 6, 0, D3DDEVTYPE_HAL, 0x00010001, 0x80, pp, out), D3D_OK);
    CHECK_EQ(T.fpu.cw, 0x007f); /* the FPU rule (DEVIATIONS 1.5) */
    uint32_t dev = g_dev = gld(out);
    CHECK(dev != 0);
    mt_dev *md = (mt_dev *)mt_from_guest(M, dev, MT_K_DEV);
    CHECK(md != NULL);
    g_lr = gscratch(8);
    uint32_t lr = g_lr, rect = gscratch(16);

    /* ---- frames: Clear + Present ---- */
    CHECK_EQ(gcall(dev, S_Device_GetBackBuffer, 4, 0, 0, 0, out), D3D_OK);
    uint32_t bb = gld(out);
    CHECK_EQ(gcall(dev, S_Device_CreateOffscreenPlainSurface, 6, 320, 240, D3DFMT_A8R8G8B8, D3DPOOL_SYSTEMMEM, out, 0), D3D_OK);
    uint32_t off = g_off = gld(out);
    d3d9metal_stats s0, s1;
    /* before the game's first ShowWindow there is no layer: Present counts the frame and commits */
    d3d9metal_get_stats(&s0);
    test_window_none(1);
    CHECK_EQ(gcall(dev, S_Device_Clear, 6, 0, 0, 7, 0xff000000u, f2u(1.0f), 0), D3D_OK);
    CHECK_EQ(gcall(dev, S_Device_Present, 4, 0, 0, 0, 0), D3D_OK);
    test_window_none(0);
    d3d9metal_get_stats(&s1);
    CHECK_EQ(s1.presents_skipped - s0.presents_skipped, 1);
    CHECK_EQ(s1.frames - s0.frames, 1);
    d3d9metal_get_stats(&s0);
    uint64_t dt[120], tmax = 0, t_prev = 0, tfirst = 0;
    int nd = 0;
    for (int f = 0; f < 120; f++) {
        uint32_t col = 0xff000000u | (uint32_t)(f * 2) << 16 | 0x4080u;
        CHECK_EQ(gcall(dev, S_Device_BeginScene, 0), D3D_OK);
        CHECK_EQ(gcall(dev, S_Device_Clear, 6, 0, 0, 7, col, f2u(1.0f), 0), D3D_OK);
        CHECK_EQ(gcall(dev, S_Device_EndScene, 0), D3D_OK);
        uint64_t t0 = now_ns();
        CHECK_EQ(gcall(dev, S_Device_Present, 4, 0, 0, 0, 0), D3D_OK);
        uint64_t t1 = now_ns();
        if (!f) tfirst = t1 - t0; /* the first Present compiles the device layer's present shader and pipeline */
        else if (t1 - t0 > tmax) tmax = t1 - t0;
        if (t_prev && nd < 120) dt[nd++] = t1 - t_prev;
        t_prev = t1;
        test_pump();
    }
    d3d9metal_get_stats(&s1);
    qsort(dt, (size_t)nd, sizeof dt[0], cmp_u64);
    fprintf(stderr, "present: 120 frames, present-to-present median %.2f ms (p10 %.2f, p90 %.2f), longest Present call %.2f ms (the first, with the shader compile, %.2f ms), "
                    "drawables nil %llu, skipped %llu\n",
            dt[nd / 2] / 1e6, dt[nd / 10] / 1e6, dt[nd * 9 / 10] / 1e6, tmax / 1e6, tfirst / 1e6, (unsigned long long)(s1.drawables_nil - s0.drawables_nil),
            (unsigned long long)(s1.presents_skipped - s0.presents_skipped));
    CHECK_EQ(s1.frames - s0.frames, 120); /* (131 in all with the windowless one and the 10 after Reset) */
    CHECK_EQ(s1.drawables_nil - s0.drawables_nil, 0);
    CHECK_EQ(s1.presents_skipped - s0.presents_skipped, 0);
    CHECK(tmax < 50000000ull);               /* Present returns promptly: within about one vsync period plus slack */
    CHECK(tfirst < 2000000000ull);
    CHECK(dt[nd / 2] > 4000000ull);          /* vsync paces (<= 240 Hz) */
    CHECK(dt[nd / 2] < 40000000ull);
    CHECK_EQ(rt_pixel(bb, off, 10, 10), 0xff000000u | (119u * 2) << 16 | 0x4080u);
    {
        d3d9metal_wininfo wi;
        d3d9metal_window_info(d3d9metal_host_layer(0x00010001), &wi);
        fprintf(stderr, "window: %.0fx%.0f pt, scale %.1f, drawable %.0fx%.0f, content layer %d, %d subviews\n", wi.w_pt, wi.h_pt, wi.scale,
                wi.drawable_w, wi.drawable_h, wi.is_content_layer, wi.subviews);
        CHECK(wi.is_content_layer);
        CHECK_EQ(wi.subviews, 0);
        CHECK_EQ((int)wi.drawable_w, (int)(320 * wi.scale)); /* Retina drawable; the 320x240 back buffer is scaled to it (3.11) */
    }

    /* ---- ColorFill ---- */
    gst(rect, 100); gst(rect + 4, 50); gst(rect + 8, 140); gst(rect + 12, 90);
    CHECK_EQ(gcall(dev, S_Device_ColorFill, 3, bb, rect, 0xff12ab34u), D3D_OK);
    CHECK_EQ(rt_pixel(bb, off, 120, 70), 0xff12ab34u);
    CHECK_EQ(rt_pixel(bb, off, 99, 70), 0xff000000u | (119u * 2) << 16 | 0x4080u);
    CHECK_EQ(gcall(dev, S_Device_ColorFill, 3, off, 0, 0xffffffffu), D3DERR_INVALIDCALL); /* SYSTEMMEM: D3D9 refuses */

    /* ---- StretchRect ---- */
    CHECK_EQ(gcall(dev, S_Device_CreateRenderTarget, 8, 64, 64, D3DFMT_A8R8G8B8, 0, 0, 0, out, 0), D3D_OK);
    uint32_t small = gld(out);
    CHECK_EQ(gcall(dev, S_Device_ColorFill, 3, small, 0, 0xff0000ffu), D3D_OK);
    gst(rect, 32); gst(rect + 4, 0); gst(rect + 8, 64); gst(rect + 12, 64);
    CHECK_EQ(gcall(dev, S_Device_ColorFill, 3, small, rect, 0xffff0000u), D3D_OK);
    uint32_t drect = gscratch(16);
    gst(drect, 0); gst(drect + 4, 0); gst(drect + 8, 64); gst(drect + 12, 64);
    CHECK_EQ(gcall(dev, S_Device_StretchRect, 5, small, 0, bb, drect, 1 /* POINT */), D3D_OK); /* same size: a blit */
    CHECK_EQ(rt_pixel(bb, off, 10, 10), 0xff0000ffu);
    CHECK_EQ(rt_pixel(bb, off, 50, 10), 0xffff0000u);
    gst(drect, 0); gst(drect + 4, 100); gst(drect + 8, 128); gst(drect + 12, 228);
    CHECK_EQ(gcall(dev, S_Device_StretchRect, 5, small, 0, bb, drect, 2 /* LINEAR */), D3D_OK); /* scaled: a draw */
    CHECK_EQ(rt_pixel(bb, off, 10, 150), 0xff0000ffu);
    CHECK_EQ(rt_pixel(bb, off, 120, 150), 0xffff0000u);
    {
        uint32_t mid = rt_pixel(bb, off, 64, 150); /* the filtered edge between the halves */
        CHECK(((mid >> 16) & 255) > 0x20 && (mid & 255) > 0x20);
    }
    CHECK_EQ(gcall(dev, S_Device_StretchRect, 5, small, 0, off, 0, 1), D3DERR_INVALIDCALL); /* into SYSTEMMEM (3.10) */
    /* ---- Present's scaling filter (util_present, area-weighted "sharp bilinear"). The back buffer now holds
     * sharp blue/red edges (the StretchRects above). At the 2x Retina scale every drawable pixel must equal its
     * back-buffer texel (pixel replication); at 1.5x the pixels inside one texel are exact and the
     * others lie between their two texels. Negative control: a plain bilinear copy is not exact at 2x. ---- */
    {
        mt_obj *bbo = mt_from_guest(M, bb, -1);
        CHECK(bbo != NULL);
        static uint32_t ref[320 * 240];
        CHECK_EQ(gcall(dev, S_Device_GetRenderTargetData, 2, bb, off), D3D_OK);
        CHECK_EQ(gcall(off, S_Surface_LockRect, 3, lr, 0, D3DLOCK_READONLY), D3D_OK);
        for (int y = 0; y < 240; y++)
            for (int x = 0; x < 320; x++) ref[y * 320 + x] = gld(gld(lr + 4) + (uint32_t)y * gld(lr) + 4u * (uint32_t)x) | 0xff000000u;
        gcall(off, S_Surface_UnlockRect, 0);
        const int kinds[2] = {MT_UTIL_PRESENT_, MT_UTIL_COPY_OPAQUE_};
        for (int k = 0; k < 2; k++)
            for (int sc = 0; sc < 2; sc++) {
                int dw = sc ? 480 : 640, dh = sc ? 360 : 480;
                MTLTextureDescriptor *td = [MTLTextureDescriptor texture2DDescriptorWithPixelFormat:MTLPixelFormatBGRA8Unorm width:dw height:dh mipmapped:NO];
                td.usage = MTLTextureUsageRenderTarget | MTLTextureUsageShaderRead;
                td.storageMode = MTLStorageModeShared;
                id<MTLTexture> dt = [md->mtl newTextureWithDescriptor:td];
                uint32_t sr[4] = {0, 0, 320, 240}, dr[4] = {0, 0, (uint32_t)dw, (uint32_t)dh};
                CHECK_EQ(mt_util_draw(md, kinds[k], bbo, sr, dt, 0, 0, MTLPixelFormatBGRA8Unorm, dr, dw, dh, 1, NULL), 0);
                mt_flush(md, 1);
                uint32_t *px = (uint32_t *)malloc((size_t)dw * dh * 4);
                [dt getBytes:px bytesPerRow:dw * 4 fromRegion:MTLRegionMake2D(0, 0, dw, dh) mipmapLevel:0];
                int exact_bad = 0, between_bad = 0, inside = 0;
                for (int y = 0; y < dh; y++)
                    for (int x = 0; x < dw; x++) {
                        double cx = (x + 0.5) * 320.0 / dw, cy = (y + 0.5) * 240.0 / dh, hx = 160.0 / dw, hy = 120.0 / dh;
                        int x0 = (int)floor(cx - hx), x1 = (int)floor(cx + hx - 1e-9), y0 = (int)floor(cy - hy), y1 = (int)floor(cy + hy - 1e-9);
                        uint32_t got = px[y * dw + x];
                        if (x0 == x1 && y0 == y1) {
                            inside++;
                            if (got != ref[y0 * 320 + x0]) exact_bad++;
                        } else {
                            uint32_t a = ref[y0 * 320 + x0], b = ref[(y1 > 239 ? 239 : y1) * 320 + (x1 > 319 ? 319 : x1)];
                            for (int c = 0; c < 24; c += 8) {
                                int g = (got >> c) & 255, lo = (int)((a >> c) & 255), hi = (int)((b >> c) & 255);
                                if (lo > hi) { int t = lo; lo = hi; hi = t; }
                                if (g < lo - 1 || g > hi + 1) { between_bad++; break; }
                            }
                        }
                    }
                fprintf(stderr, "present scale %s %dx%d: %d pixels inside one texel, %d not exact; %d edge pixels outside their texels\n",
                        k ? "bilinear (old)" : "util_present", dw, dh, inside, exact_bad, between_bad);
                if (k == 0) {
                    if (!sc) CHECK_EQ(inside, dw * dh);       /* 2x: every pixel inside one texel */
                    CHECK_EQ(exact_bad, 0);
                    CHECK_EQ(between_bad, 0);
                } else if (!sc) {
                    CHECK(exact_bad > 0);                     /* the control: plain bilinear blurs the 2x edges */
                }
                free(px);
                [dt release];
            }

        /* ---- the three present cases (docs/design/platform-macos.md section 11) ----
         * (a) 1:1 (Retina-native, the back buffer the drawable's size): an exact copy. This catches a shader that takes
         *     fract(coord) outside the area-weighted branch and so blends every texel half with its neighbour;
         * (b) the letterbox rectangle from the layer's "sporeContentRect" (absent: the whole drawable);
         * (c) letterboxed: bars black, the frame inside its rectangle, a 2x integer letterbox exact;
         * (d) scaled to the whole drawable when the layer has no rectangle (the scaling-filter checks above are this path). */
        {
            id<MTLTexture> (^mk)(int, int) = ^id<MTLTexture>(int w, int h) {
                MTLTextureDescriptor *td = [MTLTextureDescriptor texture2DDescriptorWithPixelFormat:MTLPixelFormatBGRA8Unorm width:w height:h mipmapped:NO];
                td.usage = MTLTextureUsageRenderTarget | MTLTextureUsageShaderRead;
                td.storageMode = MTLStorageModeShared;
                return [md->mtl newTextureWithDescriptor:td];
            };
            CALayer *plain = [CALayer layer], *boxl = [CALayer layer];
            /* (a) */
            id<MTLTexture> t1 = mk(320, 240);
            CHECK_EQ(mt_present_blit(md, bbo, plain, t1), 0);
            mt_flush(md, 1);
            uint32_t *px = (uint32_t *)malloc(640 * 480 * 4);
            [t1 getBytes:px bytesPerRow:320 * 4 fromRegion:MTLRegionMake2D(0, 0, 320, 240) mipmapLevel:0];
            int bad = 0;
            for (int i = 0; i < 320 * 240; i++) bad += px[i] != ref[i];
            fprintf(stderr, "present 1:1 (320x240 into 320x240): %d of %d pixels differ from the back buffer\n", bad, 320 * 240);
            CHECK_EQ(bad, 0);
            [t1 release];
            /* (b) */
            uint32_t r4[4];
            CHECK_EQ(mt_present_dest(plain, 1000, 500, r4), 0);
            CHECK(r4[0] == 0 && r4[1] == 0 && r4[2] == 1000 && r4[3] == 500);
            CHECK_EQ(mt_present_dest(NULL, 1000, 500, r4), 0);
            [boxl setValue:[NSValue valueWithRect:NSMakeRect(0.125, 0, 0.75, 1)] forKey:@"sporeContentRect"];
            CHECK_EQ(mt_present_dest(boxl, 1000, 500, r4), 1);
            CHECK(r4[0] == 125 && r4[1] == 0 && r4[2] == 875 && r4[3] == 500);
            [boxl setValue:[NSValue valueWithRect:NSMakeRect(0, 0, 1, 1)] forKey:@"sporeContentRect"];
            CHECK_EQ(mt_present_dest(boxl, 1000, 500, r4), 0);   /* the whole drawable: no bars */
            [boxl setValue:[NSValue valueWithRect:NSMakeRect(0, 0, 0, 0)] forKey:@"sporeContentRect"];
            CHECK_EQ(mt_present_dest(boxl, 1000, 500, r4), 0);   /* degenerate: ignored */
            /* (c) a 4:3 320x240 frame letterboxed 2x into an 840x480 drawable: 100-pixel bars left and right, the
             * inside pixel-replicated (as the platform layer computes the rectangle for a 4:3 client on a wider display) */
            id<MTLTexture> t2 = mk(840, 480);
            [boxl setValue:[NSValue valueWithRect:NSMakeRect(100.0 / 840, 0, 640.0 / 840, 1)] forKey:@"sporeContentRect"];
            CHECK_EQ(mt_present_blit(md, bbo, boxl, t2), 1);
            mt_flush(md, 1);
            uint32_t *p2 = (uint32_t *)malloc(840 * 480 * 4);
            [t2 getBytes:p2 bytesPerRow:840 * 4 fromRegion:MTLRegionMake2D(0, 0, 840, 480) mipmapLevel:0];
            int bar_bad = 0, in_bad = 0;
            for (int y = 0; y < 480; y++)
                for (int x = 0; x < 840; x++) {
                    uint32_t g = p2[y * 840 + x];
                    if (x < 100 || x >= 740) bar_bad += g != 0xff000000u;
                    else in_bad += g != ref[(y / 2) * 320 + (x - 100) / 2];
                }
            fprintf(stderr, "present letterboxed (320x240 into 640x480 at 100,0 of 840x480): %d bar pixels not black, %d inside pixels "
                            "not their texel\n", bar_bad, in_bad);
            CHECK_EQ(bar_bad, 0);
            CHECK_EQ(in_bad, 0);
            /* and a non-integer letterbox (1.6x, 512x384 at 164,48 of 840x480): bars black, every inside pixel between
             * the texels it covers (the sharp-bilinear rule of the scaling-filter checks) */
            [boxl setValue:[NSValue valueWithRect:NSMakeRect(164.0 / 840, 48.0 / 480, 512.0 / 840, 384.0 / 480)] forKey:@"sporeContentRect"];
            CHECK_EQ(mt_present_blit(md, bbo, boxl, t2), 1);
            mt_flush(md, 1);
            [t2 getBytes:p2 bytesPerRow:840 * 4 fromRegion:MTLRegionMake2D(0, 0, 840, 480) mipmapLevel:0];
            bar_bad = 0, in_bad = 0;
            for (int y = 0; y < 480; y++)
                for (int x = 0; x < 840; x++) {
                    uint32_t g = p2[y * 840 + x];
                    if (x < 164 || x >= 676 || y < 48 || y >= 432) { bar_bad += g != 0xff000000u; continue; }
                    double cx = (x - 164 + 0.5) / 1.6, cy = (y - 48 + 0.5) / 1.6, h = 0.5 / 1.6;
                    int x0 = (int)floor(cx - h), x1 = (int)floor(cx + h - 1e-9), y0 = (int)floor(cy - h), y1 = (int)floor(cy + h - 1e-9);
                    if (x1 > 319) x1 = 319;
                    if (y1 > 239) y1 = 239;
                    uint32_t a = ref[y0 * 320 + x0], b = ref[y1 * 320 + x1];
                    uint32_t q[4] = {a, ref[y0 * 320 + x1], ref[y1 * 320 + x0], b}; /* the (up to) four texels it covers */
                    for (int c = 0; c < 24; c += 8) {
                        int gv = (g >> c) & 255, lo = 255, hi = 0;
                        for (int j = 0; j < 4; j++) {
                            int v = (int)((q[j] >> c) & 255);
                            lo = v < lo ? v : lo, hi = v > hi ? v : hi;
                        }
                        if (gv < lo - 1 || gv > hi + 1) {
                            if (in_bad < 4) fprintf(stderr, "  1.6x pixel (%d,%d): 0x%08x, texels (%d,%d) 0x%08x (%d,%d) 0x%08x\n", x, y, g, x0, y0, a, x1, y1, b);
                            in_bad++; break;
                        }
                    }
                }
            fprintf(stderr, "present letterboxed 1.6x: %d bar pixels not black, %d inside pixels outside their texels\n", bar_bad, in_bad);
            CHECK_EQ(bar_bad, 0);
            CHECK_EQ(in_bad, 0);
            free(p2);
            free(px);
            [t2 release];
        }
    }

    CHECK_EQ(gcall(small, S_Surface_LockRect, 3, lr, 0, 0), D3DERR_INVALIDCALL);           /* not lockable (3.10) */

    /* ---- formats: LockRect -> StretchRect (sampling) -> readback ---- */
    struct fmt_case { uint32_t fmt; uint32_t texel[2]; unsigned bytes; uint32_t want; int tol; const char *name; } cases[] = {
        {D3DFMT_A8R8G8B8, {0x80402010u}, 4, 0x80402010u, 0, "A8R8G8B8"},
        {D3DFMT_X8R8G8B8, {0x00402010u}, 4, 0xff402010u, 0, "X8R8G8B8 (alpha reads 1)"},
        {D3DFMT_R5G6B5, {0xf800u}, 2, 0xffff0000u, 0, "R5G6B5 red"},
        {D3DFMT_R5G6B5, {0x07e0u}, 2, 0xff00ff00u, 0, "R5G6B5 green"},
        {D3DFMT_A1R5G5B5, {0x801fu}, 2, 0xff0000ffu, 0, "A1R5G5B5 blue"},
        {D3DFMT_A4R4G4B4, {0x8f40u}, 2, 0x88ff4400u, 0, "A4R4G4B4 (converted)"},
        {D3DFMT_L8, {0x60u}, 1, 0xff606060u, 0, "L8 (rrr1)"},
        {D3DFMT_A8L8, {0x3060u}, 2, 0x30606060u, 0, "A8L8 (rrrg)"},
        {D3DFMT_A8, {0x70u}, 1, 0x70000000u, 0, "A8 (000a)"},
        {D3DFMT_DXT1, {0xf800f800u, 0u}, 8, 0xffff0000u, 0, "DXT1 solid red"},
    };
    CHECK_EQ(gcall(dev, S_Device_CreateRenderTarget, 8, 8, 8, D3DFMT_A8R8G8B8, 0, 0, 0, out, 0), D3D_OK);
    uint32_t rt8 = gld(out);
    CHECK_EQ(gcall(dev, S_Device_CreateOffscreenPlainSurface, 6, 8, 8, D3DFMT_A8R8G8B8, D3DPOOL_SYSTEMMEM, out, 0), D3D_OK);
    uint32_t off8 = gld(out);
    for (unsigned k = 0; k < sizeof cases / sizeof cases[0]; k++) {
        uint32_t tx;
        CHECK_EQ(gcall(dev, S_Device_CreateTexture, 8, 8, 8, 1, D3DUSAGE_DYNAMIC, cases[k].fmt, D3DPOOL_DEFAULT, out, 0), D3D_OK);
        tx = gld(out);
        CHECK_EQ(gcall(tx, S_Texture_LockRect, 4, 0, lr, 0, D3DLOCK_DISCARD), D3D_OK);
        uint32_t pitch = gld(lr), bits = gld(lr + 4);
        int blocks = cases[k].fmt == D3DFMT_DXT1;
        for (int y = 0; y < (blocks ? 2 : 8); y++)
            for (int x = 0; x < (blocks ? 2 : 8); x++) {
                uint32_t a = bits + (uint32_t)y * pitch + (uint32_t)x * cases[k].bytes;
                if (cases[k].bytes == 8) gst(a, cases[k].texel[0]), gst(a + 4, cases[k].texel[1]);
                else if (cases[k].bytes == 4) gst(a, cases[k].texel[0]);
                else if (cases[k].bytes == 2) M[a] = (uint8_t)cases[k].texel[0], M[a + 1] = (uint8_t)(cases[k].texel[0] >> 8);
                else M[a] = (uint8_t)cases[k].texel[0];
            }
        CHECK_EQ(gcall(tx, S_Texture_UnlockRect, 1, 0), D3D_OK);
        CHECK_EQ(gcall(tx, S_Texture_GetSurfaceLevel, 2, 0, out), D3D_OK);
        uint32_t lvl = gld(out);
        CHECK_EQ(gcall(dev, S_Device_ColorFill, 3, rt8, 0, 0xdeadbeefu), D3D_OK);
        CHECK_EQ(gcall(dev, S_Device_StretchRect, 5, lvl, 0, rt8, 0, 1), D3D_OK);
        CHECK_EQ(gcall(dev, S_Device_GetRenderTargetData, 2, rt8, off8), D3D_OK);
        CHECK_EQ(gcall(off8, S_Surface_LockRect, 3, lr, 0, D3DLOCK_READONLY), D3D_OK);
        uint32_t got = gld(gld(lr + 4) + 3 * gld(lr) + 12);
        gcall(off8, S_Surface_UnlockRect, 0);
        if (!near_argb(got, cases[k].want, cases[k].tol)) fprintf(stderr, "format %s: got 0x%08x, expected 0x%08x\n", cases[k].name, got, cases[k].want);
        CHECK(near_argb(got, cases[k].want, cases[k].tol));
        gcall(lvl, 2, 0);
        CHECK_EQ(gcall(tx, 2, 0), 0);
    }

    /* ---- every format of the CheckDeviceFormat table makes a Metal texture; depth textures (the game's raster
     * type 10), cube render targets ---- */
    {
        static const uint32_t fmts[] = {D3DFMT_A8R8G8B8, D3DFMT_X8R8G8B8, D3DFMT_R5G6B5, D3DFMT_X1R5G5B5, D3DFMT_A1R5G5B5,
                                        D3DFMT_A4R4G4B4, D3DFMT_A8, D3DFMT_X4R4G4B4, D3DFMT_A2B10G10R10, D3DFMT_A8B8G8R8,
                                        D3DFMT_X8B8G8R8, D3DFMT_G16R16, D3DFMT_A2R10G10B10, D3DFMT_A16B16G16R16, D3DFMT_L8,
                                        D3DFMT_A8L8, D3DFMT_V8U8, D3DFMT_Q8W8V8U8, D3DFMT_V16U16, D3DFMT_L16, D3DFMT_R16F,
                                        D3DFMT_G16R16F, D3DFMT_A16B16G16R16F, D3DFMT_R32F, D3DFMT_G32R32F, D3DFMT_A32B32G32R32F,
                                        D3DFMT_DXT1, D3DFMT_DXT2, D3DFMT_DXT3, D3DFMT_DXT4, D3DFMT_DXT5};
        int made = 0, rts = 0, nrt = 0;
        for (unsigned k = 0; k < sizeof fmts / sizeof fmts[0]; k++) {
            if (gcall(dev, S_Device_CreateTexture, 8, 16, 16, 0, 0, fmts[k], D3DPOOL_MANAGED, out, 0) == D3D_OK) {
                mt_obj *t = mt_from_guest(M, gld(out), MT_K_TEX);
                if (t && t->tex && t->view) made++;
                else fprintf(stderr, "format %u: no Metal texture\n", fmts[k]);
                gcall(gld(out), 2, 0);
            } else fprintf(stderr, "format %u: CreateTexture failed\n", fmts[k]);
            if (gcall(d3d, S_D3D9_CheckDeviceFormat, 6, 0, D3DDEVTYPE_HAL, D3DFMT_X8R8G8B8, D3DUSAGE_RENDERTARGET, D3DRTYPE_TEXTURE, fmts[k]) == D3D_OK) {
                nrt++;
                if (gcall(dev, S_Device_CreateTexture, 8, 16, 16, 1, D3DUSAGE_RENDERTARGET, fmts[k], D3DPOOL_DEFAULT, out, 0) == D3D_OK) {
                    rts++;
                    gcall(gld(out), 2, 0);
                } else fprintf(stderr, "format %u: render-target texture failed\n", fmts[k]);
            }
        }
        fprintf(stderr, "formats: %d of %u textures, %d of %d render-target textures\n", made, (unsigned)(sizeof fmts / sizeof fmts[0]), rts, nrt);
        CHECK_EQ(made, (int)(sizeof fmts / sizeof fmts[0]));
        CHECK_EQ(rts, nrt);
        static const uint32_t dfmts[] = {D3DFMT_D24S8, D3DFMT_D24X8, D3DFMT_D16, D3DFMT_D32F_LOCKABLE};
        for (unsigned k = 0; k < 4; k++) {
            CHECK_EQ(gcall(dev, S_Device_CreateTexture, 8, 64, 64, 1, D3DUSAGE_DEPTHSTENCIL, dfmts[k], D3DPOOL_DEFAULT, out, 0), D3D_OK);
            uint32_t dtex = gld(out);
            CHECK_EQ(gcall(dtex, S_Texture_GetSurfaceLevel, 2, 0, out2), D3D_OK);
            uint32_t dsurf = gld(out2);
            CHECK(mt_from_guest(M, dtex, MT_K_TEX)->tex != nil);
            /* as a depth target next to a colour target of the same size, cleared */
            CHECK_EQ(gcall(dev, S_Device_CreateRenderTarget, 8, 64, 64, D3DFMT_A8R8G8B8, 0, 0, 0, out, 0), D3D_OK);
            uint32_t c64 = gld(out);
            CHECK_EQ(gcall(dev, S_Device_GetDepthStencilSurface, 1, out), D3D_OK);
            uint32_t ods = gld(out);
            CHECK_EQ(gcall(dev, S_Device_SetRenderTarget, 2, 0, c64), D3D_OK);
            CHECK_EQ(gcall(dev, S_Device_SetDepthStencilSurface, 1, dsurf), D3D_OK);
            CHECK_EQ(gcall(dev, S_Device_Clear, 6, 0, 0, 3, 0xff00ff00u, f2u(0.25f), 0), D3D_OK);
            if (dfmts[k] == D3DFMT_D32F_LOCKABLE) { /* a lockable depth format: the cleared depth reads back */
                CHECK_EQ(gcall(dsurf, S_Surface_LockRect, 3, lr, 0, D3DLOCK_READONLY), D3D_OK);
                float z;
                memcpy(&z, M + gld(lr + 4) + 5 * gld(lr) + 20, 4);
                CHECK(fabsf(z - 0.25f) < 1e-6f);
                gcall(dsurf, S_Surface_UnlockRect, 0);
            } else {
                CHECK_EQ(gcall(dsurf, S_Surface_LockRect, 3, lr, 0, D3DLOCK_READONLY), D3DERR_INVALIDCALL);
            }
            CHECK_EQ(gcall(dev, S_Device_SetRenderTarget, 2, 0, bb), D3D_OK);
            CHECK_EQ(gcall(dev, S_Device_SetDepthStencilSurface, 1, ods), D3D_OK);
            gcall(ods, 2, 0);
            CHECK_EQ(gcall(c64, 2, 0), 0);
            gcall(dsurf, 2, 0);
            CHECK_EQ(gcall(dtex, 2, 0), 0);
        }
        CHECK_EQ(gcall(dev, S_Device_CreateCubeTexture, 7, 32, 1, D3DUSAGE_RENDERTARGET, D3DFMT_A8R8G8B8, D3DPOOL_DEFAULT, out, 0), D3D_OK);
        uint32_t crt = gld(out);
        CHECK_EQ(gcall(crt, S_CubeTexture_GetCubeMapSurface, 3, 4, 0, out), D3D_OK);
        uint32_t face4 = gld(out);
        CHECK_EQ(gcall(dev, S_Device_ColorFill, 3, face4, 0, 0xff445566u), D3D_OK);
        CHECK_EQ(gcall(dev, S_Device_CreateRenderTarget, 8, 32, 32, D3DFMT_A8R8G8B8, 0, 0, 0, out, 0), D3D_OK);
        uint32_t r32 = gld(out);
        CHECK_EQ(gcall(dev, S_Device_StretchRect, 5, face4, 0, r32, 0, 1), D3D_OK);
        CHECK_EQ(gcall(dev, S_Device_CreateOffscreenPlainSurface, 6, 32, 32, D3DFMT_A8R8G8B8, D3DPOOL_SYSTEMMEM, out, 0), D3D_OK);
        uint32_t o32 = gld(out);
        CHECK_EQ(rt_pixel(r32, o32, 7, 9), 0xff445566u);
        gcall(o32, 2, 0), gcall(r32, 2, 0), gcall(face4, 2, 0);
        CHECK_EQ(gcall(crt, 2, 0), 0);
    }
    /* ---- volume and cube upload/readback through Metal: write, upload, clear the shadow, read back ---- */
    {
        CHECK_EQ(gcall(dev, S_Device_CreateVolumeTexture, 9, 8, 4, 4, 2, 0, D3DFMT_A8R8G8B8, D3DPOOL_MANAGED, out, 0), D3D_OK);
        uint32_t vt = gld(out), lb = gscratch(12);
        CHECK_EQ(gcall(vt, S_VolumeTexture_LockBox, 4, 0, lb, 0, 0), D3D_OK);
        for (uint32_t z = 0; z < 4; z++)
            for (uint32_t y = 0; y < 4; y++)
                for (uint32_t x = 0; x < 8; x++) gst(gld(lb + 8) + z * gld(lb + 4) + y * gld(lb) + 4 * x, 0xff000000u | z << 16 | y << 8 | x);
        CHECK_EQ(gcall(vt, S_VolumeTexture_UnlockBox, 1, 0), D3D_OK);
        mt_obj *mv = mt_from_guest(M, vt, MT_K_VOLTEX);
        memset(M + mv->shadow, 0, mv->lvl_slice[0] * 4);
        CHECK_EQ(mt_readback_all(md, mv), 0);
        CHECK_EQ(gld(mv->shadow + 3 * mv->lvl_slice[0] + 2 * mv->lvl_pitch[0] + 4 * 5), 0xff030205u);
        /* in use by the current command buffer: the next upload is a staged blit, ordered after that use */
        mt_cmd(md);
        mt_mark_used(md, mv);
        d3d9metal_get_stats(&s0);
        uint32_t box = gscratch(24);
        gst(box, 1); gst(box + 4, 1); gst(box + 8, 3); gst(box + 12, 2); gst(box + 16, 2); gst(box + 20, 3);
        CHECK_EQ(gcall(vt, S_VolumeTexture_LockBox, 4, 0, lb, box, 0), D3D_OK);
        gst(gld(lb + 8), 0xffabcdefu); gst(gld(lb + 8) + 4, 0xff123456u);
        CHECK_EQ(gcall(vt, S_VolumeTexture_UnlockBox, 1, 0), D3D_OK);
        d3d9metal_get_stats(&s1);
        CHECK_EQ(s1.staged_uploads - s0.staged_uploads, 1);
        CHECK_EQ(s1.bytes_out - s0.bytes_out, 8);
        memset(M + mv->shadow, 0, mv->lvl_slice[0] * 4);
        CHECK_EQ(mt_readback_all(md, mv), 0);
        CHECK_EQ(gld(mv->shadow + 2 * mv->lvl_slice[0] + 1 * mv->lvl_pitch[0] + 4 * 1), 0xffabcdefu);
        CHECK_EQ(gld(mv->shadow + 2 * mv->lvl_slice[0] + 1 * mv->lvl_pitch[0] + 4 * 2), 0xff123456u);
        CHECK_EQ(gld(mv->shadow + 2 * mv->lvl_slice[0] + 1 * mv->lvl_pitch[0] + 4 * 3), 0xff020103u);
        CHECK_EQ(gcall(vt, 2, 0), 0);
        CHECK_EQ(gcall(dev, S_Device_CreateCubeTexture, 7, 8, 2, 0, D3DFMT_DXT5, D3DPOOL_MANAGED, out, 0), D3D_OK);
        uint32_t cb = gld(out);
        for (uint32_t f = 0; f < 6; f++) {
            CHECK_EQ(gcall(cb, S_CubeTexture_LockRect, 5, f, 1, lr, 0, 0), D3D_OK); /* level 1: 4x4, one block */
            gst(gld(lr + 4), 0x11111111u * (f + 1)); gst(gld(lr + 4) + 4, 0x22222222u); gst(gld(lr + 4) + 8, 0x33333333u * (f + 1)); gst(gld(lr + 4) + 12, 0x44444444u);
            CHECK_EQ(gcall(cb, S_CubeTexture_UnlockRect, 2, f, 1), D3D_OK);
        }
        mt_obj *mc = mt_from_guest(M, cb, MT_K_CUBE);
        uint32_t f5 = mc->shadow + 5 * mc->face_bytes + mc->lvl_off[1];
        gst(f5, 0);
        CHECK_EQ(mt_readback_all(md, mc), 0);
        CHECK_EQ(gld(f5), 0x66666666u);  /* face 5, level 1, block 0: the bytes went through BC3 storage unchanged */
        CHECK_EQ(gld(f5 + 8), 0x33333333u * 6);
        CHECK_EQ(gcall(cb, 2, 0), 0);
    }

    /* ---- buffers: renaming, NOOVERWRITE, the INDEX16 slack ---- */
    uint32_t pb = gscratch(4);
    CHECK_EQ(gcall(dev, S_Device_CreateVertexBuffer, 6, 4096, 0x208, 0, D3DPOOL_DEFAULT, out, 0), D3D_OK);
    uint32_t vb = gld(out);
    mt_obj *mvb = mt_from_guest(M, vb, MT_K_VB);
    CHECK_EQ(gcall(vb, S_VertexBuffer_Lock, 4, 0, 0, pb, D3DLOCK_DISCARD), D3D_OK);
    memset(M + gld(pb), 0x11, 4096);
    CHECK_EQ(gcall(vb, S_VertexBuffer_Unlock, 0), D3D_OK);
    id<MTLBuffer> b1 = mt_buffer_of(md, mvb);
    CHECK(b1 != nil && ((uint8_t *)b1.contents)[100] == 0x11);
    mt_cmd(md);
    mt_mark_used(md, mvb); /* as a draw would: the current command buffer reads it */
    d3d9metal_get_stats(&s0);
    CHECK_EQ(gcall(vb, S_VertexBuffer_Lock, 4, 0, 0, pb, D3DLOCK_DISCARD), D3D_OK);
    memset(M + gld(pb), 0x22, 4096);
    CHECK_EQ(gcall(vb, S_VertexBuffer_Unlock, 0), D3D_OK);
    d3d9metal_get_stats(&s1);
    CHECK_EQ(s1.renames - s0.renames, 1);
    CHECK(mt_buffer_of(md, mvb) != b1);
    CHECK_EQ(((uint8_t *)b1.contents)[100], 0x11); /* what the GPU reads is intact */
    CHECK_EQ(((uint8_t *)mt_buffer_of(md, mvb).contents)[100], 0x22);
    mt_mark_used(md, mvb);
    CHECK_EQ(gcall(vb, S_VertexBuffer_Lock, 4, 1024, 64, pb, D3DLOCK_NOOVERWRITE), D3D_OK);
    memset(M + gld(pb), 0x33, 64);
    CHECK_EQ(gcall(vb, S_VertexBuffer_Unlock, 0), D3D_OK);
    d3d9metal_get_stats(&s0);
    CHECK_EQ(s0.renames - s1.renames, 0);
    CHECK_EQ(((uint8_t *)mt_buffer_of(md, mvb).contents)[1024], 0x33);
    CHECK_EQ(((uint8_t *)mt_buffer_of(md, mvb).contents)[1088], 0x22);
    /* ten more renames while in flight: at most four spares are kept, the oldest is retired, and the Metal
     * byte count follows (retired copies are no longer counted) */
    {
        d3d9metal_stats a, b;
        d3d9metal_get_stats(&a);
        for (int i = 0; i < 10; i++) {
            mt_mark_used(md, mvb);
            CHECK_EQ(gcall(vb, S_VertexBuffer_Lock, 4, 0, 0, pb, D3DLOCK_DISCARD), D3D_OK);
            CHECK_EQ(gcall(vb, S_VertexBuffer_Unlock, 0), D3D_OK);
        }
        d3d9metal_get_stats(&b);
        fprintf(stderr, "renames %llu: object metal bytes %llu (%u spares), device count change %lld\n",
                (unsigned long long)(b.renames - a.renames), (unsigned long long)mvb->metal_bytes, mvb->nspare,
                (long long)(b.metal_bytes - a.metal_bytes));
        CHECK(mvb->metal_bytes <= (uint64_t)(1 + mvb->nspare) * mvb->bytes);
        CHECK((int64_t)(b.metal_bytes - a.metal_bytes) <= (int64_t)(4 * mvb->bytes));
    }
    CHECK_EQ(gcall(dev, S_Device_CreateIndexBuffer, 6, 256, 0x208, D3DFMT_INDEX16, D3DPOOL_DEFAULT, out, 0), D3D_OK);
    uint32_t ib = gld(out);
    mt_obj *mib = mt_from_guest(M, ib, MT_K_IB);
    CHECK_EQ(gcall(ib, S_IndexBuffer_Lock, 4, 0, 0, pb, 0), D3D_OK);
    memset(M + gld(pb), 0, 256);
    CHECK_EQ(gcall(ib, S_IndexBuffer_Unlock, 0), D3D_OK);
    mt_buffer_of(md, mib);
    CHECK_EQ(gcall(ib, S_IndexBuffer_Lock, 4, 0, 24, pb, D3DLOCK_DISCARD), D3D_OK); /* the terrain brushes' count-as-bytes lock */
    for (int i = 0; i < 24; i++) M[gld(pb) + 2 * i] = (uint8_t)(i + 1), M[gld(pb) + 2 * i + 1] = 0;
    CHECK_EQ(gcall(ib, S_IndexBuffer_Unlock, 0), D3D_OK);
    {
        const uint16_t *ix = (const uint16_t *)mt_buffer_of(md, mib).contents;
        int slack = !(getenv("SPORE_METAL_IB_SLACK") && *getenv("SPORE_METAL_IB_SLACK") == '0');
        CHECK_EQ(ix[5], 6);
        CHECK_EQ(ix[23], slack ? 24 : 0); /* past the 24 locked bytes: there with the slack (DEVIATIONS 3.6) */
    }

    /* ---- AUTOGENMIPMAP ---- */
    CHECK_EQ(gcall(dev, S_Device_CreateTexture, 8, 64, 64, 0, D3DUSAGE_AUTOGENMIPMAP | D3DUSAGE_DYNAMIC, D3DFMT_A8R8G8B8, D3DPOOL_DEFAULT, out, 0), D3D_OK);
    uint32_t mip = gld(out);
    CHECK_EQ(gcall(mip, S_Texture_GetLevelCount, 0), 1);
    CHECK_EQ(gcall(mip, S_Texture_LockRect, 4, 0, lr, 0, D3DLOCK_DISCARD), D3D_OK);
    for (uint32_t i = 0; i < 64 * 64; i++) gst(gld(lr + 4) + 4 * i, (i & 1) ? 0xffffffffu : 0xff000000u);
    CHECK_EQ(gcall(mip, S_Texture_UnlockRect, 1, 0), D3D_OK);
    {
        mt_obj *mm = mt_from_guest(M, mip, MT_K_TEX);
        CHECK(mm->mips_dirty);
        id<MTLTexture> t = mt_texture_of(md, mm);
        CHECK(t != nil && !mm->mips_dirty && t.mipmapLevelCount == 7);
        id<MTLBuffer> rb = [md->mtl newBufferWithLength:4 options:MTLResourceStorageModeShared];
        id<MTLBlitCommandEncoder> be = mt_blit(md);
        [be copyFromTexture:mm->tex sourceSlice:0 sourceLevel:6 sourceOrigin:MTLOriginMake(0, 0, 0) sourceSize:MTLSizeMake(1, 1, 1)
                   toBuffer:rb destinationOffset:0 destinationBytesPerRow:4 destinationBytesPerImage:4];
        mt_flush(md, 1);
        uint32_t px = *(uint32_t *)rb.contents;
        fprintf(stderr, "autogen mip 6 (1x1) of a black/white checker: 0x%08x\n", px);
        CHECK(near_argb(px, 0xff808080u, 2));
        [rb release];
    }
    CHECK_EQ(gcall(mip, 2, 0), 0);

    /* ---- queries ---- */
    CHECK_EQ(gcall(dev, S_Device_CreateQuery, 2, D3DQUERYTYPE_EVENT, out), D3D_OK);
    uint32_t q = gld(out);
    CHECK_EQ(gcall(dev, S_Device_Clear, 6, 0, 0, 1, 0xff000000u, f2u(1.0f), 0), D3D_OK);
    CHECK_EQ(gcall(q, S_Query_Issue, 1, D3DISSUE_END), D3D_OK);
    int spins = 0;
    uint32_t r;
    while ((r = gcall(q, S_Query_GetData, 3, out2, 4, D3DGETDATA_FLUSH)) == SG_S_FALSE && spins < 100000) spins++;
    CHECK_EQ(r, D3D_OK);
    CHECK_EQ(gld(out2), 1);
    fprintf(stderr, "event query: S_OK after %d S_FALSE\n", spins);
    CHECK_EQ(gcall(q, 2, 0), 0);
    CHECK_EQ(gcall(dev, S_Device_CreateQuery, 2, D3DQUERYTYPE_OCCLUSION, out), D3D_OK);
    q = gld(out);
    CHECK_EQ(gcall(q, S_Query_Issue, 1, D3DISSUE_BEGIN), D3D_OK);
    CHECK_EQ(gcall(dev, S_Device_Clear, 6, 0, 0, 1, 0xff000000u, f2u(1.0f), 0), D3D_OK);
    CHECK_EQ(gcall(q, S_Query_Issue, 1, D3DISSUE_END), D3D_OK);
    spins = 0;
    gst(out2, 0xffffffffu);
    while ((r = gcall(q, S_Query_GetData, 3, out2, 4, D3DGETDATA_FLUSH)) == SG_S_FALSE && spins < 100000) spins++;
    CHECK_EQ(r, D3D_OK);
    CHECK_EQ(gld(out2), 0); /* no draws: no samples passed */
    CHECK_EQ(gcall(q, 2, 0), 0);

    /* ---- a bound texture released by the guest stays alive until unbound (D3D9's internal reference) ---- */
    CHECK_EQ(gcall(dev, S_Device_CreateTexture, 8, 16, 16, 1, 0, D3DFMT_A8R8G8B8, D3DPOOL_MANAGED, out, 0), D3D_OK);
    uint32_t bt = gld(out);
    CHECK_EQ(gcall(dev, S_Device_SetTexture, 2, 3, bt), D3D_OK);
    unsigned live_b = sg_com_live_objects();
    CHECK_EQ(gcall(bt, 2, 0), 0);
    CHECK_EQ(sg_com_live_objects(), live_b - 1);        /* the guest record is gone ... */
    CHECK(md->st.tex[3] != NULL && md->st.tex[3]->zombie); /* ... the object is still bound */
    CHECK_EQ(gcall(dev, S_Device_SetTexture, 2, 3, 0), D3D_OK);
    CHECK(md->st.tex[3] == NULL);

    /* ---- Reset to a new size ---- */
    gcall(bb, 2, 0);
    CHECK_EQ(gcall(vb, 2, 0), 0);
    CHECK_EQ(gcall(ib, 2, 0), 0);
    CHECK_EQ(gcall(small, 2, 0), 0);
    CHECK_EQ(gcall(rt8, 2, 0), 0);
    gst(pp, 400); gst(pp + 4, 300);
    T.fpu.cw = 0x027f;
    CHECK_EQ(gcall(dev, S_Device_Reset, 1, pp), D3D_OK);
    CHECK_EQ(T.fpu.cw, 0x007f);
    CHECK_EQ(gcall(dev, S_Device_GetBackBuffer, 4, 0, 0, 0, out), D3D_OK);
    bb = gld(out);
    uint32_t desc = gscratch(32);
    CHECK_EQ(gcall(bb, S_Surface_GetDesc, 1, desc), D3D_OK);
    CHECK_EQ(gld(desc + 24), 400);
    for (int f = 0; f < 10; f++) {
        CHECK_EQ(gcall(dev, S_Device_Clear, 6, 0, 0, 7, 0xff336699u, f2u(1.0f), 0), D3D_OK);
        CHECK_EQ(gcall(dev, S_Device_Present, 4, 0, 0, 0, 0), D3D_OK);
        test_pump();
    }
    gcall(bb, 2, 0);

    /* ---- Present into the window's layer with a letterbox rectangle set (as the platform layer sets it in
     * fullscreen): every frame still gets a drawable and presents ---- */
    {
        CALayer *wl = (CALayer *)d3d9metal_host_layer(0x00010001);
        [wl setValue:[NSValue valueWithRect:NSMakeRect(0.1, 0.05, 0.8, 0.9)] forKey:@"sporeContentRect"];
        d3d9metal_get_stats(&s0);
        for (int f = 0; f < 10; f++) {
            CHECK_EQ(gcall(dev, S_Device_Clear, 6, 0, 0, 7, 0xff993366u, f2u(1.0f), 0), D3D_OK);
            CHECK_EQ(gcall(dev, S_Device_Present, 4, 0, 0, 0, 0), D3D_OK);
            test_pump();
        }
        d3d9metal_get_stats(&s1);
        CHECK_EQ(s1.frames - s0.frames, 10);
        CHECK_EQ(s1.drawables_nil - s0.drawables_nil, 0);
        CHECK_EQ(s1.presents_skipped - s0.presents_skipped, 0);
        [wl setValue:nil forKey:@"sporeContentRect"];
    }

    /* ---- release everything ---- */
    CHECK_EQ(gcall(off, 2, 0), 0);
    CHECK_EQ(gcall(off8, 2, 0), 0);
    CHECK_EQ(gcall(dev, 2, 0), 0);
    CHECK_EQ(gcall(d3d, 2, 0), 0);
    CHECK_EQ(sg_com_live_objects(), live0);
    d3d9metal_get_stats(&s1);
    d3d9null_stats ns;
    d3d9null_get_stats(&ns);
    fprintf(stderr, "d3d9metal: frames %llu, locks %llu, out %llu B, in %llu B, renames %llu, waits %llu, cmdbufs %llu, staged %llu, direct %llu, "
                    "readbacks %llu; live resources %u, shadow %u B, metal %llu B\n",
            (unsigned long long)s1.frames, (unsigned long long)s1.locks, (unsigned long long)s1.bytes_out, (unsigned long long)s1.bytes_in,
            (unsigned long long)s1.renames, (unsigned long long)s1.waits, (unsigned long long)s1.cmdbufs, (unsigned long long)s1.staged_uploads,
            (unsigned long long)s1.direct_uploads, (unsigned long long)s1.readbacks, ns.live_resources, s1.shadow_bytes,
            (unsigned long long)s1.metal_bytes);
    CHECK_EQ(ns.live_resources, 0);
    CHECK_EQ(s1.shadow_bytes, 0);
    CHECK_EQ(s1.metal_bytes, 0);
    test_window_close();
    return harness_report("runtime/d3d9metal present");
}
