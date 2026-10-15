/* replay_hitch.m -- a no-game harness for the shader-compile hitches at each load (DEVIATIONS 3.32).
 *
 * Replays the shader traffic of a game session through the real pipeline code (pipeline/pl_shader.m): every
 * shader is created at boot (the game creates all of its 2,067 shaders in the first seconds: 1,272 VS and 795 PS
 * live at t = 5 s in two logged sessions), then the loads draw them for the first time. A first
 * draw is pl_sh_get for the vertex and pixel shader and a render pipeline state built from them (pl_pso_create),
 * all on this thread, which plays the render thread. The stall of a load is the time this thread spends in
 * those calls for the load's first draws, the part of the load's frames that the shader work costs.
 *
 * Inputs (no game files beyond the translation cache the game runs filled):
 *   - the shaders: build/d3d9metal/shadercache/<key>.d3d9msl, in the order they were first written (the order of
 *     first draws across the logged sessions and later play), each one's bytecode and translation;
 *   - the loads: the compile counts of a logged Cell-stage session (watch lines every 5 s: 4, 19, 8, 36, 4, 21, 1, 2,
 *     12, 2 new shaders at t = 5, 20, 30, 35, 75, 80, 85, 90, 95, 100 s). The logs do not name the shaders a load
 *     drew, so the cache's write order assigns them (representative, not exact). The cached shaders beyond
 *     those 109 follow as loads of 20, 5 s apart (later play);
 *   - the shaders the game creates but never draws (2,067 created, 360 ever drawn): stand-ins made by giving a
 *     cached shader's bytecode a comment token, so they are new to the translation cache, as at a real boot.
 *
 * Cold and warm. --salt S makes every shader's code unique to S (a test hook in pl_shader.m adds a branch that
 * is never taken and names S): a new S is a cold macOS Metal cache, the same S again is a warm one, the second
 * launch. "before" is compile at the first draw (SPORE_METAL_SHADER_ASYNC=0, no archive), "after" is today's (asynchronous batches,
 * the pipeline archive at --archive). tests/replay_hitch.sh runs the four cases.
 *
 * Usage: replay_hitch --mode before|after --salt S [--archive PATH] [--scale F] [--extra N] [--loads N] [--quiet]
 * The translation cache must be a private copy (SPORE_D3D9MSL_CACHE; replay_hitch.sh makes one under build/). */
#import <Metal/Metal.h>
#include <dirent.h>
#include <stdio.h>
#include <stdlib.h>
#include <string.h>
#include <sys/stat.h>
#include <unistd.h>

#include "d3d9metal_int.h"
#include "pl_shader.h"
#include "../shader/d3d9msl.h"

int mt_verbose;
extern char *(*pl_sh_test_msl_hook)(const char *msl, size_t len, size_t *out_len);
void d3d9metal_pl_shader_stats(uint64_t *compiles, uint64_t *fails, uint64_t *ns);

static uint64_t g_salt;
static uint64_t now_ns(void) { return clock_gettime_nsec_np(CLOCK_UPTIME_RAW); }

/* The shader's own sg_main gets a branch on a driver constant that is never true (alphaRef is in [0, 1]) and
 * that writes a value naming the salt: the code differs per salt, so neither the front end nor the back end of
 * the macOS cache can have seen it. */
static char *salt_msl(const char *msl, size_t len, size_t *out_len) {
    const char *at = strstr(msl, "    return out;\n");
    const char *set = NULL;
    for (const char *p = strstr(msl, "    out."); p && p < at; p = strstr(p + 1, "    out.")) { set = p; break; }
    if (!at || !set) return NULL;
    char field[64] = {0};
    if (sscanf(set, "    out.%63[A-Za-z0-9_] =", field) != 1) return NULL;
    char line[256];
    int n = snprintf(line, sizeof line, "    if (aux.alphaRef == -%llu.0f) out.%s *= %llu.0f;\n",
                     (unsigned long long)(g_salt % 1000000007ull + 2), field, (unsigned long long)(g_salt % 999983ull + 3));
    if (!strstr(msl, "constant SgAux& aux")) return NULL;
    size_t pre = (size_t)(at - msl);
    char *out = malloc(len + (size_t)n + 1);
    memcpy(out, msl, pre);
    memcpy(out + pre, line, (size_t)n);
    memcpy(out + pre + n, at, len - pre);
    out[len + n] = 0;
    *out_len = len + (size_t)n;
    return out;
}

/* ---- the cached shaders ------------------------------------------------------------------------------- */
typedef struct shader {
    char name[32];
    double birth;
    uint32_t *tok;
    uint32_t ntok;
    int is_ps;
    mt_obj *obj;
} shader;

