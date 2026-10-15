# HLE shim layer and host CRT

The HLE ("high-level emulation") layer is how the translated game reaches Windows: each of the exe's 495 imports
gets a host function, a shim, that the translated code calls in place of the DLL. This package holds the import
table and its generated stubs, a few KERNEL32 basics, and the guest CRT: msvcr90's string, number, printf/scanf,
time and math functions, written to match msvcr90 rather than the host libc. The other runtime packages
(`runtime/core`, `runtime/win32`, `runtime/platform`, the D3D9 and DirectSound back ends) provide the remaining
shims the same way.

Code: `runtime/hle/` and its generator `tools/hle/` (`gen_thunks.py`, `sigs.py`, `check_sigs.py`); the
generated `data/hle/` files are git-ignored. ABI: [runtime-abi.md](runtime-abi.md) §7 and
`runtime/include/spore_guest.h` §7. First version 2026-10-12.

Labels: **[obs]** observed in the exe or measured, with the evidence; **[doc]** taken from Microsoft's
documentation (cited); **[inf]** inferred, not yet checked.

No game code runs in the tests: they run our own C against a guest window that holds no game bytes.

## 1. Summary

| Item | Result |
|---|---|
| Thunk table | All **495** imports from 20 DLLs, in IAT order, as `sg_import hle_imports[]` (the struct from `spore_guest.h`). **2** are data imports (`_adjust_fdiv`, `_acmdln`) that get a guest variable instead of a thunk |
| Dynamic names | **66** names that the game resolves with `GetProcAddress`: **35** provided, **31** left absent on purpose (NULL, as on a GOG install without that component), each with its reason |
| Stubs | Every function import without a hand-written shim gets a generated stub that logs `unimplemented: DLL!name (called from guest 0x…)` once, then aborts (default) or returns 0 with the correct stack clean-up (`SPORE_HLE_UNIMPL=continue`) |
| Hand-written shims | **122** imports, covering **6,883 of 12,176** static uses (57%): memory/string, ctype, UTF-16 `wcs*`, number conversion, MSVC `printf`/`scanf`, `wsprintf`, time and clocks, `_CI*` and libm, FP control words, `MultiByteToWideChar`/`WideCharToMultiByte`, `qsort`, `rand`, locale queries, Interlocked*, last error |
| Tests | `make -C runtime/hle test`: **3,365 checks pass, 0 fail** (2026-10-15), at `-O2`, at `-O0`, and under `-fsanitize=address,undefined`. Every shim call in the tests goes through the real calling convention and the harness checks `eip`, the `esp` clean-up and the callee-saved registers |
| Signature check | `tools/hle/check_sigs.py` compares the convention table against **9,641** call sites in the exe: 403 imports consistent, 17 have no analysable site, 75 inconclusive under the push-count heuristic and reviewed by sampling (§3.2) |
| Naming | `sg_imp_<dll stem>_<export or ordN>`, identical to the translator's `imp_symbol` (`recomp/sgrecomp/image.py`) for all 493 function imports [obs: compared by script] |

## 2. Layout and build

```
runtime/hle/
  include/hle.h          shim support API: tables, logging/policies, guest strings, per-thread CRT
                         state, the printf/scanf/strtod/code-page engines
  gen/hle_imports.{h,c}  GENERATED: HLE_IMP_* indices, SG_IMPORT declarations, hle_imports[],
                         hle_import_info[], hle_dynamics[], and the unimplemented stubs
  src/hle_core.c         loader (IAT fill), GetProcAddress back end, logging, policies, per-thread state
  src/crt_string.c       memory, string, ctype, wcs*
  src/crt_conv.c         strtol family, atoi, strtod/atof/wcstod, _ltoa family, _ecvt/_fcvt,
                         rand, _errno, the old-CRT digit generator
  src/crt_format.c       printf engine; sprintf, _snprintf, vsprintf, _vsnprintf, printf, wsprintfA/W
  src/crt_scan.c         scanf engine; sscanf, swscanf
  src/crt_math.c         _CI*, floor/ceil/modf/ldexp, _isnan/_finite, _control87/_controlfp(_s)
  src/crt_time.c         _time32/64, _localtime64, _gmtime64, _mktime64, strftime; QPC/QPF,
                         GetTickCount, timeGetTime, GetSystemTime(AsFileTime), SystemTimeToFileTime
  src/k32_nls.c          MultiByteToWideChar, WideCharToMultiByte, GetACP
  src/crt_misc.c         qsort, setlocale/_setmbcp/_configthreadlocale/_ismbblead, getenv, fatal
                         paths, GetLastError/SetLastError, Interlocked*
  tests/                 harness + 9 test files (host C only)
  Makefile               make / make test / make gen / make check-gen
tools/hle/
  sigs.py                calling convention, argument slots and return kind of every import and
                         dynamic name (hand-written from the public prototypes)
  gen_thunks.py          generator (reads data/imports.json + sigs.py + the shims' definitions)
  check_sigs.py          cross-check of sigs.py against the exe's call sites
data/hle/
  imports-table.json     IAT slot -> symbol, convention, clean-up bytes, return kind, implemented (read by
                         tools/win32/gen_shims.py and tools/core/gen_overrides.py)
  sig-check.json         per-import result of check_sigs.py
```

