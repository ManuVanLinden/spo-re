"""Translator driver: translate functions of SporeApp.exe (GOG EP1) to C against
runtime/include/spore_guest.h. Static only: the exe is read, never run; output C is only compiled.

Usage:   PYTHONPATH=tools/.deps python3 -P recomp/translate.py --range 11e0f00-11e2000 --name crt --check
         PYTHONPATH=tools/.deps python3 -P recomp/translate.py --all --chunk N --out DIR   (the whole program)
Inputs:  the exe, the function manifest (scratch/manifest/ep1, from tools/manifest/build.py: entries, switch
         tables, no-return calls), the exception-handling resume points (data/eh/resume-points.json: landing
         owners), the memory-ordering site list (data/threads/ordering-sites.json) and the PE import table
Outputs: build/recomp/<name>.c (git-ignored; translated game code); optionally statistics (--stats) and a
         per-form lowering coverage (--coverage, which needs an instruction-form census at
         data/manifest/insn-forms.json that the converter does not build)
When:    tools/link/build.py --tus N runs it for the converter's translate stage; by hand for a slice.
Environment: SPORE_WORK_DIR, SPORE_GAME_DIR and SPORE_EXE move the outputs and name the exe (docs/design/convert.md 4).
"""
import argparse
import collections
import json
import os
import subprocess
import sys
import time

HERE = os.path.dirname(os.path.abspath(__file__))
ROOT = os.path.abspath(os.path.join(HERE, ".."))
sys.path.insert(0, HERE)
sys.path.insert(0, os.path.join(ROOT, "tools", "manifest"))
sys.path.insert(0, os.path.join(ROOT, "tools", "insn-forms"))

from capstone import x86_const as X  # noqa: E402

import manifest as manifest_loader  # noqa: E402
from forms import form_of  # noqa: E402
from sgrecomp import cfg  # noqa: E402
from sgrecomp.emit import Emitter  # noqa: E402
from sgrecomp.image import PEImage  # noqa: E402
from sgrecomp import memorder  # noqa: E402

# SPORE_WORK_DIR (default: the source tree) takes the generated outputs; SPORE_GAME_DIR / SPORE_EXE name the
# Spore folder / the EP1 exe instead of game/ (docs/design/convert.md 4).
WORK = os.environ.get("SPORE_WORK_DIR") or ROOT
EXE = os.environ.get("SPORE_EXE") or os.path.join(os.environ.get("SPORE_GAME_DIR") or os.path.join(ROOT, "game"),
                                                  "SporebinEP1", "SporeApp.exe")
NORET_IMPORTS = {"_CxxThrowException", "longjmp", "exit", "_exit", "abort", "ExitProcess", "_terminate__YAXXZ",
                 "_amsg_exit", "ExitThread", "_endthreadex"}
ABI_FLAGS = ["-std=c11", "-fno-strict-aliasing", "-fwrapv", "-fno-math-errno", "-ffp-contract=off", "-Wno-pedantic"]


