/* test_gamepad.c -- the four XInput users (sp_gamepad_core.c) with a fake back end: injected host
 * states, the XINPUT_GAMEPAD mapping, packet numbers, connection order, a fifth pad, disconnects, rumble
 * forwarding, and a reader racing a writer. No frameworks, no window. */
#include <pthread.h>
#include <stdio.h>
#include <string.h>

#include "sp_macos.h"

static int t_pass, t_fail;
#define CHECK(c, ...) do { if (c) t_pass++; else { t_fail++; fprintf(stderr, "FAIL %s:%d ", __FILE__, __LINE__); \
        fprintf(stderr, __VA_ARGS__); fputc('\n', stderr); } } while (0)
#define CHECK_EQ(a, b) do { long long a_ = (long long)(a), b_ = (long long)(b); \
        CHECK(a_ == b_, "%s = %lld, expected %lld", #a, a_, b_); } while (0)

static int dev[6];   /* fake back-end keys */
#define DEV(i) ((void *)&dev[i])

static int rum_calls;
static void *rum_dev;
static uint16_t rum_lo, rum_hi;
static void fake_rumble(void *d, uint16_t lo, uint16_t hi) { rum_calls++; rum_dev = d; rum_lo = lo; rum_hi = hi; }

static sp_pad_caps caps(int rumble) {
    sp_pad_caps c;
    memset(&c, 0, sizeof c);
    c.subtype = 1;
    c.buttons = 0xf3ffu;
    c.triggers = c.sticks = 1;
    c.rumble = (uint8_t)rumble;
    strcpy(c.name, "fake");
    return c;
}

static void test_map(void) {
    sp_pad_input in;
    sp_pad_xstate x;
    memset(&in, 0, sizeof in);
    sp_pad_map(&in, &x);
    CHECK(x.buttons == 0 && x.lt == 0 && x.rt == 0 && x.lx == 0 && x.ly == 0 && x.rx == 0 && x.ry == 0, "rest is zero");
    in.lx = 1.0f; in.ly = -1.0f; in.rx = 0.5f; in.ry = -0.5f; in.lt = 1.0f; in.rt = 0.5f;
    sp_pad_map(&in, &x);
    CHECK_EQ(x.lx, 32767);
    CHECK_EQ(x.ly, -32768);
    CHECK_EQ(x.rx, 16384);     /* 0.5 * 32767 = 16383.5, rounds to even 16384 */
    CHECK_EQ(x.ry, -16384);
    CHECK_EQ(x.lt, 255);
    CHECK_EQ(x.rt, 128);       /* 127.5 -> 128 */
    in.lx = 3.0f; in.ly = -7.0f; in.lt = -1.0f; in.rt = 2.0f; in.rx = 0.0f / 0.0f;
    sp_pad_map(&in, &x);
    CHECK(x.lx == 32767 && x.ly == -32768 && x.lt == 0 && x.rt == 255 && x.rx == 0, "clamped (and NaN is 0)");
    in.ly = 0.25f;              /* up on the stick is +y in both GameController and XInput */
    sp_pad_map(&in, &x);
    CHECK(x.ly > 0, "up is positive (%d)", x.ly);
    in.buttons = 0xffffu;       /* guide 0x0400 and 0x0800 are never reported by XInputGetState */
    sp_pad_map(&in, &x);
    CHECK_EQ(x.buttons, 0xf3ffu);
    in.buttons = SP_PAD_A | SP_PAD_DPAD_LEFT | SP_PAD_START;
    sp_pad_map(&in, &x);
    CHECK_EQ(x.buttons, 0x1014u);
}

static void test_users(void) {
    uint32_t pk = 0, pk2 = 0;
    sp_pad_xstate x;
    sp_pad_input in;
    sp_pad_caps c = caps(0), got;
    sp_pad_reset();
    for (int u = 0; u < SP_PAD_USERS; u++) CHECK_EQ(sp_pad_get_state(u, &pk, &x), -1);
    CHECK_EQ(sp_pad_get_state(4, &pk, &x), -1);
    CHECK_EQ(sp_pad_get_state(-1, &pk, &x), -1);
    /* connection order: lowest free user */
    CHECK_EQ(sp_pad_connect(DEV(0), &c), 0);
    CHECK_EQ(sp_pad_connect(DEV(1), &c), 1);
    CHECK_EQ(sp_pad_connect(DEV(0), &c), 0);   /* again: same user */
    CHECK_EQ(sp_pad_connect(DEV(2), &c), 2);
    CHECK_EQ(sp_pad_connect(DEV(3), &c), 3);
    CHECK_EQ(sp_pad_connect(DEV(4), &c), -1);  /* fifth waits */
    CHECK_EQ(sp_pad_connect(NULL, &c), -1);
    CHECK_EQ(sp_pad_user_of(DEV(2)), 2);
    CHECK_EQ(sp_pad_user_of(DEV(4)), -1);
    /* a fresh pad is at rest */
    CHECK_EQ(sp_pad_get_state(1, &pk, &x), 0);
    CHECK(x.buttons == 0 && x.lx == 0 && x.lt == 0, "connected at rest");
    /* packet numbers: change only when the state does */
    memset(&in, 0, sizeof in);
    CHECK_EQ(sp_pad_update(DEV(1), &in), 0);
    sp_pad_get_state(1, &pk2, &x);
    CHECK_EQ(pk2, pk);
    in.buttons = SP_PAD_B;
    CHECK_EQ(sp_pad_update(DEV(1), &in), 1);
    sp_pad_get_state(1, &pk2, &x);
    CHECK(pk2 != pk, "packet moved on a change");
    CHECK_EQ(x.buttons, SP_PAD_B);
    pk = pk2;
    CHECK_EQ(sp_pad_update(DEV(1), &in), 0);
    sp_pad_get_state(1, &pk2, &x);
    CHECK_EQ(pk2, pk);
    in.lx = 1e-6f;               /* below one XInput step: same mapped state, same packet */
    CHECK_EQ(sp_pad_update(DEV(1), &in), 0);
    sp_pad_get_state(1, &pk2, NULL);
    CHECK_EQ(pk2, pk);
    in.lx = 0.01f;
    CHECK_EQ(sp_pad_update(DEV(1), &in), 1);
    /* the other users did not move */
    sp_pad_get_state(0, NULL, &x);
    CHECK_EQ(x.buttons, 0);
    CHECK_EQ(sp_pad_update(DEV(4), &in), 0);   /* a waiting pad has no user */
    /* disconnect frees the user; the next pad takes the lowest free one */
    CHECK_EQ(sp_pad_disconnect(DEV(1)), 1);
    CHECK_EQ(sp_pad_disconnect(DEV(1)), -1);
    CHECK_EQ(sp_pad_get_state(1, &pk, &x), -1);
    CHECK_EQ(sp_pad_get_caps(1, &got), -1);
    CHECK_EQ(sp_pad_connect(DEV(4), &c), 1);
    CHECK_EQ(sp_pad_get_state(1, &pk2, &x), 0);
    CHECK(x.buttons == 0 && x.lx == 0, "the new pad on user 1 starts at rest (old state cleared)");
    CHECK(pk2 != pk, "reconnect gives a new packet number");
    CHECK_EQ(sp_pad_get_caps(1, &got), 0);
    CHECK(!strcmp(got.name, "fake") && got.buttons == 0xf3ffu && got.sticks == 1, "caps kept");
    CHECK_EQ(sp_pad_disconnect(DEV(0)), 0);
    CHECK_EQ(sp_pad_connect(DEV(5), NULL), 0);
    sp_pad_reset();
    for (int u = 0; u < SP_PAD_USERS; u++) CHECK_EQ(sp_pad_get_state(u, NULL, NULL), -1);
}

static void test_rumble(void) {
    sp_pad_caps with = caps(1), without = caps(0);
    sp_pad_reset();
    sp_pad_set_rumble_handler(fake_rumble);
    rum_calls = 0;
    CHECK_EQ(sp_pad_set_vibration(0, 100, 200), -1);   /* nobody there */
    CHECK_EQ(rum_calls, 0);
    sp_pad_connect(DEV(0), &with);
    sp_pad_connect(DEV(1), &without);
    CHECK_EQ(sp_pad_set_vibration(0, 0x8000, 0x1000), 0);
    CHECK(rum_calls == 1 && rum_dev == DEV(0) && rum_lo == 0x8000 && rum_hi == 0x1000, "forwarded");
    CHECK_EQ(sp_pad_set_vibration(0, 0x8000, 0x1000), 0);
    CHECK_EQ(rum_calls, 1);                             /* same speeds: not again */
    CHECK_EQ(sp_pad_set_vibration(0, 0, 0), 0);
    CHECK(rum_calls == 2 && rum_lo == 0 && rum_hi == 0, "stop forwarded");
    CHECK_EQ(sp_pad_set_vibration(1, 0xffff, 0xffff), 0);   /* no haptics: accepted, dropped */
    CHECK_EQ(rum_calls, 2);
    CHECK_EQ(sp_pad_set_vibration(7, 1, 1), -1);
    sp_pad_set_rumble_handler(NULL);
    sp_pad_reset();
}

/* A writer flips between two states whose every field differs; the reader must never see a mix. */
static volatile int stop_writer;
static void *writer(void *arg) {
    sp_pad_input a, b;
    memset(&a, 0, sizeof a);
    memset(&b, 0, sizeof b);
    b.lx = b.ly = b.rx = b.ry = 1.0f;
    b.lt = b.rt = 1.0f;
    b.buttons = 0xf3ffu;
    for (unsigned i = 0; !stop_writer; i++) sp_pad_update(DEV(0), (i & 1) ? &b : &a);
    return NULL;
}
static void test_race(void) {
    pthread_t th;
    sp_pad_caps c = caps(0);
    unsigned torn = 0, seen_a = 0, seen_b = 0;
    uint32_t last = 0, pk, backwards = 0;
    sp_pad_reset();
    sp_pad_connect(DEV(0), &c);
    stop_writer = 0;
    pthread_create(&th, NULL, writer, NULL);
    for (int i = 0; i < 2000000; i++) {
        sp_pad_xstate x;
        sp_pad_get_state(0, &pk, &x);
        int a = x.buttons == 0 && x.lt == 0 && x.rt == 0 && x.lx == 0 && x.ly == 0 && x.rx == 0 && x.ry == 0;
        int b = x.buttons == 0xf3ffu && x.lt == 255 && x.rt == 255 && x.lx == 32767 && x.ly == 32767 && x.rx == 32767 &&
                x.ry == 32767;
        if (a) seen_a++;
        else if (b) seen_b++;
        else torn++;
        if (pk < last) backwards++;
        last = pk;
    }
    stop_writer = 1;
    pthread_join(th, NULL);
    CHECK_EQ(torn, 0);
    CHECK_EQ(backwards, 0);
    CHECK(seen_a && seen_b, "both states seen (%u, %u)", seen_a, seen_b);
    printf("race: 2,000,000 reads, %u rest + %u full, %u torn, final packet %u\n", seen_a, seen_b, torn, last);
    sp_pad_reset();
}

int main(void) {
    test_map();
    test_users();
    test_rumble();
    test_race();
    printf("gamepad core tests: %d passed, %d failed\n", t_pass, t_fail);
    return t_fail ? 1 : 0;
}