Commands (from the top of the source tree; `gen_thunks.py` reads `data/imports.json`, which the import tools write
when run by hand there, `tools/imports/build_table.py`):

```
python3 tools/hle/gen_thunks.py            # regenerate after adding a shim or editing sigs.py
make -C runtime/hle test                    # build build/hle/libhle.a and run the tests
PYTHONPATH=tools/.deps python3 -P tools/hle/check_sigs.py   # 2 s, ~100 MB
```

`gen_thunks.py` finds the hand-written shims by preprocessing `src/*.c` with clang and collecting every
`sg_imp_*` function definition, so shims produced by macros count. A stub is generated only for imports
with no definition, so there is never a duplicate symbol. `--check` fails when `gen/` is stale.

## 3. The thunk table

### 3.1 Contract with the ABI

The generator fills in what `spore_guest.h` §7 declares:

- Each shim is `SG_IMPORT(dll, name)`, so it is an `sg_func`. It reads arguments with `SG_ARG(i)` and
  leaves with `SG_RET(n)`, where `n` is the stdcall argument bytes (0 for cdecl and `SG_CC_X87`).
- `hle_imports[i]` is the header's `sg_import`: `dll`, `name` (NULL for ordinals), `ordinal`, `cc`,
  `argbytes`, `iat`, `guest`, `fn`. `cc` uses `SG_CC_*`; data imports use `HLE_CC_DATA` (0xff).
- `hle_install_imports(mem)` is the loader step of runtime-abi §7.2. It calls `sg_thunk_register` for
  each import **in IAT order**, so slot numbers are the same on every run, and writes the thunk address
  into the IAT. `_adjust_fdiv` gets a guest `int 0` (no Pentium FDIV bug). `_acmdln` gets a guest
  `char *` cell that the start-up layer must fill.
- `hle_resolve(module, name, ordinal)` is the back end for `GetProcAddress`. It matches module names
  by stem, ignoring case (`KERNEL32.DLL`, `kernel32`, `Kernel32.dll`). `xinput1_1` and `xinput1_3` are
  aliases of `xinput9_1_0`. A dynamic name that is also a static import returns the same thunk. Unknown
  names return 0 and log. `hle_module_known` backs `LoadLibrary`/`GetModuleHandle`.
- For the other generators: `data/hle/imports-table.json` gives, per IAT slot, the symbol, the convention, the clean-up
  bytes and the return kind (`f` = pushes ST(0); `u`/`b` = the `_CI*` x87 effects). The translator derives the same
  symbols itself from the PE import directory; the names agree for all 493 function imports [obs].
  The 2 data imports have no symbol: an access through their IAT slot is a data load, not a call.

### 3.2 Conventions and how we checked them

`tools/hle/sigs.py` lists convention, 4-byte argument slots (a `double` or 64-bit integer counts 2) and
return kind for each import, written from the public prototypes. Notable entries:

- Everything in KERNEL32, USER32, GDI32, ADVAPI32, IMM32, ole32, VERSION, WINMM, WSOCK32/WS2_32, dbghelp,
  d3d9 and d3dx9 is stdcall, except **`wsprintfA`/`wsprintfW` (cdecl, varargs)** [doc: wsprintfA].
