// Ordinary texture size cap (M-CONTENT 1a): the sidecar "max_size" and the project-wide CookOptions::max_texture_size
// (mce_cook --max-texture-size) box-halve a PNG/JPG before its mips; min(sidecar, global) wins; no setting cooks as it
// always did; the cap is part of the re-cook key; a bad value is a cook error that says what to change. And the cook
// size report (CookReport::sizes): bytes by asset type, the five biggest assets, how many textures the cap cut.

#include <doctest/doctest.h>

#include <cstring>
#include <string>
#include <vector>

#include "cook_test_util.h"
#include "mce/asset/asset_system.h"
#include "mce/cook/cook.h"

using namespace mce;
using namespace mce::asset;
using namespace cook_test;
using cook::CookOptions;
using cook::CookReport;
using cook::CookService;

namespace {

constexpr const char* kTex = "c1c1c1c1-0000-4000-8000-000000000001";
constexpr const char* kTex2 = "c1c1c1c1-0000-4000-8000-000000000002";
constexpr const char* kWav = "c1c1c1c1-0000-4000-8000-000000000003";
constexpr const char* kScene = "c1c1c1c1-0000-4000-8000-000000000004";

std::string diag_str(const CookReport& r) {
    std::string s;
    for (const auto& d : r.diags.all()) {
        cook::String line;
        cook::format_diagnostic(d, line);
        s += line.c_str();
        s += "\n";
    }
    return s;
}

// A w x h image whose texels differ everywhere, so a box filter is visible.
std::string noisy_png(u32 w, u32 h) {
    std::vector<u8> px(size_t(w) * h * 4);
    for (u32 y = 0; y < h; ++y)
        for (u32 x = 0; x < w; ++x) {
            u8* p = &px[(size_t(y) * w + x) * 4];
            p[0] = u8((x * 7 + y * 3) & 255);
            p[1] = u8((x * 13 + y * 5 + 40) & 255);
            p[2] = u8((x ^ y) & 255);
            p[3] = 255;
        }
    return make_png(w, h, px);
}

std::string make_tex_project(const char* name, u32 w, u32 h, const char* settings = "{}") {
    const std::string root = fresh_dir(name);
    write_file(root + "/assets/tex.png", noisy_png(w, h));
    write_file(root + "/assets/tex.png.mceasset", sidecar(kTex, "texture", settings));
    return root;
}

struct Info {
    bool ok = false;
    std::string diags;
    u32 w = 0, h = 0, mips = 0;
    u32 cooked = 0, up_to_date = 0;
    u64 manifest = 0;
};

// Cooks `root` with `opt` and reads the base level of the texture `guid`.
Info cook_info(const std::string& root, const CookOptions& opt = {}, const char* guid = kTex) {
    Info i;
    CookService svc;
    const CookReport r = svc.cook(root.c_str(), opt);
    i.diags = diag_str(r);
    i.cooked = r.cooked;
    i.up_to_date = r.up_to_date;
    i.manifest = r.manifest_hash;
    i.ok = r.ok();
    if (!i.ok) return i;
    AssetSystem as;
    const std::string cooked = root + "/cooked";
    const StringView roots[] = {cooked};
    REQUIRE(as.mount(Span<const StringView>(roots, 1)));
    auto h = as.load_sync<Texture>(parse_guid(guid).value());
    REQUIRE(h);
    auto v = TextureView::read(as.resolve(h.value()));
    REQUIRE(v);
    i.w = v.value().header->width;
    i.h = v.value().header->height;
    i.mips = v.value().header->mip_count;
    as.release(AnyHandle(h.value()));
    return i;
}

CookOptions capped(u32 n) {
    CookOptions o;
    o.max_texture_size = n;
    return o;
}

}  // namespace

TEST_CASE("texture cap: sidecar max_size halves a 512 image to 128 before the mips") {
    const std::string root = make_tex_project("cap_sidecar", 512, 512, "{ \"max_size\": 128 }");
    const Info i = cook_info(root);
    INFO(i.diags);
    REQUIRE(i.ok);
    CHECK(i.w == 128);
    CHECK(i.h == 128);
    CHECK(i.mips == 8);  // 128, 64, 32, 16, 8, 4, 2, 1
}

