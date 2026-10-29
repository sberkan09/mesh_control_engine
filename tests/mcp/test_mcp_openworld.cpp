// M-OPENWORLD E1: the samples/09_openworld city plays headless over MCP. The assets are the sample's own (copied
// into a project), so this is the sample's gate: it cooks, runs with no log problem, replays to the same hash, the
// traffic and the crowd gather around the player, the player gets into the parked car, drives and gets out, and a
// gunfight in the plaza raises the wanted level and sends the police after the player.

#include <doctest/doctest.h>

#include <filesystem>
#include <string>

#include "mcp_client.h"

using namespace mce;
using namespace mcp_client;
using namespace cook_test;

namespace {

cook::JsonValue entity_at(const Client::Tool& run, u32 i) { return run.v().get("inspect").get("entities").at(i); }
cook::JsonValue comps(const Client::Tool& run, u32 i) { return entity_at(run, i).get("components"); }
double count(const Client::Tool& run) { return run.v().get("inspect").get("count").as_number(); }

std::string drive_input() {
    return "1-100 Move=0,1\\n110 Interact\\n130-330 Move=0,1\\n340-470 TriggerL\\n480 Interact";
}

}  // namespace

TEST_CASE("M-OPENWORLD E1: the 09_openworld city plays, replays, drives and policing answers a shot") {
    const std::string root = fresh_dir("mcp_openworld");
    std::filesystem::copy(std::string(MCE_SOURCE_DIR) + "/samples/09_openworld/assets", root + "/assets",
                          std::filesystem::copy_options::recursive);
    Client c;
    REQUIRE(c.server.open_project(sv(root), "scenes/city.mcscene"));
    c.init();
    REQUIRE(c.ok("cook_run").v().get("ok").as_bool());

    // 5 s with no input: the lights run, traffic and a crowd gather around the player, and nothing logs a problem.
    auto idle = c.ok("run_headless", R"({"ticks":300,"log_lines":0,"inspect":{"component":"TrafficSpawner3D","show":["TrafficSpawner3D"]}})");
    CHECK(idle.v().get("log").get("errors").as_number() == 0);
    CHECK(idle.v().get("log").get("warnings").as_number() == 0);
    REQUIRE(count(idle) == 1);
    const cook::JsonValue spawner = comps(idle, 0).get("TrafficSpawner3D");
    CHECK(spawner.get("alive").as_number() >= 10);
    CHECK(spawner.get("alive").as_number() <= spawner.get("max_cars").as_number());
    CHECK(count(c.ok("run_headless", R"({"ticks":300,"log_lines":0,"inspect":{"component":"Pedestrian3D","show":["Pedestrian3D"]}})")) >= 20);

    // The drive script: walk to the parked car, get in (seated at tick 300), drive west, brake, get out.
    const std::string in = drive_input();
    auto seated = c.ok("run_headless", R"({"ticks":300,"log_lines":0,"input":")" + in + R"(","inspect":{"component":"Rider3D","show":["Rider3D"]}})");
    CHECK(comps(seated, 0).get("Rider3D").get("seated").as_number() == 1);
    const std::string run_args = R"({"ticks":600,"log_lines":0,"input":")" + in +
                                 R"(","inspect":{"component":"Rider3D","show":["Rider3D","LocalTransform"]}})";
    auto drove = c.ok("run_headless", run_args);
    CHECK(c.ok("run_headless", run_args).v().get("state_hash").as_string() == drove.v().get("state_hash").as_string());  // replays match
    CHECK(drove.v().get("log").get("errors").as_number() == 0);
    CHECK(comps(drove, 0).get("Rider3D").get("seated").as_number() == 0);  // out again
    CHECK(comps(drove, 0).get("LocalTransform").get("position").at(0).as_number() < -85.0);  // the car took them west
    // The car starts at x = -72, just off the avenue's sidewalk, and drives west beside it, where crowd walkers are. A walker is
    // a solid kinematic capsule, so one in the way can knock the car aside and stop the drive early. So the check is "moved
    // well west", not a fixed end point.

    // A fight: the player stands 14 m from a police officer on the plaza with the camera level and fires. The officer
    // dies, the wanted level rises, and the others see the player and pursue or attack.
    c.ok("component_set", R"({"entity":"Player","component":"LocalTransform","path":"position","value":[20.263,0.95,22]})");
    c.ok("component_set", R"({"entity":"Camera","component":"CameraFollow3D","value":{"yaw":0.0,"pitch":0.0,"min_pitch":0.0,"offset":[0,0.05,0]}})");
    c.ok("scene_save");
    // M-PEDFLEE: the shots send the walkers near the plaza running; with no shots nobody runs.
    const auto fleeing = [&](const char* input) {
        auto r = c.ok("run_headless", std::string(R"({"ticks":100,"log_lines":0,"input":")") + input +
                                          R"(","inspect":{"component":"Pedestrian3D","show":["Pedestrian3D"]}})");
        u32 n = 0;
        for (u32 i = 0; i < count(r); ++i) n += str(comps(r, i).get("Pedestrian3D").get("mode")) == "Flee" ? 1u : 0u;
        MESSAGE("walkers: " << count(r) << " fleeing: " << n);
        return n;
    };
    CHECK(fleeing("") == 0);
    CHECK(fleeing("20-80 Fire") >= 1);
    auto police = c.ok("run_headless", R"({"ticks":500,"log_lines":0,"input":"20-80 Fire","inspect":{"component":"Police3D","show":["Police3D"]}})");
    CHECK(police.v().get("log").get("errors").as_number() == 0);
    CHECK(count(police) == 3);  // the officer that was shot is gone
    u32 hunting = 0;
    for (u32 i = 0; i < 3; ++i) {
        const std::string state = str(comps(police, i).get("Police3D").get("state"));
        if (state == "Pursue" || state == "Attack") ++hunting;
    }
    CHECK(hunting >= 2);
    auto wanted = c.ok("run_headless", R"({"ticks":500,"log_lines":0,"input":"20-80 Fire","inspect":{"component":"Wanted3D","show":["Wanted3D"]}})");
    CHECK(comps(wanted, 0).get("Wanted3D").get("crimes").as_number() >= 1);
    CHECK(comps(wanted, 0).get("Wanted3D").get("level").as_number() >= 1);
}
