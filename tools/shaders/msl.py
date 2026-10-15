"""D3D9 shader bytecode (vs/ps 2_0, 2_x, 3_0) -> Metal Shading Language emitter: the reference for the
run-time C library runtime/d3d9metal/shader/ (d3d9msl), which emits the same text byte for byte
(cross-checked by runtime/d3d9metal/shader/tests/crosscheck.py).

    from msl import translate
    src, refl = translate(shader, entry="sg_main", options={...})
    options: fog="vertex" (ps < 3_0: blend with the VS fog output), flat=True (colour inputs [[flat]],
             D3DSHADE_FLAT), plain=True (test mode: an ordinary function, ALU-only VS)

CLI:
    msl.py <blob> [--entry NAME] [--fog vertex] [--flat] [--plain] [--json]   print MSL (+ reflection)

Binding model (runtime/d3d9metal/shader/d3d9msl.h and runtime/d3d9metal/include/d3d9metal_int.h):
    buffer(0)  constant float4 c[]   float constants (VS 256, PS 224 slots; out-of-range relative reads -> 0)
    buffer(1)  constant int4  ci[16] integer constants
    buffer(2)  constant uint  cb[16] boolean constants (0 / non-zero)
    buffer(3)  constant SgAux& aux   alpha-test ref, fog, render-target size, half-pixel fixup,
                                     per-sampler state samp[16] (LOD bias, sRGB, border colour, depth compare)
    buffer(14+k) vertex stream k (vertex descriptor built from the D3D9 vertex declaration;
                attribute n == input register v<n>; the reflection lists usage/index per register)
    texture(n)/sampler(n)  D3D9 sampler s<n> (VS: vertex texture sampler n = D3DVERTEXTEXTURESAMPLER0 + n)
    function_constant(0) uint sg_alpha_func   D3DCMPFUNC for alpha test (0 or 8 = off), PS only
VS outputs / PS inputs are linked by [[user(<usage><index>)]] names, e.g. user(texcoord3).

D3D9 behaviours emulated in the emitted code (each matches dxbc-spirv's SM3 front end,
https://github.com/doitsujin/dxbc-spirv/blob/main/sm3/sm3_converter.cpp, MIT):
    rsq/log/pow take abs() of the base; mova rounds to nearest even; vPos = [[position]].xy - 0.5;
    vFace = +-1; unwritten outputs default to 0 except color0 = 1 and other colors = (0,0,0,1), fog = 1;
    ps < 3_0 gets post-shader fog (option), all PS get alpha test (function constant).
    Half-pixel offset: the VS adds aux.posFixup.xy * w to its position output; the runtime sets
    (+1/vpW, -1/vpH) there, or zero if it shifts the Metal viewport origin by +0.5 px.
    texld* go through sg_tex* helpers: LOD bias (also for texldl, as MSDN's texldl page says), sRGB
    decode, border colours Metal cannot express, depth-texture compare, all from aux.samp[n].
"""
import json
import struct
import sys
import os

sys.path.insert(0, os.path.dirname(os.path.abspath(__file__)))
import d3dbc  # noqa: E402

SW = "xyzw"
CMPOP = {1: ">", 2: "==", 3: ">=", 4: "<", 5: "!=", 6: "<="}


class Unsupported(Exception):
    pass


def _f32(x):
    v = struct.unpack("<f", struct.pack("<I", x))[0]
    if v != v:
        return "NAN"
    if v in (float("inf"), float("-inf")):
        return ("INFINITY" if v > 0 else "-INFINITY")
    # shortest %g text that reads back as the same float32 (the C library uses the same rule)
    for p in range(1, 10):
        r = f"{v:.{p}g}"
        if struct.unpack("<f", struct.pack("<f", float(r)))[0] == v:
            break
    if "e" not in r and "." not in r:
        r += ".0"
    return r + "f"


