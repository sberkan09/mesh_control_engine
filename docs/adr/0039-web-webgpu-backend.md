# ADR-0039 — Web graphics through WebGPU (`mce_rhi_webgpu`)

**Status:** Accepted (2026-10-15). **Milestone:** M-WEB (step 1, SBA-62). **Plan:** `docs/milestones/M-WEB.md`.

**Context.** M-WEB ships the player to browsers through Emscripten. The ROADMAP left the graphics path open: a
WebGL2 (GLES 3.0) profile of `mce_rhi_gl`, or WebGPU, through SDL GPU's WebGPU driver if usable, else a thin
`mce_rhi_webgpu` over Dawn's emdawnwebgpu bindings. The renderer today depends on std430 storage buffers
(clusters, light pool and indices, local shadows, decals, joint palettes) and on compute (M-VISUAL 1).

**Options weighed.**

| Option | For | Against |
|---|---|---|
| WebGL2 profile of `mce_rhi_gl` | Every browser since 2021; no new dependency; GLSL ES is close to our GLSL | No storage buffers and no compute: clustered lights, GPU skinning palettes, decals and every compute pass would need a second path, i.e. the renderer's Low tier only; no MDI, no DSA |
| SDL GPU (ADR-0038) on WebGPU | One backend for phones and web | SDL `release-3.4.18` has no WebGPU driver (d3d12, metal, vulkan only); not on SDL's roadmap for 3.4 |
| **`mce_rhi_webgpu` over emdawnwebgpu** | Storage buffers, compute, storage textures, indirect draws: the same feature set as SDL GPU, so the renderer runs its normal tiers; WebGPU is the browsers' modern API | A new backend; GLSL → WGSL needs a translator in the page; Firefox on Linux/Android and older Safari still lack WebGPU |

The spike confirmed WebGPU end to end in the cloud: headless Chromium (Playwright) exposes a WebGPU adapter on
SwiftShader (`--enable-unsafe-webgpu --use-webgpu-adapter=swiftshader`, features including
`indirect-first-instance`, `float32-filterable`, BC/ETC2/ASTC and `timestamp-query`), and `01_triangle` draws there
through the new backend (`scripts/web/browser_smoke.cjs`).

## Decisions

1. **WebGPU, one backend.** `mce_rhi_webgpu` implements `RenderDevice` over the WebGPU C API from Emscripten's
   `emdawnwebgpu` port (`--use-port=emdawnwebgpu`, Dawn release v20251002.162335 as pinned by emsdk). A web build
   has no other backend: `MCE_RHI_BACKEND` is `mce_rhi_webgpu`, `rhi::create_default_device` creates it, and
   `platform::gpu_api()` reports `GpuApi::WebGpu`. The device renders into an offscreen RGBA8 backbuffer and
   `present()` blits it to the `#canvas` surface, as ADR-0038 does with SDL's swapchain.
2. **Toolchain.** Emscripten 5.0.6 (the version Dawn's DEPS pins; `EMSDK` in the environment), presets
   `web-debug` and `web-release` (`cmake/MceWeb.cmake` sets `MCE_WEB`). Executables link to `.html` + `.js` +
   `.wasm` with ASYNCIFY, growable memory and the default shell page until `mce ship --target web` brings its own
   (M-WEB step 5). Packs come from the host's cook (`MCE_HOST_COOK`) as on phones.
3. **Shaders: GLSL stays the single source.** At pipeline creation the backend compiles each stage with the
   engine's glslang (Vulkan rules) to SPIR-V, rewrites its bindings into the fixed layout below and hands it to
   Tint (Dawn's compiler: SPIR-V reader + WGSL writer only, `cmake/web/TintDependencies.cmake`), whose WGSL goes to
   `wgpuDeviceCreateShaderModule`. GL meaning is kept: the vertex epilogue maps clip z to [0, w] and negates y so
   render targets hold GL's layout (front faces swap to match), `gl_VertexID`/`gl_InstanceID` become
   `gl_VertexIndex`/`gl_InstanceIndex` (engine draws use base instance 0). This is the "matching target" of the
   shader cook for this backend: translation lives in the backend, as on Metal (ADR-0028 §2) and SDL GPU
   (ADR-0038 §3), so every existing shader works at once; it moves into the cook (WGSL beside the SPIR-V that
   ADR-0033 already stores) when the renderer's shaders become cooked assets, and Tint then leaves the page.
4. **Binding map.** One layout for every pipeline, so a GL binding number is the WebGPU binding number:
   `@group(0)` uniform banks 0..3 (dynamic offsets into one per-frame ring), `@group(1)` texture s at `@binding(s)`
   and its sampler at `@binding(16 + s)` (Tint splits the combined sampler), `@group(2)` storage buffers 0..15,
   `@group(3)` storage images (compute only). Bind group layouts are built per pipeline from what the stages
   declare; bind groups are rebuilt only when a bound resource changes.
5. **Frames and uploads.** WebGPU orders `queue.writeBuffer` before later submits, so a submit stages every inline
   payload (vertex, index, storage, draw args, uniforms) into three rings with one write per ring, then encodes the
   passes; frame fences are always signalled. Readback (`read_pixels`, `read_buffer`) maps a staging buffer and
   waits for it.
6. **The loop yields.** `app::run` stays a plain loop; under Emscripten it awaits `requestAnimationFrame` after each
   present (ASYNCIFY), and the device waits for adapter, device and buffer maps with `wgpuInstanceWaitAny`
   (TimedWaitAny). No callback-style main loop is needed in game code.
7. **Gaps reported, not faked.** `DeviceCaps::compute = true`, so the renderer keeps its normal tiers and never
   needs the Low-tier fallback the ROADMAP reserved for a compute-less path. `timestamp_queries = false`
   (`timestamp-query` is optional on the web), `shader_draw_parameters = false`, MSAA is 4x only (WebGPU's one
   multisample count besides 1).
8. **Walls.** WebGPU types and calls live in `src/engine/rhi_webgpu/src/` only (WebGPU wall), Tint in
   `shader_wgsl.cpp` only (Tint wall), and glslang there joins the shader-cross wall (`check_walls.py`).

## Consequences

- The page carries Tint and glslang: `01_triangle.wasm` is 8.9 MB raw, 2.5 MB gzipped in `web-release`. Cooked
  WGSL (§3) and a narrower ASYNCIFY list are the size levers, tracked in `docs/milestones/M-WEB.md`.
- A cold web configure fetches Dawn's sources (no submodules), SPIRV-Headers, SPIRV-Tools and Abseil; only the
  Tint libraries are built.
- Browser floor: Chrome/Edge 113+ desktop and Chrome on Android 12+, Safari 26 (macOS, iOS), Firefox 141+ on
  Windows and 145+ on Apple silicon macOS. Firefox on Linux and Android is not supported until it ships WebGPU.
  If a target audience needs those browsers, a WebGL2 Low-tier profile of `mce_rhi_gl` is the re-entry point and
  this ADR gains an amendment.
- New RHI features land on GL first and need a catch-up in `mce_rhi_webgpu`, like `mce_rhi_sdlgpu`.
