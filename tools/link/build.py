#!/usr/bin/env python3
"""Build build/sporenative/spore: the whole translated image plus the runtime, one arm64 executable
(docs/design/image-linker.md).

    python3 -P tools/link/build.py                      # translated C from build/recomp/all, file-loading binary
    python3 -P tools/link/build.py --tus 512            # retranslate into build/sporenative/recomp-512 first
    python3 -P tools/link/build.py --embed              # also link spore-embed (image inside the binary)
    python3 -P tools/link/build.py --no-test            # skip the smoke run and the self-test

Steps (docs/design/image-linker.md 3):
  1. translated C: --src DIR (default build/recomp/all), or --tus N reruns recomp/translate.py --all with
     ceil(functions/N) functions per file into build/sporenative/recomp-N (cached on translator+exe hash)
  2. compile every TU with the runtime-abi 11 flags, -O2, N parallel jobs, incremental (flags+source stamp)
  3. compile the runtime: runtime/image, runtime/hle (src + gen), and whatever exists of runtime/core and
     runtime/win32 (every .c/.m/.cpp/.mm outside test/fixture directories). Missing pieces are covered
     by runtime/image's weak defaults
  4. tools/link/gen_link.py: dispatch table, buckets, resume points, manifest entry list, image blob
  5. link with clang (objects only, never archives, so strong runtime definitions replace weak ones)
  6. smoke run and dispatch self-test, each under a watchdog
--translate-only stops after step 1 (tools/convert/convert.py runs the translation as its own stage).

Inputs:  the exe, the manifest, data/eh/resume-points.json and data/threads/ordering-sites.json (step 1), the
         translated C (--src) and runtime/
Outputs: build/sporenative/spore (and spore-embed with --embed), its objects, and the --report file (default
         build/sporenative/link-report.json); nothing goes to data/
When:    the converter's translate stage (--tus 512 --translate-only) and link stage (--src <work>/build/harness/src);
         by hand after changing runtime/ (make_bundle.py --after-link relinks and rebuilds the app in one go).
Environment: SPORE_WORK_DIR, SPORE_GAME_DIR and SPORE_EXE move the outputs and name the exe (docs/design/convert.md 4).
"""
import argparse
import hashlib
import json
import math
import os
import shutil
import subprocess
import sys
import time

ROOT = os.path.abspath(os.path.join(os.path.dirname(__file__), "..", ".."))
WORK = os.environ.get("SPORE_WORK_DIR") or ROOT
OUT = os.path.join(WORK, "build", "sporenative")
EXE = os.environ.get("SPORE_EXE") or os.path.join(os.environ.get("SPORE_GAME_DIR") or os.path.join(ROOT, "game"),
                                                  "SporebinEP1", "SporeApp.exe")
CC = os.environ.get("CC", "clang")
GEN_FLAGS = ["-std=c11", "-fno-strict-aliasing", "-fwrapv", "-fno-math-errno", "-ffp-contract=off",
             "-Wno-pedantic", "-w"]  # runtime-abi 11
RT_FLAGS = ["-O2", "-g", "-DSG_CORE_OVERRIDE_HLE_STUBS=1", "-DW32_OVERRIDE_HLE_STUBS=1", "-Wall", "-Wextra", "-Wno-unused-parameter", "-Wno-unused-function"]
PLATFORM_FW = ["-framework", "AppKit", "-framework", "QuartzCore", "-framework", "Metal", "-framework", "CoreAudio",
               "-framework", "AudioToolbox"]
PLATFORM_FW += ["-framework", "GameController", "-framework", "CoreHaptics"]   # game controllers, rumble
# the Metal back end (runtime/d3d9metal) needs its own headers and the null device's private ones; it is
# compiled with these extra flags only (package-local include directories, no ARC).
METAL_FLAGS = ["-fno-objc-arc", "-Wno-missing-field-initializers"] + \
    ["-I" + os.path.join(ROOT, "runtime", *p) for p in (("d3d9metal", "include"), ("d3d9metal", "pipeline"),
                                                         ("d3d9metal", "shader"), ("d3d9null", "src"))]
