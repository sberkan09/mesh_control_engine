// mce_rhi_gl against a real driver (CI: Xvfb + Mesa llvmpipe). Renders clear + triangle into the
// backbuffer and into an offscreen target, reads pixels back and checks them.

#include <doctest/doctest.h>

#include "mce/core.h"
#include "mce/platform/platform.h"
#include "mce/platform/window.h"
#include "mce/rhi/rhi.h"
#include "mce/rhi_gl/gl_device.h"

using namespace mce;

namespace {

constexpr const char* kVS = R"(#version 450 core
layout(std140, binding = 0) uniform FrameU { vec4 offset_scale; };
layout(location = 0) in vec2 a_pos;
layout(location = 1) in vec4 a_color;
layout(location = 0) out vec4 v_color;
out gl_PerVertex { vec4 gl_Position; };
void main() {
    v_color = a_color;
    gl_Position = vec4(a_pos * offset_scale.zw + offset_scale.xy, 0.0, 1.0);
}
)";

constexpr const char* kFS = R"(#version 450 core
layout(location = 0) in vec4 v_color;
layout(location = 0) out vec4 o_color;
void main() { o_color = v_color; }
)";

struct Vertex {
    f32 x, y;
    u32 rgba;
};

constexpr u32 kW = 256, kH = 256;

struct Fixture {
    Fixture() {
        REQUIRE(platform::init().has_value());
        platform::WindowDesc wd;
        wd.title = "test_rhi_gl";
        wd.width = kW;
        wd.height = kH;
        wd.resizable = false;
        wd.high_dpi = false;
        auto w = platform::Window::create(wd);
        REQUIRE(w.has_value());
        window = std::move(w).value();
        auto d = rhi::gl::create_device(*window);
        REQUIRE(d.has_value());
        device = std::move(d).value();
    }
    ~Fixture() {
        device.reset();
        window.reset();
        platform::shutdown();
    }

    rhi::PipelineHandle make_pipeline() {
        static constexpr rhi::VertexAttribute kAttrs[] = {
            {0, rhi::VertexFormat::Float2, offsetof(Vertex, x)},
            {1, rhi::VertexFormat::UNorm8x4, offsetof(Vertex, rgba)},
        };
        rhi::GraphicsPipelineDesc desc;
        desc.vertex_shader = {kVS, "test.vert"};
        desc.fragment_shader = {kFS, "test.frag"};
        desc.vertex = {kAttrs, sizeof(Vertex)};
        desc.debug_name = "test";
        auto p = device->create_pipeline(desc);
        REQUIRE(p.has_value());
        return p.value();
    }

    // Clear green, then a red triangle hugging the top-left: (-1,1) (-1,-1) (0,1) in NDC.
    void record_scene(rhi::CommandBuffer& cmd, rhi::TextureHandle target, rhi::PipelineHandle pipe) {
        rhi::PassAttachment color;
        color.texture = target;
        color.clear.color[0] = 0.0f;
        color.clear.color[1] = 1.0f;
        color.clear.color[2] = 0.0f;
        color.clear.color[3] = 1.0f;
        rhi::RenderPassDesc pass;
        pass.colors = Span<const rhi::PassAttachment>(&color, 1);
        pass.debug_name = "test";
        static constexpr Vertex kVerts[] = {
            {-1.0f, 1.0f, 0xFF0000FFu},
            {-1.0f, -1.0f, 0xFF0000FFu},
            {0.0f, 1.0f, 0xFF0000FFu},
        };
        static constexpr u16 kIdx[] = {0, 1, 2};
        static constexpr f32 kOffsetScale[4] = {0, 0, 1, 1};
        cmd.begin_pass(pass);
        cmd.bind_pipeline(pipe);
        cmd.set_uniforms(rhi::bank::kFrame, rhi::as_bytes(kOffsetScale));
        cmd.set_vertex_data(rhi::as_bytes(Span<const Vertex>(kVerts)));
        cmd.set_index_data(rhi::as_bytes(Span<const u16>(kIdx)), rhi::IndexFormat::U16);
        cmd.draw(3);
        cmd.end_pass();
    }

    mem::UniquePtr<platform::Window> window;
    mem::UniquePtr<rhi::RenderDevice> device;
};

struct Rgba {
    u8 r, g, b, a;
};

Rgba pixel(const Array<u8>& px, u32 w, u32 x, u32 y) {
    const usize i = (usize(y) * w + x) * 4;
    return {px[i], px[i + 1], px[i + 2], px[i + 3]};
}

void check_scene(const Array<u8>& px, u32 w, u32 h) {
    const Rgba left = pixel(px, w, 4, h / 2);
    CHECK(left.r == 255);
    CHECK(left.g == 0);
    const Rgba right = pixel(px, w, w - 4, h / 2);
    CHECK(right.r == 0);
    CHECK(right.g == 255);
    // Top-row-first readback: the triangle is wide at the top and narrow at the bottom.
    CHECK(pixel(px, w, w / 4, 2).r == 255);
    CHECK(pixel(px, w, w / 4, h - 3).g == 255);
    CHECK(pixel(px, w, w - 2, 1).g == 255);
}

}  // namespace

TEST_CASE("rhi_gl: device caps") {
    Fixture f;
    const rhi::DeviceCaps& caps = f.device->caps();
    CHECK(caps.api_major == 4);
    CHECK(caps.api_minor >= 5);
    CHECK(caps.max_texture_size >= 4096);
    MESSAGE(caps.api_name.c_str() << " / " << caps.device_name.c_str());
}

