/* d3d9msl.h -- Direct3D 9 shader bytecode (vs/ps 2_0, 2_x, 3_0) to Metal Shading Language, at run time.
 *
 * A C port of tools/shaders/msl.py (the reference; d3dbc.py for the token parser). For the same bytecode and
 * options both emit the same MSL text byte for byte, and the same reflection (tests/crosscheck.py checks every
 * fixture and, with the corpus present, every unique game shader).
 *
 *   d3d9msl_result r;
 *   if (d3d9msl_translate(tokens, ntokens, &r) == 0) { ... r.msl, r.refl ... }  else  r.error
 *   d3d9msl_result_free(&r);
 *
 * Reentrant: no global state except the cache directory (set once, mutex-protected).
 *
 * ---- Binding model (matches runtime/d3d9metal/include/d3d9metal_int.h, MT_BUF_* / MT_TEX_*) ----------------
 *
 * Per stage, in the Metal buffer argument table:
 *   buffer(0)  D3D9MSL_BUF_CONST_F  constant float4 c[]   float constants c0..c255 (VS) / c0..c223 (PS),
 *                                                       16 bytes each, register n at byte 16*n.
 *   buffer(1)  D3D9MSL_BUF_CONST_I  constant int4 ci[16]  integer constants i0..i15, 16 bytes each.
 *   buffer(2)  D3D9MSL_BUF_CONST_B  constant uint cb[16]  bool constants b0..b15, one 32-bit word each (0 / 1).
 *   buffer(3)  D3D9MSL_BUF_AUX      constant SgAux&       driver constants: d3d9msl_aux below (592 bytes).
 *   buffer(14+k) D3D9MSL_BUF_STREAM0 + k  vertex stream k (k = 0..15; the vertex descriptor's layouts).
 * The three register files are three buffers, not one struct, so that each fits setVertexBytes /
 * setFragmentBytes (4 KB limit: VS c[] is exactly 4096 bytes, PS c[] 3584). Upload only up to the highest
 * register used (d3d9msl_reflection.c_max, or the whole file when relative_c is set).
 *   [[attribute(n)]] = input register v<n> (VS); the reflection lists usage/index for each.
 *   texture(n) / sampler(n) = sampler s<n> (PS: s0..s15; VS: vertex texture sampler n, i.e. guest sampler
 *     D3DVERTEXTEXTURESAMPLER0 + n = 257 + n).
 *   [[function_constant(0)]] uint  D3D9MSL_FC_ALPHA_FUNC: D3DCMPFUNC of the alpha test (PS; unset, 0 or 8 = off).
 *   VS outputs / PS inputs link by name: [[user(<usage><index>)]], e.g. user(texcoord3), user(color0), user(fog0).
 *
 * ---- What the runtime sets in d3d9msl_aux per draw ---------------------------------------------------------
 *   alpha_ref     D3DRS_ALPHAREF / 255 (PS; compared at 8-bit precision, as D3D9 drivers do)
 *   fog_color     D3DRS_FOGCOLOR as RGBA 0..1 (PS < 3_0 with D3D9MSL_OPT_FOG_VERTEX)
 *   fog_params    start, end, density, mode (reserved; only vertex fog is emitted today)
 *   rt_size       width, height, 1/width, 1/height of render target 0
 *   pos_fixup     VS: added to the position output as pos.xy += pos_fixup.xy * pos.w. D3D9 puts pixel
 *                 centres at integer window coordinates, Metal at +0.5: a D3D9 vertex must land half a pixel
 *                 right and down in Metal. Set (+1/vpWidth, -1/vpHeight) (viewport size in pixels), OR leave it
 *                 zero and add +0.5 to the Metal viewport originX/originY instead (what the pipeline does). Never both.
 *                 (not (-1/w, +1/h): that is the opposite shift.)
 *   samp[n]       per-sampler state Metal samplers cannot hold (d3d9msl_sampler):
 *     border      D3DSAMP_BORDERCOLOR as RGBA 0..1. Used only where a D3D9MSL_SAMP_BORDER_* bit is set:
 *                 set the bit (and give the Metal sampler ClampToEdge on that axis) when the address mode is
 *                 D3DTADDRESS_BORDER and the colour is not one Metal's MTLSamplerBorderColor can express
 *                 (transparent black, opaque black, opaque white); otherwise use the Metal border colour.
 *                 The shader replaces the fetch by the border colour when the coordinate on that axis is outside
 *                 [0,1] (no blend in the outermost half texel: a known approximation).
 *     mip_bias    D3DSAMP_MIPMAPLODBIAS as a float (Metal samplers have no LOD bias). Applied to texld/texldb
 *                 (implicit LOD) and texldl (explicit LOD, as MSDN's texldl page says), not to texldd.
 *     flags       D3D9MSL_SAMP_SRGB: D3DSAMP_SRGBTEXTURE, decode RGB after the fetch (alternatively bind an sRGB
 *                 texture view and leave it clear: filtering then happens in linear space, as on hardware);
 *                 D3D9MSL_SAMP_DEPTH_CMP: a depth texture is bound (D24S8/D16 etc.): return (ref <= depth) as
 *                 D3D9 shadow-map hardware does, ref = coord.z (texldp: z/w). No PCF under LINEAR.
 *   Everything else about sampling (filters, MAXANISOTROPY, MAXMIPLEVEL -> lodMinClamp, WRAP/MIRROR/CLAMP/
 *   MIRRORONCE address modes) is a Metal sampler property; format channel defaults (L8, A8, X8R8G8B8 ...) are
 *   texture-view swizzles. An unbound sampler must still have a texture bound (a 1x1 (0,0,0,1)).
 */
