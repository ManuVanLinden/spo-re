// test_conformance -- the Metal back end's Direct3D 9 conformance test.
// It exercises the D3D9 features Spore depends on: SM3 shaders (vPos, rep, ifc/else),
// vertex texture fetch, SetStreamSourceFreq instancing, DXT1/3/5 textures, a dynamic VB ring with NOOVERWRITE/DISCARD
// locks, D24S8 depth and a depth texture, z functions, a viewport-limited Clear, event-query flushing, render-target
// readback through GetRenderTargetData into a SYSTEMMEM surface (the game's path), the game's GetBackBuffer +
// StretchRect path, Present and Reset; then the sampling cases (sampling_tests) (cube face edge, CLAMP/WRAP edges, anisotropic
// filtering of a single-level texture, a grazing ground plane) and the stream and depth cases (stream_depth_tests: vertex streams without a buffer,
// depth clipping). It contains no game code or data.
//
// Written the way a C++ Direct3D 9 application is, against our own declarations (d3d9_host.h); conformance_shim.cpp
// turns every call into a call through the guest COM vtables of the Metal back end, the path translated game code
// takes. The window is tests/test_window.m's (a CAMetalLayer the back end presents into).
// Usage: test_conformance <out dir>   (writes results.json, frame.bmp, sampling.bmp and stream_depth.bmp there)

#include "d3d9_host.h"

#include <algorithm>
#include <chrono>
#include <cstdarg>
#include <cmath>
#include <cstdio>
#include <cstring>
#include <set>
#include <string>
#include <vector>
#include <unistd.h>

#include "test_shaders.h"
#include "testutil.h"

extern "C" {
void test_app_init(void);
void test_window_size(int w, int h);
void test_window_close(void);
void test_pump(void);
}

static const int W = 512, H = 256;

struct Result { std::string name; bool pass; std::string detail; };
static std::vector<Result> g_results;
static std::vector<std::string> g_info;

static void record(const std::string &n, bool p, const std::string &d) {
  g_results.push_back({n, p, d});
  printf("TEST %-28s %s  %s\n", n.c_str(), p ? "PASS" : "FAIL", d.c_str());
  fflush(stdout);
}
static void info(const std::string &s) { g_info.push_back(s); printf("INFO %s\n", s.c_str()); fflush(stdout); }

static std::string fmt(const char *f, ...) {
  char b[1024]; va_list ap; va_start(ap, f); vsnprintf(b, sizeof b, f, ap); va_end(ap); return b;
}

