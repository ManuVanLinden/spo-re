#!/usr/bin/env python3
"""Build the native app from your prepared Steam Spore Galactic Adventures folder.

    python3 -P tools/convert/convert.py <prepared-steam-folder> [-o DIR]

Writes Spore Native (Steam).app into the current folder, or into DIR with -o/--output. The user's guide is
README.md; the design (stages, the SPORE_WORK_DIR convention, resumption, the
files the converter needs) is docs/design/convert.md.

Input: the folder holding SporebinEP1/SporeApp.exe, Data/ and DataEP1/ (or its SporebinEP1 folder, or a folder with
exactly one such install inside). The EP1 exe must match the prepared Steam image pinned by SG_IMAGE_SHA256 in
runtime/image/include/sg_image.h. The original Steam executable requires separate preparation.

Stages (each prints one line with its duration; the tools' own output goes to <work>/log/<stage>.log):
  extract    the installer only: innoextract into --extract-to
  manifest   tools/manifest/build.py --tag ep1                      the function and control-flow manifest
  eh         tools/eh/ehdb.py, tools/eh/verify.py                   exception resume points
  imports    tools/imports/{parse_imports,callsites,dynres,build_table}.py   the import table scan.py reads
  threads    tools/threads/{scan,threads}.py, recomp/memorder_sites.py        memory-ordering sites
  translate  tools/link/build.py --tus 512 --translate-only         every function to C (the longest stage with link)
  harness    tools/harness/prep_src.py                              the WinMain boot hook
  link       tools/link/build.py --src <work>/build/harness/src     compile, link, loader smoke run and self-test
  bundle     tools/bundle/make_bundle.py                             the app, signed and checked
  settings   <support>/game-dir                                     so the first launch does not ask

All generated files go under the work folder (--work, default
~/Library/Caches/com.spore-native.converter/<first 12 hex digits of the exe's SHA-256>/), passed to the tools as
SPORE_WORK_DIR; the Spore folder and exe go as SPORE_GAME_DIR and SPORE_EXE. A rerun resumes: a stage is skipped
when its stamp (<work>/stamps/<stage>.json) is newer than its inputs, its outputs exist and its command is the
same. --force reruns every stage; --clean deletes the work folder first. --stage NAME runs one stage, --until
NAME stops after one (the test drives the pipeline stage by stage with them). Never starts the game: the link
stage's smoke run maps the image and exits before any game code, and the bundle stage only asks the launcher
for --version. Translated code stays in the work folder and inside the app, on this Mac.
"""
import argparse
import hashlib
import json
import os
import platform
import re
import shutil
import subprocess
import sys
import time

ROOT = os.path.realpath(os.path.join(os.path.dirname(os.path.abspath(__file__)), "..", ".."))
HERE = os.path.join(ROOT, "tools", "convert")
DEPS = os.path.join(ROOT, "tools", ".deps")
BREWFILE = os.path.join(HERE, "Brewfile")
BASE_SHA256 = "f9716458cdc232a507686dcb490e75680c514e31a5b1f33601b0dfcd05baf75d"   # README.md "Target"
APP_BUNDLE = "Spore Native (Steam).app"
SUPPORT_DIR = "~/Library/Application Support/spore-native-steam"
TUS = 512
PIP_LINE = "python3 -m pip install --target tools/.deps capstone==5.0.3"
STAGES = ["extract", "manifest", "eh", "imports", "threads", "translate", "harness", "link", "bundle",
          "settings"]
DESC = {
    "extract": "extract the GOG installer (innoextract)",
    "manifest": "manifest: every function of the exe",
    "eh": "exception resume points",
    "imports": "import table",
    "threads": "threads and memory-ordering sites",
    "translate": "translate the exe into C (512 files)",
    "harness": "boot harness",
    "link": "compile and link",
    "bundle": "build and check " + APP_BUNDLE,
    "settings": "remember the Spore folder for the first launch",
}


class Refuse(Exception):
    pass


def sha256(path):
    h = hashlib.sha256()
    with open(path, "rb") as f:
        for b in iter(lambda: f.read(1 << 20), b""):
            h.update(b)
    return h.hexdigest()


def exe_sha256_expected():
    h = open(os.path.join(ROOT, "runtime", "image", "include", "sg_image.h")).read()
    return re.search(r'#define SG_IMAGE_SHA256 "([0-9a-f]{64})"', h).group(1)


