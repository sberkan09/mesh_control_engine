#pragma once
// Transform components (ecs-gameplay.md §2.1, §3.7).
//
// LocalTransform is the pose of record for an entity. 2D code uses position.xy and a rotation
// about +Z (see rotation_z / angle_z). For entities with a live physics body, physics is the
// authoritative writer during play (physics.md D2); everything else writes it directly.

#include <cmath>

#include "mce/math/math.h"

namespace mce {

struct LocalTransform {
    math::vec3 position;
    math::quat rotation;
    math::vec3 scale{1.0f, 1.0f, 1.0f};
};

// Rotation of `radians` about +Z (the 2D plane's rotation axis).
inline math::quat rotation_z(f32 radians) {
    return {0.0f, 0.0f, std::sin(radians * 0.5f), std::cos(radians * 0.5f)};
}

// Angle about +Z of a rotation produced by rotation_z (twist about Z for anything else).
inline f32 angle_z(const math::quat& q) { return 2.0f * std::atan2(q.z, q.w); }

}  // namespace mce
