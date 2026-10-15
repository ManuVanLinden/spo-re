/* dsound.c -- DirectSound for the guest: DSOUND ordinal 1 DirectSoundCreate and ordinal 9
 * GetDeviceID (the only two the exe imports [obs, data/imports.json]), and
 * dsound_create() for DirectSoundCreate8; IDirectSound / IDirectSound8, the primary buffer, secondary
 * IDirectSoundBuffer / IDirectSoundBuffer8, and IDirectSoundNotify on buffers created with
 * DSBCAPS_CTRLPOSITIONNOTIFY.
 *
 * Two back ends behind the same guest objects:
 *   - CoreAudio: runtime/platform/macos's sp_dsound.c + sp_coreaudio.c, used when they are
 *     linked into the binary (strong sp_* symbols) and SPORE_AUDIO is not "silent". Buffer memory comes
 *     from the guest window through sp_ds_set_allocator, so Lock hands out guest pointers.
 *   - Silent: the play cursor advances with the host clock at the buffer's byte rate
 *     (frequency x block align) while playing; nothing is heard. Used when the platform audio code is
 *     not linked (the weak sp_* defaults below), or with SPORE_AUDIO=silent.
 * Frequency and pan are kept here (sp_dsound has no controls for them) and only steer the silent clock.
 *
 * IDirectSoundNotify: SetNotificationPositions' events are set through the core event API
 * (sg_core_event_set, runtime/core/include/sg_core.h section 5) by one host notifier thread that,
 * every NOTIFY_TICK_MS while some buffer has positions, reads each such buffer's play cursor (either
 * back end) and sets the event of every offset the cursor passed since the last tick; DSBPN_OFFSETSTOP
 * events are set by Stop and when a one-shot buffer reaches its end. Polling rather than a hook in
 * the CoreAudio render callback keeps sp_dsound.c unchanged and works for the silent clock too; the
 * latency (one tick, about 5 ms) is well inside a typical notify spacing. The game itself never calls
 * SetNotificationPositions (its Dac thread polls GetCurrentPosition every 20 ms, platform-macos.md
 * 10.8), so this is for completeness. ds_lock serialises the notifier with the buffer methods that
 * read or move the cursor. */
#include <pthread.h>
#include <stdio.h>
#include <stdlib.h>
#include <string.h>
#include <time.h>

#include "sg_com.h"
#include "sp_dsound.h"

#define DS_OK 0u
#define DSERR_INVALIDPARAM 0x80070057u
#define DSERR_INVALIDCALL 0x88780032u
#define DSERR_NOAGGREGATION 0x80040110u
#define DSERR_PRIORITYLEVELNEEDED 0x88780046u
#define DSERR_CONTROLUNAVAIL 0x8878001eu
#define DSERR_UNSUPPORTED 0x80004001u
#define DSERR_ALREADYINITIALIZED 0x887800aau
#define DSBCAPS_CTRLPOSITIONNOTIFY 0x100u
#define DSBPN_OFFSETSTOP 0xffffffffu
#define NOTIFY_TICK_MS 5
#define DSBCAPS_CTRL3D 0x10u
#define DSBFREQUENCY_MIN 100u
#define DSBFREQUENCY_MAX 200000u

#define ARG(i) SG_COM_ARG(c, i)
#define MEM c->mem

/* ---- weak stand-ins for runtime/platform/macos's sp_* (used only when it is not linked) ------- */
#define SPW __attribute__((weak))
#define SP_FAIL ((sp_hresult)0x80004005u)
SPW const sp_audio_backend *sp_coreaudio_backend(void) { return NULL; }
SPW void sp_ds_set_allocator(const sp_ds_allocator *a) { (void)a; }
SPW sp_hresult sp_ds_create(const sp_audio_backend *b, sp_ds **o) { (void)b; *o = NULL; return SP_FAIL; }
SPW uint32_t sp_ds_release(sp_ds *ds) { (void)ds; return 0; }
SPW sp_hresult sp_ds_set_cooperative_level(sp_ds *ds, uint32_t h, uint32_t l) { (void)ds; (void)h; (void)l; return SP_FAIL; }
SPW sp_hresult sp_ds_get_caps(sp_ds *ds, sp_dscaps *c) { (void)ds; (void)c; return SP_FAIL; }
SPW sp_hresult sp_ds_create_sound_buffer(sp_ds *ds, const sp_dsbufferdesc *d, const sp_waveformatex *f, sp_dsbuf **o) {
    (void)ds; (void)d; (void)f; *o = NULL; return SP_FAIL;
}
SPW uint32_t sp_dsbuf_release(sp_dsbuf *b) { (void)b; return 0; }
SPW sp_hresult sp_dsbuf_get_current_position(sp_dsbuf *b, uint32_t *p, uint32_t *w) { (void)b; (void)p; (void)w; return SP_FAIL; }
SPW sp_hresult sp_dsbuf_get_format(sp_dsbuf *b, sp_waveformatex *f, uint32_t s, uint32_t *w) { (void)b; (void)f; (void)s; (void)w; return SP_FAIL; }
SPW sp_hresult sp_dsbuf_get_volume(sp_dsbuf *b, int32_t *v) { (void)b; (void)v; return SP_FAIL; }
SPW sp_hresult sp_dsbuf_get_status(sp_dsbuf *b, uint32_t *s) { (void)b; (void)s; return SP_FAIL; }
SPW sp_hresult sp_dsbuf_lock(sp_dsbuf *b, uint32_t o, uint32_t n, void **p1, uint32_t *b1, void **p2, uint32_t *b2, uint32_t f) {
    (void)b; (void)o; (void)n; (void)p1; (void)b1; (void)p2; (void)b2; (void)f; return SP_FAIL;
}
SPW sp_hresult sp_dsbuf_play(sp_dsbuf *b, uint32_t r, uint32_t p, uint32_t f) { (void)b; (void)r; (void)p; (void)f; return SP_FAIL; }
SPW sp_hresult sp_dsbuf_set_current_position(sp_dsbuf *b, uint32_t o) { (void)b; (void)o; return SP_FAIL; }
SPW sp_hresult sp_dsbuf_set_format(sp_dsbuf *b, const sp_waveformatex *f) { (void)b; (void)f; return SP_FAIL; }
SPW sp_hresult sp_dsbuf_set_volume(sp_dsbuf *b, int32_t v) { (void)b; (void)v; return SP_FAIL; }
SPW sp_hresult sp_dsbuf_stop(sp_dsbuf *b) { (void)b; return SP_FAIL; }
SPW sp_hresult sp_dsbuf_unlock(sp_dsbuf *b, void *p1, uint32_t b1, void *p2, uint32_t b2) { (void)b; (void)p1; (void)b1; (void)p2; (void)b2; return SP_FAIL; }
SPW void sp_dsbuf_get_stats(sp_dsbuf *b, sp_dsbuf_stats *s) { (void)b; memset(s, 0, sizeof *s); }
SPW void sp_dsbuf_debug_set_written(sp_dsbuf *b, uint64_t t) { (void)b; (void)t; }
SPW uint8_t *sp_dsbuf_data(sp_dsbuf *b) { (void)b; return NULL; }

/* The core event API: runtime/core's strong definition replaces this when the core is linked (the
 * game binary); the dsound tests define their own recorder. */
int sg_core_event_set(uint32_t h);
__attribute__((weak)) int sg_core_event_set(uint32_t h) { (void)h; return 0; }

static int use_coreaudio(void) {
    static int v = -1;
    if (v < 0) {
        const char *s = getenv("SPORE_AUDIO");
        v = !(s && !strcmp(s, "silent")) && sp_coreaudio_backend() != NULL;
    }
    return v;
}
static void *g_alloc(void *ctx, uint32_t n) {
    uint32_t g = sg_com_galloc(n);
    (void)ctx;
    return g ? sg_com_mem + g : NULL;
}
static void g_free(void *ctx, void *p) {
    (void)ctx;
    if (p) sg_com_gfree((uint32_t)((uint8_t *)p - sg_com_mem));
}
static uint32_t guest_ptr(void *p) { return p ? (uint32_t)((uint8_t *)p - sg_com_mem) : 0; }

