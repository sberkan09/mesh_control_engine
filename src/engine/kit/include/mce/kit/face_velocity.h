#pragma once
// A model that turns to face where a character moves (M-FACEMOVE). kit::install_stock installs it.
//
//   FaceVelocity3D  on a model entity (usually the child that holds the character mesh): each tick, while the
//                   CharacterController3D on `source` moves faster than `min_speed` m/s along the ground, the
//                   entity's LocalTransform rotation turns about Y towards the direction of that velocity at
//                   `turn_rate_deg` degrees per second (shortest way round). Standing still keeps the last facing.
//                   `yaw` is the heading (0 faces -Z, the engine's forward) and `yaw_offset` is added when writing the
//                   rotation, for a mesh that faces +Z (the Kenney characters: pi). A source that sits in a car
//                   (a seated Rider3D) is ignored.
//
// Only the rotation is written; position and scale stay as authored. System: "kit_face_velocity"
// (FixedUpdate_Pre, after "kit3d_bodies").

#include "mce/ecs/entity.h"
#include "mce/math/math.h"
#include "mce/refl/macros.h"

namespace mce {
class World;
}

namespace mce::kit {

struct FaceVelocity3D {
    Entity source;                // the entity that holds the CharacterController3D
    f32 turn_rate_deg = 540.0f;   // degrees per second
    f32 min_speed = 0.4f;         // m/s on the ground below which the facing is kept
    f32 yaw_offset = 0.0f;        // radians added to the heading (pi for a mesh that faces +Z)
    f32 yaw = 0.0f;               // the current heading in radians (engine-written; authored = the start facing)
};
MCE_COMPONENT(FaceVelocity3D, (source, Help{"the entity with the CharacterController3D whose velocity this model faces"}),
              (turn_rate_deg, Save, Range{1, 3600}), (min_speed, Save, Range{0, 20}), (yaw_offset, Save, Range{-7, 7}),
              (yaw, Save, Help{"heading in radians; 0 faces -Z"}))

// Registers the component and adds the system. Idempotent.
void install_face_velocity(World& world);

}  // namespace mce::kit
