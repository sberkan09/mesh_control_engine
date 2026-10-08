#pragma once
// Types both the test host and the test game modules name. Identity across the module boundary
// is the type name (kTypeHash<T>), never a per-image static (scripting.md §3.1, finding F11).

#include "mce/ecs/entity.h"
#include "mce/types.h"

namespace mce::modtest {

struct Health {
    i32 hp;
};
struct Velocity {
    f32 x, y;
};

// Written by the module's install(): what the module saw from its side of the boundary.
struct Probe {
    ecs::ComponentId health_id = ecs::kInvalidComponent;
    ecs::ComponentId velocity_id = ecs::kInvalidComponent;
    i32 host_cvar = -1;  // a convar the HOST registered, read by the module through the engine
    u32 installs = 0;
};

inline constexpr i32 kStartHp = 100;
inline constexpr u32 kSpawned = 3;

}  // namespace mce::modtest
