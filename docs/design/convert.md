# The converter: one command from a GOG install to the app

`python3 -P tools/convert/convert.py <gog-folder | setup_spore*.exe> [-o DIR]`
runs the build chain (manifest, exception tables, imports, thread analysis, translation, link, bundle) against the user's own GOG
copy and writes `Spore.app`. It is glue: input handling, the hash gate, the prerequisite check, a work folder
outside the source tree, resumable stages and progress lines. Its code is `tools/convert/convert.py`, the user's
guide is the top-level README and the tests are `tools/convert/test_convert.py`.

## 1. Inputs

- A folder: the install (holding `SporebinEP1/SporeApp.exe`, `Data/`, `DataEP1/`), its `SporebinEP1` folder
  (taken as the parent), or a folder with exactly one install directly inside (the launcher's `normalise_pick`
  rules). The converter resolves paths with `realpath`, so a symlink to the Spore folder resolves to its target.
- The GOG offline installer (`setup_spore*.exe`, or a folder holding exactly one, with the `.bin` parts beside
  it): the `extract` stage runs
  `innoextract -d <extract-to> -I SporeBin -I SporebinEP1 -I Data -I DataEP1 -I bp1content <setup.exe>`, default `--extract-to ~/Library/Application Support/spore-native/game`, then
  continues with that folder. Measured: 53 s, 5.8 GB, and the result is file-for-file identical to an install made by
  the GOG installer (`diff -rq` empty).
- The hash gate: `SporebinEP1/SporeApp.exe` must hash to `SG_IMAGE_SHA256` (`runtime/image/include/sg_image.h`);
  `SporeBin/SporeApp.exe`, when present, to the base game's known hash (a warning when absent). The converter
  refuses anything else (exit 1) with a message naming the GOG Galactic Adventures build as the only input. For the
  installer the gate runs after extraction.

## 2. Prerequisites

The converter checks these before any stage and prints the line that fixes each one that is missing: Apple Silicon (`sysctl hw.optional.arm64`), the
Command Line Tools (`xcode-select -p`, `clang --version`), Python 3.9 or newer, the Homebrew packages, capstone.

- Homebrew: `tools/convert/Brewfile` lists `python3` and `innoextract`, all the pipeline needs
  from it (the rest is the Command Line Tools and macOS: `clang`, `codesign`, `sips`). A package counts as
  installed when `<prefix>/opt/<name>` exists (`--brew-prefix`, hidden, default `/opt/homebrew`; the test points
  it at an empty folder). Only `innoextract` is checked, and only for installer input (macOS's own `python3`
  serves too). If it is missing, the command names it, prints `brew bundle --file tools/convert/Brewfile` and
  stops.
- Python packages: the chain imports only **capstone** (checked by reading every tool's imports and by an
  audit of every file the pipeline opens; the icon path uses macOS `sips`, not an imaging package). If
  capstone is missing, the command says so and installs it with
  `python3 -m pip install --target tools/.deps capstone==5.0.3`; `--no-pip` prints the line and stops instead.
- Disk: 3 GB free at the work folder (a run uses 0.8 GB, the app 0.1 GB); 7 GB at `--extract-to` for the
  installer.

## 3. Stages

| Stage | Commands (from the top of the source tree, `PY` = `python3 -P`) | Outputs (under `<work>`) | M4 Max |
|---|---|---|---|
| extract | `innoextract ...` (installer input only) | `<extract-to>/SporebinEP1/SporeApp.exe` | 53 s |
| manifest | `PY tools/manifest/build.py --tag ep1 --exe E` | `scratch/manifest/ep1/` | 71 s |
| eh | `PY tools/eh/ehdb.py`; `PY tools/eh/verify.py` (its V3 check reports 8 known differences as FAIL; the stage passes when both files are written) | `scratch/eh/ep1/ehdb.json`, `data/eh/{resume-points,funcinfo-summary}.json` | 2 s |
| imports | `parse_imports.py E`, `callsites.py`, `dynres.py`, `build_table.py ... --hand-sections data/imports-hand.json` | `scratch/imports/*.json`, `data/imports.json` | 1 s |
| threads | `PY tools/threads/scan.py`, `threads.py`, `PY recomp/memorder_sites.py` | `scratch/threads/ep1/`, `data/threads/*.json` | 43 s |
| translate | `PY tools/link/build.py --tus 512 --translate-only --jobs N` | `build/sporenative/recomp-512/` | 101 s |
| harness | `PY tools/harness/prep_src.py` | `build/harness/src/` | 0 s |
| link | `PY tools/link/build.py --src <work>/build/harness/src --jobs N` (compile, link, the loader smoke run and dispatch self-test, which exit before any game code) | `build/sporenative/spore`, `build/sporenative/obj/` | 143 s |
| bundle | `PY tools/bundle/make_bundle.py --binary <work>/build/sporenative/spore --out <output>/Spore.app --game-dir G` | `<output>/Spore.app` (checks included) | 3 s |
| settings | in the converter: `<support>/game-dir` = the Spore folder (`SPORE_SUPPORT_DIR` honoured) | the launcher's remembered folder | 0 s |

