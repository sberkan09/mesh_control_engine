// HDR texture cook (M-VISUAL3 4): .hdr (Radiance, stb) and .exr (OpenEXR, tinyexr) cook to a linear
// RGBA16F chunk that keeps values above 1, clamps to the half range, zeroes NaN and Inf, box-filters its
// mips in float and downsizes to "max_size". Fixtures are generated here (hdr_fixtures.h).

#include <doctest/doctest.h>

#include <cmath>
#include <cstring>
#include <string>
#include <vector>

#include "cook_test_util.h"
#include "hdr_fixtures.h"
#include "mce/asset/asset_system.h"
#include "mce/cook/cook.h"

using namespace mce;
using namespace mce::asset;
using namespace cook_test;
using cook::CookOptions;
using cook::CookReport;
using cook::CookService;

namespace {

constexpr const char* kHdrGuid = "b2b2b2b2-0000-4000-8000-000000000001";

float half_to_float(u16 h) {
    const int sign = (h >> 15) & 1, e = (h >> 10) & 0x1F, m = h & 0x3FF;
    float v;
    if (e == 0) v = std::ldexp(float(m), -24);
    else if (e == 31) v = m ? std::nanf("") : INFINITY;
    else v = std::ldexp(float(m | 0x400), e - 25);
    return sign ? -v : v;
}

// A loaded cook output: the cooked texture of kHdrGuid.
struct Cooked {
    std::string root;
    AssetSystem as;
    AssetHandle<Texture> handle;
    TextureView view;
    bool ok = false;
    std::string diags;
    u32 cooked = 0;
};

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

// Cooks `source` as assets/sky<ext> with the sidecar `settings`; on success the view is loaded.
void cook_hdr(Cooked& c, const char* name, const char* ext, const std::string& source, const char* settings = "{}",
              const char* importer = "texture") {
    c.root = fresh_dir(name);
    const std::string file = c.root + "/assets/sky" + ext;
    write_file(file, source);
    write_file(file + ".mceasset", sidecar(kHdrGuid, importer, settings));
    CookService svc;
    const CookReport r = svc.cook(c.root.c_str(), CookOptions{});
    c.diags = diag_str(r);
    c.cooked = r.cooked;
    if (!r.ok()) return;
    const std::string cooked = c.root + "/cooked";
    const StringView roots[] = {cooked};
    REQUIRE(c.as.mount(Span<const StringView>(roots, 1)));
    auto h = c.as.load_sync<Texture>(parse_guid(kHdrGuid).value());
    REQUIRE(h);
    c.handle = h.value();
    auto v = TextureView::read(c.as.resolve(c.handle));
    REQUIRE(v);
    c.view = v.value();
    c.ok = true;
}

float texel(const Cooked& c, u32 mip, u32 x, u32 y, int channel) {
    const TextureMip& m = c.view.mips[mip];
    const Span<const byte> px = c.view.pixels(mip, 0);
    u16 h;
    std::memcpy(&h, px.data() + (usize(y) * m.width + x) * 8 + usize(channel) * 2, 2);
    return half_to_float(h);
}

// 64x32 grey sky: 0.25 everywhere and a 8x4 patch of 50 (a 200x hot spot) centered on x = 19, y = 15.
std::vector<float> hot_sky() {
    std::vector<float> g(64 * 32, 0.25f);
    for (u32 y = 14; y < 18; ++y)
        for (u32 x = 16; x < 24; ++x) g[y * 64 + x] = 50.0f;
    return g;
}

std::vector<float> to_rgba(const std::vector<float>& grey) {
    std::vector<float> o;
    for (float v : grey) o.insert(o.end(), {v, v, v, 1.0f});
    return o;
}

}  // namespace

TEST_CASE("cook hdr: a .hdr keeps values above 1 and the hot spot's ratio, as linear RGBA16F with a full chain") {
    Cooked c;
    cook_hdr(c, "cook_hdr_basic", ".hdr", hdr_fixtures::make_rgbe_hdr(64, 32, hot_sky()));
    INFO(c.diags);
    REQUIRE(c.ok);
    CHECK(c.view.header->format == TextureFormat::RGBA16F);
    CHECK((c.view.header->flags & TextureFlags::Srgb) == 0);
    CHECK(c.view.header->width == 64);
    CHECK(c.view.header->height == 32);
    CHECK(c.view.mips.size() == 7);  // 64x32 ... 1x1
    CHECK(c.view.mips[0].layer_size == 64u * 32 * 8);
    const float base = texel(c, 0, 2, 2, 0), hot = texel(c, 0, 19, 15, 0);
    CHECK(base == doctest::Approx(0.25f).epsilon(1e-3));
    CHECK(hot == doctest::Approx(50.0f).epsilon(1e-3));
    CHECK(hot / base == doctest::Approx(200.0f).epsilon(1e-3));  // within half-float precision
    CHECK(texel(c, 0, 19, 15, 3) == 1.0f);                       // no alpha in the file: opaque
    // The mip chain is averaged in float, not clamped to 1: the 1x1 level is the mean of the whole image.
    f64 sum = 0;
    for (float v : hot_sky()) sum += v;
    CHECK(texel(c, 6, 0, 0, 1) == doctest::Approx(float(sum / (64.0 * 32.0))).epsilon(2e-3));
    c.as.release(AnyHandle(c.handle));
}

