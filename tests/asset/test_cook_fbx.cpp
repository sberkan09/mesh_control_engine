// Autodesk FBX import (M-IMPORT3D step 3b, ADR-0047). ASCII FBX 7.4 fixtures are written here (no binary assets): a box in
// centimetres with Z up and the same box in metres with Y up (and Blender's scale pairs) give the geometry of the glTF
// box within one quantization step, a 3-bone skin with a rotation clip is sampled and compared with the analytic pose,
// materials and textures resolve, embedded textures and missing images are errors, unsupported features warn, the sidecar
// keys work, every fixture cooks to the same bytes twice, and corrupt input (truncations, seeded byte flips, a binary file
// claiming 2^29 elements, a 64K-corner polygon, 5000-deep node nesting, absurd and non-finite transforms) errors or
// succeeds but never crashes. This file names no ufbx symbol (the "ufbx wall").

#include <doctest/doctest.h>

#include <algorithm>
#include <array>
#include <chrono>
#include <cmath>
#include <cstdio>
#include <cctype>
#include <cstdlib>
#include <cstring>
#include <string>
#include <utility>
#include <vector>

#include "cook_test_util.h"
#include "fbx_importer.h"
#include "mce/asset/asset_system.h"
#include "mce/cook/cook.h"

using namespace mce;
using namespace mce::asset;
using namespace cook_test;
using cook::CookOptions;
using cook::CookReport;
using cook::CookService;

namespace {

constexpr const char* kFbxGuid = "6a1d9c58-0f4b-4b8e-9d1b-1c2a3b4c5f01";
constexpr const char* kGltfGuid = "6a1d9c58-0f4b-4b8e-9d1b-1c2a3b4c5f02";
constexpr const char* kTexGuid = "6a1d9c58-0f4b-4b8e-9d1b-1c2a3b4c5f03";
constexpr const char* kTex2Guid = "6a1d9c58-0f4b-4b8e-9d1b-1c2a3b4c5f04";

Guid G(const char* s) { return parse_guid(s).value(); }

// ---------------------------------------------------------------------------------------------------------------------
// small math (double precision, column-major 4x4), so the checks do not share the engine's float paths

struct V3 {
    double x = 0, y = 0, z = 0;
};

struct M4 {
    double m[16] = {1, 0, 0, 0, 0, 1, 0, 0, 0, 0, 1, 0, 0, 0, 0, 1};
};

M4 mul(const M4& a, const M4& b) {
    M4 r;
    for (int c = 0; c < 4; ++c)
        for (int row = 0; row < 4; ++row) {
            double s = 0;
            for (int k = 0; k < 4; ++k) s += a.m[k * 4 + row] * b.m[c * 4 + k];
            r.m[c * 4 + row] = s;
        }
    return r;
}

M4 from_trs(const float t[3], const float q[4], const float s[3]) {
    const double x = q[0], y = q[1], z = q[2], w = q[3];
    M4 r;
    r.m[0] = (1 - 2 * (y * y + z * z)) * s[0];
    r.m[1] = (2 * (x * y + z * w)) * s[0];
    r.m[2] = (2 * (x * z - y * w)) * s[0];
    r.m[4] = (2 * (x * y - z * w)) * s[1];
    r.m[5] = (1 - 2 * (x * x + z * z)) * s[1];
    r.m[6] = (2 * (y * z + x * w)) * s[1];
    r.m[8] = (2 * (x * z + y * w)) * s[2];
    r.m[9] = (2 * (y * z - x * w)) * s[2];
    r.m[10] = (1 - 2 * (x * x + y * y)) * s[2];
    r.m[12] = t[0];
    r.m[13] = t[1];
    r.m[14] = t[2];
    return r;
}

V3 point(const M4& a, V3 p) {
    return {a.m[0] * p.x + a.m[4] * p.y + a.m[8] * p.z + a.m[12], a.m[1] * p.x + a.m[5] * p.y + a.m[9] * p.z + a.m[13],
            a.m[2] * p.x + a.m[6] * p.y + a.m[10] * p.z + a.m[14]};
}

V3 dir(const M4& a, V3 p) {
    V3 r{a.m[0] * p.x + a.m[4] * p.y + a.m[8] * p.z, a.m[1] * p.x + a.m[5] * p.y + a.m[9] * p.z, a.m[2] * p.x + a.m[6] * p.y + a.m[10] * p.z};
    const double l = std::sqrt(r.x * r.x + r.y * r.y + r.z * r.z);
    return l > 0 ? V3{r.x / l, r.y / l, r.z / l} : r;
}

double dist(V3 a, V3 b) { return std::sqrt((a.x - b.x) * (a.x - b.x) + (a.y - b.y) * (a.y - b.y) + (a.z - b.z) * (a.z - b.z)); }
double dot3(V3 a, V3 b) { return a.x * b.x + a.y * b.y + a.z * b.z; }
V3 sub(V3 a, V3 b) { return {a.x - b.x, a.y - b.y, a.z - b.z}; }
V3 cross(V3 a, V3 b) { return {a.y * b.z - a.z * b.y, a.z * b.x - a.x * b.z, a.x * b.y - a.y * b.x}; }

// ---------------------------------------------------------------------------------------------------------------------
// ASCII FBX 7.4 writer

std::string num(double v) {
    char b[48];
    std::snprintf(b, sizeof(b), "%.9g", v);
    return b;
}

std::string nums(const std::vector<double>& v) {
    std::string s;
    for (size_t i = 0; i < v.size(); ++i) s += (i ? "," : "") + num(v[i]);
    return s;
}

std::string ints(const std::vector<long>& v) {
    std::string s;
    for (size_t i = 0; i < v.size(); ++i) s += (i ? "," : "") + std::to_string(v[i]);
    return s;
}

constexpr double kTicks = 46186158000.0;  // FBX time units per second

struct FbxDoc {
    int up_axis = 1;       // 1 = Y, 2 = Z (Maya / Blender Y-up: front +Z; 3ds Max Z-up: front -Y)
    double unit_cm = 100;  // UnitScaleFactor: centimetres per file unit (100 = metres, 1 = centimetres)
    int right_sign = 1;    // -1 writes a left-handed file (the right axis points to -X)
    std::string objects, connections, extra_settings;
    long next_id = 1000;

    long id() { return next_id++; }

    std::string str() const {
        const int front = up_axis == 1 ? 2 : 1;
        const int front_sign = up_axis == 1 ? 1 : -1;
        std::string s = "; FBX 7.4.0 project file\n";
        s += "FBXHeaderExtension:  {\n\tFBXHeaderVersion: 1003\n\tFBXVersion: 7400\n\tCreator: \"mce test_cook_fbx\"\n}\n";
        s += "GlobalSettings:  {\n\tVersion: 1000\n\tProperties70:  {\n";
        s += "\t\tP: \"UpAxis\", \"int\", \"Integer\", \"\"," + std::to_string(up_axis) + "\n";
        s += "\t\tP: \"UpAxisSign\", \"int\", \"Integer\", \"\",1\n";
        s += "\t\tP: \"FrontAxis\", \"int\", \"Integer\", \"\"," + std::to_string(front) + "\n";
        s += "\t\tP: \"FrontAxisSign\", \"int\", \"Integer\", \"\"," + std::to_string(front_sign) + "\n";
        s += "\t\tP: \"CoordAxis\", \"int\", \"Integer\", \"\",0\n";
        s += "\t\tP: \"CoordAxisSign\", \"int\", \"Integer\", \"\"," + std::to_string(right_sign) + "\n";
        s += "\t\tP: \"UnitScaleFactor\", \"double\", \"Number\", \"\"," + num(unit_cm) + "\n";
        s += extra_settings;
        s += "\t}\n}\n";
        s += "Objects:  {\n" + objects + "}\n";
        s += "Connections:  {\n" + connections + "}\n";
        return s;
    }

    void connect(long child, long parent) { connections += "\tC: \"OO\"," + std::to_string(child) + "," + std::to_string(parent) + "\n"; }
    void connect_prop(long child, long parent, const std::string& prop) {
        connections += "\tC: \"OP\"," + std::to_string(child) + "," + std::to_string(parent) + ",\"" + prop + "\"\n";
    }

    // A model node. parent 0 = the scene root. Rotation in degrees.
    long model(const std::string& name, const char* type, long parent, V3 t = {}, V3 r = {}, V3 s = {1, 1, 1}, const std::string& props = "") {
        const long mid = id();
        objects += "\tModel: " + std::to_string(mid) + ", \"Model::" + name + "\", \"" + type + "\" {\n\t\tVersion: 232\n\t\tProperties70:  {\n";
        objects += "\t\t\tP: \"Lcl Translation\", \"Lcl Translation\", \"\", \"A\"," + nums({t.x, t.y, t.z}) + "\n";
        objects += "\t\t\tP: \"Lcl Rotation\", \"Lcl Rotation\", \"\", \"A\"," + nums({r.x, r.y, r.z}) + "\n";
        objects += "\t\t\tP: \"Lcl Scaling\", \"Lcl Scaling\", \"\", \"A\"," + nums({s.x, s.y, s.z}) + "\n";
        objects += props;
        objects += "\t\t}\n\t}\n";
        connect(mid, parent);
        return mid;
    }

    long limb(const std::string& name, long parent, V3 t = {}) {
        const long node = model(name, "LimbNode", parent, t);
        const long attr = id();
        objects += "\tNodeAttribute: " + std::to_string(attr) + ", \"NodeAttribute::\", \"LimbNode\" {\n\t\tTypeFlags: \"Skeleton\"\n\t}\n";
        connect(attr, node);
        return node;
    }

    // Geometry from raw polygons. `normals` / `uvs` / `uv_index` empty = absent. Face-vertex lists: a face's last index
    // is stored as ~i.
    long geometry(const std::string& name, const std::vector<double>& pos, const std::vector<std::vector<long>>& faces,
                  const std::vector<double>& normals = {}, const std::vector<double>& uvs = {}, const std::vector<long>& uv_index = {},
                  const std::string& material_layer = "") {
        const long gid = id();
        std::vector<long> poly;
        for (const auto& f : faces)
            for (size_t i = 0; i < f.size(); ++i) poly.push_back(i + 1 == f.size() ? -f[i] - 1 : f[i]);
        std::string s = "\tGeometry: " + std::to_string(gid) + ", \"Geometry::" + name + "\", \"Mesh\" {\n";
        s += "\t\tVertices: *" + std::to_string(pos.size()) + " {\n\t\t\ta: " + nums(pos) + "\n\t\t}\n";
        s += "\t\tPolygonVertexIndex: *" + std::to_string(poly.size()) + " {\n\t\t\ta: " + ints(poly) + "\n\t\t}\n";
        s += "\t\tGeometryVersion: 124\n";
        std::string layer;
        if (!normals.empty()) {
            s += "\t\tLayerElementNormal: 0 {\n\t\t\tVersion: 101\n\t\t\tName: \"\"\n\t\t\tMappingInformationType: \"ByPolygonVertex\"\n";
            s += "\t\t\tReferenceInformationType: \"Direct\"\n\t\t\tNormals: *" + std::to_string(normals.size()) + " {\n\t\t\t\ta: " + nums(normals) +
                 "\n\t\t\t}\n\t\t}\n";
            layer += "\t\t\tLayerElement:  {\n\t\t\t\tType: \"LayerElementNormal\"\n\t\t\t\tTypedIndex: 0\n\t\t\t}\n";
        }
        if (!uvs.empty()) {
            s += "\t\tLayerElementUV: 0 {\n\t\t\tVersion: 101\n\t\t\tName: \"UVMap\"\n\t\t\tMappingInformationType: \"ByPolygonVertex\"\n";
            s += "\t\t\tReferenceInformationType: \"IndexToDirect\"\n\t\t\tUV: *" + std::to_string(uvs.size()) + " {\n\t\t\t\ta: " + nums(uvs) + "\n\t\t\t}\n";
            s += "\t\t\tUVIndex: *" + std::to_string(uv_index.size()) + " {\n\t\t\t\ta: " + ints(uv_index) + "\n\t\t\t}\n\t\t}\n";
            layer += "\t\t\tLayerElement:  {\n\t\t\t\tType: \"LayerElementUV\"\n\t\t\t\tTypedIndex: 0\n\t\t\t}\n";
        }
        if (!material_layer.empty()) {
            s += material_layer;
            layer += "\t\t\tLayerElement:  {\n\t\t\t\tType: \"LayerElementMaterial\"\n\t\t\t\tTypedIndex: 0\n\t\t\t}\n";
        }
        if (!layer.empty()) s += "\t\tLayer: 0 {\n\t\t\tVersion: 100\n" + layer + "\t\t}\n";
        s += "\t}\n";
        objects += s;
        return gid;
    }

