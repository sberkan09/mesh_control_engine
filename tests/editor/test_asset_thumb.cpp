// M-THUMBS 1: CPU asset thumbnails. Exact pixels: an image keeps its proportion and letterboxes, a clear pixel
// shows the checker, a waveform fills min to max per column, a hand-built quad covers a pinned number of pixels,
// the fallback tile depends on the type, and the same input always draws the same picture.

#include <doctest/doctest.h>

#include <cstring>

#include "mce/asset/formats.h"
#include "mce/editor/asset_thumb.h"

using namespace mce;
using namespace mce::editor;

namespace {

cook::Image solid(u32 w, u32 h, u8 r, u8 g, u8 b, u8 a) {
    cook::Image img;
    img.width = w;
    img.height = h;
    img.rgba.resize(static_cast<usize>(w) * h * 4);
    for (usize i = 0; i < img.rgba.size(); i += 4) {
        img.rgba[i] = r;
        img.rgba[i + 1] = g;
        img.rgba[i + 2] = b;
        img.rgba[i + 3] = a;
    }
    return img;
}

u32 count(const cook::Image& img, u8 r, u8 g, u8 b) {
    u32 n = 0;
    for (usize i = 0; i < img.rgba.size(); i += 4) n += (img.rgba[i] == r && img.rgba[i + 1] == g && img.rgba[i + 2] == b) ? 1u : 0u;
    return n;
}

bool same(const cook::Image& a, const cook::Image& b) {
    return a.rgba.size() == b.rgba.size() && std::memcmp(a.rgba.data(), b.rgba.data(), a.rgba.size()) == 0;
}

}  // namespace

TEST_CASE("thumb_from_image: proportion, letterbox and the checker behind clear pixels") {
    cook::Image out;
    thumb_from_image(solid(4, 2, 200, 10, 20, 255), 16, out);
    REQUIRE(out.width == 16);
    REQUIRE(out.height == 16);
    CHECK(count(out, 200, 10, 20) == 16u * 8u);  // 16 x 8 red, the rest letterbox
    CHECK(count(out, 40, 40, 40) == 16u * 8u);
    thumb_from_image(solid(2, 2, 255, 255, 255, 0), 16, out);  // fully clear: the checker alone
    CHECK(count(out, 100, 100, 100) == 128u);
    CHECK(count(out, 150, 150, 150) == 128u);
    // A 1:1 image downscaled 4:1 averages: half the source rows white, half black gives mid grey rows.
    cook::Image two = solid(1, 2, 255, 255, 255, 255);
    two.rgba[4] = two.rgba[5] = two.rgba[6] = 0;
    thumb_from_image(two, 16, out);
    CHECK(count(out, 255, 255, 255) == 4u * 16u);  // 8 x 16 letterboxed; the top half is white
    CHECK(count(out, 0, 0, 0) == 4u * 16u);
}

TEST_CASE("thumb_from_pcm: min to max per column on a dark tile") {
    cook::PcmClip loud;
    loud.sample_rate = 8000;
    loud.channels = 1;
    for (u32 i = 0; i < 100; ++i) loud.samples.push_back(1.0f);
    cook::Image out;
    thumb_from_pcm(loud, 32, out);
    REQUIRE(out.width == 32);
    CHECK(count(out, 90, 200, 140) == 32u * 16u);  // rows 1..16 of every column
    cook::PcmClip quiet;
    quiet.sample_rate = 8000;
    quiet.channels = 1;
    quiet.samples.resize(100);
    thumb_from_pcm(quiet, 32, out);
    CHECK(count(out, 90, 200, 140) == 32u);  // silence is one pixel per column, on the centre line
    CHECK(count(out, 60, 66, 80) == 0u);
}

TEST_CASE("thumb_from_mesh: a quad covers a pinned number of pixels; empty meshes give the tile") {
    asset::MeshHeader header{};
    header.vertex_count = 4;
    header.index_count = 6;
    header.submesh_count = 1;
    asset::MeshVertex verts[4] = {};
    const i16 q[4][2] = {{-32767, -32767}, {32767, -32767}, {32767, 32767}, {-32767, 32767}};
    for (u32 i = 0; i < 4; ++i) {
        verts[i].position[0] = q[i][0];
        verts[i].position[1] = q[i][1];
    }
    const u16 idx[6] = {0, 1, 2, 0, 2, 3};
    asset::MeshSubmesh sm{};
    sm.index_count = 6;
    sm.vertex_count = 4;
    sm.extent[0] = sm.extent[1] = sm.extent[2] = 1.0f;
    asset::MeshView view;
    view.header = &header;
    view.vertices = Span<const asset::MeshVertex>(verts, 4);
    view.indices = Span<const byte>(reinterpret_cast<const byte*>(idx), sizeof(idx));
    view.submeshes = Span<const asset::MeshSubmesh>(&sm, 1);
    cook::Image a, b;
    CHECK(thumb_from_mesh(view, 64, a));
    CHECK(thumb_from_mesh(view, 64, b));
    CHECK(same(a, b));  // a pure function of the mesh
    const u32 background = count(a, 48, 52, 60);
    CHECK(background == 2328u);  // 1768 of 4096 pixels covered by the turned quad

    asset::MeshView empty;
    cook::Image tile;
    CHECK_FALSE(thumb_from_mesh(empty, 64, tile));
    CHECK(count(tile, 48, 52, 60) == 64u * 64u);
}

TEST_CASE("thumb_tile: the colour follows the type, the size is exact") {
    cook::Image a, b;
    thumb_tile(asset::ChunkType::Scene, 4, a);  // below the minimum: clamped to 16
    thumb_tile(asset::ChunkType::Prefab, 4, b);
    CHECK(a.width == 16);
    CHECK(a.height == 16);
    CHECK(a.rgba.size() == 16u * 16u * 4u);
    CHECK_FALSE(same(a, b));
}
