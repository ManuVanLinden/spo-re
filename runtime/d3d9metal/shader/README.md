# runtime/d3d9metal/shader: D3D9 shader bytecode to MSL at run time

The Metal back end's shader translator. `CreateVertexShader` and `CreatePixelShader` hand it the game's
bytecode (vs/ps 2_0, 2_x, 3_0). It returns Metal Shading Language source and a reflection record, and it
caches both under `build/d3d9metal/shadercache/`.

It is a C port of the Python prototype `tools/shaders/msl.py`, with the parser from `d3dbc.py`. The Python
emitter stays the reference. For the same input, the two emit **the same MSL byte for byte and the same
reflection**: `tests/crosscheck.py` checks every fixture, and every one of the 8,091 unique game shaders.

Labels: **[obs]** measured here, **[inf]** reasoned, **[web]** external source.

## API (`d3d9msl.h`)

```c
d3d9msl_options o = { "sg_main", 0 };            /* entry name, D3D9MSL_OPT_* flags */
d3d9msl_result r;
int rc = d3d9msl_translate_cached(tokens, ntokens, &o, &r, &hit);  /* or d3d9msl_translate[_opts] */
/* rc 0: r.msl (NUL-terminated), r.refl;  rc < 0: r.error ("predication", "bad version token ...") */
d3d9msl_result_free(&r);
```

- `d3d9msl_length(tokens, max)` finds the bytecode length from the version token through the end token.
- `d3d9msl_hash` is FNV-1a 64, the same as `mt_obj.code_hash`.
- `d3d9msl_reflection_json` returns the reflection as the dict that `msl.translate` returns.
- `d3d9msl_prelude()` returns the shared prelude.
- Options:
  - `FOG_VERTEX`: ps < 3_0 blends with the VS fog output.
  - `FLAT`: colour inputs get `[[flat]]`, for `D3DSHADE_FLAT`.
  - `PLAIN`: test mode.
  - `NO_PRELUDE`: lets several shaders share one library.

  Options change the text, so they are part of the cache key and of any pipeline key that depends on them.
- Reentrant. The only global state is the cache directory and its counters, both under a mutex.

## Binding model (the contract with the device layer and the pipeline)

The model matches `runtime/d3d9metal/include/d3d9metal_int.h` (`MT_BUF_*`, `MT_TEX_*`).

| Index | What | Layout |
|---|---|---|
| `buffer(0)` | float constants | `float4 c[256]` (VS) / `c[224]` (PS); register *n* at byte 16·*n* |
| `buffer(1)` | int constants | `int4 ci[16]` |
| `buffer(2)` | bool constants | `uint cb[16]`, 0 or 1 |
| `buffer(3)` | `SgAux` = `d3d9msl_aux`, 592 bytes | `alphaRef` @0, `fogColor` @16, `fogParams` @32, `rtSize` @48, `posFixup` @64, `samp[16]` @80 (32 bytes each: `border` float4, `mipBias`, `flags`) |
| `buffer(14 + k)` | vertex stream *k* | `[[attribute(n)]]` = input register `v<n>` |
| `texture(n)`, `sampler(n)` | PS: sampler `s<n>`; VS: vertex texture sampler *n* (guest 257 + *n*) | |
| `function_constant(0)` | alpha-test `D3DCMPFUNC` (PS) | unset, 0 or 8 = off |

VS outputs and PS inputs link by name, `[[user(<usage><index>)]]`.

**Constants use three buffers per stage**, one per register file, rather than one argument buffer, for two
reasons:

- `d3d9metal_int.h` fixes indices 0, 1 and 2, and that header only grows.
- Each file then fits `set*Bytes` (4 KB limit): VS `c[]` is exactly 4,096 bytes, PS `c[]` 3,584.

`c_max`, `c_used` and `relative_c` in the reflection tell the runtime how much to upload.

## D3D9 conventions in the emitted code

Everything `msl.py` already did is ported unchanged:

