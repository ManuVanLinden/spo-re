# Third-party components

Every file in this repository's source tree is the project's own work. No third-party source file is vendored (we
checked for copyright, licence and SPDX headers and for source URLs). The components below are fetched, installed
or linked on your own Mac at build time. A release carries only the source tree, and the app the converter builds
contains no third-party library. The versions are the ones the project builds with today.

| Component | Version | Licence | How it is used | Where it ends up |
|---|---|---|---|---|
| [Capstone](https://www.capstone-engine.org/) | 5.0.3 | BSD-3-Clause | x86 disassembler of the manifest builder, the import, exception and thread analyses and the translator | `pip install --target tools/.deps`; never copied into the app |
| Homebrew `python`, `innoextract` | Brewfile | PSF-2.0, zlib | the converter's interpreter; unpacking the GOG installer | build time only |
| Apple frameworks (AppKit, Metal, QuartzCore, CoreAudio, AudioToolbox, GameController, CoreHaptics, Foundation) and the Command Line Tools | the installed macOS | Apple's licences | system frameworks the runtime links | dynamically linked from the system; never copied |
| [DXVK](https://github.com/doitsujin/dxvk) present blit shader | n/a | zlib | the area-weighted "sharp bilinear" present filter in `runtime/d3d9metal/src/mt_present.mm` is derived from DXVK's `dxvk_present_frag_blit.frag`, rewritten in MSL and altered; the comment there states the origin and marks the shader as altered, as the zlib licence asks | that one shader string in the app's runtime |

Nothing in this source tree imports Unicorn (its pip wheel bundles a GPL-2.0 emulator engine) or Pillow. The
translator's fixture suite and the differential tester run code under Unicorn, but they are development tools
outside this tree, and macOS `sips` re-encodes the app's icon.

## Facts about the game in the source tree

This repository and its releases hold no game file, no extracted asset and no translated code. The converter reads
your own copy at build time and writes the app under `build/`. A few small tracked files carry facts measured from
the GOG executable, because the converter needs them: the import table and its addresses
(`runtime/hle/gen/hle_imports.{c,h}`), the analysts' hand-made import sections (`data/imports-hand.json`), library
code regions (`data/libraries/text-regions.json`), the two executables' SHA-256 hashes, and individual addresses named in comments of the runtime.
