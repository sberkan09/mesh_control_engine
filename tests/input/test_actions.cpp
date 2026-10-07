#include <doctest/doctest.h>

#include <cmath>
#include <cstring>

#include "input_test_util.h"
#include "mce/input/ecs.h"
#include "test_support.h"

using namespace mce;
using namespace mce::input;
using platform::Key;
using test::key_event;

namespace {
// Tick N closes at N * 1e6 / 60 µs; this lands safely inside tick N's window.
u64 in_tick(u64 n, u64 offset_us = 1000) { return ((n - 1) * 1'000'000ull) / 60 + offset_us; }
}  // namespace

TEST_CASE("input map chunk round-trips and rejects corrupt data") {
    Array<byte> chunk;
    default_map_chunk(chunk);
    InputMapSet set;
    REQUIRE(decode_input_map(chunk.span(), set).has_value());
    CHECK(set.maps.size() == 2);
    CHECK(set.find_map(MCE_SID("Gameplay")) != nullptr);
    CHECK(set.find_map(MCE_SID("UI"))->flags == MapFlag::BlocksBelow);

    Array<byte> again;
    encode_input_map(set, again);
    REQUIRE(again.size() == chunk.size());
    CHECK(std::memcmp(again.data(), chunk.data(), chunk.size()) == 0);  // cook output is byte-stable

    InputMapSet bad;
    Array<byte> corrupt = chunk;
    corrupt[0] = byte{0};
    CHECK(decode_input_map(corrupt.span(), bad).error().code == Errc::BadMagic);
    CHECK(decode_input_map(Span<const byte>(chunk.data(), chunk.size() - 3), bad).error().code == Errc::Truncated);

    InputMapHeader h;
    std::memcpy(&h, chunk.data(), sizeof(h));
    h.version = kInputMapVersion + 1;
    std::memcpy(corrupt.data(), &h, sizeof(h));
    CHECK(decode_input_map(corrupt.span(), bad).error().code == Errc::VersionTooNew);

    // An action pointing past the binding table is rejected, not trusted.
    InputMapSet broken;
    REQUIRE(decode_input_map(chunk.span(), broken).has_value());
    broken.actions[0].binding_count = 500;
    Array<byte> enc;
    encode_input_map(broken, enc);
    CHECK(decode_input_map(enc.span(), bad).error().code == Errc::InvalidArgument);
}

TEST_CASE("composite WASD drives Move with opposing keys cancelling and diagonals normalized") {
    InputSystem in;
    test::setup_default(in);
    in.push_event(key_event(Key::D, true, in_tick(1)));
    const InputState& s1 = in.sample_for_tick(1);
    CHECK(s1.move.x == 1.f);
    CHECK(s1.move.y == 0.f);

    in.push_event(key_event(Key::W, true, in_tick(2)));
    const InputState& s2 = in.sample_for_tick(2);
    CHECK(s2.move.x == doctest::Approx(std::sqrt(0.5f)));
    CHECK(s2.move.y == doctest::Approx(std::sqrt(0.5f)));
    CHECK(std::sqrt(s2.move.x * s2.move.x + s2.move.y * s2.move.y) == doctest::Approx(1.f));

    in.push_event(key_event(Key::A, true, in_tick(3)));
    const InputState& s3 = in.sample_for_tick(3);
    CHECK(s3.move.x == 0.f);  // D and A cancel
    CHECK(s3.move.y == 1.f);
    CHECK(s3.axis2(MCE_SID("Move")).y == 1.f);
}

TEST_CASE("button edges are derived per tick from folded state") {
    InputSystem in;
    test::setup_default(in);

    in.push_event(key_event(Key::Space, true, in_tick(1)));
    const InputState s1 = in.sample_for_tick(1);
    CHECK(s1.jump_pressed);
    CHECK(s1.jump_held);
    CHECK_FALSE(s1.jump_released);
    CHECK(s1.action(MCE_SID("Jump")).held_time == doctest::Approx(1.f / 60.f));

    const InputState s2 = in.sample_for_tick(2);
    CHECK_FALSE(s2.jump_pressed);  // held, no new edge
    CHECK(s2.jump_held);
    CHECK(s2.action(MCE_SID("Jump")).held_time == doctest::Approx(2.f / 60.f));

    in.push_event(key_event(Key::Space, false, in_tick(3)));
    const InputState s3 = in.sample_for_tick(3);
    CHECK(s3.jump_released);
    CHECK_FALSE(s3.jump_held);
    CHECK(s3.action(MCE_SID("Jump")).held_time == 0.f);

    // Release with no prior press is a no-op.
    in.push_event(key_event(Key::J, false, in_tick(4)));
    const InputState s4 = in.sample_for_tick(4);
    CHECK_FALSE(s4.fire_pressed);
    CHECK_FALSE(s4.held(MCE_SID("Fire")));
    CHECK_FALSE(s4.released(MCE_SID("Fire")));
}

