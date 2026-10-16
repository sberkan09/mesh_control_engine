// M-VIEWS 5: the co-op kit. Players join on their button (and the auto_join ones at the start), spawn at their
// PlayerSpawn, leave on their button and can rejoin; the player's Move and Jump drive a CharacterController3D;
// a PlayerCamera follows its player and is enabled while it is in; SharedCamera2D frames everyone with
// both fits set; the whole run reads only InputState, so two runs hash alike.

#include <doctest/doctest.h>

#include <type_traits>
#include <vector>

#include "mce/ecs/transform.h"
#include "mce/ecs/world.h"
#include "mce/input/actions.h"
#include "mce/input/ecs.h"
#include "mce/kit/bodies3d.h"
#include "mce/kit/components.h"
#include "mce/kit/coop.h"
#include "mce/kit/views.h"

using namespace mce;
using namespace mce::kit;

namespace {

constexpr f32 kDt = 1.0f / 60.0f;

// One action of one slot in a tick's snapshot.
struct Act {
    u32 slot;
    const char* name;
    bool pressed = false;
    bool held = false;
    bool released = false;
    f32 x = 0.0f, y = 0.0f;
};

struct CoopWorld {
    World world;
    u64 tick = 0;
    CoopWorld() {
        input::install(world);
        install_coop(world, nullptr);
    }
    void run(const std::vector<Act>& acts = {}) {
        input::InputState in;
        in.tick = tick;
        for (const Act& a : acts) {
            const u32 i = in.action_count++;
            in.action_names[i] = input::player_action_id(a.slot, StringId(a.name));
            in.actions[i].pressed = a.pressed;
            in.actions[i].held = a.held || a.pressed;
            in.actions[i].released = a.released;
            in.actions[i].x = a.x;
            in.actions[i].y = a.y;
        }
        input::publish(world, in);
        world.tick(kDt, tick++);
    }
    void many(u32 n) {
        for (u32 i = 0; i < n; ++i) run();
    }
    Entity game(u32 max_players = 4, u32 auto_join = 1) {
        CoopGame g;
        g.max_players = max_players;
        g.auto_join = auto_join;
        return world.spawn_with(g);
    }
    Entity spawn_point(u32 player, math::vec3 at) {
        LocalTransform t;
        t.position = at;
        return world.spawn_with(PlayerSpawn{player}, t);
    }
    math::vec3 pos(Entity e) { return world.try_get<LocalTransform>(e)->position; }
};

Act join(u32 slot) { return Act{slot, "Join", true}; }
Act leave(u32 slot) { return Act{slot, "Leave", true}; }

}  // namespace

TEST_CASE("coop: auto_join players join once at the start, others on their button, at their spawn points") {
    CoopWorld c;
    c.game(4, 1);
    c.spawn_point(1, {-4, 0, 0});
    c.spawn_point(2, {4, 0, 0});
    c.spawn_point(0, {0, 0, 7});
    CHECK(coop_join_slots(c.world) == 4);
    CHECK(coop_joined_mask(c.world) == 0);

    c.run();
    CHECK(coop_joined_mask(c.world) == 0b0001);  // slot 0 joined without a button, at its spawn
    CHECK(c.pos(coop_player(c.world, 0)).x == -4);

    c.run({join(2)});
    CHECK(coop_joined_mask(c.world) == 0b0101);
    CHECK(c.pos(coop_player(c.world, 2)).z == 7);  // no spawn for player 3: the "any" point
    CHECK(c.world.try_get<LocalPlayer>(coop_player(c.world, 2))->player == 3);

    c.run({join(2)});  // already in: nothing more
    CHECK(coop_joined_mask(c.world) == 0b0101);
    u32 count = 0;
    c.world.query<const LocalPlayer&>().each([&](Entity, const LocalPlayer&) { ++count; });
    CHECK(count == 2);

    c.run({join(1)});
    c.run({join(3)});
    CHECK(coop_joined_mask(c.world) == 0b1111);
    c.run({join(0)});  // four is the most
    count = 0;
    c.world.query<const LocalPlayer&>().each([&](Entity, const LocalPlayer&) { ++count; });
    CHECK(count == 4);
}

TEST_CASE("coop: max_players limits the slots, Leave frees a slot (it does not auto-join again), and it can rejoin") {
    CoopWorld c;
    c.game(2, 1);
    c.run();
    c.run({join(3)});  // slot 4 does not exist in a two-player game
    CHECK(coop_joined_mask(c.world) == 0b0001);
    CHECK(coop_join_slots(c.world) == 2);

    c.run({leave(0)});
    c.many(2);
    CHECK(coop_joined_mask(c.world) == 0);  // the auto_join player is not re-added
    c.run({join(0)});
    CHECK(coop_joined_mask(c.world) == 0b0001);
    c.run({join(1), leave(0)});
    c.many(1);
    CHECK(coop_joined_mask(c.world) == 0b0010);
}