METAL_LINK = ["-framework", "Metal", "-framework", "QuartzCore", "-framework", "AppKit", "-framework", "Foundation"]
SKIP_DIRS = {"tests", "test", "fixtures", "fixture", "bench", "tools", "examples", "gen-tests"}


def log(*a):
    print("[link]", *a, flush=True)


def run_pool(jobs, cmds):
    """Runs (key, argv, cwd) commands with at most `jobs` at once. Returns {key: (rc, cpu_s, maxrss_mb, wall_s, stderr)}."""
    res, running, queue = {}, {}, list(cmds)
    while queue or running:
        while queue and len(running) < jobs:
            key, argv = queue.pop(0)
            ef = open(key + ".err", "w+b")
            p = subprocess.Popen(argv, stdout=subprocess.DEVNULL, stderr=ef, cwd=ROOT)
            running[p.pid] = (key, p, time.time(), ef)
        pid, status, ru = os.wait4(-1, 0)
        if pid not in running:
            continue
        key, p, t0, ef = running.pop(pid)
        ef.seek(0)
        err = ef.read().decode(errors="replace")
        ef.close()
        p.returncode = os.waitstatus_to_exitcode(status)
        res[key] = (p.returncode, ru.ru_utime + ru.ru_stime, ru.ru_maxrss / 2**20, time.time() - t0, err)
    return res


def sha_files(paths):
    h = hashlib.sha256()
    for p in sorted(paths):
        h.update(p.encode())
        h.update(open(p, "rb").read())
    return h.hexdigest()


def translate(tus, jobs_note):
    """Regenerates the whole program into build/sporenative/recomp-<tus> unless the cache stamp matches."""
    out = os.path.join(OUT, "recomp-%d" % tus)
    srcs = [os.path.join(dp, f) for dp, _d, fs in os.walk(os.path.join(ROOT, "recomp"))
            for f in fs if f.endswith((".py", ".h")) and "fixtures" not in dp]
    # the memory-ordering site list changes the emitted C (recomp/sgrecomp/memorder.py)
    srcs.append(os.path.join(WORK, "data", "threads", "ordering-sites.json"))
    stamp = "%s %s %d" % (sha_files(srcs), os.path.getsize(EXE), tus)
    sp = os.path.join(out, ".stamp")
    if os.path.exists(sp) and open(sp).read() == stamp:
        log("translation cached:", os.path.relpath(out, ROOT))
        return out, None
    nfunc = 93827  # refined after the run from the stats; only used to size chunks
    chunk = math.ceil(nfunc / tus)
    if os.path.isdir(out):
        shutil.rmtree(out)
    log("translating the whole program, %d functions per file -> %s" % (chunk, os.path.relpath(out, ROOT)))
    t0 = time.time()
    env = dict(os.environ, PYTHONPATH=os.path.join(ROOT, "tools", ".deps"))
    r = subprocess.run([sys.executable, "-P", os.path.join(ROOT, "recomp", "translate.py"), "--all", "--chunk", str(chunk),
                        "--name", "tu", "--out", out, "--stats", os.path.join(OUT, "translate-stats-%d.json" % tus)],
                       cwd=ROOT, env=env)
    if r.returncode:
        raise SystemExit("translator failed")
    open(sp, "w").write(stamp)
    return out, round(time.time() - t0, 1)


