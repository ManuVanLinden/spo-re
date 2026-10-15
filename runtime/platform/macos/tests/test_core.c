/* test_core.c - headless unit tests for the platform layer (queue, input, key state, fake
 * DirectSound without a device). Our own test code only. */
#include "sp_dsound.h"
#include "sp_platform.h"

#include <pthread.h>
#include <stdio.h>
#include <stdlib.h>
#include <string.h>
#include <time.h>

static int g_fail, g_pass;
#define CHECK(c) do { if (c) g_pass++; else { g_fail++; printf("FAIL %s:%d: %s\n", __FILE__, __LINE__, #c); } } while (0)

static uint32_t g_now;
static uint32_t fake_now(void *c) { (void)c; return g_now; }
static int g_wake_calls;
static void fake_wake(void *c) { (void)c; __atomic_add_fetch(&g_wake_calls, 1, __ATOMIC_RELAXED); }

static uint32_t g_seen[64]; static int g_nseen;
static int32_t proc(void *u, sp_hwnd h, uint32_t m, uint32_t w, int32_t l)
{
    (void)u; (void)w; (void)l;
    if (g_nseen < 64) g_seen[g_nseen++] = m;
    if (m == WM_PAINT) { sp_paintstruct ps; sp_begin_paint(h, &ps); sp_end_paint(h, &ps); return 0; }
    if (m == 0x777) return (int32_t)(w * 2);
    return sp_def_window_proc(h, m, w, l);
}

static sp_hwnd setup(void)
{
    sp_host_hooks hk = {0};
    hk.now_ms = fake_now; hk.wake = fake_wake;
    sp_platform_init(&hk, NULL);
    sp_hwnd h = sp_window_register(proc, NULL, SP_STYLE_DBLCLKS);
    sp_window_rect_changed(h, 0, 0, 640, 480);
    sp_show_window(h, 1, 1);
    sp_validate_rect(h);
    g_nseen = 0;
    return h;
}

static void test_order(void)
{
    sp_hwnd h = setup();
    sp_msg m;
    sp_set_timer(h, 7, 100);
    g_now = 1000;                                   /* timer due */
    sp_invalidate_rect(h, 0);
    sp_post_quit_message(3);
    sp_input_key(0x00, SP_KEY_DOWN, (uint16_t[]){'a'}, 1, NULL, 0);
    sp_post_message(h, 0x500, 1, 2);
    uint32_t order[8]; int n = 0;
    while (n < 8 && sp_peek_message(&m, 0, 0, 0, PM_REMOVE)) {
        order[n++] = m.message;
        if (m.message == WM_PAINT) sp_validate_rect(h);
    }
    CHECK(n == 5);
    CHECK(order[0] == 0x500);
    CHECK(order[1] == WM_KEYDOWN);
    CHECK(order[2] == WM_QUIT);
    CHECK(order[3] == WM_PAINT);
    CHECK(order[4] == WM_TIMER);
    /* timer is coalesced and rescheduled from the time it was retrieved */
    g_now = 1050; CHECK(!sp_peek_message(&m, 0, 0, 0, PM_REMOVE));
    g_now = 1100; CHECK(sp_peek_message(&m, 0, 0, 0, PM_REMOVE) && m.message == WM_TIMER && m.wParam == 7);
    g_now = 5000; CHECK(sp_peek_message(&m, 0, 0, 0, PM_REMOVE) && m.message == WM_TIMER);
    CHECK(!sp_peek_message(&m, 0, 0, 0, PM_REMOVE));
    sp_kill_timer(h, 7);
}