static int load_entry(const char *path, shader *s) {
    FILE *f = fopen(path, "rb");
    if (!f) return -1;
    uint8_t h[40];
    if (fread(h, 1, 40, f) != 40 || memcmp(h, "D3D9MSL", 7)) { fclose(f); return -1; }
    uint32_t nt;
    memcpy(&nt, h + 12, 4);
    s->tok = malloc(nt * 4u);
    s->ntok = nt;
    int ok = fread(s->tok, 4, nt, f) == nt;
    fclose(f);
    s->is_ps = (s->tok[0] >> 16) == 0xffff;
    return ok ? 0 : -1;
}
static int by_birth(const void *a, const void *b) {
    double x = ((const shader *)a)->birth, y = ((const shader *)b)->birth;
    return x < y ? -1 : x > y;
}
static uint64_t fnv1a(const uint8_t *p, uint32_t n) {
    uint64_t h = 0xcbf29ce484222325ull;
    for (uint32_t i = 0; i < n; i++) h = (h ^ p[i]) * 0x100000001b3ull;
    return h;
}
static uint64_t g_uid = 1;
static mt_obj *make_obj(mt_dev *d, const uint32_t *tok, uint32_t ntok, int is_ps) {
    mt_obj *o = calloc(1, sizeof *o);
    o->kind = is_ps ? MT_K_PS : MT_K_VS;
    o->dev = d;
    o->uid = g_uid++;
    o->code_len = ntok * 4;
    o->code = malloc(o->code_len);
    memcpy(o->code, tok, o->code_len);
    o->code_hash = fnv1a(o->code, o->code_len);
    return o;
}

/* A render pipeline for a first draw: the vertex shader's inputs as float4 attributes of stream 0, one BGRA8
 * target, the alpha-test function off. */
static double g_sh_ms, g_pso_ms;   /* the current load's split: shader compiles and waits / pipeline creation */
static int g_errs;
static double first_draw(mt_dev *d, shader *vs, shader *ps, int *pso_ok, int *from_archive) {
    uint64_t t0 = now_ns();
    pl_sh *v = pl_sh_get(d, vs->obj);
    pl_sh *p = ps ? pl_sh_get(d, ps->obj) : NULL;
    uint64_t t1 = now_ns();
    g_sh_ms += (t1 - t0) / 1e6;
    *pso_ok = 0;
    *from_archive = 0;
    if (v && (!ps || p)) {
        @autoreleasepool {
            MTLRenderPipelineDescriptor *pd = [[MTLRenderPipelineDescriptor alloc] init];
            pd.vertexFunction = v->fn;
            pd.fragmentFunction = p ? pl_sh_fragment(d, p, 8) : nil;
            MTLVertexDescriptor *vd = [MTLVertexDescriptor vertexDescriptor];
            for (unsigned i = 0; i < v->nin; i++) {
                vd.attributes[v->in[i].reg].format = MTLVertexFormatFloat4;
                vd.attributes[v->in[i].reg].offset = 0;
                vd.attributes[v->in[i].reg].bufferIndex = MT_BUF_STREAM0;
            }
            if (v->nin) vd.layouts[MT_BUF_STREAM0].stride = 16;
            pd.vertexDescriptor = vd;
            for (int i = 0; i < 4; i++)
                if (p ? (p->ps_out_mask >> i & 1) : i == 0) pd.colorAttachments[i].pixelFormat = MTLPixelFormatBGRA8Unorm;
            void *err = NULL;
            id<MTLRenderPipelineState> pso = pl_pso_create(d, pd, from_archive, &err);
            *pso_ok = pso != nil;
            if (!pso && g_errs++ < 3) fprintf(stderr, "replay_hitch: pipeline %s+%s: %s\n", vs->name, ps ? ps->name : "-",
                                             err ? ((NSError *)err).localizedDescription.UTF8String : "?");
            [pso release];
            [pd release];
        }
    }
    g_pso_ms += (now_ns() - t1) / 1e6;
    return (now_ns() - t0) / 1e6;
}

/* the vertex shader of the same load (else any earlier one) whose outputs cover the pixel shader's inputs */
static shader *partner(mt_dev *d, shader *ps, shader **vs_list, int nvs) {
    pl_sh *p = (pl_sh *)ps->obj->pl;
    for (int i = nvs - 1; i >= 0; i--) {
        pl_sh *v = (pl_sh *)vs_list[i]->obj->pl;
        if (!p || p->state != 1) return vs_list[i];
        if (!v || v->state != 1) continue;
        int ok = 1;
        for (unsigned k = 0; k < p->nout && ok; k++)
            if (strncmp(p->out[k], "position", 8) && !pl_sh_writes(v, p->out[k])) ok = 0;
        if (ok) return vs_list[i];
    }
    (void)d;
    return nvs ? vs_list[nvs - 1] : NULL;
}

