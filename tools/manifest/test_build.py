"""Fixture test for build.py: a tiny PE32 we assemble by hand (no game bytes).

It covers: a direct call, a bounded dword switch, an MSVC-style byte-index switch whose bound is
checked on (index - 8) after a stack reload, a function reached only through a pointer in .rdata,
an unreferenced function after int3 padding, a data island in .text that .rdata points at, and
the canonical count. Nothing here is executed; build.py only decodes the bytes.

Usage:   PYTHONPATH=tools/.deps python3 -P tools/manifest/test_build.py
When:    after changing build.py or manifest.py.
"""
import os
import struct
import sys
import tempfile

sys.path.insert(0, os.path.dirname(os.path.abspath(__file__)))
import build  # noqa: E402

BASE, TEXT, RDATA, RELOC = 0x400000, 0x401000, 0x402000, 0x403000


class Asm:
    """Just enough of an assembler: raw bytes, labels, rel8/rel32 branches, abs32 (relocated)."""

    def __init__(self, org):
        self.org, self.items, self.labels = org, [], {}

    def b(self, *bs):
        self.items.append(("b", bytes(bs)))

    def label(self, n):
        self.items.append(("l", n))

    def rel8(self, op, n):
        self.items.append(("r8", op, n))

    def rel32(self, op, n):
        self.items.append(("r32", op, n))

    def abs32(self, prefix, n, add=0):
        self.items.append(("a32", prefix, n, add))

    def align(self, k, fill=0xCC):
        self.items.append(("al", k, fill))

    def assemble(self, ext):
        for _ in range(2):
            pc, out, fix = self.org, bytearray(), []
            for it in self.items:
                k = it[0]
                if k == "l":
                    self.labels[it[1]] = pc
                    continue
                if k == "b":
                    chunk = it[1]
                elif k == "al":
                    chunk = bytes([it[2]]) * ((-pc) % it[1])
                elif k == "r8":
                    t = self.labels.get(it[2], pc)
                    chunk = it[1] + struct.pack("<b", max(-128, min(127, t - (pc + len(it[1]) + 1))))
                elif k == "r32":
                    t = self.labels.get(it[2], ext.get(it[2], pc))
                    chunk = it[1] + struct.pack("<i", t - (pc + len(it[1]) + 4))
                else:
                    t = self.labels.get(it[2], ext.get(it[2], 0)) + it[3]
                    fix.append(pc + len(it[1]))
                    chunk = it[1] + struct.pack("<I", t & 0xFFFFFFFF)
                out += chunk
                pc += len(chunk)
        return bytes(out), fix


def make_pe(path):
    t = Asm(TEXT)
    # A (entry): call B; bounded dword switch on [esp+4]
    t.label("A")
    t.b(0x6A, 0x09)                    # push 9
    t.rel32(b"\xe8", "B")              # call B
    t.b(0x83, 0xC4, 0x04)              # add esp, 4
    t.b(0x8B, 0x44, 0x24, 0x04)        # mov eax, [esp+4]
    t.b(0x83, 0xF8, 0x02)              # cmp eax, 2
    t.rel8(b"\x77", "A_def")           # ja A_def
    t.abs32(b"\xff\x24\x85", "A_tab")  # jmp [eax*4 + A_tab]
    t.label("A_c0"); t.b(0xB8, 1, 0, 0, 0, 0xC3)    # mov eax,1; ret
    t.label("A_c1"); t.b(0xB8, 2, 0, 0, 0, 0xC3)
    t.label("A_c2"); t.b(0xB8, 3, 0, 0, 0, 0xC3)
    t.label("A_def"); t.b(0x33, 0xC0, 0xC3)          # xor eax,eax; ret
    t.align(4)
    t.label("A_tab")
    for c in ("A_c0", "A_c1", "A_c2"):
        t.abs32(b"", c)
    t.align(16)
    # B: byte-index switch, bound checked on (x - 8), index reloaded from the stack
    t.label("B")
    t.b(0x8B, 0x44, 0x24, 0x04)        # mov eax, [esp+4]
    t.b(0x83, 0xC0, 0xF8)              # add eax, -8
    t.b(0x83, 0xF8, 0x02)              # cmp eax, 2
    t.rel8(b"\x77", "B_def")           # ja B_def
    t.b(0x8B, 0x44, 0x24, 0x04)        # mov eax, [esp+4]
    t.abs32(b"\x0f\xb6\x88", "B_bt", -8)  # movzx ecx, byte [eax + B_bt-8]
    t.abs32(b"\xff\x24\x8d", "B_dt")   # jmp [ecx*4 + B_dt]
    t.label("B_c0"); t.b(0xB8, 7, 0, 0, 0, 0xC3)
    t.label("B_c1"); t.b(0xB8, 8, 0, 0, 0, 0xC3)
    t.label("B_def"); t.b(0x83, 0xC8, 0xFF, 0xC3)    # or eax,-1; ret
    t.align(4)
    t.label("B_dt")
    t.abs32(b"", "B_c0")
    t.abs32(b"", "B_c1")
    t.label("B_bt")
    t.b(0, 1, 0)                       # indices for x = 8, 9, 10
    t.align(16)
    # C: reached only from a pointer in .rdata (a one-slot "vtable")
    t.label("C")
    t.b(0x8B, 0x01, 0xC3)              # mov eax,[ecx]; ret
    t.align(16)
    # D: nothing references it
    t.label("D")
    t.b(0x55, 0x8B, 0xEC, 0x8B, 0x45, 0x08, 0x5D, 0xC3)  # push ebp; mov ebp,esp; mov eax,[ebp+8]; pop ebp; ret
    t.align(16)
    # ISL: a data table inside .text that .rdata points at (decodes as hlt/junk)
    t.label("ISL")
    t.b(*([0xF4, 0x03, 0xFF, 0xFF, 0x80, 0x00, 0x01, 0x00] * 4))
    t.align(16)
    text, tfix = t.assemble({})
    r = Asm(RDATA)
    r.b(*([0] * 20))                   # empty import descriptor list
    r.label("vt")
    r.abs32(b"", "C")
    r.abs32(b"", "ISL")
    rdata, rfix = r.assemble(t.labels)
    # .reloc: one block per page
    rel = b""
    for page, fixes in ((TEXT, tfix), (RDATA, rfix)):
        ents = [0x3000 | (f - page) for f in fixes]
        if len(ents) % 2:
            ents.append(0)
        rel += struct.pack("<II", page - BASE, 8 + 2 * len(ents)) + struct.pack("<%dH" % len(ents), *ents)
    secs = [(b".text", TEXT, text, 0x60000020), (b".rdata", RDATA, rdata, 0x40000040),
            (b".reloc", RELOC, rel, 0x42000040)]
    hdr = bytearray(0x200)
    hdr[0:2] = b"MZ"
    struct.pack_into("<I", hdr, 0x3C, 0x40)
    hdr[0x40:0x44] = b"PE\0\0"
    struct.pack_into("<HHIIIHH", hdr, 0x44, 0x14C, len(secs), 0, 0, 0, 0xE0, 0x102)
    o = 0x58
    struct.pack_into("<HBBIIIIII", hdr, o, 0x10B, 9, 0, 0x200, 0x400, 0, TEXT - BASE, TEXT - BASE, RDATA - BASE)
    struct.pack_into("<III", hdr, o + 28, BASE, 0x1000, 0x200)
    struct.pack_into("<I", hdr, o + 56, 0x4000)       # SizeOfImage
    struct.pack_into("<I", hdr, o + 60, 0x200)        # SizeOfHeaders
    struct.pack_into("<I", hdr, o + 92, 16)           # NumberOfRvaAndSizes
    struct.pack_into("<II", hdr, o + 96 + 8 * 1, RDATA - BASE, 20)
    struct.pack_into("<II", hdr, o + 96 + 8 * 5, RELOC - BASE, len(rel))
    so = o + 0xE0
    raw = 0x200
    body = b""
    for i, (nm, va, data, ch) in enumerate(secs):
        rsz = (len(data) + 0x1FF) & ~0x1FF
        struct.pack_into("<8sIIIIIIHHI", hdr, so + 40 * i, nm, len(data), va - BASE, rsz, raw, 0, 0, 0, 0, ch)
        body += data + b"\0" * (rsz - len(data))
        raw += rsz
    open(path, "wb").write(bytes(hdr) + body)
    return t.labels


