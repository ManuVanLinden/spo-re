/* test_pl_archive.m -- the pipeline archive (pl_shader.m, "the pipeline archive") across
 * launches. A launch is a child process of this test (the archive is opened once per process, as in the game):
 * it builds some render pipeline states through the real path -- our own test shaders (tests/shaders) translated
 * and compiled by pl_sh_get, a descriptor like the pipeline's, pl_pso_create -- then writes the archive with
 * pl_pso_archive_save(0) and prints the archive counters. No game code or data.
 *
 * The bug this guards against: Metal's archiver refuses to write an archive that was loaded from a file and then given
 * new pipelines ("expecting 'fragment' stage in pipeline no. N"), for some pipelines and not others, depending on
 * the functions' names and code. So no loaded archive is written: each launch writes its new pipelines
 * to a new file of a set, and looks pipelines up in all of them.
 *
 * Scenarios (each from an empty directory):
 *   pairs     launch 1 builds pipeline i, launch 2 builds pipeline j, launch 3 builds both and must find both in
 *             the archive; for every ordered pair i != j of the pipelines below (one has the alpha test on, two
 *             are depth only, no fragment function).
 *   batch     launch 1 compiles the shaders in one batch (pl_sh_created + pl_sh_frame_end), launch 2 alone at
 *             the draw, launch 3 batch again: the same shader under both paths, all hits in launch 3.
 *   legacy    the default archive path (next to the translation cache) holds an archive under the file name older
 *             builds used (<GPU>.metalar): it is deleted, <GPU>.1.metalar is written and the next launch hits.
 *   grow      six launches, one new pipeline each: six files, and a seventh launch hits all six pipelines and
 *             writes nothing.
 *   compact   with SPORE_METAL_PSO_ARCHIVE_FILES=3, the launch after three files compacts them into one file of
 *             the pipelines it used (hits and new), and the next launch hits those.
 *
 * Usage: test_pl_archive WORKDIR   (the Makefile passes build/d3d9metal/pipeline/archive-test) */
#import <Metal/Metal.h>
#include <glob.h>
#include <spawn.h>
#include <stdio.h>
#include <stdlib.h>
#include <string.h>
#include <sys/stat.h>
#include <sys/wait.h>
#include <unistd.h>

#include "d3d9metal_int.h"
#include "../pl_shader.h"
#include "../../shader/d3d9msl.h"
#include "pl_test_shaders.h"

extern char **environ;
int mt_verbose = 0;
static int pass, fail;
#define CHECK(c, ...) do { if (c) pass++; else { fail++; printf("FAIL %s:%d: ", __FILE__, __LINE__); printf(__VA_ARGS__); printf("\n"); } } while (0)

/* ---- one launch ---------------------------------------------------------------------------------------- */
enum { VS_POS, VS_UBYTE, PS_COL, PS_TEX, NSH };
static const struct { const uint32_t *tok; unsigned n; } k_sh[NSH] = {
    {k_vs_pos, sizeof k_vs_pos / 4}, {k_vs_ubyte, sizeof k_vs_ubyte / 4},
    {k_ps_col, sizeof k_ps_col / 4}, {k_ps_tex, sizeof k_ps_tex / 4}};
/* the pipelines: vertex shader, pixel shader (-1: none, depth only), alpha-test function (8 = off) */
static const struct { int vs, ps; uint32_t alpha; } k_pl[] = {
    {VS_POS, PS_COL, 8}, {VS_POS, PS_TEX, 8}, {VS_UBYTE, PS_COL, 8}, {VS_POS, PS_COL, 5}, {VS_UBYTE, -1, 8},
    {VS_POS, -1, 8}};
#define NPL (int)(sizeof k_pl / sizeof k_pl[0])

static uint64_t fnv1a(const void *p, size_t n) {
    const uint8_t *b = p;
    uint64_t h = 0xcbf29ce484222325ull;
    for (size_t i = 0; i < n; i++) h = (h ^ b[i]) * 0x100000001b3ull;
    return h;
}
static mt_obj *make_obj(mt_dev *d, int i) {
    static uint64_t uid = 1;
    mt_obj *o = calloc(1, sizeof *o);
    o->kind = i >= PS_COL ? MT_K_PS : MT_K_VS;
    o->dev = d;
    o->uid = uid++;
    o->code_len = k_sh[i].n * 4;
    o->code = malloc(o->code_len);
    memcpy(o->code, k_sh[i].tok, o->code_len);
    o->code_hash = fnv1a(o->code, o->code_len);
    return o;
}

