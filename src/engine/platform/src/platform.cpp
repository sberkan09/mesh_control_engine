#include "mce/platform/platform.h"

#include <SDL3/SDL.h>

#include <algorithm>

#include "internal.h"
#include "mce/app.h"
#include "mce/convar.h"
#include "mce/log.h"

namespace mce::platform {
namespace {

MCE_CVAR(cv_video_driver, "platform.video_driver", "", "SDL video driver override (dummy, offscreen, x11, ...)",
         CVarFlags::RequiresRestart);
MCE_CVAR(cv_error_box, "platform.error_box", true, "Show a message box on boot failure", CVarFlags::None);

constexpr u32 kMaxWindows = 8;

struct State {
    bool initialized = false;
    Window* windows[kMaxWindows] = {};
    // SDL stamps events with SDL_GetTicksNS() (time since SDL init); the engine clock is
    // now_us(). Event::timestamp_us is rebased onto now_us() so mce_input can quantize events
    // against the same clock the fixed-step accumulator runs on (audio-input-ui §3.2).
    i64 sdl_to_engine_us = 0;
};
State g_state;

Window* find_window(SDL_WindowID id) {
    for (Window* w : g_state.windows)
        if (w && w->id() == id) return w;
    return nullptr;
}

u32 key_mods(SDL_Keymod m) {
    u32 f = 0;
    if (m & SDL_KMOD_SHIFT) f |= EventFlag::Shift;
    if (m & SDL_KMOD_CTRL) f |= EventFlag::Ctrl;
    if (m & SDL_KMOD_ALT) f |= EventFlag::Alt;
    if (m & SDL_KMOD_GUI) f |= EventFlag::Gui;
    return f;
}

// Decodes one UTF-8 sequence; advances s. Returns U+FFFD on malformed input.
u32 next_code_point(const char*& s) {
    const auto b0 = static_cast<unsigned char>(*s++);
    if (b0 < 0x80) return b0;
    int extra = (b0 >= 0xF0) ? 3 : (b0 >= 0xE0) ? 2 : (b0 >= 0xC0) ? 1 : -1;
    if (extra < 0) return 0xFFFD;
    u32 cp = b0 & (0x3Fu >> extra);
    for (int i = 0; i < extra; ++i) {
        const auto b = static_cast<unsigned char>(*s);
        if ((b & 0xC0) != 0x80) return 0xFFFD;
        cp = (cp << 6) | (b & 0x3Fu);
        ++s;
    }
    return cp;
}

void deliver(Window* w, const Event& e) {
    if (w) w->dispatch(e);
}

// Process-level device events (gamepads) go to the window with keyboard focus, else the first.
Window* input_window() {
    if (SDL_Window* f = SDL_GetKeyboardFocus())
        if (Window* w = find_window(SDL_GetWindowID(f))) return w;
    for (Window* w : g_state.windows)
        if (w) return w;
    return nullptr;
}

}  // namespace

const char* event_type_name(EventType t) {
    switch (t) {
        case EventType::None: return "None";
        case EventType::Close: return "Close";
        case EventType::FocusIn: return "FocusIn";
        case EventType::FocusOut: return "FocusOut";
        case EventType::Minimized: return "Minimized";
        case EventType::Restored: return "Restored";
        case EventType::Resized: return "Resized";
        case EventType::ScaleChanged: return "ScaleChanged";
        case EventType::KeyDown: return "KeyDown";
        case EventType::KeyUp: return "KeyUp";
        case EventType::MouseMove: return "MouseMove";
        case EventType::MouseButtonDown: return "MouseButtonDown";
        case EventType::MouseButtonUp: return "MouseButtonUp";
        case EventType::MouseWheel: return "MouseWheel";
        case EventType::TextInput: return "TextInput";
        case EventType::Quit: return "Quit";
        case EventType::GamepadAdded: return "GamepadAdded";
        case EventType::GamepadRemoved: return "GamepadRemoved";
        case EventType::GamepadButtonDown: return "GamepadButtonDown";
        case EventType::GamepadButtonUp: return "GamepadButtonUp";
        case EventType::GamepadAxis: return "GamepadAxis";
    }
    return "?";
}

ResultV init(const PlatformDesc& desc) {
    if (g_state.initialized) return ok();
    const StringView cv_driver = cv_video_driver.get_string();
    if (!cv_driver.empty()) {
        SDL_SetHint(SDL_HINT_VIDEO_DRIVER, cv_video_driver.get_string().data());
    } else if (desc.video_driver) {
        SDL_SetHint(SDL_HINT_VIDEO_DRIVER, desc.video_driver);
    }
    // The engine owns its quit policy: closing the last window still quits (SDL default).
    if (!SDL_Init(SDL_INIT_VIDEO | SDL_INIT_EVENTS)) {
        MCE_LOG_ERROR("platform: SDL_Init failed: {}", SDL_GetError());
        return Error(Errc::PlatformInit, "platform.init", SDL_GetError());
    }
    // Gamepads are optional: a missing joystick backend leaves the game keyboard/mouse-only.
    if (!SDL_InitSubSystem(SDL_INIT_GAMEPAD)) MCE_LOG_WARN("platform: no gamepad support: {}", SDL_GetError());
    g_state.initialized = true;
    g_state.sdl_to_engine_us = static_cast<i64>(now_us()) - static_cast<i64>(SDL_GetTicksNS() / 1000);
    MCE_LOG_INFO("platform: SDL {}.{}.{}, video driver '{}'", SDL_MAJOR_VERSION, SDL_MINOR_VERSION,
                 SDL_MICRO_VERSION, video_driver());
    return ok();
}

void shutdown() {
    if (!g_state.initialized) return;
    for (Window* w : g_state.windows) MCE_ASSERT_MSG(w == nullptr, "platform::shutdown with a live window");
    detail::close_gamepads();
    SDL_Quit();
    g_state.initialized = false;
}

bool is_initialized() { return g_state.initialized; }

const char* base_path() {
    const char* p = SDL_GetBasePath();  // owned and cached by SDL
    return p ? p : "";
}

void list_files(StringView dir, Array<FixedString<256>>& out) {
    out.clear();
    FixedString<1024> root(dir);
    if (root.truncated() || root.empty()) return;
    struct Ctx {
        Array<FixedString<256>>* out;
        const char* root;
    } ctx{&out, root.c_str()};
    auto on_entry = [](void* user, const char* dirname, const char* fname) -> SDL_EnumerationResult {
        auto& c = *static_cast<Ctx*>(user);
        FixedString<1300> full(dirname);
        full.append(fname);
        SDL_PathInfo info;
        const FixedString<256> name(fname);
        if (!full.truncated() && !name.truncated() && SDL_GetPathInfo(full.c_str(), &info) && info.type == SDL_PATHTYPE_FILE)
            c.out->push_back(name);
        return SDL_ENUM_CONTINUE;
    };
    if (!SDL_EnumerateDirectory(ctx.root, on_entry, &ctx)) out.clear();
    std::sort(out.begin(), out.end(), [](const FixedString<256>& a, const FixedString<256>& b) { return a.view() < b.view(); });
}

const char* video_driver() {
    if (!g_state.initialized) return "";
    const char* d = SDL_GetCurrentVideoDriver();
    return d ? d : "";
}

bool pump_events() {
    MCE_ASSERT_MSG(g_state.initialized, "platform::pump_events before platform::init");
    bool quit = false;
    SDL_Event ev;
    while (SDL_PollEvent(&ev)) {
        Event e;
        e.timestamp_us = static_cast<u64>(static_cast<i64>(ev.common.timestamp / 1000) + g_state.sdl_to_engine_us);
        switch (ev.type) {
            case SDL_EVENT_QUIT:
                e.type = EventType::Quit;
                quit = true;
                for (Window* w : g_state.windows) deliver(w, e);
                break;
            case SDL_EVENT_WINDOW_CLOSE_REQUESTED:
            case SDL_EVENT_WINDOW_FOCUS_GAINED:
            case SDL_EVENT_WINDOW_FOCUS_LOST:
            case SDL_EVENT_WINDOW_MINIMIZED:
            case SDL_EVENT_WINDOW_RESTORED:
            case SDL_EVENT_WINDOW_PIXEL_SIZE_CHANGED:
            case SDL_EVENT_WINDOW_DISPLAY_SCALE_CHANGED: {
                Window* w = find_window(ev.window.windowID);
                e.window_id = ev.window.windowID;
                switch (ev.type) {
                    case SDL_EVENT_WINDOW_CLOSE_REQUESTED: e.type = EventType::Close; break;
                    case SDL_EVENT_WINDOW_FOCUS_GAINED: e.type = EventType::FocusIn; break;
                    case SDL_EVENT_WINDOW_FOCUS_LOST: e.type = EventType::FocusOut; break;
                    case SDL_EVENT_WINDOW_MINIMIZED: e.type = EventType::Minimized; break;
                    case SDL_EVENT_WINDOW_RESTORED: e.type = EventType::Restored; break;
                    case SDL_EVENT_WINDOW_PIXEL_SIZE_CHANGED:
                        e.type = EventType::Resized;
                        e.x = static_cast<f32>(ev.window.data1);
                        e.y = static_cast<f32>(ev.window.data2);
                        break;
                    default:
                        e.type = EventType::ScaleChanged;
                        e.x = w ? w->display_scale() : 1.0f;
                        break;
                }
                // A close request on any window ends the app loop; multi-window hosts (editor)
                // install a handler and decide themselves via the Close event.
                if (e.type == EventType::Close) quit = true;
                deliver(w, e);
                break;
            }
            case SDL_EVENT_KEY_DOWN:
            case SDL_EVENT_KEY_UP:
                e.type = ev.type == SDL_EVENT_KEY_DOWN ? EventType::KeyDown : EventType::KeyUp;
                e.window_id = ev.key.windowID;
                e.code = static_cast<u32>(ev.key.scancode);
                e.flags = key_mods(ev.key.mod) | (ev.key.repeat ? u32(EventFlag::Repeat) : 0u);
                deliver(find_window(ev.key.windowID), e);
                break;
            case SDL_EVENT_MOUSE_MOTION:
                e.type = EventType::MouseMove;
                e.window_id = ev.motion.windowID;
                e.x = ev.motion.x;
                e.y = ev.motion.y;
                deliver(find_window(ev.motion.windowID), e);
                break;
            case SDL_EVENT_MOUSE_BUTTON_DOWN:
            case SDL_EVENT_MOUSE_BUTTON_UP:
                e.type = ev.type == SDL_EVENT_MOUSE_BUTTON_DOWN ? EventType::MouseButtonDown : EventType::MouseButtonUp;
                e.window_id = ev.button.windowID;
                e.code = ev.button.button;
                e.x = ev.button.x;
                e.y = ev.button.y;
                deliver(find_window(ev.button.windowID), e);
                break;
            case SDL_EVENT_MOUSE_WHEEL:
                e.type = EventType::MouseWheel;
                e.window_id = ev.wheel.windowID;
                e.x = ev.wheel.x;
                e.y = ev.wheel.y;
                if (ev.wheel.direction == SDL_MOUSEWHEEL_FLIPPED) {
                    e.x = -e.x;
                    e.y = -e.y;
                }
                deliver(find_window(ev.wheel.windowID), e);
                break;
            case SDL_EVENT_GAMEPAD_ADDED: {
                const u32 slot = detail::open_gamepad(ev.gdevice.which);
                if (slot == detail::kNoSlot) break;
                e.type = EventType::GamepadAdded;
                e.flags = slot;
                deliver(input_window(), e);
                break;
            }
            case SDL_EVENT_GAMEPAD_REMOVED: {
                const u32 slot = detail::close_gamepad(ev.gdevice.which);
                if (slot == detail::kNoSlot) break;
                e.type = EventType::GamepadRemoved;
                e.flags = slot;
                deliver(input_window(), e);
                break;
            }
            case SDL_EVENT_GAMEPAD_BUTTON_DOWN:
            case SDL_EVENT_GAMEPAD_BUTTON_UP: {
                const u32 slot = detail::find_gamepad(ev.gbutton.which);
                if (slot == detail::kNoSlot) break;
                e.type = ev.type == SDL_EVENT_GAMEPAD_BUTTON_DOWN ? EventType::GamepadButtonDown : EventType::GamepadButtonUp;
                e.code = ev.gbutton.button;
                e.flags = slot;
                deliver(input_window(), e);
                break;
            }
            case SDL_EVENT_GAMEPAD_AXIS_MOTION: {
                const u32 slot = detail::find_gamepad(ev.gaxis.which);
                if (slot == detail::kNoSlot) break;
                e.type = EventType::GamepadAxis;
                e.code = ev.gaxis.axis;
                e.flags = slot;
                e.x = detail::gamepad_axis_value(ev.gaxis.axis, ev.gaxis.value);
                deliver(input_window(), e);
                break;
            }
            case SDL_EVENT_TEXT_INPUT: {
                e.type = EventType::TextInput;
                e.window_id = ev.text.windowID;
                Window* w = find_window(ev.text.windowID);
                for (const char* s = ev.text.text; s && *s;) {
                    e.code = next_code_point(s);
                    deliver(w, e);
                }
                break;
            }
            default: break;
        }
    }
    return quit;
}

f64 now_seconds() { return app::steady_clock().now_seconds(); }
u64 now_us() { return static_cast<u64>(now_seconds() * 1e6); }
void sleep_ms(u32 ms) { SDL_Delay(ms); }

void show_error_box(const char* title, const char* message) {
    MCE_LOG_CRITICAL("{}: {}", title, message);
    if (!cv_error_box.get_bool()) return;
    const StringView driver = video_driver();
    if (driver == "dummy" || driver == "offscreen") return;
    SDL_ShowSimpleMessageBox(SDL_MESSAGEBOX_ERROR, title, message, nullptr);
}

namespace detail {

void register_window(Window* w) {
    for (Window*& slot : g_state.windows) {
        if (!slot) {
            slot = w;
            return;
        }
    }
    MCE_ASSERT_MSG(false, "platform: too many windows");
}

void unregister_window(Window* w) {
    for (Window*& slot : g_state.windows)
        if (slot == w) slot = nullptr;
}

}  // namespace detail
}  // namespace mce::platform
