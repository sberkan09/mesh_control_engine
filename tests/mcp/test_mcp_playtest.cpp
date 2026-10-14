// test_run, playtest and run_headless's trace (ROADMAP M-AGENT step 3) in process, against a tiny
// game defined here: a Walker that moves by the "Move" action (top-down, or side view with gravity,
// "Jump" and a wall to climb) and respawns when it crosses a hazard line.

#include <doctest/doctest.h>

#include <filesystem>
#include <string>

#include "mce/editor/play_session.h"
#include "mce/gameplay.h"
#include "mce/input/actions.h"
#include "mce/scene/scene.h"
#include "mcp_client.h"

namespace playtest_game {

using mce::f32;

struct Walker {
    f32 speed = 4.0f;
    bool side = false;       // side view: gravity, Jump, the wall
    f32 wall_x = 1000.0f;    // a wall at this x...
    f32 wall_h = 0.0f;       // ...this high (jump over it)
    f32 hazard_x = 1000.0f;  // crossing this x respawns at the origin
    f32 vy = 0.0f;
    f32 door_x = 1000.0f;    // crossing this x changes to scenes/win.mcscene
};
MCE_COMPONENT(Walker, speed, side, wall_x, wall_h, hazard_x, vy, door_x)

}  // namespace playtest_game

using namespace mce;
using namespace mcp_client;
using namespace cook_test;

namespace {

void walk(Frame& f) {
    const input::InputState& in = f.resource<input::InputState>();
    f.query<playtest_game::Walker&, LocalTransform&>().each([&](playtest_game::Walker& w, LocalTransform& t) {
        const f32 dx = in.move.x * w.speed * f.dt;
        if (w.side) {
            if (in.jump_pressed && t.position.y <= 0.001f) w.vy = 9.0f;
            w.vy -= 25.0f * f.dt;
            t.position.y += w.vy * f.dt;
            if (t.position.y < 0) {
                t.position.y = 0;
                w.vy = 0;
            }
            const f32 nx = t.position.x + dx;
            const bool crosses = (t.position.x < w.wall_x) != (nx < w.wall_x);
            if (!crosses || t.position.y >= w.wall_h) t.position.x = nx;
        } else {
            t.position.x += dx;
            t.position.y += in.move.y * w.speed * f.dt;
        }
        if (t.position.x > w.hazard_x) t.position = math::vec3(0, 0, 0);
        if (t.position.x > w.door_x) scene::request_change(f.world(), "scenes/win.mcscene");
    });
}

ResultV install(World& world) {
    world.register_components<playtest_game::Walker>();
    world.add_system(Phase::FixedUpdate_Pre, "playtest_walk", &walk);
    return ok();
}

struct GameGuard {
    GameGuard() { editor::set_play_game_install(&install); }
    ~GameGuard() { editor::set_play_game_install(nullptr); }
};

std::string scene_text(const std::string& walker) {
    return R"({
  "format": "mce-scene",
  "version": 1,
  "guid": "5ce4e000-0000-4000-8000-0000000007a1",
  "name": "play",
  "entities": [
    { "id": 1, "name": "Player", "components": { "LocalTransform": {}, "Walker": )" +
           walker + R"( } },
    { "id": 2, "name": "Flag", "components": { "LocalTransform": { "position": [6, 4, 0] } } },
    { "id": 3, "name": "Post", "components": { "LocalTransform": { "position": [6, 0, 0] } } }
  ]
}
)";
}

std::string project(const char* name, const std::string& walker = "{}") {
    const std::string root = fresh_dir(name);
    write_file(root + "/assets/scenes/play.mcscene", scene_text(walker));
    return root;
}

f64 num(const cook::JsonValue& v) { return v.as_number(); }

}  // namespace

