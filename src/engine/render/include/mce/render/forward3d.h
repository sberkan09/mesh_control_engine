#pragma once
// Forward3D — the 3D frame (rhi-renderer.md §3.5 "[v0.5] Ashen Court frame", §3.6; ADR-0024):
//
//   1. cascades      up to 4 sun cascades into one D32 2D-array (shadow_size² per layer), depth only
//   2. main pass     Forward+ PBR (Cook-Torrance GGX, ORM + normal + emissive maps) lit by the sun
//                    (cascaded PCF 3×3 shadows, cross-faded) and the clustered point lights, then
//                    the sky dome behind everything, then blended draws back-to-front; R11G11B10F
//                    HDR target with MSAA
//   3. resolve       MSAA -> single-sample HDR
//   4. bloom         soft bright-pass, 13-tap downsample chain, tent upsample (additive)
//   5. tonemap       exposure + bloom composite, ACES fit, sRGB encode (+ luma for FXAA)
//   6. FXAA          Lottes-style edge search into the final target
//
// Owned by the Renderer (Renderer::init_3d); the 2D path never pays for it. Targets follow the
// scene's viewport size and are recreated on resize. Each stage boundary writes a GPU timestamp
// (rhi::CommandBuffer::write_timestamp) for the NFR-F-01 per-stage budget table.

#include "mce/render/scene3d.h"
#include "mce/render/texture_store.h"
#include "mce/rhi/command_buffer.h"
#include "mce/rhi/render_device.h"

namespace mce::render {

struct Forward3DDesc {
    u32 msaa = 4;            // 1 = no multisampling (the HDR target is rendered directly)
    u32 shadow_size = 2048;  // per cascade; keep ShadowSettings::resolution equal (texel snapping)
    f32 render_scale = 1.0f; // 3D targets at viewport * scale (0.5..1); tonemap/FXAA upsample (quality.h)
    bool encode_srgb_output = true;  // the backbuffer/editor targets are plain RGBA8 (see RendererDesc)
    bool timestamps = true;          // write GPU timestamps [timestamp_base, timestamp_base + kStageCount]
    u32 timestamp_base = 0;
    // Forward+ light assignment in a compute pass when the device has compute (M-VISUAL 1); the
    // CPU ClusterLights::assign() otherwise or when false. Both give the same frame.
    bool gpu_light_clusters = true;
};

struct Forward3DStats {
    u32 shadow_draws = 0;    // over all cascades
    u32 cascades = 0;
    u32 main_draws = 0;
    u32 pipeline_binds = 0;
    u32 texture_binds = 0;
    u32 point_lights = 0;
    u32 light_indices = 0;   // CPU assignment only (the GPU count stays on the GPU)
    bool gpu_clusters = false;  // this frame's lights were assigned by the compute pass
    u32 joints = 0;          // joint matrices uploaded for skinned meshes
    u32 bloom_levels = 0;
    bool fxaa = false;
};

// Milliseconds per stage of one completed frame; -1 = not measured.
struct Forward3DGpuTimes {
    u64 frame_index = ~u64(0);
    f64 clusters = -1;  // the GPU light assignment (0 when it ran on the CPU)
    f64 shadow = -1, main = -1, resolve = -1, bloom = -1, tonemap = -1, fxaa = -1, total = -1;
    f64 post() const { return bloom < 0 ? -1 : bloom + tonemap + (fxaa < 0 ? 0 : fxaa); }
};

class Forward3D {
public:
    static constexpr u32 kStageCount = 7;     // timestamps written: kStageCount + 1
    static constexpr u32 kMaxBloomLevels = 6;

    Forward3D() = default;
    Forward3D(const Forward3D&) = delete;
    Forward3D& operator=(const Forward3D&) = delete;
    ~Forward3D();

    ResultV init(rhi::RenderDevice& device, const TextureStore& textures, const Forward3DDesc& desc);
    void shutdown();
    bool initialized() const { return device_ != nullptr; }

    // Records every pass for a built scene. Outside any pass; `target` null = backbuffer.
    // Leaves the target holding the final frame (2D/HUD passes may follow with LoadOp::Load).
    void record(rhi::CommandBuffer& cmd, const Scene3D& scene, rhi::TextureHandle target = {});

    // Per-stage GPU times from the device's newest completed timestamps.
    Forward3DGpuTimes gpu_times(const rhi::GpuTimestamps& ts) const;

    const Forward3DStats& last_stats() const { return stats_; }
    const Forward3DDesc& desc() const { return desc_; }
    rhi::TextureHandle shadow_map() const { return shadow_map_; }  // D32 2D array, one layer per cascade
    rhi::TextureHandle hdr_target() const { return hdr_; }
    // True when record() assigns Forward+ lights on the GPU (desc + device compute).
    bool gpu_light_clusters() const { return !cluster_build_.is_null(); }

private:
    ResultV create_pipelines();
    void ensure_targets(rhi::Extent2D size);
    void destroy_targets();
    void timestamp(rhi::CommandBuffer& cmd, u32 stage);
    void record_clusters(rhi::CommandBuffer& cmd, const Scene3D& scene);
    void record_shadows(rhi::CommandBuffer& cmd, const Scene3D& scene);
    void record_main(rhi::CommandBuffer& cmd, const Scene3D& scene);
    void record_post(rhi::CommandBuffer& cmd, const Scene3D& scene, rhi::TextureHandle target);

    rhi::RenderDevice* device_ = nullptr;
    const TextureStore* textures_ = nullptr;
    Forward3DDesc desc_;
    rhi::PipelineHandle shadow_pipe_;
    rhi::PipelineHandle mesh_cull_;      // back-face culled
    rhi::PipelineHandle mesh_two_sided_;
    rhi::PipelineHandle mesh_blend_;
    rhi::PipelineHandle skinned_shadow_pipe_;  // FR-R3D-060: SkinnedMeshVertex twins of the above
    rhi::PipelineHandle skinned_cull_;
    rhi::PipelineHandle skinned_two_sided_;
    rhi::PipelineHandle skinned_blend_;
    rhi::PipelineHandle sky_;
    rhi::PipelineHandle bloom_down_;
    rhi::PipelineHandle bloom_up_;       // additive
    rhi::PipelineHandle tonemap_;
    rhi::PipelineHandle fxaa_;
    rhi::PipelineHandle cluster_build_;  // compute; null = CPU assignment
    rhi::BufferHandle cluster_ranges_;   // GPU assignment outputs (banks 4 and 6)
    rhi::BufferHandle cluster_indices_;
    u32 cluster_ranges_cap_ = 0;         // clusters
    u32 cluster_indices_cap_ = 0;        // indices
    rhi::SamplerHandle material_sampler_;
    rhi::SamplerHandle shadow_sampler_;
    rhi::SamplerHandle point_sampler_;
    rhi::SamplerHandle linear_clamp_;
    rhi::TextureHandle shadow_map_;
    rhi::TextureHandle hdr_msaa_;
    rhi::TextureHandle depth_;
    rhi::TextureHandle hdr_;
    rhi::TextureHandle ldr_;
    rhi::TextureHandle bloom_[kMaxBloomLevels];
    rhi::Extent2D bloom_size_[kMaxBloomLevels];
    u32 bloom_levels_ = 0;
    rhi::Extent2D size_;
    Forward3DStats stats_;
};

}  // namespace mce::render