- `rsq`, `log` and `pow` take `abs()`.
- `mova` rounds to nearest even.
- `def` overrides constants, including relative reads; out-of-range relative reads return 0.
- `aL` loops, `rep`, `if b#`, `ifc` and `breakc`.
- `sincos` writes `.x` = cos and `.y` = sin.
- `texkill` tests the write-masked components.
- `oFog` and `oPts` are scalar outputs, as is `psize`.
- Defaults for unwritten outputs: color0 = 1, other colours = (0,0,0,1), fog = 1.
- `vPos` = position − 0.5, and `vFace` = ±1.
- The ps_2_0 `t#`/`v#` inputs, and `texld`/`texldp`/`texldb` on ps_2_0.
- The SM2 vertex-fog option.
- The alpha test runs at 8-bit precision.
- `_pp` gives full precision.
- Centroid inputs become `centroid_perspective`.

The translator refuses predication with an error; the corpus has none.

Added in the C port (and to `msl.py`, so the two stay identical):

- **Half-pixel offset.**
  - In the shader: the VS ends with `pos.xy += aux.posFixup.xy * pos.w`. The runtime either sets
    `posFixup = (+1/vpWidth, −1/vpHeight)`, or leaves it zero and shifts the Metal viewport origin by +0.5 px
    (what the pipeline does), never both.
  - GPU check: a D3D9 quad over window [−0.5, 3.5]² covers exactly pixels 0..3 × 0..3 with the fixup.
    Without it, the same quad covers 9 pixels [obs, `test_d3d9msl`].
  - The sign is (+1/w, −1/h); (−1/w, +1/h) shifts the wrong way. D3D9 puts pixel centres at integer window coordinates and Metal at +0.5, so geometry must move half a pixel
    right and down.
- **Per-sampler state Metal samplers cannot hold, in `SgAux.samp[n]`.** All `texld*` forms go through the
  prelude's `sg_tex2d|cube|3d_{b,l,d}` helpers:
  - `mipBias` (`D3DSAMP_MIPMAPLODBIAS`): a Metal sampler has no LOD bias. The bias applies to `texld`/`texldb`
    and to `texldl`, whose MSDN page says "Sampler states MIPMAPLODBIAS and MAXMIPLEVEL are honored"
    [web: learn.microsoft.com/windows/win32/direct3dhlsl/texldl---ps]. It does not apply to `texldd`.
  - `SG_SAMP_SRGB` (`D3DSAMPLER_SRGBTEXTURE`): decodes RGB after the fetch. Alternatively, bind an sRGB view
    and leave the flag clear; that filters in linear space, as hardware does.
  - `SG_SAMP_BORDER_U/V/W` with `border`: `D3DTADDRESS_BORDER` with a colour that `MTLSamplerBorderColor`
    cannot express (only transparent black, opaque black and opaque white exist). The runtime sets
    ClampToEdge on that axis, and the shader returns the border colour outside [0,1].
  - `SG_SAMP_DEPTH_CMP`: a depth texture is bound. The shader returns `ref <= depth`, with ref = coord.z
    (`texldp`: z/w). This is what D3D9 shadow-map hardware does and what the conformance test's (`tests/conformance.cpp`)
    `shadow.depth_compare` checks.
  - Filters, `MAXANISOTROPY`, `MAXMIPLEVEL` (lodMinClamp) and the WRAP/MIRROR/CLAMP/MIRRORONCE address modes
    are Metal sampler properties.
  - Missing format channels (L8, A8, X8R8G8B8) are texture-view swizzles.
  - An unbound sampler still needs a bound 1×1 (0,0,0,1) texture.
