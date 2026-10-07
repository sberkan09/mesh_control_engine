#include "mce/input/input.h"

#include "mce/log.h"

namespace mce::input {
namespace {

using platform::Event;
using platform::EventType;
using platform::Key;
using platform::MouseButton;

constexpr u32 key_index(Key k) { return static_cast<u32>(k); }
constexpr u32 button_bit(MouseButton b) { return 1u << static_cast<u32>(b); }

}  // namespace

void InputSystem::push(const Event& e) {
    if (count_ == kQueueCapacity) {
        head_ = (head_ + 1) % kQueueCapacity;  // drop the oldest
        --count_;
        if (stats_.events_dropped++ == 0) MCE_LOG_ERROR("input: event queue overflow, dropping oldest events");
    }
    // Insert keeping timestamp order; ties keep arrival order. Events are nearly sorted, so this
    // walks back a step or two at most.
    u32 pos = count_;
    while (pos > 0) {
        const Event& prev = queue_[(head_ + pos - 1) % kQueueCapacity];
        if (prev.timestamp_us <= e.timestamp_us) break;
        queue_[(head_ + pos) % kQueueCapacity] = prev;
        --pos;
    }
    queue_[(head_ + pos) % kQueueCapacity] = e;
    ++count_;
    ++stats_.events_queued;
}

void InputSystem::push(Span<const Event> events) {
    for (const Event& e : events) push(e);
}

bool InputSystem::key_held(Key k) const {
    const u32 i = key_index(k);
    return i < 256 && keys_.test(i);
}

bool InputSystem::mouse_held(MouseButton b) const { return (mouse_held_ & button_bit(b)) != 0; }

void InputSystem::release_all() {
    // Focus loss: everything held is released now, so nothing sticks down (FR-INP-010).
    for (u32 i = 0; i < 256; ++i) {
        if (keys_.test(i)) keys_up_.set(i);
    }
    keys_.clear();
    mouse_held_ = 0;
}

void InputSystem::fold(const Event& e) {
    switch (e.type) {
        case EventType::KeyDown: {
            if (e.code >= 256 || (e.flags & platform::EventFlag::Repeat)) break;
            if (!keys_.test(e.code)) keys_down_.set(e.code);
            keys_.set(e.code);
            break;
        }
        case EventType::KeyUp:
            if (e.code >= 256) break;
            if (keys_.test(e.code)) keys_up_.set(e.code);
            keys_.reset(e.code);
            break;
        case EventType::MouseButtonDown:
            if (e.code < 32) {
                if (!(mouse_held_ & (1u << e.code))) mouse_down_ |= 1u << e.code;
                mouse_held_ |= 1u << e.code;
            }
            break;
        case EventType::MouseButtonUp:
            if (e.code < 32) mouse_held_ &= ~(1u << e.code);
            break;
        case EventType::MouseMove:
            if (have_mouse_pos_) {
                look_acc_.x += e.x - mouse_pos_.x;
                look_acc_.y += e.y - mouse_pos_.y;
            }
            mouse_pos_ = vec2{e.x, e.y};
            have_mouse_pos_ = true;
            break;
        case EventType::MouseWheel:
            wheel_acc_.x += e.x;
            wheel_acc_.y += e.y;
            break;
        case EventType::FocusOut:
        case EventType::Minimized:
            release_all();
            break;
        default:
            break;
    }
}

const InputState* InputSystem::sample_for_tick(u64 tick, u64 tick_close_us) {
    keys_down_.clear();
    keys_up_.clear();
    mouse_down_ = 0;
    look_acc_ = vec2{};
    wheel_acc_ = vec2{};

    while (count_ > 0 && queue_[head_].timestamp_us < tick_close_us) {
        fold(queue_[head_]);
        head_ = (head_ + 1) % kQueueCapacity;
        --count_;
    }

    auto held = [&](Key k) { return keys_.test(key_index(k)); };
    auto down = [&](Key k) { return keys_down_.test(key_index(k)); };
    auto up = [&](Key k) { return keys_up_.test(key_index(k)); };

    InputState s;
    s.tick = tick;
    const f32 right = (held(Key::D) || held(Key::Right)) ? 1.0f : 0.0f;
    const f32 left = (held(Key::A) || held(Key::Left)) ? 1.0f : 0.0f;
    const f32 upv = (held(Key::W) || held(Key::Up)) ? 1.0f : 0.0f;
    const f32 downv = (held(Key::S) || held(Key::Down)) ? 1.0f : 0.0f;
    s.move = vec2{right - left, upv - downv};
    if (s.move.x != 0.0f && s.move.y != 0.0f) {
        constexpr f32 kInvSqrt2 = 0.70710678118654752f;
        s.move = vec2{s.move.x * kInvSqrt2, s.move.y * kInvSqrt2};
    }
    s.look = look_acc_;
    s.jump_held = held(Key::Space);
    s.jump_pressed = down(Key::Space);
    s.jump_released = up(Key::Space);
    s.fire_held = (mouse_held_ & button_bit(MouseButton::Left)) != 0;
    s.fire_pressed = (mouse_down_ & button_bit(MouseButton::Left)) != 0;
    s.pause_pressed = down(Key::Escape);
    s.confirm_pressed = down(Key::Return);
    s.cancel_pressed = down(Key::Backspace);
    s.mouse_pos = mouse_pos_;
    s.mouse_wheel = wheel_acc_;

    state_ = s;
    ++stats_.ticks_sampled;
    return &state_;
}

}  // namespace mce::input
