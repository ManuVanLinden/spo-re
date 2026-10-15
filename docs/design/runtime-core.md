# Runtime core: TEB, TLS, threads, synchronization and exception handling

The runtime core gives the translated game the parts of Windows that sit closest to the CPU: the guest memory
window and thunk region, a Windows-layout TEB and TLS per thread, guest threads over pthreads, kernel objects,
waits and critical sections, and the MSVC exception-handling runtime (SEH dispatch, `__CxxFrameHandler3`,
`_except_handler3/4`, `setjmp`/`longjmp`) that interprets the game's own EH tables in guest memory.

Code: `runtime/core/`, with the generator of its HLE overrides in `tools/core/gen_overrides.py`. Builds on
[runtime-abi.md](runtime-abi.md) §3, §7, §9 and §10, on the MSVC x86 exception-handling model in
`tools/eh/fh3_model.py`, and on the exe's thread analysis (`tools/threads/`).

Labels: **[obs]** measured or read from a tool result, with the evidence; **[inf]** reasoned, not checked;
**[doc]** from Microsoft's documentation or the MSVC CRT's documented behaviour.

No game code runs in the tests. Each one runs our own C, our own EH tables, or our own x86 assembly translated
by the [translator](translator.md).

## 1. Summary

| Item | Value |
|---|---|
| Library | `build/core/libcore.a`. Plain C11, no dependencies beyond libc/pthreads |
| ABI entry points defined | `sg_thunk_fn[]`, `sg_thunk_register`, `sg_call_guest`, `sg_self`, `sg_thread_create`, `sg_eh_resume`, `sg_seh_raise`, `sg_bad_resume`, `sg_return_mismatch`, `sg_misaligned_atomic`, `sg_unhandled_insn`, `sg_int3`, `sg_divide_error`, `sg_cpuid`, `sg_rdtsc`, `sg_ldf80`, `sg_stf80`, plus the translator's `sg_set_mxcsr` and `sg_fxsave` |
| Import shims | **56** (`sg_core_shims[]`), of which **51** are imports in the HLE layer's table: TLS (4), threads (18), synchronization (21, including `CloseHandle`), EH (12). Every one pops exactly the bytes that table gives for it (checked at registration). Five more (`ExitThread`, `_endthreadex`, `ResetEvent`, `WaitForSingleObjectEx`, `SwitchToThread`) are provided for completeness and are not imported [obs: `test_hle_glue`] |
| Fixture tests | `make -C runtime/core test`: 4,704 checks (2026-10-15), 0 failures at -O2; also clean under ASan+UBSan (`make test-san`) and ThreadSanitizer (`make test-tsan`) [obs] |
| Mutation check | 10 single-line mutations of the EH runtime, each caught by the fixtures (§9.2) [obs] |
| Translated EH fixtures | Our own MSVC-shaped x86 (FH3 try/catch, EH3 `__except`, `_setjmp3`/`longjmp`) translated by the translator's emitter and run on the core: **27/27** checks at -O2 and -O0 (a development integration test, §9.3) [obs] |
| HLE registration | `sg_core_register_hle_shims()` replaces 51 entries of `hle_imports[]`; `hle_install_imports` then points the IAT at the core. Checked by `make test-hle` [obs] |
| Direct calls | The translator calls known imports by symbol. The HLE generator emits its stubs weak, and `runtime/core/src/core_overrides.c` (generated strong `sg_imp_*` definitions) takes over those symbols (§3.4) |
| Loader hooks | `sg_core_startup` and `sg_boot`, which the image loader's `main.c` calls. runtime/image, runtime/win32, runtime/hle and runtime/core link together with **no duplicate symbols**: the core's definitions replace the loader's weak defaults [obs] |
| Resume-point table | `tools/link/gen_link.py` reads it from the translator's generated C: **53 resume points in 49 landing functions** [obs] |

## 2. Layout and commands

