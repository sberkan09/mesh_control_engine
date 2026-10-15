// .mctimeline -> CHNK_TIMELINE: one keyed timeline (FR-ENT-110, M-CINEMA 1). Tracks of
// time-stamped keys a TimelinePlayer component samples every tick (kit/timeline.h). A track's
// "target" is the playing entity ("self", the default) or one of the player's eight bound
// entities (0 to 7, TimelinePlayer::bindings). Property paths are checked against the reflected
// components, sound GUIDs are dependencies; errors name file:line:col.
//
// {
//   "format": "mce-timeline", "version": 1, "guid": "<uuid>", "name": "intro",
//   "duration": 6,                                          // seconds; default: the last key's end
//   "tracks": [
//     {"type": "transform", "target": 0, "keys": [          // LocalTransform channels; any subset per key
//       {"t": 0, "position": [0, 1, 4], "rotation": [0, 0, 0, 1], "scale": [1, 1, 1], "ease": "smooth"}]},
//     {"type": "property", "target": 1, "path": "PointLight3D.intensity",
//      "keys": [{"t": 0, "value": 0}, {"t": 2, "value": 4, "ease": "step"}]},   // float, vector, color or quat
//     {"type": "activation", "target": 2, "path": "MeshRef.visible", "ranges": [{"from": 1, "to": 3}]},
//     {"type": "animation", "target": 3, "keys": [{"t": 0, "clip": "walk", "loop": true, "fade": 0.2}]},
//     {"type": "audio", "keys": [{"t": 0.5, "sound": "guid:<sfx uuid>", "gain_db": -3}]},
//     {"type": "camera", "cuts": [{"t": 0, "camera": 4}, {"t": 3, "camera": 5}]},
//     {"type": "event", "keys": [{"t": 2.5, "emit": "door_open"}]},
//     {"type": "subtitle", "lines": [{"t": 1, "duration": 2, "text": "Who's there?", "speaker": "Mira"}]}
//   ]
// }
//
// Eases: "linear" (the default), "step", "smooth", "ease_in", "ease_out"; a key's ease shapes the
// curve from it to the next key. Keys of a track are in time order.

#include <cstdio>
#include <cstring>

#include "importer_common.h"
#include "mce/asset/formats.h"
#include "mce/refl/property_path.h"
#include "mce/refl/refl.h"
#include "mce/string_id.h"

namespace mce::cook {

namespace {

constexpr const char* kStage = "cook.timeline";
constexpr u32 kTimelineFormatVersion = 1;
constexpr u32 kMaxTracks = 256;
constexpr u32 kMaxKeys = 65535;

using asset::TimelineEase;
using asset::TimelineKey;
using asset::TimelineTrack;
using asset::TimelineTrackKind;

struct Builder {
    StringView file;
    DiagList& diags;
    Array<TimelineTrack> tracks;
    Array<TimelineKey> keys;
    Array<char> strings;

    void error(JsonValue at, StringView msg) { detail::error_at(diags, Errc::ParseError, file, at, kStage, msg); }

    u32 add_string(StringView s) {
        const u32 at = static_cast<u32>(strings.size());
        for (char c : s) strings.push_back(c);
        strings.push_back('\0');
        return at;
    }

    // "self" / absent -> kTimelineSelf, 0..7 -> the binding.
    bool read_target(JsonValue obj, StringView key, u8& out) {
        out = asset::kTimelineSelf;
        const JsonValue v = obj.get(key);
        if (!v.valid() || (v.is_string() && v.as_string() == "self")) return true;
        if (!v.is_integer() || v.as_number() < 0 || v.as_number() >= asset::kTimelineBindings) {
            String msg;
            appendf(msg, "'%.*s' must be \"self\" or a binding index 0 to %u", static_cast<int>(key.size()), key.data(),
                    asset::kTimelineBindings - 1);
            error(v, msg);
            return false;
        }
        out = static_cast<u8>(v.as_number());
        return true;
    }

