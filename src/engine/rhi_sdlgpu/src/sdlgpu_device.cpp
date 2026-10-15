// mce_rhi_sdlgpu: device, resources, frame slots, uploads, readback (ADR-0038, sdlgpu_device.h).

#include "sdlgpu_device.h"

#include <cmath>
#include <cstring>

#include "mce/log.h"
#include "mce/math/math.h"
#if defined(__APPLE__)
#include <TargetConditionals.h>

#include "shader_msl.h"
#endif

namespace mce::rhi::sdlgpu {
namespace {

SDL_GPUTextureFormat sdl_format(TextureFormat f) {
    switch (f) {
        case TextureFormat::RGBA8_UNorm: return SDL_GPU_TEXTUREFORMAT_R8G8B8A8_UNORM;
        case TextureFormat::RGBA8_SRGB: return SDL_GPU_TEXTUREFORMAT_R8G8B8A8_UNORM_SRGB;
        case TextureFormat::BGRA8_UNorm: return SDL_GPU_TEXTUREFORMAT_B8G8R8A8_UNORM;
        case TextureFormat::RGBA16F: return SDL_GPU_TEXTUREFORMAT_R16G16B16A16_FLOAT;
        case TextureFormat::RGBA32F: return SDL_GPU_TEXTUREFORMAT_R32G32B32A32_FLOAT;
        case TextureFormat::R8_UNorm: return SDL_GPU_TEXTUREFORMAT_R8_UNORM;
        case TextureFormat::RG8_UNorm: return SDL_GPU_TEXTUREFORMAT_R8G8_UNORM;
        case TextureFormat::R11G11B10F: return SDL_GPU_TEXTUREFORMAT_R11G11B10_UFLOAT;
        case TextureFormat::BC1_UNorm: return SDL_GPU_TEXTUREFORMAT_BC1_RGBA_UNORM;
        case TextureFormat::BC3_UNorm: return SDL_GPU_TEXTUREFORMAT_BC3_RGBA_UNORM;
        case TextureFormat::BC5_UNorm: return SDL_GPU_TEXTUREFORMAT_BC5_RG_UNORM;
        case TextureFormat::BC7_UNorm: return SDL_GPU_TEXTUREFORMAT_BC7_RGBA_UNORM;
        case TextureFormat::BC7_SRGB: return SDL_GPU_TEXTUREFORMAT_BC7_RGBA_UNORM_SRGB;
        case TextureFormat::D32_Float: return SDL_GPU_TEXTUREFORMAT_D32_FLOAT;
        case TextureFormat::D24_UNorm_S8: return SDL_GPU_TEXTUREFORMAT_D24_UNORM_S8_UINT;
        case TextureFormat::D16_UNorm: return SDL_GPU_TEXTUREFORMAT_D16_UNORM;
        case TextureFormat::ASTC_4x4_UNorm: return SDL_GPU_TEXTUREFORMAT_ASTC_4x4_UNORM;
        case TextureFormat::ASTC_4x4_SRGB: return SDL_GPU_TEXTUREFORMAT_ASTC_4x4_UNORM_SRGB;
    }
    return SDL_GPU_TEXTUREFORMAT_R8G8B8A8_UNORM;
}

usize level_bytes(TextureFormat f, u32 w, u32 h) {
    if (is_compressed_format(f)) {
        const usize block = f == TextureFormat::BC1_UNorm ? 8 : 16;
        return usize((w + 3) / 4) * ((h + 3) / 4) * block;
    }
    return usize(w) * h * bytes_per_pixel(f);
}

SDL_GPUSampleCount sample_count(u32 n) {
    switch (n) {
        case 2: return SDL_GPU_SAMPLECOUNT_2;
        case 4: return SDL_GPU_SAMPLECOUNT_4;
        case 8: return SDL_GPU_SAMPLECOUNT_8;
        default: return SDL_GPU_SAMPLECOUNT_1;
    }
}

SDL_GPUSamplerAddressMode sdl_wrap(AddressMode m) {
    switch (m) {
        case AddressMode::Repeat: return SDL_GPU_SAMPLERADDRESSMODE_REPEAT;
        case AddressMode::ClampToEdge: return SDL_GPU_SAMPLERADDRESSMODE_CLAMP_TO_EDGE;
        case AddressMode::MirroredRepeat: return SDL_GPU_SAMPLERADDRESSMODE_MIRRORED_REPEAT;
    }
    return SDL_GPU_SAMPLERADDRESSMODE_CLAMP_TO_EDGE;
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
        bits = sign | ((exp + 127 - 15) << 23) | (mant << 13);
    }
    f32 f;
    std::memcpy(&f, &bits, 4);
    return f;
}

f32 small_float(u32 bits, u32 mbits) {
    const u32 m = bits & ((1u << mbits) - 1u);
    const u32 e = bits >> mbits;
    const f32 frac = f32(m) / f32(1u << mbits);
    if (e == 0) return std::ldexp(frac, -14);
    if (e == 31) return m ? 0.0f : 65504.0f;
    return std::ldexp(1.0f + frac, i32(e) - 15);
}

u8 unorm8(f32 v) {
    const f32 c = v < 0.0f ? 0.0f : (v > 1.0f ? 1.0f : v);
    return static_cast<u8>(std::lround(c * 255.0f));
}

constexpr SDL_GPUBufferUsageFlags kAllBufferUsage = SDL_GPU_BUFFERUSAGE_VERTEX | SDL_GPU_BUFFERUSAGE_INDEX |
                                                    SDL_GPU_BUFFERUSAGE_INDIRECT |
                                                    SDL_GPU_BUFFERUSAGE_GRAPHICS_STORAGE_READ |
                                                    SDL_GPU_BUFFERUSAGE_COMPUTE_STORAGE_READ |
                                                    SDL_GPU_BUFFERUSAGE_COMPUTE_STORAGE_WRITE;

SDL_GPUBuffer* make_buffer(SDL_GPUDevice* dev, usize size, const char* name) {
    SDL_GPUBufferCreateInfo ci{};
    ci.usage = kAllBufferUsage;
    ci.size = static_cast<Uint32>(size);
    SDL_GPUBuffer* b = SDL_CreateGPUBuffer(dev, &ci);
    if (b && name && *name) SDL_SetGPUBufferName(dev, b, name);
    return b;
}

}  // namespace

bool TargetKey::operator==(const TargetKey& o) const {
    if (color_count != o.color_count || samples != o.samples || has_depth != o.has_depth || depth != o.depth) return false;
    for (u32 i = 0; i < color_count; ++i)
        if (colors[i] != o.colors[i]) return false;
    return true;
}

