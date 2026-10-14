// mce_rhi_metal on the Mac's GPU (local_ci_macos.sh; needs a logged-in window server). The same
// scenarios as test_rhi_gl, plus the places where Metal and GL conventions differ and the backend
// has to hide it: texture-target orientation and sampling, winding under the target Y flip, the
// default depth buffer, MSAA resolve, storage buffers with gl_BaseInstanceARB through draw_indirect.

#include <doctest/doctest.h>

#include "mce/core.h"
#include "mce/platform/platform.h"
#include "mce/platform/window.h"
#include "mce/rhi/rhi.h"
#include "mce/rhi_metal/metal_device.h"

using namespace mce;

#define MCE_REQUIRE_OR_RETURN(...)                  \
    do {                                            \
        const bool mce_req_ok_ = (__VA_ARGS__);     \
        REQUIRE_MESSAGE(mce_req_ok_, #__VA_ARGS__); \
        if (!mce_req_ok_) return;                   \
    } while (0)

namespace {

constexpr const char* kVS = R"(#version 450 core
layout(std140, binding = 0) uniform FrameU { vec4 offset_scale; };
layout(location = 0) in vec2 a_pos;
layout(location = 1) in vec4 a_color;
layout(location = 0) out vec4 v_color;
out gl_PerVertex { vec4 gl_Position; };
void main() {
    v_color = a_color;
    gl_Position = vec4(a_pos * offset_scale.zw + offset_scale.xy, 0.5, 1.0);
}
)";

constexpr const char* kFS = R"(#version 450 core
layout(location = 0) in vec4 v_color;
layout(location = 0) out vec4 o_color;
void main() { o_color = v_color; }
)";

// Full-target quad from gl_VertexID (no vertex input), sampling binding 6 with GL's uv convention:
// uv (0, 0) at NDC (-1, -1).
constexpr const char* kBlitVS = R"(#version 450 core
layout(location = 0) out vec2 v_uv;
out gl_PerVertex { vec4 gl_Position; };
void main() {
    vec2 p = vec2((gl_VertexID & 1) != 0 ? 1.0 : -1.0, (gl_VertexID & 2) != 0 ? 1.0 : -1.0);
    v_uv = p * 0.5 + 0.5;
    gl_Position = vec4(p, 0.0, 1.0);
}
)";
constexpr const char* kBlitFS = R"(#version 450 core
layout(binding = 6) uniform sampler2D u_tex;
layout(location = 0) in vec2 v_uv;
layout(location = 0) out vec4 o_color;
void main() { o_color = texture(u_tex, v_uv); }
)";

// Per-instance color from a storage buffer indexed by gl_BaseInstanceARB + gl_InstanceID: each
// indirect record draws the quad in its own column.
constexpr const char* kInstVS = R"(#version 450 core
#extension GL_ARB_shader_draw_parameters : require
layout(std430, binding = 3) readonly buffer Objects { vec4 colors[]; };
layout(location = 0) in vec2 a_pos;
layout(location = 0) out vec4 v_color;
out gl_PerVertex { vec4 gl_Position; };
void main() {
    int id = gl_BaseInstanceARB + gl_InstanceID;
    v_color = colors[id];
    float x0 = -1.0 + 0.5 * float(id);
    gl_Position = vec4(x0 + (a_pos.x * 0.5 + 0.5) * 0.5, a_pos.y, 0.0, 1.0);
}
)";

struct Vertex {
    f32 x, y;
    u32 rgba;
};

constexpr u32 kW = 256, kH = 256;

struct Rgba {
    u8 r, g, b, a;
};

Rgba pixel(const Array<u8>& px, u32 w, u32 x, u32 y) {
    const usize i = (usize(y) * w + x) * 4;
    return {px[i], px[i + 1], px[i + 2], px[i + 3]};
}

struct Fixture {
    Fixture() {
        MCE_REQUIRE_OR_RETURN(platform::init().has_value());
        platform::WindowDesc wd;
        wd.title = "test_rhi_metal";
        wd.width = kW;
        wd.height = kH;
        wd.resizable = false;
        wd.high_dpi = false;
        auto w = platform::Window::create(wd);
        MCE_REQUIRE_OR_RETURN(w.has_value());
        window = std::move(w).value();
        auto d = rhi::metal::create_device(*window);
        MCE_REQUIRE_OR_RETURN(d.has_value());
        device = std::move(d).value();
        ok = true;
    }
    ~Fixture() {
        device.reset();
        window.reset();
        platform::shutdown();
    }

    rhi::PipelineHandle make_pipeline(rhi::FixedState state = {}) {
        static constexpr rhi::VertexAttribute kAttrs[] = {
            {0, rhi::VertexFormat::Float2, offsetof(Vertex, x)},
            {1, rhi::VertexFormat::UNorm8x4, offsetof(Vertex, rgba)},
        };
        rhi::GraphicsPipelineDesc desc;
        desc.vertex_shader = {kVS, "test.vert"};
        desc.fragment_shader = {kFS, "test.frag"};
        desc.vertex = {kAttrs, sizeof(Vertex)};
        desc.state = state;
        desc.debug_name = "test";
        auto p = device->create_pipeline(desc);
        REQUIRE(p.has_value());
        return p.has_value() ? p.value() : rhi::PipelineHandle{};
    }

