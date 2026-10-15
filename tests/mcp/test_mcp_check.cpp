// project_check and ship_build's refusals (ROADMAP M-AGENT step 6) in process against a copy of the
// scene_demo project with problems planted in it. ship_build's happy path needs an installed SDK and
// runs in tests/sdk/sdk_test.cmake.

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

// The findings of one check, as "severity file: message" lines.
std::string findings(const Client::Tool& t, const std::string& check) {
    std::string s;
    const cook::JsonValue f = t.v().get("findings");
    for (u32 i = 0; i < f.size(); ++i)
        if (str(f.at(i).get("check")) == check)
            s += str(f.at(i).get("severity")) + " " + str(f.at(i).get("file")) + ": " + str(f.at(i).get("message")) + "\n";
    return s;
}

}  // namespace

TEST_CASE("project_check: a clean project passes; credits, unused and the per-tick budget are reported") {
    const std::string root = demo_project("mcp_check_clean");
    Client c;
    REQUIRE(c.server.open_project(sv(root), "scenes/demo.mcscene"));
    c.init();
    auto t = c.ok("project_check", R"({"ticks":30})");
    CHECK(t.v().get("pass").as_bool());
    CHECK(t.v().get("errors").as_number() == 0);
    // No CREDITS.md: every media file is named.
    const std::string credits = findings(t, "credits");
    CHECK(credits.find("warning assets/textures/ball.png: the project has no CREDITS.md") != std::string::npos);
    CHECK(credits.find("assets/fonts/ProggyClean.ttf") != std::string::npos);
    CHECK(findings(t, "accessibility").empty());  // the title is 32 px, light on dark
    const cook::JsonValue perf = t.v().get("performance");
    REQUIRE(perf.size() == 1);
    CHECK(str(perf.at(0).get("scene")) == "scenes/demo.mcscene");
    CHECK(perf.at(0).get("ok").as_bool());
    CHECK(perf.at(0).get("ticks").as_number() == 30);
    CHECK(perf.at(0).get("entities").as_number() >= 6);

    // A CREDITS.md that covers the textures by a pattern and the font by name.
    write_file(root + "/CREDITS.md",
               "# credits\n\n| Files | Source | License |\n|---|---|---|\n| `assets/textures/*.png` | drawn | CC0 |\n"
               "| `assets/fonts/ProggyClean.ttf` | Tristan Grimmer | MIT |\n");
    auto t2 = c.ok("project_check", R"({"checks":["credits","unused"]})");
    CHECK(findings(t2, "credits").empty());
    CHECK_FALSE(t2.v().get("performance").valid());
    CHECK(t2.v().get("checks").get("cook").valid() == false);

    // An asset nothing references.
    write_file(root + "/assets/textures/spare.png", make_png(2, 2, std::vector<u8>(16, 255)));
    c.ok("cook_run", R"({"adopt":true})");
    auto t3 = c.ok("project_check", R"({"checks":["unused","credits"]})");
    CHECK(findings(t3, "unused") == "info assets/textures/spare.png: no scene, prefab, material, model or project source references this asset\n");
    // A tile set a .mctilemap names is used (every .mc* document counts as a referrer).
    write_file(root + "/assets/textures/tiles.png", make_png(2, 2, std::vector<u8>(16, 200)));
    write_file(root + "/assets/textures/tiles.png.mceasset", sidecar("5ce4e000-0000-4000-8000-0000000000b1", "texture"));
    write_file(root + "/assets/maps/meadow.mctilemap",
               R"({"format":"mce-tilemap","version":1,"guid":"5ce4e000-0000-4000-8000-0000000000b2","name":"meadow","tileset":"guid:5ce4e000-0000-4000-8000-0000000000b1","columns":1,"rows":1,"width":1,"height":1,"tile_size":1,"tiles":[0]})");
    CHECK(findings(c.ok("project_check", R"({"checks":["unused"]})"), "unused").find("tiles.png") == std::string::npos);
    CHECK(findings(t3, "credits").empty());  // covered by the *.png row
    // An input map is an entry point (the player loads the pack's first .mcinput itself).
    write_file(root + "/assets/input/controls.mcinput",
               R"({"format":"mce-input","version":1,"guid":"5ce4e000-0000-4000-8000-0000000000b3",)"
               R"("maps":[{"name":"probe","actions":[{"name":"Jump","bindings":[{"key":"Space"}]}]}]})");
    CHECK(findings(c.ok("project_check", R"({"checks":["unused"]})"), "unused").find("controls.mcinput") == std::string::npos);
    // A row for a file the project no longer has.
    write_file(root + "/CREDITS.md",
               "# credits\n\n| Files | Source | License |\n|---|---|---|\n| `assets/textures/*.png` | drawn | CC0 |\n"
               "| `assets/fonts/ProggyClean.ttf` | Tristan Grimmer | MIT |\n| `assets/sfx/gone.wav` | made | CC0 |\n");
    CHECK(findings(c.ok("project_check", R"({"checks":["credits"]})"), "credits") ==
          "warning CREDITS.md: CREDITS.md names assets/sfx/gone.wav, which the project no longer has\n");

    // A budget nothing meets.
    auto slow = c.ok("project_check", R"({"checks":["performance"],"ticks":5,"budget_ms":0.000001})");
    CHECK(findings(slow, "performance").find("warning assets/scenes/demo.mcscene: the sim takes") == 0);
    CHECK(slow.v().get("pass").as_bool());  // warnings do not fail it

    CHECK(c.fails("project_check", R"({"checks":["speling"]})").find("'checks'") != std::string::npos);
    CHECK(c.fails("project_check", R"({"budget_ms":0})").find("'budget_ms'") != std::string::npos);
}

