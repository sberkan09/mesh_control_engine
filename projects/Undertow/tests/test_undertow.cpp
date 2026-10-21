// Undertow: the authored scenes, played through the game's real systems on top of the engine's
// physics, particles and sequences, with scripted input. It loads the cooked pack the build makes
// from assets/ (undertow_content), so it tests the scenes as the editor's MCP server saved them.
//
//   - the level has the pieces the design needs (drone, dock, relay, 5 cores, gates and switches)
//   - the drone thrusts, caps its speed, boosts on energy and regains it
//   - a switch opens its gate for a while; a closed gate hurts; a mine hurts and is gone
//   - a pod on the rope is hauled; entering the dock delivers it and lights a beacon
//   - delivering every core wins, and the win cue runs through the sequence runner
//   - the attract scene's autopilot docks core 1; two runs hash the same (FR-RUN-040)
//   - the screens (tools/author_ui_mcp.py): the title holds the run until Play, Pause holds it
//     until Resume, Quit to title reloads the scene; two scripted runs hash the same
#include <doctest/doctest.h>

#include <cmath>

#include "mce/asset/asset_system.h"
#include "mce/input/ecs.h"
#include "mce/kit/particles.h"
#include "mce/kit/sequence.h"
#include "mce/kit/stock.h"
#include "mce/physics/module.h"
#include "mce/scene/scene.h"
#include "undertow.h"

using namespace mce;
using namespace undertow;
using physics::BodyState;
using physics::World2D;

namespace {

struct Game {
    asset::AssetSystem assets;
    World w;
    u64 tick = 0;

    explicit Game(StringView scene_path = "scenes/main.mcscene", bool ui_flow = false) {
        const StringView roots[1] = {StringView(UNDERTOW_PACK)};
        REQUIRE(static_cast<bool>(assets.mount(Span<const StringView>(roots, 1))));
        // The player runtime's order (src/player/src/player.cpp): input, physics, kit, the game, the scene.
        input::install(w);
        physics::install_physics2d(w);
        kit::install_particles(w, &assets);
        kit::install_sequences(w, &assets);
        if (ui_flow) kit::install_ui_flow(w);  // the player installs it with the stock kit
        REQUIRE(static_cast<bool>(undertow::install(w)));
        asset::Guid scene;
        for (asset::AssetKey k = 0; k < assets.asset_count(); ++k)
            if (assets.info(k).type == asset::ChunkType::Scene && assets.info(k).path == scene_path) scene = assets.info(k).guid;
        REQUIRE(!scene.is_null());
        REQUIRE(static_cast<bool>(scene::load(w, assets, scene)));
        step();  // first tick: setup runs, physics creates the bodies
        step();
    }
    void step(input::InputState in = {}) {
        in.tick = tick;
        input::publish(w, in);
        w.tick(1.0 / 60.0, tick++);
    }
    void steps(u32 n, input::InputState in = {}) {
        for (u32 i = 0; i < n; ++i) step(in);
    }
    UndertowState& gs() { return w.resource<UndertowState>(); }
    World2D& pw() { return w.resource<World2D>(); }
    Drone& drone() { return *w.try_get_mut<Drone>(gs().drone); }
    vec2 pos() {
        BodyState s;
        REQUIRE(pw().body_state(gs().drone, s));
        return s.position;
    }
    vec2 vel() { return pw().linear_velocity(gs().drone); }
    void teleport(vec2 p) {
        pw().teleport(gs().drone, p, 0.0f);
        pw().set_linear_velocity(gs().drone, {});
        drone().prev_vel = {};
    }
    template <class C, class F>
    u32 count(F&& pred) {
        u32 n = 0;
        w.query<const C&>().each([&](const C& c) { n += pred(c) ? 1u : 0u; });
        return n;
    }
    template <class C>
    Entity first(u32 index = 0) {
        Entity found;
        u32 i = 0;
        w.query<const C&>().each([&](Entity e, const C&) {
            if (i++ == index && found.is_null()) found = e;
        });
        return found;
    }
    vec2 pos_of(Entity e) {
        BodyState s;
        if (pw().body_state(e, s)) return s.position;
        const LocalTransform* t = w.try_get<LocalTransform>(e);
        return t ? vec2{t->position.x, t->position.y} : vec2{};
    }
    const RunStats& stats() {
        const RunStats* r = nullptr;
        w.query<const RunStats&>().each([&](const RunStats& s) { r = &s; });
        REQUIRE(r != nullptr);
        return *r;
    }
};

input::InputState thrust(f32 x, f32 y, bool boost = false, bool tether = false) {
    input::InputState in;
    in.move = {x, y};
    in.jump_held = boost;
    in.jump_pressed = boost;
    in.fire_pressed = tether;
    return in;
}

// A waypoint flyer on top of the scripted input: the same control the Autopilot component uses,
// written against InputState so the test drives the drone the way a player does.
input::InputState fly_to(Game& g, vec2 target, f32 cruise = 5.0f, bool boost = false) {
    const vec2 to = target - g.pos();
    const f32 dist = math::length(to);
    const vec2 want = dist > 0.001f ? to * (math::min(cruise, dist * 1.6f) / dist) : vec2{};
    const vec2 err = want - g.vel();
    return thrust(err.x > 0.35f ? 1.0f : err.x < -0.35f ? -1.0f : 0.0f, err.y > 0.35f ? 1.0f : err.y < -0.35f ? -1.0f : 0.0f, boost);
}

bool fly(Game& g, vec2 target, u32 max_ticks, f32 reach = 0.8f, bool boost = false) {
    for (u32 i = 0; i < max_ticks; ++i) {
        if (math::length(target - g.pos()) < reach) return true;
        g.step(fly_to(g, target, 5.0f, boost));
    }
    return false;
}

}  // namespace

