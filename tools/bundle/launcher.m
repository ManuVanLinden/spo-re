/* launcher.m -- Contents/MacOS/Spore Native, the app bundle's main executable (docs/design/bundle.md).
 *
 * Finds the player's GOG Spore folder, sets up the environment the runtime reads, sends stdout and stderr to
 * ~/Library/Logs/Spore Native/, and execs the game binary (Contents/MacOS/spore-game --run) in the same
 * process, so the Dock, the menu bar and macOS's privacy prompts all see one app.
 *
 * Command line: --help, --version, --game-dir PATH (use and remember), --reset-game-dir,
 * --print-env, --resolve-only, --no-panel, --settings, --no-settings, --list-settings; only what
 * follows "--" reaches the game. Anything else is refused with
 * the usage text on stderr and exit 2, and nothing is started.
 *
 * Game folder, first match wins (each candidate must hold SporebinEP1/SporeApp.exe with the GOG EP1 SHA-256,
 * plus Data/ and DataEP1/):
 *   0. --game-dir PATH (checked first; a bad folder is an error, exit 1)
 *   1. $SPORE_GAME_DIR (an explicit override, as for development runs)
 *   2. the folder remembered in <support>/game-dir (<support> = $SPORE_SUPPORT_DIR or
 *      ~/Library/Application Support/spore-native)
 *   3. the usual places: ~/Downloads/spore-gog, ~/Applications and /Applications ("Spore", "Spore Galactic
 *      Adventures", ...), ~/GOG Games, /Users/Shared/GOG Galaxy/Games
 *   4. a native open panel asking for the folder that contains SporebinEP1 (remembered once it checks out).
 *
 * Settings (defaults domain com.spore-native.player, or $SPORE_DEFAULTS_DOMAIN for tests; the environment always
 * wins over a setting):
 *   GPUProfile        -> SPORE_GPU_PROFILE (8800gtx | 9800gtx | gtx280; checked against the install's
 *                        Data/Config/VideoCards.txt at run time)
 *   DocumentsFolder   -> SPORE_DOCUMENTS_DIR (default ~/Documents, so "My Spore Creations" lands where the game
 *                        puts it on Windows; falls back to the development sandbox if ~/Documents is not writable)
 *   Environment       -> a dictionary of extra variables (e.g. SPORE_ONLINE = 0)
 *   Arguments         -> an array of strings passed to the game after "--", before the command line's own
 *   ShowSettings      -> false: no settings window at start (absent = true)
 *
 * The settings window (docs/design/bundle.md section 10): shown at every start once the
 * game folder is resolved, unless ShowSettings is false (--settings or Option held shows it anyway; --no-settings,
 * --no-panel, SPORE_LAUNCHER_NO_PANEL, --print-env and --resolve-only never show it). Tabs of label-and-control
 * rows (General, Game options, Diagnostics), each row writing one switch; an untouched window writes nothing.
 * General has two groups under headings (k_heads[]): "Display" and "Online and folders". GPUProfile and
 * the input switches (SPORE_TEXT_INPUT, SPORE_PRESS_AND_HOLD, SPORE_GAMEPAD) have no row; they stay Terminal-only.
 * Play writes the settings and starts the game, Save writes and stays, Cancel writes nothing and exits 0. The
 * stored settings are then applied through apply_pref_environment()/gpu_profile() as on a start without it, and
 * the log is opened only after the window closes.
 *
 * Test hooks (tools/bundle/test_launcher.py): --resolve-only prints "<dir>\t<source>" (or "-\t<reason>") and
 * exits; --print-env prints the environment it would exec with; --no-panel never shows UI;
 * SPORE_LAUNCHER_TARGET replaces the game binary; SPORE_LAUNCHER_PANEL_TEST shows the open panel and cancels it;
 * SPORE_LAUNCHER_SETTINGS_TEST=play|cancel|save-then-play|reset|set:KEY=VALUE,... acts on the settings window
 * 1.5 s after it shows (set: sets rows by switch name, FPS for the pacing preset, STATE for the start state (STATE=skip
 * or SPORE_SKIP_INTRO=1 for Skip intro), RES for
 * the resolution, SPORE_FULLSCREEN_SAFE_AREA=1 for its below-the-notch item, then presses Play; reset
 * presses Reset to defaults, then Play); SPORE_LAUNCHER_SETTINGS_TAB=N opens tab N; --list-settings prints the rows; SPORE_LOG_FILE=- keeps the terminal;
 * SPORE_LOG_DIR moves the logs. Built by make_bundle.py with -DSPORE_EXE_SHA256="..." from runtime/image/include/sg_image.h. */
#import <AppKit/AppKit.h>
#include <CommonCrypto/CommonDigest.h>
#include <dirent.h>
#include <errno.h>
#include <fcntl.h>
#include <mach-o/dyld.h>
#include <stdio.h>
#include <stdlib.h>
#include <string.h>
#include <sys/mman.h>
#include <sys/stat.h>
#include <time.h>
#include <unistd.h>

#ifndef SPORE_EXE_SHA256
#error "build with -DSPORE_EXE_SHA256=\"...\" (make_bundle.py does)"
#endif
#ifndef SPORE_BUNDLE_VERSION
#define SPORE_BUNDLE_VERSION "dev"
#endif

#define CAP 2048

/* The settings domain. SPORE_DEFAULTS_DOMAIN replaces it (test_launcher.py uses a throwaway domain, so the
 * tests never read or write the player's com.spore-native.player). */
static CFStringRef pref_domain(void) {
    static CFStringRef d;
    if (!d) {
        const char *e = getenv("SPORE_DEFAULTS_DOMAIN");
        d = (e && *e) ? CFStringCreateWithCString(NULL, e, kCFStringEncodingUTF8) : NULL;
        if (!d) d = CFSTR("com.spore-native.player");
    }
    return d;
}
#define PREF_DOMAIN pref_domain()

static int g_no_panel;
static const char *g_opt_dir; /* --game-dir, already checked and remembered */
static char g_home[CAP];

/* ---- small helpers ---------------------------------------------------------------------------------- */

static int is_dir(const char *p) { struct stat st; return !stat(p, &st) && S_ISDIR(st.st_mode); }
static int is_file(const char *p) { struct stat st; return !stat(p, &st) && S_ISREG(st.st_mode); }

static void mkdir_p(const char *path) {
    char b[CAP];
    snprintf(b, sizeof b, "%s", path);
    for (char *p = b + 1; *p; p++)
        if (*p == '/') { *p = 0; mkdir(b, 0755); *p = '/'; }
    mkdir(b, 0755);
}

static void set_default_env(const char *k, const char *v) {
    const char *e = getenv(k);
    if (!e || !*e) setenv(k, v, 1);
}

static void trim(char *s) {
    size_t n = strlen(s);
    while (n && (s[n - 1] == '\n' || s[n - 1] == '\r' || s[n - 1] == ' ' || s[n - 1] == '/')) s[--n] = 0;
}

static void support_dir(char *out, size_t cap) {
    const char *s = getenv("SPORE_SUPPORT_DIR");
    if (s && *s) snprintf(out, cap, "%s", s);
    else snprintf(out, cap, "%s/Library/Application Support/spore-native", g_home);
}

/* ---- settings (CFPreferences, so the test binary outside the bundle reads the same domain) ----------- */

static int pref_string(CFStringRef key, char *out, size_t cap) {
    CFPropertyListRef v = CFPreferencesCopyAppValue(key, PREF_DOMAIN);
    int ok = 0;
    if (v && CFGetTypeID(v) == CFStringGetTypeID()) ok = CFStringGetCString((CFStringRef)v, out, (CFIndex)cap, kCFStringEncodingUTF8);
    if (v) CFRelease(v);
    return ok && out[0];
}

static void apply_pref_environment(void) {
    CFPropertyListRef v = CFPreferencesCopyAppValue(CFSTR("Environment"), PREF_DOMAIN);
    if (v && CFGetTypeID(v) == CFDictionaryGetTypeID()) {
        NSDictionary *d = (__bridge NSDictionary *)v;
        for (id k in d) {
            id val = d[k];
            if (![k isKindOfClass:[NSString class]]) continue;
            NSString *s = [val isKindOfClass:[NSString class]] ? val : [val description];
            set_default_env([k UTF8String], [s UTF8String]);
        }
    }
    if (v) CFRelease(v);
}

/* ---- game folder checks ----------------------------------------------------------------------------- */

/* 0 = good; otherwise a reason (static text). */
static const char *check_game_dir(const char *dir) {
    char p[CAP];
    if (!is_dir(dir)) return "not a folder";
    snprintf(p, sizeof p, "%s/SporebinEP1/SporeApp.exe", dir);
    if (!is_file(p)) return "no SporebinEP1/SporeApp.exe inside";
    {
        char d[CAP];
        snprintf(d, sizeof d, "%s/Data", dir);
        if (!is_dir(d)) return "no Data folder inside";
        snprintf(d, sizeof d, "%s/DataEP1", dir);
        if (!is_dir(d)) return "no DataEP1 folder inside (Galactic Adventures)";
    }
    int fd = open(p, O_RDONLY);
    struct stat st;
    if (fd < 0) return "SporebinEP1/SporeApp.exe cannot be read";
    if (fstat(fd, &st) || st.st_size <= 0) { close(fd); return "SporebinEP1/SporeApp.exe is empty"; }
    void *m = mmap(NULL, (size_t)st.st_size, PROT_READ, MAP_PRIVATE, fd, 0);
    close(fd);
    if (m == MAP_FAILED) return "SporebinEP1/SporeApp.exe cannot be mapped";
    unsigned char md[CC_SHA256_DIGEST_LENGTH];
    char hex[65];
    CC_SHA256(m, (CC_LONG)st.st_size, md);
    munmap(m, (size_t)st.st_size);
    for (int i = 0; i < 32; i++) snprintf(hex + 2 * i, 3, "%02x", md[i]);
    if (strcmp(hex, SPORE_EXE_SHA256)) return "SporebinEP1/SporeApp.exe is not the GOG Spore Galactic Adventures build";
    return NULL;
}

/* A folder the player picked: accept the install folder itself, its SporebinEP1 folder, or a folder with
 * exactly one valid install directly inside (a GOG download folder). */
static const char *normalise_pick(const char *pick, char *out, size_t cap) {
    char c[CAP];
    const char *why;
    snprintf(out, cap, "%s", pick);
    trim(out);
    if (!(why = check_game_dir(out))) return NULL;
    const char *base = strrchr(out, '/');
    if (base && !strcasecmp(base + 1, "SporebinEP1")) {
        snprintf(c, sizeof c, "%.*s", (int)(base - out), out);
        if (!check_game_dir(c)) { snprintf(out, cap, "%s", c); return NULL; }
    }
    DIR *d = opendir(out);
    if (d) {
        struct dirent *e;
        int found = 0;
        char hit[CAP] = "";
        while ((e = readdir(d))) {
            if (e->d_name[0] == '.') continue;
            snprintf(c, sizeof c, "%s/%s", out, e->d_name);
            if (is_dir(c) && !check_game_dir(c)) { found++; snprintf(hit, sizeof hit, "%s", c); }
        }
        closedir(d);
        if (found == 1) { snprintf(out, cap, "%s", hit); return NULL; }
    }
    return why;
}

static void remember(const char *dir) {
    char sup[CAP], f[CAP], tmp[CAP];
    support_dir(sup, sizeof sup);
    mkdir_p(sup);
    snprintf(f, sizeof f, "%s/game-dir", sup);
    snprintf(tmp, sizeof tmp, "%s/game-dir.tmp", sup);
    FILE *o = fopen(tmp, "w");
    if (!o) return;
    fprintf(o, "%s\n", dir);
    if (fclose(o) == 0) rename(tmp, f);
}

/* ---- UI (only touched when a panel or an alert is needed) -------------------------------------------- */

static void ui_init(void) {
    static int done;
    if (done) return;
    done = 1;
    [NSApplication sharedApplication];
    [NSApp setActivationPolicy:NSApplicationActivationPolicyRegular];
    [NSApp activateIgnoringOtherApps:YES];
}

static void alert(const char *title, const char *text) {
    fprintf(stderr, "spore-launcher: %s: %s\n", title, text);
    if (g_no_panel) return;
    @autoreleasepool {
        ui_init();
        NSAlert *a = [NSAlert new];
        a.messageText = [NSString stringWithUTF8String:title];
        a.informativeText = [NSString stringWithUTF8String:text];
        [a addButtonWithTitle:@"OK"];
        [a runModal];
    }
}

