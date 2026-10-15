# The app bundle and player defaults

`Spore.app` is how the game is played: a double-clickable, ad hoc signed macOS app built from the linked binary
`build/sporenative/spore`. The converter's bundle stage writes it where you asked (`-o`); built by hand,
`tools/bundle/make_bundle.py` writes `build/Spore.app`. The app holds only our runtime and launcher; the game
(exe, data, translated C, shader caches) stays in the Spore folder and in the work folder. The code is
`tools/bundle/` (`make_bundle.py`, `launcher.m`, `exe_icon.py`, `icon.m`).

## 1. Layout

```
Spore.app/Contents/
  Info.plist              com.spore-native.player, CFBundleName/CFBundleDisplayName "Spore" (the Dock, menu bar and
                          Finder name), version <nearest tag> (<git describe>), build <commits>,
                          NSHighResolutionCapable, LSMinimumSystemVersion = the binary's minos (26.0 now)
  PkgInfo
  MacOS/Spore Native      the launcher (tools/bundle/launcher.m), CFBundleExecutable
  MacOS/spore-game        a copy of build/sporenative/spore (any LC_RPATH removed)
  Resources/AppIcon.icns  the game's own icon, read out of the user's SporeApp.exe resources at build time by
                          tools/bundle/exe_icon.py and re-encoded (EA art: it exists only in the local build,
                          never in the repository); --own-icon draws one with tools/bundle/icon.m instead
  _CodeSignature/
```

The app holds a copy of the binary made at build time. **After every relink, run `make_bundle.py` again** so the
app has the new runtime (a symlink to the binary would break the signature).

## 2. Building and checking

```
python3 -P tools/bundle/make_bundle.py                    # the app, ad hoc signed, then the checks
python3 -P tools/bundle/make_bundle.py --verify-only      # checks only
python3 -P tools/bundle/make_bundle.py --after-link       # relink (tools/link/build.py --src build/harness/src --jobs 10,
                                                          # no game run), then bundle and check, in one command
python3 -P tools/bundle/make_bundle.py --list-gpu-profiles [GAME_DIR]
python3 -P tools/bundle/test_launcher.py                  # launcher tests on fake folder layouts
```

A build takes about 2 s (the 98 MB copy, the launcher and icon compiles, signing). The checks: `plutil -lint`;
the Info.plist keys; `codesign --verify --deep --strict`; `otool -L` and `LC_RPATH` of every Mach-O with no
`/opt/homebrew` or `/usr/local` path; no `Frameworks/`; no game-like file names; every
bundle file compared with every file of the Spore folder (size, then SHA-256); and 32 sampled 64-byte windows
of `SporeApp.exe` searched for in every bundle file (the positive control: `build/sporenative/spore-embed`,
which embeds the image, matches 29 of 29 windows; `spore` matches 0); an `iconutil -c iconset` round trip of
`AppIcon.icns` with every representation's pixel size. The listing is printed, then the bundle's
version from its own launcher (`--version`).

**Guards.** `--after-link` runs `pgrep -fl 'spore-game|sporenative/spore --run'` first and refuses to
relink while any game process exists. Every build refuses to
replace an output bundle whose `Contents/MacOS/spore-game` is running (that would delete files under a live
process). `--force` overrides both. To build while the game runs, use `--out build/bundle/<topic>/Spore.app`.

The player's guide (first run, where files go, settings, Terminal use, resetting, the icon cache) is
`tools/bundle/README.md`.

## 3. The launcher

`Contents/MacOS/Spore Native` is a small Objective-C program. It sets up the environment, then `execv`s
`spore-game --run [-- args]` in the same process, so the Dock icon, menu bar name and privacy prompts belong
to one app.

**Command line.** The launcher accepts only its own options: `--help` (usage on stdout, exit 0),
`--version`, `--game-dir PATH` (or `--game-dir=PATH`: checked, normalised like a pick, remembered, and used
ahead of everything else; a wrong folder exits 1), `--reset-game-dir` (deletes `<support>/game-dir`; alone it
then exits 0, with `--game-dir`, `--resolve-only` or `--print-env` it goes on), `--print-env`, `--resolve-only`,
`--no-panel`, and `--`, after which everything goes to the game. The game reads its command line only from
`SPORE_ARGS` (`w32_module.c` builds `"<exe path>" <SPORE_ARGS>`; `sg_boot` ignores `spore-game`'s own
arguments), so the launcher joins the arguments after `--` and the stored `Arguments` into `SPORE_ARGS`
(§10). LaunchServices' `-psn_…` and Xcode's `-NSDocumentRevisionsDebugMode X` are skipped. Anything else, an
option or a bare word, is refused with the usage text on stderr and exit 2. The launcher parses before it opens
the log or looks for a folder, so a mistake is reported on the terminal and nothing starts. (Passing unknown
arguments on to the game would let a mistyped `--help` start the real game.)

