// conformance_shim.cpp -- the guest shim (on our own d3d9_host.h): runs
// test_conformance against the Metal back end through the guest COM vtables.
//
// test_conformance calls D3D9 as a C++ app does (d3d9_host.h). This shim gives it IDirect3D9/IDirect3DDevice9/...
// objects whose every method marshals its arguments into the guest window and calls the guest record's vtable
// slot through runtime/com's test harness (gcall: push the arguments, `call [vtbl+4*slot]` through sg_lookup,
// check the stdcall pop), exactly the path translated game code takes. Behind the vtables is the Metal back end
// (runtime/d3d9metal: src/'s device and resources, pipeline/, shader/'s translator).
//
// Conversions: guest pointers <-> host pointers (mem + a); structs that differ between the 32-bit guest and
// arm64 (D3DPRESENT_PARAMETERS with its HWND, D3DLOCKED_RECT with its pointer); bytecode and declarations are
// copied into guest memory. Returned interfaces are wrapped once per guest record (identity preserved).
// Only what test_conformance calls is implemented; anything else prints "shim: not implemented" and fails.
// Our own code; no game code or data.

#include "d3d9_host.h"

#include <cstdarg>
#include <cstdio>
#include <cstdlib>
#include <cstring>
#include <map>

extern "C" {
#include "com_harness.h"
#include "d3d9metal.h"
#include "d3d9_slots.h"
void hle_stop_check(sg_ctx *ctx, uint8_t *mem, const char *import) { (void)ctx; (void)mem; (void)import; }
}

long d3dhost_unimpl(const char *what) {
  fprintf(stderr, "shim: not implemented: %s\n", what);
  return (long)D3DERR_INVALIDCALL;
}

static uint32_t f2u(float f) { uint32_t u; memcpy(&u, &f, 4); return u; }
static uint8_t *H(uint32_t g) { return g ? M + g : nullptr; }
static uint32_t scratch(uint32_t n) { return gscratch(n ? n : 4); }

// ---- the window: test_window.m's NSWindow and its CAMetalLayer (d3d9metal_host_layer) stand for any HWND ----------
static const uint32_t kHwnd = 0x00010001u;
static uint32_t guest_hwnd(HWND h) { return h ? kHwnd : 0; }

// ---- wrappers ----------------------------------------------------------------------------------------------------
struct Obj { uint32_t g = 0; };
static std::map<uint32_t, void *> g_wrap;

template <class T> static T *wrap(uint32_t g) {
  if (!g) return nullptr;
  auto it = g_wrap.find(g);
  if (it != g_wrap.end()) return (T *)it->second;
  T *o = new T();
  o->g = g;
  g_wrap[g] = (void *)o;
  return o;
}
template <class T> static uint32_t G(T *o) { return o ? o->g : 0; }

#define IUNK(SLOTREL)                                                                                     \
  HRESULT STDMETHODCALLTYPE QueryInterface(REFIID, void **) override { return E_NOINTERFACE; }        \
  ULONG STDMETHODCALLTYPE AddRef() override { return gcall(g, 1, 0); }                                \
  ULONG STDMETHODCALLTYPE Release() override { return gcall(g, 2, 0); }

static HRESULT lockrect(uint32_t g, unsigned slot, int has_level, UINT level, D3DLOCKED_RECT *lr, const RECT *r, DWORD flags) {
  uint32_t glr = scratch(8), gr = 0;
  if (r) { gr = scratch(16); memcpy(H(gr), r, 16); }
  HRESULT hr = has_level ? (HRESULT)gcall(g, slot, 4, level, glr, gr, flags) : (HRESULT)gcall(g, slot, 3, glr, gr, flags);
  if (SUCCEEDED(hr)) { lr->Pitch = (INT)gld(glr); lr->pBits = H(gld(glr + 4)); }
  return hr;
}

