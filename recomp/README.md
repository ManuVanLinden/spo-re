# recomp/: the translator (x86-32 → C)

A Python translator that reads `SporeApp.exe` with Capstone and emits C against
[`runtime/include/spore_guest.h`](../runtime/include/spore_guest.h), as
[`docs/design/runtime-abi.md`](../docs/design/runtime-abi.md) specifies. [`docs/design/translator.md`](../docs/design/translator.md)
has the design, the results and the open items.

Generated C goes to `build/` (git-ignored) and is never committed. A fixture suite of our own assembled snippets,
run under the Unicorn emulator, verified the translator; the suite is a development tool outside this source tree
(translator.md §5 has its method and results).

| Path | What |
|---|---|
| `sgrecomp/image.py` | PE / flat-blob byte access, import table, import-shim naming (`imp_symbol`) |
| `sgrecomp/cfg.py` | per-function CFG by recursive descent (the manifest builder's rules, plus EH resume roots) |
| `sgrecomp/lift.py` | instruction semantics → C |
| `sgrecomp/emit.py` | function → `SG_FUNC`: calls, returns, tail calls, switches, landings |
| `sgrecomp/memorder.py` | memory-ordering policy: which accesses become acquire loads / release stores, and the C rewrite ([memory-ordering.md](../docs/design/memory-ordering.md)) |
| `memorder_sites.py` | generates `data/threads/ordering-sites.json`, the audited site list the policy reads |
| `include/sg_recomp.h` | inline helpers the generated C calls (flags, x87, SSE/MMX lanes, strings, division) |
| `translate.py` | driver over the game exe: slices, `--all`, coverage, `clang -fsyntax-only` check |

## Commands

```
# pilot slices (each a few seconds), output in build/recomp/
PYTHONPATH=tools/.deps python3 -P recomp/translate.py --range 11e0f00-11e2000 --name crt --check
PYTHONPATH=tools/.deps python3 -P recomp/translate.py --range 8669f0-86c000 --name zlib --check
PYTHONPATH=tools/.deps python3 -P recomp/translate.py --range a7c1e0-a7cae1 --name game_a7c1e0 --check
# whole program into 500-function files (what tools/link/build.py --tus does, with its own split)
PYTHONPATH=tools/.deps python3 -P recomp/translate.py --all --chunk 500 --out build/recomp/all --name all
```

Memory ordering: `translate.py --memorder p0|p1|p2|p3` (or `SG_MEMORDER`; default `p2` from
`data/threads/ordering-sites.json`). `p0` orders nothing beyond `lock`/`xchg` (plain relaxed accesses); `p3`
orders every non-stack access. `translate.py` exits with an error if a listed site of a translated function is
not lowered as listed.

```
PYTHONPATH=tools/.deps python3 -P recomp/memorder_sites.py [--check]     # regenerate / verify the site list
```

Compile flags for generated C (runtime-abi.md §11): `-std=c11 -O2 -fno-strict-aliasing -fwrapv
-fno-math-errno -ffp-contract=off -Wno-pedantic -I runtime/include -I recomp/include`.
