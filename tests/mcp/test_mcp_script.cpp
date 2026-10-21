// The scripting tools (M-SCRIPT step 4) in process against a copy of the scene_demo project:
// script_check reports what the cook would (names with a "did you mean", types) with line and col,
// the cook writes cooked/mce.d.luau, and run_headless / test_run report a scene script that failed.

#include <doctest/doctest.h>

#include <filesystem>
#include <string>

#include "mcp_client.h"

using namespace mce;
using namespace mcp_client;
using namespace cook_test;

namespace {

constexpr const char* kFailGuid = "5c000000-0000-4000-8000-0000000000c1";
constexpr const char* kWorkGuid = "5c000000-0000-4000-8000-0000000000c2";

std::string script_project(const char* name) {
    const std::string root = fresh_dir(name);
    std::filesystem::copy(std::string(MCE_SOURCE_DIR) + "/samples/scene_demo/assets", root + "/assets",
                          std::filesystem::copy_options::recursive);
    write_file(root + "/assets/scripts/fail.luau", "function update()\n    local t = {}\n    error(\"boom at tick \" .. mce.tick())\nend\n");
    write_file(root + "/assets/scripts/fail.luau.mceasset", sidecar(kFailGuid, "script"));
    return root;
}

}  // namespace

TEST_CASE("script_check: the cook's checks on one script, every problem with its position") {
    const std::string root = script_project("mcp_script_check");
    Client c;
    REQUIRE(c.server.open_project(sv(root), "scenes/demo.mcscene"));
    c.init();

    auto bad = c.ok("script_check", R"({"source":"function update()\n    mce.each(\"LocalTransfrom\", function(e) end)\n    mce.spwan({})\nend\n"})");
    CHECK_FALSE(bad.v().get("ok").as_bool());
    const cook::JsonValue problems = bad.v().get("problems");
    REQUIRE(problems.size() == 2);
    CHECK(problems.at(0).get("line").as_number() == 2);
    CHECK(problems.at(0).get("col").as_number() == 14);
    CHECK(str(problems.at(0).get("message")) == "unknown component 'LocalTransfrom' (did you mean 'LocalTransform'?)");
    CHECK(problems.at(1).get("line").as_number() == 3);
    CHECK(str(problems.at(1).get("message")).find("spwan") != std::string::npos);

    auto file = c.ok("script_check", R"({"path":"scripts/fail.luau","definitions":true})");
    CHECK(file.v().get("ok").as_bool());
    CHECK(str(file.v().get("file")) == "scripts/fail.luau");
    CHECK(str(file.v().get("definitions")).find("declare mce: {") != std::string::npos);

    auto prefixed = c.ok("script_check", R"({"path":"assets/scripts/fail.luau"})");  // as the asset_* tools print it
    CHECK(str(prefixed.v().get("file")) == "scripts/fail.luau");

    CHECK(c.fails("script_check", "{}").find("either 'path'") != std::string::npos);
    CHECK(c.fails("script_check", R"({"path":"scenes/demo.mcscene"})").find("not a .luau") != std::string::npos);
    CHECK(c.fails("script_check", R"({"path":"scripts/nope.luau"})").find("no such file") != std::string::npos);
}

TEST_CASE("run_headless: a scene script that fails is reported with its file and line") {
    const std::string root = script_project("mcp_script_run");
    Client c;
    REQUIRE(c.server.open_project(sv(root), "scenes/demo.mcscene"));
    c.init();
    c.ok("entity_create", std::string(R"({"name":"Script: fail","transform":false,"components":{"ScriptSystem":{"script":"guid:)") + kFailGuid +
                              R"("}}})");
    c.ok("scene_save");
    auto run = c.ok("run_headless", R"({"ticks":5,"log_lines":0})");
    const cook::JsonValue errors = run.v().get("script_errors");
    REQUIRE(errors.size() == 1);
    CHECK(str(errors.at(0).get("error")).find("scripts/fail.luau:3: boom at tick") != std::string::npos);
    CHECK(std::filesystem::exists(root + "/cooked/mce.d.luau"));

    // Without the failing script there is no script_errors key.
    c.ok("scene_undo");
    c.ok("scene_save");
    CHECK_FALSE(c.ok("run_headless", R"({"ticks":5,"log_lines":0})").v().get("script_errors").valid());
}

TEST_CASE("test_run: a script check pins a scene script's step profile") {
    const std::string root = script_project("mcp_script_steps");
    write_file(root + "/assets/scripts/work.luau", "function update()\n    local n = 0\n    for i = 1, 20 do n = n + i end\nend\n");
    write_file(root + "/assets/scripts/work.luau.mceasset", sidecar(kWorkGuid, "script"));
    Client c;
    REQUIRE(c.server.open_project(sv(root), "scenes/demo.mcscene"));
    c.init();
    c.ok("entity_create", std::string(R"({"name":"Script: work","transform":false,"components":{"ScriptSystem":{"script":"guid:)") + kWorkGuid +
                              R"("}}})");
    c.ok("scene_save");
    auto r = c.ok("test_run", R"({"test":{"name":"steps","scene":"scenes/demo.mcscene","ticks":5,"assert":[)"
                              R"({"script":"work.luau","metric":"updates",">=":3},)"
                              R"({"script":"scripts/work.luau","metric":"peak_steps","<=":500},)"
                              R"({"script":"work.luau","metric":"peak_steps","<=":5},)"
                              R"({"script":"work.luau","metric":"avg_steps",">":0},)"
                              R"({"script":"nope.luau","metric":"updates",">=":0}]}})");
    const cook::JsonValue checks = r.v().get("results").at(0).get("checks");
    CHECK(checks.at(0).get("ok").as_bool());
    CHECK(checks.at(1).get("ok").as_bool());
    CHECK_FALSE(checks.at(2).get("ok").as_bool());  // 20 loop steps are more than 5
    CHECK(checks.at(3).get("ok").as_bool());
    CHECK_FALSE(checks.at(4).get("ok").as_bool());
    CHECK(str(checks.at(4).get("note")).find("scripts/work.luau") != std::string::npos);
    CHECK(str(checks.at(0).get("check")) == "script work.luau updates");
    CHECK(c.fails("test_run", R"({"test":{"name":"x","scene":"scenes/demo.mcscene","assert":[{"script":"work.luau","metric":"steps","<=":1}]}})")
              .find("peak_steps | avg_steps") != std::string::npos);
    CHECK(c.fails("test_run", R"({"test":{"name":"x","scene":"scenes/demo.mcscene","assert":[{"script":"work.luau","metric":"updates"}]}})")
              .find("needs a comparison") != std::string::npos);
}
