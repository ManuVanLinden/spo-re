/*
 * sp_dsound.c - fake IDirectSound / IDirectSoundBuffer (portable C11 atomics + pthreads).
 * See include/sp_dsound.h for what the game needs and docs/design/platform-macos.md for the design.
 *
 * Concurrency: the render callback (audio thread) is the only writer of read_pos, total and the
 * timestamp history; it publishes history entries under a sequence lock. Any thread may read
 * positions. The game's writes into the ring are made visible by the release fence in Unlock and
 * the acquire fence at the top of each render callback.
 */
#include "sp_dsound.h"

#include <pthread.h>
#include <stdatomic.h>
#include <stdlib.h>
#include <string.h>
#include <time.h>

#define HIST 32

typedef struct hent { uint64_t host_ns, total; uint32_t pos, n; } hent;

struct sp_ds {
    _Atomic uint32_t refs;
    const sp_audio_backend *backend;
    uint32_t hwnd, level;
};

struct sp_dsbuf {
    _Atomic uint32_t refs;
    sp_ds *ds;
    int primary;
    sp_waveformatex fmt;
    uint32_t flags, bytes, frames, block;
    uint8_t *data;
    pthread_mutex_t lock;              /* serialises Play/Stop/SetCurrentPosition/Release */
    _Atomic int playing, looping;
    _Atomic uint32_t read_pos;         /* next frame the render callback pulls */
    _Atomic uint64_t total;            /* frames pulled since creation */
    _Atomic uint32_t frozen_pos;       /* play position while stopped */
    _Atomic uint32_t seq;              /* history sequence lock (odd while writing) */
    _Atomic uint32_t hcount;           /* history entries written (monotonic) */
    hent hist[HIST];
    _Atomic uint32_t last_n, max_n;
    _Atomic uint64_t callbacks, underruns, dbg_written;
    _Atomic int dbg_enabled;
    int32_t volume, pan;
    void *bh;
    int bopen;
};

static const uint8_t KSDATAFORMAT_SUBTYPE_PCM[16] = {
    0x01, 0x00, 0x00, 0x00, 0x00, 0x00, 0x10, 0x00, 0x80, 0x00, 0x00, 0xAA, 0x00, 0x38, 0x9B, 0x71};

static sp_ds_allocator g_alloc;

static void *dflt_alloc(void *c, uint32_t n) { (void)c; return calloc(1, n); }
static void dflt_free(void *c, void *p) { (void)c; free(p); }

void sp_ds_set_allocator(const sp_ds_allocator *a)
{
    if (a) g_alloc = *a;
    else { g_alloc.ctx = NULL; g_alloc.alloc = dflt_alloc; g_alloc.free = dflt_free; }
}

uint64_t sp_host_now_ns(void) { return clock_gettime_nsec_np(CLOCK_UPTIME_RAW); }

/* ---- IDirectSound --------------------------------------------------------------------------- */
sp_hresult sp_ds_create(const sp_audio_backend *backend, sp_ds **out)
{
    if (!out) return SP_DSERR_INVALIDPARAM;
    if (!g_alloc.alloc) sp_ds_set_allocator(NULL);
    sp_ds *ds = calloc(1, sizeof *ds);
    if (!ds) return SP_E_OUTOFMEMORY;
    atomic_init(&ds->refs, 1);
    ds->backend = backend;
    *out = ds;
    return SP_S_OK;
}

uint32_t sp_ds_add_ref(sp_ds *ds) { return atomic_fetch_add(&ds->refs, 1) + 1; }
uint32_t sp_ds_release(sp_ds *ds)
{
    uint32_t r = atomic_fetch_sub(&ds->refs, 1) - 1;
    if (!r) free(ds);
    return r;
}

sp_hresult sp_ds_set_cooperative_level(sp_ds *ds, uint32_t hwnd, uint32_t level)
{
    if (level < DSSCL_NORMAL || level > DSSCL_WRITEPRIMARY) return SP_DSERR_INVALIDPARAM;
    ds->hwnd = hwnd; ds->level = level;
    return SP_S_OK;
}

sp_hresult sp_ds_get_caps(sp_ds *ds, sp_dscaps *c)
{
    (void)ds;
    if (!c || c->dwSize != sizeof *c) return SP_DSERR_INVALIDPARAM;
    memset(c, 0, sizeof *c);
    c->dwSize = sizeof *c;
    c->dwFlags = DSCAPS_PRIMARYMONO | DSCAPS_PRIMARYSTEREO | DSCAPS_PRIMARY8BIT | DSCAPS_PRIMARY16BIT |
                 DSCAPS_CONTINUOUSRATE | DSCAPS_SECONDARYMONO | DSCAPS_SECONDARYSTEREO |
                 DSCAPS_SECONDARY8BIT | DSCAPS_SECONDARY16BIT;
    c->dwMinSecondarySampleRate = 100;       /* DSBFREQUENCY_MIN */
    c->dwMaxSecondarySampleRate = 200000;    /* DSBFREQUENCY_MAX */
    c->dwPrimaryBuffers = 1;
    return SP_S_OK;
}

