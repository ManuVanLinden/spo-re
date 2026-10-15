/* core_stall.c -- main-thread stall watchdog.
 *
 * After a GPU device loss the game's main thread can block forever in a wait for GPU work that never
 * completes. The D3D9 API never reports the loss, so the back end reports it here (sg_core_stall_fault,
 * from its device-lost handling) and beats once per presented frame (sg_core_stall_beat). A monitor
 * thread turns either a reported fault or a long stall into an abort with a report, instead of a
 * process that hangs until someone kills it. */
#include <errno.h>
#include <pthread.h>
#include <signal.h>
#include <stdatomic.h>
#include <stdio.h>
#include <stdlib.h>
#include <string.h>
#include <time.h>
#include <unistd.h>

#include "sg_core.h"

static pthread_once_t g_once = PTHREAD_ONCE_INIT;
static pthread_t g_main, g_monitor;
static _Atomic uint64_t g_beats, g_last_beat_ns, g_fault_ns, g_first_ns;
static _Atomic int g_started, g_suspended; /* suspended: the window is inactive and the game is not drawing */
static char g_fault_what[160];
static void (*_Atomic g_reporter)(int fd);
static double g_stall_s = -1, g_grace_s = -1, g_min_frames = -1;

static uint64_t now_ns(void) {
    struct timespec ts;
    clock_gettime(CLOCK_MONOTONIC, &ts);
    return (uint64_t)ts.tv_sec * 1000000000ull + (uint64_t)ts.tv_nsec;
}
static double env_seconds(const char *name, double dflt) {
    const char *s = getenv(name);
    return s && *s ? atof(s) : dflt;
}
static void put(const char *s) { (void)!write(2, s, strlen(s)); }

static void fire(const char *why) {
    char buf[512];
    uint64_t t = now_ns(), lb = atomic_load(&g_last_beat_ns), fn = atomic_load(&g_fault_ns);
    void (*rep)(int) = atomic_load(&g_reporter);
    snprintf(buf, sizeof buf, "\ncore: stall watchdog: %s; %llu frames beaten, last beat %.2f s ago, %.2f s since the first%s%s\n",
             why, (unsigned long long)atomic_load(&g_beats), lb ? (t - lb) / 1e9 : -1.0,
             atomic_load(&g_first_ns) ? (t - atomic_load(&g_first_ns)) / 1e9 : -1.0, fn ? "; fault: " : "",
             fn ? g_fault_what : "");
    put(buf);
    if (fn) {
        snprintf(buf, sizeof buf, "core: stall watchdog: the fault was reported %.2f s ago; the main thread %s since\n",
                 (t - fn) / 1e9, lb > fn ? "still beat" : "has not beaten");
        put(buf);
    }
    if (rep) rep(2);
    put("core: stall watchdog: aborting; SIGABRT to the main thread for its call stack\n");
    pthread_kill(g_main, SIGABRT);
    /* If the main thread's handler does not end the process (signal blocked, handler stuck), end it here. */
    for (int i = 0; i < 40; i++) {
        struct timespec ts = {0, 50 * 1000000L};
        nanosleep(&ts, NULL);
    }
    put("core: stall watchdog: the main thread did not end the process; abort() from the monitor\n");
    signal(SIGABRT, SIG_DFL);
    abort();
}

static void *monitor(void *arg) {
    (void)arg;
    pthread_setname_np("sg-stall-watchdog");
    for (;;) {
        struct timespec ts = {0, 200 * 1000000L};
        nanosleep(&ts, NULL);
        uint64_t t = now_ns(), lb = atomic_load(&g_last_beat_ns), fn = atomic_load(&g_fault_ns);
        double stall = g_stall_s, grace = g_grace_s;
        if (fn && (t - fn) / 1e9 >= grace) fire("device fault");
        if (stall > 0 && lb && !atomic_load(&g_suspended) && atomic_load(&g_beats) >= (uint64_t)g_min_frames &&
            (t - lb) / 1e9 >= stall)
            fire("the main thread stopped beating");
    }
    return NULL;
}

static void start(void) {
    if (g_stall_s < 0) g_stall_s = env_seconds("SPORE_STALL_SECONDS", 0);
    if (g_grace_s < 0) g_grace_s = env_seconds("SPORE_STALL_FAULT_SECONDS", 3);
    /* loading screens present a frame and then work for a long time: arm the stall check only after this
     * many frames (SPORE_STALL_MIN_FRAMES, default 600, about 10 s of play) */
    if (g_min_frames < 0) g_min_frames = env_seconds("SPORE_STALL_MIN_FRAMES", 600);
    if (g_grace_s > 8) g_grace_s = 8; /* the report must come within 10 s of a loss */
    pthread_attr_t a;
    pthread_attr_init(&a);
    pthread_attr_setdetachstate(&a, PTHREAD_CREATE_DETACHED);
    if (pthread_create(&g_monitor, &a, monitor, NULL) == 0) atomic_store(&g_started, 1);
    else put("core: stall watchdog: could not start the monitor thread\n");
    pthread_attr_destroy(&a);
}

void sg_core_stall_config(double stall_seconds, double fault_grace_seconds) {
    if (stall_seconds >= 0) g_stall_s = stall_seconds;
    if (fault_grace_seconds >= 0) g_grace_s = fault_grace_seconds;
}

void sg_core_stall_set_reporter(void (*fn)(int fd)) { atomic_store(&g_reporter, fn); }

/* The platform calls this with 1 when the window loses activation (the game stops drawing while inactive, so no
 * beats arrive) and 0 when it is active again; the quiet time is not counted as a stall. Device faults still fire. */
void sg_core_stall_suspend(int on) {
    atomic_store(&g_suspended, on ? 1 : 0);
    if (!on) atomic_store(&g_last_beat_ns, now_ns()); /* the stall clock restarts at resume */
}

void sg_core_stall_beat(void) {
    uint64_t t = now_ns();
    if (!atomic_load(&g_first_ns)) {
        uint64_t z = 0;
        if (atomic_compare_exchange_strong(&g_first_ns, &z, t)) {
            g_main = pthread_self();
            pthread_once(&g_once, start);
        }
    }
    atomic_store(&g_last_beat_ns, t);
    atomic_fetch_add(&g_beats, 1);
}

void sg_core_stall_fault(const char *what) {
    uint64_t z = 0, t = now_ns();
    char buf[256];
    if (!atomic_compare_exchange_strong(&g_fault_ns, &z, t)) return; /* the first fault wins */
    snprintf(g_fault_what, sizeof g_fault_what, "%s", what ? what : "(unnamed)");
    snprintf(buf, sizeof buf, "core: stall watchdog: fault reported: %s (abort in %.1f s)%s\n", g_fault_what,
             g_grace_s >= 0 ? g_grace_s : env_seconds("SPORE_STALL_FAULT_SECONDS", 3),
             atomic_load(&g_first_ns) ? "" : "; no frame has been beaten yet");
    put(buf);
    if (!atomic_load(&g_first_ns)) {
        /* no beat yet: the faulting thread stands in for the main thread */
        atomic_store(&g_first_ns, t);
        g_main = pthread_self();
        pthread_once(&g_once, start);
    }
}
