#include "mce/editor/flip_entities.h"

#include <algorithm>

#include "mce/ecs/transform.h"

namespace mce::editor {

namespace {

f32& axis_of(WorldTransform& w, AlignAxis axis) { return axis == AlignAxis::X ? w.position.x : w.position.y; }

}  // namespace

bool flip_entities(EditorScene& scene, Span<const EntityId> ids, AlignAxis axis) {
    Array<EntityId> items;  // once each, with a transform
    for (EntityId id : ids) {
        if (!scene.get<LocalTransform>(id)) continue;
        bool seen = false;
        for (EntityId other : items) seen = seen || other == id;
        if (!seen) items.push_back(id);
    }
    if (items.empty()) return false;

    WorldTransform first = scene.world_transform(items[0]);
    f32 lo = axis_of(first, axis), hi = lo;
    for (EntityId id : items) {
        WorldTransform w = scene.world_transform(id);
        lo = std::min(lo, axis_of(w, axis));
        hi = std::max(hi, axis_of(w, axis));
    }
    const f32 sum = lo + hi;  // the mirror maps p to (lo + hi) - p

    // All targets are computed before any entity moves: a parent that moves must not shift its children's mirror.
    struct Target {
        EntityId id;
        WorldTransform world;
    };
    Array<Target> targets;
    for (EntityId id : items) {
        WorldTransform w = scene.world_transform(id);
        f32& p = axis_of(w, axis);
        p = sum - p;
        targets.push_back({id, w});
    }

    scene.begin_edit("Flip");
    for (const Target& t : targets) {
        LocalTransform l = scene.local_for_world(t.id, t.world);
        f32& s = axis == AlignAxis::X ? l.scale.x : l.scale.y;
        s = -s;
        scene.set(t.id, l);
    }
    scene.end_edit();
    return true;
}

}  // namespace mce::editor
