/* w32_module.c -- modules (GetModuleHandle, GetModuleFileName, LoadLibrary, GetProcAddress)
 * against runtime/hle's name tables, the command line, the environment, and process start-up
 * information (design doc section 7).
 *
 * Module handles: the exe is 0x00400000 (its own mapped headers). Every other module the
 * guest may load gets a 64 KiB reservation whose first page holds a minimal PE header ("MZ",
 * e_lfanew, "PE\0\0", i386, no exports), so handle values look and behave like image bases.
 * A module "exists" if runtime/hle knows it (hle_module_known: a static-import DLL or one with a
 * provided dynamic name) or it is ntdll, which every Windows process has. Everything else
 * fails with ERROR_MOD_NOT_FOUND, as on a GOG install without that component.
 */
#include <ctype.h>
#include <pthread.h>
#include <stdio.h>
#include <stdlib.h>
#include <string.h>
#include <strings.h>
#include <unistd.h>

#include "w32_int.h"

typedef struct module { char stem[64]; uint32_t handle; int refs; } module;
#define MAX_MODULES 64
static module modules[MAX_MODULES];
static int nmodules;
static pthread_mutex_t mod_lock = PTHREAD_MUTEX_INITIALIZER;

static char cmdline[8192];
static uint32_t cmdline_a, cmdline_w; /* guest copies, made on first use */

/* ---- environment store ---------------------------------------------------------------- */

typedef struct envvar { char *name, *value; } envvar;
static envvar *envs;
static size_t nenv;
static pthread_mutex_t env_lock = PTHREAD_MUTEX_INITIALIZER;

static char *dupstr(const char *s) { size_t n = strlen(s) + 1; char *d = (char *)malloc(n); memcpy(d, s, n); return d; }

const char *w32_env_get(const char *name) {
    const char *r = NULL;
    pthread_mutex_lock(&env_lock);
    for (size_t i = 0; i < nenv; i++)
        if (!strcasecmp(envs[i].name, name)) { r = envs[i].value; break; }
    pthread_mutex_unlock(&env_lock);
    return r;
}

int w32_env_set(const char *name, const char *value) {
    if (!name || !*name || strchr(name, '=')) return -1;
    pthread_mutex_lock(&env_lock);
    size_t i;
    for (i = 0; i < nenv; i++)
        if (!strcasecmp(envs[i].name, name)) break;
    if (!value) {
        if (i < nenv) {
            free(envs[i].name); free(envs[i].value);
            memmove(envs + i, envs + i + 1, (nenv - i - 1) * sizeof *envs);
            nenv--;
        }
    } else if (i < nenv) {
        free(envs[i].value);
        envs[i].value = dupstr(value);
    } else {
        /* keep the block sorted case-insensitively, as Windows does */
        size_t at = 0;
        while (at < nenv && strcasecmp(envs[at].name, name) < 0) at++;
        envs = (envvar *)realloc(envs, (nenv + 1) * sizeof *envs);
        memmove(envs + at + 1, envs + at, (nenv - at) * sizeof *envs);
        envs[at].name = dupstr(name);
        envs[at].value = dupstr(value);
        nenv++;
    }
    pthread_mutex_unlock(&env_lock);
    return 0;
}

size_t w32_env_block(char *out, size_t cap, unsigned *count) {
    size_t n = 0;
    pthread_mutex_lock(&env_lock);
    for (size_t i = 0; i < nenv; i++) {
        size_t a = strlen(envs[i].name), b = strlen(envs[i].value);
        if (out && n + a + b + 2 <= cap) {
            memcpy(out + n, envs[i].name, a);
            out[n + a] = '=';
            memcpy(out + n + a + 1, envs[i].value, b + 1);
        }
        n += a + b + 2;
    }
    if (count) *count = (unsigned)nenv;
    pthread_mutex_unlock(&env_lock);
    return n;
}

static unsigned ncpu(void) {
    const char *e = getenv("SPORE_NCPU");
    long n = e ? atol(e) : sysconf(_SC_NPROCESSORS_ONLN);
    if (n < 1) n = 1;
    if (n > 16) n = 16; /* keep the job manager's thread count sane; overridable */
    return (unsigned)n;
}

