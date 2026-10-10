// FR-INP-020: a project's .mcinput action maps cook to CHNK_INPUT, the chunk InputSystem::load_map
// reads; typos and impossible bindings are cook errors naming file:line.

#include <doctest/doctest.h>

#include "cook_test_util.h"
#include "mce/asset/asset_system.h"
#include "mce/cook/cook.h"
#include "mce/input/input_map.h"
#include "mce/input/input_system.h"

using namespace mce;
using namespace cook_test;

namespace {

std::string diags_of(const cook::CookReport& r) {
    std::string s;
    for (const auto& d : r.diags.all()) {
        cook::String line;
        cook::format_diagnostic(d, line);
        s += line.c_str();
        s += "\n";
    }
    return s;
}

cook::CookReport cook_one(const std::string& name, const std::string& mcinput) {
    const std::string root = fresh_dir(name);
    write_file(root + "/assets/input/controls.mcinput", mcinput);
    cook::CookService svc;
    return svc.cook(StringView(root.c_str()), cook::CookOptions{});
}

constexpr const char* kGuid = "1a9c0000-0000-4000-8000-000000000001";

}  // namespace

TEST_CASE("input map: .mcinput cooks to CHNK_INPUT and loads into the input system") {
    const std::string root = fresh_dir("input_map_cook");
    write_file(root + "/assets/input/controls.mcinput", R"({ "format": "mce-input", "version": 1,
        "guid": "1a9c0000-0000-4000-8000-000000000001",
        "maps": [
          { "name": "Gameplay", "actions": [
            { "name": "Move", "type": "axis2d", "bindings": [
              { "key": "D", "scale": 1 }, { "key": "A", "scale": -1 },
              { "key": "Up", "axis": "y" }, { "key": "Num2", "axis": "y", "scale": -1 },
              { "gamepad_axis": "LeftY", "axis": "y", "scale": -1, "deadzone": [0.25, 0.9], "curve": "quadratic" } ] },
            { "name": "Dash", "bindings": [ { "key": "LShift" }, { "gamepad_button": "RightShoulder" }, { "key": "F5" } ] },
            { "name": "Zoom", "type": "axis1d", "bindings": [ { "mouse_wheel": "y", "axis": "x" } ] } ] },
          { "name": "Menu", "blocks_below": true, "actions": [
            { "name": "Confirm", "bindings": [ { "key": "Enter" }, { "mouse_button": "Left" } ] } ] } ] })");
    cook::CookService svc;
    const cook::CookReport r = svc.cook(StringView(root.c_str()), cook::CookOptions{});
    INFO(diags_of(r));
    REQUIRE(r.ok());

    asset::AssetSystem assets;
    const std::string cooked = root + "/cooked";
    const StringView roots[] = {StringView(cooked.c_str())};
    REQUIRE(assets.mount(Span<const StringView>(roots, 1)));
    const asset::AssetKey key = assets.find(asset::parse_guid(kGuid).value());
    REQUIRE(key != asset::kInvalidAssetKey);
    CHECK(assets.info(key).type == asset::ChunkType::InputMap);
    auto h = assets.load_sync<asset::InputMap>(key);
    REQUIRE(h);

    input::InputMapSet set;
    REQUIRE(input::decode_input_map(assets.resolve(h.value()), set));
    REQUIRE(set.maps.size() == 2);
    CHECK(set.maps[0].name == StringId("Gameplay"));
    CHECK(set.maps[0].action_count == 3);
    CHECK(set.maps[1].name == StringId("Menu"));
    CHECK((set.maps[1].flags & input::MapFlag::BlocksBelow) != 0);
    REQUIRE(set.actions.size() == 4);
    const input::ActionDef& move = set.actions[0];
    CHECK(move.name == StringId("Move"));
    CHECK(move.type == input::ActionType::Axis2D);
    REQUIRE(move.binding_count == 5);
    const input::BindingDef& a = set.bindings[move.first_binding + 1];
    CHECK(a.device == input::Device::Keyboard);
    CHECK(a.code == static_cast<u32>(platform::Key::A));
    CHECK(a.scale == doctest::Approx(-1.0f));
    CHECK(set.bindings[move.first_binding + 2].axis == 1);
    CHECK(set.bindings[move.first_binding + 3].code == static_cast<u32>(platform::Key::Num2));
    const input::BindingDef& stick = set.bindings[move.first_binding + 4];
    CHECK(stick.device == input::Device::GamepadAxis);
    CHECK(stick.code == static_cast<u32>(platform::GamepadAxis::LeftY));
    CHECK(stick.curve == input::Curve::Quadratic);
    CHECK(stick.dz.inner == doctest::Approx(0.25f));
    CHECK(stick.dz.outer == doctest::Approx(0.9f));
    const input::ActionDef& dash = set.actions[1];
    CHECK(dash.type == input::ActionType::Button);
    CHECK(set.bindings[dash.first_binding].code == static_cast<u32>(platform::Key::LShift));
    CHECK(set.bindings[dash.first_binding + 1].device == input::Device::GamepadButton);
    CHECK(set.bindings[dash.first_binding + 2].code == static_cast<u32>(platform::Key::F5));
    const input::BindingDef& wheel = set.bindings[set.actions[2].first_binding];
    CHECK(wheel.device == input::Device::MouseWheel);
    CHECK(wheel.code == 1);
    CHECK(wheel.axis == 0);
    CHECK(set.bindings[set.actions[3].first_binding].code == static_cast<u32>(platform::Key::Return));

    // The chunk is what the runtime loads: the stack starts on the first map, and the second
    // can be pushed by name.
    input::InputSystem sys;
    REQUIRE(sys.load_map(assets.resolve(h.value())));
    CHECK(sys.push_map(StringId("Menu")));
    assets.release(asset::AnyHandle(h.value()));
}

