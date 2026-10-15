/* d3d9_adapter.c -- Direct3DCreate9 and the null IDirect3D9: adapter identity, caps, formats and display
 * modes per the device policy (DEVIATIONS section 3). */
#include <stdio.h>
#include <stdlib.h>
#include <string.h>
#include <strings.h>
#include <time.h>

#include "d3d9_int.h"

pthread_mutex_t d3d9_lock = PTHREAD_MUTEX_INITIALIZER;
d3d9null_stats d3d9_stats;

/* ---- host hooks (weak defaults) -------------------------------------------------------------- */
__attribute__((weak)) int sg_host_display_mode(uint32_t *w, uint32_t *h, uint32_t *hz) {
    const char *s = getenv("SPORE_DISPLAY_MODE");
    unsigned a = 0, b = 0, r = 60;
    if (!s || sscanf(s, "%ux%u@%u", &a, &b, &r) < 2 || a < 640 || b < 480) a = 1920, b = 1080, r = 60;
    *w = a, *h = b, *hz = r ? r : 60;
    return 0;
}
__attribute__((weak)) int sg_host_client_size(uint32_t hwnd, uint32_t *w, uint32_t *h) {
    (void)hwnd; (void)w; (void)h;
    return -1; /* unknown: the caller uses 1024x768 */
}
__attribute__((weak)) void sg_host_present(uint32_t hwnd, uint64_t frame) {
    /* Pace the null device so an idle game does not spin: SPORE_D3D9_FPS (default 60, 0 = no pacing). */
    static int fps = -1;
    static uint64_t next;
    struct timespec ts;
    uint64_t now, period;
    (void)hwnd; (void)frame;
    if (fps < 0) {
        const char *s = getenv("SPORE_D3D9_FPS");
        fps = s && *s ? atoi(s) : 60;
    }
    if (fps <= 0) return;
    period = 1000000000ull / (uint64_t)fps;
    clock_gettime(CLOCK_MONOTONIC, &ts);
    now = (uint64_t)ts.tv_sec * 1000000000ull + (uint64_t)ts.tv_nsec;
    if (next > now && next - now <= period) {
        uint64_t d = next - now;
        ts.tv_sec = (time_t)(d / 1000000000ull);
        ts.tv_nsec = (long)(d % 1000000000ull);
        nanosleep(&ts, NULL);
        next += period;
    } else {
        next = now + period;
    }
}

void d3d9_current_mode(uint32_t *w, uint32_t *h, uint32_t *hz) { sg_host_display_mode(w, h, hz); }

