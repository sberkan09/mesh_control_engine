#pragma once
// Gameplay debug lines (M-DEBUGVIEW 1): one data call that turns what the 3D gameplay components say
// into world-space line segments, the engine's answer to a gameplay debugger. Read-only: it only
// reads the World, so a run's state hash is the same with or without it.
//
//   Colliders  BoxCollider3D boxes (with `offset` and `pitch_deg`)
//   Bodies     RigidBody3D shapes: Box, Sphere (three circles), Capsule (caps and four side lines)
//   Triggers   Trigger3D boxes and spheres, green while something is `inside`, dim otherwise
//   Joints     Joint3D anchors (a cross) and axes, a line to the connected body or to
//              `connected_anchor`; red once broken
//   Characters CharacterController3D capsules, yellow in the air
//   Vehicles   Vehicle3D wheels (a line across each tyre, red while it skids)
//   Paths      Waypoint3D chains (a line per `next`) and the line from a VehicleDriver3D's car to its
//              `target`; NavAgent3D from the agent to `next_corner`
//
// MeshCollider3D and navmesh edges are not drawn yet.

#include "mce/containers/array.h"
#include "mce/ecs/entity.h"
#include "mce/math/math.h"
#include "mce/types.h"

namespace mce {
class World;
}

namespace mce::kit {

enum GameplayCategory : u32 {
    kDebugColliders = 1u << 0,
    kDebugBodies = 1u << 1,
    kDebugTriggers = 1u << 2,
    kDebugJoints = 1u << 3,
    kDebugCharacters = 1u << 4,
    kDebugVehicles = 1u << 5,
    kDebugPaths = 1u << 6,
    kDebugAll = 0x7Fu,
};

struct GameplayLine {
    math::vec3 a;
    math::vec3 b;
    math::Color color;
    u32 category = 0;  // the single GameplayCategory bit that produced it
};

// Clears `out` and fills it with the lines of the categories set in `mask`, entities in id order.
void gameplay_debug_lines(World& world, u32 mask, Array<GameplayLine>& out);

// The category bit for a name ("colliders", "bodies", "triggers", "joints", "characters", "vehicles",
// "paths"); 0 for anything else.
u32 gameplay_category_bit(const char* name);

}  // namespace mce::kit