Times are from one run of 2026-10-15 (stage by stage, nothing else running): 364 s in all, 6 minutes. The translated C
of that run was byte-identical to a build made by running the tools by hand in the source tree with the same exe
(all 510 files), and the manifest, `imports.json` and the `threads/` files matched too, apart from the exe path
written into headers.

Two differences from building by hand are deliberate. First, translation runs as its own stage
(`--translate-only`, added to `tools/link/build.py` for this). By hand, `build.py --tus 512` compiles and links
`recomp-512`, and `make_bundle.py --after-link` then compiles and links `build/harness/src` again; the separate
stage saves that first compile and link. Second, link and bundle are separate stages (`build.py --src
build/harness/src`, then `make_bundle.py` without `--after-link`); `--after-link` does the same in one process.

The link stage keeps `build.py`'s loader smoke run and dispatch self-test (`--no-test` skips them): they run our loader, which maps the image, installs the import table and exits before any game code,
never the game itself. `N` is `hw.perflevel0.physicalcpu` (the performance cores; `--jobs` overrides). The converter passes
`SPORE_EXE` to every tool, so the loader's smoke run finds the exe without a `game/` link in the source tree.

## 4. The work folder and `SPORE_WORK_DIR`

Run by hand, the tools write under the source tree (`scratch/`, `build/`, the generated `data/` files) and read the
exe through `game/` (a folder or a link to the Spore folder). Three environment variables move them: a
`ROOT`-relative path becomes `WORK`-relative. With the variables unset, the tools behave as described above.

