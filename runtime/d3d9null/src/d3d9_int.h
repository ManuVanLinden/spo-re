/* d3d9_int.h -- internals of the null Direct3D 9. Written from the public d3d9.h /
 * d3d9types.h / d3d9caps.h documentation (Microsoft Learn); no SDK header is included. */
#ifndef D3D9_INT_H
#define D3D9_INT_H

#include <pthread.h>
#include <stdlib.h>
#include <string.h>

#include "d3d9null.h"
#include "d3d9_slots.h"

/* ---- result codes -------------------------------------------------------------------------- */
#define D3D_OK 0u
#define D3DOK_NOAUTOGEN 0x0876086fu
#define D3DERR_NOTFOUND 0x88760866u
#define D3DERR_MOREDATA 0x88760867u
#define D3DERR_DEVICELOST 0x88760868u
#define D3DERR_NOTAVAILABLE 0x8876086au
#define D3DERR_INVALIDCALL 0x8876086cu
#define D3DERR_OUTOFVIDEOMEMORY 0x8876017cu
#define D3DERR_WASSTILLDRAWING 0x8876021cu

/* ---- enums ----------------------------------------------------------------------------------- */
enum { D3DDEVTYPE_HAL = 1, D3DDEVTYPE_REF = 2, D3DDEVTYPE_SW = 3, D3DDEVTYPE_NULLREF = 4 };
enum {
    D3DRTYPE_SURFACE = 1, D3DRTYPE_VOLUME = 2, D3DRTYPE_TEXTURE = 3, D3DRTYPE_VOLUMETEXTURE = 4,
    D3DRTYPE_CUBETEXTURE = 5, D3DRTYPE_VERTEXBUFFER = 6, D3DRTYPE_INDEXBUFFER = 7
};
enum { D3DPOOL_DEFAULT = 0, D3DPOOL_MANAGED = 1, D3DPOOL_SYSTEMMEM = 2, D3DPOOL_SCRATCH = 3 };
#define D3DUSAGE_RENDERTARGET 0x1u
#define D3DUSAGE_DEPTHSTENCIL 0x2u
#define D3DUSAGE_DYNAMIC 0x200u
#define D3DUSAGE_AUTOGENMIPMAP 0x400u
#define D3DUSAGE_DMAP 0x4000u
#define D3DCREATE_FPU_PRESERVE 0x2u
#define D3DCREATE_PUREDEVICE 0x10u
#define D3DLOCK_READONLY 0x10u
#define D3DLOCK_DISCARD 0x2000u
#define D3DLOCK_NOOVERWRITE 0x1000u
#define D3DGETDATA_FLUSH 0x1u
#define D3DISSUE_END 0x1u
#define D3DISSUE_BEGIN 0x2u
enum {
    D3DQUERYTYPE_VCACHE = 4, D3DQUERYTYPE_RESOURCEMANAGER = 5, D3DQUERYTYPE_VERTEXSTATS = 6, D3DQUERYTYPE_EVENT = 8,
    D3DQUERYTYPE_OCCLUSION = 9, D3DQUERYTYPE_TIMESTAMP = 10, D3DQUERYTYPE_TIMESTAMPDISJOINT = 11,
    D3DQUERYTYPE_TIMESTAMPFREQ = 12
};

