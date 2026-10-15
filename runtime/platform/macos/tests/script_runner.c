/* script_runner.c - see script_runner.h. Our own test code; no game code. */
#include "script_runner.h"

#include <pthread.h>
#include <stdlib.h>
#include <string.h>

static FILE *g_log;
int (*sr_pump_hook)(sp_hwnd hwnd);   /* the guest fixture pumps through the imports */
static const sr_driver *g_drv;
static unsigned g_frames;

void sr_set_log(FILE *log, const sr_driver *drv) { g_log = log; g_drv = drv; }
unsigned sr_frames(void) { return g_frames; }

static int screen_lparam(uint32_t msg)
{
    return msg == WM_NCHITTEST || msg == WM_MOUSEWHEEL || msg == WM_CONTEXTMENU || msg == WM_MOVE;
}
static int key_msg(uint32_t msg)
{
    return msg == WM_KEYDOWN || msg == WM_KEYUP || msg == WM_SYSKEYDOWN || msg == WM_SYSKEYUP;
}

static void log_msg(int depth, char kind, sp_hwnd h, uint32_t msg, uint32_t w, int32_t l)
{
    if (!g_log) return;
    for (int i = 0; i < depth; i++) fputs("  ", g_log);
    fprintf(g_log, "%c %s w=0x%08X l=", kind, sp_message_name(msg), w);
    if (screen_lparam(msg)) {
        int32_t x = (int16_t)(l & 0xFFFF), y = (int16_t)((uint32_t)l >> 16);
        sp_screen_to_client(h, &x, &y);
        fprintf(g_log, "scr(%d,%d)\n", x, y);
    } else {
        fprintf(g_log, "0x%08X\n", (uint32_t)l);
    }
}

int32_t sr_wndproc(void *user, sp_hwnd h, uint32_t msg, uint32_t w, int32_t l)
{
    (void)user;
    int depth; char kind;
    sp_delivery_info(&depth, &kind);
    log_msg(depth, kind, h, msg, w, l);
    int32_t r;
    switch (msg) {
    case WM_PAINT: {
        sp_paintstruct ps;
        sp_begin_paint(h, &ps);
        g_frames++;
        if (g_drv && g_drv->frame) g_drv->frame(g_drv->ctx, h);
        sp_end_paint(h, &ps);
        r = 0;
        break;
    }
    case WM_ERASEBKGND: r = 1; break;
    case WM_SETCURSOR: r = 1; break;
    case WM_CLOSE: sp_post_quit_message(0); r = 0; break;
    default: r = sp_def_window_proc(h, msg, w, l); break;
    }
    if (key_msg(msg) && g_log) {
        for (int i = 0; i < depth + 1; i++) fputs("  ", g_log);
        int16_t ks = sp_get_key_state((int)w);
        int async = (sp_get_async_key_state((int)w) & 0x8000) != 0;
        fprintf(g_log, "= ks(0x%02X)=0x%04X async=%d\n", w, (uint16_t)ks, async);
    }
    return r;
}

int sr_pump_once(sp_hwnd hwnd)
{
    if (g_log) fputs("-- pump\n", g_log);
    sp_invalidate_rect(hwnd, 0);
    sp_msg m;
    for (int guard = 0; guard < 10000; guard++) {
        if (!sp_peek_message(&m, 0, 0, 0, PM_REMOVE)) return 1;
        if (m.message == WM_QUIT) {
            if (g_log) fprintf(g_log, "Q WM_QUIT w=0x%08X l=0x%08X\n", m.wParam, (uint32_t)m.lParam);
            return 0;
        }
        sp_translate_message(&m);
        sp_dispatch_message(&m);
    }
    fprintf(stderr, "pump: runaway loop\n");
    return 1;
}

/* ---- script parsing ------------------------------------------------------------------------- */
typedef struct { sp_hwnd h; uint32_t msg, w; int32_t l; } post_args;
static void *post_thread(void *p)
{
    post_args *a = p;
    sp_post_message(a->h, a->msg, a->w, a->l);
    return NULL;
}

static int parse_u16_list(const char *s, uint16_t *out, int max)
{
    int n = 0;
    while (*s && n < max) {
        char *end;
        unsigned long v = strtoul(s, &end, 16);
        if (end == s) break;
        out[n++] = (uint16_t)v;
        s = end;
        if (*s == ',') s++;
    }
    return n;
}