    rhi::TextureHandle make_target(u32 w, u32 h, u8 samples = 1) {
        rhi::TextureDesc td;
        td.size = {w, h};
        td.format = rhi::TextureFormat::RGBA8_UNorm;
        td.usage = rhi::TextureUsage::SampledColorTarget;
        td.samples = samples;
        td.debug_name = "target";
        return device->create_texture(td);
    }

    // Clear green, then a red triangle hugging the top-left: (-1,1) (-1,-1) (0,1) in NDC (CCW).
    void record_scene(rhi::CommandBuffer& cmd, rhi::TextureHandle target, rhi::PipelineHandle pipe, bool ccw = false) {
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
        static constexpr u16 kCcw[] = {0, 1, 2};
        static constexpr u16 kCw[] = {0, 2, 1};
        static constexpr f32 kOffsetScale[4] = {0, 0, 1, 1};
        cmd.begin_pass(pass);
        cmd.bind_pipeline(pipe);
        cmd.set_uniforms(rhi::bank::kFrame, rhi::as_bytes(kOffsetScale));
        cmd.set_vertex_data(rhi::as_bytes(Span<const Vertex>(kVerts)));
        cmd.set_index_data(rhi::as_bytes(Span<const u16>(ccw ? kCcw : kCw)), rhi::IndexFormat::U16);
        cmd.draw(3);
        cmd.end_pass();
    }

    Array<u8> read(rhi::TextureHandle t, u32 w, u32 h) {
        Array<u8> px;
        px.resize(usize(w) * h * 4);
        REQUIRE(device->read_pixels(t, rhi::Rect2D{0, 0, w, h}, px.span()).has_value());
        return px;
    }

    mem::UniquePtr<platform::Window> window;
    mem::UniquePtr<rhi::RenderDevice> device;
    bool ok = false;
};

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

TEST_CASE("rhi_metal: device caps") {
    Fixture f;
    if (!f.ok) return;
    const rhi::DeviceCaps& caps = f.device->caps();
    CHECK(caps.max_texture_size >= 4096);
    CHECK(caps.shader_draw_parameters);
    MESSAGE(caps.api_name.c_str() << " / " << caps.device_name.c_str());
}

TEST_CASE("rhi_metal: clear + triangle to the backbuffer, read back") {
    Fixture f;
    if (!f.ok) return;
    const rhi::PipelineHandle pipe = f.make_pipeline();
    if (pipe.is_null()) return;
    f.device->begin_frame(0);
    rhi::CommandBuffer cmd;
    f.record_scene(cmd, {}, pipe);
    f.device->submit(cmd);
    const rhi::Extent2D bb = f.device->backbuffer_size();
    MCE_REQUIRE_OR_RETURN(bb.width >= kW && bb.height >= kH);
    const Array<u8> px = f.read({}, bb.width, bb.height);
    f.device->end_frame();
    f.device->present(false);
    check_scene(px, bb.width, bb.height);
    CHECK(f.device->last_frame_stats().draws == 1);
    CHECK(f.device->last_frame_stats().passes == 1);
    CHECK(f.device->validation_error_count() == 0);
    f.device->destroy_pipeline(pipe);
    CHECK_FALSE(f.device->is_alive(pipe));
}

TEST_CASE("rhi_metal: offscreen color target + persistent buffers") {
    Fixture f;
    if (!f.ok) return;
    const rhi::PipelineHandle pipe = f.make_pipeline();
    if (pipe.is_null()) return;
    const rhi::TextureHandle rt = f.make_target(128, 64);
    MCE_REQUIRE_OR_RETURN(f.device->is_alive(rt));
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
    check_scene(f.read(rt, 128, 64), 128, 64);

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
    Array<u8> px;
    px.resize(16);
    MCE_REQUIRE_OR_RETURN(f.device->read_pixels(rt, rhi::Rect2D{60, 30, 2, 2}, px.span()).has_value());
    f.device->end_frame();
    CHECK(px[0] == 0);
    CHECK(px[2] == 255);  // blue quad fills the target

    // update_buffer is ordered with the GPU work like glNamedBufferSubData: recolor and redraw.
    static constexpr Vertex kGreen[] = {
        {-1, -1, 0xFF00FF00u}, {1, -1, 0xFF00FF00u}, {1, 1, 0xFF00FF00u}, {-1, 1, 0xFF00FF00u}};
    f.device->update_buffer(vb, 0, rhi::as_bytes(Span<const Vertex>(kGreen)));
    f.device->begin_frame(6);
    f.device->submit(cmd);
    MCE_REQUIRE_OR_RETURN(f.device->read_pixels(rt, rhi::Rect2D{60, 30, 2, 2}, px.span()).has_value());
    f.device->end_frame();
    CHECK(px[1] == 255);
    CHECK(px[2] == 0);

    f.device->destroy_buffer(vb);
    f.device->destroy_buffer(ib);
    f.device->destroy_texture(rt);
    CHECK(f.device->texture_memory_bytes() == 0);
    CHECK_FALSE(f.device->is_alive(rt));
    CHECK(f.device->validation_error_count() == 0);
}

