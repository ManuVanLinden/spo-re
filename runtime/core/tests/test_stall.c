/* test_stall.c -- the main-thread stall watchdog (core_stall.c). Each case runs in a forked
 * child whose stderr goes to a pipe: the child beats like a game loop, then a fault is injected
 * (as a back end's device-lost handling does) while the main thread blocks forever, as it does in
 * a wait for lost GPU work; or the loop simply stops beating. The parent checks that the child dies
 * of SIGABRT within 10 s and that the report names the cause and contains the reporter's lines. */
#include <poll.h>
#include <signal.h>
#include <sys/wait.h>
#include <time.h>
#include <unistd.h>

#include "core_internal.h"
#include "fixture.h"

static void reporter(int fd) { dprintf(fd, "test reporter: last call IDirect3DVertexBuffer9::Lock\n"); }

static void *fault_thread(void *arg) {
    (void)arg;
    struct timespec ts = {0, 300 * 1000000L};
    nanosleep(&ts, NULL);
    sg_core_stall_fault("VK_ERROR_DEVICE_LOST (injected)");
    return NULL;
}

/* mode 0: fault from another thread, main thread hangs; 1: no fault, main thread stops beating;
 * 2: fault while the main thread keeps beating; 3: the main thread stops beating while the window is inactive
 * (suspended for 5 s, no abort), then the window is active again and the stall fires about 2 s later */
static void child(int mode) {
    pthread_t t;
    sg_core_stall_config(mode == 1 || mode == 3 ? 2.0 : 0.0, 3.0);
    sg_core_stall_set_reporter(reporter);
    for (int i = 0; i < 20; i++) {
        struct timespec ts = {0, 16 * 1000000L};
        sg_core_stall_beat();
        nanosleep(&ts, NULL);
    }
    if (mode == 3) {
        struct timespec five = {5, 0};
        sg_core_stall_suspend(1);
        nanosleep(&five, NULL); /* inactive: no beats, no abort */
        sg_core_stall_suspend(0);
    }
    if (mode != 1 && mode != 3) pthread_create(&t, NULL, fault_thread, NULL);
    for (;;) { /* the hang (modes 0, 1) or a loop that keeps rendering (mode 2) */
        struct timespec ts = {0, 16 * 1000000L};
        if (mode == 2) sg_core_stall_beat();
        nanosleep(&ts, NULL);
    }
}

static void run_case2(int mode, const char *want, double min_dt);
static void run_case(int mode, const char *want) { run_case2(mode, want, 0.0); }
static void run_case2(int mode, const char *want, double min_dt) {
    int p[2];
    char out[8192];
    size_t n = 0;
    struct timespec t0, t1;
    pid_t pid;
    int st = 0;
    if (pipe(p)) { CHECK(0, "pipe"); return; }
    clock_gettime(CLOCK_MONOTONIC, &t0);
    pid = fork();
    if (pid == 0) {
        dup2(p[1], 2);
        close(p[0]);
        signal(SIGABRT, SIG_DFL);
        setenv("SPORE_STALL_MIN_FRAMES", "10", 1);
        alarm(20); /* never outlive the test */
        child(mode);
        _exit(0);
    }
    close(p[1]);
    for (;;) {
        struct pollfd pf = {p[0], POLLIN, 0};
        if (poll(&pf, 1, 15000) <= 0) break;
        ssize_t r = read(p[0], out + n, sizeof out - 1 - n);
        if (r <= 0) break;
        n += (size_t)r;
    }
    out[n] = 0;
    close(p[0]);
    waitpid(pid, &st, 0);
    clock_gettime(CLOCK_MONOTONIC, &t1);
    double dt = (t1.tv_sec - t0.tv_sec) + (t1.tv_nsec - t0.tv_nsec) / 1e9;
    CHECK(WIFSIGNALED(st) && WTERMSIG(st) == SIGABRT, "mode %d: child status 0x%x (want SIGABRT)", mode, st);
    CHECK(dt < 10.0, "mode %d: took %.2f s", mode, dt);
    CHECK(dt >= min_dt, "mode %d: fired after %.2f s, before the %.1f s it had to wait", mode, dt, min_dt);
    CHECK(strstr(out, want) != NULL, "mode %d: no '%s' in report:\n%s", mode, want, out);
    CHECK(strstr(out, "test reporter: last call") != NULL, "mode %d: reporter lines missing", mode);
    if (t_fail == 0 && getenv("STALL_VERBOSE")) fprintf(stderr, "--- mode %d (%.2f s) ---\n%s", mode, dt, out);
}

void test_stall(void) {
    t_name = "stall";
    run_case(0, "core: stall watchdog: device fault");
    run_case(1, "core: stall watchdog: the main thread stopped beating");
    run_case(2, "the main thread still beat");
    run_case2(3, "core: stall watchdog: the main thread stopped beating", 6.5);
}
