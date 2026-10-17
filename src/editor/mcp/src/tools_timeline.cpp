// Timeline tools (M-CINEMA step 6): timeline_create, timeline_add_track, timeline_add_key. A cutscene
// is a .mctimeline played by a TimelinePlayer (component_add it with the timeline's GUID); the
// frame at a time comes from timeline_preview (tools_render.cpp).
//
// Every edit goes through the cook's own reader and writer, as the UI tools do: the document is
// edited as JSON, read with cook::read_timeline_document (so a bad edit fails with the cook's
// file:line message and nothing is written) and saved in the canonical form
// (cook::write_timeline_document), which reads back to the same document.

#include "mce/cook/cook.h"
#include "mce/cook/fs.h"
#include "mce/cook/timeline_text.h"
#include "tool_args.h"
#include "tools.h"

namespace mce::mcp {

namespace {

constexpr usize kMaxDiagnostics = 10;
constexpr u64 kMaxIndex = 65535;

bool is_mctimeline(StringView rel) { return rel.size() > 11 && rel.substr(rel.size() - 11) == ".mctimeline"; }

// Reads `text` as a .mctimeline; false with the cook's diagnostics in c.error.
bool read_doc(ToolCall& c, StringView rel, StringView text, cook::JsonDoc& doc, cook::TimelineDocument& out, const char* what) {
    cook::JsonError err;
    if (!doc.parse(text, err) || !doc.root().is_object()) {
        c.error += what;
        c.error += ": not a JSON object";
        return false;
    }
    cook::DiagList diags;
    if (cook::read_timeline_document(doc.root(), rel, out, diags)) return true;
    c.error += what;
    c.error += ":";
    append_diags(c.error, diags, kMaxDiagnostics);
    return false;
}

// The timeline named by 'timeline' (a path under assets/ or "guid:<uuid>"), its source text read.
bool arg_timeline(ToolCall& c, const editor::ProjectAsset*& out, cook::String& text) {
    if (!need_project(c)) return false;
    const cook::JsonValue d = arg(c, "timeline");
    if (!d.is_string()) return arg_missing(c, "timeline", "a .mctimeline path under assets/ or \"guid:<uuid>\"");
    const StringView ref = d.as_string();
    const editor::EditorProject& project = c.server.session().project();
    out = nullptr;
    if (ref.size() > 5 && ref.substr(0, 5) == "guid:") {
        const Result<asset::Guid> g = asset::parse_guid(ref.substr(5));
        if (g) out = project.find(g.value());
    } else {
        for (const editor::ProjectAsset& a : project.assets())
            if (a.path.view() == ref) out = &a;
    }
    if (!out) return fail(c, "argument 'timeline': no such asset (asset_list type Timeline lists them)");
    if (out->type != asset::ChunkType::Timeline) return fail(c, "argument 'timeline' is not a timeline (.mctimeline)");
    if (!cook::fs::read_text(project.asset_path(out->path.view()).view(), text)) return fail(c, "the timeline could not be read");
    return true;
}

void key(cook::String& out, bool& first, StringView k) {
    if (!first) out += ',';
    first = false;
    cook::append_json_string(out, k);
    out += ':';
}

// The array a track's keys live under ("keys", "ranges", "cuts" or "lines") and its keys' time.
StringView list_name(StringView type) {
    if (type == "activation") return "ranges";
    if (type == "camera") return "cuts";
    if (type == "subtitle") return "lines";
    return "keys";
}
StringView time_name(StringView type) { return type == "activation" ? StringView("from") : StringView("t"); }

f64 time_of(cook::JsonValue k, StringView name) {
    const cook::JsonValue t = k.get(name);
    return t.is_number() ? t.as_number() : 0.0;
}

// `list` with `add` merged in by time: each added key goes after the keys at or before its time.
void merged_keys(cook::JsonValue list, Span<const cook::JsonValue> add, StringView tname, cook::String& out) {
    out += '[';
    bool first = true;
    const u32 n = list.is_array() ? list.size() : 0;
    Array<bool> placed;
    placed.resize(add.size());
    auto emit = [&](cook::JsonValue v) {
        if (!first) out += ',';
        first = false;
        cook::write_json(v, out, false);
    };
    for (u32 i = 0; i < n; ++i) {
        const f64 ti = time_of(list.at(i), tname);
        for (usize a = 0; a < add.size(); ++a)
            if (!placed[a] && time_of(add[a], tname) < ti) {
                emit(add[a]);
                placed[a] = true;
            }
        emit(list.at(i));
    }
    for (usize a = 0; a < add.size(); ++a)
        if (!placed[a]) emit(add[a]);
    out += ']';
}

// The document `root` with one change: a track inserted at `insert_at` (`track` valid), or keys
// added to track `key_track` (`keys` non-empty).
void edited(cook::JsonValue root, cook::JsonValue track, u32 insert_at, u32 key_track, Span<const cook::JsonValue> keys,
            cook::String& out) {
    out += '{';
    bool first = true;
    bool wrote_tracks = false;
    for (u32 i = 0; i < root.size(); ++i) {
        const StringView k = root.key_at(i);
        key(out, first, k);
        if (k != "tracks") {
            cook::write_json(root.at(i), out, false);
            continue;
        }
        wrote_tracks = true;
        const cook::JsonValue tracks = root.at(i);
        const u32 n = tracks.is_array() ? tracks.size() : 0;
        out += '[';
        bool tfirst = true;
        auto sep = [&] {
            if (!tfirst) out += ',';
            tfirst = false;
        };
        for (u32 t = 0; t <= n; ++t) {
            if (track.valid() && t == insert_at) {
                sep();
                cook::write_json(track, out, false);
            }
            if (t == n) break;
            const cook::JsonValue tr = tracks.at(t);
            sep();
            if (keys.empty() || t != key_track) {
                cook::write_json(tr, out, false);
                continue;
            }
            const cook::JsonValue type = tr.get("type");
            const StringView tname = type.is_string() ? type.as_string() : StringView();
            const StringView lname = list_name(tname);
            out += '{';
            bool ffirst = true, wrote_list = false;
            for (u32 f = 0; f < tr.size(); ++f) {
                key(out, ffirst, tr.key_at(f));
                if (tr.key_at(f) == lname) {
                    merged_keys(tr.at(f), keys, time_name(tname), out);
                    wrote_list = true;
                } else {
                    cook::write_json(tr.at(f), out, false);
                }
            }
            if (!wrote_list) {
                key(out, ffirst, lname);
                merged_keys(cook::JsonValue(), keys, time_name(tname), out);
            }
            out += '}';
        }
        out += ']';
    }
    if (!wrote_tracks && track.valid()) {
        key(out, first, "tracks");
        out += '[';
        cook::write_json(track, out, false);
        out += ']';
    }
    out += '}';
}

// Reads the edit back, writes it canonically and saves it; the result names the timeline.
bool save_edit(ToolCall& c, const editor::ProjectAsset& a, StringView text) {
    // asset_validate_and_write rescans the project and frees `a`: work from copies.
    const cook::String path(a.path.view());
    const asset::Guid guid = a.guid;
    cook::JsonDoc doc;
    cook::TimelineDocument d;
    if (!read_doc(c, path.view(), text, doc, d, "the edit does not make a valid timeline (nothing was written)")) return false;
    cook::String out;
    cook::write_timeline_document(d, guid, out);
    if (!asset_validate_and_write(c, path.view(), guid, out.view())) return false;
    c.out.begin_object();
    c.out.key("timeline").str(path.view()).key("guid").str(asset_guid_ref(guid).view());
    c.out.key("tracks").num_u(d.tracks.size()).key("length").num_f(d.length());
    c.out.end_object();
    return true;
}

bool timeline_create(ToolCall& c) {
    StringView rel;
    if (!need_project(c) || !arg_string(c, "path", rel, true)) return false;
    if (!asset_rel_valid(rel) || !is_mctimeline(rel))
        return fail(c, "argument 'path' must be a .mctimeline path under assets/ like \"cutscenes/intro.mctimeline\" (forward slashes, no '..')");
    editor::EditorProject& project = c.server.session().project();
    if (cook::fs::exists(project.asset_path(rel).view()))
        return fail(c, "a file already exists there: timeline_add_track and timeline_add_key change it");
    StringView name;
    if (!arg_string(c, "name", name, false)) return false;
    const cook::JsonValue dur = arg(c, "duration"), tracks = arg(c, "tracks");
    if (dur.valid() && (!dur.is_number() || dur.as_number() < 0)) return arg_missing(c, "duration", "seconds, 0 or more (0: the last key's end)");
    if (!tracks.is_array() || tracks.size() == 0) return arg_missing(c, "tracks", "an array of one or more .mctimeline track objects");
    const asset::Guid guid = cook::generate_guid();
    cook::String text("{\"format\":\"mce-timeline\",\"version\":");
    cook::appendf(text, "%u", cook::kTimelineFormatVersion);
    text += ",\"guid\":";
    cook::append_json_string(text, asset::to_string(guid).view());
    if (!name.empty()) {
        text += ",\"name\":";
        cook::append_json_string(text, name);
    }
    if (dur.valid()) cook::appendf(text, ",\"duration\":%g", dur.as_number());
    text += ",\"tracks\":";
    cook::write_json(tracks, text, false);
    text += '}';
    cook::JsonDoc doc;
    cook::TimelineDocument d;
    if (!read_doc(c, rel, text.view(), doc, d, "the timeline is not valid (nothing was written)")) return false;
    cook::String out;
    cook::write_timeline_document(d, guid, out);
    if (!asset_validate_and_write(c, rel, guid, out.view())) return false;
    c.out.begin_object();
    c.out.key("timeline").str(rel).key("guid").str(asset_guid_ref(guid).view());
    c.out.key("tracks").num_u(d.tracks.size()).key("length").num_f(d.length());
    c.out.end_object();
    return true;
}

bool timeline_add_track(ToolCall& c) {
    const editor::ProjectAsset* a = nullptr;
    cook::String text;
    if (!arg_timeline(c, a, text)) return false;
    const cook::JsonValue track = arg(c, "track");
    if (!track.is_object() || !track.get("type").is_string())
        return arg_missing(c, "track", "a .mctimeline track object with a \"type\" (transform, property, activation, animation, audio, camera, event, subtitle, video)");
    cook::JsonDoc doc;
    cook::TimelineDocument d;
    if (!read_doc(c, a->path.view(), text.view(), doc, d, "the timeline on disk does not read")) return false;
    u64 index = d.tracks.size();
    if (arg(c, "index").valid() && !arg_uint(c, "index", index, 0, d.tracks.size(), false)) return false;
    cook::String out;
    edited(doc.root(), track, static_cast<u32>(index), ~0u, {}, out);
    return save_edit(c, *a, out.view());
}

bool timeline_add_key(ToolCall& c) {
    const editor::ProjectAsset* a = nullptr;
    cook::String text;
    if (!arg_timeline(c, a, text)) return false;
    cook::JsonDoc doc;
    cook::TimelineDocument d;
    if (!read_doc(c, a->path.view(), text.view(), doc, d, "the timeline on disk does not read")) return false;
    if (d.tracks.empty()) return fail(c, "the timeline has no tracks: timeline_add_track first");
    u64 track = 0;
    if (!arg_uint(c, "track", track, 0, kMaxIndex, true)) return false;
    if (track >= d.tracks.size()) return arg_missing(c, "track", "a track index of the timeline (0 = the first track)");
    Array<cook::JsonValue> keys;
    const cook::JsonValue one = arg(c, "key"), many = arg(c, "keys");
    if (one.valid() == many.valid()) return fail(c, "give exactly one of 'key' (a key object) or 'keys' (an array of them)");
    if (one.valid()) {
        if (!one.is_object()) return arg_missing(c, "key", "a key object, e.g. {\"t\": 2, \"value\": 1}");
        keys.push_back(one);
    } else {
        if (!many.is_array() || many.size() == 0) return arg_missing(c, "keys", "a non-empty array of key objects");
        for (u32 i = 0; i < many.size(); ++i) {
            if (!many.at(i).is_object()) return arg_missing(c, "keys", "an array of key objects");
            keys.push_back(many.at(i));
        }
    }
    cook::String out;
    edited(doc.root(), cook::JsonValue(), ~0u, static_cast<u32>(track), Span<const cook::JsonValue>(keys.data(), keys.size()), out);
    return save_edit(c, *a, out.view());
}

#define MCE_TL_DOC R"j("timeline":{"type":"string","description":"The timeline: a .mctimeline path under assets/ or \"guid:<uuid>\""})j"

const ToolDef kTimelineTools[] = {
    {"timeline_create",
     "Make a new timeline (.mctimeline: a cutscene or any keyed sequence) with a fresh GUID: 'tracks' (one or more) in the .mctimeline "
     "track format (asset_schema type Timeline describes it: transform, property, activation, animation, audio, camera, "
     "event, subtitle and video tracks; a track's \"target\" is \"self\" or a TimelinePlayer binding 0-7). Checked by the "
     "cook before it is written. Play it with a TimelinePlayer component (and a Cutscene for letterbox, hold and skip); "
     "timeline_preview shows a moment of it.",
     R"j({"type":"object","properties":{"path":{"type":"string","description":"New .mctimeline path under assets/, e.g. cutscenes/intro.mctimeline"},)j"
     R"j("name":{"type":"string"},"duration":{"type":"number","minimum":0,"description":"Seconds; 0 or omitted: the last key's end"},)j"
     R"j("tracks":{"type":"array","minItems":1,"description":".mctimeline track objects"}},"required":["path","tracks"]})j",
     timeline_create},
    {"timeline_add_track",
     "Add a track to a timeline, last or at 'index'. 'track' is a .mctimeline track object, e.g. {\"type\": \"property\", "
     "\"target\": 0, \"path\": \"CameraRail3D.t\", \"keys\": [{\"t\": 0, \"value\": 0}, {\"t\": 4, \"value\": 1, \"ease\": "
     "\"smooth\"}]} or {\"type\": \"subtitle\", \"lines\": [{\"t\": 1, \"duration\": 2, \"text\": \"...\"}]}.",
     R"j({"type":"object","properties":{)j" MCE_TL_DOC
     R"j(,"track":{"type":"object","description":"A .mctimeline track: {type, target, path, keys | ranges | cuts | lines}"},)j"
     R"j("index":{"type":"integer","minimum":0,"description":"Position among the tracks (default last)"}},"required":["timeline","track"]})j",
     timeline_add_track},
    {"timeline_add_key",
     "Add keys to a timeline's track (by index, 0 = the first): 'key' or 'keys' in the track's own form (a key, range, "
     "cut or line object), merged in by time. The result's length is the timeline's play length.",
     R"j({"type":"object","properties":{)j" MCE_TL_DOC
     R"j(,"track":{"type":"integer","minimum":0,"description":"Track index"},)j"
     R"j("key":{"type":"object","description":"One key, e.g. {\"t\": 2, \"value\": 1, \"ease\": \"smooth\"} or {\"t\": 3, \"camera\": 1, \"blend\": 1}"},)j"
     R"j("keys":{"type":"array","items":{"type":"object"},"description":"Several keys"}},"required":["timeline","track"]})j",
     timeline_add_key},
};

#undef MCE_TL_DOC

}  // namespace

Span<const ToolDef> timeline_tool_table() {
    return Span<const ToolDef>(kTimelineTools, sizeof(kTimelineTools) / sizeof(kTimelineTools[0]));
}

}  // namespace mce::mcp