def runtime_sources():
    """Runtime C/ObjC sources to link, by package. Test and fixture directories are skipped."""
    pk = {}
    pk["image"] = sorted(os.path.join(ROOT, "runtime", "image", "src", f)
                         for f in os.listdir(os.path.join(ROOT, "runtime", "image", "src")) if f.endswith(".c"))
    hle = os.path.join(ROOT, "runtime", "hle")
    pk["hle"] = sorted(os.path.join(hle, "src", f) for f in os.listdir(os.path.join(hle, "src")) if f.endswith(".c"))
    pk["hle"].append(os.path.join(hle, "gen", "hle_imports.c"))
    for name in ("core", "win32"):
        d = os.path.join(ROOT, "runtime", name)
        files = []
        if os.path.isdir(d):
            for dp, dns, fs in os.walk(d):
                dns[:] = [x for x in dns if x not in SKIP_DIRS and not x.startswith(".")]
                for f in fs:
                    if f.endswith((".c", ".m", ".cpp", ".mm")) and not f.startswith(("test_", "bench_")):
                        files.append(os.path.join(dp, f))
        pk[name] = sorted(files)
    # guest COM machinery, the null Direct3D 9 and DirectSound (src/ only; tests are host-only).
    for name in ("com", "d3d9null", "dsound"):
        d = os.path.join(ROOT, "runtime", name, "src")
        pk[name] = sorted(os.path.join(d, f) for f in os.listdir(d) if f.endswith(".c")) if os.path.isdir(d) else []
    # the Metal back end, selected at run time by SPORE_D3D9=metal (src/, pipeline/ and shader/; manual
    # retain/release, so its Objective-C compiles with -fno-objc-arc; see METAL_FLAGS).
    md = os.path.join(ROOT, "runtime", "d3d9metal")
    if os.path.isdir(os.path.join(md, "src")):
        pk["d3d9metal"] = sorted(os.path.join(md, sub, f) for sub in ("src", "pipeline", "shader")
                                 if os.path.isdir(os.path.join(md, sub))
                                 for f in os.listdir(os.path.join(md, sub)) if f.endswith((".c", ".m", ".mm")))
    # the guest USER32/GDI32/IMM32/XInput/DInput8 imports and the macOS host, plus the DirectSound buffer over
    # CoreAudio, whose strong sp_* replace dsound.c's weak stand-ins.
    pg = os.path.join(ROOT, "runtime", "platform", "guest", "src")
    pm = os.path.join(ROOT, "runtime", "platform", "macos", "src")
    if os.path.isdir(pg):
        pk["platform"] = sorted(os.path.join(pg, f) for f in os.listdir(pg) if f.endswith((".c", ".m"))) + \
            [os.path.join(pm, f) for f in ("sp_queue.c", "sp_input.c", "sp_keymap.c", "sp_window.m",
                                           "sp_dsound.c", "sp_coreaudio.c", "sp_clipboard.m", "sp_open.m")]
        # game controllers (the four XInput users and the GameController back end)
        pk["platform"] += [os.path.join(pm, f) for f in ("sp_gamepad_core.c", "sp_gamepad.m")]
        # drag and drop of files onto the game view (the NSDraggingDestination category on SPView; the guest
        # side, sgp_dragdrop.c, has weak defaults of its four sp_mac_dnd_* entries, so the link works without it)
        if os.path.exists(os.path.join(pm, "sp_dragdrop.m")):
            pk["platform"].append(os.path.join(pm, "sp_dragdrop.m"))
    return pk


def include_dirs():
    dirs = [os.path.join(ROOT, "runtime", "include"), os.path.join(ROOT, "runtime", "image", "include"),
            os.path.join(ROOT, "runtime", "hle", "include"), os.path.join(ROOT, "runtime", "hle", "gen")]
    for name in ("core", "win32"):
        d = os.path.join(ROOT, "runtime", name)
        for sub in ("include", "src", ""):
            p = os.path.join(d, sub)
            if os.path.isdir(p) and any(f.endswith(".h") for f in os.listdir(p)):
                dirs.append(p)
    # sg_com.h, d3d9null.h, and the sp_dsound.h API that runtime/dsound calls.
    for sub in (("com", "include"), ("d3d9null", "include"), ("platform", "macos", "include"),
                ("platform", "guest", "include"), ("platform", "guest", "src"), ("platform", "macos", "src")):
        p = os.path.join(ROOT, "runtime", *sub)
        if os.path.isdir(p):
            dirs.append(p)
    return dirs


