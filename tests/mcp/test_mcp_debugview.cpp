// M-DEBUGVIEW 2: the debug_lines and debug_view tools. A scene with a ground box, a trigger and a character that
// falls into it: exact line counts per category, the trigger lit once the character is inside, the category
// filter, an identical answer on a second call, and the CPU-drawn PNG for each view.

#include <doctest/doctest.h>

#include <filesystem>
#include <string>

#include "mcp_client.h"

using namespace mce;
using namespace mcp_client;
using namespace cook_test;

namespace {

void build(Client& c) {
    c.ok("scene_new", R"({"name":"yard"})");
    c.ok("entity_create", R"({"name":"Ground","components":{"LocalTransform":{"position":[0,-0.5,0]},"BoxCollider3D":{"half":[10,0.5,10]}}})");
    c.ok("entity_create", R"({"name":"Pad","components":{"LocalTransform":{"position":[0,1,0]},"Trigger3D":{"half":[1,1,1]}}})");
    c.ok("entity_create", R"({"name":"Walker","components":{"LocalTransform":{"position":[0,0.3,0]},"CharacterController3D":{}}})");
    c.ok("scene_save", R"({"path":"scenes/yard.mcscene"})");
    REQUIRE(c.ok("cook_run").v().get("ok").as_bool());
}

double count(const Client::Tool& r, const char* category) { return r.v().get("counts").get(category).as_number(); }

}  // namespace

TEST_CASE("debug_lines and debug_view: exact counts, a lit trigger, filters and the CPU picture") {
    const std::string root = fresh_dir("mcp_debugview");
    std::filesystem::create_directories(root + "/assets");
    Client c;
    REQUIRE(c.server.open_project(sv(root)));
    c.init();
    build(c);

    const auto all = c.ok("debug_lines", R"({"ticks":120})");
    CHECK(count(all, "colliders") == 12);
    CHECK(count(all, "triggers") == 12);
    CHECK(count(all, "characters") == 30);
    CHECK(all.v().get("total").as_number() == 54);
    CHECK(all.v().get("truncated").as_bool() == false);
    // The walker stands in the pad: its edges are green.
    bool lit = false;
    const cook::JsonValue lines = all.v().get("lines");
    for (u32 i = 0; i < lines.size(); ++i)
        if (str(lines.at(i).get("category")) == "triggers") lit = lines.at(i).get("color").at(1).as_number() > 0.9;
    CHECK(lit);

    const auto only = c.ok("debug_lines", R"({"ticks":120,"categories":["triggers"]})");
    CHECK(only.v().get("total").as_number() == 12);
    CHECK(c.ok("debug_lines", R"({"ticks":120})").text == all.text);  // the same run reads the same
    CHECK(c.fails("debug_lines", R"({"categories":["nope"]})").find("category") != std::string::npos);

    // The picture is a golden by count: the pixels that are not background, per view (a repeatable rasteriser).
    const struct {
        const char* view;
        u32 expected;
    } kViews[] = {{"top", 2002}, {"front", 2000}, {"side", 1888}};
    for (const auto& kv : kViews) {
        const char* view = kv.view;
        const auto v = c.ok("debug_view", std::string(R"({"ticks":120,"width":256,"height":192,"view":")") + view + R"("})");
        REQUIRE(v.images.size() == 1);
        CHECK(v.images[0].width == 256);
        CHECK(v.images[0].height == 192);
        CHECK(v.v().get("lines").as_number() == 54);
        u32 drawn = 0;
        for (usize p = 0; p + 3 < v.images[0].rgba.size(); p += 4)
            if (v.images[0].rgba[p] != 0x20 || v.images[0].rgba[p + 1] != 0x20) ++drawn;
        CHECK(drawn == kv.expected);
    }
}