/* Returns 1 with the folder in out, 0 if the player cancelled. */
static int ask_for_folder(char *out, size_t cap) {
    if (g_no_panel) return 0;
    @autoreleasepool {
        ui_init();
        NSString *start = [NSString stringWithFormat:@"%s/Downloads", g_home];
        for (;;) {
            NSOpenPanel *p = [NSOpenPanel openPanel];
            p.canChooseDirectories = YES;
            p.canChooseFiles = NO;
            p.allowsMultipleSelection = NO;
            p.canCreateDirectories = NO;
            p.title = @"Where is Spore?";
            p.message = @"Spore needs the Spore Galactic Adventures folder from GOG: the folder that contains "
                        @"SporebinEP1, Data and DataEP1. Nothing is copied; the game is read from there.";
            p.prompt = @"Use This Folder";
            p.directoryURL = [NSURL fileURLWithPath:start isDirectory:YES];
            if (getenv("SPORE_LAUNCHER_PANEL_TEST")) /* test_launcher.py: show the panel, then cancel it */
                [p performSelector:@selector(cancel:) withObject:nil afterDelay:1.5
                           inModes:@[ NSModalPanelRunLoopMode, NSDefaultRunLoopMode ]];
            if ([p runModal] != NSModalResponseOK || !p.URL) return 0;
            const char *why = normalise_pick(p.URL.path.fileSystemRepresentation, out, cap);
            if (!why) return 1;
            char msg[CAP + 256];
            snprintf(msg, sizeof msg, "%s: %s.\n\nChoose the folder that contains SporebinEP1, Data and DataEP1 "
                                      "(the GOG Spore install).", p.URL.path.fileSystemRepresentation, why);
            alert("That folder is not a Spore install", msg);
            start = p.URL.path;
        }
    }
}

/* ---- resolution ----------------------------------------------------------------------------------------- */

/* Fills dir; returns the source ("option", "env", "remembered", "found", "picked") or NULL. */
static const char *resolve_game_dir(char *dir, size_t cap, char *reason, size_t rcap) {
    char c[CAP];
    const char *why;
    reason[0] = 0;
    if (g_opt_dir) { snprintf(dir, cap, "%s", g_opt_dir); return "option"; }
    const char *env = getenv("SPORE_GAME_DIR");
    if (env && *env) {
        if (!(why = normalise_pick(env, dir, cap))) return "env";
        snprintf(reason, rcap, "SPORE_GAME_DIR=%s: %s", env, why);
        fprintf(stderr, "spore-launcher: %s; looking elsewhere\n", reason);
    }
    {
        char sup[CAP], f[CAP];
        support_dir(sup, sizeof sup);
        snprintf(f, sizeof f, "%s/game-dir", sup);
        FILE *in = fopen(f, "r");
        if (in) {
            if (fgets(c, sizeof c, in)) {
                trim(c);
                if (c[0] && !(why = check_game_dir(c))) { fclose(in); snprintf(dir, cap, "%s", c); return "remembered"; }
                if (c[0]) fprintf(stderr, "spore-launcher: remembered folder %s: %s; looking elsewhere\n", c, why);
            }
            fclose(in);
        }
    }
    static const char *home_rel[] = {"Downloads/spore-gog", "Downloads/Spore", "Applications/Spore",
                                     "Applications/Spore Galactic Adventures", "GOG Games/Spore",
                                     "GOG Games/Spore Galactic Adventures", "Games/Spore", NULL};
    static const char *abs_paths[] = {"/Applications/Spore", "/Applications/Spore Galactic Adventures",
                                      "/Applications/GOG Galaxy/Games/Spore", "/Users/Shared/GOG Galaxy/Games/Spore",
                                      "/Users/Shared/GOG Games/Spore", NULL};
    for (int i = 0; home_rel[i]; i++) {
        snprintf(c, sizeof c, "%s/%s", g_home, home_rel[i]);
        if (is_dir(c) && !normalise_pick(c, dir, cap)) return "found";
    }
    for (int i = 0; abs_paths[i]; i++)
        if (is_dir(abs_paths[i]) && !normalise_pick(abs_paths[i], dir, cap)) return "found";
    if (ask_for_folder(dir, cap)) return "picked";
    if (!reason[0]) snprintf(reason, rcap, "no Spore install found");
    return NULL;
}

/* ---- logs -------------------------------------------------------------------------------------------- */

static int cmp_names(const void *a, const void *b) { return strcmp(*(char *const *)a, *(char *const *)b); }

static void prune_logs(const char *dir, int keep) {
    DIR *d = opendir(dir);
    if (!d) return;
    char *names[4096];
    int n = 0;
    struct dirent *e;
    while ((e = readdir(d)) && n < 4096) {
        size_t l = strlen(e->d_name);
        if (l > 4 && !strcmp(e->d_name + l - 4, ".log") && e->d_name[0] >= '0' && e->d_name[0] <= '9')
            names[n++] = strdup(e->d_name);
    }
    closedir(d);
    qsort(names, (size_t)n, sizeof names[0], cmp_names);
    for (int i = 0; i < n; i++) {
        if (i < n - keep) {
            char p[CAP];
            snprintf(p, sizeof p, "%s/%s", dir, names[i]);
            unlink(p);
        }
        free(names[i]);
    }
}

/* Redirects stdout and stderr to a new log file; returns its path in out (empty when kept on the terminal). */
static void open_log(char *out, size_t cap) {
    out[0] = 0;
    const char *lf = getenv("SPORE_LOG_FILE");
    if (lf && !strcmp(lf, "-")) return;
    char dir[CAP];
    const char *ld = getenv("SPORE_LOG_DIR");
    if (ld && *ld) snprintf(dir, sizeof dir, "%s", ld);
    else snprintf(dir, sizeof dir, "%s/Library/Logs/Spore Native", g_home);
    mkdir_p(dir);
    prune_logs(dir, 29);
    if (lf && *lf) snprintf(out, cap, "%s", lf);
    else {
        time_t t = time(NULL);
        struct tm tm;
        char stamp[64];
        localtime_r(&t, &tm);
        strftime(stamp, sizeof stamp, "%Y-%m-%d %H-%M-%S", &tm);
        snprintf(out, cap, "%s/%s.log", dir, stamp);
    }
    int fd = open(out, O_WRONLY | O_CREAT | O_APPEND, 0644);
    if (fd < 0) { out[0] = 0; return; }
    if (isatty(2)) fprintf(stderr, "spore-launcher: logging to %s\n", out);
    fflush(stdout);
    fflush(stderr);
    dup2(fd, 1);
    dup2(fd, 2);
    close(fd);
    setvbuf(stdout, NULL, _IOLBF, 0);
}

/* ---- GPU profile ---------------------------------------------------------------------------------------- */

/* Must match the table in runtime/d3d9null/src/d3d9_adapter.c. */
static const struct { const char *key; unsigned device; const char *name; } k_profiles[] = {
    {"8800gtx", 0x0191, "GeForce 8800 GTX"},
    {"9800gtx", 0x0612, "GeForce 9800 GTX"},
    {"gtx280", 0x05E1, "GeForce GTX 280"},
};

/* Looks the device id up in the install's VideoCards.txt (format: `vendor "NAME" 0xVID ...` blocks of
 * `card 0xDID "Name"` lines, closed by `end`). Read at run time; nothing from the file is kept. */
static int videocards_name(const char *game, unsigned vendor, unsigned device, char *name, size_t cap) {
    char p[CAP], line[512];
    snprintf(p, sizeof p, "%s/Data/Config/VideoCards.txt", game);
    FILE *f = fopen(p, "r");
    if (!f) return -1;
    int in_vendor = 0, found = 0;
    while (fgets(line, sizeof line, f)) {
        char *s = line;
        while (*s == ' ' || *s == '\t') s++;
        if (!strncmp(s, "vendor", 6)) {
            in_vendor = 0;
            char *q = strchr(s, '"');
            q = q ? strchr(q + 1, '"') : NULL;
            if (q) {
                char *tok = strtok(q + 1, " \t\r\n");
                while (tok && *tok != '#') {
                    if (strtoul(tok, NULL, 16) == vendor) in_vendor = 1;
                    tok = strtok(NULL, " \t\r\n");
                }
            }
        } else if (!strncmp(s, "end", 3)) in_vendor = 0;
        else if (in_vendor && !strncmp(s, "card", 4)) {
            char *e;
            unsigned long id = strtoul(s + 4, &e, 16);
            char *q1 = strchr(e, '"'), *q2 = q1 ? strchr(q1 + 1, '"') : NULL;
            if (id == device && q2) { snprintf(name, cap, "%.*s", (int)(q2 - q1 - 1), q1 + 1); found = 1; break; }
        }
    }
    fclose(f);
    return found;
}

static void gpu_profile(const char *game) {
    char pref[64];
    if (!getenv("SPORE_GPU_PROFILE") && pref_string(CFSTR("GPUProfile"), pref, sizeof pref)) setenv("SPORE_GPU_PROFILE", pref, 1);
    const char *g = getenv("SPORE_GPU_PROFILE");
    if (!g || !*g) return;
    for (size_t i = 0; i < sizeof k_profiles / sizeof k_profiles[0]; i++) {
        if (strcasecmp(g, k_profiles[i].key)) continue;
        char name[128] = "";
        int r = videocards_name(game, 0x10de, k_profiles[i].device, name, sizeof name);
        if (r == 1)
            fprintf(stderr, "spore-launcher: GPU profile %s: VideoCards.txt names NVIDIA 0x%04x \"%s\"\n", k_profiles[i].key,
                    k_profiles[i].device, name);
        else {
            fprintf(stderr, "spore-launcher: GPU profile %s: device 0x%04x is %s in this install's VideoCards.txt; "
                            "using the default profile\n", k_profiles[i].key, k_profiles[i].device, r < 0 ? "unreadable" : "absent");
            unsetenv("SPORE_GPU_PROFILE");
        }
        return;
    }
    fprintf(stderr, "spore-launcher: GPU profile '%s' is not one of 8800gtx, 9800gtx, gtx280; using the default\n", g);
    unsetenv("SPORE_GPU_PROFILE");
}

/* ---- the settings window (docs/design/bundle.md section 10) ---------------------------------------------- */

/* Every row maps to one switch. Defaults are the current behaviour, so a window left untouched writes nothing. */
enum { R_CHECK, R_POPUP, R_TEXT, R_FPS, R_GAMEDIR, R_DOCS, R_SHOW,
       /* rows that add to Arguments (the game's own command-line options, -name or -name:value) */
       R_ARGFLAG, R_ARGPOP, R_ARGRES, R_ARGLOCALE, R_ARGTEXT,
       R_ARGSTATE /* the start-state popup, -state:<Name> from k_states[] */ };
#define IS_ARG(k) ((k) >= R_ARGFLAG)

typedef struct {
    const char *tab;     /* "General", "Game options", "Diagnostics"; "Window" for the checkbox under the tabs */
    const char *label;
    int kind;
    const char *sw;      /* an environment variable, a defaults key, the game's own flag (R_ARGFLAG), or the key
                          * the set: test hook uses (STATE, WINDOW, RES, LOCALE, SOUND, ARGS) */
    const char *def;     /* checkbox: "on"/"off"; popup: the default value ("" = variable unset); text: "" */
    const char *nondef;  /* checkbox: the value written while the box is not in its default state;
                          * R_ARGRES/R_ARGLOCALE/R_ARGSTATE: the option prefix the value is written after (-r:,
                          * -locale:, -state:) */
    const char *choices; /* popup: "value=Title|value=Title" ("" value = the default, nothing written) */
    const char *note;    /* the row's tooltip, on its label and its control (one sentence; there are no visible notes) */
} srow;

/* Frame pacing presets (pace() in runtime/d3d9metal/src/mt_present.mm). With vsync on, the drawable wait paces presents at the display rate.
 * SPORE_D3D9_FPS=N does not cap at N: it is a floor of HALF a period between presents,
 *     floor_ns = 500000000 / N   (a frame back in less than that sleeps the rest)
 * so with SPORE_VSYNC=0 the rate is 1e9 / floor_ns = 2N frames/s:  N=15 -> 33.3 ms -> 30 fps,
 * N=20 -> 25 ms -> 40 fps, N=30 -> 16.7 ms -> 60 fps. N=0 turns the floor off ("Uncapped"); the game still holds
 * itself to about 59 fps with its own frameLimitMS 16 (a game setting), so Uncapped and 60
 * differ little. The default (N=60, vsync on) is only an 8.3 ms floor under the display's own pacing. */
static const struct { const char *key, *title, *vsync, *fps; } k_pace[] = {
    {"vsync", "Vsync", NULL, NULL},
    {"30", "30 fps", "0", "15"},
    {"40", "40 fps", "0", "20"},
    {"60", "60 fps", "0", "30"},
    {"uncapped", "Unlimited", "0", "0"},
};
#define N_PACE (sizeof k_pace / sizeof k_pace[0])

/* Plain labels, no flags or parentheses in labels, every explanation a one-sentence note; the first item of every
 * popup is the default (no item says "(Default)"). The graphics card profile and the input
 * switches are not rows (Terminal only); the notch is an item of the Resolution list, not a row. */
