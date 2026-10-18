// TGA and BMP textures (M-IMPORTMEDIA 1): the same pixels in PNG, uncompressed 32-bit TGA (top-left and bottom-left origin),
// RLE 24-bit TGA and 24-bit BMP cook to the same base level; a header that claims more than the cook accepts is refused
// before any pixel allocation; a truncated file is a cook error, never a crash.

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

constexpr const char* kTex = "d2d2d2d2-0000-4000-8000-000000000001";
constexpr u32 kW = 5, kH = 3;

std::vector<u8> pixels(bool alpha) {
    std::vector<u8> px(size_t(kW) * kH * 4);
    for (u32 y = 0; y < kH; ++y)
        for (u32 x = 0; x < kW; ++x) {
            u8* p = &px[(size_t(y) * kW + x) * 4];
            p[0] = u8(x * 50 + 5);
            p[1] = u8(y * 90 + 7);
            p[2] = u8((x * 31 + y * 17) & 255);
            p[3] = alpha ? u8(255 - x * 40 - y * 10) : 255;
        }
    return px;
}

void put16(std::string& s, u32 v) {
    s.push_back(char(v & 255));
    s.push_back(char((v >> 8) & 255));
}
void put32(std::string& s, u32 v) {
    put16(s, v & 0xFFFF);
    put16(s, v >> 16);
}

// image type 2 = uncompressed true colour, 10 = RLE true colour (every run a raw packet of one pixel). `top_left`
// sets descriptor bit 5; rows are written in the order the origin implies.
std::string make_tga(const std::vector<u8>& rgba, u32 bpp, bool top_left, bool rle) {
    std::string s;
    s.push_back(0);            // id length
    s.push_back(0);            // no colour map
    s.push_back(rle ? 10 : 2); // image type
    put16(s, 0), put16(s, 0), s.push_back(0);
    put16(s, 0), put16(s, 0);
    put16(s, kW), put16(s, kH);
    s.push_back(char(bpp));
    s.push_back(char((top_left ? 0x20 : 0) | (bpp == 32 ? 8 : 0)));
    for (u32 r = 0; r < kH; ++r) {
        const u32 y = top_left ? r : kH - 1 - r;
        for (u32 x = 0; x < kW; ++x) {
            const u8* p = &rgba[(size_t(y) * kW + x) * 4];
            if (rle) s.push_back(0);  // a raw packet of one pixel
            s.push_back(char(p[2]));
            s.push_back(char(p[1]));
            s.push_back(char(p[0]));
            if (bpp == 32) s.push_back(char(p[3]));
        }
    }
    return s;
}

// 24-bit BMP, bottom-up, rows padded to four bytes.
std::string make_bmp(const std::vector<u8>& rgba) {
    const u32 stride = (kW * 3 + 3) & ~3u;
    std::string s = "BM";
    put32(s, 54 + stride * kH);
    put32(s, 0);
    put32(s, 54);
    put32(s, 40);
    put32(s, kW);
    put32(s, kH);
    put16(s, 1);
    put16(s, 24);
    put32(s, 0), put32(s, stride * kH), put32(s, 2835), put32(s, 2835), put32(s, 0), put32(s, 0);
    for (u32 r = 0; r < kH; ++r) {
        const u32 y = kH - 1 - r;
        std::string row;
        for (u32 x = 0; x < kW; ++x) {
            const u8* p = &rgba[(size_t(y) * kW + x) * 4];
            row.push_back(char(p[2]));
            row.push_back(char(p[1]));
            row.push_back(char(p[0]));
        }
        while (row.size() < stride) row.push_back(0);
        s += row;
    }
    return s;
}

struct Cooked {
    bool ok = false;
    std::string diags;
    u32 w = 0, h = 0;
    std::vector<u8> base;
};

