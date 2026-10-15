"""Instruction-form classification for the instruction-form census (pure functions, no binary access).

form_of(mnemonic, op_str, reloc, img) turns a Capstone (mnemonic, op_str) pair into our
normalised form string, e.g. ("mov", "eax, dword ptr [ecx + 4]") -> "mov r32,m32".
reloc says whether the instruction's bytes carry a base relocation, img is the image
(lo, hi) range used to tell relocated pointers from plain constants.

Usage:   a library, imported by recomp/translate.py (the instruction form of each lowered instruction).
"""
import re

IMG = (0x400000, 0x2400000)

R8 = set("al cl dl bl ah ch dh bh".split())
R16 = set("ax cx dx bx sp bp si di".split())
R32 = set("eax ecx edx ebx esp ebp esi edi".split())
SREG = set("cs ds es fs gs ss".split())
SIZES = {"byte": "m8", "word": "m16", "dword": "m32", "qword": "m64", "xword": "m80", "tbyte": "m80",
         "xmmword": "m128", "ymmword": "m256"}
R_MEM = re.compile(r"^(?:(\w+) ptr )?(?:([cdefgs]s):)?\[(.*)\]$")
BRANCH = {"call", "jmp", "loop", "loope", "loopne", "jecxz", "jcxz"}
SHIFTS = {"shl", "shr", "sar", "sal", "rol", "ror", "rcl", "rcr", "shld", "shrd"}
LEGACY_PREFIX = {0xF0: "lock", 0xF2: "f2", 0xF3: "f3", 0x2E: "cs", 0x36: "ss", 0x3E: "ds", 0x26: "es",
                 0x64: "fs", 0x65: "gs", 0x66: "66", 0x67: "67"}

THREEDNOW = {"femms", "pavgusb", "pmulhrw", "pfadd", "pfsub", "pfmul", "pfrcp", "pfrsqrt", "pf2id", "pi2fd",
             "pfcmpeq", "pfcmpge", "pfcmpgt", "pfmax", "pfmin", "pfacc", "pfsubr", "pfrcpit1", "pfrcpit2",
             "pfrsqit1", "pfnacc", "pfpnacc", "pswapd", "pi2fw", "pf2iw", "prefetch", "prefetchw"}
STRING = {"movsb", "movsw", "movsd", "stosb", "stosw", "stosd", "lodsb", "lodsw", "lodsd", "cmpsb", "cmpsw",
          "cmpsd", "scasb", "scasw", "scasd", "insb", "insd", "outsb", "outsd"}
SYSTEM = {"cpuid", "rdtsc", "rdtscp", "rdpmc", "hlt", "int", "int1", "int3", "into", "ud2", "in", "out", "cli", "sti",
          "lgdt", "sgdt", "lidt", "sidt", "sldt", "str", "lmsw", "smsw", "invd", "wbinvd", "invlpg", "rdmsr",
          "wrmsr", "sysenter", "sysexit", "syscall", "iretd", "iret", "arpl", "lar", "lsl", "verr", "verw",
          "bound", "fxsave", "fxrstor", "lds", "les", "lfs", "lgs", "lss", "ljmp", "lcall", "retf", "pause",
          "xgetbv"}
FLAGS = {"pushfd", "popfd", "pushf", "popf", "lahf", "sahf", "clc", "stc", "cmc", "cld", "std"}
X87_INT = {"fnstsw", "fstsw", "fnstcw", "fstcw", "fldcw", "fnclex", "fninit", "fwait", "wait", "fnsave", "frstor",
           "fnstenv", "fldenv"}
SSE_MISC = {"ldmxcsr", "stmxcsr", "sfence", "lfence", "mfence", "movnti", "prefetchnta", "prefetcht0",
            "prefetcht1", "prefetcht2", "maskmovq", "movntq", "emms"}
CTRL = {"call", "jmp", "ret", "retf", "loop", "loope", "loopne", "jecxz", "jcxz", "enter", "leave"}


