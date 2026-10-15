/*
 * w32.h -- CRT start-up, guest heap and virtual memory, modules, environment, and the
 * file / registry / path layer. Spec: docs/design/win32-layer.md.
 *
 * Everything here runs on the host. Shims are sg_func's (spore_guest.h section 7) named
 * w32_imp_<dll stem>_<export>, because runtime/hle/gen already defines a stub with the
 * sg_imp_* name for every import; w32_register_shims() swaps our functions into
 * hle_imports[].fn (and the matching hle_dynamics[] entries) before hle_install_imports().
 *
 * Start-up order for a host program (image loader, boot driver):
 *     mem = <4 GiB window, PROT_NONE>;
 *     w32_init(mem, &cfg);           // VM bookkeeping, heaps, paths, registry, environment
 *     w32_register_shims();          // before hle_install_imports
 *     <map the image; w32_vm_mark_image(...) per section>
 *     hle_install_imports(mem);
 *     w32_after_install(mem);        // fills the _acmdln data import
 *     <run the entry point 0x11e11e1>
 * No game code or game bytes in this library.
 */
#ifndef SPORE_W32_H
#define SPORE_W32_H

#include <stddef.h>
#include <stdint.h>

#include "spore_guest.h"

#ifdef __cplusplus
extern "C" {
#endif

/* Our shims: the generated sg_imp_* stubs keep their names; these replace them in the table. */
#define W32_IMPORT(dll, name) void w32_imp_##dll##_##name(sg_ctx *SG_RESTRICT ctx, uint8_t *mem)

/* ------------------------------------------------------------------------------------------ */
/* Configuration and process start-up                                                         */
/* ------------------------------------------------------------------------------------------ */

typedef struct w32_config {
    /* Host directories. NULL = default (environment variable in brackets, then the default). */
    const char *game_dir;      /* [SPORE_GAME_DIR] host folder holding SporebinEP1/, Data/, DataEP1/...
                                * default: ./game relative to the host working directory */
    const char *support_dir;   /* [SPORE_SUPPORT_DIR] ~/Library/Application Support/spore-native */
    const char *documents_dir; /* [SPORE_DOCUMENTS_DIR] ~/Documents */
    const char *temp_dir;      /* [SPORE_TEMP_DIR] $TMPDIR/spore-native */
    const char *registry_file; /* [SPORE_REGISTRY] <support_dir>/registry.json */
    /* Guest view. */
    const char *install_win;   /* Windows path of {app}; default "C:\\Spore" */
    const char *exe_rel;       /* exe path below {app}; default "SporebinEP1\\SporeApp.exe" */
    const char *args;          /* command-line arguments after the exe (UTF-8); [SPORE_ARGS] */
    int persist_registry;      /* write registry changes back to the JSON (default 0: in memory) */
    int std_handles;           /* [SPORE_W32_STDHANDLES] 1: GetStdHandle maps to host fds 0..2;
                                * 0 (default): NULL, like a GUI process started from Explorer */
} w32_config;

/* Initialises the whole layer. `mem` is the reserved guest window (PROT_NONE). Returns 0 on
 * success. Must run before anything calls hle_guest_alloc (hle_install_imports does). */
int w32_init(uint8_t *mem, const w32_config *cfg);
/* Swaps every w32 shim into hle_imports[] / hle_dynamics[]. Returns the number of import
 * slots replaced; logs (and counts in *mismatches) any shim whose stack clean-up disagrees
 * with the table. Call before hle_install_imports. */
unsigned w32_register_shims(unsigned *mismatches);
/* After hle_install_imports: points the _acmdln data import at the guest command line. */
void w32_after_install(uint8_t *mem);

extern uint8_t *w32_mem; /* the window base given to w32_init */

/* Process exit hook used by exit/_exit/ExitProcess-like paths. Default: host exit(code).
 * The boot driver may replace it (for example to longjmp back to itself). */
extern void (*w32_exit_hook)(int code);

/* Progress of _initterm / _initterm_e (the boot driver counts static constructors). */
extern volatile uint32_t w32_initterm_done;    /* function pointers called so far */
extern volatile uint32_t w32_initterm_current; /* guest address of the one running now */
/* Optional observer, called before each initializer: (index, guest fn). */
extern void (*w32_initterm_hook)(uint32_t index, uint32_t fn);

/* ------------------------------------------------------------------------------------------ */
/* Guest virtual memory (VirtualAlloc family; docs/design/win32-layer.md section 3)            */
/* ------------------------------------------------------------------------------------------ */

enum {
    W32_PAGE = 0x1000u,         /* guest page (Windows x86) */
    W32_GRANULE = 0x10000u,     /* allocation granularity */
    W32_VM_LO = 0x00010000u,    /* lowest address VirtualAlloc hands out */
    W32_VM_HI = SG_THUNK_BASE,  /* first address it never hands out (runtime regions above) */
};
/* Windows constants (winnt.h). */
enum {
    W32_MEM_COMMIT = 0x1000, W32_MEM_RESERVE = 0x2000, W32_MEM_DECOMMIT = 0x4000,
    W32_MEM_RELEASE = 0x8000, W32_MEM_FREE = 0x10000, W32_MEM_PRIVATE = 0x20000,
    W32_MEM_MAPPED = 0x40000, W32_MEM_RESET = 0x80000, W32_MEM_TOP_DOWN = 0x100000,
    W32_MEM_IMAGE = 0x1000000,
    W32_PAGE_NOACCESS = 0x01, W32_PAGE_READONLY = 0x02, W32_PAGE_READWRITE = 0x04,
    W32_PAGE_WRITECOPY = 0x08, W32_PAGE_EXECUTE = 0x10, W32_PAGE_EXECUTE_READ = 0x20,
    W32_PAGE_EXECUTE_READWRITE = 0x40, W32_PAGE_EXECUTE_WRITECOPY = 0x80, W32_PAGE_GUARD = 0x100,
};

typedef struct w32_mbi { /* MEMORY_BASIC_INFORMATION, 28 bytes on x86 */
    uint32_t base, alloc_base, alloc_protect, region_size, state, protect, type;
} w32_mbi;

/* VirtualAlloc/VirtualFree/VirtualProtect/VirtualQuery semantics on guest addresses. Each
 * returns 0 / the Win32 failure value and stores the Win32 error code in *err (may be NULL). */
uint32_t w32_vm_alloc(uint32_t addr, uint32_t size, uint32_t type, uint32_t protect, uint32_t *err);
int w32_vm_free(uint32_t addr, uint32_t size, uint32_t type, uint32_t *err);
int w32_vm_protect(uint32_t addr, uint32_t size, uint32_t protect, uint32_t *old, uint32_t *err);
int w32_vm_query(uint32_t addr, w32_mbi *out);
/* Runtime use (core stacks and TEBs, the loaded image, COM records): reserve+commit at a fixed
 * address, including the runtime regions at/above SG_THUNK_BASE that guest VirtualAlloc never
 * hands out. Returns addr or 0. */
uint32_t w32_vm_alloc_runtime(uint32_t addr, uint32_t size, uint32_t protect);
/* Records an image section the loader mapped itself (no host mprotect is done here): pages
 * become MEM_IMAGE, committed, with `protect`. The whole image is pre-registered as
 * MEM_IMAGE/PAGE_READWRITE by w32_init so it is never handed out. */
void w32_vm_mark_image(uint32_t addr, uint32_t size, uint32_t protect);
/* Statistics for tests and the boot log. */
typedef struct w32_vm_stats { uint32_t reserved_bytes, committed_bytes, allocations; } w32_vm_stats;
void w32_vm_get_stats(w32_vm_stats *s);

/* ------------------------------------------------------------------------------------------ */
/* Heaps (HeapAlloc family, GlobalAlloc/LocalAlloc, msvcr90 malloc family)                     */
/* ------------------------------------------------------------------------------------------ */

enum {
    W32_HEAP_NO_SERIALIZE = 0x1, W32_HEAP_GENERATE_EXCEPTIONS = 0x4, W32_HEAP_ZERO_MEMORY = 0x8,
    W32_HEAP_REALLOC_IN_PLACE_ONLY = 0x10,
};
/* Heap handles are guest addresses of a small control block (as on Windows, where the
 * handle is the heap's base address). */
uint32_t w32_heap_create(uint32_t flags, uint32_t initial, uint32_t maximum);
int w32_heap_destroy(uint32_t heap);
uint32_t w32_heap_alloc(uint32_t heap, uint32_t flags, uint32_t size);
int w32_heap_free(uint32_t heap, uint32_t flags, uint32_t p);
uint32_t w32_heap_realloc(uint32_t heap, uint32_t flags, uint32_t p, uint32_t size);
uint32_t w32_heap_size(uint32_t heap, uint32_t flags, uint32_t p); /* 0xFFFFFFFF on error */
int w32_heap_validate_ptr(uint32_t heap, uint32_t p);             /* 1 if p is a live block */
uint32_t w32_process_heap(void); /* GetProcessHeap */
uint32_t w32_crt_heap(void);     /* msvcr90's private _crtheap: malloc & co. */
/* msvcr90 semantics: malloc(0) gives a unique block; realloc(p, 0) frees and returns 0. */
uint32_t w32_malloc(uint32_t size);
void w32_free(uint32_t p);
uint32_t w32_realloc(uint32_t p, uint32_t size);
uint32_t w32_calloc(uint32_t n, uint32_t size);
uint32_t w32_msize(uint32_t p);

/* ------------------------------------------------------------------------------------------ */
/* Handles (files, find handles, registry keys use their own table; see the design doc)        */
/* ------------------------------------------------------------------------------------------ */

enum { W32_INVALID_HANDLE = 0xffffffffu };
enum w32_htype { W32_H_FREE = 0, W32_H_FILE, W32_H_FIND, W32_H_STD, W32_H_DIRWATCH, W32_H_FOREIGN };
/* File-layer handles are 0x00A00004, 0x00A00008, ... (multiples of 4, like NT handles), in a
 * range other packages should not use. CloseHandle on a value outside our table calls
 * w32_close_foreign (default: log once and return TRUE) so the core's kernel objects can
 * share the import; the core and the boot driver set the hook. */
#define W32_HANDLE_BASE 0x00a00000u
extern int (*w32_close_foreign)(sg_ctx *ctx, uint32_t handle);
int w32_handle_is_ours(uint32_t h);
/* Closes a handle of this layer only (files, find, std): 1, or 0 with ERROR_INVALID_HANDLE when
 * it is not ours. For the core's sg_core_close_hook; it never calls w32_close_foreign. */
int w32_close_own_handle(sg_ctx *ctx, uint32_t handle);

/* ------------------------------------------------------------------------------------------ */
/* Paths (Windows -> host), section 5 of the design doc                                        */
/* ------------------------------------------------------------------------------------------ */

enum { W32_MAX_PATH = 260, W32_PATH_CAP = 1024 };

/* GetFullPathName rules on a UTF-8 Windows path: separators to '\', relative paths against
 * the current directory, '.'/'..' folded, trailing dots/spaces dropped from the last
 * component, "\\?\" stripped. Returns the length, or -1 for paths we cannot represent (UNC,
 * device paths). Keeps a trailing '\' if the input had one. */
int w32_path_full(const char *win, char *out, size_t cap);
/* Maps a full Windows path to a host path through the mount table, resolving every existing
 * component case-insensitively. Returns 0 if the mapping exists; *exists = 1 if the final
 * object exists, *parent_exists = 1 if its directory exists. -1 if no mount covers it. */
int w32_path_to_host(const char *win_full, char *host, size_t cap, int *exists, int *parent_exists);
/* Adds or replaces a mount: Windows prefix (e.g. "C:\\Spore") -> host directory. */
int w32_path_mount(const char *win_prefix, const char *host_dir);
/* Current directory (Windows form, no trailing '\' except at a drive root). */
const char *w32_path_cwd(void);
int w32_path_set_cwd(const char *win);
/* Well-known guest folders (UTF-8 Windows paths). */
enum w32_folder { W32_F_INSTALL, W32_F_EXE, W32_F_EXE_DIR, W32_F_DOCUMENTS, W32_F_APPDATA,
                  W32_F_LOCAL_APPDATA, W32_F_COMMON_APPDATA, W32_F_COMMON_DOCUMENTS, W32_F_TEMP,
                  W32_F_WINDOWS, W32_F_SYSTEM, W32_F_PROFILE, W32_F_COUNT };
const char *w32_folder(enum w32_folder f);
/* Case-insensitive Windows wildcard match ('*', '?'; "*.*" matches everything). */
int w32_wildcard_match(const char *pattern, const char *name);
/* Case folding used for names: ASCII and Latin-1 letters (UTF-8 in, comparison only). */
int w32_name_casecmp(const char *a, const char *b);

/* ------------------------------------------------------------------------------------------ */
/* Registry (section 6)                                                                        */
/* ------------------------------------------------------------------------------------------ */

enum { W32_HKCR = 0x80000000u, W32_HKCU = 0x80000001u, W32_HKLM = 0x80000002u, W32_HKU = 0x80000003u };
enum { W32_REG_NONE = 0, W32_REG_SZ = 1, W32_REG_EXPAND_SZ = 2, W32_REG_BINARY = 3, W32_REG_DWORD = 4,
       W32_REG_MULTI_SZ = 7, W32_REG_QWORD = 11 };
/* Loads the JSON store (creating it from the GOG seed when absent). */
int w32_reg_load(const char *json_path, int persist);
/* Writes the current tree to the store file. */
int w32_reg_save(void);
/* Host-level API used by the shims and the tests. Keys are UTF-8, case-insensitive.
 * Values are kept as REG_SZ (UTF-8 text) or REG_DWORD / REG_BINARY bytes. */
int w32_reg_open(uint32_t parent, const char *subkey, int create, uint32_t *out); /* Win32 error */
int w32_reg_close(uint32_t hkey);
int w32_reg_query(uint32_t hkey, const char *name, uint32_t *type, uint8_t *data, uint32_t *len,
                  int wide); /* data in the guest encoding: UTF-16 when wide, 1252 otherwise */
int w32_reg_set(uint32_t hkey, const char *name, uint32_t type, const uint8_t *data, uint32_t len, int wide);
int w32_reg_enum_key(uint32_t hkey, uint32_t index, char *name, size_t cap); /* Win32 error */
/* The seed: what the GOG installer writes (its goggame-1948823323.script),
 * with {app} replaced by `install_win`. Returned as JSON text (caller frees). */
char *w32_reg_seed_json(const char *install_win);

/* ------------------------------------------------------------------------------------------ */
/* Strings shared by the shims                                                                 */
/* ------------------------------------------------------------------------------------------ */

/* Guest UTF-16 / ANSI (code page 1252) strings to host UTF-8 and back. The readers return the
 * UTF-8 length (truncated to cap-1) and always terminate. */
size_t w32_wstr_get(const uint8_t *mem, uint32_t a, char *out, size_t cap);
size_t w32_astr_get(const uint8_t *mem, uint32_t a, char *out, size_t cap);
/* UTF-8 -> guest units (UTF-16 when wide, code page 1252 otherwise). w32_utf8_units returns the
 * units the string needs WITHOUT the NUL. w32_put_str writes s + NUL into the guest buffer
 * `dst` of `cap` units when it fits (*fits = 1, returns the units written without the NUL);
 * otherwise it writes nothing (*fits = 0) and returns the units needed INCLUDING the NUL -- the
 * contract of GetCurrentDirectory, GetTempPath, GetFullPathName and similar APIs. */
uint32_t w32_utf8_units(const char *s, int wide);
uint32_t w32_put_str(uint8_t *mem, uint32_t dst, uint32_t cap, const char *s, int wide, int *fits);

/* Environment (GetEnvironmentVariableA / SetEnvironmentVariableA, __getmainargs envp). */
const char *w32_env_get(const char *name);       /* case-insensitive name; NULL if unset */
int w32_env_set(const char *name, const char *value); /* value NULL removes */
const char *w32_command_line(void);              /* full command line, UTF-8 */

#ifdef __cplusplus
}
#endif
#endif /* SPORE_W32_H */
