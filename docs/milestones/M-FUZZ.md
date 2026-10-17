# M-FUZZ — Hostile input: loaders and parsers never crash (milestone record)

**Status:** active (added 2026-10-16; steps 1 and 2 done). Plan: ROADMAP §M-FUZZ; Linear SBA-185 … SBA-188.

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
| 3 | The importers (png, gltf, ogg, mcskel2d, mcanim, mcterrain, mcbt, mcui, timelines, tilemaps) and scene instantiate; saves, replays, input recordings, `.mcperf`, net packet decoders, the Luau compile path | SBA-187 | in progress (3a: player data, net, Luau done) |
| 4 | The targets in the ASan stage with a time budget; a walls check that a new chunk type has a fuzz target; user guide note; the close | SBA-188 | not started |

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

## Step 3a — Player data, net, Luau (SBA-187)

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

### Planned: Detour defense in depth (separate item)

`simplifyContour` in Detour divides by `nverts`/`npoly`; step 2b stops bad layers before them, but a guard that
does not depend on our validation (`nverts < 2`, `npoly == 0`) should exist too, by a method that survives a
cache-cleaned FetchContent (vendored copy of the file or a wrapper pre-check). After it: a fuzz long run with the
validation switched on.

### Survey: parsers of untrusted bytes

| Entry point | Where | Input comes from | Mutation coverage today | Step |
|---|---|---|---|---|
| `.mce` container open | `asset/container.h` `ContainerView::open` | any pack, mod | 1,000 mutations (`test_container`) | done |
| 19 chunk readers | `asset/formats.h` `*::read` | packs, mods | mutated payloads, accepted views walked (`test_fuzz`) | done |
| UI document, theme | `ui/document_data.h` `decode_ui_document`, `decode_ui_theme` | packs | mutated, then `build_document` (`test_fuzz`) | done |
| String tables | `loc/string_table.h` `decode_table` | packs, mods | mutated (no sample pack has one yet; `test_fuzz` reads it when present) | done |
| Store, remote config, game services catalogs; input map | `monetize/catalog.h`, `liveops/remote_config.h`, `liveops/game_services.h`, `input/input_map.h` | packs | mutated (`test_fuzz`) | done |
| Nav mesh | `physics3d` `.mcnav` bytes | packs | mutated, `load` and queries, half re-hashed (`test_fuzz`); damage refused by the hash, layers by the layer checks | done |
| Scene instantiate | `scene/scene.h` `instantiate` over a `SceneView` | packs, mods | none (the view's rows and fixups are walked) | 3 |
| PNG, glTF, Ogg Vorbis, MPEG-1 | `cook/image.h` `decode_png`; cook glTF; `asset/vorbis.h` `decode_vorbis`; video | importing source files | Vorbis stream inside a sound chunk only | 3 |
| Text assets (`.mcscene`, `.mcprefab`, `.mcanim`, `.mcskel2d`, `.mcterrain`, `.mcbt`, `.mcui`, timelines, `.mcsound`) | cook JSON reader and per-type validators | project sources, agents | none | 3 |
| Save files, replays | `ecs/save.h` `read_save`, `read_replay` | the player's disk | mutated and re-sealed (`test_fuzz_player_data`) | done |
| Input recordings | `input/script.h` `parse` | tests, bug reports | raw stream and script text (`test_fuzz_player_data`) | done |
| `.mcperf` captures | `editor/core/perf_file.h` | tools | none | 3 |
| Replication, rollback and relay packets | `net/replication.h` `read_ack`, `apply`; rollback; `net/relay.h`; `net/bytes.h` `ByteReader` | the network | mutated snapshots, acks, rollback and relay messages (`test_fuzz_net`) | done |
| Luau source | `script/` `check`, `compile` | project scripts | mutated (`test_fuzz_script`) | done |
| Luau bytecode | `ScriptHost::add_bytecode` (`luau_load`) | packs, mods | **finding, see step 3a**; test off by default | open |
| MCP requests | `mcp/server.h` | agents | mutated and random requests (`test_mcp`) | done |
| Console and command line | `core/convar.cpp` | the player | none | 3 |
