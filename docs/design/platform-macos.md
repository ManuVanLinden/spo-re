# macOS platform layer

The window, input, display and sound the game gets on the Mac: an AppKit window with a Metal layer, macOS events
turned into Windows messages, the USER32/GDI32/IMM32/XInput imports over them, fullscreen and Retina handling,
text input, controllers, the clipboard, drag and drop, and DirectSound over CoreAudio.

Code: [`runtime/platform/macos/`](../../runtime/platform/macos/README.md) (the host layer, an ordinary Mac
program) and [`runtime/platform/guest/`](../../runtime/platform/guest/README.md) (the imports, §10).
Target exe: GOG `SporebinEP1/SporeApp.exe` (SHA-256 `dc04aee5…e59a37`). The app and tests are our own code; no
game code ran in them. Labels: **[obs]** read from the exe (address given) or measured on an Apple Silicon Mac; **[inf]** reasoned;
**[doc]** from Microsoft or Apple documentation.

## 1. What it is

The host side of the USER32 and DirectSound shims, first built and tested as a standalone Mac program with no
game code. It provides:

- **Window.** An `NSWindow` whose content view hosts a `CAMetalLayer`. Each `WM_PAINT` presents a cleared Metal
  frame.
- **Input.** `NSEvent` is translated to `WM_KEY*`/`WM_*CHAR`/`WM_MOUSE*` with Windows virtual-key codes, set-1
  scan codes and the full `lParam` bit layout. The 256-entry key-state table exists twice, as on Windows: a
  synchronous table for `GetKeyState`/`GetKeyboardState` and an asynchronous one for `GetAsyncKeyState`.
- **Pump.** A message queue with `PeekMessageW` semantics on the main thread. Any thread may post to it, and a
  cross-thread `SendMessage` blocks until the main thread pumps. Every `PeekMessageW` drains the AppKit event
  queue first.
- **Audio.** A fake `IDirectSound`/`IDirectSoundBuffer` over a CoreAudio output AudioUnit.
  `GetCurrentPosition` comes from the render callback's timestamps, so it follows the device's playback
  clock.
- **Tests.**
  - a reference input script, with its expected `WM_*` log written by hand before the code
  - a headless replay of that script
  - an AppKit replay through real `NSEvent`s
  - 75 unit checks
  - a re-implementation of the game's audio "Dac" pacing loop, used to measure drift

All of it is C11 plus one Objective-C file. It builds with the Command Line Tools and needs no Homebrew packages.

```
 guest                  HLE shims                     this layer
 WndProc 0x849310  <--  DispatchMessageW  ------->  sp_dispatch_message --> sp_wndproc_fn hook
 pump 0x8482f0     -->  PeekMessageW      ------->  sp_peek_message --(drain)--> sp_window.m: NSApp events
 Dac 0x113dc70     -->  IDirectSoundBuffer vtable -> sp_dsbuf_* <--- render callback (CoreAudio thread)
```

## 2. What the game expects (evidence)

| Fact | Evidence | Consequence here |
|---|---|---|
| Frames are driven by `WM_PAINT`, not a timer. Each pump pass calls `InvalidateRect(hwnd, NULL, FALSE)`, drains `PeekMessageW(PM_REMOVE)`, `TranslateMessage`, `DispatchMessageW`. The paint handler sends internal message 0x1ee100a, which runs the frame | [obs] pump 0x8482f0, paint 0x848420, frame 0xf477d0 | `WM_PAINT` is synthesised after posted and input messages whenever the window is invalid, and is not removed until `BeginPaint`/`ValidateRect`. The test pump is a line-for-line model of 0x8482f0 |
| Only `PeekMessageW` (×2) and `PeekMessageA` (×2) are imported; no `GetMessage`, `WaitMessage` or `MsgWaitForMultipleObjects` | [obs] `data/imports.json` | AppKit is serviced only inside `PeekMessage*`. The game's main thread is the AppKit main thread, and `[NSApp run]` is never called |
| `PeekMessageA(…, WM_KEYFIRST, WM_KEYLAST, PM_REMOVE)` drains key messages in the IME code | [obs] 0x84a0e3 | Range filters are honoured; `WM_QUIT` ignores them [doc] |
| Window class "Canvas" has style `CS_DBLCLKS` (8) | [obs] 0x849cc1 | NSEvent click counts become `WM_*BUTTONDBLCLK` too |
| `GWL_USERDATA` is set **after** `CreateWindowExW` returns, so creation-time messages go to `DefWindowProcW` | [obs] 0x849cfd, 0x849d18 | Creation messages are not synthesised. `ShowWindow` sends `WM_SHOWWINDOW` plus activation |
| WndProc cases include `WM_MOVE`, `WM_SIZE`, `WM_ACTIVATE(APP)`, `WM_NCACTIVATE`, `WM_SETCURSOR`, `WM_GETMINMAXINFO`, `WM_KEY*`, `WM_SYS*`, `WM_CHAR`, `WM_MOUSEMOVE…WM_MOUSEWHEEL`, `WM_SIZING`, `WM_ENTER/EXITSIZEMOVE`, `WM_MOUSELEAVE`, 0x500/0x501 | [obs] the window procedure 0x849310 | Every one of these is generated |
| Private messages 0x500/0x501 are posted with `PostMessageA` | [obs] 0x84800b, 0x848030 | Cross-thread posting is tested at 3 M msg/s |
| Key state is read with `GetKeyState` ×11, `GetAsyncKeyState` ×12, `GetKeyboardState` ×1 | [obs] `data/imports.json` | Both tables, with toggle bits |

DirectSound, as the game drives it [obs]:

| Step | Address | Detail |
|---|---|---|
| Device | 0x113d9a0 | `GetDeviceID` (DSOUND ordinal 9) then `DirectSoundCreate` (ordinal 1). `SetCooperativeLevel(GetForegroundWindow() or GetDesktopWindow(), DSSCL_PRIORITY)` (vtable +0x18) |
| Caps and format probe | 0x113d820 | `IDirectSound::GetCaps` (+0x10, `DSCAPS` 0x60 bytes). It tries the channel counts in table 0x14c2f88 at 44,100 Hz, then the rates in table 0x14c2f6c that fall between `dwMin/MaxSecondarySampleRate` |
| Buffers | 0x113d410 | Primary buffer: `DSBUFFERDESC{0x24, DSBCAPS_PRIMARYBUFFER}`, then `SetFormat` (+0x38): `WAVE_FORMAT_PCM` for 1–2 channels, `WAVE_FORMAT_EXTENSIBLE` (PCM subtype, channel mask 4/3/0x33/0x3f) otherwise. Secondary buffer: `dwFlags = 0x18000` (`GETCURRENTPOSITION2 | GLOBALFOCUS`), length = round_up_256(rate × 0.5 s) frames = **22,272 frames, 89,088 bytes** at 44.1 kHz 16-bit stereo. Object +0x64 = half the length |
| Start | 0x113da60 | `Lock(0, whole)`, `memset 0`, `Unlock`, `Play(0, 0, DSBPLAY_LOOPING)` |
| Pacing | 0x113dc70 | The "Dac" thread loops every 20 ms (`timeGetTime` + `Sleep`, `timeBeginPeriod(1)`), driven by the state globals 0x16e7b98/0x16e7b9c and the run flag 0x16e6307 |
| How much to write | 0x113d640 | `GetCurrentPosition` (+0x10). Target = **write cursor** + latency (object +0x54, ms) + 255, rounded down to 256 frames. Distances beyond half the buffer count as 0 |
| Writing | 0x113dd9c… | 256-frame blocks: `Lock(pos × blockAlign, 256 × blockAlign, &p1, &b1, NULL, NULL, 0)` (+0x2c), mix, `Unlock(p1, b1, 0, 0)` (+0x4c). It never asks for the wrapped second region, so blocks never straddle the end |
| Latency adaptation | 0x113d270 | Margin = Dac position at the start of the pass − write cursor (both in frames; the `div` by blockAlign is at 0x113d2b7). Under 5 ms: latency ×1.1 (counted as a near-underrun). Otherwise every 10 s, from the minimum margin: below 5 ms add the shortfall; above 10 ms subtract half the excess over 5 ms, floor 25 ms. Cap 200 ms |

So the game needs a **looping 0.5 s ring**, `Lock` without wrap, and a **write cursor** that marks the earliest
safe write point. It also needs a play cursor that advances with real playback, because the latency adaptation
uses the margin between the two.

## 3. Message queue design

`sp_peek_message` retrieves in this order [doc: PeekMessage, Remarks]:

