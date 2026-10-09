// glTF static import (FR-R3D-010, assets.md §5.2): .gltf (base64 buffers) and .glb cook to the
// same CHNK_MESH; quantization round-trips within its step; meshopt de-duplicates; images resolve
// to texture GUIDs as dependencies; node TRS + animation channels survive; malformed or
// unsupported input fails with the file and a JSON pointer.

#include <doctest/doctest.h>

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

constexpr const char* kTexGuid = "6a1d9c58-0f4b-4b8e-9d1b-1c2a3b4c5d01";
constexpr const char* kGltfGuid = "6a1d9c58-0f4b-4b8e-9d1b-1c2a3b4c5d02";
constexpr const char* kGlbGuid = "6a1d9c58-0f4b-4b8e-9d1b-1c2a3b4c5d03";

Guid G(const char* s) { return parse_guid(s).value(); }

void put_f32(std::string& s, float v) {
    char b[4];
    std::memcpy(b, &v, 4);
    s.append(b, 4);
}

std::string base64(const std::string& in) {
    static const char* k = "ABCDEFGHIJKLMNOPQRSTUVWXYZabcdefghijklmnopqrstuvwxyz0123456789+/";
    std::string out;
    size_t i = 0;
    for (; i + 2 < in.size(); i += 3) {
        const u32 v = (u8(in[i]) << 16) | (u8(in[i + 1]) << 8) | u8(in[i + 2]);
        for (int s = 18; s >= 0; s -= 6) out.push_back(k[(v >> s) & 63]);
    }
    if (i + 1 == in.size()) {
        const u32 v = u8(in[i]) << 16;
        out += k[(v >> 18) & 63];
        out += k[(v >> 12) & 63];
        out += "==";
    } else if (i + 2 == in.size()) {
        const u32 v = (u8(in[i]) << 16) | (u8(in[i + 1]) << 8);
        out += k[(v >> 18) & 63];
        out += k[(v >> 12) & 63];
        out += k[(v >> 6) & 63];
        out += '=';
    }
    return out;
}

// A unit cube (24 vertices: 4 per face, + 12 exact duplicates the importer must merge), a
// one-triangle primitive without normals or indices, two nodes and one animation.
struct Fixture {
    std::string bin;
    size_t pos_off, nrm_off, uv_off, idx_off, tri_off, t_off, r_off, tr_off, tv_off;
    u32 cube_vertices = 0, cube_indices = 0;
};

Fixture build_buffer() {
    Fixture f;
    struct V {
        float p[3], n[3], uv[2];
    };
    std::vector<V> verts;
    std::vector<u16> idx;
    const float axes[6][3] = {{1, 0, 0}, {-1, 0, 0}, {0, 1, 0}, {0, -1, 0}, {0, 0, 1}, {0, 0, -1}};
    for (const auto& n : axes) {
        // Two tangent axes for the face.
        float u[3] = {n[1] != 0 ? 1.0f : 0.0f, 0, n[1] != 0 ? 0.0f : 1.0f};
        if (n[2] != 0) {
            u[0] = 1;
            u[2] = 0;
        }
        const float w[3] = {n[1] * u[2] - n[2] * u[1], n[2] * u[0] - n[0] * u[2], n[0] * u[1] - n[1] * u[0]};
        const u16 base = static_cast<u16>(verts.size());
        for (int c = 0; c < 4; ++c) {
            const float su = (c == 1 || c == 2) ? 0.5f : -0.5f, sw = (c >= 2) ? 0.5f : -0.5f;
            V v{};
            for (int k = 0; k < 3; ++k) {
                v.p[k] = n[k] * 0.5f + u[k] * su + w[k] * sw;
                v.n[k] = n[k];
            }
            v.uv[0] = su + 0.5f;
            v.uv[1] = sw + 0.5f;
            verts.push_back(v);
        }
        const u16 q[6] = {0, 1, 2, 2, 3, 0};
        for (u16 i : q) idx.push_back(static_cast<u16>(base + i));
    }
    // 12 duplicates of the first 12 vertices, referenced by a second copy of the first two faces.
    for (u16 i = 0; i < 8; ++i) verts.push_back(verts[i]);
    for (u16 i = 0; i < 12; ++i) idx.push_back(static_cast<u16>(24 + (idx[i] % 8)));
    f.cube_vertices = static_cast<u32>(verts.size());
    f.cube_indices = static_cast<u32>(idx.size());

    f.pos_off = f.bin.size();
    for (const V& v : verts) for (float x : v.p) put_f32(f.bin, x);
    f.nrm_off = f.bin.size();
    for (const V& v : verts) for (float x : v.n) put_f32(f.bin, x);
    f.uv_off = f.bin.size();
    for (const V& v : verts) for (float x : v.uv) put_f32(f.bin, x);
    f.idx_off = f.bin.size();
    for (u16 i : idx) put16le(f.bin, i);
    while (f.bin.size() % 4) f.bin.push_back(0);
    f.tri_off = f.bin.size();  // one triangle in the XY plane, CCW seen from +Z
    const float tri[9] = {0, 0, 0, 1, 0, 0, 0, 1, 0};
    for (float x : tri) put_f32(f.bin, x);
    f.t_off = f.bin.size();  // key times
    put_f32(f.bin, 0.0f);
    put_f32(f.bin, 2.0f);
    f.r_off = f.bin.size();  // rotation keys: identity -> 90 deg about Y
    const float r[8] = {0, 0, 0, 1, 0, 0.70710678f, 0, 0.70710678f};
    for (float x : r) put_f32(f.bin, x);
    f.tr_off = f.bin.size();  // translation step keys
    const float t[6] = {0, 0, 0, 0, 3, 0};
    for (float x : t) put_f32(f.bin, x);
    f.tv_off = f.bin.size();
    return f;
}

