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

namespace {

bool same_components(const DocEntity& a, const DocEntity& b) {
    if (a.components.size() != b.components.size()) return false;
    for (usize i = 0; i < a.components.size(); ++i)  // both lists are sorted by type name
        if (a.components[i].type != b.components[i].type) return false;
    return true;
}

}  // namespace

Array<EntityId> select_similar(const EditorScene& scene) {
    Array<EntityId> out;
    const DocEntity* anchor = scene.doc().find(scene.selection().anchor());
    if (anchor == nullptr) return out;
    for (const DocEntity& e : scene.doc().entities())
        if (same_components(*anchor, e)) out.push_back(e.id);
    return out;
}

}  // namespace mce::editor