/* Formats we play: PCM, 1-2 channels, 8/16-bit (or 16/24/32-bit through EXTENSIBLE). The game
 * probes 1, 2, 4 and 6 channels (table 0x14c2f88); refusing more than 2 makes it pick stereo,
 * which is what EA's own Mac build chose (ConfigManager.txt: Support51Audio false under $cider). */
static int valid_format(const sp_waveformatex *f)
{
    if (!f) return 0;
    int bits = f->wBitsPerSample;
    if (f->wFormatTag == WAVE_FORMAT_EXTENSIBLE) {
        const sp_waveformatextensible *x = (const sp_waveformatextensible *)f;
        if (f->cbSize < 22 || memcmp(x->SubFormat, KSDATAFORMAT_SUBTYPE_PCM, 16) != 0) return 0;
        if (bits != 16 && bits != 24 && bits != 32) return 0;
    } else if (f->wFormatTag == WAVE_FORMAT_PCM) {
        if (bits != 8 && bits != 16) return 0;
    } else {
        return 0;
    }
    if (f->nChannels < 1 || f->nChannels > 2) return 0;
    if (f->nSamplesPerSec < 100 || f->nSamplesPerSec > 200000) return 0;
    if (f->nBlockAlign != f->nChannels * bits / 8) return 0;
    return 1;
}

sp_hresult sp_ds_create_sound_buffer(sp_ds *ds, const sp_dsbufferdesc *d, const sp_waveformatex *fmt,
                                     sp_dsbuf **out)
{
    if (!ds || !d || !out) return SP_DSERR_INVALIDPARAM;
    if (d->dwSize != sizeof *d && d->dwSize != 20) return SP_DSERR_INVALIDPARAM;
    sp_dsbuf *b = calloc(1, sizeof *b);
    if (!b) return SP_E_OUTOFMEMORY;
    atomic_init(&b->refs, 1);
    pthread_mutex_init(&b->lock, NULL);
    b->ds = ds; sp_ds_add_ref(ds);
    b->flags = d->dwFlags;
    if (d->dwFlags & DSBCAPS_PRIMARYBUFFER) {
        if (d->dwBufferBytes || fmt) { pthread_mutex_destroy(&b->lock); sp_ds_release(ds); free(b); return SP_DSERR_INVALIDPARAM; }
        b->primary = 1;
        b->fmt = (sp_waveformatex){WAVE_FORMAT_PCM, 2, 22050, 22050 * 2, 2, 8, 0};  /* DirectSound's default */
        b->block = 2; b->frames = 0; b->bytes = 0;
        *out = b;
        return SP_S_OK;
    }
    if (!valid_format(fmt)) { pthread_mutex_destroy(&b->lock); sp_ds_release(ds); free(b); return SP_DSERR_BADFORMAT; }
    if (d->dwBufferBytes < 4 || d->dwBufferBytes > 0x0FFFFFFF || d->dwBufferBytes % fmt->nBlockAlign) {
        pthread_mutex_destroy(&b->lock); sp_ds_release(ds); free(b); return SP_DSERR_INVALIDPARAM;
    }
    b->fmt = *fmt;
    b->fmt.cbSize = 0;
    if (fmt->wFormatTag == WAVE_FORMAT_EXTENSIBLE) b->fmt.wFormatTag = WAVE_FORMAT_PCM;
    b->block = fmt->nBlockAlign;
    b->bytes = d->dwBufferBytes;
    b->frames = b->bytes / b->block;
    b->data = g_alloc.alloc(g_alloc.ctx, b->bytes);
    if (!b->data) { pthread_mutex_destroy(&b->lock); sp_ds_release(ds); free(b); return SP_E_OUTOFMEMORY; }
    *out = b;
    return SP_S_OK;
}

/* ---- IDirectSoundBuffer --------------------------------------------------------------------- */
uint32_t sp_dsbuf_add_ref(sp_dsbuf *b) { return atomic_fetch_add(&b->refs, 1) + 1; }

