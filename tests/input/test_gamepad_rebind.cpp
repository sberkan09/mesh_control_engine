// v0.5 input (ROADMAP M6, ADR-0026): gamepad bindings through the same tick quantization as the
// keyboard, runtime rebinding capture, and persisted bindings (FR-INP-040) — all with synthetic
// platform events, no SDL.

#include <doctest/doctest.h>

#include <cmath>
#include <cstdio>
#include <cstring>

#include "input_test_util.h"
#include "test_support.h"

using namespace mce;
using namespace mce::input;
using platform::GamepadAxis;
using platform::GamepadButton;
using platform::Key;
using test::key_event;

namespace {

u64 in_tick(u64 n, u64 offset_us = 1000) { return ((n - 1) * 1'000'000ull) / 60 + offset_us; }

platform::Event pad_event(platform::EventType t, u32 slot, u32 code, f32 x, u64 ts) {
    platform::Event e;
    e.type = t;
    e.flags = slot;
    e.code = code;
    e.x = x;
    e.timestamp_us = ts;
    return e;
}
platform::Event pad_added(u32 slot, u64 ts) { return pad_event(platform::EventType::GamepadAdded, slot, 0, 0.f, ts); }
platform::Event pad_button(u32 slot, GamepadButton b, bool down, u64 ts) {
    return pad_event(down ? platform::EventType::GamepadButtonDown : platform::EventType::GamepadButtonUp, slot,
                     static_cast<u32>(b), 0.f, ts);
}
platform::Event pad_axis(u32 slot, GamepadAxis a, f32 v, u64 ts) {
    return pad_event(platform::EventType::GamepadAxis, slot, static_cast<u32>(a), v, ts);
}

const StringId kGameplay = MCE_SID("Gameplay");
const StringId kJump = MCE_SID("Jump");
const StringId kMove = MCE_SID("Move");

}  // namespace

TEST_CASE("gamepad: left stick and d-pad drive Move; South is Jump; deadzone and y-flip apply") {
    InputSystem in;
    test::setup_default(in);
    in.push_event(pad_added(0, in_tick(1)));
    in.push_event(pad_axis(0, GamepadAxis::LeftX, 0.1f, in_tick(1)));  // inside the 0.2 deadzone
    const InputState& s1 = in.sample_for_tick(1);
    CHECK(in.gamepad_connected(0));
    CHECK(s1.move.x == 0.f);

    in.push_event(pad_axis(0, GamepadAxis::LeftX, 0.6f, in_tick(2)));
    in.push_event(pad_axis(0, GamepadAxis::LeftY, -1.f, in_tick(2)));  // SDL: up is negative
    const InputState& s2 = in.sample_for_tick(2);
    CHECK(s2.move.x == doctest::Approx(0.5f / std::sqrt(1.25f)));  // (0.6-0.2)/0.8 = 0.5, then unit-clamped with y=1
    CHECK(s2.move.y == doctest::Approx(1.f / std::sqrt(1.25f)));

    in.push_event(pad_axis(0, GamepadAxis::LeftX, 0.f, in_tick(3)));
    in.push_event(pad_axis(0, GamepadAxis::LeftY, 0.f, in_tick(3)));
    in.push_event(pad_button(0, GamepadButton::DpadLeft, true, in_tick(3)));
    in.push_event(pad_button(0, GamepadButton::South, true, in_tick(3)));
    const InputState& s3 = in.sample_for_tick(3);
    CHECK(s3.move.x == -1.f);
    CHECK(s3.jump_pressed);

    in.push_event(pad_button(0, GamepadButton::South, false, in_tick(4)));
    const InputState& s4 = in.sample_for_tick(4);
    CHECK(s4.jump_released);
    CHECK(in.gamepad_held(0, GamepadButton::DpadLeft));
}

