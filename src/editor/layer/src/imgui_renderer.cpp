#include "imgui_renderer.h"

#include <cstddef>
#include <cstring>

#include "mce/log.h"
#include "mce/math/math.h"

namespace mce::editor {
namespace {

static_assert(sizeof(ImDrawVert) == sizeof(ClipVertex), "ImDrawVert must match ClipVertex");
static_assert(offsetof(ImDrawVert, pos) == offsetof(ClipVertex, x));
static_assert(offsetof(ImDrawVert, uv) == offsetof(ClipVertex, u));
static_assert(offsetof(ImDrawVert, col) == offsetof(ClipVertex, rgba));
static_assert(sizeof(ImDrawIdx) == sizeof(u16), "draw_clip takes 16-bit ImGui indices");

constexpr const char* kVertexShader = R"(#version 450 core
layout(std140, binding = 0) uniform FrameU { mat4 projection; };
layout(location = 0) in vec2 a_pos;
layout(location = 1) in vec2 a_uv;
layout(location = 2) in vec4 a_color;
layout(location = 0) out vec2 v_uv;
layout(location = 1) out vec4 v_color;
out gl_PerVertex { vec4 gl_Position; };
void main() {
    v_uv = a_uv;
    v_color = a_color;
    gl_Position = projection * vec4(a_pos, 0.0, 1.0);
}
)";

constexpr const char* kFragmentShader = R"(#version 450 core
layout(binding = 6) uniform sampler2D u_texture;
layout(location = 0) in vec2 v_uv;
layout(location = 1) in vec4 v_color;
layout(location = 0) out vec4 o_color;
void main() { o_color = v_color * texture(u_texture, v_uv); }
)";

struct FrameU {
    math::mat4 projection;
};

// Every batch rebinds its state, so a reset request needs no work; the address is the marker.
void reset_render_state(const ImDrawList*, const ImDrawCmd*) {}

}  // namespace

ResultV ImGuiRhiRenderer::init(rhi::RenderDevice& device) {
    device_ = &device;
    ImGuiIO& io = ImGui::GetIO();
    io.BackendRendererName = "mce_rhi";
    io.BackendFlags |= ImGuiBackendFlags_RendererHasVtxOffset | ImGuiBackendFlags_RendererHasTextures;
    ImGui::GetPlatformIO().DrawCallback_ResetRenderState = &reset_render_state;

    static constexpr rhi::VertexAttribute kAttrs[] = {
        {0, rhi::VertexFormat::Float2, offsetof(ImDrawVert, pos)},
        {1, rhi::VertexFormat::Float2, offsetof(ImDrawVert, uv)},
        {2, rhi::VertexFormat::UNorm8x4, offsetof(ImDrawVert, col)},
    };
    rhi::GraphicsPipelineDesc desc;
    desc.vertex_shader = {kVertexShader, "imgui.vert"};
    desc.fragment_shader = {kFragmentShader, "imgui.frag"};
    desc.vertex = {kAttrs, sizeof(ImDrawVert)};
    desc.state.blend.mode = rhi::BlendMode::Alpha;
    desc.debug_name = "imgui";
    MCE_TRY_ASSIGN(pipeline_, device.create_pipeline(desc));

    rhi::SamplerDesc sd;
    sd.min_filter = rhi::Filter::Linear;
    sd.mag_filter = rhi::Filter::Linear;
    sampler_ = device.create_sampler(sd);
    return ok();
}

void ImGuiRhiRenderer::shutdown() {
    if (!device_) return;
    for (ImTextureData* tex : ImGui::GetPlatformIO().Textures) {
        if (tex->RefCount != 1) continue;  // shared with another context: not ours
        if (tex->TexID != ImTextureID_Invalid) device_->destroy_texture(from_id(tex->TexID));
        tex->SetTexID(ImTextureID_Invalid);
        tex->SetStatus(ImTextureStatus_Destroyed);
    }
    if (!pipeline_.is_null()) device_->destroy_pipeline(pipeline_);
    if (!sampler_.is_null()) device_->destroy_sampler(sampler_);
    pipeline_ = {};
    sampler_ = {};
    ImGuiIO& io = ImGui::GetIO();
    io.BackendRendererName = nullptr;
    ImGui::GetPlatformIO().DrawCallback_ResetRenderState = nullptr;
    io.BackendFlags &= ~(ImGuiBackendFlags_RendererHasVtxOffset | ImGuiBackendFlags_RendererHasTextures);
    device_ = nullptr;
}

void ImGuiRhiRenderer::update_textures(ImDrawData* draw_data) {
    if (!draw_data || !draw_data->Textures) return;
    for (ImTextureData* tex : *draw_data->Textures)
        if (tex->Status != ImTextureStatus_OK) update_texture(tex);
}

