"""Fixture tests for the thread tools (scan.py, threads.py, tgraph.py). Uses only instruction text and block graphs
written here; no game bytes are read or executed.

Usage:   PYTHONPATH=tools/.deps python3 -P tools/threads/test_threads.py
When:    after changing scan.py, threads.py or tgraph.py.
"""
import collections
import io
import os
import sys

HERE = os.path.dirname(os.path.abspath(__file__))
sys.path.insert(0, HERE)
sys.path.insert(0, os.path.join(HERE, "..", "code-characteristics"))
sys.path.insert(0, os.path.join(HERE, "..", "manifest"))
import scan  # noqa: E402
import threads  # noqa: E402

Block = collections.namedtuple("Block", "start end term succ shared")
fails = 0


def check(name, got, want):
    global fails
    ok = got == want
    if not ok:
        fails += 1
    print("%-48s %s" % (name, "ok" if ok else "FAIL got %r want %r" % (got, want)))


# --- operand parsing
check("parse [ecx + 0x10]", scan.parse_mem("ecx + 0x10"), ("ecx", None, 0x10))
check("parse [ebp - 8]", scan.parse_mem("ebp - 8"), ("ebp", None, 0xFFFFFFF8))
check("parse [eax*4 + 0x10b2414]", scan.parse_mem("eax*4 + 0x10b2414"), (None, "eax", 0x10B2414))
check("parse [ebx + edi*8 + 4]", scan.parse_mem("ebx + edi*8 + 4"), ("ebx", "edi", 4))
check("parse [0x16e6307]", scan.parse_mem("0x16e6307"), (None, None, 0x16E6307))

# --- atomic classification (our own instruction sequences)
S = scan.Scanner.__new__(scan.Scanner)
seq = [(0x10, 3, "or", "esi, 0xffffffff"), (0x13, 4, "lock xadd", "dword ptr [edx], esi"),
       (0x17, 1, "dec", "esi")]
cls, det = S.classify_atomic(seq, 1, "xadd", "dword ptr [edx], esi", "h", "edx", 0, None)
check("xadd -1 -> dec", cls, "dec/heap")
seq = [(0x10, 2, "xor", "ecx, ecx"), (0x12, 4, "lock xadd", "dword ptr [edx], ecx")]
cls, det = S.classify_atomic(seq, 1, "xadd", "dword ptr [edx], ecx", "h", "edx", 0, None)
check("xadd 0 -> refined atomic-load",
      threads.refine_atomic(dict(cls=cls, detail=det, mnem="xadd"), ""), "atomic-load (xadd 0)")
seq = [(0x10, 5, "mov", "edx, 1"), (0x15, 2, "xchg", "dword ptr [eax], edx")]
cls, det = S.classify_atomic(seq, 1, "xchg", "dword ptr [eax], edx", "h", "eax", 0, None)
check("xchg 1 after dec -> release guard",
      threads.refine_atomic(dict(cls=cls, detail=det, mnem="xchg"), "or esi, 0xffffffff ; lock xadd dword ptr [edx], esi"),
      "refcount release guard (store 1 after dec-to-zero)")
cls, det = S.classify_atomic([(0, 4, "lock or", "dword ptr [esp], 0")], 0, "or", "dword ptr [esp], 0", "s", "esp", 0, None)
check("lock or [esp],0 -> fence", cls, "fence(or)")

# --- poll-loop filter
spin = ["test edi, edi", "jle 0x5a", "dec edi", "pause", "jmp 0x6a", "mov edi, dword ptr [esi + 0xc]",
        "push 0x14093d8", "call 0x922080", "add esp, 4", "mov edx, dword ptr [esi + 8]", "cmp edx, ebx", "jne 0x51"]
check("pause spin loop is a poll", threads.is_poll_loop(spin, 1)[0], True)
flag = ["cmp byte ptr [eax + 0x10], 0", "je 0x100"]
check("plain flag wait is a poll", threads.is_poll_loop(flag, 0)[0], True)
walk = ["mov ecx, dword ptr [ecx + 0x28]", "test ecx, ecx", "jne 0x10"]
check("list walk is not a poll", threads.is_poll_loop(walk, 0)[0], False)
count = ["inc eax", "cmp eax, dword ptr [esi + 0x40c]", "jb 0x10"]
check("counted loop with reloaded bound is not", threads.is_poll_loop(count, 0)[0], False)
store = ["mov dword ptr [esi], 0", "cmp dword ptr [esi + 4], 0", "jne 0x10"]
check("loop that stores is not", threads.is_poll_loop(store, 0)[0], False)
rodata = ["comiss xmm0, dword ptr [0x14852b4]", "jbe 0x10"]
check(".rdata constant is not a poll", threads.is_poll_loop(rodata, 0)[0], False)
call = ["call 0x401000", "cmp byte ptr [0x16e6307], 0", "jne 0x10"]
check("loop with an arbitrary call is excluded", threads.is_poll_loop(call, 0)[0], False)

# --- cycle detection: a backward jump to shared exit code is not a loop
S.regs = []
blocks = [Block(0x100, 0x104, "ret", [], False),        # or eax,-1 ; ret  (shared exit)
          Block(0x104, 0x10a, "jcc", [0x100, 0x10a], False),  # cmp [ecx+0x34],0 ; jne 0x100
          Block(0x10a, 0x10c, "ret", [], False)]
insns = {0x100: [(0x100, 3, "or", "eax, 0xffffffff"), (0x103, 1, "ret", "")],
         0x104: [(0x104, 4, "cmp", "dword ptr [ecx + 0x34], 0"), (0x108, 2, "jne", "0x100")],
         0x10a: [(0x10a, 2, "xor", "eax, eax"), (0x10c, 1, "ret", "")]}
out = io.StringIO()
S.find_loops(0x100, None, blocks, insns, out)
check("backward jump to exit block is not a loop", out.getvalue(), "")
blocks = [Block(0x200, 0x206, "jcc", [0x200, 0x206], False),  # cmp [eax+0x10],0 ; je 0x200
          Block(0x206, 0x207, "ret", [], False)]
insns = {0x200: [(0x200, 4, "cmp", "byte ptr [eax + 0x10], 0"), (0x204, 2, "je", "0x200")],
         0x206: [(0x206, 1, "ret", "")]}
out = io.StringIO()
S.find_loops(0x200, None, blocks, insns, out)
check("self loop found", out.getvalue().split("\t")[0], "200")

# --- CHA closure over a synthetic graph
F = {}
for e, calls, voffs in ((1, (2,), ()), (2, (), (8,)), (3, (), ()), (4, (5,), ()), (5, (), (-1,))):
    f = threads.tgraph.Fn(e, 1, 0, len(voffs), 0)
    f.calls, f.voffs = calls, voffs
    F[e] = f
seen, unknown = threads.cha_closure(F, [1], {8: {4}, 0: {3}})
check("CHA closure follows slot 8", sorted(seen), [1, 2, 4, 5])
check("CHA closure counts unknown-offset sites", unknown, 1)

print("FAILURES:", fails)
sys.exit(1 if fails else 0)
