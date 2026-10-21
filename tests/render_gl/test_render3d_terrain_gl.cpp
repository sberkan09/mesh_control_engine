// M-TERRAIN 1 against a real driver, GL 4.5 and (as test_render_sdlgpu) the SDL GPU backend: TerrainLod
// node meshes upload into the MeshStore and draw through the forward path; neighbours of different
// levels meet without a crack showing the clear color, and the terrain is lit and shadowed.

#include <doctest/doctest.h>

#include <algorithm>
#include <cmath>
#include <cstdio>
#include <cstdlib>

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

// ---- splat layers (M-TERRAIN 2) -----------------------------------------------------------------------

namespace {

// A cooked one-mip RGBA8 texture: `fn(x, y)` gives each texel.
template <class Fn>
Array<byte> cooked_texture(u32 size, Fn fn) {
    Array<byte> px;
    px.resize(usize(size) * size * 4);
    for (u32 y = 0; y < size; ++y)
        for (u32 x = 0; x < size; ++x) {
            const Rgb c = fn(x, y);
            byte* p = &px[(usize(y) * size + x) * 4];
            p[0] = byte(c.r), p[1] = byte(c.g), p[2] = byte(c.b), p[3] = byte(255);
        }
    const asset::TextureMipData mip{size, size, px.span()};
    Array<byte> out;
    asset::write_texture(asset::TextureFormat::RGBA8, 1, 0, Span<const asset::TextureMipData>(&mip, 1), out);
    return out;
}

// 65 samples over 12 m with `height(px, pz)` metres; two layers whose weights `split(px, pz)` gives for
// layer 0 (layer 1 takes the rest), at 65 weight samples per side.
template <class H, class S>
Array<byte> layered_terrain(H height, S split, u32 layer_flags, f32 tile) {
    asset::TerrainHeader h{};
    h.resolution = 65;
    h.chunk = 16;
    h.size[0] = h.size[1] = 12.0f;
    h.height_min = 0.0f;
    h.height_max = 4.0f;
    h.layer_count = 2;
    h.splat_resolution = 65;
    Array<u16> heights;
    heights.resize(65 * 65);
    Array<u8> splat;
    splat.resize(65 * 65 * 8, u8{0});
    for (u32 z = 0; z < 65; ++z)
        for (u32 x = 0; x < 65; ++x) {
            const f32 px = f32(x) * 12.0f / 64.0f - 6.0f, pz = f32(z) * 12.0f / 64.0f - 6.0f;
            heights[z * 65 + x] = u16(std::lround(math::clamp(height(px, pz) / 4.0f, 0.0f, 1.0f) * 65535.0f));
            const u8 w0 = u8(std::lround(math::clamp(split(px, pz), 0.0f, 1.0f) * 255.0f));
            splat[(z * 65 + x) * 4 + 0] = w0;
            splat[(z * 65 + x) * 4 + 1] = u8(255 - w0);
        }
    asset::TerrainLayer layers[2] = {};
    for (asset::TerrainLayer& l : layers) {
        l.tile = tile;
        l.height_strength = 1.0f;
        l.flags = layer_flags;
    }
    Array<byte> payload;
    asset::write_terrain(h, heights.span(), payload, Span<const asset::TerrainLayer>(layers, 2), splat.span());
    return payload;
}

// The terrain's shading with the two albedo textures packed by compose_layer_array, and its node keys
// (every finest node) in the fixture's store.
struct LayeredTerrain {
    TerrainLod lod;
    TerrainShading shading;
    Array<u32> keys;
};
void load(Fixture3D& f, Span<const byte> payload, Span<const byte> tex0, Span<const byte> tex1, LayeredTerrain& out) {
    const asset::TerrainView view = asset::TerrainView::read(payload).value();
    REQUIRE(out.lod.init(view).has_value());
    const asset::TextureView t0 = asset::TextureView::read(tex0).value(), t1 = asset::TextureView::read(tex1).value();
    const asset::TextureView* ptrs[2] = {&t0, &t1};
    LayerArray arr;
    REQUIRE(compose_layer_array(Span<const asset::TextureView* const>(ptrs, 2), arr));
    CHECK(arr.present == 3u);
    TextureStore& store = f.renderer.textures();
    out.shading.albedo_key = store.allocate_key();
    REQUIRE(store.upload(out.shading.albedo_key, 1, arr.upload).has_value());
    TextureMipUpload mip;
    out.shading.splat_key = store.allocate_key();
    REQUIRE(store.upload(out.shading.splat_key, 1, splat_upload(view, mip)).has_value());
    out.shading.layer_count = 2;
    out.shading.size = {12.0f, 12.0f};
    out.shading.splat_resolution = 65;
    for (u32 i = 0; i < 2; ++i) {
        out.shading.layers[i].tile = view.layers[i].tile;
        out.shading.layers[i].triplanar = (view.layers[i].flags & asset::TerrainLayerFlags::kTriplanar) != 0;
        out.shading.layers[i].albedo_map = true;
    }
    Array<byte> blob;
    for (u32 z = 0; z < 4; ++z)
        for (u32 x = 0; x < 4; ++x) {
            out.lod.build_mesh(out.lod.node_index(0, x, z), 0.0f, blob);
            out.keys.push_back(f.renderer.meshes().allocate_key());
            REQUIRE(f.renderer.meshes().upload(out.keys.back(), 1, blob.span()).has_value());
        }
}

void draw_terrain(Fixture3D& f, const LayeredTerrain& t, const Camera3D& cam) {
    f.device->begin_frame(f.frame++);
    Scene3D scene(f.renderer.meshes(), mem::heap());
    scene.begin(cam, {kW, kH});
    DirectionalLight sun;
    sun.direction = {-0.3f, -1.0f, -0.4f};
    sun.intensity = 2.0f;
    sun.ambient_sky = sun.ambient_ground = {0.3f, 0.3f, 0.3f};
    scene.set_light(sun);
    Sky sky;
    sky.enabled = false;
    scene.set_sky(sky);
    PostFx post;
    post.bloom = false;
    scene.set_post(post);
    for (u32 k : t.keys) {
        MeshInstance inst;
        inst.mesh = k;
        inst.terrain = &t.shading;
        scene.add(inst);
    }
    scene.build();
    f.view = scene.view();
    rhi::CommandBuffer cmd;
    f.renderer.record_3d(cmd, scene, f.target);
    f.device->submit(cmd);
    REQUIRE(f.device->read_pixels(f.target, rhi::Rect2D{0, 0, kW, kH}, f.pixels.span()).has_value());
    f.device->end_frame();
}

}  // namespace

