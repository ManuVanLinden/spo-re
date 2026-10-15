# Runtime ABI: generated code, runtime and shims

**Header:** [`runtime/include/spore_guest.h`](../../runtime/include/spore_guest.h) (`SG_ABI_VERSION 1`).

This document fixes the contract that three groups of code share:

- the **translated guest code** that the translator ([translator.md](translator.md)) emits, one C function per guest entry point;
- the **runtime core**: the guest memory window, threads, dispatch and exception handling;
- the **HLE shims** for Win32, the CRT, D3D9, DirectSound and the rest.

The header is the normative form. Where this document and the header disagree, the header wins, and the
mismatch is a bug in this document.

**Labels.** **[obs]** means read from the binary with our tools; the evidence is cited. **[inf]** means reasoned
and not yet tested. **[dec]** marks a design decision of this ABI.

**Evidence.** We measured the [obs] facts with our own static scripts while designing the ABI; none executes game
bytes. The scripts and their outputs are development material outside this source tree, and the citations below
name the outputs:

| Output | What it settles |
|---|---|
| `abi-facts.json` | entry alignment (dispatch shape), TLS directory, `fs:` offsets, rare instructions, the 119 `jmp [IAT]` thunks and their callers |
| `flags-live.json` | whether arithmetic flags are live across calls or at function entry |
| `eh-resume-points.json` | how many places EH must resume into the middle of a function |
| the ABI self-test | executable checks of the header against our own synthetic fixtures |

---

## 1. Decisions at a glance

| # | Topic | Decision | Basis |
|---|---|---|---|
| D1 | Guest memory | One 4 GiB + 64 KiB host reservation at base `mem`. Guest address `a` lives at `mem + (uint32_t)a`. The image sits at 0x400000 and is never relocated | [obs]; [dec] |
| D2 | Function type | `void f(sg_ctx *ctx, uint8_t *mem)` for **every** guest-callable entry: translated functions, import shims, COM-method thunks and runtime helpers | [dec] |
| D3 | CPU state | One `sg_ctx` per thread, holding 8 GPRs, lazy flags, `eip`, `fs_base`, xmm0–7, MXCSR, the x87 file and MMX. At every call boundary, all state is in `ctx` | [obs]; [dec] |
| D4 | Flags across calls | **Arithmetic flags are dead at every call and return boundary.** The translator never has to write flags to `ctx` before a call | 0 of 327,450 call sites read flags afterwards [obs, §4.3] |
| D5 | Guest stack | The guest stack is real and lives in the window. `call` pushes the real guest return address; `ret N` pops it and releases N bytes. The host call stack mirrors the guest call stack one to one | [dec] |
| D6 | Return check | Every `ret` stores the popped address in `ctx->eip`. The caller compares it with its own fall-through address and calls `sg_return_mismatch` on a difference. A compile switch can turn the check off | [dec] |
| D7 | Dispatch | A sorted entry array plus a bucket index with one bucket per 16 bytes of `.text` (about 4 MiB + 1.5 MiB), a direct table for the thunk region, and a replaceable miss hook | 97.4% of entries are 16-aligned; at most 3 per bucket [obs] |
| D8 | Host entry points | Each one gets a 16-byte slot in a guest **thunk region** at 0x7FE00000. IAT slots, `GetProcAddress` results and COM vtables hold these guest addresses | [dec] |
| D9 | COM | Host objects appear to the guest as 16-byte records `{vptr, handle, magic, 0}` in the window. Their vtables hold thunk addresses. Guest objects are called through `sg_call_guest_method` | [dec] |
| D10 | TEB/TLS | A real Windows-layout TEB per thread in the window (`fs_base`). TLS slots sit at TEB+0xE10 and TEB+0xF94, as on Windows. The image TLS block hangs off TEB+0x2C | `fs:` offsets used: 0, 4, 8, 0x18, 0x2C [obs] |
| D11 | EH | Guest EH state stays in guest memory, and the runtime's `__CxxFrameHandler3`, `_except_handler3/4` and `longjmp` interpret it. The host is involved only to resume mid-function, through **landings** (a host `setjmp`: `__builtin_setjmp` on x86, `_setjmp`/`_longjmp` on arm64) in the few functions that need them | 11 try blocks, 11 catch handlers, 28 `_setjmp3` sites [obs] |
| D12 | Memory order | Plain guest loads and stores are relaxed unless the translator's ordering policy ([memory-ordering.md](memory-ordering.md)) lowers them through the ordered `sg_ld32_acq`/`sg_st32_rel`. `lock`/`xchg` use sequentially consistent atomics | [obs] |

---

## 2. Scope and non-goals

- **In scope:** everything generated code touches, and everything a shim needs in order to be called by guest code
  and to call it back.
- **Not in scope:** how the translator lowers individual instructions. Exceptions are where the lowering
  affects the ABI, such as flag liveness, x87 sync points and division faults.
- **Also not in scope:** the internal design of the runtime's allocator, scheduler or D3D layer.
- **x87 precision and the control word** belong to the floating-point model. This ABI only reserves the state for
  them (§4.4).
- **The EH database and the FH3 algorithm** belong to the exception-handling runtime. This ABI fixes only the
  hand-off points (§10).

---

## 3. Guest address space

### 3.1 The window [dec]

The runtime reserves `SG_WINDOW_SIZE + SG_WINDOW_GUARD` (4 GiB + 64 KiB) of host virtual memory with
`PROT_NONE` and commits pages on demand. The base pointer is called `mem` everywhere.

- Guest addresses are `uint32_t`. Guest arithmetic wraps at 32 bits, as on the original CPU. An unaligned
  4-byte access at 0xFFFFFFFE lands in the 64 KiB guard instead of outside the reservation.
- **Why `mem` is a parameter.** It is passed into every function, not read from a global, so clang keeps it in an
  argument register (`x1` on arm64). A guest access then compiles to `ldr w, [x1, w_addr, uxtw]`. A global would
  have to be reloaded after any store through a `char`-typed pointer. This is the N64Recomp pattern (`rdram`).
- **A fixed host base brings nothing.** macOS would allow reserving a fixed host address above 0x70_0000_0000, but
  clang keeps the base in a register even when it is a constant, so `mem` stays a parameter
  ([image-linker.md](image-linker.md) §9.1).

### 3.2 Memory map

The exe is not LARGE_ADDRESS_AWARE (characteristics 0x102 [obs]), so everything the guest can see stays
below 0x80000000.

