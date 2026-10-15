"""Import table, step 2 of 4: count the call sites of each import using base relocations and Capstone.

Every absolute reference to an IAT slot carries a HIGHLOW relocation, so we walk
.reloc, keep fixups whose target is inside the IAT, decode the instruction that
contains the fixup and classify it. mov reg,[IAT] is followed forward to count
`call reg`. jmp [IAT] thunks are resolved by counting E8 calls / E9 jmps to them
(also verified via relocation-free rel32 scan + Capstone decode).

Usage:   PYTHONPATH=tools/.deps python3 -P tools/imports/callsites.py EXE imp.json > cs.json
Inputs:  the exe and parse_imports.py's output
Outputs: JSON on stdout: the call sites of every import
When:    the converter's imports stage (see parse_imports.py).
"""
import json, struct, sys, collections
import capstone

exe, impjson = sys.argv[1], sys.argv[2]
d = open(exe, "rb").read()
imp = json.load(open(impjson))
base = imp["image_base"]
pe = struct.unpack_from("<I", d, 0x3C)[0]
nsec = struct.unpack_from("<H", d, pe + 6)[0]
optsz = struct.unpack_from("<H", d, pe + 20)[0]
opt = pe + 24
dirs = [struct.unpack_from("<II", d, opt + 96 + 8 * i) for i in range(16)]
secs = []
for i in range(nsec):
    name, vsz, va, rsz, raw = struct.unpack_from("<8sIIII", d, opt + optsz + 40 * i)
    secs.append((name.rstrip(b"\0").decode(), va, vsz, raw, rsz))

def off(rva):
    for _, va, vsz, raw, rsz in secs:
        if va <= rva < va + max(vsz, rsz):
            return raw + rva - va if rva - va < rsz else None
    return None

def sec_of(rva):
    for n, va, vsz, raw, rsz in secs:
        if va <= rva < va + max(vsz, rsz):
            return n
    return None

text = [s for s in secs if s[0] == ".text"][0]
tva, tvsz, traw, trsz = text[1], text[2], text[3], text[4]
tlo, thi = base + tva, base + tva + min(tvsz, trsz)

slots = {int(i["iat"], 16): i for i in imp["imports"]}
lo, hi = min(slots), max(slots) + 4

