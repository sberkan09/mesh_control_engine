#pragma once
// The player's consent to data leaving the device (M-LIVEOPS), shared by every live service.
// Thread-safe. Unknown until someone sets it: the game's privacy screen, the ad network's consent
// form (the player maps M-MONETIZE's UMP / ATT outcome) or -liveops.consent.

#include "mce/types.h"

namespace mce::liveops {

enum class Consent : u8 {
    Unknown,  // not asked yet: hold everything locally
    Granted,  // send
    Denied,   // never send; analytics events are dropped, crash reports stay on this device
};

void set_consent(Consent c);
Consent consent();
const char* consent_name(Consent c);  // "unknown", "granted", "denied"

}  // namespace mce::liveops
