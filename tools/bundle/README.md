# Spore Native: playing from Spore.app

`Spore.app` is a Mac app that runs **your own copy of Spore Galactic Adventures from GOG** natively on Apple
Silicon. It contains the Spore Native runtime and a small launcher, nothing from the game: no exe, no data,
no art apart from the icon, which is read out of your own `SporeApp.exe` when the app is built and never
leaves your Mac. The game is read from your Spore folder each time it starts, and that folder is never changed.

This folder holds the app's builder (`make_bundle.py`), its launcher (`launcher.m`), the icon tools (`exe_icon.py`,
`icon.m`) and the launcher's tests (`test_launcher.py`). The design notes for developers are in
`docs/design/bundle.md`.

## Building it

The converter (see the top-level README) builds the app for you. By hand, from the top of the source tree, after
the game binary has been linked:

```
python3 -P tools/bundle/make_bundle.py --after-link   # relink the game binary, then build build/Spore.app
python3 -P tools/bundle/make_bundle.py                # build build/Spore.app from the binary already linked
```

The app draws with Apple's Metal and carries no other library: nothing beyond macOS is needed to run it.

Each prints the version at the end (`Spore Native <tag> (<git describe>)`, e.g. `0.1.0 (v0.1.0)`) and runs its checks. **Quit the game
first.** No command relinks or replaces an app while its game is running; they stop with a message
instead. Move or copy `build/Spore.app` wherever you like, for example to `/Applications`.

## First run

1. Double-click `Spore.app`.
2. **Finding Spore.** The launcher looks for the GOG install in `~/Downloads/spore-gog`, `~/Downloads/Spore`,
   `~/Applications/Spore…`, `~/GOG Games/Spore…`, `~/Games/Spore`, `/Applications/Spore…` and the GOG Galaxy
   folders. If it finds none, a window asks **"Where is Spore?"**: choose the folder that contains
   `SporebinEP1`, `Data` and `DataEP1`. Choosing `SporebinEP1` itself, or a folder with exactly one Spore
   install inside, also works. The launcher checks that `SporebinEP1/SporeApp.exe` is the GOG Galactic Adventures
   build and remembers the folder. Cancel quits.
3. **The settings window** opens (see Settings below). Press **Play** to start the game; the defaults are
   right for most players.
4. **Privacy prompts.** macOS asks once whether Spore may use:
   - **Downloads**, if your Spore folder is there. Allow it, or the game cannot be read.
   - **Documents**, for `My Spore Creations`. If you refuse, creations go to the Application Support folder
     instead (below), and the game still runs.

   The app is signed only for this Mac ("ad hoc"), so macOS may ask again after each rebuild.
5. The first start of each scene is a little slower while the shader cache fills. Later starts reuse it.

## Where things go

| What | Where |
|---|---|
| Your creations, creature cards and pictures | `~/Documents/My Spore Creations`, where Spore puts them on Windows |
| Saved games, editor saves, the game's options and resolution | `~/Library/Application Support/spore-native/AppData/Spore/` (`Games/`, `EditorSaves.package`, `Preferences/`) |
| The remembered Spore folder | `~/Library/Application Support/spore-native/game-dir` |
| Registry values and the game's `C:` drive files | `~/Library/Application Support/spore-native/` (`registry.json`, `drive_c/`) |
| Logs, one per start, the newest 30 kept | `~/Library/Logs/Spore Native/` (Console.app shows them under Log Reports) |
| Shader cache | `~/Library/Caches/com.spore-native.player/` (safe to delete; it is rebuilt) |

To back up your progress, copy `~/Library/Application Support/spore-native/AppData` and
`~/Documents/My Spore Creations`.

## Playing

- **Fullscreen** follows the fullscreen setting in the game's own options. It covers the screen without changing the
  display's resolution, with black bars when the shapes differ. **Cmd-Ctrl-F** switches between fullscreen and
  a window. **Esc** belongs to Spore's menus and does not leave fullscreen. **Cmd-Tab** brings back the menu bar
  and the Dock.
- **Quit** from the game's menu. Cmd-Q and Quit in the Dock ask the game to close, as the window's close
  button does.
- Online features (Sporepedia sharing, the EA account) behave as on a Windows PC with no network.

## Settings

Each time the app starts, a settings window opens before the game. It has three tabs:

- **General**
  - Under **Display**: whether fullscreen is allowed, whether Esc also leaves fullscreen, full
    Retina resolution and the frame rate.
  - Under **Online and folders**: whether the game connects to EA's servers, your Spore folder, and where
    "My Spore Creations" is saved.
- **Game options:** Spore's own start-up options.
  - **Start in** opens the game straight in an editor (creature, cell, building, vehicles, spaceship,
    outfitters) instead of the intro and the galaxy menu. The plant editor and the stages are offered as
    experimental. **Skip intro** starts at the galaxy menu without the intro movie. Leave it on "Default" to
    start the way you always did.
  - Fullscreen or a window, resolution, language (the languages your install has) and sound (mono, stereo or
    none).
  - Several copies at once, and safe mode.
  - A field for any other game option.
- **Diagnostics:** network logging, the stall watchdog, Metal statistics and the graphics pipeline cache.

Rest the pointer on a row to see what it does. The first choice in every list is the default, and a row you leave
alone stays at it.

