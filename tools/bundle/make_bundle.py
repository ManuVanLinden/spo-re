#!/usr/bin/env python3
"""Builds Spore.app (default build/Spore.app) from the linked game binary (docs/design/bundle.md).

Usage:
    python3 -P tools/bundle/make_bundle.py                 # the app (the Metal back end), ad hoc signed
    python3 -P tools/bundle/make_bundle.py --verify-only   # re-run the checks on an existing app
    python3 -P tools/bundle/make_bundle.py --list-gpu-profiles [GAME_DIR]
    python3 -P tools/bundle/make_bundle.py --after-link     # relink (tools/link/build.py, no game run), then bundle

Inputs:  build/sporenative/spore (--binary), the Spore folder (--game-dir: the icon and the checks)
Outputs: the app (--out), checked; a work folder under build/bundle/
When:    the converter's bundle stage; by hand after every relink (--after-link does both).
The player's guide is tools/bundle/README.md; the design is docs/design/bundle.md.

Layout:
    Spore.app/Contents/Info.plist              CFBundleName and CFBundleDisplayName "Spore" (the Dock, menu bar and
                                                Finder name); identifier com.spore-native.player; executable "Spore Native"
    Spore.app/Contents/MacOS/Spore Native     the launcher (tools/bundle/launcher.m)
    Spore.app/Contents/MacOS/spore-game       build/sporenative/spore, Homebrew rpath removed
    Spore.app/Contents/Resources/AppIcon.icns  the game's own icon from the player's SporeApp.exe (exe_icon.py),
                                                re-encoded at build time; --own-icon uses tools/bundle/icon.m instead
No Contents/Frameworks: the game binary links only system frameworks (Metal is the one renderer).
Nothing from the game goes in: no exe, no data, no translated C, no shader cache. The checks at the end prove
it by comparing every file of the app against every file of the Spore folder (size, then SHA-256).
Environment: SPORE_WORK_DIR moves the defaults of --binary, --out, --link-src and the work folder from the source
tree's build/ to $SPORE_WORK_DIR/build/; SPORE_GAME_DIR replaces game/ as the --game-dir default
(docs/design/convert.md 4).
"""
import argparse
import fnmatch
import hashlib
import os
import plistlib
import re
import shutil
import subprocess
import sys

ROOT = os.path.realpath(os.path.join(os.path.dirname(os.path.abspath(__file__)), "..", ".."))
HERE = os.path.join(ROOT, "tools", "bundle")
WORK = os.path.realpath(os.environ.get("SPORE_WORK_DIR") or ROOT)
GAME = os.environ.get("SPORE_GAME_DIR") or os.path.join(ROOT, "game")
BUNDLE_ID = "com.spore-native.steam-player"
NAME = "Spore Native"      # the launcher executable (Contents/MacOS), the log folder name
DISPLAY_NAME = "Spore Native (Steam)"     # CFBundleName/CFBundleDisplayName: what the Dock, the menu bar and Finder show

# GPU profiles (the runtime table is in runtime/d3d9null/src/d3d9_adapter.c)
GPU_PROFILES = [("8800gtx", 0x0191, "default: the identity the runtime reports unless told otherwise"),
                ("9800gtx", 0x0612, "the 2008 G92 successor of the 8800 line"),
                ("gtx280", 0x05E1, "the top single-GPU card of Galactic Adventures' release year")]


def run(argv, **kw):
    kw.setdefault("timeout", 300)
    return subprocess.run(argv, check=True, capture_output=True, text=True, **kw).stdout


def log(*a):
    print("[bundle]", *a, flush=True)


def git_version():
    """The version: "<tag> (<git describe>)" and the build number (the commit count).

    The dotted part is the nearest tag, v0.1.0 -> 0.1.0 (CFBundleShortVersionString wants dotted numbers), and
    stays the same until the next tag; describe adds -<n>-g<hash> for commits past the tag, and -dirty for
    uncommitted changes. Without a reachable tag the dotted part is 0.0 and describe is just the hash."""
    try:
        d = run(["git", "-C", ROOT, "describe", "--always", "--dirty", "--tags"]).strip()
        n = run(["git", "-C", ROOT, "rev-list", "--count", "HEAD"]).strip()
    except Exception:
        return "0.0-unknown", "0"
    m = re.match(r"v?(\d+(?:\.\d+)*)(?:-|$)", d)
    short = m.group(1) if m else "0.0"
    return "%s (%s; Steam fix 1)" % (short, d), str(int(n) + 1)


