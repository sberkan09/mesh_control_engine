# render — the 2D sprite path and the 3D forward path, on top of mce_rhi

`mce_render` is an L3 library in `src/engine/render/`. Its public dependencies are `mce_rhi`,
`mce_asset` and `mce_core` (`src/engine/render/CMakeLists.txt`); it defines
`MCE_RENDER_HAS_ASSET=1` publicly. It links `mce_rhi` only, never a backend, so no GL symbol
appears here. It is linked by `mce_gameplay_kit` (which owns 2D extract), `mce_ui`,
`mce_editor_core`, the `mce_engine` umbrella, `mce_player_runtime` (directly where game modules are
unsupported) and the samples `sprites_2d`, `03_platformer`, `04_diorama`, `04_cubes`,
`05_ashen_court` and `ui_gallery`.

The renderer owns GPU state (pipelines, samplers, the shared quad index buffer, textures, meshes,
baked tilemap buffers). It holds no scene state. Each frame the caller fills a `SpriteBatch` (2D)
or a `Scene3D` (3D) from ECS data and asks the renderer to record it into an
`rhi::CommandBuffer`. There is no separate 2D renderer: 2D is an orthographic view plus the sprite
pipeline family.

## Public surface

Headers live under `include/mce/render/`.

- `renderer.h`: `Renderer` with `init(device, RendererDesc)`, the stores (`textures()`,
  `sprite_sets()`, `meshes()`), content creation (`create_sprite_set`, `create_font`,
  `create_tilemap` / `update_tilemap` / `destroy_tilemap` / `add_tilemap`), the 3D entry
  (`init_3d`, `forward_3d()`), and recording: `record_2d` (into an open pass),
  `record_post_2d` and `record_3d` (outside any pass). Also `Post2D` (vignette, LUT color grade,
  chromatic aberration), `make_identity_lut` and `kLutSize` (16).
- `types.h`: the 2D vocabulary. `SpriteVertex` (24 bytes: position, uv, sRGB tint, packed layer /
  orientation / flags), `vertex_flags` (`kCoverage`, `kLit`), `SpriteBlend`, `SpriteSetId`,
  `SpriteRegion`, `SpriteInstance`, `PointLight2D` and `Lights2D` for lit 2D.
- `sprite_batch.h`: `SpriteSets` (region tables per set, up to 4096), `SpriteBatch` (per-frame
  `begin` / `add` / `add_text` / `add_tilemap` / `build`), `TilemapDesc`, `TilemapBake` and
  `bake_tilemap`, `Font`, `TextPlacement`, `BatchStats`, `kMaxQuadsPerDraw` (16384).
- `camera2d.h`: `Camera2D`, `View2D`, `compute_view_2d` (with pixel snap), `compute_screen_view`
  (top-left origin, y down), `ScreenShake`.
- `sort_key.h`: `PassId`, `sort_key_2d`, `sort_key_3d`, field accessors and `radix_sort` (stable
  LSD radix sort of 64-bit keys).
- `text.h`: `FontGlyph`, `FontKern`, `FontMetrics`, `FontFace`, `layout_text`, `measure_text`,
  `utf8_next`. Layout yields pixel-space glyph quads; the atlas is baked at cook time.
- `sprite_animation.h`: the flipbook clip evaluator (`SpriteClip`, `SpriteAnimState`, `advance`,
  `clip_frame_at`). The component and system that drive it belong to the gameplay kit.
- `texture_store.h`: `TextureStore`, textures keyed by a stable `u32` (asset key, or a local key
  from `allocate_key()`), several generations per key for hot reload; `get(key)` returns the
  newest or the magenta checker. Also `TextureUpload` and the free `create_texture`.
- `mesh.h`: `MeshStore` (cooked `CHNK_MESH` payloads as `GpuMesh`, keyed and generational like
  textures; works without a device for headless tests), `GpuSubmesh` (dequantization matrix,
  inline material, texture keys, optional skin index), `pose_nodes`, `pose_nodes_blend`,
  `AnimationPlayer` (with `ClipEvent` playback markers), and the optional skinning pieces:
  `GpuSkin`, `GpuJoint`, `SkinnedMeshVertex`, `skin_palette`, `skin_bounds` (FR-R3D-060).
