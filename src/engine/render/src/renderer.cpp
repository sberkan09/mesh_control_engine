#include "mce/render/renderer.h"

#include "mce/log.h"

namespace mce::render {

namespace {

// The sprite uber shader (rhi-renderer.md §2.7: material params ride in the vertex, zero
// per-sprite uniforms). Bindings follow the bank table: FrameU = 0, atlas array = 6, font = 11.
constexpr const char* kSpriteVS = R"(#version 450 core
layout(std140, binding = 0) uniform FrameU { mat4 u_view_proj; vec4 u_params; };
layout(location = 0) in vec2 a_pos;
layout(location = 1) in vec2 a_uv;
layout(location = 2) in vec4 a_color;
layout(location = 3) in uint a_packed;
layout(location = 0) out vec2 v_uv;
layout(location = 1) out vec4 v_color;
layout(location = 2) flat out uint v_packed;
out gl_PerVertex { vec4 gl_Position; };
void main() {
    v_uv = a_uv;
    v_color = a_color;
    v_packed = a_packed;
    gl_Position = u_view_proj * vec4(a_pos, 0.0, 1.0);
}
)";

// Texels and tints are linear after decode; the output is premultiplied and (unless the target
// is sRGB) re-encoded to sRGB, which keeps 2D blending in display space like every 2D engine.
constexpr const char* kSpriteFS = R"(#version 450 core
layout(std140, binding = 0) uniform FrameU { mat4 u_view_proj; vec4 u_params; };
layout(binding = 6) uniform sampler2DArray u_atlas;
layout(binding = 11) uniform sampler2DArray u_font;
layout(location = 0) in vec2 v_uv;
layout(location = 1) in vec4 v_color;
layout(location = 2) flat in uint v_packed;
layout(location = 0) out vec4 o_color;
vec3 srgb_to_linear(vec3 c) { return mix(c / 12.92, pow((c + 0.055) / 1.055, vec3(2.4)), step(0.04045, c)); }
vec3 linear_to_srgb(vec3 c) { return mix(c * 12.92, 1.055 * pow(c, vec3(1.0 / 2.4)) - 0.055, step(0.0031308, c)); }
void main() {
    float layer = float(v_packed & 0xFFu);
    uint flags = v_packed >> 24;
    vec4 texel = (flags & 1u) != 0u ? vec4(1.0, 1.0, 1.0, texture(u_font, vec3(v_uv, layer)).r)
                                    : texture(u_atlas, vec3(v_uv, layer));
    vec4 c = texel * vec4(srgb_to_linear(v_color.rgb), v_color.a);
    vec3 rgb = u_params.x > 0.5 ? linear_to_srgb(clamp(c.rgb, 0.0, 1.0)) : c.rgb;
    o_color = vec4(rgb * c.a, c.a);
}
)";

struct FrameU {
    math::mat4 view_proj;
    f32 params[4];  // x: encode sRGB output
};

}  // namespace

Renderer::~Renderer() { shutdown(); }

ResultV Renderer::init(rhi::RenderDevice& device, const RendererDesc& desc) {
    MCE_ASSERT_MSG(!device_, "Renderer::init twice");
    device_ = &device;
    desc_ = desc;
    MCE_TRY(textures_.init(device));
    meshes_.init(&device);
    MCE_TRY(create_pipelines());

    rhi::SamplerDesc sd;
    sd.min_filter = sd.mag_filter = rhi::Filter::Nearest;
    point_sampler_ = device.create_sampler(sd);
    sd.min_filter = sd.mag_filter = rhi::Filter::Linear;
    linear_sampler_ = device.create_sampler(sd);

    // The shared quad index buffer: 0 1 2 2 3 0 per quad; commands offset with base_vertex.
    Array<u32> idx;
    idx.resize(usize(kMaxQuadsPerDraw) * 6);
    for (u32 q = 0; q < kMaxQuadsPerDraw; ++q) {
        u32* i = idx.data() + usize(q) * 6;
        const u32 v = q * 4;
        i[0] = v;
        i[1] = v + 1;
        i[2] = v + 2;
        i[3] = v + 2;
        i[4] = v + 3;
        i[5] = v;
    }
    rhi::BufferDesc bd;
    bd.usage = rhi::BufferUsage::Index;
    bd.size_bytes = idx.size() * sizeof(u32);
    bd.debug_name = "mce.sprite.quad_indices";
    quad_indices_ = device.create_buffer(bd, std::as_bytes(idx.span()));
    return ok();
}

