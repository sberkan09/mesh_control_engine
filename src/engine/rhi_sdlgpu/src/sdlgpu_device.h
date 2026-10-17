#pragma once
// SdlGpuDevice: the mce_rhi_sdlgpu implementation of RenderDevice (ADR-0038). Split across
// sdlgpu_device.cpp (device, resources, frames, readback) and sdlgpu_translate.cpp (command
// stream -> SDL GPU passes).
//
// Shape of a frame:
//   - every resource lives in GL's memory layout (row 0 = NDC y = -1), including the "backbuffer",
//     which is an offscreen RGBA8 texture blitted (flipped) to the swapchain in present();
//   - submit() walks the stream twice: first it copies every inline payload (vertex, index,
//     storage, draw args) into the slot's transfer buffer and uploads it in one copy pass, then it
//     records the render passes. SDL forbids copies inside a render pass; the stream already
//     carries every payload, so nothing is lost by hoisting them;
//   - the slot's fence is the end_frame command buffer's; begin_frame waits it, which makes the
//     per-slot rings, transfer buffer and storage pool free to reuse;
//   - create/update calls with data go into a pending upload command buffer that is submitted
//     before the next submit, read_pixels or end_frame, which keeps GL's "visible to later draws";
//   - write_timestamp (caps().fence_timestamps; SDL GPU has no timestamp queries) ends the SDL command
//     buffer where it stands, with a fence, and goes on in a new one. A waiter thread stamps the CPU time
//     each fence passes; begin_frame turns those into GpuTimestamps, minus the GPU's idle time between
//     submits. Inside a pass the split waits for the pass's end.

#include <SDL3/SDL.h>

#include <condition_variable>
#include <mutex>
#include <thread>

#include "mce/containers/array.h"
#include "mce/memory/handle.h"
#include "mce/rhi_sdlgpu/sdlgpu_device.h"
#include "shader_spirv.h"

namespace mce::rhi::sdlgpu {

struct GpuBuffer {
    SDL_GPUBuffer* buffer = nullptr;
    usize size = 0;
};

struct GpuTexture {
    SDL_GPUTexture* texture = nullptr;
    SDL_GPUTextureFormat sdl_format = SDL_GPU_TEXTUREFORMAT_INVALID;
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
    SDL_GPUSampler* sampler = nullptr;
    bool compare = false;
};

// The render-target signature a pipeline variant was built for (SDL pipelines bake it in).
struct TargetKey {
    SDL_GPUTextureFormat colors[cmd::kMaxColorAttachments] = {};
    SDL_GPUTextureFormat depth = SDL_GPU_TEXTUREFORMAT_INVALID;
    u8 color_count = 0;
    u8 samples = 1;
    bool has_depth = false;
    bool operator==(const TargetKey& o) const;
};

struct PipelineVariant {
    TargetKey key;
    SDL_GPUGraphicsPipeline* pipeline = nullptr;
};

struct GpuPipeline {
    SDL_GPUShader* vs = nullptr;
    SDL_GPUShader* fs = nullptr;
    StageResources vres;
    StageResources fres;
    SDL_GPUComputePipeline* cs = nullptr;  // set for compute pipelines (vs/fs stay null)
    StageResources cres;
    VertexAttribute attrs[16] = {};
    u32 attr_count = 0;
    u32 stride = 0;
    FixedState state;
    Array<PipelineVariant> variants;
    FixedString<64> name;
};

// One per frame slot: transient geometry, draw args, transfer staging and a pool of storage
// buffers (SDL binds storage buffers without offsets, so each set_storage_data gets its own).
struct SlotRings {
    SDL_GPUBuffer* geometry = nullptr;
    SDL_GPUBuffer* indirect = nullptr;
    SDL_GPUTransferBuffer* transfer = nullptr;
    usize geometry_head = 0;
    usize indirect_head = 0;
    usize storage_head = 0;  // staging bytes for storage data, after geometry + indirect in `transfer`
    struct StorageChunk {
        SDL_GPUBuffer* buffer = nullptr;
        u32 size = 0;
    };
    Array<StorageChunk> storage;
    u32 storage_next = 0;
    SDL_GPUFence* fence = nullptr;
};

class SdlGpuDevice;

class SdlGpuFence final : public mem::FrameSlotFence {
public:
    SdlGpuDevice* device = nullptr;
    u32 slot = 0;
    bool is_signaled() override;
    void wait_blocking() override;
};

class SdlGpuDevice final : public RenderDevice {
public:
    SdlGpuDevice(platform::Window& window, const DeviceDesc& desc);
    ~SdlGpuDevice() override;

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

    mem::FrameSlotFence* fence_for_frame(u64 frame_index) override;

    bool slot_signaled(u32 slot);
    void wait_slot(u32 slot);

private:
    struct BoundTexture {
        TextureHandle texture;
        SamplerHandle sampler;
    };
    struct BoundImage {
        TextureHandle texture;
        u32 mip = 0;
    };

    // sdlgpu_device.cpp
    void ensure_backbuffer();
    void upload_buffer(SDL_GPUBuffer* dst, u64 offset, Span<const byte> data);
    void upload_texture(const GpuTexture& t, u32 mip, u32 layer, u32 x, u32 y, u32 w, u32 h, Span<const byte> data);
    SDL_GPUCopyPass* pending_copy_pass();
    void flush_pending();
    SDL_GPUBuffer* storage_chunk(SlotRings& ring, u32 size);
    void release_all();

