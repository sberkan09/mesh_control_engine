// Bullet holes (M-SHOTMARKS): a hit on a wall takes the next ring slot, faces the normal, wraps onto the oldest, fades
// and hides; a hit on a character leaves nothing.

#include <doctest/doctest.h>

#include <cmath>

#include "mce/ecs/transform.h"
#include "mce/ecs/world.h"
#include "mce/kit/bullet_hole3d.h"
#include "mce/kit/combat3d.h"
#include "mce/kit/kit3d.h"

using namespace mce;
using namespace mce::kit;

namespace {

constexpr f32 kDt = 1.0f / 60.0f;

struct Scene {
    World w;
    u64 t = 0;
    Entity hole[3];
    Scene(f32 life = 20.0f, f32 fade = 4.0f) {
        w.register_components<LocalTransform, Decal3D>();
        install_combat3d(w, nullptr);
        install_bullet_holes(w);
        for (Entity& e : hole) {
            BulletHole3D h;
            h.life = life;
            h.fade = fade;
            Decal3D d;
            d.enabled = 0;
            e = w.spawn_with(LocalTransform{}, d, h);
        }
        run(1);
    }
    void run(u32 n) {
        for (u32 i = 0; i < n; ++i) w.tick(kDt, t++);
    }
    void hit(math::vec3 p, math::vec3 n, Entity target = Entity{}) {
        WeaponHit3D e;
        e.point = p;
        e.normal = n;
        e.target = target;
        EventAccess(w).push(e);
    }
    const BulletHole3D& h(int i) { return *w.try_get<BulletHole3D>(hole[i]); }
    const Decal3D& d(int i) { return *w.try_get<Decal3D>(hole[i]); }
    const LocalTransform& tr(int i) { return *w.try_get<LocalTransform>(hole[i]); }
};

}  // namespace

TEST_CASE("bullet holes: a hit shows the next slot at the point, +Y along the normal") {
    Scene s;
    CHECK(s.d(0).enabled == 0);
    s.hit({1, 2, 3}, {0, 0, 1});
    s.run(1);
    CHECK(s.d(0).enabled == 1);
    CHECK(s.h(0).serial == 1);
    CHECK(s.tr(0).position.x == doctest::Approx(1.0f));
    CHECK(s.tr(0).position.z == doctest::Approx(3.0f));
    const math::vec3 up = math::rotate(s.tr(0).rotation, {0, 1, 0});
    CHECK(up.z == doctest::Approx(1.0f).epsilon(0.001));
    CHECK(up.y == doctest::Approx(0.0f).epsilon(0.001));
    CHECK(s.d(0).size.x == doctest::Approx(0.25f));
    CHECK(s.d(1).enabled == 0);

    s.hit({0, 0, 0}, {0, 1, 0});  // a floor: no turn needed
    s.run(1);
    CHECK(s.h(1).serial == 2);
    CHECK(math::rotate(s.tr(1).rotation, {0, 1, 0}).y == doctest::Approx(1.0f));

    s.hit({0, 0, 0}, {0, -1, 0});  // a ceiling: turned all the way round
    s.run(1);
    CHECK(math::rotate(s.tr(2).rotation, {0, 1, 0}).y == doctest::Approx(-1.0f));
}

TEST_CASE("bullet holes: the ring reuses the oldest slot, two hits in one tick take two slots") {
    Scene s;
    s.hit({1, 0, 0}, {0, 1, 0});
    s.hit({2, 0, 0}, {0, 1, 0});
    s.run(1);
    CHECK(s.h(0).serial == 1);
    CHECK(s.h(1).serial == 2);
    s.hit({3, 0, 0}, {0, 1, 0});
    s.run(1);
    CHECK(s.h(2).serial == 3);
    s.hit({4, 0, 0}, {0, 1, 0});  // full: the oldest (slot 0) goes
    s.run(1);
    CHECK(s.h(0).serial == 4);
    CHECK(s.tr(0).position.x == doctest::Approx(4.0f));
    CHECK(s.h(1).serial == 2);
}

TEST_CASE("bullet holes: a hit on a character leaves nothing") {
    Scene s;
    Health3D hp;
    hp.max = hp.current = 10.0f;
    const Entity npc = s.w.spawn_with(LocalTransform{}, hp);
    s.hit({0, 1, 0}, {0, 0, 1}, npc);
    s.run(1);
    CHECK(s.d(0).enabled == 0);
    CHECK(s.h(0).serial == 0);
    s.hit({0, 1, 0}, {0, 0, 1});  // no target: a wall
    s.run(1);
    CHECK(s.d(0).enabled == 1);
}

TEST_CASE("bullet holes: a hole stays for its life, fades and hides") {
    Scene s(0.5f, 0.5f);
    s.hit({0, 0, 0}, {0, 1, 0});
    s.run(1);
    s.run(20);  // 0.33 s
    CHECK(s.d(0).color.w == doctest::Approx(1.0f));
    s.run(20);  // 0.67 s: 0.17 s into the 0.5 s fade
    CHECK(s.d(0).enabled == 1);
    CHECK(s.d(0).color.w < 0.9f);
    CHECK(s.d(0).color.w > 0.4f);
    s.run(30);  // past 1 s
    CHECK(s.d(0).enabled == 0);
    CHECK(s.h(0).age < 0.0f);
}

TEST_CASE("bullet holes: no hole entities is harmless") {
    World w;
    w.register_components<LocalTransform, Decal3D>();
    install_combat3d(w, nullptr);
    install_bullet_holes(w);
    WeaponHit3D e;
    e.normal = {0, 1, 0};
    EventAccess(w).push(e);
    w.tick(kDt, 0);
    CHECK(true);
}
