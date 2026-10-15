// M-TERRAIN 1, headless: TerrainLod's quadtree (bounds, errors, skirts), its CDLOD-style selection and
// the node meshes it writes (cooked CHNK_MESH payloads a MeshStore takes as they are).

#include <doctest/doctest.h>

#include <cmath>
#include <cstdio>

#include "mce/render/mesh.h"
#include "mce/render/quality.h"
#include "mce/render/terrain.h"

using namespace mce;
using namespace mce::render;
using math::vec3;

namespace {

// A terrain payload of `res` samples over `size` metres, heights in [0, 40] m from `f(x, z)` (metres).
template <class F>
Array<byte> make_terrain(u32 res, f32 size, u32 chunk, F f) {
    asset::TerrainHeader h{};
    h.resolution = res;
    h.chunk = chunk;
    h.size[0] = h.size[1] = size;
    h.height_min = 0.0f;
    h.height_max = 40.0f;
    Array<u16> heights;
    heights.resize(usize(res) * res);
    for (u32 z = 0; z < res; ++z)
        for (u32 x = 0; x < res; ++x) {
            const f32 px = (static_cast<f32>(x) / static_cast<f32>(res - 1) - 0.5f) * size;
            const f32 pz = (static_cast<f32>(z) / static_cast<f32>(res - 1) - 0.5f) * size;
            const f32 m = math::clamp(f(px, pz), 0.0f, 40.0f);
            heights[usize(z) * res + x] = static_cast<u16>(std::lround(m / 40.0f * 65535.0f));
        }
    Array<byte> out;
    asset::write_terrain(h, heights.span(), out);
    return out;
}

f32 hills(f32 x, f32 z) { return 12.0f + 8.0f * std::sin(x * 0.05f) * std::cos(z * 0.04f) + 3.0f * std::sin(x * 0.31f + z * 0.17f); }

TerrainLod lod_of(const Array<byte>& payload) {
    auto v = asset::TerrainView::read(payload.span());
    REQUIRE(v);
    TerrainLod lod;
    REQUIRE(lod.init(v.value()).has_value());
    return lod;
}

View3D view_at(vec3 eye, vec3 target, rhi::Extent2D vp = {1280, 720}) {
    Camera3D cam;
    cam.position = eye;
    const vec3 d = math::normalize(target - eye);  // yaw about +Y, then pitch about the camera's +X
    cam.orientation = math::quat_from_axis_angle({0.0f, 1.0f, 0.0f}, std::atan2(-d.x, -d.z)) *
                      math::quat_from_axis_angle({1.0f, 0.0f, 0.0f}, std::asin(math::clamp(d.y, -1.0f, 1.0f)));
    cam.near_z = 0.1f;
    cam.far_z = 2000.0f;
    return compute_view_3d(cam, vp);
}

}  // namespace

TEST_CASE("terrain: the quadtree's levels, bounds and errors") {
    const Array<byte> payload = make_terrain(257, 256.0f, 32, hills);
    const TerrainLod lod = lod_of(payload);
    CHECK(lod.levels() == 4);                     // 8 x 8 finest nodes, then 4, 2, 1
    CHECK(lod.node_count() == 64 + 16 + 4 + 1);
    const TerrainNode& root = lod.node(lod.root());
    CHECK(root.level == 3);
    CHECK(root.bounds.min.x == doctest::Approx(-128.0f));
    CHECK(root.bounds.max.z == doctest::Approx(128.0f));
    for (u32 i = 0; i < lod.node_count(); ++i) {
        const TerrainNode& n = lod.node(i);
        INFO(i);
        CHECK(n.bounds.min.y <= n.bounds.max.y);
        CHECK(n.bounds.min.y >= root.bounds.min.y);
        CHECK(n.skirt > n.error);
        if (n.level == 0) {
            CHECK(n.error == 0.0f);
            continue;
        }
        CHECK(n.error > 0.0f);
        for (u32 c = 0; c < 4; ++c) {  // never more exact than a child
            const TerrainNode& child = lod.node(lod.node_index(n.level - 1, n.x * 2 + (c & 1), n.z * 2 + (c >> 1)));
            CHECK(n.error >= child.error);
            CHECK(child.bounds.min.y >= n.bounds.min.y);
            CHECK(child.bounds.max.y <= n.bounds.max.y);
        }
    }
    CHECK(lod.height_at(0.0f, 0.0f) == doctest::Approx(hills(0.0f, 0.0f)).epsilon(0.01));
    const vec3 up = lod.normal_at(128, 128);
    CHECK(up.y > 0.5f);
}