static const srow k_rows[] = {
    {"General", "Allow fullscreen", R_CHECK, "SPORE_FULLSCREEN", "on", "0", NULL,
     "Off keeps the game in a window even when it asks for fullscreen."},
    {"General", "Esc also leaves fullscreen", R_CHECK, "SPORE_FULLSCREEN_ESC", "off", "1", NULL,
     "Cmd-Ctrl-F always switches between fullscreen and a window."},
    {"General", "Full Retina resolution", R_CHECK, "SPORE_RETINA", "off", "1", NULL,
     "Sharper, but the game's menus and text are half as big."},
    {"General", "Frame rate", R_FPS, "SPORE_VSYNC+SPORE_D3D9_FPS", "vsync", NULL, NULL,
     "Menu panels slide differently below 50 fps; 40 fps is closest to how they felt on older PCs."},
    {"General", "Connect to EA's servers", R_CHECK, "SPORE_ONLINE", "on", "0", NULL,
     "Needed for Sporepedia sharing and your EA account."},
    {"General", "Spore folder", R_GAMEDIR, "game-dir", "", NULL, NULL,
     "Your GOG Spore Galactic Adventures folder, the one with SporebinEP1, Data and DataEP1."},
    {"General", "Save creations in", R_DOCS, "DocumentsFolder", "", NULL, NULL,
     "Your creations go in a \"My Spore Creations\" folder here."},
    {"Diagnostics", "Log network activity", R_POPUP, "SPORE_NET_LOG", "", NULL,
     "=Off|1=Connections|2=Connections and all data sent and received", "Writes the game's network activity to the log."},
    {"Diagnostics", "Stall watchdog", R_TEXT, "SPORE_STALL_SECONDS", "", NULL, NULL,
     "Stops a frozen game after this many seconds and logs a report."},
    {"Diagnostics", "Metal statistics", R_TEXT, "SPORE_METAL_WATCH", "", NULL, NULL,
     "How often, in seconds, frame timings are written to the log."},
    {"Diagnostics", "Keep the graphics pipeline cache", R_CHECK, "SPORE_METAL_PSO_ARCHIVE", "on", "off", NULL,
     "Off rebuilds it at every start, so scenes load more slowly."},
};

/* The "Start in" popup: the exe's -state:<Name> option starts the game in that app state instead of the
 * usual way in (names case-insensitive). One entry per popup item, in popup
 * order; {NULL} is a separator. name = what follows -state:, label = the popup title, note = the line under the
 * popup while it is selected (NULL = the default note). The names and what each does are verified from the game's
 * own start-state script SporeAppStates.trigger.
 * GalaxyGameEntry is left out: it is the normal start and plays the intro. */
#define STAGE_NOTE "No saved game is loaded; may show an empty world or crash."
static const struct { const char *name, *label, *note; } k_states[] = {
    {"CellEditor", "Cell editor", NULL},
    {"CreatureEditor", "Creature editor", NULL},
    {"CellToCreatureEditor", "Early creature editor", "The editor at the end of the cell stage, where legs are added."},
    {"BuildingEditor", "Building editor", NULL},
    {"VehicleEditor", "Vehicle editor", "A vehicle editor not tied to land, sea or air."},
    {"VehicleLandEditor", "Land vehicle editor", NULL},
    {"VehicleAirEditor", "Air vehicle editor", NULL},
    {"VehicleWaterEditor", "Sea vehicle editor", NULL},
    {"UFOEditor", "Spaceship editor", NULL},
    {"TribalAccessoriesEditor", "Tribal outfitter", NULL},
    {"AdventureAccessoriesEditor", "Captain outfitter", "The Galactic Adventures outfitter for your captain."},
    {NULL, NULL, NULL},
    {"FloraEditor", "Plant editor (experimental)", "Unfinished in the game; players report crashes."},
    {NULL, NULL, NULL},
    {"CellGame", "Cell stage (experimental)", STAGE_NOTE},
    {"CreatureGame", "Creature stage (experimental)", STAGE_NOTE},
    {"TribeGame", "Tribal stage (experimental)", STAGE_NOTE},
    {"CivilisationGame", "Civilization stage (experimental)", STAGE_NOTE},
    {"SpaceGame", "Space stage (experimental)", STAGE_NOTE},
};
#define N_STATES (sizeof k_states / sizeof k_states[0])
/* the popup's first item, and the note under it while that item (or an entry without a note) is selected */
static const char k_state_default_title[] = "Default";
/* The second item, "Skip intro", writes SPORE_SKIP_INTRO=1 (a runtime switch, DEVIATIONS 2.14) and no -state:;
 * its popup value is this sentinel, which no game state can be (the set: hook maps STATE=skip to it) */
#define STATE_SKIP @"<skip>"
static const char k_state_skip_title[] = "Skip intro";
static const char k_state_skip_note[] = "Starts at the galaxy menu without the intro movie.";
static const char k_state_default_note[] = "Choose an editor or a stage to open it without the intro.";
static const char k_state_item_note[] = "Opens straight there, skipping the intro and the galaxy menu.";
static const char k_state_custom_note[] = "Not in this list; passed to the game as it is.";

/* Group headings in the grids: the row whose switch is sw gets the heading above it. Only General has them: it
 * holds two kinds of rows; Game options reads as one list of start-up choices and Diagnostics is short. */
static const struct { const char *sw, *heading; } k_heads[] = {
    {"SPORE_FULLSCREEN", "Display"},
    {"SPORE_ONLINE", "Online and folders"},
};

/* The "Game options" tab: the exe's own command-line options a player can use.
 * Prefix - or /, names case-insensitive, a value only as -name:value in the
 * same token. They are stored in Arguments in this order and reach the game through SPORE_ARGS. */
static const srow k_game_rows[] = {
    /* first, so -state: leads the arguments */
    {"Game options", "Start in", R_ARGSTATE, "STATE+SPORE_SKIP_INTRO", "", "-state:", NULL,
     "Choose an editor or a stage to open it without the intro."}, /* = k_state_default_note; follows the selection */
    /* -f wins when both are given (checked after -w), so the popup never writes both */
    {"Game options", "Fullscreen or window", R_ARGPOP, "WINDOW", "", NULL, "=As set in the game|-w=In a window|-f=Fullscreen",
     "Overrides the fullscreen choice saved in the game's own options."},
    /* the list holds the display and (with a notch) the area below it; that item also writes
     * SPORE_FULLSCREEN_SAFE_AREA=1 into Environment, so the row names both (set: takes RES, or the variable) */
    {"Game options", "Resolution", R_ARGRES, "RES+SPORE_FULLSCREEN_SAFE_AREA", "", "-r:", NULL,
     "Sizes the display can show; the game saves the one you pick."},
    {"Game options", "Language", R_ARGLOCALE, "LOCALE", "", "-locale:", NULL,
     "The languages your Spore install has; System language follows your Mac."},
    {"Game options", "Sound", R_ARGPOP, "SOUND", "", NULL, "=As usual|-mono=Mono|-stereo=Stereo|-noSound=No sound",
     "Overrides the speaker setup, or turns the game's sound off."},
    {"Game options", "Allow several copies at once", R_ARGFLAG, "-multipleInstances", "off", NULL, NULL,
     "Lets a second copy of Spore start while one is running."},
    {"Game options", "Safe mode", R_ARGFLAG, "-safe", "off", NULL, NULL,
     "Low graphics and sound, preferences not saved, no plugins."},
    {"Game options", "Other start-up options", R_ARGTEXT, "ARGS", "", NULL, NULL,
     "Game options, separated by spaces; -5.1 gives surround sound."},
};

static const srow k_show_row = {"Window", "Show window every launch", R_SHOW, "ShowSettings", "on", "0", NULL,
                                "Off goes straight to the game; hold Option at start to see this window again."};

/* All rows in display order: the table, the game options, the show checkbox. */
static srow g_rows[64];
static int g_nrows;
static void build_rows(void) {
    if (g_nrows) return;
    for (size_t i = 0; i < sizeof k_rows / sizeof k_rows[0]; i++) g_rows[g_nrows++] = k_rows[i];
    for (size_t i = 0; i < sizeof k_game_rows / sizeof k_game_rows[0]; i++) g_rows[g_nrows++] = k_game_rows[i];
    g_rows[g_nrows++] = k_show_row;
}

static NSString *S0(const char *s) { return s ? [NSString stringWithUTF8String:s] : @""; }

/* ---- the main display, for the Resolution list ---- */

/* The menu-bar display (the one the runtime reports to the game): its frame in points, the notch inset
 * (safeAreaInsets.top, 0 without a notch, so always 0 on an external display) and the backing scale. All zero when
 * there is no screen. */
static void main_display(double *w, double *h, double *inset, double *scale) {
    *w = *h = *inset = 0;
    *scale = 1;
    NSArray<NSScreen *> *ss = NSScreen.screens;
    if (!ss.count) return;
    NSScreen *m = ss[0];
    *w = m.frame.size.width;
    *h = m.frame.size.height;
    *inset = m.safeAreaInsets.top;
    *scale = m.backingScaleFactor > 0 ? m.backingScaleFactor : 1;
}

/* The common sizes offered when they fit inside the display in the current units. */
static const struct { unsigned w, h; } k_res[] = {
    {2560, 1600}, {2560, 1440}, {1920, 1200}, {1920, 1080}, {1680, 1050},
    {1600, 900}, {1440, 900}, {1280, 800}, {1280, 720}, {1024, 768},
};

/* The two display items in the current units (points, or pixels with Retina-native rendering, DEVIATIONS 4.8):
 * "Display (WxH)" and, only with a notch, "Display below the notch (WxH)" (the frame less safeAreaInsets.top).
 * nil when there is no screen, or (the notch item) no notch. Choosing the notch item also writes
 * SPORE_FULLSCREEN_SAFE_AREA=1 (DEVIATIONS 4.12); the desktop mode the game is told is the real
 * display (or its safe area) in both units (DEVIATIONS 3.47), so both items are sizes fullscreen accepts. */
static NSString *res_display_item(BOOL pixels) {
    double w, h, inset, scale;
    main_display(&w, &h, &inset, &scale);
    double f = pixels ? scale : 1;
    if (w <= 0 || h <= 0) return nil;
    return [NSString stringWithFormat:@"Display (%ldx%ld)", lround(w * f), lround(h * f)];
}
static NSString *res_notch_item(BOOL pixels) {
    double w, h, inset, scale;
    main_display(&w, &h, &inset, &scale);
    double f = pixels ? scale : 1;
    if (w <= 0 || h <= 0 || inset <= 0) return nil;
    return [NSString stringWithFormat:@"Display below the notch (%ldx%ld)", lround(w * f), lround((h - inset) * f)];
}

/* The Resolution list: the display, the area below the notch (if any), then the common sizes that fit the display. */
static NSArray<NSString *> *resolution_items(BOOL pixels) {
    double w, h, inset, scale;
    main_display(&w, &h, &inset, &scale);
    double f = pixels ? scale : 1;
    NSMutableArray *out = [NSMutableArray array];
    NSString *d = res_display_item(pixels), *n = res_notch_item(pixels);
    if (!d) return out;
    [out addObject:d];
    if (n) [out addObject:n];
    long W = lround(w * f), H = lround(h * f);
    for (size_t k = 0; k < sizeof k_res / sizeof k_res[0]; k++)
        if ((long)k_res[k].w <= W && (long)k_res[k].h <= H && !(k_res[k].w == W && k_res[k].h == H))
            [out addObject:[NSString stringWithFormat:@"%ux%u", k_res[k].w, k_res[k].h]];
    return out;
}

/* A switch as the game will get it: the launcher's environment, else the stored Environment ("" and "0" = off). */
static BOOL switch_on(const char *name) {
    NSString *v = nil;
    const char *e = getenv(name);
    if (e && *e) v = S0(e);
    else {
        CFPropertyListRef d = CFPreferencesCopyAppValue(CFSTR("Environment"), PREF_DOMAIN);
        if (d && CFGetTypeID(d) == CFDictionaryGetTypeID()) {
            id x = ((__bridge NSDictionary *)d)[S0(name)];
            v = [x isKindOfClass:[NSString class]] ? x : [x description];
        }
        if (d) CFRelease(d);
    }
    return v.length && ![v isEqualToString:@"0"];
}

/* WIDTHxHEIGHT from a typed value or a display item ("Display (WxH)", "Display below the notch (WxH)"); nil when it
 * is neither. */
static NSString *res_value(NSString *t) {
    t = [t stringByTrimmingCharactersInSet:NSCharacterSet.whitespaceCharacterSet];
    for (NSString *pre in @[ @"Display below the notch (", @"Display (" ])
        if ([t hasPrefix:pre] && [t hasSuffix:@")"]) { t = [t substringWithRange:NSMakeRange(pre.length, t.length - pre.length - 1)]; break; }
    unsigned w = 0, h = 0;
    int n = 0;
    if (!t.length || [t characterAtIndex:0] < '0' || [t characterAtIndex:0] > '9') return nil;
    if (sscanf(t.UTF8String, "%ux%u%n", &w, &h, &n) != 2 || !w || !h || w >= 100000 || h >= 100000) return nil;
    if ((NSUInteger)n != strlen(t.UTF8String)) return nil;
    return [NSString stringWithFormat:@"%ux%u", w, h];
}

/* A language code (a Data/Locale folder name, ll-cc) as a player reads it: "French (France)". */
static NSString *locale_title(NSString *code) {
    NSString *n = [[NSLocale localeWithLocaleIdentifier:@"en_US"] displayNameForKey:NSLocaleIdentifier
                                                                               value:[code stringByReplacingOccurrencesOfString:@"-" withString:@"_"]];
    return n.length ? n : code;
}