1. cross-thread sent messages, which call the window procedure inline
2. posted messages
3. input messages
4. `WM_QUIT`, once nothing else is queued (returned regardless of the filter)
5. `WM_PAINT`
6. `WM_TIMER`

The AppKit drain runs at the start of every call. Further rules:

- **Mouse moves coalesce** when the newest queued input message is a `WM_MOUSEMOVE` with the same `wParam`, as
  Windows does.
- **Posted messages are capped at 10,000**, the Windows default; `PostMessage` fails beyond that.
- **Hit testing.** When a mouse message is removed and the mouse is not captured, `WM_NCHITTEST` and
  `WM_SETCURSOR(hwnd, MAKELPARAM(hit, msg))` are sent first, as Spy++ traces of Windows show [inf].
- **The synchronous key-state table** is updated when a key or mouse-button message is removed, so
  `GetKeyState` agrees with the message being handled. The asynchronous table is updated when the `NSEvent` is
  translated. The reference log tests both: inside each `WM_KEYDOWN`, `GetAsyncKeyState` already shows the
  later key-up.
- **`TranslateMessage`** posts the `WM_CHAR`/`WM_SYSCHAR` that the key-down carried, by the Windows rules:
  - Enter → 0x0D
  - Backspace → 0x08
  - Tab → 0x09
  - Esc → 0x1B
  - Ctrl+letter → 1–26
  - Ctrl+`[`/`\`/`]` → 0x1B–0x1D
  - AppKit's function-key code points (U+F700–F8FF) are dropped
- **Message choice under Alt.** `WM_SYSKEY*` is used when Alt is held without Ctrl, and for F10. Ctrl+Alt
  (AltGr) gives `WM_KEY*`, matching the AltGr table in the WM_SYSKEYUP documentation [doc]. Releasing Alt sends
  `WM_SYSKEYUP` if it was pressed alone, otherwise `WM_KEYUP` [inf].
- **Activation** sends `WM_ACTIVATEAPP`, `WM_NCACTIVATE`, `WM_ACTIVATE`, then `WM_SETFOCUS` from
  `DefWindowProcW`. Deactivation sends `WM_NCACTIVATE`, `WM_ACTIVATE`, `WM_ACTIVATEAPP`, then `WM_KILLFOCUS`
  [inf, the usual Spy++ order]. On deactivation, held keys are cleared from both tables without messages,
  because their key-ups go to the other app.
- **Window geometry.** A geometry change sends `WM_MOVE` before `WM_SIZE`, as Wine's `DefWindowProc` handling of
  `WM_WINDOWPOSCHANGED` does [inf], and invalidates with erase. `BeginPaint` sends `WM_ERASEBKGND` when an erase
  is pending.
- **Coordinates.** Client and screen coordinates are in points, with the screen origin at the top-left of the
  primary display. Retina scale applies only to the Metal drawable.

**AppKit integration** (`sp_window.m`):

- **The drain** is `nextEventMatchingMask:untilDate:distantPast`.
- **Key events** are translated in the drain and not forwarded. AppKit swallows key-ups while Cmd is held, and
  the game must see them; Cmd+Q becomes `WM_CLOSE`.
- **Mouse events** in the content view are translated there and also forwarded to `NSApp`, so activation
  clicks, the title bar and live resize keep working. The view's own handlers are empty.
- **Modifiers** come from `flagsChanged`, using the device-dependent bits (`NX_DEVICEL/RSHIFTKEYMASK` and so on)
  to tell left from right. Caps Lock produces a down/up pair per change.
- **Key mapping.** Cmd maps to Ctrl by default (option: the Windows key), so Mac shortcuts reach the game's Ctrl
  handlers. Option maps to Alt; `WM_SYSCHAR` carries the unmodified character.
- **Scrolling.** `scrollingDeltaY` is sent only in whole notches of 120: one line, or 40 points on a trackpad
  [inf rate].
- **Window delegate:**
  - `windowShouldClose:` → `WM_CLOSE`, and returns NO (the game decides)
  - resize and move → `WM_MOVE`/`WM_SIZE`
  - live resize → `WM_ENTER/EXITSIZEMOVE`, and `WM_GETMINMAXINFO` through a guest-memory scratch area
  - miniaturise → `WM_SIZE(SIZE_MINIMIZED)`
  - screen change → `WM_DISPLAYCHANGE`
- **Cross-thread posting** wakes the main thread only if it is blocked in `sp_mac_wait_message`. The game only
  polls, so it never is.

**Guest boundary.** The window procedure is a host callback. The runtime installs one that calls 0x849310 with
`sg_call_guest`. Every such call starts inside a shim (`PeekMessageW`, `DispatchMessageW`, `SendMessage*`,
`DefWindowProcW`, `UpdateWindow`, `ShowWindow`), as the re-entrancy rule in runtime-abi.md §7.3 requires.
`MSG`, `PAINTSTRUCT` and `MINMAXINFO` are declared with their x86 layouts, so the adapter copies them as they
are. Pointer-carrying messages use `sp_set_scratch`, which takes the host view of a guest buffer plus its guest
address.

## 4. Key map

[`sp_keymap.c`](../../runtime/platform/macos/src/sp_keymap.c) maps 119 macOS key codes. The source tables are
`kVK_*` from HIToolbox/Events.h and the VK and scan codes from Microsoft's documentation.

- **Keyboard layout.** Scan codes remain positional. With `SPORE_KEYBOARD_LAYOUT=azerty`, letter VKs follow French AZERTY (including A/Q, Z/W, and M), while AppKit supplies text. Other settings retain the US VK table. The local Steam launcher defaults to AZERTY and uses the existing ZQSD trigger resource.
- **Extended keys** carry lParam bit 24: arrows, Home/End/PgUp/PgDn, Help→Insert, forward-delete→Delete,
  keypad Enter and `/`, right Ctrl/Alt, Win.
- **Num Lock** has no Mac key. It is reported toggled on, so the keypad gives `VK_NUMPAD0–9` and digits.
- **Scan-code uniqueness.** Every (scan, extended) pair is unique except two that Windows shares as well:
  Keypad Clear = 0x4C (Numpad 5 without NumLock), and JIS Eisu = 0x3A (on the Caps Lock key).

## 5. Fake DirectSound

[`sp_dsound.c`](../../runtime/platform/macos/src/sp_dsound.c) and
[`sp_coreaudio.c`](../../runtime/platform/macos/src/sp_coreaudio.c).

- **Methods.** Every method the game calls, at the vtable slots it uses (§2), plus `GetStatus`,
  `SetCurrentPosition`, `GetFormat`, `GetCaps`, `Get/SetVolume`, `Stop` and non-looping play.
- **Formats.** PCM, 1–2 channels, 8/16-bit (16/24/32 through `EXTENSIBLE`), 100–200,000 Hz. More than 2 channels
  fails with `DSERR_BADFORMAT`, so the game's probe settles on stereo. That is EA's own Mac choice:
  `ConfigManager.txt` sets `Support51Audio false` under `$cider`.
- **Buffer memory** comes from an allocator hook, so the runtime can place it in the guest window; `Lock` must
  return guest pointers (runtime-abi.md §8.1).
- **Backend.** Each playing secondary buffer gets one `kAudioUnitSubType_DefaultOutput` unit. Its input format is
  the buffer's own PCM format, so the render callback copies straight from the ring and the unit resamples
  44.1 → 48 kHz. The unit follows the system default device, which covers what the game's 5 s device
  watchdog (0x1154870) checks.
- **Play cursor.** Each render callback publishes (output host time, ring position, frame count) under a
  sequence lock. The output host time is `AudioTimeStamp.mHostTime` plus the device's presentation latency:
  `kAudioDevicePropertyLatency` + `SafetyOffset` + stream latency, 798 frames = 16.6 ms on the built-in
  speakers [obs]. `GetCurrentPosition` interpolates between the two published blocks around "now", so the
  cursor moves at the device's rate without jumps and never goes backwards.
- **Write cursor** = the next frame the callback will pull, plus one callback period. The game must not write
  before it.
- **Memory ordering.** `Unlock` issues a release fence and the callback starts with an acquire fence. This
  hand-off needs no fence on x86 but does on arm64 ([memory-ordering.md](memory-ordering.md)).
- **Underrun accounting.** Tests can tell the buffer how far the producer has written, to count underruns.

The test producer, [`fake_dac.c`](../../runtime/platform/macos/tests/fake_dac.c), re-implements 0x113dc70,
0x113d640 and 0x113d270 as described in §2, writing a 440 Hz sine instead of the mixer output. Its initial
latency is 50 ms [inf: the constructor that sets +0x54 was not read].

## 6. Measured behaviour

| Measurement | Value | How |
|---|---|---|
| Cross-thread posting | 200,000 messages from 4 threads in 0.067 s (about 3 M msg/s), 0 per-thread order violations | `test_core`, ring-buffer queue |
| Pump cost, interactive window, 800×600, 16 ms frame target | 57 passes/s; drain + Metal clear + present averages 0.80 ms, worst 173 ms (first frame) | `sphost --interactive --seconds 6` |
| Scripted replay (19 passes) | 0.34 s, 19 Metal frames | `sphost --script` |
| `GetCurrentPosition` cost | about 0.7 µs per call | `audio_drift` |
| CoreAudio callback size | 470–471 frames at 44.1 kHz (one 512-frame I/O cycle at 48 kHz), about 10.7 ms | `audio_drift` |
| Write − play cursor gap | up to 50.4 ms (0 only before the first callback): one 10.7 ms callback period + 16.6 ms device latency + the converter and I/O buffering | `audio_drift`; Windows' DirectSound reports gaps of the same order for emulated buffers [inf] |

## 7. Tests

**Input: the `WM_*` log of a reference input script.**

[`tests/input-script.txt`](../../runtime/platform/macos/tests/input-script.txt) has 19 steps. They cover:

- show and paint
- plain, shifted, repeated, Ctrl (from Cmd), Alt and Alt-alone keys; Caps Lock on and off
- Esc, Backspace, keypad Enter, Numpad 5, F5 and Left
- coalesced mouse moves, click, double-click, right-drag with `WM_CONTEXTMENU`, wheel
- resize
- posted-versus-input ordering, with posts from another thread
- deactivate, activate, close → `WM_QUIT`

[`tests/expected-wm.log`](../../runtime/platform/macos/tests/expected-wm.log) was written by hand from the
Win32 rules above, before any implementation (SHA-256 then `8caf4a45…6da1c4`; unchanged since).

- **Headless** (`script_headless`: script → `sp_input_*` directly): matches all 204 lines, also under ASan + UBSan.
- **AppKit** (`sphost --script`: script → real `NSEvent`s via `[NSApp postEvent:]` → drain → translation):
  matches all 204 lines.
- **`test_core`** (queue order, filters, `PM_NOREMOVE`, paint/timer/quit, 10,000 limit, thread posting,
  cross-thread send, coalescing, key map, L/R modifiers, DirectSound without a device): 75 checks, clean under
  ASan + UBSan and under TSan.

**Audio: the play cursor drifts less than 1 ms per minute.**

`audio_drift --seconds 600` played a 440 Hz tone at -50 dBFS on the built-in speakers (48 kHz). It sampled
the play cursor every 100 ms and compared it with two clocks:

- the output device's own sample clock (`AudioDeviceGetCurrentTime`, scaled to 44.1 kHz), the independent
  measure of real playback
- the host clock

Results (a 600 s run):

| Quantity (10.0 min, 5,760 samples after 3 s warm-up) | Result |
|---|---|
| **Drift of the play cursor against the device's sample clock** | **-0.0000 ms/min**. The offset stayed in -41.777 … -41.719 ms (constant: the two clocks have different zero points); largest residual from the fit 0.029 ms |
| Play cursor against the host clock, nominal 44.1 kHz | +0.326 ms/min (5.4 ppm) |
| Device clock against the host clock | +0.326 ms/min (5.4 ppm): the speakers' crystal, and all of the previous line |
| Play cursor going backwards | 0 times |
| Render callbacks / frames / underruns | 56,537 / 26,595,005 / **0** |
| Fake Dac | 30,153 wakeups, 103,892 blocks; latency adapted 50 → 37.7 ms; margin min 5.74 ms, average 21.7 ms; 2 near-underruns (both at start-up) |
| Cost | `GetCurrentPosition` 446 ns average; whole process 1.9 s CPU over 603 s, 17 MB max RSS |

The play cursor tracks real playback, with one caveat [inf]: the cursor's timestamps and
`AudioDeviceGetCurrentTime` both come from the CoreAudio HAL's model of the device clock. The run shows that the
cursor follows the HAL with no accumulating error; whether the HAL matches the analogue output would take an
acoustic loopback to check. Against wall time the cursor differs only by the speaker clock's crystal error,
which a real DirectSound play cursor shows too. The Dac paces itself on the measured margin, not on wall time,
so that error does no harm.

A shorter run under ThreadSanitizer (render thread, Dac thread, sampling thread) found one race, on our test
harness's plain stop flag, the same pattern as the game's Dac run flag 0x16e6307. The harness flag is
atomic; the guest's flag gets release/acquire ordering from the translator's default memory-ordering policy
([memory-ordering.md](memory-ordering.md)).

## 8. Deviations from Windows and open points

- **Key messages are not merged** into repeat counts above 1. Windows rarely merges them either.
- **Dead keys and IME** go through `NSTextInputClient` (section 12).
- **`GetAsyncKeyState`** reflects events as they are drained, not the live hardware state. Reading the hardware
  through `CGEventSourceKeyState` may need the Input Monitoring permission [inf].
- **Mouse outside the window.** Without `SetCapture`, a drag that leaves the window keeps sending
  `WM_MOUSEMOVE`, because AppKit delivers it. Windows would send it to the other window.
- **Live resize.** No `WM_SIZING` is sent yet (it needs a `RECT` in scratch, as `WM_GETMINMAXINFO` has).
  Frames stop during the live-resize loop, as they do in Windows' modal size loop.
- **To confirm on a Windows PC** with a small message-logging program of our own (not game code):
  - the Alt-release rule
  - the activation and deactivation order
  - the `WM_SETCURSOR` high word for double-clicks
  - `WM_MOVE` before `WM_SIZE`
- **Unknown thread.** It is not known which thread calls `SendMessageA` (×8) and `PostMessageA` (×3). Both work
  from any thread here.
- **The game's mixer** writes into the ring directly through `Lock`, so the buffer must live inside the guest
  window. The allocator hook allows that.

## 9. The shim work this layer was built for

When the standalone layer was first written, the 94 USER32/DSOUND/WINMM imports stood at 24 done, 11 partial,
27 that need only a fixed answer and 32 to do. The plan for the rest (section 10 records what was built):

1. **Window management.** Mostly thin calls on `NSWindow`/`NSScreen`:
   - `SetWindowPos`, `MoveWindow`, `AdjustWindowRectEx`, `Get/SetWindowLong` (`GWL_STYLE` for fullscreen)
   - `GetSystemMetrics`, `MonitorFromWindow`, `GetMonitorInfoA`, `EnumDisplayMonitors`
2. **Clipboard** (EAClipboard) over `NSPasteboard`.
3. **`MessageBoxA` → `NSAlert`.** The assert thread 0x925290 calls it off the main thread, so it has to be
   marshalled to the main thread.
4. **Text input.** IME through `NSTextInputClient`, and a layout-aware VK map.
5. **The guest adapters**: the thunks that unpack guest arguments and call these functions, the
   DirectSound COM vtables built with `sg_com_vtable`, and `GetDeviceID`.
6. **Cursors.** Map `SetCursor` to `NSCursor`; test cursor warping and hiding with a real game window, since
   doing that in an automated test would move the user's own cursor.
7. **Gamepads.** XInput over GameController, later.

## 10. The guest imports over this layer

Code: [`runtime/platform/guest/`](../../runtime/platform/guest/README.md): the window, input and message-loop imports.
Labels as above. All runs below are of our own code, except 10.7 and 10.11, which ran the translated game inside our
binary.

### 10.1 What it is

`runtime/platform/guest` defines **110 strong `SG_IMPORT` functions**: every USER32 (87 of 89; `wsprintfA/W` stay
in runtime/hle's printf family), GDI32 (6), IMM32 (12) and DINPUT8 (1) import of the HLE import table, the three XInput
dynamic imports, and KERNEL32 `SetThreadExecutionState`. That last one is a display-sleep request, called at
0x849d7f right after the window's `SetTimer`, so it lives with the window layer; if runtime/win32 ever defines
it, delete it here. The list is generated (`gen_list.py` -> `src/sgp_list.h`, with each clean-up byte count taken from
`runtime/hle/gen/hle_imports.c`).

| File | Contents |
|---|---|
| `src/sgp_core.c` | back-end choice, lazy set-up on the first window import, the guest window-procedure bridge, window and class tables, the headless back end, the registration check |
| `src/sgp_window.c` | classes (`RegisterClassExW/A` by name or atom), `CreateWindowExW/A`, `DestroyWindow`, `ShowWindow`, `UpdateWindow`, `SetWindowPos`, `MoveWindow`, `GetClientRect`, `GetWindowRect`, `AdjustWindowRectEx`, `Client/ScreenToClient`, window longs, `SetWindowTextW`, focus and activation, `GetSystemMetrics`, `SystemParametersInfoA`, monitors |
| `src/sgp_msg.c` | `PeekMessageW/A`, `TranslateMessage`, `DispatchMessageW` (with `TIMERPROC`), `PostMessageA`, `SendMessageA`, `PostQuitMessage`, `DefWindowProcW/A`, painting, timers, key state, capture, cursor, hooks, clipboard, message boxes, window station |
| `src/sgp_misc.c` | GDI32, IMM32 (no IME), XInput (no pad), `DirectInput8Create` (no controllers), lookups for the D3D9 package (`sgp_main_window`, `sgp_metal_layer`) |
| `src/sgp_backend_mac.c` | the AppKit back end: thin calls into `sp_window.m` |

What the guest layer needs from `runtime/platform/macos`: `sp_mac_attach_window` (an NSWindow for an hwnd that already exists, created on
the window's first `ShowWindow`, so EAClipboard's never-shown window gets none), `sp_mac_set_client_rect` (clamped
into the visible frame), title, order out/key, screen info, cursor position and shape, clipboard over
`NSPasteboard`, `NSAlert` message boxes, double-click time, caption height; in `sp_queue.c`: `sp_set_focus`,
`sp_get_active`, visibility queries, and `sp_set_paint_needs_visible` (hidden windows get no `WM_PAINT`, as on
Windows; off by default so the reference log of section 7 holds). `SetTimer` returns 1 for timer id 0 (the game's
call at 0x849d6e) [obs: returning 0 stops the boot].

### 10.2 Threads

**[obs]** `sg_boot` runs the PE entry on the thread that called `main()`: `runtime/image/src/main.c` calls
`sg_boot` directly, and `sg_core_attach_main` makes the *calling* host thread the guest main thread
(`core_boot.c`, `core_thread.c`). So the guest main thread **is** the process main thread, and the design of
sections 1-3 holds unchanged: no `[NSApp run]`, AppKit is initialised by the first window import on that thread, and every
`PeekMessageW/A` drains NSEvents. No marshalling thread is needed. The first window import logs a warning if it
ever runs off the main thread (`pthread_main_np`), and `PeekMessage*` on a thread that does not own the windows
returns no messages (logged once). The main thread's host stack is 64 MiB (`-Wl,-stack_size,0x4000000` in
`tools/link/build.py`), which the nested guest window-procedure calls share.

### 10.3 Calling the guest window procedure

Each window's `sp_wndproc_fn` is a bridge that calls the guest procedure (0x849310 for "Canvas") with
`sg_call_guest(ctx, proc, SG_CC_STDCALL, 4, args)` inside an `sg_cleanup` record with `barrier = 1`, so a guest
exception that tried to unwind through `DispatchMessage`/`SendMessage` aborts with a diagnostic. The `ctx` is the one the enclosing shim was entered with (a thread-local set by every shim that can call
back, since `ctx` is `restrict` in the shim), else `sg_self->ctx`. Every call into a window procedure starts inside
a shim (runtime-abi 7.3). `EnumDisplayMonitors`, `TIMERPROC` and `IDirectInput8::AddRef` use the same barrier
helper.

Creation messages (`WM_NCCREATE`, `WM_CREATE`, ...) are not synthesised (section 2: `GWL_USERDATA` is set after
`CreateWindowExW` returns, so the game forwards them to `DefWindowProcW`).

### 10.4 Geometry and the "no screen takeover" rule

`AdjustWindowRectEx`, `GetWindowRect`, `SetWindowPos` and `MoveWindow` share one frame model: a caption of the
real title-bar height on top (32 pt measured on the Mac [obs]) when the style has `WS_CAPTION`, no side
borders, `GetSystemMetrics(SM_CXFRAME…)` = 0. The game's "fullscreen" path (0x8490c0: `WS_POPUP`,
`SetWindowPos(HWND_TOPMOST, 0, 0, w, h)`, `ShowWindow(SW_MAXIMIZE)`) gets a normal titled window clamped into
the primary screen's visible frame; `IsZoomed` is 0 and minimise requests are ignored. `GetSystemMetrics` and
`GetMonitorInfoA`/`GetDeviceCaps` report the real screen in points and never change it. The exe does **not
import** `EnumDisplaySettings`, `ChangeDisplaySettings` or `ClipCursor` [obs, data/imports.json], so nothing
here can change the display mode or confine the cursor. `ShowCursor(FALSE)` hides the cursor only while our
window is key; resigning key or pressing Esc shows it again (`sp_mac_cursor_release`), and `SetCursorPos` warps
only while our window is active.

### 10.5 Frames

`WM_PAINT` is delivered when the window is visible and dirty, after posted and input messages, which with the
game's pump (an `InvalidateRect` per pass) gives one frame per pass [obs: 54 of 54 passes in the AppKit smoke
test, 19 of 19 in the replay]. `SPORE_PAINT_HZ=<n>` adds an optional cadence (a visible window not painted for
1000/n ms is invalidated) for the case where the game stops invalidating; it is off by default, as on Windows.

### 10.6 Tests

- **`make -C runtime/platform/macos check` and `check-gui`:** the 75 unit checks and both replays of section 7.
- **`make -C runtime/platform/guest test`:** a hand-written guest window procedure and pump in translated-code
  style (`SG_ARG`, `SG_PUSH32`, `SG_CALL(sg_imp_user32_…)`, `SG_RET(16)`), registered in a run-time dispatch table
  and reached through `sg_call_guest`, replay the reference script of section 7. The window is created with
  `RegisterClassExW` + `CreateWindowExW`, shown with `ShowWindow`/`UpdateWindow`, resized with `SetWindowPos`; the
  procedure uses `BeginPaint`/`EndPaint`, `DefWindowProcW`, `GetKeyState`, `GetAsyncKeyState`, `ScreenToClient`,
  `PostQuitMessage`. Its log is identical to the hand-written `expected-wm.log` (204 lines), with 19 `WM_PAINT`
  frames in 19 passes. Further checks: geometry and the frame model, hidden windows get no `WM_PAINT`, window
  longs, `RegisterClassA`/`CreateWindowExA`, `SetTimer` → `WM_TIMER` and `TIMERPROC`, `PostMessageA`/`SendMessageA`,
  clipboard, cursor counter, IMM32, XInput, `DirectInput8Create` + `EnumDevices` through the guest vtable,
  `GetUserObjectInformationW`, the window stop (`SPORE_STOP_AT=window` stops in `CreateWindowExW`, exit 0), and the
  core event API (10.8).
- **`make -C runtime/platform/guest check-gui`:** the game's create sequence (hidden `CW_USEDEFAULT`,
  `MoveWindow`, `ShowWindow`) shows a real window; 54 pump passes in 1 s give 54 `WM_PAINT`; client 640×480; a
  `CAMetalLayer` for the D3D9 package.
- **Reachable from the import table:** `sgp_register_shims()` (run by the fixture and on the first window import in
  the game) checks each of the 110 functions against `hle_imports[]`/`hle_dynamics[]`; all 106
  USER32/GDI32/IMM32/DINPUT8 entries except `wsprintfA/W` point at this package. libhle's
  `sg_imp_user32_PeekMessageW`/`CreateWindowExW` are `weak external`, ours are `external`; the game's boot log says
  `platform: 110 imports checked against the table, 0 problems`.

### 10.7 Boot past the window (translated game code, our binary) [obs]

With this package linked into the game binary, `spore --run` with `SPORE_STOP_AT=Direct3DCreate9`:

```
RegisterClassExW("canvas", style 0x8, wndproc 0x00849310, extra 0) = 0xc000
platform: AppKit back end (caption 32 pt, paint cadence 0 Hz)
CreateWindowExW(class 0xc000 "canvas", "", style 0x00c80000, ex 0x0, 628,368 800x632) = 0x00010001
SetWindowLong(0x10001, -21, 0x0241bed8) = 0x00000000        (GWL_USERDATA, 0x849d18)
ShowWindow(0x10001, 0) = 0
SetTimer(0x10001, 0, 2000 ms, proc 0x0) = 1
SetThreadExecutionState(0x80000000) = 0x80000000
MoveWindow(0x10001, 0,-32 800x632, repaint 1)             (client 800x600 at the screen's top-left)
SetWindowLong(0x10001, -16, 0x00cb0000) = 0x00c80000      (windowed style)
SetWindowPos(0x10001, after 0xfffffffe, 0,0 0x0, flags 0x23)
ShowWindow(0x10001, 0) = 0
unimplemented: ole32.dll!OleInitialize (called from guest 0x006bbb3a)     <- strict mode stops here
```

With `SPORE_HLE_UNIMPL=continue` the boot passes `OleInitialize`, `CoLockObjectExternal` and `RegisterDragDrop`
(ole32, drag and drop onto the window) and reaches `Direct3DCreate9` at 702 ms. No USER32/GDI32/IMM32/WINMM/
XInput/DINPUT8 import was unimplemented on the way. The window stays hidden until after the device is created
(10.11).

### 10.8 The core event API

`sg_core.h` section 5: `sg_core_event_create(manual, initial)`, `sg_core_event_set(h)`,
`sg_core_event_reset(h)`, `sg_core_event_wait(h, ms)`. The handles are ordinary `SG_H_EVENT` handles (guest
`WaitForSingleObject`/`CloseHandle` work on them); every call is a full barrier and callable from any host
thread. A wait on a guest thread is `sg_core_wait`; on any other thread it blocks on a private condition
variable that every wake broadcasts to (`ko_wake_locked`). Tested in the fixture: auto and manual reset, timeouts,
a guest-thread wait woken by a host thread, host-thread waits woken by the guest `SetEvent` shim, two host waiters
on a manual event, closed handles. `make -C runtime/core test`: 4,704 passed, 0 failed (2026-10-15).

Users:

- **Directory watcher** (`runtime/win32/src/w32_dirwatch.c`, [win32-layer.md](win32-layer.md) §21): its default
  back end is this API (`sg_core_event_create`/`set`/`reset` and `sg_handle_close`), with weak stand-ins for the
  w32 unit tests; the FSEvents queue signals `OVERLAPPED.hEvent` with `sg_core_event_set(h)` directly.
- **Audio** (the DirectSound package): a buffer's `SetNotificationPositions` events, if the game used them, would be
  set from the CoreAudio render callback with `sg_core_event_set` (no allocation, one mutex). The game's Dac
  thread polls `GetCurrentPosition` every 20 ms instead (section 2), so this path exists only for completeness.

### 10.9 Linking and open items

- `tools/link/build.py` links `sp_queue.c`, `sp_input.c`, `sp_keymap.c`, `sp_window.m`, `sp_dsound.c`,
  `sp_coreaudio.c`, `sp_clipboard.m` and `sp_open.m` from runtime/platform/macos, with the
  AppKit/QuartzCore/Metal/CoreAudio/AudioToolbox frameworks.
- Open: layout-aware VK codes, `WM_SIZING`, game cursor art (`LoadImageW` cursors show as the arrow),
  `SetThreadExecutionState` as an IOKit power assertion, `MessageBox` off the main thread (returns
  the default button with a log line instead of marshalling).

### 10.10 Scripted input and cursor warps

- **Scripted input.** `SPORE_INPUT_SCRIPT` (`sgp_run.c`) posts real mouse `NSEvent`s through
  `sp_mac_post_mouse`, so a scripted click takes the same window-to-client mapping as the user's mouse; steps are
  logged as `script: input t=… down at client (x,y)`. Synthetic events carry timestamps from 1e9 s up.
  `NSEvent` timestamps count from boot, so a lower marker (900,000 s) made every real event look synthetic on a
  Mac up for more than 10.4 days, and real clicks never reached `[NSApp sendEvent:]` [obs: on a Mac up for
  62 days].
- **The scripted cursor ends with the script.** Any real mouse event ends the script's `GetCursorPos` override, and
  `SetCursorPos` moves only the scripted point while a script drives the mouse. An override that outlived the
  script would hand a window played by hand stale positions, and Spore's camera drag (`SetCursorPos` then
  `GetCursorPos` each frame) huge deltas: misplaced world clicks and a camera that turns in jumps [inf].
- **Cursor warps as Windows does them [obs, `SPORE_INPUT_TRACE=1`].** The Creature-stage camera calls
  `GetCursorPos` (0x8d32cd) every frame and warps back to the press point with `SetCursorPos` (0x8d3333).
  `SetCursorPos` posts the `WM_MOUSEMOVE` Windows makes (to the capture window anywhere, or to the active
  window when the point is in its client area). Real mouse moves stamped before the last warp are dropped,
  because their pre-warp location would undo the warp in the game's delta. Scripted warps never move the user's
  cursor and also work when the window is not key. Scripted right-drags then turn the camera gradually (+4 points per
  frame in, +4 out) and a click on a fruit plant sends the creature to it ([DEVIATIONS.md](../DEVIATIONS.md) 4.2). Without the INDEX16 lock slack
  (DEVIATIONS.md 3.6) the Creature-stage terrain is broken and picks hit the wrong surface.
- **The `check-gui` one-point flake [obs].** AppKit does not return a posted event's location exactly:
  window point (120.5, 419.5) came back as (119.87, 420.14) in one run and (119.56, 419.82) in another, so `floor`
  gave 119 about half the time, and `eventNumber` keeps only 16 bits. The poster therefore records the intended point
  under the event's unique synthetic timestamp (`sp_mac_note_synthetic_point`), which also carries a scripted
  middle button. `check-gui`: 13 of 13 runs pass (MATCH 204); without the recorded point 4 of 4 failed.

### 10.11 The window on screen, activation, the cursor

What the game did with the window layer, COM, the null device and DirectSound linked together, up to a clean
10-minute idle (600 s of pumping,
35,145 `WM_PAINT` frames at 58.6 per second, 3.4 M draws, 10 guest threads, 2,967 live D3D resources flat after
about 4 minutes, peak RSS 456 MB; no miss, mismatch, fault or watchdog event).

- **On screen [obs].** The window is created hidden (`CreateWindowExW` from 0x849d03, style 0x00c80000). After the
  device and the config the game calls `ShowWindow` 9, 3 and 1; the NSWindow is created on the first visible show.
  The `SW_MAXIMIZE` request becomes a normal titled window (fullscreen came later, §11).
- **Frames [obs].** The main loop 0xf47420 pumps `PeekMessageW`; every pass that removes a `WM_PAINT` renders one
  frame through the window procedure (0x849310 → 0x848420 → 0x847f40 → 0x8854a0 → 0xf479b0 → renderer →
  `Present`), inside `DispatchMessageW → sp_dispatch_message → sg_call_guest(0x849310)` and the cleanup
  barrier of 10.3, so a fatal inside a frame is reported with registers and frames.
- **The game does not render while inactive [obs].** macOS did not make a background-launched process key, so no
  `WM_ACTIVATEAPP`/`WM_ACTIVATE` reached the game and it pumped `WM_PAINT` at 53/s with `Present` 1 and 0 draws;
  runs where the window happened to become key rendered every frame. `SPORE_FORCE_ACTIVE=1` activates the guest
  window on its first visible `ShowWindow` even if the NSWindow is not key, and a later loss of key status no
  longer deactivates the guest (`sp_queue.c`). Cursor release and key-state reset still follow the real key window,
  so the user's cursor is never trapped. It is a test aid; the app bundle makes the window key at launch.
- **Cursor and focus [obs].** No `ClipCursor` or display-mode change is imported. The cursor is hidden only while
  the NSWindow is key and is released on focus loss or Esc.
- **Imports first needed past the window.** ole32 `OleInitialize` (from 0x6bbb3a), `CoLockObjectExternal`,
  `RegisterDragDrop` and friends succeed and keep nothing (`runtime/hle/src/ole_stub.c`; real drag and drop
  replaced them later). `TrackMouseEvent` (0x847cb1, on `WM_MOUSEMOVE`, reached only when the mouse crosses the window):
  `TME_LEAVE` arms one `WM_MOUSELEAVE` through `sp_track_mouse_leave`, `TME_CANCEL` disarms it, `TME_QUERY`
  answers that nothing is tracked. A modal `MessageBoxW` would block an unattended run, hence `SPORE_MSGBOX=auto`.
- **Shutdown [obs].** When an early run was used by hand, the game took its own clean `exit(0)` through
  about 2,000 atexit handlers (117 live COM objects left).

### 10.12 Test-run switches

The hooks the test runs use ([DEVIATIONS.md](../DEVIATIONS.md) 6.1). They are environment variables of the bare
binary, `build/sporenative/spore --run` (with `SPORE_GAME_DIR` naming the Spore folder); the app's launcher sets
none of them.

| Variable | Effect |
|---|---|
| `SPORE_STOP_AT_WINMAIN=1` | `tools/harness/prep_src.py` redirects the one call to WinMain (0x11e105c) to `sg_boot_winmain` (`runtime/core/src/core_boot_diag.c`), which reports the initializer counts and exits 0 when this is set and otherwise forwards to WinMain |
| `SPORE_STOP_AT=window` or an import name | Stop at the first `CreateWindowExW/A` or `Direct3DCreate9`, or at the named import, through `hle_stop_check()` (win32-layer.md §22.4); `core_boot_diag.c` prints a `boot: REACHED stop point` line, the guest and host frames and the I/O table, then exits 0 |
| `SPORE_WATCHDOG=S` | Armed by `core_boot_diag.c`: ends a run not finished after S s with a report. Interactive runs have none |
| `SPORE_RUN_SECONDS=N` | After N s, counted from the first `PeekMessage`, the pump prints the report and `_exit(0)`s (`sgp_run.c`): `WM_PAINT` delivered, `Present`, draws, messages, COM calls, live D3D resources, peak RSS, live guest threads by name, per-method COM counts. The game's shutdown is not run. The check sits in the pump, so a hung main thread never exits 0 and the watchdog reports it |
| `SPORE_RUN_HEARTBEAT=S` | The same one-line summary every S s (default 60) |
| `SPORE_FORCE_ACTIVE=1` | Keeps the guest window active when macOS does not make it key (§10.11) |
| `SPORE_MSGBOX=auto` | Message boxes answer with the default button without UI (DEVIATIONS 2.11) |
| `SPORE_SNAPSHOT=p1.png@S1,p2.png@S2` | PNGs of the window at the given seconds (window-server image, AppKit drawing as a fallback; `sp_window.m`) |
| `SPORE_INPUT_SCRIPT="T cmd …; …"` | Scripted input (§10.10): `move X Y`, `click X Y`, `rclick X Y`, `snap PATH`, `down`/`up`, `rdown`/`rup`, `mdown`/`mup`, `mclick`, `rel DX DY [N [DT]]`, `comtrace N`, `key`, `type`, `dclick`, `wheel`; T in seconds since the first `PeekMessage`; needs `SPORE_RUN_SECONDS` |
| `SPORE_COM_STATS=1` | Per-method COM counts at exit |

```
SPORE_GAME_DIR="$G" SPORE_STOP_AT=window SPORE_HLE_LOG=2 build/sporenative/spore --run    # run to the first window
SPORE_GAME_DIR="$G" SPORE_RUN_SECONDS=600 SPORE_FORCE_ACTIVE=1 SPORE_COM_STATS=1 SPORE_WATCHDOG=640 \
    build/sporenative/spore --run                                                         # a 10-minute idle run
```

## 11. Fullscreen, Retina-native rendering and the display

### 11.1 What the game does [obs, a run log; 0x8490c0]

With fullscreen on in its settings (a Preferences.prop with 1920×1080: `Reset(1920x1080, fullscreen
requested)` in every Metal run log), after the device exists the game calls `SetWindowLong(GWL_STYLE,
0x80080000)` (`WS_POPUP|WS_SYSMENU`), `ShowWindow(SW_RESTORE)` (the window's first show, so the NSWindow is
created here), `SetWindowPos(HWND_TOPMOST, 0, 0, w, h, SWP_FRAMECHANGED)` with its resolution,
`ShowWindow(SW_MAXIMIZE)` and `ShowWindow(SW_SHOWNORMAL)`. The windowed path uses style `0x00cb0000` (caption,
system menu, minimise and maximise boxes, no `WS_THICKFRAME`) and `SetWindowPos(…, SWP_FRAMECHANGED)`. Its
back buffer is always the resolution setting, not the client rect. The window procedure
(0x849310) handles `WM_SIZE` (0x848d00) and `WM_GETMINMAXINFO`; whether it resets the device to a new client
size is not established.

### 11.2 Borderless window, no mode switch

Fullscreen is a **borderless `NSWindow` covering the window's current display** (`NSScreen.frame`), with
`NSApplicationPresentationHideDock | HideMenuBar` while it is key (the options apply only while the app is
active, so Cmd-Tab gives the menu bar back), at the normal window level, `NSWindowCollectionBehaviorFullScreenNone`.
It never switches the display mode (`CGDisplaySetDisplayMode` is not called anywhere) and never uses a macOS
fullscreen Space: the toggle is instant, with no animation and no separate Space, and other windows can still
come in front. The game's resolution
stays its own. `SPORE_FULLSCREEN=0` keeps a titled window instead.

### 11.3 How it works

- **When.** A top-level window whose style is `WS_POPUP` without `WS_CAPTION` is fullscreen. The guest layer
  (`sgp_window.c` `sync_fullscreen`) applies the style when the window is first shown, at `SW_MAXIMIZE`, and at
  every `SetWindowPos(SWP_FRAMECHANGED)` (before the move, so the size the game passes lands in the right mode),
  through the back-end hook `sgp_backend.set_fullscreen` → `sp_mac_set_fullscreen`. The headless back end
  has none.
- **Client size.** In fullscreen the client the game sees is the size it asked for (`SetWindowPos`), at the
  display's origin (`GetWindowRect` (0,0,w,h)), as on Windows after its mode switch. Entering fullscreen keeps the
  current client, so Cmd-Ctrl-F needs no `WM_SIZE`; the game's own path then sets its resolution (`WM_SIZE` w×h).
- **Letterbox.** The client is shown in the largest rectangle of its aspect inside the screen, centred and
  snapped to whole drawable pixels (`layout()`). The rectangle goes to the presenter on the `CAMetalLayer` under
  the key `sporeContentRect` (normalised, top-left origin; absent = the whole drawable), and `mt_present.mm`
  clears the drawable black and draws the back buffer into it.
- **Mouse.** Every mouse event goes through `view_to_client`: (view point − rectangle origin) × client/rectangle;
  points on the bars are outside the client (moves are not delivered unless a button is held or the window has
  the capture). Synthetic points (`sp_mac_post_mouse`, the scripts) use the inverse. `GetCursorPos` and
  `SetCursorPos` use the same mapping relative to the display's origin, and a `GetCursorPos` while the pointer
  is still where the last warp put it returns the warp's guest point exactly, so Spore's warp-and-read-back
  camera drag cannot drift by rounding when a guest unit is not a point.
- **Keys.** Cmd-Ctrl-F (key code 0x03 with Command and Control) toggles fullscreen and is not delivered to the
  game. Esc keeps its Windows meaning (shows the cursor, goes to the game, which uses it for its menus) and leaves
  fullscreen only with `SPORE_FULLSCREEN_ESC=1` (leaving on Esc would throw the player out of fullscreen each
  time they open the pause menu, so it is opt-in).
- **Leaving.** The titled window comes back at its saved frame with the client the game had (clamped to the
  visible frame), and the presentation options are reset.
- **Display changes.** `windowDidChangeScreen` and the screen-parameters notification refit a fullscreen window
  to its display; `windowDidChangeBackingProperties` re-snaps the rectangle.
- **Windowed.** Resizable; `windowDidResize` sends `WM_SIZE` with the new client in guest units at each
  live-resize step (`WM_ENTERSIZEMOVE`/`WM_EXITSIZEMOVE` around it). The back buffer stays the game's resolution
  and is stretched to the client, as Windows' windowed `Present` does.

### 11.4 Retina-native (`SPORE_RETINA=1`, off by default)

Guest units become the primary display's pixels: `k` = its backing scale (`sp_mac_guest_scale`). Window
geometry from the game is divided by `k` on the way in and every size, point and metric multiplied by `k` on
the way out (`GetSystemMetrics`, work area, caption height, `WM_DISPLAYCHANGE`, min/max track sizes, mouse). A
window the game makes 1920×1080 is 960×540 points and its 1920×1080 back buffer meets a 1920×1080 drawable:
`mt_present.mm` copies it 1:1, exactly. The D3D9 adapter's
desktop mode becomes the display in pixels (a strong `sg_host_display_mode` in `sgp_backend_mac.c`;
`SPORE_DISPLAY_MODE` still wins), so the game's resolution list reaches the display's native size, for example
4112×2658 on a 2056×1329-point display [inf: computed, not seen in the game]. The game lays out its UI in
client pixels, so at a pixel-sized client every button and text is half its physical size, and a resolution
chosen in points gives a half-size window. That is why the switch is opt-in (DEVIATIONS 4.8).
In fullscreen the client is still the game's resolution, letterboxed; choosing the display's pixel size as the
resolution makes it 1:1 there too.

### 11.5 Tests

- **`sphost --display-test`** (also `--retina` and `--no-fullscreen`, in `make -C runtime/platform/macos check-gui`):
  windowed client and mouse mapping; a resize sends `WM_SIZE`; Cmd-Ctrl-F into fullscreen (frame = the screen's
  frame, borderless, no `WM_SIZE`, no key message to the game, client kept, 4:3 content 1772×1329 at 142,0 on a
  2056×1329 display, the layer's rectangle matches, mouse (0,0), centre and corner exact); the game setting a 16:9
  resolution while fullscreen; the stretched mapping; Cmd-Ctrl-F back (frame restored, titled, menu bar and Dock
  back); Retina guest units (view = client/2, caption ×2, drawable = client). With `SPHOST_KEY=1` (the window takes
  the focus) the menu-bar check also runs.
- **The guest GUI smoke** runs the game's fullscreen sequence through the imports (`SetWindowLong(0x80080000)`,
  `SetWindowPos(HWND_TOPMOST, 0,0,1280,960, SWP_FRAMECHANGED)`, `ShowWindow` 3 and 1): fullscreen, `GetClientRect`
  1280×960, `WM_SIZE`, `GetWindowRect` at (0,0), letterbox 4:3 and centred, `WM_MOUSEMOVE` at client (640,480) and
  (10,950), `GetCursorPos` + `ScreenToClient` = (10,950); then the windowed style and `SetWindowPos` back to a
  titled 800×600 client.
- **The Metal presenter's tests** cover the 1:1 copy (exact, 0 of 76,800 pixels differ; a filter half a texel off
  changes 1,108), `mt_present_dest` for absent, whole, degenerate and 4:3-in-wide rectangles, a 2× letterbox into 840×480
  with 100-pixel black bars, a 1.6× letterbox with every bar pixel black and every inside pixel within the texels it
  covers, and Presents into a window with a rectangle set.

### 11.6 The notch (`SPORE_FULLSCREEN_SAFE_AREA`, off by default)

A MacBook's built-in display has a strip at the top beside the camera that cannot show a picture; AppKit reports
it as `NSScreen.safeAreaInsets` (macOS 12 and later; on a 2056×1329-point MacBook display the top 38 points, `auxiliaryTopLeftArea`/`auxiliaryTopRightArea` 918×38 either side of the camera). Without the switch the
fullscreen letterbox uses the whole frame, so a picture of the display's own aspect (`-r:2056x1329`, or
4112×2658 with Retina) runs under the notch and a 16:9 one is centred in the whole frame. With
`SPORE_FULLSCREEN_SAFE_AREA=1`:

- **The window** still covers the whole display (the strip stays black, as macOS's own fullscreen leaves it), but
  `layout()` fits `s->content` inside the safe area: the view's bounds less the insets, in the view's top-left
  coordinates, snapped to whole drawable pixels like the rest of the letterbox. Everything derived from
  `s->content` moves with it: the layer's `sporeContentRect` (the Metal presenter clears the strip with the bars), the
  mouse mapping, `SetCursorPos`/`GetCursorPos`, drag and drop. The client stays at the guest screen's origin
  (`GetWindowRect` (0,0), `GetClientRect` the game's resolution), so guest (0,0) is the top-left of the area
  below the strip.
- **The screen** the game sees is the safe area: `sp_mac_screen_info` (so `GetSystemMetrics`, the desktop window's
  rectangles, `GetMonitorInfo`, `EnumDisplayMonitors`, `WM_DISPLAYCHANGE`) reports its size in guest units, and
  `HORZSIZE`/`VERTSIZE` the safe area's share of the panel in millimetres, because the game computes the monitor's
  aspect from them (0x75da80). The work area stays the real visible frame (it is below the notch already).
- **The resolution list.** RenderWare's 0x11f88f0 copies every mode `EnumAdapterModes` returns for four adapter
  formats into its mode table; the game's settings code 0x7ccf00 then makes an `App/ConfigManager/ResolutionInfo`
  entry for each mode in formats 0x15/0x16/0x21 (A8R8G8B8, X8R8G8B8, X8B8G8R8) whose size is at least the
  minimum (properties 0x46c0c15/16, 800×600 if unset) and at most the desktop mode (0x11f82a0, from
  `GetAdapterDisplayMode`). When fullscreen is requested, 0x75e9a0 asks 0x75ddd0 whether (width, height,
  refresh) is one of the enumerated modes and falls back to windowed if not. So the list is the adapter's modes
  capped by the desktop mode, and the runtime decides both. `sg_host_display_mode` (`sgp_backend_mac.c`) reports
  the safe area: in pixels with `SPORE_RETINA=1` (4112×2582 on that display, at the panel's refresh), and in
  points mode the safe area in points at 60 Hz (2056×1291) instead of the whole display in points (§11.7). The
  null adapter's mode list (`d3d9_adapter.c`, shared by Metal) is the standard sizes up to the desktop plus the
  desktop itself, so the below-the-notch size appears in the game's list and validates as `-r:` in fullscreen.
  The full-display size is then above the desktop and not listed; with the switch on it would only be scaled
  down into the safe area.
- **Unchanged:** windowed mode; a display whose insets are zero (an external display, a Mac without a notch); the
  switch unset or `0`, where every expected log and test holds byte for byte.

For tests, `SPORE_FULLSCREEN_SAFE_AREA=top[,bottom,left,right]` (points) applies those insets on any display;
the value is read on every use, so one process can switch it. Verification: `sphost --safe-area-test` and
`--safe-area-test --retina` (28/0 each: screen size less 38 or 76, the physical size scaled, the switch at `0`,
unset and `1`, fullscreen at the display's aspect below the strip and centred across, the four corners mapped,
the layer's rectangle, 16:9 centred in the area below the strip, the safe area's own size filling it exactly,
switched off at run time, windowed again), and the guest GUI smoke's
`gui_safe_area` (the desktop mode, `GetSystemMetrics`, the game's fullscreen sequence at the safe area's size,
`GetCursorPos` + `ScreenToClient` at two corners). [DEVIATIONS.md](../DEVIATIONS.md) 4.12.

### 11.7 The desktop mode is the real display

The resolution list above is the adapter's modes capped by the desktop mode, so the desktop mode decides the
largest fullscreen size the game will accept. With a fixed 1920×1080@60, nothing above 1920×1080 could be chosen
on a 2056×1329-point display. `sg_host_display_mode`
(`sgp_backend_mac.c`) reports the main display's size in guest units in every windowed case: points at 60 Hz by
default (2056×1329), the safe area with `SPORE_FULLSCREEN_SAFE_AREA` (§11.6, 2056×1291), pixels at the panel's
refresh with `SPORE_RETINA=1` (§11.4). The adapter's mode list (`d3d9_adapter.c` `mode_list`) then holds the
standard sizes up to that size (on a 2056×1329 display 640×480 to 1920×1200; the 2560 sizes are wider) and the
display's own size as the last entry, which the game lists and accepts as `-r:2056x1329` in fullscreen, drawn
1:1 over the whole display. `SPORE_DISPLAY_MODE=WxH[@Hz]` overrides every case (`=1920x1080@60` gives the fixed mode the test binaries use); `SPORE_HEADLESS=1` and the test binaries without a window (the null device's weak default) keep
1920×1080@60. Verification: the guest GUI smoke's `gui_safe_area` (desktop mode = the display's point size at
60 Hz with the switch off, the safe area with it, `SPORE_DISPLAY_MODE=1920x1080@60` winning, `SPORE_HEADLESS=1`
keeping 1920×1080). The macOS package's `sphost` tests cover the screen size
(`sp_mac_screen_info`) the desktop mode is read from. [DEVIATIONS.md](../DEVIATIONS.md) 3.47.

## 12. Text input, dead keys and IME

Code: [`src/sp_textinput.inc`](../../runtime/platform/macos/src/sp_textinput.inc) (the core, plain C),
the `NSTextInputClient` methods and `ti_route` in [`src/sp_window.m`](../../runtime/platform/macos/src/sp_window.m),
the IMM32 imports in [`sgp_imm.c`](../../runtime/platform/guest/src/sgp_imm.c). The core is compiled once per
binary by `#include` from `sgp_imm.c` (game image, guest libraries) and from `tests/hostapp.m` (sphost), so no
build list needs it; `sp_window.m` has weak stand-ins, so a test app without the core gets plain key events.
Labels: [obs] read from the exe (capstone over `SporeApp.exe`) or measured here; [inf] inferred; [doc] Microsoft Learn.

**What the game does with IME messages** [obs]:

| Where | What |
|---|---|
| 0x849310 (window procedure) | Every message is offered to listeners first (event 0x1ee100c); if one sets "handled", the procedure returns its result and **`DefWindowProc` is not called** |
| 0x84b6e0 (IMEServer listener) | `WM_IME_STARTCOMPOSITION`: asks the UI (event 0x2ac44c0e, default answer 1) whether a field wants the IME; answer 0 → `ImmSetOpenStatus(himc, FALSE)`, answer 2 → complete each composition at once. `WM_IME_ENDCOMPOSITION`: tells the UI. `WM_IME_COMPOSITION`: reads the strings (0x84ae20), then hands each `GCS_RESULTSTR` character to the UI's input manager (0x84a620, the same character event `WM_CHAR` produces). All three are marked handled, so a Windows `DefWindowProc` never turns the result into `WM_IME_CHAR`/`WM_CHAR` for the game |
| 0x84ae20 | `ImmGetCompositionStringW` per `lParam` flag: `GCS_COMPATTR` (bytes, values 0-5 mapped through a table), `GCS_COMPSTR`, `GCS_RESULTSTR` (sets "result pending"), `GCS_CURSORPOS` (the return value, clamped to the string). Size first (buffer 0), then the data |
| 0x84a440 / 0x84a480 | IsOpen (`ImmGetOpenStatus`) / SetOpen (`ImmSetOpenStatus`, Korean layouts via `ImmSimulateHotKey`, `GetKeyboardLayout() & 0x3ff == 0x12`) |
| 0x84a550, 0x84b8ac | `ImmNotifyIME(NI_COMPOSITIONSTR, CPS_CANCEL)` / `(…, CPS_COMPLETE)` |
| 0x84a5b0, 0x84a720 | `ImmNotifyIME` `NI_OPEN/CLOSECANDIDATE`, `NI_SELECTCANDIDATESTR`; 0x84b470 reads `ImmGetCandidateListW` on `WM_IME_NOTIFY` (the game can draw a candidate list) |
| 0x84ac60 | `ImmSetCompositionStringW(SCS_SETSTR)` |
| 0x84a050 | The `PeekMessageA(WM_KEYFIRST, WM_KEYLAST)` drain (0x84a0e3) collects key messages 0x100-0x109 and drops repeated mouse moves and wheel messages; `WM_IME_*` (0x10d-0x10f) is outside its range and is not touched |

So the game needs the three composition messages and the strings, and no `WM_CHAR` for committed IME text;
sending both would type every character twice.

**Behaviour** (the view's `keyDown` path, `ti_route`, runs in the drain before `translate_key`):

- A key-down is offered to the input context (`interpretKeyEvents:`) unless Cmd or Ctrl is held, or it is one of
  the game's own keys (Return, keypad Enter, Tab, Backspace, Delete, Esc, arrows, Home/End/PgUp/PgDn, Help,
  Clear, F1-F20) and nothing is being composed, or the input source is an input method the game closed.
- **Plain text**: when the context inserts exactly `NSEvent.characters`, the plain key path runs unchanged (same
  `WM_KEYDOWN`, same `WM_CHAR`/`WM_SYSCHAR` rules). Other inserted text rides on the key-down: up to four UTF-16
  units through `TranslateMessage`, more as `WM_CHAR`s after it; a surrogate pair is two `WM_CHAR`s, as Windows.
- **Dead keys** (marked text from a keyboard layout: an input source whose id has no "inputmethod"): the key-down without a
  character, then the next key-down carries the composed character. No `WM_IME_*` (a Windows dead key is a
  layout feature). A pending dead key followed by one of the game's own keys is dropped.
- **Input methods**: the keys the IM takes arrive as `WM_KEYDOWN VK_PROCESSKEY` (scan code kept), followed in
  the input queue by `WM_IME_STARTCOMPOSITION` (once), `WM_IME_COMPOSITION(last char, GCS_COMPSTR|COMPATTR|
  COMPCLAUSE|CURSORPOS|DELTASTART = 0x1B8)` per change, and on commit `WM_IME_COMPOSITION(first char,
  GCS_RESULTSTR|GCS_RESULTCLAUSE = 0x1800)` then `WM_IME_ENDCOMPOSITION`. A cancel is `WM_IME_COMPOSITION(0, 0)`
  then `WM_IME_ENDCOMPOSITION`. Attributes: `ATTR_INPUT` until the IM selects a clause, then `ATTR_TARGET_CONVERTED`
  for the selection and `ATTR_CONVERTED` for the rest. Commits and marks outside a key (candidate window
  clicks, focus loss) are queued at once.
- **IMM32**: `ImmGetContext` returns `0x494D0000 | hwnd` (0 with `SPORE_TEXT_INPUT=0`, which also sends every key
  through the plain key path). `ImmGetOpenStatus` is 1 only for an input method the game has not closed (a plain layout
  reports 0, as Windows does for a layout without an IME); `ImmSetOpenStatus(FALSE)` cancels a composition
  through the view (`discardMarkedText`); choosing an input source or pressing Kana re-opens it.
  `ImmGetCompositionStringW` follows the byte-size rules (size for a zero buffer, truncation to the buffer,
  the value itself for `GCS_CURSORPOS`/`GCS_DELTASTART`, 0 for reading strings, `IMM_ERROR_GENERAL` for an unknown
  index or context). `ImmNotifyIME(NI_COMPOSITIONSTR)`: `CPS_COMPLETE` commits the marked string,
  `CPS_CANCEL` cancels; the rest returns 0. `ImmGetCandidateListW` 0 (the IM shows its own list),
  `ImmSetCompositionStringW` 0, `ImmSimulateHotKey` 0, conversion mode `IME_CMODE_NATIVE` while open.
- **`DefWindowProc`** [doc]: an unhandled `WM_IME_COMPOSITION` with a result sends `WM_IME_CHAR` per unit and an
  unhandled `WM_IME_CHAR` posts `WM_CHAR` (`sp_ti_def_window_proc`), reached from `sp_def_window_proc` in `sp_queue.c`. The game never needs it.
- **Candidate window position** [inf]: `firstRectForCharacterRange:` returns the mouse pointer, because the game
  never reports its caret (no `ImmSetCompositionWindow`/`ImmSetCandidateWindow` import).
- Test mode keeps synthetic keys off the input context (the reference replay stays byte for byte on any layout)
  unless `sp_mac_set_text_test(1)`.

**Tests.** `sphost --text-test` (in `check-gui`): 'a' through the real input context = `WM_KEYDOWN 'A'`,
`WM_CHAR 'a'`, `WM_KEYUP` (as the replay); Option-e then e: `WM_SYSKEYDOWN 'E'` with no character and no `WM_IME_*`,
then `WM_KEYDOWN 'E'`, `WM_CHAR 0xE9`; Option-x still `WM_SYSCHAR 'x'`; dead key + Left drops the dead key;
`setMarkedText:`/`insertText:` on the view: START, COMPOSITION('k'), COMPOSITION(か), COMPOSITION(`GCS_RESULTSTR`),
END, `GCS_COMPSTR`/`GCS_RESULTSTR` = か; `CPS_COMPLETE` and `ImmSetOpenStatus(FALSE)` through the view's
`discardMarkedText`. `make -C runtime/platform/guest test` adds 62 text-input checks: the dead key, a surrogate pair,
a Japanese composition and commit, every `ImmGetCompositionStringW` index, cancel/complete, close/re-open, and the
`WM_IME_CHAR` → `WM_CHAR` chain through the guest `DefWindowProcW`.

The real dead-key composition needs key events with the machine's keyboard type: an `NSEvent` from
`keyEventWithType:` (keyboard type 0) made Option-e e give U+00B4 then 'e', one from `CGEventCreateKeyboardEvent`
(type 91 here) gives é [obs]; hardware events carry it. A real input method (Kotoeri, Pinyin) was not driven:
that would change the user's input source.

**Open points**: whether the game opens the IME itself when a text field takes the focus (SetOpen is a virtual
call; not traced); if it does not, a player whose IME the game closed in play re-opens it by choosing the input
source again or pressing Kana; `WM_DEADCHAR` is not sent; the candidate window follows the mouse, not the caret.
