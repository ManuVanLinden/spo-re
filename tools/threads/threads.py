"""Thread analysis: thread entry points, reachable sets, shared globals, atomics classes, spin loops
and barrier-policy coverage.  Reads scratch/threads/<tag>/ (from scan.py) and the function manifest;
writes small derived JSON to data/threads/ and a full per-thread function list to scratch/threads/<tag>/.

Static only; nothing from the game is executed.

Usage:   PYTHONPATH=tools/.deps python3 -P tools/threads/threads.py [--tag ep1]
Inputs:  scratch/threads/<tag>/ (from scan.py) and the function manifest
Outputs: data/threads/{threads,atomics,shared-globals,poll-loops,policy}.json and scratch/threads/<tag>/ lists
When:    the converter's threads stage, after scan.py; recomp/memorder_sites.py reads the outputs.
Environment: SPORE_WORK_DIR, SPORE_GAME_DIR and SPORE_EXE move the outputs and name the exe (docs/design/convert.md 4).
"""
import argparse
import collections
import csv
import json
import os
import re
import sys

HERE = os.path.dirname(os.path.abspath(__file__))
ROOT = os.path.abspath(os.path.join(HERE, "..", ".."))
# SPORE_WORK_DIR (default: the source tree) takes the generated outputs; SPORE_GAME_DIR / SPORE_EXE name the
# Spore folder / the EP1 exe instead of game/ (docs/design/convert.md 4).
WORK = os.environ.get("SPORE_WORK_DIR") or ROOT
GAME = os.environ.get("SPORE_GAME_DIR") or os.path.join(ROOT, "game")


def exe_of(tag):
    if tag == "ep1" and os.environ.get("SPORE_EXE"):
        return os.environ["SPORE_EXE"]
    return os.path.join(GAME, "SporebinEP1" if tag == "ep1" else "SporeBin", "SporeApp.exe")


sys.path.insert(0, HERE)
sys.path.insert(0, os.path.join(ROOT, "tools", "code-characteristics"))
sys.path.insert(0, os.path.join(ROOT, "tools", "manifest"))
import tgraph  # noqa: E402
import scan  # noqa: E402  (regions, parse_mem, MEM)

# ---------------------------------------------------------------------------------------------
# Thread kinds.  Every _beginthreadex / CreateThread site was traced by hand
# in the exe.  "roots" are where the reachable set starts:
# the OS-level start routine, the user entry it calls indirectly, and dispatch targets that were
# resolved by hand (vtable slots).  Addresses are EP1.
THREADS = [
    dict(id="main", name="main / WinMain thread", create="process entry", roots=[0x11E11E1],
         note="Direct-call closure from the PE entry; in reality reaches most of the image through "
              "virtual calls, so the policy treats every function as main-reachable."),
    dict(id="job", name="Job Thread %d (App/cJobManager/cJobThread)", create="0x690e36 -> EAThread::Begin(fn) 0x9231c0",
         roots=[0x9230C0, 0x68F530, 0x692580],
         resolved={"0x68f530 calls [mgr.vtbl+0x1c]": "0x692580 (vtable 0x14037e0 slot 7)"},
         note="Count set by -jobThreads / -noMP. Runs arbitrary jobs: 0x691cf0 calls the job closure "
              "(*job+8)(job+8, data) between EA mutex 0x14037c0 unlock/lock; job bodies are not enumerated."),
    dict(id="utfinternet", name="UTFInternet worker", create="0x9465c8 -> EAThread::Begin(fn) 0x9231c0",
         roots=[0x9230C0, 0x946480, 0x945E30],
         note="Request worker; queue under EA mutex 0x143f3bc; stop flag byte at worker+5 polled under the lock."),
    dict(id="audio-update", name="game audio update thread (rw::audio side)", create="0xa25462 -> EAThread::Begin(fn) 0x9231c0",
         roots=[0x9230C0, 0xA22810],
         note="20 ms loop calling virtuals 0xd0/0xd4 on the audio system object at [0x166d9f4]; stop flag "
              "byte at this+0x60 polled with plain loads."),
    dict(id="video", name="VideoDecodeThread", create="0xfc9435 -> EAThread::Begin(fn) 0x9231c0",
         roots=[0x9230C0, 0xFC91E0],
         note="Decodes frames into a queue; waits on condition 0x1491a98 (EA Condition 0x922920)."),
    dict(id="rwaudio-dac", name="RWAudioCore Dac", create="0x113e131 -> EAThread::Begin(fn) 0x9231c0",
         roots=[0x9230C0, 0x113DF60, 0x113DC70],
         note="Mixes into the DirectSound buffer every 20 ms (timeGetTime + Sleep). Run flag 0x16e6307 and "
              "state globals 0x16e7b98/0x16e7b9c are plain loads/stores shared with the main thread."),
    dict(id="rwaudio-watchdog", name="RwAudioCore Device WatchDog", create="0x115497f -> EAThread::Begin(fn) 0x9231c0",
         roots=[0x9230C0, 0x1154930, 0x1154870],
         note="Polls a flag word with lock-xadd-0 reads and CAS loops; Sleep 5 s; device-change check."),
    dict(id="callback", name="CallbackProcessor (EA timer callbacks)", create="0x92a9a9 -> EAThread::Begin(IRunnable) 0x923430",
         roots=[0x923330, 0x92A490],
         resolved={"IRunnable::Run [vtbl+4]": "0x92a490 (vtable 0x143e500 slot 1)"},
         note="Calls registered callbacks (fn at cb+0x14) under EA mutex 0x143e4f8."),
    dict(id="filenotify", name="FileNotification (directory watcher)", create="0x92fe2b -> EAThread::Begin(IRunnable) 0x923430",
         roots=[0x923330, 0x92FE50],
         resolved={"IRunnable::Run [vtbl+4]": "0x92fe50 (vtable 0x143e6f8 slot 1)"},
         note="ReadDirectoryChangesW-style watcher (WaitForMultipleObjects(Ex) at 0x92f97c/0x92fb6b)."),
    dict(id="rw-worker", name="RenderWare worker (unnamed)", create="0x11e6346 -> RW EAThread::Begin(fn) 0x11ecc60",
         roots=[0x11ECB60, 0x11E6210],
         note="Queue of work items under RW mutex 0x14f6548; item callbacks at item+0x44/+0x18 are indirect."),
    dict(id="assert", name="assert dialog thread", create="CreateThread 0x925429",
         roots=[0x925290], note="MessageBoxA on its own 64 KB-stack thread; waited for at 0x925438."),
]