static uint64_t now_ns(void) {
    struct timespec ts;
    clock_gettime(CLOCK_MONOTONIC, &ts);
    return (uint64_t)ts.tv_sec * 1000000000ull + (uint64_t)ts.tv_nsec;
}

#define PRIMARY_FLAGS(f) (((f) & DSBCAPS_PRIMARYBUFFER) != 0)
/* ---- objects --------------------------------------------------------------------------------- */
typedef struct dsdev {
    sg_com_obj com;
    sp_ds *sp;
    uint32_t coop, speaker;
    int is8;
} dsdev;
typedef struct dsbuf {
    sg_com_obj com;
    dsdev *ds;
    sp_dsbuf *sp;
    struct dsbuf *shares; /* duplicate: whose memory */
    uint32_t flags, bytes, data;  /* data: guest address (silent) */
    sp_waveformatex fmt;
    uint32_t freq, playing, looping;
    int32_t volume, pan;
    uint64_t t0;          /* silent clock: time of the last position change */
    uint32_t pos0;
    uint32_t nnotify, notify;     /* notify positions: count, guest copy of the DSBPOSITIONNOTIFY array */
    uint32_t nlast;               /* notifier: play cursor at the last tick */
    int nplaying;                 /* notifier: playing at the last tick */
    struct dsbuf *nnext;          /* notifier list (buffers with positions), under ds_lock */
    struct dslog *log;            /* SPORE_DS_LOG statistics, NULL when off */
} dsbuf;

extern sg_com_iface ds_iface_ds, ds_iface_ds8, ds_iface_buf, ds_iface_buf8, ds_iface_notify;

/* Silent play cursor in bytes. */
static uint32_t silent_play(dsbuf *b) {
    uint64_t rate, adv;
    if (!b->playing || !b->bytes) return b->pos0;
    rate = (uint64_t)b->freq * (b->fmt.nBlockAlign ? b->fmt.nBlockAlign : 1);
    adv = (now_ns() - b->t0) * rate / 1000000000ull;
    adv -= adv % (b->fmt.nBlockAlign ? b->fmt.nBlockAlign : 1);
    if (!b->looping && b->pos0 + adv >= b->bytes) { /* a one-shot buffer stops at its end */
        b->playing = 0;
        b->pos0 = 0;
        return 0;
    }
    return (uint32_t)((b->pos0 + adv) % b->bytes);
}
static void silent_set(dsbuf *b, uint32_t pos) {
    b->pos0 = pos;
    b->t0 = now_ns();
}

/* ---- position notifications -------------------------------------------------------------------- */
static pthread_mutex_t ds_lock = PTHREAD_MUTEX_INITIALIZER;
static pthread_cond_t ds_cv = PTHREAD_COND_INITIALIZER;
static dsbuf *nlist;
static int nthread_started;

static void notify_unlink_locked(dsbuf *b) {
    for (dsbuf **pp = &nlist; *pp; pp = &(*pp)->nnext)
        if (*pp == b) { *pp = b->nnext; b->nnext = NULL; return; }
}
/* Sets the event of every position in (from, to] (wrapping when to < from), or every offset > from
 * when to_end; stop: the DSBPN_OFFSETSTOP events. */
static void notify_fire_locked(dsbuf *b, uint32_t from, uint32_t to, int to_end, int stop) {
    for (uint32_t i = 0; i < b->nnotify; i++) {
        uint32_t off = sg_ld32(sg_com_mem, b->notify + 8u * i), ev = sg_ld32(sg_com_mem, b->notify + 8u * i + 4);
        int hit;
        if (off == DSBPN_OFFSETSTOP) hit = stop;
        else if (to_end) hit = off > from;
        else if (from < to) hit = off > from && off <= to;
        else if (from > to) hit = off > from || off <= to;
        else hit = 0;
        if (hit && ev) sg_core_event_set(ev);
    }
}
/* Play cursor and playing state, either back end (under ds_lock). */
static uint32_t silent_play(dsbuf *b);
static uint32_t notify_cursor_locked(dsbuf *b, int *playing) {
    uint32_t play = 0, write = 0, st = 0;
    if (b->sp) {
        sp_dsbuf_get_current_position(b->sp, &play, &write);
        sp_dsbuf_get_status(b->sp, &st);
        *playing = (st & DSBSTATUS_PLAYING) != 0;
    } else {
        play = silent_play(b);
        *playing = b->playing != 0;
    }
    return play;
}
static void notify_tick_locked(void) {
    for (dsbuf *b = nlist; b; b = b->nnext) {
        int playing;
        uint32_t cur = notify_cursor_locked(b, &playing);
        if (b->nplaying) {
            if (playing) notify_fire_locked(b, b->nlast, cur, 0, 0);
            else notify_fire_locked(b, b->nlast, 0, 1, 1); /* a one-shot buffer reached its end */
        }
        b->nlast = cur;
        b->nplaying = playing;
    }
}
static void *notify_main(void *arg) {
    (void)arg;
    pthread_mutex_lock(&ds_lock);
    for (;;) {
        if (!nlist) {
            pthread_cond_wait(&ds_cv, &ds_lock);
            continue;
        }
        notify_tick_locked();
        pthread_mutex_unlock(&ds_lock);
        struct timespec ts = {0, NOTIFY_TICK_MS * 1000000L};
        nanosleep(&ts, NULL);
        pthread_mutex_lock(&ds_lock);
    }
    return NULL;
}
static void notify_register_locked(dsbuf *b) {
    if (!b->nnext && nlist != b) {
        b->nnext = nlist;
        nlist = b;
    }
    if (!nthread_started) {
        pthread_t t;
        pthread_attr_t a;
        pthread_attr_init(&a);
        pthread_attr_setdetachstate(&a, PTHREAD_CREATE_DETACHED);
        if (!pthread_create(&t, &a, notify_main, NULL)) nthread_started = 1;
        pthread_attr_destroy(&a);
    }
    pthread_cond_signal(&ds_cv);
}
/* Play / Stop / SetCurrentPosition hooks for the notifier (under ds_lock). */
static void notify_on_play_locked(dsbuf *b) {
    int playing;
    if (!b->nnotify || b->nplaying) return;
    b->nlast = notify_cursor_locked(b, &playing);
    b->nplaying = 1;
}
/* cur, playing: the cursor just before the Stop. A one-shot buffer that already ran out (playing 0)
 * passed every position up to its end. */
static void notify_on_stop_locked(dsbuf *b, uint32_t cur, int playing) {
    if (!b->nnotify) return;
    if (b->nplaying) {
        if (playing) notify_fire_locked(b, b->nlast, cur, 0, 0); /* what played since the last tick */
        else notify_fire_locked(b, b->nlast, 0, 1, 0);
        notify_fire_locked(b, 0, 0, 0, 1);
    }
    b->nplaying = 0;
    b->nlast = cur;
}

/* ---- SPORE_DS_LOG: playback statistics -----------------------------------------------------
 * SPORE_DS_LOG=1 logs every buffer the game creates (format, size, flags), every Play / Stop /
 * SetCurrentPosition / SetFormat / SetFrequency / SetPan / SetVolume with the run time and host thread, interface
 * requests we refuse (3D, FX), and every DS_LOG_PERIOD s per playing buffer: GetCurrentPosition polls (count,
 * largest gap), the play cursor unwrapped from the polls against the wall clock since Play (drift), the write lead
 * (where each Unlock lands ahead of the write cursor, in ms), late writes (an Unlock behind the write cursor),
 * render underruns (the CoreAudio callback pulled frames past the last written one; sp_dsbuf_debug_set_written),
 * and the level of the PCM the game wrote (RMS and peak, dBFS). SPORE_DS_LOG=2 adds a level line per second.
 * SPORE_DS_WAV=path appends every unlocked block of the playing stream buffer to a 16-bit WAV file (diagnostic,
 * under build/). Times are seconds on the SPORE_INPUT_SCRIPT clock (sgp_run_time, found by dlsym) when a timed
 * run is armed, else since dsound was first used. dsound_dump_stats() prints the totals (called at RUN COMPLETE).
 */
