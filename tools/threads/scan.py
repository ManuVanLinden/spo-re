"""Thread scanner: one pass over every function of the function manifest (tools/manifest/build.py).

Outputs (git-ignored, derived from the exe), in scratch/threads/<tag>/:
  funcs.txt.gz    one record per function:
                    F <entry> <ninsn> <frame> <icall> <ijmp>
                    C <callee> ...          direct call targets (and tail jumps to other entries)
                    I <import> ...          imports called (call [IAT], jmp [IAT] thunk, mov reg,[IAT]; call reg)
                    P <codeptr> ...         address-taken code pointers (push/mov offset fn) in .text
                    V <off> ...             displacements of indirect calls through [reg+off] (vtable slot guess, -1 unknown)
                    M <kind>=<n> ...        memory-access counts: sr sw (stack) gr gw (global .data/.tls)
                                            rr (read-only data) hr hw (other / heap) ; w counts RMW too
                    G <addr><r|w|x> ...     distinct global (.data/.tls/CONST) addresses accessed; x = RMW;
                                            indexed accesses (table base) are suffixed with '+'
  atomics.tsv     every locked / implicitly locked instruction with context
  loops.tsv       candidate spin-wait loops (small cycles that re-load memory without storing)

Static only: the exe is read with mmap and decoded with Capstone; nothing is executed.

Usage:   PYTHONPATH=tools/.deps python3 -P tools/threads/scan.py [--tag ep1] [--exe EXE]
Inputs:  the exe, the function manifest, data/imports.json and data/libraries/text-regions.json
When:    the converter's threads stage, first; threads.py and recomp/memorder_sites.py follow.
Environment: SPORE_WORK_DIR, SPORE_GAME_DIR and SPORE_EXE move the outputs and name the exe (docs/design/convert.md 4).
"""
import argparse
import collections
import gzip
import json
import os
import re
import sys

HERE = os.path.dirname(os.path.abspath(__file__))
ROOT = os.path.abspath(os.path.join(HERE, "..", ".."))
# SPORE_WORK_DIR (default: the source tree) takes the generated outputs; SPORE_GAME_DIR / SPORE_EXE name the
# Spore folder / the EP1 exe instead of game/ (docs/design/convert.md 4).
# data/libraries/text-regions.json is tracked and stays in the source tree; data/imports.json is generated.
WORK = os.environ.get("SPORE_WORK_DIR") or ROOT
sys.path.insert(0, os.path.join(ROOT, "tools", "code-characteristics"))
sys.path.insert(0, os.path.join(ROOT, "tools", "manifest"))
import capstone  # noqa: E402
from pe import PE, EXE  # noqa: E402
import manifest as mf  # noqa: E402

MEM = re.compile(r"(?:(byte|word|dword|qword|xmmword|tbyte|fword) ptr )?(?:(\w+):)?\[([^\]]+)\]")
REGS = {"eax", "ecx", "edx", "ebx", "esp", "ebp", "esi", "edi", "ax", "cx", "dx", "bx", "sp", "bp", "si", "di"}

# first operand is written only
WRITE1 = {"mov", "movss", "movsd", "movaps", "movups", "movq", "movd", "movdqa", "movdqu", "movntq", "movntps",
          "movlps", "movhps", "movapd", "movupd", "movnti", "movntdq", "pop", "stmxcsr", "fnstcw", "fstcw", "fnstsw",
          "fstsw", "fnstenv", "fstenv", "fxsave", "fnsave", "fst", "fstp", "fist", "fistp", "fisttp", "fbstp",
          "movzx", "movsx"}  # movzx/movsx never have a memory destination; harmless
SETCC = re.compile(r"^set")
# first operand read and written
RMW1 = {"add", "sub", "and", "or", "xor", "adc", "sbb", "inc", "dec", "neg", "not", "shl", "shr", "sar", "sal",
        "rol", "ror", "rcl", "rcr", "shld", "shrd", "xchg", "xadd", "cmpxchg", "cmpxchg8b", "bts", "btr", "btc"}
