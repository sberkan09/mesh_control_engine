// The 2D/3D mirror-signature rule (physics.md §2.2, D7): both worlds spell their shared methods the
// same way. A rename on either side is a compile error here, not a code-review catch.

#include <type_traits>

#include "helpers.h"
#include "mce/physics/world2d.h"

namespace {

using mce::physics::World2D;
using mce::physics::World3D;

template <class W>
concept PhysicsWorld = requires { &W::step; &W::create_body; &W::destroy_body; &W::has_body;
                                  &W::set_linear_velocity; &W::teleport; &W::body_state; &W::raycast;
                                  &W::shape_cast; &W::controller_state; &W::hash_state; &W::stats; &W::config; };

static_assert(PhysicsWorld<World2D>);
static_assert(PhysicsWorld<World3D>);
// The two 3D additions (physics.md §2.2).
static_assert(requires { &World3D::move_character; &World3D::camera_sweep; });
// Same step contract: one fixed dt in, nothing out.
static_assert(std::is_same_v<decltype(&World2D::step), void (World2D::*)(mce::f32)>);
static_assert(std::is_same_v<decltype(&World3D::step), void (World3D::*)(mce::f32)>);
static_assert(std::is_same_v<decltype(&World2D::hash_state), mce::u64 (World2D::*)(mce::u64) const>);
static_assert(std::is_same_v<decltype(&World3D::hash_state), mce::u64 (World3D::*)(mce::u64) const>);

}  // namespace

TEST_CASE("mirror: 2D and 3D worlds share method spelling (compile-time)") { CHECK(true); }
