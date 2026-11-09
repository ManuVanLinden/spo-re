# Spo-re for Steam Galactic Adventures

This fork of [spo-re](https://github.com/garnold24/spo-re) builds a native Apple Silicon app from a specific prepared Steam copy of Spore Galactic Adventures. The upstream project supports the GOG edition. This fork adds Steam image validation, separate launcher and save paths, AZERTY input support, and a fix for a Creature Stage crash when a creature loses its target.

## Build on your Mac

You need an Apple Silicon Mac, the Xcode Command Line Tools, Python 3.11 or later, and your own Steam copies of Spore and Galactic Adventures.

Use a separate copy of the game folder containing `Data`, `DataEP1`, and `SporebinEP1/SporeApp.exe`. The converter expects an already prepared executable from the Steam build below. Preparing the executable is a separate step. This repository contains the recompiler and runtime source.

| Image | SHA-256 |
| --- | --- |
| Original Steam Galactic Adventures executable | `3010a413f9a4a05ee9c4de24756c1c94c7fb218f70cad29bcf5bc0b4abfa2466` |
| Prepared Galactic Adventures executable | `1eef3b46d077d50d7391a972cfb21df23f79e7759a73980211e0598f034e8cf3` |
| Steam base game executable, if present under `SporeBin` | `f9716458cdc232a507686dcb490e75680c514e31a5b1f33601b0dfcd05baf75d` |

From the repository root:

```sh
mkdir -p "$HOME/Applications"
python3 -P tools/convert/convert.py "/path/to/prepared/Spore" -o "$HOME/Applications"
```

The converter installs Capstone into its local tools directory if needed. See `--help` for build stages, resuming and other options. Open **Spore Native (Steam)** in Applications. Keep the prepared game folder where it is. Saves and creations live under `~/Library/Application Support/spore-native-steam`.

## Display, frame rate and online play

Hold Option while opening the app to show launcher settings. For native resolution, enable **Full Retina resolution**, choose **Fullscreen**, and choose the **Display** resolution. The below-the-notch choice uses the safe area when available.

For uncapped rendering, choose **Unlimited**. To remove Spore's own frame limit too, back up the `ConfigManager.txt` files in `Data/Config` and `DataEP1/Config` in your prepared game folder. Set the existing `intProp frameLimitMS` entry to `0` in both files.

Leave **Connect to EA's servers** enabled for the in-game online features. Use Spore's in-game **Login** with an EA account registered for Spore. Account access depends on EA and the account's entitlement. Socket diagnostics can be enabled with `SPORE_NET_LOG=1`.

AZERTY key events are enabled by default in this fork. To use US key mapping, set `SPORE_KEYBOARD_LAYOUT=us` in the launcher environment. For ZQSD movement, your game data must have matching key bindings.

## Credits and licensing

The original source is from [garnold24/spo-re](https://github.com/garnold24/spo-re) under the [MIT license](LICENSE). The Creature Stage guard adapts [SporeCrashFix](https://github.com/Rosalie241/SporeCrashFix) and is GPL-3.0-only. The combined runtime with that guard is distributed under GPL-3.0-only, with the original MIT notices retained. See [third-party credits](THIRD_PARTY.md) and the [GPL license text](runtime/core/CREATURE-FIX-LICENSE.txt).