YIELD_CALLS = {"0x922080", "dword ptr [0x13cc284]", "dword ptr [0x13cc17c]"}  # EA ThreadSleep, Sleep, SleepEx
SYNC_IMPORTS = {"EnterCriticalSection", "LeaveCriticalSection", "TryEnterCriticalSection", "WaitForSingleObject",
                "WaitForMultipleObjects", "WaitForMultipleObjectsEx", "SetEvent", "ResetEvent", "ReleaseSemaphore",
                "ReleaseMutex", "InterlockedIncrement", "InterlockedDecrement", "InterlockedExchange",
                "InterlockedExchangeAdd", "InterlockedCompareExchange", "Sleep", "SleepEx", "QueueUserAPC",
                "TlsGetValue", "TlsSetValue"}


def hx(v):
    return "0x%x" % v


def load_atomics(path):
    rows = []
    with open(path) as f:
        r = csv.reader(f, delimiter="\t")
        next(r)
        for row in r:
            rows.append(dict(va=int(row[0], 16), func=int(row[1], 16), region=row[2], mnem=row[3], ops=row[4],
                             cls=row[5], detail=row[6] if len(row) > 6 else ""))
    return rows


def refine_atomic(a, text_before):
    """Map scan.py's coarse class plus the instructions before the site to a pattern name."""
    c = a["cls"]
    det = a["detail"]
    m = re.search(r"val=(\S+)", det)
    val = m.group(1) if m else "-"
    if c == "fence":
        return "fence:" + a["mnem"]
    if a["mnem"] == "xadd":
        if val == "0x1":
            return "refcount-inc"
        if val == "0xffffffff":
            return "refcount-dec"
        if val == "0x0":
            return "atomic-load (xadd 0)"
        if val in ("0x2", "0xfffffffe"):
            return "rw-spinlock reader count (+-2)"
        return "fetch-add (other)"
    if a["mnem"] == "xchg":
        if val == "0x1" and ("lock xadd" in text_before and "0xffffffff" in text_before):
            return "refcount release guard (store 1 after dec-to-zero)"
        if val == "0x0":
            return "atomic store 0 (mostly ctor init of refcount)"
        if val == "0x1":
            return "atomic store 1"
        if val not in ("-",):
            return "atomic store const"
        return "atomic swap (register)"
    if a["mnem"] == "cmpxchg8b":
        return "cmpxchg8b (64-bit tagged-pointer CAS / 64-bit atomic load)"
    if a["mnem"] == "cmpxchg":
        return "cmpxchg (spinlock acquire / CAS loop)"
    return c


