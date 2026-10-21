#pragma once
// mce_platform (L1): haptic feedback, the phone's taps and the gamepad's rumble (M-MOBILE step 11d,
// #150).
//
//   platform::haptics::play(platform::haptics::Preset::Light);   // a button press
//   platform::haptics::play(platform::haptics::Preset::Error);   // the player died
//
// One call drives every haptic device the player has:
// - iOS: UIKit's feedback generators (haptics_uikit.swift), built when MCE_UIKIT_HAPTICS is ON.
// - Android: the vibrator's predefined effects (VibrationEffect) through
//   org.meshcontrol.haptics.MceHaptics, stamped into every game's Gradle project
//   (cmake/mobile/android_app.py); the call crosses JNI.
// - Every connected gamepad (SDL rumble), on every platform.
//
// A global setting turns all of it off (set_enabled; the player keeps it in the saved convar
// `haptics`). Haptics are output only: nothing here reads or writes World state, so they never
// enter the determinism hash, and a headless run plays none. Main thread.

#include "mce/types.h"

namespace mce::platform::haptics {

enum class Preset : u8 {
    Selection,  // a value ticked over (a slider step, a list moving)
    Light,      // a light impact (a button)
    Medium,     // a medium impact (a landing)
    Heavy,      // a heavy impact (a hit)
    Success,    // a task finished well
    Warning,    // something needs attention
    Error,      // a failure (death, a refused action)
};
inline constexpr u32 kPresetCount = 7;

enum class Backend : u8 { None, UIKit, AndroidVibrator };

Backend backend();                    // the device's own haptics; gamepads rumble regardless
const char* backend_name(Backend b);  // "none", "uikit", "android"
const char* preset_name(Preset p);    // "selection", "light", ...

// The gamepad rumble a preset plays: motor intensities 0..1 and a duration.
struct Rumble {
    f32 low = 0.f;
    f32 high = 0.f;
    u32 ms = 0;
};
Rumble gamepad_rumble_of(Preset p);

// The global setting (on by default). Off, play() does nothing and counts the call as suppressed.
void set_enabled(bool on);
bool enabled();

// Plays the preset on the device and on every connected gamepad. False when haptics are off or
// nothing could play it (no device haptics, no gamepad).
bool play(Preset p);

// What play() did in this process: what tests and the desktop's fake check.
struct Stats {
    u32 played = 0;      // calls while enabled
    u32 suppressed = 0;  // calls while disabled
    u32 device = 0;      // calls handed to the device's haptics
    u32 gamepads = 0;    // gamepad rumbles started
    Preset last = Preset::Selection;
};
Stats stats();

}  // namespace mce::platform::haptics
