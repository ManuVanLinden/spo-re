"""GPU semantics test of the MSL emitter on our own ALU-only vertex-shader fixtures.

Usage:   python3 -P tools/shaders/test_semantics.py MSLRUN [--impl build/d3d9metal/shader/d3d9msl_tool]
         (MSLRUN: a built mslcheck/mslrun.m)
When:    `make -C runtime/d3d9metal/shader test` runs it both ways; by hand after changing msl.py or ref_interp.py.

With --impl, the MSL and the reflection come from the run-time C library (runtime/d3d9metal/shader) through its
command-line tool instead of msl.py; the GPU results are checked against the same reference interpreter.

For each fixture: emit MSL in 'plain' mode, wrap it in a compute kernel, run it on the GPU over random
inputs (seeded), and compare every written output component with tools/shaders/ref_interp.py.
Only fixtures we wrote are executed; nothing from the game.
"""
import math
import os
import random
import struct
import subprocess
import sys
import tempfile

HERE = os.path.dirname(os.path.abspath(__file__))
sys.path.insert(0, HERE)
import d3dbc  # noqa: E402
import msl  # noqa: E402
import ref_interp  # noqa: E402

N = 64
# per fixture: input registers that feed address registers get small integer values
INT_INPUTS = {"vs3_skin": {2: (0, 20)}, "vs2_basic": {3: (0, 20)}, "vs3_tangent": {1: (0, 50)},
              "vs2_ops": {2: (0, 40)}}
FIXTURES = ["vs3_skin", "vs3_loop", "vs2_ops", "vs2_basic", "vs3_tangent", "vs_special"]

# extra fixture for IEEE edge cases (rcp/rsq/log of 0, pow with 0 exponent, nrm of zero vector)
SPECIAL = """vs_3_0
def c0, 0.0, 1.0, -2.0, 0.5
dcl_position v0
dcl_position o0
dcl_texcoord o1
dcl_texcoord1 o2
rcp r0.x, c0.xxxx
rsq r0.y, c0.xxxx
log r0.z, c0.xxxx
exp r0.w, c0.zzzz
mov o1, r0
pow r1.x, c0.zzzz, c0.wwww
rcp r1.y, neg(c0.xxxx)
log r1.z, c0.zzzz
rsq r1.w, c0.zzzz
mov o2, r1
mov o0, v0
"""


def close(g, r):
    if r != r:
        return g != g
    if math.isinf(r):
        return g == r
    if g != g or math.isinf(g):
        return False
    return abs(g - r) <= 2e-4 * max(1.0, abs(r))


def translate_c(tool, sh, name, tmp):
    blob = os.path.join(tmp, name + ".bin")
    js = os.path.join(tmp, name + ".refl.json")
    open(blob, "wb").write(struct.pack(f"<{len(sh.tokens)}I", *sh.tokens))
    r = subprocess.run([tool, "--plain", "--entry", name, "--json", js, blob], capture_output=True, text=True,
                       timeout=60)
    if r.returncode:
        raise RuntimeError(f"{tool}: {r.stderr}")
    import json
    return r.stdout, json.load(open(js))


AUX_FLOATS = 148  # sizeof(SgAux) / 4 = 592 / 4 (runtime/d3d9metal/shader/d3d9msl.h)


