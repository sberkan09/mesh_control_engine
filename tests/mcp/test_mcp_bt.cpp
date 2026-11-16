// The behavior tree tools (M-AI step 6) in process against a copy of the ai_demo project: bt_create /
// bt_add_node / bt_set keep a .mcbt the cook reads, written canonically, with nodes named by their
// pre-order index; mistakes fail with the cook's message and leave the file as it was; and bt_trace
// plays the demo and reports the guard hearing, spotting and chasing the thief.

#include <doctest/doctest.h>

#include <cmath>
#include <filesystem>
#include <string>

#include "mce/cook/bt_text.h"
#include "mcp_client.h"

using namespace mce;
using namespace mcp_client;
using namespace cook_test;

namespace {

std::string bt_project(const char* name) {
    const std::string root = fresh_dir(name);
    std::filesystem::copy(std::string(MCE_SOURCE_DIR) + "/samples/ai_demo/assets", root + "/assets",
                          std::filesystem::copy_options::recursive);
    return root;
}

cook::BtDocument doc_of(const std::string& path) {
    const std::string text = read_file(path);
    cook::JsonDoc doc;
    cook::JsonError err;
    REQUIRE(doc.parse(sv(text), err));
    cook::BtDocument d;
    cook::DiagList diags;
    REQUIRE(cook::read_behavior_tree_document(doc.root(), "sentry.mcbt", d, diags));
    return d;
}

std::string node_line(const Client::Tool& r, u32 i) { return str(r.v().get("nodes").at(i)); }

}  // namespace

TEST_CASE("bt tools: create, add nodes and set them; the file stays canonical and cooks") {
    const std::string root = bt_project("mcp_bt_edit");
    Client c;
    REQUIRE(c.server.open_project(sv(root), "scenes/yard.mcscene"));
    c.init();
    auto made = c.ok("bt_create", R"({"path":"ai/sentry.mcbt","name":"sentry",
        "blackboard":{"target":"entity","alert":{"type":"bool","default":false}},
        "root":{"selector":[{"task":"log","text":"idle"}]}})");
    CHECK(str(made.v().get("guid")).rfind("guid:", 0) == 0);
    REQUIRE(made.v().get("nodes").size() == 2);
    CHECK(node_line(made, 0) == "0 selector");
    CHECK(node_line(made, 1) == "1   task \"log\"");
    CHECK(str(made.v().get("keys").at(1)) == "alert: bool = false");
    const std::string file = root + "/assets/ai/sentry.mcbt";
    REQUIRE(std::filesystem::exists(file));
    CHECK(c.fails("bt_create", R"({"path":"ai/sentry.mcbt"})").find("already exists") != std::string::npos);
    CHECK(c.fails("bt_create", R"({"path":"ai/bad.mcbt","root":{"check":"nobody"}})").find("nobody") != std::string::npos);
    CHECK(c.fails("bt_create", R"({"path":"ai/bad.mcbt","root":{"dance":1}})").find("kind") != std::string::npos);
    CHECK_FALSE(std::filesystem::exists(root + "/assets/ai/bad.mcbt"));
    // Without a root: a selector over a wait.
    CHECK(c.ok("bt_create", R"({"path":"ai/blank.mcbt"})").v().get("nodes").size() == 2);

    // A chase in front, then a guard turned into an observer with bt_set.
    auto added = c.ok("bt_add_node", R"({"tree":"ai/sentry.mcbt","index":0,
        "node":{"guard":{"task":"move_to","key":"target","radius":1},"key":"target","name":"chase"}})");
    REQUIRE(added.v().get("nodes").size() == 4);
    CHECK(node_line(added, 1) == "1   guard  \"chase\"");
    CHECK(node_line(added, 2) == "2     task \"move_to\"");
    c.ok("bt_set", R"({"tree":"ai/sentry.mcbt","node":1,"fields":{"abort":"lower"}})");
    c.ok("bt_set", R"({"tree":"ai/sentry.mcbt","node":3,"fields":{"text":"bored"}})");
    c.ok("bt_add_node", R"({"tree":"ai/sentry.mcbt","node":{"wait":2}})");
    c.ok("bt_set", R"({"tree":"ai/sentry.mcbt","node":4,"value":3.5})");
    c.ok("bt_set", R"({"tree":"ai/sentry.mcbt","blackboard":{"home":"vec3"},"name":"sentry2"})");
    cook::BtDocument d = doc_of(file);
    CHECK(d.name.view() == "sentry2");
    REQUIRE(d.keys.size() == 3);
    REQUIRE(d.nodes.size() == 5);
    CHECK(d.nodes[1].field("abort") == "\"lower\"");
    CHECK(d.nodes[3].field("text") == "\"bored\"");
    CHECK(d.nodes[4].value.view() == "3.5");

    // What was written is the canonical form.
    const std::string text = read_file(file);
    cook::JsonDoc jd;
    cook::JsonError err;
    REQUIRE(jd.parse(sv(text), err));
    const Result<asset::Guid> g = asset::parse_guid(jd.root().get("guid").as_string());
    REQUIRE(g);
    cook::String again;
    cook::write_behavior_tree_document(d, g.value(), again);
    CHECK(std::string(again.view().data(), again.view().size()) == text);

    // Mistakes fail with the cook's message and leave the file as it was.
    CHECK(c.fails("bt_set", R"({"tree":"ai/sentry.mcbt","node":1,"fields":{"key":"nobody"}})").find("nobody") != std::string::npos);
    CHECK(c.fails("bt_set", R"({"tree":"ai/sentry.mcbt","blackboard":{"target":null}})").find("target") != std::string::npos);
    CHECK(c.fails("bt_set", R"({"tree":"ai/sentry.mcbt","node":0,"remove":true})").find("root") != std::string::npos);
    CHECK(c.fails("bt_set", R"({"tree":"ai/sentry.mcbt","node":0,"value":1})").find("leaf") != std::string::npos);
    CHECK(c.fails("bt_set", R"({"tree":"ai/sentry.mcbt","node":9,"value":1})").find("'node'") != std::string::npos);
    CHECK(c.fails("bt_add_node", R"({"tree":"ai/sentry.mcbt","parent":3,"node":{"wait":1}})").find("leaf") != std::string::npos);
    CHECK(c.fails("bt_add_node", R"({"tree":"ai/sentry.mcbt","parent":1,"node":{"wait":1}})").find("decorator") != std::string::npos);
    CHECK(c.fails("bt_add_node", R"({"tree":"scenes/yard.mcscene","node":{"wait":1}})").find("not a behavior tree") != std::string::npos);
    CHECK(read_file(file) == text);

    // Remove and replace.
    c.ok("bt_set", R"({"tree":"ai/sentry.mcbt","node":4,"remove":true})");
    auto replaced = c.ok("bt_set", R"({"tree":"ai/sentry.mcbt","node":0,"replace":{"sequence":[{"wait":1},{"emit":"done"}]}})");
    REQUIRE(replaced.v().get("nodes").size() == 3);
    CHECK(node_line(replaced, 0) == "0 sequence");
    CHECK(c.ok("cook_run").v().get("failed").as_number() == 0);
}

