// M-HARDEN3 step 2: encode_png and decode_png (plus image_size and decode_png_grey16) agree on the
// pixels, and the encoder and decoder refuse input they cannot honour.

#include <doctest/doctest.h>

#include <cstring>
#include <utility>

#include "mce/cook/image.h"

using namespace mce;

namespace {

Array<u8> pixels(u32 w, u32 h) {
    Array<u8> out;
    for (u32 y = 0; y < h; ++y)
        for (u32 x = 0; x < w; ++x) {
            out.push_back(static_cast<u8>(x * 37 + y * 11));
            out.push_back(static_cast<u8>(x * 5 + y * 97 + 3));
            out.push_back(static_cast<u8>((x ^ y) * 13));
            out.push_back(static_cast<u8>(255 - (x + y) % 200));  // alpha varies too
        }
    return out;
}

}  // namespace

TEST_CASE("png: encode_png then decode_png gives the same pixels, at several sizes") {
    for (const auto [w, h] : {std::pair<u32, u32>{1, 1}, {7, 3}, {3, 7}, {64, 64}, {129, 2}}) {
        const Array<u8> rgba = pixels(w, h);
        Array<byte> png;
        REQUIRE(cook::encode_png(w, h, rgba.span(), png).has_value());
        REQUIRE(png.size() > 8);
        CHECK(png[1] == byte('P'));  // the PNG signature

        const Result<cook::Image> img = cook::decode_png(png.span());
        REQUIRE(img.has_value());
        CHECK(img.value().width == w);
        CHECK(img.value().height == h);
        REQUIRE(img.value().rgba.size() == rgba.size());
        CHECK(std::memcmp(img.value().rgba.data(), rgba.data(), rgba.size()) == 0);

        u32 iw = 0, ih = 0;
        CHECK(cook::image_size(png.span(), iw, ih));
        CHECK(iw == w);
        CHECK(ih == h);
    }
}

TEST_CASE("png: encode_png refuses an empty image or a buffer of the wrong size, and appends to what is there") {
    Array<byte> out;
    const Array<u8> rgba = pixels(2, 2);
    CHECK_FALSE(cook::encode_png(0, 2, rgba.span(), out).has_value());
    CHECK_FALSE(cook::encode_png(2, 0, rgba.span(), out).has_value());
    CHECK_FALSE(cook::encode_png(2, 2, Span<const u8>(rgba.data(), rgba.size() - 1), out).has_value());
    CHECK_FALSE(cook::encode_png(3, 2, rgba.span(), out).has_value());
    CHECK(out.empty());  // a refusal writes nothing

    REQUIRE(cook::encode_png(2, 2, rgba.span(), out).has_value());
    const usize one = out.size();
    REQUIRE(cook::encode_png(2, 2, rgba.span(), out).has_value());
    CHECK(out.size() == 2 * one);  // "appends", as the header says
}

TEST_CASE("png: decode_png and decode_png_grey16 refuse damaged input") {
    const Array<u8> rgba = pixels(8, 8);
    Array<byte> png;
    REQUIRE(cook::encode_png(8, 8, rgba.span(), png).has_value());

    CHECK_FALSE(cook::decode_png(Span<const byte>()).has_value());
    CHECK_FALSE(cook::decode_png(Span<const byte>(png.data(), 4)).has_value());
    CHECK_FALSE(cook::decode_png(Span<const byte>(png.data(), png.size() / 2)).has_value());
    CHECK_FALSE(cook::decode_png_grey16(Span<const byte>()).has_value());
    u32 w = 0, h = 0;
    CHECK_FALSE(cook::image_size(Span<const byte>(), w, h));

    Array<byte> junk;
    for (u32 i = 0; i < 64; ++i) junk.push_back(byte(i * 3));
    CHECK_FALSE(cook::decode_png(junk.span()).has_value());
}

TEST_CASE("png: decode_png_grey16 widens black and white to the full 16-bit range") {
    Array<u8> rgba;
    for (const u8 v : {u8(0), u8(255)}) {
        rgba.push_back(v);
        rgba.push_back(v);
        rgba.push_back(v);
        rgba.push_back(255);
    }
    Array<byte> png;
    REQUIRE(cook::encode_png(2, 1, rgba.span(), png).has_value());
    const Result<cook::Image16> g = cook::decode_png_grey16(png.span());
    REQUIRE(g.has_value());
    CHECK(g.value().width == 2);
    CHECK(g.value().height == 1);
    REQUIRE(g.value().grey.size() == 2);
    CHECK(g.value().grey[0] == 0);
    CHECK(g.value().grey[1] == 65535);
}