def has_main(path):
    try:
        s = open(path, errors="replace").read()
    except OSError:
        return False
    return "int main(" in s or "int main (" in s


def header_stamp(flags):
    """Newest mtime and count of the headers in the -I directories (a header change recompiles the set)."""
    newest, n = 0, 0
    for f in flags:
        if f.startswith("-I") and os.path.isdir(f[2:]):
            for h in os.listdir(f[2:]):
                if h.endswith(".h"):
                    newest = max(newest, int(os.path.getmtime(os.path.join(f[2:], h))))
                    n += 1
    return "%d/%d" % (newest, n)


def compile_set(cmds_in, objdir, flags, jobs, label):
    """Incremental compile: skips objects whose stamp (flags, headers, source mtime/size) is unchanged."""
    os.makedirs(objdir, exist_ok=True)
    hs = header_stamp(flags)
    todo, objs = [], []
    for src in cmds_in:
        base = os.path.relpath(src, ROOT).replace("/", "__")
        obj = os.path.join(objdir, os.path.splitext(base)[0] + ".o")
        st = os.stat(src)
        stamp = "%s|%s|%d|%d" % (" ".join(flags), hs, st.st_size, int(st.st_mtime))
        objs.append(obj)
        sp = obj + ".stamp"
        if os.path.exists(obj) and os.path.exists(sp) and open(sp).read() == stamp:
            continue
        lang = []
        if src.endswith((".cpp", ".mm")):
            argv = ["clang++", "-std=c++17"] + [f for f in flags if not f.startswith("-std=")]
        else:
            argv = [CC] + flags + (["-fobjc-arc"] if src.endswith(".m") and "-fno-objc-arc" not in flags else [])
        todo.append((obj, argv + lang + ["-c", src, "-o", obj]))
        open(sp + ".pending", "w").write(stamp)
    t0 = time.time()
    res = run_pool(jobs, todo) if todo else {}
    wall = time.time() - t0
    bad = {k: v for k, v in res.items() if v[0]}
    for k, v in res.items():
        if not v[0]:
            os.replace(k + ".stamp.pending", k + ".stamp")
    if bad:
        for k, v in list(bad.items())[:5]:
            sys.stderr.write("compile failed: %s\n%s\n" % (k, v[4][-3000:]))
        raise SystemExit("%s: %d of %d compiles failed" % (label, len(bad), len(todo)))
    st = {"compiled": len(todo), "objects": len(objs), "wall_s": round(wall, 1),
          "cpu_s": round(sum(v[1] for v in res.values()), 1),
          "max_tu_cpu_s": round(max([v[1] for v in res.values()] or [0]), 1),
          "peak_rss_mb": round(max([v[2] for v in res.values()] or [0])),
          "object_mb": round(sum(os.path.getsize(o) for o in objs) / 2**20, 1)}
    log("%s: %s" % (label, st))
    return objs, st


def text_size(path):
    out = subprocess.run(["size", "-m", path], capture_output=True, text=True).stdout
    for line in out.splitlines():
        line = line.strip()
        if line.startswith("Section __text:"):
            return int(line.split(":")[1])
    return None


def segments(path):
    out = subprocess.run(["size", "-m", path], capture_output=True, text=True).stdout
    seg = {}
    for line in out.splitlines():
        if line.startswith("Segment "):
            name, val = line[len("Segment "):].split(":")
            seg[name.strip()] = int(val.split()[0])
    return seg


