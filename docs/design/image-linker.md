# Image loader, dispatch table and whole-image link

The translated C and the runtime become one arm64 executable, `build/sporenative/spore`. At start it maps the
game's exe into the guest memory window (the exe's bytes are data here, never executed), installs the import table,
and looks guest addresses up in a dispatch table to find the C function that stands for them. §9 has the guest-window
and compile-scale measurements behind the design.

The packages: `runtime/image/` (loader, thunks, miss hook, import overrides, weak runtime defaults, `main`) and
`tools/link/` (`gen_link.py`, `build.py`). Every generated file and every binary is under `build/sporenative/`
(git-ignored); the link reports stay there too.

Labels: **[obs]** measured (commands in §8); **[inf]** reasoned.

The smoke run and the self-test start the linked binary, map the image and install the IAT, and **call no game
code**: the self-test calls `sg_lookup` (table reads only) and, in `--probe-miss`, the miss hook with a made-up
caller. Only `--run` hands over to the game's entry point.

## 1. Summary

- **The whole image links into one arm64 executable** [obs]: `build/sporenative/spore`, 510 TUs of translated C plus
  runtime/image, runtime/hle, runtime/core and runtime/win32. Link **0.19–0.25 s wall, 0.8 CPU-s, 370–420 MB RSS**.
  Binary **97.9 MB** (`__text` 86.1 MB); 124.4 MB with the image embedded (measured on 2026-10-12).
- **`sg_lookup` resolves every manifest entry** [obs]: the self-test walks all 93,825 manifest functions, 55 labels,
  495 IAT slots and 66 GetProcAddress names, plus every bucket and 93,827 `dladdr` name checks, with 0 failures in
  both binaries (§4.1).
- **Smoke run** [obs]: SHA-256 verified, 7 sections mapped with page protections, 495 IAT slots, 525 thunk slots in
  use, exit 0, in **15–20 ms** in-process (5 ms without the hash; 1 ms with the embedded image mapped).
- The loader can read the exe or an embedded copy; §3.2 measures both.

## 2. What the binary contains

```
build/sporenative/spore
  translated code      build/sporenative/obj/<src>/*.o     every SG_FUNC of the translator output
  runtime/image        loader, thunks, miss hook, imports, weak defaults, main
  runtime/hle          import shims + generated import table (src/*.c, gen/hle_imports.c)
  runtime/core, win32  every .c/.m outside tests/, when they compile and link (else dropped, see §5)
  gen/dispatch.c       sg_dispatch_entries[93,827], sg_resume_points[53]       (C, -O1)
  gen/buckets.s        sg_dispatch_bucket[1,034,997] via .incbin                (__TEXT,__const)
  gen/image.s          spore-embed only: the exe as mapped, 0x400000-0x1d10000 (__SPIMG,__image, 16 KiB aligned)
```

- **Objects, never archives.** `build.py` links the runtime as object files, so a strong definition in runtime/core or
  runtime/win32 always replaces a weak default from runtime/image. With an archive, the linker would not pull a
  member only to replace a weak symbol that is already defined.
- **Link flags:** `-Wl,-stack_size,0x4000000` (64 MiB main-thread stack, runtime-abi 5.2) and `-lm`. The embedded
  variant adds `-Wl,-segprot,__SPIMG,r,r`.

## 3. The loader

### 3.1 Address space