**Spore folder.** First match wins. Each candidate must hold `SporebinEP1/SporeApp.exe` with the GOG EP1
SHA-256 (the same constant the binary checks, `SG_IMAGE_SHA256`, compiled in), plus `Data/` and `DataEP1/`.

0. `--game-dir PATH`, when given.
1. `SPORE_GAME_DIR`: an explicit variable wins over the remembered folder. An invalid value is logged and the
   search goes on.
2. The folder remembered in `<support>/game-dir`, where `<support>` is `SPORE_SUPPORT_DIR` or
   `~/Library/Application Support/spore-native`.
3. The usual places: `~/Downloads/spore-gog`, `~/Downloads/Spore`, `~/Applications/Spore`,
   `~/Applications/Spore Galactic Adventures`, `~/GOG Games/Spore…`, `~/Games/Spore`, `/Applications/Spore…`,
   `/Applications/GOG Galaxy/Games/Spore`, `/Users/Shared/GOG Galaxy/Games/Spore`, `/Users/Shared/GOG Games/Spore`.
4. An `NSOpenPanel` ("Where is Spore?") asking for the folder that contains SporebinEP1. A wrong folder gets an
   alert with the reason and the panel again. Cancel quits without a message.

A pick is normalised: the SporebinEP1 folder itself is accepted (its parent is used), as is a folder with
exactly one install directly inside. A found or picked folder is written to `game-dir`; an environment
override is not.

**Environment given to the game** (a variable already set always wins):

| Variable | Value | Why |
|---|---|---|
| `SPORE_GAME_DIR`, `SPORE_EXE` | the install, its `SporebinEP1/SporeApp.exe` | the binary's defaults point into the source tree it was built in |
| `SPORE_DOCUMENTS_DIR` | `~/Documents` (or the `DocumentsFolder` setting) if "My Spore Creations" can be made and listed there | §4 |
| `SPORE_D3D9MSL_CACHE` | `~/Library/Caches/com.spore-native.player/shadercache` | the Metal back end's MSL cache |
| `SPORE_REPO_ROOT` | `~/Library/Caches/com.spore-native.player` | any other state a back end keeps under `$SPORE_REPO_ROOT/build/…` (the Metal pipeline archive) lands in Caches, not in the source tree |
| `SPORE_MISS_LOG` | `~/Library/Logs/Spore Native/dispatch-misses.txt` | the default is inside the source tree |
| `SPORE_GPU_PROFILE` | from the `GPUProfile` setting, checked against the install's `VideoCards.txt` | §5 |

`SPORE_D3D9` is left unset, so the runtime picks Metal. No watchdog, stop or test variable is set: a run
of the app is an interactive run. The working directory is the support folder.

**Settings** (`defaults write com.spore-native.player KEY VALUE`; `SPORE_DEFAULTS_DOMAIN` names another domain,
which the tests use): `GPUProfile` (string), `DocumentsFolder`
(path, `~` allowed), `Environment` (a dictionary of extra variables, for example
`defaults write com.spore-native.player Environment -dict SPORE_ONLINE 0`).

