#pragma once
// Asset hot reload in the editor (FR-AST-090, editor-tools.md §4.1, ADR-0029 §2).
//
//   AssetWatcher   polls assets/ (path, size, mtime) every `interval` seconds — polling, not OS
//                  change notification (ADR-0015): portable and enough at project scale.
//   EditorSession::hot_reload()  re-cooks incrementally (the cook DB re-cooks only what changed and
//                  its dependents) and swaps each re-cooked asset into the mounted tree with
//                  AssetSystem::commit_reload, so handles stay valid and the viewport, the play world
//                  and the clip bank pick up the new generation on their next frame. A new or removed
//                  asset remounts instead. A failed re-cook keeps the previous output serving.

#include "mce/cook/string.h"
#include "mce/containers/array.h"

namespace mce::editor {

class AssetWatcher {
public:
    // Watches <root>/assets (empty root: stops watching). The first scan is the baseline.
    void watch(StringView project_root);
    StringView root() const { return root_.view(); }

    // Rescans when `interval` seconds passed since the last scan (`now`: any monotonic clock in
    // seconds; interval <= 0 scans every call). True when anything was added, removed or changed;
    // `changed` then lists those paths, relative to assets/ (sorted).
    bool poll(f64 now, f64 interval, Array<cook::String>* changed = nullptr);

private:
    struct Entry {
        cook::String path;  // relative to assets/
        u64 size = 0;
        i64 mtime = 0;
    };
    void scan(Array<Entry>& out) const;

    cook::String root_;
    Array<Entry> entries_;  // sorted by path
    f64 last_ = -1.0;
};

// What one hot_reload() did.
struct HotReloadResult {
    u32 reloaded = 0;      // assets swapped in through commit_reload
    u32 failed = 0;        // re-cook or swap failures (the previous output keeps serving)
    bool remounted = false;  // a new/removed asset needed a fresh mount
    bool scene_reloaded = false;  // the open scene changed on disk and was re-read
    f64 seconds = 0.0;     // wall time of cook + swap
};

}  // namespace mce::editor
