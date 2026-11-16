#include "mce/editor/snap_entities.h"

#include <cmath>

#include "mce/ecs/transform.h"
#include "mce/editor/gizmo.h"

namespace mce::editor {

usize snap_entities(EditorScene& scene, Span<const EntityId> ids, f32 step) {
    if (!(step > 0.0f)) return 0;
    struct Target {
        EntityId id;
        WorldTransform world;
    };
    Array<Target> moves;  // computed before anything moves: a parent that snaps must not shift its children's target
    for (EntityId id : ids) {
        if (!scene.get<LocalTransform>(id)) continue;
        bool seen = false;
        for (const Target& t : moves) seen = seen || t.id == id;
        if (seen) continue;
        WorldTransform w = scene.world_transform(id);
        const f32 x = snap_value(w.position.x, step), y = snap_value(w.position.y, step);
        if (std::fabs(x - w.position.x) <= 1e-6f && std::fabs(y - w.position.y) <= 1e-6f) continue;
        w.position.x = x;
        w.position.y = y;
        moves.push_back({id, w});
    }
    if (moves.empty()) return 0;
    scene.begin_edit("Snap to grid");
    for (const Target& t : moves) scene.set(t.id, scene.local_for_world(t.id, t.world));
    scene.end_edit();
    return moves.size();
}

}  // namespace mce::editor