    bool read_time(JsonValue obj, StringView key, f32& out) {
        const JsonValue v = obj.get(key);
        if (!v.is_number() || v.as_number() < 0) {
            String msg;
            appendf(msg, "'%.*s' must be seconds >= 0", static_cast<int>(key.size()), key.data());
            error(v.valid() ? v : obj, msg);
            return false;
        }
        out = static_cast<f32>(v.as_number());
        return true;
    }

    bool read_ease(JsonValue obj, TimelineEase& out) {
        out = TimelineEase::Linear;
        const JsonValue v = obj.get("ease");
        if (!v.valid()) return true;
        struct Name {
            const char* name;
            TimelineEase ease;
        };
        static constexpr Name kEases[] = {{"linear", TimelineEase::Linear}, {"step", TimelineEase::Step}, {"smooth", TimelineEase::Smooth},
                                          {"ease_in", TimelineEase::EaseIn}, {"ease_out", TimelineEase::EaseOut}};
        if (v.is_string())
            for (const Name& n : kEases)
                if (v.as_string() == n.name) {
                    out = n.ease;
                    return true;
                }
        error(v, "'ease' must be \"linear\", \"step\", \"smooth\", \"ease_in\" or \"ease_out\"");
        return false;
    }

    bool read_values(JsonValue v, u32 count, f32 out[4]) {
        if (count == 1 && v.is_number()) {
            out[0] = static_cast<f32>(v.as_number());
            return true;
        }
        if (!v.is_array() || v.size() != count) {
            String msg;
            if (count == 1) appendf(msg, "expected a number");
            else appendf(msg, "expected an array of %u numbers", count);
            error(v, msg);
            return false;
        }
        for (u32 i = 0; i < count; ++i) {
            if (!v.at(i).is_number()) {
                error(v.at(i), "expected a number");
                return false;
            }
            out[i] = static_cast<f32>(v.at(i).as_number());
        }
        return true;
    }

    // The keys array of a track, in time order.
    bool key_list(JsonValue track, StringView name, JsonValue& out) {
        out = track.get(name);
        if (!out.is_array() || out.size() == 0) {
            String msg;
            appendf(msg, "'%.*s' must be a non-empty array", static_cast<int>(name.size()), name.data());
            error(out.valid() ? out : track, msg);
            return false;
        }
        if (out.size() > kMaxKeys) {
            error(out, "too many keys in one track");
            return false;
        }
        return true;
    }

    TimelineKey* begin_key(JsonValue k, std::initializer_list<StringView> allowed, const char* time_key = "t") {
        if (!k.is_object()) {
            error(k, "a key must be an object");
            return nullptr;
        }
        if (!detail::check_keys(k, allowed, file, kStage, diags)) return nullptr;
        TimelineKey key{};
        key.text = asset::kTimelineNoString;
        key.text2 = asset::kTimelineNoString;
        key.target = asset::kTimelineSelf;
        if (!read_time(k, time_key, key.t)) return nullptr;
        keys.push_back(key);
        return &keys.back();
    }

    // Closes the track whose keys start at `first`: checks their time order.
    void end_track(TimelineTrack t, u32 first, JsonValue list) {
        t.first_key = first;
        t.key_count = static_cast<u32>(keys.size()) - first;
        for (u32 i = first + 1; i < keys.size(); ++i)
            if (keys[i].t < keys[i - 1].t) {
                error(list.at(i - first), "keys must be in time order");
                break;
            }
        if (t.key_count > 0) tracks.push_back(t);
    }