bool SdlGpuFence::is_signaled() { return device->slot_signaled(slot); }
void SdlGpuFence::wait_blocking() { device->wait_slot(slot); }

// ---- creation --------------------------------------------------------------------------------

Result<mem::UniquePtr<RenderDevice>> create_device(platform::Window& window, const DeviceDesc& desc) {
    SdlGpuDevice* dev = mem::make<SdlGpuDevice>(mem::heap(), window, desc);
    mem::UniquePtr<RenderDevice> owned(mem::heap(), dev);
    MCE_TRY(dev->init());
    return owned;
}

SdlGpuDevice::SdlGpuDevice(platform::Window& window, const DeviceDesc& desc) : window_(window), desc_(desc) {
    for (u32 i = 0; i < MCE_FRAMES_IN_FLIGHT; ++i) {
        fence_objs_[i].device = this;
        fence_objs_[i].slot = i;
    }
}

ResultV SdlGpuDevice::init() {
#if defined(__APPLE__)
    const SDL_GPUShaderFormat formats = SDL_GPU_SHADERFORMAT_MSL;
#else
    const SDL_GPUShaderFormat formats = SDL_GPU_SHADERFORMAT_SPIRV;
#endif
    bool debug = desc_.debug_layer;
#if defined(__APPLE__) && TARGET_OS_SIMULATOR
    // The Simulator's GPU has no indirect draw or dispatch, and SDL's debug mode turns Metal
    // validation on, which aborts on the first indirect draw (SBA-30): no debug mode there.
    debug = false;
#endif
    device_ = SDL_CreateGPUDevice(formats, debug, nullptr);
    if (!device_) {
        MCE_LOG_ERROR("rhi.sdlgpu: SDL_CreateGPUDevice failed: {}", SDL_GetError());
        return Error(Errc::Unsupported, "rhi.sdlgpu.device", SDL_GetError());
    }
    auto* sdl_window = static_cast<SDL_Window*>(window_.native_handle());
    if (!SDL_ClaimWindowForGPUDevice(device_, sdl_window)) {
        MCE_LOG_ERROR("rhi.sdlgpu: SDL_ClaimWindowForGPUDevice failed: {}", SDL_GetError());
        return Error(Errc::Unsupported, "rhi.sdlgpu.window", SDL_GetError());
    }
    claimed_ = true;
    vsync_ = desc_.vsync;
    SDL_SetGPUSwapchainParameters(device_, sdl_window, SDL_GPU_SWAPCHAINCOMPOSITION_SDR,
                                  vsync_ ? SDL_GPU_PRESENTMODE_VSYNC : SDL_GPU_PRESENTMODE_IMMEDIATE);

    caps_.api_name.append("SDL GPU (");
    caps_.api_name.append(SDL_GetGPUDeviceDriver(device_));
    caps_.api_name.append(")");
    const SDL_PropertiesID props = SDL_GetGPUDeviceProperties(device_);
    const char* dev_name = props ? SDL_GetStringProperty(props, SDL_PROP_GPU_DEVICE_NAME_STRING, "") : "";
    caps_.device_name.append(dev_name ? dev_name : "");
    caps_.api_major = 1;
    caps_.api_minor = 1;
    caps_.max_texture_size = 8192;          // SDL GPU's guaranteed minimum on every backend
    caps_.max_uniform_block_size = 32768;   // SDL_PushGPU*UniformData limit
    caps_.shader_draw_parameters = true;
    caps_.timestamp_queries = false;        // SDL GPU has no timestamp queries (ADR-0038 §7)
    caps_.storage_buffer_alignment = 256;
    // Compute (M-VISUAL 1). SDL GPU reports no limits: these are Vulkan's guaranteed minimums (Metal's
    // are higher). On Apple the compute stage goes through the MSL path like the graphics ones.
    caps_.compute = true;
    caps_.max_compute_invocations = 128;
    caps_.max_compute_group_size[0] = caps_.max_compute_group_size[1] = 128;
    caps_.max_compute_group_size[2] = 64;
    for (u32& n : caps_.max_compute_group_count) n = 65535;
    caps_.max_storage_image_units = kMaxImageUnits;
    MCE_LOG_INFO("rhi.sdlgpu: {} on {}", caps_.api_name.c_str(), caps_.device_name.c_str());

    default_depth_format_ =
        SDL_GPUTextureSupportsFormat(device_, SDL_GPU_TEXTUREFORMAT_D24_UNORM_S8_UINT, SDL_GPU_TEXTURETYPE_2D,
                                     SDL_GPU_TEXTUREUSAGE_DEPTH_STENCIL_TARGET)
            ? SDL_GPU_TEXTUREFORMAT_D24_UNORM_S8_UINT
            : SDL_GPU_TEXTUREFORMAT_D32_FLOAT_S8_UINT;

    // Rings and staging per frame slot.
    const usize geo = align_up(desc_.geometry_ring_bytes, 256);
    const usize ind = align_up(desc_.indirect_ring_bytes, 256);
    transfer_bytes_ = geo + ind + geo;  // geometry mirror | indirect mirror | storage staging
    for (SlotRings& r : rings_) {
        r.geometry = make_buffer(device_, geo, "mce.ring.geometry");
        r.indirect = make_buffer(device_, ind, "mce.ring.indirect");
        SDL_GPUTransferBufferCreateInfo ti{};
        ti.usage = SDL_GPU_TRANSFERBUFFERUSAGE_UPLOAD;
        ti.size = static_cast<Uint32>(transfer_bytes_);
        r.transfer = SDL_CreateGPUTransferBuffer(device_, &ti);
        if (!r.geometry || !r.indirect || !r.transfer) return Error(Errc::OutOfMemory, "rhi.sdlgpu.ring", SDL_GetError());
    }

    // Dummies for unbound slots.
    const SDL_GPUTextureType dummy_types[4] = {SDL_GPU_TEXTURETYPE_2D, SDL_GPU_TEXTURETYPE_2D_ARRAY,
                                               SDL_GPU_TEXTURETYPE_2D, SDL_GPU_TEXTURETYPE_2D_ARRAY};
    for (u32 i = 0; i < 4; ++i) {
        SDL_GPUTextureCreateInfo ci{};
        ci.type = dummy_types[i];
        ci.format = i < 2 ? SDL_GPU_TEXTUREFORMAT_R8G8B8A8_UNORM : SDL_GPU_TEXTUREFORMAT_D32_FLOAT;
        ci.usage = SDL_GPU_TEXTUREUSAGE_SAMPLER | (i < 2 ? 0u : SDL_GPU_TEXTUREUSAGE_DEPTH_STENCIL_TARGET);
        ci.width = ci.height = 1;
        ci.layer_count_or_depth = 1;
        ci.num_levels = 1;
        dummy_tex_[i] = SDL_CreateGPUTexture(device_, &ci);
        if (!dummy_tex_[i]) return Error(Errc::OutOfMemory, "rhi.sdlgpu.dummy", SDL_GetError());
    }
    {
        static constexpr u8 kWhite[4] = {255, 255, 255, 255};
        GpuTexture t;
        t.texture = dummy_tex_[0];
        t.format = TextureFormat::RGBA8_UNorm;
        upload_texture(t, 0, 0, 0, 0, 1, 1, Span<const byte>(reinterpret_cast<const byte*>(kWhite), 4));
        t.texture = dummy_tex_[1];
        upload_texture(t, 0, 0, 0, 0, 1, 1, Span<const byte>(reinterpret_cast<const byte*>(kWhite), 4));
    }
    SDL_GPUSamplerCreateInfo si{};
    dummy_sampler_ = SDL_CreateGPUSampler(device_, &si);
    si.enable_compare = true;
    si.compare_op = SDL_GPU_COMPAREOP_LESS_OR_EQUAL;
    dummy_compare_sampler_ = SDL_CreateGPUSampler(device_, &si);
    dummy_storage_ = make_buffer(device_, 256, "mce.dummy.storage");
    if (!dummy_sampler_ || !dummy_compare_sampler_ || !dummy_storage_)
        return Error(Errc::OutOfMemory, "rhi.sdlgpu.dummy", SDL_GetError());

    ensure_backbuffer();
    flush_pending();
    return ok();
}