void w32_env_init(const w32_config *cfg) {
    char t[W32_PATH_CAP];
    pthread_mutex_lock(&env_lock);
    for (size_t i = 0; i < nenv; i++) { free(envs[i].name); free(envs[i].value); }
    free(envs);
    envs = NULL;
    nenv = 0;
    pthread_mutex_unlock(&env_lock);
    /* a plausible Windows 7 user environment */
    w32_env_set("ALLUSERSPROFILE", w32_folder(W32_F_COMMON_APPDATA));
    w32_env_set("APPDATA", w32_folder(W32_F_APPDATA));
    w32_env_set("COMPUTERNAME", "SPORE-MAC");
    w32_env_set("HOMEDRIVE", "C:");
    w32_env_set("HOMEPATH", w32_folder(W32_F_PROFILE) + 2);
    w32_env_set("LOCALAPPDATA", w32_folder(W32_F_LOCAL_APPDATA));
    snprintf(t, sizeof t, "%u", ncpu());
    w32_env_set("NUMBER_OF_PROCESSORS", t);
    w32_env_set("OS", "Windows_NT");
    snprintf(t, sizeof t, "%s;%s", w32_folder(W32_F_SYSTEM), w32_folder(W32_F_WINDOWS));
    w32_env_set("PATH", t);
    w32_env_set("PATHEXT", ".COM;.EXE;.BAT;.CMD");
    w32_env_set("PROCESSOR_ARCHITECTURE", "x86");
    w32_env_set("PROGRAMDATA", w32_folder(W32_F_COMMON_APPDATA));
    w32_env_set("ProgramFiles", "C:\\Program Files");
    w32_env_set("PUBLIC", "C:\\Users\\Public");
    w32_env_set("SystemDrive", "C:");
    w32_env_set("SystemRoot", w32_folder(W32_F_WINDOWS));
    w32_env_set("TEMP", w32_folder(W32_F_TEMP));
    w32_env_set("TMP", w32_folder(W32_F_TEMP));
    w32_env_set("USERNAME", "Player");
    w32_env_set("USERPROFILE", w32_folder(W32_F_PROFILE));
    w32_env_set("windir", w32_folder(W32_F_WINDOWS));
    /* host variables SPORE_ENV_<NAME>=value are passed through as <NAME> (testing aid) */
    extern char **environ;
    for (char **e = environ; e && *e; e++) {
        if (strncmp(*e, "SPORE_ENV_", 10)) continue;
        const char *eq = strchr(*e, '=');
        if (!eq || eq == *e + 10) continue;
        char name[256];
        size_t n = (size_t)(eq - (*e + 10));
        if (n >= sizeof name) continue;
        memcpy(name, *e + 10, n);
        name[n] = 0;
        w32_env_set(name, eq + 1);
    }
    /* command line: "<exe path>" args */
    const char *args = cfg->args ? cfg->args : getenv("SPORE_ARGS");
    snprintf(cmdline, sizeof cmdline, "\"%s\"%s%s", w32_folder(W32_F_EXE), args && *args ? " " : "", args ? args : "");
    cmdline_a = cmdline_w = 0;
    /* modules */
    pthread_mutex_lock(&mod_lock);
    nmodules = 0;
    pthread_mutex_unlock(&mod_lock);
}

const char *w32_command_line(void) { return cmdline; }

static uint32_t guest_cmdline(uint8_t *mem, int wide) {
    uint32_t *slot = wide ? &cmdline_w : &cmdline_a;
    if (!*slot) {
        uint32_t units = w32_utf8_units(cmdline, wide) + 1;
        uint32_t g = hle_guest_alloc(units * (wide ? 2u : 1u), 4);
        w32_put_str_trunc(mem, g, units, cmdline, wide);
        *slot = g;
    }
    return *slot;
}

void w32_crt_after_install(uint8_t *mem) {
    for (unsigned i = 0; i < hle_import_count; i++)
        if (hle_imports[i].cc == HLE_CC_DATA && hle_imports[i].name && !strcmp(hle_imports[i].name, "_acmdln")) {
            sg_st32(mem, hle_imports[i].guest, guest_cmdline(mem, 0)); /* char *_acmdln */
            return;
        }
}

/* ---- modules ------------------------------------------------------------------------- */

static void stem_of(const char *name, char *out, size_t cap) {
    const char *b = name, *p;
    for (p = name; *p; p++)
        if (*p == '\\' || *p == '/') b = p + 1;
    size_t n = strlen(b);
    if (n > 4 && !strcasecmp(b + n - 4, ".dll")) n -= 4;
    else if (n > 4 && !strcasecmp(b + n - 4, ".exe")) n -= 4;
    if (n >= cap) n = cap - 1;
    for (size_t i = 0; i < n; i++) out[i] = (char)tolower((unsigned char)b[i]);
    out[n] = 0;
}