std::string json_doc(const Fixture& f, const std::string& buffer_entry, const std::string& extra_root = "",
                     const std::string& image_uri = "../textures/hero.png", int mode = 4) {
    const u32 nv = f.cube_vertices;
    auto view = [](size_t off, size_t len) {
        return "{\"buffer\":0,\"byteOffset\":" + std::to_string(off) + ",\"byteLength\":" + std::to_string(len) + "}";
    };
    std::string s = "{\"asset\":{\"version\":\"2.0\"}," + extra_root;
    s += "\"buffers\":[" + buffer_entry + "],";
    s += "\"bufferViews\":[" + view(f.pos_off, nv * 12) + "," + view(f.nrm_off, nv * 12) + "," + view(f.uv_off, nv * 8) + "," +
         view(f.idx_off, f.cube_indices * 2) + "," + view(f.tri_off, 36) + "," + view(f.t_off, 8) + "," + view(f.r_off, 32) + "," +
         view(f.tr_off, 24) + "],";
    s += "\"accessors\":[";
    s += "{\"bufferView\":0,\"componentType\":5126,\"count\":" + std::to_string(nv) + ",\"type\":\"VEC3\"},";
    s += "{\"bufferView\":1,\"componentType\":5126,\"count\":" + std::to_string(nv) + ",\"type\":\"VEC3\"},";
    s += "{\"bufferView\":2,\"componentType\":5126,\"count\":" + std::to_string(nv) + ",\"type\":\"VEC2\"},";
    s += "{\"bufferView\":3,\"componentType\":5123,\"count\":" + std::to_string(f.cube_indices) + ",\"type\":\"SCALAR\"},";
    s += "{\"bufferView\":4,\"componentType\":5126,\"count\":3,\"type\":\"VEC3\"},";
    s += "{\"bufferView\":5,\"componentType\":5126,\"count\":2,\"type\":\"SCALAR\"},";
    s += "{\"bufferView\":6,\"componentType\":5126,\"count\":2,\"type\":\"VEC4\"},";
    s += "{\"bufferView\":7,\"componentType\":5126,\"count\":2,\"type\":\"VEC3\"}],";
    s += "\"images\":[{\"uri\":\"" + image_uri + "\"}],\"textures\":[{\"source\":0}],";
    s += "\"materials\":[{\"name\":\"crate\",\"pbrMetallicRoughness\":{\"baseColorFactor\":[1,0.5,0.25,1],\"metallicFactor\":0,"
         "\"roughnessFactor\":0.75,\"baseColorTexture\":{\"index\":0,\"extensions\":{\"KHR_texture_transform\":"
         "{\"offset\":[0.5,0],\"scale\":[0.5,1]}}}},\"doubleSided\":true}],";
    s += "\"meshes\":[{\"primitives\":[{\"attributes\":{\"POSITION\":0,\"NORMAL\":1,\"TEXCOORD_0\":2},\"indices\":3,\"material\":0,"
         "\"mode\":" + std::to_string(mode) + "},{\"attributes\":{\"POSITION\":4}}]}],";
    s += "\"nodes\":[{\"name\":\"root\",\"translation\":[1,2,3],\"children\":[1]},{\"name\":\"prop\",\"mesh\":0,\"scale\":[2,2,2]}],";
    s += "\"scenes\":[{\"nodes\":[0]}],\"scene\":0,";
    s += "\"animations\":[{\"name\":\"spin\",\"samplers\":[{\"input\":5,\"output\":6},{\"input\":5,\"output\":7,"
         "\"interpolation\":\"STEP\"}],\"channels\":[{\"sampler\":0,\"target\":{\"node\":1,\"path\":\"rotation\"}},"
         "{\"sampler\":1,\"target\":{\"node\":0,\"path\":\"translation\"}}]}]}";
    return s;
}

