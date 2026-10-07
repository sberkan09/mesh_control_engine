#include <doctest/doctest.h>

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