SdlGpuDevice::~SdlGpuDevice() { release_all(); }

void SdlGpuDevice::release_all() {
    if (!device_) return;
    flush_pending();
    SDL_WaitForGPUIdle(device_);
    for (SlotRings& r : rings_) {
        if (r.fence) SDL_ReleaseGPUFence(device_, r.fence);
        SDL_ReleaseGPUBuffer(device_, r.geometry);
        SDL_ReleaseGPUBuffer(device_, r.indirect);
        SDL_ReleaseGPUTransferBuffer(device_, r.transfer);
        for (const SlotRings::StorageChunk& c : r.storage) SDL_ReleaseGPUBuffer(device_, c.buffer);
        r = SlotRings{};
    }
    for (const GpuBuffer& b : buffers_.dense()) SDL_ReleaseGPUBuffer(device_, b.buffer);
    for (const GpuTexture& t : textures_.dense()) SDL_ReleaseGPUTexture(device_, t.texture);
    for (const GpuSampler& s : samplers_.dense()) SDL_ReleaseGPUSampler(device_, s.sampler);
    for (const GpuPipeline& p : pipelines_.dense()) {
        for (const PipelineVariant& v : p.variants) SDL_ReleaseGPUGraphicsPipeline(device_, v.pipeline);
        SDL_ReleaseGPUShader(device_, p.vs);
        SDL_ReleaseGPUShader(device_, p.fs);
        if (p.cs) SDL_ReleaseGPUComputePipeline(device_, p.cs);
    }
    for (SDL_GPUTexture* t : dummy_tex_) SDL_ReleaseGPUTexture(device_, t);
    SDL_ReleaseGPUSampler(device_, dummy_sampler_);
    SDL_ReleaseGPUSampler(device_, dummy_compare_sampler_);
    SDL_ReleaseGPUBuffer(device_, dummy_storage_);
    SDL_ReleaseGPUTexture(device_, backbuffer_);
    SDL_ReleaseGPUTexture(device_, backbuffer_depth_);
    if (claimed_) SDL_ReleaseWindowFromGPUDevice(device_, static_cast<SDL_Window*>(window_.native_handle()));
    SDL_DestroyGPUDevice(device_);
    device_ = nullptr;
}

// The offscreen backbuffer and its depth buffer follow the window's pixel size.
void SdlGpuDevice::ensure_backbuffer() {
    const Extent2D want = backbuffer_size();
    if (backbuffer_ && want == backbuffer_extent_) return;
    if (want.width == 0 || want.height == 0) return;  // minimized: keep the old one
    SDL_ReleaseGPUTexture(device_, backbuffer_);
    SDL_ReleaseGPUTexture(device_, backbuffer_depth_);
    SDL_GPUTextureCreateInfo ci{};
    ci.type = SDL_GPU_TEXTURETYPE_2D;
    ci.format = SDL_GPU_TEXTUREFORMAT_R8G8B8A8_UNORM;
    ci.usage = SDL_GPU_TEXTUREUSAGE_COLOR_TARGET | SDL_GPU_TEXTUREUSAGE_SAMPLER;
    ci.width = want.width;
    ci.height = want.height;
    ci.layer_count_or_depth = 1;
    ci.num_levels = 1;
    backbuffer_ = SDL_CreateGPUTexture(device_, &ci);
    ci.format = default_depth_format_;
    ci.usage = SDL_GPU_TEXTUREUSAGE_DEPTH_STENCIL_TARGET;
    backbuffer_depth_ = SDL_CreateGPUTexture(device_, &ci);
    if (backbuffer_) SDL_SetGPUTextureName(device_, backbuffer_, "mce.backbuffer");
    backbuffer_extent_ = want;
    if (!backbuffer_ || !backbuffer_depth_) {
        MCE_LOG_ERROR("rhi.sdlgpu: backbuffer {}x{} failed: {}", want.width, want.height, SDL_GetError());
        ++validation_errors_;
    }
}

// ---- uploads -----------------------------------------------------------------------------------

SDL_GPUCopyPass* SdlGpuDevice::pending_copy_pass() {
    if (!pending_cb_) pending_cb_ = SDL_AcquireGPUCommandBuffer(device_);
    if (!pending_copy_) pending_copy_ = SDL_BeginGPUCopyPass(pending_cb_);
    return pending_copy_;
}

void SdlGpuDevice::flush_pending() {
    if (!pending_cb_) return;
    if (pending_copy_) SDL_EndGPUCopyPass(pending_copy_);
    SDL_SubmitGPUCommandBuffer(pending_cb_);
    pending_copy_ = nullptr;
    pending_cb_ = nullptr;
}

