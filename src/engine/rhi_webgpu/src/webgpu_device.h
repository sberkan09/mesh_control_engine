#pragma once
// WebGpuDevice: the mce_rhi_webgpu implementation of RenderDevice (ADR-0039). Split across
// webgpu_device.cpp (device, resources, frames, readback) and webgpu_translate.cpp (command
// stream -> WebGPU passes).
//
// Shape of a frame:
//   - every resource lives in GL's memory layout (row 0 = NDC y = -1), including the "backbuffer",
//     which is an offscreen RGBA8 texture drawn (flipped) onto the canvas in present();
//   - submit() walks the stream twice: first it copies every inline payload (uniforms, vertex,
//     index, storage) into CPU staging for the frame's rings, writes them with one
//     wgpuQueueWriteBuffer per ring, then it encodes the passes into one command buffer. The rings
//     restart every frame: queue writes are ordered after every earlier submit, so a write never
//     lands under a command buffer that still reads the old bytes, and no fence is needed;
//   - set_draw_args data is on the CPU, so draw_indirect becomes direct drawIndexed calls (WebGPU
//     has no multi-draw); draw_indirect_buffer issues one drawIndexedIndirect per command;
//   - create/update calls with data go straight to wgpuQueueWrite*, which keeps GL's "visible to
//     later draws" order.
//
// Bindings follow shader_wgsl.h: group 0 the four uniform banks (dynamic offsets into the uniform
// ring), group 1 textures + samplers, group 2 storage buffers, group 3 storage images (compute).
// Groups 1-3 are built per pipeline from what its stages declare and re-made when a binding changes.

#include <webgpu/webgpu.h>

#include "mce/containers/array.h"
#include "mce/memory/handle.h"
#include "mce/rhi_webgpu/webgpu_device.h"
#include "shader_wgsl.h"

namespace mce::rhi::webgpu {

struct GpuBuffer {
    WGPUBuffer buffer = nullptr;
    usize size = 0;
};

struct GpuTexture {
    WGPUTexture texture = nullptr;
    WGPUTextureView view = nullptr;   // every mip and layer, 2D or 2D-array: sampling
    Array<WGPUTextureView> targets;   // per layer, mip 0: render attachments
    Array<WGPUTextureView> mip_views; // per mip, every layer as a 2D view: storage images
    WGPUTextureFormat wgpu_format = WGPUTextureFormat_Undefined;
    Extent2D size;
    TextureFormat format = TextureFormat::RGBA8_UNorm;
    TextureDimension dimension = TextureDimension::Tex2D;
    u16 mips = 1;
    u16 layers = 1;
    u8 samples = 1;
    TextureUsage usage = TextureUsage::Sampled;
    usize bytes = 0;
};

struct GpuSampler {
    WGPUSampler sampler = nullptr;
    bool compare = false;
};

// The render-target signature a pipeline variant was built for (WebGPU pipelines bake it in).
struct TargetKey {
    WGPUTextureFormat colors[cmd::kMaxColorAttachments] = {};
    WGPUTextureFormat depth = WGPUTextureFormat_Undefined;
    u8 color_count = 0;
    u8 samples = 1;
    bool has_depth = false;
    bool operator==(const TargetKey& o) const;
};

struct PipelineVariant {
    TargetKey key;
    WGPURenderPipeline pipeline = nullptr;
};

struct GpuPipeline {
    WGPUShaderModule vs = nullptr;
    WGPUShaderModule fs = nullptr;
    WGPUComputePipeline cs = nullptr;  // set for compute pipelines (vs/fs stay null)
    StageResources vres;
    StageResources fres;
    StageResources cres;
    WGPUBindGroupLayout layouts[4] = {};  // groups 0..3 (shared ones are not owned: see owns_layout)
    bool owns_layout[4] = {};
    WGPUPipelineLayout layout = nullptr;
    // The bind groups last made for groups 1..3 and the binding generation they were made at.
    WGPUBindGroup groups[4] = {};
    u64 group_gen[4] = {};
    VertexAttribute attrs[16] = {};
    u32 attr_count = 0;
    u32 stride = 0;
    FixedState state;
    Array<PipelineVariant> variants;
    FixedString<64> name;
};

// The frame's transient rings: one GPU buffer each, filled from CPU staging once per submit.
struct Ring {
    WGPUBuffer buffer = nullptr;
    usize capacity = 0;
    usize head = 0;       // bytes used this frame
    usize flushed = 0;    // bytes already written to the GPU this frame
    Array<byte> staging;  // CPU mirror of [0, capacity)
};

class WebGpuDevice;

// WebGPU orders queue writes after earlier submits, so a frame slot is free as soon as it ends.
class WebGpuFence final : public mem::FrameSlotFence {
public:
    bool is_signaled() override { return true; }
    void wait_blocking() override {}
};

class WebGpuDevice final : public RenderDevice {
public:
    WebGpuDevice(platform::Window& window, const DeviceDesc& desc);
    ~WebGpuDevice() override;

