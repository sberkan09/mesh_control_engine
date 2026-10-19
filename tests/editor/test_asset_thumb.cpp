// M-THUMBS 1: CPU asset thumbnails. Exact pixels: an image keeps its proportion and letterboxes, a clear pixel
// shows the checker, a waveform fills min to max per column, a hand-built quad covers a pinned number of pixels,
// the fallback tile depends on the type, and the same input always draws the same picture.

#include <doctest/doctest.h>

#include <algorithm>
#include <cstring>
#include <filesystem>
#include <fstream>

#include "cook_test_util.h"
#include "mce/asset/formats.h"
#include "mce/editor/asset_thumb.h"
#include "mce/editor/project.h"

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

namespace {

u32 sphere_pixels(const cook::Image& img) {
    u32 n = 0;
    for (usize i = 0; i < img.rgba.size(); i += 4)
        n += (img.rgba[i] == 40 && img.rgba[i + 1] == 40 && img.rgba[i + 2] == 40) ? 0u : 1u;
    return n;
}

u32 brightest(const cook::Image& img) {
    u32 best = 0;
    for (usize i = 0; i < img.rgba.size(); i += 4) best = std::max<u32>(best, std::max<u32>(img.rgba[i], std::max<u32>(img.rgba[i + 1], img.rgba[i + 2])));
    return best;
}

const u8* center(const cook::Image& img) { return &img.rgba[(static_cast<usize>(img.height / 2) * img.width + img.width / 2) * 4]; }

}  // namespace

TEST_CASE("thumb_from_material: a lit sphere, smooth is shinier than rough, metal tints, emissive glows, blend shows the checker") {
    cook::Image out;
    MaterialSwatch m;
    thumb_from_material(m, 64, out);
    REQUIRE(out.width == 64);
    REQUIRE(out.height == 64);
    const u32 covered = sphere_pixels(out);
    CHECK(covered > 2000);  // a disc of radius 0.42 * 64 covers about 2270 of 4096
    CHECK(covered < 2500);
    CHECK(out.rgba[0] == 40);  // the corner is the background

    MaterialSwatch smooth, rough;
    smooth.roughness = 0.1f;
    rough.roughness = 0.9f;
    smooth.base[0] = smooth.base[1] = smooth.base[2] = rough.base[0] = rough.base[1] = rough.base[2] = 0.1f;  // dark, so nothing clips
    cook::Image a, b;
    thumb_from_material(smooth, 64, a);
    thumb_from_material(rough, 64, b);
    CHECK(brightest(a) > brightest(b));
    CHECK(sphere_pixels(a) == covered);  // the silhouette does not depend on the material

    MaterialSwatch gold;  // a red metal reflects red only
    gold.base[0] = 1.0f;
    gold.base[1] = 0.0f;
    gold.base[2] = 0.0f;
    gold.metallic = 1.0f;
    gold.roughness = 0.3f;
    thumb_from_material(gold, 64, a);
    CHECK(center(a)[0] > center(a)[1]);
    CHECK(center(a)[0] > center(a)[2]);

    MaterialSwatch glow;
    glow.base[0] = glow.base[1] = glow.base[2] = 0.0f;
    glow.emissive[1] = 1.0f;
    thumb_from_material(glow, 64, a);
    CHECK(center(a)[1] > 200);
    CHECK(center(a)[0] < 60);

    MaterialSwatch unlit;
    unlit.unlit = true;
    unlit.base[0] = 0.0f;
    unlit.base[1] = 0.0f;
    unlit.base[2] = 1.0f;
    thumb_from_material(unlit, 64, a);
    CHECK(center(a)[2] == 255);
    CHECK(center(a)[0] == 0);

    MaterialSwatch glass;
    glass.blend = true;
    glass.base[3] = 0.0f;
    thumb_from_material(glass, 64, a);
    CHECK(count(a, 100, 100, 100) + count(a, 150, 150, 150) + count(a, 40, 40, 40) == 64u * 64u);  // only the checker shows
    thumb_from_material(m, 64, b);
    CHECK(same(b, out));  // deterministic
}

TEST_CASE("render_thumbnail: a .mcmat file draws its sphere, other kinds the plain tile") {
    const std::string root = cook_test::fresh_dir("thumb_material");
    std::filesystem::create_directories(root + "/assets/mat");
    std::ofstream(root + "/assets/mat/gold.mcmat") << R"({ "format": "mce-material", "version": 1, "guid": "5ce4e000-0000-4000-8000-0000000000a1",
  "base_color": [1.0, 0.0, 0.0, 1.0], "metallic": 1.0, "roughness": 0.3 })";
    EditorProject project;
    REQUIRE(project.open(root));
    const ProjectAsset* gold = nullptr;
    for (const ProjectAsset& a : project.assets()) {
        if (a.path.view() == "mat/gold.mcmat") gold = &a;
    }
    REQUIRE(gold);
    CHECK(gold->type == asset::ChunkType::Material);
    cook::Image out;
    CHECK(render_thumbnail(project, *gold, 64, out));
    CHECK(center(out)[0] > center(out)[1]);
    CHECK(sphere_pixels(out) > 2000);
}

