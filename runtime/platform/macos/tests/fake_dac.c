/* fake_dac.c - see fake_dac.h. */
#include "fake_dac.h"

#include <math.h>
#include <stdio.h>
#include <string.h>
#include <time.h>

static double now_s(void) { return (double)sp_host_now_ns() / 1e9; }
static uint32_t time_get_time(void) { return (uint32_t)(sp_host_now_ns() / 1000000ull); }
static void sleep_ms(uint32_t ms) { struct timespec ts = {ms / 1000, (long)(ms % 1000) * 1000000L}; nanosleep(&ts, NULL); }

/* 0x113d640: how many frames to write this pass (a multiple of 256). */
static int32_t frames_to_write(fake_dac *d)
{
    uint32_t play, write;
    uint64_t t = sp_host_now_ns();
    sp_dsbuf_get_current_position(d->buf, &play, &write);
    d->gcp_ns += sp_host_now_ns() - t;
    d->gcp_calls++;
    double gap = (double)(((write / d->block) + d->frames - play / d->block) % d->frames) * 1000.0 / d->rate;
    if (gap < d->gap_min_ms) d->gap_min_ms = gap;
    if (gap > d->gap_max_ms) d->gap_max_ms = gap;
    int32_t target = ((int32_t)lroundf(d->latency_ms * 0.001f * (float)d->rate) + 0xff + (int32_t)(write / d->block)) & ~0xff;
    if ((int32_t)d->frames <= target) target -= (int32_t)d->frames;
    int32_t n = target - (int32_t)d->pos;
    if ((n < 0 && -(int32_t)d->half < n) || (int32_t)d->half < n) n = 0;
    else if (n < 0) n = (int32_t)d->frames - (int32_t)d->pos + target;
    return n;
}

/* 0x113d270: latency adaptation from the margin between the Dac position and the write cursor. */
static void adapt(fake_dac *d, uint32_t pos_before)
{
    uint32_t play, write;
    sp_dsbuf_get_current_position(d->buf, &play, &write);
    int32_t w = (int32_t)(write / d->block), p = (int32_t)pos_before;
    if ((int32_t)d->half + p <= w) p += (int32_t)d->frames;
    double t = now_s() - d->t0;
    if ((int32_t)lroundf(d->rate * 0.001f * 5.0f) + w >= p) {        /* within 5 ms: nearly late */
        d->near_underruns++;
        if (d->grow_after_s < t) d->latency_ms *= 1.1f;
        d->min_margin_ms = 1000.0f;
        d->next_adjust_s = t + 10.0;
    } else {
        float m = (float)(p - w) / (float)d->rate * 1000.0f;
        d->margin_sum_ms += m; d->margin_n++;
        if (m < d->margin_min_ms) d->margin_min_ms = m;
        if (m < d->min_margin_ms) d->min_margin_ms = m;
        if (d->next_adjust_s <= t) {
            float mm = d->min_margin_ms;
            d->next_adjust_s = t + 10.0;
            if (mm <= 10.0f) { if (mm < 5.0f) d->latency_ms += 5.0f - mm; }
            else if (mm < 500.0f) { d->latency_ms -= (mm - 5.0f) * 0.5f; if (d->latency_ms < 25.0f) d->latency_ms = 25.0f; }
            d->min_margin_ms = 1000.0f;
        }
    }
    if (d->latency_ms > 200.0f) d->latency_ms = 200.0f;
    if (d->latency_ms < d->lat_min) d->lat_min = d->latency_ms;
    if (d->latency_ms > d->lat_max) d->lat_max = d->latency_ms;
}

