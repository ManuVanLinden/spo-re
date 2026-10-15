"""Generate data/threads/ordering-sites.json: the memory-ordering site list the translator reads
(docs/design/memory-ordering.md). Static only: reads SporeApp.exe with Capstone.

  PYTHONPATH=tools/.deps python3 -P recomp/memorder_sites.py            # writes the JSON
  PYTHONPATH=tools/.deps python3 -P recomp/memorder_sites.py --check    # compare, write nothing

Every site is an instruction address with the function that owns it (by recomp's CFG), the kind
(acquire = the load must be an acquire load, release = the store must be a release store,
acq_rel = both), what it is, why, and the rule it comes from (docs/design/memory-ordering.md section 4 has
the same table with the counts). The sets are found from rules written below, not typed in by hand, so a reader
can check each rule against the exe and rerun it:

  spinlock      plain loads of the ticket/state words [esi], [esi+4], [esi+8] in the
                ticket RW spinlock's read-lock 0x6abf40 and write-lock 0x6abfc0
  dac           every access, image-wide, to the Dac thread's plain state globals
                (requested/current state, run flag, status bytes, counters)
  stop-flag     the plain stop-flag loads in thread loops: audio update this+0x60,
                CallbackProcessor this+0x24, RenderWare worker this+5
  publication   32-bit stores of a register into heap memory in functions reachable
                from more than one thread by direct calls (data/threads/threads.json
                closures): candidate pointer publications into shared objects

The base-game (SporeBin) twins of the spinlock sites (0x20 lower) are verified against that
exe's bytes and recorded for reference; the translator only reads the EP1 sites.

Usage:   PYTHONPATH=tools/.deps python3 -P recomp/memorder_sites.py [--check]   (--check: compare, write nothing)
Inputs:  the exe (and the base game's, for the twins), the function manifest, scratch/threads/ep1/ and
         data/threads/*.json (from tools/threads/)
Outputs: data/threads/ordering-sites.json, read by the translator (recomp/sgrecomp/memorder.py)
When:    the converter's threads stage, last; by hand after changing a rule here.
Environment: SPORE_WORK_DIR, SPORE_GAME_DIR and SPORE_EXE move the outputs and name the exe (docs/design/convert.md 4).
"""
import argparse
import collections
import glob
import hashlib
import json
import os
import re
import sys

HERE = os.path.dirname(os.path.abspath(__file__))
ROOT = os.path.abspath(os.path.join(HERE, ".."))
sys.path.insert(0, HERE)
sys.path.insert(0, os.path.join(ROOT, "tools", "manifest"))

from capstone import x86_const as X  # noqa: E402

import manifest as manifest_loader  # noqa: E402
from sgrecomp import cfg  # noqa: E402
from sgrecomp.image import PEImage  # noqa: E402
from sgrecomp.memorder import MemOrder, SITES  # noqa: E402

# SPORE_WORK_DIR (default: the source tree) takes the generated outputs; SPORE_GAME_DIR / SPORE_EXE name the
# Spore folder / the EP1 exe instead of game/ (docs/design/convert.md 4).
WORK = os.environ.get("SPORE_WORK_DIR") or ROOT
GAME = os.environ.get("SPORE_GAME_DIR") or os.path.join(ROOT, "game")
EXE = os.environ.get("SPORE_EXE") or os.path.join(GAME, "SporebinEP1", "SporeApp.exe")
BASE_EXE = os.path.join(GAME, "SporeBin", "SporeApp.exe")
REACH = os.path.join(WORK, "scratch", "threads", "ep1")

SPINLOCK_FUNCS = {0x6abf40: "ticket RW spinlock read-lock", 0x6abfc0: "ticket RW spinlock write-lock"}
SPINLOCK_BASE_DELTA = -0x20  # SporeBin: 0x6abf20 / 0x6abfa0
DAC_GLOBALS = {0x16e6304: "Dac status byte", 0x16e6305: "Dac status byte", 0x16e6306: "Dac status byte",
               0x16e6307: "Dac run flag", 0x16e633c: "Dac status byte", 0x16e7b98: "Dac current state",
               0x16e7b9c: "Dac requested state", 0x16e7ba4: "Dac counter", 0x16e7ba8: "Dac counter"}
DAC_FUNCS = {0x113d780: "stop", 0x113d7e0: "pause", 0x113db50: "resume", 0x113dbe0: "restart",
             0x113e2e0: "resume", 0x113df70: "start", 0x113dc70: "Dac thread loop"}
STOP_FLAGS = [(0xa22810, 0x60, 1, "audio update thread stop byte this+0x60"),
              (0x92a490, 0x24, 1, "CallbackProcessor run byte this+0x24"),
              (0x11e6210, 0x5, 1, "RenderWare worker stop byte this+5")]
ABS_RE = re.compile(r"\[0x([0-9a-f]+)\]")


def h(v):
    return "%#x" % v


