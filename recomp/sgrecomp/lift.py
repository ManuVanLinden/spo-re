"""Instruction semantics: one decoded x86-32 instruction -> C statements against spore_guest.h.

Control transfers (jcc, jmp, call, ret, switch dispatch) are lowered by emit.py; this module
lowers everything else and provides the operand and condition helpers emit.py uses.

Conventions of the generated C (runtime-abi.md):
  * guest registers are ctx->eax ... ctx->edi; partial registers through SG_R8L/SG_SET_R16 ...
  * guest memory only through sg_ld*/sg_st* (and the atomics for lock/xchg)
  * arithmetic flags live in the function-local lazy record `lf` (sg_lazyflags); consumers use
    sgr_cond(lf, cc). They are never written to ctx before calls or returns (D4)
  * x87 operates on the physical stack in ctx->fpu through sg_recomp.h helpers (values are host doubles)
Every instruction becomes one C block "{ ... }" so temporaries never collide.
"""
from capstone import x86_const as X

N32 = ["eax", "ecx", "edx", "ebx", "esp", "ebp", "esi", "edi"]
R32 = {getattr(X, "X86_REG_" + n.upper()): i for i, n in enumerate(N32)}
R16 = {getattr(X, "X86_REG_" + n.upper()): i for i, n in enumerate(["ax", "cx", "dx", "bx", "sp", "bp", "si", "di"])}
R8L = {getattr(X, "X86_REG_" + n.upper()): i for i, n in enumerate(["al", "cl", "dl", "bl"])}
R8H = {getattr(X, "X86_REG_" + n.upper()): i for i, n in enumerate(["ah", "ch", "dh", "bh"])}
XMM = {getattr(X, "X86_REG_XMM%d" % i): i for i in range(8)}
MM = {getattr(X, "X86_REG_MM%d" % i): i for i in range(8)}
SREG_VAL = {X.X86_REG_CS: 0x1B, X.X86_REG_DS: 0x23, X.X86_REG_ES: 0x23, X.X86_REG_SS: 0x23,
            X.X86_REG_FS: 0x3B, X.X86_REG_GS: 0x00}  # Windows x86 selector values

CC = {"o": 0, "no": 1, "b": 2, "ae": 3, "e": 4, "ne": 5, "be": 6, "a": 7, "s": 8, "ns": 9, "p": 10, "np": 11,
      "l": 12, "ge": 13, "le": 14, "g": 15}
SC = {1: 0, 2: 1, 4: 2}
MASK = {1: 0xFF, 2: 0xFFFF, 4: 0xFFFFFFFF}
BITS = {1: 8, 2: 16, 4: 32}
X87_PREFIX = {0x26, 0x2E, 0x36, 0x3E, 0x64, 0x65, 0x66, 0x67, 0xF0, 0xF2, 0xF3, 0x9B}


class Unhandled(Exception):
    pass


def h(v):
    return "0x%xu" % (v & 0xFFFFFFFF)