static void *dac_thread(void *arg)
{
    fake_dac *d = arg;
    uint32_t next = time_get_time();
    while (d->run) {
        d->wakeups++;
        int32_t n = frames_to_write(d);
        uint32_t pos_before = d->pos;
        for (; n > 0; n -= 256) {
            void *p1; uint32_t b1;
            if (sp_dsbuf_lock(d->buf, d->pos * d->block, 256 * d->block, &p1, &b1, NULL, NULL, 0) != SP_S_OK) break;
            int16_t *s = p1;
            for (int i = 0; i < 256; i++) {
                int16_t v = (int16_t)lrint(sin(d->phase) * d->amp * 32767.0);
                d->phase += 2.0 * M_PI * d->freq / d->rate;
                if (d->phase > 2.0 * M_PI) d->phase -= 2.0 * M_PI;
                s[2 * i] = v; s[2 * i + 1] = v;
            }
            sp_dsbuf_unlock(d->buf, p1, b1, NULL, 0);
            d->pos += 256;
            if (d->frames <= d->pos) d->pos -= d->frames;
            d->written += 256;
            d->blocks++;
            sp_dsbuf_debug_set_written(d->buf, d->written);
        }
        adapt(d, pos_before);
        next += 20;                                   /* 0x14: a 20 ms period */
        int32_t wait = (int32_t)(next - time_get_time());
        if (wait < 0 || wait > 200) wait = 1;
        sleep_ms((uint32_t)wait);
    }
    return NULL;
}

int fake_dac_start(fake_dac *d, const sp_audio_backend *be, double freq, double amp, float latency_ms)
{
    memset(d, 0, sizeof *d);
    d->rate = 44100; d->block = 4; d->freq = freq; d->amp = amp;
    d->latency_ms = latency_ms; d->min_margin_ms = 1000.0f;
    d->lat_min = d->lat_max = latency_ms;
    d->margin_min_ms = 1e9; d->gap_min_ms = 1e9;
    if (sp_ds_create(be, &d->ds) != SP_S_OK) return -1;
    sp_ds_set_cooperative_level(d->ds, 0, DSSCL_PRIORITY);
    sp_dscaps caps = {.dwSize = sizeof caps};
    sp_ds_get_caps(d->ds, &caps);
    sp_dsbufferdesc pd = {sizeof pd, DSBCAPS_PRIMARYBUFFER, 0, 0, 0, {0}};
    if (sp_ds_create_sound_buffer(d->ds, &pd, NULL, &d->prim) != SP_S_OK) return -2;
    sp_waveformatex f = {WAVE_FORMAT_PCM, 2, d->rate, d->rate * 4, 4, 16, 0};
    if (sp_dsbuf_set_format(d->prim, &f) != SP_S_OK) return -3;
    d->frames = (d->rate * 500 / 1000 + 0xff) & ~0xffu;       /* 0x113d5a2: 0.5 s rounded up to 256 */
    d->half = d->frames / 2;
    sp_dsbufferdesc sd = {sizeof sd, DSBCAPS_GETCURRENTPOSITION2 | DSBCAPS_GLOBALFOCUS, d->frames * 4, 0, 0, {0}};
    if (sp_ds_create_sound_buffer(d->ds, &sd, &f, &d->buf) != SP_S_OK) return -4;
    void *p1; uint32_t b1;                                     /* 0x113da60: clear, then loop */
    sp_dsbuf_lock(d->buf, 0, d->frames * 4, &p1, &b1, NULL, NULL, 0);
    memset(p1, 0, b1);
    sp_dsbuf_unlock(d->buf, p1, b1, NULL, 0);
    if (sp_dsbuf_play(d->buf, 0, 0, DSBPLAY_LOOPING) != SP_S_OK) return -5;
    d->t0 = now_s();
    d->grow_after_s = 1.0; d->next_adjust_s = 10.0;
    d->run = 1;
    pthread_create(&d->th, NULL, dac_thread, d);
    return 0;
}

void fake_dac_stop(fake_dac *d)
{
    if (d->run) { d->run = 0; pthread_join(d->th, NULL); }
    if (d->buf) { sp_dsbuf_stop(d->buf); sp_dsbuf_release(d->buf); d->buf = NULL; }
    if (d->prim) { sp_dsbuf_release(d->prim); d->prim = NULL; }
    if (d->ds) { sp_ds_release(d->ds); d->ds = NULL; }
}
