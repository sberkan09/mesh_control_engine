// .mctimeline -> CHNK_TIMELINE: one keyed timeline (FR-ENT-110, M-CINEMA). Tracks of
// time-stamped keys a TimelinePlayer component samples every tick (kit/timeline.h). A track's
// "target" is the playing entity ("self", the default) or one of the player's eight bound
// entities (0 to 7, TimelinePlayer::bindings). Property paths are checked against the reflected
// components, sound GUIDs are dependencies; errors name file:line:col. The reader and writer are
// mce/cook/timeline_text.h (the editor's Timeline panel and MCP write through them).
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
#include <cstdlib>
#include <cstring>

#include "importer_common.h"
#include "mce/asset/formats.h"
#include "mce/cook/timeline_text.h"
#include "mce/refl/property_path.h"
#include "mce/refl/refl.h"
#include "mce/string_id.h"

namespace mce::cook {

namespace {

constexpr const char* kStage = "cook.timeline";
constexpr u32 kMaxTracks = 256;
constexpr u32 kMaxKeys = 65535;

using asset::TimelineEase;
using asset::TimelineKey;
using asset::TimelineTrack;
using asset::TimelineTrackKind;
using T = TimelineDocTrackType;

constexpr const char* kTypeNames[kTimelineDocTrackTypes] = {"transform", "property", "activation", "animation",
                                                             "audio",     "camera",   "event",      "subtitle"};
constexpr const char* kEaseNames[] = {"linear", "step", "smooth", "ease_in", "ease_out"};
constexpr u32 kEaseCount = sizeof(kEaseNames) / sizeof(kEaseNames[0]);

// The array key a track's keys live under.
const char* list_key(T t) {
    switch (t) {
        case T::Activation: return "ranges";
        case T::Camera: return "cuts";
        case T::Subtitle: return "lines";
        default: return "keys";
    }
}

bool has_target(T t) { return t != T::Camera && t != T::Subtitle; }

struct Reader {
    StringView file;
    DiagList& diags;

    void error(JsonValue at, StringView msg) { detail::error_at(diags, Errc::ParseError, file, at, kStage, msg); }

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
        if (v.is_string())
            for (u32 i = 0; i < kEaseCount; ++i)
                if (v.as_string() == kEaseNames[i]) {
                    out = static_cast<TimelineEase>(i);
                    return true;
                }
        error(v, "'ease' must be \"linear\", \"step\", \"smooth\", \"ease_in\" or \"ease_out\"");
        return false;
    }

