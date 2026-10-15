/* mt_present.mm -- presentation and the device layer's own draws: the swap chain's back buffer drawn into a
 * CAMetalLayer drawable each Present, scaled to the drawable (DEVIATIONS 3.11:
 * the Retina drawable, the game's back buffer stretched to it with an area-weighted "sharp bilinear"
 * filter, util_present, so a 2x Retina scale is exact pixel replication), vsync on
 * (displaySyncEnabled), the frame committed, the stall watchdog's beat. The same small pipeline does
 * StretchRect with scaling or a format change, and ColorFill.
 * Three cases (mt_present_blit): scaled to the whole drawable as above; 1:1 when the back buffer is the
 * drawable's size (Retina-native, SPORE_RETINA=1: an exact copy); letterboxed into the rectangle the platform
 * layer puts on the layer ("sporeContentRect", fullscreen), black bars around it.
 *
 * Present returns promptly: nextDrawable waits only while all drawables are in flight (vsync pacing);
 * without a window yet (the game creates the device before its first ShowWindow) the frame is counted,
 * its command buffer committed, and the null device's pacing (sg_host_present) applies. */
#import <Metal/Metal.h>
#import <QuartzCore/CAMetalLayer.h>
#include <unistd.h>

#include "mt_priv.h"

static const char *k_util_msl =
    "#include <metal_stdlib>\n"
    "using namespace metal;\n"
    "struct VO { float4 pos [[position]]; float2 uv; };\n"
    "vertex VO util_vs(uint vid [[vertex_id]], constant float4 &r [[buffer(0)]]) {\n"
    "  float2 p = float2((vid << 1) & 2, vid & 2);\n"
    "  VO o; o.pos = float4(p * float2(2.0, -2.0) + float2(-1.0, 1.0), 0.0, 1.0);\n"
    "  o.uv = r.xy + (r.zw - r.xy) * p; return o; }\n"
    "fragment float4 util_copy(VO i [[stage_in]], texture2d<float> t [[texture(0)]], sampler s [[sampler(0)]]) {\n"
    "  return t.sample(s, i.uv); }\n"
    "fragment float4 util_copy_opaque(VO i [[stage_in]], texture2d<float> t [[texture(0)]], sampler s [[sampler(0)]]) {\n"
    "  return float4(t.sample(s, i.uv).rgb, 1.0); }\n"
    "fragment float4 util_fill(VO i [[stage_in]], constant float4 &c [[buffer(0)]]) { return c; }\n"
    /* Present's scaling, an area-weighted "sharp bilinear" filter: when a drawable pixel lies inside one
     * back-buffer texel that texel is taken as is (an integer 2x Retina scale is pixel replication), otherwise the
     * two texels are blended by the area each covers (plain bilinear sampling would soften every frame). ext.xy = the source size.
     * Attribution: the filter is derived from the present blit shader of the DXVK project (zlib licence,
     * https://github.com/doitsujin/dxvk), rewritten in MSL and altered (the 1:1 path); see THIRD_PARTY.md. */
    "fragment float4 util_present(VO i [[stage_in]], texture2d<float> t [[texture(0)]], constant float4 &ext [[buffer(0)]]) {\n"
    "  float2 coord = ext.xy * i.uv;\n"
    "  float2 delta = float2(dfdx(coord.x), dfdy(coord.y));\n"
    /* otherwise (1:1, Retina-native, or a smaller drawable) plain bilinear about texel centres, so 1:1 is an
     * exact copy (taking fract(coord) there would blend each texel half with its neighbour) */
    "  float2 fc = fract(coord - 0.5); float2 ic = floor(coord - 0.5);\n"
    "  if (all(delta < float2(1.0))) {\n"
    "    float2 lo = max(coord - 0.5 * delta, float2(0.0));\n"
    "    float2 hi = min(coord + 0.5 * delta, ext.xy - 1.0);\n"
    "    ic = floor(lo);\n"
    "    fc = select((hi - floor(hi)) / delta, float2(0.0), floor(lo) == floor(hi));\n"
    "  }\n"
    "  int2 mx = int2(ext.xy) - 1; int2 a = clamp(int2(ic), int2(0), mx); int2 b = clamp(int2(ic) + 1, int2(0), mx);\n"
    "  float4 c = mix(mix(t.read(uint2(a.x, a.y)), t.read(uint2(b.x, a.y)), fc.x),\n"
    "                 mix(t.read(uint2(a.x, b.y)), t.read(uint2(b.x, b.y)), fc.x), fc.y);\n"
    "  return float4(c.rgb, 1.0); }\n";

