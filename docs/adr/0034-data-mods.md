# ADR-0034: Data mods, opt-in per game

**Status:** Accepted (2026-10-10). **Milestone:** post-M9 feature, requested by the user. **Requirements:**
FR-RUN-090 (now Should, data mods), N10 (narrowed to code mods). **Guide:** `docs/user-guide/19-mods.md`.

**Context.** REQUIREMENTS N10 had ruled out mod support ("Never planned"). On 2026-10-10 the user asked for mod
support that a game developer can turn on, leaving games that do not want it unaffected. The engine already had
most of what a mod needs: `AssetSystem::mount` takes several roots and lets earlier ones win on a shared GUID,
references between assets are GUIDs, and every cooked chunk reader treats its payload as untrusted bytes (the
container negative suite and fuzz tests). What was missing was a way to name a mod pack, decide which packs load,
and cook a mod against a game whose sources the modder does not have.

## Decisions

1. **Data mods only.** A mod is cooked content, never code. Game code is a native module (`.so`/`.dll`), and
   loading one a player downloaded is arbitrary code execution with no sandbox to contain it. The engine has no
   scripting VM (N2), so "code mods" would mean exactly that. Data mods cover retextures, new or changed levels,
   prefabs, sequences, tilemaps, particle presets, sounds, fonts, materials and shaders. N10 now rules out code
   mods, UGC services and in-game editors.
2. **A mod is one pack file.** `mce_cook --pack` on a project that holds a `mod.json` writes `assets.mce` with a
   `CHNK_MOD` chunk (`ChunkType::ModInfo`, appended after `Sequence`, zero GUID) right after the manifest: id, name,
   version, target game, description (`asset::ModInfoView`, bounds-checked like every chunk). The player copies it
   into the game's mods folder as `<id>.mce`. Ids are 1 to 64 bytes of `[a-z0-9_.-]`.
3. **Override and add, by GUID.** A mod asset with a game asset's GUID replaces it; a new GUID adds content. An
   override must keep the asset's type. The cook rejects a type-changing override, and `AssetSystem::mount` ignores
   one at run time (the later root keeps that GUID and a warning names it), since it would break every reference
   the game makes to that asset.
4. **Cooking against the game.** `mod.json` names the game's pack(s) in `"base"` (and `mce_cook --base` adds more).
   The cook reads only their manifests: a reference into a base asset is type-checked and needs no dependency
   edge, because the base is fixed input. Base assets are known by type only, so a mod scene cannot instance a
   game prefab at cook (prefab flattening needs the prefab's source document); it can reference one through an
   `AssetRef<Prefab>` and spawn it at run time. The editor and `mce_mcp` mount the base packs behind the mod's
   cooked tree, so a mod project opens, plays and hot-reloads like any other.
5. **Opt-in on the game's side.** `player::PlayerDesc::mods_dir` is null by default, and then no command line
   enables mods. A game turns them on by setting it (the project template's host has the two lines commented out),
   and usually `mods_game_id` too, so mods built for another game are skipped. `mce_player` sets `mods_dir = "mods"`
   because it is a development tool for any game's content.
6. **Load order.** The player lists `<mods_dir>/*.mce` and calls `asset::resolve_mods`. Without
   `load_order.txt` every pack for this game loads, ordered by file name. With it, only the listed ids load, in that
   order (one id per line, `#` comments), and a later line wins over an earlier one. Packs that cannot be read,
   duplicate ids, mods for another game and anything past 256 mods are skipped with a logged reason, and the game
   always starts. The player mounts the mods last-first, then the game's pack, so the newest mod wins.
7. **Player controls.** `-mods 0` starts a modding game without its mods (a safe mode), and `-mods_dir <dir>` points
   at another folder. Both are ignored when the game did not opt in.
8. **Game code can see the mods.** When the game takes mods, the player publishes `asset::ModList` as a world
   resource before the game module's `install()` runs: the loaded mods in load order, plus what was skipped and why.
   Resources are not part of `World::state_hash`, so the list does not change determinism hashes; the content a mod
   brings does, as any content change would.

## Consequences

- Mods are as safe as the chunk readers: a malicious pack can make an asset fail to load (the placeholder serves),
  not run code. Every reader keeps its bounds checks; `CHNK_MOD` has its own negative and mutation tests.
- A mod pack is read twice at start (once to read its identity, once by the mount, which verifies its payload
  CRC). That is boot cost proportional to mod size and acceptable for now.
- A mod cooked against one version of a game keeps working on later versions as long as the GUIDs and types it
  references still exist; if one is gone the asset that names it resolves to its placeholder.
- Saves record entity state, not which mods were loaded. Loading a save with different mods works but may show
  different content.

## Tests

- `test_asset` (`tests/asset/test_mods.cpp`): `CHNK_MOD` round trip, negative cases and 1000 mutations; mod id
  rules; `resolve_mods` without and with `load_order.txt` (file-name order, game mismatch, duplicate ids, broken
  packs, comments, unlisted and unknown ids); the overlay (override, addition, type-changing override ignored).
- `test_cook` (`tests/asset/test_cook_mod.cpp`): a mod cooks against a game pack, overrides its texture, adds a
  material over a base GUID, writes `CHNK_MOD`, and re-cooks byte-identically; a type-changing override, a
  mistyped base reference, a missing base and a bad `mod.json` are errors; `--base` works without `mod.json`'s.
- `test_editor` (`tests/editor/test_mod_project.cpp`): a mod project mounts its game behind it and a hot reload
  that changes nothing does not remount.
- ctest `player_mods` (`tests/asset/mods_player_test.cmake`): `mce_cook` builds a game and a mod over it;
  `mce_player -headless` plays the mod's scene, the game's own with `-mods 0`, and the game's own again when
  `load_order.txt` leaves the mod out; a broken pack in the folder is reported, not fatal.
