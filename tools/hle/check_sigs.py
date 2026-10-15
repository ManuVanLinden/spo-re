"""Cross-check tools/hle/sigs.py against the game's call sites (static only; nothing is run).

For each import call site recorded in data/imports.json (`call [IAT]`, and `mov reg,[IAT]` ...
`call reg`), decode the function-manifest block that contains the call and measure:
  pushes  4-byte slots pushed since the previous call in that block (push r/m/imm = 1,
          `sub esp, N` = N/4). A lower bound when arguments were pushed in an earlier block.
  cleanup the `add esp, N` (or `pop reg` run) right after the call: the caller cleans up -> cdecl.
Then per import:
  stdcall: no site may clean up; the most common push count should equal the slot count.
  cdecl:   sites with args usually clean up (not required: MSVC may defer clean-up).
Writes data/hle/sig-check.json and prints mismatches.

Usage:   PYTHONPATH=tools/.deps python3 -P tools/hle/check_sigs.py
Inputs:  game/SporebinEP1/SporeApp.exe, data/imports.json and scratch/manifest/ep1/manifest.txt.gz (the manifest
         and imports stages run by hand in the source tree, without SPORE_WORK_DIR)
Outputs: data/hle/sig-check.json (git-ignored) and the mismatches on stdout
When:    after changing a convention or slot count in sigs.py.
"""
import bisect
import collections
import gzip
import json
import os
import struct
import sys

import capstone
from capstone import x86

HERE = os.path.dirname(os.path.abspath(__file__))
ROOT = os.path.abspath(os.path.join(HERE, "..", ".."))
sys.path.insert(0, HERE)
import sigs  # noqa: E402

EXE = os.path.join(ROOT, "game", "SporebinEP1", "SporeApp.exe")
MANIFEST = os.path.join(ROOT, "scratch", "manifest", "ep1", "manifest.txt.gz")


def load_pe(path):
    d = open(path, "rb").read()
    pe = struct.unpack_from("<I", d, 0x3C)[0]
    nsec = struct.unpack_from("<H", d, pe + 6)[0]
    optsz = struct.unpack_from("<H", d, pe + 20)[0]
    base = struct.unpack_from("<I", d, pe + 24 + 28)[0]
    secs = []
    for i in range(nsec):
        _, vsz, va, rsz, raw = struct.unpack_from("<8sIIII", d, pe + 24 + optsz + 40 * i)
        secs.append((base + va, base + va + max(vsz, rsz), raw, rsz))
    return d, secs


def reader(d, secs):
    def read(va, n):
        for lo, hi, raw, rsz in secs:
            if lo <= va < hi:
                o = va - lo
                return d[raw + o: raw + min(o + n, rsz)]
        return b""
    return read


def load_blocks():
    """Sorted block starts/ends from the manifest (streamed; only two int arrays kept)."""
    starts, ends = [], []
    with gzip.open(MANIFEST, "rt") as f:
        for line in f:
            if line[0] in "BS":
                p = line.split(None, 3)
                starts.append(int(p[1], 16))
                ends.append(int(p[2], 16))
    order = sorted(range(len(starts)), key=starts.__getitem__)
    return [starts[i] for i in order], [ends[i] for i in order]