class Ctx:
    def __init__(self):
        self.img = PEImage(EXE)
        self.m = manifest_loader.load("ep1")
        self.entries = set(self.m.functions)
        self.tables = {jv: list(t.targets) for jv, t in self.m.tables.items()}
        self.mo = MemOrder(self.img, "p0", {})
        self._fn = {}

    def fn(self, e):
        if e not in self._fn:
            self._fn[e] = cfg.build(self.img, e, entries=self.entries, tables=self.tables)
        return self._fn[e]

    def insns(self, e):
        for b in self.fn(e).blocks:
            for i in b.insns:
                if i is not None:
                    yield i

    def access(self, insn):
        acc = 0
        for _op, a in MemOrder.mem_ops(insn):
            acc |= a
        return acc


def kind_of(acc):
    return {1: "acquire", 2: "release", 3: "acq_rel"}.get(acc)


def site(c, e, i, kind, group, what, reason, source):
    return {"addr": h(i.address), "func": h(e), "kind": kind, "group": group, "insn": "%s %s" % (i.mnemonic, i.op_str),
            "what": what, "reason": reason, "source": source}


def spinlock_sites(c):
    out = []
    for e, name in SPINLOCK_FUNCS.items():
        for i in c.insns(e):
            if i.mnemonic != "mov" or len(i.operands) != 2 or i.operands[1].type != X.X86_OP_MEM:
                continue
            m = i.operands[1].mem
            if m.base == X.X86_REG_ESI and not m.index and m.disp in (0, 4, 8):
                word = {0: "state word lock+0", 4: "next-ticket word lock+4", 8: "serving-ticket word lock+8"}[m.disp]
                out.append(site(c, e, i, "acquire", "spinlock", "%s: load of the %s" % (name, word),
                                "spin/poll load of a lock word another thread releases with lock xadd; must be an "
                                "acquire load and must not be hoisted out of the pause/ThreadSleep loop",
                                "memory-ordering.md 4 (spinlock); data/threads/poll-loops.json"))
    return out


def base_twins(c, sites):
    """Check the SporeBin twins (0x20 lower) decode to the same instruction text."""
    if not os.path.exists(BASE_EXE):
        return {"verified": False, "note": "SporeBin exe not present"}
    b = PEImage(BASE_EXE)
    rows, ok = [], True
    for s in sites:
        a = int(s["addr"], 16)
        t = a + SPINLOCK_BASE_DELTA
        ie = cfg.decode(c.img, a)
        ib = cfg.decode(b, t)
        same = ie is not None and ib is not None and (ie.mnemonic, ie.op_str) == (ib.mnemonic, ib.op_str)
        ok &= same
        rows.append({"ep1": s["addr"], "base": h(t), "same_instruction": same})
    return {"verified": ok, "functions": [h(e + SPINLOCK_BASE_DELTA) for e in SPINLOCK_FUNCS], "sites": rows,
            "note": "reference only: the translator targets EP1. The twins are the same instructions 0x20 lower."}


def dac_sites(c):
    """Every instruction image-wide whose operand is one of the Dac globals (linear decode of
    each manifest block, then the owner's CFG for the access direction)."""
    md = cfg.disasm()
    hits = []
    for e, f in c.m.functions.items():
        for b in f.blocks:
            code = c.img.read(b.start, b.end - b.start)
            for (va, _sz, mn, ops) in md.disasm_lite(code, b.start):
                for mm in ABS_RE.finditer(ops):
                    if int(mm.group(1), 16) in DAC_GLOBALS:
                        hits.append((e, va))
    out = []
    seen = set()
    for e, va in sorted(hits):
        if va in seen:
            continue
        ins = {i.address: i for i in c.insns(e)}
        i = ins.get(va)
        if i is None:
            continue  # block not in recomp's CFG of this manifest owner; handled by verify below
        seen.add(va)
        acc = c.access(i)
        g = [int(x, 16) for x in ABS_RE.findall(i.op_str) if int(x, 16) in DAC_GLOBALS][0]
        role = DAC_FUNCS.get(e, "other")
        out.append(site(c, e, i, kind_of(acc) or "acquire", "dac",
                        "%s %s (%s, function %s)" % (DAC_GLOBALS[g], h(g), "read" if acc == 1 else
                                                     "write" if acc == 2 else "read-modify-write", role),
                        "lock-free message passing between the main thread's pause/resume/stop functions and "
                        "the RWAudioCore Dac thread: stores must be releases so the mixer state written before "
                        "them is visible, loads acquires so the Dac reads that state after them",
                        "memory-ordering.md 4 (dac)"))
    missing = sorted({va for _e, va in hits} - seen)
    return out, missing


def stop_flag_sites(c):
    out = []
    for e, disp, size, what in STOP_FLAGS:
        n = 0
        for i in c.insns(e):
            for op, acc in MemOrder.mem_ops(i):
                m = op.mem
                if m.base and m.base not in (X.X86_REG_ESP,) and not m.index and m.disp == disp and op.size == size \
                        and acc == 1:
                    out.append(site(c, e, i, "acquire", "stop-flag", what,
                                    "plain stop-flag poll in a thread loop; an acquire load cannot be hoisted out "
                                    "of the loop and orders what the thread reads after seeing the flag",
                                    "memory-ordering.md 4 (stop-flag)"))
                    n += 1
        if not n:
            raise SystemExit("stop flag %s not found in %#x" % (what, e))
    return out


