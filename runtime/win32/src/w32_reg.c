/* w32_reg.c -- the registry subset the game reads, over an in-memory tree
 * loaded from a small JSON file (win32-layer.md section 8).
 *
 * Store format (UTF-8 JSON):
 *   { "format": "spore-native registry 1",
 *     "keys": { "HKEY_LOCAL_MACHINE\\SOFTWARE\\electronic arts\\spore": {
 *                   "appdir": "Spore",                 REG_SZ
 *                   "installcompleted": {"dword": 1},   REG_DWORD
 *                   "blob": {"binary": "0a0b"},         REG_BINARY
 *                   "": "default value" }, ... } }
 * Key and value names are case-insensitive and keep the case they were created with.
 * HKLM\SOFTWARE\WOW6432Node\X is an alias of HKLM\SOFTWARE\X (the 32-bit view).
 */
#include <pthread.h>
#include <stdio.h>
#include <stdlib.h>
#include <string.h>
#include <sys/stat.h>

#include "w32_int.h"
#include "w32_json.h"

typedef struct rval { char *name; uint32_t type; uint8_t *data; uint32_t len; } rval; /* SZ: UTF-8 + NUL */
typedef struct rkey {
    char *name;
    struct rkey *parent;
    struct rkey **kids; size_t nkids;
    rval *vals; size_t nvals;
} rkey;

static rkey roots[4]; /* HKCR, HKCU, HKLM, HKU */
static const char *const root_names[4] = {"HKEY_CLASSES_ROOT", "HKEY_CURRENT_USER", "HKEY_LOCAL_MACHINE", "HKEY_USERS"};
static const char *const root_short[4] = {"HKCR", "HKCU", "HKLM", "HKU"};
#define MAX_OPEN 1024
#define REG_HANDLE_BASE 0x00b00000u
static rkey *open_keys[MAX_OPEN];
static pthread_mutex_t reg_lock = PTHREAD_MUTEX_INITIALIZER;
static char store_path[W32_PATH_CAP];
static int persist;

static char *dupstr(const char *s) { size_t n = strlen(s) + 1; char *d = (char *)malloc(n); memcpy(d, s, n); return d; }

static rkey *child(rkey *k, const char *name, int create) {
    size_t i;
    for (i = 0; i < k->nkids; i++) {
        int c = w32_name_casecmp(k->kids[i]->name, name);
        if (!c) return k->kids[i];
        if (c > 0) break; /* kept sorted: RegEnumKeyEx returns names in this order */
    }
    if (!create) return NULL;
    rkey *n = (rkey *)calloc(1, sizeof *n);
    n->name = dupstr(name);
    n->parent = k;
    k->kids = (rkey **)realloc(k->kids, (k->nkids + 1) * sizeof *k->kids);
    memmove(k->kids + i + 1, k->kids + i, (k->nkids - i) * sizeof *k->kids);
    k->kids[i] = n;
    k->nkids++;
    return n;
}

/* Walks "a\b\c" below k. */
static rkey *walk(rkey *k, const char *path, int create) {
    char comp[256];
    while (k && *path) {
        const char *e = strchr(path, '\\');
        size_t n = e ? (size_t)(e - path) : strlen(path);
        if (n >= sizeof comp) return NULL;
        memcpy(comp, path, n);
        comp[n] = 0;
        path += n;
        while (*path == '\\') path++;
        if (!n) continue;
        /* the 32-bit view: HKLM\SOFTWARE\WOW6432Node\X is HKLM\SOFTWARE\X */
        if (!w32_name_casecmp(comp, "WOW6432Node") && k->parent == &roots[2] && !w32_name_casecmp(k->name, "SOFTWARE"))
            continue;
        k = child(k, comp, create);
    }
    return k;
}