/* ---- formats --------------------------------------------------------------------------------- */
unsigned d3d9_fmt_bits(uint32_t f, unsigned *block) {
    if (block) *block = 0;
    switch (f) {
    case D3DFMT_DXT1: if (block) *block = 8; return 4;
    case D3DFMT_DXT2: case D3DFMT_DXT3: case D3DFMT_DXT4: case D3DFMT_DXT5: if (block) *block = 16; return 8;
    case D3DFMT_R8G8B8: return 24;
    case D3DFMT_A8R8G8B8: case D3DFMT_X8R8G8B8: case D3DFMT_A2B10G10R10: case D3DFMT_A8B8G8R8: case D3DFMT_X8B8G8R8:
    case D3DFMT_G16R16: case D3DFMT_A2R10G10B10: case D3DFMT_X8L8V8U8: case D3DFMT_Q8W8V8U8: case D3DFMT_V16U16:
    case D3DFMT_A2W10V10U10: case D3DFMT_D32: case D3DFMT_D24S8: case D3DFMT_D24X8: case D3DFMT_D24X4S4:
    case D3DFMT_D32F_LOCKABLE: case D3DFMT_D24FS8: case D3DFMT_G16R16F: case D3DFMT_R32F: case D3DFMT_INDEX32:
        return 32;
    case D3DFMT_R5G6B5: case D3DFMT_X1R5G5B5: case D3DFMT_A1R5G5B5: case D3DFMT_A4R4G4B4: case D3DFMT_A8R3G3B2:
    case D3DFMT_X4R4G4B4: case D3DFMT_A8P8: case D3DFMT_A8L8: case D3DFMT_V8U8: case D3DFMT_L6V5U5:
    case D3DFMT_D16_LOCKABLE: case D3DFMT_D15S1: case D3DFMT_D16: case D3DFMT_L16: case D3DFMT_R16F:
    case D3DFMT_CxV8U8: case D3DFMT_INDEX16:
        return 16;
    case D3DFMT_R3G3B2: case D3DFMT_A8: case D3DFMT_P8: case D3DFMT_L8: case D3DFMT_A4L4: return 8;
    case D3DFMT_A16B16G16R16: case D3DFMT_Q16W16V16U16: case D3DFMT_A16B16G16R16F: case D3DFMT_G32R32F: return 64;
    case D3DFMT_A32B32G32R32F: return 128;
    default: return 0;
    }
}
int d3d9_fmt_is_depth(uint32_t f) {
    return f == D3DFMT_D16_LOCKABLE || f == D3DFMT_D32 || f == D3DFMT_D15S1 || f == D3DFMT_D24S8 || f == D3DFMT_D24X8 ||
           f == D3DFMT_D24X4S4 || f == D3DFMT_D16 || f == D3DFMT_D32F_LOCKABLE || f == D3DFMT_D24FS8;
}
static int is_dxt(uint32_t f) {
    return f == D3DFMT_DXT1 || f == D3DFMT_DXT2 || f == D3DFMT_DXT3 || f == D3DFMT_DXT4 || f == D3DFMT_DXT5;
}
/* Formats a Metal device can back as textures [false for INST, P8/A8P8, YUV, MULTI2]. */
static int tex_ok(uint32_t f) {
    switch (f) {
    case D3DFMT_A8R8G8B8: case D3DFMT_X8R8G8B8: case D3DFMT_R5G6B5: case D3DFMT_X1R5G5B5: case D3DFMT_A1R5G5B5:
    case D3DFMT_A4R4G4B4: case D3DFMT_A8: case D3DFMT_X4R4G4B4: case D3DFMT_A2B10G10R10: case D3DFMT_A8B8G8R8:
    case D3DFMT_X8B8G8R8: case D3DFMT_G16R16: case D3DFMT_A2R10G10B10: case D3DFMT_A16B16G16R16: case D3DFMT_L8:
    case D3DFMT_A8L8: case D3DFMT_V8U8: case D3DFMT_Q8W8V8U8: case D3DFMT_V16U16: case D3DFMT_L16: case D3DFMT_R16F:
    case D3DFMT_G16R16F: case D3DFMT_A16B16G16R16F: case D3DFMT_R32F: case D3DFMT_G32R32F: case D3DFMT_A32B32G32R32F:
        return 1;
    default:
        return is_dxt(f);
    }
}
static int rt_ok(uint32_t f) {
    switch (f) {
    case D3DFMT_A8R8G8B8: case D3DFMT_X8R8G8B8: case D3DFMT_R5G6B5: case D3DFMT_X1R5G5B5: case D3DFMT_A1R5G5B5:
    case D3DFMT_A2B10G10R10: case D3DFMT_A8B8G8R8: case D3DFMT_X8B8G8R8: case D3DFMT_G16R16: case D3DFMT_A2R10G10B10:
    case D3DFMT_A16B16G16R16: case D3DFMT_R16F: case D3DFMT_G16R16F: case D3DFMT_A16B16G16R16F: case D3DFMT_R32F:
    case D3DFMT_G32R32F: case D3DFMT_A32B32G32R32F:
        return 1;
    default:
        return 0;
    }
}
static int ds_ok(uint32_t f) { return f == D3DFMT_D24S8 || f == D3DFMT_D24X8 || f == D3DFMT_D16 || f == D3DFMT_D32F_LOCKABLE; }
static int adapter_fmt_ok(uint32_t f) { return f == D3DFMT_X8R8G8B8 || f == D3DFMT_R5G6B5 || f == D3DFMT_X1R5G5B5; }

uint32_t d3d9_check_format(uint32_t afmt, uint32_t usage, uint32_t rtype, uint32_t f) {
    if (!adapter_fmt_ok(afmt)) return D3DERR_NOTAVAILABLE;
    if (rtype == D3DRTYPE_VERTEXBUFFER) return f == D3DFMT_VERTEXDATA ? D3D_OK : D3DERR_NOTAVAILABLE;
    if (rtype == D3DRTYPE_INDEXBUFFER) return f == D3DFMT_INDEX16 || f == D3DFMT_INDEX32 ? D3D_OK : D3DERR_NOTAVAILABLE;
    if (usage & D3DUSAGE_DMAP) return D3DERR_NOTAVAILABLE;
    if (usage & D3DUSAGE_DEPTHSTENCIL) {
        if (!ds_ok(f) || rtype == D3DRTYPE_VOLUMETEXTURE || rtype == D3DRTYPE_VOLUME) return D3DERR_NOTAVAILABLE;
        return D3D_OK;
    }
    if (d3d9_fmt_is_depth(f)) /* sampled depth (shadow maps) through a texture with DEPTHSTENCIL usage only */
        return D3DERR_NOTAVAILABLE;
    if (!tex_ok(f)) return D3DERR_NOTAVAILABLE; /* includes 'INST', P8, A8P8, YUV, MULTI2_ARGB8 */
    if ((usage & D3DUSAGE_RENDERTARGET) && !rt_ok(f)) return D3DERR_NOTAVAILABLE;
    if (is_dxt(f) && rtype == D3DRTYPE_VOLUMETEXTURE) return D3DERR_NOTAVAILABLE;
    if ((usage & D3DUSAGE_AUTOGENMIPMAP) && (is_dxt(f) || f == D3DFMT_A32B32G32R32F)) return D3DOK_NOAUTOGEN;
    return D3D_OK;
}

