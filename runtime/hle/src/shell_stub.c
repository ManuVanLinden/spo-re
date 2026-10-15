/* shell_stub.c -- shell32!ShellExecuteW: browser links.
 *
 * Who calls it [obs, Ghidra over the GOG SporeApp.exe]:
 *  - 0x935f20 calls the static import at 0x935f75: ShellExecuteW(NULL, L"open", url, NULL, NULL,
 *    SW_SHOWNORMAL) and returns (result > 32) as a bool (0x935f7b: cmp eax, 0x20; jbe failure).
 *    Its callers: the YouTube sign-in dialogs (0x63d2f0, 0xf062a0, 0xf06c20) with the constants
 *    L"http://youtube.com/t/terms" and L"http://www.youtube.com/signup"; and 0x7e9e80, the app's
 *    per-frame update, which drains a queue of string16 URLs at this+0x174 that the virtual method
 *    0x7e9dd0 (vtable slot 0x1413bac, "open this link") fills from the UI. That queue is the path
 *    for links in the game's own screens; runs 078 and 082 saw "http://www.spore.com" arrive this
 *    way from a menu click.
 *  - 0x935dd0 loads shell32.dll and calls ShellExecuteW through GetProcAddress (which resolves to
 *    this same shim): its callers open local files (README.txt from a cheat at 0x8149a0,
 *    species_dump.csv from the species_query cheat at 0x4db740). Those are file paths and are refused.
 * The EA UTF XHTML browser's URL code (0x94aca0) only classifies schemes (http, https, ..., mailto) for
 * its own HTTP client; it does not call ShellExecuteW. So a link inside an embedded web page reaches
 * the browser only if the page's handler hands it to the 0x7e9dd0 queue [inf: the only generic path].
 * Either way every call lands here and gets the same filter.
 *
 * Policy (DEVIATIONS 2.10): the verb must be NULL or "open" (case-insensitive) and the file an
 * http: or https: URL with a host, printable ASCII, no parameters. Then the URL goes to the host
 * opener (sp_mac_open_url in runtime/platform/macos/src/sp_open.m, found with dlsym so the HLE test
 * binary links without AppKit) and the result is 42 (> 32, success). Anything else (a file path,
 * file:, mailto:, javascript:, data:, another verb) is refused with SE_ERR_ACCESSDENIED (5) and logged
 * with its arguments. No opener, or an opener that fails: SE_ERR_NOASSOC (31). SPORE_OPEN_LINKS=0
 * refuses everything (SE_ERR_ACCESSDENIED for every call). The same URL again within one second is not opened twice
 * (still reported as success), so a double queue or double click gives one browser tab. */
#include <ctype.h>
#include <dlfcn.h>
#include <stdio.h>
#include <stdlib.h>
#include <string.h>
#include <strings.h>
#include <time.h>

#include "hle.h"
#include "hle_imports.h"

#define SE_OK_OPENED 42u          /* any value > 32 is success; 42 is what we return */
#define SE_ERR_ACCESSDENIED 5u
#define SE_ERR_NOASSOC 31u
#define URL_CAP 2048

typedef int (*hle_url_opener)(const char *utf8);

static hle_url_opener g_opener;    /* test hook; NULL = look up sp_mac_open_url */
static int g_opener_set;
static char g_last_url[URL_CAP];
static double g_last_time = -1e9;

/* Test hook (runtime/hle/tests/test_shell.c): replaces the host opener; returns the previous one.
 * Passing NULL goes back to the dlsym lookup. Also forgets the repeat filter. */
hle_url_opener hle_shell_set_opener(hle_url_opener fn) {
    hle_url_opener old = g_opener;
    g_opener = fn;
    g_opener_set = fn != NULL;
    g_last_url[0] = 0;
    g_last_time = -1e9;
    return old;
}

static hle_url_opener host_opener(void) {
    static hle_url_opener found;
    static int looked;
    if (g_opener_set) return g_opener;
    if (!looked) {
        looked = 1;
        found = (hle_url_opener)dlsym(RTLD_DEFAULT, "sp_mac_open_url");
    }
    return found;
}

/* Guest UTF-16 string to UTF-8. Returns -1 for a NULL pointer, 1 if it did not fit (truncated), else 0. */
static int guest_wstr(const uint8_t *mem, uint32_t p, char *out, size_t cap) {
    uint16_t buf[URL_CAP];
    size_t n = 0;
    int dflt = 0, trunc = 0;
    long r;
    out[0] = 0;
    if (!p) { snprintf(out, cap, "(null)"); return -1; }
    for (;;) {
        uint16_t c = (uint16_t)(mem[p + 2 * n] | (mem[p + 2 * n + 1] << 8));
        if (!c) break;
        if (n == URL_CAP - 1) { trunc = 1; break; }
        buf[n++] = c;
    }
    r = hle_utf16_to_mb(HLE_CP_UTF8, buf, n, (uint8_t *)out, cap - 1, &dflt, '?');
    if (r <= 0 && n) trunc = 1;
    out[r > 0 ? (size_t)r : 0] = 0;
    return trunc;
}