class Lifter:
    def __init__(self):
        self.unhandled = []  # (va, text)

    # ---------------------------------------------------------------- operands --
    def ea(self, insn, op):
        """Effective address of a memory operand, as a uint32_t C expression."""
        m = op.mem
        parts = []
        if m.base:
            if m.base not in R32:
                raise Unhandled("16-bit or odd base register")
            parts.append("ctx->" + N32[R32[m.base]])
        if m.index:
            if m.index not in R32:
                raise Unhandled("odd index register")
            parts.append("ctx->" + N32[R32[m.index]] + (" * %du" % m.scale if m.scale > 1 else ""))
        d = m.disp & 0xFFFFFFFF
        if d or not parts:
            parts.append(h(d))
        e = " + ".join(parts)
        e = "(uint32_t)(%s)" % e if len(parts) > 1 else ("(%s)" % e if parts[0].startswith("ctx") else e)
        if m.segment == X.X86_REG_FS:
            return "SG_FS(ctx, %s)" % e
        if m.segment not in (0, X.X86_REG_DS, X.X86_REG_SS, X.X86_REG_ES):
            raise Unhandled("segment override")
        return e

    def rd(self, insn, op, a=None, size=None):
        """Value of an operand, zero-extended to uint32_t. `a` = precomputed address variable."""
        size = size or op.size
        if op.type == X.X86_OP_IMM:
            return h(op.imm & MASK[size])
        if op.type == X.X86_OP_REG:
            r = op.reg
            if r in R32:
                return "ctx->" + N32[R32[r]]
            if r in R16:
                return "(uint32_t)SG_R16(ctx, %d)" % R16[r]
            if r in R8L:
                return "(uint32_t)SG_R8L(ctx, %d)" % R8L[r]
            if r in R8H:
                return "(uint32_t)SG_R8H(ctx, %d)" % R8H[r]
            if r in SREG_VAL:
                return h(SREG_VAL[r])
            raise Unhandled("register read")
        a = a or self.ea(insn, op)
        return {1: "(uint32_t)sg_ld8(mem, %s)", 2: "(uint32_t)sg_ld16(mem, %s)", 4: "sg_ld32(mem, %s)"}[size] % a

    def wr(self, insn, op, v, a=None):
        if op.type == X.X86_OP_REG:
            r = op.reg
            if r in R32:
                return "ctx->%s = (uint32_t)(%s);" % (N32[R32[r]], v)
            if r in R16:
                return "SG_SET_R16(ctx, %d, %s);" % (R16[r], v)
            if r in R8L:
                return "SG_SET_R8L(ctx, %d, %s);" % (R8L[r], v)
            if r in R8H:
                return "SG_SET_R8H(ctx, %d, %s);" % (R8H[r], v)
            raise Unhandled("register write")
        if op.type != X.X86_OP_MEM:
            raise Unhandled("write to immediate")
        a = a or self.ea(insn, op)
        return {1: "sg_st8(mem, %s, (uint8_t)(%s));", 2: "sg_st16(mem, %s, (uint16_t)(%s));",
                4: "sg_st32(mem, %s, (uint32_t)(%s));"}[op.size] % (a, v)

    def addr_decl(self, insn, op, out):
        """For read-modify-write memory operands: compute the address once."""
        if op.type == X.X86_OP_MEM:
            out.append("uint32_t a = %s;" % self.ea(insn, op))
            return "a"
        return None

    @staticmethod
    def cond(cc):
        return "sgr_cond(lf, %du)" % CC[cc]

    # ---------------------------------------------------------------- entry --
    def lower(self, insn):
        """Return a list of C lines for a non-control-transfer instruction."""
        b = insn.bytes
        i = 0
        while i < len(b) and b[i] in X87_PREFIX:
            i += 1
        if i < len(b) and 0xD8 <= b[i] <= 0xDF:
            return self.x87(insn, b[i], b[i + 1])
        mn = insn.mnemonic
        parts = mn.split()
        base = parts[-1]
        pfx = parts[0] if len(parts) > 1 else ""
        fn = getattr(self, "i_" + base, None)
        if fn is None:
            if base[:3] == "set" and base[3:] in CC:
                return self.setcc(insn, base[3:])
            if base[:4] == "cmov" and base[4:] in CC:
                return self.cmov(insn, base[4:])
            if base[:3] == "cmp" and base.endswith("ps") and base[3:-2] in CMPPS:
                return self.cmpps(insn, CMPPS[base[3:-2]])
            if base in LANE_OPS:
                return self.lane_op(insn, base)
            raise Unhandled(mn)
        return fn(insn, pfx) if base in ("movsb", "movsw", "movsd", "stosb", "stosw", "stosd", "cmpsb",
                                          "cmpsw", "cmpsd", "xadd", "cmpxchg", "cmpxchg8b") else fn(insn)

    # ---------------------------------------------------------------- integer ALU --
    def _alu(self, insn, kind, op, write=True, carry=False):
        d, s = insn.operands
        sz = d.size
        out = []
        a = self.addr_decl(insn, d, out)
        out.append("uint32_t d = %s, s = %s;" % (self.rd(insn, d, a), self.rd(insn, s, size=sz)))
        if carry:
            out.append("uint32_t c = sgr_cf(lf);")
            out.append("uint32_t r = (d %s s %s c) & %s;" % (op, op, h(MASK[sz])))
            fl = "SGR_FLC(%s, %d, r, d, s, c)" % (kind, SC[sz])
        else:
            out.append("uint32_t r = (d %s s) & %s;" % (op, h(MASK[sz])))
            fl = "SGR_FL(%s, %d, r, d, s)" % (kind, SC[sz])
        if write:
            out.append(self.wr(insn, d, "r", a))
        out.append("lf = %s;" % fl)
        return out

    def i_add(self, insn): return self._alu(insn, "SG_FOP_ADD", "+")
    def i_adc(self, insn): return self._alu(insn, "SG_FOP_ADD", "+", carry=True)
    def i_sub(self, insn): return self._alu(insn, "SG_FOP_SUB", "-")
    def i_sbb(self, insn): return self._alu(insn, "SG_FOP_SUB", "-", carry=True)
    def i_cmp(self, insn): return self._alu(insn, "SG_FOP_SUB", "-", write=False)
    def i_and(self, insn): return self._alu(insn, "SG_FOP_LOGIC", "&")
    def i_or(self, insn): return self._alu(insn, "SG_FOP_LOGIC", "|")
    def i_xor(self, insn): return self._alu(insn, "SG_FOP_LOGIC", "^")
    def i_test(self, insn): return self._alu(insn, "SG_FOP_LOGIC", "&", write=False)

    def _incdec(self, insn, kind, op):
        d = insn.operands[0]
        sz = d.size
        out = []
        a = self.addr_decl(insn, d, out)
        out.append("uint32_t d = %s;" % self.rd(insn, d, a))
        out.append("uint32_t r = (d %s 1u) & %s;" % (op, h(MASK[sz])))
        out.append(self.wr(insn, d, "r", a))
        out.append("lf = SGR_FLC(%s, %d, r, d, 1u, sgr_cf(lf));" % (kind, SC[sz]))
        return out

    def i_inc(self, insn): return self._incdec(insn, "SG_FOP_INC", "+")
    def i_dec(self, insn): return self._incdec(insn, "SG_FOP_DEC", "-")

    def i_neg(self, insn):
        d = insn.operands[0]
        sz = d.size
        out = []
        a = self.addr_decl(insn, d, out)
        out.append("uint32_t s = %s;" % self.rd(insn, d, a))
        out.append("uint32_t r = (0u - s) & %s;" % h(MASK[sz]))
        out.append(self.wr(insn, d, "r", a))
        out.append("lf = SGR_FL(SG_FOP_SUB, %d, r, 0u, s);" % SC[sz])
        return out

    def i_not(self, insn):
        d = insn.operands[0]
        out = []
        a = self.addr_decl(insn, d, out)
        out.append(self.wr(insn, d, "~%s" % self.rd(insn, d, a), a))
        return out

    # ---------------------------------------------------------------- shifts --
    def _count(self, insn, cop, out):
        """Masked shift count. Returns (C expr, constant or None)."""
        if cop.type == X.X86_OP_IMM:
            c = cop.imm & 31
            return str(c) + "u", c
        out.append("uint32_t c = %s & 31u;" % self.rd(insn, cop))
        return "c", None

    def _shift(self, insn, kind):
        ops = insn.operands
        d = ops[0]
        cop = ops[1] if len(ops) > 1 else None
        sz = d.size
        bits = BITS[sz]
        out = []
        a = self.addr_decl(insn, d, out)
        if cop is None:
            cexpr, cval = "1u", 1
        else:
            cexpr, cval = self._count(insn, cop, out)
        if cval == 0:
            return ["/* count 0: no effect */"]
        body = ["uint32_t d = %s;" % self.rd(insn, d, a)]
        m = h(MASK[sz])
        if kind == "shl":
            body.append("uint32_t r = (d << %s) & %s;" % (cexpr, m))
            fl = "SGR_FL(SG_FOP_SHL, %d, r, d, %s)" % (SC[sz], cexpr)
        elif kind == "shr":
            body.append("uint32_t r = (d & %s) >> %s;" % (m, cexpr))
            fl = "SGR_FL(SG_FOP_SHR, %d, r, d, %s)" % (SC[sz], cexpr)
        elif kind == "sar":
            sx = {1: "(int32_t)(int8_t)d", 2: "(int32_t)(int16_t)d", 4: "(int32_t)d"}[sz]
            body.append("uint32_t r = (uint32_t)(%s >> %s) & %s;" % (sx, cexpr, m))
            fl = "SGR_FL(SG_FOP_SAR, %d, r, d, %s)" % (SC[sz], cexpr)
        elif kind in ("rol", "ror"):
            body.append("uint32_t r = sgr_%s(d, %s, %d);" % (kind, cexpr, bits))
            if kind == "rol":
                body.append("uint32_t cf = r & 1u, of = ((r >> %d) ^ r) & 1u;" % (bits - 1))
            else:
                body.append("uint32_t cf = (r >> %d) & 1u, of = ((r >> %d) ^ (r >> %d)) & 1u;" % (bits - 1, bits - 1, bits - 2))
            fl = "sgr_merge(lf, SG_CF | SG_OF, (cf ? SG_CF : 0u) | (of ? SG_OF : 0u))"
        elif kind in ("rcl", "rcr"):
            body.append("uint32_t cf = sgr_cf(lf);")
            body.append("uint32_t r = sgr_%s(d, %s, %d, &cf);" % (kind, cexpr, bits))
            if kind == "rcl":
                body.append("uint32_t of = ((r >> %d) ^ cf) & 1u;" % (bits - 1))
            else:
                body.append("uint32_t of = ((r >> %d) ^ (r >> %d)) & 1u;" % (bits - 1, bits - 2))
            fl = "sgr_merge(lf, SG_CF | SG_OF, (cf ? SG_CF : 0u) | (of ? SG_OF : 0u))"
        body.append(self.wr(insn, d, "r", a))
        body.append("lf = %s;" % fl)
        if cval is None:
            out.append("if (c) {")
            out.extend("    " + x for x in body)
            out.append("}")
        else:
            out.extend(body)
        return out

    def i_shl(self, insn): return self._shift(insn, "shl")
    def i_sal(self, insn): return self._shift(insn, "shl")
    def i_shr(self, insn): return self._shift(insn, "shr")
    def i_sar(self, insn): return self._shift(insn, "sar")
    def i_rol(self, insn): return self._shift(insn, "rol")
    def i_ror(self, insn): return self._shift(insn, "ror")
    def i_rcl(self, insn): return self._shift(insn, "rcl")
    def i_rcr(self, insn): return self._shift(insn, "rcr")

    def _shxd(self, insn, left):
        d, s, cop = insn.operands
        if d.size != 4:
            raise Unhandled("16-bit shld/shrd")
        out = []
        a = self.addr_decl(insn, d, out)
        cexpr, cval = self._count(insn, cop, out)
        if cval == 0:
            return ["/* count 0: no effect */"]
        body = ["uint32_t d = %s, s = %s;" % (self.rd(insn, d, a), self.rd(insn, s))]
        if left:
            body.append("uint32_t r = (d << %s) | (uint32_t)(((uint64_t)s) >> (32u - %s));" % (cexpr, cexpr))
            body.append("uint32_t cf = (d >> (32u - %s)) & 1u;" % cexpr)
        else:
            body.append("uint32_t r = (uint32_t)((((uint64_t)s << 32) | d) >> %s);" % cexpr)
            body.append("uint32_t cf = (d >> (%s - 1u)) & 1u;" % cexpr)
        body.append("uint32_t of = ((r ^ d) >> 31) & 1u;")
        body.append(self.wr(insn, d, "r", a))
        body.append("lf = SGR_FLE(sgr_szp(r, 32) | (cf ? SG_CF : 0u) | (of ? SG_OF : 0u) | (sgr_eflags(lf) & SG_AF));")
        if cval is None:
            out.append("if (c) {")
            out.extend("    " + x for x in body)
            out.append("}")
        else:
            out.extend(body)
        return out

    def i_shld(self, insn): return self._shxd(insn, True)
    def i_shrd(self, insn): return self._shxd(insn, False)

    # ---------------------------------------------------------------- mul / div --
    def i_mul(self, insn):
        s = insn.operands[0]
        sz = s.size
        v = self.rd(insn, s)
        if sz == 4:
            return ["uint64_t p = (uint64_t)ctx->eax * %s;" % v,
                    "ctx->eax = (uint32_t)p; ctx->edx = (uint32_t)(p >> 32);",
                    "lf = SGR_FL(SG_FOP_MUL, 2, ctx->eax, 0u, ctx->edx != 0u);"]
        if sz == 2:
            return ["uint32_t p = (uint32_t)SG_R16(ctx, 0) * %s;" % v,
                    "SG_SET_R16(ctx, 0, p); SG_SET_R16(ctx, 2, p >> 16);",
                    "lf = SGR_FL(SG_FOP_MUL, 1, p & 0xffffu, 0u, (p >> 16) != 0u);"]
        return ["uint32_t p = (uint32_t)SG_R8L(ctx, 0) * %s;" % v,
                "SG_SET_R16(ctx, 0, p);",
                "lf = SGR_FL(SG_FOP_MUL, 0, p & 0xffu, 0u, (p >> 8) != 0u);"]

    def i_imul(self, insn):
        ops = insn.operands
        if len(ops) == 1:
            s = ops[0]
            sz = s.size
            v = self.rd(insn, s)
            if sz == 4:
                return ["int64_t p = (int64_t)(int32_t)ctx->eax * (int32_t)%s;" % v,
                        "ctx->eax = (uint32_t)p; ctx->edx = (uint32_t)((uint64_t)p >> 32);",
                        "lf = SGR_FL(SG_FOP_MUL, 2, ctx->eax, 0u, p != (int64_t)(int32_t)p);"]
            if sz == 2:
                return ["int32_t p = (int32_t)(int16_t)SG_R16(ctx, 0) * (int16_t)%s;" % v,
                        "SG_SET_R16(ctx, 0, (uint32_t)p); SG_SET_R16(ctx, 2, (uint32_t)p >> 16);",
                        "lf = SGR_FL(SG_FOP_MUL, 1, (uint32_t)p & 0xffffu, 0u, p != (int32_t)(int16_t)p);"]
            return ["int32_t p = (int32_t)(int8_t)SG_R8L(ctx, 0) * (int8_t)%s;" % v,
                    "SG_SET_R16(ctx, 0, (uint32_t)p);",
                    "lf = SGR_FL(SG_FOP_MUL, 0, (uint32_t)p & 0xffu, 0u, p != (int32_t)(int8_t)p);"]
        d = ops[0]
        if len(ops) == 2:
            x, y = ops[0], ops[1]
        else:
            x, y = ops[1], ops[2]
        sz = d.size
        if sz == 4:
            out = ["int64_t p = (int64_t)(int32_t)%s * (int32_t)%s;" % (self.rd(insn, x), self.rd(insn, y, size=4)),
                   self.wr(insn, d, "(uint32_t)p"),
                   "lf = SGR_FL(SG_FOP_MUL, 2, (uint32_t)p, 0u, p != (int64_t)(int32_t)p);"]
        else:
            out = ["int32_t p = (int32_t)(int16_t)%s * (int16_t)%s;" % (self.rd(insn, x), self.rd(insn, y, size=2)),
                   self.wr(insn, d, "(uint32_t)p"),
                   "lf = SGR_FL(SG_FOP_MUL, 1, (uint32_t)p & 0xffffu, 0u, p != (int32_t)(int16_t)p);"]
        return out

    def _div(self, insn, signed):
        s = insn.operands[0]
        sz = s.size
        return ["sgr_%sdiv%d(ctx, %s, %s);" % ("i" if signed else "", BITS[sz], self.rd(insn, s), h(insn.address))]

    def i_div(self, insn): return self._div(insn, False)
    def i_idiv(self, insn): return self._div(insn, True)

    # ---------------------------------------------------------------- moves --
    def i_mov(self, insn):
        d, s = insn.operands
        if d.type == X.X86_OP_REG and d.reg in SREG_VAL:
            raise Unhandled("mov to segment register")
        return [self.wr(insn, d, self.rd(insn, s, size=d.size))]

    def i_movzx(self, insn):
        d, s = insn.operands
        return [self.wr(insn, d, self.rd(insn, s))]

    def i_movsx(self, insn):
        d, s = insn.operands
        t = {1: "int8_t", 2: "int16_t"}[s.size]
        return [self.wr(insn, d, "(uint32_t)(int32_t)(%s)%s" % (t, self.rd(insn, s)))]

    def i_lea(self, insn):
        d, s = insn.operands
        m = s.mem
        if m.segment:
            raise Unhandled("lea with segment")
        return [self.wr(insn, d, self.ea(insn, s))]

    def i_cbw(self, insn): return ["SG_SET_R16(ctx, 0, (uint32_t)(int32_t)(int8_t)SG_R8L(ctx, 0));"]
    def i_cwde(self, insn): return ["ctx->eax = (uint32_t)(int32_t)(int16_t)SG_R16(ctx, 0);"]
    def i_cdq(self, insn): return ["ctx->edx = (uint32_t)((int32_t)ctx->eax >> 31);"]
    def i_cwd(self, insn): return ["SG_SET_R16(ctx, 2, (uint32_t)((int32_t)(int16_t)SG_R16(ctx, 0) >> 16));"]

    def i_bswap(self, insn):
        d = insn.operands[0]
        return [self.wr(insn, d, "__builtin_bswap32(%s)" % self.rd(insn, d))]

    def i_xchg(self, insn):
        d, s = insn.operands
        if d.type == X.X86_OP_MEM or s.type == X.X86_OP_MEM:
            m, r = (d, s) if d.type == X.X86_OP_MEM else (s, d)
            if m.size != 4:
                raise Unhandled("xchg m8/m16")
            return ["uint32_t a = %s;" % self.ea(insn, m),
                    "uint32_t o = sg_xchg32(ctx, mem, a, %s);" % self.rd(insn, r),
                    self.wr(insn, r, "o")]
        return ["uint32_t x = %s, y = %s;" % (self.rd(insn, d), self.rd(insn, s)),
                self.wr(insn, d, "y"), self.wr(insn, s, "x")]

    def i_xadd(self, insn, pfx):
        d, s = insn.operands
        if d.type != X.X86_OP_MEM or d.size != 4 or pfx != "lock":
            raise Unhandled("xadd form")
        return ["uint32_t a = %s, s = %s;" % (self.ea(insn, d), self.rd(insn, s)),
                "uint32_t o = sg_lock_xadd32(ctx, mem, a, s);",
                self.wr(insn, s, "o"),
                "lf = SGR_FL(SG_FOP_ADD, 2, o + s, o, s);"]

    def i_cmpxchg(self, insn, pfx):
        d, s = insn.operands
        if d.type != X.X86_OP_MEM or d.size != 4:
            raise Unhandled("cmpxchg form")
        return ["uint32_t a = %s, e = ctx->eax, old = e;" % self.ea(insn, d),
                "int ok = sg_lock_cmpxchg32(ctx, mem, a, &e, %s);" % self.rd(insn, s),
                "uint32_t cur = ok ? old : e;",
                "lf = SGR_FL(SG_FOP_SUB, 2, old - cur, old, cur);",
                "if (!ok) ctx->eax = cur;"]

    def i_cmpxchg8b(self, insn, pfx):
        d = insn.operands[0]
        return ["uint32_t a = %s;" % self.ea(insn, d),
                "uint64_t e = ((uint64_t)ctx->edx << 32) | ctx->eax;",
                "int ok = sg_lock_cmpxchg64(ctx, mem, a, &e, ((uint64_t)ctx->ecx << 32) | ctx->ebx);",
                "if (!ok) { ctx->eax = (uint32_t)e; ctx->edx = (uint32_t)(e >> 32); }",
                "lf = sgr_merge(lf, SG_ZF, ok ? SG_ZF : 0u);"]

    def setcc(self, insn, cc):
        d = insn.operands[0]
        return [self.wr(insn, d, "(uint32_t)%s" % self.cond(cc))]

    def cmov(self, insn, cc):
        d, s = insn.operands
        return ["uint32_t s = %s;" % self.rd(insn, s), "if (%s) %s" % (self.cond(cc), self.wr(insn, d, "s"))]

    def _bs(self, insn, rev):
        d, s = insn.operands
        sz = d.size
        return ["uint32_t s = %s;" % self.rd(insn, s),
                "if (s) %s" % self.wr(insn, d, "sgr_bs%s(s)" % ("r" if rev else "f")),
                "lf = sgr_merge(lf, SG_ZF, s ? 0u : SG_ZF);"]

    def i_bsr(self, insn): return self._bs(insn, True)
    def i_bsf(self, insn): return self._bs(insn, False)

    def i_bt(self, insn):
        d, s = insn.operands
        if d.type != X.X86_OP_REG:
            raise Unhandled("bt with memory")
        return ["uint32_t b = (%s >> (%s & %du)) & 1u;" % (self.rd(insn, d), self.rd(insn, s), BITS[d.size] - 1),
                "lf = sgr_merge(lf, SG_CF, b ? SG_CF : 0u);"]

    # ---------------------------------------------------------------- stack --
    def i_push(self, insn):
        s = insn.operands[0]
        if s.size != 4 and s.type != X.X86_OP_IMM:
            raise Unhandled("16-bit push")
        return ["SG_PUSH32(%s);" % self.rd(insn, s, size=4)]

    def i_pop(self, insn):
        d = insn.operands[0]
        if d.size != 4:
            raise Unhandled("16-bit pop")
        if d.type == X.X86_OP_REG:
            if d.reg == X.X86_REG_ESP:
                return ["ctx->esp = sg_ld32(mem, ctx->esp);"]
            return ["SG_POP32(ctx->%s);" % N32[R32[d.reg]]]
        # pop m32: esp is incremented before the destination address is computed
        return ["uint32_t v; SG_POP32(v);", self.wr(insn, d, "v")]

    def i_pushal(self, insn):
        return ["uint32_t t = ctx->esp;",
                "SG_PUSH32(ctx->eax); SG_PUSH32(ctx->ecx); SG_PUSH32(ctx->edx); SG_PUSH32(ctx->ebx);",
                "SG_PUSH32(t); SG_PUSH32(ctx->ebp); SG_PUSH32(ctx->esi); SG_PUSH32(ctx->edi);"]

    def i_popal(self, insn):
        return ["SG_POP32(ctx->edi); SG_POP32(ctx->esi); SG_POP32(ctx->ebp); ctx->esp += 4u;",
                "SG_POP32(ctx->ebx); SG_POP32(ctx->edx); SG_POP32(ctx->ecx); SG_POP32(ctx->eax);"]

    def i_pushfd(self, insn):
        return ["SG_PUSH32(sgr_eflags(lf) | 0x202u | ctx->sysfl);"]

    def i_popfd(self, insn):
        return ["uint32_t v; SG_POP32(v);", "lf = SGR_FLE(v); ctx->sysfl = v & SG_SYS_FLAGS;"]

    def i_lahf(self, insn):
        return ["SG_SET_R8H(ctx, 0, (sgr_eflags(lf) & 0xd5u) | 2u);"]

    def i_sahf(self, insn):
        return ["lf = sgr_merge(lf, SG_SF | SG_ZF | SG_AF | SG_PF | SG_CF, SG_R8H(ctx, 0));"]

    def i_leave(self, insn):
        return ["ctx->esp = ctx->ebp;", "SG_POP32(ctx->ebp);"]

    def i_clc(self, insn): return ["lf = sgr_merge(lf, SG_CF, 0u);"]
    def i_stc(self, insn): return ["lf = sgr_merge(lf, SG_CF, SG_CF);"]
    def i_cmc(self, insn): return ["lf = sgr_merge(lf, SG_CF, sgr_cf(lf) ? 0u : SG_CF);"]

    # ---------------------------------------------------------------- system --
    def i_nop(self, insn): return []
    def i_pause(self, insn): return []
    def i_cpuid(self, insn): return ["sg_cpuid(ctx);"]
    def i_rdtsc(self, insn): return ["sg_rdtsc(ctx);"]
    def i_sfence(self, insn): return ["__atomic_thread_fence(__ATOMIC_RELEASE);"]
    def i_emms(self, insn): return ["ctx->fpu.tag = 0xffffu;"]

    def i_fxsave(self, insn):
        return ["sg_fxsave(ctx, mem, %s);" % self.ea(insn, insn.operands[0])]

    def i_ldmxcsr(self, insn):
        return ["uint32_t v = sg_ld32(mem, %s);" % self.ea(insn, insn.operands[0]),
                "ctx->mxcsr = v; sg_set_mxcsr(ctx, v);"]

    def i_stmxcsr(self, insn):
        return ["sg_st32(mem, %s, ctx->mxcsr);" % self.ea(insn, insn.operands[0])]

    # ---------------------------------------------------------------- strings --
    def _str(self, pfx, kind, sz):
        rep = 1 if pfx in ("rep", "repe", "repz") else 0
        if pfx in ("repne", "repnz"):
            raise Unhandled("repne string op")
        if kind == "movs":
            return ["sgr_rep_movs(ctx, mem, %d, %d);" % (sz, rep)]
        if kind == "stos":
            return ["sgr_rep_stos(ctx, mem, %d, %d);" % (sz, rep)]
        return ["sgr_repe_cmps(ctx, mem, %d, %d, &lf);" % (sz, rep)]

    def i_movsb(self, insn, pfx): return self._str(pfx, "movs", 1)
    def i_movsw(self, insn, pfx): return self._str(pfx, "movs", 2)
    def i_stosb(self, insn, pfx): return self._str(pfx, "stos", 1)
    def i_stosw(self, insn, pfx): return self._str(pfx, "stos", 2)
    def i_stosd(self, insn, pfx): return self._str(pfx, "stos", 4)
    def i_cmpsb(self, insn, pfx): return self._str(pfx, "cmps", 1)
    def i_cmpsw(self, insn, pfx): return self._str(pfx, "cmps", 2)

    def i_movsd(self, insn, pfx):
        if insn.operands and insn.operands[0].type == X.X86_OP_REG:
            raise Unhandled("SSE2 movsd")
        return self._str(pfx, "movs", 4)

    def i_cmpsd(self, insn, pfx):
        if insn.operands and insn.operands[0].type == X.X86_OP_REG:
            raise Unhandled("SSE2 cmpsd")
        return self._str(pfx, "cmps", 4)

    # ---------------------------------------------------------------- SSE --
    def xr(self, op):
        if op.type != X.X86_OP_REG or op.reg not in XMM:
            raise Unhandled("xmm operand")
        return "ctx->xmm[%d]" % XMM[op.reg]

    def xsrc(self, insn, op, n=16):
        """An xmm-or-memory source as an sg_xmm value (n = bytes read from memory)."""
        if op.type == X.X86_OP_REG:
            if op.reg in XMM:
                return "ctx->xmm[%d]" % XMM[op.reg]
            if op.reg in MM:
                return "sgr_x64(ctx->mm[%d])" % MM[op.reg]
            raise Unhandled("xmm source")
        a = self.ea(insn, op)
        return {16: "sgr_ldx(mem, %s)", 8: "sgr_ldx64(mem, %s)", 4: "sgr_ldx32(mem, %s)"}[n] % a

    def fsrc(self, insn, op):
        """A scalar float source (xmm low lane or m32)."""
        if op.type == X.X86_OP_REG:
            return "ctx->xmm[%d].f32[0]" % XMM[op.reg]
        return "sg_ldf32(mem, %s)" % self.ea(insn, op)

    def _ss(self, insn, expr):
        d, s = insn.operands
        return ["float x = %s.f32[0], y = %s;" % (self.xr(d), self.fsrc(insn, s)),
                "%s.f32[0] = %s;" % (self.xr(d), expr)]

    def i_addss(self, insn): return self._ss(insn, "x + y")
    def i_subss(self, insn): return self._ss(insn, "x - y")
    def i_mulss(self, insn): return self._ss(insn, "x * y")
    def i_divss(self, insn): return self._ss(insn, "x / y")
    def i_minss(self, insn): return self._ss(insn, "sgr_minf(x, y)")
    def i_maxss(self, insn): return self._ss(insn, "sgr_maxf(x, y)")
    def i_sqrtss(self, insn): return self._ss(insn, "sqrtf(y)")
    def i_rcpss(self, insn): return self._ss(insn, "sgr_rcpf(y)")
    def i_rsqrtss(self, insn): return self._ss(insn, "sgr_rsqrtf(y)")

    def _ps(self, insn, expr, lane="f32"):
        d, s = insn.operands
        return ["sg_xmm y = %s; sg_xmm *p = &%s; unsigned i;" % (self.xsrc(insn, s), self.xr(d)),
                "for (i = 0; i < 4; i++) { %s x = p->%s[i], b = y.%s[i]; p->%s[i] = %s; }"
                % ("float" if lane == "f32" else "uint32_t", lane, lane, lane, expr)]

    def i_addps(self, insn): return self._ps(insn, "x + b")
    def i_subps(self, insn): return self._ps(insn, "x - b")
    def i_mulps(self, insn): return self._ps(insn, "x * b")
    def i_divps(self, insn): return self._ps(insn, "x / b")
    def i_minps(self, insn): return self._ps(insn, "sgr_minf(x, b)")
    def i_maxps(self, insn): return self._ps(insn, "sgr_maxf(x, b)")
    def i_sqrtps(self, insn): return self._ps(insn, "sqrtf(b)")
    def i_rcpps(self, insn): return self._ps(insn, "sgr_rcpf(b)")
    def i_rsqrtps(self, insn): return self._ps(insn, "sgr_rsqrtf(b)")
    def i_andps(self, insn): return self._ps(insn, "x & b", "u32")
    def i_andnps(self, insn): return self._ps(insn, "~x & b", "u32")
    def i_orps(self, insn): return self._ps(insn, "x | b", "u32")
    def i_xorps(self, insn): return self._ps(insn, "x ^ b", "u32")
    i_andpd, i_orpd, i_xorpd = i_andps, i_orps, i_xorps

    def cmpps(self, insn, pred):
        d, s = insn.operands[:2]
        return ["sg_xmm y = %s; sg_xmm *p = &%s; unsigned i;" % (self.xsrc(insn, s), self.xr(d)),
                "for (i = 0; i < 4; i++) p->u32[i] = sgr_fcmpmask(p->f32[i], y.f32[i], %du);" % pred]

    def i_cmpps(self, insn):
        return self.cmpps(insn, insn.operands[2].imm & 7)

    def _comi(self, insn):
        d, s = insn.operands
        return ["lf = SGR_FLE(sgr_fcmp_eflags(%s.f32[0], %s));" % (self.xr(d), self.fsrc(insn, s))]

    def i_comiss(self, insn): return self._comi(insn)
    def i_ucomiss(self, insn): return self._comi(insn)

    def i_movss(self, insn):
        d, s = insn.operands
        if d.type == X.X86_OP_MEM:
            return ["sg_stf32(mem, %s, %s.f32[0]);" % (self.ea(insn, d), self.xr(s))]
        if s.type == X.X86_OP_MEM:
            return ["%s = sgr_ldx32(mem, %s);" % (self.xr(d), self.ea(insn, s))]
        return ["%s.u32[0] = %s.u32[0];" % (self.xr(d), self.xr(s))]

    def _mov128(self, insn):
        d, s = insn.operands
        if d.type == X.X86_OP_MEM:
            return ["sg_st128(mem, %s, &%s);" % (self.ea(insn, d), self.xr(s))]
        return ["%s = %s;" % (self.xr(d), self.xsrc(insn, s))]

    i_movaps = i_movups = i_movdqa = i_movdqu = i_movapd = i_movntps = _mov128

    def _movlh(self, insn, half):
        d, s = insn.operands
        if d.type == X.X86_OP_MEM:
            return ["sg_st64(mem, %s, %s.u64[%d]);" % (self.ea(insn, d), self.xr(s), half)]
        return ["%s.u64[%d] = sg_ld64(mem, %s);" % (self.xr(d), half, self.ea(insn, s))]

    def i_movlps(self, insn): return self._movlh(insn, 0)
    def i_movhps(self, insn): return self._movlh(insn, 1)
    i_movlpd, i_movhpd = i_movlps, i_movhps

    def i_movhlps(self, insn):
        d, s = insn.operands
        return ["%s.u64[0] = %s.u64[1];" % (self.xr(d), self.xr(s))]

    def i_movlhps(self, insn):
        d, s = insn.operands
        return ["%s.u64[1] = %s.u64[0];" % (self.xr(d), self.xr(s))]

    def i_unpcklps(self, insn):
        d, s = insn.operands
        return ["sg_xmm y = %s, x = %s;" % (self.xsrc(insn, s), self.xr(d)),
                "%s = sgr_xunpck(x, y, 4, 0);" % self.xr(d)]

    def i_unpckhps(self, insn):
        d, s = insn.operands
        return ["sg_xmm y = %s, x = %s;" % (self.xsrc(insn, s), self.xr(d)),
                "%s = sgr_xunpck(x, y, 4, 1);" % self.xr(d)]

    def i_shufps(self, insn):
        d, s, imm = insn.operands
        return ["%s = sgr_shufps(%s, %s, %du);" % (self.xr(d), self.xr(d), self.xsrc(insn, s), imm.imm & 0xFF)]

    def i_movmskps(self, insn):
        d, s = insn.operands
        return [self.wr(insn, d, "sgr_movmskps(%s)" % self.xr(s))]

    def i_cvtsi2ss(self, insn):
        d, s = insn.operands
        return ["%s.f32[0] = (float)(int32_t)%s;" % (self.xr(d), self.rd(insn, s))]

    def i_cvtss2si(self, insn):
        d, s = insn.operands
        return [self.wr(insn, d, "sgr_cvt_f2i(%s, 0)" % self.fsrc(insn, s))]

    def i_cvttss2si(self, insn):
        d, s = insn.operands
        return [self.wr(insn, d, "sgr_cvt_f2i(%s, 1)" % self.fsrc(insn, s))]

    def i_cvttsd2si(self, insn):
        d, s = insn.operands
        src = ("sg_ldf64(mem, %s)" % self.ea(insn, s)) if s.type == X.X86_OP_MEM else "%s.f64[0]" % self.xr(s)
        return [self.wr(insn, d, "sgr_cvt_f2i(%s, 1)" % src)]

    def i_cvtpi2ps(self, insn):
        d, s = insn.operands
        src = ("sg_ld64(mem, %s)" % self.ea(insn, s)) if s.type == X.X86_OP_MEM else "ctx->mm[%d]" % MM[s.reg]
        return ["uint64_t v = %s;" % src,
                "%s.f32[0] = (float)(int32_t)(uint32_t)v; %s.f32[1] = (float)(int32_t)(uint32_t)(v >> 32);"
                % (self.xr(d), self.xr(d))]

    def i_cvtps2pi(self, insn):
        d, s = insn.operands
        return ["ctx->mm[%d] = sgr_cvtps2pi(%s, 0);" % (MM[d.reg], self.xsrc(insn, s, 8))]

    def i_cvttps2pi(self, insn):
        d, s = insn.operands
        return ["ctx->mm[%d] = sgr_cvtps2pi(%s, 1);" % (MM[d.reg], self.xsrc(insn, s, 8))]

    def i_cvttps2dq(self, insn):
        d, s = insn.operands
        return ["sg_xmm y = %s; unsigned i;" % self.xsrc(insn, s),
                "for (i = 0; i < 4; i++) %s.u32[i] = sgr_cvt_f2i(y.f32[i], 1);" % self.xr(d)]

    # movd / movq between general registers, memory, MMX and xmm
    def i_movd(self, insn):
        d, s = insn.operands
        if d.type == X.X86_OP_REG and d.reg in XMM:
            return ["%s = sgr_x64((uint64_t)%s);" % (self.xr(d), self.rd(insn, s, size=4))]
        if d.type == X.X86_OP_REG and d.reg in MM:
            return ["ctx->mm[%d] = (uint64_t)%s;" % (MM[d.reg], self.rd(insn, s, size=4))]
        if s.reg in XMM:
            return [self.wr(insn, d, "%s.u32[0]" % self.xr(s))]
        return [self.wr(insn, d, "(uint32_t)ctx->mm[%d]" % MM[s.reg])]

    def i_movq(self, insn):
        d, s = insn.operands

        def src64(op):
            if op.type == X.X86_OP_MEM:
                return "sg_ld64(mem, %s)" % self.ea(insn, op)
            if op.reg in MM:
                return "ctx->mm[%d]" % MM[op.reg]
            return "%s.u64[0]" % self.xr(op)

        if d.type == X.X86_OP_MEM:
            return ["sg_st64(mem, %s, %s);" % (self.ea(insn, d), src64(s))]
        if d.reg in MM:
            return ["ctx->mm[%d] = %s;" % (MM[d.reg], src64(s))]
        return ["%s = sgr_x64(%s);" % (self.xr(d), src64(s))]

    def i_movntq(self, insn):
        d, s = insn.operands
        return ["sg_st64(mem, %s, ctx->mm[%d]);" % (self.ea(insn, d), MM[s.reg])]

    def i_movdq2q(self, insn):
        d, s = insn.operands
        return ["ctx->mm[%d] = %s.u64[0];" % (MM[d.reg], self.xr(s))]

    # MMX / SSE2 integer lane ops
    def lane_op(self, insn, base):
        fn = LANE_OPS[base]
        d, s = insn.operands[0], insn.operands[1]
        if d.reg in MM:
            src = ("sg_ld64(mem, %s)" % self.ea(insn, s)) if s.type == X.X86_OP_MEM else (
                ("ctx->mm[%d]" % MM[s.reg]) if s.type == X.X86_OP_REG else "%du" % (s.imm & 0xFF))
            if base.startswith("pack") or base.startswith("punpck"):
                return self.mmx_pack(insn, base, d, src)
            return ["ctx->mm[%d] = %s(ctx->mm[%d], %s);" % (MM[d.reg], fn, MM[d.reg], src)]
        # xmm forms (SSE2 integer)
        if s.type == X.X86_OP_IMM:
            return ["%s.u64[0] = %s(%s.u64[0], %du); %s.u64[1] = %s(%s.u64[1], %du);"
                    % (self.xr(d), fn, self.xr(d), s.imm & 0xFF, self.xr(d), fn, self.xr(d), s.imm & 0xFF)]
        if base.startswith("pack"):
            return ["%s = sgr_xpack(%s, %s, %s);" % (self.xr(d), self.xr(d), self.xsrc(insn, s), fn)]
        if base.startswith("punpck"):
            e, hi = UNPCK[base]
            return ["sg_xmm y = %s, x = %s;" % (self.xsrc(insn, s), self.xr(d)),
                    "%s = sgr_xunpck(x, y, %d, %d);" % (self.xr(d), e, hi)]
        if base in PSHIFTS:
            # shift by an xmm/m128 count: low 64 bits are the count
            return ["uint64_t c = %s.u64[0];" % self.xsrc(insn, s),
                    "%s.u64[0] = %s(%s.u64[0], c); %s.u64[1] = %s(%s.u64[1], c);"
                    % (self.xr(d), fn, self.xr(d), self.xr(d), fn, self.xr(d))]
        return ["SGR_X2(%s, %s, %s);" % (fn, self.xr(d), self.xsrc(insn, s))]

    def mmx_pack(self, insn, base, d, src):
        n = MM[d.reg]
        if base.startswith("pack"):
            return ["ctx->mm[%d] = %s(ctx->mm[%d], %s);" % (n, LANE_OPS[base], n, src)]
        e, hi = UNPCK[base]
        if src.startswith("sg_ld64") and not hi:
            src = src.replace("sg_ld64", "(uint64_t)sg_ld32")  # punpckl* mm, m32 reads 32 bits
        return ["ctx->mm[%d] = sgr_unpck(ctx->mm[%d], %s, %d, %d);" % (n, n, src, e, hi)]

    def i_pshufw(self, insn):
        d, s, imm = insn.operands
        src = ("sg_ld64(mem, %s)" % self.ea(insn, s)) if s.type == X.X86_OP_MEM else "ctx->mm[%d]" % MM[s.reg]
        return ["ctx->mm[%d] = sgr_pshufw(%s, %du);" % (MM[d.reg], src, imm.imm & 0xFF)]

    def i_pshufd(self, insn):
        d, s, imm = insn.operands
        return ["%s = sgr_pshufd(%s, %du);" % (self.xr(d), self.xsrc(insn, s), imm.imm & 0xFF)]

    def i_psrldq(self, insn):
        d, imm = insn.operands
        return ["%s = sgr_psrldq(%s, %du);" % (self.xr(d), self.xr(d), min(imm.imm & 0xFF, 16))]

    def i_pslldq(self, insn):
        d, imm = insn.operands
        return ["%s = sgr_pslldq(%s, %du);" % (self.xr(d), self.xr(d), min(imm.imm & 0xFF, 16))]

    def i_pextrw(self, insn):
        d, s, imm = insn.operands
        if s.reg in MM:
            return [self.wr(insn, d, "(uint32_t)(uint16_t)(ctx->mm[%d] >> (16u * (%du & 3u)))" % (MM[s.reg], imm.imm))]
        return [self.wr(insn, d, "(uint32_t)(uint16_t)(%s.u64[(%du >> 2) & 1u] >> (16u * (%du & 3u)))"
                        % (self.xr(s), imm.imm & 7, imm.imm & 7))]

    def i_pmovmskb(self, insn):
        d, s = insn.operands
        if s.reg in MM:
            return [self.wr(insn, d, "sgr_pmovmskb64(ctx->mm[%d])" % MM[s.reg])]
        return [self.wr(insn, d, "sgr_pmovmskb64(%s.u64[0]) | (sgr_pmovmskb64(%s.u64[1]) << 8)" % (self.xr(s), self.xr(s)))]

    # ---------------------------------------------------------------- x87 --
    def x87(self, insn, op, modrm):
        """x87 decoded from the opcode bytes (immune to assembler mnemonic swaps for fsub/fdiv)."""
        mod, reg, rm = modrm >> 6, (modrm >> 3) & 7, modrm & 7
        ARITH = {0: "x + y", 1: "x * y", 4: "x - y", 5: "y - x", 6: "x / y", 7: "y / x"}
        if mod != 3:
            mops = [o for o in insn.operands if o.type == X.X86_OP_MEM]
            a = self.ea(insn, mops[0])
            if op in (0xD8, 0xDC, 0xDA, 0xDE):
                ld = {0xD8: "sgr_ldf32(mem, a)", 0xDC: "sg_ldf64(mem, a)",
                      0xDA: "(double)(int32_t)sg_ld32(mem, a)", 0xDE: "(double)(int16_t)sg_ld16(mem, a)"}[op]
                out = ["uint32_t a = %s;" % a, "double x = SGR_ST(0), y = %s;" % ld]
                if reg in ARITH:
                    out.append("SGR_ST(0) = SG_X87_PC(ctx, %s);" % ARITH[reg])
                elif reg == 2:
                    out.append("sgr_fcom(ctx, x, y);")
                else:
                    out.append("sgr_fcom(ctx, x, y); sgr_fpop(ctx);")
                return out
            if op == 0xD9:
                if reg == 0:
                    return ["sgr_fpush(ctx, sgr_ldf32(mem, %s));" % a]
                if reg == 2:
                    return ["sgr_stf32(mem, %s, SGR_ST(0));" % a]
                if reg == 3:
                    return ["sgr_stf32(mem, %s, SGR_ST(0)); sgr_fpop(ctx);" % a]
                if reg == 5:
                    return ["ctx->fpu.cw = sg_ld16(mem, %s);" % a]
                if reg == 7:
                    return ["sg_st16(mem, %s, ctx->fpu.cw);" % a]
                raise Unhandled("fldenv/fnstenv")
            if op == 0xDB:
                if reg == 0:
                    return ["sgr_fpush(ctx, (double)(int32_t)sg_ld32(mem, %s));" % a]
                if reg in (2, 3):
                    return ["sg_st32(mem, %s, sgr_fist32(ctx, SGR_ST(0)));%s" % (a, " sgr_fpop(ctx);" if reg == 3 else "")]
                if reg == 5:
                    return ["sgr_fpush(ctx, sg_ldf80(mem, %s));" % a]
                if reg == 7:
                    return ["sg_stf80(mem, %s, SGR_ST(0)); sgr_fpop(ctx);" % a]
                raise Unhandled("fisttp")
            if op == 0xDD:
                if reg == 0:
                    return ["sgr_fpush(ctx, sg_ldf64(mem, %s));" % a]
                if reg in (2, 3):
                    return ["sg_stf64(mem, %s, SGR_ST(0));%s" % (a, " sgr_fpop(ctx);" if reg == 3 else "")]
                if reg == 7:
                    return ["sg_st16(mem, %s, sgr_fnstsw(ctx));" % a]
                raise Unhandled("frstor/fnsave")
            if op == 0xDF:
                if reg == 0:
                    return ["sgr_fpush(ctx, (double)(int16_t)sg_ld16(mem, %s));" % a]
                if reg in (2, 3):
                    return ["sg_st16(mem, %s, sgr_fist16(ctx, SGR_ST(0)));%s" % (a, " sgr_fpop(ctx);" if reg == 3 else "")]
                if reg == 5:
                    return ["sgr_fpush(ctx, (double)(int64_t)sg_ld64(mem, %s));" % a]
                if reg == 7:
                    return ["sg_st64(mem, %s, sgr_fist64(ctx, SGR_ST(0))); sgr_fpop(ctx);" % a]
                raise Unhandled("fbld/fbstp/fisttp")
            raise Unhandled("x87 memory form")
        i = rm
        if op == 0xD8:  # st0 = st0 op sti
            if reg in ARITH:
                return ["double x = SGR_ST(0), y = SGR_ST(%d);" % i, "SGR_ST(0) = SG_X87_PC(ctx, %s);" % ARITH[reg]]
            return ["sgr_fcom(ctx, SGR_ST(0), SGR_ST(%d));%s" % (i, " sgr_fpop(ctx);" if reg == 3 else "")]
        if op in (0xDC, 0xDE):  # sti = sti op st0 [and pop]; Intel SDM opcode table
            REV = {0: "x + y", 1: "x * y", 4: "y - x", 5: "x - y", 6: "y / x", 7: "x / y"}
            pop = " sgr_fpop(ctx);" if op == 0xDE else ""
            if reg in REV:
                return ["double x = SGR_ST(%d), y = SGR_ST(0);" % i,
                        "SGR_ST(%d) = SG_X87_PC(ctx, %s);%s" % (i, REV[reg], pop)]
            if op == 0xDE and reg == 3 and i == 1:
                return ["sgr_fcom(ctx, SGR_ST(0), SGR_ST(1)); sgr_fpop(ctx); sgr_fpop(ctx);"]  # fcompp
            if op == 0xDC and reg in (2, 3):
                return ["sgr_fcom(ctx, SGR_ST(0), SGR_ST(%d));%s" % (i, " sgr_fpop(ctx);" if reg == 3 else "")]
            raise Unhandled("x87 DE/DC form")
        if op == 0xD9:
            if reg == 0:
                return ["double v = SGR_ST(%d); sgr_fpush(ctx, v);" % i]
            if reg == 1:
                return ["sgr_fxch(ctx, %d);" % i]
            if modrm == 0xD0:
                return []
            one = {0xE0: "SGR_ST(0) = -SGR_ST(0);", 0xE1: "SGR_ST(0) = fabs(SGR_ST(0));",
                   0xE8: "sgr_fpush(ctx, 1.0);", 0xE9: "sgr_fpush(ctx, SGR_L2T);", 0xEA: "sgr_fpush(ctx, SGR_L2E);",
                   0xEB: "sgr_fpush(ctx, SGR_PI);", 0xEC: "sgr_fpush(ctx, SGR_LG2);", 0xED: "sgr_fpush(ctx, SGR_LN2);",
                   0xEE: "sgr_fpush(ctx, 0.0);", 0xF0: "sgr_f2xm1(ctx);", 0xF1: "sgr_fyl2x(ctx);",
                   0xF2: "sgr_fptan(ctx);", 0xF3: "sgr_fpatan(ctx);",
                   0xFA: "SGR_ST(0) = SG_X87_PC(ctx, sqrt(SGR_ST(0)));", 0xFC: "SGR_ST(0) = sgr_frnd(ctx, SGR_ST(0));",
                   0xFD: "sgr_fscale(ctx);", 0xFE: "sgr_fsin(ctx);", 0xFF: "sgr_fcos(ctx);",
                   0xE4: "sgr_fcom(ctx, SGR_ST(0), 0.0);",
                   0xF6: "ctx->fpu.top = (ctx->fpu.top - 1u) & 7u;", 0xF7: "ctx->fpu.top = (ctx->fpu.top + 1u) & 7u;",
                   0xFB: "sgr_fsincos(ctx);"}
            if modrm in one:
                return [one[modrm]]
            raise Unhandled("x87 D9 form")
        if op == 0xDA and modrm == 0xE9:
            return ["sgr_fcom(ctx, SGR_ST(0), SGR_ST(1)); sgr_fpop(ctx); sgr_fpop(ctx);"]  # fucompp
        if op == 0xDB:
            if reg in (5, 6):  # fucomi / fcomi
                return ["lf = SGR_FLE(sgr_fcmp_eflags(SGR_ST(0), SGR_ST(%d)));" % i]
            if modrm == 0xE2:
                return ["ctx->fpu.sw &= 0x7f00u;"]
            if modrm == 0xE3:
                return ["ctx->fpu.cw = 0x037fu; ctx->fpu.sw = 0; ctx->fpu.top = 0; ctx->fpu.tag = 0xffffu;"]
            raise Unhandled("fcmov")
        if op == 0xDD:
            if reg == 0:
                return ["ctx->fpu.tag |= (uint16_t)(3u << (2u * ((ctx->fpu.top + %du) & 7u)));" % i]  # ffree
            if reg == 2:
                return ["SGR_ST(%d) = SGR_ST(0);" % i]
            if reg == 3:
                return ["SGR_ST(%d) = SGR_ST(0); sgr_fpop(ctx);" % i]
            if reg in (4, 5):
                return ["sgr_fcom(ctx, SGR_ST(0), SGR_ST(%d));%s" % (i, " sgr_fpop(ctx);" if reg == 5 else "")]
            raise Unhandled("x87 DD form")
        if op == 0xDF:
            if modrm == 0xE0:
                return ["SG_SET_R16(ctx, 0, sgr_fnstsw(ctx));"]
            if reg in (5, 6):  # fucomip / fcomip
                return ["lf = SGR_FLE(sgr_fcmp_eflags(SGR_ST(0), SGR_ST(%d))); sgr_fpop(ctx);" % i]
            raise Unhandled("x87 DF form")
        raise Unhandled("x87 form")