TEST_CASE("texture cap: the downscale is the mip filter (a 2x2 average per halving), a rectangle keeps its aspect") {
    // Linear data (srgb false): each halved texel is the rounded mean of its 2x2 block.
    const std::string root = make_tex_project("cap_filter", 8, 4, "{ \"srgb\": false, \"max_size\": 4, \"mips\": false }");
    const Info i = cook_info(root);
    INFO(i.diags);
    REQUIRE(i.ok);
    CHECK(i.w == 4);
    CHECK(i.h == 2);
    CHECK(i.mips == 1);
    AssetSystem as;
    const std::string cooked = root + "/cooked";
    const StringView roots[] = {cooked};
    REQUIRE(as.mount(Span<const StringView>(roots, 1)));
    auto h = as.load_sync<Texture>(parse_guid(kTex).value());
    REQUIRE(h);
    auto v = TextureView::read(as.resolve(h.value()));
    REQUIRE(v);
    const Span<const byte> px = v.value().pixels(0, 0);
    // texel (1, 1) of the 4x2 result averages source texels x 2..3, y 2..3
    int expect[3] = {0, 0, 0};
    for (int dy = 0; dy < 2; ++dy)
        for (int dx = 0; dx < 2; ++dx) {
            const int x = 2 + dx, y = 2 + dy;
            expect[0] += (x * 7 + y * 3) & 255;
            expect[1] += (x * 13 + y * 5 + 40) & 255;
            expect[2] += (x ^ y) & 255;
        }
    for (int c = 0; c < 3; ++c) CHECK(int(u8(px[(size_t(1) * 4 + 1) * 4 + size_t(c)])) == (expect[c] + 2) / 4);
    as.release(AnyHandle(h.value()));
}

TEST_CASE("texture cap: sidecar against project cap, the smaller wins (a bigger sidecar value is clamped)") {
    {
        const std::string root = make_tex_project("cap_prec_a", 256, 256, "{ \"max_size\": 64 }");
        const Info i = cook_info(root, capped(128));
        REQUIRE(i.ok);
        CHECK(i.w == 64);  // the sidecar is smaller
    }
    {
        const std::string root = make_tex_project("cap_prec_b", 256, 256, "{ \"max_size\": 128 }");
        const Info i = cook_info(root, capped(32));
        REQUIRE(i.ok);
        CHECK(i.w == 32);  // the project cap is smaller: the sidecar's larger value is clamped
    }
    {
        const std::string root = make_tex_project("cap_prec_c", 256, 256);
        const Info i = cook_info(root, capped(64));
        REQUIRE(i.ok);
        CHECK(i.w == 64);  // no sidecar value: the project cap applies
        CHECK(i.mips == 7);
    }
    {
        const std::string root = make_tex_project("cap_prec_d", 256, 256, "{ \"max_size\": 64 }");
        const Info i = cook_info(root);
        REQUIRE(i.ok);
        CHECK(i.w == 64);  // no project cap: the sidecar applies
    }
    {
        const std::string root = make_tex_project("cap_prec_e", 64, 64);
        const Info i = cook_info(root, capped(1024));
        REQUIRE(i.ok);
        CHECK(i.w == 64);  // a cap above the image changes nothing
    }
}

TEST_CASE("texture cap: no limit by default, and a cap that does not bite cooks byte-identical output") {
    const std::string a = make_tex_project("cap_ident_a", 96, 40);
    const std::string b = make_tex_project("cap_ident_b", 96, 40);
    const std::string c = make_tex_project("cap_ident_c", 96, 40, "{ \"max_size\": 1024 }");
    const Info ia = cook_info(a);
    const Info ib = cook_info(b, capped(1024));
    const Info ic = cook_info(c);
    REQUIRE(ia.ok);
    REQUIRE(ib.ok);
    REQUIRE(ic.ok);
    CHECK(ia.w == 96);
    CHECK(ia.mips == 7);
    // the cooked texture (and so the manifest, which names its content hash) is the same bytes
    CHECK(ia.manifest == ib.manifest);
    CHECK(read_file(a + "/cooked/manifest.mce") == read_file(b + "/cooked/manifest.mce"));
    CHECK(read_file(a + "/cooked/manifest.mce") == read_file(c + "/cooked/manifest.mce"));
    // and a project cooked twice into fresh folders matches
    const std::string a2 = make_tex_project("cap_ident_a2", 96, 40);
    CHECK(cook_info(a2).manifest == ia.manifest);
}

