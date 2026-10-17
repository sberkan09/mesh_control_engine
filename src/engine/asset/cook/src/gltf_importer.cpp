// glTF 2.0 (.glb / .gltf) -> CHNK_MESH: static meshes, the node tree, inline metallic-roughness
// materials, node-transform animation clips (assets.md §5.2, FR-R3D-010) and, optionally, skins
// (FR-R3D-060, v0.5).
//
// Scope is locked (assets.md §10 risk 2): triangle-list primitives with POSITION, optional NORMAL
// (generated when absent) and TEXCOORD_0; materials' factors, alphaMode, doubleSided and base
// color, normal, metallic-roughness (ORM), emissive and occlusion textures (all on TEXCOORD_0, one
// KHR_texture_transform per material), plus KHR_materials_unlit, KHR_materials_emissive_strength and the
// factors of KHR_materials_clearcoat / _sheen / _anisotropy / _transmission / _ior (their textures are not
// imported); node TRS (or a decomposable matrix); translation/rotation/scale animation channels; skins with
// JOINTS_n / WEIGHTS_n (the 4 largest of up to 8 sets, renormalized; up to 256 joints per skin) and inverse
// bind matrices.
//
// Accessors: sparse ones everywhere (attributes, indices, animation, inverse bind, morph deltas);
// KHR_mesh_quantization (BYTE / SHORT POSITION, NORMAL and TEXCOORD_0, normalized or not; the node transform
// carries the dequantization scale); EXT_meshopt_compression (the compressed bufferViews are decoded with
// meshoptimizer before the accessors read them; the fallback buffer is ignored). KHR_draco_mesh_compression
// is not decoded: required, it is a cook error that says to re-export with meshopt (gltfpack -c or
// gltf-transform meshopt); merely used, the uncompressed fallback geometry is read.
//
// Morph targets are not imported, but the default weights (node, else mesh) are baked into POSITION / NORMAL so
// the rest pose matches other viewers. Dropped with one warning per file: COLOR_0, TEXCOORD_1+, cameras and
// KHR_lights_punctual (each names the first JSON pointer). Required extensions other than the supported set are
// a cook error naming the extension; merely-used ones are a warning.
//
// The file is untrusted: counts, strides, offsets, sparse and compressed streams are range-checked with
// overflow-safe arithmetic, and one accessor, one decoded bufferView and the whole scene have fixed size limits
// (kMax* below) that fail with the JSON pointer instead of allocating.
//
// Buffers must be inside the file: the GLB binary chunk or base64 `data:` URIs (the cook is pure
// — an importer sees its own source bytes only). Images must be external files registered as
// texture assets; their URI (relative to the glTF) resolves to the texture's GUID, which becomes a
// dependency. Errors name the file and the JSON pointer of the offending element.
//
// Per primitive (mesh_build.cpp, shared by every model importer): de-duplicate vertices (meshopt_generateVertexRemap), meshopt_optimizeVertexCache,
// meshopt_optimizeVertexFetch, then quantize (formats.h MeshVertex). Deterministic: no hashing of
// pointers, no clocks, stable iteration order everywhere.
//
// sidecar settings: { "scale": 1.0 }          uniform scale applied to every root node
//                   { "max_triangles": N }   the mesh's triangle budget (M7 per-mesh budget table):
//                                            more triangles is a cook error naming both counts
//                   { "skinning": true }     false: import skinned meshes as static ones (skins
//                                            dropped, primitives placed by their node: the
//                                            node-transform / static character path)
//                   { "lod_levels": 0 }      coarser levels per primitive (0..3) made by
//                                            meshopt_simplify, each keeping `lod_ratio` of the
//                                            previous level's triangles (default 0.35) while the
//                                            deviation stays under `lod_error` of the primitive's
//                                            size (default 0.05); a level that saves < 10% ends it
//                   { "ragdoll": { "<bone>": { "exclude": true, "include": true, "radius": 0.08,
//                                  "swing": 45, "twist": 20, "mass": 1.5 }, ... } }
//                                            per-bone overrides for kit::Ragdoll3D (M-GAMEPLAY3D 3),
//                                            by skin joint node name: no body (the bone rides with its
//                                            parent), a body however short, capsule radius (m), swing
//                                            and twist limits (degrees), a weight on its mass share
//
// Authored LODs: a node named `<name>_LOD<n>` (n = 1..3) is not drawn; its primitives become level
// n of node `<name>`'s primitives (by index), quantized with their bounds. A primitive with
// authored levels gets no generated ones. Skinned primitives keep generated levels only.

#include <algorithm>
#include <cmath>
#include <cstring>

#include "importer_common.h"
#include "mce/asset/formats.h"
#include "mce/math/math.h"
#include "mesh_build.h"
#include "meshoptimizer.h"

