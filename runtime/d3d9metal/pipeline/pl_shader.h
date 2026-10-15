/* pl_shader.h -- what the pipeline needs from a translated shader, and the bridge to the translator library
 * (runtime/d3d9metal/shader/d3d9msl.h). pl_shader.m is the only file that knows the translator's API; everything else in
 * pipeline/ uses pl_sh. */
#ifndef PL_SHADER_H
#define PL_SHADER_H

#include "d3d9metal_int.h"

#ifdef __cplusplus
extern "C" {
#endif

/* D3DDECLUSAGE values */
enum { PL_U_POSITION, PL_U_BLENDWEIGHT, PL_U_BLENDINDICES, PL_U_NORMAL, PL_U_PSIZE, PL_U_TEXCOORD, PL_U_TANGENT,
       PL_U_BINORMAL, PL_U_TESSFACTOR, PL_U_POSITIONT, PL_U_COLOR, PL_U_FOG, PL_U_DEPTH, PL_U_SAMPLE };
/* the texture type a sampler declares (D3DSAMPLER_TEXTURE_TYPE >> 27): 2 = 2D, 3 = cube, 4 = volume */
enum { PL_ST_NONE = 0, PL_ST_2D = 2, PL_ST_CUBE = 3, PL_ST_VOLUME = 4 };

typedef struct pl_sh {
    int state;                          /* 0 untried, 1 ok, -1 failed (logged once), 2 queued, 3 compiling */
    int is_ps;
    uint32_t version;                   /* the version token (0xFFFE0300 vs_3_0, 0xFFFF0200 ps_2_0, ...) */
    MT_ID(MTLLibrary) lib;              /* +1 */
    MT_ID(MTLFunction) fn;              /* +1: unspecialised (vertex; fragment with the alpha test off) */
    void *fn_alpha[8];                  /* +1 id<MTLFunction>: fragment specialised for D3DCMPFUNC 1..7 (index f - 1) */
    int alpha_fc;                       /* 1 if the fragment function has the alpha-test function constant */
    /* vertex inputs: attribute `reg` (v<reg>) has (usage, index) */
    uint8_t nin;
    struct { uint8_t reg, usage, index; } in[16];
    /* vertex outputs other than the position, and pixel inputs: link semantics as in [[user(<semantic>)]] */
    uint8_t nout;
    char out[32][20];
    /* fragment outputs: bit n = oC<n> written */
    uint8_t ps_out_mask;
    /* samplers: bit s = sampler s used; type per sampler (PL_ST_*) */
    uint16_t smp_mask;
    uint8_t smp_type[16];
    /* float constants the shader reads: c0 .. c<nconst_f - 1>; rel = relative addressing (read the whole file) */
    uint16_t nconst_f;
    uint8_t rel;
    uint8_t uses_int, uses_bool;
    /* compile at creation on a worker (pl_shader.m "Asynchronous compiles"); the fields below belong to
     * pl_shader.m and are guarded by its lock */
    int refs;                           /* holders: the object, a queued/running batch, a racing solo compile */
    int dead;                           /* the object was freed while a job still held this record */
    int known;                          /* the MSL was already in the translation cache (drawn before) */
    uint64_t frame;                     /* frame of creation: shaders created in the same frame share a library */
    uint64_t hash;                      /* code hash (logging) */
    uint8_t *code;                      /* bytecode copy for the worker (the object may go first) */
    uint32_t code_len;
    struct pl_sh *next;                 /* queue link */
    uint64_t t_create_ns, t_ready_ns;   /* statistics */
} pl_sh;

/* The translated shader for a VS/PS object, kept in o->pl. NULL if the translation or the Metal compile
 * failed (logged once per shader). If the object was queued at creation (pl_sh_created) and its batch
 * has not finished, this waits for the batch or for a compile of this shader alone, whichever ends first,
 * so a draw never waits longer than the compile of its own shader; never skips. An object that was not
 * queued is compiled here, synchronously. */
pl_sh *pl_sh_get(mt_dev *d, mt_obj *o);
/* Called by CreateVertexShader/CreatePixelShader. Queues the shader for translation and compilation on
 * the worker threads (SPORE_METAL_SHADER_ASYNC=0 turns it off: compile at first draw). */
void pl_sh_created(mt_dev *d, mt_obj *o);
/* Frame boundary (Present): closes the frame's batch. */
void pl_sh_frame_end(void);
/* Waits until every queued compile has finished (tests and the hitch harness), at most `ms`; 0 if idle. */
int pl_sh_drain(int ms);
/* Creates a render pipeline state through the pipeline archive (build/d3d9metal/pso-archive/): looked up
 * there first, compiled and added when missing. The archive is written by pl_pso_archive_save(). *from_archive
 * is set to 1 on an archive hit. The archive is a set of files, one per launch that met new pipelines
 * (pl_shader.m, "the pipeline archive"). */
MT_ID(MTLRenderPipelineState) pl_pso_create(mt_dev *d, void *desc /* MTLRenderPipelineDescriptor * */, int *from_archive,
                                            void **err /* NSError ** */);
/* With 0: writes the archive now if pipelines were added (waits for the archive queue); returns the ms spent, -1 if
 * nothing. With > 0: nothing (the queue writes the file by itself 3 s after the last addition). */
double pl_pso_archive_save(int min_interval_ms);
/* Statistics for the watch line: queued, compiled (batches, alone at a draw), waits at a draw and their time. */
typedef struct pl_sh_async_stats {
    uint64_t created, queued, batches, batch_shaders, solo_at_draw, raced, waited, wait_ns, max_wait_ns, compile_ns;
    uint64_t pso_archive_hits, pso_archive_misses, pso_archive_adds, pso_archive_saves;
    uint64_t pso_archive_files;   /* archive files loaded at start (the set, before this launch's file) */
} pl_sh_async_stats;
void pl_sh_async_get_stats(pl_sh_async_stats *s);
/* The fragment function for an alpha-test compare function (1..8; 8 = ALWAYS = off), +0. */
MT_ID(MTLFunction) pl_sh_fragment(mt_dev *d, pl_sh *s, uint32_t alpha_func);
void pl_sh_free(mt_obj *o);

/* 1 if the vertex shader writes the semantic ("color0", "texcoord0", ...). */
int pl_sh_writes(const pl_sh *vs, const char *semantic);

/* The driver constants block (MT_BUF_DRIVER) is the translator's d3d9msl_aux (shader/d3d9msl.h). */

#ifdef __cplusplus
}
#endif
#endif