```
runtime/core/
  include/sg_core.h      API for other runtime packages (init, window, handles, threads, EH, shim table)
  src/core_internal.h    private: per-thread host state, Windows error codes
  src/core_mem.c         window reservation/protection, the stack page allocator hook, PEB, sg_core_init
  src/core_call.c        thunk registry, sg_call_guest, fatal reports, logging, fault handlers
  src/core_teb.c         TEBs, TLS slots and shims, image TLS block, live-thread registry
  src/core_thread.c      threads, handle table, events/semaphores/mutexes, waits, APCs, Sleep, critical sections
  src/core_eh.c          SEH dispatch/unwind, __CxxFrameHandler3, catch guard, throw, EH3/EH4, setjmp/longjmp
  src/core_cpu.c         cpuid, rdtsc, 80-bit loads/stores, ldmxcsr, fxsave
  src/core_shims.c       sg_core_shims[]: HLE import symbol -> core function, with clean-up bytes
  src/core_hle_glue.c    sg_core_register_hle_shims() (links against runtime/hle)
  src/core_boot.c        sg_core_startup() and sg_boot(), the hooks runtime/image's main calls
  src/core_overrides.c   GENERATED (tools/core/gen_overrides.py): strong sg_imp_* for the weak HLE stubs
  tests/                 fixture harness + suites eh, seh, tls, threads, misc, bench
  tests-hle/             link test against runtime/hle
tools/core/
  gen_overrides.py       runtime/core/src/core_overrides.c
```

```
make -C runtime/core test            # the fixture suites (about 1 s)
make -C runtime/core test-san        # ASan + UBSan
make -C runtime/core test-tsan       # ThreadSanitizer, thread suite
make -C runtime/core test-hle        # link with runtime/hle, check the registration
build/core/core_tests bench          # per-operation costs (§9.4)
python3 -P tools/core/gen_overrides.py          # regenerate src/core_overrides.c (--check: verify)
```

## 3. What other packages call (interfaces)

### 3.1 Start-up sequence

The image loader's `runtime/image/src/main.c` ([image-linker.md](image-linker.md)) calls the core's entry points
in this order. It finds the first two with `dlsym`; it defines weak defaults for the last two, which the core
replaces.

| Step | Call | What it does |
|---|---|---|
| 1 | `sg_core_init(mem)` | right after the window is reserved: thunk region (0xCC, read-only), TEB region, PEB, slots 0..3 |
| 2 | `sg_core_image_tls(SG_IMAGE_TLS_INDEX_VA, SG_IMAGE_TLS_TEMPLATE, SG_IMAGE_TLS_TEMPLATE_SIZE, 0)` | after the image is mapped |
| 3 | `sg_core_startup(mem)` | after the thunk region, before `hle_install_imports`. See below |
| 4 | `sg_boot(mem, 0x11e11e1, argc, argv)` | for `--run`. See below |

`sg_core_startup(mem)` does three things:

- calls `sg_core_register_hle_shims`;
- points runtime/win32's `w32_close_foreign` at `sg_core_close_foreign`;
- routes thread stacks through runtime/win32's `w32_vm_alloc`/`w32_vm_free` when they are linked.

`sg_boot(mem, entry, argc, argv)` creates the guest main thread (1 MiB stack, TEB and TLS block; `sg_self`) and
calls the PE entry as cdecl with no arguments.

A program without the loader's `main` does the same by hand. `sg_core_image_tls` must run before the first thread is
attached, because each thread copies the template when its TEB is made. `sg_core_install_fault_handlers()` is
optional, since the loader installs its own reporter.

### 3.2 Symbols the core does not define

| Symbol | Defined by | Notes |
|---|---|---|
| `sg_dispatch_bucket[]`, `sg_dispatch_entries[]`, `sg_miss_hook` | the image linker | used by `sg_lookup` in `sg_call_guest`, thread start and the EH code |
| `sg_resume_points[]`, `sg_resume_point_count` | the image linker | `tools/link/gen_link.py` writes the table into `dispatch.c` from the translator's output |
| `hle_guest_alloc` | the Win32 layer (guest heap) | the core does not call it |

### 3.3 Hooks and shared facilities

- **Guest pages for stacks.** `sg_core_vm_alloc`/`sg_core_vm_free`:
  - **Default:** a top-down allocator in `[0x7C000000, 0x7FE00000)`, 62 MB, about 58 one-MiB stacks with their
    64 KiB guards.
  - **With the Win32 layer linked:** its `VirtualAlloc` hands out addresses up to 0x7FE00000, which overlaps
    that range. So `sg_core_startup` switches both hooks to `w32_vm_alloc`/`w32_vm_free`, and stacks then live in
    the Win32 layer's bookkeeping ([win32-layer.md](win32-layer.md)).
  - **Known difference:** the 64 KiB guard below each stack is made `PROT_NONE` directly. The Win32 layer's `VirtualQuery`
    still reports it as read/write.
