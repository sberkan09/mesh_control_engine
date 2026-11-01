// Impact sparks (M-IMPACTFX): a landed shot moves the ImpactFx3D emitter to the hit point and bursts one tick later.

#include <doctest/doctest.h>

#include "mce/ecs/transform.h"
#include "mce/ecs/world.h"
#include "mce/kit/combat3d.h"
#include "mce/kit/effects.h"
#include "mce/kit/impact_fx.h"
#include "mce/kit/kit3d.h"

using namespace mce;
using namespace mce::kit;

namespace {

constexpr f32 kDt = 1.0f / 60.0f;

LocalTransform at(f32 x, f32 y, f32 z) { return LocalTransform{{x, y, z}, math::quat::identity(), {1, 1, 1}}; }

struct Scene {
    World w;
    u64 t = 0;
    Entity fx;
    Scene() {
        w.register_components<LocalTransform>();
        install_combat3d(w, nullptr);
        install_effects(w, nullptr);
        install_impact_fx(w);
        ImpactFx3D i;
        i.count = 7;
        fx = w.spawn_with(at(0, 0, 0), i);
        run(2);
    }
    void run(u32 n) {
        for (u32 i = 0; i < n; ++i) w.tick(kDt, t++);
    }
    void shot(math::vec3 point, math::vec3 normal) {
        WeaponHit3D h;
        h.point = point;
        h.normal = normal;
        EventAccess(w).push(h);
    }
    const ImpactFx3D& i() { return *w.try_get<ImpactFx3D>(fx); }
};

}  // namespace

TEST_CASE("impact_fx: the emitter moves to the hit point, lifted along the normal, and bursts one tick later") {
    Scene s;
    s.shot({10, 2, -5}, {0, 0, 1});
    s.run(1);
    const LocalTransform& t = *s.w.try_get<LocalTransform>(s.fx);
    CHECK(t.position.x == doctest::Approx(10.0f));
    CHECK(t.position.y == doctest::Approx(2.0f));
    CHECK(t.position.z == doctest::Approx(-5.0f + 0.03f));
    CHECK(s.i().pending == 7);
    CHECK(s.i().fired == 0);
    s.run(1);
    CHECK(s.i().pending == 0);
    CHECK(s.i().fired == 1);
    s.run(10);
    CHECK(s.i().fired == 1);  // no hit, no burst
}

TEST_CASE("impact_fx: two hits in one tick show the last one") {
    Scene s;
    s.shot({1, 0, 0}, {0, 1, 0});
    s.shot({2, 0, 0}, {0, 1, 0});
    s.run(2);
    CHECK(s.w.try_get<LocalTransform>(s.fx)->position.x == doctest::Approx(2.0f));
    CHECK(s.i().fired == 1);
}