#include <dlfcn.h>
#include <math.h>
#define DS_LOG_PERIOD 30.0
#define DS_LOG_EVENTS 400
typedef struct dslog {
    int id;
    uint64_t creates_ns;
    double t_play;          /* run time of the last Play, < 0 while stopped */
    uint64_t polls, plays, stops, setpos, locks, unlocks, late, nolead;
    double drift0, wall0;         /* drift and time at the first report: the slope is measured from there */
    int have0;
    double last_poll, max_gap, t_poll0; /* t_poll0: first poll after Play, the drift's origin */
    uint32_t last_play_pos;
    uint64_t play_bytes;     /* play cursor advance since Play, unwrapped from the polls */
    uint64_t wr_hi;          /* highest written frame, in render-total coordinates */
    double lead_min, lead_max, lead_sum;
    uint64_t lead_n;
    double next_report;
    double lvl_sumsq, lvl_peak; uint64_t lvl_n; double lvl_t;   /* per-second level window */
    double tot_sumsq, tot_peak; uint64_t tot_n, loud_s, quiet_s; /* totals over the run */
    double win_sumsq, win_peak; uint64_t win_n;                  /* per-report window */
    struct dslog *next;
} dslog;
static int ds_log_level = -1;
static dslog *ds_logs;
static int ds_log_ids, ds_log_events;
static uint64_t ds_t_base;
static FILE *ds_wav;
static uint64_t ds_wav_bytes;
static dsbuf *ds_wav_buf;
static sp_waveformatex ds_wav_fmt;
static int ds_log_on(void) {
    if (ds_log_level < 0) {
        const char *e = getenv("SPORE_DS_LOG");
        ds_log_level = e ? atoi(e) : 0;
    }
    return ds_log_level;
}
static double ds_time(void) {
    static double (*rt)(void);
    static int looked;
    double t;
    if (!looked) rt = (double (*)(void))dlsym(RTLD_DEFAULT, "sgp_run_time"), looked = 1;
    if (rt && (t = rt()) >= 0) return t;
    if (!ds_t_base) ds_t_base = now_ns();
    return (double)(now_ns() - ds_t_base) / 1e9;
}
static unsigned long long ds_tid(void) {
    uint64_t t = 0;
    pthread_threadid_np(NULL, &t);
    return (unsigned long long)t;
}
static double dbfs(double sumsq, uint64_t n) { return n && sumsq > 0 ? 10.0 * log10(sumsq / (double)n / (32768.0 * 32768.0)) : -999.0; }
static double dbfs_peak(double pk) { return pk > 0 ? 20.0 * log10(pk / 32768.0) : -999.0; }
static void ds_event(dsbuf *b, const char *what, uint32_t a0, uint32_t a1) {
    if (!b->log || ds_log_events >= DS_LOG_EVENTS) return;
    if (++ds_log_events == DS_LOG_EVENTS) fprintf(stderr, "dsound: (event log capped at %d lines; counts continue)\n", DS_LOG_EVENTS);
    fprintf(stderr, "dsound: t=%.3f buf#%d %s(0x%x, 0x%x) thread %llu\n", ds_time(), b->log->id, what, a0, a1, ds_tid());
}
static void wav_header(FILE *f, uint64_t bytes, const sp_waveformatex *w) {
    uint8_t h[44];
    uint32_t v;
#define P32(o, x) (v = (uint32_t)(x), memcpy(h + (o), &v, 4))
#define P16(o, x) do { uint16_t v16 = (uint16_t)(x); memcpy(h + (o), &v16, 2); } while (0)
    memcpy(h, "RIFF", 4); P32(4, 36 + bytes); memcpy(h + 8, "WAVEfmt ", 8); P32(16, 16); P16(20, 1); P16(22, w->nChannels);
    P32(24, w->nSamplesPerSec); P32(28, w->nAvgBytesPerSec); P16(32, w->nBlockAlign); P16(34, w->wBitsPerSample);
    memcpy(h + 36, "data", 4); P32(40, bytes);
#undef P32
#undef P16
    fseek(f, 0, SEEK_SET);
    fwrite(h, 1, 44, f);
    fseek(f, 0, SEEK_END);
}
static void ds_log_create(dsbuf *b, const sp_dsbufferdesc *d) {
    if (!ds_log_on()) return;
    b->log = calloc(1, sizeof *b->log);
    if (!b->log) return;
    b->log->id = ++ds_log_ids;
    b->log->t_play = -1;
    b->log->lead_min = 1e9;
    b->log->next = ds_logs;
    ds_logs = b->log;
    fprintf(stderr,
            "dsound: t=%.3f buf#%d CreateSoundBuffer flags 0x%x%s%s%s%s, %u bytes, format tag %u, %u ch, %u Hz, %u bits, "
            "block %u, %u B/s, thread %llu\n",
            ds_time(), b->log->id, d->dwFlags, (d->dwFlags & DSBCAPS_PRIMARYBUFFER) ? " PRIMARY" : "",
            (d->dwFlags & DSBCAPS_CTRL3D) ? " CTRL3D" : "", (d->dwFlags & DSBCAPS_CTRLPOSITIONNOTIFY) ? " CTRLPOSITIONNOTIFY" : "",
            (d->dwFlags & 0x10000u) ? " GETCURRENTPOSITION2" : "", b->bytes, b->fmt.wFormatTag, b->fmt.nChannels,
            b->fmt.nSamplesPerSec, b->fmt.wBitsPerSample, b->fmt.nBlockAlign, b->fmt.nAvgBytesPerSec, ds_tid());
}
static void ds_log_report(dsbuf *b, double t, const char *why) {
    dslog *L = b->log;
    double rate = (double)b->freq * (b->fmt.nBlockAlign ? b->fmt.nBlockAlign : 1);
    double wall = L->t_play >= 0 && L->polls ? t - L->t_poll0 : 0, cur = rate > 0 ? (double)L->play_bytes / rate : 0;
    sp_dsbuf_stats st;
    memset(&st, 0, sizeof st);
    if (b->sp) sp_dsbuf_get_stats(b->sp, &st);
    if (!L->have0 && wall > 1) L->drift0 = cur - wall, L->wall0 = wall, L->have0 = 1;
    fprintf(stderr,
            "dsound: t=%.1f buf#%d %s: %.1f s since the first poll, polls %llu (largest gap %.0f ms), play cursor %.3f s -> drift %+.1f ms "
            "(slope %+.1f ppm since the first report), lead %.0f..%.0f ms (mean %.0f), late writes %llu, render underruns %llu, callbacks %llu "
            "(max %u frames), locks %llu, level %.1f dBFS rms / %.1f peak\n",
            t, L->id, why, wall, (unsigned long long)L->polls, L->max_gap * 1000, cur, (cur - wall) * 1000,
            L->have0 && wall - L->wall0 > 1 ? ((cur - wall) - L->drift0) / (wall - L->wall0) * 1e6 : 0.0, L->lead_n ? L->lead_min : 0, L->lead_max,
            L->lead_n ? L->lead_sum / (double)L->lead_n : 0, (unsigned long long)L->late, (unsigned long long)st.underruns,
            (unsigned long long)st.callbacks, st.max_callback_frames, (unsigned long long)L->locks, dbfs(L->win_sumsq, L->win_n),
            dbfs_peak(L->win_peak));
    L->win_sumsq = L->win_peak = 0, L->win_n = 0;
    if (ds_wav) {
        wav_header(ds_wav, ds_wav_bytes, &ds_wav_fmt);
        fflush(ds_wav);
    }
}
/* GetCurrentPosition result (under ds_lock). */
static void ds_log_poll(dsbuf *b, uint32_t play) {
    dslog *L = b->log;
    double t;
    if (!L || PRIMARY_FLAGS(b->flags) || L->t_play < 0 || !b->bytes) return;
    t = ds_time();
    if (L->polls) {
        double gap = t - L->last_poll;
        if (gap > L->max_gap) L->max_gap = gap;
        L->play_bytes += (play + b->bytes - L->last_play_pos) % b->bytes;
    }
    else L->t_poll0 = t;
    L->polls++;
    L->last_poll = t;
    L->last_play_pos = play;
    if (t >= L->next_report) {
        ds_log_report(b, t, "stats");
        L->next_report = t + DS_LOG_PERIOD;
    }
}
static void ds_log_level_add(dslog *L, const uint8_t *p, uint32_t n, int bits) {
    if (bits != 16) return;
    for (uint32_t i = 0; i + 1 < n; i += 2) {
        int16_t v;
        double a;
        memcpy(&v, p + i, 2);
        a = v < 0 ? -(double)v : (double)v;
        L->lvl_sumsq += a * a;
        if (a > L->lvl_peak) L->lvl_peak = a;
    }
    L->lvl_n += n / 2;
}
static void ds_log_level_flush(dsbuf *b, double t) {
    dslog *L = b->log;
    double db = dbfs(L->lvl_sumsq, L->lvl_n);
    if (!L->lvl_n) return;
    if (ds_log_level >= 2)
        fprintf(stderr, "dsound: level t=%.0f buf#%d %.1f dBFS rms, %.1f peak\n", L->lvl_t, L->id, db, dbfs_peak(L->lvl_peak));
    if (db > -60.0) L->loud_s++;
    else L->quiet_s++;
    L->tot_sumsq += L->lvl_sumsq, L->tot_n += L->lvl_n;
    L->win_sumsq += L->lvl_sumsq, L->win_n += L->lvl_n;
    if (L->lvl_peak > L->tot_peak) L->tot_peak = L->lvl_peak;
    if (L->lvl_peak > L->win_peak) L->win_peak = L->lvl_peak;
    L->lvl_sumsq = L->lvl_peak = 0, L->lvl_n = 0;
    L->lvl_t = floor(t);
}
/* Unlock of guest regions (g1, n1), (g2, n2) (under ds_lock). */
static void ds_log_unlock(dsbuf *b, uint32_t g1, uint32_t n1, uint32_t g2, uint32_t n2) {
    dslog *L = b->log;
    double t;
    uint32_t base, blk = b->fmt.nBlockAlign ? b->fmt.nBlockAlign : 1;
    if (!L || PRIMARY_FLAGS(b->flags)) return;
    L->unlocks++;
    if (L->t_play < 0) return; /* the initial fill before Play */
    t = ds_time();
    if (floor(t) != L->lvl_t) ds_log_level_flush(b, t);
    if (g1 && n1) ds_log_level_add(L, sg_com_mem + g1, n1, b->fmt.wBitsPerSample);
    if (g2 && n2) ds_log_level_add(L, sg_com_mem + g2, n2, b->fmt.wBitsPerSample);
    if (ds_wav && ds_wav_buf == b) {
        if (g1 && n1) fwrite(sg_com_mem + g1, 1, n1, ds_wav), ds_wav_bytes += n1;
        if (g2 && n2) fwrite(sg_com_mem + g2, 1, n2, ds_wav), ds_wav_bytes += n2;
    }
    if (!b->sp || !g1) return;
    base = guest_ptr(sp_dsbuf_data(b->sp));
    {
        uint32_t frames = b->bytes / blk, off = (g1 - base) / blk, n = (n1 + n2) / blk, play = 0, write = 0, wf, ahead;
        sp_dsbuf_stats st;
        double ms;
        sp_dsbuf_get_current_position(b->sp, &play, &write);
        sp_dsbuf_get_stats(b->sp, &st);
        wf = write / blk;
        ahead = (off + frames - wf) % frames;
        ms = (double)ahead * 1000.0 / (double)b->freq;
        if (ahead > frames * 4 / 5) { /* behind the write cursor: the block lands in audio already pulled */
            L->late++;
            if (L->late <= 20)
                fprintf(stderr, "dsound: t=%.3f buf#%d late write: block at frame %u, write cursor %u (%.0f ms behind)\n", t, L->id,
                        off, wf, (double)(frames - ahead) * 1000.0 / (double)b->freq);
        } else {
            uint64_t end = st.frames_pulled + st.last_callback_frames + ahead + n;
            if (ms < L->lead_min) L->lead_min = ms;
            if (ms > L->lead_max) L->lead_max = ms;
            L->lead_sum += ms, L->lead_n++;
            if (end > L->wr_hi) {
                L->wr_hi = end;
                sp_dsbuf_debug_set_written(b->sp, end);
            }
        }
    }
}
static void ds_log_play(dsbuf *b, uint32_t flags) {
    dslog *L = b->log;
    if (!L || PRIMARY_FLAGS(b->flags)) return;
    L->plays++;
    ds_event(b, "Play", 0, flags);
    if (L->t_play < 0) {
        sp_dsbuf_stats st;
        L->t_play = ds_time();
        L->polls = 0, L->play_bytes = 0, L->max_gap = 0, L->have0 = 0;
        L->next_report = L->t_play + 5.0;
        L->lvl_t = floor(L->t_play);
        if (b->sp) { /* the whole buffer is valid when Play starts (the game clears it first) */
            memset(&st, 0, sizeof st);
            sp_dsbuf_get_stats(b->sp, &st);
            L->wr_hi = st.frames_pulled + b->bytes / (b->fmt.nBlockAlign ? b->fmt.nBlockAlign : 1);
            sp_dsbuf_debug_set_written(b->sp, L->wr_hi);
        }
        if (!ds_wav && !ds_wav_buf && (flags & DSBPLAY_LOOPING)) {
            const char *w = getenv("SPORE_DS_WAV");
            if (w && *w && b->fmt.wBitsPerSample == 16 && (ds_wav = fopen(w, "wb"))) {
                ds_wav_buf = b;
                ds_wav_fmt = b->fmt;
                wav_header(ds_wav, 0, &b->fmt);
                fprintf(stderr, "dsound: buf#%d stream written to %s\n", L->id, w);
            }
        }
    }
}
static void ds_log_stop(dsbuf *b) {
    dslog *L = b->log;
    if (!L || PRIMARY_FLAGS(b->flags)) return;
    L->stops++;
    ds_event(b, "Stop", 0, 0);
    if (L->t_play >= 0) {
        ds_log_report(b, ds_time(), "stopped");
        L->t_play = -1;
    }
}
__attribute__((visibility("default"))) void dsound_dump_stats(FILE *f);
void dsound_dump_stats(FILE *f) {
    pthread_mutex_lock(&ds_lock);
    for (dslog *L = ds_logs; L; L = L->next)
        if (L->plays || L->unlocks)
            fprintf(f,
                    "dsound: total buf#%d: plays %llu, stops %llu, SetCurrentPosition %llu, locks %llu, unlocks %llu, polls %llu, "
                    "late writes %llu, seconds with sound %llu, silent seconds %llu, level %.1f dBFS rms / %.1f peak\n",
                    L->id, (unsigned long long)L->plays, (unsigned long long)L->stops, (unsigned long long)L->setpos,
                    (unsigned long long)L->locks, (unsigned long long)L->unlocks, (unsigned long long)L->polls,
                    (unsigned long long)L->late, (unsigned long long)L->loud_s, (unsigned long long)L->quiet_s,
                    dbfs(L->tot_sumsq, L->tot_n), dbfs_peak(L->tot_peak));
    pthread_mutex_unlock(&ds_lock);
    /* the playing stream's last report */
    if (ds_wav) {
        wav_header(ds_wav, ds_wav_bytes, &ds_wav_fmt);
        fflush(ds_wav);
    }
}

