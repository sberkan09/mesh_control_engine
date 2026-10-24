#include "mce/editor/duplicate_offset.h"

#include "mce/ecs/transform.h"

namespace mce::editor {

namespace {

bool contains(const Array<EntityId>& list, EntityId id) {
    for (EntityId other : list)
        if (other == id) return true;
    return false;
}

}  // namespace

usize duplicate_offset(EditorScene& scene, Span<const EntityId> ids, f32 dx, f32 dy, Array<EntityId>* created) {
    Array<EntityId> made;
    scene.begin_edit("Duplicate offset");
    scene.duplicate_entities(ids, &made);
    for (EntityId id : made) {
        if (contains(made, scene.parent_of(id))) continue;  // a child of a copy travels with its parent
        if (!scene.get<LocalTransform>(id)) continue;
        WorldTransform w = scene.world_transform(id);
        w.position.x += dx;
        w.position.y += dy;
        scene.set(id, scene.local_for_world(id, w));
    }
    scene.end_edit();
    if (created != nullptr)
        for (EntityId id : made) created->push_back(id);
    return made.size();
}

}  // namespace mce::editor
