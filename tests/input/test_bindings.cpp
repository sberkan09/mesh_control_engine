// Persisted bindings (mce/input/bindings.h): the text form round-trips every override field, tolerates
// unknown fields, and refuses a file that is not a bindings file.

#include <doctest/doctest.h>

#include <string>

#include "mce/input/binding_names.h"
#include "mce/input/bindings.h"

using namespace mce;
using namespace mce::input;

namespace {

std::string text_of(Span<const BindingOverride> overrides) {
    Array<char> out;
    write_bindings(overrides, out);
    return std::string(out.data(), out.size());
}

}  // namespace

TEST_CASE("bindings: write_bindings and read_bindings round-trip every field of every override") {
    BindingDef a;
    a.device = Device::GamepadAxis;
    a.axis = 1;
    a.curve = Curve::Cubic;
    a.flags = 3;
    a.code = 42;
    a.scale = -1.f;
    a.dz = {0.1f, 0.9f};
    BindingDef b;  // all defaults
    const BindingOverride in[2] = {BindingOverride::make(StringId("gameplay"), StringId("Move"), 2, a),
                                   BindingOverride::make(StringId("ui"), StringId("Accept"), 0, b)};

    const std::string text = text_of(Span<const BindingOverride>(in, 2));
    Array<BindingOverride> out;
    REQUIRE(read_bindings(text, out).has_value());
    REQUIRE(out.size() == 2);
    for (usize i = 0; i < 2; ++i) {
        CHECK(out[i].map == in[i].map);
        CHECK(out[i].action == in[i].action);
        CHECK(out[i].index == in[i].index);
        CHECK(out[i].device == in[i].device);
        CHECK(out[i].axis == in[i].axis);
        CHECK(out[i].curve == in[i].curve);
        CHECK(out[i].flags == in[i].flags);
        CHECK(out[i].code == in[i].code);
        CHECK(out[i].scale == in[i].scale);
        CHECK(out[i].dz_inner == in[i].dz_inner);
        CHECK(out[i].dz_outer == in[i].dz_outer);
    }
    CHECK(out[0].binding().device == Device::GamepadAxis);
    CHECK(out[0].binding().dz.outer == 0.9f);
}

TEST_CASE("bindings: an empty list writes only the header, which reads back as no overrides") {
    const std::string text = text_of(Span<const BindingOverride>());
    CHECK_FALSE(text.empty());
    Array<BindingOverride> out;
    out.push_back(BindingOverride{});  // read_bindings clears what was there
    REQUIRE(read_bindings(text, out).has_value());
    CHECK(out.empty());
}

TEST_CASE("bindings: read_bindings ignores unknown fields and keeps defaults for missing ones") {
    BindingDef b;
    const BindingOverride one = BindingOverride::make(StringId("m"), StringId("a"), 1, b);
    std::string text = text_of(Span<const BindingOverride>(&one, 1));
    text += "future_field = 7\n";
    Array<BindingOverride> out;
    REQUIRE(read_bindings(text, out).has_value());
    REQUIRE(out.size() == 1);
    CHECK(out[0].index == 1);

    // A record that names only one field leaves the rest at their defaults.
    std::string header = text.substr(0, text.find('\n') + 1);
    REQUIRE(read_bindings(header + "[BindingOverride]\ncode = 9\n", out).has_value());
    REQUIRE(out.size() == 1);
    CHECK(out[0].code == 9);
    CHECK(out[0].scale == 1.f);
    CHECK(out[0].dz_outer == 1.f);
}

TEST_CASE("bindings: read_bindings refuses a file without the header, an unknown record, and a bad value") {
    Array<BindingOverride> out;
    CHECK_FALSE(read_bindings("", out).has_value());
    CHECK_FALSE(read_bindings("not a bindings file\n", out).has_value());

    BindingDef b;
    const BindingOverride one = BindingOverride::make(StringId("m"), StringId("a"), 0, b);
    const std::string good = text_of(Span<const BindingOverride>(&one, 1));
    const std::string header = good.substr(0, good.find('\n') + 1);
    CHECK_FALSE(read_bindings(header + "[Other]\n", out).has_value());
    CHECK_FALSE(read_bindings(header + "code = 1\n", out).has_value());  // a field outside a record
    CHECK_FALSE(read_bindings(header + "[BindingOverride]\ncode = x\n", out).has_value());
    CHECK_FALSE(read_bindings(header + "[BindingOverride]\ncode\n", out).has_value());
}

TEST_CASE("binding names: mouse buttons, gamepad buttons and gamepad axes have the names an options screen shows") {
    using platform::GamepadAxis;
    using platform::GamepadButton;
    using platform::MouseButton;
    CHECK(StringView(mouse_button_name(MouseButton::Left)) == StringView("Mouse Left"));
    CHECK(StringView(mouse_button_name(MouseButton::Right)) == StringView("Mouse Right"));
    CHECK(StringView(mouse_button_name(MouseButton::X1)) == StringView("Mouse 4"));
    CHECK(StringView(mouse_button_name(MouseButton::X2)) == StringView("Mouse 5"));
    CHECK(StringView(mouse_button_name(static_cast<MouseButton>(99))) == StringView("Mouse"));

    CHECK(StringView(gamepad_button_name(GamepadButton::South)) == StringView("Pad South"));
    CHECK(StringView(gamepad_button_name(GamepadButton::LeftShoulder)) == StringView("Left Shoulder"));
    CHECK(StringView(gamepad_button_name(GamepadButton::DpadUp)) == StringView("D-Pad Up"));
    CHECK(StringView(gamepad_button_name(GamepadButton::Touchpad)) == StringView("Touchpad"));
    CHECK(StringView(gamepad_button_name(GamepadButton::Misc1)) == StringView("Pad Button"));  // an unnamed one
    // Every real button has a non-empty, ASCII-only name (the built-in UI font draws every one).
    for (u32 i = 0; i < static_cast<u32>(GamepadButton::Count); ++i) {
        const StringView n(gamepad_button_name(static_cast<GamepadButton>(i)));
        CHECK_FALSE(n.empty());
        for (const char c : n) CHECK((static_cast<unsigned char>(c) >= 0x20 && static_cast<unsigned char>(c) < 0x7F));
    }

    CHECK(StringView(gamepad_axis_name(GamepadAxis::LeftX)) == StringView("Left Stick X"));
    CHECK(StringView(gamepad_axis_name(GamepadAxis::RightY)) == StringView("Right Stick Y"));
    CHECK(StringView(gamepad_axis_name(GamepadAxis::LeftTrigger)) == StringView("Left Trigger"));
    CHECK(StringView(gamepad_axis_name(GamepadAxis::RightTrigger)) == StringView("Right Trigger"));
    CHECK(StringView(gamepad_axis_name(static_cast<GamepadAxis>(99))) == StringView("Pad Axis"));
}
