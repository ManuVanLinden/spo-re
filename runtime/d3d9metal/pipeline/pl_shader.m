/* pl_shader.m -- the bridge from the pipeline to the shader translator (shader/d3d9msl.h): translate a
 * VS/PS object's bytecode (through the translator's on-disk cache), compile the MSL with the system Metal compiler (safe
 * math: D3D9's rcp/rsq/log edge cases need IEEE behaviour), and keep the functions and a compact reflection in
 * the object's private slot (mt_obj.pl).
 *
 * Asynchronous compiles (DEVIATIONS 3.32). At first a shader was translated and compiled at its
 * first draw, on the render thread: about 140 ms per shader when the macOS Metal cache misses (it is a shared,
 * size-limited cache that evicts), so a load that draws 12 new materials stalled a frame for a second. Now:
 *
 *   - CreateVertexShader/CreatePixelShader queue the shader (pl_sh_created). Worker threads (3 by default,
 *     utility QoS) translate it and compile it. Shaders created in the same frame are compiled together, up to
 *     16 per MTLLibrary: each translated shader goes into its own C++ namespace of one source, with the
 *     alpha-test function constant declared once at file scope. One library of 16 compiles in about 260 ms
 *     against about 140 ms for each shader alone.
 *     A library of one shader is compiled from the translator's text unchanged, so it hits the same macOS
 *     cache entries as a compile at the first draw does.
 *   - Shaders whose translation is already in the MSL cache (drawn before on this machine) are queued ahead
 *     of the others, so the ones a load is likely to draw are ready first.
 *   - A draw that needs a shader that is not ready never skips (skipping changes the picture) and never waits
 *     longer than the compile of that shader alone: a shader still in the queue is taken out and compiled at
 *     once on the render thread; one whose batch is already compiling is compiled alone at the same time
 *     (asynchronously) and the draw takes whichever finishes first.
 *
 * Pipeline archive (pl_pso_create): render pipeline states are looked up in, and added to, MTLBinaryArchive
 * files under build/d3d9metal/pso-archive/ (next to the MSL cache; SPORE_METAL_PSO_ARCHIVE=path|off). An archive
 * is keyed by Metal itself by the pipeline descriptor -- functions, vertex layout, blend state and formats, the
 * same fields as the pipeline key pl_pso_key -- so a pipeline seen in an earlier session is created without a
 * back-end compile. New pipelines are added to a new file of the set, written 3 s after the last addition, on a
 * background queue (see "the pipeline archive" below); the device's destruction writes it too.
 *
 * Switches: SPORE_METAL_SHADER_ASYNC=0 (compile at the first draw, the original scheme), SPORE_METAL_SHADER_BATCH=n
 * (shaders per library, default 16, 1 = no batching), SPORE_METAL_SHADER_WORKERS=n (default 3),
 * SPORE_METAL_PSO_ARCHIVE_FILES=n (archive files kept before a launch compacts them, default 16). */
#import <Metal/Metal.h>
#include <glob.h>
#include <pthread.h>
#include <stdio.h>
#include <stdlib.h>
#include <string.h>
#include <sys/stat.h>
#include <unistd.h>

#include "d3d9metal_int.h"
#include "pl_shader.h"
#include "../shader/d3d9msl.h"

static const char *k_usage[14] = {"position", "blendweight", "blendindices", "normal", "psize", "texcoord",
                                  "tangent", "binormal", "tessfactor", "positiont", "color", "fog", "depth", "sample"};

/* g_compiles/g_compile_fail: shaders compiled / failed on any thread; g_compile_ns: time the render thread spent
 * compiling or waiting for a shader at a draw (what a frame feels). */
static uint64_t g_compiles, g_compile_fail, g_compile_ns;

static uint64_t now_ns(void) { return clock_gettime_nsec_np(CLOCK_UPTIME_RAW); }

static id<MTLFunction> specialise(id<MTLLibrary> lib, const char *entry, uint32_t alpha_func, NSError **e) {
    MTLFunctionConstantValues *cv = [[MTLFunctionConstantValues alloc] init];
    [cv setConstantValue:&alpha_func type:MTLDataTypeUInt atIndex:D3D9MSL_FC_ALPHA_FUNC];
    id<MTLFunction> f = [lib newFunctionWithName:[NSString stringWithUTF8String:entry] constantValues:cv error:e];
    [cv release];
    return f;
}