std::string make_glb(const std::string& json, const std::string& bin) {
    std::string j = json;
    while (j.size() % 4) j.push_back(' ');
    std::string b = bin;
    while (b.size() % 4) b.push_back('\0');
    std::string out = "glTF";
    put32le(out, 2);
    put32le(out, static_cast<u32>(12 + 8 + j.size() + 8 + b.size()));
    put32le(out, static_cast<u32>(j.size()));
    out += "JSON";
    out += j;
    put32le(out, static_cast<u32>(b.size()));
    out += std::string("BIN\0", 4);
    out += b;
    return out;
}

std::string make_project(const char* name, const std::string& gltf_json, bool with_glb = true) {
    const std::string root = fresh_dir(name);
    const std::string a = root + "/assets";
    write_file(a + "/textures/hero.png", make_gradient_png(4, 4));
    write_file(a + "/textures/hero.png.mceasset", sidecar(kTexGuid, "texture"));
    write_file(a + "/models/crate.gltf", gltf_json);
    write_file(a + "/models/crate.gltf.mceasset", sidecar(kGltfGuid, "gltf"));
    if (with_glb) {
        const Fixture f = build_buffer();
        const std::string json = json_doc(f, "{\"byteLength\":" + std::to_string(f.bin.size()) + "}");
        write_file(a + "/models/crate.glb", make_glb(json, f.bin));
        write_file(a + "/models/crate.glb.mceasset", sidecar(kGlbGuid, "gltf"));
    }
    return root;
}

std::string embedded_buffer(const Fixture& f) {
    return "{\"byteLength\":" + std::to_string(f.bin.size()) + ",\"uri\":\"data:application/octet-stream;base64," + base64(f.bin) +
           "\"}";
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

std::vector<byte> cooked_mesh(const std::string& root, const char* guid) {
    AssetSystem as;
    const std::string cooked = root + "/cooked";
    const StringView r[] = {StringView(cooked.c_str())};
    REQUIRE(as.mount(Span<const StringView>(r, 1)).has_value());
    auto h = as.load_sync<Mesh>(G(guid));
    REQUIRE(h.has_value());
    const Span<const byte> p = as.resolve(h.value());
    std::vector<byte> out(p.begin(), p.end());
    as.release(AnyHandle(h.value()));
    return out;
}

math::vec3 dequant(const MeshSubmesh& sm, const MeshVertex& v) {
    return {sm.center[0] + v.position[0] / 32767.0f * sm.extent[0], sm.center[1] + v.position[1] / 32767.0f * sm.extent[1],
            sm.center[2] + v.position[2] / 32767.0f * sm.extent[2]};
}

math::vec3 oct_decode(const MeshVertex& v) {
    float x = v.normal[0] / 32767.0f, y = v.normal[1] / 32767.0f;
    const float z = 1.0f - std::fabs(x) - std::fabs(y);
    if (z < 0) {
        const float ox = x;
        x = (1.0f - std::fabs(y)) * (ox >= 0 ? 1.0f : -1.0f);
        y = (1.0f - std::fabs(ox)) * (y >= 0 ? 1.0f : -1.0f);
    }
    return math::normalize(math::vec3{x, y, z});
}

}  // namespace

