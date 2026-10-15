// .mcanim -> CHNK_ANIMG: a 3D animation state graph the kit's Animator runs over its MeshRef's
// clips (M-DATA-REACH 7). Clip names are the mesh's (glTF animation names); they are matched when
// the graph plays, so one graph serves every mesh with the same clip names.
//
// {
//   "format": "mce-animgraph", "version": 1, "guid": "<uuid>", "name": "hero",
//   "params": ["speed", "attack"],                  // up to 4; Animator.params holds their values in order
//   "start": "locomotion",
//   "states": [
//     { "name": "locomotion", "blend": { "param": "speed", "points": [ { "clip": "Idle", "at": 0 },
//                                                                      { "clip": "Walk", "at": 2 }, { "clip": "Run", "at": 6 } ] },
//       "events": [ { "time": 0.25, "name": "footstep" } ], "pos": [40, 80] },
//     { "name": "slash", "clip": "Slash", "loop": false, "speed": 1.2, "events": [ { "time": 0.3, "name": "hit" } ] } ],
//   "transitions": [
//     { "from": "*", "to": "slash", "when": [ { "param": "attack", "op": ">", "value": 0.5 } ], "fade": 0.1 },
//     { "from": "slash", "to": "locomotion", "on_finish": true, "fade": 0.2 } ]
// }
//
// A state's optional "pos" ([x, y]) is where the editor's Animation Graph panel draws it; the cook
// ignores it. The reader and writer are animgraph_text.h.

#include <cstdio>
#include <cstdlib>
#include <cstring>

#include "importer_common.h"
#include "mce/asset/formats.h"
#include "mce/cook/animgraph_text.h"
#include "mce/hash.h"

namespace mce::cook {

namespace {

constexpr const char* kStage = "cook.animgraph";

void copy_name(char* dst, usize cap, StringView s) {
    std::memset(dst, 0, cap);
    std::memcpy(dst, s.data(), s.size() < cap ? s.size() : cap - 1);
}

// Name capacities of the cooked structs (NUL included).
constexpr usize kParamCap = sizeof(asset::AnimGraphHeader::params[0]);
constexpr usize kStateCap = sizeof(asset::AnimGraphState::name);
constexpr usize kClipCap = sizeof(asset::AnimGraphState::clip);
constexpr usize kEventCap = sizeof(asset::AnimGraphEvent::name);

class Reader {
public:
    Reader(StringView file, DiagList& diags) : file_(file), diags_(diags) {}

    bool read(JsonValue doc, AnimGraphDocument& d) {
        doc_ = doc;
        if (!detail::check_keys(doc, {"format", "version", "guid", "name", "params", "start", "states", "transitions"}, file_, kStage, diags_))
            return false;
        const JsonValue fmt = doc.get("format");
        if (!fmt.is_string() || fmt.as_string() != "mce-animgraph") return fail(fmt, "\"format\" must be \"mce-animgraph\"");
        const JsonValue ver = doc.get("version");
        if (!ver.is_integer() || ver.as_number() != kAnimGraphFormatVersion) {
            String msg;
            appendf(msg, "unsupported animation graph version (this cook reads version %u)", kAnimGraphFormatVersion);
            detail::error_at(diags_, Errc::VersionTooNew, file_, ver.valid() ? ver : doc, kStage, msg);
            return false;
        }
        if (const JsonValue n = doc.get("name"); n.is_string()) d.name.assign(n.as_string());

        const JsonValue params = doc.get("params");
        if (params.valid()) {
            if (!params.is_array() || params.size() > asset::kAnimGraphMaxParams) return fail(params, "'params' must be an array of up to 4 names");
            for (u32 i = 0; i < params.size(); ++i) {
                StringView n;
                if (!name_of(params.at(i), kParamCap, "a parameter name", n)) return false;
                for (const String& o : d.params)
                    if (o.view() == n) return fail(params.at(i), "two parameters share this name");
                d.params.push_back(String(n));
            }
        }

        const JsonValue state_list = doc.get("states");
        if (!state_list.is_array() || state_list.size() == 0) return fail(state_list, "'states' must be a non-empty array");
        for (u32 si = 0; si < state_list.size(); ++si) {
            AnimGraphDocState s;
            if (!read_state(state_list.at(si), d, s)) return false;
            d.states.push_back(s);
        }
        if (const JsonValue start = doc.get("start"); start.valid()) {
            if (!state_index(start, d, false, d.start)) return false;
        }

        if (const JsonValue list = doc.get("transitions"); list.valid()) {
            if (!list.is_array()) return fail(list, "'transitions' must be an array");
            for (u32 ti = 0; ti < list.size(); ++ti) {
                AnimGraphDocTransition t;
                if (!read_transition(list.at(ti), d, t)) return false;
                d.transitions.push_back(t);
            }
        }
        return true;
    }

private:
    bool fail(JsonValue at, StringView msg) {
        detail::error_at(diags_, Errc::ParseError, file_, at.valid() ? at : doc_, kStage, msg);
        return false;
    }

