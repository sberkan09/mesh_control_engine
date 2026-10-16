// M-GAMEPLAY3D demo (ROADMAP, Linear SBA-75): the samples/07_gameplay3d arena built over MCP alone in
// an empty project. Grunts made with gen_mesh's humanoid chase a drivable cart across a navmesh
// baked with navmesh_bake and keep apart; the cart pushes a door open on its hinge, a drawbridge
// lowers on its motor, and the cart's bumper sensor sends the grunts it hits into ragdolls.
// run_headless replays the scene to the same hash and its inspect reads the agents, the joints, the
// ragdolls and the vehicle.
//
// MCE_GAMEPLAY3D_EXPORT=<dir> copies the built project's assets/ there (how the sample was made:
// MCE_GAMEPLAY3D_EXPORT=samples/07_gameplay3d build/.../test_mcp_gameplay3d).

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

// The arena, entity by entity (ids in creation order). Returns the ids the checks need.
struct Arena {
    int door = 0, bridge = 0, cart = 0, bumper = 0, grunt[3] = {0, 0, 0}, surface = 0;
};

Arena build_arena(Client& c) {
    const auto mesh = [&](const char* args) { return str(c.ok("gen_mesh", args).v().get("saved").get("guid")); };
    const std::string floor_m = mesh(R"({"shape":"box","size":[60,1,60],"color":"#6b7a5e","save":"models/floor.glb"})");
    const std::string wall_m = mesh(R"({"shape":"box","size":[28,3,0.6],"color":"#8a8478","save":"models/wall.glb"})");
    const std::string door_m = mesh(R"({"shape":"box","size":[3.8,2.3,0.2],"color":"#8b5a2b","save":"models/door.glb"})");
    const std::string deck_m = mesh(R"({"shape":"box","size":[6,1,4],"color":"#7d7d85","save":"models/platform.glb"})");
    const std::string bridge_m = mesh(R"({"shape":"box","size":[3,4,0.2],"color":"#a0703c","save":"models/bridge.glb"})");
    const std::string cart_m = mesh(R"({"shape":"box","size":[1.8,0.8,3],"color":"#d0a020","save":"models/cart.glb"})");
    const std::string grunt_m = mesh(R"({"shape":"humanoid","color":"#b04030","save":"models/grunt.glb"})");

    c.ok("scene_new", R"({"name":"arena"})");
    const auto make = [&](const std::string& args) { return static_cast<int>(c.ok("entity_create", args).v().get("id").as_number()); };
    const auto ref = [](const std::string& guid) { return cat({R"("MeshRef":{"mesh":")", guid, R"("})"}); };
    Arena a;
    make(cat({R"({"name":"Floor","components":{"LocalTransform":{"position":[0,-0.5,0]},"BoxCollider3D":{"half":[30,0.5,30]},)", ref(floor_m), "}}"}));
    // A wall across z = 0 with a 4 m gap in the middle, and the door in the gap (hinged on its left edge).
    make(cat({R"({"name":"Wall West","components":{"LocalTransform":{"position":[-16,1.5,0]},"BoxCollider3D":{"half":[14,1.5,0.3]},)", ref(wall_m), "}}"}));
    make(cat({R"({"name":"Wall East","components":{"LocalTransform":{"position":[16,1.5,0]},"BoxCollider3D":{"half":[14,1.5,0.3]},)", ref(wall_m), "}}"}));
    a.door = make(cat({R"({"name":"Door","components":{"LocalTransform":{"position":[0,1.25,0]},)",
                       R"("RigidBody3D":{"half":[1.9,1.15,0.1],"density":200},)",
                       R"("Joint3D":{"kind":"Hinge","anchor":[-1.9,0,0],"axis":[0,1,0],"limits":1,"limit_min":-110,"limit_max":110,"friction":40},)",
                       ref(door_m), "}}"}));
    // A platform in the east yard and its drawbridge, built raised; the motor lowers it into a ramp.
    make(cat({R"({"name":"Platform","components":{"LocalTransform":{"position":[12,0.5,12]},"BoxCollider3D":{"half":[3,0.5,2]},)", ref(deck_m), "}}"}));
    a.bridge = make(cat({R"({"name":"Drawbridge","components":{"LocalTransform":{"position":[12,3.05,9.85]},)",
                         R"("RigidBody3D":{"half":[1.5,2,0.1],"density":500},)",
                         R"("Joint3D":{"kind":"Hinge","anchor":[0,-2,0],"axis":[1,0,0],"limits":1,"limit_min":-110,"limit_max":0,)",
                         R"("motor":"Position","motor_target":-104,"motor_frequency":0.5},)",
                         ref(bridge_m), "}}"}));
    // The cart (the player drives it: Move throttles and steers) and its bumper sensor, a child.
    a.cart = make(cat({R"({"name":"Cart","components":{"LocalTransform":{"position":[0,1,12]},"RigidBody3D":{"half":[0.9,0.4,1.5],"density":300},)",
                       R"("Vehicle3D":{"use_input":1,"engine_torque":900},)", ref(cart_m), "}}"}));
    a.bumper = make(cat({R"({"name":"Bumper","components":{"LocalTransform":{"position":[0,0,-1.8]},"ChildOf":{"parent":"$)", num(a.cart),
                         R"("},"Overlap3D":{"shape":"Box","half":[1.1,0.6,0.4],"dynamic":0}}})"}));
    // Three grunts across the wall: walking capsules (layer 1) steered by agents that chase the
    // cart; their ragdolls (layer 2) skip the capsules and go limp when the bumper touches them.
    const float xs[3] = {-5, 0, 5};
    for (int i = 0; i < 3; ++i)
        a.grunt[i] = make(cat({R"({"name":"Grunt )", num(i), R"(","components":{"LocalTransform":{"position":[)", std::to_string(xs[i]),
                               R"(,0.9,-12]},)", ref(grunt_m), R"(,"Animator":{"clip":"Walk"},)",
                               R"("CharacterController3D":{"layer":1,"mask":65531},)",
                               R"("NavAgent3D":{"follow":"$)", num(a.cart), R"(","move_to":1,"speed":3},)",
                               R"("Ragdoll3D":{"on_trigger":1,"layer":2,"mask":65533}}})"}));
    make(cat({R"({"name":"Camera","components":{"LocalTransform":{},"Camera3DComponent":{},"CameraFollow3D":{"target":"$)", num(a.cart),
              R"(","distance":10,"pitch":0.5}}})"}));
    a.surface = make(R"({"name":"Navmesh","components":{"LocalTransform":{},"NavMeshSurface3D":{}}})");
    return a;
}

cook::JsonValue comps(const Client::Tool& run, u32 i) { return run.v().get("inspect").get("entities").at(i).get("components"); }

}  // namespace

TEST_CASE("M-GAMEPLAY3D demo: the 07_gameplay3d arena built over MCP alone") {
    const std::string root = fresh_dir("mcp_gameplay3d");
    std::filesystem::create_directories(root + "/assets");
    Client c;
    REQUIRE(c.server.open_project(sv(root)));
    c.init();
    const Arena a = build_arena(c);

    // navmesh_bake writes the .mcnav (named after the saved scene) and points the surface at it; a
    // re-bake is byte-identical.
    c.ok("scene_save", R"({"path":"scenes/arena.mcscene"})");
    auto bake = c.ok("navmesh_bake");
    CHECK(str(bake.v().get("path")) == "assets/navmesh/arena/nav_" + num(a.surface) + ".mcnav");
    CHECK(bake.v().get("bytes").as_number() > 1000);
    CHECK(bake.v().get("triangles").as_number() > 0);
    const std::string first = str(bake.v().get("content_hash"));
    CHECK(str(c.ok("navmesh_bake").v().get("content_hash")) == first);
    c.ok("scene_save");
    REQUIRE(c.ok("cook_run").v().get("ok").as_bool());

    // A second of play with no input: the grunts set off towards the cart, the drawbridge lowers.
    const std::string who = cat({R"(["Door","Drawbridge","Cart","Bumper","Grunt 0","Grunt 1","Grunt 2"])"});
    auto start = c.ok("run_headless", cat({R"({"ticks":60,"log_lines":0,"inspect":{"entities":)", who,
                                           R"(,"show":["LocalTransform","NavAgent3D","Joint3D","Vehicle3D","Ragdoll3D","Overlap3D"]}})"}));
    for (u32 g = 4; g < 7; ++g) {
        const cook::JsonValue nav = comps(start, g).get("NavAgent3D");
        CHECK(str(nav.get("state")) == "Moving");
        CHECK(nav.get("target").at(2).as_number() == doctest::Approx(12.0).epsilon(0.05));  // the cart's position
        CHECK(comps(start, g).get("LocalTransform").get("position").at(2).as_number() > -11.9);
    }
    // Avoidance: the three keep apart while they walk (agent radius 0.4).
    for (u32 g = 4; g < 7; ++g)
        for (u32 h = g + 1; h < 7; ++h) {
            const cook::JsonValue p = comps(start, g).get("LocalTransform").get("position"), q = comps(start, h).get("LocalTransform").get("position");
            const double dx = p.at(0).as_number() - q.at(0).as_number(), dz = p.at(2).as_number() - q.at(2).as_number();
            CHECK(std::sqrt(dx * dx + dz * dz) > 0.8);
        }
    CHECK(comps(start, 1).get("Joint3D").get("position").as_number() < -5.0);  // lowering
    CHECK(start.v().get("log").get("errors").as_number() == 0);

    // Five seconds driving straight ahead: through the door, into the grunts.
    const std::string drive = cat({R"({"ticks":300,"log_lines":0,"input":"1-300 Move=0,1","inspect":{"entities":)", who,
                                   R"(,"show":["LocalTransform","NavAgent3D","Joint3D","Vehicle3D","Ragdoll3D","Overlap3D"]}})"});
    auto run = c.ok("run_headless", drive);
    CHECK(str(c.ok("run_headless", drive).v().get("state_hash")) == str(run.v().get("state_hash")));  // replays match
    CHECK(run.v().get("log").get("errors").as_number() == 0);
    const cook::JsonValue door = comps(run, 0).get("Joint3D");
    CHECK(std::fabs(door.get("position").as_number()) > 20.0);  // pushed open
    CHECK(comps(run, 1).get("Joint3D").get("position").as_number() == doctest::Approx(-104.0).epsilon(0.08));  // lowered
    const cook::JsonValue cart = comps(run, 2);
    CHECK(cart.get("LocalTransform").get("position").at(2).as_number() < 0.0);  // through the wall
    CHECK(cart.get("Vehicle3D").get("wheels_on_ground").as_number() >= 3);
    CHECK(comps(run, 3).get("Overlap3D").get("enters").as_number() >= 1);
    u32 limp = 0;
    for (u32 g = 4; g < 7; ++g) {
        const cook::JsonValue rag = comps(run, g).get("Ragdoll3D");
        if (rag.get("active").as_number() == 1) {
            ++limp;
            CHECK(rag.get("parts").as_number() >= 11);
            CHECK(comps(run, g).get("NavAgent3D").get("move_to").as_number() == 0);
        }
    }
    CHECK(limp >= 1);
    MESSAGE("limp grunts: ", limp, ", cart z ", cart.get("LocalTransform").get("position").at(2).as_number(), ", door ",
            door.get("position").as_number());

    if (const char* out = std::getenv("MCE_GAMEPLAY3D_EXPORT")) {
        std::filesystem::copy(root + "/assets", std::string(out) + "/assets",
                              std::filesystem::copy_options::recursive | std::filesystem::copy_options::overwrite_existing);
        std::filesystem::copy_file(root + "/CREDITS.md", std::string(out) + "/CREDITS.md", std::filesystem::copy_options::overwrite_existing);
    }
}