static rkey *from_handle(uint32_t h) {
    if (h >= W32_HKCR && h <= W32_HKU) return &roots[h - W32_HKCR];
    if (h >= REG_HANDLE_BASE + 4 && h < REG_HANDLE_BASE + 4 * (MAX_OPEN + 1) && !(h & 3))
        return open_keys[(h - REG_HANDLE_BASE) / 4 - 1];
    return NULL;
}

static uint32_t new_handle(rkey *k) {
    for (int i = 0; i < MAX_OPEN; i++)
        if (!open_keys[i]) { open_keys[i] = k; return REG_HANDLE_BASE + 4u * (uint32_t)(i + 1); }
    return 0;
}

static rval *find_val(rkey *k, const char *name, int create) {
    for (size_t i = 0; i < k->nvals; i++)
        if (!w32_name_casecmp(k->vals[i].name, name)) return &k->vals[i];
    if (!create) return NULL;
    k->vals = (rval *)realloc(k->vals, (k->nvals + 1) * sizeof *k->vals);
    rval *v = &k->vals[k->nvals++];
    memset(v, 0, sizeof *v);
    v->name = dupstr(name);
    return v;
}

static void set_val(rkey *k, const char *name, uint32_t type, const void *data, uint32_t len) {
    rval *v = find_val(k, name, 1);
    free(v->data);
    v->type = type;
    v->data = (uint8_t *)malloc(len + 1);
    if (len) memcpy(v->data, data, len);
    v->data[len] = 0;
    v->len = len;
}

static int is_string_type(uint32_t t) { return t == W32_REG_SZ || t == W32_REG_EXPAND_SZ; }

/* ---- host API ---------------------------------------------------------------------------- */

int w32_reg_open(uint32_t parent, const char *subkey, int create, uint32_t *out) {
    int r = W32_ERROR_SUCCESS;
    pthread_mutex_lock(&reg_lock);
    rkey *k = from_handle(parent);
    if (!k) { r = W32_ERROR_INVALID_HANDLE; goto out; }
    k = walk(k, subkey ? subkey : "", create);
    if (!k) { r = W32_ERROR_FILE_NOT_FOUND; goto out; }
    if (!(*out = new_handle(k))) r = W32_ERROR_OUTOFMEMORY;
out:
    pthread_mutex_unlock(&reg_lock);
    return r;
}

int w32_reg_close(uint32_t h) {
    if (h >= W32_HKCR && h <= W32_HKU) return W32_ERROR_SUCCESS;
    pthread_mutex_lock(&reg_lock);
    int r = W32_ERROR_INVALID_HANDLE;
    if (from_handle(h)) { open_keys[(h - REG_HANDLE_BASE) / 4 - 1] = NULL; r = W32_ERROR_SUCCESS; }
    pthread_mutex_unlock(&reg_lock);
    return r;
}

/* Converts a stored value to the guest encoding into a malloc'd buffer. */
static uint8_t *encode_val(const rval *v, int wide, uint32_t *len) {
    if (!is_string_type(v->type)) {
        uint8_t *b = (uint8_t *)malloc(v->len ? v->len : 1);
        memcpy(b, v->data, v->len);
        *len = v->len;
        return b;
    }
    uint32_t units = w32_utf8_units((const char *)v->data, wide) + 1;
    uint32_t bytes = units * (wide ? 2u : 1u);
    /* encode through a scratch guest-free path: build in host memory */
    uint8_t *b = (uint8_t *)calloc(1, bytes);
    const char *s = (const char *)v->data;
    uint32_t n = 0, cp;
    while ((cp = w32_utf8_decode(&s))) {
        if (wide) {
            if (cp >= 0x10000) {
                cp -= 0x10000;
                uint16_t hi = (uint16_t)(0xd800 + (cp >> 10)), lo = (uint16_t)(0xdc00 + (cp & 0x3ff));
                memcpy(b + 2 * n, &hi, 2); memcpy(b + 2 * n + 2, &lo, 2);
                n += 2;
            } else {
                uint16_t u = (uint16_t)cp;
                memcpy(b + 2 * n, &u, 2);
                n++;
            }
        } else {
            b[n++] = (uint8_t)w32_unicode_to_cp1252(cp); /* characters outside 1252 become '?' */
        }
    }
    *len = bytes;
    return b;
}

