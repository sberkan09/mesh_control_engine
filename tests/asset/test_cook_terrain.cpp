// M-TERRAIN 1: a .mcterrain cooks to CHNK_TERRAIN (asset::TerrainView); authoring mistakes are cook
// errors naming the file.

#include <doctest/doctest.h>

#include "cook_test_util.h"
#include "mce/asset/asset_system.h"
#include "mce/asset/formats.h"
#include "mce/cook/cook.h"

using namespace mce;
using namespace cook_test;

namespace {

constexpr const char* kGuid = "7e7a0000-0000-4000-8000-0000000000a1";

std::string diags_of(const cook::CookReport& r) {
    std::string s;
    for (const auto& d : r.diags.all()) {
        cook::String line;
        cook::format_diagnostic(d, line);
        s += line.c_str();
        s += "\n";
    }
    return s;
}

std::string base64(const std::string& bytes) {
    static const char* k = "ABCDEFGHIJKLMNOPQRSTUVWXYZabcdefghijklmnopqrstuvwxyz0123456789+/";
    std::string out;
    size_t i = 0;
    for (; i + 2 < bytes.size(); i += 3) {
        const u32 v = (u32(u8(bytes[i])) << 16) | (u32(u8(bytes[i + 1])) << 8) | u8(bytes[i + 2]);
        for (int s = 18; s >= 0; s -= 6) out.push_back(k[(v >> s) & 63]);
    }
    if (i < bytes.size()) {
        u32 v = u32(u8(bytes[i])) << 16;
        if (i + 1 < bytes.size()) v |= u32(u8(bytes[i + 1])) << 8;
        out.push_back(k[(v >> 18) & 63]);
        out.push_back(k[(v >> 12) & 63]);
        out.push_back(i + 1 < bytes.size() ? k[(v >> 6) & 63] : '=');
        out.push_back('=');
    }
    return out;
}

std::string doc(const std::string& fields) {
    return std::string(R"({ "format": "mce-terrain", "version": 1, "guid": ")") + kGuid + "\", " + fields + " }";
}

}  // namespace

TEST_CASE("terrain: .mcterrain cooks to CHNK_TERRAIN with its heights") {
    const std::string root = fresh_dir("terrain_cook");
    // A 33 x 33 ramp along x: sample value x * 2000.
    std::string bytes;
    for (u32 z = 0; z < 33; ++z)
        for (u32 x = 0; x < 33; ++x) put16le(bytes, static_cast<u16>(x * 2000));
    write_file(root + "/assets/terrain/ramp.mcterrain",
               doc(R"("name": "ramp", "resolution": 33, "chunk": 16, "size": [64, 32], "height_range": [-5, 60.535], "heights": ")" +
                   base64(bytes) + "\""));
    write_file(root + "/assets/terrain/flat.mcterrain",
               R"({ "format": "mce-terrain", "version": 1, "guid": "7e7a0000-0000-4000-8000-0000000000a2",
                    "resolution": 65, "size": [10, 10], "height_range": [0, 10], "fill": 2.5 })");
    cook::CookService svc;
    const cook::CookReport r = svc.cook(StringView(root.c_str()), cook::CookOptions{});
    INFO(diags_of(r));
    REQUIRE(r.ok());

    asset::AssetSystem assets;
    const std::string cooked = root + "/cooked";
    const StringView roots[] = {StringView(cooked.c_str())};
    REQUIRE(assets.mount(Span<const StringView>(roots, 1)));
    const asset::AssetKey key = assets.find(asset::parse_guid(kGuid).value());
    REQUIRE(key != asset::kInvalidAssetKey);
    CHECK(assets.info(key).type == asset::ChunkType::Terrain);
    auto h = assets.load_sync<asset::Terrain>(key);
    REQUIRE(h);
    auto v = asset::TerrainView::read(assets.resolve(h.value()));
    REQUIRE(v);
    const asset::TerrainView& t = v.value();
    CHECK(t.resolution() == 33);
    CHECK(t.header->chunk == 16);
    CHECK(t.spacing_x() == doctest::Approx(2.0f));
    CHECK(t.spacing_z() == doctest::Approx(1.0f));
    CHECK(t.sample(0, 0) == doctest::Approx(-5.0f));
    CHECK(t.sample(10, 7) == doctest::Approx(-5.0f + 20000.0f / 65535.0f * 65.535f));
    // Bilinear between columns 16 and 17 (x = 0 is the middle of the 64 m span).
    CHECK(t.height_at(1.0f, 0.0f) == doctest::Approx(0.5f * (t.sample(16, 16) + t.sample(17, 16))));
    CHECK(t.height_at(-1000.0f, 3.0f) == doctest::Approx(t.sample(0, 0)));  // clamped to the edge

    auto fh = assets.load_sync<asset::Terrain>(assets.find(asset::parse_guid("7e7a0000-0000-4000-8000-0000000000a2").value()));
    REQUIRE(fh);
    auto fv = asset::TerrainView::read(assets.resolve(fh.value()));
    REQUIRE(fv);
    CHECK(fv.value().header->chunk == 32);  // the default
    CHECK(fv.value().sample(40, 3) == doctest::Approx(2.5f).epsilon(0.001));
}

