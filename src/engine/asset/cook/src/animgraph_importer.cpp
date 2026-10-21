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
//
// Version 2 (M-GAMEPLAY3D step 5) adds, all optional:
//   a 2D blend state: { "name": "strafe", "blend2d": { "x": "move_x", "y": "move_y",
//                        "points": [ { "clip": "Idle", "at": [0, 0] }, { "clip": "Fwd", "at": [0, 1] }, ... ] } }
//   "root_motion": "hips",                        // the bone whose ground motion moves the entity
//   "layers": [ { "name": "aim", "clip": "Aim", "mask": ["spine"], "weight": 1, "param": "aiming",
//                 "additive": false, "loop": true, "speed": 1 } ],       // up to 4, applied in order
//   "ik": [ { "name": "hand", "type": "two_bone", "bone": "hand_r", "pole": [0, 0, -1] },
//           { "name": "head", "type": "look_at", "bone": "head", "axis": [0, 0, 1], "limit": 70 },
//           { "name": "foot_l", "type": "foot", "bone": "foot_l", "pelvis": "hips", "reach": 0.4 } ]  // up to 8
// Each IK chain also takes "weight" and "param" (a parameter its weight is multiplied by).

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
constexpr usize kLayerCap = sizeof(asset::AnimGraphLayer::name);
constexpr usize kBoneCap = sizeof(asset::AnimGraphMaskBone::bone);
constexpr usize kIKCap = sizeof(asset::AnimGraphIK::name);

class Reader {
public:
    Reader(StringView file, DiagList& diags) : file_(file), diags_(diags) {}

