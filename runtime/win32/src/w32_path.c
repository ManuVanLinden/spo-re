/* w32_path.c -- the Windows-path view of the host file system (design doc section 5).
 *
 * The guest sees one drive, C:. A mount table maps Windows prefixes to host directories:
 *     C:\Spore (install root, {app})        -> game_dir (SPORE_GAME_DIR)
 *     C:\Users\Player\Documents             -> documents_dir (default <support>/Documents)
 *     C:\Users\Player\AppData\Roaming       -> <support>/AppData      (so %APPDATA%\Spore is
 *                                               <support>/AppData/Spore)
 *     C:\Users\Player\AppData\Local\Temp    -> temp_dir
 *     C:\                                    -> <support>/drive_c      (everything else)
 * The longest matching prefix wins (compared case-insensitively on component boundaries).
 * Each existing component is then matched case-insensitively against the host directory, so
 * the game's mixed-case literals work on case-sensitive volumes too.
 */
#include <dirent.h>
#include <errno.h>
#include <limits.h>
#include <pthread.h>
#include <stdio.h>
#include <stdlib.h>
#include <string.h>
#include <sys/stat.h>
#include <unistd.h>

#include "w32_int.h"

typedef struct mount { char win[W32_PATH_CAP]; char host[W32_PATH_CAP]; } mount;
#define MAX_MOUNTS 16
static mount mounts[MAX_MOUNTS];
static int nmounts;
static char cwd[W32_PATH_CAP] = "C:\\";
static char folders[W32_F_COUNT][W32_PATH_CAP];
static pthread_mutex_t path_lock = PTHREAD_MUTEX_INITIALIZER;

/* ---- case folding -------------------------------------------------------------------- */

static uint32_t fold(uint32_t c) {
    if (c >= 'A' && c <= 'Z') return c + 32;
    if (c >= 0xc0 && c <= 0xde && c != 0xd7) return c + 32;          /* Latin-1 */
    if (c >= 0x100 && c <= 0x17f && !(c & 1) && c != 0x130) return c + 1; /* Latin Extended-A pairs (approx.) */
    if (c >= 0x391 && c <= 0x3a9) return c + 32;                       /* Greek */
    if (c >= 0x410 && c <= 0x42f) return c + 32;                       /* Cyrillic */
    if (c >= 0x400 && c <= 0x40f) return c + 80;
    return c;
}

int w32_name_casecmp(const char *a, const char *b) {
    for (;;) {
        uint32_t x = fold(w32_utf8_decode(&a)), y = fold(w32_utf8_decode(&b));
        if (x != y) return x < y ? -1 : 1;
        if (!x) return 0;
    }
}

/* Compares the first n bytes of a with b (prefix test), case-insensitively for ASCII/Latin-1. */
static int prefix_ci(const char *s, const char *prefix) {
    for (;;) {
        if (!*prefix) return 1;
        uint32_t x = fold(w32_utf8_decode(&s)), y = fold(w32_utf8_decode(&prefix));
        if (x != y) return 0;
    }
}

/* ---- wildcard matching ---------------------------------------------------------------- */

static int wmatch(const char *p, const char *n) {
    while (*p) {
        if (*p == '*') {
            while (*p == '*') p++;
            if (!*p) return 1;
            for (const char *t = n;; ) {
                if (wmatch(p, t)) return 1;
                if (!*t) return 0;
                w32_utf8_decode(&t);
            }
        }
        if (!*n) return 0;
        if (*p == '?') { p++; w32_utf8_decode(&n); continue; }
        uint32_t x = fold(w32_utf8_decode(&p)), y = fold(w32_utf8_decode(&n));
        if (x != y) return 0;
    }
    return !*n;
}

int w32_wildcard_match(const char *pattern, const char *name) {
    size_t lp = strlen(pattern);
    if (!strcmp(pattern, "*") || !strcmp(pattern, "*.*")) return 1;
    if (wmatch(pattern, name)) return 1;
    /* "name.*" also matches "name" (no extension), as on Windows */
    if (lp >= 2 && pattern[lp - 2] == '.' && pattern[lp - 1] == '*' && !strchr(name, '.')) {
        char tmp[W32_PATH_CAP];
        if (lp - 2 >= sizeof tmp) return 0;
        memcpy(tmp, pattern, lp - 2);
        tmp[lp - 2] = 0;
        return wmatch(tmp, name);
    }
    return 0;
}

/* ---- normalisation (GetFullPathName) -------------------------------------------------- */

static int is_alpha(char c) { return (c >= 'a' && c <= 'z') || (c >= 'A' && c <= 'Z'); }

