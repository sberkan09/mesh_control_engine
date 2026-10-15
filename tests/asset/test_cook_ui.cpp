// M-UI-DATA step 1: a .mcui UI document cooks to CHNK_UIDOC, decodes, builds a ui::Document like
// the Builder does, and round-trips (cook -> write_ui_document -> cook gives the same bytes;
// Document -> extract_document -> encode gives them too). Mistakes fail the cook with file:line.

#include <doctest/doctest.h>

#include "cook_test_util.h"
#include "mce/asset/asset_system.h"
#include "mce/convar.h"
#include "mce/cook/cook.h"
#include "mce/cook/ui_text.h"
#include "mce/ui/document_data.h"
#include "mce/ui/ui.h"

using namespace mce;
using namespace cook_test;

namespace {

ConVar g_volume("test.ui_volume", 0.8f, "a convar the test document binds a slider to");

constexpr const char* kGuid = "5703e000-0000-4000-8000-0000000000e1";

std::string ui_diags(const cook::CookReport& r) {
    std::string s;
    for (const auto& d : r.diags.all()) {
        cook::String line;
        cook::format_diagnostic(d, line);
        s += line.c_str();
        s += "\n";
    }
    return s;
}

// Cooks `source` as assets/menu.mcui in a fresh project and returns the CHNK_UIDOC payload.
std::vector<byte> cook_ui(const std::string& dir, const std::string& source) {
    const std::string root = fresh_dir(dir);
    write_file(root + "/assets/menu.mcui", source);
    cook::CookService svc;
    const cook::CookReport r = svc.cook(StringView(root.c_str()), cook::CookOptions{});
    INFO(ui_diags(r));
    REQUIRE(r.ok());
    asset::AssetSystem assets;
    const std::string cooked = root + "/cooked";
    const StringView roots[] = {StringView(cooked.c_str())};
    REQUIRE(assets.mount(Span<const StringView>(roots, 1)));
    const asset::AssetKey key = assets.find(asset::parse_guid(kGuid).value());
    REQUIRE(key != asset::kInvalidAssetKey);
    CHECK(assets.info(key).type == asset::ChunkType::UiDocument);
    auto h = assets.load_sync<asset::UiDocument>(key);
    REQUIRE(h);
    const Span<const byte> p = assets.resolve(h.value());
    return std::vector<byte>(p.begin(), p.end());
}

const char* const kMenu = R"({ "format": "mce-ui", "version": 1, "guid": "5703e000-0000-4000-8000-0000000000e1",
  "white_region": 3,
  "widgets": [
    { "name": "bg", "fill": true, "style": { "tint": [0, 0, 0, 0.6] } },
    { "name": "menu", "kind": "Stack", "at": [0.5, 0.45, 320, 0], "stack": { "spacing": 12, "align": "Center" },
      "children": [
        { "name": "title", "kind": "Text", "text": "UNDERTOW", "style": { "text_size": 32, "text_color": [1, 0.8, 0.4, 1], "text_outline": 2, "text_shadow": [2, 3] },
          "bind": "Score/points", "format": "BEST %.0f" },
        { "name": "play", "kind": "Button", "text": "PLAY", "default_focus": true, "action": "Play",
          "styles": { "Hover": { "tint": [0.3, 0.3, 0.4, 1], "slice": { "left": 4, "right": 4 } } },
          "nav": { "down": "menu/volume" } },
        { "name": "volume", "kind": "Slider", "range": [0, 1, 0.05], "value": 0.8, "convar": "test.ui_volume",
          "nav": { "up": "menu/play" } },
        { "name": "fx", "kind": "Toggle", "text": "EFFECTS", "value": 1 },
        { "name": "levels", "kind": "Grid", "grid": { "columns": 0, "cell": [48, 48] },
          "children": [ { "name": "l1", "kind": "Image", "style": { "sprite_region": 7 } } ] },
        { "name": "options", "kind": "Tabs", "value": 1, "anchors": { "min": [0, 0], "max": [1, 0], "offset_max": [0, 200] },
          "tabs": [ { "name": "audio", "label": "AUDIO" },
                    { "name": "video", "label": "VIDEO", "children": [ { "name": "hint", "kind": "Text", "text": "soon" } ] } ] } ] },
    { "name": "confirm", "modal": true, "children": [ { "name": "yes", "kind": "Button", "text": "YES" } ] },
    { "name": "log", "kind": "Scroll", "visible": false, "interactable": false } ] })";

}  // namespace

