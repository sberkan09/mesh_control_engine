# asset — L2 cooked asset runtime (`.mce` containers, GUIDs, chunk formats) and the cook library

Two targets live here. `mce_asset` is layer L2 (`src/engine/asset/CMakeLists.txt`): the `.mce` container
reader and writer, GUIDs, cooked chunk formats, and the runtime asset registry (handles, refcount, deferred
unload, hot-reload commit, `IGpuUploadSink`). It depends publicly on `mce::mce_core` and `mce::mce_refl` and
contains no third-party code, since shipped runtimes link it (FR-AST-010). It is linked by `mce_render`,
`mce_audio`, `mce_scene`, `mce_ecs_scene`, `mce_gameplay_kit`, several samples, and the `mce_engine` umbrella.

`mce_asset_cook` (`src/engine/asset/cook/`) is the cook library: scan, importer registry, importers,
dependency graph, cook DB, content-addressed outputs and the shipping pack. It depends publicly on
`mce::mce_asset` and privately on `mce_stb`, `mce_meshopt` and glslang. It backs the `mce_cook` CLI
(`tools/cook/`), the editor's cook button and hot reload (`src/editor/core`), and tests. It is never linked
into a runtime target.

## Public surface

Runtime (`src/engine/asset/include/mce/asset/`)
- `container.h`: the `.mce` container. `ContainerHeader` (64 bytes, magic `MCE1`, version 1.0, header and
  payload CRC-32s), `TocEntry` (48 bytes: type, per-chunk schema version, flags, owning GUID, offset, size),
  the closed `ChunkType` enum (appended only), `ContainerView::open` (validated, non-owning view; `find`,
  `payload`, `check_version`) and `ContainerWriter` (chunks laid out in insertion order). Payloads are 16-byte
  aligned.
- `formats.h`: one reader and one writer per cooked chunk, each payload a fixed header plus payload-relative
  offsets: manifest (GUID to path, content hash and type, sorted by GUID; the index is the `AssetKey`),
  texture, sfx, font, material, tilemap, particle preset, sequence, shader (GLSL source, variant table, SPIR-V
  and a resource reflection list per variant), mesh (quantized streams, submeshes, node tree, animation
  clips, and an optional skin section), and scene/prefab (columnar component rows plus asset and entity fixup tables; `SceneView` reads
  both). Schema majors are the `*_VERSION` constants.
- Mesh skins (`formats.h`, FR-R3D-060): a mesh with `MeshFlags::Skinned` is chunk minor `MESH_MINOR_SKIN`
  (1) and carries a `MeshSkinHeader` at `MeshHeader::skin_offset`: `MeshSkin` entries (a range of the joint
  table, 1 to `kMaxSkinJoints` = 256 joints, since joint indices are u8), `MeshJoint` entries (driving node,
  inverse bind matrix, bind-space AABB of the vertices the joint influences), one `MeshSkinVertex` per vertex
  (4 u8 joint indices, 4 unorm8 weights summing to 255) and one skin index per submesh (-1 = rigid). Joints
  are ordinary nodes, so the node clips animate skeletons. A mesh without the flag is exactly the static
  layout (minor 0). `MeshView` exposes `skins`, `skin_vertices`, `submesh_skins` and `skinned()`, and
  validates the skin section's indices. `MeshFlags::Ragdoll` (minor `MESH_MINOR_RAGDOLL`, 3; needs `Skinned`)
  marks joints that carry ragdoll overrides in fields that were reserved (`ragdoll_flags` of `MeshRagdoll`
  Exclude / Include / Radius / Swing / Twist / Mass, `ragdoll_radius`, `ragdoll_swing`, `ragdoll_twist`,
  `ragdoll_mass`); without the flag they must be zero.