TEST_CASE("coop: without a CoopGame nothing joins and there are no join slots") {
    CoopWorld c;
    c.run({join(1)});
    CHECK(coop_joined_mask(c.world) == 0);
    CHECK(coop_join_slots(c.world) == 0);
}

TEST_CASE("coop: a slot's Move and Jump drive its CharacterController3D, other slots do not") {
    CoopWorld c;
    c.game(4, 2);
    c.run();
    for (u32 slot = 0; slot < 2; ++slot) {
        CharacterController3D cc;
        cc.speed = 4.0f;
        c.world.add(coop_player(c.world, slot), cc);
    }
    c.run({Act{1, "Move", false, true, false, 1.0f, 0.5f}, Act{1, "Jump", true}});
    const CharacterController3D& p2 = *c.world.try_get<CharacterController3D>(coop_player(c.world, 1));
    CHECK(p2.move.x == doctest::Approx(4.0f));
    CHECK(p2.move.z == doctest::Approx(-2.0f));  // forward is -z
    CHECK(p2.jump);
    const CharacterController3D& p1 = *c.world.try_get<CharacterController3D>(coop_player(c.world, 0));
    CHECK(p1.move.x == 0.0f);
    CHECK(!p1.jump);

    c.run({Act{0, "Move", false, true, false, -1.0f, 0.0f}});
    CHECK(p1.move.x == doctest::Approx(-4.0f));
    CHECK(p2.move.x == 0.0f);  // let go of the stick: stops
}

TEST_CASE("coop: a PlayerCamera follows its player and is enabled only while the player is in; SharedCamera2D frames everyone") {
    CoopWorld c;
    c.game(4, 1);
    c.spawn_point(1, {0, 0, 0});
    c.spawn_point(2, {10, 6, 0});
    PlayerCamera pc;
    pc.player = 2;
    pc.offset = {0, 5, 3};
    LocalTransform cam_t;
    CameraView view;
    view.split = 2;
    view.enabled = 1;
    const Entity cam = c.world.spawn_with(pc, cam_t, view);
    SharedCamera2D sc;
    sc.margin = 1;
    sc.min_width = 4;
    sc.min_height = 2;
    const Entity shared = c.world.spawn_with(sc, LocalTransform{}, Camera2DComponent{});

    c.run();
    CHECK(c.world.try_get<CameraView>(cam)->enabled == 0);  // player 2 is not in
    const Camera2DComponent* fit = c.world.try_get<Camera2DComponent>(shared);
    CHECK(fit->fit_width == doctest::Approx(4.0f));  // one player: the minimum
    CHECK(fit->fit_height == doctest::Approx(2.0f));  // the minimum again: one player is a zero-height box

    c.run({join(1)});
    c.run();
    CHECK(c.world.try_get<CameraView>(cam)->enabled == 1);
    CHECK(c.pos(cam).x == doctest::Approx(10.0f));
    CHECK(c.pos(cam).y == doctest::Approx(11.0f));
    CHECK(c.pos(cam).z == doctest::Approx(3.0f));
    CHECK(c.pos(shared).x == doctest::Approx(5.0f));
    CHECK(c.pos(shared).y == doctest::Approx(3.0f));
    CHECK(fit->fit_width == doctest::Approx(12.0f));  // 10 wide + 2 * margin
    CHECK(fit->fit_height == doctest::Approx(8.0f));

    c.run({leave(1)});
    c.many(2);
    CHECK(c.world.try_get<CameraView>(cam)->enabled == 0);
    CHECK(fit->fit_width == doctest::Approx(4.0f));
}

TEST_CASE("coop: two runs of the same inputs hash alike") {
    const auto play = [](std::vector<u64>& trace) {
        CoopWorld c;
        c.game(4, 1);
        c.spawn_point(0, {0, 0, 0});
        const auto step = [&](const std::vector<Act>& a) {
            c.run(a);
            trace.push_back(c.world.state_hash());
        };
        step({});
        step({join(1)});
        step({join(3)});
        for (u32 i = 0; i < 20; ++i) step({Act{1, "Move", false, true, false, 1.0f, 0.0f}});
        step({leave(3)});
        for (u32 i = 0; i < 5; ++i) step({});
    };
    std::vector<u64> a, b;
    play(a);
    play(b);
    REQUIRE(a.size() == b.size());
    for (usize i = 0; i < a.size(); ++i) CHECK_MESSAGE(a[i] == b[i], "tick " << i);
}

