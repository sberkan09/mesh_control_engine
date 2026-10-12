// Mesh LOD in the cook (#158, M-KIT3D): sidecar lod_levels makes coarser levels with meshopt's
// simplifier (fewer triangles each, growing error, same vertices); `<node>_LOD<n>` nodes become
// authored levels of `<node>` (their own vertices, the base's quantization, not drawn themselves)
// and win over generated ones; MeshView rejects a LOD range outside the payload.

#include <doctest/doctest.h>

#include <algorithm>
#include <cmath>
#include <cstring>

#include "cook_test_util.h"
#include "mce/asset/asset_system.h"
#include "mce/cook/cook.h"

using namespace mce;
using namespace mce::asset;
using namespace cook_test;
using cook::CookOptions;
using cook::CookReport;
using cook::CookService;

namespace {

constexpr const char* kRockGuid = "6a1d9c58-0f4b-4b8e-9d1b-1c2a3b4c5e01";

void put_f32(std::string& s, float v) {
    char b[4];
    std::memcpy(b, &v, 4);
    s.append(b, 4);
}

// A UV sphere: positions, normals, u16 indices appended to `bin`; returns the accessor JSON entries
// (POSITION, NORMAL, indices) and the buffer views they use.
struct Sphere {
    size_t pos_off, nrm_off, idx_off;
    u32 vertices, indices;
};

Sphere add_sphere(std::string& bin, u32 seg, u32 rings, float radius) {
    Sphere s{};
    std::vector<float> p, n;
    for (u32 r = 0; r <= rings; ++r)
        for (u32 c = 0; c <= seg; ++c) {
            const float th = 3.14159265f * float(r) / float(rings), ph = 6.2831853f * float(c) / float(seg);
            const float x = std::sin(th) * std::cos(ph), y = std::cos(th), z = std::sin(th) * std::sin(ph);
            p.insert(p.end(), {x * radius, y * radius, z * radius});
            n.insert(n.end(), {x, y, z});
        }
    std::vector<u16> idx;
    for (u32 r = 0; r < rings; ++r)
        for (u32 c = 0; c < seg; ++c) {
            const u16 a = static_cast<u16>(r * (seg + 1) + c), b = static_cast<u16>(a + seg + 1);
            idx.insert(idx.end(), {a, static_cast<u16>(a + 1), b, b, static_cast<u16>(a + 1), static_cast<u16>(b + 1)});
        }
    s.vertices = static_cast<u32>(p.size() / 3);
    s.indices = static_cast<u32>(idx.size());
    s.pos_off = bin.size();
    for (float v : p) put_f32(bin, v);
    s.nrm_off = bin.size();
    for (float v : n) put_f32(bin, v);
    s.idx_off = bin.size();
    for (u16 i : idx) put16le(bin, i);
    while (bin.size() % 4) bin.push_back(0);
    return s;
}

std::string make_glb(const std::string& json, const std::string& bin) {
    std::string j = json;
    while (j.size() % 4) j.push_back(' ');
    std::string out = "glTF";
    put32le(out, 2);
    put32le(out, static_cast<u32>(12 + 8 + j.size() + 8 + bin.size()));
    put32le(out, static_cast<u32>(j.size()));
    out += "JSON";
    out += j;
    put32le(out, static_cast<u32>(bin.size()));
    out += std::string("BIN\0", 4);
    out += bin;
    return out;
}

// A rock (48 x 24 sphere) and, with `authored`, a "rock_LOD1" node holding a 12 x 6 sphere.
std::string rock_glb(bool authored) {
    std::string bin;
    const Sphere fine = add_sphere(bin, 48, 24, 1.0f);
    const Sphere coarse = add_sphere(bin, 12, 6, 0.98f);
    std::string views, accessors;
    auto add = [&](const Sphere& s) {
        const u32 v = static_cast<u32>(std::count(views.begin(), views.end(), '{'));
        auto view = [&](size_t off, size_t len) {
            if (!views.empty()) views += ",";
            views += "{\"buffer\":0,\"byteOffset\":" + std::to_string(off) + ",\"byteLength\":" + std::to_string(len) + "}";
        };
        view(s.pos_off, s.vertices * 12);
        view(s.nrm_off, s.vertices * 12);
        view(s.idx_off, s.indices * 2);
        if (!accessors.empty()) accessors += ",";
        accessors += "{\"bufferView\":" + std::to_string(v) + ",\"componentType\":5126,\"count\":" + std::to_string(s.vertices) +
                     ",\"type\":\"VEC3\"},{\"bufferView\":" + std::to_string(v + 1) +
                     ",\"componentType\":5126,\"count\":" + std::to_string(s.vertices) + ",\"type\":\"VEC3\"},{\"bufferView\":" +
                     std::to_string(v + 2) + ",\"componentType\":5123,\"count\":" + std::to_string(s.indices) + ",\"type\":\"SCALAR\"}";
    };
    add(fine);
    add(coarse);
    std::string j = "{\"asset\":{\"version\":\"2.0\"},\"buffers\":[{\"byteLength\":" + std::to_string(bin.size()) + "}],";
    j += "\"bufferViews\":[" + views + "],\"accessors\":[" + accessors + "],";
    j += "\"meshes\":[{\"primitives\":[{\"attributes\":{\"POSITION\":0,\"NORMAL\":1},\"indices\":2}]},"
         "{\"primitives\":[{\"attributes\":{\"POSITION\":3,\"NORMAL\":4},\"indices\":5}]}],";
    if (authored)
        j += "\"nodes\":[{\"name\":\"rock\",\"mesh\":0},{\"name\":\"rock_LOD1\",\"mesh\":1,\"translation\":[5,0,0]}],"
             "\"scenes\":[{\"nodes\":[0,1]}]}";
    else
        j += "\"nodes\":[{\"name\":\"rock\",\"mesh\":0}],\"scenes\":[{\"nodes\":[0]}]}";
    return make_glb(j, bin);
}

std::string rock_project(const char* name, bool authored, const char* settings) {
    const std::string root = fresh_dir(name);
    write_file(root + "/assets/models/rock.glb", rock_glb(authored));
    write_file(root + "/assets/models/rock.glb.mceasset", sidecar(kRockGuid, "gltf", settings));
    return root;
}

std::string diag_text(const CookReport& r) {
    std::string s;
    for (const auto& d : r.diags.all()) {
        cook::String line;
        cook::format_diagnostic(d, line);
        s += line.c_str();
        s += "\n";
    }
    return s;
}

std::vector<byte> cooked_mesh(const std::string& root) {
    AssetSystem as;
    const std::string cooked = root + "/cooked";
    const StringView r[] = {StringView(cooked.c_str())};
    REQUIRE(as.mount(Span<const StringView>(r, 1)).has_value());
    auto h = as.load_sync<Mesh>(parse_guid(kRockGuid).value());
    REQUIRE(h.has_value());
    const Span<const byte> p = as.resolve(h.value());
    std::vector<byte> out(p.begin(), p.end());
    as.release(AnyHandle(h.value()));
    return out;
}

MeshView view_of(const std::vector<byte>& blob) {
    const Result<MeshView> v = MeshView::read(Span<const byte>(blob.data(), blob.size()));
    REQUIRE(v.has_value());
    return v.value();
}

}  // namespace

