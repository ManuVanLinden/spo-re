"""Self-tests for tools/shaders on fixtures we wrote ourselves (no game data).

tools/shaders is the Python reference for the Metal back end's shader translator (runtime/d3d9metal/shader):
d3dbc.py (D3D9 bytecode parser, disassembler, assembler), msl.py (the reference MSL emitter the C library matches
byte for byte), ref_interp.py (an interpreter for the GPU semantics test), fixtures/ (our own shader assembly) and mslcheck/
(compile and run MSL with the system Metal compiler, no Xcode).

Usage:   python3 -P tools/shaders/test_shaders.py [--metal MSLCHECK]   (MSLCHECK: a built mslcheck/mslcheck.m)
When:    `make -C runtime/d3d9metal/shader test` runs it; by hand after changing d3dbc.py, msl.py or a fixture.

1. Token encodings of a few instructions against values worked out by hand from the MSDN token format.
2. Every fixture in tools/shaders/fixtures/*.asm: assemble -> parse -> disassemble -> assemble is exact.
3. Every fixture translates to MSL; with --metal, every MSL file compiles with the system Metal compiler.
"""
import glob
import os
import subprocess
import sys

HERE = os.path.dirname(os.path.abspath(__file__))
sys.path.insert(0, HERE)
import d3dbc  # noqa: E402
import msl  # noqa: E402

fails = 0


def check(cond, what):
    global fails
    if not cond:
        fails += 1
        print("FAIL", what)


# 1. hand-computed encodings
check(d3dbc.assemble("vs_3_0\nmov r0, c1\n") == [0xFFFE0300, 0x02000001, 0x800F0000, 0xA0E40001, 0x0000FFFF],
      "mov r0, c1")
check(d3dbc.assemble("ps_2_0\nmul_sat_pp r1.xy, neg(r2.wzyx), t3\n")
      == [0xFFFF0200, 0x03000005, 0x80330001, 0x811B0002, 0xB0E40003, 0x0000FFFF], "mul_sat_pp")
# relative constant c[a0.x + 5] in vs_2_0: src token has bit 13, then a0.x address token (type 3, swizzle xxxx)
check(d3dbc.assemble("vs_2_0\nmov r0, c[a0.x + 5]\n")
      == [0xFFFE0200, 0x03000001, 0x800F0000, 0xA0E42005, 0xB0000000, 0x0000FFFF], "relative c")
# dcl_texcoord1 v2 in vs_3_0: usage token 0x80010005, dst v2 0x900F0002
check(d3dbc.assemble("vs_3_0\ndcl_texcoord1 v2\n") == [0xFFFE0300, 0x0200001F, 0x80010005, 0x900F0002, 0x0000FFFF],
      "dcl_texcoord1")
# def c4, 1.0, 0.0, 0.5, -2.0
check(d3dbc.assemble("ps_3_0\ndef c4, 1.0, 0.0, 0.5, -2.0\n")
      == [0xFFFF0300, 0x05000051, 0xA00F0004, 0x3F800000, 0, 0x3F000000, 0xC0000000, 0x0000FFFF], "def")
# ifc_lt with controls=4 in bits 16-23
check(d3dbc.assemble("ps_3_0\nifc_lt r0.xxxx, c0.yyyy\nendif\n")[1] == 0x02040029, "ifc_lt")
# comment block
sh = d3dbc.parse(d3dbc.assemble("vs_3_0\ncomment 43544142aabbccdd\nmov r0, c1\n"))
check(sh.insns[0].op == 0xFFFE and len(sh.insns[0].extra) == 2, "comment")

# 2 + 3. fixtures
outdir = os.path.join(HERE, "..", "..", "scratch", "shaders", "fixtures_msl")
os.makedirs(outdir, exist_ok=True)
metal_files = []
for f in sorted(glob.glob(os.path.join(HERE, "fixtures", "*.asm"))):
    name = os.path.splitext(os.path.basename(f))[0]
    toks = d3dbc.assemble(open(f).read())
    sh = d3dbc.parse(toks)
    text = d3dbc.disassemble(sh)
    check(d3dbc.assemble(text) == toks, f"{name}: round-trip")
    try:
        src, refl = msl.translate(sh, entry=name, options={"fog": "vertex"} if name.startswith("ps2") else {})
    except msl.Unsupported as e:
        check(False, f"{name}: unsupported {e}")
        continue
    p = os.path.join(outdir, name + ".metal")
    open(p, "w").write(src)
    metal_files.append(p)
    print(f"ok   {name}: {len(toks)} tokens, {len(src.splitlines())} MSL lines")

if "--metal" in sys.argv:
    tool = sys.argv[sys.argv.index("--metal") + 1]
    r = subprocess.run([tool] + metal_files, capture_output=True, text=True)
    sys.stdout.write(r.stdout)
    check(r.returncode == 0, "metal compile")

print("PASS" if not fails else f"{fails} FAILURES")
sys.exit(1 if fails else 0)