TEST_CASE("cook hdr: an .exr keeps values above 1; NaN and Inf become 0, the rest clamps to the half range") {
    {
        Cooked c;  // the same sky as the .hdr
        cook_hdr(c, "cook_exr_sky", ".exr", hdr_fixtures::make_exr(64, 32, to_rgba(hot_sky())));
        INFO(c.diags);
        REQUIRE(c.ok);
        CHECK(c.view.header->format == TextureFormat::RGBA16F);
        CHECK(c.view.mips.size() == 7);
        CHECK(texel(c, 0, 2, 2, 2) == doctest::Approx(0.25f).epsilon(1e-3));
        CHECK(texel(c, 0, 19, 15, 0) / texel(c, 0, 2, 2, 0) == doctest::Approx(200.0f).epsilon(1e-3));
        c.as.release(AnyHandle(c.handle));
    }
    Cooked c;
    std::vector<float> px(8 * 2 * 4, 1.0f);
    const float vals[8] = {std::nanf(""), INFINITY, -INFINITY, 1.0e6f, -3.0f, 70000.0f, 5.0f, 0.5f};
    for (int i = 0; i < 8; ++i) px[size_t(i) * 4 + 0] = px[size_t(i) * 4 + 1] = px[size_t(i) * 4 + 2] = vals[i];
    cook_hdr(c, "cook_exr_range", ".exr", hdr_fixtures::make_exr(8, 2, px), "{ \"mips\": false }");
    INFO(c.diags);
    REQUIRE(c.ok);
    CHECK(c.view.mips.size() == 1);
    CHECK(texel(c, 0, 0, 0, 0) == 0.0f);        // NaN
    CHECK(texel(c, 0, 1, 0, 1) == 0.0f);        // +Inf
    CHECK(texel(c, 0, 2, 0, 2) == 0.0f);        // -Inf
    CHECK(texel(c, 0, 3, 0, 0) == 65504.0f);    // clamped to the half range
    CHECK(texel(c, 0, 4, 0, 0) == 0.0f);        // negative light is nonsense
    CHECK(texel(c, 0, 5, 0, 0) == 65504.0f);
    CHECK(texel(c, 0, 6, 0, 0) == 5.0f);
    CHECK(texel(c, 0, 7, 0, 0) == 0.5f);
    CHECK(texel(c, 0, 7, 0, 3) == 1.0f);
    c.as.release(AnyHandle(c.handle));
}

TEST_CASE("cook hdr: max_size halves until the image fits; mips and exposure apply") {
    const std::string hdr = hdr_fixtures::make_rgbe_hdr(64, 32, hot_sky());
    {
        Cooked c;
        cook_hdr(c, "cook_hdr_max", ".hdr", hdr, "{ \"max_size\": 16 }");
        INFO(c.diags);
        REQUIRE(c.ok);
        CHECK(c.view.header->width == 16);  // 64x32 -> 32x16 -> 16x8
        CHECK(c.view.header->height == 8);
        CHECK(c.view.mips.size() == 5);
        // The hot patch (x 16..23, y 14..17) survives the box filter at 1/4: x 4..5, y 3..4 averaged with neighbors.
        CHECK(texel(c, 0, 5, 3, 0) > 5.0f);
        c.as.release(AnyHandle(c.handle));
    }
    {
        Cooked c;  // a 64x32 sky under the default 2048 is untouched; a max_size of 64 keeps it
        cook_hdr(c, "cook_hdr_max_fit", ".hdr", hdr, "{ \"max_size\": 64 }");
        REQUIRE(c.ok);
        CHECK(c.view.header->width == 64);
        c.as.release(AnyHandle(c.handle));
    }
    {
        Cooked c;  // a tall image: the largest side decides
        cook_hdr(c, "cook_hdr_tall", ".hdr", hdr_fixtures::make_rgbe_hdr(4, 16, std::vector<float>(64, 1.0f)), "{ \"max_size\": 4, \"mips\": false }");
        REQUIRE(c.ok);
        CHECK(c.view.header->width == 1);
        CHECK(c.view.header->height == 4);
        CHECK(c.view.mips.size() == 1);
        c.as.release(AnyHandle(c.handle));
    }
    {
        Cooked c;  // exposure in EV: +2 is x4, applied before the half clamp
        cook_hdr(c, "cook_hdr_exposure", ".hdr", hdr, "{ \"exposure\": 2, \"mips\": false }");
        REQUIRE(c.ok);
        CHECK(texel(c, 0, 2, 2, 0) == doctest::Approx(1.0f).epsilon(1e-3));
        CHECK(texel(c, 0, 19, 15, 1) == doctest::Approx(200.0f).epsilon(1e-3));
        c.as.release(AnyHandle(c.handle));
    }
    {
        Cooked c;  // box filter in float: one bright texel of 100 averages to 25, then 12.5 (8-bit would clamp at 1)
        std::vector<float> g(4 * 2, 0.0f);
        g[0] = 100.0f;
        cook_hdr(c, "cook_hdr_mip_avg", ".hdr", hdr_fixtures::make_rgbe_hdr(4, 2, g));
        REQUIRE(c.ok);
        REQUIRE(c.view.mips.size() == 3);  // 4x2, 2x1, 1x1
        CHECK(texel(c, 1, 0, 0, 0) == 25.0f);
        CHECK(texel(c, 1, 1, 0, 0) == 0.0f);
        CHECK(texel(c, 2, 0, 0, 0) == 12.5f);
        c.as.release(AnyHandle(c.handle));
    }
}

