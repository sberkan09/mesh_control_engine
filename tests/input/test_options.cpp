// M-ACCESS step 3: what an options screen needs from mce_input: action labels in the cooked map
// (chunk v2, v1 still read), readable binding names, look sensitivity, hold-to-toggle, one-hand
// keyboard presets and the bindings generation a host saves on.

#include <doctest/doctest.h>

#include <cstring>

#include "input_test_util.h"
#include "mce/input/binding_names.h"

using namespace mce;
using namespace mce::input;
using platform::GamepadAxis;
using platform::GamepadButton;
using platform::Key;
using test::key_event;

namespace {
u64 at_tick(u64 n) { return ((n - 1) * 1'000'000ull) / 60 + 1000; }

const BindingDef* binding(const InputSystem& in, StringId map, StringId action, u32 index) {
    const InputMapSet& s = in.maps();
    const ActionMapDef* m = s.find_map(map);
    if (!m) return nullptr;
    for (u32 i = 0; i < m->action_count; ++i) {
        const ActionDef& a = s.actions[m->first_action + i];
        if (a.name == action && index < a.binding_count) return &s.bindings[a.first_binding + index];
    }
    return nullptr;
}
}  // namespace

TEST_CASE("M-ACCESS: the cooked map keeps action labels (v2) and still reads v1 chunks") {
    Array<byte> chunk;
    default_map_chunk(chunk);
    InputMapSet set;
    REQUIRE(decode_input_map(chunk.span(), set).has_value());
    REQUIRE(set.labels.size() == set.actions.size());
    CHECK(set.label(0) == StringView("Move"));
    for (usize i = 0; i < set.actions.size(); ++i) CHECK(StringId(set.label(i)) == set.actions[i].name);
    CHECK(set.label(9999).empty());

    // An id-only map encodes as v1 and decodes with no labels.
    InputMapBuilder b;
    b.map(MCE_SID("Gameplay"));
    b.action(MCE_SID("Jump")).key(Key::Space);
    Array<byte> v1;
    b.encode(v1);
    InputMapHeader h;
    std::memcpy(&h, v1.data(), sizeof(h));
    CHECK(h.version == 1);
    InputMapSet old;
    REQUIRE(decode_input_map(v1.span(), old).has_value());
    CHECK(old.labels.empty());
    CHECK(old.label(0).empty());

    // A truncated label table is an error, not a crash.
    chunk.resize(chunk.size() - 2);
    InputMapSet cut;
    CHECK_FALSE(decode_input_map(chunk.span(), cut).has_value());
}

TEST_CASE("M-ACCESS: binding names read the way a player says them") {
    BindingName n;
    BindingDef b;
    b.device = Device::Keyboard;
    b.code = static_cast<u32>(Key::W);
    binding_name(b, n);
    CHECK(n.view() == StringView("W"));
    b.code = static_cast<u32>(Key::LShift);
    binding_name(b, n);
    CHECK(n.view() == StringView("Left Shift"));
    b.code = static_cast<u32>(Key::Num0);
    binding_name(b, n);
    CHECK(n.view() == StringView("0"));
    b.device = Device::MouseButton;
    b.code = static_cast<u32>(platform::MouseButton::Right);
    binding_name(b, n);
    CHECK(n.view() == StringView("Mouse Right"));
    b.device = Device::GamepadButton;
    b.code = static_cast<u32>(GamepadButton::South);
    binding_name(b, n);
    CHECK(n.view() == StringView("Pad South"));
    b.device = Device::GamepadAxis;
    b.code = static_cast<u32>(GamepadAxis::LeftY);
    b.scale = -1.f;  // the default map's Move up: sticks report y down-positive
    binding_name(b, n);
    CHECK(n.view() == StringView("Left Stick Up"));
    b.scale = 1.f;
    b.flags = BindingFlag::Invert;
    binding_name(b, n);
    CHECK(n.view() == StringView("Left Stick Up"));
    for (u32 k = 0; k < 256; ++k) CHECK(key_name(static_cast<Key>(k))[0] != '\0');
}