uint32_t sp_dsbuf_release(sp_dsbuf *b)
{
    uint32_t r = atomic_fetch_sub(&b->refs, 1) - 1;
    if (r) return r;
    sp_dsbuf_stop(b);
    if (b->bopen && b->ds->backend) b->ds->backend->close(b->bh);
    if (b->data) g_alloc.free(g_alloc.ctx, b->data);
    pthread_mutex_destroy(&b->lock);
    sp_ds_release(b->ds);
    free(b);
    return 0;
}

sp_hresult sp_dsbuf_get_caps(sp_dsbuf *b, sp_dsbcaps *c)
{
    if (!c || c->dwSize != sizeof *c) return SP_DSERR_INVALIDPARAM;
    c->dwFlags = b->flags | DSBCAPS_LOCSOFTWARE;
    c->dwBufferBytes = b->bytes;
    c->dwUnlockTransferRate = 0;
    c->dwPlayCpuOverhead = 0;
    return SP_S_OK;
}

sp_hresult sp_dsbuf_get_format(sp_dsbuf *b, sp_waveformatex *f, uint32_t size, uint32_t *written)
{
    if (written) *written = sizeof *f;
    if (!f) return written ? SP_S_OK : SP_DSERR_INVALIDPARAM;
    if (size < sizeof *f) return SP_DSERR_INVALIDPARAM;
    *f = b->fmt;
    return SP_S_OK;
}

sp_hresult sp_dsbuf_set_format(sp_dsbuf *b, const sp_waveformatex *f)
{
    if (!b->primary) return SP_DSERR_INVALIDCALL;
    if (!valid_format(f)) return SP_DSERR_BADFORMAT;
    b->fmt = *f; b->fmt.cbSize = 0;
    b->block = f->nBlockAlign;
    return SP_S_OK;
}

sp_hresult sp_dsbuf_get_volume(sp_dsbuf *b, int32_t *v) { if (!v) return SP_DSERR_INVALIDPARAM; *v = b->volume; return SP_S_OK; }

sp_hresult sp_dsbuf_set_volume(sp_dsbuf *b, int32_t v)
{
    if (v < DSBVOLUME_MIN || v > DSBVOLUME_MAX) return SP_DSERR_INVALIDPARAM;
    b->volume = v;
    if (b->bopen && b->ds->backend && b->ds->backend->set_volume) {
        float gain = v <= DSBVOLUME_MIN ? 0.0f : __builtin_powf(10.0f, (float)v / 2000.0f);
        b->ds->backend->set_volume(b->bh, gain);
    }
    return SP_S_OK;
}

sp_hresult sp_dsbuf_get_status(sp_dsbuf *b, uint32_t *s)
{
    if (!s) return SP_DSERR_INVALIDPARAM;
    *s = 0;
    if (atomic_load(&b->playing)) *s = DSBSTATUS_PLAYING | (atomic_load(&b->looping) ? DSBSTATUS_LOOPING : 0);
    return SP_S_OK;
}

/* Take a consistent copy of the history. Returns the number of valid entries (<= HIST), oldest
 * first in out[]. */
static uint32_t snapshot(sp_dsbuf *b, hent *out)
{
    for (;;) {
        uint32_t s1 = atomic_load_explicit(&b->seq, memory_order_acquire);
        if (s1 & 1) continue;
        uint32_t cnt = atomic_load_explicit(&b->hcount, memory_order_relaxed);
        uint32_t n = cnt < HIST ? cnt : HIST;
        for (uint32_t i = 0; i < n; i++) out[i] = b->hist[(cnt - n + i) % HIST];
        atomic_thread_fence(memory_order_acquire);
        if (atomic_load_explicit(&b->seq, memory_order_relaxed) == s1) return n;
    }
}

/* Play position at host time t: ring frame index and unwrapped total. */
static int play_at(sp_dsbuf *b, uint64_t t, uint32_t *ring, uint64_t *tot)
{
    hent h[HIST];
    uint32_t n = snapshot(b, h);
    if (!n) return 0;
    int k = -1;
    for (int i = (int)n - 1; i >= 0; i--) if (h[i].host_ns <= t) { k = i; break; }
    if (k < 0) { *ring = h[0].pos; *tot = h[0].total; return 1; }    /* nothing audible yet */
    uint64_t off;
    uint64_t dt = t - h[k].host_ns;
    if (k + 1 < (int)n && h[k + 1].host_ns > h[k].host_ns) {
        uint64_t span = h[k + 1].host_ns - h[k].host_ns;
        off = dt >= span ? h[k].n : (dt * h[k].n + span / 2) / span;
    } else {
        off = (dt * b->fmt.nSamplesPerSec + 500000000ull) / 1000000000ull;
        if (off > h[k].n) off = h[k].n;
    }
    *ring = (uint32_t)((h[k].pos + off) % b->frames);
    *tot = h[k].total + off;
    return 1;
}

