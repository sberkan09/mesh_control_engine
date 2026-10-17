// Static merge (M-VISUAL3 5b), headless: build_merged_mesh() puts many different rigid meshes into one payload with
// one submesh per material, at their world positions, tints folded into the material, LOD levels unioned, 32-bit
// indices when a submesh outgrows 16 bits, skinned sources left out; the result uploads and draws as an ordinary
// mesh (draws = materials x cells).

#include <doctest/doctest.h>

#include <chrono>
#include <cmath>

#include "mce/render/mesh.h"
#include "mce/render/scene3d.h"
#include "mce/render/static_merge.h"
#include "mesh_fixture.h"

using namespace mce;
using namespace mce::render;
using math::vec3;

namespace {

struct Piece {
    Array<byte> blob;
    asset::MeshView view;
};

// A box mesh `half` big whose node sits at `at` (the pieces of a kit are authored around their own origin).
Piece make_piece(vec3 half, f32 r, f32 g, f32 b, vec3 at = {}, u32 flags = 0, u32 lod_triangles = 0, f32 lod_error = 0.0f) {
    mesh_fixture::Builder bld;
    bld.add_node(-1, at);
    mesh_fixture::Box box;
    box.center = {0, half.y, 0};
    box.half = half;
    box.color[0] = r;
    box.color[1] = g;
    box.color[2] = b;
    box.flags = flags;
    box.uvs = true;
    bld.add_box(box);
    if (lod_triangles > 0) bld.add_lod(0, lod_triangles, lod_error);
    Piece p;
    p.blob = bld.build();
    const Result<asset::MeshView> v = asset::MeshView::read(p.blob.span());
    REQUIRE(v.has_value());
    p.view = v.value();
    return p;
}

Piece skinned_piece() {
    mesh_fixture::Builder b;
    const u32 root = b.add_node(-1);
    const u32 a = b.add_node(static_cast<i32>(root));
    const u32 j = b.add_node(static_cast<i32>(a), {0, 1, 0});
    mesh_fixture::Box bone;
    bone.center = {0, 1, 0};
    bone.half = {0.2f, 1.0f, 0.2f};
    bone.node = root;
    b.add_box(bone);
    b.skin_submesh(0, {a, j}, [](vec3 p) { return p.y < 1.0f ? 0u : 1u; });
    Piece p;
    p.blob = b.build();
    p.view = asset::MeshView::read(p.blob.span()).value();
    return p;
}

MergeSource src(const Piece& p, vec3 at, f32 yaw = 0.0f, f32 scale = 1.0f) {
    MergeSource s;
    s.mesh = &p.view;
    s.world = math::trs(at, math::quat_from_axis_angle({0, 1, 0}, yaw), {scale, scale, scale});
    return s;
}

// The dequantized bounds of submesh `s` of a merged payload.
math::aabb3 sub_bounds(const asset::MeshView& v, u32 s) {
    const asset::MeshSubmesh& sm = v.submeshes[s];
    math::aabb3 b{{1e9f, 1e9f, 1e9f}, {-1e9f, -1e9f, -1e9f}};
    for (u32 i = 0; i < sm.vertex_count; ++i) {
        const asset::MeshVertex& q = v.vertices[sm.base_vertex + i];
        const vec3 p{sm.center[0] + q.position[0] / 32767.0f * sm.extent[0], sm.center[1] + q.position[1] / 32767.0f * sm.extent[1],
                     sm.center[2] + q.position[2] / 32767.0f * sm.extent[2]};
        b.min = math::min(b.min, p);
        b.max = math::max(b.max, p);
    }
    return b;
}

Camera3D camera() {
    Camera3D c;
    c.near_z = 0.1f;
    c.far_z = 300.0f;
    return c;
}

}  // namespace

