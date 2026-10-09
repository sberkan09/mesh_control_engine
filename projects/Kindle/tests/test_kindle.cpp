// Kindle (UC-D, ROADMAP M9): the authored level, played through the game's real systems on top of
// the engine's physics, with scripted input. It loads the cooked pack the build makes from
// assets/ (kindle_content), so it tests the scene as the editor saved it, not a copy built in code.
//
//   - the level has the pieces the design needs
//   - the red wall blocks the way until a switch turns it into a ghost (the new mechanic)
//   - a block that turns solid around the player waits until they step out
//   - a scripted run toggles both switches and reaches the flag; two runs hash the same
#include <doctest/doctest.h>

#include <cmath>

#include "kindle.h"
#include "mce/asset/asset_system.h"
#include "mce/input/ecs.h"
#include "mce/kit/particles.h"
#include "mce/kit/sequence.h"
#include "mce/physics/module.h"
#include "mce/scene/scene.h"

using namespace mce;
using namespace kindle;
using physics::BodyState;
using physics::World2D;

namespace {

struct Game {
    asset::AssetSystem assets;
    World w;
    u64 tick = 0;

    Game() {
        const StringView roots[1] = {StringView(KINDLE_PACK)};
        REQUIRE(static_cast<bool>(assets.mount(Span<const StringView>(roots, 1))));
        // The player runtime's order (src/player/src/player.cpp): input, physics, kit, the game, the scene.
        input::install(w);
        physics::install_physics2d(w);
        kit::install_particles(w, &assets);
        kit::install_sequences(w, &assets);
        REQUIRE(static_cast<bool>(kindle::install(w)));
        asset::Guid scene;
        for (asset::AssetKey k = 0; k < assets.asset_count(); ++k)
            if (assets.info(k).type == asset::ChunkType::Scene) scene = assets.info(k).guid;
        REQUIRE(!scene.is_null());
        REQUIRE(static_cast<bool>(scene::load(w, assets, scene)));
        step();  // first tick: setup runs, physics creates the bodies
        step();  // switch blocks of the inactive colour turn into ghosts
    }
    void step(input::InputState in = {}) {
        in.tick = tick;
        input::publish(w, in);
        w.tick(1.0 / 60.0, tick++);
    }
    KindleState& ks() { return w.resource<KindleState>(); }
    World2D& pw() { return w.resource<World2D>(); }
    vec2 pos() {
        BodyState s;
        REQUIRE(pw().body_state(ks().player, s));
        return s.position;
    }
    PlayerController& player() { return *w.try_get_mut<PlayerController>(ks().player); }
    void teleport(vec2 p) {
        pw().teleport(ks().player, p, 0.0f);
        player().vel = {};
    }
    template <class C, class F>
    u32 count(F&& pred) {
        u32 n = 0;
        w.query<const C&>().each([&](const C& c) { n += pred(c) ? 1u : 0u; });
        return n;
    }
    // The switch block whose tile is at (x, y).
    Entity block_at(vec2 p) {
        Entity found;
        w.query<const SwitchBlock&, const LocalTransform&>().each([&](Entity e, const SwitchBlock&, const LocalTransform& t) {
            if (std::fabs(t.position.x - p.x) < 0.01f && std::fabs(t.position.y - p.y) < 0.01f) found = e;
        });
        return found;
    }
};

input::InputState move(f32 x, bool jump_pressed = false, bool jump_held = false) {
    input::InputState in;
    in.move.x = x;
    in.jump_pressed = jump_pressed;
    in.jump_held = jump_held || jump_pressed;
    return in;
}

// The scripted player: runs right and jumps in three places (the spike strip, onto the first red
// step, onto the second). A pure function of the sim state, like Ember Step's attract bot.
struct Bot {
    u32 hold = 0;  // ticks left to hold jump (full height)
    input::InputState next(Game& g) {
        const vec2 p = g.pos();
        const bool grounded = g.player().grounded != 0;
        const bool spikes = p.x > 7.6f && p.x < 8.6f;
        const bool step1 = p.x > 36.4f && p.x < 37.4f;
        const bool step2 = p.y > 3.9f && p.y < 4.6f && p.x > 39.2f && p.x < 41.3f;
        if (grounded && hold == 0 && (spikes || step1 || step2)) {
            hold = 20;
            return move(1.0f, true);
        }
        if (hold > 0) --hold;
        return move(1.0f, false, hold > 0);
    }
};

u64 run_level(Game& g, u32 max_ticks) {
    Bot bot;
    for (u32 i = 0; i < max_ticks && g.ks().won == 0; ++i) g.step(bot.next(g));
    for (u32 i = 0; i < 30; ++i) g.step();  // the flag celebration settles
    return g.w.state_hash();
}

}  // namespace

