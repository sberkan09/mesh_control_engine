#pragma once
// scene_stats / format_stats — what a scene document holds at a glance (M-SCENESTATS): how many entities, how deep the
// hierarchy goes, how many prefab instances, which components are used by how many entities, and the box the entities'
// local positions span. Headless, document-level (no ECS), tested in CI.

#include "mce/containers/array.h"
#include "mce/cook/string.h"
#include "mce/editor/scene_document.h"

namespace mce::editor {

struct ComponentUse {
    const refl::TypeRecord* type = nullptr;
    u32 entities = 0;  // how many entities carry it
};

struct SceneStats {
    u32 entities = 0;
    u32 roots = 0;             // entities without a (valid) parent
    u32 max_depth = 0;         // 0: no entity has a parent
    u32 prefab_instances = 0;  // entities linked to a prefab
    Array<ComponentUse> components;  // most used first, ties by type name
    bool has_bounds = false;         // at least one entity has a LocalTransform
    math::vec3 min, max;             // the box the LocalTransform positions span (local space, not world)
};

SceneStats scene_stats(const SceneDocument& doc);

// A short report, one line per fact: `entities: 12 (3 roots, depth 2, 1 prefab instance)`, `bounds: ...`,
// then `Sprite: 8` per component, most used first.
cook::String format_stats(const SceneStats& stats);

}  // namespace mce::editor
