// The M-AGENT tools (ROADMAP M-AGENT) in process against a copy of the scene_demo project. Cases in
// the "gpu" suite draw with the real player (CI: Xvfb + Mesa llvmpipe; the offscreen software path
// without a display) and register only with MCE_GPU_TESTS.

#include <doctest/doctest.h>

#include <cstdlib>
#include <filesystem>
#include <fstream>
#include <iterator>
#include <string>

#include "mcp_client.h"

using namespace mce;
using namespace mcp_client;
using namespace cook_test;

namespace {

const std::string kDemoAssets = std::string(MCE_SOURCE_DIR) + "/samples/scene_demo/assets";
const std::string kGoldens = std::string(MCE_SOURCE_DIR) + "/tests/mcp/goldens";

std::string demo_project(const char* name) {
    const std::string root = fresh_dir(name);
    std::filesystem::copy(kDemoAssets, root + "/assets", std::filesystem::copy_options::recursive);
    return root;
}

mcp::ServerOptions with_player() {
    mcp::ServerOptions opt;
    opt.player = MCE_PLAYER_EXE;
    return opt;
}

void write_png(const std::string& path, const cook::Image& img) {
    Array<byte> png;
    REQUIRE(cook::encode_png(img.width, img.height, img.rgba.span(), png));
    write_file(path, std::string(reinterpret_cast<const char*>(png.data()), png.size()));
}

cook::Image solid(u32 w, u32 h, u8 r, u8 g, u8 b) {
    cook::Image img;
    img.width = w;
    img.height = h;
    img.rgba.resize(usize(w) * h * 4);
    for (usize i = 0; i < img.rgba.size(); i += 4) {
        img.rgba[i] = r;
        img.rgba[i + 1] = g;
        img.rgba[i + 2] = b;
        img.rgba[i + 3] = 255;
    }
    return img;
}

bool same_pixels(const cook::Image& a, const cook::Image& b) {
    return a.width == b.width && a.height == b.height && a.rgba.size() == b.rgba.size() &&
           std::equal(a.rgba.begin(), a.rgba.end(), b.rgba.begin());
}

}  // namespace

TEST_CASE("mcp agent tools: listed, and arguments are checked before anything runs") {
    const std::string root = demo_project("mcp_agent_args");
    Client c(with_player());
    REQUIRE(c.server.open_project(sv(root), "scenes/demo.mcscene"));
    c.init();
    auto tools = c.request("tools/list");
    std::string names;
    const cook::JsonValue list = tools->root().get("result").get("tools");
    for (u32 i = 0; i < list.size(); ++i) names += str(list.at(i).get("name")) + " ";
    for (const char* t : {"render_frame", "render_sheet", "image_diff"}) CHECK(names.find(t) != std::string::npos);

    CHECK(c.fails("render_frame", R"({"tick":0})").find("'tick'") != std::string::npos);
    CHECK(c.fails("render_frame", R"({"width":10})").find("'width'") != std::string::npos);
    CHECK(c.fails("render_frame", R"({"scene":"scenes/none.mcscene"})").find("no such scene") != std::string::npos);
    CHECK(c.fails("render_frame", R"({"camera":{"entity":"Nobody"}})").find("no entity named") != std::string::npos);
    CHECK(c.fails("render_frame", R"({"camera":{"position":[0,1,2]}})").find("camera.position") != std::string::npos);
    CHECK(c.fails("render_frame", R"({"camera":{"fov":3}})").find("camera") != std::string::npos);
    CHECK(c.fails("render_frame", R"({"safe_area":{"top":-1}})").find("safe_area") != std::string::npos);
    CHECK(c.fails("render_frame", R"({"save":"../x.png"})").find("inside the project") != std::string::npos);
    CHECK(c.fails("render_frame", R"({"input":"x Jump"})").find("line 1") != std::string::npos);
    CHECK(c.fails("render_sheet", R"({"ticks":[5,3]})").find("ascending") != std::string::npos);
    CHECK(c.fails("render_sheet", R"({"ticks":[]})").find("ticks") != std::string::npos);
    CHECK(c.fails("image_diff", R"({"golden":"a.png"})").find("exactly one") != std::string::npos);
    CHECK(c.fails("image_diff", R"({"golden":"a.txt","image":"b.png"})").find("'golden'") != std::string::npos);
    // An unsaved edit: the frame is drawn from the cooked file, so it must be saved first.
    c.ok("entity_create", R"({"name":"Extra"})");
    CHECK(c.fails("render_frame").find("unsaved") != std::string::npos);
}

