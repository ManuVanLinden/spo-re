#!/usr/bin/env python3
"""Tests the app's launcher: Spore folder resolution, environment, logging and exec on fake folder
layouts under build/bundle/test/. No game process starts: the exec target is a shell script that reports what
it was given. The fake installs link to the real SporeApp.exe and Data folder (symlinks, nothing copied).

Section 17 drives the settings window through SPORE_LAUNCHER_SETTINGS_TEST in the same throwaway bundle; every
other launch passes --no-settings (and --no-panel), so no window shows there.

The window has no Renderer row: Metal is the one renderer; the suite checks that the row is absent.

Settings: every launch reads the throwaway defaults domain
com.spore-native.player.test through SPORE_DEFAULTS_DOMAIN, and the open-panel check runs a copy of the launcher
inside a throwaway bundle with that identifier, so the panel's own NSNav* keys land there too. The player's real
com.spore-native.player domain is exported before and after and must not change; the throwaway domain is deleted.

Usage:   python3 -P tools/bundle/test_launcher.py [--launcher PATH] [--game GAME_DIR]
         (--launcher: the launcher to test, for example an app's Contents/MacOS/Spore Native; --game: a real
         Spore folder whose exe and Data the fake installs link to)
When:    after changing launcher.m or make_bundle.py.
"""
import argparse
import os
import plistlib
import re
import shutil
import subprocess
import sys
import time

ROOT = os.path.realpath(os.path.join(os.path.dirname(os.path.abspath(__file__)), "..", ".."))
ap = argparse.ArgumentParser(description=__doc__, formatter_class=argparse.RawDescriptionHelpFormatter)
ap.add_argument("--launcher", default=os.path.join(ROOT, "build", "Spore.app", "Contents", "MacOS", "Spore Native"),
                help="the launcher to test (default: build/Spore.app's)")
ap.add_argument("--game", default=os.path.join(ROOT, "game"), help="a real Spore folder (default game/)")
a = ap.parse_args()
GAME = os.path.realpath(a.game)
T = os.path.join(ROOT, "build", "bundle", "test")
shutil.rmtree(T, ignore_errors=True)
REAL_DOMAIN = "com.spore-native.player"
TEST_DOMAIN = "com.spore-native.player.test"
REAL_PLIST = os.path.expanduser("~/Library/Preferences/%s.plist" % REAL_DOMAIN)


def defaults(*args):
    """`defaults` as the real user (no HOME override), stdout as bytes; never touches REAL_DOMAIN for writing."""
    assert REAL_DOMAIN not in args or args[0] == "export"
    return subprocess.run(["defaults"] + list(args), capture_output=True, timeout=30).stdout


def real_domain_state():
    st = os.stat(REAL_PLIST) if os.path.exists(REAL_PLIST) else None
    return defaults("export", REAL_DOMAIN, "-"), (st.st_mtime_ns, st.st_size) if st else None


REAL_BEFORE = real_domain_state()
defaults("delete", TEST_DOMAIN)
HOME = os.path.join(T, "home")
SUP = os.path.join(T, "support")
os.makedirs(HOME)
fails = 0


def check(ok, what, detail=""):
    global fails
    print("  %s  %s%s" % ("ok  " if ok else "FAIL", what, "" if ok else "  [%s]" % detail.strip()))
    fails += not ok


def install(path, exe="real", ep1=True):
    os.makedirs(os.path.join(path, "SporebinEP1"), exist_ok=True)
    e = os.path.join(path, "SporebinEP1", "SporeApp.exe")
    if exe == "real":
        os.symlink(os.path.join(GAME, "SporebinEP1", "SporeApp.exe"), e)
    else:
        with open(e, "wb") as f:
            f.write(b"MZ" + b"\0" * 4096)  # right name, wrong bytes
    os.symlink(os.path.join(GAME, "Data"), os.path.join(path, "Data"))
    if ep1:
        os.makedirs(os.path.join(path, "DataEP1"), exist_ok=True)
    return path


def launch(*args, env=None, home=HOME):
    e = {k: v for k, v in os.environ.items() if not k.startswith("SPORE_")}
    e.update(HOME=home, SPORE_SUPPORT_DIR=SUP, SPORE_LAUNCHER_NO_PANEL="1", SPORE_LOG_DIR=os.path.join(T, "logs"),
             SPORE_DEFAULTS_DOMAIN=TEST_DOMAIN)
    e.update(env or {})
    r = subprocess.run([a.launcher, "--no-panel", "--no-settings"] + list(args), capture_output=True, text=True, env=e,
                       timeout=60)
    return r


def test_bundle():
    """A copy of the launcher inside a throwaway bundle whose identifier is TEST_DOMAIN: AppKit's own writes (the
    open panel's NSNav* keys go to the main bundle's domain) then stay out of the player's domain."""
    app = os.path.join(T, "LauncherTest.app", "Contents")
    os.makedirs(os.path.join(app, "MacOS"), exist_ok=True)
    exe = os.path.join(app, "MacOS", "Spore Native Test")
    shutil.copy2(a.launcher, exe)
    with open(os.path.join(app, "Info.plist"), "wb") as f:
        plistlib.dump({"CFBundleIdentifier": TEST_DOMAIN, "CFBundleExecutable": "Spore Native Test",
                       "CFBundleName": "Spore Native Test", "CFBundlePackageType": "APPL"}, f)
    return exe


def resolve(env=None):
    r = launch("--resolve-only", env=env)
    dirpart, _, src = r.stdout.strip().partition("\t")
    return r.returncode, dirpart, src, r.stderr


def remembered():
    try:
        return open(os.path.join(SUP, "game-dir")).read().strip()
    except OSError:
        return None


print("launcher:", a.launcher)
print("fake layouts under", T)

