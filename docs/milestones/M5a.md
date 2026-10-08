# M5a — The 3D renderer becomes real (milestone record)

**Status:** engine, tests and the `04_cubes` sample merged into `dev_claude`. Decisions are in ADR-0024. Items that
need a real GPU or a desktop are listed under *Open*.
**Measured on:** the Linux CI container (4 cores, Mesa llvmpipe software GL 4.5, Xvfb), release build, 1280×720.
GPU numbers from a software rasterizer are the pessimistic floor, not the budget verdict (see *Budget table*).

## What shipped

| ROADMAP M5a item | Where | Evidence |
|---|---|---|
| Cook-Torrance GGX PBR + ORM materials (FR-R3D-020) | `shaders3d.h` mesh FS, `GpuSubmesh` normal/ORM/emissive keys, glTF importer v2 | `test_render3d_gl` metal vs dielectric; `test_cook` maps + ORM packing; 04_cubes sphere row (roughness × metallic) |
| Forward+ CPU-clustered lighting, ≥ 256 point lights, cluster build < 0.5 ms (FR-R3D-030) | `ClusterLights` (`clusters.h`), banks 4–6 via `set_storage_data` | `test_render3d_m5a`: every point of every light's sphere finds the light in its cluster, no light in a cluster it cannot touch, determinism, capacity; `test_render3d_gl` 257 lights + spot cone; 04_cubes 256 lights |
| Spot lights (FR-R3D-030 "directional + point + spot") | `PointLight::spot_inner/outer` | headless cone formula test; GL spot probe |
| 3-cascade texel-snapped CSM (FR-R3D-040) | `fit_cascade`, `frustum_slice_sphere`, `Scene3D::cascades()`, D32 array + `PassAttachment::layer` | `test_render3d_m5a`: slice sphere rotation-invariant, fixed sub-texel phase while the camera moves, contiguous splits, per-cascade caster lists; GL shadow probe |
| Sky: gradient dome + equirect texture path (FR-R3D-080) | `Sky`, sky pass after opaque | `test_render3d_gl` sky probe; 04_cubes `K` toggles the equirect map |
| Post chain: bloom, ACES, FXAA, each toggleable (FR-R3D-090) | `PostFx`, `Forward3D::record_post` | `test_render3d_gl` post toggles and pass counts |
| glTF node-animation tracks: multi-node, CUBICSPLINE, blending | `pose_nodes`, `pose_nodes_blend`, `AnimationPlayer`; CHNK_MESH `AnimInterp::CubicSpline` | `test_render3d_m5a` Hermite + step + cross-fade; `test_cook` cubic import; 04_cubes orrery (two clips, 0.8 s cross-fades) |
| Frame-budget tables (NFR-F-01 shape) | GPU timestamps in the RHI, `Forward3D::gpu_times`, 04_cubes `report_budget` | table below; `test_render3d_gl` timestamps |
| Sample `04_cubes` | `samples/04_cubes` | 300-tick headless test + GPU smoke in ctest; Debug/Release hash in the determinism stage |

`04_cubes` scene: a 2 × 7 material sphere row (roughness 0 → 1 across; a red dielectric row and a gold metal
row), a tiled floor and 28 sci-fi panel crates with base color / normal / ORM / emissive maps, an animated orrery,
and 256 orbiting coloured point lights with emissive bulbs. Its content comes from `tools/gen_assets.py`
(deterministic, stdlib only) and is cooked at build time like every sample. Keys: arrows orbit, `Z`/`X` zoom, `Space` auto-orbit, `L` lights, `B` bloom, `A` FXAA,
`1`–`3` cascades, `K` sky texture, `N` night, `P` switch clip.

## Budget table

04_cubes, 110 measured frames, 256 point lights, 3 cascades × 2048², 4× MSAA, bloom + FXAA on, release.
CPU columns are wall-clock on the CI container; GPU columns are `GL_TIMESTAMP` queries on llvmpipe.

| Stage | Target | CPU ms | GPU ms |
|---|---|---|---|
| scene build (animation + cull + sort + cascade fit + clusters) | — | 0.57 | — |
| of which Forward+ cluster build (256 lights, 12,288 clusters, ~25k indices) | < 0.5 ms CPU | **0.30–0.41** (0.3–0.35 headless; 0.41 with llvmpipe's threads competing for the 4 cores) | — |
| command recording (3D + HUD) | — | 0.35 | — |
| GL translate + submit | — | 232 (blocks on the software rasterizer) | — |
| shadow cascades | < 2 ms GPU | — | 13.5 |
| main pass (Forward+ PBR + sky) | — | — | 182 |
| MSAA resolve | — | — | 8.2 |
| post: bloom + tonemap + FXAA | < 1.5 ms GPU at 1080p | — | 43.9 (19.4 + 7.0 + 17.5) |
| 3D frame total | 16.6 ms with 2 ms headroom | — | 248 |

Reconciliation. The engine's CPU share per frame is ~0.9 ms (build + record), so the CPU side of NFR-F-01 has
> 15 ms of headroom, and the < 0.5 ms cluster target holds on this machine. Every GPU number is llvmpipe
rasterizing 1280×720 with 4× MSAA and up to ~100 light evaluations per pixel on four CPU cores: two orders of
magnitude slower than any GPU the anchor games target, so the three GPU targets (shadow < 2 ms, post < 1.5 ms,
60 fps with 256 lights) can only be judged on hardware (see *Open*). The relative shape is already useful: the
main pass dominates (fragment shading of clustered lights), shadows are ~5 % and post ~18 % of GPU time.

## Requirement gates

| Gate | State |
|---|---|
| FR-R3D-020 PBR metallic-roughness, basecolor/normal/ORM, albedo-only fallback | done; golden-image stability across runs is covered by the hash + GL probes, a perceptual golden image needs a reference GPU |
| FR-R3D-030 Forward+ clustered, directional + point + spot, ≥ 256 lights, cluster < 0.5 ms | done (CPU measured above; 60 Hz needs a real GPU) |
| FR-R3D-040 CSM 2–4 cascades, stable fit | done: rotation-invariant spheres + texel snapping (tested), cascade cross-fade; GPU < 2 ms open |
| FR-R3D-080 gradient skydome + equirect | done |
| FR-R3D-090 tonemap + bloom + FXAA, toggleable | done; < 1.5 ms at 1080p open |
| Determinism | 04_cubes hash identical across runs and Debug/Release; 04_diorama hash unchanged (`4b8f9c4484985f2d` at 600 ticks, same as before this milestone) |

## Open

1. **Real-GPU numbers and RenderDoc captures.** The CI container has only llvmpipe and no RenderDoc; the
   per-feature captures and the GPU column of the budget table need one run on desktop hardware
   (`sample_04_cubes -frames 600` prints the same table).
2. **LRN-2 close-out** (frame allocator under GPU pressure) is the author's own written gate.
3. **Not built, by design for this milestone:** depth prepass, MDI over a persistent object buffer, IBL
   prefiltering, skinning (FR-R3D-060, Should, deferred per the cut ladder), and a jobified cluster build. None
   needs an interface change.
