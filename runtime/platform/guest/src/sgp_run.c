/* sgp_run.c -- SPORE_RUN_SECONDS and the timed-run report (docs/design/platform-macos.md 10.12).
 *
 * SPORE_RUN_SECONDS=N: the first PeekMessage on the window thread after N seconds of running prints
 * the report below and exits the process with status 0 (_exit after a flush: the game's own
 * shutdown and its ~2,000 atexit handlers are not part of the measurement, and job threads are still running).
 * The check sits in the pump, so a hung main thread never reaches it and the watchdog fires instead.
 * SPORE_RUN_HEARTBEAT=S (default 60 when SPORE_RUN_SECONDS is set) prints a one-line "run: t=..."
 * progress line every S seconds from the same place.
 *
 * Report: frames (WM_PAINT delivered by the pump, Present calls on the D3D9 device), messages pumped,
 * host COM calls (D3D9 + DirectSound; per method at SPORE_COM_STATS=1 or by default here), live guest
 * threads by name, peak RSS. Optional packages are found with dlsym so this file links without them. */
#include <dlfcn.h>
#include <stdio.h>
#include <stdlib.h>
#include <string.h>
#include <sys/resource.h>
#include <time.h>
#include <unistd.h>

#include "sgp_int.h"

uint64_t sgp_stat_peek_calls, sgp_stat_msgs, sgp_stat_paints, sgp_stat_dispatched, sgp_stat_timers;

static double now_s(void) {
    struct timespec t;
    clock_gettime(CLOCK_MONOTONIC, &t);
    return (double)t.tv_sec + (double)t.tv_nsec / 1e9;
}

typedef struct { uint64_t frames, draws, primitives, locks, creates; uint32_t live_resources, resource_bytes; } d3d_stats;

static void line(FILE *f, double t) {
    d3d_stats d = {0};
    void (*gs)(d3d_stats *) = (void (*)(d3d_stats *))dlsym(RTLD_DEFAULT, "d3d9null_get_stats");
    uint64_t (*total)(void) = (uint64_t (*)(void))dlsym(RTLD_DEFAULT, "sg_com_total_calls");
    unsigned (*nthreads)(void) = (unsigned (*)(void))dlsym(RTLD_DEFAULT, "sg_core_thread_count");
    struct rusage ru;
    if (gs) gs(&d);
    getrusage(RUSAGE_SELF, &ru);
    fprintf(f,
            "run: t=%.1f s: WM_PAINT %llu, Present %llu, draws %llu, messages %llu (peeks %llu, dispatched %llu, "
            "WM_TIMER %llu), COM calls %llu, threads %u, live D3D resources %u (%u bytes), peak RSS %.1f MB\n",
            t, (unsigned long long)sgp_stat_paints, (unsigned long long)d.frames, (unsigned long long)d.draws,
            (unsigned long long)sgp_stat_msgs, (unsigned long long)sgp_stat_peek_calls,
            (unsigned long long)sgp_stat_dispatched, (unsigned long long)sgp_stat_timers,
            (unsigned long long)(total ? total() : 0), nthreads ? nthreads() : 0, d.live_resources, d.resource_bytes,
            (double)ru.ru_maxrss / (1024.0 * 1024.0));
}

/* ---- snapshots and scripted input --------------------------------------------------------------
 * SPORE_SNAPSHOT=path.png@S[,path2.png@S2...]: PNGs of the game window at those run times.
 * SPORE_INPUT_SCRIPT="T cmd args; ...": at run time T (seconds since the first PeekMessage)
 *   move X Y     a mouse move to client point (X, Y) (points)
 *   click X Y    left button down at T, up 0.15 s later, at (X, Y)
 *   rclick X Y   the same with the right button
 *   snap PATH    a snapshot
 * Drags, as a physical mouse makes them:
 *   down X Y / up X Y, rdown / rup, mdown / mup   one button edge (left, right, middle) at (X, Y)
 *   mclick X Y   middle button down and up
 *   rel DX DY [N [DT]]   N moves (default 1) of (DX, DY) points each, DT seconds apart (default 1/60),
 *                each relative to where the cursor is at that moment: after the game's SetCursorPos
 *                that is the warped point, as with a real mouse
 *   comtrace N   set runtime/com's sg_com_trace to N (2 = log every COM call, as SPORE_HLE_LOG=2; 1 = the
 *                default), so one frame's D3D9 call sequence can be captured without tracing the whole run
 * Keys and wheel (naming a creation, scaling parts):
 *   key KVK      a key down and up 0.05 s later; KVK is a macOS virtual key code (decimal or 0x..), e.g. 36
 *                Return, 53 Esc, 51 Delete (backspace), 48 Tab
 *   dclick X Y   a left double click (the second down/up pair carries click count 2)
 *   type TEXT    types TEXT (a-z, 0-9, space as '_'; no other characters), one key every 0.1 s
 *   wheel X Y N  N mouse-wheel notches (+ = away from the user) with the cursor at (X, Y)
 * Mouse events are real NSEvents posted to the AppKit queue (sp_mac_post_mouse, found by dlsym), so
 * they take the same window-to-client mapping as the user's mouse. Every step is logged ("script:"). */
