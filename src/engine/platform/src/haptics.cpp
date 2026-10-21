#include "mce/platform/haptics.h"

#include <SDL3/SDL.h>

#include <atomic>

#include "mce/log.h"
#include "mce/platform/platform.h"

#if defined(SDL_PLATFORM_ANDROID)
#include "android_jni.h"
#endif

namespace mce::platform::haptics {
namespace {

std::atomic<bool> g_enabled{true};
Stats g_stats;

bool device_play(Preset p);

}  // namespace

const char* backend_name(Backend b) {
    switch (b) {
        case Backend::UIKit: return "uikit";
        case Backend::AndroidVibrator: return "android";
        case Backend::None: break;
    }
    return "none";
}

const char* preset_name(Preset p) {
    switch (p) {
        case Preset::Selection: return "selection";
        case Preset::Light: return "light";
        case Preset::Medium: return "medium";
        case Preset::Heavy: return "heavy";
        case Preset::Success: return "success";
        case Preset::Warning: return "warning";
        case Preset::Error: return "error";
    }
    return "?";
}

Rumble gamepad_rumble_of(Preset p) {
    switch (p) {
        case Preset::Selection: return {0.0f, 0.25f, 25};
        case Preset::Light: return {0.2f, 0.4f, 40};
        case Preset::Medium: return {0.45f, 0.6f, 60};
        case Preset::Heavy: return {0.8f, 0.9f, 90};
        case Preset::Success: return {0.3f, 0.6f, 120};
        case Preset::Warning: return {0.6f, 0.5f, 150};
        case Preset::Error: return {0.9f, 0.7f, 220};
    }
    return {};
}

void set_enabled(bool on) {
    if (g_enabled.exchange(on, std::memory_order_relaxed) != on) MCE_LOG_INFO("haptics: {}", on ? "on" : "off");
}
bool enabled() { return g_enabled.load(std::memory_order_relaxed); }

Stats stats() { return g_stats; }

bool play(Preset p) {
    if (!enabled()) {
        ++g_stats.suppressed;
        return false;
    }
    ++g_stats.played;
    g_stats.last = p;
    bool any = false;
    if (device_play(p)) {
        ++g_stats.device;
        any = true;
    }
    const Rumble r = gamepad_rumble_of(p);
    for (u32 slot = 0; slot < kMaxGamepads; ++slot) {
        if (gamepad_connected(slot) && gamepad_rumble(slot, r.low, r.high, r.ms)) {
            ++g_stats.gamepads;
            any = true;
        }
    }
    return any;
}

// ---- Android: the vibrator through the stamped MceActivity ---------------------------------------
#if defined(SDL_PLATFORM_ANDROID)

Backend backend() {
    static const bool has = [] {
        detail::Jni j;
        return j.method("mceHaptic", "(I)V") != nullptr;
    }();
    return has ? Backend::AndroidVibrator : Backend::None;
}

namespace {
bool device_play(Preset p) {
    if (backend() == Backend::None) return false;
    detail::Jni j;
    j.env->CallVoidMethod(j.activity, j.method("mceHaptic", "(I)V"), static_cast<jint>(p));
    return j.ok("mceHaptic");
}
}  // namespace

// ---- iOS: UIKit feedback generators (haptics_uikit.swift) ----------------------------------------
#elif defined(SDL_PLATFORM_IOS) && defined(MCE_UIKIT_HAPTICS)

extern "C" void mce_uikit_haptic(int preset);  // @_cdecl in haptics_uikit.swift

Backend backend() { return Backend::UIKit; }

namespace {
bool device_play(Preset p) {
    mce_uikit_haptic(static_cast<int>(p));
    return true;
}
}  // namespace

// ---- desktop, and phones built without the haptics side: gamepads only ---------------------------
#else

Backend backend() { return Backend::None; }

namespace {
bool device_play(Preset) { return false; }
}  // namespace

#endif

}  // namespace mce::platform::haptics
