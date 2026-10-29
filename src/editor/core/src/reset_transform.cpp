#include "mce/editor/reset_transform.h"

#include <cmath>

#include "mce/ecs/transform.h"

namespace mce::editor {

namespace {

bool near(f32 a, f32 b) { return std::fabs(a - b) <= 1e-6f; }

bool is_identity(const LocalTransform& t) {
    return near(t.position.x, 0.0f) && near(t.position.y, 0.0f) && near(t.position.z, 0.0f) &&
           near(t.rotation.x, 0.0f) && near(t.rotation.y, 0.0f) && near(t.rotation.z, 0.0f) &&
           near(t.rotation.w, 1.0f) && near(t.scale.x, 1.0f) && near(t.scale.y, 1.0f) && near(t.scale.z, 1.0f);
}

}  // namespace

usize reset_transforms(EditorScene& scene, Span<const EntityId> ids) {
    Array<EntityId> todo;
    for (EntityId id : ids) {
        const LocalTransform* t = scene.get<LocalTransform>(id);
        if (t == nullptr || is_identity(*t)) continue;
        bool seen = false;
        for (EntityId other : todo) seen = seen || other == id;
        if (!seen) todo.push_back(id);
    }
    if (todo.empty()) return 0;
    scene.begin_edit("Reset transform");
    for (EntityId id : todo) {
        LocalTransform identity;
        identity.rotation = {0.0f, 0.0f, 0.0f, 1.0f};
        scene.set(id, identity);
    }
    scene.end_edit();
    return todo.size();
}

}  // namespace mce::editor
