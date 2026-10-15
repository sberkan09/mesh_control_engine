#pragma once
// Save games and replays (v0.5, ecs-gameplay.md §2.13 / §3.9, FR-RUN-065 / FR-RUN-070).
//
// What a save IS (this header, mce_ecs_scene): a versioned header plus a [Save]-flagged
// reflection dump of every entity carrying a SaveKey. Fields opt in with the `Save` attribute on
// MCE_COMPONENT; everything else (asset handles, caches, physics state) is rebuilt by the game.
// Identity across sessions is the author-assigned SaveKey, not the Entity handle: load finds the
// entity with the same key (or spawns one) and writes the saved fields on top — the
// "base world + deltas" model. Unknown types/fields from another module version are skipped and
// counted, never fatal; a save from a NEWER format version is rejected with Errc::VersionTooNew.
//
// File mechanics (save::write/read): <root>/<name>.mcesave written atomically — temp file, then
// the previous save rotated to .bak, then rename — so a crash mid-save keeps the old save. The
// root defaults to crash::user_data_root()/MCE/<game>/saves (mce/crash.h: %LOCALAPPDATA% on Windows, the
// app's sandbox on Android and iOS, $XDG_DATA_HOME|~/.local/share elsewhere). core.md §2.9 places these mechanics in core; they live here until
// a second consumer needs them below L2 (ADR-0025).
//
// Replays: the per-tick input stream (input::InputRecorder bytes, opaque here) framed with the
// start/final state hashes the determinism harness compares — same pipeline, same hashes.

#include "mce/asset/guid.h"
#include "mce/refl/macros.h"
#include "mce/ecs/world.h"

namespace mce {

// Stable save identity (author-assigned; unique per persistent entity).
struct SaveKey {
    u32 key = 0;
};
MCE_COMPONENT(SaveKey, key)

namespace save {

inline constexpr u32 kSaveMagic = 0x5653434D;    // "MCSV"
inline constexpr u32 kReplayMagic = 0x5052434D;  // "MCRP"
inline constexpr u16 kSaveFormatVersion = 1;
inline constexpr u16 kReplayFormatVersion = 1;

struct SaveInfo {
    u32 module_version = 0;  // the game module's own version (mismatch is reported, not fatal)
    asset::Guid scene;       // base scene the dump applies on top of
    u64 tick = 0;
    f64 playtime = 0.0;      // seconds
};

struct LoadStats {
    u32 entities = 0;            // records applied
    u32 spawned = 0;             // records whose SaveKey had no live entity
    u32 fields = 0;              // fields written
    u32 unknown_components = 0;  // not reflected / not registered in this build
    u32 unknown_fields = 0;      // field no longer exists
    u32 mismatched_fields = 0;   // field changed kind or size
    bool module_version_mismatch = false;
};

// ---- payloads (pure; no files) ---------------------------------------------------------
void write_save(World& w, const SaveInfo& info, Array<byte>& out);
// Validates and applies a save payload; outside phases. Errors: BadMagic, Truncated,
// VersionTooNew, CrcMismatch (nothing applied on any error).
Result<LoadStats> read_save(World& w, Span<const byte> payload, SaveInfo* info = nullptr, u32 module_version = 0);

struct ReplayInfo {
    u32 module_version = 0;
    asset::Guid scene;
    u64 start_tick = 0;
    u64 tick_count = 0;
    u64 start_hash = 0;  // World::state_hash before the first recorded tick
    u64 final_hash = 0;  // after the last
};
void write_replay(const ReplayInfo& info, Span<const byte> input_stream, Array<byte>& out);
// Returns the input stream (a view into `payload`) and fills `info`.
Result<Span<const byte>> read_replay(Span<const byte> payload, ReplayInfo& info);

// ---- files ----------------------------------------------------------------------------------
// Root for save names; set once at startup (default: default_root("MCE")).
void set_root(StringView dir);
FixedString<512> root();
FixedString<512> default_root(StringView game);
FixedString<512> path_of(StringView name);  // <root>/<name>.mcesave
Result<void> write(StringView name, Span<const byte> payload);
Result<void> read(StringView name, Array<byte>& out);  // falls back to <name>.mcesave.bak if the main file is missing
bool exists(StringView name);

// Whole-game convenience: write_save + write, and read + read_save. Load errors carry the file
// path (FR-RUN-065: a newer-format save fails with code + path, never a crash).
Result<void> save_game(World& w, StringView name, const SaveInfo& info);
Result<LoadStats> load_game(World& w, StringView name, SaveInfo* info = nullptr, u32 module_version = 0);

}  // namespace save
}  // namespace mce