TEST_CASE("undertow: the authored level has the pieces the design needs") {
    Game g;
    CHECK(!g.gs().drone.is_null());
    CHECK(!g.gs().visual.is_null());
    CHECK(!g.gs().boom_emitter.is_null());
    CHECK(!g.gs().spark_emitter.is_null());
    CHECK(g.gs().cores_total == 5);
    CHECK(g.count<Pad>([](const Pad& p) { return p.kind == PadKind::Dock; }) == 1);
    CHECK(g.count<Pad>([](const Pad& p) { return p.kind == PadKind::Relay; }) == 1);
    CHECK(g.count<Beacon>([](const Beacon&) { return true; }) == 5);
    CHECK(g.count<Gate>([](const Gate&) { return true; }) == 2);
    CHECK(g.count<Switch>([](const Switch&) { return true; }) == 3);
    CHECK(g.count<Mine>([](const Mine&) { return true; }) == 8);
    CHECK(g.count<Current>([](const Current&) { return true; }) == 4);
    CHECK(g.count<Mover>([](const Mover&) { return true; }) == 3);
    CHECK(g.count<Thruster>([](const Thruster&) { return true; }) == 4);
    CHECK(g.count<HudText>([](const HudText&) { return true; }) == 3);
    CHECK(g.count<Autopilot>([](const Autopilot&) { return true; }) == 0);  // the main scene is played by a person
    CHECK(g.drone().hull == 3);
    CHECK(g.drone().energy == doctest::Approx(100.0f));
    CHECK(g.gs().phase == RunPhase::Intro);
    // Every gate starts closed (solid) and every pod loose.
    CHECK(g.count<Gate>([](const Gate& gt) { return gt.closed == 1; }) == 2);
    CHECK(g.count<Cargo>([](const Cargo& c) { return c.state == CargoState::Loose; }) == 5);
    // The sequences the game defines resolved their call hooks.
    CHECK(static_cast<bool>(g.w.resource<kit::SequenceRegistry>().validate_calls()));
}

TEST_CASE("undertow: thrust moves the drone, the speed cap holds, boost spends and regains energy") {
    Game g;
    const vec2 start = g.pos();
    g.steps(90, thrust(1.0f, 0.0f));
    CHECK(g.gs().phase == RunPhase::Playing);  // the first input starts the run
    CHECK(g.pos().x > start.x + 3.0f);
    CHECK(math::length(g.vel()) <= g.drone().max_speed + 0.5f);  // the cap applies before the step; one tick of thrust rides on top
    CHECK(g.drone().energy == doctest::Approx(100.0f));
    const f32 plain = math::length(g.vel());
    g.steps(60, thrust(1.0f, 0.0f, true));
    CHECK(math::length(g.vel()) > plain);
    CHECK(math::length(g.vel()) <= g.drone().boost_max_speed + 0.5f);
    CHECK(g.drone().energy < 80.0f);
    const f32 spent = g.drone().energy;
    g.steps(120);
    CHECK(g.drone().energy > spent + 10.0f);
    CHECK(g.drone().hull == 3);
    CHECK(g.gs().deaths == 0);
}

