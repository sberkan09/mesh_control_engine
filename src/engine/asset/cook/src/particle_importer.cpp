// .mcpart -> CHNK_PART: one particle emitter preset (FR-R2D-070, assets.md §2). The texture GUID
// is a dependency (a missing or non-texture reference is a cook error naming file:line:col).
// Every key but format/version/guid is optional; ranges are [min, max] pairs.
//
// {
//   "format": "mce-particles", "version": 1, "guid": "<uuid>", "name": "dust",
//   "texture": "guid:<texture uuid>" | null,
//   "rate": 0, "burst": 12, "max_particles": 64, "duration": 0.1,
//   "lifetime": [0.3, 0.6], "speed": [1, 3], "direction": 90, "spread": 60,   // degrees
//   "gravity": [0, -9.8], "drag": 2, "size": [0.25, 0.05], "spin": [-180, 180], // degrees/s
//   "area": [0.2, 0], "color_start": [1, 1, 1, 1], "color_end": [1, 1, 1, 0],
//   "blend": "alpha" | "additive"
// }

#include <cstring>

#include "importer_common.h"
#include "mce/asset/formats.h"

namespace mce::cook {

namespace {

constexpr const char* kStage = "cook.particles";
constexpr u32 kParticleFormatVersion = 1;
constexpr u32 kMaxParticles = 65536;
constexpr f64 kDegToRad = 3.14159265358979323846 / 180.0;

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

// A [min, max] pair with min <= max (and min >= 0 when `non_negative`).
void read_range(JsonValue doc, StringView key, f32* out, bool non_negative, StringView file, DiagList& diags) {
    if (!read_vec(doc, key, out, 2, file, diags)) return;
    if (out[0] > out[1] || (non_negative && out[0] < 0.0f)) {
        String msg;
        appendf(msg, "'%.*s' must be [min, max] with min <= max%s", static_cast<int>(key.size()), key.data(),
                non_negative ? " and min >= 0" : "");
        detail::error_at(diags, Errc::ParseError, file, doc.get(key), kStage, msg);
    }
}

f32 read_f32(JsonValue doc, StringView key, f64 def, StringView file, DiagList& diags) {
    f64 v = def;
    detail::read_number(doc, key, def, v, file, kStage, diags);
    return static_cast<f32>(v);
}

class ParticleImporter final : public IImporter {
public:
    StringView id() const override { return "particles"; }
    u32 version() const override { return 1; }
    ChunkType type() const override { return ChunkType::ParticlePreset; }
    bool native() const override { return true; }
    bool handles_extension(StringView ext) const override { return ext == ".mcpart"; }

    void scan_dependencies(const ImportInput& in, Array<DepRef>& out, DiagList& diags) const override {
        const JsonValue tex = in.settings.get("texture");
        if (!tex.valid() || tex.is_null()) return;
        Guid g;
        if (!tex.is_string() || !parse_guid_ref(tex.as_string(), g)) {
            detail::error_at(diags, Errc::ParseError, in.display_path, tex, kStage, "'texture' must be \"guid:<uuid>\" or null");
            return;
        }
        DepRef d;
        d.guid = g;
        d.expected = ChunkType::Texture;
        d.line = tex.line();
        d.col = tex.col();
        d.where.assign("texture");
        out.push_back(d);
    }

