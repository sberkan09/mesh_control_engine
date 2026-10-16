// The UI document tools (ROADMAP M-UI-DATA step 5) in process against a copy of the scene_demo
// project: ui_create / ui_list / ui_widget_add / set / remove keep a document the cook reads the
// same way, and (the "gpu" suite) ui_preview captures are compared with a golden.

#include <doctest/doctest.h>

#include <cstdlib>
#include <filesystem>
#include <string>

#include "mce/cook/image.h"
#include "mce/cook/ui_text.h"
#include "mce/editor/ui_document_editor.h"
#include "mce/ui/document_data.h"
#include "mcp_client.h"

using namespace mce;
using namespace mcp_client;
using namespace cook_test;

namespace {

std::string ui_project(const char* name) {
    const std::string root = fresh_dir(name);
    std::filesystem::copy(std::string(MCE_SOURCE_DIR) + "/samples/scene_demo/assets", root + "/assets",
                          std::filesystem::copy_options::recursive);
    return root;
}

// The document as the cook reads it, encoded: equal bytes = the same document.
std::vector<byte> chunk_of(const std::string& path) {
    const std::string text = read_file(path);
    cook::JsonDoc doc;
    cook::JsonError err;
    REQUIRE(doc.parse(sv(text), err));
    ui::DocumentData d;
    cook::DiagList diags;
    REQUIRE(cook::read_ui_document(doc.root(), "menu.mcui", d, diags));
    Array<byte> out;
    ui::encode_ui_document(d, out);
    return std::vector<byte>(out.begin(), out.end());
}

ui::DocumentData data_of(const std::string& path) {
    const std::string text = read_file(path);
    cook::JsonDoc doc;
    cook::JsonError err;
    REQUIRE(doc.parse(sv(text), err));
    ui::DocumentData d;
    cook::DiagList diags;
    REQUIRE(cook::read_ui_document(doc.root(), "menu.mcui", d, diags));
    return d;
}

const char* const kMenuWidgets = R"([
  { "name": "menu", "kind": "Stack", "at": [0.5, 0.5, 320, 0], "stack": { "spacing": 12 },
    "children": [
      { "name": "title", "kind": "Text", "text": "UNDERTOW", "style": { "text_size": 32 } },
      { "name": "play", "kind": "Button", "text": "PLAY", "default_focus": true, "action": "Play",
        "nav": { "down": "menu/quit" } },
      { "name": "quit", "kind": "Button", "text": "QUIT", "action": "Quit", "nav": { "up": "menu/play" } },
      { "name": "options", "kind": "Tabs", "tabs": [ { "name": "audio", "label": "AUDIO" } ] } ] } ])";

}  // namespace

