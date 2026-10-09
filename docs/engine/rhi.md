# rhi — the backend-neutral render hardware interface and its GL, default and Metal backends

The RHI is four L2 libraries (layer per the CMake comments):

- `mce_rhi` (`src/engine/rhi/`): the interface. Handles, descriptors, the recorded `CommandBuffer`
  and the abstract `RenderDevice`. It has no GL and no SDL. Its only public dependency is
  `mce_core`. It is linked by `mce_render`, `mce_editor_core`, `mce_editor_layer`, the three backend
  libraries below and the `mce_engine` umbrella (`src/module/CMakeLists.txt`).
- `mce_rhi_gl` (`src/engine/rhi_gl/`): the OpenGL 4.5 core backend and the only GL code in the
  engine. Public dependencies `mce_rhi` and `mce_platform`; `mce_glad` and `SDL3::SDL3-static` are
  private, so GL symbols never leak past the target.
- `mce_rhi_default` (`src/engine/rhi_default/`): `rhi::create_default_device`, the one place that
  picks the native backend. Public dependencies `mce_rhi` and `mce_platform`; the backend
  (`mce_rhi_metal` on Apple, `mce_rhi_gl` elsewhere) is a private dependency named by
  `MCE_RHI_BACKEND`, set in the root `CMakeLists.txt`. Hosts link this target instead of a backend:
  the `mce_editor` executable, the `mce_engine` umbrella, `mce_player_runtime` (directly only where
  game modules are unsupported; otherwise through the umbrella), the samples (`01_triangle`, `sprites_2d`,
  `03_platformer`, `04_diorama`, `04_cubes`, `05_ashen_court`, `ui_gallery`), project editors made
  by `cmake/MceProject.cmake`, and the GPU tests `test_render_gl` and `test_shader_gl`.
- `mce_rhi_metal` (`src/engine/rhi_metal/`): the Metal backend for macOS. It is owned by the macOS
  track (see `docs/milestones/M-MAC.md` and ADR-0028); only its header is summarized below.

## Public surface

`include/mce/rhi/rhi.h` is an umbrella that includes the three headers below.

`include/mce/rhi/types.h`: vocabulary.

- `BufferHandle`, `TextureHandle`, `SamplerHandle`, `PipelineHandle`: core generational handles.
  A null texture handle used as a pass attachment means the window backbuffer (color) or the
  default depth buffer (depth).
- `TextureFormat` (RGBA8, sRGB, RGBA16F/32F, R11G11B10F, BC1/3/5/7, depth formats) with
  `is_depth_format`, `is_compressed_format`, `bytes_per_pixel`; `VertexFormat` (including the
  quantized SNorm16/UNorm16 mesh streams) with `vertex_format_size`.
- `BufferDesc`, `TextureDesc` (2D or `Tex2DArray`, mips, layers, `samples` for MSAA targets),
  `SamplerDesc` (including depth-compare sampling), `TextureRegion` for partial uploads.
- `ShaderSource` (GLSL 450 core text), `VertexLayout`, `FixedState` (depth, blend mode, raster,
  topology) and `GraphicsPipelineDesc`.
- `PassAttachment` (texture, array `layer`, load/store ops, clear value) and `RenderPassDesc`.
- `DrawIndexedIndirectCommand`: the 20-byte MDI argument record.
- `namespace bank`: the fixed binding numbers (UBO 0 FrameU, 1 PassU, 2 MaterialU, 3 ObjectU;
  samplers from 6, e.g. `kAlbedo` 6, `kNormal` 7, `kOrm` 8, `kShadowMap` 9, `kSky` 10, `kFont` 11,
  `kEmissive` 13; std430 storage `kClusters` 4, `kLightPool` 5, `kLightIndices` 6).
- `kUniformAlignment` (256) and `kMaxTimestamps` (32 GPU timestamp queries per frame slot).

`include/mce/rhi/command_buffer.h`: `CommandBuffer`, a POD byte stream of commands (`CmdType`,
`CmdHeader`, payload structs in `namespace cmd`). Recording touches no graphics API.

- Pass and state: `begin_pass`, `end_pass`, `bind_pipeline`, `bind_texture`,
  `bind_storage_buffer`, `push_debug_scope` / `pop_debug_scope`.
- Transient data copied into the device's per-slot rings at submit: `set_uniforms(bank, bytes)`,
  `set_vertex_data`, `set_index_data`, `set_storage_data`, `set_draw_args`.
