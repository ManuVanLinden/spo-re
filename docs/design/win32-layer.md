# Win32 layer: CRT start-up, guest heap, files, registry and paths

The Win32 layer is the part of KERNEL32, ADVAPI32, SHELL32, VERSION and msvcr90 that touches the process and the
file system: the CRT start-up and exit, guest virtual memory and heaps, the C `FILE` functions, a virtual `C:`
drive mapped onto the Spore folder and Application Support, the registry, module and version queries, directory
watching and NLS. Sections 3–9 describe the process and file functions, sections 15–18 directory watching, NLS,
process imports and version strings; §10 explains how the shims are registered, §12 lists the tests, and §22
records what the game's boot asks of the layer.

Code: `runtime/win32/` and its generator `tools/win32/gen_shims.py`. ABI: [runtime-abi.md](runtime-abi.md). Shim
conventions: [hle-crt.md](hle-crt.md).

Labels: **[obs]** observed in the exe, measured or tested here, with the evidence; **[doc]** taken from
Microsoft's documentation (sources in §13); **[inf]** inferred, not yet checked.

The tests run our own C against a guest window; none of the game's code runs in them. To check the
version-resource layout (§9), a Python script read the exe's header, resource directory and version block
statically. Those are the only game bytes read, and tracked files keep nothing from them but the version number.

## 1. Summary

- **Shims:** 170 `sg_func` shims (`gen_shims.py`), 149 of them for imports of this exe. The 147 of sections 3–9 by
  source file, imported/total [obs]: CRT start-up and exit 14/15, heap/malloc/Global/memory status 18/26,
  VirtualAlloc family 3/4, files/paths/folders/volumes 43/44, stdio and CRT file functions 20/20,
  modules/environment/system 17/18, registry 7/7, version/resources/identity/event log 13/13. The 12 not
  imported (`HeapCreate`, `HeapReAlloc`, `VirtualProtect`, `calloc`, `_msize`, operator new/delete, `atexit`, …)
  are implemented and tested too.
- **Start-up coverage:** of the 279 imports the game calls before its first frame, 83 are this layer's and 83
  runtime/hle's; the rest are threads, synchronisation, TLS and exceptions (runtime/core) and the platform
  (USER32/GDI/IMM/WINMM/DSOUND).
- **Tests:** `make -C runtime/win32 test` (§12).
- **Allocator speed:** 20.7 ns per small alloc-or-free, 27 ns per `malloc(64)`+`free` pair, 11.7 µs per
  100 KB alloc+free [obs: `w32_tests --bench`, shared machine].
- **Calls by symbol:** translated code calls imports by symbol (`SG_CALL(sg_imp_msvcr90_malloc, …)`), so a
  table swap alone does not reach most call sites; runtime/hle's generated stubs are weak and this layer
  links strong forwarders over them (§10).

## 2. Layout, build and start-up order

```
runtime/win32/
  include/w32.h        public API: init, VM, heaps, handles, paths, registry, strings, environment
  src/w32_int.h        private: Win32 error codes, module initialisers, helpers
  src/w32_vm.c         VirtualAlloc/Free/Query/Protect over the guest window (section 3)
  src/w32_heap.c       heaps, malloc family, GlobalAlloc/LocalAlloc, hle_guest_alloc, memory status (4)
  src/w32_crt.c        msvcr90 start-up and exit imports (5)
  src/w32_module.c     modules, GetProcAddress, command line, environment, system information (6)
  src/w32_path.c       Windows path normalisation, mount table, case-insensitive resolution (7)
  src/w32_file.c       KERNEL32 files, find, directories, folders, volumes; the handle table (7)
  src/w32_stdio.c      msvcr90 stdio, _stat64i32, _wgetcwd (7.5)
  src/w32_reg.c        registry over a JSON store; w32_json.[ch] reader (8)
  src/w32_res.c        version resources, EnumResourceNamesA, user/computer names, event log (9)
  src/w32_util.c       UTF-8/UTF-16/1252, errno mapping, FILETIME
  src/w32_init.c       w32_init, w32_register_shims (10)
  src/w32_shims.h      GENERATED: shim declarations and the (DLL, export, symbol, clean-up) list
  src/w32_overrides.c  GENERATED: strong sg_imp_* forwarders, compiled in when runtime/hle's stubs are weak
  tests/               harness + 7 test files (host C only)
tools/win32/gen_shims.py   generates the two files above; checks every shim's clean-up bytes
```

Commands (repository root):

```
python3 tools/win32/gen_shims.py        # after adding or changing a W32_IMPORT
make -C runtime/win32 test              # build build/win32/libw32.a (+ build/hle/libhle.a) and run the tests
make -C runtime/win32 test-cs           # same, with a case-sensitive APFS disk image attached
build/win32/w32_tests --bench           # allocator timings
```

**Start-up order for the host program** (`runtime/image/src/main.c` follows it):

1. Reserve the 4 GiB window `PROT_NONE` ([image-linker.md](image-linker.md) §9.1).
2. `w32_init(mem, &cfg)`: VM bookkeeping, the runtime, process and CRT heaps, paths, environment and command
   line, file handles, stdio, registry. It must come before anything calls `hle_guest_alloc`.
3. `w32_register_shims(&mismatches)`: swaps our shims into `hle_imports[].fn` and the matching
   `hle_dynamics[]` entries. Expect 0 mismatches.
4. Map the image; call `w32_vm_mark_image(addr, size, PAGE_*)` per section if `VirtualQuery` should
   report section protections (the whole image is pre-registered as `MEM_IMAGE`).
5. `hle_install_imports(mem)`, then `w32_after_install(mem)`, which points the `_acmdln` data import at
   the command line.
6. Give each thread a TEB and stack from `w32_vm_alloc_runtime` / `w32_vm_alloc` (runtime/core does).
7. Run the entry point 0x11e11e1.

`w32_config` fields (all optional, each with an environment variable): `game_dir` (`SPORE_GAME_DIR`,
default `./game`), `support_dir` (`SPORE_SUPPORT_DIR`, default `~/Library/Application Support/spore-native`),
`documents_dir` (`SPORE_DOCUMENTS_DIR`, default `<support>/Documents`), `temp_dir` (`SPORE_TEMP_DIR`),
`registry_file` (`SPORE_REGISTRY`), `install_win` (default `C:\Spore`), `exe_rel`
(`SporebinEP1\SporeApp.exe`), `args` (`SPORE_ARGS`), `persist_registry`, `std_handles`
(`SPORE_W32_STDHANDLES`). Host variables `SPORE_ENV_<NAME>=v` appear to the guest as `<NAME>=v`.

## 3. Guest virtual memory

The map follows runtime-abi §3.2. `VirtualAlloc` hands out `[0x00010000, 0x7FE00000)` minus the image;
the thunk region, TEBs and PEB above it are reserved for the runtime and handed out only by
`w32_vm_alloc_runtime`.

- **Bookkeeping** is per 4 KiB guest page (state, protection, type) and per 64 KiB granule (allocation
  base, size, `AllocationProtect`): 1.5 MB of host memory [obs].
- **Windows rules implemented [doc: VirtualAlloc, VirtualFree, VirtualQuery]:**
  - reservations are 64 KiB-aligned; a reserve at an address rounds it down to 64 KiB; commits are
    page-granular and must lie inside one reservation (`ERROR_INVALID_ADDRESS` otherwise);
  - `MEM_COMMIT` with a NULL address reserves too; `MEM_TOP_DOWN` searches from the top;
    `MEM_RESET` keeps the contents (allowed: it only permits discarding);
  - `MEM_RELEASE` needs the allocation base and size 0; `MEM_DECOMMIT` with size 0 decommits the whole
    allocation;
  - `VirtualQuery` returns runs of pages with equal state, protection and type, `MEM_FREE` runs
    outside allocations, 0 above 0x7FFF0000, and `ERROR_BAD_LENGTH` for a short buffer;
  - `VirtualProtect` needs committed pages in one allocation and a non-NULL `lpflOldProtect`
    (`ERROR_NOACCESS`).
- **Host pages are 16 KiB** [obs: `getconf PAGESIZE`]. A host page's protection is the union of its
  committed guest pages' protections. Consequences:
  - a decommitted 4 KiB page next to a committed one stays host-accessible (more permissive than
    Windows; only fault diagnostics differ);
  - when it is recommitted it is **zeroed**, which Windows guarantees for new commits [doc] and EA's
    allocator, which reserves with `MEM_RESERVE` and commits pieces (0x926eed, VirtualQuery state test at
    0x92747f [obs]), may rely on;
  - a host page with no committed guest page is replaced by a fresh `PROT_NONE` mapping, which frees
    its memory and makes it read as zeros later.