TEST_CASE("project_check: broken references fail it; sRGB data textures and unreadable text are flagged with a fix") {
    const std::string root = demo_project("mcp_check_problems");
    // A scene whose sprite names a texture that does not exist, small low-contrast text, a normal map imported as sRGB.
    write_file(root + "/assets/scenes/hud.mcscene", R"({
  "format": "mce-scene", "version": 1, "guid": "5ce4e000-0000-4000-8000-0000000000a1", "name": "hud",
  "entities": [
    { "id": 1, "name": "Camera", "components": { "Camera2DComponent": { "pixels_per_meter": 40, "clear_color": [0.1, 0.1, 0.1, 1] } } },
    { "id": 2, "name": "Hint", "components": { "LocalTransform": {}, "TextLabel": { "font": "guid:5ce4e000-0000-4000-8000-000000000010", "text": "hi", "color": [0.2, 0.2, 0.2, 1], "height": 0.2 } } },
    { "id": 3, "name": "Ghost", "components": { "LocalTransform": {}, "Sprite": { "texture": "guid:5ce4e000-0000-4000-8000-0000000000ff" } } },
    { "id": 4, "name": "TitleShadow", "components": { "LocalTransform": {}, "TextLabel": { "font": "guid:5ce4e000-0000-4000-8000-000000000010", "text": "Glow", "color": [0, 0, 0, 1], "height": 1 } } },
    { "id": 5, "name": "Title", "components": { "LocalTransform": {}, "TextLabel": { "font": "guid:5ce4e000-0000-4000-8000-000000000010", "text": "Glow", "color": [1, 0.9, 0.5, 1], "height": 1 } } },
    { "id": 6, "name": "Panel", "components": { "LocalTransform": { "position": [5, 3, 0] }, "Sprite": { "tint": [0.95, 0.9, 0.8, 1], "size": [4, 1.5] } } },
    { "id": 7, "name": "Score", "components": { "LocalTransform": { "position": [5.5, 3, 0] }, "TextLabel": { "font": "guid:5ce4e000-0000-4000-8000-000000000010", "text": "Gems 0 / 5", "color": [0.15, 0.1, 0.05, 1], "height": 0.6 } } }
  ]
}
)");
    write_file(root + "/assets/textures/rock_normal.png", make_png(2, 2, std::vector<u8>(16, 128)));
    write_file(root + "/assets/textures/rock_normal.png.mceasset", sidecar("5ce4e000-0000-4000-8000-0000000000a3", "texture"));
    Client c;
    REQUIRE(c.server.open_project(sv(root), "scenes/demo.mcscene"));
    c.init();
    auto t = c.ok("project_check", R"({"ticks":10})");
    CHECK_FALSE(t.v().get("pass").as_bool());
    CHECK(t.v().get("errors").as_number() >= 1);
    // The broken GUID is an error, from the cook or the reference scan, naming the scene.
    const cook::JsonValue first = t.v().get("findings").at(0);
    CHECK(str(first.get("severity")) == "error");
    CHECK(str(first.get("file")) == "assets/scenes/hud.mcscene");

    const std::string srgb = findings(t, "srgb");
    CHECK(srgb.find("warning assets/textures/rock_normal.png: the name says data") != std::string::npos);
    const std::string a11y = findings(t, "accessibility");
    CHECK(a11y.find("Hint: text is 8.0 px high") != std::string::npos);
    CHECK(a11y.find("Hint: text contrast against what is behind it (a backing sprite, else the clear colour) is 1.") != std::string::npos);
    CHECK(a11y.find("Score") == std::string::npos);  // dark text on its light backing panel reads well
    CHECK(a11y.find("TitleShadow") == std::string::npos);  // a drop shadow behind a readable copy of its text
    // The fix it names works.
    c.ok("asset_set_import_settings", R"({"path":"textures/rock_normal.png","settings":{"srgb":false}})");
    CHECK(findings(c.ok("project_check", R"({"checks":["srgb"]})"), "srgb").empty());
}

