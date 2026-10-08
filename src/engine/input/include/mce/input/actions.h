#pragma once
// mce_input/actions.h — the only input header gameplay code includes (audio-input-ui §2.1).
//
// InputState is the per-tick snapshot: a pure function of event timestamps against the fixed
// tick grid (FR-INP-030), immutable once sampled, registered as a World resource and carried in
// app::TickContext (alias app::ActionSnapshot). It is a flat value type so the replay recorder
// can store and compare it.

#include "mce/math/math.h"
#include "mce/string_id.h"

namespace mce::input {

using math::vec2;

enum class ActionType : u8 { Button, Axis1D, Axis2D };

inline constexpr u32 kMaxActions = 64;     // evaluated actions per tick, across the map stack
inline constexpr u32 kMaxTextBytes = 64;   // UTF-8 text accumulated per tick

struct ActionState {  // one action's value this tick
    f32 x = 0.f;           // Button: 0|1 (held). Axis1D: x. Axis2D: x,y
    f32 y = 0.f;
    f32 held_time = 0.f;   // seconds held (actuated) as of this tick, sim time
    bool held = false;     // Button: down. Axis: non-zero
    bool pressed = false;  // edge: became held THIS tick
    bool released = false; // edge: stopped being held THIS tick
};

struct InputState {
    u64 tick = 0;  // sim tick this snapshot is bound to

    // Fixed fast-path fields, filled from the actions of the same name ("Move", "Look", "Jump",
    // "Fire", "Pause", "Confirm", "Cancel", "TriggerL", "TriggerR"). Not a second system:
    // a game that does not define one of those actions simply reads zeros.
    vec2 move;
    vec2 look;
    f32 trigger_l = 0.f, trigger_r = 0.f;  // v0.5 analog
    bool jump_held = false, jump_pressed = false, jump_released = false;
    bool fire_pressed = false;
    bool pause_pressed = false;
    bool confirm_pressed = false, cancel_pressed = false;

    // Named access (remap-proof; game-defined actions). Unknown names return a zero state and
    // log one error (FR-DX-050 taxonomy: safe default + one error, never a crash).
    const ActionState& action(StringId name) const;
    bool has_action(StringId name) const;
    bool pressed(StringId name) const { return action(name).pressed; }
    bool released(StringId name) const { return action(name).released; }
    bool held(StringId name) const { return action(name).held; }
    f32 axis(StringId name) const { return action(name).x; }
    vec2 axis2(StringId name) const {
        const ActionState& a = action(name);
        return {a.x, a.y};
    }

    // Text typed during this tick's window (UTF-8, not NUL-terminated).
    StringView text() const { return StringView(text_buf, text_len); }
    vec2 mouse_pos_px() const { return mouse_pos; }  // window space, end of tick window
    vec2 mouse_wheel() const { return wheel; }       // accumulated over the tick window

    // ---- storage (public so the recorder/replayer and tests can see it; treat as read-only) ----
    vec2 mouse_pos;
    vec2 wheel;
    u32 action_count = 0;
    u32 text_len = 0;
    char text_buf[kMaxTextBytes] = {};
    StringId action_names[kMaxActions] = {};
    ActionState actions[kMaxActions] = {};
};

// Field-by-field equality over the live part of the snapshot (replay/determinism tests).
bool operator==(const InputState& a, const InputState& b);

// FNV-1a hash of the sim-visible content (feeds the determinism harness).
u64 hash_state(const InputState& s, u64 seed = kFnv64Offset);

}  // namespace mce::input

// core.md §2.1's app::ActionSnapshot is an alias of InputState, declared in mce/app.h.
