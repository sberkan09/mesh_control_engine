#include "mce/input/replay.h"

#include <cstring>

namespace mce::input {

namespace {

constexpr u32 kTickMarker = 0x4B434954u;  // "TICK": cheap corruption check per record

template <class T>
void put(Array<byte>& out, const T& v) {
    const usize at = out.size();
    out.resize(at + sizeof(T));
    std::memcpy(out.data() + at, &v, sizeof(T));
}

template <class T>
bool get(Span<const byte> in, usize& pos, T& v) {
    if (in.size() - pos < sizeof(T)) return false;
    std::memcpy(&v, in.data() + pos, sizeof(T));
    pos += sizeof(T);
    return true;
}

enum : u16 {
    kJumpHeld = 1u << 0,
    kJumpPressed = 1u << 1,
    kJumpReleased = 1u << 2,
    kFirePressed = 1u << 3,
    kPausePressed = 1u << 4,
    kConfirmPressed = 1u << 5,
    kCancelPressed = 1u << 6,
};

enum : u8 { kHeld = 1u << 0, kPressed = 1u << 1, kReleased = 1u << 2 };

// The action-count byte never exceeds kMaxActions (64); its top bit says a touch block follows the
// actions, so streams recorded before touch existed decode unchanged.
constexpr u8 kHasTouches = 0x80;

}  // namespace

void InputRecorder::record(const InputState& s) {
    put(bytes_, kTickMarker);
    put(bytes_, s.tick);
    const f32 f[10] = {s.move.x, s.move.y,      s.look.x,      s.look.y,  s.trigger_l,
                       s.trigger_r, s.mouse_pos.x, s.mouse_pos.y, s.wheel.x, s.wheel.y};
    put(bytes_, f);
    u16 bits = 0;
    if (s.jump_held) bits |= kJumpHeld;
    if (s.jump_pressed) bits |= kJumpPressed;
    if (s.jump_released) bits |= kJumpReleased;
    if (s.fire_pressed) bits |= kFirePressed;
    if (s.pause_pressed) bits |= kPausePressed;
    if (s.confirm_pressed) bits |= kConfirmPressed;
    if (s.cancel_pressed) bits |= kCancelPressed;
    put(bytes_, bits);
    put(bytes_, static_cast<u8>(s.action_count | (s.touch_count > 0 ? kHasTouches : 0)));
    put(bytes_, static_cast<u8>(s.text_len));
    for (u32 i = 0; i < s.text_len; ++i) put(bytes_, s.text_buf[i]);
    for (u32 i = 0; i < s.action_count; ++i) {
        const ActionState& a = s.actions[i];
        put(bytes_, s.action_names[i].value);
        const f32 af[3] = {a.x, a.y, a.held_time};
        put(bytes_, af);
        u8 ab = 0;
        if (a.held) ab |= kHeld;
        if (a.pressed) ab |= kPressed;
        if (a.released) ab |= kReleased;
        put(bytes_, ab);
    }
    if (s.touch_count > 0) {
        put(bytes_, static_cast<u8>(s.touch_count));
        for (u32 i = 0; i < s.touch_count; ++i) {
            const TouchPoint& t = s.touches[i];
            const f32 tf[4] = {t.pos.x, t.pos.y, t.origin.x, t.origin.y};
            put(bytes_, tf);
            u8 tb = 0;
            if (t.held) tb |= kHeld;
            if (t.pressed) tb |= kPressed;
            if (t.released) tb |= kReleased;
            put(bytes_, t.slot);
            put(bytes_, tb);
        }
    }
    ++ticks_;
}

bool InputReplayer::next(InputState& out) {
    usize pos = pos_;
    u32 marker = 0;
    if (!get(stream_, pos, marker) || marker != kTickMarker) return false;
    InputState s;
    f32 f[10];
    u16 bits = 0;
    u8 action_count = 0, text_len = 0;
    if (!get(stream_, pos, s.tick) || !get(stream_, pos, f) || !get(stream_, pos, bits) ||
        !get(stream_, pos, action_count) || !get(stream_, pos, text_len))
        return false;
    const bool has_touches = (action_count & kHasTouches) != 0;
    action_count = static_cast<u8>(action_count & ~kHasTouches);
    if (action_count > kMaxActions || text_len > kMaxTextBytes) return false;
    s.move = vec2(f[0], f[1]);
    s.look = vec2(f[2], f[3]);
    s.trigger_l = f[4];
    s.trigger_r = f[5];
    s.mouse_pos = vec2(f[6], f[7]);
    s.wheel = vec2(f[8], f[9]);
    s.jump_held = (bits & kJumpHeld) != 0;
    s.jump_pressed = (bits & kJumpPressed) != 0;
    s.jump_released = (bits & kJumpReleased) != 0;
    s.fire_pressed = (bits & kFirePressed) != 0;
    s.pause_pressed = (bits & kPausePressed) != 0;
    s.confirm_pressed = (bits & kConfirmPressed) != 0;
    s.cancel_pressed = (bits & kCancelPressed) != 0;
    s.text_len = text_len;
    for (u32 i = 0; i < text_len; ++i)
        if (!get(stream_, pos, s.text_buf[i])) return false;
    s.action_count = action_count;
    for (u32 i = 0; i < action_count; ++i) {
        f32 af[3];
        u8 ab = 0;
        if (!get(stream_, pos, s.action_names[i].value) || !get(stream_, pos, af) || !get(stream_, pos, ab))
            return false;
        ActionState& a = s.actions[i];
        a.x = af[0];
        a.y = af[1];
        a.held_time = af[2];
        a.held = (ab & kHeld) != 0;
        a.pressed = (ab & kPressed) != 0;
        a.released = (ab & kReleased) != 0;
    }
    if (has_touches) {
        u8 touch_count = 0;
        if (!get(stream_, pos, touch_count) || touch_count == 0 || touch_count > kMaxTouchPoints) return false;
        s.touch_count = touch_count;
        for (u32 i = 0; i < touch_count; ++i) {
            f32 tf[4];
            u8 tb = 0;
            TouchPoint& t = s.touches[i];
            if (!get(stream_, pos, tf) || !get(stream_, pos, t.slot) || !get(stream_, pos, tb)) return false;
            t.pos = vec2(tf[0], tf[1]);
            t.origin = vec2(tf[2], tf[3]);
            t.held = (tb & kHeld) != 0;
            t.pressed = (tb & kPressed) != 0;
            t.released = (tb & kReleased) != 0;
        }
    }
    out = s;
    pos_ = pos;
    return true;
}

}  // namespace mce::input