TEST_CASE("kindle: the authored level has a player, a flag, two switches and both block colours") {
    Game g;
    CHECK(!g.ks().player.is_null());
    CHECK(g.count<Goal>([](const Goal&) { return true; }) == 1);
    CHECK(g.count<ColorSwitch>([](const ColorSwitch&) { return true; }) == 2);
    CHECK(g.count<SwitchBlock>([](const SwitchBlock& b) { return b.color == BlockColor::Red; }) >= 5);
    CHECK(g.count<SwitchBlock>([](const SwitchBlock& b) { return b.color == BlockColor::Blue; }) >= 5);
    CHECK(g.count<Crumble>([](const Crumble&) { return true; }) == 3);
    CHECK(g.count<Mover>([](const Mover&) { return true; }) == 1);
    CHECK(g.ks().coins_total == 9);
    CHECK(g.ks().active == BlockColor::Red);
    // Red starts solid, blue starts as ghosts.
    CHECK(g.count<SwitchBlock>([](const SwitchBlock& b) { return (b.solid != 0) != (b.color == BlockColor::Red); }) == 0);
}

TEST_CASE("kindle: the red wall blocks the way until a switch turns it into a ghost") {
    Game g;
    // Past the first switch without touching it: running right stops at the wall.
    g.teleport({16.0f, 2.46f});
    for (int i = 0; i < 90; ++i) g.step(move(1.0f));
    CHECK(g.ks().active == BlockColor::Red);
    CHECK(g.pos().x < 17.7f);
    CHECK(g.pos().x > 17.5f);
    // Even jumping at it: the wall is five tiles tall.
    for (int i = 0; i < 60; ++i) g.step(move(1.0f, i == 0, i < 20));
    CHECK(g.pos().x < 17.7f);

    // Back through the switch: blue is solid now, the wall is a ghost, the bridge holds.
    for (int i = 0; i < 60 && g.ks().toggles == 0; ++i) g.step(move(-1.0f));
    REQUIRE(g.ks().toggles == 1);
    CHECK(g.ks().active == BlockColor::Blue);
    for (int i = 0; i < 150; ++i) g.step(move(1.0f));
    CHECK(g.pos().x > 26.0f);              // through the wall and onto the bridge
    CHECK(g.pos().y == doctest::Approx(2.45f).epsilon(0.02));  // standing on it, not falling into the pit
    CHECK(g.ks().deaths == 0);
}

TEST_CASE("kindle: a block that turns solid around the player waits until they step out") {
    Game g;
    g.ks().active = BlockColor::Blue;  // as if a switch had been hit
    g.step();
    const Entity wall = g.block_at({18.5f, 2.5f});
    REQUIRE(!wall.is_null());
    CHECK(g.w.try_get<SwitchBlock>(wall)->solid == 0);

    g.teleport({18.5f, 2.46f});  // stand inside the ghost wall
    g.step();
    g.ks().active = BlockColor::Red;
    for (int i = 0; i < 5; ++i) g.step();
    CHECK(g.w.try_get<SwitchBlock>(wall)->solid == 0);  // not solid around the player
    CHECK(g.w.try_get<SwitchBlock>(g.block_at({18.5f, 6.5f}))->solid == 1);  // the top of the wall is clear

    for (int i = 0; i < 40 && g.pos().x > 17.4f; ++i) g.step(move(-1.0f));  // walk out (not as far as the switch)
    for (int i = 0; i < 20; ++i) g.step();
    CHECK(g.pos().x < 17.6f);
    CHECK(g.ks().active == BlockColor::Red);
    CHECK(g.w.try_get<SwitchBlock>(wall)->solid == 1);
}

TEST_CASE("kindle: a scripted run toggles both switches and reaches the flag") {
    Game g;
    const u64 hash = run_level(g, 1800);
    const KindleState& ks = g.ks();
    MESSAGE("cleared in " << ks.won_at << " ticks; toggles " << ks.toggles << ", coins " << ks.coins << "/" << ks.coins_total
                          << ", deaths " << ks.deaths);
    CHECK(ks.won == 1);
    CHECK(ks.toggles == 2);
    CHECK(ks.active == BlockColor::Red);
    CHECK(ks.deaths == 0);
    CHECK(ks.coins >= 7);  // every coin on the path; the lift's two are a detour
    CHECK(ks.won_at < 1200);
    u32 reached = 0;
    g.w.query<const Goal&>().each([&](const Goal& goal) { reached += goal.reached; });
    CHECK(reached == 1);

    Game again;  // FR-RUN-040: same scene, same input, same hash
    CHECK(run_level(again, 1800) == hash);
}