/* launch: builds the pipelines in `mask`, writes the archive, prints "stats built hits misses adds saves files" */
static int launch(unsigned mask, int batch) {
    @autoreleasepool {
        static mt_dev D;
        D.mtl = MTLCreateSystemDefaultDevice();
        if (!D.mtl) { printf("no Metal device\n"); return 2; }
        mt_obj *sh[NSH];
        for (int i = 0; i < NSH; i++) sh[i] = make_obj(&D, i);
        if (batch) {   /* created together and compiled in one batch on the workers, as at the game's boot */
            for (int i = 0; i < NSH; i++) pl_sh_created(&D, sh[batch == 2 ? NSH - 1 - i : i]);   /* 2: other order */
            pl_sh_frame_end();
            if (pl_sh_drain(20000)) printf("shader compiles did not finish\n");
        }
        int built = 0;
        for (int k = 0; k < NPL; k++) {
            if (!(mask >> k & 1)) continue;
            pl_sh *v = pl_sh_get(&D, sh[k_pl[k].vs]);
            pl_sh *p = k_pl[k].ps >= 0 ? pl_sh_get(&D, sh[k_pl[k].ps]) : NULL;
            if (!v || (k_pl[k].ps >= 0 && !p)) { printf("pipeline %d: shader failed\n", k); continue; }
            MTLRenderPipelineDescriptor *pd = [[MTLRenderPipelineDescriptor alloc] init];
            pd.vertexFunction = v->fn;
            pd.fragmentFunction = p ? pl_sh_fragment(&D, p, k_pl[k].alpha) : nil;
            MTLVertexDescriptor *vd = [MTLVertexDescriptor vertexDescriptor];
            for (unsigned i = 0; i < v->nin; i++) {
                vd.attributes[v->in[i].reg].format = MTLVertexFormatFloat4;
                vd.attributes[v->in[i].reg].bufferIndex = MT_BUF_STREAM0;
            }
            if (v->nin) vd.layouts[MT_BUF_STREAM0].stride = 16;
            pd.vertexDescriptor = vd;
            if (p) pd.colorAttachments[0].pixelFormat = MTLPixelFormatBGRA8Unorm;
            pd.depthAttachmentPixelFormat = MTLPixelFormatDepth32Float;
            NSError *e = nil;
            int from_archive = 0;
            id<MTLRenderPipelineState> pso = pl_pso_create(&D, pd, &from_archive, (void **)&e);
            if (pso) built++;
            else printf("pipeline %d: %s\n", k, e ? e.localizedDescription.UTF8String : "?");
            [pso release];
            [pd release];
        }
        pl_pso_archive_save(0);
        pl_sh_async_stats a;
        pl_sh_async_get_stats(&a);
        printf("stats %d %llu %llu %llu %llu %llu\n", built, (unsigned long long)a.pso_archive_hits,
               (unsigned long long)a.pso_archive_misses, (unsigned long long)a.pso_archive_adds,
               (unsigned long long)a.pso_archive_saves, (unsigned long long)a.pso_archive_files);
        fflush(stdout);
    }
    return 0;
}

/* ---- the driver ---------------------------------------------------------------------------------------- */
static const char *g_self;
typedef struct { int built; unsigned long long hits, misses, adds, saves, files; char log[2048]; } result;