TEST_CASE("ui tools: create, list, add, set and remove widgets; the document reads back unchanged") {
    const std::string root = ui_project("mcp_ui_edit");
    Client c;
    REQUIRE(c.server.open_project(sv(root), "scenes/demo.mcscene"));
    c.init();
    auto made = c.ok("ui_create", std::string(R"({"path":"ui/menu.mcui","widgets":)") + kMenuWidgets + "}");
    CHECK(str(made.v().get("guid")).rfind("guid:", 0) == 0);
    const std::string file = root + "/assets/ui/menu.mcui";
    REQUIRE(std::filesystem::exists(file));
    CHECK(c.fails("ui_create", R"({"path":"ui/menu.mcui"})").find("already exists") != std::string::npos);
    CHECK(c.fails("ui_create", R"({"path":"ui/bad.mcui","widgets":[{"name":"a","kind":"Buton"}]})").find("UiWidgetKind") != std::string::npos);
    CHECK_FALSE(std::filesystem::exists(root + "/assets/ui/bad.mcui"));

    auto all = c.ok("ui_list");
    REQUIRE(all.v().get("documents").size() == 1);
    CHECK(str(all.v().get("documents").at(0).get("path")) == "ui/menu.mcui");
    CHECK(all.v().get("documents").at(0).get("actions").size() == 2);
    auto one = c.ok("ui_list", R"({"document":"ui/menu.mcui"})");
    const cook::JsonValue ws = one.v().get("widgets");
    REQUIRE(ws.size() == 9);  // menu, title, play, quit, options + its bar, button, pages, page
    CHECK(str(ws.at(2).get("path")) == "menu/play");
    CHECK(str(ws.at(2).get("action")) == "Play");
    CHECK(ws.at(2).get("default_focus").as_bool());
    CHECK(str(ws.at(5).get("path")) == "menu/options/bar");

    // Setting a field to what it is changes nothing: the file is already canonical.
    const std::string before = read_file(file);
    const std::vector<byte> chunk = chunk_of(file);
    c.ok("ui_widget_set", R"({"document":"ui/menu.mcui","widget":"menu/play","fields":{"text":"PLAY"}})");
    CHECK(read_file(file) == before);
    CHECK(chunk_of(file) == chunk);

    // Add at a position, restyle one state, drop an action, move the focus.
    c.ok("ui_widget_add", R"({"document":"ui/menu.mcui","parent":"menu","index":1,"widget":{"name":"score","kind":"Text","bind":"Score/points","format":"BEST %.0f"}})");
    c.ok("ui_widget_set", R"({"document":"ui/menu.mcui","widget":"menu/quit","fields":{"styles":{"Hover":{"tint":[1,0,0,1]}},"action":null,"default_focus":true}})");
    ui::DocumentData d = data_of(file);
    const u32 score = d.find("menu/score"), quit = d.find("menu/quit"), play = d.find("menu/play");
    REQUIRE(score != ui::kNoWidgetIndex);
    CHECK(score == d.find("menu") + 2);  // after title, before play
    CHECK(d.widgets[score].bind.view() == "Score/points");
    CHECK(d.widgets[quit].styles[1].tint.r == 1.0f);
    CHECK(d.widgets[quit].styles[1].tint.g == 0.0f);
    CHECK(d.widgets[quit].styles[0].tint.r != 1.0f);  // Normal untouched
    CHECK(d.widgets[quit].action.empty());
    CHECK(d.default_focus == quit);
    CHECK(d.widgets[quit].nav[ui::NavUp] == play);
    // Every state at once, and an anchor change.
    c.ok("ui_widget_set", R"({"document":"ui/menu.mcui","widget":"menu/title","fields":{"style":{"text_size":40},"fill":true}})");
    d = data_of(file);
    const u32 title = d.find("menu/title");
    for (u32 s = 0; s < ui::kWidgetStateCount; ++s) CHECK(d.widgets[title].styles[s].text_size == 40.0f);
    CHECK(d.widgets[title].anchors.max.x == 1.0f);

    // Mistakes fail with the cook's message and leave the file as it was.
    const std::string good = read_file(file);
    CHECK(c.fails("ui_widget_remove", R"({"document":"ui/menu.mcui","widget":"menu/play"})").find("no widget at 'menu/play'") != std::string::npos);
    CHECK(c.fails("ui_widget_set", R"({"document":"ui/menu.mcui","widget":"menu/nope","fields":{"text":"x"}})").find("no widget at") != std::string::npos);
    CHECK(c.fails("ui_widget_set", R"({"document":"ui/menu.mcui","widget":"menu/play","fields":{"format":"%s"}})").find("format") != std::string::npos);
    CHECK(c.fails("ui_widget_add", R"({"document":"ui/menu.mcui","parent":"menu","widget":{"name":"play"}})").find("already named") != std::string::npos);
    CHECK(c.fails("ui_widget_set", R"({"document":"ui/menu.mcui","widget":"menu","fields":{"children":[]}})").find("children") != std::string::npos);
    CHECK(read_file(file) == good);

    // Remove what nothing names; the cook still reads the project.
    c.ok("ui_widget_remove", R"({"document":"ui/menu.mcui","widget":"menu/options"})");
    CHECK(data_of(file).find("menu/options") == ui::kNoWidgetIndex);
    CHECK(c.ok("cook_run").v().get("failed").as_number() == 0);
}

