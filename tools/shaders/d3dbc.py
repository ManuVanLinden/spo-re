"""Direct3D 9 shader bytecode (d3dbc, SM1.x-3.0) parser, disassembler and assembler.

Written from the public token-format documentation (MSDN "Shader Binary Format":
version/instruction/parameter/comment tokens) and the D3DSHADER_INSTRUCTION_OPCODE_TYPE
and D3DSHADER_PARAM_REGISTER_TYPE enums in d3d9types.h.

Library use:
    sh = parse(blob_bytes)          # -> Shader; raises ParseError on any malformed token
    text = disassemble(sh)          # canonical text, one instruction per line
    toks = assemble(text)           # list[int] tokens; assemble(disassemble(sh)) == sh.tokens

CLI:
    d3dbc.py dis <blob> [--no-comments]
    d3dbc.py roundtrip <blob>...

Used by msl.py, the tests here and runtime/d3d9metal/tests/gen_shaders.py (which assembles the Metal back end's
test shaders at build time).
"""
import struct
import sys

# ---------------------------------------------------------------------------- tables

# opcode -> (name, has_dst, nsrc for SM2+ checking (int or tuple of allowed counts))
OPS = {
    0: ("nop", 0, 0), 1: ("mov", 1, 1), 2: ("add", 1, 2), 3: ("sub", 1, 2), 4: ("mad", 1, 3),
    5: ("mul", 1, 2), 6: ("rcp", 1, 1), 7: ("rsq", 1, 1), 8: ("dp3", 1, 2), 9: ("dp4", 1, 2),
    10: ("min", 1, 2), 11: ("max", 1, 2), 12: ("slt", 1, 2), 13: ("sge", 1, 2), 14: ("exp", 1, 1),
    15: ("log", 1, 1), 16: ("lit", 1, 1), 17: ("dst", 1, 2), 18: ("lrp", 1, 3), 19: ("frc", 1, 1),
    20: ("m4x4", 1, 2), 21: ("m4x3", 1, 2), 22: ("m3x4", 1, 2), 23: ("m3x3", 1, 2), 24: ("m3x2", 1, 2),
    25: ("call", 0, 1), 26: ("callnz", 0, 2), 27: ("loop", 0, 2), 28: ("ret", 0, 0),
    29: ("endloop", 0, 0), 30: ("label", 0, 1), 31: ("dcl", 1, 0), 32: ("pow", 1, 2), 33: ("crs", 1, 2),
    34: ("sgn", 1, 3), 35: ("abs", 1, 1), 36: ("nrm", 1, 1), 37: ("sincos", 1, (1, 3)), 38: ("rep", 0, 1),
    39: ("endrep", 0, 0), 40: ("if", 0, 1), 41: ("ifc", 0, 2), 42: ("else", 0, 0), 43: ("endif", 0, 0),
    44: ("break", 0, 0), 45: ("breakc", 0, 2), 46: ("mova", 1, 1), 47: ("defb", 1, 0), 48: ("defi", 1, 0),
    64: ("texcoord", 1, (0, 1)), 65: ("texkill", 1, 0), 66: ("texld", 1, (0, 1, 2)), 67: ("texbem", 1, 1),
    68: ("texbeml", 1, 1), 69: ("texreg2ar", 1, 1), 70: ("texreg2gb", 1, 1), 71: ("texm3x2pad", 1, 1),
    72: ("texm3x2tex", 1, 1), 73: ("texm3x3pad", 1, 1), 74: ("texm3x3tex", 1, 1), 76: ("texm3x3spec", 1, 2),
    77: ("texm3x3vspec", 1, 1), 78: ("expp", 1, 1), 79: ("logp", 1, 1), 80: ("cnd", 1, 3), 81: ("def", 1, 0),
    82: ("texreg2rgb", 1, 1), 83: ("texdp3tex", 1, 1), 84: ("texm3x2depth", 1, 1), 85: ("texdp3", 1, 1),
    86: ("texm3x3", 1, 1), 87: ("texdepth", 1, 0), 88: ("cmp", 1, 3), 89: ("bem", 1, 2), 90: ("dp2add", 1, 3),
    91: ("dsx", 1, 1), 92: ("dsy", 1, 1), 93: ("texldd", 1, 4), 94: ("setp", 1, 2), 95: ("texldl", 1, 2),
    96: ("breakp", 0, 1), 0xFFFD: ("phase", 0, 0),
}
NAME2OP = {v[0]: k for k, v in OPS.items()}

