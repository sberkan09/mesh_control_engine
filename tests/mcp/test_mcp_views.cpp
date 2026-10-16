// view_list, view_add, player_list and player_setup (M-VIEWS step 6) in process against a copy of the
// scene_demo project: the rig the tools build reads back clean, the problems they find are named, and
// render_frame's `view` argument rejects what it cannot resolve.

#include <doctest/doctest.h>

#include <filesystem>
#include <string>

#include "mcp_client.h"

using namespace mce;
using namespace mcp_client;
using namespace cook_test;

namespace {

bool mentions(cook::JsonValue problems, const char* needle) {
    for (u32 i = 0; i < problems.size(); ++i)
        if (str(problems.at(i)).find(needle) != std::string::npos) return true;
    return false;
}

std::string views_project(const char* name) {
    const std::string root = fresh_dir(name);
    std::filesystem::copy(std::string(MCE_SOURCE_DIR) + "/samples/scene_demo/assets", root + "/assets",
                          std::filesystem::copy_options::recursive);
    return root;
}

}  // namespace

TEST_CASE("view_add and view_list: a minimap texture, its camera and the monitor that shows it") {
    const std::string root = views_project("mcp_view_add");
    Client c;
    REQUIRE(c.server.open_project(sv(root), "scenes/demo.mcscene"));
    c.init();

    auto empty = c.ok("view_list");
    CHECK(empty.v().get("views").size() == 0);
    CHECK(empty.v().get("textures").size() == 0);
    CHECK(empty.v().get("split_screen").is_null());

    c.ok("entity_create", R"({"name":"Monitor","components":{"Sprite":{}}})");
    // A texture alone is a problem: nothing draws into it and nothing shows it.
    auto added = c.ok("view_add", R"({"name":"Minimap","width":128,"height":96,"update_hz":10,"fit":[20,15],"show_on":"Monitor"})");
    CHECK(str(added.v().get("texture").get("name")) == "Minimap");
    CHECK(str(added.v().get("camera").get("name")) == "Minimap camera");

    auto list = c.ok("view_list");
    REQUIRE(list.v().get("views").size() == 1);
    const cook::JsonValue view = list.v().get("views").at(0);
    CHECK(str(view.get("kind")) == "camera2d");
    CHECK(str(view.get("target").get("name")) == "Minimap");
    REQUIRE(list.v().get("textures").size() == 1);
    const cook::JsonValue tex = list.v().get("textures").at(0);
    CHECK(tex.get("width").as_number() == 128);
    CHECK(tex.get("update_hz").as_number() == 10);
    CHECK(tex.get("cameras").size() == 1);
    CHECK(str(tex.get("shown_on").at(0)) == "Monitor");
    CHECK(list.v().get("problems").size() == 0);

    // A second one with the same name is renamed, not refused; a 3D camera is allowed.
    auto second = c.ok("view_add", R"({"name":"Minimap","camera":"3d","position":[0,5,5],"look_at":[0,0,0]})");
    CHECK(str(second.v().get("texture").get("name")) == "Minimap 2");
    auto again = c.ok("view_list");
    CHECK(again.v().get("textures").size() == 2);
    CHECK(mentions(again.v().get("problems"), "is shown nowhere"));

    CHECK(c.fails("view_add", R"({"name":"X","camera":"4d"})").find("camera") != std::string::npos);
    CHECK(c.fails("view_add", R"({"name":"X","width":0})").find("'width'") != std::string::npos);
    CHECK(c.fails("view_add", R"({"name":"X","show_on":"Nobody"})").find("no entity named") != std::string::npos);
    CHECK(c.fails("view_add", R"({"name":"X","show_on":"Minimap"})").find("Sprite or a MeshRef") != std::string::npos);
}

