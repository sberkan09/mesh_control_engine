#pragma once
// InputSystem: device state + tick-quantized sampling + action evaluation (audio-input-ui §2.1,
// §3.1–3.2). Engine-internal API — the run loop and hosts call this; gameplay reads InputState.
//
// Per render frame:   input.begin_frame(events)          (events stamped by mce_platform)
// Per sim tick N:     const InputState& s = input.sample_for_tick(N)
//
// Tick N (1-based, as app::TickContext::tick_index) owns the half-open window
// [origin + (N-1)·T, origin + N·T), T = 1/tick_hz, boundaries computed exactly as
// origin + N·1'000'000 / tick_hz (no float drift). sample_for_tick folds, in timestamp order,
// every queued event stamped before tick N's close; later events wait for N+1. A tick's input is
// therefore independent of render pacing (FR-INP-030). The origin must not be later than the
// fixed-step accumulator's start (set it when the loop starts, or leave the default: the moment
// the InputSystem was constructed). Under app::run, the loop passes each tick's close itself.

#include "mce/app.h"
#include "mce/containers/array.h"
#include "mce/containers/bitset.h"
#include "mce/input/bindings.h"
#include "mce/input/input_map.h"

namespace mce::input {

class InputRecorder;
class InputReplayer;

// Runtime rebinding (FR-INP-040): which devices a capture accepts, and where it stands.
namespace RebindAccept {
enum : u8 { Keyboard = 1u << 0, Mouse = 1u << 1, Gamepad = 1u << 2, All = 0x7 };
}
enum class RebindState : u8 { Idle, Waiting, Captured, TimedOut, Cancelled };

// One-hand keyboard layouts (M-ACCESS step 3): Left keeps every keyboard binding on WASD and the
// keys around it, Right on the arrows, Enter, the right modifiers and the keys left of them.
enum class Hand : u8 { Both, Left, Right };

struct InputStats {
    u64 events_folded = 0;
    u64 events_dropped = 0;  // queue overflow (oldest dropped)
    u64 events_ignored = 0;  // captured by the editor or not input-relevant
};

// Implements core's app::IInputSource: set AppDesc::input_source = &input and the run loop hands
// every tick its window close on the app clock (shifted by time the clamp/time_scale skipped).
class InputSystem final : public app::IInputSource {
public:
    static constexpr u32 kQueueCapacity = 1024;
    static constexpr u32 kMaxMapStack = 8;

    // Touch gestures (FR-INP-070). Distances are in units of the window's shorter side, so a stick
    // or a swipe feels the same in portrait and landscape; times come from event timestamps, so a
    // gesture never depends on render pacing.
    static constexpr f32 kTouchStickRadius = 0.12f;   // full deflection
    static constexpr f32 kTapMaxTravel = 0.03f;       // a tap moves less than this...
    static constexpr u64 kTapMaxUs = 250'000;          // ...and lifts within this
    static constexpr f32 kSwipeMinTravel = 0.08f;     // a swipe moves at least this...
    static constexpr u64 kSwipeMaxUs = 500'000;        // ...within this

    explicit InputSystem(mem::IAllocator& a = mem::heap());
    InputSystem(const InputSystem&) = delete;
    InputSystem& operator=(const InputSystem&) = delete;

    // ---- maps ------------------------------------------------------------------------
    // Loads a cooked input-map chunk, replacing every map. The stack is reset to the first map
    // in the chunk; held state and edges reset (one-tick grace on hot reload, §4.2).
    Result<void> load_map(Span<const byte> chunk);
    // Overlay maps: the top map's actions shadow same-named actions below it; a map flagged
    // BlocksBelow hides everything beneath. Returns false for an unknown map / full stack.
    bool push_map(StringId map_name);
    void pop_map();
    Span<const u32> map_stack() const { return Span<const u32>(stack_, stack_size_); }
    const InputMapSet& maps() const { return set_; }
    // In-session rebinding (FR-INP-020): replace binding `index` of `action` in `map`. Applies
    // from the next sampled tick and is recorded as an override (persisted by save_bindings).
    bool rebind(StringId map, StringId action, u32 index, const BindingDef& binding);

