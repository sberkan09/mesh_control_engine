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
//       "events": [ { "time": 0.25, "name": "footstep" } ] },
//     { "name": "slash", "clip": "Slash", "loop": false, "speed": 1.2, "events": [ { "time": 0.3, "name": "hit" } ] } ],
//   "transitions": [
//     { "from": "*", "to": "slash", "when": [ { "param": "attack", "op": ">", "value": 0.5 } ], "fade": 0.1 },
//     { "from": "slash", "to": "locomotion", "on_finish": true, "fade": 0.2 } ]
// }

#include <cstring>

#include "importer_common.h"
#include "mce/asset/formats.h"
#include "mce/hash.h"

namespace mce::cook {

namespace {

constexpr const char* kStage = "cook.animgraph";
constexpr u32 kGraphFormatVersion = 1;

void copy_name(char* dst, usize cap, StringView s) {
    std::memset(dst, 0, cap);
    std::memcpy(dst, s.data(), s.size() < cap ? s.size() : cap - 1);
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
        const JsonValue doc = in.settings;
        const StringView file = in.display_path;
        auto fail = [&](JsonValue at, StringView msg) {
            detail::error_at(diags, Errc::ParseError, file, at.valid() ? at : doc, kStage, msg);
            return false;
        };
        auto name_of = [&](JsonValue v, usize cap, const char* what, StringView& out_name) {
            if (!v.is_string() || v.as_string().empty() || v.as_string().size() >= cap) {
                String msg;
                appendf(msg, "%s must be a string of 1 to %zu characters", what, cap - 1);
                return fail(v, msg.view());
            }
            out_name = v.as_string();
            return true;
        };
        if (!detail::check_keys(doc, {"format", "version", "guid", "name", "params", "start", "states", "transitions"}, file, kStage, diags))
            return false;
        const JsonValue fmt = doc.get("format");
        if (!fmt.is_string() || fmt.as_string() != "mce-animgraph") return fail(fmt, "\"format\" must be \"mce-animgraph\"");
        const JsonValue ver = doc.get("version");
        if (!ver.is_integer() || ver.as_number() != kGraphFormatVersion) {
            String msg;
            appendf(msg, "unsupported animation graph version (this cook reads version %u)", kGraphFormatVersion);
            detail::error_at(diags, Errc::VersionTooNew, file, ver.valid() ? ver : doc, kStage, msg);
            return false;
        }

        asset::AnimGraphHeader h{};
        const JsonValue params = doc.get("params");
        if (params.valid()) {
            if (!params.is_array() || params.size() > asset::kAnimGraphMaxParams) return fail(params, "'params' must be an array of up to 4 names");
            for (u32 i = 0; i < params.size(); ++i) {
                StringView n;
                if (!name_of(params.at(i), sizeof(h.params[0]), "a parameter name", n)) return false;
                for (u32 j = 0; j < i; ++j)
                    if (asset::AnimGraphView::text(h.params[j], sizeof(h.params[j])) == n) return fail(params.at(i), "two parameters share this name");
                copy_name(h.params[i], sizeof(h.params[i]), n);
            }
            h.param_count = params.size();
        }
        auto param_index = [&](JsonValue v, u8& out_index) {
            if (v.is_string())
                for (u32 i = 0; i < h.param_count; ++i)
                    if (asset::AnimGraphView::text(h.params[i], sizeof(h.params[i])) == v.as_string()) {
                        out_index = static_cast<u8>(i);
                        return true;
                    }
            return fail(v, "'param' must name one of 'params'");
        };

        // States.
        Array<asset::AnimGraphState> states;
        Array<asset::AnimGraphBlendPoint> points;
        Array<asset::AnimGraphEvent> events;
        const JsonValue state_list = doc.get("states");
        if (!state_list.is_array() || state_list.size() == 0) return fail(state_list, "'states' must be a non-empty array");
        for (u32 si = 0; si < state_list.size(); ++si) {
            const JsonValue st = state_list.at(si);
            if (!st.is_object() || !detail::check_keys(st, {"name", "clip", "blend", "loop", "speed", "events"}, file, kStage, diags))
                return fail(st, "a state is {\"name\", \"clip\" | \"blend\", \"loop\", \"speed\", \"events\"}");
            asset::AnimGraphState s{};
            StringView n;
            if (!name_of(st.get("name"), sizeof(s.name), "a state's 'name'", n)) return false;
            for (const asset::AnimGraphState& o : states)
                if (asset::AnimGraphView::text(o.name, sizeof(o.name)) == n) return fail(st.get("name"), "two states share this name");
            copy_name(s.name, sizeof(s.name), n);
            const JsonValue clip = st.get("clip"), blend = st.get("blend");
            if (clip.valid() == blend.valid()) return fail(st, "a state has exactly one of 'clip' and 'blend'");
            if (clip.valid()) {
                StringView c;
                if (!name_of(clip, sizeof(s.clip), "'clip'", c)) return false;
                copy_name(s.clip, sizeof(s.clip), c);
            } else {
                if (!blend.is_object() || !detail::check_keys(blend, {"param", "points"}, file, kStage, diags))
                    return fail(blend, "'blend' is {\"param\": name, \"points\": [{\"clip\", \"at\"}]}");
                if (!param_index(blend.get("param"), s.blend_param)) return false;
                const JsonValue pts = blend.get("points");
                if (!pts.is_array() || pts.size() < 2) return fail(pts, "'blend.points' needs at least two {\"clip\", \"at\"}");
                s.first_blend = static_cast<u32>(points.size());
                for (u32 i = 0; i < pts.size(); ++i) {
                    const JsonValue pt = pts.at(i);
                    asset::AnimGraphBlendPoint bp{};
                    StringView c;
                    if (!pt.is_object() || !detail::check_keys(pt, {"clip", "at"}, file, kStage, diags) ||
                        !name_of(pt.get("clip"), sizeof(bp.clip), "a blend point's 'clip'", c))
                        return false;
                    if (!pt.get("at").is_number()) return fail(pt.get("at"), "'at' must be a number");
                    bp.at = static_cast<f32>(pt.get("at").as_number());
                    if (i > 0 && !(bp.at > points.back().at)) return fail(pt.get("at"), "blend points must be in increasing 'at' order");
                    copy_name(bp.clip, sizeof(bp.clip), c);
                    points.push_back(bp);
                }
                s.blend_count = pts.size();
            }
            bool loop = true;
            detail::read_bool(st, "loop", true, loop, file, kStage, diags);
            s.loop = loop ? 1 : 0;
            f64 speed = 1.0;
            detail::read_number(st, "speed", 1.0, speed, file, kStage, diags);
            if (!(speed > 0.0) || speed > 100.0) return fail(st.get("speed"), "'speed' must be above 0 and at most 100");
            s.speed = static_cast<f32>(speed);
            s.first_event = static_cast<u32>(events.size());
            if (const JsonValue ev = st.get("events"); ev.valid()) {
                if (!ev.is_array()) return fail(ev, "'events' must be an array of {\"time\", \"name\"}");
                for (u32 i = 0; i < ev.size(); ++i) {
                    const JsonValue e = ev.at(i);
                    asset::AnimGraphEvent out_ev{};
                    StringView en;
                    if (!e.is_object() || !detail::check_keys(e, {"time", "name"}, file, kStage, diags) ||
                        !name_of(e.get("name"), sizeof(out_ev.name), "an event's 'name'", en))
                        return false;
                    if (!e.get("time").is_number() || e.get("time").as_number() < 0.0)
                        return fail(e.get("time"), "an event's 'time' must be seconds >= 0 into the clip");
                    out_ev.time = static_cast<f32>(e.get("time").as_number());
                    out_ev.id = fnv1a64(en);
                    copy_name(out_ev.name, sizeof(out_ev.name), en);
                    events.push_back(out_ev);
                }
                s.event_count = ev.size();
            }
            states.push_back(s);
        }
        auto state_index = [&](JsonValue v, bool any_ok, u32& out_index) {
            if (v.is_string()) {
                if (any_ok && v.as_string() == "*") {
                    out_index = asset::kAnimAnyState;
                    return true;
                }
                for (u32 i = 0; i < states.size(); ++i)
                    if (asset::AnimGraphView::text(states[i].name, sizeof(states[i].name)) == v.as_string()) {
                        out_index = i;
                        return true;
                    }
            }
            return fail(v, any_ok ? "must name a state, or \"*\" for any" : "must name a state");
        };
        if (const JsonValue start = doc.get("start"); start.valid()) {
            if (!state_index(start, false, h.start_state)) return false;
        }

        // Transitions.
        Array<asset::AnimGraphTransition> transitions;
        Array<asset::AnimGraphCondition> conditions;
        if (const JsonValue list = doc.get("transitions"); list.valid()) {
            if (!list.is_array()) return fail(list, "'transitions' must be an array");
            for (u32 ti = 0; ti < list.size(); ++ti) {
                const JsonValue tr = list.at(ti);
                if (!tr.is_object() || !detail::check_keys(tr, {"from", "to", "when", "fade", "on_finish"}, file, kStage, diags))
                    return fail(tr, "a transition is {\"from\", \"to\", \"when\", \"fade\", \"on_finish\"}");
                asset::AnimGraphTransition t{};
                if (!state_index(tr.get("from"), true, t.from) || !state_index(tr.get("to"), false, t.to)) return false;
                f64 fade = 0.2;
                detail::read_number(tr, "fade", 0.2, fade, file, kStage, diags);
                if (fade < 0.0 || fade > 10.0) return fail(tr.get("fade"), "'fade' must be 0 to 10 seconds");
                t.fade = static_cast<f32>(fade);
                bool on_finish = false;
                detail::read_bool(tr, "on_finish", false, on_finish, file, kStage, diags);
                t.on_finish = on_finish ? 1 : 0;
                t.first_condition = static_cast<u32>(conditions.size());
                if (const JsonValue when = tr.get("when"); when.valid()) {
                    if (!when.is_array()) return fail(when, "'when' must be an array of {\"param\", \"op\", \"value\"}");
                    for (u32 i = 0; i < when.size(); ++i) {
                        const JsonValue c = when.at(i);
                        asset::AnimGraphCondition cond{};
                        if (!c.is_object() || !detail::check_keys(c, {"param", "op", "value"}, file, kStage, diags))
                            return fail(c, "a condition is {\"param\", \"op\", \"value\"}");
                        if (!param_index(c.get("param"), cond.param)) return false;
                        const JsonValue op = c.get("op");
                        const StringView o = op.is_string() ? op.as_string() : StringView();
                        if (o == ">") cond.op = asset::AnimCompare::Greater;
                        else if (o == "<") cond.op = asset::AnimCompare::Less;
                        else if (o == ">=") cond.op = asset::AnimCompare::GreaterEqual;
                        else if (o == "<=") cond.op = asset::AnimCompare::LessEqual;
                        else if (o == "==") cond.op = asset::AnimCompare::Equal;
                        else if (o == "!=") cond.op = asset::AnimCompare::NotEqual;
                        else return fail(op, "'op' must be one of > < >= <= == !=");
                        if (!c.get("value").is_number()) return fail(c.get("value"), "'value' must be a number");
                        cond.value = static_cast<f32>(c.get("value").as_number());
                        conditions.push_back(cond);
                    }
                }
                t.condition_count = static_cast<u32>(conditions.size()) - t.first_condition;
                if (t.condition_count == 0 && !t.on_finish) return fail(tr, "a transition needs 'when' conditions or 'on_finish': true");
                transitions.push_back(t);
            }
        }
        if (diags.error_count() > 0) return false;

        h.state_count = static_cast<u32>(states.size());
        h.transition_count = static_cast<u32>(transitions.size());
        h.condition_count = static_cast<u32>(conditions.size());
        h.blend_point_count = static_cast<u32>(points.size());
        h.event_count = static_cast<u32>(events.size());
        Array<byte> payload;
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

        asset::ContainerWriter::ChunkDesc d;
        d.type = ChunkType::AnimGraph;
        d.version = asset::chunk_version(asset::ANIMGRAPH_VERSION, 0);
        d.guid = in.guid;
        out.add(d, payload.span());
        return true;
    }
};

}  // namespace

mem::UniquePtr<IImporter> make_animgraph_importer() { return mem::make_unique<AnimGraphImporter>(mem::heap()); }

}  // namespace mce::cook