struct SSurface : IDirect3DSurface9, Obj {
  IUNK()
  HRESULT STDMETHODCALLTYPE GetDesc(D3DSURFACE_DESC *d) override {
    uint32_t gd = scratch(32);
    HRESULT hr = (HRESULT)gcall(g, S_Surface_GetDesc, 1, gd);
    memcpy(d, H(gd), 32);
    return hr;
  }
  HRESULT STDMETHODCALLTYPE LockRect(D3DLOCKED_RECT *lr, const RECT *r, DWORD f) override { return lockrect(g, S_Surface_LockRect, 0, 0, lr, r, f); }
  HRESULT STDMETHODCALLTYPE UnlockRect() override { return (HRESULT)gcall(g, S_Surface_UnlockRect, 0); }
};
static_assert(sizeof(D3DSURFACE_DESC) == 32, "desc");

struct STexture : IDirect3DTexture9, Obj {
  IUNK()
  HRESULT STDMETHODCALLTYPE GetSurfaceLevel(UINT l, IDirect3DSurface9 **s) override {
    uint32_t o = scratch(4);
    HRESULT hr = (HRESULT)gcall(g, S_Texture_GetSurfaceLevel, 2, l, o);
    *s = wrap<SSurface>(gld(o));
    return hr;
  }
  HRESULT STDMETHODCALLTYPE LockRect(UINT l, D3DLOCKED_RECT *lr, const RECT *r, DWORD f) override { return lockrect(g, S_Texture_LockRect, 1, l, lr, r, f); }
  HRESULT STDMETHODCALLTYPE UnlockRect(UINT l) override { return (HRESULT)gcall(g, S_Texture_UnlockRect, 1, l); }
  DWORD STDMETHODCALLTYPE SetLOD(DWORD l) override { return gcall(g, S_Texture_SetLOD, 1, l); }
  DWORD STDMETHODCALLTYPE GetLevelCount() override { return gcall(g, S_Texture_GetLevelCount, 0); }
};
// Cube textures (the face-edge filtering case). The face goes before the level in the guest call.
struct SCube : IDirect3DCubeTexture9, Obj {
  IUNK()
  HRESULT STDMETHODCALLTYPE LockRect(D3DCUBEMAP_FACES face, UINT l, D3DLOCKED_RECT *lr, const RECT *r, DWORD f) override {
    uint32_t glr = scratch(8), gr = 0;
    if (r) { gr = scratch(16); memcpy(H(gr), r, 16); }
    HRESULT hr = (HRESULT)gcall(g, S_CubeTexture_LockRect, 5, (uint32_t)face, l, glr, gr, f);
    if (SUCCEEDED(hr)) { lr->Pitch = (INT)gld(glr); lr->pBits = H(gld(glr + 4)); }
    return hr;
  }
  HRESULT STDMETHODCALLTYPE UnlockRect(D3DCUBEMAP_FACES face, UINT l) override { return (HRESULT)gcall(g, S_CubeTexture_UnlockRect, 2, (uint32_t)face, l); }
};
// IDirect3DBaseTexture9 pointer -> guest object, for SetTexture with any texture kind
static std::map<void *, uint32_t> g_base;

struct SVB : IDirect3DVertexBuffer9, Obj {
  IUNK()
  HRESULT STDMETHODCALLTYPE Lock(UINT off, UINT size, void **p, DWORD f) override {
    uint32_t o = scratch(4);
    HRESULT hr = (HRESULT)gcall(g, 11 /* Lock */, 4, off, size, o, f);
    *p = H(gld(o));
    return hr;
  }
  HRESULT STDMETHODCALLTYPE Unlock() override { return (HRESULT)gcall(g, 12, 0); }
};
struct SIB : IDirect3DIndexBuffer9, Obj {
  IUNK()
  HRESULT STDMETHODCALLTYPE Lock(UINT off, UINT size, void **p, DWORD f) override {
    uint32_t o = scratch(4);
    HRESULT hr = (HRESULT)gcall(g, 11, 4, off, size, o, f);
    *p = H(gld(o));
    return hr;
  }
  HRESULT STDMETHODCALLTYPE Unlock() override { return (HRESULT)gcall(g, 12, 0); }
};
struct SDecl : IDirect3DVertexDeclaration9, Obj { IUNK() };
struct SVS : IDirect3DVertexShader9, Obj { IUNK() };
struct SPS : IDirect3DPixelShader9, Obj { IUNK() };
struct SQuery : IDirect3DQuery9, Obj {
  IUNK()
  HRESULT STDMETHODCALLTYPE Issue(DWORD f) override { return (HRESULT)gcall(g, 6 /* Issue */, 1, f); }
  HRESULT STDMETHODCALLTYPE GetData(void *data, DWORD size, DWORD f) override {
    uint32_t gd = data ? scratch(size) : 0;
    HRESULT hr = (HRESULT)gcall(g, 7 /* GetData */, 3, gd, size, f);
    if (data && hr == S_OK) memcpy(data, H(gd), size);
    return hr;
  }
};

