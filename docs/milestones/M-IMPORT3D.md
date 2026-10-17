# M-IMPORT3D — A wider 3D import path: glTF gaps, OBJ, FBX (milestone record)

**Status:** planned (added 2026-10-17; no Linear issues: the workspace is at its free limit, the record is the tracker).
Plan: this record (ROADMAP status row only). Source: the Unreal comparison of 2026-10-16
(`assessments/unreal-kiyaslama-2026-10-16.md`), which rates import "behind (the 3D DCC path is narrow)": Unreal takes
FBX, glTF and USD, the engine takes glTF only.

**Why.** Most bought and downloaded 3D content ships as FBX or OBJ, and glTF files from real exporters use features the
importer refuses (sparse accessors, quantized attributes, meshopt compression) or drops without a word (cameras, vertex
colours). Today a user's only route is "open it in Blender and export a .glb". M-VISUAL3 lists "a 3D importer beyond
glTF" as out of its scope; this milestone picks that up.

**What exists** (read on `origin/dev_claude`, 2026-10-17; line numbers are that tree's).
- `src/engine/asset/cook/src/gltf_importer.cpp` (1,719 lines), one `IImporter` for `.gltf` / `.glb`
  (`handles_extension`, :1541) that writes one `CHNK_MESH`. Buffers inside the file only (GLB chunk or base64 URIs).
  Per primitive: POSITION, NORMAL (generated when absent), TEXCOORD_0; meshopt de-duplicate, vertex cache and fetch
  order, then quantization to `asset::MeshVertex` (16 bytes: snorm16 position, octahedral normal, unorm16 UV;
  `formats.h:1443`). Node tree, TRS animation clips, skins with JOINTS_0 / WEIGHTS_0 (4 influences, 256 joints), the
  `ragdoll` sidecar, `lod_levels` generated LODs and `<name>_LOD<n>` authored LODs, `max_triangles`, `scale`.
  Materials: factors, alpha mode, the five texture slots on TEXCOORD_0, one KHR_texture_transform, KHR_materials_unlit,
  _emissive_strength, and the factors (not the textures) of _clearcoat, _sheen, _anisotropy, _transmission, _ior (:572).
- Refused or dropped: `sparse` accessors are an error (:325). POSITION and NORMAL accept FLOAT only (`read_accessor`
  with `allow_int_normalized = false`, :791-793), so a KHR_mesh_quantization file fails with "component type not
  supported". Morph targets: a warning, ignored (:782). JOINTS_1 / WEIGHTS_1: a warning, ignored (:787). A texture's
  `texCoord != 0`: a warning (:491). COLOR_0, TEXCOORD_1 and `cameras` are dropped **with no warning** (the header
  comment says cameras and lights warn; only `KHR_lights_punctual` does, through the `extensionsUsed` check, :245).
- `kSupportedExtensions` (:65) names only the first three material extensions, so a file that lists
  KHR_materials_clearcoat (or sheen, anisotropy, transmission, ior) in `extensionsUsed` gets "extension ... is
  ignored" although its factors are read, and one that lists it in `extensionsRequired` is a cook error.
- The mesh back end (`Geometry`, `FatVertex`, `import_primitive`'s meshopt and quantize pass, LOD generation, skin
  tables, the chunk write) lives in the importer's anonymous namespace (:686-1010, :1400-1710), so a second mesh
  importer cannot reuse it today.
- No OBJ, FBX or USD importer. `meshoptimizer` v0.25 is already a cook dependency and contains the
  EXT_meshopt_compression decoders (`meshopt_decodeVertexBuffer`, `meshopt_decodeIndexBuffer`, the filters).
- Fan-out (`scripts/ci/include_fanout.py`, 645 TUs): `mce/cook/importer.h` 169, `mce/asset/formats.h` 318,
  `mce/cook/cook.h` 140, `mce/kit/kit3d.h` 116, the cook-private `importer_common.h` 28.
- Collision: `MeshCollider3D` (`kit3d.h:370`) builds a static triangle mesh from a mesh asset's rest pose at Startup;
  `physics3d` has `Shape3D::convex` (`world3d.h:74`). There is **no cooked collision asset**, so the "auto collision
  at cook" idea is dropped (see Not in scope).
- `mce import` (`tools/mce_cli/mce_import.py`, M-IMPORT) and MCP `asset_import` (`src/editor/mcp/src/tools_import.cpp`,
  `tool` enum `aseprite | tiled | ldtk`, :107) convert 2D tools' exports to native text assets.

**Boundaries.** All engine work stays in `src/engine/asset/cook/` (plus `cmake/Dependencies.cmake`,
`scripts/ci/check_walls.py`, tests, docs and `tools/mce_cli/`). No change to `render/`, the shaders, `MeshVertex`, the
`CHNK_MESH` layout or the material importer: M-VISUAL3 owns the material importer (step 2, POM), and anything a vertex
layout or a shader must draw (vertex colours, a second UV set, runtime morph targets) needs the render owner and is a
follow-up here. M-IMPORT's files (`mce_import.py`, `tools_import.cpp`) are touched only to dispatch to a new script and
widen one enum, the way `mce_import_skel.py` did for M-ANIM2D.

## Steps

| # | Step | Size | Model | State |
|---|---|---|---|---|
| 0 | Seam: move the mesh back end out of `gltf_importer.cpp` into cook-private `src/mesh_build.h` / `.cpp`; byte-identical output | S (4–6 h) | Sonnet (Opus review: determinism) | done, #485 |
| 1 | glTF gaps: extension list fix, sparse accessors, KHR_mesh_quantization, EXT_meshopt_compression, 8 influences reduced to 4, morph default weights baked, warnings for every dropped attribute; KHR_draco a named error | M (12–18 h) | Sonnet (Opus review: untrusted input, determinism) | done, #496; open: M-FUZZ survey row for the meshopt path, meshopt OCTAHEDRAL/QUATERNION output is SIMD-dependent across hosts (never put a filtered fixture in the mesh golden), animation `out.keys` has no scene-wide cap |
| 2 | OBJ (+MTL) importer, own parser, on the step-0 back end | M (8–12 h) | Sonnet | done, PR pending; the `.mtl` is a registered asset (importer `mtl`), see Step 2 "as built"; open: the OBJ fuzz loop is in `test_cook_obj.cpp`, not yet in the M-FUZZ survey |
| 3a | FBX decision and pin: ADR on ufbx, `Dependencies.cmake`, the "ufbx wall", ARCHITECTURE §7 row; one smoke test | S (4–6 h) | Opus (new third-party dependency) | done, PR pending; ufbx v0.23.1 @`26a482a`, ADR-0047 (0046 is taken by M-OPENWORLD's road networks), see Step 3a "as built" |
| 3b | FBX importer on ufbx: meshes, materials, unit and axis conversion, skeleton, skins, animation stacks onto `Geometry` | L (20–30 h) | Sonnet (Opus review: axis/unit maths and determinism) | done, PR pending; see Step 3b "as built" |
| 4 | glTF / FBX lights and cameras as a prefab (`mce import gltf|fbx`), MCP `asset_import` `tool: gltf|fbx`, a per-mesh import summary, guide 8.4, fixtures, the close | M (10–14 h) | Sonnet | planned |

Total ≈ 60–85 h *(rough guess)*. Order: 0 first (everything else builds on it); 1 and 2 may run in parallel after it;
3a before 3b; 4 last.

### Step 0 — the mesh back end as a seam

- **Files.** New `src/engine/asset/cook/src/mesh_build.h` and `mesh_build.cpp` (cook-private, next to
  `importer_common.h`, so no public header and no fan-out); `gltf_importer.cpp` keeps parsing and calls them;
  `src/engine/asset/cook/CMakeLists.txt`. Moved as is: `FatVertex`, `Geometry`, `JointBox`, `quantize_weights`, the
  meshopt pass and quantization of `import_primitive`, `dequantize_positions`, LOD generation, the skin tables and the
  `asset::MeshData` fill. The interface takes a format-neutral primitive (float positions, normals, UVs, up to 4
  joint/weight pairs, indices, material slot, node) so OBJ and FBX fill the same struct.
- **Tests.** `tests/asset/test_cook_gltf.cpp` unchanged and green; a new case cooks every glTF model under `samples/`
  and the file's own fixtures and compares each `CHNK_MESH` hash with a list recorded on the base commit and checked
  into the test (the list changes only in a step that names the change).
- **Acceptance.** Every cooked mesh byte-identical; no `.h` under `include/` changed; `gltf_importer.cpp` shrinks by
  the moved code.
- **Risks.** A silent reorder of a float sum changes quantized bytes; the byte comparison catches it. Keep
  `-ffp-contract` as the cook already builds it.

### Step 1 — glTF gaps (cook-only)

- **Extension list.** Add KHR_materials_clearcoat, _sheen, _anisotropy, _transmission, _ior and KHR_mesh_quantization
  and EXT_meshopt_compression to `kSupportedExtensions`; fix the header comment.
- **Sparse accessors** (glTF 3.6.2.3): base view (or zeros) then the sparse indices / values substituted, for every
  attribute, animation and inverse-bind accessor. Errors for out-of-range or unsorted indices name the pointer.
- **KHR_mesh_quantization:** POSITION as BYTE / SHORT (normalized or not), NORMAL normalized BYTE / SHORT,
  TEXCOORD_0 as plain BYTE / SHORT (the node transform carries the dequantization scale, as the spec requires).
- **EXT_meshopt_compression:** decode the compressed bufferViews with meshoptimizer's decoders (attributes, triangles,
  indices; OCTAHEDRAL, QUATERNION, EXPONENTIAL filters) before accessors read them; the fallback buffer is ignored.
- **KHR_draco_mesh_compression:** not supported (would add Draco, Apache-2.0, ~100k lines, to the cook). When
  required: an error saying to re-export with meshopt (`gltfpack -c` or `gltf-transform meshopt`); when only used
  with a fallback: the fallback is read.
- **JOINTS_1 / WEIGHTS_1 (and further sets):** read all sets, keep the 4 largest weights per vertex (ties by set then
  slot), renormalize, then the existing unorm8 rounding; a one-line warning with the count of vertices that lost
  weight above 1%. `MeshSkinVertex` stays 4 x u8.
- **Morph targets:** the mesh's (or node's) default `weights` are applied to POSITION / NORMAL at cook so the rest
  pose matches what other viewers show; the targets themselves are still dropped with the existing warning, now naming
  the target count. Runtime morphing is a follow-up for the render owner (see Decisions).