def min_os(binary):
    out = run(["otool", "-l", binary])
    m = re.search(r"LC_BUILD_VERSION.*?minos ([0-9.]+)", out, re.S)
    return m.group(1) if m else "14.0"


def exe_sha():
    h = open(os.path.join(ROOT, "runtime", "image", "include", "sg_image.h")).read()
    return re.search(r'#define SG_IMAGE_SHA256 "([0-9a-f]{64})"', h).group(1)


def rpaths(binary):
    out = run(["otool", "-l", binary])
    return re.findall(r"cmd LC_RPATH\n\s+cmdsize \d+\n\s+path (.+?) \(offset", out)


def deps(binary):
    lines = run(["otool", "-L", binary]).splitlines()[1:]
    return [l.strip().split(" (compatibility")[0] for l in lines if l.strip()]


def macho_files(app):
    for d, _, fs in os.walk(app):
        for f in fs:
            p = os.path.join(d, f)
            if os.path.islink(p):
                continue
            with open(p, "rb") as fh:
                magic = fh.read(4)
            if magic in (b"\xcf\xfa\xed\xfe", b"\xca\xfe\xba\xbe", b"\xfe\xed\xfa\xcf"):
                yield p


# ---- build ------------------------------------------------------------------------------------------------

def build_launcher(out, version, minos):
    run(["clang", "-O2", "-fobjc-arc", "-Wall", "-Wextra", "-Wno-unused-parameter",
         "-mmacosx-version-min=" + minos,
         '-DSPORE_EXE_SHA256="%s"' % exe_sha(), '-DSPORE_BUNDLE_VERSION="%s"' % version,
         "-framework", "AppKit", "-framework", "CoreFoundation",
         os.path.join(HERE, "launcher.m"), "-o", out], timeout=180)


def build_icon(work, out_icns, game_dir=None, own=False):
    """The game's own icon from the player's SporeApp.exe (tools/bundle/exe_icon.py) when the install is at hand,
    else the icon we draw ourselves (icon.m). --own-icon forces the drawn one."""
    exe = os.path.join(game_dir, "SporebinEP1", "SporeApp.exe") if game_dir else None
    if not own and exe and os.path.exists(exe):
        sys.path.insert(0, HERE)
        import exe_icon
        size = exe_icon.make_icns(exe, out_icns)
        log("icon: the game's own %dx%d icon from %s (re-encoded; build/ only, never committed)" % (size, size, exe))
        return
    log("icon: drawn by icon.m (%s)" % ("--own-icon" if own else "no Spore folder"))
    tool = os.path.join(work, "icon")
    run(["clang", "-O2", "-fobjc-arc", "-Wall", "-framework", "CoreGraphics", "-framework", "ImageIO",
         "-framework", "CoreFoundation", "-framework", "Foundation", "-framework", "UniformTypeIdentifiers",
         os.path.join(HERE, "icon.m"), "-o", tool], timeout=180)
    iconset = os.path.join(work, "AppIcon.iconset")
    shutil.rmtree(iconset, ignore_errors=True)
    run([tool, iconset], timeout=60)
    run(["iconutil", "-c", "icns", iconset, "-o", out_icns], timeout=60)


