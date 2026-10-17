// Autodesk FBX (.fbx, binary or ASCII) -> CHNK_MESH (M-IMPORT3D step 3b, ADR-0047), on ufbx and the shared mesh back end
// (mesh_build.h). The only translation unit besides ufbx_impl.c that may name ufbx (the "ufbx wall").
//
// Read: mesh nodes (polygons triangulated by ufbx_triangulate_face), normals (smooth ones are generated when absent) and UV
// set 0 (V flipped: FBX's origin is the bottom left, the engine's the top left, as for OBJ), one submesh per node and
// material slot; materials inline from ufbx's PBR view (base colour x factor, opacity, roughness, metalness, emission, base
// colour / normal / emission maps, and a roughness + metalness pair that names one packed image); the node tree (local
// transform = translation, rotation, scale, pivots and pre/post rotations already folded in by ufbx); skins as clusters
// (the 4 largest influences of a vertex, renormalized; up to 256 joints; inverse binds from the clusters' bind poses);
// animation stacks as clips, sampled at a fixed rate (sidecar "sample_rate", default 30) with no curve fitting.
//
// Unit and axes. ufbx is asked for the engine's frame: target_axes = right-handed, +Y up, front +Z (the glTF convention:
// an asset faces +Z) and target_unit_meters = 1, with UFBX_SPACE_CONVERSION_MODIFY_GEOMETRY: the unit scale is baked into
// the vertices and the translations, and the axis turn stays a rotation in the adjust transform of the root-level nodes,
// which ufbx has already folded into every node's local_transform and every ufbx_evaluate_transform result. So the node
// tree, the animation and the vertices of rigid meshes are read as they are; a Z-up file in centimetres and a Y-up file in
// metres give the same mesh. A left-handed file is mirrored on X (handedness_conversion_axis) with the winding corrected.
// Geometry transforms are baked into the vertices (instanced meshes get helper nodes), inherit modes ufbx cannot express as
// plain TRS become scale helper nodes. Sidecar "scale" multiplies on top (root nodes, as in glTF). There is no "up" key:
// the file says which way is up.
//
// Skins keep glTF's convention (skinned geometry lives in bind space, the skinned node's own transform is ignored): the
// vertices are moved from geometry space into the world space of the bind pose, and a joint's inverse bind is its cluster's
// geometry_to_bone with that move undone, i.e. the inverse of the joint's bind world matrix.
//
// Animation. A stack is a clip named like the stack. Only its base layer (the first) is evaluated; more layers, additive
// blend modes and layer weights other than 1 are a warning. Times: with rate R and dt = 1.0 / R (double), the stack
// [begin, end] of length L = end - begin is sampled at key i = 0 .. N - 1 at begin + i * dt and the last key N at end,
// where N = ceil(L * R - 1e-4); the key times written are i * dt (float, N-th: L). A node is sampled when a layer curve
// drives one of its transform properties (or its scale helper); a path whose samples all equal the rest pose is not
// written. Rotations are made continuous (the sign flipped to the previous key's hemisphere). Euler angles, slerp and
// the composition of pivots are evaluated by ufbx with libm sin / cos / atan2, so FBX bytes are per host (ADR-0047
// "Determinism"); everything this file adds is IEEE-exact (no libm beyond sqrt and ceil).
//
// Not imported, each one warning per file with a count: hidden meshes (Visibility 0), points and lines, vertex colours,
// UV sets beyond the first, blend shapes, lights, cameras, curves and surfaces, bump maps and opacity / other maps,
// layers beyond the base layer, animation of anything but node transforms, extra skin deformers (the first is used),
// dual-quaternion skinning (linear blending is used). Embedded textures are an error ("export with external textures").
//
// sidecar settings: { "scale": 1.0, "max_triangles": 0, "lod_levels": 0, "lod_ratio": 0.35, "lod_error": 0.05,
//                     "sample_rate": 30 }, as for glTF / OBJ, plus the sampling rate.
//
// The file is untrusted. The file is read from memory with the format forced to FBX (no sniffing, no external files, no
// thread pool); ufbx runs under a temporary and a result memory budget of 1 GiB each, an allocation-count cap and a node
// depth cap, and every count the importer allocates from (nodes, materials, submeshes, vertices, indices, joints, clips,
// samples, keys) is checked against a fixed limit (kMax* below) that fails naming the limit before the allocation.
// Non-finite or absurd (|v| > 1e9) numbers are errors. The node tree is walked with an explicit stack.

#include <algorithm>
#include <cmath>
#include <cstdarg>
#include <cstdio>
#include <cstring>

#include <ufbx.h>

#include "fbx_importer.h"
#include "importer_common.h"
#include "mce/asset/formats.h"
#include "mce/containers/hash_map.h"
#include "mesh_build.h"

namespace mce::cook {

namespace {

using detail::Geometry;

constexpr const char* kStage = "cook.fbx";

// ---- limits on untrusted input ----
constexpr usize kMaxFileBytes = usize(256) << 20;      // the .fbx
constexpr usize kTempMemoryLimit = usize(1) << 30;     // ufbx_load_opts.temp_allocator.memory_limit
constexpr usize kResultMemoryLimit = usize(1) << 30;   // ufbx_load_opts.result_allocator.memory_limit
constexpr usize kMaxAllocations = usize(1) << 24;      // allocation_limit of both
constexpr u32 kMaxNodeDepth = 4096;                    // ufbx_load_opts.node_depth_limit
constexpr u32 kMaxNodes = 1u << 18;                    // imported nodes (helper nodes included)
constexpr u32 kMaxMaterials = 1u << 16;
constexpr u32 kMaxSubmeshes = 1u << 18;                // (node, material slot) pairs that hold triangles
constexpr u64 kMaxSceneVertices = u64(1) << 24;        // welded vertices over every mesh instance
constexpr u64 kMaxSceneIndices = u64(1) << 25;         // triangle corners over every mesh instance
constexpr u32 kMaxFaceCorners = 1u << 20;              // one polygon
constexpr u32 kMaxClips = 1u << 10;
constexpr u32 kMaxClipSamples = 1u << 17;              // keys per channel
constexpr u64 kMaxKeyFloats = u64(1) << 25;            // the whole key pool (128 MiB)
constexpr u32 kMaxChannels = 1u << 20;
constexpr f64 kMaxCoordinate = 1e9;
constexpr f64 kMaxSampleRate = 1000.0;
constexpr u32 kNone = ~0u;

// ---- diagnostics ----

// `t`, shortened and made printable, for a message.
void quote(String& out, StringView t) {
    out += '\'';
    const usize n = std::min<usize>(t.size(), 40);
    for (usize i = 0; i < n; ++i) {
        const char c = t[i];
        out += (c >= 0x20 && c < 0x7F) ? c : '?';
    }
    if (t.size() > n) out += "...";
    out += '\'';
}

StringView sv(ufbx_string s) { return s.length ? StringView(s.data, s.length) : StringView(); }

class Rep {
public:
    Rep(StringView file, DiagList& diags) : file_(file), diags_(diags) {}

