# runtime/platform/guest: USER32, GDI32, IMM32, XInput and DInput8 for the guest

Strong `SG_IMPORT` definitions over [runtime/platform/macos](../macos/README.md). The design, threading and verification are in
[docs/design/platform-macos.md §10](../../../docs/design/platform-macos.md).

```
make -C runtime/platform/guest test        # host-only fixture: guest window procedure + the reference input script (MATCH 204)
make -C runtime/platform/guest check-gui   # AppKit smoke: a real window for ~1 s
make -C runtime/platform/guest nm          # the strong definitions win over runtime/hle's weak stubs
python3 runtime/platform/guest/gen_list.py # regenerate src/sgp_list.h after adding an import
```

`tools/link/build.py` links this package (and runtime/platform/macos) into the game. Environment: `SPORE_HEADLESS=1` (no AppKit), `SPORE_PAINT_HZ=n` (optional paint cadence).
These targets neither build nor run game code.
