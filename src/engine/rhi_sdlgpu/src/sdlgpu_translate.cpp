// mce_rhi_sdlgpu translate stage: recorded CommandBuffer stream -> SDL GPU (sdlgpu_device.h).
// Walk 1 stages every inline payload and uploads it in one copy pass; walk 2 records the passes.

#include <cstring>

#include "mce/log.h"
#include "mce/math/math.h"
#include "sdlgpu_device.h"

namespace mce::rhi::sdlgpu {
namespace {

SDL_GPUVertexElementFormat sdl_vertex_format(VertexFormat f) {
    switch (f) {
        case VertexFormat::Float1: return SDL_GPU_VERTEXELEMENTFORMAT_FLOAT;
        case VertexFormat::Float2: return SDL_GPU_VERTEXELEMENTFORMAT_FLOAT2;
        case VertexFormat::Float3: return SDL_GPU_VERTEXELEMENTFORMAT_FLOAT3;
        case VertexFormat::Float4: return SDL_GPU_VERTEXELEMENTFORMAT_FLOAT4;
        case VertexFormat::UNorm8x4: return SDL_GPU_VERTEXELEMENTFORMAT_UBYTE4_NORM;
        case VertexFormat::UInt1: return SDL_GPU_VERTEXELEMENTFORMAT_UINT;
        case VertexFormat::SNorm16x2: return SDL_GPU_VERTEXELEMENTFORMAT_SHORT2_NORM;
        case VertexFormat::SNorm16x4: return SDL_GPU_VERTEXELEMENTFORMAT_SHORT4_NORM;
        case VertexFormat::UNorm16x2: return SDL_GPU_VERTEXELEMENTFORMAT_USHORT2_NORM;
    }
    return SDL_GPU_VERTEXELEMENTFORMAT_FLOAT4;
}

SDL_GPUCompareOp sdl_compare(CompareOp op) {
    switch (op) {
        case CompareOp::Never: return SDL_GPU_COMPAREOP_NEVER;
        case CompareOp::Less: return SDL_GPU_COMPAREOP_LESS;
        case CompareOp::Equal: return SDL_GPU_COMPAREOP_EQUAL;
        case CompareOp::LessEqual: return SDL_GPU_COMPAREOP_LESS_OR_EQUAL;
        case CompareOp::Greater: return SDL_GPU_COMPAREOP_GREATER;
        case CompareOp::NotEqual: return SDL_GPU_COMPAREOP_NOT_EQUAL;
        case CompareOp::GreaterEqual: return SDL_GPU_COMPAREOP_GREATER_OR_EQUAL;
        case CompareOp::Always: return SDL_GPU_COMPAREOP_ALWAYS;
    }
    return SDL_GPU_COMPAREOP_LESS_OR_EQUAL;
}

SDL_GPUPrimitiveType sdl_topology(PrimitiveTopology t) {
    switch (t) {
        case PrimitiveTopology::Triangles: return SDL_GPU_PRIMITIVETYPE_TRIANGLELIST;
        case PrimitiveTopology::Lines: return SDL_GPU_PRIMITIVETYPE_LINELIST;
        case PrimitiveTopology::LineStrip: return SDL_GPU_PRIMITIVETYPE_LINESTRIP;
        case PrimitiveTopology::Points: return SDL_GPU_PRIMITIVETYPE_POINTLIST;
    }
    return SDL_GPU_PRIMITIVETYPE_TRIANGLELIST;
}

SDL_GPUColorTargetBlendState sdl_blend(BlendMode m) {
    SDL_GPUColorTargetBlendState b{};
    b.color_blend_op = SDL_GPU_BLENDOP_ADD;
    b.alpha_blend_op = SDL_GPU_BLENDOP_ADD;
    b.src_color_blendfactor = b.src_alpha_blendfactor = SDL_GPU_BLENDFACTOR_ONE;
    b.dst_color_blendfactor = b.dst_alpha_blendfactor = SDL_GPU_BLENDFACTOR_ZERO;
    switch (m) {
        case BlendMode::Opaque: break;
        case BlendMode::PremultipliedAlpha:
            b.enable_blend = true;
            b.dst_color_blendfactor = b.dst_alpha_blendfactor = SDL_GPU_BLENDFACTOR_ONE_MINUS_SRC_ALPHA;
            break;
        case BlendMode::Alpha:
            b.enable_blend = true;
            b.src_color_blendfactor = SDL_GPU_BLENDFACTOR_SRC_ALPHA;
            b.dst_color_blendfactor = b.dst_alpha_blendfactor = SDL_GPU_BLENDFACTOR_ONE_MINUS_SRC_ALPHA;
            break;
        case BlendMode::Additive:
            b.enable_blend = true;
            b.dst_color_blendfactor = b.dst_alpha_blendfactor = SDL_GPU_BLENDFACTOR_ONE;
            break;
    }
    return b;
}

SDL_GPULoadOp sdl_load(LoadOp op) {
    switch (op) {
        case LoadOp::Load: return SDL_GPU_LOADOP_LOAD;
        case LoadOp::Clear: return SDL_GPU_LOADOP_CLEAR;
        case LoadOp::DontCare: return SDL_GPU_LOADOP_DONT_CARE;
    }
    return SDL_GPU_LOADOP_LOAD;
}

bool has_stencil(SDL_GPUTextureFormat f) {
    return f == SDL_GPU_TEXTUREFORMAT_D24_UNORM_S8_UINT || f == SDL_GPU_TEXTUREFORMAT_D32_FLOAT_S8_UINT;
}

bool carries_payload(CmdType t) {
    return t == CmdType::SetVertexData || t == CmdType::SetIndexData || t == CmdType::SetStorageData ||
           t == CmdType::SetDrawArgs;
}

}  // namespace

// ---- walk 1: payloads -> transfer buffer -> one copy pass ------------------------------------------

void SdlGpuDevice::prepare_payloads(const CommandBuffer& cb, SDL_GPUCommandBuffer* gcb) {
    SlotRings& ring = rings_[slot_];
    prepared_.clear();
    const usize geo_cap = align_up(desc_.geometry_ring_bytes, 256);
    const usize ind_cap = align_up(desc_.indirect_ring_bytes, 256);
    const usize geo_start = ring.geometry_head, ind_start = ring.indirect_head;
    storage_uploads_.clear();

    u8* map = nullptr;
    bool overflow_logged = false;
    auto overflow = [&](const char* what, usize size) {
        ++stats_.ring_overflows;
        if (!overflow_logged) {
            MCE_LOG_WARN("rhi.sdlgpu: {} ring overflow ({} bytes requested); commands dropped", what, size);
            overflow_logged = true;
        }
    };

    const CmdHeader* h = nullptr;
    const void* payload = nullptr;
    for (CommandBuffer::Reader r = cb.reader(); r.next(h, payload);) {
        if (!carries_payload(h->type)) continue;
        if (!map) map = static_cast<u8*>(SDL_MapGPUTransferBuffer(device_, ring.transfer, false));
        Prepared prep;
        switch (h->type) {
            case CmdType::SetVertexData:
            case CmdType::SetIndexData: {
                const u32 size = h->type == CmdType::SetVertexData ? static_cast<const cmd::SetVertexData*>(payload)->size
                                                                    : static_cast<const cmd::SetIndexData*>(payload)->size;
                const Span<const byte> data = h->type == CmdType::SetVertexData
                                                  ? CommandBuffer::inline_data(static_cast<const cmd::SetVertexData*>(payload), size)
                                                  : CommandBuffer::inline_data(static_cast<const cmd::SetIndexData*>(payload), size);
                const usize at = align_up(ring.geometry_head, 16);
                if (at + size > geo_cap) {
                    overflow("geometry", size);
                    break;
                }
                std::memcpy(map + at, data.data(), size);
                ring.geometry_head = at + size;
                prep = Prepared{ring.geometry, static_cast<u32>(at), true};
                break;
            }
            case CmdType::SetDrawArgs: {
                const auto* c = static_cast<const cmd::SetDrawArgs*>(payload);
                const u32 size = static_cast<u32>(c->count * sizeof(DrawIndexedIndirectCommand));
                const usize at = align_up(ring.indirect_head, 16);
                if (at + size > ind_cap) {
                    overflow("indirect", size);
                    break;
                }
                std::memcpy(map + geo_cap + at, CommandBuffer::inline_data(c, size).data(), size);
                ring.indirect_head = at + size;
                prep = Prepared{ring.indirect, static_cast<u32>(at), true};
                break;
            }
            case CmdType::SetStorageData: {
                const auto* c = static_cast<const cmd::SetStorageData*>(payload);
                const usize at = align_up(ring.storage_head, 16);
                if (at + c->size > geo_cap) {
                    overflow("storage", c->size);
                    break;
                }
                SDL_GPUBuffer* chunk = storage_chunk(ring, c->size);
                if (!chunk) break;
                std::memcpy(map + geo_cap + ind_cap + at, CommandBuffer::inline_data(c, c->size).data(), c->size);
                ring.storage_head = at + c->size;
                storage_uploads_.push_back(StorageUpload{chunk, static_cast<u32>(geo_cap + ind_cap + at), c->size});
                prep = Prepared{chunk, 0, true};
                break;
            }
            default: break;
        }
        prepared_.push_back(prep);
    }
    if (!map) return;
    SDL_UnmapGPUTransferBuffer(device_, ring.transfer);

    SDL_GPUCopyPass* copy = SDL_BeginGPUCopyPass(gcb);
    if (ring.geometry_head > geo_start) {
        const SDL_GPUTransferBufferLocation src{ring.transfer, static_cast<Uint32>(geo_start)};
        const SDL_GPUBufferRegion dst{ring.geometry, static_cast<Uint32>(geo_start),
                                      static_cast<Uint32>(ring.geometry_head - geo_start)};
        SDL_UploadToGPUBuffer(copy, &src, &dst, false);
    }
    if (ring.indirect_head > ind_start) {
        const SDL_GPUTransferBufferLocation src{ring.transfer, static_cast<Uint32>(geo_cap + ind_start)};
        const SDL_GPUBufferRegion dst{ring.indirect, static_cast<Uint32>(ind_start),
                                      static_cast<Uint32>(ring.indirect_head - ind_start)};
        SDL_UploadToGPUBuffer(copy, &src, &dst, false);
    }
    for (const StorageUpload& u : storage_uploads_) {
        const SDL_GPUTransferBufferLocation src{ring.transfer, u.staged};
        const SDL_GPUBufferRegion dst{u.buffer, 0, u.size};
        SDL_UploadToGPUBuffer(copy, &src, &dst, false);
    }
    SDL_EndGPUCopyPass(copy);
}

// ---- walk 2: passes ------------------------------------------------------------------------------

SDL_GPUGraphicsPipeline* SdlGpuDevice::pipeline_variant(GpuPipeline& p) {
    for (const PipelineVariant& v : p.variants)
        if (v.key == ts_.target) return v.pipeline;

    SDL_GPUVertexBufferDescription vb{};
    vb.slot = 0;
    vb.pitch = p.stride;
    vb.input_rate = SDL_GPU_VERTEXINPUTRATE_VERTEX;
    SDL_GPUVertexAttribute attrs[16];
    for (u32 i = 0; i < p.attr_count; ++i) {
        attrs[i].location = p.attrs[i].location;
        attrs[i].buffer_slot = 0;
        attrs[i].format = sdl_vertex_format(p.attrs[i].format);
        attrs[i].offset = p.attrs[i].offset;
    }
    SDL_GPUColorTargetDescription colors[cmd::kMaxColorAttachments];
    for (u32 i = 0; i < ts_.target.color_count; ++i) {
        colors[i] = SDL_GPUColorTargetDescription{};
        colors[i].format = ts_.target.colors[i];
        colors[i].blend_state = sdl_blend(p.state.blend.mode);
    }

    SDL_GPUGraphicsPipelineCreateInfo ci{};
    ci.vertex_shader = p.vs;
    ci.fragment_shader = p.fs;
    if (p.stride > 0 && p.attr_count > 0) {
        ci.vertex_input_state.vertex_buffer_descriptions = &vb;
        ci.vertex_input_state.num_vertex_buffers = 1;
        ci.vertex_input_state.vertex_attributes = attrs;
        ci.vertex_input_state.num_vertex_attributes = p.attr_count;
    }
    ci.primitive_type = sdl_topology(p.state.topology);
    const RasterState& rs = p.state.raster;
    ci.rasterizer_state.fill_mode = rs.fill == FillMode::Wireframe ? SDL_GPU_FILLMODE_LINE : SDL_GPU_FILLMODE_FILL;
    ci.rasterizer_state.cull_mode = rs.cull == CullMode::None    ? SDL_GPU_CULLMODE_NONE
                                    : rs.cull == CullMode::Front ? SDL_GPU_CULLMODE_FRONT
                                                                 : SDL_GPU_CULLMODE_BACK;
    // The vertex epilogue negates y (shader_spirv.h), which mirrors the winding: swap the front face.
    ci.rasterizer_state.front_face = rs.front_ccw ? SDL_GPU_FRONTFACE_CLOCKWISE : SDL_GPU_FRONTFACE_COUNTER_CLOCKWISE;
    ci.rasterizer_state.enable_depth_clip = true;
    ci.multisample_state.sample_count = ts_.target.samples == 8   ? SDL_GPU_SAMPLECOUNT_8
                                        : ts_.target.samples == 4 ? SDL_GPU_SAMPLECOUNT_4
                                        : ts_.target.samples == 2 ? SDL_GPU_SAMPLECOUNT_2
                                                                  : SDL_GPU_SAMPLECOUNT_1;
    const DepthState& ds = p.state.depth;
    ci.depth_stencil_state.enable_depth_test = ds.test && ts_.target.has_depth;
    ci.depth_stencil_state.enable_depth_write = ds.write && ts_.target.has_depth;
    ci.depth_stencil_state.compare_op = ds.test ? sdl_compare(ds.compare) : SDL_GPU_COMPAREOP_ALWAYS;
    ci.target_info.color_target_descriptions = colors;
    ci.target_info.num_color_targets = ts_.target.color_count;
    ci.target_info.has_depth_stencil_target = ts_.target.has_depth;
    ci.target_info.depth_stencil_format = ts_.target.depth;

    SDL_GPUGraphicsPipeline* gp = SDL_CreateGPUGraphicsPipeline(device_, &ci);
    if (!gp) {
        MCE_LOG_ERROR("rhi.sdlgpu: pipeline '{}' variant failed: {}", p.name.c_str(), SDL_GetError());
        ++validation_errors_;
        return nullptr;
    }
    p.variants.push_back(PipelineVariant{ts_.target, gp});
    return gp;
}

void SdlGpuDevice::begin_pass(SDL_GPUCommandBuffer* gcb, const cmd::BeginPass& p) {
    ++stats_.passes;
    const bool backbuffer =
        p.color_count == 0 ? (!p.has_depth || p.depth.texture.is_null()) : p.colors[0].texture.is_null();

    SDL_GPUColorTargetInfo colors[cmd::kMaxColorAttachments] = {};
    TargetKey key;
    Extent2D size;
    for (u32 i = 0; i < p.color_count; ++i) {
        SDL_GPUColorTargetInfo& c = colors[i];
        const GpuTexture* t = textures_.try_get(p.colors[i].texture);
        if (p.colors[i].texture.is_null()) {
            c.texture = backbuffer_;
            key.colors[i] = SDL_GPU_TEXTUREFORMAT_R8G8B8A8_UNORM;
            if (i == 0) size = backbuffer_extent_;
        } else if (t) {
            c.texture = t->texture;
            c.layer_or_depth_plane = p.colors[i].layer;
            key.colors[i] = t->sdl_format;
            key.samples = t->samples;
            if (i == 0) size = t->size;
        }
        c.load_op = sdl_load(p.colors[i].load);
        c.store_op = p.colors[i].store == StoreOp::Store ? SDL_GPU_STOREOP_STORE : SDL_GPU_STOREOP_DONT_CARE;
        c.clear_color = SDL_FColor{p.colors[i].clear.color[0], p.colors[i].clear.color[1], p.colors[i].clear.color[2],
                                   p.colors[i].clear.color[3]};
    }
    key.color_count = static_cast<u8>(p.color_count);

    SDL_GPUDepthStencilTargetInfo depth{};
    if (p.has_depth) {
        if (p.depth.texture.is_null()) {
            depth.texture = backbuffer_depth_;
            key.depth = default_depth_format_;
            if (p.color_count == 0) size = backbuffer_extent_;
        } else if (const GpuTexture* t = textures_.try_get(p.depth.texture)) {
            depth.texture = t->texture;
            depth.layer = static_cast<Uint8>(p.depth.layer);
            key.depth = t->sdl_format;
            key.samples = t->samples;
            if (p.color_count == 0) size = t->size;
        }
        key.has_depth = depth.texture != nullptr;
        depth.load_op = sdl_load(p.depth.load);
        depth.store_op = p.depth.store == StoreOp::Store ? SDL_GPU_STOREOP_STORE : SDL_GPU_STOREOP_DONT_CARE;
        depth.clear_depth = p.depth.clear.depth;
        depth.clear_stencil = p.depth.clear.stencil;
        const bool stencil = has_stencil(key.depth);
        depth.stencil_load_op = stencil ? depth.load_op : SDL_GPU_LOADOP_DONT_CARE;
        depth.stencil_store_op = stencil ? depth.store_op : SDL_GPU_STOREOP_DONT_CARE;
    }
    (void)backbuffer;

    ts_.pass_scope = p.debug_name && *p.debug_name;
    if (ts_.pass_scope) SDL_PushGPUDebugGroup(gcb, p.debug_name);
    ts_.pass = SDL_BeginGPURenderPass(gcb, colors, p.color_count, key.has_depth ? &depth : nullptr);
    ts_.target = key;
    ts_.bound = nullptr;
    if (!ts_.pass) {
        MCE_LOG_ERROR("rhi.sdlgpu: render pass '{}' failed: {}", p.debug_name ? p.debug_name : "", SDL_GetError());
        ++validation_errors_;
        return;
    }
    // Rect2D is top-left based; targets hold GL's layout, whose row 0 is the bottom.
    Rect2D vp = p.viewport;
    if (vp.width == 0 || vp.height == 0) vp = Rect2D{0, 0, size.width, size.height};
    SDL_GPUViewport v{};
    v.x = static_cast<f32>(vp.x);
    v.y = static_cast<f32>(static_cast<i32>(size.height) - vp.y - static_cast<i32>(vp.height));
    v.w = static_cast<f32>(vp.width);
    v.h = static_cast<f32>(vp.height);
    v.min_depth = 0.0f;
    v.max_depth = 1.0f;
    SDL_SetGPUViewport(ts_.pass, &v);
}

void SdlGpuDevice::end_pass() {
    if (ts_.pass) SDL_EndGPURenderPass(ts_.pass);
    ts_.pass = nullptr;
    ts_.bound = nullptr;
}

// Binds the pipeline variant for the current pass and every resource its stages declare.
bool SdlGpuDevice::flush_draw_state(SDL_GPUCommandBuffer* /*gcb*/) {
    GpuPipeline* p = ts_.pipeline;
    if (!p || !ts_.pass) {
        MCE_ASSERT_MSG(p, "draw without a bound pipeline");
        return false;
    }
    SDL_GPUGraphicsPipeline* gp = pipeline_variant(*p);
    if (!gp) return false;
    if (gp != ts_.bound) {
        SDL_BindGPUGraphicsPipeline(ts_.pass, gp);
        ts_.bound = gp;
    }
    auto bind_stage = [&](const StageResources& res, bool vertex) {
        if (res.sampler_count) {
            SDL_GPUTextureSamplerBinding binds[kMaxStageSamplers];
            for (u32 i = 0; i < res.sampler_count; ++i) {
                const BoundTexture& b = textures_bound_[res.sampler_slot[i]];
                const GpuTexture* t = textures_.try_get(b.texture);
                const GpuSampler* s = samplers_.try_get(b.sampler);
                const auto kind = static_cast<u32>(res.sampler_kind[i]);
                const bool shadow = res.sampler_kind[i] == SamplerKind::Shadow2D || res.sampler_kind[i] == SamplerKind::Shadow2DArray;
                binds[i].texture = t ? t->texture : dummy_tex_[kind];
                binds[i].sampler = s ? s->sampler : (shadow ? dummy_compare_sampler_ : dummy_sampler_);
            }
            if (vertex) SDL_BindGPUVertexSamplers(ts_.pass, 0, binds, res.sampler_count);
            else SDL_BindGPUFragmentSamplers(ts_.pass, 0, binds, res.sampler_count);
        }
        if (res.storage_count) {
            SDL_GPUBuffer* bufs[kMaxStageStorage];
            for (u32 i = 0; i < res.storage_count; ++i) {
                SDL_GPUBuffer* b = storage_bound_[res.storage_binding[i]];
                bufs[i] = b ? b : dummy_storage_;
            }
            if (vertex) SDL_BindGPUVertexStorageBuffers(ts_.pass, 0, bufs, res.storage_count);
            else SDL_BindGPUFragmentStorageBuffers(ts_.pass, 0, bufs, res.storage_count);
        }
    };
    bind_stage(p->vres, true);
    bind_stage(p->fres, false);
    if (p->stride > 0 && p->attr_count > 0) {
        if (!ts_.vb_valid) return false;  // vertex data overflowed or never set: skip
        const SDL_GPUBufferBinding vb{ts_.vb, ts_.vb_offset};
        SDL_BindGPUVertexBuffers(ts_.pass, 0, &vb, 1);
    }
    return true;
}

void SdlGpuDevice::resolve(SDL_GPUCommandBuffer* gcb, const cmd::Resolve& r) {
    const GpuTexture* src = textures_.try_get(r.src);
    const GpuTexture* dst = textures_.try_get(r.dst);
    const bool valid = src && dst && src->size == dst->size && src->format == dst->format && !is_depth_format(src->format);
    MCE_ASSERT_MSG(valid, "resolve: needs two live color targets of the same size and format");
    if (!valid) return;
    SDL_GPUColorTargetInfo c{};
    c.texture = src->texture;
    c.load_op = SDL_GPU_LOADOP_LOAD;
    c.store_op = SDL_GPU_STOREOP_RESOLVE_AND_STORE;
    c.resolve_texture = dst->texture;
    if (src->samples == 1) {  // GL blits single-sample targets too: copy instead
        SDL_GPUBlitInfo bi{};
        bi.source.texture = src->texture;
        bi.source.w = src->size.width;
        bi.source.h = src->size.height;
        bi.destination.texture = dst->texture;
        bi.destination.w = dst->size.width;
        bi.destination.h = dst->size.height;
        bi.load_op = SDL_GPU_LOADOP_DONT_CARE;
        bi.filter = SDL_GPU_FILTER_NEAREST;
        SDL_BlitGPUTexture(gcb, &bi);
        return;
    }
    SDL_GPURenderPass* pass = SDL_BeginGPURenderPass(gcb, &c, 1, nullptr);
    if (pass) SDL_EndGPURenderPass(pass);
}

void SdlGpuDevice::record_passes(const CommandBuffer& cb, SDL_GPUCommandBuffer* gcb) {
    ts_.pipeline = nullptr;
    ts_.bound = nullptr;
    ts_.pass = nullptr;
    ts_.vb_valid = ts_.ib_valid = false;
    ts_.args = nullptr;
    ts_.args_count = 0;
    ts_.scopes = 0;
    // Uniform pushes live in the command buffer; GL keeps bank bindings across submits.
    for (u32 b = 0; b < kMaxUniformBanks; ++b) {
        if (!uniform_set_[b]) continue;
        const auto size = static_cast<Uint32>(uniform_data_[b].size());
        SDL_PushGPUVertexUniformData(gcb, b, uniform_data_[b].data(), size);
        SDL_PushGPUFragmentUniformData(gcb, b, uniform_data_[b].data(), size);
    }

    usize next_prepared = 0;
    const CmdHeader* h = nullptr;
    const void* payload = nullptr;
    for (CommandBuffer::Reader r = cb.reader(); r.next(h, payload);) {
        ++stats_.commands;
        const Prepared prep = carries_payload(h->type) && next_prepared < prepared_.size() ? prepared_[next_prepared++]
                                                                                            : Prepared{};
        switch (h->type) {
            case CmdType::BeginPass: begin_pass(gcb, *static_cast<const cmd::BeginPass*>(payload)); break;
            case CmdType::EndPass:
                end_pass();
                if (ts_.pass_scope) SDL_PopGPUDebugGroup(gcb);
                ts_.pass_scope = false;
                break;
            case CmdType::BindPipeline: {
                GpuPipeline* p = pipelines_.try_get(static_cast<const cmd::BindPipeline*>(payload)->pipeline);
                MCE_ASSERT_MSG(p, "bind_pipeline: stale handle");
                ts_.pipeline = p;
                break;
            }
            case CmdType::SetUniforms: {
                const auto* c = static_cast<const cmd::SetUniforms*>(payload);
                if (c->bank >= kMaxUniformBanks) break;
                const Span<const byte> data = CommandBuffer::inline_data(c, c->size);
                uniform_data_[c->bank].resize(c->size);
                std::memcpy(uniform_data_[c->bank].data(), data.data(), c->size);
                uniform_set_[c->bank] = true;
                SDL_PushGPUVertexUniformData(gcb, c->bank, data.data(), c->size);
                SDL_PushGPUFragmentUniformData(gcb, c->bank, data.data(), c->size);
                stats_.uniform_bytes += c->size;
                break;
            }
            case CmdType::BindTexture: {
                const auto* c = static_cast<const cmd::BindTexture*>(payload);
                if (c->slot < kMaxStageSamplers) textures_bound_[c->slot] = BoundTexture{c->texture, c->sampler};
                break;
            }
            case CmdType::BindStorageBuffer: {
                const auto* c = static_cast<const cmd::BindStorageBuffer*>(payload);
                const GpuBuffer* b = buffers_.try_get(c->buffer);
                if (!b || c->binding >= kMaxStageStorage) break;
                if (c->offset != 0 && !warned_storage_offset_) {
                    MCE_LOG_WARN("rhi.sdlgpu: bind_storage_buffer offsets are not supported; binding the whole buffer");
                    warned_storage_offset_ = true;
                }
                storage_bound_[c->binding] = b->buffer;
                break;
            }
            case CmdType::SetStorageData: {
                const auto* c = static_cast<const cmd::SetStorageData*>(payload);
                if (prep.ok && c->binding < kMaxStageStorage) storage_bound_[c->binding] = prep.buffer;
                break;
            }
            case CmdType::SetVertexData:
                ts_.vb_valid = prep.ok;
                ts_.vb = prep.buffer;
                ts_.vb_offset = prep.offset;
                break;
            case CmdType::SetIndexData:
                ts_.ib_valid = prep.ok;
                ts_.ib = prep.buffer;
                ts_.ib_offset = prep.offset;
                ts_.ib_format = static_cast<const cmd::SetIndexData*>(payload)->format;
                break;
            case CmdType::BindVertexBuffer: {
                const auto* c = static_cast<const cmd::BindVertexBuffer*>(payload);
                const GpuBuffer* b = buffers_.try_get(c->buffer);
                ts_.vb_valid = b != nullptr;
                ts_.vb = b ? b->buffer : nullptr;
                ts_.vb_offset = static_cast<u32>(c->offset);
                break;
            }
            case CmdType::BindIndexBuffer: {
                const auto* c = static_cast<const cmd::BindIndexBuffer*>(payload);
                const GpuBuffer* b = buffers_.try_get(c->buffer);
                ts_.ib_valid = b != nullptr;
                ts_.ib = b ? b->buffer : nullptr;
                ts_.ib_offset = static_cast<u32>(c->offset);
                ts_.ib_format = c->format;
                break;
            }
            case CmdType::Draw: {
                const auto* d = static_cast<const cmd::Draw*>(payload);
                if (!ts_.ib_valid || !flush_draw_state(gcb)) break;
                const SDL_GPUBufferBinding ib{ts_.ib, ts_.ib_offset};
                SDL_BindGPUIndexBuffer(ts_.pass, &ib,
                                       ts_.ib_format == IndexFormat::U16 ? SDL_GPU_INDEXELEMENTSIZE_16BIT
                                                                         : SDL_GPU_INDEXELEMENTSIZE_32BIT);
                SDL_DrawGPUIndexedPrimitives(ts_.pass, d->index_count, d->instance_count, d->first_index, d->base_vertex,
                                             d->base_instance);
                ++stats_.draws;
                break;
            }
            case CmdType::DrawNonIndexed: {
                const auto* d = static_cast<const cmd::DrawNonIndexed*>(payload);
                if (!flush_draw_state(gcb)) break;
                SDL_DrawGPUPrimitives(ts_.pass, d->vertex_count, d->instance_count, d->first_vertex, d->base_instance);
                ++stats_.draws;
                break;
            }
            case CmdType::SetDrawArgs:
                ts_.args = prep.ok ? prep.buffer : nullptr;
                ts_.args_offset = prep.offset;
                ts_.args_count = prep.ok ? static_cast<const cmd::SetDrawArgs*>(payload)->count : 0;
                break;
            case CmdType::DrawIndirect: {
                const auto* d = static_cast<const cmd::DrawIndirect*>(payload);
                MCE_ASSERT_MSG(ts_.args && u64(d->first_command) + d->draw_count <= ts_.args_count,
                               "draw_indirect: commands out of range of the last set_draw_args");
                if (!ts_.args || u64(d->first_command) + d->draw_count > ts_.args_count || d->draw_count == 0) break;
                if (!ts_.ib_valid || !flush_draw_state(gcb)) break;
                // The index binding carries the data offset, so first_index needs no rebase (unlike GL).
                const SDL_GPUBufferBinding ib{ts_.ib, ts_.ib_offset};
                SDL_BindGPUIndexBuffer(ts_.pass, &ib,
                                       ts_.ib_format == IndexFormat::U16 ? SDL_GPU_INDEXELEMENTSIZE_16BIT
                                                                         : SDL_GPU_INDEXELEMENTSIZE_32BIT);
                SDL_DrawGPUIndexedPrimitivesIndirect(
                    ts_.pass, ts_.args,
                    static_cast<Uint32>(ts_.args_offset + usize(d->first_command) * sizeof(DrawIndexedIndirectCommand)),
                    d->draw_count);
                ++stats_.draws;
                ++stats_.indirect_draws;
                break;
            }
            case CmdType::PushDebugScope:
                SDL_PushGPUDebugGroup(gcb, static_cast<const cmd::PushDebugScope*>(payload)->name);
                ++ts_.scopes;
                break;
            case CmdType::PopDebugScope:
                if (ts_.scopes > 0) {
                    SDL_PopGPUDebugGroup(gcb);
                    --ts_.scopes;
                }
                break;
            case CmdType::Resolve: resolve(gcb, *static_cast<const cmd::Resolve*>(payload)); break;
            case CmdType::WriteTimestamp: break;  // no timestamp queries in SDL GPU (caps)
        }
    }
    if (ts_.pass) end_pass();
    for (; ts_.scopes > 0; --ts_.scopes) SDL_PopGPUDebugGroup(gcb);
}

}  // namespace mce::rhi::sdlgpu