namespace mce::cook {

namespace {

using namespace mce::math;
using detail::Geometry;
using detail::SkinTables;

constexpr const char* kStage = "cook.gltf";
constexpr u32 kGlbMagic = 0x46546C67;  // "glTF"
constexpr u32 kGlbJson = 0x4E4F534A;   // "JSON"
constexpr u32 kGlbBin = 0x004E4942;    // "BIN\0"

constexpr StringView kSupportedExtensions[] = {"KHR_texture_transform",
                                               "KHR_materials_unlit",
                                               "KHR_materials_emissive_strength",
                                               "KHR_materials_clearcoat",
                                               "KHR_materials_sheen",
                                               "KHR_materials_anisotropy",
                                               "KHR_materials_transmission",
                                               "KHR_materials_ior",
                                               "KHR_mesh_quantization",
                                               "EXT_meshopt_compression"};

// ---- limits on untrusted input (every count, size and offset below comes from the file) ----
// A bigger accessor, decoded view or scene is a cook error naming the JSON pointer, never an allocation.
constexpr u64 kMaxAccessorCount = u64(1) << 24;       // elements of one accessor
constexpr u64 kMaxAccessorFloats = u64(1) << 25;      // elements * components of one accessor (128 MiB of floats)
constexpr u64 kMaxDecodedViewBytes = u64(256) << 20;  // one EXT_meshopt_compression bufferView after decoding
constexpr u64 kMaxDecodedTotalBytes = u64(512) << 20;  // all of them together
constexpr u64 kMaxMeshoptRatio = 256;                  // decoded / compressed bytes a real v0 vertex or index stream cannot exceed (bound is ~64)
constexpr u64 kMaxMorphWork = u64(1) << 27;            // vertices * (1 + weighted targets) baked over the whole scene (instances count)
constexpr u64 kMaxSceneVertices = u64(1) << 25;        // vertices over every primitive of the scene (instances count)
constexpr u64 kMaxSceneIndices = u64(1) << 26;
constexpr u32 kMaxMorphTargets = 512;
constexpr u32 kMaxInfluenceSets = 8;                   // JOINTS_0..7 / WEIGHTS_0..7

// The warn-once features of Ctx::warn_once.
enum FeatureWarning : u32 { kWarnColor = 1, kWarnTexcoord = 2, kWarnCamera = 4, kWarnLight = 8, kWarnMorph = 16 };

bool supported_extension(StringView name) {
    for (StringView s : kSupportedExtensions)
        if (s == name) return true;
    return false;
}

// A non-negative integral JSON number (every glTF index, count and offset).
bool nonneg(JsonValue v) { return v.is_integer() && v.as_number() >= 0; }

u32 read_u32(const byte* p) {
    u32 v;
    std::memcpy(&v, p, 4);
    return v;
}

int base64_value(char c) {
    if (c >= 'A' && c <= 'Z') return c - 'A';
    if (c >= 'a' && c <= 'z') return c - 'a' + 26;
    if (c >= '0' && c <= '9') return c - '0' + 52;
    if (c == '+' || c == '-') return 62;
    if (c == '/' || c == '_') return 63;
    return -1;
}

bool base64_decode(StringView in, Array<byte>& out) {
    out.clear();
    u32 acc = 0;
    int bits = 0;
    for (char c : in) {
        if (c == '=') break;
        const int v = base64_value(c);
        if (v < 0) return false;
        acc = (acc << 6) | static_cast<u32>(v);
        bits += 6;
        if (bits >= 8) {
            bits -= 8;
            out.push_back(static_cast<byte>((acc >> bits) & 0xFF));
        }
    }
    return true;
}

// Path of `uri` relative to assets/, given the glTF's own path. Rejects absolute and escaping URIs.
bool resolve_uri(StringView gltf_path, StringView uri, String& out) {
    out.clear();
    if (uri.empty() || uri[0] == '/' || uri.find(':') != StringView::npos) return false;
    const usize slash = gltf_path.rfind('/');
    String joined;
    if (slash != StringView::npos) {
        joined += gltf_path.substr(0, slash + 1);
    }
    // Percent-decode (glTF URIs are RFC 3986).
    for (usize i = 0; i < uri.size(); ++i) {
        char c = uri[i];
        if (c == '%' && i + 2 < uri.size()) {
            auto hex = [](char h) { return h >= '0' && h <= '9' ? h - '0' : (h | 32) - 'a' + 10; };
            c = static_cast<char>(hex(uri[i + 1]) * 16 + hex(uri[i + 2]));
            i += 2;
        }
        joined += c == '\\' ? '/' : c;
    }
    // Normalize "." and ".." segments.
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

void append_pointer(String& s, StringView a, u32 i) { appendf(s, "/%.*s/%u", static_cast<int>(a.size()), a.data(), i); }

// One parsed glTF document plus its buffers.
struct Gltf {
    JsonDoc doc;
    JsonValue root;
    Array<Array<byte>> buffers;
    Span<const byte> glb_bin;
    // EXT_meshopt_compression: per bufferView, the decoded bytes and their element size (0 = not compressed).
    Array<Array<byte>> decoded;
    Array<u32> decoded_stride;
};

class Ctx {
public:
    Ctx(const ImportInput& in, DiagList& diags) : in_(in), diags_(diags) {}

    void error(JsonValue at, StringView pointer, StringView msg, Errc code = Errc::ImportFailed) {
        String m;
        if (!pointer.empty()) {
            m += pointer;
            m += ": ";
        }
        m += msg;
        detail::error_at(diags_, code, in_.display_path, at, kStage, m);
    }
    void warning(JsonValue at, StringView msg) {
        diags_.warning(in_.display_path, at.valid() ? at.line() : 0, at.valid() ? at.col() : 0, kStage, msg);
    }
    // One warning per file and feature: `flag` is a bit of FeatureWarning.
    void warn_once(u32 flag, JsonValue at, StringView msg) {
        if (warned_ & flag) return;
        warned_ |= flag;
        warning(at, msg);
    }
    // Counts scene-wide output against the importer's limits; false (after an error) when over.
    bool charge(JsonValue at, StringView pointer, u64 vertices, u64 indices) {
        vertices_ += vertices;
        indices_ += indices;
        if (vertices_ <= kMaxSceneVertices && indices_ <= kMaxSceneIndices) return true;
        error(at, pointer, "the scene is over the importer's limit of 32M vertices or 64M indices (instances count)");
        return false;
    }
    // Counts the work of baking default morph weights (`units` = vertices * (1 + weighted targets)) before any delta
    // is read; false (after an error) when the scene is over the budget.
    bool charge_morph(JsonValue at, StringView pointer, u64 units) {
        morph_work_ = units > kMaxMorphWork ? kMaxMorphWork + 1 : morph_work_ + units;
        if (morph_work_ <= kMaxMorphWork) return true;
        error(at, pointer, "baking the default morph weights is over the importer's budget (vertices x weighted targets, instances count)");
        return false;
    }
    u32 errors() const { return diags_.error_count(); }
    const ImportInput& in() const { return in_; }

private:
    const ImportInput& in_;
    DiagList& diags_;
    u32 warned_ = 0;
    u64 vertices_ = 0;
    u64 indices_ = 0;
    u64 morph_work_ = 0;
};

bool parse_container(Ctx& cx, Gltf& g) {
    const Span<const byte> src = cx.in().source;
    StringView json;
    if (src.size() >= 12 && read_u32(src.data()) == kGlbMagic) {
        const u32 version = read_u32(src.data() + 4);
        const u32 length = read_u32(src.data() + 8);
        if (version != 2 || length > src.size() || length < 20) {
            cx.error({}, {}, "malformed GLB header (version 2 expected)");
            return false;
        }
        usize at = 12;
        while (at + 8 <= length) {
            const u32 clen = read_u32(src.data() + at);
            const u32 ctype = read_u32(src.data() + at + 4);
            at += 8;
            if (clen > length - at) {
                cx.error({}, {}, "GLB chunk runs past the end of the file");
                return false;
            }
            if (ctype == kGlbJson && json.empty()) json = StringView(reinterpret_cast<const char*>(src.data() + at), clen);
            else if (ctype == kGlbBin && g.glb_bin.empty()) g.glb_bin = src.subspan(at, clen);
            at += (clen + 3) & ~3u;
        }
        if (json.empty()) {
            cx.error({}, {}, "GLB has no JSON chunk");
            return false;
        }
    } else {
        json = StringView(reinterpret_cast<const char*>(src.data()), src.size());
    }
    JsonError err;
    if (!g.doc.parse(json, err)) {
        String m("JSON parse error: ");
        m += err.message.view();
        cx.error({}, {}, m);
        return false;
    }
    g.root = g.doc.root();
    if (!g.root.is_object()) {
        cx.error(g.root, {}, "the document root must be an object");
        return false;
    }
    const JsonValue ver = g.root.get("asset").get("version");
    if (!ver.is_string() || ver.as_string().substr(0, 2) != "2.") {
        cx.error(ver.valid() ? ver : g.root, "/asset/version", "only glTF 2.x is supported");
        return false;
    }
    if (const JsonValue req = g.root.get("extensionsRequired"); req.is_array()) {
        for (u32 i = 0; i < req.size(); ++i) {
            const StringView e = req.at(i).is_string() ? req.at(i).as_string() : StringView();
            if (!supported_extension(e)) {
                String m("required extension '");
                m += e;
                m += "' is not supported";
                if (e == "KHR_draco_mesh_compression")
                    m += ": Draco is not decoded by the cook; re-export the model with meshopt compression "
                         "(gltfpack -c, or gltf-transform meshopt) or without compression";
                String ptr;
                append_pointer(ptr, "extensionsRequired", i);
                cx.error(req.at(i), ptr, m);
            }
        }
    }
    if (const JsonValue used = g.root.get("extensionsUsed"); used.is_array()) {
        for (u32 i = 0; i < used.size(); ++i) {
            const StringView e = used.at(i).is_string() ? used.at(i).as_string() : StringView();
            if (e == "KHR_lights_punctual") continue;  // warned about once, with the first light's pointer
            if (e == "KHR_draco_mesh_compression") {
                cx.warning(used.at(i), "KHR_draco_mesh_compression is not decoded: the uncompressed fallback geometry is read");
                continue;
            }
            if (!supported_extension(e)) {
                String m("extension '");
                m += e;
                m += "' is ignored";
                cx.warning(used.at(i), m);
            }
        }
    }
    return cx.errors() == 0;
}

bool load_buffers(Ctx& cx, Gltf& g) {
    const JsonValue bufs = g.root.get("buffers");
    if (!bufs.valid()) return true;
    if (!bufs.is_array()) {
        cx.error(bufs, "/buffers", "must be an array");
        return false;
    }
    for (u32 i = 0; i < bufs.size(); ++i) {
        const JsonValue b = bufs.at(i);
        String ptr;
        append_pointer(ptr, "buffers", i);
        const JsonValue len = b.get("byteLength");
        if (!nonneg(len) || len.as_number() < 0) {
            cx.error(b, ptr, "byteLength must be a non-negative integer");
            return false;
        }
        const usize need = static_cast<usize>(len.as_number());
        Array<byte> data;
        // EXT_meshopt_compression fallback buffer: no data in the file, and never read.
        if (const JsonValue fb = b.get("extensions").get("EXT_meshopt_compression").get("fallback");
            fb.is_bool() && fb.as_bool()) {
            g.buffers.push_back(std::move(data));
            continue;
        }
        const JsonValue uri = b.get("uri");
        if (!uri.valid()) {
            if (i != 0 || g.glb_bin.empty()) {
                cx.error(b, ptr, "buffer without a uri needs the GLB binary chunk");
                return false;
            }
            data.resize(g.glb_bin.size());
            if (!g.glb_bin.empty()) std::memcpy(data.data(), g.glb_bin.data(), g.glb_bin.size());
        } else {
            const StringView u = uri.is_string() ? uri.as_string() : StringView();
            const usize comma = u.find(',');
            if (u.substr(0, 5) != "data:" || comma == StringView::npos || u.substr(0, comma).find(";base64") == StringView::npos) {
                cx.error(uri, ptr,
                         "external buffers are not supported: export a .glb or embed the buffer as a base64 data: URI");
                return false;
            }
            if (!base64_decode(u.substr(comma + 1), data)) {
                cx.error(uri, ptr, "invalid base64 data");
                return false;
            }
        }
        if (data.size() < need) {
            cx.error(b, ptr, "buffer is shorter than its byteLength");
            return false;
        }
        g.buffers.push_back(std::move(data));
    }
    return true;
}

// off + (count - 1) * stride + elem <= limit, without overflow (count >= 1, stride >= 1).
bool span_fits(u64 off, u64 count, u64 stride, u64 elem, u64 limit) {
    if (elem > limit || off > limit - elem) return false;
    const u64 avail = limit - elem - off;
    return count - 1 <= avail / stride;
}

// Accepted component kinds of an attribute (floats are always accepted).
enum AccessorKinds : u32 {
    kNormInt = 1,  // normalized BYTE / UNSIGNED_BYTE / SHORT / UNSIGNED_SHORT
    kRawUint = 2,  // plain UNSIGNED_BYTE / UNSIGNED_SHORT (joint indices; quantized positions and UVs)
    kRawInt = 4,   // plain BYTE / SHORT (KHR_mesh_quantization)
};

// A bufferView resolved to bytes: the buffer slice, or the EXT_meshopt_compression output.
struct ViewRef {
    const byte* data = nullptr;
    u64 size = 0;
    u64 stride = 0;  // byteStride; 0 = tightly packed
};

bool resolve_view(Ctx& cx, const Gltf& g, JsonValue view_v, JsonValue at, StringView ptr, ViewRef& out) {
    const JsonValue views = g.root.get("bufferViews");
    if (!nonneg(view_v) || !views.is_array() || view_v.as_number() >= views.size()) {
        cx.error(at, ptr, "bufferView index out of range");
        return false;
    }
    const auto vi = static_cast<usize>(view_v.as_number());
    const JsonValue bv = views.at(static_cast<u32>(vi));
    if (vi < g.decoded_stride.size() && g.decoded_stride[vi] != 0) {  // EXT_meshopt_compression: the decoded bytes
        out.data = g.decoded[vi].data();
        out.size = g.decoded[vi].size();
        out.stride = g.decoded_stride[vi];
        return true;
    }
    const JsonValue buf_v = bv.get("buffer");
    if (!nonneg(buf_v) || buf_v.as_number() >= static_cast<f64>(g.buffers.size())) {
        cx.error(bv, ptr, "bufferView.buffer out of range");
        return false;
    }
    const Array<byte>& buf = g.buffers[static_cast<usize>(buf_v.as_number())];
    const JsonValue off_v = bv.get("byteOffset"), len_v = bv.get("byteLength"), stride_v = bv.get("byteStride");
    if ((off_v.valid() && !nonneg(off_v)) || !nonneg(len_v) || (stride_v.valid() && !nonneg(stride_v))) {
        cx.error(bv, ptr, "bufferView byteOffset, byteLength and byteStride must be non-negative integers");
        return false;
    }
    const u64 off = off_v.valid() ? static_cast<u64>(off_v.as_number()) : 0;
    const u64 len = static_cast<u64>(len_v.as_number());
    if (off > buf.size() || len > buf.size() - off) {
        cx.error(bv, ptr, "accessor range exceeds its buffer view: the bufferView runs past its buffer");
        return false;
    }
    out.data = buf.data() + off;
    out.size = len;
    out.stride = stride_v.valid() ? static_cast<u64>(stride_v.as_number()) : 0;
    return true;
}

// The parts of an accessor every reader needs, validated.
struct AccessorDesc {
    JsonValue a;
    String ptr;  // "/accessors/N"
    u32 comps = 0;
    u32 ctype = 0;
    u32 csize = 0;
    bool normalized = false;
    u64 count = 0;
};

bool describe_accessor(Ctx& cx, const Gltf& g, JsonValue index_v, StringView ptr_for_errors, AccessorDesc& d) {
    if (!nonneg(index_v)) {
        cx.error(index_v, ptr_for_errors, "accessor index must be an integer");
        return false;
    }
    const auto ai = static_cast<u32>(std::min<f64>(index_v.as_number(), 4294967295.0));
    const JsonValue accs = g.root.get("accessors");
    if (!accs.is_array() || index_v.as_number() >= accs.size()) {
        cx.error(index_v, ptr_for_errors, "accessor index out of range");
        return false;
    }
    d.a = accs.at(ai);
    d.ptr.clear();
    append_pointer(d.ptr, "accessors", ai);
    static constexpr struct {
        StringView name;
        u32 n;
    } kTypes[] = {{"SCALAR", 1}, {"VEC2", 2}, {"VEC3", 3}, {"VEC4", 4}, {"MAT4", 16}};
    const StringView type = d.a.get("type").is_string() ? d.a.get("type").as_string() : StringView();
    d.comps = 0;
    for (const auto& t : kTypes)
        if (t.name == type) d.comps = t.n;
    const JsonValue ctype_v = d.a.get("componentType");
    d.ctype = nonneg(ctype_v) && ctype_v.as_number() < 65536 ? static_cast<u32>(ctype_v.as_number()) : 0;
    d.normalized = d.a.get("normalized").is_bool() && d.a.get("normalized").as_bool();
    switch (d.ctype) {
        case 5126: case 5125: d.csize = 4; break;  // FLOAT / UNSIGNED_INT (indices only)
        case 5121: case 5120: d.csize = 1; break;  // UNSIGNED_BYTE / BYTE
        case 5123: case 5122: d.csize = 2; break;  // UNSIGNED_SHORT / SHORT
        default: d.csize = 0; break;
    }
    return true;
}

// Checks count against the importer's limits (after the type checks, so their messages come first).
bool check_accessor_count(Ctx& cx, AccessorDesc& d) {
    const JsonValue count_v = d.a.get("count");
    if (!nonneg(count_v) || count_v.as_number() < 1) {
        cx.error(d.a, d.ptr, "count must be a positive integer");
        return false;
    }
    d.count = static_cast<u64>(count_v.as_number());
    if (d.count > kMaxAccessorCount || d.count * d.comps > kMaxAccessorFloats) {
        String m;
        appendf(m, "count %llu is over the importer's limit (%llu elements, %llu values per accessor)",
                static_cast<unsigned long long>(d.count), static_cast<unsigned long long>(kMaxAccessorCount),
                static_cast<unsigned long long>(kMaxAccessorFloats));
        cx.error(d.a, d.ptr, m);
        return false;
    }
    return true;
}

// The accessor's bytes as `count` tightly packed elements: the base bufferView (or zeros), then the sparse
// indices / values substituted (glTF 3.6.2.3). Every range is checked; sparse indices must be strictly
// increasing and below count.
bool materialize(Ctx& cx, const Gltf& g, const AccessorDesc& d, Array<byte>& packed) {
    const u64 elem = u64(d.csize) * d.comps;
    packed.clear();
    packed.resize(static_cast<usize>(d.count * elem), byte(0));
    if (const JsonValue bv_v = d.a.get("bufferView"); bv_v.valid()) {
        ViewRef vr;
        if (!resolve_view(cx, g, bv_v, d.a, d.ptr, vr)) return false;
        const JsonValue off_v = d.a.get("byteOffset");
        if (off_v.valid() && !nonneg(off_v)) {
            cx.error(d.a, d.ptr, "byteOffset must be a non-negative integer");
            return false;
        }
        const u64 acc_off = off_v.valid() ? static_cast<u64>(off_v.as_number()) : 0;
        const u64 stride = vr.stride ? vr.stride : elem;
        if (stride < elem || !span_fits(acc_off, d.count, stride, elem, vr.size)) {
            cx.error(d.a, d.ptr, "accessor range exceeds its buffer view");
            return false;
        }
        const byte* base = vr.data + acc_off;
        if (stride == elem) {
            std::memcpy(packed.data(), base, packed.size());
        } else {
            for (u64 e = 0; e < d.count; ++e) std::memcpy(packed.data() + e * elem, base + e * stride, static_cast<usize>(elem));
        }
    }
    const JsonValue sp = d.a.get("sparse");
    if (!sp.valid()) return true;
    String sptr(d.ptr);
    sptr += "/sparse";
    if (!sp.is_object()) {
        cx.error(sp, sptr, "sparse must be an object");
        return false;
    }
    const JsonValue cnt_v = sp.get("count");
    if (!nonneg(cnt_v) || cnt_v.as_number() < 1 || cnt_v.as_number() > static_cast<f64>(d.count)) {
        cx.error(sp, sptr, "sparse count must be an integer in 1..the accessor's count");
        return false;
    }
    const auto n = static_cast<u64>(cnt_v.as_number());
    String iptr(sptr), vptr(sptr);
    iptr += "/indices";
    vptr += "/values";
    const JsonValue ind = sp.get("indices"), vals = sp.get("values");
    if (!ind.is_object() || !vals.is_object()) {
        cx.error(sp, sptr, "sparse needs indices and values objects");
        return false;
    }
    const JsonValue ict_v = ind.get("componentType");
    const u32 ict = nonneg(ict_v) && ict_v.as_number() < 65536 ? static_cast<u32>(ict_v.as_number()) : 0;
    const u32 isz = ict == 5121 ? 1 : ict == 5123 ? 2 : ict == 5125 ? 4 : 0;
    if (isz == 0) {
        cx.error(ind, iptr, "sparse index componentType must be UNSIGNED_BYTE, UNSIGNED_SHORT or UNSIGNED_INT");
        return false;
    }
    ViewRef ivr, vvr;
    if (!resolve_view(cx, g, ind.get("bufferView"), ind, iptr, ivr)) return false;
    if (!resolve_view(cx, g, vals.get("bufferView"), vals, vptr, vvr)) return false;
    const JsonValue ioff_v = ind.get("byteOffset"), voff_v = vals.get("byteOffset");
    if ((ioff_v.valid() && !nonneg(ioff_v)) || (voff_v.valid() && !nonneg(voff_v))) {
        cx.error(sp, sptr, "byteOffset must be a non-negative integer");
        return false;
    }
    const u64 ioff = ioff_v.valid() ? static_cast<u64>(ioff_v.as_number()) : 0;
    const u64 voff = voff_v.valid() ? static_cast<u64>(voff_v.as_number()) : 0;
    if (!span_fits(ioff, n, isz, isz, ivr.size)) {
        cx.error(ind, iptr, "sparse indices exceed their buffer view");
        return false;
    }
    if (!span_fits(voff, n, elem, elem, vvr.size)) {
        cx.error(vals, vptr, "sparse values exceed their buffer view");
        return false;
    }
    u64 prev = 0;
    for (u64 i = 0; i < n; ++i) {
        u32 idx = 0;
        std::memcpy(&idx, ivr.data + ioff + i * isz, isz);  // little-endian
        if (idx >= d.count) {
            String m;
            appendf(m, "sparse index %u at position %llu is out of range (accessor count %llu)", idx,
                    static_cast<unsigned long long>(i), static_cast<unsigned long long>(d.count));
            cx.error(ind, iptr, m);
            return false;
        }
        if (i > 0 && idx <= prev) {
            String m;
            appendf(m, "sparse indices must be strictly increasing: %u follows %llu at position %llu", idx,
                    static_cast<unsigned long long>(prev), static_cast<unsigned long long>(i));
            cx.error(ind, iptr, m);
            return false;
        }
        prev = idx;
        std::memcpy(packed.data() + u64(idx) * elem, vvr.data + voff + i * elem, static_cast<usize>(elem));
    }
    return true;
}

f32 convert_component(u32 ctype, bool normalized, const byte* q) {
    f32 v = 0.0f;
    switch (ctype) {
        case 5126: std::memcpy(&v, q, 4); break;
        case 5121: v = normalized ? f32(u8(q[0])) / 255.0f : f32(u8(q[0])); break;
        case 5120: v = normalized ? math::max(f32(i8(q[0])) / 127.0f, -1.0f) : f32(i8(q[0])); break;
        case 5123: {
            u16 x;
            std::memcpy(&x, q, 2);
            v = normalized ? f32(x) / 65535.0f : f32(x);
            break;
        }
        case 5122: {
            i16 x;
            std::memcpy(&x, q, 2);
            v = normalized ? math::max(f32(x) / 32767.0f, -1.0f) : f32(x);
            break;
        }
        case 5125: {
            u32 x;
            std::memcpy(&x, q, 4);
            v = f32(x);  // indices: exact below 2^24, read raw by read_indices
            break;
        }
        default: break;
    }
    return v;
}

// Reads accessor `index` as `comps` floats per element (sparse substitution applied; integer types are
// converted as `kinds` allows: normalized ones to [0, 1] / [-1, 1], plain ones to their integer values).
bool read_accessor(Ctx& cx, const Gltf& g, JsonValue index_v, u32 comps, u32 kinds, Array<f32>& out, StringView ptr_for_errors) {
    out.clear();
    AccessorDesc d;
    if (!describe_accessor(cx, g, index_v, ptr_for_errors, d)) return false;
    if (d.comps != comps) {
        const StringView type = d.a.get("type").is_string() ? d.a.get("type").as_string() : StringView();
        String m;
        appendf(m, "expected a %u-component accessor, got '%.*s'", comps, static_cast<int>(type.size()), type.data());
        cx.error(d.a, d.ptr, m);
        return false;
    }
    if (d.csize == 0) {
        cx.error(d.a, d.ptr, "unsupported componentType");
        return false;
    }
    const bool is_float = d.ctype == 5126;
    const bool is_index = comps == 1 && !d.normalized && (d.ctype == 5121 || d.ctype == 5123 || d.ctype == 5125);
    const bool byte_or_short = d.ctype != 5125;
    const bool raw_uint = (kinds & kRawUint) && !d.normalized && (d.ctype == 5121 || d.ctype == 5123);
    const bool raw_int = (kinds & kRawInt) && !d.normalized && (d.ctype == 5120 || d.ctype == 5122);
    const bool norm_int = (kinds & kNormInt) && d.normalized && byte_or_short;
    if (!is_float && !is_index && !raw_uint && !raw_int && !norm_int) {
        cx.error(d.a, d.ptr, "component type not supported for this attribute");
        return false;
    }
    if (!check_accessor_count(cx, d)) return false;
    Array<byte> packed;
    if (!materialize(cx, g, d, packed)) return false;
    out.resize(static_cast<usize>(d.count * comps));
    const usize elem = usize(d.csize) * comps;
    for (usize e = 0; e < d.count; ++e)
        for (u32 c = 0; c < comps; ++c) {
            const f32 v = convert_component(d.ctype, d.normalized, packed.data() + e * elem + c * d.csize);
            if (!std::isfinite(v)) {
                cx.error(d.a, d.ptr, "non-finite value");
                return false;
            }
            out[e * comps + c] = v;
        }
    return true;
}

// Index accessors are read separately to keep u32 precision.
bool read_indices(Ctx& cx, const Gltf& g, JsonValue index_v, Array<u32>& out, StringView ptr_for_errors) {
    out.clear();
    AccessorDesc d;
    const JsonValue accs = g.root.get("accessors");
    if (!nonneg(index_v) || !accs.is_array() || index_v.as_number() >= accs.size()) {
        cx.error(index_v, ptr_for_errors, "indices accessor out of range");
        return false;
    }
    if (!describe_accessor(cx, g, index_v, ptr_for_errors, d)) return false;
    if (d.ctype != 5121 && d.ctype != 5123 && d.ctype != 5125) {
        cx.error(d.a, ptr_for_errors, "indices must be UNSIGNED_BYTE, UNSIGNED_SHORT or UNSIGNED_INT");
        return false;
    }
    if (d.comps != 1 || d.normalized) {
        cx.error(d.a, d.ptr, "indices must be a plain SCALAR accessor");
        return false;
    }
    if (!check_accessor_count(cx, d)) return false;
    Array<byte> packed;
    if (!materialize(cx, g, d, packed)) return false;
    out.resize(static_cast<usize>(d.count));
    for (usize i = 0; i < d.count; ++i) {
        u32 v = 0;
        std::memcpy(&v, packed.data() + i * d.csize, d.csize);  // little-endian
        out[i] = v;
    }
    return true;
}

// EXT_meshopt_compression: decodes every compressed bufferView into Gltf::decoded (the accessors read the
// decoded bytes through resolve_view). The fallback buffer is never read. The streams are untrusted: the
// geometry of each view (mode, stride, count, filter) is validated before the meshoptimizer decoder sees it,
// the decoded size is bounded by a constant and by the compressed size, and the decoder itself checks the
// stream against `buffer_size`.
bool decode_meshopt_views(Ctx& cx, Gltf& g) {
    const JsonValue views = g.root.get("bufferViews");
    if (!views.is_array()) return true;
    g.decoded.clear();
    g.decoded.resize(views.size());
    g.decoded_stride.clear();
    g.decoded_stride.resize(views.size(), 0u);
    u64 total = 0;
    for (u32 vi = 0; vi < views.size(); ++vi) {
        const JsonValue ext = views.at(vi).get("extensions").get("EXT_meshopt_compression");
        if (!ext.valid()) continue;
        String ptr;
        appendf(ptr, "/bufferViews/%u/extensions/EXT_meshopt_compression", vi);
        if (!ext.is_object()) {
            cx.error(ext, ptr, "must be an object");
            return false;
        }
        const JsonValue buf_v = ext.get("buffer"), off_v = ext.get("byteOffset"), len_v = ext.get("byteLength"),
                        stride_v = ext.get("byteStride"), count_v = ext.get("count"), mode_v = ext.get("mode"), filter_v = ext.get("filter");
        if (!nonneg(buf_v) || (off_v.valid() && !nonneg(off_v)) || !nonneg(len_v) || !nonneg(stride_v) || !nonneg(count_v)) {
            cx.error(ext, ptr, "buffer, byteOffset, byteLength, byteStride and count must be non-negative integers");
            return false;
        }
        if (buf_v.as_number() >= static_cast<f64>(g.buffers.size())) {
            cx.error(ext, ptr, "buffer index out of range");
            return false;
        }
        const Array<byte>& buf = g.buffers[static_cast<usize>(buf_v.as_number())];
        const u64 off = off_v.valid() ? static_cast<u64>(off_v.as_number()) : 0;
        const u64 len = static_cast<u64>(len_v.as_number());
        if (off > buf.size() || len > buf.size() - off) {
            cx.error(ext, ptr, "compressed range exceeds its buffer");
            return false;
        }
        const u64 stride = static_cast<u64>(stride_v.as_number());
        const u64 count = static_cast<u64>(count_v.as_number());
        const StringView mode = mode_v.is_string() ? mode_v.as_string() : StringView();
        const StringView filter = filter_v.is_string() ? filter_v.as_string() : StringView("NONE");
        enum { Attributes, Triangles, Indices } kind;
        if (mode == "ATTRIBUTES") kind = Attributes;
        else if (mode == "TRIANGLES") kind = Triangles;
        else if (mode == "INDICES") kind = Indices;
        else {
            cx.error(ext, ptr, "mode must be ATTRIBUTES, TRIANGLES or INDICES");
            return false;
        }
        if (count < 1 || len < 1) {
            cx.error(ext, ptr, "count and byteLength must be positive");
            return false;
        }
        if (kind == Attributes ? (stride < 4 || stride > 256 || stride % 4 != 0) : (stride != 2 && stride != 4)) {
            cx.error(ext, ptr,
                     kind == Attributes ? "byteStride must be a multiple of 4 in 4..256 for ATTRIBUTES"
                                        : "byteStride must be 2 or 4 for TRIANGLES and INDICES");
            return false;
        }
        if (kind == Triangles && count % 3 != 0) {
            cx.error(ext, ptr, "TRIANGLES count must be a multiple of 3");
            return false;
        }
        enum { None, Oct, Quat, Exp } flt = None;
        if (filter == "NONE") flt = None;
        else if (filter == "OCTAHEDRAL") flt = Oct;
        else if (filter == "QUATERNION") flt = Quat;
        else if (filter == "EXPONENTIAL") flt = Exp;
        else {
            cx.error(ext, ptr, "filter must be NONE, OCTAHEDRAL, QUATERNION or EXPONENTIAL");
            return false;
        }
        if (flt != None && (kind != Attributes || (flt == Oct && stride != 4 && stride != 8) || (flt == Quat && stride != 8))) {
            cx.error(ext, ptr, "filter does not fit the mode or byteStride (OCTAHEDRAL: 4 or 8, QUATERNION: 8, ATTRIBUTES only)");
            return false;
        }
        // EXT_meshopt_compression allows only the v0 vertex codec (header 0xa0); the index codecs have versions 0 and 1
        // (headers 0xe0 | v for TRIANGLES, 0xd0 | v for INDICES). A v1 vertex stream compresses constant attributes far
        // beyond the ratio bound below, so it is refused by name instead of failing as "over the limit".
        {
            const unsigned char header = static_cast<unsigned char>(buf[static_cast<usize>(off)]);  // len >= 1 and in range
            const bool ok = kind == Attributes ? header == 0xa0 : (header & 0xf0) == (kind == Triangles ? 0xe0 : 0xd0) && (header & 0x0f) <= 1;
            if (!ok) {
                String m;
                appendf(m, "unsupported meshopt stream header 0x%02x: %s", header,
                        kind == Attributes ? "only version 0 vertex streams (header 0xa0) are allowed by EXT_meshopt_compression"
                                           : "index streams must start with 0xe0 (TRIANGLES) or 0xd0 (INDICES) plus version 0 or 1");
                cx.error(ext, ptr, m);
                return false;
            }
        }
        // count <= 2^53 and stride <= 256: the product cannot overflow u64.
        const u64 out_size = count * stride;
        if (out_size > kMaxDecodedViewBytes || out_size > len * kMaxMeshoptRatio + 1024) {
            String m;
            appendf(m, "%llu decoded bytes from %llu compressed ones is over the importer's limit (%llu MiB per view, %llux the stream)",
                    static_cast<unsigned long long>(out_size), static_cast<unsigned long long>(len),
                    static_cast<unsigned long long>(kMaxDecodedViewBytes >> 20), static_cast<unsigned long long>(kMaxMeshoptRatio));
            cx.error(ext, ptr, m);
            return false;
        }
        total += out_size;
        if (total > kMaxDecodedTotalBytes) {
            cx.error(ext, ptr, "the file's compressed bufferViews decode to more than the importer's limit");
            return false;
        }
        Array<byte>& dst = g.decoded[vi];
        dst.resize(static_cast<usize>(out_size), byte(0));
        const auto* src = reinterpret_cast<const unsigned char*>(buf.data() + off);
        int rc = 0;
        switch (kind) {
            case Attributes: rc = meshopt_decodeVertexBuffer(dst.data(), count, stride, src, len); break;
            case Triangles: rc = meshopt_decodeIndexBuffer(dst.data(), count, stride, src, len); break;
            case Indices: rc = meshopt_decodeIndexSequence(dst.data(), count, stride, src, len); break;
        }
        if (rc != 0) {
            cx.error(ext, ptr, "meshopt stream is corrupt or truncated");
            return false;
        }
        if (flt == Oct) meshopt_decodeFilterOct(dst.data(), count, stride);
        else if (flt == Quat) meshopt_decodeFilterQuat(dst.data(), count, stride);
        else if (flt == Exp) meshopt_decodeFilterExp(dst.data(), count, stride);
        g.decoded_stride[vi] = static_cast<u32>(stride);
    }
    return true;
}

// Base color texture of material `mi` -> image URI resolved against the registry.
struct TextureRef {
    bool present = false;
    Guid guid;
    JsonValue at;
    String pointer;
    f32 uv_offset[2] = {0, 0};
    f32 uv_scale[2] = {1, 1};
    f32 uv_rotation = 0;
};

// The texture of a glTF textureInfo object `info` (inside material `mi`, at JSON pointer suffix
// `where`) -> image URI resolved against the registry. Absent info = no texture, not an error.
bool texture_ref(Ctx& cx, const Gltf& g, u32 mi, JsonValue info, StringView where, TextureRef& out) {
    out = TextureRef{};
    if (!info.valid()) return true;
    out.pointer.clear();
    append_pointer(out.pointer, "materials", mi);
    out.pointer += where;
    out.at = info;
    const JsonValue ti = info.get("index");
    const JsonValue textures = g.root.get("textures");
    if (!nonneg(ti) || !textures.is_array() || ti.as_number() >= textures.size()) {
        cx.error(info, out.pointer, "texture index out of range");
        return false;
    }
    if (nonneg(info.get("texCoord")) && info.get("texCoord").as_number() != 0)
        cx.warning(info, "only TEXCOORD_0 is imported; texCoord ignored");
    if (const JsonValue tt = info.get("extensions").get("KHR_texture_transform"); tt.is_object()) {
        const JsonValue off = tt.get("offset"), sc = tt.get("scale"), rot = tt.get("rotation");
        if (off.is_array() && off.size() == 2) {
            out.uv_offset[0] = static_cast<f32>(off.at(0).as_number());
            out.uv_offset[1] = static_cast<f32>(off.at(1).as_number());
        }
        if (sc.is_array() && sc.size() == 2) {
            out.uv_scale[0] = static_cast<f32>(sc.at(0).as_number());
            out.uv_scale[1] = static_cast<f32>(sc.at(1).as_number());
        }
        if (rot.is_number()) out.uv_rotation = static_cast<f32>(rot.as_number());
        if (nonneg(tt.get("texCoord")) && tt.get("texCoord").as_number() != 0)
            cx.warning(tt, "only TEXCOORD_0 is imported; KHR_texture_transform texCoord ignored");
    }
    const JsonValue tex = textures.at(static_cast<u32>(ti.as_number()));
    const JsonValue src = tex.get("source");
    const JsonValue images = g.root.get("images");
    if (!nonneg(src) || !images.is_array() || src.as_number() >= images.size()) {
        cx.error(tex, out.pointer, "texture has no image source");
        return false;
    }
    const JsonValue img = images.at(static_cast<u32>(src.as_number()));
    const JsonValue uri = img.get("uri");
    if (!uri.is_string() || uri.as_string().substr(0, 5) == "data:") {
        cx.error(img, out.pointer, "embedded images are not supported: reference a PNG/JPG file registered as a texture asset");
        return false;
    }
    String rel;
    if (!resolve_uri(cx.in().source_path, uri.as_string(), rel)) {
        cx.error(uri, out.pointer, "image URI must be relative and stay inside assets/");
        return false;
    }
    if (!cx.in().lookup || !cx.in().lookup->guid_for_path(rel, out.guid)) {
        String m("image '");
        m += rel;
        m += "' is not a registered asset";
        cx.error(uri, out.pointer, m, Errc::MissingRef);
        return false;
    }
    out.present = true;
    return true;
}

// The material's texture slots (M5a): base color, normal, metallic-roughness (+ occlusion packed
// into its R channel: the ORM convention), emissive, and a separate occlusion texture.
enum class TexSlot : u32 { BaseColor, Normal, MetallicRoughness, Emissive, Occlusion, Count };

bool material_texture(Ctx& cx, const Gltf& g, u32 mi, TexSlot slot, TextureRef& out) {
    const JsonValue mat = g.root.get("materials").at(mi);
    switch (slot) {
        case TexSlot::BaseColor:
            return texture_ref(cx, g, mi, mat.get("pbrMetallicRoughness").get("baseColorTexture"),
                               "/pbrMetallicRoughness/baseColorTexture", out);
        case TexSlot::Normal: return texture_ref(cx, g, mi, mat.get("normalTexture"), "/normalTexture", out);
        case TexSlot::MetallicRoughness:
            return texture_ref(cx, g, mi, mat.get("pbrMetallicRoughness").get("metallicRoughnessTexture"),
                               "/pbrMetallicRoughness/metallicRoughnessTexture", out);
        case TexSlot::Emissive: return texture_ref(cx, g, mi, mat.get("emissiveTexture"), "/emissiveTexture", out);
        case TexSlot::Occlusion: return texture_ref(cx, g, mi, mat.get("occlusionTexture"), "/occlusionTexture", out);
        case TexSlot::Count: break;
    }
    out = TextureRef{};
    return true;
}

bool base_color_texture(Ctx& cx, const Gltf& g, u32 mi, TextureRef& out) {
    return material_texture(cx, g, mi, TexSlot::BaseColor, out);
}

bool same_transform(const TextureRef& a, const TextureRef& b) {
    return a.uv_offset[0] == b.uv_offset[0] && a.uv_offset[1] == b.uv_offset[1] && a.uv_scale[0] == b.uv_scale[0] &&
           a.uv_scale[1] == b.uv_scale[1] && a.uv_rotation == b.uv_rotation;
}

void set_texture(asset::MaterialChunk& m, asset::MaterialSlot slot, Guid g) {
    m.textures[static_cast<u32>(slot)][0] = g.hi;
    m.textures[static_cast<u32>(slot)][1] = g.lo;
}

// The KHR_materials_clearcoat / _sheen / _anisotropy / _transmission / _ior factors (M-VISUAL2 4).
// Their textures are not imported: a warning names each one.
asset::MaterialExt convert_material_ext(Ctx& cx, const Gltf& g, JsonValue mat_index) {
    asset::MaterialExt e{};
    const JsonValue mats = g.root.get("materials");
    if (!mat_index.valid() || !nonneg(mat_index) || !mats.is_array() || mat_index.as_number() >= mats.size()) return e;
    const JsonValue ext = mats.at(static_cast<u32>(mat_index.as_number())).get("extensions");
    if (!ext.is_object()) return e;
    auto number = [](JsonValue o, StringView key, f32 fallback) {
        const JsonValue v = o.get(key);
        return v.is_number() ? static_cast<f32>(v.as_number()) : fallback;
    };
    auto no_textures = [&](JsonValue o, std::initializer_list<StringView> keys) {
        for (StringView k : keys)
            if (o.get(k).valid()) cx.warning(o.get(k), "this material extension's texture is not imported (its factor is)");
    };
    if (const JsonValue c = ext.get("KHR_materials_clearcoat"); c.is_object()) {
        e.clearcoat = number(c, "clearcoatFactor", 0.0f);
        e.clearcoat_roughness = number(c, "clearcoatRoughnessFactor", 0.0f);
        no_textures(c, {"clearcoatTexture", "clearcoatRoughnessTexture", "clearcoatNormalTexture"});
    }
    if (const JsonValue s = ext.get("KHR_materials_sheen"); s.is_object()) {
        if (const JsonValue col = s.get("sheenColorFactor"); col.is_array() && col.size() == 3)
            for (u32 i = 0; i < 3; ++i) e.sheen_color[i] = static_cast<f32>(col.at(i).as_number());
        e.sheen_roughness = number(s, "sheenRoughnessFactor", 0.0f);
        no_textures(s, {"sheenColorTexture", "sheenRoughnessTexture"});
    }
    if (const JsonValue a = ext.get("KHR_materials_anisotropy"); a.is_object()) {
        e.anisotropy = number(a, "anisotropyStrength", 0.0f);
        e.anisotropy_rotation = number(a, "anisotropyRotation", 0.0f);
        no_textures(a, {"anisotropyTexture"});
    }
    if (const JsonValue t = ext.get("KHR_materials_transmission"); t.is_object()) {
        e.transmission = number(t, "transmissionFactor", 0.0f);
        no_textures(t, {"transmissionTexture"});
    }
    if (const JsonValue i = ext.get("KHR_materials_ior"); i.is_object()) e.ior = number(i, "ior", 1.5f);
    return e;
}

asset::MaterialChunk convert_material(Ctx& cx, const Gltf& g, JsonValue mat_index, TextureRef& tex) {
    asset::MaterialChunk m{};
    m.base_color[0] = m.base_color[1] = m.base_color[2] = m.base_color[3] = 1.0f;
    m.metallic = 1.0f;  // glTF defaults
    m.roughness = 1.0f;
    m.alpha_cutoff = 0.5f;
    tex = TextureRef{};
    if (!mat_index.valid()) return m;
    const JsonValue mats = g.root.get("materials");
    if (!nonneg(mat_index) || !mats.is_array() || mat_index.as_number() >= mats.size()) {
        cx.error(mat_index, {}, "material index out of range");
        return m;
    }
    const auto mi = static_cast<u32>(mat_index.as_number());
    const JsonValue mat = mats.at(mi);
    const JsonValue pbr = mat.get("pbrMetallicRoughness");
    if (const JsonValue bc = pbr.get("baseColorFactor"); bc.is_array() && bc.size() == 4)
        for (u32 i = 0; i < 4; ++i) m.base_color[i] = static_cast<f32>(bc.at(i).as_number());
    if (pbr.get("metallicFactor").is_number()) m.metallic = static_cast<f32>(pbr.get("metallicFactor").as_number());
    if (pbr.get("roughnessFactor").is_number()) m.roughness = static_cast<f32>(pbr.get("roughnessFactor").as_number());
    f32 emissive_strength = 1.0f;
    if (const JsonValue es = mat.get("extensions").get("KHR_materials_emissive_strength").get("emissiveStrength"); es.is_number())
        emissive_strength = static_cast<f32>(es.as_number());
    if (const JsonValue em = mat.get("emissiveFactor"); em.is_array() && em.size() == 3)
        for (u32 i = 0; i < 3; ++i) m.emissive[i] = static_cast<f32>(em.at(i).as_number()) * emissive_strength;
    if (mat.get("alphaCutoff").is_number()) m.alpha_cutoff = static_cast<f32>(mat.get("alphaCutoff").as_number());
    if (const JsonValue mode = mat.get("alphaMode"); mode.is_string()) {
        if (mode.as_string() == "MASK") m.flags |= asset::MaterialFlags::AlphaMask;
        else if (mode.as_string() == "BLEND") m.flags |= asset::MaterialFlags::AlphaBlend;
    }
    if (mat.get("doubleSided").is_bool() && mat.get("doubleSided").as_bool()) m.flags |= asset::MaterialFlags::DoubleSided;
    if (mat.get("extensions").get("KHR_materials_unlit").valid()) m.flags |= asset::MaterialFlags::Unlit;
    if (base_color_texture(cx, g, mi, tex) && tex.present) set_texture(m, asset::MaterialSlot::BaseColor, tex.guid);
    // The other maps share TEXCOORD_0 and the base color's KHR_texture_transform (baked into the
    // UVs once per submesh); a map with a different transform samples with the base one.
    TextureRef normal, mr, emissive, occlusion;
    const bool ok = material_texture(cx, g, mi, TexSlot::Normal, normal) && material_texture(cx, g, mi, TexSlot::MetallicRoughness, mr) &&
                    material_texture(cx, g, mi, TexSlot::Emissive, emissive) &&
                    material_texture(cx, g, mi, TexSlot::Occlusion, occlusion);
    if (!ok) return m;
    const TextureRef* uv_owner = tex.present ? &tex : nullptr;
    for (const TextureRef* t : {&normal, &mr, &emissive, &occlusion}) {
        if (!t->present) continue;
        if (!uv_owner) uv_owner = t;
        else if (!same_transform(*t, *uv_owner))
            cx.warning(t->at, "KHR_texture_transform differs from the material's first texture; one UV transform per material is imported");
    }
    if (!tex.present && uv_owner) {  // no base color map: the first other map's transform is baked
        tex = *uv_owner;
        tex.guid = Guid{};
    }
    if (normal.present) {
        set_texture(m, asset::MaterialSlot::Normal, normal.guid);
        if (const JsonValue sc = normal.at.get("scale"); sc.is_number() && sc.as_number() != 1.0)
            cx.warning(normal.at, "normalTexture.scale is ignored (1.0 is used)");
    }
    if (mr.present) set_texture(m, asset::MaterialSlot::MetallicRoughness, mr.guid);
    if (emissive.present) set_texture(m, asset::MaterialSlot::Emissive, emissive.guid);
    if (occlusion.present) {
        if (mr.present && occlusion.guid == mr.guid) {
            m.flags |= asset::MaterialFlags::OcclusionInMR;
        } else if (!mr.present) {
            // An occlusion-only map: sample it through the ORM slot with roughness/metallic left
            // to the factors (its G/B channels are not roughness/metallic data).
            set_texture(m, asset::MaterialSlot::MetallicRoughness, occlusion.guid);
            m.flags |= asset::MaterialFlags::OcclusionInMR | asset::MaterialFlags::OcclusionOnly;
        } else {
            cx.warning(occlusion.at, "a separate occlusion texture is ignored: pack occlusion into the metallicRoughness "
                                     "texture's R channel (ORM)");
        }
        if (const JsonValue st = occlusion.at.get("strength"); st.is_number() && st.as_number() != 1.0)
            cx.warning(occlusion.at, "occlusionTexture.strength is ignored (1.0 is used)");
    }
    return m;
}

// The four largest influences of each vertex over `sets` JOINTS_n / WEIGHTS_n pairs (ties: lower set, then lower
// slot), kept in set/slot order and renormalized; unused slots are joint 0, weight 0. Returns the number of
// vertices that lost more than 1% of their total weight.
u32 reduce_influences(const Array<Array<f32>>& joints, const Array<Array<f32>>& weights, usize vcount, Array<f32>& out_joints,
                      Array<f32>& out_weights) {
    const usize sets = joints.size();
    out_joints.clear();
    out_weights.clear();
    out_joints.resize(vcount * 4, 0.0f);
    out_weights.resize(vcount * 4, 0.0f);
    u32 lossy = 0;
    f32 cand_w[4 * kMaxInfluenceSets];
    f32 cand_j[4 * kMaxInfluenceSets];
    bool picked[4 * kMaxInfluenceSets];
    for (usize v = 0; v < vcount; ++v) {
        f64 total = 0.0;
        const usize n = sets * 4;
        for (usize s = 0; s < sets; ++s)
            for (usize c = 0; c < 4; ++c) {
                cand_w[s * 4 + c] = math::max(weights[s][v * 4 + c], 0.0f);
                cand_j[s * 4 + c] = joints[s][v * 4 + c];
                picked[s * 4 + c] = false;
                total += cand_w[s * 4 + c];
            }
        for (int k = 0; k < 4; ++k) {
            usize best = n;
            for (usize i = 0; i < n; ++i)
                if (!picked[i] && cand_w[i] > 0.0f && (best == n || cand_w[i] > cand_w[best])) best = i;
            if (best == n) break;
            picked[best] = true;
        }
        f64 kept = 0.0;
        usize slot = 0;
        for (usize i = 0; i < n; ++i)
            if (picked[i]) {
                out_joints[v * 4 + slot] = cand_j[i];
                out_weights[v * 4 + slot] = cand_w[i];
                kept += cand_w[i];
                ++slot;
            }
        if (kept > 0.0)
            for (usize c = 0; c < 4; ++c) out_weights[v * 4 + c] = static_cast<f32>(out_weights[v * 4 + c] / kept);
        if (total > 0.0 && (total - kept) / total > 0.01) ++lossy;
    }
    return lossy;
}

// The attribute names of the dropped-feature warnings: the first COLOR_n / TEXCOORD_n (n >= 1) of a primitive.
void warn_dropped_attributes(Ctx& cx, JsonValue attrs, StringView ptr) {
    for (u32 i = 0; i < attrs.size(); ++i) {
        const StringView k = attrs.key_at(i);
        if (k.substr(0, 6) == "COLOR_") {
            String m;
            appendf(m, "vertex colors are not imported (first: %.*s/attributes/%.*s)", static_cast<int>(ptr.size()), ptr.data(),
                    static_cast<int>(k.size()), k.data());
            cx.warn_once(kWarnColor, attrs, m);
        } else if (k.substr(0, 9) == "TEXCOORD_" && k != "TEXCOORD_0") {
            String m;
            appendf(m, "only TEXCOORD_0 is imported; %.*s is dropped (first: %.*s/attributes/%.*s)", static_cast<int>(k.size()), k.data(),
                    static_cast<int>(ptr.size()), ptr.data(), static_cast<int>(k.size()), k.data());
            cx.warn_once(kWarnTexcoord, attrs, m);
        }
    }
}

// Adds sum_t weights[t] * target_t[`name`] (the morph deltas) to `base` (`comps` floats per vertex). Targets with
// weight 0 are not read.
bool apply_morph_deltas(Ctx& cx, const Gltf& g, JsonValue targets, JsonValue weights, StringView name, StringView ptr,
                        Array<f32>& base) {
    const u32 n = std::min(targets.size(), weights.size());
    for (u32 t = 0; t < n; ++t) {
        const JsonValue wv = weights.at(t);
        const f32 w = wv.is_number() ? static_cast<f32>(wv.as_number()) : 0.0f;
        if (w == 0.0f) continue;
        const JsonValue acc = targets.at(t).get(name);
        if (!acc.valid()) continue;
        String tptr(ptr);
        appendf(tptr, "/targets/%u", t);
        Array<f32> delta;
        if (!read_accessor(cx, g, acc, 3, kNormInt | kRawInt, delta, tptr)) return false;
        if (delta.size() != base.size()) {
            cx.error(targets.at(t), tptr, "morph target accessor count differs from the primitive's vertex count");
            return false;
        }
        for (usize i = 0; i < base.size(); ++i) base[i] += w * delta[i];
    }
    return true;
}

// `skin` is the glTF skin of the primitive's node (-1 = rigid) with `skin_joints` joints. `morph_weights` is the
// node's (else the mesh's) default weights array, or invalid.
// With `lod_of` set the primitive is an authored LOD of submesh `*lod_of`: its vertices are
// appended quantized with that submesh's bounds and `*lod_out` receives the range (no submesh).
bool import_primitive(Ctx& cx, const Gltf& g, JsonValue prim, StringView ptr, u32 node, i32 skin, u32 skin_joints,
                      JsonValue morph_weights, Geometry& geo, const u32* lod_of = nullptr, asset::MeshLod* lod_out = nullptr) {
    if (const JsonValue mode = prim.get("mode"); mode.valid() && !(nonneg(mode) && mode.as_number() == 4)) {
        cx.error(mode, ptr, "only triangle lists (mode 4) are supported");
        return false;
    }
    const JsonValue attrs = prim.get("attributes");
    if (!attrs.get("POSITION").valid()) {
        cx.error(prim, ptr, "primitive has no POSITION attribute");
        return false;
    }
    if (const JsonValue ext = prim.get("extensions").get("KHR_draco_mesh_compression"); ext.valid()) {
        // Used but not required: the fallback accessors carry the geometry, unless the exporter left them out.
        // Every attribute accessor (and the indices) must carry data; one without a bufferView or sparse part is the
        // exporter's stripped Draco placeholder.
        auto has_fallback = [&](JsonValue acc_index) {
            AccessorDesc pd;
            if (!describe_accessor(cx, g, acc_index, ptr, pd)) return false;
            if (!pd.a.get("bufferView").valid() && !pd.a.get("sparse").valid()) {
                cx.error(prim, ptr,
                         "the primitive is Draco-compressed and has no uncompressed fallback: re-export the model with meshopt "
                         "compression (gltfpack -c, or gltf-transform meshopt) or without compression");
                return false;
            }
            return true;
        };
        for (u32 i = 0; i < attrs.size(); ++i)
            if (!has_fallback(attrs.at(i))) return false;
        if (prim.get("indices").valid() && !has_fallback(prim.get("indices"))) return false;
    }
    warn_dropped_attributes(cx, attrs, ptr);
    JsonValue targets = prim.get("targets");
    if (targets.valid() && !targets.is_array()) {
        cx.error(targets, ptr, "targets must be an array");
        return false;
    }
    if (targets.valid() && targets.size() > kMaxMorphTargets) {
        String m;
        appendf(m, "%u morph targets is over the importer's limit of %u", targets.size(), kMaxMorphTargets);
        cx.error(targets, ptr, m);
        return false;
    }
    if (skin >= 0 && !(attrs.get("JOINTS_0").valid() && attrs.get("WEIGHTS_0").valid())) {
        cx.warning(prim, "primitive of a skinned node has no JOINTS_0/WEIGHTS_0: imported rigid");
        skin = -1;
    }

    Array<f32> pos, nrm, uv, joints, weights;
    if (!read_accessor(cx, g, attrs.get("POSITION"), 3, kNormInt | kRawUint | kRawInt, pos, ptr)) return false;
    const usize vcount = pos.size() / 3;
    if (attrs.get("NORMAL").valid() && !read_accessor(cx, g, attrs.get("NORMAL"), 3, kNormInt, nrm, ptr)) return false;
    if (attrs.get("TEXCOORD_0").valid() &&
        !read_accessor(cx, g, attrs.get("TEXCOORD_0"), 2, kNormInt | kRawUint | kRawInt, uv, ptr))
        return false;
    u32 influence_sets = 0;
    Array<Array<f32>> set_joints, set_weights;
    if (skin >= 0) {
        for (u32 s = 0; s < kMaxInfluenceSets; ++s) {
            String jn, wn;
            appendf(jn, "JOINTS_%u", s);
            appendf(wn, "WEIGHTS_%u", s);
            const JsonValue jv = attrs.get(jn.view()), wv = attrs.get(wn.view());
            if (!jv.valid() && !wv.valid()) continue;
            if (!jv.valid() || !wv.valid()) {
                cx.warning(prim, "JOINTS_n without WEIGHTS_n (or the reverse): the set is ignored");
                continue;
            }
            Array<f32> sj, sw;
            if (!read_accessor(cx, g, jv, 4, kRawUint, sj, ptr)) return false;
            if (!read_accessor(cx, g, wv, 4, kNormInt, sw, ptr)) return false;
            if (sj.size() != vcount * 4 || sw.size() != vcount * 4) {
                cx.error(attrs, ptr, "attribute counts differ");
                return false;
            }
            for (usize v = 0; v < vcount; ++v)
                for (int c = 0; c < 4; ++c)
                    if (math::max(sw[v * 4 + c], 0.0f) > 0.0f && sj[v * 4 + c] >= f32(skin_joints)) {
                        String m;
                        appendf(m, "JOINTS_%u names a joint outside the node's skin", s);
                        cx.error(jv, ptr, m);
                        return false;
                    }
            set_joints.push_back(std::move(sj));
            set_weights.push_back(std::move(sw));
            ++influence_sets;
        }
        for (u32 i = 0; i < attrs.size(); ++i) {  // any JOINTS_n / WEIGHTS_n with n >= 8, whatever its digits
            const StringView k = attrs.key_at(i);
            const usize skip = k.substr(0, 7) == "JOINTS_" ? 7 : k.substr(0, 8) == "WEIGHTS_" ? 8 : 0;
            if (skip == 0 || k.size() == skip) continue;
            u64 n = 0;
            bool digits = true;
            for (usize c = skip; c < k.size() && digits; ++c) {
                digits = k[c] >= '0' && k[c] <= '9';
                if (digits && n < 1000000) n = n * 10 + static_cast<u64>(k[c] - '0');
            }
            if (digits && n >= kMaxInfluenceSets) {
                String m;
                appendf(m, "%.*s: more than %u JOINTS_n / WEIGHTS_n sets are not supported", static_cast<int>(k.size()), k.data(),
                        kMaxInfluenceSets);
                cx.error(attrs, ptr, m);
                return false;
            }
        }
        if (influence_sets == 0) {
            cx.error(attrs, ptr, "JOINTS_0 / WEIGHTS_0 are unusable");
            return false;
        }
        if (influence_sets == 1) {  // up to 4 influences: passed through untouched
            joints = std::move(set_joints[0]);
            weights = std::move(set_weights[0]);
        } else {
            const u32 lossy = reduce_influences(set_joints, set_weights, vcount, joints, weights);
            if (lossy > 0) {
                String m;
                appendf(m, "%u of %zu vertices lost more than 1%% of their skin weight: only the 4 largest of %u influences are kept",
                        lossy, vcount, influence_sets * 4);
                cx.warning(prim, m);
            }
        }
    }
    if ((!nrm.empty() && nrm.size() != vcount * 3) || (!uv.empty() && uv.size() != vcount * 2) ||
        (skin >= 0 && (joints.size() != vcount * 4 || weights.size() != vcount * 4))) {
        cx.error(attrs, ptr, "attribute counts differ");
        return false;
    }
    if (targets.valid() && targets.size() > 0) {
        bool baked = false;
        for (u32 t = 0; t < std::min(targets.size(), morph_weights.is_array() ? morph_weights.size() : 0u); ++t)
            baked = baked || (morph_weights.at(t).is_number() && morph_weights.at(t).as_number() != 0.0);
        if (baked) {
            u64 weighted = 0;
            for (u32 t = 0; t < std::min(targets.size(), morph_weights.size()); ++t)
                weighted += morph_weights.at(t).is_number() && morph_weights.at(t).as_number() != 0.0;
            if (!cx.charge_morph(targets, ptr, u64(vcount) * (1 + weighted))) return false;  // before any delta is read
            if (!apply_morph_deltas(cx, g, targets, morph_weights, "POSITION", ptr, pos)) return false;
            if (!nrm.empty() && !apply_morph_deltas(cx, g, targets, morph_weights, "NORMAL", ptr, nrm)) return false;
            for (f32 x : pos)
                if (!std::isfinite(x)) {
                    cx.error(targets, ptr, "the default morph weights produce non-finite positions");
                    return false;
                }
            for (f32 x : nrm)
                if (!std::isfinite(x)) {
                    cx.error(targets, ptr, "the default morph weights produce non-finite normals");
                    return false;
                }
        }
        String m;
        appendf(m, "%u morph target%s not imported (static import)%s", targets.size(), targets.size() == 1 ? " is" : "s are",
                baked ? "; the default weights are baked into POSITION/NORMAL" : "");
        appendf(m, " (first: %.*s)", static_cast<int>(ptr.size()), ptr.data());
        cx.warn_once(kWarnMorph, prim, m);  // once per file, not per instance
    }
    Array<u32> idx;
    if (prim.get("indices").valid()) {
        if (!read_indices(cx, g, prim.get("indices"), idx, ptr)) return false;
    } else {
        idx.resize(vcount);
        for (usize i = 0; i < vcount; ++i) idx[i] = static_cast<u32>(i);
    }
    if (idx.size() % 3 != 0) {
        cx.error(prim, ptr, "index count is not a multiple of 3");
        return false;
    }
    for (u32 i : idx)
        if (i >= vcount) {
            cx.error(prim, ptr, "index out of range of the vertex attributes");
            return false;
        }
    if (!cx.charge(prim, ptr, vcount, idx.size())) return false;

    TextureRef tex;
    const asset::MaterialChunk mat = convert_material(cx, g, prim.get("material"), tex);
    const asset::MaterialExt mat_ext = lod_of ? asset::MaterialExt{} : convert_material_ext(cx, g, prim.get("material"));
    if (cx.errors()) return false;

    Array<f32> uv_baked;
    if (tex.present) {  // KHR_texture_transform: offset * rotation * scale, baked into the UVs
        uv_baked.resize(vcount * 2);
        for (usize v = 0; v < vcount; ++v) {
            f32 u = uv.empty() ? 0.0f : uv[v * 2], w = uv.empty() ? 0.0f : uv[v * 2 + 1];
            const f32 cs = std::cos(tex.uv_rotation), sn = std::sin(tex.uv_rotation);
            const f32 su = u * tex.uv_scale[0], sv = w * tex.uv_scale[1];
            u = tex.uv_offset[0] + cs * su + sn * sv;
            w = tex.uv_offset[1] - sn * su + cs * sv;
            uv_baked[v * 2] = u;
            uv_baked[v * 2 + 1] = w;
        }
    }

    detail::Primitive out;
    out.positions = pos.span();
    out.normals = nrm.span();
    out.uvs = tex.present ? uv_baked.span() : uv.span();
    out.joints = joints.span();
    out.weights = weights.span();
    out.indices = idx.span();
    out.material = mat;
    out.material_ext = mat_ext;
    out.node = node;
    out.skin = skin;
    out.skin_joints = skin_joints;
    if (lod_of)
        *lod_out = detail::add_authored_lod(out, *lod_of, geo);
    else
        detail::add_submesh(out, geo);
    return true;
}

// Level n of name, or 0 when `name` is not `<base>_LOD<n>` (n = 1..kMaxMeshLods - 1).
u32 lod_suffix(StringView name, StringView& base) {
    if (name.size() < 6) return 0;
    const char d = name[name.size() - 1];
    if (d < '1' || d >= static_cast<char>('0' + asset::kMaxMeshLods)) return 0;
    if (name.substr(name.size() - 5, 4) != StringView("_LOD")) return 0;
    base = name.substr(0, name.size() - 5);
    return static_cast<u32>(d - '0');
}

// Decomposes a TRS matrix (column-major glTF `matrix`); shear is not representable.
void decompose(const f32 m[16], asset::MeshNode& n) {
    const vec3 c0{m[0], m[1], m[2]}, c1{m[4], m[5], m[6]}, c2{m[8], m[9], m[10]};
    f32 sx = length(c0), sy = length(c1), sz = length(c2);
    if (dot(cross(c0, c1), c2) < 0.0f) sx = -sx;
    n.translation[0] = m[12];
    n.translation[1] = m[13];
    n.translation[2] = m[14];
    n.scale[0] = sx;
    n.scale[1] = sy;
    n.scale[2] = sz;
    const vec3 r0 = sx != 0.0f ? c0 / sx : vec3{1, 0, 0};
    const vec3 r1 = sy != 0.0f ? c1 / sy : vec3{0, 1, 0};
    const vec3 r2 = sz != 0.0f ? c2 / sz : vec3{0, 0, 1};
    // Rotation matrix (columns r0 r1 r2) -> quaternion (Shepperd).
    const f32 tr = r0.x + r1.y + r2.z;
    quat q;
    if (tr > 0.0f) {
        const f32 s = std::sqrt(tr + 1.0f) * 2.0f;
        q = {(r1.z - r2.y) / s, (r2.x - r0.z) / s, (r0.y - r1.x) / s, 0.25f * s};
    } else if (r0.x > r1.y && r0.x > r2.z) {
        const f32 s = std::sqrt(1.0f + r0.x - r1.y - r2.z) * 2.0f;
        q = {0.25f * s, (r1.x + r0.y) / s, (r2.x + r0.z) / s, (r1.z - r2.y) / s};
    } else if (r1.y > r2.z) {
        const f32 s = std::sqrt(1.0f + r1.y - r0.x - r2.z) * 2.0f;
        q = {(r1.x + r0.y) / s, 0.25f * s, (r2.y + r1.z) / s, (r2.x - r0.z) / s};
    } else {
        const f32 s = std::sqrt(1.0f + r2.z - r0.x - r1.y) * 2.0f;
        q = {(r2.x + r0.z) / s, (r2.y + r1.z) / s, 0.25f * s, (r0.y - r1.x) / s};
    }
    q = normalize(q);
    n.rotation[0] = q.x;
    n.rotation[1] = q.y;
    n.rotation[2] = q.z;
    n.rotation[3] = q.w;
}

u32 add_string(Array<char>& strings, StringView s, u32& size_out) {
    const auto at = static_cast<u32>(strings.size());
    for (char c : s) strings.push_back(c);
    size_out = static_cast<u32>(s.size());
    return at;
}

struct Output {
    Geometry geo;
    Array<asset::MeshNode> nodes;
    Array<asset::MeshClip> clips;
    Array<asset::MeshChannel> channels;
    Array<f32> keys;
    Array<char> strings;
};

// Node tree in depth-first order (parents first) from the default scene's roots. With `skinning`
// off, skinned nodes import their primitives rigidly (the static character path).
struct AuthoredLod {
    u32 gltf_mesh;
    u32 level;
    String base;
    JsonValue node_weights;  // the LOD node's default morph weights (else the mesh's apply)
};

bool import_nodes(Ctx& cx, const Gltf& g, f32 import_scale, bool skinning, Output& out, Array<i32>& remap,
                  Array<AuthoredLod>& authored) {
    const JsonValue nodes = g.root.get("nodes");
    const u32 count = nodes.is_array() ? nodes.size() : 0;
    remap.clear();
    remap.resize(count, -1);
    Array<u32> roots;
    const JsonValue scenes = g.root.get("scenes");
    if (scenes.is_array() && scenes.size() > 0) {
        u32 si = 0;
        if (nonneg(g.root.get("scene"))) si = static_cast<u32>(g.root.get("scene").as_number());
        if (si >= scenes.size()) {
            cx.error(g.root.get("scene"), "/scene", "scene index out of range");
            return false;
        }
        const JsonValue sn = scenes.at(si).get("nodes");
        for (u32 i = 0; sn.is_array() && i < sn.size(); ++i)
            if (nonneg(sn.at(i)) && sn.at(i).as_number() < count) roots.push_back(static_cast<u32>(sn.at(i).as_number()));
    } else {  // no scene: every node that is nobody's child
        Array<bool> child;
        child.resize(count, false);
        for (u32 i = 0; i < count; ++i)
            if (const JsonValue ch = nodes.at(i).get("children"); ch.is_array())
                for (u32 k = 0; k < ch.size(); ++k)
                    if (nonneg(ch.at(k)) && ch.at(k).as_number() < count) child[static_cast<usize>(ch.at(k).as_number())] = true;
        for (u32 i = 0; i < count; ++i)
            if (!child[i]) roots.push_back(i);
    }

    struct Item {
        u32 gltf;
        i32 parent;
    };
    Array<Item> stack;
    for (usize r = roots.size(); r-- > 0;) stack.push_back({roots[r], -1});
    while (!stack.empty()) {
        const Item it = stack.back();
        stack.pop_back();
        String ptr;
        append_pointer(ptr, "nodes", it.gltf);
        const JsonValue n = nodes.at(it.gltf);
        if (remap[it.gltf] >= 0) {
            cx.error(n, ptr, "node is reachable twice (cycle or shared child)");
            return false;
        }
        asset::MeshNode mn{};
        mn.parent = it.parent;
        mn.scale[0] = mn.scale[1] = mn.scale[2] = 1.0f;
        mn.rotation[3] = 1.0f;
        if (const JsonValue m = n.get("matrix"); m.is_array() && m.size() == 16) {
            f32 mm[16];
            for (u32 i = 0; i < 16; ++i) mm[i] = static_cast<f32>(m.at(i).as_number());
            decompose(mm, mn);
        } else {
            if (const JsonValue t = n.get("translation"); t.is_array() && t.size() == 3)
                for (u32 i = 0; i < 3; ++i) mn.translation[i] = static_cast<f32>(t.at(i).as_number());
            if (const JsonValue s = n.get("scale"); s.is_array() && s.size() == 3)
                for (u32 i = 0; i < 3; ++i) mn.scale[i] = static_cast<f32>(s.at(i).as_number());
            if (const JsonValue q = n.get("rotation"); q.is_array() && q.size() == 4)
                for (u32 i = 0; i < 4; ++i) mn.rotation[i] = static_cast<f32>(q.at(i).as_number());
        }
        if (it.parent < 0 && import_scale != 1.0f)
            for (int i = 0; i < 3; ++i) {
                mn.translation[i] *= import_scale;
                mn.scale[i] *= import_scale;
            }
        i32 skin = -1;
        u32 skin_joints = 0;
        if (const JsonValue skin_v = n.get("skin"); skin_v.valid() && skinning && n.get("mesh").valid()) {
            const JsonValue skins = g.root.get("skins");
            if (!nonneg(skin_v) || !skins.is_array() || skin_v.as_number() >= skins.size()) {
                cx.error(skin_v, ptr, "skin index out of range");
                return false;
            }
            skin = static_cast<i32>(skin_v.as_number());
            const JsonValue js = skins.at(static_cast<u32>(skin)).get("joints");
            skin_joints = js.is_array() ? js.size() : 0;
            if (skin_joints == 0 || skin_joints > asset::kMaxSkinJoints) {
                String m;
                appendf(m, "a skin needs 1..%u joints, this one has %u", asset::kMaxSkinJoints, skin_joints);
                String sptr;
                append_pointer(sptr, "skins", static_cast<u32>(skin));
                cx.error(skins.at(static_cast<u32>(skin)), sptr, m);
                return false;
            }
        }
        const StringView name = n.get("name").is_string() ? n.get("name").as_string() : StringView();
        mn.name_offset = add_string(out.strings, name, mn.name_size);
        const auto self = static_cast<i32>(out.nodes.size());
        remap[it.gltf] = self;
        out.nodes.push_back(mn);

        if (const JsonValue mesh_v = n.get("mesh"); mesh_v.valid()) {
            const JsonValue meshes = g.root.get("meshes");
            if (!nonneg(mesh_v) || !meshes.is_array() || mesh_v.as_number() >= meshes.size()) {
                cx.error(mesh_v, ptr, "mesh index out of range");
                return false;
            }
            const auto mi = static_cast<u32>(mesh_v.as_number());
            const JsonValue prims = meshes.at(mi).get("primitives");
            StringView lod_base;
            const u32 level = lod_suffix(name, lod_base);
            if (level > 0) {  // not drawn itself: a level of `lod_base` (attached after the tree)
                AuthoredLod a;
                a.gltf_mesh = mi;
                a.level = level;
                a.base.append(lod_base);
                a.node_weights = n.get("weights");
                authored.push_back(std::move(a));
            }
            for (u32 p = 0; level == 0 && prims.is_array() && p < prims.size(); ++p) {
                String pptr;
                appendf(pptr, "/meshes/%u/primitives/%u", mi, p);
                JsonValue mw = n.get("weights");
                if (!mw.is_array()) mw = meshes.at(mi).get("weights");
                if (!import_primitive(cx, g, prims.at(p), pptr, static_cast<u32>(self), skin, skin_joints, mw, out.geo)) return false;
            }
        }
        if (const JsonValue ch = n.get("children"); ch.is_array())
            for (u32 k = ch.size(); k-- > 0;) {
                if (!nonneg(ch.at(k)) || ch.at(k).as_number() >= count) {
                    cx.error(ch.at(k), ptr, "child index out of range");
                    return false;
                }
                stack.push_back({static_cast<u32>(ch.at(k).as_number()), self});
            }
    }
    return true;
}

// `<name>_LOD<n>` meshes onto node `<name>`'s submeshes (primitive p -> its p-th submesh).
bool attach_authored_lods(Ctx& cx, const Gltf& g, const Array<AuthoredLod>& authored, Output& out) {
    const JsonValue meshes = g.root.get("meshes");
    for (const AuthoredLod& a : authored) {
        u32 node = ~0u;
        for (u32 i = 0; i < out.nodes.size() && node == ~0u; ++i) {
            const asset::MeshNode& n = out.nodes[i];
            if (StringView(out.strings.data() + n.name_offset, n.name_size) == a.base.view()) node = i;
        }
        Array<u32> subs;
        for (u32 si = 0; si < out.geo.submeshes.size(); ++si)
            if (out.geo.submeshes[si].node == node) subs.push_back(si);
        if (subs.empty()) {
            String m;
            appendf(m, "LOD mesh for '%.*s', which has no mesh primitives: ignored", static_cast<int>(a.base.size()), a.base.c_str());
            cx.warning(meshes.at(a.gltf_mesh), m);
            continue;
        }
        const JsonValue prims = meshes.at(a.gltf_mesh).get("primitives");
        for (u32 p = 0; prims.is_array() && p < prims.size() && p < subs.size(); ++p) {
            const u32 si = subs[p];
            if (out.geo.submesh_skin[si] >= 0) {
                cx.warning(prims.at(p), "authored LODs of skinned primitives are ignored");
                continue;
            }
            String pptr;
            appendf(pptr, "/meshes/%u/primitives/%u", a.gltf_mesh, p);
            asset::MeshLod lod{};
            JsonValue mw = a.node_weights;
            if (!mw.is_array()) mw = meshes.at(a.gltf_mesh).get("weights");
            if (!import_primitive(cx, g, prims.at(p), pptr, node, -1, 0, mw, out.geo, &si, &lod)) return false;
            lod.reserved[0] = a.level;  // sort key below, cleared after
            out.geo.lods[si].push_back(lod);
        }
    }
    for (Array<asset::MeshLod>& levels : out.geo.lods) {
        std::stable_sort(levels.begin(), levels.end(),
                         [](const asset::MeshLod& x, const asset::MeshLod& y) { return x.reserved[0] < y.reserved[0]; });
        for (asset::MeshLod& l : levels) l.reserved[0] = 0;
    }
    return true;
}

bool import_animations(Ctx& cx, const Gltf& g, const Array<i32>& remap, f32 import_scale, Output& out) {
    const JsonValue anims = g.root.get("animations");
    if (!anims.is_array()) return true;
    for (u32 ai = 0; ai < anims.size(); ++ai) {
        const JsonValue a = anims.at(ai);
        String ptr;
        append_pointer(ptr, "animations", ai);
        asset::MeshClip clip{};
        clip.first_channel = static_cast<u32>(out.channels.size());
        String name;
        if (a.get("name").is_string()) name = a.get("name").as_string();
        else appendf(name, "animation%u", ai);
        clip.name_offset = add_string(out.strings, name, clip.name_size);
        const JsonValue channels = a.get("channels"), samplers = a.get("samplers");
        for (u32 ci = 0; channels.is_array() && ci < channels.size(); ++ci) {
            const JsonValue ch = channels.at(ci);
            String cptr(ptr);
            appendf(cptr, "/channels/%u", ci);
            const JsonValue target = ch.get("target");
            const StringView path = target.get("path").is_string() ? target.get("path").as_string() : StringView();
            asset::MeshChannel mc{};
            u32 width = 3;
            if (path == "translation") mc.path = asset::AnimPath::Translation;
            else if (path == "rotation") {
                mc.path = asset::AnimPath::Rotation;
                width = 4;
            } else if (path == "scale") mc.path = asset::AnimPath::Scale;
            else {
                cx.warning(ch, "animation path ignored (only translation/rotation/scale are imported)");
                continue;
            }
            const JsonValue node_v = target.get("node");
            if (!nonneg(node_v) || node_v.as_number() >= static_cast<f64>(remap.size()) || remap[static_cast<usize>(node_v.as_number())] < 0) {
                cx.warning(ch, "animation channel targets a node outside the imported scene; ignored");
                continue;
            }
            mc.node = static_cast<u32>(remap[static_cast<usize>(node_v.as_number())]);
            const JsonValue si = ch.get("sampler");
            if (!nonneg(si) || !samplers.is_array() || si.as_number() >= samplers.size()) {
                cx.error(ch, cptr, "sampler index out of range");
                return false;
            }
            const JsonValue smp = samplers.at(static_cast<u32>(si.as_number()));
            const StringView interp = smp.get("interpolation").is_string() ? smp.get("interpolation").as_string() : "LINEAR";
            bool cubic = false;
            if (interp == "STEP") mc.interp = asset::AnimInterp::Step;
            else if (interp == "CUBICSPLINE") {
                cubic = true;
                mc.interp = asset::AnimInterp::CubicSpline;
            }
            Array<f32> times, values;
            if (!read_accessor(cx, g, smp.get("input"), 1, 0, times, cptr)) return false;
            if (!read_accessor(cx, g, smp.get("output"), width, kNormInt, values, cptr)) return false;
            const usize keys = times.size();
            if (values.size() != keys * width * (cubic ? 3 : 1)) {
                cx.error(smp, cptr, "sampler output count does not match its input");
                return false;
            }
            for (usize k = 1; k < keys; ++k)
                if (times[k] < times[k - 1]) {
                    cx.error(smp, cptr, "keyframe times must be ascending");
                    return false;
                }
            mc.key_count = static_cast<u32>(keys);
            mc.times_offset = static_cast<u32>(out.keys.size());
            for (f32 t : times) out.keys.push_back(t);
            mc.values_offset = static_cast<u32>(out.keys.size());
            // Root-node translation/scale tracks carry the import scale like the rest pose does.
            // CUBICSPLINE keeps glTF's {in-tangent, value, out-tangent} triplet per key; tangents
            // are in value units per second and scale with the value.
            const bool root = out.nodes[mc.node].parent < 0 && mc.path != asset::AnimPath::Rotation;
            for (usize k = 0; k < values.size() / width; ++k)
                for (u32 c = 0; c < width; ++c) {
                    const f32 v = values[k * width + c];
                    out.keys.push_back(root ? v * import_scale : v);
                }
            clip.duration = math::max(clip.duration, times[keys - 1]);
            out.channels.push_back(mc);
        }
        clip.channel_count = static_cast<u32>(out.channels.size()) - clip.first_channel;
        out.clips.push_back(clip);
    }
    return true;
}

// The glTF skins the submeshes use, in order of first use: joints remapped to imported nodes,
// inverse bind matrices (identity when absent) and the per-joint bind bounds.
bool import_skins(Ctx& cx, const Gltf& g, const Array<i32>& remap, const Geometry& geo, SkinTables& out) {
    const JsonValue skins = g.root.get("skins");
    Array<i32> mapped;  // glTF skin -> our skin
    mapped.resize(skins.is_array() ? skins.size() : 0, -1);
    for (usize s = 0; s < geo.submeshes.size(); ++s) {
        const i32 gs = geo.submesh_skin[s];
        if (gs < 0) {
            out.submesh_skins.push_back(-1);
            continue;
        }
        if (mapped[static_cast<usize>(gs)] < 0) {
            const JsonValue sk = skins.at(static_cast<u32>(gs));
            String ptr;
            append_pointer(ptr, "skins", static_cast<u32>(gs));
            const JsonValue js = sk.get("joints");
            Array<f32> ibm;
            if (sk.get("inverseBindMatrices").valid()) {
                if (!read_accessor(cx, g, sk.get("inverseBindMatrices"), 16, 0, ibm, ptr)) return false;
                if (ibm.size() != usize(js.size()) * 16) {
                    cx.error(sk, ptr, "inverseBindMatrices count differs from the joint count");
                    return false;
                }
            }
            Array<u32> joint_nodes;
            for (u32 j = 0; j < js.size(); ++j) {
                const JsonValue jv = js.at(j);
                if (!nonneg(jv) || jv.as_number() >= static_cast<f64>(remap.size()) || remap[static_cast<usize>(jv.as_number())] < 0) {
                    cx.error(jv, ptr, "skin joint is not a node of the imported scene");
                    return false;
                }
                joint_nodes.push_back(static_cast<u32>(remap[static_cast<usize>(jv.as_number())]));
            }
            mapped[static_cast<usize>(gs)] = detail::add_skin(out, geo, gs, joint_nodes.span(), ibm.span());
        }
        out.submesh_skins.push_back(mapped[static_cast<usize>(gs)]);
    }
    return true;
}

// The sidecar's "ragdoll" object: per-bone overrides written into the skin joints of the bones it
// names (asset::MeshRagdoll). `any` is set when one was written.
bool apply_ragdoll_settings(const ImportInput& in, const Output& out, SkinTables& skins, bool& any, DiagList& diags) {
    const JsonValue rag = in.settings.valid() ? in.settings.get("ragdoll") : JsonValue();
    if (!rag.valid() || rag.is_null()) return true;
    if (!rag.is_object()) {
        diags.error(Errc::ParseError, in.display_path, 0, 0, kStage, "'ragdoll' must be an object of bone name -> overrides");
        return false;
    }
    if (rag.size() > 0 && skins.joints.empty()) {
        diags.error(Errc::InvalidArgument, in.display_path, 0, 0, kStage, "'ragdoll' names bones but the mesh has no skin");
        return false;
    }
    for (u32 i = 0; i < rag.size(); ++i) {
        const StringView bone = rag.key_at(i);
        const JsonValue o = rag.at(i);
        String what;
        appendf(what, "'ragdoll' bone '%.*s'", static_cast<int>(bone.size()), bone.data());
        const auto msg = [&](const char* rest) {
            String m;
            appendf(m, "%.*s%s", static_cast<int>(what.view().size()), what.view().data(), rest);
            return m;
        };
        if (!o.is_object()) {
            diags.error(Errc::ParseError, in.display_path, 0, 0, kStage, msg(" must be an object").view());
            return false;
        }
        if (!detail::check_keys(o, {"exclude", "include", "radius", "swing", "twist", "mass"}, in.display_path, kStage, diags))
            return false;
        bool exclude = false, include = false;
        f64 radius = 0.0, swing = 0.0, twist = 0.0, mass = 0.0;
        if (!detail::read_bool(o, "exclude", false, exclude, in.display_path, kStage, diags) ||
            !detail::read_bool(o, "include", false, include, in.display_path, kStage, diags) ||
            !detail::read_number(o, "radius", 0.0, radius, in.display_path, kStage, diags) ||
            !detail::read_number(o, "swing", 0.0, swing, in.display_path, kStage, diags) ||
            !detail::read_number(o, "twist", 0.0, twist, in.display_path, kStage, diags) ||
            !detail::read_number(o, "mass", 0.0, mass, in.display_path, kStage, diags))
            return false;
        if (exclude && include) {
            diags.error(Errc::InvalidArgument, in.display_path, 0, 0, kStage, msg(": 'exclude' and 'include' both set").view());
            return false;
        }
        if (radius < 0.0 || swing < 0.0 || swing > 180.0 || twist < 0.0 || twist > 180.0 || mass < 0.0) {
            diags.error(Errc::InvalidArgument, in.display_path, 0, 0, kStage,
                        msg(": radius and mass must be >= 0, swing and twist in 0..180 degrees").view());
            return false;
        }
        u32 flags = (exclude ? asset::MeshRagdoll::Exclude : 0u) | (include ? asset::MeshRagdoll::Include : 0u) |
                    (o.get("radius").valid() ? asset::MeshRagdoll::Radius : 0u) | (o.get("swing").valid() ? asset::MeshRagdoll::Swing : 0u) |
                    (o.get("twist").valid() ? asset::MeshRagdoll::Twist : 0u) | (o.get("mass").valid() ? asset::MeshRagdoll::Mass : 0u);
        bool found = false;
        for (asset::MeshJoint& j : skins.joints) {
            const asset::MeshNode& n = out.nodes[j.node];
            if (StringView(out.strings.data() + n.name_offset, n.name_size) != bone) continue;
            found = true;
            j.ragdoll_flags = flags;
            j.ragdoll_radius = static_cast<f32>(radius);
            j.ragdoll_swing = static_cast<f32>(swing);
            j.ragdoll_twist = static_cast<f32>(twist);
            j.ragdoll_mass = static_cast<f32>(mass);
        }
        if (!found) {
            diags.error(Errc::InvalidArgument, in.display_path, 0, 0, kStage, msg(" is not a joint of the mesh's skins").view());
            return false;
        }
        any = any || flags != 0;
    }
    return true;
}

// cameras and KHR_lights_punctual: dropped, one warning each per file, naming the first pointer.
void warn_dropped_features(Ctx& cx, const Gltf& g) {
    const JsonValue nodes = g.root.get("nodes");
    const JsonValue cams = g.root.get("cameras");
    bool camera_node = false, light_node = false;
    for (u32 i = 0; nodes.is_array() && i < nodes.size(); ++i) {
        const JsonValue n = nodes.at(i);
        String ptr;
        if (!camera_node && n.get("camera").valid()) {
            camera_node = true;
            appendf(ptr, "/nodes/%u/camera", i);
            String m("cameras are not imported (first: ");
            m += ptr;
            m += ")";
            cx.warn_once(kWarnCamera, n.get("camera"), m);
        }
        if (!light_node && n.get("extensions").get("KHR_lights_punctual").valid()) {
            light_node = true;
            ptr.clear();
            appendf(ptr, "/nodes/%u/extensions/KHR_lights_punctual", i);
            String m("KHR_lights_punctual lights are not imported (first: ");
            m += ptr;
            m += ")";
            cx.warn_once(kWarnLight, n.get("extensions").get("KHR_lights_punctual"), m);
        }
    }
    if (!camera_node && cams.is_array() && cams.size() > 0)
        cx.warn_once(kWarnCamera, cams, "cameras are not imported (first: /cameras/0)");
    if (!light_node && g.root.get("extensions").get("KHR_lights_punctual").valid())
        cx.warn_once(kWarnLight, g.root.get("extensions").get("KHR_lights_punctual"),
                     "KHR_lights_punctual lights are not imported (first: /extensions/KHR_lights_punctual)");
}

class GltfImporter final : public IImporter {
public:
    StringView id() const override { return "gltf"; }
    u32 version() const override { return 4; }  // 2: normal/ORM/emissive maps, CUBICSPLINE tracks (M5a); 3: skins; 4: sparse, quantization, meshopt, 8 influence sets, baked morph weights
    ChunkType type() const override { return ChunkType::Mesh; }
    bool native() const override { return false; }
    bool handles_extension(StringView ext) const override { return ext == ".glb" || ext == ".gltf"; }

    // Image URIs -> texture GUIDs. Problems are left to cook() to report (once).
    void scan_dependencies(const ImportInput& in, Array<DepRef>& out, DiagList&) const override {
        DiagList scratch;
        Ctx cx(in, scratch);
        Gltf g;
        if (!parse_container(cx, g)) return;
        const JsonValue mats = g.root.get("materials");
        for (u32 i = 0; mats.is_array() && i < mats.size(); ++i) {
            for (u32 slot = 0; slot < static_cast<u32>(TexSlot::Count); ++slot) {
                TextureRef tex;
                if (!material_texture(cx, g, i, static_cast<TexSlot>(slot), tex) || !tex.present) continue;
                bool dup = false;
                for (const DepRef& e : out) dup = dup || e.guid == tex.guid;
                if (dup) continue;
                DepRef d;
                d.guid = tex.guid;
                d.expected = ChunkType::Texture;
                d.line = tex.at.line();
                d.col = tex.at.col();
                d.where.assign(tex.pointer.view());
                out.push_back(d);
            }
        }
    }

    bool cook(const ImportInput& in, asset::ContainerWriter& writer, DiagList& diags) const override {
        if (!detail::check_keys(in.settings, {"scale", "max_triangles", "skinning", "lod_levels", "lod_ratio", "lod_error", "ragdoll"},
                                in.display_path, kStage, diags))
            return false;
        f64 scale = 1.0, max_triangles = 0.0, lod_levels = 0.0, lod_ratio = 0.35, lod_error = 0.05;
        if (!detail::read_number(in.settings, "lod_levels", 0.0, lod_levels, in.display_path, kStage, diags)) return false;
        if (!detail::read_number(in.settings, "lod_ratio", 0.35, lod_ratio, in.display_path, kStage, diags)) return false;
        if (!detail::read_number(in.settings, "lod_error", 0.05, lod_error, in.display_path, kStage, diags)) return false;
        if (lod_levels < 0.0 || lod_levels >= asset::kMaxMeshLods || lod_levels != std::floor(lod_levels)) {
            diags.error(Errc::ParseError, in.display_path, 0, 0, kStage, "'lod_levels' must be a whole number 0..3");
            return false;
        }
        if (!(lod_ratio > 0.0 && lod_ratio < 1.0) || !(lod_error > 0.0 && lod_error <= 1.0)) {
            diags.error(Errc::ParseError, in.display_path, 0, 0, kStage, "'lod_ratio' must be in (0, 1) and 'lod_error' in (0, 1]");
            return false;
        }
        bool skinning = true;
        if (!detail::read_bool(in.settings, "skinning", true, skinning, in.display_path, kStage, diags)) return false;
        if (!detail::read_number(in.settings, "scale", 1.0, scale, in.display_path, kStage, diags)) return false;
        if (!detail::read_number(in.settings, "max_triangles", 0.0, max_triangles, in.display_path, kStage, diags))
            return false;
        if (!(scale > 0.0)) {
            diags.error(Errc::ParseError, in.display_path, 0, 0, kStage, "'scale' must be positive");
            return false;
        }
        if (max_triangles < 0.0 || max_triangles != std::floor(max_triangles)) {
            diags.error(Errc::ParseError, in.display_path, 0, 0, kStage, "'max_triangles' must be a whole number >= 0 (0 = no budget)");
            return false;
        }
        Ctx cx(in, diags);
        const u32 errors_before = diags.error_count();
        Gltf g;
        if (!parse_container(cx, g) || !load_buffers(cx, g) || !decode_meshopt_views(cx, g)) return false;
        warn_dropped_features(cx, g);
        Output out;
        Array<i32> remap;
        Array<AuthoredLod> authored;
        if (!import_nodes(cx, g, static_cast<f32>(scale), skinning, out, remap, authored)) return false;
        if (!attach_authored_lods(cx, g, authored, out)) return false;
        if (lod_levels > 0.0) detail::generate_lods(out.geo, static_cast<u32>(lod_levels), static_cast<f32>(lod_ratio), static_cast<f32>(lod_error));
        if (!import_animations(cx, g, remap, static_cast<f32>(scale), out)) return false;
        SkinTables skin_out;
        if (!import_skins(cx, g, remap, out.geo, skin_out)) return false;
        bool ragdoll = false;
        if (!apply_ragdoll_settings(in, out, skin_out, ragdoll, diags)) return false;
        if (diags.error_count() != errors_before) return false;
        if (out.geo.submeshes.empty()) cx.warning(g.root, "the scene has no mesh primitives");
        usize triangles = 0;  // the base levels: LODs are cheaper by construction
        for (const asset::MeshSubmesh& sm : out.geo.submeshes) triangles += sm.index_count / 3;
        if (max_triangles > 0.0 && static_cast<f64>(triangles) > max_triangles) {
            String msg;
            appendf(msg, "%zu triangles, over the mesh's budget of %.0f (sidecar 'max_triangles')", triangles, max_triangles);
            diags.error(Errc::InvalidArgument, in.display_path, 0, 0, kStage, msg.view());
            return false;
        }

        detail::MeshTables tables;
        tables.nodes = out.nodes.span();
        tables.clips = out.clips.span();
        tables.channels = out.channels.span();
        tables.keys = out.keys.span();
        tables.strings = out.strings.span();
        detail::write_mesh_chunk(out.geo, skin_out, tables, ragdoll, in.guid, writer);
        return true;
    }
};

}  // namespace

mem::UniquePtr<IImporter> make_gltf_importer() { return mem::make_unique<GltfImporter>(mem::heap()); }

}  // namespace mce::cook