    bool fail(StringView msg, Errc code = Errc::ImportFailed) {
        diags_.error(code, file_, 0, 0, kStage, msg);
        return false;
    }
#if defined(__GNUC__)
    __attribute__((format(printf, 2, 3)))
#endif
    bool failf(const char* fmt, ...) {
        char buf[768];
        va_list ap;
        va_start(ap, fmt);
        std::vsnprintf(buf, sizeof(buf), fmt, ap);
        va_end(ap);
        return fail(buf);
    }
    void warn(StringView msg) { diags_.warning(file_, 0, 0, kStage, msg); }
#if defined(__GNUC__)
    __attribute__((format(printf, 2, 3)))
#endif
    void warnf(const char* fmt, ...) {
        char buf[768];
        va_list ap;
        va_start(ap, fmt);
        std::vsnprintf(buf, sizeof(buf), fmt, ap);
        va_end(ap);
        warn(buf);
    }
    StringView file() const { return file_; }

private:
    String file_;
    DiagList& diags_;
};

// A counted warning with the first thing it hit.
struct Warn {
    u32 count = 0;
    String first;
    void hit(StringView what) {
        if (count == 0) first.assign(what);
        if (count < kNone) ++count;
    }
};

void emit(Rep& rep, const Warn& w, const char* what) {
    if (w.count == 0) return;
    String m;
    appendf(m, "%s (%u time%s", what, w.count, w.count == 1 ? "" : "s");
    if (!w.first.empty()) {
        m += ", first: ";
        quote(m, w.first.view());
    }
    m += ')';
    rep.warn(m.view());
}

bool fin(f64 v) { return std::isfinite(v); }
bool coord_ok(f64 v) { return std::isfinite(v) && std::fabs(v) <= kMaxCoordinate; }
bool vec3_ok(ufbx_vec3 v) { return coord_ok(v.x) && coord_ok(v.y) && coord_ok(v.z); }

asset::MaterialChunk default_material() {
    asset::MaterialChunk m{};
    m.base_color[0] = m.base_color[1] = m.base_color[2] = m.base_color[3] = 1.0f;
    m.metallic = 0.0f;
    m.roughness = 1.0f;
    m.alpha_cutoff = 0.5f;
    return m;
}

f32 clamp01(f64 v) { return static_cast<f32>(std::min(1.0, std::max(0.0, v))); }

void set_texture(asset::MaterialChunk& m, asset::MaterialSlot slot, Guid g) {
    m.textures[static_cast<u32>(slot)][0] = g.hi;
    m.textures[static_cast<u32>(slot)][1] = g.lo;
}

u32 add_string(Array<char>& strings, StringView s, u32& size_out) {
    const auto off = static_cast<u32>(strings.size());
    for (char c : s) strings.push_back(c);
    size_out = static_cast<u32>(s.size());
    return off;
}

// ---- settings ----

struct Settings {
    f64 scale = 1.0, max_triangles = 0.0, lod_levels = 0.0, lod_ratio = 0.35, lod_error = 0.05, sample_rate = 30.0;
};

bool read_settings(const ImportInput& in, Settings& s, DiagList& diags) {
    const StringView file = in.display_path;
    auto bad = [&](StringView msg) {
        diags.error(Errc::ParseError, file, 0, 0, kStage, msg);
        return false;
    };
    if (!detail::check_keys(in.settings, {"scale", "max_triangles", "lod_levels", "lod_ratio", "lod_error", "sample_rate"}, file, kStage, diags))
        return false;
    if (!detail::read_number(in.settings, "lod_levels", 0.0, s.lod_levels, file, kStage, diags)) return false;
    if (!detail::read_number(in.settings, "lod_ratio", 0.35, s.lod_ratio, file, kStage, diags)) return false;
    if (!detail::read_number(in.settings, "lod_error", 0.05, s.lod_error, file, kStage, diags)) return false;
    if (!detail::read_number(in.settings, "scale", 1.0, s.scale, file, kStage, diags)) return false;
    if (!detail::read_number(in.settings, "max_triangles", 0.0, s.max_triangles, file, kStage, diags)) return false;
    if (!detail::read_number(in.settings, "sample_rate", 30.0, s.sample_rate, file, kStage, diags)) return false;
    if (s.lod_levels < 0.0 || s.lod_levels >= asset::kMaxMeshLods || s.lod_levels != std::floor(s.lod_levels))
        return bad("'lod_levels' must be a whole number 0..3");
    if (!(s.lod_ratio > 0.0 && s.lod_ratio < 1.0) || !(s.lod_error > 0.0 && s.lod_error <= 1.0))
        return bad("'lod_ratio' must be in (0, 1) and 'lod_error' in (0, 1]");
    if (!(s.scale > 0.0) || !fin(s.scale) || s.scale > kMaxCoordinate) return bad("'scale' must be positive");
    if (s.max_triangles < 0.0 || s.max_triangles != std::floor(s.max_triangles))
        return bad("'max_triangles' must be a whole number >= 0 (0 = no budget)");
    if (!(s.sample_rate > 0.0) || !(s.sample_rate <= kMaxSampleRate)) return bad("'sample_rate' must be in (0, 1000] samples per second");
    return true;
}

// ---- loading ----

struct Scene {
    ufbx_scene* p = nullptr;
    Scene() = default;
    Scene(const Scene&) = delete;
    Scene& operator=(const Scene&) = delete;
    ~Scene() {
        if (p) ufbx_free_scene(p);
    }
};

// `content` false: no geometry, animation or embedded data (the dependency scan only needs the materials).
bool load_scene(Rep& rep, Span<const byte> src, bool content, Scene& out) {
    if (src.empty()) return rep.fail("the file is empty");
    if (src.size() > kMaxFileBytes) return rep.failf("the file is over the importer's limit of %zu MiB", kMaxFileBytes >> 20);
    ufbx_load_opts opts = {};
    opts.file_format = UFBX_FILE_FORMAT_FBX;  // no sniffing: a .fbx that looks like an OBJ stays an error
    opts.temp_allocator.memory_limit = kTempMemoryLimit;
    opts.result_allocator.memory_limit = kResultMemoryLimit;
    opts.temp_allocator.allocation_limit = kMaxAllocations;
    opts.result_allocator.allocation_limit = kMaxAllocations;
    opts.node_depth_limit = kMaxNodeDepth;
    opts.load_external_files = false;
    opts.skip_mesh_parts = true;
    opts.clean_skin_weights = true;
    opts.normalize_normals = true;
    opts.use_blender_pbr_material = true;
    opts.ignore_all_content = !content;
    opts.target_axes = ufbx_axes_right_handed_y_up;
    opts.target_unit_meters = 1.0;
    opts.space_conversion = UFBX_SPACE_CONVERSION_MODIFY_GEOMETRY;
    opts.geometry_transform_handling = UFBX_GEOMETRY_TRANSFORM_HANDLING_MODIFY_GEOMETRY;
    opts.inherit_mode_handling = UFBX_INHERIT_MODE_HANDLING_HELPER_NODES;
    opts.handedness_conversion_axis = UFBX_MIRROR_AXIS_X;
    ufbx_error err = {};
    out.p = ufbx_load_memory(src.data(), src.size(), &opts, &err);
    if (out.p) return true;
    String m;
    switch (err.type) {
        case UFBX_ERROR_MEMORY_LIMIT:
        case UFBX_ERROR_ALLOCATION_LIMIT:
        case UFBX_ERROR_OUT_OF_MEMORY:
            appendf(m, "the file needs more memory than the importer allows (%zu MiB temporary, %zu MiB for the scene): it is corrupt or too big",
                    kTempMemoryLimit >> 20, kResultMemoryLimit >> 20);
            break;
        case UFBX_ERROR_NODE_DEPTH_LIMIT:
            appendf(m, "the node hierarchy is deeper than the importer's limit of %u", kMaxNodeDepth);
            break;
        case UFBX_ERROR_UNRECOGNIZED_FILE_FORMAT:
        case UFBX_ERROR_EMPTY_FILE:
            m += "not an FBX file (binary or ASCII, version 3000 and up)";
            break;
        default: {
            char buf[512];
            ufbx_format_error(buf, sizeof(buf), &err);
            m += "cannot read the FBX: ";
            m += buf;
            m += ". If the file is valid, re-export it from Blender as glTF, or as FBX 7.4 binary";
            break;
        }
    }
    return rep.fail(m.view());
}

// ---- paths and textures ----

// Path of `name` relative to assets/, given the path of the file that names it (`from`: "models/a.fbx"). Backslashes are
// separators; absolute and escaping paths are rejected.
bool resolve_rel(StringView from, StringView name, String& out) {
    out.clear();
    if (name.empty() || name[0] == '/' || name[0] == '\\' || name.find(':') != StringView::npos) return false;
    String joined;
    const usize slash = from.rfind('/');
    if (slash != StringView::npos) joined += from.substr(0, slash + 1);
    for (char c : name) joined += c == '\\' ? '/' : c;
    Array<StringView> parts;
    const StringView j = joined.view();
    usize start = 0;
    for (usize i = 0; i <= j.size(); ++i) {
        if (i < j.size() && j[i] != '/') continue;
        const StringView seg = j.substr(start, i - start);
        start = i + 1;
        if (seg.empty() || seg == ".") continue;
        if (seg == "..") {
            if (parts.empty()) return false;
            parts.pop_back();
            continue;
        }
        parts.push_back(seg);
    }
    for (usize i = 0; i < parts.size(); ++i) {
        if (i) out += '/';
        out += parts[i];
    }
    return !out.empty();
}

StringView base_name(StringView p) {
    usize cut = 0;
    for (usize i = 0; i < p.size(); ++i)
        if (p[i] == '/' || p[i] == '\\') cut = i + 1;
    return p.substr(cut);
}

// The file texture a material map uses (layered and shader textures: their first file), or null.
const ufbx_texture* map_texture(const ufbx_material_map& m) {
    if (!m.texture || !m.texture_enabled) return nullptr;
    const ufbx_texture* t = m.texture;
    if (t->type == UFBX_TEXTURE_FILE) return t;
    return t->file_textures.count > 0 ? t->file_textures.data[0] : nullptr;
}

struct TexResolver {
    const ImportInput& in;
    const ufbx_scene* scene;
    u32* fallbacks;  // counts names read as "the file name next to the model"

