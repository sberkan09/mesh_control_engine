// glTF gaps (M-IMPORT3D step 1): sparse accessors, KHR_mesh_quantization, EXT_meshopt_compression (with its
// filters and a fallback buffer), Draco handling, JOINTS_1/WEIGHTS_1, baked default morph weights and the
// dropped-feature warnings; plus corrupt-input cases (truncated / flipped compressed streams, bad sparse indices,
// absurd counts) that must fail with a JSON pointer, never crash or allocate without bound.
//
// Fixtures are built here as GLB (JSON + binary chunk); the meshopt streams are encoded with meshoptimizer
// (the "meshopt wall" in scripts/ci/check_walls.py allows this file). The importer is called directly: no
// project tree, so the corrupt-input loops stay in the millisecond range per case.

#include <doctest/doctest.h>

#include <algorithm>
#include <array>
#include <cmath>
#include <cstdio>
#include <cstring>
#include <ctime>
#include <string>
#include <vector>

#include "cook_test_util.h"
#include "mce/asset/container.h"
#include "mce/asset/formats.h"
#include "mce/cook/importer.h"
#include "mce/math/math.h"
#include "meshoptimizer.h"

using namespace mce;
using namespace mce::asset;
using cook_test::put32le;

namespace {

// ---- fixture plumbing -------------------------------------------------------------------------------------

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

template <class T>
std::string bytes_of(const std::vector<T>& v) {
    return std::string(reinterpret_cast<const char*>(v.data()), v.size() * sizeof(T));
}

template <class T>
std::string pack(const std::vector<u32>& v) {
    std::string s;
    for (u32 x : v) {
        const T t = static_cast<T>(x);
        s.append(reinterpret_cast<const char*>(&t), sizeof(T));
    }
    return s;
}

// Buffer views and accessors over one binary chunk.
struct Doc {
    std::string bin = std::string(4, '\0');  // a GLB without a binary chunk has no buffer 0
    std::vector<std::string> views, accs;
    size_t fallback_size = 0;  // buffer 1 (the EXT_meshopt_compression fallback, no data): the decoded bytes it stands for

    size_t put(const std::string& data) {
        while (bin.size() % 4) bin.push_back('\0');
        const size_t off = bin.size();
        bin += data;
        return off;
    }
    int view(size_t off, size_t len, const std::string& extra = "") {
        views.push_back("{\"buffer\":0,\"byteOffset\":" + std::to_string(off) + ",\"byteLength\":" + std::to_string(len) + extra + "}");
        return static_cast<int>(views.size()) - 1;
    }
    int data_view(const std::string& data, const std::string& extra = "") { return view(put(data), data.size(), extra); }
    // ctype: the numeric componentType; extra: more members ("normalized":true, "byteOffset":.., "sparse":{..}).
    int acc(int view_index, int ctype, size_t count, const char* type, const std::string& extra = "") {
        std::string s = "{";
        if (view_index >= 0) s += "\"bufferView\":" + std::to_string(view_index) + ",";
        s += "\"componentType\":" + std::to_string(ctype) + ",\"count\":" + std::to_string(count) + ",\"type\":\"" + type + "\"";
        if (!extra.empty()) s += "," + extra;
        accs.push_back(s + "}");
        return static_cast<int>(accs.size()) - 1;
    }
    // A bufferView holding `stream` (compressed) in buffer 0, decoding to count * stride bytes in the fallback buffer 1.
    int meshopt_view(const std::string& stream, size_t count, size_t stride, const char* mode, const char* filter = "NONE") {
        const size_t off = put(stream);
        const size_t decoded = count * stride;
        std::string s = "{\"buffer\":1,\"byteOffset\":" + std::to_string(fallback_size) + ",\"byteLength\":" + std::to_string(decoded);
        s += ",\"extensions\":{\"EXT_meshopt_compression\":{\"buffer\":0,\"byteOffset\":" + std::to_string(off) +
             ",\"byteLength\":" + std::to_string(stream.size()) + ",\"byteStride\":" + std::to_string(stride) +
             ",\"count\":" + std::to_string(count) + ",\"mode\":\"" + mode + "\",\"filter\":\"" + filter + "\"}}}";
        fallback_size += decoded;
        views.push_back(s);
        return static_cast<int>(views.size()) - 1;
    }
    static std::string join(const std::vector<std::string>& v) {
        std::string s;
        for (size_t i = 0; i < v.size(); ++i) s += (i ? "," : "") + v[i];
        return s;
    }
    // `rest`: scenes / nodes / meshes / extension lists ... (members without the braces).
    std::string json(const std::string& rest) const {
        std::string s = "{\"asset\":{\"version\":\"2.0\"}," + rest + ",\"buffers\":[{\"byteLength\":" + std::to_string(bin.size()) + "}";
        if (fallback_size)
            s += ",{\"byteLength\":" + std::to_string(fallback_size) + ",\"extensions\":{\"EXT_meshopt_compression\":{\"fallback\":true}}}";
        s += "],\"bufferViews\":[" + join(views) + "],\"accessors\":[" + join(accs) + "]}";
        return s;
    }
    std::string glb(const std::string& rest) const { return make_glb(json(rest), bin); }
};

std::string encode_vertices(const void* data, size_t count, size_t stride) {
    std::string out(meshopt_encodeVertexBufferBound(count, stride), '\0');
    out.resize(meshopt_encodeVertexBuffer(reinterpret_cast<unsigned char*>(out.data()), out.size(), data, count, stride));
    return out;
}

std::string encode_triangles(const std::vector<u16>& idx, size_t vertex_count) {
    std::vector<unsigned int> wide(idx.begin(), idx.end());
    std::string out(meshopt_encodeIndexBufferBound(wide.size(), vertex_count), '\0');
    out.resize(meshopt_encodeIndexBuffer(reinterpret_cast<unsigned char*>(out.data()), out.size(), wide.data(), wide.size()));
    return out;
}

std::string encode_sequence(const std::vector<u16>& idx, size_t vertex_count) {
    std::vector<unsigned int> wide(idx.begin(), idx.end());
    std::string out(meshopt_encodeIndexSequenceBound(wide.size(), vertex_count), '\0');
    out.resize(meshopt_encodeIndexSequence(reinterpret_cast<unsigned char*>(out.data()), out.size(), wide.data(), wide.size()));
    return out;
}

struct Cube {
    std::vector<float> pos, nrm, uv;
    std::vector<u16> idx;
    static constexpr u32 kVerts = 24;
};

Cube make_cube() {
    Cube c;
    for (int a = 0; a < 3; ++a)
        for (int sign = -1; sign <= 1; sign += 2) {
            const u16 base = static_cast<u16>(c.pos.size() / 3);
            const float su[4] = {-0.5f, 0.5f, 0.5f, -0.5f}, sw[4] = {-0.5f, -0.5f, 0.5f, 0.5f};
            for (int k = 0; k < 4; ++k) {
                float p[3] = {0, 0, 0}, n[3] = {0, 0, 0};
                p[a] = 0.5f * static_cast<float>(sign);
                n[a] = static_cast<float>(sign);
                p[(a + 1) % 3] = su[k];
                p[(a + 2) % 3] = sw[k];
                for (float x : p) c.pos.push_back(x);
                for (float x : n) c.nrm.push_back(x);
                c.uv.push_back(su[k] + 0.5f);
                c.uv.push_back(sw[k] + 0.5f);
            }
            const u16 q[6] = {0, 1, 2, 2, 3, 0};
            for (u16 i : q) c.idx.push_back(static_cast<u16>(base + i));
        }
    // meshopt's triangle codec may rotate the corners of a triangle (the winding is kept): use the order it decodes to,
    // so the compressed and the plain documents hold the same indices.
    const std::string enc = encode_triangles(c.idx, Cube::kVerts);
    std::vector<u16> dec(c.idx.size());
    REQUIRE(meshopt_decodeIndexBuffer(dec.data(), dec.size(), 2, reinterpret_cast<const unsigned char*>(enc.data()), enc.size()) == 0);
    c.idx = dec;
    return c;
}

constexpr int kFloat = 5126, kByte = 5120, kUByte = 5121, kShort = 5122, kUShort = 5123, kUInt = 5125;

// Mesh with one primitive over the given accessors (-1 = absent), one node, one scene. `attrs_extra`:
// more attributes ("\"COLOR_0\":5"); `prim_extra` / `node_extra` / `root_extra`: more members after a leading comma.
std::string cube_rest(int ap, int an, int au, int ai, const std::string& node_extra = "", const std::string& root_extra = "",
                      const std::string& attrs_extra = "", const std::string& prim_extra = "", int prims = 1) {
    std::string attrs = "\"POSITION\":" + std::to_string(ap);
    if (an >= 0) attrs += ",\"NORMAL\":" + std::to_string(an);
    if (au >= 0) attrs += ",\"TEXCOORD_0\":" + std::to_string(au);
    if (!attrs_extra.empty()) attrs += "," + attrs_extra;
    std::string prim = "{\"attributes\":{" + attrs + "}";
    if (ai >= 0) prim += ",\"indices\":" + std::to_string(ai);
    prim += prim_extra + "}";
    std::string prim_list = prim;
    for (int i = 1; i < prims; ++i) prim_list += "," + prim;
    return "\"meshes\":[{\"primitives\":[" + prim_list + "]}],\"nodes\":[{\"name\":\"cube\",\"mesh\":0" + node_extra +
           "}],\"scenes\":[{\"nodes\":[0]}],\"scene\":0" + root_extra;
}

// ---- cooking and reading -----------------------------------------------------------------------------------

struct Cooked {
    bool ok = false;
    std::vector<byte> mesh;  // the CHNK_MESH payload
    std::vector<std::string> errors, warnings;