/* ---- caps (D3DCAPS9, 304 bytes) --------------------------------------------------------------- */
typedef struct caps9 {
    uint32_t DeviceType, AdapterOrdinal, Caps, Caps2, Caps3, PresentationIntervals, CursorCaps, DevCaps,
        PrimitiveMiscCaps, RasterCaps, ZCmpCaps, SrcBlendCaps, DestBlendCaps, AlphaCmpCaps, ShadeCaps, TextureCaps,
        TextureFilterCaps, CubeTextureFilterCaps, VolumeTextureFilterCaps, TextureAddressCaps, VolumeTextureAddressCaps,
        LineCaps, MaxTextureWidth, MaxTextureHeight, MaxVolumeExtent, MaxTextureRepeat, MaxTextureAspectRatio,
        MaxAnisotropy;
    float MaxVertexW, GuardBandLeft, GuardBandTop, GuardBandRight, GuardBandBottom, ExtentsAdjust;
    uint32_t StencilCaps, FVFCaps, TextureOpCaps, MaxTextureBlendStages, MaxSimultaneousTextures, VertexProcessingCaps,
        MaxActiveLights, MaxUserClipPlanes, MaxVertexBlendMatrices, MaxVertexBlendMatrixIndex;
    float MaxPointSize;
    uint32_t MaxPrimitiveCount, MaxVertexIndex, MaxStreams, MaxStreamStride, VertexShaderVersion, MaxVertexShaderConst,
        PixelShaderVersion;
    float PixelShader1xMaxValue;
    uint32_t DevCaps2;
    float MaxNpatchTessellationLevel;
    uint32_t Reserved5, MasterAdapterOrdinal, AdapterOrdinalInGroup, NumberOfAdaptersInGroup, DeclTypes,
        NumSimultaneousRTs, StretchRectFilterCaps;
    uint32_t VS20Caps, VS20DynamicFlowControlDepth, VS20NumTemps, VS20StaticFlowControlDepth;
    uint32_t PS20Caps, PS20DynamicFlowControlDepth, PS20NumTemps, PS20StaticFlowControlDepth, PS20NumInstructionSlots;
    uint32_t VertexTextureFilterCaps, MaxVShaderInstructionsExecuted, MaxPShaderInstructionsExecuted,
        MaxVertexShader30InstructionSlots, MaxPixelShader30InstructionSlots;
} caps9;
_Static_assert(sizeof(caps9) == D3DCAPS9_SIZE, "D3DCAPS9 is 304 bytes");
_Static_assert(__builtin_offsetof(caps9, VertexShaderVersion) == 0xc4, "D3DCAPS9.VertexShaderVersion at 0xc4");
_Static_assert(__builtin_offsetof(caps9, DevCaps2) == 0xd4, "D3DCAPS9.DevCaps2 at 0xd4");

