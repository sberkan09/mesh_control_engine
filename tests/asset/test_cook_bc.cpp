// Block-compressed texture cook (M-CONTENT 1b-ii): the sidecar's "compress" value under each cook mode (rgba8, bc, astc),
// the `auto` picks, the 4-multiple fallback, mip sizes down to 1x1, refusals, the PSNR bounds of the encoders, determinism
// (twice, the pinned encoder hash) and that --textures bc re-cooks. Fixtures are generated here.

#include <doctest/doctest.h>

#include <cmath>
#include <cstring>
#include <string>
#include <vector>

#include "bc_pinned.h"
#include "cook_test_util.h"
#include "hdr_fixtures.h"
#include "mce/asset/asset_system.h"
#include "mce/cook/cook.h"

using namespace mce;
using namespace mce::asset;
using namespace cook_test;
using cook::BcFormat;
using cook::CookOptions;
using cook::CookReport;
using cook::CookService;
using cook::TextureCompression;

namespace {

constexpr const char* kTex = "b1b1b1b1-0000-4000-8000-000000000001";

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

enum class Img { Albedo, Normal, Grey, Orm, Lut };

// 64x64 unless `w`/`h` say otherwise. Albedo: a colour gradient. Normal: unit normals bent by a smooth bump. Grey: r = g = b.
// Orm: occlusion/roughness/metal-like data (not unit-length, not grey).
std::vector<u8> make_pixels(Img kind, u32 w, u32 h) {
    std::vector<u8> px(usize(w) * h * 4);
    for (u32 y = 0; y < h; ++y)
        for (u32 x = 0; x < w; ++x) {
            u8* p = &px[(usize(y) * w + x) * 4];
            p[3] = 255;
            switch (kind) {
                case Img::Albedo:
                case Img::Lut:
                    p[0] = u8(x * 255 / (w - 1));
                    p[1] = u8(y * 255 / (h - 1));
                    p[2] = u8(((x * 7 + y * 3) & 127) + 64);
                    break;
                case Img::Normal: {
                    const float nx = 0.4f * std::sin(float(x) * 0.3f), ny = 0.4f * std::cos(float(y) * 0.3f);
                    const float nz = std::sqrt(1.0f - nx * nx - ny * ny);
                    p[0] = u8(std::lround((nx * 0.5f + 0.5f) * 255.0f));
                    p[1] = u8(std::lround((ny * 0.5f + 0.5f) * 255.0f));
                    p[2] = u8(std::lround((nz * 0.5f + 0.5f) * 255.0f));
                    break;
                }
                case Img::Grey: p[0] = p[1] = p[2] = u8((x * 4 + y * 2) & 255); break;
                case Img::Orm:
                    p[0] = 250;
                    p[1] = u8(60 + (x & 31));
                    p[2] = u8(10 + (y & 15));
                    break;
            }
        }
    return px;
}

struct CookResult {
    bool ok = false;
    std::string diags;
    TextureFormat format = TextureFormat::RGBA8;
    u32 flags = 0, w = 0, h = 0, mips = 0;
    u32 cooked = 0, up_to_date = 0;
    u64 manifest = 0;
    std::vector<u64> level_bytes;  // payload bytes per mip
};

std::string make_project(const char* name, const std::string& png, const char* settings) {
    const std::string root = fresh_dir(name);
    write_file(root + "/assets/tex.png", png);
    write_file(root + "/assets/tex.png.mceasset", sidecar(kTex, "texture", settings));
    return root;
}

CookResult cook_project(const std::string& root, TextureCompression mode) {
    CookResult i;
    CookOptions opt;
    opt.textures = mode;
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
    auto h = as.load_sync<Texture>(parse_guid(kTex).value());
    REQUIRE(h);
    auto v = TextureView::read(as.resolve(h.value()));
    REQUIRE(v);
    i.format = v.value().header->format;
    i.flags = v.value().header->flags;
    i.w = v.value().header->width;
    i.h = v.value().header->height;
    i.mips = v.value().header->mip_count;
    for (u32 m = 0; m < i.mips; ++m) i.level_bytes.push_back(v.value().pixels(m, 0).size());
    as.release(AnyHandle(h.value()));
    return i;
}

CookResult cook_one(const char* name, Img kind, u32 w, u32 h, const char* settings, TextureCompression mode) {
    return cook_project(make_project(name, make_png(w, h, make_pixels(kind, w, h)), settings), mode);
}

bool contains(const std::string& s, const char* needle) { return s.find(needle) != std::string::npos; }

}  // namespace

