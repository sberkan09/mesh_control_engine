// M-SCENETABS 1: the tab model over a headless session. A dirty scene is parked in its autosave file when another
// scene opens and comes back dirty with its edits; a clean one is re-read; closing a dirty tab keeps or drops the
// parked edits as asked; an untitled dirty scene is refused.

#include <doctest/doctest.h>

#include <filesystem>
#include <string>

#include "cook_test_util.h"
#include "mce/editor/scene_tabs.h"

using namespace mce;
using namespace mce::editor;

namespace {

StringView sv(const std::string& s) { return StringView(s.data(), s.size()); }

// A project with scenes/a.mcscene (one extra entity "A1") and scenes/b.mcscene saved, A open.
struct Fixture {
    std::string root;
    EditorSession session;
    SceneTabs tabs;

    explicit Fixture(const char* name) {
        root = cook_test::fresh_dir(name);
        std::filesystem::create_directories(root + "/assets/scenes");
        REQUIRE(session.open_project(sv(root)));
        session.new_scene("a");
        session.scene().create_entity("A1");
        REQUIRE(session.save_scene_as("scenes/a.mcscene"));
        session.new_scene("b");
        REQUIRE(session.save_scene_as("scenes/b.mcscene"));
        REQUIRE(session.open_scene("scenes/a.mcscene"));
        tabs.sync(session);
    }
};

}  // namespace

TEST_CASE("SceneTabs: a dirty scene survives a switch and comes back dirty with its edits") {
    Fixture f("scene_tabs_switch");
    REQUIRE(f.tabs.tabs().size() == 1);
    CHECK(f.tabs.active() == 0);
    const usize size_a = f.session.scene().doc().size();
    f.session.scene().create_entity("Unsaved");
    CHECK(f.tabs.dirty(f.session, 0));

    cook::String err;
    REQUIRE(f.tabs.open(f.session, "scenes/b.mcscene", err));
    REQUIRE(f.tabs.tabs().size() == 2);
    CHECK(f.tabs.active() == 1);
    CHECK_FALSE(f.session.scene().dirty());
    CHECK(f.tabs.dirty(f.session, 0));  // parked
    CHECK(f.tabs.any_dirty(f.session));

    REQUIRE(f.tabs.activate(f.session, 0, err));
    CHECK(f.tabs.active() == 0);
    CHECK(f.session.scene().dirty());
    CHECK(f.session.scene().doc().size() == size_a + 1);
    CHECK_FALSE(f.tabs.dirty(f.session, 1));
    CHECK(f.session.scene().verify_world());

    // Saving clears the dirt, and a later switch re-reads the saved file.
    REQUIRE(f.session.save_scene());
    REQUIRE(f.tabs.activate(f.session, 1, err));
    REQUIRE(f.tabs.activate(f.session, 0, err));
    CHECK_FALSE(f.session.scene().dirty());
    CHECK(f.session.scene().doc().size() == size_a + 1);
}

TEST_CASE("SceneTabs: an untitled dirty scene is refused, a bad path keeps the open scene") {
    Fixture f("scene_tabs_refuse");
    f.session.new_scene("draft");
    f.session.scene().create_entity("Draft");
    f.tabs.sync(f.session);
    CHECK(f.tabs.active() == -1);
    cook::String err;
    CHECK_FALSE(f.tabs.open(f.session, "scenes/a.mcscene", err));
    CHECK(err.view().find("save it first") != StringView::npos);
    CHECK(f.session.scene().dirty());

    REQUIRE(f.session.open_scene("scenes/a.mcscene"));
    f.tabs.sync(f.session);
    err = cook::String();
    CHECK_FALSE(f.tabs.open(f.session, "scenes/missing.mcscene", err));
    CHECK(f.tabs.active() == 0);
    CHECK(f.session.scene().path().find("a.mcscene") != StringView::npos);
}

TEST_CASE("SceneTabs: closing tabs keeps or drops parked edits as asked") {
    Fixture f("scene_tabs_close");
    cook::String err;
    f.session.scene().create_entity("Parked");
    REQUIRE(f.tabs.open(f.session, "scenes/b.mcscene", err));
    REQUIRE(f.tabs.tabs().size() == 2);

    // Closing the parked dirty tab with discard removes its autosave: reopening shows the file as saved.
    REQUIRE(f.tabs.close(f.session, 0, true, err));
    REQUIRE(f.tabs.tabs().size() == 1);
    CHECK(f.tabs.active() == 0);
    REQUIRE(f.tabs.open(f.session, "scenes/a.mcscene", err));
    CHECK_FALSE(f.session.scene().dirty());

    // Closing the active dirty tab needs `discard`.
    f.session.scene().create_entity("Again");
    CHECK_FALSE(f.tabs.close(f.session, static_cast<u32>(f.tabs.active()), false, err));
    CHECK(f.session.scene().dirty());
    REQUIRE(f.tabs.close(f.session, static_cast<u32>(f.tabs.active()), true, err));
    CHECK(f.tabs.tabs().size() == 1);
    CHECK(f.session.scene().path().find("b.mcscene") != StringView::npos);
    CHECK_FALSE(f.session.scene().dirty());

    // The last tab closes into an untitled scene.
    REQUIRE(f.tabs.close(f.session, 0, false, err));
    CHECK(f.tabs.tabs().size() == 0);
    CHECK(f.tabs.active() == -1);
    CHECK(f.session.scene().path().empty());
}