Cooked cook_one(const char* name, const char* file, const std::string& bytes) {
    Cooked c;
    const std::string root = fresh_dir(name);
    write_file(root + "/assets/" + file, bytes);
    write_file(root + "/assets/" + file + ".mceasset", sidecar(kTex, "texture", "{ \"mips\": false }"));
    CookService svc;
    const CookReport r = svc.cook(root.c_str(), CookOptions{});
    for (const auto& d : r.diags.all()) {
        cook::String line;
        cook::format_diagnostic(d, line);
        c.diags += line.c_str();
        c.diags += "\n";
    }
    c.ok = r.ok();
    if (!c.ok) return c;
    AssetSystem as;
    const std::string cooked = root + "/cooked";
    const StringView roots[] = {cooked};
    REQUIRE(as.mount(Span<const StringView>(roots, 1)));
    auto h = as.load_sync<Texture>(parse_guid(kTex).value());
    REQUIRE(h);
    auto v = TextureView::read(as.resolve(h.value()));
    REQUIRE(v);
    c.w = v.value().header->width;
    c.h = v.value().header->height;
    const Span<const byte> px = v.value().pixels(0, 0);
    c.base.assign(reinterpret_cast<const u8*>(px.data()), reinterpret_cast<const u8*>(px.data()) + px.size());
    as.release(AnyHandle(h.value()));
    return c;
}

}  // namespace

TEST_CASE("TGA and BMP cook to the same texels as the PNG of the same pixels") {
    const std::vector<u8> opaque = pixels(false);
    const Cooked ref = cook_one("fmt_png", "t.png", make_png(kW, kH, opaque));
    INFO(ref.diags);
    REQUIRE(ref.ok);
    REQUIRE(ref.base.size() == size_t(kW) * kH * 4);

    const Cooked tga24 = cook_one("fmt_tga24", "t.tga", make_tga(opaque, 24, false, false));
    INFO(tga24.diags);
    REQUIRE(tga24.ok);
    CHECK(tga24.w == kW);
    CHECK(tga24.h == kH);
    CHECK(tga24.base == ref.base);

    const Cooked tga_top = cook_one("fmt_tga_top", "t.tga", make_tga(opaque, 24, true, false));
    REQUIRE(tga_top.ok);
    CHECK(tga_top.base == ref.base);

    const Cooked tga_rle = cook_one("fmt_tga_rle", "t.tga", make_tga(opaque, 24, false, true));
    INFO(tga_rle.diags);
    REQUIRE(tga_rle.ok);
    CHECK(tga_rle.base == ref.base);

    const Cooked bmp = cook_one("fmt_bmp", "t.bmp", make_bmp(opaque));
    INFO(bmp.diags);
    REQUIRE(bmp.ok);
    CHECK(bmp.base == ref.base);
}

TEST_CASE("a 32-bit TGA keeps its alpha") {
    const std::vector<u8> rgba = pixels(true);
    const Cooked ref = cook_one("fmt_alpha_png", "t.png", make_png(kW, kH, rgba));
    REQUIRE(ref.ok);
    const Cooked tga = cook_one("fmt_alpha_tga", "t.tga", make_tga(rgba, 32, true, false));
    INFO(tga.diags);
    REQUIRE(tga.ok);
    CHECK(tga.base == ref.base);
}

TEST_CASE("a header that claims more than the cook accepts is refused before the pixels are allocated") {
    std::string tga = make_tga(pixels(false), 24, false, false);
    tga[12] = char(0xFF), tga[13] = char(0xFF);  // width 65535
    tga[14] = char(0xFF), tga[15] = char(0xFF);  // height 65535: ~12 GiB if allocated
    const Cooked c = cook_one("fmt_huge_tga", "t.tga", tga);
    CHECK_FALSE(c.ok);
    CHECK(c.diags.find("t.tga") != std::string::npos);

    std::string bmp = make_bmp(pixels(false));
    bmp[18] = char(0xFF), bmp[19] = char(0xFF), bmp[20] = char(0xFF), bmp[21] = char(0x7F);  // width 2^31 - 1
    const Cooked b = cook_one("fmt_huge_bmp", "t.bmp", bmp);
    CHECK_FALSE(b.ok);
}

TEST_CASE("truncated TGA and BMP files are cook errors") {
    const std::string tga = make_tga(pixels(false), 24, false, false);
    const std::string bmp = make_bmp(pixels(false));
    for (size_t cut : {size_t(0), size_t(5), size_t(17), tga.size() - 3}) {
        const Cooked c = cook_one("fmt_trunc_tga", "t.tga", tga.substr(0, cut));
        CHECK_FALSE(c.ok);
    }
    for (size_t cut : {size_t(0), size_t(20), size_t(54), bmp.size() - 5}) {
        const Cooked c = cook_one("fmt_trunc_bmp", "t.bmp", bmp.substr(0, cut));
        CHECK_FALSE(c.ok);
    }
}