/* A GeForce 8800-class SM 3.0 profile without PUREDEVICE. */
void d3d9_fill_caps(uint8_t *p, uint32_t devtype) {
    caps9 c;
    memset(&c, 0, sizeof c);
    c.DeviceType = devtype;
    c.Caps = 0x00020000;                 /* READ_SCANLINE */
    c.Caps2 = 0xe0020000;                /* CANAUTOGENMIPMAP | DYNAMICTEXTURES | CANSHARERESOURCE | FULLSCREENGAMMA */
    c.Caps3 = 0x000003a0;                /* ALPHA_FULLSCREEN_FLIP_OR_DISCARD | LINEAR_TO_SRGB | COPY_TO_VIDMEM/SYSTEMMEM */
    c.PresentationIntervals = 0x8000000f; /* IMMEDIATE | ONE..FOUR */
    c.CursorCaps = 0x1;
    c.DevCaps = 0x000bbef0;              /* HWTRANSFORMANDLIGHT and friends; PUREDEVICE (0x100000) NOT set */
    c.PrimitiveMiscCaps = 0x000efcf2;
    c.RasterCaps = 0x0f736191;
    c.ZCmpCaps = 0xff;
    c.SrcBlendCaps = 0x3fff;
    c.DestBlendCaps = 0x3fff;
    c.AlphaCmpCaps = 0xff;
    c.ShadeCaps = 0x00084208;
    c.TextureCaps = 0x0001ec45;          /* no POW2: non-power-of-two textures allowed */
    c.TextureFilterCaps = 0x07030700;
    c.CubeTextureFilterCaps = 0x07030700;
    c.VolumeTextureFilterCaps = 0x03030300;
    c.TextureAddressCaps = 0x3f;
    c.VolumeTextureAddressCaps = 0x3f;
    c.LineCaps = 0x1f;
    c.MaxTextureWidth = c.MaxTextureHeight = 8192;
    c.MaxVolumeExtent = 2048;
    c.MaxTextureRepeat = 8192;
    c.MaxTextureAspectRatio = 8192;
    c.MaxAnisotropy = 16;
    c.MaxVertexW = 1e10f;
    c.GuardBandLeft = c.GuardBandTop = -1e8f;
    c.GuardBandRight = c.GuardBandBottom = 1e8f;
    c.StencilCaps = 0x1ff;
    c.FVFCaps = 0x00080008;
    c.TextureOpCaps = 0x03ffffff;
    c.MaxTextureBlendStages = 8;
    c.MaxSimultaneousTextures = 8;
    c.VertexProcessingCaps = 0x17b;
    c.MaxActiveLights = 8;
    c.MaxUserClipPlanes = 8;
    c.MaxVertexBlendMatrices = 4;
    c.MaxPointSize = 8192.0f;
    c.MaxPrimitiveCount = 0x00ffffff;
    c.MaxVertexIndex = 0x00ffffff;
    c.MaxStreams = 16;
    c.MaxStreamStride = 255;
    c.VertexShaderVersion = 0xfffe0300;
    c.MaxVertexShaderConst = 256;
    c.PixelShaderVersion = 0xffff0300;
    c.PixelShader1xMaxValue = 65504.0f;
    c.DevCaps2 = 0x51;                   /* STREAMOFFSET | CAN_STRETCHRECT_FROM_TEXTURES | VERTEXELEMENTSCANSHARESTREAMOFFSET */
    c.NumberOfAdaptersInGroup = 1;
    c.DeclTypes = 0x30f;                 /* UBYTE4, UBYTE4N, SHORT2N, SHORT4N, FLOAT16_2, FLOAT16_4 */
    c.NumSimultaneousRTs = 4;
    c.StretchRectFilterCaps = 0x03000300;
    c.VS20Caps = 1;
    c.VS20DynamicFlowControlDepth = 24;
    c.VS20NumTemps = 32;
    c.VS20StaticFlowControlDepth = 4;
    c.PS20Caps = 0x1f;
    c.PS20DynamicFlowControlDepth = 24;
    c.PS20NumTemps = 32;
    c.PS20StaticFlowControlDepth = 4;
    c.PS20NumInstructionSlots = 512;
    c.VertexTextureFilterCaps = 0x03000300;
    c.MaxVShaderInstructionsExecuted = 0xffff;
    c.MaxPShaderInstructionsExecuted = 0xffff;
    c.MaxVertexShader30InstructionSlots = 32768;
    c.MaxPixelShader30InstructionSlots = 32768;
    memcpy(p, &c, sizeof c);
}

/* ---- display modes ----------------------------------------------------------------------------- */
static const uint16_t std_modes[][2] = {{640, 480},   {800, 600},   {1024, 768},  {1152, 864},  {1280, 720},
                                        {1280, 800},  {1280, 960},  {1280, 1024}, {1366, 768},  {1440, 900},
                                        {1600, 900},  {1600, 1200}, {1680, 1050}, {1920, 1080}, {1920, 1200},
                                        {2560, 1440}, {2560, 1600}};
#define NSTD (sizeof std_modes / sizeof std_modes[0])
/* Modes in X8R8G8B8: the standard list up to the desktop size, plus the desktop mode itself. */
static unsigned mode_list(uint32_t out[][2]) {
    uint32_t w, h, hz;
    unsigned n = 0, have = 0;
    d3d9_current_mode(&w, &h, &hz);
    for (unsigned i = 0; i < NSTD; i++) {
        uint32_t mw = std_modes[i][0], mh = std_modes[i][1];
        if (mw > w || mh > h) continue;
        if (!have && (mw > w || (mw == w && mh >= h))) { /* keep the list sorted by (width, height) */
            have = 1;
            if (mw != w || mh != h) { out[n][0] = w; out[n][1] = h; n++; }
        }
        out[n][0] = mw;
        out[n][1] = mh;
        n++;
    }
    if (!have) { out[n][0] = w; out[n][1] = h; n++; }
    return n;
}
static void put_mode(uint8_t *mem, uint32_t a, uint32_t w, uint32_t h, uint32_t hz, uint32_t fmt) {
    sg_st32(mem, a, w);
    sg_st32(mem, a + 4, h);
    sg_st32(mem, a + 8, hz);
    sg_st32(mem, a + 12, fmt);
}

