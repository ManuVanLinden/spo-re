"""Loader for the function manifest written by tools/manifest/build.py (scratch/manifest/<tag>/manifest.txt.gz).
A library, imported by the eh, threads and link tools and by the translator.

    from manifest import load
    m = load("ep1")
    m.functions[0x401000]  -> Function(entry, srcbits, blocks=[Block...])
    m.tables[jmp_va]       -> JumpTable
    m.labels[va]           -> kind string (secondary entry / resume point)

Record format (one record per line, hex without 0x):
  F <entry> <srcbits> <nblocks> <ninsn>            a function; its blocks follow
  B <start> <end> <term> [succ ...]                a block owned by this function only
  S <start> <end> <term> [succ ...]                a block shared with other functions
  T <jmp> <table> <bytetable|-> <n> <how> [targets...]   a switch dispatch
  L <addr> <kind>                                  a secondary entry (label) inside a function
  D <start> <end> <kind>                           data inside .text (jumptable / bytetable / island)
  X <start> <end>                                  undecoded, unreferenced bytes in .text
term is one of: fall ret jmp jcc table jmpreg jmpmem call-noret trap invalid.
Successors: jcc = [taken, fallthrough]; fall/jmp = [target]; table = sorted distinct targets.
srcbits: 1 entry point, 2 direct call target, 4 pointer in data, 8 address-taken
immediate in .text, 16 Ghidra function (recorded; seeds only with --ghidra-seed), 32 EH unwind
funclet, 64 EH catch funclet, 128 unreferenced code (nothing points at it; outside the canonical
count), 256 shared tail reached by direct jmp from several functions (promoted to a function).
Canonical functions: srcbits & (1|2|4|8|32|64|256) != 0.
T: <bytetable> is the first byte the switch can index (after any bias such as `cmp idx-8, n`);
the dword table holds <n> targets in slot order (duplicates kept).
"""
import collections
import gzip
import os

ROOT = os.path.abspath(os.path.join(os.path.dirname(__file__), "..", ".."))
# SPORE_WORK_DIR (default: the source tree) takes the generated outputs; SPORE_GAME_DIR / SPORE_EXE name the
# Spore folder / the EP1 exe instead of game/ (docs/design/convert.md 4).
WORK = os.environ.get("SPORE_WORK_DIR") or ROOT
CANON = 1 | 2 | 4 | 8 | 32 | 64 | 256
Block = collections.namedtuple("Block", "start end term succ shared")
Function = collections.namedtuple("Function", "entry srcbits blocks ninsn")
JumpTable = collections.namedtuple("JumpTable", "jmp table bytetable n how targets")


class Manifest:
    def __init__(self):
        self.functions = {}
        self.tables = {}
        self.labels = {}
        self.data = []
        self.dead = []


def load(tag="ep1", path=None):
    path = path or os.path.join(WORK, "scratch", "manifest", tag, "manifest.txt.gz")
    m = Manifest()
    cur = None
    with gzip.open(path, "rt") as f:
        for line in f:
            if line[0] == "#":
                continue
            p = line.split()
            k = p[0]
            if k == "F":
                cur = Function(int(p[1], 16), int(p[2], 16), [], int(p[4]))
                m.functions[cur.entry] = cur
            elif k in "BS":
                cur.blocks.append(Block(int(p[1], 16), int(p[2], 16), p[3], [int(x, 16) for x in p[4:]], k == "S"))
            elif k == "T":
                m.tables[int(p[1], 16)] = JumpTable(int(p[1], 16), int(p[2], 16), None if p[3] == "-" else int(p[3], 16),
                                                    int(p[4]), p[5], [int(x, 16) for x in p[6:]])
            elif k == "L":
                m.labels[int(p[1], 16)] = p[2]
            elif k == "D":
                m.data.append((int(p[1], 16), int(p[2], 16), p[3]))
            elif k == "X":
                m.dead.append((int(p[1], 16), int(p[2], 16)))
    return m


if __name__ == "__main__":
    import sys
    m = load(sys.argv[1] if len(sys.argv) > 1 else "ep1")
    nb = sum(len(f.blocks) for f in m.functions.values())
    canon = sum(1 for f in m.functions.values() if f.srcbits & CANON)
    print(len(m.functions), "functions (", canon, "canonical )", nb, "blocks", len(m.tables), "tables",
          len(m.labels), "labels")
