/* sp_gamepad_core.c -- the four XInput users as portable C (no frameworks), shared by the
 * GameController back end (sp_gamepad.m) and the tests' fake back ends.
 *
 * The back end calls sp_pad_connect/sp_pad_update/sp_pad_disconnect from its own queue; the game's
 * gamepad manager (0x8d2920, once per frame per user, main thread) reaches sp_pad_get_state through
 * XInputGetState. Everything is one small table under an os_unfair_lock: a read copies 16 bytes. */
#include <math.h>
#include <stdlib.h>
#include <string.h>
#include <os/lock.h>

#include "sp_macos.h"

typedef struct pad_user {
    void *dev;                /* back-end key (the GCController), NULL = empty */
    uint32_t packet;          /* kept across reconnects, so a new pad never repeats an old number */
    sp_pad_xstate st;
    sp_pad_caps caps;
    uint16_t vib_lo, vib_hi;
} pad_user;

static pad_user g_users[SP_PAD_USERS];
static os_unfair_lock g_lock = OS_UNFAIR_LOCK_INIT;
static sp_pad_rumble_fn g_rumble;

static int16_t axis16(float v) {
    if (!(v == v)) return 0;                 /* NaN */
    if (v >= 1.0f) return 32767;
    if (v <= -1.0f) return -32768;
    long r = v >= 0 ? lrintf(v * 32767.0f) : lrintf(v * 32768.0f);
    return (int16_t)r;
}
static uint8_t trig8(float v) {
    if (!(v > 0.0f)) return 0;
    if (v >= 1.0f) return 255;
    return (uint8_t)lrintf(v * 255.0f);
}

void sp_pad_map(const sp_pad_input *in, sp_pad_xstate *out) {
    out->buttons = in->buttons & 0xf3ffu;    /* no guide (0x0400) or 0x0800: XInputGetState never reports them */
    out->lt = trig8(in->lt);
    out->rt = trig8(in->rt);
    out->lx = axis16(in->lx);
    out->ly = axis16(in->ly);                /* GameController and XInput both have +y up */
    out->rx = axis16(in->rx);
    out->ry = axis16(in->ry);
}

static int find_dev(void *dev) {
    for (int i = 0; i < SP_PAD_USERS; i++)
        if (dev && g_users[i].dev == dev) return i;
    return -1;
}

int sp_pad_connect(void *dev, const sp_pad_caps *caps) {
    int u;
    if (!dev) return -1;
    os_unfair_lock_lock(&g_lock);
    u = find_dev(dev);
    if (u < 0) {
        for (u = 0; u < SP_PAD_USERS && g_users[u].dev; u++) {}
        if (u < SP_PAD_USERS) {
            pad_user *p = &g_users[u];
            p->dev = dev;
            p->packet++;
            memset(&p->st, 0, sizeof p->st);
            if (caps) p->caps = *caps;
            else memset(&p->caps, 0, sizeof p->caps);
            p->vib_lo = p->vib_hi = 0;
        } else {
            u = -1;
        }
    }
    os_unfair_lock_unlock(&g_lock);
    return u;
}

int sp_pad_disconnect(void *dev) {
    os_unfair_lock_lock(&g_lock);
    int u = find_dev(dev);
    if (u >= 0) {
        g_users[u].dev = NULL;
        memset(&g_users[u].st, 0, sizeof g_users[u].st);
    }
    os_unfair_lock_unlock(&g_lock);
    return u;
}

int sp_pad_user_of(void *dev) {
    os_unfair_lock_lock(&g_lock);
    int u = find_dev(dev);
    os_unfair_lock_unlock(&g_lock);
    return u;
}

int sp_pad_update(void *dev, const sp_pad_input *in) {
    sp_pad_xstate x;
    int changed = 0;
    sp_pad_map(in, &x);
    os_unfair_lock_lock(&g_lock);
    int u = find_dev(dev);
    if (u >= 0 && memcmp(&g_users[u].st, &x, sizeof x)) {
        g_users[u].st = x;
        g_users[u].packet++;
        changed = 1;
    }
    os_unfair_lock_unlock(&g_lock);
    return changed;
}

int sp_pad_get_state(int user, uint32_t *packet, sp_pad_xstate *out) {
    int r = -1;
    if (user < 0 || user >= SP_PAD_USERS) return -1;
    os_unfair_lock_lock(&g_lock);
    if (g_users[user].dev) {
        if (packet) *packet = g_users[user].packet;
        if (out) *out = g_users[user].st;
        r = 0;
    }
    os_unfair_lock_unlock(&g_lock);
    return r;
}

int sp_pad_get_caps(int user, sp_pad_caps *out) {
    int r = -1;
    if (user < 0 || user >= SP_PAD_USERS) return -1;
    os_unfair_lock_lock(&g_lock);
    if (g_users[user].dev) {
        *out = g_users[user].caps;
        r = 0;
    }
    os_unfair_lock_unlock(&g_lock);
    return r;
}

void sp_pad_set_rumble_handler(sp_pad_rumble_fn fn) { g_rumble = fn; }

int sp_pad_set_vibration(int user, uint16_t low, uint16_t high) {
    void *dev = NULL;
    int send = 0;
    if (user < 0 || user >= SP_PAD_USERS) return -1;
    os_unfair_lock_lock(&g_lock);
    pad_user *p = &g_users[user];
    if (p->dev) {
        dev = p->dev;
        send = p->caps.rumble && (p->vib_lo != low || p->vib_hi != high);
        p->vib_lo = low;
        p->vib_hi = high;
    }
    os_unfair_lock_unlock(&g_lock);
    if (!dev) return -1;
    if (send && g_rumble) g_rumble(dev, low, high);   /* the back end queues it; never blocks the game */
    return 0;
}

void sp_pad_reset(void) {
    os_unfair_lock_lock(&g_lock);
    for (int i = 0; i < SP_PAD_USERS; i++) g_users[i].dev = NULL;
    os_unfair_lock_unlock(&g_lock);
}

/* The GameController back end: the strong definition in sp_gamepad.m wins when it is linked (the game, the
 * AppKit libraries and test apps); headless builds keep this one and have no pads. */
__attribute__((weak)) void sp_mac_gamepad_start(void) {}

void sp_pad_start(void) {
    static int started;
    if (__atomic_load_n(&started, __ATOMIC_ACQUIRE) || __atomic_exchange_n(&started, 1, __ATOMIC_ACQ_REL))
        return;                              /* started by the first XInput call (game main thread) */
    const char *e = getenv("SPORE_GAMEPAD");
    if (e && e[0] == '0') return;
    sp_mac_gamepad_start();
}