TEST_CASE("render3d_terrain: splat layers paint the terrain by their weights") {
    Fixture3D f;
    const Array<byte> red = cooked_texture(8, [](u32, u32) { return Rgb{220, 40, 30}; });
    const Array<byte> blue = cooked_texture(8, [](u32, u32) { return Rgb{30, 60, 220}; });
    // Gentle hills; layer 0 west of x = 0, layer 1 east of it.
    const Array<byte> payload = layered_terrain([](f32 x, f32 z) { return 1.0f + 0.3f * std::sin(x) * std::cos(z); },
                                                [](f32 x, f32) { return x < 0.0f ? 1.0f : 0.0f; }, 0, 2.0f);
    LayeredTerrain t;
    load(f, payload.span(), red.span(), blue.span(), t);
    for (bool blend : {true, false}) {
        t.shading.height_blend = blend;
        draw_terrain(f, t, orbit_camera({0.0f, 1.0f, 0.0f}, 0.0f, math::radians(80.0f), 14.0f));
        const Rgb west = f.at_world({-3.0f, t.lod.height_at(-3.0f, 0.0f), 0.0f});
        const Rgb east = f.at_world({3.0f, t.lod.height_at(3.0f, 0.0f), 0.0f});
        INFO("height blend " << blend << ": west " << int(west.r) << "," << int(west.g) << "," << int(west.b) << " east " << int(east.r)
                             << "," << int(east.g) << "," << int(east.b));
        CHECK(west.r > west.b + 40);
        CHECK(east.b > east.r + 40);
    }
    // A layer's base color multiplies its albedo: layer 1 tinted to black.
    t.shading.layers[1].base_color = {0.0f, 0.0f, 0.0f, 1.0f};
    draw_terrain(f, t, orbit_camera({0.0f, 1.0f, 0.0f}, 0.0f, math::radians(80.0f), 14.0f));
    CHECK(f.at_world({3.0f, t.lod.height_at(3.0f, 0.0f), 0.0f}).luma() < 30);
    CHECK(f.device->validation_error_count() == 0);
}

