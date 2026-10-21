#pragma once
// cooked/.mcedb — the cook DB (assets.md §5.3): an append-only binary journal of per-asset
// records, compacted (rewritten as a snapshot) when it grows past twice its live set. A record
// is the full re-cook key: source hash, settings hash, importer id/version, tool version, and
// every dependency's output hash. The DB is an optimization only: a corrupt tail is dropped and
// those assets simply re-cook (cook is pure).

#include "mce/asset/container.h"
#include "mce/containers/hash_map.h"
#include "mce/cook/string.h"

namespace mce::cook {

struct DbDep {
    asset::Guid guid;
    u64 output_hash;
};

struct DbRecord {
    asset::Guid guid;
    u64 source_hash = 0;
    u64 settings_hash = 0;
    u64 importer_id = 0;  // fnv1a64 of the importer id
    u32 importer_version = 0;
    u32 tool_version = 0;
    u64 output_hash = 0;
    asset::ChunkType type = asset::ChunkType::Manifest;
    String source_path;
    Array<DbDep> deps;  // sorted by GUID
};

class CookDb {
public:
    // Loads the journal (missing file = empty DB). Never fails: corruption truncates.
    void load(StringView path);
    const DbRecord* find(asset::Guid g) const;
    Span<const DbRecord> records() const { return records_.span(); }

    // Appends records (replacing in-memory entries) and persists: appends to the journal, or
    // rewrites it as a snapshot when the journal holds more than twice the live records or the
    // tail was corrupt.
    bool commit(StringView path, Span<const DbRecord> fresh);

private:
    Array<DbRecord> records_;
    HashMap<asset::Guid, u32> index_;
    u32 journal_records_ = 0;
    bool needs_rewrite_ = false;
};

}  // namespace mce::cook