static int is_exe(const char *stem) {
    char e[64];
    stem_of(w32_folder(W32_F_EXE), e, sizeof e);
    return !strcmp(stem, e);
}

static uint32_t make_module_image(const char *stem) {
    uint32_t err, base = w32_vm_alloc(0, W32_PAGE, W32_MEM_RESERVE | W32_MEM_COMMIT, W32_PAGE_READWRITE, &err);
    uint8_t *m = w32_mem;
    if (!base) return 0;
    sg_st16(m, base, 0x5a4d);                      /* "MZ" */
    sg_st32(m, base + 0x3c, 0x40);                 /* e_lfanew */
    sg_st32(m, base + 0x40, 0x00004550);           /* "PE\0\0" */
    sg_st16(m, base + 0x44, 0x014c);               /* i386 */
    sg_st16(m, base + 0x54, 0xe0);                 /* SizeOfOptionalHeader */
    sg_st16(m, base + 0x56, 0x2102);               /* DLL | 32-bit | executable */
    sg_st16(m, base + 0x58, 0x010b);               /* PE32 magic */
    sg_st32(m, base + 0x74, base);                 /* ImageBase */
    sg_st32(m, base + 0x90, 0x10000);              /* SizeOfImage */
    snprintf((char *)m + base + 0x200, 64, "%s.dll", stem); /* for debugging dumps */
    uint32_t old;
    w32_vm_protect(base, W32_PAGE, W32_PAGE_READONLY, &old, &err);
    return base;
}

/* Returns the handle for a module name, loading it if `load`. 0 if it does not exist. */
static uint32_t module_handle(const char *name, int load) {
    char stem[64];
    stem_of(name, stem, sizeof stem);
    if (is_exe(stem)) return SG_IMAGE_BASE;
    pthread_mutex_lock(&mod_lock);
    for (int i = 0; i < nmodules; i++)
        if (!strcmp(modules[i].stem, stem)) {
            if (load) modules[i].refs++;
            pthread_mutex_unlock(&mod_lock);
            return modules[i].handle;
        }
    uint32_t h = 0;
    int exists = !strcmp(stem, "ntdll") || hle_module_known(stem);
    /* GetModuleHandle finds only loaded modules; the static-import DLLs and ntdll are loaded
     * at start-up, dynamically loaded ones only after LoadLibrary. */
    int preloaded = !strcmp(stem, "ntdll");
    for (unsigned i = 0; !preloaded && i < hle_import_count; i++) {
        char s2[64];
        stem_of(hle_imports[i].dll, s2, sizeof s2);
        if (!strcmp(s2, stem)) preloaded = 1;
    }
    if (exists && (load || preloaded) && nmodules < MAX_MODULES && (h = make_module_image(stem))) {
        snprintf(modules[nmodules].stem, sizeof modules[nmodules].stem, "%s", stem);
        modules[nmodules].handle = h;
        modules[nmodules].refs = 1;
        nmodules++;
    }
    pthread_mutex_unlock(&mod_lock);
    return h;
}

static const char *module_stem(uint32_t h) {
    for (int i = 0; i < nmodules; i++)
        if (modules[i].handle == h) return modules[i].stem;
    return NULL;
}

static uint32_t get_handle(sg_ctx *ctx, uint8_t *mem, uint32_t name, int wide, int load) {
    char n[W32_PATH_CAP];
    if (!name) return load ? 0 : SG_IMAGE_BASE;
    if (wide) w32_wstr_get(mem, name, n, sizeof n);
    else w32_astr_get(mem, name, n, sizeof n);
    uint32_t h = module_handle(n, load);
    hle_log(HLE_LOG_INFO, "%s(\"%s\") = 0x%08x", load ? "LoadLibrary" : "GetModuleHandle", n, h);
    if (!h) hle_set_last_error(ctx, mem, W32_ERROR_MOD_NOT_FOUND);
    return h;
}

