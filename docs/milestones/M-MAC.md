# M-MAC — macOS port: Metal backend + macOS platform (milestone record)

**Status:** first landing — `mce_rhi_metal`, the backend factory, the macOS build and `local_ci_macos.sh`. Every
pipeline the engine creates today renders on Metal; `.app` packaging and the parity backlog are open (below).
**Design and ownership:** `docs/architecture/macos-metal.md`. **Decision record:** ADR-0027.
**Measured on:** MacBook (Apple M4), macOS 26.6, Xcode toolchain (AppleClang 17), `scripts/ci/local_ci_macos.sh`.

## What shipped

| ROADMAP M-MAC item | Where | Evidence |
|---|---|---|
| Backend factory: hosts stop calling `rhi::gl::create_device` | `src/engine/rhi_default` (`rhi::create_default_device`) | samples, `mce_player`, `mce_editor`, `test_render_gl` link `mce_rhi_default`; Metal on macOS, GL elsewhere |
| Metal-capable window | `platform/src/window.cpp` | `SDL_WINDOW_METAL` on macOS, GL attributes only on the GL path |
| Metal wall (+ shader-cross wall) | `scripts/ci/check_walls.py` | `MTL*`, `<Metal/…>`, `<QuartzCore/…>` only under `rhi_metal` and `platform/src/macos`; glslang/SPIRV-Cross only under `rhi_metal` |
| `mce_rhi_metal` | `src/engine/rhi_metal` | buffers, textures (2D, arrays, MSAA, BCn, depth), samplers (incl. compare), pipelines, `CommandBuffer` → encoders, 3 frame slots on shared rings, MDI with `[[base_instance]]`, resolve, readback |
| Shaders | `rhi_metal/src/shader_cross.cpp` | GLSL 450 → SPIR-V → MSL at pipeline creation (ADR-0027 §2: deviation from the cook-time default, see Open) |
| `.dylib` modules | `src/module/CMakeLists.txt` | `-force_load` umbrella, `@loader_path` rpath; `test_module`, `abi_spike`, the devhost and `03_platformer` load game modules |
| Presets | `CMakePresets.json` | `macos-debug`, `macos-release`, `macos-asan` |
| Mac CI | `scripts/ci/local_ci_macos.sh` | walls, three presets, Metal validation, windowed samples with captures, determinism |

## Tests on Metal

| Suite | What it covers |
|---|---|
| `test_shader_cross` (no GPU) | binding map, base instance, `gl_InstanceID` fix, clip fixup, flip constant, error Results |
| `test_rhi_metal` | the `test_rhi_gl` cases plus: rendered textures sample upright, back-face culling agrees between backbuffer and texture targets, depth test on a depth target and on the default depth buffer, 4× MSAA resolve, SSBO + `gl_BaseInstanceARB` through `draw_indirect`, ordered `update_buffer` |
| `test_render_gl` (via the factory) | the renderer's 2D and 3D pixel probes, unchanged, on Metal |

## Verification (local_ci_macos.sh)

Filled per landing in the PR description; the first landing's table is in the PR that merged this file.

## Open (the M-MAC backlog)

| Item | State |
|---|---|
| Shader translation at cook time (ADR-0027 §2) | waits for cooked shaders (`ShaderKey`); then the translator moves into the cook and leaves `mce_rhi_metal` |
| `.app` packaging for the player, 10-minute soak from Finder | not started (`scripts/ship.py` produces the Linux/Windows layouts) |
| Xcode GPU capture per backend feature | not yet recorded |
| Deployment target below macOS 26 | needs a float `from_chars` fallback in the cook's JSON reader (cloud-owned) |
| Readback of R11G11B10F / depth targets | unsupported (returns `Unsupported`); GL reads them |
| AddressSanitizer on macOS | the ASan runtime (AppleClang 17 and Homebrew LLVM 21) hangs in shadow-memory init on macOS 26.6 even for a 3-line program; `macos-asan` runs UBSan only until a toolchain update fixes it |
| Indirect command buffers for MDI | one direct draw per record today |
| Parity with RHI features that land on GL after this (M5a PBR, CSM, post chain, …) | per feature, as they land |
