// FR-INP-070 (M-MOBILE): touch events sampled per tick, touch bindings, touches in record/replay.

#include <doctest/doctest.h>

#include <cmath>
#include <cstring>

#include "input_test_util.h"
#include "mce/input/replay.h"

using namespace mce;
using namespace mce::input;
using platform::EventType;

namespace {

constexpr u64 kTickUs = 1'000'000 / 60;

// Tick t's window is [(t-1)·T, t·T); `at(t, f)` is a moment f of the way into it.
u64 at(u64 tick, f32 frac = 0.5f) { return (tick - 1) * kTickUs + static_cast<u64>(frac * kTickUs); }

void setup(InputSystem& in, f32 w = 1000.f, f32 h = 500.f) {
    test::setup_default(in);
    in.set_view_size(w, h);
}

}  // namespace

TEST_CASE("touch zones pack to bytes and round-trip") {
    const TouchZoneRect z{0.25f, 0.f, 0.5f, 1.f};
    const TouchZoneRect r = unpack_touch_zone(pack_touch_zone(z));
    CHECK(std::fabs(r.x0 - 0.25f) < 0.003f);
    CHECK(r.y0 == 0.f);
    CHECK(std::fabs(r.x1 - 0.5f) < 0.003f);
    CHECK(r.y1 == 1.f);
    CHECK(r.contains(0.3f, 0.5f));
    CHECK_FALSE(r.contains(0.6f, 0.5f));
}

TEST_CASE("the default map's left-half stick drives Move, +y up, clamped to the unit circle") {
    InputSystem in;
    setup(in);  // 1000x500: the short side is the height
    in.push_event(test::touch_event(EventType::TouchDown, 0, 0.2f, 0.5f, at(1)));
    in.sample_for_tick(1);
    CHECK(in.state().move.x == 0.f);
    CHECK(in.state().touch_count == 1);
    CHECK(in.state().touches[0].pressed);

    // 0.06 of the short side to the right = half the stick radius (0.12): 0.06·500/1000 = 0.03.
    in.push_event(test::touch_event(EventType::TouchMove, 0, 0.23f, 0.5f, at(2)));
    in.sample_for_tick(2);
    // Deadzone 0.15 rescales: (0.5 - 0.15) / 0.85.
    CHECK(in.state().move.x == doctest::Approx((0.5f - 0.15f) / 0.85f).epsilon(0.01));
    CHECK(in.state().move.y == 0.f);
    CHECK_FALSE(in.state().touches[0].pressed);
    CHECK(in.state().touches[0].held);

    // Up (screen y decreases) beyond the radius: Move reads straight up at full deflection.
    in.push_event(test::touch_event(EventType::TouchMove, 0, 0.2f, 0.1f, at(3)));
    in.sample_for_tick(3);
    CHECK(in.state().move.x == doctest::Approx(0.f));
    CHECK(in.state().move.y == doctest::Approx(1.f));

    in.push_event(test::touch_event(EventType::TouchUp, 0, 0.2f, 0.1f, at(4)));
    in.sample_for_tick(4);
    CHECK(in.state().move.y == 0.f);
    REQUIRE(in.state().touch_count == 1);
    CHECK(in.state().touches[0].released);
    CHECK_FALSE(in.state().touches[0].held);
    in.sample_for_tick(5);
    CHECK(in.state().touch_count == 0);
}

TEST_CASE("a touch belongs to the zone it started in, and the oldest touch drives a stick") {
    InputSystem in;
    setup(in);
    in.push_event(test::touch_event(EventType::TouchDown, 3, 0.3f, 0.5f, at(1, 0.2f)));
    in.push_event(test::touch_event(EventType::TouchDown, 1, 0.1f, 0.5f, at(1, 0.4f)));  // lower slot, younger
    in.push_event(test::touch_event(EventType::TouchMove, 3, 0.9f, 0.5f, at(1, 0.6f)));  // crosses into the right half
    in.push_event(test::touch_event(EventType::TouchMove, 1, 0.0f, 0.5f, at(1, 0.7f)));
    in.sample_for_tick(1);
    CHECK(in.state().move.x == doctest::Approx(1.f));  // slot 3 (oldest, moved right), not slot 1
    CHECK(in.state().look.x == 0.f);                    // never started in the right half
    REQUIRE(in.state().touch_count == 2);
    CHECK(in.state().touches[0].slot == 3);  // oldest first
    CHECK(in.state().touches[1].slot == 1);
}

