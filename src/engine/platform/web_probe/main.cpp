// mce_input_web_probe: the browser's input and audio paths (M-WEB step 4). A web build only: opens a
// window and a silent audio device, then logs what reaches the engine, so a test driving the page
// (scripts/web/input_smoke.cjs) can check each path end to end:
//
//   - audio: the context starts suspended and the first click, tap or key press resumes it
//     (web::install_audio_unlock); the probe logs "probe: audio <state>" on every change.
//   - mouse: the first click asks for pointer lock (Window::set_relative_mouse), the 3D mouse look.
//   - keyboard, touch (fingers on a phone) and the Gamepad API (SDL's emscripten joystick driver).
//   - F asks for full screen (Window::set_fullscreen); Escape ends the probe ("input web probe: done").

#include <emscripten.h>

#include "mce/core.h"
#include "mce/platform/audio_output.h"
#include "mce/platform/platform.h"
#include "mce/platform/window.h"

using namespace mce;

namespace {

EM_JS(int, audio_state_code, (), {
    const s = Module.mceAudioState ? Module.mceAudioState() : "none";
    return s === "running" ? 2 : s === "suspended" ? 1 : 0;
});

const char* audio_state_name(int code) { return code == 2 ? "running" : code == 1 ? "suspended" : "none"; }

void silence(void*, f32* out, u32 frames) {
    for (u32 i = 0; i < frames * 2; ++i) out[i] = 0.0f;
}

struct Probe {
    platform::Window* window = nullptr;
    bool done = false;
};

void on_event(const platform::Event& e, void* user) {
    Probe& p = *static_cast<Probe*>(user);
    using platform::EventType;
    switch (e.type) {
        case EventType::KeyDown:
            if (e.flags & platform::EventFlag::Repeat) break;
            MCE_LOG_INFO("probe: key {}", e.code);
            if (e.code == static_cast<u32>(platform::Key::Escape)) p.done = true;
            if (e.code == static_cast<u32>(platform::Key::F))
                MCE_LOG_INFO("probe: fullscreen request {}", p.window->set_fullscreen(true) ? "accepted" : "refused");
            break;
        case EventType::MouseButtonDown:
            MCE_LOG_INFO("probe: mouse button {}", e.code);
            if (!p.window->relative_mouse())
                MCE_LOG_INFO("probe: pointer lock request {}", p.window->set_relative_mouse(true) ? "accepted" : "refused");
            break;
        case EventType::MouseMove:
            if (e.flags & platform::EventFlag::RelativeMouse) MCE_LOG_INFO("probe: relative move {} {}", e.x, e.y);
            break;
        case EventType::TouchDown: MCE_LOG_INFO("probe: touch down {} at {:.2f} {:.2f}", e.code, e.x, e.y); break;
        case EventType::TouchUp: MCE_LOG_INFO("probe: touch up {}", e.code); break;
        case EventType::GamepadAdded: MCE_LOG_INFO("probe: gamepad added {} '{}'", e.flags, platform::gamepad_name(e.flags)); break;
        case EventType::GamepadButtonDown: MCE_LOG_INFO("probe: gamepad button {} on {}", e.code, e.flags); break;
        case EventType::GamepadAxis:
            MCE_LOG_INFO("probe: gamepad axis {} {:.2f}", e.code, e.x);
            break;
        case EventType::Close:
        case EventType::Quit: p.done = true; break;
        default: break;
    }
}

}  // namespace

int main() {
    log::init({.app_name = "mce_input_web_probe", .level = log::Level::Info});
    if (!platform::init()) {
        MCE_LOG_ERROR("input web probe: platform init failed");
        return 1;
    }
    {
        platform::WindowDesc wd;
        wd.title = "mce input probe";
        wd.width = 640;
        wd.height = 360;
        wd.opengl = false;
        auto window = platform::Window::create(wd);
        if (!window) {
            FixedString<512> why;
            format_error(window.error(), why);
            MCE_LOG_ERROR("input web probe: no window: {}", why.c_str());
            return 1;
        }
        auto audio = platform::AudioOutput::open({}, &silence, nullptr);
        if (!audio) MCE_LOG_WARN("input web probe: no audio device");
        Probe probe;
        probe.window = window.value().get();
        probe.window->set_event_handler(&on_event, &probe);
        MCE_LOG_INFO("input web probe: running");
        int audio_state = -1;
        const f64 end = platform::now_seconds() + 120.0;
        while (!probe.done && platform::now_seconds() < end) {
            if (platform::pump_events()) break;
            if (const int s = audio_state_code(); s != audio_state) {
                audio_state = s;
                MCE_LOG_INFO("probe: audio {}", audio_state_name(s));
            }
            emscripten_sleep(16);
        }
        MCE_LOG_INFO("input web probe: done");
        probe.window->set_event_handler(nullptr, nullptr);
    }
    platform::shutdown();
    return 0;
}