static void fill_refl(pl_sh *s, const d3d9msl_reflection *r) {
    s->is_ps = r->stage == 1;
    s->nin = s->nout = 0;
    s->ps_out_mask = 0;
    s->uses_int = s->uses_bool = 0;
    for (unsigned i = 0; i < r->ninputs && s->nin < 16; i++) {
        const d3d9msl_input *in = &r->inputs[i];
        if (!s->is_ps) {
            s->in[s->nin].reg = in->attribute;
            s->in[s->nin].usage = in->usage;
            s->in[s->nin].index = in->usage_index;
            s->nin++;
        } else if (s->nout < 32 && in->usage < 14) {
            /* pixel inputs, for the linkage check against the vertex outputs */
            snprintf(s->out[s->nout++], 20, "%s%u", k_usage[in->usage], in->usage_index);
        }
    }
    for (unsigned i = 0; i < r->noutputs; i++) {
        const d3d9msl_output *o = &r->outputs[i];
        if (!s->is_ps && o->kind == D3D9MSL_OUT_USER && s->nout < 32) snprintf(s->out[s->nout++], 20, "%s", o->semantic);
        if (s->is_ps && o->kind == D3D9MSL_OUT_COLOR && o->regnum < 8) s->ps_out_mask |= (uint8_t)(1u << o->regnum);
    }
    s->smp_mask = (uint16_t)(r->sampler_mask & 0xffff);
    for (int k = 0; k < 16; k++) s->smp_type[k] = r->sampler_type[k];
    s->nconst_f = (uint16_t)(r->c_max + 1);
    s->rel = r->relative_c;
    for (int k = 0; k < 64; k++) { if (r->i_used[k]) s->uses_int = 1; if (r->b_used[k]) s->uses_bool = 1; }
    s->alpha_fc = r->alpha_test;
}

static MTLCompileOptions *compile_options(void) { /* +1 */
    MTLCompileOptions *opt = [[MTLCompileOptions alloc] init];
    if (@available(macOS 15.0, *)) opt.mathMode = MTLMathModeSafe;
    else {
#pragma clang diagnostic push
#pragma clang diagnostic ignored "-Wdeprecated-declarations"
        opt.fastMathEnabled = NO;
#pragma clang diagnostic pop
    }
    return opt;
}

/* ---- the queue and its lock ------------------------------------------------------------------------- */
static pthread_mutex_t g_mx = PTHREAD_MUTEX_INITIALIZER;
static pthread_cond_t g_work_cv = PTHREAD_COND_INITIALIZER;   /* workers: new work or a frame closed */
static pthread_cond_t g_done_cv = PTHREAD_COND_INITIALIZER;   /* draws and pl_sh_drain: a shader finished */
static pl_sh *g_qh[2], *g_qt[2];                             /* [0] known shaders first, [1] the others */
static uint64_t g_frame = 1;
static int g_running;                                         /* batches being compiled */
static int g_workers;                                         /* started */
static id<MTLDevice> g_dev;                                   /* +1, the device the workers compile for */
static pl_sh_async_stats g_as;

static int env_int(const char *name, int def, int lo, int hi) {
    const char *e = getenv(name);
    if (!e || !*e) return def;
    int v = atoi(e);
    return v < lo ? lo : v > hi ? hi : v;
}
static int async_on(void) {
    static int v = -1;
    if (v < 0) { const char *e = getenv("SPORE_METAL_SHADER_ASYNC"); v = !(e && e[0] == '0'); }
    return v;
}
static int batch_max(void) { static int v; if (!v) v = env_int("SPORE_METAL_SHADER_BATCH", 16, 1, 64); return v; }

/* frees a record nobody holds any more; under g_mx (or before it was shared) */
static void sh_destroy(pl_sh *s) {
    [s->fn release];
    for (int i = 0; i < 8; i++) [(id)s->fn_alpha[i] release];
    [s->lib release];
    free(s->code);
    free(s);
}
static void sh_unref_locked(pl_sh *s) {
    if (--s->refs == 0) sh_destroy(s);
}
static void q_push_locked(pl_sh *s) {
    int k = s->known ? 0 : 1;
    s->next = NULL;
    if (g_qt[k]) g_qt[k]->next = s; else g_qh[k] = s;
    g_qt[k] = s;
}
static int q_remove_locked(pl_sh *s) {
    for (int k = 0; k < 2; k++) {
        pl_sh *p = NULL;
        for (pl_sh *q = g_qh[k]; q; p = q, q = q->next)
            if (q == s) {
                if (p) p->next = q->next; else g_qh[k] = q->next;
                if (g_qt[k] == q) g_qt[k] = p;
                q->next = NULL;
                return 1;
            }
    }
    return 0;
}

/* ---- translation and compilation (any thread, no lock) ------------------------------------------------ */
typedef struct sh_out {
    id<MTLLibrary> lib;       /* +1 */
    id<MTLFunction> fn;       /* +1 */
    d3d9msl_reflection refl;
    int ok;
    char err[200];
} sh_out;

static void test_hook(d3d9msl_result *r);
static int translate(const uint8_t *code, uint32_t len, d3d9msl_result *r, int *hit) {
    memset(r, 0, sizeof *r);
    if (!code || len < 8) { snprintf(r->error, sizeof r->error, "no bytecode"); return D3D9MSL_EARG; }
    int rc = d3d9msl_translate_cached((const uint32_t *)code, len / 4, NULL, r, hit);
    if (rc == D3D9MSL_OK) test_hook(r);
    return rc;
}

