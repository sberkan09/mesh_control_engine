// The .mchlod importer (M-OPENWORLD A3): an HLOD proxy bake cooks into an ordinary CHNK_MESH with the payload as
// baked; a damaged hash, another version, a bad magic or a payload MeshView::read refuses stop at the cook with a
// hint, and the submeshes' textures are checked as links.

#include <doctest/doctest.h>

#include <cstring>
#include <string>

#include "cook_test_util.h"
#include "mce/asset/asset_system.h"
#include "mce/asset/formats.h"
#include "mce/cook/cook.h"
#include "mce/cook/hlod.h"

using namespace mce;
using namespace cook_test;

namespace {

std::string diags_of(const cook::CookReport& r) {
    std::string s;
    for (const auto& d : r.diags.all()) {
        cook::String line;
        cook::format_diagnostic(d, line);
        s += line.c_str();
        s += "\n";
    }
    return s;
}

// One triangle, one submesh, one root node: the smallest mesh MeshView::read accepts.
Array<byte> triangle_payload() {
    asset::MeshVertex v[3] = {};
    v[1].position[0] = 32767;
    v[2].position[2] = 32767;
    const u16 idx[3] = {0, 1, 2};
    asset::MeshSubmesh sm{};
    sm.index_count = 3;
    sm.vertex_count = 3;
    for (f32& e : sm.extent) e = 1.0f;
    sm.uv_scale[0] = sm.uv_scale[1] = 1.0f;
    sm.material.base_color[0] = sm.material.base_color[1] = sm.material.base_color[2] = sm.material.base_color[3] = 1.0f;
    sm.material.roughness = 1.0f;
    asset::MeshNode node{};
    node.parent = -1;
    node.scale[0] = node.scale[1] = node.scale[2] = 1.0f;
    node.rotation[3] = 1.0f;
    asset::MeshData d;
    d.vertices = Span<const asset::MeshVertex>(v, 3);
    d.indices = Span<const byte>(reinterpret_cast<const byte*>(idx), sizeof(idx));
    d.submeshes = Span<const asset::MeshSubmesh>(&sm, 1);
    d.nodes = Span<const asset::MeshNode>(&node, 1);
    d.bounds_max[0] = d.bounds_max[1] = d.bounds_max[2] = 1.0f;
    Array<byte> out;
    asset::write_mesh(d, out);
    return out;
}

std::string as_string(const Array<byte>& b) { return std::string(reinterpret_cast<const char*>(b.data()), b.size()); }

constexpr const char* kBlockGuid = "6a1d0c5e-3b2f-4e7a-9c1d-2f3e4a5b6c7d";

cook::CookReport cook_bytes(const std::string& name, const std::string& bytes, const std::string& settings = "{}",
                            std::string* root_out = nullptr) {
    const std::string root = fresh_dir(name);
    write_file(root + "/assets/hlod/block.mchlod", bytes);
    write_file(root + "/assets/hlod/block.mchlod.mceasset", std::string(R"({ "guid": ")") + kBlockGuid +
                                                                R"(", "importer": "hlod", "importer_version": 1, "settings": )" +
                                                                settings + " }");
    if (root_out) *root_out = root;
    cook::CookService svc;
    return svc.cook(StringView(root.c_str()), cook::CookOptions{});
}

// A 24 x 24 quad grid with a gentle bump: 1152 triangles a far level can thin out.
Array<byte> grid_payload() {
    constexpr u32 n = 24;
    Array<asset::MeshVertex> v;
    for (u32 z = 0; z <= n; ++z)
        for (u32 x = 0; x <= n; ++x) {
            asset::MeshVertex q{};
            const f32 fx = f32(x) / f32(n) * 2.0f - 1.0f, fz = f32(z) / f32(n) * 2.0f - 1.0f;
            q.position[0] = static_cast<i16>(fx * 32767.0f);
            q.position[1] = static_cast<i16>((1.0f - fx * fx) * (1.0f - fz * fz) * 3276.0f);
            q.position[2] = static_cast<i16>(fz * 32767.0f);
            q.normal[1] = 32767;
            v.push_back(q);
        }
    Array<u16> idx;
    for (u32 z = 0; z < n; ++z)
        for (u32 x = 0; x < n; ++x) {
            const u16 a = static_cast<u16>(z * (n + 1) + x), b = static_cast<u16>(a + 1), c = static_cast<u16>(a + n + 1),
                      d = static_cast<u16>(c + 1);
            for (u16 i : {a, c, b, b, c, d}) idx.push_back(i);
        }
    asset::MeshSubmesh sm{};
    sm.index_count = static_cast<u32>(idx.size());
    sm.vertex_count = static_cast<u32>(v.size());
    for (f32& e : sm.extent) e = 10.0f;
    sm.uv_scale[0] = sm.uv_scale[1] = 1.0f;
    sm.material.base_color[0] = sm.material.base_color[1] = sm.material.base_color[2] = sm.material.base_color[3] = 1.0f;
    sm.material.roughness = 1.0f;
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
        d.bounds_min[k] = -10.0f;
        d.bounds_max[k] = 10.0f;
    }
    Array<byte> out;
    asset::write_mesh(d, out);
    return out;
}

