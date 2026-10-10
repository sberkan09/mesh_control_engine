#pragma once
// Data mods (FR-RUN-090, ADR-0034): which mod packs a game loads, and in what order.
//
// A mod is one pack file (`<id>.mce`, written by `mce_cook --pack` for a project holding a
// mod.json): a manifest, a ModInfo chunk, and cooked assets. Mounted ahead of the game's own pack,
// a mod asset with a game asset's GUID replaces it and new GUIDs add content; nothing else
// changes. Mods are data only: no code is ever loaded from one (ADR-0034 §2).
//
// The player does the I/O around this: it lists `<mods_dir>/*.mce`, reads
// `<mods_dir>/load_order.txt` when there is one, calls resolve_mods, then mounts the result in
// reverse (the last mod first, the game's pack last: AssetSystem lets earlier roots win).
//
// load_order.txt: one mod id per line, `#` starts a comment, blank lines are ignored. Listed mods
// load in that order (a later line wins over an earlier one); packs not listed stay off. Without
// the file every pack loads, ordered by file name.

#include "mce/asset/formats.h"
#include "mce/fixed_string.h"

namespace mce::asset {

inline constexpr u32 kMaxMods = 256;

// [a-z0-9_.-], 1..64 bytes, not starting with '.' (ids name files and load_order.txt lines).
bool is_valid_mod_id(StringView id);

struct ModEntry {
    FixedString<64> id;
    FixedString<128> name;
    FixedString<32> version;
    FixedString<64> game;
    FixedString<512> path;  // the pack file
    u32 asset_count = 0;    // entries in its manifest
};

struct ModSkipped {
    FixedString<512> what;  // a pack path, or the id from a load_order.txt line
    FixedString<192> reason;
};

// Also the world resource the player publishes when the game takes mods, so game code can list
// them (a mods menu, a "modded" badge): world.try_resource<asset::ModList>(); null = mods off.
struct ModList {
    Array<ModEntry> mods;  // load order: later entries win over earlier ones, all win over the game
    Array<ModSkipped> skipped;
};

// Reads a mod pack's identity: a container with a manifest and a valid ModInfo chunk. Header CRCs
// are checked here; the payload CRC is checked when the pack is mounted.
Result<ModEntry> read_mod_pack(StringView path);

// Picks the packs that load and their order. `packs`: the *.mce files of the mods folder, any
// order. `load_order`: the text of load_order.txt, null when there is none. `game_id`: the host's
// id; a mod naming another game is skipped (an empty id on either side skips the check). Packs
// that cannot be read, duplicate ids and anything past kMaxMods are skipped with a reason.
ModList resolve_mods(Span<const StringView> packs, const StringView* load_order, StringView game_id);

}  // namespace mce::asset
