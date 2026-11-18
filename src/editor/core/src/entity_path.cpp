#include "mce/editor/entity_path.h"

namespace mce::editor {

cook::String entity_path(const EditorScene& scene, EntityId id) {
    cook::String out;
    if (scene.doc().find(id) == nullptr) return out;
    Array<EntityId> chain;  // id, its parent, ... up to a root
    EntityId cur = id;
    while (cur != kNoEntity) {
        if (scene.doc().find(cur) == nullptr) break;
        bool seen = false;  // a loop of parents: name each entity once
        for (EntityId c : chain) seen = seen || c == cur;
        if (seen) break;
        chain.push_back(cur);
        cur = scene.parent_of(cur);
    }
    for (usize i = chain.size(); i > 0; --i) {
        if (i != chain.size()) out.append('/');
        out.append(scene.doc().find(chain[i - 1])->name.view());
    }
    return out;
}

cook::String entity_paths(const EditorScene& scene, Span<const EntityId> ids) {
    cook::String out;
    bool first = true;
    for (EntityId id : ids) {
        if (scene.doc().find(id) == nullptr) continue;
        if (!first) out.append('\n');
        first = false;
        out.append(entity_path(scene, id).view());
    }
    return out;
}

}  // namespace mce::editor
