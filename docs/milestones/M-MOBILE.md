# M-MOBILE — iOS/Android port (milestone record)

**Status:** active (un-parked 2026-10-11 by the user). **Decision record:** ADR-0038 (one SDL3 GPU backend for
both phones). **Requirements:** FR-SHP-062, FR-INP-070, FR-RUN-100 (REQUIREMENTS slip note 2026-10-11).
**Scope:** the player only. The editor, the cook and the MCP server stay desktop tools; a phone runs a shipped game.

## Plan

Steps run in order; each lands as its own PR verified by `scripts/ci/local_ci.sh` (cloud) and, from step 6 on, by
a device or simulator run on the user's Mac.

| # | Step | Where it is verified | State |
|---|---|---|---|
| 1 | ADR-0038, REQUIREMENTS slip note, this plan | read-through (docs only) | done |
| 2 | `mce_rhi_sdlgpu` spike on Linux: window surface seam (`platform::gpu_api()`), device, clear + triangle, binding map, copy-pass split | `local_ci.sh` + lavapipe | done |
| 3 | `mce_rhi_sdlgpu` parity with GL: textures/arrays/MSAA/resolve, samplers incl. compare, MDI, storage data, readback; GPU tests run on both backends (`test_render_sdlgpu`, `MCE_RHI=sdlgpu`) | `local_ci.sh` + lavapipe | done (gaps below) |
| 4 | Touch input (FR-INP-070): `Touch{Down,Move,Up}` platform events, touch state per tick in `mce_input`, touch bindings (virtual stick, buttons, tap/swipe), `mce_ui` hit-testing by touch, record/replay of touches | `local_ci.sh` (synthetic touch streams, determinism) | done |
| 5 | App lifecycle (FR-RUN-100): background/foreground events, sim and audio pause, autosave on background, GPU surface loss and recreation, orientation and safe-area insets exposed to `mce_ui` | `local_ci.sh` (synthetic lifecycle events) | — |
| 6 | Android: `android-arm64` presets (NDK toolchain), SDL's Java activity, Gradle wrapper project stamped by `scripts/ship.py --target android`, APK/AAB with the cooked pack in assets | emulator + a device, on the Mac | — |
| 7 | iOS: `ios-arm64` / `ios-sim-arm64` presets, Xcode project generation, the statically linked game module (`MCE_PROJECT_STATIC_MODULE`), signing, IPA from `scripts/ship.py --target ios` | simulator + a device, on the Mac | — |
| 8 | Mobile budgets: quality tiers (shadow cascades, MSAA, post FX, render scale), ASTC texture profile in the cook, frame/thermal/battery numbers per tier | devices, on the Mac | — |
| 9 | Demo: Ember Step (2D) and Solstice Reach or `04_diorama` (3D) playable by touch on one Android and one iOS device; 10-minute soak each | devices, on the Mac | — |

## What shipped (steps 2–3)

| Item | Where | Evidence |
|---|---|---|
| Backend | `src/engine/rhi_sdlgpu` (`sdlgpu_device.cpp`, `sdlgpu_translate.cpp`) | every `RenderDevice` call; per-slot geometry/indirect rings, transfer buffer and storage pool, slot fences from the `end_frame` command buffer |
| Shader path | `rhi_sdlgpu/src/shader_spirv.cpp` | GLSL 450 → glslang (Vulkan 1.1, SPIR-V 1.3) with `gl_VertexID`/`gl_InstanceID` kept at GL meaning and a vertex epilogue (clip z, y flip); `DescriptorSet`/`Binding` rewritten into SDL's sets; `test_shader_spirv` checks the tables without a GPU |
| Surface seam | `platform::gpu_api()` (`window.h`) | `MCE_RHI=sdlgpu` gives a Vulkan window; `rhi::create_default_device` follows it, so every sample, the player and the editor run on it unchanged |
| Tests | `tests/rhi_sdlgpu`, `test_render_sdlgpu` | `test_rhi_sdlgpu` (the `test_rhi_gl` cases) and all 20 `test_render_gl` cases (2D batches, MDI, text, tilemaps, 3D forward+, cascaded shadows, MSAA resolve, tonemap, skinning) pass on lavapipe |
| Samples | `04_diorama`, `05_ashen_court` with `MCE_RHI=sdlgpu` | 60/30-frame captures differ from GL's in 0.09 % / 0.13 % of pixels (edge rasterization), screenshots in the project files (`m-mobile/`) |

**Gaps (reported, not faked):** no GPU timestamps (`caps().timestamp_queries == false`; the profiler's GPU column reads −1); `bind_storage_buffer` ignores a non-zero offset (no engine code uses one); `validation_error_count()` stays 0 because SDL exposes no validation callback; BC textures are rejected where the GPU lacks them (phones) until the ASTC cook profile (step 8); every frame ends with one full-screen blit from the offscreen backbuffer to the swapchain.

## Constraints found so far

- The cloud cannot reach `dl.google.com`, so the Android SDK/NDK cannot be installed there. Steps 6–9 run on the
  user's Mac (Android Studio for Android, Xcode for iOS). Steps 2–5 need no phone.
- `local_ci.sh` needs Mesa lavapipe (`mesa-vulkan-drivers`) for the `sdlgpu` GPU tests; `setup_env.sh` installs it.
- SDL is built with `SDL_GPU` / `SDL_VULKAN` only when `MCE_RHI_SDLGPU=ON` (the Linux default; off on macOS until
  step 7). The backend is chosen at run time, so GL stays what every host uses unless `MCE_RHI=sdlgpu` is set.

## Rules (inherited from M-MAC)

- GL stays the reference backend; new RHI features land on GL first and get an `sdlgpu` catch-up here.
- Determinism baselines are per platform (ADR-0007); phone hashes are never compared with desktop hashes.
- No Must FR rides this track; it gates no tier.
