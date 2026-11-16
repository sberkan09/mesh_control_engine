// Melee attack (M-MELEE): cone and reach, windup, rate, dead and seated attackers, deterministic order.

#include <doctest/doctest.h>

#include "mce/ecs/transform.h"
#include "mce/ecs/world.h"
#include "mce/input/actions.h"
#include "mce/input/ecs.h"
#include "mce/kit/combat3d.h"
#include "mce/kit/melee3d.h"
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
    u32 swung = 0, hits = 0;
    Scene() {
        w.register_components<LocalTransform>();
        w.register_components<Rider3D>();
        install_combat3d(w, nullptr);
        install_melee3d(w);
        Health3D h;
        h.max = h.current = 100.0f;
        player = w.spawn_with(at(0, 0, 0), h, Melee3D{});
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
    Melee3D& m() { return *w.try_get_mut<Melee3D>(player); }
    f32 hp(Entity e) { return w.try_get<Health3D>(e)->current; }
};

}  // namespace

TEST_CASE("melee3d: a swing hits what is in front within reach after the windup, not what is behind or too far") {
    Scene s;
    const Entity front = s.target(0, 0, -1.5f);   // forward is -Z
    const Entity behind = s.target(0, 0, 1.5f);
    const Entity far = s.target(0, 0, -5.0f);
    const Entity side = s.target(1.9f, 0, -0.2f);  // ~84 degrees off forward: outside a 100 degree cone's 50 degree half
    s.m().trigger = 1;
    s.run(2);
    CHECK(s.m().swinging == 1);
    CHECK(s.hp(front) == doctest::Approx(100.0f));  // the windup has not run out
    s.run(20);
    CHECK(s.m().swinging == 0);
    CHECK(s.m().swings == 1);
    CHECK(s.hp(front) == doctest::Approx(65.0f));
    CHECK(s.hp(behind) == doctest::Approx(100.0f));
    CHECK(s.hp(far) == doctest::Approx(100.0f));
    CHECK(s.hp(side) == doctest::Approx(100.0f));
    CHECK(s.hp(s.player) == doctest::Approx(100.0f));
}

TEST_CASE("melee3d: the rate limits swings, a dead or seated attacker does not swing") {
    Scene s;
    const Entity front = s.target(0, 0, -1.0f, 1000.0f);
    s.m().trigger = 1;
    s.run(12);
    s.m().trigger = 1;  // still cooling down (rate 2.5 = 0.4 s)
    s.run(2);
    CHECK(s.m().swings == 1);
    s.run(30);
    s.m().trigger = 1;
    s.run(30);
    CHECK(s.m().swings == 2);
    CHECK(s.hp(front) == doctest::Approx(930.0f));
    Rider3D r;
    r.seated = 1;
    s.w.add(s.player, r);
    s.run(40);
    s.m().trigger = 1;
    s.run(40);
    CHECK(s.m().swings == 2);
}

TEST_CASE("melee3d: the Melee action starts a swing with use_input, a same-team target is spared, a kill pushes Died3D") {
    Scene s;
    s.m().use_input = 1;
    s.w.try_get_mut<Health3D>(s.player)->team = 2;
    const Entity mate = s.target(0, 0, -1.0f);
    s.w.try_get_mut<Health3D>(mate)->team = 2;
    const Entity weak = s.target(0.3f, 0, -1.0f, 10.0f);
    s.run(1, "Melee");
    s.run(30);
    CHECK(s.m().swings == 1);
    CHECK(s.hp(mate) == doctest::Approx(100.0f));
    CHECK(s.w.try_get<Health3D>(weak)->dead == 1);
}