def main():
    imports, _ = sigs.parse()
    data = json.load(open(os.path.join(ROOT, "data", "imports.json")))
    d, secs = load_pe(EXE)
    read = reader(d, secs)
    starts, ends = load_blocks()
    md = capstone.Cs(capstone.CS_ARCH_X86, capstone.CS_MODE_32)
    md.detail = True

    iat_slots = {}
    for rec in data["imports"]:
        sg = imports[(rec["dll"], rec["name"] or rec["ordinal_name_inferred"])]
        if sg["cc"] in ("s", "t"):
            iat_slots["dword ptr [%s]" % hex(int(rec["iat"], 16))] = sg["slots"]
        elif sg["cc"] == "c":
            iat_slots["dword ptr [%s]" % hex(int(rec["iat"], 16))] = 0

    def block_of(va):
        i = bisect.bisect_right(starts, va) - 1
        # shared tails can make several blocks cover va; take the innermost start <= va that covers it
        while i >= 0 and not (starts[i] <= va < ends[i]):
            i -= 1
            if starts[i] < va - 0x4000:
                return None
        return (starts[i], ends[i]) if i >= 0 else None

    def insns(lo, hi):
        return list(md.disasm(read(lo, hi - lo + 16), lo))

    def measure(call_va, blk, after_bytes=16):
        lo, hi = blk
        ins = insns(lo, hi)
        pushes = 0
        hit = None
        for k, ii in enumerate(ins):
            if ii.address == call_va:
                hit = k
                break
            if ii.mnemonic == "call":
                # an intervening call to a known import consumes its own slots (stdcall pops
                # them; cdecl's `add esp` follows); any other call resets the count
                tgt = iat_slots.get(ii.op_str)
                pushes = max(0, pushes - tgt) if tgt is not None else 0
            elif ii.mnemonic == "push":
                pushes += 1
            elif ii.mnemonic == "sub" and ii.op_str.startswith("esp, "):
                try:
                    pushes += int(ii.op_str[5:], 0) // 4
                except ValueError:
                    pass
            elif ii.mnemonic == "add" and ii.op_str.startswith("esp, "):
                try:  # clean-up of an earlier cdecl call
                    pushes = max(0, pushes - int(ii.op_str[5:], 0) // 4)
                except ValueError:
                    pushes = 0
            elif ii.mnemonic == "pop":
                pushes = max(0, pushes - 1)
        if hit is None:
            return None
        cleanup = 0
        nxt = list(md.disasm(read(ins[hit].address + ins[hit].size, after_bytes), ins[hit].address + ins[hit].size))
        if nxt:
            n0 = nxt[0]
            if n0.mnemonic == "add" and n0.op_str.startswith("esp, "):
                try:
                    cleanup = int(n0.op_str[5:], 0)
                except ValueError:
                    pass
            elif n0.mnemonic == "pop" and n0.op_str in ("ecx", "edx"):
                cleanup = 4 * sum(1 for x in nxt[:2] if x.mnemonic == "pop" and x.op_str in ("ecx", "edx"))
        return pushes, cleanup

    out = {}
    problems = []
    for rec in data["imports"]:
        name = rec["name"] or rec["ordinal_name_inferred"]
        sig = imports[(rec["dll"], name)]
        iat = int(rec["iat"], 16)
        stats = collections.Counter()
        cleans = 0
        n = 0
        for s in rec["sites"]:
            va = int(s, 16)
            ins0 = next(md.disasm(read(va, 16), va), None)
            if ins0 is None:
                continue
            calls = []
            if ins0.mnemonic == "call":
                calls = [va]
            elif ins0.mnemonic == "mov" and ins0.operands and ins0.operands[0].type == x86.X86_OP_REG:
                reg = ins0.reg_name(ins0.operands[0].reg)
                # follow linearly to `call reg` until reg is overwritten or control leaves
                p = va + ins0.size
                for ii in md.disasm(read(p, 400), p):
                    if ii.mnemonic == "call" and ii.op_str == reg:
                        calls.append(ii.address)
                    elif ii.mnemonic in ("ret", "jmp", "int3"):
                        break
                    elif ii.op_str.startswith(reg + ",") and ii.mnemonic not in ("cmp", "test", "push"):
                        break
                    elif ii.mnemonic == "call" and reg in ("eax", "ecx", "edx"):
                        break
            for c in calls:
                blk = block_of(c)
                if not blk:
                    continue
                m = measure(c, blk)
                if m is None:
                    continue
                n += 1
                stats[m[0]] += 1
                if m[1]:
                    cleans += 1
        mode = stats.most_common(1)[0][0] if stats else None
        entry = dict(cc=sig["cc"], slots=sig["slots"], varargs=sig["varargs"], sites=n,
                     push_hist={str(k): v for k, v in sorted(stats.items())}, cleanup_sites=cleans)
        verdict = "ok"
        if n == 0:
            verdict = "no-sites"
        elif sig["cc"] in ("s", "t"):
            if cleans:
                verdict = "stdcall-but-caller-cleans"
            elif sig["slots"] in stats:
                verdict = "ok"
            elif max(stats) < sig["slots"]:
                verdict = "fewer-pushes-seen"   # args pushed in an earlier block: review by hand
            else:
                verdict = "more-pushes-seen"    # register saves counted, or slots too small: review
        elif sig["cc"] == "c":
            if sig["slots"] > 0 and cleans == 0 and n >= 3:
                verdict = "cdecl-never-cleaned"
            elif not sig["varargs"] and sig["slots"] not in stats:
                verdict = "fewer-pushes-seen" if max(stats) < sig["slots"] else "more-pushes-seen"
        entry["verdict"] = verdict
        key = "%s!%s" % (rec["dll"], name)
        out[key] = entry
        if verdict not in ("ok", "no-sites"):
            problems.append((key, verdict, entry["push_hist"], cleans, n))

    summ = collections.Counter(e["verdict"] for e in out.values())
    res = dict(method=__doc__.strip().splitlines()[0], summary=dict(summ), imports=out)
    os.makedirs(os.path.join(ROOT, "data", "hle"), exist_ok=True)
    with open(os.path.join(ROOT, "data", "hle", "sig-check.json"), "w") as f:
        json.dump(res, f, indent=1, sort_keys=True)
    print(dict(summ))
    for p in problems:
        print(*p)


if __name__ == "__main__":
    main()