def main():
    fails = []

    def check(cond, msg):
        print(("ok   " if cond else "FAIL ") + msg)
        if not cond:
            fails.append(msg)

    with tempfile.TemporaryDirectory() as d:
        exe = os.path.join(d, "fixture.exe")
        L = make_pe(exe)
        M = build.Manifest(exe, None, False)
        M.unref_pass = True
        M.load_relocs()
        M.parse_eh()
        M.seed()
        M.descend()
        M.blocks_pass()
        M.functions_pass()
        M.dead_code(None)
        M.verify()
        M.write(d, "fixture")
        V, R = M.R["verify"], M.R
        ents = set(M.entries)
        check({L["A"], L["B"], L["C"]} <= ents, "A (entry), B (call target), C (pointer in .rdata) are functions")
        check(L["D"] in ents and M.src[L["D"] - M.TS] & build.S_UNREF, "D is decoded as unreferenced code")
        check(R["function_count"]["canonical"] == 3, "canonical count is 3, got %d" % R["function_count"]["canonical"])
        ta = [r for j, r in M.tables.items() if r["table"] == L["A_tab"]]
        check(len(ta) == 1 and ta[0]["n"] == 3 and ta[0]["how"] == "cmp"
              and ta[0]["targets"] == [L["A_c0"], L["A_c1"], L["A_c2"]], "dword switch in A: 3 targets via cmp")
        tb = [r for j, r in M.tables.items() if r["table"] == L["B_dt"]]
        check(len(tb) == 1 and tb[0]["how"] == "cmp-biased" and tb[0]["bytetable"] == L["B_bt"]
              and tb[0]["nbytes"] == 3 and tb[0]["n"] == 2, "biased byte-index switch in B resolved: %s" %
              ({k: tb[0][k] for k in ("how", "n", "nbytes")} if tb else None))
        check(V["reloc_code_ptrs_not_block_start"] == 0, "every relocated code pointer is a block start")
        check([x[2] for x in V["reloc_ptrs_to_data_islands_in_text"]] == [hex(L["ISL"])],
              "the island is reported as data, not code")
        check(V["blocks_with_unknown_terminator"] == 0, "every block has a known terminator")
        check(V["reloc_runs_fully_covered_by_resolved_tables"] == V["reloc_runs(>=2 slots)"],
              "every run of .text->.text fixups is a resolved table")
        check(V["text_memory_refs_outside_resolved_tables"] == 0, "byte-table operand inside a resolved table")
        check(os.path.getsize(os.path.join(d, "manifest.txt.gz")) > 0, "manifest written")
    print("FAILED %d" % len(fails) if fails else "all passed")
    sys.exit(1 if fails else 0)


if __name__ == "__main__":
    main()