int w32_reg_query(uint32_t h, const char *name, uint32_t *type, uint8_t *data, uint32_t *len, int wide) {
    int r = W32_ERROR_SUCCESS;
    pthread_mutex_lock(&reg_lock);
    rkey *k = from_handle(h);
    rval *v;
    if (!k) { r = W32_ERROR_INVALID_HANDLE; goto out; }
    if (!(v = find_val(k, name ? name : "", 0))) { r = W32_ERROR_FILE_NOT_FOUND; goto out; }
    uint32_t n;
    uint8_t *b = encode_val(v, wide, &n);
    if (type) *type = v->type;
    if (data) {
        if (!len) r = W32_ERROR_INVALID_PARAMETER;
        else if (*len < n) { r = W32_ERROR_MORE_DATA; *len = n; }
        else { memcpy(data, b, n); *len = n; }
    } else if (len) {
        *len = n;
    }
    free(b);
out:
    pthread_mutex_unlock(&reg_lock);
    return r;
}

int w32_reg_set(uint32_t h, const char *name, uint32_t type, const uint8_t *data, uint32_t len, int wide) {
    pthread_mutex_lock(&reg_lock);
    rkey *k = from_handle(h);
    if (!k) { pthread_mutex_unlock(&reg_lock); return W32_ERROR_INVALID_HANDLE; }
    if (is_string_type(type)) {
        /* guest encoding -> UTF-8; the data may or may not include its NUL */
        size_t cap = (size_t)len * 3 + 4, o = 0;
        char *u = (char *)malloc(cap);
        if (wide) {
            for (uint32_t i = 0; i + 1 < len + 1 && 2 * i + 1 < len; i++) {
                uint16_t c;
                memcpy(&c, data + 2 * i, 2);
                if (!c) break;
                o += w32_utf8_encode(c, u + o);
            }
        } else {
            for (uint32_t i = 0; i < len && data[i]; i++) {
                o += w32_utf8_encode(w32_cp1252_to_unicode(data[i]), u + o);
            }
        }
        u[o] = 0;
        set_val(k, name ? name : "", type, u, (uint32_t)o + 1);
        free(u);
    } else {
        set_val(k, name ? name : "", type, data, len);
    }
    pthread_mutex_unlock(&reg_lock);
    if (persist) w32_reg_save();
    return W32_ERROR_SUCCESS;
}

int w32_reg_enum_key(uint32_t h, uint32_t index, char *name, size_t cap) {
    int r = W32_ERROR_SUCCESS;
    pthread_mutex_lock(&reg_lock);
    rkey *k = from_handle(h);
    if (!k) r = W32_ERROR_INVALID_HANDLE;
    else if (index >= k->nkids) r = W32_ERROR_NO_MORE_ITEMS;
    else snprintf(name, cap, "%s", k->kids[index]->name);
    pthread_mutex_unlock(&reg_lock);
    return r;
}

/* ---- store -------------------------------------------------------------------------------- */

static void free_tree(rkey *k) {
    for (size_t i = 0; i < k->nkids; i++) { free_tree(k->kids[i]); free(k->kids[i]); }
    for (size_t i = 0; i < k->nvals; i++) { free(k->vals[i].name); free(k->vals[i].data); }
    free(k->kids); free(k->vals); free(k->name);
    k->kids = NULL; k->vals = NULL; k->nkids = k->nvals = 0; k->name = NULL;
}

