// Ember Step gameplay (M6): the v0.5 mechanics and flow, driven through the real systems with
// scripted input — level loading, dash, wall slide / wall jump (and that the authored shafts are
// climbable), crumbling blocks, the menu -> play -> pause -> results flow and the best-times save.
#include <doctest/doctest.h>

#include "game/level.h"
#include "game/platformer.h"
#include "mce/ecs/save.h"
#include "mce/ecs/transform.h"
#include "mce/input/ecs.h"
#include "mce/physics/module.h"

using namespace mce;
using namespace platformer;
using physics::BodyState;
using physics::World2D;

namespace {

struct Game {
    World w;
    u64 tick = 0;

    Game() {
        input::install(w);
        physics::install_physics2d(w);
        REQUIRE(platformer::install(w));
    }
    void step(input::InputState in = {}) {
        in.tick = tick;
        input::publish(w, in);
        w.tick(1.0 / 60.0, tick++);
    }
    void idle(u32 n) {
        for (u32 i = 0; i < n; ++i) step();
    }
    GameState& gs() { return w.resource<GameState>(); }
    Player& player() { return *w.try_get_mut<Player>(gs().player); }
    vec2 pos() {
        BodyState s;
        REQUIRE(w.resource<World2D>().body_state(gs().player, s));
        return s.position;
    }
    void teleport(vec2 p) { w.resource<World2D>().teleport(gs().player, p, 0.0f); }
    void load(u32 level) {
        gs().load_request = static_cast<i32>(level);
        step();  // swaps the level (spawned at the sync point; bodies exist after this tick's physics)
        step();
    }
};

input::InputState move(f32 x) {
    input::InputState in;
    in.move.x = x;
    return in;
}

}  // namespace

TEST_CASE("ember step: every level loads with one player, one flag and coins") {
    Game g;
    for (u32 level = 0; level < kLevelCount; ++level) {
        const u32 serial = g.gs().level_serial;
        g.load(level);
        CHECK(g.gs().level == level);
        CHECK(g.gs().level_serial == serial + 1);
        CHECK(g.gs().mode == Mode::Playing);
        u32 players = 0, goals = 0, foreign = 0;
        g.w.query<const Player&>().each([&](const Player&) { ++players; });
        g.w.query<const Goal&>().each([&](const Goal&) { ++goals; });
        g.w.query<const LevelTag&>().each([&](const LevelTag& t) { foreign += t.level != level ? 1u : 0u; });
        CHECK(players == 1);
        CHECK(goals == 1);
        CHECK(foreign == 0);  // the previous level is gone
        CHECK(g.gs().coins_total > 0);
        CHECK(g.gs().level_ticks <= 1);
    }
}

TEST_CASE("ember step: a dash is a flat 3 m burst, once per air time") {
    Game g;
    g.load(3);  // Ember Dash: the start pillar is 6 tiles wide
    g.idle(10);
    REQUIRE(g.player().grounded == 1);
    const vec2 p0 = g.pos();
    input::InputState dash;
    dash.fire_pressed = true;  // no "Dash" action in this map: Fire stands in
    g.step(dash);
    REQUIRE(g.player().dash_ticks > 0);
    g.idle(9);
    const vec2 p1 = g.pos();
    CHECK(p1.x - p0.x == doctest::Approx(3.0f).epsilon(0.05));
    CHECK(p1.y == doctest::Approx(p0.y).epsilon(0.001));

    // Off the pillar: airborne, the spent dash does not fire again until landing.
    g.teleport({10.0f, 7.0f});
    g.player().vel = {};
    g.step();
    CHECK(g.player().grounded == 0);
    g.step(dash);
    CHECK(g.player().dash_ticks > 0);
    g.idle(12);
    g.step(dash);
    CHECK(g.player().dash_ticks == 0);
    CHECK(g.player().dashes == 2);
}

