// Tracer lines (M-TRACERS): a shot shows the next ring slot along its path; short shots draw nothing; the line hides
// after its life.

#include <doctest/doctest.h>

#include "mce/ecs/transform.h"
#include "mce/ecs/world.h"
#include "mce/kit/combat3d.h"
#include "mce/kit/kit3d.h"
#include "mce/kit/tracer3d.h"

using namespace mce;
using namespace mce::kit;

namespace {

constexpr f32 kDt = 1.0f / 60.0f;

struct Scene {
    World w;
    u64 t = 0;
    Entity slot[3];
    Scene() {
        w.register_components<LocalTransform, MeshRef>();
        install_combat3d(w, nullptr);
        install_tracers(w);
        for (Entity& e : slot) {
            MeshRef m;
            m.visible = 0;
            e = w.spawn_with(LocalTransform{}, m, Tracer3D{});
        }
        run(1);
    }
    void run(u32 n) {
        for (u32 i = 0; i < n; ++i) w.tick(kDt, t++);
    }
    void shoot(math::vec3 from, math::vec3 to) {
        WeaponFired3D e;
        e.origin = from;
        e.end = to;
        EventAccess(w).push(e);
    }
    const Tracer3D& tr(int i) { return *w.try_get<Tracer3D>(slot[i]); }
    const MeshRef& mr(int i) { return *w.try_get<MeshRef>(slot[i]); }
    const LocalTransform& lt(int i) { return *w.try_get<LocalTransform>(slot[i]); }
};

}  // namespace

TEST_CASE("tracers: a shot shows the next slot at its middle, turned and scaled along it") {
    Scene s;
    CHECK(s.mr(0).visible == 0);
    s.shoot({0, 1, 0}, {0, 1, -10});
    s.run(1);
    CHECK(s.mr(0).visible == 1);
    CHECK(s.tr(0).serial == 1);
    CHECK(s.lt(0).position.z == doctest::Approx(-5.0f));
    CHECK(s.lt(0).scale.z == doctest::Approx(10.0f));
    CHECK(s.lt(0).scale.x == doctest::Approx(0.04f));
    const math::vec3 along = math::rotate(s.lt(0).rotation, {0, 0, 1});
    CHECK(along.z == doctest::Approx(-1.0f).epsilon(0.001));

    s.shoot({0, 0, 0}, {10, 0, 0});  // sideways
    s.run(1);
    const math::vec3 side = math::rotate(s.lt(1).rotation, {0, 0, 1});
    CHECK(side.x == doctest::Approx(1.0f).epsilon(0.001));
    CHECK(s.lt(1).position.x == doctest::Approx(5.0f));

    s.shoot({0, 0, 0}, {0, 0, 10});  // straight +Z: no turn
    s.run(1);
    CHECK(math::rotate(s.lt(2).rotation, {0, 0, 1}).z == doctest::Approx(1.0f));
}

TEST_CASE("tracers: the ring reuses the oldest slot and two shots in a tick take two slots") {
    Scene s;
    s.shoot({0, 0, 0}, {5, 0, 0});
    s.shoot({0, 0, 0}, {6, 0, 0});
    s.run(1);
    CHECK(s.tr(0).serial == 1);
    CHECK(s.tr(1).serial == 2);
    s.shoot({0, 0, 0}, {7, 0, 0});
    s.run(1);
    s.shoot({0, 0, 0}, {8, 0, 0});  // full: the oldest (slot 0) goes
    s.run(1);
    CHECK(s.tr(0).serial == 4);
    CHECK(s.lt(0).scale.z == doctest::Approx(8.0f));
    CHECK(s.tr(1).serial == 2);
}

TEST_CASE("tracers: a short shot draws nothing and a tracer hides after its life") {
    Scene s;
    s.shoot({0, 0, 0}, {0.3f, 0, 0});
    s.run(1);
    CHECK(s.mr(0).visible == 0);
    CHECK(s.tr(0).serial == 0);

    s.shoot({0, 0, 0}, {5, 0, 0});
    s.run(1);
    CHECK(s.mr(0).visible == 1);
    s.run(2);  // 0.05 s
    CHECK(s.mr(0).visible == 1);
    s.run(3);  // past 0.06 s
    CHECK(s.mr(0).visible == 0);
    CHECK(s.tr(0).age < 0.0f);
}

TEST_CASE("tracers: no slots is harmless") {
    World w;
    w.register_components<LocalTransform, MeshRef>();
    install_combat3d(w, nullptr);
    install_tracers(w);
    WeaponFired3D e;
    e.end = {0, 0, -10};
    EventAccess(w).push(e);
    w.tick(kDt, 0);
    CHECK(true);
}
