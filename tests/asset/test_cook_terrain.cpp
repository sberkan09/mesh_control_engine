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

TEST_CASE("terrain: splat layers cook with their materials, weights and auto rules (M-TERRAIN 2)") {
    const std::string root = fresh_dir("terrain_layers");
    write_file(root + "/assets/materials/grass.mcmat",
               R"({ "format": "mce-material", "version": 1, "guid": "7e7a0000-0000-4000-8000-0000000000b1", "base_color": [0.3, 0.6, 0.2, 1] })");
    write_file(root + "/assets/materials/rock.mcmat",
               R"({ "format": "mce-material", "version": 1, "guid": "7e7a0000-0000-4000-8000-0000000000b2", "roughness": 0.8 })");
    // Explicit weights: 33 x 33 samples of 2 layers, layer 1 on the east half.
    std::string weights;
    for (u32 z = 0; z < 33; ++z)
        for (u32 x = 0; x < 33; ++x) {
            weights.push_back(char(x < 16 ? 255 : 0));
            weights.push_back(char(x < 16 ? 0 : 255));
        }
    write_file(root + "/assets/terrain/painted.mcterrain",
               doc(R"("resolution": 33, "size": [32, 32], "height_range": [0, 8], "fill": 2,
                      "layers": [ { "material": "guid:7e7a0000-0000-4000-8000-0000000000b1", "tile": 3 },
                                  { "material": "guid:7e7a0000-0000-4000-8000-0000000000b2", "triplanar": true, "height_strength": 2 } ],
                      "splat": ")" + base64(weights) + "\""));
    // Auto rules over a ramp along x (0 to 30 m over 64 m: a 25° slope) with a flat west third.
    std::string ramp;
    for (u32 z = 0; z < 65; ++z)
        for (u32 x = 0; x < 65; ++x) put16le(ramp, static_cast<u16>(x < 22 ? 0 : (x - 22) * 65535 / 42));
    write_file(root + "/assets/terrain/auto.mcterrain",
               R"({ "format": "mce-terrain", "version": 1, "guid": "7e7a0000-0000-4000-8000-0000000000a3",
                    "resolution": 65, "size": [64, 64], "height_range": [0, 30], "splat_resolution": 33, "heights": ")" +
                   base64(ramp) + R"(",
                    "layers": [ { "auto": { "slope": [0, 10] } }, { "auto": { "slope": [15, 90] } }, { "auto": { "height": [25, 30] } } ] })");
    cook::CookService svc;
    const cook::CookReport r = svc.cook(StringView(root.c_str()), cook::CookOptions{});
    INFO(diags_of(r));
    REQUIRE(r.ok());

    asset::AssetSystem assets;
    const std::string cooked = root + "/cooked";
    const StringView roots[] = {StringView(cooked.c_str())};
    REQUIRE(assets.mount(Span<const StringView>(roots, 1)));
    auto h = assets.load_sync<asset::Terrain>(assets.find(asset::parse_guid(kGuid).value()));
    REQUIRE(h);
    const asset::TerrainView t = asset::TerrainView::read(assets.resolve(h.value())).value();
    REQUIRE(t.layers.size() == 2);
    CHECK(t.splat_resolution() == 33);  // min(resolution, 1025)
    CHECK(t.layers[0].material_guid() == asset::parse_guid("7e7a0000-0000-4000-8000-0000000000b1").value());
    CHECK(t.layers[0].tile == doctest::Approx(3.0f));
    CHECK(t.layers[0].flags == 0u);
    CHECK(t.layers[1].tile == doctest::Approx(4.0f));  // the default
    CHECK(t.layers[1].flags == asset::TerrainLayerFlags::kTriplanar);
    CHECK(t.layers[1].height_strength == doctest::Approx(2.0f));
    CHECK(t.weight(3, 9, 0) == 255);
    CHECK(t.weight(3, 9, 1) == 0);
    CHECK(t.weight(30, 9, 0) == 0);
    CHECK(t.weight(30, 9, 1) == 255);
    CHECK(t.weight(30, 9, 4) == 0);  // the second plane is there, empty

    auto ah = assets.load_sync<asset::Terrain>(assets.find(asset::parse_guid("7e7a0000-0000-4000-8000-0000000000a3").value()));
    REQUIRE(ah);
    const asset::TerrainView a = asset::TerrainView::read(assets.resolve(ah.value())).value();
    REQUIRE(a.layers.size() == 3);
    CHECK(a.splat_resolution() == 33);
    // The flat west: layer 0 only. The slope: layer 1. The top of the ramp: layers 1 and 2 share it.
    CHECK(a.weight(4, 16, 0) == 255);
    CHECK(a.weight(20, 16, 1) == 255);
    CHECK(a.weight(20, 16, 0) == 0);
    CHECK(a.weight(30, 16, 2) > 100);
    CHECK(a.weight(30, 16, 1) > 100);
    CHECK(a.weight(30, 16, 0) == 0);
}

