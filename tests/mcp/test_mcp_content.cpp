// The content tools (ROADMAP M-AGENT step 5) in process against a copy of the scene_demo project:
// the generators' previews and saves (sidecar, CREDITS.md row, a cook with no adopt step) and the
// content library served from the source tree's library/library.json.

#include <doctest/doctest.h>

#include <filesystem>
#include <string>

#include "mcp_client.h"

using namespace mce;
using namespace mcp_client;
using namespace cook_test;

namespace {

const std::string kDemoAssets = std::string(MCE_SOURCE_DIR) + "/samples/scene_demo/assets";

std::string demo_project(const char* name) {
    const std::string root = fresh_dir(name);
    std::filesystem::copy(kDemoAssets, root + "/assets", std::filesystem::copy_options::recursive);
    return root;
}

std::string colors_of(const Client::Tool& t) {
    std::string s;
    const cook::JsonValue colors = t.v().get("colors");
    for (u32 i = 0; i < colors.size(); ++i) s += str(colors.at(i)) + " ";
    return s;
}

bool listed(Client& c, const std::string& path) {
    auto list = c.ok("asset_list");
    const cook::JsonValue assets = list.v().get("assets");
    for (u32 i = 0; i < assets.size(); ++i)
        if (str(assets.at(i).get("path")) == path) return true;
    return false;
}

}  // namespace