| Guest range | Contents | Protection | Label |
|---|---|---|---|
| 0x00000000–0x0000FFFF | null guard, never mapped | none | [dec] |
| 0x00010000–0x003FFFFF | free; the allocator may use it | — | [dec] |
| 0x00400000–0x01D0DFFF | the image, mapped section by section from the PE, unrelocated: `.text` 0x401000 (+0xFCAF2A), `.rdata` 0x13CC000 (IAT 0x13CC000–0x13CC80C), `.data` 0x150C000 (0x212764, 0xC4C00 initialized), `.tls` 0x171F000, `CONST` 0x1720000, `.rsrc` 0x1721000, `.reloc` 0x1C28000 | headers, `.text` and `.rdata` read-only (writes fault as on Windows); `.data`, `.tls` and `CONST` read/write; the IAT stays writable until the loader has filled it | [obs] `abi-facts.json` |
| 0x01D10000–0x7FDFFFFF | guest heap (starts at the image end rounded up to a 16 KiB host page, `SG_IMAGE_MAPPED_END`; 0x01D0E000–0x01D0FFFF shares a read-only host page with `.reloc`): `VirtualAlloc`, `HeapAlloc`, the CRT `malloc` family, thread stacks, D3D `Lock` buffers, COM records, guest vtables for host interfaces | by allocation | [dec] |
| 0x7FE00000–0x7FEFFFFF | **thunk region**: 65,536 slots of 16 bytes, filled with `0xCC` | read-only | [dec] |
| 0x7FF00000–0x7FFDDFFF | TEBs, one 4 KiB page each | read/write | [dec] |
| 0x7FFDF000 | the PEB, zero-filled apart from the fields a shim needs | read/write | [dec] |
| 0x7FFE0000–0xFFFFFFFF | never mapped. No `KUSER_SHARED_DATA` reference exists in `.text` [obs: `kuser_shared_data_refs` is empty] | none | [obs]/[dec] |

- **`.text` stays mapped and readable** even though it is never executed. Guest code may read its own bytes; the
  CPU probe and the crash handler do. Keeping it costs only address space.
- **Image TLS:** template 0x171F000–0x171F00C (12 bytes), `SizeOfZeroFill` 0, index variable at 0x16F2618.
  The callback array at 0x13EB0B4 is empty (its first entry is 0) [obs].

### 3.3 Accessing memory

- **Every guest access goes through the header's `sg_ld*` and `sg_st*` inlines** (8, 16, 32 and 64 bits, f32,
  f64 and 128 bits). They use `__builtin_memcpy`, so unaligned access is legal and type-based aliasing never applies.
  On arm64 each one compiles to a single load or store.
- **80-bit `tbyte` operands** go through `sg_ldf80`/`sg_stf80` in the runtime.
- **`fs:` accesses** become `sg_ld32(mem, SG_FS(ctx, off))`, where `fs_base` is the guest address of the TEB.
  `fs` is the only segment override in `.text`, and there are no `gs:` uses [obs: `fs_segment_uses`,
  `gs_segment_uses` = 0].
- **Faults.** A guest access to an unmapped page faults on the host. The runtime's SIGSEGV/SIGBUS handler reports
  the guest address (fault address − `mem`) and aborts; macOS arm64 raises SIGBUS, not SIGSEGV, for `PROT_NONE`
  pages ([image-linker.md](image-linker.md) §9.1). The runtime does not turn faults into a guest
  `EXCEPTION_ACCESS_VIOLATION` (§10.6).

### 3.4 Memory ordering (defaults)

| Guest operation | Lowering | Count [obs] |
|---|---|---|
| Plain `mov` load or store | relaxed, as a normal C access | — |
| `lock xadd`, `lock cmpxchg`, `lock cmpxchg8b`, `xchg r/m` | `__atomic_*` with `__ATOMIC_SEQ_CST` (`ldaddal`, `casal`, `caspal`, `swpal`) | 1,648 / 7 / 12 / 736 (1,667 `lock` prefixes in this sweep) |
| Accesses the memory-ordering analysis classifies as shared without a lock | `sg_ld32_acq` / `sg_st32_rel` (`ldar` / `stlr`) | [memory-ordering.md](memory-ordering.md) |

- **Alignment.** Atomic accesses need natural alignment on arm64. On x86 a misaligned locked access is a slow
  split lock, and the game issues them: we saw `lock cmpxchg8b` on 4-byte-aligned stack slots at run time.
  `spore_guest.h` ("Misaligned locked accesses") handles them in two ways. An access that stays inside one 16-byte
  unit becomes a wider compare-and-swap on that unit, atomic against every other locked access to those bytes. An
  access that crosses a 16-byte boundary takes a process-wide spin lock, atomic against other crossing accesses
  but **not** serialised against overlapping aligned atomics (a documented deviation,
  [DEVIATIONS.md](../DEVIATIONS.md)). `sg_misaligned_atomic` remains for the forms the fallback does not cover.

---

## 4. CPU context (`sg_ctx`)

### 4.1 Layout

`sg_ctx` lives in host memory, outside the window, and one exists per guest thread. Its first 64 bytes are fixed
by `_Static_assert`s, so debuggers and any hand-written assembly can rely on them.

| Offset | Field | Notes |
|---|---|---|
| 0 | `r[8]` / `eax ecx edx ebx esp ebp esi edi` | in ModRM register order |
| 32 | `fl` (`op, res, dst, src`) | lazy arithmetic flags (§4.2) |
| 48 | `eip` | the last address popped by `ret`, or a resume target (§5.3, §10) |
| 52 | `fs_base` | guest address of the TEB |
| 56 | `sysfl` | DF, AC and ID bits for `pushfd`/`popfd` |
| 64 | `xmm[8]`, then `mxcsr` | SSE (128 bits each) |
| 200 | `fpu` | x87: `st[8]`, `top`, `cw`, `sw`, `tag` |
| 280 | `mm[8]` | MMX |
| — | `mem`, `thread`, `eh_top`, `scratch` | host-only links |

- **`restrict`.** Translated functions are declared with `sg_ctx *restrict ctx` (`SG_FUNC`). No guest-memory
  access can alias the context, so clang may keep guest registers in host registers across loads and stores. They
  still have to be written back before any call (§4.3).

### 4.2 Lazy flags

- **The translator computes flags directly in C whenever their consumer is in the same function** and the path
  is known, for example `cmp` followed by `jcc`, or `test` followed by `setcc`. That is the common case and needs
  no `ctx` access.