TEST_CASE("cook bc: each compress value under each mode follows the table") {
    struct Row {
        const char* settings;  // "srgb": false so bc5 and bc4 are legal
        TextureFormat rgba8, bc, astc;
    };
    const TextureFormat R = TextureFormat::RGBA8, A = TextureFormat::ASTC_4x4;
    const Row rows[] = {
        {"{ \"srgb\": false }", R, TextureFormat::BC7, A},  // missing: the mode's default (auto: a colour gradient is BC7)
        {"{ \"srgb\": false, \"compress\": \"none\" }", R, R, R},
        {"{ \"srgb\": false, \"compress\": \"auto\" }", R, TextureFormat::BC7, A},
        {"{ \"srgb\": false, \"compress\": \"bc7\" }", R, TextureFormat::BC7, A},
        {"{ \"srgb\": false, \"compress\": \"bc5\" }", R, TextureFormat::BC5, A},
        {"{ \"srgb\": false, \"compress\": \"bc4\" }", R, TextureFormat::BC4, A},
        {"{ \"srgb\": false, \"compress\": \"bc1\" }", R, TextureFormat::BC1, A},
        {"{ \"srgb\": false, \"compress\": \"astc\" }", R, R, A},
    };
    int n = 0;
    for (const Row& row : rows) {
        INFO(row.settings);
        const std::string name = "bc_table_" + std::to_string(n++);
        const std::string png = make_png(32, 32, make_pixels(Img::Albedo, 32, 32));
        const CookResult a = cook_project(make_project((name + "a").c_str(), png, row.settings), TextureCompression::None);
        const CookResult b = cook_project(make_project((name + "b").c_str(), png, row.settings), TextureCompression::Bc);
        const CookResult c = cook_project(make_project((name + "c").c_str(), png, row.settings), TextureCompression::Astc4x4);
        INFO(a.diags);
        INFO(b.diags);
        INFO(c.diags);
        REQUIRE(a.ok);
        REQUIRE(b.ok);
        REQUIRE(c.ok);
        CHECK(a.format == row.rgba8);
        CHECK(b.format == row.bc);
        CHECK(c.format == row.astc);
    }
}

TEST_CASE("cook bc: srgb stays a flag on BC7 and BC1") {
    const CookResult a = cook_one("bc_srgb_7", Img::Albedo, 32, 32, "{ \"compress\": \"bc7\" }", TextureCompression::Bc);
    const CookResult b = cook_one("bc_srgb_1", Img::Albedo, 32, 32, "{ \"compress\": \"bc1\" }", TextureCompression::Bc);
    REQUIRE(a.ok);
    REQUIRE(b.ok);
    CHECK((a.flags & TextureFlags::Srgb) != 0);
    CHECK((b.flags & TextureFlags::Srgb) != 0);
    CHECK(a.format == TextureFormat::BC7);
    CHECK(b.format == TextureFormat::BC1);
}