static rkey *root_of(const char *path, const char **rest) {
    for (int i = 0; i < 4; i++) {
        size_t a = strlen(root_names[i]), b = strlen(root_short[i]);
        const char *n = NULL;
        if (!strncasecmp(path, root_names[i], a) && (path[a] == '\\' || !path[a])) n = path + a;
        else if (!strncasecmp(path, root_short[i], b) && (path[b] == '\\' || !path[b])) n = path + b;
        if (n) { *rest = *n ? n + 1 : n; return &roots[i]; }
    }
    return NULL;
}

static int load_json(const char *text, char *err, size_t errcap) {
    jv *doc = json_parse(text, err, errcap), *keys;
    if (!doc) return -1;
    if (!(keys = json_get(doc, "keys")) || keys->type != JV_OBJECT) {
        snprintf(err, errcap, "no \"keys\" object");
        json_free(doc);
        return -1;
    }
    for (size_t i = 0; i < keys->n; i++) {
        const char *rest;
        rkey *r = root_of(keys->keys[i], &rest), *k;
        jv *vals = keys->items[i];
        if (!r || vals->type != JV_OBJECT) { hle_log(HLE_LOG_WARN, "registry: skipping key %s", keys->keys[i]); continue; }
        k = walk(r, rest, 1);
        for (size_t j = 0; j < vals->n; j++) {
            jv *v = vals->items[j], *x;
            const char *name = vals->keys[j];
            if (v->type == JV_STRING) set_val(k, name, W32_REG_SZ, v->s, (uint32_t)strlen(v->s) + 1);
            else if (v->type == JV_NUMBER) { uint32_t d = (uint32_t)(int64_t)v->num; set_val(k, name, W32_REG_DWORD, &d, 4); }
            else if ((x = json_get(v, "dword")) && x->type == JV_NUMBER) {
                uint32_t d = (uint32_t)(int64_t)x->num;
                set_val(k, name, W32_REG_DWORD, &d, 4);
            } else if ((x = json_get(v, "expand_sz")) && x->type == JV_STRING) {
                set_val(k, name, W32_REG_EXPAND_SZ, x->s, (uint32_t)strlen(x->s) + 1);
            } else if ((x = json_get(v, "binary")) && x->type == JV_STRING) {
                size_t n = strlen(x->s) / 2;
                uint8_t *b = (uint8_t *)malloc(n + 1);
                for (size_t q = 0; q < n; q++) { unsigned t; sscanf(x->s + 2 * q, "%2x", &t); b[q] = (uint8_t)t; }
                set_val(k, name, W32_REG_BINARY, b, (uint32_t)n);
                free(b);
            } else {
                hle_log(HLE_LOG_WARN, "registry: unsupported value %s\\%s", keys->keys[i], name);
            }
        }
    }
    json_free(doc);
    return 0;
}

static void key_path(rkey *k, char *out, size_t cap) {
    if (!k->parent) {
        for (int i = 0; i < 4; i++)
            if (k == &roots[i]) snprintf(out, cap, "%s", root_names[i]);
        return;
    }
    key_path(k->parent, out, cap);
    size_t n = strlen(out);
    snprintf(out + n, cap - n, "\\%s", k->name);
}

static void dump(rkey *k, jbuf *b, int *first) {
    if (k->parent && (k->nvals || !k->nkids)) {
        char path[2048];
        key_path(k, path, sizeof path);
        jbuf_puts(b, *first ? "\n    " : ",\n    ");
        *first = 0;
        json_put_string(b, path);
        jbuf_puts(b, ": {");
        for (size_t i = 0; i < k->nvals; i++) {
            rval *v = &k->vals[i];
            jbuf_puts(b, i ? ", " : "");
            json_put_string(b, v->name);
            jbuf_puts(b, ": ");
            if (v->type == W32_REG_SZ) {
                json_put_string(b, (const char *)v->data);
            } else if (v->type == W32_REG_EXPAND_SZ) {
                jbuf_puts(b, "{\"expand_sz\": ");
                json_put_string(b, (const char *)v->data);
                jbuf_puts(b, "}");
            } else if (v->type == W32_REG_DWORD && v->len == 4) {
                uint32_t d;
                char t[48];
                memcpy(&d, v->data, 4);
                snprintf(t, sizeof t, "{\"dword\": %u}", d);
                jbuf_puts(b, t);
            } else {
                jbuf_puts(b, "{\"binary\": \"");
                for (uint32_t q = 0; q < v->len; q++) { char t[4]; snprintf(t, sizeof t, "%02x", v->data[q]); jbuf_puts(b, t); }
                jbuf_puts(b, "\"}");
            }
        }
        jbuf_puts(b, "}");
    }
    for (size_t i = 0; i < k->nkids; i++) dump(k->kids[i], b, first);
}