static id<MTLRenderPipelineState> util_pso(mt_dev *d, int kind, uint32_t fmt) {
    uint32_t key = ((uint32_t)kind << 24) | (fmt & 0xffffff);
    int free_slot = -1;
    for (int i = 0; i < 16; i++) {
        if (d->util_pso[i] && d->util_key[i] == key) return (id<MTLRenderPipelineState>)d->util_pso[i];
        if (!d->util_pso[i] && free_slot < 0) free_slot = i;
    }
    if (!d->util_lib) {
        NSError *err = nil;
        NSString *src = [NSString stringWithUTF8String:k_util_msl];
        d->util_lib = [d->mtl newLibraryWithSource:src options:nil error:&err];
        if (!d->util_lib) {
            MT_LOG("util library: %s\n", err ? err.localizedDescription.UTF8String : "?");
            return nil;
        }
    }
    static const char *const frag[4] = {"util_copy", "util_fill", "util_copy_opaque", "util_present"};
    MTLRenderPipelineDescriptor *pd = [[MTLRenderPipelineDescriptor alloc] init];
    id<MTLFunction> vs = [d->util_lib newFunctionWithName:@"util_vs"];
    id<MTLFunction> fs = [d->util_lib newFunctionWithName:[NSString stringWithUTF8String:frag[kind]]];
    pd.vertexFunction = vs;
    pd.fragmentFunction = fs;
    pd.colorAttachments[0].pixelFormat = (MTLPixelFormat)fmt;
    NSError *err = nil;
    id<MTLRenderPipelineState> p = [d->mtl newRenderPipelineStateWithDescriptor:pd error:&err];
    [vs release];
    [fs release];
    [pd release];
    if (!p) {
        MT_LOG("util pipeline (%s, pixel format %u): %s\n", frag[kind], fmt, err ? err.localizedDescription.UTF8String : "?");
        return nil;
    }
    if (free_slot < 0) {
        [(id)d->util_pso[0] release];
        free_slot = 0;
    }
    d->util_pso[free_slot] = p;
    d->util_key[free_slot] = key;
    return p;
}

