#include "mce/rhi/command_buffer.h"

#include <cstring>

namespace mce::rhi {

const char* cmd_type_name(CmdType t) {
    switch (t) {
        case CmdType::BeginPass: return "BeginPass";
        case CmdType::EndPass: return "EndPass";
        case CmdType::BindPipeline: return "BindPipeline";
        case CmdType::SetUniforms: return "SetUniforms";
        case CmdType::BindTexture: return "BindTexture";
        case CmdType::BindStorageBuffer: return "BindStorageBuffer";
        case CmdType::SetVertexData: return "SetVertexData";
        case CmdType::SetIndexData: return "SetIndexData";
        case CmdType::BindVertexBuffer: return "BindVertexBuffer";
        case CmdType::BindIndexBuffer: return "BindIndexBuffer";
        case CmdType::Draw: return "Draw";
        case CmdType::DrawNonIndexed: return "DrawNonIndexed";
        case CmdType::SetDrawArgs: return "SetDrawArgs";
        case CmdType::DrawIndirect: return "DrawIndirect";
        case CmdType::PushDebugScope: return "PushDebugScope";
        case CmdType::PopDebugScope: return "PopDebugScope";
    }
    return "?";
}

CommandBuffer::CommandBuffer(mem::IAllocator& alloc) : stream_(alloc) {}

namespace {
struct Empty {};
constexpr usize kHeaderSize = align_up(sizeof(CmdHeader), 16);
constexpr usize max(usize a, usize b, usize c) { return a > b ? (a > c ? a : c) : (b > c ? b : c); }
}  // namespace

template <class P>
void CommandBuffer::push(CmdType type, const P& payload, Span<const byte> data) {
    static_assert(std::is_trivially_copyable_v<P>, "command payloads are POD");
    const usize payload_size = align_up(sizeof(P), 16);
    const usize total = kHeaderSize + payload_size + align_up(data.size(), 16);
    MCE_ASSERT_MSG(total <= 0xFFFFFFFFu, "command too large");
    const usize at = stream_.size();
    // Geometric growth: resize() reserves exactly, which is quadratic on a linear arena.
    if (at + total > stream_.capacity()) stream_.reserve(max(at + total, stream_.capacity() * 2, usize(4 * KiB)));
    stream_.resize(at + total);
    byte* dst = stream_.data() + at;
    std::memset(dst, 0, total);
    CmdHeader h{type, 0, static_cast<u32>(total)};
    std::memcpy(dst, &h, sizeof(h));
    std::memcpy(dst + kHeaderSize, &payload, sizeof(P));
    if (!data.empty()) std::memcpy(dst + kHeaderSize + payload_size, data.data(), data.size());
    ++count_;
}

void CommandBuffer::begin_pass(const RenderPassDesc& desc) {
    MCE_ASSERT_MSG(!in_pass_, "begin_pass inside a pass");
    MCE_ASSERT_MSG(desc.colors.size() <= cmd::kMaxColorAttachments, "too many color attachments");
    cmd::BeginPass p{};
    p.color_count = static_cast<u32>(desc.colors.size() <= cmd::kMaxColorAttachments ? desc.colors.size()
                                                                                       : cmd::kMaxColorAttachments);
    for (u32 i = 0; i < p.color_count; ++i) p.colors[i] = desc.colors[i];
    p.has_depth = desc.depth.has_value();
    if (p.has_depth) p.depth = *desc.depth;
    p.viewport = desc.viewport;
    p.debug_name = desc.debug_name ? desc.debug_name : "";
    push(CmdType::BeginPass, p);
    in_pass_ = true;
}

void CommandBuffer::end_pass() {
    MCE_ASSERT_MSG(in_pass_, "end_pass without begin_pass");
    push(CmdType::EndPass, Empty{});
    in_pass_ = false;
}

void CommandBuffer::bind_pipeline(PipelineHandle pipeline) { push(CmdType::BindPipeline, cmd::BindPipeline{pipeline}); }

void CommandBuffer::set_uniforms(u32 bank, Span<const byte> data) {
    MCE_ASSERT_MSG(bank < bank::kUniformBanks, "uniform bank out of range (0..2)");
    push(CmdType::SetUniforms, cmd::SetUniforms{bank, static_cast<u32>(data.size())}, data);
}

void CommandBuffer::bind_texture(u32 slot, TextureHandle texture, SamplerHandle sampler) {
    push(CmdType::BindTexture, cmd::BindTexture{slot, texture, sampler});
}

void CommandBuffer::bind_storage_buffer(u32 binding, BufferHandle buffer, u64 offset, u64 size) {
    push(CmdType::BindStorageBuffer, cmd::BindStorageBuffer{binding, buffer, offset, size});
}

void CommandBuffer::set_vertex_data(Span<const byte> data) {
    push(CmdType::SetVertexData, cmd::SetVertexData{static_cast<u32>(data.size())}, data);
}

void CommandBuffer::set_index_data(Span<const byte> data, IndexFormat format) {
    push(CmdType::SetIndexData, cmd::SetIndexData{static_cast<u32>(data.size()), format}, data);
}

void CommandBuffer::bind_vertex_buffer(BufferHandle buffer, u64 offset) {
    push(CmdType::BindVertexBuffer, cmd::BindVertexBuffer{buffer, offset});
}

void CommandBuffer::bind_index_buffer(BufferHandle buffer, IndexFormat format, u64 offset) {
    push(CmdType::BindIndexBuffer, cmd::BindIndexBuffer{buffer, offset, format});
}

void CommandBuffer::draw(u32 index_count, u32 instance_count, u32 first_index, u32 base_instance, i32 base_vertex) {
    MCE_ASSERT_MSG(in_pass_, "draw outside a pass");
    push(CmdType::Draw, cmd::Draw{index_count, instance_count, first_index, base_vertex, base_instance});
}

void CommandBuffer::draw_non_indexed(u32 vertex_count, u32 instance_count, u32 first_vertex, u32 base_instance) {
    MCE_ASSERT_MSG(in_pass_, "draw outside a pass");
    push(CmdType::DrawNonIndexed, cmd::DrawNonIndexed{vertex_count, instance_count, first_vertex, base_instance});
}

void CommandBuffer::set_draw_args(Span<const DrawIndexedIndirectCommand> commands) {
    push(CmdType::SetDrawArgs, cmd::SetDrawArgs{static_cast<u32>(commands.size())}, std::as_bytes(commands));
}

void CommandBuffer::draw_indirect(u32 draw_count, u32 first_command) {
    MCE_ASSERT_MSG(in_pass_, "draw outside a pass");
    push(CmdType::DrawIndirect, cmd::DrawIndirect{first_command, draw_count});
}

void CommandBuffer::push_debug_scope(const char* name) {
    ++scope_depth_;
    push(CmdType::PushDebugScope, cmd::PushDebugScope{name ? name : ""});
}

void CommandBuffer::pop_debug_scope() {
    MCE_ASSERT_MSG(scope_depth_ > 0, "pop_debug_scope without push");
    --scope_depth_;
    push(CmdType::PopDebugScope, Empty{});
}

void CommandBuffer::reset() {
    stream_.clear();
    count_ = 0;
    scope_depth_ = 0;
    in_pass_ = false;
}

bool CommandBuffer::Reader::next(const CmdHeader*& header, const void*& payload) {
    if (offset_ + kHeaderSize > stream_.size()) return false;
    header = reinterpret_cast<const CmdHeader*>(stream_.data() + offset_);
    payload = stream_.data() + offset_ + kHeaderSize;
    MCE_ASSERT_MSG(header->size >= kHeaderSize && offset_ + header->size <= stream_.size(), "corrupt command stream");
    offset_ += header->size;
    return true;
}

}  // namespace mce::rhi