- MSVCR90 is cdecl, except `_CxxThrowException` (**stdcall**, 2 slots), the `type_info` destructor
  method (**thiscall**), and the 8 `_CI*` helpers (**x87**: operands on the FPU stack).
- `steam_api` exports are cdecl.
- dbghelp's 64-bit functions take `DWORD64` arguments: `SymLoadModule64` 7 slots,
  `SymGetSymFromAddr64` and `SymGetLineFromAddr64` 5 slots.

`check_sigs.py` disassembles the manifest block of every `call [IAT]` and `mov reg,[IAT]`…`call reg`
site (9,641 calls) and counts the 4-byte slots pushed since the previous call. A call to an import in
between consumes its own slot count. It also records whether the caller cleans up with `add esp, N`
right after the call. Results [obs, `data/hle/sig-check.json`]:

| Verdict | Imports | Meaning |
|---|---:|---|
| ok | 403 | stdcall: some site pushes exactly the table's slot count and no caller cleans up. cdecl: callers clean up |
| more-pushes-seen | 62 | every site shows more pushes than slots. These are register saves in the same block (prologue `push esi/edi`) |
| fewer-pushes-seen | 6 | the arguments were pushed in an earlier block |
| stdcall-but-caller-cleans | 3 | reviewed: not real clean-up (below) |
| cdecl-never-cleaned | 4 | `strncat`, `_fcvt`, `_beginthreadex`, `atof`: MSVC defers the clean-up and merges it into a later `add esp` |
| no-sites | 17 | reached only through `jmp [IAT]` thunks (the exe has 119 of them) or never called |

We reviewed these by hand [obs, disassembly]:

- `CreateWindowExA` @0x92b2ee: 12 arguments (11 pushes after `GetModuleHandleA`, plus `lpParam` before).
- `DuplicateHandle` @0x921f8f: 7 arguments across two nested `GetCurrentProcess` calls.
- `setsockopt` @0x94d8de: 5 (4 pushes plus one earlier).
- `RegOpenKeyExA` @0x94c48b: 5.
- `SetThreadAffinityMask` @0x6b7da2: 2, with `GetCurrentThread` in between.
- The three "caller cleans" cases are not clean-up:
  - `GetModuleFileNameA` @0x91b83c is followed by `add esp,0x1c; ret`, the caller's own frame epilogue.
  - `PostQuitMessage` @0x848c84 is followed by `add esp,0x10; ret 4`, also an epilogue.
  - `_CxxThrowException` does not return; the bytes after its call belong to other code.

**Remaining risk [inf]:** a "more-pushes" import whose table count is too small would look the same as
one with register saves. Each of the 62 needs a look before its shim is written. Getting a count wrong
corrupts `esp`, and the return-address check (runtime-abi §5.3) reports it at the first call.

### 3.3 Dynamic names

`sigs.py` `DYNAMIC` lists all 66 names from `data/imports.json` `dynamic_apis`. The generic OpenSSL DSO
lookup is not listed; any name not in the table returns NULL. Policies:

- **Absent (return NULL):**
  - `TGGetOS`, so the game's `$cider` (Mac port) flag stays false and the Windows code paths run.
  - `nvapi_QueryInterface` and `D3DKMT*` (runtime-abi §7.2).
  - `DasmX86_*`.
  - The 7 `Ras*` functions: `LoadLibrary("rasapi32.dll")` fails.
  - `NetStatisticsGet`/`NetApiBufferFree` and the Toolhelp walk; OpenSSL's `RAND_poll` tolerates NULL
    [inf].
  - The plugin entry points.
- **Provided:**
  - the XInput four;
  - `EnumDisplayDevicesA`, `TrackMouseEvent`, `GetProductInfo`, `ReadDirectoryChangesW`;
  - the `Crypt*` three, backed by the host RNG (`runtime/win32/src/w32_crypt.c`);
  - the user32 entropy calls;
  - psapi;
  - the dbghelp crash-handler set, which returns FALSE: the native crash report replaces it.

## 4. MSVC (msvcr90) behaviours implemented, with sources

Sources:

- **[VS2008-fmt]** "Format Specification Fields: printf and wprintf Functions", VS2008,
  <https://learn.microsoft.com/en-us/previous-versions/visualstudio/visual-studio-2008/56e442dc(v=vs.90)>