static void test_filters(void)
{
    sp_hwnd h = setup();
    sp_msg m;
    sp_input_mouse_move(h, 10, 10);
    sp_input_key(0x01, SP_KEY_DOWN, (uint16_t[]){'s'}, 1, NULL, 0);
    /* the IME code's PeekMessageA(WM_KEYFIRST, WM_KEYLAST, PM_REMOVE) at 0x84a0e3 */
    CHECK(sp_peek_message(&m, 0, WM_KEYFIRST, WM_KEYLAST, PM_REMOVE) && m.message == WM_KEYDOWN && m.wParam == 'S');
    CHECK(!sp_peek_message(&m, 0, WM_KEYFIRST, WM_KEYLAST, PM_REMOVE));
    /* PM_NOREMOVE leaves the message; key state changes only on removal */
    CHECK(sp_peek_message(&m, 0, 0, 0, PM_NOREMOVE) && m.message == WM_MOUSEMOVE);
    CHECK(sp_queue_depth(1) == 1);
    CHECK(sp_peek_message(&m, 0, 0, 0, PM_REMOVE) && m.message == WM_MOUSEMOVE);
    /* WM_PAINT stays until validated, even with PM_REMOVE */
    sp_invalidate_rect(h, 0);
    CHECK(sp_peek_message(&m, 0, 0, 0, PM_REMOVE) && m.message == WM_PAINT);
    CHECK(sp_peek_message(&m, 0, 0, 0, PM_REMOVE) && m.message == WM_PAINT);
    sp_dispatch_message(&m);
    CHECK(!sp_peek_message(&m, 0, 0, 0, PM_REMOVE));
    /* WM_QUIT ignores the range filter */
    sp_post_quit_message(9);
    CHECK(sp_peek_message(&m, 0, WM_KEYFIRST, WM_KEYLAST, PM_REMOVE) && m.message == WM_QUIT && m.wParam == 9);
    /* hwnd -1: thread messages only */
    sp_post_message(h, 0x600, 0, 0);
    sp_post_message(0, 0x601, 0, 0);
    CHECK(sp_peek_message(&m, (sp_hwnd)-1, 0, 0, PM_REMOVE) && m.message == 0x601 && m.hwnd == 0);
    CHECK(sp_peek_message(&m, 0, 0, 0, PM_REMOVE) && m.message == 0x600);
    /* posting to a bad window fails; the 10,000-message limit holds */
    CHECK(!sp_post_message(0x12345, 1, 0, 0));
    int ok = 0;
    for (int i = 0; i < 10005; i++) ok += sp_post_message(h, 0x700, (uint32_t)i, 0);
    CHECK(ok == 10000);
    int got = 0;
    while (sp_peek_message(&m, 0, 0x700, 0x700, PM_REMOVE)) got++;
    CHECK(got == 10000);
}

/* ---- threads -------------------------------------------------------------------------------- */
#define NTHR 4
#define PER 50000
static sp_hwnd g_th;
static void *poster(void *arg)
{
    uintptr_t id = (uintptr_t)arg;
    for (uint32_t i = 0; i < PER; ) {
        if (sp_post_message(g_th, 0x800 + (uint32_t)id, i, 0)) i++;
        else { struct timespec ts = {0, 100000}; nanosleep(&ts, NULL); }   /* queue full: retry */
    }
    return NULL;
}
static void *sender(void *arg)
{
    (void)arg;
    int32_t r = sp_send_message(g_th, 0x777, 21, 0);
    return (void *)(intptr_t)r;
}

static void test_threads(void)
{
    g_th = setup();
    pthread_t t[NTHR];
    struct timespec a, b;
    clock_gettime(CLOCK_MONOTONIC, &a);
    for (uintptr_t i = 0; i < NTHR; i++) pthread_create(&t[i], NULL, poster, (void *)i);
    uint32_t next[NTHR] = {0};
    long total = 0, bad = 0;
    sp_msg m;
    while (total < (long)NTHR * PER) {
        if (!sp_peek_message(&m, 0, 0x800, 0x800 + NTHR - 1, PM_REMOVE)) continue;
        uint32_t id = m.message - 0x800;
        if (m.wParam != next[id]) bad++;
        next[id] = m.wParam + 1;
        total++;
    }
    clock_gettime(CLOCK_MONOTONIC, &b);
    for (int i = 0; i < NTHR; i++) pthread_join(t[i], NULL);
    double s = (double)(b.tv_sec - a.tv_sec) + (double)(b.tv_nsec - a.tv_nsec) / 1e9;
    printf("  cross-thread posts: %ld messages from %d threads in %.3f s (%.2f M msg/s), order violations %ld, wake calls %d\n",
           total, NTHR, s, (double)total / s / 1e6, bad, g_wake_calls);
    CHECK(bad == 0);
    CHECK(total == (long)NTHR * PER);
    /* cross-thread SendMessage is processed inside PeekMessage on the window thread */
    pthread_t st; void *res;
    pthread_create(&st, NULL, sender, NULL);
    for (int i = 0; i < 2000000 && sp_queue_depth(0) == 0; i++) {
        sp_peek_message(&m, 0, 0, 0, PM_REMOVE);
        struct timespec ts = {0, 10000}; nanosleep(&ts, NULL);
        if (g_nseen && g_seen[g_nseen - 1] == 0x777) break;
    }
    pthread_join(st, &res);
    CHECK((intptr_t)res == 42);
}

