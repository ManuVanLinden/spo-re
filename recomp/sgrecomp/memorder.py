"""Memory-ordering policy for translated guest accesses (docs/design/memory-ordering.md).

x86 gives every plain load acquire and every plain store release ordering (TSO); arm64 does not.
This module decides, per instruction, whether its explicit memory operand is lowered as an
ordered access, and rewrites the lifter's C for that instruction accordingly:

  sg_ldN(mem, a)      -> sg_ldN_acq_u(mem, a)      (ldapr; misaligned: ldr + dmb ishld)
  sg_stN(mem, a, v)   -> sg_stN_rel_u(mem, a, v)   (stlr;  misaligned: dmb ish + str)
  sgr_ldf32/sgr_ldx*  -> their _acq forms; sgr_stf32 -> sgr_stf32_rel
  helpers that touch memory internally (string ops, 80-bit x87, fxsave) are bracketed by a
  release fence before and an acquire fence after.

Policies (translate.py --memorder, or SG_MEMORDER; the default is ordering-sites.json's
"default_policy"):

  p0  only lock/xchg are ordered (sequentially consistent); the emission before ordered accesses existed
  p1  p0 + the listed sites of data/threads/ordering-sites.json (spinlock spin loads, the Dac
      hand-off, stop flags, publication stores)
  p2  p1 + every .data/.tls/CONST global access image-wide + every heap access in the
      functions directly reachable from a worker thread (data/threads/ordering-sites.json worker_functions)
  p3  p1 + every global and heap access anywhere: x86-TSO for all non-stack data

Operand classes follow the thread scan (tools/threads/scan.py) so the counts are comparable with
data/threads/policy.json: stack = esp-based, or ebp-based in a function whose entry block has
`mov ebp, esp`; global = no base register and an absolute address in .data/.tls/CONST;
read-only = no base register, any other section; fs = TEB; heap = anything else. Stack,
read-only and fs accesses are never ordered by a class rule (a listed site always is).
"""
import collections
import json
import os
import re

from capstone import x86_const as X

ROOT = os.path.abspath(os.path.join(os.path.dirname(__file__), "..", ".."))
WORK = os.environ.get("SPORE_WORK_DIR") or ROOT  # generated data moves with it (docs/design/convert.md)
SITES = os.path.join(WORK, "data", "threads", "ordering-sites.json")
POLICIES = ("p0", "p1", "p2", "p3")

# lifter helper -> ordered helper. Loads become acquire loads, stores release stores.
LOAD_MAP = {"sg_ld8": "sg_ld8_acq_u", "sg_ld16": "sg_ld16_acq_u", "sg_ld32": "sg_ld32_acq_u",
            "sg_ld64": "sg_ld64_acq_u", "sg_ldf32": "sg_ldf32_acq_u", "sg_ldf64": "sg_ldf64_acq_u",
            "sg_ld128": "sg_ld128_acq_u", "sgr_ldf32": "sgr_ldf32_acq", "sgr_ldx": "sgr_ldx_acq",
            "sgr_ldx32": "sgr_ldx32_acq", "sgr_ldx64": "sgr_ldx64_acq"}
STORE_MAP = {"sg_st8": "sg_st8_rel_u", "sg_st16": "sg_st16_rel_u", "sg_st32": "sg_st32_rel_u",
             "sg_st64": "sg_st64_rel_u", "sg_stf32": "sg_stf32_rel_u", "sg_stf64": "sg_stf64_rel_u",
             "sg_st128": "sg_st128_rel_u", "sgr_stf32": "sgr_stf32_rel"}
FENCED = {"sgr_rep_movs", "sgr_rep_stos", "sgr_repe_cmps", "sg_ldf80", "sg_stf80", "sg_fxsave"}
ATOMIC = {"sg_lock_xadd32", "sg_xchg32", "sg_lock_cmpxchg32", "sg_lock_cmpxchg64"}
CALL_RE = re.compile(r"\b(sgr?_[a-z0-9_]+)\((?:ctx, )?mem\b")
GLOBAL_SECS = (".data", ".tls", "CONST")


