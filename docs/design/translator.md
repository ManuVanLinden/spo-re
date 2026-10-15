# Translator (x86-32 → C)

The translator turns every function of the exe into C once, ahead of time; the C is compiled for arm64 and
linked with the runtime. Translated game C goes to `build/` (git-ignored) and is never committed.

Target: GOG `SporebinEP1/SporeApp.exe` (`dc04aee5…e59a37`). Code: [`recomp/`](../../recomp/)
(see its README for commands). Labels: **[obs]** measured or read with our tools; **[inf]** reasoned; **[dec]** a
design choice others may challenge. Measurements below are of 2026-10-12 unless dated otherwise.

The fixture suite (§5) runs our own x86 snippets in the Unicorn emulator and as translated C. It is a development
tool outside this source tree (Unicorn's engine is GPL-2.0, and the converter never runs the suite); this page
records its method and results.

## 1. Results

| Criterion | Result |
|---|---|
| Every fixture passes against the oracle (Unicorn x86-32) | **Pass.** 1,437 fixtures, 5,748 runs (4 random machine states each), **0 failures at -O2 and at -O0** [obs, the suite's report]. Two further fixtures document known model deviations (§5.3) and are reported apart |
| The pilot slice compiles with `clang -fsyntax-only` | **Pass, 0 errors and 0 warnings** with `-Wall` [obs]: CRT 0x11e0f00–0x11e2000 (200 functions, 1,223 instructions), zlib 0x8669f0–0x86c000 (49 / 7,281), game leaf cluster 0xa7c1e0–0xa7cae0 (15 / 893). No unhandled instruction in any of them |
| Coverage against the instruction-form census's 547 forms | **547 / 547 forms verified by fixtures** (the census's forms against the fixtures' forms: 547/547, 100% of 4,726,571 reachable instructions). **547 / 547 forms lowered** over the whole program (`translate.py --all --coverage`) |

Beyond the pilot [obs]:

| Measure | Value |
|---|---|
| Functions translated (all manifest functions + 3 promoted labels, §4.3) | 93,827 |
| Instructions translated | 4,726,576, **0 unhandled** (`sg_unhandled_insn` emitted 0 times) |
| Generated C for the whole program | 188 files of 500 functions, 526 MB; **all pass `clang -fsyntax-only` with 0 errors, 0 warnings** (30 s with 4 jobs) |
| Translator run time / peak memory | 94 s, 370 MB (coverage only); 150 s, 459 MB writing all C |
| One 500-function file at -O2 | 30,096 instructions → 5.7 s, 358 MB RSS, 748 KB arm64 object |
| Whole program to arm64 objects at -O2 (compiled, never linked or run) | **188 / 188 files compile**; 1,265 CPU-s (421 s wall, 4 jobs), 110 MB of objects |
| Landing functions | 49 (data/eh/resume-points.json, one landing function corrected, §4.4) |

These numbers show that every function of the EP1 exe translates and compiles [obs]. Whether the code is correct at
that scale is shown by the linked game running.

## 2. Pipeline

```
manifest ───────┐   EH resume points ────┐   imports (PE)
                ▼                        ▼
       cfg.build(entry, roots)  ──►  emit.Emitter.function  ──►  SG_FUNC(xxxxxxxx) { ... }  (build/recomp/*.c)
       recursive descent             lift.Lifter per instruction    + SG_DECL / extern sg_imp_* per file
```

- **CFG** (`sgrecomp/cfg.py`): recursive descent from the entry and any EH resume roots, stopping at other
  function entries (jump to one = tail call) and after no-return calls. Switch targets come from the manifest's `T`
  records in slot order. Blocks split at targets and terminators. These are the manifest builder's rules; §4
  lists where the two differ.
- **Lifting** (`sgrecomp/lift.py`): Capstone with detail; one C block `{ ... }` per instruction, preceded by a
  comment with the address and disassembly. x87 is decoded from the opcode bytes (D8–DF + ModRM), not from
  Capstone's mnemonics, which avoids the `fsub`/`fsubr` naming swap for `st(i), st(0)` forms.
- **Emission** (`sgrecomp/emit.py`): calls, returns, tail calls, switch tables, landings; declarations per file.
- **Helpers** (`recomp/include/sg_recomp.h`): static inline semantics for flags, x87, SSE/MMX lanes, string
  instructions and division, plus two `[runtime]` hooks (§6.2).

## 3. Lowering rules

### 3.1 Registers, memory, calls (runtime-abi.md §3–§7, followed exactly)

| Guest | C |
|---|---|
| registers | `ctx->eax` … `ctx->edi`; partial registers with `SG_R8L/SG_R8H/SG_R16` and the `SG_SET_*` macros |
| memory | `sg_ld8/16/32/64`, `sg_st*`; `fs:` operands `SG_FS(ctx, off)`; addresses computed in `uint32_t` |
| `call rel32` | `SG_CALL(sg_f_<target>, <return address>)` (push, host call, return-address check) |
| `call $+5` | `SG_PUSH32(ra)` only |
| `call [IAT]` | `SG_CALL(sg_imp_<dll>_<name>, ra)` |
| `call r/m32` | `SG_CALL_IND(expr, ra)`, target evaluated before the push |
| `ret`, `ret N` | `SG_RET(N)` |
| `jmp` to another entry, `jcc` to another entry | `SG_TAILCALL(sg_f_<target>)` (`musttail`) |
| `jmp [IAT]` / `jmp r/m32` | `SG_TAILCALL(sg_imp_…)` / `SG_TAILCALL_IND(expr)` |
| `jmp [i*4+T]` | `switch ((uint32_t)(ea - T)) { case 4*k: goto L_…; … default: SG_TAILCALL_IND(sg_ld32(mem, ea)); }`: an index out of range reaches the miss hook through `sg_lookup` (abi §6.2). Works for both the dword and the two-level byte-index form, since the `movzx` before it is ordinary code |
| no-return calls | the call, then `sg_unhandled_insn(ctx, ra)` (the manifest's 13 `call-noret` blocks, plus calls to `_CxxThrowException`, `longjmp`, `exit`, `_exit`, `abort`, `ExitProcess`, `terminate`, `_amsg_exit`, `ExitThread`, `_endthreadex`, directly or through their `jmp [IAT]` thunks) |
| `int3` in a body | `sg_int3(ctx, va)` |
| landing functions | `SG_LANDING_BEGIN(va)` first, `SG_LANDING_RET`/`SG_LANDING_TAIL` at every exit, `SG_RESUME_DISPATCH` switch at the end (abi §10.3) |
| `div`/`idiv` | `sgr_div32` etc.: explicit checks call `sg_divide_error(ctx, va)` before any register changes |
| `lock xadd` / `lock cmpxchg` / `lock cmpxchg8b` / `xchg m32` | `sg_lock_xadd32`, `sg_lock_cmpxchg32/64`, `sg_xchg32` (seq_cst; alignment checked) |
| `cpuid`, `rdtsc`, `fxsave`, `ldmxcsr` | `sg_cpuid`, `sg_rdtsc`, `sg_fxsave`, `ctx->mxcsr = v; sg_set_mxcsr(ctx, v)` |
| `mov m16, sreg` | the Windows selector value (cs 0x1b, ds/es/ss 0x23, fs 0x3b, gs 0): only the CRT's register dump at 0x11e164c uses it [obs] |

Registers are accessed through `ctx` everywhere. Because `ctx` is `restrict`, clang keeps them in host registers
between calls and stores them only before calls and returns. That is abi §4.3's write-back discipline, and the
translator does not have to track it [obs: arm64 output of 0xa7c1e0 and adler32 keeps all guest state in
`w8`–`w17` and writes `ctx` once before `ret`].

### 3.2 Flags [dec]

- Every flag-writing instruction assigns a **function-local** `sg_lazyflags lf` record using the header's own
  kinds (`SGR_FL(SG_FOP_SUB, size, res, dst, src)` …). Consumers (`jcc`, `setcc`, `cmovcc`, `adc`/`sbb`,
  `inc`/`dec` carry, `lahf`, `pushfd`, `rcl`/`rcr`) call `sgr_cond(lf, cc)` / `sgr_cf(lf)`. These copy the record into
  a temporary `sg_ctx` and use `sg_cond`/`sg_flag_*` from spore_guest.h unchanged, so the flag logic is defined once,
  in the ABI header.
- No liveness pass is needed: `lf` is a local, so clang deletes dead flag computations, and when it knows the
  producing kind it folds `sgr_cond` into a plain `cmp`/`b.cc` or `csel` [obs: adler32's
  `cmp ecx,0xfff1; jb` becomes `cmp w10, w12; csel … hi`]. Nothing is written to `ctx->fl` before calls or
  returns (D4).
- Flags not representable by a record kind (`rol`/`ror`/`rcl`/`rcr`, `shld`/`shrd`, `bsr`/`bsf`, `bt`,
  `cmpxchg8b`, `sahf`, `popfd`, `fcomi`, `comiss`/`ucomiss`) use materialised `SG_FOP_EFLAGS` records, merged with
  the untouched flags through `sgr_merge`.
- Shifts by `cl` update flags only when the masked count is non-zero (`if (c) { … }`), as on x86.
- `SGR_FLAGS_AT_EXIT=1` (fixture builds only) writes `lf` to `ctx->fl` at returns and tail calls so the
  harness can compare flags.

### 3.3 x87 [dec]

- **Physical stack in `ctx->fpu`**, manipulated through inline helpers (`sgr_fpush`, `sgr_fpop`, `SGR_ST(i)`,
  `sgr_fcom`, `sgr_fist32`, `sgr_frnd` …). Keeping ST(i) in C locals with per-block stack depths would be faster and
  is not done (§7).
- **Precision control**: every `fadd/fsub/fsubr/fmul/fdiv/fdivr/fsqrt` (all forms,
  including `fiadd` etc.) is wrapped in `SG_X87_PC(ctx, v)`. That macro rounds to `float` when the guest control
  word's PC field is 0 (24-bit). It reads PC from `ctx->fpu.cw`, so the context needs no extra field.
  On arm64 it costs one `tst` + `fcsel` [obs].
- **Rounding control** applies to `fist`/`fistp`/`frndint` through `sgr_frnd` (nearest, down, up, truncate),
  and `fldcw` stores the control word. MSVC's `(int)` truncation windows (`fnstcw; or 0xc00; fldcw; fistp;
  fldcw`) come out correct without pattern matching; fusing them would only make them faster.
- `fcom` family set C0/C2/C3 in `ctx->fpu.sw`; `fnstsw` synthesises TOP. The `fnstsw ax; test ah, imm; jcc`
  idioms go through the general path, and 30 fixtures verify them. They are not fused into a single comparison.
- `fcomi`/`fucomi(p)`, `comiss`/`ucomiss` set ZF/PF/CF and clear OF/SF/AF, as Intel specifies.
- Transcendentals use host libm (`sin`, `cos`, `tan`, `atan2`, `log2`, `exp2`, `ldexp`).

### 3.4 SSE and MMX

- Scalar and packed single-precision arithmetic is literal C on `ctx->xmm[n].f32[i]`. `minss`/`maxss` follow x86
  operand order (the second operand wins on NaN or equal zeros). Compares produce lane masks, and `cvt*2si` give
  0x80000000 out of range.
- `rcpps`/`rsqrtps`/`rsqrtss` are computed exactly. Real CPUs return 12-bit approximations that differ between
  vendors, so their fixtures compare with a relative tolerance of 1e-3. In this one SSE area the fixtures cannot
  prove the result matches hardware bit for bit [inf].
- MMX and SSE2 integer ops use per-lane helpers on 64-bit halves (`SGR_LANES`), with saturating variants,
  packs, unpacks and shuffles. `emms` sets the tag word to empty (abi §4.5: MMX is not aliased onto the x87
  stack).

### 3.5 Imports

The shim symbol is `sg_imp_<dll stem lower-case>_<export>`, or `ord<N>` for ordinals (abi §5.1). The two
MSVCR90 C++ exports that are not C identifiers (`?terminate@@YAXXZ`,
`?_type_info_dtor_internal_method@type_info@@QAEXXZ`) have every non-identifier character replaced by `_`
[dec]. The HLE thunk generator (`tools/hle/gen_thunks.py`) uses the same rule, and **all 482 import symbols
referenced by the translated program exist in its thunk set** under `runtime/hle/` [obs].

## 4. CFG: where the translator differs from the manifest, and why

We compared the translator's block set with the manifest's for all 93,825 manifest functions. **45 differ, all
by intent** [obs: `translate.py` reports `cfg_mismatch_*`; classified by hand]:

| Count | Cause |
|---:|---|
| 33 | Landing owners gain the blocks at their resume points, which no CFG edge reaches |
| 7 | The matching catch funclets and SEH filters (0x841336, 0x8413a6, 0x841416, 0x84148a, 0xfd1f10, 0x1170227, 0x11e152f) lose those blocks, which the manifest had attached to them |
| 3 | Code after a no-return call that the manifest kept decoding: 0x845269 after `_CxxThrowException`, 0x11e0b27, and 0x52eee0's dead `ret` at 0x52ef4e |
| 2 | 0x864760 and 0x11f8d90 lose the blocks of the labels promoted to functions (§4.3) |

### 4.3 Three manifest labels are functions [obs]

The manifest records 55 labels. Fifty-two are EH resume points. The other three are **code addresses taken as values that
start a function of their own**:

- **0x864780**: has a full prologue and starts right after 0x864760's no-return `longjmp` call. The manifest builder
  kept decoding past the `longjmp` and recorded it as an "address-taken label".
- **0x11f8df0 and 0xc2e640**: begin on 16-byte boundaries after `int3` padding. The exception-handling scan
  flags them as false catch continuations.

A label is not a dispatch entry, so an indirect call to one would reach the miss hook. `translate.py` promotes the
three to functions [dec]. Listing them as canonical entries in the manifest would have the same effect (the count
would become 93,677).

### 4.4 One recorded landing function is wrong [obs]

Resume point 0x86483c (`setjmp-return`) is listed in `data/eh/resume-points.json` with landing function 0x864760. The `call _setjmp3` (0x11e0cec)
at 0x864837 is in 0x864780, which also stores 0x864760 as an error callback (the routine that then calls
`longjmp`). The landing must be in 0x864780; with 0x864760 the resume would end in `sg_bad_resume`.
`translate.py` corrects the landing function of a setjmp-return to the function whose CFG contains the
preceding `call`. That changes only this one entry.

## 5. Fixture suite

### 5.1 Method

- **Our own snippets only.** A generator writes them: one function per fixture, ending in `ret`.
  Apple clang's integrated assembler assembles them for `i386-unknown-linux-gnu` (a small ELF reader
  applies `R_386_32`/`R_386_PC32`). All fixtures share one blob at 0x401000, inside
  the ABI's `.text` range, so `sg_lookup` dispatch works.
- **Oracle:** Unicorn 2.1.4 x86-32. It gets the same initial state: registers, EFLAGS, x87 control word 0x27f,
  MXCSR 0x1f80, xmm0–7, MMX values in the FP registers, 8 KB of data at 0x01500000, 8 KB of stack, and a TEB at
  0x7ff00000 reached through a GDT descriptor (selector 0x3b), so `fs:` has a non-zero base.
- **Host side:** the translator converts the same bytes to C. A host harness compiles that C with the ABI
  flags against `spore_guest.h` and runs it. The harness provides a minimal runtime: the 4 GiB window, run-time
  dispatch, thunk slots, landings and `sg_eh_resume`, and fake "imports" whose x86 twins sit in Unicorn's thunk
  page.
- **Compared:** status (normal return, #DE, int3, dispatch miss, return mismatch), 8 GPRs, the six arithmetic
  flags (minus undefined ones, §5.2), all three memory regions byte for byte, x87 TOP, control word, valid
  physical registers and C0/C2/C3 after compares, xmm lanes, MMX registers and MXCSR (without sticky bits).
- **States:** four per fixture, drawn from boundary values (0, 0x7f, 0x80, 0x7fffffff, 0x80000000, …) and
  random values, plus random initial flags. Every flag consumer is therefore tested on materialised flags too.

| Family | Fixtures | What |
|---|---:|---|
| auto | 1,003 | each census form instantiated in 2–4 variants: other registers including high-byte ones, `[b+d]` and `[b+i*s+d]` addressing, and `m128` variants (§7.1) |
| fuse | 294 | 20 flag producers × every condition code they define, each feeding `setcc` and `jcc` |
| x87 control | 56 | PC=24/53/64 on 7 arithmetic forms; RC near/down/up/truncate on `frndint`/`fist`/`fistp m64`; the MSVC truncation window; `fcom`→`fnstsw`→`test ah`→`jcc` for 5 compare forms × 6 tests |
| control | 37 | all 16 `jcc`, `jcxz`, every `call`/`jmp` addressing mode in the census, `call [IAT]`, `jmp [IAT]` thunk, IAT pointer in a register, tail calls, a switch with shared and out-of-function targets, an out-of-range switch index (miss-hook path), `call $+5` |
| eh | 1 | a callee resumes in its caller through a host landing (`sg_eh_resume`), discarding a host frame |
| other | 46 | strings with overlap and `ecx=0`, atomics, `pushal`/`popal`, `pushfd`/`popfd` (including the ID-bit CPUID probe), `leave`, `cpuid`, `rdtsc`, `int3`, `fxsave`, `emms` |

### 5.2 Undefined results are masked, never "fixed"

The comparison leaves out, per fixture, the flags the SDM leaves undefined, and nothing else:

- AF after logic ops and shifts
- OF after multi-bit shifts and rotates
- CF for `shl`/`shr` counts at or above the operand size
- SF/ZF/AF/PF after `mul`/`imul`
- all flags after `div`
- all but ZF after `bsr`/`bsf`

C0–C3 are compared only after compares. `cpuid`/`rdtsc` outputs are masked.

**Where Unicorn deviates from the hardware** (checked against the SDM; these masks work around oracle bugs, and a
translator bug would still show):

- **`fcomi` family:** Intel sets OF/SF/AF to 0 ([SDM via felixcloutier.com](https://www.felixcloutier.com/x86/fcomi:fcomip:fucomi:fucomip)).
  Unicorn (QEMU) leaves them unchanged. Fixtures compare only ZF/PF/CF, and fuse fixtures that would read OF/SF
  after `fcomi` are skipped. The translator follows Intel.
- **`fyl2x` with x ≤ 0:** QEMU sets C2 and does not pop; Intel returns NaN and pops. Fixtures use positive inputs.
- **Unicorn starts with FPCW = 0 and MXCSR = 0.** The harness sets both to Windows' values.
- **Writing FS in Unicorn makes a null SS fault.** The harness gives SS/DS/ES flat GDT descriptors.

### 5.3 Known model deviations (reported, not failures)

| Fixture | Difference | Source |
|---|---|---|
| `x87:fild-fistp64` | 64-bit integers above 2^53 lose low bits through `fild`/`fistp m64`; the passing twin `x87:fild-fistp64-small` stays below 2^52 | x87 values are host doubles ([DEVIATIONS](../DEVIATIONS.md) 1.2). The exe has 515 `fild m64` and 210 `fistp m64` [census]; whether any carry values above 2^53 is open |
| `dev:nan-sign` | a NaN *generated* by an invalid operation (`fsqrt(-x)`, `0/0`) is positive on arm64 (default NaN) but negative on x86 ("real indefinite"). Propagated NaNs and all other bits agree | Hardware difference. Matching it would need a check per operation; no game dependence is known [inf] |

## 6. Notes

### 6.1 Findings

- **The instruction-form census (`tools/insn-forms/forms.py`) classifies every `xmmword ptr [...]` operand as
  `xmm`.** `opform` tests `op.startswith("xmm")` before the memory pattern. The census therefore merges m128
  loads and stores into register forms: `movaps xmm,xmm` (35,224) includes `movaps xmm, [m128]` and
  `movaps [m128], xmm`, and the one `movntps xmm,xmm` is `movntps [eax], xmm0` at 0xac1400 [obs]. Totals
  are unaffected; per-form counts and the m128 form names are. Fix: test the memory regex first. The fixtures
  cover both shapes (§5.1).
- **Manifest:** 0x864780, 0x11f8df0 and 0xc2e640 are functions (§4.3), and decoding should stop after calls to
  `longjmp`/`_CxxThrowException` and the other no-return imports (§3.1 list). The translator handles both itself.
- **Header:** no change was needed to generate correct code. `sg_set_mxcsr` and `sg_fxsave` are declared in
  `sg_recomp.h` as `[runtime]`; `SG_X87_PC` is derived from `cw` there; `SG_LANDING_RET` does not write flags
  (the translator emits `SGR_EXIT_FLAGS()` before it).

### 6.2 Runtime entry points the generated code references

`sg_return_mismatch`, `sg_miss_hook` and the dispatch tables, `sg_unhandled_insn`, `sg_int3`, `sg_divide_error`,
`sg_cpuid`, `sg_rdtsc`, `sg_misaligned_atomic`, `sg_bad_resume`, `sg_ldf80`/`sg_stf80` (never reached: there are
no m80 operands), `sg_set_mxcsr`, `sg_fxsave`, plus libm. The fixture harness implements all of them minimally.

## 7. Open points

1. **Operand coverage.** Fixtures prove instruction semantics per form; generating fixtures from census
   examples' *operand shapes* (register pairs, scale/displacement combinations) would widen per-form operand
   coverage, and flags could be cross-checked on an x86 host.
2. **x87 in locals** using per-block stack depths, with a debug-build `top` assertion at calls, and fused
   truncation windows and `fnstsw`/`lahf` idioms. These would only gain speed; the general path is verified.
3. **Indirect jumps** (`jmp r32`, 1,336 sites; `jmp [b+d]` 28) are lowered as tail calls through `sg_lookup`.
   Any of them that is an intra-function computed jump will miss at run time. The manifest's CFG treats
   them as terminators too; a static classification (target sets from the code before them) is still needed.
4. **Memory ordering:** the translator lowers selected accesses as acquire loads and release stores
   (`--memorder`, default p2); see [memory-ordering.md](memory-ordering.md).
5. **Shared-tail flags** (abi §5.6): flags are dead at every boundary, which holds for all 327K calls
   [obs]. The four data-referenced mid-function entries that read flags first are not special-cased.
6. **Output size and build time:** about 111 bytes of C per instruction, mostly comments and per-instruction
   blocks. A `--compact` mode without disassembly comments would roughly halve it [inf].
7. **Base-game exe:** not translated (no manifest for it).

## 8. Commands

```
PYTHONPATH=tools/.deps python3 -P recomp/translate.py --range 11e0f00-11e2000 --name crt --check
PYTHONPATH=tools/.deps python3 -P recomp/translate.py --range 8669f0-86c000 --name zlib --check
PYTHONPATH=tools/.deps python3 -P recomp/translate.py --range a7c1e0-a7cae1 --name game_a7c1e0 --check
PYTHONPATH=tools/.deps python3 -P recomp/translate.py --all --chunk 500 --out build/recomp/all --name all  # ~150 s, 460 MB
# then: clang -fsyntax-only (or -c -O2) with the flags in recomp/README.md on each build/recomp/all/*.c
```
`translate.py --all --no-c --coverage FILE` (about 95 s, 370 MB) also writes the per-form coverage; it needs the
instruction-form census at `data/manifest/insn-forms.json`, which the tools in this tree do not build.
