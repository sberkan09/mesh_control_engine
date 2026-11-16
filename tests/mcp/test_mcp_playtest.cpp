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
    mce::u32 flaps = 0;      // side view: Jump presses allowed in the air (a double jump)
    mce::u32 flapped = 0;
    mce::u8 seen = 0;        // a "sentry sees the player" flag the scene sets
    mce::u32 crouch_ticks = 0;    // ticks the Crouch action was held
    mce::u32 interacts = 0;       // Interact presses
    mce::u32 moved_ticks = 0;     // ticks the Move action was non-zero
};
MCE_COMPONENT(Walker, speed, side, wall_x, wall_h, hazard_x, vy, door_x, flaps, flapped, seen, crouch_ticks, interacts, moved_ticks)

struct HazardCrossed {  // pushed on each respawn
    mce::u32 unused = 0;
};

}  // namespace playtest_game

using namespace mce;
using namespace mcp_client;
using namespace cook_test;

namespace {

void walk(Frame& f) {
    const input::InputState& in = f.resource<input::InputState>();
    f.query<playtest_game::Walker&, LocalTransform&>().each([&](playtest_game::Walker& w, LocalTransform& t) {
        const StringId crouch("Crouch"), interact("Interact");
        if (in.has_action(crouch) && in.held(crouch)) ++w.crouch_ticks;
        if (in.has_action(interact) && in.pressed(interact)) ++w.interacts;
        if (in.move.x != 0.0f || in.move.y != 0.0f) ++w.moved_ticks;
        const f32 dx = in.move.x * w.speed * f.dt;
        if (w.side) {
            if (in.jump_pressed && t.position.y <= 0.001f) {
                w.vy = 9.0f;
                w.flapped = 0;
            } else if (in.jump_pressed && w.flapped < w.flaps) {
                w.vy = 9.0f;
                ++w.flapped;
            }
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
        if (t.position.x > w.hazard_x) {
            t.position = math::vec3(0, 0, 0);
            f.events().push(playtest_game::HazardCrossed{});
        }
        if (t.position.x > w.door_x) scene::request_change(f.world(), "scenes/win.mcscene");
    });
}

ResultV install(World& world) {
    world.register_components<playtest_game::Walker>();
    world.register_event<playtest_game::HazardCrossed>(Phase::FixedUpdate_Pre);
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
    // Event checks take the C++ name or the name without its namespace; an unknown one fails with the list.
    auto events = h.ok("playtest", R"({"bot":{"kind":"walk_to","actor":"Player","target":"Post"},"ticks":300,"assert":[)"
                                   R"({"event":"HazardCrossed",">=":2},{"event":"playtest_game::HazardCrossed",">=":2},{"event":"Crossed","==":0}]})");
    const cook::JsonValue ev = events.v().get("checks");
    CHECK(ev.at(0).get("ok").as_bool());
    CHECK(ev.at(1).get("ok").as_bool());
    CHECK_FALSE(ev.at(2).get("ok").as_bool());
    CHECK(str(ev.at(2).get("note")).find("playtest_game::HazardCrossed") != std::string::npos);
    CHECK_FALSE(died.v().get("ok").as_bool());
    CHECK_FALSE(died.v().get("checks").at(0).get("ok").as_bool());
    // A "death" condition counts too (here: the walker standing past x = 1.5 for the first time each life).
    auto counted = h.ok("playtest", R"({"bot":{"kind":"walk_to","actor":"Player","target":"Post"},"ticks":120,"teleport":100,)"
                                    R"("death":{"entity":"Player","field":"LocalTransform.position.x",">":1.9}})");
    CHECK(num(counted.v().get("feel").get("deaths")) >= 1);
}

TEST_CASE("playtest: a count check reads how many live entities have a component") {
    GameGuard game;
    const std::string root = project("mcp_playtest_count");
    Client c;
    REQUIRE(c.server.open_project(sv(root), "scenes/play.mcscene"));
    c.init();
    auto r = c.ok("playtest", R"({"bot":{"kind":"walk_to","actor":"Player","target":"Post"},"ticks":30,"assert":[)"
                              R"({"count":"LocalTransform",">=":2},{"count":"LocalTransform","<=":0},{"count":"Health3D","==":0}]})");
    const cook::JsonValue checks = r.v().get("checks");
    CHECK(checks.at(0).get("ok").as_bool());
    CHECK(num(checks.at(0).get("actual")) >= 2);
    CHECK_FALSE(checks.at(1).get("ok").as_bool());
    CHECK(checks.at(2).get("ok").as_bool());
    CHECK(str(checks.at(0).get("check")) == "count LocalTransform");
    CHECK(c.fails("playtest", R"({"bot":{"kind":"explore","actor":"Player"},"assert":[{"count":"NoSuchThing",">":0}]})").find("component type name") != std::string::npos);
    CHECK(c.fails("playtest", R"({"bot":{"kind":"explore","actor":"Player"},"assert":[{"count":"LocalTransform"}]})").find("needs a comparison") != std::string::npos);
}

TEST_CASE("playtest: walk_to through waypoints, air jumps, and the game's reaction to the arrival") {
    GameGuard game;
    const std::string root = project("mcp_playtest_through");
    Client c;
    REQUIRE(c.server.open_project(sv(root), "scenes/play.mcscene"));
    c.init();
    // By the Post (6, 0), then the Flag (6, 4): 10 m instead of the straight 7.2 m.
    auto t = c.ok("playtest", R"({"bot":{"kind":"walk_to","actor":"Player","through":["Post",[6,2]],"target":"Flag"},"ticks":900})");
    const cook::JsonValue feel = t.v().get("feel");
    CHECK(feel.get("finished").as_bool());
    CHECK(num(feel.get("distance_m")) > 8.5);
    CHECK(num(feel.get("time_to_finish_s")) > 2.2);
    // The run goes on half a second after the arrival, so the game can react to it.
    CHECK(num(t.v().get("ticks")) == doctest::Approx(num(feel.get("time_to_finish_s")) * 60 + 30).epsilon(0.02));
    CHECK(c.fails("playtest", R"({"bot":{"kind":"walk_to","actor":"Player","through":[],"target":"Flag"}})").find("bot.through") != std::string::npos);
    CHECK(c.fails("playtest", R"({"bot":{"kind":"walk_to","actor":"Player","through":["Nobody"],"target":"Flag"}})").find("no such entity") != std::string::npos);
    // A waypoint with a dwell stands still there `wait` ticks (SBA-61): a second longer than going by.
    auto by = c.ok("playtest", R"({"bot":{"kind":"walk_to","actor":"Player","through":["Post"],"target":"Flag"},"ticks":900})");
    auto stay = c.ok("playtest", R"({"bot":{"kind":"walk_to","actor":"Player","through":[{"at":"Post","wait":60}],"target":"Flag"},"ticks":900})");
    REQUIRE(stay.v().get("feel").get("finished").as_bool());
    CHECK(num(stay.v().get("feel").get("time_to_finish_s")) - num(by.v().get("feel").get("time_to_finish_s")) == doctest::Approx(1.0).epsilon(0.1));
    CHECK(c.fails("playtest", R"({"bot":{"kind":"walk_to","actor":"Player","through":[{"at":"Post","wait":-1}],"target":"Flag"}})").find("wait") != std::string::npos);
    CHECK(c.fails("playtest", R"({"bot":{"kind":"walk_to","actor":"Player","through":[{"wait":5}],"target":"Flag"}})").find("\"at\"") != std::string::npos);

    // Side view, a wall one jump cannot clear (1.6 m high at most) but a jump and two flaps can:
    // air_jumps presses Jump in the air on purpose (the plain bot only gets there when a stall's
    // retry happens to land mid-air).
    const std::string side = project("mcp_playtest_flap", R"({ "side": true, "wall_x": 3, "wall_h": 3.4, "flaps": 2 })");
    Client s;
    REQUIRE(s.server.open_project(sv(side), "scenes/play.mcscene"));
    s.init();
    auto plain = s.ok("playtest", R"({"bot":{"kind":"walk_to","actor":"Player","target":"Post","plane":"x"},"ticks":600})");
    auto flap = s.ok("playtest", R"({"bot":{"kind":"walk_to","actor":"Player","target":"Post","plane":"x","air_jumps":2},"ticks":600})");
    REQUIRE(flap.v().get("feel").get("finished").as_bool());
    if (plain.v().get("feel").get("finished").as_bool())
        CHECK(num(flap.v().get("feel").get("time_to_finish_s")) < num(plain.v().get("feel").get("time_to_finish_s")));
    CHECK(s.fails("playtest", R"({"bot":{"kind":"walk_to","actor":"Player","target":"Post","air_jumps":9}})").find("bot.air_jumps") != std::string::npos);

    // A door on the way changes the scene: the level is over, not the actor dead.
    const std::string door = project("mcp_playtest_door", R"({"door_x": 2})");
    write_file(door + "/assets/scenes/win.mcscene", R"({ "format": "mce-scene", "version": 1,
  "guid": "5ce4e000-0000-4000-8000-0000000007a2", "name": "win",
  "entities": [{ "name": "Banner", "components": { "LocalTransform": {} } }] })");
    Client d;
    REQUIRE(d.server.open_project(sv(door), "scenes/play.mcscene"));
    d.init();
    auto through_door = d.ok("playtest", R"({"bot":{"kind":"walk_to","actor":"Player","target":"Post"},"ticks":200})");
    CHECK(through_door.v().get("feel").get("finished").as_bool());
    CHECK(num(through_door.v().get("feel").get("deaths")) == 0);
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

    // reachable (M-PROCGEN 4): every seed of the run generates a level whose exit can be reached from its start.
    auto reach_ok = c.ok("test_run", R"({"test":{"input":"1 Move=0,0","ticks":5,"assert":[
        {"reachable":{"kind":"rooms","width":48,"height":32,"seed":1,"count":40}},
        {"reachable":{"kind":"caves","width":48,"height":32,"count":40}}]}})");
    CHECK(reach_ok.v().get("ok").as_bool());
    CHECK(str(reach_ok.v().get("results").at(0).get("checks").at(0).get("check")).find("rooms") != std::string::npos);
    // A grid the generator refuses fails the check and names the seed.
    auto reach_bad = c.ok("test_run", R"({"test":{"input":"1 Move=0,0","ticks":5,"assert":[{"reachable":{"kind":"caves","width":8,"height":8,"count":3}}]}})");
    CHECK_FALSE(reach_bad.v().get("ok").as_bool());
    CHECK(str(reach_bad.v().get("results").at(0).get("checks").at(0).get("note")).find("seed 1:") != std::string::npos);
    CHECK(c.fails("test_run", R"({"test":{"assert":[{"reachable":{"kind":"maze","width":32,"height":32}}]}})").find("rooms, caves, walk or noise") != std::string::npos);
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

