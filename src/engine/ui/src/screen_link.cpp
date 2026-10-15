// Gameplay <-> UI documents through data (M-UI-DATA step 2; mce/ui/screen_link.h).

#include "mce/ui/screen_link.h"

#include "mce/ecs/world.h"
#include "mce/log.h"
#include "mce/refl/type.h"

namespace mce::ui {

ActionRelay::Entry& ActionRelay::entry(StringView action) {
    const StringId id(action);
    for (Entry& e : entries_)
        if (e.id == id) return e;
    Entry e;
    e.name.assign(action);
    e.id = id;
    entries_.push_back(e);
    return entries_.back();
}

void ActionRelay::declare(StringView action) {
    if (!action.empty()) entry(action);
}

void ActionRelay::raise(StringView action, f32 value) {
    if (action.empty()) return;
    Entry& e = entry(action);
    e.raised = true;
    e.value = value;
}

void ActionRelay::collect(const Document& doc) {
    for (const UiEvent& ev : doc.events()) {
        if (ev.kind != UiEvent::Confirmed && ev.kind != UiEvent::ValueChanged && ev.kind != UiEvent::Cancelled) continue;
        const StringView action = doc.action(ev.widget);
        if (!action.empty()) raise(action, ev.kind == UiEvent::ValueChanged ? ev.value : 1.0f);
    }
}

bool ActionRelay::pending() const {
    for (const Entry& e : entries_)
        if (e.raised) return true;
    return false;
}

void ActionRelay::apply(input::InputState& state, f32 dt) {
    for (Entry& e : entries_) {
        const bool release = e.held && !e.raised;
        const bool raised = e.raised;
        e.held = e.raised;
        e.raised = false;
        if (!raised && !release) continue;  // like a script: an idle action is not in the snapshot
        u32 slot = state.action_count;
        for (u32 i = 0; i < state.action_count; ++i)
            if (state.action_names[i] == e.id) slot = i;
        if (slot == state.action_count) {
            if (state.action_count >= input::kMaxActions) {
                if (!overflow_logged_) MCE_LOG_ERROR("ui: no room for action '{}' in the tick's input", e.name.c_str());
                overflow_logged_ = true;
                e.held = false;
                continue;
            }
            state.action_names[slot] = e.id;
            state.actions[slot] = input::ActionState{};
            ++state.action_count;
        }
        input::ActionState& a = state.actions[slot];
        if (a.held) continue;  // a key or a script already holds it: its own state stands
        if (raised) {
            a.x = e.value;
            a.y = 0.0f;
            a.held = true;
            a.pressed = true;
            a.released = false;
            a.held_time = dt;
        } else {
            a.released = true;
        }
    }
    // The fast-path fields InputScript and InputSystem fill by name.
    for (u32 i = 0; i < state.action_count; ++i) {
        const input::ActionState& a = state.actions[i];
        const StringId n = state.action_names[i];
        if (n == MCE_SID("Pause")) state.pause_pressed = state.pause_pressed || a.pressed;
        else if (n == MCE_SID("Confirm")) state.confirm_pressed = state.confirm_pressed || a.pressed;
        else if (n == MCE_SID("Cancel")) state.cancel_pressed = state.cancel_pressed || a.pressed;
        else if (n == MCE_SID("Fire")) state.fire_pressed = state.fire_pressed || a.pressed;
    }
}

u32 bind_document(Document& doc, const World& world, Entity self, Span<const Entity> entities) {
    u32 unbound = 0;
    Array<WidgetId> stack;
    for (WidgetId c = doc.first_child(doc.root()); doc.valid(c); c = doc.next_sibling(c)) stack.push_back(c);
    while (!stack.empty()) {
        const WidgetId w = stack.back();
        stack.pop_back();
        for (WidgetId c = doc.first_child(w); doc.valid(c); c = doc.next_sibling(c)) stack.push_back(c);
        const StringView path = doc.binding(w);
        if (path.empty() || doc.binding_done(w)) continue;
        const usize slash = path.find('/');
        const refl::TypeRecord* type = slash == StringView::npos ? nullptr : refl::find_type(StringId(path.substr(0, slash)));
        if (!type || !type->is_component()) {
            MCE_LOG_ERROR("ui: '{}' on widget '{}' does not name a reflected component", path, doc.name(w));
            doc.set_binding_done(w);
            continue;
        }
        const ecs::ComponentId component = world.find_component_by_refl(type->id.value);
        Entity target;
        bool found = false;
        if (component != ecs::kInvalidComponent) {
            if (world.has(self, component)) {
                target = self;
                found = true;
            } else {
                for (const Entity e : entities) {
                    if (world.has(e, component)) {
                        target = e;
                        found = true;
                        break;
                    }
                }
            }
        }
        if (!found) {
            ++unbound;
            continue;
        }
        const FixedString<32> format(doc.binding_format(w));
        doc.bind_property(w, world, target, path, format.empty() ? nullptr : format.c_str());  // logs a bad path
        doc.set_binding_done(w);
    }
    return unbound;
}

}  // namespace mce::ui
