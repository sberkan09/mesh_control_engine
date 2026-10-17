// mce_rhi_metal: layer, resources, staging rings, frame fences, uploads and readback (ADR-0028).

#include "metal_device.h"

#include <SDL3/SDL.h>
#include <SDL3/SDL_metal.h>

#include <cmath>
#include <cstdlib>
#include <cstring>

#include "mce/log.h"
#include "mce/math/math.h"
#include "shader_cross.h"

namespace mce::rhi::metal {
namespace {

MTLPixelFormat mtl_format(TextureFormat f, MTLPixelFormat depth_stencil) {
    switch (f) {
        case TextureFormat::RGBA8_UNorm: return MTLPixelFormatRGBA8Unorm;
        case TextureFormat::RGBA8_SRGB: return MTLPixelFormatRGBA8Unorm_sRGB;
        case TextureFormat::BGRA8_UNorm: return MTLPixelFormatBGRA8Unorm;
        case TextureFormat::RGBA16F: return MTLPixelFormatRGBA16Float;
        case TextureFormat::RGBA32F: return MTLPixelFormatRGBA32Float;
        case TextureFormat::R8_UNorm: return MTLPixelFormatR8Unorm;
        case TextureFormat::RG8_UNorm: return MTLPixelFormatRG8Unorm;
        case TextureFormat::R11G11B10F: return MTLPixelFormatRG11B10Float;
        case TextureFormat::BC1_UNorm: return MTLPixelFormatBC1_RGBA;
        case TextureFormat::BC3_UNorm: return MTLPixelFormatBC3_RGBA;
        case TextureFormat::BC4_UNorm: return MTLPixelFormatBC4_RUnorm;
        case TextureFormat::BC1_SRGB: return MTLPixelFormatBC1_RGBA_sRGB;
        case TextureFormat::BC5_UNorm: return MTLPixelFormatBC5_RGUnorm;
        case TextureFormat::BC7_UNorm: return MTLPixelFormatBC7_RGBAUnorm;
        case TextureFormat::BC7_SRGB: return MTLPixelFormatBC7_RGBAUnorm_sRGB;
        case TextureFormat::D32_Float: return MTLPixelFormatDepth32Float;
        case TextureFormat::D24_UNorm_S8: return depth_stencil;  // Apple GPUs have no D24S8
        case TextureFormat::D16_UNorm: return MTLPixelFormatDepth16Unorm;
        case TextureFormat::ASTC_4x4_UNorm: return MTLPixelFormatASTC_4x4_LDR;  // Apple silicon only
        case TextureFormat::ASTC_4x4_SRGB: return MTLPixelFormatASTC_4x4_sRGB;
    }
    return MTLPixelFormatRGBA8Unorm;
}

// Bytes per row as Metal wants it for a tightly packed upload/readback (block rows for BCn).
usize row_bytes(TextureFormat f, u32 w) {
    if (is_compressed_format(f)) return usize((w + 3) / 4) * block_bytes(f);
    return usize(w) * bytes_per_pixel(f);
}

MTLCompareFunction mtl_compare(CompareOp op) {
    switch (op) {
        case CompareOp::Never: return MTLCompareFunctionNever;
        case CompareOp::Less: return MTLCompareFunctionLess;
        case CompareOp::Equal: return MTLCompareFunctionEqual;
        case CompareOp::LessEqual: return MTLCompareFunctionLessEqual;
        case CompareOp::Greater: return MTLCompareFunctionGreater;
        case CompareOp::NotEqual: return MTLCompareFunctionNotEqual;
        case CompareOp::GreaterEqual: return MTLCompareFunctionGreaterEqual;
        case CompareOp::Always: return MTLCompareFunctionAlways;
    }
    return MTLCompareFunctionLessEqual;
}

MTLSamplerAddressMode mtl_wrap(AddressMode m) {
    switch (m) {
        case AddressMode::Repeat: return MTLSamplerAddressModeRepeat;
        case AddressMode::ClampToEdge: return MTLSamplerAddressModeClampToEdge;
        case AddressMode::MirroredRepeat: return MTLSamplerAddressModeMirrorRepeat;
    }
    return MTLSamplerAddressModeClampToEdge;
}

MTLVertexFormat mtl_vertex_format(VertexFormat f) {
    switch (f) {
        case VertexFormat::Float1: return MTLVertexFormatFloat;
        case VertexFormat::Float2: return MTLVertexFormatFloat2;
        case VertexFormat::Float3: return MTLVertexFormatFloat3;
        case VertexFormat::Float4: return MTLVertexFormatFloat4;
        case VertexFormat::UNorm8x4: return MTLVertexFormatUChar4Normalized;
        case VertexFormat::UInt1: return MTLVertexFormatUInt;
        case VertexFormat::SNorm16x2: return MTLVertexFormatShort2Normalized;
        case VertexFormat::SNorm16x4: return MTLVertexFormatShort4Normalized;
        case VertexFormat::UNorm16x2: return MTLVertexFormatUShort2Normalized;
    }
    return MTLVertexFormatFloat;
}

NSString* ns(const char* s) { return [NSString stringWithUTF8String:s ? s : ""]; }

void set_label(id obj, const char* name) {
    if (name && *name) [obj setLabel:ns(name)];
}

// Compiles one translated stage into a library and fetches its entry point; vertex stages are
// specialized on the Y-flip function constant.
id<MTLFunction> make_function(id<MTLDevice> dev, const MslSource& msl, bool vertex, bool flip, const char* name,
                              FixedString<1024>& log) {
    MTLCompileOptions* opts = [[[MTLCompileOptions alloc] init] autorelease];
    opts.languageVersion = MTLLanguageVersion2_3;
    opts.mathMode = MTLMathModeSafe;  // no fast-math reassociation (GL parity, ADR-0007 spirit)
    NSError* err = nil;
    id<MTLLibrary> lib = [dev newLibraryWithSource:ns(msl.c_str()) options:opts error:&err];
    if (!lib) {
        log.append(err ? [[err localizedDescription] UTF8String] : "newLibraryWithSource failed");
        return nil;
    }
    set_label(lib, name);
    id<MTLFunction> fn = nil;
    if (vertex) {
        MTLFunctionConstantValues* cv = [[[MTLFunctionConstantValues alloc] init] autorelease];
        bool v = flip;
        [cv setConstantValue:&v type:MTLDataTypeBool atIndex:kFlipYConstant];
        fn = [lib newFunctionWithName:ns(kEntryPoint) constantValues:cv error:&err];
    } else {
        fn = [lib newFunctionWithName:ns(kEntryPoint)];
    }
    [lib release];
    if (!fn) log.append(err ? [[err localizedDescription] UTF8String] : "entry point main0 missing");
    return fn;
}

// IEEE half -> float for RGBA16F readback.
f32 half_to_float(u16 h) {
    const u32 sign = u32(h >> 15) << 31;
    u32 exp = (h >> 10) & 0x1F;
    u32 mant = h & 0x3FF;
    u32 bits;
    if (exp == 0) {
        if (mant == 0) {
            bits = sign;
        } else {
            exp = 127 - 15 + 1;
            while (!(mant & 0x400)) {
                mant <<= 1;
                --exp;
            }
            bits = sign | (exp << 23) | ((mant & 0x3FF) << 13);
        }
    } else if (exp == 31) {
        bits = sign | 0x7F800000u | (mant << 13);
    } else {
        bits = sign | ((exp - 15 + 127) << 23) | (mant << 13);
    }
    f32 f;
    std::memcpy(&f, &bits, 4);
    return f;
}

// One unsigned small float of R11G11B10F: 5-bit exponent (bias 15), `mbits`-bit mantissa, no sign.
f32 small_float(u32 bits, u32 mbits) {
    const u32 m = bits & ((1u << mbits) - 1u);
    const u32 e = bits >> mbits;
    const f32 frac = f32(m) / f32(1u << mbits);
    if (e == 0) return std::ldexp(frac, -14);
    if (e == 31) return m ? 0.0f : 65504.0f;  // NaN reads as 0, +inf saturates (unorm8 clamps both)
    return std::ldexp(1.0f + frac, i32(e) - 15);
}

u8 unorm8(f32 v) {
    const f32 c = v < 0.0f ? 0.0f : (v > 1.0f ? 1.0f : v);
    return static_cast<u8>(c * 255.0f + 0.5f);
}

}  // namespace

// ---- small types -----------------------------------------------------------------------------

usize StagingRing::alloc(usize size, usize align) {
    const usize start = align_up(head, align);
    if (start + size > per_slot) return kOverflow;
    head = start + size;
    return usize(slot) * per_slot + start;
}

bool PassFormats::operator==(const PassFormats& o) const {
    for (u32 i = 0; i < cmd::kMaxColorAttachments; ++i)
        if (colors[i] != o.colors[i]) return false;
    return depth == o.depth && stencil == o.stencil && samples == o.samples && flip == o.flip;
}

bool MetalFence::is_signaled() { return device->slot_signaled(slot); }
void MetalFence::wait_blocking() { device->wait_slot(slot); }

// ---- creation --------------------------------------------------------------------------------

Result<mem::UniquePtr<RenderDevice>> create_device(platform::Window& window, const DeviceDesc& desc) {
    MetalDevice* dev = mem::make<MetalDevice>(mem::heap(), window, desc);
    mem::UniquePtr<RenderDevice> owned(mem::heap(), dev);
    MCE_TRY(dev->init());
    return owned;
}

MetalDevice::MetalDevice(platform::Window& window, const DeviceDesc& desc) : window_(window), desc_(desc) {
    for (u32 i = 0; i < MCE_FRAMES_IN_FLIGHT; ++i) {
        fence_objs_[i].device = this;
        fence_objs_[i].slot = i;
    }
}

ResultV MetalDevice::init() {
    @autoreleasepool {
        auto* sdl_window = static_cast<SDL_Window*>(window_.native_handle());
        SDL_MetalView view = SDL_Metal_CreateView(sdl_window);
        if (!view) {
            MCE_LOG_ERROR("rhi.metal: SDL_Metal_CreateView failed: {}", SDL_GetError());
            return Error(Errc::Unsupported, "rhi.metal.view", SDL_GetError());
        }
        view_ = view;
        layer_ = (__bridge CAMetalLayer*)SDL_Metal_GetLayer(view);
        init_capture();
        device_ = MTLCreateSystemDefaultDevice();
        if (!layer_ || !device_) return Error(Errc::Unsupported, "rhi.metal.device", "no Metal device");
        queue_ = [device_ newCommandQueue];
        [queue_ setLabel:@"mce.queue"];

        layer_.device = device_;
        layer_.pixelFormat = MTLPixelFormatBGRA8Unorm;  // linear like GL's default framebuffer
        layer_.framebufferOnly = NO;                    // read_pixels(backbuffer) blits from it
        layer_.maximumDrawableCount = 3;
        vsync_ = desc_.vsync;
        layer_.displaySyncEnabled = vsync_;
        if (!device_.depth24Stencil8PixelFormatSupported) depth_stencil_format_ = MTLPixelFormatDepth32Float_Stencil8;
        else depth_stencil_format_ = MTLPixelFormatDepth24Unorm_Stencil8;

        caps_.api_name.append("Metal (MSL 2.3 via SPIRV-Cross)");
        caps_.device_name.append([[device_ name] UTF8String]);
        caps_.api_major = 3;
        caps_.api_minor = 0;
        caps_.max_texture_size = 16384;
        caps_.max_uniform_block_size = 64 * 1024;
        caps_.shader_draw_parameters = true;  // [[base_instance]] is native
        caps_.storage_buffer_alignment = 256;
        // Compute (M-VISUAL 1): every Metal device has it. Apple GPUs cap a threadgroup at
        // maxThreadsPerThreadgroup per dimension and 1024 threads in total; the grid has no limit
        // below 2^32 - 1 threadgroups per dimension.
        const MTLSize tg = [device_ maxThreadsPerThreadgroup];
        caps_.compute = true;
        caps_.max_compute_invocations = static_cast<u32>(math::min<usize>(tg.width, 1024));
        caps_.max_compute_group_size[0] = static_cast<u32>(tg.width);
        caps_.max_compute_group_size[1] = static_cast<u32>(tg.height);
        caps_.max_compute_group_size[2] = static_cast<u32>(tg.depth);
        for (u32& n : caps_.max_compute_group_count) n = 0xFFFFFFFFu;
        caps_.max_storage_image_units = kMaxImageUnits;
        if (@available(macOS 11.0, iOS 16.4, *)) caps_.texture_bc = [device_ supportsBCTextureCompression];
        caps_.texture_bc_srgb = caps_.texture_bc;  // conservative: Metal has no per-format query
        caps_.texture_astc = [device_ supportsFamily:MTLGPUFamilyApple2];
        init_timestamps();
        MCE_LOG_INFO("rhi.metal: {} on {}", caps_.api_name.c_str(), caps_.device_name.c_str());

        MCE_TRY(create_ring(geometry_ring_, desc_.geometry_ring_bytes, "mce.ring.geometry"));
        MCE_TRY(create_ring(uniform_ring_, desc_.uniform_ring_bytes, "mce.ring.uniform"));
    }
    return ok();
}

ResultV MetalDevice::create_ring(StagingRing& ring, usize per_slot, const char* name) {
    ring.per_slot = align_up(per_slot, 256);
    const usize total = ring.per_slot * MCE_FRAMES_IN_FLIGHT;
    ring.buffer = [device_ newBufferWithLength:total options:MTLResourceStorageModeShared |
                                                             MTLResourceCPUCacheModeWriteCombined];
    if (!ring.buffer) return Error(Errc::OutOfMemory, "rhi.metal.ring", name);
    set_label(ring.buffer, name);
    ring.mapped = static_cast<u8*>([ring.buffer contents]);
    return ok();
}

MetalDevice::~MetalDevice() {
    @autoreleasepool {
        release_all();
    }
    if (view_) SDL_Metal_DestroyView(static_cast<SDL_MetalView>(view_));
}

void MetalDevice::release_all() {
    if (!device_) return;
    end_encoders();
    commit_open(true);
    for (u32 s = 0; s < MCE_FRAMES_IN_FLIGHT; ++s) {
        if (slot_cb_[s]) [slot_cb_[s] waitUntilCompleted];
        [slot_cb_[s] release];
        slot_cb_[s] = nil;
    }
    for (MtlBuffer& b : buffers_.dense()) [b.buffer release];
    for (MtlTexture& t : textures_.dense()) [t.texture release];
    for (MtlSampler& s : samplers_.dense()) [s.sampler release];
    for (ImageBinding& b : images_) release_image(b);
    for (MtlPipeline& p : pipelines_.dense()) {
        [p.cs release];
        [p.vs release];
        [p.vs_flip release];
        [p.fs release];
        [p.vertex release];
    }
    for (PsoEntry& e : psos_) [e.pso release];
    for (DepthEntry& e : depth_states_) [e.state release];
    for (TimestampSlot& t : ts_slots_) [t.samples release];
    [geometry_ring_.buffer release];
    [uniform_ring_.buffer release];
    [drawable_ release];
    [depth_tex_ release];
    [queue_ release];
    [device_ release];
    drawable_ = nil;
    depth_tex_ = nil;
    queue_ = nil;
    device_ = nil;
}

// ---- command buffer plumbing -----------------------------------------------------------------

id<MTLCommandBuffer> MetalDevice::command_buffer() {
    if (!open_cb_) {
        open_cb_ = [[queue_ commandBuffer] retain];
        [open_cb_ setLabel:@"mce.frame"];
    }
    return open_cb_;
}

id<MTLBlitCommandEncoder> MetalDevice::blit_encoder() {
    if (blit_) return blit_;
    end_compute();
    u32 start = 0, end = 0;
    if (next_encoder_samples(start, end)) {
        MTLBlitPassDescriptor* bp = [MTLBlitPassDescriptor blitPassDescriptor];
        bp.sampleBufferAttachments[0].sampleBuffer = ts_slots_[slot_].samples;
        bp.sampleBufferAttachments[0].startOfEncoderSampleIndex = start;
        bp.sampleBufferAttachments[0].endOfEncoderSampleIndex = end;
        blit_ = [[command_buffer() blitCommandEncoderWithDescriptor:bp] retain];
    } else {
        blit_ = [[command_buffer() blitCommandEncoder] retain];
    }
    return blit_;
}

void MetalDevice::end_blit() {
    if (!blit_) return;
    [blit_ endEncoding];
    [blit_ release];
    blit_ = nil;
}

id<MTLComputeCommandEncoder> MetalDevice::compute_encoder() {
    if (compute_) return compute_;
    end_blit();
    // Serial dispatch: each dispatch of the encoder sees the writes of the ones before it.
    MTLComputePassDescriptor* cp = [MTLComputePassDescriptor computePassDescriptor];
    cp.dispatchType = MTLDispatchTypeSerial;
    u32 start = 0, end = 0;
    if (next_encoder_samples(start, end)) {
        cp.sampleBufferAttachments[0].sampleBuffer = ts_slots_[slot_].samples;
        cp.sampleBufferAttachments[0].startOfEncoderSampleIndex = start;
        cp.sampleBufferAttachments[0].endOfEncoderSampleIndex = end;
    }
    compute_ = [[command_buffer() computeCommandEncoderWithDescriptor:cp] retain];
    [compute_ setLabel:@"mce.compute"];
    return compute_;
}

void MetalDevice::end_compute() {
    if (!compute_) return;
    [compute_ endEncoding];
    [compute_ release];
    compute_ = nil;
}

void MetalDevice::commit_open(bool wait) {
    end_encoders();
    if (!open_cb_) return;
    id<MTLCommandBuffer> cb = open_cb_;
    open_cb_ = nil;
    [cb commit];
    if (wait) {
        [cb waitUntilCompleted];
        if ([cb status] == MTLCommandBufferStatusError) {
            ++validation_errors_;
            MCE_LOG_ERROR("rhi.metal: command buffer failed: {}",
                          [cb error] ? [[[cb error] localizedDescription] UTF8String] : "unknown");
        }
    }
    // The slot fence is the last command buffer committed for the slot: the queue completes
    // command buffers in commit order, so it covers everything before it.
    [slot_cb_[slot_] release];
    slot_cb_[slot_] = cb;  // keeps the +1 from retain in command_buffer()
}

void MetalDevice::upload_to_buffer(id<MTLBuffer> dst, usize offset, Span<const byte> data) {
    if (data.empty()) return;
    id<MTLBuffer> staging = [device_ newBufferWithBytes:data.data() length:data.size()
                                                options:MTLResourceStorageModeShared];
    [blit_encoder() copyFromBuffer:staging sourceOffset:0 toBuffer:dst destinationOffset:offset size:data.size()];
    [staging release];  // the command buffer holds it until the copy executes
}

void MetalDevice::upload_to_texture(const MtlTexture& t, const TextureRegion& region, Span<const byte> data) {
    const u32 mw = math::max(1u, t.size.width >> region.mip);
    const u32 mh = math::max(1u, t.size.height >> region.mip);
    const u32 rw = region.width ? region.width : mw;
    const u32 rh = region.height ? region.height : mh;
    const usize row = row_bytes(t.format, rw);
    const usize bytes = level_bytes(t.format, rw, rh);
    id<MTLBuffer> staging = [device_ newBufferWithBytes:data.data() length:bytes options:MTLResourceStorageModeShared];
    [blit_encoder() copyFromBuffer:staging
                      sourceOffset:0
                 sourceBytesPerRow:row
               sourceBytesPerImage:bytes
                        sourceSize:MTLSizeMake(rw, rh, 1)
                         toTexture:t.texture
                  destinationSlice:region.layer
                  destinationLevel:region.mip
                 destinationOrigin:MTLOriginMake(region.x, region.y, 0)];
    [staging release];
}

// ---- resources -------------------------------------------------------------------------------

BufferHandle MetalDevice::create_buffer(const BufferDesc& desc, Span<const byte> initial_data) {
    MCE_ASSERT_MSG(desc.size_bytes > 0, "create_buffer: size 0");
    MCE_ASSERT_MSG(initial_data.empty() || initial_data.size() <= desc.size_bytes, "initial data larger than buffer");
    MtlBuffer b;
    b.size = desc.size_bytes;
    b.usage = desc.usage;
    b.buffer = [device_ newBufferWithLength:desc.size_bytes options:MTLResourceStorageModeShared];
    // A fresh buffer is referenced by no command buffer yet: fill it directly.
    if (!initial_data.empty()) std::memcpy([b.buffer contents], initial_data.data(), initial_data.size());
    set_label(b.buffer, desc.debug_name);
    return buffers_.insert(b);
}

void MetalDevice::update_buffer(BufferHandle h, u64 offset, Span<const byte> data) {
    const MtlBuffer* b = buffers_.try_get(h);
    MCE_ASSERT_MSG(b, "update_buffer: stale handle");
    if (!b) return;
    MCE_ASSERT_MSG(offset + data.size() <= b->size, "update_buffer: out of range");
    if (offset + data.size() > b->size) return;
    @autoreleasepool {
        upload_to_buffer(b->buffer, offset, data);
    }
}

TextureHandle MetalDevice::create_texture(const TextureDesc& desc, Span<const byte> initial_data) {
    MCE_ASSERT_MSG(desc.size.width > 0 && desc.size.height > 0, "create_texture: empty extent");
    const bool array = desc.dimension == TextureDimension::Tex2DArray;
    MCE_ASSERT_MSG(array || desc.array_layers <= 1, "create_texture: layers > 1 need TextureDimension::Tex2DArray");
    MtlTexture t;
    t.size = desc.size;
    t.format = desc.format;
    t.dimension = desc.dimension;
    t.usage = desc.usage;
    t.mips = desc.mip_levels ? desc.mip_levels : 1;
    t.layers = array ? (desc.array_layers ? desc.array_layers : 1) : 1;
    t.samples = desc.samples > 1 ? desc.samples : 1;
    MCE_ASSERT_MSG(t.samples == 1 || (!array && t.mips == 1 && initial_data.empty()),
                   "create_texture: multisampled textures are single-mip 2D render targets");
    MCE_ASSERT_MSG(desc.usage != TextureUsage::SampledStorage ||
                       (t.samples == 1 && !is_depth_format(desc.format) && !is_compressed_format(desc.format) &&
                        desc.format != TextureFormat::RGBA8_SRGB && desc.format != TextureFormat::BGRA8_UNorm),
                   "create_texture: storage images need a single-sample, linear, uncompressed color format");
    @autoreleasepool {
        MTLTextureDescriptor* td = [[[MTLTextureDescriptor alloc] init] autorelease];
        td.textureType = t.samples > 1 ? MTLTextureType2DMultisample
                                       : (array ? MTLTextureType2DArray : MTLTextureType2D);
        td.pixelFormat = mtl_format(desc.format, depth_stencil_format_);
        td.width = desc.size.width;
        td.height = desc.size.height;
        td.mipmapLevelCount = t.mips;
        td.arrayLength = t.layers;
        td.sampleCount = t.samples;
        td.storageMode = MTLStorageModePrivate;
        MTLTextureUsage usage = MTLTextureUsageShaderRead;
        if (desc.usage != TextureUsage::Sampled) usage |= MTLTextureUsageRenderTarget;
        // Storage images (compute); a mip other than 0 binds through a view of that level.
        if (desc.usage == TextureUsage::SampledStorage) usage |= MTLTextureUsageShaderWrite;
        td.usage = usage;
        t.texture = [device_ newTextureWithDescriptor:td];
        set_label(t.texture, desc.debug_name);
        if (!initial_data.empty()) {
            const usize need = level_bytes(desc.format, desc.size.width, desc.size.height) * t.layers;
            MCE_ASSERT_MSG(initial_data.size() >= need, "create_texture: initial data smaller than mip 0");
            MCE_ASSERT_MSG(!is_depth_format(desc.format), "create_texture: depth formats take no initial data");
            if (initial_data.size() >= need && !is_depth_format(desc.format)) {
                const usize layer_bytes = need / t.layers;
                for (u32 l = 0; l < t.layers; ++l) {
                    TextureRegion r;
                    r.layer = l;
                    upload_to_texture(t, r, initial_data.subspan(usize(l) * layer_bytes, layer_bytes));
                }
            }
        }
    }
    for (u32 m = 0; m < t.mips; ++m)
        t.bytes += level_bytes(desc.format, math::max(1u, desc.size.width >> m), math::max(1u, desc.size.height >> m)) *
                   t.layers;
    t.bytes *= t.samples;
    texture_bytes_ += t.bytes;
    return textures_.insert(t);
}

void MetalDevice::update_texture(TextureHandle h, const TextureRegion& region, Span<const byte> data) {
    const MtlTexture* t = textures_.try_get(h);
    MCE_ASSERT_MSG(t, "update_texture: stale handle");
    if (!t) return;
    MCE_ASSERT_MSG(region.mip < t->mips && region.layer < t->layers, "update_texture: mip/layer out of range");
    if (region.mip >= t->mips || region.layer >= t->layers || is_depth_format(t->format)) return;
    const u32 mw = math::max(1u, t->size.width >> region.mip);
    const u32 mh = math::max(1u, t->size.height >> region.mip);
    const u32 rw = region.width ? region.width : mw;
    const u32 rh = region.height ? region.height : mh;
    const bool in_range = region.x + rw <= mw && region.y + rh <= mh;
    MCE_ASSERT_MSG(in_range, "update_texture: rectangle outside the mip level");
    const usize need = level_bytes(t->format, rw, rh);
    MCE_ASSERT_MSG(data.size() >= need, "update_texture: data smaller than the region");
    if (!in_range || data.size() < need) return;
    @autoreleasepool {
        upload_to_texture(*t, region, data);
    }
}

SamplerHandle MetalDevice::create_sampler(const SamplerDesc& desc) {
    MtlSampler s;
    @autoreleasepool {
        MTLSamplerDescriptor* sd = [[[MTLSamplerDescriptor alloc] init] autorelease];
        sd.minFilter = desc.min_filter == Filter::Nearest ? MTLSamplerMinMagFilterNearest : MTLSamplerMinMagFilterLinear;
        sd.magFilter = desc.mag_filter == Filter::Nearest ? MTLSamplerMinMagFilterNearest : MTLSamplerMinMagFilterLinear;
        switch (desc.mip) {
            case MipFilter::None: sd.mipFilter = MTLSamplerMipFilterNotMipmapped; break;
            case MipFilter::Nearest: sd.mipFilter = MTLSamplerMipFilterNearest; break;
            case MipFilter::Linear: sd.mipFilter = MTLSamplerMipFilterLinear; break;
        }
        sd.sAddressMode = mtl_wrap(desc.wrap_s);
        sd.tAddressMode = mtl_wrap(desc.wrap_t);
        sd.rAddressMode = MTLSamplerAddressModeClampToEdge;
        if (desc.max_aniso > 1.0f) sd.maxAnisotropy = static_cast<NSUInteger>(math::min(desc.max_aniso, 16.0f));
        if (desc.compare) sd.compareFunction = mtl_compare(desc.compare_op);
        s.sampler = [device_ newSamplerStateWithDescriptor:sd];
    }
    return samplers_.insert(s);
}

Result<PipelineHandle> MetalDevice::create_pipeline(const GraphicsPipelineDesc& desc) {
    FixedString<1024> log;
    auto vs_msl = translate_glsl(ShaderStage::Vertex, desc.vertex_shader.glsl, desc.vertex_shader.debug_name, log);
    if (!vs_msl) {
        MCE_LOG_ERROR("rhi.metal: vertex shader '{}' failed:\n{}", desc.vertex_shader.debug_name, log.c_str());
        return Error(Errc::ShaderCompile, "rhi.pipeline.vertex", desc.vertex_shader.debug_name);
    }
    auto fs_msl = translate_glsl(ShaderStage::Fragment, desc.fragment_shader.glsl, desc.fragment_shader.debug_name, log);
    if (!fs_msl) {
        MCE_LOG_ERROR("rhi.metal: fragment shader '{}' failed:\n{}", desc.fragment_shader.debug_name, log.c_str());
        return Error(Errc::ShaderCompile, "rhi.pipeline.fragment", desc.fragment_shader.debug_name);
    }

    MtlPipeline p;
    p.state = desc.state;
    p.stride = desc.vertex.stride;
    p.name.append(desc.debug_name ? desc.debug_name : "");
    @autoreleasepool {
        p.vs = make_function(device_, vs_msl.value(), true, false, desc.vertex_shader.debug_name, log);
        p.vs_flip = p.vs ? make_function(device_, vs_msl.value(), true, true, desc.vertex_shader.debug_name, log) : nil;
        if (!p.vs || !p.vs_flip) {
            MCE_LOG_ERROR("rhi.metal: vertex shader '{}' MSL failed:\n{}\n{}", desc.vertex_shader.debug_name,
                          log.c_str(), vs_msl.value().c_str());
            [p.vs release];
            return Error(Errc::ShaderCompile, "rhi.pipeline.vertex", desc.vertex_shader.debug_name);
        }
        p.fs = make_function(device_, fs_msl.value(), false, false, desc.fragment_shader.debug_name, log);
        if (!p.fs) {
            MCE_LOG_ERROR("rhi.metal: fragment shader '{}' MSL failed:\n{}\n{}", desc.fragment_shader.debug_name,
                          log.c_str(), fs_msl.value().c_str());
            [p.vs release];
            [p.vs_flip release];
            return Error(Errc::ShaderCompile, "rhi.pipeline.fragment", desc.fragment_shader.debug_name);
        }
        if (!desc.vertex.attrs.empty()) {
            MTLVertexDescriptor* vd = [[MTLVertexDescriptor alloc] init];
            for (const VertexAttribute& a : desc.vertex.attrs) {
                vd.attributes[a.location].format = mtl_vertex_format(a.format);
                vd.attributes[a.location].offset = a.offset;
                vd.attributes[a.location].bufferIndex = kVertexBufferIndex;
            }
            vd.layouts[kVertexBufferIndex].stride = desc.vertex.stride;
            vd.layouts[kVertexBufferIndex].stepFunction = MTLVertexStepFunctionPerVertex;
            p.vertex = vd;
        }
    }
    return pipelines_.insert(p);
}

Result<PipelineHandle> MetalDevice::create_compute_pipeline(const ComputePipelineDesc& desc) {
    FixedString<1024> log;
    const char* name = desc.compute_shader.debug_name;
    auto cs_msl = translate_glsl(ShaderStage::Compute, desc.compute_shader.glsl, name, log);
    if (!cs_msl) {
        MCE_LOG_ERROR("rhi.metal: compute shader '{}' failed:\n{}", name, log.c_str());
        return Error(Errc::ShaderCompile, "rhi.pipeline.compute", name);
    }
    const MslSource& msl = cs_msl.value();
    MtlPipeline p;
    p.name.append(desc.debug_name ? desc.debug_name : "");
    p.threads = MTLSizeMake(msl.local_size[0], msl.local_size[1], msl.local_size[2]);
    @autoreleasepool {
        id<MTLFunction> fn = make_function(device_, msl, false, false, name, log);
        if (!fn) {
            MCE_LOG_ERROR("rhi.metal: compute shader '{}' MSL failed:\n{}\n{}", name, log.c_str(), msl.c_str());
            return Error(Errc::ShaderCompile, "rhi.pipeline.compute", name);
        }
        MTLComputePipelineDescriptor* d = [[[MTLComputePipelineDescriptor alloc] init] autorelease];
        d.computeFunction = fn;
        set_label(d, p.name.c_str());
        NSError* err = nil;
        p.cs = [device_ newComputePipelineStateWithDescriptor:d options:MTLPipelineOptionNone reflection:nil error:&err];
        [fn release];
        if (!p.cs) {
            MCE_LOG_ERROR("rhi.metal: compute pipeline '{}' failed: {}", p.name.c_str(),
                          err ? [[err localizedDescription] UTF8String] : "unknown");
            return Error(Errc::PipelineCreate, "rhi.pipeline.compute", desc.debug_name);
        }
        const usize total = usize(msl.local_size[0]) * msl.local_size[1] * msl.local_size[2];
        if (total > [p.cs maxTotalThreadsPerThreadgroup]) {
            MCE_LOG_ERROR("rhi.metal: compute pipeline '{}': local size {} exceeds the pipeline's {} threads",
                          p.name.c_str(), total, usize([p.cs maxTotalThreadsPerThreadgroup]));
            [p.cs release];
            return Error(Errc::PipelineCreate, "rhi.pipeline.compute", desc.debug_name);
        }
    }
    return pipelines_.insert(p);
}

id<MTLRenderPipelineState> MetalDevice::pso_for(PipelineHandle h, const MtlPipeline& p, const PassFormats& f) {
    for (const PsoEntry& e : psos_)
        if (e.pipeline == h && e.formats == f) return e.pso;
    PsoEntry entry;
    entry.pipeline = h;
    entry.formats = f;
    MTLRenderPipelineDescriptor* d = [[[MTLRenderPipelineDescriptor alloc] init] autorelease];
    d.vertexFunction = f.flip ? p.vs_flip : p.vs;
    d.fragmentFunction = p.fs;
    d.vertexDescriptor = p.vertex;
    d.rasterSampleCount = f.samples;
    d.depthAttachmentPixelFormat = f.depth;
    d.stencilAttachmentPixelFormat = f.stencil;
    switch (p.state.topology) {
        case PrimitiveTopology::Triangles: d.inputPrimitiveTopology = MTLPrimitiveTopologyClassTriangle; break;
        case PrimitiveTopology::Lines:
        case PrimitiveTopology::LineStrip: d.inputPrimitiveTopology = MTLPrimitiveTopologyClassLine; break;
        case PrimitiveTopology::Points: d.inputPrimitiveTopology = MTLPrimitiveTopologyClassPoint; break;
    }
    for (u32 i = 0; i < cmd::kMaxColorAttachments; ++i) {
        if (f.colors[i] == MTLPixelFormatInvalid) continue;
        MTLRenderPipelineColorAttachmentDescriptor* c = d.colorAttachments[i];
        c.pixelFormat = f.colors[i];
        switch (p.state.blend.mode) {
            case BlendMode::Opaque: c.blendingEnabled = NO; break;
            case BlendMode::PremultipliedAlpha:
                c.blendingEnabled = YES;
                c.sourceRGBBlendFactor = c.sourceAlphaBlendFactor = MTLBlendFactorOne;
                c.destinationRGBBlendFactor = c.destinationAlphaBlendFactor = MTLBlendFactorOneMinusSourceAlpha;
                break;
            case BlendMode::Alpha:
                c.blendingEnabled = YES;
                c.sourceRGBBlendFactor = MTLBlendFactorSourceAlpha;
                c.destinationRGBBlendFactor = MTLBlendFactorOneMinusSourceAlpha;
                c.sourceAlphaBlendFactor = MTLBlendFactorOne;
                c.destinationAlphaBlendFactor = MTLBlendFactorOneMinusSourceAlpha;
                break;
            case BlendMode::Additive:
                c.blendingEnabled = YES;
                c.sourceRGBBlendFactor = c.sourceAlphaBlendFactor = MTLBlendFactorOne;
                c.destinationRGBBlendFactor = c.destinationAlphaBlendFactor = MTLBlendFactorOne;
                break;
        }
    }
    set_label(d, p.name.c_str());
    NSError* err = nil;
    entry.pso = [device_ newRenderPipelineStateWithDescriptor:d error:&err];
    if (!entry.pso) {
        ++validation_errors_;
        MCE_LOG_ERROR("rhi.metal: pipeline '{}' state failed: {}", p.name.c_str(),
                      err ? [[err localizedDescription] UTF8String] : "unknown");
    }
    psos_.push_back(entry);
    return entry.pso;
}

id<MTLDepthStencilState> MetalDevice::depth_state(const DepthState& s) {
    const DepthKey key{s.test, s.write, s.compare};
    for (const DepthEntry& e : depth_states_)
        if (e.key.test == key.test && e.key.write == key.write && e.key.compare == key.compare) return e.state;
    MTLDepthStencilDescriptor* d = [[[MTLDepthStencilDescriptor alloc] init] autorelease];
    // GL: depth writes only happen with the depth test enabled.
    d.depthCompareFunction = s.test ? mtl_compare(s.compare) : MTLCompareFunctionAlways;
    d.depthWriteEnabled = s.test && s.write;
    DepthEntry e;
    e.key = key;
    e.state = [device_ newDepthStencilStateWithDescriptor:d];
    depth_states_.push_back(e);
    return e.state;
}

void MetalDevice::destroy_buffer(BufferHandle h) {
    if (MtlBuffer* b = buffers_.try_get(h)) {
        for (BufferBinding& s : storage_)
            if (s.buffer == b->buffer) s = BufferBinding{};
        [b->buffer release];
        buffers_.erase(h);
    }
}

void MetalDevice::destroy_texture(TextureHandle h) {
    MtlTexture* t = textures_.try_get(h);
    if (!t) return;
    for (TextureBinding& b : textures_bound_)
        if (b.texture == t->texture) b = TextureBinding{};
    for (ImageBinding& b : images_)
        if (b.base == t->texture) release_image(b);
    texture_bytes_ -= t->bytes;
    [t->texture release];
    textures_.erase(h);
}

void MetalDevice::destroy_sampler(SamplerHandle h) {
    if (MtlSampler* s = samplers_.try_get(h)) {
        for (TextureBinding& b : textures_bound_)
            if (b.sampler == s->sampler) b.sampler = nil;
        [s->sampler release];
        samplers_.erase(h);
    }
}

void MetalDevice::destroy_pipeline(PipelineHandle h) {
    MtlPipeline* p = pipelines_.try_get(h);
    if (!p) return;
    for (usize i = 0; i < psos_.size();) {
        if (psos_[i].pipeline == h) {
            [psos_[i].pso release];
            psos_[i] = psos_.back();
            psos_.pop_back();
        } else {
            ++i;
        }
    }
    [p->cs release];
    [p->vs release];
    [p->vs_flip release];
    [p->fs release];
    [p->vertex release];
    pipelines_.erase(h);
}

Extent2D MetalDevice::texture_size(TextureHandle h) const {
    const MtlTexture* t = textures_.try_get(h);
    return t ? t->size : Extent2D{};
}

TextureDesc MetalDevice::texture_desc(TextureHandle h) const {
    TextureDesc d;
    const MtlTexture* t = textures_.try_get(h);
    if (!t) return d;
    d.size = t->size;
    d.mip_levels = t->mips;
    d.array_layers = t->layers;
    d.dimension = t->dimension;
    d.format = t->format;
    d.usage = t->usage;
    d.samples = t->samples;
    return d;
}

// ---- frames ----------------------------------------------------------------------------------

bool MetalDevice::slot_signaled(u32 slot) {
    id<MTLCommandBuffer> cb = slot_cb_[slot];
    if (!cb) return true;
    const MTLCommandBufferStatus s = [cb status];
    return s == MTLCommandBufferStatusCompleted || s == MTLCommandBufferStatusError;
}

void MetalDevice::wait_slot(u32 slot) {
    if (slot_cb_[slot]) [slot_cb_[slot] waitUntilCompleted];
}

mem::FrameSlotFence* MetalDevice::fence_for_frame(u64 frame_index) {
    return &fence_objs_[frame_index % MCE_FRAMES_IN_FLIGHT];
}

void MetalDevice::begin_frame(u64 frame_index) {
    MCE_ASSERT_MSG(!in_frame_, "begin_frame twice");
    frame_index_ = frame_index;
    slot_ = static_cast<u32>(frame_index % MCE_FRAMES_IN_FLIGHT);
    wait_slot(slot_);
    if (slot_cb_[slot_] && [slot_cb_[slot_] status] == MTLCommandBufferStatusError) {
        ++validation_errors_;
        MCE_LOG_ERROR("rhi.metal: frame slot {} command buffer failed", slot_);
    }
    collect_timestamps(slot_);
    ts_slots_[slot_].frame_index = frame_index;
    if (frame_index == capture_frame_) begin_capture();
    geometry_ring_.slot = uniform_ring_.slot = slot_;
    geometry_ring_.head = uniform_ring_.head = 0;
    stats_ = FrameStats{};
    in_frame_ = true;
}

void MetalDevice::end_frame() {
    MCE_ASSERT_MSG(in_frame_, "end_frame without begin_frame");
    @autoreleasepool {
        command_buffer();  // the slot always gets a fence, even for an empty frame
        commit_open(false);
    }
    stats_.geometry_bytes = geometry_ring_.head;
    stats_.uniform_bytes = uniform_ring_.head;
    last_stats_ = stats_;
    in_frame_ = false;
}

void MetalDevice::present(bool vsync) {
    @autoreleasepool {
        if (vsync != vsync_) {
            vsync_ = vsync;
            layer_.displaySyncEnabled = vsync;
        }
        if (!drawable_) return;
        commit_open(false);  // work recorded after end_frame (uploads) goes before the present
        id<MTLCommandBuffer> cb = [queue_ commandBuffer];
        [cb setLabel:@"mce.present"];
        [cb presentDrawable:drawable_];
        [cb commit];
        [drawable_ release];
        drawable_ = nil;
    }
    if (capturing_) end_capture();
}

// ---- frame captures ---------------------------------------------------------------------------

void MetalDevice::init_capture() {
    const char* spec = std::getenv("MCE_METAL_CAPTURE");
    if (!spec || !*spec) return;
    char* end = nullptr;
    const unsigned long long frame = std::strtoull(spec, &end, 10);
    if (end == spec || *end != ':' || !end[1]) {
        MCE_LOG_WARN("rhi.metal: MCE_METAL_CAPTURE wants \"<frame>:<path>.gputrace\", got '{}'", spec);
        return;
    }
    capture_frame_ = frame;
    capture_path_.assign(StringView(end + 1));
    // Metal reads it when the framework loads (SDL has loaded it by now), so it has to come from the
    // environment that launched the process.
    const char* enabled = std::getenv("MTL_CAPTURE_ENABLED");
    if (!enabled || std::strcmp(enabled, "1") != 0)
        MCE_LOG_WARN("rhi.metal: MCE_METAL_CAPTURE needs MTL_CAPTURE_ENABLED=1 in the environment too");
}

void MetalDevice::begin_capture() {
    MTLCaptureManager* m = [MTLCaptureManager sharedCaptureManager];
    if (![m supportsDestination:MTLCaptureDestinationGPUTraceDocument]) {
        MCE_LOG_WARN("rhi.metal: GPU trace capture unavailable (launch with MTL_CAPTURE_ENABLED=1)");
        return;
    }
    MTLCaptureDescriptor* d = [[MTLCaptureDescriptor new] autorelease];
    d.captureObject = device_;
    d.destination = MTLCaptureDestinationGPUTraceDocument;
    d.outputURL = [NSURL fileURLWithPath:[NSString stringWithUTF8String:capture_path_.c_str()]];
    NSError* err = nil;
    if (![m startCaptureWithDescriptor:d error:&err]) {
        MCE_LOG_ERROR("rhi.metal: capture of frame {} failed to start: {}", capture_frame_,
                      err ? [[err localizedDescription] UTF8String] : "unknown");
        return;
    }
    capturing_ = true;
    MCE_LOG_INFO("rhi.metal: capturing frame {} into {}", capture_frame_, capture_path_.c_str());
}

void MetalDevice::end_capture() {
    @autoreleasepool {
        commit_open(true);  // everything recorded for the frame lands in the trace
    }
    [[MTLCaptureManager sharedCaptureManager] stopCapture];
    capturing_ = false;
    MCE_LOG_INFO("rhi.metal: wrote {} (open it in Xcode)", capture_path_.c_str());
}

// ---- GPU timestamps -----------------------------------------------------------------------------

void MetalDevice::init_timestamps() {
    caps_.timestamp_queries = false;
    if (![device_ supportsCounterSampling:MTLCounterSamplingPointAtStageBoundary]) return;
    id<MTLCounterSet> set = nil;
    for (id<MTLCounterSet> s in [device_ counterSets])
        if ([[s name] isEqualToString:MTLCommonCounterSetTimestamp]) set = s;
    if (!set) return;
    MTLCounterSampleBufferDescriptor* d = [[MTLCounterSampleBufferDescriptor new] autorelease];
    d.counterSet = set;
    d.storageMode = MTLStorageModeShared;  // resolved on the CPU once the slot's fence passes
    d.sampleCount = kMaxSampledEncoders * 2;
    for (u32 s = 0; s < MCE_FRAMES_IN_FLIGHT; ++s) {
        d.label = [NSString stringWithFormat:@"mce.timestamps.%u", s];
        NSError* err = nil;
        ts_slots_[s].samples = [device_ newCounterSampleBufferWithDescriptor:d error:&err];
        if (!ts_slots_[s].samples) {
            MCE_LOG_WARN("rhi.metal: no timestamp sample buffer ({}); GPU timings disabled",
                         err ? [[err localizedDescription] UTF8String] : "unknown");
            for (TimestampSlot& t : ts_slots_) {
                [t.samples release];
                t.samples = nil;
            }
            return;
        }
    }
    [device_ sampleTimestamps:&calib_cpu_ gpuTimestamp:&calib_gpu_];
    caps_.timestamp_queries = true;
}

bool MetalDevice::next_encoder_samples(u32& start, u32& end) {
    if (!timestamps_active_ || !in_frame_) return false;
    TimestampSlot& t = ts_slots_[slot_];
    if (t.encoders >= kMaxSampledEncoders) return false;
    start = t.encoders * 2;
    end = start + 1;
    ++t.encoders;
    return true;
}

void MetalDevice::exec_write_timestamp(u32 index) {
    if (!caps_.timestamp_queries || index >= kMaxTimestamps) return;
    timestamps_active_ = true;
    end_blit();  // the open blit encoder (uploads) is work that comes before this timestamp
    end_compute();  // so is a run of dispatches
    TimestampSlot& t = ts_slots_[slot_];
    t.position[index] = t.encoders;
    t.written |= 1u << index;
}

// The slot's fence has passed, so every sample its frame wrote is final. A timestamp at position
// k is the latest sample of encoders [0, k): GL's "all prior work done". Empty encoders sample 0,
// and passes overlap on the GPU, so the latest *valid* sample is used, which also keeps the values
// monotonic in recording order. A timestamp before any timed encoder takes the first valid sample
// after it.
void MetalDevice::collect_timestamps(u32 slot) {
    TimestampSlot& t = ts_slots_[slot];
    if (t.written == 0) {
        t.encoders = 0;
        return;
    }
    const u32 count = t.encoders < kMaxSampledEncoders ? t.encoders : kMaxSampledEncoders;
    GpuTimestamps out;
    out.frame_index = t.frame_index;
    if (count > 0) {
        @autoreleasepool {
            NSData* data = [t.samples resolveCounterRange:NSMakeRange(0, count * 2)];
            const auto* v = data ? static_cast<const MTLCounterResultTimestamp*>([data bytes]) : nullptr;
            const usize n = data ? [data length] / sizeof(MTLCounterResultTimestamp) : 0;
            auto valid = [&](usize i) { return i < n && v[i].timestamp != 0 && v[i].timestamp != MTLCounterErrorValue; };

            MTLTimestamp cpu = 0, gpu = 0;
            [device_ sampleTimestamps:&cpu gpuTimestamp:&gpu];
            if (gpu > calib_gpu_ && cpu > calib_cpu_) gpu_to_ns_ = f64(cpu - calib_cpu_) / f64(gpu - calib_gpu_);

            for (u32 i = 0; i < kMaxTimestamps; ++i) {
                if (!((t.written >> i) & 1u)) continue;
                const usize k = t.position[i] * 2;  // samples [0, k) belong to earlier encoders
                u64 best = 0;
                for (usize s = 0; s < k && s < n; ++s)
                    if (valid(s) && v[s].timestamp > best) best = v[s].timestamp;
                if (best == 0)
                    for (usize s = k; s < n; ++s)
                        if (valid(s)) {
                            best = v[s].timestamp;
                            break;
                        }
                if (best == 0) continue;
                out.written |= 1u << i;
                out.ns[i] = u64(f64(best - calib_gpu_) * gpu_to_ns_);
            }
        }
    }
    timestamps_ = out;
    t.written = 0;
    t.encoders = 0;
}

Extent2D MetalDevice::backbuffer_size() const {
    const platform::PixelSize s = window_.framebuffer_size();
    return Extent2D{s.width, s.height};
}

id<CAMetalDrawable> MetalDevice::acquire_drawable() {
    if (drawable_) return drawable_;
    const Extent2D size = backbuffer_size();
    if (size.width == 0 || size.height == 0) return nil;
    const CGSize want = CGSizeMake(size.width, size.height);
    if (!CGSizeEqualToSize(layer_.drawableSize, want)) layer_.drawableSize = want;
    drawable_ = [[layer_ nextDrawable] retain];
    return drawable_;
}

id<MTLTexture> MetalDevice::default_depth(Extent2D size) {
    if (depth_tex_ && depth_size_ == size) return depth_tex_;
    [depth_tex_ release];
    MTLTextureDescriptor* td = [MTLTextureDescriptor texture2DDescriptorWithPixelFormat:depth_stencil_format_
                                                                                  width:size.width
                                                                                 height:size.height
                                                                              mipmapped:NO];
    td.storageMode = MTLStorageModePrivate;
    td.usage = MTLTextureUsageRenderTarget;
    depth_tex_ = [device_ newTextureWithDescriptor:td];
    [depth_tex_ setLabel:@"mce.backbuffer.depth"];
    depth_size_ = size;
    return depth_tex_;
}

ResultV MetalDevice::read_pixels(TextureHandle target, Rect2D rect, Span<u8> out) {
    const usize row = usize(rect.width) * 4;
    if (out.size() < row * rect.height) return Error(Errc::InvalidArgument, "rhi.read_pixels", "output too small");
    id<MTLTexture> tex = nil;
    TextureFormat fmt = TextureFormat::BGRA8_UNorm;
    Extent2D size;
    const bool backbuffer = target.is_null();
    if (backbuffer) {
        if (!drawable_) return Error(Errc::NotFound, "rhi.read_pixels", "nothing rendered to the backbuffer this frame");
        tex = [drawable_ texture];
        size = Extent2D{static_cast<u32>([tex width]), static_cast<u32>([tex height])};
    } else {
        const MtlTexture* t = textures_.try_get(target);
        if (!t) return Error(Errc::NotFound, "rhi.read_pixels", "stale texture handle");
        // The GL backend cannot read these as RGBA8 either (its texture readback rejects them)
        // (multisampled and depth textures are GL errors there), so neither backend offers them.
        if (t->samples > 1 || is_depth_format(t->format) || is_compressed_format(t->format))
            return Error(Errc::Unsupported, "rhi.read_pixels", "format not readable");
        tex = t->texture;
        fmt = t->format;
        size = t->size;
    }
    if (rect.x < 0 || rect.y < 0 || u32(rect.x) + rect.width > size.width || u32(rect.y) + rect.height > size.height)
        return Error(Errc::InvalidArgument, "rhi.read_pixels", "rect outside target");
    if (rect.width == 0 || rect.height == 0) return ok();

    // Texture targets hold GL's layout (row 0 = bottom); the backbuffer is top row first.
    const u32 src_y = backbuffer ? u32(rect.y) : size.height - (u32(rect.y) + rect.height);
    const usize bpp = bytes_per_pixel(fmt);
    const usize src_row = usize(rect.width) * bpp;
    @autoreleasepool {
        id<MTLBuffer> staging = [[device_ newBufferWithLength:src_row * rect.height
                                                      options:MTLResourceStorageModeShared] autorelease];
        [blit_encoder() copyFromTexture:tex
                            sourceSlice:0
                            sourceLevel:0
                           sourceOrigin:MTLOriginMake(u32(rect.x), src_y, 0)
                             sourceSize:MTLSizeMake(rect.width, rect.height, 1)
                               toBuffer:staging
                      destinationOffset:0
                 destinationBytesPerRow:src_row
               destinationBytesPerImage:src_row * rect.height];
        commit_open(true);
        const u8* src = static_cast<const u8*>([staging contents]);
        for (u32 y = 0; y < rect.height; ++y) {
            const u8* s = src + usize(backbuffer ? y : rect.height - 1 - y) * src_row;
            u8* d = out.data() + usize(y) * row;
            for (u32 x = 0; x < rect.width; ++x) {
                u8* px = d + usize(x) * 4;
                switch (fmt) {
                    case TextureFormat::RGBA8_UNorm:
                    case TextureFormat::RGBA8_SRGB: std::memcpy(px, s + x * 4, 4); break;
                    case TextureFormat::BGRA8_UNorm:
                        px[0] = s[x * 4 + 2];
                        px[1] = s[x * 4 + 1];
                        px[2] = s[x * 4 + 0];
                        px[3] = s[x * 4 + 3];
                        break;
                    case TextureFormat::RGBA16F: {
                        u16 h[4];
                        std::memcpy(h, s + x * 8, 8);
                        for (u32 c = 0; c < 4; ++c) px[c] = unorm8(half_to_float(h[c]));
                        break;
                    }
                    case TextureFormat::RGBA32F: {
                        f32 f[4];
                        std::memcpy(f, s + x * 16, 16);
                        for (u32 c = 0; c < 4; ++c) px[c] = unorm8(f[c]);
                        break;
                    }
                    case TextureFormat::R8_UNorm:
                        px[0] = s[x];
                        px[1] = px[2] = 0;
                        px[3] = 255;
                        break;
                    case TextureFormat::RG8_UNorm:
                        px[0] = s[x * 2];
                        px[1] = s[x * 2 + 1];
                        px[2] = 0;
                        px[3] = 255;
                        break;
                    case TextureFormat::R11G11B10F: {  // GL: each channel clamped to [0, 1], alpha 1
                        u32 v;
                        std::memcpy(&v, s + x * 4, 4);
                        px[0] = unorm8(small_float(v & 0x7FFu, 6));
                        px[1] = unorm8(small_float((v >> 11) & 0x7FFu, 6));
                        px[2] = unorm8(small_float(v >> 22, 5));
                        px[3] = 255;
                        break;
                    }
                    default: std::memset(px, 0, 4); break;
                }
            }
        }
    }
    return ok();
}

ResultV MetalDevice::read_buffer(BufferHandle h, u64 offset, Span<byte> out) {
    const MtlBuffer* b = buffers_.try_get(h);
    if (!b) return Error(Errc::NotFound, "rhi.read_buffer", "stale buffer handle");
    if (offset + out.size() > b->size) return Error(Errc::InvalidArgument, "rhi.read_buffer", "range outside buffer");
    // Buffers are shared storage: once every committed command buffer has completed (the queue runs
    // them in order, so waiting on an empty one is enough), the GPU's writes are in `contents`.
    @autoreleasepool {
        command_buffer();
        commit_open(true);
    }
    if (!out.empty()) std::memcpy(out.data(), static_cast<const byte*>([b->buffer contents]) + offset, out.size());
    return ok();
}

}  // namespace mce::rhi::metal