/* the functions of a compiled library for one translated shader */
static void finish(sh_out *o, id<MTLLibrary> lib, const char *fname, const d3d9msl_result *r) {
    NSError *e = nil;
    o->fn = specialise(lib, fname, 8u, &e);
    if (!o->fn) {
        snprintf(o->err, sizeof o->err, "no function %s: %s", fname, e ? e.localizedDescription.UTF8String : "?");
        return;
    }
    o->lib = [lib retain];
    o->refl = r->refl;
    o->ok = 1;
}

/* Test hook (tests/replay_hitch.m): rewrites a translated shader before it is compiled, to make its code unique
 * and so defeat the macOS Metal cache for a cold measurement. NULL in the game. */
char *(*pl_sh_test_msl_hook)(const char *msl, size_t len, size_t *out_len);
static void test_hook(d3d9msl_result *r) {
    if (!pl_sh_test_msl_hook || !r->msl) return;
    size_t n = 0;
    char *m = pl_sh_test_msl_hook(r->msl, r->msl_len, &n);
    if (!m) return;
    free(r->msl);
    r->msl = m;
    r->msl_len = n;
}

/* one shader alone, from the translator's text unchanged (the original path; same macOS cache keys) */
static void compile_one(id<MTLDevice> dev, const d3d9msl_result *r, sh_out *o) {
    @autoreleasepool {
        MTLCompileOptions *opt = compile_options();
        NSError *e = nil;
        NSString *src = [[NSString alloc] initWithBytes:r->msl length:r->msl_len encoding:NSUTF8StringEncoding];
        id<MTLLibrary> lib = [dev newLibraryWithSource:src options:opt error:&e];
        [src release];
        [opt release];
        if (!lib) {
            snprintf(o->err, sizeof o->err, "did not compile: %s", e ? e.localizedDescription.UTF8String : "?");
            return;
        }
        finish(o, lib, r->entry, r);
        [lib release];
    }
}

/* The four lines every translated shader starts with: compiled once at file scope in a batch. */
static const char *const k_hoist[4] = {
    "#include <metal_stdlib>\n", "using namespace metal;\n",
    "constant uint sg_alpha_func [[function_constant(0)]];\n",
    "constant bool sg_alpha_func_set = is_function_constant_defined(sg_alpha_func);\n"};

/* n translated shaders in one library, each in namespace sgb<i>_<hash>; on failure each is compiled alone */
static void compile_batch(id<MTLDevice> dev, d3d9msl_result *r, const uint64_t *hash, int n, sh_out *o) {
    if (n == 1) { compile_one(dev, &r[0], &o[0]); return; }
    @autoreleasepool {
        NSMutableString *src = [NSMutableString stringWithCapacity:8192 * (NSUInteger)n];
        for (int k = 0; k < 4; k++) [src appendString:@(k_hoist[k])];
        char ns[64];
        for (int i = 0; i < n; i++) {
            NSMutableString *body = [[NSMutableString alloc] initWithBytes:r[i].msl length:r[i].msl_len encoding:NSUTF8StringEncoding];
            for (int k = 0; k < 4; k++) {
                NSRange at = [body rangeOfString:@(k_hoist[k])];
                if (at.location != NSNotFound) [body deleteCharactersInRange:at];
            }
            snprintf(ns, sizeof ns, "sgb%d_%016llx", i, (unsigned long long)hash[i]);
            [src appendFormat:@"namespace %s {\n%@\n}\n", ns, body];
            [body release];
        }
        MTLCompileOptions *opt = compile_options();
        NSError *e = nil;
        id<MTLLibrary> lib = [dev newLibraryWithSource:src options:opt error:&e];
        [opt release];
        if (lib) {
            for (int i = 0; i < n; i++) {
                char fname[160];
                snprintf(ns, sizeof ns, "sgb%d_%016llx", i, (unsigned long long)hash[i]);
                snprintf(fname, sizeof fname, "%s::%s", ns, r[i].entry);
                finish(&o[i], lib, fname, &r[i]);
            }
            [lib release];
        }
    }
    for (int i = 0; i < n; i++)
        if (!o[i].ok) compile_one(dev, &r[i], &o[i]);   /* a batch that fails is retried one by one */
}

/* Moves a result into the record if it is still waiting for one; under g_mx. Releases what is not used. */
static void install_locked(pl_sh *s, sh_out *o, int is_solo) {
    if (!s->dead && (s->state == 2 || s->state == 3)) {
        if (o->ok) {
            s->lib = o->lib;
            s->fn = o->fn;
            fill_refl(s, &o->refl);
            __atomic_store_n(&s->state, 1, __ATOMIC_RELEASE);   /* pl_sh_get reads it without the lock */
            g_compiles++;
        } else {
            s->state = -1;
            g_compile_fail++;
            MT_LOG("pipeline: shader %016llx %s\n", (unsigned long long)s->hash, o->err);
            [o->lib release];
            [o->fn release];
        }
        s->t_ready_ns = now_ns();
        if (is_solo) g_as.raced++;
    } else {
        [o->lib release];
        [o->fn release];
    }
    o->lib = nil;
    o->fn = nil;
}