/* The language codes this install has: the lowercase ll-cc folder names under <game>/Data/Locale. */
static NSArray<NSString *> *locale_codes(const char *game) {
    NSMutableArray *out = [NSMutableArray array];
    if (!game || !*game) return out;
    NSString *dir = [NSString stringWithFormat:@"%s/Data/Locale", game];
    for (NSString *n in [[NSFileManager defaultManager] contentsOfDirectoryAtPath:dir error:NULL]) {
        NSString *l = n.lowercaseString;
        BOOL isdir = NO;
        if (l.length == 5 && [l characterAtIndex:2] == '-' &&
            [[NSFileManager defaultManager] fileExistsAtPath:[dir stringByAppendingPathComponent:n] isDirectory:&isdir] && isdir)
            [out addObject:l];
    }
    [out sortUsingSelector:@selector(compare:)];
    return out;
}

/* game: the resolved Spore folder or NULL (the language row then lists no codes) */
static void list_settings(const char *game) {
    build_rows();
    for (int i = 0; i < g_nrows; i++) {
        const srow *r = &g_rows[i];
        char choices[2048] = "";
        switch (r->kind) {
        case R_CHECK:
            snprintf(choices, sizeof choices, "on|off (%s writes %s)", !strcmp(r->def, "on") ? "off" : "on", r->nondef);
            break;
        case R_ARGFLAG: snprintf(choices, sizeof choices, "on|off (on adds %s to Arguments)", r->sw); break;
        case R_SHOW: snprintf(choices, sizeof choices, "on|off (off writes false)"); break;
        case R_ARGLOCALE:
            snprintf(choices, sizeof choices, "(system)");
            for (NSString *c in locale_codes(game))
                snprintf(choices + strlen(choices), sizeof choices - strlen(choices), "|%s", c.UTF8String);
            break;
        case R_ARGRES: { /* the list as the window shows it for the current SPORE_RETINA; " -- " ends the items */
            BOOL px = switch_on("SPORE_RETINA");
            double w, h, inset, scale;
            main_display(&w, &h, &inset, &scale);
            for (NSString *it in resolution_items(px))
                snprintf(choices + strlen(choices), sizeof choices - strlen(choices), "%s%s", choices[0] ? "|" : "", it.UTF8String);
            snprintf(choices + strlen(choices), sizeof choices - strlen(choices),
                     " -- %s; this display %.0fx%.0f points, notch inset %.0f points; any WIDTHxHEIGHT typed; writes -r:WxH, "
                     "the below-the-notch item also SPORE_FULLSCREEN_SAFE_AREA=1; empty = the game's",
                     px ? "pixels" : "points", w, h, inset);
            break;
        }
        case R_ARGSTATE:
            snprintf(choices, sizeof choices, "(unset)|skip");
            for (size_t k = 0; k < N_STATES; k++)
                if (k_states[k].name)
                    snprintf(choices + strlen(choices), sizeof choices - strlen(choices), "|%s", k_states[k].name);
            break;
        case R_POPUP: case R_ARGPOP: {
            char t[512];
            snprintf(t, sizeof t, "%s", r->choices);
            for (char *s = strtok(t, "|"); s; s = strtok(NULL, "|")) {
                char *eq = strchr(s, '=');
                if (eq) *eq = 0;
                snprintf(choices + strlen(choices), sizeof choices - strlen(choices), "%s%s", choices[0] ? "|" : "",
                         s[0] ? s : "(unset)");
            }
            break;
        }
        case R_FPS:
            for (size_t k = 0; k < N_PACE; k++)
                snprintf(choices + strlen(choices), sizeof choices - strlen(choices), "%s%s", k ? "|" : "", k_pace[k].key);
            break;
        case R_TEXT: snprintf(choices, sizeof choices, "number (empty = off)"); break;
        case R_ARGTEXT: snprintf(choices, sizeof choices, "text"); break;
        default: snprintf(choices, sizeof choices, "folder"); break;
        }
        const char *def = r->def[0] ? r->def : (r->kind == R_POPUP || r->kind == R_ARGPOP || r->kind == R_ARGSTATE ? "(unset)" : "(empty)");
        if (r->kind == R_ARGLOCALE) def = "(system)";
        if (r->kind == R_GAMEDIR) def = "(found or remembered)";
        if (r->kind == R_DOCS) def = "~/Documents";
        printf("%s\t%s\t%s\t%s\t%s\t%s\n", r->tab, r->label, r->sw, def, choices, r->note ? r->note : "");
    }
}

static int pref_show_settings(void) {
    Boolean valid = false;
    Boolean v = CFPreferencesGetAppBooleanValue(CFSTR("ShowSettings"), PREF_DOMAIN, &valid);
    return !valid || v;
}

/* Splits the "Extra arguments" text at blanks; '...' and "..." group (no escapes). */
static NSArray<NSString *> *split_args(NSString *s) {
    NSMutableArray *out = [NSMutableArray array];
    NSMutableString *cur = nil;
    unichar q = 0;
    for (NSUInteger i = 0; i < s.length; i++) {
        unichar c = [s characterAtIndex:i];
        if (q) { if (c == q) q = 0; else [cur appendFormat:@"%C", c]; continue; }
        if (c == ' ' || c == '\t' || c == '\n') { if (cur) [out addObject:cur]; cur = nil; continue; }
        if (!cur) cur = [NSMutableString string];
        if (c == '"' || c == '\'') q = c; else [cur appendFormat:@"%C", c];
    }
    if (cur) [out addObject:cur];
    return out;
}

/* Single-quotes a word for sh when it needs it. */
static NSString *sh_quote(NSString *s) {
    NSCharacterSet *safe = [NSCharacterSet characterSetWithCharactersInString:
        @"abcdefghijklmnopqrstuvwxyzABCDEFGHIJKLMNOPQRSTUVWXYZ0123456789-_./=:,+@%"];
    if (s.length && [s rangeOfCharacterFromSet:safe.invertedSet].location == NSNotFound) return s;
    return [NSString stringWithFormat:@"'%@'", [s stringByReplacingOccurrencesOfString:@"'" withString:@"'\\''"]];
}

/* The arguments a game-options row adds for value v. */
static NSArray<NSString *> *arg_tokens(const srow *r, NSString *v) {
    if (r->kind == R_ARGTEXT) return split_args(v);
    if (r->kind == R_ARGRES || r->kind == R_ARGLOCALE || r->kind == R_ARGSTATE)
        return @[ [@(r->nondef) stringByAppendingString:v] ];
    return @[ v ];
}

/* One argument as the game's tokenizer reads it (0x92bab0: split at blanks outside double quotes, no escapes): a
 * value with blanks is quoted after the colon, -name:"C:\My Data"; a bare word with blanks is quoted whole. */
static NSString *arg_quote(NSString *a) {
    if ([a rangeOfCharacterFromSet:NSCharacterSet.whitespaceAndNewlineCharacterSet].location == NSNotFound) return a;
    NSRange c = [a rangeOfString:@":"];
    if ([a hasPrefix:@"-"] && c.location != NSNotFound)
        return [NSString stringWithFormat:@"%@\"%@\"", [a substringToIndex:c.location + 1], [a substringFromIndex:c.location + 1]];
    return [NSString stringWithFormat:@"\"%@\"", a];
}

/* The game's command line (SPORE_ARGS, which runtime/win32 w32_module.c appends to the exe path): the arguments
 * joined with single spaces. The exe widens it byte by byte, so only ASCII is passed; anything else is dropped
 * with a message, as is a double quote inside an argument (the tokenizer has no escape for it). */
static NSString *game_cmdline(NSArray<NSString *> *args) {
    NSMutableArray *out = [NSMutableArray array];
    for (NSString *a in args) {
        BOOL ok = a.length > 0;
        for (NSUInteger i = 0; ok && i < a.length; i++) {
            unichar ch = [a characterAtIndex:i];
            ok = ch >= 0x20 && ch < 0x7f && ch != '"';
        }
        if (ok) [out addObject:arg_quote(a)];
        else fprintf(stderr, "spore-launcher: game argument '%s' dropped (only ASCII, no double quotes)\n", a.UTF8String);
    }
    return [out componentsJoinedByString:@" "];
}

@interface SNSettings : NSObject <NSWindowDelegate, NSComboBoxDelegate>
@end

@implementation SNSettings {
@public
    NSWindow *_win;
    NSMutableArray *_ctl;          /* one control per g_rows entry */
    NSMutableArray *_pathLabel;    /* R_GAMEDIR / R_DOCS: the label showing the folder (else NSNull) */
    NSDictionary *_env0;           /* the launcher's own environment, before any setting was applied */
    NSString *_gameDir, *_docsDir; /* current choices (docs: "" = ~/Documents) */
    BOOL _gameChanged;
    NSString *_launcher;
    NSArray *_stateTip;            /* the start-state row's label and popup: their tooltip follows the selection */
    BOOL _resPixels;               /* the units the Resolution list is in (follows the Retina checkbox) */
}

static NSString *S(const char *s) { return s ? [NSString stringWithUTF8String:s] : @""; }

- (BOOL)presetByEnv:(const srow *)r {
    if (r->kind == R_FPS) return [_env0[@"SPORE_VSYNC"] length] || [_env0[@"SPORE_D3D9_FPS"] length];
    if (r->kind == R_DOCS) return [_env0[@"SPORE_DOCUMENTS_DIR"] length] > 0;
    if (r->kind == R_GAMEDIR) return [_env0[@"SPORE_GAME_DIR"] length] > 0 || g_opt_dir;
    if (IS_ARG(r->kind)) return [_env0[@"SPORE_ARGS"] length] > 0; /* SPORE_ARGS set: the game gets that */
    if (!strncmp(r->sw, "SPORE_", 6)) return [_env0[S(r->sw)] length] > 0;
    return NO;
}

- (NSDictionary *)storedEnv {
    CFPropertyListRef v = CFPreferencesCopyAppValue(CFSTR("Environment"), PREF_DOMAIN);
    if (!v) return @{};
    if (CFGetTypeID(v) != CFDictionaryGetTypeID()) { CFRelease(v); return @{}; }
    return (__bridge_transfer NSDictionary *)v;
}

- (NSArray *)storedArgs {
    CFPropertyListRef v = CFPreferencesCopyAppValue(CFSTR("Arguments"), PREF_DOMAIN);
    if (!v) return @[];
    if (CFGetTypeID(v) != CFArrayGetTypeID()) { CFRelease(v); return @[]; }
    return (__bridge_transfer NSArray *)v;
}

static NSString *str_of(id v) { return !v ? nil : [v isKindOfClass:[NSString class]] ? v : [v description]; }

/* ---- controls <-> values ---- */

- (void)popup:(NSPopUpButton *)p select:(NSString *)value {
    for (NSMenuItem *it in p.itemArray)
        if (it.representedObject && [it.representedObject caseInsensitiveCompare:value] == NSOrderedSame) { [p selectItem:it]; return; }
    /* a stored value the table does not know: keep it as a choice so Save writes it back unchanged */
    [p addItemWithTitle:[NSString stringWithFormat:@"%@ (custom)", value]];
    p.lastItem.representedObject = value;
    [p selectItem:p.lastItem];
}

- (void)setRow:(int)i value:(NSString *)v {  /* v nil = the default */
    const srow *r = &g_rows[i];
    id c = _ctl[i];
    switch (r->kind) {
    case R_CHECK: case R_ARGFLAG: case R_SHOW: {
        BOOL defOn = !strcmp(r->def, "on");
        BOOL nondef = v && ([v isEqualToString:S(r->nondef)] || [v isEqualToString:defOn ? @"off" : @"on"] ||
                            (r->kind == R_ARGFLAG && ([v isEqualToString:@"1"] || [v caseInsensitiveCompare:S(r->sw)] == NSOrderedSame)));
        ((NSButton *)c).state = (defOn ^ nondef) ? NSControlStateValueOn : NSControlStateValueOff;
        break;
    }
    case R_POPUP: case R_FPS: case R_ARGLOCALE: [self popup:c select:v ? v : S(r->def)]; break;
    case R_ARGSTATE: [self popup:c select:v ? v : S(r->def)]; [self stateChanged:c]; break;
    case R_ARGPOP: /* the set: hook may name the option without its dash (WINDOW=f, SOUND=mono) */
        [self popup:c select:!v ? S(r->def) : (v.length && ![v hasPrefix:@"-"]) ? [@"-" stringByAppendingString:v] : v];
        break;
    case R_TEXT: case R_ARGTEXT: case R_ARGRES: ((NSTextField *)c).stringValue = v ? v : @""; break;
    case R_DOCS: _docsDir = v ? v : @""; [self showPaths]; break;
    case R_GAMEDIR: break;
    }
}

/* The value a row would write; nil = the default (nothing written). */
- (NSString *)valueOfRow:(int)i {
    const srow *r = &g_rows[i];
    id c = _ctl[i];
    switch (r->kind) {
    case R_CHECK: case R_ARGFLAG: case R_SHOW: {
        BOOL on = ((NSButton *)c).state == NSControlStateValueOn;
        return on == !strcmp(r->def, "on") ? nil : r->kind == R_ARGFLAG ? S(r->sw) : S(r->nondef);
    }
    case R_ARGRES: /* WIDTHxHEIGHT, both positive integers (a display item gives its size); anything else is not written */
        return res_value(((NSTextField *)c).stringValue);
    case R_POPUP: case R_FPS: case R_ARGPOP: case R_ARGLOCALE: case R_ARGSTATE: {
        NSString *v = ((NSPopUpButton *)c).selectedItem.representedObject;
        return !v || [v isEqualToString:S(r->def)] ? nil : v;
    }
    case R_TEXT: {
        NSString *s = [((NSTextField *)c).stringValue stringByTrimmingCharactersInSet:NSCharacterSet.whitespaceCharacterSet];
        char *end = NULL;
        double d = s.length ? strtod(s.UTF8String, &end) : 0;
        return s.length && end && !*end && d > 0 ? s : nil;
    }
    case R_ARGTEXT: {
        NSString *s = [((NSTextField *)c).stringValue stringByTrimmingCharactersInSet:NSCharacterSet.whitespaceAndNewlineCharacterSet];
        return s.length ? s : nil;
    }
    case R_DOCS: return _docsDir.length ? _docsDir : nil;
    default: return nil;
    }
}

