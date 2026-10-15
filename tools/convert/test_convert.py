#!/usr/bin/env python3
"""Tests tools/convert/convert.py. Never starts the game: the app's launcher is asked only for --version and
--print-env, with a stand-in exec target (as tools/bundle/test_launcher.py does), and the link stage's own smoke run
exits before any game code.

Usage:   python3 -P tools/convert/test_convert.py [--game GAME_DIR] [--quick] [--keep]
When:    after changing convert.py, or any pipeline tool (the full run).

  (a) refusals: a folder without SporebinEP1; an exe with the right name and wrong bytes; the SporebinEP1 folder
      itself is taken as its parent
  (b) --dry-run lists every stage in order
  (c) the prerequisite check names a missing Homebrew package (--brew-prefix pointing at an empty folder)
  (d) the whole pipeline, stage by stage (--stage, each under a 10-minute timeout), into a fresh work folder; then
      a second full run, which must skip every stage
  (e) the app passes make_bundle.py --verify-only, has no Frameworks folder; its launcher answers --version and
      --print-env
  (f) the game-dir file was written (to a throwaway SPORE_SUPPORT_DIR, never the player's own)
--quick runs (a) to (c) only (seconds). Everything goes to a temporary folder, deleted at the end unless --keep.
The full run takes about 7 minutes on an M4 Max. The fake installs link to the real exe and Data (nothing copied).
"""
import argparse
import os
import re
import shutil
import subprocess
import sys
import tempfile
import time

ROOT = os.path.realpath(os.path.join(os.path.dirname(os.path.abspath(__file__)), "..", ".."))
CONVERT = os.path.join(ROOT, "tools", "convert", "convert.py")
ap = argparse.ArgumentParser(description=__doc__, formatter_class=argparse.RawDescriptionHelpFormatter)
ap.add_argument("--game", default=os.path.join(ROOT, "game"), help="your Spore folder (default game/)")
ap.add_argument("--quick", action="store_true", help="run (a) to (c) only")
ap.add_argument("--keep", action="store_true", help="keep the temporary folder")
a = ap.parse_args()
GAME = os.path.realpath(a.game)
T = os.path.realpath(tempfile.mkdtemp(prefix="spore-convert-test-"))
SUP = os.path.join(T, "support")
fails = 0
ORDER = ["manifest", "eh", "imports", "threads", "translate", "harness", "link", "bundle", "settings"]


def check(ok, what, detail=""):
    global fails
    print("  %s  %s%s" % ("ok  " if ok else "FAIL", what, "" if ok else "  [%s]" % detail.strip()[-3000:]), flush=True)
    fails += not ok


def env():
    e = {k: v for k, v in os.environ.items() if not k.startswith(("SPORE_", "SG_"))}
    e["SPORE_SUPPORT_DIR"] = SUP   # the settings stage must never write the player's own game-dir file
    return e


def convert(*args, timeout=120):
    try:
        r = subprocess.run([sys.executable, "-P", CONVERT] + list(args), capture_output=True, text=True,
                           timeout=timeout, env=env(), cwd=T)
        return r.returncode, r.stdout + r.stderr
    except subprocess.TimeoutExpired as e:
        return "timeout", str(e.stdout or "")


def install(path, exe="real"):
    os.makedirs(os.path.join(path, "SporebinEP1"))
    e = os.path.join(path, "SporebinEP1", "SporeApp.exe")
    if exe == "real":
        os.symlink(os.path.join(GAME, "SporebinEP1", "SporeApp.exe"), e)
    else:
        with open(e, "wb") as f:
            f.write(b"MZ" + b"\0" * 4096)  # right name, wrong bytes
    os.symlink(os.path.join(GAME, "Data"), os.path.join(path, "Data"))
    os.makedirs(os.path.join(path, "DataEP1"))
    return path


