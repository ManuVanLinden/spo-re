/* d3d9null.h -- the null Direct3D 9.
 *
 * Direct3DCreate9 (d3d9.dll import, strong sg_imp_d3d9_Direct3DCreate9 here) returns an IDirect3D9
 * whose adapter queries follow the device policy: one
 * adapter, an NVIDIA GeForce 8800 GTX profile with driver build 117741, no PUREDEVICE, SM 3.0, the
 * ATI 'INST' FourCC hidden. CreateDevice returns an IDirect3DDevice9 on which every method the game
 * calls succeeds and draws nothing; resources are real enough (sizes, pitches, descs, guest-memory
 * Lock), shaders and declarations are kept, queries complete at once.
 *
 * Host hooks (weak defaults here; the platform layer may define strong ones):
 *   sg_host_display_mode  -- the desktop mode (default SPORE_DISPLAY_MODE=WxH[@Hz], else 1920x1080@60)
 *   sg_host_client_size   -- a window's client size for a windowed CreateDevice/Reset with 0x0
 *   sg_host_present       -- called on every Present; the default paces to SPORE_D3D9_FPS (60; 0 = off)
 */
#ifndef D3D9NULL_H
#define D3D9NULL_H

#include <stdint.h>

#include "sg_com.h"

#ifdef __cplusplus
extern "C" {
#endif

/* Creates an IDirect3D9 (one reference) and returns its guest address. sdk_version is what the game
 * passes (D3D_SDK_VERSION 32). Returns 0 on failure. */
uint32_t d3d9null_create(sg_ctx *ctx, uint8_t *mem, uint32_t sdk_version);

/* The ID3DXBuffer the D3DX shims hand out (D3DXCompileShader's code and error buffers): a buffer of
 * `size` bytes of guest memory. Returns the guest object; *data receives the buffer's guest address. */
uint32_t d3d9null_d3dx_buffer(uint32_t size, uint32_t *data);

/* Statistics for tests and the run report. */
typedef struct d3d9null_stats {
    uint64_t frames, draws, primitives, locks, creates;
    uint32_t live_resources, resource_bytes;
} d3d9null_stats;
void d3d9null_get_stats(d3d9null_stats *s);

int sg_host_display_mode(uint32_t *width, uint32_t *height, uint32_t *refresh);
int sg_host_client_size(uint32_t hwnd, uint32_t *width, uint32_t *height);
void sg_host_present(uint32_t hwnd, uint64_t frame);

/* The fixed HMONITOR GetAdapterMonitor returns (the window layer should return the same value
 * from MonitorFromWindow/MonitorFromPoint for the main display). */
#define D3D9NULL_HMONITOR 0x00e1f001u

/* ---- the Metal back end ---------------------------------------------------------------------------- *
 * Direct3DCreate9 reads SPORE_D3D9 once: unset or "metal" calls d3d9metal_create (a weak default here returns
 * 0, which falls back to the null device); "null" is the null device. What the null device shares with a
 * second handler set: */
/* The null IDirect3D9 handler for a slot (adapter count, display modes, monitor: the shared profile). */
sg_com_handler *d3d9null_d3d9_handler(unsigned slot);
/* Writes the adapter identity of the device policy (NVIDIA GeForce 8800 GTX, driver build 117741) into
 * the guest D3DADAPTER_IDENTIFIER9 at p (1100 bytes). */
void d3d9null_fill_identifier(uint8_t *mem, uint32_t p);
/* Bits per pixel (0 if unknown); *block = bytes per 4x4 block for DXT formats, else 0. */
unsigned d3d9null_fmt_bits(uint32_t fmt, unsigned *block);
/* The null device's level layout (pitches, 16-aligned level offsets, faces): the guest Lock shadow. */
int d3d9null_layout(uint32_t fmt, uint32_t w, uint32_t h, uint32_t d, uint32_t levels, uint32_t faces,
                    uint32_t *lvl_off, uint32_t *lvl_pitch, uint32_t *lvl_slice, uint32_t *face_bytes, uint32_t *bytes);
/* The one device mutex (the device policy) and the statistics the run report prints. */
pthread_mutex_t *d3d9null_device_lock(void);
d3d9null_stats *d3d9null_stats_ptr(void);

#ifdef __cplusplus
}
#endif
#endif
