#include "mce/input/input_system.h"

#include <cmath>
#include <cstdio>
#include <cstring>

#include "mce/input/replay.h"
#include "mce/fixed_string.h"
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
        case EventType::GamepadAdded:
        case EventType::GamepadRemoved:
        case EventType::GamepadButtonDown:
        case EventType::GamepadButtonUp:
        case EventType::GamepadAxis:
        case EventType::TouchDown:
        case EventType::TouchMove:
        case EventType::TouchUp:
        case EventType::Resized:
            return true;
        default:
            return false;
    }
}

// Presses captured by the editor UI never reach the game; releases always do, so a key pressed
// in-game and released over an ImGui field cannot stick (§4.6).
bool is_press(EventType t) {
    return t == EventType::KeyDown || t == EventType::MouseButtonDown || t == EventType::TextInput ||
           t == EventType::MouseWheel || t == EventType::MouseMove || t == EventType::TouchDown ||
           t == EventType::TouchMove;
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

bool bounded(Device d) { return d != Device::MouseWheel && d != Device::MouseDelta; }

constexpr f32 kCaptureAxisThreshold = 0.6f;  // a stick must be pushed this far to be captured

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

InputSystem::InputSystem(mem::IAllocator& a) : alloc_(a), set_(a), cooked_bindings_(a), overrides_(a), queue_(a) {
    queue_.reserve(kQueueCapacity);
    origin_us_ = platform::now_us();
}

// ---------------------------------------------------------------------------- maps

Result<void> InputSystem::load_map(Span<const byte> chunk) {
    InputMapSet parsed(alloc_);
    MCE_TRY(decode_input_map(chunk, parsed));
    set_ = std::move(parsed);
    cooked_bindings_ = set_.bindings;
    apply_overrides();  // the player's persisted rebinds survive a map (re)load
    if (rebind_.state == RebindState::Waiting) rebind_.state = RebindState::Cancelled;
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

BindingDef* InputSystem::find_binding(StringId map, StringId action, u32 index, const ActionDef** def) {
    const ActionMapDef* m = set_.find_map(map);
    if (!m) return nullptr;
    for (u32 i = 0; i < m->action_count; ++i) {
        const ActionDef& a = set_.actions[m->first_action + i];
        if (a.name != action) continue;
        if (index >= a.binding_count) return nullptr;
        if (def) *def = &a;
        return &set_.bindings[a.first_binding + index];
    }
    return nullptr;
}

bool InputSystem::rebind(StringId map, StringId action, u32 index, const BindingDef& binding) {
    BindingDef* b = find_binding(map, action, index);
    if (!b) return false;
    *b = binding;
    set_override(map, action, index, binding);
    return true;
}

void InputSystem::set_override(StringId map, StringId action, u32 index, const BindingDef& b) {
    const BindingDef& cooked = cooked_bindings_[static_cast<usize>(find_binding(map, action, index) - set_.bindings.data())];
    const bool is_default = cooked.device == b.device && cooked.axis == b.axis && cooked.curve == b.curve &&
                            cooked.flags == b.flags && cooked.code == b.code && cooked.scale == b.scale &&
                            cooked.dz.inner == b.dz.inner && cooked.dz.outer == b.dz.outer;
    for (usize i = 0; i < overrides_.size(); ++i) {
        BindingOverride& o = overrides_[i];
        if (o.map != map.value || o.action != action.value || o.index != index) continue;
        if (is_default) {
            overrides_.erase(i);  // rebinding back to the default leaves nothing to persist
        } else {
            o = BindingOverride::make(map, action, index, b);
        }
        return;
    }
    if (!is_default) overrides_.push_back(BindingOverride::make(map, action, index, b));
}

void InputSystem::apply_overrides() {
    u32 dropped = 0;
    for (usize i = 0; i < overrides_.size();) {
        const BindingOverride& o = overrides_[i];
        if (BindingDef* b = find_binding(StringId(o.map), StringId(o.action), o.index)) {
            *b = o.binding();
            ++i;
        } else {
            overrides_.erase(i);
            ++dropped;
        }
    }
    if (dropped) MCE_LOG_WARN("input: dropped {} saved binding(s) the current input map no longer has", dropped);
}

void InputSystem::reset_bindings() {
    if (cooked_bindings_.size() == set_.bindings.size()) set_.bindings = cooked_bindings_;
    overrides_.clear();
}

void InputSystem::serialize_bindings(Array<char>& out) const { write_bindings(overrides_.span(), out); }

Result<void> InputSystem::deserialize_bindings(StringView text) {
    Array<BindingOverride> parsed(alloc_);
    MCE_TRY(read_bindings(text, parsed));
    for (const BindingOverride& o : parsed.span()) {
        if (static_cast<u8>(o.device) > static_cast<u8>(Device::TouchSwipe) || o.axis > 1)
            return Error(Errc::InvalidArgument, "input.bindings.read", "binding out of range");
    }
    reset_bindings();
    overrides_ = std::move(parsed);
    apply_overrides();
    return ok();
}

Result<void> InputSystem::save_bindings(StringView path) const {
    Array<char> text(alloc_);
    serialize_bindings(text);
    FixedString<512> p(path), tmp(path);
    tmp.append(".tmp");
    std::FILE* f = std::fopen(tmp.c_str(), "wb");
    if (!f) return Error(Errc::IoError, "input.bindings.save", "cannot open for writing");
    const bool wrote = std::fwrite(text.data(), 1, text.size(), f) == text.size();
    const bool closed = std::fclose(f) == 0;
    if (!wrote || !closed) {
        std::remove(tmp.c_str());
        return Error(Errc::IoError, "input.bindings.save", "write failed");
    }
    std::remove(p.c_str());  // rename() does not replace an existing file on Windows
    if (std::rename(tmp.c_str(), p.c_str()) != 0) return Error(Errc::IoError, "input.bindings.save", "rename failed");
    return ok();
}

Result<void> InputSystem::load_bindings(StringView path) {
    FixedString<512> p(path);
    std::FILE* f = std::fopen(p.c_str(), "rb");
    if (!f) return Error(Errc::FileNotFound, "input.bindings.load");
    Array<char> text(alloc_);
    char buf[1024];
    for (usize n; (n = std::fread(buf, 1, sizeof(buf), f)) > 0;)
        for (usize i = 0; i < n; ++i) text.push_back(buf[i]);
    std::fclose(f);
    return deserialize_bindings(StringView(text.data(), text.size()));
}

bool InputSystem::begin_rebind(StringId map, StringId action, u32 index, f32 timeout_s, u8 accept) {
    const ActionDef* def = nullptr;
    if (!find_binding(map, action, index, &def)) return false;
    rebind_ = Rebind{};
    rebind_.state = RebindState::Waiting;
    rebind_.map = map;
    rebind_.action = action;
    rebind_.index = index;
    rebind_.accept = accept;
    rebind_.button_action = def->type == ActionType::Button;
    rebind_.deadline_tick = last_tick_ + 1 + static_cast<u64>(timeout_s * static_cast<f32>(tick_hz_));
    return true;
}

void InputSystem::cancel_rebind() {
    if (rebind_.state == RebindState::Waiting) rebind_.state = RebindState::Cancelled;
}

bool InputSystem::try_capture(const Event& e) {
    if (rebind_.state != RebindState::Waiting) return false;
    BindingDef* target = find_binding(rebind_.map, rebind_.action, rebind_.index);
    if (!target) {
        rebind_.state = RebindState::Cancelled;
        return false;
    }
    BindingDef nb;
    nb.axis = target->axis;
    const f32 sign = target->scale < 0.f ? -1.f : 1.f;
    nb.scale = sign;
    switch (e.type) {
        case EventType::KeyDown:
            if (e.flags & platform::EventFlag::Repeat) return true;
            if (e.code == static_cast<u32>(platform::Key::Escape)) {
                rebind_.state = RebindState::Cancelled;
                return true;
            }
            if (!(rebind_.accept & RebindAccept::Keyboard)) return false;
            nb.device = Device::Keyboard;
            nb.code = e.code;
            break;
        case EventType::MouseButtonDown:
            if (!(rebind_.accept & RebindAccept::Mouse)) return false;
            nb.device = Device::MouseButton;
            nb.code = e.code;
            break;
        case EventType::GamepadButtonDown:
            if (!(rebind_.accept & RebindAccept::Gamepad)) return false;
            nb.device = Device::GamepadButton;
            nb.code = e.code;
            break;
        case EventType::GamepadAxis: {
            if (!(rebind_.accept & RebindAccept::Gamepad) || std::fabs(e.x) < kCaptureAxisThreshold) return false;
            nb.device = Device::GamepadAxis;
            nb.code = e.code;
            nb.dz = {0.2f, 1.f};
            // A button action fires when the stick is pushed the captured way; an axis binding
            // keeps its sign relative to the push (the "left" half of Move stays left).
            const f32 dir = e.x < 0.f ? -1.f : 1.f;
            nb.scale = rebind_.button_action ? dir : sign * dir;
            break;
        }
        default: return false;
    }
    rebind_.result = nb;
    rebind_.state = RebindState::Captured;
    rebind(rebind_.map, rebind_.action, rebind_.index, nb);
    return true;
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

void InputSystem::set_view_size(f32 width_px, f32 height_px) {
    if (width_px > 0.f && height_px > 0.f) view_px_ = vec2(width_px, height_px);
}

vec2 InputSystem::touch_units(vec2 d) const {
    const f32 short_side = view_px_.x < view_px_.y ? view_px_.x : view_px_.y;
    return vec2(d.x * view_px_.x / short_side, d.y * view_px_.y / short_side);
}

void InputSystem::end_touch(u32 slot, vec2 pos, u64 timestamp_us, bool canceled) {
    TouchState& t = touches_[slot];
    if (!t.down || t.release_pending) return;
    t.pos = pos;
    if (!canceled && tick_gesture_count_ < platform::kMaxTouches) {
        const vec2 d = touch_units(vec2(pos.x - t.origin.x, pos.y - t.origin.y));
        const f32 travel = std::sqrt(d.x * d.x + d.y * d.y);
        const u64 dur = timestamp_us > t.start_us ? timestamp_us - t.start_us : 0;
        Gesture g;
        g.origin = t.origin;
        bool add = false;
        if (travel < kTapMaxTravel && dur <= kTapMaxUs) {
            add = true;
        } else if (travel >= kSwipeMinTravel && dur <= kSwipeMaxUs) {
            add = true;
            g.swipe = true;
            if (std::fabs(d.x) >= std::fabs(d.y)) g.dir = vec2(d.x < 0.f ? -1.f : 1.f, 0.f);
            else g.dir = vec2(0.f, d.y < 0.f ? -1.f : 1.f);
        }
        if (add) tick_gestures_[tick_gesture_count_++] = g;
    }
    // A touch that went down in this window still shows held for one tick (the visibility floor).
    if (t.tapped) {
        t.release_pending = true;
    } else {
        t.down = false;
        t.released = true;
    }
}

void InputSystem::reset_devices() {
    for (u32 i = 0; i < platform::kMaxTouches; ++i) {
        TouchState& t = touches_[i];
        if (t.down) {
            t.down = false;
            t.released = true;
        }
        t.tapped = t.release_pending = false;
    }
    for (PadState& p : pads_) {
        p.buttons.clear();
        p.tapped.clear();
        p.release_pending.clear();
        for (f32& a : p.axes) a = 0.f;
    }
    keys_.clear();
    keys_tapped_.clear();
    keys_release_pending_.clear();
    buttons_.clear();
    buttons_tapped_.clear();
    buttons_release_pending_.clear();
}

void InputSystem::fold(const Event& e) {
    ++stats_.events_folded;
    if (try_capture(e)) return;
    const u32 slot = e.flags & 0xffu;  // gamepad events: player slot
    switch (e.type) {
        case EventType::GamepadAdded:
        case EventType::GamepadRemoved:
            if (slot < platform::kMaxGamepads) {
                pads_[slot] = PadState{};  // a plugged/unplugged pad reads neutral
                pads_[slot].connected = e.type == EventType::GamepadAdded;
            }
            break;
        case EventType::GamepadButtonDown:
            if (slot < platform::kMaxGamepads && e.code < kPadButtons) {
                pads_[slot].buttons.set(e.code);
                pads_[slot].tapped.set(e.code);
                pads_[slot].release_pending.reset(e.code);
            }
            break;
        case EventType::GamepadButtonUp:
            if (slot < platform::kMaxGamepads && e.code < kPadButtons) {
                PadState& p = pads_[slot];
                if (p.tapped.test(e.code)) p.release_pending.set(e.code);
                else p.buttons.reset(e.code);
            }
            break;
        case EventType::GamepadAxis:
            if (slot < platform::kMaxGamepads && e.code < kPadAxes) pads_[slot].axes[e.code] = e.x;
            break;
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
        case EventType::TouchDown:
            if (e.code < platform::kMaxTouches) {
                TouchState& t = touches_[e.code];
                t = TouchState{};
                t.down = t.tapped = true;
                t.origin = t.pos = vec2(e.x, e.y);
                t.start_us = e.timestamp_us;
                t.seq = ++touch_seq_;
            }
            break;
        case EventType::TouchMove:
            if (e.code < platform::kMaxTouches && touches_[e.code].down && !touches_[e.code].release_pending)
                touches_[e.code].pos = vec2(e.x, e.y);
            break;
        case EventType::TouchUp:
            if (e.code < platform::kMaxTouches)
                end_touch(e.code, vec2(e.x, e.y), e.timestamp_us, (e.flags & platform::EventFlag::TouchCanceled) != 0);
            break;
        case EventType::Resized:
            set_view_size(e.x, e.y);
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
    for (PadState& p : pads_) {
        for (u32 b = 0; b < kPadButtons; ++b)
            if (p.release_pending.test(b)) p.buttons.reset(b);
        p.release_pending.clear();
        p.tapped.clear();
    }
    for (TouchState& t : touches_) {
        t.released = false;
        if (t.release_pending) {
            t.down = false;
            t.released = true;
        }
        t.release_pending = false;
        t.tapped = false;
    }
    tick_gesture_count_ = 0;
    tick_wheel_ = vec2();
    tick_delta_ = vec2();
    tick_text_len_ = 0;

    // Fold, in timestamp order, everything stamped before this tick's close.
    while (queue_head_ < queue_.size() && queue_[queue_head_].timestamp_us < close_us) fold(queue_[queue_head_++]);
    if (queue_head_ == queue_.size()) {
        queue_.clear();
        queue_head_ = 0;
    }

    if (rebind_.state == RebindState::Waiting && tick >= rebind_.deadline_tick) rebind_.state = RebindState::TimedOut;

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
        case Device::GamepadButton:
            if (b.code < kPadButtons)
                for (const PadState& p : pads_)
                    if (p.buttons.test(b.code)) v = 1.f;
            break;
        case Device::GamepadAxis:
            // The largest deflection across pads wins. The magnitude lives in its own variable:
            // the fabs(a) > fabs(v) form dropped negative deflections under AppleClang 17 -O2
            // (arm64) while every Linux build and sanitizer passed.
            if (b.code < kPadAxes) {
                f32 best_mag = 0.f;
                for (const PadState& p : pads_) {
                    const f32 a = p.axes[b.code];
                    const f32 mag = a < 0.f ? -a : a;
                    if (mag > best_mag) {
                        best_mag = mag;
                        v = a;
                    }
                }
            }
            break;
        case Device::TouchZone: {
            const TouchZoneRect z = unpack_touch_zone(b.code);
            for (const TouchState& t : touches_)
                if (t.down && z.contains(t.origin.x, t.origin.y)) v = 1.f;
            break;
        }
        case Device::TouchStick: {
            const TouchZoneRect z = unpack_touch_zone(b.code);
            const TouchState* oldest = nullptr;
            for (const TouchState& t : touches_)
                if (t.down && z.contains(t.origin.x, t.origin.y) && (!oldest || t.seq < oldest->seq)) oldest = &t;
            if (oldest) {
                vec2 d = touch_units(vec2(oldest->pos.x - oldest->origin.x, oldest->pos.y - oldest->origin.y));
                d = vec2(d.x / kTouchStickRadius, d.y / kTouchStickRadius);
                const f32 len2 = d.x * d.x + d.y * d.y;
                if (len2 > 1.f) {
                    const f32 inv = 1.f / std::sqrt(len2);
                    d = vec2(d.x * inv, d.y * inv);
                }
                v = (b.flags & BindingFlag::SourceY) ? d.y : d.x;
            }
            break;
        }
        case Device::TouchTap:
        case Device::TouchSwipe: {
            const TouchZoneRect z = unpack_touch_zone(b.code);
            const bool swipe = b.device == Device::TouchSwipe;
            for (u32 i = 0; i < tick_gesture_count_ && v == 0.f; ++i) {
                const Gesture& g = tick_gestures_[i];
                if (g.swipe != swipe || !z.contains(g.origin.x, g.origin.y)) continue;
                v = !swipe ? 1.f : (b.flags & BindingFlag::SourceY) ? g.dir.y : g.dir.x;
            }
            break;
        }
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

    // Touches, oldest first: the ones down plus the ones that lifted this tick.
    const TouchState* order[platform::kMaxTouches];
    u32 n = 0;
    for (const TouchState& t : touches_)
        if (t.down || t.released) order[n++] = &t;
    for (u32 i = 1; i < n; ++i)  // insertion sort by start order (std::sort trips GCC 13's -Warray-bounds at -O2)
        for (u32 j = i; j > 0 && order[j]->seq < order[j - 1]->seq; --j) {
            const TouchState* t = order[j];
            order[j] = order[j - 1];
            order[j - 1] = t;
        }
    for (u32 i = 0; i < n && out.touch_count < kMaxTouchPoints; ++i) {
        const TouchState& t = *order[i];
        TouchPoint& p = out.touches[out.touch_count++];
        p.pos = t.pos;
        p.origin = t.origin;
        p.slot = static_cast<u8>(&t - touches_);
        p.held = t.down;
        p.released = t.released;
        p.pressed = t.tapped;  // went down in this tick's window
    }

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

bool InputSystem::gamepad_held(u32 slot, platform::GamepadButton b) const {
    const u32 c = static_cast<u32>(b);
    return slot < platform::kMaxGamepads && c < kPadButtons && pads_[slot].buttons.test(c);
}

f32 InputSystem::gamepad_axis(u32 slot, platform::GamepadAxis a) const {
    const u32 c = static_cast<u32>(a);
    return slot < platform::kMaxGamepads && c < kPadAxes ? pads_[slot].axes[c] : 0.f;
}

bool InputSystem::mouse_held(platform::MouseButton b) const {
    const u32 c = static_cast<u32>(b);
    return c < kMouseButtons && buttons_.test(c);
}

}  // namespace mce::input