TEST_CASE("ui_theme_set: a document names a theme, by path or GUID, and goes back to the default look") {
    const std::string root = ui_project("mcp_ui_theme");
    Client c;
    REQUIRE(c.server.open_project(sv(root), "scenes/demo.mcscene"));
    c.init();
    c.ok("ui_create", std::string(R"({"path":"ui/menu.mcui","widgets":)") + kMenuWidgets + "}");
    auto t = c.ok("asset_create", R"({"path":"ui/warm.mcuitheme","content":{"palette":{"button":[0.8,0.3,0.1,1]}}})");
    const std::string theme = str(t.v().get("guid"));
    const std::string file = root + "/assets/ui/menu.mcui";
    const std::vector<byte> plain = chunk_of(file);
    CHECK(str(c.ok("ui_list", R"({"document":"ui/menu.mcui"})").v().get("theme")) == "default");

    auto set = c.ok("ui_theme_set", R"({"document":"ui/menu.mcui","theme":"ui/warm.mcuitheme"})");
    CHECK(str(set.v().get("theme")) == theme);
    CHECK(str(c.ok("ui_list", R"({"document":"ui/menu.mcui"})").v().get("theme")) == theme);
    CHECK(asset::to_string(data_of(file).theme).view() == sv(theme).substr(5));
    const std::string themed = read_file(file);
    c.ok("ui_theme_set", std::string(R"({"document":"ui/menu.mcui","theme":")") + theme + "\"}");
    CHECK(read_file(file) == themed);  // the same theme by GUID: nothing changes

    CHECK(c.fails("ui_theme_set", R"({"document":"ui/menu.mcui","theme":"scenes/demo.mcscene"})").find("not a UI theme") != std::string::npos);
    CHECK(c.fails("ui_theme_set", R"({"document":"ui/menu.mcui","theme":"ui/nope.mcuitheme"})").find("no such asset") != std::string::npos);
    CHECK(c.fails("ui_theme_set", R"({"document":"ui/menu.mcui"})").find("theme") != std::string::npos);
    CHECK(c.fails("ui_theme_set", R"({"document":"ui/warm.mcuitheme","theme":null})").find("not a UI document") != std::string::npos);
    CHECK(read_file(file) == themed);
    CHECK(c.ok("cook_run").v().get("failed").as_number() == 0);

    c.ok("ui_theme_set", R"({"document":"ui/menu.mcui","theme":null})");
    CHECK(chunk_of(file) == plain);
    CHECK(str(c.ok("ui_list", R"({"document":"ui/menu.mcui"})").v().get("theme")) == "default");
}