# 1. nothing anywhere
rc, d, src, err = resolve()
check(rc == 1 and d == "-" and src == "no Spore install found", "empty home: no install found, exit 1", d + src + err)

# 2. a folder with the right names but the wrong exe is refused
bad = install(os.path.join(HOME, "Applications", "Spore"), exe="junk")
rc, d, src, err = resolve()
check(rc == 1 and d == "-", "wrong SporeApp.exe bytes (hash check) refused", d + src + err)

# 3. no DataEP1 refused
noep1 = install(os.path.join(HOME, "GOG Games", "Spore"), ep1=False)
rc, d, src, err = resolve()
check(rc == 1, "install without DataEP1 refused", d + src + err)

# 4. the usual place ~/Downloads/spore-gog is found and remembered
dl = install(os.path.join(HOME, "Downloads", "spore-gog"))
rc, d, src, err = resolve()
check(rc == 0 and d == dl and src == "found", "~/Downloads/spore-gog found", d + src + err)
check(remembered() == dl, "found folder remembered in <support>/game-dir", str(remembered()))

# 5. the remembered folder wins over the usual places
other = install(os.path.join(T, "elsewhere", "My Spore"))
with open(os.path.join(SUP, "game-dir"), "w") as f:
    f.write(other + "\n")
rc, d, src, err = resolve()
check(rc == 0 and d == other and src == "remembered", "remembered folder wins over ~/Downloads", d + src + err)

# 6. SPORE_GAME_DIR wins over the remembered folder
third = install(os.path.join(T, "env-install"))
rc, d, src, err = resolve({"SPORE_GAME_DIR": third})
check(rc == 0 and d == third and src == "env", "SPORE_GAME_DIR wins", d + src + err)
check(remembered() == other, "an env override is not remembered", str(remembered()))

# 7. SPORE_GAME_DIR naming the SporebinEP1 folder, or a folder holding one install, is normalised
rc, d, src, err = resolve({"SPORE_GAME_DIR": os.path.join(third, "SporebinEP1")})
check(rc == 0 and d == third, "SporebinEP1 picked -> its parent", d + src + err)
rc, d, src, err = resolve({"SPORE_GAME_DIR": os.path.join(T, "elsewhere")})
check(rc == 0 and d == other, "folder with exactly one install inside -> that install", d + src + err)

# 8. an invalid SPORE_GAME_DIR falls through to the remembered folder, with a message
rc, d, src, err = resolve({"SPORE_GAME_DIR": bad})
check(rc == 0 and d == other and src == "remembered" and "SPORE_GAME_DIR" in err, "bad SPORE_GAME_DIR -> remembered", d + src + err)

# 9. a remembered folder that is gone falls through to the usual places and is replaced
shutil.rmtree(os.path.join(T, "elsewhere"))
rc, d, src, err = resolve()
check(rc == 0 and d == dl and src == "found" and remembered() == dl, "stale remembered folder -> usual place, re-remembered",
      d + src + err)

# 10. the environment the game gets
docs = os.path.join(HOME, "Documents")
r = launch("--print-env", env={"SPORE_GPU_PROFILE": "gtx280"})
env = dict(l.split("=", 1) for l in r.stdout.splitlines() if "=" in l)
caches = os.path.join(HOME, "Library", "Caches", "com.spore-native.player")
check(r.returncode == 0 and env.get("SPORE_GAME_DIR") == dl and env.get("SPORE_EXE") == os.path.join(dl, "SporebinEP1", "SporeApp.exe"),
      "SPORE_GAME_DIR and SPORE_EXE point into the install", r.stdout + r.stderr)
check(env.get("SPORE_DOCUMENTS_DIR") == docs and os.path.isdir(docs), "SPORE_DOCUMENTS_DIR = ~/Documents (My Spore Creations)", r.stdout)
check(env.get("SPORE_D3D9MSL_CACHE") == os.path.join(caches, "shadercache") and env.get("SPORE_REPO_ROOT") == caches,
      "shader cache and back-end state under ~/Library/Caches/com.spore-native.player", r.stdout)
check(env.get("SPORE_GPU_PROFILE") == "gtx280" and 'VideoCards.txt names NVIDIA 0x05e1 "GeForce GTX 280"' in r.stderr,
      "GPU profile gtx280 checked against the install's VideoCards.txt", r.stdout + r.stderr)
check("SPORE_D3D9=" not in r.stdout, "back end left to the runtime default (Metal)", r.stdout)
check(env.get("EXEC", "").endswith("spore-game --run"), "execs Contents/MacOS/spore-game --run", env.get("EXEC", ""))
r = launch("--print-env", env={"SPORE_GPU_PROFILE": "voodoo2"})
check("SPORE_GPU_PROFILE" not in r.stdout and "not one of" in r.stderr, "unknown GPU profile dropped with a message", r.stdout + r.stderr)
r = launch("--print-env", env={"SPORE_DOCUMENTS_DIR": "/tmp/x-docs", "SPORE_D3D9": "null"})
check("SPORE_DOCUMENTS_DIR=/tmp/x-docs" in r.stdout and "SPORE_D3D9=null" in r.stdout, "the environment wins over defaults", r.stdout)
r = launch("--print-env", env={"SPORE_GAME_DIR": bad, "SPORE_SUPPORT_DIR": os.path.join(T, "support-empty")},
           home=os.path.join(T, "home-empty"))
check(r.returncode == 1 and "Spore cannot start" in r.stderr, "no install, no panel: exit 1 with the reason", r.stderr)

# 11. exec and the log file: a stand-in target reports its arguments and environment on stderr
tgt = os.path.join(T, "fake-game.sh")
with open(tgt, "w") as f:
    f.write('#!/bin/sh\necho "fake-game args: $*" >&2\necho "fake-game env: GAME=$SPORE_GAME_DIR DOCS=$SPORE_DOCUMENTS_DIR" >&2\n'
            'echo "fake-game SPORE_ARGS: $SPORE_ARGS" >&2\necho "fake-game cwd: $(pwd)"\nexit 7\n')