**Resolution.** The list is made from your display. **Display** is its full size. On a MacBook with a notch,
**Display below the notch** is the area under it: the game's picture then fits below the notch, and the strip
beside the notch stays black. After those come the common sizes that fit. The sizes are in points, or in pixels
with **Full Retina resolution** on, and the list changes when you tick that box. You can still type any
WIDTHxHEIGHT. The game runs in a window when it does not accept the size you typed.

**Play** saves your choices and starts the game. **Save** saves them and keeps the window open. **Cancel** (or
Escape) quits without saving and without starting the game. **Reset to defaults** puts every row back. The reset
is saved when you press Save or Play. **Copy launch command** copies one Terminal line with your settings, for a
bug report. The window's title shows the launcher's version.

Clear **Show window every launch** to go straight to the game. To see the window again, hold Option while you
start the app, or start it from Terminal with `--settings`.

**Frame rate.** "Vsync" is the default and matches your display. The 30, 40 and 60 fps choices turn vsync off and
limit the frame rate. "Unlimited" removes our limit, but Spore still holds itself to about 59 fps. Spore's sliding
menu panels move differently below 50 fps. At 40 fps they are closest to how they felt on old Windows PCs.

**Renderer.** There is no renderer choice: Spore always draws with Apple's Metal.

**From Terminal instead.** The window stores everything with `defaults` under `com.spore-native.player`, so
you can write the same settings by hand. Some settings have no row in the window and are set only this way:

- **The GPU profile** (`GPUProfile`) is the graphics card Spore is told it has. The default is the GeForce
  8800 GTX. On the GOG install, all three profiles give the same High graphics level, so the choice changes only
  the card name the game reports. The launcher checks that name against your install's
  `Data/Config/VideoCards.txt`. If the card is not listed there, it uses the default.
- **Typing and controllers** go in `Environment`:
  - `SPORE_TEXT_INPUT 0` turns off dead keys and input methods.
  - `SPORE_PRESS_AND_HOLD 1` turns on the press-and-hold accent picker.
  - `SPORE_GAMEPAD 0` turns off game controllers.

```
defaults write com.spore-native.player GPUProfile gtx280                         # 8800gtx (default), 9800gtx, gtx280
defaults write com.spore-native.player DocumentsFolder "~/Somewhere/Else"        # where "My Spore Creations" goes
defaults write com.spore-native.player Environment -dict SPORE_RETINA 1 SPORE_VSYNC 0 SPORE_D3D9_FPS 20
defaults write com.spore-native.player Arguments -array -w -r:1280x800          # Spore's own options
defaults write com.spore-native.player ShowSettings -bool false                  # no window at start
defaults read com.spore-native.player                                            # show what is set
```

Writing `Environment` replaces the whole dictionary, so list every entry each time. Save and Play in the window
keep `Environment` entries that no row owns. The window writes only values that differ from the defaults. A
variable set in the environment the app is started with wins over a stored setting. Its row then
cannot be changed, and its tooltip says "Set by the Terminal command that started the app."

## From Terminal

The launcher is `Spore.app/Contents/MacOS/Spore Native`:

```
"Spore.app/Contents/MacOS/Spore Native" --help                    # the options
"Spore.app/Contents/MacOS/Spore Native" --version
"Spore.app/Contents/MacOS/Spore Native" --game-dir ~/Games/Spore  # use (and remember) this Spore folder
"Spore.app/Contents/MacOS/Spore Native" --reset-game-dir          # forget the remembered folder, then exit
"Spore.app/Contents/MacOS/Spore Native" --resolve-only            # which folder would be used, start nothing
"Spore.app/Contents/MacOS/Spore Native" --print-env               # what the game would get, start nothing
"Spore.app/Contents/MacOS/Spore Native" --settings                # show the settings window even if it is turned off
"Spore.app/Contents/MacOS/Spore Native" --no-settings             # skip the settings window this once
"Spore.app/Contents/MacOS/Spore Native" --list-settings           # every row of the window, start nothing
"Spore.app/Contents/MacOS/Spore Native" -- <game arguments>       # pass Spore's own options, e.g. -- -w -r:1280x800
```

Anything else is refused with the usage text, and nothing starts. `SPORE_LOG_FILE=-` in front of the command
keeps the log on the terminal. The log file is opened only after the settings window closes, so a Cancel
leaves no log. Game arguments, from `--` or from the settings window, reach Spore through the `SPORE_ARGS`
variable, which the launcher sets. If you set `SPORE_ARGS` yourself, it is used instead.

## Resetting

| To | Do |
|---|---|
| Choose a different Spore folder | `--reset-game-dir` as above (or delete `~/Library/Application Support/spore-native/game-dir`), then start the app |
| Undo every setting | **Reset to defaults**, then **Save** in the settings window, or `defaults delete com.spore-native.player` (which also shows the window again) |
| Clear the shader cache | delete `~/Library/Caches/com.spore-native.player` |
| Start the game from scratch (**deletes your saves**) | back up, then delete `~/Library/Application Support/spore-native/AppData` |
| Answer the privacy prompts again | `tccutil reset All com.spore-native.player` |

## If the icon looks wrong

The icon is Spore's own, read from your `SporeApp.exe` at sizes from 16 to 256 pixels. Finder scales the 256
pixel image for its largest views. If Finder or the Dock still shows a blank or old icon after a rebuild,
Finder's icon cache is out of date. Run `touch build/Spore.app`, or remove the app from the Dock and add it
again. If that does not help, run
`/System/Library/Frameworks/CoreServices.framework/Frameworks/LaunchServices.framework/Support/lsregister -f build/Spore.app`
and then `killall Dock`.