/* The FPS popup's values are preset keys; the stored form is two variables. A pair the presets do not cover
 * shows as a custom choice "vsync=X fps=Y". */
static NSString *pace_key(NSString *vs, NSString *fps) {
    if (!vs.length && !fps.length) return @"vsync";
    for (size_t k = 1; k < N_PACE; k++)
        if ([vs isEqualToString:S(k_pace[k].vsync)] && [fps isEqualToString:S(k_pace[k].fps)]) return S(k_pace[k].key);
    return [NSString stringWithFormat:@"SPORE_VSYNC=%@ SPORE_D3D9_FPS=%@", vs ?: @"", fps ?: @""];
}

- (void)loadStored {
    /* a row whose variable the launcher's environment sets shows that value (disabled); the rest the stored one */
    NSMutableDictionary *env = [[self storedEnv] mutableCopy];
    for (int i = 0; i < g_nrows; i++)
        for (NSString *n in [S(g_rows[i].sw) componentsSeparatedByString:@"+"])
            if ([n hasPrefix:@"SPORE_"] && [_env0[n] length]) env[n] = _env0[n];
    /* each stored argument goes to the row that writes it (one per row); the rest fill "Extra arguments" */
    NSArray *args = [_env0[@"SPORE_ARGS"] length] ? split_args(_env0[@"SPORE_ARGS"]) : [self storedArgs];
    NSMutableArray *extra = [NSMutableArray array];
    NSMutableDictionary *argval = [NSMutableDictionary dictionary]; /* row index -> value */
    for (id a in args) {
        NSString *tok = str_of(a), *val = nil;
        int row = -1;
        for (int i = 0; i < g_nrows && row < 0; i++) {
            const srow *r = &g_rows[i];
            if (argval[@(i)]) continue;
            if (r->kind == R_ARGFLAG && [tok caseInsensitiveCompare:S(r->sw)] == NSOrderedSame) row = i, val = S(r->sw);
            else if ((r->kind == R_ARGRES || r->kind == R_ARGLOCALE || r->kind == R_ARGSTATE) && tok.length > strlen(r->nondef) &&
                     [[tok substringToIndex:strlen(r->nondef)] caseInsensitiveCompare:S(r->nondef)] == NSOrderedSame)
                row = i, val = [tok substringFromIndex:strlen(r->nondef)];
            else if (r->kind == R_ARGPOP)
                for (NSString *c in [S(r->choices) componentsSeparatedByString:@"|"]) {
                    NSString *cv = [c substringToIndex:[c rangeOfString:@"="].location];
                    if (cv.length && [tok caseInsensitiveCompare:cv] == NSOrderedSame) { row = i, val = cv; break; }
                }
        }
        if (row >= 0) argval[@(row)] = val; else [extra addObject:arg_quote(tok)];
    }
    char buf[CAP];
    for (int i = 0; i < g_nrows; i++) {
        const srow *r = &g_rows[i];
        switch (r->kind) {
        case R_CHECK: case R_POPUP: case R_TEXT: [self setRow:i value:str_of(env[S(r->sw)])]; break;
        case R_FPS: [self setRow:i value:pace_key(str_of(env[@"SPORE_VSYNC"]), str_of(env[@"SPORE_D3D9_FPS"]))]; break;
        case R_DOCS:
            if ([self presetByEnv:r]) [self setRow:i value:_env0[@"SPORE_DOCUMENTS_DIR"]];
            else [self setRow:i value:pref_string(CFSTR("DocumentsFolder"), buf, sizeof buf) ? S(buf) : nil];
            break;
        case R_ARGSTATE: { /* a stored SPORE_SKIP_INTRO with no -state: is the Skip intro item */
            NSString *sk = str_of(env[@"SPORE_SKIP_INTRO"]);
            [self setRow:i value:argval[@(i)] ?: (sk.length && ![sk isEqualToString:@"0"] ? STATE_SKIP : nil)];
            break;
        }
        case R_ARGFLAG: case R_ARGPOP: case R_ARGRES: case R_ARGLOCALE: [self setRow:i value:argval[@(i)]]; break;
        case R_ARGTEXT: [self setRow:i value:extra.count ? [extra componentsJoinedByString:@" "] : nil]; break;
        case R_SHOW: [self setRow:i value:pref_show_settings() ? nil : @"0"]; break;
        }
    }
    [self refillResolutions:nil];
    /* a stored -r: equal to the area below the notch, with SPORE_FULLSCREEN_SAFE_AREA set, is that item; one equal to
     * the display is the display item; anything else stays as typed */
    NSString *sa = str_of(env[@"SPORE_FULLSCREEN_SAFE_AREA"]);
    [self matchResolution:sa.length && ![sa isEqualToString:@"0"]];
}

- (NSComboBox *)resBox {
    for (int i = 0; i < g_nrows; i++)
        if (g_rows[i].kind == R_ARGRES) return _ctl[i];
    return nil;
}

/* YES when the Resolution box holds the below-the-notch item (the one choice that writes SPORE_FULLSCREEN_SAFE_AREA) */
- (BOOL)resBelowNotch {
    NSString *n = res_notch_item(_resPixels);
    return n && [[self resBox].stringValue isEqualToString:n];
}

- (void)matchResolution:(BOOL)safe {
    NSComboBox *b = [self resBox];
    NSString *v = res_value(b.stringValue), *n = res_notch_item(_resPixels), *d = res_display_item(_resPixels);
    if (!v) return;
    if (safe && n && [res_value(n) isEqualToString:v]) b.stringValue = n;
    else if (d && [res_value(d) isEqualToString:v]) b.stringValue = d;
}

- (void)writeSettings {
    NSMutableDictionary *env = [[self storedEnv] mutableCopy];
    NSMutableArray *args = [NSMutableArray array];
    for (int i = 0; i < g_nrows; i++) {
        const srow *r = &g_rows[i];
        if ([self presetByEnv:r]) continue; /* the environment wins; the stored value is kept as it was */
        NSString *v = [self valueOfRow:i];
        switch (r->kind) {
        case R_CHECK: case R_POPUP: case R_TEXT:
            if (v) env[S(r->sw)] = v; else [env removeObjectForKey:S(r->sw)];
            break;
        case R_FPS: {
            NSString *key = v ?: @"vsync";
            size_t k = 0;
            while (k < N_PACE && ![key isEqualToString:S(k_pace[k].key)]) k++;
            if (k == N_PACE) break; /* a custom pair: left as stored */
            if (k_pace[k].vsync) env[@"SPORE_VSYNC"] = S(k_pace[k].vsync); else [env removeObjectForKey:@"SPORE_VSYNC"];
            if (k_pace[k].fps) env[@"SPORE_D3D9_FPS"] = S(k_pace[k].fps); else [env removeObjectForKey:@"SPORE_D3D9_FPS"];
            break;
        }
        case R_DOCS: CFPreferencesSetAppValue(CFSTR("DocumentsFolder"), (__bridge CFStringRef)v, PREF_DOMAIN); break;
        case R_ARGRES: /* the below-the-notch item also writes SPORE_FULLSCREEN_SAFE_AREA; anything else removes it */
            if (![_env0[@"SPORE_FULLSCREEN_SAFE_AREA"] length]) {
                if (v && [self resBelowNotch]) env[@"SPORE_FULLSCREEN_SAFE_AREA"] = @"1";
                else [env removeObjectForKey:@"SPORE_FULLSCREEN_SAFE_AREA"];
            }
            if (v) [args addObjectsFromArray:arg_tokens(r, v)];
            break;
        case R_ARGSTATE: /* Skip intro writes SPORE_SKIP_INTRO=1 and no -state:; any other choice removes it */
            if (![_env0[@"SPORE_SKIP_INTRO"] length]) {
                if ([v isEqualToString:STATE_SKIP]) env[@"SPORE_SKIP_INTRO"] = @"1";
                else [env removeObjectForKey:@"SPORE_SKIP_INTRO"];
            }
            if (v && ![v isEqualToString:STATE_SKIP]) [args addObjectsFromArray:arg_tokens(r, v)];
            break;
        case R_ARGFLAG: case R_ARGPOP: case R_ARGLOCALE: case R_ARGTEXT:
            if (v) [args addObjectsFromArray:arg_tokens(r, v)];
            break;
        case R_SHOW: CFPreferencesSetAppValue(CFSTR("ShowSettings"), v ? kCFBooleanFalse : NULL, PREF_DOMAIN); break;
        case R_GAMEDIR: if (_gameChanged) { remember(_gameDir.fileSystemRepresentation); _gameChanged = NO; } break;
        }
    }
    CFPreferencesSetAppValue(CFSTR("Environment"), env.count ? (__bridge CFDictionaryRef)env : NULL, PREF_DOMAIN);
    /* SPORE_ARGS from the environment: the game options rows were disabled and the stored Arguments stay */
    if (![_env0[@"SPORE_ARGS"] length])
        CFPreferencesSetAppValue(CFSTR("Arguments"), args.count ? (__bridge CFArrayRef)args : NULL, PREF_DOMAIN);
    if (!CFPreferencesAppSynchronize(PREF_DOMAIN)) fprintf(stderr, "spore-launcher: could not write the settings\n");
}

/* ---- actions ---- */

- (void)play:(id)sender { [self writeSettings]; [NSApp stopModalWithCode:1]; }
- (void)cancel:(id)sender { [NSApp stopModalWithCode:0]; }
- (void)save:(id)sender { [self writeSettings]; }
- (BOOL)windowShouldClose:(NSWindow *)w { [self cancel:nil]; return YES; }

- (void)reset:(id)sender {
    for (int i = 0; i < g_nrows; i++)
        if (![self presetByEnv:&g_rows[i]]) [self setRow:i value:nil];
}

- (void)copyCommand:(id)sender {
    NSMutableArray *w = [NSMutableArray array], *args = [NSMutableArray array];
    for (int i = 0; i < g_nrows; i++) {
        const srow *r = &g_rows[i];
        NSString *v = [self presetByEnv:r] ? nil : [self valueOfRow:i];
        if (!v) continue;
        switch (r->kind) {
        case R_CHECK: case R_POPUP: case R_TEXT: [w addObject:[NSString stringWithFormat:@"%s=%@", r->sw, sh_quote(v)]]; break;
        case R_FPS:
            for (size_t k = 1; k < N_PACE; k++)
                if ([v isEqualToString:S(k_pace[k].key)])
                    [w addObject:[NSString stringWithFormat:@"SPORE_VSYNC=%s SPORE_D3D9_FPS=%s", k_pace[k].vsync, k_pace[k].fps]];
            break;
        case R_DOCS: [w addObject:[@"SPORE_DOCUMENTS_DIR=" stringByAppendingString:sh_quote(v)]]; break;
        case R_ARGRES:
            if ([self resBelowNotch]) [w addObject:@"SPORE_FULLSCREEN_SAFE_AREA=1"];
            [args addObjectsFromArray:arg_tokens(r, v)];
            break;
        case R_ARGSTATE:
            if ([v isEqualToString:STATE_SKIP]) [w addObject:@"SPORE_SKIP_INTRO=1"];
            else [args addObjectsFromArray:arg_tokens(r, v)];
            break;
        case R_ARGFLAG: case R_ARGPOP: case R_ARGLOCALE: case R_ARGTEXT:
            [args addObjectsFromArray:arg_tokens(r, v)];
            break;
        default: break;
        }
    }
    /* the game reads its command line from SPORE_ARGS; the same arguments also follow "--" */
    if (args.count) [w addObject:[@"SPORE_ARGS=" stringByAppendingString:sh_quote(game_cmdline(args))]];
    [w addObject:sh_quote(_launcher)];
    [w addObject:@"--no-settings"];
    if (args.count) {
        [w addObject:@"--"];
        for (NSString *a in args) [w addObject:sh_quote(a)];
    }
    NSString *line = [w componentsJoinedByString:@" "];
    [NSPasteboard.generalPasteboard clearContents];
    [NSPasteboard.generalPasteboard setString:line forType:NSPasteboardTypeString];
    if (getenv("SPORE_LAUNCHER_SETTINGS_TEST")) fprintf(stderr, "spore-launcher: launch command: %s\n", line.UTF8String);
}

/* The Resolution box's list follows the Retina checkbox (points or pixels) and is rebuilt when it changes. A display
 * item in the box becomes the same item in the new units; a typed size is kept. */
