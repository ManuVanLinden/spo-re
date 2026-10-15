#!/usr/bin/env python3
"""exe_icon.py -- the game's own icon for the app, taken from the player's own SporeApp.exe at build time.

Usage:   python3 -P tools/bundle/exe_icon.py game/SporebinEP1/SporeApp.exe build/bundle/AppIcon.icns
When:    make_bundle.py runs it (unless --own-icon).

Reads the PE resource directory, picks the largest icon of the exe's first RT_GROUP_ICON (a 256x256 PNG in the
GOG build), re-encodes it and the smaller sizes with `sips` into an .iconset, and runs `iconutil`. Every pixel
is re-encoded, so the app carries no byte run of the executable (make_bundle.py's check). The icon is EA's
artwork: it exists only inside build/ on the player's machine, like the game data the binary reads at run time,
and is never committed.
"""
import os, shutil, struct, subprocess, sys, tempfile

RT_ICON, RT_GROUP_ICON = 3, 14


def pe_icon_png(exe):
    f = open(exe, "rb").read()
    pe = struct.unpack_from("<I", f, 0x3c)[0]
    if f[pe:pe + 4] != b"PE\0\0":
        raise SystemExit("exe_icon: not a PE file: " + exe)
    nsec = struct.unpack_from("<H", f, pe + 6)[0]
    opt = pe + 24
    optsz = struct.unpack_from("<H", f, pe + 20)[0]
    res_rva = struct.unpack_from("<I", f, opt + 96 + 2 * 8)[0]  # data directory 2: resources (PE32)
    secs = [struct.unpack_from("<IIII", f, opt + optsz + i * 40 + 8) for i in range(nsec)]  # vsize, va, rawsize, rawptr

    def off(rva):
        for vs, va, rs, rp in secs:
            if va <= rva < va + max(vs, rs):
                return rva - va + rp
        raise SystemExit("exe_icon: rva 0x%x outside every section" % rva)

    base = off(res_rva)

    def walk(o, path):
        n_named, n_id = struct.unpack_from("<HH", f, o + 12)
        out = []
        for i in range(n_named + n_id):
            name, data = struct.unpack_from("<II", f, o + 16 + i * 8)
            if data & 0x80000000:
                out += walk(base + (data & 0x7fffffff), path + [name & 0x7fffffff])
            else:
                rva, size = struct.unpack_from("<II", f, base + data)
                out.append((path + [name & 0x7fffffff], rva, size))
        return out

    entries = walk(base, [])
    icons = {e[0][1]: e for e in entries if e[0][0] == RT_ICON}
    groups = [e for e in entries if e[0][0] == RT_GROUP_ICON]
    if not groups:
        raise SystemExit("exe_icon: no icon group in " + exe)
    g = groups[0]
    go = off(g[1])
    count = struct.unpack_from("<H", f, go + 4)[0]
    best = None
    for k in range(count):
        w, h, cc, res, planes, bpp, size, iid = struct.unpack_from("<BBBBHHIH", f, go + 6 + k * 14)
        w, h = w or 256, h or 256
        if iid in icons:
            io = off(icons[iid][1])
            data = f[io:io + icons[iid][2]]
            if data[:8] == b"\x89PNG\r\n\x1a\n" and (best is None or w * h > best[0]):
                best = (w * h, w, data)
    if best is None:
        raise SystemExit("exe_icon: the icon group has no PNG image (only DIBs); not handled")
    return best[1], best[2]


def make_icns(exe, out_icns):
    size, png = pe_icon_png(exe)
    work = tempfile.mkdtemp(prefix="exe-icon-")
    try:
        src = os.path.join(work, "src.png")
        open(src, "wb").write(png)
        iconset = os.path.join(work, "AppIcon.iconset")
        os.makedirs(iconset)
        for px, names in ((16, ["icon_16x16.png"]), (32, ["icon_16x16@2x.png", "icon_32x32.png"]),
                          (64, ["icon_32x32@2x.png"]), (128, ["icon_128x128.png"]),
                          (256, ["icon_128x128@2x.png", "icon_256x256.png"])):
            if px > size:
                continue
            tmp = os.path.join(work, "s%d.png" % px)
            # sips re-encodes even at the source size, so no byte run of the exe survives into the bundle
            subprocess.run(["sips", "-s", "format", "png", "-z", str(px), str(px), src, "--out", tmp],
                           check=True, capture_output=True, timeout=60)
            for n in names:
                shutil.copy2(tmp, os.path.join(iconset, n))
        subprocess.run(["iconutil", "-c", "icns", iconset, "-o", out_icns], check=True, timeout=60)
    finally:
        shutil.rmtree(work, ignore_errors=True)
    return size


if __name__ == "__main__":
    if len(sys.argv) != 3:
        sys.exit(__doc__)
    s = make_icns(sys.argv[1], sys.argv[2])
    print("exe_icon: %dx%d icon from %s -> %s" % (s, s, sys.argv[1], sys.argv[2]))