os.chmod(tgt, 0o755)
logs = os.path.join(T, "logs")
os.makedirs(logs, exist_ok=True)
for i in range(35):  # old logs to prune
    open(os.path.join(logs, "2020-01-%02d 10-00-00.log" % (i % 28 + 1) if i < 28 else "2020-02-%02d 10-00-00.log" % i), "w").close()
r = launch("--", "-someGameArg", env={"SPORE_LAUNCHER_TARGET": tgt})
names = sorted(n for n in os.listdir(logs) if n.endswith(".log"))
newest = os.path.join(logs, names[-1]) if names else None
text = open(newest).read() if newest else ""
check(r.returncode == 7, "exec replaced the launcher (exit code is the target's)", "rc=%d %s" % (r.returncode, r.stderr))
check(r.stdout == "" and r.stderr == "", "nothing on the terminal when logging", r.stdout + r.stderr)
check("spore-launcher: Spore Native" in text and "fake-game args: --run -- -someGameArg" in text and
      ("GAME=%s DOCS=%s" % (dl, docs)) in text, "stdout and stderr of launcher and game land in the dated log", text)
check(("fake-game cwd: %s" % SUP) in text, "working directory is the support folder", text)
check("fake-game SPORE_ARGS: -someGameArg\n" in text,
      "arguments after '--' reach the game through SPORE_ARGS (the runtime reads only that)", text)
check(len(names) <= 30 and not names[0].startswith("2020-01-01"), "old logs pruned to the newest 30 (%d left)" % len(names), " ".join(names[:3]))
r = launch(env={"SPORE_LAUNCHER_TARGET": tgt, "SPORE_LOG_FILE": "-"})
check("fake-game args: --run" in r.stderr, "SPORE_LOG_FILE=- keeps the terminal", r.stderr)

# 12. the open panel (a GUI check: the panel appears on screen for ~1.5 s and is cancelled by the test hook)
e = {k: v for k, v in os.environ.items() if not k.startswith("SPORE_")}
e.update(HOME=os.path.join(T, "home-empty"), SPORE_SUPPORT_DIR=os.path.join(T, "support-panel"),
         SPORE_LOG_DIR=os.path.join(T, "logs-panel"), SPORE_LAUNCHER_PANEL_TEST="1", SPORE_DEFAULTS_DOMAIN=TEST_DOMAIN)
t0 = time.time()
r = subprocess.run([test_bundle(), "--resolve-only"], capture_output=True, text=True, env=e, timeout=60)
dt = time.time() - t0
check(r.returncode == 1 and r.stdout.startswith("-\tno Spore install found") and dt >= 1.4,
      "open panel shown when nothing is found, cancel -> no install (%.1f s)" % dt, r.stdout + r.stderr)
check(not os.path.exists(os.path.join(T, "support-panel", "game-dir")), "a cancelled panel remembers nothing", "")
panel_keys = defaults("read", TEST_DOMAIN).decode(errors="replace")
check("NSNav" in panel_keys or "NSOSP" in panel_keys, "the panel's own keys went to the throwaway domain %s" % TEST_DOMAIN,
      panel_keys)

# 13. the command line: only our options; everything else is refused before anything starts
marker = os.path.join(T, "started")
mtgt = os.path.join(T, "marker-game.sh")
with open(mtgt, "w") as f:
    f.write('#!/bin/sh\necho "$*" > "%s"\nexit 0\n' % marker)
os.chmod(mtgt, 0o755)


def cli(*args, env=None):
    if os.path.exists(marker):
        os.unlink(marker)
    before = set(os.listdir(logs))
    e = {"SPORE_LAUNCHER_TARGET": mtgt}
    e.update(env or {})
    r = launch(*args, env=e)
    started = open(marker).read().strip() if os.path.exists(marker) else None
    return r, started, set(os.listdir(logs)) - before


r, started, newlogs = cli("--help")
check(r.returncode == 0 and r.stdout.startswith("usage:") and "--game-dir PATH" in r.stdout and "--reset-game-dir" in r.stdout
      and started is None and not newlogs, "--help: usage on stdout, exit 0, nothing started, no log", r.stdout + r.stderr)
r, started, newlogs = cli("--version")
check(r.returncode == 0 and r.stdout.startswith("Spore Native ") and started is None, "--version: '%s', exit 0" % r.stdout.strip(),
      r.stdout + r.stderr)
r, started, newlogs = cli("--foo")
check(r.returncode == 2 and "unknown option '--foo'" in r.stderr and "usage:" in r.stderr and r.stdout == "" and
      started is None and not newlogs, "unknown option: usage on stderr, exit 2, nothing started, no log", r.stdout + r.stderr)
r, started, newlogs = cli("-someGameArg")
check(r.returncode == 2 and started is None, "a game argument before '--' is refused (exit 2)", r.stderr)
r, started, newlogs = cli("savegame")
check(r.returncode == 2 and "unexpected argument 'savegame'" in r.stderr and started is None,
      "a bare argument is refused (exit 2)", r.stderr)
r, started, newlogs = cli("--game-dir")
check(r.returncode == 2 and "needs a folder" in r.stderr and started is None, "--game-dir without a folder: exit 2", r.stderr)
r, started, newlogs = cli("--", "--foo", "bar")
check(r.returncode == 0 and started == "--run -- --foo bar", "'-- --foo bar' passed through to the game", str(started) + r.stderr)
r, started, newlogs = cli("-psn_0_12345")
check(r.returncode == 0 and started == "--run", "LaunchServices' -psn_ argument ignored", str(started) + r.stderr)