/* ---- workers ---------------------------------------------------------------------------------------------- */
static void *worker(void *arg) {
    (void)arg;
    pthread_set_qos_class_self_np(QOS_CLASS_UTILITY, 0);
    pl_sh *job[64];
    for (;;) {
        int n = 0, timed_out = 0;
        pthread_mutex_lock(&g_mx);
        for (;;) {
            int k = g_qh[0] ? 0 : g_qh[1] ? 1 : -1;
            if (k < 0) { timed_out = 0; pthread_cond_wait(&g_work_cv, &g_mx); continue; }
            pl_sh *h = g_qh[k];
            int run = 0, B = batch_max();
            /* The count is taken under the lock and used only under the same hold: a draw (solo compile) or a
             * Release may unlink queued shaders while the wait below releases the lock, so after any wake the
             * loop recounts from the current head (a stale count walked off the list). */
            for (pl_sh *q = h; q && q->frame == h->frame && run < B; q = q->next) run++;
            if (!timed_out && run < B && h->frame >= g_frame) {
                /* the frame that created them is still open: give it until its end (or 50 ms) to add more */
                struct timespec ts;
                clock_gettime(CLOCK_REALTIME, &ts);
                ts.tv_nsec += 50 * 1000000L;
                if (ts.tv_nsec >= 1000000000L) { ts.tv_sec++; ts.tv_nsec -= 1000000000L; }
                if (pthread_cond_timedwait(&g_work_cv, &g_mx, &ts) != 0) timed_out = 1;
                continue;
            }
            for (n = 0; n < run && g_qh[k] && g_qh[k]->frame == h->frame; n++) {
                pl_sh *q = g_qh[k];
                g_qh[k] = q->next;
                if (!g_qh[k]) g_qt[k] = NULL;
                q->next = NULL;
                q->state = 3;
                job[n] = q;
            }
            if (n == 0) { timed_out = 0; continue; }
            break;
        }
        g_running++;
        id<MTLDevice> dev = [g_dev retain];
        pthread_mutex_unlock(&g_mx);

        uint64_t t0 = now_ns();
        /* on the heap: a reflection is 3.4 KB, 64 of them in three arrays would overflow a thread's 512 KB stack */
        d3d9msl_result *r = calloc((size_t)n, sizeof *r);
        sh_out *o = calloc((size_t)n, sizeof *o), *ob = calloc((size_t)n, sizeof *ob);
        uint64_t hash[64];
        int idx[64], m = 0;
        for (int i = 0; i < n; i++) {
            int hit = 0;
            int rc = translate(job[i]->code, job[i]->code_len, &r[m], &hit);
            if (rc != D3D9MSL_OK) {
                snprintf(o[i].err, sizeof o[i].err, "not translated: %s", r[m].error);
                d3d9msl_result_free(&r[m]);
                continue;
            }
            hash[m] = job[i]->hash;
            idx[m++] = i;
        }
        if (m) {
            compile_batch(dev, r, hash, m, ob);
            for (int j = 0; j < m; j++) { o[idx[j]] = ob[j]; d3d9msl_result_free(&r[j]); }
        }
        free(r);
        free(ob);
        [dev release];
        uint64_t dt = now_ns() - t0;

        pthread_mutex_lock(&g_mx);
        g_as.batches++;
        g_as.batch_shaders += (uint64_t)n;
        g_as.compile_ns += dt;
        for (int i = 0; i < n; i++) {
            install_locked(job[i], &o[i], 0);
            sh_unref_locked(job[i]);
        }
        g_running--;
        pthread_cond_broadcast(&g_done_cv);
        pthread_mutex_unlock(&g_mx);
        free(o);
    }
    return NULL;
}

static void start_workers_locked(void) {
    if (g_workers) return;
    int nw = env_int("SPORE_METAL_SHADER_WORKERS", 3, 1, 8);
    for (int i = 0; i < nw; i++) {
        pthread_t t;
        pthread_attr_t a;
        pthread_attr_init(&a);
        pthread_attr_setdetachstate(&a, PTHREAD_CREATE_DETACHED);
        if (pthread_create(&t, &a, worker, NULL) == 0) g_workers++;
        pthread_attr_destroy(&a);
    }
}

/* 1 if the translation of this bytecode is in the MSL cache already (a shader drawn before on this machine) */
static int msl_cached(const uint8_t *code, uint32_t len) {
    static int init;
    if (!init) { init = 1; if (!d3d9msl_cache_dir()) d3d9msl_cache_init(NULL); }
    const char *dir = d3d9msl_cache_dir();
    if (!dir) return 0;
    char key[17], path[1200];
    d3d9msl_cache_key((const uint32_t *)code, len / 4, NULL, key);
    snprintf(path, sizeof path, "%s/%s.d3d9msl", dir, key);
    struct stat st;
    return stat(path, &st) == 0;
}

