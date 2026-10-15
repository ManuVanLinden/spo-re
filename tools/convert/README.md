# Spo-re

This tool is a static recompiler that turns **your own DRM-free copy of Spore with Galactic Adventures + Creepy & Cute
Parts Pack** into a native app for Macs with Apple Silicon (M1 or later). As far as I know, the only such copy is the
SPORE™ Collection from GOG. The game's Windows program is translated into C on your Mac,
compiled for your processor and packed into `Spore.app`. This static recompiler does not include any source code or
content from the game, nor does it intend to distribute it.

The tool reads your installation files, statically recompiles to native Apple Silicon ARM64 instructions,
provides a runtime that maps Direct3D 9 to Metal and a launcher for the options, and lets you play the game you
played ages ago, natively on your M-Series Mac.

## What you need

- A Mac with Apple Silicon (M-Series).
- Apple's Command Line Tools: run `xcode-select --install` once (Xcode itself is not needed).
- [Homebrew](https://brew.sh), and the packages listed in `Brewfile`. From the top folder of this repository:

  ```
  brew bundle --file tools/convert/Brewfile
  ```

  This installs Python 3 and `innoextract` (to unpack the GOG installer; not needed when you give the installed
  Spore folder).
- **Spore Collection from GOG** (Spore with Galactic Adventures), either
  - the installed Spore folder, the one that holds `SporebinEP1`, `Data` and `DataEP1` (for example copied from a
    Windows PC where GOG Galaxy installed it), or
  - the GOG offline installer: `setup_sporetm_collection_..._gog_....exe` together with its `.bin` files, all in
    the same folder.

The first time, the tool installs one Python package (capstone, a disassembler) into `tools/.deps` inside this
repository, and tells you before it does. `--no-pip` stops it from doing that.

## Converting

From the top folder of this repository:

```
python3 -P tools/convert/convert.py "/path/to/your/Spore folder"
```

or, with the installer:

```
python3 -P tools/convert/convert.py ~/Downloads/setup_sporetm_collection_3.1.0.22_gog_v2.hotfix_\(86147\).exe
```

`Spore.app` is written into the folder you run the command from; `-o ~/Applications` puts it directly into your applications.
When the command finishes, open `Spore.app` to play. The first launch already knows where your Spore folder is.

You can also name the `SporebinEP1` folder, or a folder that contains exactly one Spore install; the command
finds the install from there.

### The Process

| Step | What it does |
|---|---|
| extract | unpacks the installer (installer input only) |
| manifest | finds every function in the game's program |
| eh | finds where the program resumes after errors |
| imports | lists the Windows functions the program calls |
| threads | finds the places threads share data |
| translate | translates the program into C, 512 files |
| harness | prepares the start-up hook |
| link | compiles and links, then a quick self-check that runs no game code |
| bundle | builds, signs and checks `Spore.app` |
| settings | remembers your Spore folder for the first launch |

If the command stops (an error, Ctrl-C, a restart), run it again: it skips the finished steps and continues
where it stopped.

### Flags

| Flag | Description |
|---|---|
| `-o DIR`, `--output DIR` | write `Spore.app` into `DIR` (default: the current folder) |
| `--extract-to DIR` | installer input: where the game is unpacked (default `~/Library/Application Support/spore-native/game`). The app reads the game from there every time you play, so keep it |
| `--work DIR` | where the work files go (default `~/Library/Caches/com.spore-native.converter/<number>`) |
| `--no-pip` | do not install the Python package; only say what is missing |
| `--clean` | delete the work files first and start from scratch |
| `--force` | redo every step even when it looks done |
| `--dry-run` | show the steps and their commands, run nothing |
| `--jobs N` | compile with N parallel jobs (default: your Mac's performance cores) |
| `--version` | print the converter's version |

## Where things go

- **The app:** `Spore.app`, in the folder you run the tool from, or wherever you choose with `-o`.
- **Saves, settings and logs** of the game itself: `~/Library/Application Support/spore-native`, your creations in
  `~/Documents/My Spore Creations`, and logs in `~/Library/Logs/Spore Native`.

## Troubleshooting

- **"the Command Line Tools are missing"**: run `xcode-select --install`, wait for it to finish, run the command
  again.
- **"missing Homebrew packages"**: run the `brew bundle` line the command prints, then the command again.
- **"is not the GOG build of Spore Galactic Adventures"**: the folder holds another version of Spore (Steam, EA
  app, disc) or a modified program. Install the GOG version, or unpack the GOG installer with this command.
- **"no DataEP1 folder inside"**: Galactic Adventures is missing from that folder; the GOG Spore Collection
  includes it.
- **A step failed**: the command prints the step, its exit code and the last lines of its log; the whole log is
  in `<work folder>/log/<step>.log`. Run the command again (it continues), or with `--clean` to start over.

## How it works

The converter runs a chain of tools: an analysis of the game's program (its functions, exception tables, imports
and threads), a translator from x86 to C, and a link against our runtime, which stands in for Windows, Direct3D 9
and DirectSound on macOS. [docs/design/README.md](../../docs/design/README.md) explains how the pieces fit together and
links a design page for each. [docs/DEVIATIONS.md](../../docs/DEVIATIONS.md) lists every place where the game behaves
differently from Windows, with the switch that turns it off. [THIRD_PARTY.md](../../THIRD_PARTY.md) names the
third-party components. The source is under the MIT licence ([LICENSE](../../LICENSE)).

## Testing

`python3 -P tools/convert/test_convert.py --game "/path/to/your/Spore folder"` checks the refusals, the dry run
and the prerequisite check, then converts into a temporary folder (about 7 minutes) and checks the app without
starting the game. `--quick` runs only the first, fast part.

The other tests run from the top folder of this repository. None of them starts the game:

```
make -C runtime/<package> test        # each runtime package: core, hle, win32, com, d3d9null, dsound,
                                      # platform/guest, d3d9metal (opens a window), d3d9metal/shader
make -C runtime/platform/macos check  # the macOS host layer, headless
PYTHONPATH=tools/.deps python3 -P tools/manifest/test_build.py
PYTHONPATH=tools/.deps python3 -P tools/threads/test_threads.py
PYTHONPATH=tools/.deps python3 -P tools/insn-forms/test_forms.py
python3 -P tools/eh/test_fh3_model.py
python3 -P tools/bundle/test_launcher.py --launcher "Spore.app/Contents/MacOS/Spore Native" --game "/path/to/your/Spore folder"
```

## Features

In addition to the game running natively on Apple Silicon, I added a launcher built with Apple's AppKit that
lets you change the runtime's settings, for example the frame rate, which you can change from the original 30 fps to
60, vsync, or uncapped.

You can launch the app with documented flags, including the experimental foliage editor, the cell editor with the full parts list, and some experimental gamemode launch flags.

There's also a new option to skip the intro, so you can launch directly into the main menu.

Online features are also fully functional. You can log in to your EA account, publish and download creations, and use the online functionality as intended.
If you do not want to play online, you can explicitly disable online features in the launcher as well.

## AI Disclaimer

This project was built with AI: the x86-to-C translator, the runtime (the Windows layer, sound, input
and the Direct3D 9 to Metal back end), the launcher, this converter and the documentation were written with
Claude, with the tech stack directed and output reviewed by the author. Every part is covered by the test suites in this repository, and the
result is verified manually.
