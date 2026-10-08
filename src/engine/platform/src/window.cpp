#include "mce/platform/window.h"

#include <SDL3/SDL.h>

#include "internal.h"
#include "mce/log.h"
#include "mce/platform/platform.h"

namespace mce::platform {
namespace {
SDL_Window* sdl(void* h) { return static_cast<SDL_Window*>(h); }
}  // namespace

Result<mem::UniquePtr<Window>> Window::create(const WindowDesc& desc) {
    if (!is_initialized()) return Error(Errc::PlatformInit, "platform.window.create", "platform::init not called");

    SDL_WindowFlags flags = 0;
    if (desc.resizable) flags |= SDL_WINDOW_RESIZABLE;
    if (desc.high_dpi) flags |= SDL_WINDOW_HIGH_PIXEL_DENSITY;
    if (desc.hidden) flags |= SDL_WINDOW_HIDDEN;
#if defined(__APPLE__)
    // macOS renders through mce_rhi_metal (ADR-0027): the GPU surface is a CAMetalLayer, no GL attributes.
    if (desc.opengl) flags |= SDL_WINDOW_METAL;
#else
    if (desc.opengl) {
        flags |= SDL_WINDOW_OPENGL;
        // Default framebuffer: RGBA8 + D24S8, double-buffered. Context version/profile attributes
        // are set by mce_rhi_gl right before it creates the context.
        SDL_GL_SetAttribute(SDL_GL_RED_SIZE, 8);
        SDL_GL_SetAttribute(SDL_GL_GREEN_SIZE, 8);
        SDL_GL_SetAttribute(SDL_GL_BLUE_SIZE, 8);
        SDL_GL_SetAttribute(SDL_GL_ALPHA_SIZE, 8);
        SDL_GL_SetAttribute(SDL_GL_DEPTH_SIZE, 24);
        SDL_GL_SetAttribute(SDL_GL_STENCIL_SIZE, 8);
        SDL_GL_SetAttribute(SDL_GL_DOUBLEBUFFER, 1);
    }
#endif

    SDL_Window* handle =
        SDL_CreateWindow(desc.title, static_cast<int>(desc.width), static_cast<int>(desc.height), flags);
    if (!handle) {
        MCE_LOG_ERROR("platform: SDL_CreateWindow failed: {}", SDL_GetError());
        return Error(Errc::PlatformInit, "platform.window.create", SDL_GetError());
    }

    Window* w = mem::make<Window>(mem::heap());
    w->handle_ = handle;
    w->id_ = SDL_GetWindowID(handle);
    detail::register_window(w);
    const PixelSize fb = w->framebuffer_size();
    MCE_LOG_INFO("platform: window '{}' {}x{} (framebuffer {}x{})", desc.title, desc.width, desc.height, fb.width,
                 fb.height);
    return mem::UniquePtr<Window>(mem::heap(), w);
}

Window::~Window() {
    detail::unregister_window(this);
    if (handle_) SDL_DestroyWindow(sdl(handle_));
}

PixelSize Window::framebuffer_size() const {
    int w = 0, h = 0;
    SDL_GetWindowSizeInPixels(sdl(handle_), &w, &h);
    return PixelSize{static_cast<u32>(w > 0 ? w : 0), static_cast<u32>(h > 0 ? h : 0)};
}

f32 Window::display_scale() const {
    const f32 s = SDL_GetWindowDisplayScale(sdl(handle_));
    return s > 0.0f ? s : 1.0f;
}

void Window::set_title(const char* title) { SDL_SetWindowTitle(sdl(handle_), title); }

void Window::set_size(u32 width, u32 height) {
    SDL_SetWindowSize(sdl(handle_), static_cast<int>(width), static_cast<int>(height));
}

bool Window::set_icon(u32 width, u32 height, Span<const u8> rgba) {
    if (width == 0 || height == 0 || rgba.size() < usize(width) * height * 4) return false;
    SDL_Surface* surface = SDL_CreateSurfaceFrom(static_cast<int>(width), static_cast<int>(height), SDL_PIXELFORMAT_RGBA32,
                                                 const_cast<u8*>(rgba.data()), static_cast<int>(width * 4));
    if (!surface) return false;
    const bool ok = SDL_SetWindowIcon(sdl(handle_), surface);  // SDL copies the pixels
    SDL_DestroySurface(surface);
    return ok;
}

void Window::set_cursor_visible(bool visible) {
    if (visible) SDL_ShowCursor();
    else SDL_HideCursor();
}

void Window::request_close() {
    SDL_Event ev{};
    ev.type = SDL_EVENT_WINDOW_CLOSE_REQUESTED;
    ev.window.windowID = id_;
    ev.window.timestamp = SDL_GetTicksNS();
    SDL_PushEvent(&ev);
}

void Window::set_event_handler(EventFn fn, void* user) {
    handler_ = fn;
    handler_user_ = user;
}

void Window::dispatch(const Event& e) {
    if (e.type == EventType::Close) close_requested_ = true;
    if (handler_) handler_(e, handler_user_);
}

}  // namespace mce::platform