- **`ctx->fl` is used only when flags must outlive a block that is compiled separately:**
  - jumps into the middle of another function, the shared tails (§5.6)
  - `pushfd`, and `lahf` when its source is far away
  - resume points
  - the miss path of an interpreter, if one is ever written
- **Record format.** `op = kind | size<<8 | cin<<16`, with these kinds:
  - `EFLAGS` (already materialised)
  - `ADD`, `SUB`. With the carry-in bit set, these mean ADC and SBB; CMP and NEG are SUB.
  - `LOGIC`
  - `INC`, `DEC`. The previous CF travels in the carry-in bit, because INC and DEC preserve CF.
  - `SHL`, `SHR`, `SAR`. `src` holds the masked count, which is never 0, because a 0 count leaves flags untouched
    and the translator emits nothing.
  - `MUL`. `src` is 1 when the high half is significant.
- **Evaluation.** `res` holds the result truncated to the operand size. `sg_flag_cf/pf/af/zf/sf/of`,
  `sg_cond(cc)` (with Jcc numbering) and `sg_get_eflags`/`sg_set_eflags` evaluate the record.
- **Undefined flags.** Where the SDM leaves a flag undefined (AF after shifts and MUL; SF, ZF and PF after MUL;
  OF after multi-bit shifts), the header returns a fixed value.
- **No guest code depends on undefined flags [inf].** After a call, no flag is read before being written
  (§4.3).

### 4.3 What must be in `ctx` at a boundary

A **boundary** is any of these:

- a `call` (direct, indirect or import)
- a `ret`
- a tail `jmp` to another function
- a host call that can re-enter guest code or unwind

At every boundary, all architectural state that may be live is in `ctx`, with these exceptions:

| State | Rule | Evidence |
|---|---|---|
| Arithmetic flags | **Dead** at call sites and at function entries. The translator does not write `ctx->fl` before calls or returns | **[obs]** `flags_live.py`: of 327,450 call sites, 0 read a flag before writing all six. Of 90,522 scanned entries, 4 read flags first. All 4 (`0xa15fb9`, `0x109ad3f`, `0xffc23b`, `0x1168750`) are mid-function addresses taken from data, not genuine entries: no `call`/`jmp rel32` targets them. 0x1168750 is the CPUID probe, reached from 0x11568a0 |
| DF | Always 0. `rep movs/stos` always run forward | **[obs]** `std` and `cld` appear 0 times in `.text` (`insn_counts`) |
| x87 | `top` and the live `st[]` registers are in `ctx`. By the MSVC ABI the stack is empty at a call, except that ST(0) carries a float or double return value | [inf]; the x87 depth analysis checks it |
| xmm, MXCSR | Written back if modified. Floats are not passed in xmm registers; only 11 functions read an xmm before writing it | [obs] |
| GPRs | all 8 | — |

**The translator's view.** Guest registers can live in C locals inside a function, but they must be written back
to `ctx` before a boundary and reloaded afterwards. Clang removes the redundant loads and stores.

### 4.4 x87

- **Registers are physical.** `ST(i)` is `st[(top+i)&7]`, as on the real FPU, so a value left in ST(0) by one
  function can be read by its caller.
- **Values are host `double`s** by default. Building with `SG_X87_F80` switches to a soft-80-bit layout for code
  that needs it.
- **`sw` and `tag`.** `sw` stores C0–C3 and the exception bits. Its TOP field is synthesised from `top` when
  `fnstsw` runs. `tag` is used only by `ffree`/`emms`-style resets and by stack-overflow debugging.
- **A static x87 depth analysis** lets the translator keep the x87 stack in locals inside a function and sync
  `st[]` and `top` only at boundaries.
- **Shims with x87 arguments or results** (`_CIsqrt`, `floor`, D3DX math) use `sg_fpu_pop`, `sg_fpu_push` and
  `SG_ST(ctx, i)`. Their calling convention is `SG_CC_X87`, documented in §7.2.

### 4.5 SSE and MMX

- **SSE.** `xmm[8]` is a union of float, double and integer lanes. MXCSR starts at 0x1F80. The floating-point model maps
  FTZ/DAZ onto the host FPCR; the image has 2 `ldmxcsr` and 2 `stmxcsr` sites [obs].
- **MMX is not aliased onto the x87 registers [dec].** `mm[8]` is a separate array.
  - On real hardware, MM*n* is the mantissa of physical x87 register *n*, so code that mixes MMX and x87 data
    without `emms` would see a difference.
  - The 3,698 MMX instructions are mostly memcpy-style block moves, with 22 `emms` [obs]. Mixing is
    therefore unlikely [inf].
  - The translator makes `emms` set `fpu.tag = 0xFFFF` and leave `top` unchanged.

### 4.6 Other instructions

| Instruction | Lowering |
|---|---|
| `cpuid` (23 sites) | `sg_cpuid(ctx)`, which reports a fixed SSE-capable CPU |
| `rdtsc` (666 sites) | `sg_rdtsc(ctx)` |
| `pushfd`/`popfd` (11/8) | `sg_get_eflags`/`sg_set_eflags`. The AC and ID bits round-trip through `sysfl` so the CPUID-support probe at 0x1168750 passes [obs: it toggles bit 21] |
| `pushal`/`popal` (7/7) | inline; no ABI impact |
| `fxsave` (1, at 0x4F8194, an OS SSE-support probe) | a runtime helper that writes a plausible image |
| `div`/`idiv` | divide by zero and quotient overflow call `sg_divide_error`, because arm64 does not trap |
| `int3` | calls `sg_int3`. It occurs only as padding, but reaching it means a bad dispatch |

---

## 5. Translated functions, calls and returns

### 5.1 Signature and naming

```c
typedef void sg_func(sg_ctx *ctx, uint8_t *mem);
SG_FUNC(005a9040) { ... }      /* defines  void sg_f_005a9040(sg_ctx *restrict ctx, uint8_t *mem) */
```

- **Names.** A function's name is its guest entry address as exactly 8 lower-case hex digits. A secondary entry
  into the middle of a function gets its own `sg_f_` name (§5.6).
- **Import shims** are named `sg_imp_<dll stem>_<export>`, or `ord<N>` for exports imported by ordinal. They have
  the same type.
- **Why one signature.** All arguments and results travel through `ctx` and the guest stack. This one signature
  therefore covers all four calling conventions in the image and the LTCG register conventions:
  - cdecl, 56.8% of entries
  - thiscall, 37%
  - stdcall, 4.5%
  - fastcall, 14
  - the 680 LTCG register conventions [obs]