def version():
    sys.path.insert(0, os.path.join(ROOT, "tools", "bundle"))
    import make_bundle
    return make_bundle.git_version()[0]


# ---- input --------------------------------------------------------------------------------------------------

def layout_problem(d):
    """None when d looks like a Galactic Adventures install (hash not checked yet), else the reason."""
    if not os.path.isfile(os.path.join(d, "SporebinEP1", "SporeApp.exe")):
        return "no SporebinEP1/SporeApp.exe inside"
    if not os.path.isdir(os.path.join(d, "Data")):
        return "no Data folder inside"
    if not os.path.isdir(os.path.join(d, "DataEP1")):
        return "no DataEP1 folder inside (Galactic Adventures is required)"
    return None


def find_install(path):
    """The Spore folder for what the user named: the folder itself, its parent when it is the SporebinEP1
    folder, or the one install directly inside it (as the app's launcher accepts a picked folder)."""
    path = os.path.realpath(os.path.expanduser(path))
    if not os.path.isdir(path):
        raise Refuse("%s is not a folder" % path)
    why = layout_problem(path)
    if not why:
        return path
    if os.path.basename(path).lower() == "sporebinep1" and not layout_problem(os.path.dirname(path)):
        return os.path.dirname(path)
    inside = sorted(os.path.join(path, e) for e in os.listdir(path)
                    if not e.startswith(".") and os.path.isdir(os.path.join(path, e)) and not layout_problem(os.path.join(path, e)))
    if len(inside) == 1:
        return inside[0]
    if len(inside) > 1:
        raise Refuse("%s holds %d Spore installs (%s); name the one to convert" % (
            path, len(inside), ", ".join(os.path.basename(i) for i in inside)))
    raise Refuse("%s: %s. Give the prepared Steam Spore Galactic Adventures folder "
                 "(the one holding SporebinEP1, Data and DataEP1)" % (path, why))


def installer_in(path):
    """The GOG installer the user named: the .exe itself, or the one setup_spore*.exe in a folder."""
    path = os.path.abspath(os.path.expanduser(path))
    if os.path.isfile(path):
        if re.match(r"setup_spore.*\.exe$", os.path.basename(path), re.I):
            return path
        raise Refuse("%s is not a GOG Spore installer (setup_spore*.exe) or a Spore folder" % path)
    if os.path.isdir(path) and layout_problem(path):
        hits = sorted(e for e in os.listdir(path) if re.match(r"setup_spore.*\.exe$", e, re.I))
        if len(hits) == 1:
            return os.path.join(path, hits[0])
        if len(hits) > 1:
            raise Refuse("%s holds %d installers (%s); name the one to use" % (path, len(hits), ", ".join(hits)))
    return None


def check_exes(game):
    exe = os.path.join(game, "SporebinEP1", "SporeApp.exe")
    want = exe_sha256_expected()
    got = sha256(exe)
    if got != want:
        raise Refuse("%s does not match the prepared Steam image for Galactic Adventures "
                     "(SHA-256 %s, expected %s). See README.md for the required input." % (exe, got, want))
    base = os.path.join(game, "SporeBin", "SporeApp.exe")
    if os.path.isfile(base):
        got = sha256(base)
        if got != BASE_SHA256:
            raise Refuse("%s does not match the expected Steam base game (SHA-256 %s, expected %s)." %
                         (base, got, BASE_SHA256))
        return exe, want, "base game exe checked"
    return exe, want, "warning: no SporeBin/SporeApp.exe (base game) in %s; only the Galactic Adventures exe was checked" % game


# ---- prerequisites ------------------------------------------------------------------------------------------

def brewfile_packages(path):
    """[(name, comment)] of the brew lines of a Brewfile."""
    out = []
    for line in open(path):
        m = re.match(r'\s*brew\s+"([^"]+)"', line)
        if m:
            out.append(m.group(1))
    return out