    // "Component.field.path" against the reflected components: the leaf kind and value count.
    bool resolve_path(JsonValue at, StringView path, bool activation, u8& value_count, u8& flags) {
        const usize dot = path.find('.');
        const refl::TypeRecord* type = dot == StringView::npos || dot == 0 ? nullptr : refl::find_type(StringId(path.substr(0, dot)));
        if (!type || !type->is_component()) {
            String msg;
            appendf(msg, "'%.*s' does not name a reflected component field (\"Component.field\")", static_cast<int>(path.size()),
                    path.data());
            error(at, msg);
            return false;
        }
        Result<refl::PropertyRef> ref = refl::resolve_path(*type, path.substr(dot + 1));
        if (!ref || ref.value().count != 1) {
            String msg;
            appendf(msg, "'%.*s' does not resolve to one field", static_cast<int>(path.size()), path.data());
            error(at, msg);
            return false;
        }
        using K = refl::FieldKind;
        const K kind = ref.value().kind;
        flags = 0;
        if (activation) {
            if (kind == K::Bool || kind == K::U8 || kind == K::U32 || kind == K::I32) return true;
            error(at, "an activation track drives a bool or integer field");
            return false;
        }
        switch (kind) {
            case K::F32: value_count = 1; return true;
            case K::Vec2: value_count = 2; return true;
            case K::Vec3: value_count = 3; return true;
            case K::Vec4:
            case K::Color: value_count = 4; return true;
            case K::Quat:
                value_count = 4;
                flags = asset::TimelineTrackFlags::Quat;
                return true;
            default:
                error(at, "a property track animates a float, vector, color or quaternion field");
                return false;
        }
    }

    void transform_track(JsonValue track, u8 target) {
        JsonValue list;
        if (!key_list(track, "keys", list)) return;
        struct Channel {
            const char* key;
            const char* path;
            u8 count;
            u8 flags;
        };
        static constexpr Channel kChannels[] = {{"position", "LocalTransform.position", 3, 0},
                                                {"rotation", "LocalTransform.rotation", 4, asset::TimelineTrackFlags::Quat},
                                                {"scale", "LocalTransform.scale", 3, 0}};
        for (u32 i = 0; i < list.size(); ++i) {
            const JsonValue k = list.at(i);
            if (!k.is_object()) error(k, "a key must be an object");
            else detail::check_keys(k, {"t", "position", "rotation", "scale", "ease"}, file, kStage, diags);
        }
        bool any = false;
        for (const Channel& c : kChannels) {
            TimelineTrack t{};
            t.kind = TimelineTrackKind::Property;
            t.target = target;
            t.value_count = c.count;
            t.flags = c.flags;
            t.path = add_string(c.path);
            const u32 first = static_cast<u32>(keys.size());
            f32 prev = 0.0f;
            for (u32 i = 0; i < list.size(); ++i) {
                const JsonValue k = list.at(i);
                if (!k.is_object() || !k.get(c.key).valid()) continue;
                TimelineKey key{};
                key.text = key.text2 = asset::kTimelineNoString;
                key.target = asset::kTimelineSelf;
                if (!read_time(k, "t", key.t) || !read_values(k.get(c.key), c.count, key.v) || !read_ease(k, key.ease)) continue;
                if (key.t < prev) error(k, "keys must be in time order");
                prev = key.t;
                keys.push_back(key);
            }
            t.first_key = first;
            t.key_count = static_cast<u32>(keys.size()) - first;
            if (t.key_count > 0) {
                tracks.push_back(t);
                any = true;
            }
        }
        if (!any) error(list, "a transform key sets position, rotation or scale");
    }

    void property_track(JsonValue track, u8 target, bool activation) {
        const JsonValue path = track.get("path");
        if (!path.is_string() || path.as_string().empty()) {
            error(path.valid() ? path : track, "'path' must be \"Component.field\"");
            return;
        }
        TimelineTrack t{};
        t.kind = activation ? TimelineTrackKind::Activation : TimelineTrackKind::Property;
        t.target = target;
        if (!resolve_path(path, path.as_string(), activation, t.value_count, t.flags)) return;
        t.path = add_string(path.as_string());
        JsonValue list;
        if (!key_list(track, activation ? "ranges" : "keys", list)) return;
        const u32 first = static_cast<u32>(keys.size());
        for (u32 i = 0; i < list.size(); ++i) {
            const JsonValue k = list.at(i);
            if (activation) {
                TimelineKey* key = begin_key(k, {"from", "to"}, "from");
                if (!key) continue;
                f32 to = 0.0f;
                if (!read_time(k, "to", to)) continue;
                if (to <= key->t) error(k.get("to"), "'to' must be after 'from'");
                key->duration = to - key->t;
            } else {
                TimelineKey* key = begin_key(k, {"t", "value", "ease"});
                if (!key) continue;
                read_values(k.get("value"), t.value_count, key->v);
                read_ease(k, key->ease);
                if (t.flags & asset::TimelineTrackFlags::Quat) {
                    const f32* q = key->v;
                    const f32 len = q[0] * q[0] + q[1] * q[1] + q[2] * q[2] + q[3] * q[3];
                    if (!(len > 0.0f)) error(k.get("value"), "a rotation must be a non-zero quaternion [x, y, z, w]");
                }
            }
        }
        end_track(t, first, list);
    }