try:
    WORK = os.path.join(T, "work")
    OUT = os.path.join(T, "out")
    common = ["-o", OUT, "--work", WORK]

    print("(a) refusals and input forms")
    empty = os.path.join(T, "no-game")
    os.makedirs(empty)
    rc, out = convert(empty, *common, "--dry-run")
    check(rc == 1 and "SporebinEP1" in out and "GOG" in out, "a folder without SporebinEP1 is refused", out)
    wrong = install(os.path.join(T, "wrong-bytes"), exe="fake")
    rc, out = convert(wrong, *common, "--dry-run")
    check(rc == 1 and "not the GOG build of Spore Galactic Adventures" in out,
          "an exe with the right name and wrong bytes is refused", out)
    noep1 = os.path.join(T, "no-ep1")
    os.makedirs(os.path.join(noep1, "SporebinEP1"))
    os.symlink(os.path.join(GAME, "SporebinEP1", "SporeApp.exe"), os.path.join(noep1, "SporebinEP1", "SporeApp.exe"))
    os.symlink(os.path.join(GAME, "Data"), os.path.join(noep1, "Data"))
    rc, out = convert(noep1, *common, "--dry-run")
    check(rc == 1 and "DataEP1" in out, "a folder without DataEP1 is refused (Galactic Adventures required)", out)
    good = install(os.path.join(T, "good"))
    rc, out = convert(os.path.join(good, "SporebinEP1"), *common, "--dry-run")
    check(rc == 0 and "(GOG): %s (" % good in out, "the SporebinEP1 folder itself is normalised to its parent", out)
    parent = os.path.join(T, "downloads")
    inner = install(os.path.join(parent, "Spore"))
    os.makedirs(os.path.join(parent, "other"))
    rc, out = convert(parent, *common, "--dry-run")
    check(rc == 0 and "(GOG): %s (" % inner in out, "a folder with exactly one install inside is taken", out)
    check("warning: no SporeBin/SporeApp.exe" in out, "a missing base-game exe is a warning, not a refusal", out)

    print("(b) --dry-run")
    rc, out = convert(GAME, *common, "--dry-run")
    listed = re.findall(r"^\s*\d+\. (\w+)", out, re.M)
    check(rc == 0 and listed == ORDER, "--dry-run lists the stages in order: %s" % " ".join(listed), out)
    check(not os.path.exists(WORK) and not os.path.exists(OUT), "--dry-run writes nothing")
    rc, out = convert("--version")
    check(rc == 0 and out.startswith("spore-native converter 0."), "--version: %s" % out.strip(), out)

    print("(c) prerequisites")
    nobrew = os.path.join(T, "empty-prefix")
    os.makedirs(nobrew)
    fake_setup = os.path.join(T, "inst", "setup_spore_test.exe")
    os.makedirs(os.path.dirname(fake_setup))
    open(fake_setup, "wb").close()
    rc, out = convert(fake_setup, *common, "--brew-prefix", nobrew, "--extract-to", os.path.join(T, "x"))
    check(rc == 1 and re.search(r"missing Homebrew package.*innoextract", out) and
          "brew bundle --file" in out and "Brewfile" in out,
          "installer input with no Homebrew: innoextract named, the brew bundle line printed", out)
    check(not os.path.exists(os.path.join(T, "x")) and not os.path.exists(WORK), "nothing extracted or written")

    if not a.quick:
        print("(d) the pipeline, stage by stage, into a fresh work folder")
        for s in ORDER:
            t0 = time.time()
            rc, out = convert(GAME, *common, "--stage", s, "--timeout", "590", timeout=600)
            line = [l for l in out.splitlines() if re.match(r"\s*\d+/\d+\s+%s\b" % s, l)]
            check(rc == 0 and line and "skipped" not in line[0],
                  "stage %-9s %4.0f s" % (s, time.time() - t0), out[-3000:])
            if rc != 0:
                break
        rc, out = convert(GAME, *common, "--timeout", "590", timeout=600)
        skipped = re.findall(r"^\s*\d+/\d+\s+(\w+).*skipped \(up to date\)", out, re.M)
        check(rc == 0 and skipped == ORDER, "a second run skips every stage (%d of %d)" % (len(skipped), len(ORDER)), out)
        check("Open Spore.app to play." in out, "the run ends with the closing line", out)

        print("(e) the app")
        app = os.path.join(OUT, "Spore.app")
        r = subprocess.run([sys.executable, "-P", os.path.join(ROOT, "tools", "bundle", "make_bundle.py"), "--verify-only",
                            "--out", app, "--game-dir", GAME], capture_output=True, text=True, timeout=600, env=env())
        check(r.returncode == 0 and "result: all checks pass" in r.stdout, "make_bundle.py --verify-only passes",
              r.stdout[-2000:] + r.stderr)
        check(not os.path.exists(os.path.join(app, "Contents", "Frameworks")), "no Frameworks folder")
        launcher = os.path.join(app, "Contents", "MacOS", "Spore Native")
        r = subprocess.run([launcher, "--version"], capture_output=True, text=True, timeout=30, env=env())
        check(r.returncode == 0 and "Spore Native" in r.stdout, "launcher --version: %s" % r.stdout.strip(), r.stderr)
        tgt = os.path.join(T, "stand-in.sh")   # never the real spore-game
        with open(tgt, "w") as f:
            f.write("#!/bin/sh\necho stand-in \"$@\"\n")
        os.chmod(tgt, 0o755)
        e = env()
        e.update(SPORE_LAUNCHER_TARGET=tgt, SPORE_LAUNCHER_NO_PANEL="1", SPORE_LOG_DIR=os.path.join(T, "logs"),
                 SPORE_DEFAULTS_DOMAIN="com.spore-native.player.convert-test", HOME=os.path.join(T, "home"))
        os.makedirs(e["HOME"])
        r = subprocess.run([launcher, "--print-env", "--no-settings"], capture_output=True, text=True, timeout=30, env=e)
        check(r.returncode == 0 and ("SPORE_GAME_DIR=%s" % GAME) in r.stdout,
              "launcher --print-env resolves the converted Spore folder from the remembered setting", r.stdout + r.stderr)

        print("(f) the first-run setting")
        gd = os.path.join(SUP, "game-dir")
        got = open(gd).read() if os.path.exists(gd) else None
        check(got == GAME + "\n", "%s holds the Spore folder" % gd, repr(got))
finally:
    if a.keep:
        print("kept", T)
    else:
        shutil.rmtree(T, ignore_errors=True)
print("result: %s" % ("all checks pass" if not fails else "%d check(s) FAILED" % fails))
sys.exit(1 if fails else 0)