TEST_CASE("static merge: pieces with one material become one submesh at their world positions") {
    const Piece wall = make_piece({1.0f, 1.0f, 0.1f}, 0.8f, 0.2f, 0.2f);
    const Piece pillar = make_piece({0.2f, 1.5f, 0.2f}, 0.8f, 0.2f, 0.2f);
    const Piece floor = make_piece({1.0f, 0.1f, 1.0f}, 0.2f, 0.8f, 0.2f);
    const MergeSource s[] = {src(wall, {0, 0, 0}), src(pillar, {3, 0, 1}), src(floor, {-4, 0, 2}), src(wall, {0, 0, 6}, 1.5707963f)};
    Array<byte> blob;
    MergeStats st;
    REQUIRE(build_merged_mesh(Span<const MergeSource>(s, 4), blob, &st));
    const Result<asset::MeshView> r = asset::MeshView::read(blob.span());
    REQUIRE(r.has_value());
    const asset::MeshView& v = r.value();
    CHECK(st.sources == 4);
    CHECK(st.skipped == 0);
    CHECK(st.submeshes == 2);  // red, green
    REQUIRE(v.submeshes.size() == 2);
    CHECK(v.header->vertex_count == 4 * 24);
    CHECK(st.triangles == 4 * 12);
    CHECK(v.submeshes[0].index_count == 3 * 36);  // wall, pillar, wall
    CHECK(v.submeshes[1].index_count == 36);
    CHECK(v.submeshes[0].material.base_color[0] == doctest::Approx(0.8f));
    CHECK(v.submeshes[1].material.base_color[1] == doctest::Approx(0.8f));
    CHECK(!v.index32());
    CHECK(v.nodes.size() == 1);

    // Red: wall at the origin (x -1..1, y 0..2, z -0.1..0.1), the pillar, and the turned wall at z = 6 (its x span is
    // now along z: x -0.1..0.1, z 5..7).
    const math::aabb3 red = sub_bounds(v, 0);
    CHECK(red.min.x == doctest::Approx(-1.0f).epsilon(0.0005));
    CHECK(red.max.x == doctest::Approx(3.2f).epsilon(0.0005));
    CHECK(red.min.y == doctest::Approx(0.0f).epsilon(0.0005));
    CHECK(red.max.y == doctest::Approx(3.0f).epsilon(0.0005));
    CHECK(red.min.z == doctest::Approx(-0.1f).epsilon(0.002));
    CHECK(red.max.z == doctest::Approx(7.0f).epsilon(0.0005));
    const math::aabb3 green = sub_bounds(v, 1);
    CHECK(green.min.x == doctest::Approx(-5.0f).epsilon(0.0005));
    CHECK(green.max.x == doctest::Approx(-3.0f).epsilon(0.0005));
    CHECK(green.max.y == doctest::Approx(0.2f).epsilon(0.002));
    CHECK(green.min.z == doctest::Approx(1.0f).epsilon(0.0005));
    CHECK(v.header->bounds_min[0] == doctest::Approx(-5.0f).epsilon(0.0005));
    CHECK(v.header->bounds_max[2] == doctest::Approx(7.0f).epsilon(0.0005));

    // Deterministic: the same sources give the same bytes.
    Array<byte> again;
    REQUIRE(build_merged_mesh(Span<const MergeSource>(s, 4), again));
    REQUIRE(again.size() == blob.size());
    CHECK(std::memcmp(again.data(), blob.data(), blob.size()) == 0);

    // Normals follow the turned wall: its +Z face (octahedral (0,0)) now faces +X, so some vertex normal points +X
    // and none was left on the wrong side of the world transform (the wall's two big faces are +-Z, then +-X).
    u32 along_x = 0;
    for (u32 i = 0; i < v.submeshes[0].vertex_count; ++i) {
        const asset::MeshVertex& q = v.vertices[v.submeshes[0].base_vertex + i];
        if (q.normal[0] > 32000 && std::abs(q.normal[1]) < 600) ++along_x;  // +X: octahedral (1, 0)
    }
    CHECK(along_x >= 8);
}

TEST_CASE("static merge: a tint is folded into the material and splits the merge key") {
    const Piece a = make_piece({0.5f, 0.5f, 0.5f}, 0.8f, 0.8f, 0.8f);
    MergeSource s[3] = {src(a, {0, 0, 0}), src(a, {2, 0, 0}), src(a, {4, 0, 0})};
    s[1].tint = {0.5f, 0.25f, 1.0f, 1.0f};
    s[2].tint = {0.5f, 0.25f, 1.0f, 1.0f};  // equal tints share a submesh
    s[2].emissive_tint = {1, 1, 1};
    Array<byte> blob;
    REQUIRE(build_merged_mesh(Span<const MergeSource>(s, 3), blob));
    const asset::MeshView v = asset::MeshView::read(blob.span()).value();
    REQUIRE(v.submeshes.size() == 2);
    CHECK(v.submeshes[0].material.base_color[0] == doctest::Approx(0.8f));
    CHECK(v.submeshes[1].material.base_color[0] == doctest::Approx(0.4f));
    CHECK(v.submeshes[1].material.base_color[1] == doctest::Approx(0.2f));
    CHECK(v.submeshes[1].index_count == 72);
}

