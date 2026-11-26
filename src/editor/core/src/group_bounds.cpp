#include "mce/editor/group_bounds.h"

#include <algorithm>
#include <cmath>

#include "mce/ecs/transform.h"

namespace mce::editor {

GroupBounds group_bounds(const EditorScene& scene, Span<const EntityId> ids) {
    GroupBounds b;
    Array<EntityId> seen;
    for (EntityId id : ids) {
        if (!scene.get<LocalTransform>(id)) continue;
        bool dup = false;
        for (EntityId other : seen) dup = dup || other == id;
        if (dup) continue;
        seen.push_back(id);
        const WorldTransform w = scene.world_transform(id);
        if (b.count == 0) {
            b.min_x = b.max_x = w.position.x;
            b.min_y = b.max_y = w.position.y;
        } else {
            b.min_x = std::min(b.min_x, w.position.x);
            b.max_x = std::max(b.max_x, w.position.x);
            b.min_y = std::min(b.min_y, w.position.y);
            b.max_y = std::max(b.max_y, w.position.y);
        }
        ++b.count;
    }
    if (b.count == 0) return b;
    b.center_x = 0.5f * (b.min_x + b.max_x);
    b.center_y = 0.5f * (b.min_y + b.max_y);
    b.width = b.max_x - b.min_x;
    b.height = b.max_y - b.min_y;
    b.diagonal = std::sqrt(b.width * b.width + b.height * b.height);
    return b;
}

}  // namespace mce::editor