TEST_CASE("gen_palette, gen_sprite, gen_tileset, gen_texture: seeded, previewed, arguments checked") {
    Client c;
    c.init();
    // The schemas are valid JSON objects (a raw-string slip breaks the whole tools/list).
    auto list = c.request("tools/list");
    const cook::JsonValue tools = list->root().get("result").get("tools");
    REQUIRE(tools.is_array());
    for (u32 i = 0; i < tools.size(); ++i) CHECK_MESSAGE(tools.at(i).get("inputSchema").is_object(), str(tools.at(i).get("name")));
    auto a = c.ok("gen_palette", R"({"seed":4,"count":5,"scheme":"earth"})");
    CHECK(a.v().get("colors").size() == 5);
    CHECK(colors_of(a) == colors_of(c.ok("gen_palette", R"({"seed":4,"count":5,"scheme":"earth"})")));
    CHECK(colors_of(a) != colors_of(c.ok("gen_palette", R"({"seed":5,"count":5,"scheme":"earth"})")));
    REQUIRE(a.images.size() == 1);
    CHECK(a.images[0].width == 5 * 48);

    auto s = c.ok("gen_sprite", R"({"seed":9,"kind":"character","width":12,"height":16,"variants":3,"colors":["#202030","#806040","#e0c080"]})");
    CHECK(s.v().get("frames").size() == 3);
    CHECK(s.v().get("frames").at(2).get("x").as_number() == 24);
    REQUIRE(s.images.size() == 1);
    CHECK(s.images[0].width % 36 == 0);  // the sheet, enlarged a whole number of times
    auto t = c.ok("gen_tileset", R"({"style":"brick","tile":8})");
    CHECK(t.v().get("tiles").size() == 8);
    CHECK(str(t.v().get("tiles").at(3).get("name")) == "top");
    auto n = c.ok("gen_texture", R"({"kind":"worley","width":64,"height":32,"colors":["#000000","#ffffff"]})");
    REQUIRE(n.images.size() == 1);
    CHECK(n.images[0].width == 64);
    CHECK(n.images[0].height == 32);
    auto sky = c.ok("gen_texture", R"({"kind":"sky","stars":0.5})");
    CHECK(sky.v().get("width").as_number() == 1024);
    CHECK(sky.images[0].width == 512);  // the preview is scaled down

    CHECK(c.fails("gen_sprite", R"({"kind":"dragon"})").find("'kind'") != std::string::npos);
    CHECK(c.fails("gen_sprite", R"({"colors":["#000000","#ffffff"]})").find("at least 3") != std::string::npos);
    CHECK(c.fails("gen_palette", R"({"colors":5,"count":99})").find("'count'") != std::string::npos);
    CHECK(c.fails("gen_texture", R"({"kind":"sky","width":512,"height":512})").find("twice") != std::string::npos);
    CHECK(c.fails("gen_tileset", R"({"colors":["#000000"]})").find("4 fill") != std::string::npos);
    // Saving needs a project.
    CHECK(c.fails("gen_palette", R"({"save":"p.png"})").find("--project") != std::string::npos);
}

TEST_CASE("gen_sfx and gen_music: sfxr parameters back, exact loop length, a waveform") {
    Client c;
    c.init();
    auto s = c.ok("gen_sfx", R"({"seed":3,"preset":"laser"})");
    CHECK(s.v().get("seconds").as_number() > 0.05);
    CHECK(s.v().get("seconds").as_number() < 3.0);
    CHECK(s.v().get("peak").as_number() > 0.05);
    CHECK(s.v().get("params").get("base_freq").is_number());
    REQUIRE(s.images.size() == 1);
    auto sine = c.ok("gen_sfx", R"({"seed":3,"preset":"laser","params":{"wave":"sine","env_decay":0.1}})");
    CHECK(str(sine.v().get("params").get("wave")) == "sine");
    CHECK(sine.v().get("params").get("env_decay").as_number() == doctest::Approx(0.1));
    CHECK(c.fails("gen_sfx", R"({"params":{"bass":1}})").find("unknown sfxr parameter 'bass'") != std::string::npos);
    CHECK(c.fails("gen_sfx", R"({"params":{"duty":3}})").find("params.duty") != std::string::npos);

    auto m = c.ok("gen_music", R"({"seed":2,"bpm":120,"bars":1,"scale":"minor"})");
    CHECK(m.v().get("seconds").as_number() == doctest::Approx(2.0));
    CHECK(m.v().get("peak").as_number() > 0.1);
    CHECK(m.v().get("peak").as_number() <= 1.0);
    CHECK(c.fails("gen_music", R"({"bpm":60,"bars":32})").find("60 seconds") != std::string::npos);
}

TEST_CASE("saving generated content: file, sidecar, CREDITS.md row, cooks with no adopt, GUID kept on overwrite") {
    const std::string root = demo_project("mcp_content_save");
    Client c;
    REQUIRE(c.server.open_project(sv(root), "scenes/demo.mcscene"));
    c.init();
    auto s = c.ok("gen_sprite", R"({"seed":2,"kind":"blob","save":"sprites/slime.png"})");
    const std::string guid = str(s.v().get("saved").get("guid"));
    CHECK(guid.find("guid:") == 0);
    CHECK(std::filesystem::exists(root + "/assets/sprites/slime.png"));
    const std::string side = read_file(root + "/assets/sprites/slime.png.mceasset");
    CHECK(side.find("\"importer\": \"texture\"") != std::string::npos);
    CHECK(side.find("\"filter\": \"nearest\"") != std::string::npos);
    CHECK(side.find(guid.substr(5)) != std::string::npos);
    c.ok("gen_sfx", R"({"preset":"jump","save":"sfx/jump2.wav"})");
    c.ok("gen_texture", R"({"kind":"fbm","width":32,"height":32,"save":"textures/rock.png"})");
    const std::string credits = read_file(root + "/CREDITS.md");
    CHECK(credits.find("| `assets/sprites/slime.png` | generated by gen_sprite (seed 2, blob 16x16) |") != std::string::npos);
    CHECK(credits.find("`assets/sfx/jump2.wav`") != std::string::npos);

    // Already there: refused without overwrite; with it the GUID stays and the row is replaced.
    CHECK(c.fails("gen_sprite", R"({"seed":3,"kind":"blob","save":"sprites/slime.png"})").find("overwrite") != std::string::npos);
    auto again = c.ok("gen_sprite", R"({"seed":3,"kind":"blob","save":"sprites/slime.png","overwrite":true})");
    CHECK(str(again.v().get("saved").get("guid")) == guid);
    CHECK(again.v().get("saved").get("replaced").as_bool());
    const std::string credits2 = read_file(root + "/CREDITS.md");
    CHECK(credits2.find("seed 3, blob") != std::string::npos);
    CHECK(credits2.find("seed 2, blob") == std::string::npos);

    auto cook = c.ok("cook_run");
    CHECK(cook.v().get("ok").as_bool());
    CHECK(listed(c, "sprites/slime.png"));
    CHECK(listed(c, "sfx/jump2.wav"));
    CHECK(c.fails("gen_palette", R"({"save":"../out.png"})").find("under assets/") != std::string::npos);
    CHECK(c.fails("gen_sfx", R"({"save":"sfx/x.png"})").find(".wav") != std::string::npos);
}

TEST_CASE("library_search and library_add: CC0 models with their texture, OFL fonts with the license, recipes") {
    const std::string root = demo_project("mcp_content_library");
    Client c;
    REQUIRE(c.server.open_project(sv(root), "scenes/demo.mcscene"));
    c.init();
    auto roads = c.ok("library_search", R"({"query":"road corner"})");
    REQUIRE(roads.v().get("entries").size() >= 1);
    CHECK(str(roads.v().get("entries").at(0).get("id")) == "kenney-road-corner");
    CHECK(str(roads.v().get("entries").at(0).get("license")) == "CC0 1.0");
    CHECK(c.ok("library_search", R"({"kind":"font","license":"OFL"})").v().get("total").as_number() == 4);
    CHECK(c.ok("library_search", R"({"query":"sky","kind":"recipe"})").v().get("total").as_number() == 3);
    CHECK(c.ok("library_search", R"({"query":"zzzz"})").v().get("total").as_number() == 0);

    auto coin = c.ok("library_add", R"({"id":"kenney-coin"})");
    CHECK(coin.v().get("files").size() == 2);
    CHECK(std::filesystem::exists(root + "/assets/models/coin.glb.mceasset"));
    CHECK(std::filesystem::exists(root + "/assets/models/Textures/colormap.png"));
    auto flag = c.ok("library_add", R"({"id":"kenney-flag","path":"props/"})");
    CHECK(std::filesystem::exists(root + "/assets/props/flag.glb"));
    auto flag2 = c.ok("library_add", R"({"id":"kenney-cloud"})");
    CHECK(flag2.v().get("files").at(1).get("kept").as_bool());  // the colormap is already there
    c.ok("library_add", R"({"id":"font-crimson-pro","path":"fonts/body.ttf"})");
    CHECK(std::filesystem::exists(root + "/assets/fonts/body.ttf"));
    CHECK(std::filesystem::exists(root + "/assets/fonts/CrimsonPro-OFL.txt"));
    auto sky = c.ok("library_add", R"({"id":"sky-dusk"})");
    CHECK(str(sky.v().get("saved").get("path")) == "textures/sky-dusk.png");
    REQUIRE(sky.images.size() == 1);

    const std::string credits = read_file(root + "/CREDITS.md");
    CHECK(credits.find("| `assets/models/coin.glb` | Coin by Kenney (kenney.nl, Starter Kit: 3D Platformer") != std::string::npos);
    CHECK(credits.find("| CC0 1.0 |") != std::string::npos);
    CHECK(credits.find("Crimson Pro Regular (serif) by Jacques Le Bailly") != std::string::npos);
    CHECK(credits.find("| SIL OFL 1.1 |") != std::string::npos);
    CHECK(c.ok("cook_run").v().get("ok").as_bool());
    CHECK(listed(c, "models/coin.glb"));

    CHECK(c.fails("library_add", R"({"id":"kenney-coin"})").find("already exists") != std::string::npos);
    CHECK(c.fails("library_add", R"({"id":"nope"})").find("library_search") != std::string::npos);
    CHECK(c.fails("library_add", R"({"id":"kenney-coin","path":"x.png"})").find("extension") != std::string::npos);
}
