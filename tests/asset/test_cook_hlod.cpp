// The .mchlod importer (M-OPENWORLD A3): an HLOD proxy bake cooks into an ordinary CHNK_MESH with the payload as
// baked; a damaged hash, another version, a bad magic or a payload MeshView::read refuses stop at the cook with a
// hint, and the submeshes' textures are checked as links.

#include <doctest/doctest.h>

#include <cstring>
#include <string>

#include "cook_test_util.h"
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

cook::CookReport cook_bytes(const std::string& name, const std::string& bytes) {
    const std::string root = fresh_dir(name);
    write_file(root + "/assets/hlod/block.mchlod", bytes);
    write_file(root + "/assets/hlod/block.mchlod.mceasset",
               R"({ "guid": "6a1d0c5e-3b2f-4e7a-9c1d-2f3e4a5b6c7d", "importer": "hlod", "importer_version": 1, "settings": {} })");
    cook::CookService svc;
    return svc.cook(StringView(root.c_str()), cook::CookOptions{});
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
