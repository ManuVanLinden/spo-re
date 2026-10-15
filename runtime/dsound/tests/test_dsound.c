/* test_dsound.c -- host-only tests of the guest DirectSound objects. Built twice: silent (no platform
 * audio linked) and -DWITH_SP (runtime/platform/macos's sp_dsound.c behind a fake output device that the
 * test pulls from, standing in for the CoreAudio render callback). */
#include <stdatomic.h>
#include <stdlib.h>
#include <time.h>

#include "com_harness.h"
#include "sp_dsound.h"

extern void sg_imp_dsound_ord1(sg_ctx *ctx, uint8_t *mem);
extern void sg_imp_dsound_ord9(sg_ctx *ctx, uint8_t *mem);
uint32_t dsound_create(uint8_t *mem, int is8, uint32_t *hr);
int dsound_backend_is_coreaudio(uint32_t guest_ds, const uint8_t *mem);

#ifdef WITH_SP
static sp_dsbuf *opened;
static int fake_open(sp_dsbuf *b, const sp_waveformatex *f, void **h) { (void)f; opened = b; *h = b; return 0; }
static int fake_ok(void *h) { (void)h; return 0; }
static void fake_close(void *h) { (void)h; }
static int fake_vol(void *h, float g) { (void)h; (void)g; return 0; }
static const sp_audio_backend fake = {fake_open, fake_ok, fake_ok, fake_close, fake_vol};
const sp_audio_backend *sp_coreaudio_backend(void) { return &fake; }
#endif

static const uint8_t DEFAULT_PLAYBACK[16] = {0x00, 0x00, 0xf0, 0xde, 0x6d, 0x9c, 0xed, 0x47, 0xaa, 0xf1, 0x4d, 0xda, 0x8f, 0x2b, 0x5c, 0x03};
static const sg_guid IID_DSB8 = {0x6825a449, 0x7524, 0x4d82, {0x92, 0x0f, 0x50, 0xe3, 0x6a, 0xb3, 0xab, 0x1e}};
static const sg_guid IID_NOTIFY = {0xb0210783, 0x89cd, 0x11d0, {0xaf, 0x08, 0x00, 0xa0, 0xc9, 0x25, 0xcd, 0x16}};
static const sg_guid IID_DS8 = {0xc50a7e93, 0xf395, 0x4834, {0x9e, 0xf6, 0x7f, 0xa9, 0x9d, 0xe5, 0x09, 0x66}};
static uint32_t put_guid(const sg_guid *g) {
    uint32_t a = gscratch(16);
    memcpy(M + a, &g->d1, 4); memcpy(M + a + 4, &g->d2, 2); memcpy(M + a + 6, &g->d3, 2); memcpy(M + a + 8, g->d4, 8);
    return a;
}
static uint32_t imp(void (*fn)(sg_ctx *, uint8_t *), unsigned n, uint32_t a0, uint32_t a1, uint32_t a2) {
    sg_ctx *ctx = &T;
    uint8_t *mem = M;
    uint32_t esp0 = T.esp, a[3] = {a0, a1, a2};
    for (unsigned i = n; i-- > 0;) SG_PUSH32(a[i]);
    SG_CALL(fn, 0x0113d9c5u);
    CHECK_EQ(T.esp, esp0);
    return T.eax;
}
/* The core event API stand-in: records sg_core_event_set calls from dsound's notifier thread. */
#define DSBPN_OFFSETSTOP_ 0xffffffffu
static _Atomic unsigned ev_sets[16];
int sg_core_event_set(uint32_t h) {
    if (h >= 0x5000u && h < 0x5040u) ev_sets[(h - 0x5000u) / 4u]++;
    return 1;
}
static unsigned ev_count(uint32_t h) { return ev_sets[(h - 0x5000u) / 4u]; }
static void ev_reset(void) { for (int i = 0; i < 16; i++) ev_sets[i] = 0; }
static void sleep_ms(int ms) { struct timespec ts = {0, ms * 1000000L}; nanosleep(&ts, NULL); }
static uint32_t make_desc(uint32_t flags, uint32_t bytes, uint32_t wfx) {
    uint32_t d = gscratch(36);
    gst(d, 36); gst(d + 4, flags); gst(d + 8, bytes); gst(d + 16, wfx);
    return d;
}