TEST_CASE("cook bc: the auto picks hold on albedo, normal, grey and ORM images") {
    CHECK(cook_one("bc_auto_albedo", Img::Albedo, 64, 64, "{}", TextureCompression::Bc).format == TextureFormat::BC7);
    const CookResult n = cook_one("bc_auto_normal", Img::Normal, 64, 64, "{ \"srgb\": false }", TextureCompression::Bc);
    CHECK(n.format == TextureFormat::BC5);
    CHECK(contains(n.diags, "normal map"));  // the choice is named, and the sidecar overrides it
    CHECK(cook_one("bc_auto_grey", Img::Grey, 64, 64, "{ \"srgb\": false }", TextureCompression::Bc).format == TextureFormat::BC4);
    CHECK(cook_one("bc_auto_orm", Img::Orm, 64, 64, "{ \"srgb\": false }", TextureCompression::Bc).format == TextureFormat::BC7);
    // a grey image that is colour data (srgb true) is still BC7, and an LUT strip (w == h*h, h >= 16) stays RGBA8
    CHECK(cook_one("bc_auto_grey_srgb", Img::Grey, 64, 64, "{}", TextureCompression::Bc).format == TextureFormat::BC7);
    CHECK(cook_one("bc_auto_lut", Img::Lut, 256, 16, "{ \"srgb\": false }", TextureCompression::Bc).format == TextureFormat::RGBA8);
    // an explicit value beats the rule
    CHECK(cook_one("bc_auto_over", Img::Normal, 64, 64, "{ \"srgb\": false, \"compress\": \"bc7\" }", TextureCompression::Bc).format ==
          TextureFormat::BC7);
}

TEST_CASE("cook bc: a base side that is not a multiple of 4 cooks RGBA8 with a warning that names the fix") {
    for (const char* settings : {"{ \"compress\": \"bc7\" }", "{}"}) {
        INFO(settings);
        const CookResult r = cook_one("bc_odd", Img::Albedo, 30, 20, settings, TextureCompression::Bc);
        REQUIRE(r.ok);  // never an error
        CHECK(r.format == TextureFormat::RGBA8);
        CHECK(contains(r.diags, "multiples of 4"));
        CHECK(contains(r.diags, "tex.png"));
    }
    // max_size can make a legal base: 36x36 capped to 18 is not, capped to 9 neither, 64 -> 32 is
    CHECK(cook_one("bc_odd_cap", Img::Albedo, 36, 36, "{ \"compress\": \"bc7\", \"max_size\": 16 }", TextureCompression::Bc).format ==
          TextureFormat::RGBA8);
    CHECK(cook_one("bc_cap_ok", Img::Albedo, 64, 64, "{ \"compress\": \"bc7\", \"max_size\": 32 }", TextureCompression::Bc).format ==
          TextureFormat::BC7);
}

TEST_CASE("cook bc: mips down to 1x1 have the right byte sizes") {
    struct Case {
        const char* compress;
        const char* srgb;
        TextureFormat f;
    };
    for (const Case& c : {Case{"bc7", "true", TextureFormat::BC7}, Case{"bc5", "false", TextureFormat::BC5},
                          Case{"bc4", "false", TextureFormat::BC4}, Case{"bc1", "true", TextureFormat::BC1}}) {
        const std::string settings = std::string("{ \"compress\": \"") + c.compress + "\", \"srgb\": " + c.srgb + " }";
        INFO(settings);
        const CookResult r = cook_one("bc_mips", Img::Albedo, 64, 32, settings.c_str(), TextureCompression::Bc);
        INFO(r.diags);
        REQUIRE(r.ok);
        CHECK(r.format == c.f);
        REQUIRE(r.mips == 7);  // 64x32, 32x16, 16x8, 8x4, 4x2, 2x1, 1x1
        u32 w = 64, h = 32;
        for (u32 m = 0; m < r.mips; ++m) {
            CHECK(r.level_bytes[m] == texture_level_bytes(c.f, w, h));
            w = w > 1 ? w / 2 : 1;
            h = h > 1 ? h / 2 : 1;
        }
        const u64 block = (c.f == TextureFormat::BC1 || c.f == TextureFormat::BC4) ? 8 : 16;
        CHECK(r.level_bytes[0] == 16 * 8 * block);
        CHECK(r.level_bytes[6] == block);  // 1x1: one padded block
        CHECK(r.level_bytes[5] == block);  // 2x1
    }
}