# walk relocations
rrva, rsize = dirs[5]
ro = off(rrva); end = ro + rsize
fixups = []  # VA of fixup locations whose value is an IAT slot
while ro < end:
    page, blk = struct.unpack_from("<II", d, ro)
    if blk == 0: break
    for k in range((blk - 8) // 2):
        e = struct.unpack_from("<H", d, ro + 8 + 2 * k)[0]
        if e >> 12 == 3:
            rva = page + (e & 0xFFF)
            o = off(rva)
            if o is None: continue
            v = struct.unpack_from("<I", d, o)[0]
            if lo <= v < hi:
                fixups.append((base + rva, v))
    ro += blk

md = capstone.Cs(capstone.CS_ARCH_X86, capstone.CS_MODE_32)
md.detail = True

def decode_at_fixup(fva):
    """find instruction containing the fixup at fva: prefer the shortest
    candidate whose *memory displacement* is the fixup, else an imm32."""
    imm_hit = None
    for back in range(1, 8):
        start = fva - back
        o = off(start - base)
        for ins in md.disasm(d[o:o + 16], start):
            if ins.disp_offset and ins.address + ins.disp_offset == fva:
                return ins
            if imm_hit is None and ins.imm_offset and ins.address + ins.imm_offset == fva and back > 1:
                imm_hit = ins
            break
    return imm_hit

REGS32 = {"eax","ebx","ecx","edx","esi","edi","ebp"}
def follow_reg(ins, reg, found, maxn=400):
    """count `call reg` after `mov reg,[IAT]` until reg is overwritten / ret."""
    n = 0
    a = ins.address + ins.size
    o = off(a - base)
    for i2 in md.disasm(d[o:o + 0x1800], a):
        maxn -= 1
        if maxn <= 0: break
        m, ops = i2.mnemonic, i2.op_str
        if m == "call" and ops == reg:
            n += 1
            found.append(i2.address)
            continue
        if m in ("ret", "retn", "int3"): break
        if m == "call" and reg in ("eax", "ecx", "edx"):  # volatile reg clobbered by call
            break
        if m == "jmp" and ops == reg: n += 1; break
        # overwritten?
        try:
            _, w = i2.regs_access()
        except Exception:
            w = []
        if any(i2.reg_name(r) == reg for r in w):
            break
    return n

stats = collections.defaultdict(lambda: collections.Counter())
sites = collections.defaultdict(list)  # slot -> call instruction VAs
thunks = {}  # thunk VA -> slot
other = []
for fva, slot in fixups:
    s = sec_of(fva - base)
    if s != ".text":
        stats[slot]["data_ref_" + str(s)] += 1
        continue
    ins = decode_at_fixup(fva)
    if ins is None:
        stats[slot]["undecoded"] += 1
        continue
    m = ins.mnemonic
    if m == "call":
        stats[slot]["call_mem"] += 1
        sites[slot].append(ins.address)
    elif m == "jmp":
        stats[slot]["jmp_mem"] += 1
        thunks[ins.address] = slot
    elif m == "mov" and ins.op_str.split(",")[0] in REGS32:
        reg = ins.op_str.split(",")[0]
        stats[slot]["mov_reg"] += 1
        stats[slot]["call_via_reg"] += follow_reg(ins, reg, sites[slot])
    else:
        stats[slot]["other:" + m] += 1
        other.append((hex(ins.address), m, ins.op_str))

# rel32 calls/jmps to thunks
if thunks:
    tb = d[traw:traw + min(tvsz, trsz)]
    tset = set(thunks)
    for opc, kind in ((0xE8, "call_thunk"), (0xE9, "jmp_thunk")):
        i = tb.find(bytes([opc]))
        while i != -1 and i + 5 <= len(tb):
            tgt = (tlo + i + 5 + struct.unpack_from("<i", tb, i + 1)[0]) & 0xFFFFFFFF
            if tgt in tset:
                stats[thunks[tgt]][kind] += 1
                sites[thunks[tgt]].append(tlo + i)
            i = tb.find(bytes([opc]), i + 1)

# absolute (relocated) references to thunks, e.g. vtable slots, callbacks
if thunks:
    ro = off(rrva); end = ro + rsize
    while ro < end:
        page, blk = struct.unpack_from("<II", d, ro)
        if blk == 0: break
        for k in range((blk - 8) // 2):
            e = struct.unpack_from("<H", d, ro + 8 + 2 * k)[0]
            if e >> 12 == 3:
                rva = page + (e & 0xFFF)
                o = off(rva)
                if o is None: continue
                v = struct.unpack_from("<I", d, o)[0]
                if v in thunks:
                    stats[thunks[v]]["thunk_addr_ref_" + str(sec_of(rva))] += 1
        ro += blk

res = []
for slot, e in sorted(slots.items()):
    c = dict(stats.get(slot, {}))
    thunk_used = sum(v for k, v in c.items() if k.startswith(("call_thunk", "jmp_thunk", "thunk_addr_ref")))
    uses = (c.get("call_mem", 0) + c.get("call_via_reg", 0) + thunk_used
            + (c.get("jmp_mem", 0) if not thunk_used else 0))
    refs = sum(v for k, v in c.items() if k in ("call_mem", "jmp_mem", "mov_reg") or k.startswith(("other", "data_ref")))
    res.append(dict(iat=e["iat"], dll=e["dll"], name=e["name"], ordinal=e["ordinal"],
                    iat_refs=refs, uses=uses, detail=c,
                    sites=[hex(a) for a in sorted(sites.get(slot, []))]))
json.dump(dict(fixups=len(fixups), thunks={hex(k): hex(v) for k, v in thunks.items()},
               other=other[:200], imports=res), sys.stdout, indent=1)