- **No signatures are needed anywhere.** Recovering them would only be an optimisation.

### 5.2 The guest stack and `call`/`ret` [dec]

Each guest thread has its own stack in the window. The image default is 1 MiB reserved and 4 KiB committed
[obs, `stack_reserve`], with a `PROT_NONE` guard page below it. Translated code addresses the stack exactly as
the original did.

| Guest | Generated C (header macros) |
|---|---|
| `call rel32` | `SG_CALL(sg_f_target, ret_addr)`: push the guest return address, make the host call, then the return check |
| `call r/m32` | `SG_CALL_IND(expr, ret_addr)`. **The target is evaluated before the push**, so `call [esp+4]` reads the slot with the old esp |
| `ret` / `ret N` | `SG_RET(N)`: `eip = [esp]; esp += 4 + N; return;` |
| `jmp` to another entry | `SG_TAILCALL(fn)` / `SG_TAILCALL_IND(expr)`: a host tail call, guaranteed with clang's `musttail` |
| `push`/`pop` | `SG_PUSH32` / `SG_POP32` |

- **The return address is a real guest value.** Code reads it through `[esp]`: `_EH_prolog3`-style helpers,
  `_alloca_probe`, and stack walks by the crash handler. Callee-cleanup (`ret N`, 40K sites) needs no knowledge at
  the call site; the caller carries on with whatever `esp` the callee left.
- **`call $+5; pop reg`.** If the translator ever sees this, it emits a push and no call.
- **Host stack depth.** Each guest frame is one host frame. Translated frames are small, but guest recursion
  (scripts, tree walks) can be deep.
  - The host thread that runs a guest thread gets an 8 MiB stack [dec].
  - The main thread, which must also run AppKit, is linked with `-Wl,-stack_size,0x4000000` (64 MiB) [dec].
  - Measured host frames are small: median 48 bytes, maximum 608 at -O2 ([image-linker.md](image-linker.md) §9.2).

### 5.3 The return check

- **Every `SG_RET` writes the popped address into `ctx->eip`.** After the host call returns, `SG_AFTER_CALL`
  compares it with the caller's fall-through address.
- **A mismatch means the guest did a non-local return.** The cases are a return-address overwrite, `push x; ret`
  used as a jump, or a bug. A mismatch calls `sg_return_mismatch`, which in development builds logs both
  addresses and aborts.
- **Cost.** One store and one compare with a predicted branch per call.
- **`SG_CHECK_RETURNS=0` removes the check.** The `eip` store stays, because the EH and diagnostics code reads it.
- **Release builds keep the check [dec].**
- **Legitimate non-local transfers** (C++ catch continuations, `__except`, `longjmp`) never use this path. They use
  landings (§10).

### 5.4 Calling conventions as the shims see them

These are Win32's own rules, restated for shim authors; they add no constraint on generated code.

| Item | Rule |
|---|---|
| Arguments | `[esp+4]`, `[esp+8]` and so on at entry (`SG_ARG(i)`). A `double` takes two slots (`SG_ARG_F64`) |
| `this` | thiscall passes it in `ecx`. **COM passes it as the first stack argument**, because COM methods are stdcall |
| Result | `eax`; `edx:eax` for 64 bits; ST(0) for `float`/`double` (`sg_fpu_push`) |
| Preserved | `ebx`, `esi`, `edi`, `ebp`, `esp` (as adjusted by `ret N`), DF = 0, empty x87 stack except a returned ST(0) |
| Clobbered | `eax`, `ecx`, `edx`, flags, xmm0–7 |
| Cleanup | stdcall and thiscall: the callee, `SG_RET(argbytes)`. cdecl: the caller, `SG_RET(0)` |

### 5.5 Translation-unit independence

- **Each translated function refers to other guest functions only by name.** Calls go through `sg_f_<addr>` or
  through `sg_lookup`, and data only through guest addresses. Generated C can therefore be split into
  translation units arbitrarily.
- **No generated function holds host resources** such as heap memory, locks or open files across a call. A
  `longjmp` past it during EH (§10) therefore leaks nothing.

### 5.6 Mid-function entries

- **Shared tails.** About 90 non-EH jumps go into the middle of other functions. For these, the
  manifest either makes the tail its own function, entered by a host tail call, or gives it a secondary
  dispatch entry (`SG_DE_SECONDARY`).
- **Flags at a secondary entry.** Such an entry may need the flags left by the jumping code (§4.3). For those few
  edges, the translator writes `ctx->fl` before the jump and the entry reads it with `sg_cond`.
- **Funclets.** C++ EH unwind funclets and catch blocks are ordinary entries in the function list, reached
  through `.rdata` relocations. The runtime calls them with `ctx->ebp` set to the parent frame (§10.2).

---

## 6. Dispatch table and miss hook

### 6.1 Format [dec, sized from obs]

```c
typedef struct { uint32_t guest; uint32_t flags; sg_func *fn; } sg_dispatch_entry;   /* sorted by guest */
extern const uint32_t          sg_dispatch_bucket[];  /* one per 16 bytes of .text, plus a sentinel */
extern const sg_dispatch_entry sg_dispatch_entries[];
extern sg_func                *sg_thunk_fn[];        /* one per thunk-region slot */
```

- **Buckets.** `bucket[b]` is the index of the first entry at or above `SG_TEXT_START + 16*b`, so bucket *b*'s
  candidates are `[bucket[b], bucket[b+1])`. `sg_lookup` checks the range, makes two loads, compares, and in the
  usual case finds the entry on the first try.
- **Why buckets [obs].** The first union of entries had 91,124 addresses:
  - 88,784 (97.4%) are 16-byte aligned.
  - The other 2,340 are mostly thunks, adjustor thunks and funclets packed without padding.
  - 89,118 buckets hold one entry, 985 hold two and 12 hold three.
  - A flat per-byte table would cost 66 MB (4-byte index) to 132 MB (pointer). The bucket form costs about
    4.1 MB of buckets (1,034,996 × 4 B) plus 1.5 MB of entries, and is generated as `const` data, with no fill at
    start-up.
- **Count.** The manifest holds the canonical count; the linked table has 93,827 entries
  ([image-linker.md](image-linker.md) §4).
- **Thunk-region lookups** index `sg_thunk_fn` directly.
- **Resume points** (§10) are not dispatch entries. A computed jump to one would be a bug.

### 6.2 Miss hook