TEST_CASE("gltf: .gltf and .glb cook to the same quantized, optimized mesh") {
    const Fixture f = build_buffer();
    const std::string root = make_project("gltf_basic", json_doc(f, embedded_buffer(f)));
    CookService svc;
    const CookReport r = svc.cook(root.c_str(), CookOptions{});
    INFO(diag_text(r));
    REQUIRE(r.ok());
    CHECK(r.cooked == 3);

    const std::vector<byte> a = cooked_mesh(root, kGltfGuid);
    const std::vector<byte> b = cooked_mesh(root, kGlbGuid);
    CHECK(a == b);  // the payload carries no GUID: same source document, same bytes

    auto mv = MeshView::read(Span<const byte>(a.data(), a.size()));
    REQUIRE(mv.has_value());
    const MeshView& m = mv.value();
    REQUIRE(m.nodes.size() == 2);
    CHECK(m.nodes[0].parent == -1);
    CHECK(m.nodes[1].parent == 0);
    CHECK(m.node_name(0) == "root");
    CHECK(m.node_name(1) == "prop");
    CHECK(m.nodes[0].translation[1] == doctest::Approx(2.0f));
    CHECK(m.nodes[1].scale[0] == doctest::Approx(2.0f));
    REQUIRE(m.submeshes.size() == 2);
    CHECK(!m.index32());

    const MeshSubmesh& cube = m.submeshes[0];
    CHECK(cube.node == 1);
    CHECK(cube.vertex_count == 24);  // 32 authored, 8 exact duplicates merged
    CHECK(cube.index_count == f.cube_indices);
    for (u32 v = 0; v < cube.vertex_count; ++v) {
        const MeshVertex& q = m.vertices[cube.base_vertex + v];
        const math::vec3 p = dequant(cube, q);
        for (float c : {p.x, p.y, p.z}) CHECK(std::fabs(std::fabs(c) - 0.5f) < 1e-4f);
        const math::vec3 n = oct_decode(q);
        CHECK(std::fabs(n.x) + std::fabs(n.y) + std::fabs(n.z) == doctest::Approx(1.0f).epsilon(1e-3));
        // The KHR_texture_transform (u * 0.5 + 0.5) is baked: u lies in [0.5, 1].
        const float u = cube.uv_offset[0] + q.uv[0] / 65535.0f * cube.uv_scale[0];
        CHECK(u >= 0.5f - 1e-4f);
        CHECK(u <= 1.0f + 1e-4f);
    }
    for (u32 i = 0; i < cube.index_count; ++i) CHECK(m.index(cube.first_index + i) < cube.vertex_count);
    CHECK(cube.material.base_color[1] == doctest::Approx(0.5f));
    CHECK(cube.material.roughness == doctest::Approx(0.75f));
    CHECK((cube.material.flags & MaterialFlags::DoubleSided) != 0);
    CHECK(cube.material.texture(MaterialSlot::BaseColor) == G(kTexGuid));

    // The second primitive had no normals and no indices: both are generated.
    const MeshSubmesh& tri = m.submeshes[1];
    CHECK(tri.index_count == 3);
    CHECK(tri.vertex_count == 3);
    const math::vec3 tn = oct_decode(m.vertices[tri.base_vertex]);
    CHECK(tn.z == doctest::Approx(1.0f).epsilon(1e-3));

    // Rest bounds: cube of side 2 (node scale) and the triangle (up to y = 2), translated by (1, 2, 3).
    CHECK(m.header->bounds_min[0] == doctest::Approx(0.0f).epsilon(1e-3));
    CHECK(m.header->bounds_min[1] == doctest::Approx(1.0f).epsilon(1e-3));
    CHECK(m.header->bounds_max[1] == doctest::Approx(4.0f).epsilon(1e-3));
    CHECK(m.header->bounds_max[2] == doctest::Approx(4.0f).epsilon(1e-3));

    REQUIRE(m.clips.size() == 1);
    CHECK(m.clip_name(0) == "spin");
    CHECK(m.find_clip("spin") == 0);
    CHECK(m.clips[0].duration == doctest::Approx(2.0f));
    REQUIRE(m.clips[0].channel_count == 2);
    const MeshChannel& rot = m.channels[m.clips[0].first_channel];
    CHECK(rot.node == 1);
    CHECK(rot.path == AnimPath::Rotation);
    CHECK(rot.interp == AnimInterp::Linear);
    CHECK(rot.key_count == 2);
    CHECK(m.keys[rot.values_offset + 4 + 1] == doctest::Approx(0.70710678f));
    const MeshChannel& tr = m.channels[m.clips[0].first_channel + 1];
    CHECK(tr.node == 0);
    CHECK(tr.interp == AnimInterp::Step);
}

