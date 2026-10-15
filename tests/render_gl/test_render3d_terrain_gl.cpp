// M-TERRAIN 1 against a real driver, GL 4.5 and (as test_render_sdlgpu) the SDL GPU backend: TerrainLod
// node meshes upload into the MeshStore and draw through the forward path; neighbours of different
// levels meet without a crack showing the clear color, and the terrain is lit and shadowed.

#include <doctest/doctest.h>

#include <cmath>

#include "fixture3d.h"
#include "mce/render/terrain.h"

using namespace mce;
using namespace mce::render;
using namespace mce::render_test;

namespace {

// 65 samples over 12 m (4 x 4 finest nodes of 16 quads): ridges along x, 1.6 m waves along z, so a
// coarser node's edge misses its finer neighbour's by up to ~0.3 m.
TerrainLod bumpy_terrain() {
    asset::TerrainHeader h{};
    h.resolution = 65;
    h.chunk = 16;
    h.size[0] = h.size[1] = 12.0f;
    h.height_min = 0.0f;
    h.height_max = 3.0f;
    Array<u16> heights;
    heights.resize(65 * 65);
    for (u32 z = 0; z < 65; ++z)
        for (u32 x = 0; x < 65; ++x) {
            const f32 px = static_cast<f32>(x) * 12.0f / 64.0f - 6.0f, pz = static_cast<f32>(z) * 12.0f / 64.0f;
            const f32 t = 0.5f + 0.4f * std::cos(pz * 4.0f) + 0.1f * std::sin(px * 0.5f);
            heights[z * 65 + x] = static_cast<u16>(t * 65535.0f);
        }
    Array<byte> payload;
    asset::write_terrain(h, heights.span(), payload);
    TerrainLod lod;
    REQUIRE(lod.init(asset::TerrainView::read(payload.span()).value()).has_value());
    return lod;
}

}  // namespace

TEST_CASE("render3d_terrain: mixed-level nodes draw without cracks, lit from the sun") {
    Fixture3D f;
    const TerrainLod lod = bumpy_terrain();
    // The west half at the finest level, the east half one level coarser: T-junctions along x = 0.
    Array<u32> nodes;
    for (u32 z = 0; z < 4; ++z)
        for (u32 x = 0; x < 2; ++x) nodes.push_back(lod.node_index(0, x, z));
    for (u32 z = 0; z < 2; ++z) nodes.push_back(lod.node_index(1, 1, z));
    Array<u32> keys;
    Array<byte> blob;
    for (u32 n : nodes) {
        lod.build_mesh(n, 0.0f, blob);
        keys.push_back(f.renderer.meshes().allocate_key());
        REQUIRE(f.renderer.meshes().upload(keys.back(), 1, blob.span()).has_value());
    }

    auto draw = [&](f32 pitch_deg, bool lit, f32 yaw = 0.3f, f32 distance = 13.0f) {
        f.device->begin_frame(f.frame++);
        Scene3D scene(f.renderer.meshes(), mem::heap());
        scene.begin(orbit_camera({0.0f, 1.5f, 0.0f}, yaw, math::radians(pitch_deg), distance), {kW, kH});
        DirectionalLight sun;
        sun.direction = {-0.5f, -1.0f, -0.3f};
        sun.intensity = lit ? 3.0f : 0.0f;
        sun.ambient_sky = sun.ambient_ground = lit ? math::vec3{0.1f, 0.1f, 0.1f} : math::vec3{0.0f, 0.0f, 0.0f};
        scene.set_light(sun);
        Fog fog;
        fog.color = {1.0f, 0.0f, 1.0f};  // the clear color: any magenta pixel is a hole in the terrain
        scene.set_fog(fog);
        Sky sky;
        sky.enabled = false;
        scene.set_sky(sky);
        PostFx post;
        post.bloom = false;
        scene.set_post(post);
        ShadowSettings ss;
        ss.resolution = 1024;
        scene.set_shadows(ss);
        for (u32 k : keys) {
            MeshInstance inst;
            inst.mesh = k;
            inst.tint = {0.4f, 0.9f, 0.3f, 1.0f};
            scene.add(inst);
        }
        scene.build();
        f.view = scene.view();
        rhi::CommandBuffer cmd;
        f.renderer.record_3d(cmd, scene, f.target);
        f.device->submit(cmd);
        REQUIRE(f.device->read_pixels(f.target, rhi::Rect2D{0, 0, kW, kH}, f.pixels.span()).has_value());
        f.device->end_frame();
        if (const char* dump = std::getenv("MCE_TEST_DUMP_3D")) {  // eyeballing aid, as Fixture3D::render
            char path[512];
            std::snprintf(path, sizeof path, "%s.%u.ppm", dump, static_cast<u32>(pitch_deg));
            if (std::FILE* fp = std::fopen(path, "wb")) {
                std::fprintf(fp, "P6\n%u %u\n255\n", kW, kH);
                for (usize i = 0; i < f.pixels.size(); i += 4) std::fwrite(&f.pixels[i], 1, 3, fp);
                std::fclose(fp);
            }
        }
    };

    // Straight down (almost): the middle of the frame is all terrain, no magenta anywhere in it.
    draw(88.0f, true);
    u32 holes = 0, terrain = 0;
    for (u32 y = kH / 5; y < kH * 4 / 5; ++y)
        for (u32 x = kW / 5; x < kW * 4 / 5; ++x) {
            const u8* p = &f.pixels[(usize(y) * kW + x) * 4];
            if (p[0] > 200 && p[1] < 60 && p[2] > 200) ++holes;
            else if (p[1] > p[0] && p[1] > p[2]) ++terrain;
        }
    CHECK(holes == 0);
    CHECK(terrain > (kW * 3 / 5) * (kH * 3 / 5) * 9 / 10);

    // An oblique view from close above, across the T-junctions (looking along x): the frame holds only
    // terrain, so a magenta pixel is a crack between two nodes seen through to the clear color.
    draw(60.0f, true, 1.5708f, 4.0f);
    holes = 0;
    for (usize i = 0; i < f.pixels.size(); i += 4) holes += f.pixels[i] > 200 && f.pixels[i + 1] < 60 && f.pixels[i + 2] > 200;
    CHECK(holes == 0);

    // From afar, the clear color around the terrain.
    draw(40.0f, true, 1.5708f);
    const Rgb sky = {f.pixels[(5 * kW + kW / 2) * 4], f.pixels[(5 * kW + kW / 2) * 4 + 1], f.pixels[(5 * kW + kW / 2) * 4 + 2]};
    CHECK(sky.r > 200);
    CHECK(sky.b > 200);

    // The sun lights it: dark without the sun, green with it.
    const Rgb lit_px = f.at_world({-2.0f, lod.height_at(-2.0f, 1.0f), 1.0f});
    draw(40.0f, false, 1.5708f);
    const Rgb dark_px = f.at_world({-2.0f, lod.height_at(-2.0f, 1.0f), 1.0f});
    CHECK(lit_px.luma() > dark_px.luma() + 20);
    CHECK(lit_px.g > lit_px.r);
    CHECK(f.device->validation_error_count() == 0);
}