# 14. --game-dir PATH uses and remembers a folder; --reset-game-dir forgets it
pick = install(os.path.join(T, "picked", "Spore GA"))
r = launch("--game-dir", pick, "--resolve-only", env={"SPORE_GAME_DIR": third})
d, _, src = r.stdout.strip().partition("\t")
check(r.returncode == 0 and d == pick and src == "option" and remembered() == pick,
      "--game-dir PATH wins over SPORE_GAME_DIR and is remembered", r.stdout + r.stderr)
r = launch("--game-dir=" + os.path.join(pick, "SporebinEP1"), "--resolve-only")
check(r.returncode == 0 and r.stdout.startswith(pick + "\t"), "--game-dir=.../SporebinEP1 normalised to the install", r.stdout + r.stderr)
r, started, newlogs = cli("--game-dir", bad)
check(r.returncode == 1 and "--game-dir" in r.stderr and started is None and remembered() == pick,
      "--game-dir naming a wrong folder: exit 1, nothing started, remembered folder kept", r.stderr)
r, started, newlogs = cli("--game-dir", pick)
check(r.returncode == 0 and started == "--run", "--game-dir PATH then starts the game", str(started) + r.stderr)
r = launch("--resolve-only")
check(r.stdout.startswith(pick + "\t") and r.stdout.strip().endswith("remembered"), "the next start uses the remembered pick",
      r.stdout + r.stderr)
r, started, newlogs = cli("--reset-game-dir")
check(r.returncode == 0 and remembered() is None and started is None and "forgot" in r.stderr,
      "--reset-game-dir alone: forgets and exits, nothing started", r.stderr)
r, started, newlogs = cli("--reset-game-dir")
check(r.returncode == 0 and "no Spore folder was remembered" in r.stderr, "--reset-game-dir with nothing remembered: exit 0",
      r.stderr)
r = launch("--reset-game-dir", "--resolve-only")
check(r.returncode == 0 and r.stdout.startswith(dl + "\t") and r.stdout.strip().endswith("found"),
      "--reset-game-dir --resolve-only searches again (usual place found)", r.stdout + r.stderr)

# 15. settings come from the (throwaway) defaults domain
defaults("write", TEST_DOMAIN, "GPUProfile", "9800gtx")
defaults("write", TEST_DOMAIN, "Environment", "-dict", "SPORE_D3D9", "null")
r = launch("--print-env")
check("SPORE_GPU_PROFILE=9800gtx" in r.stdout and "SPORE_D3D9=null" in r.stdout,
      "GPUProfile and Environment read from SPORE_DEFAULTS_DOMAIN", r.stdout + r.stderr)
r = launch("--print-env", env={"SPORE_GPU_PROFILE": "gtx280"})
check("SPORE_GPU_PROFILE=gtx280" in r.stdout, "the environment still wins over a setting", r.stdout)
defaults("delete", TEST_DOMAIN)

# 17. the settings window (GUI checks: the window shows for ~1.5 s, then a test hook acts)
print("17. the settings window")
SETTINGS_SWITCHES = {"SPORE_FULLSCREEN", "SPORE_FULLSCREEN_ESC", "SPORE_RETINA", "SPORE_VSYNC",
                     "SPORE_D3D9_FPS", "SPORE_ONLINE", "game-dir", "DocumentsFolder", "STATE", "WINDOW", "RES",
                     "LOCALE", "SOUND", "-multipleInstances", "-safe", "ARGS", "SPORE_NET_LOG",
                     "SPORE_STALL_SECONDS", "SPORE_METAL_WATCH", "SPORE_METAL_PSO_ARCHIVE", "ShowSettings",
                     "SPORE_FULLSCREEN_SAFE_AREA"}
# rows deliberately not in the window (the switches stay for Terminal use)
REMOVED_SWITCHES = {"GPUProfile", "SPORE_TEXT_INPUT", "SPORE_PRESS_AND_HOLD", "SPORE_GAMEPAD"}
# (a) --list-settings lists every row, six columns (the last is the tooltip), no window
t0 = time.time()
r = launch("--list-settings")
rows = [l.split("\t") for l in r.stdout.splitlines()]
listed = set()
for row in rows:
    if len(row) == 6:
        listed.update(row[2].split("+"))
check(r.returncode == 0 and all(len(x) == 6 for x in rows) and SETTINGS_SWITCHES <= listed and time.time() - t0 < 1.4,
      "--list-settings: %d rows, every expected switch listed, exit 0, no window" % len(rows),
      "missing %s; %s" % (sorted(SETTINGS_SWITCHES - listed), r.stdout + r.stderr))
labels = [x[1] for x in rows if len(x) == 6]
renderer = [x for x in rows if len(x) == 6 and x[2] == "SPORE_D3D9"]
check(not renderer and "Renderer" not in labels, "--list-settings has no Renderer row (Metal is the one renderer)",
      repr(renderer))
check(not (REMOVED_SWITCHES & listed) and labels and not any("(" in l or l.startswith("-") for l in labels)
      and "Show window every launch" in labels and "Safe mode" in labels,
      "no graphics-card or input rows; no label carries parentheses or a raw flag; 'Show window every launch'",
      "%s %s" % (sorted(REMOVED_SWITCHES & listed), labels))
# descriptions are tooltips; every row has one sentence, with no document reference
tips = [(x[1], x[5]) for x in rows if len(x) == 6]
bad_tips = [(l, t) for l, t in tips if not t or not t.endswith(".") or ". " in t or "docs/" in t or len(t) > 100]
check(tips and not bad_tips, "every row (%d) has a one-sentence tooltip and no docs/ reference" % len(tips), repr(bad_tips))
lbin = open(a.launcher, "rb").read()
check(b"docs/" not in lbin and b"(default)" not in lbin and b"(Default)" not in lbin
      and b"Fullscreen area" not in lbin
      and b"Spore Native cannot" not in lbin and b"Quit Spore Native" not in lbin
      and b"Spore - Launcher v" in lbin and b"Match the display" not in lbin and b"Vsync" in lbin,
      "the launcher has no docs/ reference, no default marker, no Fullscreen area row, no 'Spore Native' "
      "in its alerts or menu; the window title is 'Spore - Launcher v<version>'; the pacing default is 'Vsync'", "")
