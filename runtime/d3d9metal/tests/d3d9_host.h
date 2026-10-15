/* d3d9_host.h -- our own host-side C++ declarations of the Direct3D 9 API subset test_conformance uses.
 *
 * test_conformance.cpp is written the way a C++ Direct3D 9 application is, and conformance_shim.cpp gives it
 * objects whose methods marshal into the guest window and call the guest COM vtables of the Metal back end
 * (the path translated game code takes). Neither needs the platform SDK's d3d9.h: this header declares only the
 * types, constants and methods they use, with the values and layouts of the public Direct3D 9 API (the 32-bit
 * guest layouts are written out by the shim; static_asserts there and below pin the sizes that matter).
 * An interface method the shim does not implement reports "shim: not implemented" and fails (d3dhost_unimpl).
 * Our own declarations; no SDK header text. */
#pragma once
#include <stddef.h>
#include <stdint.h>

/* ---- Win32 basics ---------------------------------------------------------------------------------------- */
typedef int32_t HRESULT;
typedef uint32_t DWORD, UINT, ULONG;
typedef int32_t INT, LONG, BOOL;
typedef uint16_t WORD;
typedef uint8_t BYTE;
typedef float FLOAT;
typedef void *HWND, *HANDLE, *HMONITOR;
typedef struct { uint32_t Data1; uint16_t Data2, Data3; uint8_t Data4[8]; } GUID;
typedef const GUID &REFIID;
typedef union { struct { uint32_t LowPart; int32_t HighPart; }; int64_t QuadPart; } LARGE_INTEGER;
typedef struct { LONG left, top, right, bottom; } RECT;
typedef struct { LONG x, y; } POINT;
typedef struct RGNDATA RGNDATA;
#define WINAPI
#define STDMETHODCALLTYPE
#define TRUE 1
#define FALSE 0
#define S_OK ((HRESULT)0)
#define S_FALSE ((HRESULT)1)
#define E_NOINTERFACE ((HRESULT)0x80004002u)
#define SUCCEEDED(hr) (((HRESULT)(hr)) >= 0)
#define FAILED(hr) (((HRESULT)(hr)) < 0)
#define MAKEFOURCC(a, b, c, d) ((DWORD)(BYTE)(a) | ((DWORD)(BYTE)(b) << 8) | ((DWORD)(BYTE)(c) << 16) | ((DWORD)(BYTE)(d) << 24))

/* ---- Direct3D 9 constants -------------------------------------------------------------------------------- */
#define D3D_SDK_VERSION 32
#define D3D_OK S_OK
#define D3DERR_INVALIDCALL ((HRESULT)0x8876086Cu)
typedef DWORD D3DCOLOR;
#define D3DCOLOR_ARGB(a, r, g, b) ((D3DCOLOR)((((a) & 0xff) << 24) | (((r) & 0xff) << 16) | (((g) & 0xff) << 8) | ((b) & 0xff)))
#define D3DVS_VERSION(major, minor) (0xFFFE0000u | ((major) << 8) | (minor))
#define D3DPS_VERSION(major, minor) (0xFFFF0000u | ((major) << 8) | (minor))
#define D3DSHADER_VERSION_MAJOR(v) (((v) >> 8) & 0xff)
#define D3DSHADER_VERSION_MINOR(v) ((v) & 0xff)
#define D3DCREATE_HARDWARE_VERTEXPROCESSING 0x40u
#define D3DCLEAR_TARGET 1u
#define D3DCLEAR_ZBUFFER 2u
#define D3DLOCK_READONLY 0x10u
#define D3DLOCK_NOOVERWRITE 0x1000u
#define D3DLOCK_DISCARD 0x2000u
#define D3DUSAGE_RENDERTARGET 0x1u
#define D3DUSAGE_DEPTHSTENCIL 0x2u
#define D3DUSAGE_WRITEONLY 0x8u
#define D3DUSAGE_DYNAMIC 0x200u
#define D3DUSAGE_AUTOGENMIPMAP 0x400u
#define D3DUSAGE_QUERY_VERTEXTEXTURE 0x100000u
#define D3DPRESENT_INTERVAL_IMMEDIATE 0x80000000u
#define D3DDEVCAPS2_STREAMOFFSET 0x1u
#define D3DGETDATA_FLUSH 0x1u
#define D3DISSUE_END 0x1u
#define D3DSTREAMSOURCE_INDEXEDDATA (1u << 30)
#define D3DSTREAMSOURCE_INSTANCEDATA (2u << 30)
#define D3DVERTEXTEXTURESAMPLER0 257u
#define D3DZB_TRUE 1u

