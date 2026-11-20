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
    return e.v().get("components").get("LocalTransform").get("position").at(axis).as_number();
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