locale_row = [x for x in rows if len(x) == 6 and x[2] == "LOCALE"]
want_locales = sorted(n.lower() for n in os.listdir(os.path.join(GAME, "Data", "Locale"))
                      if os.path.isdir(os.path.join(GAME, "Data", "Locale", n)))
check(locale_row and "en-us" in want_locales and locale_row[0][4].split("|")[1:] == want_locales,
      "Language row lists the install's Data/Locale folders (%d, incl. en-us)" % len(want_locales),
      repr(locale_row) + " " + repr(want_locales))
# (j-a) the Start-in row: -state:<Name> choices, CreatureEditor among them
state_row = [x for x in rows if len(x) == 6 and x[2] == "STATE+SPORE_SKIP_INTRO"]
START_STATES = ["CellEditor", "CreatureEditor", "CellToCreatureEditor", "BuildingEditor", "VehicleEditor",
               "VehicleLandEditor", "VehicleAirEditor", "VehicleWaterEditor", "UFOEditor", "TribalAccessoriesEditor",
               "AdventureAccessoriesEditor", "FloraEditor", "CellGame", "CreatureGame", "TribeGame", "CivilisationGame",
               "SpaceGame"]  # the start states of SporeAppStates.trigger; GalaxyGameEntry is not offered
check(state_row and state_row[0][1] == "Start in" and state_row[0][4].split("|") == ["(unset)", "skip"] + START_STATES,
      "--list-settings: the Start-in row lists Skip intro, then the 17 start states in order (CreatureEditor among "
      "them, no GalaxyGameEntry)",
      repr(state_row))
LIST_BEFORE = r.stdout
# (k-a) the Resolution list computed from the main display (the notch is an item, not a row; the desktop mode is
# the display in points too, DEVIATIONS 3.47)
res_row = [x for x in rows if len(x) == 6 and x[2] == "RES+SPORE_FULLSCREEN_SAFE_AREA"]
m_disp = re.search(r"this display (\d+)x(\d+) points, notch inset (\d+) points", res_row[0][4]) if res_row else None
check(res_row and res_row[0][0] == "Game options" and m_disp,
      "--list-settings: the Resolution row (Game options) owns RES and SPORE_FULLSCREEN_SAFE_AREA and names the display",
      repr(res_row))


def res_items(env):
    """The Resolution choices --list-settings prints for one value of SPORE_RETINA."""
    out = launch("--list-settings", env=env).stdout
    row = [x.split("\t") for x in out.splitlines() if x.split("\t")[2:3] == ["RES+SPORE_FULLSCREEN_SAFE_AREA"]]
    return row[0][4].split(" -- ")[0].split("|") if row else []


def wh(item):
    m = re.match(r"^(?:(Display|Display below the notch) \((\d+)x(\d+)\)|(\d+)x(\d+))$", item)
    if not m:
        return None
    if m.group(1):
        return (int(m.group(2)), int(m.group(3)), "notch" if "notch" in m.group(1) else "display")
    return (int(m.group(4)), int(m.group(5)), None)


# the list: "Display (WxH)", "Display below the notch (WxH)" only with a notch, then the common sizes that fit
NOTCH = {}
if m_disp:
    W, H, inset = int(m_disp.group(1)), int(m_disp.group(2)), int(m_disp.group(3))
    for retina, label in ((False, "points"), (True, "pixels")):
        items = [wh(i) for i in res_items({"SPORE_RETINA": "1"} if retina else {})]
        k = items[0][0] // W if items and items[0] and W else 0
        ok = items and all(items) and k >= 1 and items[0] == (W * k, H * k, "display")
        if inset:
            ok = ok and items[1] == (W * k, (H - inset) * k, "notch")
            NOTCH[retina] = items[1][:2]
        else:
            ok = ok and not any(t == "notch" for _, _, t in items)
        rest = [x for x in items if x and not x[2]]
        ok = ok and rest and all(w <= W * k and h <= H * k for w, h, _ in rest)
        check(ok, "Resolution list, %s: Display %s%s, then %d common sizes that fit" % (
            label, items[0][:2] if items and items[0] else "-",
            ", below the notch = display less the %d pt inset" % inset if inset else ", no notch item", len(rest)), repr(items))
# (b) every SPORE_* switch a row writes is named in docs/DEVIATIONS.md or docs/design/
row_vars = sorted(v for v in listed if v.startswith("SPORE_"))
docs_text = open(os.path.join(ROOT, "docs", "DEVIATIONS.md")).read()
for n in sorted(os.listdir(os.path.join(ROOT, "docs", "design"))):
    if n.endswith(".md"):
        docs_text += open(os.path.join(ROOT, "docs", "design", n)).read()
undocumented = [v for v in row_vars if not re.search(r"\b%s\b" % v, docs_text)]
check(row_vars and not undocumented, "every row's SPORE_* switch (%d) is named in DEVIATIONS.md or docs/design/" % len(row_vars),
      " ".join(undocumented))

gtgt = os.path.join(T, "settings-game.sh")
gout = os.path.join(T, "settings-started.txt")
with open(gtgt, "w") as f:
    f.write('#!/bin/sh\n{ echo "ARGS $*"; env | grep "^SPORE_" | sort; } > "%s"\nexit 0\n' % gout)