enum { SC_MOVE, SC_DOWN, SC_UP, SC_SNAP, SC_REL, SC_COMTRACE, SC_KEYDOWN, SC_KEYUP, SC_WHEEL };
typedef struct { double t; int op, button, clicks; int32_t x, y; char path[400]; } script_ent;
static script_ent g_sc[4096];
static int g_nsc, g_isc;

static script_ent *sc_add(double t, int op, int button, int32_t x, int32_t y, const char *path) {
    static script_ent spare;
    script_ent *e;
    int i;
    if (g_nsc >= (int)(sizeof g_sc / sizeof g_sc[0])) return &spare;
    for (i = g_nsc; i > 0 && g_sc[i - 1].t > t; i--) g_sc[i] = g_sc[i - 1];   /* keep sorted, stable */
    e = &g_sc[i];
    memset(e, 0, sizeof *e);
    e->t = t; e->op = op; e->button = button; e->x = x; e->y = y;
    if (path) snprintf(e->path, sizeof e->path, "%s", path);
    g_nsc++;
    return e;
}

static void script_parse(void) {
    static char buf[262144];
    char *save = NULL, *tok;
    const char *sn = getenv("SPORE_SNAPSHOT"), *in = getenv("SPORE_INPUT_SCRIPT");
    if (sn) {
        snprintf(buf, sizeof buf, "%s", sn);
        for (tok = strtok_r(buf, ",", &save); tok; tok = strtok_r(NULL, ",", &save)) {
            char *at = strrchr(tok, '@');
            double t = 0;
            if (at) { *at = 0; t = atof(at + 1); }
            sc_add(t, SC_SNAP, 0, 0, 0, tok);
        }
    }
    if (in) {
        snprintf(buf, sizeof buf, "%s", in);
        for (tok = strtok_r(buf, ";", &save); tok; tok = strtok_r(NULL, ";", &save)) {
            double t;
            char cmd[16] = {0}, arg[400] = {0};
            int x = 0, y = 0;
            while (*tok == ' ') tok++;
            if (sscanf(tok, "%lf %15s", &t, cmd) != 2) { fprintf(stderr, "script: bad script step '%s'\n", tok); continue; }
            if (!strcmp(cmd, "snap") && sscanf(tok, "%*f %*s %399s", arg) == 1) sc_add(t, SC_SNAP, 0, 0, 0, arg);
            else if (!strcmp(cmd, "key") && sscanf(tok, "%*f %*s %i", &x) == 1) {
                sc_add(t, SC_KEYDOWN, 0, x, 0, NULL);
                sc_add(t + 0.05, SC_KEYUP, 0, x, 0, NULL);
            } else if (!strcmp(cmd, "type") && sscanf(tok, "%*f %*s %399s", arg) == 1) {
                /* macOS ANSI virtual key codes */
                static const uint8_t az[26] = {0, 11, 8, 2, 14, 3, 5, 4, 34, 38, 40, 37, 46, 45, 31, 35, 12, 15, 1, 17, 32, 9, 13, 7, 16, 6};
                static const uint8_t d09[10] = {29, 18, 19, 20, 21, 23, 22, 26, 28, 25};
                for (int k = 0; arg[k]; k++) {
                    int c = arg[k], kv = -1, ch = c;
                    if (c >= 'a' && c <= 'z') kv = az[c - 'a'];
                    else if (c >= '0' && c <= '9') kv = d09[c - '0'];
                    else if (c == '_') kv = 49, ch = ' ';
                    if (kv < 0) { fprintf(stderr, "script: type: skipping '%c'\n", c); continue; }
                    sc_add(t + 0.1 * k, SC_KEYDOWN, 0, kv, ch, NULL);
                    sc_add(t + 0.1 * k + 0.04, SC_KEYUP, 0, kv, ch, NULL);
                }
            } else if (!strcmp(cmd, "wheel")) {
                int n = 0;
                if (sscanf(tok, "%*f %*s %d %d %d", &x, &y, &n) == 3) sc_add(t, SC_WHEEL, n, x, y, NULL);
                else fprintf(stderr, "script: bad script step '%s'\n", tok);
            }
            else if (!strcmp(cmd, "comtrace") && sscanf(tok, "%*f %*s %d", &x) == 1) sc_add(t, SC_COMTRACE, 0, x, 0, NULL);
            else if (sscanf(tok, "%*f %*s %d %d", &x, &y) != 2) fprintf(stderr, "script: bad script step '%s'\n", tok);
            else if (!strcmp(cmd, "move")) sc_add(t, SC_MOVE, 0, x, y, NULL);
            else if (!strcmp(cmd, "dclick")) { /* a double click (the second press has click count 2) */
                sc_add(t, SC_DOWN, 0, x, y, NULL);
                sc_add(t + 0.08, SC_UP, 0, x, y, NULL);
                sc_add(t + 0.16, SC_DOWN, 0, x, y, NULL)->clicks = 2;
                sc_add(t + 0.24, SC_UP, 0, x, y, NULL)->clicks = 2;
            }
            else if (!strcmp(cmd, "click") || !strcmp(cmd, "rclick") || !strcmp(cmd, "mclick")) {
                int b = cmd[0] == 'r' ? 1 : cmd[0] == 'm' ? 2 : 0;
                sc_add(t, SC_DOWN, b, x, y, NULL);
                sc_add(t + 0.15, SC_UP, b, x, y, NULL);
            } else if (!strcmp(cmd, "down") || !strcmp(cmd, "rdown") || !strcmp(cmd, "mdown"))
                sc_add(t, SC_DOWN, cmd[0] == 'r' ? 1 : cmd[0] == 'm' ? 2 : 0, x, y, NULL);
            else if (!strcmp(cmd, "up") || !strcmp(cmd, "rup") || !strcmp(cmd, "mup"))
                sc_add(t, SC_UP, cmd[0] == 'r' ? 1 : cmd[0] == 'm' ? 2 : 0, x, y, NULL);
            else if (!strcmp(cmd, "rel")) {
                int n = 1, k;
                double dt = 1.0 / 60.0;
                sscanf(tok, "%*f %*s %*d %*d %d %lf", &n, &dt);
                for (k = 0; k < n && k < 900; k++) sc_add(t + k * dt, SC_REL, 0, x, y, NULL);
            } else fprintf(stderr, "script: bad script step '%s'\n", tok);
        }
    }
}