// Each immediate upload stages through its own transfer buffer; SDL releases it once the pending
// command buffer has run.
void SdlGpuDevice::upload_buffer(SDL_GPUBuffer* dst, u64 offset, Span<const byte> data) {
    if (data.empty() || !dst) return;
    SDL_GPUTransferBufferCreateInfo ti{};
    ti.usage = SDL_GPU_TRANSFERBUFFERUSAGE_UPLOAD;
    ti.size = static_cast<Uint32>(data.size());
    SDL_GPUTransferBuffer* tb = SDL_CreateGPUTransferBuffer(device_, &ti);
    if (!tb) return;
    void* map = SDL_MapGPUTransferBuffer(device_, tb, false);
    std::memcpy(map, data.data(), data.size());
    SDL_UnmapGPUTransferBuffer(device_, tb);
    const SDL_GPUTransferBufferLocation src{tb, 0};
    const SDL_GPUBufferRegion region{dst, static_cast<Uint32>(offset), static_cast<Uint32>(data.size())};
    SDL_UploadToGPUBuffer(pending_copy_pass(), &src, &region, false);
    SDL_ReleaseGPUTransferBuffer(device_, tb);
}

void SdlGpuDevice::upload_texture(const GpuTexture& t, u32 mip, u32 layer, u32 x, u32 y, u32 w, u32 h,
                                  Span<const byte> data) {
    if (data.empty() || !t.texture) return;
    SDL_GPUTransferBufferCreateInfo ti{};
    ti.usage = SDL_GPU_TRANSFERBUFFERUSAGE_UPLOAD;
    ti.size = static_cast<Uint32>(data.size());
    SDL_GPUTransferBuffer* tb = SDL_CreateGPUTransferBuffer(device_, &ti);
    if (!tb) return;
    void* map = SDL_MapGPUTransferBuffer(device_, tb, false);
    std::memcpy(map, data.data(), data.size());
    SDL_UnmapGPUTransferBuffer(device_, tb);
    SDL_GPUTextureTransferInfo src{};
    src.transfer_buffer = tb;
    SDL_GPUTextureRegion dst{};
    dst.texture = t.texture;
    dst.mip_level = mip;
    dst.layer = layer;
    dst.x = x;
    dst.y = y;
    dst.w = w;
    dst.h = h;
    dst.d = 1;
    SDL_UploadToGPUTexture(pending_copy_pass(), &src, &dst, false);
    SDL_ReleaseGPUTransferBuffer(device_, tb);
}

// ---- resources -------------------------------------------------------------------------------

BufferHandle SdlGpuDevice::create_buffer(const BufferDesc& desc, Span<const byte> initial_data) {
    MCE_ASSERT_MSG(desc.size_bytes > 0, "create_buffer: size 0");
    MCE_ASSERT_MSG(initial_data.empty() || initial_data.size() <= desc.size_bytes, "initial data larger than buffer");
    GpuBuffer b;
    b.size = desc.size_bytes;
    b.buffer = make_buffer(device_, align_up(desc.size_bytes, 4), desc.debug_name);
    if (!b.buffer) {
        MCE_LOG_ERROR("rhi.sdlgpu: buffer '{}' ({} bytes) failed: {}", desc.debug_name, desc.size_bytes, SDL_GetError());
        return {};
    }
    upload_buffer(b.buffer, 0, initial_data);
    return buffers_.insert(b);
}

void SdlGpuDevice::update_buffer(BufferHandle h, u64 offset, Span<const byte> data) {
    const GpuBuffer* b = buffers_.try_get(h);
    MCE_ASSERT_MSG(b, "update_buffer: stale handle");
    if (!b) return;
    MCE_ASSERT_MSG(offset + data.size() <= b->size, "update_buffer: out of range");
    if (offset + data.size() > b->size) return;
    upload_buffer(b->buffer, offset, data);
}