    bool name_of(JsonValue v, usize cap, const char* what, StringView& out_name) {
        if (!v.is_string() || v.as_string().empty() || v.as_string().size() >= cap) {
            String msg;
            appendf(msg, "%s must be a string of 1 to %zu characters", what, cap - 1);
            return fail(v, msg.view());
        }
        out_name = v.as_string();
        return true;
    }

    bool param_index(JsonValue v, const AnimGraphDocument& d, u8& out_index) {
        if (v.is_string())
            for (u32 i = 0; i < d.params.size(); ++i)
                if (d.params[i].view() == v.as_string()) {
                    out_index = static_cast<u8>(i);
                    return true;
                }
        return fail(v, "'param' must name one of 'params'");
    }

    bool state_index(JsonValue v, const AnimGraphDocument& d, bool any_ok, u32& out_index) {
        if (v.is_string()) {
            if (any_ok && v.as_string() == "*") {
                out_index = kAnimGraphAny;
                return true;
            }
            for (u32 i = 0; i < d.states.size(); ++i)
                if (d.states[i].name.view() == v.as_string()) {
                    out_index = i;
                    return true;
                }
        }
        return fail(v, any_ok ? "must name a state, or \"*\" for any" : "must name a state");
    }

    bool read_state(JsonValue st, const AnimGraphDocument& d, AnimGraphDocState& s) {
        if (!st.is_object() || !detail::check_keys(st, {"name", "clip", "blend", "loop", "speed", "events", "pos"}, file_, kStage, diags_))
            return fail(st, "a state is {\"name\", \"clip\" | \"blend\", \"loop\", \"speed\", \"events\"}");
        StringView n;
        if (!name_of(st.get("name"), kStateCap, "a state's 'name'", n)) return false;
        for (const AnimGraphDocState& o : d.states)
            if (o.name.view() == n) return fail(st.get("name"), "two states share this name");
        s.name.assign(n);
        const JsonValue clip = st.get("clip"), blend = st.get("blend");
        if (clip.valid() == blend.valid()) return fail(st, "a state has exactly one of 'clip' and 'blend'");
        if (clip.valid()) {
            StringView c;
            if (!name_of(clip, kClipCap, "'clip'", c)) return false;
            s.clip.assign(c);
        } else {
            if (!blend.is_object() || !detail::check_keys(blend, {"param", "points"}, file_, kStage, diags_))
                return fail(blend, "'blend' is {\"param\": name, \"points\": [{\"clip\", \"at\"}]}");
            if (!param_index(blend.get("param"), d, s.blend_param)) return false;
            const JsonValue pts = blend.get("points");
            if (!pts.is_array() || pts.size() < 2) return fail(pts, "'blend.points' needs at least two {\"clip\", \"at\"}");
            for (u32 i = 0; i < pts.size(); ++i) {
                const JsonValue pt = pts.at(i);
                AnimGraphDocPoint bp;
                StringView c;
                if (!pt.is_object() || !detail::check_keys(pt, {"clip", "at"}, file_, kStage, diags_) ||
                    !name_of(pt.get("clip"), kClipCap, "a blend point's 'clip'", c))
                    return false;
                if (!pt.get("at").is_number()) return fail(pt.get("at"), "'at' must be a number");
                bp.at = static_cast<f32>(pt.get("at").as_number());
                if (i > 0 && !(bp.at > s.points.back().at)) return fail(pt.get("at"), "blend points must be in increasing 'at' order");
                bp.clip.assign(c);
                s.points.push_back(bp);
            }
        }
        detail::read_bool(st, "loop", true, s.loop, file_, kStage, diags_);
        f64 speed = 1.0;
        detail::read_number(st, "speed", 1.0, speed, file_, kStage, diags_);
        if (!(speed > 0.0) || speed > 100.0) return fail(st.get("speed"), "'speed' must be above 0 and at most 100");
        s.speed = static_cast<f32>(speed);
        if (const JsonValue ev = st.get("events"); ev.valid()) {
            if (!ev.is_array()) return fail(ev, "'events' must be an array of {\"time\", \"name\"}");
            for (u32 i = 0; i < ev.size(); ++i) {
                const JsonValue e = ev.at(i);
                AnimGraphDocEvent out_ev;
                StringView en;
                if (!e.is_object() || !detail::check_keys(e, {"time", "name"}, file_, kStage, diags_) ||
                    !name_of(e.get("name"), kEventCap, "an event's 'name'", en))
                    return false;
                if (!e.get("time").is_number() || e.get("time").as_number() < 0.0)
                    return fail(e.get("time"), "an event's 'time' must be seconds >= 0 into the clip");
                out_ev.time = static_cast<f32>(e.get("time").as_number());
                out_ev.name.assign(en);
                s.events.push_back(out_ev);
            }
        }
        if (const JsonValue pos = st.get("pos"); pos.valid()) {
            if (!pos.is_array() || pos.size() != 2 || !pos.at(0).is_number() || !pos.at(1).is_number())
                return fail(pos, "'pos' must be [x, y] (where the editor draws the state)");
            s.pos[0] = static_cast<f32>(pos.at(0).as_number());
            s.pos[1] = static_cast<f32>(pos.at(1).as_number());
            s.has_pos = true;
        }
        return true;
    }