TEST_CASE("mcp agent tools: image_diff scores PNGs, writes a missing golden, and refuses size mismatches") {
    const std::string root = demo_project("mcp_agent_diff");
    Client c;
    REQUIRE(c.server.open_project(sv(root), "scenes/demo.mcscene"));
    c.init();
    cook::Image grey = solid(40, 30, 100, 100, 100);
    write_png(root + "/shots/a.png", grey);
    cook::Image changed = grey;
    for (u32 y = 0; y < 15; ++y)  // the top half brighter by 50
        for (u32 x = 0; x < 40; ++x) changed.rgba[(usize(y) * 40 + x) * 4] = 150;
    write_png(root + "/shots/b.png", changed);
    write_png(root + "/shots/small.png", solid(20, 10, 0, 0, 0));

    // No golden yet: the image becomes it.
    auto first = c.ok("image_diff", R"({"golden":"goldens/a.png","image":"shots/a.png"})");
    CHECK(first.v().get("written").as_bool());
    CHECK(std::filesystem::exists(root + "/goldens/a.png"));
    REQUIRE(first.images.size() == 1);
    CHECK(same_pixels(first.images[0], grey));

    auto same = c.ok("image_diff", R"({"golden":"goldens/a.png","image":"shots/a.png"})");
    CHECK(same.v().get("score").as_number() == 0);
    CHECK(same.v().get("pass").as_bool());

    auto diff = c.ok("image_diff", R"({"golden":"goldens/a.png","image":"shots/b.png","threshold":10})");
    CHECK(diff.v().get("changed_fraction").as_number() == doctest::Approx(0.5));
    CHECK(diff.v().get("max_channel_diff").as_number() == 50);
    CHECK(diff.v().get("score").as_number() == doctest::Approx(50.0 * 0.5 / (3 * 255)));
    CHECK_FALSE(diff.v().get("pass").as_bool());
    REQUIRE(diff.images.size() == 1);  // the heat map: changed pixels red
    CHECK(diff.images[0].rgba[0] > 128);
    CHECK(diff.images[0].rgba[(usize(29) * 40) * 4] < 64);
    // A looser bar passes; a high threshold sees no changed pixel.
    CHECK(c.ok("image_diff", R"({"golden":"goldens/a.png","image":"shots/b.png","max_score":0.05})").v().get("pass").as_bool());
    CHECK(c.ok("image_diff", R"({"golden":"goldens/a.png","image":"shots/b.png","threshold":60})").v().get("changed_fraction").as_number() == 0);

    CHECK(c.fails("image_diff", R"({"golden":"goldens/a.png","image":"shots/small.png"})").find("size mismatch") != std::string::npos);
    auto upd = c.ok("image_diff", R"({"golden":"goldens/a.png","image":"shots/b.png","update":true})");
    CHECK(upd.v().get("written").as_bool());
    CHECK(c.ok("image_diff", R"({"golden":"goldens/a.png","image":"shots/b.png"})").v().get("score").as_number() == 0);
}

#if defined(MCE_PROBE_MODULE) && !defined(_WIN32)
namespace {

// A stand-in build tree: the probe module copied under <dir>/out/, a CMakeCache.txt whose
// CMAKE_COMMAND is a script printing `cmake_output` (exit `cmake_rc`), and a ctest beside it.
std::string fake_build_tree(const char* name, const std::string& cmake_output, int cmake_rc, const std::string& ctest_output, int ctest_rc) {
    const std::string dir = fresh_dir(name);
    std::filesystem::create_directories(dir + "/out");
    std::filesystem::copy_file(MCE_PROBE_MODULE, dir + "/out/libtemplate_probe.so");
    auto script = [&](const std::string& path, const std::string& text, int rc) {
        write_file(path + ".txt", text);
        write_file(path, "#!/bin/sh\necho \"$@\" > " + path + ".args\ncat " + path + ".txt\nexit " + std::to_string(rc) + "\n");
        std::filesystem::permissions(path, std::filesystem::perms::owner_all);
    };
    script(dir + "/bin/cmake", cmake_output, cmake_rc);
    script(dir + "/bin/ctest", ctest_output, ctest_rc);
    write_file(dir + "/CMakeCache.txt", "# fake\nCMAKE_COMMAND:INTERNAL=" + dir + "/bin/cmake\n");
    return dir;
}

std::string read_all(const std::string& path) {
    std::ifstream f(path);
    return std::string(std::istreambuf_iterator<char>(f), std::istreambuf_iterator<char>());
}

}  // namespace