    // Resolves a file texture to its registered texture asset. With `rep` set, a failure is reported as an error.
    bool resolve(const ufbx_texture* t, Rep* rep, StringView what, Guid& out) const {
        bool embedded = t->content.size > 0 || (t->video && t->video->content.size > 0);
        if (!embedded && t->has_file && t->file_index < scene->texture_files.count)
            embedded = scene->texture_files.data[t->file_index].content.size > 0;
        if (embedded) {
            if (rep) {
                String m("texture of ");
                m += what;
                m += " is embedded in the FBX: export with external textures (Blender: Path Mode = Copy with Embed Textures off; Maya / 3ds Max: "
                     "uncheck Embed Media), then adopt the images next to the model";
                rep->fail(m.view(), Errc::MissingRef);
            }
            return false;
        }
        const StringView names[3] = {sv(t->relative_filename), sv(t->filename), sv(t->absolute_filename)};
        StringView shown;
        for (StringView n : names)
            if (shown.empty()) shown = n;
        if (shown.empty()) {
            if (rep) {
                String m("texture of ");
                m += what;
                m += " has no file name";
                rep->fail(m.view(), Errc::MissingRef);
            }
            return false;
        }
        String rel;
        if (in.lookup) {
            for (StringView n : names)  // as named (relative to the model)
                if (!n.empty() && resolve_rel(in.source_path, n, rel) && in.lookup->guid_for_path(rel.view(), out)) return true;
            for (StringView n : names) {  // absolute or foreign paths: the file name next to the model
                if (n.empty() || !resolve_rel(in.source_path, base_name(n), rel)) continue;
                if (in.lookup->guid_for_path(rel.view(), out)) {
                    if (fallbacks && *fallbacks < kNone) ++*fallbacks;
                    return true;
                }
            }
        }
        if (rep) {
            String m("texture ");
            quote(m, shown);
            m += " of ";
            m += what;
            m += " is not a registered asset: add the image next to the model and adopt it";
            rep->fail(m.view(), Errc::MissingRef);
        }
        return false;
    }
};

// The five maps of a material that become texture slots, in a fixed order.
enum PbrSlot : u32 { kBase, kNormal, kEmission, kRoughness, kMetalness, kPbrSlots };

const ufbx_material_map& pbr_map(const ufbx_material& m, u32 s) {
    switch (s) {
        case kBase: return m.pbr.base_color;
        case kNormal: return m.pbr.normal_map;
        case kEmission: return m.pbr.emission_color;
        case kRoughness: return m.pbr.roughness;
        default: return m.pbr.metalness;
    }
}

const char* pbr_name(u32 s) {
    static const char* const k[kPbrSlots] = {"base colour", "normal map", "emission", "roughness", "metalness"};
    return k[s];
}

// ---- the importer ----

struct VKey {
    u32 p = kNone, n = kNone, t = kNone;
    friend bool operator==(const VKey& a, const VKey& b) { return a.p == b.p && a.n == b.n && a.t == b.t; }
};

struct VKeyHash {
    u64 operator()(const VKey& k) const { return hash_combine(hash_combine(k.p, k.n), k.t); }
};

// A matrix applied to the vertices (and, as its normal matrix, the normals) of one mesh instance. Inactive: identity.
struct Xf {
    bool active = false;
    ufbx_matrix m{};
    ufbx_matrix n{};
};

struct SkinDef {
    const ufbx_skin_deformer* sd = nullptr;
    ufbx_matrix to_bind{};    // geometry space -> world space of the bind pose
    ufbx_matrix from_bind{};  // its inverse
};

void matrix16(const ufbx_matrix& m, f32 out[16]) {
    for (int c = 0; c < 4; ++c) {
        out[c * 4 + 0] = static_cast<f32>(m.cols[c].x);
        out[c * 4 + 1] = static_cast<f32>(m.cols[c].y);
        out[c * 4 + 2] = static_cast<f32>(m.cols[c].z);
        out[c * 4 + 3] = c == 3 ? 1.0f : 0.0f;
    }
}

bool matrix_ok(const ufbx_matrix& m) {
    for (int c = 0; c < 4; ++c)
        if (!vec3_ok(m.cols[c])) return false;
    return true;
}

bool is_transform_prop(ufbx_string name) {
    static const char* const k[] = {"Lcl Translation", "Lcl Rotation", "Lcl Scaling",   "PreRotation",  "PostRotation",
                                    "RotationOffset",  "RotationPivot", "ScalingOffset", "ScalingPivot"};
    for (const char* n : k)
        if (name.length == std::strlen(n) && std::memcmp(name.data, n, name.length) == 0) return true;
    return false;
}

// A unit quaternion as x, y, z, w floats; false when it is not finite or has no length.
bool unit_quat(const ufbx_quat& q, f32 out[4]) {
    if (!fin(q.x) || !fin(q.y) || !fin(q.z) || !fin(q.w)) return false;
    const f64 len = std::sqrt(q.x * q.x + q.y * q.y + q.z * q.z + q.w * q.w);
    if (!(len > 1e-12) || !fin(len)) return false;
    out[0] = static_cast<f32>(q.x / len);
    out[1] = static_cast<f32>(q.y / len);
    out[2] = static_cast<f32>(q.z / len);
    out[3] = static_cast<f32>(q.w / len);
    return true;
}

// A local transform as the node's floats; `k` multiplies translation and scale (the sidecar scale on root nodes).
bool trs_floats(const ufbx_transform& t, f64 k, f32 tr[3], f32 rot[4], f32 sc[3]) {
    if (!vec3_ok(t.translation) || !vec3_ok(t.scale) || !unit_quat(t.rotation, rot)) return false;
    tr[0] = static_cast<f32>(t.translation.x * k);
    tr[1] = static_cast<f32>(t.translation.y * k);
    tr[2] = static_cast<f32>(t.translation.z * k);
    sc[0] = static_cast<f32>(t.scale.x * k);
    sc[1] = static_cast<f32>(t.scale.y * k);
    sc[2] = static_cast<f32>(t.scale.z * k);
    return true;
}

class FbxCook {
public:
    FbxCook(const ImportInput& in, const Settings& s, Rep& rep, const ufbx_scene& scene)
        : in_(in), s_(s), rep_(rep), scene_(scene), tex_{in, &scene, &tex_fallbacks_} {}

