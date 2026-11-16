#include "mce/editor/group_entities.h"

namespace mce::editor {

namespace {

bool has_ancestor_in(const EditorScene& scene, EntityId id, Span<const EntityId> ids) {
    for (EntityId other : ids)
        if (other != id && scene.is_ancestor(other, id)) return true;
    return false;
}

}  // namespace

EntityId group_entities(EditorScene& scene, Span<const EntityId> ids) {
    Array<EntityId> top;
    for (EntityId id : ids) {
        if (scene.doc().find(id) == nullptr || has_ancestor_in(scene, id, ids)) continue;
        bool seen = false;
        for (EntityId t : top) seen = seen || t == id;
        if (!seen) top.push_back(id);
    }
    if (top.empty()) return kNoEntity;
    const EntityId parent = scene.parent_of(top[0]);
    scene.begin_edit("Group");
    const EntityId group = scene.create_entity("Group");
    if (parent != kNoEntity) scene.set_parent(group, parent);
    for (EntityId id : top) scene.set_parent(id, group);
    scene.end_edit();
    return group;
}

Array<EntityId> ungroup_entities(EditorScene& scene, Span<const EntityId> ids) {
    Array<EntityId> freed;
    bool any = false;
    for (EntityId id : ids) {
        Array<EntityId> kids;
        scene.children_of(id, kids);
        any = any || !kids.empty();
    }
    if (!any) return freed;
    scene.begin_edit("Ungroup");
    for (EntityId id : ids) {
        if (scene.doc().find(id) == nullptr) continue;
        Array<EntityId> kids;
        scene.children_of(id, kids);
        if (kids.empty()) continue;
        const EntityId parent = scene.parent_of(id);
        for (EntityId k : kids) {
            scene.set_parent(k, parent);
            freed.push_back(k);
        }
        scene.destroy_entity(id);
    }
    scene.end_edit();
    return freed;
}

}  // namespace mce::editor
