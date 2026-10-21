#pragma once
// Entity names from the scenes a world has loaded (M-WORKFLOW 7, #382).
//
// The cook keeps each entity's "name" in the scene chunk; instantiate used to drop it. Now a scene
// with named entities fills the EntityNames resource, so code can find "Player" without a link set
// in the scene:
//
//   Entity e = scene::find_entity(world, "Player");   // Entity::null when unknown or gone
//
// Names are not components: the resource adds no column, so a world's state hash is the same with or
// without it. Several entities may share a name; the first one in scene order (the earliest scene
// loaded, then entity order) that is still alive wins. Entities made at run time (spawn, prefab
// instances) have no name. A scene change clears the table before the next scene fills it again; a
// streamed chunk adds its names on load, and its entities stop matching once they are destroyed.

#include "mce/string_id.h"
#include "mce/ecs/world.h"

namespace mce::scene {

struct EntityNames {
    struct Row {
        u64 hash = 0;
        Entity entity = Entity::null;
    };
    Array<Row> rows;  // sorted by hash; equal names keep insertion order
};

// The first live entity named `name`, or Entity::null.
Entity find_entity(World& world, StringView name);

}  // namespace mce::scene
