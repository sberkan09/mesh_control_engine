# M-FUZZ — Hostile input: loaders and parsers never crash (milestone record)

**Status:** done (added 2026-10-16; steps 1 to 6 done). Plan: ROADMAP §M-FUZZ; Linear SBA-185 … SBA-188.

**Why:** a shipped game opens packs, saves, recordings and downloaded mods it did not write, and the editor and
MCP server open files agents wrote. Today two parsers have a mutation test (the `.mce` container, 1,000 byte
mutations, and the MCP request path). Every other reader trusts its input after a header check, and a bad length
or count there is a crash, a hang or an out-of-bounds read that ASan would find only if something fed it one.

It needs no device: a deterministic mutator, ctest targets and fixes with regression tests.

**Not in scope:**
- A coverage-guided fuzzer (libFuzzer, AFL): the tests here are seeded and replayable in `local_ci.sh`.
- Fuzzing the GPU drivers, the OS or third-party libraries beyond the calls the engine makes into them.

## Steps

| # | Step | Linear | State |
|---|---|---|---|
| 1 | The mutator and a survey of every parser of untrusted bytes; the asset module's 19 chunk readers over mutated payloads of every cooked sample chunk | SBA-185 | done |
| 2 | Walk what the readers accept (every view's arrays and offsets) so ASan sees out-of-bounds reads; the readers outside the asset module (UI, string tables, catalogs, input map); fix the findings. The nav mesh load: fixed in 2b (integrity hash) | SBA-186 | done |
| 3 | The importers (png, gltf, ogg, mcskel2d, mcanim, mcterrain, mcbt, mcui, timelines, tilemaps) and scene instantiate; saves, replays, input recordings, `.mcperf`, net packet decoders, the Luau compile path | SBA-187 | done (3a #424, 3b #501 and #520, 3c #533) |
| 4 | The targets in the ASan stage with a time budget; a walls check that a new chunk type has a fuzz target; user guide note; the close | SBA-188 | done (#552) |
| 7 | The editor's scene document reader (`SceneDocument::parse`, with the M-SCENEMIGRATE rename tables set): mutated and value-swapped text; what loads must save, load again and survive lint, stats and find | added 2026-10-26, done (#806) |
| 8 | The scene repair (`fix_scene_lint`, M-LINTFIX) over the same mutants: lint is clean afterwards, a second pass fixes nothing, the repaired text loads again | added 2026-10-27, open (PR) |

## Step 1 — Mutator and survey (done, SBA-185)

- **Mutator.** `tests/fuzz/mutator.h`: a splitmix64 stream; each mutant has 1–4 damages (a random byte, a bit
  flip, a 32-bit field set to an edge value such as 0, 1, 0x7FFFFFFF, 0xFFFFFFFF, a zeroed range, a copied range,
  two swapped bytes) and one in six is also truncated. The copy sits in 8-byte aligned storage, as a real chunk
  does. A mutant is a pure function of (pack path, chunk index, iteration), so a failure replays from the
  `INFO` line the test prints.
- **Test.** `tests/fuzz/test_chunk_readers.cpp` (`ctest -R test_fuzz`, 3.5 s): every `assets.mce` under the
  build tree's `samples/` and `projects/` (28 packs), every chunk of a type with a `read()` (17 types present),
  64 mutants per chunk up to 64 KB, 16 up to 1 MB, 4 above: 22,680 mutants. A reader may return an error or a
  view; it must not crash, hang or fire an `MCE_ASSERT` (the test main turns an unexpected one into a failure).
  The run checks it covered at least 10 chunk types.
- **Result.** No finding: the 19 readers (`ManifestView`, `TextureView`, `SfxView`, `VideoView`, `ScriptView`,
  `FontView`, `MaterialChunk`, `TilemapView`, `AnimGraphView`, `TerrainView`, `ParticlePresetChunk`,
  `SequenceView`, `TimelineView`, `BtView`, `Skeleton2DView`, `ShaderView`, `MeshView`, `SceneView`,
  `ModInfoView`) check their headers and bounds. About 64% of the mutants still read without an error, which
  means a reader accepts them and the damage sits in what a user of the view reads next: that is step 2.
  No sample pack contains a Shader or ModInfo chunk, so those two readers ran only on the untouched path.

## Step 2 — Walk what the readers accept (done, SBA-186)

- **Walkers.** `tests/fuzz/view_walk.h` has one `walk` per view: after `read()` accepts a mutant, it follows what
  a consumer follows (string offsets of timelines, behavior trees and skeletons; the slot, attachment, vertex,
  weight and index ranges of a skeleton; submesh indices into vertices, skin joints, LODs, node names, clip
  channels and key pools of a mesh; the rows, data and fixups of a scene; state, transition, blend and layer
  ranges of an animation graph; terrain height and weight lookups; font and kerning lookups; shader variants
  and `compose`; texture mips; the Vorbis stream inside a sound) and folds every value into a checksum, so the
  reads cannot be optimised away. Under the ASan stage a stray offset is an out-of-bounds report.
- **Decoders outside the asset module.** The same loop now covers `SpriteSheetView`, the string table, input map,
  store catalog, remote config, game services catalog, UI theme and UI document (decode, then
  `build_document`, as the runtime does) and the nav mesh header check. The corpus is every `.mce` under the
  build tree's `samples/`, `projects/` and `template_probe/`: 604 files, 25 chunk types, 48,000 mutants,
  about 64% accepted.
- **Result.** ASan clean, no crash in the readers or the walkers. One finding: the nav mesh `load` crashed on a
  damaged bake; fixed in 2b below. A first walker crashed on a
  non-mesh skeleton attachment; that was the walker's mistake (only a mesh attachment owns vertices), not the
  reader's, and is commented in the walker.
- **Not covered yet.** A harness check that an injected bug is found; the importers and scene instantiate (step 3).

### Step 2b — Nav mesh bake integrity (done)

`NavMesh3D::load` hands the compressed tile layers of a bake to Detour's tile cache, which trusts them: a bake
with damaged layer bytes passed `navmesh_blob_valid` (header and sizes) and then divided by zero inside
`simplifyContour` (found by the nav chunk in `test_fuzz`, SIGFPE).

- **Hash.** Blob version 2 ends with an FNV-1a 64 of every byte before it. `navmesh_blob_valid`, `load` and the
  `.mcnav` importer refuse a blob whose hash does not match; `NAVMESH_VERSION` and the importer version are 2
  (cooked caches rebuild), and `samples/07_gameplay3d/.../nav_13.mcnav` was upgraded in place (version field
  and trailer; the bake is otherwise the same bytes). A version-1 file is refused with "re-bake the navmesh".
  The hash catches truncation, bit rot and bad downloads, not a forger who re-hashes.
- **Layer checks (review of the first version).** The hash alone leaves the root cause: a re-hashed blob still
  reaches Detour. `navmesh_blob_valid` therefore decodes every layer the way the tile cache will and refuses
  it unless its header is Detour's (magic, version), its size and usable window are consistent, its tile
  coordinates and layer index are inside the blob's grid, every area id is below 64, and every connection bit
  stays in the grid, is mutual and joins two walkable cells (the contour walk follows connections while the
  region builder ignores them; an inconsistent one reads past the region array or leaves a region with no
  edge, and the simplifier divides by its vertex count). A layer that passes is one the builder can walk.
- **Test.** `test_navmesh.cpp` flips bytes in the header, the layers and the trailer, edits the layer header
  (magic, version, tile x and y, width, window) and the decoded grid (a connection toward x − 1 at x = 0, an
  area id 200) with the hash made right again, and expects a refusal from `navmesh_blob_valid` and `load`; the
  committed sample bake loads; a version-1 file is refused with the re-bake message. `test_cook_navmesh.cpp`
  covers the importer: a good bake cooks; a flipped byte, a version-1 file, a 12-byte fragment and a wrong magic
  stop with a message. `test_fuzz` re-hashes half of the nav mutants (4,000 per chunk) and loads and queries
  them; with the layer checks removed that run crashes with SIGFPE, with them it is clean (also at 40,000).

## Step 3a — Player data, net, Luau (SBA-187, #424, merged)

- **`test_fuzz_player_data`.** Saves and replays are mutated and *re-sealed* (the xxh64 body hash and the stream
  hash are written again) so the parsers behind the hash are reached; the raw input recording goes through
  `InputReplayer` and the input script text through `parse` and evaluate. No finding at 60,000 mutants (plain) and
  8,000 (ASan).
- **`test_fuzz_net`.** Full and delta replication snapshots (with hooks), acks, two rollback sessions fed mutated
  packets, and the relay (room create, join, data, kick, quick play, list) through the simulated network with raw
  endpoints. No finding; the run checks that the relay forwarded and dropped messages, so it is not idle.
- **`test_fuzz_script`.** Mutated Luau source through `check` and `compile`: no finding.
- **Finding: Luau bytecode is trusted by the loader.** A mutated chunk reaches `luau_load` and trips
  `LUAU_ASSERT(instruction[1] < sizek)` (`lvmload.cpp:691`) in debug builds; in release the assert is compiled out
  and the loader indexes the constant table out of bounds. The pack CRC does not help (anyone who edits a mod
  recomputes it). The case is `fuzz: Luau bytecode survives mutation in the host`, skipped unless
  `MCE_FUZZ_BYTECODE=1`. Options for the owner of M-SCRIPT: (a) mods ship source and the host compiles it at load
  (breaks the "runtime parses no text" rule, but only for mods); (b) a bytecode verifier of our own in front of
  `luau_load`; (c) a signed or hashed script chunk that only the cook can produce; (d) accept the risk for
  first-party packs and refuse bytecode from mods. Not decided here.

## Step 3b — Importers (SBA-187, #501; font hardening #520)

- **`test_fuzz_importers`.** Every built-in importer is called directly (`scan_dependencies`, then `cook`) over mutated
  copies of up to six committed sources per extension (25 extensions: the native text assets, png, wav, mpg, csv,
  luau, ...). Text assets get two kinds of mutant: raw byte damage, and valid JSON with one value replaced by another
  kind (a string for a number, `null`, `[]`, an out-of-range number, a nil GUID), which is what reaches the importer's
  own type and range checks. No crash at 1,500 mutants per file (plain) and 200 (ASan). The lookup is empty, so
  every reference dangles.
- **Finding: the glTF importer asserts on a wrongly typed value.** `read_indices` (`gltf_importer.cpp:447`) calls
  `as_number()` on an accessor index it has not type-checked in the `ctype == 5125` branch (`bufferView`, `buffer`);
  in release the same read is out of bounds on the JSON node array. Owner: M-IMPORT3D (asset/cook mesh import).
  **Fixed in M-IMPORT3D 1:** the accessor reader is type-checked end to end (a systematic type-confusion mutation test covers the whole indices chain) and the `.glb`/`.gltf` files are fuzzed by default again (`MCE_FUZZ_GLTF` is gone).
- **Finding, fixed in 3b2: a mutated TrueType font crashes the font importer.** `stbrp_pack_rects` in the atlas bake got
  a negative width from a damaged glyph box (`font_importer.cpp`). stb_truetype trusts its input, so `font_structure_ok`
  now walks what it reads before it is called: table directory and bounds, head/maxp/hhea/hmtx, the one cmap subtable
  stb selects (search fields, group and range bounds), loca/glyf (flags and coordinates fit, points inside the header
  box, component nesting and expanded point count capped, no point matching or scaled components) and the kern table
  it reads. The importer also skips codepoints whose glyph id is outside the font, refuses a glyph whose box is inverted
  or scales past 1024 px (256 for a distance field), and skips with a warning a glyph (coverage bake only) whose outline rises above the box the
  font gives it (the rasterizer assumes the outline lies inside the bitmap sized from that box). Refused on purpose: CFF
  (`OTTO`) outlines, because stb sizes the CFF buffer at 512 MB. An independent review found the first version's gaps (cmap glyph ids, nesting memo,
  fan-out, SDF size, CFF, format 4 and 0 header fields); each has a case in `test_cook_font_damage.cpp`, which also
  builds small fonts by hand. A second review round added the cost cap (outline points plus component records,
  because stb_truetype keeps no memo), the kern cap, a 256 px glyph limit for the distance-field bake and a 16-byte
  minimum. Known residual: a distance-field bake of a hostile font with thousands of 256 px glyphs of thousands of points
  is slow (time, not memory); the cook is a developer-side tool. A first version compared the outline with the box inside
  the validator and refused scaled components; the project font NothingYouCouldDo (outline up to 9 units outside its
  boxes, scaled components) failed the cook, so the comparison moved to the importer, on the shapes stb itself builds. `.ttf` fuzz: 11,252 mutants under ASan, clean.

## Step 3c — Command line, `.mcperf`, WebSocket, scene instantiate (SBA-187)

- **`test_fuzz_misc`.** `parse_command_line`, `apply_deferred_command_line` and `console::execute` over random argv built
  from every registered convar and function name plus junk values (40,000 lines); `.mcperf` captures (`write_mcperf`
  output) as mutated text and as valid JSON with one value swapped, read with `read_mcperf` and summarized (40,000
  mutants). No finding.
- **`test_fuzz_websocket`.** A raw TCP client sends the host a damaged handshake and damaged frames (half of the mutants keep
  the handshake sound so the frame reader is what runs), and a raw TCP server answers the host's own client with the
  right accept key and damaged frames, or a wholly damaged answer. 3,000 connections each way, no finding. POSIX only; the
  socket wall allows this one file.
- **`test_fuzz_scene`.** Scene chunks of every cooked pack, mutated and passed to `scene::instantiate` into a fresh world
  (kit components linked): 48,000 mutants, no finding. Scenes whose columns name a game's own components are refused up
  front by the registry check, as intended.
- **Step 3 is complete.** The `.mpg` case runs by default again (the pl_mpeg overflow was fixed in #521). The Luau
  bytecode loader decision and the Detour guard below remain open.

## Step 4 — The gate and the close (SBA-188)

- **ASan stage.** The fuzz tests are ordinary ctest tests, so the `linux-asan` stage of `local_ci.sh` already ran them; what
  was missing was a failure on a UBSan report (it only printed) and a bound on time. `tests/fuzz/CMakeLists.txt` now labels
  all of them `fuzz` (`ctest -L fuzz`), gives each a 600 s timeout, and in an ASan build sets
  `UBSAN_OPTIONS=halt_on_error=1` with `cmake/ubsan.supp`. The only entry is pl_mpeg's left shift of negative values
  (third-party; every compiler we use gives the arithmetic shift the decoder wants). The default iteration counts are
  fixed per test and keep the whole group near 30 s under ASan on the CI machine; `MCE_FUZZ_ITERS` is for long runs by hand.
  Checked: without the suppression the importers test fails on the first shift.
- **Walls.** `check_walls.py` fails when a `ChunkType` is not named in `tests/fuzz/test_chunk_readers.cpp` and when an
  `*_importer.cpp` has no source of any of its extensions in `samples/`, `projects/`, `templates/`, `games/` and no corpus
  in `tests/fuzz/`. Allow-lists (`FUZZ_CHUNK_EXEMPT`, `FUZZ_IMPORTER_EXEMPT`) carry the reason, and a stale entry fails
  too. First run: `Prefab` (the scene's layout, now read as a `SceneView`) and `RoadNetwork` had no reader in the test.
  Exempt for now, each with a reason: chunk types `Atlas` and `AudioPacket` (reserved, nothing reads them); importers for
  `.mcenv`, `.mchlod` and `.glsl` (no committed source; they need an embedded corpus, a follow-up).
- **Roads case** (`tests/fuzz/test_roads.cpp`, part of `test_fuzz_importers`). A crossroads with a signal is cooked
  from mutated source text (raw and value-swapped) and the `RoadNetwork` chunk is mutated byte-wise and read through
  `RoadView` with every lane, successor, conflict and phase range followed (`fuzz::walk`).
- **Finding: heap use-after-free in the road cook on a valid network (fixed concurrently, GH-538).** `roads_importer.cpp` kept
  `RoadLane& src = lanes[...]` across the loop that `push_back`s the connector lanes, so the second connector read a
  reference into a freed buffer (found by the first ASan run of the untouched crossroads). Cook-side only. My first
  fix (copy the two fields it needs) collided with the M-OPENWORLD one for GH-538, and I took theirs. Regression: the roads
  case under the ASan stage (a plain build does not see it).
- **Convar defaults (`ConvarDefaults`, M-WORKFLOW 7).** The new wall failed on this chunk type and its importer the first time it ran after merging `dev_claude`, which is what it is for. `read_chunk` now reads it through `console::set_defaults` from a temp file, and `tests/fuzz/test_defaults.cpp` mutates a `.mcdefaults` source (raw and value-swapped) and the cooked chunk (3,000 and 6,000 mutants under ASan, no finding). `import_support.h` holds the importer helper shared with the mesh cases.
- **Mesh importers (M-IMPORT3D's, added at the coordinator's request).** The ASCII FBX (`quad.fbx`, ufbx), the MTL and the glTF of
  `samples/import_demo` are picked up by the importers test as seeds with no change (300 mutants each under ASan, clean).
  `tent.obj` was git-ignored at first, so a clean clone failed `import_demo_reimport` (fixed concurrently, GH-399, which tracks it); it is an importers seed too. `tests/fuzz/test_mesh_import.cpp` builds a corpus of its own as well: a tent OBJ with a material
  library (byte damage and swapped lines), and two GLB grids whose position and index buffers are `EXT_meshopt_compression`
  streams (one with the EXPONENTIAL filter) mutated byte-wise, which reaches the damaged streams and the damaged
  `count` / `byteStride` / `mode` / `filter` fields. 12,000 mutants of each under ASan: no finding. The "meshopt wall"
  allows this file for the encoder.
- **User guide.** 5.10 "Damaged and hostile files" (`docs/user-guide/05-assets.md`) and a limit in 19.4.
- **Milestone state.** Steps 1 to 4 are done. The corpora for the `.mcenv`, `.mchlod` and `.glsl` importers came after
  (step 5, below), then the Detour guard and a long run (step 6); `Atlas` and `AudioPacket` stay exempt as reserved types.

## Step 5 — Corpora for the three exempt importers (no Linear card; the record is the tracker)

`tests/fuzz/test_binary_import.cpp` (in `test_fuzz_importers`) builds what no sample ships:

- **`.mcenv`:** an 8 x 4 x 2 RGBM environment from `write_envmap`, mutated byte-wise (600 mutants by default).
- **`.mchlod`:** the mesh chunk a tent OBJ cooks to, wrapped by `write_hlod_file`. Half of the mutants have their trailing
  hash recomputed so the damaged payload reaches `MeshView::read` instead of stopping at the hash check.
- **`.glsl`:** a fragment stage with a variant, a uniform block and a sampler. Byte mutants (almost none stay valid) and line
  mutants (a dropped or repeated line, changed binding or location digits; about 40% still cook), so glslang and the
  binding-bank check see damaged source.
- The three entries left `FUZZ_IMPORTER_EXEMPT` in `check_walls.py`, which is now empty.

**Finding (fixed):** UBSan, `shader_importer.cpp`: a stage that binds nothing has an empty resource list, and `memcpy` from
its null `data()` (zero bytes) is undefined; the copies now check for empty. Found by the first ASan run of the line
mutants. A long run (`MCE_FUZZ_ITERS=4000`, 18,000 mutants) is clean under ASan with UBSan fatal.

### Open items from step 3

- Luau bytecode loader trusts its input (3a). Decided and pinned (coordinator 2026-10-17; M-SCRIPT3 step 8): bytecode loads
  only from the game's own cooked pack, `check_walls.py` fails a new mount site that allows code or an `add_bytecode`
  caller outside the trusted places, and mods, base packs, fetched config and user files stay data-only. The loader itself
  is not hardened, so the `MCE_FUZZ_BYTECODE` case stays off by default (it calls `add_bytecode` as a trusted caller would).

### Step 6 — Detour defense in depth (done)

`simplifyContour` in DetourTileCache takes `% cont.nverts` and `% cont.npoly` and reads vertex 0 without checking
how many vertices the contour walk made. Step 2b's `layer_valid` stops the layers that cause a contour of fewer than
two points; the second line does not depend on it: `cmake/patch_recast.cmake`, a FetchContent `PATCH_COMMAND` in
`Dependencies.cmake` (the pl_mpeg precedent), makes `simplifyContour` return an empty contour (`nverts = 0`, which
the caller already stores) when it has fewer than two points. A patch rather than a wrapper survives a
cache-cleaned FetchContent and the dependency cache key (the hash of `Dependencies.cmake`), and it fails loudly when
the anchor text changes with a Recast upgrade.

**Checked both ways.** With `layer_valid` reduced to its decode (every connection and area check removed) and an
unpatched Recast, `test_fuzz` at 4,000 iterations dies with SIGFPE in the nav mutants; with the patch the same run
passes. With the checks back (the committed code) a long run (`MCE_FUZZ_ITERS=40000`: 69,432 mutants over 733 packs)
is clean. The patch covers the division only: a connection that leaves the grid is still `layer_valid`'s to refuse.

### Step 7 — TGA, BMP, MP3 and FLAC seeds (M-IMPORTMEDIA 3, done)

`tests/fuzz/test_media_import.cpp` (in `test_fuzz_importers`): the samples ship none of these formats, so the corpus is
a raw, an RLE and a 32-bit TGA and a 24-bit BMP built in the test, plus `tests/asset/data/audio/tone.mp3` and `tone.flac`.
Each seed gets (a) a header sweep: each of its first 48 bytes set to 0x00, 0x01, 0x7f, 0x80 and 0xff, which reaches the
dimension, bit depth, sample rate, channel count and frame-size fields and so the 16384-a-side and frame caps; (b) every
prefix up to 56 bytes, the truncation cases; (c) byte mutants (300 by default, `MCE_FUZZ_ITERS`). No finding: 4,000
iterations (25,800 mutants) are clean in the debug build. The ASan stage runs it with the rest of the importers.

### Survey: parsers of untrusted bytes

| Entry point | Where | Input comes from | Mutation coverage today | Step |
|---|---|---|---|---|
| `.mce` container open | `asset/container.h` `ContainerView::open` | any pack, mod | 1,000 mutations (`test_container`) | done |
| 19 chunk readers | `asset/formats.h` `*::read` | packs, mods | mutated payloads, accepted views walked (`test_fuzz`) | done |
| UI document, theme | `ui/document_data.h` `decode_ui_document`, `decode_ui_theme` | packs | mutated, then `build_document` (`test_fuzz`) | done |
| String tables | `loc/string_table.h` `decode_table` | packs, mods | mutated (no sample pack has one yet; `test_fuzz` reads it when present) | done |
| Store, remote config, game services catalogs; input map | `monetize/catalog.h`, `liveops/remote_config.h`, `liveops/game_services.h`, `input/input_map.h` | packs | mutated (`test_fuzz`) | done |
| Nav mesh | `physics3d` `.mcnav` bytes | packs | mutated, `load` and queries, half re-hashed (`test_fuzz`); damage refused by the hash, layers by the layer checks | done |
| OBJ, FBX, MTL, glTF with meshopt streams | `obj_importer.cpp`, `fbx_importer.cpp` (ufbx), `gltf_importer.cpp` | importing source files | mutated (`test_fuzz_importers` seeds from `samples/import_demo`, `test_mesh_import.cpp`) | done |
| Item catalog | `asset/items_format.h` `decode_item_catalog`; `.mcitems` importer | packs, mods, project sources | mutated source (bytes, valid JSON with a value swapped) and mutated chunks, read back and re-checked (`test_items.cpp`, `test_chunk_readers`) | done |
| Road network | `asset/roads_format.h` `RoadView::read`; `.mcroads` importer | packs, mods, project sources | mutated source and chunk (`test_roads.cpp`); every range followed | done |
| Scene instantiate | `scene/scene.h` `instantiate` over a `SceneView` | packs, mods | mutated scene chunks of every cooked pack, spawned (`test_fuzz_scene`) | done |
| TGA, BMP, MP3, FLAC | `texture_importer.cpp` (stb_image), `audio_decode.cpp` (dr_mp3, dr_flac) | importing source files | built seeds (raw, RLE and 32-bit TGA, 24-bit BMP) and the committed tone clips; header-byte sweep, every short prefix, byte mutants (`test_media_import.cpp`, in `test_fuzz_importers`) | done |
| PNG, WAV, MPEG-1, Vorbis stream | importers of `cook/`; `asset/vorbis.h` `decode_vorbis` | importing source files | mutated bytes (`test_fuzz_importers`); Vorbis also inside a sound chunk | done |
| glTF, TrueType fonts | `gltf_importer.cpp`, `font_importer.cpp` (stb_truetype) | importing source files | mutated (`test_fuzz_importers`, `test_cook_font_damage`); both findings fixed | done |
| Text assets (`.mcscene`, `.mcprefab`, `.mcanim`, `.mcskel2d`, `.mcterrain`, `.mcbt`, `.mcui`, timelines, `.mcsound`, ...) | cook JSON reader and per-type validators | project sources, agents | mutated text and valid JSON with one value swapped (`test_fuzz_importers`) | done |
| Save files, replays | `ecs/save.h` `read_save`, `read_replay` | the player's disk | mutated and re-sealed (`test_fuzz_player_data`) | done |
| Input recordings | `input/script.h` `parse` | tests, bug reports | raw stream and script text (`test_fuzz_player_data`) | done |
| `.mcperf` captures | `editor/core/perf_file.h` | tools | mutated text and JSON with one value swapped, then summarized (`test_fuzz_misc`) | done |
| Replication, rollback and relay packets | `net/replication.h` `read_ack`, `apply`; rollback; `net/relay.h`; `net/bytes.h` `ByteReader` | the network | mutated snapshots, acks, rollback and relay messages (`test_fuzz_net`) | done |
| Luau source | `script/` `check`, `compile` | project scripts | mutated (`test_fuzz_script`) | done |
| Luau bytecode | `ScriptHost::add_bytecode` (`luau_load`) | packs, mods | **finding, see step 3a**; test off by default; only the game's own pack may carry bytecode (M-SCRIPT3 8, wall) | closed by policy |
| MCP requests | `mcp/server.h` | agents | mutated and random requests (`test_mcp`) | done |
| Console and command line | `core/convar.cpp` | the player | random argv and console lines over every registered name (`test_fuzz_misc`) | done |
