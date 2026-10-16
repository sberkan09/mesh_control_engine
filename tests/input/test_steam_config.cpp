// M-STEAM step 4: what Steam Input needs from the input layer. Action feeds add a platform's values
// to an action like one more device; a gamepad namer renames gamepad bindings; the action manifest
// and default controller configurations are written from the map itself.

#include <doctest/doctest.h>

#include <string>

#include "input_test_util.h"
#include "mce/input/binding_names.h"
#include "mce/input/steam_config.h"

using namespace mce;
using namespace mce::input;

namespace {

std::string text(const Array<char>& a) { return std::string(a.data(), a.size()); }

InputMapSet default_set() {
    Array<byte> chunk;
    default_map_chunk(chunk);
    InputMapSet set;
    REQUIRE(decode_input_map(chunk.span(), set).has_value());
    return set;
}

bool steam_names(void*, StringId action, BindingName& out) {
    if (action != MCE_SID("Jump")) return false;
    out.assign("A Button");
    return true;
}

}  // namespace

TEST_CASE("steam input: feeds hold a button and move a stick, until cleared") {
    InputSystem in;
    test::setup_default(in);
    const InputSystem::ActionFeed feeds[] = {{MCE_SID("Jump"), 1.f, 0.f}, {MCE_SID("Move"), 0.5f, 1.f}};
    in.set_action_feeds(Span<const InputSystem::ActionFeed>(feeds, 2));
    CHECK(in.action_feeds().size() == 2);
    const InputState& s1 = in.sample_for_tick(1);
    CHECK(s1.jump_pressed);
    CHECK(s1.move.x > 0.f);
    CHECK(s1.move.y > 0.f);  // y up, as W reads it
    const InputState& s2 = in.sample_for_tick(2);
    CHECK(s2.jump_held);
    CHECK_FALSE(s2.jump_pressed);
    in.clear_action_feeds();
    const InputState& s3 = in.sample_for_tick(3);
    CHECK(s3.jump_released);
    CHECK(s3.move.x == 0.f);
}

TEST_CASE("steam input: a namer renames gamepad bindings only, and only the actions it knows") {
    BindingDef pad;
    pad.device = Device::GamepadButton;
    pad.code = static_cast<u32>(platform::GamepadButton::South);
    BindingDef key;
    key.device = Device::Keyboard;
    key.code = static_cast<u32>(platform::Key::Space);
    BindingName before, n;
    binding_name(pad, before);
    set_gamepad_namer(&steam_names, nullptr);
    action_binding_name(MCE_SID("Jump"), pad, n);
    CHECK(n.view() == "A Button");
    action_binding_name(MCE_SID("Fire"), pad, n);
    CHECK(n.view() == before.view());
    BindingName space;
    binding_name(key, space);
    action_binding_name(MCE_SID("Jump"), key, n);
    CHECK(n.view() == space.view());
    set_gamepad_namer(nullptr, nullptr);
    action_binding_name(MCE_SID("Jump"), pad, n);
    CHECK(n.view() == before.view());
}

TEST_CASE("steam input: the action manifest and default configurations come from the map") {
    const InputMapSet set = default_set();
    Array<char> name;
    steam_manifest_file(480, name);
    CHECK(text(name) == "game_actions_480.vdf");
    CHECK(std::string(steam_controller_file(SteamController::Deck)) == "controller_neptune.vdf");
    CHECK(std::string(steam_controller_type(SteamController::PlayStation)) == "controller_ps4");

    Array<char> manifest;
    steam_action_manifest(set, "Demo", manifest);
    const std::string m = text(manifest);
    CHECK(m.find("\"In Game Actions\"") == 0);
    CHECK(m.find("\"Game\"") != std::string::npos);
    CHECK(m.find("\"Move\"") != std::string::npos);
    CHECK(m.find("\"joystick_move\"") != std::string::npos);
    CHECK(m.find("\"Jump\"\t\t\"#Action_Jump\"") != std::string::npos);
    CHECK(m.find("\"Title\"\t\t\"Demo\"") != std::string::npos);
    for (u32 c = 0; c < kSteamControllerCount; ++c)
        CHECK(m.find(steam_controller_file(static_cast<SteamController>(c))) != std::string::npos);
    // Braces balance: Steam refuses a file that does not parse.
    int depth = 0;
    for (const char ch : m) depth += ch == '{' ? 1 : ch == '}' ? -1 : 0;
    CHECK(depth == 0);

    Array<char> deck, xbox;
    steam_controller_config(set, SteamController::Deck, "Demo", deck);
    steam_controller_config(set, SteamController::Xbox, "Demo", xbox);
    const std::string d = text(deck), x = text(xbox);
    CHECK(d.find("\"controller_type\"\t\t\"controller_neptune\"") != std::string::npos);
    CHECK(d.find("game_action Game Jump") != std::string::npos);
    CHECK(d.find("right_trackpad") != std::string::npos);  // the Deck's trackpad joins the right stick
    CHECK(x.find("right_trackpad") == std::string::npos);
    CHECK(x.find("\"Game\"\t\t\"Move\"") != std::string::npos);  // a stick group names its action
}
