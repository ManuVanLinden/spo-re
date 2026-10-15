# runtime/platform/macos: the macOS platform layer

The host layer under the USER32 and DSOUND shims. It holds no game code and runs as an ordinary Mac program.
The design, evidence and measurements are in
[docs/design/platform-macos.md](../../../docs/design/platform-macos.md).

| File | What it is |
|---|---|
| `include/sp_win32.h` | Win32 constants and the x86 layouts of `MSG`, `PAINTSTRUCT`, `MINMAXINFO`, `WAVEFORMATEX(TENSIBLE)`, `DSBUFFERDESC`, `DSCAPS` |
| `include/sp_platform.h`, `src/sp_queue.c` | Message queue (posted, input and cross-thread sent messages), `PeekMessageW` ordering, `WM_PAINT`/`WM_TIMER`/`WM_QUIT` synthesis, `TranslateMessage`, `DispatchMessageW`, `SendMessageW`, a `DefWindowProcW` subset, window state to `WM_*`, the two key-state tables |
| `src/sp_input.c`, `src/sp_keymap.c` | macOS key codes to VK, scan code and `lParam`; mouse to `WM_MOUSE*`; `WM_CHAR` rules |
| `include/sp_macos.h`, `src/sp_window.m` | AppKit: `NSWindow` with a `CAMetalLayer` view, the event drain behind `PeekMessageW`, window delegate to `WM_*` |
| `include/sp_dsound.h`, `src/sp_dsound.c`, `src/sp_coreaudio.c` | Fake `IDirectSound`/`IDirectSoundBuffer` over a CoreAudio output AudioUnit; `GetCurrentPosition` follows real playback |
| `tests/input-script.txt`, `tests/expected-wm.log` | The reference input script and its hand-written expected `WM_*` log (written before the code) |
| `tests/hostapp.m` | Test app: `--script` (replays the script as real NSEvents) and `--interactive` (game-style loop) |
| `tests/script_headless.c`, `tests/test_core.c` | The same script without AppKit; 75 unit checks (queue, input, key state, DirectSound without a device) |
| `tests/fake_dac.c`, `tests/audio_drift.c` | Our re-implementation of the game's Dac pacing loop, and the drift measurement |

```sh
cd runtime/platform/macos
make                 # builds into build/platform-macos/ (git-ignored)
make check           # headless: unit tests + script replay compared with tests/expected-wm.log
make check-gui       # opens a window for under a second and replays the script through AppKit
../../../build/platform-macos/audio_drift --seconds 600 --json out.json   # quiet 440 Hz tone, -50 dBFS
../../../build/platform-macos/sphost --interactive --tone                 # try it by hand
```

Requirements: Apple clang (Command Line Tools), macOS 14 or later. No Homebrew packages.
