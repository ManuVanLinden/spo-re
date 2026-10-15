# Memory ordering in the translated code

The translator keeps x86's memory ordering on arm64 where the game's threads depend on it. This page gives the
accesses it orders, how that set was chosen and audited, how it was verified, and what it costs.

Code: [`recomp/sgrecomp/memorder.py`](../../recomp/sgrecomp/memorder.py) (policy and C rewrite),
[`recomp/memorder_sites.py`](../../recomp/memorder_sites.py) (site-list generator, writing
`data/threads/ordering-sites.json` into the work folder), the ordered helpers in
[`runtime/include/spore_guest.h`](../../runtime/include/spore_guest.h) (after `sg_st32_rel`) and
[`recomp/include/sg_recomp.h`](../../recomp/include/sg_recomp.h), and the host test
[`runtime/core/tests/test_memorder.c`](../../runtime/core/tests/test_memorder.c).

Labels: **[obs]** observed (a run or a measurement); **[inf]** inferred, not checked.

x86 gives every plain load acquire and every plain store release ordering (TSO); arm64 does not. The translator
lowers a subset of the exe's plain memory accesses to acquire loads and release stores, and a policy picked at
translation time decides which subset.

## 1. Summary

| Item | Value |
|---|---|
| Policy | **P2 hybrid** (default): `lock`/`xchg` sequentially consistent; **acquire loads / release stores** for the 388 listed sites (spinlock spin loads, every access to the Dac hand-off globals, thread stop flags, candidate publication stores), for **every `.data`/`.tls`/`CONST` global access image-wide**, and for **every heap access in the 296 functions directly reachable from a worker thread**. Stack, read-only data and `fs:` stay relaxed; other heap accesses stay relaxed. `SG_MEMORDER=p0\|p1\|p2\|p3` selects another policy at translation time (§2) |
| Ordered accesses, whole image | **138,592** non-atomic accesses (plus the 2,403 locked sites, 4,787 accesses) of 1,829,807 explicit operand accesses; the thread analysis predicted ~138,593 in the same units [obs, §4] |
| Relaxed emission | `--memorder p0` reproduces the translator's output without any ordering, byte for byte (510 files, `diff -r` clean apart from the cache stamp) [obs] |
| Fixtures | 1,453 fixtures / 5,812 runs against Unicorn at -O2 and -O0: 0 failures with the default emission and 0 failures with p3, which turns 1,574 of the fixtures' accesses into the ordered helpers, aligned and misaligned [obs, §5.1] |
| Differential test (p2 against Unicorn) | 47,000 calls of the core set: 46,999 agree, 1 inconclusive; 12,000 calls of the 60 most-ordered functions: 14 disagree, all in 0x7b6e20, a NaN cascade identical under p0; 25,600 calls of 800 random functions whose C changes under p2: all agree [obs, §5.2] |
| Host test | the Dac hand-off, two threads, 2 M rounds: relaxed emission stale in 20 of 20 runs, ordered emission stale in 0 of 20 [obs, §5.3] |
| Cost | translator +1%; compile time unchanged; objects +0.7%; run-time cost within noise on the functions measured (§6) |

## 2. Policy and why

The thread analysis (`tools/threads/`) weighed five policies. The numbers below are its predictions; §4 has
the translator's own counts.

| Policy | What is ordered | Predicted | Implemented as |
|---|---|---:|---|
| P0 | `lock`/`xchg` only | 2,403 sites | `--memorder p0`: the emission without ordering, byte for byte |
| P1 | P0 + annotated idioms | 2,408 + a list | `p1`: P0 + `data/threads/ordering-sites.json` |
| **P2** | P1 + all globals + heap accesses in worker closures | **141,106** | **`p2`, the default** |
| P3 | every non-stack, non-read-only access (x86-TSO) | 1,022,719 | `p3` (`fs:` left relaxed: per-thread TEB) |
| P4 | also the stack | 3,002,117 | not implemented |

**Why P2 and not P3 as the default** [inf, with the measurements of §6]:

- P1 already covers the known dependencies: the ticket spinlock, the Dac hand-off, the stop flags
  (§4). P2 adds every global, which is where the rest of the lock-free protocols of this binary live: all 32
  shared globals the thread analysis found are globals, and a global access has a constant address, so its
  alignment is known at compile time and the ordered form is one `ldapr`/`stlr` with no alignment branch
  [obs: arm64 output in §3].
- P3 orders 1,008,537 accesses, half of them in Havok, renderer and procedural inner loops. Every ordered
  access is also a compiler barrier (no merging, no hoisting, no vectorising across it), and its heap address
  has unknown alignment, so each one carries a `tst`/`b.ne` to a fence fallback.
- Extended play on P0 showed no ordering failure, so any unknown dependency that remains is rare. P2 removes the known ones at near-zero cost; P3 stays one switch away
  (`SG_MEMORDER=p3` and a retranslation) as the oracle build for a crash that looks like ordering.
- P2 does not cover heap data shared through job bodies and virtual callees that the direct worker closures
  miss, nor plain-pointer publications outside the 69 multi-thread functions. These make up the residual
  deviation ([DEVIATIONS.md](../DEVIATIONS.md) 1.6).

## 3. Implementation

**Translator** (`recomp/sgrecomp/memorder.py`, called from `emit.py` for every non-terminator instruction
and every `call`):

1. Classify the instruction's explicit memory operand exactly as the thread scanner does
   (`tools/threads/scan.py`): stack = `esp`-based, or `ebp`-based in a function whose entry block has
   `mov ebp, esp`; global = no base register and an absolute address in `.data`/`.tls`/`CONST`; read-only = no
   base register, any other section; `fs:`; heap = everything else. Locked instructions are skipped (already
   `seq_cst`).
2. Decide: a listed site is always ordered (all policies but p0); otherwise the policy's class rule.
3. Rewrite the lifter's C for that one instruction: `sg_ldN` → `sg_ldN_acq_u`, `sg_stN` → `sg_stN_rel_u`,
   `sgr_ldf32`/`sgr_ldx*` → `_acq`, `sgr_stf32` → `_rel`; helpers that touch memory internally
   (`sgr_rep_movs`/`stos`/`cmps`, 80-bit x87, `fxsave`) are bracketed by `sg_fence_rel()` / `sg_fence_acq()`.
   Implicit stack traffic (`SG_PUSH32`/`SG_POP32` in `push [m]`, `pop [m]`, `call [m]`) stays relaxed.
4. After translation, **every listed site of a translated function must have been lowered as listed** (an
   instruction with a memory operand that loads for `acquire`, stores for `release`), or `translate.py`
   exits with the list of offending sites.

Two loads stay as they are: the memory operand of a block-terminating `jmp [m]` and the switch-table load. In
practice both read vtables and jump tables in read-only data.

**Helpers** (`spore_guest.h`, after `sg_st32_rel`):

| Helper | Aligned | Misaligned |
|---|---|---|
| `sg_ld8/16/32/64_acq_u`, `sg_ldf32/f64_acq_u` | `ldaprb`/`ldaprh`/`ldapr` (RCpc acquire) | `ldr` through a `volatile` unaligned pointer, then `dmb ishld` |
| `sg_st8/16/32/64_rel_u`, `sg_stf32/f64_rel_u` | `stlrb`/`stlrh`/`stlr` | `dmb ish`, then `str` through a `volatile` unaligned pointer |
| `sg_ld128_acq_u` / `sg_st128_rel_u` | two `volatile` 64-bit accesses, then `dmb ishld` / after `dmb ish` (x86 does not make 16-byte accesses single-copy atomic either) | same |
| `sg_fence_acq()` / `sg_fence_rel()` | `dmb ishld` / `dmb ish` | |

The `_u` forms accept any alignment, as plain x86 accesses do; `ldapr`/`stlr` would fault on a misaligned
address. Clang emits `ldapr` for `__ATOMIC_ACQUIRE` on the default Apple Silicon target [obs:
`clang -O2 -S`, `t1` → `tst w1,#3; b.ne; add; ldapr`; a constant global address folds the test away:
`mov/movk; add; ldapr`]. The misaligned forms are the "half barrier" that FEX uses.

