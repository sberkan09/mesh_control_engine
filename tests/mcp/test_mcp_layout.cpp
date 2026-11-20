// entity_align, entity_distribute, entity_snap, entity_flip and entity_match_transform (M-LAYOUTTOOLS step 2) in
// process against a copy of the scene_demo project: each moves what its editor command moves, in one undo step, and
// bad arguments are named without changing the scene.

#include <doctest/doctest.h>

#include <cmath>
#include <filesystem>
#include <string>

#include "mcp_client.h"

using namespace mce;
using namespace mcp_client;
using namespace cook_test;

namespace {

std::string layout_project(const char* name) {
    const std::string root = fresh_dir(name);
    std::filesystem::copy(std::string(MCE_SOURCE_DIR) + "/samples/scene_demo/assets", root + "/assets",
                          std::filesystem::copy_options::recursive);
    return root;
}

int make(Client& c, const char* name, double x, double y) {
    const std::string args = std::string(R"({"name":")") + name + R"(","components":{"LocalTransform":{"position":[)" +
                             std::to_string(x) + "," + std::to_string(y) + R"(,0]}}})";
    return static_cast<int>(c.ok("entity_create", args).v().get("id").as_number());
}

double pos(Client& c, int id, u32 axis) {
    auto e = c.ok("scene_get_entity", "{\"entity\":" + std::to_string(id) + "}");
    const cook::JsonValue p = e.v().get("components").get("LocalTransform").get("position");
    return p.valid() ? p.at(axis).as_number() : 0.0;  // a default position is left out of the entity's JSON
}

}  // namespace

TEST_CASE("layout tools: align, distribute, snap, flip and match move the entities, each as one undo step") {
    const std::string root = layout_project("mcp_layout");
    Client c;
    REQUIRE(c.server.open_project(sv(root), "scenes/demo.mcscene"));
    c.init();
    const int a = make(c, "LA", 1, 5), b = make(c, "LB", 4.2, -2), d = make(c, "LC", 10, 3), e = make(c, "LD", 7, 0);
    const std::string all = "[" + std::to_string(a) + "," + std::to_string(b) + "," + std::to_string(d) + "]";

    CHECK(c.ok("entity_align", R"({"entities":)" + all + R"(,"axis":"x","mode":"min"})").v().get("changed").as_bool());
    CHECK(pos(c, b, 0) == doctest::Approx(1.0));
    CHECK(pos(c, d, 0) == doctest::Approx(1.0));
    CHECK(pos(c, b, 1) == doctest::Approx(-2.0));  // the other axis stays
    CHECK_FALSE(
        c.ok("entity_align", R"({"entities":)" + all + R"(,"axis":"x","mode":"min"})").v().get("changed").as_bool());
    c.ok("scene_undo");
    CHECK(pos(c, b, 0) == doctest::Approx(4.2));

    CHECK(c.ok("entity_distribute", "{\"entities\":[" + std::to_string(a) + "," + std::to_string(e) + "," +
                                        std::to_string(b) + "," + std::to_string(d) + R"(],"axis":"x"})")
              .v()
              .get("changed")
              .as_bool());
    CHECK(pos(c, a, 0) == doctest::Approx(1.0));
    CHECK(pos(c, d, 0) == doctest::Approx(10.0));
    CHECK(pos(c, b, 0) == doctest::Approx(4.0));
    CHECK(pos(c, e, 0) == doctest::Approx(7.0));

    CHECK(c.ok("entity_snap", R"({"entities":[)" + std::to_string(a) + R"(],"step":2})").v().get("moved").as_number() ==
          1);
    CHECK(pos(c, a, 1) == doctest::Approx(6.0));

    CHECK(c.ok("entity_flip", R"({"entities":[)" + std::to_string(b) + "," + std::to_string(d) + R"(],"axis":"x"})")
              .v()
              .get("changed")
              .as_bool());
    CHECK(pos(c, b, 0) == doctest::Approx(10.0));
    CHECK(pos(c, d, 0) == doctest::Approx(4.0));

    CHECK(c.ok("entity_match_transform", R"({"entities":[)" + std::to_string(b) + "," + std::to_string(d) +
                                             R"(],"anchor":"LA","part":"position"})")
              .v()
              .get("changed")
              .as_number() == 2);
    CHECK(pos(c, b, 0) == doctest::Approx(pos(c, a, 0)));
    CHECK(pos(c, d, 1) == doctest::Approx(pos(c, a, 1)));
}

TEST_CASE("layout tools name a bad argument and change nothing") {
    const std::string root = layout_project("mcp_layout_bad");
    Client c;
    REQUIRE(c.server.open_project(sv(root), "scenes/demo.mcscene"));
    c.init();
    const int a = make(c, "LA", 1, 5), b = make(c, "LB", 4, -2);
    const std::string two = "[" + std::to_string(a) + "," + std::to_string(b) + "]";
    CHECK(c.fails("entity_align", R"({"entities":)" + two + R"(,"axis":"z","mode":"min"})").find("axis") !=
          std::string::npos);
    CHECK(c.fails("entity_align", R"({"entities":)" + two + R"(,"axis":"x","mode":"left"})").find("mode") !=
          std::string::npos);
    CHECK(c.fails("entity_align", R"({"entities":[1],"axis":"x","mode":"min"})").find("entities") != std::string::npos);
    CHECK(c.fails("entity_distribute", R"({"entities":)" + two + R"(,"axis":"x"})").find("entities") !=
          std::string::npos);
    CHECK(c.fails("entity_snap", R"({"entities":)" + two + R"(,"step":0})").find("step") != std::string::npos);
    CHECK(c.fails("entity_flip", R"({"entities":["Nobody"],"axis":"x"})").find("Nobody") != std::string::npos);
    CHECK(
        c.fails("entity_match_transform", R"({"entities":)" + two + R"(,"anchor":"LA","part":"size"})").find("part") !=
        std::string::npos);
    CHECK(pos(c, a, 0) == doctest::Approx(1.0));
    CHECK(pos(c, b, 0) == doctest::Approx(4.0));
}

TEST_CASE("entity_array_duplicate copies a group into a grid as one undo step") {
    const std::string root = layout_project("mcp_layout_array");
    Client c;
    REQUIRE(c.server.open_project(sv(root), "scenes/demo.mcscene"));
    c.init();
    const int a = make(c, "LA", 1, 2);
    const double before = c.ok("scene_info").v().get("entities").as_number();
    auto r = c.ok("entity_array_duplicate",
                  "{\"entities\":[" + std::to_string(a) + R"(],"columns":3,"rows":2,"dx":10,"dy":-4})");
    CHECK(r.v().get("created").as_number() == 5);
    REQUIRE(r.v().get("ids").size() == 5);
    CHECK(c.ok("scene_info").v().get("entities").as_number() == before + 5);
    const int last = static_cast<int>(r.v().get("ids").at(4).as_number());  // column 2, row 1
    CHECK(pos(c, last, 0) == doctest::Approx(21.0));
    CHECK(pos(c, last, 1) == doctest::Approx(-2.0));
    CHECK(pos(c, a, 0) == doctest::Approx(1.0));
    c.ok("scene_undo");
    CHECK(c.ok("scene_info").v().get("entities").as_number() == before);

    CHECK(c.fails("entity_array_duplicate", "{\"entities\":[" + std::to_string(a) + R"(],"columns":1})")
              .find("at least 2") != std::string::npos);
    CHECK(c.fails("entity_array_duplicate", "{\"entities\":[" + std::to_string(a) + R"(],"columns":5000,"dx":1})")
              .find("columns") != std::string::npos);
    CHECK(c.fails("entity_array_duplicate", "{\"entities\":[" + std::to_string(a) + R"(],"columns":100,"rows":100})")
              .find("4096") != std::string::npos);
    CHECK(c.fails("entity_array_duplicate", "{\"entities\":[" + std::to_string(a) + R"(],"columns":2,"dx":"far"})")
              .find("dx") != std::string::npos);
    CHECK(c.ok("scene_info").v().get("entities").as_number() == before);
}

// M-HIERTOOLS 1: entity_set_parent, entity_rename, entity_group, entity_ungroup, entity_reset_transform.
TEST_CASE("hierarchy tools: reparent keeps the world place, group and ungroup undo, reset and rename work") {
    const std::string root = layout_project("mcp_hier");
    Client c;
    REQUIRE(c.server.open_project(sv(root), "scenes/demo.mcscene"));
    c.init();
    const int p = make(c, "HP", 10, 0), a = make(c, "HA", 3, 4), b = make(c, "HB", 5, 6);
    auto id_s = [](int v) { return std::to_string(v); };

    CHECK(c.ok("entity_set_parent", "{\"entity\":" + id_s(a) + ",\"parent\":" + id_s(p) + "}")
              .v()
              .get("changed")
              .as_bool());
    CHECK(pos(c, a, 0) == doctest::Approx(-7.0));  // local: the world x of 3 under a parent at 10
    CHECK_FALSE(
        c.ok("entity_set_parent", "{\"entity\":" + id_s(a) + ",\"parent\":\"HP\"}").v().get("changed").as_bool());
    CHECK(c.fails("entity_set_parent", "{\"entity\":" + id_s(p) + ",\"parent\":" + id_s(a) + "}")
              .find("itself or of one of its own") != std::string::npos);
    CHECK(c.ok("entity_set_parent", "{\"entity\":" + id_s(a) + "}").v().get("changed").as_bool());  // back to the root
    CHECK(pos(c, a, 0) == doctest::Approx(3.0));

    CHECK(c.ok("entity_rename", "{\"entity\":" + id_s(b) + R"(,"name":"Renamed"})").v().get("changed").as_bool());
    CHECK(str(c.ok("scene_get_entity", R"({"entity":"Renamed"})").v().get("name")) == "Renamed");
    CHECK(c.fails("entity_rename", "{\"entity\":" + id_s(b) + R"(,"name":""})").find("name") != std::string::npos);

    const double before = c.ok("scene_info").v().get("entities").as_number();
    auto g = c.ok("entity_group", "{\"entities\":[" + id_s(a) + "," + id_s(b) + "]}");
    const int group = static_cast<int>(g.v().get("group").as_number());
    CHECK(c.ok("scene_info").v().get("entities").as_number() == before + 1);
    auto un = c.ok("entity_ungroup", "{\"entities\":[" + id_s(group) + "]}");
    CHECK(un.v().get("freed").size() == 2);
    CHECK(c.ok("scene_info").v().get("entities").as_number() == before);
    CHECK(pos(c, a, 0) == doctest::Approx(3.0));

    CHECK(c.ok("entity_reset_transform", "{\"entities\":[" + id_s(a) + "," + id_s(b) + "]}")
              .v()
              .get("changed")
              .as_number() == 2);
    CHECK(pos(c, a, 0) == doctest::Approx(0.0));
    CHECK(c.fails("entity_group", R"({"entities":[]})").find("entities") != std::string::npos);
}