- **Nothing executes code from the window**, so `PAGE_EXECUTE_*` maps to read or read/write.
- **Deviation:** `PAGE_GUARD` pages are mapped `PROT_NONE`, so a touch is a fatal fault instead of a
  one-shot `STATUS_GUARD_PAGE_VIOLATION`. Thread stacks should be committed in full. [inf: the
  game does not set guard pages itself; not checked]
- The tests check that faults happen: a forked child touching a decommitted, read-only or
  no-access page dies with SIGBUS [obs].

## 4. Heaps and the msvcr90 malloc family

- **Allocator.** Every block has a 16-byte header in guest memory (requested size, size class, heap
  handle, magic). Requests up to 32 KiB use 44 size classes (16-byte steps to 256, then four per power of
  two) with a LIFO free list per class per heap, carved from 1 MiB segments committed through the VM.
  Larger requests get their own `VirtualAlloc` region and are released on free. One mutex per heap.
- **Exact sizes:** `HeapSize` and `_msize` return the requested size, as on Windows [doc: HeapSize].
- **Heap handles** are guest addresses of a small control block, like Windows' heap base addresses.
  `GetProcessHeap` and msvcr90's private `_crtheap` are different heaps, as in msvcr90 [doc: heapinit].
- **msvcr90 rules:** `malloc(0)` returns a unique 1-byte block; `realloc(NULL, n)` is `malloc`;
  `realloc(p, 0)` frees and returns NULL; requests above `_HEAP_MAXREQ` fail with `ENOMEM`; `calloc`
  checks the multiplication. `_aligned_malloc` stores the raw pointer below the aligned block, as msvcr90
  does; a non-power-of-two alignment fails with `EINVAL`.
- **`HeapReAlloc`** stays in place while the block's class fits (or the large region's pages do);
  `HEAP_REALLOC_IN_PLACE_ONLY` fails otherwise; `HEAP_ZERO_MEMORY` zeroes the growth.
- **Invalid and double frees:** the header (magic and heap handle) catches them; they are logged at WARN and
  refused instead of corrupting the heap.
- **`GlobalAlloc`/`LocalAlloc`** return fixed process-heap memory; a moveable "handle" is the pointer and
  `GlobalLock` returns it unchanged.
- **`hle_guest_alloc`** (the runtime-owned guest data runtime/hle needs) comes from a third, private heap,
  zero-filled, any power-of-two alignment.
- **Memory status:** `GlobalMemoryStatusEx` reports the host's physical memory; `GlobalMemoryStatus`
  clamps to 2 GiB as Windows does for programs that are not LARGE_ADDRESS_AWARE [doc]; total virtual is
  0x7FFE0000 and available virtual is the VM's free space.
- **Deviations:** user pointers are 16-aligned (Windows gives 8); `HeapCreate`'s maximum is not
  enforced; `HEAP_NO_SERIALIZE` is ignored (always locked); small-class memory is reused but never
  returned to the VM; `HEAP_GENERATE_EXCEPTIONS` logs instead of raising.
- **Imports:** this exe imports only `HeapAlloc`, `HeapFree` and `GetProcessHeap` from the Heap family,
  and `malloc`, `free`, `realloc` and `_aligned_*` from msvcr90. No operator new/delete, `calloc` or
  `_msize` import exists; the game's own allocators sit on these and on `VirtualAlloc` [obs:
  `data/hle/imports-table.json`].
- **Tests:** class boundaries, reuse, invalid/double/wrong-heap frees, realloc in place and moving, large
  blocks released, `HeapDestroy`, 250,000 randomised operations against a shadow model (contents and
  sizes verified, no overlaps) and 8 threads × 100,000 operations on the process heap.

## 5. msvcr90 start-up and exit imports

What the CRT entry does [obs: disassembly at the addresses given]:

| Site | Call | Shim behaviour |
|---|---|---|
| 0x11e1161 | `__set_app_type(2)` | stored |
| 0x11e116c | `_encode_pointer(-1)` into `__onexitbegin`/`__onexitend` (runtime/hle: identity) | — |
| 0x11e1180 / 0x11e118e | `*__p__fmode() = _fmode`, `*__p__commode() = _commode` | guest `int`s; stdio reads `_fmode` for the default text/binary mode |
| 0x11e119c | reads `_adjust_fdiv` through its IAT slot | runtime/hle's guest `int` 0 |
| 0x11e11c0 | `__setusermatherr(0x11e1585)` | stored, never called back |
| 0x11e11d7 | `_configthreadlocale(-1)` | runtime/hle's shim accepts it; pre_c_init returns 0, so `_initterm_e` passes |
| 0x11e0fb0 | `_initterm_e(0x13eb09c, 0x13eb0ac)` | table holds 0, 0x11e1100, 0x11e1344, 0x11e139a [obs]; calls each non-NULL entry cdecl, stops at the first non-zero result |
| 0x11e0fd9 | `_initterm(0x13cc80c, 0x13eb098)` | 31,266 constructors; calls each non-NULL entry through `sg_call_guest` |
| 0x11e0f05 | `__getmainargs(&argc, &argv, &envp, _dowildcard, &startinfo)` | §5.1 |
| 0x11e078a | `atexit`: `__onexitbegin` decodes to -1 in an exe, so it calls `_onexit`; `__dllonexit` is the DLL branch [obs: 0x11e07a8] | both implemented |
| 0x11e1070 / 0x11e10d3 | `exit` / `_cexit` after `WinMain` | §5.2 |

- **Progress counters for a boot harness:** `w32_initterm_done` (entries called), `w32_initterm_current` (the one
  running, 0 between calls) and an optional `w32_initterm_hook(index, fn)` before each call.
- **runtime/hle shims checked, not replaced:** `_amsg_exit` and `_invoke_watson` log the guest return address
  and abort (msvcr90 shows R60xx and exits); `_encode_pointer`/`_decode_pointer` are the identity, which
  keeps the -1 sentinel intact; `_controlfp_s(NULL, _PC_53, _MCW_PC)` (0x11e155d–0x11e156a) is valid;
  `_crt_debugger_hook` is a no-op. All match what the start-up code needs [obs: `runtime/hle/src`].

### 5.1 `__getmainargs`

- **argv** follows msvcr90's `parse_cmdline` [doc: "Parsing C command-line arguments"; VS2008 rules]:
  the program name ends at the closing quote with no escapes; in arguments, 2n backslashes before `"`
  give n backslashes and toggle quoting, 2n+1 give n and a literal `"`, and `""` inside quotes is a
  literal `"`. All five examples of Microsoft's table pass, plus empty arguments, tabs and 1252
  characters.