/* Draws src's rectangle (COPY) or a colour (FILL) into drect of (dslice, dlevel) of dst, in its own pass. */
extern "C" int mt_util_draw(mt_dev *d, int kind, mt_obj *src, const uint32_t srect[4], id<MTLTexture> dst, uint32_t dslice,
                            uint32_t dlevel, uint32_t dfmt, const uint32_t drect[4], uint32_t dw, uint32_t dh, int linear,
                            const float rgba[4]) {
    id<MTLRenderPipelineState> pso = util_pso(d, kind, dfmt);
    id<MTLTexture> view = nil;
    float uv[4] = {0, 0, 1, 1}, sw = 1, sh = 1;
    if (!pso) return -1;
    if (kind != MT_UTIL_FILL) {
        uint32_t f = 0, l = 0;
        mt_obj *t = mt_top(src, &f, &l);
        if (!t->tex) return -1;
        /* sampled as D3D9 samples it: the format's swizzle (X8 alpha 1, L8 rrr1, A8L8 rrrg, ...) */
        if (t->tex.textureType == MTLTextureType2D && t->tex.mipmapLevelCount == 1) view = [t->view retain];
        else if (t->mf && t->view != t->tex)
            view = [t->tex newTextureViewWithPixelFormat:t->tex.pixelFormat textureType:MTLTextureType2D levels:NSMakeRange(l, 1)
                                                  slices:NSMakeRange(t->kind == MT_K_CUBE ? f : 0, 1)
                                                 swizzle:MTLTextureSwizzleChannelsMake((MTLTextureSwizzle)t->mf->swz[0], (MTLTextureSwizzle)t->mf->swz[1],
                                                                                       (MTLTextureSwizzle)t->mf->swz[2], (MTLTextureSwizzle)t->mf->swz[3])];
        else view = [t->tex newTextureViewWithPixelFormat:t->tex.pixelFormat textureType:MTLTextureType2D levels:NSMakeRange(l, 1)
                                                   slices:NSMakeRange(t->kind == MT_K_CUBE ? f : 0, 1)];
        if (!view) return -1;
        sw = (float)lvl_dim(t->w, l), sh = (float)lvl_dim(t->h, l);
        uv[0] = srect[0] / sw, uv[1] = srect[1] / sh, uv[2] = srect[2] / sw, uv[3] = srect[3] / sh;
        mt_mark_used(d, t);
    }
    mt_pass_end(d);
    id<MTLCommandBuffer> cb = mt_cmd(d);
    MTLRenderPassDescriptor *rp = [MTLRenderPassDescriptor renderPassDescriptor];
    rp.colorAttachments[0].texture = dst;
    rp.colorAttachments[0].slice = dst.textureType == MTLTextureTypeCube ? dslice : 0;
    rp.colorAttachments[0].level = dlevel;
    int whole = drect[0] == 0 && drect[1] == 0 && drect[2] >= dw && drect[3] >= dh;
    rp.colorAttachments[0].loadAction = whole ? MTLLoadActionDontCare : MTLLoadActionLoad;
    rp.colorAttachments[0].storeAction = MTLStoreActionStore;
    id<MTLRenderCommandEncoder> enc = [cb renderCommandEncoderWithDescriptor:rp];
    if (!enc) {
        [view release];
        return -1;
    }
    [enc setRenderPipelineState:pso];
    MTLViewport vp = {(double)drect[0], (double)drect[1], (double)(drect[2] - drect[0]), (double)(drect[3] - drect[1]), 0.0, 1.0};
    [enc setViewport:vp];
    [enc setVertexBytes:uv length:sizeof uv atIndex:0];
    if (kind == MT_UTIL_FILL) {
        [enc setFragmentBytes:rgba length:16 atIndex:0];
    } else {
        [enc setFragmentTexture:view atIndex:0];
        if (kind == MT_UTIL_PRESENT) {
            float ext[4] = {sw, sh, 0, 0};
            [enc setFragmentBytes:ext length:sizeof ext atIndex:0];
        } else {
            [enc setFragmentSamplerState:linear ? d->samp_linear : d->samp_point atIndex:0];
        }
    }
    [enc drawPrimitives:MTLPrimitiveTypeTriangle vertexStart:0 vertexCount:3];
    [enc endEncoding];
    [view release];
    return 0;
}

/* ---- Present ------------------------------------------------------------------------------------------ */
/* Present-to-present intervals since the last watch line (mt_present_times) and the device whose
 * currentAllocatedSize the watch line reports. Written by the presenting thread, read under the device mutex. */