TEST_CASE("rhi_metal: a rendered texture samples upright with GL uv conventions") {
    Fixture f;
    if (!f.ok) return;
    const rhi::PipelineHandle pipe = f.make_pipeline();
    rhi::GraphicsPipelineDesc bd;
    bd.vertex_shader = {kBlitVS, "blit.vert"};
    bd.fragment_shader = {kBlitFS, "blit.frag"};
    bd.state.topology = rhi::PrimitiveTopology::Triangles;
    auto blit = f.device->create_pipeline(bd);
    MCE_REQUIRE_OR_RETURN(blit.has_value() && !pipe.is_null());
    const rhi::TextureHandle rt = f.make_target(64, 64);
    rhi::SamplerDesc sd;
    sd.min_filter = sd.mag_filter = rhi::Filter::Nearest;
    const rhi::SamplerHandle smp = f.device->create_sampler(sd);

    f.device->begin_frame(0);
    rhi::CommandBuffer cmd;
    f.record_scene(cmd, rt, pipe);
    rhi::PassAttachment color;  // backbuffer
    rhi::RenderPassDesc pass;
    pass.colors = Span<const rhi::PassAttachment>(&color, 1);
    static constexpr u16 kStrip[] = {0, 1, 2, 2, 1, 3};
    cmd.begin_pass(pass);
    cmd.bind_pipeline(blit.value());
    cmd.bind_texture(rhi::bank::kFirstSampler, rt, smp);
    cmd.set_index_data(rhi::as_bytes(Span<const u16>(kStrip)), rhi::IndexFormat::U16);
    cmd.draw(6);
    cmd.end_pass();
    f.device->submit(cmd);
    const rhi::Extent2D bb = f.device->backbuffer_size();
    const Array<u8> px = f.read({}, bb.width, bb.height);
    f.device->end_frame();
    f.device->present(false);
    check_scene(px, bb.width, bb.height);  // the target's image, not mirrored
    CHECK(f.device->validation_error_count() == 0);
}

TEST_CASE("rhi_metal: back-face culling agrees between the backbuffer and texture targets") {
    Fixture f;
    if (!f.ok) return;
    rhi::FixedState st;
    st.raster.cull = rhi::CullMode::Back;
    st.raster.front_ccw = true;
    const rhi::PipelineHandle pipe = f.make_pipeline(st);
    MCE_REQUIRE_OR_RETURN(!pipe.is_null());
    const rhi::TextureHandle rt = f.make_target(64, 64);
    for (const bool ccw : {true, false}) {
        f.device->begin_frame(ccw ? 0 : 1);
        rhi::CommandBuffer cmd;
        f.record_scene(cmd, {}, pipe, ccw);
        f.record_scene(cmd, rt, pipe, ccw);
        f.device->submit(cmd);
        const rhi::Extent2D bb = f.device->backbuffer_size();
        const Array<u8> back = f.read({}, bb.width, bb.height);
        const Array<u8> tex = f.read(rt, 64, 64);
        f.device->end_frame();
        f.device->present(false);
        // CCW (GL front) survives back-face culling; CW is culled. Same on both kinds of target.
        CHECK(pixel(back, bb.width, 4, bb.height / 2).r == (ccw ? 255 : 0));
        CHECK(pixel(tex, 64, 2, 32).r == (ccw ? 255 : 0));
    }
}

TEST_CASE("rhi_metal: depth test against a depth target and the default depth buffer") {
    Fixture f;
    if (!f.ok) return;
    rhi::FixedState st;
    st.depth.test = true;
    st.depth.write = true;
    st.depth.compare = rhi::CompareOp::Less;
    const rhi::PipelineHandle pipe = f.make_pipeline(st);
    MCE_REQUIRE_OR_RETURN(!pipe.is_null());
    const rhi::TextureHandle rt = f.make_target(64, 64);
    rhi::TextureDesc dd;
    dd.size = {64, 64};
    dd.format = rhi::TextureFormat::D32_Float;
    dd.usage = rhi::TextureUsage::DepthTarget;
    const rhi::TextureHandle depth = f.device->create_texture(dd);

    // The scene's triangle is at z = 0.5 (NDC). Cleared to 0.25 it fails Less; cleared to 1 it passes.
    for (const f32 clear : {0.25f, 1.0f}) {
        for (const bool backbuffer : {false, true}) {
            f.device->begin_frame(0);
            rhi::CommandBuffer cmd;
            rhi::PassAttachment color;
            color.texture = backbuffer ? rhi::TextureHandle{} : rt;
            color.clear.color[1] = 1.0f;
            rhi::PassAttachment z;
            z.texture = backbuffer ? rhi::TextureHandle{} : depth;
            z.clear.depth = clear;
            rhi::RenderPassDesc pass;
            pass.colors = Span<const rhi::PassAttachment>(&color, 1);
            pass.depth = z;
            static constexpr Vertex kVerts[] = {{-1, 1, 0xFF0000FFu}, {-1, -1, 0xFF0000FFu}, {0, 1, 0xFF0000FFu}};
            static constexpr u16 kIdx[] = {0, 1, 2};
            static constexpr f32 kOffsetScale[4] = {0, 0, 1, 1};
            cmd.begin_pass(pass);
            cmd.bind_pipeline(pipe);
            cmd.set_uniforms(rhi::bank::kFrame, rhi::as_bytes(kOffsetScale));
            cmd.set_vertex_data(rhi::as_bytes(Span<const Vertex>(kVerts)));
            cmd.set_index_data(rhi::as_bytes(Span<const u16>(kIdx)), rhi::IndexFormat::U16);
            cmd.draw(3);
            cmd.end_pass();
            f.device->submit(cmd);
            const rhi::Extent2D size = backbuffer ? f.device->backbuffer_size() : rhi::Extent2D{64, 64};
            const Array<u8> px = f.read(backbuffer ? rhi::TextureHandle{} : rt, size.width, size.height);
            f.device->end_frame();
            f.device->present(false);
            CHECK(pixel(px, size.width, 2, size.height / 2).r == (clear > 0.5f ? 255 : 0));
        }
    }
    CHECK(f.device->validation_error_count() == 0);
}

