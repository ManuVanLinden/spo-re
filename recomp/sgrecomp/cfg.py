"""Per-function control-flow graph by recursive descent.

Mirrors the function manifest's rules (tools/manifest/build.py): a function owns every
block reachable from its entry without passing through another function's entry. Calls fall
through unless known not to return; jumps to another entry are tail calls; `jmp [i*4+T]`
dispatches through a switch table supplied by the caller (the manifest's T records for the game).

Extra roots (EH resume points, data/eh/resume-points.json) are descended from as well, so their
blocks are emitted inside the landing owner even when no CFG edge reaches them (they
are entered when a catch handler or a longjmp resumes there, not through a jump).
"""
import collections

import capstone
from capstone import x86_const as X

Block = collections.namedtuple("Block", "start end insns term succ")
# term: fall | ret | jmp | jcc | table | jmpind | noret | trap | invalid | fallout
# succ: fall/jmp: [target]; jcc: [taken, fallthrough]; table: [slot targets]; fallout: [entry]

_md = None


def disasm():
    global _md
    if _md is None:
        _md = capstone.Cs(capstone.CS_ARCH_X86, capstone.CS_MODE_32)
        _md.detail = True
    return _md


class CFGError(Exception):
    pass


def decode(image, va):
    b = image.read(va, 16)
    for i in disasm().disasm(b, va, 1):
        return i
    return None


def branch_target(insn):
    if insn.operands and insn.operands[0].type == X.X86_OP_IMM:
        return insn.operands[0].imm & 0xFFFFFFFF
    return None


def is_jcc(mn):
    return mn[0] == "j" and mn != "jmp"


class Func:
    def __init__(self, entry, blocks, insns, roots):
        self.entry = entry
        self.blocks = blocks  # sorted list of Block
        self.insns = insns
        self.roots = roots


def build(image, entry, roots=(), entries=frozenset(), tables=None, noret=None, noret_targets=frozenset()):
    """entries: all function entries (jumps to them, other than `entry`, are tail calls).
    tables: {jmp_va: [slot targets]}; noret: set of call-instruction VAs that do not return;
    noret_targets: call targets known not to return: function VAs (e.g. the jmp [IAT] thunk of
    _CxxThrowException) and ("iat", slot) for `call [slot]`."""
    tables = tables or {}
    noret = noret or set()
    insns = {}
    leaders = {entry} | set(roots)
    todo = [entry] + list(roots)
    term_of = {}
    while todo:
        va = todo.pop()
        while va not in insns:
            if va != entry and va in entries and va not in roots:
                break  # falls into another function: handled at block formation
            insn = decode(image, va)
            if insn is None:
                insns[va] = None
                term_of[va] = ("invalid", [])
                break
            insns[va] = insn
            mn = insn.mnemonic.split()[-1]
            nxt = va + insn.size
            if mn == "ret":
                term_of[va] = ("ret", [])
                break
            if mn == "jmp":
                t = branch_target(insn)
                if t is not None:
                    if t in entries and t != entry and t not in roots:
                        term_of[va] = ("jmp", [t])
                    else:
                        term_of[va] = ("jmp", [t])
                        leaders.add(t)
                        todo.append(t)
                elif va in tables:
                    ts = tables[va]
                    term_of[va] = ("table", list(ts))
                    for t in ts:
                        if not (t in entries and t != entry):
                            leaders.add(t)
                            todo.append(t)
                else:
                    term_of[va] = ("jmpind", [])
                break
            if is_jcc(mn):
                t = branch_target(insn)
                term_of[va] = ("jcc", [t, nxt])
                if not (t in entries and t != entry and t not in roots):
                    leaders.add(t)
                    todo.append(t)
                leaders.add(nxt)
                todo.append(nxt)
                break
            if mn == "call":
                t = branch_target(insn)
                if t is None and insn.operands[0].type == X.X86_OP_MEM:
                    m = insn.operands[0].mem
                    if not m.base and not m.index:
                        t = ("iat", m.disp & 0xFFFFFFFF)  # call [IAT]: matched against noret_targets
                if va in noret or (t is not None and t in noret_targets):
                    term_of[va] = ("noret", [])
                    break
            if mn in ("int3", "hlt", "ud2"):
                term_of[va] = ("trap", [])
                break
            va = nxt
    # blocks
    addrs = sorted(insns)
    blocks = []
    cur = []
    for k, va in enumerate(addrs):
        insn = insns[va]
        if cur:
            prev = insns[cur[-1]]
            pend = cur[-1] + (prev.size if prev else 1)
            if pend > va:
                raise CFGError("overlapping instructions at %#x / %#x in %#x" % (cur[-1], va, entry))
            if va in leaders or pend != va:
                blocks.append(_close(cur, insns, term_of, entries, entry, roots))
                cur = []
        cur.append(va)
        if va in term_of:
            blocks.append(_close(cur, insns, term_of, entries, entry, roots))
            cur = []
    if cur:
        blocks.append(_close(cur, insns, term_of, entries, entry, roots))
    return Func(entry, blocks, insns, list(roots))


def _close(vas, insns, term_of, entries, entry, roots):
    last = vas[-1]
    li = insns[last]
    end = last + (li.size if li else 1)
    if last in term_of:
        term, succ = term_of[last]
    else:
        term, succ = "fall", [end]
        if end in entries and end != entry and end not in roots:
            term = "fallout"
    return Block(vas[0], end, [insns[v] for v in vas], term, succ)