def is_poll_loop(insl, pause):
    """(strict, invariant_loads) for one candidate loop given as instruction text.

    Strict poll: no stores at all (stack included), every non-stack memory operand has a
    loop-invariant address, no register arithmetic (unless the loop executes pause, i.e. a spin
    counter), and calls only to known yield/sleep helpers."""
    written = set()
    for s in insl:
        mn, _, ops = s.partition(" ")
        if mn.startswith("call"):
            written |= {"eax", "ecx", "edx"}
        dst = ops.split(",")[0].strip()
        if dst in scan.REGS and mn not in ("cmp", "test", "push"):
            written.add(dst)
        if mn in ("inc", "dec", "pop") and ops.strip() in scan.REGS:
            written.add(ops.strip())
    inv = []
    for s in insl:
        mn, _, ops = s.partition(" ")
        if mn in ("lea", "push", "call") or mn.startswith("lock"):
            continue
        for mm in scan.MEM.finditer(ops):
            b, i, d = scan.parse_mem(mm.group(3))
            if b in ("esp", "ebp") or mm.group(2) == "fs":
                continue
            if (b is None or b not in written) and (i is None or i not in written):
                inv.append(mm.group(0))
    # strict poll: no stores at all (stack included), every non-stack load has an invariant
    # address, calls only to known yield/sleep helpers.
    stores = 0
    allinv = True
    badcall = False
    for s_ in insl:
        mn, _, ops = s_.partition(" ")
        if mn == "call" and ops not in YIELD_CALLS:
            badcall = True
        mms = list(scan.MEM.finditer(ops))
        if mms and ops.find(mms[0].group(0)) == 0 and (mn in scan.WRITE1 or mn in scan.RMW1):
            stores += 1
        for mm in mms:
            b, i, d = scan.parse_mem(mm.group(3))
            if b in ("esp", "ebp") or mn == "lea":
                continue
            if (b is not None and b in written) or (i is not None and i in written):
                allinv = False
    progress = False  # register arithmetic (counters, pointer bumps) means an iterating loop
    for s_ in insl:
        mn, _, ops = s_.partition(" ")
        dst = ops.split(",")[0].strip()
        if dst in scan.REGS and mn not in ("mov", "movzx", "movsx", "cmp", "test", "push"):
            if not (mn == "xor" and ops.split(",")[-1].strip() == dst):
                progress = True
    inv = [x for x in inv if not re.match(r"\w+ ptr \[0x1[34]", x)]  # .rdata constants
    strict = bool(inv) and stores == 0 and allinv and not badcall and (not progress or pause > 0)
    return strict, inv


def vtable_slots(tag, F):
    """slot byte offset -> set of functions, over every .rdata vtable referenced from .text.

    A vtable start is an .rdata address that a .text relocation points at and whose first dword is
    a function entry; the table runs while the following dwords are function entries and stops at
    the next start.  Over-approximate on purpose: used only for an upper bound."""
    from pe import PE
    p = PE(exe_of(tag))
    _, TS, TSIZE, _, _, _ = p.sec(".text")
    _, RS, RSZ, _, _, _ = p.sec(".rdata")
    starts = set()
    for site in p.relocs():
        if TS <= site < TS + TSIZE:
            v = p.u32(site)
            if v is not None and RS <= v < RS + RSZ and p.u32(v) in F:
                starts.add(v)
    slotmap = collections.defaultdict(set)
    for st in starts:
        off = 0
        while True:
            v = p.u32(st + off)
            if v not in F or (off and st + off in starts):
                break
            slotmap[off].add(v)
            off += 4
    slotmap["_starts"] = starts
    return slotmap


def cha_closure(F, roots, slotmap):
    seen = set()
    todo = [r for r in roots if r in F]
    unknown = 0
    while todo:
        e = todo.pop()
        if e in seen:
            continue
        seen.add(e)
        f = F[e]
        todo.extend(c for c in f.calls if c in F and c not in seen)
        for o in f.voffs:
            if o < 0:
                unknown += 1
                continue
            todo.extend(c for c in slotmap.get(o, ()) if c not in seen)
    return seen, unknown


