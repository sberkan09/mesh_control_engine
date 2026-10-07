#include "imgui_platform.h"

#include <SDL3/SDL.h>
#include <imgui.h>

namespace mce::editor {
namespace {

SDL_Window* sdl_window(platform::Window* w) { return w ? static_cast<SDL_Window*>(w->native_handle()) : nullptr; }

char* g_clipboard = nullptr;

const char* get_clipboard(ImGuiContext*) {
    if (g_clipboard) SDL_free(g_clipboard);
    g_clipboard = SDL_GetClipboardText();
    return g_clipboard;
}

void set_clipboard(ImGuiContext*, const char* text) { SDL_SetClipboardText(text); }

void apply_mods(ImGuiIO& io, u32 flags) {
    io.AddKeyEvent(ImGuiMod_Ctrl, (flags & platform::EventFlag::Ctrl) != 0);
    io.AddKeyEvent(ImGuiMod_Shift, (flags & platform::EventFlag::Shift) != 0);
    io.AddKeyEvent(ImGuiMod_Alt, (flags & platform::EventFlag::Alt) != 0);
    io.AddKeyEvent(ImGuiMod_Super, (flags & platform::EventFlag::Gui) != 0);
}

}  // namespace

int imgui_key_from_hid(u32 hid) {
    if (hid >= 4 && hid <= 29) return ImGuiKey_A + static_cast<int>(hid - 4);
    if (hid >= 30 && hid <= 38) return ImGuiKey_1 + static_cast<int>(hid - 30);
    if (hid >= 58 && hid <= 69) return ImGuiKey_F1 + static_cast<int>(hid - 58);
    if (hid >= 89 && hid <= 97) return ImGuiKey_Keypad1 + static_cast<int>(hid - 89);
    switch (hid) {
        case 39: return ImGuiKey_0;
        case 40: return ImGuiKey_Enter;
        case 41: return ImGuiKey_Escape;
        case 42: return ImGuiKey_Backspace;
        case 43: return ImGuiKey_Tab;
        case 44: return ImGuiKey_Space;
        case 45: return ImGuiKey_Minus;
        case 46: return ImGuiKey_Equal;
        case 47: return ImGuiKey_LeftBracket;
        case 48: return ImGuiKey_RightBracket;
        case 49: return ImGuiKey_Backslash;
        case 51: return ImGuiKey_Semicolon;
        case 52: return ImGuiKey_Apostrophe;
        case 53: return ImGuiKey_GraveAccent;
        case 54: return ImGuiKey_Comma;
        case 55: return ImGuiKey_Period;
        case 56: return ImGuiKey_Slash;
        case 57: return ImGuiKey_CapsLock;
        case 70: return ImGuiKey_PrintScreen;
        case 71: return ImGuiKey_ScrollLock;
        case 72: return ImGuiKey_Pause;
        case 73: return ImGuiKey_Insert;
        case 74: return ImGuiKey_Home;
        case 75: return ImGuiKey_PageUp;
        case 76: return ImGuiKey_Delete;
        case 77: return ImGuiKey_End;
        case 78: return ImGuiKey_PageDown;
        case 79: return ImGuiKey_RightArrow;
        case 80: return ImGuiKey_LeftArrow;
        case 81: return ImGuiKey_DownArrow;
        case 82: return ImGuiKey_UpArrow;
        case 83: return ImGuiKey_NumLock;
        case 84: return ImGuiKey_KeypadDivide;
        case 85: return ImGuiKey_KeypadMultiply;
        case 86: return ImGuiKey_KeypadSubtract;
        case 87: return ImGuiKey_KeypadAdd;
        case 88: return ImGuiKey_KeypadEnter;
        case 98: return ImGuiKey_Keypad0;
        case 99: return ImGuiKey_KeypadDecimal;
        case 101: return ImGuiKey_Menu;
        case 103: return ImGuiKey_KeypadEqual;
        case 224: return ImGuiKey_LeftCtrl;
        case 225: return ImGuiKey_LeftShift;
        case 226: return ImGuiKey_LeftAlt;
        case 227: return ImGuiKey_LeftSuper;
        case 228: return ImGuiKey_RightCtrl;
        case 229: return ImGuiKey_RightShift;
        case 230: return ImGuiKey_RightAlt;
        case 231: return ImGuiKey_RightSuper;
        default: return ImGuiKey_None;
    }
}

void ImGuiPlatform::init(platform::Window& window) {
    window_ = &window;
    ImGuiIO& io = ImGui::GetIO();
    io.BackendPlatformName = "mce_platform";
    io.BackendFlags |= ImGuiBackendFlags_HasMouseCursors;

    ImGuiPlatformIO& pio = ImGui::GetPlatformIO();
    pio.Platform_GetClipboardTextFn = &get_clipboard;
    pio.Platform_SetClipboardTextFn = &set_clipboard;

    static constexpr SDL_SystemCursor kCursors[ImGuiMouseCursor_COUNT] = {
        SDL_SYSTEM_CURSOR_DEFAULT,      // Arrow
        SDL_SYSTEM_CURSOR_TEXT,         // TextInput
        SDL_SYSTEM_CURSOR_MOVE,         // ResizeAll
        SDL_SYSTEM_CURSOR_NS_RESIZE,    // ResizeNS
        SDL_SYSTEM_CURSOR_EW_RESIZE,    // ResizeEW
        SDL_SYSTEM_CURSOR_NESW_RESIZE,  // ResizeNESW
        SDL_SYSTEM_CURSOR_NWSE_RESIZE,  // ResizeNWSE
        SDL_SYSTEM_CURSOR_POINTER,      // Hand
        SDL_SYSTEM_CURSOR_WAIT,         // Wait
        SDL_SYSTEM_CURSOR_PROGRESS,     // Progress
        SDL_SYSTEM_CURSOR_NOT_ALLOWED,  // NotAllowed
    };
    static_assert(ImGuiMouseCursor_COUNT <= 16);
    for (int i = 0; i < ImGuiMouseCursor_COUNT; ++i) cursors_[i] = SDL_CreateSystemCursor(kCursors[i]);
}

void ImGuiPlatform::shutdown() {
    if (text_input_) SDL_StopTextInput(sdl_window(window_));
    text_input_ = false;
    for (void*& c : cursors_) {
        if (c) SDL_DestroyCursor(static_cast<SDL_Cursor*>(c));
        c = nullptr;
    }
    if (g_clipboard) SDL_free(g_clipboard);
    g_clipboard = nullptr;
    ImGuiPlatformIO& pio = ImGui::GetPlatformIO();
    pio.Platform_GetClipboardTextFn = nullptr;
    pio.Platform_SetClipboardTextFn = nullptr;
    ImGuiIO& io = ImGui::GetIO();
    io.BackendPlatformName = nullptr;
    io.BackendFlags &= ~ImGuiBackendFlags_HasMouseCursors;
    window_ = nullptr;
}

bool ImGuiPlatform::on_event(const platform::Event& e) {
    using platform::EventType;
    ImGuiIO& io = ImGui::GetIO();
    switch (e.type) {
        case EventType::MouseMove:
            io.AddMouseSourceEvent(ImGuiMouseSource_Mouse);
            io.AddMousePosEvent(e.x, e.y);
            return io.WantCaptureMouse;
        case EventType::MouseButtonDown:
        case EventType::MouseButtonUp: {
            int button = -1;
            switch (static_cast<platform::MouseButton>(e.code)) {
                case platform::MouseButton::Left: button = ImGuiMouseButton_Left; break;
                case platform::MouseButton::Right: button = ImGuiMouseButton_Right; break;
                case platform::MouseButton::Middle: button = ImGuiMouseButton_Middle; break;
                case platform::MouseButton::X1: button = 3; break;
                case platform::MouseButton::X2: button = 4; break;
            }
            if (button < 0) return false;
            io.AddMouseSourceEvent(ImGuiMouseSource_Mouse);
            io.AddMousePosEvent(e.x, e.y);
            io.AddMouseButtonEvent(button, e.type == EventType::MouseButtonDown);
            return io.WantCaptureMouse;
        }
        case EventType::MouseWheel:
            io.AddMouseSourceEvent(ImGuiMouseSource_Mouse);
            io.AddMouseWheelEvent(-e.x, e.y);  // ImGui: +x scrolls left
            return io.WantCaptureMouse;
        case EventType::KeyDown:
        case EventType::KeyUp: {
            apply_mods(io, e.flags);
            const int key = imgui_key_from_hid(e.code);
            if (key != ImGuiKey_None) {
                io.AddKeyEvent(static_cast<ImGuiKey>(key), e.type == EventType::KeyDown);
                io.SetKeyEventNativeData(static_cast<ImGuiKey>(key), -1, static_cast<int>(e.code));
            }
            return io.WantCaptureKeyboard;
        }
        case EventType::TextInput:
            io.AddInputCharacter(e.code);
            return io.WantTextInput;
        case EventType::FocusIn: io.AddFocusEvent(true); return false;
        case EventType::FocusOut: io.AddFocusEvent(false); return false;
        default: return false;
    }
}

void ImGuiPlatform::new_frame(f64 dt) {
    ImGuiIO& io = ImGui::GetIO();
    SDL_Window* w = sdl_window(window_);
    int ww = 0, wh = 0, pw = 0, ph = 0;
    SDL_GetWindowSize(w, &ww, &wh);
    SDL_GetWindowSizeInPixels(w, &pw, &ph);
    if (SDL_GetWindowFlags(w) & SDL_WINDOW_MINIMIZED) ww = wh = 0;
    io.DisplaySize = ImVec2(static_cast<float>(ww), static_cast<float>(wh));
    if (ww > 0 && wh > 0)
        io.DisplayFramebufferScale = ImVec2(static_cast<float>(pw) / static_cast<float>(ww),
                                            static_cast<float>(ph) / static_cast<float>(wh));
    // ImGui asserts on a zero delta (the first frame, or a clamped one).
    io.DeltaTime = dt > 1e-5 ? static_cast<float>(dt) : 1.0f / 60.0f;

    if (io.WantTextInput != text_input_) {
        text_input_ = io.WantTextInput;
        if (text_input_) SDL_StartTextInput(w);
        else SDL_StopTextInput(w);
    }

    if (!(io.ConfigFlags & ImGuiConfigFlags_NoMouseCursorChange)) {
        const ImGuiMouseCursor c = ImGui::GetMouseCursor();
        if (c != cursor_) {
            cursor_ = c;
            if (io.MouseDrawCursor || c == ImGuiMouseCursor_None) {
                SDL_HideCursor();
            } else {
                SDL_SetCursor(static_cast<SDL_Cursor*>(cursors_[c] ? cursors_[c] : cursors_[0]));
                SDL_ShowCursor();
            }
        }
    }
}

}  // namespace mce::editor