/* ---- input ---------------------------------------------------------------------------------- */
static void test_input(void)
{
    sp_hwnd h = setup();
    sp_msg m;
    /* coalescing: consecutive moves with the same wParam collapse; a button breaks the run */
    sp_input_mouse_move(h, 1, 1); sp_input_mouse_move(h, 2, 2); sp_input_mouse_move(h, 3, 3);
    CHECK(sp_queue_depth(1) == 1);
    sp_input_mouse_button(h, SP_MB_LEFT, 1, 3, 3, 1);
    sp_input_mouse_move(h, 4, 4); sp_input_mouse_move(h, 5, 5);
    CHECK(sp_queue_depth(1) == 3);
    while (sp_peek_message(&m, 0, WM_MOUSEFIRST, WM_MOUSELAST, PM_REMOVE)) {}
    CHECK(m.message == WM_MOUSEMOVE && m.wParam == MK_LBUTTON && m.lParam == 0x00050005);
    CHECK((uint16_t)sp_get_key_state(VK_LBUTTON) == 0xFF81);
    sp_input_mouse_button(h, SP_MB_LEFT, 0, 5, 5, 1);
    while (sp_peek_message(&m, 0, 0, 0, PM_REMOVE) && m.message != WM_PAINT) {}

    /* keymap: every (scan, ext) pair is unique across mapped non-modifier keys */
    int seen[512] = {0}, dups = 0, mapped = 0;
    sp_input_config cfg = {SP_CMD_AS_CONTROL, 1, 1};
    for (int k = 0; k < 128; k++) {
        sp_keyinfo ki;
        if (!sp_keymap_lookup((uint16_t)k, &cfg, &ki)) continue;
        mapped++;
        if (ki.vk_specific != ki.vk) continue;
        int idx = ki.scan | (ki.ext << 8);
        /* deliberate, as on Windows: Keypad Clear = scan 0x4C (Numpad 5 without NumLock),
         * JIS Eisu = scan 0x3A (it sits on the Caps Lock key) */
        if (seen[idx]++ && !(k == 0x57 || k == 0x47 || k == 0x66 || k == 0x39)) { dups++; printf("  duplicate scan code %02X ext %d (kVK %02X)\n", ki.scan, ki.ext, k); }
    }
    printf("  key map: %d mac key codes mapped\n", mapped);
    CHECK(mapped >= 110);
    CHECK(dups == 0);
    sp_input_config win = {SP_CMD_AS_WIN, 1, 1};
    sp_keyinfo ki;
    CHECK(sp_keymap_lookup(0x37, &win, &ki) && ki.vk == VK_LWIN && ki.ext == 1);

    /* Ctrl via physical Control and Cmd at once: one VK_CONTROL down/up pair */
    sp_input_modifier(0x3B, 1); sp_input_modifier(0x37, 1);
    sp_input_modifier(0x37, 0);
    CHECK((sp_get_async_key_state(VK_CONTROL) & 0x8000) != 0);
    sp_input_modifier(0x3B, 0);
    int downs = 0, ups = 0;
    while (sp_peek_message(&m, 0, WM_KEYFIRST, WM_KEYLAST, PM_REMOVE)) { downs += m.message == WM_KEYDOWN; ups += m.message == WM_KEYUP; }
    CHECK(downs == 1 && ups == 1);
    /* generic vs specific shift state */
    sp_input_modifier(0x38, 1); sp_input_modifier(0x3C, 1); sp_input_modifier(0x38, 0);
    while (sp_peek_message(&m, 0, WM_KEYFIRST, WM_KEYLAST, PM_REMOVE)) {}
    CHECK(sp_get_key_state(VK_SHIFT) < 0 && sp_get_key_state(VK_RSHIFT) < 0 && sp_get_key_state(VK_LSHIFT) >= 0);
    sp_input_modifier(0x3C, 0);
    while (sp_peek_message(&m, 0, WM_KEYFIRST, WM_KEYLAST, PM_REMOVE)) {}
    CHECK(sp_get_key_state(VK_SHIFT) >= 0);
    CHECK((sp_get_key_state(VK_NUMLOCK) & 1) == 1);
    /* Ctrl+[ gives ESC as WM_CHAR; F10 is a system key */
    sp_input_modifier(0x3B, 1);
    sp_input_key(0x21, SP_KEY_DOWN, (uint16_t[]){0x1b}, 1, NULL, 0);
    uint32_t ch = 0;
    while (sp_peek_message(&m, 0, 0, 0, PM_REMOVE) && m.message != WM_PAINT) {
        sp_translate_message(&m);
        if (m.message == WM_CHAR) ch = m.wParam;
    }
    CHECK(ch == 0x1B);
    sp_input_key(0x21, SP_KEY_UP, NULL, 0, NULL, 0);
    sp_input_modifier(0x3B, 0);
    sp_input_key(0x6D, SP_KEY_DOWN, NULL, 0, NULL, 0);
    while (sp_peek_message(&m, 0, WM_KEYFIRST, WM_KEYLAST, PM_REMOVE)) if (m.wParam == VK_F1 + 9) break;
    CHECK(m.message == WM_SYSKEYDOWN);
    sp_input_key(0x6D, SP_KEY_UP, NULL, 0, NULL, 0);
    /* deactivation forgets held keys but keeps toggles */
    sp_input_key(0x00, SP_KEY_DOWN, (uint16_t[]){'a'}, 1, NULL, 0);
    while (sp_peek_message(&m, 0, WM_KEYFIRST, WM_KEYLAST, PM_REMOVE)) {}
    CHECK(sp_get_key_state('A') < 0);
    sp_window_activate(h, 0, 1);
    CHECK(sp_get_key_state('A') >= 0 && sp_get_focus() == 0);
}