TEST_CASE("static merge: a material override, double-sided and lobes split the key") {
    const Piece a = make_piece({0.5f, 0.5f, 0.5f}, 0.8f, 0.8f, 0.8f);
    const Piece two_sided = make_piece({0.5f, 0.5f, 0.5f}, 0.8f, 0.8f, 0.8f, {}, asset::MaterialFlags::DoubleSided);
    MergeSource s[4] = {src(a, {0, 0, 0}), src(two_sided, {2, 0, 0}), src(a, {4, 0, 0}), src(a, {6, 0, 0})};
    asset::MaterialChunk over = a.view.submeshes[0].material;
    over.base_color[2] = 0.1f;
    asset::MaterialExt lobes{};
    lobes.clearcoat = 1.0f;
    s[2].material = &over;
    s[3].material = &over;
    s[3].material_ext = &lobes;
    Array<byte> blob;
    REQUIRE(build_merged_mesh(Span<const MergeSource>(s, 4), blob));
    const asset::MeshView v = asset::MeshView::read(blob.span()).value();
    CHECK(v.submeshes.size() == 4);
    CHECK((v.submeshes[1].material.flags & asset::MaterialFlags::DoubleSided) != 0);
    CHECK(v.material_ext(3).clearcoat == doctest::Approx(1.0f));
    CHECK(v.material_ext(2).clearcoat == doctest::Approx(0.0f));
}

TEST_CASE("static merge: LOD level k is the union of each source's level min(k, levels - 1)") {
    const Piece lod = make_piece({0.5f, 0.5f, 0.5f}, 0.8f, 0.2f, 0.2f, {}, 0, 6, 0.1f);
    const Piece plain = make_piece({0.5f, 0.5f, 0.5f}, 0.8f, 0.2f, 0.2f);
    const MergeSource s[] = {src(lod, {0, 0, 0}, 0.0f, 2.0f), src(plain, {3, 0, 0}), src(lod, {6, 0, 0})};
    Array<byte> blob;
    MergeStats st;
    REQUIRE(build_merged_mesh(Span<const MergeSource>(s, 3), blob, &st));
    const asset::MeshView v = asset::MeshView::read(blob.span()).value();  // validates every level's indices
    REQUIRE(v.submeshes.size() == 1);
    CHECK(st.lod_levels == 2);
    CHECK(v.submeshes[0].index_count == 3 * 36);
    const Span<const asset::MeshLod> levels = v.submesh_lods(0);
    REQUIRE(levels.size() == 1);
    CHECK(levels[0].index_count == 18 + 36 + 18);                // two coarse boxes and the plain one whole
    CHECK(levels[0].error == doctest::Approx(0.2f));             // the larger of 0.1 x 2 (scaled copy) and 0.1
    CHECK(levels[0].vertex_count == v.submeshes[0].vertex_count);
    CHECK(v.header->vertex_count == 3 * 24);  // a generated level shares the submesh's vertices: nothing is copied twice

    // A store takes it: the mesh's level 1 error is the merged one.
    MeshStore store;
    REQUIRE(store.upload(1, 1, blob.span()).has_value());
    CHECK(store.get(1)->lod_levels == 2);
    CHECK(store.get(1)->lod_error[1] == doctest::Approx(0.2f));
}