- **[VS2008-type]** "printf Type Field Characters", VS2008,
  <https://learn.microsoft.com/en-us/previous-versions/visualstudio/visual-studio-2008/hf4y5e3w(v=vs.90)>
- **[VS2010-size]** "Size Specification", VS2010,
  <https://learn.microsoft.com/ar-sa/previous-versions/visualstudio/visual-studio-2010/tcxf1dw6(v=vs.100)>
- **[VS2008-snprintf]** `_snprintf`, VS2008,
  <https://learn.microsoft.com/en-us/previous-versions/visualstudio/visual-studio-2008/2ts7cx93(v=vs.90)>
- **[VS2008-strtod]** `strtod`, VS2008,
  <https://learn.microsoft.com/en-us/previous-versions/visualstudio/visual-studio-2008/kxsfc1ab(v=vs.90)>
- **[VS2008-printf_p]** "printf_p Positional Parameters", VS2008,
  <https://msdn.microsoft.com/en-us/library/bt7tawza(v=vs.90)>
- **[VS2015-chg]** "Visual C++ change history 2003–2015", which describes the pre-2015 behaviour that
  VS2015 changed: <https://learn.microsoft.com/en-us/cpp/porting/visual-cpp-change-history-2003-2015>
- **[cur-fmt]** "Format specification syntax: printf and wprintf",
  <https://learn.microsoft.com/en-us/cpp/c-runtime-library/format-specification-syntax-printf-and-wprintf-functions>
- **[cur-scanf]** "scanf width specification",
  <https://learn.microsoft.com/en-us/cpp/c-runtime-library/scanf-width-specification>
- **[ctl87]** `_control87, _controlfp, __control87_2`,
  <https://learn.microsoft.com/en-us/cpp/c-runtime-library/reference/control87-controlfp-control87-2>
- **[wsprintf]** `wsprintfA`, <https://learn.microsoft.com/en-us/windows/win32/api/winuser/nf-winuser-wsprintfa>

### 4.1 printf family (`hle_format`)

| Behaviour | msvcr90 (what we do) | Source | Test |
|---|---|---|---|
| Spec syntax | `%[flags][width][.precision][{h\|l\|ll\|I\|I32\|I64}]type` | VS2008-fmt | many |
| `%I64d`, `%I64x`, `%lld` | 64-bit argument (2 slots); `I32` 32-bit; plain `I` pointer-sized, so 32-bit | VS2010-size | `-1 81985529216486895 …` |
| `hh`, `j`, `z`, `t` | not in msvcr90. `hh` acts as `h` [inf]; `j`/`z`/`t` are unknown characters, so `%zu` prints `zu` | VS2010-size lists none; cur-fmt marks them as later additions | `zu`, `jd`, `511` |
| Unknown type character | "the character is copied" | VS2008-fmt | `%q` → `q` |
| Positional `%1$s` | only the `_printf_p` family supports it. In `printf`, `1` is a width and `$` an unknown character, so it prints `$s` and takes no argument | VS2008-printf_p, VS2008-fmt | `$s` |
| `%F`, `%N` | before VS2015 these were far/near **size modifiers**, skipped: `%Fd` = `%d`. `%F` is not a conversion | VS2015-chg "%F specifier" | `%Fd` |
| `c`/`s` vs `C`/`S` | in printf `c`/`s` are narrow and `C`/`S` wide (reversed in wprintf); `h` forces narrow, `l`/`w` force wide | VS2008-type, VS2010-size | `abcd`, `n\|w\|n\|w\|w` |
| `%hs`, `%ls`, `%ws`, `%hc`, `%lc` | as above | VS2010-size | yes |
| `%Z` | `ANSI_STRING`; `%wZ` `UNICODE_STRING` | cur-fmt ("VS 2013 and earlier") | `counted` |
| NULL `%s`/`%S` | prints `(null)` | VS2008-type | yes |
| `%p` | 8 upper-case hex digits, zero-filled (`0012FF7C`) | [inf: the CRT sets precision to 8 and uses upper-case hex; the docs say only "hexadecimal digits"] | yes |
| `%n` | disabled by default: the invalid-parameter handler runs (in release msvcr90 `_invoke_watson` ends the process). We log and abort, or return -1 under "continue" | VS2008-type security note | yes |
| Exponent digits | at least **3** (`1.000000e+000`); the game does not import `_set_output_format` | VS2008-type note; VS2015-chg "Exponent formatting" | yes |
| Infinity/NaN | `1.#INF`, `-1.#IND` (x86 default NaN), `1.#QNAN`, `1.#SNAN`, formatted like digits and **rounded like digits**: `%.2f` of +inf is `1.#J`, `%.1f` is `1.$`, `%f` is `1.#INF00`, `%g` is `1.#INF` | VS2015-chg "Infinity and NaN Formatting", cur-fmt | yes |
| Digit generation | only **17 significant digits**, then zeros: `printf("%.0f", pow(2,80))` = `1208925819614629200000000`; `%.20f` of 0.1 = `0.10000000000000001000` | VS2015-chg "Floating point formatting and parsing" (the example is verbatim) | yes |
| Rounding | half-up on that decimal digit string: `%.0f` of 0.5 = `1`, of 2.5 = `3`; `%.1f` of 0.25 = `0.3` [inf: follows from the digit-string rounding; C99 libcs print 0, 2, 0.2] | VS2015-chg (the rounding of `#INF` shows digit-string rounding) | yes |
| `%a`/`%A` | default precision **6** (C says 13); zero padding goes before `0x` | VS2015-chg "%A and %a precision / zero padding" | `0x1.000000p+0` |
| `l`/`L` with floats | `long double` = `double` | VS2010-size | `%lf\|%Lf` |
| `_snprintf` | `len<count`: copy and NUL, return len; `len==count`: copy, **no NUL**, return len; `len>count`: copy `count`, no NUL, **return negative** | VS2008-snprintf | all three cases |
| `printf` | the game is a GUI process with no console; the text goes to the HLE log | our choice | yes |
| `wsprintfA/W` | flags `-` `#` `0` only, so `%+d` prints `+d`; no floating point (`%f` prints `f`); `#` gives `0x`; `hc`/`lc` of 0 is dropped; output capped at 1,024 characters including the NUL | wsprintf | yes |

