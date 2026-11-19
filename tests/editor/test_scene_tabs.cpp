// M-SCENETABS 1: the tab model over a headless session. A dirty scene is parked in its autosave file when another
// scene opens and comes back dirty with its edits; a clean one is re-read; closing a dirty tab keeps or drops the
// parked edits as asked; an untitled dirty scene is refused.

#include <doctest/doctest.h>

#include <filesystem>
#include <map>
#include <random>
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

    // clear() forgets the list without touching the open scene.
    f.tabs.sync(f.session);
    f.tabs.clear();
    CHECK(f.tabs.tabs().size() == 0);
    CHECK(f.tabs.active() == -1);
    f.tabs.sync(f.session);
    CHECK(f.tabs.tabs().size() == 1);

    // The last tab closes into an untitled scene.
    REQUIRE(f.tabs.close(f.session, 0, false, err));
    CHECK(f.tabs.tabs().size() == 0);
    CHECK(f.tabs.active() == -1);
    CHECK(f.session.scene().path().empty());
}

// Random open / switch / edit / save / close sequences over three scenes. The model: a scene left by a switch comes
// back with the same entity count while its tab stays; the tab list never repeats a path; the active tab is the
// session's scene.
TEST_CASE("SceneTabs fuzz: switching keeps every scene's entities and the tab list consistent") {
    static const char* const files[] = {"scenes/a.mcscene", "scenes/b.mcscene", "scenes/c.mcscene"};
    for (u32 seed = 1; seed <= 60; ++seed) {
        CAPTURE(seed);
        std::mt19937 rng(seed);
        Fixture f("scene_tabs_fuzz");
        f.session.new_scene("c");
        REQUIRE(f.session.save_scene_as("scenes/c.mcscene"));
        REQUIRE(f.session.open_scene("scenes/a.mcscene"));
        f.tabs.sync(f.session);
        std::map<std::string, usize> expect;  // the entity count a scene had when it was left
        for (u32 step = 0; step < 25; ++step) {
            CAPTURE(step);
            cook::String err;
            const std::string here(f.session.scene().path().data(), f.session.scene().path().size());
            const u32 op = static_cast<u32>(rng() % 5);
            if (op == 0) {
                f.session.scene().create_entity("E");
            } else if (op == 1) {
                if (f.session.save_scene()) CHECK_FALSE(f.session.scene().dirty());
            } else if (op == 2 || op == 3) {
                const usize left = f.session.scene().doc().size();
                std::string target;
                if (op == 2) {
                    target = files[rng() % 3];
                } else if (!f.tabs.tabs().empty()) {
                    const u32 idx = static_cast<u32>(rng() % f.tabs.tabs().size());
                    target.assign(f.tabs.tabs()[idx].path.view().data(), f.tabs.tabs()[idx].path.view().size());
                }
                if (target.empty()) continue;
                const bool ok = op == 2 ? f.tabs.open(f.session, sv(target), err) : [&] {
                    for (u32 i = 0; i < f.tabs.tabs().size(); ++i)
                        if (f.tabs.tabs()[i].path.view() == sv(target)) return f.tabs.activate(f.session, i, err);
                    return false;
                }();
                if (!ok) {  // refused by design: a dirty untitled scene has no file to park beside
                    CHECK(here.empty());
                    CHECK(f.session.scene().dirty());
                    continue;
                }
                const std::string now(f.session.scene().path().data(), f.session.scene().path().size());
                CHECK_FALSE(now.empty());
                const bool moved = now != here;
                if (moved && !here.empty()) expect[here] = left;
                const auto it = expect.find(now);
                if (it != expect.end() && moved) CHECK(f.session.scene().doc().size() == it->second);
            } else if (!f.tabs.tabs().empty()) {
                const u32 idx = static_cast<u32>(rng() % f.tabs.tabs().size());
                const std::string gone(f.tabs.tabs()[idx].path.view().data(), f.tabs.tabs()[idx].path.view().size());
                if (f.tabs.close(f.session, idx, rng() % 2 == 0, err)) expect.erase(gone);
                f.tabs.sync(f.session);
            }
            f.tabs.sync(f.session);
            // The tab list: no path twice, and the active tab is the session's scene.
            for (u32 i = 0; i < f.tabs.tabs().size(); ++i)
                for (u32 j = i + 1; j < f.tabs.tabs().size(); ++j)
                    CHECK_FALSE(f.tabs.tabs()[i].path.view() == f.tabs.tabs()[j].path.view());
            if (f.tabs.active() >= 0) {
                REQUIRE(static_cast<u32>(f.tabs.active()) < f.tabs.tabs().size());
                CHECK(f.tabs.tabs()[static_cast<u32>(f.tabs.active())].path.view() == f.session.scene().path());
            }
            CHECK(f.session.scene().verify_world());
        }
    }
}
