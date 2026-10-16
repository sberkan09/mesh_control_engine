// Local players (M-VIEWS step 1): per-slot maps, per-slot pads, join by button, "P2/" script names.
// Synthetic platform events, no SDL.

#include <doctest/doctest.h>

#include "input_test_util.h"
#include "mce/input/replay.h"
#include "mce/input/script.h"
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
platform::Event pad_added(u32 pad, u64 ts) { return pad_event(platform::EventType::GamepadAdded, pad, 0, 0.f, ts); }
platform::Event pad_removed(u32 pad, u64 ts) { return pad_event(platform::EventType::GamepadRemoved, pad, 0, 0.f, ts); }
platform::Event pad_button(u32 pad, GamepadButton b, bool down, u64 ts) {
    return pad_event(down ? platform::EventType::GamepadButtonDown : platform::EventType::GamepadButtonUp, pad,
                     static_cast<u32>(b), 0.f, ts);
}
platform::Event pad_axis(u32 pad, GamepadAxis a, f32 v, u64 ts) {
    return pad_event(platform::EventType::GamepadAxis, pad, static_cast<u32>(a), v, ts);
}

const StringId kMove = MCE_SID("Move");
const StringId kJump = MCE_SID("Jump");

// Slot 0: WASD + Space + any pad. Slot 1: arrows + Enter + its own pad.
void load_two_players(InputSystem& in) {
    InputMapBuilder b;
    b.map("P1");
    b.action("Move", ActionType::Axis2D)
        .key(Key::D, 0, +1).key(Key::A, 0, -1).key(Key::W, 1, +1).key(Key::S, 1, -1)
        .gamepad_axis(GamepadAxis::LeftX, 0, +1.f);
    b.action("Jump").key(Key::Space).gamepad_button(GamepadButton::South);
    b.map("P2", map_player_flags(1));
    b.action("Move", ActionType::Axis2D)
        .key(Key::Right, 0, +1).key(Key::Left, 0, -1)
        .gamepad_axis(GamepadAxis::LeftX, 0, +1.f);
    b.action("Jump").key(Key::Return).gamepad_button(GamepadButton::South);
    Array<byte> bytes;
    b.encode(bytes);
    REQUIRE(in.load_map(bytes.span()).has_value());
}

}  // namespace

TEST_CASE("local players: each slot reads its own keys under its own names") {
    InputSystem in;
    load_two_players(in);
    in.push_event(key_event(Key::D, true, in_tick(1)));
    in.push_event(key_event(Key::Return, true, in_tick(1)));
    const InputState& s = in.sample_for_tick(1);
    CHECK(s.axis2(kMove).x == 1.f);
    CHECK(s.action(0, kMove).x == 1.f);
    CHECK_FALSE(s.held(kJump));
    CHECK(s.action(1, kJump).held);
    CHECK(s.action(1, kJump).pressed);
    CHECK(s.action(1, kMove).x == 0.f);  // P2's Move is its own action, not P1's
    CHECK(s.has_action(1, kMove));
    CHECK(player_action_id(0, kMove) == kMove);
    CHECK_FALSE(player_action_id(1, kMove) == kMove);
}

TEST_CASE("local players: a player map is never on the stack and cannot be pushed") {
    InputSystem in;
    load_two_players(in);
    CHECK(in.map_stack().size() == 1);
    CHECK_FALSE(in.push_map(MCE_SID("P2")));
}

TEST_CASE("local players: pads belong to one slot; player 0 without a pad keeps any unclaimed pad") {
    InputSystem in;
    load_two_players(in);
    in.push_event(pad_added(0, in_tick(1)));
    in.push_event(pad_added(1, in_tick(1)));
    in.push_event(pad_axis(0, GamepadAxis::LeftX, 1.f, in_tick(1)));
    in.push_event(pad_axis(1, GamepadAxis::LeftX, -1.f, in_tick(1)));
    const InputState& a = in.sample_for_tick(1);
    // Nothing assigned: player 0 reads any pad (the stronger deflection wins, as before), player 2 none.
    CHECK(a.axis2(kMove).x != 0.f);
    CHECK(a.action(1, kMove).x == 0.f);

    REQUIRE(in.assign_pad(1, 1));
    in.push_event(pad_axis(0, GamepadAxis::LeftX, 1.f, in_tick(2)));
    in.push_event(pad_axis(1, GamepadAxis::LeftX, -1.f, in_tick(2)));
    const InputState& b = in.sample_for_tick(2);
    CHECK(b.axis2(kMove).x == doctest::Approx(1.f));       // pad 0, the pad nobody else holds
    CHECK(b.action(1, kMove).x == doctest::Approx(-1.f));  // pad 1, assigned to slot 1

    REQUIRE(in.assign_pad(0, 1));  // stealing a pad releases it from the old slot
    CHECK(in.pad_of(1) == -1);
    CHECK(in.pad_of(0) == 1);
    CHECK_FALSE(in.assign_pad(0, 9));
    CHECK_FALSE(in.assign_pad(9, 0));
}