void pl_sh_created(mt_dev *d, mt_obj *o) {
    if (!async_on() || !d || !d->mtl || o->pl || !o->code || o->code_len < 8) return;
    pl_sh *s = calloc(1, sizeof *s);
    s->code = malloc(o->code_len);
    memcpy(s->code, o->code, o->code_len);
    s->code_len = o->code_len;
    s->hash = o->code_hash;
    s->version = ((const uint32_t *)o->code)[0];
    s->known = msl_cached(o->code, o->code_len);
    s->t_create_ns = now_ns();
    s->refs = 2;            /* the object and the queue */
    s->state = 2;
    pthread_mutex_lock(&g_mx);
    if (!g_dev) g_dev = [d->mtl retain];
    s->frame = g_frame;
    o->pl = s;
    q_push_locked(s);
    g_as.created++;
    g_as.queued++;
    start_workers_locked();
    pthread_cond_signal(&g_work_cv);
    pthread_mutex_unlock(&g_mx);
}

void pl_sh_frame_end(void) {
    pthread_mutex_lock(&g_mx);
    g_frame++;
    if (g_qh[0] || g_qh[1]) pthread_cond_broadcast(&g_work_cv);
    pthread_mutex_unlock(&g_mx);
}

int pl_sh_drain(int ms) {
    uint64_t end = now_ns() + (uint64_t)ms * 1000000ull;
    pthread_mutex_lock(&g_mx);
    while ((g_qh[0] || g_qh[1] || g_running) && now_ns() < end) {
        g_frame++;   /* nothing else will close the frame for the workers */
        pthread_cond_broadcast(&g_work_cv);
        struct timespec ts;
        clock_gettime(CLOCK_REALTIME, &ts);
        ts.tv_nsec += 10 * 1000000L;
        if (ts.tv_nsec >= 1000000000L) { ts.tv_sec++; ts.tv_nsec -= 1000000000L; }
        pthread_cond_timedwait(&g_done_cv, &g_mx, &ts);
    }
    int busy = g_qh[0] || g_qh[1] || g_running;
    pthread_mutex_unlock(&g_mx);
    return busy;
}

/* The draw's path when the shader is not ready: see the header comment. Called with g_mx held, returns with it held. */
static void wait_for_locked(mt_dev *d, pl_sh *s) {
    uint64_t t0 = now_ns();
    if (s->state == 2) {
        /* still queued: take it out and compile it here, alone */
        q_remove_locked(s);
        sh_unref_locked(s);          /* the queue's reference (the object's keeps it alive) */
        s->state = 3;
        g_as.solo_at_draw++;
        pthread_mutex_unlock(&g_mx);
        d3d9msl_result r;
        sh_out o;
        memset(&o, 0, sizeof o);
        int hit = 0;
        if (translate(s->code, s->code_len, &r, &hit) == D3D9MSL_OK) compile_one(d->mtl, &r, &o);
        else snprintf(o.err, sizeof o.err, "not translated: %s", r.error);
        d3d9msl_result_free(&r);
        pthread_mutex_lock(&g_mx);
        install_locked(s, &o, 0);
    } else if (s->state == 3) {
        /* its batch is compiling: compile it alone as well and take whichever finishes first */
        d3d9msl_result r;
        int hit = 0;
        pthread_mutex_unlock(&g_mx);
        int rc = translate(s->code, s->code_len, &r, &hit);
        pthread_mutex_lock(&g_mx);
        if (rc == D3D9MSL_OK && s->state == 3) {
            s->refs++;
            pthread_mutex_unlock(&g_mx);
            NSString *src = [[NSString alloc] initWithBytes:r.msl length:r.msl_len encoding:NSUTF8StringEncoding];
            MTLCompileOptions *opt = compile_options();
            d3d9msl_result *rk = malloc(sizeof *rk);
            *rk = r;                 /* the handler owns the translation now */
            [d->mtl newLibraryWithSource:src options:opt completionHandler:^(id<MTLLibrary> lib, NSError *e) {
                sh_out o;
                memset(&o, 0, sizeof o);
                if (lib) finish(&o, lib, rk->entry, rk);
                else snprintf(o.err, sizeof o.err, "did not compile: %s", e ? e.localizedDescription.UTF8String : "?");
                d3d9msl_result_free(rk);
                free(rk);
                pthread_mutex_lock(&g_mx);
                install_locked(s, &o, 1);
                sh_unref_locked(s);
                pthread_cond_broadcast(&g_done_cv);
                pthread_mutex_unlock(&g_mx);
            }];
            [opt release];
            [src release];
            pthread_mutex_lock(&g_mx);
        } else d3d9msl_result_free(&r);
        while (s->state == 3) pthread_cond_wait(&g_done_cv, &g_mx);
    }
    uint64_t dt = now_ns() - t0;
    g_as.waited++;
    g_as.wait_ns += dt;
    if (dt > g_as.max_wait_ns) g_as.max_wait_ns = dt;
    g_compile_ns += dt;
}

