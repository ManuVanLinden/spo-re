"""Function manifest: canonical function and control-flow manifest for a 32-bit MSVC PE.

Recursive-descent disassembly seeded from every structure that can name code:
  - the program entry point
  - every HIGHLOW relocation whose target is in .text (data -> code pointers and
    address-taken immediates inside .text instructions)
  - every direct call / jmp / jcc target found while decoding
  - resolved switch tables (dword tables and MSVC byte-index tables)
  - MSVC C++ EH FuncInfo funclets and SEH scope-table entries (parsed, for labels)
  - optionally Ghidra's function list (used only as a cross-check / extra seed)

Then builds basic blocks, assigns blocks to functions, classifies tail calls,
shared tails, jumps into other functions, indirect calls, noreturn calls and
dead code. Static only: never executes any byte of the target.

Usage:   PYTHONPATH=tools/.deps python3 -P tools/manifest/build.py [--tag ep1] [--exe EXE]
         (EP1: about 1-3 minutes, under 250 MB; the base game: --tag base --exe game/SporeBin/SporeApp.exe)
Inputs:  the exe; optional cross-checks picked up from scratch/code-characteristics/ when present (a Ghidra
         function list, ghidra_funcs.u32, and an instruction-start bitmap, istart.bitmap)
Outputs: scratch/manifest/<tag>/manifest.txt.gz         compact text manifest (record format: manifest.py)
         scratch/manifest/<tag>/functions.u32           sorted function entry VAs (uint32 LE), all entries
         scratch/manifest/<tag>/functions-canonical.u32 the canonical subset (see CANON)
         scratch/manifest/<tag>/labels.u32              sorted secondary-entry VAs
         scratch/manifest/<tag>/report.json             full statistics and anomaly lists
When:    the converter's manifest stage (the first after extract); every later stage reads the manifest.
Environment: SPORE_WORK_DIR, SPORE_GAME_DIR and SPORE_EXE move the outputs and name the exe (docs/design/convert.md 4).
"""
import argparse
import array
import bisect
import collections
import gzip
import hashlib
import json
import os
import re
import resource
import struct
import sys
import time

import capstone

ROOT = os.path.abspath(os.path.join(os.path.dirname(__file__), "..", ".."))
sys.path.insert(0, os.path.join(ROOT, "tools", "code-characteristics"))
from pe import PE, EXE  # noqa: E402
# SPORE_WORK_DIR (default: the source tree) takes the generated outputs; SPORE_GAME_DIR / SPORE_EXE name the
# Spore folder / the EP1 exe instead of game/ (docs/design/convert.md 4).
WORK = os.environ.get("SPORE_WORK_DIR") or ROOT

# ---------------------------------------------------------------- constants
# per-byte flags over .text
ISTART, LEADER, COVER, DATA, CALLT, BRT = 1, 2, 4, 8, 16, 32
LEADER_BRANCH = BRT  # reached by a direct branch or switch table
# per-address seed source bits
S_ENTRY, S_CALL, S_DATAPTR, S_TEXTIMM, S_GHIDRA, S_EHUNWIND, S_EHCATCH, S_UNREF = 1, 2, 4, 8, 16, 32, 64, 128
SRC_NAMES = {S_ENTRY: "entry", S_CALL: "call", S_DATAPTR: "dataptr", S_TEXTIMM: "textimm",
             S_GHIDRA: "ghidra", S_EHUNWIND: "eh-unwind", S_EHCATCH: "eh-catch", S_UNREF: "unreferenced"}
# a function counts towards the canonical total when it has one of these sources (or is a promoted
# shared tail); Ghidra-only and unreferenced code are listed but not counted
CANON = S_ENTRY | S_CALL | S_DATAPTR | S_TEXTIMM | S_EHUNWIND | S_EHCATCH
# block terminators
T_FALL, T_RET, T_JMP, T_JCC, T_TABLE, T_JMPREG, T_JMPMEM, T_NORET, T_TRAP, T_INVALID = range(10)
TNAMES = ["fall", "ret", "jmp", "jcc", "table", "jmpreg", "jmpmem", "call-noret", "trap", "invalid"]

R_TABLE = re.compile(r"^dword ptr \[(\w+)\*4 \+ (0x[0-9a-f]+)\]$")
R_MOVZXB = re.compile(r"^(\w+), byte ptr \[(\w+) \+ (0x[0-9a-f]+)\]$")
R_CMPIMM = re.compile(r"^(\w+), (0x[0-9a-f]+|\d+)$")
R_MEM = re.compile(r"\[([^\]]+)\]")
COND_PREFIX = ("j", "loop")
ALIGN_NOPS = (b"\x8d\xa4\x24\x00\x00\x00\x00", b"\x8d\x64\x24\x00", b"\x8d\x9b\x00\x00\x00\x00",
              b"\x8d\x49\x00", b"\x8b\xff", b"\x8d\x40\x00", b"\x8d\xa4\x24", b"\xeb")
# opcodes that only show up when data is decoded as code (none occur in reachable code: instruction-form census)
DATA_LIKE = {"hlt", "in", "out", "insb", "insd", "outsb", "outsd", "aaa", "aas", "daa", "das", "aam", "aad",
             "arpl", "bound", "into", "les", "lds", "iretd", "cli", "sti", "salc", "retf", "lcall", "ljmp",
             "int1", "into", "icebp"}
WRITERS = {"mov", "lea", "pop", "xor", "add", "sub", "and", "or", "movzx", "movsx", "imul", "shl", "shr",
           "sar", "inc", "dec", "neg", "not", "xchg", "cmovne", "cmove", "sbb", "adc", "bswap"}


def mem(msg):
    print(f"[{time.strftime('%H:%M:%S')}] {msg}  rss={resource.getrusage(resource.RUSAGE_SELF).ru_maxrss // 1024}MB",
          flush=True)


def main():
    ap = argparse.ArgumentParser(description=__doc__, formatter_class=argparse.RawDescriptionHelpFormatter)
    ap.add_argument("--exe", default=EXE, help="the exe to analyse (default: the EP1 exe)")
    ap.add_argument("--tag", default="ep1", help="output folder name under scratch/manifest/ (default ep1)")
    ap.add_argument("--ghidra", help="sorted uint32 list of Ghidra function entries (cross-check; "
                    "default: scratch/code-characteristics/ghidra_funcs.u32 for the EP1 exe)")
    ap.add_argument("--ghidra-seed", action="store_true",
                    help="also use Ghidra's entries as seeds (not canonical: Ghidra splits some functions)")
    ap.add_argument("--no-unreferenced", action="store_true",
                    help="skip decoding code that nothing references")
    ap.add_argument("--sweep-istart", help="instruction-start bitmap of .text from a linear-sweep disassembly (dead-code cross-check)")
    a = ap.parse_args()
    a.exe = os.path.abspath(a.exe)
    is_ep1 = os.path.exists(EXE) and os.path.samefile(a.exe, EXE)
    if a.ghidra is None and is_ep1:
        g = os.path.join(WORK, "scratch/code-characteristics/ghidra_funcs.u32")
        a.ghidra = g if os.path.exists(g) else None
    if a.sweep_istart is None and is_ep1:
        g = os.path.join(WORK, "scratch/code-characteristics/istart.bitmap")
        a.sweep_istart = g if os.path.exists(g) else None
    outdir = os.path.join(WORK, "scratch", "manifest", a.tag)
    os.makedirs(outdir, exist_ok=True)
    M = Manifest(a.exe, a.ghidra, a.ghidra_seed)
    M.unref_pass = not a.no_unreferenced
    M.R["options"] = {"ghidra_list": bool(a.ghidra), "ghidra_seed": a.ghidra_seed,
                      "unreferenced_pass": M.unref_pass}
    M.load_relocs()
    M.parse_eh()
    M.seed()
    M.descend()
    M.blocks_pass()
    M.functions_pass()
    M.dead_code(a.sweep_istart)
    M.verify()
    M.write(outdir, a.tag)


