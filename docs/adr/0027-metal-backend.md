# ADR-0027 — The Metal backend (`mce_rhi_metal`) and GLSL → MSL translation at pipeline creation

**Status:** Accepted (2026-10-08). The design note `docs/architecture/macos-metal.md` reserves this ADR as
"ADR-0025"; that number (and 0026) went to other work first, so it is 0027.

**Context.** macOS caps OpenGL at 4.1 (no DSA, no persistent mapping, no MDI, no `ARB_shader_draw_parameters`), so
`mce_rhi_gl` cannot run there. REQUIREMENTS §8 Q2 and ROADMAP M-MAC call for a second backend behind the `mce_rhi`
seam, built on the user's Mac in parallel with the cloud threads. Every shader in the engine today is GLSL 4.50
source handed to `create_pipeline` as `rhi::ShaderSource::glsl` — string constants inside `mce_render`, the editor
layer, samples and tests. There is no shader asset, cook step or `ShaderKey` yet (the comment on `ShaderSource`
says so).

**Decisions.**

1. **Metal, not MoltenVK.** Native on Apple silicon; no Vulkan backend to write first (Vulkan stays N14). The
   recorded `CommandBuffer`, pipelines-as-objects and frame slots map onto command buffers and encoders directly.
2. **GLSL stays the single shader source, translated when a pipeline is created.** `create_pipeline` runs
   glslang (GLSL → SPIR-V with **OpenGL client semantics**, so `gl_VertexID`, `gl_InstanceID`,
   `gl_BaseInstanceARB` and separable `gl_PerVertex` mean what they mean on GL) and SPIRV-Cross (SPIR-V → MSL 2.3),
   then `newLibraryWithSource`. The design note's default was translation at cook time with no translator in a
   runtime target; that needs shaders to be cooked assets, which they are not, and making them so is a renderer
   change owned by the cloud threads. Doing it inside the backend needs no interface change and puts every existing
   shader on Metal at once. The translator is confined to `mce_rhi_metal` (private link, a "shader-cross wall" in
   `check_walls.py`) and to macOS builds; GL builds never fetch it. **When cooked shaders land, translation moves
   into the cook and the runtime path is deleted** (tracked in `docs/milestones/M-MAC.md`).
3. **A fixed binding map instead of reflection.** GL keeps uniform blocks, storage blocks and samplers in separate
   binding namespaces; the translator gives each its own descriptor set and pins them to Metal indices:
   uniform bank *b* → `[[buffer(b)]]`, storage binding *b* → `[[buffer(8 + b)]]`, sampler binding *s* →
   `[[texture(s)]]` + `[[sampler(s)]]`, vertex attributes from `[[buffer(30)]]`. The device binds by bank number with
   no per-pipeline tables.
4. **GL conventions are kept, not leaked.** Clip-space z is fixed up to Metal's [0, 1]. Texture targets are drawn
   Y-flipped (a vertex-stage function constant, so one library serves both cases) with the winding mirrored, so a
   render target's memory, `read_pixels` and sampling match GL exactly — row 0 is NDC y = −1 — and the renderer needs
   no backend switch. The backbuffer is drawn unflipped. `gl_InstanceID` is rewritten to
   `gl_InstanceID − gl_BaseInstanceARB` because Metal's `[[instance_id]]` includes the base instance and GL's does
   not.
5. **GL's binding-point and update semantics are emulated.** Bindings persist across passes and submits and are
   re-applied to each render encoder. `update_buffer` / `update_texture` / initial texture data go through blits on
   the frame's command buffer, ordered with the GPU work exactly like `glNamedBufferSubData` and
   `glTextureSubImage*`; textures are `Private`. Transient geometry and uniforms use per-slot shared-storage rings
   (`MCE_FRAMES_IN_FLIGHT` slots); a slot's fence is the last command buffer committed for it. Metal command buffers
   retain what they reference, so `destroy_*` releases at once instead of deferring.
6. **MDI is one direct draw per record.** `set_draw_args` records are CPU-visible in the stream; each becomes a
   `drawIndexedPrimitives` with its own `baseInstance` (stats still count one draw per `draw_indirect`, as GL does).
   Indirect command buffers are a later optimization.
7. **Hosts pick the backend through `rhi::create_default_device`** (`mce_rhi_default`): Metal on macOS, GL elsewhere.
   On macOS `WindowDesc::opengl` creates an `SDL_WINDOW_METAL` window and the device attaches a `CAMetalLayer`
   through `SDL_Metal_CreateView`.
8. **Objective-C++ under manual reference counting** in two `.mm` files, compiled through the CXX language with the
   engine compile policy (no exceptions, no RTTI). `shader_cross.cpp` alone is built with exceptions, because
   SPIRV-Cross reports errors by throwing; it converts them to `Result`.
9. **Dependencies** (`cmake/macos/MetalDependencies.cmake`, FetchContent per ADR-0017): glslang `16.6.0`
   (optimizer, HLSL and tools off) and SPIRV-Cross `vulkan-sdk-1.4.363.0` (core + GLSL + MSL only), both static.
10. **macOS deployment target 26.0** for now: `mce_asset_cook` uses floating-point `std::from_chars`, which Apple's
    libc++ provides from macOS 26. Lowering it needs a fallback in the cook's JSON reader (cloud-owned) and is an
    M-MAC backlog item. The macOS presets also pass **`-fmath-errno`**: Darwin defaults to `-fno-math-errno`, which
    lets clang fuse `sin` + `cos` of one angle into Apple's `__sincosf_stret` at -O2 only, and that call rounds
    differently from the separate ones — `physics2d_headless` drifted between Debug and Release at tick 197 until
    the flag went in (ADR-0007 requires Debug = Release on one machine).

**Consequences.** Every pipeline the engine creates today runs on Metal unchanged: `test_render_gl` (2D sprites,
MDI, text, tilemaps, the 3D forward path with MSAA, shadows and tonemap) passes on Metal with the API and shader
validation layers on, and the four windowed samples render. Pipeline creation is slower than on GL (translation +
MSL compile, a few ms per pipeline), which is visible only at startup. New RHI features land on GL first; each needs
a Metal catch-up, tracked in `docs/milestones/M-MAC.md`. Readback supports the uncompressed color formats the engine
reads today (RGBA8/sRGB, BGRA8, RGBA16F/32F, R8, RG8), not R11G11B10F or depth.
