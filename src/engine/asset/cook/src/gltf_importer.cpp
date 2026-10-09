// glTF 2.0 (.glb / .gltf) -> CHNK_MESH: static meshes, the node tree, inline metallic-roughness
// materials and node-transform animation clips (assets.md §5.2, FR-R3D-010).
//
// Scope is locked (assets.md §10 risk 2): triangle-list primitives with POSITION, optional NORMAL
// (generated when absent) and TEXCOORD_0; materials' factors + base color texture; node TRS (or
// a decomposable matrix); translation/rotation/scale animation channels. Skins, morph targets,
// cameras and lights are ignored with a warning. Required extensions other than the supported
// set are a cook error naming the extension; merely-used ones are a warning.
//
// Buffers must be inside the file: the GLB binary chunk or base64 `data:` URIs (the cook is pure
// — an importer sees its own source bytes only). Images must be external files registered as
// texture assets; their URI (relative to the glTF) resolves to the texture's GUID, which becomes a
// dependency. Errors name the file and the JSON pointer of the offending element.
//
// Per primitive: de-duplicate vertices (meshopt_generateVertexRemap), meshopt_optimizeVertexCache,
// meshopt_optimizeVertexFetch, then quantize (formats.h MeshVertex). Deterministic: no hashing of
// pointers, no clocks, stable iteration order everywhere.
//
// sidecar settings: { "scale": 1.0 }          uniform scale applied to every root node
//                   { "max_triangles": N }   the mesh's triangle budget (M7 per-mesh budget table):
//                                            more triangles is a cook error naming both counts

#include <cmath>
#include <cstring>

#include "importer_common.h"
#include "mce/asset/formats.h"
#include "mce/math/math.h"
#include "meshoptimizer.h"