#define MT_PT_MAX 8192
static uint32_t g_pt_us[MT_PT_MAX];
static int g_pt_n;
static uint64_t g_pt_last;
static id<MTLDevice> g_pt_mtl;
static int cmp_u32(const void *a, const void *b) {
    uint32_t x = *(const uint32_t *)a, y = *(const uint32_t *)b;
    return x < y ? -1 : x > y;
}
extern "C" int mt_present_times(double *p50_ms, double *p90_ms, double *max_ms, double *mean_ms, uint64_t *metal_alloc) {
    int n = g_pt_n;
    *p50_ms = *p90_ms = *max_ms = *mean_ms = 0;
    *metal_alloc = g_pt_mtl ? (uint64_t)g_pt_mtl.currentAllocatedSize : 0;
    if (n > 0) {
        static uint32_t tmp[MT_PT_MAX];
        double sum = 0;
        memcpy(tmp, g_pt_us, n * sizeof *tmp);
        qsort(tmp, n, sizeof *tmp, cmp_u32);
        for (int i = 0; i < n; i++) sum += tmp[i];
        *p50_ms = tmp[n / 2] / 1000.0, *p90_ms = tmp[(n * 9) / 10] / 1000.0, *max_ms = tmp[n - 1] / 1000.0;
        *mean_ms = sum / n / 1000.0;
    }
    g_pt_n = 0;
    return n;
}
static int g_pace_fps = -1;
static void pace(uint32_t hwnd, uint64_t frame, int presented) {
    if (g_pace_fps < 0) {
        const char *f = getenv("SPORE_D3D9_FPS");
        g_pace_fps = f && *f ? atoi(f) : 60;
        if (g_pace_fps < 0) g_pace_fps = 0;
    }
    if (presented && g_pace_fps > 0) {
        /* vsync paces through nextDrawable; the frame limiter is only a floor: a frame back in under half a
         * period (no vsync wait: an occluded window) sleeps the rest */
        static uint64_t last;
        uint64_t t = mt_now_ns(), floor_ns = 500000000ull / (uint64_t)g_pace_fps;
        if (last && t > last && t - last < floor_ns) {
            struct timespec ts = {0, (long)(floor_ns - (t - last))};
            nanosleep(&ts, NULL);
            t = mt_now_ns();
        }
        last = t;
    } else if (!presented) {
        sg_host_present(hwnd, frame); /* the null device's pacing until there is a window */
    }
}
/* Where the back buffer goes in the drawable. The platform layer (runtime/platform/macos sp_window.m) puts
 * the rectangle the game's client is shown in on the layer under "sporeContentRect" (normalised, top-left
 * origin): a fullscreen window letterboxes the game's resolution at its aspect, and mouse points are mapped
 * through the same rectangle. Absent (windowed, or any other host): the whole drawable, stretched as on Windows.
 * Returns 1 if the rectangle leaves bars. The rectangle is rounded to whole pixels. */
extern "C" int mt_present_dest(void *layerp, uint32_t dw, uint32_t dh, uint32_t out[4]) {
    out[0] = 0, out[1] = 0, out[2] = dw, out[3] = dh;
    if (!layerp || !dw || !dh) return 0;
    id v = [(CALayer *)layerp valueForKey:@"sporeContentRect"];
    if (!v || ![v isKindOfClass:[NSValue class]]) return 0;
    CGRect r = [(NSValue *)v rectValue];
    if (!(r.size.width > 0 && r.size.height > 0)) return 0;
    double x0 = fmax(0.0, round(r.origin.x * dw)), y0 = fmax(0.0, round(r.origin.y * dh));
    double x1 = fmin((double)dw, round((r.origin.x + r.size.width) * dw)), y1 = fmin((double)dh, round((r.origin.y + r.size.height) * dh));
    if (x1 - x0 < 1 || y1 - y0 < 1) return 0;
    out[0] = (uint32_t)x0, out[1] = (uint32_t)y0, out[2] = (uint32_t)x1, out[3] = (uint32_t)y1;
    return !(out[0] == 0 && out[1] == 0 && out[2] == dw && out[3] == dh);
}

static void setup_layer(mt_dev *d, CAMetalLayer *layer) {
    if (d->layer == layer) return;
    layer.device = d->mtl;
    layer.pixelFormat = MTLPixelFormatBGRA8Unorm;
    layer.framebufferOnly = YES;
    {   /* vsync on by default (DEVIATIONS 3.14). SPORE_VSYNC=0 presents as fast as the GPU allows, which is what
         * the game asks for (D3DPRESENT_INTERVAL_IMMEDIATE) and gets on Windows; used to test frame-rate-dependent
         * animation. */
        const char *vs = getenv("SPORE_VSYNC");
        layer.displaySyncEnabled = !(vs && *vs == '0');
    }
    layer.maximumDrawableCount = 3;
    if (layer.drawableSize.width < 1 || layer.drawableSize.height < 1) {
        CGFloat s = layer.contentsScale > 0 ? layer.contentsScale : 1;
        layer.drawableSize = CGSizeMake(layer.bounds.size.width * s, layer.bounds.size.height * s);
    }
    [d->layer release];
    d->layer = [layer retain];
    d3d9metal_wininfo wi;
    d3d9metal_window_info(layer, &wi);
    MT_LOG("presenting into CAMetalLayer %p: %.0fx%.0f pt at scale %.1f, drawable %.0fx%.0f, %s, visible %d, key %d\n", (void *)layer,
           wi.w_pt, wi.h_pt, wi.scale, wi.drawable_w, wi.drawable_h, wi.is_content_layer ? "the window's content layer" : "not a content layer",
           wi.visible, wi.key);
}
/* Draws the back buffer into dt (a drawable's texture, or a test's) where the layer says: scaled to the
 * whole texture with the sharp-bilinear filter (1:1 when the sizes match, Retina-native), or into the
 * letterbox rectangle with black bars. Returns 1 letterboxed, 0 whole, -1 on failure. */
