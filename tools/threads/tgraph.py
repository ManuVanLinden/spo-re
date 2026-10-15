"""Loader for scratch/threads/<tag>/funcs.txt.gz (written by scan.py) plus call-graph helpers.

Usage:   a library, imported by threads.py and recomp/memorder_sites.py."""
import collections
import gzip
import os

ROOT = os.path.abspath(os.path.join(os.path.dirname(__file__), "..", ".."))
WORK = os.environ.get("SPORE_WORK_DIR") or ROOT  # docs/design/convert.md


class Fn:
    __slots__ = ("entry", "ninsn", "frame", "icall", "ijmp", "calls", "imports", "ptrs", "mem", "globs", "voffs")

    def __init__(self, entry, ninsn, frame, icall, ijmp):
        self.entry, self.ninsn, self.frame, self.icall, self.ijmp = entry, ninsn, frame, icall, ijmp
        self.calls = ()
        self.imports = ()
        self.ptrs = ()
        self.mem = {}
        self.globs = {}
        self.voffs = ()


def load(tag="ep1"):
    fns = {}
    cur = None
    with gzip.open(os.path.join(WORK, "scratch", "threads", tag, "funcs.txt.gz"), "rt") as f:
        for line in f:
            k, _, rest = line.rstrip("\n").partition(" ")
            if k == "F":
                p = rest.split()
                cur = Fn(int(p[0], 16), *map(int, p[1:]))
                fns[cur.entry] = cur
            elif k == "C":
                cur.calls = tuple(int(x, 16) for x in rest.split())
            elif k == "I":
                cur.imports = tuple(rest.split())
            elif k == "P":
                cur.ptrs = tuple(int(x, 16) for x in rest.split())
            elif k == "V":
                cur.voffs = tuple(int(x) for x in rest.split())
            elif k == "M":
                cur.mem = {a: int(b) for a, b in (x.split("=") for x in rest.split())}
            elif k == "G":
                g = {}
                for x in rest.split():
                    rw = x[-1]
                    key = x[:-1]
                    idx = key.endswith("+")
                    g[(int(key.rstrip("+"), 16), idx)] = rw
                cur.globs = g
    return fns


def callers(fns):
    r = collections.defaultdict(set)
    for e, f in fns.items():
        for c in f.calls:
            r[c].add(e)
    return r


def closure(fns, roots, stop=(), follow_ptrs=False):
    """Functions reachable from roots through direct calls/tail jumps (optionally address-taken pointers)."""
    seen = set()
    todo = [r for r in roots if r in fns]
    stop = set(stop)
    while todo:
        e = todo.pop()
        if e in seen:
            continue
        seen.add(e)
        if e in stop:
            continue
        f = fns[e]
        for c in f.calls:
            if c in fns and c not in seen:
                todo.append(c)
        if follow_ptrs:
            for c in f.ptrs:
                if c in fns and c not in seen:
                    todo.append(c)
    return seen