def info_plist(version, build, minos):
    return {
        "CFBundleDevelopmentRegion": "en",
        "CFBundleExecutable": NAME,
        "CFBundleIdentifier": BUNDLE_ID,
        "CFBundleName": DISPLAY_NAME,
        "CFBundleDisplayName": DISPLAY_NAME,
        "CFBundleIconFile": "AppIcon",
        "CFBundlePackageType": "APPL",
        "CFBundleShortVersionString": version.split(" ")[0],
        "CFBundleVersion": build,
        "CFBundleGetInfoString": "Spore Native %s: a native runtime for this local Steam Galactic Adventures "
                                 "executable. The game itself is not included." % version,
        "CFBundleInfoDictionaryVersion": "6.0",
        "LSMinimumSystemVersion": minos,
        "LSArchitecturePriority": ["arm64"],
        "LSApplicationCategoryType": "public.app-category.role-playing-games",
        "NSHighResolutionCapable": True,
        "NSPrincipalClass": "NSApplication",
        "NSSupportsAutomaticGraphicsSwitching": True,
        "NSDocumentsFolderUsageDescription": "Spore keeps \"My Spore Creations\" (creation cards, pictures and "
                                             "videos) in your Documents folder, as it does on Windows.",
        "NSDownloadsFolderUsageDescription": "Spore looks for your converted Steam Spore folder in Downloads.",
        "NSDesktopFolderUsageDescription": "Spore reads your converted Steam Spore folder from where you put it.",
    }


GAME_PROCESS_PATTERN = "spore-game|sporenative/spore --run"


def game_processes(pattern=GAME_PROCESS_PATTERN):
    r = subprocess.run(["pgrep", "-fl", pattern], capture_output=True, text=True, timeout=30)
    return [l for l in r.stdout.splitlines() if l.strip()]


def relink(a):
    """--after-link: refuse while any game process runs (a relink never replaces the binary under a running game),
    then run the whole-image link. No game process is started; build.py only compiles and links."""
    running = game_processes()
    if running and not a.force:
        sys.exit("make_bundle: a game process is running, not relinking (quit the game first, or --force):\n  " +
                 "\n  ".join(running))
    argv = [sys.executable, "-P", os.path.join(ROOT, "tools", "link", "build.py"), "--src", a.link_src,
            "--jobs", str(a.jobs)]
    log("relink:", " ".join(argv))
    import time
    t0 = time.time()
    r = subprocess.run(argv, cwd=ROOT, timeout=1800)
    if r.returncode:
        sys.exit("make_bundle: the link failed (exit %d); the bundle was not rebuilt" % r.returncode)
    log("relink done in %.0f s" % (time.time() - t0))


def build(a):
    binary = os.path.realpath(a.binary)
    if not os.path.exists(binary):
        sys.exit("no %s: link it first (python3 -P tools/link/build.py --src build/harness/src)" % binary)
    version, build_no = git_version()
    minos = min_os(binary)
    work = os.path.join(WORK, "build", "bundle")
    os.makedirs(work, exist_ok=True)
    out = os.path.realpath(a.out)
    # replacing a bundle whose game is running deletes files under a live process: refuse unless asked
    running = game_processes(re.escape(os.path.join(out, "Contents", "MacOS", "spore-game")))
    if running and not a.force:
        sys.exit("make_bundle: %s is running (%s); quit the game first, write elsewhere with --out, or --force"
                 % (out, running[0]))
    tmp = out + ".tmp"
    shutil.rmtree(tmp, ignore_errors=True)
    macos = os.path.join(tmp, "Contents", "MacOS")
    res = os.path.join(tmp, "Contents", "Resources")
    os.makedirs(macos)
    os.makedirs(res)
    log("version %s, build %s, minimum macOS %s" % (version, build_no, minos))
    build_launcher(os.path.join(macos, NAME), version, minos)
    game = os.path.join(macos, "spore-game")
    shutil.copy2(binary, game)
    for rp in rpaths(game):
        run(["install_name_tool", "-delete_rpath", rp, game])
    build_icon(work, os.path.join(res, "AppIcon.icns"), a.game_dir, a.own_icon)
    with open(os.path.join(tmp, "Contents", "Info.plist"), "wb") as f:
        plistlib.dump(info_plist(version, build_no, minos), f)
    with open(os.path.join(tmp, "Contents", "PkgInfo"), "w") as f:
        f.write("APPL????")
    # ad hoc signature: the game binary first, then the bundle (which signs the launcher as its main executable)
    run(["codesign", "--force", "--sign", a.sign, "--timestamp=none", game])
    run(["codesign", "--force", "--sign", a.sign, "--timestamp=none", tmp])
    shutil.rmtree(out, ignore_errors=True)
    os.rename(tmp, out)
    log("wrote", out)
    return out


