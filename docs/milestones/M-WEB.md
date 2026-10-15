# M-WEB — Web (WASM) export (milestone record)

**Status:** active (started 2026-10-15). **Decision record:** ADR-0039 (WebGPU through `mce_rhi_webgpu`).
**Plan:** `docs/ROADMAP.md`, "M-WEB". **Linear:** project mesh_control_engine, milestone M-WEB, SBA-62…SBA-68.

## Plan

Steps run in order; each lands as its own PR verified by `scripts/ci/local_ci.sh --task` (cloud) and a headless
Chromium run on WebGPU/SwiftShader (`scripts/web/browser_smoke.cjs`).

| # | Step | Linear | State |
|---|---|---|---|
| 1 | Graphics decision (ADR-0039): `mce_rhi_webgpu` over emdawnwebgpu, GLSL → WGSL with glslang + Tint, `web-*` presets; `01_triangle` draws in a browser | SBA-62 | done |
| 2 | Emscripten toolchain for every sample and game: the player's main loop, the job system single-threaded (or pthreads under COOP/COEP), every engine library compiling | SBA-63 | done |
| 3 | Files and assets: the pack over HTTP with progress, saves in IndexedDB | SBA-64 | not started |
| 4 | Audio and input: WebAudio unlock, touch, gamepad, pointer lock, fullscreen | SBA-65 | not started |
| 5 | `mce ship --target web`: shell page, itch.io zip, `mce serve` | SBA-66 | not started |
| 6 | MCP `ship_build {"target":"web"}` and a Playwright smoke returning the state hash and a screenshot | SBA-67 | not started |
| 7 | A `web` stage in `local_ci.sh` | SBA-68 | not started |

**Demo:** Ember Step, a novel and `04_diorama` in Chrome, Firefox and a mobile browser.

## What shipped (step 1)

| Item | Where | Evidence |
|---|---|---|
| Backend | `src/engine/rhi_webgpu` (`webgpu_device.cpp`, `webgpu_translate.cpp`) | every `RenderDevice` call; uniform/geometry/storage rings with one `queue.writeBuffer` each per submit; offscreen backbuffer blitted to `#canvas` |
| Shader translator | `src/engine/rhi_webgpu/src/shader_wgsl.cpp` | glslang → SPIR-V (bindings rewritten to ADR-0039 §4) → Tint → WGSL |
| Build | `cmake/MceWeb.cmake`, `cmake/web/TintDependencies.cmake`, presets `web-debug` / `web-release` | `cmake --preset web-release && cmake --build build/web-release --target 01_triangle` |
| Loop | `core/src/app.cpp` awaits `requestAnimationFrame` per frame (ASYNCIFY) | the page stays responsive; the triangle spins |
| Smoke | `scripts/web/browser_smoke.cjs` | `01_triangle` on Chromium 141 + SwiftShader: "rhi.webgpu: WebGPU on google swiftshader", 413 colours on the canvas, PASS |

Numbers (`web-release`, MinSizeRel): `01_triangle.wasm` 8.9 MB, 2.5 MB gzipped; Tint + SPIRV-Tools alone are about
0.9 MB of that gzipped.

## What shipped (step 2)

| Item | Where | Evidence |
|---|---|---|
| Whole tree on wasm | `cmake --build build/web-release` (with `-DMCE_HOST_COOK=<host mce_cook>`): every engine library, `mce_player`, and the samples `01_triangle`, `sprites_2d`, `03_platformer`, `04_diorama`, `04_cubes`, `05_ashen_court` | build log, no errors |
| No threads | `asset_system.cpp` reads queued async loads inline; `audio_output.cpp` opens the device inline (`__EMSCRIPTEN__` without `__EMSCRIPTEN_PTHREADS__`) | the samples load and play |
| Jobs | the job system already runs jobs inline everywhere (`jobs.h`, until its v0.5 runtime flag), so the browser is single-threaded with no change; a pthreads build under COOP/COEP comes with that flag, not before | — |
| Main loop | `app::run` + ASYNCIFY (step 1); no `emscripten_set_main_loop` callback rewrite | samples run their normal loop |
| Cook | `mce_asset_cook` is skipped in cross builds (it never reaches a runtime target) | — |
| Packs | interim: `mce_sample_pack` embeds the cooked pack with `--preload-file` at its dev path; step 3 streams it over HTTP | `.data` next to each `.wasm` |
| Backend fixes | WGSL derivative uniformity relaxed like GL (`allow_non_uniform_derivatives`); write-only storage buffers become `read_write`; one transient storage ring per binding (WebGPU forbids one buffer read-write and read-only in one dispatch); WebAudio power-of-two buffers | — |
| Build | links run one at a time (`mce_web_link` Ninja pool): wasm-opt's ASYNCIFY pass takes about 6 GB per executable | four parallel links were OOM-killed |
| Smoke | `scripts/web/browser_smoke.cjs` on all six samples in headless Chromium (WebGPU/SwiftShader) | all PASS; `04_diorama` and `05_ashen_court` draw with shadows, clustered lights and GPU culling |

## Open items

- Size: cook WGSL next to SPIR-V once renderer shaders are cooked assets (drops Tint from the page), narrow
  ASYNCIFY to the frame yield and the device waits.
- Timestamp queries are off (`DeviceCaps::timestamp_queries = false`); the profiler shows CPU times on the web.
- Pixel parity with GL: checked by eye (orientation, winding) on `01_triangle`; a golden comparison against the GL
  backend comes with the renderer samples in step 2.