    bool read_transition(JsonValue tr, const AnimGraphDocument& d, AnimGraphDocTransition& t) {
        if (!tr.is_object() || !detail::check_keys(tr, {"from", "to", "when", "fade", "on_finish"}, file_, kStage, diags_))
            return fail(tr, "a transition is {\"from\", \"to\", \"when\", \"fade\", \"on_finish\"}");
        if (!state_index(tr.get("from"), d, true, t.from) || !state_index(tr.get("to"), d, false, t.to)) return false;
        f64 fade = 0.2;
        detail::read_number(tr, "fade", 0.2, fade, file_, kStage, diags_);
        if (fade < 0.0 || fade > 10.0) return fail(tr.get("fade"), "'fade' must be 0 to 10 seconds");
        t.fade = static_cast<f32>(fade);
        detail::read_bool(tr, "on_finish", false, t.on_finish, file_, kStage, diags_);
        if (const JsonValue when = tr.get("when"); when.valid()) {
            if (!when.is_array()) return fail(when, "'when' must be an array of {\"param\", \"op\", \"value\"}");
            for (u32 i = 0; i < when.size(); ++i) {
                const JsonValue c = when.at(i);
                AnimGraphDocCondition cond;
                if (!c.is_object() || !detail::check_keys(c, {"param", "op", "value"}, file_, kStage, diags_))
                    return fail(c, "a condition is {\"param\", \"op\", \"value\"}");
                if (!param_index(c.get("param"), d, cond.param)) return false;
                const JsonValue op = c.get("op");
                const StringView o = op.is_string() ? op.as_string() : StringView();
                bool found = false;
                for (u8 k = 0; k <= static_cast<u8>(asset::AnimCompare::NotEqual) && !found; ++k)
                    if (o == anim_compare_name(static_cast<asset::AnimCompare>(k))) {
                        cond.op = static_cast<asset::AnimCompare>(k);
                        found = true;
                    }
                if (!found) return fail(op, "'op' must be one of > < >= <= == !=");
                if (!c.get("value").is_number()) return fail(c.get("value"), "'value' must be a number");
                cond.value = static_cast<f32>(c.get("value").as_number());
                t.when.push_back(cond);
            }
        }
        if (t.when.empty() && !t.on_finish) return fail(tr, "a transition needs 'when' conditions or 'on_finish': true");
        return true;
    }