    bool cook(const ImportInput& in, asset::ContainerWriter& out, DiagList& diags) const override {
        const JsonValue doc = in.settings;
        const StringView file = in.display_path;
        const u32 errors_before = diags.error_count();
        if (!detail::check_keys(doc, {"format", "version", "guid", "name", "texture", "rate", "burst", "max_particles", "duration",
                                      "lifetime", "speed", "direction", "spread", "gravity", "drag", "size", "spin", "area",
                                      "color_start", "color_end", "blend"},
                                file, kStage, diags))
            return false;
        const JsonValue fmt = doc.get("format");
        if (!fmt.is_string() || fmt.as_string() != "mce-particles") {
            detail::error_at(diags, Errc::ParseError, file, fmt.valid() ? fmt : doc, kStage, "\"format\" must be \"mce-particles\"");
            return false;
        }
        if (!detail::check_format_version(doc, kParticleFormatVersion, "particle preset", file, kStage, diags)) return false;

        asset::ParticlePresetChunk p{};
        p.rate = read_f32(doc, "rate", 0, file, diags);
        p.duration = read_f32(doc, "duration", 0, file, diags);
        f64 burst = 0, max_particles = 256;
        detail::read_number(doc, "burst", 0, burst, file, kStage, diags);
        detail::read_number(doc, "max_particles", 256, max_particles, file, kStage, diags);
        if (burst < 0 || burst > kMaxParticles || burst != static_cast<f64>(static_cast<u32>(burst)))
            detail::error_at(diags, Errc::ParseError, file, doc.get("burst"), kStage, "'burst' must be a whole number from 0 to 65536");
        if (max_particles < 1 || max_particles > kMaxParticles || max_particles != static_cast<f64>(static_cast<u32>(max_particles)))
            detail::error_at(diags, Errc::ParseError, file, doc.get("max_particles"), kStage,
                             "'max_particles' must be a whole number from 1 to 65536");
        p.burst = static_cast<u32>(burst);
        p.max_particles = static_cast<u32>(max_particles);
        if (p.rate < 0.0f) detail::error_at(diags, Errc::ParseError, file, doc.get("rate"), kStage, "'rate' must be >= 0");
        if (p.duration < 0.0f) detail::error_at(diags, Errc::ParseError, file, doc.get("duration"), kStage, "'duration' must be >= 0");

        p.lifetime[0] = p.lifetime[1] = 1.0f;
        p.speed[0] = p.speed[1] = 1.0f;
        p.size_start = p.size_end = 0.25f;
        for (u32 i = 0; i < 4; ++i) p.color_start[i] = p.color_end[i] = 1.0f;
        read_range(doc, "lifetime", p.lifetime, true, file, diags);
        if (p.lifetime[1] <= 0.0f)
            detail::error_at(diags, Errc::ParseError, file, doc.get("lifetime"), kStage, "'lifetime' max must be > 0");
        read_range(doc, "speed", p.speed, true, file, diags);
        read_range(doc, "spin", p.spin, false, file, diags);
        p.spin[0] = static_cast<f32>(p.spin[0] * kDegToRad);
        p.spin[1] = static_cast<f32>(p.spin[1] * kDegToRad);
        p.direction = static_cast<f32>(read_f32(doc, "direction", 90, file, diags) * kDegToRad);
        p.spread = static_cast<f32>(read_f32(doc, "spread", 0, file, diags) * kDegToRad);
        p.drag = read_f32(doc, "drag", 0, file, diags);
        if (p.drag < 0.0f) detail::error_at(diags, Errc::ParseError, file, doc.get("drag"), kStage, "'drag' must be >= 0");
        read_vec(doc, "gravity", p.gravity, 2, file, diags);
        read_vec(doc, "area", p.area, 2, file, diags);
        f32 size[2] = {p.size_start, p.size_end};
        read_vec(doc, "size", size, 2, file, diags);
        if (size[0] < 0.0f || size[1] < 0.0f) detail::error_at(diags, Errc::ParseError, file, doc.get("size"), kStage, "'size' must be >= 0");
        p.size_start = size[0];
        p.size_end = size[1];
        read_vec(doc, "color_start", p.color_start, 4, file, diags);
        read_vec(doc, "color_end", p.color_end, 4, file, diags);
        if (const JsonValue blend = doc.get("blend"); blend.valid()) {
            const StringView s = blend.is_string() ? blend.as_string() : StringView();
            if (s == "additive") p.flags |= asset::ParticleFlags::Additive;
            else if (s != "alpha") detail::error_at(diags, Errc::ParseError, file, blend, kStage, "'blend' must be alpha or additive");
        }
        if (const JsonValue tex = doc.get("texture"); tex.is_string()) {
            Guid g;
            if (parse_guid_ref(tex.as_string(), g)) {
                p.texture[0] = g.hi;
                p.texture[1] = g.lo;
            }
        }
        if (diags.error_count() != errors_before) return false;

        asset::ContainerWriter::ChunkDesc d;
        d.type = ChunkType::ParticlePreset;
        d.version = asset::chunk_version(asset::PARTICLE_VERSION, 0);
        d.guid = in.guid;
        out.add(d, Span<const byte>(reinterpret_cast<const byte*>(&p), sizeof(p)));
        return true;
    }
};

}  // namespace

mem::UniquePtr<IImporter> make_particle_importer() { return mem::make_unique<ParticleImporter>(mem::heap()); }

}  // namespace mce::cook