    // ---- runtime rebinding + persistence (FR-INP-040, v0.5) --------------------------------------
    // Options-screen capture: the next press on an accepted device inside a sampled tick becomes
    // binding `index` of `action` (keeping that binding's axis and sign; a stick takes its
    // direction from the push). The capturing press is swallowed, so it never fires the action.
    // Escape always cancels (it cannot itself be captured), and the capture times out after
    // `timeout_s` of sim time. Returns false for an unknown map/action/index.
    bool begin_rebind(StringId map, StringId action, u32 index, f32 timeout_s = 5.f, u8 accept = RebindAccept::All);
    void cancel_rebind();
    RebindState rebind_state() const { return rebind_.state; }
    const BindingDef& rebind_result() const { return rebind_.result; }  // valid when Captured

    // Every binding that differs from the cooked map (one per map/action/index).
    Span<const BindingOverride> binding_overrides() const { return overrides_.span(); }
    void reset_bindings();  // back to the cooked map, overrides cleared
    // Reflection-serialized text (bindings.h). deserialize replaces the overrides and applies them;
    // overrides naming something the current map lacks are dropped with one warning.
    void serialize_bindings(Array<char>& out) const;
    Result<void> deserialize_bindings(StringView text);
    Result<void> save_bindings(StringView path) const;  // atomic: temp file + rename
    Result<void> load_bindings(StringView path);        // FileNotFound when there is no save yet

    // ---- player options (M-ACCESS step 3) ----------------------------------------------------------
    // Look sensitivity: the "Look" action's value (mouse, stick or touch) scaled after its clamp.
    void set_look_sensitivity(f32 s) { look_sensitivity_ = s > 0.f ? s : 0.f; }
    f32 look_sensitivity() const { return look_sensitivity_; }
    // Hold-to-toggle: a Button action marked here latches. One press turns it held, the next press
    // releases it, so the player never has to keep a key down. pressed/released follow the latch.
    void set_toggle(StringId action, bool on);
    bool is_toggle(StringId action) const;
    void clear_toggles();
    // A one-hand keyboard layout over the cooked map (Both = the cooked map): each map's keyboard
    // bindings move to the hand's keys; Escape stays, mouse, gamepad and touch bindings stay.
    // Recorded as overrides like any rebind, so save_bindings keeps it.
    void apply_hand_preset(Hand hand);
    // Bumped by every binding change (rebind, capture, reset, preset, deserialize): a host saves the
    // bindings when it moves.
    u64 bindings_generation() const { return bindings_generation_; }

    // ---- clock -----------------------------------------------------------------------
    void set_tick_rate(u32 hz);
    void set_tick_origin_us(u64 origin_us);
    u64 tick_origin_us() const { return origin_us_; }
    u64 tick_close_us(u64 tick) const;

    // The window size in pixels, for the aspect of touch distances (normalized coordinates are
    // stretched by it). A Resized event in the queue updates it at its timestamp too.
    void set_view_size(f32 width_px, f32 height_px);

    // ---- per frame / per tick ----------------------------------------------------------
    // Queues one render frame's events (any order; kept sorted by timestamp).
    void begin_frame(Span<const platform::Event> events);
    void push_event(const platform::Event& e);
    const InputState& sample_for_tick(u64 tick);
    const InputState* sample_for_tick(u64 tick, u64 tick_close_us) override;
    const InputState& state() const { return cur_; }
    usize queued_events() const { return queue_.size(); }

    // ---- raw device state (§2.2: editor camera, tools; NOT for gameplay) ------------------
    bool key_held(platform::Key k) const;
    bool mouse_held(platform::MouseButton b) const;
    vec2 mouse_pos() const { return mouse_pos_; }
    bool gamepad_connected(u32 slot) const { return slot < platform::kMaxGamepads && pads_[slot].connected; }
    bool gamepad_held(u32 slot, platform::GamepadButton b) const;
    f32 gamepad_axis(u32 slot, platform::GamepadAxis a) const;
    bool touch_held(u32 slot) const { return slot < platform::kMaxTouches && touches_[slot].down; }
    vec2 touch_pos(u32 slot) const { return slot < platform::kMaxTouches ? touches_[slot].pos : vec2(); }