# register type -> asm prefix (vs / ps differ for 3, 4, 5, 6)
REGTYPES = {
    0: "r", 1: "v", 2: "c", 3: "a/t", 4: "oRast", 5: "oD", 6: "oT/o", 7: "i", 8: "oC", 9: "oDepth",
    10: "s", 11: "c2k", 12: "c4k", 13: "c6k", 14: "b", 15: "aL", 16: "half", 17: "misc", 18: "l", 19: "p",
}
RASTOUT = {0: "oPos", 1: "oFog", 2: "oPts"}
MISC = {0: "vPos", 1: "vFace"}
CMP = {1: "gt", 2: "eq", 3: "ge", 4: "lt", 5: "ne", 6: "le"}
CMP_R = {v: k for k, v in CMP.items()}
SRCMOD = {0: "", 1: "neg", 2: "bias", 3: "biasneg", 4: "sign", 5: "signneg", 6: "comp", 7: "x2",
          8: "x2neg", 9: "dz", 10: "dw", 11: "abs", 12: "absneg", 13: "not"}
SRCMOD_R = {v: k for k, v in SRCMOD.items()}
USAGE = {0: "position", 1: "blendweight", 2: "blendindices", 3: "normal", 4: "psize", 5: "texcoord",
         6: "tangent", 7: "binormal", 8: "tessfactor", 9: "positiont", 10: "color", 11: "fog", 12: "depth",
         13: "sample"}
USAGE_R = {v: k for k, v in USAGE.items()}
TEXTYPE = {0: "unk", 2: "2d", 3: "cube", 4: "volume"}
TEXTYPE_R = {v: k for k, v in TEXTYPE.items()}
SWZ = "xyzw"


class ParseError(Exception):
    pass


# ---------------------------------------------------------------------------- data model

class Param:
    __slots__ = ("token", "rel")

    def __init__(self, token, rel=None):
        self.token = token
        self.rel = rel  # relative-address token (SM2+/vs3 out), or None

    @property
    def regtype(self):
        return ((self.token >> 28) & 7) | ((self.token >> 8) & 0x18)

    @property
    def num(self):
        return self.token & 0x7FF

    @property
    def relative(self):
        return bool(self.token & 0x2000)

    # dst
    @property
    def mask(self):
        return (self.token >> 16) & 0xF

    @property
    def dmod(self):
        return (self.token >> 20) & 0xF

    @property
    def shift(self):
        return (self.token >> 24) & 0xF

    # src
    @property
    def swizzle(self):
        return (self.token >> 16) & 0xFF

    @property
    def smod(self):
        return (self.token >> 24) & 0xF


class Insn:
    __slots__ = ("op", "token", "dst", "pred", "srcs", "extra", "offset")

    def __init__(self, op, token, offset):
        self.op = op
        self.token = token
        self.offset = offset  # token index of the opcode token
        self.dst = None
        self.pred = None
        self.srcs = []
        self.extra = []       # dcl usage token / def immediates / comment payload

    @property
    def name(self):
        return OPS[self.op][0] if self.op in OPS else {0xFFFE: "comment", 0xFFFF: "end"}.get(self.op, "?")

    @property
    def controls(self):
        return (self.token >> 16) & 0xFF

    @property
    def predicated(self):
        return bool(self.token & 0x10000000)

    @property
    def coissue(self):
        return bool(self.token & 0x40000000)