/* runs one launch in a child process; its stdout+stderr are kept for the report */
static result run(unsigned mask, int batch) {
    result r;
    memset(&r, 0, sizeof r);
    r.built = -1;
    char m[16], b[4];
    snprintf(m, sizeof m, "%u", mask);
    snprintf(b, sizeof b, "%d", batch);
    int fd[2];
    if (pipe(fd)) return r;
    posix_spawn_file_actions_t fa;
    posix_spawn_file_actions_init(&fa);
    posix_spawn_file_actions_adddup2(&fa, fd[1], 1);
    posix_spawn_file_actions_adddup2(&fa, fd[1], 2);
    posix_spawn_file_actions_addclose(&fa, fd[0]);
    char *argv[] = {(char *)g_self, "--launch", m, b, NULL};
    pid_t pid;
    int rc = posix_spawn(&pid, g_self, &fa, NULL, argv, environ);
    posix_spawn_file_actions_destroy(&fa);
    close(fd[1]);
    if (rc) { close(fd[0]); return r; }
    size_t n = 0;
    ssize_t k;
    char buf[512];
    while ((k = read(fd[0], buf, sizeof buf)) > 0)
        for (ssize_t i = 0; i < k; i++) if (n + 1 < sizeof r.log) r.log[n++] = buf[i];
    r.log[n] = 0;
    close(fd[0]);
    int st = 0;
    waitpid(pid, &st, 0);
    const char *s = strstr(r.log, "stats ");
    if (s) sscanf(s, "stats %d %llu %llu %llu %llu %llu", &r.built, &r.hits, &r.misses, &r.adds, &r.saves, &r.files);
    return r;
}
static void show(const char *what, const result *r) {
    printf("  %s: built %d, hits %llu, misses %llu, adds %llu, writes %llu, files loaded %llu\n", what, r->built,
           r->hits, r->misses, r->adds, r->saves, r->files);
    for (const char *l = r->log; *l;) {
        const char *e = strchr(l, '\n');
        int len = e ? (int)(e - l) : (int)strlen(l);
        if (strncmp(l, "stats ", 6)) printf("    | %.*s\n", len > 600 ? 600 : len, l);
        l += len + (e ? 1 : 0);
    }
}
static int popcount(unsigned m) { return __builtin_popcount(m); }

/* three launches: a, then b on a's archive, then a|b must all hit */
static int three(const char *name, unsigned a, unsigned b, int batch_a, int batch_b, int verbose) {
    result r1 = run(a, batch_a), r2 = run(b, batch_b), r3 = run(a | b, batch_a);
    int ok = r1.built == popcount(a) && r1.saves == 1 && r2.built == popcount(b) && r2.saves == 1 &&
             r3.built == popcount(a | b) && r3.hits == (unsigned long long)popcount(a | b) && r3.misses == 0;
    if (!ok || verbose) {
        printf("%s %s\n", ok ? "ok  " : "FAIL", name);
        show("launch 1", &r1);
        show("launch 2", &r2);
        show("launch 3", &r3);
    }
    return ok;
}

static char g_dir[1024], g_arch[1200], g_base[1200];
static void reset(void) {
    char cmd[1200];
    snprintf(cmd, sizeof cmd, "rm -rf '%s' && mkdir -p '%s'", g_dir, g_dir);
    if (system(cmd)) printf("could not reset %s\n", g_dir);
}

