// M-OPENWORLD E1: the samples/09_openworld city plays headless over MCP. The assets are the sample's own (copied
// into a project), so this is the sample's gate: it cooks, runs with no log problem, replays to the same hash, the
// traffic and the crowd gather around the player, the player gets into the parked car, drives and gets out, and a
// gunfight in the plaza raises the wanted level and sends the police after the player.

#include <doctest/doctest.h>

#include <cmath>
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
    // Horn and headlights (M-OWHORN): the player's car has both; G honks once while seated, L turns the headlights on.
    CHECK(count(c.ok("run_headless", R"({"ticks":1,"log_lines":0,"inspect":{"component":"VehicleHorn3D","show":["VehicleHorn3D"]}})")) >= 1);
    {
        auto honked = c.ok("run_headless", R"({"ticks":300,"log_lines":0,"input":")" + in + R"(\n250 Horn\n260 Lights","inspect":{"component":"VehicleHorn3D","show":["VehicleHorn3D","VehicleLights3D"]}})");
        double honks = 0;
        u32 on = 0;
        for (u32 i = 0; i < count(honked); ++i) {
            honks += comps(honked, i).get("VehicleHorn3D").get("honks").as_number();
            on += str(comps(honked, i).get("VehicleLights3D").get("mode")) == "On" ? 1u : 0u;
        }
        CHECK(honks == 1);
        CHECK(on == 1);
    }
    // The car radio (M-OWRADIO): one script, two Music-bus sources; Q cycles stations only while seated.
    CHECK(count(c.ok("run_headless", R"({"ticks":1,"log_lines":0,"inspect":{"component":"ScriptSystem","show":["ScriptSystem"]}})")) == 1);
    {
        const auto radio_gain = [&](const std::string& input, const char* name) {
            auto r = c.ok("run_headless", R"({"ticks":300,"log_lines":0,"input":")" + input +
                                              R"(","inspect":{"entities":[")" + name + R"("],"show":["AudioSource"]}})");
            CHECK(r.v().get("log").get("errors").as_number() == 0);
            return comps(r, 0).get("AudioSource").get("gain_db").as_number();
        };
        const std::string drive = "1-100 Move=0,1\\n110 Interact";
        CHECK(radio_gain(drive + "\\n150 Radio", "Radio 1") == doctest::Approx(-8.0));
        CHECK(radio_gain(drive + "\\n150 Radio", "Radio 2") == doctest::Approx(-60.0));
        CHECK(radio_gain(drive + "\\n150 Radio\\n160 Radio\\n170 Radio", "Radio 1") == doctest::Approx(-60.0));
        CHECK(radio_gain("150 Radio", "Radio 1") == doctest::Approx(-60.0));  // on foot Q does nothing
    }
    const std::string run_args = R"({"ticks":600,"log_lines":0,"input":")" + in +
                                 R"(","inspect":{"component":"Rider3D","show":["Rider3D","LocalTransform"]}})";
    auto drove = c.ok("run_headless", run_args);
    CHECK(c.ok("run_headless", run_args).v().get("state_hash").as_string() == drove.v().get("state_hash").as_string());  // replays match
    CHECK(drove.v().get("log").get("errors").as_number() == 0);
    CHECK(comps(drove, 0).get("Rider3D").get("seated").as_number() == 0);  // out again
    CHECK(comps(drove, 0).get("LocalTransform").get("position").at(0).as_number() < -85.0);  // the car took them west
    // The car starts at x = -72 on the avenue's sidewalk and drives west along it, where crowd walkers are. A walker turns
    // round at a car in its way, but a car that drives into one meets a solid kinematic capsule, which can knock the car
    // aside and stop the drive early. So the check is "moved well west", not a fixed end point.

    // The gun is the player's and aims from the camera; the ammo shop on the avenue refills the spare rounds for 2 cash each.
    {
        auto gun = c.ok("run_headless", R"({"ticks":1,"log_lines":0,"inspect":{"component":"Respawn3D","show":["Weapon3D","Confiscate3D"]}})");
        REQUIRE(count(gun) == 1);
        CHECK(comps(gun, 0).get("Weapon3D").get("reserve").as_number() == 150);
        // The scene was saved and reloaded for this run: the weapon still names an entity (the camera) after the remap.
        CHECK(str(comps(gun, 0).get("Weapon3D").get("aim_from")).substr(0, 1) == "$");
        CHECK(count(c.ok("run_headless", R"({"ticks":1,"log_lines":0,"inspect":{"component":"AmmoShop3D","show":["AmmoShop3D"]}})")) == 1);
        c.ok("component_set", R"({"entity":"Player","component":"LocalTransform","path":"position","value":[-30,0.95,-9]})");
        c.ok("component_set", R"({"entity":"Player","component":"Weapon3D","path":"reserve","value":10})");
        c.ok("scene_save");
        auto bought = c.ok("run_headless", R"({"ticks":30,"log_lines":0,"inspect":{"component":"Respawn3D","show":["Weapon3D","Wallet3D"]}})");
        CHECK(bought.v().get("log").get("errors").as_number() == 0);
        CHECK(comps(bought, 0).get("Weapon3D").get("reserve").as_number() == 150);
        CHECK(comps(bought, 0).get("Wallet3D").get("cash").as_number() == 220);  // 140 rounds at 2
        c.ok("component_set", R"({"entity":"Player","component":"Weapon3D","path":"reserve","value":150})");
    }

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
    // The plaza squad is the Police3D with min_level 1 (the cars and roadblock officers answer from level 2 and 3).
    u32 foot = 0;
    u32 hunting = 0;
    for (u32 i = 0; i < count(police); ++i) {
        const cook::JsonValue unit = comps(police, i).get("Police3D");
        if (unit.get("min_level").as_number() != 1) continue;
        ++foot;
        const std::string state = str(unit.get("state"));
        if (state == "Pursue" || state == "Attack") ++hunting;
    }
    CHECK(foot == 3);  // the officer that was shot is gone
    CHECK(hunting >= 2);
    auto wanted = c.ok("run_headless", R"({"ticks":500,"log_lines":0,"input":"20-80 Fire","inspect":{"component":"Wanted3D","show":["Wanted3D"]}})");
    CHECK(comps(wanted, 0).get("Wanted3D").get("crimes").as_number() >= 1);
    CHECK(comps(wanted, 0).get("Wanted3D").get("level").as_number() >= 1);

    // M-OPENWORLD E4: police cars, roadblocks and respawn. All of it is the sample's data on the existing kit components.
    auto units = c.ok("run_headless", R"({"ticks":1,"log_lines":0,"inspect":{"component":"Police3D","show":["Police3D"]}})");
    u32 cars = 0;
    for (u32 i = 0; i < count(units); ++i) {
        const cook::JsonValue unit = comps(units, i).get("Police3D");
        if (unit.get("drive_car").as_number() == 2) ++cars;
    }
    CHECK(cars == 4);
    // The roadblocks are markers: nothing stands there until the wanted level reaches 3.
    CHECK(count(c.ok("run_headless", R"({"ticks":1,"log_lines":0,"inspect":{"component":"Roadblock3D","show":["Roadblock3D"]}})")) == 2);
    CHECK(count(c.ok("run_headless", R"({"ticks":30,"log_lines":0,"inspect":{"component":"RoadblockPiece3D","show":["RoadblockPiece3D"]}})")) == 0);
    CHECK(count(c.ok("run_headless", R"({"ticks":1,"log_lines":0,"inspect":{"component":"RespawnPoint3D","show":["RespawnPoint3D"]}})")) == 2);

    // Busted: a level-1 offender who stands still beside a plaza officer is arrested and wakes up at the station, 100 poorer.
    const auto wake = [&](const char* ticks) {
        c.ok("scene_save");
        auto r = c.ok("run_headless", std::string(R"({"ticks":)") + ticks +
                                          R"(,"log_lines":0,"inspect":{"component":"Respawn3D","show":["Respawn3D","Health3D","LocalTransform","Wanted3D","Wallet3D","Weapon3D"]}})");
        CHECK(r.v().get("log").get("errors").as_number() == 0);
        REQUIRE(count(r) == 1);
        return r;
    };
    c.ok("component_set", R"({"entity":"Player","component":"LocalTransform","path":"position","value":[22,0.95,8.6]})");
    c.ok("component_set", R"({"entity":"Player","component":"Wanted3D","path":"heat","value":15})");
    auto busted = wake("600");
    cook::JsonValue pc = comps(busted, 0);
    CHECK(pc.get("LocalTransform").get("position").at(0).as_number() > 30.0);  // the station is at x = 40
    CHECK(pc.get("Wanted3D").get("level").as_number() == 0);
    CHECK(pc.get("Wallet3D").get("cash").as_number() == 400);
    CHECK(pc.get("Weapon3D").get("reserve").as_number() == 75);  // Confiscate3D: half the spare rounds are kept

    // Level 4 with the player 110 m from the east marker (and 310 m from the west one): only the east block stands, 2 cars + 2 officers.
    c.ok("component_set", R"({"entity":"Player","component":"LocalTransform","path":"position","value":[100,0.95,60]})");
    c.ok("component_set", R"({"entity":"Player","component":"Wanted3D","path":"heat","value":70})");
    c.ok("scene_save");
    auto standing = c.ok("run_headless", R"({"ticks":120,"log_lines":0,"inspect":{"component":"RoadblockPiece3D","show":["RoadblockPiece3D","LocalTransform"]}})");
    CHECK(standing.v().get("log").get("errors").as_number() == 0);
    CHECK(count(standing) == 4);
    for (u32 i = 0; i < count(standing); ++i) CHECK(comps(standing, i).get("LocalTransform").get("position").at(0).as_number() > 190.0);

    // Wasted: at level 3 the west roadblock (up once the player is 50 m away) shoots the player; it wakes up at the hospital, 200 poorer (the runs start from the saved scene, so from 500 cash again).
    c.ok("component_set", R"({"entity":"Player","component":"LocalTransform","path":"position","value":[-160,0.95,0]})");
    c.ok("component_set", R"({"entity":"Player","component":"Wanted3D","path":"heat","value":70})");
    auto wasted = wake("1800");
    pc = comps(wasted, 0);
    CHECK(pc.get("LocalTransform").get("position").at(0).as_number() < -100.0);  // the hospital is on the west avenue
    CHECK(pc.get("Health3D").get("current").as_number() > 50.0);
    CHECK(pc.get("Wanted3D").get("level").as_number() == 0);
    CHECK(pc.get("Wallet3D").get("cash").as_number() == 300);  // each run starts from the saved scene: 500 - 200

    // M-OPENWORLD E7/E8: missions, money to find and shops, all data on existing kit components.
    CHECK(count(c.ok("run_headless", R"({"ticks":1,"log_lines":0,"inspect":{"component":"Mission","show":["Mission"]}})")) == 6);
    CHECK(count(c.ok("run_headless", R"({"ticks":1,"log_lines":0,"inspect":{"component":"CashPickup3D","show":["CashPickup3D"]}})")) == 12);
    CHECK(count(c.ok("run_headless", R"({"ticks":1,"log_lines":0,"inspect":{"component":"ChopShop3D","show":["ChopShop3D"]}})")) == 1);
    CHECK(count(c.ok("run_headless", R"({"ticks":1,"log_lines":0,"inspect":{"component":"HealShop3D","show":["HealShop3D"]}})")) == 1);
    // Loot: the player carries a bag with the use hotkey, every spawned walker may drop an item when killed.
    CHECK(count(c.ok("run_headless", R"({"ticks":1,"log_lines":0,"inspect":{"component":"ItemHotkey3D","show":["ItemHotkey3D","Inventory"]}})")) == 1);
    CHECK(count(c.ok("run_headless", R"({"ticks":300,"log_lines":0,"inspect":{"component":"LootDrop","show":["LootDrop"]}})")) >= 20);
    // Cars: crashes wear them down (every car), and the cars the player can take also run people over.
    CHECK(count(c.ok("run_headless", R"({"ticks":1,"log_lines":0,"inspect":{"component":"VehicleRam3D","show":["VehicleRam3D"]}})")) >= 3);
    CHECK(count(c.ok("run_headless", R"({"ticks":120,"log_lines":0,"inspect":{"component":"ImpactDamage3D","show":["ImpactDamage3D"]}})")) >= 5);
    CHECK(count(c.ok("run_headless", R"({"ticks":120,"log_lines":0,"inspect":{"component":"ParticleBurst","show":["ParticleBurst"]}})")) >= 3);
    CHECK(count(c.ok("run_headless", R"({"ticks":1,"log_lines":0,"inspect":{"component":"RepairShop3D","show":["RepairShop3D"]}})")) == 2);
    // The pause menu (Esc): two hidden screens, one rule set (SAVE, LOAD); Pause shows the menu and holds the player, Resume walks again.
    CHECK(count(c.ok("run_headless", R"({"ticks":1,"log_lines":0,"inspect":{"component":"UiFlow","show":["UiFlow"]}})")) == 2);
    CHECK(count(c.ok("run_headless", R"({"ticks":1,"log_lines":0,"inspect":{"component":"UiFlowRules","show":["UiFlowRules"]}})")) == 1);
    {
        const auto player_z = [&](const char* input) {
            auto r = c.ok("run_headless", std::string(R"({"ticks":120,"log_lines":0,"input":")") + input +
                                              R"(","inspect":{"component":"CharacterController3D","show":["LocalTransform"]}})");
            CHECK(r.v().get("log").get("errors").as_number() == 0);
            return comps(r, 0).get("LocalTransform").get("position").at(2).as_number();
        };
        const double start = player_z("");
        const double paused = player_z("5 Pause\\n10-110 Move=0,1");
        const double resumed = player_z("5 Pause\\n10 Resume\\n20-110 Move=0,1");
        CHECK(std::abs(paused - start) < 0.01);
        CHECK(std::abs(resumed - start) > 1.0);
    }
    // Kill and interact objectives (Hit, Cop killer, Pickup): the objectives exist with the right kinds; the logic is tested in test_missions.
    {
        auto obj = c.ok("run_headless", R"({"ticks":1,"log_lines":0,"inspect":{"component":"Objective","show":["Objective"]}})");
        u32 kills = 0, interacts = 0;
        for (u32 i = 0; i < count(obj); ++i) {
            const std::string kind = str(comps(obj, i).get("Objective").get("kind"));
            kills += kind == "Kill" ? 1u : 0u;
            interacts += kind == "Interact" ? 1u : 0u;
        }
        CHECK(kills == 2);
        CHECK(interacts == 1);
        CHECK(count(c.ok("run_headless", R"({"ticks":1,"log_lines":0,"inspect":{"component":"Health3D","show":["Health3D"]}})")) >= 1);
    }
    // Standing on the Errand marker starts the mission.
    c.ok("component_set", R"({"entity":"Player","component":"LocalTransform","path":"position","value":[-45,0.95,-9]})");
    c.ok("component_set", R"({"entity":"Player","component":"Wanted3D","path":"heat","value":0})");
    c.ok("scene_save");
    const auto errand_state = [&](const char* ticks) {
        auto r = c.ok("run_headless", std::string(R"({"ticks":)") + ticks +
                                          R"(,"log_lines":0,"inspect":{"component":"Mission","show":["Mission"]}})");
        CHECK(r.v().get("log").get("errors").as_number() == 0);
        for (u32 i = 0; i < count(r); ++i) {
            const cook::JsonValue m = comps(r, i).get("Mission");
            if (str(m.get("id")) == "errand") return str(m.get("state"));
        }
        return std::string("missing");
    };
    CHECK(errand_state("30") == "Active");
    // Standing at the Errand's destination with the mission started completes it: 500 + 250.
    c.ok("component_set", R"({"entity":"Mission Errand","component":"Mission","path":"start","value":1})");
    c.ok("component_set", R"({"entity":"Player","component":"LocalTransform","path":"position","value":[95,0.95,-9]})");
    c.ok("scene_save");
    CHECK(errand_state("60") == "Complete");
    auto rich = c.ok("run_headless", R"({"ticks":60,"log_lines":0,"inspect":{"component":"Respawn3D","show":["Wallet3D"]}})");
    REQUIRE(count(rich) == 1);
    CHECK(comps(rich, 0).get("Wallet3D").get("cash").as_number() == 750);
    // Standing on a cash pad picks it up (the pad pays 40 to 100).
    c.ok("component_set", R"({"entity":"Player","component":"LocalTransform","path":"position","value":[-170,0.95,9]})");
    c.ok("scene_save");
    auto found = c.ok("run_headless", R"({"ticks":30,"log_lines":0,"inspect":{"component":"Respawn3D","show":["Wallet3D"]}})");
    REQUIRE(count(found) == 1);
    CHECK(comps(found, 0).get("Wallet3D").get("cash").as_number() > 500.0);
}
