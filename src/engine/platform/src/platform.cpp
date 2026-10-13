#include "mce/platform/platform.h"

#include <SDL3/SDL.h>

#include <algorithm>
#include <cstring>

#include "internal.h"
#include "mce/app.h"
#include "mce/convar.h"
#include "mce/log.h"

namespace mce::platform {
namespace {

MCE_CVAR(cv_video_driver, "platform.video_driver", "", "SDL video driver override (dummy, offscreen, x11, ...)",
         CVarFlags::RequiresRestart);
MCE_CVAR(cv_error_box, "platform.error_box", true, "Show a message box on boot failure", CVarFlags::None);
MCE_CVAR(cv_orientations, "platform.orientations", "LandscapeLeft LandscapeRight",
         "Phones: allowed screen orientations (SDL_HINT_ORIENTATIONS: LandscapeLeft LandscapeRight Portrait PortraitUpsideDown)",
         CVarFlags::RequiresRestart);

constexpr u32 kMaxWindows = 8;

struct State {
    bool initialized = false;
    Window* windows[kMaxWindows] = {};
    // SDL stamps events with SDL_GetTicksNS() (time since SDL init); the engine clock is
    // now_us(). Event::timestamp_us is rebased onto now_us() so mce_input can quantize events
    // against the same clock the fixed-step accumulator runs on (audio-input-ui §3.2).
    i64 sdl_to_engine_us = 0;
    // Touch slots (FR-INP-070): SDL finger ids are opaque 64-bit values; the engine hands out the
    // lowest free slot on finger down and frees it on up, so slot numbers stay small and stable.
    struct Finger {
        SDL_TouchID touch = 0;
        SDL_FingerID finger = 0;
        bool used = false;
    };
    Finger fingers[kMaxTouches] = {};
};
State g_state;

constexpr u32 kNoTouchSlot = ~0u;

u32 find_finger(SDL_TouchID t, SDL_FingerID f) {
    for (u32 i = 0; i < kMaxTouches; ++i)
        if (g_state.fingers[i].used && g_state.fingers[i].touch == t && g_state.fingers[i].finger == f) return i;
    return kNoTouchSlot;
}

u32 claim_finger(SDL_TouchID t, SDL_FingerID f) {
    if (const u32 s = find_finger(t, f); s != kNoTouchSlot) return s;
    for (u32 i = 0; i < kMaxTouches; ++i) {
        if (!g_state.fingers[i].used) {
            g_state.fingers[i] = {t, f, true};
            return i;
        }
    }
    return kNoTouchSlot;
}

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

void deliver_all(const Event& e) {
    for (Window* w : g_state.windows) deliver(w, e);
}

// SDL hands mobile lifecycle events only to event watchers, from inside the OS callback (iOS app
// delegate, Android's lifecycle pump), on the main thread. They never reach SDL_PollEvent.
bool SDLCALL lifecycle_watch(void* /*user*/, SDL_Event* ev) {
    Event e;
    switch (ev->type) {
        case SDL_EVENT_WILL_ENTER_BACKGROUND: e.type = EventType::AppBackground; break;
        case SDL_EVENT_DID_ENTER_FOREGROUND: e.type = EventType::AppForeground; break;
        case SDL_EVENT_LOW_MEMORY: e.type = EventType::AppLowMemory; break;
        case SDL_EVENT_TERMINATING: e.type = EventType::AppTerminating; break;
        default: return true;
    }
    e.timestamp_us = now_us();
    deliver_all(e);
    return true;
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
        case EventType::TouchDown: return "TouchDown";
        case EventType::TouchMove: return "TouchMove";
        case EventType::TouchUp: return "TouchUp";
        case EventType::AppBackground: return "AppBackground";
        case EventType::AppForeground: return "AppForeground";
        case EventType::AppLowMemory: return "AppLowMemory";
        case EventType::AppTerminating: return "AppTerminating";
        case EventType::SafeAreaChanged: return "SafeAreaChanged";
        case EventType::OrientationChanged: return "OrientationChanged";
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
    // Touch and mouse stay separate devices: no mouse events synthesized from fingers (a drag on
    // a virtual stick would also read as mouse look) and no fingers from the mouse. mce_ui takes
    // its pointer from either (ui::apply_touch_pointer).
    SDL_SetHint(SDL_HINT_TOUCH_MOUSE_EVENTS, "0");
    SDL_SetHint(SDL_HINT_MOUSE_TOUCH_EVENTS, "0");
#if defined(SDL_PLATFORM_ANDROID) || defined(SDL_PLATFORM_IOS)
    // Phones: games run landscape (either way up) unless the convar says otherwise. Without the
    // hint SDL derives the orientation from the window's aspect and resizability.
    SDL_SetHint(SDL_HINT_ORIENTATIONS, cv_orientations.get_string().data());
#endif
    // The engine owns its quit policy: closing the last window still quits (SDL default).
    if (!SDL_Init(SDL_INIT_VIDEO | SDL_INIT_EVENTS)) {
        MCE_LOG_ERROR("platform: SDL_Init failed: {}", SDL_GetError());
        return Error(Errc::PlatformInit, "platform.init", SDL_GetError());
    }
    // Gamepads are optional: a missing joystick backend leaves the game keyboard/mouse-only.
    if (!SDL_InitSubSystem(SDL_INIT_GAMEPAD)) MCE_LOG_WARN("platform: no gamepad support: {}", SDL_GetError());
    SDL_AddEventWatch(&lifecycle_watch, nullptr);
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
    for (auto& f : g_state.fingers) f = {};
    SDL_RemoveEventWatch(&lifecycle_watch, nullptr);
    if (detail::audio_openers_pending()) {
        // An audio device is still opening on its own thread and holds SDL's audio lock; SDL_Quit
        // would wait for it. The process is on its way out: leave SDL to the exit.
        MCE_LOG_WARN("platform: shutdown with an audio device still opening; SDL left to the process exit");
    } else {
        SDL_Quit();
    }
    g_state.initialized = false;
}

bool is_initialized() { return g_state.initialized; }

#if defined(SDL_PLATFORM_ANDROID)
namespace {

// Copies the APK asset `name` to `dest` through SDL's asset-aware IO.
bool extract_asset(const char* name, const char* dest) {
    SDL_IOStream* in = SDL_IOFromFile(name, "rb");
    if (!in) return false;
    SDL_IOStream* out = SDL_IOFromFile(dest, "wb");
    bool ok = out != nullptr;
    static char buf[1 << 16];
    while (ok) {
        const size_t n = SDL_ReadIO(in, buf, sizeof(buf));
        if (n == 0) break;
        ok = SDL_WriteIO(out, buf, n) == n;
    }
    ok = ok && SDL_GetIOStatus(in) == SDL_IO_STATUS_EOF;
    SDL_CloseIO(in);
    if (out && !SDL_CloseIO(out)) ok = false;
    return ok;
}

// The APK's assets are not files, but the pack is mounted (and memory-mapped) from a path.
// scripts/ship.py --target android lists the bundled files in the asset mce_bundle.txt ("<name> <size>"
// per line, '#' starts a comment); the first call copies each into the app's internal storage, again
// whenever the list differs from the copy kept there (an app update), and returns that directory. The
// shipped-file lookups (the pack beside the executable, FR-SHP-010) then work unchanged.
const char* android_bundle_dir() {
    static FixedString<1024> dir;
    static bool done = false;
    if (done) return dir.c_str();
    done = true;
    const char* internal = SDL_GetAndroidInternalStoragePath();
    if (!internal) return "";
    dir.append(internal);
    dir.append("/bundle/");
    size_t list_size = 0;
    char* list = static_cast<char*>(SDL_LoadFile("mce_bundle.txt", &list_size));
    if (!list) return dir.c_str();  // nothing bundled (a dev build pushed by hand)
    FixedString<1100> stamp(dir);
    stamp.append("mce_bundle.txt");
    size_t old_size = 0;
    char* old = static_cast<char*>(SDL_LoadFile(stamp.c_str(), &old_size));
    const bool fresh = old && old_size == list_size && std::memcmp(old, list, list_size) == 0;
    SDL_free(old);
    if (!fresh) {
        SDL_CreateDirectory(dir.c_str());
        bool ok = true;
        const char* line = list;
        const char* end = list + list_size;
        while (line < end) {
            const char* eol = static_cast<const char*>(std::memchr(line, '\n', static_cast<size_t>(end - line)));
            if (!eol) eol = end;
            const char* sp = static_cast<const char*>(std::memchr(line, ' ', static_cast<size_t>(eol - line)));
            if (sp && sp > line && *line != '#') {
                const FixedString<256> name(StringView(line, static_cast<usize>(sp - line)));
                FixedString<1300> dest(dir);
                dest.append(name.view());
                if (!extract_asset(name.c_str(), dest.c_str())) {
                    MCE_LOG_ERROR("platform: cannot extract bundled '{}': {}", name.view(), SDL_GetError());
                    ok = false;
                }
            }
            line = eol + 1;
        }
        // The stamp last: an interrupted extraction is redone on the next launch.
        if (ok && !SDL_SaveFile(stamp.c_str(), list, list_size))
            MCE_LOG_WARN("platform: cannot write {}: {}", stamp.view(), SDL_GetError());
        MCE_LOG_INFO("platform: extracted the bundled files to {}", dir.view());
    }
    SDL_free(list);
    return dir.c_str();
}

}  // namespace
#endif

const char* base_path() {
#if defined(SDL_PLATFORM_ANDROID)
    return android_bundle_dir();
#else
    const char* p = SDL_GetBasePath();  // owned and cached by SDL
    return p ? p : "";
#endif
}

void preferred_locales(Array<FixedString<16>>& out) {
    out.clear();
    int count = 0;
    SDL_Locale** list = SDL_GetPreferredLocales(&count);
    if (!list) return;
    for (int i = 0; i < count; ++i) {
        if (!list[i] || !list[i]->language) continue;
        FixedString<16> tag(list[i]->language);
        if (list[i]->country) {
            tag.append("-");
            tag.append(list[i]->country);
        }
        if (!tag.truncated()) out.push_back(tag);
    }
    SDL_free(list);
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
            case SDL_EVENT_WINDOW_SAFE_AREA_CHANGED:
            case SDL_EVENT_WINDOW_DISPLAY_SCALE_CHANGED: {
                Window* w = find_window(ev.window.windowID);
                e.window_id = ev.window.windowID;
                switch (ev.type) {
                    case SDL_EVENT_WINDOW_CLOSE_REQUESTED: e.type = EventType::Close; break;
                    case SDL_EVENT_WINDOW_FOCUS_GAINED: e.type = EventType::FocusIn; break;
                    case SDL_EVENT_WINDOW_FOCUS_LOST: e.type = EventType::FocusOut; break;
                    case SDL_EVENT_WINDOW_MINIMIZED: e.type = EventType::Minimized; break;
                    case SDL_EVENT_WINDOW_RESTORED: e.type = EventType::Restored; break;
                    case SDL_EVENT_WINDOW_SAFE_AREA_CHANGED: e.type = EventType::SafeAreaChanged; break;
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
            case SDL_EVENT_DISPLAY_ORIENTATION:
                e.type = EventType::OrientationChanged;
                e.code = ev.display.data1 >= 0 && ev.display.data1 <= SDL_ORIENTATION_PORTRAIT_FLIPPED
                             ? static_cast<u32>(ev.display.data1)
                             : 0u;
                deliver_all(e);
                break;
            case SDL_EVENT_KEY_DOWN:
            case SDL_EVENT_KEY_UP:
                e.type = ev.type == SDL_EVENT_KEY_DOWN ? EventType::KeyDown : EventType::KeyUp;
                e.window_id = ev.key.windowID;
                e.code = static_cast<u32>(ev.key.scancode);
                e.flags = key_mods(ev.key.mod) | (ev.key.repeat ? u32(EventFlag::Repeat) : 0u);
                deliver(find_window(ev.key.windowID), e);
                break;
            case SDL_EVENT_MOUSE_MOTION:
                if (ev.motion.which == SDL_TOUCH_MOUSEID) break;  // belt and braces: the hint is off
                e.type = EventType::MouseMove;
                e.window_id = ev.motion.windowID;
                if (SDL_Window* w = SDL_GetWindowFromID(ev.motion.windowID); w && SDL_GetWindowRelativeMouseMode(w)) {
                    e.x = ev.motion.xrel;  // the position is pinned in relative mode; the delta is the signal
                    e.y = ev.motion.yrel;
                    e.flags = EventFlag::RelativeMouse;
                } else {
                    e.x = ev.motion.x;
                    e.y = ev.motion.y;
                }
                deliver(find_window(ev.motion.windowID), e);
                break;
            case SDL_EVENT_MOUSE_BUTTON_DOWN:
            case SDL_EVENT_MOUSE_BUTTON_UP:
                if (ev.button.which == SDL_TOUCH_MOUSEID) break;
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
            case SDL_EVENT_FINGER_DOWN:
            case SDL_EVENT_FINGER_MOTION:
            case SDL_EVENT_FINGER_UP:
            case SDL_EVENT_FINGER_CANCELED: {
                if (ev.tfinger.touchID == SDL_MOUSE_TOUCHID) break;
                const u32 slot = ev.type == SDL_EVENT_FINGER_DOWN ? claim_finger(ev.tfinger.touchID, ev.tfinger.fingerID)
                                                                  : find_finger(ev.tfinger.touchID, ev.tfinger.fingerID);
                if (slot == kNoTouchSlot) break;  // more than kMaxTouches fingers, or an unknown one
                e.code = slot;
                e.x = ev.tfinger.x;
                e.y = ev.tfinger.y;
                e.window_id = ev.tfinger.windowID;
                if (ev.type == SDL_EVENT_FINGER_DOWN) {
                    e.type = EventType::TouchDown;
                } else if (ev.type == SDL_EVENT_FINGER_MOTION) {
                    e.type = EventType::TouchMove;
                } else {
                    e.type = EventType::TouchUp;
                    if (ev.type == SDL_EVENT_FINGER_CANCELED) e.flags = EventFlag::TouchCanceled;
                    g_state.fingers[slot] = {};
                }
                Window* w = find_window(ev.tfinger.windowID);
                deliver(w ? w : input_window(), e);
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

void simulate_app_event(EventType type) {
    if (type != EventType::AppBackground && type != EventType::AppForeground && type != EventType::AppLowMemory &&
        type != EventType::AppTerminating)
        return;
    Event e;
    e.type = type;
    e.timestamp_us = now_us();
    deliver_all(e);
}

void inject_touch(u32 window_id, u64 finger, TouchPhase phase, f32 x, f32 y) {
    SDL_Event ev{};
    switch (phase) {
        case TouchPhase::Down: ev.type = SDL_EVENT_FINGER_DOWN; break;
        case TouchPhase::Move: ev.type = SDL_EVENT_FINGER_MOTION; break;
        case TouchPhase::Up: ev.type = SDL_EVENT_FINGER_UP; break;
        case TouchPhase::Cancel: ev.type = SDL_EVENT_FINGER_CANCELED; break;
    }
    ev.tfinger.timestamp = SDL_GetTicksNS();
    ev.tfinger.touchID = 1;  // any id but SDL_MOUSE_TOUCHID
    ev.tfinger.fingerID = finger;
    ev.tfinger.x = x;
    ev.tfinger.y = y;
    ev.tfinger.pressure = phase == TouchPhase::Up || phase == TouchPhase::Cancel ? 0.f : 1.f;
    ev.tfinger.windowID = window_id;
    SDL_PushEvent(&ev);
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
