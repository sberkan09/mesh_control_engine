// mce_rhi_gl translate stage: recorded CommandBuffer stream -> GL calls (rhi-renderer.md §3.4 step 5).
// Payloads (uniform blocks, transient geometry) are copied into the slot's persistent-mapped rings.

#include <cstring>

#include "gl_device.h"
#include "mce/log.h"

namespace mce::rhi::gl {
namespace {

GLenum gl_compare(CompareOp op) {
    switch (op) {
        case CompareOp::Never: return GL_NEVER;
        case CompareOp::Less: return GL_LESS;
        case CompareOp::Equal: return GL_EQUAL;
        case CompareOp::LessEqual: return GL_LEQUAL;
        case CompareOp::Greater: return GL_GREATER;
        case CompareOp::NotEqual: return GL_NOTEQUAL;
        case CompareOp::GreaterEqual: return GL_GEQUAL;
        case CompareOp::Always: return GL_ALWAYS;
    }
    return GL_LEQUAL;
}

GLenum gl_topology(PrimitiveTopology t) {
    switch (t) {
        case PrimitiveTopology::Triangles: return GL_TRIANGLES;
        case PrimitiveTopology::Lines: return GL_LINES;
        case PrimitiveTopology::LineStrip: return GL_LINE_STRIP;
        case PrimitiveTopology::Points: return GL_POINTS;
    }
    return GL_TRIANGLES;
}

usize index_size(IndexFormat f) { return f == IndexFormat::U16 ? 2 : 4; }

}  // namespace

void GlDevice::reset_translate_state() {
    ts_.pipeline = nullptr;
    ts_.vb_valid = ts_.ib_valid = false;
    ts_.vb = ts_.ib = 0;
    ts_.draw_args = nullptr;
    ts_.draw_arg_count = 0;
}

void GlDevice::exec_begin_pass(const cmd::BeginPass& p) {
    ++stats_.passes;
    ts_.pass_scope = p.debug_name && *p.debug_name;
    if (ts_.pass_scope) glPushDebugGroup(GL_DEBUG_SOURCE_APPLICATION, 0, -1, p.debug_name);

    // Null color/depth handles name the window's default framebuffer.
    bool backbuffer = p.color_count == 0 ? (!p.has_depth || p.depth.texture.is_null()) : p.colors[0].texture.is_null();
    GLuint fbo = 0;
    Extent2D size;
    if (backbuffer) {
        MCE_ASSERT_MSG(!p.has_depth || p.depth.texture.is_null(), "backbuffer pass needs the default depth buffer");
        size = backbuffer_size();
    } else {
        FramebufferKey key{};
        for (u32 i = 0; i < p.color_count; ++i) {
            key.colors[i] = p.colors[i].texture;
            key.color_layers[i] = p.colors[i].layer;
        }
        if (p.has_depth) {
            key.depth = p.depth.texture;
            key.depth_layer = p.depth.layer;
        }
        fbo = framebuffer_for(key);
        size = texture_size(p.color_count ? p.colors[0].texture : p.depth.texture);
    }
    glBindFramebuffer(GL_FRAMEBUFFER, fbo);
    ts_.target_size = size;

    Rect2D vp = p.viewport;
    if (vp.width == 0 || vp.height == 0) vp = Rect2D{0, 0, size.width, size.height};
    const auto gl_y = static_cast<GLint>(size.height) - vp.y - static_cast<GLint>(vp.height);
    glViewport(vp.x, gl_y, static_cast<GLsizei>(vp.width), static_cast<GLsizei>(vp.height));
    glDisable(GL_SCISSOR_TEST);

    // Clears ignore pipeline state except the write masks: open them first.
    glColorMask(GL_TRUE, GL_TRUE, GL_TRUE, GL_TRUE);
    glDepthMask(GL_TRUE);
    glStencilMask(0xFF);
    ts_.depth_write = true;
    for (u32 i = 0; i < p.color_count; ++i) {
        if (p.colors[i].load != LoadOp::Clear) continue;
        // The default framebuffer's color buffer is draw buffer 0.
        glClearNamedFramebufferfv(fbo, GL_COLOR, backbuffer ? 0 : static_cast<GLint>(i), p.colors[i].clear.color);
    }
    if (p.has_depth && p.depth.load == LoadOp::Clear) {
        const TextureFormat df = backbuffer ? TextureFormat::D24_UNorm_S8 : [&] {
            const GlTexture* t = textures_.try_get(p.depth.texture);
            return t ? t->format : TextureFormat::D32_Float;
        }();
        if (df == TextureFormat::D24_UNorm_S8) {
            glClearNamedFramebufferfi(fbo, GL_DEPTH_STENCIL, 0, p.depth.clear.depth, p.depth.clear.stencil);
        } else {
            glClearNamedFramebufferfv(fbo, GL_DEPTH, 0, &p.depth.clear.depth);
        }
    }
    // A pipeline bound in an earlier pass must re-apply its depth mask.
    ts_.pipeline = nullptr;
}

void GlDevice::exec_bind_pipeline(PipelineHandle h) {
    const GlPipeline* p = pipelines_.try_get(h);
    MCE_ASSERT_MSG(p, "bind_pipeline: stale handle");
    ts_.pipeline = p;
    if (!p) return;
    glBindProgramPipeline(p->pipeline);
    if (p->compute) return;  // no vertex input or fixed-function state
    glBindVertexArray(p->vao);
    ts_.topology = gl_topology(p->state.topology);

    const FixedState& s = p->state;
    if (s.depth.test) {
        glEnable(GL_DEPTH_TEST);
        glDepthFunc(gl_compare(s.depth.compare));
    } else {
        glDisable(GL_DEPTH_TEST);
    }
    glDepthMask(s.depth.write ? GL_TRUE : GL_FALSE);
    ts_.depth_write = s.depth.write;

    switch (s.blend.mode) {
        case BlendMode::Opaque: glDisable(GL_BLEND); break;
        case BlendMode::PremultipliedAlpha:
            glEnable(GL_BLEND);
            glBlendFuncSeparate(GL_ONE, GL_ONE_MINUS_SRC_ALPHA, GL_ONE, GL_ONE_MINUS_SRC_ALPHA);
            break;
        case BlendMode::Alpha:
            glEnable(GL_BLEND);
            glBlendFuncSeparate(GL_SRC_ALPHA, GL_ONE_MINUS_SRC_ALPHA, GL_ONE, GL_ONE_MINUS_SRC_ALPHA);
            break;
        case BlendMode::Additive:
            glEnable(GL_BLEND);
            glBlendFunc(GL_ONE, GL_ONE);
            break;
    }

    if (s.raster.cull == CullMode::None) {
        glDisable(GL_CULL_FACE);
    } else {
        glEnable(GL_CULL_FACE);
        glCullFace(s.raster.cull == CullMode::Back ? GL_BACK : GL_FRONT);
    }
    glFrontFace(s.raster.front_ccw ? GL_CCW : GL_CW);
    glPolygonMode(GL_FRONT_AND_BACK, s.raster.fill == FillMode::Wireframe ? GL_LINE : GL_FILL);
}

void GlDevice::exec_draw(const cmd::Draw* indexed, const cmd::DrawNonIndexed* plain) {
    const GlPipeline* p = ts_.pipeline;
    if (!p || p->compute) {
        MCE_ASSERT_MSG(false, "draw without a bound graphics pipeline");
        return;
    }
    if (!ts_.vb_valid && p->stride > 0) return;  // vertex data overflowed or never set: skip
    if (ts_.vb_valid) glVertexArrayVertexBuffer(p->vao, 0, ts_.vb, static_cast<GLintptr>(ts_.vb_offset),
                                                static_cast<GLsizei>(p->stride));
    if (indexed) {
        if (!ts_.ib_valid) return;
        glVertexArrayElementBuffer(p->vao, ts_.ib);
        const usize offset = ts_.ib_offset + usize(indexed->first_index) * index_size(ts_.ib_format);
        glDrawElementsInstancedBaseVertexBaseInstance(
            ts_.topology, static_cast<GLsizei>(indexed->index_count),
            ts_.ib_format == IndexFormat::U16 ? GL_UNSIGNED_SHORT : GL_UNSIGNED_INT,
            reinterpret_cast<const void*>(offset), static_cast<GLsizei>(indexed->instance_count),
            indexed->base_vertex, indexed->base_instance);
    } else {
        glDrawArraysInstancedBaseInstance(ts_.topology, static_cast<GLint>(plain->first_vertex),
                                          static_cast<GLsizei>(plain->vertex_count),
                                          static_cast<GLsizei>(plain->instance_count), plain->base_instance);
    }
    ++stats_.draws;
}

void GlDevice::exec_draw_indirect(const cmd::DrawIndirect& d) {
    const GlPipeline* p = ts_.pipeline;
    if (!p || p->compute) {
        MCE_ASSERT_MSG(false, "draw_indirect without a bound graphics pipeline");
        return;
    }
    MCE_ASSERT_MSG(ts_.draw_args && u64(d.first_command) + d.draw_count <= ts_.draw_arg_count,
                   "draw_indirect: commands out of range of the last set_draw_args");
    if (!ts_.draw_args || u64(d.first_command) + d.draw_count > ts_.draw_arg_count || d.draw_count == 0) return;
    if (!ts_.ib_valid || (!ts_.vb_valid && p->stride > 0)) return;  // geometry overflowed or never set: skip

    // Indirect first_index counts from the start of the element buffer, so rebase the commands
    // onto the bound index-data offset while copying them into the slot's indirect ring.
    const usize isz = index_size(ts_.ib_format);
    MCE_ASSERT_MSG(ts_.ib_offset % isz == 0, "index data offset not aligned to the index size");
    const u32 index_base = static_cast<u32>(ts_.ib_offset / isz);
    const usize bytes = usize(d.draw_count) * sizeof(DrawIndexedIndirectCommand);
    const usize at = indirect_ring_.alloc(bytes, 16);
    if (at == StagingRing::kOverflow) {
        ++stats_.ring_overflows;
        MCE_LOG_WARN("rhi.gl: indirect ring overflow ({} bytes requested); draw dropped", bytes);
        return;
    }
    auto* dst = reinterpret_cast<DrawIndexedIndirectCommand*>(indirect_ring_.mapped + at);
    for (u32 i = 0; i < d.draw_count; ++i) {
        DrawIndexedIndirectCommand c = ts_.draw_args[d.first_command + i];
        c.first_index += index_base;
        std::memcpy(dst + i, &c, sizeof(c));
    }

    if (ts_.vb_valid)
        glVertexArrayVertexBuffer(p->vao, 0, ts_.vb, static_cast<GLintptr>(ts_.vb_offset), static_cast<GLsizei>(p->stride));
    glVertexArrayElementBuffer(p->vao, ts_.ib);
    glBindBuffer(GL_DRAW_INDIRECT_BUFFER, indirect_ring_.buffer);
    glMultiDrawElementsIndirect(ts_.topology, ts_.ib_format == IndexFormat::U16 ? GL_UNSIGNED_SHORT : GL_UNSIGNED_INT,
                                reinterpret_cast<const void*>(at), static_cast<GLsizei>(d.draw_count),
                                static_cast<GLsizei>(sizeof(DrawIndexedIndirectCommand)));
    ++stats_.draws;
    ++stats_.indirect_draws;
}

void GlDevice::exec_resolve(const cmd::Resolve& r) {
    const GlTexture* src = textures_.try_get(r.src);
    const GlTexture* dst = textures_.try_get(r.dst);
    const bool ok = src && dst && src->size == dst->size && src->format == dst->format && !is_depth_format(src->format);
    MCE_ASSERT_MSG(ok, "resolve: needs two live color targets of the same size and format");
    if (!ok) return;
    FramebufferKey ks{}, kd{};
    ks.colors[0] = r.src;
    kd.colors[0] = r.dst;
    const GLuint fs = framebuffer_for(ks), fd = framebuffer_for(kd);
    const auto w = static_cast<GLint>(src->size.width), h = static_cast<GLint>(src->size.height);
    glBlitNamedFramebuffer(fs, fd, 0, 0, w, h, 0, 0, w, h, GL_COLOR_BUFFER_BIT, GL_NEAREST);
}

void GlDevice::exec_bind_storage_image(const cmd::BindStorageImage& c) {
    MCE_ASSERT_MSG(c.unit < caps_.max_storage_image_units, "bind_storage_image: unit out of range");
    if (c.unit >= caps_.max_storage_image_units) return;
    const GlTexture* t = textures_.try_get(c.texture);
    if (!t) {
        glBindImageTexture(c.unit, 0, 0, GL_FALSE, 0, GL_READ_ONLY, GL_RGBA8);
        return;
    }
    const bool ok = t->usage == TextureUsage::SampledStorage && c.mip < t->mips;
    MCE_ASSERT_MSG(ok, "bind_storage_image: needs a SampledStorage texture and a mip in range");
    if (!ok) return;
    const GLenum access = c.access == StorageAccess::Read ? GL_READ_ONLY
                          : c.access == StorageAccess::Write ? GL_WRITE_ONLY
                                                              : GL_READ_WRITE;
    const GLboolean layered = t->dimension == TextureDimension::Tex2DArray ? GL_TRUE : GL_FALSE;
    glBindImageTexture(c.unit, t->id, static_cast<GLint>(c.mip), layered, 0, access, t->internal);
}

void GlDevice::exec_dispatch(const cmd::Dispatch* direct, const cmd::DispatchIndirect* indirect) {
    const GlPipeline* p = ts_.pipeline;
    if (!p || !p->compute) {
        MCE_ASSERT_MSG(false, "dispatch without a bound compute pipeline");
        return;
    }
    if (direct) {
        if (direct->groups_x == 0 || direct->groups_y == 0 || direct->groups_z == 0) return;
        glDispatchCompute(direct->groups_x, direct->groups_y, direct->groups_z);
    } else {
        const GlBuffer* b = buffers_.try_get(indirect->buffer);
        const bool ok = b && indirect->offset + sizeof(DispatchIndirectCommand) <= b->size;
        MCE_ASSERT_MSG(ok, "dispatch_indirect: stale buffer or arguments outside it");
        if (!ok) return;
        glBindBuffer(GL_DISPATCH_INDIRECT_BUFFER, b->id);
        glDispatchComputeIndirect(static_cast<GLintptr>(indirect->offset));
    }
    ++stats_.dispatches;
}

void GlDevice::exec_barrier(u32 bits) {
    if (bits == barrier::kAll) {
        glMemoryBarrier(GL_ALL_BARRIER_BITS);
        return;
    }
    GLbitfield gl = 0;
    if (bits & barrier::kStorageBuffer) gl |= GL_SHADER_STORAGE_BARRIER_BIT;
    if (bits & barrier::kStorageImage) gl |= GL_SHADER_IMAGE_ACCESS_BARRIER_BIT;
    if (bits & barrier::kSampled) gl |= GL_TEXTURE_FETCH_BARRIER_BIT;
    if (bits & barrier::kIndirect) gl |= GL_COMMAND_BARRIER_BIT;
    if (bits & barrier::kVertexIndex) gl |= GL_VERTEX_ATTRIB_ARRAY_BARRIER_BIT | GL_ELEMENT_ARRAY_BARRIER_BIT;
    if (bits & barrier::kUniform) gl |= GL_UNIFORM_BARRIER_BIT;
    if (gl) glMemoryBarrier(gl);
}

void GlDevice::submit(const CommandBuffer& cb) {
    MCE_ASSERT_MSG(in_frame_, "submit outside begin_frame/end_frame");
    MCE_ASSERT_MSG(!cb.in_pass(), "submit with an open pass");
    reset_translate_state();
    bool overflow_logged = false;
    auto overflow = [&](const char* ring, usize size) {
        ++stats_.ring_overflows;
        if (!overflow_logged) {
            MCE_LOG_WARN("rhi.gl: {} ring overflow ({} bytes requested); commands dropped", ring, size);
            overflow_logged = true;
        }
    };

    const CmdHeader* h = nullptr;
    const void* payload = nullptr;
    for (CommandBuffer::Reader r = cb.reader(); r.next(h, payload);) {
        ++stats_.commands;
        switch (h->type) {
            case CmdType::BeginPass: exec_begin_pass(*static_cast<const cmd::BeginPass*>(payload)); break;
            case CmdType::EndPass:
                if (ts_.pass_scope) glPopDebugGroup();
                ts_.pass_scope = false;
                break;
            case CmdType::BindPipeline:
                exec_bind_pipeline(static_cast<const cmd::BindPipeline*>(payload)->pipeline);
                break;
            case CmdType::SetUniforms: {
                const auto* c = static_cast<const cmd::SetUniforms*>(payload);
                const usize at = uniform_ring_.alloc(c->size, uniform_align_);
                if (at == StagingRing::kOverflow) {
                    overflow("uniform", c->size);
                    break;
                }
                std::memcpy(uniform_ring_.mapped + at, CommandBuffer::inline_data(c, c->size).data(), c->size);
                glBindBufferRange(GL_UNIFORM_BUFFER, c->bank, uniform_ring_.buffer, static_cast<GLintptr>(at),
                                  static_cast<GLsizeiptr>(c->size));
                break;
            }
            case CmdType::BindTexture: {
                const auto* c = static_cast<const cmd::BindTexture*>(payload);
                const GlTexture* t = textures_.try_get(c->texture);
                const GlSampler* s = samplers_.try_get(c->sampler);
                glBindTextureUnit(c->slot, t ? t->id : 0);
                glBindSampler(c->slot, s ? s->id : 0);
                break;
            }
            case CmdType::BindStorageBuffer: {
                const auto* c = static_cast<const cmd::BindStorageBuffer*>(payload);
                const GlBuffer* b = buffers_.try_get(c->buffer);
                if (!b) break;
                if (c->size == 0) glBindBufferBase(GL_SHADER_STORAGE_BUFFER, c->binding, b->id);
                else glBindBufferRange(GL_SHADER_STORAGE_BUFFER, c->binding, b->id, static_cast<GLintptr>(c->offset),
                                       static_cast<GLsizeiptr>(c->size));
                break;
            }
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
                const GlBuffer* b = buffers_.try_get(c->buffer);
                ts_.vb_valid = b != nullptr;
                ts_.vb = b ? b->id : 0;
                ts_.vb_offset = c->offset;
                break;
            }
            case CmdType::BindIndexBuffer: {
                const auto* c = static_cast<const cmd::BindIndexBuffer*>(payload);
                const GlBuffer* b = buffers_.try_get(c->buffer);
                ts_.ib_valid = b != nullptr;
                ts_.ib = b ? b->id : 0;
                ts_.ib_offset = c->offset;
                ts_.ib_format = c->format;
                break;
            }
            case CmdType::Draw: exec_draw(static_cast<const cmd::Draw*>(payload), nullptr); break;
            case CmdType::DrawNonIndexed: exec_draw(nullptr, static_cast<const cmd::DrawNonIndexed*>(payload)); break;
            case CmdType::SetDrawArgs: {
                const auto* c = static_cast<const cmd::SetDrawArgs*>(payload);
                const Span<const byte> data =
                    CommandBuffer::inline_data(c, static_cast<u32>(c->count * sizeof(DrawIndexedIndirectCommand)));
                ts_.draw_args = reinterpret_cast<const DrawIndexedIndirectCommand*>(data.data());
                ts_.draw_arg_count = c->count;
                break;
            }
            case CmdType::DrawIndirect: exec_draw_indirect(*static_cast<const cmd::DrawIndirect*>(payload)); break;
            case CmdType::PushDebugScope:
                glPushDebugGroup(GL_DEBUG_SOURCE_APPLICATION, 1, -1, static_cast<const cmd::PushDebugScope*>(payload)->name);
                break;
            case CmdType::PopDebugScope: glPopDebugGroup(); break;
            case CmdType::Resolve: exec_resolve(*static_cast<const cmd::Resolve*>(payload)); break;
            case CmdType::SetStorageData: {
                const auto* c = static_cast<const cmd::SetStorageData*>(payload);
                const usize at = geometry_ring_.alloc(c->size, storage_align_);
                if (at == StagingRing::kOverflow) {
                    overflow("geometry (storage)", c->size);
                    break;
                }
                std::memcpy(geometry_ring_.mapped + at, CommandBuffer::inline_data(c, c->size).data(), c->size);
                glBindBufferRange(GL_SHADER_STORAGE_BUFFER, c->binding, geometry_ring_.buffer, static_cast<GLintptr>(at),
                                  static_cast<GLsizeiptr>(c->size));
                break;
            }
            case CmdType::WriteTimestamp:
                exec_write_timestamp(static_cast<const cmd::WriteTimestamp*>(payload)->index);
                break;
            case CmdType::BindStorageImage:
                exec_bind_storage_image(*static_cast<const cmd::BindStorageImage*>(payload));
                break;
            case CmdType::Dispatch: exec_dispatch(static_cast<const cmd::Dispatch*>(payload), nullptr); break;
            case CmdType::DispatchIndirect:
                exec_dispatch(nullptr, static_cast<const cmd::DispatchIndirect*>(payload));
                break;
            case CmdType::Barrier: exec_barrier(static_cast<const cmd::Barrier*>(payload)->bits); break;
        }
    }
}

}  // namespace mce::rhi::gl