TEST_CASE("rhi_metal: 4x MSAA target resolves into a single-sample one") {
    Fixture f;
    if (!f.ok) return;
    const rhi::PipelineHandle pipe = f.make_pipeline();
    MCE_REQUIRE_OR_RETURN(!pipe.is_null());
    const rhi::TextureHandle msaa = f.make_target(64, 64, 4);
    const rhi::TextureHandle resolved = f.make_target(64, 64);
    f.device->begin_frame(0);
    rhi::CommandBuffer cmd;
    f.record_scene(cmd, msaa, pipe);
    cmd.resolve(msaa, resolved);
    f.device->submit(cmd);
    const Array<u8> px = f.read(resolved, 64, 64);
    f.device->end_frame();
    check_scene(px, 64, 64);
    // An edge pixel of the diagonal is a blend of red and green.
    bool blended = false;
    for (u32 x = 0; x < 64 && !blended; ++x) {
        const Rgba p = pixel(px, 64, x, 32);
        blended = p.r > 10 && p.r < 245 && p.g > 10 && p.g < 245;
    }
    CHECK(blended);
    CHECK(f.device->validation_error_count() == 0);
}

TEST_CASE("rhi_metal: storage buffer + gl_BaseInstanceARB through draw_indirect") {
    Fixture f;
    if (!f.ok) return;
    static constexpr rhi::VertexAttribute kAttrs[] = {{0, rhi::VertexFormat::Float2, 0}};
    rhi::GraphicsPipelineDesc desc;
    desc.vertex_shader = {kInstVS, "inst.vert"};
    desc.fragment_shader = {kFS, "test.frag"};
    desc.vertex = {kAttrs, 8};
    auto pipe = f.device->create_pipeline(desc);
    MCE_REQUIRE_OR_RETURN(pipe.has_value());
    static constexpr f32 kColors[4][4] = {{1, 0, 0, 1}, {0, 1, 0, 1}, {0, 0, 1, 1}, {1, 1, 0, 1}};
    const rhi::BufferHandle objects =
        f.device->create_buffer({rhi::BufferUsage::Storage, sizeof(kColors), "objects"}, rhi::as_bytes(kColors));
    const rhi::TextureHandle rt = f.make_target(64, 16);

    static constexpr f32 kQuad[4][2] = {{-1, -1}, {1, -1}, {1, 1}, {-1, 1}};
    static constexpr u16 kIdx[] = {0, 1, 2, 0, 2, 3};
    // Two records: instances 0..1, then instances 2..3 (base_instance 2).
    const rhi::DrawIndexedIndirectCommand args[] = {{6, 2, 0, 0, 0}, {6, 2, 0, 0, 2}};
    f.device->begin_frame(0);
    rhi::CommandBuffer cmd;
    rhi::PassAttachment color;
    color.texture = rt;
    rhi::RenderPassDesc pass;
    pass.colors = Span<const rhi::PassAttachment>(&color, 1);
    cmd.begin_pass(pass);
    cmd.bind_pipeline(pipe.value());
    cmd.bind_storage_buffer(rhi::bank::kObjects, objects, 0, sizeof(kColors));
    cmd.set_vertex_data(rhi::as_bytes(kQuad));
    cmd.set_index_data(rhi::as_bytes(Span<const u16>(kIdx)), rhi::IndexFormat::U16);
    cmd.set_draw_args(Span<const rhi::DrawIndexedIndirectCommand>(args));
    cmd.draw_indirect(2);
    cmd.end_pass();
    f.device->submit(cmd);
    const Array<u8> px = f.read(rt, 64, 16);
    f.device->end_frame();
    const Rgba c0 = pixel(px, 64, 8, 8), c1 = pixel(px, 64, 24, 8), c2 = pixel(px, 64, 40, 8), c3 = pixel(px, 64, 56, 8);
    CHECK((c0.r == 255 && c0.g == 0));
    CHECK((c1.g == 255 && c1.r == 0));
    CHECK((c2.b == 255 && c2.r == 0));
    CHECK((c3.r == 255 && c3.g == 255));
    CHECK(f.device->last_frame_stats().indirect_draws == 1);
    CHECK(f.device->validation_error_count() == 0);
}