TEST_CASE("undertow: a switch opens its gate for a while, then it closes; a closed gate hurts") {
    Game g;
    g.step(thrust(0.0f, 0.5f));  // start the run
    const Entity gate = g.first<Gate>(0);
    const Entity sw = g.first<Switch>(0);
    REQUIRE(!gate.is_null());
    REQUIRE(!sw.is_null());
    const Gate* gt = g.w.try_get<Gate>(gate);
    CHECK(gt->closed == 1);
    // Touch the switch: the gate's body goes away for open_seconds.
    g.teleport(g.pos_of(sw) + vec2{0.0f, 1.2f});
    for (u32 i = 0; i < 90 && g.w.try_get<Switch>(sw)->presses == 0; ++i) g.step(thrust(0.0f, -1.0f));
    REQUIRE(g.w.try_get<Switch>(sw)->presses == 1);
    g.step();
    CHECK(gt->closed == 0);
    CHECK((g.stats().gates_open & 1u) != 0);
    const u32 open_ticks = static_cast<u32>(g.w.try_get<Switch>(sw)->open_seconds * 60.0f);
    g.teleport({8.5f, 31.0f});  // hover above the gate, in the shaft
    g.steps(open_ticks + 5);
    CHECK(gt->closed == 1);
    CHECK((g.stats().gates_open & 1u) == 0);
    // Flying into the closed gate costs a hull point and knocks the drone back.
    CHECK(g.drone().hull == 3);
    g.teleport({8.5f, 30.0f});
    for (u32 i = 0; i < 120 && g.drone().hull == 3; ++i) g.step(thrust(0.0f, -1.0f));
    CHECK(g.drone().hull == 2);
    CHECK(g.pos().y > 28.5f);  // still on the near side of the gate
}

TEST_CASE("undertow: a mine explodes on the drone (hull, knockback) and is gone; a pad heals") {
    Game g;
    g.step(thrust(0.0f, 0.5f));
    const Entity mine = g.first<Mine>(0);
    REQUIRE(!mine.is_null());
    const vec2 at = g.pos_of(mine);
    const u32 mines = g.count<Mine>([](const Mine&) { return true; });
    g.teleport(at + vec2{-1.6f, 0.0f});
    for (u32 i = 0; i < 90 && g.drone().hull == 3; ++i) g.step(thrust(1.0f, 0.0f));
    CHECK(g.drone().hull == 1);  // a mine does 2
    g.step();
    CHECK(g.count<Mine>([](const Mine&) { return true; }) == mines - 1);
    CHECK(g.vel().x < 0.0f);  // knocked back the way it came
    CHECK(g.gs().deaths == 0);
    // The relay pad restores hull and energy and becomes the checkpoint.
    const Entity relay = g.first<Pad>(1);
    const Pad* pad = g.w.try_get<Pad>(relay);
    REQUIRE(pad->kind == PadKind::Relay);
    g.teleport(g.pos_of(relay) + vec2{0.0f, 2.5f});
    for (u32 i = 0; i < 120 && pad->visited == 0; ++i) g.step(thrust(0.0f, -1.0f));
    CHECK(pad->visited == 1);
    CHECK(g.drone().hull == 3);
    CHECK(g.drone().spawn.x == doctest::Approx(g.pos_of(relay).x));
}

TEST_CASE("undertow: losing the hull respawns at the checkpoint and drops the rope") {
    Game g;
    g.step(thrust(0.0f, 0.5f));
    const vec2 spawn = g.drone().spawn;
    Drone& d = g.drone();
    d.hull = 1;
    const Entity mine = g.first<Mine>(1);
    g.teleport(g.pos_of(mine) + vec2{-1.6f, 0.0f});
    for (u32 i = 0; i < 90 && d.dead_ticks == 0; ++i) g.step(thrust(1.0f, 0.0f));
    REQUIRE(d.dead_ticks > 0);
    CHECK(g.gs().deaths == 1);
    CHECK(g.gs().tethered.is_null());
    g.steps(d.respawn_ticks + 2);
    CHECK(d.dead_ticks == 0);
    CHECK(d.hull == 3);
    CHECK(math::length(g.pos() - spawn) < 0.5f);
}