    void cue_track(JsonValue track, TimelineTrackKind kind, u8 target) {
        const char* list_key = kind == TimelineTrackKind::Camera ? "cuts" : kind == TimelineTrackKind::Subtitle ? "lines" : "keys";
        JsonValue list;
        if (!key_list(track, list_key, list)) return;
        TimelineTrack t{};
        t.kind = kind;
        t.target = target;
        t.path = asset::kTimelineNoString;
        const u32 first = static_cast<u32>(keys.size());
        for (u32 i = 0; i < list.size(); ++i) {
            const JsonValue k = list.at(i);
            switch (kind) {
                case TimelineTrackKind::Animation: {
                    TimelineKey* key = begin_key(k, {"t", "clip", "loop", "fade"});
                    if (!key) break;
                    const JsonValue clip = k.get("clip");
                    if (!clip.is_string() || clip.as_string().empty() || clip.as_string().size() >= 32) {
                        error(clip.valid() ? clip : k, "'clip' must be a clip name of 1 to 31 bytes");
                        break;
                    }
                    key->id = StringId(clip.as_string()).value;
                    key->text = add_string(clip.as_string());
                    bool loop = false;
                    detail::read_bool(k, "loop", false, loop, file, kStage, diags);
                    f64 fade = -1.0;
                    detail::read_number(k, "fade", -1.0, fade, file, kStage, diags);
                    if (k.get("fade").valid() && fade < 0) error(k.get("fade"), "'fade' must be seconds >= 0");
                    key->v[0] = loop ? 1.0f : 0.0f;
                    key->v[1] = static_cast<f32>(fade);
                    break;
                }
                case TimelineTrackKind::Audio: {
                    TimelineKey* key = begin_key(k, {"t", "sound", "gain_db"});
                    if (!key) break;
                    const JsonValue v = k.get("sound");
                    Guid g;
                    if (!v.is_string() || !parse_guid_ref(v.as_string(), g)) {
                        error(v.valid() ? v : k, "'sound' must be \"guid:<uuid>\"");
                        break;
                    }
                    key->guid[0] = g.hi;
                    key->guid[1] = g.lo;
                    f64 gain = 0.0;
                    detail::read_number(k, "gain_db", 0.0, gain, file, kStage, diags);
                    key->v[0] = static_cast<f32>(gain);
                    break;
                }
                case TimelineTrackKind::Camera: {
                    TimelineKey* key = begin_key(k, {"t", "camera"});
                    if (!key) break;
                    if (!k.get("camera").valid()) error(k, "a cut names its 'camera' (\"self\" or a binding index)");
                    else read_target(k, "camera", key->target);
                    break;
                }
                case TimelineTrackKind::Event: {
                    TimelineKey* key = begin_key(k, {"t", "emit"});
                    if (!key) break;
                    const JsonValue v = k.get("emit");
                    if (!v.is_string() || v.as_string().empty()) error(v.valid() ? v : k, "'emit' must be a non-empty signal name");
                    else key->id = StringId(v.as_string()).value;
                    break;
                }
                case TimelineTrackKind::Subtitle: {
                    TimelineKey* key = begin_key(k, {"t", "duration", "text", "speaker"});
                    if (!key) break;
                    f32 d = 0.0f;
                    if (read_time(k, "duration", d) && d <= 0.0f) error(k.get("duration"), "'duration' must be > 0");
                    key->duration = d;
                    const JsonValue text = k.get("text");
                    if (!text.is_string()) {
                        error(text.valid() ? text : k, "'text' must be a string");
                        break;
                    }
                    key->text = add_string(text.as_string());
                    if (const JsonValue sp = k.get("speaker"); sp.valid()) {
                        if (!sp.is_string()) error(sp, "'speaker' must be a string");
                        else key->text2 = add_string(sp.as_string());
                    }
                    break;
                }
                default: break;
            }
        }
        end_track(t, first, list);
    }
};

class TimelineImporter final : public IImporter {
public:
    StringView id() const override { return "timeline"; }
    u32 version() const override { return 1; }
    ChunkType type() const override { return ChunkType::Timeline; }
    bool native() const override { return true; }
    bool handles_extension(StringView ext) const override { return ext == ".mctimeline"; }

