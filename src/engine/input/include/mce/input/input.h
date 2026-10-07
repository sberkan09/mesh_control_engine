#pragma once
// mce_input (L2): the per-tick InputState and the system that samples it (audio-input-ui.md §2.1).
//
// Tick-quantized sampling (§3.2, FR-INP-030): a tick's input is a pure function of event
// timestamps against the fixed tick grid, never of which render frame delivered them. Events are
// queued with their platform timestamp; sample_for_tick(N, close_us) folds, in timestamp order,
// every queued event with ts < close_us and leaves later ones for tick N+1. Edges come from
// transitions inside the window, so a press and release within one tick still reads as pressed.
//
// M1 skeleton: bindings are fixed (WASD/arrows move, Space jump, left mouse fire, Escape pause,
// Return confirm, Backspace cancel). Action maps, StringId lookup and replay arrive at M3.

#include "mce/app.h"
#include "mce/containers/bitset.h"
#include "mce/math/math.h"
#include "mce/platform/event.h"

namespace mce::input {

using math::vec2;

// The gameplay-facing snapshot for one tick. Immutable once sampled; plain data, no devices.
struct InputState {
    u64 tick = 0;          // sim tick this snapshot belongs to
    vec2 move;             // "Move": WASD / arrows, length <= 1, +y = up
    vec2 look;             // "Look": mouse motion folded in this tick's window (pixels)
    bool jump_held = false, jump_pressed = false, jump_released = false;  // "Jump"
    bool fire_held = false, fire_pressed = false;                         // "Fire"
    bool pause_pressed = false;                                           // "Pause"
    bool confirm_pressed = false, cancel_pressed = false;                 // UI navigation
    vec2 mouse_pos;        // window space, as of the window end
    vec2 mouse_wheel;      // wheel delta folded in this tick's window
};

struct InputStats {
    u64 events_queued = 0;
    u64 events_dropped = 0;  // queue overflow: oldest events dropped (never a stall)
    u64 ticks_sampled = 0;
};

class InputSystem final : public app::IInputSource {
public:
    static constexpr u32 kQueueCapacity = 1024;

    // Queues platform events. Each event's timestamp_us must be on the app clock's timeline
    // (platform::now_us()); events may arrive slightly out of order and are kept sorted.
    void push(const platform::Event& e);
    void push(Span<const platform::Event> events);

    // Folds every queued event with ts < tick_close_us into a new snapshot for `tick`.
    const InputState* sample_for_tick(u64 tick, u64 tick_close_us) override;

    const InputState& current() const { return state_; }
    const InputStats& stats() const { return stats_; }
    u32 queued() const { return count_; }

    // Raw device state for tools (editor camera, console); gameplay reads InputState only.
    bool key_held(platform::Key k) const;
    bool mouse_held(platform::MouseButton b) const;

private:
    void fold(const platform::Event& e);
    void release_all();

    platform::Event queue_[kQueueCapacity];
    u32 head_ = 0;   // index of the oldest queued event
    u32 count_ = 0;

    BitSet<256> keys_;        // physical keys held (USB HID usage)
    BitSet<256> keys_down_;   // went down inside the current window
    BitSet<256> keys_up_;     // went up inside the current window
    u32 mouse_held_ = 0;      // bit per MouseButton
    u32 mouse_down_ = 0;
    vec2 mouse_pos_;
    vec2 look_acc_;
    vec2 wheel_acc_;
    bool have_mouse_pos_ = false;

    InputState state_;
    InputStats stats_;
};

}  // namespace mce::input