TEST_CASE("gltf lod: lod_levels simplifies each primitive into coarser levels over the same vertices") {
    CookService svc;
    const std::string none = rock_project("lod_none", false, "{}");
    REQUIRE(svc.cook(none.c_str(), CookOptions{}).ok());
    const std::vector<byte> plain = cooked_mesh(none);
    CHECK((view_of(plain).header->flags & MeshFlags::Lods) == 0);
    CHECK(view_of(plain).lods.empty());

    const std::string root = rock_project("lod_generated", false, "{ \"lod_levels\": 3, \"lod_ratio\": 0.3, \"lod_error\": 0.1 }");
    const CookReport r = svc.cook(root.c_str(), CookOptions{});
    INFO(diag_text(r));
    REQUIRE(r.ok());
    const std::vector<byte> blob = cooked_mesh(root);
    const MeshView v = view_of(blob);
    CHECK((v.header->flags & MeshFlags::Lods) != 0);
    REQUIRE(v.submeshes.size() == 1);
    const MeshSubmesh& sm = v.submeshes[0];
    const Span<const MeshLod> lods = v.submesh_lods(0);
    REQUIRE(lods.size() >= 2);
    u32 previous = sm.index_count;
    f32 error = 0.0f;
    for (const MeshLod& l : lods) {
        CHECK(l.index_count % 3 == 0);
        CHECK(l.index_count < previous);
        CHECK(l.error >= error);
        CHECK(l.error < 0.2f);  // node-local metres on a unit sphere, under lod_error x size
        CHECK(l.base_vertex == sm.base_vertex);
        CHECK(l.vertex_count == sm.vertex_count);
        previous = l.index_count;
        error = l.error;
    }
    CHECK(lods[0].error > 0.0f);
    CHECK(lods[0].index_count * 2 < sm.index_count);  // ~30% of the base
    // The base is untouched by the levels: the same mesh as without them.
    CHECK(sm.index_count == view_of(plain).submeshes[0].index_count);

    // Deterministic: a forced re-cook writes the same bytes.
    CookOptions force;
    force.force = true;
    REQUIRE(svc.cook(root.c_str(), force).ok());
    CHECK(cooked_mesh(root) == blob);
}