    bool has_error(const char* needle) const {
        for (const std::string& e : errors)
            if (e.find(needle) != std::string::npos) return true;
        return false;
    }
    int warning_count(const char* needle) const {
        int n = 0;
        for (const std::string& w : warnings) n += w.find(needle) != std::string::npos;
        return n;
    }
    std::string text() const {
        std::string s;
        for (const std::string& e : errors) s += "error: " + e + "\n";
        for (const std::string& w : warnings) s += "warning: " + w + "\n";
        return s;
    }
    MeshView view() const {
        auto mv = MeshView::read(Span<const byte>(mesh.data(), mesh.size()));
        REQUIRE(mv.has_value());
        return mv.value();
    }
};

Cooked cook_once(const std::string& glb, const char* settings = nullptr) {
    Cooked r;
    const auto importer = cook::make_gltf_importer();
    cook::JsonDoc sdoc;
    cook::ImportInput in;
    in.guid = Guid{1, 2};
    in.display_path = "assets/models/m.glb";
    in.source_path = "models/m.glb";
    in.source = Span<const byte>(reinterpret_cast<const byte*>(glb.data()), glb.size());
    if (settings) {
        cook::JsonError err;
        REQUIRE(sdoc.parse(settings, err));
        in.settings = sdoc.root();
    }
    cook::DiagList diags;
    ContainerWriter writer;
    const bool cooked = importer->cook(in, writer, diags);
    r.ok = cooked && diags.error_count() == 0;
    for (const cook::Diagnostic& d : diags.all()) (d.severity == cook::Severity::Error ? r.errors : r.warnings).push_back(d.message.c_str());
    if (r.ok) {
        Array<byte> packed;
        writer.finish(packed);
        auto cv = ContainerView::open(packed.span());
        REQUIRE(cv.has_value());
        const u32 chunk = cv.value().find(ChunkType::Mesh);
        REQUIRE(chunk < cv.value().chunk_count());
        const Span<const byte> p = cv.value().payload(chunk);
        r.mesh.assign(p.begin(), p.end());
    }
    return r;
}

// Cooks twice (determinism: identical bytes and diagnostics) and returns the first result.
Cooked cook_glb(const std::string& glb, const char* settings = nullptr) {
    Cooked a = cook_once(glb, settings);
    const Cooked b = cook_once(glb, settings);
    CHECK(a.ok == b.ok);
    CHECK(a.mesh == b.mesh);
    CHECK(a.errors == b.errors);
    CHECK(a.warnings == b.warnings);
    return a;
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

// The dequantized vertex positions of submesh 0 times `scale`, sorted, rounded to 1e-3.
std::vector<std::array<long, 3>> positions_of(const MeshView& m, float scale = 1.0f) {
    std::vector<std::array<long, 3>> out;
    const MeshSubmesh& sm = m.submeshes[0];
    for (u32 v = 0; v < sm.vertex_count; ++v) {
        const math::vec3 p = dequant(sm, m.vertices[sm.base_vertex + v]);
        out.push_back({std::lround(p.x * scale * 1000.0f), std::lround(p.y * scale * 1000.0f), std::lround(p.z * scale * 1000.0f)});
    }
    std::sort(out.begin(), out.end());
    return out;
}

// ---- the dense reference ------------------------------------------------------------------------------------

struct Dense {
    Doc d;
    int ap, an, au, ai;
};

Dense dense_cube() {
    const Cube c = make_cube();
    Dense r;
    const int vp = r.d.data_view(bytes_of(c.pos)), vn = r.d.data_view(bytes_of(c.nrm)), vu = r.d.data_view(bytes_of(c.uv)),
              vi = r.d.data_view(bytes_of(c.idx));
    r.ap = r.d.acc(vp, kFloat, Cube::kVerts, "VEC3");
    r.an = r.d.acc(vn, kFloat, Cube::kVerts, "VEC3");
    r.au = r.d.acc(vu, kFloat, Cube::kVerts, "VEC2");
    r.ai = r.d.acc(vi, kUShort, c.idx.size(), "SCALAR");
    return r;
}

Cooked dense_result() {
    const Dense r = dense_cube();
    return cook_glb(r.d.glb(cube_rest(r.ap, r.an, r.au, r.ai)));
}

std::string sparse_ext(int index_view, int ictype, size_t n, int value_view) {
    return "\"sparse\":{\"count\":" + std::to_string(n) + ",\"indices\":{\"bufferView\":" + std::to_string(index_view) +
           ",\"componentType\":" + std::to_string(ictype) + "},\"values\":{\"bufferView\":" + std::to_string(value_view) + "}}";
}

}  // namespace

// ---- the extension list ----------------------------------------------------------------------------------------

TEST_CASE("gltf ext: the material extensions, quantization and meshopt are supported, not 'ignored'") {
    const Dense r = dense_cube();
    const std::string ext =
        "[\"KHR_materials_clearcoat\",\"KHR_materials_sheen\",\"KHR_materials_anisotropy\",\"KHR_materials_transmission\","
        "\"KHR_materials_ior\",\"KHR_mesh_quantization\",\"EXT_meshopt_compression\"]";
    const Cooked res =
        cook_glb(r.d.glb(cube_rest(r.ap, r.an, r.au, r.ai, "", ",\"extensionsUsed\":" + ext + ",\"extensionsRequired\":" + ext)));
    INFO(res.text());
    CHECK(res.ok);
    CHECK(res.warning_count("ignored") == 0);
}

// ---- sparse accessors -------------------------------------------------------------------------------------------

TEST_CASE("gltf ext: sparse accessors cook byte for byte like the dense ones") {
    const Cube c = make_cube();
    const Cooked ref = dense_result();
    REQUIRE(ref.ok);

    Doc d;
    // POSITION: a base view with junk at vertices 3, 7, 11; u8 sparse indices restore them.
    std::vector<float> junk = c.pos;
    std::vector<float> fix;
    for (u32 v : {3u, 7u, 11u}) {
        for (u32 k = 0; k < 3; ++k) {
            junk[v * 3 + k] = 99.0f;
            fix.push_back(c.pos[v * 3 + k]);
        }
    }
    const int base = d.data_view(bytes_of(junk));
    const int pi = d.data_view(pack<u8>({3, 7, 11})), pv = d.data_view(bytes_of(fix));
    const int ap = d.acc(base, kFloat, Cube::kVerts, "VEC3", sparse_ext(pi, kUByte, 3, pv));
    // NORMAL: no base view (zeros), every element sparse with u32 indices.
    std::vector<u32> all;
    for (u32 v = 0; v < Cube::kVerts; ++v) all.push_back(v);
    const int ni = d.data_view(pack<u32>(all)), nv = d.data_view(bytes_of(c.nrm));
    const int an = d.acc(-1, kFloat, Cube::kVerts, "VEC3", sparse_ext(ni, kUInt, Cube::kVerts, nv));
    // TEXCOORD_0: u16 indices and the values at byte offsets of one shared view; the base has zeros at 0 and 23.
    std::vector<float> uv0 = c.uv;
    const std::vector<float> uvfix = {c.uv[0], c.uv[1], c.uv[46], c.uv[47]};
    uv0[0] = uv0[1] = uv0[46] = uv0[47] = 0.0f;
    const int ub = d.data_view(bytes_of(uv0));
    const std::string both = std::string(4, '\0') + pack<u16>({0, 23}) + std::string(4, '\0') + bytes_of(uvfix);
    const int shared = d.data_view(both);
    const int au = d.acc(ub, kFloat, Cube::kVerts, "VEC2",
                         "\"sparse\":{\"count\":2,\"indices\":{\"bufferView\":" + std::to_string(shared) +
                             ",\"byteOffset\":4,\"componentType\":5123},\"values\":{\"bufferView\":" + std::to_string(shared) +
                             ",\"byteOffset\":12}}");
    const int vi = d.data_view(bytes_of(c.idx));
    const int ai = d.acc(vi, kUShort, c.idx.size(), "SCALAR");

    const Cooked res = cook_glb(d.glb(cube_rest(ap, an, au, ai)));
    INFO(res.text());
    REQUIRE(res.ok);
    CHECK(res.mesh == ref.mesh);
}

TEST_CASE("gltf ext: sparse indices accessor, animation output and inverse bind matrices") {
    const Cube c = make_cube();
    {
        const Cooked ref = dense_result();
        Dense dn = dense_cube();
        std::vector<u32> pos;
        for (u32 i = 0; i < c.idx.size(); ++i) pos.push_back(i);
        const int pi = dn.d.data_view(pack<u16>(pos)), pv = dn.d.data_view(bytes_of(c.idx));
        dn.d.accs[static_cast<size_t>(dn.ai)] = "{\"componentType\":5123,\"count\":" + std::to_string(c.idx.size()) + ",\"type\":\"SCALAR\"," +
                                                sparse_ext(pi, kUShort, c.idx.size(), pv) + "}";
        const Cooked res = cook_glb(dn.d.glb(cube_rest(dn.ap, dn.an, dn.au, dn.ai)));
        INFO(res.text());
        REQUIRE(res.ok);
        CHECK(res.mesh == ref.mesh);
    }

    // A skinned triangle with 2 joints, a rotation animation and (dense / sparse) inverse binds and keys.
    auto skinned = [&](bool sparse) {
        Doc d;
        const std::vector<float> pos = {0, 0, 0, 1, 0, 0, 0, 1, 0};
        const std::vector<u8> joints = {0, 1, 0, 0, 0, 1, 0, 0, 1, 0, 0, 0};
        const std::vector<float> weights = {1, 0, 0, 0, 0.5f, 0.5f, 0, 0, 1, 0, 0, 0};
        const int ap = d.acc(d.data_view(bytes_of(pos)), kFloat, 3, "VEC3");
        const int aj = d.acc(d.data_view(bytes_of(joints)), kUByte, 3, "VEC4");
        const int aw = d.acc(d.data_view(bytes_of(weights)), kFloat, 3, "VEC4");
        std::vector<float> ibm(32, 0.0f);
        for (int j = 0; j < 2; ++j)
            for (int k = 0; k < 4; ++k) ibm[static_cast<size_t>(j) * 16 + static_cast<size_t>(k) * 5] = 1.0f;
        ibm[16 + 13] = -1.0f;  // joint 1: translate y by -1
        int aibm;
        if (!sparse) {
            aibm = d.acc(d.data_view(bytes_of(ibm)), kFloat, 2, "MAT4");
        } else {
            std::vector<float> base_ibm = ibm;
            base_ibm[16 + 13] = 5.0f;  // wrong in the base, fixed by the sparse entry
            const std::vector<float> fix(ibm.begin() + 16, ibm.end());
            const int bi = d.data_view(bytes_of(base_ibm)), si = d.data_view(pack<u8>({1})), sv = d.data_view(bytes_of(fix));
            aibm = d.acc(bi, kFloat, 2, "MAT4", sparse_ext(si, kUByte, 1, sv));
        }
        const std::vector<float> times = {0, 1, 2};
        const std::vector<float> rot = {0, 0, 0, 1, 0, 0.38268343f, 0, 0.92387953f, 0, 0.70710678f, 0, 0.70710678f};
        const int at = d.acc(d.data_view(bytes_of(times)), kFloat, 3, "SCALAR");
        int ar;
        if (!sparse) {
            ar = d.acc(d.data_view(bytes_of(rot)), kFloat, 3, "VEC4");
        } else {
            std::vector<float> base_rot = rot;
            for (size_t k = 4; k < 8; ++k) base_rot[k] = 0.0f;  // key 1 zeroed, restored by the sparse entry
            const std::vector<float> fix(rot.begin() + 4, rot.begin() + 8);
            const int bi = d.data_view(bytes_of(base_rot)), si = d.data_view(pack<u16>({1})), sv = d.data_view(bytes_of(fix));
            ar = d.acc(bi, kFloat, 3, "VEC4", sparse_ext(si, kUShort, 1, sv));
        }
        const std::string rest = "\"meshes\":[{\"primitives\":[{\"attributes\":{\"POSITION\":" + std::to_string(ap) +
                                 ",\"JOINTS_0\":" + std::to_string(aj) + ",\"WEIGHTS_0\":" + std::to_string(aw) +
                                 "}}]}],\"nodes\":[{\"name\":\"body\",\"mesh\":0,\"skin\":0},{\"name\":\"a\"},{\"name\":\"b\"}],"
                                 "\"skins\":[{\"joints\":[1,2],\"inverseBindMatrices\":" + std::to_string(aibm) +
                                 "}],\"scenes\":[{\"nodes\":[0,1,2]}],\"animations\":[{\"name\":\"turn\",\"samplers\":[{\"input\":" +
                                 std::to_string(at) + ",\"output\":" + std::to_string(ar) +
                                 "}],\"channels\":[{\"sampler\":0,\"target\":{\"node\":2,\"path\":\"rotation\"}}]}]";
        return cook_glb(d.glb(rest));
    };
    const Cooked dense = skinned(false), sparse = skinned(true);
    INFO(dense.text());
    INFO(sparse.text());
    REQUIRE(dense.ok);
    REQUIRE(sparse.ok);
    CHECK(dense.mesh == sparse.mesh);
    const MeshView m = dense.view();
    REQUIRE(m.clips.size() == 1);
    CHECK(m.keys[m.channels[0].values_offset + 4 + 1] == doctest::Approx(0.38268343f));
    CHECK(m.joints[1].inverse_bind[13] == doctest::Approx(-1.0f));
}

// ---- KHR_mesh_quantization ----------------------------------------------------------------------------------------

TEST_CASE("gltf ext: a KHR_mesh_quantization cube equals the float cube within one snorm16 step") {
    const Cube c = make_cube();
    const Cooked ref = dense_result();
    REQUIRE(ref.ok);
    const MeshView rm = ref.view();

    SUBCASE("normalized SHORT positions, normalized BYTE normals, plain SHORT texcoords") {
        Doc d;
        std::vector<i16> pos;
        pos.reserve(c.pos.size());
        for (float x : c.pos) pos.push_back(static_cast<i16>(std::lround(x * 2.0f * 32767.0f)));  // +-0.5 -> +-1.0, node scale 0.5
        std::vector<i8> nrm;
        nrm.reserve(c.nrm.size());
        for (float x : c.nrm) nrm.push_back(static_cast<i8>(std::lround(x * 127.0f)));
        std::vector<i16> uv;
        uv.reserve(c.uv.size());
        for (float x : c.uv) uv.push_back(static_cast<i16>(x));  // 0 or 1
        const int ap = d.acc(d.data_view(bytes_of(pos)), kShort, Cube::kVerts, "VEC3", "\"normalized\":true");
        const int an = d.acc(d.data_view(bytes_of(nrm)), kByte, Cube::kVerts, "VEC3", "\"normalized\":true");
        const int au = d.acc(d.data_view(bytes_of(uv)), kShort, Cube::kVerts, "VEC2");
        const int ai = d.acc(d.data_view(bytes_of(c.idx)), kUShort, c.idx.size(), "SCALAR");
        const std::string ext = "[\"KHR_mesh_quantization\"]";
        const Cooked res = cook_glb(d.glb(cube_rest(ap, an, au, ai, ",\"scale\":[0.5,0.5,0.5]",
                                                    ",\"extensionsUsed\":" + ext + ",\"extensionsRequired\":" + ext)));
        INFO(res.text());
        REQUIRE(res.ok);
        const MeshView m = res.view();
        CHECK(m.nodes[0].scale[0] == doctest::Approx(0.5f));
        REQUIRE(m.submeshes[0].vertex_count == rm.submeshes[0].vertex_count);
        const MeshSubmesh& sm = m.submeshes[0];
        const MeshSubmesh& rs = rm.submeshes[0];
        for (u32 v = 0; v < sm.vertex_count; ++v) {
            const math::vec3 a = dequant(sm, m.vertices[sm.base_vertex + v]);
            const math::vec3 b = dequant(rs, rm.vertices[rs.base_vertex + v]);
            // Quantized data are unit-box coordinates; the node's 0.5 scale makes them the float cube's.
            CHECK(std::fabs(a.x * 0.5f - b.x) <= 1.0f / 32767.0f * sm.extent[0] + 1e-6f);
            CHECK(std::fabs(a.y * 0.5f - b.y) <= 1.0f / 32767.0f * sm.extent[1] + 1e-6f);
            CHECK(std::fabs(a.z * 0.5f - b.z) <= 1.0f / 32767.0f * sm.extent[2] + 1e-6f);
            const math::vec3 na = oct_decode(m.vertices[sm.base_vertex + v]), nb = oct_decode(rm.vertices[rs.base_vertex + v]);
            CHECK(math::dot(na, nb) > 0.999f);
            CHECK(m.vertices[sm.base_vertex + v].uv[0] == rm.vertices[rs.base_vertex + v].uv[0]);
        }
    }
    SUBCASE("plain BYTE positions, normalized SHORT normals, normalized UNSIGNED_SHORT texcoords") {
        Doc d;
        std::vector<i8> pos;
        pos.reserve(c.pos.size());
        for (float x : c.pos) pos.push_back(static_cast<i8>(x * 200.0f));  // +-100, node scale 0.005
        std::vector<i16> nrm;
        nrm.reserve(c.nrm.size());
        for (float x : c.nrm) nrm.push_back(static_cast<i16>(std::lround(x * 32767.0f)));
        std::vector<u16> uv;
        uv.reserve(c.uv.size());
        for (float x : c.uv) uv.push_back(static_cast<u16>(x * 65535.0f));
        const int ap = d.acc(d.data_view(bytes_of(pos)), kByte, Cube::kVerts, "VEC3");
        const int an = d.acc(d.data_view(bytes_of(nrm)), kShort, Cube::kVerts, "VEC3", "\"normalized\":true");
        const int au = d.acc(d.data_view(bytes_of(uv)), kUShort, Cube::kVerts, "VEC2", "\"normalized\":true");
        const int ai = d.acc(d.data_view(bytes_of(c.idx)), kUShort, c.idx.size(), "SCALAR");
        const Cooked res = cook_glb(d.glb(cube_rest(ap, an, au, ai, ",\"scale\":[0.005,0.005,0.005]")));
        INFO(res.text());
        REQUIRE(res.ok);
        CHECK(positions_of(res.view(), 0.005f) == positions_of(rm));
    }
}

// ---- EXT_meshopt_compression --------------------------------------------------------------------------------------

namespace {

// The cube with interleaved position / normal / uv in one compressed ATTRIBUTES view and a TRIANGLES (or INDICES) view.
struct Packed {
    Doc d;
    std::string rest;
    std::string vertex_stream, index_stream;
};

// The document for the two given streams (counts as in the cube).
Cooked cook_streams(const std::string& vs, const std::string& is, const char* index_mode) {
    Doc d;
    const int vv = d.meshopt_view(vs, Cube::kVerts, 32, "ATTRIBUTES");
    const int iv = d.meshopt_view(is, 36, 2, index_mode);
    const int ap = d.acc(vv, kFloat, Cube::kVerts, "VEC3");
    const int an = d.acc(vv, kFloat, Cube::kVerts, "VEC3", "\"byteOffset\":12");
    const int au = d.acc(vv, kFloat, Cube::kVerts, "VEC2", "\"byteOffset\":24");
    const int ai = d.acc(iv, kUShort, 36, "SCALAR");
    return cook_once(d.glb(cube_rest(ap, an, au, ai)));
}

Packed meshopt_cube(const char* index_mode = "TRIANGLES") {
    const Cube c = make_cube();
    std::vector<float> inter;
    for (u32 v = 0; v < Cube::kVerts; ++v) {
        for (u32 k = 0; k < 3; ++k) inter.push_back(c.pos[v * 3 + k]);
        for (u32 k = 0; k < 3; ++k) inter.push_back(c.nrm[v * 3 + k]);
        for (u32 k = 0; k < 2; ++k) inter.push_back(c.uv[v * 2 + k]);
    }
    Packed p;
    p.vertex_stream = encode_vertices(inter.data(), Cube::kVerts, 32);
    p.index_stream = std::string(index_mode) == "TRIANGLES" ? encode_triangles(c.idx, Cube::kVerts) : encode_sequence(c.idx, Cube::kVerts);
    const int vv = p.d.meshopt_view(p.vertex_stream, Cube::kVerts, 32, "ATTRIBUTES");
    const int iv = p.d.meshopt_view(p.index_stream, c.idx.size(), 2, index_mode);
    const int ap = p.d.acc(vv, kFloat, Cube::kVerts, "VEC3");
    const int an = p.d.acc(vv, kFloat, Cube::kVerts, "VEC3", "\"byteOffset\":12");
    const int au = p.d.acc(vv, kFloat, Cube::kVerts, "VEC2", "\"byteOffset\":24");
    const int ai = p.d.acc(iv, kUShort, c.idx.size(), "SCALAR");
    const std::string ext = "[\"EXT_meshopt_compression\"]";
    p.rest = cube_rest(ap, an, au, ai, "", ",\"extensionsUsed\":" + ext + ",\"extensionsRequired\":" + ext);
    return p;
}

}  // namespace

TEST_CASE("gltf ext: a meshopt-compressed cube cooks byte for byte like the uncompressed one; the fallback buffer is ignored") {
    const Cooked ref = dense_result();
    REQUIRE(ref.ok);
    for (const char* mode : {"TRIANGLES", "INDICES"}) {
        const Packed p = meshopt_cube(mode);
        const Cooked res = cook_glb(p.d.glb(p.rest));
        INFO(mode << "\n" << res.text());
        REQUIRE(res.ok);
        CHECK(res.mesh == ref.mesh);
        // Smaller than the uncompressed buffers, so the test really went through the decoders.
        CHECK(p.vertex_stream.size() < Cube::kVerts * 32);
    }
}

TEST_CASE("gltf ext: meshopt filters: EXPONENTIAL positions, OCTAHEDRAL normals, QUATERNION rotations") {
    const Cube c = make_cube();
    const Cooked ref = dense_result();
    REQUIRE(ref.ok);
    const MeshView rm = ref.view();

    Doc d;
    // Positions: exponential filter, 15-bit mantissas (+-0.5 is exact).
    std::vector<unsigned char> filtered(Cube::kVerts * 12);
    meshopt_encodeFilterExp(filtered.data(), Cube::kVerts, 12, 15, c.pos.data(), meshopt_EncodeExpSeparate);
    const std::string pos_stream = encode_vertices(filtered.data(), Cube::kVerts, 12);
    const int pv = d.meshopt_view(pos_stream, Cube::kVerts, 12, "ATTRIBUTES", "EXPONENTIAL");
    // Normals: octahedral filter, 8 bit (stride 4: x y z + w), read as a normalized BYTE VEC3 with a stride of 4.
    std::vector<float> n4;
    for (u32 v = 0; v < Cube::kVerts; ++v) {
        for (u32 k = 0; k < 3; ++k) n4.push_back(c.nrm[v * 3 + k]);
        n4.push_back(0.0f);
    }
    std::vector<unsigned char> oct(Cube::kVerts * 4);
    meshopt_encodeFilterOct(oct.data(), Cube::kVerts, 4, 8, n4.data());
    const std::string nrm_stream = encode_vertices(oct.data(), Cube::kVerts, 4);
    const int nv = d.meshopt_view(nrm_stream, Cube::kVerts, 4, "ATTRIBUTES", "OCTAHEDRAL");
    const int uv = d.data_view(bytes_of(c.uv));
    const std::string idx_stream = encode_triangles(c.idx, Cube::kVerts);
    const int iv = d.meshopt_view(idx_stream, c.idx.size(), 2, "TRIANGLES");
    const int ap = d.acc(pv, kFloat, Cube::kVerts, "VEC3");
    const int an = d.acc(nv, kByte, Cube::kVerts, "VEC3", "\"normalized\":true");
    const int au = d.acc(uv, kFloat, Cube::kVerts, "VEC2");
    const int ai = d.acc(iv, kUShort, c.idx.size(), "SCALAR");

    // A rotation animation: two quaternions through the quaternion filter (16 bit, stride 8, SHORT normalized).
    const std::vector<float> times = {0, 1};
    const float q[8] = {0, 0, 0, 1, 0, 0.70710678f, 0, 0.70710678f};
    std::vector<unsigned char> qf(2 * 8);
    meshopt_encodeFilterQuat(qf.data(), 2, 8, 16, q);
    const std::string q_stream = encode_vertices(qf.data(), 2, 8);
    const int qv = d.meshopt_view(q_stream, 2, 8, "ATTRIBUTES", "QUATERNION");
    const int at = d.acc(d.data_view(bytes_of(times)), kFloat, 2, "SCALAR");
    const int ar = d.acc(qv, kShort, 2, "VEC4", "\"normalized\":true");
    const std::string anim = ",\"animations\":[{\"samplers\":[{\"input\":" + std::to_string(at) + ",\"output\":" + std::to_string(ar) +
                             "}],\"channels\":[{\"sampler\":0,\"target\":{\"node\":0,\"path\":\"rotation\"}}]}]";
    const Cooked res = cook_glb(d.glb(cube_rest(ap, an, au, ai, "", anim)));
    INFO(res.text());
    REQUIRE(res.ok);
    const MeshView m = res.view();
    CHECK(positions_of(m) == positions_of(rm));
    const MeshSubmesh& sm = m.submeshes[0];
    const MeshSubmesh& rs = rm.submeshes[0];
    REQUIRE(sm.vertex_count == rs.vertex_count);
    for (u32 v = 0; v < sm.vertex_count; ++v)
        CHECK(math::dot(oct_decode(m.vertices[sm.base_vertex + v]), oct_decode(rm.vertices[rs.base_vertex + v])) > 0.99f);
    REQUIRE(m.clips.size() == 1);
    const MeshChannel& ch = m.channels[m.clips[0].first_channel];
    CHECK(ch.key_count == 2);
    for (u32 k = 0; k < 4; ++k) CHECK(m.keys[ch.values_offset + 4 + k] == doctest::Approx(q[4 + k]).epsilon(1e-3));
}

// ---- Draco -----------------------------------------------------------------------------------------------------

TEST_CASE("gltf ext: KHR_draco_mesh_compression: refused when required, the fallback is read when only used") {
    const Dense r = dense_cube();
    const std::string prim_draco = ",\"extensions\":{\"KHR_draco_mesh_compression\":{\"bufferView\":0,\"attributes\":{\"POSITION\":0}}}";
    SUBCASE("required") {
        const Cooked res = cook_once(r.d.glb(cube_rest(r.ap, r.an, r.au, r.ai, "",
                                                       ",\"extensionsUsed\":[\"KHR_draco_mesh_compression\"],"
                                                       "\"extensionsRequired\":[\"KHR_draco_mesh_compression\"]",
                                                       "", prim_draco)));
        INFO(res.text());
        CHECK(!res.ok);
        CHECK(res.has_error("/extensionsRequired/0: required extension 'KHR_draco_mesh_compression'"));
        CHECK(res.has_error("gltfpack -c"));
        CHECK(res.has_error("gltf-transform meshopt"));
    }
    SUBCASE("used, with the uncompressed fallback") {
        const Cooked ref = dense_result();
        const Cooked res =
            cook_glb(r.d.glb(cube_rest(r.ap, r.an, r.au, r.ai, "", ",\"extensionsUsed\":[\"KHR_draco_mesh_compression\"]", "", prim_draco)));
        INFO(res.text());
        REQUIRE(res.ok);
        CHECK(res.mesh == ref.mesh);
        CHECK(res.warning_count("KHR_draco_mesh_compression") == 1);
    }
    SUBCASE("used, but the exporter left the fallback out") {
        Doc d;
        const int ap = d.acc(-1, kFloat, Cube::kVerts, "VEC3");
        const Cooked res = cook_once(d.glb(cube_rest(ap, -1, -1, -1, "", ",\"extensionsUsed\":[\"KHR_draco_mesh_compression\"]", "", prim_draco)));
        INFO(res.text());
        CHECK(!res.ok);
        CHECK(res.has_error("/meshes/0/primitives/0: the primitive is Draco-compressed and has no uncompressed fallback"));
        CHECK(res.has_error("gltfpack -c"));
    }
}

// ---- JOINTS_1 / WEIGHTS_1 ------------------------------------------------------------------------------------------

namespace {

// Three vertices of a triangle, 8 joints (nodes 1..8) and two influence sets per vertex.
Cooked eight_influences() {
    Doc d;
    const std::vector<float> pos = {0, 0, 0, 1, 0, 0, 0, 1, 0};
    const std::vector<u8> j0 = {0, 1, 2, 3, 1, 2, 0, 0, 0, 1, 2, 3};
    const std::vector<u8> j1 = {4, 5, 6, 7, 0, 0, 0, 0, 4, 0, 0, 0};
    const std::vector<float> w0 = {0.30f, 0.05f, 0.10f, 0.02f, 0.5f, 0.5f, 0, 0, 0.4f, 0.3f, 0.2f, 0.095f};
    const std::vector<float> w1 = {0.25f, 0.15f, 0.08f, 0.05f, 0, 0, 0, 0, 0.005f, 0, 0, 0};
    const int ap = d.acc(d.data_view(bytes_of(pos)), kFloat, 3, "VEC3");
    const int aj0 = d.acc(d.data_view(bytes_of(j0)), kUByte, 3, "VEC4");
    const int aj1 = d.acc(d.data_view(bytes_of(j1)), kUByte, 3, "VEC4");
    const int aw0 = d.acc(d.data_view(bytes_of(w0)), kFloat, 3, "VEC4");
    const int aw1 = d.acc(d.data_view(bytes_of(w1)), kFloat, 3, "VEC4");
    std::string nodes = "{\"name\":\"body\",\"mesh\":0,\"skin\":0}";
    std::string joints, scene = "0";
    for (int i = 1; i <= 8; ++i) {
        nodes += ",{\"name\":\"j" + std::to_string(i - 1) + "\"}";
        joints += (i > 1 ? "," : "") + std::to_string(i);
        scene += "," + std::to_string(i);
    }
    const std::string rest = "\"meshes\":[{\"primitives\":[{\"attributes\":{\"POSITION\":" + std::to_string(ap) +
                             ",\"JOINTS_0\":" + std::to_string(aj0) + ",\"WEIGHTS_0\":" + std::to_string(aw0) +
                             ",\"JOINTS_1\":" + std::to_string(aj1) + ",\"WEIGHTS_1\":" + std::to_string(aw1) +
                             "}}]}],\"nodes\":[" + nodes + "],\"skins\":[{\"joints\":[" + joints + "]}],\"scenes\":[{\"nodes\":[" + scene + "]}]";
    return cook_glb(d.glb(rest));
}

}  // namespace

TEST_CASE("gltf ext: eight influences reduce to the four largest, renormalized, with a lost-weight warning") {
    const Cooked res = eight_influences();
    INFO(res.text());
    REQUIRE(res.ok);
    // Exactly one vertex (the first) loses more than 1% of its weight (20%); the third loses 0.5%.
    CHECK(res.warning_count("1 of 3 vertices lost more than 1% of their skin weight") == 1);
    const MeshView m = res.view();
    REQUIRE(m.skinned());
    const MeshSubmesh& sm = m.submeshes[0];
    REQUIRE(sm.vertex_count == 3);
    int checked = 0;
    for (u32 v = 0; v < 3; ++v) {
        const math::vec3 p = dequant(sm, m.vertices[sm.base_vertex + v]);
        const MeshSkinVertex& sv = m.skin_vertices[sm.base_vertex + v];
        const int w_sum = sv.weights[0] + sv.weights[1] + sv.weights[2] + sv.weights[3];
        CHECK(w_sum == 255);
        auto weight_of = [&](u8 joint) {
            int w = 0;
            for (int k = 0; k < 4; ++k)
                if (sv.joints[k] == joint && sv.weights[k] > 0) w += sv.weights[k];
            return static_cast<float>(w);
        };
        if (std::fabs(p.x) < 1e-3f && std::fabs(p.y) < 1e-3f) {  // vertex 0: j0 .30, j2 .10, j4 .25, j5 .15 -> / 0.80
            CHECK(std::fabs(weight_of(0) - 0.375f * 255.0f) <= 1.01f);
            CHECK(std::fabs(weight_of(2) - 0.125f * 255.0f) <= 1.01f);
            CHECK(std::fabs(weight_of(4) - 0.3125f * 255.0f) <= 1.01f);
            CHECK(std::fabs(weight_of(5) - 0.1875f * 255.0f) <= 1.01f);
            CHECK(weight_of(1) == 0.0f);
            CHECK(weight_of(3) == 0.0f);
            CHECK(weight_of(6) == 0.0f);
            CHECK(weight_of(7) == 0.0f);
            ++checked;
        } else if (p.x > 0.5f) {  // vertex 1: only set 0, 50/50
            CHECK(std::fabs(weight_of(1) - 127.5f) <= 1.01f);
            CHECK(std::fabs(weight_of(2) - 127.5f) <= 1.01f);
            ++checked;
        } else {  // vertex 2: the four set-0 influences (set 1's 0.005 is the fifth), 0.5% lost
            CHECK(weight_of(4) == 0.0f);
            CHECK(weight_of(0) > weight_of(1));
            ++checked;
        }
    }
    CHECK(checked == 3);
}

// ---- morph targets ---------------------------------------------------------------------------------------------------

TEST_CASE("gltf ext: default morph weights are baked into POSITION; the targets are dropped with a warning") {
    auto build = [&](const char* mesh_weights, const char* node_weights, bool sparse_delta) {
        Doc d;
        const std::vector<float> pos = {0, 0, 0, 1, 0, 0, 0, 1, 0};
        const int ap = d.acc(d.data_view(bytes_of(pos)), kFloat, 3, "VEC3");
        int ad;
        if (!sparse_delta) {
            const std::vector<float> delta = {0, 1, 0, 0, 1, 0, 0, 1, 0};
            ad = d.acc(d.data_view(bytes_of(delta)), kFloat, 3, "VEC3");
        } else {  // only vertex 0 moves
            const std::vector<float> val = {0, 1, 0};
            const int si = d.data_view(pack<u8>({0})), sv = d.data_view(bytes_of(val));
            ad = d.acc(-1, kFloat, 3, "VEC3", sparse_ext(si, kUByte, 1, sv));
        }
        std::string rest = "\"meshes\":[{\"primitives\":[{\"attributes\":{\"POSITION\":" + std::to_string(ap) +
                           "},\"targets\":[{\"POSITION\":" + std::to_string(ad) + "}]}]";
        if (mesh_weights) rest += std::string(",\"weights\":") + mesh_weights;
        rest += "}],\"nodes\":[{\"mesh\":0";
        if (node_weights) rest += std::string(",\"weights\":") + node_weights;
        rest += "}],\"scenes\":[{\"nodes\":[0]}]";
        return cook_glb(d.glb(rest));
    };
    SUBCASE("mesh weights") {
        const Cooked res = build("[0.5]", nullptr, false);
        INFO(res.text());
        REQUIRE(res.ok);
        CHECK(res.warning_count("1 morph target is not imported") == 1);
        CHECK(res.warning_count("baked into POSITION/NORMAL") == 1);
        const MeshView m = res.view();
        CHECK(m.header->bounds_max[1] == doctest::Approx(1.5f).epsilon(1e-3));
        CHECK(m.header->bounds_min[1] == doctest::Approx(0.5f).epsilon(1e-3));
    }
    SUBCASE("node weights override the mesh's") {
        const Cooked res = build("[0.5]", "[1.0]", false);
        REQUIRE(res.ok);
        CHECK(res.view().header->bounds_max[1] == doctest::Approx(2.0f).epsilon(1e-3));
    }
    SUBCASE("sparse deltas") {
        const Cooked res = build("[1.0]", nullptr, true);
        INFO(res.text());
        REQUIRE(res.ok);
        const MeshView m = res.view();
        // (0,0,0) moved to (0,1,0): the triangle is now (0,1,0), (1,0,0), (0,1,0).
        CHECK(m.header->bounds_max[0] == doctest::Approx(1.0f).epsilon(1e-3));
        CHECK(m.header->bounds_max[1] == doctest::Approx(1.0f).epsilon(1e-3));
        CHECK(m.header->bounds_min[1] == doctest::Approx(0.0f).epsilon(1e-3));
        CHECK(m.header->bounds_min[0] == doctest::Approx(0.0f).epsilon(1e-3));
    }
    SUBCASE("zero weights leave the rest pose and still warn with the target count") {
        const Cooked res = build("[0]", nullptr, false);
        REQUIRE(res.ok);
        CHECK(res.warning_count("1 morph target is not imported (static import)") == 1);
        CHECK(res.warning_count("baked") == 0);
        CHECK(res.view().header->bounds_max[1] == doctest::Approx(1.0f).epsilon(1e-3));
    }
}

// ---- dropped features warn once ----------------------------------------------------------------------------------------

TEST_CASE("gltf ext: vertex colors, TEXCOORD_1, cameras and lights each warn once per file with a pointer") {
    Dense r = dense_cube();
    const std::vector<float> col(Cube::kVerts * 4, 1.0f);
    const int ac = r.d.acc(r.d.data_view(bytes_of(col)), kFloat, Cube::kVerts, "VEC4");
    const std::string attrs = "\"COLOR_0\":" + std::to_string(ac) + ",\"TEXCOORD_1\":" + std::to_string(r.au) + ",\"TEXCOORD_2\":" +
                              std::to_string(r.au);
    const std::string root =
        ",\"cameras\":[{\"type\":\"perspective\",\"perspective\":{\"yfov\":1.0,\"znear\":0.1}}],"
        "\"extensionsUsed\":[\"KHR_lights_punctual\"],\"extensions\":{\"KHR_lights_punctual\":{\"lights\":[{\"type\":\"point\"}]}}";
    const std::string node = ",\"camera\":0,\"extensions\":{\"KHR_lights_punctual\":{\"light\":0}}";
    const Cooked res = cook_glb(r.d.glb(cube_rest(r.ap, r.an, r.au, r.ai, node, root, attrs, "", 2)));
    INFO(res.text());
    REQUIRE(res.ok);
    CHECK(res.warning_count("vertex colors are not imported (first: /meshes/0/primitives/0/attributes/COLOR_0)") == 1);
    CHECK(res.warning_count("TEXCOORD_1 is dropped (first: /meshes/0/primitives/0/attributes/TEXCOORD_1)") == 1);
    CHECK(res.warning_count("TEXCOORD_2") == 0);
    CHECK(res.warning_count("cameras are not imported (first: /nodes/0/camera)") == 1);
    CHECK(res.warning_count("KHR_lights_punctual lights are not imported (first: /nodes/0/extensions/KHR_lights_punctual)") == 1);
    CHECK(res.warning_count("ignored") == 0);
    // The geometry itself is the plain cube twice.
    const Cooked ref = dense_result();
    CHECK(res.view().submeshes.size() == 2);
    CHECK(positions_of(res.view()) == positions_of(ref.view()));
}

// ---- untrusted input -----------------------------------------------------------------------------------------------------

namespace {

// A cube whose POSITION is sparse over a zero base; `indices` as given (type `ictype`), `declared` values claimed.
Cooked sparse_position_case(const std::vector<u32>& indices, int ictype, double declared, double accessor_count = Cube::kVerts) {
    const Cube c = make_cube();
    Doc d;
    std::string idx_bytes;
    switch (ictype) {
        case kUByte: idx_bytes = pack<u8>(indices); break;
        case kUShort: idx_bytes = pack<u16>(indices); break;
        default: idx_bytes = pack<u32>(indices); break;
    }
    std::vector<float> vals;
    for (size_t i = 0; i < indices.size(); ++i)
        for (size_t k = 0; k < 3; ++k) vals.push_back(c.pos[(i % Cube::kVerts) * 3 + k]);
    const int si = d.data_view(idx_bytes), sv = d.data_view(bytes_of(vals));
    char count[64], acount[64];
    std::snprintf(count, sizeof(count), "%.0f", declared);
    std::snprintf(acount, sizeof(acount), "%.0f", accessor_count);
    d.accs.push_back("{\"componentType\":5126,\"count\":" + std::string(acount) + ",\"type\":\"VEC3\",\"sparse\":{\"count\":" + count +
                     ",\"indices\":{\"bufferView\":" + std::to_string(si) + ",\"componentType\":" + std::to_string(ictype) +
                     "},\"values\":{\"bufferView\":" + std::to_string(sv) + "}}}");
    const int an = d.acc(d.data_view(bytes_of(c.nrm)), kFloat, Cube::kVerts, "VEC3");
    const int ai = d.acc(d.data_view(bytes_of(c.idx)), kUShort, c.idx.size(), "SCALAR");
    return cook_once(d.glb(cube_rest(0, an, -1, ai)));
}

}  // namespace

TEST_CASE("gltf ext: corrupt sparse accessors fail with the accessor's pointer") {
    std::vector<u32> all;
    for (u32 v = 0; v < Cube::kVerts; ++v) all.push_back(v);
    SUBCASE("a valid sparse position cooks (the cases below break exactly one thing)") {
        const Cooked res = sparse_position_case(all, kUByte, 24);
        INFO(res.text());
        CHECK(res.ok);
    }
    SUBCASE("index out of range") {
        std::vector<u32> bad = all;
        bad.back() = 24;
        const Cooked res = sparse_position_case(bad, kUByte, 24);
        CHECK(!res.ok);
        CHECK(res.has_error("/accessors/0/sparse/indices: sparse index 24 at position 23 is out of range"));
    }
    SUBCASE("index out of range, u32") {
        const Cooked res = sparse_position_case({0, 4294967295u}, kUInt, 2);
        CHECK(res.has_error("/accessors/0/sparse/indices: sparse index 4294967295 at position 1 is out of range"));
    }
    SUBCASE("unsorted indices") {
        const Cooked res = sparse_position_case({5, 3}, kUByte, 2);
        CHECK(!res.ok);
        CHECK(res.has_error("/accessors/0/sparse/indices: sparse indices must be strictly increasing"));
    }
    SUBCASE("repeated index") {
        const Cooked res = sparse_position_case({3, 3}, kUShort, 2);
        CHECK(res.has_error("/accessors/0/sparse/indices: sparse indices must be strictly increasing"));
    }
    SUBCASE("count above the accessor's count") {
        const Cooked res = sparse_position_case(all, kUByte, 25);
        CHECK(res.has_error("/accessors/0/sparse: sparse count must be an integer in 1..the accessor's count"));
    }
    SUBCASE("count zero or negative") {
        CHECK(sparse_position_case(all, kUByte, 0).has_error("/accessors/0/sparse: sparse count"));
        CHECK(sparse_position_case(all, kUByte, -3).has_error("/accessors/0/sparse: sparse count"));
    }
    SUBCASE("huge count with too little data") {
        const Cooked res = sparse_position_case(all, kUByte, 9007199254740992.0, 9007199254740992.0);
        CHECK(!res.ok);
        CHECK(res.has_error("/accessors/0"));
    }
    SUBCASE("count claims more indices than the view holds") {
        const Cooked res = sparse_position_case({0, 1, 2}, kUByte, 24);
        CHECK(res.has_error("/accessors/0/sparse/indices: sparse indices exceed their buffer view"));
    }
    SUBCASE("values view too short") {
        Doc d;
        const int si = d.data_view(pack<u8>(all));
        const std::vector<float> vals(9, 0.0f);
        const int sv = d.data_view(bytes_of(vals));
        const int ap = d.acc(-1, kFloat, Cube::kVerts, "VEC3", sparse_ext(si, kUByte, 24, sv));
        const Cooked res = cook_once(d.glb(cube_rest(ap, -1, -1, -1)));
        CHECK(res.has_error("/accessors/0/sparse/values: sparse values exceed their buffer view"));
    }
    SUBCASE("bad index component type") {
        const Cooked res = sparse_position_case({0}, kFloat, 1);
        CHECK(res.has_error("/accessors/0/sparse/indices: sparse index componentType"));
    }
    SUBCASE("view index out of range") {
        Doc d;
        const int ap = d.acc(-1, kFloat, 3, "VEC3", sparse_ext(7, kUByte, 1, 8));
        const Cooked res = cook_once(d.glb(cube_rest(ap, -1, -1, -1)));
        CHECK(res.has_error("/accessors/0/sparse/indices: bufferView index out of range"));
    }
    SUBCASE("sparse is not an object") {
        Doc d;
        const int ap = d.acc(-1, kFloat, 3, "VEC3", "\"sparse\":[1]");
        CHECK(cook_once(d.glb(cube_rest(ap, -1, -1, -1))).has_error("/accessors/0/sparse: sparse must be an object"));
    }
    SUBCASE("sparse over absurd counts") {
        const Cooked res = sparse_position_case({0}, kUByte, 1, 4294967295.0);
        CHECK(res.has_error("/accessors/0: count 4294967295 is over the importer's limit"));
    }
}

TEST_CASE("gltf ext: absurd counts, strides and offsets fail without allocating") {
    const Cube c = make_cube();
    auto cook_acc = [&](double count, const std::string& acc_extra = "", const std::string& view_extra = "", double view_off = 0,
                        double view_len = -1) {
        Doc d;
        const std::string data = bytes_of(c.pos);
        d.bin = data;
        char buf[256];
        std::snprintf(buf, sizeof(buf), "{\"buffer\":0,\"byteOffset\":%.0f,\"byteLength\":%.0f%s}", view_off,
                      view_len < 0 ? static_cast<double>(data.size()) : view_len, view_extra.c_str());
        d.views.push_back(buf);
        char cnt[400];
        std::snprintf(cnt, sizeof(cnt), "%.0f", count);
        d.accs.push_back("{\"bufferView\":0,\"componentType\":5126,\"count\":" + std::string(cnt) + ",\"type\":\"VEC3\"" +
                         (acc_extra.empty() ? "" : "," + acc_extra) + "}");
        return cook_once(d.glb(cube_rest(0, -1, -1, -1)));
    };
    CHECK(cook_acc(24).ok);
    CHECK(cook_acc(25).has_error("/accessors/0: accessor range exceeds its buffer view"));
    CHECK(cook_acc(4294967295.0).has_error("/accessors/0"));
    CHECK(cook_acc(9007199254740992.0).has_error("/accessors/0: count"));
    CHECK(cook_acc(16777217.0).has_error("is over the importer's limit"));
    CHECK(cook_acc(1e300).has_error("/accessors/0: count must be a positive integer"));
    CHECK(cook_acc(0).has_error("/accessors/0: count must be a positive integer"));
    CHECK(cook_acc(-1).has_error("/accessors/0: count must be a positive integer"));
    // count * byteStride overflows 64 bits; byteStride / byteOffset at the limits of what JSON can say exactly.
    CHECK(cook_acc(24, "", ",\"byteStride\":9007199254740992").has_error("/accessors/0: accessor range exceeds"));
    CHECK(cook_acc(16777216, "", ",\"byteStride\":9007199254740992").has_error("/accessors/0"));
    CHECK(cook_acc(24, "\"byteOffset\":9007199254740992").has_error("/accessors/0: accessor range exceeds"));
    CHECK(cook_acc(24, "", "", 9007199254740992.0).has_error("the bufferView runs past its buffer"));
    CHECK(cook_acc(24, "", "", 0, 9007199254740992.0).has_error("the bufferView runs past its buffer"));
    CHECK(cook_acc(24, "", ",\"byteStride\":4").has_error("/accessors/0: accessor range exceeds"));  // stride below the element size
    CHECK(cook_acc(24, "", "", 4, 284).has_error("/accessors/0: accessor range exceeds"));          // offset leaves too few bytes
    CHECK(cook_acc(24, "", "", -4).has_error("bufferView byteOffset, byteLength and byteStride must be non-negative integers"));
    // An accessor without a view is all zeros: its count is capped instead of allocated.
    {
        Doc d;
        const int ap = d.acc(-1, kFloat, 16777216, "VEC3");
        CHECK(cook_once(d.glb(cube_rest(ap, -1, -1, -1))).has_error("/accessors/0: count 16777216 is over the importer's limit"));
    }
    // A buffer claiming more bytes than the file has.
    {
        Doc d;
        d.data_view(bytes_of(c.pos));
        d.acc(0, kFloat, 24, "VEC3");
        std::string j = d.json(cube_rest(0, -1, -1, -1));
        const size_t at = j.find("\"buffers\":[{\"byteLength\":") + 25;
        j.replace(at, j.find('}', at) - at, "9007199254740992");
        CHECK(cook_once(make_glb(j, d.bin)).has_error("/buffers/0: buffer is shorter than its byteLength"));
    }
}

TEST_CASE("gltf ext: deep JSON nesting, node cycles and morph target floods are errors") {
    const Dense r = dense_cube();
    SUBCASE("1000 nested arrays") {
        std::string deep(1000, '[');
        deep += std::string(1000, ']');
        const Cooked res = cook_once(r.d.glb(cube_rest(r.ap, r.an, r.au, r.ai, "", ",\"extras\":" + deep)));
        CHECK(!res.ok);
        CHECK(res.has_error("JSON parse error"));
    }
    SUBCASE("a node that is its own child") {
        const Cooked res = cook_once(r.d.glb(cube_rest(r.ap, r.an, r.au, r.ai, ",\"children\":[0]")));
        CHECK(!res.ok);
        CHECK(res.has_error("node is reachable twice"));
    }
    SUBCASE("two nodes that are each other's children") {
        const std::string rest = "\"meshes\":[{\"primitives\":[{\"attributes\":{\"POSITION\":" + std::to_string(r.ap) +
                                 "}}]}],\"nodes\":[{\"mesh\":0,\"children\":[1]},{\"children\":[0]}],\"scenes\":[{\"nodes\":[0,1]}]";
        CHECK(cook_once(r.d.glb(rest)).has_error("node is reachable twice"));
    }
    SUBCASE("600 morph targets") {
        std::string targets;
        for (int i = 0; i < 600; ++i) targets += (i ? "," : "") + std::string("{\"POSITION\":") + std::to_string(r.ap) + "}";
        const Cooked res = cook_once(r.d.glb(cube_rest(r.ap, r.an, r.au, r.ai, "", "", "", ",\"targets\":[" + targets + "]")));
        CHECK(res.has_error("/meshes/0/primitives/0: 600 morph targets is over the importer's limit"));
    }
    SUBCASE("a joint index outside the skin, in the second set") {
        Doc d;
        const std::vector<float> pos = {0, 0, 0, 1, 0, 0, 0, 1, 0};
        const std::vector<u8> j0 = {0, 0, 0, 0, 0, 0, 0, 0, 0, 0, 0, 0}, j1 = {9, 0, 0, 0, 0, 0, 0, 0, 0, 0, 0, 0};
        const std::vector<float> w0(12, 0.5f), w1(12, 0.5f);
        const int ap = d.acc(d.data_view(bytes_of(pos)), kFloat, 3, "VEC3");
        const int a0 = d.acc(d.data_view(bytes_of(j0)), kUByte, 3, "VEC4"), a1 = d.acc(d.data_view(bytes_of(j1)), kUByte, 3, "VEC4");
        const int b0 = d.acc(d.data_view(bytes_of(w0)), kFloat, 3, "VEC4"), b1 = d.acc(d.data_view(bytes_of(w1)), kFloat, 3, "VEC4");
        const std::string rest = "\"meshes\":[{\"primitives\":[{\"attributes\":{\"POSITION\":" + std::to_string(ap) +
                                 ",\"JOINTS_0\":" + std::to_string(a0) + ",\"WEIGHTS_0\":" + std::to_string(b0) + ",\"JOINTS_1\":" +
                                 std::to_string(a1) + ",\"WEIGHTS_1\":" + std::to_string(b1) +
                                 "}}]}],\"nodes\":[{\"mesh\":0,\"skin\":0},{}],\"skins\":[{\"joints\":[1]}],\"scenes\":[{\"nodes\":[0,1]}]";
        const Cooked res = cook_once(d.glb(rest));
        CHECK(res.has_error("JOINTS_1 names a joint outside the node's skin"));
    }
}

TEST_CASE("gltf ext: malformed EXT_meshopt_compression views fail with the view's pointer") {
    const Packed good = meshopt_cube();
    REQUIRE(cook_once(good.d.glb(good.rest)).ok);
    const std::string ptr = "/bufferViews/0/extensions/EXT_meshopt_compression";
    // Rewrites one member of the first view's extension object.
    auto mutated = [&](const std::string& from, const std::string& to) {
        std::string json = good.d.json(good.rest);
        const size_t ext = json.find("EXT_meshopt_compression\":{\"buffer\":0");
        REQUIRE(ext != std::string::npos);
        const size_t at = json.find(from, ext);
        REQUIRE(at != std::string::npos);
        json.replace(at, from.size(), to);
        return cook_once(make_glb(json, good.d.bin));
    };
    const std::string stride = "\"byteStride\":32";
    const std::string count = "\"count\":24";
    CHECK(mutated(stride, "\"byteStride\":30").has_error((ptr + ": byteStride must be a multiple of 4").c_str()));
    CHECK(mutated(stride, "\"byteStride\":0").has_error("byteStride must be a multiple of 4"));
    CHECK(mutated(stride, "\"byteStride\":260").has_error("byteStride must be a multiple of 4"));
    CHECK(mutated("\"mode\":\"ATTRIBUTES\"", "\"mode\":\"TRIANGLES\"").has_error("byteStride must be 2 or 4"));
    CHECK(mutated("\"mode\":\"ATTRIBUTES\"", "\"mode\":\"BOGUS\"").has_error("mode must be ATTRIBUTES, TRIANGLES or INDICES"));
    CHECK(mutated("\"filter\":\"NONE\"", "\"filter\":\"QUATERNION\"").has_error("filter does not fit the mode or byteStride"));
    CHECK(mutated("\"filter\":\"NONE\"", "\"filter\":\"OCTAHEDRAL\"").has_error("filter does not fit the mode or byteStride"));
    CHECK(mutated("\"filter\":\"NONE\"", "\"filter\":\"SEPIA\"").has_error("filter must be NONE"));
    CHECK(mutated(count, "\"count\":0").has_error("count and byteLength must be positive"));
    CHECK(mutated(count, "\"count\":24.5").has_error("must be non-negative integers"));
    CHECK(mutated(count, "\"count\":9007199254740992").has_error("decoded bytes"));
    CHECK(mutated(count, "\"count\":1000000").has_error("is over the importer's limit"));
    CHECK(mutated("\"buffer\":0", "\"buffer\":5").has_error("buffer index out of range"));
    CHECK(mutated("\"buffer\":0,\"byteOffset\"", "\"buffer\":1,\"byteOffset\"").has_error("compressed range exceeds its buffer"));
    CHECK(mutated("\"byteOffset\":4,\"byteLength\":" + std::to_string(good.vertex_stream.size()),  // the first stream follows Doc's 4 pad bytes
                  "\"byteOffset\":9007199254740992,\"byteLength\":" + std::to_string(good.vertex_stream.size()))
              .has_error("compressed range exceeds its buffer"));
    // A TRIANGLES view whose count is not a multiple of 3.
    {
        std::string json = good.d.json(good.rest);
        const size_t ext = json.find("\"mode\":\"TRIANGLES\"");
        REQUIRE(ext != std::string::npos);
        const size_t cat = json.rfind("\"count\":36", ext);
        REQUIRE(cat != std::string::npos);
        json.replace(cat, 10, "\"count\":35");
        CHECK(cook_once(make_glb(json, good.d.bin)).has_error("TRIANGLES count must be a multiple of 3"));
    }
    // Fewer vertices declared than the stream holds: the decoder rejects the stream or the accessors run out of view.
    CHECK(!mutated("\"count\":24", "\"count\":12").ok);
}

TEST_CASE("gltf ext: truncated and flipped meshopt streams are errors or valid meshes, never crashes") {
    // Truncation: every strictly shorter prefix of either stream is an error.
    for (const char* mode : {"TRIANGLES", "INDICES"}) {
        const Packed good = meshopt_cube(mode);
        const std::string v = good.vertex_stream, ix = good.index_stream;
        REQUIRE(cook_streams(v, ix, mode).ok);
        int vertex_ok = 0, index_ok = 0;
        for (size_t n = 0; n < v.size(); ++n) vertex_ok += cook_streams(v.substr(0, n), ix, mode).ok;
        for (size_t n = 0; n < ix.size(); ++n) index_ok += cook_streams(v, ix.substr(0, n), mode).ok;
        CHECK(vertex_ok == 0);
        CHECK(index_ok == 0);
        // Trailing garbage: accepted or rejected, but no crash.
        (void)cook_streams(v + std::string(7, '\x55'), ix, mode);
        (void)cook_streams(v, ix + std::string(7, '\x55'), mode);
    }
    // Bit flips and byte overwrites with a fixed generator: an error, or a mesh the validating reader accepts.
    u64 rng = 0x9E3779B97F4A7C15ull;
    auto next = [&]() {
        rng ^= rng << 13;
        rng ^= rng >> 7;
        rng ^= rng << 17;
        return rng;
    };
    int errors = 0, accepted = 0;
    for (const char* mode : {"TRIANGLES", "INDICES"}) {
        const Packed good = meshopt_cube(mode);
        for (int iter = 0; iter < 400; ++iter) {
            std::string v = good.vertex_stream, ix = good.index_stream;
            std::string& target = (iter & 1) ? ix : v;
            const u64 flips = 1 + next() % 3;
            for (u64 f = 0; f < flips; ++f) {
                const size_t at = next() % target.size();
                if (next() & 1) target[at] = static_cast<char>(static_cast<unsigned char>(target[at]) ^ (1u << (next() % 8)));
                else target[at] = static_cast<char>(next());
            }
            const Cooked res = cook_streams(v, ix, mode);
            if (res.ok) {
                ++accepted;
                CHECK(res.view().submeshes.size() == 1);  // MeshView::read validates every index and range
            } else {
                ++errors;
                CHECK(!res.errors.empty());
            }
        }
    }
    INFO("accepted " << accepted << ", rejected " << errors);
    CHECK(errors > 0);
    // The vertex stream header byte (0xa0 | version): only version 0 is allowed by EXT_meshopt_compression (a v1 stream
    // can compress constant attributes ~1000x, past any sane ratio bound), so v1 and unknown versions are named errors.
    {
        const Packed good = meshopt_cube();
        REQUIRE(static_cast<unsigned char>(good.vertex_stream[0]) == 0xA0);
        for (const int header : {0xA1, 0xA7, 0xB0, 0x00}) {
            std::string v = good.vertex_stream;
            v[0] = static_cast<char>(header);
            const Cooked res = cook_streams(v, good.index_stream, "TRIANGLES");
            INFO(res.text());
            CHECK(!res.ok);
            CHECK(res.has_error("/bufferViews/0/extensions/EXT_meshopt_compression: unsupported meshopt stream header"));
            CHECK(res.has_error("version 0 vertex streams"));
        }
        // An index stream header with a wrong kind or a version above the codec's 1.
        std::string ix = good.index_stream;
        ix[0] = static_cast<char>(0xE2);
        CHECK(cook_streams(good.vertex_stream, ix, "TRIANGLES").has_error("/bufferViews/1/extensions/EXT_meshopt_compression: unsupported meshopt stream header"));
        ix[0] = static_cast<char>(0xD0);
        CHECK(cook_streams(good.vertex_stream, ix, "TRIANGLES").has_error("unsupported meshopt stream header"));
    }
    // A 3-byte stream, and the same claiming a 960 MB output.
    {
        Doc d;
        const int vv = d.meshopt_view("\xa0\x01\x02", 24, 32, "ATTRIBUTES");
        const int ap = d.acc(vv, kFloat, Cube::kVerts, "VEC3");
        CHECK(cook_once(d.glb(cube_rest(ap, -1, -1, -1))).has_error("meshopt stream is corrupt or truncated"));
        Doc q;
        const int qv = q.meshopt_view("\xa0\x01\x02", 30000000, 32, "ATTRIBUTES");
        const int qp = q.acc(qv, kFloat, Cube::kVerts, "VEC3");
        CHECK(cook_once(q.glb(cube_rest(qp, -1, -1, -1))).has_error("is over the importer's limit"));
    }
}

// ---- budgets, hardening and nits (review round) ---------------------------------------------------------------------

TEST_CASE("gltf ext: a morph-bake flood is refused by the work budget before any delta is read") {
    // Millions of vertices (no bufferView: zeros) and 512 targets of weight 1 would be ~1024 reads of 15M+ floats (11M: 0.6 s to refuse).
    Doc d;
    const int ap = d.acc(-1, kFloat, 5000000, "VEC3");
    std::string targets, weights;
    for (int i = 0; i < 512; ++i) {
        targets += (i ? "," : "") + std::string("{\"POSITION\":") + std::to_string(ap) + "}";
        weights += (i ? "," : "") + std::string("1");
    }
    const std::string glb = d.glb(cube_rest(ap, -1, -1, -1, ",\"weights\":[" + weights + "]", "", "", ",\"targets\":[" + targets + "]"));
    // CPU time, not wall-clock: a loaded CI box stretches the wall time of a 0.6 s refusal past any tight limit, but not the CPU time.
    // Reading the deltas first would be ~1024 passes over 15M floats (tens of CPU seconds), so the generous limit still catches it.
    const std::clock_t c0 = std::clock();
    const Cooked res = cook_once(glb);
    const double secs = static_cast<double>(std::clock() - c0) / CLOCKS_PER_SEC;
    INFO(res.text());
    INFO("cpu seconds: " << secs);
    CHECK(!res.ok);
    CHECK(res.has_error("/meshes/0/primitives/0: baking the default morph weights is over the importer's budget"));
    CHECK(secs < 5.0);
}

TEST_CASE("gltf ext: the morph warning is once per file, not per instance") {
    Doc d;
    const std::vector<float> pos = {0, 0, 0, 1, 0, 0, 0, 1, 0}, delta = {0, 1, 0, 0, 1, 0, 0, 1, 0};
    const int ap = d.acc(d.data_view(bytes_of(pos)), kFloat, 3, "VEC3");
    const int ad = d.acc(d.data_view(bytes_of(delta)), kFloat, 3, "VEC3");
    const std::string rest = "\"meshes\":[{\"primitives\":[{\"attributes\":{\"POSITION\":" + std::to_string(ap) +
                             "},\"targets\":[{\"POSITION\":" + std::to_string(ad) + "}]}],\"weights\":[0.5]}],"
                             "\"nodes\":[{\"mesh\":0},{\"mesh\":0},{\"mesh\":0}],\"scenes\":[{\"nodes\":[0,1,2]}]";
    const Cooked res = cook_glb(d.glb(rest));
    INFO(res.text());
    REQUIRE(res.ok);
    CHECK(res.warning_count("morph target") == 1);
    CHECK(res.warning_count("(first: /meshes/0/primitives/0)") == 1);
}

TEST_CASE("gltf ext: JOINTS_n / WEIGHTS_n beyond the 8 supported sets is an error, whatever n") {
    for (const char* name : {"JOINTS_8", "JOINTS_11", "JOINTS_12", "WEIGHTS_15", "JOINTS_100"}) {
        Doc d;
        const std::vector<float> pos = {0, 0, 0, 1, 0, 0, 0, 1, 0};
        const std::vector<u8> j0(12, 0);
        const std::vector<float> w0 = {1, 0, 0, 0, 1, 0, 0, 0, 1, 0, 0, 0};
        const int ap = d.acc(d.data_view(bytes_of(pos)), kFloat, 3, "VEC3");
        const int aj = d.acc(d.data_view(bytes_of(j0)), kUByte, 3, "VEC4");
        const int aw = d.acc(d.data_view(bytes_of(w0)), kFloat, 3, "VEC4");
        const std::string rest = "\"meshes\":[{\"primitives\":[{\"attributes\":{\"POSITION\":" + std::to_string(ap) +
                                 ",\"JOINTS_0\":" + std::to_string(aj) + ",\"WEIGHTS_0\":" + std::to_string(aw) + ",\"" + name + "\":" +
                                 std::to_string(aj) + "}}]}],\"nodes\":[{\"mesh\":0,\"skin\":0},{}],\"skins\":[{\"joints\":[1]}],"
                                 "\"scenes\":[{\"nodes\":[0,1]}]";
        const Cooked res = cook_once(d.glb(rest));
        INFO(name << "\n" << res.text());
        CHECK(!res.ok);
        CHECK(res.has_error((std::string(name) + ": more than 8 JOINTS_n / WEIGHTS_n sets").c_str()));
    }
}

TEST_CASE("gltf ext: Draco without a fallback is detected on every attribute accessor, not just POSITION") {
    Doc d;
    const Cube c = make_cube();
    const int ap = d.acc(d.data_view(bytes_of(c.pos)), kFloat, Cube::kVerts, "VEC3");
    const int an = d.acc(-1, kFloat, Cube::kVerts, "VEC3");  // stripped placeholder
    const std::string prim_draco = ",\"extensions\":{\"KHR_draco_mesh_compression\":{\"bufferView\":0,\"attributes\":{\"POSITION\":0}}}";
    const Cooked res = cook_once(d.glb(cube_rest(ap, an, -1, -1, "", ",\"extensionsUsed\":[\"KHR_draco_mesh_compression\"]", "", prim_draco)));
    INFO(res.text());
    CHECK(!res.ok);
    CHECK(res.has_error("/meshes/0/primitives/0: the primitive is Draco-compressed and has no uncompressed fallback"));
}

// ---- type confusion along the indices chain ---------------------------------------------------------------------------

namespace {

// The indexed cube as JSON text where every value on the indices chain can be swapped for `repl` (the member at JSON
// path `target`, e.g. "accessors.1.componentType"); `seen` collects every path the document offers. A default of ""
// means "absent unless mutated".
struct MutDoc {
    std::string target, repl;
    std::vector<std::string> seen;