```c
typedef sg_func *(*sg_miss_hook_fn)(sg_ctx *, uint8_t *, uint32_t guest);
extern sg_miss_hook_fn sg_miss_hook;
```

- **Called for any guest code address without a translation.** It may return a function to run. The development
  hook logs the address, the calling `eip` and a guest backtrace from the `ebp` chain, appends the address to
  `misses.txt`, and aborts. The next translation run then includes the address, as in the N64Recomp and
  XenonRecomp workflow.
- **An interpreter can be added later** by installing a different hook that returns an interpreter trampoline.
  That needs no change to generated code.
- **Indirect `jmp` through a jump table is not dispatched.** The 1,350 tables become C `switch` statements inside
  the function. A table index out of range calls `sg_miss_hook` with the computed target.

---

## 7. Thunks between guest and host

### 7.1 The thunk region [dec]

- **Every host function the guest can call gets a guest address:** imports, dynamically resolved APIs, COM
  methods of host objects, and runtime sentinels. Each address is a 16-byte slot at
  `SG_THUNK_BASE + 16*slot` (0x7FE00000–0x7FEFFFFF).
- **The slot's bytes in the window are `0xCC`.** Guest code that reads them sees `int3`s.
- **The host function lives in `sg_thunk_fn[slot]`.** `sg_thunk_register(fn, name)` hands out slots at start-up
  (imports, COM vtables) or lazily (`GetProcAddress`). Their order is deterministic, so addresses are stable from
  run to run, which helps when diffing logs.

| Slot | Meaning |
|---|---|
| 0, `SG_RET_TO_HOST` | the return address `sg_call_guest` pushes |
| 1, `SG_THREAD_EXIT` | the return address at the top of a new thread's stack. Returning there runs `ExitThread(eax)` |
| 2, `SG_THUNK_UNIMPL` | the IAT value for an import with no shim. Calling it logs and aborts |
| 3–15 | reserved |
| 16 and up | imports, then dynamic APIs, then COM methods |

### 7.2 Guest → host: imports and dynamic APIs

**At load time** the runtime does what the Windows loader does:

- It writes each import's thunk address into its IAT slot. There are 495 imports from 20 DLLs, all in the
  0x13CC000–0x13CC80C IAT [obs, `data/imports.json`].
- It writes 0 into the TLS index variable.
- It calls nothing in guest code itself. The CRT entry point at 0x11E11E1 runs the initializers.

The guest then reaches a shim in one of three ways, all of which end in the same `sg_func`:

1. **`call [IAT]`**, with 402 imports used this way. When the IAT slot is known at translation time, the
   translator emits a direct host call to the shim (`SG_CALL(sg_imp_..., ra)`).
2. **`mov reg, [IAT]` … `call reg`**, with 104 imports used this way. This goes through `sg_lookup`, which finds
   the slot in the thunk region.
3. **`call thunk` where the thunk is `jmp [IAT]`.** There are 119 such thunks, at 0x6B4EB0–0x13C0FC9 [obs]; for
   example, 0x11E0784 `memcpy` has 2,309 callers. The thunk is translated as `SG_TAILCALL(sg_imp_...)`.

**Inside a shim** (`SG_IMPORT(dll, name)`):

- On entry, `[esp]` is the guest return address and the arguments follow it.
- The shim reads arguments with `SG_ARG(i)` and converts guest pointers with `SG_ARG_PTR`, after a NULL check.
- It writes `eax`, or `edx:eax` (`SG_RETURN_U64`), or pushes ST(0).
- It returns with `SG_RETURN_STDCALL(v, bytes)` or `SG_RETURN_CDECL(v)`. Both expand to `SG_RET`, so the caller's
  return check holds.

**Shim rules [dec]:**

- **Preserve** `ebx`, `esi`, `edi` and `ebp`. A shim never needs to touch them.
- **Do not keep host pointers into the window across a call to `sg_call_guest`.** The guest may free or move the
  data. Re-derive the pointer from the guest address instead.
- **`GetLastError`/`SetLastError`** read and write `TEB+0x34`.
- **Strings.** Shims read UTF-16 guest strings with our own `wcs*` helpers, because `wchar_t` on macOS is 4 bytes.
- **Structs that differ between guest and host** (`MSG`, `D3DPRESENT_PARAMETERS`, `WAVEFORMATEX`…): shims
  convert them field by field and never cast a guest struct to a host struct that contains pointers.
- **`SG_CC_X87` shims** (`_CI*`, `floor` and others) take their argument from ST(0), not the stack, and leave the
  result in ST(0):

  | CRT function | Arguments | x87 effect |
  |---|---|---|
  | `_CIsqrt`, `_CIsin`, `_CIcos`, `_CIlog`, `_CIasin`, `_CIacos` | ST(0) | replace ST(0) |
  | `_CIpow`, `_CIfmod` | ST(1) op ST(0) | pop one, replace ST(0) |
  | `floor`, `ceil` | an ordinary cdecl `double` on the stack | push the result |


**Dynamic resolution** (`GetProcAddress`, `GetModuleHandle`, about 60 names):

- A shim returns the thunk address of a known host implementation.
- For a name we deliberately do not provide (`nvapi`, `D3DKMT*`, `DasmX86Dll`), it returns NULL and sets the last
  error, as a real GOG install does. These DLLs are not shipped.

### 7.3 Host → guest: callbacks

```c
uint32_t sg_call_guest(sg_ctx *ctx, uint32_t target, enum sg_cc cc, unsigned nargs, const uint32_t *args);
```

`sg_call_guest`:

1. Pushes the arguments right to left. For thiscall, `args[0]` goes to `ecx`.
2. Pushes `SG_RET_TO_HOST`.
3. Runs `sg_lookup(target)`.
4. Checks that `eip == SG_RET_TO_HOST`.
5. For cdecl, removes the arguments. For stdcall and thiscall, checks that the callee removed them.
6. In development builds, checks that `ebx`, `esi`, `edi` and `ebp` are unchanged.
7. Returns `eax`.

It runs on the **current thread's own context and guest stack**, below the current `esp`, as a Windows callback
runs below the caller's frame.

**Re-entrancy rule [dec].** The host calls guest code only:

- from inside a shim, during a guest call to that shim, so the guest is at a call boundary and the volatile
  registers are free to clobber; or
- at the start of a thread.

There is no asynchronous injection: no signal-handler callbacks into guest code. APCs queued by `QueueUserAPC`
(at 0x922FB5) run inside the alertable-wait shims (`SleepEx` and `Wait*Ex`), which satisfies this rule.