    ResultV init();

    BufferHandle create_buffer(const BufferDesc& desc, Span<const byte> initial_data) override;
    void update_buffer(BufferHandle buffer, u64 offset, Span<const byte> data) override;
    TextureHandle create_texture(const TextureDesc& desc, Span<const byte> initial_data) override;
    void update_texture(TextureHandle texture, const TextureRegion& region, Span<const byte> data) override;
    SamplerHandle create_sampler(const SamplerDesc& desc) override;
    Result<PipelineHandle> create_pipeline(const GraphicsPipelineDesc& desc) override;
    Result<PipelineHandle> create_compute_pipeline(const ComputePipelineDesc& desc) override;
    void destroy_buffer(BufferHandle) override;
    void destroy_texture(TextureHandle) override;
    void destroy_sampler(SamplerHandle) override;
    void destroy_pipeline(PipelineHandle) override;
    bool is_alive(BufferHandle h) const override { return buffers_.is_alive(h); }
    bool is_alive(TextureHandle h) const override { return textures_.is_alive(h); }
    bool is_alive(PipelineHandle h) const override { return pipelines_.is_alive(h); }
    Extent2D texture_size(TextureHandle) const override;
    TextureDesc texture_desc(TextureHandle) const override;

    void begin_frame(u64 frame_index) override;
    u64 frame_index() const override { return frame_index_; }
    void submit(const CommandBuffer& cmd) override;
    void end_frame() override;
    void present(bool vsync) override;
    Extent2D backbuffer_size() const override;
    ResultV read_pixels(TextureHandle target, Rect2D rect, Span<u8> out_rgba8) override;
    ResultV read_buffer(BufferHandle buffer, u64 offset, Span<byte> out) override;

    const DeviceCaps& caps() const override { return caps_; }
    const FrameStats& last_frame_stats() const override { return last_stats_; }
    const GpuTimestamps& last_gpu_timestamps() const override { return last_timestamps_; }
    usize texture_memory_bytes() const override { return texture_bytes_; }
    u32 validation_error_count() const override { return validation_errors_; }

    mem::FrameSlotFence* fence_for_frame(u64) override { return &fence_; }

    void on_uncaptured_error(WGPUErrorType type, WGPUStringView message);

private:
    struct BoundTexture {
        TextureHandle texture;
        SamplerHandle sampler;
    };
    struct BoundStorage {
        WGPUBuffer buffer = nullptr;
        u64 offset = 0;
        u64 size = 0;  // 0 = to the end
    };
    struct BoundImage {
        TextureHandle texture;
        u32 mip = 0;
    };

    // webgpu_device.cpp
    ResultV request_device();
    void ensure_backbuffer();
    void configure_surface(Extent2D size);
    bool wait(WGPUFuture future);
    ResultV map_and_copy(WGPUBuffer readback, usize size, Span<byte> out);
    ResultV make_blit_pipeline();
    ResultV make_layouts(GpuPipeline& p, bool compute);
    void release_pipeline(GpuPipeline& p);
    void release_all();

    // webgpu_translate.cpp
    void stage_payloads(const CommandBuffer& cb);
    void flush_rings();
    void encode(const CommandBuffer& cb, WGPUCommandEncoder enc);
    void begin_pass(WGPUCommandEncoder enc, const cmd::BeginPass& p);
    void end_pass();
    WGPURenderPipeline pipeline_variant(GpuPipeline& p);
    bool bind_groups(GpuPipeline& p, bool compute);
    WGPUBindGroup make_texture_group(GpuPipeline& p, bool compute);
    WGPUBindGroup make_storage_group(GpuPipeline& p, bool compute);
    WGPUBindGroup make_image_group(GpuPipeline& p);
    bool flush_draw_state();
    void resolve(WGPUCommandEncoder enc, const cmd::Resolve& r);
    void dispatch(WGPUCommandEncoder enc, const cmd::Dispatch* direct, const cmd::DispatchIndirect* indirect);
    usize push_ring(Ring& ring, Span<const byte> data, usize alignment);