| Guest range | Host protection | Note |
|---|---|---|
| 0–0xFFFF | none | null guard (never committed) |
| 0x400000–0x403FFF | r | headers and the first `.text` bytes share a 16 KiB host page |
| `.text` 0x401000–0x13CBF2A | r | readable, never executed |
| `.rdata` 0x13CC000–0x150B5DE | rw while the IAT is filled, then r | 0x13CC000 is 16 KiB-aligned, so `.text` and `.rdata` share no host page |
| `.data` 0x150C000, `.tls` 0x171F000 | rw | `.tls` shares a host page with the end of `.data` |
| `CONST` 0x1720000–0x172001F | rw | shares the host page 0x1720000–0x1723FFF with the start of `.rsrc`, which therefore is rw for 12 KiB (§9.1) |
| `.rsrc` 0x1724000–, `.reloc` 0x1C28000–0x1D0D05E | r | |
| 0x1D0E000–0x1D0FFFF | r | **past SizeOfImage but in the image's last host page.** The guest heap starts at `SG_IMAGE_MAPPED_END` = 0x1D10000, not at 0x1D0E000 |
| 0x7F000000–0x7FDFFFFF | rw on demand | only when no runtime/win32 is linked: the weak `hle_guest_alloc` bump allocator for runtime-owned guest data |
| 0x7FE00000–0x7FEFFFFF | r | thunk region, `0xCC` bytes (runtime/core maps it if linked; the loader's init is idempotent) |

The self-test checks the protections with `mach_vm_region` at 18 addresses (§4).

### 3.2 Three ways to get the bytes [obs]

The PE's FileAlignment is 0x200, so its sections cannot be `mmap`ed from the exe at their virtual addresses (file
offsets must be page-aligned). The loader offers three sources instead:

| Source | How | Cost |
|---|---|---|
| `file` (default for `spore`) | `mmap` the exe read-only, SHA-256 it (CommonCrypto), `memcpy` each section into the committed window | table below |
| `embed` | `memcpy` from a pre-laid-out copy linked into the binary | table below |
| `embed-mmap` (default for `spore-embed`) | `mmap(MAP_FIXED \| MAP_PRIVATE)` the binary's own file at the blob's 16 KiB-aligned offset: no copy, demand paging, copy-on-write for `.data` | table below |

Measured on the linked binaries (the self-test output of a 512-unit link; machine shared, load 3–6):

| Variant | Load step | Of which SHA-256 | Whole start-up to "smoke run complete" (in-process) | Binary |
|---|---:|---:|---:|---:|
| `file`, SHA-256 verified (default) | 14–20 ms | 12–17 ms | 14–21 ms | 97.9 MB |
| `file`, `--no-verify` | 4–5 ms | — | 4.2–5.1 ms | 97.9 MB |
| `embed` (copy) | 17–22 ms | — | 18–22 ms | 124.4 MB |
| `embed-mmap` | 0.02–0.15 ms (pages arrive on first touch) | — | 0.8–1.1 ms | 124.4 MB |

- **The copy itself costs 2–4 ms** for 25 MB; the SHA-256 dominates the `file` path. `embed-mmap` defers the cost to
  first touch, page by page, and keeps `.text`/`.rsrc` pages clean and shared with the file cache [inf].
- **The first launch of a freshly linked binary takes about 1.2–1.4 s of wall time** before `main` (every later run:
  10–30 ms) [obs: `tests.smoke.wall_s` vs `smoke_noverify`]. This is the system's first-run check of a new 90–116 MB
  executable [inf]; the loader plays no part.
- **The default:** `build/sporenative/spore` reads the user's exe at run time (`file`) and verifies its hash, in
  about 15 ms. No game bytes go into the binary, so a rebuilt binary is never a second copy of the exe, and a
  different exe stops the start with an error. `spore-embed` (`--embed`) stays available for later: it saves about
  15 ms per start and makes the binary self-contained, at +26.5 MB (`__SPIMG` = 26,279,936 bytes, 0x400000–0x1D10000
  laid out).

### 3.3 Loader duties beyond copying (runtime-abi 7.2)

- Writes 0 into the TLS index variable 0x16F2618. The TLS callback array is empty [obs, the PE TLS directory], so nothing is
  called.
- Calls no guest code. The CRT entry 0x11E11E1 runs the initializers once `--run` hands over to `sg_boot`.
- Checks base 0x400000, SizeOfImage 0x190E000 and entry 0x11E11E1, and (for `file`) the exe's SHA-256.