    // sdlgpu_translate.cpp
    void prepare_payloads(const CommandBuffer& cb, SDL_GPUCommandBuffer* gcb);
    bool record_passes(const CommandBuffer& cb, SDL_GPUCommandBuffer*& gcb);
    void begin_pass(SDL_GPUCommandBuffer* gcb, const cmd::BeginPass& p);
    void end_pass();
    bool flush_draw_state(SDL_GPUCommandBuffer* gcb);
    SDL_GPUGraphicsPipeline* pipeline_variant(GpuPipeline& p);
    void resolve(SDL_GPUCommandBuffer* gcb, const cmd::Resolve& r);
    void dispatch(SDL_GPUCommandBuffer* gcb, const cmd::Dispatch* direct, const cmd::DispatchIndirect* indirect);
    void push_uniforms(SDL_GPUCommandBuffer* gcb);
    bool split_for_timestamps(SDL_GPUCommandBuffer*& gcb, u32 indices);

    // Fence timestamps (sdlgpu_device.cpp).
    void timing_loop();
    void collect_timestamps();
    void stop_timing();

    platform::Window& window_;
    DeviceDesc desc_;
    SDL_GPUDevice* device_ = nullptr;
    bool claimed_ = false;
    DeviceCaps caps_;
    SDL_GPUTextureFormat default_depth_format_ = SDL_GPU_TEXTUREFORMAT_D32_FLOAT;

    HandleTable<GpuBuffer, BufferTag> buffers_;
    HandleTable<GpuTexture, TextureTag> textures_;
    HandleTable<GpuSampler, SamplerTag> samplers_;
    HandleTable<GpuPipeline, PipelineTag> pipelines_;
    usize texture_bytes_ = 0;

    // The offscreen backbuffer + default depth buffer (window size, recreated on resize).
    SDL_GPUTexture* backbuffer_ = nullptr;
    SDL_GPUTexture* backbuffer_depth_ = nullptr;
    Extent2D backbuffer_extent_;

    // Dummies for unbound sampler / storage slots (SDL needs every declared slot bound).
    SDL_GPUTexture* dummy_tex_[4] = {};  // by SamplerKind
    SDL_GPUSampler* dummy_sampler_ = nullptr;
    SDL_GPUSampler* dummy_compare_sampler_ = nullptr;
    SDL_GPUBuffer* dummy_storage_ = nullptr;

    SlotRings rings_[MCE_FRAMES_IN_FLIGHT];
    usize transfer_bytes_ = 0;
    SdlGpuFence fence_objs_[MCE_FRAMES_IN_FLIGHT];

    SDL_GPUCommandBuffer* pending_cb_ = nullptr;
    SDL_GPUCopyPass* pending_copy_ = nullptr;

    // Persistent binding state (GL keeps it across passes and submits).
    Array<byte> uniform_data_[kMaxUniformBanks];
    bool uniform_set_[kMaxUniformBanks] = {};
    BoundTexture textures_bound_[kMaxStageSamplers] = {};
    SDL_GPUBuffer* storage_bound_[kMaxStorageBindings] = {};
    BoundImage images_bound_[kMaxImageUnits] = {};

    u64 frame_index_ = 0;
    u32 slot_ = 0;
    bool in_frame_ = false;
    bool vsync_ = true;
    u32 validation_errors_ = 0;
    bool warned_storage_offset_ = false;
    FrameStats stats_;
    FrameStats last_stats_;
    GpuTimestamps last_timestamps_;

    // Fence timestamps. The waiter owns a fence from its submit until it passed; then the main thread
    // releases it in collect_timestamps.
    struct TimedFence {
        SDL_GPUFence* fence = nullptr;
        u64 frame = 0;
        u32 indices = 0;  // the timestamps it stands for (bit i = index i)
        u64 submitted_ns = 0;
        u64 passed_ns = 0;
    };
    std::mutex timing_mutex_;
    std::condition_variable timing_cv_;
    std::thread timing_thread_;
    Array<TimedFence> timing_waiting_;  // guarded: submitted, in submit order; the waiter takes the front
    usize timing_head_ = 0;             // guarded: the front of timing_waiting_
    Array<TimedFence> timing_passed_;   // guarded: passed, in submit order
    bool timing_stop_ = false;          // guarded
    u32 timing_in_flight_ = 0;          // main thread: submitted, not collected
    u64 timing_last_passed_ = 0;        // main thread: when the previous collected fence passed
    u64 timing_clock_ = 0;              // main thread: GPU-busy time line of the frame being assembled
    GpuTimestamps timing_frame_;        // main thread: the frame being assembled

    // Per-submit state.
    struct Prepared {
        SDL_GPUBuffer* buffer = nullptr;
        u32 offset = 0;
        bool ok = false;
    };
    Array<Prepared> prepared_;  // one per payload command, stream order
    struct StorageUpload {
        SDL_GPUBuffer* buffer;
        u32 staged;  // offset in the slot's transfer buffer
        u32 size;
    };
    Array<StorageUpload> storage_uploads_;
    struct {
        SDL_GPURenderPass* pass = nullptr;
        TargetKey target;
        GpuPipeline* pipeline = nullptr;
        SDL_GPUGraphicsPipeline* bound = nullptr;
        SDL_GPUBuffer* vb = nullptr;
        u32 vb_offset = 0;
        SDL_GPUBuffer* ib = nullptr;
        u32 ib_offset = 0;
        IndexFormat ib_format = IndexFormat::U16;
        bool vb_valid = false;
        bool ib_valid = false;
        SDL_GPUBuffer* args = nullptr;
        u32 args_offset = 0;
        u32 args_count = 0;
        u32 scopes = 0;
        const char* scope_names[8] = {};  // the open scopes, re-opened after a timestamp split
        bool pass_scope = false;
        u32 deferred_timestamps = 0;  // written inside the open pass: split at its end
    } ts_;
};

}  // namespace mce::rhi::sdlgpu