TEST_CASE("rhi_metal: shader compile errors are Results, not crashes") {
    Fixture f;
    if (!f.ok) return;
    rhi::GraphicsPipelineDesc desc;
    desc.vertex_shader = {"#version 450 core\nvoid main() { this is not glsl }\n", "broken.vert"};
    desc.fragment_shader = {kFS, "test.frag"};
    auto p = f.device->create_pipeline(desc);
    MCE_REQUIRE_OR_RETURN(!p.has_value());
    CHECK(p.error().code == Errc::ShaderCompile);
    CHECK(p.error().path == StringView("broken.vert"));
}

TEST_CASE("rhi_metal: frame fences gate core frame slots") {
    Fixture f;
    if (!f.ok) return;
    for (u64 frame = 0; frame < 2 * MCE_FRAMES_IN_FLIGHT; ++frame) {
        f.device->begin_frame(frame);
        f.device->end_frame();
        mem::FrameSlotFence* fence = f.device->fence_for_frame(frame);
        MCE_REQUIRE_OR_RETURN(fence != nullptr);
        fence->wait_blocking();
        CHECK(fence->is_signaled());
    }
}

TEST_CASE("rhi_metal: GPU timestamps bracket passes, MCE_FRAMES_IN_FLIGHT frames later") {
    Fixture f;
    if (!f.ok) return;
    CHECK(f.device->caps().timestamp_queries);  // Apple silicon samples at stage boundaries
    if (!f.device->caps().timestamp_queries) return;
    const rhi::PipelineHandle pipe = f.make_pipeline();
    const rhi::TextureHandle target = f.make_target(512, 512);
    if (pipe.is_null()) return;
    CHECK(f.device->last_gpu_timestamps().frame_index == ~u64(0));
    for (u64 frame = 0; frame < 2 * MCE_FRAMES_IN_FLIGHT; ++frame) {
        f.device->begin_frame(frame);
        rhi::CommandBuffer cmd;
        cmd.write_timestamp(0);
        f.record_scene(cmd, target, pipe);
        cmd.write_timestamp(1);
        cmd.write_timestamp(2);  // nothing between 1 and 2: an empty stage
        f.record_scene(cmd, target, pipe);
        f.record_scene(cmd, {}, pipe);
        cmd.write_timestamp(3);
        f.device->submit(cmd);
        f.device->end_frame();
        f.device->present(false);
    }
    const rhi::GpuTimestamps& ts = f.device->last_gpu_timestamps();
    CHECK(ts.frame_index == MCE_FRAMES_IN_FLIGHT - 1);  // begin_frame(2F - 1) collected frame F - 1
    CHECK(ts.has(0));
    CHECK(ts.has(1));
    CHECK(ts.has(2));
    CHECK(ts.has(3));
    CHECK_FALSE(ts.has(4));
    CHECK(ts.ms(0, 4) == -1.0);
    CHECK(ts.ms(1, 2) == 0.0);
    CHECK(ts.ms(0, 1) >= 0.0);
    CHECK(ts.ms(2, 3) >= 0.0);
    CHECK(ts.ms(0, 3) > 0.0);
    CHECK(ts.ms(0, 3) < 1000.0);
    MESSAGE("pass A " << ts.ms(0, 1) << " ms, passes B+C " << ts.ms(2, 3) << " ms");
    CHECK(f.device->validation_error_count() == 0);
    f.device->destroy_texture(target);
    f.device->destroy_pipeline(pipe);
}

TEST_CASE("rhi_metal: R11G11B10F targets read back as RGBA8 like GL (clamped, alpha 1)") {
    Fixture f;
    if (!f.ok) return;
    rhi::TextureDesc td;
    td.size = {8, 8};
    td.format = rhi::TextureFormat::R11G11B10F;
    td.usage = rhi::TextureUsage::SampledColorTarget;
    td.debug_name = "hdr";
    const rhi::TextureHandle t = f.device->create_texture(td);
    MCE_REQUIRE_OR_RETURN(!t.is_null());
    rhi::PassAttachment color;
    color.texture = t;
    color.clear.color[0] = 0.5f;   // exact in 6 mantissa bits
    color.clear.color[1] = 4.0f;   // HDR: clamps to 255
    color.clear.color[2] = 0.25f;
    color.clear.color[3] = 0.0f;   // no alpha channel: reads as 1
    rhi::RenderPassDesc pass;
    pass.colors = Span<const rhi::PassAttachment>(&color, 1);
    f.device->begin_frame(0);
    rhi::CommandBuffer cmd;
    cmd.begin_pass(pass);
    cmd.end_pass();
    f.device->submit(cmd);
    const Array<u8> px = f.read(t, 8, 8);
    f.device->end_frame();
    const Rgba c = pixel(px, 8, 3, 3);
    CHECK(c.r == 128);
    CHECK(c.g == 255);
    CHECK(c.b == 64);
    CHECK(c.a == 255);
    // Depth and multisampled targets stay unreadable, as on GL.
    rhi::TextureDesc dd;
    dd.size = {8, 8};
    dd.format = rhi::TextureFormat::D32_Float;
    dd.usage = rhi::TextureUsage::DepthTarget;
    const rhi::TextureHandle d = f.device->create_texture(dd);
    Array<u8> out;
    out.resize(8 * 8 * 4);
    const auto r = f.device->read_pixels(d, rhi::Rect2D{0, 0, 8, 8}, out.span());
    CHECK_FALSE(r.has_value());
    CHECK(f.device->validation_error_count() == 0);
    f.device->destroy_texture(d);
    f.device->destroy_texture(t);
}