CMPPS = {"eq": 0, "lt": 1, "le": 2, "unord": 3, "neq": 4, "nlt": 5, "nle": 6, "ord": 7}

LANE_OPS = {
    "paddb": "sgr_paddb", "paddw": "sgr_paddw", "paddd": "sgr_paddd", "paddq": "sgr_paddq",
    "psubb": "sgr_psubb", "psubw": "sgr_psubw", "psubd": "sgr_psubd", "psubq": "sgr_psubq",
    "paddsb": "sgr_paddsb", "paddsw": "sgr_paddsw", "psubsb": "sgr_psubsb", "psubsw": "sgr_psubsw",
    "paddusb": "sgr_paddusb", "paddusw": "sgr_paddusw", "psubusb": "sgr_psubusb", "psubusw": "sgr_psubusw",
    "pmullw": "sgr_pmullw", "pmulhw": "sgr_pmulhw", "pmulhuw": "sgr_pmulhuw", "pmaddwd": "sgr_pmaddwd",
    "pminub": "sgr_pminub", "pmaxub": "sgr_pmaxub", "pminsw": "sgr_pminsw", "pmaxsw": "sgr_pmaxsw",
    "pavgb": "sgr_pavgb", "pavgw": "sgr_pavgw",
    "pcmpeqb": "sgr_pcmpeqb", "pcmpeqw": "sgr_pcmpeqw", "pcmpeqd": "sgr_pcmpeqd",
    "pcmpgtb": "sgr_pcmpgtb", "pcmpgtw": "sgr_pcmpgtw", "pcmpgtd": "sgr_pcmpgtd",
    "pand": "sgr_pand", "pandn": "sgr_pandn", "por": "sgr_por", "pxor": "sgr_pxor",
    "psllw": "sgr_psllw", "pslld": "sgr_pslld", "psllq": "sgr_psllq",
    "psrlw": "sgr_psrlw", "psrld": "sgr_psrld", "psrlq": "sgr_psrlq",
    "psraw": "sgr_psraw", "psrad": "sgr_psrad",
    "packsswb": "sgr_packsswb", "packuswb": "sgr_packuswb", "packssdw": "sgr_packssdw",
    "punpcklbw": "", "punpcklwd": "", "punpckldq": "", "punpcklqdq": "",
    "punpckhbw": "", "punpckhwd": "", "punpckhdq": "", "punpckhqdq": "",
}
PSHIFTS = {"psllw", "pslld", "psllq", "psrlw", "psrld", "psrlq", "psraw", "psrad"}
UNPCK = {"punpcklbw": (1, 0), "punpcklwd": (2, 0), "punpckldq": (4, 0), "punpcklqdq": (8, 0),
         "punpckhbw": (1, 1), "punpckhwd": (2, 1), "punpckhdq": (4, 1), "punpckhqdq": (8, 1)}