    StringView file_;
    DiagList& diags_;
    JsonValue doc_;
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

void field(String& out, const char* key) {
    out += ", ";
    append_json_string(out, key);
    out += ": ";
}

void write_state(const AnimGraphDocument& d, const AnimGraphDocState& s, String& out) {
    out += "{ \"name\": ";
    append_json_string(out, s.name.view());
    if (!s.is_blend()) {
        field(out, "clip");
        append_json_string(out, s.clip.view());
    } else {
        field(out, "blend");
        out += "{ \"param\": ";
        append_json_string(out, s.blend_param < d.params.size() ? d.params[s.blend_param].view() : StringView());
        out += ", \"points\": [";
        for (usize i = 0; i < s.points.size(); ++i) {
            out += i ? ", { \"clip\": " : " { \"clip\": ";
            append_json_string(out, s.points[i].clip.view());
            out += ", \"at\": ";
            append_f32(out, s.points[i].at);
            out += " }";
        }
        out += " ] }";
    }
    if (!s.loop) out += ", \"loop\": false";
    if (s.speed != 1.0f) {
        field(out, "speed");
        append_f32(out, s.speed);
    }
    if (!s.events.empty()) {
        field(out, "events");
        out += "[";
        for (usize i = 0; i < s.events.size(); ++i) {
            out += i ? ", { \"time\": " : " { \"time\": ";
            append_f32(out, s.events[i].time);
            out += ", \"name\": ";
            append_json_string(out, s.events[i].name.view());
            out += " }";
        }
        out += " ]";
    }
    if (s.has_pos) {
        field(out, "pos");
        out += "[";
        append_f32(out, s.pos[0]);
        out += ", ";
        append_f32(out, s.pos[1]);
        out += "]";
    }
    out += " }";
}

void write_transition(const AnimGraphDocument& d, const AnimGraphDocTransition& t, String& out) {
    out += "{ \"from\": ";
    append_json_string(out, t.from == kAnimGraphAny ? StringView("*") : t.from < d.states.size() ? d.states[t.from].name.view() : StringView());
    out += ", \"to\": ";
    append_json_string(out, t.to < d.states.size() ? d.states[t.to].name.view() : StringView());
    if (!t.when.empty()) {
        field(out, "when");
        out += "[";
        for (usize i = 0; i < t.when.size(); ++i) {
            const AnimGraphDocCondition& c = t.when[i];
            out += i ? ", { \"param\": " : " { \"param\": ";
            append_json_string(out, c.param < d.params.size() ? d.params[c.param].view() : StringView());
            out += ", \"op\": ";
            append_json_string(out, anim_compare_name(c.op));
            out += ", \"value\": ";
            append_f32(out, c.value);
            out += " }";
        }
        out += " ]";
    }
    if (t.fade != 0.2f) {
        field(out, "fade");
        append_f32(out, t.fade);
    }
    if (t.on_finish) out += ", \"on_finish\": true";
    out += " }";
}

class AnimGraphImporter final : public IImporter {
public:
    StringView id() const override { return "animgraph"; }
    u32 version() const override { return 1; }
    ChunkType type() const override { return ChunkType::AnimGraph; }
    bool native() const override { return true; }
    bool handles_extension(StringView ext) const override { return ext == ".mcanim"; }

    void scan_dependencies(const ImportInput&, Array<DepRef>&, DiagList&) const override {}

