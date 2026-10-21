#include "mce/input/binding_names.h"

#include <cstdio>

namespace mce::input {

using platform::GamepadAxis;
using platform::GamepadButton;
using platform::Key;
using platform::MouseButton;

const char* key_name(Key k) {
    static constexpr const char* kLetters[] = {"A", "B", "C", "D", "E", "F", "G", "H", "I", "J", "K", "L", "M",
                                               "N", "O", "P", "Q", "R", "S", "T", "U", "V", "W", "X", "Y", "Z"};
    static constexpr const char* kDigits[] = {"1", "2", "3", "4", "5", "6", "7", "8", "9", "0"};
    static constexpr const char* kF[] = {"F1", "F2", "F3", "F4", "F5", "F6", "F7", "F8", "F9", "F10", "F11", "F12"};
    const u32 c = static_cast<u32>(k);
    if (c >= static_cast<u32>(Key::A) && c <= static_cast<u32>(Key::Z)) return kLetters[c - static_cast<u32>(Key::A)];
    if (c >= static_cast<u32>(Key::Num1) && c <= static_cast<u32>(Key::Num0)) return kDigits[c - static_cast<u32>(Key::Num1)];
    if (c >= static_cast<u32>(Key::F1) && c <= static_cast<u32>(Key::F12)) return kF[c - static_cast<u32>(Key::F1)];
    switch (k) {
        case Key::Return: return "Enter";
        case Key::Escape: return "Escape";
        case Key::Backspace: return "Backspace";
        case Key::Tab: return "Tab";
        case Key::Space: return "Space";
        case Key::Grave: return "`";
        case Key::Right: return "Right";
        case Key::Left: return "Left";
        case Key::Down: return "Down";
        case Key::Up: return "Up";
        case Key::LCtrl: return "Left Ctrl";
        case Key::LShift: return "Left Shift";
        case Key::LAlt: return "Left Alt";
        case Key::LGui: return "Left Cmd";
        case Key::RCtrl: return "Right Ctrl";
        case Key::RShift: return "Right Shift";
        case Key::RAlt: return "Right Alt";
        case Key::RGui: return "Right Cmd";
        default: break;
    }
    // The remaining HID usages: punctuation and the keypad.
    switch (c) {
        case 45: return "-";
        case 46: return "=";
        case 47: return "[";
        case 48: return "]";
        case 49: return "\\";
        case 51: return ";";
        case 52: return "'";
        case 54: return ",";
        case 55: return ".";
        case 56: return "/";
        case 57: return "Caps Lock";
        case 73: return "Insert";
        case 74: return "Home";
        case 75: return "Page Up";
        case 76: return "Delete";
        case 77: return "End";
        case 78: return "Page Down";
        default: break;
    }
    if (c >= 89 && c <= 98) {  // keypad 1..9, 0
        static constexpr const char* kPad[] = {"Num 1", "Num 2", "Num 3", "Num 4", "Num 5", "Num 6", "Num 7", "Num 8", "Num 9", "Num 0"};
        return kPad[c - 89];
    }
    if (c == 88) return "Num Enter";
    return "Key";
}

const char* mouse_button_name(MouseButton b) {
    switch (b) {
        case MouseButton::Left: return "Mouse Left";
        case MouseButton::Middle: return "Mouse Middle";
        case MouseButton::Right: return "Mouse Right";
        case MouseButton::X1: return "Mouse 4";
        case MouseButton::X2: return "Mouse 5";
    }
    return "Mouse";
}

const char* gamepad_button_name(GamepadButton b) {
    switch (b) {
        case GamepadButton::South: return "Pad South";
        case GamepadButton::East: return "Pad East";
        case GamepadButton::West: return "Pad West";
        case GamepadButton::North: return "Pad North";
        case GamepadButton::Back: return "Pad Back";
        case GamepadButton::Guide: return "Pad Guide";
        case GamepadButton::Start: return "Pad Start";
        case GamepadButton::LeftStick: return "Left Stick Press";
        case GamepadButton::RightStick: return "Right Stick Press";
        case GamepadButton::LeftShoulder: return "Left Shoulder";
        case GamepadButton::RightShoulder: return "Right Shoulder";
        case GamepadButton::DpadUp: return "D-Pad Up";
        case GamepadButton::DpadDown: return "D-Pad Down";
        case GamepadButton::DpadLeft: return "D-Pad Left";
        case GamepadButton::DpadRight: return "D-Pad Right";
        case GamepadButton::RightPaddle1: return "Right Paddle 1";
        case GamepadButton::LeftPaddle1: return "Left Paddle 1";
        case GamepadButton::RightPaddle2: return "Right Paddle 2";
        case GamepadButton::LeftPaddle2: return "Left Paddle 2";
        case GamepadButton::Touchpad: return "Touchpad";
        default: return "Pad Button";
    }
}

const char* gamepad_axis_name(GamepadAxis a) {
    switch (a) {
        case GamepadAxis::LeftX: return "Left Stick X";
        case GamepadAxis::LeftY: return "Left Stick Y";
        case GamepadAxis::RightX: return "Right Stick X";
        case GamepadAxis::RightY: return "Right Stick Y";
        case GamepadAxis::LeftTrigger: return "Left Trigger";
        case GamepadAxis::RightTrigger: return "Right Trigger";
        default: return "Pad Axis";
    }
}

void binding_name(const BindingDef& b, BindingName& out) {
    // The direction the binding pushes the action: its scale, flipped by Invert.
    const bool negative = (b.scale < 0.f) != ((b.flags & BindingFlag::Invert) != 0);
    switch (b.device) {
        case Device::Keyboard: out.assign(key_name(static_cast<Key>(b.code))); return;
        case Device::MouseButton: out.assign(mouse_button_name(static_cast<MouseButton>(b.code))); return;
        case Device::MouseWheel: out.assign(b.axis == 0 ? "Mouse Wheel X" : "Mouse Wheel"); return;
        case Device::MouseDelta: out.assign(b.axis == 0 ? "Mouse X" : "Mouse Y"); return;
        case Device::GamepadButton: out.assign(gamepad_button_name(static_cast<GamepadButton>(b.code))); return;
        case Device::GamepadAxis: {
            const auto a = static_cast<GamepadAxis>(b.code);
            // Sticks report y down-positive: a binding scaled -1 on LeftY reads the stick pushed up.
            switch (a) {
                case GamepadAxis::LeftX: out.assign(negative ? "Left Stick Left" : "Left Stick Right"); return;
                case GamepadAxis::LeftY: out.assign(negative ? "Left Stick Up" : "Left Stick Down"); return;
                case GamepadAxis::RightX: out.assign(negative ? "Right Stick Left" : "Right Stick Right"); return;
                case GamepadAxis::RightY: out.assign(negative ? "Right Stick Up" : "Right Stick Down"); return;
                default: out.assign(gamepad_axis_name(a)); return;
            }
        }
        case Device::TouchZone:
        case Device::TouchStick:
        case Device::TouchTap:
        case Device::TouchSwipe: out.assign("Touch"); return;
    }
    out.assign("?");
}

namespace {
GamepadNamer g_namer = nullptr;
void* g_namer_user = nullptr;
}  // namespace

void set_gamepad_namer(GamepadNamer fn, void* user) {
    g_namer = fn;
    g_namer_user = user;
}

void action_binding_name(StringId action, const BindingDef& b, BindingName& out) {
    const bool pad = b.device == Device::GamepadButton || b.device == Device::GamepadAxis;
    if (pad && g_namer && g_namer(g_namer_user, action, out)) return;
    binding_name(b, out);
}

}  // namespace mce::input