TEST_CASE("gamepad: a tap inside one tick still shows for a tick; any pad drives the action") {
    InputSystem in;
    test::setup_default(in);
    in.push_event(pad_added(0, in_tick(1)));
    in.push_event(pad_added(2, in_tick(1)));
    in.push_event(pad_button(2, GamepadButton::South, true, in_tick(1, 2000)));
    in.push_event(pad_button(2, GamepadButton::South, false, in_tick(1, 9000)));
    CHECK(in.sample_for_tick(1).jump_pressed);   // the 1-tick visibility floor
    CHECK(in.sample_for_tick(2).jump_released);  // release lands on the next tick

    // Two pads on the same axis: the larger deflection wins (no doubling).
    in.push_event(pad_axis(0, GamepadAxis::LeftX, 0.6f, in_tick(3)));
    in.push_event(pad_axis(2, GamepadAxis::LeftX, -1.f, in_tick(3)));
    CHECK(in.sample_for_tick(3).move.x == -1.f);

    // Unplugging pad 2 neutralizes what it held.
    in.push_event(pad_event(platform::EventType::GamepadRemoved, 2, 0, 0.f, in_tick(4)));
    CHECK(in.sample_for_tick(4).move.x == doctest::Approx(0.5f));
    CHECK_FALSE(in.gamepad_connected(2));

    // Focus loss releases pads too (stuck-input fix).
    in.push_event(test::simple_event(platform::EventType::FocusOut, in_tick(5)));
    CHECK(in.sample_for_tick(5).move.x == 0.f);
}

TEST_CASE("gamepad input is tick-quantized: identical sim-visible streams at any render rate") {
    // The FR-INP-030 corollary for pads: stick + button events at fixed timestamps, delivered in
    // render frames of different rates, produce the same InputState sequence.
    auto run = [](u32 render_hz) {
        InputSystem in;
        test::setup_default(in);
        Array<platform::Event> events;
        events.push_back(pad_added(0, 500));
        for (u32 i = 0; i < 120; ++i) {
            const u64 t = 1000 + i * 7919ull;  // ~126 Hz, unrelated to the tick grid
            events.push_back(pad_axis(0, GamepadAxis::LeftX, std::sin(static_cast<f32>(i) * 0.3f), t));
            if (i % 9 == 0) events.push_back(pad_button(0, GamepadButton::South, (i / 9) % 2 == 0, t + 13));
        }
        Array<u64> hashes;
        usize next = 0;
        const u64 frame_us = 1'000'000ull / render_hz;
        u64 tick = 1;
        for (u64 now = 0; now < 1'000'000; now += frame_us) {
            while (next < events.size() && events[next].timestamp_us < now) in.push_event(events[next++]);
            while (in.tick_close_us(tick) <= now) hashes.push_back(hash_state(in.sample_for_tick(tick++)));
        }
        return hashes;
    };
    const Array<u64> a = run(30), b = run(144);
    const usize n = a.size() < b.size() ? a.size() : b.size();
    REQUIRE(n > 50);
    for (usize i = 0; i < n; ++i) CHECK(a[i] == b[i]);
}