// ---- compute (M-VISUAL 1): the rhi_gl checks, on Metal -------------------------------------------

namespace {

constexpr const char* kFillCS = R"(#version 450 core
layout(local_size_x = 64) in;
layout(std430, binding = 4) buffer Out { uint v[]; };
void main() {
    uint i = gl_GlobalInvocationID.x;
    v[i] = i * 3u + 1u;
}
)";

// Writes the dispatch arguments for the next pass: 3 groups.
constexpr const char* kArgsCS = R"(#version 450 core
layout(local_size_x = 1) in;
layout(std430, binding = 5) buffer Args { uint groups[3]; };
void main() { groups[0] = 3u; groups[1] = 1u; groups[2] = 1u; }
)";

constexpr const char* kMarkCS = R"(#version 450 core
layout(local_size_x = 1) in;
layout(std430, binding = 4) buffer Out { uint v[]; };
void main() { v[gl_WorkGroupID.x] = 7u; }
)";

constexpr const char* kImageWriteCS = R"(#version 450 core
layout(local_size_x = 8, local_size_y = 8) in;
layout(rgba8, binding = 0) uniform writeonly image2D u_out;
void main() {
    ivec2 p = ivec2(gl_GlobalInvocationID.xy);
    imageStore(u_out, p, vec4(float(p.x) / 255.0, p.y == 0 ? 1.0 : 0.0, 0.5, 1.0));
}
)";

// Reads the image back through imageLoad (the storage-image barrier path) into an SSBO.
constexpr const char* kImageReadCS = R"(#version 450 core
layout(local_size_x = 8, local_size_y = 8) in;
layout(rgba8, binding = 1) uniform readonly image2D u_in;
layout(std430, binding = 4) buffer Out { uint v[]; };
void main() {
    ivec2 p = ivec2(gl_GlobalInvocationID.xy);
    v[p.y * 64 + p.x] = uint(imageLoad(u_in, p).r * 255.0 + 0.5);
}
)";

rhi::PipelineHandle make_compute(rhi::RenderDevice& device, const char* src, const char* name) {
    rhi::ComputePipelineDesc desc;
    desc.compute_shader = {src, name};
    desc.debug_name = name;
    auto p = device.create_compute_pipeline(desc);
    REQUIRE(p.has_value());
    return p.has_value() ? p.value() : rhi::PipelineHandle{};
}

}  // namespace

TEST_CASE("rhi_metal: compute caps") {
    Fixture f;
    if (!f.ok) return;
    const rhi::DeviceCaps& caps = f.device->caps();
    CHECK(caps.compute);
    CHECK(caps.max_compute_invocations >= 1024);  // the GL 4.3 minimums hold on every Apple GPU
    CHECK(caps.max_compute_group_size[0] >= 1024);
    CHECK(caps.max_compute_group_size[2] >= 64);
    CHECK(caps.max_compute_group_count[0] >= 65535);
    CHECK(caps.max_storage_image_units >= 8);
}

TEST_CASE("rhi_metal: compute dispatch writes a storage buffer") {
    Fixture f;
    if (!f.ok) return;
    const rhi::PipelineHandle fill = make_compute(*f.device, kFillCS, "fill.comp");
    if (fill.is_null()) return;
    const rhi::BufferHandle out = f.device->create_buffer({rhi::BufferUsage::Storage, 256 * sizeof(u32), "fill.out"});

    f.device->begin_frame(0);
    rhi::CommandBuffer cmd;
    cmd.bind_pipeline(fill);
    cmd.bind_storage_buffer(rhi::bank::kClusters, out, 0, 0);
    cmd.dispatch(4);
    cmd.barrier(rhi::barrier::kStorageBuffer);
    f.device->submit(cmd);
    f.device->end_frame();
    CHECK(f.device->last_frame_stats().dispatches == 1);

    u32 v[256] = {};
    MCE_REQUIRE_OR_RETURN(f.device->read_buffer(out, 0, std::as_writable_bytes(std::span(v))).has_value());
    bool all = true;
    for (u32 i = 0; i < 256; ++i) all = all && v[i] == i * 3 + 1;
    CHECK(all);
    CHECK_FALSE(f.device->read_buffer(out, 1020, std::as_writable_bytes(std::span(v, 2))).has_value());
    CHECK(f.device->validation_error_count() == 0);
    f.device->destroy_pipeline(fill);
    CHECK_FALSE(f.device->is_alive(fill));
}