/* ---- IDirect3D9 -------------------------------------------------------------------------------- */
#define ARG(i) SG_COM_ARG(c, i)
static uint32_t d_register_sw(sg_com_call *c) { return D3DERR_NOTAVAILABLE; }
static uint32_t d_adapter_count(sg_com_call *c) { return 1; }
/* DEVIATIONS 3.1: the adapter identity the game sees, chosen once from SPORE_GPU_PROFILE (the bundle's
 * launcher sets it from `defaults write com.spore-native.player GPUProfile NAME`). All three are NVIDIA SM 3.0
 * parts listed in the game's VideoCards.txt; the driver (ForceWare 177.41, build 117741), the caps and the
 * 768 MB of texture memory stay the same, so every back end honours each one. Default: the 8800 GTX. */
static const struct { const char *key, *desc; uint32_t device; } k_gpu_profiles[] = {
    {"8800gtx", "NVIDIA GeForce 8800 GTX", 0x0191},
    {"9800gtx", "NVIDIA GeForce 9800 GTX", 0x0612},
    {"gtx280", "NVIDIA GeForce GTX 280", 0x05E1},
};
static unsigned gpu_profile(void) {
    static int sel = -1;
    if (sel < 0) {
        const char *s = getenv("SPORE_GPU_PROFILE");
        sel = 0;
        if (s && *s) {
            unsigned i;
            for (i = 0; i < sizeof k_gpu_profiles / sizeof k_gpu_profiles[0]; i++)
                if (!strcasecmp(s, k_gpu_profiles[i].key)) sel = (int)i;
            if (strcasecmp(s, k_gpu_profiles[sel].key))
                fprintf(stderr, "d3d9: SPORE_GPU_PROFILE=%s is not 8800gtx, 9800gtx or gtx280; using 8800gtx\n", s);
            else
                fprintf(stderr, "d3d9: GPU profile %s (%s, device 0x%04x)\n", k_gpu_profiles[sel].key,
                        k_gpu_profiles[sel].desc, k_gpu_profiles[sel].device);
        }
    }
    return (unsigned)sel;
}
void d3d9null_fill_identifier(uint8_t *mem, uint32_t p) {
    uint8_t *q = mem + p;
    unsigned g = gpu_profile();
    memset(q, 0, D3DADAPTER_IDENTIFIER9_SIZE);
    strcpy((char *)q, "nvd3dum.dll");                       /* Driver[512] */
    strcpy((char *)q + 512, k_gpu_profiles[g].desc);        /* Description[512] */
    strcpy((char *)q + 1024, "\\\\.\\DISPLAY1");             /* DeviceName[32] */
    sg_st32(mem, p + 1056, 0x000b1e3d);                     /* DriverVersion.LowPart: 11.7741 -> driverBuild 117741 */
    sg_st32(mem, p + 1060, 0x0006000e);                     /* HighPart: 6.14 */
    sg_st32(mem, p + 1064, 0x10de);                         /* VendorId: NVIDIA */
    sg_st32(mem, p + 1068, k_gpu_profiles[g].device);       /* DeviceId: 0x0191 GeForce 8800 GTX (VideoCards.txt) */
    sg_st32(mem, p + 1072, 0);
    sg_st32(mem, p + 1076, 0xa2);
    {
        static const uint8_t id[16] = {0xd7, 0xb7, 0x1e, 0x3e, 0x41, 0xd1, 0x11, 0xcf, 0x96, 0x0c, 0x00, 0x80, 0xc7, 0x23, 0x8e, 0x35};
        memcpy(q + 1080, id, 16);                           /* DeviceIdentifier: an arbitrary fixed GUID */
    }
    sg_st32(mem, p + 1096, 1);                              /* WHQLLevel */
}
static uint32_t d_adapter_identifier(sg_com_call *c) { /* (adapter, flags, D3DADAPTER_IDENTIFIER9*) */
    uint32_t p = ARG(2);
    if (ARG(0) != 0 || !p) return D3DERR_INVALIDCALL;
    d3d9null_fill_identifier(c->mem, p);
    return D3D_OK;
}
static uint32_t d_mode_count(sg_com_call *c) { /* (adapter, format) -> UINT */
    uint32_t m[NSTD + 1][2];
    if (ARG(0) != 0 || ARG(1) != D3DFMT_X8R8G8B8) return 0;
    return mode_list(m);
}
static uint32_t d_enum_modes(sg_com_call *c) { /* (adapter, format, mode, D3DDISPLAYMODE*) */
    uint32_t m[NSTD + 1][2], w, h, hz, n;
    if (ARG(0) != 0 || !ARG(3)) return D3DERR_INVALIDCALL;
    if (ARG(1) != D3DFMT_X8R8G8B8) return D3DERR_NOTAVAILABLE;
    n = mode_list(m);
    if (ARG(2) >= n) return D3DERR_INVALIDCALL;
    d3d9_current_mode(&w, &h, &hz);
    put_mode(c->mem, ARG(3), m[ARG(2)][0], m[ARG(2)][1], hz, D3DFMT_X8R8G8B8);
    return D3D_OK;
}
static uint32_t d_display_mode(sg_com_call *c) { /* (adapter, D3DDISPLAYMODE*) */
    uint32_t w, h, hz;
    if (ARG(0) != 0 || !ARG(1)) return D3DERR_INVALIDCALL;
    d3d9_current_mode(&w, &h, &hz);
    put_mode(c->mem, ARG(1), w, h, hz, D3DFMT_X8R8G8B8);
    return D3D_OK;
}
static uint32_t d_check_type(sg_com_call *c) { /* (adapter, devtype, adapterfmt, bbfmt, windowed) */
    uint32_t bb = ARG(3);
    if (ARG(0) != 0) return D3DERR_INVALIDCALL;
    if (ARG(1) != D3DDEVTYPE_HAL) return D3DERR_NOTAVAILABLE;
    if (!adapter_fmt_ok(ARG(2))) return D3DERR_NOTAVAILABLE;
    if (bb != D3DFMT_UNKNOWN && !rt_ok(bb)) return D3DERR_NOTAVAILABLE;
    return D3D_OK;
}
static uint32_t d_check_format(sg_com_call *c) { /* (adapter, devtype, adapterfmt, usage, rtype, checkfmt) */
    if (ARG(0) != 0) return D3DERR_INVALIDCALL;
    if (ARG(1) != D3DDEVTYPE_HAL) return D3DERR_NOTAVAILABLE;
    return d3d9_check_format(ARG(2), ARG(3), ARG(4), ARG(5));
}
static uint32_t d_check_ms(sg_com_call *c) { /* (adapter, devtype, fmt, windowed, mstype, DWORD *quality) */
    uint32_t ms = ARG(4), q = ARG(5);
    if (ARG(0) != 0) return D3DERR_INVALIDCALL;
    if (ARG(1) != D3DDEVTYPE_HAL) return D3DERR_NOTAVAILABLE;
    if (ms != 0 && ms != 1 && ms != 2 && ms != 4 && ms != 8) {
        if (q) sg_st32(c->mem, q, 0);
        return D3DERR_NOTAVAILABLE;
    }
    if (q) sg_st32(c->mem, q, ms == 1 ? 4 : 1);
    return D3D_OK;
}
static uint32_t d_check_ds_match(sg_com_call *c) { /* (adapter, devtype, adapterfmt, rtfmt, dsfmt) */
    if (ARG(0) != 0) return D3DERR_INVALIDCALL;
    if (ARG(1) != D3DDEVTYPE_HAL) return D3DERR_NOTAVAILABLE;
    return rt_ok(ARG(3)) && ds_ok(ARG(4)) ? D3D_OK : D3DERR_NOTAVAILABLE;
}
static uint32_t d_check_conversion(sg_com_call *c) { /* (adapter, devtype, src, dst) */
    if (ARG(0) != 0) return D3DERR_INVALIDCALL;
    return rt_ok(ARG(2)) && (ARG(3) == D3DFMT_X8R8G8B8 || ARG(3) == D3DFMT_A8R8G8B8) ? D3D_OK : D3DERR_NOTAVAILABLE;
}
static uint32_t d_device_caps(sg_com_call *c) { /* (adapter, devtype, D3DCAPS9*): HAL only, REF fails */
    if (ARG(0) != 0 || !ARG(2)) return D3DERR_INVALIDCALL;
    if (ARG(1) != D3DDEVTYPE_HAL) return D3DERR_NOTAVAILABLE;
    d3d9_fill_caps(SG_COM_PTR(c, ARG(2)), D3DDEVTYPE_HAL);
    return D3D_OK;
}
static uint32_t d_monitor(sg_com_call *c) { return ARG(0) == 0 ? D3D9NULL_HMONITOR : 0; }
static uint32_t d_create_device(sg_com_call *c) { /* (adapter, devtype, hFocus, flags, pp, ppDevice) */
    uint32_t r = d3d9_create_device(c, (d3d9_obj *)c->obj, ARG(0), ARG(1), ARG(2), ARG(3), ARG(4), ARG(5));
    if (r && sg_com_trace >= 1)
        fprintf(stderr, "d3d9: CreateDevice(adapter %u, type %u, focus 0x%x, flags 0x%x, pp hwnd 0x%x, %ux%u windowed %u) failed: 0x%08x\n",
                ARG(0), ARG(1), ARG(2), ARG(3), ARG(4) ? sg_ld32(c->mem, ARG(4) + 28) : 0, ARG(4) ? sg_ld32(c->mem, ARG(4)) : 0,
                ARG(4) ? sg_ld32(c->mem, ARG(4) + 4) : 0, ARG(4) ? sg_ld32(c->mem, ARG(4) + 32) : 0, r);
    return r;
}