int w32_path_full(const char *in, char *out, size_t cap) {
    char buf[2 * W32_PATH_CAP], work[2 * W32_PATH_CAP];
    size_t n = strlen(in);
    int trailing;
    if (n >= W32_PATH_CAP) return -1;
    for (size_t i = 0; i <= n; i++) buf[i] = in[i] == '/' ? '\\' : in[i];
    const char *s = buf;
    if (!strncmp(s, "\\\\?\\", 4)) {
        s += 4;
        if (!strncmp(s, "UNC\\", 4)) return -1;
    } else if (!strncmp(s, "\\\\", 2)) {
        return -1; /* UNC or device (\\.\X:) paths: not representable */
    }
    if (!*s) return -1;
    trailing = s[strlen(s) - 1] == '\\';
    pthread_mutex_lock(&path_lock);
    if (is_alpha(s[0]) && s[1] == ':') {
        if (s[2] == '\\') snprintf(work, sizeof work, "%s", s);
        else if ((cwd[0] | 32) == (s[0] | 32)) snprintf(work, sizeof work, "%s\\%s", cwd, s + 2);
        else snprintf(work, sizeof work, "%c:\\%s", s[0], s + 2);
    } else if (s[0] == '\\') {
        snprintf(work, sizeof work, "%c:%s", cwd[0], s);
    } else {
        snprintf(work, sizeof work, "%s\\%s", cwd, s);
    }
    pthread_mutex_unlock(&path_lock);
    /* fold components */
    char *comp[256];
    int nc = 0;
    char *p = work + 3; /* after "X:\" */
    while (*p) {
        char *e = strchr(p, '\\');
        if (e) *e = 0;
        if (!*p || !strcmp(p, ".")) {
        } else if (!strcmp(p, "..")) {
            if (nc) nc--;
        } else {
            if (nc == 256) return -1;
            comp[nc++] = p;
        }
        if (!e) break;
        p = e + 1;
    }
    if (nc) { /* trailing dots and spaces are dropped from the last component */
        char *l = comp[nc - 1];
        size_t k = strlen(l);
        while (k && (l[k - 1] == '.' || l[k - 1] == ' ')) l[--k] = 0;
        if (!k) nc--;
    }
    size_t o = 0;
    char head[4] = {work[0], ':', '\\', 0};
    if (o + 3 >= cap) return -1;
    memcpy(out, head, 4);
    o = 3;
    for (int i = 0; i < nc; i++) {
        size_t k = strlen(comp[i]);
        if (o + k + 2 >= cap) return -1;
        if (i) out[o++] = '\\';
        memcpy(out + o, comp[i], k);
        o += k;
    }
    if (trailing && nc) out[o++] = '\\';
    out[o] = 0;
    return (int)o;
}

/* ---- mounts and host resolution -------------------------------------------------------- */

static void strip_trailing_bs(char *s) {
    size_t n = strlen(s);
    while (n > 3 && s[n - 1] == '\\') s[--n] = 0;
}

int w32_path_mount(const char *win, const char *host) {
    char full[W32_PATH_CAP];
    if (w32_path_full(win, full, sizeof full) < 0) return -1;
    strip_trailing_bs(full);
    pthread_mutex_lock(&path_lock);
    int i;
    for (i = 0; i < nmounts; i++)
        if (!w32_name_casecmp(mounts[i].win, full)) break;
    if (i == nmounts) {
        if (nmounts == MAX_MOUNTS) { pthread_mutex_unlock(&path_lock); return -1; }
        nmounts++;
    }
    snprintf(mounts[i].win, sizeof mounts[i].win, "%s", full);
    snprintf(mounts[i].host, sizeof mounts[i].host, "%s", host);
    size_t n = strlen(mounts[i].host);
    while (n > 1 && mounts[i].host[n - 1] == '/') mounts[i].host[--n] = 0;
    pthread_mutex_unlock(&path_lock);
    return 0;
}

/* Finds `name` in host directory `dir` ignoring case; writes the real name. */
static int find_ci(const char *dir, const char *name, char *real, size_t cap) {
    DIR *d = opendir(dir);
    struct dirent *e;
    int found = 0;
    if (!d) return 0;
    while ((e = readdir(d))) {
        if (!w32_name_casecmp(e->d_name, name)) {
            snprintf(real, cap, "%s", e->d_name);
            found = 1;
            break;
        }
    }
    closedir(d);
    return found;
}