class Shader:
    def __init__(self, tokens):
        self.tokens = tokens
        v = tokens[0]
        self.kind = {0xFFFE: "vs", 0xFFFF: "ps"}.get(v >> 16)
        self.major = (v >> 8) & 0xFF
        self.minor = v & 0xFF
        self.insns = []

    @property
    def profile(self):
        mn = "x" if (self.kind == "ps" and self.major == 2 and self.minor == 1) else str(self.minor)
        if self.minor == 0xFF:
            mn = "sw"
        return f"{self.kind}_{self.major}_{mn}"

    def comments(self):
        """Yield (fourcc, payload bytes) for every comment block."""
        for i in self.insns:
            if i.op == 0xFFFE:
                b = struct.pack(f"<{len(i.extra)}I", *i.extra)
                yield b[:4], b[4:]


# ---------------------------------------------------------------------------- parsing

def tokens_of(blob):
    if len(blob) % 4:
        raise ParseError("length not a multiple of 4")
    return list(struct.unpack(f"<{len(blob) // 4}I", blob))


def _src_count_sm1(op, sh):
    """Source-parameter count for SM1.x, which has no length field."""
    if op == 66:  # tex / texld
        return 1 if (sh.kind == "ps" and sh.minor == 4) else 0
    if op == 64:  # texcoord / texcrd
        return 1 if (sh.kind == "ps" and sh.minor == 4) else 0
    n = OPS[op][2]
    return n if isinstance(n, int) else n[0]


def parse(blob_or_tokens):
    toks = tokens_of(blob_or_tokens) if isinstance(blob_or_tokens, (bytes, bytearray)) else list(blob_or_tokens)
    if not toks:
        raise ParseError("empty")
    sh = Shader(toks)
    if sh.kind is None or sh.major not in (1, 2, 3):
        raise ParseError(f"bad version token {toks[0]:#x}")
    sm2 = sh.major >= 2
    i = 1
    n = len(toks)
    while True:
        if i >= n:
            raise ParseError("no end token")
        t = toks[i]
        op = t & 0xFFFF
        ins = Insn(op, t, i)
        if op == 0xFFFF:
            if t != 0x0000FFFF:
                raise ParseError(f"bad end token {t:#x}")
            sh.insns.append(ins)
            i += 1
            break
        if op == 0xFFFE:
            ln = (t >> 16) & 0x7FFF
            if t & 0x80000000 or i + 1 + ln > n:
                raise ParseError(f"bad comment at {i}")
            ins.extra = toks[i + 1:i + 1 + ln]
            sh.insns.append(ins)
            i += 1 + ln
            continue
        if op not in OPS:
            raise ParseError(f"unknown opcode {op:#x} at token {i}")
        if t & 0x80000000:
            raise ParseError(f"bit 31 set in opcode token at {i}")
        length = (t >> 24) & 0xF if sm2 else None
        j = i + 1
        name, has_dst, _ = OPS[op]

        def take():
            nonlocal j
            if j >= n:
                raise ParseError("truncated")
            v = toks[j]
            j += 1
            return v

        def param(is_dst):
            v = take()
            if not v & 0x80000000:
                raise ParseError(f"param token without bit 31 at {j - 1} ({name})")
            p = Param(v)
            # relative addressing carries an extra token in SM2+ (srcs) and for vs_3_0 output dsts
            if p.relative and sm2 and (not is_dst or (sh.kind == "vs" and sh.major == 3)):
                p.rel = take()
            return p

        if op == 31:  # dcl: usage token, dst
            ins.extra = [take()]
            ins.dst = param(True)
        elif op in (81, 48):  # def, defi
            ins.dst = param(True)
            ins.extra = [take() for _ in range(4)]
        elif op == 47:  # defb
            ins.dst = param(True)
            ins.extra = [take()]
        elif op == 0xFFFD:
            pass
        else:
            if has_dst:
                ins.dst = param(True)
            if ins.predicated:
                ins.pred = param(False)
            if sm2:
                end = i + 1 + length
                while j < end:
                    ins.srcs.append(param(False))
            else:
                for _ in range(_src_count_sm1(op, sh)):
                    ins.srcs.append(param(False))
            ns = OPS[op][2]
            allowed = ns if isinstance(ns, tuple) else (ns,)
            if sm2 and len(ins.srcs) not in allowed:
                raise ParseError(f"{name}: {len(ins.srcs)} sources at token {i}")
        if sm2 and j - i - 1 != length:
            raise ParseError(f"{name}: length field {length} but {j - i - 1} tokens at {i}")
        sh.insns.append(ins)
        i = j
    if i != n:
        raise ParseError(f"{n - i} trailing tokens after end")
    return sh


