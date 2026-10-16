// The timeline tools (M-CINEMA step 6) in process against a copy of the scene_demo project:
// timeline_create / timeline_add_track / timeline_add_key keep a .mctimeline the cook reads, and
// (the "gpu" suite) an agent-authored cutscene seen through timeline_preview at several moments and
// through render_sheet as it plays, with the saved files untouched.

#include <doctest/doctest.h>

#include <algorithm>
#include <filesystem>
#include <string>

#include "mce/cook/timeline_text.h"
#include "mcp_client.h"

using namespace mce;
using namespace mcp_client;
using namespace cook_test;

namespace {

std::string timeline_project(const char* name) {
    const std::string root = fresh_dir(name);
    std::filesystem::copy(std::string(MCE_SOURCE_DIR) + "/samples/scene_demo/assets", root + "/assets",
                          std::filesystem::copy_options::recursive);
    return root;
}

cook::TimelineDocument doc_of(const std::string& path) {
    const std::string text = read_file(path);
    cook::JsonDoc doc;
    cook::JsonError err;
    REQUIRE(doc.parse(sv(text), err));
    cook::TimelineDocument d;
    cook::DiagList diags;
    REQUIRE(cook::read_timeline_document(doc.root(), "intro.mctimeline", d, diags));
    return d;
}

}  // namespace

TEST_CASE("timeline tools: create, add tracks and keys; the file stays canonical and cooks") {
    const std::string root = timeline_project("mcp_timeline_edit");
    Client c;
    REQUIRE(c.server.open_project(sv(root), "scenes/demo.mcscene"));
    c.init();
    auto made = c.ok("timeline_create", R"({"path":"cutscenes/intro.mctimeline","name":"intro","tracks":[
        {"type":"subtitle","lines":[{"t":1,"duration":2,"text":"Who's there?","speaker":"Mira"}]}]})");
    CHECK(str(made.v().get("guid")).rfind("guid:", 0) == 0);
    CHECK(made.v().get("length").as_number() == doctest::Approx(3.0));
    const std::string file = root + "/assets/cutscenes/intro.mctimeline";
    REQUIRE(std::filesystem::exists(file));
    CHECK(c.fails("timeline_create", R"({"path":"cutscenes/intro.mctimeline","tracks":[{"type":"event","keys":[{"t":0,"emit":"a"}]}]})")
              .find("already exists") != std::string::npos);
    CHECK(c.fails("timeline_create", R"({"path":"cutscenes/empty.mctimeline"})").find("'tracks'") != std::string::npos);
    CHECK(c.fails("timeline_create", R"({"path":"cutscenes/bad.mctimeline","tracks":[{"type":"wobble"}]})").find("unknown track type") !=
          std::string::npos);
    CHECK_FALSE(std::filesystem::exists(root + "/assets/cutscenes/bad.mctimeline"));

    // A transform track first, then keys merged in by time.
    c.ok("timeline_add_track", R"({"timeline":"cutscenes/intro.mctimeline","index":0,"track":{"type":"transform","keys":[
        {"t":0,"position":[0,8,0]},{"t":4,"position":[0,2,0],"ease":"smooth"}]}})");
    auto k = c.ok("timeline_add_key", R"({"timeline":"cutscenes/intro.mctimeline","track":0,"key":{"t":2,"position":[3,5,0]}})");
    CHECK(k.v().get("length").as_number() == doctest::Approx(4.0));
    c.ok("timeline_add_key", R"({"timeline":"cutscenes/intro.mctimeline","track":1,"keys":[
        {"t":0.5,"duration":0.5,"text":"..."},{"t":3.5,"duration":1,"text":"Only the wind."}]})");
    cook::TimelineDocument d = doc_of(file);
    REQUIRE(d.tracks.size() == 2);
    CHECK(d.tracks[0].type == cook::TimelineDocTrackType::Transform);
    REQUIRE(d.tracks[0].keys.size() == 3);
    CHECK(d.tracks[0].keys[1].t == 2.0f);
    CHECK(d.tracks[0].keys[1].position[0] == 3.0f);
    REQUIRE(d.tracks[1].keys.size() == 3);
    CHECK(d.tracks[1].keys[0].name.view() == "...");
    CHECK(d.tracks[1].keys[2].name.view() == "Only the wind.");
    CHECK(d.length() == doctest::Approx(4.5f));

    // What was written is the canonical form: writing the read document gives the same text.
    cook::String again;
    const std::string text = read_file(file);
    cook::JsonDoc jd;
    cook::JsonError err;
    REQUIRE(jd.parse(sv(text), err));
    const Result<asset::Guid> g = asset::parse_guid(jd.root().get("guid").as_string());
    REQUIRE(g);
    cook::write_timeline_document(d, g.value(), again);
    CHECK(std::string(again.view().data(), again.view().size()) == text);

    // Mistakes fail with the cook's message and leave the file as it was.
    CHECK(c.fails("timeline_add_key", R"({"timeline":"cutscenes/intro.mctimeline","track":5,"key":{"t":1}})").find("'track'") !=
          std::string::npos);
    CHECK(c.fails("timeline_add_key", R"({"timeline":"cutscenes/intro.mctimeline","track":0})").find("exactly one") != std::string::npos);
    CHECK(c.fails("timeline_add_track",
                  R"({"timeline":"cutscenes/intro.mctimeline","track":{"type":"property","path":"Nope.value","keys":[]}})")
              .find("Nope") != std::string::npos);
    CHECK(c.fails("timeline_add_track", R"({"timeline":"scenes/demo.mcscene","track":{"type":"event"}})").find("not a timeline") !=
          std::string::npos);
    CHECK(read_file(file) == text);
    CHECK(c.ok("cook_run").v().get("failed").as_number() == 0);
}