TEST_CASE("a quick still touch on the right half is a tap: Jump for one tick, even inside one window") {
    InputSystem in;
    setup(in);
    in.push_event(test::touch_event(EventType::TouchDown, 0, 0.7f, 0.6f, at(1, 0.1f)));
    in.push_event(test::touch_event(EventType::TouchUp, 0, 0.701f, 0.6f, at(1, 0.6f)));
    in.sample_for_tick(1);
    CHECK(in.state().jump_pressed);
    REQUIRE(in.state().touch_count == 1);
    CHECK(in.state().touches[0].pressed);
    CHECK(in.state().touches[0].held);  // the one-tick visibility floor
    in.sample_for_tick(2);
    CHECK(in.state().jump_released);
    REQUIRE(in.state().touch_count == 1);
    CHECK(in.state().touches[0].released);
    in.sample_for_tick(3);
    CHECK_FALSE(in.state().jump_held);
}

TEST_CASE("a held touch, a moved touch and a canceled touch are not taps") {
    InputSystem in;
    setup(in);
    in.push_event(test::touch_event(EventType::TouchDown, 0, 0.7f, 0.6f, at(1)));
    in.push_event(test::touch_event(EventType::TouchUp, 0, 0.7f, 0.6f, at(1) + InputSystem::kTapMaxUs + 1));
    in.push_event(test::touch_event(EventType::TouchDown, 1, 0.7f, 0.6f, at(40)));
    in.push_event(test::touch_event(EventType::TouchUp, 1, 0.75f, 0.6f, at(41)));  // 0.1 short-side units
    in.push_event(test::touch_event(EventType::TouchDown, 2, 0.7f, 0.6f, at(60)));
    in.push_event(test::touch_event(EventType::TouchUp, 2, 0.7f, 0.6f, at(61), platform::EventFlag::TouchCanceled));
    for (u64 t = 1; t <= 70; ++t) {
        in.sample_for_tick(t);
        CHECK_FALSE(in.state().jump_pressed);
    }
}

TEST_CASE("swipes read +/-1 on their dominant axis for one tick") {
    InputMapBuilder b;
    b.map(MCE_SID("Swipes"));
    b.action(MCE_SID("Dash"), ActionType::Axis2D)
        .touch_swipe(TouchZoneRect{}, false, 0)
        .touch_swipe(TouchZoneRect{}, true, 1, -1.f);
    Array<byte> chunk;
    b.encode(chunk);
    InputSystem in;
    REQUIRE(in.load_map(chunk.span()).has_value());
    in.set_tick_origin_us(0);
    in.set_view_size(500.f, 1000.f);  // portrait

    in.push_event(test::touch_event(EventType::TouchDown, 0, 0.5f, 0.5f, at(1)));
    in.push_event(test::touch_event(EventType::TouchMove, 0, 0.45f, 0.4f, at(3)));
    in.push_event(test::touch_event(EventType::TouchUp, 0, 0.45f, 0.3f, at(5)));  // mostly up
    for (u64 t = 1; t <= 4; ++t) {
        in.sample_for_tick(t);
        CHECK(in.state().axis2(MCE_SID("Dash")) == math::vec2(0.f, 0.f));
    }
    in.sample_for_tick(5);
    CHECK(in.state().axis2(MCE_SID("Dash")).x == 0.f);
    CHECK(in.state().axis2(MCE_SID("Dash")).y == 1.f);  // screen up, flipped to +y up
    in.sample_for_tick(6);
    CHECK(in.state().axis2(MCE_SID("Dash")).y == 0.f);

    in.push_event(test::touch_event(EventType::TouchDown, 0, 0.5f, 0.5f, at(10)));
    in.push_event(test::touch_event(EventType::TouchUp, 0, 0.1f, 0.52f, at(12)));  // left
    in.sample_for_tick(10), in.sample_for_tick(11), in.sample_for_tick(12);
    CHECK(in.state().axis2(MCE_SID("Dash")).x == -1.f);

    // Too slow: no swipe.
    in.push_event(test::touch_event(EventType::TouchDown, 0, 0.5f, 0.5f, at(20)));
    in.push_event(test::touch_event(EventType::TouchUp, 0, 0.1f, 0.5f, at(20) + InputSystem::kSwipeMaxUs + 1));
    for (u64 t = 20; t <= 60; ++t) {
        in.sample_for_tick(t);
        CHECK(in.state().axis2(MCE_SID("Dash")).x == 0.f);
    }
}