int w32_path_to_host(const char *full, char *host, size_t cap, int *exists, int *parent_exists) {
    int best = -1;
    size_t bestlen = 0;
    char win[W32_PATH_CAP];
    struct stat st;
    snprintf(win, sizeof win, "%s", full);
    strip_trailing_bs(win);
    if (exists) *exists = 0;
    if (parent_exists) *parent_exists = 0;
    pthread_mutex_lock(&path_lock);
    for (int i = 0; i < nmounts; i++) {
        size_t l = strlen(mounts[i].win);
        if (l < bestlen || !prefix_ci(win, mounts[i].win)) continue;
        char next = win[l];
        if (next && next != '\\' && mounts[i].win[l - 1] != '\\') continue; /* not a component boundary */
        best = i;
        bestlen = l;
    }
    if (best < 0) { pthread_mutex_unlock(&path_lock); return -1; }
    char cur[W32_PATH_CAP];
    snprintf(cur, sizeof cur, "%s", mounts[best].host);
    pthread_mutex_unlock(&path_lock);
    const char *rest = win + bestlen;
    while (*rest == '\\') rest++;
    int ok = stat(cur, &st) == 0 && S_ISDIR(st.st_mode); /* does the directory so far exist? */
    int last_ok = ok, parent_ok = ok;
    if (!*rest) { /* the mount point itself */
        if (exists) *exists = ok;
        if (parent_exists) *parent_exists = 1;
        snprintf(host, cap, "%s", cur);
        return 0;
    }
    while (*rest) {
        const char *e = strchr(rest, '\\');
        size_t k = e ? (size_t)(e - rest) : strlen(rest);
        char comp[NAME_MAX + 1], real[NAME_MAX + 1], cand[W32_PATH_CAP];
        if (k > NAME_MAX) k = NAME_MAX;
        memcpy(comp, rest, k);
        comp[k] = 0;
        parent_ok = last_ok;
        if (last_ok) {
            snprintf(cand, sizeof cand, "%s/%s", cur, comp);
            if (lstat(cand, &st) == 0) {
                /* exact (or case-insensitive volume) hit; recover the stored case cheaply only
                 * when the exact name is absent */
                snprintf(real, sizeof real, "%s", comp);
            } else if (find_ci(cur, comp, real, sizeof real)) {
            } else {
                snprintf(real, sizeof real, "%s", comp);
                last_ok = 0;
            }
        } else {
            snprintf(real, sizeof real, "%s", comp);
        }
        size_t cl = strlen(cur), rl = strlen(real);
        if (cl + rl + 2 > sizeof cur) return -1;
        cur[cl] = '/';
        memcpy(cur + cl + 1, real, rl + 1);
        rest += k;
        while (*rest == '\\') rest++;
    }
    if (exists) *exists = last_ok;
    if (parent_exists) *parent_exists = parent_ok;
    if (strlen(cur) + 1 > cap) return -1;
    memcpy(host, cur, strlen(cur) + 1);
    return 0;
}

const char *w32_path_cwd(void) { return cwd; }

int w32_path_set_cwd(const char *win) {
    char full[W32_PATH_CAP], host[W32_PATH_CAP];
    int ex = 0;
    struct stat st;
    if (w32_path_full(win, full, sizeof full) < 0) return -1;
    strip_trailing_bs(full);
    if (w32_path_to_host(full, host, sizeof host, &ex, NULL) || !ex || stat(host, &st) || !S_ISDIR(st.st_mode))
        return -1;
    pthread_mutex_lock(&path_lock);
    snprintf(cwd, sizeof cwd, "%s", full);
    pthread_mutex_unlock(&path_lock);
    return 0;
}

const char *w32_folder(enum w32_folder f) { return (unsigned)f < W32_F_COUNT ? folders[f] : ""; }

uint32_t w32_arg_path(const uint8_t *mem, uint32_t a, int wide, char *full, size_t cap) {
    char raw[W32_PATH_CAP * 2];
    if (!a) return W32_ERROR_INVALID_PARAMETER;
    if (wide) w32_wstr_get(mem, a, raw, sizeof raw);
    else w32_astr_get(mem, a, raw, sizeof raw);
    if (!raw[0]) return W32_ERROR_PATH_NOT_FOUND;
    if (strlen(raw) >= W32_PATH_CAP) return W32_ERROR_FILENAME_EXCED_RANGE;
    if (w32_path_full(raw, full, cap) < 0) return W32_ERROR_BAD_PATHNAME;
    return 0;
}

/* ---- initialisation -------------------------------------------------------------------- */

static void mkdirs(const char *path) {
    char tmp[W32_PATH_CAP];
    snprintf(tmp, sizeof tmp, "%s", path);
    for (char *p = tmp + 1; *p; p++)
        if (*p == '/') { *p = 0; mkdir(tmp, 0755); *p = '/'; }
    mkdir(tmp, 0755);
}

static const char *env_or(const char *v, const char *env, const char *dflt, char *buf, size_t cap) {
    if (v) return v;
    const char *e = getenv(env);
    if (e && *e) return e;
    snprintf(buf, cap, "%s", dflt);
    return buf;
}