ResultV Renderer::create_pipelines() {
    static constexpr rhi::VertexAttribute kAttrs[] = {
        {0, rhi::VertexFormat::Float2, offsetof(SpriteVertex, x)},
        {1, rhi::VertexFormat::Float2, offsetof(SpriteVertex, u)},
        {2, rhi::VertexFormat::UNorm8x4, offsetof(SpriteVertex, color)},
        {3, rhi::VertexFormat::UInt1, offsetof(SpriteVertex, packed)},
    };
    for (u32 b = 0; b < static_cast<u32>(SpriteBlend::Count); ++b) {
        rhi::GraphicsPipelineDesc pd;
        pd.vertex_shader = {kSpriteVS, "sprite.vert"};
        pd.fragment_shader = {kSpriteFS, "sprite.frag"};
        pd.vertex = {kAttrs, sizeof(SpriteVertex)};
        pd.state.depth = {};  // painter's order: no depth in 2D (rendering-2d D11)
        pd.state.raster.cull = rhi::CullMode::None;
        pd.state.blend.mode =
            static_cast<SpriteBlend>(b) == SpriteBlend::Additive ? rhi::BlendMode::Additive : rhi::BlendMode::PremultipliedAlpha;
        pd.debug_name = static_cast<SpriteBlend>(b) == SpriteBlend::Additive ? "sprite.additive" : "sprite.alpha";
        MCE_TRY_ASSIGN(pipelines_[b], device_->create_pipeline(pd));
    }
    return ok();
}

void Renderer::shutdown() {
    if (!device_) return;
    forward_.shutdown();
    meshes_.shutdown();
    for (TilemapSlot& t : tilemaps_)
        if (t.alive) device_->destroy_buffer(t.buffer);
    tilemaps_.clear();
    for (FontStorage& f : fonts_) {
        textures_.release(f.texture_key);
        sets_.destroy(f.set);
    }
    fonts_.clear();
    for (rhi::PipelineHandle& p : pipelines_) {
        device_->destroy_pipeline(p);
        p = {};
    }
    device_->destroy_sampler(point_sampler_);
    device_->destroy_sampler(linear_sampler_);
    device_->destroy_buffer(quad_indices_);
    textures_.shutdown();
    device_ = nullptr;
}

Result<Font> Renderer::create_font(const FontMetrics& metrics, Span<const FontGlyph> glyphs, Span<const FontKern> kerning,
                                   Span<const byte> atlas_r8, const char* debug_name) {
    MCE_ASSERT_MSG(device_, "Renderer::create_font before init");
    if (metrics.atlas_width == 0 || metrics.atlas_height == 0 ||
        atlas_r8.size() < usize(metrics.atlas_width) * metrics.atlas_height)
        return Error(Errc::Truncated, "render.font.atlas", debug_name ? debug_name : "");
    for (usize i = 1; i < glyphs.size(); ++i)
        if (glyphs[i - 1].codepoint >= glyphs[i].codepoint)
            return Error(Errc::InvalidArgument, "render.font.glyph_order", debug_name ? debug_name : "", static_cast<u32>(i));

    FontStorage fs;
    for (const FontGlyph& g : glyphs) fs.glyphs.push_back(g);
    for (const FontKern& k : kerning) fs.kerning.push_back(k);
    fs.texture_key = textures_.allocate_key();
    const TextureMipUpload mip{metrics.atlas_width, metrics.atlas_height, atlas_r8};
    TextureUpload up;
    up.format = rhi::TextureFormat::R8_UNorm;
    up.mips = Span<const TextureMipUpload>(&mip, 1);
    up.debug_name = debug_name;
    MCE_TRY(textures_.upload(fs.texture_key, 1, up));

    SpriteSetDesc sd;
    sd.texture_key = fs.texture_key;
    sd.filter = rhi::Filter::Linear;
    sd.coverage = true;
    sd.debug_name = debug_name;
    fs.set = sets_.create(sd);
    if (!fs.set.valid()) {
        textures_.release(fs.texture_key);
        return Error(Errc::OutOfMemory, "render.font.set", debug_name ? debug_name : "");
    }
    Font font{FontFace(metrics, fs.glyphs.span(), fs.kerning.span()), fs.set};
    fonts_.push_back(std::move(fs));  // moving the Arrays keeps their storage: the face's spans stay valid
    return font;
}

ResultV Renderer::upload_tilemap(TilemapSlot& slot) {
    const usize bytes = slot.bake.vertices.size() * sizeof(SpriteVertex);
    if (bytes == 0) return ok();
    if (bytes > slot.capacity) {
        if (slot.capacity) device_->destroy_buffer(slot.buffer);
        rhi::BufferDesc bd;
        bd.usage = rhi::BufferUsage::Vertex;
        bd.size_bytes = bytes;
        bd.debug_name = "mce.tilemap";
        slot.buffer = device_->create_buffer(bd, std::as_bytes(slot.bake.vertices.span()));
        slot.capacity = bytes;
    } else {
        // In-place: GL orders the update after draws already issued from the old contents.
        device_->update_buffer(slot.buffer, 0, std::as_bytes(slot.bake.vertices.span()));
    }
    return ok();
}