int main(int argc, char **argv) {
    const char *mode = "after", *archive = NULL;
    double scale = 0.25;
    int extra = 1707, max_loads = 1000, quiet = 0;
    for (int i = 1; i < argc; i++) {
        if (!strcmp(argv[i], "--mode") && i + 1 < argc) mode = argv[++i];
        else if (!strcmp(argv[i], "--salt") && i + 1 < argc) g_salt = strtoull(argv[++i], NULL, 0);
        else if (!strcmp(argv[i], "--archive") && i + 1 < argc) archive = argv[++i];
        else if (!strcmp(argv[i], "--scale") && i + 1 < argc) scale = atof(argv[++i]);
        else if (!strcmp(argv[i], "--extra") && i + 1 < argc) extra = atoi(argv[++i]);
        else if (!strcmp(argv[i], "--loads") && i + 1 < argc) max_loads = atoi(argv[++i]);
        else if (!strcmp(argv[i], "--quiet")) quiet = 1;
        else { fprintf(stderr, "usage: replay_hitch --mode before|after --salt S [--archive P] [--scale F] [--extra N] [--loads N]\n"); return 2; }
    }
    int after = !strcmp(mode, "after");
    if (!after) { setenv("SPORE_METAL_SHADER_ASYNC", "0", 1); setenv("SPORE_METAL_PSO_ARCHIVE", "off", 1); }
    else if (archive) setenv("SPORE_METAL_PSO_ARCHIVE", archive, 1);
    else setenv("SPORE_METAL_PSO_ARCHIVE", "off", 1);
    const char *cache = getenv("SPORE_D3D9MSL_CACHE");
    if (!cache || !*cache || strstr(cache, "build/d3d9metal/shadercache")) {
        fprintf(stderr, "replay_hitch: set SPORE_D3D9MSL_CACHE to a private copy of the translation cache\n");
        return 2;
    }
    pl_sh_test_msl_hook = salt_msl;

    /* the cached shaders, oldest first */
    DIR *dir = opendir(cache);
    if (!dir) { perror(cache); return 1; }
    int cap = 1024, n = 0;
    shader *sh = calloc((size_t)cap, sizeof *sh);
    struct dirent *de;
    while ((de = readdir(dir))) {
        size_t l = strlen(de->d_name);
        if (l < 9 || strcmp(de->d_name + l - 8, ".d3d9msl")) continue;
        char path[2048];
        snprintf(path, sizeof path, "%s/%s", cache, de->d_name);
        struct stat st;
        if (stat(path, &st) || n == cap) continue;
        shader *s = &sh[n];
        if (load_entry(path, s)) continue;
        snprintf(s->name, sizeof s->name, "%.16s", de->d_name);
        s->birth = st.st_birthtimespec.tv_sec + st.st_birthtimespec.tv_nsec * 1e-9;
        n++;
    }
    closedir(dir);
    qsort(sh, (size_t)n, sizeof *sh, by_birth);

    @autoreleasepool {
        mt_dev *d = calloc(1, sizeof *d);
        d->mtl = MTLCreateSystemDefaultDevice();
        /* boot: every shader created, the drawn ones interleaved with the never-drawn stand-ins, 100 per frame */
        uint64_t t_boot = now_ns();
        int per = n ? (extra + n - 1) / n : 0, made = 0;
        shader *fill = calloc((size_t)extra + 1, sizeof *fill);
        int nf = 0;
        for (int i = 0; i < n; i++) {
            sh[i].obj = make_obj(d, sh[i].tok, sh[i].ntok, sh[i].is_ps);
            pl_sh_created(d, sh[i].obj);
            if (++made % 100 == 0) pl_sh_frame_end();
            for (int k = 0; k < per && nf < extra; k++) {
                shader *f = &fill[nf];
                const shader *src = &sh[(i * 7 + k * 13) % n];
                /* the same bytecode with a comment token after the version: new to the translation cache */
                f->ntok = src->ntok + 2;
                f->tok = malloc(f->ntok * 4u);
                f->tok[0] = src->tok[0];
                f->tok[1] = 0x0001fffeu;
                f->tok[2] = 0x57503337u ^ (uint32_t)nf ^ (uint32_t)(g_salt * 2654435761u);
                memcpy(f->tok + 3, src->tok + 1, (src->ntok - 1) * 4u);
                f->is_ps = src->is_ps;
                f->obj = make_obj(d, f->tok, f->ntok, f->is_ps);
                pl_sh_created(d, f->obj);
                nf++;
                if (++made % 100 == 0) pl_sh_frame_end();
            }
        }
        pl_sh_frame_end();
        double boot_ms = (now_ns() - t_boot) / 1e6;

        /* the loads */
        static const struct { double t; int n; } k166[] = {{5, 4}, {20, 19}, {30, 8}, {35, 36}, {75, 4}, {80, 21}, {85, 1},
                                                            {90, 2}, {95, 12}, {100, 2}};
        int nl = 0, pos = 0;
        struct { double t; int first, count; double stall; int psos, hits, fails; } L[64];
        for (unsigned i = 0; i < sizeof k166 / sizeof k166[0] && pos < n && nl < 64; i++, nl++) {
            L[nl].t = k166[i].t; L[nl].first = pos; L[nl].count = k166[i].n; pos += k166[i].n;
        }
        for (double t = 105; pos < n && nl < 64; t += 5, nl++) { L[nl].t = t; L[nl].first = pos; L[nl].count = 20; pos += 20; }
        if (nl > max_loads) nl = max_loads;
        double worst = 0, total = 0;
        int all_psos = 0, all_hits = 0;
        shader **vs_seen = calloc((size_t)n, sizeof *vs_seen);
        int nvs = 0;
        for (int l = 0; l < nl; l++) {
            uint64_t due = t_boot + (uint64_t)(L[l].t * scale * 1e9);
            while (now_ns() < due) usleep(2000);
            double stall = 0;
            int psos = 0, hits = 0, fails = 0;
            int end = L[l].first + L[l].count > n ? n : L[l].first + L[l].count;
            /* first the load's vertex shaders, then each pixel shader with a partner */
            g_sh_ms = g_pso_ms = 0;
            for (int i = L[l].first; i < end; i++)
                if (!sh[i].is_ps) {
                    uint64_t t0 = now_ns();
                    pl_sh_get(d, sh[i].obj);
                    stall += (now_ns() - t0) / 1e6;
                    g_sh_ms += (now_ns() - t0) / 1e6;
                    vs_seen[nvs++] = &sh[i];
                }
            for (int i = L[l].first; i < end; i++)
                if (sh[i].is_ps) {
                    uint64_t t0 = now_ns();
                    pl_sh_get(d, sh[i].obj);          /* the pixel shader first: its inputs choose the partner */
                    stall += (now_ns() - t0) / 1e6;
                    g_sh_ms += (now_ns() - t0) / 1e6;
                    shader *v = partner(d, &sh[i], vs_seen, nvs);
                    if (!v) continue;
                    int ok = 0, hit = 0;
                    stall += first_draw(d, v, &sh[i], &ok, &hit);
                    psos += ok; hits += hit; fails += !ok;
                }
            L[l].stall = stall; L[l].psos = psos; L[l].hits = hits; L[l].fails = fails;
            if (stall > worst) worst = stall;
            total += stall;
            all_psos += psos; all_hits += hits;
            if (!quiet)
                printf("  load %2d at %5.1f s: %2d new shaders, %2d pipelines (%d from the archive, %d failed): stall %8.1f ms "
                       "(shaders %.1f, pipelines %.1f)\n", l, L[l].t * scale, end - L[l].first, psos, hits, fails, stall, g_sh_ms, g_pso_ms);
        }
        double save_ms = pl_pso_archive_save(0);
        pl_sh_async_stats a;
        pl_sh_async_get_stats(&a);
        uint64_t c = 0, f = 0, ns = 0;
        d3d9metal_pl_shader_stats(&c, &f, &ns);
        printf("replay_hitch %s salt %llu: %d drawn shaders in %d loads, %d stand-ins created at boot (%.0f ms to create); "
               "longest load stall %.1f ms, all loads %.1f ms; %d pipelines (%d from the archive)\n",
               mode, (unsigned long long)g_salt, pos < n ? pos : n, nl, nf, boot_ms, worst, total, all_psos, all_hits);
        printf("  compiled %llu, failed %llu; async: %llu batches of %.1f, %.1f s on workers, %llu compiled alone at a draw, "
               "%llu raced, %llu waits (longest %.1f ms); archive: %llu hits, %llu misses, %llu added, written in %.1f ms\n",
               (unsigned long long)c, (unsigned long long)f, (unsigned long long)a.batches,
               a.batches ? (double)a.batch_shaders / a.batches : 0.0, a.compile_ns / 1e9, (unsigned long long)a.solo_at_draw,
               (unsigned long long)a.raced, (unsigned long long)a.waited, a.max_wait_ns / 1e6, (unsigned long long)a.pso_archive_hits,
               (unsigned long long)a.pso_archive_misses, (unsigned long long)a.pso_archive_adds, save_ms);
        /* wait for the workers before exiting (their Metal calls must not race process teardown) */
        pl_sh_drain(120000);
    }
    return 0;
}