def link(objs, out, extra):
    argv = [CC, "-o", out] + objs + extra + ["-Wl,-stack_size,0x4000000", "-lm"]
    rsp = out + ".rsp"
    open(rsp, "w").write("\n".join(argv[3:]))
    t0 = time.time()
    with open(out + ".link.err", "w+b") as ef:
        p = subprocess.Popen([CC, "-o", out, "@" + rsp], cwd=ROOT, stderr=ef)
        _pid, status, ru = os.wait4(p.pid, 0)
        ef.seek(0)
        err = ef.read().decode(errors="replace")
    wall = time.time() - t0
    if os.waitstatus_to_exitcode(status):
        sys.stderr.write(err[-3000:])
        raise SystemExit("link failed: %s: %s" % (out, " | ".join(err.splitlines()[:6])))
    st = {"wall_s": round(wall, 2), "cpu_s": round(ru.ru_utime + ru.ru_stime, 2), "rss_mb": round(ru.ru_maxrss / 2**20),
          "binary_bytes": os.path.getsize(out), "text_bytes": text_size(out), "segments": segments(out),
          "warnings": [l for l in err.splitlines() if "warning" in l][:20]}
    log("linked %s: %s" % (os.path.relpath(out, ROOT), {k: v for k, v in st.items() if k != "segments"}))
    return st


def run_watchdog(argv, seconds):
    t0 = time.time()
    try:
        p = subprocess.run(argv, capture_output=True, text=True, timeout=seconds, cwd=ROOT)
        rc, out, err = p.returncode, p.stdout, p.stderr
    except subprocess.TimeoutExpired as e:
        rc, out, err = "timeout", (e.stdout or b"").decode() if isinstance(e.stdout, bytes) else (e.stdout or ""), "timeout"
    return {"argv": [os.path.relpath(a, ROOT) if a.startswith(ROOT) else a for a in argv], "rc": rc,
            "wall_s": round(time.time() - t0, 3), "stdout": out[-4000:], "stderr": err[-4000:]}


def weak_report(rt_objs):
    """Which weak defaults of runtime/image are replaced by a strong definition elsewhere in the link."""
    weak, strong = set(), set()
    for o in rt_objs:
        out = subprocess.run(["nm", "-m", "-U", o], capture_output=True, text=True).stdout
        for line in out.splitlines():
            if "external" not in line or not any(k in line for k in ("(__TEXT,__text)", "(__DATA", "(__TEXT,__const)", "thread", "(common)")):
                continue
            sym = line.split()[-1]
            if "weak external" in line and "/runtime/image/" in o.replace("__", "/"):
                weak.add(sym)
            elif "weak" not in line:
                strong.add(sym)
    return sorted(s.lstrip("_") for s in weak - strong), sorted(s.lstrip("_") for s in weak & strong)


