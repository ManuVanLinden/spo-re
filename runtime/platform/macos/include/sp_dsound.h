/*
 * sp_dsound.h - a fake DirectSound (IDirectSound + IDirectSoundBuffer) over CoreAudio.
 *
 * What the game does with DirectSound (observed in the EP1 exe, see docs/design/platform-macos.md):
 *   - DirectSoundCreate(&guid, &ds, NULL) at 0x113d9c5 after GetDeviceID(DSDEVID_DefaultPlayback);
 *     SetCooperativeLevel(hwnd, DSSCL_PRIORITY) (vtable +0x18); GetCaps (+0x10, DSCAPS size 0x60).
 *   - Per probed format (0x113d410): CreateSoundBuffer(+0x0c) for the primary buffer (dwFlags
 *     DSBCAPS_PRIMARYBUFFER), primary SetFormat (+0x38) with WAVE_FORMAT_PCM (1-2 channels) or
 *     WAVE_FORMAT_EXTENSIBLE, then a secondary buffer with dwFlags 0x18000
 *     (GETCURRENTPOSITION2 | GLOBALFOCUS) and 0.5 s of audio rounded up to 256 frames
 *     (22,272 frames, 89,088 bytes at 44.1 kHz 16-bit stereo).
 *   - Start (0x113da60): Lock(0, whole buffer) + memset 0 + Unlock, Play(0, 0, DSBPLAY_LOOPING).
 *   - The RWAudioCore Dac thread (0x113dc70) wakes every 20 ms (timeGetTime + Sleep), calls
 *     GetCurrentPosition (+0x10), aims at writeCursor + latency (25..200 ms, adaptive) rounded up to
 *     256 frames, and fills 256-frame blocks with Lock(offset, 1024 bytes, &p1, &b1, NULL, NULL, 0)
 *     (+0x2c) / Unlock (+0x4c). It never asks for the wrap-around second region.
 *
 * Positions: GetCurrentPosition's play cursor is interpolated from the CoreAudio render
 * timestamps, so it advances with real playback (the device's sample clock), not with a host
 * timer. The write cursor is the first frame not yet pulled by the render callback plus one
 * callback period, the earliest point the game may still safely write.
 *
 * Buffer memory comes from an allocator hook so the runtime can place it inside the 4 GB guest
 * window (Lock must return guest-visible pointers, runtime-abi.md section 8.1).
 */
#ifndef SP_DSOUND_H
#define SP_DSOUND_H

#include <stdint.h>
#include "sp_win32.h"