TEST_CASE("terrain: splat layer mistakes are cook errors") {
    const std::string mat = R"({ "format": "mce-material", "version": 1, "guid": "7e7a0000-0000-4000-8000-0000000000b1" })";
    const std::string base = R"("resolution": 33, "size": [10, 10], "height_range": [0, 1], )";
    const std::string bad[] = {
        doc(base + R"("splat": "AAAA")"),                                                                     // splat without layers
        doc(base + R"("layers": [])"),                                                                        // no layers
        doc(base + R"("layers": [{},{},{},{},{},{},{},{},{}])"),                                              // nine layers
        doc(base + R"("layers": [{ "tile": 0 }])"),                                                           // zero tile
        doc(base + R"("layers": [{ "tiles": 2 }])"),                                                          // a typo'd key
        doc(base + R"("layers": [{ "auto": { "slope": [10, 120] } }])"),                                      // slope past 90
        doc(base + R"("layers": [{}], "splat_resolution": 65)"),                                             // finer than the heights
        doc(base + R"("layers": [{}, {}], "splat": "AAAA")"),                                                // wrong byte count
        doc(base + R"("layers": [{ "material": "guid:7e7a0000-0000-4000-8000-0000000000ff" }])"),            // not in the project
        doc(base + R"("layers": [{ "material": "guid:)" + std::string(kGuid) + R"(" }])"),                   // not a material
    };
    for (const std::string& text : bad) {
        const std::string root = fresh_dir("terrain_layers_bad");
        write_file(root + "/assets/m.mcmat", mat);
        write_file(root + "/assets/t.mcterrain", text);
        cook::CookService svc;
        const cook::CookReport r = svc.cook(StringView(root.c_str()), cook::CookOptions{});
        INFO(text);
        INFO(diags_of(r));
        CHECK_FALSE(r.ok());
        CHECK(diags_of(r).find("t.mcterrain") != std::string::npos);
    }
}