**Logs.** stdout and stderr (the launcher's lines, then everything the runtime prints) go to
`~/Library/Logs/Spore Native/YYYY-MM-DD HH-MM-SS.log`, one file per run, the newest 30 kept. Console.app shows
them under Log Reports. `SPORE_LOG_FILE=-` keeps the terminal; `SPORE_LOG_FILE=path` picks the file;
`SPORE_LOG_DIR` moves the folder. A run of the bare binary (`build/sporenative/spore --run`) does not go through
the launcher and logs to the terminal.

**Test hooks.** `--resolve-only` prints `<dir>\t<source>` and exits; `--print-env` prints what would be exec'd;
`--no-panel` / `SPORE_LAUNCHER_NO_PANEL` never shows UI; `SPORE_LAUNCHER_TARGET` replaces the game binary;
`SPORE_LAUNCHER_PANEL_TEST` shows the panel and cancels it after 1.5 s; `SPORE_DEFAULTS_DOMAIN` reads the
settings from another domain.

**The tests never touch the player's settings.** Every launch in `test_launcher.py`
sets `SPORE_DEFAULTS_DOMAIN=com.spore-native.player.test`. That covers the launcher's own reads, but AppKit
writes the open panel's `NSNavPanelExpandedSizeForOpenMode` and `NSOSPLastRootDirectory` into the *main
bundle's* domain, whatever the launcher reads. So the panel check runs a copy of the launcher inside a
throwaway `build/bundle/test/LauncherTest.app` whose `CFBundleIdentifier` is that test domain. A bare copy with
no bundle is no better: AppKit then writes into the shared
`com.apple.ViewBridge.masquerading-service-lacks-host-bundle-identifier` domain [obs, one probe]. The test
exports the real `com.spore-native.player` and notes its plist's mtime before and after, and fails if either
changed. It deletes the test domain and its empty leftover plist at the end.

## 4. Player defaults: where files go

| What | Bare-binary runs (`spore --run`) | The app |
|---|---|---|
| "My Spore Creations" | `~/Library/Application Support/spore-native/Documents/` | **`~/Documents/My Spore Creations`**, the game's own Windows location |
| AppData (saves `Games/`, `EditorSaves.package`, preferences, resolution) | `~/Library/Application Support/spore-native/AppData/` | same |
| registry file, `drive_c` | Application Support | same |
| logs | the terminal | `~/Library/Logs/Spore Native/` |
| shader cache | `build/d3d9metal/shadercache/` | `~/Library/Caches/com.spore-native.player/shadercache/` |

The app uses `~/Documents/My Spore Creations`. That is where Spore puts it on Windows, where players and the
Sporepedia's PNG creation cards expect it, and it is visible in Finder. Bare-binary runs keep the Application
Support sandbox, so a test run can never write into the player's real Documents.

The app's first run triggers macOS's "Spore would like to access files in your
Documents folder" prompt; the launcher makes the first access (creating "My Spore Creations"), so a refusal
falls back to the sandbox location instead of failing inside the game. Saves made by bare-binary runs carry
over (AppData is shared), but creation cards and pictures they made are in the sandbox's "My Spore Creations";
the launcher logs a hint, and either copying them across or `defaults write com.spore-native.player
DocumentsFolder "~/Library/Application Support/spore-native/Documents"` keeps everything in one place. The launcher
moves nothing itself.

## 5. GPU profile

`SPORE_GPU_PROFILE` (or the `GPUProfile` setting) chooses the identity `GetAdapterIdentifier` reports. All
three are NVIDIA SM 3.0 cards in the game's `VideoCards.txt`. Driver build 117741 (ForceWare 177.41, mid-2008,
the GTX 280's launch driver), caps, 768 MB of texture memory and one GPU stay the same, so the Metal back end
honours every one with no other change.

| Profile | Device | `VideoCards.txt` name [obs, read at run time] | `ConfigManager.txt` rule it hits [obs] |
|---|---|---|---|
| `8800gtx` (default) | 0x0191 | GeForce 8800 GTX | `*88?0*` → `gpuLevel High` |
| `9800gtx` | 0x0612 | GeForce 9800 GTX | `*98?0*` → `gpuLevel High` |
| `gtx280` | 0x05E1 | GeForce GTX 280 | `*GeForce GTX*` → `gpuLevel High` |

On this install **the game changes none of its options for any of the three.** In the GOG install's
`ConfigManager.txt`, all 42 `seti gpuLevel` assignments set `$gpuLevelHigh` [obs, `grep`], so the card name
never lowers a level. The other identity inputs (`driverBuild` for `sporeProfile`/`oldDriver`, `gpuCount`,
`textureMemory < 125`, the vendor checks) are the same for all three. What does change is the card name and
device id the game reads: the `=== Card …` trace line, and anything in the exe that branches on the device id
directly (none found; the exe's strings reach the identity only through the script variables `graphicsCard`,
`graphicsCardIDs`, `driverBuild`, `textureMemory` [inf]). The setting still gives a real choice to a player
with an older `ConfigManager.txt` whose card rules use Medium and Low, and the 8800 GTX stays the default so
nothing changes unless asked. `make_bundle.py --list-gpu-profiles` reprints the table from any install.

**Runtime half.** `d3d9null_fill_identifier` in `runtime/d3d9null` fills the identity for every back end: one
table, one `getenv`, the 8800 GTX when the variable is unset or names an unknown profile (which
logs a message).

## 6. Metal only

The Metal back end needs nothing outside the system: `otool -L` of the game binary lists only AppKit,
QuartzCore, Metal, CoreAudio, AudioToolbox, GameController, CoreHaptics, Foundation, CoreFoundation,
CoreGraphics, libobjc and libSystem, and it has no `LC_RPATH` (the bundle deletes any it finds). So the game
runs on a Mac without Homebrew, and the bundle has no `Frameworks/`.

## 7. Signing, Gatekeeper and privacy prompts

Ad hoc signing (`codesign --force --sign -`): `spore-game` first, then the bundle,
which signs the launcher as its main executable. A locally built bundle has no quarantine attribute, so
Gatekeeper does not stop it. macOS's privacy control (TCC) identifies an ad hoc signed app by its code hash,
so **each rebuild may ask again** for Downloads (when the install is in `~/Downloads`) and
Documents. A Developer ID signature would end that; it is not needed for an app built on the Mac that runs it.
`Info.plist` carries the usage strings for the Documents, Downloads and Desktop prompts.

The launcher creates an `NSApplication` only when it has to show the panel or an alert, then `exec`s. When
the panel is shown (first run only), the game starts in a process that has already checked in with the window
server. `exec` replaces the process image, and the game's `sp_mac_app_init` creates its own `NSApplication`
and checks in again under the same pid.

## 8. Icon

The GOG exe's largest icon is 256 px, so the `.icns` holds 16–256 px at 1x and 2x (seven representations) and
no 512 or 1024 representation. Finder's largest icon views and Get Info scale the 256 px image up. The Dock is
at most 128 pt (256 px on Retina), which the set covers. Upscaling at build time would add nothing over Finder's
own scaling, so the build does not upscale. A stale icon after a rebuild is Finder's cache; the remedies are in
`tools/bundle/README.md`.

## 9. What the tests cover

`test_launcher.py` runs the launcher on fake folder layouts with `SPORE_LAUNCHER_TARGET` standing in for the
game: no install, wrong hash, no DataEP1, a usual place found and remembered, remembered beats usual places,
environment beats remembered (and is not remembered), SporebinEP1 and parent-folder picks normalised, invalid
environment and stale remembered folders fall through, the environment's contents, the GPU profile check against
`VideoCards.txt`, exec with arguments, logs (dated file, pruning, terminal opt-out), the open panel shown and
cancelled in the throwaway domain, the command-line and `--game-dir`/`--reset-game-dir` cases, and the settings
window (§10). Each run checks that the real `com.spore-native.player` domain is unchanged.

## 10. The settings window

The launcher shows a settings window before the game starts, built in AppKit code in `launcher.m` with no
nib. The model is the Kenshi launcher: a banner, a tab bar, label-and-control rows, buttons along the
bottom.

**Layout.** A titled, closable, fixed-size window, 640 pt wide and centred. Its title
is "Spore - Launcher v<version>", the version without the git hash (the hash stays in `--version`). Its height
fits the content: the tallest tab's grid (`fittingSize`), the tab view's own chrome, the banner and
the bottom rows. With the rows below that is 640 by 624 pt including the title bar (measured on a 16-inch
MacBook Pro). The banner holds the bundle's `Contents/Resources/AppIcon.icns` at 96 pt, the title
"Spore" and one line, "Choose how Spore starts. Play keeps your choices for next time." Under the banner is an
`NSTabView` with three tabs: General, Game options and Diagnostics. Each tab is an `NSGridView` of two columns.
The label sits on the left, 210 pt wide. The control sits on the right. Every popup, the Resolution box and the
Other start-up options field are 270 pt wide, so the controls line up. Rows are 8 pt apart and there is no
visible helper text in the tabs: each row's description is a tooltip (below). The "Show window every launch"
checkbox is under the tabs. The bottom row has Reset to defaults and Copy launch command on the left, and Save,
Cancel and Play on the right. There is no footer text; the version is in the title.
Return is Play and Escape is Cancel. No field takes focus when the window opens. The window uses only system
colours and fonts, so light and dark mode both work. A minimal menu bar gives Quit Spore (Cmd-Q) and the text fields'
Cmd-C/V/X/A. The alerts are titled "Spore cannot start" and "Spore cannot start the game". The executable keeps the
name "Spore Native", and `--help` and `--version` still use that project name.

**Copy rules.** Labels are in sentence case and say what the row is in plain words. No label carries
parentheses or a raw flag: a checkbox has no title, and its row label carries the meaning. Each description is a
tooltip, the standard AppKit `toolTip` set to the same sentence on the row label and on its control (on the Change…
button for the two folder rows, whose path label keeps the full path as its tooltip). macOS shows it after its
usual delay. A tooltip is one sentence and never mentions a document or a file path. A row disabled because the
launcher's environment sets its variable still shows the environment's value; its tooltip becomes "Set by the
Terminal command that started the app." `--list-settings` prints each row's tooltip as a sixth column. The rows
whose tooltip the table below leaves out have these:

- Spore folder: "Your GOG Spore Galactic Adventures folder, the one with SporebinEP1, Data and DataEP1."
- Fullscreen or window: "Overrides the fullscreen choice saved in the game's own options."
- Language: "The languages your Spore install has; System language follows your Mac."
- Sound: "Overrides the speaker setup, or turns the game's sound off."
- Allow several copies at once: "Lets a second copy of Spore start while one is running."
- Log network activity: "Writes the game's network activity to the log."
- Show window every launch: "Off goes straight to the game; hold Option at start to see this window again."

The first item of every popup is the default. No item says "(Default)"; each first item says what happens
instead: Vsync, Off, As set in the game, As usual, System language. Start in's first item is "Default".
Reset to defaults puts every row back on its first item.

**Rows.** `k_rows[]` in `launcher.m` is the one table. Each row is {tab, label, kind, switch, default,
written value, choices, tooltip}. The game options rows are a second table, `k_game_rows[]`, built from the exe's
own command-line parser (prefix `-` or `/`, names case-insensitive, a value only as `-name:value` in the same
token). `-vSync`/`-noVSync` are not offered, because Frame rate covers them and the Metal back end ignores the
game's presentation interval ([DEVIATIONS](../DEVIATIONS.md) 3.14). `-noPlugins`, `-nofocus` and `-silent` are
not offered either. `--list-settings` prints every row as
`tab<TAB>label<TAB>switch<TAB>default<TAB>choices<TAB>tooltip`.

| Tab | Row | Switch | Default (nothing written) | Written when changed |
|---|---|---|---|---|
| General | Allow fullscreen | `SPORE_FULLSCREEN` | on | `0`. Tooltip: "Off keeps the game in a window even when it asks for fullscreen." |
| General | Esc also leaves fullscreen | `SPORE_FULLSCREEN_ESC` | off | `1`. Tooltip: "Cmd-Ctrl-F always switches between fullscreen and a window." |
| General | Full Retina resolution | `SPORE_RETINA` | off | `1`. Tooltip: "Sharper, but the game's menus and text are half as big." |
| General | Frame rate | `SPORE_VSYNC` + `SPORE_D3D9_FPS` | Vsync | 30 fps `0`+`15`, 40 fps `0`+`20`, 60 fps `0`+`30`, Unlimited `0`+`0` |
| General ("Online and folders") | Connect to EA's servers | `SPORE_ONLINE` | on | `0`. Tooltip: "Needed for Sporepedia sharing and your EA account." |
| General | Spore folder | `<support>/game-dir` | found or remembered | the open-panel flow of §3, remembered on Save/Play |
| General | Save creations in | `DocumentsFolder` key | `~/Documents` | the chosen folder. Tooltip: "Your creations go in a "My Spore Creations" folder here." |
| Game options | Start in | `Arguments` (`STATE`), plus `SPORE_SKIP_INTRO` | Default | "Skip intro": `SPORE_SKIP_INTRO=1` in `Environment` and no `-state:` (a runtime switch, [DEVIATIONS](../DEVIATIONS.md) 2.14). Otherwise `-state:<Name>` from `k_states[]`: the editors, the plant editor and the stages (the game's own start-state script). A stored name the table lacks is kept as a custom item |
| Game options | Fullscreen or window | `Arguments` (set-hook key `WINDOW`) | As set in the game | `-w` (In a window) or `-f` (Fullscreen), never both (`-f` wins in the exe) |
| Game options | Resolution | `Arguments` (`RES`), plus `SPORE_FULLSCREEN_SAFE_AREA` | empty: the game's saved one | `-r:WxH` from a combo box (see "Resolution and the notch"), or any `<int>x<int>` typed (nothing else is written). The below-the-notch item also writes `SPORE_FULLSCREEN_SAFE_AREA=1`. Tooltip: "Sizes the display can show; the game saves the one you pick." |
| Game options | Language | `Arguments` (`LOCALE`) | System language | `-locale:ll-cc`. The choices are the folder names under `<game>/Data/Locale`, shown by name ("French (France)", from `NSLocale`) and sorted by name. They are read when the window opens, and again after Change… of the Spore folder. `-l:Name` is not offered |
| Game options | Sound | `Arguments` (`SOUND`) | As usual | `-mono`, `-stereo` or `-noSound` |
| Game options | Allow several copies at once | `Arguments` | off | `-multipleInstances` |
| Game options | Safe mode | `Arguments` | off | `-safe`. Tooltip: "Low graphics and sound, preferences not saved, no plugins." |
| Game options | Other start-up options | `Arguments` (`ARGS`) | empty | split at blanks; quotes group. Tooltip: "Game options, separated by spaces; -5.1 gives surround sound." |
| Diagnostics | Log network activity | `SPORE_NET_LOG` | Off | `1` (Connections), `2` (Connections and all data sent and received) |
| Diagnostics | Stall watchdog | `SPORE_STALL_SECONDS` | empty (off) | a positive number. Tooltip: "Stops a frozen game after this many seconds and logs a report." |
| Diagnostics | Metal statistics | `SPORE_METAL_WATCH` | empty (off) | a positive number. Tooltip: "How often, in seconds, frame timings are written to the log." |
| Diagnostics | Keep the graphics pipeline cache | `SPORE_METAL_PSO_ARCHIVE` | on | `off`. Tooltip: "Off rebuilds it at every start, so scenes load more slowly." |
| (under the tabs) | Show window every launch | `ShowSettings` key | on (absent) | `false` |

**Rows the window does not have.** The graphics card profile (`GPUProfile`, `SPORE_GPU_PROFILE`, §5) and
the input switches `SPORE_TEXT_INPUT`, `SPORE_PRESS_AND_HOLD` and `SPORE_GAMEPAD` are set from Terminal only.
The window neither shows nor writes them. A stored `GPUProfile` survives Save, Play and Reset to defaults, and
`gpu_profile()` still applies it. There is no renderer row: Metal is the one renderer, and `set:SPORE_D3D9=...`
is refused.

**Tabs and headings.** General holds display rows and setup rows (online play and the two folders), so it uses
two headings (`k_heads[]`): "Display" and "Online and folders". A heading is 15 pt semibold, larger than the
13 pt row labels, with a thin separator line (`NSBoxSeparator`, a grid row of its own) under it across the grid,
and 16 pt of space above it when it is not the first. Game options reads as one list of start-up choices, so it
has no headings. Diagnostics is short and has no headings either.

**Start in.** The Game options tab's first row, "Start in", is a popup. It writes the exe's
`-state:<Name>` (names case-insensitive) as the first entry of `Arguments`. The
game then starts in that app state, for example straight in the Creature Creator, instead of the intro and the
galaxy menu. The popup's items come from one static table, `k_states[]` ({name, label, note}, `{NULL}` for a
separator). First comes "Default", which writes nothing. Second comes "Skip intro", which writes
`SPORE_SKIP_INTRO=1` into `Environment` and no `-state:`, so the game starts at the galaxy menu without the intro
movie. Any other choice removes the variable, unless the launcher's
own environment set it. A stored `SPORE_SKIP_INTRO=1` with no `-state:` reloads as Skip intro, and the row's
switch column reads `STATE+SPORE_SKIP_INTRO`. A separator follows, then the eleven editors, a separator, the
plant editor (experimental), a separator and the five stages (experimental). The list comes from
the game's own start-state script, `SporeAppStates.trigger`. It leaves out GalaxyGameEntry, which is the normal
start and plays the intro. The tooltip of the popup and its label follows the selection:

- On Default: "Choose an editor or a stage to open it without the intro."
- On Skip intro: "Starts at the galaxy menu without the intro movie."
- On an entry with its own note in `k_states[]`, that note.
- On any other entry: "Opens straight there, skipping the intro and the galaxy menu."

The stages share the tooltip "No saved game is loaded; may show an empty world or crash." The title
movie starts inside the galaxy-entry mode, which a `-state:` skips. On load, a stored `-state:X` goes back to the
popup, never to Other start-up options. A name the table does not have shows as "X (custom)", with the tooltip "Not in
this list; passed to the game as it is.", and is written back unchanged. The set hook's key is `STATE`
(`set:STATE=CreatureEditor`; `set:STATE=skip` or `set:SPORE_SKIP_INTRO=1` for Skip intro). Changing the list
means editing `k_states[]` alone.

**Resolution and the notch.** The notch has no option of its own. The Resolution row is an
`NSComboBox` whose list is built from the main display (`NSScreen.screens[0]`, the display the runtime reports).
Its items, in order:

1. "Display (WxH)": the display's size.
2. "Display below the notch (WxH)": the display less `safeAreaInsets.top`. This item appears only when the inset
   is not 0, so never on an external display.
3. The common sizes from 2560x1600 down to 1024x768 that fit the display.

The units follow the Retina checkbox: points, or pixels with Full Retina resolution on
([DEVIATIONS](../DEVIATIONS.md) 4.8). The list is rebuilt when the checkbox changes, and a display item in the
box becomes the same item in the new units. On a 16-inch MacBook Pro at its largest scaled resolution, with its 38 pt inset:

| Full Retina resolution | Display | Display below the notch |
|---|---|---|
| off | 2056x1329 | 2056x1291 |
| on | 4112x2658 | 4112x2582 |

Fullscreen accepts a size only if the adapter enumerates it: the standard sizes up to the desktop mode the runtime
reports, plus that mode itself. For anything else the game falls back to windowed (code at 0x75e9a0;
[DEVIATIONS](../DEVIATIONS.md) 4.12). The desktop mode is the display itself, in points or pixels, or its safe
area with `SPORE_FULLSCREEN_SAFE_AREA` ([DEVIATIONS](../DEVIATIONS.md) 3.47); `SPORE_DISPLAY_MODE=1920x1080@60`
fixes it instead. So both display items are sizes fullscreen accepts.

Choosing the below-the-notch item writes `-r:WxH` and also `SPORE_FULLSCREEN_SAFE_AREA=1` into `Environment`. The
runtime then fits the fullscreen picture below the notch and keeps the notch strip black
([DEVIATIONS](../DEVIATIONS.md) 4.12). Choosing anything else, typing a size or clearing the box removes the
variable. A variable set in the launcher's own environment wins and is left alone. The combo box shows the
item's whole title, and only its `WxH` is stored. Any text can be typed; the launcher validates, stores and reloads it. On
load, a stored `-r:` equal to the below-the-notch size, with the variable set, selects that item. One equal to
the display's size shows as the Display item. Any other size comes back unchanged, and a stored variable without
its item is dropped on the next Save or Play. `--list-settings` prints the list for the current `SPORE_RETINA`
(environment, else stored), then ` -- `, the units, the display's size in points and its notch inset. The switch
column of the row reads `RES+SPORE_FULLSCREEN_SAFE_AREA`, and `set:` takes either key: `RES=<item or WxH>`, or
`SPORE_FULLSCREEN_SAFE_AREA=1`, which picks the below-the-notch item after the other rows are set.

**Frame pacing.** The presets come from the pacing code. In the Metal back end (`pace()` in `mt_present.mm`),
`SPORE_D3D9_FPS=N` is a floor of half a period between presents,
`500000000 / N` ns: a frame that comes back sooner sleeps the rest. It is not a cap at N. With vsync off, the
rate is therefore 2N: N=15 gives 33.3 ms and 30 fps (measured), N=20 gives 25 ms and 40 fps, and
N=30 gives 16.7 ms and 60 fps. N=0 removes the floor. The game still limits itself to about 59 fps
(its `frameLimitMS 16`), so Unlimited and 60 fps differ little. The default (vsync on, N=60) is an
8.3 ms floor under the display's own pacing. The popup's tooltip reads "Menu panels slide differently below 50
fps; 40 fps is closest to how they felt on older PCs."; in play the panels look right at about 40 fps. A stored
pair that matches no preset shows as a custom choice and is kept unchanged.

**Storage.** Everything goes through CFPreferences to `pref_domain()` (`com.spore-native.player`, or
`$SPORE_DEFAULTS_DOMAIN` for the tests). `Environment` holds only non-default values. A row set back to its
default removes its key, an empty dictionary removes `Environment`, and keys that no row owns are kept.
`DocumentsFolder` is removed at its default; `GPUProfile` is never written by the window. `Arguments` is an
array of strings, in row order. When the window loads, each stored argument goes back to the row that writes
it, and the rest fill "Other start-up options".

**How the arguments reach the game.** The runtime builds the game's command line as
`"<exe path>" <SPORE_ARGS>` (`runtime/win32/src/w32_module.c`). `sg_boot` (`core_boot.c`) discards the
arguments `spore-game` itself receives after `--`, so arguments must travel in `SPORE_ARGS`. Before exec, the
launcher joins the stored `Arguments`, then the arguments after its own `--`, with single spaces into
`SPORE_ARGS`. It still also passes them after `--`, so `--print-env` and test targets show them. The join
follows the game's tokenizer (0x92bab0: blanks split outside double quotes, no escapes). A value with blanks is
quoted after its colon, as in `-dataDir:"C:\My Data"`, and a bare word with blanks is quoted whole. Only ASCII is
passed, because the exe widens the line byte by byte. An argument with other characters, or with a double quote
inside, is dropped with a log line. A `SPORE_ARGS` already in the launcher's environment wins: the game options
rows are then disabled, show its contents, and Save keeps the stored `Arguments`. `ShowSettings`
is written only as `false`. A variable already in the launcher's own environment wins. The window
shows that row with the environment's value, disabled and marked "(set by the environment)", and Save leaves
the stored value for that row alone.

**Flow.** The launcher parses its options first. `--list-settings`, `--help` and `--version` exit at once.
`--resolve-only`, `--print-env`, `--reset-game-dir` alone, `--no-panel`, `SPORE_LAUNCHER_NO_PANEL` and
`--no-settings` never show the window. Otherwise the window shows when `ShowSettings` is absent or true, when
`--settings` is given, or when Option is held at start. The Spore folder is resolved as in §3, so a first run
still asks with the open panel, and then the window shows. Cancel, the close button and Cmd-Q exit 0 with
nothing written and nothing started. Play writes the settings and then undoes what the stored settings put
into the environment before the window showed (except variables the launcher was started with). It then
applies the domain again through `apply_pref_environment()` and `gpu_profile()`, the same path as a start
without the window, and execs the game. The log is opened only after the window closes, so a Cancel leaves no
log file. In this case the folder search before the window reports to the terminal, not the log.

**Copy launch command** puts one shell line on the pasteboard for bug reports: the non-default variables as
`NAME=value` (`SPORE_DOCUMENTS_DIR` for the folder key, `SPORE_FULLSCREEN_SAFE_AREA=1` for the below-the-notch
item), `SPORE_ARGS='<arguments>'`, the
quoted launcher path, `--no-settings`, and `-- <arguments>`.

**Test hooks.** `SPORE_LAUNCHER_SETTINGS_TEST` acts 1.5 s after the window shows, through
`performSelector:afterDelay:inModes:` in the modal and default run-loop modes. The values are `play`,
`cancel`, `save-then-play`, `reset` (Reset to defaults, then Play) and `set:KEY=VALUE,...`. `set:` sets rows
by switch name (`FPS` for the pacing preset, `ShowSettings`, `game-dir`; for the game options
`WINDOW=f`, `RES=1280x800` or `RES=Display (WxH)`, `SPORE_FULLSCREEN_SAFE_AREA=1`, `LOCALE=fr-fr`, `SOUND=mono`,
`-multipleInstances=1`, `-safe=1`, `ARGS=...`), prints the launch command on stderr and presses Play. A disabled
choice can be set this way. A switch with no row is refused ("no row for ...", exit 2, nothing started);
`test_launcher.py` checks that for `SPORE_D3D9`, which has no row. `SPORE_LAUNCHER_SETTINGS_TAB=N` opens on tab
N (0 to 2), for screenshots.

`test_launcher.py`'s settings section runs these in its throwaway bundle: `--list-settings` lists every switch,
and the Language row lists the install's `Data/Locale` folders; every `SPORE_*` row is documented; an empty domain
gives the window, Play, and nothing added or written; `set:` writes exactly the chosen variables, with
`--run -- -f -r:1280x800 -locale:fr-fr -mono -multipleInstances` and the same string in `SPORE_ARGS`, and
`Environment` with only the non-default keys; the environment wins; a value with a blank is quoted after its
colon; a `SPORE_ARGS` in the environment wins; Cancel writes nothing and leaves no log; Reset leaves no
`Environment`, `Arguments` or `GPUProfile`; `ShowSettings` false, `--settings`, `--no-settings`,
`SPORE_LAUNCHER_NO_PANEL`; the labels (no parentheses, no raw flag) and the launcher binary's strings; Skip intro
and the below-the-notch item written, reloaded and removed; the Resolution list in points and in pixels; a
Terminal-written `GPUProfile` surviving Play and Reset; no `SPORE_D3D9` row.

**A Mods tab** would be a fourth `NSTabViewItem`, added in `buildWithGame:` after the three grids (the comment
there marks the place). It would get its own view, a list rather than a grid, and its own storage key. The
`Environment` and `Arguments` rows do not need to change for it.
