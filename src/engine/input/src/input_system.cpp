#include "mce/input/input_system.h"

#include <cmath>
#include <cstring>

#include "mce/input/replay.h"
#include "mce/log.h"
#include "mce/platform/platform.h"

namespace mce::input {

using platform::Event;
using platform::EventType;

namespace {

bool is_input_event(EventType t) {
    switch (t) {
        case EventType::KeyDown:
        case EventType::KeyUp:
        case EventType::MouseMove:
        case EventType::MouseButtonDown:
        case EventType::MouseButtonUp:
        case EventType::MouseWheel:
        case EventType::TextInput:
        case EventType::FocusOut:
            return true;
        default:
            return false;
    }
}

// Presses captured by the editor UI never reach the game; releases always do, so a key pressed
// in-game and released over an ImGui field cannot stick (§4.6).
bool is_press(EventType t) {
    return t == EventType::KeyDown || t == EventType::MouseButtonDown || t == EventType::TextInput ||
           t == EventType::MouseWheel || t == EventType::MouseMove;
}

f32 apply_deadzone(f32 v, Deadzone dz) {
    const f32 mag = std::fabs(v);
    if (mag <= dz.inner) return 0.f;
    const f32 t = mag >= dz.outer ? 1.f : (mag - dz.inner) / (dz.outer - dz.inner);
    return v < 0.f ? -t : t;
}

f32 apply_curve(f32 v, Curve c) {
    const f32 a = std::fabs(v);
    f32 r = a;
    switch (c) {
        case Curve::Linear: r = a; break;
        case Curve::Smooth: r = a * a * (3.f - 2.f * a); break;
        case Curve::Quadratic: r = a * a; break;
        case Curve::Cubic: r = a * a * a; break;
    }
    return v < 0.f ? -r : r;
}

bool bounded(Device d) { return d == Device::Keyboard || d == Device::MouseButton; }

// Appends one code point as UTF-8; drops it whole when it does not fit.
void append_utf8(char* buf, u32& len, u32 cp) {
    char tmp[4];
    u32 n;
    if (cp < 0x80) {
        tmp[0] = static_cast<char>(cp);
        n = 1;
    } else if (cp < 0x800) {
        tmp[0] = static_cast<char>(0xC0 | (cp >> 6));
        tmp[1] = static_cast<char>(0x80 | (cp & 0x3F));
        n = 2;
    } else if (cp < 0x10000) {
        tmp[0] = static_cast<char>(0xE0 | (cp >> 12));
        tmp[1] = static_cast<char>(0x80 | ((cp >> 6) & 0x3F));
        tmp[2] = static_cast<char>(0x80 | (cp & 0x3F));
        n = 3;
    } else {
        tmp[0] = static_cast<char>(0xF0 | (cp >> 18));
        tmp[1] = static_cast<char>(0x80 | ((cp >> 12) & 0x3F));
        tmp[2] = static_cast<char>(0x80 | ((cp >> 6) & 0x3F));
        tmp[3] = static_cast<char>(0x80 | (cp & 0x3F));
        n = 4;
    }
    if (len + n > kMaxTextBytes) return;
    std::memcpy(buf + len, tmp, n);
    len += n;
}

}  // namespace

InputSystem::InputSystem(mem::IAllocator& a) : alloc_(a), set_(a), queue_(a) {
    queue_.reserve(kQueueCapacity);
    origin_us_ = platform::now_us();
}

// ---------------------------------------------------------------------------- maps

Result<void> InputSystem::load_map(Span<const byte> chunk) {
    InputMapSet parsed(alloc_);
    MCE_TRY(decode_input_map(chunk, parsed));
    set_ = std::move(parsed);
    stack_size_ = 0;
    stack_[stack_size_++] = 0;
    // Hot reload resets held state: a one-tick grace instead of stale edges.
    reset_devices();
    prev_ = InputState{};
    cur_ = InputState{};
    return ok();
}

bool InputSystem::push_map(StringId name) {
    for (u32 i = 0; i < set_.maps.size(); ++i) {
        if (set_.maps[i].name != name) continue;
        if (stack_size_ == kMaxMapStack) {
            MCE_LOG_ERROR("input: map stack full ({} maps)", kMaxMapStack);
            return false;
        }
        stack_[stack_size_++] = i;
        return true;
    }
    MCE_LOG_ERROR("input: push_map of unknown map (id {:#018x})", name.value);
    return false;
}

void InputSystem::pop_map() {
    MCE_ASSERT_MSG(stack_size_ > 0, "input: pop_map on an empty map stack");
    if (stack_size_ > 0) --stack_size_;
}

bool InputSystem::rebind(StringId map, StringId action, u32 index, const BindingDef& binding) {
    const ActionMapDef* m = set_.find_map(map);
    if (!m) return false;
    for (u32 i = 0; i < m->action_count; ++i) {
        const ActionDef& a = set_.actions[m->first_action + i];
        if (a.name != action) continue;
        if (index >= a.binding_count) return false;
        set_.bindings[a.first_binding + index] = binding;
        return true;
    }
    return false;
}

// ---------------------------------------------------------------------------- clock

void InputSystem::set_tick_rate(u32 hz) {
    MCE_ASSERT_MSG(hz > 0, "input: tick rate must be positive");
    tick_hz_ = hz;
}

void InputSystem::set_tick_origin_us(u64 origin_us) { origin_us_ = origin_us; }

u64 InputSystem::tick_close_us(u64 tick) const {
    // Exact rational boundary: no integer period exists for 60 Hz (16666.67 µs).
    return origin_us_ + (tick * 1'000'000ull) / tick_hz_;
}

// ---------------------------------------------------------------------------- events

void InputSystem::begin_frame(Span<const Event> events) {
    for (const Event& e : events) push_event(e);
}

void InputSystem::push_event(const Event& e) {
    if (!is_input_event(e.type) || ((e.flags & platform::EventFlag::ConsumedByEditor) && is_press(e.type))) {
        ++stats_.events_ignored;
        return;
    }
    if (queue_.size() - queue_head_ >= kQueueCapacity) {
        // Overflow: drop the oldest, never stall. Log once; the counter keeps the total.
        if (stats_.events_dropped++ == 0) MCE_LOG_ERROR("input: event queue overflow; dropping oldest events");
        ++queue_head_;
    }
    // Compact the consumed prefix instead of growing (the queue never reallocates after reserve).
    if (queue_head_ > 0 && queue_.size() == queue_.capacity()) {
        const usize live = queue_.size() - queue_head_;
        std::memmove(static_cast<void*>(queue_.data()), queue_.data() + queue_head_, live * sizeof(Event));
        queue_.resize(live);
        queue_head_ = 0;
    }
    queue_.push_back(e);
    // Keep timestamp order (stable): events normally arrive sorted, so this is O(1).
    usize i = queue_.size() - 1;
    while (i > queue_head_ && queue_[i - 1].timestamp_us > queue_[i].timestamp_us) {
        const Event t = queue_[i - 1];
        queue_[i - 1] = queue_[i];
        queue_[i] = t;
        --i;
    }
}

void InputSystem::reset_devices() {
    keys_.clear();
    keys_tapped_.clear();
    keys_release_pending_.clear();
    buttons_.clear();
    buttons_tapped_.clear();
    buttons_release_pending_.clear();
}

void InputSystem::fold(const Event& e) {
    ++stats_.events_folded;
    switch (e.type) {
        case EventType::KeyDown: {
            if (e.flags & platform::EventFlag::Repeat) break;  // OS auto-repeat is not a new press
            if (e.code >= kKeyCount) break;
            keys_.set(e.code);
            keys_tapped_.set(e.code);
            keys_release_pending_.reset(e.code);
            break;
        }
        case EventType::KeyUp: {
            if (e.code >= kKeyCount) break;
            // A press and release inside one tick window still shows for one tick (the
            // 1-tick visibility floor): the release lands at the start of the next tick.
            if (keys_tapped_.test(e.code)) keys_release_pending_.set(e.code);
            else keys_.reset(e.code);
            break;
        }
        case EventType::MouseButtonDown: {
            if (e.code >= kMouseButtons) break;
            buttons_.set(e.code);
            buttons_tapped_.set(e.code);
            buttons_release_pending_.reset(e.code);
            break;
        }
        case EventType::MouseButtonUp: {
            if (e.code >= kMouseButtons) break;
            if (buttons_tapped_.test(e.code)) buttons_release_pending_.set(e.code);
            else buttons_.reset(e.code);
            break;
        }
        case EventType::MouseMove: {
            const vec2 p(e.x, e.y);
            if (have_mouse_pos_) {
                tick_delta_.x += p.x - mouse_pos_.x;
                tick_delta_.y += p.y - mouse_pos_.y;
            }
            mouse_pos_ = p;
            have_mouse_pos_ = true;
            break;
        }
        case EventType::MouseWheel:
            tick_wheel_.x += e.x;
            tick_wheel_.y += e.y;
            break;
        case EventType::TextInput:
            append_utf8(tick_text_, tick_text_len_, e.code);
            break;
        case EventType::FocusOut:
            // Stuck-key fix: everything reads released from here on; the release edges fire on
            // this tick (FR-RUN-060, §4.4).
            reset_devices();
            break;
        default:
            break;
    }
}

// ---------------------------------------------------------------------------- sampling

const InputState& InputSystem::sample_for_tick(u64 tick) { return *sample_for_tick(tick, tick_close_us(tick)); }

const InputState* InputSystem::sample_for_tick(u64 tick, u64 close_us) {
    MCE_ASSERT_MSG(tick > last_tick_, "input: ticks must be sampled in increasing order");
    last_tick_ = tick;

    // Releases deferred from taps in the previous window land now.
    for (u32 k = 0; k < kKeyCount; ++k)
        if (keys_release_pending_.test(k)) keys_.reset(k);
    for (u32 b = 0; b < kMouseButtons; ++b)
        if (buttons_release_pending_.test(b)) buttons_.reset(b);
    keys_release_pending_.clear();
    keys_tapped_.clear();
    buttons_release_pending_.clear();
    buttons_tapped_.clear();
    tick_wheel_ = vec2();
    tick_delta_ = vec2();
    tick_text_len_ = 0;

    // Fold, in timestamp order, everything stamped before this tick's close.
    while (queue_head_ < queue_.size() && queue_[queue_head_].timestamp_us < close_us) fold(queue_[queue_head_++]);
    if (queue_head_ == queue_.size()) {
        queue_.clear();
        queue_head_ = 0;
    }

    prev_ = cur_;
    if (replayer_) {
        InputState replayed;
        if (replayer_->next(replayed)) {
            cur_ = replayed;
        } else {
            cur_ = InputState{};
            cur_.tick = tick;
        }
    } else {
        cur_ = InputState{};
        cur_.tick = tick;
        evaluate(cur_, prev_);
    }
    if (recorder_) recorder_->record(cur_);
    return &cur_;
}

f32 InputSystem::binding_value(const BindingDef& b) const {
    f32 v = 0.f;
    switch (b.device) {
        case Device::Keyboard: v = (b.code < kKeyCount && keys_.test(b.code)) ? 1.f : 0.f; break;
        case Device::MouseButton: v = (b.code < kMouseButtons && buttons_.test(b.code)) ? 1.f : 0.f; break;
        case Device::MouseWheel: v = b.axis == 0 ? tick_wheel_.x : tick_wheel_.y; break;
        case Device::MouseDelta: v = b.axis == 0 ? tick_delta_.x : tick_delta_.y; break;
    }
    if (bounded(b.device)) v = apply_curve(apply_deadzone(v, b.dz), b.curve);
    if (b.flags & BindingFlag::Invert) v = -v;
    return v * b.scale;
}

void InputSystem::evaluate(InputState& out, const InputState& prev) const {
    const f32 dt = 1.f / static_cast<f32>(tick_hz_);

    // Top of the stack first: a name already produced by a higher map is shadowed below.
    for (u32 s = stack_size_; s-- > 0;) {
        const ActionMapDef& m = set_.maps[stack_[s]];
        for (u32 ai = 0; ai < m.action_count; ++ai) {
            const ActionDef& def = set_.actions[m.first_action + ai];
            if (out.has_action(def.name)) continue;
            if (out.action_count == kMaxActions) {
                MCE_ASSERT_MSG(false, "input: more than kMaxActions actions across the map stack");
                break;
            }

            f32 x = 0.f, y = 0.f;
            bool all_bounded = true;
            for (u32 bi = 0; bi < def.binding_count; ++bi) {
                const BindingDef& b = set_.bindings[def.first_binding + bi];
                all_bounded = all_bounded && bounded(b.device);
                const f32 v = binding_value(b);
                if (def.type == ActionType::Axis2D && b.axis == 1) y += v;
                else x += v;
            }

            ActionState st;
            switch (def.type) {
                case ActionType::Button:
                    st.held = std::fabs(x) >= 0.5f;
                    st.x = st.held ? 1.f : 0.f;
                    break;
                case ActionType::Axis1D:
                    if (all_bounded) x = math::clamp(x, -1.f, 1.f);
                    st.x = x;
                    st.held = x != 0.f;
                    break;
                case ActionType::Axis2D: {
                    // Opposing keys cancel; diagonals clamp to the unit circle (WASD speed).
                    if (all_bounded) {
                        const f32 len2 = x * x + y * y;
                        if (len2 > 1.f) {
                            const f32 inv = 1.f / std::sqrt(len2);
                            x *= inv;
                            y *= inv;
                        }
                    }
                    st.x = x;
                    st.y = y;
                    st.held = x != 0.f || y != 0.f;
                    break;
                }
            }

            const ActionState* before = nullptr;
            for (u32 p = 0; p < prev.action_count; ++p)
                if (prev.action_names[p] == def.name) before = &prev.actions[p];
            const bool was_held = before && before->held;
            st.pressed = st.held && !was_held;
            st.released = !st.held && was_held;
            st.held_time = st.held ? (was_held ? before->held_time + dt : dt) : 0.f;

            out.action_names[out.action_count] = def.name;
            out.actions[out.action_count] = st;
            ++out.action_count;
        }
        if (m.flags & MapFlag::BlocksBelow) break;
    }

    // Fixed fast-path fields (§3.1): the starter kit's universal actions by name.
    auto find = [&](StringId n) -> const ActionState* {
        for (u32 i = 0; i < out.action_count; ++i)
            if (out.action_names[i] == n) return &out.actions[i];
        return nullptr;
    };
    if (const ActionState* a = find(MCE_SID("Move"))) out.move = vec2(a->x, a->y);
    if (const ActionState* a = find(MCE_SID("Look"))) out.look = vec2(a->x, a->y);
    if (const ActionState* a = find(MCE_SID("TriggerL"))) out.trigger_l = a->x;
    if (const ActionState* a = find(MCE_SID("TriggerR"))) out.trigger_r = a->x;
    if (const ActionState* a = find(MCE_SID("Jump"))) {
        out.jump_held = a->held;
        out.jump_pressed = a->pressed;
        out.jump_released = a->released;
    }
    if (const ActionState* a = find(MCE_SID("Fire"))) out.fire_pressed = a->pressed;
    if (const ActionState* a = find(MCE_SID("Pause"))) out.pause_pressed = a->pressed;
    if (const ActionState* a = find(MCE_SID("Confirm"))) out.confirm_pressed = a->pressed;
    if (const ActionState* a = find(MCE_SID("Cancel"))) out.cancel_pressed = a->pressed;

    out.mouse_pos = mouse_pos_;
    out.wheel = tick_wheel_;
    out.text_len = tick_text_len_;
    std::memcpy(out.text_buf, tick_text_, tick_text_len_);
}

// ---------------------------------------------------------------------------- raw state

bool InputSystem::key_held(platform::Key k) const {
    const u32 c = static_cast<u32>(k);
    return c < kKeyCount && keys_.test(c);
}

bool InputSystem::mouse_held(platform::MouseButton b) const {
    const u32 c = static_cast<u32>(b);
    return c < kMouseButtons && buttons_.test(c);
}

}  // namespace mce::input