- (void)refillResolutions:(id)sender {
    BOOL pixels = NO;
    for (int i = 0; i < g_nrows; i++)
        if (!strcmp(g_rows[i].sw, "SPORE_RETINA")) pixels = ((NSButton *)_ctl[i]).state == NSControlStateValueOn;
    NSComboBox *b = [self resBox];
    if (!b) return;
    NSString *keep = b.stringValue;
    if ([keep isEqualToString:res_notch_item(_resPixels) ?: @""]) keep = res_notch_item(pixels) ?: res_display_item(pixels);
    else if ([keep isEqualToString:res_display_item(_resPixels) ?: @""]) keep = res_display_item(pixels);
    _resPixels = pixels;
    [b removeAllItems];
    [b addItemsWithObjectValues:resolution_items(pixels)];
    b.stringValue = keep ?: @"";
}

/* the start-state row's tooltip follows the selection */
- (void)stateChanged:(NSPopUpButton *)p {
    NSString *v = p.selectedItem.representedObject;
    const char *note = k_state_default_note;
    if ([v isEqualToString:STATE_SKIP]) note = k_state_skip_note;
    else if (v.length) {
        note = k_state_custom_note;
        for (size_t k = 0; k < N_STATES; k++)
            if (k_states[k].name && [v caseInsensitiveCompare:S(k_states[k].name)] == NSOrderedSame)
                note = k_states[k].note ? k_states[k].note : k_state_item_note;
    }
    for (NSView *x in _stateTip) x.toolTip = S(note);
}

- (void)showPaths {
    for (int i = 0; i < g_nrows; i++) {
        NSTextField *l = _pathLabel[i];
        if ((id)l == [NSNull null]) continue;
        NSString *p = g_rows[i].kind == R_GAMEDIR ? _gameDir : (_docsDir.length ? _docsDir : @"~/Documents");
        l.stringValue = p;
        l.toolTip = p;
    }
}

- (void)changeGameDir:(id)sender {
    char out[CAP];
    if (ask_for_folder(out, sizeof out)) { _gameDir = S(out); _gameChanged = YES; [self showPaths]; [self refillLocales]; }
}

/* the language popup: "System language", then the chosen install's Data/Locale folders by their names
 * ("French (France)"), sorted by name; the value written is the folder's code */
- (void)fillLocales:(NSPopUpButton *)p {
    while (p.numberOfItems > 1) [p removeItemAtIndex:1];
    NSArray *codes = [locale_codes(_gameDir.fileSystemRepresentation) sortedArrayUsingComparator:^NSComparisonResult(id x, id y) {
        return [locale_title(x) localizedCaseInsensitiveCompare:locale_title(y)];
    }];
    for (NSString *code in codes) {
        [p addItemWithTitle:locale_title(code)];
        p.lastItem.representedObject = code;
    }
}

/* after Change... of the game folder; the selection is kept */
- (void)refillLocales {
    for (int i = 0; i < g_nrows; i++) {
        if (g_rows[i].kind != R_ARGLOCALE) continue;
        NSPopUpButton *p = _ctl[i];
        NSString *sel = p.selectedItem.representedObject ?: @"";
        [self fillLocales:p];
        [self popup:p select:sel];
    }
}

- (void)changeDocsDir:(id)sender {
    NSOpenPanel *p = [NSOpenPanel openPanel];
    p.canChooseDirectories = YES;
    p.canChooseFiles = NO;
    p.canCreateDirectories = YES;
    p.message = @"Choose the folder that holds \"My Spore Creations\".";
    p.prompt = @"Use This Folder";
    if ([p runModal] == NSModalResponseOK && p.URL) { _docsDir = p.URL.path; [self showPaths]; }
}

/* ---- test hooks (SPORE_LAUNCHER_SETTINGS_TEST) ---- */

- (int)rowForKey:(NSString *)k {
    for (int i = 0; i < g_nrows; i++) {
        const srow *r = &g_rows[i];
        if ([[S(r->sw) componentsSeparatedByString:@"+"] containsObject:k] || (r->kind == R_FPS && [k isEqualToString:@"FPS"])) return i;
    }
    return -1;
}

- (void)testHook {
    NSString *t = S(getenv("SPORE_LAUNCHER_SETTINGS_TEST"));
    if ([t isEqualToString:@"cancel"]) [self cancel:nil];
    else if ([t isEqualToString:@"save-then-play"]) { [self save:nil]; [self play:nil]; }
    else if ([t isEqualToString:@"reset"]) { [self reset:nil]; [self play:nil]; }
    else if ([t hasPrefix:@"set:"]) {
        NSString *safe = nil; /* SPORE_FULLSCREEN_SAFE_AREA: chooses the below-the-notch item, after the other rows */
        for (NSString *kv in [[t substringFromIndex:4] componentsSeparatedByString:@","]) {
            NSRange eq = [kv rangeOfString:@"="];
            NSString *k = eq.location == NSNotFound ? kv : [kv substringToIndex:eq.location];
            NSString *v = eq.location == NSNotFound ? @"1" : [kv substringFromIndex:eq.location + 1];
            int i = [self rowForKey:k];
            if (i < 0) { fprintf(stderr, "spore-launcher: settings test: no row for '%s'\n", k.UTF8String); [NSApp stopModalWithCode:2]; return; }
            if ([k isEqualToString:@"SPORE_SKIP_INTRO"]) v = [v isEqualToString:@"0"] ? @"" : STATE_SKIP;
            else if ([k isEqualToString:@"STATE"] && [v caseInsensitiveCompare:@"skip"] == NSOrderedSame) v = STATE_SKIP;
            if ([k isEqualToString:@"SPORE_FULLSCREEN_SAFE_AREA"]) safe = v;
            else if (g_rows[i].kind == R_GAMEDIR) {
                char out[CAP];
                if (!normalise_pick(v.fileSystemRepresentation, out, sizeof out)) { _gameDir = S(out); _gameChanged = YES; [self showPaths]; }
            } else [self setRow:i value:v];
        }
        [self refillResolutions:nil];
        if (safe && ![safe isEqualToString:@"0"]) {
            NSString *n = res_notch_item(_resPixels);
            if (!n) { fprintf(stderr, "spore-launcher: settings test: this display has no notch\n"); [NSApp stopModalWithCode:2]; return; }
            [self resBox].stringValue = n;
        } else if (safe && [self resBelowNotch]) [self resBox].stringValue = res_value(res_notch_item(_resPixels));
        [self copyCommand:nil];
        [self play:nil];
    } else [self play:nil]; /* "play" */
}

/* ---- building the window ---- */

static NSTextField *label(NSString *s, CGFloat size, BOOL secondary) {
    NSTextField *l = [NSTextField labelWithString:s];
    l.font = [NSFont systemFontOfSize:size];
    if (secondary) l.textColor = NSColor.secondaryLabelColor;
    return l;
}

- (NSView *)controlForRow:(int)i {
    const srow *r = &g_rows[i];
    NSView *v = nil;
    [_pathLabel addObject:[NSNull null]];
    switch (r->kind) {
    case R_CHECK: case R_ARGFLAG: {
        /* the row label carries the meaning, so the box has no title; Retina rebuilds the Resolution list */
        BOOL retina = !strcmp(r->sw, "SPORE_RETINA");
        NSButton *b = [NSButton checkboxWithTitle:@"" target:retina ? self : nil
                                           action:retina ? @selector(refillResolutions:) : nil];
        b.font = [NSFont systemFontOfSize:11];
        v = b;
        break;
    }
    case R_POPUP: case R_FPS: case R_ARGPOP: case R_ARGLOCALE: case R_ARGSTATE: {
        NSPopUpButton *p = [[NSPopUpButton alloc] initWithFrame:NSZeroRect pullsDown:NO];
        p.autoenablesItems = NO;
        if (r->kind == R_ARGLOCALE) {
            [p addItemWithTitle:@"System language"];
            p.lastItem.representedObject = @"";
            [self fillLocales:p];
        } else if (r->kind == R_ARGSTATE) {
            [p addItemWithTitle:S(k_state_default_title)];
            p.lastItem.representedObject = @"";
            [p addItemWithTitle:S(k_state_skip_title)];
            p.lastItem.representedObject = STATE_SKIP;
            [p.menu addItem:[NSMenuItem separatorItem]];
            for (size_t k = 0; k < N_STATES; k++) {
                if (!k_states[k].name) { [p.menu addItem:[NSMenuItem separatorItem]]; continue; }
                [p addItemWithTitle:S(k_states[k].label)];
                p.lastItem.representedObject = S(k_states[k].name);
            }
            p.target = self;
            p.action = @selector(stateChanged:);
        } else if (r->kind == R_FPS) {
            for (size_t k = 0; k < N_PACE; k++) { [p addItemWithTitle:S(k_pace[k].title)]; p.lastItem.representedObject = S(k_pace[k].key); }
        } else {
            for (NSString *c in [S(r->choices) componentsSeparatedByString:@"|"]) {
                NSRange eq = [c rangeOfString:@"="];
                [p addItemWithTitle:[c substringFromIndex:eq.location + 1]];
                p.lastItem.representedObject = [c substringToIndex:eq.location];
            }
        }
        [p.widthAnchor constraintEqualToConstant:270].active = YES; /* every popup, the box and the field line up */
        v = p;
        break;
    }
    case R_ARGRES: { /* a combo box: the display's sizes, or any WIDTHxHEIGHT typed */
        NSComboBox *b = [[NSComboBox alloc] initWithFrame:NSZeroRect];
        b.placeholderString = @"As set in the game";
        b.usesDataSource = NO;
        b.completes = NO;
        b.numberOfVisibleItems = 12;
        [b.widthAnchor constraintEqualToConstant:270].active = YES;
        v = b;
        break;
    }
    case R_TEXT: case R_ARGTEXT: {
        NSTextField *f = [NSTextField textFieldWithString:@""];
        f.placeholderString = r->kind == R_TEXT ? @"Off" : @"";
        [f.widthAnchor constraintEqualToConstant:r->kind == R_ARGTEXT ? 270 : 80].active = YES;
        v = f;
        break;
    }
    case R_GAMEDIR: case R_DOCS: {
        NSTextField *l = label(@"", 12, YES);
        l.lineBreakMode = NSLineBreakByTruncatingMiddle;
        l.selectable = YES;
        [l.widthAnchor constraintEqualToConstant:190].active = YES;
        [l setContentCompressionResistancePriority:NSLayoutPriorityDefaultLow forOrientation:NSLayoutConstraintOrientationHorizontal];
        NSButton *b = [NSButton buttonWithTitle:@"Change…" target:self
                                         action:r->kind == R_GAMEDIR ? @selector(changeGameDir:) : @selector(changeDocsDir:)];
        _pathLabel[i] = l;
        NSStackView *s = [NSStackView stackViewWithViews:@[ l, b ]];
        s.spacing = 8;
        [_ctl addObject:b];
        return s;
    }
    }
    [_ctl addObject:v];
    return v;
}

