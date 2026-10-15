"""Import table, step 3 of 4: recover the string arguments at LoadLibrary*/GetModuleHandle*/GetProcAddress call sites.

For each call site (from callsites.py output) we re-synchronise a linear
disassembly that lands exactly on the call, then look at the preceding ~12
instructions for `push imm32` / `mov reg, imm32` / `lea` operands that point to
ASCII or UTF-16 strings.

Usage:   PYTHONPATH=tools/.deps python3 -P tools/imports/dynres.py EXE cs.json > dyn.json
Inputs:  the exe and callsites.py's output
Outputs: JSON on stdout: the DLL and function names resolved at run time, with their call sites
When:    the converter's imports stage (see parse_imports.py).
"""
import json, re, struct, sys
import capstone

exe, csj = sys.argv[1], sys.argv[2]
d = open(exe, "rb").read()
cs = json.load(open(csj))
pe = struct.unpack_from("<I", d, 0x3C)[0]
nsec = struct.unpack_from("<H", d, pe + 6)[0]
optsz = struct.unpack_from("<H", d, pe + 20)[0]
base = struct.unpack_from("<I", d, pe + 24 + 28)[0]
secs = [struct.unpack_from("<8sIIII", d, pe + 24 + optsz + 40 * i) for i in range(nsec)]

def off(va):
    rva = va - base
    for _, vsz, sva, rsz, raw in secs:
        if sva <= rva < sva + rsz:
            return raw + rva - sva
    return None

def string_at(va):
    o = off(va)
    if o is None: return None
    m = re.match(rb"[\x20-\x7e]{2,}", d[o:o + 200])
    if m and d[o + len(m.group())] == 0:
        return m.group().decode()
    m = re.match(rb"(?:[\x20-\x7e]\x00){2,}", d[o:o + 400])
    if m and d[o + len(m.group()):o + len(m.group()) + 2] == b"\0\0":
        return "L:" + m.group().decode("utf-16le")
    return None

md = capstone.Cs(capstone.CS_ARCH_X86, capstone.CS_MODE_32)

def window(site, back=0x60):
    best = None
    for s in range(site - back, site - back + 16):
        o = off(s)
        ins = list(md.disasm(d[o:o + (site - s) + 8], s))
        addrs = [i.address for i in ins]
        if site in addrs:
            k = addrs.index(site)
            if best is None or k > len(best) - 1:
                best = ins[:k + 1]
    return best or []

TARGETS = ("GetProcAddress", "LoadLibraryA", "LoadLibraryW", "GetModuleHandleA", "GetModuleHandleW")
out = []
for imp in cs["imports"]:
    if imp["name"] not in TARGETS: continue
    for s in imp["sites"]:
        site = int(s, 16)
        ins = window(site)
        strs = []
        for i in ins[-14:-1]:
            for imm in re.findall(r"0x[0-9a-f]{6,8}", i.op_str):
                t = string_at(int(imm, 16))
                if t: strs.append(t)
        out.append(dict(api=imp["name"], site=s, strings=strs))
json.dump(out, sys.stdout, indent=1)