    bool run(asset::ContainerWriter& writer);

private:
    bool build_nodes();
    bool build_meshes();
    bool import_instance(u32 out_node, const ufbx_node& node);
    bool material_chunk(const ufbx_material* mat, const asset::MaterialChunk*& out);
    bool convert_material(const ufbx_material& mat, asset::MaterialChunk& out);
    bool skin_for(const ufbx_skin_deformer& sd, i32& id);
    bool build_skins(detail::SkinTables& out);
    bool build_animations();
    void unsupported_scene_warnings();
    u32 pick_influences(const ufbx_skin_deformer& sd, u32 vertex, f32 joints[4], f32 weights[4]) const;

    const ImportInput& in_;
    const Settings& s_;
    Rep& rep_;
    const ufbx_scene& scene_;
    u32 tex_fallbacks_ = 0;
    TexResolver tex_;

    // output
    Geometry geo_;
    Array<asset::MeshNode> nodes_;
    Array<asset::MeshClip> clips_;
    Array<asset::MeshChannel> channels_;
    Array<f32> keys_;
    Array<char> strings_;

    // node tree
    Array<const ufbx_node*> order_;  // output node -> ufbx node
    Array<u32> node_of_;             // ufbx node typed_id -> output node
    bool any_scale_helper_ = false;

    // materials
    Array<u8> mat_state_;  // 0 = not built, 1 = built, 2 = failed
    Array<asset::MaterialChunk> mat_chunk_;
    asset::MaterialChunk default_mat_ = default_material();

    // skins
    Array<SkinDef> skin_defs_;  // importer skin id -> deformer
    Array<i32> skin_of_;        // deformer typed_id -> importer skin id