W32_IMPORT(kernel32, GetModuleHandleA) { SG_RETURN_STDCALL(get_handle(ctx, mem, SG_ARG(0), 0, 0), 4); }
W32_IMPORT(kernel32, GetModuleHandleW) { SG_RETURN_STDCALL(get_handle(ctx, mem, SG_ARG(0), 1, 0), 4); }
W32_IMPORT(kernel32, LoadLibraryA) { SG_RETURN_STDCALL(get_handle(ctx, mem, SG_ARG(0), 0, 1), 4); }
W32_IMPORT(kernel32, LoadLibraryW) { SG_RETURN_STDCALL(get_handle(ctx, mem, SG_ARG(0), 1, 1), 4); }
W32_IMPORT(kernel32, FreeLibrary) {
    uint32_t h = SG_ARG(0);
    int ok = h == SG_IMAGE_BASE || module_stem(h) != NULL;
    if (!ok) hle_set_last_error(ctx, mem, W32_ERROR_INVALID_HANDLE);
    SG_RETURN_STDCALL(ok, 4); /* module images stay mapped: handles remain stable */
}

W32_IMPORT(kernel32, GetProcAddress) {
    uint32_t h = SG_ARG(0), np = SG_ARG(1), r = 0;
    char name[256];
    const char *stem = h == SG_IMAGE_BASE || !h ? NULL : module_stem(h);
    if (np < 0x10000) {
        if (stem) r = hle_resolve(stem, NULL, np);
        hle_log(HLE_LOG_INFO, "GetProcAddress(%s, #%u) = 0x%08x", stem ? stem : "?", np, r);
    } else {
        w32_astr_get(mem, np, name, sizeof name);
        if (stem) r = hle_resolve(stem, name, 0);
        hle_log(HLE_LOG_INFO, "GetProcAddress(%s, %s) = 0x%08x", stem ? stem : "?", name, r);
    }
    if (!r) hle_set_last_error(ctx, mem, stem || h == SG_IMAGE_BASE ? W32_ERROR_PROC_NOT_FOUND : W32_ERROR_MOD_NOT_FOUND);
    SG_RETURN_STDCALL(r, 8);
}

/* GetModuleFileName (Vista+ rules): copies up to nSize-1 characters and a NUL; when the
 * name does not fit it is truncated, the result is nSize and the error INSUFFICIENT_BUFFER. */
static uint32_t module_file_name(sg_ctx *ctx, uint8_t *mem, int wide) {
    uint32_t h = SG_ARG(0), buf = SG_ARG(1), size = SG_ARG(2);
    char path[W32_PATH_CAP];
    const char *stem;
    if (!h || h == SG_IMAGE_BASE) {
        snprintf(path, sizeof path, "%s", w32_folder(W32_F_EXE));
    } else if ((stem = module_stem(h))) {
        snprintf(path, sizeof path, "%s\\%s.dll", w32_folder(W32_F_SYSTEM), stem);
    } else {
        hle_set_last_error(ctx, mem, W32_ERROR_MOD_NOT_FOUND);
        return 0;
    }
    if (!size) { hle_set_last_error(ctx, mem, W32_ERROR_INSUFFICIENT_BUFFER); return 0; }
    uint32_t need = w32_utf8_units(path, wide);
    uint32_t n = w32_put_str_trunc(mem, buf, size, path, wide);
    if (need + 1 > size) { hle_set_last_error(ctx, mem, W32_ERROR_INSUFFICIENT_BUFFER); return size; }
    hle_set_last_error(ctx, mem, 0);
    return n;
}
W32_IMPORT(kernel32, GetModuleFileNameA) { SG_RETURN_STDCALL(module_file_name(ctx, mem, 0), 12); }
W32_IMPORT(kernel32, GetModuleFileNameW) { SG_RETURN_STDCALL(module_file_name(ctx, mem, 1), 12); }

/* ---- command line and environment ------------------------------------------------------ */

W32_IMPORT(kernel32, GetCommandLineA) { SG_RETURN_STDCALL(guest_cmdline(mem, 0), 0); }
W32_IMPORT(kernel32, GetCommandLineW) { SG_RETURN_STDCALL(guest_cmdline(mem, 1), 0); }

W32_IMPORT(kernel32, GetEnvironmentVariableA) {
    /* (lpName, lpBuffer, nSize): length without NUL on success; needed size WITH the NUL
     * when the buffer is too small; 0 + ERROR_ENVVAR_NOT_FOUND when unset. */
    char name[512];
    uint32_t buf = SG_ARG(1), size = SG_ARG(2);
    w32_astr_get(mem, SG_ARG(0), name, sizeof name);
    const char *v = w32_env_get(name);
    if (!v) { hle_set_last_error(ctx, mem, W32_ERROR_ENVVAR_NOT_FOUND); SG_RETURN_STDCALL(0, 12); }
    uint32_t need = w32_utf8_units(v, 0);
    if (need + 1 > size || !buf) SG_RETURN_STDCALL(need + 1, 12);
    w32_put_str_trunc(mem, buf, size, v, 0);
    SG_RETURN_STDCALL(need, 12);
}

