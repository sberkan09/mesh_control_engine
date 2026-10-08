#pragma once
// MetalDevice: the mce_rhi_metal implementation of RenderDevice (ADR-0028). Split across
// metal_device.mm (layer, resources, frames, readback) and metal_translate.mm (command stream ->
// encoders). Objective-C++ under manual reference counting: objects live in plain structs inside
// HandleTables, so every `id` member is an owned (+1) reference released explicitly.
//
// Parity with mce_rhi_gl, the reference backend:
//  - Bindings (uniform banks, storage buffers, texture slots) persist across passes and submits like
//    GL's indexed binding points; they are re-applied to every new render encoder.
//  - Buffer/texture updates are ordered with the GPU work (blits on the frame's command buffer),
//    like glNamedBufferSubData / glTextureSubImage, so frames in flight never see a torn update.
//  - Texture targets are rendered Y-flipped (shader_cross.h), so their memory layout, read_pixels
//    and sampling match GL exactly; the backbuffer is not flipped.
//  - Metal command buffers retain the resources they reference, so destroy_* releases immediately.

#import <Metal/Metal.h>
#import <QuartzCore/CAMetalLayer.h>

#include "mce/containers/array.h"
#include "mce/memory/handle.h"
#include "mce/rhi_metal/metal_device.h"

namespace mce::rhi::metal {

struct MtlBuffer {
    id<MTLBuffer> buffer = nil;
    usize size = 0;
    BufferUsage usage = BufferUsage::Vertex;
};

struct MtlTexture {
    id<MTLTexture> texture = nil;
    Extent2D size;
    TextureFormat format = TextureFormat::RGBA8_UNorm;
    TextureDimension dimension = TextureDimension::Tex2D;
    u16 mips = 1;
    u16 layers = 1;
    u8 samples = 1;
    TextureUsage usage = TextureUsage::Sampled;
    usize bytes = 0;
};

struct MtlSampler {
    id<MTLSamplerState> sampler = nil;
};

struct MtlPipeline {
    id<MTLFunction> vs = nil;       // gl_Position.y as written (backbuffer)
    id<MTLFunction> vs_flip = nil;  // y negated (texture targets)
    id<MTLFunction> fs = nil;
    MTLVertexDescriptor* vertex = nil;  // nil when the pipeline has no vertex input
    FixedState state;
    u32 stride = 0;
    FixedString<64> name;
};

// The attachment formats a render pipeline state is compiled against.
struct PassFormats {
    MTLPixelFormat colors[cmd::kMaxColorAttachments] = {};
    MTLPixelFormat depth = MTLPixelFormatInvalid;
    MTLPixelFormat stencil = MTLPixelFormatInvalid;
    u32 samples = 1;
    bool flip = false;
    bool operator==(const PassFormats& o) const;
};

struct PsoEntry {
    PipelineHandle pipeline;
    PassFormats formats;
    id<MTLRenderPipelineState> pso = nil;  // nil: creation failed (logged once, draws skipped)
};

// One shared-storage buffer split into MCE_FRAMES_IN_FLIGHT regions; bump-allocated per frame
// (rhi-renderer.md §3.1), the counterpart of the GL backend's persistent-mapped rings.
struct StagingRing {
    id<MTLBuffer> buffer = nil;
    u8* mapped = nullptr;
    usize per_slot = 0;
    usize head = 0;
    u32 slot = 0;

    static constexpr usize kOverflow = ~usize(0);
    usize alloc(usize size, usize align);
};

class MetalDevice;

class MetalFence final : public mem::FrameSlotFence {
public:
    MetalDevice* device = nullptr;
    u32 slot = 0;
    bool is_signaled() override;
    void wait_blocking() override;
};

inline constexpr u32 kMaxBufferBanks = 8;    // uniform banks 0..7 -> [[buffer(0..7)]]
inline constexpr u32 kMaxStorageBindings = 8;  // SSBO binding b -> [[buffer(8 + b)]]
inline constexpr u32 kMaxTextureSlots = 16;

class MetalDevice final : public RenderDevice {
public:
    MetalDevice(platform::Window& window, const DeviceDesc& desc);
    ~MetalDevice() override;

    ResultV init();

    BufferHandle create_buffer(const BufferDesc& desc, Span<const byte> initial_data) override;
    void update_buffer(BufferHandle buffer, u64 offset, Span<const byte> data) override;
    TextureHandle create_texture(const TextureDesc& desc, Span<const byte> initial_data) override;
    void update_texture(TextureHandle texture, const TextureRegion& region, Span<const byte> data) override;
    SamplerHandle create_sampler(const SamplerDesc& desc) override;
    Result<PipelineHandle> create_pipeline(const GraphicsPipelineDesc& desc) override;
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
    void submit(const CommandBuffer& cmd) override;
    void end_frame() override;
    void present(bool vsync) override;
    Extent2D backbuffer_size() const override;
    ResultV read_pixels(TextureHandle target, Rect2D rect, Span<u8> out_rgba8) override;

    const DeviceCaps& caps() const override { return caps_; }
    const FrameStats& last_frame_stats() const override { return last_stats_; }
    // Timestamp queries are not wired yet (caps().timestamp_queries == false): always "none".
    const GpuTimestamps& last_gpu_timestamps() const override { return timestamps_; }
    usize texture_memory_bytes() const override { return texture_bytes_; }
    u32 validation_error_count() const override { return validation_errors_; }

