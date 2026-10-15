/* d3d9msl_tool -- command-line front end of the d3d9msl library (tests and corpus runs).
 *
 *   d3d9msl_tool [--entry NAME] [--fog vertex] [--flat] [--plain] [--no-prelude] [--json OUT.json] BLOB
 *       translate one bytecode file (raw little-endian tokens); MSL to stdout, reflection JSON to OUT.json
 *   d3d9msl_tool --batch LIST OUTDIR
 *       LIST: lines "BLOB NAME ENTRY FLAGS" (FLAGS = D3D9MSL_OPT_* as a number). Writes OUTDIR/NAME.metal and
 *       OUTDIR/NAME.json, or OUTDIR/NAME.err with the error. Prints one summary line.
 *   d3d9msl_tool --cache DIR [--entry NAME] BLOB...
 *       translate through the cache; prints hit/miss per file and the cache statistics.
 * Exit status: 0 all translated, 1 any failure, 2 usage. */
#include "d3d9msl.h"

#include <stdio.h>
#include <stdlib.h>
#include <string.h>
#include <time.h>

static uint32_t *load(const char *path, size_t *n) {
    FILE *f = fopen(path, "rb");
    if (!f) return NULL;
    fseek(f, 0, SEEK_END);
    long sz = ftell(f);
    fseek(f, 0, SEEK_SET);
    if (sz < 0 || sz % 4) { fclose(f); return NULL; }
    uint32_t *t = malloc((size_t)sz + 4);
    if (t && fread(t, 1, (size_t)sz, f) != (size_t)sz) { free(t); t = NULL; }
    fclose(f);
    *n = (size_t)sz / 4;
    return t;
}

static int write_file(const char *path, const char *s, size_t n) {
    FILE *f = fopen(path, "wb");
    if (!f) return -1;
    int ok = fwrite(s, 1, n, f) == n;
    return (fclose(f) == 0 && ok) ? 0 : -1;
}

static int batch(const char *list, const char *outdir) {
    FILE *f = fopen(list, "r");
    if (!f) { fprintf(stderr, "cannot read %s\n", list); return 2; }
    char line[2048], blob[1024], name[256], entry[128], path[1600];
    unsigned flags;
    long ok = 0, bad = 0;
    struct timespec t0, t1;
    clock_gettime(CLOCK_MONOTONIC, &t0);
    double tr = 0;
    while (fgets(line, sizeof line, f)) {
        if (sscanf(line, "%1023s %255s %127s %u", blob, name, entry, &flags) != 4) continue;
        size_t n;
        uint32_t *t = load(blob, &n);
        d3d9msl_result r;
        d3d9msl_options o = {entry, flags};
        int rc;
        struct timespec a, b;
        if (!t) { rc = -9; memset(&r, 0, sizeof r); snprintf(r.error, sizeof r.error, "cannot read %s", blob); }
        else {
            clock_gettime(CLOCK_MONOTONIC, &a);
            rc = d3d9msl_translate_opts(t, n, &o, &r);
            clock_gettime(CLOCK_MONOTONIC, &b);
            tr += (b.tv_sec - a.tv_sec) + (b.tv_nsec - a.tv_nsec) * 1e-9;
        }
        if (rc == 0) {
            snprintf(path, sizeof path, "%s/%s.metal", outdir, name);
            write_file(path, r.msl, r.msl_len);
            char *js = d3d9msl_reflection_json(&r.refl, r.entry);
            snprintf(path, sizeof path, "%s/%s.json", outdir, name);
            if (js) write_file(path, js, strlen(js));
            free(js);
            ok++;
        } else {
            snprintf(path, sizeof path, "%s/%s.err", outdir, name);
            char msg[400];
            int l = snprintf(msg, sizeof msg, "%d %s\n", rc, r.error);
            write_file(path, msg, (size_t)l);
            bad++;
        }
        d3d9msl_result_free(&r);
        free(t);
    }
    fclose(f);
    clock_gettime(CLOCK_MONOTONIC, &t1);
    printf("batch: %ld translated, %ld failed, translate time %.3f s (%.1f us each), wall %.3f s\n", ok, bad, tr,
           ok + bad ? tr * 1e6 / (double)(ok + bad) : 0.0, (t1.tv_sec - t0.tv_sec) + (t1.tv_nsec - t0.tv_nsec) * 1e-9);
    return bad ? 1 : 0;
}