TEST_CASE("bt_trace: the demo's guard hears the noise, spots the thief and chases it") {
    const std::string root = bt_project("mcp_bt_trace");
    Client c;
    REQUIRE(c.server.open_project(sv(root), "scenes/yard.mcscene"));
    c.init();
    auto t = c.ok("bt_trace", R"({"entity":"Guard","ticks":1200,"every":60})");
    CHECK(str(t.v().get("tree")) == "ai/guard.mcbt");
    const cook::JsonValue rows = t.v().get("rows");
    REQUIRE(rows.size() >= 20);
    CHECK(rows.at(0).get("tick").as_number() == 1);
    bool patrolled = false, saw = false;
    for (u32 i = 0; i < rows.size(); ++i) {
        const std::string running = str(rows.at(i).get("running"));
        const std::string bb = str(rows.at(i).get("blackboard"));
        patrolled |= running.find("move_to") != std::string::npos;
        saw |= bb.find("sees=true") != std::string::npos;
    }
    CHECK(patrolled);
    CHECK(saw);
    std::string started;
    const cook::JsonValue s = t.v().get("started");
    for (u32 i = 0; i < s.size(); ++i) started += str(s.at(i)) + "\n";
    CHECK(started.find("\"chase\"") != std::string::npos);
    CHECK(started.find("\"investigate\"") != std::string::npos);
    // The same run twice reports the same thing.
    auto again = c.ok("bt_trace", R"({"entity":"Guard","ticks":1200,"every":60})");
    CHECK(again.text == t.text);

    CHECK(c.fails("bt_trace", R"({"entity":"Floor"})").find("AiBrain") != std::string::npos);
}

TEST_CASE("perception_debug_lines: the guard's eye, forward direction, cone lines and what it sees") {
    const std::string root = bt_project("mcp_perception_lines");
    Client c;
    REQUIRE(c.server.open_project(sv(root), "scenes/yard.mcscene"));
    c.init();
    auto r = c.ok("perception_debug_lines", R"({"ticks":1})");
    const cook::JsonValue ps = r.v().get("perceivers");
    REQUIRE(ps.size() >= 1);
    bool found = false;
    for (u32 i = 0; i < ps.size(); ++i) {
        const cook::JsonValue p = ps.at(i);
        if (str(p.get("name")) != "Guard") continue;
        found = true;
        const cook::JsonValue f = p.get("forward");
        REQUIRE(f.size() == 3);
        const double fx = f.at(0).as_number(), fy = f.at(1).as_number(), fz = f.at(2).as_number();
        CHECK(std::abs(fx * fx + fy * fy + fz * fz - 1.0) < 1e-3);
        // yaw_deg is the forward direction about +Y: 0 looks along -Z, 90 along -X.
        const double yaw = p.get("yaw_deg").as_number() * 3.14159265358979 / 180.0;
        CHECK(std::abs(fx + std::sin(yaw)) < 1e-3);
        CHECK(std::abs(fz + std::cos(yaw)) < 1e-3);
        CHECK(p.get("sight_range").as_number() == 7);
        CHECK(p.get("sight_angle").as_number() == 140);
        CHECK(str(p.get("space")) == "Space3D");
        CHECK(p.get("eye").size() == 3);
    }
    CHECK(found);
    const cook::JsonValue lines = r.v().get("lines");
    REQUIRE(lines.size() >= 3);  // two cone edges and the arc
    CHECK(lines.at(0).get("from").size() == 3);
    CHECK(lines.at(0).get("color").size() == 4);
    // Played long enough the guard has seen the thief: a line to its target appears.
    auto later = c.ok("perception_debug_lines", R"({"ticks":1200})");
    CHECK(later.v().get("ticks").as_number() == 1200);
    // The same run twice reports the same thing.
    CHECK(c.ok("perception_debug_lines", R"({"ticks":1200})").text == later.text);
}
