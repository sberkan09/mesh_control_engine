#include "agent_sim.h"

#include <cmath>
#include <cstring>

#include "mce/cook/fs.h"
#include "mce/ecs/hierarchy.h"
#include "mce/ecs/transform.h"
#include "mce/scene/scene.h"
#include "tool_args.h"

namespace mce::mcp {

using math::vec2;
using math::vec3;

namespace {

bool err_out(cook::String& err, StringView msg) {
    err += msg;
    return false;
}

const void* component_of(const World& world, Entity e, const refl::TypeRecord& t) {
    if (e.is_null() || !world.alive(e)) return nullptr;
    const ecs::ComponentId cid = world.find_component(t.type_hash);
    return cid == ecs::kInvalidComponent ? nullptr : world.get_raw(e, cid);
}

f64 read_scalar(const byte* p, refl::FieldKind k) {
    using K = refl::FieldKind;
    auto load = [p](auto v) {
        std::memcpy(&v, p, sizeof(v));
        return static_cast<f64>(v);
    };
    switch (k) {
        case K::I8: return load(i8{});
        case K::I16: return load(i16{});
        case K::I32: return load(i32{});
        case K::I64: return load(i64{});
        case K::U8: return load(u8{});
        case K::U16: return load(u16{});
        case K::U32: return load(u32{});
        case K::U64: return load(u64{});
        case K::F32: return load(f32{});
        case K::F64: return load(f64{});
        default: return 0;
    }
}

i64 read_enum(const byte* p, u32 size, bool is_signed) {
    i64 v = 0;
    switch (size) {
        case 1: v = is_signed ? i64(i8(p[0])) : i64(u8(p[0])); break;
        case 2: {
            u16 x;
            std::memcpy(&x, p, 2);
            v = is_signed ? i64(i16(x)) : i64(x);
            break;
        }
        case 4: {
            u32 x;
            std::memcpy(&x, p, 4);
            v = is_signed ? i64(i32(x)) : i64(x);
            break;
        }
        default: std::memcpy(&v, p, sizeof(v)); break;
    }
    return v;
}

bool parse_op(StringView s, CompareOp& op) {
    struct Name {
        const char* s;
        CompareOp op;
    };
    static constexpr Name kOps[] = {{"==", CompareOp::Eq}, {"!=", CompareOp::Ne}, {"<", CompareOp::Lt},
                                    {"<=", CompareOp::Le}, {">", CompareOp::Gt},  {">=", CompareOp::Ge}};
    for (const Name& n : kOps)
        if (s == n.s) {
            op = n.op;
            return true;
        }
    return false;
}

const char* op_name(CompareOp op) {
    switch (op) {
        case CompareOp::Eq: return "==";
        case CompareOp::Ne: return "!=";
        case CompareOp::Lt: return "<";
        case CompareOp::Le: return "<=";
        case CompareOp::Gt: return ">";
        default: return ">=";
    }
}

// Ordered comparisons need one number on each side; == and != compare every lane (within
// `tol`), bools and strings exactly.
bool compare(const Value& a, CompareOp op, const Value& b, f64 tol) {
    if (a.kind == Value::Kind::None) return false;
    if (op != CompareOp::Eq && op != CompareOp::Ne) {
        if (a.kind != Value::Kind::Number || b.kind != Value::Kind::Number || a.lanes != 1 || b.lanes != 1) return false;
        switch (op) {
            case CompareOp::Lt: return a.v[0] < b.v[0];
            case CompareOp::Le: return a.v[0] <= b.v[0] + tol;
            case CompareOp::Gt: return a.v[0] > b.v[0];
            default: return a.v[0] + tol >= b.v[0];
        }
    }
    bool same = a.kind == b.kind;
    if (same && a.kind == Value::Kind::Number) {
        same = a.lanes == b.lanes;
        for (u32 i = 0; same && i < a.lanes; ++i) same = std::fabs(a.v[i] - b.v[i]) <= tol;
    } else if (same && a.kind == Value::Kind::Bool) {
        same = (a.v[0] != 0) == (b.v[0] != 0);
    } else if (same) {
        same = a.s.view() == b.s.view();
    }
    return op == CompareOp::Eq ? same : !same;
}

void number(Value& v, f64 x) {
    v.kind = Value::Kind::Number;
    v.lanes = 1;
    v.v[0] = x;
}

void set_bool(Value& v, bool b) {
    v.kind = Value::Kind::Bool;
    v.lanes = 1;
    v.v[0] = b ? 1 : 0;
}

}  // namespace

bool load_scene_doc(Server& server, StringView rel, editor::SceneDocument& doc, cook::String& err) {
    cook::String abs;
    if (!scene_path(server, rel, abs, err)) return false;
    cook::String text;
    if (!cook::fs::read_text(abs.view(), text)) {
        err += "cannot read the scene file ";
        err += rel;
        return false;
    }
    cook::DiagList diags;
    if (!doc.parse(text.view(), rel, diags)) {
        err += "the scene does not parse:";
        append_diags(err, diags, 5);
        return false;
    }
    return true;
}

bool scene_entity_index(const editor::SceneDocument& doc, const cook::JsonValue& v, usize& index, cook::String& err) {
    i32 found = -1;
    if (v.is_integer() && v.as_number() >= 1) {
        found = doc.index_of(static_cast<editor::EntityId>(v.as_number()));
    } else if (v.is_string()) {
        for (usize i = 0; i < doc.size(); ++i) {
            if (doc.at(i).name.view() != v.as_string()) continue;
            if (found >= 0) {
                err += "two entities are named '";
                err += v.as_string();
                return err_out(err, "': give its id");
            }
            found = static_cast<i32>(i);
        }
    } else {
        return err_out(err, "an entity is an id (integer) or a unique name (string)");
    }
    if (found < 0) {
        err += "no such entity in the scene: ";
        if (v.is_string()) err += v.as_string();
        else cook::appendf(err, "%.0f", v.as_number());
        return false;
    }
    for (i32 i = 0; i < found; ++i)
        if (!doc.at(static_cast<usize>(i)).prefab.is_null())
            return err_out(err, "a prefab instance comes before that entity in the file, so its run index is not known");
    index = static_cast<usize>(found);
    return true;
}

bool parse_probe(const editor::SceneDocument& doc, const cook::JsonValue& entity, StringView field, FieldProbe& out,
                 cook::String& err) {
    if (!scene_entity_index(doc, entity, out.index, err)) return false;
    const usize dot = field.find('.');
    const StringView type_name = dot == StringView::npos ? field : field.substr(0, dot);
    const refl::TypeRecord* t = refl::find_type(StringId(type_name));
    if (!t || !t->is_component()) {
        err += "unknown component type '";
        err += type_name;
        return err_out(err, "' (refl_list_components lists them)");
    }
    if (dot == StringView::npos) return err_out(err, "a field is 'Component.path', e.g. LocalTransform.position.x");
    Result<refl::PropertyRef> ref = refl::resolve_path(*t, field.substr(dot + 1));
    if (!ref) {
        err += "no field '";
        err += field;
        return err_out(err, "' (refl_describe_component lists the fields)");
    }
    if (ref.value().count != 1 || ref.value().kind == refl::FieldKind::Struct || ref.value().kind == refl::FieldKind::AssetRef ||
        ref.value().kind == refl::FieldKind::EntityRef) {
        err += field;
        return err_out(err, ": probe a number, bool, string, enum or vector field (or one of its lanes)");
    }
    out.type = t;
    out.ref = ref.value();
    out.entity = cook::String(entity.is_string() ? entity.as_string() : StringView());
    if (!entity.is_string()) cook::appendf(out.entity, "%.0f", entity.as_number());
    out.field = cook::String(field);
    return true;
}

bool read_value(const World& world, Span<const Entity> spawned, const FieldProbe& p, Value& out) {
    out = Value{};
    if (p.index >= spawned.size() || !p.type) return false;
    const void* obj = component_of(world, spawned[p.index], *p.type);
    if (!obj) return false;
    const byte* at = static_cast<const byte*>(obj) + p.ref.offset;
    using K = refl::FieldKind;
    switch (p.ref.kind) {
        case K::Bool:
            out.kind = Value::Kind::Bool;
            out.lanes = 1;
            out.v[0] = *reinterpret_cast<const bool*>(at) ? 1 : 0;
            return true;
        case K::String:
            out.kind = Value::Kind::String;
            out.s.assign(refl::get_string(obj, p.ref));
            return true;
        case K::Enum: {
            const refl::EnumRecord* e = refl::find_enum(p.ref.type_id);
            const i64 raw = read_enum(at, p.ref.size, e && e->is_signed);
            const refl::EnumValue* named = e ? e->find(raw) : nullptr;
            if (named) {
                out.kind = Value::Kind::String;
                out.s.assign(StringView(named->name_str));
            } else {
                number(out, static_cast<f64>(raw));
            }
            return true;
        }
        case K::Vec2: case K::Vec3: case K::Vec4: case K::Quat: case K::Color: {
            out.kind = Value::Kind::Number;
            out.lanes = p.ref.kind == K::Vec2 ? 2 : p.ref.kind == K::Vec3 ? 3 : 4;
            for (u32 i = 0; i < out.lanes; ++i) {
                f32 x;
                std::memcpy(&x, at + i * sizeof(f32), sizeof(x));
                out.v[i] = x;
            }
            return true;
        }
        default: number(out, read_scalar(at, p.ref.kind)); return true;
    }
}

bool parse_value(const cook::JsonValue& v, Value& out) {
    out = Value{};
    if (v.is_number()) {
        number(out, v.as_number());
    } else if (v.is_bool()) {
        set_bool(out, v.as_bool());
    } else if (v.is_string()) {
        out.kind = Value::Kind::String;
        out.s.assign(v.as_string());
    } else if (v.is_array() && v.size() >= 1 && v.size() <= 4) {
        out.kind = Value::Kind::Number;
        out.lanes = v.size();
        for (u32 i = 0; i < v.size(); ++i) {
            if (!v.at(i).is_number()) return false;
            out.v[i] = v.at(i).as_number();
        }
    } else {
        return false;
    }
    return true;
}

void write_value(JsonWriter& w, const Value& v) {
    switch (v.kind) {
        case Value::Kind::None: w.null(); break;
        case Value::Kind::Bool: w.boolean(v.v[0] != 0); break;
        case Value::Kind::String: w.str(v.s.view()); break;
        case Value::Kind::Number:
            if (v.lanes == 1) {
                w.num_f(v.v[0]);
            } else {
                w.begin_array();
                for (u32 i = 0; i < v.lanes; ++i) w.num_f(v.v[i]);
                w.end_array();
            }
            break;
    }
}

bool entity_position(const World& world, Entity e, vec3& out) {
    static const refl::TypeRecord* world_t = refl::find_type(MCE_SID("WorldTransform"));
    static const refl::TypeRecord* local_t = refl::find_type(MCE_SID("LocalTransform"));
    if (world_t)
        if (const void* p = component_of(world, e, *world_t)) {
            out = static_cast<const WorldTransform*>(p)->position;
            return true;
        }
    if (local_t)
        if (const void* p = component_of(world, e, *local_t)) {
            out = static_cast<const LocalTransform*>(p)->position;
            return true;
        }
    return false;
}

// ---------------------------------------------------------------- feel report

void FeelTracker::tick(const World& world, Span<const Entity> spawned, usize actor, u64 tick, bool wants_move, bool died_now) {
    vec3 pos;
    const bool alive = actor < spawned.size() && entity_position(world, spawned[actor], pos);
    if (was_alive_ && !alive) ++deaths_;
    if (died_now) ++deaths_;
    was_alive_ = alive;
    alive_ = alive;
    if (!alive) return;
    if (samples_ == 0) {
        lo_ = hi_ = last_ = pos;
    } else {
        const f32 step = math::length(pos - last_);
        if (step > teleport) {
            ++deaths_;  // a respawn: count it and restart the window there
            samples_ = 0;
            lo_ = hi_ = pos;
        } else {
            distance_ += step;
            if (step < 1e-3f) ++idle_;
        }
    }
    lo_ = vec3(std::fmin(lo_.x, pos.x), std::fmin(lo_.y, pos.y), std::fmin(lo_.z, pos.z));
    hi_ = vec3(std::fmax(hi_.x, pos.x), std::fmax(hi_.y, pos.y), std::fmax(hi_.z, pos.z));
    // Coverage: 1 m cells.
    const i64 cx = static_cast<i64>(std::floor(pos.x)), cy = static_cast<i64>(std::floor(pos.y)), cz = static_cast<i64>(std::floor(pos.z));
    const u64 cell = hash_combine(hash_combine(mix64(static_cast<u64>(cx)), static_cast<u64>(cy)), static_cast<u64>(cz));
    bool seen = false;
    for (const u64 c : cells_) seen = seen || c == cell;
    if (!seen) cells_.push_back(cell);
    // Stuck: asked to move but under 0.25 m from where it was a window ago.
    if (samples_ >= kWindow && wants_move && math::length(pos - window_[samples_ % kWindow]) < 0.25f) {
        ++stuck_;
        FeelSpot* spot = nullptr;
        for (FeelSpot& s : spots_)
            if (math::length(s.at - pos) < 1.0f) spot = &s;
        if (!spot && spots_.size() < kMaxSpots) spot = &spots_.push_back(FeelSpot{pos, tick, 0});
        if (spot) ++spot->ticks;
    }
    window_[samples_ % kWindow] = pos;
    ++samples_;
    last_ = pos;
}

bool FeelTracker::metric(StringView name, f64& out) const {
    if (name == "deaths") out = deaths_;
    else if (name == "idle_ticks") out = static_cast<f64>(idle_);
    else if (name == "stuck_ticks") out = static_cast<f64>(stuck_);
    else if (name == "stuck_spots") out = static_cast<f64>(spots_.size());
    else if (name == "distance") out = distance_;
    else if (name == "cells") out = static_cast<f64>(cells_.size());
    else if (name == "finished_tick") out = static_cast<f64>(finished_tick_);
    else return false;
    return true;
}

void FeelTracker::write(JsonWriter& w, u64 ticks) const {
    const f64 hz = 60.0;
    w.begin_object();
    w.key("finished").boolean(finished_tick_ != 0);
    if (finished_tick_) w.key("time_to_finish_s").num_f(static_cast<f64>(finished_tick_) / hz);
    else w.key("time_to_finish_s").null();
    w.key("deaths").num_u(deaths_);
    w.key("idle_s").num_f(static_cast<f64>(idle_) / hz);
    w.key("idle_fraction").num_f(ticks ? static_cast<f64>(idle_) / static_cast<f64>(ticks) : 0.0);
    w.key("stuck_s").num_f(static_cast<f64>(stuck_) / hz);
    w.key("stuck_spots").begin_array();
    for (const FeelSpot& s : spots_) {
        w.begin_object().key("at").begin_array().num_f(s.at.x).num_f(s.at.y).num_f(s.at.z).end_array();
        w.key("first_tick").num_u(s.first_tick).key("seconds").num_f(static_cast<f64>(s.ticks) / hz).end_object();
    }
    w.end_array();
    w.key("distance_m").num_f(distance_);
    w.key("cells_visited").num_u(cells_.size());
    w.key("alive_at_end").boolean(alive_);
    if (samples_ || distance_ > 0) {
        w.key("bounds").begin_object();
        w.key("min").begin_array().num_f(lo_.x).num_f(lo_.y).num_f(lo_.z).end_array();
        w.key("max").begin_array().num_f(hi_.x).num_f(hi_.y).num_f(hi_.z).end_array();
        w.end_object();
    }
    w.end_object();
}

// ---------------------------------------------------------------- conditions

bool parse_condition(const editor::SceneDocument& doc, const cook::JsonValue& v, Condition& out, cook::String& err) {
    if (!v.is_object()) return err_out(err, "a check is an object, e.g. {\"entity\": \"Player\", \"field\": \"LocalTransform.position.x\", \">\": 5}");
    const cook::JsonValue tick = v.get("tick");
    if (tick.valid()) {
        if (!tick.is_integer() || tick.as_number() < 1) return err_out(err, "'tick' is a sim tick >= 1");
        out.tick = static_cast<u64>(tick.as_number());
    }
    const cook::JsonValue tol = v.get("tolerance");
    if (tol.valid()) {
        if (!tol.is_number() || tol.as_number() < 0) return err_out(err, "'tolerance' is a number >= 0");
        out.tolerance = tol.as_number();
    }
    // The comparison: "op" + "value", or one operator key ({"<=": 3}).
    bool have_value = false;
    const cook::JsonValue op = v.get("op");
    if (op.valid()) {
        if (!op.is_string() || !parse_op(op.as_string(), out.op)) return err_out(err, "'op' is one of == != < <= > >=");
    }
    const cook::JsonValue value = v.get("value");
    if (value.valid()) {
        if (!parse_value(value, out.expected)) return err_out(err, "'value' is a number, bool, string or [x, y, ...]");
        have_value = true;
    }
    for (u32 i = 0; i < v.size(); ++i) {
        CompareOp o;
        if (!parse_op(v.key_at(i), o)) continue;
        if (have_value) return err_out(err, "give one comparison per check");
        out.op = o;
        if (!parse_value(v.at(i), out.expected)) return err_out(err, "a comparison value is a number, bool, string or [x, y, ...]");
        have_value = true;
    }
    const cook::JsonValue field = v.get("field"), alive = v.get("alive"), event = v.get("event"), log_has = v.get("log_contains"),
                          log_err = v.get("log_errors"), log_warn = v.get("log_warnings"), feel = v.get("feel"),
                          scene = v.get("scene");
    if (field.valid()) {
        out.what = Condition::What::Field;
        if (!field.is_string()) return err_out(err, "'field' is 'Component.path'");
        if (!parse_probe(doc, v.get("entity"), field.as_string(), out.probe, err)) return false;
        if (!have_value) return err_out(err, "a field check needs a comparison: \"value\" (==), or {\">\": 5} etc.");
    } else if (alive.valid()) {
        out.what = Condition::What::Alive;
        if (!scene_entity_index(doc, alive, out.probe.index, err)) return false;
        out.probe.entity = cook::String(alive.is_string() ? alive.as_string() : StringView());
        if (!have_value) set_bool(out.expected, true);
    } else if (event.valid()) {
        out.what = Condition::What::Event;
        if (!event.is_string() || event.as_string().empty()) return err_out(err, "'event' is an event type name");
        out.text = cook::String(event.as_string());
        if (!have_value) {
            out.op = CompareOp::Ge;
            number(out.expected, 1);
        }
    } else if (scene.valid()) {
        // The scene the run is in (scene::request_change moves it), by source path.
        out.what = Condition::What::Scene;
        if (!scene.is_string() || scene.as_string().empty()) return err_out(err, "'scene' is a scene's source path, e.g. \"scenes/win.mcscene\"");
        if (have_value) return err_out(err, "a scene check takes no comparison: {\"scene\": \"scenes/win.mcscene\"}");
        StringView want = scene.as_string();
        if (want.starts_with("assets/")) want.remove_prefix(7);
        out.text = cook::String(want);
        out.expected = Value{};
        out.expected.kind = Value::Kind::String;
        out.expected.s.assign(want);
    } else if (log_has.valid()) {
        out.what = Condition::What::LogContains;
        if (!log_has.is_string() || log_has.as_string().empty()) return err_out(err, "'log_contains' is the text to find");
        out.text = cook::String(log_has.as_string());
        if (!have_value) set_bool(out.expected, true);
    } else if (log_err.valid() || log_warn.valid()) {
        const cook::JsonValue n = log_err.valid() ? log_err : log_warn;
        out.what = log_err.valid() ? Condition::What::LogErrors : Condition::What::LogWarnings;
        if (!n.is_integer() || n.as_number() < 0) return err_out(err, "'log_errors' / 'log_warnings' is the most allowed (an integer)");
        out.op = CompareOp::Le;
        number(out.expected, n.as_number());
    } else if (feel.valid()) {
        out.what = Condition::What::Feel;
        f64 probe = 0;
        FeelTracker t;
        if (!feel.is_string() || !t.metric(feel.as_string(), probe))
            return err_out(err, "'feel' is deaths, idle_ticks, stuck_ticks, stuck_spots, distance, cells or finished_tick");
        out.text = cook::String(feel.as_string());
        if (!have_value) return err_out(err, "a feel check needs a comparison, e.g. {\"feel\": \"deaths\", \"<=\": 0}");
    } else {
        return err_out(err, "a check names one of: field (+ entity), alive, event, scene, log_contains, log_errors, log_warnings, feel");
    }
    return true;
}

bool eval_world(Condition& c, const World& world, Span<const Entity> spawned) {
    switch (c.what) {
        case Condition::What::Field: read_value(world, spawned, c.probe, c.actual); break;
        case Condition::What::Alive:
            c.actual = Value{};
            set_bool(c.actual, c.probe.index < spawned.size() && world.alive(spawned[c.probe.index]));
            break;
        case Condition::What::Event: {
            Array<EventInfo> events;
            world.event_registry(events);
            // The C++ name (game::GemCollected), or the name without its namespace when that is unique.
            const StringView want = c.text.view();
            auto unqualified = [&](StringView name) {
                return name.size() > want.size() + 2 && name.substr(name.size() - want.size()) == want &&
                       name.substr(name.size() - want.size() - 2, 2) == "::";
            };
            u64 pushed = 0;
            u32 exact = 0, suffix = 0;
            for (const EventInfo& e : events) exact += e.name == want ? 1u : 0u;
            for (const EventInfo& e : events) {
                const bool match = exact ? e.name == want : unqualified(e.name);
                if (!match) continue;
                pushed += e.total_pushed;
                ++suffix;
            }
            // An unknown or ambiguous name has no count (the check fails, with a note), never a silent 0.
            c.actual = Value{};
            if (exact || suffix == 1) number(c.actual, static_cast<f64>(pushed));
            c.note.clear();
            if (!exact && suffix > 1) {
                c.note = "several event types end in '";
                c.note += want;
                c.note += "': name one with its namespace";
            } else if (!exact && suffix == 0) {
                c.note = "no event type named '";
                c.note += want;
                c.note += "' is registered; the game's events:";
                for (const EventInfo& e : events) {
                    c.note += ' ';
                    c.note += e.name;
                }
            }
            break;
        }
        case Condition::What::Scene: {
            // The World is const here; the resource is only read.
            const scene::SceneChange* sc = const_cast<World&>(world).try_resource<scene::SceneChange>();  // NOLINT(cppcoreguidelines-pro-type-const-cast)
            c.actual = Value{};
            c.actual.kind = Value::Kind::String;
            if (sc) c.actual.s.assign(sc->current.view());
            break;
        }
        default: return false;
    }
    c.checked = true;
    c.ok = compare(c.actual, c.op, c.expected, c.tolerance);
    return c.ok;
}

void eval_after(Condition& c, Span<const log::ConsoleLine> lines, const FeelTracker* feel) {
    if (c.what != Condition::What::LogContains && c.what != Condition::What::LogErrors && c.what != Condition::What::LogWarnings &&
        c.what != Condition::What::Feel)
        return;  // world checks were evaluated in the run
    c.actual = Value{};
    switch (c.what) {
        case Condition::What::LogContains: {
            bool found = false;
            for (const log::ConsoleLine& l : lines) found = found || StringView(l.text.view()).find(c.text.view()) != StringView::npos;
            set_bool(c.actual, found);
            break;
        }
        case Condition::What::LogErrors:
        case Condition::What::LogWarnings: {
            u64 n = 0;
            for (const log::ConsoleLine& l : lines) {
                if (c.what == Condition::What::LogWarnings && l.level == log::Level::Warn) ++n;
                if (c.what == Condition::What::LogErrors && l.level >= log::Level::Error && l.level != log::Level::Off) ++n;
            }
            number(c.actual, static_cast<f64>(n));
            break;
        }
        case Condition::What::Feel: {
            f64 m = 0;
            if (feel && feel->metric(c.text.view(), m)) number(c.actual, m);
            break;
        }
        default: return;
    }
    c.checked = true;
    c.ok = compare(c.actual, c.op, c.expected, c.tolerance);
}

void write_condition(JsonWriter& w, const Condition& c) {
    w.begin_object();
    cook::String what;
    switch (c.what) {
        case Condition::What::Field:
            what += c.probe.entity.view();
            what += '.';
            what += c.probe.field.view();
            break;
        case Condition::What::Alive:
            what += "alive ";
            what += c.probe.entity.view();
            break;
        case Condition::What::Event:
            what += "event ";
            what += c.text.view();
            break;
        case Condition::What::Scene: what += "scene"; break;
        case Condition::What::LogContains:
            what += "log contains \"";
            what += c.text.view();
            what += '"';
            break;
        case Condition::What::LogErrors: what += "log errors"; break;
        case Condition::What::LogWarnings: what += "log warnings"; break;
        case Condition::What::Feel:
            what += "feel ";
            what += c.text.view();
            break;
    }
    w.key("check").str(what.view());
    w.key("op").str(op_name(c.op));
    w.key("expected");
    write_value(w, c.expected);
    w.key("actual");
    write_value(w, c.actual);
    if (c.tick) w.key("tick").num_u(c.tick);
    w.key("ok").boolean(c.ok);
    if (!c.checked) w.key("note").str(c.tick ? "the run ended before this tick" : "not evaluated");
    else if (!c.note.empty())
        w.key("note").str(c.note.view());
    else if (c.what == Condition::What::Field && c.actual.kind == Value::Kind::None)
        w.key("note").str("the entity is gone or has no such component (or the run is in another scene)");
    w.end_object();
}

// ---------------------------------------------------------------- bots

const char* bot_kind_name(BotConfig::Kind k) {
    switch (k) {
        case BotConfig::Kind::WalkTo: return "walk_to";
        case BotConfig::Kind::Explore: return "explore";
        default: return "masher";
    }
}

bool parse_bot(const editor::SceneDocument& doc, const cook::JsonValue& v, BotConfig& out, cook::String& err) {
    if (!v.is_object()) return err_out(err, "'bot' is an object: {\"kind\": \"walk_to\", \"actor\": \"Player\", \"target\": \"Goal\"}");
    const cook::JsonValue kind = v.get("kind");
    const StringView k = kind.is_string() ? kind.as_string() : StringView();
    if (k == "walk_to") out.kind = BotConfig::Kind::WalkTo;
    else if (k == "explore") out.kind = BotConfig::Kind::Explore;
    else if (k == "masher") out.kind = BotConfig::Kind::Masher;
    else return err_out(err, "bot.kind is walk_to, explore or masher");
    const cook::JsonValue actor = v.get("actor");
    if (actor.valid()) {
        if (!scene_entity_index(doc, actor, out.actor, err)) return false;
        out.has_actor = true;
    } else if (out.kind != BotConfig::Kind::Masher) {
        return err_out(err, "bot.actor: the entity the bot plays (an id or a unique name)");
    }
    const cook::JsonValue plane = v.get("plane");
    if (plane.valid()) {
        const StringView p = plane.is_string() ? plane.as_string() : StringView();
        if (p == "xy") out.plane = BotConfig::Plane::XY;
        else if (p == "x") out.plane = BotConfig::Plane::X;
        else if (p == "xz") out.plane = BotConfig::Plane::XZ;
        else return err_out(err, "bot.plane is xy (top-down 2D), x (side view) or xz (3D ground)");
    }
    if (out.kind == BotConfig::Kind::WalkTo) {
        auto one = [&](const cook::JsonValue& t, const char* what) {
            BotTarget bt;
            if (t.is_array()) {
                Value pt;
                if (!parse_value(t, pt) || pt.kind != Value::Kind::Number || pt.lanes < 2 || pt.lanes > 3) {
                    err += what;
                    return err_out(err, " is a point [x, y] / [x, y, z] or an entity");
                }
                bt.point = vec3(static_cast<f32>(pt.v[0]), static_cast<f32>(pt.v[1]), pt.lanes == 3 ? static_cast<f32>(pt.v[2]) : 0.f);
            } else {
                if (!scene_entity_index(doc, t, bt.index, err)) return false;
                bt.is_entity = true;
            }
            out.targets.push_back(bt);
            return true;
        };
        const cook::JsonValue through = v.get("through");
        if (through.valid()) {
            if (!through.is_array() || through.size() == 0 || through.size() > 64)
                return err_out(err, "bot.through is an array of 1-64 waypoints (points [x, y(, z)] or entities), reached in order before target");
            for (u32 i = 0; i < through.size(); ++i)
                if (!one(through.at(i), "bot.through[]")) return false;
        }
        const cook::JsonValue target = v.get("target");
        if (!target.valid()) return err_out(err, "bot.target: a point [x, y(, z)] or an entity for walk_to");
        if (!one(target, "bot.target")) return false;
    }
    auto name = [&](const char* key, FixedString<32>& dst) {
        const cook::JsonValue n = v.get(key);
        if (!n.valid()) return true;
        if (!n.is_string() || n.as_string().empty() || n.as_string().size() > 31) {
            err += "bot.";
            err += key;
            return err_out(err, " is an action name of 1-31 characters");
        }
        dst.assign(n.as_string());
        return true;
    };
    if (!name("move", out.move) || !name("jump", out.jump)) return false;
    const cook::JsonValue actions = v.get("actions");
    if (actions.valid()) {
        if (!actions.is_array() || actions.size() == 0 || actions.size() > 16) return err_out(err, "bot.actions is an array of 1-16 action names");
        for (u32 i = 0; i < actions.size(); ++i) {
            const cook::JsonValue a = actions.at(i);
            if (!a.is_string() || a.as_string().empty() || a.as_string().size() > 31) return err_out(err, "bot.actions: action names of 1-31 characters");
            out.actions.push_back(FixedString<32>(a.as_string()));
        }
    } else if (out.kind == BotConfig::Kind::Masher) {
        for (const char* a : {"Move", "Jump", "Fire", "Confirm", "Cancel"}) out.actions.push_back(FixedString<32>(StringView(a)));
    }
    const cook::JsonValue air = v.get("air_jumps");
    if (air.valid()) {
        if (!air.is_integer() || air.as_number() < 0 || air.as_number() > 4) return err_out(err, "bot.air_jumps is 0-4 extra Jump presses in the air");
        out.air_jumps = static_cast<u32>(air.as_number());
    }
    const cook::JsonValue reach = v.get("reach"), seed = v.get("seed"), period = v.get("period");
    if (reach.valid()) {
        if (!reach.is_number() || reach.as_number() <= 0) return err_out(err, "bot.reach is a distance > 0 (metres)");
        out.reach = static_cast<f32>(reach.as_number());
    }
    if (seed.valid()) {
        if (!seed.is_integer() || seed.as_number() < 0) return err_out(err, "bot.seed is an integer >= 0");
        out.seed = static_cast<u64>(seed.as_number());
    }
    if (period.valid()) {
        if (!period.is_integer() || period.as_number() < 1 || period.as_number() > 600) return err_out(err, "bot.period is 1-600 ticks");
        out.period = static_cast<u32>(period.as_number());
    }
    return true;
}

u64 Bot::next() {
    rng_ = mix64(rng_ + 0x9E3779B97F4A7C15ull);
    return rng_;
}

void Bot::hold_move(u64 tick, vec2 dir) {
    if (cfg_.plane == BotConfig::Plane::X) dir.y = 0;
    if (math::length(dir) < 1e-4f) return;
    wants_move_ = true;
    script_.add(cfg_.move.view(), tick, tick, dir.x, dir.y);
}

const input::InputState* Bot::sample_for_tick(u64 tick, u64) {
    script_.clear_segments();
    wants_move_ = false;
    vec3 pos;
    const bool have = cfg_.has_actor && world_ && cfg_.actor < spawned_.size() && entity_position(*world_, spawned_[cfg_.actor], pos);
    switch (cfg_.kind) {
        case BotConfig::Kind::WalkTo:
            if (have) walk_to(tick, pos);
            break;
        case BotConfig::Kind::Explore:
            if (have) explore(tick, pos);
            break;
        case BotConfig::Kind::Masher: masher(tick); break;
    }
    return &script_.evaluate(tick);
}

namespace {

// The plane's 2D coordinates of a world point (xz: forward is -Z, so Move.y = -z).
vec2 flat(BotConfig::Plane p, vec3 v) { return p == BotConfig::Plane::XZ ? vec2(v.x, -v.z) : vec2(v.x, p == BotConfig::Plane::X ? 0.f : v.y); }

}  // namespace

void Bot::walk_to(u64 tick, vec3 pos) {
    // The current leg's target; a reached waypoint (or a gone entity: collected) moves on to the next.
    vec2 d;
    f32 dist = 0;
    for (;;) {
        if (leg_ >= cfg_.targets.size()) {
            reached_ = true;
            return;
        }
        const BotTarget& t = cfg_.targets[leg_];
        vec3 at = t.point;
        const bool gone = t.is_entity && (t.index >= spawned_.size() || !entity_position(*world_, spawned_[t.index], at));
        d = flat(cfg_.plane, at) - flat(cfg_.plane, pos);
        dist = math::length(d);
        if (!gone && dist > cfg_.reach) break;
        if (leg_ + 1 == cfg_.targets.size()) {
            reached_ = true;
            return;
        }
        ++leg_;
        best_ = 1e30f;
        best_tick_ = tick;
    }
    reached_ = false;
    if (dist < best_ - 0.05f) {
        best_ = dist;
        best_tick_ = tick;
    }
    const bool stalled = tick - best_tick_ > 30;
    vec2 dir = d * (1.0f / dist);
    if (cfg_.plane == BotConfig::Plane::X) {
        // Side view: hold the direction, jump when progress stalls (a step, a gap, a wall).
        dir = vec2(d.x > 0 ? 1.f : -1.f, 0.f);
        // With air_jumps, each press is held 10 ticks and released for 2 before the next one.
        if (stalled && tick >= jump_until_ + 20) {
            jump_from_ = tick;
            jump_until_ = tick + 14 + 12 * cfg_.air_jumps;
            best_tick_ = tick;  // give the jump its chance
        }
        const bool released = cfg_.air_jumps > 0 && (tick - jump_from_) % 12 >= 10;
        if (tick <= jump_until_ && !released) script_.add(cfg_.jump.view(), tick, tick);
    } else {
        // Top-down: sidestep (alternating sides) for half a second when progress stalls.
        if (stalled && tick >= detour_until_) {
            detour_until_ = tick + 30;
            detour_side_ = -detour_side_;
            best_tick_ = tick;
        }
        if (tick < detour_until_) dir = math::normalize(vec2(-dir.y, dir.x) * detour_side_ + dir * 0.25f);
    }
    hold_move(tick, dir);
}

void Bot::explore(u64 tick, vec3 pos) {
    const bool side = cfg_.plane == BotConfig::Plane::X;
    // Blocked: under 5 cm in a quarter second while walking.
    if (math::length(pos - last_pos_) > 0.05f || still_since_ == 0) {
        last_pos_ = pos;
        still_since_ = tick;
    }
    const bool blocked = tick - still_since_ > 15;
    if (tick >= turn_at_ || blocked) {
        if (side) {
            heading_ = vec2(blocked ? -heading_.x : (unit() < 0.5f ? -1.f : 1.f), 0.f);
            if (heading_.x == 0) heading_.x = 1.f;
        } else {
            const f32 a = unit() * 2.0f * math::kPi;
            heading_ = vec2(std::cos(a), std::sin(a));
        }
        turn_at_ = tick + 30 + (next() % 61);
        still_since_ = tick;
        last_pos_ = pos;
        if (side && blocked) jump_until_ = tick + 14;
    }
    if (side && (tick <= jump_until_ || (next() % 97) == 0)) {
        if (tick > jump_until_) jump_until_ = tick + 10;
        script_.add(cfg_.jump.view(), tick, tick);
    }
    hold_move(tick, heading_);
}

void Bot::masher(u64 tick) {
    // A new random chord every `period` ticks, held for the period: presses and releases on the edges.
    if (tick >= turn_at_) {
        turn_at_ = tick + cfg_.period;
        heading_ = vec2(unit() * 2.f - 1.f, unit() * 2.f - 1.f);
        chord_ = next();  // bit i: actions[i] is held
    }
    for (usize i = 0; i < cfg_.actions.size(); ++i) {
        if (!((chord_ >> i) & 1)) continue;
        if (cfg_.actions[i].view() == cfg_.move.view()) hold_move(tick, heading_);
        else script_.add(cfg_.actions[i].view(), tick, tick);
    }
}

// ---------------------------------------------------------------- traces

bool parse_trace(const editor::SceneDocument& doc, const cook::JsonValue& v, Trace& out, cook::String& err) {
    if (!v.is_object()) return err_out(err, "'trace' is an object: {\"every\": 10, \"fields\": [{\"entity\": \"Player\", \"field\": \"LocalTransform.position\"}]}");
    const cook::JsonValue every = v.get("every"), fields = v.get("fields");
    if (every.valid()) {
        if (!every.is_integer() || every.as_number() < 1 || every.as_number() > 3600) return err_out(err, "trace.every is 1-3600 ticks");
        out.every = static_cast<u64>(every.as_number());
    }
    if (!fields.is_array() || fields.size() == 0 || fields.size() > Trace::kMaxFields)
        return err_out(err, "trace.fields is an array of 1-16 {entity, field}");
    for (u32 i = 0; i < fields.size(); ++i) {
        const cook::JsonValue f = fields.at(i);
        if (!f.is_object() || !f.get("field").is_string()) return err_out(err, "trace.fields[]: {\"entity\": id or name, \"field\": \"Component.path\"}");
        FieldProbe p;
        if (!parse_probe(doc, f.get("entity"), f.get("field").as_string(), p, err)) return false;
        out.fields.push_back(static_cast<FieldProbe&&>(p));
    }
    return true;
}

void Trace::sample(const World& world, Span<const Entity> spawned, u64 tick) {
    if (tick % every != 0 || ticks.size() >= kMaxRows) return;
    ticks.push_back(tick);
    for (const FieldProbe& p : fields) {
        Value v;
        read_value(world, spawned, p, v);
        values.push_back(v);
    }
}

void Trace::write(JsonWriter& w) const {
    w.begin_object();
    w.key("every").num_u(every);
    w.key("ticks").begin_array();
    for (const u64 t : ticks) w.num_u(t);
    w.end_array();
    w.key("series").begin_array();
    for (usize f = 0; f < fields.size(); ++f) {
        w.begin_object().key("entity").str(fields[f].entity.view()).key("field").str(fields[f].field.view());
        w.key("values").begin_array();
        for (usize r = 0; r < ticks.size(); ++r) write_value(w, values[r * fields.size() + f]);
        w.end_array().end_object();
    }
    w.end_array();
    if (ticks.size() >= kMaxRows) w.key("truncated").boolean(true);
    w.end_object();
}

// ---------------------------------------------------------------- one run

RunHooks SimRun::hooks() {
    RunHooks h;
    h.user = this;
    h.start = [](World& world, Span<const Entity> spawned, void* user) {
        auto* r = static_cast<SimRun*>(user);
        if (r->bot) r->bot->attach(world, spawned);
    };
    h.tick = [](const World& world, Span<const Entity> spawned, u64 tick, void* user) {
        auto* r = static_cast<SimRun*>(user);
        r->last_tick = tick;
        if (r->trace) r->trace->sample(world, spawned, tick);
        // A scene change leaves the starting scene (the hook sees no entities): the level is over,
        // not the actor dead.
        if (spawned.empty() && !r->left_scene) {
            r->left_scene = true;
            if (r->feel) r->feel->finish(tick);
        }
        bool died = false;
        if (r->death && !r->left_scene) {
            const bool now = eval_world(*r->death, world, spawned);
            died = now && !r->death_was;
            r->death_was = now;
        }
        if (r->feel && r->has_actor && !r->left_scene) {
            bool wants = r->bot && r->bot->wants_move();
            if (!r->bot && r->script) wants = math::length(r->script->state().move) > 1e-4f;
            r->feel->tick(world, spawned, r->actor, tick, wants, died);
        }
        if (r->checks)
            for (Condition& c : *r->checks)
                if (c.tick == tick) eval_world(c, world, spawned);
        bool stop = false;
        // walk_to: the game gets half a second to react to the arrival (a door opens, a pickup
        // counts, the scene changes) before the run ends.
        if (r->bot && r->bot->config().kind == BotConfig::Kind::WalkTo && r->bot->reached() && r->reached_at == 0) {
            r->reached_at = tick;
            if (r->feel) r->feel->finish(tick);
        }
        if (r->reached_at != 0 && tick >= r->reached_at + SimRun::kSettleTicks) stop = true;
        if (r->until && eval_world(*r->until, world, spawned)) stop = true;
        if (stop) {
            if (r->feel) r->feel->finish(tick);
            r->stopped_early = true;
            return false;
        }
        return true;
    };
    return h;
}

}  // namespace mce::mcp