extern "C" int mt_present_blit(mt_dev *d, mt_obj *bb, void *layer, id<MTLTexture> dt) {
    uint32_t sr[4] = {0, 0, bb->w, bb->h}, drc[4], whole[4] = {0, 0, (uint32_t)dt.width, (uint32_t)dt.height};
    static const float black[4] = {0, 0, 0, 1};
    int boxed = mt_present_dest(layer, whole[2], whole[3], drc);
    if (boxed && mt_util_draw(d, MT_UTIL_FILL, NULL, NULL, dt, 0, 0, dt.pixelFormat, whole, whole[2], whole[3], 0, black)) return -1;
    if (mt_util_draw(d, MT_UTIL_PRESENT, bb, sr, dt, 0, 0, dt.pixelFormat, drc, whole[2], whole[3], 1, NULL)) return -1;
    static uint32_t last[6];
    uint32_t now[6] = {drc[0], drc[1], drc[2], drc[3], bb->w, bb->h};
    if (memcmp(last, now, sizeof last)) { /* once per change */
        memcpy(last, now, sizeof last);
        MT_LOG("present: back buffer %ux%u into %ux%u at %u,%u of the %ux%u drawable (%s)\n", bb->w, bb->h, drc[2] - drc[0], drc[3] - drc[1],
               drc[0], drc[1], whole[2], whole[3],
               boxed ? "letterboxed" : bb->w == whole[2] && bb->h == whole[3] ? "1:1" : "scaled to the drawable");
    }
    return boxed;
}
extern "C" uint32_t mt_present(sg_com_call *c, mt_dev *d, mt_obj *sc, uint32_t hwnd) {
    (void)c;
    int presented = 0;
    d->frames++;
    nst()->frames++;
    g_mst.frames++;
    {
        uint64_t t = mt_now_ns();
        if (g_pt_last && g_pt_n < MT_PT_MAX) g_pt_us[g_pt_n++] = (uint32_t)((t - g_pt_last) / 1000);
        g_pt_last = t;
        if (!g_pt_mtl) g_pt_mtl = [d->mtl retain];
    }
    d->present_hwnd = hwnd;
    mt_obj *bb = sc ? sc->backbuf : NULL;
    CAMetalLayer *layer = (CAMetalLayer *)d3d9metal_host_layer(hwnd);
    mt_pass_end(d);
    if (layer && bb && bb->tex) {
        setup_layer(d, layer);
        id<CAMetalDrawable> dr = [layer nextDrawable];
        if (dr) {
            if (mt_present_blit(d, bb, layer, dr.texture) >= 0) {
                [mt_cmd(d) presentDrawable:dr];
                presented = 1;
            }
        } else {
            g_mst.drawables_nil++;
        }
    } else {
        g_mst.presents_skipped++;
    }
    mt_cmd(d); /* every frame commits a command buffer, so serials (queries, renames) advance */
    mt_flush(d, 0);
    mt_pl_frame_end(d);
    if (g_mt_trace >= 2 || (g_mt_trace >= 1 && d->frames <= 3))
        MT_LOG("Present frame %llu hwnd 0x%x: %s\n", (unsigned long long)d->frames, hwnd,
               presented ? "presented" : layer ? "no drawable" : "no window yet");
    pace(hwnd, d->frames, presented);
    d->last_present_ns = mt_now_ns();
    mt_watch_tick(0);
    mt_stall_beat();
    return D3D_OK;
}
