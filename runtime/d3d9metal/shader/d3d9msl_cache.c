/* d3d9msl_cache.c -- the translated-shader cache under build/d3d9metal/shadercache/. See d3d9msl.h. */
#include "d3d9msl.h"

#include <errno.h>
#include <pthread.h>
#include <stdio.h>
#include <stdlib.h>
#include <string.h>
#include <sys/stat.h>
#include <unistd.h>

#ifndef SG_REPO_ROOT
#define SG_REPO_ROOT "."
#endif

#define MAGIC "D3D9MSL\0"

static pthread_mutex_t g_lock = PTHREAD_MUTEX_INITIALIZER;
static int g_state;            /* 0 not initialised, 1 enabled, -1 disabled */
static char g_dir[1024];
static d3d9msl_cache_stats g_stats;

static void mkdir_p(const char *path) {
    char buf[1024];
    snprintf(buf, sizeof buf, "%s", path);
    for (char *p = buf + 1; *p; p++)
        if (*p == '/') {
            *p = 0;
            mkdir(buf, 0755);
            *p = '/';
        }
    mkdir(buf, 0755);
}

int d3d9msl_cache_init(const char *dir) {
    char d[1024];
    if (!dir) {
        const char *env = getenv("SPORE_D3D9MSL_CACHE");
        if (env && *env) dir = env;
    }
    if (dir) snprintf(d, sizeof d, "%s", dir);
    else {
        const char *root = getenv("SPORE_REPO_ROOT");
        snprintf(d, sizeof d, "%s/build/d3d9metal/shadercache", root && *root ? root : SG_REPO_ROOT);
    }
    pthread_mutex_lock(&g_lock);
    int rc = -1;
    if (!strcmp(d, "off") || !*d) g_state = -1;
    else {
        mkdir_p(d);
        struct stat st;
        if (stat(d, &st) == 0 && S_ISDIR(st.st_mode) && access(d, W_OK) == 0) {
            snprintf(g_dir, sizeof g_dir, "%s", d);
            g_state = 1;
            rc = 0;
        } else g_state = -1;
    }
    pthread_mutex_unlock(&g_lock);
    return rc;
}

const char *d3d9msl_cache_dir(void) { return g_state == 1 ? g_dir : NULL; }

static uint64_t fnv(uint64_t h, const void *p, size_t n) {
    const uint8_t *b = p;
    for (size_t i = 0; i < n; i++) { h ^= b[i]; h *= 1099511628211ull; }
    return h;
}

void d3d9msl_cache_key(const uint32_t *tokens, size_t ntokens, const d3d9msl_options *opts, char key[17]) {
    uint64_t h = d3d9msl_hash(tokens, ntokens);
    uint32_t flags = opts ? opts->flags : 0;
    const char *entry = opts && opts->entry ? opts->entry : "sg_main";
    h = fnv(h, D3D9MSL_VERSION, sizeof D3D9MSL_VERSION);
    h = fnv(h, &flags, 4);
    h = fnv(h, entry, strlen(entry) + 1);
    uint32_t rs = (uint32_t)sizeof(d3d9msl_reflection);
    h = fnv(h, &rs, 4);
    snprintf(key, 17, "%016llx", (unsigned long long)h);
}

typedef struct {
    char magic[8];
    uint32_t refl_size, ntokens, flags, entry_len, msl_len, pad;
    char version[8];
} hdr_t;

static void path_of(const char *key, char *out, size_t cap) { snprintf(out, cap, "%s/%s.d3d9msl", g_dir, key); }

