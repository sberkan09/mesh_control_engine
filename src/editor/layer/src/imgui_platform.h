#pragma once
// ImGui platform backend over mce::platform (editor-tools.md §2.10 input arbitration).
//
// Input arrives as mce::platform::Event (raw SDL events never leave mce_platform); this file maps it
// to ImGui's io queue. The window's SDL_Window* (Window::native_handle(), the sanctioned leak)
// is used only for what events cannot carry: logical window size, text-input state, cursors and
// the clipboard.

#include "mce/platform/event.h"
#include "mce/platform/window.h"

namespace mce::editor {

class ImGuiPlatform {
public:
    void init(platform::Window& window);
    void shutdown();

    // Feeds one event to ImGui. Returns true when ImGui wants it (the viewport must ignore it).
    bool on_event(const platform::Event& e);
    // Display size, framebuffer scale, delta time, text input and cursor. Before ImGui::NewFrame.
    void new_frame(f64 dt);

private:
    platform::Window* window_ = nullptr;
    bool text_input_ = false;
    int cursor_ = -2;  // ImGuiMouseCursor last applied (-2 = none yet)
    void* cursors_[16] = {};
};

// Opens a file with the system's default application (a .luau error opens its script). False when
// the system has none.
bool open_file_externally(const char* absolute_path);

// USB HID usage (platform::Key / SDL scancode) -> ImGuiKey. ImGuiKey_None when unmapped.
int imgui_key_from_hid(u32 hid);

}  // namespace mce::editor