class _Emit:
    def __init__(self, sh, entry, opts):
        self.sh = sh
        self.entry = entry
        self.opts = opts
        self.vs = sh.kind == "vs"
        self.sm3 = sh.major >= 3
        if sh.major < 2:
            raise Unsupported("shader model 1.x not supported by the prototype")
        self.lines = []
        self.ind = 1
        self.defs_f = {}      # c index -> 4 tokens
        self.defs_i = {}
        self.defs_b = {}
        self.inputs = {}      # reg (type,num) -> dict
        self.outputs = {}
        self.samplers = {}    # num -> texture type
        self.temps = set()
        self.max_c = -1
        self.rel_c = False
        self.c_used = set()   # float/int/bool constant registers read from the buffers (def'd ones excluded)
        self.i_used = set()
        self.b_used = set()
        self.loop_depth = 0
        self.uses = set()
        self.refl = dict(kind=sh.kind, profile=sh.profile, entry=entry)

    # ------------------------------------------------------------ helpers
    def w(self, s):
        self.lines.append("    " * self.ind + s)

    def reg(self, p, for_dst=False):
        """MSL lvalue/rvalue (float4 unless noted) for a parameter, without swizzle/modifier."""
        rt, n = p.regtype, p.num
        if rt == 0:
            self.temps.add(n)
            return f"r{n}"
        if rt == 1:
            if self.vs or self.sm3:
                return f"in.v{n}"
            return f"in.d{n}"  # ps_2 colour input
        if rt == 2:
            if p.relative:
                self.rel_c = True
                return f"sg_ldc(c, {self._reladdr(p)} + {n})"
            self.max_c = max(self.max_c, n)
            if n in self.defs_f:
                return f"kc{n}"
            self.c_used.add(n)
            return f"c[{n}]"
        if rt == 3:
            if self.vs:
                return "a0"
            return f"in.t{n}"
        if rt == 4:
            name = {0: "oPos", 1: "oFog", 2: "oPts"}[n]
            self.outputs.setdefault((4, n), dict(reg=name, user=None))
            return name
        if rt == 5:
            self.outputs.setdefault((5, n), dict(reg=f"oD{n}", usage="color", index=n))
            return f"oD{n}"
        if rt == 6:
            if self.sm3:
                if (6, n) not in self.outputs:
                    raise Unsupported(f"o{n} written without dcl")
                return f"o{n}"
            self.outputs.setdefault((6, n), dict(reg=f"oT{n}", usage="texcoord", index=n))
            return f"oT{n}"
        if rt == 7:
            if n in self.defs_i:
                return f"ki{n}"
            self.i_used.add(n)
            return f"ci[{n}]"
        if rt == 8:
            self.outputs.setdefault((8, n), dict(reg=f"oC{n}"))
            return f"oC{n}"
        if rt == 9:
            self.outputs.setdefault((9, 0), dict(reg="oDepth"))
            return "oDepth"
        if rt == 14:
            if n in self.defs_b:
                return f"kb{n}"
            self.b_used.add(n)
            return f"(cb[{n}] != 0u)"
        if rt == 15:
            return "aL"
        if rt == 17:
            if n == 0:
                self.uses.add("vPos")
                return "float4(sg_pos.xy - 0.5f, 0.0f, 0.0f)"
            self.uses.add("vFace")
            return "float4(sg_front ? 1.0f : -1.0f)"
        raise Unsupported(f"register type {rt}")

    def _reladdr(self, p):
        r = d3dbc.Param(p.rel)
        comp = SW[r.swizzle & 3]
        if r.regtype == 15:
            return "aL"
        if r.regtype == 3:
            return f"a0.{comp}"
        raise Unsupported("relative address register")

    def src(self, p, comps=None):
        """Swizzled + modified source as float4 (or narrower if comps given: a string like 'xyz')."""
        base = self.reg(p)
        sw = "".join(SW[(p.swizzle >> (2 * k)) & 3] for k in range(4))
        if p.regtype in (7, 14, 15):
            raise Unsupported("integer/bool register as float source")
        if comps is not None:
            sw = "".join(sw[SW.index(c)] for c in comps)
        e = base if sw == "xyzw" else f"{base}.{sw}"
        m = p.smod
        if m == 0:
            return e
        if m == 1:
            return f"(-{e})"
        if m == 11:
            return f"abs({e})"
        if m == 12:
            return f"(-abs({e}))"
        if m == 2:
            return f"({e} - 0.5f)"
        if m == 3:
            return f"(0.5f - {e})"
        if m == 4:
            return f"({e} * 2.0f - 1.0f)"
        if m == 5:
            return f"(1.0f - {e} * 2.0f)"
        if m == 6:
            return f"(1.0f - {e})"
        if m == 7:
            return f"({e} * 2.0f)"
        if m == 8:
            return f"(-{e} * 2.0f)"
        raise Unsupported(f"source modifier {m}")

    def scalar(self, p):
        return self.src(p, "x")

    def store(self, ins, expr, scalar=False):
        """Assign float4 expression to dst honouring mask, _sat. scalar=True: expr is a float."""
        d = ins.dst
        if d.shift:
            raise Unsupported("result shift")
        if d.relative:
            raise Unsupported("relative-addressed destination")
        lhs = self.reg(d, True)
        mask = "".join(SW[k] for k in range(4) if d.mask >> k & 1)
        if scalar:
            e = expr if not d.dmod & 1 else f"saturate({expr})"
            val = e if len(mask) == 1 else f"float{len(mask)}({e})"
        else:
            e = f"({expr})"
            if d.dmod & 1:
                e = f"saturate{e}"
            val = e if mask == "xyzw" else f"{e}.{mask}"
        if lhs in ("oFog", "oPts", "oDepth") or any(  # scalar outputs
                d0.get("reg") == lhs and d0.get("usage") == "psize" for d0 in self.outputs.values()):
            if not scalar:
                e = f"({expr})" if not d.dmod & 1 else f"saturate({expr})"
                expr1 = f"({e}).{mask[0]}"
            else:
                expr1 = expr if not d.dmod & 1 else f"saturate({expr})"
            self.w(f"{lhs} = {expr1};")
            return
        if d.regtype == 3 and self.vs:  # address register: round to nearest even
            val = f"int{len(mask) if len(mask) > 1 else ''}(rint({val}))"
        self.w(f"{lhs}{'' if mask == 'xyzw' else '.' + mask} = {val};")

    # ------------------------------------------------------------ declarations
    def scan(self):
        for ins in self.sh.insns:
            if ins.op == 81:
                self.defs_f[ins.dst.num] = ins.extra
            elif ins.op == 48:
                self.defs_i[ins.dst.num] = ins.extra
            elif ins.op == 47:
                self.defs_b[ins.dst.num] = ins.extra[0]
            elif ins.op == 31:
                u = ins.extra[0]
                d = ins.dst
                rt, n = d.regtype, d.num
                if rt == 10:
                    self.samplers[n] = d3dbc.TEXTYPE.get((u >> 27) & 0xF, "unk")
                    continue
                usage, idx = u & 0x1F, (u >> 16) & 0xF
                if usage not in d3dbc.USAGE:
                    raise Unsupported(f"dcl usage {usage}")
                cen = bool(d.dmod & 4)
                flat = bool(self.opts.get("flat")) and not self.vs and (
                    d3dbc.USAGE[usage] == "color" if self.sm3 else rt == 1)
                if self.vs and rt == 1:
                    self.inputs[(1, n)] = dict(reg=f"v{n}", usage=d3dbc.USAGE.get(usage), index=idx, attribute=n)
                elif self.vs and rt == 6:
                    self.outputs[(6, n)] = dict(reg=f"o{n}", usage=d3dbc.USAGE.get(usage), index=idx)
                elif not self.vs and self.sm3 and rt == 1:
                    self.inputs[(1, n)] = dict(reg=f"v{n}", usage=d3dbc.USAGE.get(usage), index=idx, centroid=cen,
                                               flat=flat)
                elif not self.vs and not self.sm3 and rt == 3:
                    self.inputs[(3, n)] = dict(reg=f"t{n}", usage="texcoord", index=n, centroid=cen, flat=flat)
                elif not self.vs and not self.sm3 and rt == 1:
                    self.inputs[(1, n)] = dict(reg=f"d{n}", usage="color", index=n, centroid=cen, flat=flat)
                elif rt == 17:
                    pass
                else:
                    raise Unsupported(f"dcl of register type {rt}")

    # ------------------------------------------------------------ body
    def sample(self, ins, coord_p, samp_p, mode):
        """D3D9 texld forms through the prelude's sg_tex* helpers, which apply the per-sampler state of
        SgAux.samp[n] (LOD bias, sRGB read, border colour, depth compare) that Metal samplers cannot hold."""
        n = samp_p.num
        ty = self.samplers.get(n, "2d")
        fn = {"2d": "sg_tex2d", "cube": "sg_texcube", "volume": "sg_tex3d"}.get(ty)
        if fn is None:
            raise Unsupported(f"sampler type {ty}")
        c4 = self.src(coord_p)
        tex, smp, st = f"t{n}", f"s{n}", f"aux.samp[{n}]"
        if mode == "p":
            e = f"{fn}_b({tex}, {smp}, sg_proj({c4}), 0.0f, {st})"
        elif mode == "b":
            e = f"{fn}_b({tex}, {smp}, {c4}, ({c4}).w, {st})"
        elif mode == "l":
            e = f"{fn}_l({tex}, {smp}, {c4}, ({c4}).w, {st})"
        elif mode == "d":
            e = f"{fn}_d({tex}, {smp}, {c4}, {self.src(ins.srcs[2])}, {self.src(ins.srcs[3])}, {st})"
        elif self.vs:
            e = f"{fn}_l({tex}, {smp}, {c4}, 0.0f, {st})"
        else:
            e = f"{fn}_b({tex}, {smp}, {c4}, 0.0f, {st})"
        sw = "".join(SW[(samp_p.swizzle >> (2 * k)) & 3] for k in range(4))
        if sw != "xyzw":
            e = f"({e}).{sw}"
        return e

    def body(self):
        for ins in self.sh.insns:
            op = ins.op
            if op in (0xFFFE, 31, 81, 48, 47, 0):
                continue
            if op == 0xFFFF:
                break
            if ins.predicated:
                raise Unsupported("predication")
            nm = ins.name
            s = ins.srcs
            if nm == "mov":
                self.store(ins, self.src(s[0]))
            elif nm == "mova":
                self.store(ins, self.src(s[0]))
            elif nm in ("add", "sub", "mul"):
                o = {"add": "+", "sub": "-", "mul": "*"}[nm]
                self.store(ins, f"{self.src(s[0])} {o} {self.src(s[1])}")
            elif nm == "mad":
                self.store(ins, f"{self.src(s[0])} * {self.src(s[1])} + {self.src(s[2])}")
            elif nm == "rcp":
                self.store(ins, f"1.0f / {self.scalar(s[0])}", scalar=True)
            elif nm == "rsq":
                self.store(ins, f"rsqrt(abs({self.scalar(s[0])}))", scalar=True)
            elif nm in ("exp", "expp"):
                self.store(ins, f"exp2({self.scalar(s[0])})", scalar=True)
            elif nm in ("log", "logp"):
                self.store(ins, f"log2(abs({self.scalar(s[0])}))", scalar=True)
            elif nm == "pow":
                self.store(ins, f"pow(abs({self.scalar(s[0])}), {self.scalar(s[1])})", scalar=True)
            elif nm == "frc":
                self.store(ins, f"fract({self.src(s[0])})")
            elif nm in ("min", "max"):
                self.store(ins, f"{nm}({self.src(s[0])}, {self.src(s[1])})")
            elif nm in ("slt", "sge"):
                o = "<" if nm == "slt" else ">="
                self.store(ins, f"select(float4(0.0f), float4(1.0f), {self.src(s[0])} {o} {self.src(s[1])})")
            elif nm in ("dp3", "dp4"):
                cs = "xyz" if nm == "dp3" else "xyzw"
                self.store(ins, f"dot({self.src(s[0], cs)}, {self.src(s[1], cs)})", scalar=True)
            elif nm == "dp2add":
                self.store(ins, f"dot({self.src(s[0], 'xy')}, {self.src(s[1], 'xy')}) + {self.scalar(s[2])}",
                           scalar=True)
            elif nm == "lrp":
                self.store(ins, f"mix({self.src(s[2])}, {self.src(s[1])}, {self.src(s[0])})")
            elif nm == "cmp":
                self.store(ins, f"select({self.src(s[2])}, {self.src(s[1])}, {self.src(s[0])} >= 0.0f)")
            elif nm == "abs":
                self.store(ins, f"abs({self.src(s[0])})")
            elif nm == "sgn":
                self.store(ins, f"sign({self.src(s[0])})")
            elif nm == "nrm":
                a = self.src(s[0])
                self.store(ins, f"{a} * rsqrt(dot(({a}).xyz, ({a}).xyz))")
            elif nm == "crs":
                self.store(ins, f"float4(cross({self.src(s[0], 'xyz')}, {self.src(s[1], 'xyz')}), 0.0f)")
            elif nm == "sincos":
                x = self.scalar(s[0])
                self.store(ins, f"float4(cos({x}), sin({x}), 0.0f, 0.0f)")
            elif nm == "lit":
                self.store(ins, f"sg_lit({self.src(s[0])})")
            elif nm == "dst":
                self.store(ins, f"float4(1.0f, ({self.src(s[0])}).y * ({self.src(s[1])}).y, "
                                f"({self.src(s[0])}).z, ({self.src(s[1])}).w)")
            elif nm in ("m4x4", "m4x3", "m3x4", "m3x3", "m3x2"):
                k = int(nm[1])
                rows = int(nm[3])
                cs = "xyzw"[:k]
                v = self.src(s[0], cs)
                parts = []
                for j in range(rows):
                    q = d3dbc.Param(s[1].token + j, s[1].rel)
                    parts.append(f"dot({v}, {self.src(q, cs)})")
                while len(parts) < 4:
                    parts.append("0.0f")
                self.store(ins, f"float4({', '.join(parts)})")
            elif nm == "texld":
                mode = {0: "", 1: "p", 2: "b"}[ins.controls & 3]
                self.store(ins, self.sample(ins, s[0], s[1], mode))
            elif nm == "texldl":
                self.store(ins, self.sample(ins, s[0], s[1], "l"))
            elif nm == "texldd":
                self.store(ins, self.sample(ins, s[0], s[1], "d"))
            elif nm == "texkill":
                d = ins.dst
                mask = "".join(SW[k] for k in range(4) if d.mask >> k & 1)
                self.uses.add("texkill")
                self.w(f"if (any(({self.reg(d)}).{mask} < 0.0f)) discard_fragment();")
            elif nm in ("dsx", "dsy"):
                self.store(ins, f"{'dfdx' if nm == 'dsx' else 'dfdy'}({self.src(s[0])})")
            elif nm == "if":
                self.w(f"if ({self.reg(s[0])}) {{")
                self.ind += 1
            elif nm == "ifc":
                self.w(f"if ({self.scalar(s[0])} {CMPOP[ins.controls & 7]} {self.scalar(s[1])}) {{")
                self.ind += 1
            elif nm == "else":
                self.ind -= 1
                self.w("} else {")
                self.ind += 1
            elif nm in ("endif", "endloop", "endrep"):
                self.ind -= 1
                self.w("}")
                if nm in ("endloop", "endrep"):
                    self.loop_depth -= 1
            elif nm == "loop":
                self.loop_depth += 1
                L = f"_L{self.loop_depth}"
                self.w(f"for (int4 {L} = {self.reg(s[1])}, _n = int4(0); _n.x < {L}.x; ++_n.x) {{")
                self.ind += 1
                self.w(f"int aL = {L}.y + _n.x * {L}.z;")
            elif nm == "rep":
                self.loop_depth += 1
                L = f"_L{self.loop_depth}"
                self.w(f"for (int {L} = 0; {L} < {self.reg(s[0])}.x; ++{L}) {{")
                self.ind += 1
            elif nm == "break":
                self.w("break;")
            elif nm == "breakc":
                self.w(f"if ({self.scalar(s[0])} {CMPOP[ins.controls & 7]} {self.scalar(s[1])}) break;")
            elif nm == "phase":
                pass
            else:
                raise Unsupported(f"opcode {nm}")

    # ------------------------------------------------------------ assembly of the function
    def build(self):
        self.scan()
        self.body()
        body = self.lines
        self.lines = []
        L = self.lines
        sh = self.sh
        L.append(f"// {sh.profile} -> MSL (spore-native d3d9msl {VERSION})")
        # constant defs
        pre = []
        for n, v in sorted(self.defs_f.items()):
            pre.append(f"const float4 kc{n} = float4({', '.join(_f32(x) for x in v)});")
        for n, v in sorted(self.defs_i.items()):
            pre.append(f"const int4 ki{n} = int4({', '.join(str(struct.unpack('<i', struct.pack('<I', x))[0]) for x in v)});")
        for n, v in sorted(self.defs_b.items()):
            pre.append(f"const bool kb{n} = {'true' if v else 'false'};")
        nconst = 256 if self.vs else 224
        # structs
        ins_name = f"{self.entry}_in"
        out_name = f"{self.entry}_out"
        L.append(f"struct {ins_name} {{")
        for (rt, n), d in sorted(self.inputs.items()):
            if self.vs:
                L.append(f"    float4 v{n} [[attribute({n})]];")
            else:
                interp = (", centroid_perspective" if d.get("centroid") else "") + (", flat" if d.get("flat") else "")
                L.append(f"    float4 {d['reg']} [[user({d['usage']}{d['index']}){interp}]];")
        if not self.vs and self.opts.get("fog") == "vertex":
            L.append("    float sg_fog [[user(fog0)]];")
        L.append("};")
        has_in = len(L[-1]) and L[-2] != f"struct {ins_name} {{"
        if not has_in:
            L.pop()
            L.pop()
        L.append(f"struct {out_name} {{")
        outs = []
        if self.vs:
            for (rt, n), d in sorted(self.outputs.items()):
                if rt == 4:
                    if n == 0:
                        outs.append(("oPos", "float4", "[[position]]", "float4(0.0f)"))
                    elif n == 1:
                        outs.append(("oFog", "float", "[[user(fog0)]]", "1.0f"))
                    else:
                        outs.append(("oPts", "float", "[[point_size]]", "1.0f"))
                    continue
                u, i = d["usage"], d["index"]
                if u == "position" and i == 0:
                    attr = "[[position]]"
                elif u == "psize":
                    attr = "[[point_size]]"
                else:
                    attr = f"[[user({u}{i})]]"
                ty = "float" if attr == "[[point_size]]" else "float4"
                if u == "color":
                    dflt = "float4(1.0f)" if i == 0 else "float4(0.0f, 0.0f, 0.0f, 1.0f)"
                elif u == "fog":
                    dflt = "float4(1.0f)"
                else:
                    dflt = "float4(0.0f)" if ty == "float4" else "1.0f"
                outs.append((d["reg"], ty, attr, dflt))
        else:
            for (rt, n), d in sorted(self.outputs.items()):
                if rt == 8:
                    outs.append((f"oC{n}", "float4", f"[[color({n})]]", "float4(0.0f)"))
                elif rt == 9:
                    outs.append(("oDepth", "float", "[[depth(any)]]", "0.0f"))
        for nm, ty, attr, _ in outs:
            L.append(f"    {ty} {nm} {attr};")
        L.append("};")
        # signature
        args = ([f"{ins_name} in [[stage_in]]"] if has_in else []) + [
                "constant float4* c [[buffer(0)]]", "constant int4* ci [[buffer(1)]]",
                "constant uint* cb [[buffer(2)]]", "constant SgAux& aux [[buffer(3)]]"]
        for n, ty in sorted(self.samplers.items()):
            tt = {"2d": "texture2d<float>", "cube": "texturecube<float>", "volume": "texture3d<float>"}[ty]
            args.append(f"{tt} t{n} [[texture({n})]]")
            args.append(f"sampler s{n} [[sampler({n})]]")
        if "vPos" in self.uses:
            args.append("float4 sg_pos [[position]]")
        if "vFace" in self.uses:
            args.append("bool sg_front [[front_facing]]")
        stage = "vertex" if self.vs else "fragment"
        if self.opts.get("plain"):
            # test mode: an ordinary function a compute kernel can call (no stage attributes)
            if not self.vs or self.samplers:
                raise Unsupported("plain mode is for ALU-only vertex shaders")
            stage = "static"
            args = [a.split(" [[")[0] for a in args]
            if has_in:
                args[0] = f"thread const {ins_name}& in"
        L.append(f"{stage} {out_name} {self.entry}(" + ",\n        ".join(args) + ") {")
        for p in pre:
            L.append("    " + p)
        for n in sorted(self.temps):
            L.append(f"    float4 r{n} = float4(0.0f);")
        if self.vs:
            L.append("    int4 a0 = int4(0);")
        for nm, ty, attr, dflt in outs:
            L.append(f"    {ty} {nm} = {dflt};")
        L.extend(body)
        pos = [nm for nm, ty, attr, _ in outs if attr == "[[position]]"]
        if self.vs and pos:
            # D3D9 pixel centres are at integer coordinates: the runtime puts (+1/vpW, -1/vpH) here, or zero
            # when it shifts the Metal viewport by +0.5 px instead (runtime/d3d9metal/shader/d3d9msl.h)
            L.append(f"    {pos[0]}.xy += aux.posFixup.xy * {pos[0]}.w;")
        if not self.vs:
            if (8, 0) in self.outputs:
                if self.opts.get("fog") == "vertex" and not self.sm3:
                    L.append("    oC0.rgb = mix(aux.fogColor.rgb, oC0.rgb, saturate(in.sg_fog));")
                L.append("    if (!sg_alpha_test(oC0.a, aux.alphaRef)) discard_fragment();")
        L.append(f"    {out_name} out;")
        for nm, ty, attr, dflt in outs:
            L.append(f"    out.{nm} = {nm};")
        L.append("    return out;")
        L.append("}")
        text = "\n".join(L) + "\n"
        if self.rel_c:
            # def'd registers override the constant buffer for relative reads too
            sw = "".join(f"    if (i == {n}) return float4({', '.join(_f32(x) for x in v)});\n"
                         for n, v in sorted(self.defs_f.items()))
            helper = (f"static float4 sg_ldc_{self.entry}(constant float4* c, int i) {{\n{sw}"
                      f"    return (i >= 0 && i < {nconst}) ? c[i] : float4(0.0f);\n}}\n")
            text = helper + text.replace("sg_ldc(", f"sg_ldc_{self.entry}(")
        self.refl.update(
            inputs=[d for _, d in sorted(self.inputs.items())],
            outputs=[dict(reg=nm, type=ty, binding=attr) for nm, ty, attr, _ in outs],
            samplers={str(k): v for k, v in sorted(self.samplers.items())},
            float_constants_max_index=self.max_c, relative_constants=self.rel_c,
            float_constants_used=sorted(self.c_used), int_constants_used=sorted(self.i_used),
            bool_constants_used=sorted(self.b_used),
            uses=sorted(self.uses), position_fixup=bool(self.vs and pos),
            alpha_test=(not self.vs and (8, 0) in self.outputs),
            fog_input=(not self.vs and self.opts.get("fog") == "vertex"),
            defs=len(self.defs_f) + len(self.defs_i) + len(self.defs_b),
        )
        return text


