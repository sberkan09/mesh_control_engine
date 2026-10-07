#include <doctest/doctest.h>

#include "mce/input/input.h"

using namespace mce;
using platform::Event;
using platform::EventType;
using platform::Key;

namespace {

constexpr u64 kHz = 60;
constexpr u64 close_of(u64 tick) { return (tick * 1'000'000ull) / kHz; }

Event key(EventType t, Key k, u64 ts, u32 flags = 0) {
    Event e;
    e.type = t;
    e.code = static_cast<u32>(k);
    e.flags = flags;
    e.timestamp_us = ts;
    return e;
}

Event mouse(EventType t, u32 code, f32 x, f32 y, u64 ts) {
    Event e;
    e.type = t;
    e.code = code;
    e.x = x;
    e.y = y;
    e.timestamp_us = ts;
    return e;
}

bool same(const input::InputState& a, const input::InputState& b) {
    return a.tick == b.tick && a.move.x == b.move.x && a.move.y == b.move.y && a.look.x == b.look.x &&
           a.look.y == b.look.y && a.jump_held == b.jump_held && a.jump_pressed == b.jump_pressed &&
           a.jump_released == b.jump_released && a.fire_held == b.fire_held && a.fire_pressed == b.fire_pressed &&
           a.pause_pressed == b.pause_pressed && a.confirm_pressed == b.confirm_pressed &&
           a.cancel_pressed == b.cancel_pressed && a.mouse_pos.x == b.mouse_pos.x &&
           a.mouse_pos.y == b.mouse_pos.y && a.mouse_wheel.x == b.mouse_wheel.x && a.mouse_wheel.y == b.mouse_wheel.y;
}

}  // namespace

TEST_CASE("input: press and release edges land on the tick whose window holds the timestamp") {
    input::InputSystem in;
    in.push(key(EventType::KeyDown, Key::Space, close_of(1) + 10));  // inside tick 2's window
    const input::InputState* s1 = in.sample_for_tick(1, close_of(1));
    CHECK_FALSE(s1->jump_held);
    CHECK_FALSE(s1->jump_pressed);
    CHECK(in.queued() == 1);

    const input::InputState* s2 = in.sample_for_tick(2, close_of(2));
    CHECK(s2->tick == 2);
    CHECK(s2->jump_held);
    CHECK(s2->jump_pressed);

    const input::InputState* s3 = in.sample_for_tick(3, close_of(3));
    CHECK(s3->jump_held);
    CHECK_FALSE(s3->jump_pressed);

    in.push(key(EventType::KeyUp, Key::Space, close_of(3) + 1));
    const input::InputState* s4 = in.sample_for_tick(4, close_of(4));
    CHECK_FALSE(s4->jump_held);
    CHECK(s4->jump_released);
}

TEST_CASE("input: a press and release inside one window still reads as pressed") {
    input::InputSystem in;
    in.push(key(EventType::KeyDown, Key::Return, close_of(1) + 100));
    in.push(key(EventType::KeyUp, Key::Return, close_of(1) + 200));
    const input::InputState* s = in.sample_for_tick(2, close_of(2));
    CHECK(s->confirm_pressed);
}

TEST_CASE("input: key repeat does not re-fire the pressed edge") {
    input::InputSystem in;
    in.push(key(EventType::KeyDown, Key::Escape, 10));
    CHECK(in.sample_for_tick(1, close_of(1))->pause_pressed);
    in.push(key(EventType::KeyDown, Key::Escape, close_of(1) + 5, platform::EventFlag::Repeat));
    CHECK_FALSE(in.sample_for_tick(2, close_of(2))->pause_pressed);
}

TEST_CASE("input: move axis from WASD and arrows, diagonals normalized") {
    input::InputSystem in;
    in.push(key(EventType::KeyDown, Key::D, 1));
    CHECK(in.sample_for_tick(1, close_of(1))->move.x == 1.0f);
    in.push(key(EventType::KeyDown, Key::Up, close_of(1) + 1));
    const input::InputState* s = in.sample_for_tick(2, close_of(2));
    CHECK(s->move.x == doctest::Approx(0.70710678));
    CHECK(s->move.y == doctest::Approx(0.70710678));
    in.push(key(EventType::KeyDown, Key::A, close_of(2) + 1));
    CHECK(in.sample_for_tick(3, close_of(3))->move.x == 0.0f);
}

TEST_CASE("input: focus loss releases everything held") {
    input::InputSystem in;
    in.push(key(EventType::KeyDown, Key::Space, 1));
    in.push(mouse(EventType::MouseButtonDown, static_cast<u32>(platform::MouseButton::Left), 0, 0, 2));
    const input::InputState* s1 = in.sample_for_tick(1, close_of(1));
    CHECK(s1->jump_held);
    CHECK(s1->fire_held);
    CHECK(s1->fire_pressed);

    Event focus;
    focus.type = EventType::FocusOut;
    focus.timestamp_us = close_of(1) + 1;
    in.push(focus);
    const input::InputState* s2 = in.sample_for_tick(2, close_of(2));
    CHECK_FALSE(s2->jump_held);
    CHECK(s2->jump_released);
    CHECK_FALSE(s2->fire_held);
    CHECK_FALSE(in.key_held(Key::Space));
}