- (void)buildWithGame:(const char *)game contents:(const char *)contents version:(const char *)version {
    /* 640 pt wide (210 pt labels, 270 pt controls; descriptions are tooltips); the height fits the tallest
     * tab, computed below */
    const CGFloat W = 640;
    _win = [[NSWindow alloc] initWithContentRect:NSMakeRect(0, 0, W, 600)
                                       styleMask:NSWindowStyleMaskTitled | NSWindowStyleMaskClosable
                                         backing:NSBackingStoreBuffered defer:NO];
    { /* "Spore - Launcher v0.95": the version without the build's git hash (that stays in --version) */
        NSString *ver = S(version);
        NSRange sp = [ver rangeOfString:@" "];
        if (sp.location != NSNotFound) ver = [ver substringToIndex:sp.location];
        _win.title = [@"Spore - Launcher v" stringByAppendingString:ver];
    }
    _win.delegate = self;
    _win.releasedWhenClosed = NO;
    NSView *cv = _win.contentView;

    /* tabs of label-and-control rows */
    NSTabView *tabs = [[NSTabView alloc] initWithFrame:NSMakeRect(14, 0, W - 28, 400)];
    NSArray *names = @[ @"General", @"Game options", @"Diagnostics" ];
    NSMutableDictionary *grids = [NSMutableDictionary dictionary];
    for (NSString *n in names) {
        NSTabViewItem *it = [[NSTabViewItem alloc] initWithIdentifier:n];
        it.label = n;
        NSGridView *g = [NSGridView gridViewWithNumberOfColumns:2 rows:0];
        g.translatesAutoresizingMaskIntoConstraints = NO;
        g.rowSpacing = 8;
        g.columnSpacing = 12;
        [g columnAtIndex:0].xPlacement = NSGridCellPlacementLeading;
        [g columnAtIndex:0].width = 210;
        g.yPlacement = NSGridCellPlacementCenter;
        NSView *iv = [[NSView alloc] initWithFrame:NSZeroRect];
        [iv addSubview:g];
        [NSLayoutConstraint activateConstraints:@[
            [g.topAnchor constraintEqualToAnchor:iv.topAnchor constant:12],
            [g.leadingAnchor constraintEqualToAnchor:iv.leadingAnchor constant:16],
            [g.trailingAnchor constraintLessThanOrEqualToAnchor:iv.trailingAnchor constant:-12],
        ]];
        it.view = iv;
        [tabs addTabViewItem:it];
        grids[n] = g;
    }
    /* a Mods tab would be one more NSTabViewItem here, with its own view instead of a grid (bundle.md §10) */

    NSButton *show = nil;
    for (int i = 0; i < g_nrows; i++) {
        const srow *r = &g_rows[i];
        if (r->kind == R_SHOW) {
            show = [NSButton checkboxWithTitle:S(r->label) target:nil action:nil];
            show.toolTip = S(r->note);
            [_pathLabel addObject:[NSNull null]];
            [_ctl addObject:show];
            continue;
        }
        NSView *c = [self controlForRow:i];
        BOOL env = [self presetByEnv:r];
        NSTextField *l = label(S(r->label), 13, NO);
        l.lineBreakMode = NSLineBreakByWordWrapping;
        l.preferredMaxLayoutWidth = 210;
        NSGridView *g = grids[S(r->tab)];
        for (size_t k = 0; k < sizeof k_heads / sizeof k_heads[0]; k++) {
            if (strcmp(k_heads[k].sw, r->sw)) continue;
            /* a group heading: 15 pt semibold, larger than the 13 pt row labels, across both
             * columns, with a thin separator line under it spanning the grid and space above when not first */
            NSTextField *h = [NSTextField labelWithString:S(k_heads[k].heading)];
            h.font = [NSFont systemFontOfSize:15 weight:NSFontWeightSemibold];
            [g addRowWithViews:@[ h, [NSGridCell emptyContentView] ]];
            NSGridRow *hr = [g rowAtIndex:g.numberOfRows - 1];
            [hr mergeCellsInRange:NSMakeRange(0, 2)];
            hr.topPadding = g.numberOfRows > 1 ? 16 : 0;
            NSBox *sep = [[NSBox alloc] initWithFrame:NSZeroRect];
            sep.boxType = NSBoxSeparator;
            [g addRowWithViews:@[ sep, [NSGridCell emptyContentView] ]];
            NSGridRow *sr = [g rowAtIndex:g.numberOfRows - 1];
            [sr mergeCellsInRange:NSMakeRange(0, 2)];
            [g cellForView:sep].xPlacement = NSGridCellPlacementFill;
            sr.topPadding = -2;
            sr.bottomPadding = 4;
        }
        [g addRowWithViews:@[ l, c ]];
        /* no visible notes: the row's sentence is the tooltip of its label and its control */
        NSString *note = r->note ? S(r->note) : nil;
        if (env) note = @"Set by the Terminal command that started the app.";
        if (env) {
            for (NSView *x in [c isKindOfClass:[NSStackView class]] ? ((NSStackView *)c).views : @[ c ])
                if ([x isKindOfClass:[NSControl class]] && ![x isKindOfClass:[NSTextField class]]) ((NSControl *)x).enabled = NO;
            if ([c isKindOfClass:[NSTextField class]]) ((NSTextField *)c).enabled = NO;
            l.textColor = NSColor.disabledControlTextColor;
        }
        /* the folder rows: on the label and the Change… button (the path label shows the full path) */
        NSArray *tipViews = [c isKindOfClass:[NSStackView class]] ? @[ l, ((NSStackView *)c).views.lastObject ] : @[ l, c ];
        for (NSView *x in tipViews) x.toolTip = note;
        if (r->kind == R_ARGSTATE && !env) _stateTip = tipViews;
    }

    /* the height: the tallest grid, the tab view's own chrome, the banner and the bottom rows */
    CGFloat gh = 0;
    for (NSString *n in names) gh = MAX(gh, ((NSGridView *)grids[n]).fittingSize.height);
    CGFloat chrome = tabs.frame.size.height - tabs.contentRect.size.height;
    const CGFloat tabsY = 82, tabsH = ceil(gh) + 12 + 16 + chrome, bannerH = 12 + 96 + 16;
    const CGFloat H = tabsY + tabsH + bannerH;
    [_win setContentSize:NSMakeSize(W, H)];
    tabs.frame = NSMakeRect(14, tabsY, W - 28, tabsH);
    [cv addSubview:tabs];

    /* banner: the app icon, "Spore", one line of explanation */
    const CGFloat iy = tabsY + tabsH + 12;
    NSImageView *icon = [NSImageView imageViewWithImage:
        [[NSImage alloc] initWithContentsOfFile:[NSString stringWithFormat:@"%s/Resources/AppIcon.icns", contents]] ?: NSApp.applicationIconImage];
    icon.frame = NSMakeRect(20, iy, 96, 96);
    icon.imageScaling = NSImageScaleProportionallyUpOrDown;
    [cv addSubview:icon];
    NSTextField *title = [NSTextField labelWithString:@"Spore"];
    title.font = [NSFont boldSystemFontOfSize:26];
    title.frame = NSMakeRect(132, iy + 46, W - 152, 34);
    [cv addSubview:title];
    NSTextField *sub = label(@"Choose how Spore starts. Play keeps your choices for next time.", 13, YES);
    sub.frame = NSMakeRect(132, iy + 22, W - 152, 18);
    [cv addSubview:sub];

    show.frame = NSMakeRect(20, 54, W - 40, 20);
    { /* SPORE_LAUNCHER_SETTINGS_TAB=N opens on tab N (screenshots of each tab) */
        const char *t = getenv("SPORE_LAUNCHER_SETTINGS_TAB");
        if (t && atoi(t) > 0 && atoi(t) < tabs.numberOfTabViewItems) [tabs selectTabViewItemAtIndex:atoi(t)];
    }
    [cv addSubview:show];

    /* bottom row: Reset, Copy on the left; Save, Cancel, Play on the right */
    NSButton *reset = [NSButton buttonWithTitle:@"Reset to defaults" target:self action:@selector(reset:)];
    NSButton *copy = [NSButton buttonWithTitle:@"Copy launch command" target:self action:@selector(copyCommand:)];
    NSButton *save = [NSButton buttonWithTitle:@"Save" target:self action:@selector(save:)];
    NSButton *cancel = [NSButton buttonWithTitle:@"Cancel" target:self action:@selector(cancel:)];
    NSButton *play = [NSButton buttonWithTitle:@"Play" target:self action:@selector(play:)];
    cancel.keyEquivalent = @"\033";
    play.keyEquivalent = @"\r";
    for (NSButton *b in @[ save, cancel, play ]) [b.widthAnchor constraintGreaterThanOrEqualToConstant:76].active = YES;
    NSStackView *bar = [[NSStackView alloc] initWithFrame:NSMakeRect(14, 14, W - 28, 32)];
    bar.spacing = 8;
    [bar addView:reset inGravity:NSStackViewGravityLeading];
    [bar addView:copy inGravity:NSStackViewGravityLeading];
    [bar addView:save inGravity:NSStackViewGravityTrailing];
    [bar addView:cancel inGravity:NSStackViewGravityTrailing];
    [bar addView:play inGravity:NSStackViewGravityTrailing];
    [cv addSubview:bar];
    _win.defaultButtonCell = play.cell;
}

@end

/* A minimal menu bar, so Cmd-Q, Cmd-W and the text fields' Cmd-C/V/X/A work while the window shows. */
static void settings_menu(void) {
    NSMenu *bar = [NSMenu new], *app = [NSMenu new], *edit = [[NSMenu alloc] initWithTitle:@"Edit"];
    NSMenuItem *ai = [NSMenuItem new], *ei = [NSMenuItem new];
    [app addItemWithTitle:@"Close" action:@selector(performClose:) keyEquivalent:@"w"]; /* = Cancel */
    [app addItemWithTitle:@"Quit Spore" action:@selector(terminate:) keyEquivalent:@"q"];
    [edit addItemWithTitle:@"Cut" action:@selector(cut:) keyEquivalent:@"x"];
    [edit addItemWithTitle:@"Copy" action:@selector(copy:) keyEquivalent:@"c"];
    [edit addItemWithTitle:@"Paste" action:@selector(paste:) keyEquivalent:@"v"];
    [edit addItemWithTitle:@"Select All" action:@selector(selectAll:) keyEquivalent:@"a"];
    ai.submenu = app;
    ei.submenu = edit;
    [bar addItem:ai];
    [bar addItem:ei];
    NSApp.mainMenu = bar;
}

/* Shows the window modally. Returns 1 for Play (settings written, game folder possibly changed in game),
 * 0 for Cancel (nothing written), 2 for a test-hook error. env0 is the launcher's environment before any setting
 * was applied (a variable set there wins and its row is disabled). */
static int settings_window(char *game, size_t cap, const char *contents, const char *launcher, NSDictionary *env0) {
    build_rows();
    ui_init();
    settings_menu();
    SNSettings *s = [SNSettings new];
    s->_ctl = [NSMutableArray array];
    s->_pathLabel = [NSMutableArray array];
    s->_env0 = env0;
    s->_gameDir = S(game);
    s->_launcher = S(launcher);
    [s buildWithGame:game contents:contents version:SPORE_BUNDLE_VERSION];
    [s loadStored];
    [s showPaths];
    [s->_win center];
    [s->_win makeKeyAndOrderFront:nil];
    [s->_win makeFirstResponder:nil]; /* no text field takes focus on open: Return is Play */
    if (getenv("SPORE_LAUNCHER_SETTINGS_TEST"))
        [s performSelector:@selector(testHook) withObject:nil afterDelay:1.5
                   inModes:@[ NSModalPanelRunLoopMode, NSDefaultRunLoopMode ]];
    NSModalResponse r = [NSApp runModalForWindow:s->_win];
    [s->_win orderOut:nil];
    if (r == 1) snprintf(game, cap, "%s", s->_gameDir.fileSystemRepresentation);
    return (int)r;
}

/* After the window: forget what the stored settings put into the environment before it showed, then apply the
 * domain again through the same path as a start without the window. */
static void reapply_settings(NSDictionary *env0, NSDictionary *before) {
    NSMutableSet *names = [NSMutableSet setWithArray:before.allKeys];
    for (int i = 0; i < g_nrows; i++)
        for (NSString *n in [S(g_rows[i].sw) componentsSeparatedByString:@"+"])
            if ([n hasPrefix:@"SPORE_"]) [names addObject:n];
    for (NSString *n in names)
        if ([n isKindOfClass:[NSString class]] && !env0[n]) unsetenv(n.UTF8String);
    apply_pref_environment();
}

static NSArray<NSString *> *pref_arguments(void) {
    CFPropertyListRef v = CFPreferencesCopyAppValue(CFSTR("Arguments"), PREF_DOMAIN);
    NSMutableArray *out = [NSMutableArray array];
    if (v && CFGetTypeID(v) == CFArrayGetTypeID())
        for (id a in (__bridge NSArray *)v) { NSString *s = str_of(a); if (s.length) [out addObject:s]; }
    if (v) CFRelease(v);
    return out;
}

/* ---- command line ------------------------------------------------------------------------------------------ */

static void usage(FILE *o) {
    fprintf(o,
            "usage: \"Spore Native\" [options] [-- game arguments]\n"
            "\n"
            "Starts Spore Galactic Adventures (GOG) through Spore Native %s.\n"
            "\n"
            "options:\n"
            "  --help             show this text and exit\n"
            "  --version          print the version and exit\n"
            "  --game-dir PATH    use the Spore folder PATH (the one holding SporebinEP1, Data and DataEP1) and\n"
            "                     remember it for later starts\n"
            "  --reset-game-dir   forget the remembered Spore folder (with no other option: then exit)\n"
            "  --print-env        print the environment and command the game would get, start nothing\n"
            "  --resolve-only     print the Spore folder that would be used (\"<dir><TAB><source>\"), start nothing\n"
            "  --no-panel         never show the folder dialog, the settings window or alerts\n"
            "  --settings         show the settings window even if it is turned off\n"
            "  --no-settings      skip the settings window for this start\n"
            "  --list-settings    print every row of the settings window (tab, label, switch, default, choices)\n"
            "                     and exit\n"
            "  --                 everything after this is passed to the game (after the stored Arguments)\n"
            "\n"
            "The settings window shows at every start unless its \"Show window every launch\" box is cleared\n"
            "(ShowSettings); holding Option at start shows it too. Play saves and starts, Cancel writes nothing and\n"
            "starts nothing. The log is opened after the window closes, so a Cancel leaves no log file.\n"
            "\n"
            "settings: defaults write com.spore-native.player GPUProfile 8800gtx|9800gtx|gtx280\n"
            "          defaults write com.spore-native.player DocumentsFolder PATH\n"
            "          defaults write com.spore-native.player Environment -dict NAME VALUE ...\n"
            "          defaults write com.spore-native.player Arguments -array ARG ...\n"
            "          defaults write com.spore-native.player ShowSettings -bool false\n"
            "logs:     ~/Library/Logs/Spore Native/ (SPORE_LOG_FILE=- keeps this terminal)\n",
            SPORE_BUNDLE_VERSION);
}

static int bad_usage(const char *fmt, const char *arg) {
    fprintf(stderr, "spore-launcher: ");
    fprintf(stderr, fmt, arg);
    fprintf(stderr, "\n\n");
    usage(stderr);
    return 2;
}

/* ---- main ------------------------------------------------------------------------------------------------ */

/* The documents folder is usable if "My Spore Creations" can be made (or exists) inside it and listed. Inside
 * ~/Documents this is the first access macOS's privacy control sees, so the prompt (if any) comes from the
 * launcher; a refusal falls back to the development sandbox instead of failing inside the game. */
