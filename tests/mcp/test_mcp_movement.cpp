// M-MOVE3D demo (ROADMAP, Linear SBA-139): the samples/08_movement course and race built over MCP alone in an
// empty project. scenes/course.mcscene: a humanoid rides a lift, jumps a gap late enough to need coyote time and
// crouches through a tunnel whose camera arm shortens against the walls. scenes/race.mcscene: a cart the player
// drives races two AI karts around a short track; corners skid, laps are counted by reactions. run_headless
// replays both to the same hash.
//
// MCE_MOVEMENT_EXPORT=<dir> copies the built project's assets/ there (how the sample was made:
// MCE_MOVEMENT_EXPORT=samples/08_movement build/.../test_mcp_movement).

#include <doctest/doctest.h>

#include <algorithm>
#include <cmath>
#include <cstdlib>
#include <filesystem>
#include <string>

#include "mcp_client.h"

using namespace mce;
using namespace mcp_client;
using namespace cook_test;

namespace {

std::string cat(std::initializer_list<std::string_view> parts) {
    std::string out;
    for (std::string_view p : parts) out += p;
    return out;
}

std::string num(int v) { return std::to_string(v); }
std::string fnum(double v) { return std::to_string(v); }

int make(Client& c, const std::string& args) { return static_cast<int>(c.ok("entity_create", args).v().get("id").as_number()); }

std::string ref(const std::string& guid) { return cat({R"("MeshRef":{"mesh":")", guid, R"("})"}); }

// A static box: collider and mesh from one gen_mesh asset.
int block(Client& c, const char* name, const std::string& guid, double x, double y, double z, double hx, double hy, double hz) {
    return make(c, cat({R"({"name":")", name, R"(","components":{"LocalTransform":{"position":[)", fnum(x), ",", fnum(y), ",", fnum(z),
                        R"(]},"BoxCollider3D":{"half":[)", fnum(hx), ",", fnum(hy), ",", fnum(hz), "]},", ref(guid), "}}"}));
}

std::string box_mesh(Client& c, const char* file, double hx, double hy, double hz, const char* color) {
    return str(c.ok("gen_mesh", cat({R"({"shape":"box","size":[)", fnum(2 * hx), ",", fnum(2 * hy), ",", fnum(2 * hz), R"(],"color":")", color,
                                     R"(","save":"models/)", file, R"("})"}))
                   .v()
                   .get("saved")
                   .get("guid"));
}

cook::JsonValue comps(const Client::Tool& run, u32 i) { return run.v().get("inspect").get("entities").at(i).get("components"); }
double at(const Client::Tool& run, u32 i, const char* comp, const char* field, u32 lane) {
    return comps(run, i).get(comp).get(field).at(lane).as_number();
}

struct Course {
    int runner = 0, lift = 0, camera = 0, goal = 0;
};

Course build_course(Client& c) {
    const std::string floor_m = box_mesh(c, "ground.glb", 40, 0.5, 60, "#6b7a5e");
    const std::string lift_m = box_mesh(c, "lift.glb", 2, 0.25, 2, "#4a6fa5");
    const std::string deck_a_m = box_mesh(c, "deck_a.glb", 3, 0.25, 7, "#8a8478");
    const std::string deck_b_m = box_mesh(c, "deck_b.glb", 3, 0.25, 13.5, "#8a8478");
    const std::string wall_m = box_mesh(c, "tunnel_wall.glb", 0.25, 1.5, 3, "#a0703c");
    const std::string lintel_m = box_mesh(c, "tunnel_lintel.glb", 2.75, 0.35, 3, "#a0703c");
    const std::string goal_m = box_mesh(c, "goal.glb", 3, 0.05, 1, "#d0a020");
    const std::string runner_m = str(c.ok("gen_mesh", R"({"shape":"humanoid","color":"#3060b0","save":"models/runner.glb"})").v().get("saved").get("guid"));

    c.ok("scene_new", R"({"name":"course"})");
    Course k;
    block(c, "Ground", floor_m, 0, -0.5, -20, 40, 0.5, 60);
    // The lift: a dynamic plate on a slider to the world, its motor raising it three metres.
    k.lift = make(c, cat({R"({"name":"Lift","components":{"LocalTransform":{"position":[0,0.25,0]},"RigidBody3D":{"half":[2,0.25,2],"density":500},)",
                          R"("Joint3D":{"kind":"Slider","axis":[0,1,0],"limits":1,"limit_min":0,"limit_max":3,)",
                          R"("motor":"Velocity","motor_target":1},)", ref(lift_m), "}}"}));
    block(c, "Deck A", deck_a_m, 0, 3.25, -9, 3, 0.25, 7);
    block(c, "Deck B", deck_b_m, 0, 3.25, -32.5, 3, 0.25, 13.5);
    block(c, "Tunnel West", wall_m, -2.5, 5.0, -29, 0.25, 1.5, 3);
    block(c, "Tunnel East", wall_m, 2.5, 5.0, -29, 0.25, 1.5, 3);
    block(c, "Tunnel Lintel", lintel_m, 0, 5.15, -29, 2.75, 0.35, 3);
    k.goal = make(c, cat({R"({"name":"Goal","components":{"LocalTransform":{"position":[0,3.6,-44]},"Trigger3D":{"half":[3,2,1]},)",
                          R"("ShakeTrigger":{"trauma":0.4,"on":"TriggerEnter"},)", ref(goal_m), "}}"}));
    k.runner = make(c, cat({R"({"name":"Runner","components":{"LocalTransform":{"position":[0,1.45,0]},)", ref(runner_m), ",",
                            R"("CharacterController3D":{"use_input":true,"acceleration":40,"deceleration":60,"air_acceleration":12,)",
                            R"("coyote_time":0.15,"jump_buffer":0.12,"jump_cut":0.5,"jump_speed":6,"crouch_half_height":0.3},)",
                            R"("ShakeTrigger":{"trauma":0.2,"on":"Landed"}}})"}));
    k.camera = make(c, cat({R"({"name":"Camera","components":{"LocalTransform":{},"Camera3DComponent":{},"CameraFollow3D":{"target":"$)", num(k.runner),
                            R"(","offset":[0,0.2,0],"distance":6,"pitch":0.35,"collide":true,"look_ahead":0.2}}})"}));
    return k;
}

struct Race {
    int player = 0, fast = 0, slow = 0, camera = 0, first_point = 0;
};

// A rounded-rectangle track of nine waypoints on a flat ground with kerb walls, and three karts on the grid.
Race build_race(Client& c) {
    const std::string ground_m = box_mesh(c, "track_ground.glb", 45, 0.5, 50, "#55604c");
    const std::string kart_m = box_mesh(c, "kart.glb", 0.9, 0.4, 2.0, "#c04030");
    const std::string post_m = box_mesh(c, "marker.glb", 0.3, 0.3, 0.3, "#e0e0e0");
    c.ok("scene_new", R"({"name":"race"})");
    Race r;
    make(c, cat({R"({"name":"Ground","components":{"LocalTransform":{"position":[0,-0.5,0]},"BoxCollider3D":{"half":[45,0.5,50],"friction":1},)", ref(ground_m), "}}"}));
    // Waypoints, created last to first so each can name the one after it; the last is pointed back at the first below.
    const double pts[9][2] = {{0, 25}, {-15, 25}, {-25, 15}, {-25, -15}, {-15, -25}, {15, -25}, {25, -15}, {25, 15}, {15, 25}};
    int ids[9] = {};
    for (int i = 8; i >= 0; --i) {
        const std::string next = i == 8 ? R"(,"Waypoint3D":{"radius":5})" : cat({R"(,"Waypoint3D":{"next":"$)", num(ids[i + 1]), R"(","radius":5})"});
        ids[i] = make(c, cat({R"({"name":"Waypoint )", num(i), R"(","components":{"LocalTransform":{"position":[)", fnum(pts[i][0]), ",0.3,", fnum(pts[i][1]),
                              "]},", ref(post_m), next, "}}"}));
    }
    c.ok("component_set", cat({R"({"entity":)", num(ids[8]), R"(,"component":"Waypoint3D","path":"next","value":"$)", num(ids[0]), R"("})"}));
    r.first_point = ids[0];
    const std::string tune = R"("anti_roll":8000,"downforce":2.0,"skid_slip":0.2,"drive":"All","front_axle":-1.4,"rear_axle":1.4,"track":0.9,"wheel_y":-0.3)";
    const auto kart = [&](const char* name, double x, double z, bool ai, double top, const char* extra) {
        return make(c, cat({R"({"name":")", name, R"(","components":{"LocalTransform":{"position":[)", fnum(x), ",0.7,", fnum(z),
                            R"(],"rotation":[0,0.7071068,0,0.7071068]},"RigidBody3D":{"half":[0.9,0.4,2],"density":208},)", ref(kart_m), ",",
                            R"("Vehicle3D":{)", tune, ai ? "" : R"(,"use_input":1)", R"(},)",
                            ai ? cat({R"("VehicleDriver3D":{"path":"$)", num(ids[0]), R"(","max_speed":)", fnum(top), R"(,"laps":2},)"}) : std::string(),
                            R"("ShakeTrigger":{"trauma":0.05,"on":"Skid"})", extra, "}}"}));
    };
    r.fast = kart("Kart Fast", 14, 21, true, 20, "");
    r.slow = kart("Kart Slow", 14, 29, true, 15, "");
    r.player = kart("Cart", 24, 25, false, 0, "");
    r.camera = make(c, cat({R"({"name":"Camera","components":{"LocalTransform":{},"Camera3DComponent":{},"CameraFollow3D":{"target":"$)", num(r.player),
                            R"(","offset":[0,0.5,0],"distance":9,"pitch":0.45,"yaw":1.5707963,"look_ahead":0.3}}})"}));
    return r;
}