TEST_CASE("UI Document panel: a document edited in the editor round-trips through the cook and the MCP tools") {
    const std::string root = ui_project("mcp_ui_panel");
    Client c;
    REQUIRE(c.server.open_project(sv(root), "scenes/demo.mcscene"));
    c.init();
    editor::EditorSession& session = c.server.session();
    editor::UiDocumentEditor ed;
    REQUIRE(ed.create(session, "ui/pause.mcui").has_value());
    REQUIRE(ed.add_widget(ui::kNoWidgetIndex, ui::WidgetKind::Stack));
    REQUIRE(ed.add_widget(0, ui::WidgetKind::Button));
    REQUIRE(ed.add_widget(0, ui::WidgetKind::Slider));
    ui::DocumentData next = ed.data();
    next.widgets[1].text.assign("RESUME");
    next.widgets[1].action.assign("Resume");
    next.widgets[1].nav[ui::NavDown] = 2;
    next.widgets[2].convar.assign("snd.volume");
    next.default_focus = 1;
    REQUIRE(ed.apply(next));
    REQUIRE(ed.save(session).has_value());
    const std::string file = root + "/assets/ui/pause.mcui";
    const std::string saved = read_file(file);
    const std::vector<byte> chunk = chunk_of(file);

    // MCP reads what the panel wrote, and an edit that changes nothing leaves it byte for byte.
    auto one = c.ok("ui_list", R"({"document":"ui/pause.mcui"})");
    REQUIRE(one.v().get("widgets").size() == 3);
    CHECK(str(one.v().get("widgets").at(1).get("path")) == "stack/button");
    CHECK(str(one.v().get("widgets").at(1).get("action")) == "Resume");
    CHECK(one.v().get("widgets").at(1).get("default_focus").as_bool());
    c.ok("ui_widget_set", R"({"document":"ui/pause.mcui","widget":"stack/button","fields":{"text":"RESUME"}})");
    CHECK(read_file(file) == saved);
    CHECK(chunk_of(file) == chunk);
    CHECK(c.ok("cook_run").v().get("failed").as_number() == 0);

    // An MCP edit comes back into the panel, and the panel's next save keeps it.
    c.ok("ui_widget_set", R"({"document":"ui/pause.mcui","widget":"stack/slider","fields":{"range":[0,1,0.25]}})");
    REQUIRE(ed.sync(session));
    CHECK(ed.data().widgets[2].step == 0.25f);
    REQUIRE(ed.move_widget(2, -1));
    REQUIRE(ed.save(session).has_value());
    auto moved = c.ok("ui_list", R"({"document":"ui/pause.mcui"})");
    CHECK(str(moved.v().get("widgets").at(1).get("path")) == "stack/slider");
    CHECK(data_of(file).widgets[data_of(file).find("stack/slider")].step == 0.25f);
    CHECK(read_file(file) == std::string(ed.text().view().data(), ed.text().view().size()));
}

