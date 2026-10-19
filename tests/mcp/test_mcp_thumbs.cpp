// M-THUMBS 2: the asset_thumbnail tool. A generated texture, sound and mesh in one project: one PNG per asset at
// the asked size, a contact sheet with the exact geometry (columns, rows, gap), the plain tile for a scene, the
// type filter, argument errors, and the same picture on a second call.

#include <doctest/doctest.h>

#include <cstring>
#include <filesystem>
#include <string>

#include "mcp_client.h"

using namespace mce;
using namespace mcp_client;
using namespace cook_test;

namespace {

bool same_pixels(const cook::Image& a, const cook::Image& b) {
    return a.width == b.width && a.height == b.height && a.rgba.size() == b.rgba.size() &&
           std::memcmp(a.rgba.data(), b.rgba.data(), a.rgba.size()) == 0;
}

u32 distinct_pixels(const cook::Image& img) {
    u32 n = 0;
    for (usize i = 4; i + 3 < img.rgba.size(); i += 4) n += std::memcmp(&img.rgba[i], &img.rgba[0], 4) != 0 ? 1u : 0u;
    return n;
}

}  // namespace

TEST_CASE("asset_thumbnail: one asset, a contact sheet, the plain tile, filters and errors") {
    const std::string root = fresh_dir("mcp_thumbs");
    std::filesystem::create_directories(root + "/assets");
    Client c;
    REQUIRE(c.server.open_project(sv(root)));
    c.init();
    c.ok("gen_texture", R"({"kind":"worley","width":32,"height":16,"colors":["#000000","#ffffff"],"save":"art/rock.png"})");
    c.ok("gen_sfx", R"({"seed":3,"preset":"laser","save":"sfx/zap.wav"})");
    c.ok("gen_mesh", R"({"shape":"box","size":[1,1,1],"color":"#d03020","save":"art/crate.glb"})");
    c.ok("asset_create", R"({"path":"mats/gold.mcmat","content":{"base_color":[1,0.7,0.1,1],"metallic":1,"roughness":0.25}})");
    c.ok("scene_new", R"({"name":"empty"})");
    c.ok("scene_save", R"({"path":"scenes/empty.mcscene"})");
    REQUIRE(c.ok("cook_run").v().get("ok").as_bool());

    const auto tex = c.ok("asset_thumbnail", R"({"path":"art/rock.png","size":64})");
    REQUIRE(tex.images.size() == 1);
    CHECK(tex.images[0].width == 64);
    CHECK(tex.images[0].height == 64);
    CHECK(tex.v().get("real").as_bool());
    CHECK(distinct_pixels(tex.images[0]) > 200);  // noise over a letterboxed 2:1 picture
    const auto again = c.ok("asset_thumbnail", R"({"path":"art/rock.png","size":64})");
    CHECK(same_pixels(tex.images[0], again.images[0]));

    const auto wav = c.ok("asset_thumbnail", R"({"path":"sfx/zap.wav","size":48})");
    REQUIRE(wav.images.size() == 1);
    CHECK(wav.images[0].width == 48);
    CHECK(wav.v().get("real").as_bool());

    const auto mesh = c.ok("asset_thumbnail", R"({"path":"art/crate.glb","size":64})");
    REQUIRE(mesh.images.size() == 1);
    CHECK(mesh.v().get("real").as_bool());
    CHECK(distinct_pixels(mesh.images[0]) > 300);

    const auto mat = c.ok("asset_thumbnail", R"({"path":"mats/gold.mcmat","size":64})");
    REQUIRE(mat.images.size() == 1);
    CHECK(mat.v().get("real").as_bool());
    CHECK(distinct_pixels(mat.images[0]) > 1500);  // the lit sphere on the dark background

    const auto tile = c.ok("asset_thumbnail", R"({"path":"scenes/empty.mcscene","size":32})");
    CHECK_FALSE(tile.v().get("real").as_bool());
    CHECK(tile.images[0].width == 32);

    // A contact sheet: n assets in ceil(sqrt(n)) columns, tile and a 2 pixel gap.
    const auto sheet = c.ok("asset_thumbnail", R"({"folder":"","size":32})");
    REQUIRE(sheet.images.size() == 1);
    const u32 n = static_cast<u32>(sheet.v().get("assets").size());
    CHECK(n >= 4);
    const u32 cols = static_cast<u32>(sheet.v().get("columns").as_number());
    const u32 rows = static_cast<u32>(sheet.v().get("rows").as_number());
    CHECK(cols * rows >= n);
    CHECK(sheet.images[0].width == 2 + cols * 34);
    CHECK(sheet.images[0].height == 2 + rows * 34);
    CHECK(sheet.v().get("truncated").as_bool() == false);

    const auto only = c.ok("asset_thumbnail", R"({"folder":"art/","type":"Texture","size":32})");
    CHECK(only.v().get("assets").size() == 1);
    CHECK(only.images[0].width == 2 + 34);
    CHECK(c.ok("asset_thumbnail", R"({"folder":"","limit":2,"size":32})").v().get("truncated").as_bool());

    CHECK(c.fails("asset_thumbnail", R"({})").find("one of") != std::string::npos);
    CHECK(c.fails("asset_thumbnail", R"({"path":"art/rock.png","folder":""})").find("one of") != std::string::npos);
    CHECK(c.fails("asset_thumbnail", R"({"path":"nope.png"})").find("no registered asset") != std::string::npos);
    CHECK(c.fails("asset_thumbnail", R"({"folder":"zzz/"})").find("no registered asset") != std::string::npos);
    CHECK(c.fails("asset_thumbnail", R"({"path":"art/rock.png","size":8})").find("size") != std::string::npos);
}
