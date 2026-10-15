// M-ACCESS step 3: the stock options screen's controls page (mce/ui/options_panel.h): the remap
// list is built from the input map, a binding button captures the next press, the Toggle switch
// sets hold-to-toggle through input.toggle_actions, and the preset buttons swap keyboard layouts.

#include <doctest/doctest.h>

#include "../input/input_test_util.h"
#include "mce/convar.h"
#include "mce/ui/options_panel.h"
#include "mce/ui/ui.h"

using namespace mce;
using namespace mce::ui;
using platform::Key;

namespace {

u64 at_tick(u64 n) { return ((n - 1) * 1'000'000ull) / 60 + 1000; }

UpdateContext frame(const input::InputState* actions = nullptr) {
    UpdateContext c;
    c.viewport = rhi::Extent2D{1280, 720};
    c.dt = 1.0 / 60.0;
    c.actions = actions;
    return c;
}

// Focus `w` and confirm it, as a player with a gamepad would.
void confirm(Document& d, OptionsPanel& panel, WidgetId w) {
    d.focus(w);
    input::InputState in;
    in.confirm_pressed = true;
    d.update(frame(&in));
    panel.update();
    d.update(frame());
    panel.update();
}

}  // namespace

TEST_CASE("M-ACCESS: the options panel lists the remappable actions and rebinds by capture") {
    input::InputSystem in;
    test::setup_default(in);
    Document d;
    Builder root(d, d.root());
    root.stack("presets").button("preset_right", "RIGHT HAND");
    root.button("reset_bindings", "RESET");
    root.scroll("list").stack("remap_list");
    OptionsPanel panel;
    REQUIRE(panel.attach(d, in));
    d.update(frame());

    // Gameplay rows only (the UI map is menu navigation): Move per direction, then the buttons.
    // Look has only mouse and stick axes it reads as a whole and touch: its stick halves list.
    CHECK(panel.row_count() >= 4 + 3);
    const StringId gp = MCE_SID("Gameplay");
    const WidgetId jump = panel.binding_button(gp, MCE_SID("Jump"), 0);
    REQUIRE(d.valid(jump));
    CHECK(d.text(jump) == StringView("Space"));
    CHECK(d.text(panel.binding_button(gp, MCE_SID("Jump"), 1)) == StringView("Pad South"));
    CHECK_FALSE(d.valid(panel.binding_button(gp, MCE_SID("Jump"), 2)));  // the touch tap is not listed
    CHECK(d.text(panel.binding_button(gp, MCE_SID("Move"), 0)) == StringView("D"));
    CHECK_FALSE(d.valid(panel.binding_button(MCE_SID("UI"), MCE_SID("Confirm"), 0)));

    // Confirming Jump's key captures the next press.
    in.sample_for_tick(1);
    confirm(d, panel, jump);
    CHECK(panel.capturing());
    CHECK(d.text(jump) == StringView("Press a key"));
    in.push_event(test::key_event(Key::K, true, at_tick(2)));
    in.sample_for_tick(2);
    d.update(frame());
    panel.update();
    CHECK_FALSE(panel.capturing());
    CHECK(d.text(jump) == StringView("K"));

    // Presets and reset relabel every button.
    confirm(d, panel, d.find("presets/preset_right"));
    CHECK(d.text(jump) == StringView("Enter"));
    CHECK(d.text(panel.binding_button(gp, MCE_SID("Move"), 0)) == StringView("Right"));
    confirm(d, panel, d.find("reset_bindings"));
    CHECK(d.text(jump) == StringView("Space"));
    CHECK(in.binding_overrides().empty());
}

TEST_CASE("M-ACCESS: the Toggle switch sets hold-to-toggle and the convars drive the input system") {
    ConVar* toggles = ConVar::find(StringId("input.toggle_actions"));
    ConVar* sens = ConVar::find(StringId("input.look_sensitivity"));
    REQUIRE(toggles);
    REQUIRE(sens);
    input::InputSystem in;
    test::setup_default(in);
    Document d;
    Builder(d, d.root()).stack("remap_list");
    OptionsPanel panel;
    REQUIRE(panel.attach(d, in));
    d.update(frame());
    const WidgetId fire_toggle = panel.toggle_switch(MCE_SID("Fire"));
    REQUIRE(d.valid(fire_toggle));
    CHECK(d.value(fire_toggle) == 0.0f);
    confirm(d, panel, fire_toggle);
    CHECK(toggles->get_string() == StringView("Fire"));
    CHECK(in.is_toggle(MCE_SID("Fire")));
    set_toggle_option("Jump", true);
    set_toggle_option("Fire", false);
    CHECK(toggles->get_string() == StringView("Jump"));
    sens->set_float(1.5f);
    apply_input_options(in);
    CHECK(in.is_toggle(MCE_SID("Jump")));
    CHECK_FALSE(in.is_toggle(MCE_SID("Fire")));
    CHECK(in.look_sensitivity() == 1.5f);
    toggles->reset();
    sens->reset();
    apply_input_options(in);
    CHECK_FALSE(in.is_toggle(MCE_SID("Jump")));

    // A document without a remap list leaves the panel off.
    Document plain;
    OptionsPanel off;
    CHECK_FALSE(off.attach(plain, in));
    off.update();
}
