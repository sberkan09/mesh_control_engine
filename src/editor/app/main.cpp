// mce_editor — the M0 docking editor (ROADMAP M0: "editor opens 01_triangle rendered in a docking
// viewport"). Window + GL device + EditorShell, driven by the one app loop (mce::app::run).
//
//   mce_editor                          run until the window closes
//   mce_editor -frames 120 -selftest    CI smoke: check the viewport and shell pixels, then quit
//   mce_editor -frames 60 -capture shot.ppm
//   mce_editor -editor.ini ""           no dock-layout persistence

#include <cstdio>

#include "mce/core.h"
#include "mce/editor/editor_shell.h"
#include "mce/platform/platform.h"
#include "mce/platform/window.h"
#include "mce/rhi_gl/gl_device.h"

namespace {

using namespace mce;

MCE_CVAR(cv_frames, "frames", 0, "Quit after N frames (0 = run until the window closes)", CVarFlags::None);
MCE_CVAR(cv_capture, "capture", "", "Write the last frame (with -frames) to this .ppm path", CVarFlags::None);
MCE_CVAR(cv_selftest, "selftest", false, "With -frames: verify the viewport and shell pixels on the last frame",
         CVarFlags::None);
MCE_CVAR(cv_ini, "editor.ini", "mce_editor.ini", "Dock layout file (empty = no persistence)", CVarFlags::RequiresRestart);
MCE_CVAR(cv_gl_debug, "r.gl_debug", true, "Enable the GL_KHR_debug validation callback", CVarFlags::RequiresRestart);

class EditorApp final : public app::Application, public app::IPlatformHooks {
public:
    EditorApp(platform::Window& window, rhi::RenderDevice& device) : window_(window), device_(device) {}

    ResultV on_init(app::Context&) override {
        editor::add_default_panels(shell_);
        editor::EditorShellDesc desc;
        desc.window = &window_;
        desc.device = &device_;
        FixedString<256> ini;
        ini.append(cv_ini.get_string());
        desc.ini_path = ini.c_str();
        MCE_TRY(shell_.init(desc));
        window_.set_event_handler(&EditorApp::on_event, this);
        return ok();
    }

    void on_tick(const app::TickContext& t) override { shell_.tick(t.dt); }

    void on_render(const app::RenderContext& rc) override {
        device_.begin_frame(rc.frame_index);
        shell_.build_ui(rc.frame_dt);
        rhi::CommandBuffer cmd(mem::frame());
        shell_.record(cmd, rc.alpha);
        device_.submit(cmd);

        const bool last = cv_frames.get_int() > 0 && rc.frame_index + 1 >= static_cast<u64>(cv_frames.get_int());
        if (last) {
            if (!cv_capture.get_string().empty()) capture(cv_capture.get_string().data());
            if (cv_selftest.get_bool() && !selftest()) exit_code_ = 3;
        }
        device_.end_frame();
        if (last || shell_.quit_requested()) quit(exit_code_);
    }

    void on_shutdown() override {
        window_.set_event_handler(nullptr, nullptr);
        shell_.shutdown();
    }

    // app::IPlatformHooks
    bool pump_events() override { return platform::pump_events(); }
    void present(bool vsync) override { device_.present(vsync); }

private:
    static void on_event(const platform::Event& e, void* user) { static_cast<EditorApp*>(user)->shell_.on_event(e); }

    bool read(rhi::TextureHandle target, rhi::Extent2D size, Array<u8>& px) {
        px.resize(usize(size.width) * size.height * 4);
        return static_cast<bool>(device_.read_pixels(target, rhi::Rect2D{0, 0, size.width, size.height}, px.span()));
    }

    void capture(const char* path) {
        const rhi::Extent2D size = device_.backbuffer_size();
        Array<u8> px;
        if (!read({}, size, px)) return;
        std::FILE* f = std::fopen(path, "wb");
        if (!f) {
            MCE_LOG_ERROR("mce_editor: cannot write '{}'", path);
            return;
        }
        std::fprintf(f, "P6\n%u %u\n255\n", size.width, size.height);
        for (usize i = 0; i < px.size(); i += 4) std::fwrite(&px[i], 1, 3, f);
        std::fclose(f);
        MCE_LOG_INFO("mce_editor: wrote {}", path);
    }