static void script_step(double t) {
    static int parsed;
    if (!parsed) { parsed = 1; script_parse(); }
    while (g_isc < g_nsc && t >= g_sc[g_isc].t) {
        script_ent *e = &g_sc[g_isc++];
        if (e->op == SC_COMTRACE) {
            int *tr = (int *)dlsym(RTLD_DEFAULT, "sg_com_trace");
            if (tr) *tr = e->x;
            fprintf(stderr, "script: t=%.2f s: sg_com_trace = %d%s (WM_PAINT %llu)\n", t, e->x, tr ? "" : " (not linked)",
                    (unsigned long long)sgp_stat_paints);
        } else if (e->op == SC_KEYDOWN || e->op == SC_KEYUP) {
            int (*pk)(uint32_t, int, uint16_t, uint16_t) = (int (*)(uint32_t, int, uint16_t, uint16_t))dlsym(RTLD_DEFAULT, "sp_mac_post_key");
            int r = pk ? pk(sgp_main_window(), e->op == SC_KEYDOWN ? 1 : 2, (uint16_t)e->x, (uint16_t)e->y) : -1;
            fprintf(stderr, "script: input t=%.2f s: key %s 0x%x '%c' posted %d (WM_PAINT %llu)\n", t, e->op == SC_KEYDOWN ? "down" : "up",
                    e->x, e->y >= 32 && e->y < 127 ? e->y : '?', r, (unsigned long long)sgp_stat_paints);
        } else if (e->op == SC_WHEEL) {
            int (*pw)(uint32_t, int32_t, int32_t, int) = (int (*)(uint32_t, int32_t, int32_t, int))dlsym(RTLD_DEFAULT, "sp_mac_post_wheel");
            int r = pw ? pw(sgp_main_window(), e->x, e->y, e->button) : -1;
            fprintf(stderr, "script: input t=%.2f s: wheel %d at client (%d,%d) posted %d (WM_PAINT %llu)\n", t, e->button, e->x, e->y, r,
                    (unsigned long long)sgp_stat_paints);
        } else if (e->op == SC_SNAP) {
            int (*snap)(uint32_t, const char *) = (int (*)(uint32_t, const char *))dlsym(RTLD_DEFAULT, "sp_mac_snapshot");
            fprintf(stderr, "run: snapshot of window 0x%x at t=%.1f s -> %s: %d\n", sgp_main_window(), t, e->path,
                    snap ? snap(sgp_main_window(), e->path) : -1);
            line(stderr, t);
        } else {
            static const char *names[] = {"move", "down", "up", "", "rel move"};
            static const char *bnames[] = {"", "right ", "middle "};
            int (*post)(uint32_t, int, int, int32_t, int32_t, int) =
                (int (*)(uint32_t, int, int, int32_t, int32_t, int))dlsym(RTLD_DEFAULT, "sp_mac_post_mouse");
            int32_t x = e->x, y = e->y;
            int r;
            if (e->op == SC_REL) { /* relative to the cursor now (scripted point, moved by SetCursorPos) */
                int32_t cx, cy;
                sgp_be->get_cursor_pos(&cx, &cy);
                sp_screen_to_client(sgp_main_window(), &cx, &cy);
                x = cx + e->x;
                y = cy + e->y;
            }
            r = post ? post(sgp_main_window(), e->op == SC_REL ? SC_MOVE : e->op, e->button, x, y, e->clicks ? e->clicks : 1) : -1;
            fprintf(stderr, "script: input t=%.2f s: %s%s at client (%d,%d) posted %d (WM_PAINT %llu)\n", t,
                    bnames[e->button], names[e->op], x, y, r, (unsigned long long)sgp_stat_paints);
        }
    }
}