// The guest base texture of any of our texture-like wrappers (only 2D textures here).
static uint32_t G_tex(IDirect3DBaseTexture9 *t) {
  if (!t) return 0;
  auto it = g_base.find((void *)t);
  if (it != g_base.end()) return it->second;
  return static_cast<STexture *>(static_cast<IDirect3DTexture9 *>(t))->g;
}

static uint32_t put_pp(const D3DPRESENT_PARAMETERS *pp) {
  uint32_t a = scratch(56);
  uint32_t v[14] = {pp->BackBufferWidth, pp->BackBufferHeight, (uint32_t)pp->BackBufferFormat, pp->BackBufferCount,
                    (uint32_t)pp->MultiSampleType, pp->MultiSampleQuality, (uint32_t)pp->SwapEffect, guest_hwnd(pp->hDeviceWindow),
                    (uint32_t)pp->Windowed, (uint32_t)pp->EnableAutoDepthStencil, (uint32_t)pp->AutoDepthStencilFormat, pp->Flags,
                    pp->FullScreen_RefreshRateInHz, pp->PresentationInterval};
  for (int i = 0; i < 14; i++) gst(a + 4 * i, v[i]);
  return a;
}

// Bytecode length: version token .. end token (comments skipped).
static uint32_t code_bytes(const DWORD *c) {
  uint32_t i = 1;
  for (;;) {
    DWORD t = c[i];
    if (t == 0x0000FFFFu) return 4 * (i + 1);
    if ((t & 0xffff) == 0xfffe) i += 1 + (t >> 16);
    else i++;
    if (i > (1u << 20)) return 0;
  }
}
static uint32_t put_blob(const void *p, uint32_t n) { uint32_t a = scratch(n); memcpy(H(a), p, n); return a; }