# ---- checks -------------------------------------------------------------------------------------------------

def game_index(game_dir):
    """size -> [paths] for every file of the Spore folder (followed through the symlink)."""
    idx = {}
    for d, _, fs in os.walk(os.path.realpath(game_dir)):
        for f in fs:
            p = os.path.join(d, f)
            try:
                idx.setdefault(os.path.getsize(p), []).append(p)
            except OSError:
                pass
    return idx


def sha(p):
    h = hashlib.sha256()
    with open(p, "rb") as f:
        for b in iter(lambda: f.read(1 << 20), b""):
            h.update(b)
    return h.hexdigest()


def verify(app, game_dir):
    fails = []

    def check(ok, what):
        print("  %s  %s" % ("ok  " if ok else "FAIL", what))
        if not ok:
            fails.append(what)

    print("checks on", app)
    plist = os.path.join(app, "Contents", "Info.plist")
    r = subprocess.run(["plutil", "-lint", plist], capture_output=True, text=True, timeout=30)
    check(r.returncode == 0, "plutil -lint: " + r.stdout.strip())
    info = plistlib.load(open(plist, "rb"))
    check(info.get("CFBundleIdentifier") == BUNDLE_ID and info.get("CFBundleName") == DISPLAY_NAME and
          info.get("CFBundleDisplayName") == DISPLAY_NAME and
          info.get("NSHighResolutionCapable") is True and info.get("CFBundleExecutable") == NAME,
          "Info.plist: %s, %s, version %s (%s), LSMinimumSystemVersion %s, NSHighResolutionCapable %s" % (
              info.get("CFBundleIdentifier"), info.get("CFBundleName"), info.get("CFBundleShortVersionString"),
              info.get("CFBundleVersion"), info.get("LSMinimumSystemVersion"), info.get("NSHighResolutionCapable")))
    r = subprocess.run(["codesign", "--verify", "--deep", "--strict", "--verbose=2", app], capture_output=True,
                       text=True, timeout=300)
    check(r.returncode == 0, "codesign --verify --deep --strict: " + " / ".join(r.stderr.strip().splitlines()))
    r = subprocess.run(["codesign", "-dv", app], capture_output=True, text=True, timeout=60)
    sig = [l for l in r.stderr.splitlines() if l.startswith(("Identifier=", "Signature=", "Format="))]
    print("        " + "; ".join(sig))
    homebrew = []
    for m in macho_files(app):
        rel = os.path.relpath(m, app)
        for d in deps(m):
            if d.startswith(("/opt/homebrew", "/usr/local")):
                homebrew.append("%s -> %s" % (rel, d))
        for rp in rpaths(m):
            if rp.startswith(("/opt/homebrew", "/usr/local")):
                homebrew.append("%s rpath %s" % (rel, rp))
        print("        otool -L %s: %s" % (rel, ", ".join(os.path.basename(d) for d in deps(m))))
        print("        LC_RPATH %s: %s" % (rel, ", ".join(rpaths(m)) or "(none)"))
    check(not homebrew, "no Homebrew or /usr/local paths in any Mach-O" + (": " + "; ".join(homebrew) if homebrew else ""))
    check(not os.path.exists(os.path.join(app, "Contents", "Frameworks")), "no Contents/Frameworks (system frameworks only)")
    check_icon(app, check)
    # nothing from the game: names, then content (every file against every game file of the same size)
    files = []
    for d, _, fs in os.walk(app):
        for f in fs:
            files.append(os.path.join(d, f))
    bad_names = [f for f in files if re.search(r"\.(exe|dll|package|mdmp|smt|fxp|ini)$|SporeApp|VideoCards|ConfigManager"
                                                r"|Properties\.txt|shadercache|\.msl$", f, re.I)]
    check(not bad_names, "no game-named files (exe, dll, package, config, shader cache)" +
          (": " + ", ".join(bad_names) if bad_names else ""))
    if game_dir and os.path.isdir(game_dir):
        idx = game_index(game_dir)
        same = []
        for f in files:
            s = os.path.getsize(f)
            if s in idx:
                h = sha(f)
                same += ["%s == %s" % (os.path.relpath(f, app), g) for g in idx[s] if sha(g) == h]
        check(not same, "no bundle file equals any of the %d files of %s (size, then SHA-256)" % (
            sum(len(v) for v in idx.values()), game_dir) + (": " + "; ".join(same) if same else ""))
        # and no file carries the exe's bytes inside it (the linked binary loads SporeApp.exe from the install
        # at run time; an --embed link would carry it): 32 windows of 64 bytes spread over the exe's sections
        exe = os.path.join(game_dir, "SporebinEP1", "SporeApp.exe")
        if os.path.exists(exe):
            blob = open(exe, "rb").read()
            step = (len(blob) - 0x2000) // 32
            probes = [blob[0x1000 + i * step:0x1000 + i * step + 64] for i in range(32)]
            probes = [w for w in probes if len(set(w)) > 16]  # skip zero fill and padding
            hits = []
            for f in files:
                data = open(f, "rb").read()
                n = sum(1 for w in probes if w in data)
                if n:
                    hits.append("%s (%d of %d windows)" % (os.path.relpath(f, app), n, len(probes)))
            check(not hits, "no bundle file contains SporeApp.exe bytes (%d distinct 64-byte windows searched)" % len(probes) +
                  (": " + "; ".join(hits) if hits else ""))
    else:
        print("  skip  content comparison (no Spore folder)")
    print("  listing (%d files):" % len(files))
    for f in sorted(files):
        print("        %10d  %s" % (os.path.getsize(f), os.path.relpath(f, os.path.dirname(app))))
    return fails