TEST_CASE("gltf: re-cooking with --force is byte-identical; the texture is a dependency") {
    const Fixture f = build_buffer();
    const std::string root = make_project("gltf_determinism", json_doc(f, embedded_buffer(f)), false);
    CookService svc;
    REQUIRE(svc.cook(root.c_str(), CookOptions{}).ok());
    const std::vector<byte> a = cooked_mesh(root, kGltfGuid);
    CookOptions force;
    force.force = true;
    const CookReport r = svc.cook(root.c_str(), force);
    REQUIRE(r.ok());
    CHECK(cooked_mesh(root, kGltfGuid) == a);
    // Topological order: the image cooks before the mesh that references it.
    REQUIRE(r.assets.size() == 2);
    CHECK(r.assets[0].guid == G(kTexGuid));
    CHECK(r.assets[1].guid == G(kGltfGuid));
}

TEST_CASE("gltf: import scale applies to the root nodes and their animation tracks") {
    const Fixture f = build_buffer();
    const std::string root = make_project("gltf_scale", json_doc(f, embedded_buffer(f)), false);
    write_file(root + "/assets/models/crate.gltf.mceasset", sidecar(kGltfGuid, "gltf", "{ \"scale\": 0.5 }"));
    CookService svc;
    const CookReport r = svc.cook(root.c_str(), CookOptions{});
    INFO(diag_text(r));
    REQUIRE(r.ok());
    const std::vector<byte> a = cooked_mesh(root, kGltfGuid);
    const MeshView m = MeshView::read(Span<const byte>(a.data(), a.size())).value();
    CHECK(m.nodes[0].translation[2] == doctest::Approx(1.5f));
    CHECK(m.nodes[0].scale[0] == doctest::Approx(0.5f));
    CHECK(m.nodes[1].scale[0] == doctest::Approx(2.0f));  // children keep their local scale
    const MeshChannel& tr = m.channels[1];
    CHECK(m.keys[tr.values_offset + 3 + 1] == doctest::Approx(1.5f));
}