class Manifest:
    def __init__(self, exe, ghidra, ghidra_seed):
        self.exe = exe
        self.sha = hashlib.sha256(open(exe, "rb").read()).hexdigest()
        self.p = p = PE(exe)
        self.imp = p.imports()
        _, self.TS, self.TSIZE, self.RAW, _, _ = p.sec(".text")
        self.TE = self.TS + self.TSIZE
        self.code = p.d[self.RAW:self.RAW + self.TSIZE]  # bytes copy of .text (16.5 MB)
        self.fl = bytearray(self.TSIZE)
        self.src = bytearray(self.TSIZE)
        self.md = capstone.Cs(capstone.CS_ARCH_X86, capstone.CS_MODE_32)
        self.ghidra = array.array("I")
        if ghidra:
            self.ghidra = array.array("I", open(ghidra, "rb").read())
        self.ghidra_seed = ghidra_seed
        self.ref_bits = S_CALL | S_TEXTIMM
        self.unref_pass = True
        self.R = collections.OrderedDict()  # report
        self.R["exe"] = os.path.relpath(exe, ROOT)
        self.R["sha256"] = self.sha
        self.anom = collections.defaultdict(list)
        self.tables = {}  # jmp insn va -> dict
        self.labels = {}  # va -> kind (secondary entries: resume points inside functions)

    # ------------------------------------------------------------ helpers
    def intext(self, va):
        return self.TS <= va < self.TE

    def u32(self, va):
        return self.p.u32(va)

    def isrel(self, va):
        o = va - self.IMG
        return 0 <= o < self.IMGSZ and self.rel[o >> 3] >> (o & 7) & 1

    def decode1(self, va):
        o = va - self.TS
        for ins in self.md.disasm_lite(self.code[o:o + 16], va, 1):
            return ins
        return None

    def prev_insns(self, va, n):
        """Up to n already-decoded instructions immediately preceding va (contiguous)."""
        out = []
        cur = va
        while len(out) < n:
            for back in range(1, 16):
                b = cur - back
                if b < self.TS:
                    return out
                if self.fl[b - self.TS] & ISTART:
                    ins = self.decode1(b)
                    if ins is None or ins[0] + ins[1] != cur:
                        return out
                    out.append(ins)
                    cur = b
                    break
            else:
                return out
        return out

    # ------------------------------------------------------------ relocations
    def load_relocs(self):
        p = self.p
        self.IMG = p.base
        self.IMGSZ = max(s[1] + max(s[2], s[4]) for s in p.secs) - p.base
        self.rel = bytearray(self.IMGSZ // 8 + 1)
        self.d2c = array.array("I")  # pairs site,target for non-.text sites -> .text
        self.t2t = array.array("I")  # pairs for .text sites -> .text
        outside = []
        mat = collections.Counter()
        n = 0
        for s in p.relocs():
            n += 1
            o = s - self.IMG
            self.rel[o >> 3] |= 1 << (o & 7)
            t = p.u32(s)
            ts = p.secname(t) if t is not None and self.IMG <= t < self.IMG + self.IMGSZ else None
            ss = p.secname(s)
            mat[f"{ss}->{ts}"] += 1
            if ts is None or ts == "CONST":
                outside.append((s, t, ts))
            if ts == ".text":
                (self.t2t if ss == ".text" else self.d2c).extend((s, t))
        self.R["relocs"] = {"highlow": n, "other_types": p.reloc_other, "matrix": dict(mat.most_common())}
        # Q4: fixups in .text whose target is outside every section (or the tiny CONST section)
        q4 = []
        for s, t, ts in outside:
            ins = self.containing_insn(s)
            q4.append({"site": hex(s), "target": hex(t), "target_section": ts or "PE header (outside sections)",
                       "insn": f"{hex(ins[0])} {ins[2]} {ins[3]}" if ins else None})
        self.R["q4_fixups_outside_sections"] = q4
        mem(f"relocs {n}: d2c={len(self.d2c)//2} t2t={len(self.t2t)//2}")

    def containing_insn(self, site):
        """Linear decode a little before `site` to find the instruction covering it (for reporting)."""
        for back in range(1, 16):
            st = site - back
            if not self.intext(st):
                continue
            ins = self.decode1(st)
            if ins and ins[0] + ins[1] >= site + 4 and ins[0] < site:
                # make sure the window decodes consistently from a bit further back
                return ins
        return None

    # ------------------------------------------------------------ EH metadata
    def parse_eh(self):
        """MSVC FH3 FuncInfo records in .rdata: unwind funclets and catch handlers."""
        p = self.p
        _, RS, RVS, RRAW, RRSZ, _ = p.sec(".rdata")
        self.eh_unwind, self.eh_catch = set(), set()
        nfi = 0
        bad = 0
        for o in range(RRAW, RRAW + RRSZ - 36, 4):
            mg = struct.unpack_from("<I", p.d, o)[0]
            if mg not in (0x19930520, 0x19930521, 0x19930522):
                continue
            magic, maxst, punw, ntry, ptry, nip, pip, pes, flags = struct.unpack_from("<9I", p.d, o)
            if maxst > 100000 or ntry > 10000:
                bad += 1
                continue
            nfi += 1
            for i in range(maxst):
                to, act = struct.unpack_from("<iI", p.d, p.off(punw + 8 * i))
                if act:
                    self.eh_unwind.add(act)
            for i in range(ntry):
                lo, hi, ch, ncat, parr = struct.unpack_from("<iiiiI", p.d, p.off(ptry + 20 * i))
                for k in range(ncat):
                    adj, ptype, disp, h = struct.unpack_from("<IIiI", p.d, p.off(parr + 16 * k))
                    self.eh_catch.add(h)
        self.R["eh"] = {"funcinfo": nfi, "funcinfo_rejected": bad, "unwind_funclets": len(self.eh_unwind),
                        "catch_handlers": len(self.eh_catch),
                        "unwind_not_in_text": sum(1 for x in self.eh_unwind if not self.intext(x)),
                        "catch_not_in_text": sum(1 for x in self.eh_catch if not self.intext(x))}
        mem(f"eh {self.R['eh']}")

    # ------------------------------------------------------------ seeds
    def addsrc(self, va, bit):
        if self.intext(va):
            self.src[va - self.TS] |= bit
            return True
        return False

    def seed(self):
        self.addsrc(self.p.entry, S_ENTRY)
        for i in range(0, len(self.d2c), 2):
            self.addsrc(self.d2c[i + 1], S_DATAPTR)
        # pre-mark runs (>=2) of consecutive .text->.text relocs as switch-table data
        sites = sorted(self.t2t[i] for i in range(0, len(self.t2t), 2))
        self.t2t_sites = set(sites)
        runs = []
        i = 0
        while i < len(sites):
            j = i
            while j + 1 < len(sites) and sites[j + 1] == sites[j] + 4:
                j += 1
            if j > i:
                runs.append((sites[i], j - i + 1))
            i = j + 1
        self.reloc_runs = runs
        self.run_slot = set()
        for s, n in runs:
            for k in range(n):
                self.run_slot.add(s + 4 * k)
                for b in range(4):
                    self.fl[s + 4 * k + b - self.TS] |= DATA
        # remaining .text->.text relocs are seeded lazily by descend(): an immediate operand
        # is an address-taken code pointer, a memory operand is a data table inside .text
        self.text_mem_refs = []
        for x in self.eh_unwind:
            self.addsrc(x, S_EHUNWIND)
        for x in self.eh_catch:
            self.addsrc(x, S_EHCATCH)
        for g in self.ghidra:
            if self.intext(g):
                self.src[g - self.TS] |= S_GHIDRA
        mem(f"seeded; reloc runs (tables) {len(runs)}")

    def _br(self, work, t):
        if self.intext(t):
            self.fl[t - self.TS] |= BRT
        work.append(t)

    def trial_ok(self, va):
        """Linear trial decode of a pointer-derived seed: reject it (as data) if decoding hits an
        invalid opcode or an instruction that straddles a relocation site (pointer data)."""
        o = va - self.TS
        n = 0
        for addr, sz, m, ops in self.md.disasm_lite(self.code[o:o + 0x400], va):
            n += 1
            if not self.insn_plausible(addr, sz, m):
                return False
            base = m.rsplit(" ", 1)[-1]
            if base in ("ret", "jmp", "int3") or n >= 48:
                return True
        return False  # ran into undecodable bytes (or the end of the window) before a terminator

    def insn_plausible(self, addr, sz, m):
        for x in range(addr - 3, addr + sz):
            if self.isrel(x) and not (addr < x and x + 4 <= addr + sz):
                return False  # a relocated dword straddles the instruction: pointer data
        # opcodes never emitted by MSVC 2008 in this image's reachable code: data decoded as code
        return m.rsplit(" ", 1)[-1] not in DATA_LIKE

    def trial_isolated(self, va, end):
        """Unreferenced code candidate [va, end): it must decode exactly up to `end`, end in
        ret/jmp (so it does not fall into the next code), and branch only within itself or to
        canonical function entries. Anything else is dead code belonging to a function."""
        o = va - self.TS
        last = None
        for addr, sz, m, ops in self.md.disasm_lite(self.code[o:end - self.TS], va):
            if not self.insn_plausible(addr, sz, m):
                return False
            base = m.rsplit(" ", 1)[-1]
            if (base[0] == "j" or base.startswith("loop")) and ops.startswith("0x"):
                t = int(ops, 16)
                if not (va <= t < end or self.intext(t) and self.src[t - self.TS] & CANON):
                    return False
            last = (addr + sz, base)
        return last is not None and last[0] == end and last[1] in ("ret", "jmp")

    # ------------------------------------------------------------ pass 1: recursive descent
    def descend(self):
        TS, src = self.TS, self.src
        work = []
        mask = 0xFF & ~S_UNREF if self.ghidra_seed else (0xFF & ~(S_GHIDRA | S_UNREF))
        for o in range(self.TSIZE):
            if src[o] & mask:
                work.append(TS + o)
        work.reverse()
        self.n_seeds = len(work)
        t0 = time.time()
        ninsn = self._walk(work)
        if self.unref_pass:
            ninsn += self.unreferenced_code()
        self._finish_tables()
        tm = collections.Counter(b for _, _, b in self.text_mem_refs)
        self.R["descent"] = {"seconds": round(time.time() - t0, 1), "instructions": ninsn,
                             "seeds_initial": self.n_seeds,
                             "text_memory_operand_refs": dict(tm)}
        mem(f"descent done: {ninsn} insns")

    def _walk(self, work):
        """Recursive descent from the addresses in `work`; returns instructions decoded."""
        TS, fl, src = self.TS, self.fl, self.src
        conflicts = self.anom
        md = self.md
        ninsn = 0
        while work:
            va = work.pop()
            if not self.intext(va):
                conflicts["branch_target_outside_text"].append(va)
                continue
            o = va - TS
            fl[o] |= LEADER
            if fl[o] & ISTART:
                continue
            if fl[o] & (COVER | DATA):
                conflicts["target_inside_insn_or_data"].append(va)
                continue
            if not src[o] & (S_CALL | S_ENTRY) and src[o] & (S_DATAPTR | S_TEXTIMM | S_GHIDRA) \
                    and not fl[o] & LEADER_BRANCH and not self.trial_ok(va):
                conflicts["pointer_seed_rejected_as_data"].append(va)
                src[o] = 0
                fl[o] &= ~LEADER
                continue
            cur = va
            stop = False
            while not stop:
                co = cur - TS
                got = False
                for addr, sz, m, ops in md.disasm_lite(self.code[co:co + 0x800], cur):
                    got = True
                    ao = addr - TS
                    if addr != va and fl[ao] & ISTART:
                        stop = True  # joined already-decoded code
                        fl[ao] |= LEADER
                        break
                    clash = False
                    for b in range(ao, ao + sz):
                        if fl[b] & (COVER | DATA):
                            clash = True
                            break
                    if clash:
                        conflicts["decode_overlap"].append(addr)
                        stop = True
                        break
                    fl[ao] |= ISTART
                    for b in range(ao, ao + sz):
                        fl[b] |= COVER
                    ninsn += 1
                    cur = addr + sz
                    base = m.rsplit(" ", 1)[-1]
                    if sz >= 5:
                        for s in range(addr + 1, cur - 3):
                            if s in self.t2t_sites:
                                t = self.u32(s)
                                hx = "%#x" % t
                                i = ops.find(hx)
                                in_mem = i >= 0 and ops.rfind("[", 0, i) > ops.rfind("]", 0, i)
                                if base in ("call", "jmp") or base[0] == "j":
                                    pass  # branch operands handled below
                                elif in_mem:
                                    self.text_mem_refs.append((addr, t, base))
                                else:
                                    src[t - TS] |= self.ref_bits & S_TEXTIMM or S_UNREF
                                    work.append(t)
                    if base == "call":
                        if ops.startswith("0x") and int(ops, 16) == cur:
                            conflicts["getpc_call_next"].append(addr)  # call $+5; pop r
                            continue
                        if ops.startswith("0x"):
                            t = int(ops, 16)
                            if self.intext(t):
                                src[t - TS] |= self.ref_bits & S_CALL or S_UNREF
                                fl[t - TS] |= CALLT
                                work.append(t)
                            else:
                                conflicts["call_outside_text"].append(addr)
                        if self.intext(cur) and self.code[cur - TS] == 0xCC:
                            stop = True  # noreturn call followed by padding
                            break
                        continue
                    if base == "ret":
                        stop = True
                        break
                    if base == "jmp":
                        if ops.startswith("0x"):
                            self._br(work, int(ops, 16))
                        elif R_TABLE.match(ops):
                            for t in self.resolve_table(addr, ops):
                                self._br(work, t)
                        stop = True
                        break
                    if base[0] == "j" or base.startswith("loop"):
                        self._br(work, int(ops, 16))
                        if self.intext(cur):
                            fl[cur - TS] |= LEADER
                        continue
                    if base in ("int3", "hlt", "ud2"):
                        conflicts["trap_in_code"].append(addr)
                        stop = True
                        break
                    if not self.intext(cur):
                        stop = True
                        break
                if not got:
                    conflicts["invalid_insn" if cur != va else "seed_not_decodable"].append(cur)
                    stop = True
                if not self.intext(cur):
                    stop = True
        return ninsn

    def unreferenced_code(self):
        """Decode code that nothing references: undecoded regions that start a function-like
        run (after int3 padding, at a 16-byte boundary, >= 16 bytes after a ret/jmp, or right
        after code this pass already accepted) and trial-decode cleanly on their own. They become
        functions with source bit S_UNREF, outside the canonical count; a translator may include
        them so that a reference we cannot see statically still finds code."""
        TS, fl, code = self.TS, self.fl, self.code
        ds = sorted(set(self.anom.get("pointer_seed_rejected_as_data", [])) |
                    set(self.anom.get("seed_not_decodable", [])))
        o, n = 0, self.TSIZE
        n0 = k = 0
        while o < n:
            if fl[o] & (COVER | DATA) or code[o] in (0xCC, 0x90, 0x00):
                o += 1
                continue
            e = o
            while e < n and not fl[e] & (COVER | DATA) and code[e] != 0xCC:
                e += 1
            j = bisect.bisect_left(ds, TS + o)
            if j < len(ds) and ds[j] < TS + e:
                o = e  # a data island that a relocated pointer targets
                continue
            chained = False
            while o < e:
                b = bytes(code[o:e])
                if e - o < 3 or (b.startswith(ALIGN_NOPS) and e - o <= 7):
                    break
                prevb = code[o - 1] if o else 0xCC
                if not (chained or prevb == 0xCC or (TS + o) % 16 == 0 or e - o >= 16):
                    break
                va = TS + o
                if not self.trial_isolated(va, TS + e):
                    break
                self.src[o] |= S_UNREF
                n0 += 1
                self.ref_bits = S_UNREF  # what unreferenced code calls is itself unreferenced
                k += self._walk([va])
                self.ref_bits = S_CALL | S_TEXTIMM
                chained = True
                while o < e and fl[o] & (COVER | DATA):
                    o += 1
                while o < e and code[o] in (0x90, 0x00):
                    o += 1
            o = e
        self.R["unreferenced_code"] = {"seeds": n0, "instructions": k}
        mem(f"unreferenced code: {n0} seeds, {k} insns")
        return k

    def _finish_tables(self):
        starts = sorted({r["table"] for r in self.tables.values()} |
                        {r["bytetable"] for r in self.tables.values() if r["bytetable"] is not None})
        trunc = 0
        for jva, r in self.tables.items():
            if r["how"] != "reloc-run":
                continue
            k = bisect.bisect_right(starts, r["table"])
            if k < len(starts) and starts[k] < r["table"] + 4 * r["n"]:
                n2 = (starts[k] - r["table"]) // 4
                r["n"], r["targets"] = n2, r["targets"][:n2]
                r["how"] = "reloc-run-truncated"
                trunc += 1
        self.R["tables_truncated_at_next_table"] = trunc

    def resolve_table(self, jva, ops):
        """Resolve `jmp [idx*4+T]`, including MSVC two-level byte-index switches."""
        mt = R_TABLE.match(ops)
        idx, T = mt.group(1), int(mt.group(2), 16)
        prev = self.prev_insns(jva, 12)
        bt = None
        bound = None
        how = "cmp"
        want = idx
        lo = 0  # first valid index value (non-zero when the bound check is on index-k)
        pend = None  # (reg, bound) of a cmp on a register that may turn out to be want+bias
        bias = 0
        for k, (pa, psz, pm, pops) in enumerate(prev):
            pb = pm.rsplit(" ", 1)[-1]
            first = pops.split(",")[0]
            if pb == "movzx" and bt is None:
                mb = R_MOVZXB.match(pops)
                if mb and mb.group(1) == want:
                    bt = int(mb.group(3), 16)
                    want = mb.group(2)
                    continue
            if pb == "cmp":
                parts = pops.rsplit(", ", 1)
                if len(parts) == 2 and re.match(r"^(0x[0-9a-f]+|\d+)$", parts[1]):
                    imm = int(parts[1], 0)
                    nxt = prev[k - 1][2] if k >= 1 else ""  # the branch following the cmp
                    b_ = imm if nxt.endswith("jae") or nxt.endswith("jnb") else imm + 1
                    if parts[0] == want:
                        bound = b_
                        break
                    if pend is None and want.startswith("dword ptr ["):
                        pend, bias = (parts[0], b_), 0
                        continue
            if pend is not None and first == pend[0]:
                # cmp reg was on (want + bias): `mov reg, want; add reg, -k; cmp reg, n`
                if pb in ("add", "sub") and re.match(r"^\w+, (0x[0-9a-f]+|-?0x[0-9a-f]+|-?\d+)$", pops):
                    v = int(pops.split(", ")[1], 0)
                    bias += v if pb == "add" else -v
                    continue
                if pb == "mov" and pops.split(", ", 1)[1] == want:
                    lo, bound = (-bias) & 0xFFFFFFFF, pend[1]
                    how = "cmp-biased"
                    break
                pend = None
            if pb == "mov" and first == want and "ptr [" in pops.split(", ", 1)[1]:
                want = pops.split(", ", 1)[1]  # index reloaded from a stack slot (unoptimised code)
                continue
            if pb in WRITERS and first == want and pb != "movzx":
                break  # index register redefined before any bound check
        if lo and (bt is None or lo > 0x1000):
            lo, bound, how = 0, None, "cmp"  # only byte-index tables are handled with a bias
        rec = {"table": T, "bytetable": bt, "bound": bound}
        if bt is not None:
            if bound is None:
                how = "bytetable-unbounded"
                fullrun = 0
                while self.intext(T + 4 * fullrun) and (T + 4 * fullrun) in self.t2t_sites:
                    fullrun += 1
                nb = 0
                while self.intext(bt + nb) and self.code[bt + nb - self.TS] < fullrun and nb < 4096 \
                        and not (self.fl[bt + nb - self.TS] & (COVER | DATA)):
                    nb += 1
                bound = nb
            idxs = [self.code[bt + lo + i - self.TS] for i in range(bound)]
            n = max(idxs) + 1 if idxs else 0
            for i in range(bound):
                self.fl[bt + lo + i - self.TS] |= DATA
            rec["bytetable"] = bt + lo  # first byte actually indexed
            rec["bytetable_base"] = bt
            rec["nbytes"] = bound
        else:
            n = bound
        # validate / bound by relocation slots
        run = 0
        while self.intext(T + 4 * run) and (T + 4 * run) in self.t2t_sites:
            run += 1
            if n is not None and run >= n:
                break
        if n is None:
            how = "reloc-run"
            n = run
        elif run < n:
            self.anom["table_bound_exceeds_reloc_slots"].append(jva)
            n = run
        targets = [self.u32(T + 4 * i) for i in range(n)]
        for i in range(n):
            for b in range(4):
                self.fl[T + 4 * i + b - self.TS] |= DATA
        rec.update(n=n, how=how, targets=targets)
        self.tables[jva] = rec
        return targets

    # ------------------------------------------------------------ pass 2: blocks
    def blocks_pass(self):
        TS, fl, md = self.TS, self.fl, self.md
        istart_cls = re.compile(bytes("[" + "".join("\\x%02x" % b for b in range(256) if b & ISTART) + "]", "latin1"))
        self.bstart = array.array("I")
        self.bend = array.array("I")
        self.bterm = array.array("B")
        self.bsucc_off = array.array("I")
        self.succ = array.array("I")
        self.bninsn = array.array("I")
        callreg = collections.Counter()
        callreg_imports = collections.Counter()
        callmem = collections.Counter()
        vslots = collections.Counter()
        callreg_samples = collections.defaultdict(list)
        textimm_kind = collections.Counter()
        setjmp_thunks = {t for t, nm in self.thunk_names().items() if "setjmp" in nm}
        seh_thunks = {t for t, nm in self.thunk_names().items() if "_except_handler" in nm}
        self.seh_tables = []
        self._seh4_seen = set()
        nins = 0
        pos = 0
        stats_calls = collections.Counter()
        last_report = 0
        t0 = time.time()
        while True:
            mm = istart_cls.search(fl, pos)
            if not mm:
                break
            va = TS + mm.start()
            # decode a maximal run of contiguous decoded instructions
            bst = va
            ring = collections.deque(maxlen=12)
            cur = va
            ninb = 0
            done_run = False
            while not done_run:
                co = cur - TS
                progressed = False
                for addr, sz, m, ops in md.disasm_lite(self.code[co:co + 0x800], cur):
                    ao = addr - TS
                    if not fl[ao] & ISTART:
                        done_run = True
                        break
                    progressed = True
                    if addr != bst and fl[ao] & LEADER:
                        self.emit(bst, addr, T_FALL, [addr], ninb)
                        bst, ninb = addr, 0
                        ring.clear()
                    nins += 1
                    ninb += 1
                    nxt = addr + sz
                    base = m.rsplit(" ", 1)[-1]
                    # address-taken immediates in this instruction (labels / callbacks)
                    if not base.startswith("j") and base != "call":
                        for s in range(addr + 1, nxt - 3):
                            if s in self.t2t_sites:
                                t = self.u32(s)
                                textimm_kind[base] += 1
                                if base == "mov" and ops.startswith("eax,"):
                                    nx = self.decode1(nxt)
                                    if nx and nx[2] == "ret":
                                        self.labels.setdefault(t, "eh-catch-continuation")
                    term = None
                    if base == "call":
                        if ops.startswith("0x"):
                            t = int(ops, 16)
                            stats_calls["direct"] += 1
                            if t in setjmp_thunks:
                                self.labels.setdefault(nxt, "setjmp-return")
                            if ring and ring[-1][2] == "push" and ring[-1][3].startswith("0x"):
                                self.parse_scopetable4(int(ring[-1][3], 16), addr)
                        elif ops.startswith("dword ptr [0x"):
                            t = int(ops[11:-1], 16)
                            stats_calls["iat" if t in self.imp else "global-fnptr"] += 1
                        elif ops.startswith("dword ptr ["):
                            k = self.classify_mem_call(ops, ring)
                            callmem[k[0]] += 1
                            stats_calls["mem[reg+disp]"] += 1
                            if k[1] is not None:
                                vslots[k[1]] += 1
                        else:
                            k = self.classify_reg_call(ops, ring)
                            callreg[k[0]] += 1
                            stats_calls["reg"] += 1
                            if k[0] == "import-via-register":
                                callreg_imports[k[2]] += 1
                            if k[1] is not None:
                                vslots[k[1]] += 1
                            if len(callreg_samples[k[0]]) < 8:
                                callreg_samples[k[0]].append(hex(addr))
                        if ops.startswith("0x") and int(ops, 16) == nxt:
                            term = None  # get-PC idiom: behaves like `push nxt`
                        elif self.intext(nxt) and not fl[nxt - TS] & ISTART:
                            term = (T_NORET, [])
                    elif base == "ret":
                        term = (T_RET, [])
                    elif base == "jmp":
                        if ops.startswith("0x"):
                            term = (T_JMP, [int(ops, 16)])
                        elif addr in self.tables:
                            term = (T_TABLE, sorted(set(self.tables[addr]["targets"])))
                            self.refine_table(addr, ring)
                        elif ops.startswith("dword ptr"):
                            term = (T_JMPMEM, [])
                        else:
                            term = (T_JMPREG, [])
                    elif base[0] == "j" or base.startswith("loop"):
                        term = (T_JCC, [int(ops, 16), nxt])
                    elif base in ("int3", "hlt", "ud2"):
                        term = (T_TRAP, [])
                    elif base == "push" and ops.startswith("0x") and int(ops, 16) in seh_thunks and ring:
                        pa, psz, pm, pops = ring[-1]
                        if pm == "push" and pops.startswith("0x"):
                            self.parse_scopetable(int(pops, 16), addr)
                    ring.append((addr, sz, m, ops))
                    if term is None and not (self.intext(nxt) and fl[nxt - TS] & ISTART):
                        term = (T_INVALID, [])  # decoded code stops without a terminator
                    if term is not None:
                        self.emit(bst, nxt, term[0], term[1], ninb)
                        bst, ninb = nxt, 0
                        ring.clear()
                        if not (self.intext(nxt) and fl[nxt - TS] & ISTART):
                            cur = nxt
                            done_run = True
                            break
                    cur = nxt
                    if not self.intext(cur):
                        done_run = True
                        break
                if not progressed:
                    done_run = True
            pos = max(cur - TS, va - TS + 1)
            if nins - last_report > 500000:
                last_report = nins
                mem(f"  blocks pass at {cur:#x}: {nins} insns, {len(self.bstart)} blocks")
            if ninb:
                self.emit(bst, cur, T_INVALID, [], ninb)
        self.R["blocks"] = {"seconds": round(time.time() - t0, 1), "instructions": nins, "blocks": len(self.bstart),
                            "by_terminator": dict(collections.Counter(TNAMES[k] for k in self.bterm).most_common())}
        self.R["calls"] = dict(stats_calls)
        self.R["q7_call_reg"] = {"total": sum(callreg.values()), "classes": dict(callreg.most_common()),
                                 "import_via_register_top": dict(callreg_imports.most_common(40)),
                                 "samples": dict(callreg_samples)}
        self.R["call_mem_reg_disp"] = dict(callmem.most_common())
        self.R["virtual_slot_hist_top"] = {str(k): v for k, v in vslots.most_common(30)}
        self.R["textimm_by_insn"] = dict(textimm_kind.most_common(20))
        mem(f"blocks: {len(self.bstart)}")

    def emit(self, s, e, k, succ, n):
        self.bstart.append(s)
        self.bend.append(e)
        self.bterm.append(k)
        self.bsucc_off.append(len(self.succ))
        self.succ.extend(succ)
        self.bninsn.append(n)

    def thunk_names(self):
        if not hasattr(self, "_thunks"):
            self._thunks = {}
            # import thunks: `jmp [IAT]` instructions that are block starts
            for mt in re.finditer(b"\xff\x25", self.code):
                jva_off = mt.start()
                if self.fl[jva_off] & ISTART:
                    t = struct.unpack_from("<I", self.code, jva_off + 2)[0]
                    if t in self.imp:
                        self._thunks[self.TS + jva_off] = self.imp[t]
        return self._thunks

    def refine_table(self, jva, ring):
        pass  # bound already derived from the decoded predecessors in resolve_table

    def parse_scopetable4(self, st, site):
        """MSVC SEH4 scope table pushed before a call to __SEH_prolog4:
        {GSCookieOffset=-2, GSCookieXOROffset, EHCookieOffset, EHCookieXOROffset} then
        {EnclosingLevel (-2 = none), FilterFunc, HandlerFunc} x n. The handler is entered by a jump
        from _except_handler4 inside the parent's frame, so it is a label of the parent."""
        if self.p.secname(st) != ".rdata" or self.p.u32(st) != 0xFFFFFFFE or st in self._seh4_seen:
            return
        ents = []
        for i in range(32):
            o = self.p.off(st + 16 + 12 * i)
            if o is None:
                break
            enc, flt, hnd = struct.unpack_from("<iII", self.p.d, o)
            if not (enc == -2 or 0 <= enc < i) or not self.intext(hnd) or (flt and not self.intext(flt)):
                break
            if not (self.isrel(st + 16 + 12 * i + 8) and (not flt or self.isrel(st + 16 + 12 * i + 4))):
                break
            ents.append((enc, flt, hnd))
        if not ents:
            return
        self._seh4_seen.add(st)
        for enc, flt, hnd in ents:
            if flt:
                self.labels.setdefault(hnd, "seh4-except-handler")
        self.seh_tables.append({"frame_insn": hex(site), "scopetable": hex(st), "kind": "seh4",
                                "entries": [[e, hex(f), hex(h)] for e, f, h in ents]})

    def parse_scopetable(self, st, site):
        """_except_handler3 scope table: {enclosing, filter, handler} x n (in .rdata)."""
        ents = []
        i = 0
        while i < 64:
            o = self.p.off(st + 12 * i)
            if o is None:
                break
            enc, flt, hnd = struct.unpack_from("<iII", self.p.d, o)
            if not (enc == -1 or 0 <= enc < i) or not self.intext(hnd) or (flt and not self.intext(flt)):
                break
            ents.append((enc, flt, hnd))
            if flt:
                self.labels.setdefault(hnd, "seh-except-handler")
            i += 1
        self.seh_tables.append({"frame_insn": hex(site), "scopetable": hex(st),
                                "entries": [[e, hex(f), hex(h)] for e, f, h in ents]})

    def find_writer(self, reg, ring, start):
        for i in range(start, -1, -1):
            a, sz, m, ops = ring[i]
            b = m.rsplit(" ", 1)[-1]
            first = ops.split(",")[0]
            if b == "call":
                if reg in ("eax", "ecx", "edx"):
                    return i, ("call-result" if reg == "eax" else "clobbered-by-call"), None
                continue
            if b in WRITERS and first == reg:
                return i, b, ops
            if b == "xchg" and reg in ops:
                return i, b, ops
        return None, None, None

    def classify_reg_call(self, ops, ring):
        reg = ops.strip()
        i, b, wops = self.find_writer(reg, ring, len(ring) - 1)
        if i is None:
            return ("defined-before-block", None, None)
        if b in ("call-result", "clobbered-by-call"):
            return (b, None, None)
        if b == "mov":
            srcop = wops.split(", ", 1)[1]
            if srcop.startswith("dword ptr [0x"):
                t = int(srcop[11:-1], 16)
                if t in self.imp:
                    return ("import-via-register", None, self.imp[t])
                return ("global-fnptr", None, None)
            if srcop.startswith("0x"):
                return ("immediate-address", None, None)
            mm = R_MEM.search(srcop)
            if mm:
                inner = mm.group(1)
                breg = inner.split(" ")[0]
                disp = 0
                md2 = re.search(r"([+-]) (0x[0-9a-f]+|\d+)$", inner)
                if md2:
                    disp = int(md2.group(2), 0) * (1 if md2.group(1) == "+" else -1)
                if breg in ("esp", "ebp"):
                    return ("stack-slot (fnptr arg/local)", None, None)
                j, b2, w2 = self.find_writer(breg, ring, i - 1)
                if b2 == "mov" and w2 and re.match(r"^\w+, dword ptr \[\w+\]$", w2):
                    return ("virtual (vptr load + slot)", disp // 4, None)
                if b2 == "mov" and w2 and R_MEM.search(w2.split(", ", 1)[1] or ""):
                    return ("load from loaded pointer (vtable/field)", disp // 4, None)
                return ("load [reg+disp], base defined earlier", disp // 4 if disp % 4 == 0 else None, None)
            return ("register copy", None, None)
        return ("other:" + b, None, None)

    def classify_mem_call(self, ops, ring):
        mm = R_MEM.search(ops)
        inner = mm.group(1)
        breg = inner.split(" ")[0]
        disp = 0
        md2 = re.search(r"([+-]) (0x[0-9a-f]+|\d+)$", inner)
        if md2:
            disp = int(md2.group(2), 0) * (1 if md2.group(1) == "+" else -1)
        if breg in ("esp", "ebp"):
            return ("stack-slot", None)
        j, b2, w2 = self.find_writer(breg, ring, len(ring) - 1)
        if b2 == "mov" and w2 and re.match(r"^\w+, dword ptr \[\w+\]$", w2):
            return ("virtual (vptr load + slot)", disp // 4)
        if j is None:
            return ("base defined before block", disp // 4 if disp % 4 == 0 else None)
        return ("field/other", None)

    # ------------------------------------------------------------ pass 3: functions
    def functions_pass(self):
        TS, src = self.TS, self.src
        nb = len(self.bstart)
        bstart = self.bstart

        class _BIdx:
            def get(self_, t, d=None):
                i = bisect.bisect_left(bstart, t)
                return i if i < nb and bstart[i] == t else d

            def __contains__(self_, t):
                return self_.get(t) is not None

            def __getitem__(self_, t):
                return self_.get(t)

        bidx = _BIdx()
        self.bidx = bidx

        def succs(i):
            e = self.bsucc_off[i + 1] if i + 1 < nb else len(self.succ)
            return self.succ[self.bsucc_off[i]:e]

        self.succs = succs
        # intra-procedural predecessors (jcc / fall / table edges), to tell labels from entries
        intra_in = bytearray(nb)
        jmp_in = bytearray(nb)
        jmp_preds = collections.defaultdict(list)
        for i in range(nb):
            k = self.bterm[i]
            for t in succs(i):
                j = bidx.get(t)
                if j is None:
                    continue
                if k in (T_FALL, T_TABLE) or (k == T_JCC and t != self.bend[i]):
                    intra_in[j] |= 1 if k != T_JCC else 2
                elif k == T_JMP:
                    jmp_in[j] = 1
                    jmp_preds[j].append(i)
                if k == T_JCC and t == self.bend[i]:
                    intra_in[j] |= 1  # jcc fall-through
        # candidate entries
        cand = {}
        cmask = 0xFF if self.ghidra_seed else (0xFF & ~S_GHIDRA)
        for o in range(self.TSIZE):
            if src[o] & cmask and self.fl[o] & ISTART:
                if bidx.get(TS + o) is None:
                    self.anom["candidate_not_block_start"].append(TS + o)
                    continue
                cand[TS + o] = src[o]
        demoted = collections.Counter()
        entries = []
        label_src = {}
        for va, s in cand.items():
            i = bidx.get(va)
            strong = s & (S_ENTRY | S_CALL | S_EHUNWIND | S_EHCATCH)
            if va in self.labels and not strong:
                label_src[va] = s
                demoted[self.labels[va]] += 1
                continue
            if not strong and i is not None and intra_in[i] & 1:
                # reached by a conditional branch / fall-through / switch: an address-taken label
                self.labels.setdefault(va, "address-taken-label")
                label_src[va] = s
                demoted["intra-reached " + "+".join(n for b, n in SRC_NAMES.items() if s & b)] += 1
                continue
            entries.append(va)
        entries.sort()
        self.entries = entries
        eset = set(entries)
        self.eset = eset
        self.R["entry_demotions"] = dict(demoted.most_common())
        self.promoted = set()
        for it in range(32):
            owner, shared = self.assign(entries, eset, nb, succs, bidx, dry=True)
            # promote a shared block reached by `jmp` only when at least one jumping block is
            # itself unshared; otherwise wait for an outer shared tail to be promoted first, so
            # loop heads inside a shared tail are not split off (cascade).
            prom = sorted(self.bstart[b] for b in shared if jmp_in[b] and not intra_in[b] & 1
                          and any(owner[p] >= 0 for p in jmp_preds[b]))
            if not prom:
                prom = sorted(self.bstart[b] for b in shared if jmp_in[b] and not intra_in[b] & 1)[:1]
            if not prom:
                break
            self.promoted.update(prom)
            entries = sorted(set(entries) | set(prom))
            eset = set(entries)
        self.entries, self.eset = entries, eset
        self.R["shared_tail_promotion_rounds"] = it
        self.R["promoted_shared_jmp_targets"] = len(self.promoted)
        self.R["promoted_samples"] = [hex(x) for x in sorted(self.promoted)[:20]]
        self.assign_and_report(entries, eset, nb, succs, bidx)

    def assign(self, entries, eset, nb, succs, bidx, dry):
        owner = array.array("i", [-1]) * nb
        shared = collections.defaultdict(set)
        for fi, e in enumerate(entries):
            st = [bidx[e]]
            seen = {bidx[e]}
            while st:
                i = st.pop()
                for t in succs(i):
                    j = bidx.get(t)
                    if j is None or (t in eset and t != e):
                        continue
                    if j not in seen:
                        seen.add(j)
                        st.append(j)
            for i in seen:
                if owner[i] == -1:
                    owner[i] = fi
                elif owner[i] != fi:
                    if owner[i] >= 0:
                        shared[i].add(owner[i])
                    shared[i].add(fi)
                    owner[i] = -2
        return owner, shared

    def assign_and_report(self, entries, eset, nb, succs, bidx):
        # ownership
        owner = array.array("i", [-1]) * nb
        shared = collections.defaultdict(set)
        tail_calls = 0
        fall_into = []
        jmp_mid = []
        noret_fall = 0
        cond_tail = []
        jmp_to_promoted = []
        fblocks = []  # per function: array of block idx (for writing)
        for fi, e in enumerate(entries):
            st = [bidx[e]]
            seen = {bidx[e]}
            while st:
                i = st.pop()
                k = self.bterm[i]
                for t in succs(i):
                    j = bidx.get(t)
                    if j is None:
                        continue
                    if t in eset and t != e:
                        if k == T_JMP:
                            tail_calls += 1
                            if t in self.promoted:
                                jmp_to_promoted.append((self.bstart[i], t))
                        elif k == T_FALL:
                            fall_into.append((self.bstart[i], t))
                        elif k == T_JCC:
                            cond_tail.append((self.bstart[i], t))
                        elif k == T_TABLE:
                            self.anom["table_to_function_entry"].append((self.bstart[i], t))
                        continue
                    if j not in seen:
                        seen.add(j)
                        st.append(j)
            blks = array.array("I", sorted(seen))
            fblocks.append(blks)
            for i in blks:
                if owner[i] == -1:
                    owner[i] = fi
                elif owner[i] != fi:
                    if owner[i] >= 0:
                        shared[i].add(owner[i])
                    shared[i].add(fi)
                    owner[i] = -2
        self.owner, self.fblocks, self.shared = owner, fblocks, shared
        # blocks not reached from any entry: hang them on the function owning the preceding block
        orphan_roots = []
        for i in range(nb):
            if owner[i] == -1:
                orphan_roots.append(i)
        attached = collections.Counter()
        for i in orphan_roots:
            if owner[i] != -1:
                continue
            # a resume point (catch continuation, setjmp return) belongs to the function its
            # code flows back into; fall back to the function owning the preceding block
            succ_owners = set()
            stk, seen_o = [i], {i}
            while stk and len(seen_o) < 64:
                b = stk.pop()
                for t in succs(b):
                    jj = bidx.get(t)
                    if jj is None or jj in seen_o or t in eset:
                        continue
                    if owner[jj] >= 0:
                        succ_owners.add(owner[jj])
                    elif owner[jj] == -1:
                        seen_o.add(jj)
                        stk.append(jj)
            if len(succ_owners) == 1:
                fi = succ_owners.pop()
            else:
                j = i - 1
                while j >= 0 and owner[j] < 0:
                    j -= 1
                fi = owner[j] if j >= 0 else -1
            kind = self.labels.get(self.bstart[i], "unreached")
            attached[kind] += 1
            if fi < 0:
                continue
            stk = [i]
            add = []
            while stk:
                b = stk.pop()
                if owner[b] != -1:
                    continue
                owner[b] = fi
                add.append(b)
                for t in succs(b):
                    jj = bidx.get(t)
                    if jj is not None and owner[jj] == -1 and t not in eset:
                        stk.append(jj)
            fblocks[fi] = array.array("I", sorted(set(fblocks[fi]) | set(add)))
            if self.bstart[i] not in self.labels:
                self.labels[self.bstart[i]] = "runtime-only-label"
        # jumps into the middle of another function (Q5)
        for i in range(nb):
            if self.bterm[i] != T_JMP:
                continue
            t = self.succ[self.bsucc_off[i]]
            j = bidx.get(t)
            if j is None or t in eset:
                continue
            if owner[i] >= 0 and owner[j] >= 0 and owner[i] != owner[j]:
                jmp_mid.append((self.bstart[i], t, "other-owner"))
            elif owner[j] == -2:
                jmp_mid.append((self.bstart[i], t, "shared-tail"))
        # noreturn calls whose fall-through is another function entry
        for i in range(nb):
            if self.bterm[i] == T_FALL and owner[i] != owner[bidx.get(self.bend[i], i)] and self.bend[i] in eset:
                noret_fall += 1
        shared_insn = sum(self.bninsn[i] * (len(f) - 1) for i, f in shared.items())
        # Q5 detail: classify shared tails by what the shared code does
        tail_kind = collections.Counter()
        for i in shared:
            tail_kind[TNAMES[self.bterm[i]]] += 1
        self.R["functions"] = {
            "entries": len(entries),
            "labels_secondary_entries": len(self.labels),
            "labels_by_kind": dict(collections.Counter(self.labels.values()).most_common()),
            "tail_call_jmps": tail_calls,
            "jmps_to_promoted_shared_tails": len(jmp_to_promoted),
            "jmps_to_promoted_by_target_top": [[hex(t), n] for t, n in collections.Counter(
                t for _, t in jmp_to_promoted).most_common(25)],
            "conditional_tail_calls(jcc to entry)": len(cond_tail),
            "conditional_tail_call_samples": [[hex(a), hex(b)] for a, b in cond_tail[:10]],
            "fall_into_other_function": len(fall_into),
            "fall_into_samples": [[hex(a), hex(b)] for a, b in fall_into[:10]],
            "shared_blocks": len(shared),
            "shared_block_terminators": dict(tail_kind),
            "shared_extra_insns_if_duplicated": shared_insn,
            "unreached_block_roots": dict(attached),
        }
        self.R["q5_jumps_into_other_functions"] = {
            "count": len(jmp_mid), "distinct_targets": len({t for _, t, _ in jmp_mid}),
            "by_kind": dict(collections.Counter(k for _, _, k in jmp_mid)),
            "samples": [[hex(a), hex(t), k] for a, t, k in jmp_mid[:40]]}
        self.jmp_mid = jmp_mid
        mem(f"functions: {len(entries)} entries, {len(self.labels)} labels, {len(shared)} shared blocks")

    # ------------------------------------------------------------ dead code
    def dead_code(self, sweep_istart):
        """Bytes neither decoded nor data nor int3/nop padding: unreferenced code or data."""
        TS, fl, code = self.TS, self.fl, self.code
        self.data_seeds = set(self.anom.get("pointer_seed_rejected_as_data", [])) | \
            set(self.anom.get("seed_not_decodable", []))
        ds = sorted(self.data_seeds)
        self.islands = []
        regions = []
        o = 0
        n = self.TSIZE
        while o < n:
            if fl[o] & (COVER | DATA) or code[o] == 0xCC:
                o += 1
                continue
            s = o
            while o < n and not (fl[o] & (COVER | DATA)) and code[o] != 0xCC:
                o += 1
            regions.append((TS + s, TS + o))
        # padding-like regions: nop fills / zero fills
        kinds = collections.Counter()
        total = collections.Counter()
        samples = collections.defaultdict(list)
        self.dead = []
        for s, e in regions:
            b = code[s - TS:e - TS]
            k_ = bisect.bisect_left(ds, s)
            if k_ < len(ds) and ds[k_] < e:
                k = "data island (target of a relocated pointer)"
                self.islands.append((s, e))
            elif set(b) <= {0x90} or set(b) <= {0}:
                k = "nop/zero padding"
            elif e - s <= 3 and (b in (b"\x8d\x49\x00", b"\x8b\xff", b"\x8d\x40\x00") or set(b) <= {0x90, 0x8b, 0xff, 0x8d, 0x49, 0x40, 0x00, 0x64, 0x24, 0xa4}):
                k = "alignment nops"
            elif b.startswith(b"\x8d\xa4\x24\x00\x00\x00\x00") or b.startswith(b"\x8d\x64\x24\x00") or b.startswith(b"\x8d\x9b\x00\x00\x00\x00") or b.startswith(b"\x8d\x49\x00") or b.startswith(b"\xeb\x03"):
                k = "alignment nops"
            else:
                prevb = code[s - TS - 1] if s > TS else 0xCC
                k = "undecoded code after int3 padding (unreferenced or dead)" if prevb == 0xCC else "dead code after unconditional transfer"
                self.dead.append((s, e))
            kinds[k] += 1
            total[k] += e - s
            if len(samples[k]) < 15:
                samples[k].append([hex(s), e - s])
        self.R["undecoded_text"] = {"regions": dict(kinds), "bytes": dict(total), "samples": dict(samples)}
        if sweep_istart:
            sw = open(sweep_istart, "rb").read()
            both = only_sweep = only_rd = 0
            for i in range(0, n):
                a = sw[i >> 3] >> (i & 7) & 1
                b = fl[i] & ISTART
                if a and b:
                    both += 1
                elif a and not b and code[i] != 0xCC:
                    only_sweep += 1
                elif b and not a:
                    only_rd += 1
            self.R["vs_linear_sweep"] = {"insn_both": both, "only_linear_sweep(non-int3)": only_sweep,
                                         "only_descent": only_rd}
        mem("dead code done")

    # ------------------------------------------------------------ verification
    def verify(self):
        V = collections.OrderedDict()
        bidx, eset = self.bidx, self.eset
        bstarts = bidx
        # 1. every relocated code pointer / direct call target is a function or block start
        miss, to_data = [], []
        memt = {t for _, t, _ in self.text_mem_refs} | {r["table"] for r in self.tables.values()}
        for arr, nm in ((self.d2c, "data->code"), (self.t2t, "text-imm")):
            for i in range(0, len(arr), 2):
                s, t = arr[i], arr[i + 1]
                if nm == "text-imm" and (s in self.run_slot or t in memt):
                    continue
                if t not in bstarts:
                    if t in self.data_seeds:
                        to_data.append((nm, hex(s), hex(t)))
                    else:
                        miss.append((nm, hex(s), hex(t)))
        calls_miss = [hex(self.TS + o) for o in range(self.TSIZE) if self.fl[o] & CALLT and self.TS + o not in eset]
        V["reloc_code_ptrs_not_block_start"] = len(miss)
        V["reloc_code_ptrs_not_block_start_samples"] = miss[:20]
        V["reloc_ptrs_to_data_islands_in_text"] = to_data
        V["call_targets_not_function_entry"] = len(calls_miss)
        V["call_targets_not_function_entry_samples"] = calls_miss[:20]
        # every .text reloc site is inside an instruction operand, a table slot, or undecoded (island)
        tbl_slots = set()
        for r in self.tables.values():
            for k in range(r["n"]):
                tbl_slots.add(r["table"] + 4 * k)
        straddle, island = [], []
        nin = 0
        for s in self.text_reloc_sites():
            if s in tbl_slots:
                continue
            o = s - self.TS
            if all(self.fl[o + b] & COVER for b in range(4)):
                # find start of containing instruction
                st = o
                while st > o - 15 and not self.fl[st] & ISTART:
                    st -= 1
                if st == o:
                    straddle.append(hex(s))
                    continue
                ins = self.decode1(self.TS + st)
                if ins and ins[0] + ins[1] >= s + 4:
                    nin += 1
                else:
                    straddle.append(hex(s))
            elif any(self.fl[o + b] & COVER for b in range(4)):
                straddle.append(hex(s))
            else:
                island.append(s)
        isl = []
        for s in sorted(island):
            if isl and s - isl[-1][1] <= 64:
                isl[-1][1] = s
                isl[-1][2] += 1
            else:
                isl.append([s, s, 1])
        V["text_relocs_inside_insns"] = nin
        V["text_relocs_straddling_insn_boundary"] = len(straddle)
        V["text_relocs_straddle_samples"] = straddle[:20]
        V["text_reloc_sites_in_undecoded_islands"] = len(island)
        V["islands"] = [[hex(a), hex(b), n] for a, b, n in isl]
        # 2. jump tables
        runs_cov = 0
        for s, n in self.reloc_runs:
            if all(s + 4 * k in tbl_slots for k in range(n)):
                runs_cov += 1
        tabs = {}
        for jva, r in self.tables.items():
            tabs.setdefault(r["table"], r)
        bt = {r["bytetable"] for r in self.tables.values() if r["bytetable"] is not None}
        V["switch_dispatches"] = len(self.tables)
        V["distinct_dword_tables"] = len(tabs)
        V["distinct_byte_tables"] = len(bt)
        V["reloc_runs(>=2 slots)"] = len(self.reloc_runs)
        V["reloc_runs_fully_covered_by_resolved_tables"] = runs_cov
        V["reloc_runs_not_covered"] = [[hex(s), n] for s, n in self.reloc_runs
                                       if not all(s + 4 * k in tbl_slots for k in range(n))][:30]
        V["table_bound_method"] = dict(collections.Counter(r["how"] for r in self.tables.values()))
        V["table_slots_resolved"] = len(tbl_slots)
        both = sum(1 for o in range(self.TSIZE) if self.fl[o] & COVER and self.fl[o] & DATA)
        V["bytes_both_code_and_table_data"] = both
        dranges = []
        for r in self.tables.values():
            dranges.append((r["table"], r["table"] + 4 * r["n"]))
            if r["bytetable"] is not None:
                dranges.append((r["bytetable"], r["bytetable"] + r["nbytes"]))
        dranges.sort()
        starts = [a for a, _ in dranges]
        unk = []
        for a, t, b in self.text_mem_refs:
            k = bisect.bisect_right(starts, t) - 1
            if not (k >= 0 and dranges[k][0] <= t < dranges[k][1]):
                unk.append([hex(a), hex(t), b])
        V["text_memory_refs_outside_resolved_tables"] = len(unk)
        V["text_memory_refs_outside_samples"] = unk[:20]
        # 3. terminators
        V["blocks_with_unknown_terminator"] = sum(1 for k in self.bterm if k == T_INVALID)
        V["unknown_terminator_samples"] = [hex(self.bstart[i]) for i in range(len(self.bterm))
                                           if self.bterm[i] == T_INVALID][:20]
        # 4. Ghidra cross-check
        if len(self.ghidra):
            g = [x for x in self.ghidra if self.intext(x)]
            V["ghidra_entries"] = len(g)
            V["ghidra_not_decoded"] = [hex(x) for x in g if not self.fl[x - self.TS] & ISTART][:30]
            V["ghidra_not_decoded_count"] = sum(1 for x in g if not self.fl[x - self.TS] & ISTART)
            gl = [x for x in g if x in self.labels]
            V["ghidra_entries_classified_as_labels"] = len(gl)
            V["ghidra_label_samples"] = [[hex(x), self.labels[x]] for x in gl[:20]]
            gonly = [x for x in g if self.src[x - self.TS] == S_GHIDRA]
            V["ghidra_only_seeds"] = len(gonly)
            V["ghidra_only_samples"] = [hex(x) for x in gonly[:20]]
        V["anomalies"] = {k: len(v) for k, v in self.anom.items()}
        V["anomaly_samples"] = {k: [hex(x) if isinstance(x, int) else [hex(y) for y in x] for x in v[:15]]
                                for k, v in self.anom.items()}
        self.R["verify"] = V
        mem("verify done")

    def text_reloc_sites(self):
        for i in range(0, len(self.t2t), 2):
            yield self.t2t[i]
        # .text sites pointing to non-text targets
        o0 = self.TS - self.IMG
        for byte_i in range(o0 >> 3, (o0 + self.TSIZE) >> 3):
            b = self.rel[byte_i]
            while b:
                lb = b & -b
                s = self.IMG + byte_i * 8 + lb.bit_length() - 1
                b ^= lb
                if self.intext(s) and s not in self.t2t_sites:
                    yield s

    # ------------------------------------------------------------ output
    def write(self, outdir, tag):
        TS = self.TS
        entries = self.entries
        array.array("I", entries).tofile(open(os.path.join(outdir, "functions.u32"), "wb"))
        array.array("I", [e for e in entries if e in self.promoted or self.src[e - TS] & CANON]).tofile(
            open(os.path.join(outdir, "functions-canonical.u32"), "wb"))
        array.array("I", sorted(self.labels)).tofile(open(os.path.join(outdir, "labels.u32"), "wb"))
        ninsn_f = []
        with gzip.open(os.path.join(outdir, "manifest.txt.gz"), "wt", compresslevel=6) as f:
            f.write(f"# spore-native manifest v1\n# exe {self.R['exe']} sha256 {self.sha}\n")
            f.write("# F entry srcbits nblocks ninsn | B start end term succ.. | S = shared block | "
                    "T jmp table bytetable n how targets.. | L addr kind | D start end kind | X start end\n")
            for fi, e in enumerate(entries):
                blks = self.fblocks[fi]
                n = sum(self.bninsn[i] for i in blks)
                ninsn_f.append(n)
                sb_ = self.src[e - TS] | (0x100 if e in self.promoted else 0)
                f.write(f"F {e:x} {sb_:x} {len(blks)} {n}\n")
                for i in blks:
                    sb = self.bsucc_off[i]
                    se = self.bsucc_off[i + 1] if i + 1 < len(self.bstart) else len(self.succ)
                    tag_ = "S" if self.owner[i] == -2 else "B"
                    f.write(f"{tag_} {self.bstart[i]:x} {self.bend[i]:x} {TNAMES[self.bterm[i]]}"
                            + "".join(f" {t:x}" for t in self.succ[sb:se]) + "\n")
            for jva in sorted(self.tables):
                r = self.tables[jva]
                bt = f"{r['bytetable']:x}" if r["bytetable"] is not None else "-"
                f.write(f"T {jva:x} {r['table']:x} {bt} {r['n']} {r['how']}"
                        + "".join(f" {t:x}" for t in r["targets"]) + "\n")
            for va in sorted(self.labels):
                f.write(f"L {va:x} {self.labels[va]}\n")
            for jva, r in sorted(self.tables.items()):
                f.write(f"D {r['table']:x} {r['table'] + 4 * r['n']:x} jumptable\n")
                if r["bytetable"] is not None:
                    f.write(f"D {r['bytetable']:x} {r['bytetable'] + r['nbytes']:x} bytetable\n")
            for s, e in self.islands:
                f.write(f"D {s:x} {e:x} island\n")
            for s, e in self.dead:
                f.write(f"X {s:x} {e:x}\n")
        # size distribution
        ninsn_f.sort()
        q = lambda p: ninsn_f[min(len(ninsn_f) - 1, int(p * len(ninsn_f)))] if ninsn_f else 0  # noqa: E731
        self.R["function_size_insns"] = {"p50": q(.5), "p90": q(.9), "p99": q(.99), "max": ninsn_f[-1] if ninsn_f else 0,
                                         "total": sum(ninsn_f)}
        srcc = collections.Counter()
        canon = ghidra_only = unref = 0
        for e in entries:
            s_ = self.src[e - TS]
            if e in self.promoted or s_ & CANON:
                canon += 1
            elif s_ & S_UNREF:
                unref += 1
            else:
                ghidra_only += 1
            if e in self.promoted:
                srcc["promoted-shared-tail(" + ("+".join(n for b, n in SRC_NAMES.items() if s_ & b) or "jmp-only") + ")"] += 1
                continue
            srcc["+".join(n for b, n in SRC_NAMES.items() if s_ & b)] += 1
        self.R["function_count"] = {
            "canonical": canon, "unreferenced_code_functions": unref, "ghidra_only_seeded": ghidra_only,
            "all_entries_in_manifest": len(entries),
            "definition": "entries reached from the program entry, a direct call, a relocated code pointer "
                          "(data or address-taken immediate), an EH funclet record, or a shared tail jumped "
                          "to from several functions; excludes code nothing references and Ghidra-only seeds"}
        self.R["entries_by_source"] = dict(srcc.most_common())
        self.R["seh_scopetables"] = self.seh_tables
        self.R["peak_rss_mb"] = resource.getrusage(resource.RUSAGE_SELF).ru_maxrss // 1024
        json.dump(self.R, open(os.path.join(outdir, "report.json"), "w"), indent=1)
        mem(f"wrote {outdir}")


if __name__ == "__main__":
    main()
