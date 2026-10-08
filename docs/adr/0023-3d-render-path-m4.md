# ADR-0023 — The M4 MVP 3D render path as built

**Status:** Accepted (2026-10-08).

**Context.** `rhi-renderer.md` §3.5–§3.7 commit the MVP 3D frame: one shadow map, a forward pass with a
Lambert + Blinn-Phong sun, exp² fog, an R11G11B10F HDR target with 4× MSAA resolved by a blit, and an ACES
tonemap to the backbuffer. Building it against the CHNK_MESH format of ADR-0022 settled a few points the sketch
leaves open.

**Decisions.**

1. **`MeshStore` mirrors `TextureStore`.** Keyed by asset-system key + generation (newest wins, `retire` drops a
   generation), it owns the GPU buffers and a CPU copy of the node hierarchy, rest pose and clips. With no device
   (`init(nullptr)`) it keeps only the CPU side, which is what headless runs, culling tests and the determinism
   ladder use.
2. **Quantized streams are decoded in the vertex shader.** Each submesh carries a dequantization matrix
   (center/extent from the cook) folded into its model matrix, so positions stay `snorm16x4`, normals
   octahedral `snorm16x2` and UVs `unorm16x2` all the way to the GPU (16 B/vertex). The UV rect rides in `ObjectU`.
3. **One draw per visible submesh with a per-draw `ObjectU` (bank 3), not an `ObjectData` SSBO + MDI.** The
   diorama's draw count (a few hundred) is far inside the per-draw budget on every MVP driver; MDI over a
   persistent object buffer is a pure-performance change that arrives with Forward+ (v0.5) and does not change
   the Scene3D interface.
4. **Node animation is CPU-side.** `pose_nodes` samples a clip (binary search, lerp/slerp/step, looping) into
   per-node world matrices that the instance passes as `MeshInstance::pose`. No skinning at MVP (ADR-0022 §scope).
5. **Sort and cull.** `Scene3D::build` culls each submesh's world AABB (Arvo transform) as a sphere against the six
   Gribb–Hartmann planes (FR-R3D-050), then radix-sorts 64-bit keys: opaque by material then front-to-back depth,
   blended in the transparent pass back-to-front. Equal keys keep submission order, so draw order is a pure
   function of scene + camera (FR-RUN-040). Shadow draws include off-screen casters and skip blended submeshes.
6. **Shadow fit.** A single D32 map (2D, not yet the v0.5 array) with an orthographic box around the bounding
   sphere of the visible-scene bounds, texel-snapped so a moving camera does not shimmer. PCF is 2×2 hardware
   compare (sampler `compare_op`), with a normal offset of 1.5 shadow texels plus a small constant bias. PCF 3×3
   arrives with cascades.
7. **The RHI grew only what the frame needs:** `TextureDesc::samples`, `SamplerDesc::compare`, snorm/unorm16
   vertex formats, `CommandBuffer::resolve` (a blit outside any pass) and a fourth uniform bank. With
   `msaa = 1` the HDR target is rendered directly and the resolve is skipped.
8. **Tonemap.** Narkowicz's ACES fit with its 0.6 input pre-scale, then an sRGB encode in the shader (the
   targets are plain RGBA8, as in ADR-0020 §4). Exposure 1 maps albedo-range lighting to mid tones.

**Consequences.** The 2D and 3D paths share the texture store, the sort-key radix sort and the uniform ring; the
3D path is opt-in (`Renderer::init_3d`) so 2D games pay nothing. The v0.5 ladder (PBR, Forward+, cascades, MDI)
replaces the shading and submission inside `Forward3D` without touching `Scene3D`'s callers.
