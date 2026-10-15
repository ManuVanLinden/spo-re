"""Import table, step 1 of 4: parse the import directory (normal and delay-load) of a 32-bit PE.

Usage:   python3 -P tools/imports/parse_imports.py EXE > imp.json
Inputs:  the exe
Outputs: a JSON list of imports (DLL, name or ordinal, IAT address) on stdout
When:    the converter's imports stage runs the four steps (this, callsites.py, dynres.py, build_table.py).
"""
import json, struct, sys

def load(path):
    d = open(path, "rb").read()
    pe = struct.unpack_from("<I", d, 0x3C)[0]
    nsec = struct.unpack_from("<H", d, pe + 6)[0]
    optsz = struct.unpack_from("<H", d, pe + 20)[0]
    opt = pe + 24
    base = struct.unpack_from("<I", d, opt + 28)[0]
    ndirs = struct.unpack_from("<I", d, opt + 92)[0]
    dirs = [struct.unpack_from("<II", d, opt + 96 + 8 * i) for i in range(ndirs)]
    secs = []
    for i in range(nsec):
        name, vsz, va, rsz, raw = struct.unpack_from("<8sIIII", d, opt + optsz + 40 * i)
        secs.append((name.rstrip(b"\0").decode(), va, max(vsz, rsz), raw, rsz))
    return d, base, dirs, secs

def mk_off(secs):
    def off(rva):
        for _, va, sz, raw, rsz in secs:
            if va <= rva < va + sz:
                return raw + rva - va
        return None
    return off

def cstr(d, o):
    return d[o:d.index(b"\0", o)].decode("latin-1")

def parse(path):
    d, base, dirs, secs = load(path)
    off = mk_off(secs)
    out = []
    # normal imports
    rva = dirs[1][0]
    o = off(rva)
    while True:
        oft, ts, fwd, name, ft = struct.unpack_from("<IIIII", d, o)
        if not (oft or name or ft):
            break
        dll = cstr(d, off(name))
        lookup = oft or ft
        i = 0
        while True:
            v = struct.unpack_from("<I", d, off(lookup) + 4 * i)[0]
            if v == 0:
                break
            iat = base + ft + 4 * i
            if v & 0x80000000:
                ent = dict(dll=dll, name=None, ordinal=v & 0xFFFF, hint=None)
            else:
                ho = off(v)
                ent = dict(dll=dll, name=cstr(d, ho + 2), ordinal=None,
                           hint=struct.unpack_from("<H", d, ho)[0])
            ent.update(iat=f"0x{iat:08x}", kind="static")
            out.append(ent)
            i += 1
        o += 20
    # delay imports
    if len(dirs) > 13 and dirs[13][0]:
        o = off(dirs[13][0])
        while True:
            attrs, name, hmod, iat, int_, bound, unload, ts = struct.unpack_from("<8I", d, o)
            if not name:
                break
            rvabased = attrs & 1
            fix = (lambda x: x) if rvabased else (lambda x: x - base)
            dll = cstr(d, off(fix(name)))
            i = 0
            while True:
                v = struct.unpack_from("<I", d, off(fix(int_)) + 4 * i)[0]
                if v == 0:
                    break
                if v & 0x80000000:
                    ent = dict(dll=dll, name=None, ordinal=v & 0xFFFF, hint=None)
                else:
                    ho = off(fix(v))
                    ent = dict(dll=dll, name=cstr(d, ho + 2), ordinal=None, hint=struct.unpack_from("<H", d, ho)[0])
                ent.update(iat=f"0x{base + fix(iat) + 4*i:08x}", kind="delay")
                out.append(ent)
                i += 1
            o += 32
    return dict(image_base=base, sections=[s[:3] for s in secs],
                dirs={k: dirs[k] for k in (1, 12, 13) if k < len(dirs)}, imports=out)

if __name__ == "__main__":
    r = parse(sys.argv[1])
    json.dump(r, sys.stdout, indent=1)
