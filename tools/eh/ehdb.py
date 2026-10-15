"""Build the exception-handling database for SporeApp.exe (static analysis only).

Parses, from the exe's bytes and its relocation table:
  * every MSVC C++ EH FuncInfo record (magic 0x1993052x) with its unwind map, try map and
    HandlerType arrays, the TypeDescriptors they name, and the handler stubs
    `mov eax, FuncInfo; jmp __CxxFrameHandler3` that point at them;
  * the code that registers each stub in the fs:[0] chain (owner function, registration form);
  * catch funclets and the continuation addresses they return;
  * every _CxxThrowException site with its ThrowInfo / CatchableTypeArray / CatchableType chain;
  * the _except_handler3 and _except_handler4 (__SEH_prolog4) frames with their scope tables;
  * every _setjmp3 and longjmp call site;
  * the other fs:[0] writers, classified.

Usage:   PYTHONPATH=tools/.deps python3 -P tools/eh/ehdb.py [--base]     (--base: the base game's exe)
Inputs:  the exe and the function manifest (scratch/manifest/<tag>)
Outputs: scratch/eh/<tag>/ehdb.json          full database (addresses and derived structure; git-ignored)
         data/eh/funcinfo-summary.json       counts, checks and short per-item tables (ep1 only)
When:    the converter's eh stage, after the manifest; verify.py runs next.
Environment: SPORE_WORK_DIR, SPORE_GAME_DIR and SPORE_EXE move the outputs and name the exe (docs/design/convert.md 4).
"""
import collections
import json
import os
import re
import struct
import sys

HERE = os.path.dirname(os.path.abspath(__file__))
sys.path.insert(0, HERE)
from ehimage import Image, FuncIndex, ROOT, WORK, BASE_EXE, EXE  # noqa: E402

FH3_MAGICS = (0x19930520, 0x19930521, 0x19930522)


def h(x):
    return None if x is None else "0x%x" % x