TEST_CASE("touch input is independent of render pacing") {
    // The same touch stream delivered in one batch per tick vs. in odd-sized frame batches.
    struct Ev {
        EventType type;
        u32 slot;
        f32 x, y;
        u64 ts;
    };
    Array<Ev> stream;
    for (u64 i = 0; i < 600; ++i) {
        const u64 ts = 7'919 * i + 1'000;
        const u32 slot = static_cast<u32>(i / 40) % 3;
        const u64 phase = i % 40;
        const f32 x = (slot == 0 ? 0.2f : 0.7f) + 0.004f * static_cast<f32>(phase % 13);
        const f32 y = 0.5f + 0.003f * static_cast<f32>(phase % 7);
        const EventType t = phase == 0 ? EventType::TouchDown : phase == 39 ? EventType::TouchUp : EventType::TouchMove;
        stream.push_back({t, slot, x, y, ts});
    }
    const u64 ticks = 7'919 * 600 / kTickUs + 2;

    auto run = [&](u32 frame_events, InputRecorder& rec) {
        InputSystem in;
        setup(in);
        in.set_recorder(&rec);
        usize next = 0;
        for (u64 t = 1; t <= ticks; ++t) {
            // Deliver events in frame-sized batches as long as they could belong to this tick.
            while (next < stream.size() && stream[next].ts < in.tick_close_us(t)) {
                for (u32 k = 0; k < frame_events && next < stream.size(); ++k, ++next)
                    in.push_event(test::touch_event(stream[next].type, stream[next].slot, stream[next].x, stream[next].y,
                                                    stream[next].ts));
            }
            in.sample_for_tick(t);
        }
    };
    InputRecorder a, b;
    run(1, a);
    run(7, b);
    REQUIRE(a.bytes().size() == b.bytes().size());
    CHECK(std::memcmp(a.bytes().data(), b.bytes().data(), a.bytes().size()) == 0);
}

TEST_CASE("a recorded touch session replays and hashes identically") {
    InputRecorder live;
    u64 live_hash = 0;
    {
        InputSystem in;
        setup(in);
        in.set_recorder(&live);
        for (u64 t = 1; t <= 600; ++t) {
            const u64 ts = in.tick_close_us(t - 1) + 300;
            const u32 slot = static_cast<u32>(t / 50) % 2;
            const f32 x0 = 0.25f + 0.5f * static_cast<f32>(slot);
            const f32 k = static_cast<f32>(t % 50);
            if (t % 50 == 1) in.push_event(test::touch_event(EventType::TouchDown, slot, x0, 0.5f, ts));
            else if (t % 50 == 30) in.push_event(test::touch_event(EventType::TouchUp, slot, x0 + 0.05f, 0.4f, ts));
            else if (t % 50 < 30) in.push_event(test::touch_event(EventType::TouchMove, slot, x0 + 0.002f * k, 0.5f - 0.003f * k, ts));
            if (t % 50 == 40) {  // a tap in the pause corner
                in.push_event(test::touch_event(EventType::TouchDown, 5, 0.95f, 0.05f, ts));
                in.push_event(test::touch_event(EventType::TouchUp, 5, 0.95f, 0.05f, ts + 100));
            }
            live_hash = hash_state(in.sample_for_tick(t), live_hash);
        }
    }
    u64 replay_hash = 0;
    InputReplayer player(live.bytes());
    InputSystem in;
    setup(in);
    in.set_replayer(&player);
    u32 pauses = 0;
    for (u64 t = 1; t <= 600; ++t) {
        const InputState& s = in.sample_for_tick(t);
        pauses += s.pause_pressed ? 1 : 0;
        replay_hash = hash_state(s, replay_hash);
    }
    CHECK(player.done());
    CHECK(pauses == 12);
    CHECK(replay_hash == live_hash);
}

TEST_CASE("streams without touches keep their format and hashes") {
    InputState s;
    s.tick = 9;
    const u64 h = hash_state(s);
    InputRecorder rec;
    rec.record(s);
    // marker + tick + 10 floats + bits + counts: no touch block.
    CHECK(rec.bytes().size() == 4 + 8 + 40 + 2 + 1 + 1);
    s.touch_count = 1;
    s.touches[0].held = true;
    CHECK(hash_state(s) != h);
}

TEST_CASE("focus loss lifts every touch") {
    InputSystem in;
    setup(in);
    in.push_event(test::touch_event(EventType::TouchDown, 0, 0.2f, 0.5f, at(1)));
    in.sample_for_tick(1);
    in.push_event(test::simple_event(EventType::FocusOut, at(2)));
    in.sample_for_tick(2);
    REQUIRE(in.state().touch_count == 1);
    CHECK(in.state().touches[0].released);
    CHECK_FALSE(in.touch_held(0));
    in.sample_for_tick(3);
    CHECK(in.state().touch_count == 0);
}
