// .mcsound -> a sound set in an Sfx chunk (M-AUDIO2 2, asset::SoundSetHeader): clips picked per play
// with a pitch and gain range, so footsteps and impacts do not repeat. Every clip is a dependency (a
// missing or non-Sfx reference, or another sound set, is a cook error naming file:line:col).
//
// {
//   "format": "mce-sound", "version": 1, "guid": "<uuid>",
//   "clips": ["guid:<sfx uuid>", ...],          // up to 4096; an empty set plays nothing (a warning)
//   "pick": "random" | "sequence",               // default random
//   "no_repeat": true,                           // random: never the same clip twice in a row
//   "pitch": [1, 1],                             // playback rate range, 0.25 .. 4
//   "gain_db": [0, 0]                            // added to the play's gain, -60 .. 12
// }

#include "importer_common.h"
#include "mce/asset/formats.h"

namespace mce::cook {

namespace {

constexpr const char* kStage = "cook.sound";
constexpr u32 kSoundFormatVersion = 1;
constexpr u32 kMaxClips = 4096;

bool read_range(JsonValue doc, StringView key, f32 lo, f32 hi, f32* out, StringView file, DiagList& diags) {
    const JsonValue v = doc.get(key);
    if (!v.valid()) return true;
    if (!v.is_array() || v.size() != 2 || !v.at(0).is_number() || !v.at(1).is_number() || v.at(0).as_number() > v.at(1).as_number() ||
        v.at(0).as_number() < lo || v.at(1).as_number() > hi) {
        String msg;
        appendf(msg, "'%.*s' must be [min, max] with %g <= min <= max <= %g", static_cast<int>(key.size()), key.data(), static_cast<f64>(lo),
                static_cast<f64>(hi));
        detail::error_at(diags, Errc::ParseError, file, v, kStage, msg);
        return false;
    }
    out[0] = static_cast<f32>(v.at(0).as_number());
    out[1] = static_cast<f32>(v.at(1).as_number());
    return true;
}

class SoundSetImporter final : public IImporter {
public:
    StringView id() const override { return "sound"; }
    u32 version() const override { return 1; }
    ChunkType type() const override { return ChunkType::Sfx; }
    bool native() const override { return true; }
    bool handles_extension(StringView ext) const override { return ext == ".mcsound"; }

    void scan_dependencies(const ImportInput& in, Array<DepRef>& out, DiagList& diags) const override {
        const JsonValue clips = in.settings.get("clips");
        if (!clips.is_array()) return;  // cook() reports it
        for (u32 i = 0; i < clips.size(); ++i) {
            const JsonValue c = clips.at(i);
            Guid g;
            if (!c.is_string() || !parse_guid_ref(c.as_string(), g)) {
                detail::error_at(diags, Errc::ParseError, in.display_path, c, kStage, "a clip must be \"guid:<uuid>\" of a sound");
                continue;
            }
            DepRef d;
            d.guid = g;
            d.expected = ChunkType::Sfx;
            d.line = c.line();
            d.col = c.col();
            String where;
            appendf(where, "clips[%u]", i);
            d.where.assign(where.view());
            out.push_back(d);
        }
    }

    bool cook(const ImportInput& in, asset::ContainerWriter& out, DiagList& diags) const override {
        const JsonValue doc = in.settings;
        const StringView file = in.display_path;
        const u32 errors_before = diags.error_count();
        if (!detail::check_keys(doc, {"format", "version", "guid", "clips", "pick", "no_repeat", "pitch", "gain_db"}, file, kStage, diags))
            return false;
        const JsonValue fmt = doc.get("format");
        if (!fmt.is_string() || fmt.as_string() != "mce-sound") {
            detail::error_at(diags, Errc::ParseError, file, fmt.valid() ? fmt : doc, kStage, "\"format\" must be \"mce-sound\"");
            return false;
        }
        const JsonValue ver = doc.get("version");
        if (!ver.is_integer() || ver.as_number() != kSoundFormatVersion) {
            String msg;
            appendf(msg, "unsupported sound set version (this cook reads version %u)", kSoundFormatVersion);
            detail::error_at(diags, Errc::VersionTooNew, file, ver.valid() ? ver : doc, kStage, msg);
            return false;
        }

        asset::SoundSetHeader h{};
        h.pick = asset::SoundSetPick::Random;
        h.pitch_min = h.pitch_max = 1.0f;
        if (const JsonValue p = doc.get("pick"); p.valid()) {
            const StringView s = p.is_string() ? p.as_string() : StringView();
            if (s == "sequence") h.pick = asset::SoundSetPick::Sequence;
            else if (s != "random") detail::error_at(diags, Errc::ParseError, file, p, kStage, "'pick' must be random or sequence");
        }
        bool no_repeat = true;
        detail::read_bool(doc, "no_repeat", true, no_repeat, file, kStage, diags);
        if (no_repeat) h.flags |= asset::SoundSetFlags::NoRepeat;
        f32 pitch[2] = {1.0f, 1.0f}, gain[2] = {0.0f, 0.0f};
        read_range(doc, "pitch", 0.25f, 4.0f, pitch, file, diags);
        read_range(doc, "gain_db", -60.0f, 12.0f, gain, file, diags);
        h.pitch_min = pitch[0];
        h.pitch_max = pitch[1];
        h.gain_min_db = gain[0];
        h.gain_max_db = gain[1];

        Array<Guid> members;
        const JsonValue clips = doc.get("clips");
        if (!clips.is_array() || clips.size() > kMaxClips) {
            detail::error_at(diags, Errc::ParseError, file, clips.valid() ? clips : doc, kStage, "'clips' must list up to 4096 sounds");
        } else {
            if (clips.size() == 0) diags.warning(file, clips.line(), clips.col(), kStage, "the sound set has no clips: it plays nothing");
            for (u32 i = 0; i < clips.size(); ++i) {
                Guid g;
                if (!clips.at(i).is_string() || !parse_guid_ref(clips.at(i).as_string(), g)) continue;  // scan_dependencies reported it
                // A set of sets would pick twice: refuse it here, where the line is known.
                JsonValue member;
                StringView member_path;
                if (in.lookup && in.lookup->native_document(g, member, member_path) && member.get("format").is_string() &&
                    member.get("format").as_string() == "mce-sound") {
                    detail::error_at(diags, Errc::ParseError, file, clips.at(i), kStage, "a sound set cannot contain another sound set");
                    continue;
                }
                members.push_back(g);
            }
        }
        if (diags.error_count() != errors_before) return false;

        Array<byte> payload;
        asset::write_sound_set(h, members.span(), payload);
        asset::ContainerWriter::ChunkDesc d;
        d.type = ChunkType::Sfx;
        d.version = asset::chunk_version(asset::SFX_VERSION, 2);
        d.guid = in.guid;
        out.add(d, payload.span());
        return true;
    }
};

}  // namespace

mem::UniquePtr<IImporter> make_sound_set_importer() { return mem::make_unique<SoundSetImporter>(mem::heap()); }

}  // namespace mce::cook