TEST_CASE("playtest: walk_to reaches a point top-down, and the feel report says how it went") {
    GameGuard game;
    const std::string root = project("mcp_playtest_walk");
    Client c;
    REQUIRE(c.server.open_project(sv(root), "scenes/play.mcscene"));
    c.init();
    auto t = c.ok("playtest", R"({"bot":{"kind":"walk_to","actor":"Player","target":"Flag"},"ticks":600})");
    const cook::JsonValue r = t.v();
    CHECK(r.get("ok").as_bool());
    CHECK(r.get("stopped_early").as_bool());
    CHECK(str(r.get("bot")) == "walk_to");
    const cook::JsonValue feel = r.get("feel");
    CHECK(feel.get("finished").as_bool());
    // 7.2 m at 4 m/s: about 1.8 s (the run stops within reach 0.5).
    CHECK(num(feel.get("time_to_finish_s")) > 1.4);
    CHECK(num(feel.get("time_to_finish_s")) < 2.0);
    CHECK(num(feel.get("deaths")) == 0);
    CHECK(feel.get("stuck_spots").size() == 0);
    CHECK(num(feel.get("distance_m")) > 6.0);
    CHECK(num(r.get("ticks")) < 600);
    // Deterministic: the same bot gives the same run.
    auto again = c.ok("playtest", R"({"bot":{"kind":"walk_to","actor":"Player","target":"Flag"},"ticks":600})");
    CHECK(str(again.v().get("state_hash")) == str(r.get("state_hash")));

    // A target entity id, a point, and argument errors.
    CHECK(c.ok("playtest", R"({"bot":{"kind":"walk_to","actor":1,"target":[-3,0]},"ticks":600})").v().get("feel").get("finished").as_bool());
    CHECK(c.fails("playtest", R"({"ticks":10})").find("'bot'") != std::string::npos);
    CHECK(c.fails("playtest", R"({"bot":{"kind":"fly"}})").find("walk_to, explore or masher") != std::string::npos);
    CHECK(c.fails("playtest", R"({"bot":{"kind":"walk_to","actor":"Nobody","target":[1,1]}})").find("no such entity") != std::string::npos);
    CHECK(c.fails("playtest", R"({"bot":{"kind":"walk_to","actor":"Player"}})").find("bot.target") != std::string::npos);
    CHECK(c.fails("playtest", R"({"bot":{"kind":"explore","actor":"Player"},"speed":3})").find("unknown key 'speed'") != std::string::npos);
}

TEST_CASE("playtest: side view jumps the wall; without Jump it reports the stuck spot; a hazard counts deaths") {
    GameGuard game;
    const std::string root = project("mcp_playtest_side", R"({ "side": true, "wall_x": 3, "wall_h": 1 })");
    Client c;
    REQUIRE(c.server.open_project(sv(root), "scenes/play.mcscene"));
    c.init();
    auto jumped = c.ok("playtest", R"({"bot":{"kind":"walk_to","actor":"Player","target":"Post","plane":"x"},"ticks":900})");
    CHECK(jumped.v().get("feel").get("finished").as_bool());

    auto stuck = c.ok("playtest", R"({"bot":{"kind":"walk_to","actor":"Player","target":"Post","plane":"x","jump":"NoJump"},"ticks":300})");
    const cook::JsonValue feel = stuck.v().get("feel");
    CHECK_FALSE(feel.get("finished").as_bool());
    CHECK_FALSE(stuck.v().get("stopped_early").as_bool());
    REQUIRE(feel.get("stuck_spots").size() >= 1);
    CHECK(num(feel.get("stuck_spots").at(0).get("at").at(0)) == doctest::Approx(3.0).epsilon(0.05));
    CHECK(num(feel.get("stuck_s")) > 2.0);
    CHECK(num(feel.get("idle_s")) > 2.0);

    // A hazard before the wall: each crossing teleports home, which counts as a death.
    const std::string hazard = project("mcp_playtest_hazard", R"({ "hazard_x": 2 })");
    Client h;
    REQUIRE(h.server.open_project(sv(hazard), "scenes/play.mcscene"));
    h.init();
    auto died = h.ok("playtest", R"({"bot":{"kind":"walk_to","actor":"Player","target":"Post"},"ticks":300,)"
                                 R"("teleport":1,"assert":[{"feel":"deaths","<=":0}]})");
    CHECK(num(died.v().get("feel").get("deaths")) >= 2);
    CHECK_FALSE(died.v().get("ok").as_bool());
    CHECK_FALSE(died.v().get("checks").at(0).get("ok").as_bool());
    // A "death" condition counts too (here: the walker standing past x = 1.5 for the first time each life).
    auto counted = h.ok("playtest", R"({"bot":{"kind":"walk_to","actor":"Player","target":"Post"},"ticks":120,"teleport":100,)"
                                    R"("death":{"entity":"Player","field":"LocalTransform.position.x",">":1.9}})");
    CHECK(num(counted.v().get("feel").get("deaths")) >= 1);
}

TEST_CASE("playtest: explore covers ground and masher presses chords, both seeded") {
    GameGuard game;
    const std::string root = project("mcp_playtest_explore");
    Client c;
    REQUIRE(c.server.open_project(sv(root), "scenes/play.mcscene"));
    c.init();
    auto a = c.ok("playtest", R"({"bot":{"kind":"explore","actor":"Player","seed":7},"ticks":900})");
    CHECK(num(a.v().get("feel").get("cells_visited")) > 10);
    CHECK_FALSE(a.v().get("stopped_early").as_bool());
    auto b = c.ok("playtest", R"({"bot":{"kind":"explore","actor":"Player","seed":7},"ticks":900})");
    CHECK(str(a.v().get("state_hash")) == str(b.v().get("state_hash")));
    auto other = c.ok("playtest", R"({"bot":{"kind":"explore","actor":"Player","seed":8},"ticks":900})");
    CHECK(str(a.v().get("state_hash")) != str(other.v().get("state_hash")));

    auto mash = c.ok("playtest", R"({"bot":{"kind":"masher","actor":"Player","actions":["Move","Jump","Fire"],"period":5},"ticks":300,)"
                                 R"("assert":[{"log_errors":0}]})");
    CHECK(mash.v().get("ok").as_bool());
    CHECK(num(mash.v().get("feel").get("distance_m")) > 0.5);
}

