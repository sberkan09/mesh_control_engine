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
| 2 | `mce_rhi_sdlgpu` spike on Linux: window `Surface` seam, device, clear + triangle, binding map, copy-pass split | `local_ci.sh` + lavapipe | next |
| 3 | `mce_rhi_sdlgpu` parity with GL: textures/arrays/MSAA/resolve, samplers incl. compare, MDI, storage data, readback; GPU tests run on both backends (`MCE_TEST_RHI=sdlgpu`) | `local_ci.sh` + lavapipe | — |
| 4 | Touch input (FR-INP-070): `Touch{Down,Move,Up}` platform events, touch state per tick in `mce_input`, touch bindings (virtual stick, buttons, tap/swipe), `mce_ui` hit-testing by touch, record/replay of touches | `local_ci.sh` (synthetic touch streams, determinism) | — |
| 5 | App lifecycle (FR-RUN-100): background/foreground events, sim and audio pause, autosave on background, GPU surface loss and recreation, orientation and safe-area insets exposed to `mce_ui` | `local_ci.sh` (synthetic lifecycle events) | — |
| 6 | Android: `android-arm64` presets (NDK toolchain), SDL's Java activity, Gradle wrapper project stamped by `scripts/ship.py --target android`, APK/AAB with the cooked pack in assets | emulator + a device, on the Mac | — |
| 7 | iOS: `ios-arm64` / `ios-sim-arm64` presets, Xcode project generation, the statically linked game module (`MCE_PROJECT_STATIC_MODULE`), signing, IPA from `scripts/ship.py --target ios` | simulator + a device, on the Mac | — |
| 8 | Mobile budgets: quality tiers (shadow cascades, MSAA, post FX, render scale), ASTC texture profile in the cook, frame/thermal/battery numbers per tier | devices, on the Mac | — |
| 9 | Demo: Ember Step (2D) and Solstice Reach or `04_diorama` (3D) playable by touch on one Android and one iOS device; 10-minute soak each | devices, on the Mac | — |

## Constraints found so far

- The cloud cannot reach `dl.google.com`, so the Android SDK/NDK cannot be installed there. Steps 6–9 run on the
  user's Mac (Android Studio for Android, Xcode for iOS). Steps 2–5 need no phone.
- `local_ci.sh` needs Mesa lavapipe (`mesa-vulkan-drivers`) for the `sdlgpu` GPU tests; `setup_env.sh` installs it
  from step 2 on.
- SDL is built with `SDL_GPU OFF` / `SDL_VULKAN OFF` today; step 2 turns them on only when `MCE_RHI_SDLGPU=ON`, so
  desktop builds that don't opt in are unchanged.

## Rules (inherited from M-MAC)

- GL stays the reference backend; new RHI features land on GL first and get an `sdlgpu` catch-up here.
- Determinism baselines are per platform (ADR-0007); phone hashes are never compared with desktop hashes.
- No Must FR rides this track; it gates no tier.
