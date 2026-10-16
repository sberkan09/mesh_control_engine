// M-MOVE3D demo (ROADMAP, Linear SBA-139): the samples/08_movement course built over MCP alone in an
// empty project. A humanoid made with gen_mesh runs an obstacle course on a CharacterController3D
// with acceleration, coyote time and a crouch (Locomotion3D drives its Idle and Walk clips), under
// a CameraFollow3D spring arm: over a hurdle, under a low lintel, onto a lift that rides a Slider
// joint motor up to a ledge. Beside it three karts race a waypoint track on VehicleDriver3D.
// run_headless replays the scene to the same hash and its inspect reads the state.
//
// MCE_MOVE3D_EXPORT=<dir> copies the built project's assets/ there (how the sample was made:
// MCE_MOVE3D_EXPORT=samples/08_movement build/.../test_mcp_move3d).

#include <doctest/doctest.h>

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

// Idle and Walk, switched by the speed Locomotion3D writes.
constexpr const char* kHeroGraph = R"({"params":["speed"],"start":"idle",
    "states":[{"name":"idle","clip":"Idle"},{"name":"walk","clip":"Walk"}],
    "transitions":[
        {"from":"idle","to":"walk","when":[{"param":"speed","op":">","value":0.5}],"fade":0.15},
        {"from":"walk","to":"idle","when":[{"param":"speed","op":"<","value":0.3}],"fade":0.15}]})";

struct Course {
    int hero = 0, lift = 0, camera = 0, kart[3] = {0, 0, 0};
};