TEST_CASE("terrain: authoring mistakes are cook errors") {
    const std::string bad[] = {
        doc(R"("resolution": 100, "size": [10, 10], "height_range": [0, 1])"),                  // not 2^n + 1
        doc(R"("resolution": 8193, "size": [10, 10], "height_range": [0, 1])"),                 // too large
        doc(R"("resolution": 33, "chunk": 12, "size": [10, 10], "height_range": [0, 1])"),      // chunk not a power of two
        doc(R"("resolution": 33, "size": [0, 10], "height_range": [0, 1])"),                    // empty size
        doc(R"("resolution": 33, "size": [10, 10], "height_range": [5, 1])"),                   // inverted range
        doc(R"("resolution": 33, "size": [10, 10], "height_range": [0, 1], "heights": "AAAA"))"),  // wrong byte count
        doc(R"("resolution": 33, "size": [10, 10], "height_range": [0, 1], "heights": "@@@@"))"),  // not base64
        doc(R"("resolution": 33, "size": [10, 10], "height_range": [0, 1], "fill": 3)"),        // fill outside the range
        doc(R"("resolution": 33, "size": [10, 10], "height_range": [0, 1], "heigths": "")"),    // a typo'd key
    };
    for (const std::string& text : bad) {
        const std::string root = fresh_dir("terrain_bad");
        write_file(root + "/assets/t.mcterrain", text);
        cook::CookService svc;
        const cook::CookReport r = svc.cook(StringView(root.c_str()), cook::CookOptions{});
        INFO(text);
        CHECK_FALSE(r.ok());
        CHECK(diags_of(r).find("t.mcterrain") != std::string::npos);
    }
}

TEST_CASE("terrain: TerrainView rejects payloads that do not hold together") {
    asset::TerrainHeader h{};
    h.resolution = 33;
    h.chunk = 16;
    h.size[0] = h.size[1] = 10.0f;
    h.height_min = 0.0f;
    h.height_max = 1.0f;
    Array<u16> heights;
    heights.resize(33 * 33, u16{0});
    Array<byte> payload;
    asset::write_terrain(h, heights.span(), payload);
    CHECK(asset::TerrainView::read(payload.span()));
    CHECK_FALSE(asset::TerrainView::read(payload.span().subspan(0, payload.size() - 64)));  // truncated samples
    asset::TerrainHeader odd = h;
    odd.resolution = 34;
    asset::write_terrain(odd, heights.span(), payload);
    CHECK_FALSE(asset::TerrainView::read(payload.span()));
    asset::TerrainHeader chunky = h;
    chunky.chunk = 64;  // larger than the terrain
    asset::write_terrain(chunky, heights.span(), payload);
    CHECK_FALSE(asset::TerrainView::read(payload.span()));
}
