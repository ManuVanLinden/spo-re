"""One guest function -> one SG_FUNC in C (runtime-abi.md sections 5, 6 and 10).

  call rel32          SG_CALL(sg_f_<target>, ret)          (call $+5 becomes a push)
  call [IAT]          SG_CALL(sg_imp_<dll>_<name>, ret)
  call r/m32          SG_CALL_IND(expr, ret)               (target evaluated before the push)
  ret / ret N         SG_RET(N)                            (landing owners: SG_LANDING_RET)
  jmp other entry     SG_TAILCALL(sg_f_<target>)           (landing owners: SG_LANDING_TAIL)
  jmp [IAT]           SG_TAILCALL(sg_imp_...)
  jmp r/m32           SG_TAILCALL_IND(expr)
  jmp [i*4+T]         switch over the manifest table; out of range -> SG_TAILCALL_IND -> miss hook
  resume points       SG_LANDING_BEGIN + SG_RESUME_DISPATCH (data/eh/resume-points.json)
"""
from capstone import x86_const as X

from .lift import Lifter, Unhandled, CC, h

DECL_IMPORT = "extern sg_func %s;"


def fname(va):
    return "sg_f_%08x" % va


class Emitter:
    def __init__(self, image, entries, landing=None, noret_targets=frozenset(), memorder=None):
        self.image = image
        self.entries = entries
        self.landing = landing or {}  # owner entry -> [resume VAs]
        self.lifter = Lifter()
        self.refs_f = set()
        self.refs_imp = set()
        self.defined = set()
        self.stats = {"functions": 0, "insns": 0, "unhandled": 0}
        self.unhandled = []  # (va, function, text, reason)
        self.flags_through_calls = False  # fixture builds: reload lf from ctx->fl after each call
        self.memorder = memorder  # sgrecomp.memorder.MemOrder, or None for the relaxed emission (p0)

    # ---------------------------------------------------------------- helpers --
    def callee(self, t):
        self.refs_f.add(t)
        return fname(t)

    def imp_at(self, op):
        """Import symbol if `op` is [abs] on an IAT slot."""
        if op.type == X.X86_OP_MEM and not op.mem.base and not op.mem.index and not op.mem.segment:
            sym = self.image.imports.get(op.mem.disp & 0xFFFFFFFF)
            if sym:
                self.refs_imp.add(sym)
                return sym
        return None

    def tail(self, fn_expr, land):
        if land:
            return ["SGR_EXIT_FLAGS();", "SG_LANDING_TAIL(%s);" % fn_expr]
        return ["SGR_EXIT_FLAGS();", "SG_TAILCALL(%s);" % fn_expr]

    def tail_ind(self, expr, land):
        if land:
            return ["{ uint32_t t = %s; sg_func *f = sg_lookup(ctx, mem, t); SGR_EXIT_FLAGS(); SG_LANDING_POP(); f(ctx, mem); return; }" % expr]
        return ["SGR_EXIT_FLAGS();", "SG_TAILCALL_IND(%s);" % expr]

    def target_expr(self, insn, op):
        if op.type == X.X86_OP_REG:
            return self.lifter.rd(insn, op)
        return "sg_ld32(mem, %s)" % self.lifter.ea(insn, op)

    # ---------------------------------------------------------------- function --
    def function(self, fn):
        """Return C text for one translated function."""
        L = self.lifter
        entry = fn.entry
        land = entry in self.landing
        if self.memorder is not None:
            self.memorder.begin_function(fn)
        self.defined.add(entry)
        self.stats["functions"] += 1
        blocks = fn.blocks
        starts = {b.start for b in blocks}
        labels = set()
        body = []
        order = [b.start for b in blocks]
        for k, b in enumerate(blocks):
            nxt = order[k + 1] if k + 1 < len(order) else None
            body.append(("label", b.start))
            for insn in b.insns[:-1] if b.term != "fall" and b.term != "fallout" else b.insns:
                body.extend(self.insn_lines(fn, insn))
            last = b.insns[-1]
            t = b.term
            if t in ("fall", "fallout"):
                tgt = b.succ[0]
                if t == "fallout":
                    body.extend(self.tail(self.callee(tgt), land))
                elif tgt != nxt:
                    body.append("goto L_%08x;" % tgt)
                    labels.add(tgt)
                continue
            if last is None:
                body.append("sg_unhandled_insn(ctx, %s); /* undecodable */" % h(b.insns and b.start or 0))
                self.note_unhandled(fn, b.start, "(undecodable)", "decode")
                continue
            self.stats["insns"] += 1
            body.append("/* %08x: %s %s */" % (last.address, last.mnemonic, last.op_str))
            mn = last.mnemonic.split()[-1]
            if t == "ret":
                n = last.operands[0].imm if last.operands else 0
                body.append("SGR_EXIT_FLAGS(); SG_LANDING_RET(%d);" % n if land else "SGR_EXIT_FLAGS(); SG_RET(%d);" % n)
            elif t == "jmp":
                tg = b.succ[0]
                if tg in starts:
                    body.append("goto L_%08x;" % tg)
                    labels.add(tg)
                else:
                    body.extend(self.tail(self.callee(tg), land))
            elif t == "jcc":
                tg, ft = b.succ
                c = self.jcc_cond(last, mn)
                if tg in starts:
                    body.append("if (%s) goto L_%08x;" % (c, tg))
                    labels.add(tg)
                else:
                    body.append("if (%s) {" % c)
                    body.extend("    " + x for x in self.tail(self.callee(tg), land))
                    body.append("}")
                if ft != nxt:
                    if ft in starts:
                        body.append("goto L_%08x;" % ft)
                        labels.add(ft)
                    else:
                        body.extend(self.tail(self.callee(ft), land))
            elif t == "table":
                body.extend(self.switch(last, b.succ, starts, labels, land))
            elif t == "jmpind":
                op = last.operands[0]
                sym = self.imp_at(op)
                if sym:
                    body.extend(self.tail(sym, land))
                else:
                    body.extend(self.tail_ind(self.target_expr(last, op), land))
            elif t == "noret":
                body.extend(self.call_lines(last))
                body.append("sg_unhandled_insn(ctx, %s); /* a no-return call returned */" % h(last.address + last.size))
            elif t == "trap":
                body.append("sg_int3(ctx, %s);" % h(last.address))
            else:
                body.append("sg_unhandled_insn(ctx, %s);" % h(last.address))
                self.note_unhandled(fn, last.address, last.mnemonic, t)
        # resume points
        rps = sorted(self.landing.get(entry, []))
        for r in rps:
            labels.add(r)
        if blocks and blocks[0].start != entry:
            labels.add(entry)
        out = ["SG_FUNC(%08x) {" % entry, "    sg_lazyflags lf = ctx->fl;", "    (void)lf;"]
        if land:
            out.append("    SG_LANDING_BEGIN(%s);" % h(entry))
        if blocks and blocks[0].start != entry:
            out.append("    goto L_%08x;" % entry)
        for item in body:
            if isinstance(item, tuple):
                if item[1] in labels:
                    out.append("L_%08x:;" % item[1])
            else:
                out.append("    " + item)
        if blocks:
            last_b = blocks[-1]
            if last_b.term in ("fall",) and last_b.succ[0] not in starts:
                out.append("    sg_unhandled_insn(ctx, %s); /* fell off the function */" % h(last_b.end))
        if land:
            out.append("SG_RESUME_DISPATCH:")
            out.append("    switch (ctx->eip) {")
            for r in rps:
                out.append("    case %s: goto L_%08x;" % (h(r), r))
            out.append("    default: sg_bad_resume(ctx);")
            out.append("    }")
        out.append("}")
        return "\n".join(out) + "\n"

    def note_unhandled(self, fn, va, text, reason):
        self.stats["unhandled"] += 1
        self.unhandled.append((va, fn.entry, text, reason))

    def jcc_cond(self, insn, mn):
        if mn == "jcxz":
            return "SG_R16(ctx, 1) == 0u"
        if mn == "jecxz":
            return "ctx->ecx == 0u"
        cc = mn[1:]
        if cc not in CC:
            raise Unhandled(mn)
        return self.lifter.cond(cc)

    def switch(self, insn, slots, starts, labels, land):
        op = insn.operands[0]
        table = op.mem.disp & 0xFFFFFFFF
        e = self.lifter.ea(insn, op)
        out = ["/* %08x: %s %s */" % (insn.address, insn.mnemonic, insn.op_str),
               "switch ((uint32_t)(%s - %s)) {" % (e, h(table))]
        bytarget = {}
        for k, t in enumerate(slots):
            bytarget.setdefault(t, []).append(4 * k)
        for t, offs in bytarget.items():
            cases = " ".join("case %du:" % o for o in offs)
            if t in starts:
                out.append("%s goto L_%08x;" % (cases, t))
                labels.add(t)
            else:
                out.append("%s %s" % (cases, " ".join(self.tail(self.callee(t), land))))
        out.append("default: %s" % " ".join(self.tail_ind("sg_ld32(mem, %s)" % e, land)))
        out.append("}")
        return out

    def call_lines(self, insn):
        ra = insn.address + insn.size
        op = insn.operands[0]
        if op.type == X.X86_OP_IMM:
            t = op.imm & 0xFFFFFFFF
            if t == ra:
                return ["SG_PUSH32(%s); /* call $+5 */" % h(ra)]
            out = ["SG_CALL(%s, %s);" % (self.callee(t), h(ra))]
        else:
            sym = self.imp_at(op)
            if sym:
                out = ["SG_CALL(%s, %s);" % (sym, h(ra))]
            else:
                out = ["SG_CALL_IND(%s, %s);" % (self.target_expr(insn, op), h(ra))]
        if self.flags_through_calls:
            out.append("lf = ctx->fl;")
        return out

    def insn_lines(self, fn, insn):
        self.stats["insns"] += 1
        head = "/* %08x: %s %s */" % (insn.address, insn.mnemonic, insn.op_str)
        mn = insn.mnemonic.split()[-1]
        try:
            if mn == "call":
                lines = self.call_lines(insn)
                if self.memorder is not None:
                    lines = self.memorder.apply(insn, lines)
                return [head] + lines
            if mn == "jmp" or (mn[0] == "j" and mn != "jmp") or mn == "ret":
                raise Unhandled("control transfer inside a block")
            lines = self.lifter.lower(insn)
            if self.memorder is not None:
                lines = self.memorder.apply(insn, lines)
        except Unhandled as e:
            self.note_unhandled(fn, insn.address, "%s %s" % (insn.mnemonic, insn.op_str), str(e))
            return [head, "sg_unhandled_insn(ctx, %s);" % h(insn.address)]
        if not lines:
            return [head]
        if len(lines) == 1:
            return [head, "{ %s }" % lines[0]]
        return [head, "{"] + ["    " + x for x in lines] + ["}"]

    def prologue(self):
        """Declarations for everything referenced but not defined in this translation unit."""
        out = []
        for t in sorted(self.refs_f):
            out.append("SG_DECL(%08x);" % t)
        for s in sorted(self.refs_imp):
            out.append(DECL_IMPORT % s)
        return out