int d3d9msl_cache_load(const uint32_t *tokens, size_t ntokens, const d3d9msl_options *opts, d3d9msl_result *out) {
    if (g_state != 1) return 0;
    char key[17], path[1200];
    d3d9msl_cache_key(tokens, ntokens, opts, key);
    path_of(key, path, sizeof path);
    FILE *f = fopen(path, "rb");
    if (!f) return 0;
    const char *entry = opts && opts->entry ? opts->entry : "sg_main";
    uint32_t flags = opts ? opts->flags : 0;
    hdr_t h;
    int rc = -1;
    uint32_t *tk = NULL;
    char *msl = NULL;
    char ent[64];
    memset(out, 0, sizeof *out);
    if (fread(&h, sizeof h, 1, f) != 1 || memcmp(h.magic, MAGIC, 8) || h.refl_size != sizeof(d3d9msl_reflection) ||
        strncmp(h.version, D3D9MSL_VERSION, sizeof h.version) || h.ntokens != ntokens || h.flags != flags ||
        h.entry_len >= sizeof ent || h.entry_len != strlen(entry))
        goto miss;
    tk = malloc(ntokens * 4 + 1);
    msl = malloc((size_t)h.msl_len + 1);
    if (!tk || !msl) goto done;
    if (fread(tk, 4, ntokens, f) != ntokens || memcmp(tk, tokens, ntokens * 4)) goto miss;
    if (fread(ent, 1, h.entry_len, f) != h.entry_len) goto miss;
    ent[h.entry_len] = 0;
    if (strcmp(ent, entry)) goto miss;
    if (fread(&out->refl, sizeof out->refl, 1, f) != 1) goto miss;
    if (fread(msl, 1, h.msl_len, f) != h.msl_len) goto miss;
    msl[h.msl_len] = 0;
    out->msl = msl;
    out->msl_len = h.msl_len;
    snprintf(out->entry, sizeof out->entry, "%s", entry);
    msl = NULL;
    rc = 1;
    goto done;
miss:
    rc = 0;
    memset(out, 0, sizeof *out);
done:
    free(tk);
    free(msl);
    fclose(f);
    return rc;
}

int d3d9msl_cache_store(const uint32_t *tokens, size_t ntokens, const d3d9msl_options *opts, const d3d9msl_result *res) {
    if (g_state != 1 || !res->msl) return -1;
    char key[17], path[1200], tmp[1300];
    d3d9msl_cache_key(tokens, ntokens, opts, key);
    path_of(key, path, sizeof path);
    static unsigned seq;
    pthread_mutex_lock(&g_lock);
    unsigned s = ++seq;
    pthread_mutex_unlock(&g_lock);
    snprintf(tmp, sizeof tmp, "%s.tmp.%d.%u", path, (int)getpid(), s);
    const char *entry = opts && opts->entry ? opts->entry : "sg_main";
    hdr_t h;
    memset(&h, 0, sizeof h);
    memcpy(h.magic, MAGIC, 8);
    h.refl_size = sizeof(d3d9msl_reflection);
    h.ntokens = (uint32_t)ntokens;
    h.flags = opts ? opts->flags : 0;
    h.entry_len = (uint32_t)strlen(entry);
    h.msl_len = (uint32_t)res->msl_len;
    snprintf(h.version, sizeof h.version, "%s", D3D9MSL_VERSION);
    FILE *f = fopen(tmp, "wb");
    if (!f) return -1;
    int ok = fwrite(&h, sizeof h, 1, f) == 1 && fwrite(tokens, 4, ntokens, f) == ntokens &&
             fwrite(entry, 1, h.entry_len, f) == h.entry_len && fwrite(&res->refl, sizeof res->refl, 1, f) == 1 &&
             fwrite(res->msl, 1, res->msl_len, f) == res->msl_len;
    ok = (fclose(f) == 0) && ok;
    if (!ok || rename(tmp, path)) {
        unlink(tmp);
        return -1;
    }
    return 0;
}

int d3d9msl_translate_cached(const uint32_t *tokens, size_t ntokens, const d3d9msl_options *opts,
                             d3d9msl_result *out, int *hit) {
    if (hit) *hit = 0;
    if (g_state == 0) d3d9msl_cache_init(NULL);
    int r = d3d9msl_cache_load(tokens, ntokens, opts, out);
    pthread_mutex_lock(&g_lock);
    if (r == 1) g_stats.hits++;
    else if (g_state == 1) g_stats.misses++;
    if (r < 0) g_stats.errors++;
    pthread_mutex_unlock(&g_lock);
    if (r == 1) {
        if (hit) *hit = 1;
        return D3D9MSL_OK;
    }
    int rc = d3d9msl_translate_opts(tokens, ntokens, opts, out);
    if (rc == D3D9MSL_OK && g_state == 1) {
        int s = d3d9msl_cache_store(tokens, ntokens, opts, out);
        pthread_mutex_lock(&g_lock);
        if (s == 0) g_stats.stores++;
        else g_stats.errors++;
        pthread_mutex_unlock(&g_lock);
    }
    return rc;
}

void d3d9msl_cache_get_stats(d3d9msl_cache_stats *s) {
    pthread_mutex_lock(&g_lock);
    *s = g_stats;
    pthread_mutex_unlock(&g_lock);
}