TEST_CASE("ui: .mcui cooks to CHNK_UIDOC, decodes and builds a Document") {
    const std::vector<byte> bytes = cook_ui("ui_cook", kMenu);
    ui::DocumentData d;
    REQUIRE(ui::decode_ui_document(Span<const byte>(bytes.data(), bytes.size()), d));
    CHECK(d.white_region == 3);
    CHECK(d.modal);
    const u32 play = d.find("menu/play");
    REQUIRE(play != ui::kNoWidgetIndex);
    CHECK(d.default_focus == play);
    CHECK(d.widgets[play].nav[ui::NavDown] == d.find("menu/volume"));
    CHECK(d.widgets[play].styles[u32(ui::WidgetState::Hover)].slice.left == 4.0f);
    CHECK(d.widgets[d.find("menu/volume")].step == 0.05f);
    CHECK(d.widgets[d.find("menu/levels/l1")].styles[0].sprite_region == 7);
    CHECK(d.widgets[d.find("menu/title")].styles[0].text_outline == 2.0f);
    CHECK(d.widgets[d.find("menu/title")].styles[4].text_shadow.y == 3.0f);
    CHECK(d.find("menu/options/bar/video") != ui::kNoWidgetIndex);
    CHECK_FALSE(d.widgets[d.find("menu/options/pages/audio")].visible);
    CHECK(d.widgets[d.find("menu/options/pages/video")].visible);
    CHECK_FALSE(d.widgets[d.find("confirm")].visible);

    ui::Document doc;
    REQUIRE(ui::build_document(d, doc));
    const ui::WidgetId w = doc.find("menu/play");
    REQUIRE(doc.valid(w));
    CHECK(doc.kind(w) == ui::WidgetKind::Button);
    CHECK(doc.text(w) == "PLAY");
    CHECK(doc.default_focus() == w);
    CHECK(doc.nav(w).down == doc.find("menu/volume"));
    CHECK(doc.convar(doc.find("menu/volume")) == "test.ui_volume");
    CHECK(doc.value(doc.find("menu/volume")) == doctest::Approx(0.8f));  // the convar's value
    CHECK(doc.selected_tab(doc.find("menu/options")) == 1);
    CHECK(doc.blocks_pointer(doc.find("confirm")));
    CHECK(doc.anchors(doc.find("confirm")).to == ui::AnchorTo::Screen);
}

TEST_CASE("ui: a widget starts from the look the Builder gives its kind") {
    const std::vector<byte> bytes = cook_ui("ui_builder", R"({ "format": "mce-ui", "version": 1,
      "guid": "5703e000-0000-4000-8000-0000000000e1",
      "widgets": [ { "name": "bg", "fill": true },
                   { "name": "col", "kind": "Stack", "at": [0.5, 0.5, 320, 0],
                     "children": [ { "name": "resume", "kind": "Button", "text": "RESUME", "default_focus": true },
                                   { "name": "music", "kind": "Toggle", "text": "MUSIC", "value": 1 },
                                   { "name": "speed", "kind": "Slider", "range": [1, 4, 0.5], "value": 2 } ] },
                   { "name": "tabs", "kind": "Tabs", "tabs": [ { "name": "a", "label": "A" }, { "name": "b", "label": "B" } ] },
                   { "name": "ask", "modal": true, "children": [ { "name": "ok", "kind": "Button", "text": "OK" } ] },
                   { "name": "console", "kind": "Console" } ] })");

    ui::Document built;
    ui::Builder b(built, built.root());
    b.panel("bg").fill();
    ui::Builder col = b.stack("col").at({0.5f, 0.5f}, {320, 0});
    col.button("resume", "RESUME").default_focus();
    col.toggle("music", "MUSIC", true);
    col.slider("speed", 1.0f, 4.0f, 0.5f);
    built.set_value(built.find("col/speed"), 2.0f);
    ui::Builder tabs = b.tabs("tabs");
    tabs.tab("a", "A");
    tabs.tab("b", "B");
    b.modal("ask").button("ok", "OK");
    b.console("console");

    ui::DocumentData from_builder;
    ui::extract_document(built, from_builder);
    Array<byte> enc;
    ui::encode_ui_document(from_builder, enc);
    CHECK(std::vector<byte>(enc.begin(), enc.end()) == bytes);
}

TEST_CASE("ui: a document round-trips through the cook and through ui::Document unchanged") {
    const std::vector<byte> first = cook_ui("ui_trip_a", kMenu);
    ui::DocumentData d;
    REQUIRE(ui::decode_ui_document(Span<const byte>(first.data(), first.size()), d));

    // Data -> .mcui text -> cook: the same chunk, and writing it again gives the same text.
    cook::String text;
    cook::write_ui_document(d, asset::parse_guid(kGuid).value(), text);
    const std::vector<byte> second = cook_ui("ui_trip_b", text.c_str());
    CHECK(second == first);
    ui::DocumentData d2;
    REQUIRE(ui::decode_ui_document(Span<const byte>(second.data(), second.size()), d2));
    cook::String text2;
    cook::write_ui_document(d2, asset::parse_guid(kGuid).value(), text2);
    CHECK(std::string(text2.c_str()) == std::string(text.c_str()));

    // Data -> Document -> data: the same chunk too (what an editor saving a document writes).
    ui::Document doc;
    REQUIRE(ui::build_document(d, doc));
    ui::DocumentData back;
    back.white_region = d.white_region;
    ui::extract_document(doc, back);
    Array<byte> enc;
    ui::encode_ui_document(back, enc);
    CHECK(std::vector<byte>(enc.begin(), enc.end()) == first);
}

