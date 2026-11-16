// The impostor bake (M-OPENWORLD A3): groups of cooked meshes captured on the CPU into one atlas and placed on box
// sides or cross cards. Four groups: a red box, a tall thin pole (cards), a box textured green through the callback,
// a box whose texture is missing (white, counted); a blended source left out. Same input, same bytes.

#include <doctest/doctest.h>

#include <cstring>

#include "mce/asset/formats.h"
#include "mce/cook/impostor.h"

using namespace mce;

namespace {

constexpr asset::Guid kGreen{0x1111, 0x2222};
constexpr asset::Guid kMissing{0x3333, 0x4444};
constexpr asset::Guid kAtlas{0x5555, 0x6666};

// A unit cube around the origin, 24 vertices (a face's own normal and UVs), one submesh with `material`.
Array<byte> cube_payload(const asset::MaterialChunk& material) {
    Array<asset::MeshVertex> v;
    Array<u16> idx;
    const i16 F = 32767;
    const i16 axes[6][3][3] = {
        // normal, right, up (snorm)
        {{F, 0, 0}, {0, 0, -F}, {0, F, 0}}, {{-F, 0, 0}, {0, 0, F}, {0, F, 0}}, {{0, 0, F}, {F, 0, 0}, {0, F, 0}},
        {{0, 0, -F}, {-F, 0, 0}, {0, F, 0}}, {{0, F, 0}, {F, 0, 0}, {0, 0, -F}}, {{0, -F, 0}, {F, 0, 0}, {0, 0, F}},
    };
    for (const auto& a : axes) {
        const u16 base = static_cast<u16>(v.size());
        const int corner[4][2] = {{-1, -1}, {1, -1}, {1, 1}, {-1, 1}};
        for (const auto& c : corner) {
            asset::MeshVertex q{};
            for (int k = 0; k < 3; ++k)
                q.position[k] = static_cast<i16>(a[0][k] + c[0] * a[1][k] + c[1] * a[2][k] > 0 ? F : -F);
            q.uv[0] = c[0] > 0 ? 65535 : 0;
            q.uv[1] = c[1] > 0 ? 0 : 65535;
            v.push_back(q);
        }
        for (const int i : {0, 1, 2, 0, 2, 3}) idx.push_back(static_cast<u16>(base + i));
    }
    asset::MeshSubmesh sm{};
    sm.index_count = static_cast<u32>(idx.size());
    sm.vertex_count = static_cast<u32>(v.size());
    for (f32& e : sm.extent) e = 0.5f;
    sm.uv_scale[0] = sm.uv_scale[1] = 1.0f;
    sm.material = material;
    asset::MeshNode node{};
    node.parent = -1;
    node.scale[0] = node.scale[1] = node.scale[2] = 1.0f;
    node.rotation[3] = 1.0f;
    asset::MeshData d;
    d.vertices = v.span();
    d.indices = std::as_bytes(idx.span());
    d.submeshes = Span<const asset::MeshSubmesh>(&sm, 1);
    d.nodes = Span<const asset::MeshNode>(&node, 1);
    for (int k = 0; k < 3; ++k) {
        d.bounds_min[k] = -0.5f;
        d.bounds_max[k] = 0.5f;
    }
    Array<byte> out;
    asset::write_mesh(d, out);
    return out;
}

asset::MaterialChunk material(f32 r, f32 g, f32 b, asset::Guid texture = {}, u32 flags = 0) {
    asset::MaterialChunk m{};
    m.base_color[0] = r;
    m.base_color[1] = g;
    m.base_color[2] = b;
    m.base_color[3] = 1.0f;
    m.roughness = 1.0f;
    m.alpha_cutoff = 0.5f;
    m.flags = flags;
    m.textures[0][0] = texture.hi;
    m.textures[0][1] = texture.lo;
    return m;
}

Array<byte> green_texture() {
    u8 px[4 * 4 * 4];
    for (usize i = 0; i < 16; ++i) {
        px[i * 4 + 0] = 0;
        px[i * 4 + 1] = 255;
        px[i * 4 + 2] = 0;
        px[i * 4 + 3] = 255;
    }
    const asset::TextureMipData mip{4, 4, Span<const byte>(reinterpret_cast<const byte*>(px), sizeof(px))};
    Array<byte> out;
    asset::write_texture(asset::TextureFormat::RGBA8, 1, asset::TextureFlags::Srgb, Span<const asset::TextureMipData>(&mip, 1), out);
    return out;
}

struct Lookup {
    Array<byte> green;
    u32 calls = 0;
};
Span<const byte> lookup(asset::Guid g, void* user) {
    Lookup& l = *static_cast<Lookup*>(user);
    ++l.calls;
    return g == kGreen ? l.green.span() : Span<const byte>{};
}

u32 count_colour(const cook::ImpostorBake& b, u8 r, u8 g, u8 bl) {
    u32 n = 0;
    for (usize i = 0; i < b.atlas.size(); i += 4)
        if (b.atlas[i + 3] == 255 && b.atlas[i] == r && b.atlas[i + 1] == g && b.atlas[i + 2] == bl) ++n;
    return n;
}

}  // namespace

