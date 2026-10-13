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
    usize indirect_ring_bytes = 256 * KiB;  // per frame slot (§3.1)
};

struct DeviceCaps {
    FixedString<128> api_name;     // "OpenGL 4.5 (Core Profile) Mesa 24.0"
    FixedString<128> device_name;  // GL_RENDERER
    u32 api_major = 0, api_minor = 0;
    u32 max_texture_size = 0;
    u32 max_uniform_block_size = 0;
    bool shader_draw_parameters = false;  // gl_BaseInstanceARB for MDI (§3.2)
    bool timestamp_queries = false;       // CommandBuffer::write_timestamp produces values
    u32 storage_buffer_alignment = 256;   // offset alignment of set_storage_data ranges
    // Compute (M-VISUAL 1): create_compute_pipeline, dispatch, storage images. Backends without it
    // leave this false and refuse compute pipelines; renderers keep their CPU path there.
    bool compute = false;
    u32 max_compute_invocations = 0;         // threads per work group
    u32 max_compute_group_size[3] = {};      // local_size_x/y/z limits
    u32 max_compute_group_count[3] = {};     // dispatch() limits
    u32 max_storage_image_units = 0;         // bind_storage_image units
};

// GPU timestamps of one completed frame (CommandBuffer::write_timestamp). Values are nanoseconds
// on the GPU clock: only differences are meaningful.
struct GpuTimestamps {
    u64 frame_index = ~u64(0);  // the frame they were written in; ~0 = none completed yet
    u32 written = 0;            // bit i set = index i was written that frame
    u64 ns[kMaxTimestamps] = {};

    bool has(u32 i) const { return i < kMaxTimestamps && (written >> i) & 1u; }
    // Milliseconds between two written timestamps, or -1 when either is missing.
    f64 ms(u32 from, u32 to) const { return has(from) && has(to) ? f64(i64(ns[to] - ns[from])) * 1e-6 : -1.0; }
};

struct FrameStats {
    u32 draws = 0;
    u32 passes = 0;
    u32 commands = 0;
    usize geometry_bytes = 0;
    usize uniform_bytes = 0;
    usize indirect_bytes = 0;
    u32 indirect_draws = 0;  // MDI submits (each counts once in `draws` too)
    u32 dispatches = 0;      // compute dispatches (direct and indirect)
    u32 ring_overflows = 0;
};

class RenderDevice : public app::IFrameFenceSource {
public:
    ~RenderDevice() override = default;

    // ---- resources: creation is immediate, destruction is deferred until the GPU is done with
    // every frame that could reference the object (MCE_FRAMES_IN_FLIGHT frames later).
    virtual BufferHandle create_buffer(const BufferDesc& desc, Span<const byte> initial_data = {}) = 0;
    virtual void update_buffer(BufferHandle buffer, u64 offset, Span<const byte> data) = 0;
    // `initial_data` fills mip 0 with tightly packed rows; row 0 is sampled at v = 0. For array
    // textures it holds every layer's mip 0, layers contiguous.
    virtual TextureHandle create_texture(const TextureDesc& desc, Span<const byte> initial_data = {}) = 0;
    // Uploads one mip of one layer (or a sub-rectangle). Uncompressed formats may update any
    // rectangle; block-compressed formats need 4-aligned rectangles. Immediate (DSA upload); the
    // caller must not update a texture a frame still in flight samples unless the race is benign.
    virtual void update_texture(TextureHandle texture, const TextureRegion& region, Span<const byte> data) = 0;
    virtual SamplerHandle create_sampler(const SamplerDesc& desc) = 0;
    virtual Result<PipelineHandle> create_pipeline(const GraphicsPipelineDesc& desc) = 0;
    // Compute pipelines are bound with CommandBuffer::bind_pipeline like graphics ones and destroyed
    // with destroy_pipeline. Errc::Unsupported when caps().compute is false.
    virtual Result<PipelineHandle> create_compute_pipeline(const ComputePipelineDesc& desc) {
        return Error(Errc::Unsupported, "rhi.compute", desc.debug_name);
    }
    virtual void destroy_buffer(BufferHandle) = 0;
    virtual void destroy_texture(TextureHandle) = 0;
    virtual void destroy_sampler(SamplerHandle) = 0;
    virtual void destroy_pipeline(PipelineHandle) = 0;

    virtual bool is_alive(BufferHandle) const = 0;
    virtual bool is_alive(TextureHandle) const = 0;
    virtual bool is_alive(PipelineHandle) const = 0;
    virtual Extent2D texture_size(TextureHandle) const = 0;
    virtual TextureDesc texture_desc(TextureHandle) const = 0;  // debug_name is not kept

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
    // Reads back buffer bytes [offset, offset + out.size()) after every submitted command (compute
    // results). Test and tooling plumbing; stalls the GPU.
    virtual ResultV read_buffer(BufferHandle buffer, u64 offset, Span<byte> out) {
        (void)buffer, (void)offset, (void)out;
        return Error(Errc::Unsupported, "rhi.read_buffer", "backend has no buffer readback");
    }

    virtual const DeviceCaps& caps() const = 0;
    virtual const FrameStats& last_frame_stats() const = 0;
    // The newest frame whose timestamps the GPU has finished (MCE_FRAMES_IN_FLIGHT behind).
    virtual const GpuTimestamps& last_gpu_timestamps() const = 0;
    virtual usize texture_memory_bytes() const = 0;
    // Errors reported by the API validation layer (GL_KHR_debug) since device creation.
    virtual u32 validation_error_count() const = 0;
};

}  // namespace mce::rhi
