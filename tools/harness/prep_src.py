#!/usr/bin/env python3
"""Boot-hook source tree: build/harness/src mirrors the translated translation units as symlinks, except the
one holding the CRT entry, where the single `call 0xf489e0` (WinMain, at 0x11e105c) is redirected to
sg_boot_winmain (runtime/core/src/core_boot_diag.c). The linked app is built from this tree. The hook prints a boot
report and exits when SPORE_STOP_AT_WINMAIN is set (a check that the CRT start-up reached WinMain), and
otherwise forwards to WinMain through dispatch.

Usage:   python3 -P tools/harness/prep_src.py [--src build/sporenative/recomp-512] [--out build/harness/src]
Inputs:  the translated C (--src, from tools/link/build.py --tus 512)
Outputs: the mirror tree (--out), symlinks plus one patched copy
When:    the converter's harness stage, between translate and link.
Environment: SPORE_WORK_DIR moves build/ (docs/design/convert.md 4)."""
import argparse, os, sys

ROOT = os.path.dirname(os.path.dirname(os.path.dirname(os.path.abspath(__file__))))
WORK = os.environ.get("SPORE_WORK_DIR") or ROOT
CALL = "SG_CALL(sg_f_00f489e0, 0x11e1061u);"
HOOK = "SG_CALL(sg_boot_winmain, 0x11e1061u);"
DECL = "extern void sg_boot_winmain(sg_ctx *ctx, uint8_t *mem);\n"
CREATURE_ENTRY = "SG_FUNC(00d65be0) {\n"
CREATURE_GUARD = CREATURE_ENTRY + "    if (sg_creature_buddy_guard(ctx, mem)) return;\n"
CREATURE_DECL = "extern int sg_creature_buddy_guard(sg_ctx *ctx, uint8_t *mem);\n"

ap = argparse.ArgumentParser(description=__doc__, formatter_class=argparse.RawDescriptionHelpFormatter)
ap.add_argument("--src", default=os.path.join(WORK, "build", "sporenative", "recomp-512"), help="the translated C")
ap.add_argument("--out", default=os.path.join(WORK, "build", "harness", "src"), help="the mirror tree to write")
a = ap.parse_args()
a.src, a.out = os.path.abspath(a.src), os.path.abspath(a.out)  # symlink targets must not be relative to a.out
os.makedirs(a.out, exist_ok=True)
hits = 0
creature_hits = 0
for f in sorted(os.listdir(a.src)):
    if not f.endswith(".c"):
        continue
    s, d = os.path.join(a.src, f), os.path.join(a.out, f)
    text = open(s).read()
    new = text
    declarations = ""
    if CALL in new:
        hits += new.count(CALL)
        new = new.replace(CALL, HOOK)
        declarations += DECL
        print("prep_src: %s: %d WinMain call site(s) redirected" % (f, text.count(CALL)))
    if CREATURE_ENTRY in new:
        creature_hits += new.count(CREATURE_ENTRY)
        new = new.replace(CREATURE_ENTRY, CREATURE_GUARD)
        declarations += CREATURE_DECL
        print("prep_src: %s: Creature Stage null-buddy guard installed" % f)
    if new != text:
        i = new.index("SG_DECL(")  # declare next to the first SG_DECL
        new = new[:i] + declarations + new[i:]
        if os.path.islink(d) or (os.path.exists(d) and open(d).read() != new):
            os.unlink(d)
        if not os.path.exists(d):
            open(d, "w").write(new)
    else:
        if os.path.islink(d) and os.readlink(d) == s:
            continue
        if os.path.lexists(d):
            os.unlink(d)
        os.symlink(s, d)
if hits != 1:
    sys.exit("prep_src: expected exactly one WinMain call site, found %d" % hits)
if creature_hits != 1:
    sys.exit("prep_src: expected exactly one Creature Stage entry, found %d" % creature_hits)