def main():
    ap = argparse.ArgumentParser(description=__doc__, formatter_class=argparse.RawDescriptionHelpFormatter)
    ap.add_argument("--tag", default="ep1", help="the manifest and output folder tag (default ep1)")
    a = ap.parse_args()
    sdir = os.path.join(WORK, "scratch", "threads", a.tag)
    ddir = os.path.join(WORK, "data", "threads")
    os.makedirs(ddir, exist_ok=True)
    F = tgraph.load(a.tag)
    regs = scan.regions()
    totals = json.load(open(os.path.join(sdir, "scan-totals.json")))

    # ------------------------------------------------------------- global writers / accessors
    writers = collections.defaultdict(set)
    accessors = collections.defaultdict(set)
    for e, f in F.items():
        for (g, idx), rw in f.globs.items():
            accessors[g].add(e)
            if rw in "wx":
                writers[g].add(e)

    # ------------------------------------------------------------- per-thread closures
    out_threads = []
    sets = {}
    for t in THREADS:
        cl = tgraph.closure(F, t["roots"])
        missing = [hx(r) for r in t["roots"] if r not in F]
        sets[t["id"]] = cl
        ninsn = sum(F[e].ninsn for e in cl)
        icall = sum(F[e].icall for e in cl)
        ijmp = sum(F[e].ijmp for e in cl)
        imps = collections.Counter(i for e in cl for i in F[e].imports)
        gl = {}
        for e in cl:
            for (g, idx), rw in F[e].globs.items():
                old = gl.get(g)
                gl[g] = rw if old is None or old == rw else "x"
        mem = collections.Counter()
        for e in cl:
            mem.update(F[e].mem)
        out_threads.append(dict(
            id=t["id"], name=t["name"], created_at=t["create"], roots=[hx(r) for r in t["roots"]],
            roots_missing_from_manifest=missing, resolved_by_hand=t.get("resolved", {}),
            reachable_functions=len(cl), reachable_insns=ninsn,
            unresolved_indirect_calls_in_set=icall, unresolved_indirect_jumps_in_set=ijmp,
            imports_used=sorted(imps), sync_imports=sorted(i for i in imps if i in SYNC_IMPORTS),
            globals_touched=len(gl), globals_written=sum(1 for v in gl.values() if v in "wx"),
            mem_accesses=dict(mem), note=t["note"]))
        with open(os.path.join(sdir, "reach-%s.txt" % t["id"]), "w") as fo:
            fo.write("\n".join(hx(e) for e in sorted(cl)) + "\n")

    # ------------------------------------------------------------- CHA-style upper bound
    slotmap = vtable_slots(a.tag, F)
    nslots = sum(len(v) for v in slotmap.values())
    for t in out_threads:
        roots = [int(r, 16) for r in t["roots"]]
        ub, unknown = cha_closure(F, roots, slotmap)
        t["upper_bound_functions_cha"] = len(ub)
        t["upper_bound_insns_cha"] = sum(F[e].ninsn for e in ub)
        t["upper_bound_unknown_offset_icall_funcs"] = unknown

    workers = [t["id"] for t in THREADS if t["id"] != "main"]
    W = set()
    for w in workers:
        W |= sets[w]

    # ------------------------------------------------------------- shared globals (worker sets)
    shared = {}
    for w in workers:
        cl = sets[w]
        for e in cl:
            for (g, idx), rw in F[e].globs.items():
                others_write = any(x not in cl for x in writers[g])
                self_write = rw in "wx"
                others_access = any(x not in cl for x in accessors[g])
                if (others_write) or (self_write and others_access):
                    d = shared.setdefault(g, dict(threads=set(), worker_rw=set(), indexed=False))
                    d["threads"].add(w)
                    d["worker_rw"].add(rw)
                    d["indexed"] |= idx
    shared_list = []
    for g, d in sorted(shared.items()):
        shared_list.append(dict(addr=hx(g), threads=sorted(d["threads"]), worker_access="".join(sorted(d["worker_rw"])),
                                indexed=d["indexed"], writers_total=len(writers[g]), accessors_total=len(accessors[g])))
    # static instruction counts of accesses to those globals anywhere in the image
    shared_set = set(shared)
    # (per-function globals are distinct addresses, so count functions x addresses as an approximation)
    shared_fn_refs = sum(1 for e, f in F.items() for (g, idx) in f.globs if g in shared_set)

    # ------------------------------------------------------------- atomics
    atoms = load_atomics(os.path.join(sdir, "atomics.tsv"))
    # context text: take the previous few instructions by decoding the function blocks again
    import capstone
    from pe import PE
    import manifest as mf
    M = mf.load(a.tag)
    p = PE(exe_of(a.tag))
    _, TS, TSIZE, raw, _, _ = p.sec(".text")
    code = p.d[raw:raw + TSIZE]
    md = capstone.Cs(capstone.CS_ARCH_X86, capstone.CS_MODE_32)
    ins_cache = {}

    def fn_insns(e):
        if e not in ins_cache:
            lst = []
            for b in sorted(M.functions[e].blocks):
                lst.extend(md.disasm_lite(code[b.start - TS:b.end - TS], b.start))
            ins_cache[e] = lst
            if len(ins_cache) > 4000:
                ins_cache.clear()
        return ins_cache[e]

    classes = collections.Counter()
    class_by_region = collections.defaultdict(collections.Counter)
    class_in_W = collections.Counter()
    examples = collections.defaultdict(list)
    loop_sites = 0
    for at in atoms:
        ins = fn_insns(at["func"])
        k = next((i for i, x in enumerate(ins) if x[0] == at["va"]), None)
        before = " ; ".join("%s %s" % (x[2], x[3]) for x in ins[max(0, k - 10):k]) if k is not None else ""
        c = refine_atomic(at, before)
        at["pattern"] = c
        classes[c] += 1
        class_by_region[at["region"]][c] += 1
        if at["func"] in W:
            class_in_W[c] += 1
        if len(examples[c]) < 4:
            examples[c].append(hx(at["va"]))
    # misaligned-atomic risk: atomic operand with a displacement that is not a multiple of the width
    mis = []
    for at in atoms:
        mm = scan.MEM.search(at["ops"])
        if not mm:
            continue
        b, i, d = scan.parse_mem(mm.group(3))
        width = 8 if "qword" in at["ops"] else 4 if "dword" in at["ops"] else 2 if "word" in at["ops"] else 1
        if d % width:
            mis.append(hx(at["va"]))

    # ------------------------------------------------------------- spin / poll loops
    polls = []
    with open(os.path.join(sdir, "loops.tsv")) as f:
        r = csv.reader(f, delimiter="\t")
        next(r)
        for row in r:
            head, func, region, n, loads, calls, pause, text = row
            insl = [x.strip() for x in text.split("|")]
            strict, inv = is_poll_loop(insl, int(pause))
            if strict:
                polls.append(dict(head=hx(int(head, 16)), func=hx(int(func, 16)), region=region, insns=int(n),
                                  calls=int(calls), pause=int(pause), invariant_loads=sorted(set(inv))[:4],
                                  in_worker_set=int(func, 16) in W))

    # ------------------------------------------------------------- policy coverage
    tot = totals["totals"]
    L = lambda k: tot.get("mem:" + k, 0)  # noqa: E731
    stack = L("sr") + L("sw")
    glob = L("gr") + L("gw")
    heap = L("hr") + L("hw")
    rodata = L("rr")
    teb = L("tr") + L("tw")
    allacc = stack + glob + heap + rodata + teb
    natom = len([x for x in atoms if not x["pattern"].startswith("fence")])
    memW = collections.Counter()
    for e in W:
        memW.update(F[e].mem)
    heapW = memW["hr"] + memW["hw"]
    globW = memW["gr"] + memW["gw"]
    # globals accessed anywhere that are written somewhere
    polls_n = len(polls)
    pol = {
        "access_counts_static": dict(all=allacc, stack=stack, global_data=glob, heap_or_object=heap,
                                     readonly_data=rodata, teb_fs=teb, atomics=natom,
                                     note="Static instruction-operand counts over the 93,674 canonical functions; "
                                          "a read-modify-write counts once as a load and once as a store; "
                                          "stack includes implicit push/pop/call/ret."),
        "policies": [
            dict(id="P0", name="atomics only", ordered=natom, share=round(natom / allacc, 5),
                 desc="lock/xchg as seq_cst atomics, every other access relaxed. Unsound: the "
                      "spin loops and plain flag/state hand-offs in section 5 would break."),
            dict(id="P1", name="atomics + annotated idioms", ordered=natom + polls_n,
                 share=round((natom + polls_n) / allacc, 5),
                 desc="P0 plus acquire loads in the %d invariant-address poll loops and release stores "
                      "on a hand-maintained list of publish sites. Cheapest; needs per-site review and "
                      "misses unknown hand-offs." % polls_n),
            dict(id="P2", name="worker-reachable + globals", ordered=natom + heapW + glob,
                 share=round((natom + heapW + glob) / allacc, 4),
                 desc="acquire/release on every .data/.tls global access image-wide and on every heap "
                      "access inside functions reachable from a worker thread's resolved roots. "
                      "Unsound in principle because job bodies and virtual calls are not in the sets."),
            dict(id="P3", name="TSO for all non-stack accesses", ordered=natom + heap + glob + teb,
                 share=round((natom + heap + glob + teb) / allacc, 4),
                 desc="every non-stack load is ldapr/ldapur (acquire-PC), every non-stack store is "
                      "stlr/stlur (release); stack (esp/ebp-frame) and read-only data relaxed; "
                      "atomics seq_cst. Equivalent to x86-TSO for data that is not on a stack."),
            dict(id="P4", name="TSO for everything", ordered=allacc - rodata, share=round((allacc - rodata) / allacc, 4),
                 desc="also orders stack accesses; only needed if another thread reads a stack "
                      "object without a sync call between."),
        ],
        "recommended": "P3 first (correct by construction), measured on the Mac; then shrink "
                       "toward P2 per subsystem with evidence.",
    }

    out = dict(
        tag=a.tag, method="tools/threads/scan.py + tools/threads/threads.py (static; Capstone over the function "
                          "manifest; direct-call closures; thread roots resolved by hand with REA/Ghidra)",
        thread_creation_sites=dict(beginthreadex=["0x92325e", "0x9234ce", "0x11eccfe"], CreateThread=["0x925429"],
                                   ea_begin_fn="0x9231c0 (trampoline 0x9230c0)",
                                   ea_begin_runnable="0x923430 (trampoline 0x923330)",
                                   rw_begin_fn="0x11ecc60 (trampoline 0x11ecb60)",
                                   begin_call_sites=9),
        threads=out_threads,
        vtables_found=len(slotmap["_starts"]),
        union_worker_functions=len(W),
        union_worker_insns=sum(F[e].ninsn for e in W),
    )
    json.dump(out, open(os.path.join(ddir, "threads.json"), "w"), indent=1)
    json.dump(dict(total_sites=len(atoms), by_pattern=classes.most_common(), examples=examples,
                   in_worker_sets=class_in_W.most_common(),
                   by_region={k: v.most_common() for k, v in sorted(class_by_region.items())},
                   operand_kind={k: v for k, v in tot.items() if k.startswith("atomic")},
                   misaligned_displacement_sites=mis, fences=totals["fences"]),
              open(os.path.join(ddir, "atomics.json"), "w"), indent=1)
    json.dump(dict(count=len(polls), with_pause=sum(1 for x in polls if x["pause"]),
                   without_call=sum(1 for x in polls if not x["calls"]),
                   in_worker_sets=sum(1 for x in polls if x["in_worker_set"]),
                   loops=polls if len(polls) < 400 else polls[:400], truncated=len(polls) >= 400),
              open(os.path.join(ddir, "poll-loops.json"), "w"), indent=1)
    json.dump(dict(count=len(shared_list), function_refs=shared_fn_refs,
                   by_thread=collections.Counter(t for s in shared_list for t in s["threads"]),
                   globals=shared_list),
              open(os.path.join(ddir, "shared-globals.json"), "w"), indent=1)
    json.dump(pol, open(os.path.join(ddir, "policy.json"), "w"), indent=1)
    for t in out_threads:
        print(t["id"], "CHA upper bound", t["upper_bound_functions_cha"], t["upper_bound_insns_cha"])
    for t in out_threads:
        print("%-17s funcs=%6d insns=%8d icall=%5d globals=%5d/%5d sync=%s" % (
            t["id"], t["reachable_functions"], t["reachable_insns"], t["unresolved_indirect_calls_in_set"],
            t["globals_touched"], t["globals_written"], ",".join(t["sync_imports"])))
    print("worker union", len(W), "shared globals", len(shared_list), "polls", len(polls))
    print(json.dumps(pol["access_counts_static"]))
    for x in pol["policies"]:
        print(x["id"], x["ordered"], x["share"])
    print(classes.most_common())


if __name__ == "__main__":
    main()