static char *to_json(void) {
    jbuf b = {0};
    int first = 1;
    jbuf_puts(&b, "{\n  \"format\": \"spore-native registry 1\",\n  \"keys\": {");
    for (int i = 0; i < 4; i++) dump(&roots[i], &b, &first);
    jbuf_puts(&b, "\n  }\n}\n");
    return b.p;
}

int w32_reg_save(void) {
    if (!store_path[0]) return -1;
    pthread_mutex_lock(&reg_lock);
    char *text = to_json();
    pthread_mutex_unlock(&reg_lock);
    char tmp[W32_PATH_CAP + 8];
    snprintf(tmp, sizeof tmp, "%s.tmp", store_path);
    FILE *f = fopen(tmp, "wb");
    int r = -1;
    if (f) {
        r = fwrite(text, 1, strlen(text), f) == strlen(text) ? 0 : -1;
        if (fclose(f)) r = -1;
        if (!r) r = rename(tmp, store_path);
    }
    free(text);
    return r;
}

/* What the GOG installer writes [obs: the installer's goggame-1948823323.script,
 * setRegistry actions], plus the Internet Settings key every Windows install has, which the network check
 * at 0x94c48b reads and writes. */
char *w32_reg_seed_json(const char *install) {
    jbuf b = {0};
    char app[W32_PATH_CAP], t[W32_PATH_CAP * 2];
    snprintf(app, sizeof app, "%s", install);
    jbuf_puts(&b, "{\n  \"format\": \"spore-native registry 1\",\n  \"keys\": {\n");
    jbuf_puts(&b, "    \"HKEY_LOCAL_MACHINE\\\\SOFTWARE\\\\electronic arts\\\\spore\": {\n");
    jbuf_puts(&b, "      \"reindexyesorno\": {\"dword\": 0}, \"installcompleted\": {\"dword\": 1},\n");
    jbuf_puts(&b, "      \"appdir\": \"Spore\", \"installloc\": ");
    json_put_string(&b, app);
    snprintf(t, sizeof t, "%s\\Data", app);
    jbuf_puts(&b, ", \"datadir\": ");
    json_put_string(&b, t);
    jbuf_puts(&b, ",\n      \"playerdir\": \"My Spore Creations\", \"locale\": \"en-us\"},\n");
    jbuf_puts(&b, "    \"HKEY_LOCAL_MACHINE\\\\SOFTWARE\\\\electronic arts\\\\spore_EP1\": {\n");
    snprintf(t, sizeof t, "%s\\DataEP1\\", app);
    jbuf_puts(&b, "      \"AddOnID\": {\"dword\": 2}, \"PackID\": {\"dword\": 128448390}, \"DataDir\": ");
    json_put_string(&b, t);
    jbuf_puts(&b, "},\n");
    jbuf_puts(&b, "    \"HKEY_LOCAL_MACHINE\\\\SOFTWARE\\\\electronic arts\\\\spore Creepy and Cute Parts Pack\": {\n");
    snprintf(t, sizeof t, "%s\\bp1content\\", app);
    jbuf_puts(&b, "      \"AddOnID\": {\"dword\": 1}, \"PackID\": {\"dword\": 116700625}, \"DataDir\": ");
    json_put_string(&b, t);
    jbuf_puts(&b, "},\n");
    jbuf_puts(&b, "    \"HKEY_LOCAL_MACHINE\\\\SOFTWARE\\\\electronic arts\\\\ea games\\\\spore(tm)\": {},\n");
    jbuf_puts(&b, "    \"HKEY_CURRENT_USER\\\\Software\\\\Microsoft\\\\Windows\\\\CurrentVersion\\\\Internet Settings\": "
                  "{\"EnableAutodial\": {\"dword\": 0}}\n");
    jbuf_puts(&b, "  }\n}\n");
    return b.p;
}

