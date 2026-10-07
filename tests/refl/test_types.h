#pragma once
// Reflected test types shared by two translation units (registration must happen exactly once).

#include "mce/refl/refl.h"

namespace refl_test {

using namespace mce;

// Stand-in for a higher-layer handle type (mce_ecs declares mce::Entity the same way).
struct FakeEntity {
    u32 index = 0;
    u32 generation = 0;
};

enum class EnemyState : u8 { Idle, Patrol, Chase = 5, Attack };
MCE_ENUM(EnemyState, Idle, Patrol, Chase, Attack)

enum class Unreflected : i32 { A, B };

struct Waypoint {
    math::vec3 pos;
    f32 wait = 0.5f;
};
MCE_REFL_STRUCT(Waypoint, pos, (wait, Range{0, 10}))

struct Health {
    f32 hp = 100.0f;
    f32 max = 100.0f;
    u8 armor = 3;
};
MCE_COMPONENT(Health,
    (hp, Range{0, 1000}, Save, Help{"current hit points"}),
    max,
    (armor, ReadOnly, Transient))

struct Patrol {
    EnemyState state = EnemyState::Patrol;
    Unreflected mode = Unreflected::B;
    Waypoint waypoints[4];
    Waypoint home;
    math::Color tint = math::Color::white();
    math::quat facing;
    FixedString<16> label = "guard";
    FakeEntity target;
    i64 seed = -7;
    bool active = true;
    f64 timer = 0.25;
    u16 lanes[3] = {1, 2, 3};
};

struct Tag {};
MCE_COMPONENT(Tag)

}  // namespace refl_test

template <>
struct mce::refl::FieldTraits<refl_test::FakeEntity> {
    static constexpr bool supported = true;
    static constexpr FieldKind kind = FieldKind::EntityRef;
    static constexpr const char* type_name = "FakeEntity";
};

namespace refl_test {
MCE_COMPONENT(Patrol, state, mode, waypoints, home, tint, facing, label, target, seed, active, timer, lanes)
}  // namespace refl_test