TEST_CASE("module_build: compiler errors as file:line:col JSON; a good build maps the module again and keeps the scene") {
    const std::string root = fresh_dir("mcp_agent_build_project");
    std::filesystem::copy(std::string(MCE_PROBE_PROJECT) + "/assets", root + "/assets", std::filesystem::copy_options::recursive);
    const std::string bad = fake_build_tree("mcp_agent_build_bad",
                                            "[1/2] Building CXX object game.cpp.o\n"
                                            "FAILED: game.cpp.o\n"
                                            "/p/games/x/module/x.cpp:12:5: error: 'Spinn' was not declared in this scope\n"
                                            "/p/games/x/module/x.cpp:12:5: note: suggested alternative: 'Spin'\n"
                                            "/p/games/x/module/x.h:3:1: warning: unused variable 'k' [-Wunused-variable]\n"
                                            "/p/games/x/module/x.cpp:12:5: error: 'Spinn' was not declared in this scope\n"
                                            "C:\\p\\x.cpp(40,7): error C2065: 'q': undeclared identifier\n"
                                            "ninja: build stopped: subcommand failed.\n",
                                            1, "", 0);
    {
        Client c;
        REQUIRE(c.server.load_module(sv(bad + "/out/libtemplate_probe.so")));
        REQUIRE(c.server.open_project(sv(root), "scenes/main.mcscene"));
        c.init();
        auto r = c.ok("module_build", R"({"warnings":true})");
        CHECK_FALSE(r.v().get("ok").as_bool());
        CHECK_FALSE(r.v().get("reloaded").as_bool());
        CHECK(str(r.v().get("target")) == "template_probe_game");
        CHECK(r.v().get("errors").as_number() == 2);  // the repeated one once
        CHECK(r.v().get("warnings").as_number() == 1);
        const cook::JsonValue d = r.v().get("diagnostics");
        REQUIRE(d.size() == 4);
        CHECK(str(d.at(0).get("file")) == "/p/games/x/module/x.cpp");
        CHECK(d.at(0).get("line").as_number() == 12);
        CHECK(d.at(0).get("col").as_number() == 5);
        CHECK(str(d.at(0).get("severity")) == "error");
        CHECK(str(d.at(1).get("severity")) == "note");
        CHECK(str(d.at(3).get("file")) == "C:\\p\\x.cpp");
        CHECK(d.at(3).get("line").as_number() == 40);
        CHECK(str(r.v().get("output_tail")).find("subcommand failed") != std::string::npos);
        CHECK(read_all(bad + "/bin/cmake.args").find("--build " + bad + " --target template_probe_game") != std::string::npos);
    }

    const std::string good = fake_build_tree("mcp_agent_build_good", "[1/1] Linking CXX shared module libtemplate_probe.so\n", 0,
                                             "Test project /x\n"
                                             "    Start 1: template_probe_headless_300\n"
                                             "1/2 Test  #1: template_probe_headless_300 ...   Passed    0.30 sec\n"
                                             "2/2 Test  #2: template_probe_gpu .............***Failed    0.10 sec\n",
                                             8);
    Client c;
    REQUIRE(c.server.load_module(sv(good + "/out/libtemplate_probe.so")));
    REQUIRE(c.server.open_project(sv(root), "scenes/main.mcscene"));
    c.init();
    auto before = c.ok("run_headless", R"({"ticks":60,"log_lines":0})");
    c.ok("entity_create", R"({"name":"Added","components":{"Spin":{}}})");  // unsaved work survives the reload
    auto r = c.ok("module_build");
    CHECK(r.v().get("ok").as_bool());
    CHECK(r.v().get("reloaded").as_bool());
    CHECK(r.v().get("diagnostics").size() == 0);
    CHECK(c.server.module().loaded());
    auto info = c.ok("scene_info");
    CHECK(info.v().get("dirty").as_bool());
    CHECK(c.ok("scene_get_entity", R"({"entity":"Added"})").text.find("Spin") != std::string::npos);
    CHECK(c.fails("scene_undo").find("nothing to undo") != std::string::npos);  // a fresh history
    c.ok("scene_open", R"({"path":"scenes/main.mcscene","force":true})");
    auto after = c.ok("run_headless", R"({"ticks":60,"log_lines":0})");
    CHECK(str(after.v().get("state_hash")) == str(before.v().get("state_hash")));  // the module's systems still run

    auto t = c.ok("module_test");
    CHECK_FALSE(t.v().get("ok").as_bool());
    CHECK(t.v().get("passed").as_number() == 1);
    CHECK(t.v().get("failed").as_number() == 1);
    CHECK(str(t.v().get("tests").at(1).get("name")) == "template_probe_gpu");
    CHECK(read_all(good + "/bin/ctest.args").find("-R ^template_probe_") != std::string::npos);

    Client none;  // no module: nothing to rebuild
    REQUIRE(none.server.open_project(sv(root)));
    none.init();
    CHECK(none.fails("module_build").find("no game module") != std::string::npos);
}
#endif