TEST_CASE("static merge: a skinned source is left out, an empty merge fails") {
    const Piece skinned = skinned_piece();
    const Piece box = make_piece({0.5f, 0.5f, 0.5f}, 0.5f, 0.5f, 0.5f);
    Array<byte> blob;
    MergeStats st;
    MergeSource s[3] = {src(skinned, {0, 0, 0}), src(box, {2, 0, 0}), MergeSource{}};
    REQUIRE(build_merged_mesh(Span<const MergeSource>(s, 3), blob, &st));
    CHECK(st.sources == 1);
    CHECK(st.skipped == 2);
    CHECK(asset::MeshView::read(blob.span()).value().submeshes.size() == 1);
    CHECK_FALSE(build_merged_mesh(Span<const MergeSource>(s, 1), blob, &st));
    CHECK(blob.empty());
    CHECK_FALSE(build_merged_mesh(Span<const MergeSource>(), blob));
}

TEST_CASE("static merge: past 65536 vertices in a submesh the indices widen, and 100k vertices merge in a blink") {
    const Piece a = make_piece({0.2f, 0.2f, 0.2f}, 0.6f, 0.6f, 0.6f);
    const Piece b = make_piece({0.3f, 0.1f, 0.2f}, 0.6f, 0.6f, 0.6f);
    Array<MergeSource> s;
    for (u32 i = 0, row = 0; i < 4300; ++i, row = i / 65) s.push_back(src(i % 2 ? a : b, {f32(i % 65) * 0.8f, 0.0f, f32(row) * 0.8f}, 0.01f * f32(i % 7)));
    Array<byte> blob;
    MergeStats st;
    const auto t0 = std::chrono::steady_clock::now();
    REQUIRE(build_merged_mesh(s.span(), blob, &st));
    const f64 ms = std::chrono::duration<f64, std::milli>(std::chrono::steady_clock::now() - t0).count();
    CHECK(st.vertices == 4300 * 24);  // 103,200
    MESSAGE("merged ", st.vertices, " vertices in ", ms, " ms");
    // Printed, not asserted: ~10 ms in a release build, several times that unoptimized and loaded.
    const Result<asset::MeshView> r = asset::MeshView::read(blob.span());
    REQUIRE(r.has_value());
    CHECK(r.value().index32());
    CHECK(r.value().submeshes.size() == 1);
    MeshStore store;
    REQUIRE(store.upload(1, 1, blob.span()).has_value());
    CHECK(store.get(1)->index_format == rhi::IndexFormat::U32);
}

TEST_CASE("static merge: the merged cells draw once per material, not once per piece") {
    const Piece wall = make_piece({1.0f, 1.0f, 0.1f}, 0.8f, 0.2f, 0.2f);
    const Piece pillar = make_piece({0.2f, 1.5f, 0.2f}, 0.2f, 0.8f, 0.2f);
    const Piece beam = make_piece({1.0f, 0.1f, 0.1f}, 0.2f, 0.2f, 0.8f);
    constexpr u32 kCells = 3, kPerCell = 8;
    MeshStore store;
    Array<MeshInstance> loose;
    for (u32 c = 0; c < kCells; ++c) {
        Array<MergeSource> s;
        for (u32 i = 0; i < kPerCell; ++i) {
            const Piece& p = i % 3 == 0 ? wall : i % 3 == 1 ? pillar : beam;
            const u32 row = i / 4;
            const vec3 at{f32(c) * 8.0f - 8.0f + f32(i % 4) * 1.5f, 0.0f, -30.0f - f32(row) * 1.5f};
            s.push_back(src(p, at));
            const u32 key = 100 + (i % 3);
            if (!store.get(key)) REQUIRE(store.upload(key, 1, p.blob.span()).has_value());
            MeshInstance m;
            m.mesh = key;
            m.world = s.back().world;
            loose.push_back(m);
        }
        Array<byte> blob;
        REQUIRE(build_merged_mesh(s.span(), blob));
        REQUIRE(store.upload(1 + c, 1, blob.span()).has_value());
    }
    Scene3D merged(store, mem::heap());
    merged.begin(camera(), {256, 256});
    for (u32 c = 0; c < kCells; ++c) {
        MeshInstance m;
        m.mesh = 1 + c;
        merged.add(m);
    }
    merged.build();
    Scene3D unmerged(store, mem::heap());
    unmerged.begin(camera(), {256, 256});
    for (const MeshInstance& m : loose) unmerged.add(m);
    unmerged.build();
    CHECK(merged.draws().size() == 3 * kCells);                  // materials x cells
    CHECK(unmerged.draws().size() == kCells * kPerCell);         // one submesh per piece
    // The same triangles are drawn either way (every piece is in view).
    CHECK(merged.stats().triangles == unmerged.stats().triangles);
}