uint64_t sp_dsbuf_play_frames_at(sp_dsbuf *b, uint64_t t)
{
    uint32_t r; uint64_t tot = 0;
    if (!play_at(b, t, &r, &tot)) return 0;
    return tot;
}

sp_hresult sp_dsbuf_get_current_position(sp_dsbuf *b, uint32_t *play, uint32_t *write)
{
    if (b->primary) { if (play) *play = 0; if (write) *write = 0; return SP_S_OK; }
    uint32_t p, w;
    if (!atomic_load(&b->playing)) {
        p = w = atomic_load(&b->frozen_pos);
    } else {
        uint64_t tot;
        uint32_t rp = atomic_load_explicit(&b->read_pos, memory_order_acquire);
        if (!play_at(b, sp_host_now_ns(), &p, &tot)) p = rp;
        w = (rp + atomic_load(&b->last_n)) % b->frames;
    }
    if (play) *play = p * b->block;
    if (write) *write = w * b->block;
    return SP_S_OK;
}

sp_hresult sp_dsbuf_lock(sp_dsbuf *b, uint32_t offset, uint32_t bytes, void **p1, uint32_t *b1,
                         void **p2, uint32_t *b2, uint32_t flags)
{
    if (b->primary) return SP_DSERR_PRIOLEVELNEEDED;
    if (!p1 || !b1) return SP_DSERR_INVALIDPARAM;
    if (flags & DSBLOCK_ENTIREBUFFER) { offset = 0; bytes = b->bytes; }
    else if (flags & DSBLOCK_FROMWRITECURSOR) sp_dsbuf_get_current_position(b, NULL, &offset);
    if (offset >= b->bytes || bytes == 0 || bytes > b->bytes) return SP_DSERR_INVALIDPARAM;
    uint32_t first = b->bytes - offset;
    if (first > bytes) first = bytes;
    *p1 = b->data + offset;
    *b1 = first;
    if (p2) *p2 = (bytes > first) ? b->data : NULL;
    if (b2) *b2 = (bytes > first) ? bytes - first : 0;
    return SP_S_OK;
}

sp_hresult sp_dsbuf_unlock(sp_dsbuf *b, void *p1, uint32_t b1, void *p2, uint32_t b2)
{
    uint8_t *lo = b->data, *hi = b->data + b->bytes;
    if (p1 && ((uint8_t *)p1 < lo || (uint8_t *)p1 + b1 > hi)) return SP_DSERR_INVALIDPARAM;
    if (p2 && b2 && ((uint8_t *)p2 < lo || (uint8_t *)p2 + b2 > hi)) return SP_DSERR_INVALIDPARAM;
    atomic_thread_fence(memory_order_release);   /* publish the game's writes to the audio thread */
    return SP_S_OK;
}

static void clear_history(sp_dsbuf *b)
{
    atomic_fetch_add_explicit(&b->seq, 1, memory_order_acq_rel);
    atomic_store_explicit(&b->hcount, 0, memory_order_relaxed);
    atomic_fetch_add_explicit(&b->seq, 1, memory_order_release);
}

sp_hresult sp_dsbuf_play(sp_dsbuf *b, uint32_t reserved, uint32_t priority, uint32_t flags)
{
    if (reserved) return SP_DSERR_INVALIDPARAM;
    (void)priority;
    if (b->primary) return SP_S_OK;      /* the primary buffer always "plays" in software mixing */
    pthread_mutex_lock(&b->lock);
    atomic_store(&b->looping, (flags & DSBPLAY_LOOPING) != 0);
    if (!atomic_load(&b->playing)) {
        const sp_audio_backend *be = b->ds->backend;
        if (be && !b->bopen) {
            if (be->open(b, &b->fmt, &b->bh) != 0) { pthread_mutex_unlock(&b->lock); return SP_DSERR_NODRIVER; }
            b->bopen = 1;
            if (b->volume) sp_dsbuf_set_volume(b, b->volume);
        }
        atomic_store(&b->read_pos, atomic_load(&b->frozen_pos));
        atomic_store(&b->last_n, 0);
        clear_history(b);
        atomic_store_explicit(&b->playing, 1, memory_order_release);
        if (be && be->start(b->bh) != 0) {
            atomic_store(&b->playing, 0);
            pthread_mutex_unlock(&b->lock);
            return SP_DSERR_NODRIVER;
        }
    }
    pthread_mutex_unlock(&b->lock);
    return SP_S_OK;
}