- **Handle table.** One table serves every package: `sg_handle_alloc_external(tag, obj, close)`,
  `sg_handle_external(h, tag)`, `sg_handle_type(h)`, `sg_handle_close(h)`. Tags `>= SG_H_EXTERNAL` (16) belong to
  other packages. Waiting on an external handle returns `WAIT_OBJECT_0`.
- **`CloseHandle` and the Win32 layer.** The Win32 layer keeps its file, find and registry handles in its own table (0x00A00004…,
  disjoint from the core's 0x100…) and provides its own `CloseHandle`. It forwards foreign handles to
  `w32_close_foreign`, which `sg_core_startup` points at `sg_core_close_foreign`. The core's `CloseHandle`
  (used if the Win32 layer is absent or registered first) forwards unknown handles to `sg_core_close_hook` [obs: test
  `threads.close-hooks`]. Either registration order works if both hooks are wired.
- **PEB.** The core writes `BeingDebugged` 0, `ImageBaseAddress` 0x400000, `NumberOfProcessors` 1 and OS
  version 5.1 at 0x7FFDF000. The Win32 layer sets `ProcessHeap` (+0x18) if its heap wants it.
- **Barriers for callbacks (runtime-abi §10.4).** A shim that calls guest code while holding host state links an
  `sg_cleanup` record (`sg_cleanup_push/pop`). `sg_eh_resume` and `ExitThread` run every record below the target
  landing; a record with `barrier = 1` makes unwinding through it fatal. The guest platform's window-procedure
  trampoline (`sgp_guest_call` in `runtime/platform/guest/src/sgp_core.c`) declares one, so no C++ exception can
  cross `DispatchMessage` unnoticed.
- **Fatal errors.** `sg_core_fatal(ctx, fmt, …)` logs the thread, the registers and a guest `ebp` backtrace, then
  aborts. `sg_core_fatal_hook` and `sg_core_unhandled_hook` let a harness intercept.

### 3.4 Registration with the HLE layer, and direct calls

**The table.** The core's shims are named `sgc_<dll>_<name>`, so they never clash with the HLE layer's
`sg_imp_` symbols ([hle-crt.md](hle-crt.md)).

- `sg_core_shims[]` maps each HLE import symbol to its core function and its clean-up bytes.
- `sg_core_register_hle_shims()` checks the clean-up bytes against the HLE table and writes the function into
  `hle_imports[i].fn` (and `hle_dynamics[i].fn`). It does this the same way the Win32 layer's
  `w32_register_shims` does.
- `hle_install_imports` then makes those functions the IAT thunks.

This covers every call through the IAT and `GetProcAddress`.

**Direct calls.** When the IAT slot is known, the translator emits `SG_CALL(sg_imp_kernel32_TlsGetValue, ra)`
(e.g. the call before 0x452d19). That binds to the HLE layer's generated stub symbol, not to the table:

- The HLE generator emits its stubs weak.
- `runtime/core/src/core_overrides.c`, generated by `tools/core/gen_overrides.py`, defines strong `sg_imp_*`
  symbols that tail-call the core.
- It skips the symbols the HLE layer implements and those the Win32 layer overrides, so nothing is defined
  twice. `CloseHandle` is the Win32 layer's, and it reaches core objects through `w32_close_foreign`.
- `runtime/core/Makefile` switches the overrides on (`SG_CORE_OVERRIDE_HLE_STUBS`) when `gen/hle_imports.c` has
  weak stubs; `tools/link/build.py` passes `-DSG_CORE_OVERRIDE_HLE_STUBS=1` and `-DW32_OVERRIDE_HLE_STUBS=1`.
- So the hottest import, `TlsGetValue`, binds directly.

Without the overrides, a direct call to a core-provided import reaches the HLE layer's "unimplemented" stub,
which aborts and names the import.

## 4. Guest address space used by the core

| Range | Use |
|---|---|
| 0x7FE00000–0x7FEFFFFF | thunk region, committed at init, filled with 0xCC, then read-only. Slots 0–2 as in runtime-abi §7.1; **slot 3 is the catch guard handler** (§7.3) |
| 0x7FF00000–0x7FFDEFFF | TEB pages (4 KiB each, 223 pages); TLS expansion pages come from the same pool. Committed once at init (host pages are 16 KiB) |
| 0x7FFDF000 | PEB |
| 0x7C000000–0x7FDFFFFF | default thread stacks: 1 MiB (or the requested size, rounded to 64 KiB) plus a 64 KiB `PROT_NONE` guard below |

Inside each TEB page, beyond the Windows fields of runtime-abi §9.2: `ThreadLocalStoragePointer` points at
TEB+0xFA0, whose entry 0 points at TEB+0xFB0, this thread's copy of the image TLS template (12 bytes; up to
80 bytes fit).