TEST_CASE("input map: authoring mistakes are cook errors") {
    const char* const bad[] = {
        // unknown key name
        R"({ "format": "mce-input", "version": 1, "guid": "1a9c0000-0000-4000-8000-000000000001",
            "maps": [ { "name": "G", "actions": [ { "name": "Jump", "bindings": [ { "key": "Spacebar" } ] } ] } ] })",
        // two devices in one binding
        R"({ "format": "mce-input", "version": 1, "guid": "1a9c0000-0000-4000-8000-000000000001",
            "maps": [ { "name": "G", "actions": [ { "name": "Jump", "bindings": [ { "key": "Space", "gamepad_button": "South" } ] } ] } ] })",
        // a y-axis binding on a button
        R"({ "format": "mce-input", "version": 1, "guid": "1a9c0000-0000-4000-8000-000000000001",
            "maps": [ { "name": "G", "actions": [ { "name": "Jump", "bindings": [ { "key": "Space", "axis": "y" } ] } ] } ] })",
        // typo'd key
        R"({ "format": "mce-input", "version": 1, "guid": "1a9c0000-0000-4000-8000-000000000001",
            "maps": [ { "name": "G", "actions": [ { "name": "Jump", "bindinds": [] } ] } ] })",
        // two maps with one name (decode_input_map rejects it)
        R"({ "format": "mce-input", "version": 1, "guid": "1a9c0000-0000-4000-8000-000000000001",
            "maps": [ { "name": "G", "actions": [] }, { "name": "G", "actions": [] } ] })",
        // no maps
        R"({ "format": "mce-input", "version": 1, "guid": "1a9c0000-0000-4000-8000-000000000001", "maps": [] })",
    };
    for (u32 i = 0; i < sizeof(bad) / sizeof(bad[0]); ++i) {
        CAPTURE(i);
        const cook::CookReport r = cook_one("input_map_bad", bad[i]);
        CHECK_FALSE(r.ok());
        const std::string text = diags_of(r);
        CHECK(text.find("controls.mcinput") != std::string::npos);
    }
}