os.chmod(gtgt, 0o755)
gexe = test_bundle()
GUI_VARS = set(row_vars) | {"SPORE_GPU_PROFILE", "SPORE_ARGS"}


def gui(*args, test=None, env=None, logdir=None):
    """Runs the throwaway-bundle launcher (AppKit's keys stay in TEST_DOMAIN); returns (result, seconds, args the
    stand-in game got or None, its SPORE_* environment restricted to the settings variables)."""
    if os.path.exists(gout):
        os.unlink(gout)
    e = {k: v for k, v in os.environ.items() if not k.startswith("SPORE_")}
    e.update(HOME=HOME, SPORE_SUPPORT_DIR=SUP, SPORE_LOG_DIR=logdir or os.path.join(T, "logs-settings"),
             SPORE_DEFAULTS_DOMAIN=TEST_DOMAIN, SPORE_LAUNCHER_TARGET=gtgt)
    if test:
        e["SPORE_LAUNCHER_SETTINGS_TEST"] = test
    e.update(env or {})
    t0 = time.time()
    r = subprocess.run([gexe] + list(args), capture_output=True, text=True, env=e, timeout=60)
    dt = time.time() - t0
    if not os.path.exists(gout):
        return r, dt, None, {}
    lines = open(gout).read().splitlines()
    genv = dict(l.split("=", 1) for l in lines[1:] if "=" in l)
    return r, dt, lines[0][5:], {k: v for k, v in genv.items() if k in GUI_VARS}


def stored():
    out = defaults("export", TEST_DOMAIN, "-")
    try:
        d = plistlib.loads(out) if out else {}
    except Exception:
        d = {}
    return {k: d[k] for k in ("Environment", "Arguments", "GPUProfile", "ShowSettings", "DocumentsFolder") if k in d}


# (c) an empty domain: the window shows (every start by default), Play starts the game with nothing added
defaults("delete", TEST_DOMAIN)
r, dt, gargs, genv = gui(test="play")
check(dt >= 1.4 and gargs == "--run" and genv == {} and stored() == {},
      "empty domain: window shown (%.1f s), Play starts the game with no extra environment, writes nothing" % dt,
      "%s %s %s %s" % (gargs, genv, stored(), r.stderr))
# (d) every kind of row set through the hook, then Play
# (no Renderer row: set: refuses the switch)
r, dt, gargs, genv = gui(test="set:SPORE_D3D9=metal")
check(r.returncode == 2 and "no row for 'SPORE_D3D9'" in r.stderr and gargs is None and stored() == {},
      "the window has no Renderer row (set:SPORE_D3D9 finds none), nothing started or written",
      "%s %s %s" % (r.returncode, gargs, r.stderr))
want_env = {"SPORE_ONLINE": "0", "SPORE_RETINA": "1", "SPORE_VSYNC": "0", "SPORE_D3D9_FPS": "20"}
GAME_ARGS = "-f -r:1280x800 -locale:fr-fr -mono -multipleInstances"
defaults("write", TEST_DOMAIN, "GPUProfile", "gtx280")  # Terminal only; the window keeps it and the game gets it
r, dt, gargs, genv = gui(test="set:SPORE_ONLINE=0,SPORE_RETINA=1,FPS=40,"
                              "WINDOW=f,RES=1280x800,LOCALE=fr-fr,SOUND=mono,-multipleInstances=1")
check(gargs == "--run -- " + GAME_ARGS and genv == dict(want_env, SPORE_GPU_PROFILE="gtx280", SPORE_ARGS=GAME_ARGS),
      "set: online, retina, 40 fps and five game options -> the game gets exactly those, SPORE_ARGS too, "
      "and the Terminal-written GPUProfile gtx280", "%s %s %s" % (gargs, genv, r.stderr))
cmd = r.stderr.split("launch command: ")[1].split("\n")[0] if "launch command: " in r.stderr else ""
check(cmd.startswith("SPORE_RETINA=1 SPORE_VSYNC=0 SPORE_D3D9_FPS=20 SPORE_ONLINE=0 "
                     "SPORE_ARGS='%s' " % GAME_ARGS) and cmd.endswith("--no-settings -- " + GAME_ARGS),
      "Copy launch command: the non-default environment, SPORE_ARGS, the launcher, -- arguments", cmd + r.stderr)
st = stored()
check(st == {"Environment": want_env, "GPUProfile": "gtx280", "Arguments": GAME_ARGS.split()},
      "domain: Environment holds only the non-default keys, Arguments set, GPUProfile kept", repr(st))
r = launch("--print-env")
penv = {k: v for k, v in (l.split("=", 1) for l in r.stdout.splitlines() if "=" in l) if k in GUI_VARS}
check(penv == dict(want_env, SPORE_GPU_PROFILE="gtx280", SPORE_ARGS=GAME_ARGS) and r.stdout.rstrip().endswith("--run -- " + GAME_ARGS),
      "--print-env afterwards shows exactly those variables, SPORE_ARGS and the stored arguments", r.stdout + r.stderr)
# (h) a variable set in the environment wins over the stored value (and Save keeps the stored one)
r, dt, gargs, genv = gui(test="save-then-play", env={"SPORE_ONLINE": "1"})
check(genv.get("SPORE_ONLINE") == "1" and stored().get("Environment", {}).get("SPORE_ONLINE") == "0" and genv.get("SPORE_RETINA") == "1",
      "the environment wins over a stored setting; Save leaves the stored value alone", "%s %s" % (genv, stored()))
# (e) Cancel: nothing started, nothing written, no log
before = stored()
cancel_logs = os.path.join(T, "logs-cancel")
r, dt, gargs, genv = gui(test="cancel", logdir=cancel_logs)
check(r.returncode == 0 and dt >= 1.4 and gargs is None and stored() == before and not os.path.exists(cancel_logs)
      and "cancelled" in r.stderr, "Cancel: exit 0, nothing started, nothing written, no log", "%s %s" % (gargs, r.stderr))