    long material(const std::string& name, const std::string& props) {
        const long mid = id();
        objects += "\tMaterial: " + std::to_string(mid) + ", \"Material::" + name + "\", \"\" {\n\t\tVersion: 102\n\t\tShadingModel: \"phong\"\n";
        objects += "\t\tMultiLayer: 0\n\t\tProperties70:  {\n" + props + "\t\t}\n\t}\n";
        return mid;
    }

    // A file texture; `content_b64` non-empty embeds it (a Video clip with Content).
    long texture(const std::string& name, const std::string& absolute, const std::string& relative, const std::string& content_b64 = "") {
        const long tid = id();
        objects += "\tTexture: " + std::to_string(tid) + ", \"Texture::" + name + "\", \"\" {\n\t\tType: \"TextureVideoClip\"\n\t\tVersion: 202\n";
        objects += "\t\tTextureName: \"Texture::" + name + "\"\n\t\tFileName: \"" + absolute + "\"\n\t\tRelativeFilename: \"" + relative + "\"\n\t}\n";
        if (!content_b64.empty()) {
            const long vid = id();
            objects += "\tVideo: " + std::to_string(vid) + ", \"Video::" + name + "\", \"Clip\" {\n\t\tType: \"Clip\"\n\t\tProperties70:  {\n";
            objects += "\t\t\tP: \"Path\", \"KString\", \"XRefUrl\", \"\", \"" + absolute + "\"\n\t\t}\n";
            objects += "\t\tFileName: \"" + absolute + "\"\n\t\tRelativeFilename: \"" + relative + "\"\n\t\tContent: , \"" + content_b64 + "\"\n\t}\n";
            connect(vid, tid);
        }
        return tid;
    }

    // Skin deformer on `geom` with one cluster per bone. indices / weights per bone; bind matrices column-major.
    long skin(long geom) {
        const long sid = id();
        objects += "\tDeformer: " + std::to_string(sid) + ", \"Deformer::Skin\", \"Skin\" {\n\t\tVersion: 101\n\t\tLink_DeformAcuracy: 50\n\t}\n";
        connect(sid, geom);
        return sid;
    }

    long cluster(long skin_id, long bone, const std::vector<long>& index, const std::vector<double>& weight, const M4& mesh_bind, const M4& bone_bind) {
        const long cid = id();
        std::vector<double> t(mesh_bind.m, mesh_bind.m + 16), l(bone_bind.m, bone_bind.m + 16);
        objects += "\tDeformer: " + std::to_string(cid) + ", \"SubDeformer::Cluster\", \"Cluster\" {\n\t\tVersion: 100\n\t\tUserData: \"\", \"\"\n";
        objects += "\t\tIndexes: *" + std::to_string(index.size()) + " {\n\t\t\ta: " + ints(index) + "\n\t\t}\n";
        objects += "\t\tWeights: *" + std::to_string(weight.size()) + " {\n\t\t\ta: " + nums(weight) + "\n\t\t}\n";
        objects += "\t\tTransform: *16 {\n\t\t\ta: " + nums(t) + "\n\t\t}\n\t\tTransformLink: *16 {\n\t\t\ta: " + nums(l) + "\n\t\t}\n\t}\n";
        connect(cid, skin_id);
        connect(bone, cid);
        return cid;
    }

    // One animation stack [0, length_s] with `layers` layers; returns the first layer's id.
    long stack(const std::string& name, double length_s, int layers = 1) {
        const long sid = id();
        objects += "\tAnimationStack: " + std::to_string(sid) + ", \"AnimStack::" + name + "\", \"\" {\n\t\tProperties70:  {\n";
        objects += "\t\t\tP: \"LocalStart\", \"KTime\", \"Time\", \"\",0\n\t\t\tP: \"LocalStop\", \"KTime\", \"Time\", \"\"," +
                   std::to_string(static_cast<long long>(std::llround(length_s * kTicks))) + "\n\t\t}\n\t}\n";
        long first = 0;
        for (int i = 0; i < layers; ++i) {
            const long lid = id();
            objects += "\tAnimationLayer: " + std::to_string(lid) + ", \"AnimLayer::Layer" + std::to_string(i) + "\", \"\" {\n\t}\n";
            connect(lid, sid);
            if (!first) first = lid;
        }
        return first;
    }