def load_relocs(img):
    """Bitmap of base-relocation sites over the image (for the instruction-form census's imm/addr distinction)."""
    import struct
    lo, hi = img.base, img.base + 0x2000000
    bm = bytearray((hi - lo) // 8 + 1)
    rva, size = img.dirs[5]
    va = img.base + rva
    end = va + size
    while va < end:
        page, bsz = struct.unpack("<II", img.read(va, 8))
        if bsz == 0:
            break
        ents = img.read(va + 8, bsz - 8)
        for i in range(0, len(ents), 2):
            e = ents[i] | ents[i + 1] << 8
            if e >> 12 == 3:
                o = img.base + page + (e & 0xFFF) - lo
                bm[o >> 3] |= 1 << (o & 7)
        va += bsz

    def has(a, n):
        for x in range(a - lo, a - lo + n):
            if bm[x >> 3] >> (x & 7) & 1:
                return True
        return False
    return has


def write_chunk(a, em, funcs, k):
    """Write one translation unit with declarations for what it references; streams to disk."""
    os.makedirs(a.out, exist_ok=True)
    p = os.path.join(a.out, "%s%s.c" % (a.name, "" if k is None else "_%04d" % k))
    head = ["/* Translated from SporeApp.exe (GOG EP1) by recomp/translate.py. Generated; git-ignored. */",
            '#include "sg_recomp.h"', ""] + em.prologue() + [""]
    with open(p, "w") as f:
        f.write("\n".join(head))
        f.write("\n".join(funcs))
    em.refs_f.clear()
    em.refs_imp.clear()
    return p


def main():
    ap = argparse.ArgumentParser(description=__doc__, formatter_class=argparse.RawDescriptionHelpFormatter)
    ap.add_argument("--exe", default=EXE, help="the exe (default: the EP1 exe)")
    ap.add_argument("--tag", default="ep1", help="the manifest to read, scratch/manifest/<tag> (default ep1)")
    ap.add_argument("--range", action="append", default=[], help="hex lo-hi of function entries (repeatable)")
    ap.add_argument("--functions", default="", help="comma-separated hex entries")
    ap.add_argument("--all", action="store_true", help="every function of the manifest")
    ap.add_argument("--name", default="slice", help="output file name stem (default slice)")
    ap.add_argument("--out", default=os.path.join(WORK, "build", "recomp"), help="output folder (default build/recomp)")
    ap.add_argument("--chunk", type=int, default=0, help="functions per .c file (0 = one file)")
    ap.add_argument("--no-c", action="store_true", help="translate but do not write C (coverage runs)")
    ap.add_argument("--check", action="store_true", help="clang -fsyntax-only on the output")
    ap.add_argument("--coverage", default="", help="write per-form lowering coverage JSON here")
    ap.add_argument("--stats", default="", help="write slice statistics JSON here")
    ap.add_argument("--memorder", default="", help="memory-ordering policy p0|p1|p2|p3 (default: $SG_MEMORDER, "
                    "else data/threads/ordering-sites.json default_policy); docs/design/memory-ordering.md")
    ap.add_argument("--sites", default=memorder.SITES, help="ordering site list (data/threads/ordering-sites.json)")
    a = ap.parse_args()
    t0 = time.time()

    img = PEImage(a.exe)
    m = manifest_loader.load(a.tag)
    entries = set(m.functions)
    tables = {jv: list(t.targets) for jv, t in m.tables.items()}
    # no-return calls: the manifest's call-noret blocks, plus calls to known no-return imports and their thunks
    noret = set()
    for f in m.functions.values():
        for b in f.blocks:
            if b.term == "call-noret":
                last = None
                for ins in cfg.disasm().disasm(img.read(b.start, b.end - b.start), b.start):
                    last = ins
                if last is not None:
                    noret.add(last.address)
    noret_targets = set()
    for iat, sym in img.imports.items():
        if sym.split("_", 3)[-1] in NORET_IMPORTS or any(sym.endswith("_" + n) for n in NORET_IMPORTS):
            noret_targets.add(("iat", iat))
    for e in entries:
        i = cfg.decode(img, e)
        if i and i.mnemonic == "jmp" and i.operands[0].type == X.X86_OP_MEM and \
                ("iat", i.operands[0].mem.disp & 0xFFFFFFFF) in noret_targets:
            noret_targets.add(e)
    # landings: landing owners from data/eh/resume-points.json, not the manifest's labels
    rp = json.load(open(os.path.join(WORK, "data", "eh", "resume-points.json")))
    landing = collections.defaultdict(list)
    for r in rp["resume_points"]:
        landing[int(r["landing_owner"][0], 16)].append(int(r["resume"], 16))

    # manifest labels that are not exception-handling resume points are code addresses taken as values (0x864780
    # after a no-return longjmp; 0x11f8df0 and 0xc2e640, which the exception-handling scan classes as false catch
    # continuations). They are reached only through their address, so they become functions with their own dispatch
    # entries.
    rset = {r for rs in landing.values() for r in rs}
    promoted = sorted(va for va in m.labels if va not in rset)
    entries |= set(promoted)
    # a setjmp return belongs to the function that contains the `call _setjmp3` before it; after the
    # promotion above that can differ from the function the exception-handling scan recorded (0x86483c: the call
    # is in 0x864780, not 0x864760)
    owner_fixes = []
    srt = sorted(entries)
    import bisect
    for r in rp["resume_points"]:
        if r["kind"] != "setjmp-return":
            continue
        rv = int(r["resume"], 16)
        old_owner = int(r["landing_owner"][0], 16)
        cand = srt[bisect.bisect_right(srt, rv) - 1]
        if cand == old_owner:
            continue
        fn = cfg.build(img, cand, entries=entries, tables=tables, noret=noret, noret_targets=noret_targets)
        if any(i and i.address + i.size == rv and i.mnemonic == "call" for b in fn.blocks for i in b.insns):
            landing[old_owner].remove(rv)
            if not landing[old_owner]:
                del landing[old_owner]
            landing[cand].append(rv)
            owner_fixes.append(("%#x" % rv, "%#x" % old_owner, "%#x" % cand))
    if a.all:
        sel = sorted(entries)
    else:
        sel = set()
        for r in a.range:
            lo, hi = (int(x, 16) for x in r.split("-"))
            sel |= {e for e in entries if lo <= e < hi}
        if a.functions:
            sel |= {int(x, 16) for x in a.functions.split(",")}
        sel = sorted(sel)
    has_reloc = load_relocs(img)

    mo_data = memorder.load_sites(a.sites)
    mo = memorder.MemOrder(img, memorder.resolve_policy(a.memorder, mo_data), mo_data)
    em = Emitter(img, entries, landing=dict(landing), noret_targets=noret_targets, memorder=mo)
    form_total = collections.Counter()
    form_unhandled = collections.Counter()
    cfg_mismatch = []
    cfg_errors = []
    paths = []
    cur = []
    nfun = 0
    for e in sel:
        try:
            fn = cfg.build(img, e, roots=landing.get(e, ()), entries=entries, tables=tables, noret=noret,
                           noret_targets=noret_targets)
        except cfg.CFGError as ex:
            cfg_errors.append(str(ex))
            continue
        # cross-check: blocks reached by our descent vs the manifest
        mf = m.functions.get(e)
        if mf is not None:
            ours = {b.start for b in fn.blocks}
            theirs = {b.start for b in mf.blocks}
            if ours != theirs:
                cfg_mismatch.append((e, len(ours - theirs), len(theirs - ours)))
        n_before = len(em.unhandled)
        text = em.function(fn)
        bad = {va for va, _f, _t, _r in em.unhandled[n_before:]}
        for b in fn.blocks:
            for ins in b.insns:
                if ins is None:
                    continue
                reloc = ins.size >= 5 and "0x" in ins.op_str and has_reloc(ins.address + 1, ins.size - 4)
                fm = form_of(ins.mnemonic, ins.op_str, reloc)[0]
                form_total[fm] += 1
                if ins.address in bad:
                    form_unhandled[fm] += 1
        nfun += 1
        if not a.no_c:
            cur.append(text)
            if a.chunk and len(cur) >= a.chunk:
                paths.append(write_chunk(a, em, cur, len(paths)))
                cur = []
    if cur and not a.no_c:
        paths.append(write_chunk(a, em, cur, None if not paths and not a.chunk else len(paths)))
    t_tr = time.time() - t0
    try:
        mo.verify_sites(set(sel))
    except memorder.SiteError as ex:
        raise SystemExit(str(ex))
    stats = {"functions": nfun, "instructions": sum(form_total.values()),
             "unhandled_instructions": sum(form_unhandled.values()),
             "unhandled_by_reason": collections.Counter(r for _v, _f, _t, r in em.unhandled).most_common(40),
             "cfg_mismatch_functions": len(cfg_mismatch), "cfg_mismatch_examples": [
                 ["%#x" % e, more, fewer] for e, more, fewer in cfg_mismatch[:30]],
             "cfg_errors": cfg_errors[:30], "promoted_labels": ["%#x" % v for v in promoted], "landing_owner_fixes": owner_fixes, "landing_functions": len([e for e in sel if e in landing]),
             "c_files": [os.path.relpath(p, ROOT) for p in paths], "translate_seconds": round(t_tr, 1),
             "memorder": mo.summary()}
    if a.check and paths:
        tc = time.time()
        res = []
        for p in paths:
            r = subprocess.run(["clang", "-fsyntax-only", "-Wall", "-Wno-unused-label", "-Wno-unused-variable",
                                "-Wno-unused-but-set-variable"] + ABI_FLAGS +
                               ["-I", os.path.join(ROOT, "runtime", "include"), "-I", os.path.join(HERE, "include"), p],
                               capture_output=True, text=True)
            nerr = r.stderr.count(" error:")
            nwarn = r.stderr.count(" warning:")
            res.append({"file": os.path.relpath(p, ROOT), "rc": r.returncode, "errors": nerr, "warnings": nwarn})
            if r.returncode or nwarn:
                print(r.stderr[:3000])
        stats["syntax_check"] = res
        stats["syntax_check_seconds"] = round(time.time() - tc, 1)
        stats["bytes_of_c"] = sum(os.path.getsize(p) for p in paths)
    if a.coverage:
        census = json.load(open(os.path.join(ROOT, "data", "manifest", "insn-forms.json")))["forms"]
        reach = {f[0]: f[2] for f in census if f[2] > 0}
        lowered = sorted(f for f in reach if form_total.get(f, 0) and not form_unhandled.get(f, 0))
        partial = sorted(f for f in reach if form_unhandled.get(f, 0))
        unseen = sorted(f for f in reach if not form_total.get(f, 0))
        extra = sorted(f for f in form_total if f not in reach)
        cov = {"about": "Lowering coverage over translated functions, by instruction form (tools/insn-forms). "
                        "'lowered' = every instance translated without sg_unhandled_insn. Derived counts only.",
               "scope": "all manifest functions" if a.all else a.range,
               "forms_census": len(reach), "forms_seen": len(reach) - len(unseen), "forms_lowered": len(lowered),
               "forms_with_unhandled": {f: [form_unhandled[f], form_total[f]] for f in partial},
               "forms_not_in_scope": len(unseen), "forms_outside_census": {f: form_total[f] for f in extra},
               "instructions": sum(form_total.values()), "instructions_unhandled": sum(form_unhandled.values()),
               "per_form": {f: form_total[f] for f in sorted(form_total)}}
        os.makedirs(os.path.dirname(os.path.abspath(a.coverage)), exist_ok=True)
        json.dump(cov, open(a.coverage, "w"), indent=1)
        stats["coverage_file"] = a.coverage
    if a.stats:
        os.makedirs(os.path.dirname(os.path.abspath(a.stats)), exist_ok=True)
        json.dump(stats, open(a.stats, "w"), indent=1)
    stats["seconds"] = round(time.time() - t0, 1)
    print(json.dumps(stats, indent=1, default=str)[:4000])


if __name__ == "__main__":
    main()