TEST_SUITE("gpu") {

// An agent writes a cutscene with the tools alone and looks at it: the title slides down under a
// cutscene's letterbox; each moment draws the same frame twice and different moments differ.
TEST_CASE("timeline_preview: an agent-authored cutscene seen at several moments") {
    const std::string root = timeline_project("mcp_timeline_preview");
    Client c([] {
        mcp::ServerOptions opt;
        opt.player = MCE_PLAYER_EXE;
        return opt;
    }());
    REQUIRE(c.server.open_project(sv(root), "scenes/demo.mcscene"));
    c.init();
    auto made = c.ok("timeline_create", R"({"path":"cutscenes/intro.mctimeline","name":"intro","tracks":[{"type":"transform","keys":[
        {"t":0,"position":[0,8,0]},{"t":4,"position":[0,1,0],"ease":"smooth"}]}]})");
    const std::string guid = str(made.v().get("guid"));
    c.ok("timeline_add_track", R"({"timeline":"cutscenes/intro.mctimeline","track":{"type":"subtitle","lines":[
        {"t":2,"duration":2,"text":"The engine wakes."}]}})");
    c.ok("component_add", std::string(R"({"entity":"Title","component":"TimelinePlayer","value":{"timeline":")") + guid + R"("}})");
    c.ok("component_add", R"({"entity":"Title","component":"Cutscene"})");
    c.ok("scene_save");
    const std::string scene_file = root + "/assets/scenes/demo.mcscene", timeline_file = root + "/assets/cutscenes/intro.mctimeline";
    const std::string scene_before = read_file(scene_file), timeline_before = read_file(timeline_file);
    CHECK(c.fails("timeline_preview", R"({"time":-1})").find("'time'") != std::string::npos);

    const char* const at0 = R"({"time":0,"width":320,"height":240})";
    auto a = c.ok("timeline_preview", at0);
    REQUIRE(a.images.size() == 1);
    CHECK(a.v().get("time").as_number() == 0.0);
    auto a2 = c.ok("timeline_preview", at0);
    auto same = [](const auto& x, const auto& y) {
        return x.rgba.size() == y.rgba.size() && std::equal(x.rgba.begin(), x.rgba.end(), y.rgba.begin());
    };
    CHECK(same(a.images[0], a2.images[0]));
    auto b = c.ok("timeline_preview", R"({"time":3,"width":320,"height":240,"save":"shots/intro_3s.png"})");
    CHECK_FALSE(same(a.images[0], b.images[0]));
    CHECK(std::filesystem::exists(root + "/shots/intro_3s.png"));
    // The letterbox: the top rows are black while the cutscene shows.
    const auto& img = b.images[0];
    u32 dark = 0;
    for (u32 x = 0; x < img.width; ++x) {
        const usize i = static_cast<usize>(x) * 4;
        if (img.rgba[i] < 8 && img.rgba[i + 1] < 8 && img.rgba[i + 2] < 8) ++dark;
    }
    CHECK(dark == img.width);
    // render_frame plays the cutscene from its start: the frame at tick 1 is the one at time 0.
    auto f = c.ok("render_frame", R"({"tick":1,"width":320,"height":240})");
    CHECK_FALSE(same(f.images[0], b.images[0]));
    // render_sheet sees it play: three moments in one image, one column each.
    auto sheet = c.ok("render_sheet", R"({"ticks":[1,120,200],"cell_width":160,"cell_height":120})");
    REQUIRE(sheet.images.size() == 1);
    CHECK(sheet.images[0].width >= 3 * 160);
    // Previewing never touches the saved scene or the timeline.
    CHECK(read_file(scene_file) == scene_before);
    CHECK(read_file(timeline_file) == timeline_before);
}

// timeline_preview over two scenes back to back (the cook inside each call rescans the project): each reply names
// the scene that was drawn.
TEST_CASE("timeline_preview: two scenes back to back each echo their own scene") {
    const std::string root = timeline_project("mcp_timeline_preview_pair");
    Client c([] {
        mcp::ServerOptions opt;
        opt.player = MCE_PLAYER_EXE;
        return opt;
    }());
    REQUIRE(c.server.open_project(sv(root), "scenes/demo.mcscene"));
    c.init();
    c.ok("scene_save", R"({"path":"scenes/zz_copy.mcscene"})");
    for (int i = 0; i < 2; ++i) {
        auto a = c.ok("timeline_preview", R"({"time":0,"scene":"scenes/zz_copy.mcscene","width":160,"height":120,"software":true})");
        CHECK(str(a.v().get("scene")) == "scenes/zz_copy.mcscene");
        auto b = c.ok("timeline_preview", R"({"time":0,"scene":"scenes/demo.mcscene","width":160,"height":120,"software":true})");
        CHECK(str(b.v().get("scene")) == "scenes/demo.mcscene");
    }
}

}  // TEST_SUITE("gpu")
