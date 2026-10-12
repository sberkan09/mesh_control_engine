#pragma once
// mce_platform window (core.md §2.5) — the only SDL_Window touchpoint.
//
// native_handle() returns the SDL_Window* typed as void*: the single sanctioned leak, consumed only
// by mce_rhi_gl (GL context creation) and mce_editor_layer (ImGui platform backend).

#include "mce/memory/allocator.h"
#include "mce/platform/event.h"
#include "mce/result.h"

namespace mce::platform {

struct PixelSize {
    u32 width = 0;
    u32 height = 0;
};

// The GPU API a window's surface is made for (ADR-0038). Resolved once per process: Android and iOS
// always use SdlGpu; elsewhere MCE_RHI=sdlgpu selects it, else OpenGL (Metal on macOS).
// rhi::create_default_device makes the matching device.
enum class GpuApi : u8 { OpenGL, Metal, SdlGpu };
GpuApi gpu_api();

// Pixels the OS keeps for itself at each edge of a window (notch, rounded corners, home
// indicator): UI that must stay visible goes inside them (M-MOBILE, FR-RUN-100). Zero on desktop.
struct SafeInsets {
    u32 left = 0, top = 0, right = 0, bottom = 0;
    bool operator==(const SafeInsets&) const = default;
};

// The display's orientation (values match SDL_DisplayOrientation).
enum class Orientation : u8 { Unknown, Landscape, LandscapeFlipped, Portrait, PortraitFlipped };

struct WindowDesc {
    const char* title = "Mesh Control Engine";
    u32 width = 1600;
    u32 height = 900;
    bool resizable = true;
    bool high_dpi = true;
    bool opengl = true;   // create a GPU-capable surface for gpu_api() (the device makes the context)
    bool hidden = false;  // tests, offscreen captures
};

class Window {
public:
    using EventFn = void (*)(const Event& e, void* user);

    static Result<mem::UniquePtr<Window>> create(const WindowDesc& desc);
    ~Window();
    Window(const Window&) = delete;
    Window& operator=(const Window&) = delete;

    void* native_handle() const { return handle_; }  // SDL_Window* (mce_rhi_gl, editor_layer only)
    u32 id() const { return id_; }

    // Drawable size in pixels, DPI-correct (FR-R2D-080). Can differ from the requested size.
    PixelSize framebuffer_size() const;
    f32 display_scale() const;
    SafeInsets safe_insets() const;  // in pixels, like framebuffer_size()
    Orientation orientation() const;  // of the display the window is on

    void set_title(const char* title);
    void set_size(u32 width, u32 height);
    void set_cursor_visible(bool visible);
    // Relative mouse mode (3D mouse look): the cursor hides and stays put, and MouseMove carries
    // unbounded deltas (EventFlag::RelativeMouse), so turning never stops at the window edge. The OS
    // suspends it while the window is unfocused; turn it off while a console or menu needs the cursor.
    // False when the platform refuses it.
    bool set_relative_mouse(bool on);
    bool relative_mouse() const;
    // TextInput events flow only while on (an IME may show); a console turns it on while open.
    void set_text_input(bool on);
    // Taskbar/title-bar icon from tightly packed RGBA8 rows (FR-SHP-010). False when the platform refuses it.
    bool set_icon(u32 width, u32 height, Span<const u8> rgba);

    // Queues a Close event for this window (delivered by the next pump_events()).
    void request_close();
    bool close_requested() const { return close_requested_; }

    // Receives this window's events during pump_events(). One handler; null to clear.
    void set_event_handler(EventFn fn, void* user);

    // Internal: called by the pump.
    void dispatch(const Event& e);

    Window() = default;  // use create()

private:

    void* handle_ = nullptr;
    u32 id_ = 0;
    EventFn handler_ = nullptr;
    void* handler_user_ = nullptr;
    bool close_requested_ = false;
};

}  // namespace mce::platform