static void buf_destroy(sg_com_obj *o) {
    dsbuf *b = (dsbuf *)o;
    pthread_mutex_lock(&ds_lock);
    notify_unlink_locked(b);
    if (b->log) {
        if (b->log->t_play >= 0) ds_log_report(b, ds_time(), "released");
        b->log->t_play = -1;
        if (ds_log_events < DS_LOG_EVENTS) fprintf(stderr, "dsound: t=%.3f buf#%d released\n", ds_time(), b->log->id);
    }
    if (ds_wav_buf == b) ds_wav_buf = (dsbuf *)1; /* keep the file; no further blocks */
    pthread_mutex_unlock(&ds_lock);
    if (b->sp) sp_dsbuf_release(b->sp);
    if (b->data && !b->shares) sg_com_gfree(b->data);
    if (b->shares) sg_com_obj_release(&b->shares->com);
    if (b->notify) sg_com_gfree(b->notify);
    sg_com_obj_retire(o);
    if (b->ds) sg_com_obj_release(&b->ds->com);
    free(b);
}
static uint32_t buf_query(sg_com_obj *o, const sg_guid *iid) {
    dsbuf *b = (dsbuf *)o;
    uint32_t g = 0;
    if (sg_guid_eq(iid, &ds_iface_buf8.iid) && !(b->flags & DSBCAPS_PRIMARYBUFFER)) g = sg_com_obj_expose(o, &ds_iface_buf8);
    else if (sg_guid_eq(iid, &ds_iface_notify.iid) && (b->flags & DSBCAPS_CTRLPOSITIONNOTIFY)) g = sg_com_obj_expose(o, &ds_iface_notify);
    if (g) sg_com_obj_addref(o);
    if (b->log)
        fprintf(stderr, "dsound: t=%.3f buf#%d QueryInterface {%08x-%04x-%04x-%02x%02x-...} -> %s\n", ds_time(), b->log->id, iid->d1,
                iid->d2, iid->d3, iid->d4[0], iid->d4[1], g ? "granted" : "refused");
    return g;
}
static const sg_com_class buf_class = {"IDirectSoundBuffer", buf_destroy, buf_query};