// Plays `scene` for `ticks` with `input` and inspects the named entities (in id order) with the movement components.
Client::Tool play(Client& c, const char* scene, u32 ticks, const std::string& input, const std::string& who) {
    return c.ok("run_headless", cat({R"({"scene":"scenes/)", scene, R"(.mcscene","ticks":)", num(static_cast<int>(ticks)), R"(,"log_lines":0,"input":")", input,
                                     R"(","inspect":{"entities":)", who,
                                     R"(,"show":["LocalTransform","CharacterController3D","CameraFollow3D","Trigger3D","Joint3D","ShakeTrigger","Vehicle3D","VehicleDriver3D"]}})"}));
}

void export_assets(const std::string& root) {
    if (const char* out = std::getenv("MCE_MOVEMENT_EXPORT")) {
        std::filesystem::copy(root + "/assets", std::string(out) + "/assets",
                              std::filesystem::copy_options::recursive | std::filesystem::copy_options::overwrite_existing);
        std::filesystem::copy_file(root + "/CREDITS.md", std::string(out) + "/CREDITS.md", std::filesystem::copy_options::overwrite_existing);
    }
}

// The script samples/08_movement/course.input plays: wait for the lift, run, jump the gap a few ticks after the deck
// ends (inside the 0.15 s of coyote time), crouch through the tunnel. Each line is cut to the run's length: run_headless warns about a script that reaches past it.
std::string course_script(u32 ticks, u32 jump_from = 424, bool crouch = true) {
    std::string out;
    const auto line = [&](u32 from, u32 to, const char* what) {
        if (from > ticks) return;
        if (!out.empty()) out += "\\n";
        out += num(static_cast<int>(from)) + "-" + num(static_cast<int>(std::min(to, ticks))) + " " + what;
    };
    line(220, 900, "Move=0,1");
    line(jump_from, jump_from + 21, "Jump");
    if (crouch) line(500, 690, "Crouch");
    return out;
}

}  // namespace