| Variable | Default | Meaning |
|---|---|---|
| `SPORE_WORK_DIR` | the source tree | where generated outputs go and are read back: `scratch/manifest`, `scratch/eh`, `scratch/threads`, `scratch/code-characteristics`, `build/` (recomp, sporenative, harness, bundle work folder), and the generated `data/eh/`, `data/imports.json`, `data/threads/` |
| `SPORE_GAME_DIR` | `<source tree>/game` | the Spore folder (base exe `SporeBin/SporeApp.exe` under it) |
| `SPORE_EXE` | `$SPORE_GAME_DIR/SporebinEP1/SporeApp.exe` | the EP1 exe (the runtime's loader honours it too) |

Tracked inputs stay in the source tree whatever `SPORE_WORK_DIR` says: `data/imports-hand.json` and
`data/libraries/text-regions.json`. These tools honour the variables, and each
says so in its docstring or next to its constants: `tools/code-characteristics/pe.py` (`EXE`, `GAME`),
`tools/manifest/{build,manifest}.py`, `tools/eh/{ehimage,ehdb,verify}.py`, `tools/threads/{scan,threads,tgraph}.py`,
`recomp/{translate,memorder_sites}.py`, `recomp/sgrecomp/memorder.py`, `tools/link/{build,gen_link}.py`,
`tools/harness/prep_src.py`, `tools/bundle/make_bundle.py` (`--binary`, `--out`, `--link-src`, the bundle work folder,
`--game-dir`). `make_bundle.py`'s default `--out` follows the variable too, so with it
set nothing lands in the source tree's `build/Spore.app`. The converter also refuses an output of
`<source tree>/build/Spore.app` (where `make_bundle.py` puts an app built by hand) unless `--force`.

The default work folder is `~/Library/Caches/com.spore-native.converter/<first 12 hex of the exe's SHA-256>/`
(`--work` overrides). It holds the outputs above plus `log/<stage>.log` (the tools' own output) and
`stamps/<stage>.json`.

## 5. Resumption

A stage is skipped when its stamp exists, every output exists, the stamp records the same command (the argv with
`--jobs` left out, plus the folder pair for settings), and the stamp is newer
than every input. Inputs are the exe, the stage's tools (folders are walked, `__pycache__` ignored), its tracked
data, and the stamps of the stages it reads from; the link stage's inputs include all of `runtime/`, so a
`git pull` that changes the runtime relinks (incrementally: `build.py`'s own object stamps). The settings stage
also compares the file's content with the Spore folder, since the player can change it in the app. A failed stage
leaves no stamp. `--force` reruns every stage, `--clean` deletes the work folder first. `--stage NAME` runs
one stage (and refuses when an earlier one has no stamp), `--until NAME` stops after one; the test drives the
pipeline stage by stage with `--stage` so that no one process passes 10 minutes. Every tool runs in its own
process group under `--timeout` (default 1800 s) and the converter kills it and its children when that passes. A failure
prints the stage, the exit code and the last 25 lines of its log, and exits 1.

## 6. The renderer

The app's one renderer is the Metal back end. The converter has no renderer
option, the bundle has no `Contents/Frameworks` and the launcher no Renderer row ([bundle.md](bundle.md) §10).

## 7. What the converter needs

Everything the converter runs or reads, by path. Folders are needed whole unless noted.

- **The converter:** `tools/convert/convert.py`, `Brewfile`, `test_convert.py`. The top-level `Brewfile` is a copy
  of `tools/convert/Brewfile`, so that `brew bundle` also works without `--file`; the top-level `.gitignore` keeps
  out what the tools write inside the source tree: `build/`, a `work/` folder, any `*.app`, `tools/.deps/` (where
  the converter installs capstone), `__pycache__/`, `*.pyc` and `.DS_Store`.
- **Manifest:** `tools/manifest/build.py`, `tools/manifest/manifest.py`, `tools/code-characteristics/pe.py`.
- **Exception resume points:** `tools/eh/ehdb.py`, `tools/eh/verify.py`, `tools/eh/ehimage.py`.
- **Imports:** `tools/imports/parse_imports.py`, `callsites.py`, `dynres.py`, `build_table.py`;
  `data/imports-hand.json`.
- **Threads:** `tools/threads/scan.py`, `threads.py`, `tgraph.py`; `recomp/memorder_sites.py`;
  `data/libraries/text-regions.json`.
- **Translator:** `recomp/translate.py`, `recomp/sgrecomp/` (`__init__`, `cfg`, `emit`, `image`, `lift`,
  `memorder`), `recomp/include/`, `tools/insn-forms/forms.py`.
- **Link:** `tools/link/build.py`, `tools/link/gen_link.py`, `tools/harness/prep_src.py`, `runtime/` (every package
  `build.py` compiles for the converter: `image`, `hle` with `gen/`, `core`, `win32`, `com`, `d3d9null`,
  `dsound`, `d3d9metal`, `platform`, and `include/`; the packages' tests are not compiled).
  `runtime/image/include/sg_image.h` is also where the converter reads the expected hash.
- **Bundle:** `tools/bundle/make_bundle.py`, `launcher.m`, `exe_icon.py`, `icon.m`.
- **Not needed by the converter:** the packages' tests, the generators of the runtime's tracked generated files
  (`tools/hle`, `tools/win32`, `tools/core`, `tools/d3d9`) and the shader tools (`tools/shaders`), which the
  runtime's own `make test` and `make gen` targets use; any Python package besides capstone. `translate.py --coverage` reads an instruction-form census the converter never builds.

The version string (`--version`, the app's `CFBundleShortVersionString`) is `make_bundle.py`'s `git_version()`:
`<tag> (<git describe>)` of the source tree's git history: the nearest `v<major>.<minor>.<patch>` tag without
its `v` (`0.0` when no tag is reachable), then `git describe --tags --always --dirty`. `CFBundleVersion` is the
commit count. A release is made by tagging: `git tag -a v0.2.0 -m "..."`, then rebuilding the bundle stage.
