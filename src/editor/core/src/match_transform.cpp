#include "mce/editor/match_transform.h"

#include <cmath>

#include "mce/ecs/transform.h"

namespace mce::editor {

namespace {

bool near(f32 a, f32 b) { return std::fabs(a - b) <= 1e-6f; }

}  // namespace

usize match_transforms(EditorScene& scene, Span<const EntityId> ids, EntityId anchor, MatchPart part) {
    const LocalTransform* a = scene.get<LocalTransform>(anchor);
    if (a == nullptr) return 0;
    const LocalTransform source = *a;  // a copy: the sets below move the storage
    const WorldTransform source_world = scene.world_transform(anchor);

    struct Change {
        EntityId id;
        LocalTransform value;
    };
    Array<Change> changes;
    for (EntityId id : ids) {
        if (id == anchor) continue;
        const LocalTransform* t = scene.get<LocalTransform>(id);
        if (t == nullptr) continue;
        bool seen = false;
        for (const Change& c : changes) seen = seen || c.id == id;
        if (seen) continue;
        LocalTransform next = *t;
        bool differs = false;
        switch (part) {
            case MatchPart::Position: {
                WorldTransform w = scene.world_transform(id);
                differs = !near(w.position.x, source_world.position.x) || !near(w.position.y, source_world.position.y) ||
                          !near(w.position.z, source_world.position.z);
                w.position = source_world.position;
                next = scene.local_for_world(id, w);
                break;
            }
            case MatchPart::Rotation:
                differs = !near(t->rotation.x, source.rotation.x) || !near(t->rotation.y, source.rotation.y) ||
                          !near(t->rotation.z, source.rotation.z) || !near(t->rotation.w, source.rotation.w);
                next.rotation = source.rotation;
                break;
            case MatchPart::Scale:
                differs = !near(t->scale.x, source.scale.x) || !near(t->scale.y, source.scale.y) ||
                          !near(t->scale.z, source.scale.z);
                next.scale = source.scale;
                break;
        }
        if (differs) changes.push_back({id, next});
    }
    if (changes.empty()) return 0;
    scene.begin_edit("Match transform");
    for (const Change& c : changes) scene.set(c.id, c.value);
    scene.end_edit();
    return changes.size();
}

}  // namespace mce::editor