pl_sh *pl_sh_get(mt_dev *d, mt_obj *o) {
    pl_sh *s = (pl_sh *)o->pl;
    if (s && __atomic_load_n(&s->state, __ATOMIC_ACQUIRE) == 1) return s;   /* a record never leaves state 1 */
    if (!s) {
        /* not queued at creation (asynchronous compiles off, or a test's stub device): compile here, at the first draw */
        s = calloc(1, sizeof *s);
        o->pl = s;
        s->refs = 1;
        s->state = -1;
        s->hash = o->code_hash;
        if (!o->code || o->code_len < 8) return NULL;
        s->code = malloc(o->code_len);
        memcpy(s->code, o->code, o->code_len);
        s->code_len = o->code_len;
        s->version = ((const uint32_t *)o->code)[0];
        uint64_t t0 = now_ns();
        d3d9msl_result r;
        sh_out out;
        memset(&out, 0, sizeof out);
        int hit = 0;
        if (translate(s->code, s->code_len, &r, &hit) == D3D9MSL_OK) compile_one(d->mtl, &r, &out);
        else snprintf(out.err, sizeof out.err, "not translated: %s", r.error);
        d3d9msl_result_free(&r);
        pthread_mutex_lock(&g_mx);
        s->state = 3;
        install_locked(s, &out, 0);
        g_compile_ns += now_ns() - t0;
        pthread_mutex_unlock(&g_mx);
        if (mt_verbose >= 2 && s->state == 1)
            MT_LOG("pipeline: shader %016llx compiled at its first draw (%s)\n", (unsigned long long)s->hash, hit ? "cache" : "translated");
        return s->state == 1 ? s : NULL;
    }
    pthread_mutex_lock(&g_mx);
    if (s->state == 2 || s->state == 3) wait_for_locked(d, s);
    int st = s->state;
    pthread_mutex_unlock(&g_mx);
    return st == 1 ? s : NULL;
}

MT_ID(MTLFunction) pl_sh_fragment(mt_dev *d, pl_sh *s, uint32_t f) {
    (void)d;
    if (f < 1 || f > 7 || !s->alpha_fc) return s->fn;
    if (!s->fn_alpha[f - 1]) {
        NSError *e = nil;
        s->fn_alpha[f - 1] = specialise(s->lib, s->fn.name.UTF8String, f, &e);
        if (!s->fn_alpha[f - 1]) return s->fn;
    }
    return (id<MTLFunction>)s->fn_alpha[f - 1];
}

int pl_sh_writes(const pl_sh *vs, const char *sem) {
    for (unsigned i = 0; i < vs->nout; i++)
        if (!strcmp(vs->out[i], sem)) return 1;
    return 0;
}

void pl_sh_free(mt_obj *o) {
    pl_sh *s = (pl_sh *)o->pl;
    if (!s) return;
    o->pl = NULL;
    pthread_mutex_lock(&g_mx);
    if (s->state == 2 && q_remove_locked(s)) sh_unref_locked(s);   /* the queue's reference */
    s->dead = 1;
    sh_unref_locked(s);                                            /* the object's */
    pthread_mutex_unlock(&g_mx);
}

void d3d9metal_pl_shader_stats(uint64_t *compiles, uint64_t *fails, uint64_t *ns) {
    *compiles = g_compiles; *fails = g_compile_fail; *ns = g_compile_ns;
}

/* ---- the pipeline archive ------------------------------------------------------------------------------
 * Metal refuses to write an archive that was loaded from a file and then given new pipelines, often enough that
 * from the second launch on the file was never written again ("Failed to generate machO ...
 * expecting 'fragment' stage in pipeline no. N", or "missing 'vertex' stage"). Whether such a merge is refused
 * depends on the functions' names and code in a way we could not predict (tests/test_pl_archive.m; renaming the
 * functions moved the failures around), and a merge that is accepted can lose an entry. An archive made fresh in
 * this process always writes. So the archive is a set of files, <base>.<n>.metalar (base: <dir>/<GPU>, or
 * SPORE_METAL_PSO_ARCHIVE less ".metalar"), and no archive loaded from a file is ever written:
 *   g_arch_look  every file of the set, loaded at start, read only, used by the render thread to look pipelines
 *                up (pl_pso_create);
 *   g_arch_rec   a new, empty archive owned by the serial queue g_arch_q, which adds every pipeline the lookup
 *                missed (a back-end compile, off the render thread) and writes it as the next file of the set,
 *                <base>.<largest n + 1>.metalar, 3 s after the last addition (420 ms for 183 pipelines in the
 *                replay; also off the render thread), and again after later additions.
 * The next launch looks up what this one recorded, in all the files. A launch that meets no new pipeline writes
 * nothing. When the set has SPORE_METAL_PSO_ARCHIVE_FILES files (default 16) the launch compacts it: it records
 * every pipeline it creates, hits included, writes them as one new file and deletes the others (a pipeline that
 * launch did not use is compiled again when next needed). A single-file archive <base>.metalar, the layout
 * older builds wrote, is deleted. */
