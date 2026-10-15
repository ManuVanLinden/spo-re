/* pl_maps.c -- D3D9 -> Metal enum mappings for the pipeline package. Pure C, no Metal headers: the
 * Metal values are the public enum values (MTLBlendFactor etc.), checked by static asserts in pl_draw.m.
 * Sources: the D3D9 enum documentation (d3d9types.h, learn.microsoft.com "D3DBLEND", "D3DCMPFUNC",
 * "D3DSTENCILOP", "D3DTEXTUREADDRESS", "D3DDECLTYPE", "D3DPRIMITIVETYPE") and the Metal framework headers
 * (MTLRenderPipeline.h, MTLDepthStencil.h, MTLSampler.h, MTLVertexDescriptor.h, MTLRenderCommandEncoder.h). */
#include <string.h>

#include "pl_int.h"

/* MTLBlendFactor */
enum { BF_ZERO, BF_ONE, BF_SRC_COLOR, BF_INV_SRC_COLOR, BF_SRC_ALPHA, BF_INV_SRC_ALPHA, BF_DST_COLOR,
       BF_INV_DST_COLOR, BF_DST_ALPHA, BF_INV_DST_ALPHA, BF_SRC_ALPHA_SAT, BF_BLEND_COLOR, BF_INV_BLEND_COLOR,
       BF_BLEND_ALPHA, BF_INV_BLEND_ALPHA, BF_SRC1_COLOR, BF_INV_SRC1_COLOR, BF_SRC1_ALPHA, BF_INV_SRC1_ALPHA };

uint32_t pl_blend_factor(uint32_t b, int rt_no_alpha) {
    switch (b) {
    case 1: return BF_ZERO;
    case 2: return BF_ONE;
    case 3: return BF_SRC_COLOR;
    case 4: return BF_INV_SRC_COLOR;
    case 5: return BF_SRC_ALPHA;
    case 6: return BF_INV_SRC_ALPHA;
    case 7: return rt_no_alpha ? BF_ONE : BF_DST_ALPHA;       /* X8 targets read alpha as 1 */
    case 8: return rt_no_alpha ? BF_ZERO : BF_INV_DST_ALPHA;
    case 9: return BF_DST_COLOR;
    case 10: return BF_INV_DST_COLOR;
    case 11: return BF_SRC_ALPHA_SAT;
    case 12: return BF_SRC_ALPHA;                             /* BOTHSRCALPHA (pl_blend_both fixes the dst) */
    case 13: return BF_INV_SRC_ALPHA;                         /* BOTHINVSRCALPHA */
    case 14: return BF_BLEND_COLOR;                           /* BLENDFACTOR */
    case 15: return BF_INV_BLEND_COLOR;
    case 16: return BF_SRC1_COLOR;                            /* SRCCOLOR2 (D3D9Ex dual source) */
    case 17: return BF_INV_SRC1_COLOR;
    default: return BF_ONE;
    }
}

int pl_blend_both(uint32_t src, uint32_t *s, uint32_t *d) {
    if (src == 12) { *s = 5; *d = 6; return 1; }              /* SRCALPHA, INVSRCALPHA */
    if (src == 13) { *s = 6; *d = 5; return 1; }              /* INVSRCALPHA, SRCALPHA */
    return 0;
}

uint32_t pl_blend_op(uint32_t op) {                           /* ADD 1, SUBTRACT 2, REVSUBTRACT 3, MIN 4, MAX 5 */
    return op >= 1 && op <= 5 ? op - 1 : 0;
}

uint32_t pl_color_write_mask(uint32_t m) {                    /* D3D: R 1, G 2, B 4, A 8; Metal: R 8, G 4, B 2, A 1 */
    return ((m & 1) ? 8u : 0) | ((m & 2) ? 4u : 0) | ((m & 4) ? 2u : 0) | ((m & 8) ? 1u : 0);
}

uint32_t pl_compare(uint32_t f) {                             /* NEVER 1 .. ALWAYS 8 -> Never 0 .. Always 7 */
    return f >= 1 && f <= 8 ? f - 1 : 7;
}

uint32_t pl_stencil_op(uint32_t op) {                         /* KEEP 1 .. DECR 8 -> Keep 0 .. DecrementWrap 7 */
    return op >= 1 && op <= 8 ? op - 1 : 0;
}

uint32_t pl_cull(uint32_t c) {
    /* D3D9 culls by winding in window space: CW (2) culls clockwise, CCW (3, the default) counter-clockwise.
     * The encoder's front face is set to clockwise, so CW -> Front, CCW -> Back. */
    return c == 2 ? 1u : c == 3 ? 2u : 0u;
}

uint32_t pl_fill(uint32_t f) {                                /* POINT 1 (no Metal mode: solid), WIREFRAME 2, SOLID 3 */
    return f == 2 ? 1u : 0u;
}

uint32_t pl_address(uint32_t a) {
    switch (a) {
    case 1: return 2;   /* WRAP -> Repeat */
    case 2: return 3;   /* MIRROR -> MirrorRepeat */
    case 3: return 0;   /* CLAMP -> ClampToEdge */
    case 4: return 5;   /* BORDER -> ClampToBorderColor */
    case 5: return 1;   /* MIRRORONCE -> MirrorClampToEdge */
    default: return 2;
    }
}