TEST_CASE("texture cap: the cap is part of the re-cook key") {
    const std::string root = make_tex_project("cap_key", 64, 64);
    Info i = cook_info(root);
    REQUIRE(i.ok);
    CHECK(i.cooked == 1);
    i = cook_info(root);
    CHECK(i.cooked == 0);
    CHECK(i.up_to_date == 1);
    i = cook_info(root, capped(16));
    REQUIRE(i.ok);
    CHECK(i.cooked == 1);  // a new cap re-cooks
    CHECK(i.w == 16);
    i = cook_info(root, capped(16));
    CHECK(i.cooked == 0);
    i = cook_info(root, capped(8));
    CHECK(i.cooked == 1);
    CHECK(i.w == 8);
    i = cook_info(root);  // back to no limit: re-cooked at full size
    CHECK(i.cooked == 1);
    CHECK(i.w == 64);
}

TEST_CASE("texture cap: bad values are cook errors that say what to change") {
    for (const char* bad : {"{ \"max_size\": 100 }", "{ \"max_size\": 0 }", "{ \"max_size\": 32768 }", "{ \"max_size\": -4 }",
                            "{ \"max_size\": 2.5 }", "{ \"max_size\": \"big\" }"}) {
        const std::string root = make_tex_project("cap_bad", 8, 8, bad);
        const Info i = cook_info(root);
        INFO(bad, ": ", i.diags);
        CHECK_FALSE(i.ok);
        CHECK(i.diags.find("max_size") != std::string::npos);
        CHECK(i.diags.find("tex.png") != std::string::npos);
    }
    {
        const std::string root = make_tex_project("cap_bad2", 8, 8, "{ \"max_size\": 100 }");
        const Info i = cook_info(root);
        CHECK(i.diags.find("power of two from 1 to 16384") != std::string::npos);
        CHECK(i.diags.find("sidecar") != std::string::npos);  // where to change it
    }
    for (u32 bad : {3u, 100u, 32768u, 0xFFFFFFFFu}) {
        const std::string root = make_tex_project("cap_bad_global", 8, 8);
        const Info i = cook_info(root, capped(bad));
        INFO(bad, ": ", i.diags);
        CHECK_FALSE(i.ok);
        CHECK(i.cooked == 0);
        CHECK(i.diags.find("power of two from 1 to 16384") != std::string::npos);
        CHECK(i.diags.find("--max-texture-size") != std::string::npos);
    }
    CHECK(cook::valid_max_texture_size(0));
    CHECK(cook::valid_max_texture_size(1));
    CHECK(cook::valid_max_texture_size(16384));
    CHECK_FALSE(cook::valid_max_texture_size(32768));
    CHECK_FALSE(cook::valid_max_texture_size(1000));
}

TEST_CASE("texture cap: a 1x1 floor and an odd size") {
    {
        const std::string root = make_tex_project("cap_one", 16, 16, "{ \"max_size\": 1 }");
        const Info i = cook_info(root);
        REQUIRE(i.ok);
        CHECK(i.w == 1);
        CHECK(i.h == 1);
        CHECK(i.mips == 1);
    }
    {
        const std::string root = make_tex_project("cap_odd", 100, 60, "{ \"max_size\": 32" "}");
        const Info i = cook_info(root);
        REQUIRE(i.ok);
        CHECK(i.w == 25);  // 100 -> 50 -> 25: halved until the largest side fits
        CHECK(i.h == 15);
    }
}