**Clang cannot hoist the spin loads out of the loop** [obs]: in `sg_f_006abf40` compiled at -O2 under p2 the spin loads
of `lock+4`/`lock+8` are `ldapr` inside the inner loop (`LBB0_6`, "Inner Loop Header"), seven `ldapr` in the
function against none under p0.

## 4. The site list and how to audit it

`recomp/memorder_sites.py` generates `data/threads/ordering-sites.json` (208 KB, addresses and instruction text
only) from rules, so each group can be checked and rerun:

```
PYTHONPATH=tools/.deps python3 -P recomp/memorder_sites.py            # regenerate (about 7 s)
PYTHONPATH=tools/.deps python3 -P recomp/memorder_sites.py --check    # exit 1 if the list would change
```

| Group | Sites | Rule |
|---|---:|---|
| `spinlock` | 10 | plain loads of `[esi]`, `[esi+4]`, `[esi+8]` (state, next ticket, serving ticket) in the ticket RW spinlock's read-lock 0x6abf40 and write-lock 0x6abfc0: 0x6abf43, 0x6abf46, 0x6abf6a, 0x6abf6d, 0x6abf7f, 0x6abfa9, 0x6abfd3, 0x6abff9, 0x6ac011, 0x6ac039 (acquire). The back-off count `[esi+0xc]` is not a lock word and stays plain. The SporeBin twins 0x20 lower decode to the same instructions (`base_game_twins.verified`) |
| `dac` | 77 | every instruction image-wide whose operand is one of the Dac globals 0x16e6304–0x16e6307, 0x16e633c, 0x16e7b98, 0x16e7b9c, 0x16e7ba4, 0x16e7ba8: writers 0x113d780/0x113d7e0/0x113db50/0x113dbe0/0x113e2e0/0x113df70 (release stores, e.g. 0x113d797 `mov [0x16e7b9c], 0`, 0x113d7a5 `mov byte [0x16e6307], 0`), the Dac loop 0x113dc70 (acquire loads, e.g. 0x113dc98, 0x113dcb0, 0x113df28) and 12 helper functions around them (0x113ce70–0x113e3a0); read-modify-writes (0x113cf67, 0x113def0) are `acq_rel`. All 77 hits lie in the translator's CFG of their manifest owner |
| `stop-flag` | 5 | byte loads of the stop/run flag in the thread loops: audio update 0xa228a6, 0xa229d8 (`this+0x60`), CallbackProcessor 0x92a49f, 0x92a70b (`this+0x24`), RenderWare worker 0x11e62c9 (`this+5`) (acquire) |
| `publication` | 296 | 32-bit stores of a register into heap memory in the 69 functions that the direct-call closures of two or more thread kinds share (main counts as one): candidate pointer publications into shared objects (release). Most are EA string, container and allocator code (0x921930, 0x926eb0–0x927e70) |

The file also carries `worker_functions` (the 296-function union of the worker closures that p2 uses) and
`multi_thread_functions` (69), so translation depends on no other analysis output. `tools/link/build.py`'s
translation cache stamp includes the file, so a change to the list retranslates.

**Counts** (translator, whole image, `translate.py --all`; one count per memory-operand access, a
read-modify-write counts twice, as in the thread analysis) [obs]:

| Class | Accesses | Thread analysis |
|---|---:|---:|
| global | 134,079 | 134,090 |
| heap (includes locked operands) | 879,245 | 879,275 |
| read-only | 50,649 | 52,316 |
| `fs:` | 6,950 | 6,951 |
| explicit stack operands | 758,884 | (1,981,801 with implicit push/pop/call/ret) |

| Policy | Ordered, excluding locked | Locked accesses | Prediction (same units) |
|---|---:|---:|---:|
| p0 | 0 | 4,787 | 0 |
| p1 | 390 (the 388 sites; 2 are read-modify-writes) | 4,787 | "a list" |
| **p2** | **138,592** = 134,000 global + 4,202 worker heap + 390 sites | 4,787 | 138,703 − the ~110 locked accesses inside the worker closures (55 sites) = **~138,593** |
| p3 | 1,008,537 = 134,000 + 874,147 + 390 | 4,787 | 1,013,365 − 4,787 = **1,008,578** |