static uint32_t parse_mods(const char *s)
{
    uint32_t m = 0;
    if (strstr(s, "shift")) m |= SR_MODS_SHIFT;
    if (strstr(s, "ctrl")) m |= SR_MODS_CTRL;
    if (strstr(s, "alt")) m |= SR_MODS_ALT;
    if (strstr(s, "cmd")) m |= SR_MODS_CMD;
    return m;
}

int sr_run_script(const char *path, sp_hwnd hwnd)
{
    FILE *f = fopen(path, "r");
    if (!f) { perror(path); return -1; }
    char line[512];
    int in_step = 0, pumps = 0, running = 1;
    while (running) {
        char *got = fgets(line, sizeof line, f);
        int is_step = got && strncmp(line, "step ", 5) == 0;
        if ((is_step || !got) && in_step) { pumps++; running = sr_pump_hook ? sr_pump_hook(hwnd) : sr_pump_once(hwnd); }
        if (!got || !running) break;
        char *nl = strchr(line, '\n'); if (nl) *nl = 0;
        if (line[0] == '#' || line[0] == 0) continue;
        char a[32] = "", b[32] = "", c[32] = "", d[32] = "";
        char rest[256] = "";
        sscanf(line, "%31s %31s %31s %31s %31s", a, b, c, d, rest);
        if (is_step) { if (g_log) fprintf(g_log, "# step %s\n", b); in_step = 1; continue; }
        if (!strcmp(a, "show")) g_drv->show(g_drv->ctx);
        else if (!strcmp(a, "key")) {
            int action = !strcmp(b, "down") ? SP_KEY_DOWN : !strcmp(b, "up") ? SP_KEY_UP : SP_KEY_REPEAT;
            uint16_t kvk = (uint16_t)strtoul(c, NULL, 16);
            uint16_t ch[8], ig[8]; int n = 0, ni = 0;
            const char *p;
            if ((p = strstr(line, "chars="))) n = parse_u16_list(p + 6, ch, 8);
            if ((p = strstr(line, "ign="))) ni = parse_u16_list(p + 4, ig, 8);
            else { memcpy(ig, ch, sizeof ch); ni = n; }
            uint32_t mods = (p = strstr(line, "mods=")) ? parse_mods(p + 5) : 0;
            g_drv->key(g_drv->ctx, action, kvk, ch, n, ig, ni, mods);
        } else if (!strcmp(a, "flags")) {
            uint16_t kvk = (uint16_t)strtoul(b, NULL, 16);
            int on = !strcmp(c, "down") || !strcmp(c, "on");
            g_drv->flags(g_drv->ctx, kvk, on);
        } else if (!strcmp(a, "mouse")) {
            int kind = !strcmp(b, "move") ? SR_MOUSE_MOVE : !strcmp(b, "down") ? SR_MOUSE_DOWN
                     : !strcmp(b, "up") ? SR_MOUSE_UP : SR_MOUSE_DRAG;
            int button = !strcmp(c, "left") ? SP_MB_LEFT : !strcmp(c, "right") ? SP_MB_RIGHT
                       : !strcmp(c, "middle") ? SP_MB_MIDDLE : -1;
            int x = atoi(d), y = 0, clicks = 1;
            sscanf(line, "%*s %*s %*s %*d %d", &y);
            const char *p = strstr(line, "clicks=");
            if (p) clicks = atoi(p + 7);
            g_drv->mouse(g_drv->ctx, kind, button, x, y, clicks);
        } else if (!strcmp(a, "wheel")) g_drv->wheel(g_drv->ctx, atoi(b));
        else if (!strcmp(a, "resize")) g_drv->resize(g_drv->ctx, atoi(b), atoi(c));
        else if (!strcmp(a, "activate")) g_drv->activate(g_drv->ctx, 1);
        else if (!strcmp(a, "deactivate")) g_drv->activate(g_drv->ctx, 0);
        else if (!strcmp(a, "close")) g_drv->close(g_drv->ctx);
        else if (!strcmp(a, "post")) {
            post_args pa = {hwnd, (uint32_t)strtoul(b, NULL, 16), (uint32_t)strtoul(c, NULL, 16),
                            (int32_t)strtoul(d, NULL, 16)};
            pthread_t t;
            pthread_create(&t, NULL, post_thread, &pa);
            pthread_join(t, NULL);
        } else {
            fprintf(stderr, "script: unknown command: %s\n", line);
            fclose(f);
            return -1;
        }
    }
    fclose(f);
    return pumps;
}