    // The viewport target must hold the stub world's triangle (all three vertex colors dominate
    // somewhere), and the backbuffer must hold more than a clear (the dock shell drew).
    bool selftest() {
        const editor::ViewportInfo& v = shell_.viewport();
        if (v.target.is_null() || v.size.width < 16 || v.size.height < 16) {
            MCE_LOG_ERROR("selftest: viewport panel not visible");
            return false;
        }
        Array<u8> px;
        if (!read(v.target, v.size, px)) {
            MCE_LOG_ERROR("selftest: viewport read-back failed");
            return false;
        }
        u32 dominant[3] = {};
        for (usize i = 0; i < px.size(); i += 4) {
            const int r = px[i], g = px[i + 1], b = px[i + 2];
            if (r > 120 && r > g + 60 && r > b + 60) ++dominant[0];
            if (g > 120 && g > r + 60 && g > b + 60) ++dominant[1];
            if (b > 120 && b > r + 60 && b > g + 60) ++dominant[2];
        }
        const rhi::Extent2D bb = device_.backbuffer_size();
        Array<u8> back;
        if (!read({}, bb, back)) {
            MCE_LOG_ERROR("selftest: backbuffer read-back failed");
            return false;
        }
        u32 distinct = 0;
        u32 seen[64] = {};
        for (usize i = 0; i < back.size() && distinct < 64; i += 4 * 7) {
            const u32 c = back[i] | (u32(back[i + 1]) << 8) | (u32(back[i + 2]) << 16);
            bool found = false;
            for (u32 k = 0; k < distinct; ++k) found |= seen[k] == c;
            if (!found) seen[distinct++] = c;
        }
        MCE_LOG_INFO("selftest: viewport {}x{} red/green/blue px {}/{}/{}; backbuffer {}+ colors", v.size.width,
                     v.size.height, dominant[0], dominant[1], dominant[2], distinct);
        return dominant[0] > 0 && dominant[1] > 0 && dominant[2] > 0 && distinct >= 8;
    }

    platform::Window& window_;
    rhi::RenderDevice& device_;
    editor::EditorShell shell_;
    int exit_code_ = 0;
};

int fail(const char* what, const Error& e) {
    FixedString<512> msg;
    format_error(e, msg);
    FixedString<640> text;
    text.append(what);
    text.append(": ");
    text.append(msg.view());
    platform::show_error_box("mce_editor", text.c_str());
    return 1;
}

}  // namespace

int main(int argc, char** argv) {
    log::LogDesc log_desc;
    log_desc.app_name = "mce_editor";
    log::init(log_desc);
    console::parse_command_line(argc, argv);

    int rc = 1;
    if (ResultV r = platform::init(); !r) {
        rc = fail("platform init failed", r.error());
    } else {
        platform::WindowDesc wd;
        wd.title = "Mesh Control Engine - Editor";
        auto window = platform::Window::create(wd);
        if (!window) {
            rc = fail("window creation failed", window.error());
        } else {
            rhi::DeviceDesc dd;
            dd.debug_layer = cv_gl_debug.get_bool();
            auto device = rhi::gl::create_device(*window.value(), dd);
            if (!device) {
                rc = fail("OpenGL 4.5 device creation failed", device.error());
            } else {
                EditorApp app(*window.value(), *device.value());
                app::AppDesc desc;
                desc.name = "mce_editor";
                desc.platform = &app;
                desc.fences = device.value().get();
                app::RunStats stats;
                rc = app::run(app, desc, &stats);
                const u32 gl_errors = device.value()->validation_error_count();
                std::printf("mce_editor: %llu frames, %llu ticks, %u GL errors\n",
                            static_cast<unsigned long long>(stats.frames), static_cast<unsigned long long>(stats.ticks),
                            gl_errors);
                if (rc == 0 && gl_errors != 0) rc = 2;
            }
        }
    }
    platform::shutdown();
    log::shutdown();
    return rc;
}
