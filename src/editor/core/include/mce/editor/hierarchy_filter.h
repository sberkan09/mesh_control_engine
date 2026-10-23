#pragma once
// HierarchyFilter — the Hierarchy panel's search box (M-HIERFILTER). A query is words separated by spaces; an
// entity matches when its name contains every word (case-insensitive). The filter shows the matches and the
// ancestors that lead to them, so a hit stays where it is in the tree. Rebuilt each frame while the query is not
// empty (a flag per entity, O(entities x depth)). Headless, tested in CI.

#include "mce/containers/array.h"
#include "mce/editor/editor_scene.h"

namespace mce::editor {

class HierarchyFilter {
public:
    // True when `name` contains every space-separated word of `query` (an empty query matches everything).
    static bool name_matches(StringView name, StringView query);

    // Recomputes the flags for `scene`'s document; an empty query makes the filter inactive.
    void rebuild(const EditorScene& scene, StringView query);

    bool active() const { return active_; }
    // Inactive: everything is visible. Active: the matches and their ancestors.
    bool visible(const SceneDocument& doc, EntityId id) const { return flag(doc, id) != 0; }
    // Active: only the entities whose own name matches (the panel highlights these).
    bool is_match(const SceneDocument& doc, EntityId id) const { return flag(doc, id) == 2; }
    usize match_count() const { return matches_; }

private:
    u8 flag(const SceneDocument& doc, EntityId id) const {
        if (!active_) return 1;
        const i32 i = doc.index_of(id);
        return i < 0 || static_cast<usize>(i) >= flags_.size() ? u8{0} : flags_[static_cast<usize>(i)];
    }

    Array<u8> flags_;  // per document index: 0 hidden, 1 ancestor of a match, 2 match
    usize matches_ = 0;
    bool active_ = false;
};

}  // namespace mce::editor