/* ---- DirectSound without a device ----------------------------------------------------------- */
static void test_dsound(void)
{
    sp_ds *ds;
    CHECK(sp_ds_create(NULL, &ds) == SP_S_OK);
    CHECK(sp_ds_set_cooperative_level(ds, 0x10001, DSSCL_PRIORITY) == SP_S_OK);
    sp_dscaps caps = {.dwSize = 0x60};
    CHECK(sizeof caps == 0x60 && sizeof(sp_dsbufferdesc) == 0x24 && sizeof(sp_msg) == 28);
    CHECK(sp_ds_get_caps(ds, &caps) == SP_S_OK && caps.dwMinSecondarySampleRate <= 44100 && caps.dwMaxSecondarySampleRate >= 44100);

    /* the game's probe (0x113d410): primary + SetFormat, then the secondary buffer */
    sp_dsbufferdesc pd = {0x24, DSBCAPS_PRIMARYBUFFER, 0, 0, 0, {0}};
    sp_dsbuf *prim;
    CHECK(sp_ds_create_sound_buffer(ds, &pd, NULL, &prim) == SP_S_OK);
    sp_waveformatex st = {WAVE_FORMAT_PCM, 2, 44100, 44100 * 4, 4, 16, 0};
    CHECK(sp_dsbuf_set_format(prim, &st) == SP_S_OK);
    sp_waveformatextensible six = {{WAVE_FORMAT_EXTENSIBLE, 6, 44100, 44100 * 12, 12, 16, 22}, 16, 0x3f,
                                   {0x01, 0, 0, 0, 0, 0, 0x10, 0, 0x80, 0, 0, 0xAA, 0, 0x38, 0x9B, 0x71}};
    CHECK(sp_dsbuf_set_format(prim, &six.Format) == SP_DSERR_BADFORMAT);
    uint32_t frames = (44100 * 500 / 1000 + 0xff) & ~0xffu;
    CHECK(frames == 22272);
    sp_dsbufferdesc sd = {0x24, 0x18000, frames * 4, 0, 0, {0}};
    sp_dsbuf *b;
    CHECK(sp_ds_create_sound_buffer(ds, &sd, &st, &b) == SP_S_OK);

    void *p1, *p2; uint32_t b1, b2;
    CHECK(sp_dsbuf_lock(b, 0, frames * 4, &p1, &b1, NULL, NULL, 0) == SP_S_OK && b1 == frames * 4);
    memset(p1, 0, b1);
    CHECK(sp_dsbuf_unlock(b, p1, b1, NULL, 0) == SP_S_OK);
    /* wrap-around: two regions when asked, truncated when ppvAudioPtr2 is NULL (as the Dac does) */
    CHECK(sp_dsbuf_lock(b, frames * 4 - 512, 1024, &p1, &b1, &p2, &b2, 0) == SP_S_OK && b1 == 512 && b2 == 512 && p2 == sp_dsbuf_data(b));
    CHECK(sp_dsbuf_lock(b, frames * 4 - 512, 1024, &p1, &b1, NULL, NULL, 0) == SP_S_OK && b1 == 512);
    CHECK(sp_dsbuf_lock(b, frames * 4, 4, &p1, &b1, NULL, NULL, 0) == SP_DSERR_INVALIDPARAM);
    CHECK(sp_dsbuf_lock(prim, 0, 4, &p1, &b1, NULL, NULL, 0) != SP_S_OK);

    /* pattern in, render out */
    int16_t *s = (int16_t *)sp_dsbuf_data(b);
    for (uint32_t i = 0; i < frames * 2; i++) s[i] = (int16_t)(i * 7);
    CHECK(sp_dsbuf_play(b, 0, 0, DSBPLAY_LOOPING) == SP_S_OK);
    uint32_t status = 0;
    sp_dsbuf_get_status(b, &status);
    CHECK(status == (DSBSTATUS_PLAYING | DSBSTATUS_LOOPING));

    /* drive the render callback by hand: 512-frame blocks, heard at H0 + k * 512/44100 s */
    const uint64_t H0 = 1000000000ull;
    int16_t out[512 * 2];
    uint64_t per = 512ull * 1000000000ull / 44100ull;
    int data_ok = 1;
    uint32_t blocks = 100;   /* 51,200 frames: wraps the 22,272-frame ring twice */
    for (uint32_t k = 0; k < blocks; k++) {
        sp_dsbuf_render(b, out, 512, H0 + k * per);
        for (uint32_t i = 0; i < 512 * 2; i++) {
            uint32_t f = (k * 512 + i / 2) % frames;
            if (out[i] != (int16_t)((f * 2 + (i & 1)) * 7)) data_ok = 0;
        }
    }
    CHECK(data_ok);
    /* interpolation between timestamps: exact at block starts, linear in between */
    CHECK(sp_dsbuf_play_frames_at(b, H0 + 90 * per) == 90 * 512);
    CHECK(sp_dsbuf_play_frames_at(b, H0 + 90 * per + per / 2) == 90 * 512 + 256);
    /* past the newest block: extrapolate at the nominal rate, at most one block */
    uint64_t pf = sp_dsbuf_play_frames_at(b, H0 + 99 * per + per / 4);
    CHECK(pf >= 99 * 512 + 127 && pf <= 99 * 512 + 129);
    CHECK(sp_dsbuf_play_frames_at(b, H0 + 200 * per) == 100 * 512);
    sp_dsbuf_stats stt; sp_dsbuf_get_stats(b, &stt);
    CHECK(stt.frames_pulled == 100 * 512 && stt.callbacks == 100);
    /* underrun accounting */
    sp_dsbuf_debug_set_written(b, 100 * 512 + 256);
    sp_dsbuf_render(b, out, 512, H0 + 100 * per);
    sp_dsbuf_get_stats(b, &stt);
    CHECK(stt.underruns == 1);
    /* Stop freezes the cursor; SetCurrentPosition moves it */
    CHECK(sp_dsbuf_stop(b) == SP_S_OK);
    uint32_t pp, ww;
    sp_dsbuf_get_current_position(b, &pp, &ww);
    CHECK(pp == ww && pp % 4 == 0);
    CHECK(sp_dsbuf_set_current_position(b, 4000) == SP_S_OK);
    sp_dsbuf_get_current_position(b, &pp, &ww);
    CHECK(pp == 4000);
    /* non-looping playback stops at the end and fills silence */
    sp_dsbuf_set_current_position(b, (frames - 100) * 4);
    sp_dsbuf_play(b, 0, 0, 0);
    sp_dsbuf_render(b, out, 512, H0);
    sp_dsbuf_get_status(b, &status);
    CHECK(status == 0 && out[2 * 100] == 0 && out[2 * 511 + 1] == 0);
    CHECK(sp_dsbuf_release(b) == 0);
    CHECK(sp_dsbuf_release(prim) == 0);
    CHECK(sp_ds_release(ds) == 0);
}

int main(void)
{
    printf("test_order\n"); test_order();
    printf("test_filters\n"); test_filters();
    printf("test_threads\n"); test_threads();
    printf("test_input\n"); test_input();
    printf("test_dsound\n"); test_dsound();
    printf("%d passed, %d failed\n", g_pass, g_fail);
    return g_fail != 0;
}