/* Run time in seconds since the first PeekMessage of a timed run (the SPORE_INPUT_SCRIPT clock), or -1 before
 * that or when no timed run is armed. Other packages (runtime/dsound's SPORE_DS_LOG) find it with dlsym so their
 * log lines carry the script's time base. */
static double run_t0 = -1;
__attribute__((visibility("default"))) double sgp_run_time(void);
double sgp_run_time(void) { return run_t0 < 0 ? -1 : now_s() - run_t0; }

void sgp_run_check(void) {
    static int armed = -1;
    static double t0, limit, beat, next_beat;
    double t;
    if (armed == 0) return;
    if (armed < 0) {
        const char *s = getenv("SPORE_RUN_SECONDS"), *h = getenv("SPORE_RUN_HEARTBEAT");
        armed = s && atof(s) > 0;
        if (!armed) return;
        t0 = now_s();
        run_t0 = t0;
        limit = atof(s);
        beat = h ? atof(h) : 60.0;
        next_beat = beat > 0 ? beat : 1e300;
        /* t0 is the first pump call; the boot before it (about 3 s) is not counted. */
        fprintf(stderr, "run: SPORE_RUN_SECONDS=%g armed at the first PeekMessage\n", limit);
        return;
    }
    t = now_s() - t0;
    script_step(t);
    if (t >= next_beat) {
        line(stderr, t);
        next_beat += beat;
    }
    if (t < limit) return;
    {
        static char buf[4096];
        unsigned (*list)(char *, size_t) = (unsigned (*)(char *, size_t))dlsym(RTLD_DEFAULT, "sg_core_thread_list");
        void (*dump)(FILE *) = (void (*)(FILE *))dlsym(RTLD_DEFAULT, "sg_com_dump_stats");
        void (*dsdump)(FILE *) = (void (*)(FILE *))dlsym(RTLD_DEFAULT, "dsound_dump_stats");
        fprintf(stderr, "\nrun: RUN COMPLETE after %.1f s (SPORE_RUN_SECONDS=%g)\n", t, limit);
        line(stderr, t);
        if (list) {
            unsigned n = list(buf, sizeof buf);
            fprintf(stderr, "run: %u guest threads alive: %s\n", n, buf);
        }
        if (dump) dump(stderr);
        if (dsdump) dsdump(stderr);
        fprintf(stderr, "run: exit 0\n");
        fflush(NULL);
        _exit(0);
    }
}