TEST_CASE("playtest: bots hold, press and stop on a field check (crouch, interact, freeze when seen)") {
    GameGuard game;
    // The Player starts at the origin; `seen` is the scene's "a sentry sees the player" flag.
    const std::string root = project("mcp_playtest_actions", R"({ "seen": 1 })");
    Client c;
    REQUIRE(c.server.open_project(sv(root), "scenes/play.mcscene"));
    c.init();
    auto ran = [&](const std::string& bot, const char* ticks) {
        return c.ok("playtest", std::string(R"({"ticks":)") + ticks + R"(,"bot":)" + bot + R"(,"assert":[)" +
                                    R"({"entity":"Player","field":"Walker.crouch_ticks",">=":0},)" +
                                    R"({"entity":"Player","field":"Walker.interacts",">=":0},)" +
                                    R"({"entity":"Player","field":"Walker.moved_ticks",">=":0}]})");
    };
    auto actual = [](const auto& r, u32 i) { return num(r.v().get("checks").at(i).get("actual")); };

    // Without `hold` the game sees no Crouch; with it, it is down on every tick.
    auto plain = ran(R"({"kind":"walk_to","actor":"Player","target":[60,0]})", "120");
    CHECK(actual(plain, 0) == 0);
    auto held = ran(R"({"kind":"walk_to","actor":"Player","target":[60,0],"hold":["Crouch"]})", "120");
    CHECK(actual(held, 0) == 120);
    CHECK(actual(held, 2) > 0);  // it still walked

    // `hold_when` holds Crouch only while the check is true: `seen` is 1 here, so all ticks; 0 makes it never.
    auto when_seen = ran(R"({"kind":"walk_to","actor":"Player","target":[60,0],"hold_when":[{"action":"Crouch","entity":"Player","field":"Walker.seen","value":1}]})", "120");
    CHECK(actual(when_seen, 0) == 120);
    auto when_not = ran(R"({"kind":"walk_to","actor":"Player","target":[60,0],"hold_when":[{"action":"Crouch","entity":"Player","field":"Walker.seen","value":0}]})", "120");
    CHECK(actual(when_not, 0) == 0);

    // `stop_when` freezes the bot while it holds: seen == 1 means it never moves; seen == 0 never stops it.
    auto frozen = ran(R"({"kind":"walk_to","actor":"Player","target":[60,0],"stop_when":{"entity":"Player","field":"Walker.seen","value":1}})", "120");
    CHECK(actual(frozen, 2) == 0);
    auto free_run = ran(R"({"kind":"walk_to","actor":"Player","target":[60,0],"stop_when":{"entity":"Player","field":"Walker.seen","value":0}})", "120");
    CHECK(actual(free_run, 2) > 0);

    // A waypoint's `press` goes out once, on arrival: Interact pressed once at the point, once at the target.
    auto pressed = ran(R"({"kind":"walk_to","actor":"Player","through":[{"at":[1,0],"press":["Interact"]}],"target":[2,0]})", "300");
    CHECK(actual(pressed, 1) == 1);
    auto both = ran(R"({"kind":"walk_to","actor":"Player","through":[{"at":[1,0],"press":["Interact"]}],"target":{"at":[2,0],"press":["Interact"]}})", "300");
    CHECK(actual(both, 1) == 2);

    // Mistakes name the key.
    CHECK(c.fails("playtest", R"({"bot":{"kind":"walk_to","actor":"Player","target":[1,0],"hold":"Crouch"}})").find("bot.hold") != std::string::npos);
    const std::string no_check = c.fails("playtest", R"({"bot":{"kind":"walk_to","actor":"Player","target":[1,0],"hold_when":[{"action":"Crouch"}]}})");
    CHECK_MESSAGE(no_check.find("field") != std::string::npos, no_check);  // no check given
    CHECK(c.fails("playtest", R"({"bot":{"kind":"walk_to","actor":"Player","target":[1,0],"stop_when":{"alive":"Player"}}})").find("bot.stop_when") != std::string::npos);
    CHECK(c.fails("playtest", R"({"bot":{"kind":"walk_to","actor":"Player","through":[{"at":[1,0],"press":[]}],"target":[2,0]}})").find("press") != std::string::npos);
}