- **One allocation** holds the pointer array and the strings, from the CRT heap, as msvcr90 does.
- **envp** comes from the w32 environment block (§6), converted to 1252.
- Wildcard expansion (`_dowildcard`) is logged and not done [inf: the exe's flag is 0].

### 5.2 `_onexit`, `__dllonexit`, `exit`, `_cexit`, `_exit`, `_lock`

- `_onexit` appends to a host-side table. `exit` and `_cexit` run it last-in first-out through
  `sg_call_guest`; a handler that registers another handler during exit gets it run too (msvcr90
  re-reads its table) [doc: atexit].
- `__dllonexit(fn, &begin, &end)` keeps a module's table in guest memory and grows it with `realloc`
  using `_msize`, as msvcr90's `onexit.c` does.
- `exit` runs the table, flushes stdio, then calls `w32_exit_hook(code)` (default: host `exit`).
  `_exit` skips the table. A hook that returns aborts the process; a harness can `longjmp` out.
- `_lock`/`_unlock` use 64 recursive mutexes.

## 6. Modules, environment, command line, system information

- **Module handles.** The exe is 0x00400000. Any other module that exists gets a 64 KiB reservation whose
  first page holds a minimal PE header (`MZ`, `PE\0\0`, i386, no exports), so handles look like image
  bases. A module exists if runtime/hle knows it (`hle_module_known`: a static-import DLL or one with a provided
  dynamic name) or it is `ntdll`. `GetModuleHandle` finds static-import DLLs and ntdll at once, and
  others only after `LoadLibrary`, as on Windows.
- **`GetProcAddress`** resolves through runtime/hle's `hle_resolve` by the module's name; absent names (TGGetOS,
  nvapi, D3DKMT, DasmX86, Ras*) return NULL with `ERROR_PROC_NOT_FOUND`. The TGGetOS probe at
  0x6b8130 gets a real ntdll handle and then NULL, so `$cider` stays false [obs:
  test].
- **How `$cider` is evaluated [obs: boot runs and lldb].** The probe 0x6b8130 calls `GetModuleHandleA("ntdll")`,
  then `GetProcAddress(…, "TGGetOS")`, and returns `al = 1` only if `TGGetOS()` exists and returns `"MacOSX"`
  (otherwise `xor al,al` at 0x6b818c). Its only caller, the system-info builder 0x6b8630 (call at 0x6b8a23), stores
  the result at record +0x9c; 0x7cb640 turns the record into a float array at `[this+0x74]` (0.0/1.0 for flags,
  and numbers such as `driverBuild` = `(build>>16)*10000 + (build&0xffff)`); 0x7c7bd0 publishes entries 0..0x20 as
  ConfigManager variables under the names of the `{name, id}` table at 0x14113f0, where 0x14114d8 is
  `{"cider", 0x1d}`. lldb at `sg_f_007c7f50` read `array[0x1d]` = 0.0 and `array[0x4]` (driverBuild) = 117741, the
  null device's NVIDIA profile. `ConfigManager.txt` uses `$cider` at lines 52, 297, 527, 557 and 670: with it false
  the game skips the Mac-wrapper block (`AlwaysFullscreen`, `RenderTargetCorrection false`, `MacSpecificText`,
  `Support51Audio false`), runs the vendor and driver-build checks, keeps instancing and does not take the
  `NumFramesToBuffer 3` path. `ConfigManager.txt` itself runs only after `Direct3DCreate9` (its reader 0x7c93d0 is
  slot +0x18 of the singleton `[0x15fd8b8]` that init step 18 creates after the renderer). The `$cider` answer is a
  row of [DEVIATIONS.md](../DEVIATIONS.md).
- **`GetModuleFileNameA/W`**: `C:\Spore\SporebinEP1\SporeApp.exe` for the exe, `C:\Windows\system32\<name>.dll`
  for others; Vista+ truncation (returns nSize, `ERROR_INSUFFICIENT_BUFFER`) [doc].
- **Command line:** `"C:\Spore\SporebinEP1\SporeApp.exe" <args>`; `GetCommandLineA/W` and the `_acmdln`
  data import point at guest copies.
- **Environment:** a Windows 7-like user environment (`APPDATA`, `TEMP`, `windir`, `NUMBER_OF_PROCESSORS`,
  …), case-insensitive names, sorted like Windows' block. `GetEnvironmentVariableA` returns the length,
  or the needed size including the NUL, or 0 with `ERROR_ENVVAR_NOT_FOUND` [doc]. Start-up depends on this:
  EA's singleton manager publishes its pointer as `SingleMgrMutex<pid>` = `%I64x` and reads it back
  [obs: 0x935b02–0x935b96]. msvcr90 `getenv` is runtime/hle's and returns NULL.
- **System information:** `GetSystemInfo` (4 KiB pages, 64 KiB granularity, host CPU count capped at 16,
  `SPORE_NCPU` to override), `GetVersion`/`GetVersionExA/W` (Windows 7 SP1, 6.1.7601), `GetStartupInfoA`
  (no `STARTF_USESHOWWINDOW`, so WinMain gets `SW_SHOWDEFAULT`), `IsDebuggerPresent` (0).
  [inf: Windows 7 is the newest version contemporary with the game's checks; not checked against them]

## 7. Paths and files

### 7.1 The Windows view

The guest sees one drive, `C:`, with these mounts (longest prefix wins, compared case-insensitively on
component boundaries):

| Windows path | Host | Notes |
|---|---|---|
| `C:\Spore` (`{app}`, configurable) | `game_dir` | the Spore folder holding `SporebinEP1\`, `Data\`, `DataEP1\`, `bp1content\` |
| `C:\Users\Player\Documents` | `documents_dir` | `My Spore Creations` lives here |
| `C:\Users\Player\AppData\Roaming` | `<support>/AppData` | so `%APPDATA%\Spore` is `<support>/AppData/Spore` |
| `C:\Users\Player\AppData\Local\Temp` | `temp_dir` | `GetTempPathW` |
| `C:\` | `<support>/drive_c` | everything else, including `C:\Windows`, `C:\ProgramData` |

- **Documents default to `<support>/Documents`, not `~/Documents`,** so development runs do not create
  `My Spore Creations` in the user's real Documents folder. `SPORE_DOCUMENTS_DIR` points it at `~/Documents`
  instead.
- **The working directory** is the exe's folder, as a GOG shortcut sets it, so `..\Data\…` relative paths
  resolve under the install root.
- **Normalisation** follows `GetFullPathName` [doc: "File path formats on Windows systems"]: `/` becomes
  `\`; relative paths, `C:relative` and `\rooted` paths use the current directory; `.` and `..` fold (`..`
  stops at the root); trailing dots and spaces are dropped from the last component; `\\?\` is stripped.
  UNC and device paths (`\\.\C:`) are not representable: `CreateFile` on them fails with
  `ERROR_ACCESS_DENIED`, so the volume-extents probe (0x6b8240) gets no handle.
- **Case-insensitive lookup.** Each existing component is first tried as given, then matched by scanning
  the directory with a fold of ASCII, Latin-1, Latin Extended-A pairs, Greek and Cyrillic. New names keep
  the guest's spelling below the real parent. The default macOS volume is case-insensitive [obs:
  `diskutil`], so the scan runs only on case-sensitive volumes; `make test-cs` tests it on an APFS image
  [obs: 8 checks]. **Deviation:** Unicode normalisation (NFC/NFD) is not folded [inf: harmless with APFS,
  which preserves what was written].

### 7.2 Handles

File, find and std handles share one table: values `0x00A00004 + 4k`, multiples of 4 like NT handles, in
a range other packages should avoid. `CloseHandle` on any other value calls `w32_close_foreign(ctx, h)`,
a hook runtime/core sets for its threads, events and mutexes (default: log once, return TRUE). `CloseHandle` on a
find handle fails, as on Windows; `FindClose` owns those.

### 7.3 `CreateFile`, I/O and names

- **`CreateFileA/W`**: all five dispositions with Windows' errors (`ERROR_FILE_EXISTS` for `CREATE_NEW`,
  `ERROR_ALREADY_EXISTS` as a success code for `CREATE_ALWAYS`/`OPEN_ALWAYS` on an existing file,
  `ERROR_PATH_NOT_FOUND` for a missing directory) [doc: CreateFile]; directories open only with
  `FILE_FLAG_BACKUP_SEMANTICS` (the directory watcher's open at 0x92fdc8); `FILE_FLAG_DELETE_ON_CLOSE`.
  Sharing modes are not enforced.
- **`ReadFile`/`WriteFile`** go straight to host descriptors; a short read at end of file succeeds; an
  `OVERLAPPED` gives the offset (synchronous, `Internal`/`InternalHigh` filled). Both first check that the
  guest buffer is committed.
- **`SetFilePointer`** with and without the high word, `ERROR_NEGATIVE_SEEK`, and `SetLastError(0)` on
  success so callers can tell 0xFFFFFFFF apart; `GetFileSize`, `SetEndOfFile`, `FlushFileBuffers` (no
  fsync), `GetFileType`.
- **`GetStdHandle`** returns NULL: a GUI process started from Explorer has no console [doc]. With
  `SPORE_W32_STDHANDLES=1` the handles map to host fds 0–2.
- **Names:** `DeleteFileA/W`, `MoveFileA`/`MoveFileExW` (`REPLACE_EXISTING`, `COPY_ALLOWED` across
  devices), `CopyFileW`, `GetFileAttributesW` (directories `0x10`; files `ARCHIVE`, plus `READONLY` when the
  host file is not writable), `SetFileAttributesW` (`READONLY` only), `CreateDirectoryA/W`,
  `RemoveDirectoryA/W` (a folder holding only macOS metadata counts as empty).
- **Folders:** `GetFullPathNameA/W` (with the file-part pointer), `GetLongPathNameW` (the input, once it
  exists: there are no 8.3 names), `GetTempPathW`, `GetCurrentDirectoryA`, `GetWindowsDirectoryW`,
  `GetSystemDirectoryW`, `SHGetFolderPathW` (`CSIDL_PERSONAL`, `APPDATA`, `LOCAL_APPDATA`, `COMMON_*`,
  `WINDOWS`, `SYSTEM`, `PROFILE`, …; the game's folder table at 0x143e890 uses these) [doc: CSIDL].
- **Volumes:** `GetVolumeInformationA/W` (no label, serial 0x53504F52 fixed so derived IDs are stable,
  NTFS), `GetVolumePathNameW`, `GetDriveTypeA` (C: fixed), `GetLogicalDrives` (C: only),
  `GetDiskFreeSpaceExA` (host `statfs`), `DeviceIoControl` (fails). With no label, the `"yzexo"` probe at
  0x606530 never matches [obs: test].

### 7.4 `FindFirstFile`

The listing is taken when the search starts: `.` and `..` first (not at a drive root), then names sorted
case-insensitively, as NTFS returns them. Wildcards follow Windows: `*`, `?`, `*.*` matches everything and
`name.*` also matches `name`. `.DS_Store` and `._*` files are hidden. `WIN32_FIND_DATAA/W` get attributes,
times, size and `cFileName`; `cAlternateFileName` stays empty. Errors: `ERROR_FILE_NOT_FOUND` when
nothing matches, `ERROR_PATH_NOT_FOUND` for a missing directory, `ERROR_NO_MORE_FILES` at the end.
**Deviation:** 8.3-name matches (`*.txt` matching `a.txt1` through its short name) do not happen.

### 7.5 msvcr90 stdio

- A guest `FILE` is msvcr90's 32-byte record; only `_flag` and `_file` are kept up to date, because the
  game reaches streams only through imported functions (`getc`/`putc` are not imported) [obs:
  `data/imports.json`].
- **Text mode** writes `\n` as `\r\n`, reads `\r\n` as `\n` and stops at Ctrl-Z; the default follows
  `_fmode` (`_O_BINARY` = 0x8000) [doc: fopen]. `ftell` reports the raw file offset.
- `fopen` modes `r w a + t b` (encoding flags are logged and ignored), `fread`, `fwrite`, `fgets`,
  `fputs`, `fprintf`/`vfprintf` (runtime/hle's MSVC printf engine), `fseek`, `ftell`, `fflush`, `feof`, `ferror`,
  `fclose`, `tmpfile` (deleted on close) and `__iob_func`.
- **stdout and stderr** of this console-less process (`_file` = -2) go to the HLE log at INFO, line by line.
- `_stat64i32`/`_wstat64i32` fill msvcr90's 48-byte layout (`st_dev` = 2 for C:, user permission bits
  copied to group and other, the execute bit for `.exe`/`.com`/`.bat`/`.cmd`, trailing separators
  rejected) [doc: _stat]. `_wgetcwd`, `_getdrive` (3) and `_wgetdcwd`.

### 7.6 `SPORE_SKIP_INTRO`: the intro-movie property served false

This is the one place where the layer changes what a read returns. It is opt-in and serves the launcher's
"Skip intro" choice (a row of [DEVIATIONS.md](../DEVIATIONS.md)). Source `src/w32_skipintro.c`; tests `tests/test_skipintro.c` (synthetic packages).

- **What the game asks.** The title sequence's movie step (0xdeabd0) plays the movie only when the
  application property 0xC14680EF is true; false sends it down its own "no movie" branch to the galaxy
  screen. The property is `true` in the application properties resource (`.prop`
  type 0x00B1B104, group 0, instance 0x08EA246B) of `PatchData.package`, `Spore_Game.package` and
  `Spore_EP1_Data.package`, RefPack-compressed in all three.
- **When.** `CreateFile` of an existing `*.package` for reading, with `SPORE_SKIP_INTRO` set and not `0`,
  scans the package once (cached by device, inode, size and modification time; under a millisecond per
  shipped package): the DBPF index, then that resource, decoded with the stream position of every literal
  and a mark on every byte a later back-reference copies. The record `C1 46 80 EF 00 01 00 00` (id, bool,
  no flags) must occur exactly once and its value byte must be 1, a literal, and never copied. Then that
  literal's file offset is kept on the handle (`wfile.skip_intro_at`); otherwise the reason goes to the
  log as a warning and the package is served as it is. An uncompressed resource is handled the same way
  without the decode.
- **The read.** `ReadFile` (plain or `OVERLAPPED`, any chunking) on such a handle returns 0 for that one
  byte when it lies in the range read and reads 1. The decoded resource then differs from the original in
  that byte only, so the property list the game parses has the one property false [obs: decoding the three
  shipped resources with the byte changed gives exactly one changed property]. Files opened for writing,
  other files and every other byte are untouched; nothing is written to disk.
- **Default off:** unset or `0` scans nothing and changes nothing.

## 8. Registry

- **Store:** `~/Library/Application Support/spore-native/registry.json` (or `<support>/registry.json`,
  `SPORE_REGISTRY`), small UTF-8 JSON: `{"keys": {"HKEY_LOCAL_MACHINE\\SOFTWARE\\…": {"name": "text",
  "n": {"dword": 1}, "b": {"binary": "0a0b"}}}}`. A missing file is created from the seed; a damaged one is
  left alone, logged, and the seed is used in memory without saving. Writes stay in memory unless
  `persist_registry` is set (so the network check's `EnableAutodial` write is discarded).
- **Seed** = what the GOG installer writes [obs: the `setRegistry` actions of the installer's
  `goggame-1948823323.script`], with `{app}` replaced by the install path:
  `HKLM\SOFTWARE\electronic arts\spore` (`reindexyesorno` dword 0, `installcompleted` dword 1, `appdir`
  "Spore", `installloc`, `datadir` = `{app}\Data`, `playerdir` "My Spore Creations", `locale` "en-us"),
  `…\spore_EP1` (`AddOnID` dword 2, `PackID` dword 128448390, `DataDir` = `{app}\DataEP1\`),
  `…\spore Creepy and Cute Parts Pack` (`AddOnID` 1, `PackID` 116700625, `DataDir` = `{app}\bp1content\`),
  the empty `…\ea games\spore(tm)`, plus `HKCU\…\Internet Settings\EnableAutodial` dword 0, which every
  Windows has and the network check at 0x94c48b reads and writes. No `BlessID`, `ergc` or `ProductKey`
  (the game does not need them).
- **Lookups** are case-insensitive and keep the stored case. `HKLM\SOFTWARE\WOW6432Node\X` aliases
  `HKLM\SOFTWARE\X` (the 32-bit view the GOG configurator edits).
- **Semantics [doc: RegQueryValueEx, RegEnumKeyEx]:** a query with NULL data and NULL size returns only
  the type (the DWORD wrapper at 0x6ab6f0 does this, then insists on `REG_DWORD` [obs: 0x6ab726–0x6ab73f]);
  NULL data with a size returns the size; a short buffer gives `ERROR_MORE_DATA` with the size; strings
  come back as UTF-16 with the NUL for W and as 1252 for A. `RegEnumKeyExW` returns subkeys sorted, with
  `ERROR_MORE_DATA` for a short name buffer and `ERROR_NO_MORE_ITEMS` at the end.
- **The include filter of 0x687bd0** (lower-case each subkey of `HKLM\SOFTWARE\Electronic Arts`, keep those
  containing one of six strings) passes `spore_EP1` and `spore Creepy and Cute Parts Pack` and not `spore`
  or `ea games` with the seed [obs: replayed in `test_reg.c`].
- `RegOpenKeyExA/W`, `RegQueryValueExA/W`, `RegSetValueExA`, `RegEnumKeyExW`, `RegCloseKey`: all seven
  registry imports.

## 9. Resources, version information and identity

- **Version information** is read from the image the loader mapped at 0x400000: PE header → data
  directory 2 → the resource tree → `RT_VERSION`, preferring language 0x409, then neutral, then the
  first. 0x6b8630 calls `GetModuleFileNameA(NULL)` → `GetFileVersionInfoSizeA` → `LocalAlloc` →
  `GetFileVersionInfoA` → `VerQueryValueA(…, "\")` and formats `%u.%u.%u.%u`, tolerating each failure
  [obs: 0x6b8b5f–0x6b8c19]. The exe's block is id 1 in 22 languages, all with `VS_FIXEDFILEINFO` at
  offset 40 and file version 3.1.0.29 [obs: static read of the exe]. `VerQueryValue` serves the root block
  only; string-table queries fail with `ERROR_RESOURCE_DATA_NOT_FOUND`. Other files report
  `ERROR_RESOURCE_TYPE_NOT_FOUND` (or `ERROR_FILE_NOT_FOUND`).
- **`EnumResourceNamesA`** walks integer-named entries of a type in the exe and calls the guest callback
  (stdcall, 4 arguments) until it returns FALSE.
- **Identity:** `GetComputerNameA`/`GetComputerNameExA` ("SPORE-MAC"; empty for the DNS-domain types),
  `GetUserNameA/W` ("Player", the size counts the NUL as Windows does).
- **Event log** (OpenSSL's fatal reporter, 0x11814bc–0x11814e7): `RegisterEventSourceA` returns a token,
  `ReportEventA` writes the strings to the HLE log, `DeregisterEventSource` succeeds.

## 10. Registration, and why runtime/hle's stubs are weak

`w32_register_shims()` matches each shim to `hle_imports[]` by DLL stem and export name, checks its stack
clean-up against the table, and replaces `fn` (and any `hle_dynamics[]` entry that shared the stub). The
clean-up of every shim is taken from its body by `gen_shims.py` (all `SG_RET`/`SG_RETURN_*` in a body must
agree) and compared with `data/hle/imports-table.json` at generation time and with `hle_imports[]` at run
time: **0 mismatches** [obs].

**The swap alone misses most calls** [obs: the translated C]: the translator emits direct calls to the shim
symbol for `call [IAT]` (`SG_CALL(sg_imp_msvcr90_malloc, 0x862e5bu)`) and `jmp [IAT]` thunks
(`SG_TAILCALL(sg_imp_msvcr90_malloc)`), as runtime-abi §7.2 specifies. Only `mov reg,[IAT]; call reg` sites
and `GetProcAddress` see the table.

So `tools/hle/gen_thunks.py` emits runtime/hle's generated "unimplemented" stubs as `__attribute__((weak))`,
and any strong `sg_imp_*` definition in another library wins at link time. `gen_shims.py` generates
`src/w32_overrides.c`, one strong forwarder per import
(`SG_IMPORT(msvcr90, malloc) { SG_TAILCALL(w32_imp_msvcr90_malloc); }`), which the Makefile compiles in
when `runtime/hle/gen/hle_imports.c` contains weak stubs; a build that compiles
`runtime/win32/src` itself must pass `-DW32_OVERRIDE_HLE_STUBS=1`. The binary then resolves
`sg_imp_msvcr90_malloc` to the forwarder and direct calls by symbol reach our heap [obs]. runtime/core's shims
(`TlsGetValue`, critical sections, …) rely on the same mechanism.

## 11. Interfaces for the other runtime packages

| Package | What to call or know |
|---|---|
| Loader and link (`runtime/image`) | Link `build/win32/libw32.a` **and** `build/hle/libhle.a`; follow §2's start-up order (`runtime/image/src/main.c` does, through `dlsym`); `hle_guest_alloc` is defined here (do not stub it). Optionally `w32_vm_mark_image` per section |
| Runtime core (`runtime/core`) | Guest memory for stacks: `w32_vm_alloc(0, size, MEM_RESERVE\|MEM_COMMIT, PAGE_READWRITE, &err)` (+ `w32_vm_protect(..., PAGE_NOACCESS)` for a guard page); TEBs, PEB and the thunk region: `w32_vm_alloc_runtime(addr, size, prot)`. Set `w32_close_foreign` so `CloseHandle` reaches the core's objects (`sg_core_startup` does); `w32_close_own_handle(ctx, h)` closes only this layer's handles and suits `sg_core_close_hook`. Handle values `0x00A00000–0x00A04000` and registry keys `0x00B00000+` are this layer's. `CloseHandle`'s strong override is this layer's (`tools/core/gen_overrides.py` skips it) |
| Boot harness | `w32_initterm_done`/`w32_initterm_current`/`w32_initterm_hook` count constructors; `w32_exit_hook` catches `exit`; set `SPORE_GAME_DIR` to a folder with the GOG data; `SPORE_HLE_LOG=2` shows every file, registry and module call |

## 12. Tests

`make -C runtime/win32 test` runs the groups VM, heap, paths, registry, files and stdio, CRT start-up,
modules/resources/identity, the groups of §15 and `skipintro`; the suite also passes at `-O0` and under
ASan+UBSan. `make test-cs` adds 8 checks on a case-sensitive APFS image. Every shim call in the tests goes
through the real calling convention and the harness checks `eip`, the stack clean-up and the callee-saved
registers.

The `skipintro` group (§7.6) has 2,319 checks (most are one per `ReadFile` in the byte-at-a-time reads): off
when unset and when `0`; on, the value reads 0 through plain, `OVERLAPPED`, 1-, 7- and 4096-byte reads, for a
compressed and an uncompressed resource and under another spelling of the name; unchanged when the value
literal is copied by a later back-reference, for another resource instance, for a file not named `.package`
and for a handle opened for writing; every other byte equals the file. Whole suite on 2026-10-15: **3,330
passed, 0 failed**.

## 13. Sources

- [doc] VirtualAlloc, VirtualFree, VirtualQuery, VirtualProtect:
  <https://learn.microsoft.com/en-us/windows/win32/api/memoryapi/>
- [doc] HeapAlloc, HeapReAlloc, HeapSize, HeapFree: <https://learn.microsoft.com/en-us/windows/win32/api/heapapi/>
- [doc] CreateFileW (dispositions and errors): <https://learn.microsoft.com/en-us/windows/win32/api/fileapi/nf-fileapi-createfilew>
- [doc] File path formats and normalisation: <https://learn.microsoft.com/en-us/dotnet/standard/io/file-path-formats>
- [doc] FindFirstFileW: <https://learn.microsoft.com/en-us/windows/win32/api/fileapi/nf-fileapi-findfirstfilew>
- [doc] GetModuleFileNameW (truncation): <https://learn.microsoft.com/en-us/windows/win32/api/libloaderapi/nf-libloaderapi-getmodulefilenamew>
- [doc] GetEnvironmentVariableA: <https://learn.microsoft.com/en-us/windows/win32/api/processenv/nf-processenv-getenvironmentvariablea>
- [doc] RegQueryValueExW, RegEnumKeyExW: <https://learn.microsoft.com/en-us/windows/win32/api/winreg/>
- [doc] Parsing C command-line arguments: <https://learn.microsoft.com/en-us/cpp/c-language/parsing-c-command-line-arguments>
- [doc] fopen text/binary modes, `_stat`: <https://learn.microsoft.com/en-us/cpp/c-runtime-library/reference/fopen-wfopen>,
  <https://learn.microsoft.com/en-us/cpp/c-runtime-library/reference/stat-functions>
- [doc] GetFileVersionInfoA, VerQueryValueA: <https://learn.microsoft.com/en-us/windows/win32/api/winver/>
- [doc] GlobalMemoryStatus (2 GiB clamp): <https://learn.microsoft.com/en-us/windows/win32/api/winbase/nf-winbase-globalmemorystatus>
- [doc] SHGetFolderPathW and CSIDL values: <https://learn.microsoft.com/en-us/windows/win32/shell/csidl>

## 14. Open points

- **Not implemented:** `SetFileAttributesA`-style variants that are not imported; the drag-and-drop shell
  calls (`DragQueryFileW`, `ShellExecuteW`) and OLE are the platform layer's.
- **Unverified [inf]:** guard-page use by the game; Windows 7 as the reported version; Unicode
  normalisation of names; `HeapCreate` maximum; the 8.3-name match quirk; text-mode `ftell` after
  reads in msvcr90 (we report the raw offset).

---

## 15. Directory watching, NLS, process imports and version strings: summary

Code: `runtime/win32/src/w32_dirwatch.c`, `w32_nls.c`, `w32_process.c`, `w32_version.c`, the shared header
`w32_dirwatch.h`, and `tests/test_{dirwatch,nls,process,version}.c`. Labels as above.

- **Shims:** 23 `W32_IMPORT`s; 14 are imports of this exe and get strong `sg_imp_*` overrides in the generated
  `w32_overrides.c`. `ReadDirectoryChangesW` is a dynamic import, so its strong override is in `w32_dirwatch.c`
  itself.
- **Tests:** 206 checks (84 NLS, 20 process, 38 version, 64 directory watching); they also pass with
  `SPORE_W32_DIRWATCH_POLL=1` and on repeated runs; about 3.6 s wall [obs].
- `w32_res.c` sends non-root `VerQueryValue` queries to `w32_ver_query`; `w32_file.c` sends `CancelIo` and
  `CloseHandle` on a watched directory to the watcher.

Registration [obs: `nm build/win32/w32_tests`]: `_sg_imp_kernel32_CompareStringW`, `_GetLocaleInfoW`,
`_TerminateProcess`, `_ReadDirectoryChangesW`, `_sg_imp_msvcr90__wsearchenv` are `T` (strong) symbols from
libw32, so direct `SG_CALL(sg_imp_…)` sites, the IAT and `GetProcAddress` (through `hle_dynamics[]`,
which names the same symbol) all reach these shims. runtime/hle's weak stand-in `GetSystemDefaultLCID`
(`runtime/hle/src/k32_nls_weak.c`) loses to ours at link time.

## 16. Directory watching (`w32_dirwatch.c`)

**What the game does** [obs: 0x92f8c0–0x92fb11]: a watcher thread opens the folder with
`CreateFileW(FILE_FLAG_BACKUP_SEMANTICS | FILE_FLAG_OVERLAPPED)`, makes an auto-reset event
(`CreateEventA(0,0,0,0)` at 0x92f903) as `OVERLAPPED.hEvent`, calls `ReadDirectoryChangesW(h, buf, 0x2000,
subtree, filter, &n, &ov, NULL)` through the pointer resolved at 0x130029a (stored at 0x1668f20), waits on
{event, stop event} with `WaitForMultipleObjectsEx(2, …, FALSE, INFINITE, FALSE)` (0x92f97c), and walks the
`FILE_NOTIFY_INFORMATION` records in the buffer itself (no `GetOverlappedResult`). It maps actions 1/5 → 2,
2/4 → 1, 3 → 8 or 0x28 for its callback, re-arms, and on stop calls `CancelIo` (0x92faf1) and closes the
event. The fallback 0x92fb20 uses `FindFirstChangeNotificationW` + `WaitForMultipleObjects(2, …)` +
`FindNextChangeNotification` + `FindCloseChangeNotification`.

**Back end: FSEvents as the trigger, a snapshot diff for the records.**
- Why FSEvents and not kqueue: `EVFILT_VNODE` on a directory reports only that its entry list changed;
  a write into an existing file needs a descriptor on that file, and a subtree watch needs one per
  subdirectory and file. One FSEvents stream (`kFSEventStreamCreateFlagFileEvents | NoDefer | WatchRoot`,
  30 ms latency, on a private serial dispatch queue) covers a whole tree including content changes
  [doc: FSEvents Programming Guide; FSEventStreamCreate].
- Why a snapshot diff: FSEvents coalesces flags (created + modified in one event) and reports a rename as
  two unpaired paths. On each callback the watched tree is rescanned (`lstat` of each entry; `.DS_Store`
  and `._*` hidden as in `FindFirstFile`) and compared with the previous scan by (inode, type, size,
  mtime, birth time, mode, uid). This yields `FILE_ACTION_ADDED`/`REMOVED`/`MODIFIED` and
  `RENAMED_OLD_NAME` immediately followed by `RENAMED_NEW_NAME` (same inode, same parent; a move to another
  folder is REMOVED + ADDED, as NTFS reports it) [doc: FILE_NOTIFY_INFORMATION]. A renamed folder is
  reported alone, not its children. Order: removals deepest first, rename pairs, additions parents first,
  modifications.
- Filters [doc: ReadDirectoryChangesW]: names of files → `FILE_NAME`, of folders → `DIR_NAME`; size →
  `SIZE`; mtime → `LAST_WRITE` (a folder's mtime changes when its entries do, so with `LAST_WRITE` the
  parent folder gets MODIFIED, as on NTFS); writable bit → `ATTRIBUTES`; mode/owner → `SECURITY`; birth
  time → `CREATION`. `LAST_ACCESS` never fires (atime not tracked).
- CoreFoundation and CoreServices are opened with `dlopen`, so neither the unit tests nor the image link
  need a framework flag. If they are missing, or `SPORE_W32_DIRWATCH_POLL=1`, a thread rescans every
  250 ms instead.

**`ReadDirectoryChangesW`** (`hDirectory` must be a directory handle from `CreateFile`; the host path is
recovered from its descriptor with `fcntl(F_GETPATH)`):
- the first call starts recording for the handle; later calls get what happened in between, as Windows
  keeps a buffer per handle; subtree flag and filter are fixed by the first call;
- overlapped: returns TRUE at once, `Internal` = `STATUS_PENDING`, the event is reset; on completion the
  records are written into the guest buffer, `Internal` = 0, `InternalHigh` = bytes, then the event is
  set; a pending request with queued changes completes immediately;
- synchronous: blocks on a condition variable until there is something to report and returns the
  byte count in `*lpBytesReturned`;
- **deviation:** records that do not fit stay queued for the next call; only when not even one record
  fits does it report an overflow (`STATUS_NOTIFY_ENUM_DIR`, 0 bytes; `ERROR_NOTIFY_ENUM_DIR` for the
  synchronous form). Windows discards everything on overflow [doc]; keeping them suits the game's loop;
- completion routines: FALSE, `ERROR_NOT_SUPPORTED` (the game passes NULL) [obs: 0x92f932];
- errors: `ERROR_INVALID_HANDLE` (not a directory handle), `ERROR_INVALID_PARAMETER` (filter 0 or unknown
  bits, unaligned buffer), `ERROR_NOACCESS` (buffer not committed).
- `w32_dirwatch_cancel(h)` (CancelIo) completes the pending request with `STATUS_CANCELLED`
  and sets the event [doc: CancelIo]; `w32_dirwatch_close(h)` (CloseHandle) cancels and ends
  the watch. When a handle value is reused (the file object differs), the
  watcher drops the old watch.

**Change notifications:** `FindFirstChangeNotificationA/W` returns a **manual-reset core event** owned by
the watch, so `WaitForSingleObject`/`WaitForMultipleObjects` work unchanged; a matching change sets it,
`FindNextChangeNotification` resets it, `FindCloseChangeNotification` stops the watch and closes the
handle [doc]. Errors: `ERROR_PATH_NOT_FOUND`, `ERROR_DIRECTORY` (a file), `ERROR_INVALID_PARAMETER`.

**Kernel objects.** `w32_dirwatch_sync` (in `w32_dirwatch.h`) holds the four operations the watcher needs
(create, set and reset an event, close a handle). Their default is the core event API (§21); the unit tests
do not link the core (its `sg_call_guest`/thunk symbols collide with the test harness), so they install a
pthread event implementation through the same struct.

**Tests** (64 checks, our own folders under `build/win32/test-root/docs/watch`): notification signalled
by a create, not by a write under `FILE_NAME`, not by a nested file without the subtree flag, by a nested
write with subtree + `LAST_WRITE`; errors; the game's overlapped loop for create, rename (paired and
adjacent), modify, subfolder + file (parent first), folder rename (children not reported), deletes,
changes made between calls, a 16-byte buffer (overflow), cancel, invalid handles/parameters, a file
handle, close; the synchronous form with a writer thread (a 5 s safety close keeps it from hanging).
Each change is seen within one 30 ms FSEvents batch plus the scan [obs: the whole group runs in ~2 s].

## 17. NLS (`w32_nls.c`)

Call sites [obs]: `GetSystemDefaultLCID` at 0x87dba0 (locale set-up 0x87db80 stores LCID, language id
and sort id); `GetLocaleInfoW(langid, LOCALE_IDEFAULTANSICODEPAGE, buf, 0x80)` then `wcstoul` at 0x847f11
and 0x849c24 (the ANSI code page of a language); `CompareStringW` at 0x6b8e3b with LCID 0x400, or 0x7f
when the caller's bit 0x20 is set, and flags built from `NORM_IGNORECASE`, `NORM_IGNORENONSPACE`,
`NORM_IGNORESYMBOLS`, `NORM_IGNOREKANATYPE`, `NORM_IGNOREWIDTH`, `SORT_STRINGSORT`; `LCMapStringW(lcid,
LCMAP_LOWERCASE, src, n, NULL, 0)` for the size and again into a buffer at 0x6b8f55/0x6b8fb3.

- **LCIDs:** `GetSystemDefaultLCID`, `GetUserDefaultLCID`, `GetThreadLocale`, `Get{System,User}DefaultLangID`
  return 0x0409. `LOCALE_USER_DEFAULT`, `LOCALE_SYSTEM_DEFAULT`, 0 and neutral sublanguages resolve to
  en-US [doc: Locale Identifiers].
- **`GetLocaleInfoA/W`** [doc: GetLocaleInfoW, Locale Information Constants]: about 80 LCTYPEs with Windows 7's
  en-US values (names, ISO codes, `SNAME`, number/currency/date/time formats, day and month names,
  calendar, code pages). Code pages follow the language for any LCID: 1250/1251/1253/1254/1255/1256/
  1257/1258/874/932/936/949/950, else 1252; OEM 437 for en-US, 850 western, 852/866/737/857/862/720/775
  otherwise [doc: Code Page Identifiers]. Names and ISO codes cover the 22 languages of the exe's resources.
  **Deviation:** other text types for non-English LCIDs return the en-US value (logged once).
  `LOCALE_RETURN_NUMBER` writes a DWORD and returns 2 (W) / 4 (A); `cchData` 0 returns the size;
  short buffer `ERROR_INSUFFICIENT_BUFFER`; unknown type `ERROR_INVALID_FLAGS`.
- **`CompareStringW`** [doc: CompareStringW, CompareStringEx, Handling Sorting in Your Applications]: ICU
  from the system (`/usr/lib/libicucore.A.dylib` via `dlopen`; the `en_US` collator, root for
  `LOCALE_INVARIANT`), which implements the Unicode Collation Algorithm that Windows' NLS sorting is
  close to. Strength: tertiary; `NORM_IGNORECASE`/`LINGUISTIC_IGNORECASE` → secondary;
  `NORM_IGNORENONSPACE`/`LINGUISTIC_IGNOREDIACRITIC` → primary with the case level on; both → primary.
  `NORM_IGNORESYMBOLS` removes separators, punctuation and symbols (ICU general categories). Default
  "word sort": hyphens and apostrophes are ignored first and only break ties (fewer first), so `coop` <
  `co-op` < `cop`; `SORT_STRINGSORT` compares them as symbols. Kana/width flags are accepted and have no
  effect on non-Japanese text. Results `CSTR_LESS_THAN`/`EQUAL`/`GREATER_THAN` (1/2/3); 0 with
  `ERROR_INVALID_PARAMETER` (NULL string, bad LCID) or `ERROR_INVALID_FLAGS`. Lower case sorts before upper
  case, as on Windows. `CompareStringOrdinal` (not imported) compares code units, upper-casing first when
  asked.
- **`LCMapStringA/W`** [doc: LCMapStringW]: `LCMAP_LOWERCASE`/`UPPERCASE` with ICU's simple one-to-one
  case mapping (the length never changes: `ß` stays `ß`), surrogate pairs mapped as code points;
  `LCMAP_BYTEREV`; `LCMAP_SORTKEY` (bytes; `NORM_*` and `SORT_STRINGSORT` allowed) = ICU key of the
  word-sort form, a tie-break byte, ICU key of the full form, so `memcmp` order equals `CompareStringW`
  order (the test checks all 1,280 ordered pairs of 16 strings under 5 flag sets). A `-1` source length
  includes the NUL in the result; `cchDest` 0 returns the size; contradictory flags give
  `ERROR_INVALID_FLAGS`.
- Without ICU everything falls back to ordinal comparison after `towupper` (logged once). [inf: ICU's
  and Windows' tables differ in details such as the order of some punctuation and digits vs. letters
  under word sort; not checked against a Windows machine.]

## 18. Process imports (`w32_process.c`) and version strings (`w32_version.c`)

- **`TerminateProcess`** [doc]: for the current-process pseudo handle (-1; `GetCurrentProcess` is the
  core's) logs the code and the guest return address at ERROR level, flushes guest stdio and host stdio,
  and calls `w32_exit_hook(code)`, the path `exit`/`_exit` use, without running `atexit` handlers. Sites:
  0x11e1722 (msvcr90's `__report_gsfailure`, code `0xC0000409`), 0x91e9b9. Other handles:
  `ERROR_INVALID_HANDLE`.
- **`GetProcessTimes`** [doc]: creation time from `sysctl(KERN_PROC_PID)` as FILETIME, exit time 0, kernel
  and user time from `getrusage` in 100 ns units. Sites 0xa5d859/0xa5d8a4.
- **`_searchenv`/`_wsearchenv`** [doc]: the current directory first (the exe folder), then each
  `;`-separated folder of the variable (quotes removed); the result is the Windows path as given;
  `""` + `ENOENT` when not found, `ERANGE` past `_MAX_PATH`. The game's wrapper 0x935d70 defaults the
  variable to `L"PATH"` and returns `buf[0] != 0`.
- **`_spawnv`/`_wspawnv`** [doc: _spawn functions]: log the command line, return -1 with `errno = ENOENT`;
  they never start a host process (no Windows binary ever runs).
- **`_getch`**: -1 (EOF) at once: there is no console [doc: _getch; inf: msvcr90 returns EOF when CONIN$
  cannot be opened].
- **`VerQueryValue`** (`w32_ver_query`) [doc: VerQueryValueW, VS_VERSIONINFO, StringFileInfo, StringTable,
  String, VarFileInfo, Var]: walks `wLength`/`wValueLength`/`wType`/`szKey` nodes with 32-bit padding;
  keys case-insensitive, empty segments skipped; text values return the pointer and the stored length in
  characters (with the NUL); binary values (`Translation`) the bytes; a block without a value TRUE with
  length 0; not found `ERROR_RESOURCE_TYPE_NOT_FOUND`. `VerQueryValueA` returns a code-page-1252 copy
  written to `block + align4(wLength) + offset/2`, inside the 3/2 room `GetFileVersionInfoSize` reports,
  as Windows keeps its ANSI copies after the Unicode block. The exe's en-US block (1,300 bytes) has
  `StringFileInfo\040904b0` with 11 strings and `VarFileInfo\Translation` = 0409 04b0 [obs: static read;
  the test loads the exe's header and `.rsrc` into the test window at run time when `game/` is present,
  and checks `FileVersion` = `3.1.0.29`, `OriginalFilename`, `Translation` and 10 named strings; nothing
  from the file is stored]. Reached from the `VerQueryValueA/W` shims.

## 19. Open points (sections 15–18)

- Rescans are of the whole watched tree per FSEvents batch; for a large `My Spore Creations` folder,
  restricting the scan to the folders named in the event paths is the next step.
- NLS: non-English formats for `GetLocaleInfo`; checking ICU vs. Windows ordering on a Windows machine.
- Not run: the suite under ASan/UBSan (the Makefile's `OUT` is fixed, so a second build directory needs
  a Makefile change).

## 20. Sources (sections 15–18)

- [doc] ReadDirectoryChangesW, FILE_NOTIFY_INFORMATION, FindFirstChangeNotificationW,
  FindNextChangeNotification, FindCloseChangeNotification, CancelIo:
  <https://learn.microsoft.com/en-us/windows/win32/api/winbase/nf-winbase-readdirectorychangesw>,
  <https://learn.microsoft.com/en-us/windows/win32/api/winnt/ns-winnt-file_notify_information>,
  <https://learn.microsoft.com/en-us/windows/win32/api/fileapi/nf-fileapi-findfirstchangenotificationw>,
  <https://learn.microsoft.com/en-us/windows/win32/fileio/cancelio>
- [doc] FSEvents: <https://developer.apple.com/documentation/coreservices/file_system_events>,
  File System Events Programming Guide (Apple, archive)
- [doc] GetLocaleInfoW and LCTYPE constants: <https://learn.microsoft.com/en-us/windows/win32/api/winnls/nf-winnls-getlocaleinfow>,
  <https://learn.microsoft.com/en-us/windows/win32/intl/locale-information-constants>;
  Code Page Identifiers: <https://learn.microsoft.com/en-us/windows/win32/intl/code-page-identifiers>
- [doc] CompareStringW, CompareStringEx (flags), LCMapStringW, CompareStringOrdinal:
  <https://learn.microsoft.com/en-us/windows/win32/api/stringapiset/nf-stringapiset-comparestringw>,
  <https://learn.microsoft.com/en-us/windows/win32/api/stringapiset/nf-stringapiset-comparestringex>,
  <https://learn.microsoft.com/en-us/windows/win32/api/winnls/nf-winnls-lcmapstringw>,
  <https://learn.microsoft.com/en-us/windows/win32/api/stringapiset/nf-stringapiset-comparestringordinal>
- [doc] ICU collation attributes: <https://unicode-org.github.io/icu/userguide/collation/customization/>
- [doc] TerminateProcess, GetProcessTimes: <https://learn.microsoft.com/en-us/windows/win32/api/processthreadsapi/>
- [doc] _searchenv, _spawnv, _getch: <https://learn.microsoft.com/en-us/cpp/c-runtime-library/reference/searchenv-wsearchenv>,
  <https://learn.microsoft.com/en-us/cpp/c-runtime-library/reference/spawnv-wspawnv>,
  <https://learn.microsoft.com/en-us/cpp/c-runtime-library/reference/getch-getwch>
- [doc] VerQueryValueW and the version structures: <https://learn.microsoft.com/en-us/windows/win32/api/winver/nf-winver-verqueryvaluew>,
  <https://learn.microsoft.com/en-us/windows/win32/menurc/vs-versioninfo>

## 21. The directory watcher on the core event API

`w32_dirwatch_sync`'s default (`runtime/win32/src/w32_dirwatch.c`) is the core event API of
`runtime/core/include/sg_core.h` section 5:

```c
static uint32_t core_event_create(int manual) { return sg_core_event_create(manual, 0); }
w32_dirwatch_ops w32_dirwatch_sync = {core_event_create, sg_core_event_set, sg_core_event_reset, sg_handle_close};
```

The FSEvents dispatch queue sets `OVERLAPPED.hEvent` and the change-notification handle directly from its host
thread. `runtime/win32` does not include the core's headers. The four prototypes are repeated in the file with
**weak stand-ins** that return 0; these take effect only when runtime/core is not linked, as in the
w32 unit tests. In the game binary the core's definitions win: `nm -m build/sporenative/spore` shows
`_sg_core_event_create`, `_sg_core_event_set` and `_sg_handle_close` as non-weak `external` text [obs].

**Test over the real core events.** `runtime/core/tests/dirwatch/` is a separate
binary that links `libw32.a`, `libcore.a` and `libhle.a`, the three packages the game links, and keeps the
default operations. It starts up in the game's order: `sg_window_reserve`, `sg_core_init`, `w32_init`,
`sg_core_startup` (which also wires runtime/win32's `CloseHandle` to the core through `w32_close_foreign`), and
a guest main thread. Then it drives both of the game's patterns, waiting with the core's own
`WaitForSingleObject`/`WaitForMultipleObjectsEx` shims:

- the default operations are the core's functions, and an event they create is an `SG_H_EVENT`;
- `FindFirstChangeNotificationA` → a core manual-reset event. The FSEvents queue wakes the guest thread
  12 ms after a file write [obs, one run]. The event stays signalled, `FindNextChangeNotification` resets it,
  and `FindCloseChangeNotification` closes the handle (`sg_handle_type` 0, and a wait fails);
- the watcher thread at 0x92f8c0: `CreateEventA(0,0,0,0)` as `OVERLAPPED.hEvent`, then `CreateFileW(BACKUP_SEMANTICS
  | OVERLAPPED)` and an overlapped `ReadDirectoryChangesW` (`STATUS_PENDING`). `WaitForMultipleObjectsEx({event,
  stop})` returns 0 after a write, and `ADDED a.txt` is in the buffer 13 ms after the write. The wait consumes the
  auto-reset event. Then a re-arm, `SetEvent(stop)` → index 1, and `CancelIo` → `STATUS_CANCELLED` with the
  event set through the API. `CloseHandle` on the directory ends the watch, and `CloseHandle` on the events
  (runtime/win32's shim, forwarded to the core) closes them.

`make -C runtime/core test-dirwatch` (or `make -C runtime/core/tests/dirwatch test`): **38 passed, 0 failed**
(about 1 s).

## 22. What the boot asks of this layer [obs]

This section records what the game did between WinMain and its first `CreateWindowExW` (0x849d03, the game's
init step 18) on a clean sandbox. The boot to that point takes about 0.64 s: about 534 ms from the entry to WinMain, about 100 ms
from WinMain to the window. `SPORE_STOP_AT=window` on the bare binary reproduces it (platform-macos.md
§10.12) and prints the registry reads and the per-file I/O table (`w32_io_report()` in `w32_file.c`: opens, reads,
writes, bytes and furthest offset per host path; `SPORE_HLE_LOG=2` logs attribute probes, `fopen`, `_stat` and
registry enumeration, `=3` every `ReadFile`/`WriteFile`).

### 22.1 The steam_api stand-in

GOG's `SporebinEP1/steam_api.dll` (15,360 bytes; we analysed it with Capstone only and looked at nothing of
Valve's library) calls itself `steam_api_sr3.dll`, image base 0x3b400000: GOG's
generic stand-in, 17 exports, importing only KERNEL32, USER32, SHELL32 and ADVAPI32. It reads an optional
`gog.ini` next to itself; **the Spore folder has none**, so every INI read takes its "absent" branch.

- **`SteamAPI_Init`** (0x3b401110) is `mov al,1; ret`: true, only `al` set. The game calls it at init step 1
  (0xf47e99) and tests `al`; false would end init. `SteamAPI_Shutdown` is a bare `ret` (called from 0xf47630).
- **`SteamApps`** returns a static object (vtable 0x3b4032e8, 8 thiscall slots then NULL). With no gog.ini: +0x00
  → 1; +0x04 (`[Config] LowViolence`) → 0; +0x08/+0x0c → 0; +0x10/+0x14 (language: gog.ini, else a 237-entry LCID
  table, else `"english"`) → `"english"`; +0x18 (u32 id, `ret 4`) → 1 if id is 0xd7be, else the DLC check; +0x1c
  (u32 id, `ret 4`) the DLC check: ids outside its 19-entry table (55380–55400, another title's DLC) → 1, inside →
  gog.ini `[EnableDLC]` if present, else 1.
- **The game uses one slot.** `SteamApps` is called at exactly one site, the wrapper 0x6b4ed0, which calls slot
  +0x18 with one argument and returns its `al` (a NULL object gives false). Its only caller is 0x686c15 in the pack
  check 0x6866b0, with the u32 from property 0x7634d70
  of the pack being registered: once per boot, 17440 for the Creepy & Cute pack, answer 1.
- While scanning `SporebinEP1/*.dll` for EA plugins the game also `LoadLibrary`s `steam_api.dll` and asks for
  `EASharedLibraryStartupFunction0`/`…Function`, which the stand-in does not export (NULL, as on Windows).

`runtime/hle/src/steam_stub.c` returns the "no gog.ini" column: the bool slots return 0 or 1 in the whole of `eax`
(the stand-in leaves the upper bits undefined), and the language slots return `"english"` without the table, since
the game never calls them (`runtime/hle/tests/test_steam.c`, including the `ret 4` clean-up). The stand-in is a row of
[DEVIATIONS.md](../DEVIATIONS.md).

### 22.2 Registry and paths the game uses

- **Keys and values read** (HKLM, case-insensitive): `Software\Electronic Arts\SPORE` → `PlayerDir` ("My Spore
  Creations"), `AppDir` ("Spore"), `DataDir` (`C:\Spore\Data`), `Locale` ("en-us"); `SOFTWARE\Electronic Arts`
  enumerated with `RegEnumKeyExW` (`ea games`, `spore`, `spore Creepy and Cute Parts Pack`, `spore_EP1`, then 259);
  for each pack key `AddOnID`, `PackID`, `DataDir`, `ProductKey` (parts pack 1, 116700625, `C:\Spore\bp1content\`;
  EP1 2, 128448390, `C:\Spore\DataEP1\`). Each value is read twice (size, then data). `ProductKey` is absent, as
  on a GOG install, and the game goes on. The seeded values it never asks for: `reindexyesorno`,
  `installcompleted`, `installloc`, `…\ea games\spore(tm)`, and HKCU `EnableAutodial` (read later, by the network
  check). There are no registry writes during the boot.
- **The virtual C:** `C:\Spore` is the Spore folder (§7.1), `Documents` and `AppData` the sandbox. The Data
  directory check at 0x7e91a9 is `GetFileAttributesW("C:/Spore/Data/")` = 0x10; any other answer ends the game with
  "Could not find the Data directory, we cannot run."

### 22.3 Files read, probed and created

- **Read:** `DataEP1/Config/Properties.txt` (6,425 bytes) through **one `ReadFile` per byte**, harmless at boot;
  `AppData/Spore/Preferences/Preferences.prop` (`_wstat64i32`, opened, then written back with 146 small writes);
  `FindFirstFileW("…/Config/*")`. The game also enumerates and opens its 16 packages.
- **Probed and not found** (none ships with the GOG install): `SporebinEP1/demo.txt`, `DataEP1/Config/LocalLogging.txt`
  and `Logging.txt`, `Data/Preload.package`, `Spore_DDFList.txt` and `DDFLists/` (never touched), and
  `DDFMap.txt` in each data and user folder and its parents. `Logging.txt` is an optional config **script**, not a
  log: 0x7e7c00 checks for `LocalLogging.txt`, then `Logging.txt`, in the config folder and `sinclude`s the first
  that exists.
- **DLLs:** `SporebinEP1/*.dll` is scanned for EA plugins: `goggame-1948823323.dll` (our `LoadLibrary` returns NULL;
  on Windows it loads and has no EA entry point [inf]), `steam_api.dll` (above), `msvcr90.dll` (not loaded).
  `rasapi32.dll` → NULL, so the RAS check is skipped.
- **Created under the sandbox** (nothing under the Spore folder): `AppData/Spore/EditorSaves.package`,
  `Planets.package`, `RigblockInfo.package` (96 bytes each) and `Pollination.package`, `GraphicsCache.package`
  (120 bytes; the game also probes `GraphicsCache.001.package`), each created with an empty DBPF header and read
  back; the folders `AppData/Spore/{Temp,Preferences,CityMusic,Games/Game0}` and
  `Documents/My Spore Creations/{Pictures,Movies,AnimatedAvatars}`.
- **Large files:** package index offsets reach 0x3cdbe34e (1.02 GB); reads are `pread` loops and `SetFilePointer`
  passes the high word (`test_file.c` seeks to 4 GiB + 16 in a sparse file). The boot needs no mmap: its reads
  total 4.6 MB.

### 22.4 Pre-window placeholders

The imports the boot needs before the window are weak in runtime/hle, so the platform layer's strong versions
replace them at link time: `user_prewindow.c` (`XInputGetState` and friends
report no pad, 1167; `GetKeyboardLayout` 0x04090409; `CreateSolidBrush`, `LoadCursorA`, `LoadIconA` pseudo handles
0x00e0xxxx; `RegisterClassExW` atom 0xc000+n), `dinput_stub.c` (`DirectInput8Create` whose `EnumDevices` finds
nothing: with XInput reporting no pad, the gamepad manager 0x8d2180 falls back to DirectInput8, `EnumDevices` class
4, `DIEDFL_ATTACHEDONLY`), and `hle_boot_stops.c` (`hle_stop_check()`, which every window and D3D9 entry point calls
first so `SPORE_STOP_AT` keeps working).