int main(void) {
    harness_init();
    unsigned live0 = sg_com_live_objects();
    uint32_t src = gscratch(16), dev = gscratch(16), out = gscratch(4), out2 = gscratch(4);
    memcpy(M + src, DEFAULT_PLAYBACK, 16);
    /* the game's sequence (0x113d9a0): GetDeviceID(DSDEVID_DefaultPlayback), DirectSoundCreate(&guid) */
    CHECK_EQ(imp(sg_imp_dsound_ord9, 2, src, dev, 0), 0);
    CHECK(memcmp(M + dev, DEFAULT_PLAYBACK, 16) != 0);
    CHECK_EQ(imp(sg_imp_dsound_ord1, 3, dev, out, 0), 0);
    uint32_t ds = gld(out);
    CHECK(ds != 0);
#ifdef WITH_SP
    CHECK(dsound_backend_is_coreaudio(ds, M));
#else
    CHECK(!dsound_backend_is_coreaudio(ds, M));
#endif
    CHECK_EQ(imp(sg_imp_dsound_ord1, 3, dev, out, 1), 0x80040110u); /* no aggregation */
    /* CreateSoundBuffer before SetCooperativeLevel fails, as in DirectSound */
    uint32_t pdesc = make_desc(DSBCAPS_PRIMARYBUFFER, 0, 0);
    CHECK_EQ(gcall(ds, 3, 3, pdesc, out, 0), 0x88780032u);
    CHECK_EQ(gcall(ds, 6, 2, 0x00c0ffee, DSSCL_PRIORITY), 0);
    uint32_t caps = gscratch(96);
    gst(caps, 96);
    CHECK_EQ(gcall(ds, 4, 1, caps), 0);
    CHECK(gld(caps + 4) & DSCAPS_PRIMARYSTEREO);
    CHECK(gld(caps + 12) >= 44100);                      /* dwMaxSecondarySampleRate */
    /* primary buffer + SetFormat (44.1 kHz 16-bit stereo) */
    CHECK_EQ(gcall(ds, 3, 3, pdesc, out, 0), 0);
    uint32_t prim = gld(out), wfx = gscratch(20);
    sp_waveformatex f = {WAVE_FORMAT_PCM, 2, 44100, 176400, 4, 16, 0};
    memcpy(M + wfx, &f, sizeof f);
    CHECK_EQ(gcall(prim, 14, 1, wfx), 0);
    uint32_t fmtout = gscratch(20);
    CHECK_EQ(gcall(prim, 5, 3, fmtout, 18, out2), 0);
    CHECK_EQ(gld(fmtout + 4), 44100);
    CHECK_EQ(gcall(prim, 11, 7, 0, 0, out, out2, 0, 0, DSBLOCK_ENTIREBUFFER), 0x88780046u);
    /* the stream buffer: 22,272 frames = 89,088 bytes, flags 0x18000 */
    uint32_t sdesc = make_desc(0x18000, 89088, wfx);
    CHECK_EQ(gcall(ds, 3, 3, sdesc, out, 0), 0);
    uint32_t sb = gld(out), p1 = gscratch(4), b1 = gscratch(4), p2 = gscratch(4), b2 = gscratch(4);
    uint32_t bc = gscratch(20);
    gst(bc, 20);
    CHECK_EQ(gcall(sb, 3, 1, bc), 0);
    CHECK_EQ(gld(bc + 8), 89088);
    /* Start (0x113da60): Lock(0, whole) + memset + Unlock + Play(LOOPING) */
    CHECK_EQ(gcall(sb, 11, 7, 0, 89088, p1, b1, p2, b2, 0), 0);
    CHECK(gld(p1) >= 0x20000000u && gld(p1) < 0x30000000u); /* guest memory */
    CHECK_EQ(gld(b1), 89088);
    CHECK_EQ(gld(b2), 0);
    memset(M + gld(p1), 0, 89088);
    CHECK_EQ(gcall(sb, 19, 4, gld(p1), gld(b1), gld(p2), gld(b2)), 0);
    CHECK_EQ(gcall(sb, 12, 3, 0, 0, DSBPLAY_LOOPING), 0);
    CHECK_EQ(gcall(sb, 9, 1, out), 0);
    CHECK(gld(out) & DSBSTATUS_PLAYING);
    /* the play cursor advances with playback */
    uint32_t play0 = gscratch(4), write0 = gscratch(4);
    CHECK_EQ(gcall(sb, 4, 2, play0, write0), 0);
#ifdef WITH_SP
    CHECK(opened != NULL);
    static int16_t sink[4096 * 2];
    for (int i = 0; i < 8; i++) sp_dsbuf_render(opened, sink, 512, sp_host_now_ns());
    sleep_ms(20);
#else
    sleep_ms(60);
#endif
    CHECK_EQ(gcall(sb, 4, 2, out, out2), 0);
    uint32_t moved = (gld(out) + 89088 - gld(play0)) % 89088;
    CHECK(moved >= 2048 && moved < 89088 / 2);
    CHECK_EQ(gld(out) % 4, 0);
    CHECK(gld(out2) != gld(out));                        /* the write cursor leads the play cursor */
    /* the Dac thread's 256-frame blocks: Lock(offset, 1024); wrap-around at the end */
    CHECK_EQ(gcall(sb, 11, 7, 89088 - 512, 1024, p1, b1, p2, b2, 0), 0);
    CHECK_EQ(gld(b1), 512);
    CHECK_EQ(gld(b2), 512);
    CHECK_EQ(gcall(sb, 19, 4, gld(p1), gld(b1), gld(p2), gld(b2)), 0);
    CHECK_EQ(gcall(sb, 11, 7, 89088, 1024, p1, b1, p2, b2, 0), 0x80070057u);
    /* controls */
    CHECK_EQ(gcall(sb, 15, 1, (uint32_t)-600), 0);
    CHECK_EQ(gcall(sb, 6, 1, out), 0);
    CHECK_EQ(gld(out), (uint32_t)-600);
    CHECK_EQ(gcall(sb, 15, 1, 5), 0x80070057u);
    CHECK_EQ(gcall(sb, 17, 1, 22050), 0);
    CHECK_EQ(gcall(sb, 8, 1, out), 0);
    CHECK_EQ(gld(out), 22050);
    CHECK_EQ(gcall(sb, 16, 1, (uint32_t)-2000), 0);
    CHECK_EQ(gcall(sb, 7, 1, out), 0);
    CHECK_EQ(gld(out), (uint32_t)-2000);
    CHECK_EQ(gcall(sb, 18, 0), 0);                     /* Stop */
    CHECK_EQ(gcall(sb, 9, 1, out), 0);
    CHECK_EQ(gld(out) & DSBSTATUS_PLAYING, 0);
    CHECK_EQ(gcall(sb, 13, 1, 4000), 0);               /* SetCurrentPosition */
    CHECK_EQ(gcall(sb, 4, 2, out, 0), 0);
    CHECK_EQ(gld(out), 4000);
    /* IDirectSoundBuffer8 by QI; IDirectSoundNotify only with CTRLPOSITIONNOTIFY */
    CHECK_EQ(gcall(sb, 0, 2, put_guid(&IID_DSB8), out), 0);
    uint32_t sb8 = gld(out);
    CHECK(sb8 && sb8 != sb);
    CHECK_EQ(gcall(sb8, 21, 3, 0, 0, 0), 0);           /* SetFX(0, ...) */
    CHECK_EQ(gcall(sb8, 2, 0), 1);
    CHECK_EQ(gcall(sb, 0, 2, put_guid(&IID_NOTIFY), out), SG_E_NOINTERFACE);
    uint32_t ndesc = make_desc(0x100 | 0x8000, 8192, wfx);
    CHECK_EQ(gcall(ds, 3, 3, ndesc, out, 0), 0);
    uint32_t nb = gld(out);
    CHECK_EQ(gcall(nb, 0, 2, put_guid(&IID_NOTIFY), out), 0);
    uint32_t nf = gld(out), pos = gscratch(16);
    gst(pos, 4096); gst(pos + 4, 0x1234); gst(pos + 8, 0xffffffffu); gst(pos + 12, 0x1238);
    CHECK_EQ(gcall(nf, 3, 2, 2, pos), 0);
    CHECK_EQ(gcall(nf, 2, 0), 1);
    CHECK_EQ(gcall(nb, 2, 0), 0);
    /* IDirectSoundNotify signalling: the notifier thread sets the events through
     * sg_core_event_set (recorded below); buffer of 17,640 bytes = 100 ms at 44.1 kHz stereo */
    {
        enum { E0 = 0x5000, E1 = 0x5004, ES = 0x5008 };
        uint32_t d2 = make_desc(0x100 | 0x8000, 17640, wfx);
        CHECK_EQ(gcall(ds, 3, 3, d2, out, 0), 0);
        uint32_t b2b = gld(out);
        CHECK_EQ(gcall(b2b, 0, 2, put_guid(&IID_NOTIFY), out), 0);
        uint32_t n2 = gld(out), ps = gscratch(24);
        gst(ps, 17640); gst(ps + 4, E0);
        CHECK_EQ(gcall(n2, 3, 2, 1, ps), 0x80070057u);   /* offset beyond the buffer */
        gst(ps, 0); gst(ps + 4, E0); gst(ps + 8, 8820); gst(ps + 12, E1); gst(ps + 16, DSBPN_OFFSETSTOP_); gst(ps + 20, ES);
        CHECK_EQ(gcall(n2, 3, 2, 3, ps), 0);
        ev_reset();
        CHECK_EQ(gcall(b2b, 12, 3, 0, 0, DSBPLAY_LOOPING), 0);
        CHECK_EQ(gcall(n2, 3, 2, 3, ps), 0x88780032u);   /* not while playing */
#ifdef WITH_SP
        /* the cursor moves only as the fake device pulls: 2,400 frames (9,600 bytes) passes 8,820 */
        sp_dsbuf *mine = opened;
        static int16_t sink2[2400 * 2];
        sp_dsbuf_render(mine, sink2, 2400, sp_host_now_ns());
        sleep_ms(90);
        CHECK_EQ(ev_count(E1), 1);
        CHECK_EQ(ev_count(E0), 0);
        sp_dsbuf_render(mine, sink2, 2400, sp_host_now_ns()); /* 19,200 bytes: wraps past 0 */
        sleep_ms(90);
        CHECK_EQ(ev_count(E0), 1);
        CHECK_EQ(ev_count(ES), 0);
        CHECK_EQ(gcall(b2b, 18, 0), 0);                    /* Stop: DSBPN_OFFSETSTOP */
        CHECK_EQ(ev_count(ES), 1);
#else
        sleep_ms(255);                                     /* passes 8820 at 50, 150, 250 ms; 0 at 100, 200 */
        CHECK(ev_count(E1) >= 2 && ev_count(E1) <= 3);
        CHECK(ev_count(E0) >= 2 && ev_count(E0) <= 3);
        CHECK_EQ(ev_count(ES), 0);
        CHECK_EQ(gcall(b2b, 18, 0), 0);                    /* Stop: DSBPN_OFFSETSTOP */
        CHECK_EQ(ev_count(ES), 1);
        sleep_ms(30);
        unsigned e1 = ev_count(E1);
        CHECK_EQ(ev_count(ES), 1);                         /* stopped: nothing more */
        /* a one-shot buffer: 8820 once, then its end sets the stop event */
        ev_reset();
        CHECK_EQ(gcall(b2b, 13, 1, 0), 0);
        CHECK_EQ(gcall(b2b, 12, 3, 0, 0, 0), 0);
        sleep_ms(170);
        CHECK_EQ(gcall(b2b, 9, 1, out), 0);
        CHECK_EQ(gld(out) & DSBSTATUS_PLAYING, 0);
        CHECK_EQ(ev_count(E1), 1);
        CHECK_EQ(ev_count(ES), 1);
        CHECK_EQ(gcall(b2b, 18, 0), 0);                    /* Stop after the end: no second stop event */
        CHECK_EQ(ev_count(ES), 1);
        /* SetCurrentPosition jumps over positions without setting them */
        ev_reset();
        CHECK_EQ(gcall(b2b, 13, 1, 4000), 0);
        CHECK_EQ(gcall(b2b, 12, 3, 0, 0, DSBPLAY_LOOPING), 0);
        CHECK_EQ(gcall(b2b, 13, 1, 12000), 0);             /* over 8820 */
        sleep_ms(20);                                       /* 12000 + ~3500 */
        CHECK_EQ(gcall(b2b, 18, 0), 0);
        CHECK_EQ(ev_count(E1), 0);
        CHECK_EQ(ev_count(ES), 1);
        /* count 0 clears the positions */
        CHECK_EQ(gcall(n2, 3, 2, 0, 0), 0);
        ev_reset();
        CHECK_EQ(gcall(b2b, 12, 3, 0, 0, DSBPLAY_LOOPING), 0);
        sleep_ms(120);
        CHECK_EQ(gcall(b2b, 18, 0), 0);
        CHECK_EQ(ev_count(E0) + ev_count(E1) + ev_count(ES), 0);
        (void)e1;
#endif
        CHECK_EQ(gcall(n2, 2, 0), 1);
        CHECK_EQ(gcall(b2b, 2, 0), 0);                     /* released while registered: unlinked */
        sleep_ms(20);
    }
    /* DuplicateSoundBuffer */
    CHECK_EQ(gcall(ds, 5, 2, sb, out), 0);
    uint32_t dup = gld(out);
    CHECK(dup && dup != sb);
    CHECK_EQ(gcall(dup, 3, 1, bc), 0);
    CHECK_EQ(gld(bc + 8), 89088);
    CHECK_EQ(gcall(dup, 2, 0), 0);
    CHECK_EQ(gcall(sb, 2, 0), 0);
    CHECK_EQ(gcall(prim, 2, 0), 0);
    CHECK_EQ(gcall(ds, 2, 0), 0);
    /* DirectSoundCreate8: IDirectSound8 with VerifyCertification, and IDirectSound by QI */
    uint32_t hr, ds8 = dsound_create(M, 1, &hr);
    CHECK(ds8 && hr == 0);
    CHECK_EQ(gcall(ds8, 11, 1, out), 0);
    CHECK_EQ(gcall(ds8, 0, 2, put_guid(&IID_DS8), out), 0);
    CHECK_EQ(gld(out), ds8);
    CHECK_EQ(gcall(ds8, 2, 0), 1);
    CHECK_EQ(gcall(ds8, 2, 0), 0);
    CHECK_EQ(sg_com_live_objects(), live0);
#ifdef WITH_SP
    return harness_report("runtime/dsound (sp_dsound back end)");
#else
    return harness_report("runtime/dsound (silent back end)");
#endif
}