TEST_CASE("gltf: normal, metallic-roughness, occlusion and emissive maps; CUBICSPLINE tracks") {
    Fixture f = build_buffer();
    // Cubic translation keys for node 1 at t = 0, 2: {in, value, out} per key.
    const size_t cubic_off = f.bin.size();
    const float cubic[18] = {0, 0, 0, 0, 0, 0, 1, 0, 0, -1, 0, 0, 4, 0, 0, 0, 0, 0};
    for (float x : cubic) put_f32(f.bin, x);
    std::string j = json_doc(f, embedded_buffer(f));
    const auto replace = [&](const std::string& from, const std::string& to) {
        const size_t at = j.find(from);
        REQUIRE(at != std::string::npos);
        j.replace(at, from.size(), to);
    };
    replace("}],\"accessors\":[", "},{\"buffer\":0,\"byteOffset\":" + std::to_string(cubic_off) + ",\"byteLength\":72}],\"accessors\":[");
    replace("\"type\":\"VEC3\"}],\"images\"",
            "\"type\":\"VEC3\"},{\"bufferView\":8,\"componentType\":5126,\"count\":6,\"type\":\"VEC3\"}],\"images\"");
    // Every map on the one image; occlusion shares the metallic-roughness texture (ORM packing).
    replace("\"doubleSided\":true}", "\"doubleSided\":true,\"normalTexture\":{\"index\":0},\"occlusionTexture\":{\"index\":0},"
                                     "\"emissiveTexture\":{\"index\":0},\"emissiveFactor\":[1,0.5,0]}");
    replace("\"roughnessFactor\":0.75,", "\"roughnessFactor\":0.75,\"metallicRoughnessTexture\":{\"index\":0},");
    replace("\"interpolation\":\"STEP\"}]",
            "\"interpolation\":\"STEP\"},{\"input\":5,\"output\":8,\"interpolation\":\"CUBICSPLINE\"}]");
    replace("\"path\":\"translation\"}}]", "\"path\":\"translation\"}},{\"sampler\":2,\"target\":{\"node\":1,\"path\":\"translation\"}}]");

    const std::string root = make_project("gltf_maps", j, false);
    CookService svc;
    const CookReport r = svc.cook(root.c_str(), CookOptions{});
    INFO(diag_text(r));
    REQUIRE(r.ok());
    const std::vector<byte> a = cooked_mesh(root, kGltfGuid);
    const MeshView m = MeshView::read(Span<const byte>(a.data(), a.size())).value();
    const MeshSubmesh& cube = m.submeshes[0];
    CHECK(cube.material.texture(MaterialSlot::Normal) == G(kTexGuid));
    CHECK(cube.material.texture(MaterialSlot::MetallicRoughness) == G(kTexGuid));
    CHECK(cube.material.texture(MaterialSlot::Emissive) == G(kTexGuid));
    CHECK((cube.material.flags & MaterialFlags::OcclusionInMR) != 0);
    CHECK((cube.material.flags & MaterialFlags::OcclusionOnly) == 0);
    CHECK(cube.material.emissive[1] == doctest::Approx(0.5f));
    // The base color's KHR_texture_transform differs from the other maps' (none): a warning,
    // since one UV transform per submesh is baked.
    CHECK(r.diags.warning_count() >= 1);

    REQUIRE(m.clips[0].channel_count == 3);
    const MeshChannel& cu = m.channels[m.clips[0].first_channel + 2];
    CHECK(cu.node == 1);
    CHECK(cu.path == AnimPath::Translation);
    CHECK(cu.interp == AnimInterp::CubicSpline);
    CHECK(cu.key_count == 2);
    CHECK(m.keys[cu.values_offset + 6] == doctest::Approx(1.0f));   // key 0 out-tangent x
    CHECK(m.keys[cu.values_offset + 9] == doctest::Approx(-1.0f));  // key 1 in-tangent x
    CHECK(m.keys[cu.values_offset + 12] == doctest::Approx(4.0f));  // key 1 value x
}

namespace {

bool has_error(const CookReport& r, const char* needle, Errc code = Errc::Unknown) {
    for (const auto& d : r.diags.all())
        if (d.severity == cook::Severity::Error && std::strstr(d.message.c_str(), needle) &&
            (code == Errc::Unknown || d.code == code) && std::strstr(d.file.c_str(), "crate.gltf"))
            return true;
    return false;
}

}  // namespace

TEST_CASE("gltf: a sidecar triangle budget fails the cook when the mesh exceeds it") {
    const Fixture f = build_buffer();
    CookService svc;
    SUBCASE("within budget") {
        const std::string root = make_project("gltf_budget_ok", json_doc(f, embedded_buffer(f)), false);
        write_file(root + "/assets/models/crate.gltf.mceasset", sidecar(kGltfGuid, "gltf", "{ \"max_triangles\": 100000 }"));
        const CookReport r = svc.cook(root.c_str(), CookOptions{});
        INFO(diag_text(r));
        CHECK(r.ok());
    }
    SUBCASE("over budget") {
        const std::string root = make_project("gltf_budget_over", json_doc(f, embedded_buffer(f)), false);
        write_file(root + "/assets/models/crate.gltf.mceasset", sidecar(kGltfGuid, "gltf", "{ \"max_triangles\": 1 }"));
        const CookReport r = svc.cook(root.c_str(), CookOptions{});
        INFO(diag_text(r));
        CHECK(!r.ok());
        CHECK(has_error(r, "over the mesh's budget of 1 (sidecar 'max_triangles')", Errc::InvalidArgument));
    }
    SUBCASE("malformed budget") {
        const std::string root = make_project("gltf_budget_bad", json_doc(f, embedded_buffer(f)), false);
        write_file(root + "/assets/models/crate.gltf.mceasset", sidecar(kGltfGuid, "gltf", "{ \"max_triangles\": 2.5 }"));
        const CookReport r = svc.cook(root.c_str(), CookOptions{});
        CHECK(has_error(r, "'max_triangles' must be a whole number"));
    }
}