### 4.2 Parsing: `strtod`, `atof`, `wcstod`, `strtol` family

- **Float form:** `[whitespace][sign][digits][.digits][{d|D|e|E}[sign]digits]`. **`d`/`D` are accepted as
  the exponent letter**; there is no `inf`, `nan` or hex-float parsing; an exponent letter with no digits
  is not consumed [doc: VS2008-strtod; VS2015-chg "Hexadecimal and infinity/NaN floating point parsing"
  says those came only with VS2015].
- **Only 17 significant digits count;** the rest are dropped before conversion [doc: VS2015-chg].
  We convert the kept digits with correct rounding. msvcr90 used its own 80-bit routine, which can
  differ in the last bit for some inputs [inf].
- **Overflow** returns ±HUGE_VAL with `ERANGE`; **underflow** returns 0 with `ERANGE` [doc: VS2008-strtod].
- **`strtol`/`strtoul`/`_strtoui64`/`wcstol`/`wcstoul`:** C saturation and `ERANGE`. When there are no
  digits, including right after `0x`, `*endptr = nptr` [inf: msvcr90 source behaviour; C99 would point
  after the `0`].
- **`atoi`/`atol`/`_wtol`:** accumulate with 32-bit wrap-around and no overflow check, so
  `atoi("4294967297") = 1` [inf: VS2008 `atox.c` has a plain loop; later CRTs route through `strtol`].
- **`errno` values are msvcr90's** (`ERANGE` 34, `EINVAL` 22, `EILSEQ` 42, …), stored in a per-thread
  guest `int` that `_errno()` returns.

### 4.3 scanf family (`hle_scan`)

- Size prefixes `h`, `l`, `ll`, `I64`, `L`, `w`. `c`/`s` store narrow and `C`/`S` wide in `sscanf`, the
  reverse in `swscanf`; `h`/`l` override [doc: cur-scanf; I64 is the Microsoft extension].
- **Sets:** `%[a-z]` and `%[z-a]` are equivalent; a leading `]` is a member. `%Nc` stores N characters
  and no NUL [doc: cur-scanf].
- **Floats:** decimal only; `inf`/`nan` are not matched before VS2015 [doc: VS2015-chg].
- **Integers:** they wrap rather than saturate, and `%d` of `4294967297` gives 1 [inf: old `input.c`];
  `%i` honours `0x` and `0` prefixes.
