#include "mce/editor/select_all.h"

#include "mce/editor/hierarchy_filter.h"

namespace mce::editor {

Array<EntityId> select_all(const EditorScene& scene) {
    Array<EntityId> out;
    for (const DocEntity& e : scene.doc().entities()) out.push_back(e.id);
    return out;
}

Array<EntityId> invert_selection(const EditorScene& scene) {
    Array<EntityId> out;
    for (const DocEntity& e : scene.doc().entities())
        if (!scene.selection().contains(e.id)) out.push_back(e.id);
    return out;
}

Array<EntityId> select_matching(const EditorScene& scene, StringView query) {
    Array<EntityId> out;
    bool blank = true;
    for (const char c : query) blank = blank && c == ' ';
    if (blank) return out;
    for (const DocEntity& e : scene.doc().entities())
        if (HierarchyFilter::name_matches(e.name.view(), query)) out.push_back(e.id);
    return out;
}

}  // namespace mce::editor