def main():
    runner = sys.argv[1]
    impl = sys.argv[sys.argv.index("--impl") + 1] if "--impl" in sys.argv else None
    rng = random.Random(1234)
    total = bad = 0
    tmp = tempfile.mkdtemp(prefix="sgsem", dir=os.path.join(HERE, "..", "..", "scratch", "shaders"))
    for name in FIXTURES:
        text = SPECIAL if name == "vs_special" else open(os.path.join(HERE, "fixtures", name + ".asm")).read()
        sh = d3dbc.parse(d3dbc.assemble(text))
        if impl:
            src, refl = translate_c(impl, sh, name, tmp)
        else:
            src, refl = msl.translate(sh, entry=name, options={"plain": True})
        ins_regs = [int(d["reg"][1:]) for d in refl["inputs"]]
        outs = [o["reg"] for o in refl["outputs"]]
        # kernel wrapper
        k = [src, f"kernel void run_{name}(device const float4* vin [[buffer(10)]], device float4* vout [[buffer(11)]],",
             "    constant float4* c [[buffer(0)]], constant int4* ci [[buffer(1)]], constant uint* cb [[buffer(2)]],",
             "    constant SgAux& aux [[buffer(3)]], uint tid [[thread_position_in_grid]]) {"]
        if ins_regs:
            k.append(f"    {name}_in in;")
            for j, r in enumerate(ins_regs):
                k.append(f"    in.v{r} = vin[tid * {len(ins_regs)} + {j}];")
            k.append(f"    {name}_out o = {name}(in, c, ci, cb, aux);")
        else:
            k.append(f"    {name}_out o = {name}(c, ci, cb, aux);")
        for j, (o, ty) in enumerate(zip(outs, [x["type"] for x in refl["outputs"]])):
            k.append(f"    vout[tid * {len(outs)} + {j}] = float4(o.{o});")
        k.append("}")
        mf = os.path.join(tmp, name + ".metal")
        open(mf, "w").write("\n".join(k) + "\n")
        for bval in (0, 1):
            c = [[rng.uniform(-2, 2) for _ in range(4)] for _ in range(256)]
            iv = [[0, 0, 0, 0] for _ in range(16)]
            bv = [bval] * 16
            vin = []
            for t in range(N):
                row = {}
                for r in ins_regs:
                    lo_hi = INT_INPUTS.get(name, {}).get(r)
                    row[r] = ([float(rng.randint(*lo_hi)) for _ in range(4)] if lo_hi
                              else [rng.uniform(-3, 3) for _ in range(4)])
                vin.append(row)
            files = {}
            for key, fmt, data in (("c", "f", [x for v in c for x in v]), ("ci", "i", [x for v in iv for x in v]),
                                   ("cb", "I", bv), ("aux", "f", [0.0] * AUX_FLOATS),
                                   ("vin", "f", [x for row in vin for r in ins_regs for x in row[r]] or [0.0])):
                p = os.path.join(tmp, f"{name}_{key}.bin")
                open(p, "wb").write(struct.pack(f"<{len(data)}{fmt}", *data))
                files[key] = p
            outp = os.path.join(tmp, f"{name}_out.bin")
            cmd = [runner, mf, f"run_{name}", str(N), f"in:0:{files['c']}", f"in:1:{files['ci']}",
                   f"in:2:{files['cb']}", f"in:3:{files['aux']}", f"in:10:{files['vin']}",
                   f"out:11:{outp}:{N * len(outs) * 16}"]
            r = subprocess.run(cmd, capture_output=True, text=True)
            if r.returncode:
                print("RUN FAIL", name, r.stderr[:2000])
                bad += 1
                continue
            raw = open(outp, "rb").read()
            gpu = struct.unpack(f"<{len(raw) // 4}f", raw)
            nbad = 0
            for t in range(N):
                ref = ref_interp.run(sh, {r: vin[t][r] for r in ins_regs}, c, iv, bv)
                for j, o in enumerate(outs):
                    if o not in ref:
                        continue
                    vals, mask = ref[o]
                    for comp in range(4):
                        if not mask >> comp & 1:
                            continue
                        if o in ("oFog", "oPts") and comp:
                            continue
                        total += 1
                        gv = gpu[(t * len(outs) + j) * 4 + comp]
                        if not close(gv, vals[comp]):
                            nbad += 1
                            if nbad <= 5:
                                print(f"  MISMATCH {name} b={bval} v#{t} {o}.{'xyzw'[comp]}: gpu {gv!r} ref {vals[comp]!r}")
            bad += nbad
            print(f"{'ok  ' if not nbad else 'FAIL'} {name} (b={bval}): {N} vertices, {len(outs)} outputs")
    print(f"{total - bad}/{total} components match ({'C library ' + impl if impl else 'msl.py'})")
    if not bad:
        __import__("shutil").rmtree(tmp, ignore_errors=True)
    return 1 if bad else 0


if __name__ == "__main__":
    sys.exit(main())