# ---------------------------------------------------------------------------- disassembly

def _regname(sh, p, is_dst):
    rt, num = p.regtype, p.num
    if rt == 3:
        return (f"a{num}" if sh.kind == "vs" else f"t{num}")
    if rt == 4:
        return RASTOUT.get(num, f"oRast{num}")
    if rt == 5:
        return f"oD{num}"
    if rt == 6:
        return (f"o{num}" if (sh.kind == "vs" and sh.major == 3) else f"oT{num}")
    if rt == 17:
        return MISC.get(num, f"misc{num}")
    if rt == 15:
        return "aL" if num == 0 else f"aL{num}"
    if rt == 9:
        return "oDepth" if num == 0 else f"oDepth{num}"
    if rt == 16:
        return f"half{num}"
    return f"{REGTYPES.get(rt, f'reg{rt}_')}{num}"


def _swz(s):
    comps = [SWZ[(s >> (2 * k)) & 3] for k in range(4)]
    txt = "".join(comps)
    if txt == "xyzw":
        return ""
    return "." + txt  # always full four letters: lossless


def _rel(sh, p):
    if not p.relative:
        return ""
    if p.rel is None:
        return "[a0.x]"  # vs_1_1 implicit
    r = Param(p.rel)
    comp = SWZ[(r.swizzle) & 3]
    extra = "" if (r.token & 0x00FCE000) == 0 and ((r.swizzle >> 2) & 0x3F) == 0 else f"{{{r.token:#x}}}"
    return f"[{_regname(sh, r, False)}.{comp}{extra}]"


def _src(sh, p):
    base = _regname(sh, p, False)
    if p.relative:
        if p.regtype == 2 and p.rel is not None:
            base = f"c[{_rel(sh, p)[1:-1]} + {p.num}]"
        else:
            base = base + _rel(sh, p)
    s = base + _swz(p.swizzle)
    m = p.smod
    if m == 0:
        return s
    return f"{SRCMOD.get(m, f'mod{m}')}({s})"


def _dst(sh, p, ins):
    base = _regname(sh, p, True)
    if p.relative:
        base += _rel(sh, p)
    if p.mask != 0xF:
        base += "." + "".join(SWZ[k] for k in range(4) if p.mask >> k & 1)
    return base


def _dmods(p):
    s = ""
    if p.dmod & 1:
        s += "_sat"
    if p.dmod & 2:
        s += "_pp"
    if p.dmod & 4:
        s += "_centroid"
    if p.dmod & 8:
        s += "_dmod8"
    if p.shift:
        s += f"_shift{p.shift}"
    return s


def _f(x):
    return struct.unpack("<f", struct.pack("<I", x))[0]