## 4. Dispatch table

- **The table comes from the C the binary is built from.** `gen_link.py` scans the translator output for
  `SG_FUNC(xxxxxxxx)` definitions, so the table cannot list a function that is not linked. It also reads each
  landing function's `SG_RESUME_DISPATCH` switch to produce `sg_resume_points[]` (resume address → the function
  that lands it), the table runtime-abi §10.3 says is "generated with the dispatch table".
- **Entries [obs]:** 93,827 = the 93,825 manifest functions (93,674 canonical + 151 unreferenced) + the two promoted
  labels 0x864780 and 0x11F8DF0. No manifest function is untranslated. Flags: `SG_DE_FUNC` on all;
  `SG_DE_SECONDARY` also on the 2 promoted labels and the manifest's shared tails (srcbits 256).
- **Buckets [obs]:** 1,034,997 (one per 16 bytes of `.text` plus the sentinel). 942,721 are empty, 90,774 hold one
  entry, 1,450 two and 51 three; 90,572 entries (96.5%) are 16-aligned.
- **Labels.** The manifest has 55 labels. Three are function entries (0x864780, 0x11F8DF0 and 0xC2E640, which the
  manifest already lists as a function). The other 52 are EH resume points (9 catch continuations, 28 `setjmp` returns and
  15 `__except` bodies: 11 SEH3, 4 SEH4). **They are not dispatch entries, by design** (runtime-abi §6.1:
  "a computed jump to one would be a bug"); they resolve through `sg_resume_points` and `sg_eh_resume`. The
  translator emits 53 resume points: the 52 labels plus 0x11E152F, an SEH filter continuation that the manifest
  has as a block of its own function.
