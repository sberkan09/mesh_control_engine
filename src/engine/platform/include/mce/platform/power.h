#pragma once
// mce_platform (L1): the battery and the device's thermal state (M-MOBILE step 8: per-tier battery and
// thermal numbers on phones).
//
// - Battery: SDL_GetPowerInfo on every platform (phones, laptops; a desktop without one says NoBattery).
// - Thermal: NSProcessInfo.thermalState on iOS and macOS (power_apple.mm); Unknown elsewhere for now
//   (Android's PowerManager.getCurrentThermalStatus is not bridged yet).
//
// `-platform.power_log <seconds>` logs one `power:` line at that interval from pump_events (0 = off,
// the default), which is how a soak run on a device records the drain and the heat.

#include "mce/types.h"

namespace mce::platform {

enum class PowerState : u8 { Unknown, OnBattery, NoBattery, Charging, Charged };
enum class ThermalState : u8 { Unknown, Nominal, Fair, Serious, Critical };

struct PowerInfo {
    PowerState state = PowerState::Unknown;
    i32 battery_percent = -1;  // -1: unknown or no battery
    ThermalState thermal = ThermalState::Unknown;
};

// Main thread, after platform::init.
PowerInfo power_info();
const char* power_state_name(PowerState s);      // "unknown", "on battery", "no battery", "charging", "charged"
const char* thermal_state_name(ThermalState t);  // "unknown", "nominal", "fair", "serious", "critical"

}  // namespace mce::platform