void ImGuiRhiRenderer::update_texture(ImTextureData* tex) {
    if (tex->Status == ImTextureStatus_WantDestroy) {
        if (tex->UnusedFrames < 3) return;  // MCE_FRAMES_IN_FLIGHT: let in-flight frames finish
        if (tex->TexID != ImTextureID_Invalid) device_->destroy_texture(from_id(tex->TexID));
        tex->SetTexID(ImTextureID_Invalid);
        tex->SetStatus(ImTextureStatus_Destroyed);
        return;
    }
    if (tex->Status != ImTextureStatus_WantCreate && tex->Status != ImTextureStatus_WantUpdates) return;

    // mce_rhi has no sub-image upload at M0: (re)create the whole texture. Atlas updates are rare
    // (a new font size or glyph range), and the old texture's destroy is deferred by the device.
    const usize pixels = usize(tex->Width) * usize(tex->Height);
    Span<const byte> data;
    if (tex->Format == ImTextureFormat_RGBA32) {
        data = {reinterpret_cast<const byte*>(tex->GetPixels()), pixels * 4};
    } else {
        staging_.resize(pixels * 4);
        const auto* src = static_cast<const u8*>(tex->GetPixels());
        for (usize i = 0; i < pixels; ++i) {
            staging_[i * 4 + 0] = 255;
            staging_[i * 4 + 1] = 255;
            staging_[i * 4 + 2] = 255;
            staging_[i * 4 + 3] = src[i];
        }
        data = std::as_bytes(staging_.span());
    }
    rhi::TextureDesc td;
    td.size = {static_cast<u32>(tex->Width), static_cast<u32>(tex->Height)};
    td.format = rhi::TextureFormat::RGBA8_UNorm;
    td.usage = rhi::TextureUsage::Sampled;
    td.debug_name = "imgui.atlas";
    const rhi::TextureHandle h = device_->create_texture(td, data);
    if (h.is_null()) {
        MCE_LOG_ERROR("editor: ImGui texture creation failed ({}x{})", tex->Width, tex->Height);
        return;
    }
    if (tex->TexID != ImTextureID_Invalid) device_->destroy_texture(from_id(tex->TexID));
    tex->SetTexID(to_id(h));
    tex->SetStatus(ImTextureStatus_OK);
}

void ImGuiRhiRenderer::record(ImDrawData* draw_data, rhi::CommandBuffer& cmd, const f32 clear_color[4]) {
    rhi::PassAttachment color;  // null texture = backbuffer
    for (int i = 0; i < 4; ++i) color.clear.color[i] = clear_color[i];
    rhi::RenderPassDesc pass;
    pass.colors = Span<const rhi::PassAttachment>(&color, 1);
    pass.debug_name = "imgui";
    cmd.begin_pass(pass);

    vertices_.clear();
    indices_.clear();
    batches_.clear();
    last_clipped_ = 0;
    if (draw_data && draw_data->TotalVtxCount > 0 && draw_data->DisplaySize.x > 0 && draw_data->DisplaySize.y > 0) {
        for (const ImDrawList* list : draw_data->CmdLists) {
            const u32 base = static_cast<u32>(vertices_.size());
            const auto* src = reinterpret_cast<const ClipVertex*>(list->VtxBuffer.Data);
            const Span<const ClipVertex> verts(src, static_cast<usize>(list->VtxBuffer.Size));
            for (const ClipVertex& v : verts) vertices_.push_back(v);

            for (const ImDrawCmd& dc : list->CmdBuffer) {
                if (dc.UserCallback) {
                    dc.UserCallback(list, &dc);
                    continue;
                }
                // Vertices and clip rects share ImGui's absolute coordinates; the projection maps
                // DisplayPos..DisplayPos+DisplaySize to the backbuffer.
                const ClipRect rect{dc.ClipRect.x, dc.ClipRect.y, dc.ClipRect.z, dc.ClipRect.w};
                const Span<const u16> idx(reinterpret_cast<const u16*>(list->IdxBuffer.Data) + dc.IdxOffset,
                                          dc.ElemCount);
                const usize before_v = vertices_.size();
                const u32 first = static_cast<u32>(indices_.size());
                const u32 count = clip_triangles(verts.subspan(dc.VtxOffset), base + dc.VtxOffset, idx, rect,
                                                 vertices_, indices_);
                last_clipped_ += static_cast<u32>(vertices_.size() - before_v);
                if (count == 0) continue;
                const rhi::TextureHandle tex = from_id(dc.GetTexID());
                if (!batches_.empty() && batches_.back().texture == tex &&
                    batches_.back().first_index + batches_.back().index_count == first) {
                    batches_.back().index_count += count;
                } else {
                    batches_.push_back(Batch{tex, first, count});
                }
            }
        }
    }
    last_vertices_ = static_cast<u32>(vertices_.size());

    if (!batches_.empty()) {
        const ImVec2 p = draw_data->DisplayPos;
        const ImVec2 s = draw_data->DisplaySize;
        FrameU frame_u;
        // Top-left origin, y down: top maps to +1 in GL clip space.
        frame_u.projection = math::ortho_rh_no(p.x, p.x + s.x, p.y + s.y, p.y, -1.0f, 1.0f);
        cmd.bind_pipeline(pipeline_);
        cmd.set_uniforms(rhi::bank::kFrame, rhi::as_bytes(frame_u));
        cmd.set_vertex_data(rhi::as_bytes(Span<const ClipVertex>(vertices_.span())));
        cmd.set_index_data(rhi::as_bytes(Span<const u32>(indices_.span())), rhi::IndexFormat::U32);
        for (const Batch& b : batches_) {
            cmd.bind_texture(rhi::bank::kFirstSampler, b.texture, sampler_);
            cmd.draw(b.index_count, 1, b.first_index);
        }
    }
    cmd.end_pass();
}

}  // namespace mce::editor