TEST_CASE("terrain: holes cook to a bitmask; an empty mask cooks to none (M-TERRAIN 3)") {
    // 33 x 33 = 1089 samples, 137 bytes of bits: a 3 x 3 hole at samples (10..12, 20..22).
    std::string mask(137, '\0');
    for (u32 z = 20; z <= 22; ++z)
        for (u32 x = 10; x <= 12; ++x) {
            const u32 i = z * 33 + x;
            mask[i / 8] = static_cast<char>(u8(mask[i / 8]) | (1u << (i % 8)));
        }
    const std::string root = fresh_dir("terrain_holes");
    write_file(root + "/assets/cave.mcterrain",
               doc(R"("resolution": 33, "size": [32, 32], "height_range": [0, 10], "fill": 4, "holes": ")" + base64(mask) + "\""));
    write_file(root + "/assets/none.mcterrain",
               R"({ "format": "mce-terrain", "version": 1, "guid": "7e7a0000-0000-4000-8000-0000000000a3",
                    "resolution": 33, "size": [32, 32], "height_range": [0, 10], "holes": ")" +
                   base64(std::string(137, '\0')) + "\" }");
    cook::CookService svc;
    const cook::CookReport r = svc.cook(StringView(root.c_str()), cook::CookOptions{});
    INFO(diags_of(r));
    REQUIRE(r.ok());
    asset::AssetSystem assets;
    const std::string cooked = root + "/cooked";
    const StringView roots[] = {StringView(cooked.c_str())};
    REQUIRE(assets.mount(Span<const StringView>(roots, 1)));

    auto h = assets.load_sync<asset::Terrain>(assets.find(asset::parse_guid(kGuid).value()));
    REQUIRE(h);
    auto v = asset::TerrainView::read(assets.resolve(h.value()));
    REQUIRE(v);
    const asset::TerrainView& t = v.value();
    CHECK((t.header->flags & asset::TerrainFlags::kHoles) != 0);
    CHECK(t.holes.size() == 137);
    u32 holes = 0;
    for (u32 z = 0; z < 33; ++z)
        for (u32 x = 0; x < 33; ++x) holes += t.hole(x, z);
    CHECK(holes == 9);
    CHECK(t.hole(11, 21));
    CHECK_FALSE(t.hole(9, 21));
    CHECK_FALSE(t.hole(11, 23));
    // The heights under a hole stay as authored.
    CHECK(t.sample(11, 21) == doctest::Approx(4.0f).epsilon(0.001));

    auto nh = assets.load_sync<asset::Terrain>(assets.find(asset::parse_guid("7e7a0000-0000-4000-8000-0000000000a3").value()));
    REQUIRE(nh);
    auto nv = asset::TerrainView::read(assets.resolve(nh.value()));
    REQUIRE(nv);
    CHECK((nv.value().header->flags & asset::TerrainFlags::kHoles) == 0);
    CHECK(nv.value().holes.empty());
    CHECK_FALSE(nv.value().hole(11, 21));

    // A mask of the wrong size is a cook error.
    const std::string bad = fresh_dir("terrain_holes_bad");
    write_file(bad + "/assets/t.mcterrain", doc(R"("resolution": 33, "size": [32, 32], "height_range": [0, 10], "holes": "AAAA")"));
    cook::CookService svc2;
    const cook::CookReport rb = svc2.cook(StringView(bad.c_str()), cook::CookOptions{});
    CHECK_FALSE(rb.ok());
    CHECK(diags_of(rb).find("holes") != std::string::npos);
}

TEST_CASE("terrain: surface_at follows the two triangles of each quad") {
    // 33 x 33 samples over 32 x 32 m: only sample (1, 1) is raised, so the quad (0..1, 0..1) is two planes.
    asset::TerrainHeader hd{};
    hd.resolution = 33;
    hd.chunk = 8;
    hd.size[0] = hd.size[1] = 32.0f;
    hd.height_min = 0.0f;
    hd.height_max = 1.0f;
    Array<u16> heights;
    heights.resize(33 * 33, u16{0});
    heights[34] = 65535;
    Array<byte> blob;
    asset::write_terrain(hd, heights.span(), blob);
    auto v = asset::TerrainView::read(blob.span());
    REQUIRE(v);
    const asset::TerrainView& t = v.value();
    // Quad (0, 0): corners at (-16, -16) .. (-15, -15) metres; the diagonal runs from h00 = 0 to h11 = 1.
    CHECK(t.surface_at(-15.5f, -15.5f) == doctest::Approx(0.5f));    // on the diagonal
    CHECK(t.surface_at(-15.25f, -15.75f) == doctest::Approx(0.25f));  // u = 0.75, w = 0.25: h00 + u(h10-h00) + w(h11-h10)
    CHECK(t.surface_at(-15.75f, -15.25f) == doctest::Approx(0.25f));  // the other triangle, mirrored
    // Bilinear differs off the diagonal: 0.75 * 0.25 there.
    CHECK(t.height_at(-15.25f, -15.75f) == doctest::Approx(0.1875f));
    CHECK(t.surface_at(-1000.0f, -1000.0f) == doctest::Approx(0.0f));  // clamped
}