namespace {

Piece from_builder(const mesh_fixture::Builder& b) {
    Piece p;
    p.blob = b.build();
    p.view = asset::MeshView::read(p.blob.span()).value();
    return p;
}

// One triangle in the XY plane whose stored normal is (1, 1, 0) / sqrt 2.
Piece slanted_piece() {
    mesh_fixture::Builder b;
    b.add_node(-1);
    const f32 xs[3] = {-1, 1, 0}, ys[3] = {-1, -1, 1};
    for (int i = 0; i < 3; ++i) {
        asset::MeshVertex q{};
        q.position[0] = mesh_fixture::Builder::snorm(xs[i]);
        q.position[1] = mesh_fixture::Builder::snorm(ys[i]);
        q.normal[0] = mesh_fixture::Builder::snorm(0.5f);
        q.normal[1] = mesh_fixture::Builder::snorm(0.5f);
        b.vertices.push_back(q);
    }
    asset::MeshSubmesh sm{};
    sm.extent[0] = sm.extent[1] = 1.0f;
    sm.extent[2] = 0.01f;
    sm.uv_scale[0] = sm.uv_scale[1] = 1.0f;
    for (int i = 0; i < 4; ++i) sm.material.base_color[i] = 1.0f;
    sm.vertex_count = 3;
    sm.index_count = 3;
    b.submeshes.push_back(sm);
    for (const u16 i : {u16(0), u16(1), u16(2)}) b.indices.push_back(i);
    return from_builder(b);
}

vec3 decode_normal(const asset::MeshVertex& q) {
    const f32 x = q.normal[0] / 32767.0f, y = q.normal[1] / 32767.0f;
    vec3 n{x, y, 1.0f - std::fabs(x) - std::fabs(y)};
    if (n.z < 0.0f) n = {(1.0f - std::fabs(n.y)) * (n.x >= 0 ? 1.0f : -1.0f), (1.0f - std::fabs(n.x)) * (n.y >= 0 ? 1.0f : -1.0f), n.z};
    return math::normalize(n);
}

// `n` vertices in one submesh, a few triangles over the first of them, one coarser level.
Piece big_piece(u32 n) {
    mesh_fixture::Builder b;
    b.add_node(-1);
    for (u32 i = 0; i < n; ++i) {
        asset::MeshVertex q{};
        q.position[0] = mesh_fixture::Builder::snorm(f32(i % 200) / 100.0f - 1.0f);
        q.position[1] = mesh_fixture::Builder::snorm(f32(i / 200 % 200) / 100.0f - 1.0f);
        b.vertices.push_back(q);
    }
    asset::MeshSubmesh sm{};
    sm.extent[0] = sm.extent[1] = sm.extent[2] = 1.0f;
    sm.uv_scale[0] = sm.uv_scale[1] = 1.0f;
    for (int i = 0; i < 4; ++i) sm.material.base_color[i] = 1.0f;
    sm.vertex_count = n;
    sm.index_count = 900;
    for (u16 k = 0; k < 300; ++k)
        for (u16 i : {k, u16(k + 1), u16(k + 2)}) b.indices.push_back(i);
    b.submeshes.push_back(sm);
    b.add_lod(0, 50, 0.1f);
    return from_builder(b);
}

}  // namespace

TEST_CASE("static merge: 0xFFFF vertices stay 16-bit, 0x10000 and more widen every group and level") {
    const Piece box = make_piece({0.5f, 0.5f, 0.5f}, 0.2f, 0.4f, 0.8f);
    for (const u32 n : {65535u, 65536u, 65537u}) {
        CAPTURE(n);
        const Piece big = big_piece(n);
        const MergeSource s[] = {src(big, {0, 0, 0}), src(box, {5, 0, 0})};
        Array<byte> blob;
        REQUIRE(build_merged_mesh(Span<const MergeSource>(s, 2), blob));
        const Result<asset::MeshView> r = asset::MeshView::read(blob.span());  // checks every index of every level
        REQUIRE(r.has_value());
        const asset::MeshView& v = r.value();
        REQUIRE(v.submeshes.size() == 2);
        CHECK(v.submeshes[0].vertex_count == n);
        CHECK(v.submeshes[1].vertex_count == 24);
        CHECK(v.submesh_lods(0).size() == 1);
        CHECK(v.index32() == (n > 0xFFFF));
        MeshStore store;
        REQUIRE(store.upload(1, 1, blob.span()).has_value());
        CHECK(store.get(1)->index_format == (n > 0xFFFF ? rhi::IndexFormat::U32 : rhi::IndexFormat::U16));
    }
}

