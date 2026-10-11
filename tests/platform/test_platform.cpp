#include <doctest/doctest.h>

#include <filesystem>
#include <fstream>
#include <string>

#include "mce/platform/platform.h"
#include "mce/platform/window.h"

using namespace mce;

// No-exception doctest: a failed REQUIRE does not leave the test case, so bail out explicitly.
#define MCE_REQUIRE_OR_RETURN(...)              \
    do {                                        \
        const bool mce_req_ok_ = (__VA_ARGS__); \
        REQUIRE_MESSAGE(mce_req_ok_, #__VA_ARGS__); \
        if (!mce_req_ok_) return;               \
    } while (0)

namespace {

struct Recorder {
    platform::Event events[64];
    u32 count = 0;
    static void on_event(const platform::Event& e, void* user) {
        auto* r = static_cast<Recorder*>(user);
        if (r->count < 64) r->events[r->count++] = e;
    }
    bool saw(platform::EventType t) const {
        for (u32 i = 0; i < count; ++i)
            if (events[i].type == t) return true;
        return false;
    }
};

// SDL's dummy video driver: windows without a display, valid on every CI runner.
struct PlatformScope {
    PlatformScope() {
        platform::PlatformDesc d;
        d.video_driver = "dummy";
        ok = platform::init(d).has_value();
    }
    ~PlatformScope() { platform::shutdown(); }
    bool ok = false;
};

}  // namespace

TEST_CASE("platform: init, driver, shutdown") {
    {
        PlatformScope p;
        MCE_REQUIRE_OR_RETURN(p.ok);
        CHECK(platform::is_initialized());
        CHECK(StringView(platform::video_driver()) == "dummy");
        CHECK_FALSE(platform::pump_events());
    }
    CHECK_FALSE(platform::is_initialized());
    CHECK(StringView(platform::video_driver()).empty());
}

TEST_CASE("platform: window create requires init") {
    platform::WindowDesc wd;
    wd.opengl = false;
    auto w = platform::Window::create(wd);
    MCE_REQUIRE_OR_RETURN(!w.has_value());
    CHECK(w.error().code == Errc::PlatformInit);
}

TEST_CASE("platform: window lifecycle and close request") {
    PlatformScope p;
    MCE_REQUIRE_OR_RETURN(p.ok);
    platform::WindowDesc wd;
    wd.title = "test";
    wd.width = 320;
    wd.height = 200;
    wd.opengl = false;  // the dummy driver has no GL
    auto w = platform::Window::create(wd);
    MCE_REQUIRE_OR_RETURN(w.has_value());
    platform::Window& win = *w.value();
    CHECK(win.native_handle() != nullptr);
    CHECK(win.id() != 0);
    const platform::PixelSize fb = win.framebuffer_size();
    CHECK(fb.width >= 320);
    CHECK(fb.height >= 200);
    CHECK(win.display_scale() > 0.0f);
    win.set_title("renamed");

    Recorder rec;
    win.set_event_handler(&Recorder::on_event, &rec);
    CHECK_FALSE(win.close_requested());
    win.request_close();
    CHECK(platform::pump_events());  // a window close ends the loop
    CHECK(win.close_requested());
    CHECK(rec.saw(platform::EventType::Close));
    for (u32 i = 0; i < rec.count; ++i)
        if (rec.events[i].type == platform::EventType::Close) CHECK(rec.events[i].window_id == win.id());

    win.set_event_handler(nullptr, nullptr);
    CHECK_FALSE(platform::pump_events());
}

TEST_CASE("platform: clock is monotonic") {
    const f64 a = platform::now_seconds();
    const u64 ua = platform::now_us();
    const f64 b = platform::now_seconds();
    CHECK(b >= a);
    CHECK(platform::now_us() >= ua);
}

TEST_CASE("platform: event names") {
    CHECK(StringView(platform::event_type_name(platform::EventType::KeyDown)) == "KeyDown");
    CHECK(StringView(platform::event_type_name(platform::EventType::Close)) == "Close");
    CHECK(static_cast<u32>(platform::Key::Escape) == 41);  // USB HID usage, = SDL_SCANCODE_ESCAPE
    CHECK(static_cast<u32>(platform::Key::Z) == 29);
}

TEST_CASE("platform: a virtual gamepad hot-plugs into slot 0 and streams standard-layout events") {
    PlatformScope p;
    MCE_REQUIRE_OR_RETURN(p.ok);
    platform::WindowDesc wd;
    wd.opengl = false;
    auto w = platform::Window::create(wd);
    MCE_REQUIRE_OR_RETURN(w.has_value());
    Recorder rec;
    w.value()->set_event_handler(&Recorder::on_event, &rec);

    const u32 pad = platform::attach_virtual_gamepad("test pad");
    MCE_REQUIRE_OR_RETURN(pad != 0);
    platform::pump_events();
    CHECK(rec.saw(platform::EventType::GamepadAdded));
    CHECK(platform::gamepad_connected(0));
    CHECK_FALSE(platform::gamepad_connected(1));
    CHECK(StringView(platform::gamepad_name(0)).size() > 0);

    rec.count = 0;
    platform::set_virtual_gamepad_button(pad, platform::GamepadButton::South, true);
    platform::set_virtual_gamepad_axis(pad, platform::GamepadAxis::LeftX, -1.f);
    platform::set_virtual_gamepad_axis(pad, platform::GamepadAxis::RightTrigger, 0.5f);
    platform::pump_events();
    bool south = false, left = false, trigger = false;
    for (u32 i = 0; i < rec.count; ++i) {
        const platform::Event& e = rec.events[i];
        CHECK(e.flags == 0);  // slot 0
        if (e.type == platform::EventType::GamepadButtonDown && e.code == u32(platform::GamepadButton::South)) south = true;
        if (e.type == platform::EventType::GamepadAxis && e.code == u32(platform::GamepadAxis::LeftX)) left = e.x == doctest::Approx(-1.f);
        if (e.type == platform::EventType::GamepadAxis && e.code == u32(platform::GamepadAxis::RightTrigger))
            trigger = e.x == doctest::Approx(0.5f).epsilon(1e-3);
    }
    CHECK(south);
    CHECK(left);
    CHECK(trigger);

    rec.count = 0;
    platform::detach_virtual_gamepad(pad);
    platform::pump_events();
    CHECK(rec.saw(platform::EventType::GamepadRemoved));
    CHECK_FALSE(platform::gamepad_connected(0));
    w.value()->set_event_handler(nullptr, nullptr);
}

TEST_CASE("platform: fingers get small reusable slots and normalized positions") {
    PlatformScope p;
    MCE_REQUIRE_OR_RETURN(p.ok);
    platform::WindowDesc wd;
    wd.opengl = false;
    auto w = platform::Window::create(wd);
    MCE_REQUIRE_OR_RETURN(w.has_value());
    Recorder rec;
    w.value()->set_event_handler(&Recorder::on_event, &rec);
    const u32 id = w.value()->id();
    using platform::EventType;
    using platform::TouchPhase;

    platform::inject_touch(id, 0xdead'beef'0000ull, TouchPhase::Down, 0.25f, 0.75f);
    platform::inject_touch(id, 77, TouchPhase::Down, 0.5f, 0.5f);
    platform::inject_touch(id, 0xdead'beef'0000ull, TouchPhase::Move, 0.3f, 0.7f);
    platform::inject_touch(id, 0xdead'beef'0000ull, TouchPhase::Up, 0.3f, 0.7f);
    platform::inject_touch(id, 99, TouchPhase::Down, 0.1f, 0.1f);  // takes the freed slot 0
    platform::inject_touch(id, 77, TouchPhase::Cancel, 0.5f, 0.5f);
    platform::inject_touch(id, 12345, TouchPhase::Move, 0.5f, 0.5f);  // never went down: dropped
    platform::pump_events();

    struct Want {
        EventType type;
        u32 slot;
        f32 x;
        u32 flags;
    };
    const Want want[] = {
        {EventType::TouchDown, 0, 0.25f, 0}, {EventType::TouchDown, 1, 0.5f, 0}, {EventType::TouchMove, 0, 0.3f, 0},
        {EventType::TouchUp, 0, 0.3f, 0},    {EventType::TouchDown, 0, 0.1f, 0},
        {EventType::TouchUp, 1, 0.5f, platform::EventFlag::TouchCanceled},
    };
    u32 n = 0;
    for (u32 i = 0; i < rec.count; ++i) {
        const platform::Event& e = rec.events[i];
        if (e.type != EventType::TouchDown && e.type != EventType::TouchMove && e.type != EventType::TouchUp) continue;
        CHECK(e.type != EventType::MouseMove);
        REQUIRE(n < 6);
        CHECK(e.type == want[n].type);
        CHECK(e.code == want[n].slot);
        CHECK(e.x == doctest::Approx(want[n].x));
        CHECK(e.flags == want[n].flags);
        ++n;
    }
    CHECK(n == 6);
    for (u32 i = 0; i < rec.count; ++i)  // no mouse synthesized from fingers
        CHECK(rec.events[i].type != EventType::MouseButtonDown);
    CHECK(StringView(platform::event_type_name(EventType::TouchMove)) == "TouchMove");
    platform::inject_touch(id, 99, TouchPhase::Up, 0.1f, 0.1f);
    platform::pump_events();
    w.value()->set_event_handler(nullptr, nullptr);
}

TEST_CASE("platform: app lifecycle events reach every window synchronously; desktop has no insets") {
    PlatformScope p;
    MCE_REQUIRE_OR_RETURN(p.ok);
    platform::WindowDesc wd;
    wd.opengl = false;
    auto a = platform::Window::create(wd);
    auto b = platform::Window::create(wd);
    MCE_REQUIRE_OR_RETURN(a.has_value() && b.has_value());
    Recorder ra, rb;
    a.value()->set_event_handler(&Recorder::on_event, &ra);
    b.value()->set_event_handler(&Recorder::on_event, &rb);
    using platform::EventType;

    platform::simulate_app_event(EventType::AppBackground);
    CHECK(ra.saw(EventType::AppBackground));  // before any pump: the OS callback's call stack
    CHECK(rb.saw(EventType::AppBackground));
    platform::simulate_app_event(EventType::AppForeground);
    platform::simulate_app_event(EventType::AppLowMemory);
    platform::simulate_app_event(EventType::AppTerminating);
    platform::simulate_app_event(EventType::KeyDown);  // not a lifecycle event: ignored
    CHECK(ra.count == 4);
    CHECK(ra.events[1].type == EventType::AppForeground);
    CHECK(ra.events[3].type == EventType::AppTerminating);
    CHECK(StringView(platform::event_type_name(EventType::SafeAreaChanged)) == "SafeAreaChanged");

    const platform::SafeInsets none;
    CHECK(a.value()->safe_insets() == none);
    CHECK(static_cast<u8>(a.value()->orientation()) <= static_cast<u8>(platform::Orientation::PortraitFlipped));
    a.value()->set_event_handler(nullptr, nullptr);
    b.value()->set_event_handler(nullptr, nullptr);
}

TEST_CASE("platform: list_files names the regular files of a folder, sorted; a missing folder is empty") {
    namespace fs = std::filesystem;
    const fs::path dir = fs::path(MCE_TEST_TMP_DIR) / "list_files";
    fs::remove_all(dir);
    fs::create_directories(dir / "sub.mce");
    for (const char* n : {"b.mce", "a.mce", "load_order.txt"}) std::ofstream(dir / n) << "x";
    const std::string d = dir.generic_string() + "/";
    Array<FixedString<256>> names;
    mce::platform::list_files(StringView(d.data(), d.size()), names);
    REQUIRE(names.size() == 3);  // the sub.mce directory is left out
    if (names.size() != 3) return;
    CHECK(names[0].view() == "a.mce");
    CHECK(names[1].view() == "b.mce");
    CHECK(names[2].view() == "load_order.txt");
    const std::string missing = (dir / "nope").generic_string();
    mce::platform::list_files(StringView(missing.data(), missing.size()), names);
    CHECK(names.empty());
}
