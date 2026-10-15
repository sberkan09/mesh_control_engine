#pragma once
// mce_platform (L1): Firebase, the live services vendor (M-LIVEOPS step 1): Crashlytics for native
// crashes, Firebase Analytics for events and Firebase Remote Config as remote config's server
// fetcher (M-LIVEOPS step 3).
//
// The store wall (check_walls.py) covers this too: the Firebase SDKs and JNI live only in
// mce_platform and the stamped Android project. mce_liveops never sees them; the player hands
// analytics events here (an analytics::IBackend) and the consent state.
//
// - Android: org.meshcontrol.firebase.MceFirebase, stamped into the Gradle project of a game shipped
//   with MCE_FIREBASE_ANDROID_CONFIG (the project's google-services.json, cmake/mobile/android_app.py);
//   Crashlytics' NDK handler catches native crashes and its Gradle plugin uploads the symbols.
// - iOS: firebase_ios.swift, built when MCE_FIREBASE is ON with the SDKs in MCE_FIREBASE_SDK_DIR and
//   shipped with MCE_FIREBASE_IOS_CONFIG (GoogleService-Info.plist); ship.py uploads the dSYM.
// - Everywhere else: backend() is None and every call is a no-op.
//
// Nothing leaves the device until set_collection(Granted): both SDKs start with collection off (the
// stamped manifest and Info.plist say so), keep crash reports on the device while Undecided, and
// delete them on Denied.
//
// Calls are made on the main thread; remote config answers arrive on the SDK's thread.

#include "mce/types.h"

namespace mce::platform::firebase {

enum class Backend : u8 { None, Firebase };

enum class Collection : u8 {
    Undecided,  // nothing is sent; crash reports wait on the device
    Granted,    // analytics and crash reports are sent (the waiting reports too)
    Denied,     // nothing is sent; waiting crash reports are deleted
};

struct Param {
    enum class Kind : u8 { Int, Float, String };
    StringView key;
    Kind kind = Kind::Int;
    i64 i = 0;
    f64 f = 0.0;
    StringView s;
};

Backend backend();
const char* backend_name(Backend b);  // "none", "firebase"

// Starts the SDKs with collection off. False when backend() is None.
bool start();
void set_collection(Collection c);
// One Analytics event (names and params already checked by liveops::analytics).
void log_event(StringView name, Span<const Param> params);

// Remote Config: fetches and activates in the background, then answers through `fn` on the SDK's
// thread: once per value the service sent (key and value as text, `done` false), then once with
// `done` true and `ok` false when the fetch failed. Values are text because the console stores them
// so; liveops::parse_config_value types them by the cooked defaults. One fetch at a time: false
// (and no answer) while one is running, after start() failed, or with no vendor.
using ConfigFn = void (*)(void* user, StringView key, StringView value, bool done, bool ok);
bool fetch_remote_config(ConfigFn fn, void* user);

}  // namespace mce::platform::firebase
