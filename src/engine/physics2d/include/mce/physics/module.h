#pragma once
// ECS integration for mce_physics2d (physics.md §3.1, §3.3).
//
// install_physics2d(world) registers the physics components and events, the World2D resource,
// the on_remove hooks that free bodies at sync points, and exactly one system:
//
//   FixedUpdate_Physics  physics_step
//     1 IMPORT  entities with RigidBody2D + Collider2D, in entity-index order: create missing
//               bodies (pose from LocalTransform), push changed component values in place,
//               add CharacterGround2D to controllers that lack it (applied at the sync point)
//     2 STEP    World2D::step(frame.dt)
//     3 EXPORT  dynamic/kinematic poses -> LocalTransform, controller results -> CharacterGround2D
//     4 EVENTS  TriggerEnter/Exit, ContactBegin2D/End2D -> EventQueue<T> (readable in Post)
//
// Gameplay drives bodies from FixedUpdate_Pre through f.resource<physics::World2D>():
// set_linear_velocity / apply_impulse / teleport / move_and_collide.

#include "mce/ecs/world.h"
#include "mce/physics/world2d.h"

namespace mce::physics {

void install_physics2d(World& world, const PhysicsConfig& config = {});

// The system install_physics2d registers (exposed for tests and custom pipelines).
void physics_step(Frame& f);

}  // namespace mce::physics