int w32_reg_load(const char *path, int persist_flag) {
    char err[160];
    for (int i = 0; i < 4; i++) free_tree(&roots[i]);
    memset(open_keys, 0, sizeof open_keys);
    snprintf(store_path, sizeof store_path, "%s", path ? path : "");
    persist = persist_flag;
    FILE *f = path ? fopen(path, "rb") : NULL;
    if (!f) {
        char *seed = w32_reg_seed_json(w32_folder(W32_F_INSTALL)[0] ? w32_folder(W32_F_INSTALL) : "C:\\Spore");
        int r = load_json(seed, err, sizeof err);
        free(seed);
        if (r) w32_fatal("registry seed does not parse: %s", err);
        if (path && w32_reg_save()) hle_log(HLE_LOG_WARN, "registry: cannot write %s", path);
        else if (path) hle_log(HLE_LOG_INFO, "registry: seeded %s from the GOG installer values", path);
        return 0;
    }
    fseek(f, 0, SEEK_END);
    long n = ftell(f);
    fseek(f, 0, SEEK_SET);
    char *text = (char *)malloc((size_t)n + 1);
    size_t got = fread(text, 1, (size_t)n, f);
    fclose(f);
    text[got] = 0;
    int r = load_json(text, err, sizeof err);
    free(text);
    if (r) {
        /* keep the damaged file for the user; run on the GOG seed in memory without saving */
        hle_log(HLE_LOG_ERROR, "registry: %s: %s; using the built-in GOG values, not saving", path, err);
        for (int i = 0; i < 4; i++) free_tree(&roots[i]);
        char *seed = w32_reg_seed_json(w32_folder(W32_F_INSTALL)[0] ? w32_folder(W32_F_INSTALL) : "C:\\Spore");
        load_json(seed, err, sizeof err);
        free(seed);
        store_path[0] = 0;
        persist = 0;
        return -1;
    }
    return 0;
}

/* ---- shims ---------------------------------------------------------------------------- */

static void arg_name(const uint8_t *mem, uint32_t a, int wide, char *out, size_t cap) {
    if (!a) { out[0] = 0; return; }
    if (wide) w32_wstr_get(mem, a, out, cap);
    else w32_astr_get(mem, a, out, cap);
}

static uint32_t open_common(uint8_t *mem, int wide, uint32_t hkey, uint32_t sub, uint32_t result) {
    char name[1024];
    uint32_t h = 0;
    arg_name(mem, sub, wide, name, sizeof name);
    if (!result) return W32_ERROR_INVALID_PARAMETER;
    int r = w32_reg_open(hkey, name, 0, &h);
    sg_st32(mem, result, r ? 0 : h);
    hle_log(HLE_LOG_INFO, "RegOpenKeyEx(0x%08x, \"%s\") = %d", hkey, name, r);
    return (uint32_t)r;
}
W32_IMPORT(advapi32, RegOpenKeyExA) { SG_RETURN_STDCALL(open_common(mem, 0, SG_ARG(0), SG_ARG(1), SG_ARG(4)), 20); }
W32_IMPORT(advapi32, RegOpenKeyExW) { SG_RETURN_STDCALL(open_common(mem, 1, SG_ARG(0), SG_ARG(1), SG_ARG(4)), 20); }
W32_IMPORT(advapi32, RegCloseKey) { SG_RETURN_STDCALL(w32_reg_close(SG_ARG(0)), 4); }

