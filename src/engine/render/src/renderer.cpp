#include "mce/render/renderer.h"

#include "mce/log.h"
#include "shaders_cvd.h"

namespace mce::render {

namespace {

// The sprite uber shader (rhi-renderer.md §2.7: material params ride in the vertex, zero
// per-sprite uniforms). Bindings follow the bank table: FrameU = 0, LightsU = 1 (the pass bank),
// atlas array = 6, normal array = 7, font = 11.
constexpr const char* kSpriteVS = R"(#version 450 core
layout(std140, binding = 0) uniform FrameU { mat4 u_view_proj; vec4 u_params; };
layout(location = 0) in vec2 a_pos;
layout(location = 1) in vec2 a_uv;
layout(location = 2) in vec4 a_color;
layout(location = 3) in uint a_packed;
layout(location = 0) out vec2 v_uv;
layout(location = 1) out vec4 v_color;
layout(location = 2) flat out uint v_packed;
layout(location = 3) out vec2 v_world;
out gl_PerVertex { vec4 gl_Position; };
void main() {
    v_uv = a_uv;
    v_color = a_color;
    v_packed = a_packed;
    v_world = a_pos;
    gl_Position = u_view_proj * vec4(a_pos, 0.0, 1.0);
}
)";

// Texels and tints are linear after decode; the output is premultiplied and (unless the target
// is sRGB) re-encoded to sRGB, which keeps 2D blending in display space like every 2D engine.
//
// Lit quads (FR-R2D-090) turn their tangent-space normal by the packed orientation and are shaded
// with ambient + point lights (Lambert, (1 - d²/r²)² falloff, the light `height` above the plane).
constexpr const char* kSpriteFS = R"(#version 450 core
layout(std140, binding = 0) uniform FrameU { mat4 u_view_proj; vec4 u_params; };
layout(std140, binding = 1) uniform LightsU { vec4 u_ambient; vec4 u_light_count; vec4 u_light_pos[16]; vec4 u_light_color[16]; };
layout(binding = 6) uniform sampler2DArray u_atlas;
layout(binding = 7) uniform sampler2DArray u_normal;
layout(binding = 11) uniform sampler2DArray u_font;
layout(location = 0) in vec2 v_uv;
layout(location = 1) in vec4 v_color;
layout(location = 2) flat in uint v_packed;
layout(location = 3) in vec2 v_world;
layout(location = 0) out vec4 o_color;
vec3 shade(float layer) {
    vec3 n = texture(u_normal, vec3(v_uv, layer)).xyz * 2.0 - 1.0;
    uint o = (v_packed >> 8) & 0xFFFFu;
    if ((o & 0x4000u) != 0u) n.x = -n.x;
    if ((o & 0x8000u) != 0u) n.y = -n.y;
    float a = float(o & 0x3FFFu) * (6.28318530718 / 16384.0);
    float ca = cos(a), sa = sin(a);
    n = normalize(vec3(ca * n.x - sa * n.y, sa * n.x + ca * n.y, n.z));
    vec3 light = u_ambient.rgb;
    int count = int(u_light_count.x);
    for (int i = 0; i < count; ++i) {
        vec3 d = vec3(u_light_pos[i].xy - v_world, u_light_pos[i].z);
        float r = u_light_pos[i].w;
        float k = clamp(1.0 - dot(d.xy, d.xy) / (r * r), 0.0, 1.0);
        light += u_light_color[i].rgb * u_light_color[i].a * (k * k) * max(dot(n, normalize(d)), 0.0);
    }
    return light;
}
vec3 srgb_to_linear(vec3 c) { return mix(c / 12.92, pow((c + 0.055) / 1.055, vec3(2.4)), step(0.04045, c)); }
vec3 linear_to_srgb(vec3 c) { return mix(c * 12.92, 1.055 * pow(c, vec3(1.0 / 2.4)) - 0.055, step(0.0031308, c)); }
void main() {
    float layer = float(v_packed & 0xFFu);
    uint flags = v_packed >> 24;
    // Font atlases: coverage is the alpha; a distance field (flag 4) is thresholded at 0.5 over one
    // screen pixel, so magnified text keeps a hard edge. The derivative is taken outside the branch.
    float field = texture(u_font, vec3(v_uv, layer)).r;
    float aa = max(fwidth(field), 1e-4);
    float cover = (flags & 4u) != 0u ? clamp((field - 0.5) / aa + 0.5, 0.0, 1.0) : field;
    vec4 texel = (flags & 1u) != 0u ? vec4(1.0, 1.0, 1.0, cover) : texture(u_atlas, vec3(v_uv, layer));
    vec4 c = texel * vec4(srgb_to_linear(v_color.rgb), v_color.a);
    if ((flags & 2u) != 0u) c.rgb *= shade(layer);
    vec3 rgb = u_params.x > 0.5 ? linear_to_srgb(clamp(c.rgb, 0.0, 1.0)) : c.rgb;
    o_color = vec4(rgb * c.a, c.a);
}
)";