def check_prereqs(a, need_innoextract):
    """Prints what is missing and returns False when the run cannot go on."""
    ok = True
    problems = []
    arm = subprocess.run(["sysctl", "-n", "hw.optional.arm64"], capture_output=True, text=True, timeout=30).stdout.strip()
    if platform.system() != "Darwin" or arm != "1":
        problems.append("this needs a Mac with Apple Silicon (M1 or later)")
    r = subprocess.run(["xcode-select", "-p"], capture_output=True, text=True, timeout=30)
    clang = shutil.which("clang")
    if r.returncode or not clang or subprocess.run(["clang", "--version"], capture_output=True, timeout=60).returncode:
        problems.append("the Command Line Tools are missing: run  xcode-select --install  and try again")
    if sys.version_info < (3, 11):
        problems.append("Python 3.11 or newer is needed (this is %s): brew install python3" % platform.python_version())
    for p in problems:
        print("  missing: " + p)
    if problems:
        return False
    # Homebrew packages: <prefix>/opt/<name> exists for every installed formula (aliases included)
    need = set()
    if need_innoextract:
        need.add("innoextract")
    listed = brewfile_packages(BREWFILE)
    missing = [n for n in listed if n in need and not os.path.exists(os.path.join(a.brew_prefix, "opt", n))]
    if missing:
        print("  missing Homebrew package%s: %s" % ("s" if len(missing) > 1 else "", ", ".join(missing)))
        if not os.path.exists(os.path.join(a.brew_prefix, "bin", "brew")):
            print("  Homebrew itself is not installed: see https://brew.sh")
        print("  install them with:  brew bundle --file %s" % os.path.relpath(BREWFILE))
        ok = False
    # the Python package the tools import (capstone, a disassembler), local to the checkout
    env = dict(os.environ, PYTHONPATH=DEPS)
    r = subprocess.run([sys.executable, "-P", "-c", "import capstone"], env=env, capture_output=True, timeout=60)
    if r.returncode:
        if a.no_pip:
            print("  missing Python package capstone; install it with:  " + PIP_LINE)
            ok = False
        elif ok:
            print("  installing the Python package capstone into tools/.deps:  " + PIP_LINE)
            r = subprocess.run([sys.executable, "-m", "pip", "install", "--target", DEPS, "capstone==5.0.3"],
                               cwd=ROOT, timeout=600)
            if r.returncode or subprocess.run([sys.executable, "-P", "-c", "import capstone"], env=env,
                                              capture_output=True, timeout=60).returncode:
                print("  the install failed; run it yourself:  " + PIP_LINE)
                ok = False
    return ok


# ---- stages -------------------------------------------------------------------------------------------------

def newest(paths):
    """Newest mtime over files and (walked) folders; Python caches are ignored."""
    t = 0.0
    for p in paths:
        if os.path.isdir(p):
            for d, dns, fs in os.walk(p):
                dns[:] = [x for x in dns if x != "__pycache__" and not x.startswith(".")]
                for f in fs:
                    try:
                        t = max(t, os.stat(os.path.join(d, f)).st_mtime)
                    except OSError:
                        pass
        elif os.path.exists(p):
            t = max(t, os.stat(p).st_mtime)
    return t


