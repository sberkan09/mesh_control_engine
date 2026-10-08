#pragma once
// ChildOf transform hierarchy (v0.5, ecs-gameplay.md §2.8/§3.7, FR-ENT-050).
//
// Users write ChildOf{parent}; the engine keeps the parent -> children index (a resource
// maintained by ChildOf hooks and reconciled against ChildOf writes) and writes WorldTransform.
// Propagation runs in FixedUpdate_Post as "transform_propagate":
//
//   seed  entities whose LocalTransform or ChildOf changed since the system's previous run, plus
//         entities that still lack a WorldTransform
//   order seeds by (depth, entity index) — ancestors first, deterministic
//   walk  depth-first from each unvisited seed: world = parent_world * local; every visited node
//         is marked, so a seed under an already-walked seed costs nothing
//
// Rotating one parent therefore re-composes exactly its subtree (HierarchyStats::propagated is
// the count the FR-ENT-050 test asserts). Destroying a parent destroys its subtree (the
// committed orphans-destroy semantic, ecs-gameplay.md §3.4). A ChildOf that would close a cycle
// is rejected: the entity stays a root and HierarchyStats::cycles_rejected counts it.

#include "mce/ecs/transform.h"
#include "mce/ecs/world.h"

namespace mce {

struct ChildOf {
    Entity parent;
};
MCE_COMPONENT(ChildOf, (parent, Save))

// Engine-written; read-only to gameplay. `matrix` = T * R * S of the three fields.
struct WorldTransform {
    math::vec3 position;
    math::quat rotation;
    math::vec3 scale{1.0f, 1.0f, 1.0f};
    math::mat4 matrix = math::mat4::identity();
};
MCE_COMPONENT(WorldTransform, (position, ReadOnly, Transient), (rotation, ReadOnly, Transient),
              (scale, ReadOnly, Transient))

namespace ecs {

struct HierarchyStats {
    u32 seeds = 0;            // distinct dirty entities found by the last propagation
    u32 propagated = 0;       // WorldTransforms composed by the last propagation
    u32 cycles_rejected = 0;  // cumulative
    u64 runs = 0;
};

// Registers LocalTransform/ChildOf/WorldTransform, the ChildOf hooks, the destroy hook and the
// "transform_propagate" system (FixedUpdate_Post, registered at the call — install before any
// FixedUpdate_Post system that reads WorldTransform). Idempotent.
void install_hierarchy(World& w);
bool hierarchy_installed(World& w);

// Runs propagation now, outside any phase (after a scene load / prefab instantiation, or in
// tools). Missing WorldTransforms are added immediately.
void propagate_transforms(World& w);

// Attaches (parent != null) or detaches (parent == null) `child` immediately. Outside phases.
// Returns false when the parent is dead or the link would create a cycle.
bool set_parent(World& w, Entity child, Entity parent);

Entity parent_of(World& w, Entity e);
Span<const Entity> children_of(World& w, Entity e);  // in attach order; empty when none
u32 depth_of(World& w, Entity e);                    // roots are 0
const HierarchyStats& hierarchy_stats(World& w);

// world = parent * local (scale composes component-wise; no shear).
WorldTransform compose(const WorldTransform& parent, const LocalTransform& local);
WorldTransform to_world(const LocalTransform& local);

}  // namespace ecs
}  // namespace mce