// 2D post (FR-R2D-100): chromatic aberration (radial R/B split), a 16³ strip-LUT grade, the
// vignette, then the color-vision correction (M-ACCESS). Bindings: PostU = 1, the frame = 6, the LUT = 7.
constexpr const char* kPostVS = R"(#version 450 core
layout(location = 0) out vec2 v_uv;
out gl_PerVertex { vec4 gl_Position; };
void main() {
    vec2 p = vec2((gl_VertexID << 1) & 2, gl_VertexID & 2);
    v_uv = p;
    gl_Position = vec4(p * 2.0 - 1.0, 0.0, 1.0);
}
)";

constexpr const char* kPostFS = "#version 450 core\n" MCE_GLSL_CVD R"(
layout(std140, binding = 1) uniform PostU { vec4 u_enable; vec4 u_vignette; vec4 u_grade; };
layout(binding = 6) uniform sampler2D u_src;
layout(binding = 7) uniform sampler2DArray u_lut;
layout(location = 0) in vec2 v_uv;
layout(location = 0) out vec4 o_color;
vec3 grade(vec3 c) {
    c = clamp(c, 0.0, 1.0) * 15.0;
    float b0 = floor(c.b), b1 = min(b0 + 1.0, 15.0);
    vec2 rg = vec2((c.r + 0.5) / 256.0, (c.g + 0.5) / 16.0);
    vec3 s0 = textureLod(u_lut, vec3(rg + vec2(b0 / 16.0, 0.0), 0.0), 0.0).rgb;
    vec3 s1 = textureLod(u_lut, vec3(rg + vec2(b1 / 16.0, 0.0), 0.0), 0.0).rgb;
    return mix(s0, s1, c.b - b0);
}
void main() {
    vec2 d = v_uv - 0.5;
    vec3 c;
    if (u_enable.z > 0.5) {
        vec2 off = d * (2.0 * u_grade.z);
        c = vec3(texture(u_src, v_uv + off).r, texture(u_src, v_uv).g, texture(u_src, v_uv - off).b);
    } else {
        c = texture(u_src, v_uv).rgb;
    }
    if (u_enable.y > 0.5) c = mix(c, grade(c), u_grade.x);
    if (u_enable.x > 0.5) {
        float r = length(vec2(d.x * u_vignette.w, d.y)) * 2.0;
        c *= 1.0 - u_vignette.x * smoothstep(u_vignette.y, u_vignette.y + u_vignette.z, r);
    }
    if (u_enable.w > 0.5) {  // color-vision correction: on linear values, last
        vec3 l = mix(c / 12.92, pow((c + 0.055) / 1.055, vec3(2.4)), step(0.04045, c));
        l = cvd_correct(int(u_enable.w + 0.5), u_grade.y, clamp(l, 0.0, 1.0));
        c = mix(l * 12.92, 1.055 * pow(l, vec3(1.0 / 2.4)) - 0.055, step(0.0031308, l));
    }
    o_color = vec4(c, 1.0);
}
)";

