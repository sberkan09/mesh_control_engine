#pragma once
// 3D physics queries as data (M-GAMEPLAY3D step 6): a ray and an overlap the engine runs every
// tick from an entity, so a stock-player game, a sequence or an agent sees what is in front of or
// around something without code. kit::install_stock installs them.
//
//   Raycast3D  a ray (or a sphere cast) from the entity: the closest hit within `distance`
//   Overlap3D  a sphere or a box at the entity: every body and character touching it
//
// Each sensor pushes TriggerEnter3D { sensor, other } when something comes into it and
// TriggerExit3D when it leaves (or either is destroyed, or the sensor is switched off), the events
// a Trigger3D pushes. So the sensor's and the other's PhysicsEvents count them (trigger_enters,
// trigger_exits, overlapping, last_trigger) and effects with FireOn::TriggerEnter fire on them.
// Unlike Trigger3D, which tests body centres, sensors test the colliders' real shapes, static ones
// included, and characters' capsules. A sensor never sees its own entity or the entities it is
// attached under (ChildOf).
//
// System: "kit3d_sensors" (FixedUpdate_Physics, after "kit3d_triggers").

#include "mce/ecs/entity.h"
#include "mce/kit/bodies3d.h"
#include "mce/math/math.h"
#include "mce/refl/macros.h"

namespace mce {
class World;
}

namespace mce::asset {
class AssetSystem;
}

namespace mce::kit {

struct Raycast3D {
    math::vec3 offset;                   // start, in entity space
    math::vec3 direction{0, 0, -1};      // in entity space (forward is -Z)
    f32 distance = 10.0f;                // metres
    f32 radius = 0.0f;                   // > 0: a sphere cast of this radius (bodies only)
    u16 mask = 0xFFFF;                   // physics layers it hits
    u8 enabled = 1;
    u8 dynamic = 1;                      // hits dynamic bodies
    u8 characters = 1;                   // hits CharacterController3D capsules (a ray only)
    u8 pad_[3] = {};
    Entity hit;                          // engine-written: what it hits now (null: nothing)
    math::vec3 point;                    // engine-written: where, world space
    math::vec3 normal;                   // engine-written: the surface's normal there
    f32 hit_distance = 0.0f;             // engine-written: metres from the start
    u32 hits = 0;                        // engine-written: times something new came into the ray
};
MCE_COMPONENT(Raycast3D, (offset, Help{"start, entity space"}), (direction, Help{"entity space; forward is -Z"}),
              (distance, Range{0.01f, 10000}), (radius, Range{0, 100}, Help{"> 0 casts a sphere (bodies only)"}),
              (mask, Help{"physics layers it hits"}), enabled, (dynamic, Help{"hits dynamic bodies"}),
              (characters, Help{"hits character capsules (a ray only)"}), (hit, ReadOnly, Transient), (point, ReadOnly, Transient),
              (normal, ReadOnly, Transient), (hit_distance, ReadOnly, Transient), (hits, ReadOnly, Transient))

struct Overlap3D {
    TriggerShape3D shape = TriggerShape3D::Sphere;
    u8 enabled = 1;
    u8 dynamic = 1;                      // sees dynamic bodies
    u8 characters = 1;                   // sees CharacterController3D capsules
    u16 mask = 0xFFFF;                   // physics layers it sees
    u16 pad_ = 0;
    math::vec3 half{0.5f, 0.5f, 0.5f};   // Box half extents, entity space (turned and scaled with it)
    f32 radius = 0.5f;                   // Sphere, scaled by the entity's largest scale
    math::vec3 offset;                   // centre, entity space
    u32 count = 0;                       // engine-written: entities touching it now
    u32 enters = 0;                      // engine-written
    Entity last_other;                   // engine-written: the last to come in or leave
};
MCE_COMPONENT(Overlap3D, shape, enabled, (dynamic, Help{"sees dynamic bodies"}), (characters, Help{"sees character capsules"}),
              (mask, Help{"physics layers it sees"}), (half, Help{"Box half extents, entity space"}), (radius, Range{0.01f, 1000}),
              (offset, Help{"centre, entity space"}), (count, ReadOnly, Transient), (enters, ReadOnly, Transient),
              (last_other, ReadOnly, Transient))

// Registers the components and adds "kit3d_sensors" (after install_bodies3d, which it calls). Idempotent.
void install_sensors3d(World& world, asset::AssetSystem* assets);

}  // namespace mce::kit
