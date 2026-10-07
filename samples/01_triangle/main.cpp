// 01_triangle — M0 sample: SDL3 window + mce_rhi on the OpenGL 4.5 backend, driven by the one app
// loop (mce::app::run). A colored triangle spins at a fixed sim rate; rendering interpolates the
// angle with the loop's alpha (FR-RUN-020).
//
//   01_triangle                 run until the window closes (Esc quits)
//   01_triangle -frames 120     quit after 120 frames (CI smoke)
//   01_triangle -frames 60 -capture tri.ppm   also write the last frame as a PPM image

#include <cstdio>

#include "mce/core.h"
#include "mce/platform/platform.h"
#include "mce/platform/window.h"
#include "mce/rhi/rhi.h"
#include "mce/rhi_gl/gl_device.h"

namespace {

using namespace mce;
using namespace mce::math;

MCE_CVAR(cv_frames, "frames", 0, "Quit after N frames (0 = run until the window closes)", CVarFlags::None);
MCE_CVAR(cv_capture, "capture", "", "Write the last frame (with -frames) to this .ppm path", CVarFlags::None);
MCE_CVAR(cv_gl_debug, "r.gl_debug", true, "Enable the GL_KHR_debug validation callback", CVarFlags::RequiresRestart);

constexpr const char* kVertexShader = R"(#version 450 core
layout(std140, binding = 0) uniform FrameU { mat4 transform; };
layout(location = 0) in vec2 a_pos;
layout(location = 1) in vec4 a_color;
layout(location = 0) out vec4 v_color;
out gl_PerVertex { vec4 gl_Position; };
void main() {
    v_color = a_color;
    gl_Position = transform * vec4(a_pos, 0.0, 1.0);
}
)";

constexpr const char* kFragmentShader = R"(#version 450 core
layout(location = 0) in vec4 v_color;
layout(location = 0) out vec4 o_color;
void main() { o_color = v_color; }
)";

struct Vertex {
    f32 x, y;
    u32 rgba;  // little-endian RGBA8: 0xAABBGGRR
};

struct FrameU {
    mat4 transform;
};

class TriangleApp final : public app::Application, public app::IPlatformHooks {
public:
    TriangleApp(platform::Window& window, rhi::RenderDevice& device) : window_(window), device_(device) {}

    ResultV on_init(app::Context&) override {
        static constexpr rhi::VertexAttribute kAttrs[] = {
            {0, rhi::VertexFormat::Float2, offsetof(Vertex, x)},
            {1, rhi::VertexFormat::UNorm8x4, offsetof(Vertex, rgba)},
        };
        rhi::GraphicsPipelineDesc desc;
        desc.vertex_shader = {kVertexShader, "triangle.vert"};
        desc.fragment_shader = {kFragmentShader, "triangle.frag"};
        desc.vertex = {kAttrs, sizeof(Vertex)};
        desc.debug_name = "triangle";
        MCE_TRY_ASSIGN(pipeline_, device_.create_pipeline(desc));
        window_.set_event_handler(&TriangleApp::on_event, this);
        return ok();
    }

    void on_tick(const app::TickContext& t) override {
        prev_angle_ = angle_;
        angle_ += static_cast<f32>(t.dt) * kTurnsPerSecond * kTau;
    }