static void dev_destroy(sg_com_obj *o) {
    dsdev *d = (dsdev *)o;
    if (d->sp) sp_ds_release(d->sp);
    sg_com_obj_retire(o);
    free(d);
}
static uint32_t dev_query(sg_com_obj *o, const sg_guid *iid) {
    uint32_t g = 0;
    if (sg_guid_eq(iid, &ds_iface_ds8.iid) && ((dsdev *)o)->is8) g = sg_com_obj_expose(o, &ds_iface_ds8);
    if (g) sg_com_obj_addref(o);
    return g;
}
static const sg_com_class dev_class = {"IDirectSound", dev_destroy, dev_query};

/* ---- IDirectSound / IDirectSound8 -------------------------------------------------------------- */
#define DV ((dsdev *)c->obj)
static uint32_t make_buffer(dsdev *d, const sp_dsbufferdesc *desc, const sp_waveformatex *fmt, dsbuf **out) {
    dsbuf *b = calloc(1, sizeof *b);
    int primary = (desc->dwFlags & DSBCAPS_PRIMARYBUFFER) != 0;
    *out = NULL;
    if (!b) return SG_E_OUTOFMEMORY;
    b->flags = desc->dwFlags | (primary ? DSBCAPS_PRIMARYBUFFER : 0);
    b->bytes = primary ? 0 : desc->dwBufferBytes;
    if (fmt) b->fmt = *fmt;
    else b->fmt = (sp_waveformatex){WAVE_FORMAT_PCM, 2, 22050, 22050 * 4, 4, 16, 0}; /* the primary's default */
    b->freq = b->fmt.nSamplesPerSec;
    if (d->sp) {
        sp_hresult r = sp_ds_create_sound_buffer(d->sp, desc, fmt, &b->sp);
        if (r) {
            free(b);
            return r;
        }
    } else if (!primary) {
        b->data = sg_com_galloc(b->bytes);
        if (!b->data) {
            free(b);
            return SG_E_OUTOFMEMORY;
        }
    }
    if (sg_com_obj_init(&b->com, &buf_class) || !sg_com_obj_expose(&b->com, &ds_iface_buf)) {
        if (b->sp) sp_dsbuf_release(b->sp);
        if (b->data) sg_com_gfree(b->data);
        free(b);
        return SG_E_OUTOFMEMORY;
    }
    b->ds = d;
    sg_com_obj_addref(&d->com);
    *out = b;
    return DS_OK;
}
static uint32_t ds_create_buffer(sg_com_call *c) { /* (LPCDSBUFFERDESC, LPDIRECTSOUNDBUFFER*, LPUNKNOWN) */
    sp_dsbufferdesc desc;
    sp_waveformatex fmt;
    int primary;
    dsbuf *b;
    uint32_t r;
    if (!ARG(0) || !ARG(1)) return DSERR_INVALIDPARAM;
    sg_st32(MEM, ARG(1), 0);
    if (ARG(2)) return DSERR_NOAGGREGATION;
    if (!DV->coop) return DSERR_INVALIDCALL; /* SetCooperativeLevel first, as DirectSound requires */
    memset(&desc, 0, sizeof desc);
    memcpy(&desc, MEM + ARG(0), sg_ld32(MEM, ARG(0)) >= 36 ? 36 : 20);
    if (desc.dwSize != 36 && desc.dwSize != 20) return DSERR_INVALIDPARAM;
    primary = (desc.dwFlags & DSBCAPS_PRIMARYBUFFER) != 0;
    if (primary) {
        if (desc.dwBufferBytes || desc.lpwfxFormat) return DSERR_INVALIDPARAM;
    } else {
        if (!desc.lpwfxFormat || desc.dwBufferBytes < 4 || desc.dwBufferBytes > 0x0fffffffu) return DSERR_INVALIDPARAM;
        memcpy(&fmt, MEM + desc.lpwfxFormat, sizeof fmt);
        if (!fmt.nBlockAlign || !fmt.nSamplesPerSec || !fmt.nChannels) return DSERR_INVALIDPARAM;
    }
    r = make_buffer(DV, &desc, primary ? NULL : &fmt, &b);
    if (r) return r;
    pthread_mutex_lock(&ds_lock);
    ds_log_create(b, &desc);
    pthread_mutex_unlock(&ds_lock);
    if (sg_com_trace >= 1)
        fprintf(stderr, "dsound: CreateSoundBuffer(flags 0x%x, %u bytes%s) = 0x%08x (%s)\n", desc.dwFlags, desc.dwBufferBytes,
                primary ? ", primary" : "", sg_com_obj_guest(&b->com, NULL), b->sp ? "CoreAudio" : "silent");
    sg_st32(MEM, ARG(1), sg_com_obj_guest(&b->com, NULL));
    return DS_OK;
}
static uint32_t ds_get_caps(sg_com_call *c) { /* (LPDSCAPS) */
    sp_dscaps caps;
    if (!ARG(0) || sg_ld32(MEM, ARG(0)) != sizeof caps) return DSERR_INVALIDPARAM;
    if (DV->sp && !sp_ds_get_caps(DV->sp, &caps)) {
        memcpy(MEM + ARG(0), &caps, sizeof caps);
        return DS_OK;
    }
    memset(&caps, 0, sizeof caps);
    caps.dwSize = sizeof caps;
    caps.dwFlags = DSCAPS_PRIMARYMONO | DSCAPS_PRIMARYSTEREO | DSCAPS_PRIMARY8BIT | DSCAPS_PRIMARY16BIT | DSCAPS_CONTINUOUSRATE |
                   DSCAPS_SECONDARYMONO | DSCAPS_SECONDARYSTEREO | DSCAPS_SECONDARY8BIT | DSCAPS_SECONDARY16BIT;
    caps.dwMinSecondarySampleRate = DSBFREQUENCY_MIN;
    caps.dwMaxSecondarySampleRate = DSBFREQUENCY_MAX;
    caps.dwPrimaryBuffers = 1;
    memcpy(MEM + ARG(0), &caps, sizeof caps);
    return DS_OK;
}
static uint32_t ds_duplicate(sg_com_call *c) { /* (LPDIRECTSOUNDBUFFER src, LPDIRECTSOUNDBUFFER *dup) */
    dsbuf *src, *b;
    sp_dsbufferdesc desc;
    uint32_t r;
    if (!ARG(0) || !ARG(1)) return DSERR_INVALIDPARAM;
    sg_st32(MEM, ARG(1), 0);
    src = (dsbuf *)sg_com_lookup(MEM, ARG(0));
    if (!src || src->com.cls != &buf_class || (src->flags & DSBCAPS_PRIMARYBUFFER)) return DSERR_INVALIDCALL;
    memset(&desc, 0, sizeof desc);
    desc.dwSize = 36;
    desc.dwFlags = src->flags & ~DSBCAPS_PRIMARYBUFFER;
    desc.dwBufferBytes = src->bytes;
    if (src->sp) { /* CoreAudio back end: a new buffer with a copy of the data */
        r = make_buffer(DV, &desc, &src->fmt, &b);
        if (r) return r;
        {
            void *p1, *p2, *q1, *q2;
            uint32_t b1, b2, d1, d2;
            if (!sp_dsbuf_lock(src->sp, 0, src->bytes, &p1, &b1, &p2, &b2, 2) && !sp_dsbuf_lock(b->sp, 0, b->bytes, &q1, &d1, &q2, &d2, 2)) {
                memcpy(q1, p1, b1 < d1 ? b1 : d1);
                sp_dsbuf_unlock(b->sp, q1, d1, q2, d2);
                sp_dsbuf_unlock(src->sp, p1, b1, p2, b2);
            }
        }
    } else { /* silent: share the memory, own cursor */
        b = calloc(1, sizeof *b);
        if (!b) return SG_E_OUTOFMEMORY;
        *b = *src;
        memset(&b->com, 0, sizeof b->com);
        b->playing = 0;
        b->pos0 = 0;
        b->nnotify = 0;
        b->notify = 0;
        b->nlast = 0;
        b->nplaying = 0;
        b->nnext = NULL;
        b->shares = src->shares ? src->shares : src;
        if (sg_com_obj_init(&b->com, &buf_class) || !sg_com_obj_expose(&b->com, &ds_iface_buf)) {
            free(b);
            return SG_E_OUTOFMEMORY;
        }
        sg_com_obj_addref(&b->shares->com);
        sg_com_obj_addref(&DV->com);
    }
    sg_st32(MEM, ARG(1), sg_com_obj_guest(&b->com, NULL));
    return DS_OK;
}
static uint32_t ds_coop(sg_com_call *c) { /* (HWND, level) */
    if (ARG(1) < 1 || ARG(1) > 3) return DSERR_INVALIDPARAM; /* NORMAL, PRIORITY, EXCLUSIVE (= PRIORITY since DX8) */
    if (DV->sp) sp_ds_set_cooperative_level(DV->sp, ARG(0), ARG(1));
    DV->coop = ARG(1);
    return DS_OK;
}
static uint32_t ds_compact(sg_com_call *c) { return DS_OK; }
static uint32_t ds_get_speaker(sg_com_call *c) {
    if (!ARG(0)) return DSERR_INVALIDPARAM;
    sg_st32(MEM, ARG(0), DV->speaker);
    return DS_OK;
}
static uint32_t ds_set_speaker(sg_com_call *c) { DV->speaker = ARG(0); return DS_OK; }
static uint32_t ds_initialize(sg_com_call *c) { return DSERR_ALREADYINITIALIZED; }
static uint32_t ds_verify(sg_com_call *c) {
    if (!ARG(0)) return DSERR_INVALIDPARAM;
    sg_st32(MEM, ARG(0), 2); /* DS_UNCERTIFIED: not a certified driver */
    return DS_OK;
}
#define DS_METHODS                                                                                              \
    SG_COM_IUNKNOWN, {"CreateSoundBuffer", 3, 16, ds_create_buffer}, {"GetCaps", 4, 8, ds_get_caps},           \
        {"DuplicateSoundBuffer", 5, 12, ds_duplicate}, {"SetCooperativeLevel", 6, 12, ds_coop},                \
        {"Compact", 7, 4, ds_compact}, {"GetSpeakerConfig", 8, 8, ds_get_speaker},                             \
        {"SetSpeakerConfig", 9, 8, ds_set_speaker}, {"Initialize", 10, 8, ds_initialize}
