#!/usr/bin/env python3
"""Generate the link support for build/sporenative/spore (docs/design/image-linker.md).

Reads the translator's output directory (SG_FUNC definitions, landing owners and their resume switches),
the function manifest and the exe, and writes into --out (always under build/, git-ignored):

  dispatch.c       sg_dispatch_entries[] (sorted), sg_dispatch_count, sg_dispatch_source,
                   sg_resume_points[] / sg_resume_point_count (runtime-abi 6.1, 10.3)
  buckets.bin/.s   sg_dispatch_bucket[]: one uint32 per 16 bytes of .text plus a sentinel, as raw data
                   pulled in with .incbin (1,034,996 entries; a C initializer would be slow to compile)
  entries.txt      every manifest function (F) and label (L <kind>) for `spore --selftest`
  image.bin/.s     the exe laid out as mapped, 0x400000-0x1d10000, for the embedded-image variant
  image_none.c     sg_embedded_image_size = 0, for the file-loading variant
  gen-report.json  counts and cross-checks

Usage:   python3 -P tools/link/gen_link.py --src build/recomp/all --out build/sporenative/gen
When:    tools/link/build.py runs it (step 4) at every link; by hand only to inspect its outputs.
Environment: SPORE_WORK_DIR, SPORE_GAME_DIR and SPORE_EXE move the outputs and name the exe (docs/design/convert.md 4).
"""
import argparse
import hashlib
import json
import os
import re
import struct
import sys
import time

ROOT = os.path.abspath(os.path.join(os.path.dirname(__file__), "..", ".."))
sys.path.insert(0, os.path.join(ROOT, "tools", "manifest"))
# SPORE_WORK_DIR (default: the source tree) takes the generated outputs; SPORE_GAME_DIR / SPORE_EXE name the
# Spore folder / the EP1 exe instead of game/ (docs/design/convert.md 4).
WORK = os.environ.get("SPORE_WORK_DIR") or ROOT
EXE = os.environ.get("SPORE_EXE") or os.path.join(os.environ.get("SPORE_GAME_DIR") or os.path.join(ROOT, "game"),
                                                  "SporebinEP1", "SporeApp.exe")
import manifest as mf  # noqa: E402

TEXT_START, TEXT_END = 0x401000, 0x13CBF2A
IMAGE_BASE, IMAGE_SIZE, HOST_PAGE = 0x400000, 0x190E000, 0x4000
MAPPED_END = (IMAGE_BASE + IMAGE_SIZE + HOST_PAGE - 1) & ~(HOST_PAGE - 1)
NBUCKETS = ((TEXT_END - TEXT_START) + 15) // 16 + 1  # SG_DISPATCH_BUCKETS
DE_FUNC, DE_SECONDARY = 1, 2
SHARED_TAIL = 256
EXE_SHA = "dc04aee5a3debc3f1ad4c1a937460e99a29b9bd3bc285008be83615dd5e59a37"

RE_FUNC = re.compile(r"^SG_FUNC\(([0-9a-f]{8})\)")
RE_LAND = re.compile(r"SG_LANDING_BEGIN\(0x([0-9a-f]+)u?\)")
RE_CASE = re.compile(r"^\s*case 0x([0-9a-f]+)u?:")


def scan_sources(src):
    """Returns (funcs {va: file}, resume [(resume, owner)], files)."""
    funcs, resume = {}, []
    files = sorted(f for f in os.listdir(src) if f.endswith(".c"))
    for name in files:
        cur, in_resume = None, False
        with open(os.path.join(src, name), "r", errors="replace") as f:
            for line in f:
                if line.startswith("SG_FUNC("):
                    m = RE_FUNC.match(line)
                    if m:
                        cur = int(m.group(1), 16)
                        if cur in funcs:
                            raise SystemExit("duplicate SG_FUNC %08x in %s and %s" % (cur, funcs[cur], name))
                        funcs[cur] = name
                        in_resume = False
                    continue
                if in_resume:
                    m = RE_CASE.match(line)
                    if m:
                        resume.append((int(m.group(1), 16), cur))
                    elif "sg_bad_resume" in line:
                        in_resume = False
                elif line.startswith("SG_RESUME_DISPATCH:"):
                    in_resume = True
                elif "SG_LANDING_BEGIN(" in line:
                    m = RE_LAND.search(line)
                    if m and int(m.group(1), 16) != cur:
                        raise SystemExit("landing %s inside %08x" % (m.group(1), cur))
    return funcs, sorted(resume), files


