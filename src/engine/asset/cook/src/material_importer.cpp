// .mcmat -> CHNK_MAT: fixed-width metallic-roughness param block + texture refs by GUID
// (assets.md §5.2). Texture GUIDs are dependencies: a missing or non-texture reference is a cook
// error naming file:line:col.
//
// {
//   "format": "mce-material", "version": 1, "guid": "<uuid>",
//   "base_color": [1, 1, 1, 1], "metallic": 0, "roughness": 1, "emissive": [0, 0, 0],
//   "uv_scale": [1, 1],   (how often the textures repeat across the mesh's UVs: [8, 8] tiles a floor)
//   "alpha_mode": "opaque" | "mask" | "blend", "alpha_cutoff": 0.5,
//   "double_sided": false, "unlit": false,
//   (M-VISUAL2 4, all optional, 0 = off) "clearcoat": 0..1, "clearcoat_roughness": 0..1,
//   "sheen_color": [r, g, b], "sheen_roughness": 0..1, "anisotropy": -1..1, "anisotropy_rotation": radians,
//   "transmission": 0..1, "ior": 1..3 (default 1.5),
//   (M-VISUAL3 2, parallax occlusion mapping, 0 = off) "height_scale": 0..0.5 (depth in texture repeats),
//   "parallax_steps": [min, max] (4..64, march steps looking straight on and at a grazing angle),
//   "textures": { "base_color": "guid:<uuid>", "normal": ..., "metallic_roughness": ..., "emissive": ...,
//                 "height": ... (linear, white is high) }
// }

#include <cstring>

#include "importer_common.h"
#include "mce/asset/formats.h"

namespace mce::cook {

namespace {

constexpr const char* kStage = "cook.material";
constexpr u32 kMaterialFormatVersion = 1;
constexpr StringView kSlotNames[] = {"base_color", "normal", "metallic_roughness", "emissive"};
constexpr StringView kHeightSlot = "height";  // not a MaterialChunk slot: its GUID lives in MaterialExt

bool read_vec(JsonValue doc, StringView key, f32* out, u32 n, StringView file, DiagList& diags) {
    const JsonValue v = doc.get(key);
    if (!v.valid()) return true;
    if (!v.is_array() || v.size() != n) {
        String msg;
        appendf(msg, "'%.*s' must be an array of %u numbers", static_cast<int>(key.size()), key.data(), n);
        detail::error_at(diags, Errc::ParseError, file, v, kStage, msg);
        return false;
    }
    for (u32 i = 0; i < n; ++i) {
        if (!v.at(i).is_number()) {
            detail::error_at(diags, Errc::ParseError, file, v.at(i), kStage, "expected a number");
            return false;
        }
        out[i] = static_cast<f32>(v.at(i).as_number());
    }
    return true;
}

class MaterialImporter final : public IImporter {
public:
    StringView id() const override { return "material"; }
    u32 version() const override { return 2; }
    ChunkType type() const override { return ChunkType::Material; }
    bool native() const override { return true; }
    bool handles_extension(StringView ext) const override { return ext == ".mcmat"; }

    void scan_dependencies(const ImportInput& in, Array<DepRef>& out, DiagList& diags) const override {
        const JsonValue tex = in.settings.get("textures");
        if (!tex.valid()) return;
        if (!tex.is_object()) {
            detail::error_at(diags, Errc::ParseError, in.display_path, tex, kStage, "'textures' must be an object");
            return;
        }
        for (u32 i = 0; i < tex.size(); ++i) {
            const JsonValue v = tex.at(i);
            if (v.is_null()) continue;
            Guid g;
            if (!v.is_string() || !parse_guid_ref(v.as_string(), g)) {
                detail::error_at(diags, Errc::ParseError, in.display_path, v, kStage, "texture reference must be \"guid:<uuid>\" or null");
                continue;
            }
            DepRef d;
            d.guid = g;
            d.expected = ChunkType::Texture;
            d.line = v.line();
            d.col = v.col();
            d.where.assign("textures.");
            d.where.append(tex.key_at(i));
            out.push_back(d);
        }
    }

