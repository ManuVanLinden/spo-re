/*
 * audio_drift.c - plays a test tone through the fake DirectSound buffer on the real output device,
 * driven by the fake Dac loop, and measures how GetCurrentPosition's play cursor tracks playback.
 *
 * Three clocks are compared every --sample-ms:
 *   P   our play cursor (unwrapped frames), sp_dsbuf_play_frames_at(now)
 *   D   the output device's own sample clock, AudioDeviceGetCurrentTime(), scaled from the device
 *       rate (48 kHz here) to the buffer's 44.1 kHz: an independent measure of real playback
 *   H   the host clock (mach continuous time)
 * Reported drifts are least-squares slopes, in ms per minute:
 *   P vs D  - the acceptance criterion: does the cursor track real playback? (target < 1 ms/min)
 *   P vs H  - nominal 44.1 kHz against wall time (the device crystal's error shows up here)
 * Our own test program; plays a quiet tone (default -50 dBFS).
 */
#include "fake_dac.h"
#include "sp_dsound.h"

#include <math.h>
#include <stdio.h>
#include <stdlib.h>
#include <string.h>
#include <time.h>

typedef struct { double t, p, d; } sample;

static void fit(const double *x, const double *y, int n, double *slope, double *icept, double *maxres)
{
    double sx = 0, sy = 0, sxx = 0, sxy = 0;
    for (int i = 0; i < n; i++) { sx += x[i]; sy += y[i]; sxx += x[i] * x[i]; sxy += x[i] * y[i]; }
    double den = n * sxx - sx * sx;
    *slope = den ? (n * sxy - sx * sy) / den : 0;
    *icept = (sy - *slope * sx) / n;
    double m = 0;
    for (int i = 0; i < n; i++) { double r = fabs(y[i] - (*slope * x[i] + *icept)); if (r > m) m = r; }
    *maxres = m;
}