ICON_REPS = [("icon_16x16.png", 16), ("icon_16x16@2x.png", 32), ("icon_32x32.png", 32), ("icon_32x32@2x.png", 64),
             ("icon_128x128.png", 128), ("icon_128x128@2x.png", 256), ("icon_256x256.png", 256)]


def check_icon(app, check):
    """iconutil round trip of Resources/AppIcon.icns: every size Finder and the Dock ask for, at its pixel size.
    The game's icon is 256 px at most, so 256@2x, 512 and 1024 are left to Finder's scaling of the 256 px image."""
    icns = os.path.join(app, "Contents", "Resources", "AppIcon.icns")
    if not os.path.exists(icns):
        check(False, "Resources/AppIcon.icns exists")
        return
    import tempfile
    work = tempfile.mkdtemp(prefix="icns-check-")
    try:
        iconset = os.path.join(work, "AppIcon.iconset")
        r = subprocess.run(["iconutil", "-c", "iconset", icns, "-o", iconset], capture_output=True, text=True, timeout=60)
        got = {}
        for f in sorted(os.listdir(iconset)) if r.returncode == 0 else []:
            out = subprocess.run(["sips", "-g", "pixelWidth", "-g", "pixelHeight", os.path.join(iconset, f)],
                                 capture_output=True, text=True, timeout=30).stdout
            w = re.search(r"pixelWidth: (\d+)", out)
            h = re.search(r"pixelHeight: (\d+)", out)
            got[f] = (int(w.group(1)) if w else 0, int(h.group(1)) if h else 0)
        print("        icon reps (iconutil -c iconset): " + ", ".join("%s %dx%d" % (f, *wh) for f, wh in got.items()))
        bad = [n for n, px in ICON_REPS if got.get(n) != (px, px)]
        check(r.returncode == 0 and not bad, "AppIcon.icns round trip: %d reps, 16-256 px at 1x and 2x%s" % (
            len(got), (": missing or wrong " + ", ".join(bad)) if bad else ""))
    finally:
        shutil.rmtree(work, ignore_errors=True)


# ---- GPU profiles -------------------------------------------------------------------------------------------