    bool read_values(JsonValue owner, JsonValue v, u32 count, f32* out) {
        if (count == 1 && v.is_number()) {
            out[0] = static_cast<f32>(v.as_number());
            return true;
        }
        if (!v.is_array() || v.size() != count) {
            String msg;
            if (count == 1) appendf(msg, "expected a number");
            else appendf(msg, "expected an array of %u numbers", count);
            error(v.valid() ? v : owner, msg);
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

    bool nonzero_quat(JsonValue at, const f32* q) {
        if (q[0] * q[0] + q[1] * q[1] + q[2] * q[2] + q[3] * q[3] > 0.0f) return true;
        error(at, "a rotation must be a non-zero quaternion [x, y, z, w]");
        return false;
    }

    bool read_name(JsonValue k, const char* key, String& out, const char* msg, usize max = 0) {
        const JsonValue v = k.get(key);
        if (!v.is_string() || v.as_string().empty() || (max && v.as_string().size() > max)) {
            error(v.valid() ? v : k, msg);
            return false;
        }
        out.assign(v.as_string());
        return true;
    }

    void read_key(JsonValue k, TimelineDocTrack& tr, TimelineDocKey& key) {
        switch (tr.type) {
            case T::Transform: {
                if (!detail::check_keys(k, {"t", "position", "rotation", "scale", "ease"}, file, kStage, diags)) return;
                read_time(k, "t", key.t);
                read_ease(k, key.ease);
                if (const JsonValue v = k.get("position"); v.valid() && read_values(k, v, 3, key.position))
                    key.channels |= TimelineChannel::Position;
                if (const JsonValue v = k.get("rotation"); v.valid() && read_values(k, v, 4, key.rotation) && nonzero_quat(v, key.rotation))
                    key.channels |= TimelineChannel::Rotation;
                if (const JsonValue v = k.get("scale"); v.valid() && read_values(k, v, 3, key.scale)) key.channels |= TimelineChannel::Scale;
                if (!k.get("position").valid() && !k.get("rotation").valid() && !k.get("scale").valid())
                    error(k, "a transform key sets position, rotation or scale");
                return;
            }
            case T::Property:
                if (!detail::check_keys(k, {"t", "value", "ease"}, file, kStage, diags)) return;
                read_time(k, "t", key.t);
                read_ease(k, key.ease);
                if (tr.value_count == 0) return;  // the path did not resolve (reported)
                if (read_values(k, k.get("value"), tr.value_count, key.value) && tr.quat) nonzero_quat(k.get("value"), key.value);
                return;
            case T::Activation: {
                if (!detail::check_keys(k, {"from", "to"}, file, kStage, diags)) return;
                f32 to = 0.0f;
                if (!read_time(k, "from", key.t) || !read_time(k, "to", to)) return;
                if (to <= key.t) error(k.get("to"), "'to' must be after 'from'");
                key.duration = to - key.t;
                return;
            }
            case T::Animation: {
                if (!detail::check_keys(k, {"t", "clip", "loop", "fade"}, file, kStage, diags)) return;
                read_time(k, "t", key.t);
                read_name(k, "clip", key.name, "'clip' must be a clip name of 1 to 31 bytes", 31);
                detail::read_bool(k, "loop", false, key.loop, file, kStage, diags);
                if (const JsonValue f = k.get("fade"); f.valid()) {
                    if (!f.is_number() || f.as_number() < 0) error(f, "'fade' must be seconds >= 0");
                    else key.fade = static_cast<f32>(f.as_number());
                }
                return;
            }
            case T::Audio: {
                if (!detail::check_keys(k, {"t", "sound", "gain_db"}, file, kStage, diags)) return;
                read_time(k, "t", key.t);
                const JsonValue v = k.get("sound");
                if (!v.is_string() || !parse_guid_ref(v.as_string(), key.sound)) error(v.valid() ? v : k, "'sound' must be \"guid:<uuid>\"");
                f64 gain = 0.0;
                detail::read_number(k, "gain_db", 0.0, gain, file, kStage, diags);
                key.gain_db = static_cast<f32>(gain);
                return;
            }
            case T::Camera:
                if (!detail::check_keys(k, {"t", "camera"}, file, kStage, diags)) return;
                read_time(k, "t", key.t);
                if (!k.get("camera").valid()) error(k, "a cut names its 'camera' (\"self\" or a binding index)");
                else read_target(k, "camera", key.camera);
                return;
            case T::Event:
                if (!detail::check_keys(k, {"t", "emit"}, file, kStage, diags)) return;
                read_time(k, "t", key.t);
                read_name(k, "emit", key.name, "'emit' must be a non-empty signal name");
                return;
            case T::Subtitle: {
                if (!detail::check_keys(k, {"t", "duration", "text", "speaker"}, file, kStage, diags)) return;
                read_time(k, "t", key.t);
                if (read_time(k, "duration", key.duration) && key.duration <= 0.0f) error(k.get("duration"), "'duration' must be > 0");
                const JsonValue text = k.get("text");
                if (!text.is_string()) error(text.valid() ? text : k, "'text' must be a string");
                else key.name.assign(text.as_string());
                if (const JsonValue sp = k.get("speaker"); sp.valid()) {
                    if (!sp.is_string()) error(sp, "'speaker' must be a string");
                    else key.speaker.assign(sp.as_string());
                }
                return;
            }
        }
    }

    void read_track(JsonValue track, TimelineDocTrack& tr) {
        const JsonValue type = track.get("type");
        const StringView ty = type.is_string() ? type.as_string() : StringView();
        u32 i = 0;
        while (i < kTimelineDocTrackTypes && ty != kTypeNames[i]) ++i;
        if (i == kTimelineDocTrackTypes) {
            error(type.valid() ? type : track,
                  "unknown track type: expected transform, property, activation, animation, audio, camera, event or subtitle");
            return;
        }
        tr.type = static_cast<T>(i);
        const bool pathed = tr.type == T::Property || tr.type == T::Activation;
        const StringView keys_name = list_key(tr.type);
        bool keys_ok = true;
        if (pathed) keys_ok = detail::check_keys(track, {"type", "target", "path", keys_name}, file, kStage, diags);
        else if (has_target(tr.type)) keys_ok = detail::check_keys(track, {"type", "target", keys_name}, file, kStage, diags);
        else keys_ok = detail::check_keys(track, {"type", keys_name}, file, kStage, diags);
        if (!keys_ok || !read_target(track, "target", tr.target)) return;
        if (pathed) {
            const JsonValue path = track.get("path");
            if (!path.is_string() || path.as_string().empty()) {
                error(path.valid() ? path : track, "'path' must be \"Component.field\"");
                return;
            }
            tr.path.assign(path.as_string());
            String why;
            if (!resolve_timeline_path(tr, why)) {
                error(path, why.view());
                return;
            }
        }
        const JsonValue list = track.get(keys_name);
        if (!list.is_array() || list.size() == 0) {
            String msg;
            appendf(msg, "'%.*s' must be a non-empty array", static_cast<int>(keys_name.size()), keys_name.data());
            error(list.valid() ? list : track, msg);
            return;
        }
        if (list.size() > kMaxKeys) {
            error(list, "too many keys in one track");
            return;
        }
        for (u32 k = 0; k < list.size(); ++k) {
            const JsonValue kv = list.at(k);
            if (!kv.is_object()) {
                error(kv, "a key must be an object");
                continue;
            }
            TimelineDocKey key;
            read_key(kv, tr, key);
            if (!tr.keys.empty() && key.t < tr.keys.back().t) error(kv, "keys must be in time order");
            tr.keys.push_back(key);
        }
    }
};

// ---- writing ----

// The shortest decimal that reads back as the same float.
void append_f32(String& out, f32 v) {
    if (v == 0.0f) v = 0.0f;  // no "-0"
    char buf[32];
    for (int p = 6; p <= 9; ++p) {
        std::snprintf(buf, sizeof(buf), "%.*g", p, static_cast<f64>(v));
        if (std::strtof(buf, nullptr) == v) break;
    }
    out += StringView(buf);
}

void append_floats(String& out, const f32* v, u32 n) {
    if (n == 1) {
        append_f32(out, v[0]);
        return;
    }
    out += "[";
    for (u32 i = 0; i < n; ++i) {
        if (i) out += ", ";
        append_f32(out, v[i]);
    }
    out += "]";
}

void field(String& out, const char* key) {
    out += ", ";
    append_json_string(out, key);
    out += ": ";
}

void write_key(const TimelineDocTrack& tr, const TimelineDocKey& k, String& out) {
    out += "{ ";
    if (tr.type == T::Activation) {
        out += "\"from\": ";
        append_f32(out, k.t);
        field(out, "to");
        append_f32(out, k.t + k.duration);
        out += " }";
        return;
    }
    out += "\"t\": ";
    append_f32(out, k.t);
    switch (tr.type) {
        case T::Transform:
            if (k.channels & TimelineChannel::Position) {
                field(out, "position");
                append_floats(out, k.position, 3);
            }
            if (k.channels & TimelineChannel::Rotation) {
                field(out, "rotation");
                append_floats(out, k.rotation, 4);
            }
            if (k.channels & TimelineChannel::Scale) {
                field(out, "scale");
                append_floats(out, k.scale, 3);
            }
            break;
        case T::Property:
            field(out, "value");
            append_floats(out, k.value, tr.value_count ? tr.value_count : 1);
            break;
        case T::Animation:
            field(out, "clip");
            append_json_string(out, k.name.view());
            if (k.loop) out += ", \"loop\": true";
            if (k.fade >= 0.0f) {
                field(out, "fade");
                append_f32(out, k.fade);
            }
            break;
        case T::Audio: {
            field(out, "sound");
            String s("guid:");
            s += asset::to_string(k.sound).view();
            append_json_string(out, s.view());
            if (k.gain_db != 0.0f) {
                field(out, "gain_db");
                append_f32(out, k.gain_db);
            }
            break;
        }
        case T::Camera:
            field(out, "camera");
            if (k.camera == asset::kTimelineSelf) out += "\"self\"";
            else appendf(out, "%u", k.camera);
            break;
        case T::Event:
            field(out, "emit");
            append_json_string(out, k.name.view());
            break;
        case T::Subtitle:
            field(out, "duration");
            append_f32(out, k.duration);
            field(out, "text");
            append_json_string(out, k.name.view());
            if (!k.speaker.empty()) {
                field(out, "speaker");
                append_json_string(out, k.speaker.view());
            }
            break;
        case T::Activation: break;
    }
    if ((tr.type == T::Transform || tr.type == T::Property) && k.ease != TimelineEase::Linear) {
        field(out, "ease");
        append_json_string(out, timeline_ease_name(k.ease));
    }
    out += " }";
}

// ---- the chunk ----

struct ChunkBuilder {
    Array<TimelineTrack> tracks;
    Array<TimelineKey> keys;
    Array<char> strings;

    u32 add_string(StringView s) {
        const u32 at = static_cast<u32>(strings.size());
        for (char c : s) strings.push_back(c);
        strings.push_back('\0');
        return at;
    }
    static TimelineKey blank(const TimelineDocKey& k) {
        TimelineKey key{};
        key.t = k.t;
        key.duration = k.duration;
        key.text = key.text2 = asset::kTimelineNoString;
        key.target = asset::kTimelineSelf;
        return key;
    }
    void push(TimelineTrack t, u32 first) {
        t.first_key = first;
        t.key_count = static_cast<u32>(keys.size()) - first;
        if (t.key_count > 0) tracks.push_back(t);
    }
};

TimelineTrackKind kind_of(T t) {
    switch (t) {
        case T::Transform:
        case T::Property: return TimelineTrackKind::Property;
        case T::Activation: return TimelineTrackKind::Activation;
        case T::Animation: return TimelineTrackKind::Animation;
        case T::Audio: return TimelineTrackKind::Audio;
        case T::Camera: return TimelineTrackKind::Camera;
        case T::Event: return TimelineTrackKind::Event;
        case T::Subtitle: return TimelineTrackKind::Subtitle;
    }
    return TimelineTrackKind::Event;
}

}  // namespace

const char* timeline_track_type_name(TimelineDocTrackType t) {
    const u32 i = static_cast<u32>(t);
    return i < kTimelineDocTrackTypes ? kTypeNames[i] : "?";
}

const char* timeline_ease_name(asset::TimelineEase e) {
    const u32 i = static_cast<u32>(e);
    return i < kEaseCount ? kEaseNames[i] : "linear";
}

f32 TimelineDocument::keys_end() const {
    f32 end = 0.0f;
    for (const TimelineDocTrack& t : tracks)
        for (const TimelineDocKey& k : t.keys) end = k.t + k.duration > end ? k.t + k.duration : end;
    return end;
}

bool resolve_timeline_path(TimelineDocTrack& track, String& why) {
    const StringView path = track.path.view();
    const usize dot = path.find('.');
    const refl::TypeRecord* type = dot == StringView::npos || dot == 0 ? nullptr : refl::find_type(StringId(path.substr(0, dot)));
    why = String();
    if (!type || !type->is_component()) {
        appendf(why, "'%.*s' does not name a reflected component field (\"Component.field\")", static_cast<int>(path.size()),
                path.data());
        return false;
    }
    Result<refl::PropertyRef> ref = refl::resolve_path(*type, path.substr(dot + 1));
    if (!ref || ref.value().count != 1) {
        appendf(why, "'%.*s' does not resolve to one field", static_cast<int>(path.size()), path.data());
        return false;
    }
    using K = refl::FieldKind;
    const K kind = ref.value().kind;
    track.quat = false;
    if (track.type == TimelineDocTrackType::Activation) {
        track.value_count = 1;
        if (kind == K::Bool || kind == K::U8 || kind == K::U32 || kind == K::I32) return true;
        why.assign("an activation track drives a bool or integer field");
        return false;
    }
    switch (kind) {
        case K::F32: track.value_count = 1; return true;
        case K::Vec2: track.value_count = 2; return true;
        case K::Vec3: track.value_count = 3; return true;
        case K::Vec4:
        case K::Color: track.value_count = 4; return true;
        case K::Quat:
            track.value_count = 4;
            track.quat = true;
            return true;
        default:
            track.value_count = 0;
            why.assign("a property track animates a float, vector, color or quaternion field");
            return false;
    }
}

bool read_timeline_document(JsonValue doc, StringView file, TimelineDocument& out, DiagList& diags) {
    const u32 errors_before = diags.error_count();
    out = TimelineDocument();
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
    Reader r{file, diags};
    if (const JsonValue name = doc.get("name"); name.valid()) {
        if (!name.is_string() || name.as_string().size() >= 32) r.error(name, "'name' must be a string of at most 31 bytes");
        else out.name.assign(name.as_string());
    }
    const JsonValue tracks = doc.get("tracks");
    if (!tracks.is_array() || tracks.size() == 0 || tracks.size() > kMaxTracks) {
        String msg;
        appendf(msg, "'tracks' must be an array of 1 to %u tracks", kMaxTracks);
        r.error(tracks.valid() ? tracks : doc, msg);
        return false;
    }
    for (u32 i = 0; i < tracks.size(); ++i) {
        const JsonValue track = tracks.at(i);
        if (!track.is_object()) {
            r.error(track, "a track must be an object");
            continue;
        }
        TimelineDocTrack tr;
        r.read_track(track, tr);
        out.tracks.push_back(tr);
    }
    const f32 end = out.keys_end();
    if (const JsonValue d = doc.get("duration"); d.valid()) {
        if (!d.is_number() || !(d.as_number() > 0)) {
            r.error(d, "'duration' must be seconds > 0");
        } else {
            out.duration = static_cast<f32>(d.as_number());
            if (out.duration < end) {
                String msg;
                appendf(msg, "'duration' (%g s) ends before the last key (%g s)", static_cast<f64>(out.duration), static_cast<f64>(end));
                r.error(d, msg);
            }
        }
    } else if (!(end > 0.0f)) {
        r.error(doc, "a timeline whose keys all sit at 0 needs a 'duration' > 0");
    }
    return diags.error_count() == errors_before;
}

void write_timeline_document(const TimelineDocument& d, asset::Guid guid, String& out) {
    out += "{\n  \"format\": \"mce-timeline\", \"version\": ";
    appendf(out, "%u", kTimelineFormatVersion);
    out += ", \"guid\": ";
    append_json_string(out, asset::to_string(guid).view());
    if (!d.name.empty()) {
        field(out, "name");
        append_json_string(out, d.name.view());
    }
    if (d.duration > 0.0f) {
        field(out, "duration");
        append_f32(out, d.duration);
    }
    out += ",\n  \"tracks\": [";
    for (usize i = 0; i < d.tracks.size(); ++i) {
        const TimelineDocTrack& t = d.tracks[i];
        out += i ? ",\n    { \"type\": " : "\n    { \"type\": ";
        append_json_string(out, timeline_track_type_name(t.type));
        if (has_target(t.type) && t.target != asset::kTimelineSelf) appendf(out, ", \"target\": %u", t.target);
        if (t.type == T::Property || t.type == T::Activation) {
            field(out, "path");
            append_json_string(out, t.path.view());
        }
        field(out, list_key(t.type));
        out += "[";
        for (usize k = 0; k < t.keys.size(); ++k) {
            out += k ? ",\n        " : "\n        ";
            write_key(t, t.keys[k], out);
        }
        out += t.keys.empty() ? "] }" : "\n      ] }";
    }
    out += d.tracks.empty() ? "]\n}\n" : "\n  ]\n}\n";
}

void build_timeline_chunk(const TimelineDocument& d, Array<byte>& payload) {
    ChunkBuilder b;
    for (const TimelineDocTrack& t : d.tracks) {
        if (t.type == T::Transform) {
            struct Channel {
                u8 bit;
                const char* path;
                u8 count;
                u8 flags;
            };
            static constexpr Channel kChannels[] = {{TimelineChannel::Position, "LocalTransform.position", 3, 0},
                                                    {TimelineChannel::Rotation, "LocalTransform.rotation", 4, asset::TimelineTrackFlags::Quat},
                                                    {TimelineChannel::Scale, "LocalTransform.scale", 3, 0}};
            for (const Channel& c : kChannels) {
                TimelineTrack tr{};
                tr.kind = TimelineTrackKind::Property;
                tr.target = t.target;
                tr.value_count = c.count;
                tr.flags = c.flags;
                const u32 first = static_cast<u32>(b.keys.size());
                for (const TimelineDocKey& k : t.keys) {
                    if (!(k.channels & c.bit)) continue;
                    TimelineKey key = ChunkBuilder::blank(k);
                    key.ease = k.ease;
                    const f32* src = c.bit == TimelineChannel::Position ? k.position : c.bit == TimelineChannel::Rotation ? k.rotation : k.scale;
                    std::memcpy(key.v, src, sizeof(f32) * c.count);
                    b.keys.push_back(key);
                }
                if (b.keys.size() > first) tr.path = b.add_string(c.path);
                b.push(tr, first);
            }
            continue;
        }
        TimelineTrack tr{};
        tr.kind = kind_of(t.type);
        tr.target = has_target(t.type) ? t.target : asset::kTimelineSelf;
        tr.path = asset::kTimelineNoString;
        if (t.type == T::Property || t.type == T::Activation) {
            tr.path = b.add_string(t.path.view());
            tr.value_count = t.type == T::Property ? t.value_count : 0;
            tr.flags = t.quat ? asset::TimelineTrackFlags::Quat : 0;
        }
        const u32 first = static_cast<u32>(b.keys.size());
        for (const TimelineDocKey& k : t.keys) {
            TimelineKey key = ChunkBuilder::blank(k);
            switch (t.type) {
                case T::Property:
                    key.ease = k.ease;
                    std::memcpy(key.v, k.value, sizeof(f32) * t.value_count);
                    break;
                case T::Animation:
                    key.id = StringId(k.name.view()).value;
                    key.text = b.add_string(k.name.view());
                    key.v[0] = k.loop ? 1.0f : 0.0f;
                    key.v[1] = k.fade;
                    break;
                case T::Audio:
                    key.guid[0] = k.sound.hi;
                    key.guid[1] = k.sound.lo;
                    key.v[0] = k.gain_db;
                    break;
                case T::Camera: key.target = k.camera; break;
                case T::Event: key.id = StringId(k.name.view()).value; break;
                case T::Subtitle:
                    key.text = b.add_string(k.name.view());
                    if (!k.speaker.empty()) key.text2 = b.add_string(k.speaker.view());
                    break;
                default: break;
            }
            b.keys.push_back(key);
        }
        b.push(tr, first);
    }
    while (b.strings.size() % 4 != 0) b.strings.push_back('\0');

    asset::TimelineHeader header{};
    const usize name_len = d.name.size() < sizeof(header.name) ? d.name.size() : sizeof(header.name) - 1;
    std::memcpy(header.name, d.name.view().data(), name_len);
    header.duration = d.length();
    header.track_count = static_cast<u32>(b.tracks.size());
    header.key_count = static_cast<u32>(b.keys.size());
    header.string_bytes = static_cast<u32>(b.strings.size());
    const usize tracks_at = sizeof(header);
    const usize keys_at = tracks_at + b.tracks.size() * sizeof(TimelineTrack);
    const usize strings_at = keys_at + b.keys.size() * sizeof(TimelineKey);
    payload.resize(strings_at + b.strings.size());
    std::memcpy(payload.data(), &header, sizeof(header));
    if (!b.tracks.empty()) std::memcpy(payload.data() + tracks_at, b.tracks.data(), b.tracks.size() * sizeof(TimelineTrack));
    if (!b.keys.empty()) std::memcpy(payload.data() + keys_at, b.keys.data(), b.keys.size() * sizeof(TimelineKey));
    if (!b.strings.empty()) std::memcpy(payload.data() + strings_at, b.strings.data(), b.strings.size());
}

namespace {

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
        TimelineDocument doc;
        if (!read_timeline_document(in.settings, in.display_path, doc, diags)) return false;
        Array<byte> payload;
        build_timeline_chunk(doc, payload);
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
