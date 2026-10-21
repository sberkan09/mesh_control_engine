# ADR-0024 — The M5a 3D renderer: PBR, Forward+, cascaded shadows, sky and post

**Status:** Accepted (2026-10-08).

**Context.** ROADMAP M5a turns the M4 forward path (ADR-0023) into the v0.5 renderer committed by
`rhi-renderer.md` §3.5–§3.6 and FR-R3D-020/030/040/080/090: Cook-Torrance GGX with ORM materials, CPU-clustered
Forward+ lighting for at least 256 point lights, three texel-snapped shadow cascades, a sky, and a bloom → ACES →
FXAA post chain, plus multi-node glTF animation and a per-stage frame budget. The sketch leaves several choices
open; this records how they were settled. `Scene3D`'s interface stays source-compatible with M4 callers, and the
04_diorama determinism hash is unchanged.

**Decisions.**

1. **Shading model.** Cook-Torrance with GGX `D`, the height-correlated Smith visibility term and Schlick Fresnel;
   Lambert diffuse weighted by `(1 − F)`; `f0 = mix(0.04, base_color, metallic)`. Roughness is perceptual and
   squared for `α` (clamped to 0.045 to keep highlights finite).
2. **Light units: intensity is reflected radiance, not photometric.** The BRDF is multiplied by π, so a white
   Lambert surface facing a light of intensity `I` reflects exactly `I`. M4 scenes keep their look and their
   tuning (sun intensity ≈ 2, exposure 1); physically based lux/candela units can come with real exposure control
   and do not change any interface.
3. **Material maps.** Base color (sRGB), tangent-space normal, ORM (glTF metallic-roughness: G roughness, B metallic,
   R occlusion when the material's occlusion texture is the same image — `MaterialFlags::OcclusionInMR`) and
   emissive (sRGB). Each map is optional and falls back to white (or the vertex normal), so the M4 albedo-only
   material is the degenerate case. Normal mapping derives the tangent frame from screen-space derivatives
   (cotangent frame) instead of baking tangents into the 16-byte vertex: no CHNK_MESH stream change, no MikkTSpace
   at cook. glTF's convention needs the bitangent flipped, which the shader does. An occlusion texture on its own
   is cooked into the ORM slot with `OcclusionOnly`; occlusion in a third image is not supported (warning).
4. **Forward+ on the CPU.** A 32 × 16 × 24 grid of screen tiles × exponential depth slices. Per light: frustum test,
   tight clipped-sphere screen bounds (Mara & McGuire 2013 tangent planes, with the near-plane chord when the
   sphere crosses it), then an exact sphere-vs-cluster-AABB test over that tile rectangle, made separable (depth
   per slice, y per row, a monotone x run per row) with the per-slice tile extents precomputed. Count, prefix-sum
   and scatter keep lights in submission order inside each cluster, so the lists are a pure function of view and
   lights. The three std430 arrays (ranges, light pool, index list) go through the new `set_storage_data`
   (banks 4–6, from the per-frame ring like uniforms), so there is no persistent GPU state to manage. Spot lights
   are point lights with a cone term (`saturate(dot(−L, dir·scale) + offset)²`) and are clustered by their
   bounding sphere. 256 lights in 04_cubes take ~0.3–0.45 ms CPU on the CI container (release).
5. **Attenuation.** Inverse square, windowed to exactly 0 at the light's radius (`(1 − (d/r)⁴)²`, the
   UE4/Frostbite form), so clustering by radius never cuts a visible contribution.
6. **Cascades.** Each cascade bounds its view-frustum slice with the slice's minimal bounding sphere, which
   depends only on the projection and the slice depths, so its size never changes as the camera turns. Its
   radius is rounded up to 1/16 m, the light-space origin is snapped to whole texels, and the depth range is
   stretched toward the light to the scene bounds so off-screen casters still cast. Split depths come from
   `ShadowSettings` (fractions of the shadowed distance); each cascade cross-fades into the next over
   `blend` of its range. Casters are culled per cascade (sphere vs the light-space box). The map is one D32
   2D array, rendered layer by layer through `PassAttachment::layer`, sampled through a
   `sampler2DArrayShadow` with 3 × 3 PCF. Bias is a slope-scaled normal offset in texels of the cascade
   being sampled, so the same scene looks the same at any shadow resolution. M4's single-map fit
   (`light_view_proj`) is still computed for callers and the diorama's hash.
7. **Sky.** A full-screen pass at depth 1 after the opaque draws and before blended ones: a zenith/horizon/ground
   gradient with a sun disk in the sun's direction, or an equirect texture (`Sky::texture`). The same function
   lights specular ambient through Karis's analytic split-sum approximation, with a horizon occlusion factor so
   reflections do not leak under surfaces. No IBL prefiltering (cut-ladder item); rough surfaces sample the
   texture at a higher LOD.
8. **Post.** HDR (R11G11B10F) → bloom (13-tap Karis-weighted downsample with a soft-knee bright pass on the first
   level, tent-filtered additive upsample, up to 6 levels down to 8 px) → ACES (Narkowicz, as M4) with sRGB encode
   and luma in alpha → FXAA (3.11-style, on the LDR target). Each stage toggles per frame (`PostFx`); with both
   bloom and FXAA off the pass list is M4's.
9. **Timing.** The RHI gained GPU timestamps (`CommandBuffer::write_timestamp`, `RenderDevice::last_gpu_timestamps`)
   read back in `begin_frame` once the frame's fence has passed, so reading them never stalls. `Forward3D` writes
   one per stage boundary and turns them into per-stage milliseconds; 04_cubes prints the NFR-F-01 table.
10. **Animation.** CHNK_MESH keeps its layout: `AnimInterp::CubicSpline` stores glTF's `{in, value, out}` key
    triplets in the key pool (validation accounts for the 3×). `pose_nodes` evaluates every channel of a clip
    (multi-node), and `pose_nodes_blend` / `AnimationPlayer` blend clips in local TRS space for cross-fades.
    The glTF importer version moved to 2, so existing caches re-cook.

**RHI additions (additive, all backends must implement).** `PassAttachment::layer`; commands `SetStorageData` and
`WriteTimestamp`; `DeviceCaps::timestamp_queries` and `storage_buffer_alignment`; `GpuTimestamps`. Bank table
(`rhi-renderer.md` §3.3) gains normal 7, ORM 8, shadow array 9, sky 10, emissive 13 and SSBOs 4–6.

**Consequences.** Light count is no longer a special case in the shader, materials can be authored in any glTF
tool, and the frame has a measured per-stage budget. Open: no depth prepass (the main pass shades overdraw),
no MDI yet (one draw per submesh, as M4), the cluster build runs on the main thread (the job system can take it
later without an interface change), and real-GPU numbers plus RenderDoc captures need hardware the CI container
does not have.

*Amendment (M-IMPORT3D 4a):* the glTF / FBX light converters (`mce import gltf|fbx`) take the usual 683 lm/W: engine intensity = candela / (683 pi) and sun intensity = lux / (683 pi), the reflected-radiance convention above.
