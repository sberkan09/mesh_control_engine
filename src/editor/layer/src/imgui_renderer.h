#pragma once
// ImGui renderer backend over mce_rhi (editor-tools.md §2.7, rhi-renderer.md §3.9).
//
// The editor never calls GL: ImGui's draw data is recorded into an rhi::CommandBuffer like any
// other pass. ImTextureID carries an rhi::TextureHandle (index | generation << 32), so the font
// atlas and the viewport's render target travel through ImGui as plain handles. Clip rects are
// applied on the CPU (draw_clip.h) because mce_rhi has no scissor state yet.

#include <imgui.h>

#include "mce/containers/array.h"
#include "mce/editor/draw_clip.h"
#include "mce/rhi/rhi.h"

namespace mce::editor {

class ImGuiRhiRenderer {
public:
    // Sets the renderer backend flags on the current ImGui context and creates the pipeline.
    ResultV init(rhi::RenderDevice& device);
    // Destroys every texture ImGui asked for and the pipeline.
    void shutdown();

    // Honors ImGui's texture requests (font atlas create/update/destroy). Call after ImGui::Render().
    void update_textures(ImDrawData* draw_data);
    // Records one pass to the backbuffer that clears it and draws `draw_data`.
    void record(ImDrawData* draw_data, rhi::CommandBuffer& cmd, const f32 clear_color[4]);

    static ImTextureID to_id(rhi::TextureHandle h) {
        return (static_cast<ImTextureID>(h.generation) << 32) | static_cast<ImTextureID>(h.index);
    }
    static rhi::TextureHandle from_id(ImTextureID id) {
        return rhi::TextureHandle{static_cast<u32>(id & 0xFFFFFFFFu), static_cast<u32>(id >> 32)};
    }

    u32 last_vertex_count() const { return last_vertices_; }
    u32 last_clipped_triangles() const { return last_clipped_; }

private:
    struct Batch {
        rhi::TextureHandle texture;
        u32 first_index;
        u32 index_count;
    };

    void update_texture(ImTextureData* tex);

    rhi::RenderDevice* device_ = nullptr;
    rhi::PipelineHandle pipeline_;
    rhi::SamplerHandle sampler_;
    Array<ClipVertex> vertices_;
    Array<u32> indices_;
    Array<Batch> batches_;
    Array<u8> staging_;  // Alpha8 -> RGBA8 conversion
    u32 last_vertices_ = 0;
    u32 last_clipped_ = 0;
};

}  // namespace mce::editor