VERSION = "1"   # bump with any change to the emitted text (it is part of the run-time cache key)

PRELUDE = """#include <metal_stdlib>
using namespace metal;
struct SgSampler {
    float4 border;           // D3DSAMP_BORDERCOLOR as RGBA 0..1, used where an SG_SAMP_BORDER_* bit is set
    float mipBias;           // D3DSAMP_MIPMAPLODBIAS (a Metal sampler has no LOD bias)
    uint flags;              // SG_SAMP_* below
    float _pad1[2];
};
struct SgAux {
    float alphaRef;          // D3DRS_ALPHAREF / 255
    float _pad0[3];
    float4 fogColor;         // D3DRS_FOGCOLOR
    float4 fogParams;        // start, end, density, mode
    float4 rtSize;           // width, height, 1/width, 1/height
    float4 posFixup;         // VS: added to oPos.xy per unit of oPos.w (half-pixel offset), or zero
    SgSampler samp[16];      // PS: sampler s0..s15; VS: vertex texture sampler 0..3
};
constant uint SG_SAMP_SRGB = 1u;       // D3DSAMP_SRGBTEXTURE: decode sRGB after the fetch
constant uint SG_SAMP_BORDER_U = 2u;   // D3DTADDRESS_BORDER on U with a colour the Metal sampler cannot hold
constant uint SG_SAMP_BORDER_V = 4u;
constant uint SG_SAMP_BORDER_W = 8u;
constant uint SG_SAMP_DEPTH_CMP = 16u; // depth texture: return (ref <= depth) like D3D9 shadow-map hardware
constant uint sg_alpha_func [[function_constant(0)]];
constant bool sg_alpha_func_set = is_function_constant_defined(sg_alpha_func);
static float4 sg_fin(float4 r, float4 c, constant SgSampler& st, uint dims) {
    uint f = st.flags;
    if (f == 0u) return r;
    if ((f & SG_SAMP_SRGB) != 0u)
        r.rgb = select(pow((r.rgb + 0.055f) / 1.055f, float3(2.4f)), r.rgb / 12.92f, r.rgb <= 0.04045f);
    uint b = f & dims;
    if (b != 0u) {
        bool ox = (b & SG_SAMP_BORDER_U) != 0u && (c.x < 0.0f || c.x > 1.0f);
        bool oy = (b & SG_SAMP_BORDER_V) != 0u && (c.y < 0.0f || c.y > 1.0f);
        bool oz = (b & SG_SAMP_BORDER_W) != 0u && (c.z < 0.0f || c.z > 1.0f);
        if (ox || oy || oz) r = st.border;
    }
    if ((f & SG_SAMP_DEPTH_CMP) != 0u) r = float4(c.z <= r.x ? 1.0f : 0.0f);
    return r;
}
static float4 sg_proj(float4 c) { return float4(c.xyz / c.w, c.w); }
static float4 sg_tex2d_b(texture2d<float> t, sampler s, float4 c, float b, constant SgSampler& st) {
    return sg_fin(t.sample(s, c.xy, bias(b + st.mipBias)), c, st, 6u);
}
static float4 sg_tex2d_l(texture2d<float> t, sampler s, float4 c, float l, constant SgSampler& st) {
    return sg_fin(t.sample(s, c.xy, level(l + st.mipBias)), c, st, 6u);
}
static float4 sg_tex2d_d(texture2d<float> t, sampler s, float4 c, float4 dx, float4 dy, constant SgSampler& st) {
    return sg_fin(t.sample(s, c.xy, gradient2d(dx.xy, dy.xy)), c, st, 6u);
}
static float4 sg_texcube_b(texturecube<float> t, sampler s, float4 c, float b, constant SgSampler& st) {
    return sg_fin(t.sample(s, c.xyz, bias(b + st.mipBias)), c, st, 0u);
}
static float4 sg_texcube_l(texturecube<float> t, sampler s, float4 c, float l, constant SgSampler& st) {
    return sg_fin(t.sample(s, c.xyz, level(l + st.mipBias)), c, st, 0u);
}
static float4 sg_texcube_d(texturecube<float> t, sampler s, float4 c, float4 dx, float4 dy, constant SgSampler& st) {
    return sg_fin(t.sample(s, c.xyz, gradientcube(dx.xyz, dy.xyz)), c, st, 0u);
}
static float4 sg_tex3d_b(texture3d<float> t, sampler s, float4 c, float b, constant SgSampler& st) {
    return sg_fin(t.sample(s, c.xyz, bias(b + st.mipBias)), c, st, 14u);
}
static float4 sg_tex3d_l(texture3d<float> t, sampler s, float4 c, float l, constant SgSampler& st) {
    return sg_fin(t.sample(s, c.xyz, level(l + st.mipBias)), c, st, 14u);
}
static float4 sg_tex3d_d(texture3d<float> t, sampler s, float4 c, float4 dx, float4 dy, constant SgSampler& st) {
    return sg_fin(t.sample(s, c.xyz, gradient3d(dx.xyz, dy.xyz)), c, st, 14u);
}
static bool sg_alpha_test(float a, float ref) {
    uint f = sg_alpha_func_set ? sg_alpha_func : 8u;
    // D3D9 compares at render-target precision (8 bits for the formats Spore uses)
    float qa = rint(saturate(a) * 255.0f), qr = rint(saturate(ref) * 255.0f);
    switch (f) {
        case 1u: return false;
        case 2u: return qa <  qr;
        case 3u: return qa == qr;
        case 4u: return qa <= qr;
        case 5u: return qa >  qr;
        case 6u: return qa != qr;
        case 7u: return qa >= qr;
        default: return true;
    }
}
static float4 sg_lit(float4 s) {
    float4 r = float4(1.0f, 0.0f, 0.0f, 1.0f);
    float p = clamp(s.w, -127.9961f, 127.9961f);
    if (s.x > 0.0f) {
        r.y = s.x;
        if (s.y > 0.0f) r.z = pow(s.y, p);
    }
    return r;
}
"""


def translate(sh, entry="sg_main", options=None, prelude=True):
    e = _Emit(sh, entry, options or {})
    text = e.build()
    return ((PRELUDE if prelude else "") + text), e.refl


if __name__ == "__main__":
    a = sys.argv[1:]
    entry = "sg_main"
    opts = {}
    if "--entry" in a:
        entry = a[a.index("--entry") + 1]
    if "--fog" in a:
        opts["fog"] = a[a.index("--fog") + 1]
    if "--flat" in a:
        opts["flat"] = True
    if "--plain" in a:
        opts["plain"] = True
    sh = d3dbc.parse(open(a[0], "rb").read())
    src, refl = translate(sh, entry, opts)
    sys.stdout.write(src)
    if "--json" in a:
        print("/* reflection\n" + json.dumps(refl, indent=1) + "\n*/")