TEST_CASE("taps shorter than a tick still show for one tick (1-tick visibility floor)") {
    InputSystem in;
    test::setup_default(in);

    // press + release inside tick 1's window
    in.push_event(key_event(Key::Space, true, in_tick(1, 1000)));
    in.push_event(key_event(Key::Space, false, in_tick(1, 3000)));
    const InputState s1 = in.sample_for_tick(1);
    CHECK(s1.jump_pressed);
    CHECK(s1.jump_held);
    const InputState s2 = in.sample_for_tick(2);
    CHECK(s2.jump_released);
    CHECK_FALSE(s2.jump_held);

    // press / release / press inside one window: final state wins, one pressed edge
    in.push_event(key_event(Key::Space, true, in_tick(3, 1000)));
    in.push_event(key_event(Key::Space, false, in_tick(3, 2000)));
    in.push_event(key_event(Key::Space, true, in_tick(3, 3000)));
    const InputState s3 = in.sample_for_tick(3);
    CHECK(s3.jump_pressed);
    CHECK(s3.jump_held);
    const InputState s4 = in.sample_for_tick(4);
    CHECK(s4.jump_held);
    CHECK_FALSE(s4.jump_pressed);
    CHECK_FALSE(s4.jump_released);
}

TEST_CASE("OS key repeat is not a new press") {
    InputSystem in;
    test::setup_default(in);
    in.push_event(key_event(Key::Space, true, in_tick(1)));
    CHECK(in.sample_for_tick(1).jump_pressed);
    in.push_event(key_event(Key::Space, true, in_tick(2), platform::EventFlag::Repeat));
    CHECK_FALSE(in.sample_for_tick(2).jump_pressed);
}

TEST_CASE("focus loss zeroes held state and fires release edges once") {
    InputSystem in;
    test::setup_default(in);
    in.push_event(key_event(Key::D, true, in_tick(1)));
    in.push_event(key_event(Key::Space, true, in_tick(1)));
    CHECK(in.sample_for_tick(1).jump_held);

    in.push_event(test::simple_event(platform::EventType::FocusOut, in_tick(2)));
    const InputState s2 = in.sample_for_tick(2);
    CHECK_FALSE(s2.jump_held);
    CHECK(s2.jump_released);
    CHECK(s2.move.x == 0.f);
    CHECK(s2.released(MCE_SID("Move")));
    CHECK_FALSE(in.key_held(Key::Space));

    const InputState s3 = in.sample_for_tick(3);
    CHECK_FALSE(s3.jump_released);  // exactly once
}

TEST_CASE("editor capture: captured presses are ignored, releases still land") {
    InputSystem in;
    test::setup_default(in);
    using platform::EventFlag::ConsumedByEditor;

    in.push_event(key_event(Key::W, true, in_tick(1), ConsumedByEditor));  // typing "w" in a field
    CHECK(in.sample_for_tick(1).move.y == 0.f);

    in.push_event(key_event(Key::D, true, in_tick(2)));  // game view focused
    CHECK(in.sample_for_tick(2).move.x == 1.f);
    in.push_event(key_event(Key::D, false, in_tick(3), ConsumedByEditor));  // released over a panel
    const InputState s3 = in.sample_for_tick(3);
    CHECK(s3.move.x == 0.f);
    CHECK(in.stats().events_ignored == 1);
}

TEST_CASE("overlay maps: UI shadows gameplay; BlocksBelow hides the rest; pop restores") {
    InputSystem in;
    test::setup_default(in);
    in.push_event(key_event(Key::Space, true, in_tick(1)));
    const InputState s1 = in.sample_for_tick(1);
    CHECK(s1.jump_pressed);
    CHECK_FALSE(s1.confirm_pressed);

    REQUIRE(in.push_map(MCE_SID("UI")));
    in.push_event(key_event(Key::Space, false, in_tick(2)));
    in.sample_for_tick(2);
    in.push_event(key_event(Key::Space, true, in_tick(3)));
    const InputState s3 = in.sample_for_tick(3);
    CHECK(s3.confirm_pressed);
    CHECK_FALSE(s3.jump_pressed);
    CHECK_FALSE(s3.has_action(MCE_SID("Jump")));  // blocked, not merely released

    in.pop_map();
    const InputState s4 = in.sample_for_tick(4);
    CHECK(s4.has_action(MCE_SID("Jump")));
    CHECK(s4.jump_pressed);  // Space is still down: it re-reads as a fresh press for gameplay
    CHECK_FALSE(in.push_map(MCE_SID("NoSuchMap")));
}