TextureHandle SdlGpuDevice::create_texture(const TextureDesc& desc, Span<const byte> initial_data) {
    MCE_ASSERT_MSG(desc.size.width > 0 && desc.size.height > 0, "create_texture: empty extent");
    const bool array = desc.dimension == TextureDimension::Tex2DArray;
    MCE_ASSERT_MSG(array || desc.array_layers <= 1, "create_texture: layers > 1 need TextureDimension::Tex2DArray");
    GpuTexture t;
    t.size = desc.size;
    t.format = desc.format;
    t.dimension = desc.dimension;
    t.usage = desc.usage;
    t.mips = desc.mip_levels ? desc.mip_levels : 1;
    t.layers = array ? (desc.array_layers ? desc.array_layers : 1) : 1;
    t.samples = desc.samples > 1 ? desc.samples : 1;
    MCE_ASSERT_MSG(t.samples == 1 || (!array && t.mips == 1 && initial_data.empty()),
                   "create_texture: multisampled textures are single-mip 2D render targets");

    t.sdl_format = sdl_format(desc.format);
    SDL_GPUTextureUsageFlags usage = 0;
    switch (desc.usage) {
        case TextureUsage::Sampled: usage = SDL_GPU_TEXTUREUSAGE_SAMPLER; break;
        case TextureUsage::ColorTarget:
        case TextureUsage::SampledColorTarget: usage = SDL_GPU_TEXTUREUSAGE_COLOR_TARGET; break;
        case TextureUsage::DepthTarget: usage = SDL_GPU_TEXTUREUSAGE_DEPTH_STENCIL_TARGET; break;
        case TextureUsage::SampledStorage:
            usage = SDL_GPU_TEXTUREUSAGE_COMPUTE_STORAGE_READ | SDL_GPU_TEXTUREUSAGE_COMPUTE_STORAGE_WRITE;
            break;
    }
    // StorageAccess::ReadWrite reads and writes one image in a dispatch, which SDL allows only for
    // textures made with SIMULTANEOUS_READ_WRITE (format-dependent).
    if (desc.usage == TextureUsage::SampledStorage &&
        SDL_GPUTextureSupportsFormat(device_, t.sdl_format, array ? SDL_GPU_TEXTURETYPE_2D_ARRAY : SDL_GPU_TEXTURETYPE_2D,
                                     usage | SDL_GPU_TEXTUREUSAGE_SAMPLER |
                                         SDL_GPU_TEXTUREUSAGE_COMPUTE_STORAGE_SIMULTANEOUS_READ_WRITE))
        usage |= SDL_GPU_TEXTUREUSAGE_COMPUTE_STORAGE_SIMULTANEOUS_READ_WRITE;
    // Render targets are sampled by later passes (shadow maps, post chains) unless multisampled.
    if (t.samples == 1) usage |= SDL_GPU_TEXTUREUSAGE_SAMPLER;
    const SDL_GPUTextureType type = array ? SDL_GPU_TEXTURETYPE_2D_ARRAY : SDL_GPU_TEXTURETYPE_2D;
    if (!SDL_GPUTextureSupportsFormat(device_, t.sdl_format, type, usage)) {
        if (t.sdl_format == SDL_GPU_TEXTUREFORMAT_D24_UNORM_S8_UINT) {
            t.sdl_format = SDL_GPU_TEXTUREFORMAT_D32_FLOAT_S8_UINT;  // Apple GPUs have no D24S8
        } else {
            MCE_LOG_ERROR("rhi.sdlgpu: texture '{}': format {} not supported here", desc.debug_name,
                          static_cast<u32>(desc.format));
            ++validation_errors_;
            return {};
        }
    }
    // Not every GPU takes every sample count (the iOS Simulator has no 2x): use the nearest one it
    // does, higher first, so a quality tier never fails on a missing count. Pipelines follow the
    // target's count.
    if (t.samples > 1 && !SDL_GPUTextureSupportsSampleCount(device_, t.sdl_format, sample_count(t.samples))) {
        const u8 wanted = t.samples;
        t.samples = 1;
        for (const u8 n : {u8(4), u8(8), u8(2)})
            if (SDL_GPUTextureSupportsSampleCount(device_, t.sdl_format, sample_count(n))) {
                t.samples = n;
                break;
            }
        MCE_LOG_WARN("rhi.sdlgpu: texture '{}': {}x MSAA not supported, using {}x", desc.debug_name, wanted, t.samples);
    }
    SDL_GPUTextureCreateInfo ci{};
    ci.type = type;
    ci.format = t.sdl_format;
    ci.usage = usage;
    ci.width = desc.size.width;
    ci.height = desc.size.height;
    ci.layer_count_or_depth = t.layers;
    ci.num_levels = t.mips;
    ci.sample_count = sample_count(t.samples);
    t.texture = SDL_CreateGPUTexture(device_, &ci);
    if (!t.texture) {
        MCE_LOG_ERROR("rhi.sdlgpu: texture '{}' failed: {}", desc.debug_name, SDL_GetError());
        ++validation_errors_;
        return {};
    }
    if (desc.debug_name && *desc.debug_name) SDL_SetGPUTextureName(device_, t.texture, desc.debug_name);
    for (u32 m = 0; m < t.mips; ++m)
        t.bytes += level_bytes(desc.format, math::max(1u, desc.size.width >> m), math::max(1u, desc.size.height >> m)) *
                   t.layers;
    t.bytes *= t.samples;

    if (!initial_data.empty()) {
        const usize layer_bytes = level_bytes(desc.format, desc.size.width, desc.size.height);
        MCE_ASSERT_MSG(initial_data.size() >= layer_bytes * t.layers, "create_texture: initial data smaller than mip 0");
        MCE_ASSERT_MSG(!is_depth_format(desc.format), "create_texture: depth formats take no initial data");
        if (initial_data.size() >= layer_bytes * t.layers && !is_depth_format(desc.format))
            for (u32 l = 0; l < t.layers; ++l)
                upload_texture(t, 0, l, 0, 0, desc.size.width, desc.size.height,
                               initial_data.subspan(usize(l) * layer_bytes, layer_bytes));
    }
    texture_bytes_ += t.bytes;
    return textures_.insert(t);
}

void SdlGpuDevice::update_texture(TextureHandle h, const TextureRegion& region, Span<const byte> data) {
    const GpuTexture* t = textures_.try_get(h);
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
    upload_texture(*t, region.mip, region.layer, region.x, region.y, rw, rh, data.subspan(0, need));
}

SamplerHandle SdlGpuDevice::create_sampler(const SamplerDesc& desc) {
    SDL_GPUSamplerCreateInfo ci{};
    ci.min_filter = desc.min_filter == Filter::Nearest ? SDL_GPU_FILTER_NEAREST : SDL_GPU_FILTER_LINEAR;
    ci.mag_filter = desc.mag_filter == Filter::Nearest ? SDL_GPU_FILTER_NEAREST : SDL_GPU_FILTER_LINEAR;
    ci.mipmap_mode = desc.mip == MipFilter::Linear ? SDL_GPU_SAMPLERMIPMAPMODE_LINEAR : SDL_GPU_SAMPLERMIPMAPMODE_NEAREST;
    ci.address_mode_u = sdl_wrap(desc.wrap_s);
    ci.address_mode_v = sdl_wrap(desc.wrap_t);
    ci.address_mode_w = SDL_GPU_SAMPLERADDRESSMODE_CLAMP_TO_EDGE;
    ci.min_lod = 0.0f;
    ci.max_lod = desc.mip == MipFilter::None ? 0.0f : 1000.0f;  // GL: no mip filter samples level 0 only
    ci.enable_anisotropy = desc.max_aniso > 1.0f;
    ci.max_anisotropy = desc.max_aniso > 1.0f ? desc.max_aniso : 1.0f;
    ci.enable_compare = desc.compare;
    ci.compare_op = sdl_compare(desc.compare_op);
    GpuSampler s;
    s.sampler = SDL_CreateGPUSampler(device_, &ci);
    s.compare = desc.compare;
    if (!s.sampler) {
        MCE_LOG_ERROR("rhi.sdlgpu: sampler failed: {}", SDL_GetError());
        return {};
    }
    return samplers_.insert(s);
}