/* an archive written the way older builds named functions: sg_main for vertex and fragment alike */
static id<MTLFunction> lib_fn(id<MTLDevice> dev, NSString *src, NSString *name) {
    NSError *e = nil;
    id<MTLLibrary> lib = [[dev newLibraryWithSource:src options:nil error:&e] autorelease];
    if (!lib) { printf("test library: %s\n", e.localizedDescription.UTF8String); return nil; }
    MTLFunctionConstantValues *cv = [[[MTLFunctionConstantValues alloc] init] autorelease];
    uint32_t f = 8;
    [cv setConstantValue:&f type:MTLDataTypeUInt atIndex:D3D9MSL_FC_ALPHA_FUNC];
    return [[lib newFunctionWithName:name constantValues:cv error:&e] autorelease];
}
static NSString *msl_of(int i) {
    d3d9msl_result r;
    memset(&r, 0, sizeof r);
    if (d3d9msl_translate(k_sh[i].tok, k_sh[i].n, &r) != D3D9MSL_OK) return nil;
    NSString *s = [[[NSString alloc] initWithBytes:r.msl length:r.msl_len encoding:NSUTF8StringEncoding] autorelease];
    d3d9msl_result_free(&r);
    return s;
}
static int write_archive(id<MTLDevice> dev, id<MTLFunction> v, id<MTLFunction> f, const char *path) {
    MTLBinaryArchiveDescriptor *ad = [[[MTLBinaryArchiveDescriptor alloc] init] autorelease];
    NSError *e = nil;
    id<MTLBinaryArchive> a = [[dev newBinaryArchiveWithDescriptor:ad error:&e] autorelease];
    MTLRenderPipelineDescriptor *pd = [[[MTLRenderPipelineDescriptor alloc] init] autorelease];
    pd.vertexFunction = v;
    pd.fragmentFunction = f;
    pd.colorAttachments[0].pixelFormat = MTLPixelFormatBGRA8Unorm;
    pd.depthAttachmentPixelFormat = MTLPixelFormatDepth32Float;
    MTLVertexDescriptor *vd = [MTLVertexDescriptor vertexDescriptor];
    for (int i = 0; i < 16; i++) {
        vd.attributes[i].format = MTLVertexFormatFloat4;
        vd.attributes[i].bufferIndex = MT_BUF_STREAM0;
    }
    vd.layouts[MT_BUF_STREAM0].stride = 16;
    pd.vertexDescriptor = vd;
    int ok = a && [a addRenderPipelineFunctionsWithDescriptor:pd error:&e] &&
             [a serializeToURL:[NSURL fileURLWithPath:@(path)] error:&e];
    if (!ok) printf("test archive: %s\n", e ? e.localizedDescription.UTF8String : "?");
    return ok;
}
/* the files of the archive set <base>.<n>.metalar */
static int nfiles(const char *base) {
    char pat[1300];
    snprintf(pat, sizeof pat, "%s.*.metalar", base);
    glob_t g;
    if (glob(pat, 0, NULL, &g)) return 0;
    int n = (int)g.gl_pathc;
    globfree(&g);
    return n;
}
static void gpu_name(id<MTLDevice> dev, char *out, size_t n) {
    snprintf(out, n, "%s", dev.name.UTF8String);
    for (char *p = out; *p; p++) if (!((*p >= 'a' && *p <= 'z') || (*p >= 'A' && *p <= 'Z') || (*p >= '0' && *p <= '9'))) *p = '-';
}

