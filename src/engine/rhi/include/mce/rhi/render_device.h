#pragma once
// RenderDevice — resource creation and frame submission (rhi-renderer.md §2.2).
//
// The Godot-"server" shape: create/configure by handle, data owned internally. Backends implement
// this interface (mce_rhi_gl today); the renderer and the editor only ever see this header.
//
// Frame protocol (one thread, the GL thread):
//   device.begin_frame(frame_index);   // waits the slot's fence from MCE_FRAMES_IN_FLIGHT ago
//   device.submit(cmd);                // translate + execute (any number of times)
//   device.end_frame();                // fence the slot, run deferred destroys
//   device.present(vsync);             // swap (hosts do this via app::IPlatformHooks::present)
//
// The device is also the app loop's IFrameFenceSource: core's frame-slot arenas are released on
// the same fences as the GPU staging rings (core.md §4.3).

#include "mce/app.h"
#include "mce/fixed_string.h"
#include "mce/rhi/command_buffer.h"
#include "mce/rhi/types.h"

namespace mce::rhi {

struct DeviceDesc {
    bool debug_layer = true;  // KHR_debug callback + object labels (default on in dev builds)
    bool vsync = true;
    usize geometry_ring_bytes = 4 * MiB;  // per frame slot (§3.1)
    usize uniform_ring_bytes = 1 * MiB;   // per frame slot (§3.1)
};

struct DeviceCaps {
    FixedString<128> api_name;     // "OpenGL 4.5 (Core Profile) Mesa 24.0"
    FixedString<128> device_name;  // GL_RENDERER
    u32 api_major = 0, api_minor = 0;
    u32 max_texture_size = 0;
    u32 max_uniform_block_size = 0;
    bool shader_draw_parameters = false;  // gl_BaseInstanceARB for MDI (§3.2)
};

struct FrameStats {
    u32 draws = 0;
    u32 passes = 0;
    u32 commands = 0;
    usize geometry_bytes = 0;
    usize uniform_bytes = 0;
    u32 ring_overflows = 0;
};

class RenderDevice : public app::IFrameFenceSource {
public:
    ~RenderDevice() override = default;

    // ---- resources: creation is immediate, destruction is deferred until the GPU is done with
    // every frame that could reference the object (MCE_FRAMES_IN_FLIGHT frames later).
    virtual BufferHandle create_buffer(const BufferDesc& desc, Span<const byte> initial_data = {}) = 0;
    virtual void update_buffer(BufferHandle buffer, u64 offset, Span<const byte> data) = 0;
    // `initial_data` fills mip 0 with tightly packed rows; row 0 is sampled at v = 0.
    virtual TextureHandle create_texture(const TextureDesc& desc, Span<const byte> initial_data = {}) = 0;
    virtual SamplerHandle create_sampler(const SamplerDesc& desc) = 0;
    virtual Result<PipelineHandle> create_pipeline(const GraphicsPipelineDesc& desc) = 0;
    virtual void destroy_buffer(BufferHandle) = 0;
    virtual void destroy_texture(TextureHandle) = 0;
    virtual void destroy_sampler(SamplerHandle) = 0;
    virtual void destroy_pipeline(PipelineHandle) = 0;

    virtual bool is_alive(BufferHandle) const = 0;
    virtual bool is_alive(TextureHandle) const = 0;
    virtual bool is_alive(PipelineHandle) const = 0;
    virtual Extent2D texture_size(TextureHandle) const = 0;

    // ---- frames
    virtual void begin_frame(u64 frame_index) = 0;
    virtual void submit(const CommandBuffer& cmd) = 0;
    virtual void end_frame() = 0;
    virtual void present(bool vsync) = 0;

    // The current window backbuffer size in pixels.
    virtual Extent2D backbuffer_size() const = 0;

    // Reads back RGBA8 pixels of a color target (null = backbuffer, before present()). Rows are
    // returned top row first. Golden-image and test plumbing (FR-DBG-020); stalls the GPU.
    virtual ResultV read_pixels(TextureHandle target, Rect2D rect, Span<u8> out_rgba8) = 0;

    virtual const DeviceCaps& caps() const = 0;
    virtual const FrameStats& last_frame_stats() const = 0;
    virtual usize texture_memory_bytes() const = 0;
    // Errors reported by the API validation layer (GL_KHR_debug) since device creation.
    virtual u32 validation_error_count() const = 0;
};

}  // namespace mce::rhi