W32_IMPORT(kernel32, SetEnvironmentVariableA) {
    char name[512], value[4096];
    uint32_t pv = SG_ARG(1);
    w32_astr_get(mem, SG_ARG(0), name, sizeof name);
    if (pv) w32_astr_get(mem, pv, value, sizeof value);
    int r = w32_env_set(name, pv ? value : NULL);
    if (r) hle_set_last_error(ctx, mem, W32_ERROR_INVALID_PARAMETER);
    SG_RETURN_STDCALL(!r, 8);
}

/* ---- start-up and system information ------------------------------------------------- */

W32_IMPORT(kernel32, GetStartupInfoA) {
    uint32_t p = SG_ARG(0);
    memset(mem + p, 0, 68);
    sg_st32(mem, p, 68); /* cb; dwFlags 0: WinMain gets SW_SHOWDEFAULT */
    SG_RET(4);
}

W32_IMPORT(kernel32, GetSystemInfo) {
    uint32_t p = SG_ARG(0);
    unsigned n = ncpu();
    memset(mem + p, 0, 36);
    sg_st16(mem, p, 0);                 /* PROCESSOR_ARCHITECTURE_INTEL */
    sg_st32(mem, p + 4, W32_PAGE);      /* dwPageSize */
    sg_st32(mem, p + 8, W32_VM_LO);     /* lpMinimumApplicationAddress */
    sg_st32(mem, p + 12, 0x7ffeffffu);  /* lpMaximumApplicationAddress */
    sg_st32(mem, p + 16, n >= 32 ? 0xffffffffu : (1u << n) - 1u);
    sg_st32(mem, p + 20, n);
    sg_st32(mem, p + 24, 586);          /* PROCESSOR_INTEL_PENTIUM */
    sg_st32(mem, p + 28, W32_GRANULE);
    sg_st16(mem, p + 32, 6);            /* wProcessorLevel */
    sg_st16(mem, p + 34, 0x1706);       /* wProcessorRevision */
    SG_RET(4);
}

/* Windows 7 SP1 (6.1.7601), the newest version contemporary with the GOG re-release that the
 * game's version checks know. */
#define WINVER_MAJOR 6
#define WINVER_MINOR 1
#define WINVER_BUILD 7601
W32_IMPORT(kernel32, GetVersion) {
    SG_RETURN_STDCALL((WINVER_BUILD << 16) | (WINVER_MINOR << 8) | WINVER_MAJOR, 0);
}
static int version_ex(uint8_t *mem, uint32_t p, int wide) {
    uint32_t size = sg_ld32(mem, p);
    uint32_t base = wide ? 276u : 148u, ex = wide ? 284u : 156u;
    if (size != base && size != ex) return 0;
    memset(mem + p + 4, 0, size - 4);
    sg_st32(mem, p + 4, WINVER_MAJOR);
    sg_st32(mem, p + 8, WINVER_MINOR);
    sg_st32(mem, p + 12, WINVER_BUILD);
    sg_st32(mem, p + 16, 2); /* VER_PLATFORM_WIN32_NT */
    w32_put_str_trunc(mem, p + 20, 128, "Service Pack 1", wide);
    if (size == ex) {
        uint32_t q = p + 20 + (wide ? 256u : 128u);
        sg_st16(mem, q, 1);     /* wServicePackMajor */
        sg_st16(mem, q + 2, 0); /* wServicePackMinor */
        sg_st16(mem, q + 4, 0x100); /* wSuiteMask: VER_SUITE_SINGLEUSERTS */
        mem[q + 6] = 1;         /* wProductType: VER_NT_WORKSTATION */
    }
    return 1;
}
W32_IMPORT(kernel32, GetVersionExA) {
    int ok = version_ex(mem, SG_ARG(0), 0);
    if (!ok) hle_set_last_error(ctx, mem, W32_ERROR_INSUFFICIENT_BUFFER);
    SG_RETURN_STDCALL(ok, 4);
}
W32_IMPORT(kernel32, GetVersionExW) {
    int ok = version_ex(mem, SG_ARG(0), 1);
    if (!ok) hle_set_last_error(ctx, mem, W32_ERROR_INSUFFICIENT_BUFFER);
    SG_RETURN_STDCALL(ok, 4);
}
W32_IMPORT(kernel32, IsDebuggerPresent) { SG_RETURN_STDCALL(0, 0); }