## 5. TEB and TLS

- **TEB fields set [obs: `tests/test_tls.c`]:** ExceptionList = 0xFFFFFFFF, StackBase/StackLimit = the guest
  stack, Self, ClientId (pid 0x1000, tid), ThreadLocalStoragePointer, PEB, LastErrorValue.
- **Thread ids** are multiples of 4 from 0xF04, as on Windows.
- **TLS slots:** 64 inline (TEB+0xE10) and 1,024 expansion slots (TEB+0xF94, one page allocated on the first
  `TlsSetValue` above 63), 1,088 in all.
  - `TlsAlloc`/`TlsFree` use a global bitmap. Both clear the slot in every live thread, so a reused index
    reads 0 [doc: TlsFree].
  - `TlsGetValue` clears the last error on success [doc: TlsGetValue]; callers use that to tell a stored 0 from
    an invalid index.
  - The `TlsGetValue` shim is `sg_tls_get` plus that store: **5.7 ns** per call measured through `SG_CALL`
    (§9.4).
- **`__declspec(thread)`:** `sg_core_image_tls` writes 0 to the image's TLS index (0x16F2618) and records the
  template (0x171F000, 12 bytes). Each new thread copies it into its TEB page [obs: test checks the copy and that
  a thread's write does not reach another thread].

## 6. Threads and synchronization

### 6.1 Threads

- `sg_thread_create(entry, param, stack, suspended)` allocates the guest stack and guard, the TEB, and a thread
  kernel object, then starts a detached pthread with an 8 MiB host stack.
- The pthread waits while suspended, then pushes `param` and `SG_THREAD_EXIT` and calls `entry` through
  `sg_lookup`.
  - When the stdcall entry returns, `eip` must be `SG_THREAD_EXIT`, and `eax` is the exit code.
  - `ExitThread`/`_endthreadex` from any depth longjmp to the thread's start. Host cleanup records run first.
- **At exit:**
  - the thread object is signalled;
  - mutexes the thread still owns are released;
  - the TEB, TLS pages and guest stack return to their pools.

  The `sg_thread` itself is freed when the last handle closes.
- **Shims:**

  | Group | Shims and behaviour |
  |---|---|
  | Creation | `CreateThread`, `_beginthreadex` (`CREATE_SUSPENDED` = 4, `thrdaddr`/`lpThreadId` written) |
  | Lifetime | `ResumeThread` (returns the previous count), `GetExitCodeThread` (`STILL_ACTIVE` = 259) |
  | Identity | `GetCurrentThread`/`GetCurrentProcess` (pseudo handles −2/−1), `GetCurrentThreadId`/`ProcessId` |
  | Handles | `DuplicateHandle` (a pseudo handle becomes a real one; `DUPLICATE_CLOSE_SOURCE`) |
  | Priorities | `Set/GetThreadPriority`, `SetPriorityClass`, `GetPriorityClass` (stored and reported back only) |
  | Affinity | `SetThreadAffinityMask`, `SetThreadIdealProcessor`, `Set/GetProcessAffinityMask` (masks for the host's cores, up to 32) |

  Priorities and affinity do nothing else, because macOS has no affinity API [inf].
- `RaiseException(0x406D1388)` ("SetThreadName") is swallowed. It names the thread for logs and, for the current
  thread, with `pthread_setname_np`.

### 6.2 Kernel objects and waits

- **Objects.** Events (manual and auto-reset), semaphores, mutexes (recursive, owner-only release:
  `ERROR_NOT_OWNER`) and thread objects. All their state is guarded by one host mutex.
- **Named objects** share one namespace. Creating an existing name returns a new handle to the same object with
  `ERROR_ALREADY_EXISTS` (183), which single-instance checks rely on [doc: CreateMutex]. The name disappears with
  the object's last handle [obs: test].
- **`sg_core_wait`** backs `WaitForSingleObject(Ex)` and `WaitForMultipleObjects(Ex)`, for both any and all.
  - Each waiting thread sleeps on its own condition variable, and only waiters whose set contains a signalled
    object are woken.
  - Timeouts use absolute deadlines. A 100 ms wait returned after 95–400 ms in every run [obs].
  - Results are `WAIT_OBJECT_0+i`, `WAIT_TIMEOUT` (0x102), `WAIT_IO_COMPLETION` (0xC0) or `WAIT_FAILED`, with
    `ERROR_INVALID_HANDLE`/`ERROR_INVALID_PARAMETER`.
- **APCs.** `QueueUserAPC` queues to the target thread and wakes it if it is in an alertable wait.
  `SleepEx(…, TRUE)` and `Wait*Ex(…, TRUE)` run the queued APCs on their own thread, as stdcall with one
  argument, and return 0xC0 [obs: test checks the APC's thread id].
- **Not modelled:** `WAIT_ABANDONED` (an exiting owner's mutex is released) and the signalled state of file
  handles. The thread analysis found the game using neither [inf].

### 6.3 Critical sections

- The core works on the guest's own `CRITICAL_SECTION` (24 bytes):
  - `OwningThread` (+12) holds the owner's tid; a thread claims it with a compare-and-swap.
  - `RecursionCount` (+8) counts re-entry.
  - `LockCount` (+4) reads −1 when free and −2 when held.
  - `LockSemaphore` (+16) holds our waiter count.
- **Contended path.** After 64 spins (`yield`), a thread parks on one of 256 host mutex/condition buckets chosen
  by address. The waiter increments the count before its CAS and the releaser reads it after its store, both
  sequentially consistent, so no wake-up is lost.
- **Costs [obs, §9.4]:**
  - uncontended Enter+Leave: 11 ns
  - 8 threads × 20,000 contended Enter/Leave pairs (with recursion): about 30 ms, giving exactly 160,000 plain
    increments
- `LeaveCriticalSection` by a non-owner is logged and ignored. On Windows that corrupts the lock.

### 6.4 Memory ordering (required of the runtime whatever the translator's policy)

- **Every synchronization shim is a full barrier.** This covers:
  - events, semaphores, mutexes and waits
  - critical sections, whose sequentially consistent CAS/store pair acts as one
  - `Sleep`, `SleepEx` and `SwitchToThread`
  - thread creation and resume
  - `QueueUserAPC`

  Each issues `__atomic_thread_fence(__ATOMIC_SEQ_CST)` on entry and exit, besides the host locks.
- The HLE layer's `Interlocked*` shims already use sequentially consistent atomics [obs: `runtime/hle/src/crt_misc.c`].
- `TlsGetValue`/`TlsSetValue` are per-thread and unordered.
- **Test [obs]:** a writer thread does 500 rounds of 64 plain guest stores, then `SetEvent`; the reader waits,
  checks all 64 words, then acknowledges. **0 stale words.** ThreadSanitizer reports no race in the core.
- **Not the core's job:** acquire/release on the translated code's own plain accesses, which the translator
  emits ([memory-ordering.md](memory-ordering.md)).

## 7. Exception handling

The implementation follows the MSVC x86 model in `tools/eh/fh3_model.py` step for step and reads every
structure from guest memory with the documented MSVC x86 layouts. Funclets, filters, handlers, copy constructors,
destructors and the EH4 cookie check are guest code, called through `sg_core_call`.

### 7.1 Dispatch and unwind

- **`sg_seh_raise(code, flags, n, params)`**
  1. Builds a CONTEXT (integer registers, `eip` = the return address at `[esp]`) and an `EXCEPTION_RECORD`
     on the guest stack below `esp`.
  2. Walks `fs:[0]`, calling each handler as cdecl with `(rec, rn, ctx, 0)`. Each registration must lie inside
     the thread's stack, as `RtlDispatchException` checks.
  3. `ContinueExecution` returns (fatal if the exception is noncontinuable). Reaching the end of the chain logs
     the code, the C++ type name (from the CatchableTypeArray), the address and a backtrace, then aborts.
- **`sg_seh_unwind(target, rec)`** (RtlUnwind): calls each handler above `target` with `UNWINDING`
  (+`EXIT_UNWIND` for the end of the chain) and pops `fs:[0]` after each.
  - A NULL `rec` builds a `STATUS_UNWIND` record, as `_global_unwind2` does.
  - A target not on the chain is fatal.

### 7.2 `__CxxFrameHandler3` and `_CxxThrowException`

- **The handler** takes the FuncInfo from `eax` (the stub convention) and accepts magic
  0x19930520–0x19930522.
  - **Unwinding call:** `__FrameUnwindToState(−1)` stores each next state before calling its action with
    `ebp = rn+0x0C` [obs: tested].
  - **Search:** walks the try map for the current state, with the `TypeMatch` rules: `catch (...)`, name or
    pointer equality, reference-only, const/volatile/unaligned.
  - **Catch:**
    1. builds the catch object at `rn+0x0C+disp` (reference, simple type, or the copy constructor as
       thiscall, with the extra `1` argument for a virtual base);
    2. global unwind to `rn`;
    3. local unwind to `tryLow`;
    4. sets state `tryHigh+1`;
    5. pushes a catch guard;
    6. runs the catch funclet;
    7. pops the guard and destroys the exception object;
    8. `_JumpToContinuation`: `esp = [rn−4]`, `ebp = rn+0x0C`, `fs:[0] = rn`, then `sg_eh_resume`.
- **`_CxxThrowException(obj, ti)`** builds the 0xE06D7363 record. `obj = ti = 0` rethrows the innermost caught
  exception, or terminates if there is none.
- No `_set_se_translator` is imported, so SEH exceptions never reach C++ catch blocks.

### 7.3 The catch guard

- **Layout.** A 16-byte registration `{next, handler = slot 3, rn, 0}` on the guest stack. The thread's
  "in flight" list records `(obj, ThrowInfo, guard)`.
- **Search phase:** it continues the search.
- **Unwind phase** (a new exception or `longjmp` leaves the catch block): it destroys the caught object unless
  the dispatch in progress carries that same object, which is a rethrow. So each object is destroyed exactly
  once, and **before** the catch block's own locals are unwound, the MSVC order [obs: fixture `eh.throw-from-catch`
  logs throw X, catch, throw Y, dtor X, catch-local dtor, outer catch, dtor Y].

### 7.4 `_except_handler3` / `_except_handler4_common`

- **EH3** reads the scope table at `[rn+8]`; **EH4** reads `[rn+8] ^ [cookie]`, with records after the 16-byte
  header and −2 as the outermost level.
- **EH4 cookies.** The GS cookie (if `GSCookieOffset != −2`) and the EH cookie are checked first and again
  before the transfer. The check calls the guest `__security_check_cookie` with the value in `ecx` [obs: test
  `seh.cookie-mismatch` smashes the EH cookie in a filter and the check sees exactly one mismatch].
- **Search:**
  1. Stores `EXCEPTION_POINTERS*` at `rn−4`.
  2. Calls the filters with `ebp = rn+0x10`.
  3. A negative result means `ContinueExecution`; zero moves to the enclosing level; positive goes on to step 4.
  4. Global unwind (EH3 with a fresh `STATUS_UNWIND` record, EH4 with the exception's record).
  5. Local unwind to the matching level, running inner `__finally` blocks.
  6. `trylevel = enclosing`, `ebp = rn+0x10`, `esp = [rn−8]`, `fs:[0] = rn`, then `sg_eh_resume(handler)`.
- **Unwind call:** runs the `__finally` blocks down to the outermost level.

### 7.5 `_setjmp3` and `longjmp`

- **`_setjmp3`** stores, in the `_JUMP_BUFFER` field order:
  - `Ebp`, `Ebx`, `Edi`, `Esi`
  - `Esp` (at entry, pointing at the return address)
  - `Eip` (the return address)
  - `Registration`, `TryLevel`
  - the `VC20` cookie and `UnwindFunc` (0 for count 0)

  It returns 0.
- **`longjmp(buf, val)`:**
  1. Global unwind to the saved registration, with a `STATUS_LONGJUMP` record.
  2. If `UnwindFunc` is set it is called; buffers without the cookie get an EH3 local unwind.
  3. Restores the registers, sets `esp = saved + 4`, `eax = val ? val : 1` and `fs:[0]`, then
     `sg_eh_resume(Eip)`.

  The `+4` and the cookie handling are [inf] readings of the MSVC x86 CRT's `longjmp`. Both are no-ops for the
  game's 28 count-0 sites, which own no frames.

### 7.6 Landings

- **`sg_eh_resume(ctx, target)`:**
  1. Finds the owner of `target` in the resume table (binary search).
  2. Takes the innermost landing of that owner whose `frame_esp` is above the new `esp`, which skips deeper
     recursive instances [obs: fixture `eh.recursion`].
  3. Runs the host cleanup records below it.
  4. Unlinks the inner landings, sets `eip` and `_longjmp`s.
- **Fatal cases:** an unknown target, or no live landing.
- **Costs:** landing entry and exit (a host `_setjmp`) 5.7 ns; a full throw-catch-resume through
  `__CxxFrameHandler3` 136 ns [obs, §9.4].

### 7.7 Other EH-related imports

| Import | Behaviour |
|---|---|
| `RaiseException` | 0x406D1388 swallowed (§6.1); other codes go to `sg_seh_raise`, and the call returns if a filter continues |
| `_XcptFilter` | logs and returns `EXCEPTION_CONTINUE_SEARCH` |
| `SetUnhandledExceptionFilter` | records the filter and never calls it (runtime-abi §10.5) |
| `UnhandledExceptionFilter` | returns 0 |
| `terminate` | fatal |

## 8. CPU helpers

| Helper | Behaviour |
|---|---|
| `sg_cpuid` | A fixed SSE2-class CPU: vendor `GenuineIntel`, family 6 model 15, EDX = FPU TSC MSR CX8 CMOV CLFSH MMX FXSR SSE SSE2, no SSE3, one logical processor, a brand string. Which long-tail code paths run depends on this profile [inf] |
| `sg_rdtsc` | A nominal 3 GHz counter from `mach_absolute_time` |
| `sg_ldf80`/`sg_stf80` | Exact conversion between the 80-bit format and host doubles, including infinities and NaN. Extra mantissa bits round to double |
| `sg_set_mxcsr` | Sets FPCR.FZ from FTZ\|DAZ and FPCR.RMode from RC |
| `sg_fxsave` | Writes a plausible 512-byte image: FCW, FSW with TOP, the abridged tag word, MXCSR, `MXCSR_MASK` 0xFFFF (DAZ supported), ST0–7, XMM0–7 |
| `sg_divide_error`, faults | Abort with a report, by design. No guest SEH conversion |

## 9. Verification

### 9.1 Fixture suites (`runtime/core/tests/`, all our own code and tables)

| Suite | Checks | What it covers |
|---|---:|---|
| `eh` | 4,100 | the 12 behaviours of the EH model's tests, plus a virtual-base copy, a const-qualified throw refused by a non-const handler, and 1,000 repeated catches (no leak of landings or in-flight entries) |
| `seh` | 69 | EH3 filter and `__except`; EH4 nested `__finally` inside `__except`, with cookie checks; a C++ throw through an EH3 filter that declines and an EH4 `__finally` (model test 11); `ContinueExecution`; noncontinuable plus `−1` is fatal; SetThreadName; `longjmp` with 0 and 7 across a frame with destructors (registers and `esp` restored); host cleanup records and barriers; bad resumes; a smashed EH cookie |
| `tls` | 270 | TEB fields, the image TLS block, 80 TLS indices across the inline/expansion boundary, free/realloc clearing, invalid indices |
| `threads` | 140 | create/join/exit codes, `_beginthreadex` suspended + `ResumeThread`, 8-thread critical-section contention, `TryEnter`, auto/manual events, timeouts, `WaitForMultiple` any/all/invalid, semaphores (1,000 hand-offs, maximum enforced), named recursive mutexes, APC delivery, `ExitThread` from a nested call, C++ EH on a worker thread, message passing (§6.4), `DuplicateHandle`, 64 threads started and joined with pool reuse, external handles, the `CloseHandle` hooks shared with the Win32 layer |
| `misc` | 30 | cpuid, rdtsc, f80 round trips, thiscall/fastcall through `sg_call_guest`, stdcall clean-up/callee-saved/return-address/dispatch-miss/`SG_THUNK_UNIMPL` diagnostics, thunk registration, the shim table |

Each test calls shims exactly as translated code does: it pushes the arguments and a return address, calls,
then checks `eip`, the stack clean-up and the callee-saved registers. The EH fixture functions follow the exact
MSVC frame shapes and the translator's emission pattern (`SG_LANDING_BEGIN` … `SG_RESUME_DISPATCH`).

### 9.2 Are the fixtures strong enough? [obs]

We built 10 single-line mutations of `core_eh.c`, each from clean, and ran the suite on each. It fails on all 10:

| Mutation | Result |
|---|---|
| guard destroys rethrown objects | fail |
| outermost instead of innermost landing | fail |
| no local unwind before `__except` | fail |
| `longjmp` `esp` off by 4 | fail |
| state stored after the action | fail |
| catch object at `rn+8` | fail |
| exception destroyed before the catch block | fail |
| const check dropped | fail |
| EH4 cookie check skipped | fail |
| no global unwind before the catch | fail |

The suite catches the fifth mutation only because the unwind funclets record the frame state they see.

### 9.3 Translator-generated code (a development integration test, not part of this source tree) [obs]

- **Fixtures.** We assemble our own x86 in MSVC's shapes with clang's i386 assembler and translate it with
  `sgrecomp`'s CFG and `Emitter`, with the three resume points as landing roots: the path `translate.py` takes
  for the game. The fixtures are:
  - an FH3 frame with a handler stub and a catch funclet, catching through an intermediate FH3 frame whose
    unwind funclet must run;
  - an EH3 frame with a filter reading `GetExceptionCode` through `[ebp−14h]` and an `__except` body that starts
    `mov esp,[ebp−18h]`;
  - `_setjmp3`/`longjmp` through a frame with an unwind funclet.
- **Run.** The generated C links with `libcore.a` and runs 3 rounds of each scenario.
- **Result:** 27/27 checks pass at -O2 and -O0. That covers the return values, the exception object destroyed
  once with the thrown value, the middle frame's funclet run with the right `ebp`, the registers restored by
  `longjmp`, and no leaked `esp`, `fs:[0]` or landings.

### 9.4 Costs (Apple M4 Max, shared with other jobs; `core_tests bench`) [obs]

| Operation | Cost |
|---|---:|
| `TlsGetValue` via `SG_CALL` (push, call, return check) | 5.7 ns |
| `Enter`+`LeaveCriticalSection`, uncontended | 11.3 ns |
| `SetEvent`+`WaitForSingleObject`, signalled | 37.6 ns |
| `sg_call_guest` to an empty function | 9.0 ns |
| landing owner entry+exit (host `_setjmp`) | 5.7 ns |
| C++ throw → catch → resume | 136 ns |

### 9.5 Sanitizers [obs]

| Run | Result |
|---|---|
| ASan + UBSan (-O1) | all checks; no reports |
| ThreadSanitizer, full suite | no reports; the owning-thread check in `LeaveCriticalSection` uses an atomic load |

## 10. Known limits and open points

1. **Catch-guard slot.** The guard is fixed at thunk slot 3 (runtime-reserved), so import slot numbers stay as
   the HLE layer assigns them.
2. **`_setjmp3` always writes the `VC20` cookie**, though the game's count-0 buffers need none. Both readings
   make `longjmp`'s local unwind a no-op for the game's sites. Writing the field avoids reading uninitialised
   bytes. Not yet confirmed against msvcr90 [inf].
3. **The CONTEXT record carries only the integer registers.** A handler that edits it and returns
   `ContinueExecution` has no effect beyond returning. No handler in the image does that (the 5 that return −1
   belong to SetThreadName, which is swallowed) [obs].
4. **WAIT_ABANDONED and thread priorities are not modelled** (§6.2, §6.1).
5. **One kernel-object lock.** With the game's ~25 threads we do not expect it to matter [inf].
6. **Not done:**
   - mapping `SetThreadPriority` to QoS classes;
   - `SuspendThread`/`TerminateThread`/`GetThreadContext` (not imported);
   - converting host faults into guest SEH.