TEST_CASE("cook hdr: bad settings are refused with a message that says what to change") {
    const std::string hdr = hdr_fixtures::make_rgbe_hdr(8, 4, std::vector<float>(32, 1.0f));
    {
        Cooked c;
        cook_hdr(c, "cook_hdr_srgb", ".hdr", hdr, "{ \"srgb\": true }");
        CHECK_FALSE(c.ok);
        CHECK(c.diags.find("srgb") != std::string::npos);
        CHECK(c.diags.find("remove") != std::string::npos);  // what to change
        CHECK(c.diags.find("sky.hdr") != std::string::npos);
    }
    {
        Cooked c;  // explicit false is fine (it is what an HDR is)
        cook_hdr(c, "cook_hdr_srgb_false", ".exr", hdr_fixtures::make_exr(8, 4, to_rgba(std::vector<float>(32, 1.0f))), "{ \"srgb\": false }");
        INFO(c.diags);
        CHECK(c.ok);
        if (c.ok) c.as.release(AnyHandle(c.handle));
    }
    for (const char* bad : {"{ \"max_size\": 100 }", "{ \"max_size\": 0 }", "{ \"max_size\": 32768 }", "{ \"exposure\": 100 }", "{ \"bogus\": 1 }"}) {
        Cooked c;
        cook_hdr(c, "cook_hdr_bad", ".hdr", hdr, bad);
        INFO(bad);
        CHECK_FALSE(c.ok);
    }
    {
        Cooked c;  // not a Radiance file
        cook_hdr(c, "cook_hdr_garbage", ".hdr", "this is not an image");
        CHECK_FALSE(c.ok);
        CHECK(c.diags.find("decode failed") != std::string::npos);
    }
    {
        Cooked c;  // a truncated EXR
        const std::string exr = hdr_fixtures::make_exr(8, 4, to_rgba(std::vector<float>(32, 1.0f)));
        cook_hdr(c, "cook_exr_truncated", ".exr", exr.substr(0, exr.size() / 2));
        CHECK_FALSE(c.ok);
    }
    {
        Cooked c;  // the 8-bit keys do not exist on a PNG: max_size is HDR-only
        cook_hdr(c, "cook_png_max_size", ".png", make_gradient_png(4, 4), "{ \"max_size\": 2 }");
        CHECK_FALSE(c.ok);
    }
}

TEST_CASE("cook hdr: re-cooking is byte-identical and the phone cook keeps RGBA16F") {
    Cooked a, b;
    const std::string hdr = hdr_fixtures::make_rgbe_hdr(64, 32, hot_sky());
    cook_hdr(a, "cook_hdr_det_a", ".hdr", hdr);
    cook_hdr(b, "cook_hdr_det_b", ".hdr", hdr);
    REQUIRE(a.ok);
    REQUIRE(b.ok);
    CHECK(read_file(a.root + "/cooked/manifest.mce") == read_file(b.root + "/cooked/manifest.mce"));
    a.as.release(AnyHandle(a.handle));
    b.as.release(AnyHandle(b.handle));

    CookService svc;
    CookOptions astc;
    astc.textures = cook::TextureCompression::Astc4x4;
    const CookReport r = svc.cook(a.root.c_str(), astc);
    INFO(diag_str(r));
    REQUIRE(r.ok());
    AssetSystem as;
    const std::string cooked = a.root + "/cooked";
    const StringView roots[] = {cooked};
    REQUIRE(as.mount(Span<const StringView>(roots, 1)));
    auto th = as.load_sync<Texture>(parse_guid(kHdrGuid).value());
    REQUIRE(th);
    auto tv = TextureView::read(as.resolve(th.value()));
    REQUIRE(tv);
    CHECK(tv.value().header->format == TextureFormat::RGBA16F);
    as.release(AnyHandle(th.value()));
}