    std::string val(const std::string& path, const std::string& def) {
        seen.push_back(path);
        return path == target ? repl : def;
    }
    std::string obj(const std::string& path, const std::vector<std::pair<std::string, std::string>>& fields) {
        std::string s = "{";
        bool first = true;
        for (const auto& [key, def] : fields) {
            std::string sub = path;
            sub += '.';
            sub += key;
            const std::string v = val(sub, def);
            if (v.empty()) continue;
            if (!first) s += ',';
            s += '"';
            s += key;
            s += "\":";
            s += v;
            first = false;
        }
        return val(path, s + "}");
    }
};

// ictype: componentType of the indices (5121 / 5123 / 5125). The indices accessor is sparse (one substituted element).
std::string indices_chain_glb(MutDoc& m, int ictype) {
    const Cube c = make_cube();
    const size_t isz = ictype == kUInt ? 4 : ictype == kUShort ? 2 : 1;
    std::string bin = bytes_of(c.pos);  // view 0: positions
    const size_t ioff = bin.size();
    std::vector<u32> wide(c.idx.begin(), c.idx.end());
    if (ictype == kUByte)  // a cube with 24 vertices does not fit u8 indices: use the first 8 vertices' triangles
        for (u32& x : wide) x %= 8;
    const std::string idx_bytes = ictype == kUInt ? pack<u32>(wide) : ictype == kUShort ? pack<u16>(wide) : pack<u8>(wide);
    bin += idx_bytes;  // view 1: indices
    while (bin.size() % 4) bin.push_back('\0');
    const size_t soff = bin.size();
    bin += std::string(1, '\0');  // view 2: sparse index {0}
    while (bin.size() % 4) bin.push_back('\0');
    const size_t voff = bin.size();
    bin += idx_bytes.substr(0, isz);  // view 3: the sparse value (the same element 0)
    while (bin.size() % 4) bin.push_back('\0');
    auto num = [](size_t n) { return std::to_string(n); };

    const std::string sparse =
        m.obj("accessors.1.sparse",
              {{"count", "1"},
               {"indices", m.obj("accessors.1.sparse.indices", {{"bufferView", "2"}, {"byteOffset", "0"}, {"componentType", "5121"}})},
               {"values", m.obj("accessors.1.sparse.values", {{"bufferView", "3"}, {"byteOffset", "0"}})}});
    const std::string acc0 = m.obj("accessors.0", {{"bufferView", "0"}, {"byteOffset", "0"}, {"componentType", "5126"},
                                                   {"count", num(Cube::kVerts)}, {"type", "\"VEC3\""}});
    const std::string acc1 = m.obj("accessors.1", {{"bufferView", "1"}, {"byteOffset", "0"}, {"componentType", num(static_cast<size_t>(ictype))},
                                                   {"count", num(wide.size())}, {"type", "\"SCALAR\""}, {"normalized", "false"},
                                                   {"sparse", sparse}});
    const std::string bv0 = m.obj("bufferViews.0", {{"buffer", "0"}, {"byteOffset", "0"}, {"byteLength", num(ioff)}, {"byteStride", "12"}});
    const std::string bv1 = m.obj("bufferViews.1", {{"buffer", "0"}, {"byteOffset", num(ioff)}, {"byteLength", num(idx_bytes.size())}, {"byteStride", ""}});
    const std::string bv2 = m.obj("bufferViews.2", {{"buffer", "0"}, {"byteOffset", num(soff)}, {"byteLength", "1"}});
    const std::string bv3 = m.obj("bufferViews.3", {{"buffer", "0"}, {"byteOffset", num(voff)}, {"byteLength", num(isz)}});
    const std::string buf0 = m.obj("buffers.0", {{"byteLength", num(bin.size())}, {"uri", ""}});
    const std::string attrs = m.obj("meshes.0.primitives.0.attributes", {{"POSITION", "0"}});
    const std::string prim = m.obj("meshes.0.primitives.0", {{"attributes", attrs}, {"indices", "1"}});
    const std::string json =
        "{\"asset\":{\"version\":\"2.0\"},\"meshes\":[{\"primitives\":[" + prim + "]}],\"nodes\":[{\"mesh\":0}],\"scenes\":[{\"nodes\":[0]}],"
        "\"scene\":0,\"buffers\":" + m.val("buffers", "[" + buf0 + "]") + ",\"bufferViews\":" +
        m.val("bufferViews", "[" + bv0 + "," + bv1 + "," + bv2 + "," + bv3 + "]") + ",\"accessors\":" +
        m.val("accessors", "[" + acc0 + "," + acc1 + "]") + "}";
    return make_glb(json, bin);
}

}  // namespace

TEST_CASE("gltf ext: every wrongly typed value along the indices chain is an error or a mesh, never an assert") {
    const char* const kJunk[] = {"\"str\"", "{}", "[]", "true", "null", "-1", "1.5", "1e300"};
    for (const int ictype : {kUInt, kUShort, kUByte}) {
        MutDoc base;
        const std::string good = indices_chain_glb(base, ictype);
        const Cooked ref = cook_once(good);
        INFO("ictype " << ictype << "\n" << ref.text());
        REQUIRE(ref.ok);
        // The paths include accessors.1.{bufferView,byteOffset,componentType,count,type,normalized,sparse...}, the
        // bufferViews and buffers members, primitive.indices and the containers themselves.
        const std::vector<std::string> paths = base.seen;
        CHECK(paths.size() > 30);
        int cooked = 0, rejected = 0;
        for (const std::string& path : paths)
            for (const char* junk : kJunk) {
                MutDoc m;
                m.target = path;
                m.repl = junk;
                const Cooked res = cook_once(indices_chain_glb(m, ictype));
                INFO("ictype " << ictype << ", " << path << " = " << junk << "\n" << res.text());
                CHECK((res.ok || !res.errors.empty()));
                (res.ok ? cooked : rejected)++;
            }
        CHECK(rejected > 100);
        CHECK(cooked > 0);  // e.g. normalized: false -> an absent-equivalent value is still a cube
    }
}
