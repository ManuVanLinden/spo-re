/* pl_int.h -- internal header of the Metal back end's pipeline package (runtime/d3d9metal/pipeline/).
 *
 * The pipeline turns the device layer's stored D3D9 state (mt_state in d3d9metal_int.h) into Metal objects at draw time:
 * render pipeline states (cached by vertex shader, pixel shader, vertex layout, blend, colour formats, depth
 * format, sample count and the alpha-test function), depth-stencil states, sampler states, the vertex
 * descriptor from the D3D9 declaration, constant uploads, topology conversion (triangle fans, primitive
 * restart), the UP-draw scratch path and Clear.
 *
 * Two layers:
 *   pl_maps.c  pure C: every D3D9 -> Metal enum mapping as numbers (no Metal headers), unit-tested on its own
 *              (tests/test_pl_maps.c). Values of the Metal enums are the public ones of the Metal headers
 *              (MTLBlendFactor, MTLCompareFunction, ...); pl_draw.m checks them with static asserts.
 *   pl_draw.m  Objective-C (-fno-objc-arc, as the whole package): caches, handlers, draws, clear.
 *   pl_shader.m the bridge to the shader library (shader/d3d9msl.h): MTLFunction + reflection per shader.
 */
#ifndef PL_INT_H
#define PL_INT_H

#include <stdint.h>