TEST_CASE("cook size report: bytes by type, the five largest assets, textures cut by the cap") {
    const std::string root = fresh_dir("cap_report");
    const std::string a = root + "/assets";
    write_file(a + "/big.png", noisy_png(128, 128));
    write_file(a + "/big.png.mceasset", sidecar(kTex, "texture"));
    write_file(a + "/small.png", noisy_png(16, 16));
    write_file(a + "/small.png.mceasset", sidecar(kTex2, "texture"));
    write_file(a + "/beep.wav", make_wav(22050, 1, 2205, 660.0));
    write_file(a + "/beep.wav.mceasset", sidecar(kWav, "wav"));
    write_file(a + "/level.mcscene", std::string("{ \"format\": \"mce-scene\", \"version\": 1, \"guid\": \"") + kScene +
                                         "\", \"name\": \"level\", \"settings\": {}, \"entities\": [ { \"name\": \"Empty\" } ] }");
    CookService svc;
    const CookReport full = svc.cook(root.c_str(), CookOptions{});
    INFO(diag_str(full));
    REQUIRE(full.ok());
    const cook::SizeReport& s = full.sizes;
    CHECK(s.texture_count == 2);
    CHECK(s.textures_downscaled == 0);
    CHECK(s.textures > 128u * 128u * 4u);  // base level of the big one alone
    CHECK(s.audio > 0);
    CHECK(s.scenes > 0);
    CHECK(s.meshes == 0);
    CHECK(s.total() == s.textures + s.meshes + s.audio + s.scenes + s.other);
    REQUIRE(s.largest.size() == 4);
    CHECK(std::string(s.largest[0].path.c_str()) == "big.png");
    CHECK(s.largest[0].bytes > s.largest[1].bytes);
    for (size_t k = 1; k < s.largest.size(); ++k) CHECK(s.largest[k - 1].bytes >= s.largest[k].bytes);
    u64 sum = 0;
    for (const auto& e : s.largest) sum += e.bytes;
    CHECK(sum == s.total());  // 4 assets, all listed

    // A project cap of 32 cuts the big one (not the 16x16): the report is the same on a cached cook of that cap.
    for (int pass = 0; pass < 2; ++pass) {
        const CookReport r = svc.cook(root.c_str(), capped(32));
        INFO(pass, ": ", diag_str(r));
        REQUIRE(r.ok());
        CHECK(r.cooked == (pass == 0 ? 2u : 0u));  // the two textures; the sound and the scene are not touched by the cap
        CHECK(r.sizes.texture_count == 2);
        CHECK(r.sizes.textures_downscaled == 1);
        CHECK(r.sizes.textures < s.textures / 4);
        CHECK(r.sizes.audio == s.audio);
        CHECK(r.sizes.scenes == s.scenes);
    }
    // A sidecar cut counts too.
    write_file(a + "/small.png.mceasset", sidecar(kTex2, "texture", "{ \"max_size\": 4 }"));
    const CookReport r2 = svc.cook(root.c_str(), CookOptions{});
    REQUIRE(r2.ok());
    CHECK(r2.sizes.textures_downscaled == 1);
}

TEST_CASE("cook size report: only the five largest are listed") {
    const std::string root = fresh_dir("cap_report_top5");
    for (int k = 0; k < 7; ++k) {
        char guid[64];
        std::snprintf(guid, sizeof guid, "c2c2c2c2-0000-4000-8000-0000000000%02d", k + 10);
        const std::string name = "/assets/t" + std::to_string(k) + ".png";
        write_file(root + name, noisy_png(u32(8 + 8 * k), u32(8 + 8 * k)));
        write_file(root + name + ".mceasset", sidecar(guid, "texture", "{ \"mips\": false }"));
    }
    CookService svc;
    const CookReport r = svc.cook(root.c_str(), CookOptions{});
    REQUIRE(r.ok());
    REQUIRE(r.sizes.largest.size() == 5);
    CHECK(std::string(r.sizes.largest[0].path.c_str()) == "t6.png");
    CHECK(std::string(r.sizes.largest[4].path.c_str()) == "t2.png");
    CHECK(r.sizes.texture_count == 7);
}