Array<byte> cooked_mesh(const std::string& root) {
    asset::AssetSystem as;
    const std::string cooked = root + "/cooked";
    const StringView r[] = {StringView(cooked.c_str())};
    REQUIRE(as.mount(Span<const StringView>(r, 1)).has_value());
    const auto g = asset::parse_guid(kBlockGuid);
    REQUIRE(g.has_value());
    auto h = as.load_sync<asset::Mesh>(g.value());
    REQUIRE(h.has_value());
    Array<byte> out;
    for (const byte b : as.resolve(h.value())) out.push_back(b);
    as.release(asset::AnyHandle(h.value()));
    return out;
}

}  // namespace

TEST_CASE("hlod importer: a baked proxy cooks as a mesh; damage, versions and bad meshes are refused with a hint") {
    const Array<byte> payload = triangle_payload();
    REQUIRE(asset::MeshView::read(payload.span()).has_value());
    Array<byte> file;
    cook::write_hlod_file(payload.span(), file);
    const std::string good = as_string(file);
    REQUIRE(good.size() == payload.size() + 16);
    CHECK(std::memcmp(good.data(), "MCHL", 4) == 0);
    {
        const cook::CookReport r = cook_bytes("hlod_good", good);
        INFO(diags_of(r));
        CHECK(r.ok());
        CHECK(r.cooked == 1);
    }
    {  // one flipped byte inside the mesh: the hash no longer matches
        std::string bad = good;
        bad[bad.size() / 2] = static_cast<char>(bad[bad.size() / 2] ^ 0x5A);
        const cook::CookReport r = cook_bytes("hlod_bad_hash", bad);
        CHECK_FALSE(r.ok());
        CHECK(diags_of(r).find("re-bake") != std::string::npos);
    }
    {  // another version
        std::string other = good;
        other[4] = 2;
        const cook::CookReport r = cook_bytes("hlod_v2", other);
        CHECK_FALSE(r.ok());
        CHECK(diags_of(r).find("version") != std::string::npos);
    }
    {  // not a bake at all
        const cook::CookReport r = cook_bytes("hlod_magic", std::string(64, 'x'));
        CHECK_FALSE(r.ok());
        CHECK(diags_of(r).find("bad magic") != std::string::npos);
    }
    {  // a valid wrapper around bytes that are not a mesh
        Array<byte> junk;
        junk.resize(40, byte{7});
        Array<byte> wrapped;
        cook::write_hlod_file(junk.span(), wrapped);
        const cook::CookReport r = cook_bytes("hlod_not_mesh", as_string(wrapped));
        CHECK_FALSE(r.ok());
        CHECK(diags_of(r).find("does not validate") != std::string::npos);
    }
}

TEST_CASE("hlod importer: lod_levels adds simplified far levels to a proxy whose sources had none") {
    const Array<byte> payload = grid_payload();
    REQUIRE(asset::MeshView::read(payload.span()).has_value());
    Array<byte> file;
    cook::write_hlod_file(payload.span(), file);
    std::string root;
    {  // default: the payload as baked
        const cook::CookReport r = cook_bytes("hlod_lods_off", as_string(file), "{}", &root);
        INFO(diags_of(r));
        REQUIRE(r.ok());
        const Array<byte> m = cooked_mesh(root);
        CHECK(m.size() == payload.size());
        CHECK(std::memcmp(m.data(), payload.data(), payload.size()) == 0);
    }
    {
        const cook::CookReport r = cook_bytes("hlod_lods_on", as_string(file), R"({ "lod_levels": 2 })", &root);
        INFO(diags_of(r));
        REQUIRE(r.ok());
        const Array<byte> m = cooked_mesh(root);
        const Result<asset::MeshView> v = asset::MeshView::read(m.span());
        REQUIRE(v.has_value());
        REQUIRE(v.value().submeshes.size() == 1);
        CHECK(v.value().submeshes[0].index_count == 24u * 24u * 6u);  // level 0 kept
        const Span<const asset::MeshLod> lods = v.value().submesh_lods(0);
        REQUIRE(lods.size() >= 1);
        CHECK(lods[0].index_count < v.value().submeshes[0].index_count / 2);
        CHECK(lods[0].error > 0.0f);
        // Deterministic: the same file cooks to the same bytes.
        std::string again;
        REQUIRE(cook_bytes("hlod_lods_again", as_string(file), R"({ "lod_levels": 2 })", &again).ok());
        const Array<byte> m2 = cooked_mesh(again);
        CHECK(m2.size() == m.size());
        CHECK(std::memcmp(m2.data(), m.data(), m.size()) == 0);
    }
    {
        const cook::CookReport r = cook_bytes("hlod_lods_bad", as_string(file), R"({ "lod_levels": 7 })");
        CHECK_FALSE(r.ok());
        CHECK(diags_of(r).find("lod_levels") != std::string::npos);
    }
}
