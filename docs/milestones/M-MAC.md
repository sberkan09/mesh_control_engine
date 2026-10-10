# M-MAC — macOS port: Metal backend + macOS platform (milestone record)

**Status:** first landing — `mce_rhi_metal`, the backend factory, the macOS build and `local_ci_macos.sh`. Every
pipeline the engine creates today renders on Metal; `scripts/ship.py` produces a signed `.app`. The parity backlog
is open (below).
**Design and ownership:** `docs/architecture/macos-metal.md`. **Decision record:** ADR-0028.
**Measured on:** MacBook (Apple M4), macOS 26.6, Xcode toolchain (AppleClang 17), `scripts/ci/local_ci_macos.sh`.

## What shipped

| ROADMAP M-MAC item | Where | Evidence |
|---|---|---|
| Backend factory: hosts stop calling `rhi::gl::create_device` | `src/engine/rhi_default` (`rhi::create_default_device`) | samples, `mce_player`, `mce_editor`, `test_render_gl` link `mce_rhi_default`; Metal on macOS, GL elsewhere |
| Metal-capable window | `platform/src/window.cpp` | `SDL_WINDOW_METAL` on macOS, GL attributes only on the GL path |
| Metal wall (+ shader-cross wall) | `scripts/ci/check_walls.py` | `MTL*`, `<Metal/…>`, `<QuartzCore/…>` only under `rhi_metal` and `platform/src/macos`; glslang/SPIRV-Cross only under `rhi_metal` |
| `mce_rhi_metal` | `src/engine/rhi_metal` | buffers, textures (2D, arrays, MSAA, BCn, depth), samplers (incl. compare), pipelines, `CommandBuffer` → encoders, 3 frame slots on shared rings, MDI with `[[base_instance]]`, resolve, readback |
| Shaders | `rhi_metal/src/shader_cross.cpp` | GLSL 450 → SPIR-V → MSL at pipeline creation (ADR-0028 §2: deviation from the cook-time default, see Open) |
| GPU timestamps | `rhi_metal/src/metal_device.mm` (`init_timestamps`, `collect_timestamps`) | `caps().timestamp_queries` true on Apple silicon; every encoder samples start/end into the slot's `MTLCounterSampleBuffer` (Apple GPUs only sample at stage boundaries), `write_timestamp(i)` = latest valid sample of the encoders before it (GL `glQueryCounter` semantics); `render3d_gl: GPU timestamps time every stage` now runs on Metal |
| `.dylib` modules | `src/module/CMakeLists.txt` | `-force_load` umbrella, `@loader_path` rpath; `test_module`, `abi_spike`, the devhost and `03_platformer` load game modules |
| Presets | `CMakePresets.json` | `macos-debug`, `macos-release`, `macos-asan` |
| Mac CI | `scripts/ci/local_ci_macos.sh` | walls, three presets, Metal validation, windowed samples with captures, determinism, ship |
| `.app` packaging | `cmake/macos/macos_bundle.py`, darwin hooks in `scripts/ship.py` | `ship.py platformer` / `--project projects/SolsticeReach` write `<Name>.app` (executable + game module + `libmce_engine.dylib` in `Contents/MacOS`, pack/VERSION/CREDITS in `Contents/Resources`, where `SDL_GetBasePath` points; a module symlink there keeps the base-path lookup), dSYM symbol archive beside the zip (`symbolicate.py` finds it by LC_UUID), `strip -x`, ad-hoc signature, `codesign --verify --strict`. Zips keep the bundle symlink; `ditto -x -k` extraction verifies |
| Read-only bundle | `module_host` maps the module in place when its folder is read-only (8647148, cloud) | 2026-10-11: the Ember Step `.app` made read-only (`chmod -R a-w`, as App Translocation does) starts, loads its module in place and exits 0 |
| R11G11B10F readback | `rhi_metal/src/metal_device.mm` (`read_pixels`) | unpacked to RGBA8 like GL's `glGetTextureSubImage(GL_RGBA, GL_UNSIGNED_BYTE)`: channels clamped to [0, 1], alpha 255 (`test_rhi_metal`). Depth and multisampled targets stay `Unsupported`, which matches GL, where that call is an error for them |
| 10-minute Finder soak | Ember Step `.app`, opened through LaunchServices (`open`, the Finder path) | 2026-10-10: 10 min alive, RSS flat at 138.6 MB after start-up, CPU steady 11.5 %, clean exit on SIGTERM, no crash report (title screen, no input; lid closed, so audio ran silent) |

## Tests on Metal

| Suite | What it covers |
|---|---|
| `test_rhi_metal` timestamps | indices bracket passes, an empty stage reads 0 ms, unwritten indices stay missing, read back MCE_FRAMES_IN_FLIGHT frames later |
| `test_shader_cross` (no GPU) | binding map, base instance, `gl_InstanceID` fix, clip fixup, flip constant, error Results |
| `test_rhi_metal` | the `test_rhi_gl` cases plus: rendered textures sample upright, back-face culling agrees between backbuffer and texture targets, depth test on a depth target and on the default depth buffer, 4× MSAA resolve, SSBO + `gl_BaseInstanceARB` through `draw_indirect`, ordered `update_buffer` |
| `test_render_gl` (via the factory) | the renderer's 2D and 3D pixel probes, unchanged, on Metal |

## Verification (local_ci_macos.sh)

Filled per landing in the PR description; the first landing's table is in the PR that merged this file.

## Open (the M-MAC backlog)

| Item | State |
|---|---|
| Shader translation at cook time (ADR-0028 §2) | waits for cooked shaders (`ShaderKey`); then the translator moves into the cook and leaves `mce_rhi_metal` |
| Developer ID signing + notarization | out of scope until FR-SHP-030; bundles are signed ad hoc |
| Xcode GPU capture per backend feature | not yet recorded |
| Deployment target below macOS 26 | needs a float `from_chars` fallback in the cook's JSON reader (cloud-owned) |
| AddressSanitizer on macOS | the ASan runtime (AppleClang 17 and Homebrew LLVM 21) hangs in shadow-memory init on macOS 26.6 even for a 3-line program; `macos-asan` runs UBSan only until a toolchain update fixes it |
| Indirect command buffers for MDI | one direct draw per record today |
| Parity with RHI features that land on GL after this (M5a PBR, CSM, post chain, …) | per feature, as they land |