struct PostU {
    math::vec4 enable;    // x vignette, y grade, z aberration, w color-vision mode (0 = off)
    math::vec4 vignette;  // intensity, radius, softness, aspect
    math::vec4 grade;     // lut strength, color-vision strength, aberration
};

struct FrameU {
    math::mat4 view_proj;
    f32 params[4];  // x: encode sRGB output
};

struct LightsU {
    math::vec4 ambient;
    math::vec4 count;  // x
    math::vec4 pos[kMaxLights2D];    // xy position, z height, w radius
    math::vec4 color[kMaxLights2D];  // rgb linear color, a intensity
};
static_assert(sizeof(LightsU) == 32 + 32 * kMaxLights2D);

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
    rhi::GraphicsPipelineDesc post;
    post.vertex_shader = {kPostVS, "post2d.vert"};
    post.fragment_shader = {kPostFS, "post2d.frag"};
    post.state.depth = {};
    post.state.raster.cull = rhi::CullMode::None;
    post.debug_name = "post2d";
    MCE_TRY_ASSIGN(post_2d_, device_->create_pipeline(post));
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
    device_->destroy_pipeline(post_2d_);
    post_2d_ = {};
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
    sd.sdf = metrics.sdf_spread > 0;
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

void Renderer::add_tilemap(SpriteBatch& batch, TilemapId id, math::vec2 offset) const {
    if (const TilemapBake* b = tilemap(id)) batch.add_tilemap(id.value, *b, offset);
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

void Renderer::record_3d(rhi::CommandBuffer& cmd, const Scene3D& scene, rhi::TextureHandle target, const Forward3D::ViewOutput& out) {
    MCE_ASSERT_MSG(forward_.initialized(), "record_3d before init_3d");
    if (!forward_.initialized()) return;
    forward_.record(cmd, scene, target, out);
}

u32 Renderer::view_slot(u64 key) {
    for (u32 i = 0; i < Forward3D::kMaxViews; ++i) {
        if (view_slots_[i].held && view_slots_[i].key == key) {
            view_slots_[i].asked = true;
            return i;
        }
    }
    for (u32 i = 0; i < Forward3D::kMaxViews; ++i) {
        if (view_slots_[i].held) continue;
        view_slots_[i] = {key, true, true};
        return i;
    }
    return ~0u;
}

void Renderer::end_views() {
    for (u32 i = 0; i < Forward3D::kMaxViews; ++i) {
        ViewSlot& v = view_slots_[i];
        if (v.held && !v.asked) {
            if (i > 0) forward_.release_view(i);
            v = {};
        }
        v.asked = false;
    }
}

void Renderer::release_views() {
    for (u32 i = 0; i < Forward3D::kMaxViews; ++i) {
        if (i > 0 && view_slots_[i].held) forward_.release_view(i);
        view_slots_[i] = {};
    }
}

void make_identity_lut(Array<u8>& out) {
    out.resize(usize(kLutSize) * kLutSize * kLutSize * 4);
    for (u32 y = 0; y < kLutSize; ++y)
        for (u32 x = 0; x < kLutSize * kLutSize; ++x) {
            u8* p = out.data() + (usize(y) * kLutSize * kLutSize + x) * 4;
            p[0] = static_cast<u8>((x % kLutSize) * 255 / (kLutSize - 1));
            p[1] = static_cast<u8>(y * 255 / (kLutSize - 1));
            p[2] = static_cast<u8>((x / kLutSize) * 255 / (kLutSize - 1));
            p[3] = 255;
        }
}

void Renderer::record_post_2d(rhi::CommandBuffer& cmd, rhi::TextureHandle source, rhi::Extent2D size, const Post2D& post,
                              rhi::TextureHandle target) {
    MCE_ASSERT_MSG(!cmd.in_pass(), "record_post_2d: inside a pass");
    rhi::PassAttachment out;
    out.texture = target;
    out.load = rhi::LoadOp::DontCare;
    rhi::RenderPassDesc pass;
    pass.colors = Span<const rhi::PassAttachment>(&out, 1);
    pass.debug_name = "post2d";
    cmd.begin_pass(pass);
    PostU pu{};
    const f32 cvd_strength = math::clamp(post.color_vision_strength, 0.0f, 1.0f);
    const f32 cvd = cvd_strength > 0.0f ? static_cast<f32>(post.color_vision) : 0.0f;
    pu.enable = {post.vignette ? 1.0f : 0.0f, post.color_grade ? 1.0f : 0.0f, post.chromatic_aberration ? 1.0f : 0.0f, cvd};
    pu.vignette = {math::clamp(post.vignette_intensity, 0.0f, 1.0f), post.vignette_radius, math::max(post.vignette_softness, 1e-3f),
                   size.height ? static_cast<f32>(size.width) / static_cast<f32>(size.height) : 1.0f};
    pu.grade = {math::clamp(post.lut_strength, 0.0f, 1.0f), cvd_strength, post.aberration, 0.0f};
    cmd.bind_pipeline(post_2d_);
    cmd.set_uniforms(rhi::bank::kPass, rhi::as_bytes(pu));
    cmd.bind_texture(rhi::bank::kAtlasArray, source, linear_sampler_);
    cmd.bind_texture(rhi::bank::kNormal, post.color_grade ? textures_.get(post.lut_key) : textures_.white(), linear_sampler_);
    cmd.draw_non_indexed(3);
    cmd.end_pass();
}

void Renderer::record_2d(rhi::CommandBuffer& cmd, const SpriteBatch& batch, bool linear_output) {
    MCE_ASSERT_MSG(batch.built(), "record_2d: batch not built");
    MCE_ASSERT_MSG(cmd.in_pass(), "record_2d: no open pass");
    if (!batch.built() || batch.groups().empty()) return;

    cmd.push_debug_scope("sprites");
    FrameU fu;
    fu.view_proj = batch.view().view_proj;
    fu.params[0] = desc_.encode_srgb_output && !linear_output ? 1.0f : 0.0f;
    fu.params[1] = fu.params[2] = fu.params[3] = 0.0f;
    cmd.set_uniforms(rhi::bank::kFrame, rhi::as_bytes(fu));
    const Lights2D& lights = batch.lights();
    LightsU lu{};
    lu.ambient = {lights.ambient.r, lights.ambient.g, lights.ambient.b, 1.0f};
    lu.count.x = static_cast<f32>(math::min(lights.count, kMaxLights2D));
    for (u32 i = 0; i < lights.count && i < kMaxLights2D; ++i) {
        const PointLight2D& p = lights.points[i];
        lu.pos[i] = {p.position.x, p.position.y, p.height, math::max(p.radius, 1e-3f)};
        lu.color[i] = {p.color.r, p.color.g, p.color.b, p.intensity};
    }
    cmd.set_uniforms(rhi::bank::kPass, rhi::as_bytes(lu));
    cmd.set_draw_args(batch.draw_args());
    cmd.bind_index_buffer(quad_indices_, rhi::IndexFormat::U32);

    u32 bound_pipeline = ~0u;
    u32 bound_segment = ~0u;
    u32 bound_set = ~0u;
    math::vec2 bound_offset{};
    for (const SpriteBatch::Group& g : batch.groups()) {
        // Tilemaps drawn with a parallax offset translate the view; everything else draws at zero.
        const math::vec2 offset = g.source == SpriteBatch::GroupSource::Tilemap ? g.offset : math::vec2{};
        if (offset.x != bound_offset.x || offset.y != bound_offset.y) {
            fu.view_proj = batch.view().view_proj * math::translation({offset.x, offset.y, 0.0f});
            cmd.set_uniforms(rhi::bank::kFrame, rhi::as_bytes(fu));
            bound_offset = offset;
        }
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
            const bool lit = sets_.valid(g.set) && sets_.lit(g.set);
            cmd.bind_texture(rhi::bank::kNormal, lit ? textures_.get(sets_.normal_key(g.set)) : textures_.white(), smp);
            bound_set = g.set.value;
        }
        cmd.draw_indirect(g.command_count, g.first_command);
    }
    cmd.pop_debug_scope();
}

}  // namespace mce::render
