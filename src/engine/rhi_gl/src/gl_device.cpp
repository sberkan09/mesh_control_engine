// mce_rhi_gl: context, resources, staging rings, fences, deferred destruction.
// rhi-renderer.md §3.0-§3.1: DSA creation only (glCreate*), persistent-mapped coherent rings,
// one glFenceSync per frame slot.

#include "gl_device.h"

#include <SDL3/SDL.h>

#include <cstring>

#include "mce/log.h"
#include "mce/math/math.h"

namespace mce::rhi::gl {
namespace {

// S3TC enums are not in the 4.5 core headers (EXT_texture_compression_s3tc, universal on desktop).
constexpr GLenum kCompressedRgbaS3tcDxt1 = 0x83F1;
constexpr GLenum kCompressedRgbaS3tcDxt5 = 0x83F3;
constexpr GLenum kCompressedSrgbAlphaS3tcDxt1 = 0x8C4D;  // GL_COMPRESSED_SRGB_ALPHA_S3TC_DXT1_EXT
// GL_TEXTURE_MAX_ANISOTROPY: core in 4.6, ARB/EXT_texture_filter_anisotropic before (same value).
constexpr GLenum kTextureMaxAnisotropy = 0x84FE;

struct GlFormat {
    GLenum internal;
    GLenum format;  // upload format (0 for compressed / depth)
    GLenum type;
};

GlFormat gl_format(TextureFormat f) {
    switch (f) {
        case TextureFormat::RGBA8_UNorm: return {GL_RGBA8, GL_RGBA, GL_UNSIGNED_BYTE};
        case TextureFormat::RGBA8_SRGB: return {GL_SRGB8_ALPHA8, GL_RGBA, GL_UNSIGNED_BYTE};
        case TextureFormat::BGRA8_UNorm: return {GL_RGBA8, GL_BGRA, GL_UNSIGNED_BYTE};
        case TextureFormat::RGBA16F: return {GL_RGBA16F, GL_RGBA, GL_HALF_FLOAT};
        case TextureFormat::RGBA32F: return {GL_RGBA32F, GL_RGBA, GL_FLOAT};
        case TextureFormat::R8_UNorm: return {GL_R8, GL_RED, GL_UNSIGNED_BYTE};
        case TextureFormat::RG8_UNorm: return {GL_RG8, GL_RG, GL_UNSIGNED_BYTE};
        case TextureFormat::R11G11B10F: return {GL_R11F_G11F_B10F, GL_RGB, GL_UNSIGNED_INT_10F_11F_11F_REV};
        case TextureFormat::BC1_UNorm: return {kCompressedRgbaS3tcDxt1, 0, 0};
        case TextureFormat::BC3_UNorm: return {kCompressedRgbaS3tcDxt5, 0, 0};
        case TextureFormat::BC4_UNorm: return {GL_COMPRESSED_RED_RGTC1, 0, 0};
        case TextureFormat::BC1_SRGB: return {kCompressedSrgbAlphaS3tcDxt1, 0, 0};
        case TextureFormat::BC5_UNorm: return {GL_COMPRESSED_RG_RGTC2, 0, 0};
        case TextureFormat::BC7_UNorm: return {GL_COMPRESSED_RGBA_BPTC_UNORM, 0, 0};
        case TextureFormat::BC7_SRGB: return {GL_COMPRESSED_SRGB_ALPHA_BPTC_UNORM, 0, 0};
        case TextureFormat::D32_Float: return {GL_DEPTH_COMPONENT32F, 0, 0};
        case TextureFormat::D24_UNorm_S8: return {GL_DEPTH24_STENCIL8, 0, 0};
        case TextureFormat::D16_UNorm: return {GL_DEPTH_COMPONENT16, 0, 0};
        // KHR_texture_compression_astc_ldr: phone packs only; desktop GL drivers rarely take them.
        case TextureFormat::ASTC_4x4_UNorm: return {0x93B0, 0, 0};  // GL_COMPRESSED_RGBA_ASTC_4x4_KHR
        case TextureFormat::ASTC_4x4_SRGB: return {0x93D0, 0, 0};   // GL_COMPRESSED_SRGB8_ALPHA8_ASTC_4x4_KHR
    }
    return {GL_RGBA8, GL_RGBA, GL_UNSIGNED_BYTE};
}

// True when the driver lists `name` among its extensions (core profile: glGetStringi).
bool has_gl_extension(const char* name) {
    GLint n = 0;
    glGetIntegerv(GL_NUM_EXTENSIONS, &n);
    for (GLint i = 0; i < n; ++i) {
        const auto* e = reinterpret_cast<const char*>(glGetStringi(GL_EXTENSIONS, static_cast<GLuint>(i)));
        if (e && std::strcmp(e, name) == 0) return true;
    }
    return false;
}

GLenum gl_compare_func(CompareOp op) {
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

GLenum gl_filter(Filter f, MipFilter mip) {
    if (f == Filter::Nearest) {
        switch (mip) {
            case MipFilter::None: return GL_NEAREST;
            case MipFilter::Nearest: return GL_NEAREST_MIPMAP_NEAREST;
            case MipFilter::Linear: return GL_NEAREST_MIPMAP_LINEAR;
        }
    }
    switch (mip) {
        case MipFilter::None: return GL_LINEAR;
        case MipFilter::Nearest: return GL_LINEAR_MIPMAP_NEAREST;
        case MipFilter::Linear: return GL_LINEAR_MIPMAP_LINEAR;
    }
    return GL_LINEAR;
}

GLenum gl_wrap(AddressMode m) {
    switch (m) {
        case AddressMode::Repeat: return GL_REPEAT;
        case AddressMode::ClampToEdge: return GL_CLAMP_TO_EDGE;
        case AddressMode::MirroredRepeat: return GL_MIRRORED_REPEAT;
    }
    return GL_CLAMP_TO_EDGE;
}

void label(GLenum kind, GLuint id, const char* name) {
    if (name && *name) glObjectLabel(kind, id, -1, name);
}

void GLAD_API_PTR debug_callback(GLenum /*source*/, GLenum type, GLuint /*id*/, GLenum severity, GLsizei /*length*/,
                             const GLchar* message, const void* user) {
    const_cast<GlDevice*>(static_cast<const GlDevice*>(user))->on_debug_message(type, severity, message);
}

// Compiles one separable program; on failure returns the info log in `log`.
GLuint compile_stage(GLenum stage, const ShaderSource& src, FixedString<1024>& log) {
    Array<char> text;
    text.resize(src.glsl.size() + 1);
    std::memcpy(text.data(), src.glsl.data(), src.glsl.size());
    text[src.glsl.size()] = '\0';
    const char* ptr = text.data();
    const GLuint prog = glCreateShaderProgramv(stage, 1, &ptr);
    GLint linked = GL_FALSE;
    glGetProgramiv(prog, GL_LINK_STATUS, &linked);
    if (!linked) {
        char buf[1024] = {};
        glGetProgramInfoLog(prog, sizeof(buf), nullptr, buf);
        log.append(buf);
        glDeleteProgram(prog);
        return 0;
    }
    label(GL_PROGRAM, prog, src.debug_name);
    return prog;
}

}  // namespace

// ---- staging ring / fences -------------------------------------------------------------------

usize StagingRing::alloc(usize size, usize align) {
    const usize start = align_up(head, align);
    if (start + size > per_slot) return kOverflow;
    head = start + size;
    return usize(slot) * per_slot + start;
}

bool FramebufferKey::operator==(const FramebufferKey& o) const {
    for (u32 i = 0; i < cmd::kMaxColorAttachments; ++i)
        if (!(colors[i] == o.colors[i]) || color_layers[i] != o.color_layers[i]) return false;
    return depth == o.depth && depth_layer == o.depth_layer;
}

bool GlFence::is_signaled() { return device->slot_signaled(slot); }
void GlFence::wait_blocking() { device->wait_slot(slot); }

// ---- creation --------------------------------------------------------------------------------

Result<mem::UniquePtr<RenderDevice>> create_device(platform::Window& window, const DeviceDesc& desc) {
    GlDevice* dev = mem::make<GlDevice>(mem::heap(), window, desc);
    mem::UniquePtr<RenderDevice> owned(mem::heap(), dev);
    MCE_TRY(dev->init());
    return owned;
}

GlDevice::GlDevice(platform::Window& window, const DeviceDesc& desc) : window_(window), desc_(desc) {
    for (u32 i = 0; i < MCE_FRAMES_IN_FLIGHT; ++i) {
        fence_objs_[i].device = this;
        fence_objs_[i].slot = i;
    }
}

ResultV GlDevice::init() {
    auto* sdl_window = static_cast<SDL_Window*>(window_.native_handle());
    SDL_GL_SetAttribute(SDL_GL_CONTEXT_MAJOR_VERSION, 4);
    SDL_GL_SetAttribute(SDL_GL_CONTEXT_MINOR_VERSION, 5);
    SDL_GL_SetAttribute(SDL_GL_CONTEXT_PROFILE_MASK, SDL_GL_CONTEXT_PROFILE_CORE);
    SDL_GL_SetAttribute(SDL_GL_CONTEXT_FLAGS, desc_.debug_layer ? SDL_GL_CONTEXT_DEBUG_FLAG : 0);

    SDL_GLContext ctx = SDL_GL_CreateContext(sdl_window);
    if (!ctx) {
        MCE_LOG_ERROR("rhi.gl: OpenGL 4.5 core context creation failed: {}", SDL_GetError());
        return Error(Errc::Unsupported, "rhi.gl.context", SDL_GetError());
    }
    context_ = ctx;
    SDL_GL_MakeCurrent(sdl_window, ctx);

    const int version = gladLoadGL(reinterpret_cast<GLADloadfunc>(SDL_GL_GetProcAddress));
    if (version == 0) return Error(Errc::Unsupported, "rhi.gl.load", "gladLoadGL failed");
    caps_.api_major = static_cast<u32>(GLAD_VERSION_MAJOR(version));
    caps_.api_minor = static_cast<u32>(GLAD_VERSION_MINOR(version));
    if (caps_.api_major < 4 || (caps_.api_major == 4 && caps_.api_minor < 5)) {
        MCE_LOG_ERROR("rhi.gl: OpenGL 4.5 required, driver offers {}.{}", caps_.api_major, caps_.api_minor);
        return Error(Errc::Unsupported, "rhi.gl.version", "OpenGL 4.5 core required",
                     caps_.api_major * 10 + caps_.api_minor);
    }
    gl_loaded_ = true;

    caps_.api_name.append("OpenGL ");
    caps_.api_name.append(reinterpret_cast<const char*>(glGetString(GL_VERSION)));
    caps_.device_name.append(reinterpret_cast<const char*>(glGetString(GL_RENDERER)));
    GLint v = 0;
    glGetIntegerv(GL_MAX_TEXTURE_SIZE, &v);
    caps_.max_texture_size = static_cast<u32>(v);
    glGetIntegerv(GL_MAX_UNIFORM_BLOCK_SIZE, &v);
    caps_.max_uniform_block_size = static_cast<u32>(v);
    glGetIntegerv(GL_UNIFORM_BUFFER_OFFSET_ALIGNMENT, &v);
    if (v > 0 && usize(v) > uniform_align_) uniform_align_ = usize(v);
    caps_.shader_draw_parameters = GLAD_GL_ARB_shader_draw_parameters != 0;
    glGetIntegerv(GL_SHADER_STORAGE_BUFFER_OFFSET_ALIGNMENT, &v);
    if (v > 0) storage_align_ = usize(v);
    caps_.storage_buffer_alignment = static_cast<u32>(storage_align_);
    GLint timer_bits = 0;
    glGetQueryiv(GL_TIMESTAMP, GL_QUERY_COUNTER_BITS, &timer_bits);
    caps_.timestamp_queries = timer_bits > 0;
    // Compute is core in GL 4.3, so every GL 4.5 context has it (M-VISUAL 1).
    caps_.compute = true;
    glGetIntegerv(GL_MAX_COMPUTE_WORK_GROUP_INVOCATIONS, &v);
    caps_.max_compute_invocations = static_cast<u32>(v);
    for (GLuint i = 0; i < 3; ++i) {
        glGetIntegeri_v(GL_MAX_COMPUTE_WORK_GROUP_SIZE, i, &v);
        caps_.max_compute_group_size[i] = static_cast<u32>(v);
        glGetIntegeri_v(GL_MAX_COMPUTE_WORK_GROUP_COUNT, i, &v);
        caps_.max_compute_group_count[i] = static_cast<u32>(v);
    }
    glGetIntegerv(GL_MAX_IMAGE_UNITS, &v);
    caps_.max_storage_image_units = static_cast<u32>(v);
    // GL 4.5 is required, so BC4/BC5 (RGTC) and BC7 (BPTC, sRGB included) are core; BC1 comes with S3TC, which every
    // desktop driver exposes. BC1_SRGB needs the sRGB flavour of S3TC (GL_EXT_texture_sRGB or
    // GL_EXT_texture_compression_s3tc_srgb), so it has its own bit; texture_bc_srgb false makes the texture store refuse it.
    caps_.texture_bc = has_gl_extension("GL_EXT_texture_compression_s3tc");
    caps_.texture_bc_srgb = caps_.texture_bc && (has_gl_extension("GL_EXT_texture_sRGB") || has_gl_extension("GL_EXT_texture_compression_s3tc_srgb"));
    caps_.texture_astc = has_gl_extension("GL_KHR_texture_compression_astc_ldr");
    MCE_LOG_INFO("rhi.gl: {} on {}", caps_.api_name.c_str(), caps_.device_name.c_str());

    if (desc_.debug_layer) {
        glEnable(GL_DEBUG_OUTPUT);
        glEnable(GL_DEBUG_OUTPUT_SYNCHRONOUS);
        glDebugMessageCallback(debug_callback, this);
        glDebugMessageControl(GL_DONT_CARE, GL_DONT_CARE, GL_DEBUG_SEVERITY_NOTIFICATION, 0, nullptr, GL_FALSE);
    }

    glPixelStorei(GL_UNPACK_ALIGNMENT, 1);
    glPixelStorei(GL_PACK_ALIGNMENT, 1);
    glEnable(GL_PROGRAM_POINT_SIZE);

    MCE_TRY(create_ring(geometry_ring_, desc_.geometry_ring_bytes, "mce.ring.geometry"));
    MCE_TRY(create_ring(uniform_ring_, desc_.uniform_ring_bytes, "mce.ring.uniform"));
    MCE_TRY(create_ring(indirect_ring_, desc_.indirect_ring_bytes, "mce.ring.indirect"));
    if (caps_.timestamp_queries)
        for (u32 s = 0; s < MCE_FRAMES_IN_FLIGHT; ++s) glCreateQueries(GL_TIMESTAMP, kMaxTimestamps, timestamp_queries_[s]);

    swap_interval_ = desc_.vsync ? 1 : 0;
    SDL_GL_SetSwapInterval(swap_interval_);
    return ok();
}

ResultV GlDevice::create_ring(StagingRing& ring, usize per_slot, const char* name) {
    ring.per_slot = align_up(per_slot, 256);
    const usize total = ring.per_slot * MCE_FRAMES_IN_FLIGHT;
    const GLbitfield flags = GL_MAP_WRITE_BIT | GL_MAP_PERSISTENT_BIT | GL_MAP_COHERENT_BIT;
    glCreateBuffers(1, &ring.buffer);
    glNamedBufferStorage(ring.buffer, static_cast<GLsizeiptr>(total), nullptr, flags);
    ring.mapped = static_cast<u8*>(glMapNamedBufferRange(ring.buffer, 0, static_cast<GLsizeiptr>(total), flags));
    label(GL_BUFFER, ring.buffer, name);
    if (!ring.mapped) return Error(Errc::OutOfMemory, "rhi.gl.ring", name);
    return ok();
}

void GlDevice::destroy_ring(StagingRing& ring) {
    if (!ring.buffer) return;
    glUnmapNamedBuffer(ring.buffer);
    glDeleteBuffers(1, &ring.buffer);
    ring = StagingRing{};
}

GlDevice::~GlDevice() {
    if (!context_) return;
    if (gl_loaded_) release_gl_objects();
    SDL_GL_DestroyContext(static_cast<SDL_GLContext>(context_));
}

void GlDevice::release_gl_objects() {
    glFinish();
    for (u32 s = 0; s < MCE_FRAMES_IN_FLIGHT; ++s) {
        flush_deletes(s);
        if (fences_[s]) glDeleteSync(fences_[s]);
    }
    for (const GlBuffer& b : buffers_.dense()) glDeleteBuffers(1, &b.id);
    for (const GlTexture& t : textures_.dense()) glDeleteTextures(1, &t.id);
    for (const GlSampler& s : samplers_.dense()) glDeleteSamplers(1, &s.id);
    for (const GlPipeline& p : pipelines_.dense()) {
        glDeleteProgramPipelines(1, &p.pipeline);
        glDeleteProgram(p.vs);
        glDeleteProgram(p.fs);
        glDeleteProgram(p.cs);
        glDeleteVertexArrays(1, &p.vao);
    }
    for (const FramebufferEntry& f : framebuffers_) glDeleteFramebuffers(1, &f.fbo);
    if (caps_.timestamp_queries)
        for (u32 s = 0; s < MCE_FRAMES_IN_FLIGHT; ++s) glDeleteQueries(kMaxTimestamps, timestamp_queries_[s]);
    destroy_ring(geometry_ring_);
    destroy_ring(uniform_ring_);
    destroy_ring(indirect_ring_);
}

// ---- debug -----------------------------------------------------------------------------------

void GlDevice::on_debug_message(GLenum type, GLenum severity, const char* message) {
    if (type == GL_DEBUG_TYPE_ERROR || severity == GL_DEBUG_SEVERITY_HIGH) {
        ++validation_errors_;
        MCE_LOG_ERROR("gl: {}", message);
    } else if (severity == GL_DEBUG_SEVERITY_MEDIUM) {
        MCE_LOG_WARN("gl: {}", message);
    } else {
        MCE_LOG_DEBUG("gl: {}", message);
    }
}

// ---- resources -------------------------------------------------------------------------------

BufferHandle GlDevice::create_buffer(const BufferDesc& desc, Span<const byte> initial_data) {
    MCE_ASSERT_MSG(desc.size_bytes > 0, "create_buffer: size 0");
    MCE_ASSERT_MSG(initial_data.empty() || initial_data.size() <= desc.size_bytes, "initial data larger than buffer");
    GlBuffer b;
    b.size = desc.size_bytes;
    b.usage = desc.usage;
    glCreateBuffers(1, &b.id);
    // Allocate full size, then fill: initial_data may be shorter than the buffer.
    glNamedBufferStorage(b.id, static_cast<GLsizeiptr>(desc.size_bytes), nullptr, GL_DYNAMIC_STORAGE_BIT);
    if (!initial_data.empty())
        glNamedBufferSubData(b.id, 0, static_cast<GLsizeiptr>(initial_data.size()), initial_data.data());
    label(GL_BUFFER, b.id, desc.debug_name);
    return buffers_.insert(b);
}

void GlDevice::update_buffer(BufferHandle h, u64 offset, Span<const byte> data) {
    const GlBuffer* b = buffers_.try_get(h);
    MCE_ASSERT_MSG(b, "update_buffer: stale handle");
    if (!b) return;
    MCE_ASSERT_MSG(offset + data.size() <= b->size, "update_buffer: out of range");
    if (offset + data.size() > b->size) return;
    glNamedBufferSubData(b->id, static_cast<GLintptr>(offset), static_cast<GLsizeiptr>(data.size()), data.data());
}

TextureHandle GlDevice::create_texture(const TextureDesc& desc, Span<const byte> initial_data) {
    MCE_ASSERT_MSG(desc.size.width > 0 && desc.size.height > 0, "create_texture: empty extent");
    const bool array = desc.dimension == TextureDimension::Tex2DArray;
    MCE_ASSERT_MSG(array || desc.array_layers <= 1, "create_texture: layers > 1 need TextureDimension::Tex2DArray");
    const GlFormat fmt = gl_format(desc.format);
    MCE_ASSERT_MSG(desc.usage != TextureUsage::SampledStorage ||
                       (desc.samples <= 1 && !is_depth_format(desc.format) && !is_compressed_format(desc.format) &&
                        desc.format != TextureFormat::RGBA8_SRGB && desc.format != TextureFormat::BGRA8_UNorm),
                   "create_texture: storage images need a single-sample, linear, uncompressed color format");
    GlTexture t;
    t.internal = fmt.internal;
    t.size = desc.size;
    t.format = desc.format;
    t.dimension = desc.dimension;
    t.usage = desc.usage;
    t.mips = desc.mip_levels ? desc.mip_levels : 1;
    t.layers = array ? (desc.array_layers ? desc.array_layers : 1) : 1;
    const auto w = static_cast<GLsizei>(desc.size.width);
    const auto hgt = static_cast<GLsizei>(desc.size.height);
    t.samples = desc.samples > 1 ? desc.samples : 1;
    MCE_ASSERT_MSG(t.samples == 1 || (!array && t.mips == 1 && initial_data.empty()),
                   "create_texture: multisampled textures are single-mip 2D render targets");
    if (t.samples > 1) {
        glCreateTextures(GL_TEXTURE_2D_MULTISAMPLE, 1, &t.id);
        glTextureStorage2DMultisample(t.id, static_cast<GLsizei>(t.samples), fmt.internal, w, hgt, GL_TRUE);
    } else if (array) {
        glCreateTextures(GL_TEXTURE_2D_ARRAY, 1, &t.id);
        glTextureStorage3D(t.id, t.mips, fmt.internal, w, hgt, t.layers);
    } else {
        glCreateTextures(GL_TEXTURE_2D, 1, &t.id);
        glTextureStorage2D(t.id, t.mips, fmt.internal, w, hgt);
    }
    for (u32 m = 0; m < t.mips; ++m)
        t.bytes += level_bytes(desc.format, math::max(1u, desc.size.width >> m), math::max(1u, desc.size.height >> m)) *
                   t.layers;
    t.bytes *= t.samples;
    // Mip levels that are never uploaded must not be sampled: clamp until update_texture fills them.
    if (t.samples == 1) glTextureParameteri(t.id, GL_TEXTURE_MAX_LEVEL, static_cast<GLint>(t.mips - 1));

    if (!initial_data.empty()) {
        const usize need = level_bytes(desc.format, desc.size.width, desc.size.height) * t.layers;
        MCE_ASSERT_MSG(initial_data.size() >= need, "create_texture: initial data smaller than mip 0");
        MCE_ASSERT_MSG(!is_depth_format(desc.format), "create_texture: depth formats take no initial data");
        if (initial_data.size() >= need && !is_depth_format(desc.format)) {
            const auto bytes = static_cast<GLsizei>(need);
            if (array && is_compressed_format(desc.format)) {
                glCompressedTextureSubImage3D(t.id, 0, 0, 0, 0, w, hgt, t.layers, fmt.internal, bytes, initial_data.data());
            } else if (array) {
                glTextureSubImage3D(t.id, 0, 0, 0, 0, w, hgt, t.layers, fmt.format, fmt.type, initial_data.data());
            } else if (is_compressed_format(desc.format)) {
                glCompressedTextureSubImage2D(t.id, 0, 0, 0, w, hgt, fmt.internal, bytes, initial_data.data());
            } else {
                glTextureSubImage2D(t.id, 0, 0, 0, w, hgt, fmt.format, fmt.type, initial_data.data());
            }
        }
    }
    label(GL_TEXTURE, t.id, desc.debug_name);
    texture_bytes_ += t.bytes;
    return textures_.insert(t);
}

void GlDevice::update_texture(TextureHandle h, const TextureRegion& region, Span<const byte> data) {
    const GlTexture* t = textures_.try_get(h);
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
    const GlFormat fmt = gl_format(t->format);
    const auto level = static_cast<GLint>(region.mip);
    const auto x = static_cast<GLint>(region.x), y = static_cast<GLint>(region.y);
    const auto w = static_cast<GLsizei>(rw), hgt = static_cast<GLsizei>(rh);
    if (t->dimension == TextureDimension::Tex2DArray) {
        const auto z = static_cast<GLint>(region.layer);
        if (is_compressed_format(t->format))
            glCompressedTextureSubImage3D(t->id, level, x, y, z, w, hgt, 1, fmt.internal, static_cast<GLsizei>(need),
                                          data.data());
        else
            glTextureSubImage3D(t->id, level, x, y, z, w, hgt, 1, fmt.format, fmt.type, data.data());
    } else {
        if (is_compressed_format(t->format))
            glCompressedTextureSubImage2D(t->id, level, x, y, w, hgt, fmt.internal, static_cast<GLsizei>(need), data.data());
        else
            glTextureSubImage2D(t->id, level, x, y, w, hgt, fmt.format, fmt.type, data.data());
    }
}

SamplerHandle GlDevice::create_sampler(const SamplerDesc& desc) {
    GlSampler s;
    glCreateSamplers(1, &s.id);
    glSamplerParameteri(s.id, GL_TEXTURE_MIN_FILTER, static_cast<GLint>(gl_filter(desc.min_filter, desc.mip)));
    glSamplerParameteri(s.id, GL_TEXTURE_MAG_FILTER,
                        desc.mag_filter == Filter::Nearest ? GL_NEAREST : GL_LINEAR);
    glSamplerParameteri(s.id, GL_TEXTURE_WRAP_S, static_cast<GLint>(gl_wrap(desc.wrap_s)));
    glSamplerParameteri(s.id, GL_TEXTURE_WRAP_T, static_cast<GLint>(gl_wrap(desc.wrap_t)));
    if (desc.max_aniso > 1.0f) glSamplerParameterf(s.id, kTextureMaxAnisotropy, desc.max_aniso);
    if (desc.compare) {
        glSamplerParameteri(s.id, GL_TEXTURE_COMPARE_MODE, GL_COMPARE_REF_TO_TEXTURE);
        glSamplerParameteri(s.id, GL_TEXTURE_COMPARE_FUNC, static_cast<GLint>(gl_compare_func(desc.compare_op)));
    }
    return samplers_.insert(s);
}

Result<PipelineHandle> GlDevice::create_pipeline(const GraphicsPipelineDesc& desc) {
    FixedString<1024> log;
    const GLuint vs = compile_stage(GL_VERTEX_SHADER, desc.vertex_shader, log);
    if (!vs) {
        MCE_LOG_ERROR("rhi.gl: vertex shader '{}' failed:\n{}", desc.vertex_shader.debug_name, log.c_str());
        return Error(Errc::ShaderCompile, "rhi.pipeline.vertex", desc.vertex_shader.debug_name);
    }
    const GLuint fs = compile_stage(GL_FRAGMENT_SHADER, desc.fragment_shader, log);
    if (!fs) {
        MCE_LOG_ERROR("rhi.gl: fragment shader '{}' failed:\n{}", desc.fragment_shader.debug_name, log.c_str());
        glDeleteProgram(vs);
        return Error(Errc::ShaderCompile, "rhi.pipeline.fragment", desc.fragment_shader.debug_name);
    }

    GlPipeline p;
    p.vs = vs;
    p.fs = fs;
    p.state = desc.state;
    p.stride = desc.vertex.stride;
    glCreateProgramPipelines(1, &p.pipeline);
    glUseProgramStages(p.pipeline, GL_VERTEX_SHADER_BIT, vs);
    glUseProgramStages(p.pipeline, GL_FRAGMENT_SHADER_BIT, fs);
    glValidateProgramPipeline(p.pipeline);
    GLint valid = GL_FALSE;
    glGetProgramPipelineiv(p.pipeline, GL_VALIDATE_STATUS, &valid);
    if (!valid) {
        char buf[1024] = {};
        glGetProgramPipelineInfoLog(p.pipeline, sizeof(buf), nullptr, buf);
        MCE_LOG_ERROR("rhi.gl: pipeline '{}' does not validate:\n{}", desc.debug_name, buf);
        glDeleteProgramPipelines(1, &p.pipeline);
        glDeleteProgram(vs);
        glDeleteProgram(fs);
        return Error(Errc::PipelineCreate, "rhi.pipeline.link", desc.debug_name);
    }

    glCreateVertexArrays(1, &p.vao);
    for (const VertexAttribute& a : desc.vertex.attrs) {
        glEnableVertexArrayAttrib(p.vao, a.location);
        switch (a.format) {
            case VertexFormat::Float1:
            case VertexFormat::Float2:
            case VertexFormat::Float3:
            case VertexFormat::Float4:
                glVertexArrayAttribFormat(p.vao, a.location, static_cast<GLint>(vertex_format_size(a.format) / 4),
                                          GL_FLOAT, GL_FALSE, a.offset);
                break;
            case VertexFormat::UNorm8x4:
                glVertexArrayAttribFormat(p.vao, a.location, 4, GL_UNSIGNED_BYTE, GL_TRUE, a.offset);
                break;
            case VertexFormat::UInt1: glVertexArrayAttribIFormat(p.vao, a.location, 1, GL_UNSIGNED_INT, a.offset); break;
            case VertexFormat::SNorm16x2: glVertexArrayAttribFormat(p.vao, a.location, 2, GL_SHORT, GL_TRUE, a.offset); break;
            case VertexFormat::SNorm16x4: glVertexArrayAttribFormat(p.vao, a.location, 4, GL_SHORT, GL_TRUE, a.offset); break;
            case VertexFormat::UNorm16x2:
                glVertexArrayAttribFormat(p.vao, a.location, 2, GL_UNSIGNED_SHORT, GL_TRUE, a.offset);
                break;
        }
        glVertexArrayAttribBinding(p.vao, a.location, 0);
    }
    label(GL_PROGRAM_PIPELINE, p.pipeline, desc.debug_name);
    label(GL_VERTEX_ARRAY, p.vao, desc.debug_name);
    return pipelines_.insert(p);
}

void GlDevice::defer_delete(DeadKind kind, GLuint id) {
    if (id) dead_[slot_].push_back(DeadObject{kind, id});
}

Result<PipelineHandle> GlDevice::create_compute_pipeline(const ComputePipelineDesc& desc) {
    FixedString<1024> log;
    const GLuint cs = compile_stage(GL_COMPUTE_SHADER, desc.compute_shader, log);
    if (!cs) {
        MCE_LOG_ERROR("rhi.gl: compute shader '{}' failed:\n{}", desc.compute_shader.debug_name, log.c_str());
        return Error(Errc::ShaderCompile, "rhi.pipeline.compute", desc.compute_shader.debug_name);
    }
    GlPipeline p;
    p.cs = cs;
    p.compute = true;
    glCreateProgramPipelines(1, &p.pipeline);
    glUseProgramStages(p.pipeline, GL_COMPUTE_SHADER_BIT, cs);
    label(GL_PROGRAM_PIPELINE, p.pipeline, desc.debug_name);
    return pipelines_.insert(p);
}

void GlDevice::delete_now(const DeadObject& d) {
    switch (d.kind) {
        case DeadKind::Buffer: glDeleteBuffers(1, &d.id); break;
        case DeadKind::Texture: glDeleteTextures(1, &d.id); break;
        case DeadKind::Sampler: glDeleteSamplers(1, &d.id); break;
        case DeadKind::Program: glDeleteProgram(d.id); break;
        case DeadKind::Pipeline: glDeleteProgramPipelines(1, &d.id); break;
        case DeadKind::VertexArray: glDeleteVertexArrays(1, &d.id); break;
    }
}

void GlDevice::flush_deletes(u32 slot) {
    for (const DeadObject& d : dead_[slot]) delete_now(d);
    dead_[slot].clear();
}

void GlDevice::destroy_buffer(BufferHandle h) {
    if (const GlBuffer* b = buffers_.try_get(h)) {
        defer_delete(DeadKind::Buffer, b->id);
        buffers_.erase(h);
    }
}

void GlDevice::destroy_texture(TextureHandle h) {
    const GlTexture* t = textures_.try_get(h);
    if (!t) return;
    // Framebuffers are container objects (not shared): drop every one that references the texture.
    for (usize i = 0; i < framebuffers_.size();) {
        const FramebufferKey& k = framebuffers_[i].key;
        bool uses = k.depth == h;
        for (const TextureHandle& c : k.colors) uses = uses || c == h;
        if (uses) {
            glDeleteFramebuffers(1, &framebuffers_[i].fbo);
            framebuffers_[i] = framebuffers_.back();
            framebuffers_.pop_back();
        } else {
            ++i;
        }
    }
    texture_bytes_ -= t->bytes;
    defer_delete(DeadKind::Texture, t->id);
    textures_.erase(h);
}

void GlDevice::destroy_sampler(SamplerHandle h) {
    if (const GlSampler* s = samplers_.try_get(h)) {
        defer_delete(DeadKind::Sampler, s->id);
        samplers_.erase(h);
    }
}

void GlDevice::destroy_pipeline(PipelineHandle h) {
    if (const GlPipeline* p = pipelines_.try_get(h)) {
        defer_delete(DeadKind::Pipeline, p->pipeline);
        if (p->compute) {
            defer_delete(DeadKind::Program, p->cs);
            pipelines_.erase(h);
            return;
        }
        defer_delete(DeadKind::Program, p->vs);
        defer_delete(DeadKind::Program, p->fs);
        defer_delete(DeadKind::VertexArray, p->vao);
        pipelines_.erase(h);
    }
}

Extent2D GlDevice::texture_size(TextureHandle h) const {
    const GlTexture* t = textures_.try_get(h);
    return t ? t->size : Extent2D{};
}

TextureDesc GlDevice::texture_desc(TextureHandle h) const {
    TextureDesc d;
    const GlTexture* t = textures_.try_get(h);
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

GLuint GlDevice::framebuffer_for(const FramebufferKey& key) {
    for (const FramebufferEntry& e : framebuffers_)
        if (e.key == key) return e.fbo;
    GLuint fbo = 0;
    glCreateFramebuffers(1, &fbo);
    GLenum draw_buffers[cmd::kMaxColorAttachments];
    GLsizei n = 0;
    for (u32 i = 0; i < cmd::kMaxColorAttachments; ++i) {
        const GlTexture* t = textures_.try_get(key.colors[i]);
        if (!t) continue;
        MCE_ASSERT_MSG(key.color_layers[i] < t->layers, "render target layer out of range");
        if (t->dimension == TextureDimension::Tex2DArray)
            glNamedFramebufferTextureLayer(fbo, GL_COLOR_ATTACHMENT0 + i, t->id, 0, key.color_layers[i]);
        else
            glNamedFramebufferTexture(fbo, GL_COLOR_ATTACHMENT0 + i, t->id, 0);
        draw_buffers[n++] = GL_COLOR_ATTACHMENT0 + i;
    }
    if (n > 0) glNamedFramebufferDrawBuffers(fbo, n, draw_buffers);
    else glNamedFramebufferDrawBuffer(fbo, GL_NONE);
    if (const GlTexture* d = textures_.try_get(key.depth)) {
        const GLenum attach =
            d->format == TextureFormat::D24_UNorm_S8 ? GL_DEPTH_STENCIL_ATTACHMENT : GL_DEPTH_ATTACHMENT;
        MCE_ASSERT_MSG(key.depth_layer < d->layers, "depth target layer out of range");
        if (d->dimension == TextureDimension::Tex2DArray) glNamedFramebufferTextureLayer(fbo, attach, d->id, 0, key.depth_layer);
        else glNamedFramebufferTexture(fbo, attach, d->id, 0);
    }
    const GLenum status = glCheckNamedFramebufferStatus(fbo, GL_FRAMEBUFFER);
    if (status != GL_FRAMEBUFFER_COMPLETE) {
        MCE_LOG_ERROR("rhi.gl: incomplete framebuffer (0x{:x})", static_cast<u32>(status));
        ++validation_errors_;
    }
    framebuffers_.push_back(FramebufferEntry{key, fbo});
    return fbo;
}

// ---- frames ----------------------------------------------------------------------------------

bool GlDevice::slot_signaled(u32 slot) {
    GLsync s = fences_[slot];
    if (!s) return true;
    const GLenum r = glClientWaitSync(s, 0, 0);
    return r == GL_ALREADY_SIGNALED || r == GL_CONDITION_SATISFIED;
}

void GlDevice::wait_slot(u32 slot) {
    GLsync s = fences_[slot];
    if (!s) return;
    constexpr GLuint64 kTimeoutNs = 1'000'000'000;  // 1 s, then warn and keep waiting
    for (;;) {
        const GLenum r = glClientWaitSync(s, GL_SYNC_FLUSH_COMMANDS_BIT, kTimeoutNs);
        if (r == GL_ALREADY_SIGNALED || r == GL_CONDITION_SATISFIED) return;
        if (r == GL_WAIT_FAILED) {
            MCE_LOG_ERROR("rhi.gl: glClientWaitSync failed on slot {}", slot);
            return;
        }
        MCE_LOG_WARN("rhi.gl: frame slot {} fence not signaled after 1 s", slot);
    }
}

mem::FrameSlotFence* GlDevice::fence_for_frame(u64 frame_index) {
    return &fence_objs_[frame_index % MCE_FRAMES_IN_FLIGHT];
}

void GlDevice::begin_frame(u64 frame_index) {
    MCE_ASSERT_MSG(!in_frame_, "begin_frame twice");
    frame_index_ = frame_index;
    slot_ = static_cast<u32>(frame_index % MCE_FRAMES_IN_FLIGHT);
    wait_slot(slot_);
    if (fences_[slot_]) {
        glDeleteSync(fences_[slot_]);
        fences_[slot_] = nullptr;
    }
    flush_deletes(slot_);
    collect_timestamps(slot_);
    geometry_ring_.slot = uniform_ring_.slot = indirect_ring_.slot = slot_;
    geometry_ring_.head = uniform_ring_.head = indirect_ring_.head = 0;
    stats_ = FrameStats{};
    in_frame_ = true;
}

// The slot's fence has passed, so every query written in it has its result.
void GlDevice::collect_timestamps(u32 slot) {
    const u32 written = timestamps_written_[slot];
    if (written == 0) return;
    GpuTimestamps t;
    t.frame_index = timestamps_frame_[slot];
    t.written = written;
    for (u32 i = 0; i < kMaxTimestamps; ++i) {
        if (!((written >> i) & 1u)) continue;
        GLuint64 v = 0;
        glGetQueryObjectui64v(timestamp_queries_[slot][i], GL_QUERY_RESULT, &v);
        t.ns[i] = v;
    }
    last_timestamps_ = t;
    timestamps_written_[slot] = 0;
}

void GlDevice::exec_write_timestamp(u32 index) {
    if (!caps_.timestamp_queries || index >= kMaxTimestamps) return;
    glQueryCounter(timestamp_queries_[slot_][index], GL_TIMESTAMP);
    timestamps_written_[slot_] |= 1u << index;
    timestamps_frame_[slot_] = frame_index_;
}

void GlDevice::end_frame() {
    MCE_ASSERT_MSG(in_frame_, "end_frame without begin_frame");
    fences_[slot_] = glFenceSync(GL_SYNC_GPU_COMMANDS_COMPLETE, 0);
    stats_.geometry_bytes = geometry_ring_.head;
    stats_.uniform_bytes = uniform_ring_.head;
    stats_.indirect_bytes = indirect_ring_.head;
    last_stats_ = stats_;
    in_frame_ = false;
}

void GlDevice::present(bool vsync) {
    const int interval = vsync ? 1 : 0;
    if (interval != swap_interval_) {
        SDL_GL_SetSwapInterval(interval);
        swap_interval_ = interval;
    }
    SDL_GL_SwapWindow(static_cast<SDL_Window*>(window_.native_handle()));
}

Extent2D GlDevice::backbuffer_size() const {
    const platform::PixelSize s = window_.framebuffer_size();
    return Extent2D{s.width, s.height};
}

ResultV GlDevice::read_buffer(BufferHandle h, u64 offset, Span<byte> out) {
    const GlBuffer* b = buffers_.try_get(h);
    if (!b) return Error(Errc::NotFound, "rhi.read_buffer", "stale buffer handle");
    if (offset + out.size() > b->size) return Error(Errc::InvalidArgument, "rhi.read_buffer", "range outside buffer");
    if (out.empty()) return ok();
    // Shader writes reach buffer reads only behind this barrier; the read itself synchronizes.
    glMemoryBarrier(GL_BUFFER_UPDATE_BARRIER_BIT);
    glGetNamedBufferSubData(b->id, static_cast<GLintptr>(offset), static_cast<GLsizeiptr>(out.size()), out.data());
    return ok();
}

ResultV GlDevice::read_pixels(TextureHandle target, Rect2D rect, Span<u8> out) {
    const usize row = usize(rect.width) * 4;
    if (out.size() < row * rect.height) return Error(Errc::InvalidArgument, "rhi.read_pixels", "output too small");
    Extent2D size;
    const GlTexture* t = nullptr;
    if (target.is_null()) {
        size = backbuffer_size();
    } else {
        t = textures_.try_get(target);
        if (!t) return Error(Errc::NotFound, "rhi.read_pixels", "stale texture handle");
        size = t->size;
    }
    if (rect.x < 0 || rect.y < 0 || u32(rect.x) + rect.width > size.width || u32(rect.y) + rect.height > size.height)
        return Error(Errc::InvalidArgument, "rhi.read_pixels", "rect outside target");

    const auto gl_y = static_cast<GLint>(size.height - (u32(rect.y) + rect.height));  // GL origin: bottom-left
    const auto bytes = static_cast<GLsizei>(row * rect.height);
    if (t) {
        glGetTextureSubImage(t->id, 0, rect.x, gl_y, 0, static_cast<GLsizei>(rect.width),
                             static_cast<GLsizei>(rect.height), 1, GL_RGBA, GL_UNSIGNED_BYTE, bytes, out.data());
    } else {
        glBindFramebuffer(GL_READ_FRAMEBUFFER, 0);
        glReadBuffer(GL_BACK);
        glReadnPixels(rect.x, gl_y, static_cast<GLsizei>(rect.width), static_cast<GLsizei>(rect.height), GL_RGBA,
                      GL_UNSIGNED_BYTE, bytes, out.data());
    }
    // Flip to top-row-first.
    Array<u8> tmp;
    tmp.resize(row);
    for (u32 y = 0; y < rect.height / 2; ++y) {
        u8* a = out.data() + usize(y) * row;
        u8* b = out.data() + usize(rect.height - 1 - y) * row;
        std::memcpy(tmp.data(), a, row);
        std::memcpy(a, b, row);
        std::memcpy(b, tmp.data(), row);
    }
    return ok();
}

}  // namespace mce::rhi::gl