def disassemble(sh, comments=True, offsets=False):
    """Canonical text. With comments=True the output re-assembles to the exact token stream."""
    out = [sh.profile]
    for ins in sh.insns:
        pre = f"/*{ins.offset:04x}*/ " if offsets else ""
        if ins.op == 0xFFFF:
            break
        if ins.op == 0xFFFE:
            if comments:
                b = struct.pack(f"<{len(ins.extra)}I", *ins.extra)
                out.append(pre + "comment " + b.hex())
            else:
                fourcc = struct.pack("<I", ins.extra[0]) if ins.extra else b""
                out.append(pre + f"// comment {len(ins.extra)} tokens {fourcc!r}")
            continue
        name = ins.name
        ctl = ins.controls
        sfx = ""
        if ins.op in (41, 45, 94):  # ifc, breakc, setp
            sfx = "_" + CMP.get(ctl & 7, f"cmp{ctl}")
            ctl_rest = ctl & ~7
        elif ins.op == 66 and sh.major >= 2:  # texld variants
            sfx = {0: "", 1: "p", 2: "b"}.get(ctl & 3, f"_ctl{ctl & 3}")
            ctl_rest = ctl & ~3
        else:
            ctl_rest = ctl
        if ins.op == 66 and sh.major < 2:
            name = "tex" if sh.minor < 4 else "texld"
        if ins.op == 64 and sh.kind == "ps" and sh.minor == 4 and sh.major == 1:
            name = "texcrd"
        if ins.op == 31:
            u = ins.extra[0]
            ty = ins.dst.regtype
            if ty == 17:  # vPos / vFace
                name = "dcl"
                rest = u & ~0x80000000
            elif ty == 10:  # sampler
                tt = (u >> 27) & 0xF
                name = f"dcl_{TEXTYPE.get(tt, f'tex{tt}')}"
                rest = u & ~(0xF << 27) & ~0x80000000
            elif sh.major >= 3 or sh.kind == "vs" or (u & 0x1F) or ((u >> 16) & 0xF):
                ui, idx = u & 0x1F, (u >> 16) & 0xF
                name = f"dcl_{USAGE.get(ui, f'usage{ui}')}{idx if idx else ''}"
                rest = u & ~0x1F & ~(0xF << 16) & ~0x80000000
            else:
                name = "dcl"
                rest = u & ~0x80000000
            if not (u & 0x80000000):
                rest = u | 1 << 32  # force the explicit {usage=...} form so bit 31 round-trips
            line = name + _dmods(ins.dst) + " " + _dst(sh, ins.dst, ins)
            if rest:
                line += f" {{usage={u:#x}}}"
            out.append(pre + line)
            continue
        tail = ""
        if ins.predicated:
            name = f"({'!' if ins.pred.smod == 13 else ''}{_regname(sh, ins.pred, False)}{_swz(ins.pred.swizzle)}) " + name
            if ins.pred.smod not in (0, 13) or ins.pred.relative:
                tail += f" {{pred={ins.pred.token:#x}}}"
        if ins.coissue:
            name = "+" + name
        if ctl_rest:
            tail += f" {{ctl={ctl_rest:#x}}}"
        if (ins.token >> 28) & 0x8 or (ins.token >> 29) & 1:
            tail += f" {{optok={ins.token:#x}}}"
        args = []
        name += sfx
        sfx = ""
        if ins.dst is not None:
            name += _dmods(ins.dst)
            args.append(_dst(sh, ins.dst, ins))
        if ins.op in (81,):
            args += [repr(_f(x)) if struct.pack("<f", _f(x)) == struct.pack("<I", x) and _f(x) == _f(x)
                     else f"0x{x:08x}" for x in ins.extra]
        elif ins.op == 48:
            args += [str(struct.unpack("<i", struct.pack("<I", x))[0]) for x in ins.extra]
        elif ins.op == 47:
            args.append("true" if ins.extra[0] == 1 else ("false" if ins.extra[0] == 0 else f"0x{ins.extra[0]:x}"))
        args += [_src(sh, p) for p in ins.srcs]
        out.append(pre + name + sfx + (" " + ", ".join(args) if args else "") + tail)
    return "\n".join(out) + "\n"


# ---------------------------------------------------------------------------- assembler

import re

_REG_RE = re.compile(r"^(oPos|oFog|oPts|oDepth|oD|oT|oC|vPos|vFace|aL|half|c2k|c4k|c6k|misc|reg\d+_|[rvcatsibpol])(\d*)$")


