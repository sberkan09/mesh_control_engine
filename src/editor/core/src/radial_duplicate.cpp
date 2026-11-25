#include "mce/editor/radial_duplicate.h"

#include <cmath>

#include "mce/ecs/transform.h"
#include "mce/editor/duplicate_offset.h"

namespace mce::editor {

namespace {

constexpr f32 kPi = 3.14159265358979f;

bool contains(const Array<EntityId>& list, EntityId id) {
    for (EntityId other : list)
        if (other == id) return true;
    return false;
}

}  // namespace

bool radial_params_valid(const RadialParams& p) {
    const f32 all[] = {p.center_x, p.center_y, p.total_degrees};
    for (f32 v : all)
        if (!std::isfinite(v)) return false;
    return p.count >= 2 && p.count <= kMaxRadialCopies + 1 && p.total_degrees > 0.0f && p.total_degrees <= 3600.0f;
}

usize radial_duplicate(EditorScene& scene, Span<const EntityId> ids, const RadialParams& p, Array<EntityId>* created) {
    if (!radial_params_valid(p)) return 0;
    Array<EntityId> originals;  // document order, the order duplicate_offset hands the copies back in
    for (const DocEntity& e : scene.doc().entities())
        for (EntityId id : ids)
            if (id == e.id) {
                originals.push_back(id);
                break;
            }
    if (originals.empty() || (static_cast<usize>(p.count) - 1) * originals.size() > kMaxRadialCopies) return 0;
    Array<WorldTransform> start;
    for (EntityId id : originals) start.push_back(scene.world_transform(id));

    const bool ring = p.total_degrees >= 360.0f;
    const f32 step = p.total_degrees / static_cast<f32>(ring ? p.count : p.count - 1) * (kPi / 180.0f);
    Array<EntityId> all;
    scene.begin_edit("Radial duplicate");
    for (u32 k = 1; k < p.count; ++k) {
        Array<EntityId> made;
        duplicate_offset(scene, ids, 0.0f, 0.0f, &made);
        const bool paired = made.size() == originals.size();
        const f32 a = step * static_cast<f32>(k), c = std::cos(a), s = std::sin(a);
        for (usize i = 0; i < made.size(); ++i) {
            const EntityId id = made[i];
            all.push_back(id);
            if (contains(made, scene.parent_of(id))) continue;  // a copy under a copied parent travels with it
            if (!scene.get<LocalTransform>(id)) continue;
            WorldTransform w = paired ? start[i] : scene.world_transform(id);
            const f32 rx = w.position.x - p.center_x, ry = w.position.y - p.center_y;
            w.position.x = p.center_x + rx * c - ry * s;
            w.position.y = p.center_y + rx * s + ry * c;
            if (p.turn) w.rotation = rotation_z(angle_z(w.rotation) + a);
            scene.set(id, scene.local_for_world(id, w));
        }
    }
    scene.end_edit();
    if (created != nullptr)
        for (EntityId id : all) created->push_back(id);
    return all.size();
}

}  // namespace mce::editor