TEST_CASE("cook bc: refusals say what to change") {
    const CookResult a = cook_one("bc_ref_bc4", Img::Grey, 32, 32, "{ \"compress\": \"bc4\" }", TextureCompression::Bc);  // srgb defaults to true
    CHECK_FALSE(a.ok);
    CHECK(contains(a.diags, "\"srgb\": false"));
    const CookResult b = cook_one("bc_ref_bc5", Img::Normal, 32, 32, "{ \"compress\": \"bc5\", \"srgb\": true }", TextureCompression::None);
    CHECK_FALSE(b.ok);  // the value is checked in every mode, so a mode switch never turns a mistake into an error
    const CookResult c = cook_one("bc_ref_unknown", Img::Albedo, 32, 32, "{ \"compress\": \"dxt5\" }", TextureCompression::None);
    CHECK_FALSE(c.ok);
    for (const char* v : {"none", "auto", "bc7", "bc5", "bc4", "bc1", "astc"}) CHECK(contains(c.diags, (std::string("\"") + v + "\"").c_str()));
    CHECK(contains(c.diags, "tex.png"));
    CHECK(contains(c.diags, "compress"));
}

TEST_CASE("cook bc: compress on an .hdr is an unknown-key error") {
    const std::string root = fresh_dir("bc_hdr");
    write_file(root + "/assets/sky.hdr", hdr_fixtures::make_rgbe_hdr(8, 4, std::vector<float>(32, 1.0f)));
    write_file(root + "/assets/sky.hdr.mceasset", sidecar(kTex, "texture", "{ \"compress\": \"bc7\" }"));
    CookService svc;
    CookOptions opt;
    opt.textures = TextureCompression::Bc;
    const CookReport r = svc.cook(root.c_str(), opt);
    CHECK_FALSE(r.ok());
    CHECK(contains(diag_str(r), "compress"));
}

TEST_CASE("cook bc: an opaque BC1 texture warns when it had alpha") {
    std::vector<u8> px = make_pixels(Img::Albedo, 32, 32);
    for (usize i = 3; i < px.size(); i += 4) px[i] = 100;
    const CookResult r = cook_project(make_project("bc_bc1_alpha", make_png(32, 32, px), "{ \"compress\": \"bc1\" }"), TextureCompression::Bc);
    REQUIRE(r.ok);
    CHECK(r.format == TextureFormat::BC1);
    CHECK(contains(r.diags, "alpha"));
}

namespace {

// PSNR over `channels` of two RGBA8 images, and the largest per-channel error.
void compare(const std::vector<u8>& a, const std::vector<u8>& b, int channels, double& min_psnr, int& max_err) {
    min_psnr = 1e9;
    max_err = 0;
    for (int c = 0; c < channels; ++c) {
        double se = 0;
        for (usize i = c; i < a.size(); i += 4) {
            const int d = int(a[i]) - int(b[i]);
            se += double(d * d);
            max_err = std::max(max_err, std::abs(d));
        }
        const double mse = se / double(a.size() / 4);
        min_psnr = std::min(min_psnr, mse == 0 ? 99.0 : 10.0 * std::log10(255.0 * 255.0 / mse));
    }
}

}  // namespace

TEST_CASE("cook bc: PSNR bounds on a 256x256 gradient+noise image") {
    const u32 N = 256;
    const std::vector<u8> src = bc_pinned::gradient_noise(N, N);
    struct Case {
        BcFormat f;
        const char* name;
        int channels;
        double min_psnr;
    };
    for (const Case& c : {Case{BcFormat::BC7, "bc7", 4, 35.0}, Case{BcFormat::BC5, "bc5", 2, 38.0}, Case{BcFormat::BC4, "bc4", 1, 38.0}}) {
        INFO(c.name);
        std::vector<u8> blocks(usize(cook::bc_level_bytes(c.f, N, N)));
        cook::encode_bc_level(c.f, true, src.data(), N, N, blocks.data());
        std::vector<u8> dec(src.size());
        cook::decode_bc_level(c.f, blocks.data(), N, N, dec.data());
        double psnr;
        int max_err;
        compare(src, dec, c.channels, psnr, max_err);
        INFO("psnr " << psnr << " max error " << max_err);
        CHECK(psnr >= c.min_psnr);
        CHECK(max_err <= 48);
    }
    // BC1 is lossy on gradients; only sanity here (it is for opaque colour by explicit choice)
    std::vector<u8> blocks(usize(cook::bc_level_bytes(BcFormat::BC1, N, N)));
    cook::encode_bc_level(BcFormat::BC1, true, src.data(), N, N, blocks.data());
    std::vector<u8> dec(src.size());
    cook::decode_bc_level(BcFormat::BC1, blocks.data(), N, N, dec.data());
    double psnr;
    int max_err;
    compare(src, dec, 3, psnr, max_err);
    CHECK(psnr >= 28.0);
}

