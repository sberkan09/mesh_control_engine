// Thrown grenades (M-GRENADE): throw rules, fuse, blast falloff, credit, deterministic order.

#include <doctest/doctest.h>

#include "mce/ecs/transform.h"
#include "mce/ecs/world.h"
#include "mce/input/actions.h"
#include "mce/input/ecs.h"
#include "mce/kit/combat3d.h"
#include "mce/kit/grenade3d.h"
#include "mce/kit/seats3d.h"
#include "mce/string_id.h"

using namespace mce;
using namespace mce::kit;

namespace {

constexpr f32 kDt = 1.0f / 60.0f;

LocalTransform at(f32 x, f32 y, f32 z) { return LocalTransform{{x, y, z}, math::quat::identity(), {1, 1, 1}}; }

struct Scene {
    World w;
    u64 t = 0;
    Entity player;
    Scene() {
        w.register_components<LocalTransform>();
        w.register_components<Rider3D, VehicleSeat3D>();
        install_combat3d(w, nullptr);
        install_grenade3d(w);
        Health3D h;
        h.max = h.current = 1000.0f;
        Grenade3D g;
        g.fuse = 1.0f;
        g.throw_speed = 0.0f;  // dropped at the thrower's feet unless a test sets a speed
        g.lift = 0.0f;
        player = w.spawn_with(at(0, 0, 0), h, g);
    }
    Entity target(f32 x, f32 y, f32 z, f32 hp = 100.0f) {
        Health3D h;
        h.max = h.current = hp;
        return w.spawn_with(at(x, y, z), h);
    }
    void run(u32 n, const char* action = nullptr) {
        for (u32 i = 0; i < n; ++i) {
            input::InputState in;
            in.tick = t;
            if (action && i == 0) {
                in.action_count = 1;
                in.action_names[0] = StringId(action);
                in.actions[0].held = in.actions[0].pressed = true;
            }
            input::publish(w, in);
            w.tick(kDt, t++);
        }
    }
    Grenade3D& g() { return *w.try_get_mut<Grenade3D>(player); }
    f32 hp(Entity e) { return w.try_get<Health3D>(e)->current; }
    u32 live() {
        u32 n = 0;
        w.query<GrenadeLive3D>().each([&](Entity, GrenadeLive3D&) { ++n; });
        return n;
    }
};

}  // namespace

TEST_CASE("grenade3d: a throw takes one from the stack and the grenade waits out its fuse") {
    Scene s;
    s.g().trigger = 1;
    s.run(2);
    CHECK(s.g().count == 2);
    CHECK(s.g().thrown == 1);
    CHECK(s.live() == 1);
    s.run(40);  // 0.67 s of a 1 s fuse
    CHECK(s.live() == 1);
    s.run(30);
    CHECK(s.live() == 0);
}

TEST_CASE("grenade3d: the blast falls off linearly and misses what is outside the radius") {
    Scene s;
    s.g().radius = 10.0f;
    s.g().damage = 100.0f;
    // The grenade drops about 1.2 m up at the thrower's x/z; targets are measured from where it ends up.
    const Entity near = s.target(0, 1.2f, -0.5f);   // ~0.5 m from the centre
    const Entity mid = s.target(5, 1.2f, 0);        // ~5 m
    const Entity far = s.target(20, 1.2f, 0);       // outside
    s.g().trigger = 1;
    s.run(90);
    CHECK(s.hp(far) == doctest::Approx(100.0f));
    CHECK(s.hp(near) < s.hp(mid));
    CHECK(s.hp(mid) < 100.0f);
}

TEST_CASE("grenade3d: the thrower is credited and also hurt; GrenadeBlast3D counts the hits") {
    Scene s;
    s.g().radius = 10.0f;
    const Entity victim = s.target(2, 1.2f, 0, 20.0f);
    u32 blasts = 0, hits = 0;
    Entity owner;
    s.w.query<Health3D>();  // keep the query path warm
    s.g().trigger = 1;
    for (u32 i = 0; i < 90; ++i) {
        s.run(1);
        EventAccess ev(s.w);
        for (const GrenadeBlast3D& b : ev.read<GrenadeBlast3D>()) {
            ++blasts;
            hits = b.hits;
            owner = b.owner;
        }
    }
    CHECK(blasts == 1);
    CHECK(hits == 2);  // the victim and the thrower
    CHECK(owner == s.player);
    CHECK(s.w.try_get<Health3D>(victim)->dead == 1);
    CHECK(s.w.try_get<Health3D>(victim)->last_attacker == s.player);
    CHECK(s.hp(s.player) < 1000.0f);
}

TEST_CASE("grenade3d: the cooldown, an empty stack, a dead or seated thrower stop a throw") {
    Scene s;
    s.g().rate = 1.0f;
    s.g().trigger = 1;
    s.run(1);
    s.g().trigger = 1;
    s.run(1);  // inside the 1 s cooldown
    CHECK(s.g().thrown == 1);
    s.run(70);
    s.g().trigger = 1;
    s.run(1);
    CHECK(s.g().thrown == 2);
    s.run(70);
    s.g().count = 0;
    s.g().trigger = 1;
    s.run(1);
    CHECK(s.g().thrown == 2);
    s.g().count = 3;
    VehicleSeat3D seat;
    seat.driver = s.player;
    const Entity car = s.w.spawn_with(at(0, 0, 0), seat);
    Rider3D r;
    r.seated = 1;
    r.vehicle = car;
    s.w.add(s.player, r);
    s.g().trigger = 1;
    s.run(1);
    CHECK(s.g().thrown == 2);
    s.w.remove<Rider3D>(s.player);
    s.w.try_get_mut<Health3D>(s.player)->dead = 1;
    s.g().trigger = 1;
    s.run(1);
    CHECK(s.g().thrown == 2);
}

TEST_CASE("grenade3d: the action Throw throws one when use_input is on") {
    Scene s;
    s.g().use_input = 1;
    s.run(1);
    CHECK(s.g().thrown == 0);
    s.run(1, "Throw");
    CHECK(s.g().thrown == 1);
}

TEST_CASE("grenade3d: a grenade keeps the numbers it was thrown with") {
    Scene s;
    s.g().radius = 10.0f;
    s.g().damage = 100.0f;
    const Entity t = s.target(1, 1.2f, 0);
    s.g().trigger = 1;
    s.run(2);
    s.g().damage = 0.0f;  // a later change must not weaken the grenade in flight
    s.run(80);
    CHECK(s.hp(t) < 100.0f);
}