TEST_CASE("unknown action names read as released with no crash") {
    InputSystem in;
    test::setup_default(in);
    const InputState& s = in.sample_for_tick(1);
    CHECK_FALSE(s.pressed(MCE_SID("Dash")));
    CHECK(s.axis(MCE_SID("Dash")) == 0.f);
}

TEST_CASE("deadzones and response curves") {
    InputMapBuilder b;
    BindingDef wheel;
    wheel.device = Device::MouseWheel;
    wheel.axis = 1;
    b.map(MCE_SID("M"));
    b.action(MCE_SID("Zoom"), ActionType::Axis1D).bind(wheel);
    BindingDef curved;
    curved.device = Device::Keyboard;
    curved.code = static_cast<u32>(Key::E);
    curved.curve = Curve::Smooth;
    curved.scale = 0.5f;
    b.action(MCE_SID("Half"), ActionType::Axis1D).bind(curved);
    BindingDef inv = curved;
    inv.curve = Curve::Linear;
    inv.flags = BindingFlag::Invert;
    inv.scale = 1.f;
    b.action(MCE_SID("Inv"), ActionType::Axis1D).bind(inv);
    Array<byte> chunk;
    b.encode(chunk);

    InputSystem in;
    REQUIRE(in.load_map(chunk.span()).has_value());
    in.set_tick_origin_us(0);
    platform::Event w = test::simple_event(platform::EventType::MouseWheel, in_tick(1));
    w.y = 3.f;
    in.push_event(w);
    in.push_event(key_event(Key::E, true, in_tick(1)));
    const InputState& s = in.sample_for_tick(1);
    CHECK(s.axis(MCE_SID("Zoom")) == 3.f);  // unbounded devices are not clamped
    CHECK(s.axis(MCE_SID("Half")) == 0.5f);
    CHECK(s.axis(MCE_SID("Inv")) == -1.f);
    CHECK(s.mouse_wheel().y == 3.f);

    // Deadzone/curve math on an analog value goes through the same helpers once gamepads land;
    // here: wheel resets per tick.
    CHECK(in.sample_for_tick(2).axis(MCE_SID("Zoom")) == 0.f);
}

TEST_CASE("in-session rebinding applies from the next sampled tick") {
    InputSystem in;
    test::setup_default(in);
    in.push_event(key_event(Key::K, true, in_tick(1)));
    CHECK_FALSE(in.sample_for_tick(1).jump_held);

    BindingDef k;
    k.code = static_cast<u32>(Key::K);
    REQUIRE(in.rebind(MCE_SID("Gameplay"), MCE_SID("Jump"), 0, k));
    const InputState& s2 = in.sample_for_tick(2);
    CHECK(s2.jump_held);
    CHECK(s2.jump_pressed);
    CHECK_FALSE(in.rebind(MCE_SID("Gameplay"), MCE_SID("Jump"), 5, k));
}

TEST_CASE("mouse delta and text accumulate over the tick window") {
    InputSystem in;
    test::setup_default(in);
    in.push_event(test::mouse_move(100.f, 100.f, in_tick(1, 100)));
    in.push_event(test::mouse_move(110.f, 95.f, in_tick(1, 200)));
    in.push_event(test::mouse_move(130.f, 90.f, in_tick(1, 300)));
    in.push_event(test::simple_event(platform::EventType::TextInput, in_tick(1, 400), 'h'));
    in.push_event(test::simple_event(platform::EventType::TextInput, in_tick(1, 500), 0x00FC));  // ü
    const InputState s1 = in.sample_for_tick(1);
    CHECK(s1.look.x == 30.f);
    CHECK(s1.look.y == -10.f);
    CHECK(s1.mouse_pos_px().x == 130.f);
    CHECK(s1.text() == StringView("h\xC3\xBC"));
    const InputState s2 = in.sample_for_tick(2);
    CHECK(s2.look.x == 0.f);
    CHECK(s2.text().empty());
}

TEST_CASE("InputState is published as a world resource") {
    World w;
    install(w);
    REQUIRE(w.try_resource<InputState>() != nullptr);
    InputSystem in;
    test::setup_default(in);
    in.push_event(key_event(Key::Space, true, in_tick(1)));
    publish(w, in.sample_for_tick(1));
    CHECK(w.resource<InputState>().jump_pressed);
    CHECK(w.resource<InputState>().tick == 1);
}