- **Return value:** returns the number of assigned fields, or EOF (-1) when the input ends before the
  first conversion.
- **Unknown conversion characters:** we treat them as a literal to match [inf].

### 4.4 ctype and locale

[obs] The game calls `setlocale` only for categories 1, 3, 4 and 5 (`LC_COLLATE`, `LC_MONETARY`,
`LC_NUMERIC`, `LC_TIME`; call sites 0x87fab3–0x87faeb), treats NULL as failure, and then calls
`_setmbcp(-4)` at 0x87fb3f. It **never sets `LC_CTYPE`**. So msvcr90 takes its "C"-locale paths:

- **ctype:** `is*` cover ASCII only; 128–255 are unclassified; `tolower`/`toupper`, `towlower`/`towupper`,
  `_stricmp`, `_strnicmp`, `_wcsicmp` and `_wcsnicmp` fold A–Z only. `_stricmp` folds to **lower** case
  and returns the difference, so `_` sorts before letters.
- **Wide classification:** `iswctype` uses the wide table, which classifies Latin-1 (`é` is alpha) and,
  above U+00FF, the host's Unicode data. The Latin-1 details (superscript digits, ª º µ) are [inf].
- **`setlocale`:** accepts and reports the names. Our formatting keeps the "C" rules, which match an
  English locale: decimal point `.` and English day/month names. **Deviation:** a user with a German or
  French locale would get `,` from msvcr90's `printf`/`strtod` under `LC_NUMERIC`, and we do not.
  If testing shows the game depends on it, `hle_format` can read a decimal-point variable.

### 4.5 Time

- `struct tm` is 9 ints.
- **`_localtime64`/`_gmtime64`:** return a per-thread guest buffer; NULL for negative times or times
  after 3000-12-31.
- **`strftime`:** follows the "C" locale's MSVC definitions:
  - `%c` = `%m/%d/%y %H:%M:%S`, `%x` = `%m/%d/%y`, `%X` = `%H:%M:%S`.
  - `#` removes leading zeros, or selects the long form: `%#c` = `Tuesday, March 14, 1995, 12:41:29`.
  - An unknown conversion is an invalid parameter.
  - **Deviation:** `%z`/`%Z` print the host's zone abbreviation; Windows prints the long name
    ("Pacific Standard Time").
- **Clocks:**
  - `QueryPerformanceFrequency` reports 10 MHz, as current Windows does.
  - `QueryPerformanceCounter`, `GetTickCount` and `timeGetTime` read one monotonic host clock
    (`CLOCK_UPTIME_RAW`).
  - `FILETIME` and `SYSTEMTIME` conversions use a civil-date formula, because macOS `timegm` rejects
    year 1601 [obs: returns -1].

### 4.6 `_CI*`, libm and control words

- **`_CI*`:** operands and results on the x87 stack [obs]. Unary helpers replace ST(0). `_CIpow`/`_CIfmod`
  compute f(ST(1), ST(0)) and pop once. `floor`/`ceil`/`modf`/`ldexp`/`atof`/`strtod`/`wcstod` take
  cdecl stack arguments and push ST(0). Values come from host libm in double precision, which matches
  msvcr90's SSE2 code path.
- **`_control87`/`_controlfp`/`_controlfp_s`:** map msvcr90's abstract word onto **both** the x87 CW and
  MXCSR, as on x86 [doc: ctl87].
  - `_controlfp` never changes the denormal mask.
  - When the two units disagree, `_EM_AMBIGUOUS` is set.
  - The reset state (CW 0x027F, MXCSR 0x1F80) reads back as **0x0009001F**, and **0x000A001F** after
    `_PC_24`, the values printed in the reference page's example [doc, tested].
- **Interface note for the translator and runtime:** the shims update `ctx->fpu.cw` and `ctx->mxcsr` only.
  The floating-point model allows a derived per-thread `pc24` flag. A translator that caches one must
  re-derive it after any call to these three imports. The header's `sg_x87` has no such field today.

### 4.7 Code pages

[obs] Five of the seven `MultiByteToWideChar`/`WideCharToMultiByte` call sites pass `CP_UTF8` (0xFDE9) as
an immediate (0x9317cc, 0x931dc0, 0x9395f2, 0x931837, 0x93801c). The other two take the code page in a
register, fed by `GetACP`.