static const sg_com_method d3d9_methods[] = {
    SG_COM_IUNKNOWN,
    {"RegisterSoftwareDevice", S_D3D9_RegisterSoftwareDevice, 8, d_register_sw},
    {"GetAdapterCount", S_D3D9_GetAdapterCount, 4, d_adapter_count},
    {"GetAdapterIdentifier", S_D3D9_GetAdapterIdentifier, 16, d_adapter_identifier},
    {"GetAdapterModeCount", S_D3D9_GetAdapterModeCount, 12, d_mode_count},
    {"EnumAdapterModes", S_D3D9_EnumAdapterModes, 20, d_enum_modes},
    {"GetAdapterDisplayMode", S_D3D9_GetAdapterDisplayMode, 12, d_display_mode},
    {"CheckDeviceType", S_D3D9_CheckDeviceType, 24, d_check_type},
    {"CheckDeviceFormat", S_D3D9_CheckDeviceFormat, 28, d_check_format},
    {"CheckDeviceMultiSampleType", S_D3D9_CheckDeviceMultiSampleType, 28, d_check_ms},
    {"CheckDepthStencilMatch", S_D3D9_CheckDepthStencilMatch, 24, d_check_ds_match},
    {"CheckDeviceFormatConversion", S_D3D9_CheckDeviceFormatConversion, 20, d_check_conversion},
    {"GetDeviceCaps", S_D3D9_GetDeviceCaps, 16, d_device_caps},
    {"GetAdapterMonitor", S_D3D9_GetAdapterMonitor, 8, d_monitor},
    {"CreateDevice", S_D3D9_CreateDevice, 28, d_create_device},
};
sg_com_iface d3d9_iface_d3d9 = {"IDirect3D9", {0x81bdcbca, 0x64d4, 0x426d, {0xae, 0x8d, 0xad, 0x01, 0x47, 0xf4, 0x27, 0x5c}},
                                D3D9_NSLOTS, d3d9_methods, SG_COM_N(d3d9_methods), d3d9_names, d3d9_args,
                                D3DERR_INVALIDCALL, &d3d9_lock, 0, 0};

