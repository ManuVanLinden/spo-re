# How it works

Spore's Windows program, `SporebinEP1/SporeApp.exe`, never runs. The converter translates its x86 machine code into
C once, on your Mac, compiles that for arm64 and links it with our own runtime, which stands in for Windows,
Direct3D 9 and DirectSound. The game reads its data files from the Spore folder at run time, unchanged.

## The pipeline

`tools/convert/convert.py` runs these steps in order ([convert.md](convert.md)). You can also run each tool by hand
from the top of the source tree; its docstring says how.

1. **Manifest** (`tools/manifest/`): every function, basic block and switch table of the exe, found by
   recursive-descent disassembly.
2. **Exception tables** (`tools/eh/`): the MSVC C++ and SEH records, and the places where execution resumes
   after an exception, which the translator must be able to re-enter.
3. **Imports** (`tools/imports/`): the Windows functions the exe calls, statically and through `GetProcAddress`.
4. **Threads** (`tools/threads/`, then `recomp/memorder_sites.py`): which code runs on which thread, and the
   memory accesses that must keep x86's ordering on arm64.
5. **Translate** (`recomp/`): every function to C against the ABI header `runtime/include/spore_guest.h`.
6. **Link** (`tools/link/`, with `tools/harness/prep_src.py`): the translated C and the runtime into one
   executable, plus the table that maps guest addresses to the C functions.
7. **Bundle** (`tools/bundle/`): `Spore.app`, with the launcher and its settings window.

The tools share a PE reader (`tools/code-characteristics/pe.py`) and an instruction-form classifier
(`tools/insn-forms/forms.py`). `tools/hle`, `tools/win32`, `tools/core` and `tools/d3d9` regenerate tracked files
of the runtime (`make gen` in those packages); `tools/shaders` is the Python reference of the Metal back end's
shader translator, which its tests use.

## The runtime

At run time the translated game sees a 4 GiB guest address space, a Windows-like process and the imports it
expects. Each import is a C function, called a shim. `runtime/README.md` lists the packages and their tests.

## Design pages

| Page | What it covers |
|---|---|
| [convert.md](convert.md) | The converter: inputs, the hash gate, prerequisites, stages, the work folder, resumption, what it needs |
| [translator.md](translator.md) | The x86-to-C translator: lowering of instructions, flags, x87, SSE and control flow; results |
| [runtime-abi.md](runtime-abi.md) | The contract between translated code, the runtime and the shims: memory, CPU context, calls, dispatch, thunks, COM, threads, exception hand-off |
| [memory-ordering.md](memory-ordering.md) | Which memory accesses the translator orders on arm64, why, and what it costs |
| [image-linker.md](image-linker.md) | The loader, the dispatch table and the whole-image link |
| [runtime-core.md](runtime-core.md) | TEB, TLS, threads, synchronization and the MSVC exception-handling runtime |
| [hle-crt.md](hle-crt.md) | The import table, its generated stubs and the msvcr90 CRT functions |
| [win32-layer.md](win32-layer.md) | CRT start-up, guest heap, files and paths, registry, modules, directory watching, NLS |
| [platform-macos.md](platform-macos.md) | Window, input, display, fullscreen, text input and sound on macOS |
| [bundle.md](bundle.md) | `Spore.app`: layout, launcher, player defaults, signing, the settings window |
| [../DEVIATIONS.md](../DEVIATIONS.md) | Every behaviour that differs from the same exe on Windows, with its switch |

The source tree's top-level `.gitignore` and `Brewfile` are explained in [convert.md](convert.md) §7.