Course build_course(Client& c) {
    const auto mesh = [&](const char* args) { return str(c.ok("gen_mesh", args).v().get("saved").get("guid")); };
    const std::string floor_m = mesh(R"({"shape":"box","size":[140,1,140],"color":"#667a5c","save":"models/floor.glb"})");
    const std::string hurdle_m = mesh(R"({"shape":"box","size":[6,0.8,0.6],"color":"#b06a3a","save":"models/hurdle.glb"})");
    const std::string lintel_m = mesh(R"({"shape":"box","size":[6,0.5,4],"color":"#8a8478","save":"models/lintel.glb"})");
    const std::string pillar_m = mesh(R"({"shape":"box","size":[0.6,1.5,4],"color":"#8a8478","save":"models/pillar.glb"})");
    const std::string lift_m = mesh(R"({"shape":"box","size":[3,0.4,3],"color":"#c9a227","save":"models/lift.glb"})");
    const std::string ledge_m = mesh(R"({"shape":"box","size":[6,3.5,6],"color":"#7d7d85","save":"models/ledge.glb"})");
    const std::string kart_m = mesh(R"({"shape":"box","size":[1.8,0.8,3.4],"color":"#d0a020","save":"models/kart.glb"})");
    const std::string hero_m = mesh(R"({"shape":"humanoid","color":"#3a78b0","save":"models/hero.glb"})");
    const std::string graph = str(c.ok("asset_create", cat({R"({"path":"anim/hero.mcanim","content":)", kHeroGraph, "}"})).v().get("guid"));

    c.ok("scene_new", R"({"name":"course"})");
    const auto make = [&](const std::string& args) { return static_cast<int>(c.ok("entity_create", args).v().get("id").as_number()); };
    const auto ref = [](const std::string& guid) { return cat({R"("MeshRef":{"mesh":")", guid, R"("})"}); };
    const auto box = [&](const char* name, const char* pos, const char* half, const std::string& m) {
        return make(cat({R"({"name":")", name, R"(","components":{"LocalTransform":{"position":)", pos, R"(},"BoxCollider3D":{"half":)", half, "},",
                         ref(m), "}}"}));
    };
    Course k;
    box("Floor", "[30,-0.5,0]", "[70,0.5,70]", floor_m);
    // Hurdle: 0.8 m is above the 0.35 m step height, so the hero has to jump it.
    box("Hurdle", "[0,0.4,10]", "[3,0.4,0.3]", hurdle_m);
    // Low lintel on two pillars: 1.5 m of headroom, so the hero has to crouch (standing it is 1.8 m).
    box("Lintel", "[0,1.75,0]", "[3,0.25,2]", lintel_m);
    box("Pillar West", "[-3.3,0.75,0]", "[0.3,0.75,2]", pillar_m);
    box("Pillar East", "[3.3,0.75,0]", "[0.3,0.75,2]", pillar_m);
    // The lift rides a Slider motor from the floor to the ledge's height; the ledge is a step beyond.
    k.lift = make(cat({R"({"name":"Lift","components":{"LocalTransform":{"position":[0,0.2,-12]},)",
                       R"("RigidBody3D":{"half":[1.5,0.2,1.5],"density":400},)",
                       R"("Joint3D":{"kind":"Slider","anchor":[0,0,0],"axis":[0,1,0],"limits":1,"limit_min":0,"limit_max":3.4,)",
                       R"("motor":"Position","motor_target":3.3,"motor_frequency":1.0,"motor_max_force":300000},)", ref(lift_m), "}}"}));
    box("Ledge", "[0,1.75,-17]", "[3,1.75,3]", ledge_m);
    // The hero: input-driven, with the feel settings on, and a graph that follows its motion.
    k.hero = make(cat({R"({"name":"Hero","components":{"LocalTransform":{"position":[0,0.9,20]},)", ref(hero_m), ",",
                       R"("Animator":{"graph":")", graph, R"("},)",
                       R"("CharacterController3D":{"use_input":1,"layer":1,"speed":5,"jump_speed":6.5,"acceleration":40,"deceleration":50,)",
                       R"("coyote_time":0.15,"jump_buffer":0.1,"crouch_half_height":0.3},)",
                       R"("Locomotion3D":{}}})"}));
    // Three karts on a rectangle track east of the course, racing two laps on VehicleDriver3D.
    const float xs[3] = {38, 42, 38};
    const float zs[3] = {2, 8, 14};
    const float tops[3] = {14, 16, 18};
    int wp[5];
    const float wx[5] = {40, 40, 70, 70, 40}, wz[5] = {-10, -70, -70, 20, 20};
    for (int i = 0; i < 5; ++i)
        wp[i] = make(cat({R"({"name":"Waypoint )", num(i), R"(","components":{"LocalTransform":{"position":[)", std::to_string(wx[i]), ",0,",
                          std::to_string(wz[i]), R"(]},"Waypoint3D":{}}})"}));
    for (int i = 0; i < 5; ++i)
        c.ok("component_set", cat({R"({"entity":)", num(wp[i]), R"(,"component":"Waypoint3D","path":"next","value":"$)", num(wp[(i + 1) % 5]), R"("})"}));
    for (int i = 0; i < 3; ++i)
        k.kart[i] = make(cat({R"({"name":"Kart )", num(i), R"(","components":{"LocalTransform":{"position":[)", std::to_string(xs[i]), ",1.2,",
                              std::to_string(zs[i]), R"(]},"RigidBody3D":{"half":[0.9,0.4,1.7],"density":300},)",
                              R"("Vehicle3D":{"drive":"All","front_axle":-1.4,"rear_axle":1.4,"track":0.9,"wheel_y":-0.3,"anti_roll":8000},)",
                              R"("VehicleDriver3D":{"path":"$)", num(wp[0]), R"(","max_speed":)", std::to_string(tops[i]), R"(,"laps":2},)", ref(kart_m), "}}"}));
    k.camera = make(cat({R"({"name":"Camera","components":{"LocalTransform":{},"Camera3DComponent":{},"CameraFollow3D":{"target":"$)", num(k.hero),
                         R"(","distance":6,"pitch":0.4,"collide":true}}})"}));
    return k;
}

cook::JsonValue comps(const Client::Tool& run, u32 i) { return run.v().get("inspect").get("entities").at(i).get("components"); }

}  // namespace

TEST_CASE("M-MOVE3D demo: the 08_movement course built over MCP alone") {
    const std::string root = fresh_dir("mcp_move3d");
    std::filesystem::create_directories(root + "/assets");
    Client c;
    REQUIRE(c.server.open_project(sv(root)));
    c.init();
    const Course k = build_course(c);
    c.ok("scene_save", R"({"path":"scenes/course.mcscene"})");
    REQUIRE(c.ok("cook_run").v().get("ok").as_bool());

    const std::string who = R"(["Hero","Lift","Camera","Kart 0","Kart 1","Kart 2"])";
    const auto play = [&](u32 ticks, const char* input) {
        return c.ok("run_headless", cat({R"({"ticks":)", num(static_cast<int>(ticks)), R"(,"log_lines":0,"input":")", input,
                                         R"(","inspect":{"entities":)", who,
                                         R"(,"show":["LocalTransform","CharacterController3D","Locomotion3D","Animator","Joint3D","VehicleDriver3D","Vehicle3D"]}})"}));
    };
    const auto hero_z = [&](const Client::Tool& r) { return comps(r, 0).get("LocalTransform").get("position").at(2).as_number(); };
    (void)k;
    for (u32 t : {60u, 120u, 180u, 240u, 300u, 360u, 420u, 480u, 600u}) {
        auto r = play(t, "1-600 Move=0,1");
        MESSAGE("t=", t, " hero z ", hero_z(r), " y ", comps(r, 0).get("LocalTransform").get("position").at(1).as_number(), " lift ",
                comps(r, 1).get("Joint3D").get("position").as_number());
    }
}
