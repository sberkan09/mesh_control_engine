// Hit marker (M-HITMARKER): a landed shot sets HitMarker3D.timer, a kill sets kill_timer, both decay, and only the
// shooter's own hits on another living Health3D count.

#include <doctest/doctest.h>

#include "mce/ecs/transform.h"
#include "mce/ecs/world.h"
#include "mce/kit/combat3d.h"
#include "mce/kit/hit_marker.h"

using namespace mce;
using namespace mce::kit;

namespace {

constexpr f32 kDt = 1.0f / 60.0f;

LocalTransform at(f32 x, f32 y, f32 z) { return LocalTransform{{x, y, z}, math::quat::identity(), {1, 1, 1}}; }

struct Scene {
    World w;
    u64 t = 0;
    Entity shooter, victim, wall;
    Scene() {
        w.register_components<LocalTransform>();
        install_combat3d(w, nullptr);
        install_hit_marker(w);
        shooter = w.spawn_with(at(0, 1, 0), Health3D{}, HitMarker3D{});
        victim = w.spawn_with(at(5, 1, 0), Health3D{});
        wall = w.spawn_with(at(9, 1, 0));
        run(2);
    }
    void run(u32 n) {
        for (u32 i = 0; i < n; ++i) w.tick(kDt, t++);
    }
    void shot(Entity owner, Entity target) {
        WeaponHit3D h;
        h.owner = owner;
        h.target = target;
        EventAccess(w).push(h);
    }
    HitMarker3D& m() { return *w.try_get_mut<HitMarker3D>(shooter); }
};

}  // namespace

TEST_CASE("hit_marker: a shot on a living target lights the marker, which decays") {
    Scene s;
    s.shot(s.shooter, s.victim);
    s.run(2);
    CHECK(s.m().hits == 1);
    CHECK(s.m().timer > 0.0f);
    CHECK(s.m().kill_timer == doctest::Approx(0.0f));
    s.run(20);  // 0.33 s > duration
    CHECK(s.m().timer == doctest::Approx(0.0f));
    CHECK(s.m().hits == 1);
}

TEST_CASE("hit_marker: walls, self hits and other shooters do not count") {
    Scene s;
    s.shot(s.shooter, s.wall);      // no Health3D
    s.shot(s.shooter, s.shooter);   // itself
    s.shot(s.victim, s.shooter);    // the victim has no HitMarker3D; the shooter is not the owner
    s.run(2);
    CHECK(s.m().hits == 0);
    CHECK(s.m().timer == doctest::Approx(0.0f));
}

TEST_CASE("hit_marker: a kill lights the kill marker for its killer only") {
    Scene s;
    EventAccess(s.w).push(Died3D{s.victim, s.shooter});
    s.run(2);
    CHECK(s.m().kill_timer > 0.0f);
    s.run(40);
    CHECK(s.m().kill_timer == doctest::Approx(0.0f));
    EventAccess(s.w).push(Died3D{s.shooter, s.victim});
    s.run(2);
    CHECK(s.m().kill_timer == doctest::Approx(0.0f));
}