TEST_CASE("ember step: jump + dash clears a 6-tile spike gap") {
    Game g;
    g.load(3);
    g.idle(5);
    // Run right; jump at the pillar's edge (x = 7), dash at the apex, land on the next pillar.
    bool jumped = false, dashed = false;
    for (u32 i = 0; i < 120; ++i) {
        input::InputState in = move(1.0f);
        const vec2 p = g.pos();
        if (!jumped && p.x > 6.4f) {
            in.jump_pressed = in.jump_held = jumped = true;
        } else if (jumped) {
            in.jump_held = true;
            if (!dashed && g.player().vel.y < 1.0f) in.fire_pressed = dashed = true;
        }
        g.step(in);
        if (dashed && g.player().grounded) break;
    }
    CHECK(g.gs().deaths == 0);
    CHECK(g.player().grounded == 1);
    CHECK(g.pos().x > 13.0f);
    CHECK(g.pos().x < 18.0f);
}

namespace {
// Wall-jump climber: hold into the wall, jump whenever touching one, then hold toward the other.
f32 climb(Game& g, vec2 start, u32 ticks) {
    g.teleport(start);
    g.player().vel = {};
    f32 best = start.y;
    i32 hold = -1;
    for (u32 i = 0; i < ticks; ++i) {
        input::InputState in = move(static_cast<f32>(hold));
        const Player& p = g.player();
        if (p.wall != 0 && p.jump_buffer == 0) {
            in.jump_pressed = true;
            hold = -p.wall;
        } else if (p.grounded) {
            in.jump_pressed = true;  // back on the floor: hop into the wall again
        }
        in.jump_held = true;
        g.step(in);
        best = math::max(best, g.pos().y);
    }
    return best;
}
}  // namespace

TEST_CASE("ember step: wall slide caps the fall; a wall jump kicks away and up") {
    Game g;
    g.load(2);  // The Chimney: the first shaft is x 8..10 between walls at x 7 and 11
    g.teleport({8.4f, 12.0f});
    g.player().vel = {};
    for (u32 i = 0; i < 20; ++i) g.step(move(-1.0f));
    CHECK(g.player().wall == -1);
    CHECK(g.player().sliding == 1);
    CHECK(g.player().vel.y == doctest::Approx(-3.0f));
    input::InputState jump = move(-1.0f);
    jump.jump_pressed = jump.jump_held = true;
    g.step(jump);
    CHECK(g.player().vel.x > 7.0f);
    CHECK(g.player().vel.y > 11.0f);
    CHECK(g.player().wall_lock > 0);
}

TEST_CASE("ember step: the authored shafts are climbable by wall jumps") {
    SUBCASE("The Chimney, first shaft") {
        Game g;
        g.load(2);
        CHECK(climb(g, {8.4f, 3.0f}, 400) > 15.0f);  // above the walls' tops
    }
    SUBCASE("The Summit") {
        Game g;
        g.load(4);
        CHECK(climb(g, {38.4f, 7.0f}, 400) > 16.5f);  // up to the ledge at y 16
    }
}

TEST_CASE("ember step: crumbling blocks give way under the player and grow back") {
    Game g;
    g.load(1);  // Crumble Bridge
    Entity block;
    vec2 at;
    g.w.query<const Crumble&, const LocalTransform&>().each([&](Entity e, const Crumble&, const LocalTransform& t) {
        if (block.is_null()) {
            block = e;
            at = {t.position.x, t.position.y};
        }
    });
    REQUIRE(!block.is_null());
    g.teleport({at.x, at.y + 1.0f});
    g.player().vel = {};
    u32 t = 0;
    while (g.w.try_get<Crumble>(block)->state == CrumbleState::Solid && t++ < 30) g.step();
    REQUIRE(g.w.try_get<Crumble>(block)->state == CrumbleState::Shaking);
    g.idle(31);
    CHECK(g.w.try_get<Crumble>(block)->state == CrumbleState::Gone);
    BodyState bs;
    REQUIRE(g.w.resource<World2D>().body_state(block, bs));
    CHECK(!bs.enabled);
    g.idle(25);
    CHECK((g.pos().y < at.y || g.gs().deaths > 0));  // fell through (onto the spikes below)
    g.teleport({2.5f, 3.0f});  // out of the way
    g.idle(160);
    CHECK(g.w.try_get<Crumble>(block)->state == CrumbleState::Solid);
    REQUIRE(g.w.resource<World2D>().body_state(block, bs));
    CHECK(bs.enabled);
}

