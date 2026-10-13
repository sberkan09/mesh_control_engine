// mce_rhi_metal translate stage: recorded CommandBuffer stream -> Metal encoders (rhi-renderer.md
// §3.4 step 5). Payloads (uniform blocks, transient geometry) are copied into the slot's shared rings.

#include <cstring>

#include "metal_device.h"
#include "mce/log.h"
#include "shader_cross.h"

namespace mce::rhi::metal {
namespace {

MTLPrimitiveType mtl_primitive(PrimitiveTopology t) {
    switch (t) {
        case PrimitiveTopology::Triangles: return MTLPrimitiveTypeTriangle;
        case PrimitiveTopology::Lines: return MTLPrimitiveTypeLine;
        case PrimitiveTopology::LineStrip: return MTLPrimitiveTypeLineStrip;
        case PrimitiveTopology::Points: return MTLPrimitiveTypePoint;
    }
    return MTLPrimitiveTypeTriangle;
}

MTLLoadAction mtl_load(LoadOp op) {
    switch (op) {
        case LoadOp::Load: return MTLLoadActionLoad;
        case LoadOp::Clear: return MTLLoadActionClear;
        case LoadOp::DontCare: return MTLLoadActionDontCare;
    }
    return MTLLoadActionLoad;
}

MTLStoreAction mtl_store(StoreOp op) { return op == StoreOp::Store ? MTLStoreActionStore : MTLStoreActionDontCare; }

usize index_size(IndexFormat f) { return f == IndexFormat::U16 ? 2 : 4; }

}  // namespace

void MetalDevice::reset_translate_state() {
    ts_.pipeline = nullptr;
    ts_.pipeline_ok = false;
    ts_.vb_valid = ts_.ib_valid = false;
    ts_.vb = ts_.ib = nil;
    ts_.draw_args = nullptr;
    ts_.draw_arg_count = 0;
}

void MetalDevice::attach_samples(MTLRenderPassDescriptor* rp) {
    u32 start = 0, end = 0;
    if (!next_encoder_samples(start, end)) return;
    MTLRenderPassSampleBufferAttachmentDescriptor* a = rp.sampleBufferAttachments[0];
    a.sampleBuffer = ts_slots_[slot_].samples;
    a.startOfVertexSampleIndex = start;
    a.endOfVertexSampleIndex = MTLCounterDontSample;
    a.startOfFragmentSampleIndex = MTLCounterDontSample;
    a.endOfFragmentSampleIndex = end;
}

void MetalDevice::exec_begin_pass(const cmd::BeginPass& p) {
    ++stats_.passes;
    end_blit();
    ts_.skip_pass = false;
    ts_.pipeline = nullptr;
    ts_.pipeline_ok = false;
    ts_.scope_depth = 0;

    // Null color/depth handles name the window's backbuffer and its default depth buffer.
    const bool backbuffer =
        p.color_count == 0 ? (!p.has_depth || p.depth.texture.is_null()) : p.colors[0].texture.is_null();
    MTLRenderPassDescriptor* rp = [MTLRenderPassDescriptor renderPassDescriptor];
    PassFormats f;
    f.flip = !backbuffer;
    Extent2D size;
    if (backbuffer) {
        id<CAMetalDrawable> d = acquire_drawable();
        if (!d) {
            ts_.skip_pass = true;  // minimized / zero-size window: nothing to draw into
            return;
        }
        id<MTLTexture> color = [d texture];
        size = Extent2D{static_cast<u32>([color width]), static_cast<u32>([color height])};
        if (p.color_count > 0) {
            rp.colorAttachments[0].texture = color;
            rp.colorAttachments[0].loadAction = mtl_load(p.colors[0].load);
            rp.colorAttachments[0].storeAction = MTLStoreActionStore;
            const f32* c = p.colors[0].clear.color;
            rp.colorAttachments[0].clearColor = MTLClearColorMake(c[0], c[1], c[2], c[3]);
            f.colors[0] = [color pixelFormat];
        }
        // GL's default framebuffer always has its depth-stencil buffer; mirror that so depth-tested
        // pipelines work in backbuffer passes that do not name the depth attachment.
        id<MTLTexture> depth = default_depth(size);
        const LoadOp dl = p.has_depth ? p.depth.load : LoadOp::Load;
        rp.depthAttachment.texture = depth;
        rp.depthAttachment.loadAction = mtl_load(dl);
        rp.depthAttachment.storeAction = MTLStoreActionStore;
        rp.depthAttachment.clearDepth = p.has_depth ? p.depth.clear.depth : 1.0;
        rp.stencilAttachment.texture = depth;
        rp.stencilAttachment.loadAction = mtl_load(dl);
        rp.stencilAttachment.storeAction = MTLStoreActionStore;
        rp.stencilAttachment.clearStencil = p.has_depth ? p.depth.clear.stencil : 0;
        f.depth = f.stencil = depth_stencil_format_;
    } else {
        for (u32 i = 0; i < p.color_count; ++i) {
            const MtlTexture* t = textures_.try_get(p.colors[i].texture);
            MCE_ASSERT_MSG(t, "begin_pass: stale color target");
            if (!t) continue;
            MCE_ASSERT_MSG(p.colors[i].layer < t->layers, "render target layer out of range");
            rp.colorAttachments[i].texture = t->texture;
            rp.colorAttachments[i].slice = p.colors[i].layer;
            rp.colorAttachments[i].loadAction = mtl_load(p.colors[i].load);
            rp.colorAttachments[i].storeAction = mtl_store(p.colors[i].store);
            const f32* c = p.colors[i].clear.color;
            rp.colorAttachments[i].clearColor = MTLClearColorMake(c[0], c[1], c[2], c[3]);
            f.colors[i] = [t->texture pixelFormat];
            f.samples = t->samples;
            if (i == 0) size = t->size;
        }
        if (p.has_depth) {
            const MtlTexture* t = textures_.try_get(p.depth.texture);
            MCE_ASSERT_MSG(t, "begin_pass: stale depth target");
            if (t) {
                const MTLPixelFormat df = [t->texture pixelFormat];
                MCE_ASSERT_MSG(p.depth.layer < t->layers, "depth target layer out of range");
                rp.depthAttachment.texture = t->texture;
                rp.depthAttachment.slice = p.depth.layer;
                rp.depthAttachment.loadAction = mtl_load(p.depth.load);
                rp.depthAttachment.storeAction = mtl_store(p.depth.store);
                rp.depthAttachment.clearDepth = p.depth.clear.depth;
                f.depth = df;
                if (df == MTLPixelFormatDepth32Float_Stencil8 || df == MTLPixelFormatDepth24Unorm_Stencil8) {
                    rp.stencilAttachment.texture = t->texture;
                    rp.stencilAttachment.slice = p.depth.layer;
                    rp.stencilAttachment.loadAction = mtl_load(p.depth.load);
                    rp.stencilAttachment.storeAction = mtl_store(p.depth.store);
                    rp.stencilAttachment.clearStencil = p.depth.clear.stencil;
                    f.stencil = df;
                }
                f.samples = t->samples;
                if (p.color_count == 0) size = t->size;
            }
        }
    }
    ts_.formats = f;
    ts_.target_size = size;
    attach_samples(rp);

    ts_.enc = [[command_buffer() renderCommandEncoderWithDescriptor:rp] retain];
    ts_.pass_scope = p.debug_name && *p.debug_name;
    if (ts_.pass_scope) [ts_.enc setLabel:[NSString stringWithUTF8String:p.debug_name]];

    Rect2D vp = p.viewport;
    if (vp.width == 0 || vp.height == 0) vp = Rect2D{0, 0, size.width, size.height};
    // Rect2D is top-left based. Texture targets are drawn Y-flipped into GL's bottom-up layout, so
    // their viewport origin is GL's bottom-left one.
    const f64 y = backbuffer ? f64(vp.y) : f64(i64(size.height) - vp.y - i64(vp.height));
    MTLViewport mv;
    mv.originX = f64(vp.x);
    mv.originY = y;
    mv.width = f64(vp.width);
    mv.height = f64(vp.height);
    mv.znear = 0.0;
    mv.zfar = 1.0;
    [ts_.enc setViewport:mv];
    apply_bindings();
}

void MetalDevice::exec_end_pass() {
    if (!ts_.enc) return;
    for (; ts_.scope_depth > 0; --ts_.scope_depth) [ts_.enc popDebugGroup];
    [ts_.enc endEncoding];
    [ts_.enc release];
    ts_.enc = nil;
}

void MetalDevice::apply_bindings() {
    id<MTLRenderCommandEncoder> e = ts_.enc;
    if (!e) return;
    for (u32 i = 0; i < kMaxBufferBanks; ++i) {
        if (!uniforms_[i].buffer) continue;
        [e setVertexBuffer:uniforms_[i].buffer offset:uniforms_[i].offset atIndex:i];
        [e setFragmentBuffer:uniforms_[i].buffer offset:uniforms_[i].offset atIndex:i];
    }
    for (u32 i = 0; i < kMaxStorageBindings; ++i) {
        if (!storage_[i].buffer) continue;
        [e setVertexBuffer:storage_[i].buffer offset:storage_[i].offset atIndex:kStorageBufferBase + i];
        [e setFragmentBuffer:storage_[i].buffer offset:storage_[i].offset atIndex:kStorageBufferBase + i];
    }
    for (u32 i = 0; i < kMaxTextureSlots; ++i) {
        if (textures_bound_[i].texture) {
            [e setVertexTexture:textures_bound_[i].texture atIndex:i];
            [e setFragmentTexture:textures_bound_[i].texture atIndex:i];
        }
        if (textures_bound_[i].sampler) {
            [e setVertexSamplerState:textures_bound_[i].sampler atIndex:i];
            [e setFragmentSamplerState:textures_bound_[i].sampler atIndex:i];
        }
    }
}

void MetalDevice::exec_bind_pipeline(PipelineHandle h) {
    const MtlPipeline* p = pipelines_.try_get(h);
    MCE_ASSERT_MSG(p, "bind_pipeline: stale handle");
    ts_.pipeline = p;
    ts_.pipeline_ok = false;
    if (!p || !ts_.enc) return;
    id<MTLRenderPipelineState> pso = pso_for(h, *p, ts_.formats);
    if (!pso) return;
    ts_.pipeline_ok = true;
    id<MTLRenderCommandEncoder> e = ts_.enc;
    [e setRenderPipelineState:pso];
    [e setDepthStencilState:depth_state(p->state.depth)];
    ts_.primitive = mtl_primitive(p->state.topology);
    const RasterState& r = p->state.raster;
    [e setCullMode:r.cull == CullMode::None ? MTLCullModeNone
                                            : (r.cull == CullMode::Back ? MTLCullModeBack : MTLCullModeFront)];
    // The Y flip of texture targets mirrors the winding seen by the rasterizer.
    const bool ccw = r.front_ccw != ts_.formats.flip;
    [e setFrontFacingWinding:ccw ? MTLWindingCounterClockwise : MTLWindingClockwise];
    [e setTriangleFillMode:r.fill == FillMode::Wireframe ? MTLTriangleFillModeLines : MTLTriangleFillModeFill];
}

void MetalDevice::exec_draw(const cmd::Draw* indexed, const cmd::DrawNonIndexed* plain) {
    const MtlPipeline* p = ts_.pipeline;
    if (!p) {
        MCE_ASSERT_MSG(ts_.skip_pass, "draw without a bound pipeline");
        return;
    }
    if (!ts_.enc || !ts_.pipeline_ok) return;
    if (!ts_.vb_valid && p->stride > 0) return;  // vertex data overflowed or never set: skip
    id<MTLRenderCommandEncoder> e = ts_.enc;
    if (ts_.vb_valid) [e setVertexBuffer:ts_.vb offset:ts_.vb_offset atIndex:kVertexBufferIndex];
    if (indexed) {
        if (!ts_.ib_valid) return;
        const usize offset = ts_.ib_offset + usize(indexed->first_index) * index_size(ts_.ib_format);
        [e drawIndexedPrimitives:ts_.primitive
                      indexCount:indexed->index_count
                       indexType:ts_.ib_format == IndexFormat::U16 ? MTLIndexTypeUInt16 : MTLIndexTypeUInt32
                     indexBuffer:ts_.ib
               indexBufferOffset:offset
                   instanceCount:indexed->instance_count
                      baseVertex:indexed->base_vertex
                    baseInstance:indexed->base_instance];
    } else {
        [e drawPrimitives:ts_.primitive
              vertexStart:plain->first_vertex
              vertexCount:plain->vertex_count
            instanceCount:plain->instance_count
             baseInstance:plain->base_instance];
    }
    ++stats_.draws;
}

void MetalDevice::exec_draw_indirect(const cmd::DrawIndirect& d) {
    const MtlPipeline* p = ts_.pipeline;
    if (!p) {
        MCE_ASSERT_MSG(ts_.skip_pass, "draw_indirect without a bound pipeline");
        return;
    }
    MCE_ASSERT_MSG(ts_.draw_args && u64(d.first_command) + d.draw_count <= ts_.draw_arg_count,
                   "draw_indirect: commands out of range of the last set_draw_args");
    if (!ts_.draw_args || u64(d.first_command) + d.draw_count > ts_.draw_arg_count || d.draw_count == 0) return;
    if (!ts_.enc || !ts_.pipeline_ok) return;
    if (!ts_.ib_valid || (!ts_.vb_valid && p->stride > 0)) return;

    // The records are CPU-visible (they live in the recorded stream), so each becomes one direct
    // draw: the same result as GL's MultiDrawElementsIndirect without an indirect buffer.
    id<MTLRenderCommandEncoder> e = ts_.enc;
    if (ts_.vb_valid) [e setVertexBuffer:ts_.vb offset:ts_.vb_offset atIndex:kVertexBufferIndex];
    const usize isz = index_size(ts_.ib_format);
    const MTLIndexType type = ts_.ib_format == IndexFormat::U16 ? MTLIndexTypeUInt16 : MTLIndexTypeUInt32;
    for (u32 i = 0; i < d.draw_count; ++i) {
        const DrawIndexedIndirectCommand& c = ts_.draw_args[d.first_command + i];
        if (c.index_count == 0 || c.instance_count == 0) continue;
        [e drawIndexedPrimitives:ts_.primitive
                      indexCount:c.index_count
                       indexType:type
                     indexBuffer:ts_.ib
               indexBufferOffset:ts_.ib_offset + usize(c.first_index) * isz
                   instanceCount:c.instance_count
                      baseVertex:c.base_vertex
                    baseInstance:c.base_instance];
    }
    stats_.indirect_bytes += usize(d.draw_count) * sizeof(DrawIndexedIndirectCommand);
    ++stats_.draws;
    ++stats_.indirect_draws;
}

void MetalDevice::exec_resolve(const cmd::Resolve& r) {
    const MtlTexture* src = textures_.try_get(r.src);
    const MtlTexture* dst = textures_.try_get(r.dst);
    const bool ok = src && dst && src->size == dst->size && src->format == dst->format && !is_depth_format(src->format);
    MCE_ASSERT_MSG(ok, "resolve: needs two live color targets of the same size and format");
    if (!ok) return;
    end_blit();
    if (src->samples <= 1) {
        [blit_encoder() copyFromTexture:src->texture toTexture:dst->texture];
        return;
    }
    // An empty pass whose store action resolves the multisampled target.
    MTLRenderPassDescriptor* rp = [MTLRenderPassDescriptor renderPassDescriptor];
    rp.colorAttachments[0].texture = src->texture;
    rp.colorAttachments[0].loadAction = MTLLoadActionLoad;
    rp.colorAttachments[0].storeAction = MTLStoreActionMultisampleResolve;
    rp.colorAttachments[0].resolveTexture = dst->texture;
    attach_samples(rp);
    id<MTLRenderCommandEncoder> e = [command_buffer() renderCommandEncoderWithDescriptor:rp];
    [e setLabel:@"mce.resolve"];
    [e endEncoding];
}

void MetalDevice::submit(const CommandBuffer& cb) {
    MCE_ASSERT_MSG(in_frame_, "submit outside begin_frame/end_frame");
    MCE_ASSERT_MSG(!cb.in_pass(), "submit with an open pass");
    reset_translate_state();
    bool overflow_logged = false;
    auto overflow = [&](const char* ring, usize size) {
        ++stats_.ring_overflows;
        if (!overflow_logged) {
            MCE_LOG_WARN("rhi.metal: {} ring overflow ({} bytes requested); commands dropped", ring, size);
            overflow_logged = true;
        }
    };

    @autoreleasepool {
        const CmdHeader* h = nullptr;
        const void* payload = nullptr;
        for (CommandBuffer::Reader r = cb.reader(); r.next(h, payload);) {
            ++stats_.commands;
            switch (h->type) {
                case CmdType::BeginPass: exec_begin_pass(*static_cast<const cmd::BeginPass*>(payload)); break;
                case CmdType::EndPass:
                    exec_end_pass();
                    ts_.skip_pass = false;
                    break;
                case CmdType::BindPipeline:
                    exec_bind_pipeline(static_cast<const cmd::BindPipeline*>(payload)->pipeline);
                    break;
                case CmdType::SetUniforms: {
                    const auto* c = static_cast<const cmd::SetUniforms*>(payload);
                    const usize at = uniform_ring_.alloc(c->size, kUniformAlignment);
                    if (at == StagingRing::kOverflow) {
                        overflow("uniform", c->size);
                        break;
                    }
                    std::memcpy(uniform_ring_.mapped + at, CommandBuffer::inline_data(c, c->size).data(), c->size);
                    MCE_ASSERT_MSG(c->bank < kMaxBufferBanks, "set_uniforms: bank out of range");
                    if (c->bank >= kMaxBufferBanks) break;
                    uniforms_[c->bank] = BufferBinding{uniform_ring_.buffer, at};
                    if (ts_.enc) {
                        [ts_.enc setVertexBuffer:uniform_ring_.buffer offset:at atIndex:c->bank];
                        [ts_.enc setFragmentBuffer:uniform_ring_.buffer offset:at atIndex:c->bank];
                    }
                    break;
                }
                case CmdType::BindTexture: {
                    const auto* c = static_cast<const cmd::BindTexture*>(payload);
                    MCE_ASSERT_MSG(c->slot < kMaxTextureSlots, "bind_texture: slot out of range");
                    if (c->slot >= kMaxTextureSlots) break;
                    const MtlTexture* t = textures_.try_get(c->texture);
                    const MtlSampler* s = samplers_.try_get(c->sampler);
                    textures_bound_[c->slot] = TextureBinding{t ? t->texture : nil, s ? s->sampler : nil};
                    if (ts_.enc) {
                        [ts_.enc setVertexTexture:textures_bound_[c->slot].texture atIndex:c->slot];
                        [ts_.enc setFragmentTexture:textures_bound_[c->slot].texture atIndex:c->slot];
                        [ts_.enc setVertexSamplerState:textures_bound_[c->slot].sampler atIndex:c->slot];
                        [ts_.enc setFragmentSamplerState:textures_bound_[c->slot].sampler atIndex:c->slot];
                    }
                    break;
                }
                case CmdType::BindStorageBuffer: {
                    const auto* c = static_cast<const cmd::BindStorageBuffer*>(payload);
                    const MtlBuffer* b = buffers_.try_get(c->buffer);
                    if (!b || c->binding >= kMaxStorageBindings) break;
                    storage_[c->binding] = BufferBinding{b->buffer, static_cast<usize>(c->offset)};
                    if (ts_.enc) {
                        const u32 idx = kStorageBufferBase + c->binding;
                        [ts_.enc setVertexBuffer:b->buffer offset:c->offset atIndex:idx];
                        [ts_.enc setFragmentBuffer:b->buffer offset:c->offset atIndex:idx];
                    }
                    break;
                }
                case CmdType::SetStorageData: {
                    const auto* c = static_cast<const cmd::SetStorageData*>(payload);
                    const usize at = geometry_ring_.alloc(c->size, caps_.storage_buffer_alignment);
                    if (at == StagingRing::kOverflow) {
                        overflow("geometry (storage)", c->size);
                        break;
                    }
                    std::memcpy(geometry_ring_.mapped + at, CommandBuffer::inline_data(c, c->size).data(), c->size);
                    if (c->binding >= kMaxStorageBindings) break;
                    storage_[c->binding] = BufferBinding{geometry_ring_.buffer, at};
                    if (ts_.enc) {
                        const u32 idx = kStorageBufferBase + c->binding;
                        [ts_.enc setVertexBuffer:geometry_ring_.buffer offset:at atIndex:idx];
                        [ts_.enc setFragmentBuffer:geometry_ring_.buffer offset:at atIndex:idx];
                    }
                    break;
                }
                case CmdType::WriteTimestamp:
                    exec_write_timestamp(static_cast<const cmd::WriteTimestamp*>(payload)->index);
                    break;
                // Compute is GL-only so far (caps().compute stays false here; M-VISUAL 1 parity backlog).
                case CmdType::BindStorageImage:
                case CmdType::Dispatch:
                case CmdType::DispatchIndirect:
                case CmdType::Barrier: break;
                case CmdType::SetVertexData: {
                    const auto* c = static_cast<const cmd::SetVertexData*>(payload);
                    const usize at = geometry_ring_.alloc(c->size, 16);
                    ts_.vb_valid = at != StagingRing::kOverflow;
                    if (!ts_.vb_valid) {
                        overflow("geometry", c->size);
                        break;
                    }
                    std::memcpy(geometry_ring_.mapped + at, CommandBuffer::inline_data(c, c->size).data(), c->size);
                    ts_.vb = geometry_ring_.buffer;
                    ts_.vb_offset = at;
                    break;
                }
                case CmdType::SetIndexData: {
                    const auto* c = static_cast<const cmd::SetIndexData*>(payload);
                    const usize at = geometry_ring_.alloc(c->size, 16);
                    ts_.ib_valid = at != StagingRing::kOverflow;
                    if (!ts_.ib_valid) {
                        overflow("geometry", c->size);
                        break;
                    }
                    std::memcpy(geometry_ring_.mapped + at, CommandBuffer::inline_data(c, c->size).data(), c->size);
                    ts_.ib = geometry_ring_.buffer;
                    ts_.ib_offset = at;
                    ts_.ib_format = c->format;
                    break;
                }
                case CmdType::BindVertexBuffer: {
                    const auto* c = static_cast<const cmd::BindVertexBuffer*>(payload);
                    const MtlBuffer* b = buffers_.try_get(c->buffer);
                    ts_.vb_valid = b != nullptr;
                    ts_.vb = b ? b->buffer : nil;
                    ts_.vb_offset = c->offset;
                    break;
                }
                case CmdType::BindIndexBuffer: {
                    const auto* c = static_cast<const cmd::BindIndexBuffer*>(payload);
                    const MtlBuffer* b = buffers_.try_get(c->buffer);
                    ts_.ib_valid = b != nullptr;
                    ts_.ib = b ? b->buffer : nil;
                    ts_.ib_offset = c->offset;
                    ts_.ib_format = c->format;
                    break;
                }
                case CmdType::Draw: exec_draw(static_cast<const cmd::Draw*>(payload), nullptr); break;
                case CmdType::DrawNonIndexed:
                    exec_draw(nullptr, static_cast<const cmd::DrawNonIndexed*>(payload));
                    break;
                case CmdType::SetDrawArgs: {
                    const auto* c = static_cast<const cmd::SetDrawArgs*>(payload);
                    const Span<const byte> data =
                        CommandBuffer::inline_data(c, static_cast<u32>(c->count * sizeof(DrawIndexedIndirectCommand)));
                    ts_.draw_args = reinterpret_cast<const DrawIndexedIndirectCommand*>(data.data());
                    ts_.draw_arg_count = c->count;
                    break;
                }
                case CmdType::DrawIndirect: exec_draw_indirect(*static_cast<const cmd::DrawIndirect*>(payload)); break;
                case CmdType::PushDebugScope: {
                    NSString* name =
                        [NSString stringWithUTF8String:static_cast<const cmd::PushDebugScope*>(payload)->name];
                    if (ts_.enc) {
                        [ts_.enc pushDebugGroup:name];
                        ++ts_.scope_depth;
                    }
                    break;
                }
                case CmdType::PopDebugScope:
                    if (ts_.enc && ts_.scope_depth > 0) {
                        [ts_.enc popDebugGroup];
                        --ts_.scope_depth;
                    }
                    break;
                case CmdType::Resolve: exec_resolve(*static_cast<const cmd::Resolve*>(payload)); break;
            }
        }
        exec_end_pass();
    }
}

}  // namespace mce::rhi::metal