Result<PipelineHandle> SdlGpuDevice::create_pipeline(const GraphicsPipelineDesc& desc) {
    GpuPipeline p;
    FixedString<1024> log;
    Array<u32> vspv, fspv;
    if (auto r = compile_glsl(ShaderStage::Vertex, desc.vertex_shader.glsl, desc.vertex_shader.debug_name, vspv, p.vres, log);
        !r) {
        MCE_LOG_ERROR("rhi.sdlgpu: vertex shader '{}' failed:\n{}", desc.vertex_shader.debug_name, log.c_str());
        return Error(Errc::ShaderCompile, "rhi.pipeline.vertex", desc.vertex_shader.debug_name);
    }
    if (auto r = compile_glsl(ShaderStage::Fragment, desc.fragment_shader.glsl, desc.fragment_shader.debug_name, fspv,
                              p.fres, log);
        !r) {
        MCE_LOG_ERROR("rhi.sdlgpu: fragment shader '{}' failed:\n{}", desc.fragment_shader.debug_name, log.c_str());
        return Error(Errc::ShaderCompile, "rhi.pipeline.fragment", desc.fragment_shader.debug_name);
    }
    auto make_shader = [&](const Array<u32>& spv, const StageResources& res, SDL_GPUShaderStage stage) {
        SDL_GPUShaderCreateInfo ci{};
#if defined(__APPLE__)
        // SDL GPU's Metal driver takes MSL (shader_msl.h).
        Array<char> msl;
        const ShaderStage st = stage == SDL_GPU_SHADERSTAGE_VERTEX ? ShaderStage::Vertex : ShaderStage::Fragment;
#if TARGET_OS_IPHONE
        constexpr MslPlatform kPlatform = MslPlatform::iOS;
#else
        constexpr MslPlatform kPlatform = MslPlatform::macOS;
#endif
        const char* stage_name = st == ShaderStage::Vertex ? desc.vertex_shader.debug_name : desc.fragment_shader.debug_name;
        if (auto r = spirv_to_msl(st, spv, res, kPlatform, stage_name, msl, log); !r) {
            MCE_LOG_ERROR("rhi.sdlgpu: MSL translation of '{}' failed:\n{}", stage_name, log.c_str());
            return static_cast<SDL_GPUShader*>(nullptr);
        }
        ci.code_size = msl.size() - 1;
        ci.code = reinterpret_cast<const Uint8*>(msl.data());
        ci.entrypoint = kMslEntryPoint;
        ci.format = SDL_GPU_SHADERFORMAT_MSL;
#else
        ci.code_size = spv.size() * sizeof(u32);
        ci.code = reinterpret_cast<const Uint8*>(spv.data());
        ci.entrypoint = "main";
        ci.format = SDL_GPU_SHADERFORMAT_SPIRV;
#endif
        ci.stage = stage;
        ci.num_samplers = res.sampler_count;
        ci.num_storage_buffers = res.storage_count;
        ci.num_uniform_buffers = res.uniform_count;
        return SDL_CreateGPUShader(device_, &ci);
    };
    p.vs = make_shader(vspv, p.vres, SDL_GPU_SHADERSTAGE_VERTEX);
    p.fs = make_shader(fspv, p.fres, SDL_GPU_SHADERSTAGE_FRAGMENT);
    if (!p.vs || !p.fs) {
        MCE_LOG_ERROR("rhi.sdlgpu: pipeline '{}': SDL_CreateGPUShader failed: {}", desc.debug_name, SDL_GetError());
        SDL_ReleaseGPUShader(device_, p.vs);
        SDL_ReleaseGPUShader(device_, p.fs);
        return Error(Errc::PipelineCreate, "rhi.pipeline.link", desc.debug_name);
    }
    MCE_ASSERT_MSG(desc.vertex.attrs.size() <= 16, "create_pipeline: more than 16 vertex attributes");
    p.attr_count = static_cast<u32>(math::min<usize>(desc.vertex.attrs.size(), 16));
    for (u32 i = 0; i < p.attr_count; ++i) p.attrs[i] = desc.vertex.attrs[i];
    p.stride = desc.vertex.stride;
    p.state = desc.state;
    p.name.append(desc.debug_name ? desc.debug_name : "");
    return pipelines_.insert(std::move(p));
}

Result<PipelineHandle> SdlGpuDevice::create_compute_pipeline(const ComputePipelineDesc& desc) {
    if (!caps_.compute) return Error(Errc::Unsupported, "rhi.compute", desc.debug_name);
    GpuPipeline p;
    FixedString<1024> log;
    Array<u32> spv;
    if (auto r = compile_glsl(ShaderStage::Compute, desc.compute_shader.glsl, desc.compute_shader.debug_name, spv, p.cres,
                              log);
        !r) {
        MCE_LOG_ERROR("rhi.sdlgpu: compute shader '{}' failed:\n{}", desc.compute_shader.debug_name, log.c_str());
        return Error(Errc::ShaderCompile, "rhi.pipeline.compute", desc.compute_shader.debug_name);
    }
    const StageResources& res = p.cres;
    SDL_GPUComputePipelineCreateInfo ci{};
#if defined(__APPLE__)
    Array<char> msl;
#if TARGET_OS_IPHONE
    constexpr MslPlatform kPlatform = MslPlatform::iOS;
#else
    constexpr MslPlatform kPlatform = MslPlatform::macOS;
#endif
    if (auto r = spirv_to_msl(ShaderStage::Compute, spv, res, kPlatform, desc.compute_shader.debug_name, msl, log); !r) {
        MCE_LOG_ERROR("rhi.sdlgpu: MSL translation of '{}' failed:\n{}", desc.compute_shader.debug_name, log.c_str());
        return Error(Errc::ShaderCompile, "rhi.pipeline.compute", desc.compute_shader.debug_name);
    }
    ci.code_size = msl.size() - 1;
    ci.code = reinterpret_cast<const Uint8*>(msl.data());
    ci.entrypoint = kMslEntryPoint;
    ci.format = SDL_GPU_SHADERFORMAT_MSL;
#else
    ci.code_size = spv.size() * sizeof(u32);
    ci.code = reinterpret_cast<const Uint8*>(spv.data());
    ci.entrypoint = "main";
    ci.format = SDL_GPU_SHADERFORMAT_SPIRV;
#endif
    ci.num_samplers = res.sampler_count;
    ci.num_readonly_storage_textures = 0;  // storage images all bind read-write (shader_spirv.h)
    ci.num_readonly_storage_buffers = res.storage_count;
    ci.num_readwrite_storage_textures = res.rw_image_count;
    ci.num_readwrite_storage_buffers = res.rw_storage_count;
    ci.num_uniform_buffers = res.uniform_count;
    ci.threadcount_x = res.local_size[0];
    ci.threadcount_y = res.local_size[1];
    ci.threadcount_z = res.local_size[2];
    p.cs = SDL_CreateGPUComputePipeline(device_, &ci);
    if (!p.cs) {
        MCE_LOG_ERROR("rhi.sdlgpu: compute pipeline '{}' failed: {}", desc.debug_name, SDL_GetError());
        return Error(Errc::PipelineCreate, "rhi.pipeline.compute", desc.debug_name);
    }
    p.name.append(desc.debug_name ? desc.debug_name : "");
    return pipelines_.insert(std::move(p));
}

void SdlGpuDevice::destroy_buffer(BufferHandle h) {
    if (const GpuBuffer* b = buffers_.try_get(h)) {
        for (SDL_GPUBuffer*& s : storage_bound_)
            if (s == b->buffer) s = nullptr;
        SDL_ReleaseGPUBuffer(device_, b->buffer);  // SDL defers until the GPU is done with it
        buffers_.erase(h);
    }
}