# (f) Reset to defaults (then Play): the domain loses Environment, Arguments and GPUProfile
r, dt, gargs, genv = gui(test="reset")
st = stored()
check(gargs == "--run" and genv == {"SPORE_GPU_PROFILE": "gtx280"} and not any(k in st for k in ("Environment", "Arguments"))
      and st.get("GPUProfile") == "gtx280",
      "Reset to defaults: no Environment or Arguments left; GPUProfile (no row) is not the window's to reset",
      "%s %s %s" % (gargs, genv, st))
defaults("delete", TEST_DOMAIN, "GPUProfile")
# (i) an extra argument with a blank reaches the game quoted after its colon; SPORE_ARGS in the environment wins
r, dt, gargs, genv = gui(test='set:ARGS=-dataDir:"C:\\My Data" -noSound')
check(genv.get("SPORE_ARGS") == '-dataDir:"C:\\My Data" -noSound' and stored().get("Arguments") == ["-dataDir:C:\\My Data", "-noSound"],
      "Extra arguments: a value with a blank is quoted after the colon in SPORE_ARGS", "%s %s" % (genv, stored()))
r, dt, gargs, genv = gui(test="set:SOUND=stereo", env={"SPORE_ARGS": "-w"})
check(genv.get("SPORE_ARGS") == "-w" and stored().get("Arguments") == ["-dataDir:C:\\My Data", "-noSound"],
      "SPORE_ARGS in the environment wins; the game options rows keep the stored Arguments", "%s %s" % (genv, stored()))
defaults("delete", TEST_DOMAIN, "Arguments")
# (j) the Start-in popup: -state:<Name> leads the arguments, comes back to the popup, unknown names kept
r, dt, gargs, genv = gui(test="set:STATE=CreatureEditor,SOUND=mono")
check(gargs == "--run -- -state:CreatureEditor -mono" and genv.get("SPORE_ARGS") == "-state:CreatureEditor -mono"
      and stored().get("Arguments") == ["-state:CreatureEditor", "-mono"],
      "set: STATE=CreatureEditor -> the game gets -state:CreatureEditor first, SPORE_ARGS carries it",
      "%s %s %s %s" % (gargs, genv, stored(), r.stderr))
r = launch("--print-env")
check("SPORE_ARGS=-state:CreatureEditor -mono" in r.stdout and r.stdout.rstrip().endswith("--run -- -state:CreatureEditor -mono"),
      "--print-env afterwards shows the start state", r.stdout + r.stderr)
defaults("delete", TEST_DOMAIN, "Arguments")
defaults("write", TEST_DOMAIN, "Arguments", "-array", "-state:CreatureEditor")
r, dt, gargs, genv = gui(test="play")
r2 = launch("--list-settings")
check(gargs == "--run -- -state:CreatureEditor" and genv.get("SPORE_ARGS") == "-state:CreatureEditor"
      and stored().get("Arguments") == ["-state:CreatureEditor"] and r2.stdout == LIST_BEFORE,
      "a stored -state:CreatureEditor goes back to the popup: Play keeps it once (not duplicated, not in Extra arguments)",
      "%s %s %s" % (gargs, genv, stored()))
r, dt, gargs, genv = gui(test="set:ARGS=")
check(stored().get("Arguments") == ["-state:CreatureEditor"],
      "clearing Extra arguments leaves the start state (it lives in the popup)", repr(stored()))
# Skip intro writes SPORE_SKIP_INTRO=1 and no -state:, reloads as that item, and an editor removes it
defaults("delete", TEST_DOMAIN)
r, dt, gargs, genv = gui(test="set:STATE=skip")
st = stored()
check(gargs == "--run" and genv == {"SPORE_SKIP_INTRO": "1"} and st == {"Environment": {"SPORE_SKIP_INTRO": "1"}},
      "set: STATE=skip (Skip intro) -> SPORE_SKIP_INTRO=1 written and given to the game, no -state:", "%s %s %s %s" % (gargs, genv, st, r.stderr))
r, dt, gargs, genv = gui(test="play")
check(gargs == "--run" and genv == {"SPORE_SKIP_INTRO": "1"} and stored() == {"Environment": {"SPORE_SKIP_INTRO": "1"}},
      "a stored SPORE_SKIP_INTRO with no -state: reloads as Skip intro: Play keeps it", "%s %s %s" % (gargs, genv, stored()))
r, dt, gargs, genv = gui(test="set:STATE=CreatureEditor")
check(gargs == "--run -- -state:CreatureEditor" and "SPORE_SKIP_INTRO" not in genv and stored() == {"Arguments": ["-state:CreatureEditor"]},
      "choosing an editor after Skip intro removes SPORE_SKIP_INTRO and writes -state:", "%s %s %s" % (gargs, genv, stored()))
r, dt, gargs, genv = gui(test="set:SPORE_SKIP_INTRO=1")
check(genv.get("SPORE_SKIP_INTRO") == "1" and "Arguments" not in stored(),
      "set: SPORE_SKIP_INTRO=1 picks Skip intro too (the -state: is dropped)", "%s %s" % (genv, stored()))
defaults("delete", TEST_DOMAIN)
defaults("write", TEST_DOMAIN, "Arguments", "-array", "-state:Foo")
r, dt, gargs, genv = gui(test="play")
check(gargs == "--run -- -state:Foo" and stored().get("Arguments") == ["-state:Foo"],
      "an unknown stored -state:Foo is kept unchanged through Play (a custom popup item)",
      "%s %s %s" % (gargs, genv, stored()))
