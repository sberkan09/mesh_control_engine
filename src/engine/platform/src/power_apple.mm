// mce_platform: the thermal state for platform/power.h on iOS and macOS (NSProcessInfo.thermalState).
// Objective-C++ without ARC, like mce_rhi_metal: the call returns a plain enum and keeps nothing.

#import <Foundation/Foundation.h>

#include "mce/platform/power.h"

namespace mce::platform {

ThermalState apple_thermal_state() {
    switch ([[NSProcessInfo processInfo] thermalState]) {
        case NSProcessInfoThermalStateNominal: return ThermalState::Nominal;
        case NSProcessInfoThermalStateFair: return ThermalState::Fair;
        case NSProcessInfoThermalStateSerious: return ThermalState::Serious;
        case NSProcessInfoThermalStateCritical: return ThermalState::Critical;
    }
    return ThermalState::Unknown;
}

}  // namespace mce::platform