TEST_CASE("render3d_terrain: triplanar layers keep their texture on a cliff") {
    Fixture3D f;
    // A checker of 4-texel squares over a 16-texel tile: 4 squares per metre each way at a 1 m tile.
    const Array<byte> stripes = cooked_texture(16, [](u32 x, u32 y) {
        const bool on = ((x / 4) + (y / 4)) % 2 == 0;
        return on ? Rgb{240, 240, 240} : Rgb{15, 15, 15};
    });
    // A 3 m wall where z crosses 0 (low side north, z < 0), facing the camera that looks from -z.
    auto cliff = [](f32, f32 z) { return z < 0.0f ? 0.2f : 3.2f; };
    for (bool triplanar : {false, true}) {
        const Array<byte> payload = layered_terrain(cliff, [](f32, f32) { return 1.0f; },
                                                    triplanar ? asset::TerrainLayerFlags::kTriplanar : 0u, 1.0f);
        LayeredTerrain t;
        load(f, payload.span(), stripes.span(), stripes.span(), t);
        // Straight at the wall from 6 m north of it, at mid height.
        draw_terrain(f, t, orbit_camera({0.0f, 1.7f, 0.0f}, math::radians(180.0f), math::radians(5.0f), 6.0f));
        if (const char* dump = std::getenv("MCE_TEST_DUMP_3D")) {
            char path[512];
            std::snprintf(path, sizeof path, "%s.cliff%d.ppm", dump, triplanar ? 1 : 0);
            if (std::FILE* fp = std::fopen(path, "wb")) {
                std::fprintf(fp, "P6\n%u %u\n255\n", kW, kH);
                for (usize i = 0; i < f.pixels.size(); i += 4) std::fwrite(&f.pixels[i], 1, 3, fp);
                std::fclose(fp);
            }
        }
        // Light-dark flips down the wall's middle column: the top projection smears the stripes into
        // vertical bars (no flips), the side projection keeps them.
        auto project = [&](math::vec3 p) {
            const math::vec4 c = f.view.view_proj * math::vec4(p, 1.0f);
            return math::vec2{(c.x / c.w * 0.5f + 0.5f) * kW, (0.5f - c.y / c.w * 0.5f) * kH};
        };
        const math::vec2 top = project({0.3f, 2.9f, 0.0f}), bottom = project({0.3f, 0.5f, 0.0f});
        REQUIRE(top.y >= 0.0f);
        REQUIRE(bottom.y < f32(kH));
        auto red = [&](u32 y) { return int(f.pixels[(usize(y) * kW + u32(top.x)) * 4]); };
        int lo = 255, hi = 0;
        for (u32 y = u32(top.y) + 2; y + 2 < u32(bottom.y); ++y) lo = std::min(lo, red(y)), hi = std::max(hi, red(y));
        u32 flips = 0;
        int last = -1;
        for (u32 y = u32(top.y) + 2; hi - lo > 20 && y + 2 < u32(bottom.y); ++y) {  // the wall is in shade: its own mid-grey
            const int bright = red(y) * 2 > lo + hi ? 1 : 0;
            if (last >= 0 && bright != last) ++flips;
            last = bright;
        }
        INFO("triplanar " << triplanar << ": " << flips << " flips between rows " << top.y << " and " << bottom.y);
        if (triplanar) CHECK(flips >= 4);
        else CHECK(flips <= 1);
    }
    CHECK(f.device->validation_error_count() == 0);
}
