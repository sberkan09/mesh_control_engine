#pragma once
// mce_ui: gameplay and UI documents talk through data (M-UI-DATA step 2). A host that shows
// documents over a world (the stock player's kit::UiScreen) links them with two pieces:
//
//   ActionRelay   the input actions a document's widgets name ("action" in .mcui): a confirmed
//                 button, a changed slider / toggle / tabs or a cancelled modal raises its action,
//                 and the next tick's InputState carries it like any other input. Gameplay reads
//                 input::InputState only, and a -input script naming the action replays the run.
//   bind_document the widgets' "bind": "Component/field" paths, bound read-only to the world
//                 (Document::bind_property) for HUD values.
//
// Gameplay modules never include this header (or any mce/ui header): check_walls.py enforces it.

#include "mce/containers/array.h"
#include "mce/fixed_string.h"
#include "mce/input/actions.h"
#include "mce/ui/ui.h"

namespace mce::ui {

class ActionRelay {
public:
    // An action a loaded document names (declared_count; the host logs them). Like a script's
    // actions, it is in a tick's snapshot only on the tick it is pressed and the tick it is
    // released, so gameplay asks has_action() first (an unknown name logs an error).
    void declare(StringView action);
    // Raised for the next apply(): pressed and held for one tick with x = value.
    void raise(StringView action, f32 value = 1.0f);
    // Raises the actions of the widgets behind `doc`'s last update() events: Confirmed and
    // Cancelled with 1, ValueChanged with the new value. Widgets without an action raise nothing.
    void collect(const Document& doc);
    // Folds the raised pulses into the tick's snapshot with InputScript's rules: a pulse is pressed
    // + held for exactly one tick (held_time = dt, the fast-path field of its name set) and named
    // released on the next, so a script line "<tick> <Action>[=<value>]" produces the same
    // snapshot (and hash). An action the snapshot already holds (a key, a script) keeps its own
    // state. Past kMaxActions, actions are dropped with one error.
    void apply(input::InputState& state, f32 dt);
    bool pending() const;  // a raise waits for the next apply()
    void clear() { entries_.clear(); }
    u32 declared_count() const { return static_cast<u32>(entries_.size()); }

private:
    struct Entry {
        FixedString<32> name;
        StringId id;
        f32 value = 0.0f;
        bool raised = false;
        bool held = false;  // pulsed on the last applied tick
    };
    Entry& entry(StringView action);
    Array<Entry> entries_;
    bool overflow_logged_ = false;
};

// Binds every widget of `doc` that names a "bind" path (Document::binding) and is not bound yet:
// to `self` when it has the component, else to the first of `entities` that has it. Returns how
// many are still unbound because no entity has the component yet (call again later, e.g. once
// the game's Startup systems ran). A path that cannot resolve logs one error and counts as done.
u32 bind_document(Document& doc, const World& world, Entity self, Span<const Entity> entities);

}  // namespace mce::ui