struct SDevice : IDirect3DDevice9, Obj {
  IUNK()
  HRESULT STDMETHODCALLTYPE CreateVertexShader(const DWORD *code, IDirect3DVertexShader9 **s) override {
    uint32_t o = scratch(4);
    HRESULT hr = (HRESULT)gcall(g, S_Device_CreateVertexShader, 2, put_blob(code, code_bytes(code)), o);
    *s = wrap<SVS>(gld(o));
    return hr;
  }
  HRESULT STDMETHODCALLTYPE CreatePixelShader(const DWORD *code, IDirect3DPixelShader9 **s) override {
    uint32_t o = scratch(4);
    HRESULT hr = (HRESULT)gcall(g, S_Device_CreatePixelShader, 2, put_blob(code, code_bytes(code)), o);
    *s = wrap<SPS>(gld(o));
    return hr;
  }
  HRESULT STDMETHODCALLTYPE CreateVertexDeclaration(const D3DVERTEXELEMENT9 *e, IDirect3DVertexDeclaration9 **d) override {
    uint32_t n = 0;
    while (e[n].Stream != 0xff) n++;
    uint32_t o = scratch(4);
    HRESULT hr = (HRESULT)gcall(g, S_Device_CreateVertexDeclaration, 2, put_blob(e, 8 * (n + 1)), o);
    *d = wrap<SDecl>(gld(o));
    return hr;
  }
  HRESULT STDMETHODCALLTYPE CreateTexture(UINT w, UINT h, UINT l, DWORD u, D3DFORMAT f, D3DPOOL p, IDirect3DTexture9 **t, HANDLE *) override {
    uint32_t o = scratch(4);
    HRESULT hr = (HRESULT)gcall(g, S_Device_CreateTexture, 8, w, h, l, u, (uint32_t)f, (uint32_t)p, o, 0);
    *t = wrap<STexture>(gld(o));
    return hr;
  }
  HRESULT STDMETHODCALLTYPE CreateCubeTexture(UINT e, UINT l, DWORD u, D3DFORMAT f, D3DPOOL p, IDirect3DCubeTexture9 **t, HANDLE *) override {
    uint32_t o = scratch(4);
    HRESULT hr = (HRESULT)gcall(g, S_Device_CreateCubeTexture, 7, e, l, u, (uint32_t)f, (uint32_t)p, o, 0);
    SCube *c = wrap<SCube>(gld(o));
    if (c) g_base[(void *)static_cast<IDirect3DBaseTexture9 *>(static_cast<IDirect3DCubeTexture9 *>(c))] = c->g;
    *t = c;
    return hr;
  }
  HRESULT STDMETHODCALLTYPE DrawPrimitiveUP(D3DPRIMITIVETYPE pt, UINT n, const void *data, UINT stride) override {
    UINT nv = pt == D3DPT_TRIANGLELIST ? 3 * n : pt == D3DPT_TRIANGLESTRIP || pt == D3DPT_TRIANGLEFAN ? n + 2
            : pt == D3DPT_LINELIST ? 2 * n : pt == D3DPT_LINESTRIP ? n + 1 : n;
    return (HRESULT)gcall(g, S_Device_DrawPrimitiveUP, 4, (uint32_t)pt, n, put_blob(data, nv * stride), stride);
  }
  HRESULT STDMETHODCALLTYPE CreateDepthStencilSurface(UINT w, UINT h, D3DFORMAT f, D3DMULTISAMPLE_TYPE ms, DWORD q, BOOL disc,
                                                      IDirect3DSurface9 **s, HANDLE *) override {
    uint32_t o = scratch(4);
    HRESULT hr = (HRESULT)gcall(g, S_Device_CreateDepthStencilSurface, 8, w, h, (uint32_t)f, (uint32_t)ms, q, (uint32_t)disc, o, 0);
    *s = wrap<SSurface>(gld(o));
    return hr;
  }
  HRESULT STDMETHODCALLTYPE CreateOffscreenPlainSurface(UINT w, UINT h, D3DFORMAT f, D3DPOOL p, IDirect3DSurface9 **s, HANDLE *) override {
    uint32_t o = scratch(4);
    HRESULT hr = (HRESULT)gcall(g, S_Device_CreateOffscreenPlainSurface, 6, w, h, (uint32_t)f, (uint32_t)p, o, 0);
    *s = wrap<SSurface>(gld(o));
    return hr;
  }
  HRESULT STDMETHODCALLTYPE CreateVertexBuffer(UINT len, DWORD u, DWORD fvf, D3DPOOL p, IDirect3DVertexBuffer9 **b, HANDLE *) override {
    uint32_t o = scratch(4);
    HRESULT hr = (HRESULT)gcall(g, S_Device_CreateVertexBuffer, 6, len, u, fvf, (uint32_t)p, o, 0);
    *b = wrap<SVB>(gld(o));
    return hr;
  }
  HRESULT STDMETHODCALLTYPE CreateIndexBuffer(UINT len, DWORD u, D3DFORMAT f, D3DPOOL p, IDirect3DIndexBuffer9 **b, HANDLE *) override {
    uint32_t o = scratch(4);
    HRESULT hr = (HRESULT)gcall(g, S_Device_CreateIndexBuffer, 6, len, u, (uint32_t)f, (uint32_t)p, o, 0);
    *b = wrap<SIB>(gld(o));
    return hr;
  }
  HRESULT STDMETHODCALLTYPE CreateQuery(D3DQUERYTYPE t, IDirect3DQuery9 **q) override {
    if (!q) return (HRESULT)gcall(g, S_Device_CreateQuery, 2, (uint32_t)t, 0);
    uint32_t o = scratch(4);
    HRESULT hr = (HRESULT)gcall(g, S_Device_CreateQuery, 2, (uint32_t)t, o);
    *q = wrap<SQuery>(gld(o));
    return hr;
  }
  HRESULT STDMETHODCALLTYPE SetRenderTarget(DWORD i, IDirect3DSurface9 *s) override {
    return (HRESULT)gcall(g, S_Device_SetRenderTarget, 2, i, G(static_cast<SSurface *>(s)));
  }
  HRESULT STDMETHODCALLTYPE SetDepthStencilSurface(IDirect3DSurface9 *s) override {
    return (HRESULT)gcall(g, S_Device_SetDepthStencilSurface, 1, G(static_cast<SSurface *>(s)));
  }
  HRESULT STDMETHODCALLTYPE GetBackBuffer(UINT sc, UINT i, D3DBACKBUFFER_TYPE t, IDirect3DSurface9 **s) override {
    uint32_t o = scratch(4);
    HRESULT hr = (HRESULT)gcall(g, S_Device_GetBackBuffer, 4, sc, i, (uint32_t)t, o);
    *s = wrap<SSurface>(gld(o));
    return hr;
  }
  HRESULT STDMETHODCALLTYPE GetRenderTargetData(IDirect3DSurface9 *a, IDirect3DSurface9 *b) override {
    return (HRESULT)gcall(g, S_Device_GetRenderTargetData, 2, G(static_cast<SSurface *>(a)), G(static_cast<SSurface *>(b)));
  }
  HRESULT STDMETHODCALLTYPE StretchRect(IDirect3DSurface9 *a, const RECT *ra, IDirect3DSurface9 *b, const RECT *rb, D3DTEXTUREFILTERTYPE f) override {
    uint32_t ga = ra ? put_blob(ra, 16) : 0, gb = rb ? put_blob(rb, 16) : 0;
    return (HRESULT)gcall(g, S_Device_StretchRect, 5, G(static_cast<SSurface *>(a)), ga, G(static_cast<SSurface *>(b)), gb, (uint32_t)f);
  }
  HRESULT STDMETHODCALLTYPE SetVertexShaderConstantF(UINT r, const float *c, UINT n) override {
    return (HRESULT)gcall(g, S_Device_SetVertexShaderConstantF, 3, r, put_blob(c, 16 * n), n);
  }
  HRESULT STDMETHODCALLTYPE SetPixelShaderConstantF(UINT r, const float *c, UINT n) override {
    return (HRESULT)gcall(g, S_Device_SetPixelShaderConstantF, 3, r, put_blob(c, 16 * n), n);
  }
  HRESULT STDMETHODCALLTYPE SetVertexDeclaration(IDirect3DVertexDeclaration9 *d) override {
    return (HRESULT)gcall(g, S_Device_SetVertexDeclaration, 1, G(static_cast<SDecl *>(d)));
  }
  HRESULT STDMETHODCALLTYPE SetVertexShader(IDirect3DVertexShader9 *s) override { return (HRESULT)gcall(g, S_Device_SetVertexShader, 1, G(static_cast<SVS *>(s))); }
  HRESULT STDMETHODCALLTYPE SetPixelShader(IDirect3DPixelShader9 *s) override { return (HRESULT)gcall(g, S_Device_SetPixelShader, 1, G(static_cast<SPS *>(s))); }
  HRESULT STDMETHODCALLTYPE SetRenderState(D3DRENDERSTATETYPE s, DWORD v) override { return (HRESULT)gcall(g, S_Device_SetRenderState, 2, (uint32_t)s, v); }
  HRESULT STDMETHODCALLTYPE SetSamplerState(DWORD s, D3DSAMPLERSTATETYPE t, DWORD v) override {
    return (HRESULT)gcall(g, S_Device_SetSamplerState, 3, s, (uint32_t)t, v);
  }
  HRESULT STDMETHODCALLTYPE SetTexture(DWORD s, IDirect3DBaseTexture9 *t) override { return (HRESULT)gcall(g, S_Device_SetTexture, 2, s, G_tex(t)); }
  HRESULT STDMETHODCALLTYPE SetViewport(const D3DVIEWPORT9 *v) override { return (HRESULT)gcall(g, S_Device_SetViewport, 1, put_blob(v, 24)); }
  HRESULT STDMETHODCALLTYPE SetStreamSource(UINT s, IDirect3DVertexBuffer9 *b, UINT off, UINT stride) override {
    return (HRESULT)gcall(g, S_Device_SetStreamSource, 4, s, G(static_cast<SVB *>(b)), off, stride);
  }
  HRESULT STDMETHODCALLTYPE SetStreamSourceFreq(UINT s, UINT f) override { return (HRESULT)gcall(g, S_Device_SetStreamSourceFreq, 2, s, f); }
  HRESULT STDMETHODCALLTYPE SetIndices(IDirect3DIndexBuffer9 *b) override { return (HRESULT)gcall(g, S_Device_SetIndices, 1, G(static_cast<SIB *>(b))); }
  HRESULT STDMETHODCALLTYPE BeginScene() override { return (HRESULT)gcall(g, S_Device_BeginScene, 0); }
  HRESULT STDMETHODCALLTYPE EndScene() override { return (HRESULT)gcall(g, S_Device_EndScene, 0); }
  HRESULT STDMETHODCALLTYPE Clear(DWORD n, const D3DRECT *r, DWORD f, D3DCOLOR c, float z, DWORD s) override {
    uint32_t gr = r && n ? put_blob(r, 16 * n) : 0;
    return (HRESULT)gcall(g, S_Device_Clear, 6, n, gr, f, (uint32_t)c, f2u(z), s);
  }
  HRESULT STDMETHODCALLTYPE DrawPrimitive(D3DPRIMITIVETYPE t, UINT start, UINT n) override {
    return (HRESULT)gcall(g, S_Device_DrawPrimitive, 3, (uint32_t)t, start, n);
  }
  HRESULT STDMETHODCALLTYPE DrawIndexedPrimitive(D3DPRIMITIVETYPE t, INT base, UINT minv, UINT nv, UINT start, UINT n) override {
    return (HRESULT)gcall(g, S_Device_DrawIndexedPrimitive, 6, (uint32_t)t, (uint32_t)base, minv, nv, start, n);
  }
  HRESULT STDMETHODCALLTYPE Present(const RECT *, const RECT *, HWND, const RGNDATA *) override {
    return (HRESULT)gcall(g, S_Device_Present, 4, 0, 0, 0, 0);
  }
  HRESULT STDMETHODCALLTYPE Reset(D3DPRESENT_PARAMETERS *pp) override {
    uint32_t a = put_pp(pp);
    return (HRESULT)gcall(g, S_Device_Reset, 1, a);
  }
};