#ifdef __cplusplus
extern "C" {
#endif

/* ---- D3D9 enum values used here (public d3d9types.h values) ------------------------------------------- */
enum {
    PL_RS_ZENABLE = 7, PL_RS_FILLMODE = 8, PL_RS_SHADEMODE = 9, PL_RS_ZWRITEENABLE = 14, PL_RS_ALPHATESTENABLE = 15,
    PL_RS_LASTPIXEL = 16, PL_RS_SRCBLEND = 19, PL_RS_DESTBLEND = 20, PL_RS_CULLMODE = 22, PL_RS_ZFUNC = 23,
    PL_RS_ALPHAREF = 24, PL_RS_ALPHAFUNC = 25, PL_RS_DITHERENABLE = 26, PL_RS_ALPHABLENDENABLE = 27,
    PL_RS_FOGENABLE = 28, PL_RS_SPECULARENABLE = 29, PL_RS_FOGCOLOR = 34, PL_RS_FOGTABLEMODE = 35,
    PL_RS_FOGSTART = 36, PL_RS_FOGEND = 37, PL_RS_FOGDENSITY = 38, PL_RS_RANGEFOGENABLE = 48,
    PL_RS_STENCILENABLE = 52, PL_RS_STENCILFAIL = 53, PL_RS_STENCILZFAIL = 54, PL_RS_STENCILPASS = 55,
    PL_RS_STENCILFUNC = 56, PL_RS_STENCILREF = 57, PL_RS_STENCILMASK = 58, PL_RS_STENCILWRITEMASK = 59,
    PL_RS_TEXTUREFACTOR = 60, PL_RS_WRAP0 = 128, PL_RS_CLIPPING = 136, PL_RS_LIGHTING = 137,
    PL_RS_FOGVERTEXMODE = 140, PL_RS_CLIPPLANEENABLE = 152, PL_RS_POINTSIZE = 154, PL_RS_POINTSPRITEENABLE = 156,
    PL_RS_MULTISAMPLEANTIALIAS = 161, PL_RS_MULTISAMPLEMASK = 162, PL_RS_COLORWRITEENABLE = 168,
    PL_RS_BLENDOP = 171, PL_RS_SCISSORTESTENABLE = 174, PL_RS_SLOPESCALEDEPTHBIAS = 175,
    PL_RS_TWOSIDEDSTENCILMODE = 185, PL_RS_CCW_STENCILFAIL = 186, PL_RS_CCW_STENCILZFAIL = 187,
    PL_RS_CCW_STENCILPASS = 188, PL_RS_CCW_STENCILFUNC = 189, PL_RS_COLORWRITEENABLE1 = 190,
    PL_RS_COLORWRITEENABLE2 = 191, PL_RS_COLORWRITEENABLE3 = 192, PL_RS_BLENDFACTOR = 193,
    PL_RS_SRGBWRITEENABLE = 194, PL_RS_DEPTHBIAS = 195, PL_RS_WRAP8 = 198,
    PL_RS_SEPARATEALPHABLENDENABLE = 206, PL_RS_SRCBLENDALPHA = 207, PL_RS_DESTBLENDALPHA = 208,
    PL_RS_BLENDOPALPHA = 209
};
enum {
    PL_SAMP_ADDRESSU = 1, PL_SAMP_ADDRESSV = 2, PL_SAMP_ADDRESSW = 3, PL_SAMP_BORDERCOLOR = 4,
    PL_SAMP_MAGFILTER = 5, PL_SAMP_MINFILTER = 6, PL_SAMP_MIPFILTER = 7, PL_SAMP_MIPMAPLODBIAS = 8,
    PL_SAMP_MAXMIPLEVEL = 9, PL_SAMP_MAXANISOTROPY = 10, PL_SAMP_SRGBTEXTURE = 11
};
enum { PL_PT_POINTLIST = 1, PL_PT_LINELIST, PL_PT_LINESTRIP, PL_PT_TRIANGLELIST, PL_PT_TRIANGLESTRIP, PL_PT_TRIANGLEFAN };
#define PL_D3DFMT_INDEX16 101u
#define PL_D3DFMT_INDEX32 102u
#define PL_SSF_INDEXEDDATA (1u << 30)
#define PL_SSF_INSTANCEDATA (2u << 30)
#define PL_CLEAR_TARGET 1u
#define PL_CLEAR_ZBUFFER 2u
#define PL_CLEAR_STENCIL 4u
enum {                                       /* D3DDECLTYPE */
    PL_DT_FLOAT1, PL_DT_FLOAT2, PL_DT_FLOAT3, PL_DT_FLOAT4, PL_DT_D3DCOLOR, PL_DT_UBYTE4, PL_DT_SHORT2,
    PL_DT_SHORT4, PL_DT_UBYTE4N, PL_DT_SHORT2N, PL_DT_SHORT4N, PL_DT_USHORT2N, PL_DT_USHORT4N, PL_DT_UDEC3,
    PL_DT_DEC3N, PL_DT_FLOAT16_2, PL_DT_FLOAT16_4, PL_DT_UNUSED
};

/* ---- mappings (pl_maps.c). Each returns the Metal enum's numeric value. ------------------------------- */
/* D3DBLEND -> MTLBlendFactor. alpha_slot: the factor is used for the alpha channel (SRCCOLOR -> SRCALPHA
 * equivalence is Metal's own, nothing to do). rt_no_alpha: the render target has no alpha channel
 * (X8R8G8B8, R5G6B5): D3D9 reads destination alpha as 1 there. */
uint32_t pl_blend_factor(uint32_t d3dblend, int rt_no_alpha);
/* The BOTHSRCALPHA (12) / BOTHINVSRCALPHA (13) source factors also set the destination: returns 1 and the
 * D3DBLEND pair (src, dst) to use instead, else 0. */
int pl_blend_both(uint32_t src, uint32_t *src_out, uint32_t *dst_out);
uint32_t pl_blend_op(uint32_t d3dblendop);              /* D3DBLENDOP -> MTLBlendOperation */
uint32_t pl_color_write_mask(uint32_t d3dmask);        /* D3DCOLORWRITEENABLE bits -> MTLColorWriteMask */
uint32_t pl_compare(uint32_t d3dcmp);                  /* D3DCMPFUNC -> MTLCompareFunction */
uint32_t pl_stencil_op(uint32_t d3dsop);               /* D3DSTENCILOP -> MTLStencilOperation */
uint32_t pl_cull(uint32_t d3dcull);                    /* D3DCULL -> MTLCullMode (front face = clockwise) */
uint32_t pl_fill(uint32_t d3dfill);                    /* D3DFILLMODE -> MTLTriangleFillMode */
/* D3DTEXTUREADDRESS -> MTLSamplerAddressMode */
uint32_t pl_address(uint32_t d3dtaddr);
/* D3DTEXTUREFILTERTYPE (MAG/MIN) -> MTLSamplerMinMagFilter; MIP -> MTLSamplerMipFilter */
uint32_t pl_minmag(uint32_t d3dtexf);
uint32_t pl_mip(uint32_t d3dtexf);
/* D3DSAMP_BORDERCOLOR (ARGB) -> MTLSamplerBorderColor (TransparentBlack 0, OpaqueBlack 1, OpaqueWhite 2):
 * the nearest of the three; *exact = 0 when the colour is none of them. */
uint32_t pl_border(uint32_t argb, int *exact);
/* D3DDECLTYPE -> MTLVertexFormat (0 = Invalid: no Metal equivalent); *size = bytes in the vertex. */
uint32_t pl_vertex_format(uint32_t decltype_, uint32_t *size);
/* D3DPRIMITIVETYPE + primitive count -> MTLPrimitiveType and the vertex (or index) count to draw.
 * Fans report the triangle list they become (3 * n) with *fan = 1. Returns 0 for an unknown type. */
int pl_topology(uint32_t d3dpt, uint32_t prims, uint32_t *mtl_prim, uint32_t *count, int *fan);
/* Fills `out` (3 * prims entries) with the triangle list of a fan whose vertices are first..first+prims+1
 * (non-indexed: idx == NULL) or the fan's indices idx[0..prims+1] (16- or 32-bit, isz 2/4). */
void pl_fan_to_list(uint32_t prims, const void *idx, uint32_t isz, uint32_t first, uint32_t *out);
/* 1 if any of the n indices equals the Metal primitive-restart value of its size (0xFFFF, 0xFFFFFFFF). */
int pl_has_restart(const void *idx, uint32_t isz, uint32_t n);
/* D3DCOLOR -> float RGBA (Clear, blend factor, fog colour). */
void pl_argb_to_float(uint32_t argb, float rgba[4]);

#ifdef __cplusplus
}
#endif
#endif
