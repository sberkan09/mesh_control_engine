#pragma once
// Shared scaffolding for the World3D tests.

#include <doctest/doctest.h>

#include "mce/physics3d/world3d.h"
#include "test_support.h"

namespace phys3_test {

using namespace mce;
using namespace mce::physics;

inline Entity ent(u32 i) { return Entity{i, 1}; }

constexpr f32 kDt = 1.0f / 60.0f;

// A 100 x 1 x 100 static floor whose top face is y = 0.
inline void add_floor(World3D& w, Entity e = ent(1000)) {
    BodyDesc3D d;
    d.shape = Shape3D::box({50.0f, 0.5f, 50.0f});
    d.position = {0.0f, -0.5f, 0.0f};
    REQUIRE(w.create_body(e, d));
}

inline bool add_box(World3D& w, Entity e, BodyType3D type, vec3 pos, vec3 half, quat rot = {}) {
    BodyDesc3D d;
    d.type = type;
    d.shape = Shape3D::box(half);
    d.position = pos;
    d.rotation = rot;
    return w.create_body(e, d);
}

inline vec3 position_of(const World3D& w, Entity e) {
    BodyState3D s;
    REQUIRE(w.body_state(e, s));
    return s.position;
}

inline bool has(u32 flags, u32 f) { return (flags & f) != 0; }

}  // namespace phys3_test
