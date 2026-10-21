# M6 — Shader cook v0.5 (milestone record)

**Scope:** the "shader cook v0.5: variant enumeration + stored SPIR-V reflection blob" item of ROADMAP M6, plus the
M2 validation slice it builds on (never built at M2). Design decisions: ADR-0033. **Measured on:** the Linux CI
container (4 cores), `scripts/ci/local_ci.sh`.

| ROADMAP / FR item | Where | Evidence |
|---|---|---|
| Validation per stage at cook, errors at the source line (FR-AST-060) | `shader_importer.cpp` (glslang, OpenGL client semantics) | `test_cook` "shader cook: compile errors…": the error names `bad.frag.glsl:6`, the identifier and the failing variant only |
| Binding-bank conformance | the bank table check in `shader_importer.cpp` | "…the binding-bank table is enforced": a uniform block at 5, a sampler at 2 and an unbound sampler are each a named cook error |
| Variant enumeration | `// @variant NAME` defines, every combination (≤ 6 defines) | 2 defines → 4 variants, each with different SPIR-V; duplicate and 7th defines rejected |
| Stored SPIR-V + reflection blob | `CHNK_SHD` = `ShaderHeader` / `ShaderVariant` / `ShaderResource` / source / SPIR-V (`formats.h`, `ShaderView`) | per variant: SPIR-V magic, `PassU` at binding 1 (16 bytes), `u_atlas` at 6, `v_uv` / `o_color` at location 0 |
| GL compiles what the cook validated | `ShaderView::compose(mask)` | `test_shader_gl` (GPU): all four variants of a cooked fragment shader link on the driver and draw the colors their defines select, no GL errors |

Not built: the ≤ 1 s shader save-to-pixels loop and moving the renderer's built-in shaders to cooked assets
(ADR-0033 §Consequences).
