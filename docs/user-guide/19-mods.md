# 19. Mods

A game built on the engine can let its players add content: new levels, retextures, extra prefabs,
sounds, sequences. Mods are **data only**. A mod is a pack of cooked assets that loads over the
game's own pack; it never contains code. Mods are **off** unless the game turns them on, so a game
that does nothing in this chapter is unaffected. The design record is ADR-0034.

## 19.1 Turning mods on (game developer)

In the host's `main.cpp` (the project template has these lines commented out):

```cpp
desc.mods_dir = "mods";                 // the mods folder, beside the executable
desc.mods_game_id = MCE_PROJECT_NAME;   // mods made for another game are skipped
```

That is all. At start the player reads `mods/` beside the executable, mounts the mods it finds
over `assets.mce`, and logs each one. A mod that cannot be read is skipped with a warning; the
game always starts. Leave `mods_dir` null and no command line can enable mods.

Game code can list the loaded mods (for a mods menu, or a "modded" mark on a save):

```cpp
#include "mce/asset/mods.h"

if (const auto* mods = world.try_resource<mce::asset::ModList>()) {  // null: this game takes no mods
    for (const mce::asset::ModEntry& m : mods->mods)  // load order; the last one wins
        MCE_LOG_INFO("mod {} {} ({} assets)", m.name.view(), m.version.view(), m.asset_count);
    // mods->skipped: what was not loaded, and why
}
```

The resource is published before your module's `install()` runs. It is not part of the state
hash, but the content mods bring does change it, like any content change.

Two things to keep in mind when you ship a moddable game:

- **GUIDs are your modding API.** A mod replaces an asset by reusing its GUID, so keep the GUIDs of
  assets you expect people to mod stable across updates (they are stable unless you delete the
  sidecar). Publish the ones that matter, for example in a README next to the game.
- **Ship the module.** A mod scene that uses your game's components needs them at cook time. On
  Linux and macOS the shipped `lib<game>.so`/`.dylib` serves: `mce_cook --module`. On Windows the
  game is linked into the executable, so modders can only cook scenes that use engine components;
  give them your `<game>_cook` tool if you want more.

## 19.2 Making a mod (modder)

A mod is an ordinary project folder (an `assets/` tree, as in chapter 5) plus a `mod.json` beside
`assets/`:

```json
{
  "format": "mce-mod",
  "version": 1,
  "id": "more-crates",
  "name": "More Crates",
  "mod_version": "1.0.0",
  "game": "ember_step",
  "description": "Adds a crate field to level 2.",
  "base": "../EmberStep/assets.mce"
}
```

| Key | Required | Meaning |
|---|---|---|
| `id` | yes | 1 to 64 characters of `a-z 0-9 _ . -`; names the mod in `load_order.txt` |
| `name`, `mod_version`, `description` | no | shown in logs and to game code |
| `game` | no | the game's id (its `mods_game_id`); a game with another id skips the mod |
| `base` | no | the game's `assets.mce` (or a list of them), relative to the mod folder; `mce_cook --base` adds more |

Then cook it with the SDK's `mce_cook` (add `--module` with the game's library when your scenes use
the game's components):

```sh
mce_cook mods/more-crates --pack [--module EmberStep/libember_step.so]
cp mods/more-crates/assets.mce EmberStep/mods/more-crates.mce
```

The cook prints `mod on N base asset(s), M override(s)` and writes `assets.mce` with the mod's
identity inside. In the mod:

- **To replace a game asset**, give your asset the game asset's GUID (in the sidecar for a PNG or
  WAV, inline for a scene or material). The type must stay the same: a texture replaces a texture.
  Replacing the game's start scene GUID replaces its first level.
- **To add content**, use new GUIDs. Your assets can reference the game's assets by GUID
  (`"texture": "guid:..."`) and the cook checks the type of each reference against the base pack.
- **Prefab instances**: a mod scene cannot instance a game prefab (the cook needs the prefab's
  source). Instance your own prefabs, or reference a game prefab through an `AssetRef<Prefab>`
  field that the game's code spawns.

Open the mod in the editor or `mce_mcp` like any project: the game's pack mounts behind the mod, so
the mod's scenes draw the game's assets and play-in-editor works.

## 19.3 Installing and ordering mods (player)

Put each mod's `.mce` file into the game's `mods/` folder. Without a `load_order.txt`, every mod
loads, in file-name order. To choose the mods and their order, add `mods/load_order.txt`:

```
# one mod id per line; the last one wins where two mods change the same asset
more-crates
hd-textures
```

Only the listed mods load. An empty file turns every mod off. The log names each loaded mod and
each skipped one with the reason (not a mod pack, made for another game, duplicate id, not listed).

| Flag | Meaning |
|---|---|
| `-mods 0` | start without mods (a safe mode when a mod breaks the game) |
| `-mods_dir <dir>` | read mods from another folder |

Both flags do nothing in a game that did not turn mods on. `mce_player` takes mods from `mods/`
beside it, so `mce_player -pack EmberStep/assets.mce -mods_dir mymods` tries a mod against any
game's pack.

## 19.4 Limits

- No code: mods cannot add systems, components or C++ of any kind. Behaviour they can change is
  what data already drives (sequences, prefabs, particle presets, tilemaps, materials, shaders).
- A mod pack is read twice at start (its identity, then the mount), so very large mods add boot
  time.
- Saves do not record which mods were loaded.