typedef enum { D3DDEVTYPE_HAL = 1 } D3DDEVTYPE;
typedef enum {
    D3DFMT_A8R8G8B8 = 21, D3DFMT_X8R8G8B8 = 22, D3DFMT_D24S8 = 75, D3DFMT_INDEX16 = 101, D3DFMT_R32F = 114,
    D3DFMT_A32B32G32R32F = 116,
    D3DFMT_DXT1 = MAKEFOURCC('D', 'X', 'T', '1'), D3DFMT_DXT3 = MAKEFOURCC('D', 'X', 'T', '3'),
    D3DFMT_DXT5 = MAKEFOURCC('D', 'X', 'T', '5'),
    D3DFMT_FORCE_DWORD = 0x7fffffff
} D3DFORMAT;
typedef enum { D3DPOOL_DEFAULT = 0, D3DPOOL_MANAGED = 1, D3DPOOL_SYSTEMMEM = 2 } D3DPOOL;
typedef enum { D3DMULTISAMPLE_NONE = 0 } D3DMULTISAMPLE_TYPE;
typedef enum { D3DSWAPEFFECT_COPY = 3 } D3DSWAPEFFECT;
typedef enum { D3DRTYPE_SURFACE = 1, D3DRTYPE_TEXTURE = 3 } D3DRESOURCETYPE;
typedef enum { D3DBACKBUFFER_TYPE_MONO = 0 } D3DBACKBUFFER_TYPE;
typedef enum { D3DQUERYTYPE_EVENT = 8 } D3DQUERYTYPE;
typedef enum {
    D3DPT_LINELIST = 2, D3DPT_LINESTRIP = 3, D3DPT_TRIANGLELIST = 4, D3DPT_TRIANGLESTRIP = 5, D3DPT_TRIANGLEFAN = 6
} D3DPRIMITIVETYPE;
typedef enum {
    D3DRS_ZENABLE = 7, D3DRS_ZWRITEENABLE = 14, D3DRS_CULLMODE = 22, D3DRS_ZFUNC = 23, D3DRS_ALPHABLENDENABLE = 27,
    D3DRS_LIGHTING = 137
} D3DRENDERSTATETYPE;
typedef enum {
    D3DSAMP_ADDRESSU = 1, D3DSAMP_ADDRESSV = 2, D3DSAMP_ADDRESSW = 3, D3DSAMP_MAGFILTER = 5, D3DSAMP_MINFILTER = 6,
    D3DSAMP_MIPFILTER = 7, D3DSAMP_MIPMAPLODBIAS = 8, D3DSAMP_MAXMIPLEVEL = 9, D3DSAMP_MAXANISOTROPY = 10
} D3DSAMPLERSTATETYPE;
typedef enum { D3DTEXF_NONE = 0, D3DTEXF_POINT = 1, D3DTEXF_LINEAR = 2, D3DTEXF_ANISOTROPIC = 3 } D3DTEXTUREFILTERTYPE;
enum { D3DTADDRESS_WRAP = 1, D3DTADDRESS_CLAMP = 3 };
enum { D3DCULL_NONE = 1 };
typedef enum {
    D3DCMP_NEVER = 1, D3DCMP_LESS = 2, D3DCMP_EQUAL = 3, D3DCMP_LESSEQUAL = 4, D3DCMP_GREATER = 5, D3DCMP_NOTEQUAL = 6,
    D3DCMP_GREATEREQUAL = 7, D3DCMP_ALWAYS = 8
} D3DCMPFUNC;
typedef enum {
    D3DCUBEMAP_FACE_POSITIVE_X = 0, D3DCUBEMAP_FACE_NEGATIVE_X = 1, D3DCUBEMAP_FACE_POSITIVE_Y = 2,
    D3DCUBEMAP_FACE_NEGATIVE_Y = 3, D3DCUBEMAP_FACE_POSITIVE_Z = 4, D3DCUBEMAP_FACE_NEGATIVE_Z = 5
} D3DCUBEMAP_FACES;
enum {
    D3DDECLTYPE_FLOAT1 = 0, D3DDECLTYPE_FLOAT2 = 1, D3DDECLTYPE_FLOAT3 = 2, D3DDECLTYPE_FLOAT4 = 3,
    D3DDECLTYPE_D3DCOLOR = 4, D3DDECLTYPE_UNUSED = 17
};
enum { D3DDECLMETHOD_DEFAULT = 0 };
enum { D3DDECLUSAGE_POSITION = 0, D3DDECLUSAGE_TEXCOORD = 5, D3DDECLUSAGE_COLOR = 10 };