TEST_CASE("rhi_gl: clear + triangle to the backbuffer, read back") {
    Fixture f;
    const rhi::PipelineHandle pipe = f.make_pipeline();
    f.device->begin_frame(0);
    rhi::CommandBuffer cmd;
    f.record_scene(cmd, {}, pipe);
    f.device->submit(cmd);

    const rhi::Extent2D bb = f.device->backbuffer_size();
    REQUIRE(bb.width >= kW);
    Array<u8> px;
    px.resize(usize(bb.width) * bb.height * 4);
    REQUIRE(f.device->read_pixels({}, rhi::Rect2D{0, 0, bb.width, bb.height}, px.span()).has_value());
    f.device->end_frame();
    f.device->present(false);
    check_scene(px, bb.width, bb.height);

    CHECK(f.device->last_frame_stats().draws == 1);
    CHECK(f.device->last_frame_stats().passes == 1);
    CHECK(f.device->validation_error_count() == 0);
    f.device->destroy_pipeline(pipe);
    CHECK_FALSE(f.device->is_alive(pipe));
}

TEST_CASE("rhi_gl: offscreen color target + persistent buffers") {
    Fixture f;
    const rhi::PipelineHandle pipe = f.make_pipeline();
    rhi::TextureDesc td;
    td.size = {128, 64};
    td.format = rhi::TextureFormat::RGBA8_UNorm;
    td.usage = rhi::TextureUsage::SampledColorTarget;
    td.debug_name = "offscreen";
    const rhi::TextureHandle rt = f.device->create_texture(td);
    REQUIRE(f.device->is_alive(rt));
    CHECK(f.device->texture_size(rt) == rhi::Extent2D{128, 64});
    CHECK(f.device->texture_memory_bytes() == 128u * 64u * 4u);

    for (u64 frame = 0; frame < 5; ++frame) {  // wraps the frame-slot ring
        f.device->begin_frame(frame);
        rhi::CommandBuffer cmd;
        f.record_scene(cmd, rt, pipe);
        f.device->submit(cmd);
        f.device->end_frame();
        f.device->present(false);
    }
    Array<u8> px;
    px.resize(128 * 64 * 4);
    REQUIRE(f.device->read_pixels(rt, rhi::Rect2D{0, 0, 128, 64}, px.span()).has_value());
    check_scene(px, 128, 64);

    // Persistent vertex/index buffers instead of transient ring data.
    static constexpr Vertex kQuad[] = {
        {-1, -1, 0xFFFF0000u}, {1, -1, 0xFFFF0000u}, {1, 1, 0xFFFF0000u}, {-1, 1, 0xFFFF0000u}};
    static constexpr u32 kQuadIdx[] = {0, 1, 2, 0, 2, 3};
    const rhi::BufferHandle vb = f.device->create_buffer({rhi::BufferUsage::Vertex, sizeof(kQuad), "quad.vb"},
                                                         rhi::as_bytes(Span<const Vertex>(kQuad)));
    const rhi::BufferHandle ib = f.device->create_buffer({rhi::BufferUsage::Index, sizeof(kQuadIdx), "quad.ib"},
                                                         rhi::as_bytes(Span<const u32>(kQuadIdx)));
    f.device->begin_frame(5);
    rhi::CommandBuffer cmd;
    rhi::PassAttachment color;
    color.texture = rt;
    rhi::RenderPassDesc pass;
    pass.colors = Span<const rhi::PassAttachment>(&color, 1);
    static constexpr f32 kOffsetScale[4] = {0, 0, 1, 1};
    cmd.begin_pass(pass);
    cmd.bind_pipeline(pipe);
    cmd.set_uniforms(rhi::bank::kFrame, rhi::as_bytes(kOffsetScale));
    cmd.bind_vertex_buffer(vb);
    cmd.bind_index_buffer(ib, rhi::IndexFormat::U32);
    cmd.draw(6);
    cmd.end_pass();
    f.device->submit(cmd);
    REQUIRE(f.device->read_pixels(rt, rhi::Rect2D{60, 30, 2, 2}, Span<u8>(px.data(), 16)).has_value());
    f.device->end_frame();
    CHECK(px[0] == 0);
    CHECK(px[2] == 255);  // blue quad fills the target

    f.device->destroy_buffer(vb);
    f.device->destroy_buffer(ib);
    f.device->destroy_texture(rt);
    CHECK(f.device->texture_memory_bytes() == 0);
    CHECK_FALSE(f.device->is_alive(rt));
    CHECK(f.device->validation_error_count() == 0);
}

TEST_CASE("rhi_gl: shader compile errors are Results, not crashes") {
    Fixture f;
    rhi::GraphicsPipelineDesc desc;
    desc.vertex_shader = {"#version 450 core\nvoid main() { this is not glsl }\n", "broken.vert"};
    desc.fragment_shader = {kFS, "test.frag"};
    auto p = f.device->create_pipeline(desc);
    REQUIRE_FALSE(p.has_value());
    CHECK(p.error().code == Errc::ShaderCompile);
    CHECK(p.error().path == StringView("broken.vert"));
}

TEST_CASE("rhi_gl: frame fences gate core frame slots") {
    Fixture f;
    for (u64 frame = 0; frame < 2 * MCE_FRAMES_IN_FLIGHT; ++frame) {
        f.device->begin_frame(frame);
        f.device->end_frame();
        mem::FrameSlotFence* fence = f.device->fence_for_frame(frame);
        REQUIRE(fence != nullptr);
        fence->wait_blocking();
        CHECK(fence->is_signaled());
    }
}
