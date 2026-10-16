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
    const std::string floor_m = mesh(R"({"shape":"box","size":[240,1,240],"color":"#667a5c","save":"models/floor.glb"})");
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
    box("Floor", "[30,-0.5,-20]", "[120,0.5,120]", floor_m);
    // Hurdle: 0.8 m is above the 0.35 m step height, so the hero has to jump it.
    box("Hurdle", "[0,0.4,10]", "[3,0.4,0.3]", hurdle_m);
    // Low lintel on two pillars: 1.5 m of headroom, so the hero has to crouch (standing it is 1.8 m).
    box("Lintel", "[0,1.75,0]", "[3,0.25,2]", lintel_m);
    box("Pillar West", "[-3.3,0.75,0]", "[0.3,0.75,2]", pillar_m);
    box("Pillar East", "[3.3,0.75,0]", "[0.3,0.75,2]", pillar_m);
    // The lift is a kinematic platform a looping timeline moves: it waits on the floor, rises to the
    // ledge's height, waits there, and comes back down.
    const std::string rise = str(c.ok("timeline_create", R"({"path":"anim/lift.mctimeline","name":"lift","tracks":[
        {"type":"transform","keys":[{"t":0,"position":[0,0.2,-12]},{"t":8,"position":[0,0.2,-12]},{"t":12,"position":[0,3.3,-12],"ease":"smooth"},
                                    {"t":20,"position":[0,3.3,-12]},{"t":24,"position":[0,0.2,-12],"ease":"smooth"}]}]})").v().get("guid"));
    k.lift = make(cat({R"({"name":"Lift","components":{"LocalTransform":{"position":[0,0.2,-12]},)",
                       R"("RigidBody3D":{"type":"Kinematic","half":[1.5,0.2,1.5]},)",
                       R"("TimelinePlayer":{"timeline":")", rise, R"(","loop":true},)", ref(lift_m), "}}"}));
    box("Ledge", "[0,1.75,-17]", "[3,1.75,3]", ledge_m);
    // The hero: input-driven, with the feel settings on, and a graph that follows its motion.
    k.hero = make(cat({R"({"name":"Hero","components":{"LocalTransform":{"position":[0,0.9,20]},)", ref(hero_m), ",",
                       R"("Animator":{"graph":")", graph, R"("},)",
                       R"("CharacterController3D":{"use_input":true,"layer":1,"speed":5,"jump_speed":6.5,"acceleration":40,"deceleration":50,)",
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
    build_course(c);
    c.ok("scene_save", R"({"path":"scenes/course.mcscene"})");
    REQUIRE(c.ok("cook_run").v().get("ok").as_bool());

    // inspect answers in scene order: the lift, the hero, the karts, the camera.
    enum Row : u32 { kLift = 0, kHero = 1, kKart0 = 2, kCamera = 5 };
    const std::string who = R"(["Hero","Lift","Camera","Kart 0","Kart 1","Kart 2"])";
    const auto play = [&](u32 ticks, const std::string& input) {
        return c.ok("run_headless", cat({R"({"ticks":)", num(static_cast<int>(ticks)), R"(,"log_lines":0,"input":")", input,
                                         R"(","inspect":{"entities":)", who,
                                         R"(,"show":["LocalTransform","CharacterController3D","Locomotion3D","Animator","VehicleDriver3D","Vehicle3D","CameraFollow3D"]}})"}));
    };
    // The walk: forward all the way, a jump at the hurdle, a crouch for the tunnel.
    const auto walk = [](u32 ticks, bool jump = true, bool crouch = true) {
        std::string in = cat({"1-", num(static_cast<int>(ticks)), " Move=0,1"});
        if (jump && ticks >= 100) in += "\\n95-100 Jump";
        if (crouch && ticks >= 170) in += cat({"\\n170-", num(static_cast<int>(ticks < 400 ? ticks : 400)), " Crouch"});
        return in;
    };
    const auto hero = [&](const Client::Tool& r) { return comps(r, kHero); };
    const auto at = [&](const Client::Tool& r, u32 axis) { return hero(r).get("LocalTransform").get("position").at(axis).as_number(); };

    // The first tick: it eases in (40 m/s^2 is 0.67 m/s a tick) and the graph is still on Idle; a second
    // in, the Walk clip plays at the speed Locomotion3D measured.
    auto first = play(3, walk(3));
    CHECK(std::fabs(hero(first).get("CharacterController3D").get("velocity").at(2).as_number()) < 3.0);
    auto walking = play(60, walk(60));
    CHECK(str(hero(walking).get("Animator").get("state")) == "walk");
    CHECK(hero(walking).get("Locomotion3D").get("ground_speed").as_number() == doctest::Approx(5.0).epsilon(0.05));
    CHECK(hero(walking).get("Locomotion3D").get("grounded").as_number() == 1);
    // The camera's spring arm is out: this far from walls it is the whole 6 m.
    CHECK(comps(walking, kCamera).get("CameraFollow3D").get("arm").as_number() > 5.0);

    // The hurdle is above the step height: only a jump gets over it.
    auto no_jump = play(160, walk(160, false, false));
    CHECK(at(no_jump, 2) > 10.0);  // held up in front of it
    auto up = play(120, walk(120));
    CHECK(at(up, 1) > 1.5);        // in the air
    auto over = play(180, walk(180));
    CHECK(at(over, 2) < 9.0);      // landed beyond it
    CHECK(hero(over).get("CharacterController3D").get("grounded").as_bool());

    // The lintel leaves 1.5 m: crouched, the hero fits under it; the same walk standing stops short.
    auto under = play(300, walk(300));
    CHECK(hero(under).get("CharacterController3D").get("crouched").as_bool());
    CHECK(std::fabs(at(under, 2)) < 2.0);
    CHECK(at(under, 0) == doctest::Approx(0.0).epsilon(0.1));
    auto stood = play(450, walk(450, true, false));
    CHECK(at(stood, 2) > 2.0);     // stopped at the lintel's edge
    auto through = play(450, walk(450));
    CHECK(at(through, 2) < -5.0);
    CHECK_FALSE(hero(through).get("CharacterController3D").get("crouched").as_bool());

    // The lift waits on the floor, carries the hero up, and the hero walks onto the ledge.
    auto boarded = play(600, walk(600));
    CHECK(comps(boarded, kLift).get("LocalTransform").get("position").at(1).as_number() > 1.0);
    CHECK(at(boarded, 1) > 2.0);
    auto ledge = play(750, walk(750));
    CHECK(at(ledge, 1) > 4.3);     // standing on the ledge (3.5 m + the capsule's 0.9)
    CHECK(at(ledge, 2) < -16.0);

    // A race of two laps: the three karts each drive it to the end on their own, and the replay matches.
    const std::string race = cat({R"({"ticks":4800,"log_lines":0,"input":")", walk(4800), R"(","inspect":{"entities":)", who,
                                  R"(,"show":["LocalTransform","VehicleDriver3D","Vehicle3D"]}})"});
    auto run = c.ok("run_headless", race);
    CHECK(str(c.ok("run_headless", race).v().get("state_hash")) == str(run.v().get("state_hash")));
    CHECK(run.v().get("log").get("errors").as_number() == 0);
    f32 best[3] = {};
    for (u32 i = 0; i < 3; ++i) {
        const cook::JsonValue d = comps(run, kKart0 + i).get("VehicleDriver3D");
        CHECK(d.get("laps_done").as_number() == 2);
        CHECK(d.get("finished").as_number() == 1);
        best[i] = static_cast<f32>(d.get("best_lap").as_number());
        CHECK(best[i] > 15.0f);
        CHECK(best[i] < 45.0f);
        CHECK(comps(run, kKart0 + i).get("Vehicle3D").get("wheels_on_ground").as_number() >= 3);
    }
    if (const char* out = std::getenv("MCE_MOVE3D_EXPORT")) {
        std::filesystem::copy(root + "/assets", std::string(out) + "/assets",
                              std::filesystem::copy_options::recursive | std::filesystem::copy_options::overwrite_existing);
        std::filesystem::copy_file(root + "/CREDITS.md", std::string(out) + "/CREDITS.md", std::filesystem::copy_options::overwrite_existing);
    }
}