- We implement **UTF-8 and 1252** (`GetACP` = 1252), with the documented Win32 contract:
  - `-1` lengths include the NUL;
  - a size-0 call is a query;
  - a buffer that is too small gives `ERROR_INSUFFICIENT_BUFFER`;
  - `MB_ERR_INVALID_CHARS` gives `ERROR_NO_UNICODE_TRANSLATION`; without it, invalid UTF-8 becomes U+FFFD;
  - UTF-8 accepts flags 0 or 8 only, and rejects `lpDefaultChar`/`lpUsedDefaultChar`.
- **Deviation:** there is no 1252 "best fit" mapping; unmappable characters become `?`.

### 4.8 Other

- **`qsort`:** the msvcr80/90 algorithm: non-recursive quicksort with median of three, `CUTOFF` 8 and a
  selection-sort `shortsort`. It calls the guest comparator through `sg_call_guest`. The order of equal
  elements depends on the algorithm. We wrote the code from the published algorithm, so a tie order
  matching msvcr90's is **[inf]**; the test records it as a regression value.
- **`rand`:** msvcr90's LCG (`seed*214013+2531011`, bits 16–30), seeded with 1 per thread. The first
  values are 41, 18467, 6334, 26500, 19169 [tested]. The game imports no `srand`.
- **`_encode_pointer`/`_decode_pointer`:** identity. msvcr90 XORs with a cookie, but the game only
  round-trips the values.
- **`getenv`:** returns NULL. The game asks for `JPEGMEM` [obs, 0x863b82] and OpenSSL variables, none of
  which a normal install sets.
- **Fatal paths:** `_purecall`, `abort`, `_invoke_watson` and `_amsg_exit` log the guest return address
  and abort.

## 5. Policies

- **Unimplemented imports:** `SPORE_HLE_UNIMPL=abort` (default) or `continue`.
  - With `continue`, the stub returns 0, 0:0 or 0.0, or leaves the `_CI*` x87 stack correct, and pops
    the right number of bytes. So `sigs.py` must be right even for stubs.
  - Each import is logged once with the guest return address.
- **Invalid parameters** (`%n`, NULL strings to `_stricmp`, bad `_controlfp_s` mask, unknown `strftime`
  conversion): logged; abort under the default policy, matching msvcr90's `_invoke_watson`.
- **Logging:** `SPORE_HLE_LOG=0..3`; the sink `hle_log_sink` can be replaced.

## 6. What the runtime core must provide

These are declared in `hle.h` or `spore_guest.h` and stubbed by `tests/harness.c`:

- `hle_guest_alloc` (guest heap)
- `sg_thunk_register` and `sg_thunk_fn[]`
- `sg_call_guest`
- `sg_misaligned_atomic`
- the TEB at `ctx->fs_base` (`LastError` at +0x34)

Per-thread CRT state uses host `_Thread_local` storage; this assumes one host thread per guest thread,
which runtime-abi §9 specifies.

## 7. Open items

- **Imports outside this package.** The 122 hand-written shims here cover the CRT's pure functions. CRT start-up
  and exception handling, threads and synchronization are in `runtime/core`; the heap, stdio, files, paths, the
  registry and the rest of KERNEL32 in `runtime/win32`; USER32, GDI32, IMM32 and XInput in `runtime/platform`;
  D3D9 and DirectSound in their back ends. Their strong definitions replace this package's weak generated stubs
  at link time ([win32-layer.md](win32-layer.md) §10); an import nobody implements still reaches its stub and
  logs `unimplemented:`.
- **The 62 "more-pushes" and 6 "fewer-pushes" imports in `sig-check.json`:** review each slot count
  before writing its shim (§3.2).
- **Check against a real msvcr90.** Compare the printf/scanf/strtod engines with msvcr90 output for the
  same fixtures, especially the [inf] rows: `#x` of 0, `hh`, the `%a` details, `%S` with characters
  above 0xFF, `qsort` tie order, the `atoi` wrap, and `strtol` end pointers. Any Windows machine can run
  that test program, since it holds no game code.
- **Locale-dependent decimal point** (§4.4) and Windows zone names in `strftime`, if they turn out to
  matter.
