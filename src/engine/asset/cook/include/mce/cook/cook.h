#pragma once
// The cook library (assets.md §5): scan -> graph build -> topological order -> stage execution ->
// manifest + DB commit. Backs mce_cook and the editor's cook button (CookService, §7.1).
//
// Project layout (assets.md §4.4): <project>/assets/ is the source tree (the only tree a human
// edits), <project>/cooked/ is written only by the cook and is disposable: deleting it and
// cooking again yields byte-identical outputs.

#include "mce/asset/guid.h"
#include "mce/cook/importer.h"
#include "mce/cook/mod_project.h"

namespace mce::cook {

// Bump to force every asset to re-cook after a change to shared cook code (the "tool versions"
// key of the cook DB record, assets.md §5.3).
inline constexpr u32 kCookToolVersion = 1;

struct AssetRecord {
    Guid guid;
    String source_path;    // relative to assets/, forward slashes
    String sidecar_path;   // relative to assets/; empty for native assets
    const IImporter* importer = nullptr;
    u64 source_hash = 0;   // xxh64 of source bytes (content, never mtime)
    u64 settings_hash = 0; // canonical sidecar settings + importer id/version
    Array<byte> source;    // source bytes (kept for the cook pass)
    JsonDoc doc;           // sidecar (interchange) or the document itself (native)
    u32 settings_node = kNoSettings;  // node in `doc` (an index: records move, values would dangle)
    Array<DepRef> deps;    // filled by the graph build

    static constexpr u32 kNoSettings = ~0u;
    JsonValue settings() const { return settings_node == kNoSettings ? JsonValue() : JsonValue(&doc, settings_node); }
};

// Source registry: the in-memory projection of sidecars + native GUIDs (assets.md §4.3).
class AssetRegistry {
public:
    // Enumerates assets/ (sorted, hidden entries skipped), loads sidecars, verifies one GUID per
    // file. Missing sidecars, duplicate GUIDs and malformed sidecars are reported as a batch.
    // With `adopt`, unregistered sources get a fresh sidecar instead of an error.
    bool scan(StringView project_root, const ImporterRegistry& importers, bool adopt, DiagList& diags);

    usize size() const { return records_.size(); }
    AssetRecord& at(usize i) { return records_[i]; }
    const AssetRecord& at(usize i) const { return records_[i]; }
    const AssetRecord* by_guid(Guid g) const;
    // Errc::NotFound when no asset lives at `rel_path` (relative to assets/).
    Result<Guid> guid_for_path(StringView rel_path) const;

    // Rename/move with GUID stability: moves source and sidecar together. References are GUIDs,
    // so dependents need no rewrite (FR-EDT-110).
    Result<void> move(Guid g, StringView new_rel_path);

    // Previous-cook knowledge used to auto-repair external renames (orphaned sidecar + source with
    // the same content hash). Set by CookService before scan.
    struct KnownSource {
        Guid guid;
        u64 source_hash;
    };
    void set_known_sources(Span<const KnownSource> known) { known_ = known; }

private:
    Array<AssetRecord> records_;
    String root_;
    Span<const KnownSource> known_;
};

enum class AssetStatus : u8 {
    Cooked,    // re-cooked this pass
    UpToDate,  // cook DB hit, output reused
    Failed,    // importer error (previous output, if any, keeps serving)
    Blocked,   // depends on a failed/missing asset or sits in a cycle
};

struct AssetReport {
    Guid guid;
    String path;  // relative to assets/
    AssetStatus status = AssetStatus::Failed;
    ChunkType type = ChunkType::Manifest;
    u64 output_hash = 0;  // content hash of the cooked container (0 if none)
};

struct CookReport {
    Array<AssetReport> assets;  // in cook (topological) order
    DiagList diags;
    u32 cooked = 0;
    u32 up_to_date = 0;
    u32 failed = 0;
    u64 manifest_hash = 0;  // xxh64 of cooked/manifest.mce
    u64 pack_hash = 0;      // xxh64 of assets.mce when --pack
    bool mod = false;       // the project has a mod.json (its pack carries a ModInfo chunk)
    u32 base_assets = 0;    // assets in the base packs references may name (mods, --base)
    u32 overrides = 0;      // project assets that replace a base asset (same GUID, same type)

    bool ok() const { return diags.error_count() == 0; }
};

struct CookOptions {
    bool force = false;  // ignore the cook DB, re-cook everything
    bool pack = false;   // also write <project>/assets.mce (FR-SHP-010)
    bool adopt = false;  // create sidecars for unregistered sources
    // Packs (or cooked dirs) this project builds on, added to a mod.json's "base" (ADR-0034):
    // references may name their assets, and a project asset with one of their GUIDs overrides it.
    Array<String> base_packs;
};

class CookService {
public:
    CookService();
    ~CookService();
    ImporterRegistry& importers() { return importers_; }
    const ImporterRegistry& importers() const { return importers_; }

    // Full + incremental cook of a project directory.
    CookReport cook(StringView project_root, const CookOptions& opt);

private:
    ImporterRegistry importers_;
};

// Cooks one native document from memory (the editor's unsaved scene, for play-in-editor) exactly
// as the cook would cook it from `source_rel_path`: same importer, references and prefab instances
// resolved against `registry` (a scan of the project). `out` receives the container bytes. False
// (with diagnostics) when the text does not parse or the importer refuses it.
bool cook_document(const AssetRegistry& registry, const IImporter& importer, Guid guid, StringView source_rel_path,
                   StringView text, Array<byte>& out, DiagList& diags);

// Fresh UUIDv4 (sidecar creation only — the cook never needs deterministic GUIDs, only stable ones).
Guid generate_guid();

}  // namespace mce::cook