#ifdef __cplusplus
extern "C" {
#endif

typedef struct sp_ds sp_ds;
typedef struct sp_dsbuf sp_dsbuf;

typedef struct sp_ds_allocator {
    void *ctx;
    void *(*alloc)(void *ctx, uint32_t bytes);   /* must return zeroed memory */
    void  (*free)(void *ctx, void *p);
} sp_ds_allocator;
void sp_ds_set_allocator(const sp_ds_allocator *a);   /* NULL = calloc/free */

/* Output backend. sp_coreaudio_backend() is the real one; tests may pass NULL (no device) and
 * drive sp_dsbuf_render() themselves. */
typedef struct sp_audio_backend {
    int  (*open)(sp_dsbuf *b, const sp_waveformatex *fmt, void **handle);
    int  (*start)(void *handle);
    int  (*stop)(void *handle);
    void (*close)(void *handle);
    int  (*set_volume)(void *handle, float gain);
} sp_audio_backend;
const sp_audio_backend *sp_coreaudio_backend(void);
uint64_t sp_host_now_ns(void);   /* the clock the play cursor is interpolated against */
/* CoreAudio helpers (sp_coreaudio.c): presentation latency of the default output device, and its
 * current sample time with the matching host time (an independent playback clock for tests). */
uint64_t sp_coreaudio_output_latency_ns(double *device_rate, uint32_t *latency_frames);
int      sp_coreaudio_device_time(double *sample_time, uint64_t *host_ns);

/* ---- IDirectSound (vtable slots: 3 CreateSoundBuffer, 4 GetCaps, 6 SetCooperativeLevel) ---- */
sp_hresult sp_ds_create(const sp_audio_backend *backend, sp_ds **out);   /* DirectSoundCreate */
uint32_t   sp_ds_add_ref(sp_ds *ds);
uint32_t   sp_ds_release(sp_ds *ds);
sp_hresult sp_ds_set_cooperative_level(sp_ds *ds, uint32_t hwnd, uint32_t level);
sp_hresult sp_ds_get_caps(sp_ds *ds, sp_dscaps *caps);
/* fmt is the host view of desc->lpwfxFormat (the adapter resolves the guest pointer). */
sp_hresult sp_ds_create_sound_buffer(sp_ds *ds, const sp_dsbufferdesc *desc,
                                     const sp_waveformatex *fmt, sp_dsbuf **out);

/* ---- IDirectSoundBuffer (slot numbers in comments; byte offset = 4 * slot) ------------------ */
uint32_t   sp_dsbuf_add_ref(sp_dsbuf *b);                                  /* 1 */
uint32_t   sp_dsbuf_release(sp_dsbuf *b);                                  /* 2 */
sp_hresult sp_dsbuf_get_caps(sp_dsbuf *b, sp_dsbcaps *caps);               /* 3 */
sp_hresult sp_dsbuf_get_current_position(sp_dsbuf *b, uint32_t *play, uint32_t *write); /* 4, +0x10 */
sp_hresult sp_dsbuf_get_format(sp_dsbuf *b, sp_waveformatex *fmt, uint32_t size, uint32_t *written); /* 5 */
sp_hresult sp_dsbuf_get_volume(sp_dsbuf *b, int32_t *vol);                /* 6 */
sp_hresult sp_dsbuf_get_status(sp_dsbuf *b, uint32_t *status);             /* 9 */
sp_hresult sp_dsbuf_lock(sp_dsbuf *b, uint32_t offset, uint32_t bytes, void **p1, uint32_t *b1,
                         void **p2, uint32_t *b2, uint32_t flags);         /* 11, +0x2c */
sp_hresult sp_dsbuf_play(sp_dsbuf *b, uint32_t reserved, uint32_t priority, uint32_t flags); /* 12, +0x30 */
sp_hresult sp_dsbuf_set_current_position(sp_dsbuf *b, uint32_t offset);   /* 13 */
sp_hresult sp_dsbuf_set_format(sp_dsbuf *b, const sp_waveformatex *fmt);  /* 14, +0x38 (primary) */
sp_hresult sp_dsbuf_set_volume(sp_dsbuf *b, int32_t vol);                 /* 15 */
sp_hresult sp_dsbuf_stop(sp_dsbuf *b);                                     /* 18 */
sp_hresult sp_dsbuf_unlock(sp_dsbuf *b, void *p1, uint32_t b1, void *p2, uint32_t b2); /* 19, +0x4c */

/* ---- backend side ------------------------------------------------------------------------- */
/* Pull nframes into out (interleaved, the buffer's own format). out_host_ns is the host time
 * at which the first of these frames is heard. Called on the audio thread. */
void sp_dsbuf_render(sp_dsbuf *b, void *out, uint32_t nframes, uint64_t out_host_ns);

/* Statistics and test hooks. */
typedef struct sp_dsbuf_stats {
    uint64_t frames_pulled;       /* total frames handed to the device */
    uint64_t callbacks;
    uint64_t underruns;           /* callbacks that read past dbg_written_frames (if set) */
    uint32_t last_callback_frames, max_callback_frames;
} sp_dsbuf_stats;
void     sp_dsbuf_get_stats(sp_dsbuf *b, sp_dsbuf_stats *s);
/* Unwrapped play position in frames at host time t_ns (what GetCurrentPosition reports, before
 * the modulo). Returns 0 if nothing has been rendered yet. */
uint64_t sp_dsbuf_play_frames_at(sp_dsbuf *b, uint64_t t_ns);
/* Writer progress for underrun accounting in tests: total frames the producer has written. */
void     sp_dsbuf_debug_set_written(sp_dsbuf *b, uint64_t total_frames_written);
uint8_t *sp_dsbuf_data(sp_dsbuf *b);

#ifdef __cplusplus
}
#endif
#endif