TEST_CASE("rhi_metal: dispatch_indirect reads arguments a dispatch wrote") {
    Fixture f;
    if (!f.ok) return;
    const rhi::PipelineHandle args_pipe = make_compute(*f.device, kArgsCS, "args.comp");
    const rhi::PipelineHandle mark = make_compute(*f.device, kMarkCS, "mark.comp");
    if (args_pipe.is_null() || mark.is_null()) return;
    const rhi::BufferHandle args =
        f.device->create_buffer({rhi::BufferUsage::IndirectArgs, sizeof(rhi::DispatchIndirectCommand), "args"});
    const u32 zeros[8] = {};
    const rhi::BufferHandle out =
        f.device->create_buffer({rhi::BufferUsage::Storage, sizeof(zeros), "mark.out"}, rhi::as_bytes(zeros));

    f.device->begin_frame(0);
    rhi::CommandBuffer cmd;
    cmd.bind_pipeline(args_pipe);
    cmd.bind_storage_buffer(rhi::bank::kLightPool, args, 0, 0);
    cmd.dispatch(1);
    cmd.barrier(rhi::barrier::kIndirect);
    cmd.bind_pipeline(mark);
    cmd.bind_storage_buffer(rhi::bank::kClusters, out, 0, 0);
    cmd.dispatch_indirect(args);
    cmd.barrier(rhi::barrier::kStorageBuffer);
    f.device->submit(cmd);
    f.device->end_frame();
    CHECK(f.device->last_frame_stats().dispatches == 2);

    u32 v[8] = {};
    MCE_REQUIRE_OR_RETURN(f.device->read_buffer(out, 0, std::as_writable_bytes(std::span(v))).has_value());
    CHECK(v[0] == 7);
    CHECK(v[2] == 7);
    CHECK(v[3] == 0);  // three groups, not four
    CHECK(f.device->validation_error_count() == 0);
}

TEST_CASE("rhi_metal: storage image written by compute, read by readback and imageLoad") {
    Fixture f;
    if (!f.ok) return;
    const rhi::PipelineHandle write = make_compute(*f.device, kImageWriteCS, "image_write.comp");
    const rhi::PipelineHandle read = make_compute(*f.device, kImageReadCS, "image_read.comp");
    if (write.is_null() || read.is_null()) return;
    rhi::TextureDesc td;
    td.size = {64, 32};
    td.format = rhi::TextureFormat::RGBA8_UNorm;
    td.usage = rhi::TextureUsage::SampledStorage;
    td.debug_name = "storage";
    const rhi::TextureHandle img = f.device->create_texture(td);
    MCE_REQUIRE_OR_RETURN(f.device->is_alive(img));
    const rhi::BufferHandle out = f.device->create_buffer({rhi::BufferUsage::Storage, 64 * 32 * sizeof(u32), "read.out"});

    f.device->begin_frame(0);
    rhi::CommandBuffer cmd;
    cmd.bind_pipeline(write);
    cmd.bind_storage_image(0, img, 0, rhi::StorageAccess::Write);
    cmd.dispatch(8, 4);
    cmd.barrier(rhi::barrier::kStorageImage | rhi::barrier::kSampled);
    cmd.bind_pipeline(read);
    cmd.bind_storage_image(1, img, 0, rhi::StorageAccess::Read);
    cmd.bind_storage_buffer(rhi::bank::kClusters, out, 0, 0);
    cmd.dispatch(8, 4);
    cmd.barrier(rhi::barrier::kStorageBuffer);
    f.device->submit(cmd);

    Array<u8> px;
    px.resize(64 * 32 * 4);
    MCE_REQUIRE_OR_RETURN(f.device->read_pixels(img, rhi::Rect2D{0, 0, 64, 32}, px.span()).has_value());
    f.device->end_frame();
    // Image row 0 is the bottom row of a top-row-first readback (the render-target convention).
    CHECK(pixel(px, 64, 10, 31).r == 10);
    CHECK(pixel(px, 64, 10, 31).g == 255);
    CHECK(pixel(px, 64, 40, 0).r == 40);
    CHECK(pixel(px, 64, 40, 0).g == 0);
    CHECK(pixel(px, 64, 40, 0).b == 128);

    Array<u32> v;
    v.resize(64 * 32);
    MCE_REQUIRE_OR_RETURN(f.device->read_buffer(out, 0, std::as_writable_bytes(v.span())).has_value());
    CHECK(v[5 * 64 + 33] == 33);
    CHECK(v[31 * 64 + 63] == 63);
    CHECK(f.device->validation_error_count() == 0);
}

