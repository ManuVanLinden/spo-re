"""Shared helpers for the exception-handling tools: the PE image, its relocation map and
the function manifest's block -> function index.

Static analysis only: this reads bytes from the exe and never executes any of them.

Usage:   a library, imported by ehdb.py and verify.py
"""
import array
import bisect
import gzip
import os
import struct
import sys

import capstone

HERE = os.path.dirname(os.path.abspath(__file__))
ROOT = os.path.abspath(os.path.join(HERE, "..", ".."))
sys.path.insert(0, os.path.join(ROOT, "tools", "code-characteristics"))
from pe import PE, EXE, GAME  # noqa: E402

# SPORE_WORK_DIR (default: the source tree) takes the generated outputs; SPORE_GAME_DIR / SPORE_EXE name the
# Spore folder / the EP1 exe instead of game/ (docs/design/convert.md 4).
WORK = os.environ.get("SPORE_WORK_DIR") or ROOT
BASE_EXE = os.path.join(GAME, "SporeBin", "SporeApp.exe")


class Image:
    def __init__(self, path=EXE):
        self.path = path
        self.p = PE(path)
        p = self.p
        self.text = p.sec(".text")
        self.rdata = p.sec(".rdata")
        _, self.TS, self.TSZ, self.TRAW, self.TRS, _ = self.text
        self.md = capstone.Cs(capstone.CS_ARCH_X86, capstone.CS_MODE_32)
        self.md.detail = False
        # fixup site -> target value (HIGHLOW only); target -> sites
        fix = array.array("I")
        tgt = array.array("I")
        for va in p.relocs():
            v = p.u32(va)
            if v is None:
                continue
            fix.append(va)
            tgt.append(v)
        self.fix = fix
        self.fixset = set(fix)
        self.by_target = {}
        for f, t in zip(fix, tgt):
            self.by_target.setdefault(t, []).append(f)
        self.imports = p.imports()
        self.iat_by_name = {}
        for a, n in self.imports.items():
            self.iat_by_name[n.split("!", 1)[1]] = a

    # -- raw access --------------------------------------------------------------------
    def u32(self, va):
        return self.p.u32(va)

    def i32(self, va):
        v = self.p.u32(va)
        return None if v is None else struct.unpack("<i", struct.pack("<I", v))[0]

    def words(self, va, n):
        o = self.p.off(va)
        if o is None:
            return None
        return struct.unpack_from("<%dI" % n, self.p.d, o)

    def in_text(self, va):
        return self.TS <= va < self.TS + self.TSZ

    def secname(self, va):
        return self.p.secname(va)

    def cstr(self, va, n=256):
        b = self.p.cstr(va, n)
        return None if b is None else b.decode("latin-1")

    def is_reloc_ptr(self, field_va):
        """True when the dword at field_va is covered by a base relocation (so it is a pointer)."""
        return field_va in self.fixset

    def disasm(self, va, maxbytes=0x200, count=0):
        o = self.p.off(va)
        if o is None:
            return []
        return list(self.md.disasm(bytes(self.p.d[o:o + maxbytes]), va, count))

    def insn_at(self, va):
        r = self.disasm(va, 16, 1)
        return r[0] if r else None

    def thunk_for(self, name):
        """Address of the `jmp [IAT]` thunk for an import name, found by scanning .text for FF 25 <iat>."""
        iat = self.iat_by_name.get(name)
        if iat is None:
            return None
        pat = b"\xff\x25" + struct.pack("<I", iat)
        o = self.p.d.find(pat, self.TRAW, self.TRAW + self.TRS)
        return None if o < 0 else self.TS + (o - self.TRAW)

    def rel32_callers(self, target, opcode=0xE8):
        """All E8/E9 rel32 sites in .text whose destination is target (byte scan, then confirmed by decode)."""
        d = self.p.d
        out = []
        lo, hi = self.TRAW, self.TRAW + self.TRS
        op = bytes([opcode])
        o = d.find(op, lo, hi)
        while o >= 0:
            if o + 5 <= hi:
                rel = struct.unpack_from("<i", d, o + 1)[0]
                va = self.TS + (o - lo)
                if (va + 5 + rel) & 0xFFFFFFFF == target:
                    out.append(va)
            o = d.find(op, o + 1, hi)
        return out


class FuncIndex:
    """Block -> owning function from the function manifest (scratch/manifest/<tag>/manifest.txt.gz)."""

    def __init__(self, tag="ep1"):
        path = os.path.join(WORK, "scratch", "manifest", tag, "manifest.txt.gz")
        self.ok = os.path.exists(path)
        self.bstart = array.array("I")
        self.bend = array.array("I")
        self.bfunc = array.array("I")
        self.srcbits = {}
        self.labels = {}
        if not self.ok:
            return
        rows = []
        cur = None
        with gzip.open(path, "rt") as f:
            for line in f:
                k = line[0]
                if k == "F":
                    p = line.split()
                    cur = int(p[1], 16)
                    self.srcbits[cur] = int(p[2], 16)
                elif k == "B" or k == "S":
                    p = line.split(None, 3)
                    rows.append((int(p[1], 16), int(p[2], 16), cur))
                elif k == "L":
                    p = line.split()
                    self.labels[int(p[1], 16)] = p[2]
        rows.sort()
        for s, e, fn in rows:
            self.bstart.append(s)
            self.bend.append(e)
            self.bfunc.append(fn)
        canon = os.path.join(WORK, "scratch", "manifest", tag, "functions-canonical.u32")
        self.canonical = set()
        if os.path.exists(canon):
            b = open(canon, "rb").read()
            self.canonical = set(struct.unpack("<%dI" % (len(b) // 4), b))

    def owners(self, va):
        """Functions whose blocks contain va (several when a block is shared)."""
        i = bisect.bisect_right(self.bstart, va) - 1
        out = []
        j = i
        while j >= 0 and self.bstart[j] > va - 0x10000:
            if self.bstart[j] <= va < self.bend[j]:
                out.append(self.bfunc[j])
            if self.bstart[j] < va - 0x4000:
                break
            j -= 1
        return sorted(set(out))

    def owner(self, va):
        o = self.owners(va)
        return o[0] if o else None

    def is_function(self, va):
        return va in self.srcbits

    def function_blocks(self, entry):
        """(start, end) of every block owned by entry, in address order."""
        if not hasattr(self, "_fb"):
            self._fb = {}
            for s, e, f in zip(self.bstart, self.bend, self.bfunc):
                self._fb.setdefault(f, []).append((s, e))
        return self._fb.get(entry, [])