- Persistent geometry: `bind_vertex_buffer`, `bind_index_buffer`.
- Draws: `draw`, `draw_non_indexed`, and `draw_indirect` (one multi-draw-indirect over the
  `set_draw_args` records).
- `resolve(src, dst)`: MSAA color target into a single-sample one, outside a pass.
- `write_timestamp(index)`: GPU timestamp, read back `MCE_FRAMES_IN_FLIGHT` frames later.
- `barrier()`: a no-op today.
- `Reader` and `inline_data<P>()` let a backend walk the stream.

`include/mce/rhi/render_device.h`: `RenderDevice`, which also implements core's
`app::IFrameFenceSource`.

- `DeviceDesc`: debug layer, vsync, per-slot ring sizes (geometry 4 MiB, uniform 1 MiB,
  indirect 256 KiB).
- Resources: `create_buffer`, `update_buffer`, `create_texture`, `update_texture`,
  `create_sampler`, `create_pipeline` (returns `Result`, so a shader compile error is not a crash),
  the matching `destroy_*`, `is_alive`, `texture_size`, `texture_desc`.
- Frames: `begin_frame(frame_index)`, `submit(cmd)`, `end_frame()`, `present(vsync)`,
  `backbuffer_size()`.
- Diagnostics: `read_pixels` (RGBA8 readback, top row first; stalls), `caps()` (`DeviceCaps`),
  `last_frame_stats()` (`FrameStats`, including `ring_overflows`), `last_gpu_timestamps()`
  (`GpuTimestamps::ms(from, to)`), `texture_memory_bytes()`, `validation_error_count()`.

`include/mce/rhi/default_device.h` (in `mce_rhi_default`): `rhi::create_default_device(window, desc)`.

`include/mce/rhi_gl/gl_device.h`: `rhi::gl::create_device(window, desc)`. The device creates and
owns the GL context from `Window::native_handle()`. Internals are `src/gl_device.cpp` (context,
resources, rings, frames) and `src/gl_translate.cpp` (command stream to GL calls).

`include/mce/rhi_metal/metal_device.h`: `rhi::metal::create_device(window, desc)`. Per the header,
it attaches a `CAMetalLayer` to the window through SDL's Metal view, needs a Metal-capable window
(`WindowDesc::opengl` selects `SDL_WINDOW_METAL` on macOS), and translates the GLSL 450 pipeline
sources to MSL when a pipeline is created.

## Rules and invariants

- **Threading.** Every device call happens on the thread that created the device (the main
  thread). Recording a `CommandBuffer` touches no API, so recording on another thread would need
  no interface change, but nothing does that today.
- **Frame protocol.** `begin_frame` waits on the slot's fence from `MCE_FRAMES_IN_FLIGHT` (3)
  frames ago, runs that slot's deferred deletes, collects its timestamps and resets the slot's ring
  heads. `submit` is only legal between `begin_frame` and `end_frame` and never with an open pass
  (both asserted). `end_frame` fences the slot. Core's frame-slot arenas are released on the same
  fences through `fence_for_frame`.
- **Allocation.** Command streams live in the allocator passed to the constructor, normally
  `mem::frame()`. Transient vertex, index, uniform, storage and indirect data is valid for the
  current frame only.
- **Ring overflow.** In the GL backend an overflow drops the affected commands, logs one warning
  per submit and counts `FrameStats::ring_overflows`; draws whose data was dropped are skipped.
  It does not crash and does not grow a buffer.
- **Resource lifetime.** Creation is immediate; `destroy_*` is deferred until no in-flight frame
  can reference the object. Handles are generational; the GL backend asserts on a stale handle in
  `update_buffer` and `bind_pipeline` and then ignores the call.
- **Fixed bindings.** Shaders must use the `rhi::bank` numbers. The shader cook checks this for
  cooked shaders (ADR-0033); built-in renderer shaders are GLSL strings and follow it by hand.
  Vertex shaders of separable programs must redeclare `out gl_PerVertex`.
- **GL context requirements.** `mce_rhi_gl` refuses drivers below OpenGL 4.5 core
  (`Errc::Unsupported`). With `debug_layer` on, the KHR_debug callback counts errors and
  high-severity messages in `validation_error_count()`.
