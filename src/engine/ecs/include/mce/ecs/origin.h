#pragma once
// Floating origin (M-OPENWORLD A2, docs/milestones/M-OPENWORLD-A-design.md): the world rebase.
//
// Positions are f32 everywhere, so a world far from its origin loses precision (0.24 mm of float spacing at
// 2 km, 4 mm at 32 km): the render wobbles, contacts degrade. A rebase moves the whole simulated world by a
// whole number of `snap`-metre cells so play stays near zero; the WorldOrigin resource remembers how far it
// moved, so an absolute position is `local + cell * snap` (to_absolute / to_local, in f64).
//
//   install_world_origin(world, 256.0f);            // once (scene::FloatingOrigin3D does it for a scene)
//   on_origin_shift(world, &my_shift, &my_state);   // a module whose caches hold world positions
//   rebase_world(world, cells);                     // between ticks: everything moves by -cells * snap
//
// rebase_world subtracts the shift from every root LocalTransform (an entity without ChildOf), from every
// WorldTransform (its position and matrix), and from every reflected field flagged WorldPoint (a Vec3, or a
// fixed array of them, holding a world-space point). The writes do not stamp change ticks, so a rebase does
// not look like an edit or a teleport; caches outside the components (physics bodies, navigation, audio
// history) follow through the listeners, called in registration order after the components moved.
// A world that never installs the origin is never touched.

#include "mce/ecs/entity.h"
#include "mce/math/math.h"

namespace mce {

class World;

struct WorldPoint64 {  // an absolute position (the math library has no f64 vector)
    f64 x = 0.0, y = 0.0, z = 0.0;
};

// World resource. Not part of World::state_hash (resources are not), but every rebase moves hashed positions.
struct WorldOrigin {
    i32 cell[3] = {0, 0, 0};  // the origin, in `snap` units
    f32 snap = 256.0f;        // metres per cell (a power of two keeps world-anchored textures seamless)
    u64 serial = 0;           // bumped by every rebase
    math::vec3 last_delta{};  // the last shift: local positions moved by -last_delta
};

using OriginShiftFn = void (*)(World& world, const math::vec3& delta, void* user);

// Adds the WorldOrigin resource (once; a later call keeps the origin and only sets `snap` while the origin
// is still zero). Returns it.
WorldOrigin& install_world_origin(World& world, f32 snap = 256.0f);
// The resource, or null when the world never installed it.
WorldOrigin* world_origin(World& world);

// Listeners for caches that hold world positions outside components. A (fn, user) pair registers once.
void on_origin_shift(World& world, OriginShiftFn fn, void* user = nullptr);
void remove_origin_shift(World& world, OriginShiftFn fn, void* user = nullptr);

// Moves the world by -cells * snap (installs the origin when missing). Outside phases only. A zero shift
// does nothing.
void rebase_world(World& world, const i32 cells[3]);

// Subtracts `delta` from these entities' root LocalTransforms, WorldTransforms and WorldPoint fields, with no
// listener call and no serial bump: entities that arrive authored in absolute coordinates after a rebase (a
// streamed chunk) are placed with delta = the origin's offset (origin_offset).
void shift_entities(World& world, Span<const Entity> entities, const math::vec3& delta);

// The origin as a local-space offset (cell * snap), zero without the resource.
math::vec3 origin_offset(World& world);
WorldPoint64 to_absolute(World& world, const math::vec3& local);
math::vec3 to_local(World& world, const WorldPoint64& absolute);

}  // namespace mce