def main():
    ap = argparse.ArgumentParser(description=__doc__, formatter_class=argparse.RawDescriptionHelpFormatter)
    ap.add_argument("--src", default=os.path.join(WORK, "build", "recomp", "all"),
                    help="folder of translated C to link (default build/recomp/all)")
    ap.add_argument("--tus", type=int, default=0, help="retranslate into this many TUs first (0: use --src)")
    ap.add_argument("--translate-only", action="store_true", help="with --tus: translate (or find it cached), then stop")
    ap.add_argument("--jobs", type=int, default=8, help="parallel compiles (default 8)")
    ap.add_argument("--opt", default="-O2", help="optimisation flag for the translated C (default -O2)")
    ap.add_argument("--embed", action="store_true", help="also link spore-embed with the image inside")
    ap.add_argument("--no-test", action="store_true", help="skip the smoke run and the dispatch self-test")
    ap.add_argument("--strict", action="store_true", help="fail instead of dropping a broken runtime/core or runtime/win32")
    ap.add_argument("--without", default="", help="comma-separated optional packages to leave out (core,win32)")
    ap.add_argument("--report", default=os.path.join(OUT, "link-report.json"), help="where to write the link report")
    a = ap.parse_args()
    os.makedirs(OUT, exist_ok=True)
    rep = {"date": time.strftime("%Y-%m-%d %H:%M"), "opt": a.opt, "jobs": a.jobs,
           "cc": subprocess.run([CC, "--version"], capture_output=True, text=True).stdout.splitlines()[0],
           "load_avg_start": [round(x, 1) for x in os.getloadavg()]}
    t_all = time.time()

    src = a.src
    if a.tus:
        src, rep["translate_s"] = translate(a.tus, a.jobs)
    if a.translate_only:
        if not a.tus:
            raise SystemExit("--translate-only needs --tus N")
        log("translation in %s; --translate-only, stopping" % src)
        return 0
    tus = sorted(os.path.join(src, f) for f in os.listdir(src) if f.endswith(".c"))
    if not tus:
        raise SystemExit("no translated C in %s (run recomp/translate.py --all --chunk 500 --name all --out build/recomp/all)" % src)
    rep["src"] = os.path.relpath(src, ROOT)
    rep["tus"] = len(tus)
    rep["c_mb"] = round(sum(os.path.getsize(t) for t in tus) / 2**20, 1)
    log("%d translation units, %.0f MB of C, from %s" % (len(tus), rep["c_mb"], rep["src"]))

    inc = ["-I" + os.path.join(ROOT, "runtime", "include"), "-I" + os.path.join(ROOT, "recomp", "include")]
    tag = os.path.basename(src.rstrip("/")) + a.opt.replace("-", "")
    gen_objs, rep["compile_game"] = compile_set(tus, os.path.join(OUT, "obj", tag), GEN_FLAGS + [a.opt] + inc, a.jobs,
                                                "translated code")

    pk = runtime_sources()
    for n in filter(None, a.without.split(",")):
        pk[n] = []
        rep.setdefault("packages_excluded", []).append(n)
    for name in ("core", "win32", "platform"):
        if name not in pk:
            continue
        drop = [f for f in pk[name] if has_main(f)]
        pk[name] = [f for f in pk[name] if f not in drop]
        rep["%s_skipped_main" % name] = [os.path.relpath(f, ROOT) for f in drop]
    rep["runtime_sources"] = {k: [os.path.relpath(f, ROOT) for f in v] for k, v in pk.items()}
    rt_flags = ["-std=c11"] + RT_FLAGS + ["-I" + d for d in include_dirs()] + ['-DSG_REPO_ROOT="%s"' % ROOT]
    rt_by_pkg, rep["compile_runtime"], rep["packages_dropped"] = {}, {}, {}
    for name, files in pk.items():
        if not files:
            continue
        try:
            rt_by_pkg[name], rep["compile_runtime"][name] = compile_set(files, os.path.join(OUT, "obj", "runtime"),
                                                                        rt_flags + (METAL_FLAGS if name == "d3d9metal" else []),
                                                                        a.jobs, "runtime/" + name)
        except SystemExit as e:
            if name in ("image", "hle") or a.strict:
                raise
            # A package another agent is still writing: link without it (runtime/image's weak defaults).
            rep["packages_dropped"][name] = "compile: %s" % e
            log("dropping runtime/%s: %s" % (name, e))
    rt_objs = [o for v in rt_by_pkg.values() for o in v]

    gdir = os.path.join(OUT, "gen")
    t0 = time.time()
    g = subprocess.run([sys.executable, "-P", os.path.join(ROOT, "tools", "link", "gen_link.py"), "--src", src, "--out", gdir]
                       + ([] if a.embed else ["--no-image"]), cwd=ROOT, capture_output=True, text=True)
    if g.returncode:
        sys.stderr.write(g.stderr)
        raise SystemExit("gen_link failed")
    rep["gen"] = json.load(open(os.path.join(gdir, "gen-report.json")))
    rep["gen_s"] = round(time.time() - t0, 1)
    gen_src = [os.path.join(gdir, "dispatch.c"), os.path.join(gdir, "image_none.c")]
    link_objs, rep["compile_link_support"] = compile_set(gen_src, os.path.join(OUT, "obj", "gen"),
                                                         ["-std=c11", "-O1", "-I" + os.path.join(ROOT, "runtime", "include")],
                                                         a.jobs, "link support")
    asm = [os.path.join(gdir, "buckets.s")] + ([os.path.join(gdir, "image.s")] if a.embed else [])
    asm_objs = []
    for s in asm:
        o = os.path.join(OUT, "obj", "gen", os.path.basename(s)[:-2] + ".s.o")
        subprocess.run([CC, "-c", s, "-o", o], check=True)
        asm_objs.append(o)
    dispatch_o, none_o = link_objs
    bucket_o = asm_objs[0]

    out_file = os.path.join(OUT, "spore")
    rep["link"] = {}
    optional = [n for n in ("core", "win32", "com", "d3d9null", "dsound", "platform", "d3d9metal") if n in rt_by_pkg]
    tries = [optional] + ([[n for n in optional if n != d] for d in reversed(optional)] + [[]] if optional and not a.strict else [])
    for keep in tries:
        rt_objs = [o for n, v in rt_by_pkg.items() if n in ("image", "hle") or n in keep for o in v]
        common = gen_objs + rt_objs + [dispatch_o, bucket_o]
        try:
            rep["link"]["spore"] = link(common + [none_o], out_file, (PLATFORM_FW if "platform" in keep else []) +
                                       (METAL_LINK if "d3d9metal" in keep else []))
            break
        except SystemExit as e:
            if keep == tries[-1]:
                raise
            log("link failed with %s; retrying with fewer packages" % (keep or "no optional packages"))
            rep.setdefault("link_failures", []).append({"packages": keep, "error": str(e)})
    for n in optional:
        if n not in keep:
            rep["packages_dropped"][n] = "link (duplicate or missing symbols; see link_failures)"
    rep["packages_linked"] = ["image", "hle"] + keep
    if a.embed:
        rep["link"]["spore-embed"] = link(common + [asm_objs[1]], os.path.join(OUT, "spore-embed"),
                                          ["-Wl,-segprot,__SPIMG,r,r"] + (PLATFORM_FW if "platform" in keep else []))
    rep["weak_defaults_in_use"], rep["weak_defaults_replaced"] = weak_report(rt_objs)

    if not a.no_test:
        entries = os.path.join(gdir, "entries.txt")
        tests = {"smoke": [out_file, "--smoke"], "smoke_noverify": [out_file, "--smoke", "--no-verify"],
                 "selftest": [out_file, "--selftest", entries]}
        if a.embed:
            emb = os.path.join(OUT, "spore-embed")
            tests.update({"embed_mmap": [emb, "--smoke", "--source", "embed-mmap"],
                          "embed_copy": [emb, "--smoke", "--source", "embed"],
                          "embed_file": [emb, "--smoke", "--source", "file"],
                          "embed_selftest": [emb, "--selftest", entries]})
        rep["tests"] = {}
        for k, argv in tests.items():
            r = run_watchdog(argv, 120)
            rep["tests"][k] = r
            log("%s: rc=%s %.3fs %s" % (k, r["rc"], r["wall_s"], (r["stdout"].strip().splitlines() or [""])[-1]))
            if r["rc"] != 0:
                sys.stderr.write(r["stderr"][-2000:])
    rep["total_s"] = round(time.time() - t_all, 1)
    rep["load_avg_end"] = [round(x, 1) for x in os.getloadavg()]
    json.dump(rep, open(a.report, "w"), indent=1)
    failed = [k for k, t in rep.get("tests", {}).items() if t["rc"] != 0]
    log("done in %.0f s; report %s%s" % (rep["total_s"], os.path.relpath(a.report, ROOT),
                                         ("; FAILED: " + ", ".join(failed)) if failed else ""))
    return 1 if failed else 0


if __name__ == "__main__":
    sys.exit(main())
