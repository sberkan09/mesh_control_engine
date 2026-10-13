#pragma once
// mce_platform (L1): the phone's ad network, Google Mobile Ads with Google's consent form (UMP) and,
// on iOS, App Tracking Transparency (M-MONETIZE step 4).
//
// The store wall (check_walls.py) covers this too: the Google Mobile Ads and UMP SDKs, ATT and JNI
// live only in mce_platform and the stamped Android project. This header speaks ad unit ids as
// strings; mce_monetize's PlatformAds provider maps them to its catalog.
//
// - Android: org.meshcontrol.ads.MceAds, stamped into the Gradle project of a game whose store
//   catalog has ad placements (cmake/mobile/android_app.py); the calls cross JNI.
// - iOS: ads_admob.swift, built when MCE_ADMOB is ON with the SDKs in MCE_ADMOB_SDK_DIR.
// - Everywhere else: backend() is None and every load is answered NoFill, every show Failed.
//
// start() gathers consent first: the UMP form where the law needs one (EEA, UK, some US states),
// then on iOS the ATT prompt. Ads are requested only once that is done; while it runs, requests
// wait. Without consent to personalized ads (declined, or ATT denied) requests are non-personalized.
//
// Calls are made on the main thread. Answers arrive through the handler on any thread.

#include "mce/types.h"

namespace mce::platform::ads {

enum class Backend : u8 { None, GoogleMobileAds };

enum class Format : u8 { Rewarded, Interstitial, Banner };  // monetize::AdFormat's values

enum class AnswerKind : u8 {
    Loaded,    // the unit has an ad ready to show (fullscreen) or on screen (banner)
    NoFill,    // the network had no ad
    Shown,     // a fullscreen ad covers the game
    Rewarded,  // a rewarded ad was watched to the end: amount
    Closed,    // the fullscreen ad is gone
    Failed,    // the request or the show failed
};

struct Answer {
    AnswerKind kind = AnswerKind::Failed;
    u32 request = 0;
    StringView unit;  // valid during the call only
    u32 amount = 0;   // Rewarded
};

using AnswerFn = void (*)(void* user, const Answer& a);

// What start() learned about consent; Unknown until it finishes.
enum class Consent : u8 { Unknown, Personalized, NonPersonalized };

struct Options {
    bool test_device = false;  // UMP debug geography (EEA) and GMA test-device mode: the form always shows
};

Backend backend();
const char* backend_name(Backend b);  // "none", "admob"

// Google's own test units, which always fill and never pay (developers.google.com/admob): what a
// placement without a unit, and every placement in a debug build, uses.
StringView test_unit(Format f);

// Initializes the SDK and gathers consent. False when backend() is None (the handler is kept, so
// calls still get their answers).
bool start(AnswerFn fn, void* user, const Options& opt = {});
void stop();
Consent consent();
// UMP wants a way back to the form ("Privacy options" in the game's settings) where it applies.
bool privacy_options_required();
void show_privacy_options();

void load(Format f, StringView unit, u32 request);
void show(Format f, StringView unit, u32 request);
// A banner along the bottom (or top) edge, kept inside the safe-area insets (device pixels).
void show_banner(StringView unit, u32 request, bool top, u32 inset_left, u32 inset_top, u32 inset_right,
                 u32 inset_bottom);
void hide_banner();

}  // namespace mce::platform::ads
