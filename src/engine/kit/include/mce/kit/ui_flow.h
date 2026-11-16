// kit::UiFlowRules (M-WORKFLOW 7, #382): the other half of UiFlow. UiFlow reacts to input actions;
// UiFlowRules reacts to what the game does: a signal (a sequence's `emit`, a timeline event key, a
// behavior tree's emit node, Luau `mce.signal`) or a component field crossing a value ("Health/hp
// below 1", "Score/lives equals 0"). A HUD that shows only in play, a game-over screen that appears
// when the last life goes, an ending that follows a cutscene's signal: all as scene data.
#pragma once

#include "mce/asset/handle.h"
#include "mce/fixed_string.h"
#include "mce/refl/macros.h"

namespace mce {
class Frame;
namespace input {
struct InputState;
}
}  // namespace mce

namespace mce::kit {

enum class UiFlowWhen : u8 { Action, Signal, FieldEquals, FieldAbove, FieldBelow };
MCE_ENUM(UiFlowWhen, Action, Signal, FieldEquals, FieldAbove, FieldBelow)

// What a rule does to the UiScreen on its entity (Load and Quit are the game's, like UiFlow's). SaveSlot writes the save slot named by
// the rule's `field` (empty: "quick"; 1 to 64 letters, digits, '_' or '-'); LoadSlot applies that slot after the tick, on top of the rule's
// `scene` (or the loaded one) when the slot exists, else it only logs. They run while paused, so a pause menu can save and load.
enum class UiFlowDo : u8 { None, Show, Hide, Toggle, Load, Quit, SaveSlot, LoadSlot };
MCE_ENUM(UiFlowDo, None, Show, Hide, Toggle, Load, Quit, SaveSlot, LoadSlot)

struct UiFlowRule {
    UiFlowWhen when = UiFlowWhen::Signal;
    UiFlowDo act = UiFlowDo::None;
    UiFlowDo act2 = UiFlowDo::None;
    u8 pad_ = 0;
    FixedString<32> name;   // Action: an input action. Signal: the signal id.
    FixedString<48> field;  // Field*: "Component/field" of a numeric or bool field. SaveSlot / LoadSlot: the slot name
    f32 value = 0.0f;       // Field*: what the field is compared with
    asset::AssetRef<asset::Scene> scene;  // Load: the scene to load
};
MCE_REFL_STRUCT(UiFlowRule, (when, Help{"Action: an input action pressed. Signal: a signal this tick carries. Field*: the field becomes equal, above or below 'value'"}),
                (act, Help{"what to do"}), (act2, Help{"and then this"}),
                (name, Help{"Action: the input action. Signal: the signal id (SeqSignal, TimelineSignal, BtSignal)"}),
                (field, Help{"Field*: \"Component/field\", read from the first entity that has the component"}),
                (value, Help{"Field*: the number the field is compared with"}),
                (scene, Help{"Load: the scene to load"}))

inline constexpr u32 kMaxUiFlowRules = 8;

// On the same entity as a UiScreen (and its UiFlow, which keeps working: its actions are evaluated
// first, then these rules in order). Signals act the tick after they were emitted, like every event.
// A Field rule fires when its condition turns true, not on every tick it holds: `armed` remembers
// which rules held last tick (not saved: a loaded game's screens are back at their authored state, so a
// condition that still holds fires again). A field rule whose condition already holds on the first tick fires then.
static_assert(kMaxUiFlowRules <= 8, "UiFlowRules::armed has one bit per rule");

struct UiFlowRules {
    UiFlowRule rules[kMaxUiFlowRules];
    u8 armed = 0;
    u8 pad_[7] = {};
};
MCE_COMPONENT(UiFlowRules, (rules, Help{"up to 8 rules, evaluated in order each tick, even while the world is held"}),
              (armed, ReadOnly, Help{"bit i: the Field rule i held last tick"}))

// Evaluates every UiFlowRules this tick (kit_ui_flow calls it after the UiFlow actions); a Load rule
// leaves its scene in `load` when none is set yet.
void apply_ui_flow_rules(Frame& f, const input::InputState* in, asset::Guid& load);

}  // namespace mce::kit