static int prefix_ci(const char *s, const char *p) {
    for (; *p; s++, p++)
        if (tolower((unsigned char)*s) != *p) return 0;
    return 1;
}

/* 1 if url is an http(s) URL we hand to the browser; *why gets the reason otherwise. */
static int url_allowed(const char *url, const char **why) {
    const char *host;
    const unsigned char *c;
    if (prefix_ci(url, "https://")) host = url + 8;
    else if (prefix_ci(url, "http://")) host = url + 7;
    else { *why = "not an http: or https: URL"; return 0; }
    if (!*host || *host == '/' || *host == '?' || *host == '#') { *why = "no host"; return 0; }
    for (c = (const unsigned char *)url; *c; c++)
        if (*c <= 0x20 || *c >= 0x7f || *c == '\\') { *why = "not printable ASCII"; return 0; }
    return 1;
}

static double now_s(void) {
    struct timespec ts;
    clock_gettime(CLOCK_MONOTONIC, &ts);
    return (double)ts.tv_sec + ts.tv_nsec * 1e-9;
}

/* A copy for the log: control characters become '?', and long strings are cut at 300 bytes. */
static const char *loggable(const char *s, char *out, size_t cap) {
    size_t i = 0;
    for (; s[i] && i + 4 < cap && i < 300; i++) out[i] = (unsigned char)s[i] < 0x20 || s[i] == 0x7f ? '?' : s[i];
    if (s[i]) { memcpy(out + i, "...", 3); i += 3; }
    out[i] = 0;
    return out;
}

static int verb_is_open(const char *op, int op_null) {
    if (op_null) return 1;
    return !strcasecmp(op, "open");
}

SG_IMPORT(shell32, ShellExecuteW) { /* (hwnd, op, file, params, dir, show) */
    char op[64], file[URL_CAP * 3], params[256], dir[256];
    int op_st = guest_wstr(mem, SG_ARG(1), op, sizeof op);
    int file_st = guest_wstr(mem, SG_ARG(2), file, sizeof file);
    int par_st = guest_wstr(mem, SG_ARG(3), params, sizeof params);
    const char *env = getenv("SPORE_OPEN_LINKS");
    const char *why = NULL;
    uint32_t rc;
    hle_url_opener open_fn;

    if (env && !strcmp(env, "0")) why = "SPORE_OPEN_LINKS=0";
    else if (op_st > 0 || !verb_is_open(op, op_st < 0)) why = "verb is not \"open\"";
    else if (file_st != 0) why = file_st < 0 ? "no file" : "URL too long";
    else if (par_st >= 0 && params[0]) why = "parameters given";
    else url_allowed(file, &why);

    guest_wstr(mem, SG_ARG(4), dir, sizeof dir);
    if (why) {
        char l_op[320], l_file[320], l_par[320], l_dir[320];
        hle_log(HLE_LOG_WARN, "ShellExecuteW(hwnd 0x%x, \"%s\", \"%s\", \"%s\", \"%s\", show %u): refused (%s) "
                "= SE_ERR_ACCESSDENIED", SG_ARG(0), loggable(op, l_op, sizeof l_op), loggable(file, l_file, sizeof l_file),
                loggable(params, l_par, sizeof l_par), loggable(dir, l_dir, sizeof l_dir), SG_ARG(5), why);
        SG_RETURN_STDCALL(SE_ERR_ACCESSDENIED, 24);
    }

    {
        double t = now_s();
        if (!strcmp(file, g_last_url) && t - g_last_time < 1.0) {
            hle_log(HLE_LOG_WARN, "ShellExecuteW(\"open\", \"%s\"): repeated within 1 s, not opened again = 42", file);
            SG_RETURN_STDCALL(SE_OK_OPENED, 24);
        }
        open_fn = host_opener();
        if (open_fn && open_fn(file)) {
            snprintf(g_last_url, sizeof g_last_url, "%s", file);
            g_last_time = t;
            rc = SE_OK_OPENED;
            hle_log(HLE_LOG_WARN, "ShellExecuteW(hwnd 0x%x, \"%s\", \"%s\", show %u): opened in the default browser = 42",
                    SG_ARG(0), op, file, SG_ARG(5));
        } else {
            rc = SE_ERR_NOASSOC;
            hle_log(HLE_LOG_WARN, "ShellExecuteW(hwnd 0x%x, \"%s\", \"%s\", show %u): %s = SE_ERR_NOASSOC", SG_ARG(0), op,
                    file, SG_ARG(5), open_fn ? "the host could not open it" : "no host opener (sp_mac_open_url not linked)");
        }
    }
    SG_RETURN_STDCALL(rc, 24);
}