static void d3d9_destroy(sg_com_obj *o) {
    sg_com_obj_retire(o);
    free(o);
}
static const sg_com_class d3d9_class = {"IDirect3D9", d3d9_destroy, NULL};

uint32_t d3d9null_create(sg_ctx *ctx, uint8_t *mem, uint32_t sdk) {
    d3d9_obj *d;
    uint32_t g;
    (void)ctx;
    sg_com_init(mem);
    if (sdk != 32 && sdk != (32u | 0x80000000u) && sg_com_trace >= 1)
        fprintf(stderr, "d3d9: Direct3DCreate9(SDK version %u): expected 32; continuing\n", sdk);
    d = calloc(1, sizeof *d);
    if (!d || sg_com_obj_init(&d->com, &d3d9_class)) {
        free(d);
        return 0;
    }
    g = sg_com_obj_expose(&d->com, &d3d9_iface_d3d9);
    if (!g) {
        sg_com_obj_retire(&d->com);
        free(d);
    }
    return g;
}

/* ---- what a second back end shares (d3d9null.h) --------------------------------------------- */
sg_com_handler *d3d9null_d3d9_handler(unsigned slot) {
    for (unsigned i = 0; i < SG_COM_N(d3d9_methods); i++)
        if (d3d9_methods[i].slot == slot) return d3d9_methods[i].fn;
    return NULL;
}
unsigned d3d9null_fmt_bits(uint32_t fmt, unsigned *block) { return d3d9_fmt_bits(fmt, block); }
pthread_mutex_t *d3d9null_device_lock(void) { return &d3d9_lock; }
d3d9null_stats *d3d9null_stats_ptr(void) { return &d3d9_stats; }
/* The Metal back end (runtime/d3d9metal) defines the strong one; without it SPORE_D3D9=metal falls back. */
__attribute__((weak)) uint32_t d3d9metal_create(sg_ctx *ctx, uint8_t *mem, uint32_t sdk) {
    (void)ctx; (void)mem; (void)sdk;
    return 0;
}
/* IDirect3D9 *WINAPI Direct3DCreate9(UINT SDKVersion) -- strong; replaces hle_boot_stops.c's weak placeholder.
 * The back end is chosen once, from SPORE_D3D9 (null | metal). Unset means metal when the Metal back end
 * is linked (the player default since 2026-10-13), else null (the test binaries). */
