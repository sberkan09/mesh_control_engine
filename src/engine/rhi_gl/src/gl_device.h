#pragma once
// GlDevice: the mce_rhi_gl implementation of RenderDevice. Split across gl_device.cpp (context,
// resources, frames) and gl_translate.cpp (command stream -> GL).

#include <glad/gl.h>

#include "mce/containers/array.h"
#include "mce/memory/handle.h"
#include "mce/rhi_gl/gl_device.h"

namespace mce::rhi::gl {

struct GlBuffer {
    GLuint id = 0;
    usize size = 0;
    BufferUsage usage = BufferUsage::Vertex;
};

struct GlTexture {
    GLuint id = 0;
    Extent2D size;
    TextureFormat format = TextureFormat::RGBA8_UNorm;
    TextureDimension dimension = TextureDimension::Tex2D;
    u16 mips = 1;
    u16 layers = 1;
    u8 samples = 1;
    TextureUsage usage = TextureUsage::Sampled;
    usize bytes = 0;
};

struct GlSampler {
    GLuint id = 0;
};

struct GlPipeline {
    GLuint pipeline = 0;
    GLuint vs = 0;
    GLuint fs = 0;
    GLuint vao = 0;
    u32 stride = 0;
    FixedState state;
};

// One persistent-mapped, coherent buffer split into MCE_FRAMES_IN_FLIGHT regions; bump-allocated
// per frame (rhi-renderer.md §3.1).
struct StagingRing {
    GLuint buffer = 0;
    u8* mapped = nullptr;
    usize per_slot = 0;
    usize head = 0;  // offset within the current slot's region
    u32 slot = 0;

    static constexpr usize kOverflow = ~usize(0);
    // Returns the absolute buffer offset, or kOverflow.
    usize alloc(usize size, usize align);
};

class GlDevice;

class GlFence final : public mem::FrameSlotFence {
public:
    GlDevice* device = nullptr;
    u32 slot = 0;
    bool is_signaled() override;
    void wait_blocking() override;
};

struct FramebufferKey {
    TextureHandle colors[cmd::kMaxColorAttachments];
    TextureHandle depth;
    bool operator==(const FramebufferKey& o) const;
};

class GlDevice final : public RenderDevice {
public:
    GlDevice(platform::Window& window, const DeviceDesc& desc);
    ~GlDevice() override;

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
    usize texture_memory_bytes() const override { return texture_bytes_; }
    u32 validation_error_count() const override { return validation_errors_; }

    mem::FrameSlotFence* fence_for_frame(u64 frame_index) override;

    // Fence plumbing (GlFence).
    bool slot_signaled(u32 slot);
    void wait_slot(u32 slot);

    void on_debug_message(GLenum type, GLenum severity, const char* message);

private:
    enum class DeadKind : u8 { Buffer, Texture, Sampler, Program, Pipeline, VertexArray };
    struct DeadObject {
        DeadKind kind;
        GLuint id;
    };
    struct FramebufferEntry {
        FramebufferKey key;
        GLuint fbo = 0;
    };

    void defer_delete(DeadKind kind, GLuint id);
    void flush_deletes(u32 slot);
    void delete_now(const DeadObject& d);
    GLuint framebuffer_for(const FramebufferKey& key);
    ResultV create_ring(StagingRing& ring, usize per_slot, const char* name);
    void destroy_ring(StagingRing& ring);
    void release_gl_objects();

    // gl_translate.cpp
    void exec_begin_pass(const cmd::BeginPass& p);
    void exec_bind_pipeline(PipelineHandle h);
    void exec_draw(const cmd::Draw* indexed, const cmd::DrawNonIndexed* plain);
    void exec_draw_indirect(const cmd::DrawIndirect& d);
    void exec_resolve(const cmd::Resolve& r);
    void reset_translate_state();

    platform::Window& window_;
    DeviceDesc desc_;
    void* context_ = nullptr;  // SDL_GLContext
    bool gl_loaded_ = false;
    DeviceCaps caps_;
    usize uniform_align_ = kUniformAlignment;

    HandleTable<GlBuffer, BufferTag> buffers_;
    HandleTable<GlTexture, TextureTag> textures_;
    HandleTable<GlSampler, SamplerTag> samplers_;
    HandleTable<GlPipeline, PipelineTag> pipelines_;
    usize texture_bytes_ = 0;

    StagingRing geometry_ring_;
    StagingRing uniform_ring_;
    StagingRing indirect_ring_;
    GLsync fences_[MCE_FRAMES_IN_FLIGHT] = {};
    GlFence fence_objs_[MCE_FRAMES_IN_FLIGHT];
    Array<DeadObject> dead_[MCE_FRAMES_IN_FLIGHT];
    Array<FramebufferEntry> framebuffers_;

    u64 frame_index_ = 0;
    u32 slot_ = 0;
    bool in_frame_ = false;
    int swap_interval_ = -2;  // unknown
    u32 validation_errors_ = 0;
    FrameStats stats_;
    FrameStats last_stats_;

    // Translate state (valid within one submit()).
    struct {
        const GlPipeline* pipeline = nullptr;
        GLenum topology = GL_TRIANGLES;
        GLuint vb = 0;
        usize vb_offset = 0;
        GLuint ib = 0;
        usize ib_offset = 0;
        IndexFormat ib_format = IndexFormat::U16;
        bool vb_valid = false;
        bool ib_valid = false;
        Extent2D target_size;
        bool depth_write = true;
        const DrawIndexedIndirectCommand* draw_args = nullptr;  // points into the submitted stream
        u32 draw_arg_count = 0;
        bool pass_scope = false;
    } ts_;
};

}  // namespace mce::rhi::gl