TEST_CASE("local players: join by South or Start, lowest free slot; a removed pad frees its slot") {
    InputSystem in;
    load_two_players(in);
    in.set_join_slots(0, 2);
    in.push_event(pad_added(0, in_tick(1)));
    in.push_event(pad_added(2, in_tick(1)));
    in.push_event(pad_button(2, GamepadButton::Start, true, in_tick(2)));
    in.sample_for_tick(1);
    in.sample_for_tick(2);
    CHECK(in.pad_of(0) == 2);
    in.push_event(pad_button(0, GamepadButton::South, true, in_tick(3)));
    in.sample_for_tick(3);
    CHECK(in.pad_of(1) == 0);
    // A third pad finds no free slot in [0, 2).
    in.push_event(pad_added(3, in_tick(4)));
    in.push_event(pad_button(3, GamepadButton::South, true, in_tick(4)));
    in.sample_for_tick(4);
    CHECK(in.pad_of(0) == 2);
    CHECK(in.pad_of(1) == 0);
    in.push_event(pad_removed(2, in_tick(5)));
    in.sample_for_tick(5);
    CHECK(in.pad_of(0) == -1);
    in.set_join_slots(0, 0);
}

TEST_CASE("local players: a recording of two players replays to the same snapshots") {
    InputSystem in;
    load_two_players(in);
    InputRecorder rec;
    in.set_recorder(&rec);
    Array<InputState> seen;
    for (u64 t = 1; t <= 5; ++t) {
        if (t == 2) in.push_event(key_event(Key::D, true, in_tick(t)));
        if (t == 3) in.push_event(key_event(Key::Return, true, in_tick(t)));
        seen.push_back(in.sample_for_tick(t));
    }
    in.set_recorder(nullptr);

    InputSystem again;
    load_two_players(again);
    InputReplayer rep(rec.bytes());
    again.set_replayer(&rep);
    for (u64 t = 1; t <= 5; ++t) {
        const InputState& s = again.sample_for_tick(t);
        CHECK(s == seen[t - 1]);
        CHECK(hash_state(s) == hash_state(seen[t - 1]));
    }
    CHECK(again.state().action(1, kJump).held);
}

TEST_CASE("local players: single-player maps publish exactly the names they always did") {
    InputSystem in;
    test::setup_default(in);
    const InputState& s = in.sample_for_tick(1);
    for (u32 slot = 1; slot < kMaxLocalPlayers; ++slot) CHECK_FALSE(s.has_action(slot, kMove));
    CHECK(s.has_action(kMove));
}

TEST_CASE("local players: a P2/ script name drives the second player") {
    InputScript script;
    REQUIRE(script.parse("1-3 P2/Move=1,0 P1/Jump\n2 P2/Jump\n").has_value());
    const InputState& s = script.evaluate(2);
    CHECK(s.action(1, kMove).x == 1.f);
    CHECK(s.action(1, kJump).held);
    CHECK(s.held(kJump));              // P1/ is the plain name
    CHECK_FALSE(s.has_action(kMove));  // P2's Move is not P1's
    CHECK(s.action(1, kJump).pressed);
}

TEST_CASE("local players: a map with a slot beyond the maximum is rejected") {
    InputMapBuilder b;
    b.map("Bad", map_player_flags(5));
    b.action("Move", ActionType::Axis2D).key(Key::D, 0, +1);
    Array<byte> bytes;
    b.encode(bytes);
    InputSystem in;
    CHECK_FALSE(in.load_map(bytes.span()).has_value());
}
