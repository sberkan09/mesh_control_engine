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

struct WindowDesc {
    const char* title = "Mesh Control Engine";
    u32 width = 1600;
    u32 height = 900;
    bool resizable = true;
    bool high_dpi = true;
    bool opengl = true;   // create a GL-capable surface (mce_rhi_gl makes the context)
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

    void set_title(const char* title);
    void set_size(u32 width, u32 height);
    void set_cursor_visible(bool visible);
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