static const sg_com_method ds_methods[] = {DS_METHODS};
static const sg_com_method ds8_methods[] = {DS_METHODS, {"VerifyCertification", 11, 8, ds_verify}};
sg_com_iface ds_iface_ds = {"IDirectSound", {0x279afa83, 0x4981, 0x11ce, {0xa5, 0x21, 0x00, 0x20, 0xaf, 0x0b, 0xe5, 0x60}},
                            11, ds_methods, SG_COM_N(ds_methods), NULL, NULL, DSERR_UNSUPPORTED, NULL, 0, 0};
sg_com_iface ds_iface_ds8 = {"IDirectSound8", {0xc50a7e93, 0xf395, 0x4834, {0x9e, 0xf6, 0x7f, 0xa9, 0x9d, 0xe5, 0x09, 0x66}},
                             12, ds8_methods, SG_COM_N(ds8_methods), NULL, NULL, DSERR_UNSUPPORTED, NULL, 0, 0};

/* ---- IDirectSoundBuffer / IDirectSoundBuffer8 ------------------------------------------------- */
#define B ((dsbuf *)c->obj)
#define PRIMARY(b) (((b)->flags & DSBCAPS_PRIMARYBUFFER) != 0)
static uint32_t b_get_caps(sg_com_call *c) {
    uint32_t p = ARG(0);
    if (!p || sg_ld32(MEM, p) != 20) return DSERR_INVALIDPARAM;
    sg_st32(MEM, p + 4, (B->flags & ~DSBCAPS_PRIMARYBUFFER) | DSBCAPS_LOCSOFTWARE);
    sg_st32(MEM, p + 8, B->bytes);
    sg_st32(MEM, p + 12, 0);
    sg_st32(MEM, p + 16, 0);
    return DS_OK;
}
static uint32_t b_position(sg_com_call *c) { /* (DWORD *play, DWORD *write) */
    uint32_t play = 0, write = 0;
    if (PRIMARY(B)) {
    } else if (B->sp) {
        if (sp_dsbuf_get_current_position(B->sp, &play, &write)) return DSERR_INVALIDCALL;
    } else {
        uint32_t ahead = (B->freq * (B->fmt.nBlockAlign ? B->fmt.nBlockAlign : 1)) / 100; /* 10 ms */
        ahead -= ahead % (B->fmt.nBlockAlign ? B->fmt.nBlockAlign : 1);
        play = silent_play(B);
        write = B->playing ? (play + ahead) % B->bytes : play;
    }
    if (ARG(0)) sg_st32(MEM, ARG(0), play);
    if (ARG(1)) sg_st32(MEM, ARG(1), write);
    if (B->log) ds_log_poll(B, play);
    return DS_OK;
}
static uint32_t b_get_format(sg_com_call *c) { /* (LPWAVEFORMATEX, size, DWORD *written) */
    uint32_t need = sizeof(sp_waveformatex);
    if (!ARG(0) && !ARG(2)) return DSERR_INVALIDPARAM;
    if (ARG(0)) {
        if (ARG(1) < need) return DSERR_INVALIDPARAM;
        memcpy(MEM + ARG(0), &B->fmt, need);
    }
    if (ARG(2)) sg_st32(MEM, ARG(2), need);
    return DS_OK;
}
static uint32_t b_get_volume(sg_com_call *c) {
    if (!ARG(0)) return DSERR_INVALIDPARAM;
    sg_st32(MEM, ARG(0), (uint32_t)B->volume);
    return DS_OK;
}
static uint32_t b_get_pan(sg_com_call *c) {
    if (!ARG(0)) return DSERR_INVALIDPARAM;
    sg_st32(MEM, ARG(0), (uint32_t)B->pan);
    return DS_OK;
}
static uint32_t b_get_freq(sg_com_call *c) {
    if (!ARG(0)) return DSERR_INVALIDPARAM;
    sg_st32(MEM, ARG(0), B->freq);
    return DS_OK;
}
static uint32_t b_get_status(sg_com_call *c) {
    uint32_t s = 0;
    if (!ARG(0)) return DSERR_INVALIDPARAM;
    if (B->sp) sp_dsbuf_get_status(B->sp, &s);
    else {
        silent_play(B);
        s = B->playing ? DSBSTATUS_PLAYING | (B->looping ? DSBSTATUS_LOOPING : 0) : 0;
    }
    sg_st32(MEM, ARG(0), s);
    return DS_OK;
}
static uint32_t b_initialize(sg_com_call *c) { return DSERR_ALREADYINITIALIZED; }
static uint32_t b_lock(sg_com_call *c) { /* (offset, bytes, void **p1, DWORD *b1, void **p2, DWORD *b2, flags) */
    uint32_t off = ARG(0), n = ARG(1), fl = ARG(6), g1, l1, g2 = 0, l2 = 0;
    if (!ARG(2) || !ARG(3)) return DSERR_INVALIDPARAM;
    if (PRIMARY(B)) return DSERR_PRIORITYLEVELNEEDED; /* only DSSCL_WRITEPRIMARY may lock the primary */
    if (B->log) B->log->locks++;
    if (B->sp) {
        void *p1 = NULL, *p2 = NULL;
        uint32_t r = sp_dsbuf_lock(B->sp, off, n, &p1, &l1, &p2, &l2, fl);
        if (r) return r;
        g1 = guest_ptr(p1);
        g2 = guest_ptr(p2);
    } else {
        uint32_t base = (B->shares ? B->shares : B)->data;
        if (fl & DSBLOCK_ENTIREBUFFER) off = 0, n = B->bytes;
        else if (fl & DSBLOCK_FROMWRITECURSOR) {
            uint32_t ahead = (B->freq * B->fmt.nBlockAlign) / 100;
            off = B->playing ? (silent_play(B) + ahead - ahead % B->fmt.nBlockAlign) % B->bytes : silent_play(B);
        }
        if (off >= B->bytes || !n || n > B->bytes) return DSERR_INVALIDPARAM;
        g1 = base + off;
        l1 = n <= B->bytes - off ? n : B->bytes - off;
        if (l1 < n) g2 = base, l2 = n - l1;
    }
    sg_st32(MEM, ARG(2), g1);
    sg_st32(MEM, ARG(3), l1);
    if (ARG(4)) sg_st32(MEM, ARG(4), g2);
    if (ARG(5)) sg_st32(MEM, ARG(5), l2);
    return DS_OK;
}
static uint32_t b_play(sg_com_call *c) { /* (reserved, priority, flags) */
    if (B->sp) {
        uint32_t r = sp_dsbuf_play(B->sp, ARG(0), ARG(1), ARG(2));
        if (r) return r;
    } else if (!B->playing) {
        silent_set(B, B->pos0);
    }
    B->playing = 1;
    B->looping = (ARG(2) & DSBPLAY_LOOPING) != 0;
    ds_log_play(B, ARG(2));
    notify_on_play_locked(B);
    return DS_OK;
}
static uint32_t b_set_position(sg_com_call *c) {
    uint32_t r = DS_OK;
    if (!PRIMARY(B) && ARG(0) >= B->bytes) return DSERR_INVALIDPARAM;
    if (B->log) B->log->setpos++, ds_event(B, "SetCurrentPosition", ARG(0), 0);
    if (B->sp) r = sp_dsbuf_set_current_position(B->sp, ARG(0));
    else silent_set(B, ARG(0) - ARG(0) % (B->fmt.nBlockAlign ? B->fmt.nBlockAlign : 1));
    if (!r && B->nnotify) { /* a jump does not pass the positions in between */
        int playing;
        B->nlast = notify_cursor_locked(B, &playing);
    }
    return r;
}
static uint32_t b_set_format(sg_com_call *c) {
    sp_waveformatex f;
    if (!ARG(0)) return DSERR_INVALIDPARAM;
    if (!PRIMARY(B)) return DSERR_INVALIDCALL;
    memcpy(&f, MEM + ARG(0), sizeof f);
    if (!f.nChannels || !f.nSamplesPerSec || !f.nBlockAlign) return DSERR_INVALIDPARAM;
    if (B->log)
        fprintf(stderr, "dsound: t=%.3f buf#%d SetFormat tag %u, %u ch, %u Hz, %u bits, block %u\n", ds_time(), B->log->id, f.wFormatTag,
                f.nChannels, f.nSamplesPerSec, f.wBitsPerSample, f.nBlockAlign);
    if (B->sp) {
        uint32_t r = sp_dsbuf_set_format(B->sp, &f);
        if (r) return r;
    }
    B->fmt = f;
    B->freq = f.nSamplesPerSec;
    return DS_OK;
}
static uint32_t b_set_volume(sg_com_call *c) {
    int32_t v = (int32_t)ARG(0);
    if (v < DSBVOLUME_MIN || v > DSBVOLUME_MAX) return DSERR_INVALIDPARAM;
    if (B->log && v != B->volume) ds_event(B, "SetVolume", (uint32_t)v, 0);
    if (B->sp) sp_dsbuf_set_volume(B->sp, v);
    B->volume = v;
    return DS_OK;
}
static uint32_t b_set_pan(sg_com_call *c) {
    int32_t v = (int32_t)ARG(0);
    if (v < -10000 || v > 10000) return DSERR_INVALIDPARAM;
    if (B->log && v != B->pan) ds_event(B, "SetPan", (uint32_t)v, 0);
    B->pan = v;
    return DS_OK;
}
static uint32_t b_set_freq(sg_com_call *c) { /* 0 = DSBFREQUENCY_ORIGINAL */
    uint32_t f = ARG(0);
    if (PRIMARY(B)) return DSERR_CONTROLUNAVAIL;
    if (f && (f < DSBFREQUENCY_MIN || f > DSBFREQUENCY_MAX)) return DSERR_INVALIDPARAM;
    if (B->log) ds_event(B, "SetFrequency", f, 0);
    if (!B->sp) silent_set(B, silent_play(B));
    B->freq = f ? f : B->fmt.nSamplesPerSec;
    return DS_OK;
}
static uint32_t b_stop(sg_com_call *c) {
    uint32_t cur = 0;
    int playing = 0;
    if (B->nnotify) cur = notify_cursor_locked(B, &playing);
    if (B->sp) sp_dsbuf_stop(B->sp);
    else silent_set(B, silent_play(B));
    B->playing = 0;
    ds_log_stop(B);
    if (B->nnotify) notify_on_stop_locked(B, cur, playing);
    return DS_OK;
}
static uint32_t b_unlock(sg_com_call *c) { /* (p1, b1, p2, b2) */
    if (B->log) {
        pthread_mutex_lock(&ds_lock);
        ds_log_unlock(B, ARG(0), ARG(1), ARG(2), ARG(3));
        pthread_mutex_unlock(&ds_lock);
    }
    if (B->sp) return sp_dsbuf_unlock(B->sp, ARG(0) ? MEM + ARG(0) : NULL, ARG(1), ARG(2) ? MEM + ARG(2) : NULL, ARG(3));
    return DS_OK;
}
static uint32_t b_restore(sg_com_call *c) { return DS_OK; }
static uint32_t b_set_fx(sg_com_call *c) { return ARG(0) ? DSERR_CONTROLUNAVAIL : DS_OK; }
static uint32_t b_acquire(sg_com_call *c) { return DS_OK; }
static uint32_t b_object_in_path(sg_com_call *c) {
    if (ARG(3)) sg_st32(MEM, ARG(3), 0);
    return 0x88780109u; /* DSERR_OBJECTNOTFOUND */
}
/* The methods that read or move the cursor run under ds_lock (the notifier thread reads it too). */
#define DS_LOCKED(fn) \
    static uint32_t fn##_l(sg_com_call *c) { pthread_mutex_lock(&ds_lock); uint32_t r_ = fn(c); pthread_mutex_unlock(&ds_lock); return r_; }