static NSArray *g_arch_look;                           /* +1, of id<MTLBinaryArchive>; nil if the set is empty */
static id<MTLBinaryArchive> g_arch_rec;                /* +1 */
static NSMutableArray *g_arch_old;                     /* +1, of NSString: the files loaded (deleted by compaction) */
static dispatch_queue_t g_arch_q;
static int g_arch_state;              /* 0 not tried, 1 in use, -1 off */
static int g_arch_compact;            /* this launch compacts the set */
static char g_arch_path[1300];        /* the file this launch writes */
static uint64_t g_arch_pending, g_arch_gen;            /* g_arch_q only */
static double g_arch_last_save_ms = -1;                /* g_arch_q only (read after a dispatch_sync) */
static uint64_t g_pso_hits, g_pso_misses, g_pso_adds, g_pso_saves, g_pso_files;

static id<MTLBinaryArchive> archive_open(id<MTLDevice> dev, const char *path) {   /* +1; path NULL: a new one */
    MTLBinaryArchiveDescriptor *ad = [[MTLBinaryArchiveDescriptor alloc] init];
    NSError *err = nil;
    if (path) ad.url = [NSURL fileURLWithPath:@(path)];
    id<MTLBinaryArchive> a = [dev newBinaryArchiveWithDescriptor:ad error:&err];
    if (!a && path) MT_LOG("pipeline archive %s not usable (%s); deleted\n", path, err ? err.localizedDescription.UTF8String : "?");
    [ad release];
    return a;
}

static void archive_init(id<MTLDevice> dev) {
    if (g_arch_state) return;
    g_arch_state = -1;
    char base[1200];
    const char *e = getenv("SPORE_METAL_PSO_ARCHIVE");
    if (e && (!strcmp(e, "off") || !strcmp(e, "0"))) return;
    if (e && *e) {
        snprintf(base, sizeof base, "%s", e);
        size_t l = strlen(base);
        if (l > 8 && !strcmp(base + l - 8, ".metalar")) base[l - 8] = 0;
    } else {
        if (!d3d9msl_cache_dir()) d3d9msl_cache_init(NULL);
        const char *c = d3d9msl_cache_dir();
        char dir[1100];
        if (c) {
            snprintf(dir, sizeof dir, "%s", c);
            char *sl = strrchr(dir, '/');
            if (sl) *sl = 0;
        } else {
            const char *root = getenv("SPORE_REPO_ROOT");
            snprintf(dir, sizeof dir, "%s/build/d3d9metal", root && *root ? root : SG_REPO_ROOT);
        }
        char name[128];
        snprintf(name, sizeof name, "%s", dev.name.UTF8String);
        for (char *p = name; *p; p++) if (!((*p >= 'a' && *p <= 'z') || (*p >= 'A' && *p <= 'Z') || (*p >= '0' && *p <= '9'))) *p = '-';
        snprintf(base, sizeof base, "%s/pso-archive", dir);
        mkdir(base, 0755);
        size_t l = strlen(base);
        snprintf(base + l, sizeof base - l, "/%s", name);
    }
    /* the single-file archive of older builds (merged into the set, and so never written again) */
    snprintf(g_arch_path, sizeof g_arch_path, "%s.metalar", base);
    unlink(g_arch_path);
    /* the set: <base>.<n>.metalar */
    NSMutableArray *look = [[NSMutableArray alloc] init];
    g_arch_old = [[NSMutableArray alloc] init];
    unsigned long maxn = 0;
    char pat[1300];
    snprintf(pat, sizeof pat, "%s.*.metalar", base);
    glob_t g;
    if (glob(pat, 0, NULL, &g) == 0) {
        size_t bl = strlen(base);
        for (size_t i = 0; i < g.gl_pathc; i++) {
            const char *f = g.gl_pathv[i], *num = f + bl + 1;
            char *end = NULL;
            unsigned long n = strtoul(num, &end, 10);
            if (end == num || strcmp(end, ".metalar")) continue;   /* not <base>.<n>.metalar */
            if (n > maxn) maxn = n;
            struct stat st;
            if (stat(f, &st) != 0 || st.st_size <= 0 || st.st_size >= 256ll * 1024 * 1024) { unlink(f); continue; }
            id<MTLBinaryArchive> a = archive_open(dev, f);
            if (!a) { unlink(f); continue; }
            [look addObject:a];
            [a release];
            [g_arch_old addObject:@(f)];
        }
        globfree(&g);
    }
    snprintf(g_arch_path, sizeof g_arch_path, "%s.%lu.metalar", base, maxn + 1);
    g_arch_rec = archive_open(dev, NULL);
    if (!g_arch_rec) {
        MT_LOG("pipeline archive off: no MTLBinaryArchive\n");
        [look release];
        return;
    }
    g_pso_files = look.count;
    if (look.count) g_arch_look = look;
    else [look release];
    const char *mf = getenv("SPORE_METAL_PSO_ARCHIVE_FILES");
    unsigned long maxf = mf && *mf ? strtoul(mf, NULL, 10) : 16;
    g_arch_compact = g_pso_files >= (maxf ? maxf : 1);
    g_arch_q = dispatch_queue_create("spore.d3d9metal.pso-archive",
                                     dispatch_queue_attr_make_with_qos_class(DISPATCH_QUEUE_SERIAL, QOS_CLASS_UTILITY, 0));
    g_arch_state = 1;
    if (mt_verbose >= 1)
        MT_LOG("pipeline archive %s.*.metalar: %llu files loaded%s; writes %s\n", base, (unsigned long long)g_pso_files,
               g_arch_compact ? ", compacting them" : "", g_arch_path);
}

