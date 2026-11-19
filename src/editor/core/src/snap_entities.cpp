#include "mce/editor/snap_entities.h"

#include <cmath>

#include "mce/ecs/transform.h"
#include "mce/editor/gizmo.h"
#include "mce/editor/parents_first.h"

namespace mce::editor {

usize snap_entities(EditorScene& scene, Span<const EntityId> ids, f32 step) {
    if (!(step > 0.0f)) return 0;
    struct Target {
        EntityId id;
        WorldTransform world;
    };
    Array<Target> moves;  // computed before anything moves: a parent that snaps must not shift its children's target
    usize changed = 0;    // entities that were off the grid
    for (EntityId id : ids) {
        if (!scene.get<LocalTransform>(id)) continue;
        bool seen = false;
        for (const Target& t : moves) seen = seen || t.id == id;
        if (seen) continue;
        WorldTransform w = scene.world_transform(id);
        const f32 x = snap_value(w.position.x, step), y = snap_value(w.position.y, step);
        if (std::fabs(x - w.position.x) > 1e-6f || std::fabs(y - w.position.y) > 1e-6f) ++changed;
        w.position.x = x;
        w.position.y = y;
        moves.push_back({id, w});
    }
    if (changed == 0) return 0;
    // Every entity is placed, parents first: a parent that snaps carries its children, an on-grid one among them.
    sort_parents_first(scene, moves, [](const Target& t) { return t.id; });
    scene.begin_edit("Snap to grid");
    for (const Target& t : moves) scene.set(t.id, scene.local_for_world(t.id, t.world));
    scene.end_edit();
    return changed;
}

}  // namespace mce::editor
