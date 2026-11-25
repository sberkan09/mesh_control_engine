#include "mce/editor/transform_group.h"

#include <algorithm>
#include <cmath>

#include "mce/ecs/transform.h"
#include "mce/editor/parents_first.h"

namespace mce::editor {

namespace {
constexpr f32 kPi = 3.14159265358979f;
}

bool group_transform_valid(const GroupTransformParams& p) {
    const f32 all[] = {p.degrees, p.scale, p.pivot_x, p.pivot_y};
    for (f32 v : all)
        if (!std::isfinite(v)) return false;
    return p.scale > 0.0f;
}

usize transform_group(EditorScene& scene, Span<const EntityId> ids, const GroupTransformParams& p) {
    if (!group_transform_valid(p)) return 0;
    if (p.degrees == 0.0f && p.scale == 1.0f) return 0;
    struct Target {
        EntityId id;
        WorldTransform world;
    };
    Array<Target> targets;  // computed before anything moves: a parent that moves must not shift its children's target
    for (EntityId id : ids) {
        if (!scene.get<LocalTransform>(id)) continue;
        bool seen = false;
        for (const Target& t : targets) seen = seen || t.id == id;
        if (!seen) targets.push_back({id, scene.world_transform(id)});
    }
    if (targets.empty()) return 0;

    f32 px = p.pivot_x, py = p.pivot_y;
    if (!p.use_pivot) {
        f32 lo_x = targets[0].world.position.x, hi_x = lo_x, lo_y = targets[0].world.position.y, hi_y = lo_y;
        for (const Target& t : targets) {
            lo_x = std::min(lo_x, t.world.position.x);
            hi_x = std::max(hi_x, t.world.position.x);
            lo_y = std::min(lo_y, t.world.position.y);
            hi_y = std::max(hi_y, t.world.position.y);
        }
        px = 0.5f * (lo_x + hi_x);
        py = 0.5f * (lo_y + hi_y);
    }
    const f32 a = p.degrees * (kPi / 180.0f), c = std::cos(a), s = std::sin(a);
    for (Target& t : targets) {
        WorldTransform& w = t.world;
        const f32 rx = (w.position.x - px) * p.scale, ry = (w.position.y - py) * p.scale;
        w.position.x = px + rx * c - ry * s;
        w.position.y = py + rx * s + ry * c;
        w.rotation = rotation_z(angle_z(w.rotation) + a);
        w.scale.x *= p.scale;
        w.scale.y *= p.scale;
        w.scale.z *= p.scale;
    }
    sort_parents_first(scene, targets, [](const Target& t) { return t.id; });
    scene.begin_edit("Turn and resize group");
    for (const Target& t : targets) scene.set(t.id, scene.local_for_world(t.id, t.world));
    scene.end_edit();
    return targets.size();
}

}  // namespace mce::editor