uint32_t pl_minmag(uint32_t f) { return f == 1 || f == 0 ? 0u : 1u; }   /* NONE/POINT nearest; LINEAR, ANISOTROPIC, ... linear */
uint32_t pl_mip(uint32_t f) { return f == 0 ? 0u : f == 1 ? 1u : 2u; }  /* NONE, POINT, LINEAR+ */

uint32_t pl_border(uint32_t argb, int *exact) {
    uint32_t a = argb >> 24, rgb = argb & 0xffffffu;
    unsigned lum = ((argb >> 16) & 255) + ((argb >> 8) & 255) + (argb & 255);
    if (exact) *exact = (argb == 0) || (argb == 0xff000000u) || (argb == 0xffffffffu);
    (void)rgb;
    if (a < 128) return 0;                                    /* TransparentBlack */
    return lum >= 3 * 128 ? 2u : 1u;                          /* OpaqueWhite / OpaqueBlack */
}

/* MTLVertexFormat values */
enum { VF_INVALID = 0, VF_UCHAR4 = 3, VF_UCHAR4N = 9, VF_SHORT2 = 16, VF_SHORT4 = 18, VF_USHORT2N = 19,
       VF_USHORT4N = 21, VF_SHORT2N = 22, VF_SHORT4N = 24, VF_HALF2 = 25, VF_HALF4 = 27, VF_FLOAT = 28,
       VF_FLOAT2 = 29, VF_FLOAT3 = 30, VF_FLOAT4 = 31, VF_INT1010102N = 40, VF_UCHAR4N_BGRA = 42 };

uint32_t pl_vertex_format(uint32_t t, uint32_t *size) {
    static const struct { uint8_t vf, size; } m[] = {
        {VF_FLOAT, 4}, {VF_FLOAT2, 8}, {VF_FLOAT3, 12}, {VF_FLOAT4, 16},
        {VF_UCHAR4N_BGRA, 4},      /* D3DCOLOR: bytes B,G,R,A in memory, read as (R,G,B,A) */
        {VF_UCHAR4, 4},            /* UBYTE4: 0..255 as float (blend indices) */
        {VF_SHORT2, 4}, {VF_SHORT4, 8}, {VF_UCHAR4N, 4}, {VF_SHORT2N, 4}, {VF_SHORT4N, 8},
        {VF_USHORT2N, 4}, {VF_USHORT4N, 8},
        {VF_INVALID, 4},           /* UDEC3: unsigned 10.10.10 unnormalised, no Metal format (not in our DeclTypes caps) */
        {VF_INT1010102N, 4},       /* DEC3N: w differs (Metal reads the 2-bit field, D3D9 gives 1); not in our caps */
        {VF_HALF2, 4}, {VF_HALF4, 8},
    };
    if (t >= sizeof m / sizeof m[0]) { if (size) *size = 0; return VF_INVALID; }
    if (size) *size = m[t].size;
    return m[t].vf;
}

/* MTLPrimitiveType: Point 0, Line 1, LineStrip 2, Triangle 3, TriangleStrip 4 */
int pl_topology(uint32_t pt, uint32_t n, uint32_t *prim, uint32_t *count, int *fan) {
    *fan = 0;
    switch (pt) {
    case PL_PT_POINTLIST: *prim = 0; *count = n; return 1;
    case PL_PT_LINELIST: *prim = 1; *count = 2 * n; return 1;
    case PL_PT_LINESTRIP: *prim = 2; *count = n + 1; return 1;
    case PL_PT_TRIANGLELIST: *prim = 3; *count = 3 * n; return 1;
    case PL_PT_TRIANGLESTRIP: *prim = 4; *count = n + 2; return 1;
    case PL_PT_TRIANGLEFAN: *prim = 3; *count = 3 * n; *fan = 1; return 1;
    default: return 0;
    }
}

void pl_fan_to_list(uint32_t n, const void *idx, uint32_t isz, uint32_t first, uint32_t *out) {
    for (uint32_t i = 0; i < n; i++) {
        uint32_t a, b, c;
        if (!idx) { a = first; b = first + i + 1; c = first + i + 2; }
        else if (isz == 2) { const uint16_t *s = (const uint16_t *)idx; a = s[0]; b = s[i + 1]; c = s[i + 2]; }
        else { const uint32_t *s = (const uint32_t *)idx; a = s[0]; b = s[i + 1]; c = s[i + 2]; }
        /* D3D9 fan triangle i is (v0, v[i+1], v[i+2]): the same winding as the first triangle */
        out[3 * i] = a; out[3 * i + 1] = b; out[3 * i + 2] = c;
    }
}

int pl_has_restart(const void *idx, uint32_t isz, uint32_t n) {
    if (isz == 2) {
        const uint16_t *s = (const uint16_t *)idx;
        uint16_t acc = 0;
        for (uint32_t i = 0; i < n; i++) acc |= (uint16_t)(s[i] == 0xffff);   /* branch-free: vectorises */
        return acc != 0;
    }
    const uint32_t *s = (const uint32_t *)idx;
    uint32_t acc = 0;
    for (uint32_t i = 0; i < n; i++) acc |= (uint32_t)(s[i] == 0xffffffffu);
    return acc != 0;
}

void pl_argb_to_float(uint32_t c, float rgba[4]) {
    rgba[0] = ((c >> 16) & 255) / 255.0f;
    rgba[1] = ((c >> 8) & 255) / 255.0f;
    rgba[2] = (c & 255) / 255.0f;
    rgba[3] = (c >> 24) / 255.0f;
}
