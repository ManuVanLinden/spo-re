# runtime/: what the translated game runs on

These packages provide what Windows, Direct3D 9 and DirectSound would give the translated C. `tools/link/build.py`
compiles them (sources outside the test folders) and links them with the translated code into
`build/sporenative/spore`. [docs/design/README.md](../docs/design/README.md) shows where they fit in the pipeline.
Every package's tests run our own code, never the game's.

| Package | What it is | Design | Tests |
|---|---|---|---|
| `include/` | `spore_guest.h`, the ABI between translated code, the runtime and the shims | [runtime-abi.md](../docs/design/runtime-abi.md) | (used by every package) |
| `image/` | loader, thunk region, dispatch support, import overrides, weak defaults, `main` | [image-linker.md](../docs/design/image-linker.md) | `tools/link/build.py` (smoke run and self-test) |
| `core/` | TEB and TLS, guest threads, kernel objects and waits, critical sections, the MSVC exception-handling runtime | [runtime-core.md](../docs/design/runtime-core.md) | `make -C runtime/core test` (also `test-san`, `test-tsan`, `test-hle`) |
| `hle/` | the import table and its generated stubs, the msvcr90 CRT functions | [hle-crt.md](../docs/design/hle-crt.md) | `make -C runtime/hle test` |
| `win32/` | CRT start-up, guest heap, files and paths, registry, modules, directory watching, NLS | [win32-layer.md](../docs/design/win32-layer.md) | `make -C runtime/win32 test` (and `test-cs`) |
| `com/` | guest-visible COM objects backed by host objects | [runtime-abi.md](../docs/design/runtime-abi.md) §8 | `make -C runtime/com test` |
| `d3d9null/` | Direct3D 9's `IDirect3D9` (adapter identity, caps, formats, modes), which the Metal back end shares, and a null device that draws nothing (the test binaries' device) | [DEVIATIONS.md](../docs/DEVIATIONS.md) §3 | `make -C runtime/d3d9null test` |
| `d3d9metal/` | the Metal back end: device layer (`src/`), shader translator (`shader/`, [README](d3d9metal/shader/README.md)), pipeline and draws (`pipeline/`) | [DEVIATIONS.md](../docs/DEVIATIONS.md) §3 | `make -C runtime/d3d9metal test` (opens a window), `make -C runtime/d3d9metal/shader test`, `make -C runtime/d3d9metal/pipeline test` |
| `dsound/` | DirectSound for the guest, over the platform layer's CoreAudio output | [DEVIATIONS.md](../docs/DEVIATIONS.md) §5 | `make -C runtime/dsound test` |
| `platform/macos/` | the AppKit window, events, clipboard, drag and drop, gamepads and CoreAudio, as an ordinary Mac program | [platform-macos.md](../docs/design/platform-macos.md) | `make -C runtime/platform/macos check` (`check-gui` opens a window) |
| `platform/guest/` | the USER32, GDI32, IMM32, XInput and DirectInput8 imports over `platform/macos` | [platform-macos.md](../docs/design/platform-macos.md) §10 | `make -C runtime/platform/guest test` |

Build output goes to `build/<package>/` (git-ignored). Some generated files are tracked. Each generator takes
`--check` to compare them with what it would write:

| Generated | Generator | Also as |
|---|---|---|
| `hle/gen/hle_imports.{h,c}` | `tools/hle/gen_thunks.py` (reads `data/imports.json`: run the import tools by hand in the source tree first, `tools/imports/build_table.py`) | `make -C runtime/hle gen` / `check-gen` |
| `win32/src/w32_shims.h`, `w32_overrides.c` | `tools/win32/gen_shims.py` | `make -C runtime/win32 gen` / `check-gen` |
| `core/src/core_overrides.c` | `tools/core/gen_overrides.py` | |
| `d3d9null/src/d3d9_slots.h` | `runtime/d3d9null/gen/gen_slots.py` | `make -C runtime/d3d9null gen` / `check-gen` (part of `make test`) |
| `platform/guest/src/sgp_list.h` | `runtime/platform/guest/gen_list.py` | `make -C runtime/platform/guest check-gen` |