TEST_SUITE("gpu") {

TEST_CASE("render_frame: the player's frame at a tick, repeatable, from other cameras, checked against a golden") {
    const std::string root = demo_project("mcp_agent_render");
    Client c(with_player());
    REQUIRE(c.server.open_project(sv(root), "scenes/demo.mcscene"));
    c.init();
    auto a = c.ok("render_frame", R"({"tick":30,"width":320,"height":180,"save":"shots/t30.png"})");
    CHECK(a.v().get("tick").as_number() == 30);
    REQUIRE(a.images.size() == 1);
    CHECK(a.images[0].width == 320);
    CHECK(a.images[0].height == 180);
    CHECK(std::filesystem::exists(root + "/shots/t30.png"));
    // The capture frame shows the held tick itself: the same call gives the same pixels.
    auto again = c.ok("render_frame", R"({"tick":30,"width":320,"height":180})");
    CHECK(same_pixels(a.images[0], again.images[0]));
    // The offscreen software path (what a box without a display uses) draws the same frame.
    auto soft = c.ok("render_frame", R"({"tick":30,"width":320,"height":180,"software":true})");
    CHECK(str(soft.v().get("renderer")).find("software") != std::string::npos);
    CHECK(same_pixels(a.images[0], soft.images[0]));

    // Later ticks and scripted input change the frame; so does another camera (render only).
    auto later = c.ok("render_frame", R"({"tick":90,"width":320,"height":180})");
    CHECK_FALSE(same_pixels(a.images[0], later.images[0]));
    auto by_name = c.ok("render_frame", R"({"tick":30,"width":320,"height":180,"camera":{"entity":"Camera"}})");
    CHECK(same_pixels(a.images[0], by_name.images[0]));  // the scene's own camera, by name
    auto free2d = c.ok("render_frame", R"({"tick":30,"width":320,"height":180,"camera":{"center":[6,0],"zoom":0.5}})");
    CHECK_FALSE(same_pixels(a.images[0], free2d.images[0]));
    // A safe area: outlined on the returned image (cyan-ish edge at the inset).
    auto safe = c.ok("render_frame", R"({"tick":30,"width":320,"height":180,"safe_area":{"top":20}})");
    const u8* edge = &safe.images[0].rgba[(usize(20) * 320 + 160) * 4];
    CHECK(edge[2] > 200);

    // The golden (tests/mcp/goldens, Mesa llvmpipe): within 0.5 % mean difference. A missing golden
    // is written and passes; MCE_UPDATE_GOLDENS=1 rewrites it.
    const std::string golden = kGoldens + "/scene_demo_t30.png";
    const char* upd = std::getenv("MCE_UPDATE_GOLDENS");
    if (!std::filesystem::exists(golden) || (upd && *upd == '1')) {
        std::filesystem::create_directories(kGoldens);
        std::filesystem::copy_file(root + "/shots/t30.png", golden, std::filesystem::copy_options::overwrite_existing);
        MESSAGE("wrote the golden " << golden);
    }
    std::filesystem::create_directories(root + "/goldens");
    std::filesystem::copy_file(golden, root + "/goldens/scene_demo_t30.png", std::filesystem::copy_options::overwrite_existing);
    auto d = c.ok("image_diff", R"({"golden":"goldens/scene_demo_t30.png","render":{"tick":30,"width":320,"height":180},"max_score":0.005})");
    CHECK_MESSAGE(d.v().get("pass").as_bool(), "render_frame drifted from tests/mcp/goldens/scene_demo_t30.png: " << d.text);
}

TEST_CASE("render_sheet: ticks in columns, cameras in rows, one image") {
    const std::string root = demo_project("mcp_agent_sheet");
    Client c(with_player());
    REQUIRE(c.server.open_project(sv(root), "scenes/demo.mcscene"));
    c.init();
    auto s = c.ok("render_sheet", R"({"ticks":[1,30,60],"cell_width":160,"cell_height":90,"cameras":[{"entity":"Camera"},{"center":[0,0],"zoom":0.25}]})");
    CHECK(s.v().get("rows").as_number() == 2);
    CHECK(s.v().get("columns").as_number() == 3);
    REQUIRE(s.images.size() == 1);
    CHECK(s.images[0].width == 3 * 160 + 4 * 4);
    CHECK(s.images[0].height == 2 * 90 + 3 * 4);
}

}  // TEST_SUITE("gpu")