TEST_CASE("impostor bake: boxes and cross cards over one atlas, albedo captured, deterministic") {
    const Array<byte> red = cube_payload(material(1, 0, 0));
    const Array<byte> textured = cube_payload(material(1, 1, 1, kGreen));
    const Array<byte> missing = cube_payload(material(1, 1, 1, kMissing));
    const Array<byte> glass = cube_payload(material(0, 0, 1, {}, asset::MaterialFlags::AlphaBlend));
    const asset::MeshView red_v = asset::MeshView::read(red.span()).value();
    const asset::MeshView tex_v = asset::MeshView::read(textured.span()).value();
    const asset::MeshView miss_v = asset::MeshView::read(missing.span()).value();
    const asset::MeshView glass_v = asset::MeshView::read(glass.span()).value();

    cook::ImpostorSource src[5];
    src[0] = {&red_v, math::translation({0, 5, 0}) * math::scaling({10, 10, 10}), {1, 1, 1, 1}, nullptr, 0};
    src[1] = {&red_v, math::translation({20, 3, 0}) * math::scaling({1, 6, 1}), {1, 1, 1, 1}, nullptr, 1};
    src[2] = {&tex_v, math::translation({-20, 4, 0}) * math::scaling({8, 8, 8}), {1, 1, 1, 1}, nullptr, 2};
    src[3] = {&miss_v, math::translation({0, 4, 20}) * math::scaling({8, 8, 8}), {1, 1, 1, 1}, nullptr, 3};
    src[4] = {&glass_v, math::translation({0, 4, -20}) * math::scaling({8, 8, 8}), {1, 1, 1, 1}, nullptr, 4};

    Lookup l;
    l.green = green_texture();
    cook::ImpostorBake out;
    REQUIRE(cook::bake_impostor(Span<const cook::ImpostorSource>(src, 5), lookup, &l, kAtlas, {}, out));
    const cook::ImpostorStats& st = out.stats;
    CHECK(st.groups == 4);
    CHECK(st.boxes == 3);
    CHECK(st.cards == 1);
    CHECK(st.quads == 3 * 5 + 2);
    CHECK(st.skipped == 1);
    CHECK(st.untextured == 1);
    CHECK(l.calls == 2);  // one lookup per GUID
    CHECK(st.bounds.min.x == doctest::Approx(-24.0f));
    CHECK(st.bounds.max.x == doctest::Approx(20.5f));
    CHECK(st.bounds.max.y == doctest::Approx(10.0f));
    CHECK(out.atlas_width >= 1024);
    CHECK(out.atlas.size() == static_cast<usize>(out.atlas_width) * out.atlas_height * 4);

    // The captures: the red box and pole, the green texture (sRGB 255 stays 255), the missing texture as white.
    CHECK(count_colour(out, 255, 0, 0) > 1000);
    CHECK(count_colour(out, 0, 255, 0) > 1000);
    CHECK(count_colour(out, 255, 255, 255) > 1000);
    CHECK(count_colour(out, 0, 0, 255) == 0);  // the blended source is not captured

    // The mesh: boxes single-sided, cards double-sided, both masked over the atlas.
    const Result<asset::MeshView> mv = asset::MeshView::read(out.mesh.span());
    REQUIRE(mv);
    const asset::MeshView& m = mv.value();
    REQUIRE(m.submeshes.size() == 2);
    CHECK(m.submeshes[0].index_count == 15 * 6);
    CHECK(m.submeshes[1].index_count == 2 * 6);
    CHECK(m.header->vertex_count == 17 * 4);
    for (const asset::MeshSubmesh& sm : m.submeshes) {
        CHECK(sm.material.texture(asset::MaterialSlot::BaseColor) == kAtlas);
        CHECK((sm.material.flags & asset::MaterialFlags::AlphaMask) != 0);
    }
    CHECK((m.submeshes[0].material.flags & asset::MaterialFlags::DoubleSided) == 0);
    CHECK((m.submeshes[1].material.flags & asset::MaterialFlags::DoubleSided) != 0);
    CHECK(m.header->bounds_max[1] == doctest::Approx(10.0f));

    // Every quad's UV window is covered (the box faces are filled edge to edge by their cube).
    u32 transparent = 0;
    for (u32 i = 0; i < m.header->index_count; ++i) {
        const u32 sub = i < m.submeshes[0].index_count ? 0 : 1;
        const asset::MeshSubmesh& sm = m.submeshes[sub];
        const asset::MeshVertex& q = m.vertices[sm.base_vertex + m.index(i)];
        const f32 u = sm.uv_offset[0] + q.uv[0] / 65535.0f * sm.uv_scale[0], v = sm.uv_offset[1] + q.uv[1] / 65535.0f * sm.uv_scale[1];
        // A texel just inside the corner.
        const u32 x = math::min(static_cast<u32>(u * static_cast<f32>(out.atlas_width) + (u < 0.5f ? 0.5f : -0.5f)), out.atlas_width - 1);
        const u32 y = math::min(static_cast<u32>(v * static_cast<f32>(out.atlas_height) + (v < 0.5f ? 0.5f : -0.5f)), out.atlas_height - 1);
        if (out.atlas[(static_cast<usize>(y) * out.atlas_width + x) * 4 + 3] != 255) ++transparent;
    }
    CHECK(transparent == 0);

    // Deterministic.
    cook::ImpostorBake again;
    REQUIRE(cook::bake_impostor(Span<const cook::ImpostorSource>(src, 5), lookup, &l, kAtlas, {}, again));
    CHECK(again.mesh.size() == out.mesh.size());
    CHECK(std::memcmp(again.mesh.data(), out.mesh.data(), out.mesh.size()) == 0);
    CHECK(std::memcmp(again.atlas.data(), out.atlas.data(), out.atlas.size()) == 0);

    // Nothing to capture: false, everything empty.
    cook::ImpostorBake none;
    CHECK_FALSE(cook::bake_impostor(Span<const cook::ImpostorSource>(&src[4], 1), lookup, &l, kAtlas, {}, none));
    CHECK(none.mesh.empty());
    CHECK(none.atlas.empty());
}