NOACCESS = {"lea", "nop", "prefetchnta", "prefetcht0", "prefetcht1", "prefetcht2"}


def regions():
    d = json.load(open(os.path.join(ROOT, "data", "libraries", "text-regions.json")))
    out = []
    for r in d["regions"]:
        out.append((int(r["start"], 16), int(r["end"], 16), r["name"]))
    out.sort()
    return out


def region_of(regs, va):
    lo, hi = 0, len(regs)
    while lo < hi:
        mid = (lo + hi) // 2
        if regs[mid][0] <= va:
            lo = mid + 1
        else:
            hi = mid
    i = lo - 1
    if i >= 0 and regs[i][0] <= va < regs[i][1]:
        return regs[i][2]
    return "?"


def parse_mem(inner):
    """Return (base, index, disp) from the text inside [...]."""
    base = index = None
    disp = 0
    s = inner.replace(" ", "")
    for sign, term in re.findall(r"([+-]?)([^+-]+)", s):
        if "*" in term:
            index = term.split("*")[0]
        elif term in REGS:
            if base is None:
                base = term
            else:
                index = term
        else:
            v = int(term, 16) if term.startswith("0x") else int(term)
            disp += -v if sign == "-" else v
    return base, index, disp & 0xFFFFFFFF


class Scanner:
    def __init__(self, exe, tag):
        self.p = p = PE(exe)
        self.tag = tag
        self.imp = {k: v.split("!", 1)[1] for k, v in p.imports().items()}
        _, self.TS, self.TSIZE, raw, _, _ = p.sec(".text")
        self.TE = self.TS + self.TSIZE
        self.code = p.d[raw:raw + self.TSIZE]
        self.md = capstone.Cs(capstone.CS_ARCH_X86, capstone.CS_MODE_32)
        self.secs = [(n, va, max(vsz, rsz), ch) for n, va, vsz, raw_, rsz, ch in p.secs]
        self.m = mf.load(tag)
        self.regs = regions()
        self.thunks = {}

    def load_imports(self):
        d = json.load(open(os.path.join(WORK, "data", "imports.json")))
        return {int(x["iat"], 16): x["name"] for x in d["imports"]}

    def secof(self, va):
        for n, sva, sz, ch in self.secs:
            if sva <= va < sva + sz:
                return n
        return None

    def decode(self, start, end):
        o = start - self.TS
        return list(self.md.disasm_lite(self.code[o:end - self.TS], start))

    def find_thunks(self):
        for e, f in self.m.functions.items():
            if f.ninsn == 1 and f.blocks:
                ins = self.decode(f.blocks[0].start, f.blocks[0].end)
                if ins and ins[0][2] == "jmp":
                    mm = MEM.search(ins[0][3])
                    if mm:
                        b, i, d = parse_mem(mm.group(3))
                        if b is None and i is None and d in self.imp:
                            self.thunks[e] = self.imp[d]

    def run(self, outdir):
        os.makedirs(outdir, exist_ok=True)
        self.find_thunks()
        fo = gzip.open(os.path.join(outdir, "funcs.txt.gz"), "wt")
        fa = open(os.path.join(outdir, "atomics.tsv"), "w")
        fl = open(os.path.join(outdir, "loops.tsv"), "w")
        fa.write("va\tfunc\tregion\tmnem\tops\tclass\tdetail\n")
        fl.write("head\tfunc\tregion\tninsn\tloads\tcalls\tpause\ttext\n")
        entries = self.m.functions
        tot = collections.Counter()
        fences = collections.Counter()
        for n, (e, f) in enumerate(sorted(entries.items())):
            if not f.srcbits & mf.CANON:
                continue
            self.scan_func(e, f, fo, fa, fl, tot, fences)
            if n % 10000 == 0:
                print(n, hex(e), file=sys.stderr, flush=True)
        fo.close()
        fa.close()
        fl.close()
        json.dump({"totals": tot, "fences": fences, "thunks": len(self.thunks)},
                  open(os.path.join(outdir, "scan-totals.json"), "w"), indent=1)
        print(json.dumps(tot), json.dumps(fences))

    def scan_func(self, e, f, fo, fa, fl, tot, fences):
        entries = self.m.functions
        blocks = sorted(f.blocks, key=lambda b: b.start)
        insns = {}
        order = []
        for b in blocks:
            ins = self.decode(b.start, b.end)
            insns[b.start] = ins
            order.extend(ins)
        # frame pointer: push ebp ; mov ebp, esp in the first 4 instructions of the entry block
        frame = 0
        first = insns.get(e) or []
        for i in first[:6]:
            if i[2] == "mov" and i[3] == "ebp, esp":
                frame = 1
        calls, imps, ptrs = set(), set(), set()
        icall = ijmp = 0
        cnt = collections.Counter()
        globs = {}
        lastmov = {}  # reg -> import name for mov reg,[IAT]
        voffs = set()  # displacement of [reg+off] for indirect calls (vtable slot guess); -1 unknown
        for b in blocks:
            lastimm = {}
            ins = insns[b.start]
            for k, (va, sz, mn, ops) in enumerate(ins):
                prefix = ""
                if mn.startswith("lock "):
                    prefix = "lock"
                    mn = mn[5:]
                if mn.startswith("rep") and " " in mn:
                    mn = mn.split(" ", 1)[1]
                if mn in ("mfence", "lfence", "sfence"):
                    fences[mn] += 1
                    fa.write("%x\t%x\t%s\t%s\t%s\tfence\t\n" % (va, e, region_of(self.regs, va), mn, ops))
                if mn == "pause":
                    fences["pause"] += 1
                # control flow
                if mn == "call":
                    if ops.startswith("0x"):
                        t = int(ops, 16)
                        if t in self.thunks:
                            imps.add(self.thunks[t])
                        else:
                            calls.add(t)
                    else:
                        mm = MEM.search(ops)
                        if mm:
                            bb, ii, d = parse_mem(mm.group(3))
                            if bb is None and ii is None and d in self.imp:
                                imps.add(self.imp[d])
                            else:
                                icall += 1
                                if bb is not None and ii is None and bb not in ("esp", "ebp"):
                                    voffs.add(d)
                                else:
                                    voffs.add(-1)
                        elif ops in lastmov:
                            imps.add(lastmov[ops])
                        else:
                            icall += 1
                            off = -1
                            for _, _, m3, o3 in reversed(ins[max(0, k - 4):k]):
                                if o3.split(",")[0].strip() == ops:
                                    m4 = MEM.search(o3)
                                    if m3 == "mov" and m4:
                                        b4, i4, d4 = parse_mem(m4.group(3))
                                        if b4 is not None and i4 is None and b4 not in ("esp", "ebp"):
                                            off = d4
                                    break
                            voffs.add(off)
                elif mn == "jmp":
                    if ops.startswith("0x"):
                        t = int(ops, 16)
                        if t in entries and t != e:
                            if t in self.thunks:
                                imps.add(self.thunks[t])
                            else:
                                calls.add(t)
                    else:
                        mm = MEM.search(ops)
                        if mm:
                            bb, ii, d = parse_mem(mm.group(3))
                            if bb is None and ii is None and d in self.imp:
                                imps.add(self.imp[d])
                            elif not (bb is None and ii is not None):
                                ijmp += 1
                        else:
                            ijmp += 1
                # address-taken code pointers
                for imm in re.findall(r"(?<![\[\w])0x[0-9a-f]{6,8}\b", ops if "[" not in ops else re.sub(r"\[[^\]]*\]", "", ops)):
                    v = int(imm, 16)
                    if mn not in ("call", "jmp") and not mn.startswith("j") and v in entries:
                        ptrs.add(v)
                if mn == "push" and ops.startswith("0x"):
                    pass
                # memory operands
                mms = list(MEM.finditer(ops))
                if mn == "mov" and mms:
                    dst = ops.split(",")[0].strip()
                    bb, ii, d = parse_mem(mms[-1].group(3))
                    if dst in REGS and bb is None and ii is None and d in self.imp:
                        lastmov[dst] = self.imp[d]
                if mn in NOACCESS or not mms:
                    if mn in ("push",) and False:
                        pass
                    continue
                for j, mm in enumerate(mms):
                    seg = mm.group(2)
                    bb, ii, d = parse_mem(mm.group(3))
                    if j == 0 and ops.lstrip().startswith(mm.group(0)[:3]) and ops.find(mm.group(0)) == 0:
                        first_op = True
                    else:
                        first_op = (j == 0 and ops.find(mm.group(0)) == 0)
                    if first_op and (mn in WRITE1 or SETCC.match(mn)):
                        rw = "w"
                    elif first_op and mn in RMW1:
                        rw = "x"
                    elif mn in ("movs", "movsb", "movsw", "movsd", "stos", "stosb", "stosw", "stosd") and seg == "es":
                        rw = "w"
                    elif mn == "xchg":
                        rw = "x"
                    else:
                        rw = "r"
                    if mn == "push" or mn == "pop" or mn == "call":
                        pass
                    if seg == "fs":
                        kind = "t"  # TEB
                    elif bb in ("esp", "sp") or (frame and bb == "ebp"):
                        kind = "s"
                    elif bb is None and d >= 0x400000:
                        sec = self.secof(d)
                        if sec in (".data", ".tls", "CONST"):
                            kind = "g"
                            key = "%x%s" % (d, "+" if ii else "")
                            old = globs.get(key)
                            globs[key] = rw if old is None or old == rw else "x"
                        else:
                            kind = "r"  # .rdata / .text tables / IAT
                    else:
                        kind = "h"
                    if kind == "r":
                        cnt["rr"] += 1
                    elif kind == "t":
                        cnt["tr" if rw == "r" else "tw"] += 1
                    else:
                        cnt[kind + ("r" if rw == "r" else "w")] += 1
                        if rw == "x":
                            cnt[kind + "r"] += 1
                    # atomics
                    if prefix == "lock" or (mn == "xchg"):
                        cls, det = self.classify_atomic(ins, k, mn, ops, kind, bb, d, ii)
                        fa.write("%x\t%x\t%s\t%s\t%s\t%s\t%s\n" % (va, e, region_of(self.regs, va), mn, ops,
                                                                 cls, det))
                        tot["atomic:" + mn] += 1
                        tot["atomic-kind:" + kind] += 1
                # implicit stack push/pop of non-memory operands are stack accesses
            for va, sz, mn, ops in ins:
                if mn in ("push", "pop", "call", "ret", "pushal", "popal", "pushfd", "popfd", "leave"):
                    cnt["sr" if mn in ("pop", "ret", "popal", "popfd", "leave") else "sw"] += 1
        # spin-loop candidates: back edges inside the function to a block that loads memory
        self.find_loops(e, f, blocks, insns, fl)
        n = sum(len(v) for v in insns.values())
        fo.write("F %x %d %d %d %d\n" % (e, n, frame, icall, ijmp))
        if calls:
            fo.write("C " + " ".join("%x" % c for c in sorted(calls)) + "\n")
        if imps:
            fo.write("I " + " ".join(sorted(imps)) + "\n")
        if ptrs:
            fo.write("P " + " ".join("%x" % c for c in sorted(ptrs)) + "\n")
        if voffs:
            fo.write("V " + " ".join("%d" % c for c in sorted(voffs)) + "\n")
        if cnt:
            fo.write("M " + " ".join("%s=%d" % kv for kv in sorted(cnt.items())) + "\n")
        if globs:
            fo.write("G " + " ".join(k + v for k, v in sorted(globs.items())) + "\n")
        for k, v in cnt.items():
            tot["mem:" + k] += v
        tot["functions"] += 1
        tot["insns"] += n

    def classify_atomic(self, ins, k, mn, ops, kind, bb, d, ii):
        """Coarse pattern class of one locked instruction: fence, inc/dec/add, xchg by operand, or mnemonic/target."""
        src = ops.split(",")[-1].strip()
        val = None
        for va, sz, m2, o2 in reversed(ins[max(0, k - 6):k]):
            dst = o2.split(",")[0].strip()
            if dst == src:
                if m2 == "mov" and re.match(r"^-?(0x)?[0-9a-f]+$", o2.split(",")[1].strip()):
                    val = int(o2.split(",")[1].strip(), 0) & 0xFFFFFFFF
                elif m2 == "or" and o2.split(",")[1].strip() in ("0xffffffff", "-1"):
                    val = 0xFFFFFFFF
                elif m2 == "xor" and o2.split(",")[1].strip() == dst:
                    val = 0
                break
        nxt = " ; ".join("%s %s" % (m, o) for _, _, m, o in ins[k + 1:k + 4])
        det = "src=%s val=%s next=%s" % (src, "-" if val is None else "%#x" % val, nxt)
        target = "global" if kind == "g" else ("stack" if kind == "s" else "heap")
        if kind == "s" and mn in ("or", "add", "and") :
            return "fence(%s)" % mn, det
        if mn == "xadd":
            if val == 1:
                return "inc/" + target, det
            if val == 0xFFFFFFFF:
                return "dec/" + target, det
            return "add/" + target, det
        if mn == "xchg":
            if val is not None:
                return "xchg-imm%s/%s" % ("0" if val == 0 else "1" if val == 1 else "N", target), det
            return "xchg-reg/" + target, det
        if mn in ("cmpxchg", "cmpxchg8b"):
            return mn + "/" + target, det
        return mn + "/" + target, det

    def find_loops(self, e, f, blocks, insns, fl):
        starts = {b.start: b for b in blocks}
        for b in blocks:
            for s in b.succ:
                if s in starts and s <= b.start:  # back edge s <- b
                    cand = {x.start: x for x in blocks if s <= x.start <= b.start}
                    fwd, todo = set(), [s]
                    while todo:  # blocks reachable from the head inside the address range
                        y = todo.pop()
                        if y in fwd or y not in cand:
                            continue
                        fwd.add(y)
                        todo.extend(cand[y].succ)
                    if b.start not in fwd:
                        continue  # not a cycle: a jump back to shared exit code
                    bwd, todo = set(), [b.start]
                    preds = collections.defaultdict(list)
                    for y in fwd:
                        for z in cand[y].succ:
                            preds[z].append(y)
                    while todo:  # ... and that reach the latch
                        y = todo.pop()
                        if y in bwd:
                            continue
                        bwd.add(y)
                        todo.extend(preds[y])
                    body = [cand[y] for y in sorted(fwd & bwd)]
                    if len(body) > 4:
                        continue
                    ins = [i for x in body for i in insns[x.start]]
                    if len(ins) > 24:
                        continue
                    loads = stores = calls = pause = 0
                    text = []
                    for va, sz, mn, ops in ins:
                        text.append(mn + " " + ops)
                        if mn == "pause":
                            pause += 1
                        if mn == "call":
                            calls += 1
                        mm = MEM.search(ops)
                        if mm and mn not in NOACCESS:
                            bb, ii, d = parse_mem(mm.group(3))
                            if bb in ("esp",) or (bb == "ebp"):
                                continue
                            if ops.find(mm.group(0)) == 0 and (mn in WRITE1 or mn in RMW1) and not mn.startswith("lock"):
                                stores += 1
                            else:
                                loads += 1
                    if loads and not stores:
                        fl.write("%x\t%x\t%s\t%d\t%d\t%d\t%d\t%s\n" % (s, e, region_of(self.regs, s), len(ins),
                                                                    loads, calls, pause, " | ".join(text)[:3000]))


def main():
    ap = argparse.ArgumentParser(description=__doc__, formatter_class=argparse.RawDescriptionHelpFormatter)
    ap.add_argument("--tag", default="ep1", help="the manifest and output folder tag (default ep1)")
    ap.add_argument("--exe", default=EXE, help="the exe (default: the EP1 exe)")
    a = ap.parse_args()
    s = Scanner(a.exe, a.tag)
    s.run(os.path.join(WORK, "scratch", "threads", a.tag))


if __name__ == "__main__":
    main()