DS_LOCKED(b_position)
DS_LOCKED(b_get_status)
DS_LOCKED(b_lock)
DS_LOCKED(b_play)
DS_LOCKED(b_set_position)
DS_LOCKED(b_set_freq)
DS_LOCKED(b_stop)
#define BUF_METHODS                                                                                             \
    SG_COM_IUNKNOWN, {"GetCaps", 3, 8, b_get_caps}, {"GetCurrentPosition", 4, 12, b_position_l},               \
        {"GetFormat", 5, 16, b_get_format}, {"GetVolume", 6, 8, b_get_volume}, {"GetPan", 7, 8, b_get_pan},    \
        {"GetFrequency", 8, 8, b_get_freq}, {"GetStatus", 9, 8, b_get_status_l}, {"Initialize", 10, 12, b_initialize}, \
        {"Lock", 11, 32, b_lock_l}, {"Play", 12, 16, b_play_l}, {"SetCurrentPosition", 13, 8, b_set_position_l},      \
        {"SetFormat", 14, 8, b_set_format}, {"SetVolume", 15, 8, b_set_volume}, {"SetPan", 16, 8, b_set_pan},  \
        {"SetFrequency", 17, 8, b_set_freq_l}, {"Stop", 18, 4, b_stop_l}, {"Unlock", 19, 20, b_unlock},             \
        {"Restore", 20, 4, b_restore}