namespace mce::cook {

namespace {

using namespace mce::math;

constexpr const char* kStage = "cook.gltf";
constexpr u32 kGlbMagic = 0x46546C67;  // "glTF"
constexpr u32 kGlbJson = 0x4E4F534A;   // "JSON"
constexpr u32 kGlbBin = 0x004E4942;    // "BIN\0"

constexpr StringView kSupportedExtensions[] = {"KHR_texture_transform", "KHR_materials_unlit", "KHR_materials_emissive_strength"};

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
    u32 errors() const { return diags_.error_count(); }
    const ImportInput& in() const { return in_; }

private:
    const ImportInput& in_;
    DiagList& diags_;
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
                String ptr;
                append_pointer(ptr, "extensionsRequired", i);
                cx.error(req.at(i), ptr, m);
            }
        }
    }
    if (const JsonValue used = g.root.get("extensionsUsed"); used.is_array()) {
        for (u32 i = 0; i < used.size(); ++i) {
            const StringView e = used.at(i).is_string() ? used.at(i).as_string() : StringView();
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

// Reads accessor `index` as `comps` floats per element (normalized integer types are converted).
bool read_accessor(Ctx& cx, const Gltf& g, JsonValue index_v, u32 comps, bool allow_int_normalized, Array<f32>& out,
                   StringView ptr_for_errors) {
    out.clear();
    if (!nonneg(index_v)) {
        cx.error(index_v, ptr_for_errors, "accessor index must be an integer");
        return false;
    }
    const auto ai = static_cast<u32>(index_v.as_number());
    const JsonValue accs = g.root.get("accessors");
    if (!accs.is_array() || ai >= accs.size()) {
        cx.error(index_v, ptr_for_errors, "accessor index out of range");
        return false;
    }
    const JsonValue a = accs.at(ai);
    String ptr;
    append_pointer(ptr, "accessors", ai);
    if (a.get("sparse").valid()) {
        cx.error(a, ptr, "sparse accessors are not supported");
        return false;
    }
    static constexpr struct {
        StringView name;
        u32 n;
    } kTypes[] = {{"SCALAR", 1}, {"VEC2", 2}, {"VEC3", 3}, {"VEC4", 4}};
    const StringView type = a.get("type").is_string() ? a.get("type").as_string() : StringView();
    u32 n = 0;
    for (const auto& t : kTypes)
        if (t.name == type) n = t.n;
    if (n != comps) {
        String m;
        appendf(m, "expected a %u-component accessor, got '%.*s'", comps, static_cast<int>(type.size()), type.data());
        cx.error(a, ptr, m);
        return false;
    }
    const JsonValue ctype_v = a.get("componentType");
    const u32 ctype = nonneg(ctype_v) ? static_cast<u32>(ctype_v.as_number()) : 0;
    const bool normalized = a.get("normalized").is_bool() && a.get("normalized").as_bool();
    u32 csize = 0;
    switch (ctype) {
        case 5126: csize = 4; break;           // FLOAT
        case 5121: case 5120: csize = 1; break;  // UNSIGNED_BYTE / BYTE
        case 5123: case 5122: csize = 2; break;  // UNSIGNED_SHORT / SHORT
        case 5125: csize = 4; break;           // UNSIGNED_INT (indices only)
        default: cx.error(a, ptr, "unsupported componentType"); return false;
    }
    const bool is_float = ctype == 5126;
    const bool is_index = comps == 1 && !normalized && (ctype == 5121 || ctype == 5123 || ctype == 5125);
    if (!is_float && !is_index && !(allow_int_normalized && normalized && ctype != 5125)) {
        cx.error(a, ptr, "component type not supported for this attribute");
        return false;
    }
    const JsonValue count_v = a.get("count");
    if (!nonneg(count_v) || count_v.as_number() < 1) {
        cx.error(a, ptr, "count must be a positive integer");
        return false;
    }
    const auto count = static_cast<usize>(count_v.as_number());
    out.resize(count * comps, 0.0f);
    const JsonValue bv_v = a.get("bufferView");
    if (!bv_v.valid()) return true;  // all zeros (glTF 3.6.2.2)
    const JsonValue views = g.root.get("bufferViews");
    if (!nonneg(bv_v) || !views.is_array() || bv_v.as_number() >= views.size()) {
        cx.error(a, ptr, "bufferView index out of range");
        return false;
    }
    const JsonValue bv = views.at(static_cast<u32>(bv_v.as_number()));
    const JsonValue buf_v = bv.get("buffer");
    if (!nonneg(buf_v) || buf_v.as_number() >= static_cast<f64>(g.buffers.size())) {
        cx.error(bv, ptr, "bufferView.buffer out of range");
        return false;
    }
    const Array<byte>& buf = g.buffers[static_cast<usize>(buf_v.as_number())];
    const u64 view_off = nonneg(bv.get("byteOffset")) ? static_cast<u64>(bv.get("byteOffset").as_number()) : 0;
    const u64 view_len = nonneg(bv.get("byteLength")) ? static_cast<u64>(bv.get("byteLength").as_number()) : 0;
    const u64 acc_off = nonneg(a.get("byteOffset")) ? static_cast<u64>(a.get("byteOffset").as_number()) : 0;
    const u64 elem = u64(csize) * comps;
    const u64 stride = nonneg(bv.get("byteStride")) ? static_cast<u64>(bv.get("byteStride").as_number()) : elem;
    if (stride < elem || view_off + view_len > buf.size() || acc_off + (count - 1) * stride + elem > view_len) {
        cx.error(a, ptr, "accessor range exceeds its buffer view");
        return false;
    }
    const byte* base = buf.data() + view_off + acc_off;
    for (usize e = 0; e < count; ++e) {
        const byte* p = base + e * stride;
        for (u32 c = 0; c < comps; ++c) {
            const byte* q = p + c * csize;
            f32 v = 0.0f;
            switch (ctype) {
                case 5126: std::memcpy(&v, q, 4); break;
                case 5121: v = normalized ? f32(u8(q[0])) / 255.0f : f32(u8(q[0])); break;
                case 5120: v = math::max(f32(i8(q[0])) / 127.0f, -1.0f); break;
                case 5123: {
                    u16 x;
                    std::memcpy(&x, q, 2);
                    v = normalized ? f32(x) / 65535.0f : f32(x);
                    break;
                }
                case 5122: {
                    i16 x;
                    std::memcpy(&x, q, 2);
                    v = math::max(f32(x) / 32767.0f, -1.0f);
                    break;
                }
                case 5125: {
                    u32 x;
                    std::memcpy(&x, q, 4);
                    v = f32(x);  // indices: exact below 2^24, checked by the caller
                    break;
                }
                default: break;
            }
            if (!std::isfinite(v)) {
                cx.error(a, ptr, "non-finite value");
                return false;
            }
            out[e * comps + c] = v;
        }
    }
    return true;
}

// Index accessors are read separately to keep u32 precision.
bool read_indices(Ctx& cx, const Gltf& g, JsonValue index_v, Array<u32>& out, StringView ptr_for_errors) {
    out.clear();
    const JsonValue accs = g.root.get("accessors");
    if (!nonneg(index_v) || !accs.is_array() || index_v.as_number() >= accs.size()) {
        cx.error(index_v, ptr_for_errors, "indices accessor out of range");
        return false;
    }
    const JsonValue a = accs.at(static_cast<u32>(index_v.as_number()));
    const u32 ctype = nonneg(a.get("componentType")) ? static_cast<u32>(a.get("componentType").as_number()) : 0;
    if (ctype == 5125) {
        // Re-read through the float path would lose precision past 2^24: read raw.
        Array<f32> tmp;
        if (!read_accessor(cx, g, index_v, 1, false, tmp, ptr_for_errors)) return false;
        const JsonValue bv = g.root.get("bufferViews").at(static_cast<u32>(a.get("bufferView").as_number()));
        const Array<byte>& buf = g.buffers[static_cast<usize>(bv.get("buffer").as_number())];
        const u64 view_off = nonneg(bv.get("byteOffset")) ? static_cast<u64>(bv.get("byteOffset").as_number()) : 0;
        const u64 acc_off = nonneg(a.get("byteOffset")) ? static_cast<u64>(a.get("byteOffset").as_number()) : 0;
        const u64 stride = nonneg(bv.get("byteStride")) ? static_cast<u64>(bv.get("byteStride").as_number()) : 4;
        out.resize(tmp.size());
        for (usize i = 0; i < tmp.size(); ++i) std::memcpy(&out[i], buf.data() + view_off + acc_off + i * stride, 4);
        return true;
    }
    if (ctype != 5121 && ctype != 5123) {
        cx.error(a, ptr_for_errors, "indices must be UNSIGNED_BYTE, UNSIGNED_SHORT or UNSIGNED_INT");
        return false;
    }
    Array<f32> tmp;
    if (!read_accessor(cx, g, index_v, 1, false, tmp, ptr_for_errors)) return false;
    out.resize(tmp.size());
    for (usize i = 0; i < tmp.size(); ++i) out[i] = static_cast<u32>(tmp[i]);
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

struct FatVertex {
    f32 p[3];
    f32 n[3];
    f32 uv[2];
};

i16 snorm16(f32 v) {
    const f32 c = math::clamp(v, -1.0f, 1.0f);
    return static_cast<i16>(std::lround(c * 32767.0f));
}

u16 unorm16(f32 v) { return static_cast<u16>(std::lround(math::clamp(v, 0.0f, 1.0f) * 65535.0f)); }

// Octahedral encoding (Cigolle et al. 2014) of a unit vector into [-1, 1]^2.
void oct_encode(const f32 n[3], f32 out[2]) {
    const f32 l1 = std::fabs(n[0]) + std::fabs(n[1]) + std::fabs(n[2]);
    f32 x = n[0] / l1, y = n[1] / l1;
    if (n[2] < 0.0f) {
        const f32 ox = x, oy = y;
        x = (1.0f - std::fabs(oy)) * (ox >= 0.0f ? 1.0f : -1.0f);
        y = (1.0f - std::fabs(ox)) * (oy >= 0.0f ? 1.0f : -1.0f);
    }
    out[0] = x;
    out[1] = y;
}

struct Geometry {
    Array<asset::MeshVertex> vertices;
    Array<u32> indices;  // relative to the submesh's base_vertex
    Array<asset::MeshSubmesh> submeshes;
};

bool import_primitive(Ctx& cx, const Gltf& g, JsonValue prim, StringView ptr, u32 node, Geometry& geo) {
    if (const JsonValue mode = prim.get("mode"); mode.valid() && !(nonneg(mode) && mode.as_number() == 4)) {
        cx.error(mode, ptr, "only triangle lists (mode 4) are supported");
        return false;
    }
    const JsonValue attrs = prim.get("attributes");
    if (!attrs.get("POSITION").valid()) {
        cx.error(prim, ptr, "primitive has no POSITION attribute");
        return false;
    }
    if (prim.get("targets").valid()) cx.warning(prim, "morph targets are ignored (static import)");
    if (attrs.get("JOINTS_0").valid()) cx.warning(prim, "skinning attributes are ignored (static import, skinning is v0.5)");

    Array<f32> pos, nrm, uv;
    if (!read_accessor(cx, g, attrs.get("POSITION"), 3, false, pos, ptr)) return false;
    const usize vcount = pos.size() / 3;
    if (attrs.get("NORMAL").valid() && !read_accessor(cx, g, attrs.get("NORMAL"), 3, false, nrm, ptr)) return false;
    if (attrs.get("TEXCOORD_0").valid() && !read_accessor(cx, g, attrs.get("TEXCOORD_0"), 2, true, uv, ptr)) return false;
    if ((!nrm.empty() && nrm.size() != vcount * 3) || (!uv.empty() && uv.size() != vcount * 2)) {
        cx.error(attrs, ptr, "attribute counts differ");
        return false;
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

    TextureRef tex;
    const asset::MaterialChunk mat = convert_material(cx, g, prim.get("material"), tex);
    if (cx.errors()) return false;

    Array<FatVertex> fat;
    fat.resize(vcount);
    for (usize v = 0; v < vcount; ++v) {
        FatVertex& f = fat[v];
        for (int c = 0; c < 3; ++c) f.p[c] = pos[v * 3 + c];
        for (int c = 0; c < 3; ++c) f.n[c] = nrm.empty() ? 0.0f : nrm[v * 3 + c];
        f32 u = uv.empty() ? 0.0f : uv[v * 2], w = uv.empty() ? 0.0f : uv[v * 2 + 1];
        if (tex.present) {  // KHR_texture_transform: offset * rotation * scale, baked into the UVs
            const f32 cs = std::cos(tex.uv_rotation), sn = std::sin(tex.uv_rotation);
            const f32 su = u * tex.uv_scale[0], sv = w * tex.uv_scale[1];
            u = tex.uv_offset[0] + cs * su + sn * sv;
            w = tex.uv_offset[1] - sn * su + cs * sv;
        }
        f.uv[0] = u;
        f.uv[1] = w;
    }
    if (nrm.empty()) {  // area-weighted smooth normals over the indexed triangles
        for (usize t = 0; t + 2 < idx.size(); t += 3) {
            const vec3 a{fat[idx[t]].p[0], fat[idx[t]].p[1], fat[idx[t]].p[2]};
            const vec3 b{fat[idx[t + 1]].p[0], fat[idx[t + 1]].p[1], fat[idx[t + 1]].p[2]};
            const vec3 c{fat[idx[t + 2]].p[0], fat[idx[t + 2]].p[1], fat[idx[t + 2]].p[2]};
            const vec3 fn = cross(b - a, c - a);
            for (int k = 0; k < 3; ++k) {
                FatVertex& f = fat[idx[t + k]];
                f.n[0] += fn.x;
                f.n[1] += fn.y;
                f.n[2] += fn.z;
            }
        }
    }
    for (FatVertex& f : fat) {
        const f32 l = std::sqrt(f.n[0] * f.n[0] + f.n[1] * f.n[1] + f.n[2] * f.n[2]);
        if (l > 1e-12f) {
            for (f32& c : f.n) c /= l;
        } else {
            f.n[0] = 0.0f;
            f.n[1] = 1.0f;
            f.n[2] = 0.0f;
        }
    }

    // meshopt: de-duplicate, then vertex cache, then vertex fetch order.
    Array<u32> remap;
    remap.resize(vcount);
    const usize unique = meshopt_generateVertexRemap(remap.data(), idx.data(), idx.size(), fat.data(), vcount, sizeof(FatVertex));
    Array<FatVertex> uniq;
    uniq.resize(unique);
    meshopt_remapVertexBuffer(uniq.data(), fat.data(), vcount, sizeof(FatVertex), remap.data());
    meshopt_remapIndexBuffer(idx.data(), idx.data(), idx.size(), remap.data());
    meshopt_optimizeVertexCache(idx.data(), idx.data(), idx.size(), unique);
    Array<FatVertex> opt;
    opt.resize(unique);
    const usize used = meshopt_optimizeVertexFetch(opt.data(), idx.data(), idx.size(), uniq.data(), unique, sizeof(FatVertex));

    asset::MeshSubmesh sm{};
    sm.first_index = static_cast<u32>(geo.indices.size());
    sm.index_count = static_cast<u32>(idx.size());
    sm.base_vertex = static_cast<u32>(geo.vertices.size());
    sm.vertex_count = static_cast<u32>(used);
    sm.node = node;
    sm.material = mat;
    f32 pmin[3] = {0, 0, 0}, pmax[3] = {0, 0, 0}, umin[2] = {0, 0}, umax[2] = {0, 0};
    for (usize v = 0; v < used; ++v) {
        for (int c = 0; c < 3; ++c) {
            pmin[c] = v ? math::min(pmin[c], opt[v].p[c]) : opt[v].p[c];
            pmax[c] = v ? math::max(pmax[c], opt[v].p[c]) : opt[v].p[c];
        }
        for (int c = 0; c < 2; ++c) {
            umin[c] = v ? math::min(umin[c], opt[v].uv[c]) : opt[v].uv[c];
            umax[c] = v ? math::max(umax[c], opt[v].uv[c]) : opt[v].uv[c];
        }
    }
    for (int c = 0; c < 3; ++c) {
        sm.center[c] = (pmin[c] + pmax[c]) * 0.5f;
        sm.extent[c] = math::max((pmax[c] - pmin[c]) * 0.5f, 1e-6f);
    }
    for (int c = 0; c < 2; ++c) {
        sm.uv_offset[c] = umin[c];
        sm.uv_scale[c] = umax[c] > umin[c] ? umax[c] - umin[c] : 1.0f;
    }
    for (usize v = 0; v < used; ++v) {
        asset::MeshVertex q{};
        for (int c = 0; c < 3; ++c) q.position[c] = snorm16((opt[v].p[c] - sm.center[c]) / sm.extent[c]);
        f32 o[2];
        oct_encode(opt[v].n, o);
        q.normal[0] = snorm16(o[0]);
        q.normal[1] = snorm16(o[1]);
        for (int c = 0; c < 2; ++c) q.uv[c] = unorm16((opt[v].uv[c] - sm.uv_offset[c]) / sm.uv_scale[c]);
        geo.vertices.push_back(q);
    }
    for (u32 i : idx) geo.indices.push_back(i);
    geo.submeshes.push_back(sm);
    return true;
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

// Node tree in depth-first order (parents first) from the default scene's roots.
bool import_nodes(Ctx& cx, const Gltf& g, f32 import_scale, Output& out, Array<i32>& remap) {
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
        if (n.get("skin").valid()) cx.warning(n, "skins are ignored (static import, skinning is v0.5)");
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
            for (u32 p = 0; prims.is_array() && p < prims.size(); ++p) {
                String pptr;
                appendf(pptr, "/meshes/%u/primitives/%u", mi, p);
                if (!import_primitive(cx, g, prims.at(p), pptr, static_cast<u32>(self), out.geo)) return false;
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
            if (!read_accessor(cx, g, smp.get("input"), 1, false, times, cptr)) return false;
            if (!read_accessor(cx, g, smp.get("output"), width, true, values, cptr)) return false;
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

mat4 node_local(const asset::MeshNode& n) {
    return trs({n.translation[0], n.translation[1], n.translation[2]},
               normalize(quat{n.rotation[0], n.rotation[1], n.rotation[2], n.rotation[3]}), {n.scale[0], n.scale[1], n.scale[2]});
}

class GltfImporter final : public IImporter {
public:
    StringView id() const override { return "gltf"; }
    u32 version() const override { return 2; }  // 2: normal/ORM/emissive maps, CUBICSPLINE tracks (M5a)
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
        if (!detail::check_keys(in.settings, {"scale", "max_triangles"}, in.display_path, kStage, diags)) return false;
        f64 scale = 1.0, max_triangles = 0.0;
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
        if (!parse_container(cx, g) || !load_buffers(cx, g)) return false;
        Output out;
        Array<i32> remap;
        if (!import_nodes(cx, g, static_cast<f32>(scale), out, remap)) return false;
        if (!import_animations(cx, g, remap, static_cast<f32>(scale), out)) return false;
        if (diags.error_count() != errors_before) return false;
        if (out.geo.submeshes.empty()) cx.warning(g.root, "the scene has no mesh primitives");
        const usize triangles = out.geo.indices.size() / 3;
        if (max_triangles > 0.0 && static_cast<f64>(triangles) > max_triangles) {
            String msg;
            appendf(msg, "%zu triangles, over the mesh's budget of %.0f (sidecar 'max_triangles')", triangles, max_triangles);
            diags.error(Errc::InvalidArgument, in.display_path, 0, 0, kStage, msg.view());
            return false;
        }

        // Rest-pose model bounds over every submesh's node-local box.
        Array<mat4> world;
        world.resize(out.nodes.size());
        for (usize i = 0; i < out.nodes.size(); ++i)
            world[i] = out.nodes[i].parent < 0 ? node_local(out.nodes[i]) : world[static_cast<usize>(out.nodes[i].parent)] * node_local(out.nodes[i]);
        asset::MeshData d;
        bool any = false;
        u32 max_vertices = 0;
        for (const asset::MeshSubmesh& sm : out.geo.submeshes) {
            max_vertices = math::max(max_vertices, sm.vertex_count);
            for (int corner = 0; corner < 8; ++corner) {
                const vec3 p{sm.center[0] + ((corner & 1) ? sm.extent[0] : -sm.extent[0]),
                             sm.center[1] + ((corner & 2) ? sm.extent[1] : -sm.extent[1]),
                             sm.center[2] + ((corner & 4) ? sm.extent[2] : -sm.extent[2])};
                const vec3 w = transform_point(world[sm.node], p);
                const f32 c[3] = {w.x, w.y, w.z};
                for (int k = 0; k < 3; ++k) {
                    d.bounds_min[k] = any ? math::min(d.bounds_min[k], c[k]) : c[k];
                    d.bounds_max[k] = any ? math::max(d.bounds_max[k], c[k]) : c[k];
                }
                any = true;
            }
        }

        Array<u16> idx16;
        if (max_vertices > 0xFFFF) {
            d.flags |= asset::MeshFlags::Index32;
            d.indices = std::as_bytes(out.geo.indices.span());
        } else {
            idx16.resize(out.geo.indices.size());
            for (usize i = 0; i < idx16.size(); ++i) idx16[i] = static_cast<u16>(out.geo.indices[i]);
            d.indices = std::as_bytes(idx16.span());
        }
        d.vertices = out.geo.vertices.span();
        d.submeshes = out.geo.submeshes.span();
        d.nodes = out.nodes.span();
        d.clips = out.clips.span();
        d.channels = out.channels.span();
        d.keys = out.keys.span();
        d.strings = out.strings.span();
        Array<byte> payload;
        asset::write_mesh(d, payload);
        asset::ContainerWriter::ChunkDesc desc;
        desc.type = ChunkType::Mesh;
        desc.version = asset::chunk_version(asset::MESH_VERSION, 0);
        desc.guid = in.guid;
        writer.add(desc, payload.span());
        return true;
    }
};

}  // namespace

mem::UniquePtr<IImporter> make_gltf_importer() { return mem::make_unique<GltfImporter>(mem::heap()); }

}  // namespace mce::cook
