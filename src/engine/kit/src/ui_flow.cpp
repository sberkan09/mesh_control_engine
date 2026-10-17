#include "mce/kit/ui_flow.h"

#include "mce/ecs/world.h"
#include "mce/input/actions.h"
#include "mce/kit/ai.h"
#include "mce/kit/components.h"
#include "mce/kit/sequence.h"
#include "mce/kit/stock.h"
#include "mce/kit/timeline.h"
#include "mce/refl/property_path.h"
#include "mce/refl/type.h"
#include "mce/scene/scene.h"

namespace mce::kit {

namespace {

f64 read_number(const void* p, refl::FieldKind k) {
    switch (k) {
        case refl::FieldKind::Bool: return *static_cast<const bool*>(p) ? 1.0 : 0.0;
        case refl::FieldKind::I8: return *static_cast<const i8*>(p);
        case refl::FieldKind::I16: return *static_cast<const i16*>(p);
        case refl::FieldKind::I32: return *static_cast<const i32*>(p);
        case refl::FieldKind::I64: return static_cast<f64>(*static_cast<const i64*>(p));
        case refl::FieldKind::U8: return *static_cast<const u8*>(p);
        case refl::FieldKind::U16: return *static_cast<const u16*>(p);
        case refl::FieldKind::U32: return *static_cast<const u32*>(p);
        case refl::FieldKind::U64: return static_cast<f64>(*static_cast<const u64*>(p));
        case refl::FieldKind::F32: return *static_cast<const f32*>(p);
        case refl::FieldKind::F64: return *static_cast<const f64*>(p);
        default: break;
    }
    return 0.0;
}

// The number at "Component/field" on the first entity (table order, then row) that has the
// component. False when the path does not resolve, is not a number, or no entity has it.
bool read_field(const World& w, StringView path, f64& out) {
    const usize slash = path.find('/');
    if (slash == StringView::npos) return false;
    const refl::TypeRecord* type = refl::find_type(StringId(path.substr(0, slash)));
    if (!type || !type->is_component()) return false;
    Result<refl::PropertyRef> ref = refl::resolve_path(*type, path.substr(slash + 1));
    if (!ref || ref.value().count != 1) return false;
    const refl::FieldKind k = ref.value().kind;
    if (!(k == refl::FieldKind::Bool || (k >= refl::FieldKind::I8 && k <= refl::FieldKind::F64))) return false;
    const ecs::ComponentId c = w.find_component_by_refl(type->id.value);
    if (c == ecs::kInvalidComponent) return false;
    for (const auto& tp : w.tables()) {
        if (tp->size() == 0) continue;
        const void* cell = tp->cell(c, 0);
        if (!cell) continue;
        out = read_number(static_cast<const byte*>(cell) + ref.value().offset, k);
        return true;
    }
    return false;
}

bool condition_holds(const World& w, const UiFlowRule& r) {
    if (r.field.empty()) return false;
    f64 v = 0.0;
    if (!read_field(w, r.field.view(), v)) return false;
    switch (r.when) {
        case UiFlowWhen::FieldEquals: return v == static_cast<f64>(r.value);
        case UiFlowWhen::FieldAbove: return v > static_cast<f64>(r.value);
        case UiFlowWhen::FieldBelow: return v < static_cast<f64>(r.value);
        default: break;
    }
    return false;
}

// This tick's readable signals of one type; the type may not be installed in a world without it.
template <class T>
bool signal_of(Frame& f, StringId id) {
    if (!f.world().event_queue<T>()) return false;
    for (const T& s : f.events().read<T>())
        if (s.id == id) return true;
    return false;
}

bool signalled(Frame& f, StringId id) {
    // Every producer of a BtSignal (a BT emit node, Luau mce.signal) also pushes a SeqSignal, but the two queues
    // swap in different phases, so reading both could see one emit on two ticks: BtSignal only stands in
    // for a world without sequences.
    return signal_of<SeqSignal>(f, id) || signal_of<TimelineSignal>(f, id) ||
           (!f.world().event_queue<SeqSignal>() && signal_of<BtSignal>(f, id));
}

}  // namespace

void apply_ui_flow_rules(Frame& f, const input::InputState* in, asset::Guid& load) {
    f.query<UiFlowRules&>().each([&](Entity e, UiFlowRules& rules) {
        u8 armed = rules.armed;
        for (u32 i = 0; i < kMaxUiFlowRules; ++i) {
            const UiFlowRule& r = rules.rules[i];
            const u8 bit = static_cast<u8>(1u << i);
            const bool field_rule = r.when >= UiFlowWhen::FieldEquals;
            if ((r.act == UiFlowDo::None && r.act2 == UiFlowDo::None) || !field_rule) armed = static_cast<u8>(armed & ~bit);
            if (r.act == UiFlowDo::None && r.act2 == UiFlowDo::None) continue;
            bool fire = false;
            switch (r.when) {
                case UiFlowWhen::Action:
                    if (!r.name.empty() && in) {
                        const StringId id(r.name.view());
                        fire = in->has_action(id) && in->pressed(id);
                    }
                    break;
                case UiFlowWhen::Signal: fire = !r.name.empty() && signalled(f, StringId(r.name.view())); break;
                default: {
                    const bool holds = condition_holds(f.world(), r);
                    fire = holds && !(armed & bit);
                    armed = holds ? static_cast<u8>(armed | bit) : static_cast<u8>(armed & ~bit);
                    break;
                }
            }
            if (!fire) continue;
            for (const UiFlowDo act : {r.act, r.act2}) {
                switch (act) {
                    case UiFlowDo::Show:
                    case UiFlowDo::Hide:
                    case UiFlowDo::Toggle:
                        if (UiScreen* s = f.world().try_get_mut<UiScreen>(e))
                            s->visible = act == UiFlowDo::Toggle ? !s->visible : act == UiFlowDo::Show;
                        break;
                    case UiFlowDo::Load:
                        if (!r.scene.guid.is_null() && load.is_null()) load = r.scene.guid;
                        break;
                    case UiFlowDo::Quit: scene::request_quit(f.world()); break;
                    case UiFlowDo::None: break;
                }
            }
        }
        if (armed != rules.armed) rules.armed = armed;
    });
}

}  // namespace mce::kit