class EHDB:
    def __init__(self, im, fi):
        self.im = im
        self.fi = fi
        self.thunk = {n: im.thunk_for(n) for n in (
            "__CxxFrameHandler3", "_CxxThrowException", "_except_handler3", "_except_handler4_common",
            "_setjmp3", "longjmp", "RaiseException", "?terminate@@YAXXZ", "_purecall")}
        self.problems = []

    def problem(self, kind, va, msg=""):
        self.problems.append({"kind": kind, "at": h(va), "msg": msg})

    # ------------------------------------------------------------------ TypeDescriptors
    def typedesc(self, va):
        if not va:
            return None
        vft, spare = self.im.words(va, 2)
        name = self.im.cstr(va + 8, 512)
        return {"va": h(va), "vftable": h(vft), "name": name}

    # ------------------------------------------------------------------ FuncInfo
    def scan_funcinfo(self):
        im = self.im
        _, RS, RSZ, RRAW, RRS, _ = im.rdata
        d = im.p.d
        out = {}
        for magic in FH3_MAGICS:
            pat = struct.pack("<I", magic)
            o = d.find(pat, RRAW, RRAW + RRS)
            while o >= 0:
                if (o - RRAW) % 4 == 0:
                    va = RS + (o - RRAW)
                    rec = self.parse_funcinfo(va)
                    if rec is not None:
                        out[va] = rec
                o = d.find(pat, o + 1, RRAW + RRS)
        self.funcinfo = out
        return out

    def parse_funcinfo(self, va):
        im = self.im
        w = im.words(va, 9)
        magic, maxst, punw, ntry, ptry, nip, pip = w[:7]
        estl, flags = (w[7], w[8]) if magic >= 0x19930521 else (0, 0)
        if magic == 0x19930521:
            flags = 0
        # plausibility: pointer fields are relocated, counts small
        if maxst > 100000 or ntry > 10000:
            return None
        if maxst and not im.is_reloc_ptr(va + 8):
            return None
        if ntry and not im.is_reloc_ptr(va + 16):
            return None
        rec = {"va": h(va), "magic": h(magic), "maxState": maxst, "nTryBlocks": ntry,
               "nIPMapEntries": nip, "pIPtoStateMap": h(pip), "pESTypeList": h(estl), "EHFlags": flags,
               "unwind": [], "try": []}
        for i in range(maxst):
            to, act = struct.unpack("<iI", struct.pack("<II", *im.words(punw + 8 * i, 2)))
            rec["unwind"].append([to, h(act) if act else None])
            if not (-1 <= to < i):
                self.problem("unwind-toState-not-below-index", va, "state %d -> %d" % (i, to))
            if act and not im.is_reloc_ptr(punw + 8 * i + 4):
                self.problem("unwind-action-not-relocated", va, "state %d" % i)
        for t in range(ntry):
            lo, hi, chi, ncatch, parr = struct.unpack("<iiiII", struct.pack("<5I", *im.words(ptry + 20 * t, 5)))
            tb = {"tryLow": lo, "tryHigh": hi, "catchHigh": chi, "catches": []}
            if not (0 <= lo <= hi < chi < maxst):
                self.problem("try-state-range", va, "try %d: %d..%d catchHigh %d max %d" % (t, lo, hi, chi, maxst))
            for c in range(ncatch):
                adj, ptype, disp, addr = struct.unpack("<IIiI", struct.pack("<4I", *im.words(parr + 16 * c, 4)))
                td = self.typedesc(ptype)
                tb["catches"].append({"adjectives": h(adj), "type": td["name"] if td else None,
                                      "typedesc": h(ptype) if ptype else None, "dispCatchObj": disp,
                                      "handler": h(addr)})
            rec["try"].append(tb)
        # FH3 requires inner try blocks before outer ones: a later block must not nest inside an earlier one
        for a in range(ntry):
            for b in range(a + 1, ntry):
                A, B = rec["try"][a], rec["try"][b]
                if A["tryLow"] <= B["tryLow"] and B["catchHigh"] <= A["catchHigh"] and (A != B):
                    self.problem("try-order-outer-before-inner", va, "%d contains %d" % (a, b))
        return rec

    # ------------------------------------------------------------------ handler stubs
    def scan_stubs(self):
        """`B8 <FuncInfo> E9 <rel32 -> __CxxFrameHandler3 thunk>` anywhere in .text."""
        im = self.im
        tgt = self.thunk["__CxxFrameHandler3"]
        stubs = {}
        for jva in im.rel32_callers(tgt, 0xE9):
            sva = jva - 5
            o = im.p.off(sva)
            if im.p.d[o] == 0xB8:
                fiva = im.u32(sva + 1)
                stubs[sva] = fiva
            else:
                self.problem("framehandler-jmp-without-mov-eax", jva)
        self.stubs = stubs
        return stubs

    # ------------------------------------------------------------------ registrations
    def registration(self, stub):
        """Where and how the stub is installed in the fs:[0] chain."""
        im, fi = self.im, self.fi
        regs = []
        for site in im.by_target.get(stub, []):
            ins = im.insn_at(site - 1)
            form = None
            if ins is not None and ins.address == site - 1 and ins.mnemonic == "push":
                form = "push-stub"
            else:
                ins = im.insn_at(site - 1)
                if ins is not None and ins.mnemonic == "mov" and ins.op_str.startswith("eax,"):
                    # mov eax, stub ; call __EH_prolog3*
                    nxt = im.insn_at(site + 4)
                    if nxt is not None and nxt.mnemonic == "call":
                        form = "eh_prolog:" + nxt.op_str
                    else:
                        form = "mov-eax-stub"
                else:
                    form = "other"
            regs.append({"site": h(site - 1), "form": form, "owner": h(fi.owner(site - 1))})
        return regs

    # ------------------------------------------------------------------ funclet decoding
    def block_insns(self, s, e):
        return self.im.disasm(s, e - s)

    def catch_continuations(self, entry):
        """Continuations a catch funclet returns in eax, by dataflow over its manifest blocks."""
        fi, im = self.fi, self.im
        blocks = fi.function_blocks(entry)
        rethrow = False
        throws_new = False
        calls = []
        if not blocks:
            # manifest missing: linear decode
            blocks = [(entry, entry + 0x400)]
        bmap = {s: e for s, e in blocks}
        conts = set()
        unknown_ret = 0
        seen = set()
        work = [(entry, None)]
        while work:
            s, eax = work.pop()
            if (s, eax) in seen or s not in bmap:
                continue
            seen.add((s, eax))
            last = None
            pushes = []
            noreturn = False
            for ins in self.block_insns(s, bmap[s]):
                last = ins
                if ins.mnemonic == "mov" and re.match(r"eax, 0x[0-9a-f]+$", ins.op_str):
                    eax = int(ins.op_str[5:], 16)
                elif ins.mnemonic == "push":
                    pushes.append(ins.op_str)
                elif ins.mnemonic == "call":
                    try:
                        t = int(ins.op_str, 16)
                    except ValueError:
                        t = None
                    calls.append(h(t) if t else ins.op_str)
                    if t == self.thunk["_CxxThrowException"]:
                        if pushes[-2:] == ["0", "0"]:
                            rethrow = True
                        else:
                            throws_new = True
                        noreturn = True
                        break
                    eax = None
                    pushes = []
                elif ins.op_str.startswith("eax") and ins.mnemonic not in ("cmp", "test", "push"):
                    eax = None
            if last is None or noreturn:
                continue
            m = last.mnemonic
            succ = []
            if m == "ret":
                if eax is None:
                    unknown_ret += 1
                else:
                    conts.add(eax)
                continue
            if m == "jmp":
                try:
                    succ = [int(last.op_str, 16)]
                except ValueError:
                    succ = []
            elif m.startswith("j"):
                succ = [int(last.op_str, 16), last.address + last.size]
            else:
                succ = [last.address + last.size]
            for t in succ:
                if t in bmap:
                    work.append((t, eax))
                elif im.in_text(t):
                    # a jmp into a shared epilogue outside this funclet's blocks: decode a little
                    for i2 in im.disasm(t, 64):
                        if i2.mnemonic == "mov" and re.match(r"eax, 0x[0-9a-f]+$", i2.op_str):
                            eax = int(i2.op_str[5:], 16)
                        if i2.mnemonic == "ret":
                            if eax is not None:
                                conts.add(eax)
                            break
        return sorted(conts), rethrow, unknown_ret, calls, throws_new

    # ------------------------------------------------------------------ throw sites
    def push_args_before(self, call_va, n):
        """Operands of the last n pushes before call_va within its manifest block (most recent last)."""
        im, fi = self.im, self.fi
        start = None
        i = __import__("bisect").bisect_right(fi.bstart, call_va) - 1
        while i >= 0:
            if fi.bstart[i] <= call_va < fi.bend[i]:
                start = fi.bstart[i]
                break
            i -= 1
            if fi.bstart[i] < call_va - 0x1000:
                break
        if start is None:
            start = call_va - 32
        pushes = []
        for ins in im.disasm(start, call_va - start):
            if ins.address >= call_va:
                break
            if ins.mnemonic == "push":
                pushes.append((ins.address, ins.op_str))
            elif ins.mnemonic == "call":
                pushes = []
        return pushes[-n:]

    def throwinfo(self, va):
        im = self.im
        attr, punwind, pfwd, pcta = im.words(va, 4)
        cta = []
        n = im.u32(pcta)
        for k in range(n):
            pct = im.u32(pcta + 4 + 4 * k)
            props, ptype, mdisp, pdisp, vdisp, size, copy = im.words(pct, 7)
            td = self.typedesc(ptype)
            cta.append({"ct": h(pct), "properties": props, "type": td["name"] if td else None,
                        "typedesc": h(ptype), "thisDisp": [mdisp, struct.unpack("<i", struct.pack("<I", pdisp))[0],
                                                            vdisp],
                        "size": size, "copyFunction": h(copy) if copy else None})
        return {"va": h(va), "attributes": attr, "destructor": h(punwind) if punwind else None,
                "catchable": cta}

    def scan_throws(self):
        im = self.im
        out = []
        tis = {}
        for site in im.rel32_callers(self.thunk["_CxxThrowException"]):
            args = self.push_args_before(site, 2)
            rec = {"site": h(site), "owner": h(self.fi.owner(site))}
            if len(args) == 2:
                ti_op, obj_op = args[0][1], args[1][1]
                rec["pushes"] = [ti_op, obj_op]
                if ti_op == "0" and obj_op == "0":
                    rec["kind"] = "rethrow"
                elif re.match(r"0x[0-9a-f]+$", ti_op):
                    tiva = int(ti_op, 16)
                    rec["kind"] = "throw"
                    rec["throwinfo"] = h(tiva)
                    if tiva not in tis:
                        tis[tiva] = self.throwinfo(tiva)
                    rec["type"] = tis[tiva]["catchable"][0]["type"] if tis[tiva]["catchable"] else None
                else:
                    rec["kind"] = "unresolved"
            else:
                rec["kind"] = "unresolved"
            if rec["kind"] == "unresolved":
                self.problem("throw-site-unresolved", site)
            out.append(rec)
        self.throws = out
        self.throwinfos = tis
        return out

    # ------------------------------------------------------------------ SEH
    def scan_seh3(self):
        im, fi = self.im, self.fi
        frames = []
        for site in im.by_target.get(self.thunk["_except_handler3"], []):
            push = im.insn_at(site - 1)
            # preceding `push scopetable` is 5 bytes earlier
            st_ins = im.insn_at(site - 6)
            st = int(st_ins.op_str, 16) if st_ins is not None and st_ins.mnemonic == "push" else None
            owner = fi.owner(site - 1)
            levels = self.trylevels(owner)
            recs = self.scope_entries(st, 0, -1)
            frames.append({"kind": "_except_handler3", "push_site": h(site - 1), "owner": h(owner),
                           "scopetable": h(st), "trylevels_written": sorted(levels), "scopes": recs})
        return frames

    def scope_entries(self, st, first, outside):
        """Walk a scope table from `first` while entries look valid: enclosing level in [outside, k-1],
        filter 0 (a __finally) or a relocated .text pointer, handler a relocated .text pointer."""
        im = self.im
        recs = []
        k = 0
        while k < 64:
            a = st + first + 12 * k
            w = im.words(a, 3)
            if w is None:
                break
            prev = struct.unpack("<i", struct.pack("<I", w[0]))[0]
            filt, hand = w[1], w[2]
            if not (outside <= prev < k) or (prev < -1 and prev != outside):
                break
            if not (im.is_reloc_ptr(a + 8) and im.in_text(hand)):
                break
            if filt and not (im.is_reloc_ptr(a + 4) and im.in_text(filt)):
                break
            recs.append({"enclosing": prev, "filter": h(filt) if filt else None, "handler": h(hand),
                         "kind": "__finally" if filt == 0 else "__except"})
            k += 1
        return recs

    def trylevels(self, owner, slot="[ebp - 4]"):
        """Immediate values stored to the try-level slot in the owner's blocks."""
        vals = set()
        if owner is None:
            return vals
        for s, e in self.fi.function_blocks(owner):
            for ins in self.block_insns(s, e):
                if ins.mnemonic == "and" and ins.op_str == "dword ptr " + slot + ", 0":
                    vals.add(0)
                if ins.mnemonic == "mov" and slot in ins.op_str and ins.op_str.startswith(("dword ptr " + slot,)):
                    v = ins.op_str.split(",")[-1].strip()
                    try:
                        v = int(v, 16)
                    except ValueError:
                        continue
                    if v != 0xFFFFFFFE and v != 0xFFFFFFFF:
                        vals.add(v)
        return vals

    def scan_seh4(self):
        """__SEH_prolog4 users: `push scopetable4; push localsize; call __SEH_prolog4`."""
        im, fi = self.im, self.fi
        h4 = self.thunk["_except_handler4_common"]
        frames = []
        # the local _except_handler4 is the function calling the common thunk
        local_h4 = set()
        for c in im.rel32_callers(h4):
            local_h4.add(fi.owner(c))
        uses = []   # (form, owner, site, scopetable)
        for lh in local_h4:
            for site in im.by_target.get(lh, []):
                st_ins = im.insn_at(site - 6)
                if st_ins is not None and st_ins.mnemonic == "push" and re.match(r"0x[0-9a-f]+$", st_ins.op_str):
                    # inline frame: push -2; push scopetable4; push _except_handler4
                    uses.append(("inline", fi.owner(site - 1), site - 1, int(st_ins.op_str, 16)))
                else:
                    # __SEH_prolog4: callers do push scopetable4; push localsize; call prolog
                    prolog = fi.owner(site - 1)
                    for call in im.rel32_callers(prolog):
                        args = self.push_args_before(call, 2)
                        st = int(args[1][1], 16) if len(args) == 2 and re.match(r"0x[0-9a-f]+$", args[1][1]) \
                            else None
                        uses.append(("__SEH_prolog4@" + h(prolog), fi.owner(call), call, st))
        for form, owner, site, st in sorted(uses, key=lambda u: u[2]):
            levels = self.trylevels(owner, "[ebp - 4]")
            recs = []
            if st:
                recs = self.scope_entries(st, 16, -2)
            else:
                self.problem("seh4-scopetable-unresolved", site)
            frames.append({"kind": "_except_handler4", "form": form, "site": h(site), "owner": h(owner),
                           "scopetable": h(st), "trylevels_written": sorted(levels), "scopes": recs})
        return frames, sorted(h(x) for x in local_h4 if x)

    # ------------------------------------------------------------------ setjmp / longjmp
    def scan_setjmp(self):
        im, fi = self.im, self.fi
        sj = []
        for site in im.rel32_callers(self.thunk["_setjmp3"]):
            args = self.push_args_before(site, 2)
            sj.append({"site": h(site), "resume": h(site + 5), "owner": h(fi.owner(site)),
                       "args": [a[1] for a in args]})
        lj = []
        iat = im.iat_by_name.get("longjmp")
        for site in im.by_target.get(iat, []):
            ins = im.insn_at(site - 2)
            args = self.push_args_before(site - 2, 2)
            lj.append({"site": h(site - 2), "insn": (ins.mnemonic + " " + ins.op_str) if ins else None,
                       "owner": h(fi.owner(site - 2)), "args": [a[1] for a in args]})
        return sj, lj

    # ------------------------------------------------------------------ fs:[0] writers
    def scan_fs0(self):
        """Classify every `mov fs:[0], reg` by what the function registered."""
        im = self.im
        d = im.p.d
        counts = collections.Counter()
        sites = 0
        o = d.find(b"\x64", im.TRAW, im.TRAW + im.TRS)
        writers = collections.Counter()
        while o >= 0:
            va = im.TS + (o - im.TRAW)
            ins = im.insn_at(va)
            if ins is not None and ins.address == va and ins.mnemonic == "mov" and \
                    ins.op_str.startswith("dword ptr fs:[0],"):
                sites += 1
                writers[ins.op_str.split(",")[1].strip()] += 1
            o = d.find(b"\x64", o + 1, im.TRAW + im.TRS)
        return sites, writers

    # ------------------------------------------------------------------ build
    def build(self):
        fi, im = self.fi, self.im
        self.scan_funcinfo()
        self.scan_stubs()
        db = {"exe": os.path.relpath(im.path, ROOT), "thunks": {k: h(v) for k, v in self.thunk.items()}}
        # stubs -> funcinfo
        stub_rows = []
        fi_to_stubs = collections.defaultdict(list)
        for s, f in sorted(self.stubs.items()):
            fi_to_stubs[f].append(s)
            if f not in self.funcinfo:
                self.problem("stub-funcinfo-unparsed", s, h(f))
        recs = []
        for va in sorted(self.funcinfo):
            r = self.funcinfo[va]
            r["stubs"] = [h(s) for s in fi_to_stubs.get(va, [])]
            regs = []
            for s in fi_to_stubs.get(va, []):
                regs += self.registration(s)
            r["registrations"] = regs
            for tb in r["try"]:
                for c in tb["catches"]:
                    hv = int(c["handler"], 16)
                    conts, rethrow, unk, calls, tnew = self.catch_continuations(hv)
                    c["throws_new"] = tnew
                    c["continuations"] = [h(x) for x in conts]
                    c["rethrows"] = rethrow
                    c["returns_unknown"] = unk
                    c["calls"] = calls
                    c["continuation_owner"] = [h(fi.owner(x)) for x in conts]
            recs.append(r)
        db["funcinfo"] = recs
        db["stubs_without_funcinfo"] = [h(s) for s, f in self.stubs.items() if f not in self.funcinfo]
        db["funcinfo_without_stub"] = [h(v) for v in self.funcinfo if v not in fi_to_stubs]
        db["throws"] = self.scan_throws()
        db["throwinfo"] = list(self.throwinfos.values())
        seh3 = self.scan_seh3()
        seh4, local_h4 = self.scan_seh4()
        db["seh"] = seh3 + seh4
        db["except_handler4_local"] = local_h4
        sj, lj = self.scan_setjmp()
        db["setjmp"] = sj
        db["longjmp"] = lj
        n, writers = self.scan_fs0()
        db["fs0_writes"] = {"sites": n, "by_source": dict(writers)}
        db["problems"] = self.problems
        return db


def main():
    base = "--base" in sys.argv
    tag = "base" if base else "ep1"
    im = Image(BASE_EXE if base else EXE)
    fi = FuncIndex(tag)
    db = EHDB(im, fi).build()
    out = os.path.join(WORK, "scratch", "eh", tag)
    os.makedirs(out, exist_ok=True)
    with open(os.path.join(out, "ehdb.json"), "w") as f:
        json.dump(db, f, indent=1)
    print("wrote", os.path.join(out, "ehdb.json"), "funcinfo", len(db["funcinfo"]), "problems", len(db["problems"]))


if __name__ == "__main__":
    main()
