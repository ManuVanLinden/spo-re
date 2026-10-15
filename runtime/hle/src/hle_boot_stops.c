/* hle_boot_stops.c -- the stop before the first window (docs/design/win32-layer.md 22.4).
 *
 * With SPORE_STOP_AT=window the boot stops at the first call to CreateWindowExW,
 * CreateWindowExA or Direct3DCreate9, whichever comes first: the point where the window and the
 * device begin. The stop calls hle_stop_hook (runtime/core's core_boot_diag.c installs one that prints
 * the guest call stack, the I/O table and exits 0); without a hook it prints and _exits(0).
 * SPORE_STOP_AT may also name one import (e.g. SPORE_STOP_AT=Direct3DCreate9).
 *
 * The three definitions are WEAK placeholders that keep the generated "unimplemented" behaviour
 * when no stop is requested. The real implementations (the platform layer, the D3D9 back ends)
 * must call hle_stop_check() first so the stop keeps working (one line each). */
#include <stdio.h>
#include <stdlib.h>
#include <string.h>
#include <unistd.h>

#include "hle_imports.h"

void (*hle_stop_hook)(sg_ctx *ctx, uint8_t *mem, const char *import);

int hle_stop_wanted(const char *import) {
    const char *s = getenv("SPORE_STOP_AT");
    if (!s || !*s) return 0;
    if (!strcmp(s, "window"))
        return !strcmp(import, "CreateWindowExW") || !strcmp(import, "CreateWindowExA") || !strcmp(import, "Direct3DCreate9");
    return !strcmp(s, import);
}

void hle_stop_check(sg_ctx *ctx, uint8_t *mem, const char *import) {
    if (!hle_stop_wanted(import)) return;
    if (hle_stop_hook) hle_stop_hook(ctx, mem, import);
    fprintf(stderr, "\nhle: STOP at %s (SPORE_STOP_AT=%s), called from guest 0x%08x\n", import, getenv("SPORE_STOP_AT"),
            sg_ld32(mem, ctx->esp));
    fflush(NULL);
    _exit(0);
}

__attribute__((weak)) SG_IMPORT(user32, CreateWindowExW) {
    hle_stop_check(ctx, mem, "CreateWindowExW");
    hle_unimplemented(ctx, mem, HLE_IMP_user32_CreateWindowExW);
    SG_RET(48);
}
__attribute__((weak)) SG_IMPORT(user32, CreateWindowExA) {
    hle_stop_check(ctx, mem, "CreateWindowExA");
    hle_unimplemented(ctx, mem, HLE_IMP_user32_CreateWindowExA);
    SG_RET(48);
}
__attribute__((weak)) SG_IMPORT(d3d9, Direct3DCreate9) {
    hle_stop_check(ctx, mem, "Direct3DCreate9");
    hle_unimplemented(ctx, mem, HLE_IMP_d3d9_Direct3DCreate9);
    SG_RET(4);
}
