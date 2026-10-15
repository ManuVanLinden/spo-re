/* fake_dac.h - our re-implementation of the RWAudioCore "Dac" pacing loop, used to drive the fake
 * DirectSound buffer the way the game does (observed behaviour of 0x113dc70, 0x113d640, 0x113d270,
 * 0x113da60 and 0x113d410; see docs/design/platform-macos.md section 5). It writes a sine tone
 * instead of the game's mixer output. Our own code. */
#ifndef FAKE_DAC_H
#define FAKE_DAC_H

#include <pthread.h>
#include <stdatomic.h>
#include <stdint.h>
#include "sp_dsound.h"

typedef struct fake_dac {
    sp_ds *ds;
    sp_dsbuf *prim, *buf;
    uint32_t rate, frames, half, block;
    uint32_t pos;                 /* the Dac's write position in frames (object +0x5c) */
    uint64_t written;             /* unwrapped frames written */
    float latency_ms;             /* +0x54, adaptive 25..200 ms */
    float min_margin_ms;          /* +0x58 */
    double next_adjust_s;         /* +0x80 */
    double grow_after_s;          /* +0x70 */
    double phase, freq, amp;
    _Atomic int run;               /* atomic: the game's own flag 0x16e6307 is a plain byte */
    pthread_t th;
    double t0;
    /* statistics */
    uint64_t wakeups, blocks, gcp_calls, gcp_ns, near_underruns;
    float lat_min, lat_max;
    double margin_min_ms, margin_sum_ms; uint64_t margin_n;
    double gap_min_ms, gap_max_ms;   /* write cursor - play cursor */
} fake_dac;

/* Creates the DirectSound objects as the game does and starts the Dac thread. */
int  fake_dac_start(fake_dac *d, const sp_audio_backend *be, double freq, double amp, float latency_ms);
void fake_dac_stop(fake_dac *d);

#endif