- **Miss hook** (`sg_image_miss_hook`, installed as the weak `sg_miss_hook`): prints the address, whether it is
  inside `.text` (with the nearest entry below), an empty thunk slot, or not code; whether it is a resume point and
  of which function; the caller (`[esp]`, the return address an indirect `call` pushed before the lookup; for an
  indirect `jmp` it is the enclosing function's return address) and the function containing it; `ctx->eip`; and the
  `ebp` chain, read with `mach_vm_read_overwrite` so a corrupt chain cannot fault. It appends
  `<addr> caller=<ra> eip=<eip>` to `$SPORE_MISS_LOG` (default `build/sporenative/misses.txt`) for the next
  translation run, then aborts. `SPORE_MISS=continue` returns a stub that returns 0 (cdecl) instead.

### 4.1 Self-test (`spore --selftest build/sporenative/gen/entries.txt`)

1. Table shape: sorted, unique, inside `.text`, non-null; **every** bucket equals the first entry at or above its
   start; sentinel = count.
2. Symbol identity: `dladdr` on every entry's host function must name `sg_f_<guest>`.
3. Every manifest function resolves through `sg_lookup` (a counting miss hook detects misses) to its own entry;
   every manifest label is an entry or a resume point with a landing function.
4. Every IAT slot holds its import's thunk address, and `sg_lookup` on it returns that import's shim; the two data
   imports hold their guest variables.
5. Every GetProcAddress name in runtime/hle's dynamic table resolves (`hle_resolve`) to a thunk that `sg_lookup` maps to
   its shim; the names left out on purpose resolve to 0.
6. `entry + 1` misses for every entry not followed by another entry (negative test).
7. Page protections at 18 addresses (§3.1), and the thunk region reads as `0xCC`.

Result [obs, `spore --selftest`, both binaries, with and without runtime/core and runtime/win32 linked]:
`93827 dispatch entries (93827 named by dladdr), 93825 manifest functions, 55 labels (3 entries, 52 resume
points), 495 imports, 66 dynamic names (31 absent), 53 resume points; 0 failures`, 7.3–8.3 s (almost all of it
`dladdr`). A `--probe-miss` run (§8) exercised the miss hook: 0x401001 reports "inside .text, nearest entry below
0x00401000" with the fake caller 0x00401005 and writes `misses.txt`; 0x84DDC4 is reported as "an EH resume point of
0x0084ddb0"; 0x864780 resolves without a miss.

## 5. Imports and the other runtime packages

- `sg_image_install_imports` calls runtime/hle's `hle_install_imports` (493 thunks in IAT order, 2 data variables), then
  registers every dynamic name in table order, so GetProcAddress addresses are stable from run to run. 35 of the
  66 dynamic names have shims (9 share a static import's thunk); the other 31 are left out on purpose and resolve to NULL.
- **Overrides without editing runtime/hle.** `sg_import_override(dll, name, fn)` and
  `sg_import_override_symbol("sg_imp_kernel32_TlsAlloc", fn)` replace an import's shim (and the dynamic-table
  entries that share it) before installation. runtime/win32 has its own equivalent (`w32_register_shims`), which
  `main` calls.
- **Start-up order in `main`** (the optional packages are found with `dlsym` on the executable itself, so
  `main.c` needs neither header and links with or without them):
  1. reserve the window; fault reporter (SIGSEGV and SIGBUS)
  2. `sg_core_init(mem)` if runtime/core is linked; `w32_init(mem, defaults)` if runtime/win32 is
  3. map the image; `sg_core_image_tls(0x16F2618, 0x171F000, 12, 0)`; `w32_vm_mark_image` per section
  4. `sg_thunks_init` (idempotent; fills slots 0–2 only if empty); weak hooks `sg_core_startup`/`sg_win32_startup`;
     `w32_register_shims`
  5. imports and dynamic names; `w32_after_install`
  6. final protections; then `--smoke` exits, `--selftest` walks the tables, `--run` calls `sg_boot(mem, 0x11E11E1,
     argc, argv)`
- **Weak defaults** (`weak_runtime.c`): every `[runtime]` symbol that generated code, runtime/hle or the loader
  references has a weak definition. The fatal reports (return mismatch, `int3`, untranslated instruction, `#DE`,
  misaligned atomic, bad resume) and the 80-bit conversions are complete as defaults; the rest (`sg_cpuid`, `sg_rdtsc`, `sg_set_mxcsr`, `sg_fxsave`, `sg_call_guest`, `sg_eh_resume`, `sg_seh_raise`,
  `sg_thread_create`, COM) print "not provided by runtime/core" and abort. `build.py` reports which defaults
  survive a link (`weak_defaults_in_use`).
- **Optional packages.** `build.py` compiles runtime/core and runtime/win32 per package. If one fails to
  compile, or the link fails with it, that package is dropped (recorded under `packages_dropped`) and the image
  links with the weak defaults; `--strict` turns that into an error.

Measured on a 512-unit link with all four packages [obs]: runtime/win32 swapped in 122 import shims (0 stack
clean-up mismatches against runtime/hle's table), and strong definitions from runtime/core and runtime/win32
replaced 20 weak defaults (`hle_guest_alloc`, `sg_call_guest`, `sg_cpuid`, `sg_eh_resume`, `sg_thread_create`,
`sg_thunk_register`, ...). `sg_miss_hook` stays runtime/image's by design; `sg_boot` and the COM entry points come
from the other runtime packages when they are linked, and their weak defaults abort if reached. A link without
runtime/core or runtime/win32 (either fails to compile) still passes the smoke run and the self-test with the weak
defaults.

runtime/core declares `sg_thunk_fn` as a tentative (common) definition [obs]; ld64 prefers runtime/image's
weak *initialised* definition of the same size, so the array lives in runtime/image's object. Both packages use the one
symbol, so behaviour is identical; making core's a real definition (`= {0}` or `-fno-common`) would make it win.

## 6. Build pipeline and measurements

A full build [obs, a 512-unit link; load average 25–48 during the run, so times are high]:

| Step | Wall | CPU | Peak RSS | Output |
|---|---:|---:|---:|---|
| Translate (`translate.py --all --chunk 184`, cached on translator source hash) | 94 s | 1 process | ~1.1 GB | 510 files, 526 MB of C |
| Compile translated C, -O2, 8 jobs | 194 s | 1,251 s (max TU 23 s) | 873 MB per clang | 102 MB of objects |
| Compile runtime (35 files) | 1.4 s | 5 s | 75 MB | |
| `gen_link.py` (scan 526 MB of C, manifest, tables) | 3 s | | 430 MB | dispatch.c 6 MB, buckets 4.1 MB |
| Compile link support (`dispatch.c` at -O1) | 1.5 s | | 303 MB | 4.9 MB |
| **Link** `spore` | **0.25 s** | 0.86 s | 371 MB | **97,922,168 B** |
| **Link** `spore-embed` | **0.19 s** | 0.76 s | 420 MB | **124,407,432 B** |

Segments of `spore`: `__TEXT` 90.5 MB (of which `__text` 86.1 MB and the 4.1 MB bucket table), `__DATA_CONST`
1.5 MB (the 93,827-entry table with function pointers, rebased by dyld), `__DATA` 4.5 MB, `__LINKEDIT` 5.4 MB.

Comparison runs [obs]:

| Source | TUs | Compile wall / CPU | Peak RSS per clang | Objects | Binary | `__text` |
|---|---:|---:|---:|---:|---:|---:|
| `build/recomp/all` (the translator's own chunking) | 188 | 179 s / 1,179 s, max TU 37 s | 1,109 MB | 101.7 MB | 97.8 MB | 86.0 MB |
| retranslated, first run of the session | 510 | 177 s / 1,209 s | 746 MB | 93.5 MB | 89.5 MB | 77.8 MB |
| retranslated, final | 510 | 194 s / 1,251 s | 873 MB | 102.3 MB | 97.9 MB | 86.1 MB |

- The `__text` difference between the two 510-TU runs comes from the translator, not the split: `recomp/` changed
  between them, so the cache stamp triggered a retranslation. At equal translator versions, 188 and
  510 TUs give the same code size within 0.1% [obs]. The first run matches the 77.8 MB of the scale measurements
  (§9.2) exactly.
- At this load, 188 and 510 TUs cost the same CPU; 510 TUs halve the largest TU (37 → 23 s) and lower peak memory
  per process, as the scale measurements predict (§9.2). 510 is the default.
- **An incremental rebuild after a runtime change** recompiles only the changed runtime files, regenerates the tables and relinks in
  about 4.5 s; with the smoke run and the self-test, 13 s [obs: touched `runtime/image/src/imports.c`]. Stamps cover flags, the headers of every `-I` directory, and each source's size
  and mtime.

## 7. Notes

1. **`sg_boot`.** `spore --run` calls `sg_boot(mem, 0x11E11E1, argc, argv)`, which runtime/core defines (it attaches
   the guest main thread and calls the CRT entry); runtime/image's weak default only prints that no boot path is
   linked.
2. **The image's last host page** (0x1D0C000–0x1D0FFFF) is read-only, so a guest allocation at 0x1D0E000 would
   fault on the first write. `spore_guest.h` therefore defines `SG_HOST_PAGE` and
   `SG_IMAGE_MAPPED_END` (0x1D10000), where the guest heap starts; runtime/win32's 64 KiB granule makes 0x1D10000 the
   first possible allocation anyway [inf].
3. **Indirect jumps.** The translator lowers 1,364 `jmp r32`/`jmp [b+d]` sites to `sg_lookup` tail calls. Any of them
   that is an intra-function jump reaches the miss hook at run time; `misses.txt` collects them for the
   next translation.
4. **Start-up wiring by `dlsym`.** `main.c` finds `sg_core_init`, `sg_core_image_tls`, `w32_init`,
   `w32_register_shims`, `w32_vm_mark_image` and `w32_after_install` by name, so it does not depend on those packages'
   headers. If a signature changes there, `main.c` must change with it. `w32_init` gets an all-zero `w32_config`
   (all defaults).
5. **Not measured:** an idle-machine rerun of compile times; `-g` and `-O1` builds of the whole image (`--opt -O1`
   works but was not timed); a strip of the binary for size.

## 8. Reproduce

```
python3 -P tools/link/build.py --tus 512 --embed      # retranslate (cached), compile, link, smoke + self-test
python3 -P tools/link/build.py --src build/recomp/all  # the 188-file chunked translator output instead
python3 -P tools/link/gen_link.py --src build/recomp/all --out build/sporenative/gen   # tables only (3 s)
build/sporenative/spore --smoke [--source file|embed|embed-mmap] [--no-verify]
build/sporenative/spore --selftest build/sporenative/gen/entries.txt
SPORE_MISS_LOG=/tmp/m.txt build/sporenative/spore --quiet --probe-miss 84ddc4   # miss-hook report, aborts
```
Reports: `build/sporenative/link-report*.json`, never copied into `data/`.

## 9. The guest window and the compile scale on macOS arm64

*Measured before the image linker was written, with small probe programs of our own (a window probe, its Metal
variant and a code-generation probe) and the whole program compiled but never run. Machine: M4 Max, Apple clang 21, ld-1267, shared with other jobs, so
the times are upper bounds; the sizes reproduce byte for byte.*

### 9.1 The guest window

- **The window's base varies [obs].** `mmap(NULL, 4 GiB + 64 KiB, PROT_NONE, MAP_ANON|MAP_PRIVATE|MAP_NORESERVE)` always
  succeeds (4–8 µs; 64 such windows coexist in one process) and costs no memory until committed. Its base is
  64 KiB-aligned in general; over-reserving 8 GiB and unmapping the excess guarantees a 4 GiB-aligned base. That
  is why `mem` is a parameter (runtime-abi §3.1) and not a constant.
- **Nothing can be mapped below 4 GiB [obs]:** `MAP_32BIT` fails, and `MAP_FIXED` at 0x10000, 0x400000 and
  0x8000_0000 fails with `ENOMEM`. `__PAGEZERO` cannot be shrunk: a binary linked with `-pagezero_size` below
  about 3.94 GiB is killed at `exec` (exit 137; the threshold moves between runs). Guest address == host
  address is therefore impossible on arm64 macOS, and every access must go through `mem + (uint32_t)a`.
- **Fixed low addresses are refused [obs]** (`mach_vm_allocate(VM_FLAGS_FIXED)`, the safe call: it returns
  `KERN_NO_SPACE` on a live mapping instead of replacing it): `KERN_INVALID_ADDRESS` below 4 GiB (`__PAGEZERO`),
  `KERN_NO_SPACE` at 0x2_0000_0000 (dyld shared cache), 0x8_0000_0000 (malloc's large region) and
  0x10_0000_0000–0x70_0000_0000 (a `PROT_NONE` GPU carve-out present from launch). Above 0x70_0000_0000 is free;
  0x100_0000_0000 (1 TiB) would do if a fixed base were ever wanted. It is not: compiled with a constant base,
  the probe loop still keeps the base in a register (one extra `mov`, no instruction saved), so a fixed base
  brings nothing [obs, codegen probe; runtime-abi §13 question 1].
- **`MAP_FIXED` replaces existing mappings without warning** (no `MAP_FIXED_NOREPLACE` on macOS) [obs]. The runtime
  uses it only inside its own reservation, for decommit.
- **Metal does not interfere [obs]:** with a device and 8 × 64 MiB shared buffers, the same table holds; Metal's
  CPU mappings sit near 0x1_1xxx_xxxx and its `gpuAddress` space is separate.
- **16 KiB pages [obs].** `mprotect` needs 16 KiB-aligned addresses (`EINVAL` at 0x401000), and a 4 KiB length is
  rounded up to the host page. Win32's 4 KiB protection granularity cannot be reproduced: `VirtualProtect` on a
  4 KiB page applies to its 16 KiB host page (the 64 KiB allocation granularity keeps allocations aligned), a
  `PAGE_GUARD` stack page becomes a 16 KiB `PROT_NONE` page that the 4 KiB stack probe still hits, and four TEBs
  share one host page. In the image, `.rsrc` at 0x1721000 shares the host page 0x1720000–0x1723FFF with RW
  `CONST`, so 12 KiB of `.rsrc` lose write protection (§3.1). `SG_HOST_PAGE` and `SG_IMAGE_MAPPED_END` follow
  from this.
- **Commit and decommit [obs]:** commit is `mprotect(PROT_READ|PROT_WRITE)` (first touch 0.6–1.0 µs per page);
  `madvise(MADV_FREE_REUSABLE)` returns the footprint at once, and `mmap(MAP_FIXED, PROT_NONE)` over a range of
  our own window drops it and resets protection, the closest match to `VirtualFree(MEM_DECOMMIT)`.
- **Faults arrive as SIGBUS [obs].** Every access to a `PROT_NONE` page of the window (guest 0x0, 0xFFFC, a write
  to a read-only page, 0x8000_0000, 0xFFFF_FFFC, a 4-byte read at 0xFFFF_FFFE straddling the top, 0x1_0000_FFFC
  in the 64 KiB guard) raises **SIGBUS, not SIGSEGV**, with the exact `si_addr`; `si_addr − mem` is the guest
  address. The fault handler (§5, step 1) therefore catches both signals.

### 9.2 Compile and link at whole-program scale

- **Size of the job [obs].** 93,674 canonical functions, 4,726,387 guest instructions (the translator: 93,827
  functions with promoted and EH entries, 0 unhandled instructions, 95 s and 1.1 GB to translate).
- **Splitting.** Compile cost grows about linearly with the amount of C (about 0.13 ms of CPU per guest instruction at
  -O2), so the split is chosen for memory and rebuild latency. clang's peak RSS grows about 130–175 MB per MB of C
  in a TU (plus about 250 MB); a single TU of the whole program would need about 35–40 GB [inf]; each extra TU
  costs about 20 ms of process overhead. About 512 TUs, contiguous by address and balanced by instruction count,
  each declaring only what it uses, works best [obs + inf], hence the 510 of §6. The largest function
  (0x7960f0, 12,272 instructions) dominates its TU at any split.
- **-O2 at 512 TUs [obs]:** 500–630 CPU-s, 66–80 s wall at 8 jobs, under 0.9 GB per clang; 78 MB of `__text`
  (about 16.5 bytes, or 4 arm64 instructions, per guest instruction); link 0.1 s in 340 MB. **-O1:** 85 MB of
  `__text`, 500 CPU-s.
- **Never build the whole image at -O0 [obs].** The whole program at -O0 is 3.4 GB of `__text`, and Apple's default linker
  (ld-prime) asserts ("up to 16 clusters supported", its branch-island limit, about 2 GB of `__text` [inf]);
  `-ld_classic` links it in 2.7 s and 7 GB. Use -O1 for development builds and -O0 only per TU.
- **Host frames [obs, `-fstack-usage`, all functions at -O2]:** median 48 bytes, p90 80, p99 128, maximum 608; 45%
  use no stack. The guest stack lives in the window, so the 64 MiB main-thread stack holds more than 500,000
  nested guest calls at p99 [inf].
- **Code generation [obs, our fixture read as assembly]:** clang keeps a loop's ctx registers in host registers and
  writes them back once at the exit (`SG_RESTRICT` on `ctx` and `memcpy` accesses on `mem` allow it); a guest
  load is one `ldr w, [x1, w, uxtw]`; `SG_TAILCALL` is a single `b` (`musttail`); a landing function's
  `SG_LANDING_BEGIN` is a 272-byte frame and a `bl __setjmp`.