def pe_layout(exe):
    d = open(exe, "rb").read()
    sha = hashlib.sha256(d).hexdigest()
    pe = struct.unpack_from("<I", d, 0x3C)[0]
    nsec, opt = struct.unpack_from("<H", d, pe + 6)[0], struct.unpack_from("<H", d, pe + 20)[0]
    base, size_hdr = struct.unpack_from("<I", d, pe + 24 + 28)[0], struct.unpack_from("<I", d, pe + 24 + 60)[0]
    assert base == IMAGE_BASE, hex(base)
    img = bytearray(MAPPED_END - IMAGE_BASE)
    img[0:size_hdr] = d[0:size_hdr]
    secs = []
    for i in range(nsec):
        name, vsz, va, rsz, raw = struct.unpack_from("<8sIIII", d, pe + 24 + opt + 40 * i)
        n = min(vsz, rsz)
        img[va:va + n] = d[raw:raw + n]
        secs.append({"name": name.rstrip(b"\0").decode(), "va": "%#x" % (base + va), "vsize": vsz, "raw": rsz})
    return sha, bytes(img), secs


def main():
    ap = argparse.ArgumentParser(description=__doc__, formatter_class=argparse.RawDescriptionHelpFormatter)
    ap.add_argument("--src", default=os.path.join(WORK, "build", "recomp", "all"), help="the translated C")
    ap.add_argument("--out", default=os.path.join(WORK, "build", "sporenative", "gen"), help="output folder")
    ap.add_argument("--exe", default=EXE, help="the exe (default: the EP1 exe)")
    ap.add_argument("--tag", default="ep1", help="the manifest to read (default ep1)")
    ap.add_argument("--no-image", action="store_true", help="skip image.bin (file-loading builds only)")
    a = ap.parse_args()
    t0 = time.time()
    os.makedirs(a.out, exist_ok=True)

    funcs, resume, files = scan_sources(a.src)
    t_scan = time.time() - t0
    m = mf.load(a.tag)
    entries = sorted(funcs)
    man_funcs = set(m.functions)
    canon = {e for e, f in m.functions.items() if f.srcbits & mf.CANON}
    missing = sorted(man_funcs - set(funcs))        # manifest functions with no translation
    extra = sorted(set(funcs) - man_funcs)            # translated entries the manifest lacks (promoted labels)
    shared = {e for e, f in m.functions.items() if f.srcbits & SHARED_TAIL}
    labels = m.labels
    out_text = [e for e in entries if not TEXT_START <= e < TEXT_END]
    if out_text:
        raise SystemExit("entries outside .text: %s" % out_text[:5])

    # dispatch.c
    with open(os.path.join(a.out, "dispatch.c"), "w") as f:
        f.write("/* GENERATED by tools/link/gen_link.py from %s. Do not edit. */\n" % os.path.relpath(a.src, ROOT))
        f.write('#include "spore_guest.h"\n\n')
        for e in entries:
            f.write("SG_DECL(%08x);\n" % e)
        f.write("\nconst sg_dispatch_entry sg_dispatch_entries[%d] = {\n" % len(entries))
        for e in entries:
            fl = DE_FUNC | (DE_SECONDARY if (e in shared or e in extra) else 0)
            f.write("    {0x%08xu, %d, sg_f_%08x},\n" % (e, fl, e))
        f.write("};\nconst uint32_t sg_dispatch_count = %du;\n" % len(entries))
        f.write('const char sg_dispatch_source[] = "%s, %d files";\n' % (os.path.relpath(a.src, ROOT), len(files)))
        f.write("\nconst sg_resume_point sg_resume_points[%d] = {\n" % max(1, len(resume)))
        for r, o in resume:
            f.write("    {0x%08xu, 0x%08xu},\n" % (r, o))
        if not resume:
            f.write("    {0, 0},\n")
        f.write("};\nconst uint32_t sg_resume_point_count = %du;\n" % len(resume))

    # buckets: bucket[b] = first entry index with guest >= TEXT_START + 16*b; plus sentinel
    buckets = bytearray(4 * (NBUCKETS + 1))
    i = 0
    for b in range(NBUCKETS + 1):
        start = TEXT_START + 16 * b
        while i < len(entries) and entries[i] < start:
            i += 1
        struct.pack_into("<I", buckets, 4 * b, i)
    assert struct.unpack_from("<I", buckets, 4 * NBUCKETS)[0] == len(entries)
    bpath = os.path.join(a.out, "buckets.bin")
    open(bpath, "wb").write(buckets)
    with open(os.path.join(a.out, "buckets.s"), "w") as f:
        f.write("// GENERATED by tools/link/gen_link.py\n\t.section __TEXT,__const\n\t.globl _sg_dispatch_bucket\n"
                "\t.p2align 2\n_sg_dispatch_bucket:\n\t.incbin \"%s\"\n" % bpath)
    per_bucket = {}
    for b in range(NBUCKETS):
        n = struct.unpack_from("<I", buckets, 4 * b + 4)[0] - struct.unpack_from("<I", buckets, 4 * b)[0]
        per_bucket[n] = per_bucket.get(n, 0) + 1

    # entries.txt (manifest walk for the self-test)
    with open(os.path.join(a.out, "entries.txt"), "w") as f:
        f.write("# F <va> <srcbits>: manifest functions; L <va> <kind>: manifest labels\n")
        for e in sorted(m.functions):
            f.write("F %08x %x\n" % (e, m.functions[e].srcbits))
        for e in sorted(labels):
            f.write("L %08x %s\n" % (e, labels[e]))

    # images
    with open(os.path.join(a.out, "image_none.c"), "w") as f:
        f.write("/* GENERATED: no embedded image (the loader reads the exe at run time). */\n#include <stdint.h>\n"
                "const uint8_t sg_embedded_image[1] = {0};\nconst uint32_t sg_embedded_image_size = 0;\n")
    img = {}
    if not a.no_image:
        sha, blob, secs = pe_layout(a.exe)
        if sha != EXE_SHA:
            raise SystemExit("exe SHA-256 %s is not the GOG EP1 build" % sha)
        ipath = os.path.join(a.out, "image.bin")
        open(ipath, "wb").write(blob)
        with open(os.path.join(a.out, "image.s"), "w") as f:
            f.write("// GENERATED by tools/link/gen_link.py: the exe laid out as mapped (build/ only)\n"
                    "\t.section __SPIMG,__image\n\t.globl _sg_embedded_image\n\t.p2align 14\n"
                    "_sg_embedded_image:\n\t.incbin \"%s\"\n"
                    "\t.section __TEXT,__const\n\t.globl _sg_embedded_image_size\n\t.p2align 2\n"
                    "_sg_embedded_image_size:\n\t.long 0x%x\n" % (ipath, len(blob)))
        img = {"bytes": len(blob), "sha256_exe": sha, "sections": secs}

    rep = {
        "src": os.path.relpath(a.src, ROOT), "files": len(files), "entries": len(entries),
        "resume_points": len(resume), "landing_owners": len({o for _r, o in resume}),
        "manifest_functions": len(man_funcs), "manifest_canonical": len(canon), "manifest_labels": len(labels),
        "manifest_functions_untranslated": ["%#x" % x for x in missing],
        "translated_not_in_manifest": ["%#x" % x for x in extra],
        "labels_that_are_entries": ["%#x" % x for x in sorted(labels) if x in funcs],
        "labels_that_are_resume_points": sum(1 for x in labels if x in dict(resume)),
        "labels_neither": ["%#x" % x for x in sorted(labels) if x not in funcs and x not in dict(resume)],
        "resume_points_not_labels": ["%#x" % r for r, _o in resume if r not in labels],
        "buckets": NBUCKETS + 1, "entries_per_bucket": {str(k): v for k, v in sorted(per_bucket.items())},
        "aligned16": sum(1 for e in entries if e % 16 == 0),
        "image": img, "scan_seconds": round(t_scan, 1), "seconds": round(time.time() - t0, 1),
    }
    json.dump(rep, open(os.path.join(a.out, "gen-report.json"), "w"), indent=1)
    print(json.dumps({k: v for k, v in rep.items() if k != "image"}, indent=1))


if __name__ == "__main__":
    main()
