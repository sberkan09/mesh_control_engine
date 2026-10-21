// mce_rhi_webgpu translate stage: recorded CommandBuffer stream -> WebGPU (webgpu_device.h).
// Walk 1 stages every inline payload into the frame's rings; walk 2 encodes the passes.

#include <cstring>

#include "mce/log.h"
#include "mce/math/math.h"
#include "webgpu_device.h"

namespace mce::rhi::webgpu {
namespace {

WGPUVertexFormat wgpu_vertex_format(VertexFormat f) {
    switch (f) {
        case VertexFormat::Float1: return WGPUVertexFormat_Float32;
        case VertexFormat::Float2: return WGPUVertexFormat_Float32x2;
        case VertexFormat::Float3: return WGPUVertexFormat_Float32x3;
        case VertexFormat::Float4: return WGPUVertexFormat_Float32x4;
        case VertexFormat::UNorm8x4: return WGPUVertexFormat_Unorm8x4;
        case VertexFormat::UInt1: return WGPUVertexFormat_Uint32;
        case VertexFormat::SNorm16x2: return WGPUVertexFormat_Snorm16x2;
        case VertexFormat::SNorm16x4: return WGPUVertexFormat_Snorm16x4;
        case VertexFormat::UNorm16x2: return WGPUVertexFormat_Unorm16x2;
    }
    return WGPUVertexFormat_Float32x4;
}

WGPUCompareFunction wgpu_compare(CompareOp op) {
    switch (op) {
        case CompareOp::Never: return WGPUCompareFunction_Never;
        case CompareOp::Less: return WGPUCompareFunction_Less;
        case CompareOp::Equal: return WGPUCompareFunction_Equal;
        case CompareOp::LessEqual: return WGPUCompareFunction_LessEqual;
        case CompareOp::Greater: return WGPUCompareFunction_Greater;
        case CompareOp::NotEqual: return WGPUCompareFunction_NotEqual;
        case CompareOp::GreaterEqual: return WGPUCompareFunction_GreaterEqual;
        case CompareOp::Always: return WGPUCompareFunction_Always;
    }
    return WGPUCompareFunction_LessEqual;
}

WGPUPrimitiveTopology wgpu_topology(PrimitiveTopology t) {
    switch (t) {
        case PrimitiveTopology::Triangles: return WGPUPrimitiveTopology_TriangleList;
        case PrimitiveTopology::Lines: return WGPUPrimitiveTopology_LineList;
        case PrimitiveTopology::LineStrip: return WGPUPrimitiveTopology_LineStrip;
        case PrimitiveTopology::Points: return WGPUPrimitiveTopology_PointList;
    }
    return WGPUPrimitiveTopology_TriangleList;
}

// False for Opaque (no blend state).
bool wgpu_blend(BlendMode m, WGPUBlendState& b) {
    b.color = WGPUBlendComponent{WGPUBlendOperation_Add, WGPUBlendFactor_One, WGPUBlendFactor_Zero};
    b.alpha = b.color;
    switch (m) {
        case BlendMode::Opaque: return false;
        case BlendMode::PremultipliedAlpha:
            b.color.dstFactor = b.alpha.dstFactor = WGPUBlendFactor_OneMinusSrcAlpha;
            return true;
        case BlendMode::Alpha:
            b.color.srcFactor = WGPUBlendFactor_SrcAlpha;
            b.color.dstFactor = b.alpha.dstFactor = WGPUBlendFactor_OneMinusSrcAlpha;
            return true;
        case BlendMode::Additive:
            b.color.dstFactor = b.alpha.dstFactor = WGPUBlendFactor_One;
            return true;
    }
    return false;
}

WGPULoadOp wgpu_load(LoadOp op) {
    // WebGPU has no "don't care" load: clearing is the cheap equivalent on tilers.
    return op == LoadOp::Load ? WGPULoadOp_Load : WGPULoadOp_Clear;
}

bool has_stencil(WGPUTextureFormat f) {
    return f == WGPUTextureFormat_Depth24PlusStencil8 || f == WGPUTextureFormat_Depth32FloatStencil8;
}

bool carries_payload(CmdType t) {
    return t == CmdType::SetVertexData || t == CmdType::SetIndexData || t == CmdType::SetStorageData ||
           t == CmdType::SetDrawArgs;
}

}  // namespace

// ---- walk 1: payloads -> the frame's rings ---------------------------------------------------------

// Copies `data` into the ring's staging at the next `alignment` boundary; ~0 when it does not fit.
usize WebGpuDevice::push_ring(Ring& ring, Span<const byte> data, usize alignment) {
    const usize at = align_up(ring.head, alignment);
    const usize size = align_up(data.size(), 4);
    if (at + size > ring.capacity) return ~usize(0);
    std::memcpy(ring.staging.data() + at, data.data(), data.size());
    if (size > data.size()) std::memset(ring.staging.data() + at + data.size(), 0, size - data.size());
    ring.head = at + size;
    return at;
}

void WebGpuDevice::flush_rings() {
    auto flush = [&](Ring& r) {
        if (r.head <= r.flushed) return;
        const usize from = r.flushed & ~usize(3);
        wgpuQueueWriteBuffer(queue_, r.buffer, from, r.staging.data() + from, align_up(r.head, 4) - from);
        r.flushed = r.head;
    };
    flush(uniforms_);
    flush(geometry_);
    for (Ring& r : storage_) flush(r);
}

void WebGpuDevice::stage_payloads(const CommandBuffer& cb) {
    prepared_.clear();
    bool overflow_logged = false;
    auto overflow = [&](const char* what, usize size) {
        ++stats_.ring_overflows;
        if (!overflow_logged) {
            MCE_LOG_WARN("rhi.webgpu: {} ring overflow ({} bytes requested); commands dropped", what, size);
            overflow_logged = true;
        }
    };
    const CmdHeader* h = nullptr;
    const void* payload = nullptr;
    for (CommandBuffer::Reader r = cb.reader(); r.next(h, payload);) {
        if (!carries_payload(h->type)) continue;
        Prepared prep;
        switch (h->type) {
            case CmdType::SetVertexData: {
                const auto* c = static_cast<const cmd::SetVertexData*>(payload);
                const usize at = push_ring(geometry_, CommandBuffer::inline_data(c, c->size), 16);
                if (at == ~usize(0)) overflow("geometry", c->size);
                else prep = Prepared{geometry_.buffer, static_cast<u32>(at), c->size, true};
                break;
            }
            case CmdType::SetIndexData: {
                const auto* c = static_cast<const cmd::SetIndexData*>(payload);
                const usize at = push_ring(geometry_, CommandBuffer::inline_data(c, c->size), 16);
                if (at == ~usize(0)) overflow("geometry", c->size);
                else prep = Prepared{geometry_.buffer, static_cast<u32>(at), c->size, true};
                break;
            }
            case CmdType::SetDrawArgs: {
                const auto* c = static_cast<const cmd::SetDrawArgs*>(payload);
                const Span<const byte> data = CommandBuffer::inline_data(c, u32(c->count * sizeof(DrawIndexedIndirectCommand)));
                const u32 first = static_cast<u32>(draw_args_.size());
                draw_args_.resize(first + c->count);
                std::memcpy(draw_args_.data() + first, data.data(), data.size());
                prep = Prepared{nullptr, first, c->count, true};
                stats_.indirect_bytes += data.size();
                break;
            }
            case CmdType::SetStorageData: {
                const auto* c = static_cast<const cmd::SetStorageData*>(payload);
                Ring* ring = c->binding < kMaxStorageBindings ? storage_ring(c->binding) : nullptr;
                const usize at = ring ? push_ring(*ring, CommandBuffer::inline_data(c, c->size),
                                                  math::max<usize>(caps_.storage_buffer_alignment, 16))
                                      : ~usize(0);
                if (at == ~usize(0)) overflow("storage", c->size);
                else prep = Prepared{ring->buffer, static_cast<u32>(at), static_cast<u32>(align_up(usize(c->size), 4)), true};
                break;
            }
            default: break;
        }
        prepared_.push_back(prep);
    }
}

// ---- walk 2: passes ------------------------------------------------------------------------------

WGPURenderPipeline WebGpuDevice::pipeline_variant(GpuPipeline& p) {
    for (const PipelineVariant& v : p.variants)
        if (v.key == ts_.target) return v.pipeline;

    WGPUVertexAttribute attrs[16];
    for (u32 i = 0; i < p.attr_count; ++i) {
        attrs[i] = WGPU_VERTEX_ATTRIBUTE_INIT;
        attrs[i].shaderLocation = p.attrs[i].location;
        attrs[i].format = wgpu_vertex_format(p.attrs[i].format);
        attrs[i].offset = p.attrs[i].offset;
    }
    WGPUVertexBufferLayout vb = WGPU_VERTEX_BUFFER_LAYOUT_INIT;
    vb.stepMode = WGPUVertexStepMode_Vertex;
    vb.arrayStride = p.stride;
    vb.attributeCount = p.attr_count;
    vb.attributes = attrs;

    WGPUBlendState blend = WGPU_BLEND_STATE_INIT;
    const bool blended = wgpu_blend(p.state.blend.mode, blend);
    WGPUColorTargetState colors[cmd::kMaxColorAttachments];
    for (u32 i = 0; i < ts_.target.color_count; ++i) {
        colors[i] = WGPU_COLOR_TARGET_STATE_INIT;
        colors[i].format = ts_.target.colors[i];
        colors[i].blend = blended ? &blend : nullptr;
    }
    WGPUFragmentState fs = WGPU_FRAGMENT_STATE_INIT;
    fs.module = p.fs;
    fs.entryPoint = WGPUStringView{"main", 4};
    fs.targetCount = ts_.target.color_count;
    fs.targets = colors;

    WGPUDepthStencilState ds = WGPU_DEPTH_STENCIL_STATE_INIT;
    const DepthState& d = p.state.depth;
    ds.format = ts_.target.depth;
    ds.depthWriteEnabled = d.write ? WGPUOptionalBool_True : WGPUOptionalBool_False;
    ds.depthCompare = d.test ? wgpu_compare(d.compare) : WGPUCompareFunction_Always;

    WGPURenderPipelineDescriptor pd = WGPU_RENDER_PIPELINE_DESCRIPTOR_INIT;
    pd.label = WGPUStringView{p.name.c_str(), p.name.size()};
    pd.layout = p.layout;
    pd.vertex.module = p.vs;
    pd.vertex.entryPoint = WGPUStringView{"main", 4};
    if (p.stride > 0 && p.attr_count > 0) {
        pd.vertex.bufferCount = 1;
        pd.vertex.buffers = &vb;
    }
    pd.primitive.topology = wgpu_topology(p.state.topology);
    if (p.state.topology == PrimitiveTopology::LineStrip)
        pd.primitive.stripIndexFormat = ts_.ib_format == IndexFormat::U16 ? WGPUIndexFormat_Uint16 : WGPUIndexFormat_Uint32;
    const RasterState& rs = p.state.raster;
    pd.primitive.cullMode = rs.cull == CullMode::None    ? WGPUCullMode_None
                            : rs.cull == CullMode::Front ? WGPUCullMode_Front
                                                         : WGPUCullMode_Back;
    // The vertex epilogue negates y (shader_wgsl.h), which mirrors the winding: swap the front face.
    pd.primitive.frontFace = rs.front_ccw ? WGPUFrontFace_CW : WGPUFrontFace_CCW;
    // FillMode::Wireframe has no WebGPU equivalent: drawn solid.
    pd.depthStencil = ts_.target.has_depth ? &ds : nullptr;
    pd.multisample.count = ts_.target.samples;
    pd.fragment = &fs;

    WGPURenderPipeline gp = wgpuDeviceCreateRenderPipeline(device_, &pd);
    if (!gp) {
        MCE_LOG_ERROR("rhi.webgpu: pipeline '{}' variant failed", p.name.c_str());
        ++validation_errors_;
        return nullptr;
    }
    p.variants.push_back(PipelineVariant{ts_.target, gp});
    return gp;
}

void WebGpuDevice::begin_pass(WGPUCommandEncoder enc, const cmd::BeginPass& p) {
    ++stats_.passes;
    WGPURenderPassColorAttachment colors[cmd::kMaxColorAttachments];
    TargetKey key;
    Extent2D size;
    for (u32 i = 0; i < p.color_count; ++i) {
        WGPURenderPassColorAttachment& c = colors[i];
        c = WGPU_RENDER_PASS_COLOR_ATTACHMENT_INIT;
        if (p.colors[i].texture.is_null()) {
            c.view = backbuffer_view_;
            key.colors[i] = WGPUTextureFormat_RGBA8Unorm;
            if (i == 0) size = backbuffer_extent_;
        } else if (const GpuTexture* t = textures_.try_get(p.colors[i].texture);
                   t && p.colors[i].layer < t->targets.size()) {
            c.view = t->targets[p.colors[i].layer];
            key.colors[i] = t->wgpu_format;
            key.samples = t->samples;
            if (i == 0) size = t->size;
        }
        c.loadOp = wgpu_load(p.colors[i].load);
        c.storeOp = p.colors[i].store == StoreOp::Store ? WGPUStoreOp_Store : WGPUStoreOp_Discard;
        const f32* cc = p.colors[i].clear.color;
        c.clearValue = WGPUColor{cc[0], cc[1], cc[2], cc[3]};
    }
    key.color_count = static_cast<u8>(p.color_count);

    WGPURenderPassDepthStencilAttachment depth = WGPU_RENDER_PASS_DEPTH_STENCIL_ATTACHMENT_INIT;
    if (p.has_depth) {
        if (p.depth.texture.is_null()) {
            depth.view = backbuffer_depth_view_;
            key.depth = WGPUTextureFormat_Depth24PlusStencil8;
            if (p.color_count == 0) size = backbuffer_extent_;
        } else if (const GpuTexture* t = textures_.try_get(p.depth.texture); t && p.depth.layer < t->targets.size()) {
            depth.view = t->targets[p.depth.layer];
            key.depth = t->wgpu_format;
            key.samples = t->samples;
            if (p.color_count == 0) size = t->size;
        }
        key.has_depth = depth.view != nullptr;
        depth.depthLoadOp = wgpu_load(p.depth.load);
        depth.depthStoreOp = p.depth.store == StoreOp::Store ? WGPUStoreOp_Store : WGPUStoreOp_Discard;
        depth.depthClearValue = p.depth.clear.depth;
        if (has_stencil(key.depth)) {
            depth.stencilLoadOp = depth.depthLoadOp;
            depth.stencilStoreOp = depth.depthStoreOp;
            depth.stencilClearValue = p.depth.clear.stencil;
        }
    }
    for (u32 i = 0; i < p.color_count; ++i)
        if (!colors[i].view) {
            MCE_LOG_ERROR("rhi.webgpu: render pass '{}': color attachment {} is not a live target",
                          p.debug_name ? p.debug_name : "", i);
            ++validation_errors_;
            return;
        }

    WGPURenderPassDescriptor rp = WGPU_RENDER_PASS_DESCRIPTOR_INIT;
    if (p.debug_name) rp.label = WGPUStringView{p.debug_name, WGPU_STRLEN};
    rp.colorAttachmentCount = p.color_count;
    rp.colorAttachments = colors;
    rp.depthStencilAttachment = key.has_depth ? &depth : nullptr;
    ts_.pass = wgpuCommandEncoderBeginRenderPass(enc, &rp);
    ts_.target = key;
    ts_.bound = nullptr;
    ts_.ib_bound = false;
    // Rect2D is top-left based; targets hold GL's layout, whose row 0 is the bottom.
    Rect2D vp = p.viewport;
    if (vp.width == 0 || vp.height == 0) vp = Rect2D{0, 0, size.width, size.height};
    const f32 y = static_cast<f32>(static_cast<i32>(size.height) - vp.y - static_cast<i32>(vp.height));
    wgpuRenderPassEncoderSetViewport(ts_.pass, static_cast<f32>(vp.x), y, static_cast<f32>(vp.width),
                                     static_cast<f32>(vp.height), 0.0f, 1.0f);
}

void WebGpuDevice::end_pass() {
    if (ts_.pass) {
        wgpuRenderPassEncoderEnd(ts_.pass);
        wgpuRenderPassEncoderRelease(ts_.pass);
    }
    ts_.pass = nullptr;
    ts_.bound = nullptr;
}

WGPUBindGroup WebGpuDevice::make_texture_group(GpuPipeline& p, bool compute) {
    const u32 mask = compute ? p.cres.sampler_mask : (p.vres.sampler_mask | p.fres.sampler_mask);
    WGPUBindGroupEntry entries[2 * kMaxSamplers];
    u32 n = 0;
    for (u32 s = 0; s < kMaxSamplers; ++s) {
        if (!(mask & (1u << s))) continue;
        const StageResources& res = compute ? p.cres : ((p.vres.sampler_mask & (1u << s)) ? p.vres : p.fres);
        const SamplerKind kind = res.sampler_kind[s];
        const bool shadow = kind == SamplerKind::Shadow2D || kind == SamplerKind::Shadow2DArray;
        const bool array = kind == SamplerKind::Tex2DArray || kind == SamplerKind::Shadow2DArray;
        const BoundTexture& b = textures_bound_[s];
        const GpuTexture* t = textures_.try_get(b.texture);
        const GpuSampler* sm = samplers_.try_get(b.sampler);
        // A view of the wrong dimension or a missing one falls back to the dummy (GL samples black/white).
        const bool view_ok = t && t->view && ((t->dimension == TextureDimension::Tex2DArray) == array) &&
                             is_depth_format(t->format) == shadow;
        WGPUBindGroupEntry& te = entries[n++];
        te = WGPU_BIND_GROUP_ENTRY_INIT;
        te.binding = s;
        te.textureView = view_ok ? t->view : dummy_view_[static_cast<u32>(kind)];
        WGPUBindGroupEntry& se = entries[n++];
        se = WGPU_BIND_GROUP_ENTRY_INIT;
        se.binding = kSamplerBindingBase + s;
        se.sampler = sm && sm->compare == shadow ? sm->sampler : (shadow ? dummy_compare_sampler_ : dummy_sampler_);
    }
    WGPUBindGroupDescriptor gd = WGPU_BIND_GROUP_DESCRIPTOR_INIT;
    gd.layout = p.layouts[kGroupTextures];
    gd.entryCount = n;
    gd.entries = entries;
    return wgpuDeviceCreateBindGroup(device_, &gd);
}

WGPUBindGroup WebGpuDevice::make_storage_group(GpuPipeline& p, bool compute) {
    const u32 mask = compute ? p.cres.storage_mask : (p.vres.storage_mask | p.fres.storage_mask);
    WGPUBindGroupEntry entries[kMaxStorageBindings];
    u32 n = 0;
    for (u32 b = 0; b < kMaxStorageBindings; ++b) {
        if (!(mask & (1u << b))) continue;
        const BoundStorage& s = storage_bound_[b];
        WGPUBindGroupEntry& e = entries[n++];
        e = WGPU_BIND_GROUP_ENTRY_INIT;
        e.binding = b;
        e.buffer = s.buffer ? s.buffer : dummy_storage_;
        e.offset = s.buffer ? s.offset : 0;
        e.size = s.buffer && s.size ? s.size : WGPU_WHOLE_SIZE;
    }
    WGPUBindGroupDescriptor gd = WGPU_BIND_GROUP_DESCRIPTOR_INIT;
    gd.layout = p.layouts[kGroupStorage];
    gd.entryCount = n;
    gd.entries = entries;
    return wgpuDeviceCreateBindGroup(device_, &gd);
}

// Storage images (compute): the layout carries each image's format, so it is made from the bound
// textures at the first dispatch and the pipeline with it.
WGPUBindGroup WebGpuDevice::make_image_group(GpuPipeline& p) {
    const StageResources& res = p.cres;
    WGPUBindGroupEntry entries[kMaxImageUnits];
    u32 n = 0;
    for (u32 u = 0; u < kMaxImageUnits; ++u) {
        if (!(res.image_mask & (1u << u))) continue;
        const GpuTexture* t = textures_.try_get(images_bound_[u].texture);
        if (!t || t->usage != TextureUsage::SampledStorage || images_bound_[u].mip >= t->mip_views.size()) return nullptr;
        WGPUBindGroupEntry& e = entries[n++];
        e = WGPU_BIND_GROUP_ENTRY_INIT;
        e.binding = u;
        e.textureView = t->mip_views[images_bound_[u].mip];
    }
    WGPUBindGroupDescriptor gd = WGPU_BIND_GROUP_DESCRIPTOR_INIT;
    gd.layout = p.layouts[kGroupImages];
    gd.entryCount = n;
    gd.entries = entries;
    return wgpuDeviceCreateBindGroup(device_, &gd);
}

// Sets groups 0..2 (and 3 for compute) on the open pass, re-making those whose bindings changed.
bool WebGpuDevice::bind_groups(GpuPipeline& p, bool compute) {
    // Group 0: re-stage banks whose data predates this frame's ring.
    u32 offsets[kMaxUniformBanks];
    for (u32 b = 0; b < kMaxUniformBanks; ++b) {
        if (uniform_set_[b] && uniform_offset_[b] == ~0u) {
            const usize at = push_ring(uniforms_, uniform_data_[b].span(), kUniformAlignment);
            uniform_offset_[b] = at == ~usize(0) ? 0u : static_cast<u32>(at);
        }
        offsets[b] = uniform_set_[b] ? uniform_offset_[b] : 0u;
    }
    const u64 gens[4] = {0, texture_gen_, storage_gen_, image_gen_};
    WGPUBindGroup groups[4] = {compute ? uniform_group_compute_ : uniform_group_, nullptr, nullptr, nullptr};
    const u32 group_count = compute && p.cres.image_mask ? 4 : 3;
    for (u32 g = 1; g < group_count; ++g) {
        if (p.layouts[g] == empty_layout_) {
            groups[g] = empty_group_;
            continue;
        }
        if (!p.groups[g] || p.group_gen[g] != gens[g]) {
            if (p.groups[g]) retired_groups_.push_back(p.groups[g]);
            p.groups[g] = g == kGroupTextures  ? make_texture_group(p, compute)
                          : g == kGroupStorage ? make_storage_group(p, compute)
                                               : make_image_group(p);
            p.group_gen[g] = gens[g];
        }
        groups[g] = p.groups[g];
        if (!groups[g]) return false;
    }
    for (u32 g = 0; g < group_count; ++g) {
        const u32 dyn = g == kGroupUniforms ? kMaxUniformBanks : 0;
        if (compute) wgpuComputePassEncoderSetBindGroup(compute_pass_, g, groups[g], dyn, dyn ? offsets : nullptr);
        else wgpuRenderPassEncoderSetBindGroup(ts_.pass, g, groups[g], dyn, dyn ? offsets : nullptr);
    }
    return true;
}

// Binds the pipeline variant for the current pass and every resource its stages declare.
bool WebGpuDevice::flush_draw_state() {
    GpuPipeline* p = ts_.pipeline;
    if (!p || !ts_.pass || !p->vs || !p->fs) {
        MCE_ASSERT_MSG(p && p->fs, "draw without a bound graphics pipeline");
        return false;
    }
    WGPURenderPipeline gp = pipeline_variant(*p);
    if (!gp) return false;
    if (gp != ts_.bound) {
        wgpuRenderPassEncoderSetPipeline(ts_.pass, gp);
        ts_.bound = gp;
    }
    if (!bind_groups(*p, false)) return false;
    if (p->stride > 0 && p->attr_count > 0) {
        if (!ts_.vb_valid) return false;  // vertex data overflowed or never set: skip
        wgpuRenderPassEncoderSetVertexBuffer(ts_.pass, 0, ts_.vb, ts_.vb_offset, WGPU_WHOLE_SIZE);
    }
    return true;
}

void WebGpuDevice::resolve(WGPUCommandEncoder enc, const cmd::Resolve& r) {
    const GpuTexture* src = textures_.try_get(r.src);
    const GpuTexture* dst = textures_.try_get(r.dst);
    const bool valid = src && dst && src->size == dst->size && src->format == dst->format && !is_depth_format(src->format) &&
                       !src->targets.empty() && !dst->targets.empty();
    MCE_ASSERT_MSG(valid, "resolve: needs two live color targets of the same size and format");
    if (!valid) return;
    if (src->samples == 1) {  // GL blits single-sample targets too: copy instead
        WGPUTexelCopyTextureInfo a = WGPU_TEXEL_COPY_TEXTURE_INFO_INIT;
        a.texture = src->texture;
        WGPUTexelCopyTextureInfo b = WGPU_TEXEL_COPY_TEXTURE_INFO_INIT;
        b.texture = dst->texture;
        const WGPUExtent3D size{src->size.width, src->size.height, 1};
        wgpuCommandEncoderCopyTextureToTexture(enc, &a, &b, &size);
        return;
    }
    WGPURenderPassColorAttachment c = WGPU_RENDER_PASS_COLOR_ATTACHMENT_INIT;
    c.view = src->targets[0];
    c.resolveTarget = dst->targets[0];
    c.loadOp = WGPULoadOp_Load;
    c.storeOp = WGPUStoreOp_Store;
    WGPURenderPassDescriptor rp = WGPU_RENDER_PASS_DESCRIPTOR_INIT;
    rp.colorAttachmentCount = 1;
    rp.colorAttachments = &c;
    WGPURenderPassEncoder pass = wgpuCommandEncoderBeginRenderPass(enc, &rp);
    wgpuRenderPassEncoderEnd(pass);
    wgpuRenderPassEncoderRelease(pass);
}

// Every dispatch gets its own compute pass; WebGPU orders passes, which makes barrier() a no-op.
void WebGpuDevice::dispatch(WGPUCommandEncoder enc, const cmd::Dispatch* direct, const cmd::DispatchIndirect* indirect) {
    GpuPipeline* p = ts_.pipeline;
    if (!p || p->fs || !p->vs) {
        MCE_ASSERT_MSG(false, "dispatch without a bound compute pipeline");
        return;
    }
    const GpuBuffer* args = nullptr;
    if (direct) {
        if (direct->groups_x == 0 || direct->groups_y == 0 || direct->groups_z == 0) return;
    } else {
        args = buffers_.try_get(indirect->buffer);
        const bool ok = args && indirect->offset + sizeof(DispatchIndirectCommand) <= args->size;
        MCE_ASSERT_MSG(ok, "dispatch_indirect: stale buffer or arguments outside it");
        if (!ok) return;
    }
    if (!p->cs) {
        // Storage images: build group 3 from the bound images' formats, then the pipeline.
        const StageResources& res = p->cres;
        WGPUBindGroupLayoutEntry entries[kMaxImageUnits];
        u32 n = 0;
        for (u32 u = 0; u < kMaxImageUnits; ++u) {
            if (!(res.image_mask & (1u << u))) continue;
            const GpuTexture* t = textures_.try_get(images_bound_[u].texture);
            const bool ok = t && t->usage == TextureUsage::SampledStorage;
            MCE_ASSERT_MSG(ok, "dispatch: storage image unit without a SampledStorage texture");
            if (!ok) return;
            WGPUBindGroupLayoutEntry& e = entries[n++];
            e = WGPU_BIND_GROUP_LAYOUT_ENTRY_INIT;
            e.binding = u;
            e.visibility = WGPUShaderStage_Compute;
            e.storageTexture.access = (res.image_read_only_mask & (1u << u))    ? WGPUStorageTextureAccess_ReadOnly
                                      : (res.image_write_only_mask & (1u << u)) ? WGPUStorageTextureAccess_WriteOnly
                                                                                : WGPUStorageTextureAccess_ReadWrite;
            e.storageTexture.format = t->wgpu_format;
            e.storageTexture.viewDimension =
                t->dimension == TextureDimension::Tex2DArray ? WGPUTextureViewDimension_2DArray : WGPUTextureViewDimension_2D;
        }
        WGPUBindGroupLayoutDescriptor ld = WGPU_BIND_GROUP_LAYOUT_DESCRIPTOR_INIT;
        ld.entryCount = n;
        ld.entries = entries;
        p->layouts[kGroupImages] = wgpuDeviceCreateBindGroupLayout(device_, &ld);
        p->owns_layout[kGroupImages] = true;
        WGPUPipelineLayoutDescriptor pld = WGPU_PIPELINE_LAYOUT_DESCRIPTOR_INIT;
        pld.bindGroupLayoutCount = 4;
        pld.bindGroupLayouts = p->layouts;
        p->layout = wgpuDeviceCreatePipelineLayout(device_, &pld);
        WGPUComputePipelineDescriptor cd = WGPU_COMPUTE_PIPELINE_DESCRIPTOR_INIT;
        cd.label = WGPUStringView{p->name.c_str(), p->name.size()};
        cd.layout = p->layout;
        cd.compute.module = p->vs;
        cd.compute.entryPoint = WGPUStringView{"main", 4};
        p->cs = wgpuDeviceCreateComputePipeline(device_, &cd);
        if (!p->cs) {
            MCE_LOG_ERROR("rhi.webgpu: compute pipeline '{}' failed", p->name.c_str());
            ++validation_errors_;
            return;
        }
    }
    compute_pass_ = wgpuCommandEncoderBeginComputePass(enc, nullptr);
    wgpuComputePassEncoderSetPipeline(compute_pass_, p->cs);
    if (bind_groups(*p, true)) {
        if (direct) wgpuComputePassEncoderDispatchWorkgroups(compute_pass_, direct->groups_x, direct->groups_y, direct->groups_z);
        else wgpuComputePassEncoderDispatchWorkgroupsIndirect(compute_pass_, args->buffer, indirect->offset);
        ++stats_.dispatches;
    }
    wgpuComputePassEncoderEnd(compute_pass_);
    wgpuComputePassEncoderRelease(compute_pass_);
    compute_pass_ = nullptr;
}

void WebGpuDevice::encode(const CommandBuffer& cb, WGPUCommandEncoder enc) {
    ts_.pipeline = nullptr;
    ts_.bound = nullptr;
    ts_.pass = nullptr;
    ts_.vb_valid = ts_.ib_valid = ts_.ib_bound = false;
    ts_.args_count = 0;

    auto bind_index = [&] {
        if (ts_.ib_bound) return;
        wgpuRenderPassEncoderSetIndexBuffer(ts_.pass, ts_.ib,
                                            ts_.ib_format == IndexFormat::U16 ? WGPUIndexFormat_Uint16 : WGPUIndexFormat_Uint32,
                                            ts_.ib_offset, WGPU_WHOLE_SIZE);
        ts_.ib_bound = true;
    };

    usize next_prepared = 0;
    const CmdHeader* h = nullptr;
    const void* payload = nullptr;
    for (CommandBuffer::Reader r = cb.reader(); r.next(h, payload);) {
        ++stats_.commands;
        const Prepared prep = carries_payload(h->type) && next_prepared < prepared_.size() ? prepared_[next_prepared++]
                                                                                            : Prepared{};
        switch (h->type) {
            case CmdType::BeginPass: begin_pass(enc, *static_cast<const cmd::BeginPass*>(payload)); break;
            case CmdType::EndPass: end_pass(); break;
            case CmdType::BindPipeline: {
                GpuPipeline* p = pipelines_.try_get(static_cast<const cmd::BindPipeline*>(payload)->pipeline);
                MCE_ASSERT_MSG(p, "bind_pipeline: stale handle");
                ts_.pipeline = p;
                break;
            }
            case CmdType::SetUniforms: {
                const auto* c = static_cast<const cmd::SetUniforms*>(payload);
                if (c->bank >= kMaxUniformBanks) break;
                MCE_ASSERT_MSG(c->size <= uniform_bind_size_, "set_uniforms: block larger than the bank's binding");
                const Span<const byte> data = CommandBuffer::inline_data(c, c->size);
                uniform_data_[c->bank].resize(c->size);
                std::memcpy(uniform_data_[c->bank].data(), data.data(), c->size);
                uniform_set_[c->bank] = true;
                const usize at = push_ring(uniforms_, data, kUniformAlignment);
                if (at == ~usize(0)) {
                    ++stats_.ring_overflows;
                    uniform_offset_[c->bank] = 0;
                } else {
                    uniform_offset_[c->bank] = static_cast<u32>(at);
                }
                break;
            }
            case CmdType::BindTexture: {
                const auto* c = static_cast<const cmd::BindTexture*>(payload);
                if (c->slot < kMaxSamplers) {
                    textures_bound_[c->slot] = BoundTexture{c->texture, c->sampler};
                    ++texture_gen_;
                }
                break;
            }
            case CmdType::BindStorageBuffer: {
                const auto* c = static_cast<const cmd::BindStorageBuffer*>(payload);
                const GpuBuffer* b = buffers_.try_get(c->buffer);
                if (!b || c->binding >= kMaxStorageBindings) break;
                storage_bound_[c->binding] = BoundStorage{b->buffer, c->offset, c->size};
                ++storage_gen_;
                break;
            }
            case CmdType::SetStorageData: {
                const auto* c = static_cast<const cmd::SetStorageData*>(payload);
                if (prep.ok && c->binding < kMaxStorageBindings) {
                    storage_bound_[c->binding] = BoundStorage{prep.buffer, prep.offset, prep.size};
                    ++storage_gen_;
                }
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
                ts_.ib_bound = false;
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
                ts_.ib_bound = false;
                break;
            }
            case CmdType::Draw: {
                const auto* d = static_cast<const cmd::Draw*>(payload);
                if (!ts_.ib_valid || !flush_draw_state()) break;
                bind_index();
                wgpuRenderPassEncoderDrawIndexed(ts_.pass, d->index_count, d->instance_count, d->first_index, d->base_vertex,
                                                 d->base_instance);
                ++stats_.draws;
                break;
            }
            case CmdType::DrawNonIndexed: {
                const auto* d = static_cast<const cmd::DrawNonIndexed*>(payload);
                if (!flush_draw_state()) break;
                wgpuRenderPassEncoderDraw(ts_.pass, d->vertex_count, d->instance_count, d->first_vertex, d->base_instance);
                ++stats_.draws;
                break;
            }
            case CmdType::SetDrawArgs:
                ts_.args_first = prep.ok ? prep.offset : 0;
                ts_.args_count = prep.ok ? prep.size : 0;
                break;
            case CmdType::DrawIndirect: {
                // The commands are on the CPU (set_draw_args): issue them as direct draws.
                const auto* d = static_cast<const cmd::DrawIndirect*>(payload);
                MCE_ASSERT_MSG(u64(d->first_command) + d->draw_count <= ts_.args_count,
                               "draw_indirect: commands out of range of the last set_draw_args");
                if (u64(d->first_command) + d->draw_count > ts_.args_count || d->draw_count == 0) break;
                if (!ts_.ib_valid || !flush_draw_state()) break;
                bind_index();
                for (u32 i = 0; i < d->draw_count; ++i) {
                    const DrawIndexedIndirectCommand& c = draw_args_[ts_.args_first + d->first_command + i];
                    if (c.index_count && c.instance_count)
                        wgpuRenderPassEncoderDrawIndexed(ts_.pass, c.index_count, c.instance_count, c.first_index,
                                                         c.base_vertex, c.base_instance);
                }
                ++stats_.draws;
                ++stats_.indirect_draws;
                break;
            }
            case CmdType::DrawIndirectBuffer: {
                const auto* d = static_cast<const cmd::DrawIndirectBuffer*>(payload);
                const GpuBuffer* b = buffers_.try_get(d->buffer);
                const bool ok = b && d->offset + u64(d->draw_count) * sizeof(DrawIndexedIndirectCommand) <= b->size;
                MCE_ASSERT_MSG(ok, "draw_indirect_buffer: stale buffer or commands outside it");
                if (!ok || d->draw_count == 0) break;
                if (!ts_.ib_valid || !flush_draw_state()) break;
                bind_index();
                for (u32 i = 0; i < d->draw_count; ++i)
                    wgpuRenderPassEncoderDrawIndexedIndirect(ts_.pass, b->buffer,
                                                             d->offset + u64(i) * sizeof(DrawIndexedIndirectCommand));
                ++stats_.draws;
                ++stats_.indirect_draws;
                break;
            }
            case CmdType::PushDebugScope:
            case CmdType::PopDebugScope: break;  // scopes may span passes; WebGPU's may not (not mirrored)
            case CmdType::Resolve: resolve(enc, *static_cast<const cmd::Resolve*>(payload)); break;
            case CmdType::WriteTimestamp: break;  // caps().timestamp_queries is false
            case CmdType::BindStorageImage: {
                const auto* c = static_cast<const cmd::BindStorageImage*>(payload);
                MCE_ASSERT_MSG(c->unit < kMaxImageUnits, "bind_storage_image: unit out of range");
                if (c->unit < kMaxImageUnits) {
                    images_bound_[c->unit] = BoundImage{c->texture, c->mip};
                    ++image_gen_;
                }
                break;
            }
            case CmdType::Dispatch: dispatch(enc, static_cast<const cmd::Dispatch*>(payload), nullptr); break;
            case CmdType::DispatchIndirect: dispatch(enc, nullptr, static_cast<const cmd::DispatchIndirect*>(payload)); break;
            case CmdType::Barrier: break;  // each dispatch is its own compute pass; WebGPU orders passes
        }
    }
    if (ts_.pass) end_pass();
}

}  // namespace mce::rhi::webgpu