int w32_paths_init(const w32_config *cfg) {
    char sup_b[W32_PATH_CAP], game_b[W32_PATH_CAP], doc_b[W32_PATH_CAP], tmp_b[W32_PATH_CAP], dflt[W32_PATH_CAP];
    char hostcwd[W32_PATH_CAP], tmp[W32_PATH_CAP];
    const char *home = getenv("HOME");
    nmounts = 0;
    snprintf(cwd, sizeof cwd, "C:\\");
    snprintf(dflt, sizeof dflt, "%s/Library/Application Support/spore-native", home ? home : "/tmp");
    const char *support = env_or(cfg->support_dir, "SPORE_SUPPORT_DIR", dflt, sup_b, sizeof sup_b);
    if (!getcwd(hostcwd, sizeof hostcwd)) snprintf(hostcwd, sizeof hostcwd, ".");
    snprintf(dflt, sizeof dflt, "%s/game", hostcwd);
    const char *game = env_or(cfg->game_dir, "SPORE_GAME_DIR", dflt, game_b, sizeof game_b);
    snprintf(dflt, sizeof dflt, "%s/Documents", support);
    const char *docs = env_or(cfg->documents_dir, "SPORE_DOCUMENTS_DIR", dflt, doc_b, sizeof doc_b);
    const char *td = getenv("TMPDIR");
    snprintf(dflt, sizeof dflt, "%s/spore-native", td && *td ? td : "/tmp");
    const char *temp = env_or(cfg->temp_dir, "SPORE_TEMP_DIR", dflt, tmp_b, sizeof tmp_b);

    const char *install = cfg->install_win ? cfg->install_win : "C:\\Spore";
    const char *exe_rel = cfg->exe_rel ? cfg->exe_rel : "SporebinEP1\\SporeApp.exe";
    if (w32_path_full(install, folders[W32_F_INSTALL], W32_PATH_CAP) < 0) return -1;
    strip_trailing_bs(folders[W32_F_INSTALL]);
    snprintf(tmp, sizeof tmp, "%s\\%s", folders[W32_F_INSTALL], exe_rel);
    if (w32_path_full(tmp, folders[W32_F_EXE], W32_PATH_CAP) < 0) return -1;
    snprintf(folders[W32_F_EXE_DIR], W32_PATH_CAP, "%s", folders[W32_F_EXE]);
    *strrchr(folders[W32_F_EXE_DIR], '\\') = 0;
    snprintf(folders[W32_F_PROFILE], W32_PATH_CAP, "C:\\Users\\Player");
    snprintf(folders[W32_F_DOCUMENTS], W32_PATH_CAP, "C:\\Users\\Player\\Documents");
    snprintf(folders[W32_F_APPDATA], W32_PATH_CAP, "C:\\Users\\Player\\AppData\\Roaming");
    snprintf(folders[W32_F_LOCAL_APPDATA], W32_PATH_CAP, "C:\\Users\\Player\\AppData\\Local");
    snprintf(folders[W32_F_TEMP], W32_PATH_CAP, "C:\\Users\\Player\\AppData\\Local\\Temp");
    snprintf(folders[W32_F_COMMON_APPDATA], W32_PATH_CAP, "C:\\ProgramData");
    snprintf(folders[W32_F_COMMON_DOCUMENTS], W32_PATH_CAP, "C:\\Users\\Public\\Documents");
    snprintf(folders[W32_F_WINDOWS], W32_PATH_CAP, "C:\\Windows");
    snprintf(folders[W32_F_SYSTEM], W32_PATH_CAP, "C:\\Windows\\system32");

    char drive_c[W32_PATH_CAP], appdata[W32_PATH_CAP];
    snprintf(drive_c, sizeof drive_c, "%s/drive_c", support);
    snprintf(appdata, sizeof appdata, "%s/AppData", support);
    mkdirs(drive_c);
    mkdirs(appdata);
    mkdirs(docs);
    mkdirs(temp);
    w32_path_mount("C:\\", drive_c);
    w32_path_mount(folders[W32_F_APPDATA], appdata);
    w32_path_mount(folders[W32_F_DOCUMENTS], docs);
    w32_path_mount(folders[W32_F_TEMP], temp);
    w32_path_mount(folders[W32_F_INSTALL], game);
    /* the folders Windows always has, so existence checks on them succeed */
    for (int f = W32_F_DOCUMENTS; f < W32_F_COUNT; f++) { /* every folder except the install tree */
        char h[W32_PATH_CAP];
        if (!w32_path_to_host(folders[f], h, sizeof h, NULL, NULL)) mkdirs(h);
    }
    /* The working directory a GOG shortcut sets: the exe's folder (falls back to C:\). */
    if (w32_path_set_cwd(folders[W32_F_EXE_DIR])) snprintf(cwd, sizeof cwd, "%s", folders[W32_F_EXE_DIR]);
    hle_log(HLE_LOG_INFO, "paths: %s -> %s; AppData -> %s; Documents -> %s; temp -> %s; C:\\ -> %s",
            folders[W32_F_INSTALL], game, appdata, docs, temp, drive_c);
    return 0;
}