TEST_CASE("M-ACCESS: look sensitivity scales Look only, and hold-to-toggle latches a button") {
    InputSystem in;
    test::setup_default(in);
    in.set_look_sensitivity(2.0f);
    in.push_event(test::mouse_move(10, 10, at_tick(1)));
    in.sample_for_tick(1);
    in.push_event(test::mouse_move(13, 10, at_tick(2)));
    in.push_event(key_event(Key::D, true, at_tick(2)));
    const InputState& s = in.sample_for_tick(2);
    CHECK(s.look.x == doctest::Approx(6.0f));
    CHECK(s.move.x == doctest::Approx(1.0f));  // Move is untouched
    in.push_event(key_event(Key::D, false, at_tick(3)));

    in.set_toggle(MCE_SID("Jump"), true);
    CHECK(in.is_toggle(MCE_SID("Jump")));
    in.push_event(key_event(Key::Space, true, at_tick(3)));
    CHECK(in.sample_for_tick(3).jump_pressed);
    in.push_event(key_event(Key::Space, false, at_tick(4)));
    const InputState& s4 = in.sample_for_tick(4);
    CHECK(s4.jump_held);  // let go: still held
    CHECK_FALSE(s4.jump_released);
    CHECK(in.sample_for_tick(5).jump_held);
    in.push_event(key_event(Key::Space, true, at_tick(6)));
    const InputState& s6 = in.sample_for_tick(6);
    CHECK_FALSE(s6.jump_held);  // the second press releases
    CHECK(s6.jump_released);
    in.push_event(key_event(Key::Space, false, at_tick(7)));
    CHECK_FALSE(in.sample_for_tick(7).jump_held);
    in.set_toggle(MCE_SID("Jump"), false);
    in.push_event(key_event(Key::Space, true, at_tick(8)));
    CHECK(in.sample_for_tick(8).jump_held);
    in.push_event(key_event(Key::Space, false, at_tick(9)));
    CHECK_FALSE(in.sample_for_tick(9).jump_held);
}

TEST_CASE("M-ACCESS: one-hand presets move the keyboard to one side and persist as overrides") {
    InputSystem in;
    test::setup_default(in);
    const StringId gp = MCE_SID("Gameplay");
    const u64 g0 = in.bindings_generation();
    in.apply_hand_preset(Hand::Right);
    CHECK(in.bindings_generation() > g0);
    // Move's WASD half becomes the arrows; Jump and Fire take right-hand keys; Escape stays.
    CHECK(binding(in, gp, MCE_SID("Move"), 0)->code == static_cast<u32>(Key::Right));  // D
    CHECK(binding(in, gp, MCE_SID("Move"), 1)->code == static_cast<u32>(Key::Left));   // A
    CHECK(binding(in, gp, MCE_SID("Move"), 2)->code == static_cast<u32>(Key::Up));     // W
    const u32 jump = binding(in, gp, MCE_SID("Jump"), 0)->code;
    const u32 fire = binding(in, gp, MCE_SID("Fire"), 0)->code;
    CHECK(jump == static_cast<u32>(Key::Return));
    CHECK(fire != jump);
    CHECK(binding(in, gp, MCE_SID("Pause"), 0)->code == static_cast<u32>(Key::Escape));
    CHECK(binding(in, gp, MCE_SID("Fire"), 1)->device == Device::MouseButton);  // the mouse stays
    CHECK_FALSE(in.binding_overrides().empty());

    // It plays: the right hand moves and jumps.
    in.push_event(key_event(Key::Right, true, at_tick(1)));
    in.push_event(key_event(Key::Return, true, at_tick(1)));
    const InputState& s = in.sample_for_tick(1);
    CHECK(s.move.x == doctest::Approx(1.0f));
    CHECK(s.jump_pressed);

    // Saved and loaded like any rebind.
    Array<char> text;
    in.serialize_bindings(text);
    InputSystem other;
    test::setup_default(other);
    REQUIRE(other.deserialize_bindings(StringView(text.data(), text.size())).has_value());
    CHECK(binding(other, gp, MCE_SID("Jump"), 0)->code == static_cast<u32>(Key::Return));

    in.apply_hand_preset(Hand::Left);
    CHECK(binding(in, gp, MCE_SID("Move"), 4)->code == static_cast<u32>(Key::D));  // the arrows' Right
    CHECK(binding(in, gp, MCE_SID("Jump"), 0)->code == static_cast<u32>(Key::Space));
    in.apply_hand_preset(Hand::Both);
    CHECK(in.binding_overrides().empty());
}

TEST_CASE("toggle: clear_toggles turns every toggle action back into a plain hold") {
    InputSystem in;
    test::setup_default(in);
    in.set_toggle(MCE_SID("Jump"), true);
    in.set_toggle(MCE_SID("Fire"), true);
    CHECK(in.is_toggle(MCE_SID("Jump")));
    CHECK(in.is_toggle(MCE_SID("Fire")));
    in.clear_toggles();
    CHECK_FALSE(in.is_toggle(MCE_SID("Jump")));
    CHECK_FALSE(in.is_toggle(MCE_SID("Fire")));

    in.push_event(key_event(Key::Space, true, at_tick(1)));
    CHECK(in.sample_for_tick(1).jump_held);
    in.push_event(key_event(Key::Space, false, at_tick(2)));
    CHECK_FALSE(in.sample_for_tick(2).jump_held);  // let go: no longer held
}