def reach_sets():
    sets = {}
    for p in sorted(glob.glob(os.path.join(REACH, "reach-*.txt"))):
        t = os.path.basename(p)[6:-4]
        sets[t] = {int(l.split()[0], 16) for l in open(p) if l.strip()}
    return sets


def publication_sites(c, multi):
    out = []
    for e in sorted(multi):
        if e not in c.entries:
            continue
        c.mo.begin_function(c.fn(e))
        for i in c.insns(e):
            if i.mnemonic != "mov" or len(i.operands) != 2:
                continue
            d, s = i.operands
            if d.type != X.X86_OP_MEM or d.size != 4 or s.type != X.X86_OP_REG:
                continue
            if c.mo.op_class(d) != "heap":
                continue
            out.append(site(c, e, i, "release", "publication", "32-bit register store into a heap object in a "
                            "function reachable from more than one thread",
                            "candidate publication of a pointer (or a value published with it) into an object "
                            "another thread reads; a release store keeps the object's earlier writes before it",
                            "memory-ordering.md 4 (publication); data/threads/threads.json direct closures"))
    return out


def main():
    ap = argparse.ArgumentParser(description=__doc__, formatter_class=argparse.RawDescriptionHelpFormatter)
    ap.add_argument("--out", default=SITES, help="the site list to write (default data/threads/ordering-sites.json)")
    ap.add_argument("--check", action="store_true", help="exit 1 if the list would change; write nothing")
    ap.add_argument("--default-policy", default=None, help="the default_policy recorded in the list, p0-p3 (default: the existing list's, else p2)")
    a = ap.parse_args()
    old = json.load(open(a.out)) if os.path.exists(a.out) else {}
    c = Ctx()
    sp = spinlock_sites(c)
    dac, dac_missing = dac_sites(c)
    st = stop_flag_sites(c)
    sets = reach_sets()
    if sets:
        cnt = collections.Counter(f for s in sets.values() for f in s)
        multi = sorted(f for f, n in cnt.items() if n >= 2)
        workers = sorted(set().union(*[s for t, s in sets.items() if t != "main"]))
    else:  # scratch/threads is git-ignored: fall back to the committed lists
        multi = [int(x, 16) for x in old.get("multi_thread_functions", [])]
        workers = [int(x, 16) for x in old.get("worker_functions", [])]
    pub = publication_sites(c, multi)
    sites, seen = [], set()
    for s in sp + dac + st + pub:  # first group wins for an address listed twice
        if s["addr"] in seen:
            continue
        seen.add(s["addr"])
        sites.append(s)
    sites.sort(key=lambda s: int(s["addr"], 16))
    groups = collections.Counter(s["group"] for s in sites)
    doc = {
        "about": "Memory-ordering site list for recomp/translate.py (docs/design/memory-ordering.md). "
                 "Generated by recomp/memorder_sites.py from SporeApp.exe (static Capstone reads); addresses and "
                 "instruction text only. Every policy except p0 lowers these sites as ordered accesses; the "
                 "translator fails if a listed site of a translated function is not found as listed.",
        "exe": "SporebinEP1/SporeApp.exe",
        "exe_sha256": hashlib.sha256(open(EXE, "rb").read()).hexdigest(),
        "default_policy": a.default_policy or old.get("default_policy", "p2"),
        "policies": {
            "p0": "lock/xchg only (sequentially consistent); every plain access relaxed (the translator's emission before ordered accesses existed)",
            "p1": "p0 + the sites below",
            "p2": "p1 + every .data/.tls/CONST global access image-wide + every heap access in worker_functions",
            "p3": "p1 + every global and heap access (x86-TSO for all non-stack data)",
        },
        "site_counts": dict(sorted(groups.items())),
        "dac_hits_outside_recomp_cfg": [h(v) for v in dac_missing],
        "base_game_twins": base_twins(c, sp),
        "worker_functions_note": "union of the direct-call closures of the 9 worker thread kinds "
                                 "(tools/threads/threads.py THREADS); p2 orders every heap access in them",
        "worker_functions": [h(x) for x in workers],
        "multi_thread_functions_note": "functions in the direct closures of two or more thread kinds (main counts "
                                       "as one); the publication group is drawn from them",
        "multi_thread_functions": [h(x) for x in multi],
        "sites": sites,
    }
    if a.check:
        same = old.get("sites") == doc["sites"] and old.get("worker_functions") == doc["worker_functions"]
        print("site list %s (%d sites: %s)" % ("unchanged" if same else "DIFFERS", len(sites), dict(groups)))
        raise SystemExit(0 if same else 1)
    os.makedirs(os.path.dirname(a.out), exist_ok=True)
    with open(a.out, "w") as f:
        json.dump(doc, f, indent=1)
        f.write("\n")
    print("wrote %s: %d sites %s, %d worker functions, %d multi-thread functions, %d Dac hits outside the CFG" %
          (os.path.relpath(a.out, ROOT), len(sites), dict(groups), len(workers), len(multi), len(dac_missing)))


if __name__ == "__main__":
    main()
