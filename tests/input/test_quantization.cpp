// FR-INP-030, the load-bearing test: a tick's input is a pure function of event timestamps
// against the fixed tick grid. Three render loops pacing 30/90/240 Hz (plus a jittery one and a
// stalling one) see the same synthetic event stream and must produce identical per-tick
// InputState sequences.

#include <doctest/doctest.h>

#include "input_test_util.h"
#include "mce/input/replay.h"

using namespace mce;
using namespace mce::input;
using platform::Key;

namespace {

constexpr u64 kTicks = 600;  // 10 s at 60 Hz
constexpr u64 kEnd = (kTicks * 1'000'000ull) / 60;

struct Rng {
    u64 s;
    u64 next() {
        s ^= s << 13;
        s ^= s >> 7;
        s ^= s << 17;
        return s;
    }
    u64 below(u64 n) { return next() % n; }
};

Array<platform::Event> make_stream(u64 seed) {
    Array<platform::Event> ev;
    Rng rng{seed};
    const Key keys[] = {Key::W, Key::A, Key::S, Key::D, Key::Space, Key::J, Key::Escape};
    bool down[7] = {};
    u64 t = 500;
    f32 mx = 400, my = 300;
    while (t < kEnd - 50'000) {
        t += 200 + rng.below(40'000);  // 0.2–40 ms apart: many land several-per-tick
        // Every ~10th event sits exactly on a tick boundary (belongs to the next tick).
        if (rng.below(10) == 0) t = ((t * 60) / 1'000'000 + 1) * 1'000'000 / 60;
        const u64 kind = rng.below(10);
        if (kind < 7) {
            const u32 k = static_cast<u32>(kind);
            down[k] = !down[k];
            ev.push_back(test::key_event(keys[k], down[k], t));
            // Sometimes a tap shorter than a tick.
            if (down[k] && rng.below(4) == 0) {
                down[k] = false;
                ev.push_back(test::key_event(keys[k], false, t + 1 + rng.below(4000)));
            }
        } else {
            mx += static_cast<f32>(rng.below(21)) - 10.f;
            my += static_cast<f32>(rng.below(21)) - 10.f;
            ev.push_back(test::mouse_move(mx, my, t));
        }
    }
    // Delivery order within a frame is by arrival; keep it sorted like SDL does.
    for (usize i = 1; i < ev.size(); ++i)
        for (usize j = i; j > 0 && ev[j - 1].timestamp_us > ev[j].timestamp_us; --j) {
            const platform::Event tmp = ev[j - 1];
            ev[j - 1] = ev[j];
            ev[j] = tmp;
        }
    return ev;
}

// Runs a render loop whose frame k starts at frame_time(k): deliver everything stamped up to
// "now", then run every tick whose window has closed — exactly what the fixed-step loop does.
template <class FrameTime>
void run_loop(const Array<platform::Event>& stream, FrameTime frame_time, InputRecorder& rec) {
    InputSystem in;
    test::setup_default(in);
    in.set_recorder(&rec);
    usize next_event = 0;
    u64 tick = 0;
    for (u64 frame = 0; tick < kTicks; ++frame) {
        const u64 now = frame_time(frame);
        const usize first = next_event;
        while (next_event < stream.size() && stream[next_event].timestamp_us <= now) ++next_event;
        in.begin_frame(Span<const platform::Event>(stream.data() + first, next_event - first));
        while (tick < kTicks && in.tick_close_us(tick + 1) <= now) in.sample_for_tick(++tick);
    }
}

void check_identical(const InputRecorder& a, const InputRecorder& b) {
    REQUIRE(a.ticks() == kTicks);
    REQUIRE(b.ticks() == kTicks);
    InputReplayer ra(a.bytes()), rb(b.bytes());
    InputState sa, sb;
    for (u64 t = 1; t <= kTicks; ++t) {
        REQUIRE(ra.next(sa));
        REQUIRE(rb.next(sb));
        INFO("tick " << t);
        REQUIRE(sa == sb);
        REQUIRE(hash_state(sa) == hash_state(sb));
    }
}

}  // namespace

TEST_CASE("FR-INP-030: identical sim-visible input at 30/90/240 Hz render rates") {
    for (u64 seed : {0x1234567ull, 0xDEADBEEFull, 0x5EEDull}) {
        const Array<platform::Event> stream = make_stream(seed);
        REQUIRE(stream.size() > 300);

        InputRecorder r30, r90, r240, rjitter, rstall;
        run_loop(stream, [](u64 f) { return (f * 1'000'000ull) / 30; }, r30);
        run_loop(stream, [](u64 f) { return (f * 1'000'000ull) / 90; }, r90);
        run_loop(stream, [](u64 f) { return (f * 1'000'000ull) / 240; }, r240);
        // Irregular frame pacing: 3–25 ms frames.
        run_loop(stream, [](u64 f) {
            u64 t = 0;
            Rng r{f * 7919 + 1};
            for (u64 i = 0; i < f; ++i) t += 3000 + (r.next() % 22'000);
            return t;
        }, rjitter);
        // A loop that stalls for 250 ms every second, then catches up in one burst.
        run_loop(stream, [](u64 f) {
            const u64 t = (f * 1'000'000ull) / 60;
            return (t % 1'000'000) < 250'000 ? (t / 1'000'000) * 1'000'000 + 250'000 : t;
        }, rstall);

        check_identical(r30, r90);
        check_identical(r30, r240);
        check_identical(r30, rjitter);
        check_identical(r30, rstall);
    }
}

TEST_CASE("tick windows use exact rational boundaries") {
    InputSystem in;
    in.set_tick_origin_us(1'000'000);
    in.set_tick_rate(60);
    CHECK(in.tick_close_us(1) == 1'016'666);
    CHECK(in.tick_close_us(3) == 1'050'000);
    CHECK(in.tick_close_us(60) == 2'000'000);       // no drift after a second
    CHECK(in.tick_close_us(216'000) == 3'601'000'000);  // ... or an hour
}

TEST_CASE("events stamped at or after a tick's close wait for the next tick") {
    InputSystem in;
    test::setup_default(in);
    const u64 close1 = in.tick_close_us(1);
    in.push_event(test::key_event(Key::Space, true, close1));  // exactly on the boundary
    CHECK_FALSE(in.sample_for_tick(1).jump_held);
    CHECK(in.sample_for_tick(2).jump_pressed);

    // Arrival order does not matter, timestamps do.
    in.push_event(test::key_event(Key::Space, false, in.tick_close_us(3) + 10));
    in.push_event(test::key_event(Key::J, true, in.tick_close_us(2) + 10));
    const InputState s3 = in.sample_for_tick(3);
    CHECK(s3.fire_pressed);
    CHECK(s3.jump_held);
    CHECK(in.sample_for_tick(4).jump_released);
}

TEST_CASE("event queue overflow drops the oldest without growing") {
    InputSystem in;
    test::setup_default(in);
    for (u32 i = 0; i < InputSystem::kQueueCapacity + 100; ++i)
        in.push_event(test::mouse_move(static_cast<f32>(i), 0.f, 1000 + i));
    CHECK(in.queued_events() == InputSystem::kQueueCapacity);
    CHECK(in.stats().events_dropped == 100);
    in.sample_for_tick(1);
    CHECK(in.state().mouse_pos_px().x == static_cast<f32>(InputSystem::kQueueCapacity + 99));
}