int main(int argc, char **argv) {
    d3d9msl_options o = {NULL, 0};
    const char *json = NULL, *cache = NULL;
    int i = 1;
    if (argc >= 4 && !strcmp(argv[1], "--batch")) return batch(argv[2], argv[3]);
    for (; i < argc && argv[i][0] == '-' && argv[i][1] == '-'; i++) {
        if (!strcmp(argv[i], "--entry") && i + 1 < argc) o.entry = argv[++i];
        else if (!strcmp(argv[i], "--fog") && i + 1 < argc) { if (!strcmp(argv[++i], "vertex")) o.flags |= D3D9MSL_OPT_FOG_VERTEX; }
        else if (!strcmp(argv[i], "--flat")) o.flags |= D3D9MSL_OPT_FLAT;
        else if (!strcmp(argv[i], "--plain")) o.flags |= D3D9MSL_OPT_PLAIN;
        else if (!strcmp(argv[i], "--no-prelude")) o.flags |= D3D9MSL_OPT_NO_PRELUDE;
        else if (!strcmp(argv[i], "--json") && i + 1 < argc) json = argv[++i];
        else if (!strcmp(argv[i], "--cache") && i + 1 < argc) cache = argv[++i];
        else { fprintf(stderr, "unknown option %s\n", argv[i]); return 2; }
    }
    if (i >= argc) { fprintf(stderr, "usage: see the comment at the top of d3d9msl_tool.c\n"); return 2; }
    if (cache) {
        if (d3d9msl_cache_init(cache)) { fprintf(stderr, "cache %s unusable\n", cache); return 1; }
        int bad = 0;
        for (; i < argc; i++) {
            size_t n;
            uint32_t *t = load(argv[i], &n);
            if (!t) { printf("FAIL %s: unreadable\n", argv[i]); bad++; continue; }
            d3d9msl_result r;
            int hit = 0, rc = d3d9msl_translate_cached(t, n, &o, &r, &hit);
            char key[17];
            d3d9msl_cache_key(t, n, &o, key);
            if (rc) { printf("FAIL %s: %s\n", argv[i], r.error); bad++; }
            else printf("%s %s %s %zu bytes\n", hit ? "hit " : "miss", key, argv[i], r.msl_len);
            d3d9msl_result_free(&r);
            free(t);
        }
        d3d9msl_cache_stats s;
        d3d9msl_cache_get_stats(&s);
        printf("cache %s: hits %llu misses %llu stores %llu errors %llu\n", d3d9msl_cache_dir(),
               (unsigned long long)s.hits, (unsigned long long)s.misses, (unsigned long long)s.stores,
               (unsigned long long)s.errors);
        return bad ? 1 : 0;
    }
    size_t n;
    uint32_t *t = load(argv[i], &n);
    if (!t) { fprintf(stderr, "cannot read %s\n", argv[i]); return 1; }
    d3d9msl_result r;
    int rc = d3d9msl_translate_opts(t, n, &o, &r);
    if (rc) { fprintf(stderr, "error %d: %s\n", rc, r.error); free(t); return 1; }
    fwrite(r.msl, 1, r.msl_len, stdout);
    if (json) {
        char *js = d3d9msl_reflection_json(&r.refl, r.entry);
        if (!js || write_file(json, js, strlen(js))) { fprintf(stderr, "cannot write %s\n", json); rc = 1; }
        free(js);
    }
    d3d9msl_result_free(&r);
    free(t);
    return rc ? 1 : 0;
}