defaults("delete", TEST_DOMAIN, "Arguments")
# (k) the Resolution box: the display items, the below-the-notch item writing SPORE_FULLSCREEN_SAFE_AREA,
# a typed size, an unlisted stored size
def env_of(out, name):
    return [l.split("=", 1)[1] for l in out.splitlines() if l.startswith(name + "=")]


if m_disp:
    r, dt, gargs, genv = gui(test="set:RES=Display (%dx%d)" % (W, H))
    check(stored().get("Arguments") == ["-r:%dx%d" % (W, H)] and "SPORE_FULLSCREEN_SAFE_AREA" not in stored().get("Environment", {}),
          "the Display item stores only -r:%dx%d (no safe-area variable)" % (W, H), "%s %s" % (stored(), r.stderr))
if NOTCH:
    nw, nh = NOTCH[True]
    r, dt, gargs, genv = gui(test="set:SPORE_FULLSCREEN_SAFE_AREA=1,RES=4112x2582,SPORE_RETINA=1")
    out = launch("--print-env").stdout
    check(env_of(out, "SPORE_FULLSCREEN_SAFE_AREA") == ["1"] and env_of(out, "SPORE_RETINA") == ["1"]
          and env_of(out, "SPORE_ARGS") == ["-r:%dx%d" % (nw, nh)] and gargs == "--run -- -r:%dx%d" % (nw, nh),
          "set: Retina and the below-the-notch item -> -r:%dx%d and SPORE_FULLSCREEN_SAFE_AREA=1; --print-env shows all three"
          % (nw, nh), out + r.stderr)
    r, dt, gargs, genv = gui(test="play")
    check(gargs == "--run -- -r:%dx%d" % (nw, nh) and genv.get("SPORE_FULLSCREEN_SAFE_AREA") == "1"
          and stored().get("Environment", {}).get("SPORE_FULLSCREEN_SAFE_AREA") == "1",
          "a stored below-the-notch -r: with the variable set reloads as that item: Play keeps both", "%s %s" % (gargs, stored()))
    r, dt, gargs, genv = gui(test="set:SPORE_RETINA=0")
    pw, ph = NOTCH[False]
    check(gargs == "--run -- -r:%dx%d" % (pw, ph) and genv.get("SPORE_FULLSCREEN_SAFE_AREA") == "1",
          "Retina off: the below-the-notch item follows into points (-r:%dx%d), the variable stays" % (pw, ph),
          "%s %s %s" % (gargs, genv, r.stderr))
    r, dt, gargs, genv = gui(test="set:RES=1280x800")
    check(stored().get("Arguments") == ["-r:1280x800"] and "SPORE_FULLSCREEN_SAFE_AREA" not in stored().get("Environment", {})
          and gargs == "--run -- -r:1280x800",
          "choosing anything else (a typed 1280x800) removes SPORE_FULLSCREEN_SAFE_AREA", "%s %s" % (gargs, stored()))
else:
    r, dt, gargs, genv = gui(test="set:RES=1280x800")
    check(stored().get("Arguments") == ["-r:1280x800"] and gargs == "--run -- -r:1280x800",
          "a typed RES=1280x800 still works (this display has no notch: no below-the-notch item)", "%s %s" % (gargs, stored()))
defaults("delete", TEST_DOMAIN)
defaults("write", TEST_DOMAIN, "Arguments", "-array", "-r:1234x567")
defaults("write", TEST_DOMAIN, "Environment", "-dict", "SPORE_FULLSCREEN_SAFE_AREA", "1")
r, dt, gargs, genv = gui(test="play")
check(gargs == "--run -- -r:1234x567" and stored().get("Arguments") == ["-r:1234x567"] and "Environment" not in stored(),
      "a stored -r: size that is not in the list reloads unchanged; a stray safe-area variable without its item is dropped",
      "%s %s" % (gargs, stored()))
defaults("delete", TEST_DOMAIN)

# (g) ShowSettings false skips the window; --settings shows it anyway; --no-settings and SPORE_LAUNCHER_NO_PANEL skip it
r, dt, gargs, genv = gui(test="set:ShowSettings=0")
check(stored().get("ShowSettings") is False, "the show-every-time box cleared -> ShowSettings false", repr(stored()))
r, dt, gargs, genv = gui(test="play")
check(gargs == "--run" and dt < 1.4, "ShowSettings false: no window, the game starts (%.1f s)" % dt, r.stderr)
r, dt, gargs, genv = gui("--settings", test="play")
check(gargs == "--run" and dt >= 1.4, "--settings shows the window anyway (%.1f s)" % dt, r.stderr)
defaults("delete", TEST_DOMAIN, "ShowSettings")
r, dt, gargs, genv = gui("--no-settings", test="play")
check(gargs == "--run" and dt < 1.4, "--no-settings skips it for one start (%.1f s)" % dt, r.stderr)
r, dt, gargs, genv = gui(test="play", env={"SPORE_LAUNCHER_NO_PANEL": "1"})
check(gargs == "--run" and dt < 1.4, "SPORE_LAUNCHER_NO_PANEL alone suppresses the window (%.1f s)" % dt, r.stderr)
defaults("delete", TEST_DOMAIN)

# the throwaway domain leaves an empty plist behind after `defaults delete`; remove it when it is empty
tp = os.path.expanduser("~/Library/Preferences/%s.plist" % TEST_DOMAIN)
try:
    if plistlib.load(open(tp, "rb")) == {}:
        os.remove(tp)
except (OSError, plistlib.InvalidFileException):
    pass

# 16. the player's real settings domain was not touched
after = real_domain_state()
check(after == REAL_BEFORE, "real %s unchanged (export and plist mtime, before == after)" % REAL_DOMAIN,
      "before %r after %r" % (REAL_BEFORE[1], after[1]))

print("result: %s" % ("all launcher tests pass" if not fails else "%d FAILED" % fails))
sys.exit(1 if fails else 0)
