#pragma once
// Mod projects (FR-RUN-090, ADR-0034): a normal project with a mod.json beside assets/. The cook
// then resolves references into the game's own pack ("base"), checks that every override keeps the
// type of the game asset it replaces, and with --pack writes a ModInfo chunk after the manifest, so
// the pack drops into a game's mods folder as <id>.mce.
//
//   {
//     "format": "mce-mod",
//     "version": 1,
//     "id": "more-crates",                 // [a-z0-9_.-], 1..64: the name load_order.txt uses
//     "name": "More Crates",               // optional, defaults to the id
//     "mod_version": "1.0.0",              // optional
//     "game": "ember_step",                // optional: the game's PlayerDesc::mods_game_id
//     "description": "Adds crates.",       // optional
//     "base": ["../EmberStep/assets.mce"]  // the game's pack(s) or cooked dirs; a string is one entry
//   }
//
// Relative base paths are taken from the project root. mce_cook --base <pack> adds more.

#include "mce/asset/container.h"
#include "mce/asset/guid.h"
#include "mce/containers/hash_map.h"
#include "mce/cook/diagnostics.h"
#include "mce/cook/string.h"

namespace mce::cook {

struct ModProject {
    String id;
    String name;
    String version;
    String game;
    String description;
    Array<String> base;  // resolved: absolute, or joined to the project root
};

// <project>/mod.json exists.
bool is_mod_project(StringView project_root);

// Parses <project>/mod.json. False with diagnostics (file:line:col) when it is malformed.
bool read_mod_project(StringView project_root, ModProject& out, DiagList& diags);

// The assets of the base packs a mod builds on: GUID -> primary chunk type. A pack path ends in
// ".mce"; anything else is a cooked directory holding manifest.mce. The first base to list a GUID
// wins, like a mount. False with a diagnostic per base that cannot be read.
bool read_base_assets(Span<const String> bases, HashMap<asset::Guid, asset::ChunkType>& out, DiagList& diags);

}  // namespace mce::cook