/* ---- structures (host layouts; the shim converts where the 32-bit guest's differ) ------------------------ */
typedef struct { WORD Stream, Offset; BYTE Type, Method, Usage, UsageIndex; } D3DVERTEXELEMENT9;
#define D3DDECL_END() {0xFF, 0, D3DDECLTYPE_UNUSED, 0, 0, 0}
typedef struct { DWORD X, Y, Width, Height; float MinZ, MaxZ; } D3DVIEWPORT9;
typedef struct { LONG x1, y1, x2, y2; } D3DRECT;
typedef struct { INT Pitch; void *pBits; } D3DLOCKED_RECT;
typedef struct {
    D3DFORMAT Format; D3DRESOURCETYPE Type; DWORD Usage; D3DPOOL Pool; D3DMULTISAMPLE_TYPE MultiSampleType;
    DWORD MultiSampleQuality; UINT Width, Height;
} D3DSURFACE_DESC;
typedef struct {
    UINT BackBufferWidth, BackBufferHeight; D3DFORMAT BackBufferFormat; UINT BackBufferCount;
    D3DMULTISAMPLE_TYPE MultiSampleType; DWORD MultiSampleQuality; D3DSWAPEFFECT SwapEffect; HWND hDeviceWindow;
    BOOL Windowed, EnableAutoDepthStencil; D3DFORMAT AutoDepthStencilFormat; DWORD Flags;
    UINT FullScreen_RefreshRateInHz, PresentationInterval;
} D3DPRESENT_PARAMETERS;
typedef struct {
    char Driver[512], Description[512], DeviceName[32];
    LARGE_INTEGER DriverVersion;
    DWORD VendorId, DeviceId, SubSysId, Revision;
    GUID DeviceIdentifier;
    DWORD WHQLLevel;
} D3DADAPTER_IDENTIFIER9;
typedef struct { DWORD Caps; INT DynamicFlowControlDepth, NumTemps, StaticFlowControlDepth; } D3DVSHADERCAPS2_0;
typedef struct { DWORD Caps; INT DynamicFlowControlDepth, NumTemps, StaticFlowControlDepth, NumInstructionSlots; } D3DPSHADERCAPS2_0;
typedef struct {
    D3DDEVTYPE DeviceType; UINT AdapterOrdinal;
    DWORD Caps, Caps2, Caps3, PresentationIntervals, CursorCaps, DevCaps, PrimitiveMiscCaps, RasterCaps, ZCmpCaps,
        SrcBlendCaps, DestBlendCaps, AlphaCmpCaps, ShadeCaps, TextureCaps, TextureFilterCaps, CubeTextureFilterCaps,
        VolumeTextureFilterCaps, TextureAddressCaps, VolumeTextureAddressCaps, LineCaps, MaxTextureWidth,
        MaxTextureHeight, MaxVolumeExtent, MaxTextureRepeat, MaxTextureAspectRatio, MaxAnisotropy;
    float MaxVertexW, GuardBandLeft, GuardBandTop, GuardBandRight, GuardBandBottom, ExtentsAdjust;
    DWORD StencilCaps, FVFCaps, TextureOpCaps, MaxTextureBlendStages, MaxSimultaneousTextures, VertexProcessingCaps,
        MaxActiveLights, MaxUserClipPlanes, MaxVertexBlendMatrices, MaxVertexBlendMatrixIndex;
    float MaxPointSize;
    DWORD MaxPrimitiveCount, MaxVertexIndex, MaxStreams, MaxStreamStride, VertexShaderVersion, MaxVertexShaderConst,
        PixelShaderVersion;
    float PixelShader1xMaxValue;
    DWORD DevCaps2;
    float MaxNpatchTessellationLevel;
    DWORD Reserved5;
    UINT MasterAdapterOrdinal, AdapterOrdinalInGroup, NumberOfAdaptersInGroup;
    DWORD DeclTypes, NumSimultaneousRTs, StretchRectFilterCaps;
    D3DVSHADERCAPS2_0 VS20Caps;
    D3DPSHADERCAPS2_0 PS20Caps;
    DWORD VertexTextureFilterCaps, MaxVShaderInstructionsExecuted, MaxPShaderInstructionsExecuted,
        MaxVertexShader30InstructionSlots, MaxPixelShader30InstructionSlots;
} D3DCAPS9;
static_assert(sizeof(D3DCAPS9) == 304, "D3DCAPS9 is 304 bytes in every ABI");
static_assert(sizeof(D3DSURFACE_DESC) == 32, "D3DSURFACE_DESC");
static_assert(sizeof(D3DVERTEXELEMENT9) == 8, "D3DVERTEXELEMENT9");
static_assert(sizeof(D3DVIEWPORT9) == 24, "D3DVIEWPORT9");
static_assert(offsetof(D3DADAPTER_IDENTIFIER9, DriverVersion) == 1056 && offsetof(D3DADAPTER_IDENTIFIER9, WHQLLevel) == 1096,
              "D3DADAPTER_IDENTIFIER9 (the guest's layout up to its tail padding)");

