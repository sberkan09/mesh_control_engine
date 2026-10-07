#pragma once
// CommandBuffer — a recorded, translatable command stream (rhi-renderer.md §2.3).
//
// Recording is POD-only and backend-neutral: commands and their payloads (uniform blocks, vertex
// and index data) are appended to one byte stream. RenderDevice::submit() translates the stream to
// API calls and copies payloads into its per-slot staging rings. Because recording touches no API,
// a worker-thread recorder needs no interface change.
//
//   rhi::CommandBuffer cmd(mem::frame());
//   cmd.begin_pass(pass);
//   cmd.bind_pipeline(pipe);
//   cmd.set_uniforms(rhi::bank::kFrame, as_bytes(frame_u));
//   cmd.set_vertex_data(as_bytes(verts));
//   cmd.set_index_data(as_bytes(indices), rhi::IndexFormat::U16);
//   cmd.draw(3, 1, 0, 0);
//   cmd.end_pass();
//   device.submit(cmd);

#include "mce/containers/array.h"
#include "mce/rhi/types.h"

namespace mce::rhi {

enum class CmdType : u16 {
    BeginPass,
    EndPass,
    BindPipeline,
    SetUniforms,
    BindTexture,
    BindStorageBuffer,
    SetVertexData,
    SetIndexData,
    BindVertexBuffer,
    BindIndexBuffer,
    Draw,
    DrawNonIndexed,
    SetDrawArgs,
    DrawIndirect,
    PushDebugScope,
    PopDebugScope,
};

const char* cmd_type_name(CmdType t);

// Every command starts with this header; `size` covers header + payload + inline data, 16-aligned.
struct CmdHeader {
    CmdType type;
    u16 reserved;
    u32 size;
};

namespace cmd {
inline constexpr u32 kMaxColorAttachments = 4;

struct BeginPass {
    u32 color_count;
    bool has_depth;
    PassAttachment colors[kMaxColorAttachments];
    PassAttachment depth;
    Rect2D viewport;
    const char* debug_name;
};
struct BindPipeline { PipelineHandle pipeline; };
struct SetUniforms { u32 bank; u32 size; };  // followed by `size` bytes
struct BindTexture { u32 slot; TextureHandle texture; SamplerHandle sampler; };
struct BindStorageBuffer { u32 binding; BufferHandle buffer; u64 offset; u64 size; };
struct SetVertexData { u32 size; };          // followed by `size` bytes
struct SetIndexData { u32 size; IndexFormat format; };  // followed by `size` bytes
struct BindVertexBuffer { BufferHandle buffer; u64 offset; };
struct BindIndexBuffer { BufferHandle buffer; u64 offset; IndexFormat format; };
struct Draw { u32 index_count; u32 instance_count; u32 first_index; i32 base_vertex; u32 base_instance; };
struct DrawNonIndexed { u32 vertex_count; u32 instance_count; u32 first_vertex; u32 base_instance; };
struct SetDrawArgs { u32 count; };            // followed by count * DrawIndexedIndirectCommand
struct DrawIndirect { u32 first_command; u32 draw_count; };
struct PushDebugScope { const char* name; };
}  // namespace cmd

class CommandBuffer {
public:
    // The stream lives in `alloc` (normally the frame arena: mem::frame()).
    explicit CommandBuffer(mem::IAllocator& alloc = mem::heap());

    void begin_pass(const RenderPassDesc& desc);
    void end_pass();
    void bind_pipeline(PipelineHandle pipeline);
    // Copies `data` into the slot's uniform ring and binds it at the bank's fixed binding point.
    void set_uniforms(u32 bank, Span<const byte> data);
    void bind_texture(u32 slot, TextureHandle texture, SamplerHandle sampler);
    void bind_storage_buffer(u32 binding, BufferHandle buffer, u64 offset, u64 size);
    // Transient geometry: copied into the slot's staging ring at submit (valid for this frame).
    void set_vertex_data(Span<const byte> data);
    void set_index_data(Span<const byte> data, IndexFormat format);
    // Persistent geometry from a created buffer.
    void bind_vertex_buffer(BufferHandle buffer, u64 offset = 0);
    void bind_index_buffer(BufferHandle buffer, IndexFormat format, u64 offset = 0);
    void draw(u32 index_count, u32 instance_count = 1, u32 first_index = 0, u32 base_instance = 0,
              i32 base_vertex = 0);
    void draw_non_indexed(u32 vertex_count, u32 instance_count = 1, u32 first_vertex = 0, u32 base_instance = 0);
    // Multi-draw-indirect (rhi-renderer.md §3.2): set_draw_args copies the commands into the
    // slot's indirect ring at submit; draw_indirect issues `draw_count` of them, starting at
    // `first_command`, as ONE MultiDrawElementsIndirect against the bound vertex/index data.
    void set_draw_args(Span<const DrawIndexedIndirectCommand> commands);
    void draw_indirect(u32 draw_count, u32 first_command = 0);
    // No-op on GL; a real barrier on a future explicit backend.
    void barrier() {}
    void push_debug_scope(const char* name);  // `name` must outlive submit (string literal)
    void pop_debug_scope();

    void reset();
    bool empty() const { return stream_.empty(); }
    u32 command_count() const { return count_; }
    Span<const byte> stream() const { return stream_.span(); }
    bool in_pass() const { return in_pass_; }

    // Iterates the recorded commands in order.
    class Reader {
    public:
        explicit Reader(Span<const byte> stream) : stream_(stream) {}
        bool next(const CmdHeader*& header, const void*& payload);

    private:
        Span<const byte> stream_;
        usize offset_ = 0;
    };
    Reader reader() const { return Reader(stream()); }

    // The inline data that follows a payload struct of type P.
    template <class P>
    static Span<const byte> inline_data(const P* payload, u32 size) {
        return {reinterpret_cast<const byte*>(payload) + align_up(sizeof(P), 16), size};
    }

private:
    template <class P>
    void push(CmdType type, const P& payload, Span<const byte> data = {});

    Array<byte> stream_;
    u32 count_ = 0;
    i32 scope_depth_ = 0;
    bool in_pass_ = false;
};

template <class T>
Span<const byte> as_bytes(const T& value) {
    return {reinterpret_cast<const byte*>(&value), sizeof(T)};
}
template <class T>
Span<const byte> as_bytes(Span<T> values) {
    return std::as_bytes(values);
}

}  // namespace mce::rhi