struct SD3D9 : IDirect3D9, Obj {
  IUNK()
  HRESULT STDMETHODCALLTYPE GetAdapterIdentifier(UINT a, DWORD f, D3DADAPTER_IDENTIFIER9 *id) override {
    uint32_t gi = scratch(1104);
    HRESULT hr = (HRESULT)gcall(g, S_D3D9_GetAdapterIdentifier, 3, a, f, gi);
    memcpy(id, H(gi), 1100);
    return hr;
  }
  HRESULT STDMETHODCALLTYPE GetDeviceCaps(UINT a, D3DDEVTYPE t, D3DCAPS9 *c) override {
    uint32_t gc = scratch(sizeof(D3DCAPS9));
    HRESULT hr = (HRESULT)gcall(g, S_D3D9_GetDeviceCaps, 3, a, (uint32_t)t, gc);
    memcpy(c, H(gc), sizeof(D3DCAPS9));
    return hr;
  }
  HRESULT STDMETHODCALLTYPE CheckDeviceFormat(UINT a, D3DDEVTYPE t, D3DFORMAT af, DWORD u, D3DRESOURCETYPE rt, D3DFORMAT f) override {
    return (HRESULT)gcall(g, S_D3D9_CheckDeviceFormat, 6, a, (uint32_t)t, (uint32_t)af, u, (uint32_t)rt, (uint32_t)f);
  }
  HRESULT STDMETHODCALLTYPE CreateDevice(UINT a, D3DDEVTYPE t, HWND focus, DWORD flags, D3DPRESENT_PARAMETERS *pp, IDirect3DDevice9 **dev) override {
    uint32_t gp = put_pp(pp), o = scratch(4);
    HRESULT hr = (HRESULT)gcall(g, S_D3D9_CreateDevice, 6, a, (uint32_t)t, guest_hwnd(focus), flags, gp, o);
    *dev = wrap<SDevice>(gld(o));
    return hr;
  }
};
static_assert(sizeof(D3DCAPS9) == 304, "caps");

IDirect3D9 *Direct3DCreate9(UINT sdk) {
  static int inited;
  if (!inited) { harness_init(); inited = 1; }
  uint32_t g = d3d9metal_create(&T, M, sdk);
  if (!g) { fprintf(stderr, "shim: d3d9metal_create failed\n"); return nullptr; }
  return wrap<SD3D9>(g);
}