TEST_CASE("input: mouse look and wheel fold per window") {
    input::InputSystem in;
    in.push(mouse(EventType::MouseMove, 0, 100, 100, 1));
    in.push(mouse(EventType::MouseMove, 0, 110, 95, 2));
    in.push(mouse(EventType::MouseWheel, 0, 0, 1, 3));
    const input::InputState* s1 = in.sample_for_tick(1, close_of(1));
    CHECK(s1->look.x == 10.0f);
    CHECK(s1->look.y == -5.0f);
    CHECK(s1->mouse_pos.x == 110.0f);
    CHECK(s1->mouse_wheel.y == 1.0f);
    const input::InputState* s2 = in.sample_for_tick(2, close_of(2));
    CHECK(s2->look.x == 0.0f);
    CHECK(s2->mouse_wheel.y == 0.0f);
    CHECK(s2->mouse_pos.x == 110.0f);
}

TEST_CASE("input: out-of-order arrival is folded in timestamp order") {
    input::InputSystem in;
    in.push(key(EventType::KeyUp, Key::Space, 20));
    in.push(key(EventType::KeyDown, Key::Space, 10));
    const input::InputState* s = in.sample_for_tick(1, close_of(1));
    CHECK(s->jump_pressed);
    CHECK(s->jump_released);
    CHECK_FALSE(s->jump_held);
}

TEST_CASE("input: queue overflow drops the oldest events and counts them") {
    input::InputSystem in;
    for (u32 i = 0; i < input::InputSystem::kQueueCapacity + 10; ++i) {
        in.push(mouse(EventType::MouseWheel, 0, 0, 1, i));
    }
    CHECK(in.stats().events_dropped == 10);
    CHECK(in.queued() == input::InputSystem::kQueueCapacity);
}

// FR-INP-030: the same timestamped event stream, delivered by render loops at 30, 90 and 240 Hz,
// must give identical per-tick snapshots. Each frame delivers events whose timestamp has passed,
// then runs every tick whose window has closed, like app::run does.
TEST_CASE("input: sim-visible input is independent of render rate (FR-INP-030)") {
    constexpr u64 kDurationUs = 2'000'000;
    Event script[400];
    u32 n = 0;
    u64 rng = 0x1234567;
    auto next = [&] {
        rng ^= rng << 13;
        rng ^= rng >> 7;
        rng ^= rng << 17;
        return rng;
    };
    const Key keys[] = {Key::W, Key::A, Key::S, Key::D, Key::Space, Key::Escape, Key::Return};
    bool down[7] = {};
    for (u64 ts = 1000; ts < kDurationUs && n < 380; ts += 1000 + next() % 9000) {
        const u32 r = static_cast<u32>(next() % 10);
        if (r < 7) {
            down[r] = !down[r];
            script[n++] = key(down[r] ? EventType::KeyDown : EventType::KeyUp, keys[r], ts);
        } else if (r == 7) {
            script[n++] = mouse(EventType::MouseMove, 0, static_cast<f32>(next() % 640), static_cast<f32>(next() % 480), ts);
        } else if (r == 8) {
            script[n++] = mouse(EventType::MouseButtonDown, 1, 0, 0, ts);
            script[n++] = mouse(EventType::MouseButtonUp, 1, 0, 0, ts + 3000);
        } else {
            script[n++] = mouse(EventType::MouseWheel, 0, 0, 1, ts);
        }
    }
    // Keep the script sorted by delivery time (the button-up above can overtake later events).
    for (u32 i = 1; i < n; ++i) {
        for (u32 j = i; j > 0 && script[j - 1].timestamp_us > script[j].timestamp_us; --j) {
            const Event t = script[j];
            script[j] = script[j - 1];
            script[j - 1] = t;
        }
    }

    constexpr u64 kTicks = 110;  // all of them close before kDurationUs
    static input::InputState results[3][kTicks];
    const u64 render_hz[3] = {30, 90, 240};
    for (int r = 0; r < 3; ++r) {
        input::InputSystem in;
        u32 next_event = 0;
        u64 tick = 0;
        for (u64 frame = 1; tick < kTicks; ++frame) {
            const u64 now = (frame * 1'000'000ull) / render_hz[r];
            while (next_event < n && script[next_event].timestamp_us <= now) in.push(script[next_event++]);
            while (tick < kTicks && close_of(tick + 1) <= now) {
                ++tick;
                results[r][tick - 1] = *in.sample_for_tick(tick, close_of(tick));
            }
        }
    }
    u32 presses = 0;
    for (u64 t = 0; t < kTicks; ++t) {
        CHECK(same(results[0][t], results[1][t]));
        CHECK(same(results[0][t], results[2][t]));
        presses += results[0][t].jump_pressed ? 1u : 0u;
    }
    CHECK(n > 100);
    CHECK(presses > 0);  // the script actually exercises edges
}
