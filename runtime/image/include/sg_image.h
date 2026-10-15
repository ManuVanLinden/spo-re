/*
 * sg_image.h -- image loader, dispatch support and process start-up.
 * Design: docs/design/image-linker.md. ABI: runtime/include/spore_guest.h, docs/design/runtime-abi.md.
 *
 * What lives here:
 *   - the 4 GiB guest window (reserve, fault report);
 *   - the PE loader: maps SporeApp.exe's sections at 0x400000 inside the window, either read from the
 *     exe file at run time or from a copy embedded in the binary, then applies page protections;
 *   - the thunk region and sg_thunk_register (weak: runtime/core may provide its own);
 *   - the dispatch miss hook (logs the address and the caller) and the resume-point lookup;
 *   - the import override registry other runtime packages use to replace runtime/hle's generated stubs;
 *   - main() of build/sporenative/spore (src/main.c).
 * No game code and no game bytes are in this directory. Generated tables live under build/.
 */
#ifndef SG_IMAGE_H
#define SG_IMAGE_H

#include <stddef.h>
#include <stdint.h>

#include "spore_guest.h"

#ifdef __cplusplus
extern "C" {
#endif

/* Host pages are 16 KiB on arm64 macOS; Win32's 4 KiB protection granularity cannot be reproduced
 * (image-linker.md 9.1). The image therefore occupies whole host pages up to SG_IMAGE_MAPPED_END:
 * the guest heap must start at or above it, not at SizeOfImage's 0x01d0e000. */
#ifndef SG_HOST_PAGE /* candidates for spore_guest.h */
#define SG_HOST_PAGE 0x4000u
#endif
#define SG_IMAGE_END (SG_IMAGE_BASE + SG_IMAGE_SIZE)                                  /* 0x01d0e000 */
#ifndef SG_IMAGE_MAPPED_END
#define SG_IMAGE_MAPPED_END ((SG_IMAGE_END + SG_HOST_PAGE - 1u) & ~(SG_HOST_PAGE - 1u)) /* 0x01d10000 */
#endif
#define SG_IMAGE_ENTRY 0x011e11e1u /* AddressOfEntryPoint (the CRT's start-up code) */

/* ---- window ------------------------------------------------------------------------------ */

/* The window base once sg_image_window_reserve() has run (NULL before). */
extern uint8_t *sg_image_mem;
/* Reserves SG_WINDOW_SIZE + SG_WINDOW_GUARD bytes PROT_NONE. Returns the base or NULL. Idempotent. */
uint8_t *sg_image_window_reserve(void);
/* Makes [lo, hi) of the guest space accessible with `prot` (PROT_* bits), rounding to host pages. */
int sg_image_window_commit(uint32_t lo, uint32_t hi, int prot);
/* SIGSEGV/SIGBUS handler that reports a guest address (fault - mem) and aborts (runtime-abi 3.3). */
void sg_image_install_fault_handler(void);

/* ---- loader ------------------------------------------------------------------------------- */

enum sg_image_source {
    SG_IMAGE_FROM_FILE = 0,   /* read the sections from the exe at run time (pread into the window) */
    SG_IMAGE_EMBED_COPY = 1,  /* copy the pre-laid-out image embedded in this binary */
    SG_IMAGE_EMBED_MMAP = 2,  /* map the embedded image straight from this binary's file (no copy) */
};

typedef struct sg_image_section {
    char name[9];
    uint32_t va, vsize;      /* guest address and virtual size */
    uint32_t raw_off, raw_size;
    uint32_t characteristics;
    int prot;                /* PROT_* derived from characteristics */
} sg_image_section;

typedef struct sg_image_info {
    enum sg_image_source source;
    uint32_t image_base, size_of_image, entry;
    unsigned nsections;
    sg_image_section sec[16];
    double load_ms;          /* reading or mapping the bytes */
    double protect_ms;       /* applying page protections */
    double hash_ms;          /* SHA-256 of the exe (FROM_FILE with verification only) */
    int sha_ok;              /* 1 match, 0 mismatch, -1 not checked */
    char path[1024];         /* file the bytes came from */
} sg_image_info;

extern sg_image_info sg_image;

/* Expected SHA-256 of game/SporebinEP1/SporeApp.exe (README). */
#define SG_IMAGE_SHA256 "dc04aee5a3debc3f1ad4c1a937460e99a29b9bd3bc285008be83615dd5e59a37"

/* Maps the image into the window. `exe_path` is used by SG_IMAGE_FROM_FILE (NULL: $SPORE_EXE, then
 * game/SporebinEP1/SporeApp.exe relative to the source tree the binary was built in). Leaves every image page
 * writable so the IAT can be filled; call sg_image_protect() afterwards. Returns 0 on success. */
int sg_image_load(uint8_t *mem, enum sg_image_source src, const char *exe_path, int verify_sha);
/* Applies the final protections: headers, .text, .rdata (with the IAT), .rsrc and .reloc read-only;
 * .data, .tls and CONST read/write. A host page shared by sections gets the union (CONST/.rsrc). */
int sg_image_protect(uint8_t *mem);
/* Is an embedded image linked into this binary? (weak symbols from the generated image.s) */
int sg_image_embedded(void);

/* ---- thunks (runtime-abi 7.1) -------------------------------------------------------------- */

/* Maps the thunk region (0xCC bytes, read-only) and fills the reserved slots 0..2. Idempotent. */
void sg_thunks_init(uint8_t *mem);
extern unsigned sg_thunk_count;      /* slots handed out so far (including the 16 reserved) */
extern const char *sg_image_thunk_names[];  /* debug names, parallel to sg_thunk_fn */

/* ---- dispatch ------------------------------------------------------------------------------ */

extern const uint32_t sg_dispatch_count;        /* entries in sg_dispatch_entries (generated) */
extern const char sg_dispatch_source[];         /* where the table came from (generated) */
/* Default miss hook: logs the address, the caller (return address at [esp] and ctx->eip), an ebp-chain
 * backtrace, whether the address is a resume point or a known label, appends it to $SPORE_MISS_LOG
 * (default build/sporenative/misses.txt), then aborts. SPORE_MISS=continue returns a stub that fakes a
 * cdecl `return 0` instead (debugging aid only). */
sg_func *sg_image_miss_hook(sg_ctx *ctx, uint8_t *mem, uint32_t guest);
/* Finds the dispatch entry for exactly `guest`, or NULL (no miss hook). */
const sg_dispatch_entry *sg_dispatch_find(uint32_t guest);
/* Resume-point lookup (sorted table generated with the dispatch table): owner function or 0. */
uint32_t sg_resume_owner(uint32_t resume);
extern unsigned long sg_miss_count;

/* ---- imports ------------------------------------------------------------------------------- */

/* Replaces the host function of an import before hle_install_imports runs (runtime/core and
 * runtime/win32 call this from their start-up hooks instead of editing runtime/hle). `dll` is
 * matched like GetModuleHandle ("kernel32", "KERNEL32.dll"); `name` exactly. Also replaces the
 * dynamic-table entry (GetProcAddress) of the same name. Returns the number of entries changed. */
unsigned sg_import_override(const char *dll, const char *name, sg_func *fn);
/* The same, by HLE shim symbol name ("sg_imp_kernel32_TlsAlloc"). */
unsigned sg_import_override_symbol(const char *symbol, sg_func *fn);
/* Registers every import (hle_install_imports) and then every dynamic name in table order, so thunk
 * addresses are the same on every run. Returns the number of IAT slots written. Must run after
 * sg_thunks_init and while the IAT is writable (between sg_image_load and sg_image_protect). */
unsigned sg_image_install_imports(uint8_t *mem);

/* ---- start-up hooks other packages define ---------------------------------------------------- */

/* runtime/image provides WEAK default definitions of these; a strong definition in runtime/core,
 * runtime/win32 or the integration package replaces them at link time (all runtime code is linked
 * as objects, never through archives, so a strong definition always wins). The defaults record that
 * the package is absent (sg_image_hooks_present). */
/* Called after the window and thunk region exist and before imports are installed. Register
 * overrides here (sg_import_override). */
void sg_core_startup(uint8_t *mem);   /* runtime/core  */
void sg_win32_startup(uint8_t *mem);  /* runtime/win32 */
/* Called by `spore --run` after loading: create the main guest thread and run the entry point.
 * Returns the process exit code. Provided by runtime/core;
 * the default prints that no boot path is linked and returns 3. */
int sg_boot(uint8_t *mem, uint32_t entry, int argc, char **argv);
/* Bit i set when the default (absent) hook i ran: 1 core, 2 win32, 4 boot. */
extern unsigned sg_image_hooks_absent;

#ifdef __cplusplus
}
#endif
#endif /* SG_IMAGE_H */