TEST_CASE("M-MOVE3D demo: the 08_movement course built over MCP alone") {
    const std::string root = fresh_dir("mcp_movement");
    std::filesystem::create_directories(root + "/assets");
    Client c;
    REQUIRE(c.server.open_project(sv(root)));
    c.init();
    build_course(c);
    c.ok("scene_save", R"({"path":"scenes/course.mcscene"})");
    REQUIRE(c.ok("cook_run").v().get("ok").as_bool());
    const std::string who = R"(["Runner","Lift","Camera","Goal"])";  // run_headless lists them by id: Lift, Goal, Runner, Camera
    constexpr u32 kLift = 0, kGoal = 1, kRunner = 2, kCamera = 3;
    const auto pos = [&](const Client::Tool& r, u32 i, u32 lane) { return at(r, i, "LocalTransform", "position", lane); };
    const auto cc = [&](const Client::Tool& r) { return comps(r, kRunner).get("CharacterController3D"); };

    // The lift carries the runner up three metres (1 m/s) and stops at its limit.
    auto lift = play(c, "course", 90, "", who);
    CHECK(comps(lift, kLift).get("Joint3D").get("position").as_number() == doctest::Approx(1.5).epsilon(0.1));
    CHECK(pos(lift, kRunner, 1) > 2.0);
    CHECK(cc(lift).get("grounded").as_bool());
    lift = play(c, "course", 240, "", who);
    CHECK(comps(lift, kLift).get("Joint3D").get("position").as_number() == doctest::Approx(3.0).epsilon(0.01));
    CHECK(pos(lift, kRunner, 1) == doctest::Approx(4.4).epsilon(0.01));  // on the 3.5 m deck: 0.9 m capsule centre above it

    // The whole run: gap, tunnel, goal.
        auto at430 = play(c, "course", 430, course_script(430), who);
    CHECK_FALSE(cc(at430).get("grounded").as_bool());  // in the air over the gap
    CHECK(pos(at430, kRunner, 1) > 4.4);
    CHECK(pos(at430, kRunner, 2) < -16.0);
    auto tunnel = play(c, "course", 630, course_script(630), who);
    CHECK(cc(tunnel).get("crouched").as_bool());
    CHECK(pos(tunnel, kRunner, 2) < -26.0);  // under the lintel
    const double arm = comps(tunnel, kCamera).get("CameraFollow3D").get("arm").as_number();
    CHECK(arm < 5.0);  // the spring arm pulled in against the lintel
    auto clear = play(c, "course", 800, course_script(800), who);
    CHECK_FALSE(cc(clear).get("crouched").as_bool());
    CHECK(comps(clear, kCamera).get("CameraFollow3D").get("arm").as_number() > 5.0);  // grown back
    auto done = play(c, "course", 900, course_script(900), who);
    CHECK(comps(done, kGoal).get("Trigger3D").get("enters").as_number() == 1);
    CHECK(comps(done, kGoal).get("ShakeTrigger").get("fired").as_number() == 1);
    CHECK(comps(done, kRunner).get("ShakeTrigger").get("fired").as_number() >= 2);  // Landed3D: the lift's deck and the far side of the gap
    CHECK(pos(done, kRunner, 2) < -44.0);
    CHECK(pos(done, kRunner, 1) > 3.5);
    CHECK(str(play(c, "course", 900, course_script(900), who).v().get("state_hash")) == str(done.v().get("state_hash")));  // replays match
    CHECK(done.v().get("log").get("errors").as_number() == 0);

    // Coyote time is what makes the late jump work: sixteen ticks later (past the 9 it allows) the runner falls into the gap.
    const auto late = play(c, "course", 560, course_script(560, 440), who);
    CHECK(pos(late, kRunner, 1) < 2.0);
    // And the tunnel is only passable crouched: without Crouch the lintel stops the runner at its mouth.
    const auto stuck = play(c, "course", 700, course_script(700, 424, false), who);
    CHECK(pos(stuck, kRunner, 2) > -26.0);
    CHECK(pos(stuck, kRunner, 2) < -22.0);

    export_assets(root);
}

