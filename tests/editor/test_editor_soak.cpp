// M-SOAK step 3: the editor's scene lifecycle must stay flat. Opening a scene, editing it, undoing the edits and
// switching to another scene 200 times leaves the live heap allocation count where the first round left it.

#include <doctest/doctest.h>

#include <chrono>
#include <filesystem>
#include <fstream>
#include <string>

#include "cook_test_util.h"
#include "mce/asset/formats.h"
#include "mce/editor/editor_session.h"
#include "mce/memory/allocator.h"

using namespace mce;
using namespace mce::editor;

namespace {
StringView sv(const std::string& s) { return StringView(s.data(), s.size()); }
}  // namespace

TEST_CASE(
    "editor soak: 200 rounds of open, edit, undo and switch leave the live allocation count where round one left it") {
    const std::string root = cook_test::fresh_dir("editor_soak");
    std::filesystem::create_directories(root + "/assets/scenes");
    EditorSession session;
    REQUIRE(session.open_project(sv(root)));
    session.new_scene("a");
    for (int i = 0; i < 20; ++i) session.scene().create_entity("A");
    REQUIRE(session.save_scene_as("scenes/a.mcscene"));
    session.new_scene("b");
    REQUIRE(session.save_scene_as("scenes/b.mcscene"));

    auto round = [&] {
        REQUIRE(session.open_scene("scenes/a.mcscene"));
        const EntityId e = session.scene().create_entity("Extra");
        session.scene().create_entity("Extra2");
        session.scene().destroy_entity(e);
        while (session.scene().undo()) {
        }
        REQUIRE(session.open_scene("scenes/b.mcscene"));
    };

    round();  // the first round fills the caches that fill once
    round();
    const usize baseline = mem::heap_live_allocations();
    for (int i = 0; i < 200; ++i) round();
    const usize after = mem::heap_live_allocations();
    INFO("live allocations: " << baseline << " after the warm-up, " << after << " after 200 more rounds");
    CHECK(after <= baseline + 8);
}

TEST_CASE(
    "editor soak: 200 hot reloads of an edited material leave the live allocation count where round one left it") {
    const std::string root = cook_test::fresh_dir("editor_soak_reload");
    std::filesystem::create_directories(root + "/assets/materials");
    const std::string path = root + "/assets/materials/paint.mcmat";
    auto put = [&](int i) {
        std::ofstream f(path, std::ios::binary | std::ios::trunc);
        f << "{ \"format\": \"mce-material\", \"version\": 1, \"guid\": \"6f1c2a3b-4d5e-4f60-8a7b-9c0d1e2f3a4b\", "
             "\"base_color\": [1, 0.5, 0.25, 1], \"roughness\": 0."
          << (100 + i % 800) << " }\n";
        f.close();
        // A new mtime on every write, so the watcher sees the edit even when the size stays the same.
        std::filesystem::last_write_time(path,
                                         std::filesystem::file_time_type::clock::now() + std::chrono::seconds(2 * i));
    };
    EditorSession session;
    REQUIRE(session.open_project(sv(root)));
    session.tick_hot_reload(0.0, 0.0);
    put(0);
    REQUIRE(session.tick_hot_reload(1.0, 0.0));  // a new asset: fresh mount
    f64 now = 1.0;
    int step = 0;
    auto round = [&] {
        put(++step);
        now += 1.0;
        REQUIRE(session.tick_hot_reload(now, 0.0));
    };

    round();  // the first rounds fill the caches that fill once
    round();
    const usize baseline = mem::heap_live_allocations();
    for (int i = 0; i < 200; ++i) round();
    const usize after = mem::heap_live_allocations();
    INFO("live allocations: " << baseline << " after the warm-up, " << after << " after 200 more rounds");
    CHECK(after <= baseline + 8);
}

TEST_CASE("editor soak: 100 play and stop rounds leave the live allocation count where round one left it") {
    const std::string root = cook_test::fresh_dir("editor_soak_play");
    std::filesystem::copy(std::string(MCE_SOURCE_DIR) + "/samples/scene_demo/assets", root + "/assets",
                          std::filesystem::copy_options::recursive);
    EditorSession session;
    REQUIRE(session.open_project(sv(root)));
    REQUIRE(session.open_scene("scenes/demo.mcscene"));
    PlaySession& play = session.play();

    auto round = [&] {
        REQUIRE(session.enter_play());
        while (play.ticks() < 30) play.advance(1.0 / 60.0);
        session.stop_play();
        CHECK_FALSE(play.active());
    };

    round();  // the first rounds fill the caches that fill once
    round();
    const usize baseline = mem::heap_live_allocations();
    for (int i = 0; i < 100; ++i) round();
    const usize after = mem::heap_live_allocations();
    INFO("live allocations: " << baseline << " after the warm-up, " << after << " after 100 more rounds");
    CHECK(after <= baseline + 8);
}
