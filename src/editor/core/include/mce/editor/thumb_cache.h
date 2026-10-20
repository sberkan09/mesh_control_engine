#pragma once
// The asset browser's thumbnail cache (M-THUMBS 3): headless, so a test can pin it. A picture is kept per asset
// GUID at one size, rendered by render_thumbnail a few at a time per frame (a folder of two hundred textures does
// not stall the editor), and drawn again when the asset changes: each entry remembers a stamp of its source file
// (size and last-write time, hashed) and looks at it again about once a second. Stale pictures stay on screen
// until the new one is ready. The ImGui panel uploads an entry to the GPU whenever its `revision` moves.

#include "mce/asset/container.h"
#include "mce/containers/array.h"
#include "mce/cook/image.h"
#include "mce/types.h"

namespace mce::editor {

class EditorProject;
struct ProjectAsset;

class ThumbCache {
public:
    struct Entry {
        asset::Guid guid;
        cook::Image image;     // size x size RGBA, valid once revision > 0
        bool real = false;     // shows the asset itself (not the plain tile)
        u32 revision = 0;      // +1 at every render
        u64 stamp = 0;         // of the source when rendered
        u64 checked = 0;       // frame the stamp was last compared
        u64 used = 0;          // frame last asked for
    };

    static constexpr u32 kDefaultBudget = 4;       // renders per frame
    static constexpr u64 kRecheckFrames = 60;      // between two looks at an entry's source
    static constexpr usize kMaxEntries = 512;      // oldest unused pictures go beyond this

    // Starts a frame: the render budget is refilled. Call once before the first get().
    void begin_frame(u32 budget = kDefaultBudget);
    // The picture of `asset` at `size` (kThumbMin..kThumbMax), or null while it waits for a render slot. A new
    // size drops every picture. The pointer is good until the next get(). Entries that were not asked for stay (the folder may come back) until trim().
    const Entry* get(EditorProject& project, const ProjectAsset& asset, u32 size);
    // Keeps at most kMaxEntries entries, dropping the longest unused first. Call at the end of a frame.
    void trim();
    // Entries whose asset is gone from the catalog are removed; `gone` lists their GUIDs (for the GPU side).
    void drop_missing(const EditorProject& project, Array<asset::Guid>& gone);
    void clear() { entries_.clear(); }

    usize size() const { return entries_.size(); }
    u32 size_px() const { return size_px_; }
    u64 frame() const { return frame_; }
    u32 renders() const { return renders_; }  // since construction

private:
    Entry* find(asset::Guid g);

    Array<Entry> entries_;
    u32 size_px_ = 0;
    u64 frame_ = 0;
    u32 budget_ = kDefaultBudget;
    u32 renders_ = 0;
};

// Hash of an asset's source file (size and last-write time), 0 when it cannot be inspected. Mesh assets also
// fold in whether a cooked tree is mounted, since that is what a mesh thumbnail reads.
u64 thumb_stamp(EditorProject& project, const ProjectAsset& asset);

}  // namespace mce::editor