def category(base, ops):
    if base in THREEDNOW:
        return "3dnow"
    if base in X87_INT:
        return "x87-control"
    if base[0] == "f" and base not in ("fxsave", "fxrstor"):
        return "x87"
    if base in ("pushal", "popal", "pushad", "popad", "enter", "leave"):
        return "stack-special"
    if base in SYSTEM:
        return "system"
    if base in ("fxsave", "fxrstor"):
        return "system"
    if base.startswith("cvt") or "xmm" in ops or base in ("ldmxcsr", "stmxcsr", "sfence", "lfence", "mfence", "movnti", "prefetchnta",
                                "prefetcht0", "prefetcht1", "prefetcht2"):
        return "sse"
    if re.search(r"\bmm[0-7]\b", ops) or base in ("emms", "maskmovq", "movntq"):
        return "mmx"
    if base in STRING:
        return "string"
    if base in CTRL or base[0] == "j":
        return "control"
    if base in FLAGS:
        return "flags"
    if base in ("xchg", "xadd", "cmpxchg", "cmpxchg8b") or base.startswith("lock"):
        return "atomic-candidate"
    return "integer"


def opform(base, op, last, reloc, img=IMG):
    """Return (form token, addressing-mode or None)."""
    if op in R32:
        return "r32", None
    if op in R8:
        if op == "cl" and last and base in SHIFTS:
            return "cl", None
        return "r8", None
    if op in R16:
        return "r16", None
    if op.startswith("xmm"):
        return "xmm", None
    if op.startswith("mm"):
        return "mm", None
    if op == "st(0)":
        return "st0", None
    if op.startswith("st("):
        return "sti", None
    if op in SREG:
        return "sreg", None
    m = R_MEM.match(op)
    if m:
        sz, seg, inner = m.groups()
        tok = SIZES.get(sz, "m?" + sz) if sz else "m"
        if seg:
            tok = seg + ":" + tok
        parts = [x.strip() for x in re.split(r" ([+-]) ", inner)]
        mode = []
        nreg = 0
        for x in parts:
            if x in ("+", "-"):
                continue
            if "*" in x:
                mode.append("i*" + x.split("*")[1])
            elif x in R32 or x in R16:
                nreg += 1
                if nreg == 2:  # second register is an index with scale 1
                    mode.append("i*1")
                else:
                    mode.append("b" if x not in ("esp", "ebp") else x)
            else:
                v = int(x, 0)
                mode.append("A" if (reloc and img[0] <= v < img[1]) else "d")
        mode = "+".join(mode)
        if mode == "A":
            mode = "abs"
        if seg:
            mode = seg + ":" + mode
        return tok, mode
    if base in BRANCH or base[0] == "j":
        return "rel", None
    try:
        v = int(op, 0)
    except ValueError:
        return "?" + op, None
    if reloc and img[0] <= (v & 0xFFFFFFFF) < img[1]:
        return "addr", None
    return "imm", None


def form_of(m, ops, reloc, img=IMG):
    """Return (form, base mnemonic, addressing modes, explicit-esp-write form or None)."""
    parts = m.split()
    base = parts[-1]
    key_ops = []
    amodes = []
    if ops:
        opl = ops.split(", ")
        for i, o in enumerate(opl):
            t, am = opform(base, o, i == len(opl) - 1, reloc, img)
            key_ops.append(t)
            if am:
                amodes.append(am)
    form = m + (" " + ",".join(key_ops) if key_ops else "")
    if base in ("call", "jmp") and amodes:
        # indirect branch through memory: the addressing mode decides how it is lowered
        # (abs = IAT/global pointer, i*4+A = switch table, b+d = vtable/struct slot)
        form += "[" + amodes[0] + "]"
    espw = None
    if base not in ("push", "pop", "call", "ret") and ops.startswith("esp,"):
        espw = m + " esp," + ",".join(key_ops[1:])
    return form, base, amodes, espw