    platform::Window& window_;
    DeviceDesc desc_;
    WGPUInstance instance_ = nullptr;
    WGPUAdapter adapter_ = nullptr;
    WGPUDevice device_ = nullptr;
    WGPUQueue queue_ = nullptr;
    WGPUSurface surface_ = nullptr;
    WGPUTextureFormat surface_format_ = WGPUTextureFormat_BGRA8Unorm;
    Extent2D surface_extent_;
    DeviceCaps caps_;
    bool float32_filterable_ = false;
    WebGpuFence fence_;

    HandleTable<GpuBuffer, BufferTag> buffers_;
    HandleTable<GpuTexture, TextureTag> textures_;
    HandleTable<GpuSampler, SamplerTag> samplers_;
    HandleTable<GpuPipeline, PipelineTag> pipelines_;
    usize texture_bytes_ = 0;

    // The offscreen backbuffer + default depth buffer (window size, recreated on resize).
    WGPUTexture backbuffer_ = nullptr;
    WGPUTextureView backbuffer_view_ = nullptr;
    WGPUTexture backbuffer_depth_ = nullptr;
    WGPUTextureView backbuffer_depth_view_ = nullptr;
    Extent2D backbuffer_extent_;

    // present(): the backbuffer drawn onto the canvas, flipped.
    WGPURenderPipeline blit_pipeline_ = nullptr;
    WGPUBindGroupLayout blit_layout_ = nullptr;
    WGPUSampler blit_sampler_ = nullptr;
    WGPUBindGroup blit_group_ = nullptr;

    // Shared layouts: group 0 (uniform banks, graphics and compute) and the empty group.
    WGPUBindGroupLayout uniform_layout_ = nullptr;
    WGPUBindGroupLayout uniform_layout_compute_ = nullptr;
    WGPUBindGroupLayout empty_layout_ = nullptr;
    WGPUBindGroup uniform_group_ = nullptr;
    WGPUBindGroup uniform_group_compute_ = nullptr;
    WGPUBindGroup empty_group_ = nullptr;

    // Dummies for unbound slots (WebGPU needs every declared binding bound).
    WGPUTexture dummy_tex_[4] = {};  // by SamplerKind
    WGPUTextureView dummy_view_[4] = {};
    WGPUSampler dummy_sampler_ = nullptr;
    WGPUSampler dummy_compare_sampler_ = nullptr;
    WGPUBuffer dummy_storage_ = nullptr;

    Ring uniforms_;  // 256-aligned blocks, bound through dynamic offsets
    Ring geometry_;  // vertex + index data
    Ring storage_;   // set_storage_data, 256-aligned
    u32 uniform_bind_size_ = 16384;

    // Persistent binding state (GL keeps it across passes and submits).
    Array<byte> uniform_data_[kMaxUniformBanks];
    bool uniform_set_[kMaxUniformBanks] = {};
    u32 uniform_offset_[kMaxUniformBanks] = {};
    BoundTexture textures_bound_[kMaxSamplers] = {};
    BoundStorage storage_bound_[kMaxStorageBindings] = {};
    BoundImage images_bound_[kMaxImageUnits] = {};
    u64 texture_gen_ = 1;  // bumped on every texture / storage / image binding change
    u64 storage_gen_ = 1;
    u64 image_gen_ = 1;

    u64 frame_index_ = 0;
    bool in_frame_ = false;
    bool vsync_ = true;
    u32 validation_errors_ = 0;
    FrameStats stats_;
    FrameStats last_stats_;
    GpuTimestamps last_timestamps_;

    // Per-submit state.
    struct Prepared {
        WGPUBuffer buffer = nullptr;
        u32 offset = 0;
        u32 size = 0;
        bool ok = false;
    };
    Array<Prepared> prepared_;  // one per payload command, stream order
    Array<DrawIndexedIndirectCommand> draw_args_;  // the frame's set_draw_args data (CPU side)
    Array<WGPUBindGroup> retired_groups_;  // replaced bind groups, released after the submit
    struct {
        WGPURenderPassEncoder pass = nullptr;
        TargetKey target;
        GpuPipeline* pipeline = nullptr;
        WGPURenderPipeline bound = nullptr;
        WGPUBuffer vb = nullptr;
        u32 vb_offset = 0;
        WGPUBuffer ib = nullptr;
        u32 ib_offset = 0;
        IndexFormat ib_format = IndexFormat::U16;
        bool vb_valid = false;
        bool ib_valid = false;
        bool ib_bound = false;
        u32 args_first = 0;  // into draw_args_
        u32 args_count = 0;
        u32 scopes = 0;
        bool pass_scope = false;
    } ts_;
};

}  // namespace mce::rhi::webgpu