## 5. Verification

### 5.1 Fixtures

The translator's fixture suite (small x86 programs of our own, run on the Unicorn emulator and as translated C;
a development tool that is not part of this source tree) [obs, about 10 s per run]:

- default (`--memorder p0`, the fixtures' translation is unchanged): 1,453 fixtures, 5,812 runs, **0
  failures** at -O2 and at -O0, 547/547 forms verified;
- `--memorder p3`: every non-stack access of every fixture becomes an ordered helper (1,574 accesses, 1,547
  C lines rewritten; all `_acq_u`/`_rel_u` forms, the 128-bit forms, `sgr_ldf32_acq`, `sgr_ldx_acq`, the
  fence brackets around string ops and 80-bit x87). Random machine states make many of them misaligned, so
  both paths of each helper run. **0 failures at -O2 and at -O0**;
- `--memorder p1`: identical to p0 for fixtures (no listed sites in fixture code), 0 failures.

No fixture's generated C changes under the default policy (the fixture image has no `.data` and no worker
functions, so p2 = p0 there).

### 5.2 Differential test

The differential tester (also a development tool outside this source tree) runs translated functions on the host
against Unicorn executing the original bytes [obs]:

| Set | Functions | Calls | Result (p2 host against Unicorn on the original bytes) |
|---|---:|---:|---|
| `core` (the tester's core set, 1,000 cases each) | 47 (5 with ordered accesses under p2) | 47,000 | **46,999 agree, 0 disagree**, 1 inconclusive (emulator budget) |
| the 60 functions with the most ordered accesses under p2 (up to 366 each: 0x1146e40, 0x1169fb0, 0x1171c00, 0x11f4660, 0x795f70 …), 200 cases each | 60 | 12,000 | **11,986 agree**, 14 disagree, all in 0x7b6e20, a known NaN cascade; the same 14 cases disagree under p0 |
| random sample of the 25,563 functions whose C changes under p2, 32 cases each | 800 | 25,600 | **25,600 agree, 0 disagree** (25,235 return normally and are compared in full, 365 fault at the same place on both sides; 4 match the known `nan-payload` deviation) |
| `wide` random sample (up to 1,500 functions, seed 7), stopped part-way (slow, large functions first) | 252 | 8,064 | 12 disagree, all in 0x11763f0, the same kind of NaN cascade; the rest agree or match a known deviation |

25,563 of the 44,719 functions the tester covers (57%) have at least one ordered access under p2 (38,346
under p3).

**p3 and p2 against p0, same cases, host only** [obs]: over 10,474 normally returning core cases and 9,772
global-heavy cases, every output of p2 and p3 equals p0's except NaN payloads: 18 (p2) and 35 (p3) cases
differ only in which NaN bit pattern is stored (0x7fffffff / 0xfffffffe / 0xffffffff), or, in 0x7b6e20, in
the NaN cascade that follows; two runs of p0 itself differ in none. Ordered accesses change clang's
instruction order around them, which changes which operand's NaN an `addss`/`mulss` returns. That is the
accepted `nan-payload` deviation ([DEVIATIONS.md](../DEVIATIONS.md)); ordering plays no part in it.

### 5.3 Host test: the Dac hand-off

`runtime/core/tests/test_memorder.c`, registered in `tests/test_main.c` next to `test_stall`:

- Writer thread = the main thread's pause/resume/stop functions: six record fields with plain `sg_st32`
  (as the translator emits for an unlisted heap store), then the state word. Reader thread = the Dac loop:
  polls the state word, then reads the record. Every value is the round number, so a field below the state
  just read is stale. Fields and state are 128 bytes apart (Apple's cache line).
- **ordered** = the p2 emission (`sg_st32_rel_u` state store, `sg_ld32_acq_u` poll, plain fields);
  **relaxed** = the p0 emission, with a compiler barrier between every access so that only the CPU can
  reorder (the game's loop has opaque calls there).
- `core_tests memorder` (part of `make -C runtime/core test`) asserts the ordered case and reports the
  relaxed one; `core_tests memorder-relaxed` also asserts the relaxed case, so it fails and reproduces the
  relaxed emission's bug.

Results, 20 runs each on an Apple Silicon Mac [obs]:

| Emission | Runs failing | Stale reads per run | Reads |
|---|---:|---|---|
| ordered | **0 / 20** | 0 | 128 M in total |
| relaxed (`memorder-relaxed`) | **20 / 20** | 467,853 – 740,899 | 1.46 M – 2.1 M per run |

Both halves are needed [obs, a variant of the same test]: release store only, 5,552 stale of 60 M reads (the
reader's loads pass each other); acquire poll only, 190,182 of 2.8 M (the writer's stores pass each
other); both, 0 of 22 M.

## 6. Cost

Measured on an Apple Silicon Mac under background load; "CPU-s" from the compile driver's rusage.

| Measure | p0 | p2 (default) | p3 |
|---|---:|---:|---:|
| ordered non-atomic accesses, whole image | 0 | 138,592 (7.6% of explicit accesses) | 1,008,537 (55%) |
| translate `--all`, writing C | 103.0 s, 460 MB | 103.9 s, 457 MB | (counts only: 79.9 s) |
| generated C | 527 MB | 528 MB | — |
| compile, controlled sample of 21 TUs, -j10 (twice each) | 28.6 / 28.6 CPU-s | 28.4 / 28.5 CPU-s | — |
| compile, the 10 most-ordered TUs | 10.7 CPU-s | 10.9 CPU-s | — |
| compile, whole image, -j10 | 674.9 CPU-s, 68.4 s (quiet) | 1,250.5 CPU-s, 132.5 s (under load) | — |
| objects, whole image | 102.6 MB | 103.3 MB (+0.7%) | — |
| differential-test host binary (44,719 functions) | 15.7 MB | 16.3 MB (+3.7%) | 20.9 MB (+33%) |
| host time, 948 zlib cases (inflate, longest_match, adler32, crc32, inflate_table), median of 3 | 2.135 s | 2.163 s (+1.3%) | 2.149 s (+0.7%) |
| host time, 9,772 cases of the 60 most-ordered functions, median of 5 | 1.332 s | 1.315 s (−1.3%) | 1.346 s (+1.1%) |

The whole-image compile times are not comparable, because the p2 build ran under load; the controlled samples
show no compile-time change. The host timings include the harness's per-case work (state restore, page comparison: about
0.14–0.4 ms per case), so they bound the cost rather than isolate it: on these functions neither p2 nor p3
costs more than the ±1–2% run-to-run noise [obs]. That is consistent with Apple cores executing
`ldapr`/`stlr` near the speed of plain accesses [inf], but these are leaf functions; Havok, the renderer's
inner loops and anything vectorised were not measured (they call imports or indirect targets and are
outside the tester's selection), and p3's +33% code size shows how much more code it emits.

We therefore expect the default's run-time cost in play to be negligible (7.6% of static accesses, most of them
constant-address globals with no alignment branch) [inf].

## 7. Regenerating

```
PYTHONPATH=tools/.deps python3 -P recomp/translate.py --all --chunk 184 --name tu --out <dir>   # p2 by default
SG_MEMORDER=p0 ... (or --memorder p0)    # the emission without ordering, byte-identical
```

The whole image under p2 is 510 files, 528 MB, and compiles with `tools/link/build.py`'s flags to 510/510
objects with 0 errors (103.3 MB).

## 8. What remains

- The run-time cost in the game is not measured directly; if the frame rate drops, `SG_MEMORDER=p1` keeps the
  known idioms only.
- Job bodies and virtual callees lie outside the worker closures, so their heap accesses stay relaxed under
  p2. Enumerating the `cJobManager::CreateJob` closures would let p2 cover them.
- Publication coverage is a heuristic (296 stores in 69 functions). The reader side of a pointer publication
  relies on the address dependency, which arm64 hardware respects and clang has no reason to break, though C
  does not promise it [inf].
- The translator does not rewrite block-terminating `jmp [m]` or switch-table loads (read-only data in
  practice).
- If an ordering failure is suspected in play: retranslate with `SG_MEMORDER=p3` (TSO for all non-stack
  data) and compare.