    bool cook(const ImportInput& in, asset::ContainerWriter& out, DiagList& diags) const override {
        const JsonValue doc = in.settings;
        const StringView file = in.display_path;
        const u32 errors_before = diags.error_count();
        if (!detail::check_keys(doc, {"format", "version", "guid", "name", "base_color", "metallic", "roughness", "emissive", "alpha_mode",
                                      "alpha_cutoff", "double_sided", "unlit", "uv_scale", "textures", "clearcoat",
                                      "clearcoat_roughness", "sheen_color", "sheen_roughness", "anisotropy", "anisotropy_rotation",
                                      "transmission", "ior", "height_scale", "parallax_steps"},
                                file, kStage, diags))
            return false;
        const JsonValue fmt = doc.get("format");
        if (!fmt.is_string() || fmt.as_string() != "mce-material") {
            detail::error_at(diags, Errc::ParseError, file, fmt.valid() ? fmt : doc, kStage, "\"format\" must be \"mce-material\"");
            return false;
        }
        const JsonValue ver = doc.get("version");
        if (!ver.is_integer() || ver.as_number() != kMaterialFormatVersion) {
            String msg;
            appendf(msg, "unsupported material version (this cook reads version %u)", kMaterialFormatVersion);
            detail::error_at(diags, Errc::VersionTooNew, file, ver.valid() ? ver : doc, kStage, msg);
            return false;
        }

        asset::MaterialChunk m{};
        m.base_color[0] = m.base_color[1] = m.base_color[2] = m.base_color[3] = 1.0f;
        m.roughness = 1.0f;
        m.alpha_cutoff = 0.5f;
        f64 metallic = 0, roughness = 1, cutoff = 0.5;
        bool double_sided = false, unlit = false;
        read_vec(doc, "base_color", m.base_color, 4, file, diags);
        read_vec(doc, "emissive", m.emissive, 3, file, diags);
        f32 uv_scale[2] = {1.0f, 1.0f};
        if (read_vec(doc, "uv_scale", uv_scale, 2, file, diags)) {
            if (!(uv_scale[0] > 0.0f && uv_scale[1] > 0.0f && uv_scale[0] <= 4096.0f && uv_scale[1] <= 4096.0f))
                detail::error_at(diags, Errc::ParseError, file, doc.get("uv_scale"), kStage,
                                 "'uv_scale' must be two numbers in (0, 4096]: how often the textures repeat across the mesh's UVs");
            else m.uv_scale_half = asset::MaterialChunk::pack_uv_scale(uv_scale[0], uv_scale[1]);
        }
        detail::read_number(doc, "metallic", 0, metallic, file, kStage, diags);
        detail::read_number(doc, "roughness", 1, roughness, file, kStage, diags);
        detail::read_number(doc, "alpha_cutoff", 0.5, cutoff, file, kStage, diags);
        detail::read_bool(doc, "double_sided", false, double_sided, file, kStage, diags);
        detail::read_bool(doc, "unlit", false, unlit, file, kStage, diags);
        m.metallic = static_cast<f32>(metallic);
        m.roughness = static_cast<f32>(roughness);
        m.alpha_cutoff = static_cast<f32>(cutoff);
        if (double_sided) m.flags |= asset::MaterialFlags::DoubleSided;
        if (unlit) m.flags |= asset::MaterialFlags::Unlit;
        if (const JsonValue mode = doc.get("alpha_mode"); mode.valid()) {
            const StringView s = mode.is_string() ? mode.as_string() : StringView();
            if (s == "mask") m.flags |= asset::MaterialFlags::AlphaMask;
            else if (s == "blend") m.flags |= asset::MaterialFlags::AlphaBlend;
            else if (s != "opaque") detail::error_at(diags, Errc::ParseError, file, mode, kStage, "'alpha_mode' must be opaque, mask or blend");
        }

        Guid height;
        if (const JsonValue tex = doc.get("textures"); tex.is_object()) {
            for (u32 i = 0; i < tex.size(); ++i) {
                Guid g;
                const bool have = tex.at(i).is_string() && parse_guid_ref(tex.at(i).as_string(), g);
                if (tex.key_at(i) == kHeightSlot) {
                    if (have) height = g;
                    continue;
                }
                u32 slot = 0;
                while (slot < 4 && kSlotNames[slot] != tex.key_at(i)) ++slot;
                if (slot == 4) {
                    detail::error_at(diags, Errc::ParseError, file, tex.at(i), kStage,
                                     "unknown texture slot (base_color, normal, metallic_roughness, emissive, height)");
                    continue;
                }
                if (have) {
                    m.textures[slot][0] = g.hi;
                    m.textures[slot][1] = g.lo;
                }
            }
        }
        // The extension lobes, each range-checked; written only when one is set.
        asset::MaterialExt e{};
        auto ranged = [&](StringView key, f64 lo, f64 hi, f32& dst) {
            f64 v = 0;
            if (!doc.get(key).valid()) return;
            detail::read_number(doc, key, 0, v, file, kStage, diags);
            if (v < lo || v > hi) {
                String msg;
                appendf(msg, "'%.*s' must be within [%g, %g]", static_cast<int>(key.size()), key.data(), lo, hi);
                detail::error_at(diags, Errc::ParseError, file, doc.get(key), kStage, msg);
                return;
            }
            dst = static_cast<f32>(v);
        };
        ranged("clearcoat", 0, 1, e.clearcoat);
        ranged("clearcoat_roughness", 0, 1, e.clearcoat_roughness);
        read_vec(doc, "sheen_color", e.sheen_color, 3, file, diags);
        ranged("sheen_roughness", 0, 1, e.sheen_roughness);
        ranged("anisotropy", -1, 1, e.anisotropy);
        ranged("anisotropy_rotation", -6.2832, 6.2832, e.anisotropy_rotation);
        ranged("transmission", 0, 1, e.transmission);
        ranged("ior", 1, 3, e.ior);
        ranged("height_scale", 0, 0.5, e.height_scale);
        e.set_height(height);
        // Height is data, not color: an sRGB import (the texture default) would be uploaded as sRGB and bend the depth.
        if (ChunkType ht = ChunkType::Manifest; !height.is_null() && in.lookup && in.lookup->type_of(height, ht) && ht == ChunkType::Texture) {
            JsonValue ts;
            StringView tpath;
            bool srgb = true;
            DiagList unused;
            if (in.lookup->native_document(height, ts, tpath)) detail::read_bool(ts, "srgb", true, srgb, tpath, kStage, unused);
            if (tpath.empty()) {  // no settings in the sidecar: the path still names the file
                Span<const byte> bytes;
                in.lookup->source_bytes(height, bytes, tpath);
            }
            if (srgb) {
                String msg("the height map ");
                if (!tpath.empty()) {
                    msg += "assets/";
                    msg += tpath;
                }
                msg += " is imported as sRGB (its \"srgb\" setting defaults to true): set \"srgb\": false in its .mceasset so the depth is not bent";
                const JsonValue at = doc.get("textures").get(kHeightSlot);
                diags.warning(file, at.valid() ? at.line() : 0, at.valid() ? at.col() : 0, kStage, msg);
            }
        }
        if (e.height_scale > 0.0f && height.is_null())
            detail::error_at(diags, Errc::ParseError, file, doc.get("height_scale"), kStage,
                             "'height_scale' needs a height map: set textures.height");
        if (const JsonValue st = doc.get("parallax_steps"); st.valid()) {
            const bool shaped = st.is_array() && st.size() == 2 && st.at(0).is_integer() && st.at(1).is_integer();
            const f64 lo = shaped ? st.at(0).as_number() : 0, hi = shaped ? st.at(1).as_number() : 0;
            if (!shaped || lo < 4 || hi > 64 || lo > hi)
                detail::error_at(diags, Errc::ParseError, file, st, kStage, "'parallax_steps' must be [min, max] integers with 4 <= min <= max <= 64");
            else e.parallax_steps = static_cast<u32>(lo) | static_cast<u32>(hi) << 16;
        }
        if (diags.error_count() != errors_before) return false;

        asset::ContainerWriter::ChunkDesc d;
        d.type = ChunkType::Material;
        d.guid = in.guid;
        if (e.any() || e.parallax() || !height.is_null()) {
            byte payload[sizeof(m) + sizeof(e)];
            std::memcpy(payload, &m, sizeof(m));
            std::memcpy(payload + sizeof(m), &e, sizeof(e));
            d.version = asset::chunk_version(asset::MATERIAL_VERSION, asset::MATERIAL_MINOR_EXT);
            out.add(d, Span<const byte>(payload, sizeof(payload)));
        } else {
            d.version = asset::chunk_version(asset::MATERIAL_VERSION, 0);
            out.add(d, asset::as_bytes_of(m));
        }
        return true;
    }
};

}  // namespace

mem::UniquePtr<IImporter> make_material_importer() { return mem::make_unique<MaterialImporter>(mem::heap()); }

}  // namespace mce::cook