    bool cook(const ImportInput& in, asset::ContainerWriter& out, DiagList& diags) const override {
        AnimGraphDocument d;
        if (!read_animgraph_document(in.settings, in.display_path, d, diags)) return false;
        Array<byte> payload;
        build_animgraph_chunk(d, payload);
        asset::ContainerWriter::ChunkDesc desc;
        desc.type = ChunkType::AnimGraph;
        desc.version = asset::chunk_version(asset::ANIMGRAPH_VERSION, 0);
        desc.guid = in.guid;
        out.add(desc, payload.span());
        return true;
    }
};

}  // namespace

const char* anim_compare_name(asset::AnimCompare op) {
    switch (op) {
        case asset::AnimCompare::Greater: return ">";
        case asset::AnimCompare::Less: return "<";
        case asset::AnimCompare::GreaterEqual: return ">=";
        case asset::AnimCompare::LessEqual: return "<=";
        case asset::AnimCompare::Equal: return "==";
        case asset::AnimCompare::NotEqual: return "!=";
    }
    return "?";
}

bool read_animgraph_document(JsonValue doc, StringView file, AnimGraphDocument& out, DiagList& diags) {
    const u32 errors_before = diags.error_count();
    out = AnimGraphDocument();
    Reader r(file, diags);
    return r.read(doc, out) && diags.error_count() == errors_before;
}

void write_animgraph_document(const AnimGraphDocument& d, asset::Guid guid, String& out) {
    out += "{\n  \"format\": \"mce-animgraph\", \"version\": ";
    appendf(out, "%u", kAnimGraphFormatVersion);
    out += ", \"guid\": ";
    append_json_string(out, asset::to_string(guid).view());
    if (!d.name.empty()) {
        field(out, "name");
        append_json_string(out, d.name.view());
    }
    if (!d.params.empty()) {
        out += ",\n  \"params\": [";
        for (usize i = 0; i < d.params.size(); ++i) {
            if (i) out += ", ";
            append_json_string(out, d.params[i].view());
        }
        out += "]";
    }
    if (d.start != 0 && d.start < d.states.size()) {
        out += ",\n  \"start\": ";
        append_json_string(out, d.states[d.start].name.view());
    }
    out += ",\n  \"states\": [";
    for (usize i = 0; i < d.states.size(); ++i) {
        out += i ? ",\n    " : "\n    ";
        write_state(d, d.states[i], out);
    }
    out += d.states.empty() ? "]" : "\n  ]";
    if (!d.transitions.empty()) {
        out += ",\n  \"transitions\": [";
        for (usize i = 0; i < d.transitions.size(); ++i) {
            out += i ? ",\n    " : "\n    ";
            write_transition(d, d.transitions[i], out);
        }
        out += "\n  ]";
    }
    out += "\n}\n";
}

void build_animgraph_chunk(const AnimGraphDocument& d, Array<byte>& payload) {
    asset::AnimGraphHeader h{};
    h.param_count = static_cast<u32>(d.params.size());
    for (u32 i = 0; i < d.params.size() && i < asset::kAnimGraphMaxParams; ++i) copy_name(h.params[i], kParamCap, d.params[i].view());
    h.start_state = d.start;

    Array<asset::AnimGraphState> states;
    Array<asset::AnimGraphBlendPoint> points;
    Array<asset::AnimGraphEvent> events;
    for (const AnimGraphDocState& ds : d.states) {
        asset::AnimGraphState s{};
        copy_name(s.name, kStateCap, ds.name.view());
        if (!ds.is_blend()) {
            copy_name(s.clip, kClipCap, ds.clip.view());
        } else {
            s.blend_param = ds.blend_param;
            s.first_blend = static_cast<u32>(points.size());
            for (const AnimGraphDocPoint& p : ds.points) {
                asset::AnimGraphBlendPoint bp{};
                copy_name(bp.clip, kClipCap, p.clip.view());
                bp.at = p.at;
                points.push_back(bp);
            }
            s.blend_count = static_cast<u32>(ds.points.size());
        }
        s.loop = ds.loop ? 1 : 0;
        s.speed = ds.speed;
        s.first_event = static_cast<u32>(events.size());
        for (const AnimGraphDocEvent& e : ds.events) {
            asset::AnimGraphEvent ev{};
            ev.time = e.time;
            ev.id = fnv1a64(e.name.view());
            copy_name(ev.name, kEventCap, e.name.view());
            events.push_back(ev);
        }
        s.event_count = static_cast<u32>(ds.events.size());
        states.push_back(s);
    }
    Array<asset::AnimGraphTransition> transitions;
    Array<asset::AnimGraphCondition> conditions;
    for (const AnimGraphDocTransition& dt : d.transitions) {
        asset::AnimGraphTransition t{};
        t.from = dt.from;
        t.to = dt.to;
        t.fade = dt.fade;
        t.on_finish = dt.on_finish ? 1 : 0;
        t.first_condition = static_cast<u32>(conditions.size());
        for (const AnimGraphDocCondition& c : dt.when) {
            asset::AnimGraphCondition cond{};
            cond.param = c.param;
            cond.op = c.op;
            cond.value = c.value;
            conditions.push_back(cond);
        }
        t.condition_count = static_cast<u32>(dt.when.size());
        transitions.push_back(t);
    }

    h.state_count = static_cast<u32>(states.size());
    h.transition_count = static_cast<u32>(transitions.size());
    h.condition_count = static_cast<u32>(conditions.size());
    h.blend_point_count = static_cast<u32>(points.size());
    h.event_count = static_cast<u32>(events.size());
    payload.clear();
    auto append = [&](const void* p, usize n) {
        const usize at = payload.size();
        payload.resize(at + n, byte{0});
        if (n) std::memcpy(payload.data() + at, p, n);
    };
    append(&h, sizeof(h));
    append(states.data(), states.size() * sizeof(asset::AnimGraphState));
    append(transitions.data(), transitions.size() * sizeof(asset::AnimGraphTransition));
    append(conditions.data(), conditions.size() * sizeof(asset::AnimGraphCondition));
    append(points.data(), points.size() * sizeof(asset::AnimGraphBlendPoint));
    append(events.data(), events.size() * sizeof(asset::AnimGraphEvent));
}

mem::UniquePtr<IImporter> make_animgraph_importer() { return mem::make_unique<AnimGraphImporter>(mem::heap()); }

}  // namespace mce::cook