- **Walls** (`scripts/ci/check_walls.py`):
  - GL wall: `gl[A-Z]...(` calls only under `src/engine/rhi_gl/`.
  - glad wall: `<glad/...>` only under `src/engine/rhi_gl/` (tests included).
  - SDL include wall: `<SDL3/...>` is allowed in `rhi_gl` and `rhi_metal` (they consume
    `native_handle()`), as well as platform and the editor layer.
  - Metal wall: `MTL*`, `<Metal/...>`, `<QuartzCore/...>` only under `src/engine/rhi_metal/` and
    `src/engine/platform/src/macos/`.
  - shader-cross wall: glslang and SPIRV-Cross only under `src/engine/rhi_metal/` and the cook's
    `shader_importer.cpp`.
  - The `std::vector`/`std::string`/iostream ban applies to all of `src/engine/`.

## How it is tested

- `tests/rhi/test_command_buffer.cpp` builds the ctest `test_rhi` (links only `mce_rhi`, no GPU).
  Cases: "rhi: command buffer records and reads back in order", "rhi: command buffer on the frame
  arena", "rhi: misuse asserts", "rhi: draw args and indirect draws round-trip", "rhi: resolve
  records outside a pass...", "rhi: storage data and timestamps round-trip...".
- `tests/rhi_gl/test_rhi_gl.cpp` builds `test_rhi_gl` (GPU, label `gpu`, not built on Apple).
  Cases: "rhi_gl: device caps", "rhi_gl: clear + triangle to the backbuffer, read back",
  "rhi_gl: offscreen color target + persistent buffers", "rhi_gl: shader compile errors are
  Results, not crashes", "rhi_gl: frame fences gate core frame slots".
- `tests/rhi_metal/` (macOS only, owned by the macOS track): `test_shader_cross` (no GPU) and
  `test_rhi_metal` (GPU, label `gpu`). `docs/milestones/M-MAC.md` lists what they cover.
- `test_render_gl` (see [render.md](render.md)) and `test_shader_gl` (`tests/asset/`) exercise the
  device through `create_default_device`.
- GPU tests always build but register only with `-DMCE_GPU_TESTS=ON`. `scripts/ci/local_ci.sh`
  turns it on for `linux-gcc-debug` and `linux-clang-release` and runs ctest under Xvfb with Mesa
  llvmpipe (`LIBGL_ALWAYS_SOFTWARE=1`).

## Design records

- [rhi-renderer.md](../architecture/rhi-renderer.md): §2.1 handles, §2.2 RenderDevice, §2.3
  CommandBuffer, §3.0 module layout and the seam, §3.1 frame slots and staging rings, §3.2 MDI,
  §3.3 binding banks, §4 threading and lifecycle. The file split in §3.0 (`gl_staging.cpp`,
  `gl_pipeline.cpp`, `gl_debug.cpp`) is a plan; the built backend has two files.
- [macos-metal.md](../architecture/macos-metal.md): why Metal, the seams it needed, and file
  ownership between the cloud threads and the Mac session.
- [ADR-0028](../adr/0028-metal-backend.md): Metal backend, GLSL to MSL at pipeline creation,
  fixed binding map, `create_default_device`.
- [ADR-0033](../adr/0033-shader-cook.md): shader cook and the binding-bank check.
- [ADR-0023](../adr/0023-3d-render-path-m4.md) §7: what the RHI gained for the M4 3D path
  (`TextureDesc::samples`, `SamplerDesc::compare`, SNorm/UNorm16 vertex formats, `resolve`, a
  fourth uniform bank).
- [ADR-0024](../adr/0024-pbr-forward-plus-csm-m5a.md) "RHI additions": `PassAttachment::layer`,
  `SetStorageData`, `WriteTimestamp`, `GpuTimestamps` and the bank-table additions.
- Milestones: [M-MAC.md](../milestones/M-MAC.md), [M6-shader-cook.md](../milestones/M6-shader-cook.md),
  [M5a.md](../milestones/M5a.md).

## Known gaps

- Pipelines still take GLSL source text. The `ShaderSource` comment says so, and M6-shader-cook
  records that moving the renderer's built-in shaders to cooked assets is not built.
- `barrier()` is a no-op ("a real barrier on a future explicit backend").
- M-MAC backlog: shader translation moves to the cook once shaders are cooked assets (ADR-0028
  §2); `.app` packaging, Xcode GPU captures and a deployment target below macOS 26 are open.
- Real-GPU timings and RenderDoc captures are open in M5a: CI has only llvmpipe.