TEST_CASE("gltf lod: a <node>_LOD1 node is an authored level of <node>, not drawn itself") {
    CookService svc;
    const std::string root = rock_project("lod_authored", true, "{ \"lod_levels\": 3 }");
    const CookReport r = svc.cook(root.c_str(), CookOptions{});
    INFO(diag_text(r));
    REQUIRE(r.ok());
    const std::vector<byte> blob = cooked_mesh(root);
    const MeshView v = view_of(blob);
    REQUIRE(v.submeshes.size() == 1);  // the LOD node's primitive is a level, not a submesh
    CHECK(v.nodes.size() == 2);
    const Span<const MeshLod> lods = v.submesh_lods(0);
    REQUIRE(lods.size() == 1);  // authored levels win over lod_levels
    const MeshLod& l = lods[0];
    const MeshSubmesh& sm = v.submeshes[0];
    CHECK(l.base_vertex >= sm.base_vertex + sm.vertex_count);  // its own vertices
    CHECK(l.vertex_count < sm.vertex_count);
    CHECK(l.index_count == 12 * 6 * 6);
    CHECK(l.error > 0.0f);
    CHECK(l.error < 0.3f);
    // Its vertices dequantize with the base's bounds onto the coarse sphere (radius 0.98).
    for (u32 i = 0; i < l.vertex_count; ++i) {
        const MeshVertex& q = v.vertices[l.base_vertex + i];
        f32 len2 = 0.0f;
        for (int c = 0; c < 3; ++c) {
            const f32 p = sm.center[c] + q.position[c] / 32767.0f * sm.extent[c];
            len2 += p * p;
        }
        CHECK(std::sqrt(len2) == doctest::Approx(0.98f).epsilon(0.01));
    }
}

TEST_CASE("gltf lod: bad settings fail the cook; MeshView rejects a LOD range past the indices") {
    CookService svc;
    const std::string bad = rock_project("lod_bad", false, "{ \"lod_levels\": 5 }");
    const CookReport r = svc.cook(bad.c_str(), CookOptions{});
    CHECK_FALSE(r.ok());
    CHECK(diag_text(r).find("'lod_levels' must be a whole number 0..3") != std::string::npos);
    const std::string ratio = rock_project("lod_bad_ratio", false, "{ \"lod_levels\": 1, \"lod_ratio\": 1.5 }");
    CHECK_FALSE(svc.cook(ratio.c_str(), CookOptions{}).ok());

    const std::string root = rock_project("lod_corrupt", false, "{ \"lod_levels\": 1 }");
    REQUIRE(svc.cook(root.c_str(), CookOptions{}).ok());
    std::vector<byte> blob = cooked_mesh(root);
    const MeshView v = view_of(blob);
    REQUIRE(v.lods.size() == 1);
    const usize at = static_cast<usize>(reinterpret_cast<const byte*>(v.lods.data()) - blob.data());
    MeshLod l = v.lods[0];
    l.index_count = v.header->index_count + 3;
    std::memcpy(blob.data() + at, &l, sizeof l);
    CHECK_FALSE(MeshView::read(Span<const byte>(blob.data(), blob.size())).has_value());
}