TEST_CASE("static merge: blended, transmissive and parallax sources keep their own draw") {
    const Piece plain = make_piece({0.5f, 0.5f, 0.5f}, 0.5f, 0.5f, 0.5f);
    const Piece glass = make_piece({0.5f, 0.5f, 0.5f}, 0.5f, 0.5f, 0.5f, {}, asset::MaterialFlags::AlphaBlend);
    mesh_fixture::Builder tb;
    tb.add_node(-1);
    mesh_fixture::Box box;
    box.half = {0.5f, 0.5f, 0.5f};
    tb.add_box(box);
    asset::MaterialExt lobes{};
    lobes.transmission = 0.8f;
    tb.materials.push_back(lobes);
    const Piece water = from_builder(tb);
    mesh_fixture::Builder pb;
    pb.add_node(-1);
    pb.add_box(box);
    asset::MaterialExt pom{};
    pom.set_height(asset::Guid{1, 2});
    pom.height_scale = 0.05f;
    pb.materials.push_back(pom);
    const Piece bricks = from_builder(pb);

    MergeSource s[6] = {src(plain, {0, 0, 0}), src(glass, {2, 0, 0}), src(water, {4, 0, 0}),
                        src(bricks, {6, 0, 0}), src(plain, {8, 0, 0}), src(plain, {10, 0, 0})};
    asset::MaterialChunk blend_over = plain.view.submeshes[0].material;
    blend_over.flags |= asset::MaterialFlags::AlphaBlend;
    s[5].material = &blend_over;  // an override can make an opaque mesh blended
    Array<byte> blob;
    MergeStats st;
    REQUIRE(build_merged_mesh(Span<const MergeSource>(s, 6), blob, &st));
    CHECK(st.sources == 2);
    CHECK(st.skipped == 4);
    CHECK(st.skipped_blended == 3);
    CHECK(st.skipped_parallax == 1);
    const asset::MeshView v = asset::MeshView::read(blob.span()).value();
    CHECK(v.submeshes.size() == 1);
    CHECK(v.submeshes[0].index_count == 72);

    // A parallax override through the extension counts too, and nothing left means no mesh.
    asset::MaterialExt over_ext = pom;
    MergeSource p[1] = {src(plain, {0, 0, 0})};
    p[0].material = &plain.view.submeshes[0].material;
    p[0].material_ext = &over_ext;
    CHECK_FALSE(build_merged_mesh(Span<const MergeSource>(p, 1), blob, &st));
    CHECK(st.skipped_parallax == 1);
}

TEST_CASE("static merge: materials share one quantization box, so a seam between them stays closed") {
    const Piece red = make_piece({0.7f, 0.31f, 0.4f}, 0.8f, 0.2f, 0.2f);
    const Piece green = make_piece({0.9f, 0.5f, 0.45f}, 0.2f, 0.8f, 0.2f);
    // Red's +X face and green's -X face meet in the plane x = 0.3, with a different bounds for each material alone.
    const MergeSource s[] = {src(red, {-0.4f, 0, 0}), src(green, {1.2f, 0, 0})};
    Array<byte> blob;
    REQUIRE(build_merged_mesh(Span<const MergeSource>(s, 2), blob));
    const asset::MeshView v = asset::MeshView::read(blob.span()).value();
    REQUIRE(v.submeshes.size() == 2);
    for (int k = 0; k < 3; ++k) {
        CHECK(v.submeshes[0].center[k] == v.submeshes[1].center[k]);
        CHECK(v.submeshes[0].extent[k] == v.submeshes[1].extent[k]);
    }
    CHECK(sub_bounds(v, 0).max.x == sub_bounds(v, 1).min.x);  // bit for bit the same plane
    CHECK(sub_bounds(v, 0).max.x == doctest::Approx(0.3f).epsilon(0.0005));
    // The quantum is extent / 65534: the error of any vertex is within half of it.
    CHECK(sub_bounds(v, 1).max.x == doctest::Approx(2.1f).epsilon(0.0005));
}