TEST_CASE("terrain: a flat terrain draws as its root from anywhere above it") {
    const TerrainLod lod = lod_of(make_terrain(129, 128.0f, 16, [](f32, f32) { return 5.0f; }));
    for (vec3 eye : {vec3{0, 6, 0}, vec3{50, 30, -40}, vec3{-200, 10, 0}}) {
        Array<u32> nodes;
        lod.select(view_at(eye, {0, 5, 0}), {}, nodes);
        REQUIRE(nodes.size() == 1);
        CHECK(nodes[0] == lod.root());
    }
}

TEST_CASE("terrain: selection covers the terrain once, finer near the camera, coarser far") {
    const TerrainLod lod = lod_of(make_terrain(257, 256.0f, 32, hills));
    const vec3 eye{-120.0f, hills(-120.0f, -120.0f) + 2.0f, -120.0f};
    TerrainSelectParams p;
    p.shadow_reach = 1e9f;  // nothing culled: the whole terrain must be covered
    Array<u32> nodes;
    lod.select(view_at(eye, {0, 10, 0}), p, nodes);
    REQUIRE(!nodes.empty());
    f32 area = 0.0f;
    u32 near_level = 99, far_level = 0;
    for (u32 i : nodes) {
        const TerrainNode& n = lod.node(i);
        area += (n.bounds.max.x - n.bounds.min.x) * (n.bounds.max.z - n.bounds.min.z);
        const f32 d = math::length(vec3{(n.bounds.min.x + n.bounds.max.x) * 0.5f, 0, (n.bounds.min.z + n.bounds.max.z) * 0.5f} -
                                   vec3{eye.x, 0, eye.z});
        if (d < 30.0f) near_level = math::min(near_level, n.level);
        if (d > 150.0f) far_level = math::max(far_level, n.level);
    }
    CHECK(area == doctest::Approx(256.0f * 256.0f));
    CHECK(near_level == 0);
    CHECK(far_level >= 1);
    CHECK(nodes.size() < 64);  // fewer than drawing every finest node

    // A tighter error bound needs more nodes; a looser one fewer.
    Array<u32> tight, loose;
    p.pixel_error = 0.25f;
    lod.select(view_at(eye, {0, 10, 0}), p, tight);
    p.pixel_error = 16.0f;
    lod.select(view_at(eye, {0, 10, 0}), p, loose);
    CHECK(tight.size() > nodes.size());
    CHECK(loose.size() < nodes.size());
}

TEST_CASE("terrain: nodes behind the camera drop out unless they can shadow the view") {
    const TerrainLod lod = lod_of(make_terrain(257, 256.0f, 32, hills));
    const vec3 eye{0.0f, 60.0f, 120.0f};
    const View3D v = view_at(eye, {0.0f, 60.0f, 200.0f});  // looking away from the terrain, off its edge
    Array<u32> culled, shadowing;
    lod.select(v, {}, culled);
    TerrainSelectParams p;
    p.shadow_reach = 80.0f;
    lod.select(v, p, shadowing);
    CHECK(culled.empty());
    CHECK(!shadowing.empty());
}

TEST_CASE("terrain: the world transform moves, turns and scales the selection") {
    const TerrainLod lod = lod_of(make_terrain(257, 256.0f, 32, hills));
    TerrainSelectParams here;
    here.shadow_reach = 1e9f;
    TerrainSelectParams moved = here;
    moved.world = math::translation({1000.0f, 0.0f, 0.0f});
    const View3D v = view_at({-120.0f, 20.0f, -120.0f}, {0, 10, 0});
    const View3D v_moved = view_at({880.0f, 20.0f, -120.0f}, {1000.0f, 10.0f, 0.0f});
    Array<u32> a, b;
    lod.select(v, here, a);
    lod.select(v_moved, moved, b);
    REQUIRE(a.size() == b.size());
    for (usize i = 0; i < a.size(); ++i) CHECK(a[i] == b[i]);
}