    mem::FrameSlotFence* fence_for_frame(u64 frame_index) override;

    bool slot_signaled(u32 slot);
    void wait_slot(u32 slot);

private:
    struct BufferBinding {
        id<MTLBuffer> buffer = nil;  // not owned: a ring or a live MtlBuffer
        usize offset = 0;
    };
    struct TextureBinding {
        id<MTLTexture> texture = nil;  // not owned
        id<MTLSamplerState> sampler = nil;
    };
    struct DepthKey {
        bool test = false;
        bool write = false;
        CompareOp compare = CompareOp::LessEqual;
    };
    struct DepthEntry {
        DepthKey key;
        id<MTLDepthStencilState> state = nil;
    };

    id<MTLCommandBuffer> command_buffer();  // the open command buffer (created on demand)
    id<MTLBlitCommandEncoder> blit_encoder();
    void end_blit();
    void commit_open(bool wait);  // commits the open command buffer, if any
    void upload_to_buffer(id<MTLBuffer> dst, usize offset, Span<const byte> data);
    void upload_to_texture(const MtlTexture& t, const TextureRegion& region, Span<const byte> data);
    id<CAMetalDrawable> acquire_drawable();
    id<MTLTexture> default_depth(Extent2D size);
    id<MTLRenderPipelineState> pso_for(PipelineHandle h, const MtlPipeline& p, const PassFormats& f);
    id<MTLDepthStencilState> depth_state(const DepthState& d);
    ResultV create_ring(StagingRing& ring, usize per_slot, const char* name);
    void release_all();

    // metal_translate.mm
    void exec_begin_pass(const cmd::BeginPass& p);
    void exec_end_pass();
    void exec_bind_pipeline(PipelineHandle h);
    void exec_draw(const cmd::Draw* indexed, const cmd::DrawNonIndexed* plain);
    void exec_draw_indirect(const cmd::DrawIndirect& d);
    void exec_resolve(const cmd::Resolve& r);
    void apply_bindings();
    void reset_translate_state();

    platform::Window& window_;
    DeviceDesc desc_;
    void* view_ = nullptr;  // SDL_MetalView
    CAMetalLayer* layer_ = nil;
    id<MTLDevice> device_ = nil;
    id<MTLCommandQueue> queue_ = nil;
    DeviceCaps caps_;
    MTLPixelFormat depth_stencil_format_ = MTLPixelFormatDepth32Float_Stencil8;

    HandleTable<MtlBuffer, BufferTag> buffers_;
    HandleTable<MtlTexture, TextureTag> textures_;
    HandleTable<MtlSampler, SamplerTag> samplers_;
    HandleTable<MtlPipeline, PipelineTag> pipelines_;
    Array<PsoEntry> psos_;
    Array<DepthEntry> depth_states_;
    usize texture_bytes_ = 0;

    StagingRing geometry_ring_;
    StagingRing uniform_ring_;

    id<MTLCommandBuffer> open_cb_ = nil;  // owned
    id<MTLBlitCommandEncoder> blit_ = nil;  // owned, on open_cb_
    id<MTLCommandBuffer> slot_cb_[MCE_FRAMES_IN_FLIGHT] = {};  // owned: the slot's last commit
    MetalFence fence_objs_[MCE_FRAMES_IN_FLIGHT];
    id<CAMetalDrawable> drawable_ = nil;  // owned, this frame's
    id<MTLTexture> depth_tex_ = nil;      // owned, the backbuffer's depth-stencil
    Extent2D depth_size_;

    u64 frame_index_ = 0;
    u32 slot_ = 0;
    bool in_frame_ = false;
    bool vsync_ = true;
    u32 validation_errors_ = 0;
    FrameStats stats_;
    FrameStats last_stats_;
    GpuTimestamps timestamps_;

    // Bindings: persistent across submits (GL binding-point semantics).
    BufferBinding uniforms_[kMaxBufferBanks];
    BufferBinding storage_[kMaxStorageBindings];
    TextureBinding textures_bound_[kMaxTextureSlots];

    // Translate state (valid within one submit()).
    struct {
        id<MTLRenderCommandEncoder> enc = nil;  // owned while a pass is open
        bool skip_pass = false;                 // no drawable: commands until end_pass are dropped
        PassFormats formats;
        Extent2D target_size;
        const MtlPipeline* pipeline = nullptr;
        bool pipeline_ok = false;
        MTLPrimitiveType primitive = MTLPrimitiveTypeTriangle;
        id<MTLBuffer> vb = nil;
        usize vb_offset = 0;
        id<MTLBuffer> ib = nil;
        usize ib_offset = 0;
        IndexFormat ib_format = IndexFormat::U16;
        bool vb_valid = false;
        bool ib_valid = false;
        const DrawIndexedIndirectCommand* draw_args = nullptr;
        u32 draw_arg_count = 0;
        bool pass_scope = false;
        u32 scope_depth = 0;  // debug groups pushed on the encoder in this pass
    } ts_;
};

}  // namespace mce::rhi::metal
