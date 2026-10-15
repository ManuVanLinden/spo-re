# runtime/image: image loader, dispatch support and `main`

The design, measurements and start-up order are in [docs/design/image-linker.md](../../docs/design/image-linker.md).

| File | What |
|---|---|
| `include/sg_image.h` | loader, thunk, dispatch and import-override API |
| `src/loader.c` | guest window, PE section mapping (from the exe, or embedded), page protections, fault report |
| `src/thunks.c` | thunk region and a weak `sg_thunk_register` / `sg_thunk_fn` (runtime/core's strong ones win) |
| `src/dispatch_rt.c` | miss hook (address, caller, ebp chain, `misses.txt`), exact lookup, resume-point lookup |
| `src/imports.c` | `sg_import_override`, IAT and dynamic-name installation through runtime/hle |
| `src/weak_runtime.c` | weak defaults for every `[runtime]` symbol, so the image also links without runtime/core and runtime/win32 (tests, partial builds) |
| `src/main.c` | `build/sporenative/spore`: `--smoke`, `--selftest`, `--probe-miss`, `--run` |

Build and test (all output under `build/sporenative/`, git-ignored):

```
python3 -P tools/link/build.py --tus 512 --embed     # translate, compile, link spore and spore-embed, smoke + self-test
build/sporenative/spore --smoke                       # map the image, install the IAT, exit before any game code
build/sporenative/spore --selftest build/sporenative/gen/entries.txt
```

This directory holds no game code and no game bytes.