- `scene3d.h`: `Camera3D`, `orbit_camera`, `DirectionalLight`, `Fog`, `Sky`, `PostFx`,
  `ShadowSettings`, `Cascade`, `MeshInstance`, `View3D`, `compute_view_3d`, `sphere_visible`, and
  `Scene3D` (per-frame `begin` / `add` / `add_point_light` / `build`: cull, sort, cascades,
  clusters). Free helpers `transform_aabb`, `frustum_slice_sphere`, `fit_cascade`. `Sky` is the
  gradient dome with a sun disk, or an equirect texture (`texture_key`, scaled by
  `texture_intensity`); with `texture_additive` the texture is added over the gradient instead of
  replacing it (a star field fading in over a dusk sky: Solstice Reach's night).
- `clusters.h`: CPU Forward+ light assignment. `PointLight` (spot when `spot_outer > 0`),
  `ClusterGridDesc`, `ClusterLights`, `GpuPointLight`, `sphere_ndc_bounds`.
- `forward3d.h`: `Forward3D`, the 3D frame: cascaded shadows, Forward+ PBR main pass into an
  R11G11B10F HDR target with MSAA, sky, blended draws, resolve, bloom, ACES tonemap, FXAA. Also
  `Forward3DDesc`, `Forward3DStats` and `Forward3DGpuTimes` (per-stage GPU milliseconds).
- `asset_upload.h`: `AssetUploadSink`, the renderer's `asset::IGpuUploadSink` (cooked textures
  and meshes uploaded under their asset key and generation; retired generations destroyed), plus
  `upload_cooked_texture` and `load_cooked_font`.

Built-in shaders are GLSL 450 strings in `src/renderer.cpp` (2D) and `src/shaders3d.h` (3D).

## Rules and invariants

- **Threading.** Main thread only, the same thread as the device.
- **Per-frame allocation.** `SpriteBatch` and `Scene3D` take an allocator for all per-frame
  arrays; pass `mem::frame()` (tests pass `mem::heap()`). They hold nothing across frames.
- **Determinism.** Batch and scene builds are CPU-only and deterministic. The radix sort is
  stable, so ties keep submission order and draw order is a function of submission order and
  camera. Callers must submit in ECS iteration order. `Scene3D::Stats::cluster_ms` and `build_ms`
  are the only non-deterministic fields.
- **2D batching.** A group is a maximal run sharing (blend, sprite set) and source (dynamic stream
  or one tilemap layer); each group is one multi-draw-indirect. Runs longer than
  `kMaxQuadsPerDraw` split into several commands of the same MDI. Sprite sets upload as 2D-array
  textures even with one layer.
- **Tilemaps.** A layer is baked into 32×32-tile chunks and uploaded once into a persistent vertex
  buffer; per frame only chunk visibility is computed. `add_tilemap`'s `offset` moves a layer for
  parallax without a re-bake. `update_tilemap` retires the old buffer past the frames in flight.
- **3D drawing.** One indexed draw per visible submesh with a per-draw `ObjectU` uniform block
  (bank 3), not MDI. Opaque draws sort front-to-back, then blended draws back-to-front.
  Conventions: right-handed, Y up, column vectors, GL clip z in [-1, 1].
- **3D is opt-in.** `init_3d` creates the HDR, MSAA and shadow targets on first use, so 2D-only
  games never allocate them. Targets follow the scene viewport and are recreated on resize.
  `ShadowSettings::resolution` must equal `Forward3DDesc::shadow_size`.
- **Color output.** Backbuffer and editor targets are plain RGBA8. The sprite shader and the 3D
  tonemap encode sRGB themselves (`RendererDesc::encode_srgb_output`,
  `Forward3DDesc::encode_srgb_output`); turn it off when rendering into an sRGB target. 2D post
  works in that display (sRGB-encoded) space.
- **Floating origin.** `Scene3D::set_origin_shift(delta)` marks the frame after a world rebase
  (M-OPENWORLD A2). Forward3D treats it as a camera cut (TAA, velocity and SSR history restart),
  keeps the accumulated origin so reflection probes and GI volumes are keyed by absolute position
  (no re-capture), and restarts GPU particle rings.
- **Failure handling.** Missing or failed textures resolve to the magenta checker; uploads
  validate cooked bytes and keep the previous generation on failure. Mesh instances whose key is
  not resident are skipped.
- **Walls.** GL wall and glad wall (`scripts/ci/check_walls.py`): no `gl*()` calls or glad
  includes here. The stb and meshopt walls keep cook-time code out; the engine container ban
  (`std::vector`, `std::string`, iostream) applies.

## How it is tested