TEST_CASE("FR-INP-040: capture rebinding swallows the capturing press and applies next tick") {
    InputSystem in;
    test::setup_default(in);
    in.sample_for_tick(1);
    REQUIRE(in.begin_rebind(kGameplay, kJump, 0));
    CHECK(in.rebind_state() == RebindState::Waiting);
    CHECK_FALSE(in.begin_rebind(kGameplay, kJump, 9));
    CHECK_FALSE(in.begin_rebind(kGameplay, MCE_SID("NoSuchAction"), 0));

    REQUIRE(in.begin_rebind(kGameplay, kJump, 0));
    in.push_event(key_event(Key::K, true, in_tick(2)));
    const InputState& s2 = in.sample_for_tick(2);
    CHECK(in.rebind_state() == RebindState::Captured);
    CHECK(in.rebind_result().device == Device::Keyboard);
    CHECK(in.rebind_result().code == static_cast<u32>(Key::K));
    CHECK_FALSE(s2.jump_pressed);  // the capturing press never fires the action
    in.push_event(key_event(Key::K, false, in_tick(3)));
    in.sample_for_tick(3);

    in.push_event(key_event(Key::Space, true, in_tick(4)));
    CHECK_FALSE(in.sample_for_tick(4).jump_pressed);  // the old key no longer jumps
    in.push_event(key_event(Key::Space, false, in_tick(5)));
    in.push_event(key_event(Key::K, true, in_tick(5)));
    CHECK(in.sample_for_tick(5).jump_pressed);
    REQUIRE(in.binding_overrides().size() == 1);

    // Escape cancels and is not captured; a capture times out in sim time.
    REQUIRE(in.begin_rebind(kGameplay, kJump, 0, 0.5f));
    in.push_event(key_event(Key::K, false, in_tick(6)));
    in.push_event(key_event(Key::Escape, true, in_tick(6)));
    in.sample_for_tick(6);
    CHECK(in.rebind_state() == RebindState::Cancelled);
    REQUIRE(in.begin_rebind(kGameplay, kJump, 0, 0.5f));
    for (u64 t = 7; t < 7 + 31; ++t) in.sample_for_tick(t);
    CHECK(in.rebind_state() == RebindState::TimedOut);

    // Device filter: a keyboard press is ignored by a gamepad-only capture (and acts normally).
    REQUIRE(in.begin_rebind(kGameplay, kJump, 0, 5.f, RebindAccept::Gamepad));
    in.push_event(pad_added(0, in_tick(40)));
    in.push_event(key_event(Key::K, true, in_tick(40)));
    CHECK(in.sample_for_tick(40).jump_pressed);
    CHECK(in.rebind_state() == RebindState::Waiting);
    in.push_event(pad_button(0, GamepadButton::North, true, in_tick(41)));
    in.sample_for_tick(41);
    CHECK(in.rebind_state() == RebindState::Captured);
    CHECK(in.rebind_result().device == Device::GamepadButton);
}

TEST_CASE("FR-INP-040: a stick captured onto an axis binding keeps that binding's direction") {
    InputSystem in;
    test::setup_default(in);
    in.push_event(pad_added(0, in_tick(1)));
    in.sample_for_tick(1);
    // Move binding 1 is A (x, scale -1). Capture the right stick pushed left onto it.
    REQUIRE(in.begin_rebind(kGameplay, kMove, 1));
    in.push_event(pad_axis(0, GamepadAxis::RightX, -0.3f, in_tick(2)));  // too small: ignored
    in.push_event(pad_axis(0, GamepadAxis::RightX, -0.9f, in_tick(2, 5000)));
    in.sample_for_tick(2);
    REQUIRE(in.rebind_state() == RebindState::Captured);
    CHECK(in.rebind_result().device == Device::GamepadAxis);
    CHECK(in.rebind_result().scale == 1.f);  // (-1 binding) x (pushed left) -> raw stick value
    in.push_event(pad_axis(0, GamepadAxis::RightX, -1.f, in_tick(3)));
    CHECK(in.sample_for_tick(3).move.x == -1.f);  // pushing left moves left

    // On a Button action the push direction is the trigger direction.
    REQUIRE(in.begin_rebind(kGameplay, kJump, 0));
    in.push_event(pad_axis(0, GamepadAxis::LeftY, 0.95f, in_tick(4)));  // stick down
    in.sample_for_tick(4);
    CHECK(in.rebind_result().scale == 1.f);
    in.push_event(pad_axis(0, GamepadAxis::LeftY, 0.f, in_tick(5)));
    in.sample_for_tick(5);
    in.push_event(pad_axis(0, GamepadAxis::LeftY, 0.9f, in_tick(6)));
    CHECK(in.sample_for_tick(6).jump_pressed);
}