TEST_CASE("player_setup and player_list: a co-op rig reads back, missing input maps are named") {
    const std::string root = views_project("mcp_player_setup");
    Client c;
    REQUIRE(c.server.open_project(sv(root), "scenes/demo.mcscene"));
    c.init();

    auto before = c.ok("player_list");
    CHECK(before.v().get("coop_game").is_null());
    CHECK(mentions(before.v().get("problems"), "no CoopGame"));

    auto made = c.ok("player_setup", R"({"players":3,"layout":"columns"})");
    CHECK(made.v().get("players").as_number() == 3);
    CHECK(made.v().get("entities").size() == 1 + 1 + 3 * 2);  // game, split screen, a spawn and a camera each
    CHECK(c.fails("player_setup", R"({"players":2})").find("already has a CoopGame") != std::string::npos);

    auto views = c.ok("view_list");
    CHECK(views.v().get("views").size() == 3);
    CHECK(str(views.v().get("split_screen").get("layout")) == "columns");
    CHECK(views.v().get("views").at(1).get("split").as_number() == 2);
    CHECK(views.v().get("views").at(1).get("player_camera").as_number() == 2);
    CHECK_FALSE(views.v().get("views").at(1).get("enabled").as_bool());

    auto players = c.ok("player_list");
    CHECK(players.v().get("coop_game").get("max_players").as_number() == 3);
    CHECK(players.v().get("players").size() == 4);
    CHECK(players.v().get("players").at(1).get("spawns").size() == 1);
    CHECK(players.v().get("players").at(1).get("cameras").size() == 1);
    CHECK(mentions(players.v().get("problems"), "no player prefab"));
    CHECK(mentions(players.v().get("problems"), "player 2 has no input map"));
    CHECK_FALSE(mentions(players.v().get("problems"), "no PlayerSpawn"));

    // An input map with "player": 2 satisfies player 2.
    c.ok("asset_create", R"({"path":"input/coop.mcinput","content":{"maps":[
      {"name":"P2","player":2,"actions":[{"name":"Move","type":"axis2d","bindings":[{"key":"Left","scale":-1}]},{"name":"Join","bindings":[{"key":"Return"}]}]}]}})");
    auto fixed = c.ok("player_list");
    CHECK(fixed.v().get("players").at(1).get("input_maps").size() == 1);
    CHECK(fixed.v().get("players").at(1).get("input_maps").at(0).get("join").as_bool());
    CHECK_FALSE(mentions(fixed.v().get("problems"), "player 2 has no input map"));
    CHECK(mentions(fixed.v().get("problems"), "player 3 has no input map"));

    CHECK(c.fails("player_setup", R"({"players":5})").find("'players'") != std::string::npos);
}

TEST_CASE("render_frame view: bad players and textures are errors before anything renders") {
    const std::string root = views_project("mcp_render_view");
    Client c;
    REQUIRE(c.server.open_project(sv(root), "scenes/demo.mcscene"));
    c.init();
    c.ok("scene_save", R"({"path":"scenes/demo.mcscene"})");

    CHECK(c.fails("render_frame", R"({"view":{"player":2}})").find("no camera follows that player") != std::string::npos);
    CHECK(c.fails("render_frame", R"({"view":{"player":9}})").find("view.player") != std::string::npos);
    CHECK(c.fails("render_frame", R"({"view":{}})").find("'view'") != std::string::npos);
    CHECK(c.fails("render_frame", R"({"view":{"player":1,"texture":"X"}})").find("'view'") != std::string::npos);
    CHECK(c.fails("render_frame", R"({"view":{"player":1},"camera":{"index":0}})").find("either") != std::string::npos);
    CHECK(c.fails("render_frame", R"({"view":{"texture":"Nobody"}})").find("no entity named") != std::string::npos);

    c.ok("view_add", R"({"name":"Mirror"})");
    c.ok("scene_save", R"({"path":"scenes/demo.mcscene"})");
    // The texture exists and has a camera, so these resolve (rendering itself is the GPU/software tests' job).
    CHECK(c.fails("render_frame", R"({"view":{"texture":"Nobody"}})").find("no entity named") != std::string::npos);
}