- `guid.h`: `Guid` (128-bit UUIDv4), `to_string`, `to_hex`, `parse_guid`.
- `handle.h`: asset type tags (`Texture`, `Mesh`, `Material`, `Shader`, `Sfx`, `Font`, `Atlas`, `Scene`,
  `InputMap`, `Prefab`, `Tilemap`, `Sequence`, `ParticlePreset`, `SpriteSheet`), `AssetHandle<T>` (generational index),
  `AnyHandle`, and `AssetRef<T>` (GUID + handle, what components store). Specializes `refl::FieldTraits` so an
  `AssetRef<T>` field is `FieldKind::AssetRef`.
- `asset_system.h`: `AssetSystem`. `mount` (dev cooked directories holding `manifest.mce`, or a pack
  `assets.mce`; earlier roots win on a shared GUID), `find`, `info`, `load_sync<T>` (one reference per
  successful call), `retain`/`release`, `resolve` (a null or stale handle resolves to the type's placeholder
  and logs once), `resolve(AssetRef&)` (re-stamps a handle left stale by a reload), `commit_reload`,
  `end_frame`, double-buffered `events()` (`Reloaded`, `Unloading`, `UnloadFailed`, `LoadFailed`),
  `set_upload_sink`. Async loading (FR-ENT-120): `request_async<T>(key)` returns an `AsyncTicket`; a dev
  mount's file is read, hashed and CRC-checked on a loader thread (started by the first such request) and
  committed on the main thread by `poll_async()` (also run by `end_frame`), exactly as `load_sync` would make it
  resident. Resident and pack assets are ready at once. `async_status`, `take_async` (the handle with one
  reference, or the load error), `cancel_async` (drops a pending read, releases a ready one), `wait_async`,
  `async_pending`. A reload that changes the content hash while a read is in flight falls back to a sync load.
