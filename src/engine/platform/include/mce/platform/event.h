#pragma once
// Platform events: the SDL-free event type every consumer sees (core.md §2.5).
// mce_input maps these to actions; the editor feeds them to its UI layer.

#include "mce/types.h"

namespace mce::platform {

enum class EventType : u8 {
    None,
    Close,          // window close button / request_close()
    FocusIn,
    FocusOut,
    Minimized,
    Restored,
    Resized,        // x, y = new framebuffer size in pixels
    ScaleChanged,   // x = new display scale (DPI change, FR-R2D-080)
    KeyDown,        // code = Key (USB HID usage), flags = KeyMod | EventFlag::Repeat
    KeyUp,
    MouseMove,      // x, y = cursor position in window coordinates
    MouseButtonDown,  // code = MouseButton, x, y = position
    MouseButtonUp,
    MouseWheel,     // x, y = scroll delta (positive y = away from the user)
    TextInput,      // code = Unicode code point (one event per code point)
    Quit,           // OS-level quit request (SIGINT, logout, last window closed)
    // Gamepads (v0.5, FR-INP-010): flags = player slot (0..kMaxGamepads-1).
    GamepadAdded,
    GamepadRemoved,
    GamepadButtonDown,  // code = GamepadButton
    GamepadButtonUp,
    GamepadAxis,        // code = GamepadAxis, x = value (sticks -1..1, triggers 0..1)
};

// Key codes are USB HID keyboard usages (layout-independent physical keys); SDL scancodes use the
// same numbering, so the platform layer passes them through untranslated.
enum class Key : u32 {
    Unknown = 0,
    A = 4, B, C, D, E, F, G, H, I, J, K, L, M, N, O, P, Q, R, S, T, U, V, W, X, Y, Z,
    Num1 = 30, Num2, Num3, Num4, Num5, Num6, Num7, Num8, Num9, Num0,
    Return = 40, Escape = 41, Backspace = 42, Tab = 43, Space = 44,
    F1 = 58, F2, F3, F4, F5, F6, F7, F8, F9, F10, F11, F12,
    Right = 79, Left = 80, Down = 81, Up = 82,
    LCtrl = 224, LShift = 225, LAlt = 226, LGui = 227, RCtrl = 228, RShift = 229, RAlt = 230, RGui = 231,
};

enum class MouseButton : u32 { Left = 1, Middle = 2, Right = 3, X1 = 4, X2 = 5 };

// The SDL standard gamepad layout (positional: South is A on Xbox, Cross on PlayStation).
// Values match SDL_GamepadButton / SDL_GamepadAxis, so the platform passes them through.
enum class GamepadButton : u32 {
    South = 0, East, West, North, Back, Guide, Start, LeftStick, RightStick, LeftShoulder, RightShoulder,
    DpadUp, DpadDown, DpadLeft, DpadRight, Misc1, RightPaddle1, LeftPaddle1, RightPaddle2, LeftPaddle2,
    Touchpad, Misc2, Misc3, Misc4, Misc5, Misc6,
    Count,
};
// Sticks: x right-positive, y DOWN-positive (SDL convention); triggers 0 (released) .. 1.
enum class GamepadAxis : u32 { LeftX = 0, LeftY, RightX, RightY, LeftTrigger, RightTrigger, Count };

inline constexpr u32 kMaxGamepads = 4;  // player slots (FR-INP-060: more is not a supported scenario)

namespace EventFlag {
enum : u32 {
    Shift = 1u << 0,
    Ctrl = 1u << 1,
    Alt = 1u << 2,
    Gui = 1u << 3,
    Repeat = 1u << 8,
    // Set by the editor's UI layer on events it captured (ImGui WantCaptureKeyboard/Mouse) before
    // they reach mce_input; input ignores captured presses but still honors releases (§4.6).
    ConsumedByEditor = 1u << 9,
};
}

struct Event {
    EventType type = EventType::None;
    u32 window_id = 0;  // platform window id (Window::id()); 0 for process-level events
    u32 code = 0;
    u32 flags = 0;
    f32 x = 0, y = 0;
    u64 timestamp_us = 0;  // engine clock (platform::now_us() base), not SDL ticks
};

const char* event_type_name(EventType t);

}  // namespace mce::platform
