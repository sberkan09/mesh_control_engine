#include "mce/editor/stub_world.h"

#include <cstddef>

#include "mce/math/math.h"

namespace mce::editor {
namespace {

using namespace mce::math;

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

}  // namespace

ResultV StubWorld::init(rhi::RenderDevice& device) {
    device_ = &device;
    static constexpr rhi::VertexAttribute kAttrs[] = {
        {0, rhi::VertexFormat::Float2, offsetof(Vertex, x)},
        {1, rhi::VertexFormat::UNorm8x4, offsetof(Vertex, rgba)},
    };
    rhi::GraphicsPipelineDesc desc;
    desc.vertex_shader = {kVertexShader, "stub_world.vert"};
    desc.fragment_shader = {kFragmentShader, "stub_world.frag"};
    desc.vertex = {kAttrs, sizeof(Vertex)};
    desc.debug_name = "stub_world";
    MCE_TRY_ASSIGN(pipeline_, device.create_pipeline(desc));
    return ok();
}

void StubWorld::shutdown() {
    if (device_ && !pipeline_.is_null()) device_->destroy_pipeline(pipeline_);
    pipeline_ = {};
    device_ = nullptr;
}

void StubWorld::tick(f64 dt) {
    prev_angle_ = angle_;
    angle_ += static_cast<f32>(dt) * turns_per_second * kTau;
    // Keep the angle small so interpolation stays precise in long sessions.
    if (angle_ > kTau && prev_angle_ > kTau) {
        angle_ -= kTau;
        prev_angle_ -= kTau;
    }
    ++ticks_;
}

void StubWorld::record(rhi::CommandBuffer& cmd, rhi::TextureHandle target, rhi::Extent2D size, f32 alpha) const {
    rhi::PassAttachment color;
    color.texture = target;
    for (int i = 0; i < 4; ++i) color.clear.color[i] = clear_color[i];
    rhi::RenderPassDesc pass;
    pass.colors = Span<const rhi::PassAttachment>(&color, 1);
    pass.debug_name = "stub_world";
    cmd.begin_pass(pass);
    if (!pipeline_.is_null() && size.width > 0 && size.height > 0) {
        const f32 aspect = static_cast<f32>(size.width) / static_cast<f32>(size.height);
        FrameU frame_u;
        frame_u.transform = scaling(vec3(1.0f / aspect, 1.0f, 1.0f)) *
                            rotation(quat_from_axis_angle(vec3(0, 0, 1), lerp(prev_angle_, angle_, alpha)));

        static constexpr Vertex kVerts[] = {
            {0.0f, 0.75f, 0xFF3030FFu},    // red
            {-0.65f, -0.5f, 0xFF30FF30u},  // green
            {0.65f, -0.5f, 0xFFFF3030u},   // blue
        };
        static constexpr u16 kIndices[] = {0, 1, 2};
        cmd.bind_pipeline(pipeline_);
        cmd.set_uniforms(rhi::bank::kFrame, rhi::as_bytes(frame_u));
        cmd.set_vertex_data(rhi::as_bytes(Span<const Vertex>(kVerts)));
        cmd.set_index_data(rhi::as_bytes(Span<const u16>(kIndices)), rhi::IndexFormat::U16);
        cmd.draw(3);
    }
    cmd.end_pass();
}

}  // namespace mce::editor