TEST_CASE("undertow: a pod on the rope is hauled, and the dock takes delivery") {
    Game g;
    g.step(thrust(0.0f, 0.5f));
    const Entity pod = g.first<Cargo>(0);
    REQUIRE(!pod.is_null());
    g.steps(60);  // the pod settles on the cavern floor
    const vec2 pod_at = g.pos_of(pod);
    // Out of range: the throw finds nothing.
    g.teleport(pod_at + vec2{0.0f, 6.0f});
    g.step(thrust(0.0f, 0.0f, false, true));
    CHECK(g.gs().tethered.is_null());
    // In range: the rope attaches and the pod follows.
    g.teleport(pod_at + vec2{0.0f, 2.0f});
    g.step(thrust(0.0f, 0.0f, false, true));
    REQUIRE(g.gs().tethered == pod);
    CHECK(g.w.try_get<Cargo>(pod)->state == CargoState::Tethered);
    CHECK(g.stats().tethered == 1);
    g.steps(240, thrust(0.0f, 1.0f, true));
    CHECK(g.pos_of(pod).y > pod_at.y + 2.0f);
    CHECK(math::length(g.pos_of(pod) - g.pos()) <= g.drone().tether_length + 0.3f);
    // Drop it, pick it up again, then carry it into the dock's sensor.
    g.step(thrust(0.0f, 0.0f, false, true));
    CHECK(g.gs().tethered.is_null());
    CHECK(g.w.try_get<Cargo>(pod)->state == CargoState::Loose);
    g.step(thrust(0.0f, 0.0f, false, true));
    REQUIRE(g.gs().tethered == pod);
    const Entity dock = g.first<Pad>(0);
    REQUIRE(g.w.try_get<Pad>(dock)->kind == PadKind::Dock);
    const vec2 dock_at = g.pos_of(dock);
    g.teleport(dock_at + vec2{0.0f, 4.5f});
    g.pw().teleport(pod, dock_at + vec2{0.0f, 2.6f}, 0.0f);
    for (u32 i = 0; i < 240 && g.gs().cores == 0; ++i) g.step(thrust(0.0f, -1.0f));
    CHECK(g.gs().cores == 1);
    CHECK(g.w.try_get<Cargo>(pod)->state == CargoState::Delivered);
    CHECK(g.gs().tethered.is_null());
    CHECK(g.pos_of(pod).y == doctest::Approx(dock_at.y + 0.9f).epsilon(0.01));
    g.steps(30);
    CHECK(g.count<Beacon>([](const Beacon& b) { return b.lit == 1; }) == 1);
    CHECK(g.stats().cores == 1);
    CHECK(g.gs().phase == RunPhase::Playing);
}

TEST_CASE("undertow: delivering every core wins and runs the win cue through the sequence runner") {
    Game g;
    g.step(thrust(0.0f, 0.5f));
    const Entity dock = g.first<Pad>(0);
    const vec2 dock_at = g.pos_of(dock);
    g.teleport(dock_at + vec2{0.0f, 6.0f});
    g.w.query<const Cargo&>().each([&](Entity e, const Cargo&) { g.pw().teleport(e, dock_at + vec2{0.0f, 2.6f}, 0.0f); });
    for (u32 i = 0; i < 300 && g.gs().phase != RunPhase::Won; ++i) g.step();
    CHECK(g.gs().cores == 5);
    CHECK(g.gs().phase == RunPhase::Won);
    CHECK(g.stats().won == 1);
    const u32 won_at = g.gs().won_at;
    // The win cue is a SequencePlayer on the drone; it ends on its own and leaves the final banner.
    REQUIRE(g.w.has<kit::SequencePlayer>(g.gs().drone));
    g.steps(60 * 4);
    CHECK(g.w.try_get<kit::SequencePlayer>(g.gs().drone)->active == false);
    CHECK(g.gs().banner.view().starts_with("SIGNAL RESTORED"));
    CHECK(g.gs().won_at == won_at);  // the clock stopped
    CHECK(g.count<Beacon>([](const Beacon& b) { return b.lit == 1; }) == 5);
    // Input no longer moves the drone.
    const vec2 p = g.pos();
    g.steps(60, thrust(1.0f, 0.0f, true));
    CHECK(std::fabs(g.pos().x - p.x) < 0.5f);
}