extern void hle_stop_check(sg_ctx *ctx, uint8_t *mem, const char *import);
SG_IMPORT(d3d9, Direct3DCreate9) {
    static int backend = -1; /* 0 null, 2 metal, 3 unset */
    uint32_t g = 0;
    hle_stop_check(ctx, mem, "Direct3DCreate9");
    if (backend < 0) {
        const char *s = getenv("SPORE_D3D9");
        backend = !s || !*s ? 3 : !strcmp(s, "metal") ? 2 : 0;
        if (s && *s && strcmp(s, "metal") && strcmp(s, "null"))
            fprintf(stderr, "d3d9: SPORE_D3D9=%s is not null or metal; using the null device\n", s);
    }
    if (backend == 3) { /* unset: the Metal back end if it is linked, quietly the null device otherwise */
        g = d3d9metal_create(ctx, mem, SG_ARG(0));
        if (g) SG_RETURN_STDCALL(g, 4);
        backend = 0;
    }
    if (backend == 2) {
        g = d3d9metal_create(ctx, mem, SG_ARG(0));
        if (g) SG_RETURN_STDCALL(g, 4);
        fprintf(stderr, "d3d9: SPORE_D3D9=metal, but the Metal back end is not linked or failed to start; using the null device\n");
        backend = 0;
    }
    g = d3d9null_create(ctx, mem, SG_ARG(0));
    if (sg_com_trace >= 1)
        fprintf(stderr, "d3d9: Direct3DCreate9(%u) = 0x%08x (null device, NVIDIA GeForce 8800 GTX profile)\n", SG_ARG(0), g);
    SG_RETURN_STDCALL(g, 4);
}

void d3d9null_get_stats(d3d9null_stats *s) { *s = d3d9_stats; }

/* ---- the three D3DX imports that only depend on the device profile ---------------------------- */
/* BOOL D3DXCheckVersion(UINT D3DSdkVersion, UINT D3DXSdkVersion): TRUE for the versions this exe was
 * built with (32, 27 for d3dx9_27.dll). [obs] With it returning 0 the game shows its
 * "DirectX" message box right after Direct3DCreate9 and exits. */
SG_IMPORT(d3dx9_27, D3DXCheckVersion) {
    uint32_t ok = SG_ARG(0) == 32 && SG_ARG(1) == 27;
    if (!ok && sg_com_trace >= 1) fprintf(stderr, "d3d9: D3DXCheckVersion(%u, %u): unexpected versions\n", SG_ARG(0), SG_ARG(1));
    SG_RETURN_STDCALL(ok, 8);
}
static uint32_t profile_string(const char *s) {
    uint32_t g = sg_com_galloc((uint32_t)strlen(s) + 1);
    if (g) memcpy(sg_com_mem + g, s, strlen(s) + 1);
    return g;
}
/* LPCSTR D3DXGetPixelShaderProfile(LPDIRECT3DDEVICE9) / D3DXGetVertexShaderProfile: the highest
 * profile the device supports; SM 3.0 per the caps above. */
SG_IMPORT(d3dx9_27, D3DXGetPixelShaderProfile) {
    static uint32_t s;
    sg_com_init(mem);
    if (!s) s = profile_string("ps_3_0");
    SG_RETURN_STDCALL(SG_ARG(0) ? s : 0, 4);
}
SG_IMPORT(d3dx9_27, D3DXGetVertexShaderProfile) {
    static uint32_t s;
    sg_com_init(mem);
    if (!s) s = profile_string("vs_3_0");
    SG_RETURN_STDCALL(SG_ARG(0) ? s : 0, 4);
}