/* ---- interfaces: only the methods test_conformance calls; the rest are left out --------------------------- */
long d3dhost_unimpl(const char *what); /* conformance_shim.cpp: prints and returns D3DERR_INVALIDCALL */
#define D3DHOST_DEFAULT(iface, name) { return (HRESULT)d3dhost_unimpl(#iface "::" #name); }

struct IUnknown {
    virtual HRESULT QueryInterface(REFIID, void **) = 0;
    virtual ULONG AddRef() = 0;
    virtual ULONG Release() = 0;
    virtual ~IUnknown() {}
};
struct IDirect3DSurface9 : IUnknown {
    virtual HRESULT GetDesc(D3DSURFACE_DESC *) D3DHOST_DEFAULT(IDirect3DSurface9, GetDesc)
    virtual HRESULT LockRect(D3DLOCKED_RECT *, const RECT *, DWORD) D3DHOST_DEFAULT(IDirect3DSurface9, LockRect)
    virtual HRESULT UnlockRect() D3DHOST_DEFAULT(IDirect3DSurface9, UnlockRect)
};
struct IDirect3DBaseTexture9 : IUnknown {
    virtual DWORD SetLOD(DWORD) { return (DWORD)d3dhost_unimpl("IDirect3DBaseTexture9::SetLOD"); }
    virtual DWORD GetLevelCount() { return (DWORD)d3dhost_unimpl("IDirect3DBaseTexture9::GetLevelCount"); }
};
struct IDirect3DTexture9 : IDirect3DBaseTexture9 {
    virtual HRESULT GetSurfaceLevel(UINT, IDirect3DSurface9 **) D3DHOST_DEFAULT(IDirect3DTexture9, GetSurfaceLevel)
    virtual HRESULT LockRect(UINT, D3DLOCKED_RECT *, const RECT *, DWORD) D3DHOST_DEFAULT(IDirect3DTexture9, LockRect)
    virtual HRESULT UnlockRect(UINT) D3DHOST_DEFAULT(IDirect3DTexture9, UnlockRect)
};
struct IDirect3DCubeTexture9 : IDirect3DBaseTexture9 {
    virtual HRESULT LockRect(D3DCUBEMAP_FACES, UINT, D3DLOCKED_RECT *, const RECT *, DWORD) D3DHOST_DEFAULT(IDirect3DCubeTexture9, LockRect)
    virtual HRESULT UnlockRect(D3DCUBEMAP_FACES, UINT) D3DHOST_DEFAULT(IDirect3DCubeTexture9, UnlockRect)
};
struct IDirect3DVertexBuffer9 : IUnknown {
    virtual HRESULT Lock(UINT, UINT, void **, DWORD) D3DHOST_DEFAULT(IDirect3DVertexBuffer9, Lock)
    virtual HRESULT Unlock() D3DHOST_DEFAULT(IDirect3DVertexBuffer9, Unlock)
};
struct IDirect3DIndexBuffer9 : IUnknown {
    virtual HRESULT Lock(UINT, UINT, void **, DWORD) D3DHOST_DEFAULT(IDirect3DIndexBuffer9, Lock)
    virtual HRESULT Unlock() D3DHOST_DEFAULT(IDirect3DIndexBuffer9, Unlock)
};
struct IDirect3DVertexDeclaration9 : IUnknown {};
struct IDirect3DVertexShader9 : IUnknown {};
struct IDirect3DPixelShader9 : IUnknown {};
struct IDirect3DQuery9 : IUnknown {
    virtual HRESULT Issue(DWORD) D3DHOST_DEFAULT(IDirect3DQuery9, Issue)
    virtual HRESULT GetData(void *, DWORD, DWORD) D3DHOST_DEFAULT(IDirect3DQuery9, GetData)
};
struct IDirect3DDevice9 : IUnknown {
#define M_(name, args) virtual HRESULT name args D3DHOST_DEFAULT(IDirect3DDevice9, name)
    M_(CreateVertexShader, (const DWORD *, IDirect3DVertexShader9 **))
    M_(CreatePixelShader, (const DWORD *, IDirect3DPixelShader9 **))
    M_(CreateVertexDeclaration, (const D3DVERTEXELEMENT9 *, IDirect3DVertexDeclaration9 **))
    M_(CreateTexture, (UINT, UINT, UINT, DWORD, D3DFORMAT, D3DPOOL, IDirect3DTexture9 **, HANDLE *))
    M_(CreateCubeTexture, (UINT, UINT, DWORD, D3DFORMAT, D3DPOOL, IDirect3DCubeTexture9 **, HANDLE *))
    M_(CreateDepthStencilSurface, (UINT, UINT, D3DFORMAT, D3DMULTISAMPLE_TYPE, DWORD, BOOL, IDirect3DSurface9 **, HANDLE *))
    M_(CreateOffscreenPlainSurface, (UINT, UINT, D3DFORMAT, D3DPOOL, IDirect3DSurface9 **, HANDLE *))
    M_(CreateVertexBuffer, (UINT, DWORD, DWORD, D3DPOOL, IDirect3DVertexBuffer9 **, HANDLE *))
    M_(CreateIndexBuffer, (UINT, DWORD, D3DFORMAT, D3DPOOL, IDirect3DIndexBuffer9 **, HANDLE *))
    M_(CreateQuery, (D3DQUERYTYPE, IDirect3DQuery9 **))
    M_(SetRenderTarget, (DWORD, IDirect3DSurface9 *))
    M_(SetDepthStencilSurface, (IDirect3DSurface9 *))
    M_(GetBackBuffer, (UINT, UINT, D3DBACKBUFFER_TYPE, IDirect3DSurface9 **))
    M_(GetRenderTargetData, (IDirect3DSurface9 *, IDirect3DSurface9 *))
    M_(StretchRect, (IDirect3DSurface9 *, const RECT *, IDirect3DSurface9 *, const RECT *, D3DTEXTUREFILTERTYPE))
    M_(SetVertexShaderConstantF, (UINT, const float *, UINT))
    M_(SetPixelShaderConstantF, (UINT, const float *, UINT))
    M_(SetVertexDeclaration, (IDirect3DVertexDeclaration9 *))
    M_(SetVertexShader, (IDirect3DVertexShader9 *))
    M_(SetPixelShader, (IDirect3DPixelShader9 *))
    M_(SetRenderState, (D3DRENDERSTATETYPE, DWORD))
    M_(SetSamplerState, (DWORD, D3DSAMPLERSTATETYPE, DWORD))
    M_(SetTexture, (DWORD, IDirect3DBaseTexture9 *))
    M_(SetViewport, (const D3DVIEWPORT9 *))
    M_(SetStreamSource, (UINT, IDirect3DVertexBuffer9 *, UINT, UINT))
    M_(SetStreamSourceFreq, (UINT, UINT))
    M_(SetIndices, (IDirect3DIndexBuffer9 *))
    M_(BeginScene, ())
    M_(EndScene, ())
    M_(Clear, (DWORD, const D3DRECT *, DWORD, D3DCOLOR, float, DWORD))
    M_(DrawPrimitive, (D3DPRIMITIVETYPE, UINT, UINT))
    M_(DrawIndexedPrimitive, (D3DPRIMITIVETYPE, INT, UINT, UINT, UINT, UINT))
    M_(DrawPrimitiveUP, (D3DPRIMITIVETYPE, UINT, const void *, UINT))
    M_(Present, (const RECT *, const RECT *, HWND, const RGNDATA *))
    M_(Reset, (D3DPRESENT_PARAMETERS *))
#undef M_
};
struct IDirect3D9 : IUnknown {
    virtual HRESULT GetAdapterIdentifier(UINT, DWORD, D3DADAPTER_IDENTIFIER9 *) D3DHOST_DEFAULT(IDirect3D9, GetAdapterIdentifier)
    virtual HRESULT GetDeviceCaps(UINT, D3DDEVTYPE, D3DCAPS9 *) D3DHOST_DEFAULT(IDirect3D9, GetDeviceCaps)
    virtual HRESULT CheckDeviceFormat(UINT, D3DDEVTYPE, D3DFORMAT, DWORD, D3DRESOURCETYPE, D3DFORMAT) D3DHOST_DEFAULT(IDirect3D9, CheckDeviceFormat)
    virtual HRESULT CreateDevice(UINT, D3DDEVTYPE, HWND, DWORD, D3DPRESENT_PARAMETERS *, IDirect3DDevice9 **) D3DHOST_DEFAULT(IDirect3D9, CreateDevice)
};
IDirect3D9 *Direct3DCreate9(UINT sdk);