    // totals and warnings
    u64 total_vertices_ = 0, total_indices_ = 0;
    Warn w_hidden_, w_pointline_, w_color_, w_uvsets_, w_blend_, w_multiskin_, w_dq_, w_bump_, w_opacity_map_, w_layered_tex_, w_packed_;
    Warn w_layers_, w_blendmode_, w_otheranim_, w_emptyclip_;
    u32 lossy_vertices_ = 0;
};

// ---- nodes ----

bool FbxCook::build_nodes() {
    const usize total = scene_.nodes.count;
    node_of_.resize(total, kNone);
    struct Item {
        const ufbx_node* n;
        i32 parent;
    };
    Array<Item> stack;
    const ufbx_node* root = scene_.root_node;
    for (usize i = root->children.count; i-- > 0;) stack.push_back({root->children.data[i], -1});
    while (!stack.empty()) {
        const Item it = stack.back();
        stack.pop_back();
        const ufbx_node& n = *it.n;
        if (order_.size() >= kMaxNodes) return rep_.failf("more than %u nodes is over the importer's limit", kMaxNodes);
        if (n.typed_id >= total || node_of_[n.typed_id] != kNone) return rep_.fail("a node is reachable twice (cycle or shared child)");
        asset::MeshNode mn{};
        mn.parent = it.parent;
        if (!trs_floats(n.local_transform, it.parent < 0 ? s_.scale : 1.0, mn.translation, mn.rotation, mn.scale)) {
            String m("the transform of node ");
            quote(m, sv(n.name));
            m += " is not finite or is over the importer's limit of 1e9";
            return rep_.fail(m.view());
        }
        mn.name_offset = add_string(strings_, sv(n.name), mn.name_size);
        const auto self = static_cast<i32>(nodes_.size());
        node_of_[n.typed_id] = static_cast<u32>(self);
        order_.push_back(&n);
        nodes_.push_back(mn);
        if (n.is_scale_helper) any_scale_helper_ = true;
        for (usize i = n.children.count; i-- > 0;) stack.push_back({n.children.data[i], self});
    }
    return true;
}

// ---- materials ----

bool FbxCook::material_chunk(const ufbx_material* mat, const asset::MaterialChunk*& out) {
    if (!mat || mat->typed_id >= mat_state_.size()) {
        out = &default_mat_;
        return true;
    }
    u8& state = mat_state_[mat->typed_id];
    if (state == 0) state = convert_material(*mat, mat_chunk_[mat->typed_id]) ? 1 : 2;
    if (state != 1) return false;
    out = &mat_chunk_[mat->typed_id];
    return true;
}

bool FbxCook::convert_material(const ufbx_material& mat, asset::MaterialChunk& out) {
    out = default_material();
    String what("material ");
    quote(what, sv(mat.name));
    const ufbx_material_pbr_maps& p = mat.pbr;
    bool bad = false;
    auto val = [&](f64 v) {
        if (!fin(v)) bad = true;
        return v;
    };
    const f64 factor = p.base_factor.has_value ? val(p.base_factor.value_real) : 1.0;
    if (p.base_color.has_value) {
        out.base_color[0] = clamp01(val(p.base_color.value_vec3.x) * factor);
        out.base_color[1] = clamp01(val(p.base_color.value_vec3.y) * factor);
        out.base_color[2] = clamp01(val(p.base_color.value_vec3.z) * factor);
    }
    if (p.opacity.has_value) {
        out.base_color[3] = clamp01(val(p.opacity.value_real));
        if (out.base_color[3] < 1.0f) out.flags |= asset::MaterialFlags::AlphaBlend;
    } else if (p.transmission_factor.has_value) {
        // Plain FBX materials name opacity by its complement: TransparencyFactor 0 is opaque, 1 is clear.
        out.base_color[3] = 1.0f - clamp01(val(p.transmission_factor.value_real));
        if (out.base_color[3] < 1.0f) out.flags |= asset::MaterialFlags::AlphaBlend;
    }
    if (p.roughness.has_value) out.roughness = clamp01(val(p.roughness.value_real));
    if (p.metalness.has_value) out.metallic = clamp01(val(p.metalness.value_real));
    if (p.emission_color.has_value) {
        const f64 ef = p.emission_factor.has_value ? val(p.emission_factor.value_real) : 1.0;
        out.emissive[0] = static_cast<f32>(std::max(0.0, val(p.emission_color.value_vec3.x) * ef));
        out.emissive[1] = static_cast<f32>(std::max(0.0, val(p.emission_color.value_vec3.y) * ef));
        out.emissive[2] = static_cast<f32>(std::max(0.0, val(p.emission_color.value_vec3.z) * ef));
    }
    if (bad) {
        String m(what.view());
        m += " has a value that is not a finite number";
        return rep_.fail(m.view());
    }

    bool ok = true;
    auto slot = [&](u32 s, asset::MaterialSlot dst) {
        const ufbx_texture* t = map_texture(pbr_map(mat, s));
        if (!t) return false;
        if (pbr_map(mat, s).texture->type != UFBX_TEXTURE_FILE) w_layered_tex_.hit(sv(mat.name));
        Guid g;
        String w(what.view());
        w += ' ';
        w += pbr_name(s);
        if (!tex_.resolve(t, &rep_, w.view(), g)) {
            ok = false;
            return false;
        }
        set_texture(out, dst, g);
        return true;
    };
    slot(kBase, asset::MaterialSlot::BaseColor);
    slot(kNormal, asset::MaterialSlot::Normal);
    if (slot(kEmission, asset::MaterialSlot::Emissive) && !p.emission_color.has_value) {
        const f32 ef = p.emission_factor.has_value ? static_cast<f32>(std::max(0.0, p.emission_factor.value_real)) : 1.0f;
        out.emissive[0] = out.emissive[1] = out.emissive[2] = ef;
    }
    // Roughness and metalness maps: the engine reads one packed image (G = roughness, B = metallic).
    {
        const ufbx_texture* tr = map_texture(p.roughness);
        const ufbx_texture* tm = map_texture(p.metalness);
        Guid gr, gm;
        bool has_r = false, has_m = false;
        if (tr) {
            String w(what.view());
            w += " roughness";
            has_r = tex_.resolve(tr, &rep_, w.view(), gr);
            ok = ok && has_r;
        }
        if (tm) {
            String w(what.view());
            w += " metalness";
            has_m = tex_.resolve(tm, &rep_, w.view(), gm);
            ok = ok && has_m;
        }
        if (has_r && has_m && gr == gm) {
            set_texture(out, asset::MaterialSlot::MetallicRoughness, gr);
            // The shader multiplies the packed image by the factors: default them to 1 unless the file gave them.
            if (!p.roughness.has_value) out.roughness = 1.0f;
            if (!p.metalness.has_value) out.metallic = 1.0f;
        } else if (ok && (tr || tm)) {
            w_packed_.hit(sv(mat.name));
        }
    }
    if (!map_texture(p.normal_map) && map_texture(mat.fbx.bump)) w_bump_.hit(sv(mat.name));
    if (map_texture(p.opacity)) w_opacity_map_.hit(sv(mat.name));
    return ok;
}

// ---- skins ----

bool FbxCook::skin_for(const ufbx_skin_deformer& sd, i32& id) {
    if (sd.typed_id >= skin_of_.size()) return rep_.fail("a skin deformer is out of range");
    if (skin_of_[sd.typed_id] >= 0) {
        id = skin_of_[sd.typed_id];
        return true;
    }
    if (sd.clusters.count == 0 || sd.clusters.count > asset::kMaxSkinJoints)
        return rep_.failf("a skin needs 1..%u joints, this one has %zu", asset::kMaxSkinJoints, sd.clusters.count);
    // Geometry space -> world space at the bind pose, from the first cluster (every cluster of a consistent file agrees).
    const ufbx_skin_cluster* c0 = nullptr;
    for (usize i = 0; i < sd.clusters.count && !c0; ++i)
        if (sd.clusters.data[i]->bone_node) c0 = sd.clusters.data[i];
    if (!c0) return rep_.fail("a skin has no joint node");
    SkinDef d;
    d.sd = &sd;
    d.to_bind = ufbx_matrix_mul(&c0->bind_to_world, &c0->geometry_to_bone);
    const f64 det = ufbx_matrix_determinant(&d.to_bind);
    d.from_bind = ufbx_matrix_invert(&d.to_bind);
    if (!matrix_ok(d.to_bind) || !fin(det) || !(std::fabs(det) > 1e-18) || !matrix_ok(d.from_bind))
        return rep_.fail("a skin has a singular or non-finite bind matrix");
    skin_defs_.push_back(d);
    id = static_cast<i32>(skin_defs_.size() - 1);
    skin_of_[sd.typed_id] = id;
    return true;
}

// The 4 largest influences of `vertex` (ties: the lower joint), in descending weight order, renormalized; unused slots are
// joint 0 with weight 0. Returns 1 when the vertex lost more than 1% of its total weight.
u32 FbxCook::pick_influences(const ufbx_skin_deformer& sd, u32 vertex, f32 joints[4], f32 weights[4]) const {
    for (int k = 0; k < 4; ++k) joints[k] = weights[k] = 0.0f;
    if (vertex >= sd.vertices.count) return 0;
    const ufbx_skin_vertex sv_ = sd.vertices.data[vertex];
    if (static_cast<u64>(sv_.weight_begin) + sv_.num_weights > sd.weights.count) return 0;
    const ufbx_skin_weight* w = sd.weights.data + sv_.weight_begin;
    const u32 n = sv_.num_weights;
    f64 total = 0.0;
    for (u32 i = 0; i < n; ++i)
        if (w[i].weight > 0.0 && fin(w[i].weight) && w[i].cluster_index < sd.clusters.count) total += w[i].weight;
    u32 chosen[4] = {kNone, kNone, kNone, kNone};
    f64 kept = 0.0;
    f64 picked[4] = {0, 0, 0, 0};
    u32 count = 0;
    for (int k = 0; k < 4; ++k) {
        u32 best = kNone;
        for (u32 i = 0; i < n; ++i) {
            if (!(w[i].weight > 0.0) || !fin(w[i].weight) || w[i].cluster_index >= sd.clusters.count) continue;
            if (i == chosen[0] || i == chosen[1] || i == chosen[2] || i == chosen[3]) continue;
            if (best == kNone || w[i].weight > w[best].weight || (w[i].weight == w[best].weight && w[i].cluster_index < w[best].cluster_index))
                best = i;
        }
        if (best == kNone) break;
        chosen[k] = best;
        joints[k] = static_cast<f32>(w[best].cluster_index);
        picked[k] = w[best].weight;
        kept += w[best].weight;
        ++count;
    }
    if (kept > 0.0)
        for (u32 k = 0; k < count; ++k) weights[k] = static_cast<f32>(picked[k] / kept);
    return (total > 0.0 && (total - kept) / total > 0.01) ? 1u : 0u;
}

bool FbxCook::build_skins(detail::SkinTables& out) {
    Array<i32> mapped;
    mapped.resize(skin_defs_.size(), -1);
    for (usize s = 0; s < geo_.submeshes.size(); ++s) {
        const i32 gs = geo_.submesh_skin[s];
        if (gs < 0) {
            out.submesh_skins.push_back(-1);
            continue;
        }
        if (mapped[static_cast<usize>(gs)] < 0) {
            const SkinDef& d = skin_defs_[static_cast<usize>(gs)];
            Array<u32> joint_nodes;
            Array<f32> ibm;
            for (usize j = 0; j < d.sd->clusters.count; ++j) {
                const ufbx_skin_cluster& c = *d.sd->clusters.data[j];
                if (!c.bone_node || c.bone_node->typed_id >= node_of_.size() || node_of_[c.bone_node->typed_id] == kNone)
                    return rep_.fail("a skin joint is not a node of the imported scene");
                joint_nodes.push_back(node_of_[c.bone_node->typed_id]);
                const ufbx_matrix m = ufbx_matrix_mul(&c.geometry_to_bone, &d.from_bind);
                if (!matrix_ok(m)) return rep_.fail("a skin joint has a non-finite bind matrix");
                f32 f[16];
                matrix16(m, f);
                for (f32 v : f) ibm.push_back(v);
            }
            mapped[static_cast<usize>(gs)] = detail::add_skin(out, geo_, gs, joint_nodes.span(), ibm.span());
        }
        out.submesh_skins.push_back(mapped[static_cast<usize>(gs)]);
    }
    return true;
}

// ---- meshes ----

bool FbxCook::build_meshes() {
    for (u32 oi = 0; oi < order_.size(); ++oi) {
        const ufbx_node& n = *order_[oi];
        if (!n.mesh) continue;
        if (!n.visible) {
            w_hidden_.hit(sv(n.name));
            continue;
        }
        if (!import_instance(oi, n)) return false;
    }
    return true;
}

bool FbxCook::import_instance(u32 out_node, const ufbx_node& node) {
    const ufbx_mesh& m = *node.mesh;
    String where("mesh of node ");
    quote(where, sv(node.name));
    if (m.num_point_faces + m.num_line_faces > 0) w_pointline_.hit(sv(node.name));
    if (m.vertex_color.exists) w_color_.hit(sv(node.name));
    if (m.uv_sets.count > 1) w_uvsets_.hit(sv(node.name));
    if (m.blend_deformers.count > 0) w_blend_.hit(sv(node.name));
    if (m.skin_deformers.count > 1) w_multiskin_.hit(sv(node.name));
    if (!m.vertex_position.exists || m.num_faces == 0 || m.num_triangles == 0) return true;

    // The arrays ufbx hands out must agree with each other; check before indexing.
    const bool has_n = m.vertex_normal.exists, has_t = m.vertex_uv.exists;
    if (m.faces.count < m.num_faces || m.vertex_position.indices.count < m.num_indices || m.vertex_indices.count < m.num_indices ||
        (has_n && m.vertex_normal.indices.count < m.num_indices) || (has_t && m.vertex_uv.indices.count < m.num_indices)) {
        String e(where.view());
        e += " has inconsistent index arrays";
        return rep_.fail(e.view());
    }
    if (m.max_face_triangles + 2 > kMaxFaceCorners) {
        String e(where.view());
        appendf(e, " has a polygon over the importer's limit of %u corners", kMaxFaceCorners);
        return rep_.fail(e.view());
    }
    if (total_indices_ + static_cast<u64>(m.num_triangles) * 3 > kMaxSceneIndices)
        return rep_.failf("more than %llu triangle corners in the scene is over the importer's limit", static_cast<unsigned long long>(kMaxSceneIndices));

    // The skin, if any, decides the space the vertices are written in.
    const ufbx_skin_deformer* sd = (m.skin_deformers.count > 0 && m.skin_deformers.data[0]->clusters.count > 0) ? m.skin_deformers.data[0] : nullptr;
    i32 skin_id = -1;
    Xf xf;
    if (sd) {
        if (!skin_for(*sd, skin_id)) return false;
        if (sd->skinning_method == UFBX_SKINNING_METHOD_DUAL_QUATERNION || sd->skinning_method == UFBX_SKINNING_METHOD_BLENDED_DQ_LINEAR)
            w_dq_.hit(sv(node.name));
        xf.active = true;
        xf.m = skin_defs_[static_cast<usize>(skin_id)].to_bind;
        xf.n = ufbx_matrix_for_normals(&xf.m);
    } else if (node.has_geometry_transform) {
        xf.active = true;
        xf.m = node.geometry_to_node;
        xf.n = ufbx_matrix_for_normals(&xf.m);
    }

    // Faces by material slot, in face order.
    const usize slots = std::max<usize>(1, m.materials.count);
    Array<u32> start;
    start.resize(slots + 1, 0u);
    auto slot_of = [&](usize f) -> usize {
        if (f < m.face_material.count) {
            const u32 v = m.face_material.data[f];
            return v < slots ? v : slots - 1;
        }
        return 0;
    };
    for (usize f = 0; f < m.num_faces; ++f) ++start[slot_of(f) + 1];
    for (usize s = 0; s < slots; ++s) start[s + 1] += start[s];
    Array<u32> fill;
    fill.resize(slots, 0u);
    Array<u32> order;
    order.resize(m.num_faces, 0u);
    for (usize f = 0; f < m.num_faces; ++f) {
        const usize s = slot_of(f);
        order[start[s] + fill[s]++] = static_cast<u32>(f);
    }

    Array<u32> tri;
    tri.resize(std::max<usize>(1, m.max_face_triangles) * 3, 0u);
    for (usize s = 0; s < slots; ++s) {
        if (start[s] == start[s + 1]) continue;
        const ufbx_material* um = s < node.materials.count ? node.materials.data[s] : (s < m.materials.count ? m.materials.data[s] : nullptr);
        const asset::MaterialChunk* mat = nullptr;
        if (!material_chunk(um, mat)) return false;

        HashMap<VKey, u32, VKeyHash> weld;
        Array<f32> pos, nrm, uv, joints, weights;
        Array<u32> idx;
        auto add_vertex = [&](u32 ix, u32& out) -> bool {
            VKey key;
            key.p = m.vertex_position.indices.data[ix];
            if (has_n) key.n = m.vertex_normal.indices.data[ix];
            if (has_t) key.t = m.vertex_uv.indices.data[ix];
            auto [slotp, inserted] = weld.try_emplace(key, static_cast<u32>(pos.size() / 3));
            out = *slotp;
            if (!inserted) return true;
            if (total_vertices_ + pos.size() / 3 >= kMaxSceneVertices)
                return rep_.failf("more than %llu vertices in the scene is over the importer's limit", static_cast<unsigned long long>(kMaxSceneVertices));
            if (key.p >= m.vertex_position.values.count || (has_n && key.n >= m.vertex_normal.values.count) || (has_t && key.t >= m.vertex_uv.values.count)) {
                String e(where.view());
                e += " has an attribute index out of range";
                return rep_.fail(e.view());
            }
            ufbx_vec3 p = m.vertex_position.values.data[key.p];
            if (xf.active) p = ufbx_transform_position(&xf.m, p);
            if (!vec3_ok(p)) {
                String e(where.view());
                e += " has a vertex that is not finite or is over the importer's limit of 1e9";
                return rep_.fail(e.view());
            }
            pos.push_back(static_cast<f32>(p.x));
            pos.push_back(static_cast<f32>(p.y));
            pos.push_back(static_cast<f32>(p.z));
            if (has_n) {
                ufbx_vec3 nv = m.vertex_normal.values.data[key.n];
                if (xf.active) nv = ufbx_transform_direction(&xf.n, nv);
                const f64 len = std::sqrt(nv.x * nv.x + nv.y * nv.y + nv.z * nv.z);
                if (!fin(len)) {
                    String e(where.view());
                    e += " has a normal that is not finite";
                    return rep_.fail(e.view());
                }
                const f64 inv = len > 0.0 ? 1.0 / len : 1.0;
                nrm.push_back(static_cast<f32>(nv.x * inv));
                nrm.push_back(static_cast<f32>(nv.y * inv));
                nrm.push_back(static_cast<f32>(nv.z * inv));
            }
            if (has_t) {
                const ufbx_vec2 t = m.vertex_uv.values.data[key.t];
                if (!coord_ok(t.x) || !coord_ok(t.y)) {
                    String e(where.view());
                    e += " has a texture coordinate that is not finite or is over the importer's limit of 1e9";
                    return rep_.fail(e.view());
                }
                uv.push_back(static_cast<f32>(t.x));
                uv.push_back(static_cast<f32>(1.0 - t.y));  // FBX's origin is the bottom left
            }
            if (sd) {
                f32 j[4], w[4];
                lossy_vertices_ += pick_influences(*sd, m.vertex_indices.data[ix], j, w);
                for (int k = 0; k < 4; ++k) {
                    joints.push_back(j[k]);
                    weights.push_back(w[k]);
                }
            }
            return true;
        };
        for (u32 k = start[s]; k < start[s + 1]; ++k) {
            const ufbx_face face = m.faces.data[order[k]];
            if (face.num_indices < 3) continue;
            if (face.num_indices > kMaxFaceCorners) {
                String e(where.view());
                appendf(e, " has a polygon over the importer's limit of %u corners", kMaxFaceCorners);
                return rep_.fail(e.view());
            }
            const u32 nt = ufbx_triangulate_face(tri.data(), tri.size(), &m, face);
            for (u32 c = 0; c < nt * 3; ++c) {
                if (tri[c] >= m.num_indices) return rep_.fail("triangulation returned an index out of range");
                u32 v = 0;
                if (!add_vertex(tri[c], v)) return false;
                idx.push_back(v);
            }
        }
        if (idx.empty()) continue;
        if (geo_.submeshes.size() >= kMaxSubmeshes) return rep_.failf("more than %u submeshes is over the importer's limit", kMaxSubmeshes);
        detail::Primitive prim{};
        prim.positions = pos.span();
        if (has_n) prim.normals = nrm.span();
        if (has_t) prim.uvs = uv.span();
        prim.indices = idx.span();
        prim.material = *mat;
        prim.material_ext = asset::MaterialExt{};
        prim.node = out_node;
        if (sd) {
            prim.joints = joints.span();
            prim.weights = weights.span();
            prim.skin = skin_id;
            prim.skin_joints = static_cast<u32>(sd->clusters.count);
        }
        detail::add_submesh(prim, geo_);
        total_vertices_ += pos.size() / 3;
        total_indices_ += idx.size();
    }
    return true;
}

// ---- animation ----

bool FbxCook::build_animations() {
    const auto& stacks = scene_.anim_stacks;
    if (stacks.count == 0) return true;
    if (stacks.count > kMaxClips) return rep_.failf("%zu animation stacks is over the importer's limit of %u", stacks.count, kMaxClips);
    const f64 rate = s_.sample_rate;
    const f64 dt = 1.0 / rate;
    Array<u8> animated;
    Array<f64> td;     // evaluation offsets (double) of the keys
    Array<f32> times;  // key times
    Array<f32> tr, rot, scl;
    for (usize si = 0; si < stacks.count; ++si) {
        const ufbx_anim_stack& st = *stacks.data[si];
        String name;
        if (st.name.length) name.assign(sv(st.name));
        else appendf(name, "clip%zu", si);
        asset::MeshClip clip{};
        clip.name_offset = add_string(strings_, name.view(), clip.name_size);
        clip.first_channel = static_cast<u32>(channels_.size());
        if (st.layers.count == 0 || !st.layers.data[0]->anim) {
            w_emptyclip_.hit(name.view());
            clips_.push_back(clip);
            continue;
        }
        const ufbx_anim_layer& base = *st.layers.data[0];
        const ufbx_anim* anim = base.anim;
        if (st.layers.count > 1) w_layers_.hit(name.view());
        if (base.weight_is_animated || std::fabs(base.weight - 1.0) > 1e-9) w_blendmode_.hit(name.view());

        // The sampled range.
        f64 t0 = st.time_begin, t1 = st.time_end;
        if (!(t1 > t0)) {
            t0 = anim->time_begin;
            t1 = anim->time_end;
        }
        if (!fin(t0) || !fin(t1) || t1 < t0 || std::fabs(t0) > 1e9 || std::fabs(t1) > 1e9) {
            String e("animation stack ");
            quote(e, name.view());
            e += " has a time range that is not finite";
            return rep_.fail(e.view());
        }
        const f64 length = t1 - t0;
        const f64 span = length * rate;
        if (!(span <= static_cast<f64>(kMaxClipSamples - 1))) {
            String e("animation stack ");
            quote(e, name.view());
            appendf(e, " is %.3f s long: at %g samples per second that is over the importer's limit of %u samples per channel (lower 'sample_rate')",
                    length, rate, kMaxClipSamples);
            return rep_.fail(e.view());
        }
        const f64 steps_f = std::ceil(span - 1e-4);
        const u32 steps = steps_f > 0.0 ? static_cast<u32>(steps_f) : 0u;
        const u32 frames = steps + 1;
        times.clear();
        td.clear();
        for (u32 i = 0; i < frames; ++i) {
            const f64 t = i < steps ? static_cast<f64>(i) * dt : length;
            td.push_back(t);
            times.push_back(static_cast<f32>(t));
        }
        clip.duration = times[frames - 1];

        // Which nodes the layer's curves drive.
        animated.clear();
        animated.resize(order_.size(), any_scale_helper_ ? u8(1) : u8(0));
        auto mark = [&](const ufbx_node* nd) {
            if (nd && nd->typed_id < node_of_.size() && node_of_[nd->typed_id] != kNone) animated[node_of_[nd->typed_id]] = 1;
        };
        bool other = false;
        for (usize i = 0; i < base.anim_props.count; ++i) {
            const ufbx_anim_prop& ap = base.anim_props.data[i];
            const ufbx_node* nd = ap.element && ap.element->type == UFBX_ELEMENT_NODE ? ufbx_as_node(ap.element) : nullptr;
            if (!nd || !is_transform_prop(ap.prop_name)) {
                other = true;
                continue;
            }
            mark(nd);
            mark(nd->scale_helper);
        }
        if (other) w_otheranim_.hit(name.view());

        u32 times_off = kNone;
        u32 emitted = 0;
        for (u32 oi = 0; oi < order_.size(); ++oi) {
            if (!animated[oi]) continue;
            const ufbx_node& nd = *order_[oi];
            const u64 need = static_cast<u64>(frames) * 10 + (times_off == kNone ? frames : 0);
            if (keys_.size() + need > kMaxKeyFloats)
                return rep_.failf("animation data is over the importer's limit of %llu key values (lower 'sample_rate' or shorten the stacks)",
                                  static_cast<unsigned long long>(kMaxKeyFloats));
            if (channels_.size() + 3 > kMaxChannels) return rep_.failf("more than %u animation channels is over the importer's limit", kMaxChannels);
            tr.clear();
            rot.clear();
            scl.clear();
            const asset::MeshNode& rest = nodes_[oi];
            const f64 k = rest.parent < 0 ? s_.scale : 1.0;
            bool t_diff = false, r_diff = false, s_diff = false;
            for (u32 i = 0; i < frames; ++i) {
                const ufbx_transform tf = ufbx_evaluate_transform(anim, &nd, t0 + td[i]);
                f32 a[3], q[4], b[3];
                if (!trs_floats(tf, k, a, q, b)) {
                    String e("animation ");
                    quote(e, name.view());
                    e += " of node ";
                    quote(e, sv(nd.name));
                    appendf(e, " is not finite or is over the importer's limit of 1e9 at sample %u", i);
                    return rep_.fail(e.view());
                }
                const f32* prev = i == 0 ? rest.rotation : &rot[(static_cast<usize>(i) - 1) * 4];  // keep the quaternions in one hemisphere
                if (prev[0] * q[0] + prev[1] * q[1] + prev[2] * q[2] + prev[3] * q[3] < 0.0f)
                    for (int c = 0; c < 4; ++c) q[c] = -q[c];
                for (int c = 0; c < 3; ++c) {
                    tr.push_back(a[c]);
                    scl.push_back(b[c]);
                    t_diff = t_diff || a[c] != rest.translation[c];
                    s_diff = s_diff || b[c] != rest.scale[c];
                }
                for (int c = 0; c < 4; ++c) {
                    rot.push_back(q[c]);
                    r_diff = r_diff || q[c] != rest.rotation[c];
                }
            }
            if (!t_diff && !r_diff && !s_diff) continue;
            if (times_off == kNone) {
                times_off = static_cast<u32>(keys_.size());
                for (f32 t : times) keys_.push_back(t);
            }
            auto emit_channel = [&](asset::AnimPath path, const Array<f32>& values) {
                asset::MeshChannel mc{};
                mc.node = oi;
                mc.path = path;
                mc.interp = asset::AnimInterp::Linear;
                mc.key_count = frames;
                mc.times_offset = times_off;
                mc.values_offset = static_cast<u32>(keys_.size());
                for (f32 v : values) keys_.push_back(v);
                channels_.push_back(mc);
                ++emitted;
            };
            if (t_diff) emit_channel(asset::AnimPath::Translation, tr);
            if (r_diff) emit_channel(asset::AnimPath::Rotation, rot);
            if (s_diff) emit_channel(asset::AnimPath::Scale, scl);
        }
        clip.channel_count = emitted;
        if (emitted == 0) w_emptyclip_.hit(name.view());
        clips_.push_back(clip);
    }
    return true;
}

// ---- the whole file ----

void FbxCook::unsupported_scene_warnings() {
    const usize other = scene_.lights.count + scene_.cameras.count + scene_.nurbs_curves.count + scene_.nurbs_surfaces.count +
                        scene_.line_curves.count;
    if (other > 0)
        rep_.warnf("%zu light%s, camera%s, curve or surface node%s are not imported", other, other == 1 ? "" : "s", other == 1 ? "" : "s",
                   other == 1 ? "" : "s");
    usize ufbx_warnings = 0;
    for (usize i = 0; i < scene_.metadata.warnings.count; ++i) ufbx_warnings += scene_.metadata.warnings.data[i].count;
    if (ufbx_warnings > 0) {
        String m;
        appendf(m, "the FBX reader noted %zu problem%s while reading (first: ", ufbx_warnings, ufbx_warnings == 1 ? "" : "s");
        quote(m, sv(scene_.metadata.warnings.data[0].description));
        m += ')';
        rep_.warn(m.view());
    }
}

bool FbxCook::run(asset::ContainerWriter& writer) {
    if (!scene_.root_node) return rep_.fail("the file has no scene");
    if (scene_.materials.count > kMaxMaterials) return rep_.failf("more than %u materials is over the importer's limit", kMaxMaterials);
    mat_state_.resize(scene_.materials.count, 0);
    mat_chunk_.resize(scene_.materials.count, default_material());
    skin_of_.resize(scene_.skin_deformers.count, -1);
    unsupported_scene_warnings();
    if (!build_nodes()) return false;
    if (!build_meshes()) return false;
    if (tex_fallbacks_ > 0)
        rep_.warnf("%u texture path%s absolute or outside assets/: read as the file name next to the model", tex_fallbacks_,
                   tex_fallbacks_ == 1 ? " was" : "s were");
    emit(rep_, w_hidden_, "hidden meshes (Visibility 0) are skipped");
    emit(rep_, w_pointline_, "points and lines are not imported");
    emit(rep_, w_color_, "vertex colors are not imported");
    emit(rep_, w_uvsets_, "UV sets beyond the first are not imported");
    emit(rep_, w_blend_, "blend shapes (morph targets) are not imported");
    emit(rep_, w_multiskin_, "a mesh has more than one skin deformer: the first is used");
    emit(rep_, w_dq_, "dual-quaternion skinning is imported as linear blend skinning");
    emit(rep_, w_bump_, "bump maps are not imported (connect a tangent-space normal map as NormalMap)");
    emit(rep_, w_opacity_map_, "opacity maps are not imported");
    emit(rep_, w_layered_tex_, "layered or shader textures are imported as their first image");
    emit(rep_, w_packed_,
         "roughness and metalness maps are imported only when both name one packed image (G = roughness, B = metallic): ignored");
    if (lossy_vertices_ > 0)
        rep_.warnf("%u vertices have more than 4 skin influences and lost more than 1%% of their weight", lossy_vertices_);

    if (s_.lod_levels > 0.0)
        detail::generate_lods(geo_, static_cast<u32>(s_.lod_levels), static_cast<f32>(s_.lod_ratio), static_cast<f32>(s_.lod_error));
    if (!build_animations()) return false;
    emit(rep_, w_layers_, "animation layers beyond the base layer are ignored");
    emit(rep_, w_blendmode_, "the base animation layer's weight is ignored");
    emit(rep_, w_otheranim_, "animation of anything but node transforms (visibility, blend shape weights, materials, cameras) is ignored");
    emit(rep_, w_emptyclip_, "animation stack animates no node");

    detail::SkinTables skins;
    if (!build_skins(skins)) return false;
    if (geo_.submeshes.empty()) rep_.warn("the file has no meshes");
    usize triangles = 0;  // the base levels: LODs are cheaper by construction
    for (const asset::MeshSubmesh& sm : geo_.submeshes) triangles += sm.index_count / 3;
    if (s_.max_triangles > 0.0 && static_cast<f64>(triangles) > s_.max_triangles) {
        String msg;
        appendf(msg, "%zu triangles, over the mesh's budget of %.0f (sidecar 'max_triangles')", triangles, s_.max_triangles);
        return rep_.fail(msg.view(), Errc::InvalidArgument);
    }
    detail::MeshTables tables;
    tables.nodes = nodes_.span();
    tables.clips = clips_.span();
    tables.channels = channels_.span();
    tables.keys = keys_.span();
    tables.strings = strings_.span();
    detail::write_mesh_chunk(geo_, skins, tables, false, in_.guid, writer);
    return true;
}

// ---- the importer ----

void add_dep(Array<DepRef>& out, Guid g, StringView where) {
    for (const DepRef& e : out)
        if (e.guid == g) return;
    DepRef d;
    d.guid = g;
    d.expected = ChunkType::Texture;
    d.line = 0;
    d.col = 0;
    d.where.assign(where);
    out.push_back(d);
}

class FbxImporter final : public IImporter {
public:
    StringView id() const override { return "fbx"; }
    u32 version() const override { return 1; }
    ChunkType type() const override { return ChunkType::Mesh; }
    bool native() const override { return false; }
    bool handles_extension(StringView ext) const override { return ext == ".fbx"; }