class Pipeline:
    def __init__(self, a, game, exe, installer):
        self.a, self.game, self.exe, self.installer = a, game, exe, installer
        self.work = a.work
        self.app = os.path.join(a.output, APP_BUNDLE)
        W = lambda *p: os.path.join(self.work, *p)  # noqa: E731
        R = lambda *p: os.path.join(ROOT, *p)  # noqa: E731
        self.W = W
        py = [sys.executable, "-P"]
        imp = W("scratch", "imports")
        jobs = ["--jobs", str(a.jobs)]
        bundle = py + [R("tools", "bundle", "make_bundle.py"), "--binary", W("build", "sporenative", "spore"),
                       "--out", self.app, "--game-dir", game or ""]
        stamp = lambda s: W("stamps", s + ".json")  # noqa: E731
        # name: (commands, inputs, outputs). A command is an argv list, or (argv, stdout file), or a Python callable.
        self.stages = {
            "extract": ([(["innoextract", "--progress=0", "-d", a.extract_to, "-I", "SporeBin", "-I", "SporebinEP1",
                           "-I", "Data", "-I", "DataEP1", "-I", "bp1content", installer or ""], None)],
                        [installer or ""], [os.path.join(a.extract_to, "SporebinEP1", "SporeApp.exe")]),
            "manifest": ([py + [R("tools", "manifest", "build.py"), "--tag", "ep1", "--exe", exe or ""]],
                         [exe or "", R("tools", "manifest"), R("tools", "code-characteristics", "pe.py")],
                         [W("scratch", "manifest", "ep1", "manifest.txt.gz"),
                          W("scratch", "manifest", "ep1", "functions-canonical.u32")]),
            "eh": ([py + [R("tools", "eh", "ehdb.py")], self.eh_verify],
                   [stamp("manifest"), R("tools", "eh")],
                   [W("data", "eh", "resume-points.json"), W("data", "eh", "funcinfo-summary.json")]),
            "imports": ([(py + [R("tools", "imports", "parse_imports.py"), exe or ""], os.path.join(imp, "imp.json")),
                         (py + [R("tools", "imports", "callsites.py"), exe or "", os.path.join(imp, "imp.json")],
                          os.path.join(imp, "cs.json")),
                         (py + [R("tools", "imports", "dynres.py"), exe or "", os.path.join(imp, "cs.json")],
                          os.path.join(imp, "dyn.json")),
                         py + [R("tools", "imports", "build_table.py"), os.path.join(imp, "imp.json"),
                               os.path.join(imp, "cs.json"), os.path.join(imp, "dyn.json"), W("data", "imports.json"),
                               "--hand-sections", R("data", "imports-hand.json")]],
                        [exe or "", R("tools", "imports"), R("data", "imports-hand.json")],
                        [W("data", "imports.json")]),
            "threads": ([py + [R("tools", "threads", "scan.py")], py + [R("tools", "threads", "threads.py")],
                         py + [R("recomp", "memorder_sites.py")]],
                        [stamp("manifest"), stamp("imports"), R("tools", "threads"), R("recomp", "memorder_sites.py"),
                         R("recomp", "sgrecomp"), R("data", "libraries", "text-regions.json")],
                        [W("data", "threads", "ordering-sites.json")]),
            "translate": ([py + [R("tools", "link", "build.py"), "--tus", str(TUS), "--translate-only"] + jobs],
                          [stamp("manifest"), stamp("eh"), stamp("threads"), R("recomp", "translate.py"),
                           R("recomp", "sgrecomp"), R("recomp", "include"), R("tools", "insn-forms", "forms.py"),
                           R("tools", "link", "build.py")],
                          [W("build", "sporenative", "recomp-%d" % TUS, ".stamp")]),
            "harness": ([py + [R("tools", "harness", "prep_src.py")]],
                        [stamp("translate"), R("tools", "harness", "prep_src.py")],
                        [W("build", "harness", "src")]),
            "link": ([py + [R("tools", "link", "build.py"), "--src", W("build", "harness", "src")] + jobs],
                     [stamp("harness"), R("runtime"), R("recomp", "include"), R("tools", "link")],
                     [W("build", "sporenative", "spore")]),
            "bundle": ([bundle],
                       [stamp("link"), R("tools", "bundle", "make_bundle.py"), R("tools", "bundle", "launcher.m"),
                        R("tools", "bundle", "exe_icon.py"), R("tools", "bundle", "icon.m")],
                       [os.path.join(self.app, "Contents", "MacOS", "spore-game")]),
            "settings": ([self.write_game_dir], [stamp("bundle")], [self.game_dir_file()]),
        }
        # the command that, when it changes, makes a stage rerun (jobs and the log are left out)
        self.keys = {s: self.key(s) for s in self.stages}
        self.keys["settings"] = "game-dir %s -> %s" % (game, self.game_dir_file())

    def key(self, s):
        parts = []
        for c in self.stages[s][0]:
            if callable(c):
                parts.append(c.__name__)
                continue
            argv, out = (c if isinstance(c, tuple) else (c, None))
            argv = [x for i, x in enumerate(argv) if x != "--jobs" and not (i and argv[i - 1] == "--jobs")]
            parts.append(" ".join(argv[1:]) + (" > " + out if out else ""))
        return " && ".join(parts)

    def game_dir_file(self):
        sup = os.environ.get("SPORE_SUPPORT_DIR") or os.path.expanduser(SUPPORT_DIR)
        return os.path.join(sup, "game-dir")

    def env(self):
        e = {k: v for k, v in os.environ.items() if k not in ("SG_MEMORDER", "SPORE_D3D9")}
        e["PYTHONPATH"] = DEPS + (os.pathsep + e["PYTHONPATH"] if e.get("PYTHONPATH") else "")
        e["SPORE_WORK_DIR"] = self.work
        if self.game:
            e["SPORE_GAME_DIR"] = self.game
            e["SPORE_EXE"] = self.exe
        return e

    # ---- the in-process steps
    def eh_verify(self, log):
        """tools/eh/verify.py; the stage succeeds when it wrote both of its files.

        verify.py reports the 8 known landing-owner differences of its V3 check as FAIL, which is expected for this
        exe. It exits 0 today; a non-zero exit is tolerated as long as both output files were written."""
        outs = [self.W("data", "eh", "resume-points.json"), self.W("data", "eh", "funcinfo-summary.json")]
        for o in outs:
            if os.path.exists(o):
                os.unlink(o)
        rc = self.run_cmd([sys.executable, "-P", os.path.join(ROOT, "tools", "eh", "verify.py")], None, log)
        if rc:
            log.write("verify.py exited %d; checking its outputs\n" % rc)
        return 0 if all(os.path.exists(o) for o in outs) else (rc or 1)

    def write_game_dir(self, log):
        """Remember the selected game folder for the launcher."""
        f = self.game_dir_file()
        os.makedirs(os.path.dirname(f), exist_ok=True)
        with open(f + ".tmp", "w") as o:
            o.write(os.path.realpath(self.game) + "\n")
        os.replace(f + ".tmp", f)
        log.write("wrote %s: %s\n" % (f, os.path.realpath(self.game)))
        return 0

    # ---- running
    def run_cmd(self, argv, out, log):
        log.write("== %s%s\n" % (" ".join(argv), (" > " + out) if out else ""))
        log.flush()
        t0 = time.time()
        if out:
            os.makedirs(os.path.dirname(out), exist_ok=True)
        fo = open(out, "w") if out else log
        try:
            p = subprocess.Popen(argv, stdout=fo, stderr=log, cwd=ROOT, env=self.env(), start_new_session=True)
            try:
                rc = p.wait(timeout=self.a.timeout)
            except subprocess.TimeoutExpired:
                os.killpg(p.pid, 9)
                p.wait()
                log.write("timed out after %d s\n" % self.a.timeout)
                rc = 124
            except KeyboardInterrupt:
                os.killpg(p.pid, 9)
                p.wait()
                raise
        finally:
            if out:
                fo.close()
        log.write("== exit %d after %.0f s\n" % (rc, time.time() - t0))
        log.flush()
        return rc

    def up_to_date(self, s):
        _cmds, inputs, outputs = self.stages[s]
        sp = os.path.join(self.work, "stamps", s + ".json")
        if not os.path.exists(sp) or not all(os.path.exists(o) for o in outputs):
            return False
        try:
            st = json.load(open(sp))
        except ValueError:
            return False
        if s == "settings":  # the player can change the folder in the app since: compare the file itself
            try:
                if open(self.game_dir_file()).read().strip() != os.path.realpath(self.game):
                    return False
            except OSError:
                return False
        return st.get("key") == self.keys[s] and os.stat(sp).st_mtime >= newest(inputs)

    def run_stage(self, s, idx, total):
        label = "%2d/%d  %-9s %s" % (idx, total, s, DESC[s])
        if not self.a.force and self.up_to_date(s):
            print("%-62s skipped (up to date)" % label, flush=True)
            return
        cmds, _inputs, outputs = self.stages[s]
        sp = os.path.join(self.work, "stamps", s + ".json")
        if os.path.exists(sp):
            os.unlink(sp)
        os.makedirs(os.path.join(self.work, "log"), exist_ok=True)
        os.makedirs(os.path.join(self.work, "stamps"), exist_ok=True)
        lp = os.path.join(self.work, "log", s + ".log")
        t0 = time.time()
        if sys.stdout.isatty():
            print("%-62s ..." % label, end="\r", flush=True)
        with open(lp, "w") as log:
            for c in cmds:
                if callable(c):
                    rc = c(log)
                else:
                    argv, out = (c if isinstance(c, tuple) else (c, None))
                    rc = self.run_cmd(argv, out, log)
                if rc:
                    break
        missing = [o for o in outputs if not os.path.exists(o)] if not rc else []
        if rc or missing:
            print("%-62s FAILED" % label)
            print("stage %s failed (exit %s%s). Log: %s; its last 25 lines:" % (
                s, rc, ("; missing " + ", ".join(missing)) if missing else "", lp))
            lines = open(lp, errors="replace").read().splitlines()
            for line in lines[-25:]:
                print("  | " + line)
            raise SystemExit(1)
        with open(sp, "w") as f:
            json.dump({"key": self.keys[s], "seconds": round(time.time() - t0, 1),
                       "finished": time.strftime("%Y-%m-%d %H:%M:%S")}, f, indent=1)
        print("%-62s %5d s" % (label, time.time() - t0), flush=True)

    def dry_run(self, todo):
        print("work folder: %s" % self.work)
        print("environment: SPORE_WORK_DIR=%s SPORE_GAME_DIR=%s SPORE_EXE=%s PYTHONPATH=%s" % (
            self.work, self.game, self.exe, DEPS))
        for i, s in enumerate(todo, 1):
            print("%2d. %-9s %s" % (i, s, DESC[s]))
            for c in self.stages[s][0]:
                if callable(c):
                    print("      (in the converter) %s" % (c.__doc__ or c.__name__).strip().splitlines()[0])
                    continue
                argv, out = (c if isinstance(c, tuple) else (c, None))
                print("      " + " ".join(_q(x) for x in argv) + ((" > " + _q(out)) if out else ""))


