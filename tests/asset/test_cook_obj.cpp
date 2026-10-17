// Wavefront OBJ (+ MTL) import (M-IMPORT3D step 2): a quad cube cooks to the bytes of the same cube written as glTF, n-gons
// fan, negative indices, MTL materials and textures resolve (a missing .mtl is a warning, a missing texture an error naming
// the .mtl line), the sidecar's up / scale / lod_levels / max_triangles work, cooks are deterministic whatever the line
// ends, and corrupt input (truncations, absurd indices and numbers, binary garbage, huge counts and lines, seeded
// mutations) errors or succeeds but never crashes.

#include <doctest/doctest.h>

#include <algorithm>
#include <cmath>
#include <cstring>
#include <string>
#include <vector>

#include "cook_test_util.h"
#include "mce/asset/asset_system.h"
#include "mce/cook/cook.h"
#include "obj_importer.h"

using namespace mce;
using namespace mce::asset;
using namespace cook_test;
using cook::CookOptions;
using cook::CookReport;
using cook::CookService;

namespace {

constexpr const char* kTexGuid = "6a1d9c58-0f4b-4b8e-9d1b-1c2a3b4c5e01";
constexpr const char* kObjGuid = "6a1d9c58-0f4b-4b8e-9d1b-1c2a3b4c5e02";
constexpr const char* kGltfGuid = "6a1d9c58-0f4b-4b8e-9d1b-1c2a3b4c5e03";
constexpr const char* kMtlGuid = "6a1d9c58-0f4b-4b8e-9d1b-1c2a3b4c5e04";

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

// ---- the cube: 8 positions, 4 texture coordinates, 6 normals, 6 quads (counter-clockwise from outside) ----

const int kFaceVerts[6][4] = {{1, 3, 7, 5}, {0, 4, 6, 2}, {2, 6, 7, 3}, {0, 1, 5, 4}, {4, 5, 7, 6}, {0, 2, 3, 1}};
const int kFaceNormal[6][3] = {{1, 0, 0}, {-1, 0, 0}, {0, 1, 0}, {0, -1, 0}, {0, 0, 1}, {0, 0, -1}};
const float kUv[4][2] = {{0, 0}, {1, 0}, {1, 1}, {0, 1}};

const char* half(int bit) { return bit ? "0.5" : "-0.5"; }
std::string num(int v) { return std::to_string(v); }

// The cube as OBJ text. `z_up` writes the same cube with +Z up (the sidecar says "up": "z"); `negative` uses relative indices.
std::string cube_obj(const std::string& eol = "\n", bool negative = false, bool z_up = false, const std::string& header = "o Cube") {
    std::string s;
    auto line = [&](const std::string& l) {
        s += l;
        s += eol;
    };
    line("# unit cube");
    line(header);
    for (int i = 0; i < 8; ++i) {
        const char *x = half(i & 1), *y = half(i & 2), *z = half(i & 4);
        // +Z-up file coordinates (x, -z, y) become the engine's (x, y, z).
        if (z_up) line(std::string("v ") + x + " " + (i & 4 ? "-0.5" : "0.5") + " " + y);
        else line(std::string("v ") + x + " " + y + " " + z);
    }
    for (const auto& uv : kUv) line("vt " + num(int(uv[0])) + " " + num(int(uv[1])));
    for (const auto& n : kFaceNormal) line(z_up ? "vn " + num(n[0]) + " " + num(n[2] == 0 ? 0 : -n[2]) + " " + num(n[1]) : "vn " + num(n[0]) + " " + num(n[1]) + " " + num(n[2]));
    for (int f = 0; f < 6; ++f) {
        std::string l = "f";
        for (int k = 0; k < 4; ++k) {
            const int v = kFaceVerts[f][k] + 1, t = k + 1, n = f + 1;
            l += negative ? " " + num(v - 9) + "/" + num(t - 5) + "/" + num(n - 7) : " " + num(v) + "/" + num(t) + "/" + num(n);
        }
        line(l);
    }
    return s;
}

// The same cube as glTF: 24 vertices, the fan indices of the OBJ, one node "Cube", the OBJ importer's default material.
std::string cube_gltf() {
    std::string bin;
    for (int f = 0; f < 6; ++f)
        for (int k = 0; k < 4; ++k) {
            const int v = kFaceVerts[f][k];
            put_f32(bin, (v & 1) ? 0.5f : -0.5f);
            put_f32(bin, (v & 2) ? 0.5f : -0.5f);
            put_f32(bin, (v & 4) ? 0.5f : -0.5f);
        }
    for (int f = 0; f < 6; ++f)
        for (int k = 0; k < 4; ++k)
            for (int c = 0; c < 3; ++c) put_f32(bin, float(kFaceNormal[f][c]));
    for (int f = 0; f < 6; ++f)
        for (int k = 0; k < 4; ++k) {
            put_f32(bin, kUv[k][0]);
            put_f32(bin, 1.0f - kUv[k][1]);  // OBJ's V runs up, glTF's down
        }
    for (int f = 0; f < 6; ++f)
        for (int i : {0, 1, 2, 0, 2, 3}) put16le(bin, static_cast<u16>(f * 4 + i));
    const std::string b64 = base64(bin);
    auto view = [](size_t off, size_t len) {
        return "{\"buffer\":0,\"byteOffset\":" + std::to_string(off) + ",\"byteLength\":" + std::to_string(len) + "}";
    };
    std::string s = "{\"asset\":{\"version\":\"2.0\"},";
    s += "\"buffers\":[{\"byteLength\":" + std::to_string(bin.size()) + ",\"uri\":\"data:application/octet-stream;base64," + b64 + "\"}],";
    s += "\"bufferViews\":[" + view(0, 288) + "," + view(288, 288) + "," + view(576, 192) + "," + view(768, 72) + "],";
    s += "\"accessors\":[{\"bufferView\":0,\"componentType\":5126,\"count\":24,\"type\":\"VEC3\"},"
         "{\"bufferView\":1,\"componentType\":5126,\"count\":24,\"type\":\"VEC3\"},"
         "{\"bufferView\":2,\"componentType\":5126,\"count\":24,\"type\":\"VEC2\"},"
         "{\"bufferView\":3,\"componentType\":5123,\"count\":36,\"type\":\"SCALAR\"}],";
    s += "\"materials\":[{\"pbrMetallicRoughness\":{\"baseColorFactor\":[1,1,1,1],\"metallicFactor\":0,\"roughnessFactor\":1}}],";
    s += "\"meshes\":[{\"primitives\":[{\"attributes\":{\"POSITION\":0,\"NORMAL\":1,\"TEXCOORD_0\":2},\"indices\":3,\"material\":0}]}],";
    s += "\"nodes\":[{\"name\":\"Cube\",\"mesh\":0}],\"scenes\":[{\"nodes\":[0]}],\"scene\":0}";
    return s;
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

MeshView view_of(const std::vector<byte>& bytes) {
    auto mv = MeshView::read(Span<const byte>(bytes.data(), bytes.size()));
    REQUIRE(mv.has_value());
    return mv.value();
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

// A project with one .obj (plus anything `extra` writes), cooked once. Returns the cooked bytes of the OBJ asset.
struct Cooked {
    std::string root;
    CookReport report;
    std::string text;
    std::vector<byte> mesh;
};

Cooked cook_obj(const char* name, const std::string& obj, const char* settings = "{}") {
    Cooked c;
    c.root = fresh_dir(name);
    write_file(c.root + "/assets/models/m.obj", obj);
    write_file(c.root + "/assets/models/m.obj.mceasset", sidecar(kObjGuid, "obj", settings));
    CookService svc;
    c.report = svc.cook(c.root.c_str(), CookOptions{});
    c.text = diag_text(c.report);
    if (c.report.ok()) c.mesh = cooked_mesh(c.root, kObjGuid);
    return c;
}

// ---- direct importer calls (corrupt-input tests): no project, no registry ----

struct Direct {
    bool ok = false;
    std::string diags;
    std::vector<byte> bytes;
};

Direct cook_direct(const cook::IImporter& imp, const std::string& text, const char* settings = nullptr) {
    Direct d;
    cook::JsonDoc doc;
    cook::JsonError je;
    cook::ImportInput in;
    in.guid = G(kObjGuid);
    in.display_path = imp.id() == "mtl" ? "assets/m.mtl" : "assets/m.obj";
    in.source_path = imp.id() == "mtl" ? "m.mtl" : "m.obj";
    in.source = Span<const byte>(reinterpret_cast<const byte*>(text.data()), text.size());
    if (settings) {
        REQUIRE(doc.parse(StringView(settings), je));
        in.settings = doc.root();
    }
    ContainerWriter w;
    cook::DiagList diags;
    d.ok = imp.cook(in, w, diags);
    for (const auto& x : diags.all()) {
        cook::String line;
        cook::format_diagnostic(x, line);
        d.diags += line.c_str();
        d.diags += "\n";
    }
    if (d.ok) {
        Array<byte> out;
        w.finish(out);
        d.bytes.assign(out.begin(), out.end());
    }
    return d;
}

Direct obj_direct(const std::string& text, const char* settings = nullptr) {
    static const mem::UniquePtr<cook::IImporter> imp = cook::make_obj_importer();
    return cook_direct(*imp, text, settings);
}

Direct mtl_direct(const std::string& text) {
    static const mem::UniquePtr<cook::IImporter> imp = cook::make_mtl_importer();
    return cook_direct(*imp, text);
}


// Index of the first byte where a and b differ (or their common size when one is longer), -1 when equal: a failing
// comparison prints this instead of the vectors.
long first_diff(const std::vector<byte>& a, const std::vector<byte>& b) {
    const size_t n = std::min(a.size(), b.size());
    for (size_t i = 0; i < n; ++i)
        if (a[i] != b[i]) return static_cast<long>(i);
    return a.size() == b.size() ? -1 : static_cast<long>(n);
}

#define CHECK_SAME_BYTES(a, b)                                        \
    do {                                                              \
        INFO("sizes " << (a).size() << " and " << (b).size());        \
        CHECK(first_diff((a), (b)) == -1);                            \
    } while (0)

bool has(const std::string& hay, const char* needle) { return hay.find(needle) != std::string::npos; }

const char* kTri = "v 0 0 0\nv 1 0 0\nv 0 1 0\n";

// xorshift64*: the fuzz loops are deterministic.
struct Rng {
    u64 s;
    u64 next() {
        s ^= s >> 12;
        s ^= s << 25;
        s ^= s >> 27;
        return s * 0x2545F4914F6CDD1Dull;
    }
    size_t below(size_t n) { return n ? static_cast<size_t>(next() % n) : 0; }
};

}  // namespace

TEST_CASE("obj: a quad cube cooks to the bytes of the same cube as glTF") {
    const std::string root = fresh_dir("obj_vs_gltf");
    write_file(root + "/assets/models/cube.obj", cube_obj());
    write_file(root + "/assets/models/cube.obj.mceasset", sidecar(kObjGuid, "obj"));
    write_file(root + "/assets/models/cube.gltf", cube_gltf());
    write_file(root + "/assets/models/cube.gltf.mceasset", sidecar(kGltfGuid, "gltf"));
    CookService svc;
    const CookReport r = svc.cook(root.c_str(), CookOptions{});
    INFO(diag_text(r));
    REQUIRE(r.ok());
    CHECK(r.cooked == 2);
    const std::vector<byte> a = cooked_mesh(root, kObjGuid);
    const std::vector<byte> b = cooked_mesh(root, kGltfGuid);
    CHECK_SAME_BYTES(a, b);

    const MeshView m = view_of(a);
    REQUIRE(m.nodes.size() == 1);
    CHECK(m.node_name(0) == "Cube");
    REQUIRE(m.submeshes.size() == 1);
    CHECK(m.submeshes[0].vertex_count == 24);
    CHECK(m.submeshes[0].index_count == 36);
}

TEST_CASE("obj: negative indices, CRLF, CR and line continuation give the same bytes") {
    const Cooked base = cook_obj("obj_lf", cube_obj());
    INFO(base.text);
    REQUIRE(base.report.ok());
    const Cooked neg = cook_obj("obj_neg", cube_obj("\n", true));
    REQUIRE(neg.report.ok());
    CHECK_SAME_BYTES(neg.mesh, base.mesh);
    const Cooked crlf = cook_obj("obj_crlf", cube_obj("\r\n"));
    REQUIRE(crlf.report.ok());
    CHECK_SAME_BYTES(crlf.mesh, base.mesh);
    const Cooked cr = cook_obj("obj_cr", cube_obj("\r"));
    REQUIRE(cr.report.ok());
    CHECK_SAME_BYTES(cr.mesh, base.mesh);

    // A face split over two lines with a backslash is one face.
    std::string joined = cube_obj();
    const size_t at = joined.find("f 2/1/1 ");
    REQUIRE(at != std::string::npos);
    joined.insert(joined.find(' ', at + 8), " \\\n");
    const Cooked cont = cook_obj("obj_cont", joined);
    INFO(cont.text);
    REQUIRE(cont.report.ok());
    CHECK_SAME_BYTES(cont.mesh, base.mesh);

    // Cooked twice: identical (and --force changes nothing).
    const Cooked again = cook_obj("obj_lf_again", cube_obj());
    CHECK_SAME_BYTES(again.mesh, base.mesh);
    CookService svc;
    CookOptions force;
    force.force = true;
    REQUIRE(svc.cook(base.root.c_str(), force).ok());
    CHECK_SAME_BYTES(cooked_mesh(base.root, kObjGuid), base.mesh);
}

TEST_CASE("obj: n-gons are fan-triangulated") {
    const std::string verts = "v 0 0 0\nv 1 0 0\nv 1.5 1 0\nv 0.5 1.5 0\nv -0.5 1 0\n";
    const Cooked ngon = cook_obj("obj_ngon", verts + "f 1 2 3 4 5\n");
    INFO(ngon.text);
    REQUIRE(ngon.report.ok());
    const Cooked manual = cook_obj("obj_ngon_manual", verts + "f 1 2 3\nf 1 3 4\nf 1 4 5\n");
    REQUIRE(manual.report.ok());
    CHECK_SAME_BYTES(ngon.mesh, manual.mesh);
    const MeshView m = view_of(ngon.mesh);
    REQUIRE(m.submeshes.size() == 1);
    CHECK(m.submeshes[0].index_count == 9);
    CHECK(m.submeshes[0].vertex_count == 5);
    // No vn: the generated normal of a counter-clockwise face in the XY plane is +Z.
    for (u32 v = 0; v < 5; ++v) CHECK(oct_decode(m.vertices[m.submeshes[0].base_vertex + v]).z == doctest::Approx(1.0f).epsilon(1e-3));
}

TEST_CASE("obj: o / g make nodes, usemtl makes submeshes, up and scale come from the sidecar") {
    const std::string text = std::string("g left\nv 0 0 0\nv 1 0 0\nv 0 1 0\nf 1 2 3\ng right\nv 2 0 0\nv 3 0 0\nv 2 1 0\nf 4 5 6\n") +
                             "g empty\ng third\nusemtl a\nf 1 2 3\nusemtl b\nf 4 5 6\nusemtl a\nf 1 3 2\n";
    const Cooked c = cook_obj("obj_nodes", text, "{ \"scale\": 0.5, \"up\": \"z\" }");
    INFO(c.text);
    REQUIRE(c.report.ok());
    CHECK(has(c.text, "is not defined by any mtllib"));  // no mtllib: the materials are reported as default
    const MeshView m = view_of(c.mesh);
    REQUIRE(m.nodes.size() == 3);  // "empty" holds no face
    CHECK(m.node_name(0) == "left");
    CHECK(m.node_name(1) == "right");
    CHECK(m.node_name(2) == "third");
    for (u32 i = 0; i < 3; ++i) {
        CHECK(m.nodes[i].parent == -1);
        CHECK(m.nodes[i].scale[0] == doctest::Approx(0.5f));
    }
    REQUIRE(m.submeshes.size() == 4);  // left, right, third/a (two faces), third/b
    CHECK(m.submeshes[2].index_count == 6);
    CHECK(m.submeshes[3].index_count == 3);
    // +Z up: the triangle in the file's XY plane, facing the file's +Z, faces the engine's +Y.
    const math::vec3 n = oct_decode(m.vertices[m.submeshes[0].base_vertex]);
    CHECK(n.y == doctest::Approx(1.0f).epsilon(1e-3));

    // "up": "z" on a cube written +Z up equals the +Y-up cube.
    const Cooked y_up = cook_obj("obj_up_y", cube_obj());
    const Cooked z_up = cook_obj("obj_up_z", cube_obj("\n", false, true), "{ \"up\": \"z\" }");
    INFO(z_up.text);
    REQUIRE(z_up.report.ok());
    CHECK_SAME_BYTES(z_up.mesh, y_up.mesh);
    const Cooked bad_up = cook_obj("obj_up_bad", cube_obj(), "{ \"up\": \"x\" }");
    CHECK(!bad_up.report.ok());
    CHECK(has(bad_up.text, "'up'"));
}

TEST_CASE("obj: MTL materials and textures resolve to the mesh's inline materials") {
    const std::string root = fresh_dir("obj_mtl");
    const std::string a = root + "/assets";
    write_file(a + "/textures/hero.png", make_gradient_png(4, 4));
    write_file(a + "/textures/hero.png.mceasset", sidecar(kTexGuid, "texture"));
    write_file(a + "/models/box.mtl",
               "# materials\nnewmtl red\nKd 1 0.5 0.25\nd 0.5\nNs 98\nKe 0.1 0.2 0.3\nmap_Kd -s 2 2 ../textures/hero.png\n"
               "Ka 1 1 1\nmap_Ks specular.png\nnewmtl plain\nKd 0.2\nPr 0.25\nPm 1\n");
    write_file(a + "/models/box.mtl.mceasset", sidecar(kMtlGuid, "mtl"));
    write_file(a + "/models/box.obj", std::string("mtllib box.mtl\n") + kTri + "usemtl red\nf 1 2 3\nusemtl plain\nf 1 3 2\nusemtl ghost\nf 1 2 3\n");
    write_file(a + "/models/box.obj.mceasset", sidecar(kObjGuid, "obj"));
    CookService svc;
    const CookReport r = svc.cook(root.c_str(), CookOptions{});
    const std::string text = diag_text(r);
    INFO(text);
    REQUIRE(r.ok());
    CHECK(r.cooked == 3);  // the texture, the .mtl, then the model
    CHECK(has(text, "'ghost' is not defined by any mtllib"));
    CHECK(has(text, "'map_Ks' is not imported"));
    CHECK(has(text, "texture options"));
    const std::vector<byte> bytes = cooked_mesh(root, kObjGuid);
    const MeshView m = view_of(bytes);
    REQUIRE(m.submeshes.size() == 3);
    const MaterialChunk& red = m.submeshes[0].material;
    CHECK(red.base_color[0] == doctest::Approx(1.0f));
    CHECK(red.base_color[1] == doctest::Approx(0.5f));
    CHECK(red.base_color[2] == doctest::Approx(0.25f));
    CHECK(red.base_color[3] == doctest::Approx(0.5f));
    CHECK((red.flags & MaterialFlags::AlphaBlend) != 0);
    CHECK(red.roughness == doctest::Approx(std::pow(2.0f / 100.0f, 0.25f)));  // perceptual: the shader squares it
    CHECK(red.emissive[2] == doctest::Approx(0.3f));
    CHECK(red.texture(MaterialSlot::BaseColor) == G(kTexGuid));
    const MaterialChunk& plain = m.submeshes[1].material;
    CHECK(plain.base_color[2] == doctest::Approx(0.2f));
    CHECK(plain.roughness == doctest::Approx(0.25f));
    CHECK(plain.metallic == doctest::Approx(1.0f));
    CHECK((plain.flags & MaterialFlags::AlphaBlend) == 0);
    CHECK(plain.texture(MaterialSlot::BaseColor).is_null());
    const MaterialChunk& ghost = m.submeshes[2].material;  // default
    CHECK(ghost.base_color[0] == doctest::Approx(1.0f));
    CHECK(ghost.roughness == doctest::Approx(1.0f));

    // The .mtl itself is an asset: a change to any of its materials (here the second) re-cooks the model.
    write_file(a + "/models/box.mtl",
               "newmtl red\nKd 1 0.5 0.25\nd 0.5\nNs 98\nKe 0.1 0.2 0.3\nmap_Kd -s 2 2 ../textures/hero.png\nnewmtl plain\nKd 0.9\nPr 0.25\nPm 1\n");
    const CookReport r2 = svc.cook(root.c_str(), CookOptions{});
    INFO(diag_text(r2));
    REQUIRE(r2.ok());
    const std::vector<byte> m2_bytes = cooked_mesh(root, kObjGuid);  // the view points into these bytes
    const MeshView m2 = view_of(m2_bytes);
    CHECK(m2.submeshes[1].material.base_color[2] == doctest::Approx(0.9f));
    CHECK(m2.submeshes[0].material.texture(MaterialSlot::BaseColor) == G(kTexGuid));
}

TEST_CASE("obj: packed ORM, map_Ke, d over Tr and absolute texture paths") {
    const std::string root = fresh_dir("obj_mtl_rules");
    const std::string a = root + "/assets";
    write_file(a + "/models/rm.png", make_gradient_png(4, 4));
    write_file(a + "/models/rm.png.mceasset", sidecar(kTexGuid, "texture"));
    write_file(a + "/models/box.mtl",
               "newmtl packed\nNs 98\nmap_Pr rm.png\nmap_Pm rm.png\n"
               "newmtl explicit\nPr 0.3\nPm 0.5\nmap_Pr rm.png\nmap_Pm rm.png\n"
               "newmtl glow\nmap_Ke rm.png\n"
               "newmtl glow2\nKe 0.2 0 0\nmap_Ke rm.png\n"
               "newmtl dfirst\nd 0.5\nTr 0\n"
               "newmtl trfirst\nTr 0.9\nd 1\n"
               "newmtl abs1\nmap_Kd C:\\Users\\me\\tex\\rm.png\n"
               "newmtl abs2\nmap_Kd /home/me/rm.png\n");
    write_file(a + "/models/box.mtl.mceasset", sidecar(kMtlGuid, "mtl"));
    write_file(a + "/models/box.obj", std::string("mtllib box.mtl\n") + kTri +
                                          "usemtl packed\nf 1 2 3\nusemtl explicit\nf 1 2 3\nusemtl glow\nf 1 2 3\nusemtl glow2\nf 1 2 3\n"
                                          "usemtl dfirst\nf 1 2 3\nusemtl trfirst\nf 1 2 3\nusemtl abs1\nf 1 2 3\nusemtl abs2\nf 1 2 3\n");
    write_file(a + "/models/box.obj.mceasset", sidecar(kObjGuid, "obj"));
    CookService svc;
    const CookReport r = svc.cook(root.c_str(), CookOptions{});
    const std::string text = diag_text(r);
    INFO(text);
    REQUIRE(r.ok());
    CHECK(has(text, "absolute texture paths"));
    CHECK(has(text, "first: line 22, 2 statements"));
    const std::vector<byte> m_bytes = cooked_mesh(root, kObjGuid);  // the view points into these bytes
    const MeshView m = view_of(m_bytes);
    REQUIRE(m.submeshes.size() == 8);
    const MaterialChunk& packed = m.submeshes[0].material;
    CHECK(packed.texture(MaterialSlot::MetallicRoughness) == G(kTexGuid));
    CHECK(packed.roughness == doctest::Approx(1.0f));  // Ns does not apply under the packed texture
    CHECK(packed.metallic == doctest::Approx(1.0f));
    const MaterialChunk& expl = m.submeshes[1].material;
    CHECK(expl.roughness == doctest::Approx(0.3f));
    CHECK(expl.metallic == doctest::Approx(0.5f));
    CHECK(m.submeshes[2].material.emissive[0] == doctest::Approx(1.0f));
    CHECK(m.submeshes[2].material.emissive[2] == doctest::Approx(1.0f));
    CHECK(m.submeshes[3].material.emissive[0] == doctest::Approx(0.2f));
    CHECK(m.submeshes[3].material.emissive[1] == doctest::Approx(0.0f));
    CHECK(m.submeshes[4].material.base_color[3] == doctest::Approx(0.5f));
    CHECK(m.submeshes[5].material.base_color[3] == doctest::Approx(1.0f));
    CHECK(m.submeshes[6].material.texture(MaterialSlot::BaseColor) == G(kTexGuid));
    CHECK(m.submeshes[7].material.texture(MaterialSlot::BaseColor) == G(kTexGuid));
}

TEST_CASE("obj: an absolute texture path whose file name is not there is a missing-texture error") {
    const std::string root = fresh_dir("obj_abs_missing");
    const std::string a = root + "/assets";
    write_file(a + "/models/box.mtl", "newmtl red\nKd 1 0 0\nmap_Kd C:\\x\\nope.png\n");
    write_file(a + "/models/box.mtl.mceasset", sidecar(kMtlGuid, "mtl"));
    write_file(a + "/models/box.obj", std::string("mtllib box.mtl\n") + kTri + "usemtl red\nf 1 2 3\n");
    write_file(a + "/models/box.obj.mceasset", sidecar(kObjGuid, "obj"));
    CookService svc;
    const std::string text = diag_text(svc.cook(root.c_str(), CookOptions{}));
    INFO(text);
    CHECK(has(text, "box.mtl:3"));
    CHECK(has(text, "models/nope.png' is not a registered asset"));
    const Direct esc = mtl_direct("newmtl a\nmap_Kd ../../up.png\n");
    CHECK(!esc.ok);  // an escape from the project is still rejected
    CHECK(has(esc.diags, "stays inside assets/"));
}

TEST_CASE("mtl: float underflow parses as zero, overflow stays an error") {
    const Direct under = mtl_direct("newmtl a\nKd 1e-400 0.5 0.5\nNs 1e-400\nd 1e-9999\n");
    INFO(under.diags);
    CHECK(under.ok);
    CHECK(!mtl_direct("newmtl a\nKd 1e400\n").ok);
}

TEST_CASE("obj: a missing .mtl is a warning, a missing texture an error naming the line") {
    {
        const Cooked c = cook_obj("obj_no_mtl", std::string("mtllib nothere.mtl\n") + kTri + "usemtl a\nf 1 2 3\n");
        INFO(c.text);
        REQUIRE(c.report.ok());
        CHECK(has(c.text, "warning"));
        CHECK(has(c.text, "mtllib 'nothere.mtl' is not a registered asset"));
        CHECK(!has(c.text, "'a' is not defined"));  // the missing file already explained the default
        const MeshView m = view_of(c.mesh);
        CHECK(m.submeshes.size() == 1);
    }
    {
        const std::string root = fresh_dir("obj_no_tex");
        const std::string a = root + "/assets";
        write_file(a + "/models/box.mtl", "newmtl red\nKd 1 0 0\nmap_Kd gone.png\n");
        write_file(a + "/models/box.mtl.mceasset", sidecar(kMtlGuid, "mtl"));
        write_file(a + "/models/box.obj", std::string("mtllib box.mtl\n") + kTri + "usemtl red\nf 1 2 3\n");
        write_file(a + "/models/box.obj.mceasset", sidecar(kObjGuid, "obj"));
        CookService svc;
        const CookReport r = svc.cook(root.c_str(), CookOptions{});
        const std::string text = diag_text(r);
        INFO(text);
        CHECK(!r.ok());
        CHECK(has(text, "box.mtl:3"));
        CHECK(has(text, "models/gone.png' is not a registered asset"));
    }
}

TEST_CASE("obj: lod_levels and max_triangles") {
    // A 24 x 24 grid of quads (1152 triangles) with a mild bump, so simplification has something to remove.
    std::string grid;
    const int n = 24;
    for (int y = 0; y <= n; ++y)
        for (int x = 0; x <= n; ++x) grid += "v " + num(x) + " " + num(y) + " " + std::to_string(((x * 7 + y * 3) % 5) * 0.01) + "\n";
    for (int y = 0; y < n; ++y)
        for (int x = 0; x < n; ++x) {
            const int i = y * (n + 1) + x + 1;
            grid += "f " + num(i) + " " + num(i + 1) + " " + num(i + n + 2) + " " + num(i + n + 1) + "\n";
        }
    const Cooked plain = cook_obj("obj_lod_plain", grid);
    REQUIRE(plain.report.ok());
    CHECK(view_of(plain.mesh).submeshes[0].index_count == u32(n * n * 6));
    CHECK(view_of(plain.mesh).submesh_lods(0).empty());
    const Cooked lod = cook_obj("obj_lod", grid, "{ \"lod_levels\": 2, \"lod_ratio\": 0.4, \"lod_error\": 0.5 }");
    INFO(lod.text);
    REQUIRE(lod.report.ok());
    const MeshView m = view_of(lod.mesh);
    CHECK(m.submeshes[0].index_count == u32(n * n * 6));
    CHECK(m.submesh_lods(0).size() >= 1);
    CHECK(m.submesh_lods(0)[0].index_count < m.submeshes[0].index_count);
    const Cooked lod2 = cook_obj("obj_lod_again", grid, "{ \"lod_levels\": 2, \"lod_ratio\": 0.4, \"lod_error\": 0.5 }");
    CHECK(lod2.mesh == lod.mesh);

    const Cooked within = cook_obj("obj_budget_ok", grid, "{ \"max_triangles\": 1152 }");
    CHECK(within.report.ok());
    const Cooked over = cook_obj("obj_budget_over", grid, "{ \"max_triangles\": 1151 }");
    CHECK(!over.report.ok());
    CHECK(has(over.text, "1152 triangles, over the mesh's budget of 1151"));
    const Cooked bad_lod = cook_obj("obj_lod_bad", grid, "{ \"lod_levels\": 7 }");
    CHECK(!bad_lod.report.ok());
    const Cooked typo = cook_obj("obj_typo", grid, "{ \"scal\": 2 }");
    CHECK(!typo.report.ok());
}

TEST_CASE("obj: unsupported statements warn once each, with the first line and the count") {
    const std::string text = std::string("v 0 0 0 1 0 0\nv 1 0 0 0 1 0\nv 0 1 0 0 0 1\nv 1 1 1\nl 1 2\nl 2 3\np 1\ns 1\ns off\nvp 0.1\ncurv 0 1 1 2\nbevel on\n") +
                             "f 1 2 3\n";
    const Direct d = obj_direct(text);
    INFO(d.diags);
    REQUIRE(d.ok);
    CHECK(has(d.diags, "obj:5: warning: points and lines (l, p) are not imported (first: line 5, 3 statements)"));
    CHECK(has(d.diags, "free-form curves and surfaces are not imported (first: line 10, 2 statements)"));
    CHECK(has(d.diags, "smoothing groups (s) are ignored"));
    CHECK(has(d.diags, "other attribute statements are ignored"));
    CHECK(has(d.diags, "vertex colors after v x y z are not imported (first: line 1, 3 statements)"));
    const Direct none = obj_direct("# nothing\n");
    REQUIRE(none.ok);
    CHECK(has(none.diags, "the file has no faces"));
}

TEST_CASE("obj: malformed input is an error naming file:line") {
    struct Case {
        std::string text;
        const char* expect;
    };
    std::string long_face = std::string(kTri) + "f";
    for (int i = 0; i < 1048577; ++i) long_face += " 1";
    long_face += "\n";
    std::string long_line = "# ";
    long_line.append((size_t(16) << 20) + 8, 'a');
    std::string nul = std::string(kTri) + "f 1 2 3\n";
    nul[4] = '\0';
    const Case cases[] = {
        {std::string("v 0 0 0\nf 1 2 3\n"), "m.obj:2: error: vertex index 2 is out of range (1 defined so far)"},
        {std::string(kTri) + "f -4 -2 -1\n", "m.obj:4: error: vertex index -4 reaches before the first one (3 defined so far)"},
        {std::string(kTri) + "f 0 1 2\n", "index 0 is not valid"},
        {std::string(kTri) + "f 1/2 2 3\n", "texture coordinate index 2 is out of range (0 defined so far)"},
        {std::string(kTri) + "f 1//2 2 3\n", "normal index 2 is out of range"},
        {std::string(kTri) + "f 1 2 99999999999999999999\n", "invalid vertex index"},
        {std::string(kTri) + "f 1 2 x\n", "invalid vertex index 'x'"},
        {std::string(kTri) + "f 1/2/3/4 2 3\n", "invalid face vertex"},
        {std::string(kTri) + "f 1/ 2 3\n", "invalid face vertex"},
        {std::string(kTri) + "f 1 2\n", "a face needs at least 3 vertices, this one has 2"},
        {std::string(kTri) + "f 1/2/ 2 3\n", "invalid face vertex"},
        {std::string("v 1 2\n"), "'v' needs x y z"},
        {std::string("v 1 2 3 4 5\n"), "'v' needs x y z"},
        {std::string("v 1 2 3 4 5 6 7\n"), "has more than 6 values"},
        {std::string("v 1 nan 3\n"), "is not a finite number"},
        {std::string("v inf 0 0\n"), "is not a finite number"},
        {std::string("v 1e300 0 0\n"), "out of range"},
        {std::string("v 1e999 0 0\n"), "out of range"},
        {std::string("v -1e300 0 0\n"), "out of range"},
        {std::string("v 1,5 0 0\n"), "invalid number"},
        {std::string("v 0x10 0 0\n"), "invalid number"},
        {std::string("vt\n"), "'vt' needs u"},
        {std::string("vn 1 2\n"), "'vn' needs x y z"},
        {std::string("xyzzy 1 2\n"), "unknown statement 'xyzzy'"},
        {std::string("usemtl\n"), "usemtl needs a material name"},
        {std::string("mtllib\n"), "mtllib needs a file name"},
        {nul, "binary data"},
        {long_face, "a face with more than 1048576 vertices"},
        {long_line, "line is over the importer's limit of 16 MiB"},
    };
    for (const Case& c : cases) {
        const Direct d = obj_direct(c.text);
        INFO(std::string(c.expect));
        INFO(d.diags);
        CHECK(!d.ok);
        CHECK(has(d.diags, c.expect));
        CHECK(has(d.diags, "error"));
    }
}

TEST_CASE("obj: large but valid counts work and an index past them does not") {
    std::string big;
    for (int i = 0; i < 100000; ++i) big += "v " + num(i) + " 0 " + num(i % 7) + "\n";
    const Direct ok = obj_direct(big + "f 1 50000 100000\nf -1 -2 -3\n");
    INFO(ok.diags);
    CHECK(ok.ok);
    const Direct bad = obj_direct(big + "f 1 50000 100001\n");
    CHECK(!bad.ok);
    CHECK(has(bad.diags, "m.obj:100001: error: vertex index 100001 is out of range (100000 defined so far)"));
}

TEST_CASE("mtl: malformed input is an error naming the line") {
    struct Case {
        std::string text;
        const char* expect;
    };
    const Case cases[] = {
        {"Kd 1 1 1\n", "before the first newmtl"},
        {"newmtl\n", "newmtl needs a name"},
        {"newmtl a\nKd 1 1\n", "needs 1 or 3 values"},
        {"newmtl a\nKd 1 nan 1\n", "is not a finite number"},
        {"newmtl a\nNs 1e300\n", "out of range"},
        {"newmtl a\nd\n", "'d' needs a value"},
        {"newmtl a\nmap_Kd\n", "texture statement has no file name"},
        {"newmtl a\nmap_Kd -bm\n", "needs a value"},
        {"newmtl a\nmap_Kd -zz 1 a.png\n", "unknown texture option"},
        {std::string("newmtl a\n") + '\0' + "\n", "binary data"},
    };
    for (const Case& c : cases) {
        const Direct d = mtl_direct(c.text);
        INFO(std::string(c.expect));
        INFO(d.diags);
        CHECK(!d.ok);
        CHECK(has(d.diags, c.expect));
    }
    const Direct dup = mtl_direct("newmtl a\nKd 1 0 0\nnewmtl a\nKd 0 1 0\nNi 1.5\nillum 2\n");
    CHECK(dup.ok);
    CHECK(has(dup.diags, "is defined twice"));
    const Direct two = mtl_direct("newmtl a\nnewmtl b\n");
    CHECK(two.ok);
    CHECK(has(two.diags, "only the first material is cooked"));
}

TEST_CASE("obj: fuzzed input never crashes") {
    // The base files exercise every statement; every prefix of them, then seeded byte flips, insertions, deletions,
    // duplications and random binary buffers are cooked. Each must error or succeed, and a success must be repeatable.
    const std::string obj_base = std::string("# fuzz\nmtllib a.mtl b.mtl\no thing\ng part\ns 1\nv 0 0 0 1 0 0\nv 1 0 0\nv 1 1 0\nv 0 1 0\nv 0.5 0.5 1\n") +
                                 "vt 0 0\nvt 1 0 0.5\nvt 1 1\nvt 0 1\nvn 0 0 1\nvn 0 1 0\nusemtl red\nf 1/1/1 2/2/1 3/3/1 4/4/1\nf 1//2 2//2 5//2\n" +
                                 "usemtl blue\nf -5 -4 -3\nl 1 2\np 3\ng other\nf 1 2 3 4 5 \\\n  1 2 3\nvp 0\ncurv 0 1 1 2\nf -1/-1 -2/-2 -3/-3\n";
    const std::string mtl_base = std::string("newmtl red\nKd 1 0.5 0.25\nd 0.5\nNs 98\nKe 0 0 0\nmap_Kd -s 2 2 -clamp on tex/a.png\nmap_Bump -bm 0.5 n.png\n") +
                                 "newmtl blue\nKd 0.2\nTr 0.3\nPr 0.5\nPm 1\nmap_Pr rm.png\nmap_Pm rm.png\nmap_Ke e.png\nillum 2\nKa 1 1 1\nmap_d x.png\n";
    const char* const dict[] = {"-1", "0", "1e300", "nan", "inf", "-0", "99999999999999999999", "/", "//", "1/1/1/1", "\\\n", "\r", " ", "-", "+", "1e-999",
                                "f", "v", "vn", "vt", "usemtl", "mtllib", "newmtl", "o", "g", "# ", "4294967295", "2147483648", "-2147483649", "1048577"};
    const size_t dict_n = sizeof(dict) / sizeof(dict[0]);
    Rng rng{0x9E3779B97F4A7C15ull};
    auto run = [&](const std::string& text, bool mtl) {
        const Direct a = mtl ? mtl_direct(text) : obj_direct(text);
        if (a.ok) {
            const Direct b = mtl ? mtl_direct(text) : obj_direct(text);
            CHECK(b.ok);
            CHECK_SAME_BYTES(a.bytes, b.bytes);
        } else {
            CHECK(has(a.diags, "error"));
        }
    };
    auto mutate = [&](const std::string& base) {
        std::string s = base;
        const size_t edits = 1 + rng.below(4);
        for (size_t e = 0; e < edits; ++e) {
            switch (rng.below(5)) {
                case 0:
                    if (!s.empty()) s[rng.below(s.size())] = static_cast<char>(rng.below(256));
                    break;
                case 1:
                    s.insert(rng.below(s.size() + 1), dict[rng.below(dict_n)]);
                    break;
                case 2:
                    if (!s.empty()) s.erase(rng.below(s.size()), 1 + rng.below(12));
                    break;
                case 3:
                    if (!s.empty()) {
                        const size_t at = rng.below(s.size());
                        s.insert(rng.below(s.size() + 1), s.substr(at, 1 + rng.below(40)));
                    }
                    break;
                default:
                    if (!s.empty()) s[rng.below(s.size())] ^= static_cast<char>(1u << rng.below(8));
                    break;
            }
        }
        return s;
    };
    run(obj_base, false);
    run(mtl_base, true);
    for (size_t len = 0; len <= obj_base.size(); ++len) run(obj_base.substr(0, len), false);
    for (size_t len = 0; len <= mtl_base.size(); ++len) run(mtl_base.substr(0, len), true);
    for (int i = 0; i < 2000; ++i) run(mutate(obj_base), false);
    for (int i = 0; i < 1000; ++i) run(mutate(mtl_base), true);
    for (int i = 0; i < 60; ++i) {  // binary garbage
        std::string g(1 + rng.below(3000), '\0');
        for (char& c : g) c = static_cast<char>(rng.below(256));
        run(g, false);
        run(g, true);
    }
    // Absurd claims: indices past the 32-bit range, a million-vertex claim by index, absurd numbers everywhere.
    run(std::string(kTri) + "f 4294967296 1 2\n", false);
    run(std::string(kTri) + "f 1000000 1 2\n", false);
    run(std::string(kTri) + "f -9223372036854775808 1 2\n", false);
    run("v 1e300 1e300 1e300\nv 0 0 0\nv 1 0 0\nf 1 2 3\n", false);
    run("v 3.4e38 0 0\nv 0 0 0\nv 0 3.4e38 0\nf 1 2 3\n", false);
    run(std::string(kTri) + "vt 1e300 nan\nvn inf 0 0\nf 1/1/1 2/1/1 3/1/1\n", false);
    run(std::string(kTri) + "f 1 1 1\nf 1 2 2\nf 1 2 3 3 3 3\n", false);  // degenerate faces
}