static void archive_write(void) {   /* g_arch_q */
    if (!g_arch_pending) return;
    uint64_t t = now_ns();
    char tmp[1400];
    snprintf(tmp, sizeof tmp, "%s.tmp.%d", g_arch_path, (int)getpid());
    NSError *e = nil;
    BOOL ok = [g_arch_rec serializeToURL:[NSURL fileURLWithPath:@(tmp)] error:&e];
    if (ok && rename(tmp, g_arch_path) == 0) {
        g_pso_saves++;
        if (g_arch_compact) {   /* this launch's file replaces the set */
            for (NSString *f in g_arch_old) unlink(f.UTF8String);
            [g_arch_old removeAllObjects];
        }
    } else {
        unlink(tmp);
        static int warned;
        if (!warned++) MT_LOG("pipeline archive: not written (%s)\n", e ? e.localizedDescription.UTF8String : "rename failed");
    }
    g_arch_pending = 0;
    g_arch_last_save_ms = (double)(now_ns() - t) / 1e6;
}

/* adds a pipeline to this launch's archive and writes it once no pipeline was added for 3 s (the end of a load) */
static void archive_add(MTLRenderPipelineDescriptor *pd) {
    MTLRenderPipelineDescriptor *cp = [pd copy];
    dispatch_async(g_arch_q, ^{
        NSError *ae = nil;
        if ([g_arch_rec addRenderPipelineFunctionsWithDescriptor:cp error:&ae]) {
            g_arch_pending++;
            uint64_t gen = ++g_arch_gen;
            dispatch_after(dispatch_time(DISPATCH_TIME_NOW, 3 * NSEC_PER_SEC), g_arch_q, ^{
                if (gen == g_arch_gen) archive_write();
            });
        } else {
            static int warned;
            if (!warned++) MT_LOG("pipeline archive: add failed: %s\n", ae ? ae.localizedDescription.UTF8String : "?");
        }
        [cp release];
    });
}

MT_ID(MTLRenderPipelineState) pl_pso_create(mt_dev *d, void *desc, int *from_archive, void **errp) {
    MTLRenderPipelineDescriptor *pd = (MTLRenderPipelineDescriptor *)desc;
    archive_init(d->mtl);
    if (from_archive) *from_archive = 0;
    id<MTLRenderPipelineState> pso = nil;
    NSError *e = nil;
    if (g_arch_look) {
        pd.binaryArchives = g_arch_look;
        pso = [d->mtl newRenderPipelineStateWithDescriptor:pd options:MTLPipelineOptionFailOnBinaryArchiveMiss
                                                reflection:nil error:&e];
        pd.binaryArchives = nil;
        if (pso) {
            g_pso_hits++;
            if (from_archive) *from_archive = 1;
            if (g_arch_compact) archive_add(pd);   /* kept by the compacted set */
            return pso;
        }
        e = nil;
    }
    if (g_arch_state == 1) g_pso_misses++;
    pso = [d->mtl newRenderPipelineStateWithDescriptor:pd error:&e];
    if (!pso) {
        if (errp) *errp = e;
        return nil;
    }
    if (g_arch_state == 1) {
        g_pso_adds++;
        archive_add(pd);
    }
    return pso;
}

double pl_pso_archive_save(int min_interval_ms) {
    if (g_arch_state != 1 || min_interval_ms > 0) return -1;   /* the queue writes by itself after a load */
    __block double ms = -1;
    dispatch_sync(g_arch_q, ^{
        if (g_arch_pending) { archive_write(); ms = g_arch_last_save_ms; }
    });
    return ms;
}

void pl_sh_async_get_stats(pl_sh_async_stats *s) {
    pthread_mutex_lock(&g_mx);
    *s = g_as;
    pthread_mutex_unlock(&g_mx);
    s->pso_archive_hits = g_pso_hits;
    s->pso_archive_misses = g_pso_misses;
    s->pso_archive_adds = g_pso_adds;
    s->pso_archive_saves = g_pso_saves;
    s->pso_archive_files = g_pso_files;
}

/* the watch line's view of pl_sh_async_stats (mt_d3d9.mm, weak default there) */
int d3d9metal_pl_async_stats(uint64_t v[14]) {
    pl_sh_async_stats a;
    pl_sh_async_get_stats(&a);
    uint64_t t[14] = {a.created, a.queued, a.batches, a.batch_shaders, a.solo_at_draw, a.raced, a.waited, a.wait_ns,
                      a.max_wait_ns, a.compile_ns, a.pso_archive_hits, a.pso_archive_misses, a.pso_archive_adds,
                      a.pso_archive_saves};
    memcpy(v, t, sizeof t);
    return 1;
}