    // A linear curve on one channel (0 = X, 1 = Y, 2 = Z) of a node's "Lcl Rotation" / "Lcl Translation" / "Lcl Scaling".
    void curve(long layer, long node, const char* prop, int channel, const std::vector<std::pair<double, double>>& keys) {
        const long cn = id(), cv = id();
        const char* label = std::strcmp(prop, "Lcl Rotation") == 0 ? "R" : std::strcmp(prop, "Lcl Translation") == 0 ? "T" : "S";
        const double def = std::strcmp(prop, "Lcl Scaling") == 0 ? 1.0 : 0.0;
        objects += "\tAnimationCurveNode: " + std::to_string(cn) + ", \"AnimCurveNode::" + label + "\", \"\" {\n\t\tProperties70:  {\n";
        for (const char* c : {"d|X", "d|Y", "d|Z"}) objects += std::string("\t\t\tP: \"") + c + "\", \"Number\", \"\", \"A\"," + num(def) + "\n";
        objects += "\t\t}\n\t}\n";
        std::vector<long> times;
        std::vector<double> values;
        for (const auto& k : keys) {
            times.push_back(static_cast<long>(std::llround(k.first * kTicks)));
            values.push_back(k.second);
        }
        objects += "\tAnimationCurve: " + std::to_string(cv) + ", \"AnimCurve::\", \"\" {\n\t\tDefault: 0\n\t\tKeyVer: 4009\n";
        objects += "\t\tKeyTime: *" + std::to_string(times.size()) + " {\n\t\t\ta: " + ints(times) + "\n\t\t}\n";
        objects += "\t\tKeyValueFloat: *" + std::to_string(values.size()) + " {\n\t\t\ta: " + nums(values) + "\n\t\t}\n";
        objects += "\t\tKeyAttrFlags: *1 {\n\t\t\ta: 4\n\t\t}\n\t\tKeyAttrDataFloat: *4 {\n\t\t\ta: 0,0,0,0\n\t\t}\n";
        objects += "\t\tKeyAttrRefCount: *1 {\n\t\t\ta: " + std::to_string(times.size()) + "\n\t\t}\n\t}\n";
        connect(cn, layer);
        connect_prop(cn, node, prop);
        connect_prop(cv, cn, channel == 0 ? "d|X" : channel == 1 ? "d|Y" : "d|Z");
    }
};

// ---------------------------------------------------------------------------------------------------------------------
// the box: 8 corners, 6 quads (counter-clockwise from outside), per-corner normals and UVs

const int kFaceVerts[6][4] = {{1, 3, 7, 5}, {0, 4, 6, 2}, {2, 6, 7, 3}, {0, 1, 5, 4}, {4, 5, 7, 6}, {0, 2, 3, 1}};
const int kFaceNormal[6][3] = {{1, 0, 0}, {-1, 0, 0}, {0, 1, 0}, {0, -1, 0}, {0, 0, 1}, {0, 0, -1}};
const double kUv[4][2] = {{0, 0}, {1, 0}, {1, 1}, {0, 1}};

// Half extents are in file coordinates (the file's own axes and unit). `reverse` writes the clockwise winding of a
// left-handed exporter.
// `half` is on the engine's axes; a Z-up file stores (x, -z, y) of them and a left-handed one stores x mirrored.
long box(FbxDoc& doc, V3 half, bool reverse = false, const std::string& material_layer = "") {
    const bool zup = doc.up_axis == 2;
    const double mx = doc.right_sign < 0 ? -1.0 : 1.0;
    auto put = [&](std::vector<double>& out, double x, double y, double z) {
        out.push_back(mx * x);
        out.push_back(zup ? -z : y);
        out.push_back(zup ? y : z);
    };
    std::vector<double> pos;
    for (int i = 0; i < 8; ++i) put(pos, (i & 1) ? half.x : -half.x, (i & 2) ? half.y : -half.y, (i & 4) ? half.z : -half.z);
    std::vector<std::vector<long>> faces;
    std::vector<double> normals, uvs;
    std::vector<long> uv_index;
    for (int f = 0; f < 6; ++f) {
        std::vector<long> face;
        face.reserve(4);
        for (int k = 0; k < 4; ++k) face.push_back(kFaceVerts[f][k]);
        if (reverse) std::reverse(face.begin(), face.end());
        faces.push_back(face);
        for (int k = 0; k < 4; ++k) {
            put(normals, kFaceNormal[f][0], kFaceNormal[f][1], kFaceNormal[f][2]);
            uv_index.push_back(reverse ? 3 - k : k);
        }
    }
    for (const auto& uv : kUv) {
        uvs.push_back(uv[0]);
        uvs.push_back(uv[1]);
    }
    return doc.geometry("Box", pos, faces, normals, uvs, uv_index, material_layer);
}

std::string all_same_material() {
    return "\t\tLayerElementMaterial: 0 {\n\t\t\tVersion: 101\n\t\t\tName: \"\"\n\t\t\tMappingInformationType: \"AllSame\"\n"
           "\t\t\tReferenceInformationType: \"IndexToDirect\"\n\t\t\tMaterials: *1 {\n\t\t\t\ta: 0\n\t\t\t}\n\t\t}\n";
}

// A file with one box node "Cube". `engine_half` are the half extents in metres on the engine's axes (x right, y up,
// z toward the viewer); the file gets the same box in its own units and axes.
std::string box_fbx(V3 engine_half, int up_axis, double unit_cm, bool left_handed = false, double node_scale = 1.0, double parent_scale = 0.0) {
    FbxDoc doc;
    doc.up_axis = up_axis;
    doc.unit_cm = unit_cm;
    doc.right_sign = left_handed ? -1 : 1;
    // Metres -> file units, then the file's axes: Z-up files hold (x, -z, y) of the engine's (x, y, z).
    const double k = 100.0 / unit_cm / node_scale / (parent_scale > 0 ? parent_scale : 1.0);
    const V3 half{engine_half.x * k, engine_half.y * k, engine_half.z * k};
    const long geom = box(doc, half, left_handed);
    long parent = 0;
    if (parent_scale > 0) parent = doc.model("Armature", "Null", 0, {}, {}, {parent_scale, parent_scale, parent_scale});
    const long node = doc.model("Cube", "Mesh", parent, {}, {}, {node_scale, node_scale, node_scale});
    doc.connect(geom, node);
    return doc.str();
}

// ---- the same box as glTF ----

void put_f32(std::string& s, float v) {
    char b[4];
    std::memcpy(b, &v, 4);
    s.append(b, 4);
}
void put_u16(std::string& s, unsigned v) {
    s.push_back(static_cast<char>(v & 0xFF));
    s.push_back(static_cast<char>(v >> 8));
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

std::string box_gltf(V3 half) {
    std::string bin;
    for (int f = 0; f < 6; ++f)
        for (int k = 0; k < 4; ++k) {
            const int v = kFaceVerts[f][k];
            put_f32(bin, static_cast<float>((v & 1) ? half.x : -half.x));
            put_f32(bin, static_cast<float>((v & 2) ? half.y : -half.y));
            put_f32(bin, static_cast<float>((v & 4) ? half.z : -half.z));
        }
    for (int f = 0; f < 6; ++f)
        for (int k = 0; k < 4; ++k)
            for (int c = 0; c < 3; ++c) put_f32(bin, static_cast<float>(kFaceNormal[f][c]));
    for (int f = 0; f < 6; ++f)
        for (int k = 0; k < 4; ++k) {
            put_f32(bin, static_cast<float>(kUv[k][0]));
            put_f32(bin, static_cast<float>(1.0 - kUv[k][1]));  // FBX's V runs up, glTF's down
        }
    for (int f = 0; f < 6; ++f)
        for (int i : {0, 1, 2, 0, 2, 3}) put_u16(bin, static_cast<unsigned>(f * 4 + i));
    auto view = [](size_t off, size_t len) {
        return "{\"buffer\":0,\"byteOffset\":" + std::to_string(off) + ",\"byteLength\":" + std::to_string(len) + "}";
    };
    std::string s = "{\"asset\":{\"version\":\"2.0\"},";
    s += "\"buffers\":[{\"byteLength\":" + std::to_string(bin.size()) + ",\"uri\":\"data:application/octet-stream;base64," + base64(bin) + "\"}],";
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

// ---------------------------------------------------------------------------------------------------------------------
// cooking and reading back

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

bool has(const std::string& hay, const char* needle) { return hay.find(needle) != std::string::npos; }

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

struct Cooked {
    std::string root;
    CookReport report;
    std::string text;
    std::vector<byte> mesh;
};

// A project with m.fbx (+ its sidecar) and any `extra` files (path under assets/ -> bytes), cooked once.
Cooked cook_fbx(const char* name, const std::string& fbx, const char* settings = "{}", const std::vector<std::pair<std::string, std::string>>& extra = {}) {
    Cooked c;
    c.root = fresh_dir(name);
    write_file(c.root + "/assets/models/m.fbx", fbx);
    write_file(c.root + "/assets/models/m.fbx.mceasset", sidecar(kFbxGuid, "fbx", settings));
    for (const auto& e : extra) write_file(c.root + "/assets/" + e.first, e.second);
    CookService svc;
    c.report = svc.cook(c.root.c_str(), CookOptions{});
    c.text = diag_text(c.report);
    if (c.report.ok()) c.mesh = cooked_mesh(c.root, kFbxGuid);
    return c;
}

Cooked cook_gltf(const char* name, const std::string& gltf) {
    Cooked c;
    c.root = fresh_dir(name);
    write_file(c.root + "/assets/models/m.gltf", gltf);
    write_file(c.root + "/assets/models/m.gltf.mceasset", sidecar(kGltfGuid, "gltf"));
    CookService svc;
    c.report = svc.cook(c.root.c_str(), CookOptions{});
    c.text = diag_text(c.report);
    if (c.report.ok()) c.mesh = cooked_mesh(c.root, kGltfGuid);
    return c;
}

// Direct importer calls (corrupt-input tests): no project, no registry, no texture lookup.
struct Direct {
    bool ok = false;
    std::string diags;
    std::vector<byte> bytes;
};

Direct fbx_direct(const std::string& text, const char* settings = nullptr) {
    static const mem::UniquePtr<cook::IImporter> imp = cook::make_fbx_importer();
    Direct d;
    cook::JsonDoc doc;
    cook::JsonError je;
    cook::ImportInput in;
    in.guid = G(kFbxGuid);
    in.display_path = "assets/m.fbx";
    in.source_path = "m.fbx";
    in.source = Span<const byte>(reinterpret_cast<const byte*>(text.data()), text.size());
    if (settings) {
        REQUIRE(doc.parse(StringView(settings), je));
        in.settings = doc.root();
    }
    ContainerWriter w;
    cook::DiagList diags;
    d.ok = imp->cook(in, w, diags);
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

long first_diff(const std::vector<byte>& a, const std::vector<byte>& b) {
    const size_t n = std::min(a.size(), b.size());
    for (size_t i = 0; i < n; ++i)
        if (a[i] != b[i]) return static_cast<long>(i);
    return a.size() == b.size() ? -1 : static_cast<long>(n);
}

#define CHECK_SAME_BYTES(a, b)                                 \
    do {                                                       \
        INFO("sizes " << (a).size() << " and " << (b).size()); \
        CHECK(first_diff((a), (b)) == -1);                     \
    } while (0)

V3 oct_decode(const MeshVertex& v) {
    double x = v.normal[0] / 32767.0, y = v.normal[1] / 32767.0;
    const double z = 1.0 - std::fabs(x) - std::fabs(y);
    double zz = z;
    if (z < 0) {
        const double ox = x;
        x = (1.0 - std::fabs(y)) * (ox >= 0 ? 1.0 : -1.0);
        y = (1.0 - std::fabs(ox)) * (y >= 0 ? 1.0 : -1.0);
        zz = z;
    }
    const double l = std::sqrt(x * x + y * y + zz * zz);
    return {x / l, y / l, zz / l};
}

M4 node_local(const MeshNode& n) { return from_trs(n.translation, n.rotation, n.scale); }

std::vector<M4> world_matrices(const MeshView& m, const std::vector<M4>* override_local = nullptr) {
    std::vector<M4> w(m.nodes.size());
    for (size_t i = 0; i < m.nodes.size(); ++i) {
        const M4 local = override_local ? (*override_local)[i] : node_local(m.nodes[i]);
        w[i] = m.nodes[i].parent < 0 ? local : mul(w[static_cast<size_t>(m.nodes[i].parent)], local);
    }
    return w;
}

struct WorldVertex {
    V3 p, n;
    double u = 0, v = 0;
};

V3 bind_position(const MeshSubmesh& sm, const MeshVertex& q) {
    auto d = [&](int c) { return sm.center[c] + std::max(q.position[c] / 32767.0, -1.0) * sm.extent[c]; };
    return {d(0), d(1), d(2)};
}

// Every rigid vertex of the mesh in world space (node chain applied), with its triangles' indices.
struct WorldMesh {
    std::vector<WorldVertex> v;
    std::vector<std::array<int, 3>> tris;
    double step = 0;  // the largest quantization step of a submesh
};

WorldMesh world_mesh(const MeshView& m) {
    WorldMesh out;
    const std::vector<M4> w = world_matrices(m);
    for (u32 s = 0; s < m.submeshes.size(); ++s) {
        const MeshSubmesh& sm = m.submeshes[s];
        const M4& nw = w[sm.node];
        const int base = static_cast<int>(out.v.size());
        for (u32 i = 0; i < sm.vertex_count; ++i) {
            const MeshVertex& q = m.vertices[sm.base_vertex + i];
            WorldVertex wv;
            wv.p = point(nw, bind_position(sm, q));
            wv.n = dir(nw, oct_decode(q));
            wv.u = sm.uv_offset[0] + q.uv[0] / 65535.0 * sm.uv_scale[0];
            wv.v = sm.uv_offset[1] + q.uv[1] / 65535.0 * sm.uv_scale[1];
            out.v.push_back(wv);
        }
        for (u32 i = 0; i + 2 < sm.index_count; i += 3)
            out.tris.push_back({base + static_cast<int>(m.index(sm.first_index + i)), base + static_cast<int>(m.index(sm.first_index + i + 1)),
                                base + static_cast<int>(m.index(sm.first_index + i + 2))});
        for (int c = 0; c < 3; ++c) out.step = std::max(out.step, sm.extent[c] / 32767.0);
    }
    return out;
}

// Each vertex of `a` has its own partner in `b` (position within `tol`, normal and uv close).
bool same_vertices(const WorldMesh& a, const WorldMesh& b, double tol) {
    if (a.v.size() != b.v.size()) return false;
    std::vector<char> used(b.v.size(), 0);
    for (const WorldVertex& x : a.v) {
        bool found = false;
        for (size_t j = 0; j < b.v.size() && !found; ++j) {
            const WorldVertex& y = b.v[j];
            if (used[j] || dist(x.p, y.p) > tol || dist(x.n, y.n) > 1e-3 || std::fabs(x.u - y.u) > 1e-3 || std::fabs(x.v - y.v) > 1e-3) continue;
            used[j] = 1;
            found = true;
        }
        if (!found) return false;
    }
    return true;
}

// Every triangle winds counter-clockwise about its vertices' own normals, and the total area is `area`.
void check_winding_and_area(const WorldMesh& m, double area) {
    double sum = 0;
    for (const auto& t : m.tris) {
        const V3 c = cross(sub(m.v[t[1]].p, m.v[t[0]].p), sub(m.v[t[2]].p, m.v[t[0]].p));
        const double len = std::sqrt(dot3(c, c));
        sum += len * 0.5;
        const V3 gn{c.x / len, c.y / len, c.z / len};
        for (int k = 0; k < 3; ++k) CHECK(dot3(gn, m.v[t[k]].n) > 0.99);
    }
    CHECK(sum == doctest::Approx(area).epsilon(1e-3));
}

}  // namespace

// ---------------------------------------------------------------------------------------------------------------------

TEST_CASE("fbx: a box in centimetres with Z up equals the glTF box in metres with Y up") {
    const V3 half{0.5, 1.0, 2.0};  // metres on the engine's axes: x 1 wide, y 2 tall, z 4 deep
    const double area = 2 * (1 * 2 + 1 * 4 + 2 * 4);

    const Cooked ref = cook_gltf("fbx_ref_gltf", box_gltf(half));
    INFO(ref.text);
    REQUIRE(ref.report.ok());
    const WorldMesh want = world_mesh(view_of(ref.mesh));
    REQUIRE(want.v.size() == 24);

    struct Variant {
        const char* name;
        std::string fbx;
    };
    const Variant variants[] = {
        {"fbx_y_m", box_fbx(half, 1, 100.0)},                       // Maya: Y up, metres
        {"fbx_y_cm", box_fbx(half, 1, 1.0)},                        // Y up, centimetres
        {"fbx_z_cm", box_fbx(half, 2, 1.0)},                        // 3ds Max: Z up, centimetres
        {"fbx_z_m", box_fbx(half, 2, 100.0)},                       // Z up, metres
        {"fbx_blender_100", box_fbx(half, 2, 1.0, false, 100.0)},   // Blender: centimetres, node scale 100 on metre vertices
        {"fbx_blender_pair", box_fbx(half, 2, 1.0, false, 0.01, 100.0)},  // armature 100 over a node of 0.01
        {"fbx_left_handed", box_fbx(half, 1, 100.0, true)},         // a left-handed file, mirrored on X with the winding fixed
    };
    std::vector<byte> first;
    for (const Variant& v : variants) {
        INFO("variant " << std::string(v.name));
        const Cooked c = cook_fbx(v.name, v.fbx);
        INFO(c.text);
        REQUIRE(c.report.ok());
        const MeshView mv = view_of(c.mesh);
        REQUIRE(mv.submeshes.size() == 1);
        const WorldMesh got = world_mesh(mv);
        CHECK(got.tris.size() == 12);
        CHECK(same_vertices(got, want, want.step + 1e-6));
        check_winding_and_area(got, area);
        // The cook is deterministic: the same file again gives the same bytes.
        const Cooked again = cook_fbx((std::string(v.name) + "_again").c_str(), v.fbx);
        REQUIRE(again.report.ok());
        CHECK_SAME_BYTES(c.mesh, again.mesh);
        // No warnings for a plain box.
        CHECK(c.report.diags.warning_count() == 0);
        if (std::string(v.name) == "fbx_z_cm") {
            // The axis turn is the root node's rotation (-90 degrees about X), the unit scale is in the vertices.
            REQUIRE(mv.nodes.size() == 1);
            CHECK(mv.node_name(0) == "Cube");
            // -90 degrees about +X (Z up to Y up): x and w differ in sign (q and -q are the same rotation, x * w = -1/2 either way).
            CHECK(mv.nodes[0].rotation[0] * mv.nodes[0].rotation[3] == doctest::Approx(-0.5f).epsilon(1e-4));
            CHECK(std::fabs(mv.nodes[0].rotation[1]) < 1e-5);
            CHECK(std::fabs(mv.nodes[0].rotation[2]) < 1e-5);
            CHECK(mv.nodes[0].scale[0] == doctest::Approx(1.0f));
            CHECK(mv.nodes[0].parent == -1);
        }
        if (std::string(v.name) == "fbx_y_m") {
            REQUIRE(mv.nodes.size() == 1);
            CHECK(std::fabs(mv.nodes[0].rotation[3]) == doctest::Approx(1.0f));
        }
    }
}

TEST_CASE("fbx: the sidecar scale multiplies the root nodes; V is flipped; unknown keys are errors") {
    const V3 half{0.5, 0.5, 0.5};
    const std::string fbx = box_fbx(half, 1, 100.0);
    const Cooked a = cook_fbx("fbx_scale_1", fbx);
    const Cooked b = cook_fbx("fbx_scale_2", fbx, "{\"scale\": 2.0}");
    REQUIRE(a.report.ok());
    REQUIRE(b.report.ok());
    const WorldMesh wa = world_mesh(view_of(a.mesh)), wb = world_mesh(view_of(b.mesh));
    REQUIRE(wa.v.size() == wb.v.size());
    for (size_t i = 0; i < wa.v.size(); ++i) CHECK(dist({wa.v[i].p.x * 2, wa.v[i].p.y * 2, wa.v[i].p.z * 2}, wb.v[i].p) < 1e-4);
    CHECK(view_of(b.mesh).nodes[0].scale[1] == doctest::Approx(2.0f));

    // The box's UV corner (0, 0) of the file sits at the bottom left: after the flip its V is 1 for the first corner.
    bool v0 = false, v1 = false;
    for (const WorldVertex& v : wa.v) {
        v0 = v0 || std::fabs(v.v) < 1e-3;
        v1 = v1 || std::fabs(v.v - 1.0) < 1e-3;
    }
    CHECK((v0 && v1));

    for (const char* bad : {"{\"up\": \"z\"}", "{\"scale\": 0}", "{\"scale\": -1}", "{\"sample_rate\": 0}", "{\"sample_rate\": 5000}", "{\"lod_levels\": 9}",
                            "{\"max_triangles\": 1.5}", "{\"lod_ratio\": 2}"}) {
        CAPTURE(bad);
        const Direct d = fbx_direct(fbx, bad);
        CHECK_FALSE(d.ok);
        CHECK(!d.diags.empty());
    }
}

TEST_CASE("fbx: lod_levels and max_triangles work") {
    // A 24 x 24 grid of quads (1152 triangles) with a bump, so the simplifier has something to keep.
    FbxDoc doc;
    std::vector<double> pos;
    std::vector<std::vector<long>> faces;
    const int n = 24;
    for (int y = 0; y <= n; ++y)
        for (int x = 0; x <= n; ++x) {
            pos.push_back(x * 0.1);
            pos.push_back(std::sin(x * 0.3) * std::cos(y * 0.3) * 0.2);
            pos.push_back(y * 0.1);
        }
    for (int y = 0; y < n; ++y)
        for (int x = 0; x < n; ++x) {
            const long a = y * (n + 1) + x;
            faces.push_back({a, a + 1, a + n + 2, a + n + 1});
        }
    const long geom = doc.geometry("Grid", pos, faces);
    const long node = doc.model("Grid", "Mesh", 0);
    doc.connect(geom, node);
    const std::string fbx = doc.str();

    const Cooked plain = cook_fbx("fbx_lod0", fbx);
    INFO(plain.text);
    REQUIRE(plain.report.ok());
    CHECK(view_of(plain.mesh).submeshes[0].index_count == 24 * 24 * 6);
    CHECK(view_of(plain.mesh).lods.empty());

    const Cooked lod = cook_fbx("fbx_lod2", fbx, "{\"lod_levels\": 2, \"lod_ratio\": 0.4, \"lod_error\": 0.5}");
    INFO(lod.text);
    REQUIRE(lod.report.ok());
    const MeshView lv = view_of(lod.mesh);
    CHECK_FALSE(lv.lods.empty());
    CHECK_FALSE(lv.submesh_lods(0).empty());
    CHECK(lv.submesh_lods(0)[0].index_count < lv.submeshes[0].index_count);

    const Cooked over = cook_fbx("fbx_budget", fbx, "{\"max_triangles\": 1000}");
    CHECK_FALSE(over.report.ok());
    CHECK(has(over.text, "max_triangles"));
    CHECK(has(over.text, "1152"));
    const Cooked fits = cook_fbx("fbx_budget_ok", fbx, "{\"max_triangles\": 1152}");
    CHECK(fits.report.ok());
}

// ---- skins and animation ----

namespace {

// Bones in a chain along the engine's +Y (1 m apart), a mesh with `verts` (engine coordinates) skinned to them.
struct SkinSpec {
    int bones = 3;
    std::vector<V3> verts;                            // engine metres
    std::vector<std::vector<long>> faces;
    std::vector<std::vector<std::pair<int, double>>> weights;  // per vertex: (bone, weight)
};

V3 to_file(V3 e, int up_axis, double unit_cm, bool left_handed = false) {
    const double k = 100.0 / unit_cm * (left_handed ? -1.0 : 1.0);  // a left-handed file stores x mirrored
    const double ky = 100.0 / unit_cm;
    return up_axis == 2 ? V3{e.x * k, -e.z * ky, e.y * ky} : V3{e.x * k, e.y * ky, e.z * ky};
}

M4 translation_matrix(V3 t) {
    M4 m;
    m.m[12] = t.x;
    m.m[13] = t.y;
    m.m[14] = t.z;
    return m;
}

SkinSpec tube_spec() {
    SkinSpec s;
    s.bones = 3;
    const double rx[4] = {-0.5, 0.5, 0.5, -0.5}, rz[4] = {-0.5, -0.5, 0.5, 0.5};
    const std::vector<std::pair<int, double>> ring_w[4] = {{{0, 1.0}}, {{0, 0.5}, {1, 0.5}}, {{1, 0.5}, {2, 0.5}}, {{2, 1.0}}};
    for (int r = 0; r < 4; ++r)
        for (int k = 0; k < 4; ++k) {
            s.verts.push_back({rx[k], static_cast<double>(r), rz[k]});
            s.weights.push_back(ring_w[r]);
        }
    for (int r = 0; r < 3; ++r)
        for (int k = 0; k < 4; ++k) {
            const long k1 = (k + 1) % 4;
            s.faces.push_back({r * 4 + k, r * 4 + k1, (r + 1) * 4 + k1, (r + 1) * 4 + k});
        }
    return s;
}

// The skinned file: Armature (null) > bone0 > bone1 > bone2, the mesh node beside the armature. Bone 1 rotates about X
// from 0 to 90 degrees over `length_s` seconds when `animate`.
std::string skin_fbx(const SkinSpec& s, int up_axis, double unit_cm, bool animate, int layers = 1, double length_s = 1.0, bool left_handed = false) {
    FbxDoc doc;
    doc.up_axis = up_axis;
    doc.unit_cm = unit_cm;
    doc.right_sign = left_handed ? -1 : 1;
    std::vector<double> pos;
    for (const V3& e : s.verts) {
        const V3 f = to_file(e, up_axis, unit_cm, left_handed);
        pos.insert(pos.end(), {f.x, f.y, f.z});
    }
    std::vector<std::vector<long>> faces = s.faces;
    if (left_handed)  // the clockwise winding of a left-handed exporter
        for (auto& f : faces) std::reverse(f.begin(), f.end());
    const long geom = doc.geometry("Tube", pos, faces);
    const long mesh_node = doc.model("Tube", "Mesh", 0);
    doc.connect(geom, mesh_node);
    const long armature = doc.model("Armature", "Null", 0);
    std::vector<long> bone;
    bone.reserve(static_cast<size_t>(s.bones));
    const V3 step = to_file({0, 1, 0}, up_axis, unit_cm);
    for (int b = 0; b < s.bones; ++b) bone.push_back(doc.limb("Bone" + std::to_string(b), b == 0 ? armature : bone[static_cast<size_t>(b) - 1], b == 0 ? V3{} : step));
    const long sk = doc.skin(geom);
    for (int b = 0; b < s.bones; ++b) {
        std::vector<long> idx;
        std::vector<double> w;
        for (size_t v = 0; v < s.weights.size(); ++v)
            for (const auto& bw : s.weights[v])
                if (bw.first == b) {
                    idx.push_back(static_cast<long>(v));
                    w.push_back(bw.second);
                }
        const V3 at = to_file({0, static_cast<double>(b), 0}, up_axis, unit_cm);
        // "Transform" is the mesh node -> bone matrix (ufbx's mesh_node_to_bone): inverse(bone bind) * mesh bind, the mesh at the origin.
        doc.cluster(sk, bone[static_cast<size_t>(b)], idx, w, translation_matrix({-at.x, -at.y, -at.z}), translation_matrix(at));
    }
    if (animate) {
        const long layer = doc.stack("Take 001", length_s, layers);
        doc.curve(layer, bone[1], "Lcl Rotation", 0, {{0.0, 0.0}, {length_s, 90.0}});
    }
    return doc.str();
}

// Skinned position of bind vertex `p` (engine space) with bone 1 rotated by `theta` about X (pivot (0,1,0)), per the weights.
V3 expected_pose(V3 p, const std::vector<std::pair<int, double>>& w, double theta) {
    const double c = std::cos(theta), s = std::sin(theta);
    const V3 r{p.x, 1 + (p.y - 1) * c - p.z * s, (p.y - 1) * s + p.z * c};  // bones 1 and 2 both carry the rotation
    V3 out;
    for (const auto& bw : w) {
        const V3& q = bw.first == 0 ? p : r;
        out = {out.x + bw.second * q.x, out.y + bw.second * q.y, out.z + bw.second * q.z};
    }
    return out;
}

}  // namespace

TEST_CASE("fbx: a 3-bone skin with a rotation clip is sampled to the analytic pose, in Y-up metres, Z-up centimetres and a left-handed file") {
    const SkinSpec spec = tube_spec();
    struct Case {
        const char* name;
        int up;
        double unit;
        bool left = false;
    };
    for (const Case& cs : {Case{"fbx_skin_y_m", 1, 100.0}, Case{"fbx_skin_z_cm", 2, 1.0}, Case{"fbx_skin_left", 1, 100.0, true}}) {
        CAPTURE(cs.name);
        const std::string fbx = skin_fbx(spec, cs.up, cs.unit, true, 1, 1.0, cs.left);
        const Cooked c = cook_fbx(cs.name, fbx);
        INFO(c.text);
        REQUIRE(c.report.ok());
        const MeshView m = view_of(c.mesh);
        REQUIRE(m.skinned());
        REQUIRE(m.skins.size() == 1);
        REQUIRE(m.skins[0].joint_count == 3);
        REQUIRE(m.nodes.size() == 5);  // Tube, Armature, Bone0..2 (depth first, the file's order)
        REQUIRE(m.submeshes.size() == 1);
        REQUIRE(m.clips.size() == 1);
        CHECK(m.clip_name(0) == "Take 001");
        CHECK(m.clips[0].duration == doctest::Approx(1.0f));
        REQUIRE(m.clips[0].channel_count == 1);  // only bone 1's rotation differs from the rest pose
        const MeshChannel& ch = m.channels[m.clips[0].first_channel];
        CHECK(m.node_name(ch.node) == "Bone1");
        CHECK(ch.path == AnimPath::Rotation);
        CHECK(ch.interp == AnimInterp::Linear);
        REQUIRE(ch.key_count == 31);  // 30 Hz over one second: keys 0 .. 30
        for (u32 k = 0; k < ch.key_count; ++k) {
            CAPTURE(k);
            CHECK(m.keys[ch.times_offset + k] == doctest::Approx(static_cast<float>(k) / 30.0f).epsilon(1e-5));
            const double theta = (k / 30.0) * (3.14159265358979323846 / 2);
            const float* q = &m.keys[ch.values_offset + k * 4];
            CHECK(q[0] == doctest::Approx(std::sin(theta / 2)).epsilon(1e-5).scale(1.0));
            CHECK(std::fabs(q[1]) < 1e-5);
            CHECK(std::fabs(q[2]) < 1e-5);
            CHECK(q[3] == doctest::Approx(std::cos(theta / 2)).epsilon(1e-5));
        }

        // The rest pose binds: every joint's world matrix times its inverse bind is the identity (the geometry is in bind space).
        const std::vector<M4> rest = world_matrices(m);
        for (u32 j = 0; j < 3; ++j) {
            const MeshJoint& mj = m.joints[m.skins[0].first_joint + j];
            M4 ibm;
            for (int i = 0; i < 16; ++i) ibm.m[i] = mj.inverse_bind[i];
            const M4 id = mul(rest[mj.node], ibm);
            for (int i = 0; i < 16; ++i) CHECK(id.m[i] == doctest::Approx(i % 5 == 0 ? 1.0 : 0.0).epsilon(1e-4).scale(1.0));
        }

        // Skin every vertex at several keys and compare with the closed form.
        const MeshSubmesh& sm = m.submeshes[0];
        REQUIRE(m.submesh_skins[0] == 0);
        for (u32 k : {0u, 7u, 15u, 30u}) {
            CAPTURE(k);
            std::vector<M4> local(m.nodes.size());
            for (size_t i = 0; i < m.nodes.size(); ++i) local[i] = node_local(m.nodes[i]);
            float t[3] = {m.nodes[ch.node].translation[0], m.nodes[ch.node].translation[1], m.nodes[ch.node].translation[2]};
            const float* q = &m.keys[ch.values_offset + k * 4];
            local[ch.node] = from_trs(t, q, m.nodes[ch.node].scale);
            const std::vector<M4> world = world_matrices(m, &local);
            std::vector<M4> palette(3);
            for (u32 j = 0; j < 3; ++j) {
                const MeshJoint& mj = m.joints[m.skins[0].first_joint + j];
                M4 ibm;
                for (int i = 0; i < 16; ++i) ibm.m[i] = mj.inverse_bind[i];
                palette[j] = mul(world[mj.node], ibm);
            }
            const double theta = (k / 30.0) * (3.14159265358979323846 / 2);
            int matched = 0;
            for (u32 i = 0; i < sm.vertex_count; ++i) {
                const V3 bind = bind_position(sm, m.vertices[sm.base_vertex + i]);
                const MeshSkinVertex& sv = m.skin_vertices[sm.base_vertex + i];
                V3 got;
                for (int s = 0; s < 4; ++s) {
                    if (!sv.weights[s]) continue;
                    const V3 p = point(palette[sv.joints[s]], bind);
                    const double w = sv.weights[s] / 255.0;
                    got = {got.x + w * p.x, got.y + w * p.y, got.z + w * p.z};
                }
                // Which source vertex is this? The bind position is the engine-space one (the geometry is in bind space).
                for (size_t v = 0; v < spec.verts.size(); ++v) {
                    if (dist(bind, spec.verts[v]) > 2e-3) continue;
                    CHECK(dist(got, expected_pose(spec.verts[v], spec.weights[v], theta)) < 0.02);
                    ++matched;
                }
            }
            CHECK(matched == 16);
        }

        // Bounds are the rest pose's, and the file's tube is 3 m tall in metres whatever the file's unit.
        CHECK(m.header->bounds_max[1] - m.header->bounds_min[1] == doctest::Approx(3.0f).epsilon(1e-3));

        // Same bytes twice.
        const Cooked again = cook_fbx((std::string(cs.name) + "_again").c_str(), fbx);
        REQUIRE(again.report.ok());
        CHECK_SAME_BYTES(c.mesh, again.mesh);
    }
}

TEST_CASE("fbx: sample_rate sets the key count; layers beyond the base warn") {
    const SkinSpec spec = tube_spec();
    const Cooked a = cook_fbx("fbx_rate_10", skin_fbx(spec, 1, 100.0, true), "{\"sample_rate\": 10}");
    INFO(a.text);
    REQUIRE(a.report.ok());
    const MeshView ma = view_of(a.mesh);
    REQUIRE(ma.channels.size() == 1);
    CHECK(ma.channels[0].key_count == 11);
    CHECK(ma.keys[ma.channels[0].times_offset + 10] == doctest::Approx(1.0f));
    CHECK(ma.keys[ma.channels[0].times_offset + 1] == doctest::Approx(0.1f));

    // A length that is not a whole number of samples: the last key is at the end.
    const Cooked b = cook_fbx("fbx_rate_odd", skin_fbx(spec, 1, 100.0, true, 1, 0.5), "{\"sample_rate\": 12}");
    REQUIRE(b.report.ok());
    const MeshView mb = view_of(b.mesh);
    CHECK(mb.channels[0].key_count == 7);  // 6 steps of 1/12 s
    CHECK(mb.clips[0].duration == doctest::Approx(0.5f));

    const Cooked c = cook_fbx("fbx_rate_odd2", skin_fbx(spec, 1, 100.0, true, 1, 0.55), "{\"sample_rate\": 12}");
    REQUIRE(c.report.ok());
    const MeshView mc = view_of(c.mesh);
    CHECK(mc.channels[0].key_count == 8);  // 0.55 s * 12 = 6.6 -> 7 steps, the last key at 0.55 s
    CHECK(mc.keys[mc.channels[0].times_offset + 7] == doctest::Approx(0.55f));
    CHECK(mc.keys[mc.channels[0].times_offset + 6] == doctest::Approx(0.5f));

    const Cooked layered = cook_fbx("fbx_layers", skin_fbx(spec, 1, 100.0, true, 2));
    INFO(layered.text);
    REQUIRE(layered.report.ok());
    CHECK(has(layered.text, "layers beyond the base layer"));
    CHECK(layered.report.diags.warning_count() >= 1);

    // A stack longer than the sampling limit is an error naming it.
    const Direct longer = fbx_direct(skin_fbx(spec, 1, 100.0, true, 1, 100000.0));
    CHECK_FALSE(longer.ok);
    CHECK(has(longer.diags, "samples per channel"));
}

TEST_CASE("fbx: more than 4 influences keep the 4 largest, renormalized, with a warning") {
    SkinSpec s;
    s.bones = 6;
    s.verts = {{0.7, 0.1, 0.3}, {0.0, 0.0, 0.0}, {1.0, 0.0, 0.0}};
    s.faces = {{0, 1, 2}};
    s.weights = {{{0, 0.4}, {1, 0.3}, {2, 0.15}, {3, 0.1}, {4, 0.04}, {5, 0.01}}, {{0, 1.0}}, {{0, 1.0}}};
    const Cooked c = cook_fbx("fbx_influences", skin_fbx(s, 1, 100.0, false));
    INFO(c.text);
    REQUIRE(c.report.ok());
    CHECK(has(c.text, "more than 4 skin influences"));
    const MeshView m = view_of(c.mesh);
    REQUIRE(m.skinned());
    REQUIRE(m.skins[0].joint_count == 6);
    const MeshSubmesh& sm = m.submeshes[0];
    int found = 0;
    for (u32 i = 0; i < sm.vertex_count; ++i) {
        const V3 p = bind_position(sm, m.vertices[sm.base_vertex + i]);
        if (dist(p, s.verts[0]) > 2e-3) continue;
        ++found;
        const MeshSkinVertex& sv = m.skin_vertices[sm.base_vertex + i];
        const int expect_joint[4] = {0, 1, 2, 3};
        const double expect_w[4] = {0.4 / 0.95, 0.3 / 0.95, 0.15 / 0.95, 0.1 / 0.95};
        int sum = 0;
        for (int k = 0; k < 4; ++k) {
            CHECK(sv.joints[k] == expect_joint[k]);
            CHECK(sv.weights[k] / 255.0 == doctest::Approx(expect_w[k]).epsilon(0.01).scale(1.0));
            sum += sv.weights[k];
        }
        CHECK(sum == 255);
    }
    CHECK(found == 1);
}

TEST_CASE("fbx: more than 256 joints is an error naming the cap") {
    SkinSpec s;
    s.bones = 257;
    s.verts = {{0, 0, 0}, {1, 0, 0}, {0, 0, 1}};
    s.faces = {{0, 1, 2}};
    s.weights = {{{0, 1.0}}, {{0, 1.0}}, {{0, 1.0}}};
    const Direct d = fbx_direct(skin_fbx(s, 1, 100.0, false));
    CHECK_FALSE(d.ok);
    CHECK(has(d.diags, "1..256 joints"));
}

// ---- materials and textures ----

namespace {

std::string prop(const char* name, const char* type, const std::string& values) {
    return std::string("\t\t\tP: \"") + name + "\", \"" + type + "\", \"\", \"A\"," + values + "\n";
}

// A Phong material with a few values, one box. `extra` adds textures to the material.
struct MatFile {
    std::string fbx;
};

}  // namespace

TEST_CASE("fbx: materials import inline, textures resolve to GUIDs, absolute paths fall back to the file name") {
    FbxDoc doc;
    const long geom = box(doc, {0.5, 0.5, 0.5}, false, all_same_material());
    const long node = doc.model("Cube", "Mesh", 0);
    doc.connect(geom, node);
    std::string props;
    props += prop("DiffuseColor", "Color", "0.8,0.4,0.2");
    props += prop("EmissiveColor", "Color", "0.1,0.2,0.3");
    props += prop("EmissiveFactor", "Number", "2");
    props += prop("TransparencyFactor", "Number", "0.5");  // FBX stores transparency: opacity 0.5
    const long mat = doc.material("Painted", props);
    doc.connect(mat, node);
    const long base = doc.texture("Base", "C:\\Users\\artist\\work\\tex.png", "tex.png");
    doc.connect_prop(base, mat, "DiffuseColor");
    const long normal = doc.texture("Normal", "C:\\Users\\artist\\work\\nrm.png", "..\\..\\..\\nrm.png");  // relative path escapes assets/
    doc.connect_prop(normal, mat, "NormalMap");

    const std::vector<std::pair<std::string, std::string>> files = {
        {"models/tex.png", make_gradient_png(4, 4)},
        {"models/tex.png.mceasset", sidecar(kTexGuid, "texture")},
        {"models/nrm.png", make_gradient_png(4, 4, 9)},
        {"models/nrm.png.mceasset", sidecar(kTex2Guid, "texture")},
    };
    const Cooked c = cook_fbx("fbx_material", doc.str(), "{}", files);
    INFO(c.text);
    REQUIRE(c.report.ok());
    CHECK(has(c.text, "absolute or outside assets/"));  // the normal map: read as the file name next to the model
    const MeshView m = view_of(c.mesh);
    REQUIRE(m.submeshes.size() == 1);
    const MaterialChunk& mc = m.submeshes[0].material;
    // The base colour map is the colour: DiffuseColor is not multiplied in (the next test covers a material without a map).
    CHECK(mc.base_color[0] == doctest::Approx(1.0f));
    CHECK(mc.base_color[1] == doctest::Approx(1.0f));
    CHECK(mc.base_color[2] == doctest::Approx(1.0f));
    CHECK(mc.base_color[3] == doctest::Approx(0.5f).epsilon(1e-3));
    CHECK((mc.flags & MaterialFlags::AlphaBlend) != 0);
    CHECK(mc.emissive[0] == doctest::Approx(0.2f).epsilon(1e-3));
    CHECK(mc.emissive[2] == doctest::Approx(0.6f).epsilon(1e-3));
    CHECK(mc.texture(MaterialSlot::BaseColor) == G(kTexGuid));
    CHECK(mc.texture(MaterialSlot::Normal) == G(kTex2Guid));
    CHECK(mc.texture(MaterialSlot::Emissive).is_null());

    // The same file twice: identical bytes.
    const Cooked again = cook_fbx("fbx_material_again", doc.str(), "{}", files);
    REQUIRE(again.report.ok());
    CHECK_SAME_BYTES(c.mesh, again.mesh);

    // A texture that is not a registered asset is an error naming the image.
    const Cooked missing = cook_fbx("fbx_material_missing", doc.str(), "{}", {});
    CHECK_FALSE(missing.report.ok());
    CHECK(has(missing.text, "tex.png"));
    CHECK(has(missing.text, "not a registered asset"));
}

TEST_CASE("fbx: a material without values, and one slot per submesh") {
    // Two materials on a box: faces 0-2 use slot 0, faces 3-5 slot 1 -> two submeshes in slot order.
    FbxDoc doc;
    std::string layer =
        "\t\tLayerElementMaterial: 0 {\n\t\t\tVersion: 101\n\t\t\tName: \"\"\n\t\t\tMappingInformationType: \"ByPolygon\"\n"
        "\t\t\tReferenceInformationType: \"IndexToDirect\"\n\t\t\tMaterials: *6 {\n\t\t\t\ta: 0,0,0,1,1,1\n\t\t\t}\n\t\t}\n";
    const long geom = box(doc, {0.5, 0.5, 0.5}, false, layer);
    const long node = doc.model("Cube", "Mesh", 0);
    doc.connect(geom, node);
    doc.connect(doc.material("Red", prop("DiffuseColor", "Color", "1,0,0")), node);
    doc.connect(doc.material("Blue", prop("DiffuseColor", "Color", "0,0,1")), node);
    const Cooked c = cook_fbx("fbx_two_mats", doc.str());
    INFO(c.text);
    REQUIRE(c.report.ok());
    const MeshView m = view_of(c.mesh);
    REQUIRE(m.submeshes.size() == 2);
    CHECK(m.submeshes[0].material.base_color[0] == doctest::Approx(1.0f));
    CHECK(m.submeshes[0].material.base_color[2] == doctest::Approx(0.0f));
    CHECK(m.submeshes[1].material.base_color[2] == doctest::Approx(1.0f));
    CHECK(m.submeshes[0].index_count == 18);
    CHECK(m.submeshes[1].index_count == 18);
}

TEST_CASE("fbx: an embedded texture is an error that says to export with external textures") {
    FbxDoc doc;
    const long geom = box(doc, {0.5, 0.5, 0.5}, false, all_same_material());
    const long node = doc.model("Cube", "Mesh", 0);
    doc.connect(geom, node);
    const long mat = doc.material("Painted", prop("DiffuseColor", "Color", "1,1,1"));
    doc.connect(mat, node);
    const std::string png = make_gradient_png(4, 4);
    const long tex = doc.texture("Base", "C:\\art\\tex.png", "tex.png", base64(png));
    doc.connect_prop(tex, mat, "DiffuseColor");
    const std::vector<std::pair<std::string, std::string>> files = {{"models/tex.png", png}, {"models/tex.png.mceasset", sidecar(kTexGuid, "texture")}};
    const Cooked c = cook_fbx("fbx_embedded", doc.str(), "{}", files);
    CHECK_FALSE(c.report.ok());
    INFO(c.text);
    CHECK(has(c.text, "export with external textures"));
}

TEST_CASE("fbx: unsupported features warn once with a count and the import goes on") {
    FbxDoc doc;
    const long geom = box(doc, {0.5, 0.5, 0.5});
    const long node = doc.model("Cube", "Mesh", 0);
    doc.connect(geom, node);
    // Hidden mesh, a light, a camera-like null are fine; the hidden one is skipped with a warning.
    const long geom2 = box(doc, {0.25, 0.25, 0.25});
    const long hidden = doc.model("Hidden", "Mesh", 0, {1, 0, 0}, {}, {1, 1, 1}, prop("Visibility", "Visibility", "0"));
    doc.connect(geom2, hidden);
    const long light = doc.id();
    doc.objects += "\tNodeAttribute: " + std::to_string(light) + ", \"NodeAttribute::\", \"Light\" {\n\t\tTypeFlags: \"Light\"\n\t}\n";
    const long lnode = doc.model("Lamp", "Light", 0);
    doc.connect(light, lnode);
    const Cooked c = cook_fbx("fbx_warnings", doc.str());
    INFO(c.text);
    REQUIRE(c.report.ok());
    CHECK(has(c.text, "hidden meshes"));
    CHECK(has(c.text, "not imported"));
    const MeshView m = view_of(c.mesh);
    CHECK(m.submeshes.size() == 1);  // the hidden mesh is skipped
}

TEST_CASE("fbx: the importer is registered for .fbx and declares its textures as dependencies") {
    cook::ImporterRegistry reg;
    cook::ImporterRegistry::add_builtins(reg);
    const cook::IImporter* imp = reg.for_extension(".fbx");
    REQUIRE(imp != nullptr);
    CHECK(imp->id() == "fbx");
    CHECK(imp->type() == ChunkType::Mesh);
    CHECK_FALSE(imp->native());
    CHECK(reg.find("fbx") == imp);
}

// ---- corrupt input ----

namespace {

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

// Errors or succeeds with a mesh the reader accepts; a crash is the failure.
int cook_and_validate(const std::string& bytes) {
    const Direct d = fbx_direct(bytes);
    if (!d.ok) {
        CHECK(!d.diags.empty());
        return 1;
    }
    // The cooked container must read back.
    auto c = ContainerView::open(Span<const byte>(d.bytes.data(), d.bytes.size()));
    CHECK(c.has_value());
    if (c.has_value()) {
        const u32 chunk = c.value().find(ChunkType::Mesh);
        CHECK(chunk < c.value().chunk_count());
        if (chunk < c.value().chunk_count()) CHECK(MeshView::read(c.value().payload(chunk)).has_value());
    }
    return 0;
}

// Binary FBX 7.4 writer, just enough for a node tree with an array property (32-bit offsets below version 7500).
struct BinaryFbx {
    std::string bytes;
    BinaryFbx() {
        bytes.append("Kaydara FBX Binary  \0", 21);
        bytes.push_back('\x1a');
        bytes.push_back('\0');
        u32(7400);
    }
    void u32(uint32_t v) {
        for (int i = 0; i < 4; ++i) bytes.push_back(static_cast<char>((v >> (8 * i)) & 0xffu));
    }
    void patch_u32(size_t at, uint32_t v) {
        for (int i = 0; i < 4; ++i) bytes[at + static_cast<size_t>(i)] = static_cast<char>((v >> (8 * i)) & 0xffu);
    }
    template <class Props>
    size_t begin(const char* name, Props props) {
        const size_t node = bytes.size();
        u32(0);
        u32(0);
        u32(0);
        bytes.push_back(static_cast<char>(std::strlen(name)));
        bytes.append(name);
        const size_t prop_start = bytes.size();
        const uint32_t count = props();
        patch_u32(node + 4, count);
        patch_u32(node + 8, static_cast<uint32_t>(bytes.size() - prop_start));
        return node;
    }
    void end(size_t node, bool has_children) {
        if (has_children) bytes.append(13, '\0');
        patch_u32(node, static_cast<uint32_t>(bytes.size()));
    }
    void double_array_claim(uint32_t count, uint32_t encoding, const std::string& payload) {
        bytes.push_back('d');
        u32(count);
        u32(encoding);
        u32(static_cast<uint32_t>(payload.size()));
        bytes.append(payload);
    }
    void finish() { bytes.append(13, '\0'); }
};

std::string binary_huge_vertices(uint32_t encoding) {
    BinaryFbx f;
    const size_t objects = f.begin("Objects", [] { return 0u; });
    const size_t geom = f.begin("Geometry", [&f] {
        f.bytes.push_back('L');
        for (int i = 0; i < 8; ++i) f.bytes.push_back(i == 0 ? '\x01' : '\0');
        return 1u;
    });
    const std::string zlib("\x78\x9c\x03\x00\x00\x00\x00\x01", 8);
    const std::string raw(16, '\0');
    const size_t verts = f.begin("Vertices", [&] {
        f.double_array_claim(0x20000000u, encoding, encoding == 1 ? zlib : raw);
        return 1u;
    });
    f.end(verts, false);
    f.end(geom, true);
    f.end(objects, true);
    f.finish();
    return f.bytes;
}

// Rewrites the ASCII fixtures this file writes as a valid binary FBX 7.4 (32-bit offsets, raw arrays, no footer): ids and
// key times are int64, integers int32, other numbers doubles (P-node values by their declared type), arrays by name, and
// "Type::Name" object names as "Name\x00\x01Type". Handles only the grammar FbxDoc emits (no embedded Content).
class AsciiToBinary {
public:
    explicit AsciiToBinary(const std::string& t) : t_(t) {}

    std::string run() {
        out_.append("Kaydara FBX Binary  \0", 21);
        out_.push_back('\x1a');
        out_.push_back('\0');
        u32(7400);
        for (;;) {
            skip_ws();
            if (pos_ >= t_.size()) break;
            node("");
        }
        out_.append(13, '\0');
        return out_;
    }

private:
    struct Arg {
        char kind = 'S';  // S string, N number, A array marker
        std::string s;
        bool integer = false;
    };

    const std::string& t_;
    size_t pos_ = 0;
    std::string out_;

    void u32(uint32_t v) {
        for (int i = 0; i < 4; ++i) out_.push_back(static_cast<char>((v >> (8 * i)) & 0xffu));
    }
    void u64v(uint64_t v) {
        for (int i = 0; i < 8; ++i) out_.push_back(static_cast<char>((v >> (8 * i)) & 0xffu));
    }
    void patch(size_t at, uint32_t v) {
        for (int i = 0; i < 4; ++i) out_[at + static_cast<size_t>(i)] = static_cast<char>((v >> (8 * i)) & 0xffu);
    }
    void skip_ws() {
        while (pos_ < t_.size()) {
            const char c = t_[pos_];
            if (c == ';') {
                while (pos_ < t_.size() && t_[pos_] != '\n') ++pos_;
            } else if (c == ' ' || c == '\t' || c == '\r' || c == '\n') {
                ++pos_;
            } else {
                break;
            }
        }
    }

    static bool is_obj(const std::string& n) {
        for (const char* k : {"Model", "Geometry", "NodeAttribute", "Deformer", "AnimationStack", "AnimationLayer", "AnimationCurveNode", "AnimationCurve",
                              "Material", "Texture", "Video"})
            if (n == k) return true;
        return false;
    }

    void put_number(char kind, const std::string& text) {
        out_.push_back(kind);
        if (kind == 'I') u32(static_cast<uint32_t>(static_cast<int32_t>(std::strtol(text.c_str(), nullptr, 10))));
        else if (kind == 'L') u64v(static_cast<uint64_t>(std::strtoll(text.c_str(), nullptr, 10)));
        else {
            const double d = std::strtod(text.c_str(), nullptr);
            uint64_t b;
            std::memcpy(&b, &d, 8);
            u64v(b);
        }
    }

    void node(const std::string& parent) {
        size_t b = pos_;
        while (pos_ < t_.size() && (std::isalnum(static_cast<unsigned char>(t_[pos_])) || t_[pos_] == '_')) ++pos_;
        const std::string name = t_.substr(b, pos_ - b);
        REQUIRE((!name.empty() && pos_ < t_.size() && t_[pos_] == ':'));
        ++pos_;
        std::vector<Arg> args;
        bool block = false;
        for (;;) {
            while (pos_ < t_.size() && (t_[pos_] == ' ' || t_[pos_] == '\t' || t_[pos_] == '\r')) ++pos_;
            if (pos_ >= t_.size() || t_[pos_] == '\n') break;
            const char c = t_[pos_];
            if (c == '{') {
                block = true;
                ++pos_;
                break;
            }
            if (c == ',') {
                ++pos_;
                continue;
            }
            Arg a;
            if (c == '"') {
                const size_t e = t_.find('"', pos_ + 1);
                REQUIRE(e != std::string::npos);
                a.s = t_.substr(pos_ + 1, e - pos_ - 1);
                pos_ = e + 1;
            } else if (c == '*') {
                a.kind = 'A';
                ++pos_;
                while (pos_ < t_.size() && std::isdigit(static_cast<unsigned char>(t_[pos_]))) ++pos_;
            } else {
                a.kind = 'N';
                b = pos_;
                while (pos_ < t_.size() && t_[pos_] != ',' && t_[pos_] != ' ' && t_[pos_] != '\t' && t_[pos_] != '\r' && t_[pos_] != '\n' && t_[pos_] != '{') ++pos_;
                a.s = t_.substr(b, pos_ - b);
                a.integer = a.s.find_first_of(".eEn") == std::string::npos;
            }
            args.push_back(a);
        }

        const size_t start = out_.size();
        u32(0);
        u32(0);
        u32(0);
        out_.push_back(static_cast<char>(name.size()));
        out_ += name;
        const size_t props_at = out_.size();
        uint32_t count = 0;
        const bool array_node = !args.empty() && args.back().kind == 'A';
        for (size_t i = 0; i < args.size(); ++i) {
            const Arg& a = args[i];
            if (a.kind == 'A') continue;
            ++count;
            if (a.kind == 'S') {
                std::string v = a.s;
                const size_t sep = v.find("::");
                if (parent == "Objects" && i == 1 && sep != std::string::npos) v = v.substr(sep + 2) + std::string("\x00\x01", 2) + v.substr(0, sep);
                out_.push_back('S');
                u32(static_cast<uint32_t>(v.size()));
                out_ += v;
                continue;
            }
            char kind = a.integer ? 'I' : 'D';
            if (parent == "Objects" && i == 0 && is_obj(name)) kind = 'L';
            else if (parent == "Connections") kind = 'L';
            else if (name == "P" && i >= 4) {
                const std::string& type = args[1].s;
                kind = (type == "int" || type == "enum" || type == "bool") ? 'I' : type == "KTime" ? 'L' : 'D';
            } else if (name == "Default") kind = 'D';
            put_number(kind, a.s);
        }
        if (array_node) {
            // "{ a: n, n, ... }" (the brace is already consumed)
            skip_ws();
            REQUIRE(t_.compare(pos_, 2, "a:") == 0);
            pos_ += 2;
            std::vector<std::string> v;
            for (;;) {
                while (pos_ < t_.size() && (t_[pos_] == ' ' || t_[pos_] == '\t' || t_[pos_] == '\r' || t_[pos_] == '\n' || t_[pos_] == ',')) ++pos_;
                if (pos_ >= t_.size() || t_[pos_] == '}') break;
                b = pos_;
                while (pos_ < t_.size() && t_[pos_] != ',' && t_[pos_] != '}' && t_[pos_] != '\n' && t_[pos_] != ' ') ++pos_;
                v.push_back(t_.substr(b, pos_ - b));
            }
            REQUIRE((pos_ < t_.size() && t_[pos_] == '}'));
            ++pos_;
            char kind = 'd';
            for (const char* k : {"PolygonVertexIndex", "UVIndex", "Indexes", "Materials", "KeyAttrFlags", "KeyAttrRefCount"})
                if (name == k) kind = 'i';
            if (name == "KeyTime") kind = 'l';
            if (name == "KeyValueFloat" || name == "KeyAttrDataFloat") kind = 'f';
            out_.push_back(kind);
            u32(static_cast<uint32_t>(v.size()));
            u32(0);
            u32(static_cast<uint32_t>(v.size() * (kind == 'i' || kind == 'f' ? 4 : 8)));
            for (const std::string& x : v) {
                if (kind == 'i') u32(static_cast<uint32_t>(static_cast<int32_t>(std::strtol(x.c_str(), nullptr, 10))));
                else if (kind == 'l') u64v(static_cast<uint64_t>(std::strtoll(x.c_str(), nullptr, 10)));
                else if (kind == 'f') {
                    const float f = std::strtof(x.c_str(), nullptr);
                    uint32_t bits;
                    std::memcpy(&bits, &f, 4);
                    u32(bits);
                } else {
                    const double d = std::strtod(x.c_str(), nullptr);
                    uint64_t bits;
                    std::memcpy(&bits, &d, 8);
                    u64v(bits);
                }
            }
            ++count;
        }
        patch(start + 4, count);
        patch(start + 8, static_cast<uint32_t>(out_.size() - props_at));
        bool children = false;
        if (block && !array_node) {
            for (;;) {
                skip_ws();
                REQUIRE(pos_ < t_.size());
                if (t_[pos_] == '}') {
                    ++pos_;
                    break;
                }
                children = true;
                node(name);
            }
        }
        if (children) out_.append(13, '\0');
        patch(start, static_cast<uint32_t>(out_.size()));
    }
};

std::string to_binary(const std::string& ascii) { return AsciiToBinary(ascii).run(); }

}  // namespace

TEST_CASE("fbx: truncations of a valid file error or succeed, never crash") {
    const std::string fbx = box_fbx({0.5, 1.0, 2.0}, 2, 1.0);
    int errors = 0;
    for (size_t n = 0; n < fbx.size(); n += 1) errors += cook_and_validate(fbx.substr(0, n));
    CHECK(errors > static_cast<int>(fbx.size() / 2));
    // The skinned and animated file, in coarser steps, as ASCII and as binary.
    const std::string skin = skin_fbx(tube_spec(), 2, 1.0, true);
    for (size_t n = 0; n < skin.size(); n += 37) cook_and_validate(skin.substr(0, n));
    const std::string bin = to_binary(skin);
    for (size_t n = 0; n < bin.size(); n += 23) cook_and_validate(bin.substr(0, n));
}

TEST_CASE("fbx: seeded byte flips of valid files error or succeed, never crash") {
    const std::string files[] = {box_fbx({0.5, 1.0, 2.0}, 2, 1.0), skin_fbx(tube_spec(), 2, 1.0, true), skin_fbx(tube_spec(), 1, 100.0, true, 2)};
    Rng rng{0x9e3779b97f4a7c15ull};
    int round = 0;
    for (const std::string& base : files) {
        for (int i = 0; i < 300; ++i, ++round) {
            std::string m = base;
            const size_t flips = 1 + rng.below(8);
            for (size_t f = 0; f < flips; ++f) {
                const size_t at = rng.below(m.size());
                switch (rng.below(3)) {
                    case 0: m[at] = static_cast<char>(rng.next() & 0xFF); break;
                    case 1: m[at] = static_cast<char>(m[at] ^ (1 << rng.below(8))); break;
                    default: m[at] = "0123456789-.e,:*{}\""[rng.below(19)]; break;
                }
            }
            cook_and_validate(m);
        }
    }
    CHECK(round == 900);
    // A flip at every byte of a small binary file (the binary parser's turn).
    const std::string bin = binary_huge_vertices(1);
    for (size_t i = 0; i < bin.size(); ++i) {
        std::string m = bin;
        m[i] = static_cast<char>(m[i] ^ 0x5a);
        cook_and_validate(m);
    }
    // A valid binary file with a mesh, a skin and an animation: the same cook as the ASCII one, then flips along the whole
    // file (a bounded number of positions, two patterns each), every result an error or a mesh the reader accepts.
    const auto t0 = std::chrono::steady_clock::now();
    const std::string rich_ascii = skin_fbx(tube_spec(), 2, 1.0, true);
    const std::string rich = to_binary(rich_ascii);
    const Direct from_ascii = fbx_direct(rich_ascii), from_binary = fbx_direct(rich);
    INFO(from_binary.diags);
    REQUIRE(from_ascii.ok);
    REQUIRE(from_binary.ok);
    CHECK_SAME_BYTES(from_ascii.bytes, from_binary.bytes);
    const size_t stride = rich.size() / 1500 + 1;
    int flips = 0, rejected = 0;
    for (size_t i = 0; i < rich.size(); i += stride) {
        for (int pattern = 0; pattern < 2; ++pattern) {
            std::string m = rich;
            m[i] = pattern == 0 ? static_cast<char>(m[i] ^ 0x5a) : static_cast<char>(0x7f);
            rejected += cook_and_validate(m);
            ++flips;
        }
    }
    CHECK(flips >= 1000);
    CHECK(rejected > 0);
    const double seconds = std::chrono::duration<double>(std::chrono::steady_clock::now() - t0).count();
    INFO("binary flips took " << seconds << " s");
    CHECK(seconds < 15.0);
}

TEST_CASE("fbx: files that claim huge arrays, huge polygons or deep nesting error without allocating the claim") {
    // A binary file claiming 2^29 doubles (4 GiB), zlib-packed and raw.
    for (uint32_t enc : {1u, 0u}) {
        const Direct d = fbx_direct(binary_huge_vertices(enc));
        CHECK_FALSE(d.ok);
        CHECK(!d.diags.empty());
    }
    // ASCII counts are hints: the values present are read.
    {
        std::string fbx = box_fbx({0.5, 0.5, 0.5}, 1, 100.0);
        const std::string from = "Vertices: *24 {";
        const size_t at = fbx.find(from);
        REQUIRE(at != std::string::npos);
        fbx.replace(at, from.size(), "Vertices: *2000000000 {");
        const Direct d = fbx_direct(fbx);
        CHECK((d.ok || !d.diags.empty()));  // ok or an error, never a 16 GB allocation
    }
    // One polygon with 64K + 8 corners.
    {
        FbxDoc doc;
        std::vector<long> face;
        for (long i = 0; i < (1l << 16) + 8; ++i) face.push_back(i % 3);
        const long geom = doc.geometry("Fan", {0, 0, 0, 1, 0, 0, 0, 0, 1}, {face});
        doc.connect(geom, doc.model("Fan", "Mesh", 0));
        const Direct d = fbx_direct(doc.str());
        CHECK_FALSE(d.ok);
        CHECK(has(d.diags, "polygon over the importer's limit"));
    }
    // Node nesting: 3000 deep is fine (the tree is walked with a stack), 5000 is over the depth cap.
    for (int depth : {3000, 5000}) {
        CAPTURE(depth);
        FbxDoc doc;
        long parent = 0;
        for (int i = 0; i < depth; ++i) parent = doc.model("n", "Null", parent, {0.001, 0, 0});
        const Direct d = fbx_direct(doc.str());
        if (depth == 3000) {
            INFO(d.diags);
            CHECK(d.ok);
        } else {
            CHECK_FALSE(d.ok);
            CHECK(has(d.diags, "deeper than the importer's limit of 4096"));
        }
    }
    // Empty and non-FBX input.
    CHECK_FALSE(fbx_direct("").ok);
    CHECK_FALSE(fbx_direct("v 0 0 0\nv 1 0 0\nv 0 1 0\nf 1 2 3\n").ok);  // an OBJ is not sniffed
    CHECK_FALSE(fbx_direct(std::string(4096, '\0')).ok);
}

TEST_CASE("fbx: non-finite and absurd transforms and vertices are errors") {
    auto box_with_translation = [](const char* t) {
        FbxDoc doc;
        const long geom = box(doc, {0.5, 0.5, 0.5});
        const long node = doc.model("Cube", "Mesh", 0);
        doc.connect(geom, node);
        const std::string needle = "\"Lcl Translation\", \"Lcl Translation\", \"\", \"A\",0,0,0";
        std::string s = doc.str();
        const size_t at = s.find(needle);
        REQUIRE(at != std::string::npos);
        s.replace(at, needle.size(), std::string("\"Lcl Translation\", \"Lcl Translation\", \"\", \"A\",") + t);
        return s;
    };
    for (const char* t : {"1e999,0,0", "1e30,0,0", "1e300,0,0", "-1e12,0,5", "nan,0,0", "inf,0,0", "-nan,1,1"}) {
        CAPTURE(t);
        const Direct d = fbx_direct(box_with_translation(t));
        if (d.ok) {  // a parser that read it as something finite and small: fine, the output must still read back
            auto c = ContainerView::open(Span<const byte>(d.bytes.data(), d.bytes.size()));
            CHECK(c.has_value());
        } else {
            CHECK(!d.diags.empty());
        }
    }
    // Over 1e9 is always an error, never a silently clamped value.
    {
        const Direct d = fbx_direct(box_with_translation("1e12,0,0"));
        CHECK_FALSE(d.ok);
        CHECK(has(d.diags, "1e9"));
    }
    // A vertex over the limit.
    {
        FbxDoc doc;
        const long geom = doc.geometry("Far", {0, 0, 0, 1e12, 0, 0, 0, 0, 1}, {{0, 1, 2}});
        doc.connect(geom, doc.model("Far", "Mesh", 0));
        const Direct d = fbx_direct(doc.str());
        CHECK_FALSE(d.ok);
        CHECK(has(d.diags, "1e9"));
    }
    // A non-finite animation key.
    {
        FbxDoc doc;
        const long node = doc.model("Mover", "Null", 0);
        const long layer = doc.stack("Take", 1.0);
        doc.curve(layer, node, "Lcl Translation", 0, {{0.0, 0.0}, {1.0, 1e15}});
        const Direct d = fbx_direct(doc.str());
        CHECK_FALSE(d.ok);
        CHECK(has(d.diags, "is not finite or is over the importer's limit"));
    }
}

// ---- review fixes: opacity, maps, budgets ----

namespace {

struct Quat {
    double x = 0, y = 0, z = 0, w = 1;
};
Quat qmul(Quat a, Quat b) {
    return {a.w * b.x + a.x * b.w + a.y * b.z - a.z * b.y, a.w * b.y - a.x * b.z + a.y * b.w + a.z * b.x,
            a.w * b.z + a.x * b.y - a.y * b.x + a.z * b.w, a.w * b.w - a.x * b.x - a.y * b.y - a.z * b.z};
}
Quat qconj(Quat a) { return {-a.x, -a.y, -a.z, a.w}; }

// The material of a one-box file whose material has `props` and the textures named in `maps` (property name -> texture asset).
MaterialChunk cook_material(const char* name, const std::string& props, const std::vector<const char*>& maps = {}) {
    FbxDoc doc;
    const long geom = box(doc, {0.5, 0.5, 0.5}, false, all_same_material());
    const long node = doc.model("Cube", "Mesh", 0);
    doc.connect(geom, node);
    const long mat = doc.material("M", props);
    doc.connect(mat, node);
    for (const char* property : maps) {
        const long tex = doc.texture(std::string("T") + property, "C:\\art\\tex.png", "tex.png");
        doc.connect_prop(tex, mat, property);
    }
    const std::vector<std::pair<std::string, std::string>> files = {{"models/tex.png", make_gradient_png(4, 4)},
                                                                    {"models/tex.png.mceasset", sidecar(kTexGuid, "texture")}};
    const Cooked c = cook_fbx(name, doc.str(), "{}", files);
    INFO(c.text);
    REQUIRE(c.report.ok());
    const MeshView m = view_of(c.mesh);
    REQUIRE(m.submeshes.size() == 1);
    return m.submeshes[0].material;
}

}  // namespace

TEST_CASE("fbx: plain FBX opacity: Maya's TransparencyFactor 1 with a black TransparentColor is opaque") {
    // Maya / Max: opaque is TransparentColor 0 0 0 whatever the factor.
    {
        const MaterialChunk mc = cook_material("fbx_opaque_maya", prop("DiffuseColor", "Color", "0.5,0.5,0.5") + prop("TransparentColor", "Color", "0,0,0") +
                                                                       prop("TransparencyFactor", "Number", "1"));
        CHECK(mc.base_color[3] == doctest::Approx(1.0f));
        CHECK((mc.flags & MaterialFlags::AlphaBlend) == 0);
    }
    // Genuinely transparent: the colour scaled by the factor.
    {
        const MaterialChunk mc = cook_material("fbx_transparent_half", prop("TransparentColor", "Color", "1,1,1") + prop("TransparencyFactor", "Number", "0.5"));
        CHECK(mc.base_color[3] == doctest::Approx(0.5f));
        CHECK((mc.flags & MaterialFlags::AlphaBlend) != 0);
    }
    {
        const MaterialChunk mc = cook_material("fbx_transparent_gray", prop("TransparentColor", "Color", "0.25,0.25,0.25") + prop("TransparencyFactor", "Number", "1"));
        CHECK(mc.base_color[3] == doctest::Approx(0.75f));
        CHECK((mc.flags & MaterialFlags::AlphaBlend) != 0);
    }
    // The Opacity property wins when the file has one.
    {
        const MaterialChunk mc = cook_material("fbx_opacity_prop", prop("Opacity", "Number", "0.5") + prop("TransparentColor", "Color", "0,0,0") +
                                                                       prop("TransparencyFactor", "Number", "1"));
        CHECK(mc.base_color[3] == doctest::Approx(0.5f));
        CHECK((mc.flags & MaterialFlags::AlphaBlend) != 0);
    }
    // A factor alone is the transparency.
    {
        const MaterialChunk mc = cook_material("fbx_transparency_only", prop("TransparencyFactor", "Number", "0.25"));
        CHECK(mc.base_color[3] == doctest::Approx(0.75f));
    }
    // No transparency at all: opaque.
    {
        const MaterialChunk mc = cook_material("fbx_no_transparency", prop("DiffuseColor", "Color", "1,0,0"));
        CHECK(mc.base_color[3] == doctest::Approx(1.0f));
        CHECK((mc.flags & MaterialFlags::AlphaBlend) == 0);
    }
}

TEST_CASE("fbx: a map on a plain FBX colour is the colour: the colour property is not multiplied in") {
    // No map: colour times factor.
    {
        const MaterialChunk mc = cook_material("fbx_color_factor", prop("DiffuseColor", "Color", "0.5,0.5,0.5") + prop("DiffuseFactor", "Number", "0.8"));
        CHECK(mc.base_color[0] == doctest::Approx(0.4f));
    }
    // A base map: the map is the colour, only the explicit factor scales it.
    {
        const MaterialChunk mc = cook_material("fbx_map_factor", prop("DiffuseColor", "Color", "0.5,0.5,0.5") + prop("DiffuseFactor", "Number", "0.8"), {"DiffuseColor"});
        for (int c = 0; c < 3; ++c) CHECK(mc.base_color[c] == doctest::Approx(0.8f));
        CHECK(mc.texture(MaterialSlot::BaseColor) == G(kTexGuid));
    }
    {
        const MaterialChunk mc = cook_material("fbx_map_only", prop("DiffuseColor", "Color", "0.5,0.5,0.5"), {"DiffuseColor"});
        for (int c = 0; c < 3; ++c) CHECK(mc.base_color[c] == doctest::Approx(1.0f));
    }
    // Maya writes EmissiveColor 0 next to an emission map: the map must show.
    {
        const MaterialChunk mc = cook_material("fbx_emission_map", prop("EmissiveColor", "Color", "0,0,0"), {"EmissiveColor"});
        for (int c = 0; c < 3; ++c) CHECK(mc.emissive[c] == doctest::Approx(1.0f));
        CHECK(mc.texture(MaterialSlot::Emissive) == G(kTexGuid));
    }
    {
        const MaterialChunk mc = cook_material("fbx_emission_map_factor", prop("EmissiveColor", "Color", "0,0,0") + prop("EmissiveFactor", "Number", "2.5"), {"EmissiveColor"});
        for (int c = 0; c < 3; ++c) CHECK(mc.emissive[c] == doctest::Approx(2.5f));
    }
    // An absurd emission is capped, never a float overflow.
    {
        const MaterialChunk mc = cook_material("fbx_emission_huge", prop("EmissiveColor", "Color", "1e30,1,1") + prop("EmissiveFactor", "Number", "1e30"));
        CHECK(std::isfinite(mc.emissive[0]));
        CHECK(mc.emissive[0] <= 1e6f);
    }
}

TEST_CASE("fbx: a Z-up centimetre clip on a root node is in metres, turns about the engine's Y, and static roots get no channel") {
    FbxDoc doc;
    doc.up_axis = 2;
    doc.unit_cm = 1;
    const long geom = box(doc, {50, 50, 50});  // file units: centimetres
    const long mover = doc.model("Mover", "Mesh", 0);
    doc.connect(geom, mover);
    doc.model("Still", "Null", 0, {10, 20, 30});  // cm, file axes
    const long layer = doc.stack("Move", 1.0);
    doc.curve(layer, mover, "Lcl Translation", 1, {{0.0, 0.0}, {1.0, 100.0}});  // file +Y, 100 cm
    doc.curve(layer, mover, "Lcl Rotation", 2, {{0.0, 0.0}, {1.0, 90.0}});      // about the file's Z
    const Cooked c = cook_fbx("fbx_zup_root_clip", doc.str());
    INFO(c.text);
    REQUIRE(c.report.ok());
    const MeshView m = view_of(c.mesh);
    u32 mover_node = ~0u, still_node = ~0u;
    for (u32 i = 0; i < m.nodes.size(); ++i) {
        if (m.node_name(i) == "Mover") mover_node = i;
        if (m.node_name(i) == "Still") still_node = i;
    }
    REQUIRE(mover_node != ~0u);
    REQUIRE(still_node != ~0u);
    // The static root: its translation in metres on the engine's axes (file x, y, z -> x, z, -y), and no channel.
    CHECK(m.nodes[still_node].translation[0] == doctest::Approx(0.1f).epsilon(1e-4));
    CHECK(m.nodes[still_node].translation[1] == doctest::Approx(0.3f).epsilon(1e-4));
    CHECK(m.nodes[still_node].translation[2] == doctest::Approx(-0.2f).epsilon(1e-4));
    REQUIRE(m.clips.size() == 1);
    REQUIRE(m.clips[0].channel_count == 2);
    const MeshChannel* tr = nullptr;
    const MeshChannel* rot = nullptr;
    for (u32 i = 0; i < m.clips[0].channel_count; ++i) {
        const MeshChannel& ch = m.channels[m.clips[0].first_channel + i];
        CHECK(ch.node == mover_node);
        (ch.path == AnimPath::Translation ? tr : rot) = &ch;
    }
    REQUIRE(tr != nullptr);
    REQUIRE(rot != nullptr);
    REQUIRE(rot->path == AnimPath::Rotation);
    REQUIRE(tr->key_count == 31);
    REQUIRE(rot->key_count == 31);
    const Quat rest{m.nodes[mover_node].rotation[0], m.nodes[mover_node].rotation[1], m.nodes[mover_node].rotation[2], m.nodes[mover_node].rotation[3]};
    for (u32 k : {0u, 10u, 30u}) {
        CAPTURE(k);
        const double f = k / 30.0;
        const float* t = &m.keys[tr->values_offset + k * 3];
        CHECK(std::fabs(t[0]) < 1e-5);
        CHECK(std::fabs(t[1]) < 1e-5);
        CHECK(t[2] == doctest::Approx(static_cast<float>(-f)).epsilon(1e-4).scale(1.0));  // file +Y, metres: engine -Z
        const float* q = &m.keys[rot->values_offset + k * 4];
        Quat rel = qmul({q[0], q[1], q[2], q[3]}, qconj(rest));  // the turn the clip adds to the rest pose, in the engine's frame
        if (rel.w < 0) rel = {-rel.x, -rel.y, -rel.z, -rel.w};
        const double half = f * 3.14159265358979323846 / 4;  // 90 degrees at f = 1
        CHECK(std::fabs(rel.x) < 1e-4);
        CHECK(rel.y == doctest::Approx(std::sin(half)).epsilon(1e-4).scale(1.0));  // about +Y
        CHECK(std::fabs(rel.z) < 1e-4);
        CHECK(rel.w == doctest::Approx(std::cos(half)).epsilon(1e-4));
    }
}

TEST_CASE("fbx: the animation sampling work of the whole file is budgeted") {
    // One 4000 s clip at 30 Hz (120001 frames) over 600 animated nodes: 72M evaluations, over the 64M budget. The cook says
    // so before it evaluates anything.
    FbxDoc doc;
    const long layer = doc.stack("Long", 4000.0);
    for (int i = 0; i < 600; ++i) {
        const long node = doc.model("n" + std::to_string(i), "Null", 0);
        doc.curve(layer, node, "Lcl Translation", 0, {{0.0, 0.0}, {4000.0, 1.0}});
    }
    const std::string fbx = doc.str();
    const auto t0 = std::chrono::steady_clock::now();
    const Direct d = fbx_direct(fbx);
    const double seconds = std::chrono::duration<double>(std::chrono::steady_clock::now() - t0).count();
    INFO(d.diags);
    CHECK_FALSE(d.ok);
    CHECK(has(d.diags, "transform evaluations"));
    CHECK(has(d.diags, "sample_rate"));
    CHECK(seconds < 10.0);
}

TEST_CASE("fbx: a vertex with an absurd number of skin weights is an error naming the cap") {
    auto spec_with = [](int weights_on_vertex0) {
        SkinSpec s;
        s.bones = 1;
        s.verts = {{0, 0, 0}, {1, 0, 0}, {0, 0, 1}};
        s.faces = {{0, 1, 2}};
        std::vector<std::pair<int, double>> w;
        w.reserve(static_cast<size_t>(weights_on_vertex0));
        for (int i = 0; i < weights_on_vertex0; ++i) w.push_back({0, 1.0 / weights_on_vertex0});
        s.weights = {w, {{0, 1.0}}, {{0, 1.0}}};
        return s;
    };
    const Direct bad = fbx_direct(skin_fbx(spec_with(3000), 1, 100.0, false));
    INFO(bad.diags);
    CHECK_FALSE(bad.ok);
    CHECK(has(bad.diags, "weights, over the importer's limit of 1024"));
    // At the cap the cook still works (and the repeated pairs add up to one influence).
    const Direct fine = fbx_direct(skin_fbx(spec_with(1000), 1, 100.0, false));
    INFO(fine.diags);
    CHECK(fine.ok);
}

TEST_CASE("fbx: a mesh with millions of non-triangle faces instanced on many nodes errors on the work budget") {
    // One triangle and 60000 line faces, shared by 1000 nodes: 180M face visits, over the 128M budget.
    FbxDoc doc;
    std::vector<std::vector<long>> faces = {{0, 1, 2}};
    for (int i = 0; i < 60000; ++i) faces.push_back({0, 1});
    const long geom = doc.geometry("Lines", {0, 0, 0, 1, 0, 0, 0, 0, 1}, faces);
    for (int i = 0; i < 1000; ++i) doc.connect(geom, doc.model("m" + std::to_string(i), "Mesh", 0));
    const Direct d = fbx_direct(doc.str());
    INFO(d.diags);
    CHECK_FALSE(d.ok);
    CHECK(has(d.diags, "work limit"));
}

TEST_CASE("fbx: a read error shows the reader's description on one line, without a stack trace") {
    std::string fbx = box_fbx({0.5, 0.5, 0.5}, 1, 100.0);
    fbx.insert(fbx.find("Objects:"), "\x01\x02 garbage {{{ \x7f\xff }\n");
    const Direct d = fbx_direct(fbx);
    INFO(d.diags);
    REQUIRE_FALSE(d.ok);
    CHECK(has(d.diags, "cannot read the FBX: "));
    CHECK_FALSE(has(d.diags, (std::string("ufbx") + ".c").c_str()));  // spelled so the ufbx wall does not read it as the header
    CHECK_FALSE(has(d.diags, "Stack"));
    CHECK_FALSE(has(d.diags, "\x7f"));
    CHECK(std::count(d.diags.begin(), d.diags.end(), '\n') == 1);
}