- **Dropped attributes warn:** COLOR_0, TEXCOORD_1+, `cameras` and `KHR_lights_punctual` each get one warning per file
  naming the feature and the first pointer (step 4 turns cameras and lights into prefab entities).
- **Files.** `gltf_importer.cpp`, `tests/asset/test_cook_gltf.cpp` (new cases), `docs/engine/` asset format notes if
  they list supported features, `changelog.d/`.
- **Tests.** Each feature built in-test as JSON + base64 buffers (`make_glb` already exists): a sparse POSITION equals
  the dense cook byte for byte; a quantized cube equals the float cube within one snorm16 step; a meshopt-compressed
  cube (encoded in-test with `meshopt_encodeVertexBuffer` / `IndexBuffer`) equals the uncompressed cook byte for byte;
  8 weights reduce to the expected 4; draco-required is an error with the advice text; each dropped attribute warns.
  Determinism: every new fixture cooked twice gives identical bytes.
- **Acceptance.** Existing glTF cooks unchanged (step 0's comparison stays green); the cases above pass.
- **Risks.** Real exporter output (Blender, Maya, gltfpack) is not available in the container: fixtures are
  hand-written to the spec, so exporter quirks are untested. The meshopt decoder's input is untrusted bytes: add the
  new path to the M-FUZZ survey.

### Step 2 — OBJ (+MTL)

- **Scope.** `v`, `vt`, `vn`, `f` (triangles, quads and n-gons fan-triangulated; negative indices), `o` / `g` as
  nodes, `usemtl` as submeshes, `mtllib` read as a sibling `.mtl` (the cook reads the dependency through the same
  file-dependency mechanism the material importer uses; *(guess: confirm it allows a non-asset sibling)*). MTL:
  `Kd`, `d` / `Tr`, `Ns` → roughness, `Ke`, `map_Kd`, `map_Bump` / `norm`, `map_Pr` / `map_Pm` (PBR extension) as
  inline materials, textures resolved to GUIDs like glTF images. Ignored with a warning: `l`, `p`, curves, `s`
  groups (normals are generated per face-corner angle when `vn` is absent), vertex colours after `v x y z`.
- **Axis and units.** OBJ has neither: sidecar `"up": "y"|"z"` (default y) and the existing `"scale"`.
- **Files.** `src/engine/asset/cook/src/obj_importer.cpp` (own parser, no dependency, locale-free number parsing
  with the cook's JSON number reader or `std::from_chars`), registered in `importer_registry.cpp`; the factory is
  declared in a cook-private header (not `importer.h`, 169 TUs); `.obj` added to `tools_check.cpp:87` and
  `tools_docs.cpp:728` lists (editor, one line each); `tests/asset/test_cook_obj.cpp`.
- **Tests.** In-test text fixtures: a quad cube equals the same cube written as glTF (same `CHNK_MESH` bytes, both
  through step 0's back end); n-gon fans; missing `.mtl` is a warning, missing texture an error naming the line;
  `lod_levels` and `max_triangles` work. Determinism: cooked twice, identical bytes; CRLF and LF give the same bytes.
- **Risks.** OBJ dialects vary (3ds Max, ZBrush): the parser errors with file:line on anything it cannot read.
- **As built (PR pending).** The *(guess)* about a non-asset sibling was wrong: an importer sees only registered
  assets (`IAssetLookup::guid_for_path` / `source_bytes`), and the re-cook key hashes a dependency's *output*. So the
  `.mtl` is an asset of its own: importer `mtl` (`.mtl` -> the `CHNK_MAT` of its first `newmtl`, with a fingerprint of
  the whole file in the chunk's unused `user_flags`, so a change to any material re-cooks the models), declared by the
  `.obj` as a dependency together with the textures its materials name. An `.obj` whose `mtllib` is missing or not
  adopted cooks with a warning and default materials. Both factories are in the cook-private `obj_importer.h`;
  `importer.h` is unchanged. Also: V of `vt` is flipped (OBJ's origin is the bottom left); `Pr` / `Pm` scalars are read as
  well as the maps; a texture's `-s -o -bm ...` options are skipped with a warning; the editor's unused-asset lint counts
  `.obj` / `.mtl` text as a model that names textures (`tools_check.cpp`). Limits: 256 MiB file, 16 MiB line, 16M
  v / vt / vn, 32M face corners, 1M-vertex polygon, 1M nodes, 256K submeshes.

### Step 3a — FBX: ufbx, the pin and the wall

- **Library.** ufbx (github.com/ufbx/ufbx), one `ufbx.c` + `ufbx.h`, no dependencies. License read from the repo's
  `LICENSE` on 2026-10-17: dual, MIT (Copyright (c) 2020 Samuli Raivio) **or** public domain (Unlicense), user's
  choice. The master header reads `UFBX_HEADER_VERSION 0.23.1`; pin a tagged release by commit hash (the tag list was
  not reachable from this container: confirm the tag and its hash when pinning).
- **Files.** `docs/adr/0047-fbx-ufbx.md` (FetchContent per ADR-0017; why ufbx over Assimp and the Autodesk SDK:
  license, size, no system install); `cmake/Dependencies.cmake` (`mce_ufbx` static, warnings off, built like
  `mce_tinyexr`, `-ffp-contract=off` / `/fp:precise`); `scripts/ci/check_walls.py` "ufbx wall" (`ufbx.h` and
  `ufbx_\w+(` only in `fbx_importer.cpp` / `ufbx_impl.c`) and a link wall (`mce_ufbx` only under
  `src/engine/asset/cook/` and `tests/asset/`), like the bc7enc pair; a `docs/ARCHITECTURE.md` §7 row ("FBX import").
- **Tests.** `tests/asset/test_ufbx_smoke.cpp` loads an ASCII FBX 7.4 cube written in-test from a string.
- **Risks.** A new dependency is the coordinator's call; ufbx's own float paths (unit and axis conversion,
  `ufbx_evaluate_*`) must be checked for determinism across hosts (Linux and Windows cook the same bytes).
- **As built (PR pending).** ufbx **v0.23.1**, tag commit `26a482ae66871d7de36eb722aa060bce95bce274` (2026-09-25; tags
  read with `git ls-remote`, licence read from the pinned `LICENSE`: MIT or Unlicense, unchanged). The ADR is
  [ADR-0047](../adr/0047-fbx-ufbx.md): 0046 is the road-network ADR of an in-flight M-OPENWORLD PR. `mce_ufbx` compiles
  `src/engine/asset/cook/src/ufbx_impl.c` (which includes `ufbx.c`, like `tinyexr_impl.cpp`) with `UFBX_NO_FORMAT_OBJ`
  and `UFBX_NO_GEOMETRY_CACHE`; ufbx has no threading switch and runs single-threaded unless given a thread pool.
  `tests/asset/test_ufbx_smoke.cpp` (its own small binary, links `mce_ufbx` only) loads the ASCII cube (8 vertices,
  6 quads, 12 triangles), checks two loads byte-equal raw and with Z-up centimetres converted to +Y up metres
  (`MODIFY_GEOMETRY` scales the vertices; the axis turn stays in the node's adjust rotation, so 3b reads
  `geometry_to_world` or the adjust transforms, not the vertices alone; the option names in 3b's "Unit and axis"
  exist as written in v0.23.1), and runs every truncation, 400 seeded byte
  mutations and a flip at every byte of a binary file. Findings for 3b: without `memory_limit` a 150-byte binary file
  claiming 2^29 doubles makes ufbx ask for 4 GiB (with 64 MiB limits it is `UFBX_ERROR_MEMORY_LIMIT`); ASCII `*N`
  counts are ignored (the values present are read). Cross-host: parsing, unit and axis maths are IEEE-exact with
  `-ffp-contract=off`; Euler rotations and animation evaluation call libm `sin` / `cos` / `atan2` (ADR-0047
  "Determinism"), so FBX goldens are per host until checked, with `UFBX_EXTERNAL_MATH` as the escape hatch.

### Step 3b — the FBX importer

- **Scope.** Meshes (polygons triangulated by `ufbx_triangulate_face`, deterministic), normals and UV set 0,
  materials (ufbx's PBR mapping: base colour, roughness, metalness, emissive, normal map) inline as in glTF, the node
  tree, skins as clusters (more than 4 influences reduced as in step 1, 256 joints), bind poses as inverse binds,
  animation stacks as clips (sampled at a fixed rate, sidecar `"sample_rate"`, default 30; no curve fitting), embedded
  textures an error with "export with external textures" (the cook resolves textures to GUIDs).
- **Unit and axis.** `ufbx_load_opts.target_axes` = the engine's (+Y up, right-handed, metres) and
  `target_unit_meters = 1`, with `space_conversion = UFBX_SPACE_CONVERSION_MODIFY_GEOMETRY` so nodes keep identity
  roots (option names to check against the pinned version); the sidecar `"scale"` multiplies on top. Blender's 0.01 / 100 scale pair and 3ds Max Z-up are the cases to
  test.
- **Files.** `src/engine/asset/cook/src/fbx_importer.cpp` (`ufbx_impl.c` landed in 3a), registry and the private factory header,
  the two editor extension lists (`.fbx`), `tests/asset/test_cook_fbx.cpp`.
- **Tests.** ASCII FBX fixtures generated in-test (cube in cm and Z-up equals the glTF cube in m and Y-up within a
  quantization step; a 3-bone skin with one clip whose sampled poses match the analytic ones; binary FBX via a
  small generator in the test, or ufbx's own test files only if a size-checked copy (< 20 KB) is allowed). Determinism:
  each fixture cooked twice, identical bytes.
- **Risks.** No DCC exporter in the container, so real Maya / Blender / Max files are unverified; ask the user for
  two or three real files before the close. FBX animation layers and blend modes beyond the base layer: warning.
- **As built (PR pending).** `fbx_importer.cpp` (+ cook-private `fbx_importer.h`), registered for `.fbx` where `mce_ufbx`
  exists (`MCE_HAS_FBX`, host builds); `.fbx` joins the editor's media list and the unused-asset lint's model case.
  ufbx options: `target_axes = ufbx_axes_right_handed_y_up`, `target_unit_meters = 1`, `space_conversion` and
  `geometry_transform_handling = MODIFY_GEOMETRY`, `inherit_mode_handling = HELPER_NODES`,
  `handedness_conversion_axis = X`, `node_depth_limit = 4096`, `clean_skin_weights`, `use_blender_pbr_material`,
  `file_format = FBX`, both allocators limited to 1 GiB plus an allocation cap, `ufbx_load_memory`, no thread pool.
  The axis turn stays a rotation on root-level nodes (`MODIFY_GEOMETRY`), which `ufbx_evaluate_transform` includes, so
  the node tree and animation are read as they are; the Blender 0.01 / 100 pair and Z-up centimetres equal the glTF cube
  within one quantization step; a left-handed file is mirrored with the winding fixed. Skinned geometry goes to the bind
  pose's world space and the inverse bind is the inverse of the joint's bind world matrix (glTF's convention). Clips:
  `N = ceil(L * rate - 1e-4)` steps, key times `i * (1 / rate)`, the last key at the stack end; animated nodes are found
  from the base layer's transform properties; a channel is written only when it differs from the rest pose. Plain FBX
  `TransparencyFactor` is read as `1 - opacity`. Findings: ufbx reads the cluster `Transform` as the mesh-node-to-bone
  matrix (not the mesh's world matrix), and a base layer with no `BlendMode` is "additive" to ufbx, which is not worth
  a warning on its own (only a weight other than 1 or an animated weight warns). Embedded textures, hidden meshes, blend
  shapes, extra layers, non-base-transform animation and unsupported content warn once per file (see guide 8.4).
  Tests: `test_cook_fbx.cpp` (about 9 s with the rest of `test_cook`). Open: no real DCC file was tested; FBX
  cooks are per host (ADR-0047); the FBX fixtures are not in the M-FUZZ survey yet; the caps (256K nodes, 16M vertices)
  are checked only by their code path, not at size.

### Step 4 — lights, cameras, the MCP tool, the report, the guide, the close

- **Lights and cameras.** A cook importer writes one `CHNK_MESH` and cannot place entities, so
  `mce import gltf <file>` / `mce import fbx <file>` (new `tools/mce_cli/mce_import_3d.py`, dispatched from
  `mce_import.py`; FBX through the cook's own reader: *(guess: a `mce_cook --dump-fbx-scene` JSON, to avoid a second
  FBX parser in Python)*) writes `<name>.mcprefab`: a root entity with `MeshRef` to the model, children with
  `PointLight3D`, `SpotLight3D` or `Camera3DComponent` at the node's world transform. Directional lights: a warning
  naming `Environment3D.sun_direction` and the direction found. Intensity mapping (candela / lux to the engine's
  0..100) is a fixed documented formula. Same rules as M-IMPORT: same bytes twice, kept GUIDs, named warnings,
  `--werror`.
- **MCP.** `asset_import` `tool` enum gains `gltf` and `fbx` (`tools_import.cpp`, `tests/mcp/test_mcp_import.cpp`);
  the transcript golden changes in that schema only (the integration runner regenerates it).
- **Import summary.** Per mesh, in `CookReport` (`cook.h`, 140 TUs, under the 150 line but check again; if over,
  a seam PR): triangles, submeshes, materials, joints, clips, LOD levels, warnings count; `mce_cook` prints one line
  per model, `cook_run` returns `meshes`.
- **Docs and fixtures.** User guide 8.4 (formats, sidecar keys, what is dropped and why), 5.3 and 14 for the tool;
  `samples/import_demo` gains a generated `.obj` and an ASCII `.fbx` written by its `gen_art.py` (no binaries), and
  its `import_demo_reimport` check covers the prefab. The record closed with "as built".

## Decisions (approved by the coordinator)

1. **FBX through ufbx**, pinned, cook-only, behind a wall, with an ADR (step 3a). The import guide also carries a
   short "convert via Blender" fallback note.
2. **Vertex colours, TEXCOORD_1 and runtime morph targets**: this milestone only warns for them and bakes the default
   morph weights. A render-owned milestone (a `MeshFlags` extra vertex stream, morph deltas, a vertex or compute path)
   sits in the ROADMAP backlog and starts only if a user's content needs it.
3. **KHR_draco is refused** with a re-export hint; EXT_meshopt_compression is supported.
4. **Review:** the Opus review of steps 3a and 3b also covers malformed-file limits and fuzz-style corrupt-input tests
   (as in the HDR/EXR follow-up). A step that changes a header ships that header as its own seam PR first.
5. Stay off `render/`, the script VM and perf files.

Also worth a look: lights and cameras land as a converter-written prefab (step 4), not inside the mesh asset.

**Not in scope:** USD / USDZ and Alembic (each a large dependency; revisit on demand); Datasmith-style CAD import;
FBX export; embedded FBX textures; NURBS and curves; automatic collision at cook (no cooked collision asset exists:
`MeshCollider3D` reads a mesh asset at Startup, and a separate simplified collision mesh can already be a second
model file or a `lod_levels` export; a cooked convex-hull asset would be a physics3d change); live re-import in the
editor; anything in `render/`.

**Gates:** every mesh cooked before step 0 cooks to the same bytes after each step unless the step names it; every new
fixture cooks to identical bytes twice; every dropped feature is a named warning; the ufbx and link walls in the walls
stage; replay hashes unchanged (meshes are render input; colliders built from them unchanged by step 0); `local_ci.sh
--task` green on every step.

**CI notes.** No public header changes in steps 0 to 3: new factories go in a cook-private header, so `importer.h`
(169 TUs) and `formats.h` (318) stay as they are; a step that must touch either ships the header as its own seam PR
first. The FBX and OBJ suites must stay under 60 s (small in-test fixtures); a longer one gets a smoke variant and
`mce_mark_slow`.