    void scan_dependencies(const ImportInput& in, Array<DepRef>& out, DiagList& diags) const override {
        const JsonValue tracks = in.settings.get("tracks");
        if (!tracks.is_array()) return;
        for (u32 i = 0; i < tracks.size(); ++i) {
            const JsonValue track = tracks.at(i);
            const JsonValue type = track.is_object() ? track.get("type") : JsonValue();
            if (!type.is_string() || type.as_string() != "audio") continue;
            const JsonValue list = track.get("keys");
            if (!list.is_array()) continue;
            for (u32 j = 0; j < list.size(); ++j) {
                const JsonValue v = list.at(j).is_object() ? list.at(j).get("sound") : JsonValue();
                if (!v.valid()) continue;
                Guid g;
                if (!v.is_string() || !parse_guid_ref(v.as_string(), g)) {
                    detail::error_at(diags, Errc::ParseError, in.display_path, v, kStage, "expected \"guid:<uuid>\"");
                    continue;
                }
                DepRef d;
                d.guid = g;
                d.expected = ChunkType::Sfx;
                d.line = v.line();
                d.col = v.col();
                char where[64];
                std::snprintf(where, sizeof(where), "tracks[%u].keys[%u].sound", i, j);
                d.where.assign(where);
                out.push_back(d);
            }
        }
    }

