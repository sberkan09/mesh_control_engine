#include "mce/editor/duplicate_offset.h"

#include <algorithm>

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
    // The originals in document order: the copies come back in the same order. A copy whose parent was not copied
    // lands at the root, so its offset starts from the original's world position, not from its local one.
    Array<EntityId> originals;
    for (const DocEntity& e : scene.doc().entities())
        for (EntityId id : ids)
            if (id == e.id) {
                originals.push_back(id);
                break;
            }
    Array<WorldTransform> start;
    for (EntityId id : originals) start.push_back(scene.world_transform(id));
    Array<EntityId> made;
    scene.begin_edit("Duplicate offset");
    scene.duplicate_entities(ids, &made);
    const bool paired = made.size() == originals.size();
    for (usize i = 0; i < made.size(); ++i) {
        const EntityId id = made[i];
        if (contains(made, scene.parent_of(id))) continue;  // a copy under a copied parent travels with it
        if (!scene.get<LocalTransform>(id)) continue;
        WorldTransform w = paired ? start[i] : scene.world_transform(id);
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