TEST_CASE("static merge: a mirrored or non-uniformly scaled source keeps its winding and its normals follow the inverse transpose") {
    const Piece tri = slanted_piece();
    MergeSource s[2];
    s[0].mesh = &tri.view;
    s[0].world = math::scaling({1.0f, 3.0f, 1.0f});
    s[1].mesh = &tri.view;
    s[1].world = math::translation({4, 0, 0}) * math::scaling({-1.0f, 3.0f, 1.0f});  // mirrored: negative determinant
    Array<byte> blob;
    REQUIRE(build_merged_mesh(Span<const MergeSource>(s, 2), blob));
    const asset::MeshView v = asset::MeshView::read(blob.span()).value();
    REQUIRE(v.submeshes.size() == 1);
    REQUIRE(v.header->vertex_count == 6);
    // diag(1, 3, 1): the normal (1, 1, 0) becomes (3, 1, 0) / sqrt 10; mirrored in x it is (-3, 1, 0) / sqrt 10.
    const vec3 a = decode_normal(v.vertices[0]), b = decode_normal(v.vertices[3]);
    CHECK(a.x == doctest::Approx(0.9487f).epsilon(0.002));
    CHECK(a.y == doctest::Approx(0.3162f).epsilon(0.005));
    CHECK(b.x == doctest::Approx(-0.9487f).epsilon(0.002));
    CHECK(b.y == doctest::Approx(0.3162f).epsilon(0.005));
    // Winding as authored, for the mirrored copy too (the engine does not flip it for unmerged draws either).
    for (u32 i = 0; i < 3; ++i) {
        CHECK(v.index(i) == i);
        CHECK(v.index(3 + i) == 3 + i);
    }
    // The mirrored triangle's apex (0, 1) went to x = 4, y = 3 and its base to x = 5 and 3.
    const math::aabb3 box = sub_bounds(v, 0);
    CHECK(box.min.x == doctest::Approx(-1.0f).epsilon(0.0005));
    CHECK(box.max.x == doctest::Approx(5.0f).epsilon(0.0005));
    CHECK(box.max.y == doctest::Approx(3.0f).epsilon(0.0005));
}

TEST_CASE("static merge: an authored level with a vertex window of its own is copied once and merged") {
    mesh_fixture::Builder b;
    b.add_node(-1);
    mesh_fixture::Box box;
    box.half = {0.5f, 0.5f, 0.5f};
    b.add_box(box);
    b.add_lod(0, 6, 0.2f, true);
    const Piece authored = from_builder(b);
    const Piece plain = make_piece({0.5f, 0.5f, 0.5f}, 1.0f, 1.0f, 1.0f);
    const MergeSource s[] = {src(authored, {0, 0, 0}), src(plain, {3, 0, 0}), src(authored, {6, 0, 0})};
    Array<byte> blob;
    REQUIRE(build_merged_mesh(Span<const MergeSource>(s, 3), blob));
    const Result<asset::MeshView> r = asset::MeshView::read(blob.span());  // every level's indices stay inside its window
    REQUIRE(r.has_value());
    const asset::MeshView& v = r.value();
    REQUIRE(v.submeshes.size() == 1);
    // The base and the level are two windows of 24 per authored source, the plain one has one.
    CHECK(v.header->vertex_count == 24 * 2 * 2 + 24);
    REQUIRE(v.submesh_lods(0).size() == 1);
    CHECK(v.submesh_lods(0)[0].index_count == 18 + 36 + 18);
    CHECK(v.submesh_lods(0)[0].error == doctest::Approx(0.2f));
    // The coarse level's vertices sit where the base's do: the same bounds.
    const math::aabb3 bb = sub_bounds(v, 0);
    CHECK(bb.min.x == doctest::Approx(-0.5f).epsilon(0.0005));
    CHECK(bb.max.x == doctest::Approx(6.5f).epsilon(0.0005));
}
