// mce_rhi_webgpu: device, resources, frames, uploads, readback (ADR-0039, webgpu_device.h).

#include "webgpu_device.h"

#include <cmath>
#include <cstdio>
#include <cstring>

#include "mce/log.h"
#include "mce/math/math.h"

namespace mce::rhi::webgpu {
namespace {

WGPUStringView sv(const char* s) { return WGPUStringView{s, s ? WGPU_STRLEN : 0}; }

WGPUTextureFormat wgpu_format(TextureFormat f) {
    switch (f) {
        case TextureFormat::RGBA8_UNorm: return WGPUTextureFormat_RGBA8Unorm;
        case TextureFormat::RGBA8_SRGB: return WGPUTextureFormat_RGBA8UnormSrgb;
        case TextureFormat::BGRA8_UNorm: return WGPUTextureFormat_BGRA8Unorm;
        case TextureFormat::RGBA16F: return WGPUTextureFormat_RGBA16Float;
        case TextureFormat::RGBA32F: return WGPUTextureFormat_RGBA32Float;
        case TextureFormat::R8_UNorm: return WGPUTextureFormat_R8Unorm;
        case TextureFormat::RG8_UNorm: return WGPUTextureFormat_RG8Unorm;
        case TextureFormat::R11G11B10F: return WGPUTextureFormat_RG11B10Ufloat;
        case TextureFormat::BC1_UNorm: return WGPUTextureFormat_BC1RGBAUnorm;
        case TextureFormat::BC3_UNorm: return WGPUTextureFormat_BC3RGBAUnorm;
        case TextureFormat::BC5_UNorm: return WGPUTextureFormat_BC5RGUnorm;
        case TextureFormat::BC7_UNorm: return WGPUTextureFormat_BC7RGBAUnorm;
        case TextureFormat::BC7_SRGB: return WGPUTextureFormat_BC7RGBAUnormSrgb;
        case TextureFormat::D32_Float: return WGPUTextureFormat_Depth32Float;
        case TextureFormat::D24_UNorm_S8: return WGPUTextureFormat_Depth24PlusStencil8;
        case TextureFormat::D16_UNorm: return WGPUTextureFormat_Depth16Unorm;
        case TextureFormat::ASTC_4x4_UNorm: return WGPUTextureFormat_ASTC4x4Unorm;
        case TextureFormat::ASTC_4x4_SRGB: return WGPUTextureFormat_ASTC4x4UnormSrgb;
    }
    return WGPUTextureFormat_RGBA8Unorm;
}

usize level_bytes(TextureFormat f, u32 w, u32 h) {
    if (is_compressed_format(f)) {
        const usize block = f == TextureFormat::BC1_UNorm ? 8 : 16;
        return usize((w + 3) / 4) * ((h + 3) / 4) * block;
    }
    return usize(w) * h * bytes_per_pixel(f);
}

// Bytes of one row of blocks (compressed) or pixels, and the number of such rows.
void row_layout(TextureFormat f, u32 w, u32 h, u32& bytes_per_row, u32& rows) {
    if (is_compressed_format(f)) {
        bytes_per_row = ((w + 3) / 4) * (f == TextureFormat::BC1_UNorm ? 8u : 16u);
        rows = (h + 3) / 4;
    } else {
        bytes_per_row = w * bytes_per_pixel(f);
        rows = h;
    }
}

WGPUAddressMode wgpu_wrap(AddressMode m) {
    switch (m) {
        case AddressMode::Repeat: return WGPUAddressMode_Repeat;
        case AddressMode::ClampToEdge: return WGPUAddressMode_ClampToEdge;
        case AddressMode::MirroredRepeat: return WGPUAddressMode_MirrorRepeat;
    }
    return WGPUAddressMode_ClampToEdge;
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

constexpr WGPUBufferUsage kAllBufferUsage = WGPUBufferUsage_Vertex | WGPUBufferUsage_Index | WGPUBufferUsage_Indirect |
                                            WGPUBufferUsage_Storage | WGPUBufferUsage_Uniform | WGPUBufferUsage_CopyDst |
                                            WGPUBufferUsage_CopySrc;

WGPUBuffer make_buffer(WGPUDevice dev, usize size, WGPUBufferUsage usage, const char* name) {
    WGPUBufferDescriptor bd = WGPU_BUFFER_DESCRIPTOR_INIT;
    bd.label = sv(name);
    bd.usage = usage;
    bd.size = align_up(size, 4);
    return wgpuDeviceCreateBuffer(dev, &bd);
}

WGPUTextureView make_view(WGPUTexture t, WGPUTextureViewDimension dim, u32 base_mip, u32 mips, u32 base_layer,
                          u32 layers, WGPUTextureAspect aspect = WGPUTextureAspect_All) {
    WGPUTextureViewDescriptor vd = WGPU_TEXTURE_VIEW_DESCRIPTOR_INIT;
    vd.dimension = dim;
    vd.baseMipLevel = base_mip;
    vd.mipLevelCount = mips;
    vd.baseArrayLayer = base_layer;
    vd.arrayLayerCount = layers;
    vd.aspect = aspect;
    return wgpuTextureCreateView(t, &vd);
}

void uncaptured_error(WGPUDevice const*, WGPUErrorType type, WGPUStringView message, void* user, void*) {
    static_cast<WebGpuDevice*>(user)->on_uncaptured_error(type, message);
}

// present(): a fullscreen triangle that samples the backbuffer with v flipped (GL layout -> canvas).
constexpr const char* kBlitWgsl = R"(
@group(0) @binding(0) var src: texture_2d<f32>;
@group(0) @binding(1) var smp: sampler;
struct VsOut { @builtin(position) pos: vec4f, @location(0) uv: vec2f };
@vertex fn vs(@builtin(vertex_index) i: u32) -> VsOut {
    let p = vec2f(f32((i << 1u) & 2u), f32(i & 2u));
    var o: VsOut;
    o.pos = vec4f(p * 2.0 - 1.0, 0.0, 1.0);
    o.uv = vec2f(p.x, p.y);
    return o;
}
@fragment fn fs(v: VsOut) -> @location(0) vec4f { return vec4f(textureSample(src, smp, v.uv).rgb, 1.0); }
)";

}  // namespace

bool TargetKey::operator==(const TargetKey& o) const {
    if (color_count != o.color_count || samples != o.samples || has_depth != o.has_depth || depth != o.depth) return false;
    for (u32 i = 0; i < color_count; ++i)
        if (colors[i] != o.colors[i]) return false;
    return true;
}

// ---- creation --------------------------------------------------------------------------------

Result<mem::UniquePtr<RenderDevice>> create_device(platform::Window& window, const DeviceDesc& desc) {
    WebGpuDevice* dev = mem::make<WebGpuDevice>(mem::heap(), window, desc);
    mem::UniquePtr<RenderDevice> owned(mem::heap(), dev);
    MCE_TRY(dev->init());
    return owned;
}

WebGpuDevice::WebGpuDevice(platform::Window& window, const DeviceDesc& desc) : window_(window), desc_(desc) {}

WebGpuDevice::~WebGpuDevice() { release_all(); }

void WebGpuDevice::on_uncaptured_error(WGPUErrorType type, WGPUStringView message) {
    ++validation_errors_;
    MCE_LOG_ERROR("rhi.webgpu: error {}: {}", static_cast<u32>(type),
                  StringView(message.data ? message.data : "", message.data ? (message.length == WGPU_STRLEN
                                                                                    ? std::strlen(message.data)
                                                                                    : message.length)
                                                                              : 0));
}

bool WebGpuDevice::wait(WGPUFuture future) {
    WGPUFutureWaitInfo info{future, false};
    const WGPUWaitStatus st = wgpuInstanceWaitAny(instance_, 1, &info, UINT64_MAX);
    return st == WGPUWaitStatus_Success && info.completed;
}

ResultV WebGpuDevice::request_device() {
    static constexpr WGPUInstanceFeatureName kInstanceFeatures[] = {WGPUInstanceFeatureName_TimedWaitAny};
    WGPUInstanceDescriptor id = WGPU_INSTANCE_DESCRIPTOR_INIT;
    id.requiredFeatureCount = 1;
    id.requiredFeatures = kInstanceFeatures;
    instance_ = wgpuCreateInstance(&id);
    if (!instance_) return Error(Errc::Unsupported, "rhi.webgpu.instance", "this browser has no WebGPU");

    struct AdapterOut {
        WGPUAdapter adapter = nullptr;
        bool done = false;
    } a;
    WGPURequestAdapterOptions ao = WGPU_REQUEST_ADAPTER_OPTIONS_INIT;
    ao.powerPreference = WGPUPowerPreference_HighPerformance;
    WGPURequestAdapterCallbackInfo acb = WGPU_REQUEST_ADAPTER_CALLBACK_INFO_INIT;
    acb.mode = WGPUCallbackMode_WaitAnyOnly;
    acb.callback = [](WGPURequestAdapterStatus status, WGPUAdapter adapter, WGPUStringView, void* u, void*) {
        auto* out = static_cast<AdapterOut*>(u);
        out->done = true;
        if (status == WGPURequestAdapterStatus_Success) out->adapter = adapter;
    };
    acb.userdata1 = &a;
    if (!wait(wgpuInstanceRequestAdapter(instance_, &ao, acb)) || !a.adapter)
        return Error(Errc::Unsupported, "rhi.webgpu.adapter", "navigator.gpu.requestAdapter() found no adapter");
    adapter_ = a.adapter;

    // Optional features, each used only when the adapter has it.
    WGPUFeatureName wanted[] = {WGPUFeatureName_Float32Filterable, WGPUFeatureName_TextureCompressionBC,
                                WGPUFeatureName_TextureCompressionASTC, WGPUFeatureName_RG11B10UfloatRenderable,
                                WGPUFeatureName_IndirectFirstInstance, WGPUFeatureName_Depth32FloatStencil8};
    WGPUFeatureName features[8];
    usize feature_count = 0;
    for (const WGPUFeatureName f : wanted)
        if (wgpuAdapterHasFeature(adapter_, f)) features[feature_count++] = f;
    float32_filterable_ = wgpuAdapterHasFeature(adapter_, WGPUFeatureName_Float32Filterable);

    WGPULimits supported = WGPU_LIMITS_INIT;
    wgpuAdapterGetLimits(adapter_, &supported);
    WGPULimits limits = WGPU_LIMITS_INIT;
    // The banks: 16 sampled textures + 16 samplers per stage (shader_wgsl.h), the storage banks.
    limits.maxSampledTexturesPerShaderStage = math::min(supported.maxSampledTexturesPerShaderStage, 16u);
    limits.maxSamplersPerShaderStage = math::min(supported.maxSamplersPerShaderStage, 16u);
    limits.maxStorageBuffersPerShaderStage = math::min(supported.maxStorageBuffersPerShaderStage, 10u);
    limits.maxBindingsPerBindGroup = math::min(supported.maxBindingsPerBindGroup, 1000u);
    limits.maxStorageBufferBindingSize = supported.maxStorageBufferBindingSize;
    limits.maxBufferSize = supported.maxBufferSize;
    limits.maxTextureDimension2D = supported.maxTextureDimension2D;
    limits.maxTextureArrayLayers = supported.maxTextureArrayLayers;
    limits.maxComputeInvocationsPerWorkgroup = supported.maxComputeInvocationsPerWorkgroup;
    limits.maxComputeWorkgroupSizeX = supported.maxComputeWorkgroupSizeX;
    limits.maxComputeWorkgroupSizeY = supported.maxComputeWorkgroupSizeY;
    limits.maxComputeWorkgroupSizeZ = supported.maxComputeWorkgroupSizeZ;
    limits.maxComputeWorkgroupStorageSize = supported.maxComputeWorkgroupStorageSize;

    struct DeviceOut {
        WGPUDevice device = nullptr;
        FixedString<256> message;
    } d;
    WGPUDeviceDescriptor dd = WGPU_DEVICE_DESCRIPTOR_INIT;
    dd.label = sv("mce");
    dd.requiredFeatureCount = feature_count;
    dd.requiredFeatures = features;
    dd.requiredLimits = &limits;
    dd.uncapturedErrorCallbackInfo.callback = uncaptured_error;
    dd.uncapturedErrorCallbackInfo.userdata1 = this;
    WGPURequestDeviceCallbackInfo dcb = WGPU_REQUEST_DEVICE_CALLBACK_INFO_INIT;
    dcb.mode = WGPUCallbackMode_WaitAnyOnly;
    dcb.callback = [](WGPURequestDeviceStatus status, WGPUDevice device, WGPUStringView message, void* u, void*) {
        auto* out = static_cast<DeviceOut*>(u);
        if (status == WGPURequestDeviceStatus_Success) out->device = device;
        else if (message.data) out->message.append(StringView(message.data, message.length == WGPU_STRLEN ? std::strlen(message.data) : message.length));
    };
    dcb.userdata1 = &d;
    if (!wait(wgpuAdapterRequestDevice(adapter_, &dd, dcb)) || !d.device) {
        MCE_LOG_ERROR("rhi.webgpu: requestDevice failed: {}", d.message.c_str());
        return Error(Errc::Unsupported, "rhi.webgpu.device", d.message.c_str());
    }
    device_ = d.device;
    queue_ = wgpuDeviceGetQueue(device_);

    WGPULimits got = WGPU_LIMITS_INIT;
    wgpuDeviceGetLimits(device_, &got);
    caps_.api_name.append("WebGPU");
    WGPUAdapterInfo info = WGPU_ADAPTER_INFO_INIT;
    if (wgpuAdapterGetInfo(adapter_, &info) == WGPUStatus_Success) {
        auto put = [&](WGPUStringView s) {
            if (s.data && s.length && s.length != WGPU_STRLEN) {
                if (!caps_.device_name.empty()) caps_.device_name.append(" ");
                caps_.device_name.append(StringView(s.data, s.length));
            }
        };
        put(info.vendor);
        put(info.architecture);
        put(info.description);
        wgpuAdapterInfoFreeMembers(info);
    }
    caps_.api_major = 1;
    caps_.api_minor = 0;
    caps_.max_texture_size = got.maxTextureDimension2D;
    uniform_bind_size_ = static_cast<u32>(math::min<u64>(got.maxUniformBufferBindingSize, 16384));
    caps_.max_uniform_block_size = uniform_bind_size_;
    caps_.shader_draw_parameters = false;  // WGSL has no base instance (shader_wgsl.h)
    caps_.timestamp_queries = false;       // timestamp-query is optional on the web; not wired yet (ADR-0039)
    caps_.storage_buffer_alignment = got.minStorageBufferOffsetAlignment;
    caps_.compute = true;
    caps_.max_compute_invocations = got.maxComputeInvocationsPerWorkgroup;
    caps_.max_compute_group_size[0] = got.maxComputeWorkgroupSizeX;
    caps_.max_compute_group_size[1] = got.maxComputeWorkgroupSizeY;
    caps_.max_compute_group_size[2] = got.maxComputeWorkgroupSizeZ;
    for (u32& n : caps_.max_compute_group_count) n = got.maxComputeWorkgroupsPerDimension;
    caps_.max_storage_image_units = kMaxImageUnits;
    MCE_LOG_INFO("rhi.webgpu: {} on {}", caps_.api_name.c_str(), caps_.device_name.c_str());
    return ok();
}

ResultV WebGpuDevice::init() {
    MCE_TRY(request_device());

    WGPUEmscriptenSurfaceSourceCanvasHTMLSelector canvas = WGPU_EMSCRIPTEN_SURFACE_SOURCE_CANVAS_HTML_SELECTOR_INIT;
    canvas.selector = sv("#canvas");
    WGPUSurfaceDescriptor sd = WGPU_SURFACE_DESCRIPTOR_INIT;
    sd.nextInChain = &canvas.chain;
    surface_ = wgpuInstanceCreateSurface(instance_, &sd);
    if (!surface_) return Error(Errc::Unsupported, "rhi.webgpu.surface", "no #canvas element");
    WGPUSurfaceCapabilities sc = WGPU_SURFACE_CAPABILITIES_INIT;
    if (wgpuSurfaceGetCapabilities(surface_, adapter_, &sc) == WGPUStatus_Success && sc.formatCount > 0)
        surface_format_ = sc.formats[0];
    wgpuSurfaceCapabilitiesFreeMembers(sc);
    vsync_ = desc_.vsync;

    // Rings (one per kind; the frame restarts them, webgpu_device.h).
    auto make_ring = [&](Ring& r, usize bytes, WGPUBufferUsage usage, const char* name) {
        r.capacity = align_up(bytes, 256);
        r.buffer = make_buffer(device_, r.capacity, usage | WGPUBufferUsage_CopyDst, name);
        r.staging.resize(r.capacity);
        return r.buffer != nullptr;
    };
    if (!make_ring(uniforms_, desc_.uniform_ring_bytes + uniform_bind_size_, WGPUBufferUsage_Uniform, "mce.ring.uniform") ||
        !make_ring(geometry_, desc_.geometry_ring_bytes, WGPUBufferUsage_Vertex | WGPUBufferUsage_Index, "mce.ring.geometry"))
        return Error(Errc::OutOfMemory, "rhi.webgpu.ring", "ring buffers");

    // Group 0: the four uniform banks, every bank bound at a dynamic offset into the uniform ring.
    auto uniform_layout = [&](WGPUShaderStage visibility, WGPUBindGroupLayout& layout, WGPUBindGroup& group) {
        WGPUBindGroupLayoutEntry e[kMaxUniformBanks];
        WGPUBindGroupEntry b[kMaxUniformBanks];
        for (u32 i = 0; i < kMaxUniformBanks; ++i) {
            e[i] = WGPU_BIND_GROUP_LAYOUT_ENTRY_INIT;
            e[i].binding = i;
            e[i].visibility = visibility;
            e[i].buffer.type = WGPUBufferBindingType_Uniform;
            e[i].buffer.hasDynamicOffset = true;
            b[i] = WGPU_BIND_GROUP_ENTRY_INIT;
            b[i].binding = i;
            b[i].buffer = uniforms_.buffer;
            b[i].offset = 0;
            b[i].size = uniform_bind_size_;
        }
        WGPUBindGroupLayoutDescriptor ld = WGPU_BIND_GROUP_LAYOUT_DESCRIPTOR_INIT;
        ld.entryCount = kMaxUniformBanks;
        ld.entries = e;
        layout = wgpuDeviceCreateBindGroupLayout(device_, &ld);
        WGPUBindGroupDescriptor gd = WGPU_BIND_GROUP_DESCRIPTOR_INIT;
        gd.layout = layout;
        gd.entryCount = kMaxUniformBanks;
        gd.entries = b;
        group = wgpuDeviceCreateBindGroup(device_, &gd);
    };
    uniform_layout(WGPUShaderStage_Vertex | WGPUShaderStage_Fragment, uniform_layout_, uniform_group_);
    uniform_layout(WGPUShaderStage_Compute, uniform_layout_compute_, uniform_group_compute_);
    {
        WGPUBindGroupLayoutDescriptor ld = WGPU_BIND_GROUP_LAYOUT_DESCRIPTOR_INIT;
        empty_layout_ = wgpuDeviceCreateBindGroupLayout(device_, &ld);
        WGPUBindGroupDescriptor gd = WGPU_BIND_GROUP_DESCRIPTOR_INIT;
        gd.layout = empty_layout_;
        empty_group_ = wgpuDeviceCreateBindGroup(device_, &gd);
    }

    // Dummies for unbound slots: white color textures, depth textures for the shadow kinds.
    for (u32 i = 0; i < 4; ++i) {
        const bool depth = i >= 2;
        WGPUTextureDescriptor td = WGPU_TEXTURE_DESCRIPTOR_INIT;
        td.size = WGPUExtent3D{1, 1, 1};
        td.format = depth ? WGPUTextureFormat_Depth32Float : WGPUTextureFormat_RGBA8Unorm;
        td.usage = WGPUTextureUsage_TextureBinding | (depth ? WGPUTextureUsage_RenderAttachment : WGPUTextureUsage_CopyDst);
        dummy_tex_[i] = wgpuDeviceCreateTexture(device_, &td);
        const bool array = i == 1 || i == 3;
        dummy_view_[i] = make_view(dummy_tex_[i], array ? WGPUTextureViewDimension_2DArray : WGPUTextureViewDimension_2D,
                                   0, 1, 0, 1);
        if (!depth) {
            static constexpr u8 kWhite[4] = {255, 255, 255, 255};
            WGPUTexelCopyTextureInfo dst = WGPU_TEXEL_COPY_TEXTURE_INFO_INIT;
            dst.texture = dummy_tex_[i];
            WGPUTexelCopyBufferLayout layout = WGPU_TEXEL_COPY_BUFFER_LAYOUT_INIT;
            layout.bytesPerRow = 4;
            layout.rowsPerImage = 1;
            const WGPUExtent3D one{1, 1, 1};
            wgpuQueueWriteTexture(queue_, &dst, kWhite, 4, &layout, &one);
        }
    }
    WGPUSamplerDescriptor sdesc = WGPU_SAMPLER_DESCRIPTOR_INIT;
    dummy_sampler_ = wgpuDeviceCreateSampler(device_, &sdesc);
    sdesc.compare = WGPUCompareFunction_LessEqual;
    dummy_compare_sampler_ = wgpuDeviceCreateSampler(device_, &sdesc);
    dummy_storage_ = make_buffer(device_, 256, kAllBufferUsage, "mce.dummy.storage");
    if (!dummy_sampler_ || !dummy_compare_sampler_ || !dummy_storage_ || !uniform_group_ || !empty_group_)
        return Error(Errc::OutOfMemory, "rhi.webgpu.dummy", "dummy resources");

    MCE_TRY(make_blit_pipeline());
    ensure_backbuffer();
    return ok();
}

ResultV WebGpuDevice::make_blit_pipeline() {
    WGPUShaderSourceWGSL src = WGPU_SHADER_SOURCE_WGSL_INIT;
    src.code = sv(kBlitWgsl);
    WGPUShaderModuleDescriptor md = WGPU_SHADER_MODULE_DESCRIPTOR_INIT;
    md.nextInChain = &src.chain;
    md.label = sv("mce.blit");
    WGPUShaderModule module = wgpuDeviceCreateShaderModule(device_, &md);

    WGPUBindGroupLayoutEntry e[2] = {WGPU_BIND_GROUP_LAYOUT_ENTRY_INIT, WGPU_BIND_GROUP_LAYOUT_ENTRY_INIT};
    e[0].binding = 0;
    e[0].visibility = WGPUShaderStage_Fragment;
    e[0].texture.sampleType = WGPUTextureSampleType_Float;
    e[0].texture.viewDimension = WGPUTextureViewDimension_2D;
    e[1].binding = 1;
    e[1].visibility = WGPUShaderStage_Fragment;
    e[1].sampler.type = WGPUSamplerBindingType_Filtering;
    WGPUBindGroupLayoutDescriptor ld = WGPU_BIND_GROUP_LAYOUT_DESCRIPTOR_INIT;
    ld.entryCount = 2;
    ld.entries = e;
    blit_layout_ = wgpuDeviceCreateBindGroupLayout(device_, &ld);
    WGPUPipelineLayoutDescriptor pld = WGPU_PIPELINE_LAYOUT_DESCRIPTOR_INIT;
    pld.bindGroupLayoutCount = 1;
    pld.bindGroupLayouts = &blit_layout_;
    WGPUPipelineLayout layout = wgpuDeviceCreatePipelineLayout(device_, &pld);

    WGPUColorTargetState color = WGPU_COLOR_TARGET_STATE_INIT;
    color.format = surface_format_;
    WGPUFragmentState fs = WGPU_FRAGMENT_STATE_INIT;
    fs.module = module;
    fs.entryPoint = sv("fs");
    fs.targetCount = 1;
    fs.targets = &color;
    WGPURenderPipelineDescriptor pd = WGPU_RENDER_PIPELINE_DESCRIPTOR_INIT;
    pd.label = sv("mce.blit");
    pd.layout = layout;
    pd.vertex.module = module;
    pd.vertex.entryPoint = sv("vs");
    pd.fragment = &fs;
    blit_pipeline_ = wgpuDeviceCreateRenderPipeline(device_, &pd);
    wgpuPipelineLayoutRelease(layout);
    wgpuShaderModuleRelease(module);

    WGPUSamplerDescriptor sdesc = WGPU_SAMPLER_DESCRIPTOR_INIT;
    blit_sampler_ = wgpuDeviceCreateSampler(device_, &sdesc);
    if (!blit_pipeline_ || !blit_sampler_) return Error(Errc::PipelineCreate, "rhi.webgpu.blit", "present pipeline");
    return ok();
}

void WebGpuDevice::release_pipeline(GpuPipeline& p) {
    for (const PipelineVariant& v : p.variants) wgpuRenderPipelineRelease(v.pipeline);
    p.variants.clear();
    if (p.cs) wgpuComputePipelineRelease(p.cs);
    if (p.vs) wgpuShaderModuleRelease(p.vs);
    if (p.fs) wgpuShaderModuleRelease(p.fs);
    for (u32 g = 0; g < 4; ++g) {
        if (p.groups[g]) wgpuBindGroupRelease(p.groups[g]);
        if (p.owns_layout[g] && p.layouts[g]) wgpuBindGroupLayoutRelease(p.layouts[g]);
    }
    if (p.layout) wgpuPipelineLayoutRelease(p.layout);
    p = GpuPipeline{};
}

void WebGpuDevice::release_all() {
    if (!device_) {
        if (adapter_) wgpuAdapterRelease(adapter_);
        if (instance_) wgpuInstanceRelease(instance_);
        return;
    }
    for (WGPUBindGroup g : retired_groups_) wgpuBindGroupRelease(g);
    retired_groups_.clear();
    for (const GpuBuffer& b : buffers_.dense()) wgpuBufferRelease(b.buffer);
    for (GpuTexture& t : textures_.dense()) {
        for (WGPUTextureView v : t.targets) wgpuTextureViewRelease(v);
        for (WGPUTextureView v : t.mip_views) wgpuTextureViewRelease(v);
        wgpuTextureViewRelease(t.view);
        wgpuTextureRelease(t.texture);
    }
    for (const GpuSampler& s : samplers_.dense()) wgpuSamplerRelease(s.sampler);
    for (GpuPipeline& p : pipelines_.dense()) release_pipeline(p);
    for (Ring* r : {&uniforms_, &geometry_})
        if (r->buffer) wgpuBufferRelease(r->buffer);
    for (Ring& r : storage_)
        if (r.buffer) wgpuBufferRelease(r.buffer);
    for (u32 i = 0; i < 4; ++i) {
        if (dummy_view_[i]) wgpuTextureViewRelease(dummy_view_[i]);
        if (dummy_tex_[i]) wgpuTextureRelease(dummy_tex_[i]);
    }
    if (dummy_sampler_) wgpuSamplerRelease(dummy_sampler_);
    if (dummy_compare_sampler_) wgpuSamplerRelease(dummy_compare_sampler_);
    if (dummy_storage_) wgpuBufferRelease(dummy_storage_);
    for (WGPUBindGroup g : {uniform_group_, uniform_group_compute_, empty_group_, blit_group_})
        if (g) wgpuBindGroupRelease(g);
    for (WGPUBindGroupLayout l : {uniform_layout_, uniform_layout_compute_, empty_layout_, blit_layout_})
        if (l) wgpuBindGroupLayoutRelease(l);
    if (blit_pipeline_) wgpuRenderPipelineRelease(blit_pipeline_);
    if (blit_sampler_) wgpuSamplerRelease(blit_sampler_);
    if (backbuffer_view_) wgpuTextureViewRelease(backbuffer_view_);
    if (backbuffer_) wgpuTextureRelease(backbuffer_);
    if (backbuffer_depth_view_) wgpuTextureViewRelease(backbuffer_depth_view_);
    if (backbuffer_depth_) wgpuTextureRelease(backbuffer_depth_);
    if (surface_) {
        wgpuSurfaceUnconfigure(surface_);
        wgpuSurfaceRelease(surface_);
    }
    if (queue_) wgpuQueueRelease(queue_);
    wgpuDeviceRelease(device_);
    if (adapter_) wgpuAdapterRelease(adapter_);
    if (instance_) wgpuInstanceRelease(instance_);
    device_ = nullptr;
}

void WebGpuDevice::configure_surface(Extent2D size) {
    if (size == surface_extent_ || size.width == 0 || size.height == 0) return;
    WGPUSurfaceConfiguration c = WGPU_SURFACE_CONFIGURATION_INIT;
    c.device = device_;
    c.format = surface_format_;
    c.usage = WGPUTextureUsage_RenderAttachment;
    c.width = size.width;
    c.height = size.height;
    c.alphaMode = WGPUCompositeAlphaMode_Opaque;
    c.presentMode = WGPUPresentMode_Fifo;  // the browser presents on its own frame clock
    wgpuSurfaceConfigure(surface_, &c);
    surface_extent_ = size;
}

// The offscreen backbuffer and its depth buffer follow the canvas's pixel size.
void WebGpuDevice::ensure_backbuffer() {
    const Extent2D want = backbuffer_size();
    if (backbuffer_ && want == backbuffer_extent_) return;
    if (want.width == 0 || want.height == 0) return;
    if (backbuffer_view_) wgpuTextureViewRelease(backbuffer_view_);
    if (backbuffer_) wgpuTextureRelease(backbuffer_);
    if (backbuffer_depth_view_) wgpuTextureViewRelease(backbuffer_depth_view_);
    if (backbuffer_depth_) wgpuTextureRelease(backbuffer_depth_);
    WGPUTextureDescriptor td = WGPU_TEXTURE_DESCRIPTOR_INIT;
    td.label = sv("mce.backbuffer");
    td.size = WGPUExtent3D{want.width, want.height, 1};
    td.format = WGPUTextureFormat_RGBA8Unorm;
    td.usage = WGPUTextureUsage_RenderAttachment | WGPUTextureUsage_TextureBinding | WGPUTextureUsage_CopySrc;
    backbuffer_ = wgpuDeviceCreateTexture(device_, &td);
    backbuffer_view_ = wgpuTextureCreateView(backbuffer_, nullptr);
    td.label = sv("mce.backbuffer.depth");
    td.format = WGPUTextureFormat_Depth24PlusStencil8;
    td.usage = WGPUTextureUsage_RenderAttachment;
    backbuffer_depth_ = wgpuDeviceCreateTexture(device_, &td);
    backbuffer_depth_view_ = wgpuTextureCreateView(backbuffer_depth_, nullptr);
    backbuffer_extent_ = want;

    if (blit_group_) wgpuBindGroupRelease(blit_group_);
    WGPUBindGroupEntry e[2] = {WGPU_BIND_GROUP_ENTRY_INIT, WGPU_BIND_GROUP_ENTRY_INIT};
    e[0].binding = 0;
    e[0].textureView = backbuffer_view_;
    e[1].binding = 1;
    e[1].sampler = blit_sampler_;
    WGPUBindGroupDescriptor gd = WGPU_BIND_GROUP_DESCRIPTOR_INIT;
    gd.layout = blit_layout_;
    gd.entryCount = 2;
    gd.entries = e;
    blit_group_ = wgpuDeviceCreateBindGroup(device_, &gd);
}

// ---- resources -------------------------------------------------------------------------------

BufferHandle WebGpuDevice::create_buffer(const BufferDesc& desc, Span<const byte> initial_data) {
    MCE_ASSERT_MSG(desc.size_bytes > 0, "create_buffer: size 0");
    MCE_ASSERT_MSG(initial_data.empty() || initial_data.size() <= desc.size_bytes, "initial data larger than buffer");
    GpuBuffer b;
    b.size = desc.size_bytes;
    b.buffer = make_buffer(device_, desc.size_bytes, kAllBufferUsage, desc.debug_name);
    if (!b.buffer) {
        MCE_LOG_ERROR("rhi.webgpu: buffer '{}' ({} bytes) failed", desc.debug_name, desc.size_bytes);
        return {};
    }
    const BufferHandle h = buffers_.insert(b);
    if (!initial_data.empty()) update_buffer(h, 0, initial_data);
    return h;
}

void WebGpuDevice::update_buffer(BufferHandle h, u64 offset, Span<const byte> data) {
    const GpuBuffer* b = buffers_.try_get(h);
    MCE_ASSERT_MSG(b, "update_buffer: stale handle");
    if (!b || data.empty()) return;
    MCE_ASSERT_MSG(offset + data.size() <= b->size, "update_buffer: out of range");
    if (offset + data.size() > b->size) return;
    // writeBuffer needs 4-aligned offsets and sizes: widen to the aligned range, keeping the
    // neighbouring bytes only when the data already covers them (the engine writes whole structs).
    if ((offset & 3u) == 0 && (data.size() & 3u) == 0) {
        wgpuQueueWriteBuffer(queue_, b->buffer, offset, data.data(), data.size());
        return;
    }
    MCE_ASSERT_MSG((offset & 3u) == 0, "update_buffer: WebGPU needs 4-aligned offsets");
    if (offset & 3u) return;
    Array<byte> padded;
    padded.resize(align_up(data.size(), 4));
    std::memset(padded.data(), 0, padded.size());
    std::memcpy(padded.data(), data.data(), data.size());
    wgpuQueueWriteBuffer(queue_, b->buffer, offset, padded.data(), padded.size());
}

TextureHandle WebGpuDevice::create_texture(const TextureDesc& desc, Span<const byte> initial_data) {
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
    t.samples = desc.samples > 1 ? 4 : 1;  // WebGPU multisampling is 4x only
    MCE_ASSERT_MSG(t.samples == 1 || (!array && t.mips == 1 && initial_data.empty()),
                   "create_texture: multisampled textures are single-mip 2D render targets");
    t.wgpu_format = wgpu_format(desc.format);

    WGPUTextureUsage usage = WGPUTextureUsage_CopyDst | WGPUTextureUsage_CopySrc;
    switch (desc.usage) {
        case TextureUsage::Sampled: usage |= WGPUTextureUsage_TextureBinding; break;
        case TextureUsage::ColorTarget:
        case TextureUsage::SampledColorTarget:
        case TextureUsage::DepthTarget: usage |= WGPUTextureUsage_RenderAttachment; break;
        case TextureUsage::SampledStorage: usage |= WGPUTextureUsage_StorageBinding | WGPUTextureUsage_TextureBinding; break;
    }
    // Render targets are sampled by later passes (shadow maps, post chains) unless multisampled.
    if (t.samples == 1) usage |= WGPUTextureUsage_TextureBinding;
    if (is_compressed_format(desc.format)) usage &= ~WGPUTextureUsage_CopySrc;

    WGPUTextureDescriptor td = WGPU_TEXTURE_DESCRIPTOR_INIT;
    td.label = sv(desc.debug_name);
    td.usage = usage;
    td.size = WGPUExtent3D{desc.size.width, desc.size.height, t.layers};
    td.format = t.wgpu_format;
    td.mipLevelCount = t.mips;
    td.sampleCount = t.samples;
    t.texture = wgpuDeviceCreateTexture(device_, &td);
    if (!t.texture) {
        MCE_LOG_ERROR("rhi.webgpu: texture '{}' failed", desc.debug_name);
        ++validation_errors_;
        return {};
    }
    const bool depth = is_depth_format(desc.format);
    // Sampling views see the depth aspect only (depth24plus-stencil8 has two).
    const WGPUTextureAspect aspect = depth ? WGPUTextureAspect_DepthOnly : WGPUTextureAspect_All;
    if (t.samples == 1)
        t.view = make_view(t.texture, array ? WGPUTextureViewDimension_2DArray : WGPUTextureViewDimension_2D, 0, t.mips,
                           0, t.layers, aspect);
    for (u32 l = 0; l < t.layers; ++l)
        if (desc.usage != TextureUsage::Sampled)
            t.targets.push_back(make_view(t.texture, WGPUTextureViewDimension_2D, 0, 1, l, 1));
    if (desc.usage == TextureUsage::SampledStorage)
        for (u32 m = 0; m < t.mips; ++m)
            t.mip_views.push_back(make_view(t.texture, array ? WGPUTextureViewDimension_2DArray : WGPUTextureViewDimension_2D,
                                            m, 1, 0, t.layers));

    for (u32 m = 0; m < t.mips; ++m)
        t.bytes += level_bytes(desc.format, math::max(1u, desc.size.width >> m), math::max(1u, desc.size.height >> m)) *
                   t.layers;
    t.bytes *= t.samples;
    texture_bytes_ += t.bytes;
    const TextureHandle h = textures_.insert(std::move(t));

    if (!initial_data.empty()) {
        const usize layer_bytes = level_bytes(desc.format, desc.size.width, desc.size.height);
        MCE_ASSERT_MSG(initial_data.size() >= layer_bytes * (array ? desc.array_layers : 1u),
                       "create_texture: initial data smaller than mip 0");
        MCE_ASSERT_MSG(!depth, "create_texture: depth formats take no initial data");
        const u16 layers = textures_.try_get(h)->layers;
        if (initial_data.size() >= layer_bytes * layers && !depth)
            for (u32 l = 0; l < layers; ++l) {
                TextureRegion region;
                region.layer = l;
                update_texture(h, region, initial_data.subspan(usize(l) * layer_bytes, layer_bytes));
            }
    }
    return h;
}

void WebGpuDevice::update_texture(TextureHandle h, const TextureRegion& region, Span<const byte> data) {
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
    u32 bytes_per_row = 0, rows = 0;
    row_layout(t->format, rw, rh, bytes_per_row, rows);
    WGPUTexelCopyTextureInfo dst = WGPU_TEXEL_COPY_TEXTURE_INFO_INIT;
    dst.texture = t->texture;
    dst.mipLevel = region.mip;
    dst.origin = WGPUOrigin3D{region.x, region.y, region.layer};
    WGPUTexelCopyBufferLayout layout = WGPU_TEXEL_COPY_BUFFER_LAYOUT_INIT;
    layout.bytesPerRow = bytes_per_row;
    layout.rowsPerImage = rows;
    // Compressed copies cover whole blocks: a mip smaller than a block copies the full block.
    u32 cw = rw, ch = rh;
    if (is_compressed_format(t->format)) {
        cw = align_up(rw, 4u);
        ch = align_up(rh, 4u);
    }
    const WGPUExtent3D size{cw, ch, 1};
    wgpuQueueWriteTexture(queue_, &dst, data.data(), need, &layout, &size);
}

SamplerHandle WebGpuDevice::create_sampler(const SamplerDesc& desc) {
    WGPUSamplerDescriptor sd = WGPU_SAMPLER_DESCRIPTOR_INIT;
    sd.minFilter = desc.min_filter == Filter::Nearest ? WGPUFilterMode_Nearest : WGPUFilterMode_Linear;
    sd.magFilter = desc.mag_filter == Filter::Nearest ? WGPUFilterMode_Nearest : WGPUFilterMode_Linear;
    sd.mipmapFilter = desc.mip == MipFilter::Linear ? WGPUMipmapFilterMode_Linear : WGPUMipmapFilterMode_Nearest;
    sd.addressModeU = wgpu_wrap(desc.wrap_s);
    sd.addressModeV = wgpu_wrap(desc.wrap_t);
    sd.addressModeW = WGPUAddressMode_ClampToEdge;
    sd.lodMinClamp = 0.0f;
    sd.lodMaxClamp = desc.mip == MipFilter::None ? 0.0f : 1000.0f;  // GL: no mip filter samples level 0 only
    // WebGPU allows anisotropy only with linear filtering everywhere.
    const bool aniso = desc.max_aniso > 1.0f && desc.min_filter == Filter::Linear && desc.mag_filter == Filter::Linear &&
                       desc.mip == MipFilter::Linear;
    sd.maxAnisotropy = aniso ? static_cast<u16>(math::min(desc.max_aniso, 16.0f)) : 1;
    if (desc.compare) sd.compare = wgpu_compare(desc.compare_op);
    GpuSampler s;
    s.sampler = wgpuDeviceCreateSampler(device_, &sd);
    s.compare = desc.compare;
    if (!s.sampler) {
        MCE_LOG_ERROR("rhi.webgpu: sampler failed");
        return {};
    }
    return samplers_.insert(s);
}

// Builds groups 1..3 for a pipeline from what its stages declare (group 0 is shared).
ResultV WebGpuDevice::make_layouts(GpuPipeline& p, bool compute) {
    const StageResources* stages[2] = {compute ? &p.cres : &p.vres, compute ? nullptr : &p.fres};
    const WGPUShaderStage stage_bits[2] = {compute ? WGPUShaderStage_Compute : WGPUShaderStage_Vertex,
                                           WGPUShaderStage_Fragment};
    WGPUBindGroupLayoutEntry entries[2 * kMaxSamplers + kMaxStorageBindings];

    // Group 1: textures + their samplers.
    u32 n = 0;
    for (u32 s = 0; s < kMaxSamplers; ++s) {
        WGPUShaderStage vis = WGPUShaderStage_None;
        SamplerKind kind = SamplerKind::Tex2D;
        for (u32 i = 0; i < 2; ++i) {
            if (!stages[i] || !(stages[i]->sampler_mask & (1u << s))) continue;
            if (vis != WGPUShaderStage_None && stages[i]->sampler_kind[s] != kind)
                return Error(Errc::PipelineCreate, "rhi.webgpu.layout", "sampler binding declared with two types");
            vis |= stage_bits[i];
            kind = stages[i]->sampler_kind[s];
        }
        if (vis == WGPUShaderStage_None) continue;
        const bool shadow = kind == SamplerKind::Shadow2D || kind == SamplerKind::Shadow2DArray;
        const bool array = kind == SamplerKind::Tex2DArray || kind == SamplerKind::Shadow2DArray;
        WGPUBindGroupLayoutEntry& t = entries[n++];
        t = WGPU_BIND_GROUP_LAYOUT_ENTRY_INIT;
        t.binding = s;
        t.visibility = vis;
        t.texture.sampleType = shadow ? WGPUTextureSampleType_Depth : WGPUTextureSampleType_Float;
        t.texture.viewDimension = array ? WGPUTextureViewDimension_2DArray : WGPUTextureViewDimension_2D;
        WGPUBindGroupLayoutEntry& sm = entries[n++];
        sm = WGPU_BIND_GROUP_LAYOUT_ENTRY_INIT;
        sm.binding = kSamplerBindingBase + s;
        sm.visibility = vis;
        sm.sampler.type = shadow ? WGPUSamplerBindingType_Comparison : WGPUSamplerBindingType_Filtering;
    }
    auto make = [&](u32 group, u32 count) {
        if (count == 0) {
            p.layouts[group] = empty_layout_;
            p.owns_layout[group] = false;
            return;
        }
        WGPUBindGroupLayoutDescriptor ld = WGPU_BIND_GROUP_LAYOUT_DESCRIPTOR_INIT;
        ld.entryCount = count;
        ld.entries = entries;
        p.layouts[group] = wgpuDeviceCreateBindGroupLayout(device_, &ld);
        p.owns_layout[group] = true;
    };
    make(kGroupTextures, n);

    // Group 2: storage buffers (read-only in graphics stages).
    n = 0;
    for (u32 b = 0; b < kMaxStorageBindings; ++b) {
        WGPUShaderStage vis = WGPUShaderStage_None;
        bool rw = false;
        for (u32 i = 0; i < 2; ++i) {
            if (!stages[i] || !(stages[i]->storage_mask & (1u << b))) continue;
            vis |= stage_bits[i];
            rw = rw || (stages[i]->storage_rw_mask & (1u << b));
        }
        if (vis == WGPUShaderStage_None) continue;
        WGPUBindGroupLayoutEntry& e = entries[n++];
        e = WGPU_BIND_GROUP_LAYOUT_ENTRY_INIT;
        e.binding = b;
        e.visibility = vis;
        e.buffer.type = rw ? WGPUBufferBindingType_Storage : WGPUBufferBindingType_ReadOnlyStorage;
    }
    make(kGroupStorage, n);

    // Group 3: storage images (compute). Their format comes from the bound texture at dispatch,
    // so the layout is made lazily there; graphics pipelines have none.
    p.layouts[kGroupImages] = empty_layout_;
    p.owns_layout[kGroupImages] = false;

    p.layouts[kGroupUniforms] = compute ? uniform_layout_compute_ : uniform_layout_;
    p.owns_layout[kGroupUniforms] = false;
    WGPUPipelineLayoutDescriptor pld = WGPU_PIPELINE_LAYOUT_DESCRIPTOR_INIT;
    pld.bindGroupLayoutCount = compute && p.cres.image_mask ? 4 : 3;
    pld.bindGroupLayouts = p.layouts;
    if (compute && p.cres.image_mask) return ok();  // made at the first dispatch (webgpu_translate.cpp)
    p.layout = wgpuDeviceCreatePipelineLayout(device_, &pld);
    return p.layout ? ResultV(ok()) : ResultV(Error(Errc::PipelineCreate, "rhi.webgpu.layout", p.name.c_str()));
}

Result<PipelineHandle> WebGpuDevice::create_pipeline(const GraphicsPipelineDesc& desc) {
    GpuPipeline p;
    p.name.append(desc.debug_name ? desc.debug_name : "");
    FixedString<1024> log;
    Array<char> vwgsl, fwgsl;
    if (auto r = compile_glsl(ShaderStage::Vertex, desc.vertex_shader.glsl, desc.vertex_shader.debug_name, vwgsl, p.vres, log);
        !r) {
        MCE_LOG_ERROR("rhi.webgpu: vertex shader '{}' failed:\n{}", desc.vertex_shader.debug_name, log.c_str());
        return Error(Errc::ShaderCompile, "rhi.pipeline.vertex", desc.vertex_shader.debug_name);
    }
    if (auto r = compile_glsl(ShaderStage::Fragment, desc.fragment_shader.glsl, desc.fragment_shader.debug_name, fwgsl,
                              p.fres, log);
        !r) {
        MCE_LOG_ERROR("rhi.webgpu: fragment shader '{}' failed:\n{}", desc.fragment_shader.debug_name, log.c_str());
        return Error(Errc::ShaderCompile, "rhi.pipeline.fragment", desc.fragment_shader.debug_name);
    }
    auto make_module = [&](const Array<char>& wgsl, const char* name) {
        WGPUShaderSourceWGSL src = WGPU_SHADER_SOURCE_WGSL_INIT;
        src.code = WGPUStringView{wgsl.data(), wgsl.size() - 1};
        WGPUShaderModuleDescriptor md = WGPU_SHADER_MODULE_DESCRIPTOR_INIT;
        md.nextInChain = &src.chain;
        md.label = sv(name);
        return wgpuDeviceCreateShaderModule(device_, &md);
    };
    p.vs = make_module(vwgsl, desc.vertex_shader.debug_name);
    p.fs = make_module(fwgsl, desc.fragment_shader.debug_name);
    if (!p.vs || !p.fs) {
        release_pipeline(p);
        return Error(Errc::PipelineCreate, "rhi.pipeline.link", desc.debug_name);
    }
    if (auto r = make_layouts(p, false); !r) {
        MCE_LOG_ERROR("rhi.webgpu: pipeline '{}': {} {}", desc.debug_name, r.error().stage, r.error().path.c_str());
        release_pipeline(p);
        return r.error();
    }
    MCE_ASSERT_MSG(desc.vertex.attrs.size() <= 16, "create_pipeline: more than 16 vertex attributes");
    p.attr_count = static_cast<u32>(math::min<usize>(desc.vertex.attrs.size(), 16));
    for (u32 i = 0; i < p.attr_count; ++i) p.attrs[i] = desc.vertex.attrs[i];
    p.stride = desc.vertex.stride;
    p.state = desc.state;
    return pipelines_.insert(std::move(p));
}

Result<PipelineHandle> WebGpuDevice::create_compute_pipeline(const ComputePipelineDesc& desc) {
    GpuPipeline p;
    p.name.append(desc.debug_name ? desc.debug_name : "");
    FixedString<1024> log;
    Array<char> wgsl;
    if (auto r = compile_glsl(ShaderStage::Compute, desc.compute_shader.glsl, desc.compute_shader.debug_name, wgsl, p.cres,
                              log);
        !r) {
        MCE_LOG_ERROR("rhi.webgpu: compute shader '{}' failed:\n{}", desc.compute_shader.debug_name, log.c_str());
        return Error(Errc::ShaderCompile, "rhi.pipeline.compute", desc.compute_shader.debug_name);
    }
    WGPUShaderSourceWGSL src = WGPU_SHADER_SOURCE_WGSL_INIT;
    src.code = WGPUStringView{wgsl.data(), wgsl.size() - 1};
    WGPUShaderModuleDescriptor md = WGPU_SHADER_MODULE_DESCRIPTOR_INIT;
    md.nextInChain = &src.chain;
    md.label = sv(desc.compute_shader.debug_name);
    p.vs = wgpuDeviceCreateShaderModule(device_, &md);  // kept until the pipeline exists (storage images)
    if (!p.vs) return Error(Errc::PipelineCreate, "rhi.pipeline.compute", desc.debug_name);
    if (auto r = make_layouts(p, true); !r) {
        release_pipeline(p);
        return r.error();
    }
    if (p.layout) {  // no storage images: the pipeline can be made now
        WGPUComputePipelineDescriptor cd = WGPU_COMPUTE_PIPELINE_DESCRIPTOR_INIT;
        cd.label = sv(desc.debug_name);
        cd.layout = p.layout;
        cd.compute.module = p.vs;
        cd.compute.entryPoint = sv("main");
        p.cs = wgpuDeviceCreateComputePipeline(device_, &cd);
        if (!p.cs) {
            release_pipeline(p);
            return Error(Errc::PipelineCreate, "rhi.pipeline.compute", desc.debug_name);
        }
    }
    return pipelines_.insert(std::move(p));
}

void WebGpuDevice::destroy_buffer(BufferHandle h) {
    if (const GpuBuffer* b = buffers_.try_get(h)) {
        for (BoundStorage& s : storage_bound_)
            if (s.buffer == b->buffer) s = BoundStorage{};
        ++storage_gen_;
        wgpuBufferRelease(b->buffer);  // WebGPU keeps it alive while a submitted command buffer uses it
        buffers_.erase(h);
    }
}

void WebGpuDevice::destroy_texture(TextureHandle h) {
    GpuTexture* t = textures_.try_get(h);
    if (!t) return;
    texture_bytes_ -= t->bytes;
    for (WGPUTextureView v : t->targets) wgpuTextureViewRelease(v);
    for (WGPUTextureView v : t->mip_views) wgpuTextureViewRelease(v);
    if (t->view) wgpuTextureViewRelease(t->view);
    wgpuTextureRelease(t->texture);
    textures_.erase(h);
    ++texture_gen_;
    ++image_gen_;
}

void WebGpuDevice::destroy_sampler(SamplerHandle h) {
    if (const GpuSampler* s = samplers_.try_get(h)) {
        wgpuSamplerRelease(s->sampler);
        samplers_.erase(h);
        ++texture_gen_;
    }
}

void WebGpuDevice::destroy_pipeline(PipelineHandle h) {
    if (GpuPipeline* p = pipelines_.try_get(h)) {
        if (ts_.pipeline == p) ts_.pipeline = nullptr;
        release_pipeline(*p);
        pipelines_.erase(h);
    }
}

Extent2D WebGpuDevice::texture_size(TextureHandle h) const {
    const GpuTexture* t = textures_.try_get(h);
    return t ? t->size : Extent2D{};
}

TextureDesc WebGpuDevice::texture_desc(TextureHandle h) const {
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

Ring* WebGpuDevice::storage_ring(u32 binding) {
    Ring& r = storage_[binding];
    if (!r.buffer) {
        char name[32];
        std::snprintf(name, sizeof(name), "mce.ring.storage.%u", binding);
        r.capacity = align_up(desc_.geometry_ring_bytes, 256);
        r.buffer = make_buffer(device_, r.capacity, WGPUBufferUsage_Storage | WGPUBufferUsage_CopyDst, name);
        if (!r.buffer) return nullptr;
        r.staging.resize(r.capacity);
    }
    return &r;
}

void WebGpuDevice::begin_frame(u64 frame_index) {
    MCE_ASSERT_MSG(!in_frame_, "begin_frame twice");
    frame_index_ = frame_index;
    for (Ring* r : {&uniforms_, &geometry_}) r->head = r->flushed = 0;
    for (Ring& r : storage_) r.head = r.flushed = 0;
    draw_args_.clear();
    // Bank data persists across frames (GL semantics); the ring does not, so it is re-staged lazily.
    for (u32 b = 0; b < kMaxUniformBanks; ++b) uniform_offset_[b] = ~0u;
    // Transient storage bindings point into last frame's ring bytes: they end with the frame.
    for (u32 b = 0; b < kMaxStorageBindings; ++b)
        if (storage_[b].buffer && storage_bound_[b].buffer == storage_[b].buffer) storage_bound_[b] = BoundStorage{};
    ++storage_gen_;
    ensure_backbuffer();
    stats_ = FrameStats{};
    in_frame_ = true;
}

void WebGpuDevice::submit(const CommandBuffer& cb) {
    MCE_ASSERT_MSG(in_frame_, "submit outside begin_frame/end_frame");
    MCE_ASSERT_MSG(!cb.in_pass(), "submit with an open pass");
    stage_payloads(cb);
    WGPUCommandEncoderDescriptor ed = WGPU_COMMAND_ENCODER_DESCRIPTOR_INIT;
    WGPUCommandEncoder enc = wgpuDeviceCreateCommandEncoder(device_, &ed);
    encode(cb, enc);
    flush_rings();  // every write lands on the queue before the command buffer that reads it
    WGPUCommandBuffer commands = wgpuCommandEncoderFinish(enc, nullptr);
    wgpuQueueSubmit(queue_, 1, &commands);
    wgpuCommandBufferRelease(commands);
    wgpuCommandEncoderRelease(enc);
    for (WGPUBindGroup g : retired_groups_) wgpuBindGroupRelease(g);
    retired_groups_.clear();
}

void WebGpuDevice::end_frame() {
    MCE_ASSERT_MSG(in_frame_, "end_frame without begin_frame");
    stats_.geometry_bytes = geometry_.head;
    for (const Ring& r : storage_) stats_.geometry_bytes += r.head;
    stats_.uniform_bytes = uniforms_.head;
    last_stats_ = stats_;
    in_frame_ = false;
}

void WebGpuDevice::present(bool vsync) {
    vsync_ = vsync;  // the browser paces frames (requestAnimationFrame); there is no tearing mode
    if (!backbuffer_) return;
    configure_surface(backbuffer_extent_);
    WGPUSurfaceTexture st = WGPU_SURFACE_TEXTURE_INIT;
    wgpuSurfaceGetCurrentTexture(surface_, &st);
    if (st.status != WGPUSurfaceGetCurrentTextureStatus_SuccessOptimal &&
        st.status != WGPUSurfaceGetCurrentTextureStatus_SuccessSuboptimal) {
        if (st.texture) wgpuTextureRelease(st.texture);
        surface_extent_ = Extent2D{};  // reconfigure next frame
        return;
    }
    WGPUTextureView view = wgpuTextureCreateView(st.texture, nullptr);
    WGPUCommandEncoder enc = wgpuDeviceCreateCommandEncoder(device_, nullptr);
    WGPURenderPassColorAttachment color = WGPU_RENDER_PASS_COLOR_ATTACHMENT_INIT;
    color.view = view;
    color.loadOp = WGPULoadOp_Clear;
    color.storeOp = WGPUStoreOp_Store;
    color.clearValue = WGPUColor{0, 0, 0, 1};
    WGPURenderPassDescriptor rp = WGPU_RENDER_PASS_DESCRIPTOR_INIT;
    rp.colorAttachmentCount = 1;
    rp.colorAttachments = &color;
    WGPURenderPassEncoder pass = wgpuCommandEncoderBeginRenderPass(enc, &rp);
    wgpuRenderPassEncoderSetPipeline(pass, blit_pipeline_);
    wgpuRenderPassEncoderSetBindGroup(pass, 0, blit_group_, 0, nullptr);
    wgpuRenderPassEncoderDraw(pass, 3, 1, 0, 0);
    wgpuRenderPassEncoderEnd(pass);
    wgpuRenderPassEncoderRelease(pass);
    WGPUCommandBuffer commands = wgpuCommandEncoderFinish(enc, nullptr);
    wgpuQueueSubmit(queue_, 1, &commands);
    wgpuCommandBufferRelease(commands);
    wgpuCommandEncoderRelease(enc);
    wgpuTextureViewRelease(view);
    wgpuTextureRelease(st.texture);
    // The browser shows the canvas when this task yields (app::run awaits the next animation frame).
}

Extent2D WebGpuDevice::backbuffer_size() const {
    const platform::PixelSize s = window_.framebuffer_size();
    return Extent2D{s.width, s.height};
}

// Maps a MapRead buffer and copies its first `size` bytes out (waits for the GPU).
ResultV WebGpuDevice::map_and_copy(WGPUBuffer readback, usize size, Span<byte> out) {
    struct MapOut {
        bool ok = false;
    } m;
    WGPUBufferMapCallbackInfo cb = WGPU_BUFFER_MAP_CALLBACK_INFO_INIT;
    cb.mode = WGPUCallbackMode_WaitAnyOnly;
    cb.callback = [](WGPUMapAsyncStatus status, WGPUStringView, void* u, void*) {
        static_cast<MapOut*>(u)->ok = status == WGPUMapAsyncStatus_Success;
    };
    cb.userdata1 = &m;
    if (!wait(wgpuBufferMapAsync(readback, WGPUMapMode_Read, 0, align_up(size, 4), cb)) || !m.ok)
        return Error(Errc::Unsupported, "rhi.webgpu.readback", "mapAsync failed");
    const void* bytes = wgpuBufferGetConstMappedRange(readback, 0, align_up(size, 4));
    if (!bytes) {
        wgpuBufferUnmap(readback);
        return Error(Errc::Unsupported, "rhi.webgpu.readback", "no mapped range");
    }
    std::memcpy(out.data(), bytes, math::min(size, out.size()));
    wgpuBufferUnmap(readback);
    return ok();
}

ResultV WebGpuDevice::read_buffer(BufferHandle h, u64 offset, Span<byte> out) {
    const GpuBuffer* b = buffers_.try_get(h);
    if (!b) return Error(Errc::NotFound, "rhi.read_buffer", "stale buffer handle");
    if (offset + out.size() > b->size) return Error(Errc::InvalidArgument, "rhi.read_buffer", "range outside buffer");
    if (out.empty()) return ok();
    const u64 start = offset & ~u64(3);
    const usize span = align_up(usize(offset - start) + out.size(), 4);
    if (start + span > align_up(b->size, 4)) return Error(Errc::InvalidArgument, "rhi.read_buffer", "range outside buffer");
    WGPUBuffer rb = make_buffer(device_, span, WGPUBufferUsage_MapRead | WGPUBufferUsage_CopyDst, "mce.readback");
    WGPUCommandEncoder enc = wgpuDeviceCreateCommandEncoder(device_, nullptr);
    wgpuCommandEncoderCopyBufferToBuffer(enc, b->buffer, start, rb, 0, span);
    WGPUCommandBuffer commands = wgpuCommandEncoderFinish(enc, nullptr);
    wgpuQueueSubmit(queue_, 1, &commands);
    wgpuCommandBufferRelease(commands);
    wgpuCommandEncoderRelease(enc);
    Array<byte> tmp;
    tmp.resize(span);
    const ResultV r = map_and_copy(rb, span, tmp.span());
    wgpuBufferRelease(rb);
    if (!r) return r;
    std::memcpy(out.data(), tmp.data() + (offset - start), out.size());
    return ok();
}

ResultV WebGpuDevice::read_pixels(TextureHandle target, Rect2D rect, Span<u8> out) {
    const usize row = usize(rect.width) * 4;
    if (out.size() < row * rect.height) return Error(Errc::InvalidArgument, "rhi.read_pixels", "output too small");
    WGPUTexture tex = nullptr;
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
    const u32 src_row = static_cast<u32>(align_up(usize(rect.width) * bpp, 256));  // copy rows are 256-aligned
    WGPUBuffer rb = make_buffer(device_, usize(src_row) * rect.height, WGPUBufferUsage_MapRead | WGPUBufferUsage_CopyDst,
                                "mce.readback");
    WGPUCommandEncoder enc = wgpuDeviceCreateCommandEncoder(device_, nullptr);
    WGPUTexelCopyTextureInfo src = WGPU_TEXEL_COPY_TEXTURE_INFO_INIT;
    src.texture = tex;
    src.origin = WGPUOrigin3D{u32(rect.x), src_y, 0};
    WGPUTexelCopyBufferInfo dst = WGPU_TEXEL_COPY_BUFFER_INFO_INIT;
    dst.buffer = rb;
    dst.layout.bytesPerRow = src_row;
    dst.layout.rowsPerImage = rect.height;
    const WGPUExtent3D extent{rect.width, rect.height, 1};
    wgpuCommandEncoderCopyTextureToBuffer(enc, &src, &dst, &extent);
    WGPUCommandBuffer commands = wgpuCommandEncoderFinish(enc, nullptr);
    wgpuQueueSubmit(queue_, 1, &commands);
    wgpuCommandBufferRelease(commands);
    wgpuCommandEncoderRelease(enc);
    Array<byte> staged;
    staged.resize(usize(src_row) * rect.height);
    const ResultV r = map_and_copy(rb, staged.size(), staged.span());
    wgpuBufferRelease(rb);
    if (!r) return r;

    const u8* bytes = reinterpret_cast<const u8*>(staged.data());
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
    return ok();
}

}  // namespace mce::rhi::webgpu
