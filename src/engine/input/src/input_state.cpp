#include "mce/input/actions.h"

#include <cstring>

#include "mce/log.h"

namespace mce::input {

namespace {
const ActionState kNullAction{};
}

bool InputState::has_action(StringId name) const {
    for (u32 i = 0; i < action_count; ++i)
        if (action_names[i] == name) return true;
    return false;
}

const ActionState& InputState::action(StringId name) const {
    for (u32 i = 0; i < action_count; ++i)
        if (action_names[i] == name) return actions[i];
    // One error per distinct unknown name in a row (not per call): gameplay polls every tick.
    static thread_local u64 last_unknown = 0;
    if (last_unknown != name.value) {
        last_unknown = name.value;
        MCE_LOG_ERROR("input: unknown action (id {:#018x}) — reads as released; is it in the active map?",
                      name.value);
    }
    return kNullAction;
}

namespace {
bool same(const ActionState& a, const ActionState& b) {
    return a.x == b.x && a.y == b.y && a.held_time == b.held_time && a.held == b.held && a.pressed == b.pressed &&
           a.released == b.released;
}
bool same(vec2 a, vec2 b) { return a.x == b.x && a.y == b.y; }
bool same(const TouchPoint& a, const TouchPoint& b) {
    return same(a.pos, b.pos) && same(a.origin, b.origin) && a.slot == b.slot && a.held == b.held &&
           a.pressed == b.pressed && a.released == b.released;
}
}  // namespace

bool operator==(const InputState& a, const InputState& b) {
    if (a.tick != b.tick || !same(a.move, b.move) || !same(a.look, b.look) || a.trigger_l != b.trigger_l ||
        a.trigger_r != b.trigger_r || a.jump_held != b.jump_held || a.jump_pressed != b.jump_pressed ||
        a.jump_released != b.jump_released || a.fire_pressed != b.fire_pressed || a.pause_pressed != b.pause_pressed ||
        a.confirm_pressed != b.confirm_pressed || a.cancel_pressed != b.cancel_pressed ||
        !same(a.mouse_pos, b.mouse_pos) || !same(a.wheel, b.wheel) || a.action_count != b.action_count ||
        a.text() != b.text() || a.touch_count != b.touch_count)
        return false;
    for (u32 i = 0; i < a.touch_count; ++i)
        if (!same(a.touches[i], b.touches[i])) return false;
    for (u32 i = 0; i < a.action_count; ++i)
        if (a.action_names[i] != b.action_names[i] || !same(a.actions[i], b.actions[i])) return false;
    return true;
}

u64 hash_state(const InputState& s, u64 seed) {
    Hasher64 h(seed);
    auto f = [&](f32 v) { h.bytes(&v, sizeof(v)); };
    auto bit = [&](bool v) {
        const u8 b = v ? 1 : 0;
        h.bytes(&b, 1);
    };
    h.bytes(&s.tick, sizeof(s.tick));
    f(s.move.x), f(s.move.y), f(s.look.x), f(s.look.y), f(s.trigger_l), f(s.trigger_r);
    bit(s.jump_held), bit(s.jump_pressed), bit(s.jump_released), bit(s.fire_pressed), bit(s.pause_pressed);
    bit(s.confirm_pressed), bit(s.cancel_pressed);
    h.bytes(&s.action_count, sizeof(s.action_count));
    for (u32 i = 0; i < s.action_count; ++i) {
        const ActionState& a = s.actions[i];
        h.bytes(&s.action_names[i].value, sizeof(u64));
        f(a.x), f(a.y), f(a.held_time);
        bit(a.held), bit(a.pressed), bit(a.released);
    }
    h.bytes(s.text_buf, s.text_len);
    // Touches only when present, so snapshots without touch keep the hashes they always had.
    if (s.touch_count > 0) {
        h.bytes(&s.touch_count, sizeof(s.touch_count));
        for (u32 i = 0; i < s.touch_count; ++i) {
            const TouchPoint& t = s.touches[i];
            f(t.pos.x), f(t.pos.y), f(t.origin.x), f(t.origin.y);
            h.bytes(&t.slot, 1);
            bit(t.held), bit(t.pressed), bit(t.released);
        }
    }
    return h.value();
}

}  // namespace mce::input
