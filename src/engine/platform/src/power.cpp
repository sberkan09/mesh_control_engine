#include "mce/platform/power.h"

#include <SDL3/SDL.h>

namespace mce::platform {

#if defined(__APPLE__)
ThermalState apple_thermal_state();  // power_apple.mm
#endif

PowerInfo power_info() {
    PowerInfo p;
    int percent = -1;
    switch (SDL_GetPowerInfo(nullptr, &percent)) {
        case SDL_POWERSTATE_ON_BATTERY: p.state = PowerState::OnBattery; break;
        case SDL_POWERSTATE_NO_BATTERY: p.state = PowerState::NoBattery; break;
        case SDL_POWERSTATE_CHARGING: p.state = PowerState::Charging; break;
        case SDL_POWERSTATE_CHARGED: p.state = PowerState::Charged; break;
        default: p.state = PowerState::Unknown; break;
    }
    p.battery_percent = percent;
#if defined(__APPLE__)
    p.thermal = apple_thermal_state();
#endif
    return p;
}

const char* power_state_name(PowerState s) {
    switch (s) {
        case PowerState::OnBattery: return "on battery";
        case PowerState::NoBattery: return "no battery";
        case PowerState::Charging: return "charging";
        case PowerState::Charged: return "charged";
        case PowerState::Unknown: break;
    }
    return "unknown";
}

const char* thermal_state_name(ThermalState t) {
    switch (t) {
        case ThermalState::Nominal: return "nominal";
        case ThermalState::Fair: return "fair";
        case ThermalState::Serious: return "serious";
        case ThermalState::Critical: return "critical";
        case ThermalState::Unknown: break;
    }
    return "unknown";
}

}  // namespace mce::platform