static const sg_com_method buf_methods[] = {BUF_METHODS};
static const sg_com_method buf8_methods[] = {BUF_METHODS, {"SetFX", 21, 16, b_set_fx}, {"AcquireResources", 22, 16, b_acquire},
                                             {"GetObjectInPath", 23, 20, b_object_in_path}};
sg_com_iface ds_iface_buf = {"IDirectSoundBuffer", {0x279afa85, 0x4981, 0x11ce, {0xa5, 0x21, 0x00, 0x20, 0xaf, 0x0b, 0xe5, 0x60}},
                             21, buf_methods, SG_COM_N(buf_methods), NULL, NULL, DSERR_UNSUPPORTED, NULL, 0, 0};
sg_com_iface ds_iface_buf8 = {"IDirectSoundBuffer8", {0x6825a449, 0x7524, 0x4d82, {0x92, 0x0f, 0x50, 0xe3, 0x6a, 0xb3, 0xab, 0x1e}},
                              24, buf8_methods, SG_COM_N(buf8_methods), NULL, NULL, DSERR_UNSUPPORTED, NULL, 0, 0};

/* ---- IDirectSoundNotify ----------------------------------------------------------------------- */
/* SetNotificationPositions(count, LPCDSBPOSITIONNOTIFY {dwOffset, hEventNotify}) [doc: SDK
 * IDirectSoundNotify8::SetNotificationPositions]: replaces the buffer's positions (count 0 clears
 * them); each offset is below the buffer size or DSBPN_OFFSETSTOP (else DSERR_INVALIDPARAM); the
 * buffer must be stopped (DSERR_INVALIDCALL while playing). The events are guest event handles,
 * set by the notifier thread through sg_core_event_set. */
static uint32_t n_set_positions(sg_com_call *c) {
    uint32_t n = ARG(0), r = DS_OK;
    if (n && !ARG(1)) return DSERR_INVALIDPARAM;
    if (n > 0x100000u) return DSERR_INVALIDPARAM;
    for (uint32_t i = 0; i < n; i++) {
        uint32_t off = sg_ld32(MEM, ARG(1) + 8u * i);
        if (off != DSBPN_OFFSETSTOP && off >= B->bytes) return DSERR_INVALIDPARAM;
    }
    pthread_mutex_lock(&ds_lock);
    {
        int playing;
        notify_cursor_locked(B, &playing);
        if (playing) r = DSERR_INVALIDCALL;
    }
    if (!r) {
        notify_unlink_locked(B);
        if (B->notify) sg_com_gfree(B->notify);
        B->notify = 0;
        B->nnotify = 0;
        B->nplaying = 0;
        if (n) {
            B->notify = sg_com_galloc(8u * n);
            if (!B->notify) r = SG_E_OUTOFMEMORY;
            else {
                memcpy(MEM + B->notify, MEM + ARG(1), 8u * n);
                B->nnotify = n;
                notify_register_locked(B);
            }
        }
    }
    pthread_mutex_unlock(&ds_lock);
    if (!r && n && sg_com_trace >= 1) fprintf(stderr, "dsound: SetNotificationPositions(%u): signalled by the notifier thread\n", n);
    return r;
}
static const sg_com_method notify_methods[] = {SG_COM_IUNKNOWN, {"SetNotificationPositions", 3, 12, n_set_positions}};
sg_com_iface ds_iface_notify = {"IDirectSoundNotify", {0xb0210783, 0x89cd, 0x11d0, {0xaf, 0x08, 0x00, 0xa0, 0xc9, 0x25, 0xcd, 0x16}},
                                4, notify_methods, SG_COM_N(notify_methods), NULL, NULL, DSERR_UNSUPPORTED, NULL, 0, 0};

/* ---- creation and the imports ------------------------------------------------------------------ */
uint32_t dsound_create(uint8_t *mem, int is8, uint32_t *hr);
uint32_t dsound_create(uint8_t *mem, int is8, uint32_t *hr) {
    dsdev *d;
    sg_com_init(mem);
    d = calloc(1, sizeof *d);
    *hr = SG_E_OUTOFMEMORY;
    if (!d) return 0;
    d->is8 = is8;
    d->speaker = 0x00000004u; /* DSSPEAKER_STEREO */
    if (use_coreaudio()) {
        static const sp_ds_allocator a = {NULL, g_alloc, g_free};
        sp_ds_set_allocator(&a);
        if (sp_ds_create(sp_coreaudio_backend(), &d->sp)) d->sp = NULL; /* fall back to silent */
    }
    if (sg_com_obj_init(&d->com, &dev_class) || !sg_com_obj_expose(&d->com, is8 ? &ds_iface_ds8 : &ds_iface_ds)) {
        free(d);
        return 0;
    }
    if (is8) sg_com_obj_expose(&d->com, &ds_iface_ds);
    *hr = DS_OK;
    return sg_com_obj_guest(&d->com, NULL);
}
int dsound_backend_is_coreaudio(uint32_t guest_ds, const uint8_t *mem);
int dsound_backend_is_coreaudio(uint32_t g, const uint8_t *mem) {
    dsdev *d = (dsdev *)sg_com_lookup(mem, g);
    return d && d->sp;
}

static const uint8_t default_playback[16] = {0x00, 0x00, 0xf0, 0xde, 0x6d, 0x9c, 0xed, 0x47, 0xaa, 0xf1, 0x4d, 0xda, 0x8f, 0x2b, 0x5c, 0x03};
static const uint8_t default_voice_playback[16] = {0x02, 0x00, 0xf0, 0xde, 0x6d, 0x9c, 0xed, 0x47, 0xaa, 0xf1, 0x4d, 0xda, 0x8f, 0x2b, 0x5c, 0x03};
/* The one output device's GUID (an arbitrary fixed value). */
static const uint8_t our_device[16] = {0x5d, 0x0a, 0x9e, 0x5a, 0x2b, 0x31, 0x4f, 0x6e, 0x9a, 0x6b, 0x53, 0x50, 0x4f, 0x52, 0x45, 0x31};

/* HRESULT GetDeviceID(LPCGUID pGuidSrc, LPGUID pGuidDest) -- DSOUND ordinal 9 */
SG_IMPORT(dsound, ord9) {
    uint32_t src = SG_ARG(0), dst = SG_ARG(1);
    if (!dst) SG_RETURN_STDCALL(DSERR_INVALIDPARAM, 8);
    if (!src || !memcmp(mem + src, default_playback, 16) || !memcmp(mem + src, default_voice_playback, 16) ||
        !memcmp(mem + src, our_device, 16)) {
        memcpy(mem + dst, our_device, 16);
        SG_RETURN_STDCALL(DS_OK, 8);
    }
    SG_RETURN_STDCALL(DSERR_INVALIDPARAM, 8); /* capture devices and unknown GUIDs */
}
/* HRESULT DirectSoundCreate(LPCGUID, LPDIRECTSOUND *, LPUNKNOWN) -- DSOUND ordinal 1 */
SG_IMPORT(dsound, ord1) {
    uint32_t guid = SG_ARG(0), out = SG_ARG(1), hr, g;
    if (!out) SG_RETURN_STDCALL(DSERR_INVALIDPARAM, 12);
    sg_st32(mem, out, 0);
    if (SG_ARG(2)) SG_RETURN_STDCALL(DSERR_NOAGGREGATION, 12);
    if (guid && memcmp(mem + guid, our_device, 16) && memcmp(mem + guid, default_playback, 16) &&
        memcmp(mem + guid, default_voice_playback, 16))
        SG_RETURN_STDCALL(0x88780078u /* DSERR_NODRIVER */, 12);
    g = dsound_create(mem, 0, &hr);
    sg_st32(mem, out, g);
    if (sg_com_trace >= 1)
        fprintf(stderr, "dsound: DirectSoundCreate = 0x%08x (%s)\n", g, dsound_backend_is_coreaudio(g, mem) ? "CoreAudio" : "silent");
    SG_RETURN_STDCALL(hr, 12);
}