TEST_CASE("test_run: .mctest files under tests/ with field, tick, alive, event, log and feel checks") {
    GameGuard game;
    const std::string root = project("mcp_test_run");
    write_file(root + "/tests/reach.mctest", R"({
  "name": "reach the flag",
  "bot": { "kind": "walk_to", "actor": "Player", "target": "Flag" },
  "ticks": 900,
  "assert": [
    { "feel": "finished_tick", ">": 0 },
    { "entity": "Player", "field": "LocalTransform.position.x", ">": 0.5, "tick": 30 },
    { "entity": "Player", "field": "LocalTransform.position", "value": [6, 4, 0], "tolerance": 0.6 },
    { "entity": "Player", "field": "Walker.side", "value": false },
    { "alive": "Flag" },
    { "log_errors": 0 }
  ],
  "trace": { "every": 15, "fields": [{ "entity": "Player", "field": "LocalTransform.position.x" }] }
})");
    write_file(root + "/tests/sub/scripted.mctest", R"({
  "input": "1-60 Move=1,0",
  "ticks": 120,
  "actor": "Player",
  "assert": [
    { "entity": "Player", "field": "LocalTransform.position.x", "op": ">=", "value": 10 },
    { "event": "NoSuchEvent" },
    { "log_contains": "never logged" },
    { "entity": "Player", "field": "LocalTransform.position.x", "<": 1, "tick": 500 }
  ]
})");
    Client c;
    REQUIRE(c.server.open_project(sv(root), "scenes/play.mcscene"));
    c.init();
    auto all = c.ok("test_run");
    const cook::JsonValue v = all.v();
    CHECK(num(v.get("passed")) == 1);
    CHECK(num(v.get("failed")) == 1);
    CHECK_FALSE(v.get("ok").as_bool());
    const cook::JsonValue results = v.get("results");
    REQUIRE(results.size() == 2);
    const cook::JsonValue reach = results.at(0);
    CHECK(str(reach.get("name")) == "reach the flag");
    CHECK(str(reach.get("file")) == "tests/reach.mctest");
    CHECK(reach.get("ok").as_bool());
    CHECK(reach.get("stopped_early").as_bool());
    const cook::JsonValue trace = reach.get("trace");
    REQUIRE(trace.get("ticks").size() >= 4);
    const cook::JsonValue xs = trace.get("series").at(0).get("values");
    CHECK(num(xs.at(xs.size() - 1)) > num(xs.at(0)));

    const cook::JsonValue scripted = results.at(1);
    CHECK(str(scripted.get("file")) == "tests/sub/scripted.mctest");
    CHECK_FALSE(scripted.get("ok").as_bool());
    const cook::JsonValue checks = scripted.get("checks");
    REQUIRE(checks.size() == 4);
    CHECK_FALSE(checks.at(0).get("ok").as_bool());  // 60 ticks at 4 m/s: 4 m, not 10
    CHECK(num(checks.at(0).get("actual")) == doctest::Approx(4.0).epsilon(0.02));
    CHECK_FALSE(checks.at(1).get("ok").as_bool());
    CHECK_FALSE(checks.at(2).get("ok").as_bool());
    CHECK_FALSE(checks.at(3).get("ok").as_bool());
    CHECK(str(checks.at(3).get("note")).find("ended before") != std::string::npos);
    CHECK(scripted.get("log").get("tail").is_array());

    // One file, and one inline scenario.
    CHECK(num(c.ok("test_run", R"({"path":"tests/reach.mctest"})").v().get("passed")) == 1);
    auto inline_run = c.ok("test_run", R"({"test":{"input":"1-30 Move=0,1","ticks":30,"assert":[{"entity":2,"field":"LocalTransform.position.y","value":4}]}})");
    CHECK(inline_run.v().get("ok").as_bool());
    // Until: stop when the walker passes x = 2.
    auto until = c.ok("test_run", R"({"test":{"input":"1- Move=1,0","ticks":600,"until":{"entity":"Player","field":"LocalTransform.position.x",">":2}}})");
    CHECK(until.v().get("results").at(0).get("stopped_early").as_bool());
    CHECK(num(until.v().get("results").at(0).get("ticks")) < 40);

    // Errors name the file and the problem.
    write_file(root + "/bad/broken.mctest", R"({"assert":[{"entity":"Player","field":"Walker.nope","value":1}]})");
    CHECK(c.fails("test_run", R"({"path":"bad"})").find("bad/broken.mctest: assert[0]: no field 'Walker.nope'") != std::string::npos);
    CHECK(c.fails("test_run", R"({"path":"nothing"})").find("no such file or folder") != std::string::npos);
    CHECK(c.fails("test_run", R"({"path":"../x"})").find("inside the project") != std::string::npos);
    CHECK(c.fails("test_run", R"({"test":{"input":"1 Jump","bot":{"kind":"masher"}}})").find("not both") != std::string::npos);
    CHECK(c.fails("test_run", R"({"test":{"assert":[{"feel":"fun","==":1}]}})").find("'feel' is") != std::string::npos);
}