These are the callback sites the runtime must drive [obs]:

| Callback | Shim that calls it | Convention |
|---|---|---|
| Window procedure 0x849310 | `DispatchMessageW`, `SendMessageW`, `CreateWindowExW`, and others | stdcall, 4 args |
| Low-level keyboard hook | `SetWindowsHookExA` | stdcall, 3 args |
| Thread entry points | `_beginthreadex` ×3, `CreateThread` ×1 | stdcall, 1 arg |
| `qsort` comparators, `_onexit`/`atexit`, `_initterm` tables | the CRT shims | cdecl |
| `SetUnhandledExceptionFilter` filter | not called [dec]; the native crash handler is used | stdcall, 1 arg |
| EH handlers, funclets and filters | the runtime's EH code (§10) | see §10 |
| Guest COM objects (`IDropTarget`, and `ID3DXInclude` if passed) | the relevant shim | stdcall, `this` first |

---

## 8. COM objects

### 8.1 Host objects handed to the guest [dec]

Examples: `IDirect3D9` from `Direct3DCreate9` at 0x11F84EE, `IDirect3DDevice9`, the resources, `IDirectSound`
and its buffers, `IDirectInput8`, and `ID3DXBuffer`.

```
guest record (16 bytes, 16-aligned, in the window)        guest vtable (per interface, built once)
+0  vptr ─────────────────────────────────────────────▶  [0] thunk(QueryInterface)
+4  handle   (index into the runtime's host object table) [1] thunk(AddRef)
+8  SG_COM_MAGIC 'SCOM' (checked in development builds)   [2] thunk(Release)
+12 0                                                      ... one 16-byte thunk slot per method
```

- **Guest code only ever dereferences `vptr` and calls through the slots** (`mov ecx,[eax]; push eax; call
  [ecx+8]`, §12.3).
- **Method shims are `sg_func`s.** `SG_ARG(0)` is `this`, and `sg_com_host(ctx, this)` returns the host object.
- **Building the records.** `sg_com_vtable` builds a guest vtable from an array of `sg_func*`. `sg_com_wrap`
  makes a record. An object that exposes several interfaces gets one record per interface, sharing the handle.
- **Reference counts and lifetime** are the host object's business. The guest record is freed when the host
  object dies. A stale guest pointer then hits `SG_COM_MAGIC == 0` in development builds.
- **Guest-visible memory returned by host methods** must be inside the window: `Lock`, `LockRect`,
  `ID3DXBuffer::GetBufferPointer` and `GetDesc` structs. How that interacts with Metal buffers is the
  Metal back end's concern.

### 8.2 Guest objects called by the host

**`sg_call_guest_method(ctx, obj, slot, nargs, args)`** reads `vptr` and the slot from guest memory, then calls
the method stdcall with `this` first. Examples:

- `RegisterDragDrop(hwnd, IDropTarget*)`
- possibly `ID3DXInclude` passed to `D3DXCompileShader`, if the game uses one
- the game's `IUnknown` implementations

---

## 9. Threads, TEB and TLS

### 9.1 Per-thread state

`sg_thread` has the context as its first member, so a `sg_ctx*` and a `sg_thread*` convert freely. It also holds
the TEB address, the guest stack bounds, the copy of the TLS block, a Windows-style thread id, and
runtime-private host state. `sg_self` is the host thread-local pointer to it.

`sg_thread_create(entry, param, stack_bytes, suspended)` backs `_beginthreadex` and `CreateThread`. It:

1. Allocates the guest stack (default 1 MiB) and a guard page.
2. Allocates the TEB and the TLS block.
3. Starts a host pthread with an 8 MiB host stack.
4. On that thread, pushes `SG_THREAD_EXIT` as the outermost return address and runs `entry(param)` with
   `sg_call_guest`.

"Create suspended" means the pthread waits on a semaphore until `ResumeThread`.

### 9.2 TEB layout (Windows x86 offsets)

| Offset | Field | Used by the image [obs `fs_segment_uses`] |
|---|---|---|
| 0x000 | ExceptionList (SEH chain head) | `fs:[0]` ×6,927 |
| 0x004 / 0x008 | StackBase / StackLimit | ×1 each (0x920106, 0x920110, crash handler) |
| 0x018 | Self | ×9 |
| 0x020 / 0x024 | ClientId (pid, tid) | via shims |
| 0x02C | ThreadLocalStoragePointer → array whose [0] is this thread's copy of the 12-byte image TLS template | ×11 |
| 0x030 | ProcessEnvironmentBlock → 0x7FFDF000 | not read directly |
| 0x034 | LastErrorValue | via `Get/SetLastError` |
| 0xE10 | TlsSlots[64] | via `TlsGetValue`/`TlsSetValue` |
| 0xF94 | TlsExpansionSlots → 1,024 more slots, allocated on first use | via shims |

- **`TlsGetValue` is the hottest import** (1,973 uses, 1,928 of them in Havok [obs]). Its shim body
  is `sg_tls_get`: a bounds test and one or two guest loads, with no lock.
- **The 1,088-slot limit** (64 + 1,024) matches Windows.

---

## 10. Exception-handling hand-off

### 10.1 Principle [dec]

All guest EH state stays in guest memory, as on Windows:

- the `fs:[0]` registration chain, built by translated code with plain stores
- `FuncInfo`, unwind maps and try maps in `.rdata`
- the saved-esp and state slots in each frame

The runtime implements, in host code, the few msvcr90 entry points that interpret that state:

| Entry point (import) | Called by | Runtime role |
|---|---|---|
| `_CxxThrowException` (stdcall, 2 args), 132 sites | throw sites | builds `EXCEPTION_RECORD` (0xE06D7363) and `CONTEXT` on the guest stack, then calls `sg_seh_raise` |
| `RaiseException` | `SetThreadName` (0x406D1388), at 0x922E37 and 0x11EC977 | **swallowed** for 0x406D1388 (optionally mapped to `pthread_setname_np`); other codes go to `sg_seh_raise` |
| `sg_seh_raise` (runtime) | the two above, and later hardware faults | walks the `fs:[0]` chain from `TEB+0`, calling each registration's handler with `sg_call_guest(cdecl, 4 args: pExcept, pRN, pContext, pDC)` |
| `__CxxFrameHandler3` | the 1,013 guest stubs `mov eax, FuncInfo; jmp 0x11E094C` | **receives `FuncInfo` in `eax`** (a custom convention, which the full-context ABI handles for free). Implements FH3: matches the catch type through ThrowInfo, unwinds frames above (calling their handlers with the unwind flag), runs unwind funclets, calls the catch funclet, then resumes (§10.3) |
| `_except_handler3` (10 frames) / `_except_handler4_common` (1) | SEH `__try` frames | runs filter funclets, local unwind, then resumes at the `__except` body |
| `_setjmp3` (28 sites) / `longjmp` (15) | for example 0x84DDBF | `_setjmp3` fills the guest `jmp_buf` (MSVC layout) and returns 0. `longjmp` runs the guest local unwind for frames above the saved registration, restores registers and resumes at the saved return address with `eax = val` |

