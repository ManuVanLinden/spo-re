/* script_runner.h - replays tests/input-script.txt through a driver and writes the WM_* log. */
#ifndef SCRIPT_RUNNER_H
#define SCRIPT_RUNNER_H

#include <stdio.h>
#include "sp_platform.h"

enum { SR_MODS_SHIFT = 1, SR_MODS_CTRL = 2, SR_MODS_ALT = 4, SR_MODS_CMD = 8 };
enum { SR_MOUSE_MOVE = 0, SR_MOUSE_DOWN = 1, SR_MOUSE_UP = 2, SR_MOUSE_DRAG = 3 };

/* How events reach the platform layer: directly (headless) or as real NSEvents (AppKit). */
typedef struct sr_driver {
    void *ctx;
    void (*key)(void *ctx, int action, uint16_t kvk, const uint16_t *chars, int n,
                const uint16_t *ign, int ni, uint32_t mods);
    void (*flags)(void *ctx, uint16_t kvk, int down_or_on);
    void (*mouse)(void *ctx, int kind, int button, int32_t x, int32_t y, int clicks);
    void (*wheel)(void *ctx, int lines);
    void (*resize)(void *ctx, int32_t w, int32_t h);
    void (*activate)(void *ctx, int on);
    void (*close)(void *ctx);
    void (*show)(void *ctx);
    void (*frame)(void *ctx, sp_hwnd h);      /* render one frame on WM_PAINT (may be NULL) */
} sr_driver;

/* The test window procedure: logs every message, handles WM_PAINT/WM_ERASEBKGND/WM_SETCURSOR/WM_CLOSE. */
int32_t sr_wndproc(void *user, sp_hwnd h, uint32_t msg, uint32_t w, int32_t l);
void    sr_set_log(FILE *log, const sr_driver *drv);
/* Runs every step; returns the number of pump passes, or -1 on a parse error. */
int     sr_run_script(const char *path, sp_hwnd hwnd);
/* One pump pass like the game's 0x8482f0. Returns 0 when WM_QUIT was seen. */
int     sr_pump_once(sp_hwnd hwnd);
unsigned sr_frames(void);
/* When set, sr_run_script pumps through this instead of sr_pump_once (the guest fixture). */
extern int (*sr_pump_hook)(sp_hwnd hwnd);

#endif