def _regtype(sh, prefix):
    m = {
        "r": 0, "v": 1, "c": 2, "a": 3, "t": 3, "oPos": 4, "oFog": 4, "oPts": 4, "oD": 5, "oT": 6, "o": 6,
        "i": 7, "oC": 8, "oDepth": 9, "s": 10, "c2k": 11, "c4k": 12, "c6k": 13, "b": 14, "aL": 15,
        "half": 16, "vPos": 17, "vFace": 17, "misc": 17, "l": 18, "p": 19,
    }
    if prefix.startswith("reg"):
        return int(prefix[3:-1])
    return m[prefix]


def _enc_reg(rt, num):
    return ((rt & 7) << 28) | ((rt & 0x18) << 8) | (num & 0x7FF) | 0x80000000


def _parse_reg(sh, txt):
    """'r3' / 'c[a0.x + 4]' / 'v[aL.x]' etc. -> (token without swizzle/mask, rel token or None)"""
    rel = None
    relative = False
    m = re.match(r"^c\[(.+?) \+ (\d+)\]$", txt)
    if m:
        relative = True
        rel = _parse_rel(sh, m.group(1))
        rt, num = 2, int(m.group(2))
    else:
        m = re.match(r"^([^\[]+)\[(.+)\]$", txt)
        if m:
            relative = True
            rel = _parse_rel(sh, m.group(2))
            txt = m.group(1)
        mm = _REG_RE.match(txt)
        if not mm:
            raise ParseError(f"bad register {txt!r}")
        pfx, n = mm.group(1), mm.group(2)
        rt = _regtype(sh, pfx)
        if pfx in ("oPos", "vPos"):
            num = 0
        elif pfx in ("oFog", "vFace"):
            num = 1
        elif pfx == "oPts":
            num = 2
        else:
            num = int(n) if n else 0
    tok = _enc_reg(rt, num) | (0x2000 if relative else 0)
    return tok, rel


def _parse_rel(sh, txt):
    extra = None
    m = re.match(r"^(.*)\{(0x[0-9a-f]+)\}$", txt)
    if m:
        txt, extra = m.group(1), int(m.group(2), 16)
    reg, comp = txt.split(".")
    if extra is not None:
        return extra
    if sh.major < 2:
        return None  # vs_1_1: implicit a0.x
    t, _ = _parse_reg(sh, reg)
    c = SWZ.index(comp)
    return t | (c * 0x55) << 16


def _parse_swz(s):
    if not s:
        return 0xE4
    s = (s + s[-1] * 4)[:4]
    v = 0
    for k, ch in enumerate(s):
        v |= SWZ.index(ch) << (2 * k)
    return v


def _split_src(sh, txt):
    mod = 0
    m = re.match(r"^([a-z]+)\((.*)\)$", txt)
    if m and m.group(1) in SRCMOD_R:
        mod = SRCMOD_R[m.group(1)]
        txt = m.group(2)
    elif m and m.group(1).startswith("mod"):
        mod = int(m.group(1)[3:])
        txt = m.group(2)
    # swizzle: trailing .xxxx outside brackets
    sw = ""
    m = re.match(r"^(.*?)(?:\.([xyzw]{1,4}))?$", txt)
    if m and m.group(2) and not m.group(1).endswith("]") or (m and m.group(2) and m.group(1).endswith("]")):
        txt, sw = m.group(1), m.group(2)
    tok, rel = _parse_reg(sh, txt)
    return tok | (_parse_swz(sw) << 16) | (mod << 24), rel


def _split_dst(sh, txt, dmod, shift):
    m = re.match(r"^(.*?)(?:\.([xyzw]{1,4}))?$", txt)
    base, mask = m.group(1), m.group(2)
    mk = 0xF if not mask else sum(1 << SWZ.index(ch) for ch in mask)
    tok, rel = _parse_reg(sh, base)
    return tok | (mk << 16) | (dmod << 20) | (shift << 24), rel


