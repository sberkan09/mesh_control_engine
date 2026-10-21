#pragma once
// Shared scene builders for test_physics2d.

#include <doctest/doctest.h>

#include "mce/physics/world2d.h"

namespace phys_test {

using namespace mce;
using namespace mce::physics;
using math::vec2;

inline constexpr f32 kDt = 1.0f / 60.0f;

inline Entity ent(u32 index) { return Entity{index, 1}; }

inline bool add_box(World2D& w, Entity e, BodyType type, vec2 pos, vec2 half, f32 friction = 0.6f,
                    f32 restitution = 0.0f, f32 angle = 0.0f) {
    BodyDesc d;
    d.body.type = type;
    d.collider.shape = ColliderShape::box(half);
    d.collider.friction = friction;
    d.collider.restitution = restitution;
    d.position = pos;
    d.angle = angle;
    return w.create_body(e, d);
}

inline bool add_circle(World2D& w, Entity e, BodyType type, vec2 pos, f32 radius, f32 friction = 0.6f,
                       f32 restitution = 0.0f) {
    BodyDesc d;
    d.body.type = type;
    d.collider.shape = ColliderShape::circle(radius);
    d.collider.friction = friction;
    d.collider.restitution = restitution;
    d.position = pos;
    return w.create_body(e, d);
}

inline BodyState state(const World2D& w, Entity e) {
    BodyState s;
    REQUIRE(w.body_state(e, s));
    return s;
}

}  // namespace phys_test