sp_hresult sp_dsbuf_stop(sp_dsbuf *b)
{
    if (b->primary) return SP_S_OK;
    pthread_mutex_lock(&b->lock);
    if (atomic_load(&b->playing)) {
        uint32_t p; uint64_t tot;
        if (!play_at(b, sp_host_now_ns(), &p, &tot)) p = atomic_load(&b->read_pos);
        atomic_store(&b->playing, 0);
        if (b->bopen && b->ds->backend) b->ds->backend->stop(b->bh);
        atomic_store(&b->frozen_pos, p);
        atomic_store(&b->read_pos, p);
        clear_history(b);
    }
    pthread_mutex_unlock(&b->lock);
    return SP_S_OK;
}

sp_hresult sp_dsbuf_set_current_position(sp_dsbuf *b, uint32_t offset)
{
    if (b->primary) return SP_DSERR_INVALIDCALL;
    if (offset >= b->bytes) return SP_DSERR_INVALIDPARAM;
    pthread_mutex_lock(&b->lock);
    uint32_t f = offset / b->block;
    atomic_store(&b->frozen_pos, f);
    atomic_store(&b->read_pos, f);
    clear_history(b);
    pthread_mutex_unlock(&b->lock);
    return SP_S_OK;
}

/* ---- render (audio thread) ------------------------------------------------------------------ */
void sp_dsbuf_render(sp_dsbuf *b, void *out, uint32_t nframes, uint64_t out_host_ns)
{
    atomic_thread_fence(memory_order_acquire);
    uint8_t *dst = out;
    uint32_t blk = b->block;
    if (!atomic_load_explicit(&b->playing, memory_order_acquire)) {
        memset(dst, blk == 1 ? 0x80 : 0, (size_t)nframes * blk);   /* 8-bit PCM silence is 0x80 */
        return;
    }
    uint32_t pos = atomic_load_explicit(&b->read_pos, memory_order_relaxed);
    uint64_t total = atomic_load_explicit(&b->total, memory_order_relaxed);
    if (atomic_load_explicit(&b->dbg_enabled, memory_order_relaxed) &&
        total + nframes > atomic_load_explicit(&b->dbg_written, memory_order_acquire))
        atomic_fetch_add_explicit(&b->underruns, 1, memory_order_relaxed);

    uint32_t done = 0, p = pos;
    int looping = atomic_load_explicit(&b->looping, memory_order_relaxed);
    while (done < nframes) {
        uint32_t chunk = b->frames - p;
        if (chunk > nframes - done) chunk = nframes - done;
        memcpy(dst + (size_t)done * blk, b->data + (size_t)p * blk, (size_t)chunk * blk);
        done += chunk;
        p += chunk;
        if (p == b->frames) {
            p = 0;
            if (!looping) {
                memset(dst + (size_t)done * blk, blk == 1 ? 0x80 : 0, (size_t)(nframes - done) * blk);
                atomic_store(&b->playing, 0);
                atomic_store(&b->frozen_pos, 0);
                break;
            }
        }
    }

    /* publish the timestamp for this block */
    uint32_t cnt = atomic_load_explicit(&b->hcount, memory_order_relaxed);
    atomic_fetch_add_explicit(&b->seq, 1, memory_order_acq_rel);
    b->hist[cnt % HIST] = (hent){out_host_ns, total, pos, nframes};
    atomic_store_explicit(&b->hcount, cnt + 1, memory_order_relaxed);
    atomic_fetch_add_explicit(&b->seq, 1, memory_order_release);

    atomic_store_explicit(&b->total, total + nframes, memory_order_relaxed);
    atomic_store_explicit(&b->read_pos, p, memory_order_release);
    atomic_store_explicit(&b->last_n, nframes, memory_order_relaxed);
    if (nframes > atomic_load_explicit(&b->max_n, memory_order_relaxed))
        atomic_store_explicit(&b->max_n, nframes, memory_order_relaxed);
    atomic_fetch_add_explicit(&b->callbacks, 1, memory_order_relaxed);
}

void sp_dsbuf_get_stats(sp_dsbuf *b, sp_dsbuf_stats *s)
{
    s->frames_pulled = atomic_load(&b->total);
    s->callbacks = atomic_load(&b->callbacks);
    s->underruns = atomic_load(&b->underruns);
    s->last_callback_frames = atomic_load(&b->last_n);
    s->max_callback_frames = atomic_load(&b->max_n);
}

void sp_dsbuf_debug_set_written(sp_dsbuf *b, uint64_t total)
{
    atomic_store_explicit(&b->dbg_written, total, memory_order_release);
    atomic_store(&b->dbg_enabled, 1);
}

uint8_t *sp_dsbuf_data(sp_dsbuf *b) { return b->data; }