TEST_CASE("cook bc: padded blocks repeat the edge texels (a 2x1 level round-trips its two texels)") {
    const u8 src[8] = {10, 20, 30, 255, 200, 190, 180, 255};
    u8 blocks[16];
    cook::encode_bc_level(BcFormat::BC7, false, src, 2, 1, blocks);
    u8 dec[8];
    cook::decode_bc_level(BcFormat::BC7, blocks, 2, 1, dec);
    for (int i = 0; i < 8; ++i) CHECK(std::abs(int(dec[i]) - int(src[i])) <= 6);
}

TEST_CASE("cook bc: the cook is deterministic and the encoder output is pinned") {
    const std::string png = make_png(64, 64, bc_pinned::gradient_noise(64, 64));
    const CookResult a = cook_project(make_project("bc_det_a", png, "{ \"compress\": \"bc7\" }"), TextureCompression::Bc);
    const CookResult b = cook_project(make_project("bc_det_b", png, "{ \"compress\": \"bc7\" }"), TextureCompression::Bc);
    REQUIRE(a.ok);
    REQUIRE(b.ok);
    CHECK(a.manifest == b.manifest);
    // the same bytes, level by level, across two runs of the encoder in one process
    CHECK(bc_pinned::chain_hash() == bc_pinned::chain_hash());
    // x86_64 value (cook goldens are guaranteed there only; test_cook_bc_arch checks other hosts)
    CHECK(bc_pinned::chain_hash() == bc_pinned::kPinnedBcHash);
}

TEST_CASE("cook bc: a missing key under rgba8 cooks the same manifest as before; --textures bc re-cooks") {
    const std::string png = make_png(32, 32, make_pixels(Img::Albedo, 32, 32));
    const CookResult plain = cook_project(make_project("bc_key_plain", png, "{}"), TextureCompression::None);
    const CookResult keyed = cook_project(make_project("bc_key_keyed", png, "{ \"compress\": \"bc7\" }"), TextureCompression::None);
    REQUIRE(plain.ok);
    REQUIRE(keyed.ok);
    CHECK(plain.format == TextureFormat::RGBA8);
    CHECK(keyed.format == TextureFormat::RGBA8);
    CHECK(plain.manifest == keyed.manifest);

    const std::string root = make_project("bc_recook", png, "{}");
    CookResult r = cook_project(root, TextureCompression::None);
    REQUIRE(r.ok);
    CHECK(r.cooked == 1);
    r = cook_project(root, TextureCompression::None);
    CHECK(r.cooked == 0);
    CHECK(r.up_to_date == 1);
    r = cook_project(root, TextureCompression::Bc);
    REQUIRE(r.ok);
    CHECK(r.cooked == 1);  // the mode is in the key
    CHECK(r.format == TextureFormat::BC7);
    r = cook_project(root, TextureCompression::Bc);
    CHECK(r.cooked == 0);
    CHECK(r.up_to_date == 1);
    // editing the sidecar's compress value re-cooks too (it is in settings_hash)
    write_file(root + "/assets/tex.png.mceasset", sidecar(kTex, "texture", "{ \"compress\": \"bc1\" }"));
    r = cook_project(root, TextureCompression::Bc);
    REQUIRE(r.ok);
    CHECK(r.cooked == 1);
    CHECK(r.format == TextureFormat::BC1);
}
