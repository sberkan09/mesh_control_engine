#include "@GAME@.h"

#include <cmath>

namespace @GAME@ {

using namespace mce;

namespace {

void spin(Frame& f) {
    f.query<const Spin&, LocalTransform&>().each([&](const Spin& s, LocalTransform& t) {
        const f32 radians = s.degrees_per_second * (math::kPi / 180.0f) * f.dt;
        t.rotation = math::quat_from_axis_angle({0, 0, 1}, radians) * t.rotation;
    });
}

void bob(Frame& f) {
    f.query<Bob&, LocalTransform&>().each([&](Bob& b, LocalTransform& t) {
        if (!b.started) {
            b.base_y = t.position.y;
            b.started = true;
        }
        b.time += f.dt;
        const f32 period = b.period > 0.0f ? b.period : 1.0f;
        t.position.y = b.base_y + b.height * std::sin(b.time * (2.0f * math::kPi / period));
    });
}

}  // namespace

ResultV install(World& world) {
    world.register_components<Spin, Bob>();
    world.add_system(Phase::FixedUpdate_Pre, "@GAME@_spin", &spin);
    world.add_system(Phase::FixedUpdate_Pre, "@GAME@_bob", &bob, "@GAME@_spin");
    return ok();
}

}  // namespace @GAME@