static int writable_docs(const char *d) {
    char t[CAP];
    mkdir_p(d);
    snprintf(t, sizeof t, "%s/My Spore Creations", d);
    if (mkdir(t, 0755) && errno != EEXIST) return 0;
    DIR *x = opendir(t);
    if (!x) return 0;
    closedir(x);
    return access(t, W_OK) == 0;
}

int main(int argc, char **argv) {
    @autoreleasepool {
        /* the launcher's own environment, before any setting is applied (the settings window disables a row
         * whose variable is set here: the environment wins over a stored setting) */
        NSDictionary *env0 = [[NSProcessInfo processInfo].environment copy];
        int resolve_only = 0, print_env = 0, reset_dir = 0, i, gargc = 0;
        int want_settings = 0, no_settings = 0, list = 0;
        const char *opt_dir = NULL;
        char **gargv = calloc((size_t)argc + 1, sizeof *gargv);
        /* Only the options below are ours; anything else is refused (exit 2) rather than handed to the game, and
         * only what follows "--" reaches the game. Parsing happens before the log is opened, so a mistake is
         * reported on the terminal and nothing is started. */
        for (i = 1; i < argc; i++) {
            const char *a = argv[i];
            if (!strcmp(a, "--help") || !strcmp(a, "-h")) { usage(stdout); return 0; }
            else if (!strcmp(a, "--version")) { printf("Spore Native %s\n", SPORE_BUNDLE_VERSION); return 0; }
            else if (!strcmp(a, "--resolve-only")) resolve_only = 1;
            else if (!strcmp(a, "--print-env")) print_env = 1;
            else if (!strcmp(a, "--no-panel")) g_no_panel = 1;
            else if (!strcmp(a, "--settings")) want_settings = 1;
            else if (!strcmp(a, "--no-settings")) no_settings = 1;
            else if (!strcmp(a, "--list-settings")) list = 1;
            else if (!strcmp(a, "--reset-game-dir")) reset_dir = 1;
            else if (!strcmp(a, "--game-dir")) {
                if (i + 1 >= argc || !argv[i + 1][0]) return bad_usage("%s needs a folder", a);
                opt_dir = argv[++i];
            } else if (!strncmp(a, "--game-dir=", 11)) {
                if (!a[11]) return bad_usage("%s needs a folder", "--game-dir");
                opt_dir = a + 11;
            }
            else if (!strncmp(a, "-psn_", 5)) continue; /* old LaunchServices process serial number */
            else if (!strcmp(a, "-NSDocumentRevisionsDebugMode") && i + 1 < argc) i++; /* added by Xcode */
            else if (!strcmp(a, "--")) { while (++i < argc) gargv[gargc++] = argv[i]; }
            else if (a[0] == '-') return bad_usage("unknown option '%s' (game arguments go after \"--\")", a);
            else return bad_usage("unexpected argument '%s' (game arguments go after \"--\")", a);
        }
        if (getenv("SPORE_LAUNCHER_NO_PANEL")) g_no_panel = 1;
        const char *h = getenv("HOME");
        snprintf(g_home, sizeof g_home, "%s", h && *h ? h : NSHomeDirectory().fileSystemRepresentation);

        if (reset_dir) {
            char sup[CAP], f[CAP];
            support_dir(sup, sizeof sup);
            snprintf(f, sizeof f, "%s/game-dir", sup);
            if (!unlink(f)) fprintf(stderr, "spore-launcher: forgot the remembered Spore folder (%s)\n", f);
            else if (errno == ENOENT) fprintf(stderr, "spore-launcher: no Spore folder was remembered\n");
            else { fprintf(stderr, "spore-launcher: cannot remove %s: %s\n", f, strerror(errno)); return 1; }
            if (!opt_dir && !resolve_only && !print_env) return 0;
        }
        char picked[CAP];
        if (opt_dir) {
            const char *why = normalise_pick(opt_dir, picked, sizeof picked);
            if (why) { fprintf(stderr, "spore-launcher: --game-dir %s: %s\n", opt_dir, why); return 1; }
            remember(picked);
            g_opt_dir = picked;
        }

        if (list) { /* the language row lists the resolved install's Data/Locale folders; no UI, nothing remembered */
            char game[CAP], reason[CAP];
            g_no_panel = 1;
            list_settings(resolve_game_dir(game, sizeof game, reason, sizeof reason) ? game : NULL);
            return 0;
        }

        char self[CAP], real[CAP], macos[CAP];
        uint32_t sz = sizeof self;
        if (_NSGetExecutablePath(self, &sz) || !realpath(self, real)) snprintf(real, sizeof real, "%s", argv[0]);
        snprintf(macos, sizeof macos, "%s", real);
        { char *s = strrchr(macos, '/'); if (s) *s = 0; }
        char contents[CAP];
        snprintf(contents, sizeof contents, "%s/..", macos);
        { char t[CAP]; if (realpath(contents, t)) snprintf(contents, sizeof contents, "%s", t); }

        apply_pref_environment();

        char game[CAP], reason[CAP];
        if (resolve_only) {
            const char *src = resolve_game_dir(game, sizeof game, reason, sizeof reason);
            if (!src) { printf("-\t%s\n", reason); return 1; }
            if (!strcmp(src, "picked") || !strcmp(src, "found")) remember(game);
            printf("%s\t%s\n", game, src);
            return 0;
        }

        /* The settings window: at every start unless ShowSettings is false; --settings or Option held shows
         * it anyway, --no-settings, --no-panel and SPORE_LAUNCHER_NO_PANEL never. When it shows, the log is opened
         * after it closes (a Cancel leaves no log), so the folder search before it reports on the terminal. */
        int show = 0;
        if (!print_env && !g_no_panel && !no_settings)
            show = want_settings || pref_show_settings() || ([NSEvent modifierFlags] & NSEventModifierFlagOption) != 0;
        char logpath[CAP];
        logpath[0] = 0;
        if (!print_env && !show) open_log(logpath, sizeof logpath);
        if (!show) {
            time_t t = time(NULL);
            fprintf(stderr, "spore-launcher: Spore Native %s, %s", SPORE_BUNDLE_VERSION, ctime(&t));
        }
        const char *src = resolve_game_dir(game, sizeof game, reason, sizeof reason);
        if (!src) {
            if (!g_no_panel && !strcmp(reason, "no Spore install found")) return 0; /* the player cancelled */
            alert("Spore cannot start", reason[0] ? reason : "no Spore install found");
            return 1;
        }
        if (!strcmp(src, "picked") || !strcmp(src, "found")) remember(game);
        if (show) {
            NSDictionary *before = [NSDictionary dictionary];
            CFPropertyListRef bv = CFPreferencesCopyAppValue(CFSTR("Environment"), PREF_DOMAIN);
            if (bv && CFGetTypeID(bv) == CFDictionaryGetTypeID()) before = (__bridge NSDictionary *)bv;
            char prev[CAP];
            snprintf(prev, sizeof prev, "%s", game);
            int r = settings_window(game, sizeof game, contents, real, env0);
            if (r != 1) {
                fprintf(stderr, "spore-launcher: settings window %s; nothing started\n", r ? "test error" : "cancelled");
                if (bv) CFRelease(bv);
                return r ? 2 : 0;
            }
            reapply_settings(env0, before);
            if (bv) CFRelease(bv);
            if (strcmp(prev, game)) src = "settings window";
            open_log(logpath, sizeof logpath);
            time_t t = time(NULL);
            fprintf(stderr, "spore-launcher: Spore Native %s, %s", SPORE_BUNDLE_VERSION, ctime(&t));
            fprintf(stderr, "spore-launcher: settings window: Play\n");
        }
        fprintf(stderr, "spore-launcher: game folder %s (%s)\n", game, src);

        /* the environment the runtime reads (docs/design/bundle.md section 3) */
        char p[CAP];
        setenv("SPORE_GAME_DIR", game, 1);
        snprintf(p, sizeof p, "%s/SporebinEP1/SporeApp.exe", game);
        setenv("SPORE_EXE", p, 1);
        if (!getenv("SPORE_DOCUMENTS_DIR")) {
            char d[CAP];
            if (pref_string(CFSTR("DocumentsFolder"), d, sizeof d)) {
                if (d[0] == '~') { char t[CAP]; snprintf(t, sizeof t, "%s%s", g_home, d + 1); snprintf(d, sizeof d, "%s", t); }
            } else snprintf(d, sizeof d, "%s/Documents", g_home);
            if (writable_docs(d)) {
                char old[CAP], sup2[CAP];
                setenv("SPORE_DOCUMENTS_DIR", d, 1);
                support_dir(sup2, sizeof sup2);
                snprintf(old, sizeof old, "%s/Documents/My Spore Creations", sup2);
                if (is_dir(old))
                    fprintf(stderr, "spore-launcher: note: development runs keep \"My Spore Creations\" in %s; the "
                                    "bundle uses %s/My Spore Creations (copy files across to keep them together, or "
                                    "defaults write com.spore-native.player DocumentsFolder \"%s/Documents\")\n",
                            old, d, sup2);
            }
            else fprintf(stderr, "spore-launcher: %s is not writable (privacy setting?); \"My Spore Creations\" stays "
                                 "in the Application Support folder\n", d);
        }
        char caches[CAP];
        snprintf(caches, sizeof caches, "%s/Library/Caches/com.spore-native.player", g_home);
        snprintf(p, sizeof p, "%s/shadercache", caches);
        set_default_env("SPORE_D3D9MSL_CACHE", p);
        set_default_env("SPORE_REPO_ROOT", caches); /* writable state a back end keeps under build/ goes here */
        {
            char ld[CAP];
            const char *e = getenv("SPORE_LOG_DIR");
            if (e && *e) snprintf(ld, sizeof ld, "%s", e);
            else snprintf(ld, sizeof ld, "%s/Library/Logs/Spore Native", g_home);
            snprintf(p, sizeof p, "%s/dispatch-misses.txt", ld);
            set_default_env("SPORE_MISS_LOG", p);
        }
        gpu_profile(game);

        char sup[CAP];
        support_dir(sup, sizeof sup);
        mkdir_p(sup);
        if (chdir(sup)) (void)chdir("/");

        const char *target = getenv("SPORE_LAUNCHER_TARGET");
        char tbuf[CAP];
        if (!target || !*target) { snprintf(tbuf, sizeof tbuf, "%s/spore-game", macos); target = tbuf; }
        NSArray<NSString *> *stored = pref_arguments(); /* the settings window's Arguments, then the command line's */
        char **av = calloc((size_t)gargc + stored.count + 4, sizeof *av);
        int n = 0;
        av[n++] = (char *)target;
        av[n++] = "--run";
        if (gargc || stored.count) {
            av[n++] = "--";
            for (NSString *a in stored) av[n++] = strdup(a.UTF8String);
            for (i = 0; i < gargc; i++) av[n++] = gargv[i];
        }
        av[n] = NULL;
        /* The game reads its command line only from SPORE_ARGS (runtime/win32 w32_module.c builds
         * "<exe path>" SPORE_ARGS; spore-game's own "--" arguments are not used): the stored Arguments and the
         * command line's, joined. A SPORE_ARGS already in the environment wins. */
        if (gargc || stored.count) {
            NSMutableArray *all = [NSMutableArray arrayWithArray:stored];
            for (i = 0; i < gargc; i++) [all addObject:@(gargv[i])];
            NSString *line = game_cmdline(all);
            if (line.length) set_default_env("SPORE_ARGS", line.UTF8String);
        }

        static const char *shown[] = {"SPORE_GAME_DIR", "SPORE_EXE", "SPORE_DOCUMENTS_DIR", "SPORE_SUPPORT_DIR",
                                      "SPORE_D3D9", "SPORE_GPU_PROFILE", "SPORE_D3D9MSL_CACHE", "SPORE_REPO_ROOT",
                                      "SPORE_MISS_LOG",
                                      /* the settings window's switches */
                                      "SPORE_FULLSCREEN", "SPORE_FULLSCREEN_SAFE_AREA", "SPORE_FULLSCREEN_ESC", "SPORE_RETINA", "SPORE_VSYNC",
                                      "SPORE_D3D9_FPS", "SPORE_ONLINE", "SPORE_TEXT_INPUT", "SPORE_PRESS_AND_HOLD",
                                      "SPORE_GAMEPAD", "SPORE_NET_LOG", "SPORE_STALL_SECONDS", "SPORE_METAL_WATCH",
                                      "SPORE_METAL_PSO_ARCHIVE", "SPORE_SKIP_INTRO", "SPORE_ARGS", NULL};
        FILE *o = print_env ? stdout : stderr;
        for (i = 0; shown[i]; i++)
            if (getenv(shown[i])) fprintf(o, "%s%s=%s\n", print_env ? "" : "spore-launcher: ", shown[i], getenv(shown[i]));
        fprintf(o, "%sexec", print_env ? "EXEC=" : "spore-launcher: ");
        for (i = 0; av[i]; i++) fprintf(o, " %s", av[i]);
        fprintf(o, "\n");
        if (print_env) return 0;
        fflush(stdout);
        fflush(stderr);
        execv(target, av);
        char msg[CAP + 128];
        snprintf(msg, sizeof msg, "%s: %s", target, strerror(errno));
        alert("Spore cannot start the game", msg);
        return 1;
    }
}