TEST_CASE("M-ACCESS gate: project_check flags a seeded UI contrast failure, small UI text, a timeline with no subtitles and uncaptioned sounds") {
    const std::string root = demo_project("mcp_check_access");
    // Grey on grey in a menu (the seeded failure), a readable title and button, tiny print, and a
    // themed document whose unstated colours are the theme's (not judged).
    write_file(root + "/assets/ui/menu.mcui", R"({
  "format": "mce-ui", "version": 1, "guid": "5ce4e000-0000-4000-8000-0000000000b1",
  "widgets": [
    { "name": "box", "fill": true, "style": { "tint": [0.35, 0.35, 0.35, 1] },
      "children": [
        { "name": "title", "kind": "Text", "text": "PAUSED", "style": { "text_color": [1, 1, 1, 1], "text_size": 32 } },
        { "name": "hint", "kind": "Text", "text": "press start", "style": { "text_color": [0.5, 0.5, 0.5, 1] } },
        { "name": "legal", "kind": "Text", "text": "v1.0", "style": { "text_size": 9 } },
        { "name": "resume", "kind": "Button", "text": "RESUME", "style": { "tint": [0.05, 0.05, 0.1, 1], "text_color": [1, 0.9, 0.5, 1] } }
      ] }
  ]
}
)");
    write_file(root + "/assets/ui/menu.mcui.mceasset", sidecar("5ce4e000-0000-4000-8000-0000000000b1", "ui"));
    write_file(root + "/assets/cutscene.mctimeline", R"({ "format": "mce-timeline", "version": 1, "name": "cutscene", "duration": 2,
  "tracks": [ { "type": "audio", "keys": [ { "t": 0.5, "sound": "guid:5ce4e000-0000-4000-8000-000000000020" } ] } ] }
)");
    write_file(root + "/assets/scenes/bell.mcscene", R"({
  "format": "mce-scene", "version": 1, "guid": "5ce4e000-0000-4000-8000-0000000000b2", "name": "bell",
  "entities": [
    { "id": 1, "name": "Bell", "components": { "SfxTrigger": { "on": "Start" } } },
    { "id": 2, "name": "Door", "components": { "SfxTrigger": { "on": "Start" }, "SoundCaption": { "text": "[door creaks]" } } },
    { "id": 3, "name": "Theme", "components": { "AudioSource": { "bus": "Music", "loop": true } } }
  ]
}
)");
    Client c;
    REQUIRE(c.server.open_project(sv(root), "scenes/demo.mcscene"));
    c.init();
    const std::string a11y = findings(c.ok("project_check", R"({"checks":["accessibility"]})"), "accessibility");
    INFO(a11y);
    CHECK(a11y.find("warning assets/ui/menu.mcui: hint: UI text contrast against the panel behind it is 1.8:1") != std::string::npos);
    CHECK(a11y.find("legal: UI text is 9 px") != std::string::npos);
    CHECK(a11y.find("title:") == std::string::npos);   // white on dark grey
    CHECK(a11y.find("resume:") == std::string::npos);  // its own dark quad behind it
    CHECK(a11y.find("warning assets/cutscene.mctimeline: the timeline plays sounds but has no subtitle track") != std::string::npos);
    CHECK(a11y.find("info assets/scenes/bell.mcscene: Bell plays a sound with no caption") != std::string::npos);
    CHECK(a11y.find("Door plays") == std::string::npos);   // captioned
    CHECK(a11y.find("Theme plays") == std::string::npos);  // music needs none

    // The fixes it names clear them.
    write_file(root + "/assets/ui/menu.mcui", R"({
  "format": "mce-ui", "version": 1, "guid": "5ce4e000-0000-4000-8000-0000000000b1",
  "widgets": [ { "name": "box", "fill": true, "style": { "tint": [0.35, 0.35, 0.35, 1] },
      "children": [ { "name": "hint", "kind": "Text", "text": "press start", "style": { "text_color": [1, 1, 1, 1], "text_size": 18 } } ] } ]
}
)");
    write_file(root + "/assets/cutscene.mctimeline", R"({ "format": "mce-timeline", "version": 1, "name": "cutscene", "duration": 2,
  "tracks": [ { "type": "audio", "keys": [ { "t": 0.5, "sound": "guid:5ce4e000-0000-4000-8000-000000000020" } ] },
              { "type": "subtitle", "lines": [ { "t": 0.5, "duration": 1, "text": "[a bell rings]" } ] } ] }
)");
    const std::string after = findings(c.ok("project_check", R"({"checks":["accessibility"]})"), "accessibility");
    CHECK(after.find("menu.mcui") == std::string::npos);
    CHECK(after.find("cutscene.mctimeline") == std::string::npos);
}

TEST_CASE("ship_build: refused without a game module or outside an SDK") {
    const std::string root = demo_project("mcp_check_ship");
    Client c;
    REQUIRE(c.server.open_project(sv(root), "scenes/demo.mcscene"));
    c.init();
    CHECK(c.fails("ship_build").find("no game module is loaded") != std::string::npos);
    CHECK(c.fails("ship_build", R"({"version":"1 0"})").find("'version'") != std::string::npos);
    CHECK(c.fails("ship_build", R"({"out":"../x"})").find("'out'") != std::string::npos);
}