int main(int argc, char **argv) {
    if (argc == 4 && !strcmp(argv[1], "--launch")) return launch((unsigned)strtoul(argv[2], NULL, 0), atoi(argv[3]));
    if (argc != 2) { printf("usage: test_pl_archive WORKDIR\n"); return 2; }
    @autoreleasepool {
    g_self = argv[0];
    snprintf(g_dir, sizeof g_dir, "%s", argv[1]);
    snprintf(g_arch, sizeof g_arch, "%s/test.metalar", g_dir);
    snprintf(g_base, sizeof g_base, "%s/test", g_dir);
    id<MTLDevice> dev = MTLCreateSystemDefaultDevice();
    if (!dev) { printf("no Metal device\n"); return 2; }
    setenv("SPORE_D3D9MSL_CACHE", "off", 1);
    setenv("SPORE_METAL_SHADER_WORKERS", "1", 1);

    /* pairs: every ordered pair of pipelines over two launches */
    setenv("SPORE_METAL_PSO_ARCHIVE", g_arch, 1);
    int bad = 0, n = 0;
    for (int i = 0; i < NPL; i++)
        for (int j = 0; j < NPL; j++) {
            if (i == j) continue;
            char name[64];
            snprintf(name, sizeof name, "pairs: pipeline %d, then %d", i, j);
            reset();
            n++;
            if (!three(name, 1u << i, 1u << j, 0, 0, 0)) bad++;
        }
    printf("pairs: %d of %d two-launch sequences failed\n", bad, n);
    CHECK(bad == 0, "every two-launch sequence writes the archive and the third launch hits");

    /* batch: the same shaders compiled in a batch, then alone, then in a batch */
    reset();
    unsigned all = (1u << NPL) - 1;
    CHECK(three("batch: all pipelines (batch), then all (alone), then all (batch)", all, all, 1, 0, 1),
          "batch and solo compiles of the same shaders share the archive");
    reset();
    CHECK(three("batch: pipelines 0-2 in a batch, then 3-5 in a batch", 7u, all & ~7u, 1, 1, 1),
          "a batch launch adds to an archive a batch launch wrote");

    /* legacy: an archive under the older file name, written the older way, at the default path */
    reset();
    unsetenv("SPORE_METAL_PSO_ARCHIVE");
    char cache[1100], gpu[128], legacy[1400], cur[1400];
    snprintf(cache, sizeof cache, "%s/shadercache", g_dir);
    mkdir(cache, 0755);
    setenv("SPORE_D3D9MSL_CACHE", cache, 1);
    gpu_name(dev, gpu, sizeof gpu);
    snprintf(legacy, sizeof legacy, "%s/pso-archive", g_dir);
    mkdir(legacy, 0755);
    snprintf(cur, sizeof cur, "%s/pso-archive/%s.1.metalar", g_dir, gpu);
    snprintf(legacy, sizeof legacy, "%s/pso-archive/%s.metalar", g_dir, gpu);
    {
        NSString *vs = msl_of(VS_POS), *ps = msl_of(PS_COL);
        id<MTLFunction> v = vs ? lib_fn(dev, vs, @"sg_main") : nil, f = ps ? lib_fn(dev, ps, @"sg_main") : nil;
        CHECK(v && f && write_archive(dev, v, f, legacy), "legacy archive written");
    }
    {
        result r1 = run(all, 0);
        struct stat st;
        int gone = stat(legacy, &st) != 0, there = stat(cur, &st) == 0 && st.st_size > 0;
        result r2 = run(all, 0);
        int ok = gone && there && r1.saves == 1 && r2.hits == (unsigned long long)NPL && r2.misses == 0;
        printf("%s legacy: old file %s, new file %s\n", ok ? "ok  " : "FAIL", gone ? "removed" : "still there",
               there ? "written" : "missing");
        show("launch 1", &r1);
        show("launch 2", &r2);
        CHECK(ok, "the old-name archive is replaced by the new one, and the next launch hits");
    }
    setenv("SPORE_D3D9MSL_CACHE", "off", 1);

    /* grow: one file per launch that met a new pipeline */
    reset();
    setenv("SPORE_METAL_PSO_ARCHIVE", g_arch, 1);
    {
        int ok = 1;
        for (int k = 0; k < NPL; k++) {
            result r = run(1u << k, 0);
            if (r.built != 1 || r.misses != 1 || r.saves != 1 || r.files != (unsigned long long)k) { ok = 0; show("launch", &r); }
        }
        result r = run(all, 0);
        ok = ok && r.built == NPL && r.hits == (unsigned long long)NPL && r.misses == 0 && r.saves == 0 &&
             nfiles(g_base) == NPL;
        printf("%s grow: %d launches, %d files; the next launch: %llu hits, %llu misses\n", ok ? "ok  " : "FAIL", NPL,
               nfiles(g_base), r.hits, r.misses);
        if (!ok) show("last launch", &r);
        CHECK(ok, "each launch adds a file and a later launch hits every pipeline");
    }

    /* compact: at the file limit a launch writes one file of the pipelines it used and deletes the others */
    reset();
    setenv("SPORE_METAL_PSO_ARCHIVE_FILES", "3", 1);
    {
        for (int k = 0; k < 3; k++) run(1u << k, 0);
        int before = nfiles(g_base);
        result r1 = run(1u << 0 | 1u << 3, 0);   /* pipeline 0 is a hit, 3 is new */
        int after = nfiles(g_base);
        result r2 = run(1u << 0 | 1u << 3, 0);
        int ok = before == 3 && r1.files == 3 && r1.hits == 1 && r1.misses == 1 && r1.saves == 1 && after == 1 &&
                 r2.files == 1 && r2.hits == 2 && r2.misses == 0;
        printf("%s compact: %d files, then %d after the compacting launch; the next launch: %llu hits, %llu misses\n",
               ok ? "ok  " : "FAIL", before, after, r2.hits, r2.misses);
        show("compacting launch", &r1);
        show("next launch", &r2);
        CHECK(ok, "compaction keeps the pipelines the compacting launch used, in one file");
    }
    unsetenv("SPORE_METAL_PSO_ARCHIVE_FILES");
    }
    printf("test_pl_archive: %d passed, %d failed\n", pass, fail);
    return fail != 0;
}