#ifndef D3D9MSL_H
#define D3D9MSL_H

#include <stddef.h>
#include <stdint.h>

#ifdef __cplusplus
extern "C" {
#endif

#define D3D9MSL_VERSION "1"          /* bumped with any change to the emitted text; part of the cache key */

/* buffer and function-constant indices (see above) */
#define D3D9MSL_BUF_CONST_F 0
#define D3D9MSL_BUF_CONST_I 1
#define D3D9MSL_BUF_CONST_B 2
#define D3D9MSL_BUF_AUX 3
#define D3D9MSL_BUF_STREAM0 14
#define D3D9MSL_FC_ALPHA_FUNC 0
#define D3D9MSL_VS_NCONSTF 256
#define D3D9MSL_PS_NCONSTF 224
#define D3D9MSL_NSAMPLERS 16

/* d3d9msl_sampler.flags */
#define D3D9MSL_SAMP_SRGB 1u
#define D3D9MSL_SAMP_BORDER_U 2u
#define D3D9MSL_SAMP_BORDER_V 4u
#define D3D9MSL_SAMP_BORDER_W 8u
#define D3D9MSL_SAMP_DEPTH_CMP 16u

typedef struct d3d9msl_sampler {     /* MSL struct SgSampler, 32 bytes */
    float border[4];
    float mip_bias;
    uint32_t flags;
    float pad[2];
} d3d9msl_sampler;

typedef struct d3d9msl_aux {         /* MSL struct SgAux, buffer(3), 592 bytes */
    float alpha_ref;
    float pad0[3];
    float fog_color[4];
    float fog_params[4];
    float rt_size[4];
    float pos_fixup[4];
    d3d9msl_sampler samp[D3D9MSL_NSAMPLERS];
} d3d9msl_aux;

/* options */
#define D3D9MSL_OPT_FOG_VERTEX 1u    /* ps < 3_0: blend oC0.rgb with aux.fog_color by the VS fog output (user(fog0)) */
#define D3D9MSL_OPT_FLAT 2u          /* D3DSHADE_FLAT: colour inputs of the PS get [[flat]] */
#define D3D9MSL_OPT_PLAIN 4u         /* test mode: an ordinary function (ALU-only VS), for compute wrappers */
#define D3D9MSL_OPT_NO_PRELUDE 8u    /* omit the shared prelude (concatenating several shaders in one library) */

typedef struct d3d9msl_options {
    const char *entry;               /* function name; NULL = "sg_main". [A-Za-z_][A-Za-z0-9_]*, < 64 chars */
    uint32_t flags;                  /* D3D9MSL_OPT_* */
} d3d9msl_options;

#define D3D9MSL_MAX_IO 32

typedef struct d3d9msl_input {
    char reg[8];                     /* "v3" (VS and ps_3_0), "t0" / "d1" (ps_2 texture / colour inputs) */
    uint8_t regtype;                 /* D3DSPR: 1 input, 3 texture (ps_2) */
    uint8_t regnum;
    uint8_t usage;                   /* D3DDECLUSAGE (ps_2: TEXCOORD for t#, COLOR for v#) */
    uint8_t usage_index;
    uint8_t attribute;               /* VS: [[attribute(n)]] = regnum */
    uint8_t centroid;                /* PS: centroid_perspective */
    uint8_t flat;                    /* PS: [[flat]] (D3D9MSL_OPT_FLAT on a colour input) */
    uint8_t pad;
} d3d9msl_input;

enum { D3D9MSL_OUT_USER, D3D9MSL_OUT_POSITION, D3D9MSL_OUT_POINT_SIZE, D3D9MSL_OUT_COLOR, D3D9MSL_OUT_DEPTH };

typedef struct d3d9msl_output {
    char name[12];                   /* MSL member: oPos oFog oPts oD0 oT1 o3 oC0 oDepth */
    char binding[28];                /* "[[user(texcoord3)]]", "[[position]]", "[[color(0)]]" ... */
    uint8_t kind;                    /* D3D9MSL_OUT_* */
    uint8_t regtype, regnum;
    uint8_t scalar;                  /* float, not float4 (oFog, oPts, psize, oDepth) */
    char semantic[20];               /* USER: "<usage><index>" as in user(...), e.g. "texcoord3", "fog0" */
} d3d9msl_output;

typedef struct d3d9msl_reflection {
    uint8_t stage;                   /* 0 vertex, 1 pixel */
    uint8_t major, minor;
    char profile[12];                /* "vs_3_0", "ps_2_x" ... */
    int32_t c_max;                   /* highest float constant read directly (def'd ones included), -1 none */
    uint8_t relative_c;              /* c[a0.x+n] / c[aL+n] reads: upload the whole float file */
    uint32_t c_used[64];             /* bit n: float constant n read from buffer(0) (def'd ones excluded) */
    uint32_t i_used[64];             /* int constants read from buffer(1) */
    uint32_t b_used[64];             /* bool constants read from buffer(2) */
    uint16_t defs;                   /* def + defi + defb count */
    uint8_t ninputs, noutputs;
    d3d9msl_input inputs[D3D9MSL_MAX_IO];
    d3d9msl_output outputs[D3D9MSL_MAX_IO];
    uint32_t sampler_mask;           /* bit n: s<n> declared (texture(n)/sampler(n) arguments present) */
    uint8_t sampler_type[32];        /* D3DSAMPLER_TEXTURE_TYPE >> 27: 2 2d, 3 cube, 4 volume */
    uint8_t uses_vpos, uses_vface, uses_texkill;
    uint8_t position_fixup;          /* VS: applies aux.pos_fixup */
    uint8_t alpha_test;              /* PS writes oC0 and applies the alpha test */
    uint8_t fog_input;               /* PS reads user(fog0) as float (D3D9MSL_OPT_FOG_VERTEX, ps < 3_0) */
    uint32_t options;                /* the D3D9MSL_OPT_* used */
} d3d9msl_reflection;

typedef struct d3d9msl_result {
    char *msl;                       /* NUL-terminated MSL source (malloc), NULL on failure */
    size_t msl_len;
    d3d9msl_reflection refl;
    char entry[64];                  /* the entry point's name */
    char error[256];                 /* on failure: why */
} d3d9msl_result;

/* return codes */
#define D3D9MSL_OK 0
#define D3D9MSL_EPARSE (-1)          /* malformed bytecode */
#define D3D9MSL_EUNSUPPORTED (-2)    /* valid bytecode the translator does not handle (SM1, predication, ...) */
#define D3D9MSL_ENOMEM (-3)
#define D3D9MSL_EARG (-4)

int d3d9msl_translate(const uint32_t *tokens, size_t ntokens, d3d9msl_result *out);
int d3d9msl_translate_opts(const uint32_t *tokens, size_t ntokens, const d3d9msl_options *opts,
                           d3d9msl_result *out);
void d3d9msl_result_free(d3d9msl_result *r);

/* Number of tokens from the version token through the end token, or 0 if no end token within max_tokens.
 * (CreateVertexShader/CreatePixelShader receive only a pointer.) */
size_t d3d9msl_length(const uint32_t *tokens, size_t max_tokens);

/* FNV-1a 64 over the bytecode bytes (the same hash as mt_obj.code_hash in d3d9metal_int.h). */
uint64_t d3d9msl_hash(const uint32_t *tokens, size_t ntokens);

/* The reflection as JSON, the same document tools/shaders/msl.py's translate() returns as a dict
 * (malloc'd; NULL on allocation failure). */
char *d3d9msl_reflection_json(const d3d9msl_reflection *r, const char *entry);

/* The MSL prelude every shader starts with (unless D3D9MSL_OPT_NO_PRELUDE): structs, sampling helpers. */
const char *d3d9msl_prelude(void);

/* ---- compile cache ------------------------------------------------------------------------------------
 * Files <dir>/<key>.d3d9msl, key = 16 hex digits of FNV-1a 64 over (bytecode, D3D9MSL_VERSION, options, entry).
 * Each file holds the bytecode (compared on lookup, so a hash collision is a miss), the options, the
 * reflection record and the MSL. Written to a temporary name and renamed (safe with concurrent processes).
 * Compiled Metal libraries are not stored: there is no MTLLibrary serialisation without the offline `metal`
 * tool, and newLibraryWithSource results are cached by the OS's Metal shader cache anyway; MTLBinaryArchive
 * (pipelines) is the pipeline package's choice.
 * Directory: d3d9msl_cache_init(dir); dir NULL = $SPORE_D3D9MSL_CACHE, else
 * $SPORE_REPO_ROOT (or the build's SG_REPO_ROOT) + "/build/d3d9metal/shadercache". "off" disables. */
int d3d9msl_cache_init(const char *dir);      /* 0 ok (created if needed), -1 disabled/unusable */
const char *d3d9msl_cache_dir(void);          /* NULL when disabled */
void d3d9msl_cache_key(const uint32_t *tokens, size_t ntokens, const d3d9msl_options *opts, char key[17]);
/* 1 hit (out filled), 0 miss, <0 error (treated as a miss by d3d9msl_translate_cached). */
int d3d9msl_cache_load(const uint32_t *tokens, size_t ntokens, const d3d9msl_options *opts, d3d9msl_result *out);
int d3d9msl_cache_store(const uint32_t *tokens, size_t ntokens, const d3d9msl_options *opts,
                        const d3d9msl_result *res);
/* Look up, else translate and store. *hit = 1 when served from the cache. Same return codes as translate.
 * Initialises the cache directory with the defaults on first use if d3d9msl_cache_init was not called. */
int d3d9msl_translate_cached(const uint32_t *tokens, size_t ntokens, const d3d9msl_options *opts,
                             d3d9msl_result *out, int *hit);
typedef struct d3d9msl_cache_stats { uint64_t hits, misses, stores, errors; } d3d9msl_cache_stats;
void d3d9msl_cache_get_stats(d3d9msl_cache_stats *s);

#ifdef __cplusplus
}
#endif
#endif
