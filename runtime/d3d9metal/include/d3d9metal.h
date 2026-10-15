/* d3d9metal.h -- the Metal back end of the guest Direct3D 9 (src/: device, resources, present;
 * pipeline/: state and draws; shader/: d3dbc -> MSL). The public face: the entry, the host window hook,
 * statistics.
 *
 * runtime/d3d9null's Direct3DCreate9 calls d3d9metal_create for SPORE_D3D9=metal or unset. The guest sees
 * the same 16 interfaces and slot layouts as the null device, the same adapter identity and caps.
 *
 * Environment:
 *   SPORE_METAL_VERBOSE=1   log creation, formats, passes and presents (2: every upload/readback)
 *   SPORE_METAL_WATCH=N     one statistics line every N seconds (from Present) and at exit
 *   SPORE_METAL_IB_SLACK=0  turn off the INDEX16 lock slack (DEVIATIONS 3.6)
 *   SPORE_METAL_RING=0      turn off the 256-entry call ring the stall report prints
 *   SPORE_D3D9_FPS          as on the other back ends: 0 turns off the pacing floor
 */
#ifndef D3D9METAL_H
#define D3D9METAL_H

#include <stdint.h>

#include "spore_guest.h"

#ifdef __cplusplus
extern "C" {
#endif

uint32_t d3d9metal_create(sg_ctx *ctx, uint8_t *mem, uint32_t sdk_version);
/* 1 once d3d9metal_create has found a Metal device. */
int d3d9metal_loaded(void);

/* Host window hook: the CAMetalLayer * (unretained) to present into for a guest HWND, or NULL if the
 * window has none yet. The weak default (mt_mac.m) asks runtime/platform/guest's sgp_metal_layer through
 * dlsym (a child HWND falls back to sgp_main_window's layer); tests define a strong one. */
void *d3d9metal_host_layer(uint32_t hwnd);

typedef struct d3d9metal_wininfo {
    double scale, w_pt, h_pt, drawable_w, drawable_h;
    int has_window, subviews, visible, key, is_content_layer;
} d3d9metal_wininfo;
void d3d9metal_window_info(void *layer, d3d9metal_wininfo *out);

typedef struct d3d9metal_stats {
    uint64_t frames, presents_skipped, drawables_nil;
    uint64_t locks, unlocks, bytes_out, bytes_in;   /* guest shadow -> Metal, and back */
    uint64_t copy_ns;                               /* time in shadow copies and conversions */
    uint64_t renames, waits, wait_ns;               /* buffer renames on DISCARD/in flight; CPU waits for the GPU */
    uint64_t cmdbufs, blits, staged_uploads, direct_uploads, readbacks;
    uint64_t draws_stubbed, clears, query_waits, shader_creates;
    uint64_t gpu_errors;
    uint32_t shadow_bytes;                          /* guest shadow memory live */
    uint64_t metal_bytes;                           /* Metal buffers and textures we created, live */
} d3d9metal_stats;
void d3d9metal_get_stats(d3d9metal_stats *s);

#ifdef __cplusplus
}
#endif
#endif