    bool cook(const ImportInput& in, asset::ContainerWriter& out, DiagList& diags) const override {
        const JsonValue doc = in.settings;
        const StringView file = in.display_path;
        const u32 errors_before = diags.error_count();
        if (!detail::check_keys(doc, {"format", "version", "guid", "name", "duration", "tracks"}, file, kStage, diags)) return false;
        const JsonValue fmt = doc.get("format");
        if (!fmt.is_string() || fmt.as_string() != "mce-timeline") {
            detail::error_at(diags, Errc::ParseError, file, fmt.valid() ? fmt : doc, kStage, "\"format\" must be \"mce-timeline\"");
            return false;
        }
        const JsonValue ver = doc.get("version");
        if (!ver.is_integer() || ver.as_number() != kTimelineFormatVersion) {
            String msg;
            appendf(msg, "unsupported timeline version (this cook reads version %u)", kTimelineFormatVersion);
            detail::error_at(diags, Errc::VersionTooNew, file, ver.valid() ? ver : doc, kStage, msg);
            return false;
        }

        asset::TimelineHeader header{};
        if (const JsonValue name = doc.get("name"); name.valid()) {
            if (!name.is_string() || name.as_string().size() >= sizeof(header.name))
                detail::error_at(diags, Errc::ParseError, file, name, kStage, "'name' must be a string of at most 31 bytes");
            else std::memcpy(header.name, name.as_string().data(), name.as_string().size());
        }

        const JsonValue tracks = doc.get("tracks");
        if (!tracks.is_array() || tracks.size() == 0 || tracks.size() > kMaxTracks) {
            String msg;
            appendf(msg, "'tracks' must be an array of 1 to %u tracks", kMaxTracks);
            detail::error_at(diags, Errc::ParseError, file, tracks.valid() ? tracks : doc, kStage, msg);
            return false;
        }
        Builder b{file, diags, {}, {}, {}};
        for (u32 i = 0; i < tracks.size(); ++i) {
            const JsonValue track = tracks.at(i);
            if (!track.is_object()) {
                b.error(track, "a track must be an object");
                continue;
            }
            const JsonValue type = track.get("type");
            const StringView ty = type.is_string() ? type.as_string() : StringView();
            u8 target = asset::kTimelineSelf;
            if (ty == "transform") {
                if (detail::check_keys(track, {"type", "target", "keys"}, file, kStage, diags) && b.read_target(track, "target", target))
                    b.transform_track(track, target);
            } else if (ty == "property" || ty == "activation") {
                const bool act = ty == "activation";
                if (detail::check_keys(track, {"type", "target", "path", act ? StringView("ranges") : StringView("keys")}, file, kStage,
                                       diags) &&
                    b.read_target(track, "target", target))
                    b.property_track(track, target, act);
            } else if (ty == "animation" || ty == "audio" || ty == "event") {
                const TimelineTrackKind kind = ty == "animation" ? TimelineTrackKind::Animation
                                               : ty == "audio"   ? TimelineTrackKind::Audio
                                                                 : TimelineTrackKind::Event;
                if (detail::check_keys(track, {"type", "target", "keys"}, file, kStage, diags) && b.read_target(track, "target", target))
                    b.cue_track(track, kind, target);
            } else if (ty == "camera") {
                if (detail::check_keys(track, {"type", "cuts"}, file, kStage, diags)) b.cue_track(track, TimelineTrackKind::Camera, target);
            } else if (ty == "subtitle") {
                if (detail::check_keys(track, {"type", "lines"}, file, kStage, diags)) b.cue_track(track, TimelineTrackKind::Subtitle, target);
            } else {
                b.error(type.valid() ? type : track,
                        "unknown track type: expected transform, property, activation, animation, audio, camera, event or subtitle");
            }
        }
        if (b.tracks.size() > kMaxTracks || b.keys.size() > (1u << 20)) b.error(tracks, "too many tracks or keys");

        f32 end = 0.0f;
        for (const TimelineKey& k : b.keys) end = k.t + k.duration > end ? k.t + k.duration : end;
        if (const JsonValue d = doc.get("duration"); d.valid()) {
            if (!d.is_number() || !(d.as_number() > 0)) {
                b.error(d, "'duration' must be seconds > 0");
            } else {
                header.duration = static_cast<f32>(d.as_number());
                if (header.duration < end) {
                    String msg;
                    appendf(msg, "'duration' (%g s) ends before the last key (%g s)", static_cast<f64>(header.duration), static_cast<f64>(end));
                    b.error(d, msg);
                }
            }
        } else {
            header.duration = end;
            if (!(end > 0.0f)) b.error(doc, "a timeline whose keys all sit at 0 needs a 'duration' > 0");
        }
        if (diags.error_count() != errors_before) return false;

        while (b.strings.size() % 4 != 0) b.strings.push_back('\0');
        header.track_count = static_cast<u32>(b.tracks.size());
        header.key_count = static_cast<u32>(b.keys.size());
        header.string_bytes = static_cast<u32>(b.strings.size());

        Array<byte> payload;
        const usize tracks_at = sizeof(header);
        const usize keys_at = tracks_at + b.tracks.size() * sizeof(TimelineTrack);
        const usize strings_at = keys_at + b.keys.size() * sizeof(TimelineKey);
        payload.resize(strings_at + b.strings.size());
        std::memcpy(payload.data(), &header, sizeof(header));
        if (!b.tracks.empty()) std::memcpy(payload.data() + tracks_at, b.tracks.data(), b.tracks.size() * sizeof(TimelineTrack));
        if (!b.keys.empty()) std::memcpy(payload.data() + keys_at, b.keys.data(), b.keys.size() * sizeof(TimelineKey));
        if (!b.strings.empty()) std::memcpy(payload.data() + strings_at, b.strings.data(), b.strings.size());
        asset::ContainerWriter::ChunkDesc d;
        d.type = ChunkType::Timeline;
        d.version = asset::chunk_version(asset::TIMELINE_VERSION, 0);
        d.guid = in.guid;
        out.add(d, payload.span());
        return true;
    }
};

}  // namespace

mem::UniquePtr<IImporter> make_timeline_importer() { return mem::make_unique<TimelineImporter>(mem::heap()); }

}  // namespace mce::cook
