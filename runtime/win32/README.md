# runtime/win32: CRT start-up, guest heap, files, registry and paths

The design, behaviours, sources and the start-up order for host programs are in
[docs/design/win32-layer.md](../../docs/design/win32-layer.md).

```
python3 tools/win32/gen_shims.py      # regenerate src/w32_shims.h and src/w32_overrides.c
make -C runtime/win32 test            # build build/win32/libw32.a and run the host-only tests
make -C runtime/win32 test-cs         # the same with a case-sensitive APFS image attached
```

Shims are `W32_IMPORT(dll, export)` functions named `w32_imp_*`; `w32_register_shims()` swaps them into
runtime/hle's `hle_imports[]`. Translated code calls `sg_imp_*` symbols directly, so runtime/hle's generated
stubs must be weak for every call site to reach these shims (section 10 of the design page). Nothing here runs game code.