TEST_SUITE("gpu") {

TEST_CASE("ui_preview: a document alone at a viewport, safe area and scale, checked against a golden") {
    const std::string root = ui_project("mcp_ui_preview");
    Client c([] {
        mcp::ServerOptions opt;
        opt.player = MCE_PLAYER_EXE;
        return opt;
    }());
    REQUIRE(c.server.open_project(sv(root), "scenes/demo.mcscene"));
    c.init();
    c.ok("ui_create", std::string(R"({"path":"ui/menu.mcui","widgets":)") + kMenuWidgets + "}");
    CHECK(c.fails("ui_preview", R"({"document":"scenes/demo.mcscene"})").find("not a UI document") != std::string::npos);

    auto a = c.ok("ui_preview", R"({"document":"ui/menu.mcui","width":320,"height":240,"save":"shots/menu.png"})");
    REQUIRE(a.images.size() == 1);
    CHECK(a.images[0].width == 320);
    auto again = c.ok("ui_preview", R"({"document":"ui/menu.mcui","width":320,"height":240})");
    CHECK(a.images[0].rgba.size() == again.images[0].rgba.size());
    CHECK(std::equal(a.images[0].rgba.begin(), a.images[0].rgba.end(), again.images[0].rgba.begin()));
    // The UI scale (on a screen big enough for it: a small one lowers the scale to fit) and the safe area.
    auto one = c.ok("ui_preview", R"({"document":"ui/menu.mcui","width":1280,"height":960})");
    auto two = c.ok("ui_preview", R"({"document":"ui/menu.mcui","width":1280,"height":960,"ui_scale":2})");
    CHECK_FALSE(std::equal(one.images[0].rgba.begin(), one.images[0].rgba.end(), two.images[0].rgba.begin()));
    auto inset = c.ok("ui_preview", R"({"document":"ui/menu.mcui","width":320,"height":240,"safe_area":{"left":100}})");
    CHECK_FALSE(std::equal(a.images[0].rgba.begin(), a.images[0].rgba.end(), inset.images[0].rgba.begin()));
    c.ok("ui_widget_set", R"({"document":"ui/menu.mcui","widget":"menu/play","fields":{"text":"START"}})");
    auto edited = c.ok("ui_preview", R"({"document":"ui/menu.mcui","width":320,"height":240})");
    CHECK_FALSE(std::equal(a.images[0].rgba.begin(), a.images[0].rgba.end(), edited.images[0].rgba.begin()));

    // The golden (tests/mcp/goldens, Mesa llvmpipe): within 0.5 % mean difference. A missing golden
    // is written and passes; MCE_UPDATE_GOLDENS=1 rewrites it.
    const std::string goldens = std::string(MCE_SOURCE_DIR) + "/tests/mcp/goldens";
    const std::string golden = goldens + "/ui_preview_menu.png";
    const char* upd = std::getenv("MCE_UPDATE_GOLDENS");
    if (!std::filesystem::exists(golden) || (upd && *upd == '1')) {
        std::filesystem::copy_file(root + "/shots/menu.png", golden, std::filesystem::copy_options::overwrite_existing);
        MESSAGE("wrote the golden " << golden);
    }
    std::filesystem::create_directories(root + "/goldens");
    std::filesystem::copy_file(golden, root + "/goldens/ui_preview_menu.png", std::filesystem::copy_options::overwrite_existing);
    auto d = c.ok("image_diff", R"({"golden":"goldens/ui_preview_menu.png","image":"shots/menu.png","max_score":0.005})");
    CHECK_MESSAGE(d.v().get("pass").as_bool(), "ui_preview drifted from tests/mcp/goldens/ui_preview_menu.png: " << d.text);
}

TEST_CASE("ui_preview: one document under two themes draws two different frames") {
    const std::string root = ui_project("mcp_ui_preview_theme");
    Client c([] {
        mcp::ServerOptions opt;
        opt.player = MCE_PLAYER_EXE;
        return opt;
    }());
    REQUIRE(c.server.open_project(sv(root), "scenes/demo.mcscene"));
    c.init();
    c.ok("ui_create", std::string(R"({"path":"ui/menu.mcui","widgets":)") + kMenuWidgets + "}");
    c.ok("asset_create", R"({"path":"ui/warm.mcuitheme","content":{"palette":{"button":[0.85,0.35,0.1,1],"text":[1,0.95,0.8,1]}}})");
    c.ok("asset_create", R"({"path":"ui/cold.mcuitheme","content":{"palette":{"button":[0.1,0.3,0.85,1],"text":[0.8,0.95,1,1]}}})");
    const char* const shot = R"({"document":"ui/menu.mcui","width":320,"height":240})";
    auto plain = c.ok("ui_preview", shot);
    c.ok("ui_theme_set", R"({"document":"ui/menu.mcui","theme":"ui/warm.mcuitheme"})");
    auto warm = c.ok("ui_preview", shot);
    c.ok("ui_theme_set", R"({"document":"ui/menu.mcui","theme":"ui/cold.mcuitheme"})");
    auto cold = c.ok("ui_preview", shot);
    REQUIRE(warm.images.size() == 1);
    REQUIRE(cold.images.size() == 1);
    REQUIRE(warm.images[0].rgba.size() == cold.images[0].rgba.size());
    auto same = [](const auto& a, const auto& b) { return std::equal(a.rgba.begin(), a.rgba.end(), b.rgba.begin()); };
    CHECK_FALSE(same(warm.images[0], cold.images[0]));
    CHECK_FALSE(same(plain.images[0], warm.images[0]));
    // Back to the default look: the first frame again.
    c.ok("ui_theme_set", R"({"document":"ui/menu.mcui","theme":null})");
    CHECK(same(plain.images[0], c.ok("ui_preview", shot).images[0]));
}

TEST_CASE("ui_preview: a theme's sprite-sheet skin draws its 9-slice frame; a bad one falls back to flat quads") {
    const std::string root = ui_project("mcp_ui_preview_skin");
    Client c([] {
        mcp::ServerOptions opt;
        opt.player = MCE_PLAYER_EXE;
        return opt;
    }());
    REQUIRE(c.server.open_project(sv(root), "scenes/demo.mcscene"));
    c.init();
    c.ok("ui_create", std::string(R"({"path":"ui/menu.mcui","widgets":)") + kMenuWidgets + "}");
    // The skin: a 16 x 16 texture with a white 4 x 4 block and a 12 x 12 frame with a 3-texel black
    // border around a white middle. The sheet's default 1 x 1 grid is region 0, so they are 1 and 2.
    Array<u8> rgba;
    for (u32 y = 0; y < 16; ++y)
        for (u32 x = 0; x < 16; ++x) {
            const bool frame = x >= 4 && y < 12;
            const bool border = frame && (x < 7 || x > 12 || y < 3 || y > 8);
            const u8 v = border ? 0 : 255;
            for (u8 b : {v, v, v, u8(frame || (x < 4 && y < 4) ? 255 : 0)}) rgba.push_back(b);
        }
    Array<byte> png;
    REQUIRE(cook::encode_png(16, 16, rgba.span(), png).has_value());
    std::filesystem::create_directories(root + "/assets/ui");
    write_file(root + "/assets/ui/skin.png", std::string(reinterpret_cast<const char*>(png.data()), png.size()));
    write_file(root + "/assets/ui/skin.png.mceasset",
               "{ \"guid\": \"5ce4e000-0000-4000-8000-0000000000a1\", \"importer\": \"texture\", \"importer_version\": 1,"
               " \"settings\": { \"srgb\": true, \"mips\": false } }\n");
    const std::string sheet = str(c.ok("asset_create", R"({"path":"ui/skin.mcsheet","content":{"texture":"guid:5ce4e000-0000-4000-8000-0000000000a1",)"
                                                       R"("image_size":[16,16],"regions":[{"name":"white","rect":[0,0,4,4]},{"name":"frame","rect":[4,0,12,12]}]}})")
                                      .v().get("guid"));
    const std::string palette = R"("palette":{"button":[0.85,0.35,0.1,1],"text":[1,0.95,0.8,1]})";
    const std::string framed = R"("styles":{"Button":{"style":{"sprite_region":2,"slice":{"left":3,"top":3,"right":3,"bottom":3}}}})";
    c.ok("asset_create", R"({"path":"ui/flat.mcuitheme","content":{)" + palette + "}}");
    c.ok("asset_create", R"({"path":"ui/skinned.mcuitheme","content":{)" + palette + R"(,"sprites":")" + sheet + R"(","white_region":1,)" + framed + "}}");
    c.ok("asset_create", R"({"path":"ui/broken.mcuitheme","content":{)" + palette + R"(,"sprites":")" + sheet + R"(","white_region":7,)" + framed + "}}");
    const char* const shot = R"({"document":"ui/menu.mcui","width":320,"height":240})";
    // Dark pixels: the frame's border, tinted by the button color, stays dark.
    auto dark = [](const auto& img) {
        usize n = 0;
        for (usize i = 0; i + 3 < img.rgba.size(); i += 4)
            if (img.rgba[i] < 24 && img.rgba[i + 1] < 24 && img.rgba[i + 2] < 24) ++n;
        return n;
    };
    c.ok("ui_theme_set", R"({"document":"ui/menu.mcui","theme":"ui/flat.mcuitheme"})");
    auto flat = c.ok("ui_preview", shot);
    c.ok("ui_theme_set", R"({"document":"ui/menu.mcui","theme":"ui/skinned.mcuitheme"})");
    auto skinned = c.ok("ui_preview", shot);
    c.ok("ui_theme_set", R"({"document":"ui/menu.mcui","theme":"ui/broken.mcuitheme"})");
    auto broken = c.ok("ui_preview", shot);
    CHECK(str(broken.v().get("scene")) == "scenes/demo.mcscene");  // a copy, not a view into the rescanned assets
    REQUIRE(flat.images.size() == 1);
    REQUIRE(skinned.images.size() == 1);
    REQUIRE(broken.images.size() == 1);
    // Two buttons, each with a 3-pixel border at UI scale 1: well over a hundred more dark pixels.
    CHECK(dark(skinned.images[0]) > dark(flat.images[0]) + 200);
    CHECK(dark(broken.images[0]) == dark(flat.images[0]));
}

}  // TEST_SUITE("gpu")