void SdlGpuDevice::destroy_texture(TextureHandle h) {
    const GpuTexture* t = textures_.try_get(h);
    if (!t) return;
    texture_bytes_ -= t->bytes;
    SDL_ReleaseGPUTexture(device_, t->texture);
    textures_.erase(h);
}

void SdlGpuDevice::destroy_sampler(SamplerHandle h) {
    if (const GpuSampler* s = samplers_.try_get(h)) {
        SDL_ReleaseGPUSampler(device_, s->sampler);
        samplers_.erase(h);
    }
}

void SdlGpuDevice::destroy_pipeline(PipelineHandle h) {
    if (GpuPipeline* p = pipelines_.try_get(h)) {
        if (ts_.pipeline == p) ts_.pipeline = nullptr;
        for (const PipelineVariant& v : p->variants) SDL_ReleaseGPUGraphicsPipeline(device_, v.pipeline);
        SDL_ReleaseGPUShader(device_, p->vs);
        SDL_ReleaseGPUShader(device_, p->fs);
        if (p->cs) SDL_ReleaseGPUComputePipeline(device_, p->cs);
        pipelines_.erase(h);
    }
}

Extent2D SdlGpuDevice::texture_size(TextureHandle h) const {
    const GpuTexture* t = textures_.try_get(h);
    return t ? t->size : Extent2D{};
}