TEST_CASE("gltf: unsupported and malformed input fails with the JSON pointer") {
    const Fixture f = build_buffer();
    CookService svc;
    SUBCASE("required extension") {
        const std::string root = make_project(
            "gltf_ext", json_doc(f, embedded_buffer(f), "\"extensionsRequired\":[\"KHR_draco_mesh_compression\"],"), false);
        const CookReport r = svc.cook(root.c_str(), CookOptions{});
        INFO(diag_text(r));
        CHECK(has_error(r, "/extensionsRequired/0: required extension 'KHR_draco_mesh_compression'"));
    }
    SUBCASE("image that is not a registered asset") {
        const std::string root = make_project("gltf_missing_image", json_doc(f, embedded_buffer(f), "", "missing.png"), false);
        const CookReport r = svc.cook(root.c_str(), CookOptions{});
        INFO(diag_text(r));
        CHECK(has_error(r, "/materials/0/pbrMetallicRoughness/baseColorTexture: image 'models/missing.png'", Errc::MissingRef));
    }
    SUBCASE("image URI escaping assets/") {
        const std::string root = make_project("gltf_escape", json_doc(f, embedded_buffer(f), "", "../../../etc/x.png"), false);
        const CookReport r = svc.cook(root.c_str(), CookOptions{});
        CHECK(has_error(r, "must be relative and stay inside assets/"));
    }
    SUBCASE("external .bin buffer") {
        const std::string root =
            make_project("gltf_external", json_doc(f, "{\"byteLength\":" + std::to_string(f.bin.size()) + ",\"uri\":\"crate.bin\"}"), false);
        const CookReport r = svc.cook(root.c_str(), CookOptions{});
        CHECK(has_error(r, "/buffers/0: external buffers are not supported"));
    }
    SUBCASE("non-triangle primitive") {
        const std::string root = make_project("gltf_lines", json_doc(f, embedded_buffer(f), "", "../textures/hero.png", 1), false);
        const CookReport r = svc.cook(root.c_str(), CookOptions{});
        CHECK(has_error(r, "/meshes/0/primitives/0: only triangle lists"));
    }
    SUBCASE("truncated buffer") {
        std::string short_bin = f.bin.substr(0, 40);
        const std::string root = make_project(
            "gltf_short",
            json_doc(f, "{\"byteLength\":40,\"uri\":\"data:application/octet-stream;base64," + base64(short_bin) + "\"}"), false);
        const CookReport r = svc.cook(root.c_str(), CookOptions{});
        CHECK(has_error(r, "accessor range exceeds its buffer view"));
    }
    SUBCASE("not JSON") {
        const std::string root = make_project("gltf_garbage", "{ nope", false);
        const CookReport r = svc.cook(root.c_str(), CookOptions{});
        CHECK(has_error(r, "JSON parse error"));
    }
}

TEST_CASE("gltf: MeshView rejects corrupt payloads") {
    const Fixture f = build_buffer();
    const std::string root = make_project("gltf_corrupt", json_doc(f, embedded_buffer(f)), false);
    CookService svc;
    REQUIRE(svc.cook(root.c_str(), CookOptions{}).ok());
    std::vector<byte> a = cooked_mesh(root, kGltfGuid);
    REQUIRE(MeshView::read(Span<const byte>(a.data(), a.size())).has_value());

    MeshHeader h;
    std::memcpy(&h, a.data(), sizeof(h));
    SUBCASE("index past its submesh's vertex range") {
        u16 bad = 0xFFFF;
        std::memcpy(a.data() + h.indices_offset, &bad, 2);
        CHECK(!MeshView::read(Span<const byte>(a.data(), a.size())).has_value());
    }
    SUBCASE("node whose parent follows it") {
        MeshNode n;
        std::memcpy(&n, a.data() + h.nodes_offset, sizeof(n));
        n.parent = 1;
        std::memcpy(a.data() + h.nodes_offset, &n, sizeof(n));
        CHECK(!MeshView::read(Span<const byte>(a.data(), a.size())).has_value());
    }
    SUBCASE("truncated") {
        CHECK(!MeshView::read(Span<const byte>(a.data(), 64)).has_value());
        CHECK(!MeshView::read(Span<const byte>(a.data(), static_cast<usize>(h.strings_offset))).has_value());
    }
    SUBCASE("unknown flag") {
        h.flags |= 0x80;
        std::memcpy(a.data(), &h, sizeof(h));
        CHECK(!MeshView::read(Span<const byte>(a.data(), a.size())).has_value());
    }
}