def list_gpu_profiles(game_dir):
    """Reads the install's VideoCards.txt and ConfigManager.txt at run time and reports, for each profile, the
    name the game will match and the ConfigManager rules whose card-name pattern it hits."""
    cfg = os.path.join(game_dir, "Data", "Config")
    vc = open(os.path.join(cfg, "VideoCards.txt"), errors="replace").read().splitlines()
    names, vendor = {}, None
    for line in vc:
        s = line.split("#", 1)[0].strip()
        m = re.match(r'vendor\s+"([^"]+)"\s+(.*)', s)
        if m:
            vendor = [int(x, 16) for x in m.group(2).split()]
            continue
        if s == "end":
            vendor = None
        m = re.match(r'card\s+(0x[0-9a-fA-F]+)\s+"([^"]*)"', s)
        if m and vendor and 0x10de in vendor:
            names[int(m.group(1), 16)] = m.group(2)
    cm = open(os.path.join(cfg, "ConfigManager.txt"), errors="replace").read().splitlines()
    rules = []
    for i, line in enumerate(cm):
        for pat in re.findall(r'match\("\$\{cardName\}",\s*"([^"]+)"\)', line):
            nxt = " ".join(x.strip() for x in cm[i + 1:i + 3] if x.strip().startswith(("seti", "setb", "boolProp")))
            rules.append((i + 1, pat, line.strip().split()[0], nxt))
    print("GPU profiles (SPORE_GPU_PROFILE or defaults write %s GPUProfile NAME):" % BUNDLE_ID)
    for key, dev, why in GPU_PROFILES:
        name = names.get(dev)
        print("  %-8s NVIDIA 0x%04x  VideoCards.txt: %s  (%s)" % (key, dev, '"%s"' % name if name else "ABSENT", why))
        if name:
            for ln, pat, kw, nxt in rules:
                if fnmatch.fnmatchcase(name, pat):
                    print("           ConfigManager.txt line %d: %s match %r -> %s" % (ln, kw, pat, nxt or "(no direct effect)"))
    print("Every other identity input is the same for all three: driver build 117741 (sporeProfile true), one GPU,\n"
          "768 MB texture memory, SM 3.0, 4 render targets, instancing through SetStreamSourceFreq.")


def main():
    ap = argparse.ArgumentParser(description=__doc__, formatter_class=argparse.RawDescriptionHelpFormatter)
    ap.add_argument("--binary", default=os.path.join(WORK, "build", "sporenative", "spore"), help="the linked game binary")
    ap.add_argument("--out", default=os.path.join(WORK, "build", "Spore.app"), help="the app to write (default build/Spore.app)")
    ap.add_argument("--sign", default="-", help="codesign identity (default: ad hoc)")
    ap.add_argument("--game-dir", default=GAME, help="the Spore folder: the icon source and the nothing-from-the-game check")
    ap.add_argument("--own-icon", action="store_true", help="use the icon drawn by icon.m instead of the game's")
    ap.add_argument("--verify-only", action="store_true", help="run the checks on an existing app, build nothing")
    ap.add_argument("--list-gpu-profiles", nargs="?", const=GAME, metavar="GAME_DIR",
                    help="print the GPU profiles and what each changes for that Spore folder, then exit")
    ap.add_argument("--after-link", action="store_true",
                    help="relink build/sporenative/spore with tools/link/build.py first (no game run), then build the app")
    ap.add_argument("--link-src", default=os.path.join(WORK, "build", "harness", "src"), help="--after-link: build.py --src")
    ap.add_argument("--jobs", type=int, default=10, help="--after-link: build.py --jobs")
    ap.add_argument("--force", action="store_true", help="relink or replace the app even while the game runs")
    a = ap.parse_args()
    if a.list_gpu_profiles:
        list_gpu_profiles(a.list_gpu_profiles)
        return 0
    if a.after_link and a.verify_only:
        ap.error("--after-link and --verify-only do not go together")
    if a.after_link:
        relink(a)
    app = os.path.realpath(a.out) if a.verify_only else build(a)
    fails = verify(app, a.game_dir)
    # the launcher's own --version (it exits before resolving a folder or starting anything)
    r = subprocess.run([os.path.join(app, "Contents", "MacOS", NAME), "--version"], capture_output=True, text=True,
                       timeout=30, env={k: v for k, v in os.environ.items() if not k.startswith("SPORE_")})
    print("bundle: %s: %s" % (app, r.stdout.strip() or "(--version failed: %s)" % r.stderr.strip()))
    print("result: %s" % ("all checks pass" if not fails else "%d check(s) FAILED" % len(fails)))
    return 1 if fails else 0


if __name__ == "__main__":
    sys.exit(main())
