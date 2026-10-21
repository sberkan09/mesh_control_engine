// M-MOBILE step 8: the battery and thermal probe behind -platform.power_log. The numbers themselves come
// from devices; here the names and the shape of what a host reports.

#include <doctest/doctest.h>

#include "mce/fixed_string.h"
#include "mce/platform/platform.h"
#include "mce/platform/power.h"

using namespace mce;

TEST_CASE("power: names") {
    CHECK(StringView(platform::power_state_name(platform::PowerState::OnBattery)) == "on battery");
    CHECK(StringView(platform::power_state_name(platform::PowerState::Charged)) == "charged");
    CHECK(StringView(platform::power_state_name(platform::PowerState::Unknown)) == "unknown");
    CHECK(StringView(platform::thermal_state_name(platform::ThermalState::Nominal)) == "nominal");
    CHECK(StringView(platform::thermal_state_name(platform::ThermalState::Critical)) == "critical");
    CHECK(StringView(platform::thermal_state_name(platform::ThermalState::Unknown)) == "unknown");
}

TEST_CASE("power: a host reports a consistent battery and, on Apple, a thermal state") {
    const platform::PowerInfo p = platform::power_info();
    CHECK(p.battery_percent >= -1);
    CHECK(p.battery_percent <= 100);
    if (p.state == platform::PowerState::NoBattery) CHECK(p.battery_percent == -1);
#if defined(__APPLE__)
    CHECK(p.thermal != platform::ThermalState::Unknown);
#else
    CHECK(p.thermal == platform::ThermalState::Unknown);
#endif
}
