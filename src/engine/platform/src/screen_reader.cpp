#include "mce/platform/screen_reader.h"

#include <SDL3/SDL.h>

#include "mce/fixed_string.h"
#include "mce/platform/window.h"

#if defined(SDL_PLATFORM_ANDROID)
#include "android_jni.h"
#endif

namespace mce::platform::screen_reader {
namespace {

Stats g_stats;
Sink g_sink = nullptr;
void* g_sink_user = nullptr;
[[maybe_unused]] SDL_Window* g_window = nullptr;  // read by the Windows backend only

bool device_running();
bool device_announce(StringView text);

}  // namespace

// The Windows and macOS backends live in their own files (UI Automation, AppKit).
#if defined(SDL_PLATFORM_WINDOWS) && defined(MCE_WIN32_ACCESS)
bool win32_running();
bool win32_announce(SDL_Window* window, StringView text);
#elif defined(SDL_PLATFORM_MACOS) && defined(MCE_APPKIT_ACCESS)
bool apple_running();
bool apple_announce(StringView text);
#endif

const char* backend_name(Backend b) {
    switch (b) {
        case Backend::UIKit: return "uikit";
        case Backend::Android: return "android";
        case Backend::MacOS: return "macos";
        case Backend::Windows: return "windows";
        case Backend::None: break;
    }
    return "none";
}

void attach(const Window& window) { g_window = static_cast<SDL_Window*>(window.native_handle()); }

void set_sink(Sink sink, void* user) {
    g_sink = sink;
    g_sink_user = user;
}

Stats stats() { return g_stats; }

bool running() { return device_running(); }

bool announce(StringView text) {
    if (text.empty()) return false;
    ++g_stats.announced;
    if (g_sink) g_sink(g_sink_user, text);
    if (!device_announce(text)) return false;
    ++g_stats.spoken;
    return true;
}

// ---- Android: TalkBack through the stamped MceActivity --------------------------------------------
#if defined(SDL_PLATFORM_ANDROID)

Backend backend() {
    static const bool has = [] {
        detail::Jni j;
        return j.method("mceAnnounce", "(Ljava/lang/String;)V") != nullptr;
    }();
    return has ? Backend::Android : Backend::None;
}

namespace {
bool device_running() {
    if (backend() == Backend::None) return false;
    detail::Jni j;
    const jboolean on = j.env->CallBooleanMethod(j.activity, j.method("mceScreenReaderOn", "()Z"));
    return j.ok("mceScreenReaderOn") && on == JNI_TRUE;
}
bool device_announce(StringView text) {
    if (backend() == Backend::None) return false;
    detail::Jni j;
    jstring s = j.str(text);
    j.env->CallVoidMethod(j.activity, j.method("mceAnnounce", "(Ljava/lang/String;)V"), s);
    j.env->DeleteLocalRef(s);
    return j.ok("mceAnnounce");
}
}  // namespace

// ---- iOS: VoiceOver (screen_reader_uikit.swift) ---------------------------------------------------
#elif defined(SDL_PLATFORM_IOS) && defined(MCE_UIKIT_ACCESS)

extern "C" bool mce_uikit_voiceover_running();          // @_cdecl in screen_reader_uikit.swift
extern "C" void mce_uikit_announce(const char* utf8);  // copies the text before returning

Backend backend() { return Backend::UIKit; }

namespace {
bool device_running() { return mce_uikit_voiceover_running(); }
bool device_announce(StringView text) {
    const FixedString<512> z(text);
    mce_uikit_announce(z.c_str());
    return true;
}
}  // namespace

// ---- Windows: UI Automation (screen_reader_win32.cpp) ---------------------------------------------
#elif defined(SDL_PLATFORM_WINDOWS) && defined(MCE_WIN32_ACCESS)

Backend backend() { return Backend::Windows; }

namespace {
bool device_running() { return win32_running(); }
bool device_announce(StringView text) { return win32_announce(g_window, text); }
}  // namespace

// ---- macOS: AppKit (screen_reader_apple.mm) -------------------------------------------------------
#elif defined(SDL_PLATFORM_MACOS) && defined(MCE_APPKIT_ACCESS)

Backend backend() { return Backend::MacOS; }

namespace {
bool device_running() { return apple_running(); }
bool device_announce(StringView text) { return apple_announce(text); }
}  // namespace

// ---- Linux desktop, the web, builds without MCE_SCREEN_READER_NATIVE: the sink only ---------------
#else

Backend backend() { return Backend::None; }

namespace {
bool device_running() { return false; }
bool device_announce(StringView) { return false; }
}  // namespace

#endif

}  // namespace mce::platform::screen_reader