### 10.2 Funclets

Unwind funclets, catch blocks and SEH filters are translated as ordinary functions. The runtime calls them the
way msvcr90's `_CallSettingFrame` does:

1. Set `ctx->ebp` to the parent frame's ebp (from the registration node).
2. Call the funclet with `sg_call_guest`.
3. Restore `ebp`.

A catch funclet returns the guest **continuation address** in `eax`. For example, the funclet at 0x67F054 ends
`mov eax, 0x67F03E; ret` [obs].

### 10.3 Landings: resuming in the middle of a translated function

**The problem.** A catch continuation, an `__except` body or the instruction after `call _setjmp3` sits inside a
translated function whose host frame is further up the host stack. The runtime must discard the host frames below
it (translated callees, shims, its own EH code), which takes a host-level non-local jump.

**The design.** Only functions that contain resume points pay for it. The translator marks them from the
generated **resume-point table** `sg_resume_points[] = {resume, func}`.

```c
SG_FUNC(0067efa0) {                    /* hypothetical function owning a resume point */
    SG_LANDING_BEGIN(0x0067efa0);      /* links a sg_landing (on the host stack) into ctx->eh_top,
                                          records frame_esp = esp at entry, host setjmp */
    ...                                /* every exit: SG_LANDING_RET(n) / SG_LANDING_TAIL(fn) */
SG_RESUME_DISPATCH:
    switch (ctx->eip) { case 0x0067f03e: goto L_67f03e; default: sg_bad_resume(ctx); }
}
```

The runtime finishes with `sg_eh_resume(ctx, target)`. Before that call it has already:

- unwound guest state, by running funclets and restoring `fs:[0]`;
- set `esp`, `ebp` and the other registers, as `_JumpToContinuation` or `longjmp` would.

`sg_eh_resume` then:

1. Looks up the owning function of `target`.
2. Walks `ctx->eh_top` from the innermost landing and picks the first landing of that function whose
   `frame_esp` is above the new `esp`. Recursive inner instances of the same function have lower `frame_esp` and
   are skipped.
3. Unlinks every inner landing.
4. Sets `ctx->eip = target` and calls the host `longjmp` (`SG_HOST_LONGJMP`).
5. The landing's setjmp branch dispatches on `eip` to the label.

**Why this is enough.** All resumed state is reloaded from `ctx` and guest memory, and translated functions keep
nothing in host locals across a resume point. The C rules on variables modified between `setjmp` and `longjmp`
therefore do not bite.

**Size [obs, `eh-resume-points.json`].** Only **11 of the 1,013 `FuncInfo` records have try blocks**, with
**11 catch handlers** in total. The other 1,002 exist only to run destructors during unwinding, and unwinding
calls funclets and needs no landing. The landing functions are therefore the 11 or so functions with catch
handlers, the callers of `_setjmp3` (28 call sites) and the 10 functions with `__except` bodies: **under 50
functions**. `__builtin_setjmp` (a few stores) instead of `setjmp` keeps the entry cost small even for those.

**Option (b) rejected [dec].** Mapping guest try blocks to host C++ `try` would need C++ for the generated code and
correct unwind tables, and it still would not cover `longjmp` or SEH. At about 50 functions it gains nothing.

### 10.4 Host frames between throw and catch

**Translated frames hold nothing**, so they are safe to discard (§5.5). **Shim frames are different**:
`DispatchMessageW` or `sg_call_guest` itself may sit between a throw and its catch, for example when the window
procedure throws to a catch in the message loop.

Rule [dec]:

- A shim that calls guest code while holding host resources must register a host cleanup record. The record is a
  small struct linked like a landing; `sg_eh_resume` runs it while unlinking.
- Where that is not practical, the shim may declare itself a **barrier**. Unwinding through a barrier aborts with
  a diagnostic.
- On Windows, exceptions propagate through `DispatchMessage` on x86 [inf].

### 10.5 Unhandled exceptions

If `sg_seh_raise` reaches the end of the chain, the runtime logs the exception and a guest backtrace, then
aborts. It does not call the guest's `SetUnhandledExceptionFilter` callback (the crash reporter, 5 sites).

### 10.6 Hardware faults

Turning SIGSEGV, SIGBUS or a divide error inside guest code into a guest `EXCEPTION_ACCESS_VIOLATION` would need a
`CONTEXT` built from `ctx` at the faulting instruction. That needs the translator to keep `ctx` precise at memory
accesses, which it does not. **Not supported [dec].** We expect the 10 `__try` frames to guard API calls, not
pointer accesses [inf].

---

## 11. Build conventions for generated code

- **Compiler flags.** C11, compiled with clang. The flags are `-O2 -fno-strict-aliasing -fwrapv -fno-math-errno
  -ffp-contract=off -Wno-pedantic`:
  - Strict aliasing is moot because of `memcpy`; `-fno-strict-aliasing` is a safety net.
  - `-fwrapv` is also a safety net. The translator still emits unsigned arithmetic only and casts to `int32_t`
    only for signed compares, `sar`, `imul` and `idiv`.
  - `-ffp-contract=off` keeps `a*b+c` from fusing into FMA where x86 rounded twice.
  - `-Wno-pedantic` allows `return f();` in `void` functions, which `musttail` requires.
- **Shift counts.** The translator masks shift counts to 5 bits before shifting in C, as x86 does, which also
  avoids C's undefined behaviour.
- **Names.** Exported symbols are `sg_f_*` for translated functions, `sg_imp_*` for import shims and `sg_*` for
  the runtime. Hand-written replacements override a translated function by defining the same `sg_f_<addr>` and
  dropping the generated one at link time.

---

## 12. Three worked examples

**How these were done.** We hand-translated each example against the header, in the form the translator emits.
The full translations derive from the game's code, so the tree does not carry them; the excerpts below are a few
lines each.

