#pragma once
// EditorProject — the open project folder (assets.md §4.4: <root>/assets is the source tree,
// <root>/cooked the disposable cook output). The editor drives the cook library and reads the
// GUID registry; it implements neither (editor-tools.md §1.2).
//
//   catalog   GUID registry scan of assets/: pickers, scene list (finding F6 MVP pickers)
//   cook      CookService over the project; synchronous at MVP. The last report feeds the cook
//             error panel (FR-EDT-070). After every cook the cooked tree is mounted fresh, so the
//             viewport sees new content; assets that failed keep their previous output.

#include "mce/asset/asset_system.h"
#include "mce/cook/cook.h"
#include "mce/editor/hot_reload.h"
#include "mce/memory/allocator.h"

namespace mce::editor {

struct ProjectAsset {
    asset::Guid guid;
    cook::String path;  // relative to assets/, forward slashes
    asset::ChunkType type = asset::ChunkType::Manifest;
};

// "Texture", "Font", ... for a chunk type ("" when it is not an asset type).
const char* asset_type_name(asset::ChunkType type);

class EditorProject {
public:
    EditorProject();
    ~EditorProject();
    EditorProject(const EditorProject&) = delete;
    EditorProject& operator=(const EditorProject&) = delete;

    // `root` must hold an assets/ directory. Scans the catalog; does not cook.
    Result<void> open(StringView root);
    void close();
    bool is_open() const { return !root_.empty(); }
    StringView root() const { return root_.view(); }

    // <root>/assets/<rel>
    cook::String asset_path(StringView rel) const;
    // Relative to assets/ when `path` lies inside it (false otherwise).
    bool relative_asset_path(StringView path, cook::String& rel) const;

    // ---- catalog ----
    void rescan();
    Span<const ProjectAsset> assets() const { return assets_.span(); }
    const ProjectAsset* find(asset::Guid g) const;
    const cook::DiagList& scan_diags() const { return scan_diags_; }

    // ---- cook ----
    const cook::CookReport& cook(bool pack = false);
    bool has_cooked() const { return cooked_; }
    const cook::CookReport& last_report() const { return report_; }
    // The cooked tree, mounted after the last cook (null before the first cook or when nothing
    // could be mounted). Replaced on every cook: holders re-read it when generation() moves.
    asset::AssetSystem* mounted() { return mounted_.get(); }
    u64 generation() const { return generation_; }
    // Rescans + cooks only when the cooked manifest is missing (first open of a fresh checkout).
    void ensure_cooked();
    // Hot reload: an incremental cook whose re-cooked assets are swapped into the live mount
    // (AssetSystem::commit_reload: handles stay valid, mounted() and generation() do not change).
    // When the set of assets changed (or nothing is mounted) the tree is remounted instead, and
    // `before_remount(user)` runs first so holders of mounted() can let go of it.
    using BeforeRemountFn = void (*)(void* user);
    HotReloadResult reload(BeforeRemountFn before_remount = nullptr, void* user = nullptr);
    // The cook's importers (play-in-editor cooks the open document with them).
    const cook::ImporterRegistry& importers() const { return service_.importers(); }

private:
    void log_report() const;
    void remount();

    cook::CookService service_;
    cook::String root_;
    Array<ProjectAsset> assets_;
    cook::DiagList scan_diags_;
    cook::CookReport report_;
    mem::UniquePtr<asset::AssetSystem> mounted_;
    u64 generation_ = 0;
    bool cooked_ = false;
};

}  // namespace mce::editor
