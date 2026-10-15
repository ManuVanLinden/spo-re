"""Unit tests for forms.py on hand-written encodings (our own bytes, decoded only, never executed).
Usage:   PYTHONPATH=tools/.deps python3 -P tools/insn-forms/test_forms.py
When:    after changing forms.py."""
import os
import sys
import capstone
sys.path.insert(0, os.path.dirname(os.path.abspath(__file__)))
from forms import category, form_of  # noqa: E402

md = capstone.Cs(capstone.CS_ARCH_X86, capstone.CS_MODE_32)
# (hex bytes, reloc flag, expected form, expected addressing modes, expected category)
CASES = [
    ("8b4104", False, "mov r32,m32", ["b+d"], "integer"),
    ("ff5004", False, "call m32[b+d]", ["b+d"], "control"),
    ("ffd0", False, "call r32", [], "control"),
    ("ff2485" "00104000", True, "jmp m32[i*4+A]", ["i*4+A"], "control"),
    ("ff15" "00104000", True, "call m32[abs]", ["abs"], "control"),
    ("8b1500000000", False, "mov r32,m32", ["d"], "integer"),
    ("68" "00104000", True, "push addr", [], "integer"),
    ("6a10", False, "push imm", [], "integer"),
    ("f3a5", False, "rep movsd es:m32,m32", ["es:b", "b"], "string"),
    ("f00fc10e", False, "lock xadd m32,r32", ["b"], "atomic-candidate"),
    ("d3e0", False, "shl r32,cl", [], "integer"),
    ("88c8", False, "mov r8,r8", [], "integer"),
    ("dd442404", False, "fld m64", ["esp+d"], "x87"),
    ("d9c9", False, "fxch sti", [], "x87"),
    ("dfe0", False, "fnstsw r16", [], "x87-control"),
    ("0fae0424", False, "fxsave m", ["esp"], "system"),
    ("60", False, "pushal", [], "stack-special"),
    ("f00fc70e", False, "lock cmpxchg8b m64", ["b"], "atomic-candidate"),
    ("64a100000000", False, "mov r32,fs:m32", ["fs:d"], "integer"),
    ("f30f1000", False, "movss xmm,m32", ["b"], "sse"),
    ("f30f2c00", False, "cvttss2si r32,m32", ["b"], "sse"),
    ("0f6f00", False, "movq mm,m64", ["b"], "mmx"),
    ("8b0408", False, "mov r32,m32", ["b+i*1"], "integer"),
    ("83c410", False, "add r32,imm", [], "integer"),
    ("67e310", False, "jcxz rel", [], "control"),
    ("8c16", False, "mov m16,sreg", ["b"], "integer"),
    ("db2c24", False, "fld m80", ["esp"], "x87"),
]
fails = 0
for hx, reloc, want, want_modes, want_cat in CASES:
    (a, n, m, ops), = list(md.disasm_lite(bytes.fromhex(hx), 0x1000))
    form, base, modes, _ = form_of(m, ops, reloc)
    cat = category(base, ops)
    ok = form == want and modes == want_modes and cat == want_cat
    if not ok:
        fails += 1
        print("FAIL", hx, m, ops, "->", form, modes, cat, "want", want, want_modes, want_cat)
_, _, _, espw = form_of("add", "esp, 0x10", False)
if espw != "add esp,imm":
    fails += 1
    print("FAIL esp write", espw)
print("%d/%d passed" % (len(CASES) + 1 - fails, len(CASES) + 1))
sys.exit(1 if fails else 0)
