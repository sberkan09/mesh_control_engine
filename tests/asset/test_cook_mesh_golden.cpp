// Cooked-mesh golden (M-IMPORT3D step 0): the CHNK_MESH payload of every glTF under samples/ and of this file's
// own fixtures (a cube, a skinned bar, a sphere with generated LODs, a sphere with an authored LOD) is hashed
// (FNV-1a 64) and compared with a list recorded on the commit before the mesh back end moved to mesh_build.cpp.
// The list changes only in a step that names the change (a deliberate change to quantization, the optimizer,
// LOD generation or the skin tables). Set MCE_MESH_GOLDEN_PRINT=1 to print the current list in source form.

#include <doctest/doctest.h>

#include <algorithm>
#include <cstdio>
#include <cstdlib>
#include <cstring>
#include <filesystem>
#include <map>
#include <string>
#include <vector>

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

std::string embedded_buffer(const Fixture& f) {
    return "{\"byteLength\":" + std::to_string(f.bin.size()) + ",\"uri\":\"data:application/octet-stream;base64," + base64(f.bin) +
           "\"}";
}

// A skinned bar (FR-R3D-060): 8 corners of a 0.4 x 2 x 0.4 box standing on the origin; the bottom
// four belong wholly to joint 0 ("hip"), the top four 0.75 to joint 1 ("knee", 1 m up) and 0.25
// to joint 0. Node order: armature -> hip -> knee, armature -> body (mesh + skin 0).
std::string skinned_doc(u8 top_joint = 1, const char* skin_extra = "") {
    std::string bin;
    for (int i = 0; i < 8; ++i) {
        put_f32(bin, (i & 1) ? 0.2f : -0.2f);
        put_f32(bin, (i & 2) ? 2.0f : 0.0f);
        put_f32(bin, (i & 4) ? 0.2f : -0.2f);
    }
    const size_t joints_off = bin.size();
    for (int i = 0; i < 8; ++i) {
        const bool top = (i & 2) != 0;
        bin.push_back(static_cast<char>(top ? top_joint : 0));
        bin.push_back(0);
        bin.push_back(0);
        bin.push_back(0);
    }
    const size_t weights_off = bin.size();
    for (int i = 0; i < 8; ++i) {
        const bool top = (i & 2) != 0;
        put_f32(bin, top ? 0.75f : 1.0f);
        put_f32(bin, top ? 0.25f : 0.0f);
        put_f32(bin, 0.0f);
        put_f32(bin, 0.0f);
    }
    const size_t ibm_off = bin.size();
    for (int m = 0; m < 2; ++m)
        for (int k = 0; k < 16; ++k) put_f32(bin, k % 5 == 0 ? 1.0f : (m == 1 && k == 13 ? -1.0f : 0.0f));
    const size_t idx_off = bin.size();
    const u16 faces[36] = {0, 1, 3, 3, 2, 0, 4, 6, 7, 7, 5, 4, 0, 4, 5, 5, 1, 0, 2, 3, 7, 7, 6, 2, 0, 2, 6, 6, 4, 0, 1, 5, 7, 7, 3, 1};
    for (u16 i : faces) put16le(bin, i);
    auto view = [](size_t off, size_t len) {
        return "{\"buffer\":0,\"byteOffset\":" + std::to_string(off) + ",\"byteLength\":" + std::to_string(len) + "}";
    };
    std::string s = "{\"asset\":{\"version\":\"2.0\"},";
    s += "\"buffers\":[{\"byteLength\":" + std::to_string(bin.size()) + ",\"uri\":\"data:application/octet-stream;base64," +
         base64(bin) + "\"}],";
    s += "\"bufferViews\":[" + view(0, 96) + "," + view(joints_off, 32) + "," + view(weights_off, 128) + "," + view(ibm_off, 128) +
         "," + view(idx_off, 72) + "],";
    s += "\"accessors\":[{\"bufferView\":0,\"componentType\":5126,\"count\":8,\"type\":\"VEC3\"},"
         "{\"bufferView\":1,\"componentType\":5121,\"count\":8,\"type\":\"VEC4\"},"
         "{\"bufferView\":2,\"componentType\":5126,\"count\":8,\"type\":\"VEC4\"},"
         "{\"bufferView\":3,\"componentType\":5126,\"count\":2,\"type\":\"MAT4\"},"
         "{\"bufferView\":4,\"componentType\":5123,\"count\":36,\"type\":\"SCALAR\"}],";
    s += "\"meshes\":[{\"primitives\":[{\"attributes\":{\"POSITION\":0,\"JOINTS_0\":1,\"WEIGHTS_0\":2},\"indices\":4}]}],";
    s += "\"nodes\":[{\"name\":\"armature\",\"children\":[1,3]},{\"name\":\"hip\",\"children\":[2]},"
         "{\"name\":\"knee\",\"translation\":[0,1,0]},{\"name\":\"body\",\"mesh\":0,\"skin\":0}],";
    s += "\"skins\":[{\"joints\":[1,2],\"inverseBindMatrices\":3" + std::string(skin_extra) + "}],";
    s += "\"scenes\":[{\"nodes\":[0]}],\"scene\":0}";
    return s;
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

struct Golden {
    const char* name;
    u64 hash;
};

// Recorded on the base commit (before mesh_build.cpp). Sorted by name.
const Golden kGolden[] = {
    {"fixture/models/cube.glb", 0xfcdc180854e0f88eull},
    {"fixture/models/cube.gltf", 0xfcdc180854e0f88eull},
    {"fixture/models/rock.glb", 0x96716f939a9fb113ull},
    {"fixture/models/rock_authored.glb", 0xc4c78b80c7e950ecull},
    {"fixture/models/rock_lods.glb", 0xa5902696bebef7a0ull},
    {"fixture/models/rock_scaled.glb", 0x8e3127f3c5218e82ull},
    {"fixture/models/skin.gltf", 0x223f71e6e0fdee37ull},
    {"fixture/models/skin_static.gltf", 0x379755f58aec5642ull},
    {"samples/04_cubes/models/bulb.glb", 0x8cb4c1f8b324aa16ull},
    {"samples/04_cubes/models/floor.glb", 0x18c2791f5c8066d5ull},
    {"samples/04_cubes/models/orrery.glb", 0x5e0960d18f245f1dull},
    {"samples/04_cubes/models/panel_cube.glb", 0xe91431c8a8702167ull},
    {"samples/04_cubes/models/reed.glb", 0x77cabb355199ec1aull},
    {"samples/04_cubes/models/sphere_row.glb", 0xbdadc8e975a9f53dull},
    {"samples/04_cubes/models/walker.glb", 0x107f0f6569aa306full},
    {"samples/04_diorama/models/building-garage.glb", 0x15b59cc7959b5c83ull},
    {"samples/04_diorama/models/building-small-a.glb", 0xc84f5ac03943112eull},
    {"samples/04_diorama/models/building-small-b.glb", 0x8e1129eb8e8af617ull},
    {"samples/04_diorama/models/building-small-c.glb", 0xd68056fa3ea13b42ull},
    {"samples/04_diorama/models/building-small-d.glb", 0x1f13907a882665daull},
    {"samples/04_diorama/models/character.glb", 0x54a2de9a286283c8ull},
    {"samples/04_diorama/models/cloud.glb", 0x580148b1778bab0aull},
    {"samples/04_diorama/models/coin.glb", 0x0283a4f8ddb984a6ull},
    {"samples/04_diorama/models/flag.glb", 0x57b5de021f095b06ull},
    {"samples/04_diorama/models/grass-trees-tall.glb", 0x2aea40c6b75ac4f4ull},
    {"samples/04_diorama/models/grass-trees.glb", 0x40c0f9a7ec185a60ull},
    {"samples/04_diorama/models/grass.glb", 0x7ff8476a3730ffa4ull},
    {"samples/04_diorama/models/pavement-fountain.glb", 0x00cc883e0273299eull},
    {"samples/04_diorama/models/pavement.glb", 0x5432f2fae320bb6dull},
    {"samples/04_diorama/models/road-corner.glb", 0xc11fe43ea89ba51full},
    {"samples/04_diorama/models/road-intersection.glb", 0x4a72dcaccae5972full},
    {"samples/04_diorama/models/road-split.glb", 0x058c5aa8f4b8b605ull},
    {"samples/04_diorama/models/road-straight-lightposts.glb", 0x69604c516a468bddull},
    {"samples/04_diorama/models/road-straight.glb", 0x9824ce4f60ab16c7ull},
    {"samples/05_ashen_court/models/arch.glb", 0x9d038f3e90e3f08full},
    {"samples/05_ashen_court/models/brazier.glb", 0x844d950246c3d4e2ull},
    {"samples/05_ashen_court/models/broken_column.glb", 0x86002e70e384e3cdull},
    {"samples/05_ashen_court/models/brute.glb", 0xc202a0d96b8704a5ull},
    {"samples/05_ashen_court/models/column.glb", 0x630974159c030e16ull},
    {"samples/05_ashen_court/models/court_floor.glb", 0xe3ef4017ef9e5e64ull},
    {"samples/05_ashen_court/models/dais.glb", 0xc88e6b4a26ac8b81ull},
    {"samples/05_ashen_court/models/ember_heart.glb", 0x4da53d7748f3385bull},
    {"samples/05_ashen_court/models/hero.glb", 0x8ee8e8bdb1901f7eull},
    {"samples/05_ashen_court/models/husk.glb", 0x3fa7a2a6d12404d1ull},
    {"samples/05_ashen_court/models/orb.glb", 0x9fd4bc553bdcdf17ull},
    {"samples/05_ashen_court/models/rubble.glb", 0x8e85e1c96156f532ull},
    {"samples/05_ashen_court/models/sword.glb", 0xbec2e5fd7049d265ull},
    {"samples/05_ashen_court/models/wall_segment.glb", 0xb8ffd85ed2d623deull},
    {"samples/05_ashen_court/models/wisp.glb", 0xaf080145d1a153bcull},
    {"samples/06_showcase/models/brazier.glb", 0xc54900399caa9b67ull},
    {"samples/06_showcase/models/cobbles.glb", 0xf0cb4f086bdb8175ull},
    {"samples/06_showcase/models/courtyard.glb", 0xb5fc0e64c2b2ae6dull},
    {"samples/06_showcase/models/crate.glb", 0x781cdc87f9bc4497ull},
    {"samples/06_showcase/models/flower.glb", 0x830b61bc65f69db6ull},
    {"samples/06_showcase/models/grass.glb", 0x7af2edf104e343ecull},
    {"samples/06_showcase/models/lantern.glb", 0xb1dcfcc135ab9060ull},
    {"samples/06_showcase/models/pavilion.glb", 0xd3be38e571b2365dull},
    {"samples/06_showcase/models/statue.glb", 0x9019fceaa5cc5689ull},
    {"samples/06_showcase/models/wall_cap.glb", 0x07fff0f975e4039bull},
    {"samples/06_showcase/models/wall_pillar.glb", 0xeef2decaa3972c26ull},
    {"samples/06_showcase/models/wall_slab.glb", 0x61bfe0e86735c5ddull},
    {"samples/07_gameplay3d/models/bridge.glb", 0xc5827e6085843714ull},
    {"samples/07_gameplay3d/models/cart.glb", 0xc13f4d832988eb19ull},
    {"samples/07_gameplay3d/models/door.glb", 0xf9326af46adf6525ull},
    {"samples/07_gameplay3d/models/floor.glb", 0xa593f3ca5b7db3f2ull},
    {"samples/07_gameplay3d/models/grunt.glb", 0x3ddbd5cb30dea27eull},
    {"samples/07_gameplay3d/models/platform.glb", 0x9f74288193d02af7ull},
    {"samples/07_gameplay3d/models/wall.glb", 0xe2306426c5ca4c54ull},
    {"samples/08_movement/models/deck_a.glb", 0x0edacb8dcb657abeull},
    {"samples/08_movement/models/deck_b.glb", 0xd7337551b2f4aa51ull},
    {"samples/08_movement/models/goal.glb", 0x350a66fdb0fc8d8bull},
    {"samples/08_movement/models/ground.glb", 0xffc273cbb6591922ull},
    {"samples/08_movement/models/kart.glb", 0x8c5899ab20242364ull},
    {"samples/08_movement/models/lift.glb", 0x1e70b2762bf87001ull},
    {"samples/08_movement/models/marker.glb", 0x9e4b6dc907316f0dull},
    {"samples/08_movement/models/runner.glb", 0x3a171297bba3a1f5ull},
    {"samples/08_movement/models/track_ground.glb", 0x62c50353d5d4c8b8ull},
    {"samples/08_movement/models/tunnel_lintel.glb", 0x3aed460631a860e8ull},
    {"samples/08_movement/models/tunnel_wall.glb", 0x16eb8e3029fd2510ull},
    {"samples/09_openworld/models/box.glb", 0xf9aa709a08c4fe96ull},
    {"samples/09_openworld/third_party/kenney-blocky-characters/models/character-a.glb", 0xb485884391ad2ab4ull},
    {"samples/09_openworld/third_party/kenney-blocky-characters/models/character-b.glb", 0xaa8d8dc7f79e1cf5ull},
    {"samples/09_openworld/third_party/kenney-blocky-characters/models/character-c.glb", 0xfe6ca0625397d828ull},
    {"samples/09_openworld/third_party/kenney-blocky-characters/models/character-d.glb", 0xd606a0afcd8f99dbull},
    {"samples/09_openworld/third_party/kenney-blocky-characters/models/character-e.glb", 0x440ad2dbf8897988ull},
    {"samples/09_openworld/third_party/kenney-blocky-characters/models/character-f.glb", 0xe67eefb686d099cbull},
    {"samples/09_openworld/third_party/kenney-car-kit/models/delivery.glb", 0x4eabffc832ec8fbaull},
    {"samples/09_openworld/third_party/kenney-car-kit/models/police.glb", 0xb757e010cdd4a6bcull},
    {"samples/09_openworld/third_party/kenney-car-kit/models/sedan-sports.glb", 0x8137e07aff451ca3ull},
    {"samples/09_openworld/third_party/kenney-car-kit/models/sedan.glb", 0xeabdd30c7ac7a634ull},
    {"samples/09_openworld/third_party/kenney-car-kit/models/suv.glb", 0x85ad62c4f716b480ull},
    {"samples/09_openworld/third_party/kenney-car-kit/models/taxi.glb", 0xf15d5b12ebfeca91ull},
    {"samples/09_openworld/third_party/kenney-city-kit-commercial/models/building-a.glb", 0x37930bc585780876ull},
    {"samples/09_openworld/third_party/kenney-city-kit-commercial/models/building-b.glb", 0x3de8188f041e64d0ull},
    {"samples/09_openworld/third_party/kenney-city-kit-commercial/models/building-c.glb", 0x2c9c13553c76691bull},
    {"samples/09_openworld/third_party/kenney-city-kit-commercial/models/building-d.glb", 0xd6bde0faba34d132ull},
    {"samples/09_openworld/third_party/kenney-city-kit-commercial/models/building-e.glb", 0xace9d7db3f6b1088ull},
    {"samples/09_openworld/third_party/kenney-city-kit-commercial/models/building-f.glb", 0x2c87acb0fa88f137ull},
    {"samples/09_openworld/third_party/kenney-city-kit-commercial/models/building-g.glb", 0x5b5e74cd80842d3aull},
    {"samples/09_openworld/third_party/kenney-city-kit-commercial/models/building-h.glb", 0x24d7f3f3be3690fcull},
    {"samples/09_openworld/third_party/kenney-city-kit-commercial/models/building-i.glb", 0x8f2d908392f6f16dull},
    {"samples/09_openworld/third_party/kenney-city-kit-commercial/models/building-j.glb", 0x073a06c0312d7e30ull},
    {"samples/09_openworld/third_party/kenney-city-kit-commercial/models/building-k.glb", 0xd246a06b5268a507ull},
    {"samples/09_openworld/third_party/kenney-city-kit-commercial/models/building-l.glb", 0x1cf6c623e31f5b32ull},
    {"samples/09_openworld/third_party/kenney-city-kit-commercial/models/building-m.glb", 0xa61fad824596369eull},
    {"samples/09_openworld/third_party/kenney-city-kit-commercial/models/building-n.glb", 0x30760aa694470fd9ull},
    {"samples/09_openworld/third_party/kenney-city-kit-commercial/models/building-skyscraper-a.glb", 0x06a8f897882f1402ull},
    {"samples/09_openworld/third_party/kenney-city-kit-commercial/models/building-skyscraper-b.glb", 0x44c946c708f21ad7ull},
    {"samples/09_openworld/third_party/kenney-city-kit-commercial/models/building-skyscraper-c.glb", 0xc861d6deff11f6d8ull},
    {"samples/09_openworld/third_party/kenney-city-kit-suburban/models/building-type-a.glb", 0xd6274bb2bdf232f2ull},
    {"samples/09_openworld/third_party/kenney-city-kit-suburban/models/building-type-b.glb", 0xcc7f49c3fe6cf73cull},
    {"samples/09_openworld/third_party/kenney-city-kit-suburban/models/building-type-c.glb", 0x86e1c57c9926814full},
    {"samples/09_openworld/third_party/kenney-city-kit-suburban/models/building-type-d.glb", 0x59f4179551227d0full},
    {"samples/09_openworld/third_party/kenney-city-kit-suburban/models/building-type-e.glb", 0xa72be4b4402424d8ull},
    {"samples/09_openworld/third_party/kenney-city-kit-suburban/models/building-type-f.glb", 0x207075cb5eb11f42ull},
    {"samples/10_missions/models/floor.glb", 0xa593f3ca5b7db3f2ull},
    {"samples/10_missions/models/grunt.glb", 0x3ddbd5cb30dea27eull},
    {"samples/10_missions/models/platform.glb", 0x9f74288193d02af7ull},
    {"samples/cinema_demo/models/crate.glb", 0x75071bac877d3561ull},
    {"samples/cinema_demo/models/floor.glb", 0xc1a71f9c9bc7ef7eull},
    {"samples/cinema_demo/models/hero.glb", 0x4af44d6ba68f71b7ull},
    {"samples/cinema_demo/models/lamp.glb", 0x21779ad752359465ull},
    {"samples/cinema_demo/models/pillar.glb", 0x26fd6ac1db48ef4cull},
    {"samples/cinema_demo/models/wall.glb", 0x20233f5c5972163bull},
    {"samples/kit3d_tour/models/cart.glb", 0xc13f4d832988eb19ull},
    {"samples/kit3d_tour/models/grunt.glb", 0x3ddbd5cb30dea27eull},
    {"samples/kit3d_tour/models/platform.glb", 0x9f74288193d02af7ull},
    {"samples/import_demo/models/lamp.gltf", 0x2c1c7e56b23e617full},  // M-IMPORT3D 4b: the demo's glTF triangle
};

u64 fnv1a64(const std::vector<byte>& b) {
    u64 h = 0xcbf29ce484222325ull;
    for (byte c : b) {
        h ^= static_cast<u8>(c);
        h *= 0x100000001b3ull;
    }
    return h;
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

std::vector<byte> cooked_mesh(const std::string& root, const Guid& guid) {
    AssetSystem as;
    const std::string cooked = root + "/cooked";
    const StringView r[] = {StringView(cooked.c_str())};
    REQUIRE(as.mount(Span<const StringView>(r, 1)).has_value());
    auto h = as.load_sync<Mesh>(guid);
    REQUIRE(h.has_value());
    const Span<const byte> p = as.resolve(h.value());
    std::vector<byte> out(p.begin(), p.end());
    as.release(AnyHandle(h.value()));
    return out;
}

bool is_gltf_path(const std::string& p) {
    auto ends = [&](const char* e) { return p.size() >= std::strlen(e) && p.compare(p.size() - std::strlen(e), std::string::npos, e) == 0; };
    return ends(".glb") || ends(".gltf");
}

// Cooks `paths` (relative to assets/) of the project at `root` and records the mesh hash of every glTF asset as
// `<prefix>/<path>`.
void cook_and_hash(const std::string& root, const std::vector<std::string>& paths, const std::string& prefix,
                   std::map<std::string, u64>& out) {
    CookOptions opt;
    for (const std::string& p : paths) opt.paths.push_back(cook::String(p.c_str()));
    CookService svc;
    const CookReport r = svc.cook(root.c_str(), opt);
    INFO(prefix);
    INFO(diag_text(r));
    REQUIRE(r.ok());
    u32 meshes = 0;
    for (const auto& a : r.assets) {
        const std::string path = a.path.c_str();
        if (!is_gltf_path(path)) continue;
        std::string key = prefix;
        key += '/';
        key += path;
        out[key] = fnv1a64(cooked_mesh(root, a.guid));
        ++meshes;
    }
    CHECK(meshes == paths.size());
}

void fixture_project(const char* name, const std::vector<std::pair<std::string, std::pair<std::string, std::string>>>& files,
                     std::map<std::string, u64>& out) {
    // files: path under assets/ -> (bytes, sidecar text); a png texture is always present for the cube's image.
    const std::string root = fresh_dir(name);
    const std::string a = root + "/assets";
    write_file(a + "/textures/hero.png", make_gradient_png(4, 4));
    write_file(a + "/textures/hero.png.mceasset", sidecar(kTexGuid, "texture"));
    std::vector<std::string> paths;
    for (const auto& f : files) {
        write_file(a + "/" + f.first, f.second.first);
        write_file(a + "/" + f.first + ".mceasset", f.second.second);
        paths.push_back(f.first);
    }
    cook_and_hash(root, paths, std::string("fixture"), out);
}

std::map<std::string, u64> collect() {
    std::map<std::string, u64> out;
    namespace fs = std::filesystem;

    // The fixtures: the cube (.gltf and .glb), the skinned bar (rigged and imported static), the rock.
    {
        const Fixture f = build_buffer();
        const std::string json = json_doc(f, "{\"byteLength\":" + std::to_string(f.bin.size()) + "}");
        fixture_project("mesh_golden_cube",
                        {{"models/cube.gltf", {json_doc(f, embedded_buffer(f)), sidecar(kGltfGuid, "gltf")}},
                         {"models/cube.glb", {make_glb(json, f.bin), sidecar(kGlbGuid, "gltf")}}},
                        out);
    }
    fixture_project("mesh_golden_skin", {{"models/skin.gltf", {skinned_doc(), sidecar(kGltfGuid, "gltf")}},
                                         {"models/skin_static.gltf", {skinned_doc(), sidecar(kGlbGuid, "gltf", "{ \"skinning\": false }")}}},
                    out);
    fixture_project("mesh_golden_rock",
                    {{"models/rock.glb", {rock_glb(false), sidecar(kGltfGuid, "gltf")}},
                     {"models/rock_lods.glb", {rock_glb(false), sidecar(kGlbGuid, "gltf", "{ \"lod_levels\": 3, \"lod_ratio\": 0.3, \"lod_error\": 0.1 }")}},
                     {"models/rock_authored.glb", {rock_glb(true), sidecar("6a1d9c58-0f4b-4b8e-9d1b-1c2a3b4c5d04", "gltf", "{ \"lod_levels\": 3 }")}},
                     {"models/rock_scaled.glb", {rock_glb(false), sidecar("6a1d9c58-0f4b-4b8e-9d1b-1c2a3b4c5d05", "gltf", "{ \"scale\": 2.5 }")}}},
                    out);

    // Every sample project that ships glTF models: its assets/ is copied (the cook writes next to it) and the
    // models are cooked with their textures.
    std::vector<fs::path> samples;
    for (const auto& e : fs::directory_iterator(MCE_SAMPLES_DIR))
        if (e.is_directory() && fs::is_directory(e.path() / "assets")) samples.push_back(e.path());
    std::sort(samples.begin(), samples.end());
    for (const fs::path& s : samples) {
        std::vector<std::string> models;
        const fs::path assets = s / "assets";
        for (const auto& e : fs::recursive_directory_iterator(assets))
            if (e.is_regular_file() && is_gltf_path(e.path().generic_string())) models.push_back(fs::relative(e.path(), assets).generic_string());
        if (models.empty()) continue;
        std::sort(models.begin(), models.end());
        const std::string name = s.filename().generic_string();
        const std::string root = fresh_dir("mesh_golden_" + name);
        fs::copy(assets, root + "/assets", fs::copy_options::recursive);
        cook_and_hash(root, models, "samples/" + name, out);
    }
    return out;
}

}  // namespace

TEST_CASE("mesh golden: every sample and fixture glTF cooks to the recorded CHNK_MESH bytes") {
    const std::map<std::string, u64> got = collect();
    if (const char* p = std::getenv("MCE_MESH_GOLDEN_PRINT"); p && *p) {
        for (const auto& kv : got) std::printf("    {\"%s\", 0x%016llxull},\n", kv.first.c_str(), static_cast<unsigned long long>(kv.second));
    }
    // The rock fixtures place vertices with std::sin / std::cos, whose last bit differs between libm builds, so their
    // hashes are recorded for x86_64 only (cook goldens are guaranteed there only).
#if defined(__x86_64__) || defined(_M_X64)
    constexpr bool kHostLibm = true;
#else
    constexpr bool kHostLibm = false;
#endif
    std::size_t expected = 0;
    for (const Golden& g : kGolden) expected += (kHostLibm || std::strstr(g.name, "/rock") == nullptr) ? 1 : 0;
    CHECK(got.size() == expected + (kHostLibm ? 0 : 4));
    for (const Golden& g : kGolden) {
        if (!kHostLibm && std::strstr(g.name, "/rock") != nullptr) continue;
        const auto it = got.find(g.name);
        INFO(g.name);
        REQUIRE(it != got.end());
        CHECK(it->second == g.hash);
    }
}