- `mods.h`: data mods (ADR-0034). `read_mod_pack` (a pack's manifest and `CHNK_MOD` identity), `resolve_mods`
  (which `*.mce` packs of a mods folder load and in what order: file-name order, or `load_order.txt`; mods for
  another game, duplicate ids and unreadable packs are skipped with a reason), `is_valid_mod_id`, and `ModList`,
  which the player also publishes as a world resource. `formats.h` holds the `CHNK_MOD` reader and writer
  (`ModInfoView`, `write_mod_info`).
- `gpu_upload_sink.h`: `IGpuUploadSink`, the inverted seam to the renderer (fix F6): `upload_mesh`,
  `upload_texture`, `update_material`, `destroy_gpu_objects`. Defined here and implemented by `mce_render`, so
  the include direction stays render to asset.
- `file_io.h`: `read_file`, whole-file reads into 16-byte aligned heap storage.
- `xxhash.h`: `xxh64`, the pipeline's content hash.

Cook (`src/engine/asset/cook/include/mce/cook/`)
- `cook.h`: `CookService::cook(project_root, CookOptions{force, pack, adopt, base_packs})` returning a `CookReport`
  (per-asset `Cooked`/`UpToDate`/`Failed`/`Blocked`, diagnostics, manifest and pack hashes); `AssetRegistry`
  (scan of `assets/` and `.mceasset` sidecars, `guid_for_path`, GUID-stable `move`); `cook_document` (cooks an
  unsaved native document from memory, used by play-in-editor); `generate_guid`; `kCookToolVersion`.
- `importer.h`: `IImporter` (`id`, `version`, `schema_hash`, `type`, `native`, `handles_extension`,
  `scan_dependencies`, `cook`), `ImportInput`, `IAssetLookup`, `DepRef`, `ImporterRegistry::add_builtins`.
  Built-in importers: texture (PNG and JPEG via stb_image), WAV, font (TTF atlas via stb_truetype), material
  (`.mcmat`), scene (`.mcscene`), prefab (`.mcprefab`), glTF (meshoptimizer), tilemap (`.mctilemap`),
  sprite sheet (`.mcsheet` → `ChunkType::SpriteSheet`, `SpriteSheetView`), animation graph (`.mcanim` →
  `ChunkType::AnimGraph`, `AnimGraphView`; a version 2 graph adds the chunk's extension block, minor 1: 2D blend
  points, layers, mask bones, IK chains and the root motion bone), particles (`.mcpart`), sequence (`.mcseq`), input map (`.mcinput` → `ChunkType::InputMap`, the
  `mce/input/input_map.h` chunk, validated with `decode_input_map` at cook), shader (`.vert/.frag/.comp.glsl`, glslang). The glTF importer
  also imports skins (`JOINTS_0` / `WEIGHTS_0`, 4 influences, up to 256 joints per skin, inverse bind
  matrices); the sidecar setting `"skinning": false` imports a skinned mesh as a static one, and `"ragdoll"`
  writes per-bone ragdoll overrides into the joints (`MeshFlags::Ragdoll`, `MeshRagdoll` flags, chunk minor 3).
- `mod_project.h`: mod projects (ADR-0034). `is_mod_project`, `read_mod_project` (`mod.json`), and
  `read_base_assets` (GUID to type from the manifests of the game packs a mod builds on). `CookService::cook`
  uses them: references into a base resolve by type without a dependency edge, an override (a base GUID in the
  project) must keep its type, `CookOptions::base_packs` adds bases (`mce_cook --base`), and a mod's pack gets a
  `CHNK_MOD` chunk after the manifest. `CookReport` gains `mod`, `base_assets` and `overrides`.
- `json.h`: hand-written JSON/JSONC reader and canonical writer; every node keeps its line and column.
- `diagnostics.h`: `Diagnostic`/`DiagList`, printed as `file:line:col: error: message [stage]`.
- `scene_text.h`: the `.mcscene` component codec shared by the scene cook and the editor's `SceneDocument`.
- `audio.h` (WAV reader, resampler, normalization), `fs.h` (`write_file_atomic` and other filesystem helpers),
  `string.h` (cook-side `String`).

The cook pipeline (`src/cook.cpp`):
1. Scan `assets/` and the sidecars.
2. Graph build: importers declare references; missing and mistyped references are reported.
3. Topological order (Kahn), ties broken by GUID.
4. Stage execution, level by level, through `mce::jobs`.
5. Commit: content-addressed outputs at `cooked/<aa>/<guid-hex>/<content-hash>.mce`, the manifest, and the
   cook DB (`cooked/.mcedb`, an append-only journal of re-cook keys, `src/cook_db.h`).
6. With `--pack`, `<project>/assets.mce`: the manifest chunk first, then every live asset's chunks in GUID
   order.

`mce_cook <project-dir> [--module <game module>] [--base <pack>]... [--pack] [--force] [--adopt] [--quiet]` exits 0 (clean), 1 (cook
errors, a module that fails to load among them) or 2 (usage).

## Rules and invariants

- The runtime never parses text. JSON, stb, meshoptimizer and glslang stay in the cook library (FR-AST-010).
  Walls in `scripts/ci/check_walls.py`: the stb and meshopt walls allow those symbols only under
  `src/engine/asset/cook/`; the shader-cross wall allows glslang only in
  `src/engine/asset/cook/src/shader_importer.cpp` (and `mce_rhi_metal`); the cook wall allows `#include
  "mce/cook/..."` only in `src/engine/asset/cook/`, `src/editor/` and `tools/cook/`; the cook link wall allows
  `mce_asset_cook` only in CMake files under `src/engine/asset/`, `src/editor/`, `tools/` and `tests/`.
- Cook determinism (FR-AST-030): stages are pure (bytes and settings in, bytes out; no clock, paths or
  environment in any output), each stage writes only its own outputs, and the manifest and pack are sorted by
  GUID. Deleting `cooked/` and re-cooking gives byte-identical files.
- Re-cook keys are content hashes, never mtimes: source hash, settings hash, importer id and version,
  `kCookToolVersion`, the importer's `schema_hash` (the scene cook hashes every reflected component layout),
  and each dependency's output hash. The cook DB is an optimization only; a corrupt tail is dropped and those
  assets re-cook.
- Outputs are new files, never renamed over. A failed importer leaves the previous output serving.
- Container validation order is part of the contract: size, magic, version, reserved bytes, bounds, alignment,
  header CRC, payload CRC. Each failure has its own `Errc` (`BadMagic`, `VersionTooNew`, `ReservedNonZero`,
  `OutOfBounds`, `Misaligned`, `CrcMismatch`, `PayloadCrcMismatch`, `ChunkVersionTooNew`). Chunk payloads are
  untrusted bytes; every reader bounds-checks before handing out views.
- Dev mounts verify every container's payload CRC on load; a pack is verified once at mount and trusted
  afterwards.
- Mounts overlay by GUID: an earlier root wins, but only with the same type. A later root whose GUID an earlier one
  holds with another type keeps it (with a warning) while nothing has loaded that asset, so a mod cannot retype
  a game asset (ADR-0034).
- `AssetSystem` is main-thread only: the main thread is the sole mutator, so the read path takes no locks.
  `IGpuUploadSink` is called from the main thread only.
- A release at refcount zero queues the asset; it is reaped `MCE_FRAMES_IN_FLIGHT` `end_frame()` calls later.
  A re-acquire in between keeps it resident (`UnloadFailed` event). After `commit_reload` the old blob stays
  alive for the same number of frames.
- `commit_reload` is all-or-nothing: on error the old generation keeps serving. Hot reload is driven by the
  editor (`src/editor/core/include/mce/editor/hot_reload.h`: a polling `AssetWatcher`, an incremental re-cook,
  then `commit_reload`); a shipped runtime has no watcher.
- GUIDs are created once by the sidecar (or inline in native assets), never at runtime, and stay stable across
  moves and renames. References between assets are GUIDs, written `guid:<canonical>` in native text.
- The importer set is open for engine types and closed for users (assets.md §7.1).

## How it is tested

`tests/asset/CMakeLists.txt` registers:
- ctest `test_asset` (links `mce_asset` only). `test_container.cpp`: round-trip, "writer output is
  deterministic", the negative suite ("each failure is a distinct, diagnosable error"), schema version check,
  "fuzz — 1000 deterministic byte mutations never crash". `test_guid_formats.cpp`: GUID parse and ordering,
  xxh64 reference vectors, texture/sfx/font/manifest formats. `test_asset_system.cpp`: "mount a dev tree,
  load, refcount, deferred unload", "re-acquire between release and reap keeps the asset resident", "corrupted
  cooked file fails the load, placeholder serves", "hot-reload commit bumps the generation; AssetRef
  re-stamps", "pack mount serves chunks straight out of the pack", "placeholders are valid payloads of their
  type", "missing mount root is an error", and the async cases ("async load reads off the main thread,
  commits on poll", "cancelled async loads leave nothing behind", "an async read of a corrupt file fails like
  load_sync", "async loads from a pack are ready at once").
- ctest `test_cook` (links `mce_asset_cook`). `test_cook_project.cpp`: "full project cooks, then everything is
  up to date", "determinism — deleting cooked/ and re-cooking is byte-identical (FR-AST-030)", "incremental —
  a changed texture re-cooks itself and its dependents only", "a corrupt cook DB is dropped and the cook still
  succeeds", "author -> cook -> mount -> load the scene (M2 pipeline)", missing sidecar and `--adopt`,
  duplicate GUIDs. `test_cook_gltf.cpp`: `.gltf`/`.glb` equivalence, `--force` byte identity, triangle budget,
  `MeshView` rejecting corrupt payloads, "skins import joints, inverse binds, 4 unorm8 influences and per-joint
  bind bounds", "skinning is optional per asset; bad skins fail with the JSON pointer". `test_cook_json_audio.cpp`: JSON positions and nesting limits,
  canonical writer, WAV decode and resampling. `test_cook_shader.cpp`: variants, SPIR-V and reflection in
  `CHNK_SHD`, compile errors with source line and variant, binding-bank enforcement. `test_cook_input.cpp`:
  ".mcinput cooks to CHNK_INPUT and loads into the input system", "authoring mistakes are cook errors".
- `test_mods.cpp` (in `test_asset`): `CHNK_MOD` round trip, negative cases and mutations, mod ids,
  `resolve_mods` with and without `load_order.txt`, and the mount overlay. `test_cook_mod.cpp` (in `test_cook`):
  a mod cooks against a game pack (override, addition, `CHNK_MOD`, byte-identical re-cook) and its error cases.
  ctest `player_mods` (registered in `src/player`): a game and a mod cooked with `mce_cook`, played headless.
- ctest `mce_cook_cli` (`cook_cli_test.cmake`): exit codes 0/1/2 and byte-identical re-cooks of the sample
  project in `tests/asset/data`.
- ctest `mce_cook_module` (`cook_module_test.cmake`, only when `template_probe_game` is built): the stock
  `mce_cook` with `--module` cooks the template probe's scene that uses the game's components; without it the
  cook reports them unknown.
- ctest `test_shader_gl` (label `gpu`, only with `MCE_GPU_TESTS`): cooked shader variants compile on a real GL
  4.5 driver.

Scene and prefab chunks are also exercised end to end by `tests/ecs/test_prefab.cpp` and `tests/scene` (see
[ecs.md](ecs.md) and [scene.md](scene.md)).

## Design records

- [../architecture/assets.md](../architecture/assets.md): chunk types (§2), the container (§3, §3.1 pack and
  content-addressed output), the GUID registry and sidecars (§4), the cook graph, per-type stages and cook DB
  (§5), scene serialization (§6), runtime loading, refcounting and hot reload (§7), threading (§8), tests
  (§10).
- [../ARCHITECTURE.md](../ARCHITECTURE.md) §4.1 (import, cook, hot reload flow), fix F6 (upload sink) and F12
  / ADR-0015 (editor-owned watcher) in §5.1.
- `docs/adr/0019-asset-pipeline-m2.md`: stb dependency, hand-written cook
  JSON, container layout fixes.
- `docs/adr/0022-gltf-static-import-m4.md`: glTF import and `CHNK_MESH` as
  built.
- `docs/adr/0033-shader-cook.md`: shader validation, variants and SPIR-V reflection.
- `docs/adr/0024-runtime-scene-path.md`: cooked scenes into entities.
- `docs/adr/0029-play-in-editor.md` §2: hot reload in the editor.
- `docs/adr/0030-m6-editor-authoring.md` §8 (tilemaps),
  `docs/adr/0031-m6-2d-extras.md` (particle presets),
  `docs/adr/0032-sequence-runner.md` (sequences).
- `docs/adr/0034-data-mods.md`: data mods (mod packs, `CHNK_MOD`, the mount overlay, mod projects).
- Milestones: `docs/milestones/M4.md`,
  `docs/milestones/M6-shader-cook.md`,
  `docs/milestones/M6-2d.md`,
  `docs/milestones/M8-project-template.md` (per-project `<game>_cook` that
  links the game's component types),
  `docs/milestones/M-SKIN.md` (skinned meshes),
  `docs/milestones/scene-streaming.md` (async loads behind scene streaming).

## Known gaps

- `file_io.h` is a placeholder until `mce_platform`'s filesystem and mmap land: files are read whole into heap
  memory, including a mounted pack.
- Async loading (`request_async`) moves only the file read, hash and CRC check of a dev mount off the main
  thread. A pack is read whole at mount, so its async requests are synchronous binds; GPU uploads still
  happen at commit on the main thread.
- The shader placeholder is an empty payload (`src/asset_system.cpp`: "Shader/...: empty payload until their
  formats land").
- Cook stages go through `mce::jobs`, which runs jobs inline today, so the cook is single-threaded.
- `mce_cook` and `mce_editor` know the engine's reflected components (`tools/cook/engine_components.cpp`)
  plus the game module given with `--module` / `-module` (modules.md); on Windows, where modules are not
  built, a game's components cook only through the project's own `<game>_cook` (M8-project-template.md).
- macOS deployment below macOS 26 needs a float `from_chars` fallback in the cook's JSON reader (M-MAC.md).