    // ---- record / replay (FR-RUN-070) --------------------------------------------------------
    // While a recorder is set every sampled tick is appended to it. While a replayer is set,
    // sample_for_tick ignores devices and returns the recorded stream (events still drain).
    void set_recorder(InputRecorder* r) { recorder_ = r; }
    void set_replayer(InputReplayer* r) { replayer_ = r; }

    const InputStats& stats() const { return stats_; }

private:
    static constexpr u32 kKeyCount = 512;
    static constexpr u32 kMouseButtons = 8;

    void reset_devices();
    void fold(const platform::Event& e);
    bool try_capture(const platform::Event& e);  // rebind capture; true = event swallowed
    BindingDef* find_binding(StringId map, StringId action, u32 index, const ActionDef** def = nullptr);
    void set_override(StringId map, StringId action, u32 index, const BindingDef& b);
    void apply_overrides();
    void evaluate(InputState& out, const InputState& prev) const;
    f32 binding_value(const BindingDef& b) const;
    void end_touch(u32 slot, vec2 pos, u64 timestamp_us, bool canceled);
    vec2 touch_units(vec2 normalized_delta) const;  // to shorter-side units

    mem::IAllocator& alloc_;
    InputMapSet set_;
    Array<BindingDef> cooked_bindings_;  // the map as loaded, for reset and override diffs
    Array<BindingOverride> overrides_;
    u32 stack_[kMaxMapStack] = {};
    u32 stack_size_ = 0;

    u32 tick_hz_ = 60;
    u64 origin_us_ = 0;
    u64 last_tick_ = 0;

    Array<platform::Event> queue_;  // sorted by timestamp; front = oldest
    usize queue_head_ = 0;

    // Device state as of the last folded event.
    BitSet<kKeyCount> keys_;
    BitSet<kKeyCount> keys_tapped_;         // went down during the current tick window
    BitSet<kKeyCount> keys_release_pending_;  // tapped and released inside one window: release next tick
    BitSet<kMouseButtons> buttons_;
    BitSet<kMouseButtons> buttons_tapped_;
    BitSet<kMouseButtons> buttons_release_pending_;
    vec2 mouse_pos_;
    bool have_mouse_pos_ = false;
    vec2 tick_wheel_;
    vec2 tick_delta_;
    char tick_text_[kMaxTextBytes] = {};
    u32 tick_text_len_ = 0;

    static constexpr u32 kPadButtons = static_cast<u32>(platform::GamepadButton::Count);
    static constexpr u32 kPadAxes = static_cast<u32>(platform::GamepadAxis::Count);
    struct PadState {
        bool connected = false;
        BitSet<kPadButtons> buttons, tapped, release_pending;
        f32 axes[kPadAxes] = {};
    };
    PadState pads_[platform::kMaxGamepads];

    struct TouchState {
        bool down = false;
        bool tapped = false;           // went down during the current tick window
        bool release_pending = false;  // went down and up inside one window: lifts next tick
        bool released = false;         // lifted this tick (reported once in the snapshot)
        vec2 origin, pos;
        u64 start_us = 0;
        u64 seq = 0;  // start order: the oldest touch in a zone drives its stick
    };
    TouchState touches_[platform::kMaxTouches];
    u64 touch_seq_ = 0;
    struct Gesture {
        vec2 origin;
        vec2 dir;  // swipe: unit vector on its dominant axis; tap: zero
        bool swipe = false;
    };
    Gesture tick_gestures_[platform::kMaxTouches];
    u32 tick_gesture_count_ = 0;
    vec2 view_px_{1.f, 1.f};

    struct Rebind {
        RebindState state = RebindState::Idle;
        StringId map, action;
        u32 index = 0;
        u8 accept = RebindAccept::All;
        u64 deadline_tick = 0;
        bool button_action = true;
        BindingDef result;
    } rebind_;

    f32 look_sensitivity_ = 1.f;
    Array<StringId> toggles_;
    mutable Array<StringId> toggle_raw_held_;  // toggle actions whose bindings were held last tick
    u64 bindings_generation_ = 0;

    InputState cur_;
    InputState prev_;
    InputRecorder* recorder_ = nullptr;
    InputReplayer* replayer_ = nullptr;
    InputStats stats_;
};

}  // namespace mce::input