TEST_CASE("FR-INP-040: bindings round-trip through reflection and survive a restart") {
    const char* path = MCE_TEST_TMP_DIR "/bindings.txt";
    std::remove(path);
    auto stream = [](InputSystem& in, u64 first_tick) {
        Array<u64> h;
        in.push_event(pad_added(0, in_tick(first_tick)));
        in.push_event(key_event(Key::K, true, in_tick(first_tick + 1)));
        in.push_event(pad_button(0, GamepadButton::North, true, in_tick(first_tick + 2)));
        in.push_event(key_event(Key::K, false, in_tick(first_tick + 3)));
        in.push_event(pad_axis(0, GamepadAxis::RightX, -1.f, in_tick(first_tick + 3)));
        for (u64 t = first_tick; t < first_tick + 6; ++t) h.push_back(hash_state(in.sample_for_tick(t)));
        return h;
    };

    Array<u64> before;
    {
        InputSystem in;
        test::setup_default(in);
        CHECK(in.load_bindings(path).error().code == Errc::FileNotFound);  // first run: no save yet
        BindingDef k;
        k.code = static_cast<u32>(Key::K);
        REQUIRE(in.rebind(kGameplay, kJump, 0, k));
        BindingDef north;
        north.device = Device::GamepadButton;
        north.code = static_cast<u32>(GamepadButton::North);
        REQUIRE(in.rebind(kGameplay, MCE_SID("Fire"), 2, north));
        BindingDef stick;
        stick.device = Device::GamepadAxis;
        stick.code = static_cast<u32>(GamepadAxis::RightX);
        stick.dz = {0.25f, 0.95f};
        stick.curve = Curve::Quadratic;
        REQUIRE(in.rebind(kGameplay, kMove, 1, stick));
        CHECK(in.binding_overrides().size() == 3);

        Array<char> text;
        in.serialize_bindings(text);
        const StringView sv(text.data(), text.size());
        CHECK(sv.find("[BindingOverride]") != StringView::npos);
        CHECK(sv.find("device = GamepadAxis") != StringView::npos);  // enums by name
        CHECK(sv.find("curve = Quadratic") != StringView::npos);
        REQUIRE(in.save_bindings(path));
        before = stream(in, 2);
    }
    // "Restart": a fresh system on the cooked default map, then the save.
    InputSystem in2;
    test::setup_default(in2);
    REQUIRE(in2.load_bindings(path));
    CHECK(in2.binding_overrides().size() == 3);
    const Array<u64> after = stream(in2, 2);
    REQUIRE(after.size() == before.size());
    for (usize i = 0; i < before.size(); ++i) CHECK(after[i] == before[i]);

    // Reloading the cooked map keeps the player's bindings.
    Array<byte> chunk;
    default_map_chunk(chunk);
    REQUIRE(in2.load_map(chunk.span()));
    CHECK(in2.binding_overrides().size() == 3);

    // Rebinding back to the default leaves nothing to persist; reset clears everything.
    BindingDef space;
    space.code = static_cast<u32>(Key::Space);
    REQUIRE(in2.rebind(kGameplay, kJump, 0, space));
    CHECK(in2.binding_overrides().size() == 2);
    in2.reset_bindings();
    CHECK(in2.binding_overrides().empty());
    std::remove(path);
}

TEST_CASE("FR-INP-040: bad and stale binding files") {
    InputSystem in;
    test::setup_default(in);
    CHECK(in.deserialize_bindings("not a bindings file\n").error().code == Errc::BadMagic);
    CHECK(in.deserialize_bindings("# mce input bindings v1\ncode = 3\n").error().code == Errc::InvalidArgument);
    CHECK(in.deserialize_bindings("# mce input bindings v1\n[BindingOverride]\ndevice = Banana\n").error().code ==
          Errc::InvalidArgument);
    // Unknown fields are ignored (a newer build's file); an override for an action this map lacks
    // is dropped, the rest applied.
    char text[512];
    std::snprintf(text, sizeof(text),
                  "# mce input bindings v1\n"
                  "[BindingOverride]\nmap = 0x%016llx\naction = 0x%016llx\nindex = 0\ncode = 14\nfuture_field = 7\n"
                  "[BindingOverride]\nmap = 0x%016llx\naction = 0x%016llx\nindex = 0\ncode = 15\n",
                  static_cast<unsigned long long>(kGameplay.value), static_cast<unsigned long long>(kJump.value),
                  static_cast<unsigned long long>(kGameplay.value),
                  static_cast<unsigned long long>(MCE_SID("Gone").value));
    REQUIRE(in.deserialize_bindings(text));
    REQUIRE(in.binding_overrides().size() == 1);
    in.push_event(key_event(Key::K, true, in_tick(1)));  // code 14 = K
    CHECK(in.sample_for_tick(1).jump_pressed);
}