    bool read(JsonValue doc, AnimGraphDocument& d) {
        doc_ = doc;
        if (!detail::check_keys(doc, {"format", "version", "guid", "name", "params", "start", "states", "transitions", "root_motion", "layers", "ik"}, file_, kStage, diags_))
            return false;
        const JsonValue fmt = doc.get("format");
        if (!fmt.is_string() || fmt.as_string() != "mce-animgraph") return fail(fmt, "\"format\" must be \"mce-animgraph\"");
        const JsonValue ver = doc.get("version");
        if (!ver.is_integer() || ver.as_number() < 1 || ver.as_number() > kAnimGraphFormatVersion) {
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

        if (const JsonValue root = doc.get("root_motion"); root.valid()) {
            StringView b;
            if (!name_of(root, kBoneCap, "'root_motion' (a bone name)", b)) return false;
            d.root_motion.assign(b);
        }
        if (const JsonValue list = doc.get("layers"); list.valid()) {
            if (!list.is_array() || list.size() > asset::kAnimGraphMaxLayers) return fail(list, "'layers' must be an array of up to 4 layers");
            for (u32 i = 0; i < list.size(); ++i) {
                AnimGraphDocLayer l;
                if (!read_layer(list.at(i), d, l)) return false;
                d.layers.push_back(l);
            }
        }
        if (const JsonValue list = doc.get("ik"); list.valid()) {
            if (!list.is_array() || list.size() > asset::kAnimGraphMaxIK) return fail(list, "'ik' must be an array of up to 8 chains");
            for (u32 i = 0; i < list.size(); ++i) {
                AnimGraphDocIK k;
                if (!read_ik(list.at(i), d, k)) return false;
                d.ik.push_back(k);
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

    bool read_vec3(JsonValue v, const char* what, f32 (&out)[3]) {
        if (!v.is_array() || v.size() != 3 || !v.at(0).is_number() || !v.at(1).is_number() || !v.at(2).is_number()) {
            String msg;
            appendf(msg, "'%s' must be [x, y, z]", what);
            return fail(v, msg.view());
        }
        for (u32 i = 0; i < 3; ++i) out[i] = static_cast<f32>(v.at(i).as_number());
        return true;
    }

    bool weight_of(JsonValue obj, const AnimGraphDocument& d, f32& weight, u8& param) {
        f64 w = 1.0;
        detail::read_number(obj, "weight", 1.0, w, file_, kStage, diags_);
        if (w < 0.0 || w > 1.0) return fail(obj.get("weight"), "'weight' must be 0 to 1");
        weight = static_cast<f32>(w);
        if (const JsonValue p = obj.get("param"); p.valid()) return param_index(p, d, param);
        return true;
    }

    bool read_layer(JsonValue lv, const AnimGraphDocument& d, AnimGraphDocLayer& l) {
        if (!lv.is_object() ||
            !detail::check_keys(lv, {"name", "clip", "mask", "weight", "param", "additive", "loop", "speed"}, file_, kStage, diags_))
            return fail(lv, "a layer is {\"name\", \"clip\", \"mask\", \"weight\", \"param\", \"additive\", \"loop\", \"speed\"}");
        StringView n, c;
        if (!name_of(lv.get("name"), kLayerCap, "a layer's 'name'", n)) return false;
        for (const AnimGraphDocLayer& o : d.layers)
            if (o.name.view() == n) return fail(lv.get("name"), "two layers share this name");
        if (!name_of(lv.get("clip"), kClipCap, "a layer's 'clip'", c)) return false;
        l.name.assign(n);
        l.clip.assign(c);
        if (const JsonValue mask = lv.get("mask"); mask.valid()) {
            if (!mask.is_array()) return fail(mask, "'mask' must be an array of bone names");
            for (u32 i = 0; i < mask.size(); ++i) {
                StringView b;
                if (!name_of(mask.at(i), kBoneCap, "a mask bone", b)) return false;
                l.mask.push_back(String(b));
            }
        }
        if (!weight_of(lv, d, l.weight, l.weight_param)) return false;
        detail::read_bool(lv, "additive", false, l.additive, file_, kStage, diags_);
        detail::read_bool(lv, "loop", true, l.loop, file_, kStage, diags_);
        f64 speed = 1.0;
        detail::read_number(lv, "speed", 1.0, speed, file_, kStage, diags_);
        if (!(speed > 0.0) || speed > 100.0) return fail(lv.get("speed"), "'speed' must be above 0 and at most 100");
        l.speed = static_cast<f32>(speed);
        return true;
    }

    bool read_ik(JsonValue kv, const AnimGraphDocument& d, AnimGraphDocIK& k) {
        if (!kv.is_object() ||
            !detail::check_keys(kv, {"name", "type", "bone", "pelvis", "weight", "param", "pole", "axis", "limit", "reach"}, file_, kStage, diags_))
            return fail(kv, "an IK chain is {\"name\", \"type\", \"bone\", ...}");
        StringView n, b;
        if (!name_of(kv.get("name"), kIKCap, "an IK chain's 'name'", n)) return false;
        for (const AnimGraphDocIK& o : d.ik)
            if (o.name.view() == n) return fail(kv.get("name"), "two IK chains share this name");
        k.name.assign(n);
        const JsonValue type = kv.get("type");
        const StringView t = type.is_string() ? type.as_string() : StringView();
        bool found = false;
        for (u8 i = 0; i <= static_cast<u8>(asset::AnimIKKind::Foot) && !found; ++i)
            if (t == anim_ik_kind_name(static_cast<asset::AnimIKKind>(i))) {
                k.kind = static_cast<asset::AnimIKKind>(i);
                found = true;
            }
        if (!found) return fail(type.valid() ? type : kv, "'type' must be \"two_bone\", \"look_at\" or \"foot\"");
        if (!name_of(kv.get("bone"), kBoneCap, "an IK chain's 'bone'", b)) return false;
        k.bone.assign(b);
        if (!weight_of(kv, d, k.weight, k.weight_param)) return false;
        const bool look = k.kind == asset::AnimIKKind::LookAt, foot = k.kind == asset::AnimIKKind::Foot;
        if (kv.get("pelvis").valid() && !foot) return fail(kv.get("pelvis"), "'pelvis' is for \"foot\" chains");
        if (kv.get("reach").valid() && !foot) return fail(kv.get("reach"), "'reach' is for \"foot\" chains");
        if ((kv.get("axis").valid() || kv.get("limit").valid()) && !look)
            return fail(kv.get("axis").valid() ? kv.get("axis") : kv.get("limit"), "'axis' and 'limit' are for \"look_at\" chains");
        if (kv.get("pole").valid() && look) return fail(kv.get("pole"), "'pole' is for \"two_bone\" and \"foot\" chains");
        if (const JsonValue p = kv.get("pelvis"); p.valid()) {
            StringView pb;
            if (!name_of(p, kBoneCap, "'pelvis'", pb)) return false;
            k.pelvis.assign(pb);
        }
        if (look) {
            k.axis[2] = 1.0f;
            if (const JsonValue a = kv.get("axis"); a.valid() && !read_vec3(a, "axis", k.axis)) return false;
            if (k.axis[0] == 0.0f && k.axis[1] == 0.0f && k.axis[2] == 0.0f) return fail(kv.get("axis"), "'axis' must not be zero");
            f64 limit = 60.0;
            detail::read_number(kv, "limit", 60.0, limit, file_, kStage, diags_);
            if (!(limit > 0.0) || limit > 180.0) return fail(kv.get("limit"), "'limit' must be above 0 and at most 180 degrees");
            k.limit_deg = static_cast<f32>(limit);
        } else if (const JsonValue p = kv.get("pole"); p.valid() && !read_vec3(p, "pole", k.axis)) {
            return false;
        }
        if (foot) {
            f64 reach = 0.5;
            detail::read_number(kv, "reach", 0.5, reach, file_, kStage, diags_);
            if (!(reach > 0.0) || reach > 10.0) return fail(kv.get("reach"), "'reach' must be above 0 and at most 10 metres");
            k.reach = static_cast<f32>(reach);
        }
        return true;
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
        if (!st.is_object() ||
            !detail::check_keys(st, {"name", "clip", "blend", "blend2d", "loop", "speed", "events", "pos"}, file_, kStage, diags_))
            return fail(st, "a state is {\"name\", \"clip\" | \"blend\" | \"blend2d\", \"loop\", \"speed\", \"events\"}");
        StringView n;
        if (!name_of(st.get("name"), kStateCap, "a state's 'name'", n)) return false;
        for (const AnimGraphDocState& o : d.states)
            if (o.name.view() == n) return fail(st.get("name"), "two states share this name");
        s.name.assign(n);
        const JsonValue clip = st.get("clip"), blend = st.get("blend"), blend2d = st.get("blend2d");
        if (int(clip.valid()) + int(blend.valid()) + int(blend2d.valid()) != 1)
            return fail(st, "a state has exactly one of 'clip', 'blend' and 'blend2d'");
        if (clip.valid()) {
            StringView c;
            if (!name_of(clip, kClipCap, "'clip'", c)) return false;
            s.clip.assign(c);
        } else if (blend2d.valid()) {
            if (!blend2d.is_object() || !detail::check_keys(blend2d, {"x", "y", "points"}, file_, kStage, diags_))
                return fail(blend2d, "'blend2d' is {\"x\": name, \"y\": name, \"points\": [{\"clip\", \"at\": [x, y]}]}");
            s.blend_2d = true;
            if (!param_index(blend2d.get("x"), d, s.blend_param) || !param_index(blend2d.get("y"), d, s.blend_param_y)) return false;
            if (s.blend_param == s.blend_param_y) return fail(blend2d.get("y"), "'x' and 'y' must be two different parameters");
            const JsonValue pts = blend2d.get("points");
            if (!pts.is_array() || pts.size() < 3 || pts.size() > asset::kAnimGraphMaxBlend2D)
                return fail(pts, "'blend2d.points' needs 3 to 8 {\"clip\", \"at\": [x, y]}");
            for (u32 i = 0; i < pts.size(); ++i) {
                const JsonValue pt = pts.at(i);
                AnimGraphDocPoint2D bp;
                StringView c;
                if (!pt.is_object() || !detail::check_keys(pt, {"clip", "at"}, file_, kStage, diags_) ||
                    !name_of(pt.get("clip"), kClipCap, "a blend point's 'clip'", c))
                    return false;
                const JsonValue at = pt.get("at");
                if (!at.is_array() || at.size() != 2 || !at.at(0).is_number() || !at.at(1).is_number())
                    return fail(at, "a 2D blend point's 'at' must be [x, y]");
                bp.clip.assign(c);
                bp.x = static_cast<f32>(at.at(0).as_number());
                bp.y = static_cast<f32>(at.at(1).as_number());
                for (const AnimGraphDocPoint2D& o : s.points2d)
                    if (o.x == bp.x && o.y == bp.y) return fail(at, "two 2D blend points share this position");
                s.points2d.push_back(bp);
            }
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
    } else if (s.blend_2d) {
        field(out, "blend2d");
        out += "{ \"x\": ";
        append_json_string(out, s.blend_param < d.params.size() ? d.params[s.blend_param].view() : StringView());
        out += ", \"y\": ";
        append_json_string(out, s.blend_param_y < d.params.size() ? d.params[s.blend_param_y].view() : StringView());
        out += ", \"points\": [";
        for (usize i = 0; i < s.points2d.size(); ++i) {
            out += i ? ", { \"clip\": " : " { \"clip\": ";
            append_json_string(out, s.points2d[i].clip.view());
            out += ", \"at\": [";
            append_f32(out, s.points2d[i].x);
            out += ", ";
            append_f32(out, s.points2d[i].y);
            out += "] }";
        }
        out += " ] }";
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

void write_weight(const AnimGraphDocument& d, f32 weight, u8 param, String& out) {
    if (weight != 1.0f) {
        field(out, "weight");
        append_f32(out, weight);
    }
    if (param != asset::kAnimNoParam) {
        field(out, "param");
        append_json_string(out, param < d.params.size() ? d.params[param].view() : StringView());
    }
}

void write_vec3(const char* key, const f32 (&v)[3], String& out) {
    field(out, key);
    out += "[";
    for (int i = 0; i < 3; ++i) {
        if (i) out += ", ";
        append_f32(out, v[i]);
    }
    out += "]";
}

void write_layer(const AnimGraphDocument& d, const AnimGraphDocLayer& l, String& out) {
    out += "{ \"name\": ";
    append_json_string(out, l.name.view());
    field(out, "clip");
    append_json_string(out, l.clip.view());
    if (!l.mask.empty()) {
        field(out, "mask");
        out += "[";
        for (usize i = 0; i < l.mask.size(); ++i) {
            if (i) out += ", ";
            append_json_string(out, l.mask[i].view());
        }
        out += "]";
    }
    write_weight(d, l.weight, l.weight_param, out);
    if (l.additive) out += ", \"additive\": true";
    if (!l.loop) out += ", \"loop\": false";
    if (l.speed != 1.0f) {
        field(out, "speed");
        append_f32(out, l.speed);
    }
    out += " }";
}

void write_ik(const AnimGraphDocument& d, const AnimGraphDocIK& k, String& out) {
    out += "{ \"name\": ";
    append_json_string(out, k.name.view());
    field(out, "type");
    append_json_string(out, anim_ik_kind_name(k.kind));
    field(out, "bone");
    append_json_string(out, k.bone.view());
    if (!k.pelvis.empty()) {
        field(out, "pelvis");
        append_json_string(out, k.pelvis.view());
    }
    write_weight(d, k.weight, k.weight_param, out);
    if (k.kind == asset::AnimIKKind::LookAt) {
        if (k.axis[0] != 0.0f || k.axis[1] != 0.0f || k.axis[2] != 1.0f) write_vec3("axis", k.axis, out);
        if (k.limit_deg != 60.0f) {
            field(out, "limit");
            append_f32(out, k.limit_deg);
        }
    } else if (k.axis[0] != 0.0f || k.axis[1] != 0.0f || k.axis[2] != 0.0f) {
        write_vec3("pole", k.axis, out);
    }
    if (k.kind == asset::AnimIKKind::Foot && k.reach != 0.5f) {
        field(out, "reach");
        append_f32(out, k.reach);
    }
    out += " }";
}

class AnimGraphImporter final : public IImporter {
public:
    StringView id() const override { return "animgraph"; }
    u32 version() const override { return 2; }
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
        desc.version = asset::chunk_version(asset::ANIMGRAPH_VERSION, d.extended() ? 1 : 0);
        desc.guid = in.guid;
        out.add(desc, payload.span());
        return true;
    }
};

}  // namespace

bool AnimGraphDocument::extended() const {
    if (!root_motion.empty() || !layers.empty() || !ik.empty()) return true;
    for (const AnimGraphDocState& s : states)
        if (s.blend_2d) return true;
    return false;
}

const char* anim_ik_kind_name(asset::AnimIKKind k) {
    switch (k) {
        case asset::AnimIKKind::TwoBone: return "two_bone";
        case asset::AnimIKKind::LookAt: return "look_at";
        case asset::AnimIKKind::Foot: return "foot";
    }
    return "?";
}

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
    appendf(out, "%u", d.extended() ? kAnimGraphFormatVersion : 1u);
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
    if (!d.root_motion.empty()) {
        out += ",\n  \"root_motion\": ";
        append_json_string(out, d.root_motion.view());
    }
    if (!d.layers.empty()) {
        out += ",\n  \"layers\": [";
        for (usize i = 0; i < d.layers.size(); ++i) {
            out += i ? ",\n    " : "\n    ";
            write_layer(d, d.layers[i], out);
        }
        out += "\n  ]";
    }
    if (!d.ik.empty()) {
        out += ",\n  \"ik\": [";
        for (usize i = 0; i < d.ik.size(); ++i) {
            out += i ? ",\n    " : "\n    ";
            write_ik(d, d.ik[i], out);
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
    h.flags = d.extended() ? asset::kAnimGraphExtended : 0u;

    Array<asset::AnimGraphState> states;
    Array<asset::AnimGraphBlendPoint> points;
    Array<asset::AnimGraphBlendPoint2D> points2d;
    Array<asset::AnimGraphEvent> events;
    for (const AnimGraphDocState& ds : d.states) {
        asset::AnimGraphState s{};
        copy_name(s.name, kStateCap, ds.name.view());
        if (!ds.is_blend()) {
            copy_name(s.clip, kClipCap, ds.clip.view());
        } else if (ds.blend_2d) {
            s.blend_2d = 1;
            s.blend_param = ds.blend_param;
            s.blend_param_y = ds.blend_param_y;
            s.first_blend = static_cast<u32>(points2d.size());
            for (const AnimGraphDocPoint2D& p : ds.points2d) {
                asset::AnimGraphBlendPoint2D bp{};
                copy_name(bp.clip, kClipCap, p.clip.view());
                bp.x = p.x;
                bp.y = p.y;
                points2d.push_back(bp);
            }
            s.blend_count = static_cast<u32>(ds.points2d.size());
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
    if (!d.extended()) return;

    asset::AnimGraphExtension x{};
    copy_name(x.root_bone, sizeof x.root_bone, d.root_motion.view());
    Array<asset::AnimGraphLayer> layers;
    Array<asset::AnimGraphMaskBone> mask;
    for (const AnimGraphDocLayer& dl : d.layers) {
        asset::AnimGraphLayer l{};
        copy_name(l.name, kLayerCap, dl.name.view());
        copy_name(l.clip, kClipCap, dl.clip.view());
        l.first_mask = static_cast<u32>(mask.size());
        for (const String& b : dl.mask) {
            asset::AnimGraphMaskBone m{};
            copy_name(m.bone, kBoneCap, b.view());
            mask.push_back(m);
        }
        l.mask_count = static_cast<u32>(dl.mask.size());
        l.weight = dl.weight;
        l.speed = dl.speed;
        l.weight_param = dl.weight_param;
        l.additive = dl.additive ? 1 : 0;
        l.loop = dl.loop ? 1 : 0;
        layers.push_back(l);
    }
    Array<asset::AnimGraphIK> ik;
    for (const AnimGraphDocIK& dk : d.ik) {
        asset::AnimGraphIK k{};
        copy_name(k.name, kIKCap, dk.name.view());
        copy_name(k.bone, kBoneCap, dk.bone.view());
        copy_name(k.pelvis, kBoneCap, dk.pelvis.view());
        k.kind = dk.kind;
        k.weight_param = dk.weight_param;
        k.weight = dk.weight;
        for (int i = 0; i < 3; ++i) k.axis[i] = dk.axis[i];
        k.limit_deg = dk.limit_deg;
        k.reach = dk.reach;
        ik.push_back(k);
    }
    x.blend2d_count = static_cast<u32>(points2d.size());
    x.layer_count = static_cast<u32>(layers.size());
    x.mask_count = static_cast<u32>(mask.size());
    x.ik_count = static_cast<u32>(ik.size());
    append(&x, sizeof(x));
    append(points2d.data(), points2d.size() * sizeof(asset::AnimGraphBlendPoint2D));
    append(layers.data(), layers.size() * sizeof(asset::AnimGraphLayer));
    append(mask.data(), mask.size() * sizeof(asset::AnimGraphMaskBone));
    append(ik.data(), ik.size() * sizeof(asset::AnimGraphIK));
}

mem::UniquePtr<IImporter> make_animgraph_importer() { return mem::make_unique<AnimGraphImporter>(mem::heap()); }

}  // namespace mce::cook
