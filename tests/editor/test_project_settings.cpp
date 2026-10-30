// The project settings helpers the Project Settings panel and MCP share: which .mceproj a project uses,
// the budgets check, the budget a target resolves to, the scene's settings entity, the gizmo mode
// labels, and a .mcperf file read from disk.

#include <doctest/doctest.h>

#include <filesystem>
#include <fstream>
#include <string>

#include "cook_test_util.h"
#include "mce/cook/json.h"
#include "mce/editor/editor_scene.h"
#include "mce/editor/gizmo.h"
#include "mce/editor/perf_file.h"
#include "mce/editor/project.h"
#include "mce/editor/project_settings.h"
#include "mce/perf_budget.h"

using namespace mce;
using namespace mce::editor;
using namespace cook_test;

namespace {

StringView sv(const std::string& s) { return StringView(s.data(), s.size()); }

void write(const std::string& path, const std::string& text) {
    std::ofstream out(path, std::ios::binary | std::ios::trunc);
    out << text;
}

bool budgets_ok(const char* json, std::string* why = nullptr) {
    cook::JsonDoc doc;
    cook::JsonError je;
    REQUIRE(doc.parse(StringView(json), je));
    cook::String err;
    const bool ok = check_budgets(doc.root(), err);
    if (why) *why = std::string(err.view().data(), err.view().size());
    return ok;
}

}  // namespace

TEST_CASE("project settings: project_file_path picks the first .mceproj by name") {
    const std::string root = fresh_dir("project_settings_file");
    std::filesystem::create_directories(root + "/assets");
    EditorProject closed;
    CHECK(project_file_path(closed).empty());

    EditorProject project;
    REQUIRE(project.open(root));
    CHECK(project_file_path(project).empty());  // none yet
    write(root + "/zeta.mceproj", "{}");
    write(root + "/alpha.mceproj", "{}");
    write(root + "/notes.txt", "x");
    const cook::String path = project_file_path(project);
    const std::string got(path.view().data(), path.view().size());
    CHECK(got == root + "/alpha.mceproj");
}

TEST_CASE("project settings: check_budgets names what is wrong") {
    CHECK(budgets_ok("{ \"mobile\": { \"draws\": 300, \"frame_ms\": 33.3 } }"));
    CHECK(budgets_ok("{}"));
    std::string why;
    CHECK_FALSE(budgets_ok("[1]", &why));
    CHECK(why.find("object of targets") != std::string::npos);
    why.clear();
    CHECK_FALSE(budgets_ok("{ \"console\": {} }", &why));
    CHECK(why.find("'console' is not a target") != std::string::npos);
    why.clear();
    CHECK_FALSE(budgets_ok("{ \"mobile\": 3 }", &why));
    CHECK(why.find("budgets.mobile must be an object") != std::string::npos);
    why.clear();
    CHECK_FALSE(budgets_ok("{ \"mobile\": { \"fps\": 60 } }", &why));
    CHECK(why.find("'fps' is not a budget") != std::string::npos);
}

TEST_CASE("project settings: project_budget overlays the file on the target's default") {
    const std::string root = fresh_dir("project_settings_budget");
    std::filesystem::create_directories(root + "/assets");
    EditorProject project;
    REQUIRE(project.open(root));

    perf::Budget b = perf::default_budget(perf::Target::Mobile);
    cook::String err;
    CHECK(project_budget(project, perf::Target::Mobile, b, err));  // no file: the default stays
    CHECK(b.draws == perf::default_budget(perf::Target::Mobile).draws);

    write(root + "/game.mceproj", "{ \"budgets\": { \"mobile\": { \"draws\": 123 } } }");
    b = perf::default_budget(perf::Target::Mobile);
    REQUIRE(project_budget(project, perf::Target::Mobile, b, err));
    CHECK(b.draws == 123);
    perf::Budget web = perf::default_budget(perf::Target::Web);
    REQUIRE(project_budget(project, perf::Target::Web, web, err));
    CHECK(web.draws == perf::default_budget(perf::Target::Web).draws);  // another target is untouched

    write(root + "/game.mceproj", "{ \"budgets\": { \"mobile\": { \"fps\": 1 } } }");
    err.clear();
    CHECK_FALSE(project_budget(project, perf::Target::Mobile, b, err));
    CHECK_FALSE(err.empty());
    write(root + "/game.mceproj", "[1]");
    err.clear();
    CHECK_FALSE(project_budget(project, perf::Target::Mobile, b, err));
}

TEST_CASE("project settings: the settings entity is found, then created once") {
    EditorScene s;
    CHECK(world_settings_entity(s) == kNoEntity);
    const EntityId made = ensure_world_settings(s);
    CHECK(made != kNoEntity);
    CHECK(world_settings_entity(s) == made);
    const usize count = s.doc().entities().size();
    CHECK(ensure_world_settings(s) == made);  // again: the same one, nothing added
    CHECK(s.doc().entities().size() == count);
}

TEST_CASE("gizmo and perf file: mode labels, and a .mcperf that is missing or not a capture") {
    CHECK(StringView(gizmo_mode_label(GizmoMode::Move)) == StringView("Move"));
    CHECK(StringView(gizmo_mode_label(GizmoMode::Rotate)) == StringView("Rotate"));
    CHECK(StringView(gizmo_mode_label(GizmoMode::Scale)) == StringView("Scale"));

    const std::string root = fresh_dir("project_settings_perf");
    perf::Capture c;
    cook::String err;
    const std::string missing = root + "/none.mcperf";
    CHECK_FALSE(load_mcperf(sv(missing), c, err));
    CHECK(std::string(err.view().data(), err.view().size()).find("cannot read") != std::string::npos);

    const std::string bad = root + "/bad.mcperf";
    write(bad, "{ \"format\": \"something-else\" }");
    err.clear();
    CHECK_FALSE(load_mcperf(sv(bad), c, err));
    const std::string msg(err.view().data(), err.view().size());
    CHECK(msg.rfind(bad, 0) == 0);  // the message starts with the path
}