TEST_CASE("thumb_from_particles: a straight jet is one column, an empty preset is the background, the cap and the colours show") {
    ParticleSwatch jet;  // 20 particles straight up at 1 to 3 m/s, lasting 5 s, 0.1 m wide
    jet.burst = 20;
    jet.lifetime[0] = jet.lifetime[1] = 5.0f;
    jet.speed[0] = 1.0f;
    jet.speed[1] = 3.0f;
    jet.size[0] = jet.size[1] = 0.1f;
    jet.color_start[0] = jet.color_end[0] = 1.0f;
    jet.color_start[1] = jet.color_end[1] = 0.0f;
    jet.color_start[2] = jet.color_end[2] = 0.0f;
    cook::Image out;
    thumb_from_particles(jet, 64, out);
    REQUIRE(out.width == 64);
    REQUIRE(out.height == 64);
    const u32 lit = sphere_pixels(out);
    CHECK(lit > 0);
    // Every lit pixel is in the same few columns (the jet has no spread and no sideways velocity).
    u32 min_x = 64, max_x = 0, min_y = 64, max_y = 0;
    for (u32 y = 0; y < 64; ++y)
        for (u32 x = 0; x < 64; ++x) {
            const u8* px = &out.rgba[(static_cast<usize>(y) * 64 + x) * 4];
            if (px[0] == 40 && px[1] == 40 && px[2] == 40) continue;
            min_x = std::min(min_x, x);
            max_x = std::max(max_x, x);
            min_y = std::min(min_y, y);
            max_y = std::max(max_y, y);
            CHECK(px[0] > px[1]);  // red dominates
        }
    CHECK(max_x - min_x <= 4);
    CHECK(max_y - min_y > 20);  // and it runs up the picture
    cook::Image again;
    thumb_from_particles(jet, 64, again);
    CHECK(same(out, again));

    ParticleSwatch nothing;
    thumb_from_particles(nothing, 64, out);
    CHECK(sphere_pixels(out) == 0);

    ParticleSwatch capped = jet;
    capped.max_particles = 1;
    cook::Image one;
    thumb_from_particles(capped, 64, one);
    CHECK(sphere_pixels(one) > 0);
    CHECK(sphere_pixels(one) < lit);

    ParticleSwatch fountain;  // rate-driven, spread and gravity: a wider picture than the jet
    fountain.rate = 60;
    fountain.spread = 0.6f;
    fountain.lifetime[0] = fountain.lifetime[1] = 1.0f;
    fountain.speed[0] = 2.0f;
    fountain.speed[1] = 3.0f;
    fountain.gravity[1] = -9.8f;
    fountain.size[0] = fountain.size[1] = 0.05f;
    thumb_from_particles(fountain, 64, out);
    u32 fx0 = 64, fx1 = 0;
    for (u32 y = 0; y < 64; ++y)
        for (u32 x = 0; x < 64; ++x)
            if (out.rgba[(static_cast<usize>(y) * 64 + x) * 4] != 40) {
                fx0 = std::min(fx0, x);
                fx1 = std::max(fx1, x);
            }
    CHECK(fx1 - fx0 > 10);
}

TEST_CASE("render_thumbnail: a .mcpart file draws its particles") {
    const std::string root = cook_test::fresh_dir("thumb_particles");
    std::filesystem::create_directories(root + "/assets/fx");
    std::ofstream(root + "/assets/fx/jet.mcpart") << R"({ "format": "mce-particles", "version": 1, "guid": "5ce4e000-0000-4000-8000-0000000000b1",
  "burst": 20, "lifetime": [5, 5], "speed": [2, 2], "direction": 90, "size": [0.1, 0.1], "color_start": [1, 0, 0, 1], "color_end": [1, 0, 0, 1] })";
    EditorProject project;
    REQUIRE(project.open(root));
    const ProjectAsset* jet = nullptr;
    for (const ProjectAsset& a : project.assets())
        if (a.path.view() == "fx/jet.mcpart") jet = &a;
    REQUIRE(jet);
    CHECK(jet->type == asset::ChunkType::ParticlePreset);
    cook::Image out;
    CHECK(render_thumbnail(project, *jet, 64, out));
    CHECK(sphere_pixels(out) > 0);
    CHECK(sphere_pixels(out) < 400);
}
