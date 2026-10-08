#pragma once
// Forward3D — the MVP 3D diorama frame (rhi-renderer.md §3.5, §3.6, §3.7):
//
//   1. shadow pass   2048² D32, orthographic fit to the scene (Scene3D), depth only
//   2. main pass     forward Lambert + Blinn-Phong sun, PCF 2×2 shadow with normal-offset bias,
//                    hemisphere ambient, exp² fog, into an R11G11B10F HDR target with 4× MSAA
//   3. resolve       MSAA -> single-sample HDR (the transient-RT pool's resolve, §3.5)
//   4. tonemap       ACES-fitted, exposure, sRGB encode -> the backbuffer (or an editor target)
//
// Owned by the Renderer (Renderer::init_3d); the 2D path never pays for it. Targets follow the
// scene's viewport size and are recreated on resize.

#include "mce/render/scene3d.h"
#include "mce/render/texture_store.h"
#include "mce/rhi/command_buffer.h"
#include "mce/rhi/render_device.h"

namespace mce::render {

struct Forward3DDesc {
    u32 msaa = 4;            // 1 = no multisampling (the HDR target is rendered directly)
    u32 shadow_size = 2048;
    bool encode_srgb_output = true;  // the backbuffer/editor targets are plain RGBA8 (see RendererDesc)
};

struct Forward3DStats {
    u32 shadow_draws = 0;
    u32 main_draws = 0;
    u32 pipeline_binds = 0;
    u32 texture_binds = 0;
};

class Forward3D {
public:
    Forward3D() = default;
    Forward3D(const Forward3D&) = delete;
    Forward3D& operator=(const Forward3D&) = delete;
    ~Forward3D();

    ResultV init(rhi::RenderDevice& device, const TextureStore& textures, const Forward3DDesc& desc);
    void shutdown();
    bool initialized() const { return device_ != nullptr; }

    // Records the four passes for a built scene. Outside any pass; `target` null = backbuffer.
    // Leaves the target holding the tonemapped frame (2D/HUD passes may follow with LoadOp::Load).
    void record(rhi::CommandBuffer& cmd, const Scene3D& scene, rhi::TextureHandle target = {});

    const Forward3DStats& last_stats() const { return stats_; }
    rhi::TextureHandle shadow_map() const { return shadow_map_; }
    rhi::TextureHandle hdr_target() const { return hdr_; }

private:
    ResultV create_pipelines();
    void ensure_targets(rhi::Extent2D size);
    void destroy_targets();

    rhi::RenderDevice* device_ = nullptr;
    const TextureStore* textures_ = nullptr;
    Forward3DDesc desc_;
    rhi::PipelineHandle shadow_pipe_;
    rhi::PipelineHandle mesh_cull_;      // back-face culled
    rhi::PipelineHandle mesh_two_sided_;
    rhi::PipelineHandle mesh_blend_;
    rhi::PipelineHandle tonemap_;
    rhi::SamplerHandle albedo_sampler_;
    rhi::SamplerHandle shadow_sampler_;
    rhi::SamplerHandle hdr_sampler_;
    rhi::TextureHandle shadow_map_;
    rhi::TextureHandle hdr_msaa_;
    rhi::TextureHandle depth_;
    rhi::TextureHandle hdr_;
    rhi::Extent2D size_;
    Forward3DStats stats_;
};

}  // namespace mce::render
