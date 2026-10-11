# ADR-0038 — Mobile graphics through one SDL3 GPU backend (`mce_rhi_sdlgpu`)

**Status:** Accepted (2026-10-11). **Milestone:** M-MOBILE (un-parked 2026-10-11 by the user).
**Requirements:** FR-SHP-062, FR-INP-070 (REQUIREMENTS slip note 2026-10-11). **Plan:** `docs/milestones/M-MOBILE.md`.

**Context.** The player has to run on Android and iOS. `mce_rhi_gl` targets desktop GL 4.5 (DSA, persistent
mapping, MDI), none of which exists on Android's GLES or on iOS; `mce_rhi_metal` (ADR-0028) is macOS-only and
attaches to an `SDL_WINDOW_METAL` window. The ROADMAP graphics-layer note asked for one cross-platform layer to be
evaluated before writing two mobile backends: SDL3 GPU first, WebGPU via Dawn second, bgfx / MoltenVK / ANGLE as
fallbacks. The `mce_rhi` interface is small (one `RenderDevice`, a recorded `CommandBuffer`, four uniform banks,
samplers 6..15, storage 0..7) and every pipeline is GLSL 450 that the shader cook (ADR-0033) already compiles to
SPIR-V with glslang.

**Options weighed.**

| Option | For | Against |
|---|---|---|
| **SDL3 GPU** | Already in the tree (SDL `release-3.4.18`, today built with `SDL_GPU OFF`); Vulkan on Android, Metal on iOS, also Vulkan/D3D12/Metal on desktop; no new dependency; window, surface and swapchain come from the SDL window we already own; testable in the cloud on Linux through Mesa lavapipe (software Vulkan) | No timestamp queries; storage buffers bind without offsets; first-instance/first-vertex not visible to shader built-ins; shaders must arrive per backend (SPIR-V for Vulkan, MSL for Metal) |
| WebGPU (Dawn) | Rich, well specified; one shader language (WGSL via Tint) | Dawn is a very large build (its own toolchain conventions, tens of minutes cold); a second windowing/surface path beside SDL; GLSL → WGSL needs another translator; Android/iOS builds of Dawn are the least travelled |
| Two native backends (GLES 3.x or Vulkan for Android + Metal on iOS) | Each tuned to its platform | Two new backends to write and keep at parity; a hand-written Vulkan backend is N14 (deferred until both anchor games ship); a GLES backend cannot do MDI or base-instance |
| bgfx / MoltenVK / ANGLE | Mature | bgfx replaces the RHI rather than sitting behind it; MoltenVK needs a Vulkan backend first; ANGLE gives GLES, which still lacks the GL 4.5 features `mce_rhi_gl` uses |

## Decisions

1. **One new backend, `mce_rhi_sdlgpu` (`src/engine/rhi_sdlgpu`), behind the existing `mce_rhi` seam.** It is the
   mobile backend on Android (Vulkan) and iOS (Metal). GL stays the reference backend and the desktop default; Metal
   stays the macOS default. On desktop the new backend is opt-in (`-rhi sdlgpu` on the player, `MCE_RHI_SDLGPU=ON` at
   configure), which is how it is developed and tested before any phone is involved. This is not the N14 hand-written
   Vulkan/D3D12 backend: SDL owns the API code; N14 stays as written.
2. **Developed and verified on Linux first.** Configure with `SDL_GPU=ON SDL_VULKAN=ON` only when
   `MCE_RHI_SDLGPU=ON`; `local_ci.sh` gains Mesa lavapipe and runs the GPU tests a second time on `sdlgpu`
   (`MCE_TEST_RHI=sdlgpu`), so every RHI feature is checked on Vulkan in the cloud. Phones and simulators are the last
   step, on the user's Mac.
3. **Shaders: GLSL stays the single source.** glslang compiles each stage to SPIR-V with Vulkan semantics, the
   backend rewrites the GL binding numbers into SDL's fixed resource layout (below) by patching the
   `DescriptorSet`/`Binding` decorations in the SPIR-V, and on Metal SPIRV-Cross emits MSL with SDL's index order.
   As on macOS (ADR-0028 §2), translation starts inside the backend at pipeline creation so every existing shader
   works at once; it moves into the shader cook (ADR-0033 already stores SPIR-V) when the renderer's shaders become
   cooked assets, and the runtime translator is then dropped from mobile builds.
4. **Binding map.** SDL's layout fits the bank table: the four uniform banks (Frame, Pass, Material, Object) are
   SDL's four uniform slots per stage (`SDL_PushGPU{Vertex,Fragment}UniformData`, set 1 / set 3), pushed to both
   stages; samplers 6..15 and storage 0..7 are compacted per pipeline into SDL's sampler and storage-buffer ranges
   (set 0 / set 2), and the pipeline keeps the GL-binding → SDL-index table. Bindings persist across passes like GL's
   and are re-applied per pipeline.
5. **Uploads happen before passes.** SDL forbids copies inside a render pass, but a `CommandBuffer` carries all its
   payloads inline, so `submit()` makes two walks: a copy pass that writes every transient payload (vertex, index,
   storage, draw args) into per-slot ring buffers through one transfer buffer, then the render passes. Transient
   storage data gets its own buffer per binding per slot, which sidesteps SDL's offset-less storage binding (no
   engine code calls `bind_storage_buffer` with an offset today). `update_buffer`/`update_texture` queue into the next
   submit's copy pass, which keeps GL's "visible to later draws" order.
6. **Draw parameters.** `draw_indirect` maps to `SDL_DrawGPUIndexedPrimitivesIndirect` with the same 20-byte command
   layout. Engine shaders use `gl_InstanceID` and `gl_VertexID` but never `gl_BaseInstanceARB`; the translator keeps
   GL meaning (instance id without the base instance, as ADR-0028 §4 does for Metal), which on Vulkan needs the
   `shaderDrawParameters` feature; the spike checks lavapipe and the target GPUs expose it. Clip-space z and the
   render-target Y flip follow ADR-0028 §4 so `read_pixels` and goldens match GL.
7. **Gaps reported, not faked.** `DeviceCaps::timestamp_queries = false` (the profiler shows CPU times only);
   wireframe needs `fillModeNonSolid` and is skipped where missing; formats are checked with
   `SDL_GPUTextureSupportsFormat` (D24S8 falls back to D32 on Apple GPUs). BC textures are not sampled on phones:
   the cook gains a mobile texture profile (ASTC, uncompressed RGBA8 until it lands).
8. **Platform floor.** Android 10 (API 29), arm64-v8a, Vulkan 1.1; iOS 16, arm64, plus the arm64 simulator. No GLES
   fallback: devices without Vulkan are not supported.
9. **The window path.** `platform::WindowDesc` gets a `Surface { GL, Metal, GPU }` choice in place of the `opengl`
   flag; the `GPU` path creates a plain SDL window and the device claims it with `SDL_ClaimWindowForGPUDevice`.
   `rhi::create_default_device` keeps choosing GL/Metal on desktop and chooses `sdlgpu` on Android and iOS.

**Consequences.** One backend serves both phones and is exercised on every Linux CI run, so mobile rendering does
not rot between Mac sessions. Desktop users also get Vulkan (Linux, Windows) and D3D12 (Windows) through it for free,
but those stay unsupported until a game needs them. New RHI features still land on GL first and need a catch-up in
`mce_rhi_sdlgpu`, tracked in `docs/milestones/M-MOBILE.md`. If the SDL GPU spike (M-MOBILE step 2) shows a blocker
that the binding map or the copy-pass split cannot absorb, this ADR is superseded and WebGPU (Dawn) is evaluated next.