def _split_args(s):
    out, depth, cur = [], 0, ""
    for ch in s:
        if ch in "([":
            depth += 1
        elif ch in ")]":
            depth -= 1
        if ch == "," and depth == 0:
            out.append(cur.strip())
            cur = ""
        else:
            cur += ch
    if cur.strip():
        out.append(cur.strip())
    return out


def assemble(text):
    lines = [ln.strip() for ln in text.splitlines()]
    lines = [ln for ln in lines if ln and not ln.startswith("//")]
    prof = lines[0]
    kind, maj, mn = prof.split("_")
    major = int(maj)
    minor = {"x": 1, "sw": 0xFF}.get(mn, None)
    minor = int(mn) if minor is None else minor
    ver = (0xFFFE0000 if kind == "vs" else 0xFFFF0000) | (major << 8) | minor
    sh = Shader([ver])
    toks = [ver]
    for ln in lines[1:]:
        if ln.startswith("comment "):
            b = bytes.fromhex(ln[8:])
            n = len(b) // 4
            toks.append(0xFFFE | (n << 16))
            toks += list(struct.unpack(f"<{n}I", b))
            continue
        extras = dict(re.findall(r"\{(\w+)=(0x[0-9a-f]+)\}", ln))
        ln = re.sub(r"\s*\{(usage|ctl|optok|pred)=0x[0-9a-f]+\}", "", ln)
        pred_tok = None
        coissue = False
        if ln.startswith("+"):
            coissue = True
            ln = ln[1:]
        if ln.startswith("("):
            pt, ln = ln[1:].split(") ", 1)
            neg = pt.startswith("!")
            pt = pt.lstrip("!")
            t, _ = _split_src(sh, pt)
            pred_tok = t | ((13 << 24) if neg else 0)
            if "pred" in extras:
                pred_tok = int(extras["pred"], 16)
        parts = ln.split(" ", 1)
        mnem = parts[0]
        args = _split_args(parts[1]) if len(parts) > 1 else []
        # split modifiers off the mnemonic
        segs = mnem.split("_")
        base = segs[0]
        dmod = shift = 0
        ctl = 0
        rest = []
        for sgm in segs[1:]:
            if sgm == "sat":
                dmod |= 1
            elif sgm == "pp":
                dmod |= 2
            elif sgm == "centroid":
                dmod |= 4
            elif sgm == "dmod8":
                dmod |= 8
            elif sgm.startswith("shift"):
                shift = int(sgm[5:])
            else:
                rest.append(sgm)
        if base == "dcl":
            op = 31
            if rest:
                r0 = rest[0]
                if r0 in TEXTYPE_R:
                    u = (TEXTYPE_R[r0] << 27)
                elif r0.startswith("tex") and r0[3:].isdigit():
                    u = int(r0[3:]) << 27
                else:
                    mm = re.match(r"^([a-z]+?)(\d*)$", r0)
                    nm, idx = mm.group(1), mm.group(2)
                    if nm.startswith("usage"):
                        nm, idx = "usage", ""
                        u = int(re.match(r"usage(\d+)", r0).group(1))
                    else:
                        u = USAGE_R[nm]
                    u |= (int(idx) if idx else 0) << 16
            else:
                u = 0
            u |= 0x80000000
            if "usage" in extras:
                u = int(extras["usage"], 16)
            d, drel = _split_dst(sh, args[0], dmod, shift)
            body = [u, d] + ([drel] if drel is not None else [])
        else:
            if base in ("tex", "texld", "texldp", "texldb") and major < 2:
                op = 66
            elif base == "texcrd":
                op = 64
            elif base in ("texldp", "texldb"):
                op = 66
                ctl = 1 if base == "texldp" else 2
            elif base in ("ifc", "breakc", "setp") and rest:
                op = NAME2OP[base]
                ctl = CMP_R[rest.pop(0)]
            else:
                op = NAME2OP[base]
            if rest:
                raise ParseError(f"unknown suffix in {mnem}")
            if "ctl" in extras:
                ctl |= int(extras["ctl"], 16)
            body = []
            has_dst = OPS[op][1]
            ai = 0
            if has_dst:
                d, drel = _split_dst(sh, args[0], dmod, shift)
                body += [d] + ([drel] if drel is not None else [])
                ai = 1
            if pred_tok is not None:
                body.append(pred_tok)
            if op == 81:
                for a in args[1:5]:
                    body.append(int(a, 16) if a.startswith("0x") else struct.unpack("<I", struct.pack("<f", float(a)))[0])
                ai = 5
            elif op == 48:
                body += [struct.unpack("<I", struct.pack("<i", int(a)))[0] for a in args[1:5]]
                ai = 5
            elif op == 47:
                a = args[1]
                body.append(1 if a == "true" else 0 if a == "false" else int(a, 16))
                ai = 2
            for a in args[ai:]:
                s, srel = _split_src(sh, a)
                body += [s] + ([srel] if srel is not None else [])
        ot = op | (ctl << 16)
        if major >= 2:
            ot |= len(body) << 24
        if pred_tok is not None:
            ot |= 0x10000000
        if coissue:
            ot |= 0x40000000
        if "optok" in extras:
            ot = int(extras["optok"], 16)
        toks.append(ot)
        toks += body
    toks.append(0x0000FFFF)
    return toks