def _q(s):
    return s if re.match(r"^[\w./@%+=:,-]+$", s) else "'" + s.replace("'", "'\\''") + "'"


def default_jobs():
    for k in ("hw.perflevel0.physicalcpu", "hw.physicalcpu"):
        r = subprocess.run(["sysctl", "-n", k], capture_output=True, text=True, timeout=30)
        if r.returncode == 0 and r.stdout.strip().isdigit():
            return int(r.stdout.strip())
    return os.cpu_count() or 4


def free_gb(path):
    p = path
    while not os.path.exists(p):
        p = os.path.dirname(p)
    st = os.statvfs(p)
    return st.f_bavail * st.f_frsize / 2**30


def main():
    ap = argparse.ArgumentParser(description="Build a native Mac app from your prepared Steam Spore folder.",
                                 epilog="Guide: README.md")
    ap.add_argument("input", nargs="?", help="the prepared Steam folder (holding SporebinEP1, Data, DataEP1)")
    ap.add_argument("-o", "--output", default=".",
                    help="folder to write " + APP_BUNDLE + " into (default: the current folder)")
    ap.add_argument("--work", help="work folder (default ~/Library/Caches/com.spore-native.converter/<exe hash>)")
    ap.add_argument("--extract-to", default=os.path.expanduser("~/Library/Application Support/spore-native/game"),
                    help="installer input: where the game is extracted (the app reads it from there)")
    ap.add_argument("--no-pip", action="store_true", help="never install the Python package; say what is missing")
    ap.add_argument("--clean", action="store_true", help="delete the work folder first")
    ap.add_argument("--force", action="store_true", help="rerun every stage")
    ap.add_argument("--dry-run", action="store_true", help="print the stages and their commands, run nothing")
    ap.add_argument("--stage", choices=STAGES, help="run this one stage")
    ap.add_argument("--until", choices=STAGES, help="stop after this stage")
    ap.add_argument("--jobs", type=int, default=0, help="parallel compiles (default: the performance cores)")
    ap.add_argument("--timeout", type=int, default=1800, help="seconds any one tool may run (default 1800)")
    ap.add_argument("--brew-prefix", default="/opt/homebrew", help=argparse.SUPPRESS)
    ap.add_argument("--version", action="store_true", help="print the version and exit")
    a = ap.parse_args()
    if a.version:
        print("spore-native converter %s" % version())
        return 0
    if not a.input:
        ap.error("name your prepared Steam Spore folder")
    if a.stage and a.until:
        ap.error("--stage and --until do not go together")
    a.jobs = a.jobs or default_jobs()
    a.output = os.path.abspath(os.path.expanduser(a.output))
    a.extract_to = os.path.abspath(os.path.expanduser(a.extract_to))

    try:
        installer = installer_in(a.input)
        game = exe = None
        if installer:
            if not os.path.isfile(os.path.join(a.extract_to, "SporebinEP1", "SporeApp.exe")) or a.force:
                game = a.extract_to          # checked after the extract stage
            else:
                game = find_install(a.extract_to)
        else:
            game = find_install(a.input)
        if not installer or os.path.isfile(os.path.join(game, "SporebinEP1", "SporeApp.exe")):
            exe, digest, note = check_exes(game)
        else:
            exe, digest, note = os.path.join(game, "SporebinEP1", "SporeApp.exe"), exe_sha256_expected(), \
                "the exe is checked after extraction"
    except Refuse as e:
        print("convert: " + str(e), file=sys.stderr)
        return 1
    a.work = os.path.abspath(os.path.expanduser(a.work or os.path.join(
        "~/Library/Caches/com.spore-native.converter", digest[:12])))
    if os.path.realpath(os.path.join(a.output, APP_BUNDLE)) == os.path.realpath(os.path.join(ROOT, "build", "Spore.app")) \
            and not a.force:
        print("convert: %s is the development bundle; write elsewhere with -o, or pass --force" %
              os.path.join(a.output, APP_BUNDLE), file=sys.stderr)
        return 1

    todo = [s for s in STAGES if s != "extract" or installer]
    if a.stage:
        if a.stage not in todo:
            print("convert: stage %s does not apply to this run (%s)" % (a.stage, ", ".join(todo)), file=sys.stderr)
            return 2
        todo = [a.stage]
    elif a.until:
        if a.until not in todo:
            print("convert: stage %s does not apply to this run (%s)" % (a.until, ", ".join(todo)), file=sys.stderr)
            return 2
        todo = todo[:todo.index(a.until) + 1]

    pipe = Pipeline(a, game, exe, installer)
    print("Spore Galactic Adventures (local Steam conversion): %s (%s)" % (game, note))
    if installer:
        print("installer: %s, extracted to %s" % (installer, a.extract_to))
    print("app: %s" % pipe.app)
    if a.dry_run:
        pipe.dry_run(todo)
        return 0
    print("checking prerequisites ...", flush=True)
    if not check_prereqs(a, bool(installer)):
        print("convert: install what is missing above, then run the same command again", file=sys.stderr)
        return 1
    if a.clean and os.path.isdir(a.work):
        shutil.rmtree(a.work)
    os.makedirs(a.work, exist_ok=True)
    if free_gb(a.work) < 3 and not os.path.exists(os.path.join(a.work, "stamps", "translate.json")):
        print("convert: %s has %.1f GB free; the conversion needs about 3 GB (the work folder can go elsewhere "
              "with --work)" % (a.work, free_gb(a.work)), file=sys.stderr)
        return 1
    if installer and "extract" in todo and free_gb(a.extract_to) < 7 and \
            not os.path.isfile(os.path.join(a.extract_to, "SporebinEP1", "SporeApp.exe")):
        print("convert: %s has %.1f GB free; the extracted game needs about 6 GB (choose another place with "
              "--extract-to)" % (a.extract_to, free_gb(a.extract_to)), file=sys.stderr)
        return 1
    print("work folder: %s" % a.work)

    t_all = time.time()
    order = [s for s in STAGES if s != "extract" or installer]
    for s in todo:
        i = order.index(s)
        before = [p for p in order[:i] if p not in ("extract", "settings")]
        if s != "extract" and not a.dry_run:
            gone = [p for p in before if p not in todo and not os.path.exists(os.path.join(a.work, "stamps", p + ".json"))]
            if gone:
                print("convert: stage %s needs %s first (--until %s)" % (s, ", ".join(gone), gone[-1]), file=sys.stderr)
                return 1
        pipe.run_stage(s, i + 1, len(order))
        if s == "extract":
            try:
                game = find_install(a.extract_to)
                exe, digest, note = check_exes(game)
            except Refuse as e:
                print("convert: after extraction: " + str(e), file=sys.stderr)
                return 1
            pipe = Pipeline(a, game, exe, installer)
    print("%-62s %5d s" % ("done", time.time() - t_all))
    if "bundle" in todo:
        print("Your app: %s" % pipe.app)
    if "settings" in todo:
        print("Spore folder: %s (remembered in %s)" % (game, pipe.game_dir_file()))
        print("Open %s to play." % APP_BUNDLE)
    return 0


if __name__ == "__main__":
    try:
        sys.exit(main())
    except KeyboardInterrupt:
        print("\nconvert: interrupted; run the same command again to continue where it stopped", file=sys.stderr)
        sys.exit(130)
