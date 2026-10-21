// M-MOBILE step 11d: haptics. Off a phone there are no device haptics, so play() reaches the gamepads
// only: a virtual pad (SDL's virtual joystick with a rumble callback) is the fake that shows a preset
// arrives as rumble, and that the global setting stops it. The phone taps themselves are checked in the
// iOS Simulator (Xcode's haptics log) and on an Android emulator, on the Mac.

#include <doctest/doctest.h>

#include "mce/platform/haptics.h"
#include "mce/platform/platform.h"

using namespace mce;
namespace haptics = mce::platform::haptics;

TEST_CASE("haptics: names, and every preset has a rumble that stays in range") {
    CHECK(haptics::backend() == haptics::Backend::None);
    CHECK(StringView(haptics::backend_name(haptics::Backend::None)) == "none");
    CHECK(StringView(haptics::backend_name(haptics::Backend::UIKit)) == "uikit");
    CHECK(StringView(haptics::backend_name(haptics::Backend::AndroidVibrator)) == "android");
    u32 last_ms = 0;
    for (u32 i = 0; i < haptics::kPresetCount; ++i) {
        const auto p = static_cast<haptics::Preset>(i);
        CHECK(StringView(haptics::preset_name(p)) != "?");
        const haptics::Rumble r = haptics::gamepad_rumble_of(p);
        CHECK(r.low >= 0.f);
        CHECK(r.low <= 1.f);
        CHECK(r.high > 0.f);
        CHECK(r.high <= 1.f);
        CHECK(r.ms > 0);
        CHECK(r.ms <= 250);
        if (i <= static_cast<u32>(haptics::Preset::Heavy)) {  // impacts grow with their weight
            CHECK(r.ms >= last_ms);
            last_ms = r.ms;
        }
    }
}

TEST_CASE("haptics: without a pad or device haptics, play() counts the call and plays nothing") {
    haptics::set_enabled(true);
    const haptics::Stats before = haptics::stats();
    CHECK_FALSE(haptics::play(haptics::Preset::Medium));
    const haptics::Stats after = haptics::stats();
    CHECK(after.played == before.played + 1);
    CHECK(after.device == before.device);
    CHECK(after.gamepads == before.gamepads);
    CHECK(after.last == haptics::Preset::Medium);
}

TEST_CASE("haptics: a preset rumbles the connected pad; the setting turns it off") {
    platform::PlatformDesc d;
    d.video_driver = "dummy";
    const bool ok = platform::init(d).has_value();
    REQUIRE(ok);
    if (!ok) return;  // no-exception doctest: REQUIRE does not leave the case
    const u32 pad = platform::attach_virtual_gamepad("haptics pad");
    REQUIRE(pad != 0);
    platform::pump_events();  // the hot-plug opens it into slot 0
    REQUIRE(platform::gamepad_connected(0));
    if (pad == 0 || !platform::gamepad_connected(0)) {
        platform::shutdown();
        return;
    }

    haptics::set_enabled(true);
    const haptics::Stats before = haptics::stats();
    CHECK(haptics::play(haptics::Preset::Heavy));
    platform::VirtualRumble r = platform::virtual_gamepad_rumble(pad);
    const haptics::Rumble heavy = haptics::gamepad_rumble_of(haptics::Preset::Heavy);
    CHECK(r.count == 1);
    CHECK(r.low == doctest::Approx(heavy.low).epsilon(1e-3));
    CHECK(r.high == doctest::Approx(heavy.high).epsilon(1e-3));
    CHECK(haptics::stats().gamepads == before.gamepads + 1);

    CHECK(haptics::play(haptics::Preset::Selection));
    r = platform::virtual_gamepad_rumble(pad);
    CHECK(r.count == 2);
    CHECK(r.low == doctest::Approx(0.f));
    CHECK(r.high == doctest::Approx(haptics::gamepad_rumble_of(haptics::Preset::Selection).high).epsilon(1e-3));

    haptics::set_enabled(false);
    CHECK_FALSE(haptics::enabled());
    CHECK_FALSE(haptics::play(haptics::Preset::Error));
    CHECK(platform::virtual_gamepad_rumble(pad).count == 2);
    CHECK(haptics::stats().suppressed == before.suppressed + 1);
    haptics::set_enabled(true);

    platform::detach_virtual_gamepad(pad);
    platform::pump_events();
    platform::shutdown();
}