#define FOURCC(a, b, c, d) ((uint32_t)(uint8_t)(a) | ((uint32_t)(uint8_t)(b) << 8) | ((uint32_t)(uint8_t)(c) << 16) | ((uint32_t)(uint8_t)(d) << 24))
enum {
    D3DFMT_UNKNOWN = 0, D3DFMT_R8G8B8 = 20, D3DFMT_A8R8G8B8 = 21, D3DFMT_X8R8G8B8 = 22, D3DFMT_R5G6B5 = 23,
    D3DFMT_X1R5G5B5 = 24, D3DFMT_A1R5G5B5 = 25, D3DFMT_A4R4G4B4 = 26, D3DFMT_R3G3B2 = 27, D3DFMT_A8 = 28,
    D3DFMT_A8R3G3B2 = 29, D3DFMT_X4R4G4B4 = 30, D3DFMT_A2B10G10R10 = 31, D3DFMT_A8B8G8R8 = 32, D3DFMT_X8B8G8R8 = 33,
    D3DFMT_G16R16 = 34, D3DFMT_A2R10G10B10 = 35, D3DFMT_A16B16G16R16 = 36, D3DFMT_A8P8 = 40, D3DFMT_P8 = 41,
    D3DFMT_L8 = 50, D3DFMT_A8L8 = 51, D3DFMT_A4L4 = 52, D3DFMT_V8U8 = 60, D3DFMT_L6V5U5 = 61, D3DFMT_X8L8V8U8 = 62,
    D3DFMT_Q8W8V8U8 = 63, D3DFMT_V16U16 = 64, D3DFMT_A2W10V10U10 = 67, D3DFMT_D16_LOCKABLE = 70, D3DFMT_D32 = 71,
    D3DFMT_D15S1 = 73, D3DFMT_D24S8 = 75, D3DFMT_D24X8 = 77, D3DFMT_D24X4S4 = 79, D3DFMT_D16 = 80, D3DFMT_L16 = 81,
    D3DFMT_D32F_LOCKABLE = 82, D3DFMT_D24FS8 = 83, D3DFMT_VERTEXDATA = 100, D3DFMT_INDEX16 = 101,
    D3DFMT_INDEX32 = 102, D3DFMT_Q16W16V16U16 = 110, D3DFMT_R16F = 111, D3DFMT_G16R16F = 112,
    D3DFMT_A16B16G16R16F = 113, D3DFMT_R32F = 114, D3DFMT_G32R32F = 115, D3DFMT_A32B32G32R32F = 116,
    D3DFMT_CxV8U8 = 117
};
#define D3DFMT_DXT1 FOURCC('D', 'X', 'T', '1')
#define D3DFMT_DXT2 FOURCC('D', 'X', 'T', '2')
#define D3DFMT_DXT3 FOURCC('D', 'X', 'T', '3')
#define D3DFMT_DXT4 FOURCC('D', 'X', 'T', '4')
#define D3DFMT_DXT5 FOURCC('D', 'X', 'T', '5')
#define D3DFMT_INST FOURCC('I', 'N', 'S', 'T')

/* ---- guest struct sizes ---------------------------------------------------------------------- */
#define D3DADAPTER_IDENTIFIER9_SIZE 1100
#define D3DCAPS9_SIZE 304
#define D3DPP_DWORDS 14 /* D3DPRESENT_PARAMETERS, 56 bytes */
enum {
    PP_W, PP_H, PP_FMT, PP_COUNT, PP_MS, PP_MSQ, PP_SWAP, PP_HWND, PP_WINDOWED, PP_AUTODS, PP_DSFMT, PP_FLAGS,
    PP_REFRESH, PP_INTERVAL
};

/* ---- formats --------------------------------------------------------------------------------- */
/* Bits per pixel (0 for unknown); for block formats, *block = bytes per 4x4 block. */
unsigned d3d9_fmt_bits(uint32_t fmt, unsigned *block);
int d3d9_fmt_is_depth(uint32_t fmt);
/* CheckDeviceFormat's answer. */
uint32_t d3d9_check_format(uint32_t adapter_fmt, uint32_t usage, uint32_t rtype, uint32_t fmt);
void d3d9_fill_caps(uint8_t *p, uint32_t devtype);
void d3d9_current_mode(uint32_t *w, uint32_t *h, uint32_t *hz);

/* ---- objects --------------------------------------------------------------------------------- */
extern pthread_mutex_t d3d9_lock; /* the one device mutex (the device policy) */
extern sg_com_iface d3d9_iface_d3d9, d3d9_iface_device, d3d9_iface_swapchain, d3d9_iface_texture,
    d3d9_iface_cube, d3d9_iface_volumetex, d3d9_iface_surface, d3d9_iface_volume, d3d9_iface_vb, d3d9_iface_ib,
    d3d9_iface_vdecl, d3d9_iface_vs, d3d9_iface_ps, d3d9_iface_query, d3d9_iface_stateblock, d3d9_iface_d3dxbuf;

typedef struct d3d9_obj {
    sg_com_obj com;
} d3d9_obj;

struct d3d9_dev;
uint32_t d3d9_create_device(sg_com_call *c, d3d9_obj *d3d, uint32_t adapter, uint32_t devtype, uint32_t focus,
                            uint32_t flags, uint32_t pp, uint32_t out);

extern d3d9null_stats d3d9_stats;

#endif