- **Reflection**:
  - input registers with usage, index, attribute, centroid and flat;
  - outputs with MSL name, binding, kind, semantic and scalar flag;
  - sampler mask and types;
  - `c_max` and `relative_c`;
  - bitmaps of the float, int and bool constants read from the buffers (def'd ones excluded);
  - `uses_vpos`, `uses_vface` and `uses_texkill`;
  - `position_fixup`, `alpha_test` and `fog_input`.

## Cache

**Files.** One file per shader: `<dir>/<16 hex>.d3d9msl`.

- Key: FNV-1a 64 over the bytecode, `D3D9MSL_VERSION`, the option flags, the entry name and the size
  of the reflection struct.
- Contents: header, bytecode, entry, reflection struct, MSL. The bytecode is compared on load, so a
  hash collision is only a miss.
- Writes: to a temporary name, then a rename, which is safe with concurrent processes.

**Directory.** `SPORE_D3D9MSL_CACHE`, or `$SPORE_REPO_ROOT/build/d3d9metal/shadercache`; `off` disables it.

**The cache does not store compiled libraries**: without the offline `metal` tool there is no `MTLLibrary`
serialisation.
The OS keeps its own cache of `newLibraryWithSource` results [inf]. `MTLBinaryArchive`, which works without
Xcode, is a pipeline-level choice (`pipeline/pl_shader.m` uses it).

## Build and tests

```
make -C runtime/d3d9metal/shader test      # about 1 minute
make -C runtime/d3d9metal/shader corpus    # development check, see below; ~25 min
```

`make corpus` runs every unique game shader through the C library and the Python reference and compiles the
result. It reads the game's compiled-shader containers (`material_shaders~`, DBPF group `0x40212004`) from
`$SHADER_CORPUS` (default `scratch/shaders`, git-ignored); extracting them and reading their `.smt` format takes
development tools that are not part of this source tree, so in this tree only `make test` applies.

| Check | Result |
|---|---|
| `crosscheck.py`: our fixtures (10 `tools/shaders/fixtures`, the 7 conformance-test shaders (`tests/shaders`), the semantics test's edge-case shader) × 5 option sets, C vs `msl.py` | 90/90 identical MSL and reflection (80 translated, 10 `PLAIN` refusals on PS or sampler shaders, refused by both) [obs] |
| `test_d3d9msl`: API and errors, cache, every fixture blob translated in-process and compiled (`MTLMathModeSafe`, specialised), GPU checks of the conventions above | 131/131 [obs]. Translation is ~40 µs per fixture; Metal compile ~157 ms per fixture |
| `test_semantics.py --impl d3d9msl_tool`: the GPU semantics test on the C library's output | 11,136/11,136 components, the same as with `msl.py` [obs] |
| `test_shaders.py --metal`: Python round-trip and compile of the fixtures | PASS, 10/10 [obs] |
| Corpus (`tests/corpus.sh`): 8,091 unique blobs from the EP1 and base-game `.smt` containers, C vs `msl.py`, then a Metal compile | **8,091/8,091** identical MSL and reflection; **8,091/8,091** compile. C translation ~1 s per 2,000 blobs; compiling took 1,208 s in four chunks (329, 354, 266, 259 s), with a log per chunk [obs] |

## Compile cost [obs, a one-off benchmark over 18 fixture shaders]

| How | Cost per shader |
|---|---|
| A new source, compiled alone | **130–176 ms**. Most of this is `metal_stdlib` plus the prelude |
| The same source again, in the same process | 0.01 ms |
| The same source again, in another process (the OS Metal cache) | ~0 ms (`make test`'s second run) |
| 18 shaders concatenated into one library (`NO_PRELUDE`, prelude once) | 13 ms |
| 18 asynchronous `newLibraryWithSource:completionHandler:` calls in parallel | 18 ms |

Compiled one at a time on the render thread, the game's few thousand shaders would cost minutes of stalls the
first time they are created. The OS cache makes later runs cheap [obs]. For the pipeline this means
(`pipeline/pl_shader.m` does both):
  - compile asynchronously when the shader is created, and wait only at the first draw that needs the
    shader;
  - or batch the shaders created in one burst into one library [inf].