TextureDesc SdlGpuDevice::texture_desc(TextureHandle h) const {
    TextureDesc d;
    const GpuTexture* t = textures_.try_get(h);
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

bool SdlGpuDevice::slot_signaled(u32 slot) {
    SDL_GPUFence* f = rings_[slot].fence;
    return !f || SDL_QueryGPUFence(device_, f);
}

void SdlGpuDevice::wait_slot(u32 slot) {
    SDL_GPUFence* f = rings_[slot].fence;
    if (f) SDL_WaitForGPUFences(device_, true, &f, 1);
}

mem::FrameSlotFence* SdlGpuDevice::fence_for_frame(u64 frame_index) {
    return &fence_objs_[frame_index % MCE_FRAMES_IN_FLIGHT];
}

void SdlGpuDevice::begin_frame(u64 frame_index) {
    MCE_ASSERT_MSG(!in_frame_, "begin_frame twice");
    frame_index_ = frame_index;
    slot_ = static_cast<u32>(frame_index % MCE_FRAMES_IN_FLIGHT);
    SlotRings& r = rings_[slot_];
    wait_slot(slot_);
    if (r.fence) {
        SDL_ReleaseGPUFence(device_, r.fence);
        r.fence = nullptr;
    }
    r.geometry_head = r.indirect_head = r.storage_head = 0;
    r.storage_next = 0;
    ensure_backbuffer();
    stats_ = FrameStats{};
    in_frame_ = true;
}

SDL_GPUBuffer* SdlGpuDevice::storage_chunk(SlotRings& ring, u32 size) {
    const u32 want = math::max<u32>(4096u, size);
    if (ring.storage_next < ring.storage.size()) {
        SlotRings::StorageChunk& c = ring.storage[ring.storage_next];
        if (c.size < want) {  // unused since this slot's fence passed: safe to replace
            SDL_ReleaseGPUBuffer(device_, c.buffer);
            u32 grown = 4096;
            while (grown < want) grown *= 2;
            c.buffer = make_buffer(device_, grown, "mce.storage");
            c.size = c.buffer ? grown : 0;
        }
        ++ring.storage_next;
        return c.buffer;
    }
    u32 grown = 4096;
    while (grown < want) grown *= 2;
    SlotRings::StorageChunk c;
    c.buffer = make_buffer(device_, grown, "mce.storage");
    c.size = c.buffer ? grown : 0;
    ring.storage.push_back(c);
    ++ring.storage_next;
    return c.buffer;
}

void SdlGpuDevice::submit(const CommandBuffer& cb) {
    MCE_ASSERT_MSG(in_frame_, "submit outside begin_frame/end_frame");
    MCE_ASSERT_MSG(!cb.in_pass(), "submit with an open pass");
    flush_pending();
    SDL_GPUCommandBuffer* gcb = SDL_AcquireGPUCommandBuffer(device_);
    if (!gcb) {
        MCE_LOG_ERROR("rhi.sdlgpu: SDL_AcquireGPUCommandBuffer failed: {}", SDL_GetError());
        return;
    }
    prepare_payloads(cb, gcb);
    record_passes(cb, gcb);
    SDL_SubmitGPUCommandBuffer(gcb);
}

void SdlGpuDevice::end_frame() {
    MCE_ASSERT_MSG(in_frame_, "end_frame without begin_frame");
    flush_pending();
    SlotRings& r = rings_[slot_];
    SDL_GPUCommandBuffer* gcb = SDL_AcquireGPUCommandBuffer(device_);
    if (gcb) r.fence = SDL_SubmitGPUCommandBufferAndAcquireFence(gcb);
    stats_.geometry_bytes = r.geometry_head;
    stats_.indirect_bytes = r.indirect_head;
    last_stats_ = stats_;
    in_frame_ = false;
}

void SdlGpuDevice::present(bool vsync) {
    auto* sdl_window = static_cast<SDL_Window*>(window_.native_handle());
    if (vsync != vsync_) {
        const SDL_GPUPresentMode mode =
            !vsync && SDL_WindowSupportsGPUPresentMode(device_, sdl_window, SDL_GPU_PRESENTMODE_IMMEDIATE)
                ? SDL_GPU_PRESENTMODE_IMMEDIATE
                : SDL_GPU_PRESENTMODE_VSYNC;
        SDL_SetGPUSwapchainParameters(device_, sdl_window, SDL_GPU_SWAPCHAINCOMPOSITION_SDR, mode);
        vsync_ = vsync;
    }
    flush_pending();
    SDL_GPUCommandBuffer* gcb = SDL_AcquireGPUCommandBuffer(device_);
    if (!gcb) return;
    SDL_GPUTexture* swap = nullptr;
    Uint32 w = 0, h = 0;
    if (SDL_WaitAndAcquireGPUSwapchainTexture(gcb, sdl_window, &swap, &w, &h) && swap && backbuffer_) {
        // The backbuffer holds GL's layout (row 0 = bottom); the swapchain shows row 0 at the top.
        SDL_GPUBlitInfo bi{};
        bi.source.texture = backbuffer_;
        bi.source.w = backbuffer_extent_.width;
        bi.source.h = backbuffer_extent_.height;
        bi.destination.texture = swap;
        bi.destination.w = w;
        bi.destination.h = h;
        bi.load_op = SDL_GPU_LOADOP_DONT_CARE;
        bi.flip_mode = SDL_FLIP_VERTICAL;
        bi.filter = SDL_GPU_FILTER_NEAREST;
        SDL_BlitGPUTexture(gcb, &bi);
    }
    SDL_SubmitGPUCommandBuffer(gcb);
}

Extent2D SdlGpuDevice::backbuffer_size() const {
    const platform::PixelSize s = window_.framebuffer_size();
    return Extent2D{s.width, s.height};
}

ResultV SdlGpuDevice::read_buffer(BufferHandle h, u64 offset, Span<byte> out) {
    const GpuBuffer* b = buffers_.try_get(h);
    if (!b) return Error(Errc::NotFound, "rhi.read_buffer", "stale buffer handle");
    if (offset + out.size() > b->size) return Error(Errc::InvalidArgument, "rhi.read_buffer", "range outside buffer");
    if (out.empty()) return ok();
    SDL_GPUTransferBufferCreateInfo ti{};
    ti.usage = SDL_GPU_TRANSFERBUFFERUSAGE_DOWNLOAD;
    ti.size = static_cast<Uint32>(out.size());
    SDL_GPUTransferBuffer* tb = SDL_CreateGPUTransferBuffer(device_, &ti);
    if (!tb) return Error(Errc::OutOfMemory, "rhi.read_buffer", SDL_GetError());

    flush_pending();
    SDL_GPUCommandBuffer* gcb = SDL_AcquireGPUCommandBuffer(device_);
    SDL_GPUCopyPass* copy = SDL_BeginGPUCopyPass(gcb);
    const SDL_GPUBufferRegion src{b->buffer, static_cast<Uint32>(offset), static_cast<Uint32>(out.size())};
    const SDL_GPUTransferBufferLocation dst{tb, 0};
    SDL_DownloadFromGPUBuffer(copy, &src, &dst);
    SDL_EndGPUCopyPass(copy);
    SDL_GPUFence* fence = SDL_SubmitGPUCommandBufferAndAcquireFence(gcb);
    if (fence) {
        SDL_WaitForGPUFences(device_, true, &fence, 1);
        SDL_ReleaseGPUFence(device_, fence);
    }
    const void* bytes = SDL_MapGPUTransferBuffer(device_, tb, false);
    if (!bytes) {
        SDL_ReleaseGPUTransferBuffer(device_, tb);
        return Error(Errc::Unsupported, "rhi.read_buffer", SDL_GetError());
    }
    std::memcpy(out.data(), bytes, out.size());
    SDL_UnmapGPUTransferBuffer(device_, tb);
    SDL_ReleaseGPUTransferBuffer(device_, tb);
    return ok();
}

ResultV SdlGpuDevice::read_pixels(TextureHandle target, Rect2D rect, Span<u8> out) {
    const usize row = usize(rect.width) * 4;
    if (out.size() < row * rect.height) return Error(Errc::InvalidArgument, "rhi.read_pixels", "output too small");
    SDL_GPUTexture* tex = nullptr;
    TextureFormat fmt = TextureFormat::RGBA8_UNorm;
    Extent2D size;
    if (target.is_null()) {
        tex = backbuffer_;
        size = backbuffer_extent_;
    } else {
        const GpuTexture* t = textures_.try_get(target);
        if (!t) return Error(Errc::NotFound, "rhi.read_pixels", "stale texture handle");
        if (t->samples > 1 || is_depth_format(t->format) || is_compressed_format(t->format))
            return Error(Errc::Unsupported, "rhi.read_pixels", "format not readable");
        tex = t->texture;
        fmt = t->format;
        size = t->size;
    }
    if (!tex) return Error(Errc::NotFound, "rhi.read_pixels", "no backbuffer");
    if (rect.x < 0 || rect.y < 0 || u32(rect.x) + rect.width > size.width || u32(rect.y) + rect.height > size.height)
        return Error(Errc::InvalidArgument, "rhi.read_pixels", "rect outside target");
    if (rect.width == 0 || rect.height == 0) return ok();

    // Every target holds GL's layout (row 0 = bottom); the result is top row first.
    const u32 src_y = size.height - (u32(rect.y) + rect.height);
    const usize bpp = bytes_per_pixel(fmt);
    const usize src_row = usize(rect.width) * bpp;
    SDL_GPUTransferBufferCreateInfo ti{};
    ti.usage = SDL_GPU_TRANSFERBUFFERUSAGE_DOWNLOAD;
    ti.size = static_cast<Uint32>(src_row * rect.height);
    SDL_GPUTransferBuffer* tb = SDL_CreateGPUTransferBuffer(device_, &ti);
    if (!tb) return Error(Errc::OutOfMemory, "rhi.read_pixels", SDL_GetError());

    flush_pending();
    SDL_GPUCommandBuffer* gcb = SDL_AcquireGPUCommandBuffer(device_);
    SDL_GPUCopyPass* copy = SDL_BeginGPUCopyPass(gcb);
    SDL_GPUTextureRegion src{};
    src.texture = tex;
    src.x = u32(rect.x);
    src.y = src_y;
    src.w = rect.width;
    src.h = rect.height;
    src.d = 1;
    SDL_GPUTextureTransferInfo dst{};
    dst.transfer_buffer = tb;
    SDL_DownloadFromGPUTexture(copy, &src, &dst);
    SDL_EndGPUCopyPass(copy);
    SDL_GPUFence* fence = SDL_SubmitGPUCommandBufferAndAcquireFence(gcb);
    if (fence) {
        SDL_WaitForGPUFences(device_, true, &fence, 1);
        SDL_ReleaseGPUFence(device_, fence);
    }
    const u8* bytes = static_cast<const u8*>(SDL_MapGPUTransferBuffer(device_, tb, false));
    if (!bytes) {
        SDL_ReleaseGPUTransferBuffer(device_, tb);
        return Error(Errc::Unsupported, "rhi.read_pixels", SDL_GetError());
    }
    for (u32 y = 0; y < rect.height; ++y) {
        const u8* s = bytes + usize(rect.height - 1 - y) * src_row;
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
                    u16 hv[4];
                    std::memcpy(hv, s + x * 8, 8);
                    for (u32 c = 0; c < 4; ++c) px[c] = unorm8(half_to_float(hv[c]));
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
    SDL_UnmapGPUTransferBuffer(device_, tb);
    SDL_ReleaseGPUTransferBuffer(device_, tb);
    return ok();
}

}  // namespace mce::rhi::sdlgpu