# ---------------------------------------------------------------------------- CTAB

RSET = {0: "bool", 1: "int4", 2: "float4", 3: "sampler"}
PCLASS = {0: "scalar", 1: "vector", 2: "matrix_rows", 3: "matrix_columns", 4: "object", 5: "struct"}
PTYPE = {0: "void", 1: "bool", 2: "int", 3: "float", 4: "string", 5: "texture", 6: "texture1d", 7: "texture2d",
         8: "texture3d", 9: "texturecube", 10: "sampler", 11: "sampler1d", 12: "sampler2d", 13: "sampler3d",
         14: "samplercube"}


def _cstr(b, o):
    if o <= 0 or o >= len(b):
        return None
    e = b.find(b"\0", o)
    return b[o:e].decode("latin-1")


def ctab(sh):
    """Parse the CTAB comment, or return None. Returns dict(creator, target, constants=[...])."""
    for fourcc, p in sh.comments():
        if fourcc != b"CTAB":
            continue
        size, creator, ver, n, cinfo, flags, target = struct.unpack_from("<7I", p, 0)
        consts = []
        for k in range(n):
            nm, rs, ri, rc, _res, ti, dv = struct.unpack_from("<IHHHHII", p, cinfo + 20 * k)
            cl, ty, rows, cols, elems, members, moff = struct.unpack_from("<HHHHHHI", p, ti) if ti else (0,) * 7
            consts.append(dict(name=_cstr(p, nm), set=RSET.get(rs, rs), index=ri, count=rc,
                               cls=PCLASS.get(cl, cl), type=PTYPE.get(ty, ty), rows=rows, cols=cols,
                               elements=elems, has_default=bool(dv)))
        return dict(creator=_cstr(p, creator), target=_cstr(p, target), flags=flags, constants=consts)
    return None


# ---------------------------------------------------------------------------- CLI

def _main(argv):
    if len(argv) < 3:
        print(__doc__)
        return 2
    cmd = argv[1]
    if cmd == "dis":
        sh = parse(open(argv[2], "rb").read())
        sys.stdout.write(disassemble(sh, comments="--no-comments" not in argv, offsets="--offsets" in argv))
        return 0
    if cmd == "roundtrip":
        bad = 0
        for f in argv[2:]:
            sh = parse(open(f, "rb").read())
            if assemble(disassemble(sh)) != sh.tokens:
                bad += 1
                print("MISMATCH", f)
        print(f"{len(argv) - 2 - bad}/{len(argv) - 2} round-trip")
        return 1 if bad else 0
    print(__doc__)
    return 2


if __name__ == "__main__":
    sys.exit(_main(sys.argv))
