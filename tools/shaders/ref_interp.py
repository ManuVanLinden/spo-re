"""Reference interpreter for D3D9 vertex-shader ALU semantics (vs_2_0 / vs_3_0), written from the
D3D9 instruction reference (MSDN "Vertex Shader Instructions"). Used only to check the MSL emitter on
our own fixtures: every result is rounded to float32 after each instruction.

    outs = run(shader, inputs={reg: [x,y,z,w]}, c=[[...]*4]*256, i=[[...]*4]*16, b=[0/1]*16)
    -> {output register name: [x, y, z, w]}
"""
import math
import struct
import sys
import os

sys.path.insert(0, os.path.dirname(os.path.abspath(__file__)))
import d3dbc  # noqa: E402

FMAX = 3.4028234663852886e38


def f32(x):
    if x != x:
        return x
    if x > FMAX:
        return math.inf
    if x < -FMAX:
        return -math.inf
    return struct.unpack("<f", struct.pack("<f", x))[0]


def _rcp(x):
    if x == 0:
        return math.copysign(math.inf, x)
    return 1.0 / x


def _rsq(x):
    x = abs(x)
    return math.inf if x == 0 else 1.0 / math.sqrt(x)


def _log2(x):
    x = abs(x)
    if x == 0:
        return -math.inf
    return math.log2(x) if x != math.inf else math.inf


def _exp2(x):
    try:
        return 2.0 ** x
    except OverflowError:
        return math.inf


def _pow(x, y):
    x = abs(x)
    try:
        return x ** y
    except (OverflowError, ZeroDivisionError):
        return math.inf


class _State:
    def __init__(self, sh, inputs, c, i, b):
        self.sh = sh
        self.r = {}
        self.v = {k: [f32(x) for x in val] for k, val in inputs.items()}
        self.c = [list(map(f32, x)) for x in c]
        self.i = [list(x) for x in i]
        self.b = list(b)
        self.a0 = [0, 0, 0, 0]
        self.aL = 0
        self.out = {}
        self.written = {}  # output name -> component mask written

    def addr(self, p):
        if p.rel is None:
            return self.a0[0]
        r = d3dbc.Param(p.rel)
        if r.regtype == 15:
            return self.aL
        return self.a0[r.swizzle & 3]

    def read(self, p):
        rt, n = p.regtype, p.num
        if rt == 0:
            val = self.r.get(n, [0.0] * 4)
        elif rt == 1:
            val = self.v.get(n, [0.0, 0.0, 0.0, 1.0])
        elif rt == 2:
            idx = n + (self.addr(p) if p.relative else 0)
            val = self.c[idx] if 0 <= idx < len(self.c) else [0.0] * 4
        elif rt == 3:
            val = [float(x) for x in self.a0]
        elif rt == 15:
            val = [float(self.aL)] * 4
        else:
            raise NotImplementedError(f"read regtype {rt}")
        sw = p.swizzle
        val = [val[(sw >> (2 * k)) & 3] for k in range(4)]
        m = p.smod
        if m == 1:
            val = [-x for x in val]
        elif m == 11:
            val = [abs(x) for x in val]
        elif m == 12:
            val = [-abs(x) for x in val]
        elif m:
            raise NotImplementedError(f"srcmod {m}")
        return val

    def outname(self, p):
        rt, n = p.regtype, p.num
        if rt == 4:
            return {0: "oPos", 1: "oFog", 2: "oPts"}[n]
        if rt == 5:
            return f"oD{n}"
        if rt == 6:
            return f"o{n}" if self.sh.major == 3 else f"oT{n}"
        return None

    def write(self, ins, val):
        d = ins.dst
        val = [f32(x) for x in val]
        if d.dmod & 1:
            val = [min(1.0, max(0.0, x)) if x == x else 0.0 for x in val]
        rt, n = d.regtype, d.num
        if rt == 3:  # a0: round to nearest even
            for k in range(4):
                if d.mask >> k & 1:
                    self.a0[k] = int(round(val[k]))
            return
        if rt == 0:
            cur = self.r.setdefault(n, [0.0] * 4)
        else:
            cur = self.out.setdefault(self.outname(d), [0.0] * 4)
            self.written[self.outname(d)] = self.written.get(self.outname(d), 0) | d.mask
        for k in range(4):
            if d.mask >> k & 1:
                cur[k] = val[k]


def _match(insns):
    """Map control-flow instruction index -> partner index."""
    stack, m = [], {}
    for k, ins in enumerate(insns):
        nm = ins.name
        if nm in ("if", "ifc", "loop", "rep"):
            stack.append(k)
        elif nm == "else":
            m[stack[-1]] = k
            m[k] = None  # filled at endif
            stack.append(("else", k))
        elif nm in ("endif", "endloop", "endrep"):
            top = stack.pop()
            if isinstance(top, tuple):
                m[top[1]] = k
                top = stack.pop()
                m.setdefault(top, k)
                m[("end", top)] = k
            else:
                m[top] = k
                m[("end", top)] = k
    return m


CMPF = {1: lambda a, b: a > b, 2: lambda a, b: a == b, 3: lambda a, b: a >= b, 4: lambda a, b: a < b,
        5: lambda a, b: a != b, 6: lambda a, b: a <= b}