    // Texture files of the materials -> texture GUIDs. Problems are left to cook() to report (once).
    void scan_dependencies(const ImportInput& in, Array<DepRef>& out, DiagList&) const override {
        DiagList scratch;
        Rep rep(in.display_path, scratch);
        Scene scene;
        if (!load_scene(rep, in.source, false, scene) || !scene.p) return;
        u32 fallbacks = 0;
        const TexResolver tex{in, scene.p, &fallbacks};
        const usize n = std::min<usize>(scene.p->materials.count, kMaxMaterials);
        for (usize i = 0; i < n; ++i) {
            const ufbx_material& mat = *scene.p->materials.data[i];
            for (u32 s = 0; s < kPbrSlots; ++s) {
                const ufbx_texture* t = map_texture(pbr_map(mat, s));
                Guid g;
                if (!t || !tex.resolve(t, nullptr, "", g)) continue;
                String w("material ");
                quote(w, sv(mat.name));
                w += ' ';
                w += pbr_name(s);
                add_dep(out, g, w.view());
            }
        }
    }

    bool cook(const ImportInput& in, asset::ContainerWriter& writer, DiagList& diags) const override {
        Settings settings;
        if (!read_settings(in, settings, diags)) return false;
        Rep rep(in.display_path, diags);
        Scene scene;
        if (!load_scene(rep, in.source, true, scene) || !scene.p) return false;
        FbxCook cook(in, settings, rep, *scene.p);
        return cook.run(writer);
    }
};

}  // namespace

mem::UniquePtr<IImporter> make_fbx_importer() { return mem::make_unique<FbxImporter>(mem::heap()); }

}  // namespace mce::cook
