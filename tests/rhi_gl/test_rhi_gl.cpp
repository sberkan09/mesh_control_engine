// mce_rhi_gl against a real driver (CI: Xvfb + Mesa llvmpipe). Renders clear + triangle into the
// backbuffer and into an offscreen target, reads pixels back and checks them.

#include <doctest/doctest.h>

#include "mce/core.h"
#include "mce/platform/platform.h"
#include "mce/platform/window.h"
#include "mce/rhi/rhi.h"
#include "mce/rhi_gl/gl_device.h"

using namespace mce;

// The tests build with DOCTEST_CONFIG_NO_EXCEPTIONS_BUT_WITH_ALL_ASSERTS, so a failed REQUIRE records
// the failure but does not leave the test case. Bail out explicitly before dereferencing results.
#define MCE_REQUIRE_OR_RETURN(...)              \
    do {                                        \
        const bool mce_req_ok_ = (__VA_ARGS__); \
        REQUIRE_MESSAGE(mce_req_ok_, #__VA_ARGS__); \
        if (!mce_req_ok_) return;               \
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
    // A display or GL driver that is missing (or an Xvfb that reset between test cases) fails the
    // test case instead of crashing it: every test returns early when `ok` is false.
    Fixture() {
        MCE_REQUIRE_OR_RETURN(platform::init().has_value());
        platform::WindowDesc wd;
        wd.title = "test_rhi_gl";
        wd.width = kW;
        wd.height = kH;
        wd.resizable = false;
        wd.high_dpi = false;
        auto w = platform::Window::create(wd);
        MCE_REQUIRE_OR_RETURN(w.has_value());
        window = std::move(w).value();
        auto d = rhi::gl::create_device(*window);
        MCE_REQUIRE_OR_RETURN(d.has_value());
        device = std::move(d).value();
        ok = true;
    }
    ~Fixture() {
        device.reset();
        window.reset();
        platform::shutdown();
    }

    // Null handle (and a recorded failure) when the pipeline does not build.
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
        return p.has_value() ? p.value() : rhi::PipelineHandle{};
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
    bool ok = false;
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
    if (!f.ok) return;
    const rhi::DeviceCaps& caps = f.device->caps();
    CHECK(caps.api_major == 4);
    CHECK(caps.api_minor >= 5);
    CHECK(caps.max_texture_size >= 4096);
    MESSAGE(caps.api_name.c_str() << " / " << caps.device_name.c_str());
}

TEST_CASE("rhi_gl: clear + triangle to the backbuffer, read back") {
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
    Array<u8> px;
    px.resize(usize(bb.width) * bb.height * 4);
    MCE_REQUIRE_OR_RETURN(f.device->read_pixels({}, rhi::Rect2D{0, 0, bb.width, bb.height}, px.span()).has_value());
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
    if (!f.ok) return;
    const rhi::PipelineHandle pipe = f.make_pipeline();
    if (pipe.is_null()) return;
    rhi::TextureDesc td;
    td.size = {128, 64};
    td.format = rhi::TextureFormat::RGBA8_UNorm;
    td.usage = rhi::TextureUsage::SampledColorTarget;
    td.debug_name = "offscreen";
    const rhi::TextureHandle rt = f.device->create_texture(td);
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
    Array<u8> px;
    px.resize(128 * 64 * 4);
    MCE_REQUIRE_OR_RETURN(f.device->read_pixels(rt, rhi::Rect2D{0, 0, 128, 64}, px.span()).has_value());
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
    MCE_REQUIRE_OR_RETURN(f.device->read_pixels(rt, rhi::Rect2D{60, 30, 2, 2}, Span<u8>(px.data(), 16)).has_value());
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
    if (!f.ok) return;
    rhi::GraphicsPipelineDesc desc;
    desc.vertex_shader = {"#version 450 core\nvoid main() { this is not glsl }\n", "broken.vert"};
    desc.fragment_shader = {kFS, "test.frag"};
    auto p = f.device->create_pipeline(desc);
    MCE_REQUIRE_OR_RETURN(!p.has_value());
    CHECK(p.error().code == Errc::ShaderCompile);
    CHECK(p.error().path == StringView("broken.vert"));
}

TEST_CASE("rhi_gl: frame fences gate core frame slots") {
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

// ---- compute (M-VISUAL 1) ------------------------------------------------------------------------

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

// Writes two indexed draw commands for draw_indirect_buffer: the quad's second triangle, once, and
// its first triangle zero times (a culled draw).
constexpr const char* kDrawArgsCS = R"(#version 450 core
layout(local_size_x = 1) in;
layout(std430, binding = 5) buffer Args { uint cmds[10]; };
void main() {
    cmds[0] = 3u; cmds[1] = 1u; cmds[2] = 3u; cmds[3] = 0u; cmds[4] = 0u;
    cmds[5] = 3u; cmds[6] = 0u; cmds[7] = 0u; cmds[8] = 0u; cmds[9] = 0u;
}
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

TEST_CASE("rhi_gl: compute caps") {
    Fixture f;
    if (!f.ok) return;
    const rhi::DeviceCaps& caps = f.device->caps();
    CHECK(caps.compute);
    CHECK(caps.max_compute_invocations >= 1024);  // GL 4.3 minimums
    CHECK(caps.max_compute_group_size[0] >= 1024);
    CHECK(caps.max_compute_group_size[2] >= 64);
    CHECK(caps.max_compute_group_count[0] >= 65535);
    CHECK(caps.max_storage_image_units >= 8);
}

TEST_CASE("rhi_gl: compute dispatch writes a storage buffer") {
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

TEST_CASE("rhi_gl: dispatch_indirect reads arguments a dispatch wrote") {
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

TEST_CASE("rhi_gl: draw_indirect_buffer draws the commands a dispatch wrote") {
    Fixture f;
    if (!f.ok) return;
    const rhi::PipelineHandle pipe = f.make_pipeline();
    const rhi::PipelineHandle args_pipe = make_compute(*f.device, kDrawArgsCS, "draw_args.comp");
    if (pipe.is_null() || args_pipe.is_null()) return;
    rhi::TextureDesc td;
    td.size = {64, 64};
    td.format = rhi::TextureFormat::RGBA8_UNorm;
    td.usage = rhi::TextureUsage::SampledColorTarget;
    const rhi::TextureHandle rt = f.device->create_texture(td);
    // A full-target quad split along its diagonal: (0,1,2) below it, (0,2,3) above it.
    static constexpr Vertex kQuad[] = {
        {-1, -1, 0xFFFF0000u}, {1, -1, 0xFFFF0000u}, {1, 1, 0xFFFF0000u}, {-1, 1, 0xFFFF0000u}};
    static constexpr u32 kQuadIdx[] = {0, 1, 2, 0, 2, 3};
    const rhi::BufferHandle vb = f.device->create_buffer({rhi::BufferUsage::Vertex, sizeof(kQuad), "quad.vb"},
                                                         rhi::as_bytes(Span<const Vertex>(kQuad)));
    const rhi::BufferHandle ib = f.device->create_buffer({rhi::BufferUsage::Index, sizeof(kQuadIdx), "quad.ib"},
                                                         rhi::as_bytes(Span<const u32>(kQuadIdx)));
    const rhi::BufferHandle args =
        f.device->create_buffer({rhi::BufferUsage::IndirectArgs, 2 * sizeof(rhi::DrawIndexedIndirectCommand), "draw.args"});

    f.device->begin_frame(0);
    rhi::CommandBuffer cmd;
    cmd.bind_pipeline(args_pipe);
    cmd.bind_storage_buffer(rhi::bank::kLightPool, args, 0, 0);
    cmd.dispatch(1);
    cmd.barrier(rhi::barrier::kIndirect);
    rhi::PassAttachment color;
    color.texture = rt;
    color.clear.color[1] = 1.0f;
    color.clear.color[3] = 1.0f;
    rhi::RenderPassDesc pass;
    pass.colors = Span<const rhi::PassAttachment>(&color, 1);
    static constexpr f32 kOffsetScale[4] = {0, 0, 1, 1};
    cmd.begin_pass(pass);
    cmd.bind_pipeline(pipe);
    cmd.set_uniforms(rhi::bank::kFrame, rhi::as_bytes(kOffsetScale));
    cmd.bind_vertex_buffer(vb);
    cmd.bind_index_buffer(ib, rhi::IndexFormat::U32);
    cmd.draw_indirect_buffer(args, 0, 2);
    cmd.end_pass();
    f.device->submit(cmd);
    Array<u8> px;
    px.resize(64 * 64 * 4);
    MCE_REQUIRE_OR_RETURN(f.device->read_pixels(rt, rhi::Rect2D{0, 0, 64, 64}, px.span()).has_value());
    f.device->end_frame();
    CHECK(f.device->last_frame_stats().indirect_draws == 1);
    // Rows are top first: the upper-left half is the drawn triangle, the lower-right one was culled.
    CHECK(pixel(px, 64, 8, 4).b == 255);
    CHECK(pixel(px, 64, 8, 4).g == 0);
    CHECK(pixel(px, 64, 56, 60).g == 255);
    CHECK(pixel(px, 64, 56, 60).b == 0);
    f.device->destroy_buffer(args);
    f.device->destroy_buffer(vb);
    f.device->destroy_buffer(ib);
    f.device->destroy_texture(rt);
    CHECK(f.device->validation_error_count() == 0);
}

TEST_CASE("rhi_gl: storage image written by compute, read by readback and imageLoad") {
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

TEST_CASE("rhi_gl: compute compile errors are Results") {
    Fixture f;
    if (!f.ok) return;
    rhi::ComputePipelineDesc desc;
    desc.compute_shader = {"#version 450 core\nlayout(local_size_x = 1) in;\nvoid main() { oops; }\n", "broken.comp"};
    auto p = f.device->create_compute_pipeline(desc);
    MCE_REQUIRE_OR_RETURN(!p.has_value());
    CHECK(p.error().code == Errc::ShaderCompile);
}