#define CHECK_HR(expr) do { HRESULT _hr = (expr); if (FAILED(_hr)) { \
  fprintf(stderr, "FAILED 0x%08x: %s (line %d)\n", (unsigned)_hr, #expr, __LINE__); return false; } } while (0)

struct Vtx { float x, y, z, w; float u, v; D3DCOLOR c; };

// Dynamic VB ring used exactly the way RW's driver does it (0x11f57d0): NOOVERWRITE appends, DISCARD on wrap.
struct Ring {
  IDirect3DVertexBuffer9 *vb = nullptr;
  UINT cap = 0, off = 0, discards = 0, locks = 0;
  std::set<uintptr_t> discardPtrs;
  bool init(IDirect3DDevice9 *d, UINT bytes) {
    cap = bytes;
    return SUCCEEDED(d->CreateVertexBuffer(bytes, D3DUSAGE_WRITEONLY | D3DUSAGE_DYNAMIC, 0, D3DPOOL_DEFAULT, &vb, nullptr));
  }
  // Copies n vertices in and returns the first vertex index, or -1.
  int push(const Vtx *v, UINT n) {
    UINT sz = n * sizeof(Vtx);
    DWORD flags = D3DLOCK_NOOVERWRITE;
    if (off + sz > cap) { off = 0; flags = D3DLOCK_DISCARD; discards++; }
    void *p = nullptr;
    if (FAILED(vb->Lock(off, sz, &p, flags))) return -1;
    locks++;
    if (flags == D3DLOCK_DISCARD) discardPtrs.insert((uintptr_t)p);
    memcpy(p, v, sz);
    vb->Unlock();
    int first = off / sizeof(Vtx);
    off += sz;
    return first;
  }
};

static void quad(Vtx out[4], float x0, float y0, float x1, float y1, float z, float u0, float v0, float u1, float v1, D3DCOLOR c) {
  // D3D9 pixel centres are at integer coordinates: shift by -0.5 so [x0,x1) covers whole pixels.
  float X0 = x0 - 0.5f, X1 = x1 - 0.5f, Y0 = y0 - 0.5f, Y1 = y1 - 0.5f;
  out[0] = {X0, Y0, z, 1, u0, v0, c};
  out[1] = {X1, Y0, z, 1, u1, v0, c};
  out[2] = {X0, Y1, z, 1, u0, v1, c};
  out[3] = {X1, Y1, z, 1, u1, v1, c};
}

struct Img {
  std::vector<uint8_t> px; int w = 0, h = 0;  // BGRA
  const uint8_t *at(int x, int y) const { return &px[((size_t)y * w + x) * 4]; }
};

static bool readback(IDirect3DDevice9 *dev, IDirect3DSurface9 *rt, Img &img) {
  D3DSURFACE_DESC d; CHECK_HR(rt->GetDesc(&d));
  IDirect3DSurface9 *sys = nullptr;
  CHECK_HR(dev->CreateOffscreenPlainSurface(d.Width, d.Height, d.Format, D3DPOOL_SYSTEMMEM, &sys, nullptr));
  CHECK_HR(dev->GetRenderTargetData(rt, sys));
  D3DLOCKED_RECT lr; CHECK_HR(sys->LockRect(&lr, nullptr, D3DLOCK_READONLY));
  img.w = d.Width; img.h = d.Height; img.px.resize((size_t)d.Width * d.Height * 4);
  for (UINT y = 0; y < d.Height; y++) memcpy(&img.px[(size_t)y * d.Width * 4], (uint8_t *)lr.pBits + (size_t)y * lr.Pitch, d.Width * 4);
  sys->UnlockRect();
  sys->Release();
  return true;
}

// Compare pixel (x,y) to RGBA within tol. Returns max abs difference.
static int diff(const Img &im, int x, int y, int r, int g, int b, int a) {
  const uint8_t *p = im.at(x, y);
  int d = std::abs(p[2] - r);
  d = std::max(d, std::abs(p[1] - g)); d = std::max(d, std::abs(p[0] - b));
  if (a >= 0) d = std::max(d, std::abs(p[3] - a));
  return d;
}
static std::string pxs(const Img &im, int x, int y) {
  const uint8_t *p = im.at(x, y);
  return fmt("(%d,%d)=rgba(%d,%d,%d,%d)", x, y, p[2], p[1], p[0], p[3]);
}

static D3DCOLOR inst_color(int i) { return D3DCOLOR_ARGB(255, 30 * i + 20, 255 - 25 * i, (i * 97) & 255); }
static D3DCOLOR ring_color(int i) { return D3DCOLOR_ARGB(255, (i * 8) & 255, 255 - (i * 5), (i * 53) & 255); }

static std::vector<uint8_t> g_dxt[3];  // block data for DXT1/3/5
static const D3DFORMAT kDxtFmt[3] = {D3DFMT_DXT1, D3DFMT_DXT3, D3DFMT_DXT5};
static const int kDxtNum[3] = {1, 3, 5};


// ---- sampling cases (group "sampling"), rendered into `target` (W x H) and saved as sampling.bmp ----------------
// W1 cube.face_edge: a 4x4 cube whose +Z face is black and whose -X face is white, sampled bilinearly next to the edge
//    between them. D3D9-era hardware filters inside one face (black); a seamless filter blends the white face in. Metal
//    filters seamlessly on Apple GPUs, so the test checks the controls and reports the mode.
// W2 ground.grazing: a mipmapped noise texture on a ground plane seen at a grazing angle, with the sampler states
//    RenderWare sets (MIN/MAG ANISOTROPIC, MIP LINEAR, MAXANISOTROPY 16, SetLOD 0). No analytic answer: the test checks
//    that it is drawn and reports the edge energy per band (how sharp the far ground stays) for comparison between runs.
// W4 sampler.aniso_single_level (a haze in the main menu's top band): the menu's post-process passes sample single-level render targets with
//    MIN/MAG ANISOTROPIC (RenderWare's flush, 0x11f1a30) and MAXANISOTROPY 8 (set at start-up). A texture
//    with one mip level is filtered bilinearly (no anisotropy). A 256x64 single-level texture of random texels
//    drawn into 32x32 pixels (8x across, 2x down) must equal one bilinear tap per pixel. Exact (tolerance 2).
// W3 sampler.edge: an 8x8 texture read at half-texel boundaries 1.5 texels past its top-left edge (the menu blur's
//    taps), CLAMP and WRAP, against a CPU bilinear reference. Exact (8-bit filtering, tolerance 2).
struct VtxC { float x, y, z, w; float dx, dy, dz; D3DCOLOR c; };
static bool sampling_tests(IDirect3DDevice9 *dev, const char *outdir, IDirect3DVertexShader9 *vsBasic, IDirect3DPixelShader9 *psTex,
                       IDirect3DSurface9 *target) {
  IDirect3DPixelShader9 *psCube = nullptr;
  CHECK_HR(dev->CreatePixelShader((const DWORD *)k_ps_cube, &psCube));
  D3DVERTEXELEMENT9 eB[] = {
      {0, 0, D3DDECLTYPE_FLOAT4, D3DDECLMETHOD_DEFAULT, D3DDECLUSAGE_POSITION, 0},
      {0, 16, D3DDECLTYPE_FLOAT2, D3DDECLMETHOD_DEFAULT, D3DDECLUSAGE_TEXCOORD, 0},
      {0, 24, D3DDECLTYPE_D3DCOLOR, D3DDECLMETHOD_DEFAULT, D3DDECLUSAGE_COLOR, 0}, D3DDECL_END()};
  D3DVERTEXELEMENT9 eC[] = {
      {0, 0, D3DDECLTYPE_FLOAT4, D3DDECLMETHOD_DEFAULT, D3DDECLUSAGE_POSITION, 0},
      {0, 16, D3DDECLTYPE_FLOAT3, D3DDECLMETHOD_DEFAULT, D3DDECLUSAGE_TEXCOORD, 0},
      {0, 28, D3DDECLTYPE_D3DCOLOR, D3DDECLMETHOD_DEFAULT, D3DDECLUSAGE_COLOR, 0}, D3DDECL_END()};
  IDirect3DVertexDeclaration9 *declB = nullptr, *declC = nullptr;
  CHECK_HR(dev->CreateVertexDeclaration(eB, &declB));
  CHECK_HR(dev->CreateVertexDeclaration(eC, &declC));
  uint32_t seed = 0x37370001;

  // W1: the cube. Faces +X 0, -X 1, +Y 2, -Y 3, +Z 4, -Z 5.
  IDirect3DCubeTexture9 *cube = nullptr;
  CHECK_HR(dev->CreateCubeTexture(4, 1, 0, D3DFMT_A8R8G8B8, D3DPOOL_MANAGED, &cube, nullptr));
  for (int f = 0; f < 6; f++) {
    D3DLOCKED_RECT lr; CHECK_HR(cube->LockRect((D3DCUBEMAP_FACES)f, 0, &lr, nullptr, 0));
    uint32_t c = f == 4 ? 0xff000000u : f == 1 ? 0xffffffffu : 0xff808080u;
    for (int y = 0; y < 4; y++) for (int x = 0; x < 4; x++) ((uint32_t *)((uint8_t *)lr.pBits + y * lr.Pitch))[x] = c;
    cube->UnlockRect((D3DCUBEMAP_FACES)f, 0);
  }
  // W2: 256x256 noise, full chain by a 2x2 box filter.
  IDirect3DTexture9 *ground = nullptr;
  CHECK_HR(dev->CreateTexture(256, 256, 0, 0, D3DFMT_A8R8G8B8, D3DPOOL_MANAGED, &ground, nullptr));
  {
    std::vector<uint8_t> lv((size_t)256 * 256 * 4);
    for (size_t i = 0; i < lv.size(); i++) lv[i] = (i & 3) == 3 ? 255 : (uint8_t)(64 + (tu_rand(&seed) & 127));
    int n = 256;
    for (DWORD l = 0; l < ground->GetLevelCount(); l++) {
      D3DLOCKED_RECT lr; CHECK_HR(ground->LockRect(l, &lr, nullptr, 0));
      for (int y = 0; y < n; y++) memcpy((uint8_t *)lr.pBits + y * lr.Pitch, &lv[(size_t)y * n * 4], (size_t)n * 4);
      ground->UnlockRect(l);
      if (n == 1) break;
      int m = n / 2;
      std::vector<uint8_t> nx((size_t)m * m * 4);
      for (int y = 0; y < m; y++) for (int x = 0; x < m; x++) for (int k = 0; k < 4; k++)
        nx[((size_t)y * m + x) * 4 + k] = (uint8_t)((lv[((size_t)(2 * y) * n + 2 * x) * 4 + k] + lv[((size_t)(2 * y) * n + 2 * x + 1) * 4 + k] +
                                                     lv[((size_t)(2 * y + 1) * n + 2 * x) * 4 + k] + lv[((size_t)(2 * y + 1) * n + 2 * x + 1) * 4 + k] + 2) / 4);
      lv.swap(nx); n = m;
    }
    ground->SetLOD(0);
  }
  // W3: 8x8 random texels.
  IDirect3DTexture9 *edge = nullptr;
  static uint8_t g_edge[8 * 8 * 4];
  for (auto &b : g_edge) b = (uint8_t)tu_rand(&seed);
  for (int i = 3; i < 256; i += 4) g_edge[i] = 255;
  CHECK_HR(dev->CreateTexture(8, 8, 1, 0, D3DFMT_A8R8G8B8, D3DPOOL_MANAGED, &edge, nullptr));
  {
    D3DLOCKED_RECT lr; CHECK_HR(edge->LockRect(0, &lr, nullptr, 0));
    for (int y = 0; y < 8; y++) memcpy((uint8_t *)lr.pBits + y * lr.Pitch, g_edge + y * 32, 32);
    edge->UnlockRect(0);
  }

  // W4: 256x64 random texels, one level
  IDirect3DTexture9 *one = nullptr;
  static uint8_t g_one[256 * 64 * 4];
  for (auto &b : g_one) b = (uint8_t)tu_rand(&seed);
  for (int i = 3; i < (int)sizeof g_one; i += 4) g_one[i] = 255;
  CHECK_HR(dev->CreateTexture(256, 64, 1, 0, D3DFMT_A8R8G8B8, D3DPOOL_MANAGED, &one, nullptr));
  {
    D3DLOCKED_RECT lr; CHECK_HR(one->LockRect(0, &lr, nullptr, 0));
    for (int y = 0; y < 64; y++) memcpy((uint8_t *)lr.pBits + y * lr.Pitch, g_one + y * 1024, 1024);
    one->UnlockRect(0);
  }

  CHECK_HR(dev->SetRenderTarget(0, target));
  dev->SetDepthStencilSurface(nullptr);
  CHECK_HR(dev->BeginScene());
  CHECK_HR(dev->Clear(0, nullptr, D3DCLEAR_TARGET, 0xff000000, 1.0f, 0));
  float c0[4] = {2.0f / W, -2.0f / H, -1.0f, 1.0f};
  dev->SetVertexShaderConstantF(0, c0, 1);
  dev->SetVertexShader(vsBasic);
  dev->SetRenderState(D3DRS_ZENABLE, FALSE);
  dev->SetRenderState(D3DRS_CULLMODE, D3DCULL_NONE);
  dev->SetRenderState(D3DRS_ALPHABLENDENABLE, FALSE);
  for (int s = 0; s < 1; s++) {
    dev->SetSamplerState(s, D3DSAMP_ADDRESSU, D3DTADDRESS_WRAP); dev->SetSamplerState(s, D3DSAMP_ADDRESSV, D3DTADDRESS_WRAP);
    dev->SetSamplerState(s, D3DSAMP_ADDRESSW, D3DTADDRESS_WRAP);
  }
  bool drawOk = true;
  // W1 at (0,0)-(64,64): directions (-1..-0.5, 0.25..-0.25, 1): u of +Z from 0 to 0.25 (one texel), v 0.375..0.625.
  // (64,0)-(96,32): the centre of -X (white); (96,0)-(128,32): the centre of +Z (black).
  {
    dev->SetVertexDeclaration(declC); dev->SetPixelShader(psCube); dev->SetTexture(0, cube);
    dev->SetSamplerState(0, D3DSAMP_MINFILTER, D3DTEXF_LINEAR); dev->SetSamplerState(0, D3DSAMP_MAGFILTER, D3DTEXF_LINEAR);
    dev->SetSamplerState(0, D3DSAMP_MIPFILTER, D3DTEXF_NONE);
    auto cq = [&](float x0, float y0, float x1, float y1, float dxl, float dxr, float dyt, float dyb, float dz) {
      VtxC v[4] = {{x0 - 0.5f, y0 - 0.5f, 0.5f, 1, dxl, dyt, dz, 0}, {x1 - 0.5f, y0 - 0.5f, 0.5f, 1, dxr, dyt, dz, 0},
                   {x0 - 0.5f, y1 - 0.5f, 0.5f, 1, dxl, dyb, dz, 0}, {x1 - 0.5f, y1 - 0.5f, 0.5f, 1, dxr, dyb, dz, 0}};
      return SUCCEEDED(dev->DrawPrimitiveUP(D3DPT_TRIANGLESTRIP, 2, v, sizeof(VtxC)));
    };
    drawOk &= cq(0, 0, 64, 64, -1.0f, -0.5f, 0.25f, -0.25f, 1.0f);
    VtxC w[4] = {{63.5f, -0.5f, 0.5f, 1, -1, 0.1f, 0.1f, 0}, {95.5f, -0.5f, 0.5f, 1, -1, 0.1f, -0.1f, 0},
                 {63.5f, 31.5f, 0.5f, 1, -1, -0.1f, 0.1f, 0}, {95.5f, 31.5f, 0.5f, 1, -1, -0.1f, -0.1f, 0}};
    drawOk &= SUCCEEDED(dev->DrawPrimitiveUP(D3DPT_TRIANGLESTRIP, 2, w, sizeof(VtxC)));
    drawOk &= cq(96, 0, 128, 32, -0.1f, 0.1f, 0.1f, -0.1f, 1.0f);
    dev->SetTexture(0, nullptr);
  }
  // W3 at (128,0)-(136,8) CLAMP and (144,0)-(152,8) WRAP: pixel i samples texel boundary i - 1.5.
  {
    dev->SetVertexDeclaration(declB); dev->SetPixelShader(psTex); dev->SetTexture(0, edge);
    dev->SetSamplerState(0, D3DSAMP_MINFILTER, D3DTEXF_LINEAR); dev->SetSamplerState(0, D3DSAMP_MAGFILTER, D3DTEXF_LINEAR);
    dev->SetSamplerState(0, D3DSAMP_MIPFILTER, D3DTEXF_NONE);
    for (int m = 0; m < 2; m++) {
      DWORD a = m ? D3DTADDRESS_WRAP : D3DTADDRESS_CLAMP;
      dev->SetSamplerState(0, D3DSAMP_ADDRESSU, a); dev->SetSamplerState(0, D3DSAMP_ADDRESSV, a);
      Vtx q[4]; float o = -1.5f / 8;
      quad(q, 128 + 16 * m, 0, 136 + 16 * m, 8, 0.5f, o, o, 1 + o, 1 + o, 0);
      drawOk &= SUCCEEDED(dev->DrawPrimitiveUP(D3DPT_TRIANGLESTRIP, 2, q, sizeof(Vtx)));
    }
    dev->SetSamplerState(0, D3DSAMP_ADDRESSU, D3DTADDRESS_WRAP); dev->SetSamplerState(0, D3DSAMP_ADDRESSV, D3DTADDRESS_WRAP);
    dev->SetTexture(0, nullptr);
  }
  // W4 at (160,0)-(192,32): one level, ANISOTROPIC min/mag, MAXANISOTROPY 8, MIPFILTER LINEAR (as the post passes)
  {
    dev->SetVertexDeclaration(declB); dev->SetPixelShader(psTex); dev->SetTexture(0, one);
    dev->SetSamplerState(0, D3DSAMP_ADDRESSU, D3DTADDRESS_CLAMP); dev->SetSamplerState(0, D3DSAMP_ADDRESSV, D3DTADDRESS_CLAMP);
    dev->SetSamplerState(0, D3DSAMP_MINFILTER, D3DTEXF_ANISOTROPIC); dev->SetSamplerState(0, D3DSAMP_MAGFILTER, D3DTEXF_ANISOTROPIC);
    dev->SetSamplerState(0, D3DSAMP_MIPFILTER, D3DTEXF_LINEAR); dev->SetSamplerState(0, D3DSAMP_MAXANISOTROPY, 8);
    Vtx q[4];
    quad(q, 160, 0, 192, 32, 0.5f, 0, 0, 1, 1, 0);
    drawOk &= SUCCEEDED(dev->DrawPrimitiveUP(D3DPT_TRIANGLESTRIP, 2, q, sizeof(Vtx)));
    dev->SetSamplerState(0, D3DSAMP_ADDRESSU, D3DTADDRESS_WRAP); dev->SetSamplerState(0, D3DSAMP_ADDRESSV, D3DTADDRESS_WRAP);
    dev->SetSamplerState(0, D3DSAMP_MAXANISOTROPY, 1);
    dev->SetTexture(0, nullptr);
  }
  // W2 at rows 64..256: a ground plane, far edge y 96 (x 128..384, w 8), near edge y 256 (x 0..512, w 1).
  {
    dev->SetVertexDeclaration(declB); dev->SetPixelShader(psTex); dev->SetTexture(0, ground);
    dev->SetSamplerState(0, D3DSAMP_MINFILTER, D3DTEXF_ANISOTROPIC); dev->SetSamplerState(0, D3DSAMP_MAGFILTER, D3DTEXF_ANISOTROPIC);
    dev->SetSamplerState(0, D3DSAMP_MIPFILTER, D3DTEXF_LINEAR); dev->SetSamplerState(0, D3DSAMP_MAXANISOTROPY, 16);
    dev->SetSamplerState(0, D3DSAMP_MAXMIPLEVEL, 0); dev->SetSamplerState(0, D3DSAMP_MIPMAPLODBIAS, 0);
    auto gv = [&](float sx, float sy, float w, float u, float v) {
      float nx = 2 * sx / W - 1, ny = 1 - 2 * sy / H;     /* the NDC wanted after the divide by w */
      Vtx r = {(nx * w + 1) * W / 2, (1 - ny * w) * H / 2, 0.5f * w, w, u, v, 0};   /* texcoords: perspective-correct */
      return r;
    };
    Vtx g[4] = {gv(128 - 0.5f, 96 - 0.5f, 8, -8, 16), gv(384 - 0.5f, 96 - 0.5f, 8, 8, 16), gv(-0.5f, 256 - 0.5f, 1, -2, 2),
                gv(512 - 0.5f, 256 - 0.5f, 1, 2, 2)};
    drawOk &= SUCCEEDED(dev->DrawPrimitiveUP(D3DPT_TRIANGLESTRIP, 2, g, sizeof(Vtx)));
    dev->SetSamplerState(0, D3DSAMP_MINFILTER, D3DTEXF_POINT); dev->SetSamplerState(0, D3DSAMP_MAGFILTER, D3DTEXF_POINT);
    dev->SetSamplerState(0, D3DSAMP_MIPFILTER, D3DTEXF_NONE); dev->SetSamplerState(0, D3DSAMP_MAXANISOTROPY, 1);
    dev->SetTexture(0, nullptr);
  }
  CHECK_HR(dev->EndScene());
  record("sampling.draws", drawOk, "cube face edge, sampler edge, grazing ground (DrawPrimitiveUP)");

  Img im;
  if (!readback(dev, target, im)) { record("sampling.readback", false, ""); return false; }
  std::string bmp = std::string(outdir) + "/sampling.bmp";
  tu_write_bmp(bmp.c_str(), im.px.data(), W, H, W * 4);
  // W1
  {
    int mx = 0, mxx = 0;
    for (int y = 0; y < 64; y++) for (int x = 0; x < 64; x++) {
      int v = std::max({(int)im.at(x, y)[0], (int)im.at(x, y)[1], (int)im.at(x, y)[2]});
      if (v > mx) { mx = v; mxx = x; }
    }
    bool ctrl = diff(im, 80, 16, 255, 255, 255, -1) <= 2 && diff(im, 112, 16, 0, 0, 0, -1) <= 2;
    const char *mode = mx <= 2 ? "per face, as D3D9-era hardware" : "seamless (blends across the face edge)";
    info(fmt("sampling cube filtering at a face edge: %s; brightest edge pixel %d at x %d", mode, mx, mxx));
    record("cube.face_edge", ctrl && (mx <= 2 || mx >= 100), fmt("%s (edge pixel %d); controls %s %s", mode, mx,
                                                               pxs(im, 80, 16).c_str(), pxs(im, 112, 16).c_str()));
  }
  // W3
  {
    int bad = 0, worst = 0; std::string ex;
    for (int m = 0; m < 2; m++)
      for (int py = 0; py < 8; py++) for (int px = 0; px < 8; px++) {
        auto ix = [&](int i) { return m ? (i + 8) % 8 : std::min(7, std::max(0, i)); };
        int x0 = ix(px - 2), x1 = ix(px - 1), y0 = ix(py - 2), y1 = ix(py - 1);
        for (int k = 0; k < 3; k++) {
          int want = (g_edge[(y0 * 8 + x0) * 4 + k] + g_edge[(y0 * 8 + x1) * 4 + k] + g_edge[(y1 * 8 + x0) * 4 + k] +
                      g_edge[(y1 * 8 + x1) * 4 + k] + 2) / 4;
          int got = im.at(128 + 16 * m + px, py)[k];
          int d = std::abs(got - want);
          if (d > worst) worst = d;
          if (d > 2) { if (!bad) ex = fmt("%s (%d,%d) channel %d: %d, want %d", m ? "WRAP" : "CLAMP", px, py, k, got, want); bad++; }
        }
      }
    record("sampler.edge_clamp_wrap", bad == 0, fmt("%d of 384 channels off by more than 2 (largest %d) %s", bad, worst, ex.c_str()));
  }
  // W4: pixel (i,j) centre -> texel x 8i+3.5, y 2j+0.5: the average of texels (8i+3..8i+4, 2j..2j+1)
  {
    int bad = 0, worst = 0; std::string ex;
    for (int j = 0; j < 32; j++) for (int i = 0; i < 32; i++) for (int k = 0; k < 3; k++) {
      auto T = [&](int x, int y) { return (int)g_one[(y * 256 + x) * 4 + k]; };
      int want = (T(8 * i + 3, 2 * j) + T(8 * i + 4, 2 * j) + T(8 * i + 3, 2 * j + 1) + T(8 * i + 4, 2 * j + 1) + 2) / 4;
      int d = std::abs((int)im.at(160 + i, j)[k] - want);
      if (d > worst) worst = d;
      if (d > 2) { if (!bad) ex = fmt("(%d,%d) channel %d: %d, want %d", i, j, k, (int)im.at(160 + i, j)[k], want); bad++; }
    }
    record("sampler.aniso_single_level", bad == 0, fmt("%d of 3072 channels differ from one bilinear tap by more than 2 (largest %d) %s",
                                                       bad, worst, ex.c_str()));
  }
  // W2: edge energy per band (far-ground sharpness), reported; drawn = not black
  {
    std::string bands;
    bool drawn = true;
    for (int y0 : {100, 130, 170, 220}) {
      double e = 0; long n = 0; int lum = 0;
      for (int y = y0; y < y0 + 20; y++) for (int x = 140; x < 372; x++) {
        auto L = [&](int xx, int yy) { const uint8_t *p = im.at(xx, yy); return (p[0] + 2 * p[1] + p[2]) / 4; };
        e += std::abs(L(x + 1, y) - L(x, y)) + std::abs(L(x, y + 1) - L(x, y)); n++; lum += L(x, y) > 8;
      }
      drawn &= lum > 0;
      bands += fmt(" y%d %.2f", y0, e / n);
    }
    info("sampling ground edge energy per band:" + bands);
    record("ground.grazing", drawn, "drawn; edge energy per band" + bands);
  }
  psCube->Release(); declB->Release(); declC->Release(); cube->Release(); ground->Release(); edge->Release(); one->Release();
  return true;
}

// ---- vertex streams without a buffer, and depth clipping (group "stream_depth") ---------------------------------
// Drawn into `target` (W x H, cleared to grey 64) and saved as stream_depth.bmp; frame.bmp is not touched. `groups` selects
// them: "vb", "depth", or anything else (the default, "all") for both; the environment variable TEST_CONFORMANCE_CASES overrides it.
// V1 vb.stream_unset (0,0)-(64,64): the declaration takes COLOR from stream 1, which has no buffer. Expect the
//    draw to happen with COLOR = 0 (a zero-filled element, D3DCOLOR (0,0,0,0)), which is what D3D9 drivers give.
// V2 vb.decl_missing_input (64,0)-(128,64): the declaration has no COLOR although vs_basic reads it; such an input
//    reads zero. Expect COLOR = (0,0,0,0).
// V3 vb.stream0_unset_after_up (128,0)-(192,64): DrawPrimitiveUP (red) leaves stream 0 unset (D3D9 semantics);
//    a DrawPrimitive right after it then reads positions from no buffer (all zero: degenerate, nothing drawn).
//    Expect the red UP quad and no crash.
// D1 depth.clip_far (192,0)-(256,64): z = 1.5 (behind the far plane), ZENABLE FALSE. D3D9 always clips: grey.
// D2 depth.clip_near (256,0)-(320,64): z = -0.5 (in front of the near plane). Expect grey.
// D3 depth.clip_control (320,0)-(384,64): z = 0.99, drawn green.
static bool stream_depth_tests(IDirect3DDevice9 *dev, const char *outdir, IDirect3DVertexShader9 *vsBasic, IDirect3DPixelShader9 *psColor,
                       IDirect3DVertexDeclaration9 *declBasic, IDirect3DSurface9 *target, const char *groups) {
  bool vb = !strcmp(groups, "vb") || strcmp(groups, "depth");
  bool depth = !strcmp(groups, "depth") || strcmp(groups, "vb");
  D3DVERTEXELEMENT9 eColor1[] = {
      {0, 0, D3DDECLTYPE_FLOAT4, D3DDECLMETHOD_DEFAULT, D3DDECLUSAGE_POSITION, 0},
      {0, 16, D3DDECLTYPE_FLOAT2, D3DDECLMETHOD_DEFAULT, D3DDECLUSAGE_TEXCOORD, 0},
      {1, 0, D3DDECLTYPE_D3DCOLOR, D3DDECLMETHOD_DEFAULT, D3DDECLUSAGE_COLOR, 0}, D3DDECL_END()};
  D3DVERTEXELEMENT9 eNoColor[] = {
      {0, 0, D3DDECLTYPE_FLOAT4, D3DDECLMETHOD_DEFAULT, D3DDECLUSAGE_POSITION, 0},
      {0, 16, D3DDECLTYPE_FLOAT2, D3DDECLMETHOD_DEFAULT, D3DDECLUSAGE_TEXCOORD, 0}, D3DDECL_END()};
  IDirect3DVertexDeclaration9 *declColor1 = nullptr, *declNoColor = nullptr;
  CHECK_HR(dev->CreateVertexDeclaration(eColor1, &declColor1));
  CHECK_HR(dev->CreateVertexDeclaration(eNoColor, &declNoColor));
  IDirect3DVertexBuffer9 *vb0 = nullptr;
  CHECK_HR(dev->CreateVertexBuffer(8 * sizeof(Vtx), D3DUSAGE_WRITEONLY, 0, D3DPOOL_MANAGED, &vb0, nullptr));
  Vtx q[4];
  float c0[4] = {2.0f / W, -2.0f / H, -1.0f, 1.0f};

  CHECK_HR(dev->SetRenderTarget(0, target));
  CHECK_HR(dev->BeginScene());
  CHECK_HR(dev->Clear(0, nullptr, D3DCLEAR_TARGET, D3DCOLOR_ARGB(255, 64, 64, 64), 1.0f, 0));
  dev->SetRenderState(D3DRS_CULLMODE, D3DCULL_NONE);
  dev->SetRenderState(D3DRS_ZENABLE, FALSE);
  dev->SetRenderState(D3DRS_ALPHABLENDENABLE, FALSE);
  dev->SetVertexShaderConstantF(0, c0, 1);
  dev->SetVertexShader(vsBasic); dev->SetPixelShader(psColor);
  bool drawOk = true;
  auto putVb = [&](const Vtx *v) {
    void *p; if (FAILED(vb0->Lock(0, 4 * sizeof(Vtx), &p, 0))) return false;
    memcpy(p, v, 4 * sizeof(Vtx)); vb0->Unlock(); return true;
  };
  if (vb) {
    // V1
    quad(q, 0, 0, 64, 64, 0.5f, 0, 0, 0, 0, 0xffffffff); drawOk &= putVb(q);
    dev->SetVertexDeclaration(declColor1);
    dev->SetStreamSource(0, vb0, 0, sizeof(Vtx));
    dev->SetStreamSource(1, nullptr, 0, 0);
    drawOk &= SUCCEEDED(dev->DrawPrimitive(D3DPT_TRIANGLESTRIP, 0, 2));
    // V2: its own buffer, so V1's draw keeps its vertices
    quad(q, 64, 0, 128, 64, 0.5f, 0, 0, 0, 0, 0xffffffff);
    IDirect3DVertexBuffer9 *vb1 = nullptr;
    CHECK_HR(dev->CreateVertexBuffer(4 * sizeof(Vtx), D3DUSAGE_WRITEONLY, 0, D3DPOOL_MANAGED, &vb1, nullptr));
    { void *p; CHECK_HR(vb1->Lock(0, 0, &p, 0)); memcpy(p, q, sizeof q); vb1->Unlock(); }
    dev->SetVertexDeclaration(declNoColor);
    dev->SetStreamSource(0, vb1, 0, sizeof(Vtx));
    drawOk &= SUCCEEDED(dev->DrawPrimitive(D3DPT_TRIANGLESTRIP, 0, 2));
    // V3
    dev->SetVertexDeclaration(declBasic);
    quad(q, 128, 0, 192, 64, 0.5f, 0, 0, 0, 0, D3DCOLOR_ARGB(255, 255, 0, 0));
    drawOk &= SUCCEEDED(dev->DrawPrimitiveUP(D3DPT_TRIANGLESTRIP, 2, q, sizeof(Vtx)));
    drawOk &= SUCCEEDED(dev->DrawPrimitive(D3DPT_TRIANGLESTRIP, 0, 2));
    dev->SetStreamSource(0, nullptr, 0, 0);
    vb1->Release();
  }
  if (depth) {
    dev->SetVertexDeclaration(declBasic);
    const float zs[3] = {1.5f, -0.5f, 0.99f};
    for (int k = 0; k < 3; k++) {
      quad(q, 192 + 64 * k, 0, 256 + 64 * k, 64, zs[k], 0, 0, 0, 0, D3DCOLOR_ARGB(255, 0, 255, 0));
      drawOk &= SUCCEEDED(dev->DrawPrimitiveUP(D3DPT_TRIANGLESTRIP, 2, q, sizeof(Vtx)));
    }
  }
  CHECK_HR(dev->EndScene());
  record("stream_depth.draws", drawOk, fmt("groups '%s'", groups));

  Img im;
  if (!readback(dev, target, im)) { record("stream_depth.readback", false, ""); return false; }
  std::string bmp = std::string(outdir) + "/stream_depth.bmp";
  tu_write_bmp(bmp.c_str(), im.px.data(), W, H, W * 4);
  if (vb) {
    record("vb.stream_unset", diff(im, 32, 32, 0, 0, 0, 0) == 0, pxs(im, 32, 32) + " want (0,0,0,0)");
    record("vb.decl_missing_input", diff(im, 96, 32, 0, 0, 0, 0) == 0, pxs(im, 96, 32) + " want (0,0,0,0)");
    record("vb.stream0_unset_after_up", diff(im, 160, 32, 255, 0, 0, 255) == 0, pxs(im, 160, 32) + " want (255,0,0,255)");
  }
  if (depth) {
    record("depth.clip_far", diff(im, 224, 32, 64, 64, 64, 255) == 0, pxs(im, 224, 32) + " want grey (clipped)");
    record("depth.clip_near", diff(im, 288, 32, 64, 64, 64, 255) == 0, pxs(im, 288, 32) + " want grey (clipped)");
    record("depth.clip_control", diff(im, 352, 32, 0, 255, 0, 255) == 0, pxs(im, 352, 32) + " want green");
  }
  declColor1->Release(); declNoColor->Release(); vb0->Release();
  return true;
}

static bool run(void *win, const char *outdir) {
  IDirect3D9 *d3d = Direct3DCreate9(D3D_SDK_VERSION);
  if (!d3d) { record("Direct3DCreate9", false, "returned NULL"); return false; }
  record("Direct3DCreate9", true, "");

  D3DADAPTER_IDENTIFIER9 id = {};
  d3d->GetAdapterIdentifier(0, 0, &id);
  info(fmt("adapter: '%s' vendor 0x%04x device 0x%04x driver %u.%u", id.Description, (unsigned)id.VendorId, (unsigned)id.DeviceId,
           (unsigned)id.DriverVersion.HighPart, (unsigned)id.DriverVersion.LowPart));

  D3DCAPS9 caps = {};
  CHECK_HR(d3d->GetDeviceCaps(0, D3DDEVTYPE_HAL, &caps));
  info(fmt("caps: VS %u.%u PS %u.%u MaxVSConst %u NumSimultaneousRTs %u MaxStreams %u MaxSimultaneousTextures %u "
           "MaxTex %ux%u DevCaps 0x%08x DevCaps2 0x%08x Caps2 0x%08x DeclTypes 0x%08x VS30slots %u PS30slots %u",
           (unsigned)D3DSHADER_VERSION_MAJOR(caps.VertexShaderVersion), (unsigned)D3DSHADER_VERSION_MINOR(caps.VertexShaderVersion),
           (unsigned)D3DSHADER_VERSION_MAJOR(caps.PixelShaderVersion), (unsigned)D3DSHADER_VERSION_MINOR(caps.PixelShaderVersion),
           (unsigned)caps.MaxVertexShaderConst, (unsigned)caps.NumSimultaneousRTs, (unsigned)caps.MaxStreams,
           (unsigned)caps.MaxSimultaneousTextures, (unsigned)caps.MaxTextureWidth, (unsigned)caps.MaxTextureHeight,
           (unsigned)caps.DevCaps, (unsigned)caps.DevCaps2, (unsigned)caps.Caps2, (unsigned)caps.DeclTypes,
           (unsigned)caps.MaxVertexShader30InstructionSlots, (unsigned)caps.MaxPixelShader30InstructionSlots));
  record("caps.sm3", caps.VertexShaderVersion >= D3DVS_VERSION(3, 0) && caps.PixelShaderVersion >= D3DPS_VERSION(3, 0),
         fmt("VS 0x%x PS 0x%x", (unsigned)caps.VertexShaderVersion, (unsigned)caps.PixelShaderVersion));
  record("caps.streamoffset", (caps.DevCaps2 & D3DDEVCAPS2_STREAMOFFSET) != 0, fmt("DevCaps2 0x%x", (unsigned)caps.DevCaps2));

  auto cdf = [&](DWORD usage, D3DRESOURCETYPE rt, D3DFORMAT f) {
    return SUCCEEDED(d3d->CheckDeviceFormat(0, D3DDEVTYPE_HAL, D3DFMT_X8R8G8B8, usage, rt, f));
  };
  bool vtfR32F = cdf(D3DUSAGE_QUERY_VERTEXTEXTURE, D3DRTYPE_TEXTURE, D3DFMT_R32F);
  bool vtfA32 = cdf(D3DUSAGE_QUERY_VERTEXTEXTURE, D3DRTYPE_TEXTURE, D3DFMT_A32B32G32R32F);
  bool inst = cdf(0, D3DRTYPE_SURFACE, (D3DFORMAT)MAKEFOURCC('I', 'N', 'S', 'T'));
  bool intz = cdf(D3DUSAGE_DEPTHSTENCIL, D3DRTYPE_TEXTURE, (D3DFORMAT)MAKEFOURCC('I', 'N', 'T', 'Z'));
  info(fmt("CheckDeviceFormat: VTF R32F %d, VTF A32B32G32R32F %d, 'INST' %d, 'INTZ' depth texture %d, DXT1 %d DXT3 %d DXT5 %d, "
           "D24S8 depth %d, D24S8 depth texture %d, A8R8G8B8 RT %d, autogen mip A8R8G8B8 %d",
           vtfR32F, vtfA32, inst, intz, cdf(0, D3DRTYPE_TEXTURE, D3DFMT_DXT1), cdf(0, D3DRTYPE_TEXTURE, D3DFMT_DXT3),
           cdf(0, D3DRTYPE_TEXTURE, D3DFMT_DXT5), cdf(D3DUSAGE_DEPTHSTENCIL, D3DRTYPE_SURFACE, D3DFMT_D24S8),
           cdf(D3DUSAGE_DEPTHSTENCIL, D3DRTYPE_TEXTURE, D3DFMT_D24S8), cdf(D3DUSAGE_RENDERTARGET, D3DRTYPE_TEXTURE, D3DFMT_A8R8G8B8),
           cdf(D3DUSAGE_AUTOGENMIPMAP, D3DRTYPE_TEXTURE, D3DFMT_A8R8G8B8)));
  record("format.dxt", cdf(0, D3DRTYPE_TEXTURE, D3DFMT_DXT1) && cdf(0, D3DRTYPE_TEXTURE, D3DFMT_DXT3) && cdf(0, D3DRTYPE_TEXTURE, D3DFMT_DXT5), "");
  record("format.vtf", vtfR32F || vtfA32, fmt("R32F %d A32B32G32R32F %d", vtfR32F, vtfA32));

  // Present parameters as the game builds them, windowed.
  D3DPRESENT_PARAMETERS pp = {};
  pp.BackBufferWidth = W; pp.BackBufferHeight = H; pp.BackBufferFormat = D3DFMT_A8R8G8B8; pp.BackBufferCount = 1;
  pp.SwapEffect = D3DSWAPEFFECT_COPY; pp.hDeviceWindow = (HWND)win; pp.Windowed = TRUE;
  pp.EnableAutoDepthStencil = TRUE; pp.AutoDepthStencilFormat = D3DFMT_D24S8;
  pp.PresentationInterval = D3DPRESENT_INTERVAL_IMMEDIATE;
  IDirect3DDevice9 *dev = nullptr;
  HRESULT hr = d3d->CreateDevice(0, D3DDEVTYPE_HAL, (HWND)win, D3DCREATE_HARDWARE_VERTEXPROCESSING, &pp, &dev);
  record("CreateDevice", SUCCEEDED(hr), fmt("hr 0x%08x, behaviour flags 0x40 as the game", (unsigned)hr));
  if (FAILED(hr)) return false;

  // Shaders.
  IDirect3DVertexShader9 *vsBasic = nullptr, *vsVtf = nullptr, *vsInst = nullptr;
  IDirect3DPixelShader9 *psColor = nullptr, *psTex = nullptr, *psSm3 = nullptr, *psShadow = nullptr;
  bool shOk = SUCCEEDED(dev->CreateVertexShader((const DWORD *)k_vs_basic, &vsBasic)) &&
              SUCCEEDED(dev->CreateVertexShader((const DWORD *)k_vs_vtf, &vsVtf)) &&
              SUCCEEDED(dev->CreateVertexShader((const DWORD *)k_vs_inst, &vsInst)) &&
              SUCCEEDED(dev->CreatePixelShader((const DWORD *)k_ps_color, &psColor)) &&
              SUCCEEDED(dev->CreatePixelShader((const DWORD *)k_ps_tex, &psTex)) &&
              SUCCEEDED(dev->CreatePixelShader((const DWORD *)k_ps_sm3, &psSm3)) &&
              SUCCEEDED(dev->CreatePixelShader((const DWORD *)k_ps_shadow, &psShadow));
  record("CreateShader.sm3", shOk, "3 vs_3_0 + 4 ps_3_0 assembled by tools/shaders/d3dbc.py");
  if (!shOk) return false;

  D3DVERTEXELEMENT9 eBasic[] = {
      {0, 0, D3DDECLTYPE_FLOAT4, D3DDECLMETHOD_DEFAULT, D3DDECLUSAGE_POSITION, 0},
      {0, 16, D3DDECLTYPE_FLOAT2, D3DDECLMETHOD_DEFAULT, D3DDECLUSAGE_TEXCOORD, 0},
      {0, 24, D3DDECLTYPE_D3DCOLOR, D3DDECLMETHOD_DEFAULT, D3DDECLUSAGE_COLOR, 0}, D3DDECL_END()};
  D3DVERTEXELEMENT9 eInst[] = {
      {0, 0, D3DDECLTYPE_FLOAT2, D3DDECLMETHOD_DEFAULT, D3DDECLUSAGE_POSITION, 0},
      {1, 0, D3DDECLTYPE_FLOAT4, D3DDECLMETHOD_DEFAULT, D3DDECLUSAGE_TEXCOORD, 0},
      {1, 16, D3DDECLTYPE_D3DCOLOR, D3DDECLMETHOD_DEFAULT, D3DDECLUSAGE_COLOR, 0}, D3DDECL_END()};
  IDirect3DVertexDeclaration9 *declBasic = nullptr, *declInst = nullptr;
  CHECK_HR(dev->CreateVertexDeclaration(eBasic, &declBasic));
  CHECK_HR(dev->CreateVertexDeclaration(eInst, &declInst));

  // Render target texture + matching depth surface + readback.
  IDirect3DTexture9 *rtTex = nullptr, *rt2Tex = nullptr;
  CHECK_HR(dev->CreateTexture(W, H, 1, D3DUSAGE_RENDERTARGET, D3DFMT_A8R8G8B8, D3DPOOL_DEFAULT, &rtTex, nullptr));
  CHECK_HR(dev->CreateTexture(W, H, 1, D3DUSAGE_RENDERTARGET, D3DFMT_A8R8G8B8, D3DPOOL_DEFAULT, &rt2Tex, nullptr));
  IDirect3DSurface9 *rtSurf = nullptr, *rt2Surf = nullptr, *ds = nullptr, *bb = nullptr;
  CHECK_HR(rtTex->GetSurfaceLevel(0, &rtSurf));
  CHECK_HR(rt2Tex->GetSurfaceLevel(0, &rt2Surf));
  CHECK_HR(dev->CreateDepthStencilSurface(W, H, D3DFMT_D24S8, D3DMULTISAMPLE_NONE, 0, FALSE, &ds, nullptr));

  // VTF texture: 4x4 R32F (or A32B32G32R32F), value (i+1)/17.
  D3DFORMAT vtfFmt = vtfR32F ? D3DFMT_R32F : D3DFMT_A32B32G32R32F;
  IDirect3DTexture9 *vtf = nullptr;
  CHECK_HR(dev->CreateTexture(4, 4, 1, 0, vtfFmt, D3DPOOL_MANAGED, &vtf, nullptr));
  {
    D3DLOCKED_RECT lr; CHECK_HR(vtf->LockRect(0, &lr, nullptr, 0));
    for (int y = 0; y < 4; y++)
      for (int x = 0; x < 4; x++) {
        float v = (y * 4 + x + 1) / 17.0f;
        float *row = (float *)((uint8_t *)lr.pBits + y * lr.Pitch);
        if (vtfFmt == D3DFMT_R32F) row[x] = v; else { row[x * 4] = v; row[x * 4 + 1] = v; row[x * 4 + 2] = v; row[x * 4 + 3] = 1; }
      }
    vtf->UnlockRect(0);
  }

  // DXT textures: 8x8, random blocks, MANAGED, filled through LockRect as RW does (0x11ef7a0).
  IDirect3DTexture9 *dxt[3] = {};
  uint32_t seed = 0x5eed1234;
  for (int k = 0; k < 3; k++) {
    int bs = k == 0 ? 8 : 16;
    g_dxt[k].resize(4 * bs);
    for (auto &b : g_dxt[k]) b = (uint8_t)tu_rand(&seed);
    CHECK_HR(dev->CreateTexture(8, 8, 1, 0, kDxtFmt[k], D3DPOOL_MANAGED, &dxt[k], nullptr));
    D3DLOCKED_RECT lr; CHECK_HR(dxt[k]->LockRect(0, &lr, nullptr, 0));
    for (int by = 0; by < 2; by++) memcpy((uint8_t *)lr.pBits + by * lr.Pitch, &g_dxt[k][by * 2 * bs], 2 * bs);
    dxt[k]->UnlockRect(0);
  }

  // Plain A8R8G8B8 8x8 texture with random texels (control for the DXT test).
  IDirect3DTexture9 *argb = nullptr;
  static uint8_t g_argb[8 * 8 * 4];
  for (auto &b : g_argb) b = (uint8_t)tu_rand(&seed);
  CHECK_HR(dev->CreateTexture(8, 8, 1, 0, D3DFMT_A8R8G8B8, D3DPOOL_MANAGED, &argb, nullptr));
  {
    D3DLOCKED_RECT lr; CHECK_HR(argb->LockRect(0, &lr, nullptr, 0));
    for (int y = 0; y < 8; y++) memcpy((uint8_t *)lr.pBits + y * lr.Pitch, g_argb + y * 32, 32);
    argb->UnlockRect(0);
  }

  // Instancing: stream 0 = unit square corners (static), stream 1 = 8 instances. Index buffer 16-bit.
  IDirect3DVertexBuffer9 *vbCorners = nullptr, *vbInst = nullptr;
  IDirect3DIndexBuffer9 *ib = nullptr;
  CHECK_HR(dev->CreateVertexBuffer(4 * 8, 0, 0, D3DPOOL_MANAGED, &vbCorners, nullptr));
  CHECK_HR(dev->CreateVertexBuffer(8 * 20, 0, 0, D3DPOOL_MANAGED, &vbInst, nullptr));
  CHECK_HR(dev->CreateIndexBuffer(6 * 2, 0, D3DFMT_INDEX16, D3DPOOL_MANAGED, &ib, nullptr));
  {
    float c[8] = {0, 0, 1, 0, 0, 1, 1, 1}; void *p;
    CHECK_HR(vbCorners->Lock(0, 0, &p, 0)); memcpy(p, c, sizeof c); vbCorners->Unlock();
    uint16_t idx[6] = {0, 1, 2, 2, 1, 3};
    CHECK_HR(ib->Lock(0, 0, &p, 0)); memcpy(p, idx, sizeof idx); ib->Unlock();
    struct { float x, y, s, pad; D3DCOLOR c; } in[8];
    for (int i = 0; i < 8; i++) in[i] = {(float)(i % 4) * 32 + 8, 128 + (float)(i / 4) * 64 + 8, 16, 0, inst_color(i)};
    static_assert(sizeof(in[0]) == 20, "instance stride");
    CHECK_HR(vbInst->Lock(0, 0, &p, 0)); memcpy(p, in, sizeof in); vbInst->Unlock();
  }

  Ring ring, smallRing;
  if (!ring.init(dev, 64 * 1024) || !smallRing.init(dev, 10 * 4 * sizeof(Vtx))) { record("CreateVertexBuffer.dynamic", false, ""); return false; }

  if (getenv("TEST_CONFORMANCE_DEPTH_DIAG")) {  // opt-in depth diagnostic
    // Diagnostic: for each clear depth, compare function and quad depth, report whether the quad was drawn.
    dev->SetRenderTarget(0, rtSurf); dev->SetDepthStencilSurface(ds);
    float c0d[4] = {2.0f / W, -2.0f / H, -1.0f, 1.0f};
    dev->SetVertexShaderConstantF(0, c0d, 1);
    dev->SetVertexDeclaration(declBasic); dev->SetVertexShader(vsBasic); dev->SetPixelShader(psColor);
    dev->SetRenderState(D3DRS_CULLMODE, D3DCULL_NONE);
    const float clears[3] = {1.0f, 0.5f, 0.0f}, zs[3] = {0.2f, 0.5f, 0.8f};
    const D3DCMPFUNC fn[4] = {D3DCMP_LESS, D3DCMP_LESSEQUAL, D3DCMP_GREATER, D3DCMP_ALWAYS};
    const char *fnn[4] = {"LESS", "LESSEQUAL", "GREATER", "ALWAYS"};
    for (float cz : clears) for (int f = 0; f < 4; f++) {
      std::string line = fmt("clearZ %.1f %-9s:", cz, fnn[f]);
      for (float z : zs) {
        dev->BeginScene();
        dev->Clear(0, nullptr, D3DCLEAR_TARGET | D3DCLEAR_ZBUFFER, 0xff000000, cz, 0);
        dev->SetRenderState(D3DRS_ZENABLE, D3DZB_TRUE); dev->SetRenderState(D3DRS_ZWRITEENABLE, TRUE);
        dev->SetRenderState(D3DRS_ZFUNC, fn[f]);
        Vtx qq[4]; quad(qq, 0, 0, 64, 64, z, 0, 0, 0, 0, 0xffffffff);
        int first = ring.push(qq, 4);
        dev->SetStreamSource(0, ring.vb, 0, sizeof(Vtx));
        dev->DrawPrimitive(D3DPT_TRIANGLESTRIP, first, 2);
        dev->EndScene();
        Img d; readback(dev, rtSurf, d);
        line += fmt(" z=%.1f:%s", z, d.at(32, 32)[0] ? "drawn" : "-");
      }
      info(line);
    }
    dev->SetRenderState(D3DRS_ZENABLE, FALSE);
  }

  // Shadow map: a 64x64 D24S8 depth *texture* (RW raster type 10) with its own 64x64 colour target.
  IDirect3DTexture9 *shadowTex = nullptr, *shadowRt = nullptr;
  IDirect3DSurface9 *shadowDs = nullptr, *shadowRtSurf = nullptr;
  bool shadowOk = SUCCEEDED(dev->CreateTexture(64, 64, 1, D3DUSAGE_DEPTHSTENCIL, D3DFMT_D24S8, D3DPOOL_DEFAULT, &shadowTex, nullptr)) &&
                  SUCCEEDED(dev->CreateTexture(64, 64, 1, D3DUSAGE_RENDERTARGET, D3DFMT_A8R8G8B8, D3DPOOL_DEFAULT, &shadowRt, nullptr)) &&
                  SUCCEEDED(shadowTex->GetSurfaceLevel(0, &shadowDs)) && SUCCEEDED(shadowRt->GetSurfaceLevel(0, &shadowRtSurf));
  record("CreateTexture.depth_texture", shadowOk, "64x64 D24S8 with D3DUSAGE_DEPTHSTENCIL");

  // ---- Frame ----
  CHECK_HR(dev->BeginScene());
  Vtx q[4];
  bool drawOk = true;
  float c0[4] = {2.0f / W, -2.0f / H, -1.0f, 1.0f}, c1[4] = {0, 1, 0.5f, 0}, p1[4] = {1 / 16.0f, 1 / 16.0f, 0, 0};
  float p2[4] = {0.5f, 0, 0, 0};
  auto drawQuad = [&](Ring &r, const Vtx *v) {
    int first = r.push(v, 4);
    if (first < 0) return false;
    dev->SetStreamSource(0, r.vb, 0, sizeof(Vtx));
    return SUCCEEDED(dev->DrawPrimitive(D3DPT_TRIANGLESTRIP, first, 2));
  };
  if (shadowOk) {
    // Shadow pass: depth 0.3 on the left half, cleared 1.0 on the right.
    dev->SetRenderTarget(0, shadowRtSurf); dev->SetDepthStencilSurface(shadowDs);
    dev->Clear(0, nullptr, D3DCLEAR_TARGET | D3DCLEAR_ZBUFFER, 0, 1.0f, 0);
    float cs[4] = {2.0f / 64, -2.0f / 64, -1.0f, 1.0f};
    dev->SetVertexShaderConstantF(0, cs, 1);
    dev->SetVertexDeclaration(declBasic); dev->SetVertexShader(vsBasic); dev->SetPixelShader(psColor);
    dev->SetRenderState(D3DRS_CULLMODE, D3DCULL_NONE);
    dev->SetRenderState(D3DRS_ZENABLE, D3DZB_TRUE); dev->SetRenderState(D3DRS_ZWRITEENABLE, TRUE);
    dev->SetRenderState(D3DRS_ZFUNC, D3DCMP_LESSEQUAL);
    quad(q, 0, 0, 32, 64, 0.3f, 0, 0, 0, 0, 0xffffffff); drawOk &= drawQuad(ring, q);
  }
  CHECK_HR(dev->SetRenderTarget(0, rtSurf));
  CHECK_HR(dev->SetDepthStencilSurface(ds));
  D3DVIEWPORT9 vpFull = {0, 0, W, H, 0, 1};
  dev->SetViewport(&vpFull);
  CHECK_HR(dev->Clear(0, nullptr, D3DCLEAR_TARGET | D3DCLEAR_ZBUFFER, D3DCOLOR_ARGB(255, 0, 0, 0), 1.0f, 0));
  dev->SetRenderState(D3DRS_CULLMODE, D3DCULL_NONE);
  dev->SetRenderState(D3DRS_ZENABLE, FALSE);
  dev->SetRenderState(D3DRS_LIGHTING, FALSE);
  dev->SetVertexShaderConstantF(0, c0, 1);
  dev->SetVertexShaderConstantF(1, c1, 1);
  dev->SetPixelShaderConstantF(1, p1, 1);
  dev->SetPixelShaderConstantF(2, p2, 1);

  // A: SM3 pixel shader over (0,0)-(128,128).
  dev->SetVertexDeclaration(declBasic); dev->SetVertexShader(vsBasic); dev->SetPixelShader(psSm3);
  quad(q, 0, 0, 128, 128, 0.5f, 0, 0, 1, 1, 0xffffffff); drawOk &= drawQuad(ring, q);

  // B: VTF, 4x4 cells of 32x32 over (128,0)-(256,128).
  dev->SetVertexShader(vsVtf); dev->SetPixelShader(psColor);
  dev->SetTexture(D3DVERTEXTEXTURESAMPLER0, vtf);
  dev->SetSamplerState(D3DVERTEXTEXTURESAMPLER0, D3DSAMP_MINFILTER, D3DTEXF_POINT);
  dev->SetSamplerState(D3DVERTEXTEXTURESAMPLER0, D3DSAMP_MAGFILTER, D3DTEXF_POINT);
  dev->SetSamplerState(D3DVERTEXTEXTURESAMPLER0, D3DSAMP_MIPFILTER, D3DTEXF_NONE);
  for (int cy = 0; cy < 4; cy++)
    for (int cx = 0; cx < 4; cx++) {
      float u = (cx + 0.5f) / 4, v = (cy + 0.5f) / 4;
      quad(q, 128 + cx * 32, cy * 32, 160 + cx * 32, 32 + cy * 32, 0.5f, u, v, u, v, 0);
      drawOk &= drawQuad(ring, q);
    }
  dev->SetTexture(D3DVERTEXTEXTURESAMPLER0, nullptr);

  // C: instancing over (0,128)-(128,256), as the game's 0x6eb2b1: INDEXEDDATA|n on 0, INSTANCEDATA|1 on 1.
  dev->SetVertexDeclaration(declInst); dev->SetVertexShader(vsInst); dev->SetPixelShader(psColor);
  dev->SetStreamSource(0, vbCorners, 0, 8);
  dev->SetStreamSource(1, vbInst, 0, 20);
  dev->SetIndices(ib);
  bool ssfOk = SUCCEEDED(dev->SetStreamSourceFreq(0, D3DSTREAMSOURCE_INDEXEDDATA | 8)) &&
               SUCCEEDED(dev->SetStreamSourceFreq(1, D3DSTREAMSOURCE_INSTANCEDATA | 1));
  drawOk &= SUCCEEDED(dev->DrawIndexedPrimitive(D3DPT_TRIANGLELIST, 0, 0, 4, 0, 2));
  ssfOk &= SUCCEEDED(dev->SetStreamSourceFreq(0, 1)) && SUCCEEDED(dev->SetStreamSourceFreq(1, 1));
  dev->SetStreamSource(1, nullptr, 0, 0);

  // D: depth over (128,128)-(256,256): far red, near green (left half), mid blue. Expect green | blue.
  dev->SetVertexDeclaration(declBasic); dev->SetVertexShader(vsBasic); dev->SetPixelShader(psColor);
  dev->SetRenderState(D3DRS_ZENABLE, D3DZB_TRUE); dev->SetRenderState(D3DRS_ZWRITEENABLE, TRUE);
  dev->SetRenderState(D3DRS_ZFUNC, D3DCMP_LESSEQUAL);
  quad(q, 128, 128, 256, 256, 0.8f, 0, 0, 0, 0, D3DCOLOR_ARGB(255, 255, 0, 0)); drawOk &= drawQuad(ring, q);
  quad(q, 128, 128, 192, 256, 0.2f, 0, 0, 0, 0, D3DCOLOR_ARGB(255, 0, 255, 0)); drawOk &= drawQuad(ring, q);
  quad(q, 128, 128, 256, 256, 0.5f, 0, 0, 0, 0, D3DCOLOR_ARGB(255, 0, 0, 255)); drawOk &= drawQuad(ring, q);
  dev->SetRenderState(D3DRS_ZENABLE, FALSE);

  // E: DXT1/3/5 at (256,0), (320,0), (384,0), each 8x8 texels magnified 8x, point sampled.
  dev->SetPixelShader(psTex);
  for (int k = 0; k < 3; k++) {
    dev->SetTexture(0, dxt[k]);
    dev->SetSamplerState(0, D3DSAMP_MINFILTER, D3DTEXF_POINT);
    dev->SetSamplerState(0, D3DSAMP_MAGFILTER, D3DTEXF_POINT);
    dev->SetSamplerState(0, D3DSAMP_MIPFILTER, D3DTEXF_NONE);
    quad(q, 256 + k * 64, 0, 320 + k * 64, 64, 0.5f, 0, 0, 1, 1, 0); drawOk &= drawQuad(ring, q);
  }
  // E2: an 8x8 A8R8G8B8 texture at (256,64)-(320,128), same shader and sampler: separates format from sampling bugs.
  dev->SetTexture(0, argb);
  quad(q, 256, 64, 320, 128, 0.5f, 0, 0, 1, 1, 0); drawOk &= drawQuad(ring, q);
  dev->SetTexture(0, nullptr);

  // D2: depth probes. (320,64)-(384,128): ZFUNC NEVER with a white quad -> must stay black.
  // (384,64)-(448,128): near green z=0.2 first, then far red z=0.8 with LESS -> must stay green.
  dev->SetPixelShader(psColor);
  dev->SetRenderState(D3DRS_ZENABLE, D3DZB_TRUE); dev->SetRenderState(D3DRS_ZWRITEENABLE, TRUE);
  dev->SetRenderState(D3DRS_ZFUNC, D3DCMP_NEVER);
  quad(q, 320, 64, 384, 128, 0.5f, 0, 0, 0, 0, 0xffffffff); drawOk &= drawQuad(ring, q);
  dev->SetRenderState(D3DRS_ZFUNC, D3DCMP_LESS);
  quad(q, 384, 64, 448, 128, 0.2f, 0, 0, 0, 0, D3DCOLOR_ARGB(255, 0, 255, 0)); drawOk &= drawQuad(ring, q);
  quad(q, 384, 64, 448, 128, 0.8f, 0, 0, 0, 0, D3DCOLOR_ARGB(255, 255, 0, 0)); drawOk &= drawQuad(ring, q);
  dev->SetRenderState(D3DRS_ZENABLE, FALSE);

  // S: shadow-map compare over (448,64)-(512,128): ref 0.5 vs depth 0.3 (left) / 1.0 (right) -> 0 | 1.
  if (shadowOk) {
    dev->SetPixelShader(psShadow);
    dev->SetTexture(0, shadowTex);
    dev->SetSamplerState(0, D3DSAMP_MINFILTER, D3DTEXF_POINT);
    dev->SetSamplerState(0, D3DSAMP_MAGFILTER, D3DTEXF_POINT);
    quad(q, 448, 64, 512, 128, 0.5f, 0, 0, 1, 1, 0); drawOk &= drawQuad(ring, q);
    dev->SetTexture(0, nullptr);
  }

  // F: 32 quads through a 10-quad dynamic ring (forces DISCARD renames) over (256,128)-(512,256).
  dev->SetPixelShader(psColor);
  for (int i = 0; i < 32; i++) {
    int cx = i % 8, cy = i / 8;
    quad(q, 256 + cx * 32, 128 + cy * 32, 288 + cx * 32, 160 + cy * 32, 0.5f, 0, 0, 0, 0, ring_color(i));
    drawOk &= drawQuad(smallRing, q);
  }

  // G: viewport-limited Clear over (448,0)-(512,64) (Clear respects the viewport in D3D9).
  D3DVIEWPORT9 vpG = {448, 0, 64, 64, 0, 1};
  dev->SetViewport(&vpG);
  drawOk &= SUCCEEDED(dev->Clear(0, nullptr, D3DCLEAR_TARGET, D3DCOLOR_ARGB(255, 0x33, 0x66, 0x99), 1.0f, 0));
  dev->SetViewport(&vpFull);
  CHECK_HR(dev->EndScene());
  record("draws.submitted", drawOk, "all Draw*/Clear calls returned D3D_OK");
  record("SetStreamSourceFreq", ssfOk, "INDEXEDDATA|8 / INSTANCEDATA|1, then reset to 1");

  // Event query, as RW's frame limiter (0x11f8190): Issue(END) then spin GetData(NULL,0,FLUSH).
  IDirect3DQuery9 *q9 = nullptr;
  bool qprobe = SUCCEEDED(dev->CreateQuery(D3DQUERYTYPE_EVENT, nullptr));
  CHECK_HR(dev->CreateQuery(D3DQUERYTYPE_EVENT, &q9));
  q9->Issue(D3DISSUE_END);
  auto t0 = std::chrono::steady_clock::now();
  long spins = 0; HRESULT qhr;
  while ((qhr = q9->GetData(nullptr, 0, D3DGETDATA_FLUSH)) == S_FALSE) spins++;
  double qms = std::chrono::duration<double, std::milli>(std::chrono::steady_clock::now() - t0).count();
  record("query.event", qhr == S_OK && qprobe, fmt("probe(NULL) %d, %ld spins, %.2f ms", qprobe, spins, qms));

  // ---- Readback (the game's GetRenderTargetData path) and checks ----
  Img im;
  if (!readback(dev, rtSurf, im)) { record("GetRenderTargetData", false, ""); return false; }
  record("GetRenderTargetData", true, "RT texture level 0 -> SYSTEMMEM offscreen plain surface -> LockRect(READONLY)");
  std::string bmp = std::string(outdir) + "/frame.bmp";
  tu_write_bmp(bmp.c_str(), im.px.data(), W, H, W * 4);

  // A: SM3.
  {
    int bad = 0, worst = 0; std::string ex;
    for (int y = 0; y < 128; y += 3)
      for (int x = 0; x < 128; x += 3) {
        int g = (x % 16) < 8 ? 255 : 0;
        int b = (int)std::lround((y % 16) / 16.0 * 255);
        int d = diff(im, x, y, 64, g, b, 255);
        if (d > 2) { if (!bad) ex = pxs(im, x, y) + fmt(" want (64,%d,%d,255)", g, b); bad++; }
        worst = std::max(worst, d);
      }
    record("sm3.ps_vpos_rep_ifc", bad == 0, fmt("%d mismatching samples of 1849, max diff %d %s", bad, worst, ex.c_str()));
    // Exact coverage of the half-pixel-adjusted quad: column 127 is SM3 output, 128 belongs to region B.
    // Pixel (127,5) is the last SM3 column (x%16 = 15 -> G 0, B = 5/16), (128,5) is VTF cell 0 (1/17 grey),
    // (5,127) the last SM3 row (B = 15/16), (5,128) is the black gap above instance 0.
    bool edge = diff(im, 127, 5, 64, 0, 80, 255) <= 2 && diff(im, 128, 5, 15, 15, 15, -1) <= 2 &&
                diff(im, 5, 127, 64, 255, 239, 255) <= 2 && diff(im, 5, 128, 0, 0, 0, 255) == 0;
    record("halfpixel.edges", edge, pxs(im, 127, 5) + " " + pxs(im, 128, 5) + " " + pxs(im, 5, 127) + " " + pxs(im, 5, 128));
  }
  // B: VTF.
  {
    int bad = 0; std::string ex;
    for (int cy = 0; cy < 4; cy++)
      for (int cx = 0; cx < 4; cx++) {
        int v = (int)std::lround((cy * 4 + cx + 1) / 17.0 * 255);
        int x = 128 + cx * 32 + 16, y = cy * 32 + 16;
        if (diff(im, x, y, v, v, v, -1) > 2) { if (!bad) ex = pxs(im, x, y) + fmt(" want %d", v); bad++; }
      }
    record("vtf.texldl", bad == 0, fmt("%s, %d/16 cells wrong %s", vtfFmt == D3DFMT_R32F ? "R32F" : "A32B32G32R32F", bad, ex.c_str()));
  }
  // C: instancing.
  {
    int bad = 0; std::string ex;
    for (int i = 0; i < 8; i++) {
      int x = (i % 4) * 32 + 16, y = 128 + (i / 4) * 64 + 16;
      D3DCOLOR c = inst_color(i);
      if (diff(im, x, y, (c >> 16) & 255, (c >> 8) & 255, c & 255, 255) > 1) { if (!bad) ex = pxs(im, x, y); bad++; }
    }
    bool gap = diff(im, 4, 132, 0, 0, 0, 255) == 0;  // outside every instance
    record("instancing.ssf", bad == 0 && gap, fmt("%d/8 instances wrong, gap black %d %s", bad, gap, ex.c_str()));
  }
  // D: depth.
  {
    bool ok = diff(im, 150, 200, 0, 255, 0, 255) == 0 && diff(im, 230, 200, 0, 0, 255, 255) == 0;
    record("depth.d24s8", ok, pxs(im, 150, 200) + " " + pxs(im, 230, 200));
  }
  // E: DXT.
  for (int k = 0; k < 3; k++) {
    uint8_t ref[8 * 8 * 4];
    tu_decode_dxt(kDxtNum[k], g_dxt[k].data(), 8, 8, ref);
    int bad = 0, worst = 0; std::string ex;
    for (int ty = 0; ty < 8; ty++)
      for (int tx = 0; tx < 8; tx++) {
        const uint8_t *r = ref + (ty * 8 + tx) * 4;
        int x = 256 + k * 64 + tx * 8 + 4, y = ty * 8 + 4;
        int d = diff(im, x, y, r[0], r[1], r[2], r[3]);
        worst = std::max(worst, d);
        if (d > 8) { if (!bad) ex = pxs(im, x, y) + fmt(" want (%d,%d,%d,%d)", r[0], r[1], r[2], r[3]); bad++; }
      }
    record(fmt("dxt%d.sample", kDxtNum[k]), bad == 0, fmt("%d/64 texels off by >8, max diff %d %s", bad, worst, ex.c_str()));
  }
  // E2: A8R8G8B8 control.
  {
    int bad = 0; std::string ex;
    for (int ty = 0; ty < 8; ty++)
      for (int tx = 0; tx < 8; tx++) {
        const uint8_t *t = g_argb + (ty * 8 + tx) * 4;  // B,G,R,A in memory
        int x = 256 + tx * 8 + 4, y = 64 + ty * 8 + 4;
        if (diff(im, x, y, t[2], t[1], t[0], t[3]) > 1) { if (!bad) ex = pxs(im, x, y) + fmt(" want (%d,%d,%d,%d)", t[2], t[1], t[0], t[3]); bad++; }
      }
    record("tex.a8r8g8b8.sample", bad == 0, fmt("%d/64 texels wrong %s", bad, ex.c_str()));
  }
  // D2: depth probes.
  {
    bool never = diff(im, 350, 96, 0, 0, 0, 255) == 0;
    bool less = diff(im, 416, 96, 0, 255, 0, 255) == 0;
    record("depth.zfunc_never", never, pxs(im, 350, 96));
    record("depth.zfunc_less_order", less, pxs(im, 416, 96));
  }
  // S: shadow compare.
  if (shadowOk) {
    bool ok = diff(im, 464, 96, 0, 0, 0, -1) <= 1 && diff(im, 496, 96, 255, 255, 255, -1) <= 1;
    record("shadow.depth_compare", ok, pxs(im, 464, 96) + " " + pxs(im, 496, 96) + " want 0 | 255");
  }
  // F: dynamic ring.
  {
    int bad = 0; std::string ex;
    for (int i = 0; i < 32; i++) {
      int x = 256 + (i % 8) * 32 + 16, y = 128 + (i / 8) * 32 + 16;
      D3DCOLOR c = ring_color(i);
      if (diff(im, x, y, (c >> 16) & 255, (c >> 8) & 255, c & 255, 255) > 1) { if (!bad) ex = pxs(im, x, y); bad++; }
    }
    uintptr_t lo = UINTPTR_MAX, hi = 0;
    for (auto p : smallRing.discardPtrs) { lo = std::min(lo, p); hi = std::max(hi, p); }
    info(fmt("Lock(DISCARD) pointers span 0x%llx..0x%llx: %s the low 4 GiB (the guest window)",
             (unsigned long long)lo, (unsigned long long)hi, hi >= (1ull << 32) ? "outside" : "inside"));
    record("lock.dynamic_ring", bad == 0, fmt("%d/32 quads wrong; %u locks, %u DISCARDs, %zu distinct DISCARD pointers %s",
                                              bad, smallRing.locks, smallRing.discards, smallRing.discardPtrs.size(), ex.c_str()));
  }
  // G: viewport clear.
  {
    bool in = diff(im, 480, 32, 0x33, 0x66, 0x99, 255) == 0;
    bool out = diff(im, 480, 70, 0x33, 0x66, 0x99, 255) != 0;  // just below the viewport
    record("clear.viewport", in && out, pxs(im, 480, 32) + " " + pxs(im, 480, 70));
  }

  // the sampling and stream/depth cases, drawn into rt2 (the back-buffer test below overwrites it)
  if (!sampling_tests(dev, outdir, vsBasic, psTex, rt2Surf)) record("sampling", false, "aborted");
  {
    const char *g = getenv("TEST_CONFORMANCE_CASES");
    if (!stream_depth_tests(dev, outdir, vsBasic, psColor, declBasic, rt2Surf, g && *g ? g : "all")) record("stream_depth", false, "aborted");
  }
  dev->SetRenderTarget(0, rtSurf); dev->SetDepthStencilSurface(ds);

  // ---- Back buffer: StretchRect RT -> back buffer, then the game's GetBackBuffer + StretchRect(LINEAR) -> RT texture ----
  CHECK_HR(dev->GetBackBuffer(0, 0, D3DBACKBUFFER_TYPE_MONO, &bb));
  bool srOk = SUCCEEDED(dev->StretchRect(rtSurf, nullptr, bb, nullptr, D3DTEXF_POINT));
  srOk &= SUCCEEDED(dev->StretchRect(bb, nullptr, rt2Surf, nullptr, D3DTEXF_LINEAR));
  Img im2;
  bool same = srOk && readback(dev, rt2Surf, im2);
  int ndiff = 0;
  if (same) for (size_t i = 0; i < im.px.size(); i++) ndiff += im.px[i] != im2.px[i];
  record("backbuffer.stretchrect", same && ndiff == 0, fmt("RT->BB (POINT), BB->RT2 (LINEAR, 1:1), %d differing bytes", ndiff));
  Img imbb;
  bool bbOk = readback(dev, bb, imbb);
  int nbb = 0;
  if (bbOk) for (size_t i = 0; i < im.px.size(); i++) nbb += im.px[i] != imbb.px[i];
  record("backbuffer.readback", bbOk && nbb == 0, fmt("GetRenderTargetData(back buffer), %d differing bytes", nbb));

  // Present a few frames so the window shows the result; measure a simple present rate.
  dev->SetRenderTarget(0, bb);
  int frames = 0;
  auto f0 = std::chrono::steady_clock::now();
  HRESULT phr = D3D_OK;
  while (std::chrono::duration<double>(std::chrono::steady_clock::now() - f0).count() < 1.5) {
    test_pump();
    dev->BeginScene();
    dev->StretchRect(rtSurf, nullptr, bb, nullptr, D3DTEXF_POINT);
    dev->EndScene();
    phr = dev->Present(nullptr, nullptr, nullptr, nullptr);
    if (FAILED(phr)) break;
    frames++;
  }
  double secs = std::chrono::duration<double>(std::chrono::steady_clock::now() - f0).count();
  record("Present", SUCCEEDED(phr) && frames > 0, fmt("%d frames in %.2f s (%.0f/s, IMMEDIATE)", frames, secs, frames / secs));

  // Device reset as in the lost-device path: release DEFAULT-pool resources, Reset, recreate.
  ring.vb->Release(); smallRing.vb->Release(); bb->Release(); rtSurf->Release(); rt2Surf->Release(); rtTex->Release(); rt2Tex->Release(); ds->Release();
  q9->Release();
  if (shadowOk) { shadowDs->Release(); shadowRtSurf->Release(); shadowTex->Release(); shadowRt->Release(); }
  dev->SetRenderTarget(0, nullptr);
  HRESULT rhr = dev->Reset(&pp);
  record("Reset", SUCCEEDED(rhr), fmt("hr 0x%08x after releasing DEFAULT-pool resources", (unsigned)rhr));

  for (auto *t : dxt) t->Release();
  argb->Release();
  psShadow->Release();
  vtf->Release(); vbCorners->Release(); vbInst->Release(); ib->Release();
  declBasic->Release(); declInst->Release();
  vsBasic->Release(); vsVtf->Release(); vsInst->Release(); psColor->Release(); psTex->Release(); psSm3->Release();
  ULONG left = dev->Release();
  info(fmt("device refcount after release: %lu", (unsigned long)left));
  d3d->Release();
  return true;
}

static void write_json(const char *outdir) {
  std::string p = std::string(outdir) + "/results.json";
  FILE *f = fopen(p.c_str(), "w");
  if (!f) return;
  auto esc = [](const std::string &s) { std::string o; for (char c : s) { if (c == '"' || c == '\\') o += '\\'; o += c; } return o; };
  fprintf(f, "{\n  \"tests\": [\n");
  for (size_t i = 0; i < g_results.size(); i++)
    fprintf(f, "    {\"name\": \"%s\", \"pass\": %s, \"detail\": \"%s\"}%s\n", esc(g_results[i].name).c_str(),
            g_results[i].pass ? "true" : "false", esc(g_results[i].detail).c_str(), i + 1 < g_results.size() ? "," : "");
  fprintf(f, "  ],\n  \"info\": [\n");
  for (size_t i = 0; i < g_info.size(); i++)
    fprintf(f, "    \"%s\"%s\n", esc(g_info[i]).c_str(), i + 1 < g_info.size() ? "," : "");
  fprintf(f, "  ]\n}\n");
  fclose(f);
}

int main(int argc, char **argv) {
  const char *outdir = argc > 1 ? argv[1] : ".";
  alarm(120);  // watchdog: never hang the shared machine
  test_app_init();
  test_window_size(W, H);
  if (!getenv("SPORE_D3D9")) setenv("SPORE_D3D9", "metal", 1);
  void *win = (void *)1;  // any non-null HWND: the shim maps it to the guest window handle, test_window.m to its layer
  bool ok = run(win, outdir);
  write_json(outdir);
  int fails = 0;
  for (auto &r : g_results) fails += !r.pass;
  printf("SUMMARY %zu tests, %d failed%s\n", g_results.size(), fails, ok ? "" : " (aborted early)");
  test_window_close();
  return ok && fails == 0 ? 0 : 1;
}