**Synthetic fixtures.** The ABI self-test contains our own fixtures with the same three shapes, together
with host→guest callbacks, an EH landing and resume, TLS, atomics, and lazy-flag tests. On an x86 host the
lazy-flag tests include a check against the CPU itself on 200,000 random operand pairs.

### 12.1 A thiscall virtual method: 0x6094A0 (vtable 0x13FB0AC, slot 3)

- **Where it comes from [obs].** The vptr is stored at 0x6094EF. The function is not in Ghidra's list; it is
  reachable only through the vtable, which makes it typical of the 32,896 vtable-only entries.
- **What it does.** It adds 0xC to `ecx`, pushes it, loads `[esp+8]` (the caller's first argument), pushes a
  `.rdata` constant, sets `ecx` to `arg0 + 0x2F4`, calls 0x944C20 (thiscall, `ret 8`), then returns with
  `ret 4`.

```c
SG_FUNC(006094a0) {
    ctx->ecx += 0xcu;  SG_PUSH32(ctx->ecx);
    ctx->ecx = sg_ld32(mem, ctx->esp + 8u);          /* arg0: [esp+4] is the return address */
    ...
    SG_CALL(SG_FN_NAME(00944c20), 0x006094b8u);      /* callee pops its 8 bytes */
    SG_RET(4);
}
```

| ABI element exercised | Result |
|---|---|
| `this` in `ecx` with no signature | ✔ an ordinary context register |
| Stack offsets through a real return-address slot | ✔ `[esp+8]` is right only because `SG_CALL` pushed the guest return address |
| Nested callee-cleanup (`ret 8` inside, `ret 4` out) | ✔ balanced without call-site knowledge |
| Reached by `call [edx+0xC]` | ✔ `SG_CALL_IND` → `sg_lookup` → bucket hit (0x6094A0 is 16-aligned) |
| Flags | ✔ `add ecx` flags are dead; nothing written |

### 12.2 An LTCG register argument: 0x5A9040

- **Where it comes from [obs].** The caller at 0x5A9315 does `lea eax,[edi+0x10]; call 0x5A9040; mov
  [edi+0xC],eax`. Ghidra types the function `undefined FUN_005a9040(void)`.
- **What it does.** It starts `push ecx` (a local slot) `; push esi ; mov esi,eax`. It makes:
  - a call to a singleton getter;
  - a 6-argument callee-cleanup virtual call `call edx`, with `ecx = this` and no `add esp` afterwards;
  - a thiscall virtual call with one argument;
  - a cdecl call followed by `add esp,4`;
  - a final virtual call.

  It returns the result in `eax` through a plain `ret`.

| ABI element exercised | Result |
|---|---|
| Register argument (`eax`) with no signature | ✔ `ctx->esi = ctx->eax` at entry |
| Mixed conventions in one body (cdecl caller-pops, thiscall callee-pops ×2) | ✔ esp bookkeeping is literal |
| `push ecx` used as stack-slot allocation, later read at `[esp+4]` | ✔ the stack is real guest memory |
| Indirect target computed in a register before the pushes | ✔ `SG_CALL_IND` evaluates first |
| Flags (`test al,al; je`) consumed locally | ✔ computed directly in C |

### 12.3 An import through a `jmp [IAT]` thunk, then a COM call: 0x11F84E0

- **Where it comes from [obs].** At 0x11F84EC the code does `push 0x20; call 0x120BB80`, and 0x120BB80 is
  `jmp [0x13CC790]`, which is `d3d9!Direct3DCreate9`. The result goes to the global 0x16F89C8, followed by
  `D3DXCheckVersion(0x20, 0x1B)` through the thunk 0x120BB86. On failure the code releases the object with
  `mov ecx,[eax]; push eax; call [ecx+8]`.

```c
SG_FUNC(0120bb80) { SG_TAILCALL(sg_imp_d3d9_Direct3DCreate9); }      /* the jmp [IAT] thunk */
SG_IMPORT(d3d9, Direct3DCreate9) { SG_RETURN_STDCALL(d3d9_create_iface(ctx, SG_ARG(0)), 4); }
/* in 0x11f84e0: */
SG_PUSH32(0x20u);  SG_CALL(SG_FN_NAME(0120bb80), 0x011f84f3u);
...
ctx->ecx = sg_ld32(mem, ctx->eax);  SG_PUSH32(ctx->eax);
SG_CALL_IND(sg_ld32(mem, ctx->ecx + 8u), 0x011f8516u);             /* IDirect3D9::Release */
```

| ABI element exercised | Result |
|---|---|
| Thunk as a host tail call; the shim sees `[esp]` = 0x11F84F3 | ✔ the return check passes because the shim's `SG_RET(4)` pops the caller's own address |
| stdcall argument cleanup by the shim | ✔ `SG_RETURN_STDCALL(..., 4)` |
| IAT value valid as data | ✔ it holds a thunk-region address; `sg_lookup` resolves it if loaded into a register |
| Host COM object returned to the guest | ✔ a 16-byte guest record. `[eax]` is a guest vtable, and `[vt+8]` is a thunk-region address resolved by `sg_lookup` through `sg_thunk_fn` |
| COM `this` on the stack (stdcall), not in `ecx` | ✔ the shim reads `SG_ARG(0)`; `ecx` holds the vtable here, as the guest code left it |

**Verdict.** All three shapes translate with the header as written, with no extra mechanism. The two places
where the examples sharpened the design are the evaluate-before-push rule for `SG_CALL_IND` and COM `this`
travelling on the stack. Both are explicit in the header comments.

---

## 13. Open questions

1. **Is a fixed host base possible on macOS** (D1)? Possible, but clang keeps `mem` in a register anyway, so a
   fixed base gives no benefit ([image-linker.md](image-linker.md) §9.1). Closed; the base stays a parameter.
2. **Is the return check worth keeping in release** (D6)? Its cost has not been measured separately.
3. **Are there code paths that are legitimately non-local** beyond EH and `longjmp`, such as `push addr; ret`
   jumps in hand-written CRT code? `sg_return_mismatch` will reveal them in the first boot; none are known
   statically.
4. **Does any shim need to re-enter guest code on a thread other than the caller's?** Examples would be a
   DirectSound notification thread or an XInput callback. The current answer is no: DirectSound is pulled by the
   game's own audio thread.
5. **Does SIGSEGV need to become a guest exception (§10.6)?** It depends on what the `__try` frames guard.

## 14. Change log

- **2026-10-12.** First version of the header and this document, with the measurement scripts and the self-test.