TEST_CASE("ui: a truncated or tampered chunk does not decode") {
    const std::vector<byte> bytes = cook_ui("ui_decode", kMenu);
    ui::DocumentData d;
    CHECK_FALSE(ui::decode_ui_document(Span<const byte>(bytes.data(), bytes.size() - 1), d));
    std::vector<byte> bad = bytes;
    bad[33] = byte{0xFF};  // widget_count past the limit and the payload
    CHECK_FALSE(ui::decode_ui_document(Span<const byte>(bad.data(), bad.size()), d));
}

TEST_CASE("ui: authoring mistakes fail the cook with file and line") {
    struct Bad {
        const char* widgets;
        const char* expect;
    };
    const Bad bad[] = {
        {R"([ { "name": "a", "kind": "Buton" } ])", "UiWidgetKind"},
        {R"([ { "name": "a" }, { "name": "a" } ])", "already named 'a'"},
        {R"([ { "name": "a/b" } ])", "no '/'"},
        {R"([ { "name": "a", "colour": 1 } ])", "colour"},
        {R"([ { "name": "a", "style": { "tnit": [1, 1, 1, 1] } } ])", "tnit"},
        {R"([ { "name": "a", "styles": { "Count": {} } } ])", "Normal, Hover"},
        {R"([ { "name": "a", "kind": "Slider", "range": [0, 1, 0.1], "value": 2 } ])", "outside the widget's range"},
        {R"([ { "name": "a", "nav": { "down": "nowhere" } } ])", "no widget at 'nowhere'"},
        {R"([ { "name": "a", "default_focus": true }, { "name": "b", "default_focus": true } ])",
         "already has default_focus"},
        {R"([ { "name": "a", "fill": true, "at": [0, 0, 1, 1] } ])", "one of"},
        {R"([ { "name": "a", "kind": "Text", "modal": true } ])", "goes on a Panel"},
        {R"([ { "name": "t", "kind": "Tabs", "value": 2, "tabs": [ { "name": "x" } ] } ])", "one of its pages"},
        {R"([ { "name": "t", "kind": "Tabs", "children": [ { "name": "bar", "kind": "Stack" } ] } ])",
         "Stack of tab buttons"},
        {R"([ { "name": "a", "anchors": { "to": "Window" } } ])", "enumerator"},
        {R"([ { "name": "a", "kind": "Text", "bind": "Score/points", "format": "%s" } ])", "number format"},
        {R"([ { "name": "a", "kind": "Text", "format": "%g" } ])", "needs a 'bind'"},
        {R"([ { "name": "a", "kind": "Text", "bind": "points" } ])", "<Component>/<field>"},
        {R"([ { "name": "a", "kind": "Button", "action": "" } ])", "action name"},
    };
    for (const Bad& b : bad) {
        CAPTURE(b.widgets);
        const std::string root = fresh_dir("ui_bad");
        std::string src = "{ \"format\": \"mce-ui\", \"version\": 1, \"guid\": \"";
        src += kGuid;
        src += "\",\n  \"widgets\": ";
        src += b.widgets;
        src += " }";
        write_file(root + "/assets/menu.mcui", src);
        cook::CookService svc;
        const cook::CookReport r = svc.cook(StringView(root.c_str()), cook::CookOptions{});
        CHECK_FALSE(r.ok());
        const std::string diags = ui_diags(r);
        CHECK(diags.find("menu.mcui:2:") != std::string::npos);  // the widgets are on line 2
        CHECK(diags.find(b.expect) != std::string::npos);
    }
    {  // a newer version names the version line
        const std::string root = fresh_dir("ui_bad");
        write_file(root + "/assets/menu.mcui",
                   std::string("{ \"format\": \"mce-ui\", \"guid\": \"") + kGuid + "\",\n\"version\": 2 }");
        cook::CookService svc;
        const cook::CookReport r = svc.cook(StringView(root.c_str()), cook::CookOptions{});
        CHECK_FALSE(r.ok());
        CHECK(ui_diags(r).find("menu.mcui:2:") != std::string::npos);
    }
}
