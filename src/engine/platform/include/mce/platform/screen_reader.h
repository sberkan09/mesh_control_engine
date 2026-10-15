#pragma once
// mce_platform (L1): the device's screen reader (M-ACCESS step 4). The engine draws its own UI, so a
// screen reader sees one canvas; the player speaks for it: what gets focus, and a focused slider's
// new value, through the reader the player already uses.
//
//   platform::screen_reader::announce("Music, slider, 40%");
//
// - Android: TalkBack (View.announceForAccessibility) through MceActivity's mceAnnounce, stamped
//   into every game's Gradle project (cmake/mobile/android_app.py); the call crosses JNI.
// - With MCE_SCREEN_READER_NATIVE (opt-in until run on each device):
//   iOS: VoiceOver (UIAccessibility announcements; screen_reader_uikit.swift);
//   macOS: VoiceOver (NSAccessibility announcement requests; screen_reader_apple.mm);
//   Windows: Narrator, NVDA and JAWS through a UI Automation notification event on the game's
//   window (screen_reader_win32.cpp).
// - Linux desktop and the web: none (no AT-SPI bridge); announce() still reaches the sink.
//
// running() says whether the OS reports a reader on, so the player can speak only then
// (access.screen_reader 1). Output only: nothing here touches World state. Main thread.

#include "mce/types.h"

namespace mce::platform {
class Window;
}

namespace mce::platform::screen_reader {

enum class Backend : u8 { None, UIKit, Android, MacOS, Windows };
Backend backend();
const char* backend_name(Backend b);  // "none", "uikit", "android", "macos", "windows"

// The window the announcements belong to (Windows: its HWND hosts the UI Automation element).
// The player calls it once its window exists; announce() before then speaks where it can.
void attach(const Window& window);

// The OS reports a screen reader on (VoiceOver, TalkBack with explore by touch, or a Windows
// reader through SPI_GETSCREENREADER). False where it cannot tell.
bool running();

// Speaks `text` now, interrupting what the reader was saying. False when nothing could speak it
// (the sink still sees it).
bool announce(StringView text);

// Every announce() also goes to the sink: tests, and the desktop's -access_log.
using Sink = void (*)(void* user, StringView text);
void set_sink(Sink sink, void* user);

struct Stats {
    u32 announced = 0;  // announce() calls
    u32 spoken = 0;     // handed to the device's reader
};
Stats stats();

}  // namespace mce::platform::screen_reader