TEST_CASE("undertow: the scripted flyer fetches core 1 and docks it on the main scene") {
    Game g;
    g.step(thrust(0.0f, 0.5f));
    const Entity pod = g.first<Cargo>(0);
    REQUIRE(fly(g, {36.0f, 47.0f}, 600));
    REQUIRE(fly(g, {27.0f, 46.5f}, 600));
    REQUIRE(fly(g, {20.0f, 43.0f}, 600));
    REQUIRE(fly(g, {15.2f, 39.7f}, 600));
    g.step(thrust(0.0f, 0.0f, false, true));
    REQUIRE(g.gs().tethered == pod);
    REQUIRE(fly(g, {22.0f, 46.5f}, 900, 0.8f, true));
    REQUIRE(fly(g, {30.0f, 46.5f}, 900, 0.8f, true));
    REQUIRE(fly(g, {44.0f, 47.0f}, 900, 0.8f, true));
    for (u32 i = 0; i < 900 && g.gs().cores == 0; ++i) g.step(fly_to(g, {50.0f, 46.8f}, 5.0f, true));
    CHECK(g.gs().cores == 1);
    CHECK(g.gs().deaths == 0);
    MESSAGE("core 1 docked at tick " << g.tick << ", hull " << g.drone().hull << ", energy " << g.drone().energy);
}

TEST_CASE("undertow: the attract scene's autopilot docks core 1; two runs hash the same (FR-RUN-040)") {
    auto run = [](u32 ticks) {
        Game g("scenes/attract.mcscene");
        CHECK(g.count<Autopilot>([](const Autopilot&) { return true; }) == 1);
        for (u32 i = 0; i < ticks; ++i) g.step();
        CHECK(g.stats().cores == 1);
        CHECK(g.stats().deaths == 0);
        CHECK(g.stats().started == 1);
        return g.w.state_hash();
    };
    const u64 a = run(2400);
    const u64 b = run(2400);
    CHECK(a == b);
    MESSAGE("attract 2400 ticks: state_hash " << a);
}

namespace {

input::InputState press(const char* action, f32 mx = 0.0f, f32 my = 0.0f) {
    input::InputState in = thrust(mx, my);
    if (action) {
        in.action_count = 1;
        in.action_names[0] = StringId(action);
        in.actions[0].held = in.actions[0].pressed = true;
    }
    return in;
}

// The screen whose flow hides it on `hide_on` (Play: the title, Resume: pause, Back: options).
bool screen_visible(Game& g, const char* hide_on) {
    bool visible = false, found = false;
    g.w.query<const kit::UiFlow&, const kit::UiScreen&>().each([&](const kit::UiFlow& f, const kit::UiScreen& s) {
        if (f.hide_on.view() == StringView(hide_on)) found = true, visible = s.visible;
    });
    REQUIRE(found);
    return visible;
}

}  // namespace

TEST_CASE("undertow: the title holds the run until Play; Pause holds it until Resume; two runs hash the same") {
    auto run = [] {
        Game g("scenes/main.mcscene", true);
        CHECK(g.count<kit::UiFlow>([](const kit::UiFlow&) { return true; }) == 3);
        CHECK(screen_visible(g, "Play"));
        CHECK(g.w.held());
        const u64 before = g.w.state_hash();
        g.steps(30, thrust(1.0f, 0.0f));  // thrust under the title: the run does not move
        CHECK(g.w.state_hash() == before);
        g.step(press("Play"));
        CHECK_FALSE(screen_visible(g, "Play"));
        CHECK_FALSE(g.w.held());
        g.step();  // the first played tick creates the bodies
        const vec2 start = g.pos();
        g.steps(40, thrust(1.0f, 0.0f));
        CHECK(math::length(g.pos() - start) > 0.1f);
        g.step(press("Pause"));
        CHECK(screen_visible(g, "Resume"));
        CHECK(g.w.held());
        const vec2 paused = g.pos();
        g.steps(30, thrust(0.0f, 1.0f));
        CHECK(math::length(g.pos() - paused) < 1e-4f);
        g.step(press("Options"));
        CHECK(screen_visible(g, "Back"));
        g.step(press("Back"));
        CHECK_FALSE(screen_visible(g, "Back"));
        CHECK(g.w.held());
        g.step(press("Resume"));
        CHECK_FALSE(screen_visible(g, "Resume"));
        CHECK_FALSE(g.w.held());
        g.steps(40, thrust(0.0f, 1.0f));
        return g.w.state_hash();
    };
    const u64 a = run();
    const u64 b = run();
    CHECK(a == b);
    MESSAGE("ui flow run: state_hash " << a);
}