Result<TilemapId> Renderer::create_tilemap(const TilemapDesc& desc) {
    MCE_ASSERT_MSG(device_, "Renderer::create_tilemap before init");
    u32 index = 0;
    while (index < tilemaps_.size() && tilemaps_[index].alive) ++index;
    if (index == tilemaps_.size()) tilemaps_.push_back(TilemapSlot{});
    TilemapSlot& slot = tilemaps_[index];
    MCE_TRY(bake_tilemap(desc, sets_, slot.bake));
    MCE_TRY(upload_tilemap(slot));
    slot.alive = true;
    return TilemapId{index};
}

ResultV Renderer::update_tilemap(TilemapId id, const TilemapDesc& desc) {
    if (!tilemap(id)) return Error(Errc::NotFound, "render.tilemap.update");
    TilemapSlot& slot = tilemaps_[id.value];
    MCE_TRY(bake_tilemap(desc, sets_, slot.bake));
    return upload_tilemap(slot);
}

void Renderer::destroy_tilemap(TilemapId id) {
    if (!tilemap(id)) return;
    TilemapSlot& slot = tilemaps_[id.value];
    if (slot.capacity) device_->destroy_buffer(slot.buffer);
    slot = TilemapSlot{};
}

const TilemapBake* Renderer::tilemap(TilemapId id) const {
    return id.value < tilemaps_.size() && tilemaps_[id.value].alive ? &tilemaps_[id.value].bake : nullptr;
}

void Renderer::add_tilemap(SpriteBatch& batch, TilemapId id) const {
    if (const TilemapBake* b = tilemap(id)) batch.add_tilemap(id.value, *b);
}

ResultV Renderer::init_3d(const Forward3DDesc& desc) {
    MCE_ASSERT_MSG(device_, "Renderer::init_3d before init");
    if (forward_.initialized()) return ok();
    Forward3DDesc d = desc;
    d.encode_srgb_output = desc_.encode_srgb_output;
    return forward_.init(*device_, textures_, d);
}

void Renderer::record_3d(rhi::CommandBuffer& cmd, const Scene3D& scene, rhi::TextureHandle target) {
    MCE_ASSERT_MSG(forward_.initialized(), "record_3d before init_3d");
    if (!forward_.initialized()) return;
    forward_.record(cmd, scene, target);
}

void Renderer::record_2d(rhi::CommandBuffer& cmd, const SpriteBatch& batch) {
    MCE_ASSERT_MSG(batch.built(), "record_2d: batch not built");
    MCE_ASSERT_MSG(cmd.in_pass(), "record_2d: no open pass");
    if (!batch.built() || batch.groups().empty()) return;

    cmd.push_debug_scope("sprites");
    FrameU fu;
    fu.view_proj = batch.view().view_proj;
    fu.params[0] = desc_.encode_srgb_output ? 1.0f : 0.0f;
    fu.params[1] = fu.params[2] = fu.params[3] = 0.0f;
    cmd.set_uniforms(rhi::bank::kFrame, rhi::as_bytes(fu));
    cmd.set_draw_args(batch.draw_args());
    cmd.bind_index_buffer(quad_indices_, rhi::IndexFormat::U32);

    u32 bound_pipeline = ~0u;
    u32 bound_segment = ~0u;
    u32 bound_set = ~0u;
    for (const SpriteBatch::Group& g : batch.groups()) {
        const auto blend = static_cast<u32>(g.blend);
        if (blend != bound_pipeline) {
            cmd.bind_pipeline(pipelines_[blend]);
            bound_pipeline = blend;
        }
        if (g.source == SpriteBatch::GroupSource::Tilemap) {
            if (g.tilemap >= tilemaps_.size() || !tilemaps_[g.tilemap].alive) continue;
            cmd.bind_vertex_buffer(tilemaps_[g.tilemap].buffer);
            bound_segment = ~0u;
        } else if (g.segment != bound_segment) {
            const SpriteBatch::Segment& s = batch.segments()[g.segment];
            cmd.set_vertex_data(std::as_bytes(batch.vertices().subspan(s.first_vertex, s.vertex_count)));
            bound_segment = g.segment;
        }
        if (g.set.value != bound_set) {
            const rhi::TextureHandle tex =
                sets_.valid(g.set) ? textures_.get(sets_.texture_key(g.set)) : textures_.error_texture();
            const rhi::SamplerHandle smp = sampler(sets_.valid(g.set) ? sets_.filter(g.set) : rhi::Filter::Nearest);
            const bool coverage = sets_.valid(g.set) && sets_.coverage(g.set);
            cmd.bind_texture(rhi::bank::kAtlasArray, coverage ? textures_.white() : tex, smp);
            cmd.bind_texture(rhi::bank::kFont, coverage ? tex : textures_.white(), smp);
            bound_set = g.set.value;
        }
        cmd.draw_indirect(g.command_count, g.first_command);
    }
    cmd.pop_debug_scope();
}

}  // namespace mce::render