def run(sh, inputs, c, i, b):
    insns = [x for x in sh.insns if x.op not in (0xFFFE,)]
    st = _State(sh, inputs, c, i, b)
    for x in insns:  # defs override constants
        if x.op == 81:
            st.c[x.dst.num] = [f32(struct.unpack("<f", struct.pack("<I", t))[0]) for t in x.extra]
        elif x.op == 48:
            st.i[x.dst.num] = [struct.unpack("<i", struct.pack("<I", t))[0] for t in x.extra]
        elif x.op == 47:
            st.b[x.dst.num] = x.extra[0]
    m = _match(insns)
    loops = []  # (start index, remaining, step, saved aL)
    pc = 0
    steps = 0
    while pc < len(insns):
        steps += 1
        if steps > 100000:
            raise RuntimeError("runaway")
        ins = insns[pc]
        nm = ins.name
        s = ins.srcs
        R = st.read
        if nm in ("dcl", "def", "defi", "defb", "nop"):
            pass
        elif nm == "end":
            break
        elif nm in ("mov", "mova"):
            st.write(ins, R(s[0]))
        elif nm in ("add", "sub", "mul", "min", "max", "slt", "sge"):
            a, bb = R(s[0]), R(s[1])
            f = {"add": lambda x, y: x + y, "sub": lambda x, y: x - y, "mul": lambda x, y: x * y,
                 "min": min, "max": max, "slt": lambda x, y: 1.0 if x < y else 0.0,
                 "sge": lambda x, y: 1.0 if x >= y else 0.0}[nm]
            st.write(ins, [f(a[k], bb[k]) for k in range(4)])
        elif nm == "mad":
            a, bb, cc = R(s[0]), R(s[1]), R(s[2])
            st.write(ins, [f32(a[k] * bb[k]) + cc[k] for k in range(4)])
        elif nm in ("rcp", "rsq", "exp", "expp", "log", "logp"):
            x = R(s[0])[0]
            f = {"rcp": _rcp, "rsq": _rsq, "exp": _exp2, "expp": _exp2, "log": _log2, "logp": _log2}[nm]
            st.write(ins, [f(x)] * 4)
        elif nm == "pow":
            st.write(ins, [_pow(R(s[0])[0], R(s[1])[0])] * 4)
        elif nm == "frc":
            st.write(ins, [x - math.floor(x) for x in R(s[0])])
        elif nm in ("dp3", "dp4"):
            a, bb = R(s[0]), R(s[1])
            n = 3 if nm == "dp3" else 4
            st.write(ins, [sum(a[k] * bb[k] for k in range(n))] * 4)
        elif nm == "lrp":
            a, bb, cc = R(s[0]), R(s[1]), R(s[2])
            st.write(ins, [a[k] * (bb[k] - cc[k]) + cc[k] for k in range(4)])
        elif nm == "abs":
            st.write(ins, [abs(x) for x in R(s[0])])
        elif nm == "sgn":
            st.write(ins, [(x > 0) - (x < 0) for x in R(s[0])])
        elif nm == "nrm":
            a = R(s[0])
            l2 = a[0] * a[0] + a[1] * a[1] + a[2] * a[2]
            f = _rsq(l2)
            st.write(ins, [x * f for x in a])
        elif nm == "crs":
            a, bb = R(s[0]), R(s[1])
            st.write(ins, [a[1] * bb[2] - a[2] * bb[1], a[2] * bb[0] - a[0] * bb[2], a[0] * bb[1] - a[1] * bb[0], 0.0])
        elif nm == "sincos":
            x = R(s[0])[0]
            st.write(ins, [math.cos(x), math.sin(x), 0.0, 0.0])
        elif nm == "lit":
            a = R(s[0])
            p = max(-127.9961, min(127.9961, a[3]))
            y = a[0] if a[0] > 0 else 0.0
            z = _pow(a[1], p) if (a[0] > 0 and a[1] > 0) else 0.0
            st.write(ins, [1.0, y, z, 1.0])
        elif nm == "dst":
            a, bb = R(s[0]), R(s[1])
            st.write(ins, [1.0, a[1] * bb[1], a[2], bb[3]])
        elif nm in ("m4x4", "m4x3", "m3x4", "m3x3", "m3x2"):
            k, rows = int(nm[1]), int(nm[3])
            a = R(s[0])
            res = [0.0] * 4
            for j in range(rows):
                q = d3dbc.Param(s[1].token + j, s[1].rel)
                row = R(q)
                res[j] = sum(a[t] * row[t] for t in range(k))
            st.write(ins, res)
        elif nm in ("if", "ifc"):
            if nm == "if":
                cond = bool(st.b[s[0].num])
            else:
                cond = CMPF[ins.controls & 7](R(s[0])[0], R(s[1])[0])
            if not cond:
                pc = m[pc] + 1  # partner = else (run else body) or endif
                continue
        elif nm == "else":
            pc = m[pc] + 1
            continue
        elif nm == "endif":
            pass
        elif nm in ("loop", "rep"):
            iv = st.i[s[1].num] if nm == "loop" else st.i[s[0].num]
            if iv[0] <= 0:
                pc = m[("end", pc)] + 1
                continue
            loops.append([pc, iv[0], iv[2], st.aL])
            if nm == "loop":
                st.aL = iv[1]
        elif nm in ("endloop", "endrep"):
            top = loops[-1]
            top[1] -= 1
            if top[1] > 0:
                if nm == "endloop":
                    st.aL += top[2]
                pc = top[0] + 1
                continue
            st.aL = top[3]
            loops.pop()
        elif nm in ("break", "breakc"):
            if nm == "break" or CMPF[ins.controls & 7](R(s[0])[0], R(s[1])[0]):
                top = loops.pop()
                st.aL = top[3]
                pc = m[("end", top[0])] + 1
                continue
        else:
            raise NotImplementedError(nm)
        pc += 1
    return {k: (v, st.written[k]) for k, v in st.out.items()}