TEST_CASE("terrain: a node mesh is a cooked mesh on the surface, with skirts on inner edges only") {
    const TerrainLod lod = lod_of(make_terrain(257, 256.0f, 32, hills));
    MeshStore store;
    store.init(nullptr);
    for (u32 index : {lod.node_index(0, 0, 0), lod.node_index(0, 3, 4), lod.node_index(2, 1, 0), lod.root()}) {
        INFO(index);
        const TerrainNode& n = lod.node(index);
        Array<byte> blob;
        lod.build_mesh(index, 8.0f, blob);
        auto view = asset::MeshView::read(blob.span());
        REQUIRE(view);
        const asset::MeshView& m = view.value();
        REQUIRE(m.submeshes.size() == 1);
        CHECK(m.header->index_count / 3 == lod.node_triangles(index));
        // Every vertex sits on the surface (grid) or a skirt's depth below it (skirts), within quantization.
        const asset::MeshSubmesh& sm = m.submeshes[0];
        u32 on_surface = 0, below = 0;
        for (const asset::MeshVertex& q : m.vertices) {
            const f32 x = sm.center[0] + q.position[0] / 32767.0f * sm.extent[0];
            const f32 y = sm.center[1] + q.position[1] / 32767.0f * sm.extent[1];
            const f32 z = sm.center[2] + q.position[2] / 32767.0f * sm.extent[2];
            const f32 surface = lod.height_at(x, z);
            // Height quantization, plus the slope (under 1.5 here) times the x/z quantization.
            const f32 tol = 2.0f * sm.extent[1] / 32767.0f + 3.0f * math::max(sm.extent[0], sm.extent[2]) / 32767.0f + 0.002f;
            if (std::fabs(y - surface) <= tol) ++on_surface;
            else if (std::fabs(y - (surface - n.skirt)) <= tol) ++below;
        }
        CHECK(on_surface == 33 * 33);
        CHECK(on_surface + below == m.vertices.size());
        const u32 side = 8u >> n.level;
        const u32 inner = (n.x > 0) + (n.x + 1 < side) + (n.z > 0) + (n.z + 1 < side);
        CHECK(below == inner * 33);
        const u32 key = store.allocate_key();
        CHECK(key >= MeshStore::kFirstLocalKey);
        REQUIRE(store.upload(key, 1, blob.span()).has_value());
        REQUIRE(store.get(key) != nullptr);
        CHECK(store.get(key)->triangle_count() == lod.node_triangles(index));
    }
    CHECK(store.live_meshes() == 4);
}

TEST_CASE("terrain: the grid faces up and the skirts face away from their node") {
    const TerrainLod lod = lod_of(make_terrain(129, 128.0f, 16, [](f32, f32) { return 10.0f; }));
    const u32 index = lod.node_index(0, 1, 1);  // an inner node: four skirts
    Array<byte> blob;
    lod.build_mesh(index, 0.0f, blob);
    const asset::MeshView m = asset::MeshView::read(blob.span()).value();
    const asset::MeshSubmesh& sm = m.submeshes[0];
    auto pos = [&](u32 i) {
        const asset::MeshVertex& q = m.vertices[i];
        return vec3{sm.center[0] + q.position[0] / 32767.0f * sm.extent[0], sm.center[1] + q.position[1] / 32767.0f * sm.extent[1],
                    sm.center[2] + q.position[2] / 32767.0f * sm.extent[2]};
    };
    const TerrainNode& n = lod.node(index);
    const vec3 c = (n.bounds.min + n.bounds.max) * 0.5f;
    u32 up = 0, out = 0;
    for (u32 t = 0; t < m.header->index_count; t += 3) {
        const vec3 a = pos(m.index(t)), b = pos(m.index(t + 1)), d = pos(m.index(t + 2));
        const vec3 f = math::cross(b - a, d - a);
        if (std::fabs(f.y) > 1e-4f) {
            CHECK(f.y > 0.0f);
            ++up;
        } else {
            const vec3 mid = (a + b + d) * (1.0f / 3.0f) - c;
            CHECK(f.x * mid.x + f.z * mid.z > 0.0f);
            ++out;
        }
    }
    CHECK(up == 16 * 16 * 2);
    CHECK(out == 4 * 16 * 2);
}