class SiteError(Exception):
    pass


def load_sites(path=SITES):
    if not os.path.exists(path):
        return {"default_policy": "p0", "sites": [], "worker_functions": []}
    return json.load(open(path))


def resolve_policy(arg=None, data=None):
    p = (arg or os.environ.get("SG_MEMORDER") or (data or {}).get("default_policy") or "p0").lower()
    if p not in POLICIES:
        raise SystemExit("unknown memory-ordering policy %r (one of %s)" % (p, ", ".join(POLICIES)))
    return p


class MemOrder:
    """Per-instruction ordering decisions and the C rewrite. One instance per translation run."""

    def __init__(self, image, policy="p0", data=None, check_sites=True):
        self.image = image
        self.policy = policy
        data = data if data is not None else {}
        self.sites = {}
        for s in data.get("sites", []):
            self.sites[int(s["addr"], 16)] = s
        self.site_func = {int(s["addr"], 16): int(s["func"], 16) for s in data.get("sites", [])}
        self.workers = {int(x, 16) for x in data.get("worker_functions", [])}
        self.check_sites = check_sites
        self.seen_sites = {}  # addr -> problem string or "" when lowered as listed
        self.secs = [(n, va, max(vsz, rsz)) for n, va, vsz, _raw, rsz in getattr(image, "secs", [])]
        self.frame = False
        self.func = None
        # counts in the thread scan's units: one per memory operand access, a read-modify-write counts twice
        self.count = collections.Counter()
        self.lines_rewritten = 0

    # ------------------------------------------------------------------ classification --
    def begin_function(self, fn):
        self.func = fn.entry
        self.frame = False
        first = next((b for b in fn.blocks if b.start == fn.entry), None)
        if first is not None:
            for i in first.insns[:6]:
                if i is not None and i.mnemonic == "mov" and i.op_str == "ebp, esp":
                    self.frame = True

    def secof(self, va):
        for n, sva, sz in self.secs:
            if sva <= va < sva + sz:
                return n
        return None

    def op_class(self, op):
        m = op.mem
        if m.segment == X.X86_REG_FS:
            return "fs"
        if m.base in (X.X86_REG_ESP, X.X86_REG_SP) or (self.frame and m.base == X.X86_REG_EBP):
            return "stack"
        d = m.disp & 0xFFFFFFFF
        if not m.base and d >= 0x400000:
            sec = self.secof(d)
            if sec is None:
                return "heap"
            return "global" if sec in GLOBAL_SECS else "readonly"
        return "heap"

    @staticmethod
    def mem_ops(insn):
        """Explicit memory operands with their access (1 read, 2 write, 3 both). `lea` and
        `nop` name an address without accessing it."""
        mn = insn.mnemonic.split()[-1]
        if mn in ("lea", "nop") or mn.startswith("prefetch"):
            return []
        out = []
        for op in insn.operands:
            if op.type == X.X86_OP_MEM:
                acc = op.access or 1
                out.append((op, acc))
        return out

    # ------------------------------------------------------------------ decision --
    def decide(self, insn):
        """(acquire, release, reason) for this instruction's memory operand(s)."""
        ops = self.mem_ops(insn)
        if not ops:
            return False, False, None
        locked = insn.mnemonic.startswith("lock ") or insn.mnemonic.split()[-1] == "xchg"
        classes = [self.op_class(op) for op, _a in ops]
        acc = 0
        for _op, a in ops:
            acc |= a
        for (op, a), c in zip(ops, classes):
            n = (1 if a & 1 else 0) + (1 if a & 2 else 0)
            self.count["access:" + c] += n
            if locked:
                self.count["ordered:atomic"] += n
        if locked:
            return False, False, "atomic"
        site = self.sites.get(insn.address)
        if site is not None and self.policy != "p0":
            kind = site["kind"]
            want_r = kind in ("acquire", "acq_rel")
            want_w = kind in ("release", "acq_rel")
            prob = ""
            if want_r and not acc & 1:
                prob = "listed as %s but the instruction does not load" % kind
            if want_w and not acc & 2:
                prob = "listed as %s but the instruction does not store" % kind
            self.seen_sites[insn.address] = prob
            for (op, a), c in zip(ops, classes):
                self.count["ordered:site"] += (1 if a & 1 else 0) + (1 if a & 2 else 0)
            # a listed site orders both directions it performs: the list says which one matters
            return bool(acc & 1), bool(acc & 2), "site"
        reason = None
        if self.policy == "p3":
            if any(c in ("heap", "global") for c in classes):
                reason = "p3"
        elif self.policy == "p2":
            if any(c == "global" for c in classes):
                reason = "global"
            elif self.func in self.workers and any(c == "heap" for c in classes):
                reason = "worker-heap"
        if reason is None:
            return False, False, None
        for (op, a), c in zip(ops, classes):
            if c in ("heap", "global"):
                self.count["ordered:" + c] += (1 if a & 1 else 0) + (1 if a & 2 else 0)
        return bool(acc & 1), bool(acc & 2), reason

    # ------------------------------------------------------------------ rewrite --
    def rewrite(self, lines):
        """Rewrite one instruction's C lines to ordered helpers: every guest load in them becomes
        an acquire load and every guest store a release store, whatever capstone reports as the
        operand's access (an ordered instruction has only the one ordered operand; implicit stack
        pushes and pops go through the SG_PUSH32/SG_POP32 macros and stay relaxed)."""
        fenced = False
        out = []
        for ln in lines:
            def sub(m):
                nonlocal fenced
                name = m.group(1)
                if name in LOAD_MAP:
                    return m.group(0).replace(name, LOAD_MAP[name], 1)
                if name in STORE_MAP:
                    return m.group(0).replace(name, STORE_MAP[name], 1)
                if name in FENCED or (name not in ATOMIC and name not in LOAD_MAP and name not in STORE_MAP):
                    fenced = True
                return m.group(0)
            nl = CALL_RE.sub(sub, ln)
            if nl != ln:
                self.lines_rewritten += 1
            out.append(nl)
        if fenced:
            out = ["sg_fence_rel();"] + out + ["sg_fence_acq();"]
        return out

    def apply(self, insn, lines):
        acq, rel, _why = self.decide(insn)
        if not (acq or rel):
            return lines
        return self.rewrite(lines)

    # ------------------------------------------------------------------ audit --
    def verify_sites(self, selected_functions):
        """Fail loudly when a listed site in a translated function was not found as an
        instruction with a memory operand of the listed direction."""
        if self.policy == "p0" or not self.check_sites:
            return []
        bad = []
        for a, s in sorted(self.sites.items()):
            f = self.site_func[a]
            if f not in selected_functions:
                continue
            if a not in self.seen_sites:
                bad.append("%#x (%s, function %#x): not found as a memory-accessing instruction" % (a, s["what"], f))
            elif self.seen_sites[a]:
                bad.append("%#x (%s): %s" % (a, s["what"], self.seen_sites[a]))
        if bad:
            raise SiteError("ordering-sites.json: %d listed site(s) not lowered as listed:\n  " % len(bad) +
                            "\n  ".join(bad[:50]))
        return bad

    def summary(self):
        c = self.count
        ordered = sum(v for k, v in c.items() if k.startswith("ordered:"))
        total = sum(v for k, v in c.items() if k.startswith("access:"))
        return {"policy": self.policy, "accesses": total, "ordered": ordered,
                "ordered_excluding_atomics": ordered - c["ordered:atomic"],
                "by_class": {k: v for k, v in sorted(c.items())},
                "sites_listed": len(self.sites), "sites_lowered": sum(1 for v in self.seen_sites.values() if not v),
                "c_lines_rewritten": self.lines_rewritten}