TEST_CASE("rhi_metal: compute compile errors are Results") {
    Fixture f;
    if (!f.ok) return;
    rhi::ComputePipelineDesc desc;
    desc.compute_shader = {"#version 450 core\nlayout(local_size_x = 1) in;\nvoid main() { oops; }\n", "broken.comp"};
    auto p = f.device->create_compute_pipeline(desc);
    MCE_REQUIRE_OR_RETURN(!p.has_value());
    CHECK(p.error().code == Errc::ShaderCompile);
}

// A storage image bound at mip 1 goes through a texture view: the write lands in that level only.
constexpr const char* kMipWriteCS = R"(#version 450 core
layout(local_size_x = 8, local_size_y = 8) in;
layout(rgba8, binding = 2) uniform writeonly image2D u_out;
void main() { imageStore(u_out, ivec2(gl_GlobalInvocationID.xy), vec4(0.0, 0.0, 1.0, 1.0)); }
)";

constexpr const char* kSampleCS = R"(#version 450 core
layout(local_size_x = 1) in;
layout(binding = 7) uniform sampler2D u_tex;
layout(std430, binding = 4) buffer Out { uint v[]; };
void main() {
    v[0] = uint(textureLod(u_tex, vec2(0.5), 0.0).b * 255.0 + 0.5);
    v[1] = uint(textureLod(u_tex, vec2(0.5), 1.0).b * 255.0 + 0.5);
}
)";

TEST_CASE("rhi_metal: storage image at mip 1, then sampled by a compute shader") {
    Fixture f;
    if (!f.ok) return;
    const rhi::PipelineHandle write = make_compute(*f.device, kMipWriteCS, "mip_write.comp");
    const rhi::PipelineHandle sample = make_compute(*f.device, kSampleCS, "sample.comp");
    if (write.is_null() || sample.is_null()) return;
    rhi::TextureDesc td;
    td.size = {16, 16};
    td.mip_levels = 2;
    td.format = rhi::TextureFormat::RGBA8_UNorm;
    td.usage = rhi::TextureUsage::SampledStorage;
    td.debug_name = "mips";
    Array<u8> zeros;
    zeros.resize(16 * 16 * 4);
    for (u8& b : zeros) b = 0;
    const rhi::TextureHandle img = f.device->create_texture(td, rhi::as_bytes(zeros.span()));
    rhi::TextureRegion mip1;
    mip1.mip = 1;
    Array<u8> zeros1;
    zeros1.resize(8 * 8 * 4);
    for (u8& b : zeros1) b = 0;
    f.device->update_texture(img, mip1, rhi::as_bytes(zeros1.span()));
    rhi::SamplerDesc sd;
    sd.min_filter = sd.mag_filter = rhi::Filter::Nearest;
    sd.mip = rhi::MipFilter::Nearest;
    const rhi::SamplerHandle smp = f.device->create_sampler(sd);
    const rhi::BufferHandle out = f.device->create_buffer({rhi::BufferUsage::Storage, 2 * sizeof(u32), "sample.out"});

    f.device->begin_frame(0);
    rhi::CommandBuffer cmd;
    cmd.bind_pipeline(write);
    cmd.bind_storage_image(2, img, 1, rhi::StorageAccess::Write);
    cmd.dispatch(1, 1);
    cmd.barrier(rhi::barrier::kSampled);
    cmd.bind_pipeline(sample);
    cmd.bind_texture(7, img, smp);
    cmd.bind_storage_buffer(rhi::bank::kClusters, out, 0, 0);
    cmd.dispatch(1);
    cmd.barrier(rhi::barrier::kStorageBuffer);
    f.device->submit(cmd);
    f.device->end_frame();

    u32 v[2] = {};
    MCE_REQUIRE_OR_RETURN(f.device->read_buffer(out, 0, std::as_writable_bytes(std::span(v))).has_value());
    CHECK(v[0] == 0);    // mip 0 untouched
    CHECK(v[1] == 255);  // mip 1 written through the view
    CHECK(f.device->validation_error_count() == 0);
}

TEST_CASE("rhi_metal: a run of dispatches is timed like a pass") {
    Fixture f;
    if (!f.ok) return;
    if (!f.device->caps().timestamp_queries) return;
    const rhi::PipelineHandle fill = make_compute(*f.device, kFillCS, "fill.comp");
    if (fill.is_null()) return;
    const rhi::BufferHandle out = f.device->create_buffer({rhi::BufferUsage::Storage, 256 * sizeof(u32), "fill.out"});
    for (u64 frame = 0; frame <= MCE_FRAMES_IN_FLIGHT; ++frame) {
        f.device->begin_frame(frame);
        rhi::CommandBuffer cmd;
        cmd.write_timestamp(0);
        cmd.bind_pipeline(fill);
        cmd.bind_storage_buffer(rhi::bank::kClusters, out, 0, 0);
        cmd.dispatch(4);
        cmd.write_timestamp(1);
        f.device->submit(cmd);
        f.device->end_frame();
    }
    const rhi::GpuTimestamps& ts = f.device->last_gpu_timestamps();
    CHECK(ts.frame_index == 0);
    CHECK(ts.has(0));
    CHECK(ts.has(1));
    CHECK(ts.ms(0, 1) >= 0.0);
}

