# ADR-0033: Shader cook (validation, variants, SPIR-V reflection)

**Status:** Accepted (2026-10-09). **Milestone:** M6 (shader cook v0.5). **Requirements:** FR-AST-060.
**Design:** `docs/architecture/assets.md` §5.2 (Shader row), `docs/architecture/rhi-renderer.md` §3.3 (binding banks).

**Context.** assets.md planned the shader cook in two steps: an MVP slice at M2 (validate each `.glsl` stage, check
the binding-bank table, emit the source) and v0.5 (enumerate variants, store a SPIR-V reflection blob). The M2
slice was never built; the renderer's shaders are GLSL strings in `renderer.cpp`. This lands both steps at once, as
an asset type the cook produces and the runtime can read. Moving the engine's own shaders out of `renderer.cpp`
into cooked assets is separate work (it changes how the renderer boots).

## Decisions

1. **glslang as a library, cook-side only.** The cook links glslang (`cmake/Dependencies.cmake`, same tag and
   options as the Metal backend's copy, declared first so macOS builds one glslang) instead of shelling out to
   `glslangValidator`: no tool on PATH, the same parser everywhere, errors as data. Only
   `shader_importer.cpp` includes it (`check_walls.py` shader-cross wall); runtime targets never link it.
2. **One stage per file, stage from the name.** `name.vert.glsl`, `name.frag.glsl`, `name.comp.glsl` with an
   `.mceasset` sidecar (importer `shader`) cook to one `CHNK_SHD` each. A program is two assets; the material or
   renderer that uses them names both.
3. **Variants are boolean defines declared in the source.** `// @variant NAME` lines (at most 6, so at most 64
   variants) declare defines that are 0 or 1; the cook compiles every combination. The defines are passed as a
   glslang preamble, so error lines are the authored lines, and every diagnostic names the variant
   (`[variant RED=1 HALF=0]`). At run time `ShaderView::compose(mask)` inserts the same `#define` lines after
   `#version`, which is exactly the text glslang validated, for `glShaderSource`.
4. **Binding banks are checked at cook.** Every uniform block, storage block and sampler needs an explicit
   `layout(binding = N)`; uniform blocks must use 0..3 (Frame, Pass, Material, Object), samplers 6..15, storage
   blocks 0..7 (`rhi::bank`). A violation is a cook error naming the resource and the binding.
5. **The chunk stores source, SPIR-V and reflection.** `ShaderHeader` (stage, defines, counts) |
   `ShaderVariant[]` (mask, SPIR-V range, resource range) | `ShaderResource[]` (name, kind, binding or location,
   block size) | source text | SPIR-V words. SPIR-V is compiled with OpenGL client semantics, the same way the
   Metal backend compiles at pipeline creation, so a backend that translates from SPIR-V can take the cooked words
   instead of running glslang at run time. GL program binaries are still never cooked (driver-specific,
   assets.md §5.2).

## Consequences

- Every build fetches and compiles glslang once (ccache keeps it cheap afterwards).
- Not built here: the ≤ 1 s save-to-pixels hot-reload loop for shaders (needs the renderer to consume cooked
  shaders), and moving the built-in shaders to assets.
