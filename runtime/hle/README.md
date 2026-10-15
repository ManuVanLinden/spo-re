# runtime/hle: import shims and the host CRT

The design, and the MSVC behaviours implemented with their sources, are in [docs/design/hle-crt.md](../../docs/design/hle-crt.md).

```
python3 tools/hle/gen_thunks.py      # regenerate gen/ (after adding a shim or editing tools/hle/sigs.py;
                                     # reads data/imports.json: run tools/imports/ by hand first)
make -C runtime/hle test             # build build/hle/libhle.a and run the host-only unit tests
```

To add a shim, define `SG_IMPORT(<dll stem>, <export>) { ... }` in `src/*.c` and rerun the generator, which then
drops its "unimplemented" stub for that import. Nothing here runs game code.