    void on_render(const app::RenderContext& rc) override {
        device_.begin_frame(rc.frame_index);

        const rhi::Extent2D bb = device_.backbuffer_size();
        const f32 aspect = bb.height ? static_cast<f32>(bb.width) / static_cast<f32>(bb.height) : 1.0f;
        const f32 angle = lerp(prev_angle_, angle_, rc.alpha);
        FrameU frame_u;
        frame_u.transform = scaling(vec3(1.0f / aspect, 1.0f, 1.0f)) * rotation(quat_from_axis_angle(vec3(0, 0, 1), angle));

        static constexpr Vertex kVerts[] = {
            {0.0f, 0.75f, 0xFF3030FFu},     // red
            {-0.65f, -0.5f, 0xFF30FF30u},   // green
            {0.65f, -0.5f, 0xFFFF3030u},    // blue
        };
        static constexpr u16 kIndices[] = {0, 1, 2};

        rhi::PassAttachment color;  // null texture = backbuffer
        color.clear.color[0] = 0.08f;
        color.clear.color[1] = 0.09f;
        color.clear.color[2] = 0.12f;
        rhi::RenderPassDesc pass;
        pass.colors = Span<const rhi::PassAttachment>(&color, 1);
        pass.debug_name = "main";

        rhi::CommandBuffer cmd(mem::frame());
        cmd.begin_pass(pass);
        cmd.bind_pipeline(pipeline_);
        cmd.set_uniforms(rhi::bank::kFrame, rhi::as_bytes(frame_u));
        cmd.set_vertex_data(rhi::as_bytes(Span<const Vertex>(kVerts)));
        cmd.set_index_data(rhi::as_bytes(Span<const u16>(kIndices)), rhi::IndexFormat::U16);
        cmd.draw(3);
        cmd.end_pass();
        device_.submit(cmd);

        const bool last = cv_frames.get_int() > 0 && rc.frame_index + 1 >= static_cast<u64>(cv_frames.get_int());
        if (last && !cv_capture.get_string().empty()) capture(cv_capture.get_string().data(), bb);
        device_.end_frame();
        if (last) quit(0);
    }

    void on_shutdown() override { device_.destroy_pipeline(pipeline_); }

    // app::IPlatformHooks
    bool pump_events() override { return platform::pump_events(); }
    void present(bool vsync) override { device_.present(vsync); }

private:
    static constexpr f32 kTurnsPerSecond = 0.25f;

    void capture(const char* path, rhi::Extent2D size) {
        Array<u8> px;
        px.resize(usize(size.width) * size.height * 4);
        if (!device_.read_pixels({}, rhi::Rect2D{0, 0, size.width, size.height}, px.span())) return;
        std::FILE* f = std::fopen(path, "wb");
        if (!f) {
            MCE_LOG_ERROR("01_triangle: cannot write '{}'", path);
            return;
        }
        std::fprintf(f, "P6\n%u %u\n255\n", size.width, size.height);
        for (usize i = 0; i < px.size(); i += 4) std::fwrite(&px[i], 1, 3, f);
        std::fclose(f);
        MCE_LOG_INFO("01_triangle: wrote {}", path);
    }

    static void on_event(const platform::Event& e, void* user) {
        auto* self = static_cast<TriangleApp*>(user);
        if (e.type == platform::EventType::KeyDown && e.code == static_cast<u32>(platform::Key::Escape)) self->quit(0);
    }

    platform::Window& window_;
    rhi::RenderDevice& device_;
    rhi::PipelineHandle pipeline_;
    f32 angle_ = 0.0f;
    f32 prev_angle_ = 0.0f;
};

int fail(const char* what, const Error& e) {
    FixedString<512> msg;
    format_error(e, msg);
    FixedString<640> text;
    text.append(what);
    text.append(": ");
    text.append(msg.view());
    platform::show_error_box("01_triangle", text.c_str());
    return 1;
}

}  // namespace

int main(int argc, char** argv) {
    log::LogDesc log_desc;
    log_desc.app_name = "01_triangle";
    log::init(log_desc);
    console::parse_command_line(argc, argv);

    int rc = 1;
    if (ResultV r = platform::init(); !r) {
        rc = fail("platform init failed", r.error());
    } else {
        platform::WindowDesc wd;
        wd.title = "MCE - 01_triangle";
        wd.width = 1280;
        wd.height = 720;
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
                TriangleApp app(*window.value(), *device.value());
                app::AppDesc desc;
                desc.name = "01_triangle";
                desc.platform = &app;
                desc.fences = device.value().get();
                app::RunStats stats;
                rc = app::run(app, desc, &stats);
                const u32 gl_errors = device.value()->validation_error_count();
                std::printf("01_triangle: %llu frames, %llu ticks, %u GL errors\n",
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