TEST_CASE("terrain: a 513-sample terrain stays inside each quality tier's draw budget") {
    // The budget a tier promises (ROADMAP M-TERRAIN gate): a 512 m terrain seen from 2 m above the
    // ground at 1280x720, looking across it, with 60 m of shadow reach. The hills carry 3 m bumps every
    // 20 m, a worst case for the error bound: smoother ground draws far less.
    const TerrainLod lod = lod_of(make_terrain(513, 512.0f, 32, hills));
    const vec3 eye{-230.0f, hills(-230.0f, -230.0f) + 2.0f, -230.0f};
    struct Budget {
        QualityTier tier;
        u32 max_nodes;
        u32 max_triangles;
    };
    const Budget budgets[] = {{QualityTier::Low, 64, 150000}, {QualityTier::Medium, 96, 220000}, {QualityTier::High, 144, 330000}};
    u32 prev_tris = 0;
    for (const Budget& b : budgets) {
        TerrainSelectParams p;
        p.pixel_error = quality_settings(b.tier).terrain_pixel_error;
        p.shadow_reach = 60.0f;
        Array<u32> nodes;
        lod.select(view_at(eye, {0.0f, 12.0f, 0.0f}), p, nodes);
        u32 tris = 0;
        for (u32 n : nodes) tris += lod.node_triangles(n);
        std::printf("terrain budget %s: %u nodes, %u triangles\n", quality_tier_name(b.tier), static_cast<u32>(nodes.size()), tris);
        INFO(quality_tier_name(b.tier));
        CHECK(nodes.size() <= b.max_nodes);
        CHECK(tris <= b.max_triangles);
        CHECK(tris >= prev_tris);  // a finer tier never draws less
        prev_tris = tris;
    }
}

TEST_CASE("terrain: compose_layer_array packs matching layer textures and leaves the rest out") {
    // Cooked RGBA8 textures of one value; 8x8 with 2 mips, or a 4x4 odd one out.
    auto texture = [](u32 size, u32 mips, u8 value, Array<byte>& out) {
        Array<byte> px[2];
        asset::TextureMipData data[2];
        for (u32 m = 0; m < mips; ++m) {
            const u32 s = size >> m;
            px[m].resize(usize(s) * s * 4, byte(value));
            data[m] = {s, s, px[m].span()};
        }
        asset::write_texture(asset::TextureFormat::RGBA8, 1, asset::TextureFlags::Srgb, Span<const asset::TextureMipData>(data, mips), out);
    };
    Array<byte> a, b, odd;
    texture(8, 2, 10, a);
    texture(8, 2, 20, b);
    texture(4, 1, 30, odd);
    const asset::TextureView va = asset::TextureView::read(a.span()).value(), vb = asset::TextureView::read(b.span()).value(),
                             vo = asset::TextureView::read(odd.span()).value();
    const asset::TextureView* layers[4] = {&va, nullptr, &vo, &vb};
    render::LayerArray arr;
    REQUIRE(render::compose_layer_array(Span<const asset::TextureView* const>(layers, 4), arr));
    CHECK(arr.present == 0b1001u);  // layer 1 has none, layer 2's does not match layer 0's
    CHECK(arr.upload.layers == 4);
    CHECK(arr.upload.format == rhi::TextureFormat::RGBA8_SRGB);
    REQUIRE(arr.upload.mips.size() == 2);
    const render::TextureMipUpload& m1 = arr.upload.mips[1];
    CHECK(m1.width == 4);
    REQUIRE(m1.pixels.size() == 4 * 4 * 4 * 4);
    const usize layer = 4 * 4 * 4;
    CHECK(u8(m1.pixels[0]) == 10);
    CHECK(u8(m1.pixels[layer]) == 0);      // missing: zero
    CHECK(u8(m1.pixels[2 * layer]) == 0);  // left out
    CHECK(u8(m1.pixels[3 * layer + 5]) == 20);

    const asset::TextureView* none[2] = {nullptr, nullptr};
    CHECK_FALSE(render::compose_layer_array(Span<const asset::TextureView* const>(none, 2), arr));
    CHECK(arr.present == 0u);
}