static uint32_t do_query(sg_ctx *ctx, uint8_t *mem, int wide) {
    char name[1024];
    uint32_t type = 0, h = SG_ARG(0), ptype = SG_ARG(3), pdata = SG_ARG(4), pcb = SG_ARG(5);
    uint32_t len = pcb ? sg_ld32(mem, pcb) : 0;
    arg_name(mem, SG_ARG(1), wide, name, sizeof name);
    if (pdata && !pcb) return W32_ERROR_INVALID_PARAMETER;
    uint8_t *tmp = pdata ? (uint8_t *)malloc(len ? len : 1) : NULL;
    int r = w32_reg_query(h, name, &type, tmp, pcb ? &len : NULL, wide);
    if ((r == 0 || r == W32_ERROR_MORE_DATA) && ptype) sg_st32(mem, ptype, type);
    if (r == 0 && pdata) memcpy(mem + pdata, tmp, len);
    if ((r == 0 || r == W32_ERROR_MORE_DATA) && pcb) sg_st32(mem, pcb, len);
    free(tmp);
    hle_log(HLE_LOG_INFO, "RegQueryValueEx(0x%08x, \"%s\") = %d (type %u, %u bytes)", h, name, r, type, len);
    return (uint32_t)r;
}
W32_IMPORT(advapi32, RegQueryValueExA) { SG_RETURN_STDCALL(do_query(ctx, mem, 0), 24); }
W32_IMPORT(advapi32, RegQueryValueExW) { SG_RETURN_STDCALL(do_query(ctx, mem, 1), 24); }

W32_IMPORT(advapi32, RegSetValueExA) {
    char name[1024];
    uint32_t data = SG_ARG(4), len = SG_ARG(5);
    arg_name(mem, SG_ARG(1), 0, name, sizeof name);
    int r = w32_reg_set(SG_ARG(0), name, SG_ARG(3), data ? mem + data : (const uint8_t *)"", data ? len : 0, 0);
    hle_log(HLE_LOG_INFO, "RegSetValueExA(0x%08x, \"%s\", type %u, %u bytes) = %d", SG_ARG(0), name, SG_ARG(3), len, r);
    SG_RETURN_STDCALL(r, 24);
}

W32_IMPORT(advapi32, RegEnumKeyExW) {
    /* (hKey, dwIndex, lpName, lpcchName, lpReserved, lpClass, lpcchClass, lpftLastWriteTime) */
    char name[1024];
    uint32_t pname = SG_ARG(2), pcch = SG_ARG(3), pclass = SG_ARG(5), pcclass = SG_ARG(6), pft = SG_ARG(7);
    if (!pname || !pcch) SG_RETURN_STDCALL(W32_ERROR_INVALID_PARAMETER, 32);
    int r = w32_reg_enum_key(SG_ARG(0), SG_ARG(1), name, sizeof name);
    if (!r) {
        uint32_t cap = sg_ld32(mem, pcch), need = w32_utf8_units(name, 1);
        if (need + 1 > cap) r = W32_ERROR_MORE_DATA;
        else {
            w32_put_str_trunc(mem, pname, cap, name, 1);
            sg_st32(mem, pcch, need);
            if (pclass && pcclass && sg_ld32(mem, pcclass)) sg_st16(mem, pclass, 0);
            if (pcclass) sg_st32(mem, pcclass, 0);
            if (pft) sg_st64(mem, pft, w32_filetime_from_unix(1220000000, 0)); /* fixed: Sept 2008 */
        }
    }
    hle_log(HLE_LOG_INFO, "RegEnumKeyExW(0x%08x, %u) = %d%s%s%s", SG_ARG(0), SG_ARG(1), r, r ? "" : " \"", r ? "" : name,
            r ? "" : "\"");
    SG_RETURN_STDCALL(r, 32);
}
