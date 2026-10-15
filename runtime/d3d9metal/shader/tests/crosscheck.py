"""Cross-check the C library (runtime/d3d9metal/shader, via d3d9msl_tool) against the Python reference
(tools/shaders/msl.py): the MSL text must be byte-identical and the reflection JSON equal.

    python3 -P crosscheck.py TOOL WORKDIR                         our fixtures, each with 4 option sets
    python3 -P crosscheck.py TOOL WORKDIR --corpus SMT_DIR[,SMT_DIR...] [--chunk K/N]   unique game shaders (git-ignored WORKDIR only)

Fixtures are our own assembly (tools/shaders/fixtures/*.asm plus the semantics test's edge-case shader and
the conformance test's shaders, runtime/d3d9metal/tests/shaders). Corpus blobs, MSL and logs stay under WORKDIR, which must be git-ignored (under build/, for example): the corpus output is derived from game data.
"""
import glob
import hashlib
import json
import os
import subprocess
import sys
import time

HERE = os.path.dirname(os.path.abspath(__file__))
ROOT = os.path.abspath(os.path.join(HERE, "..", "..", "..", ".."))
sys.path.insert(0, os.path.join(ROOT, "tools", "shaders"))
import d3dbc  # noqa: E402
import msl  # noqa: E402

FOG, FLAT, PLAIN = 1, 2, 4


def opts_dict(flags):
    o = {}
    if flags & FOG:
        o["fog"] = "vertex"
    if flags & FLAT:
        o["flat"] = True
    if flags & PLAIN:
        o["plain"] = True
    return o


def fixture_sources():
    out = []
    for f in sorted(glob.glob(os.path.join(ROOT, "tools", "shaders", "fixtures", "*.asm"))):
        out.append((os.path.splitext(os.path.basename(f))[0], open(f).read()))
    for f in sorted(glob.glob(os.path.join(ROOT, "runtime", "d3d9metal", "tests", "shaders", "*.asm"))):
        out.append(("conformance_" + os.path.splitext(os.path.basename(f))[0], open(f).read()))
    import test_semantics  # noqa: E402  (its SPECIAL edge-case shader)
    out.append(("vs_special", test_semantics.SPECIAL))
    return out


def run_batch(tool, work, items):
    """items: (name, tokens bytes, entry, flags). Returns {name: (ok, msl or err, refl or None)}."""
    blobs = os.path.join(work, "blobs")
    outd = os.path.join(work, "c_out")
    os.makedirs(blobs, exist_ok=True)
    os.makedirs(outd, exist_ok=True)
    lst = os.path.join(work, "list.txt")
    with open(lst, "w") as f:
        for name, b, entry, flags in items:
            p = os.path.join(blobs, name + ".bin")
            if not os.path.exists(p):
                open(p, "wb").write(b)
            f.write(f"{p} {name} {entry} {flags}\n")
    for ext in ("metal", "json", "err"):
        for p in glob.glob(os.path.join(outd, f"*.{ext}")):
            os.unlink(p)
    r = subprocess.run([tool, "--batch", lst, outd], capture_output=True, text=True, timeout=540)
    print(r.stdout.strip())
    res = {}
    for name, _, _, _ in items:
        e = os.path.join(outd, name + ".err")
        if os.path.exists(e):
            res[name] = (False, open(e).read().strip(), None)
        else:
            res[name] = (True, open(os.path.join(outd, name + ".metal")).read(),
                         json.load(open(os.path.join(outd, name + ".json"))))
    return res


def compare(items, cres, py):
    bad = 0
    for name, b, entry, flags in items:
        ok, text, refl = cres[name]
        pok, ptext, prefl = py[name]
        if ok != pok:
            bad += 1
            print(f"DIFF {name}: C {'ok' if ok else 'failed: ' + text}, Python {'ok' if pok else 'failed: ' + ptext}")
            continue
        if not ok:
            continue
        if text != ptext:
            bad += 1
            a, c = ptext.splitlines(), text.splitlines()
            k = next((i for i in range(min(len(a), len(c))) if a[i] != c[i]), min(len(a), len(c)))
            print(f"DIFF {name}: MSL differs at line {k + 1}\n  py: {a[k] if k < len(a) else '<eof>'}\n"
                  f"  C:  {c[k] if k < len(c) else '<eof>'}")
            continue
        if refl != json.loads(json.dumps(prefl)):
            bad += 1
            ks = sorted(set(refl) | set(prefl))
            d = [k for k in ks if refl.get(k) != json.loads(json.dumps(prefl.get(k)))]
            print(f"DIFF {name}: reflection differs in {d}: C {[refl.get(k) for k in d]} py {[prefl.get(k) for k in d]}")
    return bad


def py_translate(b, entry, flags):
    try:
        sh = d3dbc.parse(b)
        src, refl = msl.translate(sh, entry=entry, options=opts_dict(flags))
        return True, src, refl
    except (msl.Unsupported, d3dbc.ParseError) as e:
        return False, str(e), None


def main(argv):
    tool, work = argv[1], os.path.abspath(argv[2])
    os.makedirs(work, exist_ok=True)
    items = []
    if "--corpus" in argv:
        import smt  # noqa: E402
        smt_dir = argv[argv.index("--corpus") + 1]
        k, n = (1, 1)
        if "--chunk" in argv:
            k, n = map(int, argv[argv.index("--chunk") + 1].split("/"))
        seen = set()
        uniq = []
        files = [f for d in smt_dir.split(",") for f in sorted(glob.glob(os.path.join(d, "*.0469a3f7")))]
        for f in files:
            s = smt.parse(open(f, "rb").read())
            for _, _, b in smt.blobs(s):
                h = hashlib.sha1(b).hexdigest()[:16]
                if h not in seen:
                    seen.add(h)
                    uniq.append((h, bytes(b)))
        uniq.sort()
        lo, hi = len(uniq) * (k - 1) // n, len(uniq) * k // n
        for h, b in uniq[lo:hi]:
            items.append((h, b, f"sg_{h}", 0))
        print(f"corpus: {len(uniq)} unique blobs, chunk {k}/{n}: {len(items)}")
    else:
        for name, text in fixture_sources():
            toks = d3dbc.assemble(text)
            b = b"".join(t.to_bytes(4, "little") for t in toks)
            for flags in (0, FOG, FLAT, FOG | FLAT, PLAIN):
                items.append((f"{name}.o{flags}", b, name, flags))
    t0 = time.time()
    py = {name: py_translate(b, entry, flags) for name, b, entry, flags in items}
    t1 = time.time()
    cres = run_batch(tool, work, items)
    t2 = time.time()
    bad = compare(items, cres, py)
    nok = sum(1 for v in py.values() if v[0])
    print(f"{len(items)} translations compared ({nok} translated by both, {len(items) - nok} refused by both "
          f"with matching outcome): {len(items) - bad} identical, {bad} differ. Python {t1 - t0:.1f} s, C batch {t2 - t1:.1f} s")
    fails = {}
    for name, (ok, text, _) in cres.items():
        if not ok:
            fails[text] = fails.get(text, 0) + 1
    for msg, c in sorted(fails.items(), key=lambda x: -x[1]):
        print(f"  refused x{c}: {msg}")
    return 1 if bad else 0


if __name__ == "__main__":
    sys.exit(main(sys.argv))