TEST_CASE("M-MOVE3D demo: the 08_movement race built over MCP alone") {
    const std::string root = fresh_dir("mcp_movement_race");
    std::filesystem::create_directories(root + "/assets");
    Client c;
    REQUIRE(c.server.open_project(sv(root)));
    c.init();
    build_race(c);
    c.ok("scene_save", R"({"path":"scenes/race.mcscene"})");
    REQUIRE(c.ok("cook_run").v().get("ok").as_bool());
    const std::string who = R"(["Kart Fast","Kart Slow","Cart"])";
    const std::string script = "1-70 Move=0,1\\n71-120 Move=1,1\\n71-120 Jump";  // straight, then a handbrake turn
    const auto veh = [&](const Client::Tool& r, u32 i) { return comps(r, i).get("Vehicle3D"); };

    const auto mid = play(c, "race", 600, script, who);
    for (u32 i = 0; i < 3; ++i) CHECK(at(mid, i, "LocalTransform", "position", 1) < 1.5);  // all on the ground
    const auto race = play(c, "race", 2600, script, who);
    CHECK(race.v().get("log").get("errors").as_number() == 0);
    for (u32 i = 0; i < 2; ++i) {  // both AI karts finish their two laps and stop
        const cook::JsonValue d = comps(race, i).get("VehicleDriver3D");
        CHECK(d.get("laps_done").as_number() == 2);
        CHECK(d.get("finished").as_number() == 1);
        CHECK(d.get("best_lap").as_number() > 5.0);
        CHECK(comps(race, i).get("ShakeTrigger").get("fired").as_number() > 0);  // Skid3D reactions: corners skid
        CHECK(veh(race, i).get("rightings").as_number() == 0);                   // and neither flips
    }
    CHECK(comps(race, 2).get("ShakeTrigger").get("fired").as_number() > 0);  // the handbrake turn skidded the cart too
    CHECK(str(play(c, "race", 2600, script, who).v().get("state_hash")) == str(race.v().get("state_hash")));

    export_assets(root);
}
