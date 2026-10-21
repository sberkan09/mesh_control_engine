#include "mce/input/script.h"

#include <cstdio>
#include <cstdlib>
#include <cstring>

#include "mce/log.h"

namespace mce::input {

namespace {

bool is_space(char c) { return c == ' ' || c == '\t' || c == '\r'; }

// Decimal unsigned integer; false on anything else.
bool parse_u64(StringView s, u64& out) {
    if (s.empty() || s.size() > 18) return false;
    u64 v = 0;
    for (char c : s) {
        if (c < '0' || c > '9') return false;
        v = v * 10 + static_cast<u64>(c - '0');
    }
    out = v;
    return true;
}

bool parse_f32(StringView s, f32& out) {
    FixedString<64> tmp(s);
    if (tmp.truncated() || tmp.empty()) return false;
    char* end = nullptr;
    out = std::strtof(tmp.c_str(), &end);
    return end == tmp.c_str() + tmp.size();
}

// "1", "1,0", "true", "false", "0.5"; `held` is false only for false / off.
bool parse_value(StringView s, f32& x, f32& y, bool& held) {
    x = 1.f;
    y = 0.f;
    held = true;
    if (s == "true" || s == "on") return true;
    if (s == "false" || s == "off") {
        x = 0.f;
        held = false;
        return true;
    }
    const usize comma = s.find(',');
    if (comma == StringView::npos) return parse_f32(s, x);
    return parse_f32(s.substr(0, comma), x) && parse_f32(s.substr(comma + 1), y);
}

// The message travels in Error::path ("line 3: expected ..."), the line number in detail.
Error parse_error(u32 line, const char* what) {
    FixedString<256> msg;
    char num[24];
    std::snprintf(num, sizeof num, "line %u: ", line);
    msg.append(num);
    msg.append(what);
    return Error(Errc::ParseError, "input.script", msg.view(), line);
}

}  // namespace

namespace {
// "P2/Move" is local player 2's "Move" (M-VIEWS): P1 is slot 0 and keeps the plain name, P2..P4 are
// player_action_id(1..3, name). Anything else is the action's own name.
StringId resolve_action(StringView spelled) {
    if (spelled.size() > 3 && spelled[0] == 'P' && spelled[1] >= '1' && spelled[1] <= '4' && spelled[2] == '/')
        return player_action_id(static_cast<u32>(spelled[1] - '1'), StringId(spelled.substr(3)));
    return StringId(spelled);
}
}  // namespace

bool InputScript::add(StringView action, u64 from, u64 to, f32 x, f32 y) {
    if (action.empty() || action.size() > 31 || from == 0 || to < from) return false;
    ScriptSegment s;
    s.from = from;
    s.to = to;
    s.action = resolve_action(action);
    s.name.assign(action);
    s.x = x;
    s.y = y;
    s.held = x != 0.f || y != 0.f;
    segments_.push_back(s);
    return true;
}

bool InputScript::add(const ScriptSegment& seg) {
    if (!add(seg.name.view(), seg.from, seg.to, seg.x, seg.y)) return false;
    segments_.back().held = seg.held;
    return true;
}

void InputScript::clear() {
    segments_.clear();
    state_ = InputState{};
    prev_ = InputState{};
    last_evaluated_ = 0;
}

u64 InputScript::last_tick() const {
    u64 last = 0;
    for (const ScriptSegment& s : segments_)
        if (s.to != ~u64(0) && s.to > last) last = s.to;
    return last;
}

Result<void> InputScript::parse(StringView text) {
    u32 line_no = 0;
    usize pos = 0;
    while (pos <= text.size()) {
        usize end = text.find('\n', pos);
        if (end == StringView::npos) end = text.size();
        StringView line = text.substr(pos, end - pos);
        pos = end + 1;
        ++line_no;
        if (const usize hash = line.find('#'); hash != StringView::npos) line = line.substr(0, hash);
        // Tokens separated by blanks.
        StringView tokens[kMaxActions + 1];
        usize n = 0;
        usize i = 0;
        while (i < line.size()) {
            while (i < line.size() && is_space(line[i])) ++i;
            if (i >= line.size()) break;
            const usize start = i;
            while (i < line.size() && !is_space(line[i])) ++i;
            if (n > kMaxActions) return parse_error(line_no, "too many actions on one line");
            tokens[n++] = line.substr(start, i - start);
        }
        if (n == 0) continue;
        // "<from>", "<from>-<to>" or "<from>-"
        u64 from = 0, to = ~u64(0);
        const StringView range = tokens[0];
        const usize dash = range.find('-');
        if (dash == StringView::npos) {
            if (!parse_u64(range, from)) return parse_error(line_no, "expected a tick or a tick range (e.g. 10-70) first");
            to = from;
        } else {
            if (!parse_u64(range.substr(0, dash), from)) return parse_error(line_no, "expected a tick range like 10-70 or 120-");
            const StringView rest = range.substr(dash + 1);
            if (!rest.empty() && !parse_u64(rest, to)) return parse_error(line_no, "expected a tick range like 10-70 or 120-");
        }
        if (from == 0) return parse_error(line_no, "ticks are 1-based: the first tick is 1");
        if (to < from) return parse_error(line_no, "the range ends before it starts");
        if (n == 1) return parse_error(line_no, "expected at least one action after the tick range");
        for (usize t = 1; t < n; ++t) {
            StringView name = tokens[t];
            f32 x = 1.f, y = 0.f;
            bool held = true;
            if (const usize eq = name.find('='); eq != StringView::npos) {
                if (!parse_value(name.substr(eq + 1), x, y, held))
                    return parse_error(line_no, "expected a value after '=': a number, x,y or true/false");
                name = name.substr(0, eq);
            }
            if (!add(name, from, to, x, y)) return parse_error(line_no, "action names are 1-31 characters");
            segments_.back().held = held;
        }
    }
    return ok();
}

const InputState& InputScript::evaluate(u64 tick) {
    // Edges are against the previously evaluated tick; evaluating the same tick again gives the
    // same answer (prev_ stays), and a jump in tick numbers starts fresh.
    if (tick != last_evaluated_) {
        const bool sequential = last_evaluated_ != 0 && tick == last_evaluated_ + 1;
        prev_ = sequential ? state_ : InputState{};
    }
    last_evaluated_ = tick;
    InputState& out = state_;
    out = InputState{};
    out.tick = tick;
    const f32 dt = 1.f / static_cast<f32>(kTickHz);

    // Active segments, in order: a later segment for the same action replaces the earlier value.
    // "Touch" and "Mouse" are not actions: a finger down at (x, y) of the window, and the mouse there.
    bool touch = false;
    vec2 touch_at;
    for (const ScriptSegment& s : segments_) {
        if (tick < s.from || tick > s.to) continue;
        if (s.action == MCE_SID("Touch")) {
            touch = true;
            touch_at = vec2(s.x, s.y);
            continue;
        }
        if (s.action == MCE_SID("Mouse")) {
            out.mouse_pos = vec2(s.x, s.y);
            continue;
        }
        u32 slot = out.action_count;
        for (u32 i = 0; i < out.action_count; ++i)
            if (out.action_names[i] == s.action) slot = i;
        if (slot == out.action_count) {
            if (out.action_count == kMaxActions) {
                MCE_LOG_ERROR("input script: more than {} actions at tick {}; '{}' dropped", kMaxActions, tick, s.name.c_str());
                continue;
            }
            out.action_names[slot] = s.action;
            ++out.action_count;
        }
        ActionState& st = out.actions[slot];
        st.x = s.x;
        st.y = s.y;
        st.held = s.held;
    }
    // A segment ending at this tick's predecessor: the action is released this tick, which the loop
    // above never sees (it is not active). Carry it as a named, released action for one tick.
    for (u32 p = 0; p < prev_.action_count; ++p) {
        if (!prev_.actions[p].held || out.has_action(prev_.action_names[p]) || out.action_count == kMaxActions) continue;
        out.action_names[out.action_count] = prev_.action_names[p];
        out.actions[out.action_count] = ActionState{};
        ++out.action_count;
    }
    for (u32 i = 0; i < out.action_count; ++i) {
        ActionState& st = out.actions[i];
        const ActionState* before = nullptr;
        for (u32 p = 0; p < prev_.action_count; ++p)
            if (prev_.action_names[p] == out.action_names[i]) before = &prev_.actions[p];
        const bool was_held = before && before->held;
        st.pressed = st.held && !was_held;
        st.released = !st.held && was_held;
        st.held_time = st.held ? (was_held ? before->held_time + dt : dt) : 0.f;
    }

    // One scripted finger (slot 0): down while a Touch segment runs, lifted the tick after.
    const TouchPoint* before = prev_.touch_count > 0 && prev_.touches[0].held ? &prev_.touches[0] : nullptr;
    if (touch || before) {
        TouchPoint& t = out.touches[0];
        t.pos = touch ? touch_at : before->pos;
        t.origin = before ? before->origin : touch_at;
        t.held = touch;
        t.pressed = touch && !before;
        t.released = !touch;
        out.touch_count = 1;
    }

    // The starter kit's fast-path fields, by name (InputSystem::evaluate does the same).
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
    return out;
}

const InputState* InputScript::sample_for_tick(u64 tick, u64) { return &evaluate(tick); }

}  // namespace mce::input