- `test_render` (`tests/render/`, no GPU, links only `mce_render`):
  - `test_render.cpp`: sort keys and radix sort, Camera2D and pixel snap, screen shake, flipbook
    events, UTF-8 and text layout, quad expansion, "render: batch groups by blend × sprite set, one
    MDI each (FR-R2D-010)", culling, long-run splitting, tilemap chunk bake and cull, "render: batch
    build is deterministic", lit-quad orientation bits.
  - `test_render3d.cpp`: `MeshStore` without a device, `pose_nodes`, frustum culling, opaque and
    blended ordering, stable ties, shadow fit.
  - `test_render3d_m5a.cpp`: Forward+ cluster bounds and assignment ("every point inside a light's
    sphere finds the light in its cluster"), CSM slice spheres and texel snapping ("cascades snap to
    whole texels as the camera moves (no shimmer)"), CUBICSPLINE keys and cross-fades.
  - `test_render3d_skin.cpp`: the skin section round trip and corrupt-section rejection, joint
    palettes, posed bounds, per-instance opt-out, playback events.
- `test_render_gl` (`tests/render_gl/`, GPU, label `gpu`, via `rhi::create_default_device`): pixel
  probes. `test_render_gl.cpp`: two atlas layers in one MDI, premultiplied and additive blending,
  tilemaps interleaving with sprites, parallax offset, lit sprites, 2D post toggles, coverage text,
  texture hot reload and magenta fallback, mip and layer uploads, cooked `CHNK_TEX` / `CHNK_FONT`
  through the asset sink. `test_render3d_gl.cpp`: "lit box, its shadow on the ground, zero
  validation errors", post toggles, sky, metal vs dielectric, clustered point and spot lights, GPU
  timestamps per stage, fog, MSAA off, animated node, "a skinned bone bends with its joint;
  skinning off draws the bind pose".
- GPU tests register only with `-DMCE_GPU_TESTS=ON` (Xvfb + Mesa llvmpipe in
  `scripts/ci/local_ci.sh`). The samples register windowed runs the same way, labelled `gpu`
  (for example `sample_04_cubes_gpu`, `sample_05_ashen_court_gpu`, `sample_03_platformer_gpu`).

## Design records

- [rhi-renderer.md](../architecture/rhi-renderer.md): §2.4 renderer API, §2.5 cameras, §3.2 MDI
  model, §3.4 per-frame pipeline (extract, cull, sort, batch, record), §3.5 pass lists, §3.6
  lighting, §3.7 render targets, §6 test strategy. Where the built code differs, the ADRs below
  record it.
- ADR-0020 (`docs/adr/0020-2d-render-path.md`): the 2D path as built (extract in the kit, 24-byte
  `SpriteVertex`, segment MDI, sRGB in the shader, array textures, runtime tilemap bake).
- ADR-0031 (`docs/adr/0031-m6-2d-extras.md`): particles, parallax, lit 2D, 2D post.
- ADR-0022 (`docs/adr/0022-gltf-static-import-m4.md`): `CHNK_MESH` format.
- ADR-0023 (`docs/adr/0023-3d-render-path-m4.md`): the M4 3D path (MeshStore, quantized streams,
  per-draw `ObjectU`, CPU node animation, shadow fit, tonemap).
- ADR-0024 (`docs/adr/0024-pbr-forward-plus-csm-m5a.md`): PBR, Forward+, CSM, sky, post, timing.
- ADR-0033 (`docs/adr/0033-shader-cook.md`): shader cook.
- M-SKIN.md (`docs/milestones/M-SKIN.md`): optional skinning, the skin section of `CHNK_MESH`
  and the skinned pipelines.
- Milestones: M4.md (`docs/milestones/M4.md`), M5a.md (`docs/milestones/M5a.md`),
  M6-2d.md (`docs/milestones/M6-2d.md`), M6-shader-cook.md (`docs/milestones/M6-shader-cook.md`),
  M7-ashen-court.md (`docs/milestones/M7-ashen-court.md`).

## Known gaps

- M5a "Not built, by design": depth prepass, MDI over a persistent object buffer, IBL
  prefiltering and a jobified cluster build. (Skinning, also listed there, was built later as an
  opt-in feature: M-SKIN.)
- Built-in shaders are not cooked assets yet (M6-shader-cook "Not built"), and the ≤ 1 s shader
  save-to-pixels loop is not built.
- RenderDoc captures and golden images need hardware (M5a, M6-2d, M7). Linux CI has only
  llvmpipe; real-GPU numbers exist only from the Mac leg (`scripts/ci/local_ci_macos.sh`). The
  lit-2D and 2D-post GPU budgets in M6-2d are open for that reason.
- M7: Ashen Court's characters are static node-transform proxies; skinning exists (M-SKIN) but
  the game does not use it. Skinning has no morph targets, dual quaternions or GPU palette
  compute, and the editor does not preview skeletons.
- `AssetUploadSink::update_material` is a no-op; 3D materials travel inline in the mesh chunk
  (`GpuSubmesh::material`).