TEST_CASE("test_run: a scene check follows the game's scene changes (scene::request_change)") {
    GameGuard game;
    const std::string root = project("mcp_test_scene_change", R"({"door_x": 2})");
    write_file(root + "/assets/scenes/win.mcscene", R"({ "format": "mce-scene", "version": 1,
  "guid": "5ce4e000-0000-4000-8000-0000000007a2", "name": "win",
  "entities": [{ "name": "Banner", "components": { "LocalTransform": {} } }] })");
    Client c;
    REQUIRE(c.server.open_project(sv(root), "scenes/play.mcscene"));
    c.init();
    // Walking right crosses the door at x = 2 (0.5 s at 4 m/s): the run moves to the win scene.
    auto r = c.ok("test_run", R"({"test":{"input":"1- Move=1,0","ticks":120,"assert":[)"
                              R"({"scene":"scenes/play.mcscene","tick":10},{"scene":"assets/scenes/win.mcscene"},)"
                              R"({"entity":"Player","field":"LocalTransform.position.x",">":0}]}})");
    const cook::JsonValue checks = r.v().get("results").at(0).get("checks");
    REQUIRE(checks.size() == 3);
    CHECK(checks.at(0).get("ok").as_bool());
    CHECK(checks.at(1).get("ok").as_bool());
    CHECK(str(checks.at(1).get("actual")) == "scenes/win.mcscene");
    CHECK_FALSE(checks.at(2).get("ok").as_bool());  // the Player is in the scene the run left
    CHECK(str(checks.at(2).get("note")).find("another scene") != std::string::npos);
    // Until the win scene: the run stops on the tick after the door.
    auto until = c.ok("test_run", R"({"test":{"input":"1- Move=1,0","ticks":600,"until":{"scene":"scenes/win.mcscene"}}})");
    CHECK(until.v().get("results").at(0).get("stopped_early").as_bool());
    CHECK(num(until.v().get("results").at(0).get("ticks")) < 40);
    CHECK(c.fails("test_run", R"({"test":{"assert":[{"scene":"scenes/win.mcscene","==":1}]}})").find("no comparison") != std::string::npos);
    // A change to a scene the pack lacks fails the run.
    std::filesystem::remove(root + "/assets/scenes/win.mcscene");
    auto broken = c.ok("test_run", R"({"test":{"input":"1- Move=1,0","ticks":120}})");
    CHECK_FALSE(broken.v().get("ok").as_bool());
    CHECK(str(broken.v().get("results").at(0).get("error")).find("scene.change") != std::string::npos);
}

TEST_CASE("run_headless trace: fields sampled every N ticks") {
    GameGuard game;
    const std::string root = project("mcp_trace");
    Client c;
    REQUIRE(c.server.open_project(sv(root), "scenes/play.mcscene"));
    c.init();
    auto r = c.ok("run_headless", R"({"ticks":60,"input":"1-60 Move=1,0",)"
                                  R"("trace":{"every":20,"fields":[{"entity":"Player","field":"LocalTransform.position"},{"entity":"Player","field":"Walker.speed"}]}})");
    const cook::JsonValue trace = r.v().get("trace");
    REQUIRE(trace.get("ticks").size() == 3);
    CHECK(num(trace.get("ticks").at(2)) == 60);
    const cook::JsonValue pos = trace.get("series").at(0).get("values");
    CHECK(num(pos.at(0).at(0)) == doctest::Approx(4.0 / 3.0).epsilon(0.02));
    CHECK(num(pos.at(2).at(0)) == doctest::Approx(4.0).epsilon(0.02));
    CHECK(num(trace.get("series").at(1).get("values").at(1)) == 4);
    CHECK(c.fails("run_headless", R"({"trace":{"fields":[{"entity":"Player","field":"Walker"}]}})").find("Component.path") != std::string::npos);
}
