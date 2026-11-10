// M-SOAK step 3: the editor's scene lifecycle must stay flat. Opening a scene, editing it, undoing the edits and
// switching to another scene 200 times leaves the live heap allocation count where the first round left it.

#include <doctest/doctest.h>

#include <filesystem>
#include <string>

#include "cook_test_util.h"
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