int main(int argc, char **argv)
{
    double seconds = 300, amp = 0.003, freq = 440, sample_ms = 100, warm = 3;
    const char *json = NULL;
    for (int i = 1; i < argc; i++) {
        if (!strcmp(argv[i], "--seconds") && i + 1 < argc) seconds = atof(argv[++i]);
        else if (!strcmp(argv[i], "--amp") && i + 1 < argc) amp = atof(argv[++i]);
        else if (!strcmp(argv[i], "--freq") && i + 1 < argc) freq = atof(argv[++i]);
        else if (!strcmp(argv[i], "--sample-ms") && i + 1 < argc) sample_ms = atof(argv[++i]);
        else if (!strcmp(argv[i], "--json") && i + 1 < argc) json = argv[++i];
        else { fprintf(stderr, "usage: %s [--seconds S] [--amp A] [--freq HZ] [--sample-ms MS] [--json out]\n", argv[0]); return 2; }
    }
    double dev_rate = 0; uint32_t lat_frames = 0;
    uint64_t lat_ns = sp_coreaudio_output_latency_ns(&dev_rate, &lat_frames);
    printf("output device: %.0f Hz, presentation latency %u frames (%.2f ms)\n", dev_rate, lat_frames, lat_ns / 1e6);

    fake_dac dac;
    int rc = fake_dac_start(&dac, sp_coreaudio_backend(), freq, amp, 50.0f);
    if (rc) { fprintf(stderr, "fake_dac_start failed: %d\n", rc); return 1; }

    int cap = (int)(seconds * 1000 / sample_ms) + 16, n = 0;
    sample *s = calloc((size_t)cap, sizeof *s);
    uint64_t start = sp_host_now_ns();
    long backwards = 0;
    double lastp = -1;
    struct timespec ts = {0, (long)(sample_ms * 1e6)};
    while (n < cap) {
        nanosleep(&ts, NULL);
        double st; uint64_t dh;
        if (!sp_coreaudio_device_time(&st, &dh)) continue;
        uint64_t now = sp_host_now_ns();
        double t = (now - start) / 1e9;
        if (t > seconds + warm) break;
        double p = (double)sp_dsbuf_play_frames_at(dac.buf, now);
        double pd = (double)sp_dsbuf_play_frames_at(dac.buf, dh);       /* cursor at the device's instant */
        if (p < lastp) backwards++;
        lastp = p;
        if (t < warm) continue;
        s[n++] = (sample){t, pd, st * 44100.0 / dev_rate};
        (void)p;
    }
    sp_dsbuf_stats stats;
    sp_dsbuf_get_stats(dac.buf, &stats);
    fake_dac_stop(&dac);

    double *x = malloc(n * sizeof *x), *y = malloc(n * sizeof *y);
    /* P vs device clock: offset (ms) over time */
    for (int i = 0; i < n; i++) { x[i] = s[i].t / 60.0; y[i] = (s[i].p - s[i].d) * 1000.0 / 44100.0; }
    double pd_slope, pd_ic, pd_res; fit(x, y, n, &pd_slope, &pd_ic, &pd_res);
    double off_min = 1e9, off_max = -1e9;
    for (int i = 0; i < n; i++) { if (y[i] < off_min) off_min = y[i]; if (y[i] > off_max) off_max = y[i]; }
    /* P vs host clock at nominal 44.1 kHz */
    for (int i = 0; i < n; i++) { x[i] = s[i].t / 60.0; y[i] = s[i].p * 1000.0 / 44100.0 - s[i].t * 1000.0; }
    double ph_slope, ph_ic, ph_res; fit(x, y, n, &ph_slope, &ph_ic, &ph_res);
    /* device clock vs host clock */
    for (int i = 0; i < n; i++) { y[i] = s[i].d * 1000.0 / 44100.0 - s[i].t * 1000.0; }
    double dh_slope, dh_ic, dh_res; fit(x, y, n, &dh_slope, &dh_ic, &dh_res);

    double minutes = n ? (s[n - 1].t - s[0].t) / 60.0 : 0;
    printf("samples: %d over %.2f min (every %.0f ms, after %.0f s warm-up)\n", n, minutes, sample_ms, warm);
    printf("drift P vs device clock : %+.4f ms/min (offset range %.3f .. %.3f ms, max fit residual %.3f ms)\n",
           pd_slope, off_min, off_max, pd_res);
    printf("drift P vs host clock   : %+.4f ms/min (%.1f ppm)\n", ph_slope, ph_slope / 60000.0 * 1e6);
    printf("device clock vs host    : %+.4f ms/min (%.1f ppm)\n", dh_slope, dh_slope / 60000.0 * 1e6);
    printf("play cursor went backwards: %ld times\n", backwards);
    printf("render callbacks: %llu, frames %llu, callback size %u (max %u), underruns %llu\n",
           (unsigned long long)stats.callbacks, (unsigned long long)stats.frames_pulled,
           stats.last_callback_frames, stats.max_callback_frames, (unsigned long long)stats.underruns);
    printf("Dac: %llu wakeups, %llu blocks, latency %.1f..%.1f ms (final %.1f), margin min %.2f ms avg %.2f ms, near-underruns %llu\n",
           (unsigned long long)dac.wakeups, (unsigned long long)dac.blocks, dac.lat_min, dac.lat_max, dac.latency_ms,
           dac.margin_min_ms, dac.margin_n ? dac.margin_sum_ms / dac.margin_n : 0, (unsigned long long)dac.near_underruns);
    printf("write-play gap %.2f..%.2f ms; GetCurrentPosition %.0f ns avg over %llu calls\n",
           dac.gap_min_ms, dac.gap_max_ms, dac.gcp_calls ? (double)dac.gcp_ns / dac.gcp_calls : 0,
           (unsigned long long)dac.gcp_calls);
    int pass = fabs(pd_slope) < 1.0 && backwards == 0;
    printf("VERIFY (|P vs device| < 1 ms/min, monotonic): %s\n", pass ? "PASS" : "FAIL");

    if (json) {
        FILE *f = fopen(json, "w");
        if (f) {
            fprintf(f, "{\n  \"tool\": \"runtime/platform/macos/tests/audio_drift.c\",\n");
            fprintf(f, "  \"device_rate_hz\": %.0f, \"device_latency_frames\": %u, \"device_latency_ms\": %.3f,\n", dev_rate, lat_frames, lat_ns / 1e6);
            fprintf(f, "  \"buffer\": {\"rate_hz\": 44100, \"channels\": 2, \"bits\": 16, \"frames\": %u, \"flags\": \"0x18000\"},\n", dac.frames);
            fprintf(f, "  \"tone\": {\"freq_hz\": %.1f, \"amplitude\": %.4f},\n", freq, amp);
            fprintf(f, "  \"samples\": %d, \"minutes\": %.3f, \"sample_ms\": %.0f,\n", n, minutes, sample_ms);
            fprintf(f, "  \"drift_ms_per_min\": {\"play_vs_device_clock\": %.5f, \"play_vs_host_clock\": %.5f, \"device_vs_host_clock\": %.5f},\n",
                    pd_slope, ph_slope, dh_slope);
            fprintf(f, "  \"play_vs_device_offset_ms\": {\"min\": %.4f, \"max\": %.4f, \"max_fit_residual\": %.4f},\n", off_min, off_max, pd_res);
            fprintf(f, "  \"play_cursor_backwards\": %ld,\n", backwards);
            fprintf(f, "  \"render\": {\"callbacks\": %llu, \"frames\": %llu, \"callback_frames\": %u, \"max_callback_frames\": %u, \"underruns\": %llu},\n",
                    (unsigned long long)stats.callbacks, (unsigned long long)stats.frames_pulled, stats.last_callback_frames,
                    stats.max_callback_frames, (unsigned long long)stats.underruns);
            fprintf(f, "  \"dac\": {\"wakeups\": %llu, \"blocks\": %llu, \"latency_ms_min\": %.2f, \"latency_ms_max\": %.2f, \"latency_ms_final\": %.2f, \"margin_ms_min\": %.3f, \"margin_ms_avg\": %.3f, \"near_underruns\": %llu},\n",
                    (unsigned long long)dac.wakeups, (unsigned long long)dac.blocks, dac.lat_min, dac.lat_max, dac.latency_ms,
                    dac.margin_min_ms, dac.margin_n ? dac.margin_sum_ms / dac.margin_n : 0, (unsigned long long)dac.near_underruns);
            fprintf(f, "  \"write_play_gap_ms\": {\"min\": %.3f, \"max\": %.3f},\n", dac.gap_min_ms, dac.gap_max_ms);
            fprintf(f, "  \"get_current_position_ns_avg\": %.1f,\n", dac.gcp_calls ? (double)dac.gcp_ns / dac.gcp_calls : 0);
            fprintf(f, "  \"verify_pass\": %s\n}\n", pass ? "true" : "false");
            fclose(f);
        }
    }
    free(s); free(x); free(y);
    return pass ? 0 : 1;
}