TEST_CASE("ember step: menu -> play -> pause -> restart -> win -> results -> next level") {
    Game g;
    g.gs().mode = Mode::Menu;
    g.step();
    CHECK(g.gs().paused);
    input::InputState down;
    down.move.y = -1.0f;
    g.step(down);
    CHECK(g.gs().selection == 0);  // only level 1 is open
    input::InputState confirm;
    confirm.confirm_pressed = true;
    const u32 serial = g.gs().level_serial;
    g.step(confirm);
    CHECK(g.gs().mode == Mode::Playing);
    CHECK(g.gs().level_serial == serial + 1);
    g.idle(30);
    CHECK(g.gs().level_ticks > 20);

    input::InputState pause;
    pause.pause_pressed = true;
    g.step(pause);
    CHECK(g.gs().mode == Mode::Paused);
    const u64 frozen = g.gs().level_ticks;
    g.idle(10);
    CHECK(g.gs().level_ticks == frozen);
    g.step(down);  // -> Restart
    g.step();
    CHECK(g.gs().selection == 1);
    g.step(confirm);
    CHECK(g.gs().mode == Mode::Playing);
    CHECK(g.gs().level_serial == serial + 2);
    g.idle(2);
    CHECK(g.gs().level_ticks <= 2);

    // Reach the flag.
    vec2 flag;
    g.w.query<const Goal&, const LocalTransform&>().each(
        [&](const Goal&, const LocalTransform& t) { flag = {t.position.x, t.position.y}; });
    g.idle(60);
    g.teleport({flag.x, flag.y - 0.5f});
    g.idle(2);
    REQUIRE(g.gs().won);
    const Records& rec = *g.w.try_get<Records>(g.gs().records);
    CHECK(rec.best(0) == static_cast<u32>(g.gs().won_at));
    CHECK(rec.unlocked == 2);
    CHECK(g.gs().new_best);
    g.idle(kResultsDelayTicks);
    CHECK(g.gs().mode == Mode::Results);
    g.step(confirm);  // Next level
    CHECK(g.gs().mode == Mode::Playing);
    CHECK(g.gs().level == 1);

    // A slower finish keeps the old best.
    g.load(0);
    g.idle(200);
    g.teleport({flag.x, flag.y - 0.5f});
    g.idle(2);
    REQUIRE(g.gs().won);
    CHECK(!g.gs().new_best);
    CHECK(g.w.try_get<Records>(g.gs().records)->best(0) < g.gs().won_at);
}

TEST_CASE("ember step: best times round-trip through a save payload") {
    Array<byte> payload;
    {
        Game g;
        Records& r = *g.w.try_get_mut<Records>(g.gs().records);
        r.set_best(0, 1234);
        r.set_best(2, 4321);
        r.unlocked = 4;
        save::write_save(g.w, {}, payload);
    }
    Game g;
    auto st = save::read_save(g.w, payload.span());
    REQUIRE(st);
    CHECK(st.value().spawned == 0);  // matched the fresh game's records entity by SaveKey
    const Records& r = *g.w.try_get<Records>(g.gs().records);
    CHECK(r.best(0) == 1234);
    CHECK(r.best(1) == 0);
    CHECK(r.best(2) == 4321);
    CHECK(r.unlocked == 4);
    GameState menu;
    menu.mode = Mode::Menu;
    CHECK(menu_options(menu, &r) == 4);
}
