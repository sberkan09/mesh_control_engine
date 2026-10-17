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
    // `scope` (normalized with normalize_scope_path; empty = the whole tree): a source outside it that
    // cannot be registered (no sidecar, unreadable, malformed) is left out silently, and `adopt` does
    // not touch it. It is still registered when it can be, so references into it resolve.
    bool scan(StringView project_root, const ImporterRegistry& importers, bool adopt, DiagList& diags,
              Span<const String> scope = {});

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
    Skipped,   // outside CookOptions::paths and what they depend on: untouched, its last output keeps serving
};

struct AssetReport {
    Guid guid;
    String path;  // relative to assets/
    AssetStatus status = AssetStatus::Failed;
    ChunkType type = ChunkType::Manifest;
    u64 output_hash = 0;  // content hash of the cooked container (0 if none)
};

// One file the cook could not take: the first error that names it (more are in CookReport::diags).
struct FailedFile {
    String file;  // as the diagnostics name it: "assets/textures/extra.png"
    Errc code = Errc::Unknown;
    u32 line = 0;
    u32 col = 0;
    u32 errors = 0;  // how many errors name this file
    String message;
};

// The cook size report (M-CONTENT 1a): the bytes of every live asset's cooked file (the pack holds the same chunks,
// plus a small header), summed by kind, with the biggest assets by name. Computed on every cook, cached or not.
struct SizeEntry {
    String path;  // relative to assets/
    u64 bytes = 0;
};
struct SizeReport {
    u64 textures = 0, meshes = 0, audio = 0, scenes = 0, other = 0;  // bytes; audio = sfx + music; scenes = scenes + prefabs
    u32 texture_count = 0;
    u32 textures_downscaled = 0;  // ordinary textures cooked smaller than their source by max_size (sidecar or --max-texture-size)
    u64 total() const { return textures + meshes + audio + scenes + other; }
    Array<SizeEntry> largest;  // up to 5, biggest first (ties by path)
};

struct CookReport {
    Array<AssetReport> assets;  // in cook (topological) order
    DiagList diags;
    Array<FailedFile> failed_files;  // every file with an error, in diagnostic order (not capped)
    u32 cooked = 0;
    u32 up_to_date = 0;
    u32 failed = 0;
    u32 skipped = 0;        // assets outside CookOptions::paths (their last output keeps serving)
    bool subset = false;    // CookOptions::paths was set: cooked/ and its manifest were updated, the pack never
    bool pack_written = false;  // assets.mce was (re)written this run
    u64 manifest_hash = 0;  // xxh64 of cooked/manifest.mce
    u64 pack_hash = 0;      // xxh64 of assets.mce when --pack
    bool mod = false;       // the project has a mod.json (its pack carries a ModInfo chunk)
    u32 base_assets = 0;    // assets in the base packs references may name (mods, --base)
    u32 overrides = 0;      // project assets that replace a base asset (same GUID, same type)
    SizeReport sizes;       // what the cooked project weighs, by asset type (computed on every cook)

    bool ok() const { return diags.error_count() == 0; }
};

struct CookOptions {
    bool force = false;  // ignore the cook DB, re-cook everything
    bool pack = false;   // also write <project>/assets.mce (FR-SHP-010)
    bool adopt = false;  // create sidecars for unregistered sources
    // Cook only these assets and what they depend on. Entries are paths relative to assets/ (a file,
    // or a directory for everything under it; a leading "assets/" and a ".mceasset" suffix are
    // accepted). Everything else is left as it was: not scanned for errors, not re-cooked, still in
    // the manifest with its last output. An entry that matches no asset is an error. A subset never
    // writes the pack (a pack must hold every asset of one consistent cook); `pack` is ignored then.
    Array<String> paths;
    // Keep going past files that cannot be cooked: the run still cooks everything it can and the
    // report lists every failing file in CookReport::failed_files. The pack is still written only
    // by a run with no errors (a pack missing an asset the game references would not load).
    bool skip_errors = false;
    // Packs (or cooked dirs) this project builds on, added to a mod.json's "base" (ADR-0034):
    // references may name their assets, and a project asset with one of their GUIDs overrides it.
    Array<String> base_packs;
    // Texture storage (mce_cook --textures astc): the phone builds cook ASTC 4x4. Part of the re-cook key.
    TextureCompression textures = TextureCompression::None;
    // Project-wide cap on the largest side of ordinary (PNG/JPG) textures (mce_cook --max-texture-size N): a power of
    // two from 1 to 16384, 0 = no limit (the default, which cooks exactly as before). Applied as min(sidecar
    // "max_size", this); a bigger texture is box-halved before its mips. HDR (.hdr/.exr) textures keep their own
    // "max_size". Part of the re-cook key. Any other value is a cook error.
    u32 max_texture_size = 0;
};

// The cook DB's tool version for these options: a different texture compression or texture cap re-cooks (the cap
// as log2 + 1 in bits 24..31, so 0 = no limit leaves the version as it was).
inline u32 cook_tool_version(TextureCompression textures, u32 max_texture_size = 0) {
    u32 v = kCookToolVersion | (u32(textures) << 16);
    if (max_texture_size) {
        u32 log2 = 0;
        while ((1u << log2) < max_texture_size && log2 < 31) ++log2;
        v |= (log2 + 1u) << 24;
    }
    return v;
}

// True for a valid CookOptions::max_texture_size: 0 (no limit) or a power of two from 1 to 16384.
inline bool valid_max_texture_size(u32 v) { return v == 0 || (v <= 16384u && (v & (v - 1)) == 0); }

class CookService {
public:
    CookService();
    ~CookService();
    ImporterRegistry& importers() { return importers_; }
    const ImporterRegistry& importers() const { return importers_; }

    // Full + incremental cook of a project directory.
    CookReport cook(StringView project_root, const CookOptions& opt);

private:
    CookReport run(StringView project_root, const CookOptions& opt);
    ImporterRegistry importers_;
};

// Cooks one native document from memory (the editor's unsaved scene, for play-in-editor) exactly
// as the cook would cook it from `source_rel_path`: same importer, references and prefab instances
// resolved against `registry` (a scan of the project). `out` receives the container bytes. False
// (with diagnostics) when the text does not parse or the importer refuses it.
bool cook_document(const AssetRegistry& registry, const IImporter& importer, Guid guid, StringView source_rel_path,
                   StringView text, Array<byte>& out, DiagList& diags);

// A CookOptions::paths entry as the cook compares it: forward slashes, no leading "assets/" or "./",
// no trailing "/", no ".mceasset" suffix.
String normalize_scope_path(StringView p);
// True when `rel` (relative to assets/) is one of `scope`'s files or lies under one of its
// directories; an empty `scope` holds everything.
bool in_scope(StringView rel, Span<const String> scope);

// Fresh UUIDv4 (sidecar creation only — the cook never needs deterministic GUIDs, only stable ones).
Guid generate_guid();

}  // namespace mce::cook
