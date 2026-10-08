#include "mce/render/forward3d.h"

#include "mce/log.h"

namespace mce::render {

using namespace mce::math;

namespace {

// Vertex streams of CHNK_MESH (asset::MeshVertex): snorm16 position, octahedral snorm16 normal,
// unorm16 UV — the GPU unpacks them, the shader applies the per-submesh dequantization.
constexpr const char* kMeshVS = R"(#version 450 core
layout(std140, binding = 0) uniform FrameU { mat4 u_view_proj; vec4 u_camera_pos; vec4 u_params; };
layout(std140, binding = 3) uniform ObjectU {
    mat4 o_model; mat4 o_normal; vec4 o_uv_rect; vec4 o_base_color; vec4 o_emissive; vec4 o_params;
};
layout(location = 0) in vec4 a_pos;
layout(location = 1) in vec2 a_oct;
layout(location = 2) in vec2 a_uv;
layout(location = 0) out vec3 v_world;
layout(location = 1) out vec3 v_normal;
layout(location = 2) out vec2 v_uv;
out gl_PerVertex { vec4 gl_Position; };
vec3 oct_decode(vec2 e) {
    vec3 n = vec3(e, 1.0 - abs(e.x) - abs(e.y));
    if (n.z < 0.0) n.xy = (1.0 - abs(n.yx)) * vec2(n.x >= 0.0 ? 1.0 : -1.0, n.y >= 0.0 ? 1.0 : -1.0);
    return normalize(n);
}
void main() {
    vec4 w = o_model * vec4(a_pos.xyz, 1.0);
    v_world = w.xyz;
    v_normal = mat3(o_normal) * oct_decode(a_oct);
    v_uv = o_uv_rect.xy + a_uv * o_uv_rect.zw;
    gl_Position = u_view_proj * w;
}
)";

constexpr const char* kMeshFS = R"(#version 450 core
layout(std140, binding = 0) uniform FrameU { mat4 u_view_proj; vec4 u_camera_pos; vec4 u_params; };
layout(std140, binding = 1) uniform PassU {
    vec4 p_light_dir; vec4 p_light_color; vec4 p_sky; vec4 p_ground; vec4 p_fog; mat4 p_shadow; vec4 p_shadow_params;
};
layout(std140, binding = 3) uniform ObjectU {
    mat4 o_model; mat4 o_normal; vec4 o_uv_rect; vec4 o_base_color; vec4 o_emissive; vec4 o_params;
};
layout(binding = 6) uniform sampler2DArray u_albedo;
layout(binding = 9) uniform sampler2DShadow u_shadow;
layout(location = 0) in vec3 v_world;
layout(location = 1) in vec3 v_normal;
layout(location = 2) in vec2 v_uv;
layout(location = 0) out vec4 o_color;

// PCF 2x2 on top of the hardware 2x2 compare filter; normal-offset bias on the lookup position.
float shadow_term(vec3 world, vec3 n) {
    if (p_shadow_params.w < 0.5) return 1.0;
    vec4 s = p_shadow * vec4(world + n * p_shadow_params.y, 1.0);
    vec3 c = s.xyz / s.w * 0.5 + 0.5;
    if (any(lessThan(c, vec3(0.0))) || any(greaterThan(c, vec3(1.0)))) return 1.0;
    float t = p_shadow_params.x;
    float sum = 0.0;
    sum += texture(u_shadow, vec3(c.xy + vec2(-0.5, -0.5) * t, c.z - p_shadow_params.z));
    sum += texture(u_shadow, vec3(c.xy + vec2( 0.5, -0.5) * t, c.z - p_shadow_params.z));
    sum += texture(u_shadow, vec3(c.xy + vec2(-0.5,  0.5) * t, c.z - p_shadow_params.z));
    sum += texture(u_shadow, vec3(c.xy + vec2( 0.5,  0.5) * t, c.z - p_shadow_params.z));
    return sum * 0.25;
}

void main() {
    vec4 albedo = texture(u_albedo, vec3(v_uv, 0.0)) * o_base_color;
    if (o_params.x > 0.0 && albedo.a < o_params.x) discard;  // alpha mask
    vec3 n = normalize(v_normal);
    if (!gl_FrontFacing) n = -n;
    vec3 color;
    if (o_params.y > 0.5) {
        color = albedo.rgb;  // unlit
    } else {
        float metallic = clamp(o_params.w, 0.0, 1.0);
        float rough = clamp(o_params.z, 0.05, 1.0);
        vec3 l = -p_light_dir.xyz;
        vec3 v = normalize(u_camera_pos.xyz - v_world);
        float ndl = max(dot(n, l), 0.0);
        float sh = ndl > 0.0 ? shadow_term(v_world, n) : 0.0;
        // Blinn-Phong with roughness -> exponent (Walter's mapping) and its normalization term.
        float shininess = clamp(2.0 / (rough * rough * rough * rough) - 2.0, 1.0, 2048.0);
        vec3 h = normalize(l + v);
        float spec = pow(max(dot(n, h), 0.0), shininess) * (shininess + 8.0) / 8.0;
        vec3 f0 = mix(vec3(0.04), albedo.rgb, metallic);
        vec3 diffuse = albedo.rgb * (1.0 - metallic);
        vec3 direct = p_light_color.rgb * ndl * sh * (diffuse + f0 * spec);
        vec3 ambient = mix(p_ground.rgb, p_sky.rgb, n.y * 0.5 + 0.5) * albedo.rgb * mix(0.55, 1.0, sh * ndl);
        color = direct + ambient;
    }
    color += o_emissive.rgb;
    float dist = length(v_world - u_camera_pos.xyz);
    float f = 1.0 - exp(-(p_fog.w * dist) * (p_fog.w * dist));
    color = mix(color, p_fog.rgb, clamp(f, 0.0, 1.0));
    o_color = vec4(color, albedo.a);
}
)";

constexpr const char* kShadowVS = R"(#version 450 core
layout(std140, binding = 3) uniform ShadowU { mat4 s_mvp; };
layout(location = 0) in vec4 a_pos;
out gl_PerVertex { vec4 gl_Position; };
void main() { gl_Position = s_mvp * vec4(a_pos.xyz, 1.0); }
)";

constexpr const char* kShadowFS = R"(#version 450 core
void main() {}
)";

constexpr const char* kTonemapVS = R"(#version 450 core
layout(location = 0) out vec2 v_uv;
out gl_PerVertex { vec4 gl_Position; };
void main() {
    vec2 p = vec2((gl_VertexID << 1) & 2, gl_VertexID & 2);
    v_uv = p;
    gl_Position = vec4(p * 2.0 - 1.0, 0.0, 1.0);
}
)";

// ACES filmic fit (Narkowicz 2015, with its 0.6 input pre-scale so exposure 1 maps scene-referred
// albedo-range values to mid tones), then sRGB encode for the plain RGBA8 targets.
constexpr const char* kTonemapFS = R"(#version 450 core
layout(std140, binding = 0) uniform FrameU { mat4 u_view_proj; vec4 u_camera_pos; vec4 u_params; };
layout(binding = 6) uniform sampler2D u_hdr;
layout(location = 0) in vec2 v_uv;
layout(location = 0) out vec4 o_color;
vec3 aces(vec3 x) { return clamp((x * (2.51 * x + 0.03)) / (x * (2.43 * x + 0.59) + 0.14), 0.0, 1.0); }
vec3 linear_to_srgb(vec3 c) { return mix(c * 12.92, 1.055 * pow(c, vec3(1.0 / 2.4)) - 0.055, step(0.0031308, c)); }
void main() {
    vec3 c = aces(texture(u_hdr, v_uv).rgb * (u_params.x * 0.6));
    o_color = vec4(u_params.y > 0.5 ? linear_to_srgb(c) : c, 1.0);
}
)";

struct FrameU {
    mat4 view_proj;
    vec4 camera_pos;
    vec4 params;  // x: exposure, y: encode sRGB
};

struct PassU {
    vec4 light_dir;
    vec4 light_color;
    vec4 sky;
    vec4 ground;
    vec4 fog;  // rgb, density
    mat4 shadow;
    vec4 shadow_params;  // x: texel (uv), y: normal offset (m), z: depth bias, w: enabled
};

struct ObjectU {
    mat4 model;
    mat4 normal;
    vec4 uv_rect;
    vec4 base_color;
    vec4 emissive;
    vec4 params;  // x: alpha cutoff (0 = no mask), y: unlit, z: roughness, w: metallic
};

struct ShadowU {
    mat4 mvp;
};

}  // namespace

Forward3D::~Forward3D() { shutdown(); }

ResultV Forward3D::init(rhi::RenderDevice& device, const TextureStore& textures, const Forward3DDesc& desc) {
    MCE_ASSERT_MSG(!device_, "Forward3D::init twice");
    device_ = &device;
    textures_ = &textures;
    desc_ = desc;
    if (desc_.msaa < 1) desc_.msaa = 1;
    if (auto r = create_pipelines(); !r) {
        shutdown();
        return r;
    }
    rhi::SamplerDesc sd;
    sd.min_filter = sd.mag_filter = rhi::Filter::Linear;
    sd.mip = rhi::MipFilter::Linear;
    sd.wrap_s = sd.wrap_t = rhi::AddressMode::Repeat;
    sd.max_aniso = 4.0f;
    albedo_sampler_ = device.create_sampler(sd);
    rhi::SamplerDesc ss;
    ss.min_filter = ss.mag_filter = rhi::Filter::Linear;
    ss.compare = true;
    ss.compare_op = rhi::CompareOp::LessEqual;
    shadow_sampler_ = device.create_sampler(ss);
    rhi::SamplerDesc hs;
    hs.min_filter = hs.mag_filter = rhi::Filter::Nearest;
    hdr_sampler_ = device.create_sampler(hs);

    rhi::TextureDesc td;
    td.size = {desc_.shadow_size, desc_.shadow_size};
    td.format = rhi::TextureFormat::D32_Float;
    td.usage = rhi::TextureUsage::DepthTarget;
    td.debug_name = "mce.3d.shadow_map";
    shadow_map_ = device.create_texture(td);
    return ok();
}

ResultV Forward3D::create_pipelines() {
    static constexpr rhi::VertexAttribute kAttrs[] = {
        {0, rhi::VertexFormat::SNorm16x4, offsetof(asset::MeshVertex, position)},
        {1, rhi::VertexFormat::SNorm16x2, offsetof(asset::MeshVertex, normal)},
        {2, rhi::VertexFormat::UNorm16x2, offsetof(asset::MeshVertex, uv)},
    };
    const rhi::VertexLayout layout{kAttrs, sizeof(asset::MeshVertex)};

    rhi::GraphicsPipelineDesc pd;
    pd.vertex_shader = {kMeshVS, "mesh.vert"};
    pd.fragment_shader = {kMeshFS, "mesh.frag"};
    pd.vertex = layout;
    pd.state.depth = {true, true, rhi::CompareOp::LessEqual};
    pd.state.raster.cull = rhi::CullMode::Back;
    pd.debug_name = "mesh.opaque";
    MCE_TRY_ASSIGN(mesh_cull_, device_->create_pipeline(pd));
    pd.state.raster.cull = rhi::CullMode::None;
    pd.debug_name = "mesh.two_sided";
    MCE_TRY_ASSIGN(mesh_two_sided_, device_->create_pipeline(pd));
    pd.state.depth.write = false;
    pd.state.blend.mode = rhi::BlendMode::Alpha;
    pd.debug_name = "mesh.blend";
    MCE_TRY_ASSIGN(mesh_blend_, device_->create_pipeline(pd));

    rhi::GraphicsPipelineDesc sp;
    sp.vertex_shader = {kShadowVS, "shadow.vert"};
    sp.fragment_shader = {kShadowFS, "shadow.frag"};
    sp.vertex = layout;
    sp.state.depth = {true, true, rhi::CompareOp::LessEqual};
    sp.state.raster.cull = rhi::CullMode::None;  // kit meshes are often single-sided shells
    sp.debug_name = "mesh.shadow";
    MCE_TRY_ASSIGN(shadow_pipe_, device_->create_pipeline(sp));

    rhi::GraphicsPipelineDesc tp;
    tp.vertex_shader = {kTonemapVS, "tonemap.vert"};
    tp.fragment_shader = {kTonemapFS, "tonemap.frag"};
    tp.state.depth = {};
    tp.debug_name = "tonemap";
    MCE_TRY_ASSIGN(tonemap_, device_->create_pipeline(tp));
    return ok();
}

void Forward3D::destroy_targets() {
    if (!device_) return;
    for (rhi::TextureHandle* t : {&hdr_msaa_, &depth_, &hdr_}) {
        if (!t->is_null()) device_->destroy_texture(*t);
        *t = {};
    }
    size_ = {};
}

void Forward3D::ensure_targets(rhi::Extent2D size) {
    if (size == size_ && !hdr_.is_null()) return;
    destroy_targets();
    size_ = size;
    rhi::TextureDesc d;
    d.size = size;
    d.format = rhi::TextureFormat::R11G11B10F;
    d.usage = rhi::TextureUsage::SampledColorTarget;
    d.debug_name = "mce.3d.hdr";
    hdr_ = device_->create_texture(d);
    if (desc_.msaa > 1) {
        d.samples = static_cast<u8>(desc_.msaa);
        d.usage = rhi::TextureUsage::ColorTarget;
        d.debug_name = "mce.3d.hdr_msaa";
        hdr_msaa_ = device_->create_texture(d);
    }
    rhi::TextureDesc z;
    z.size = size;
    z.format = rhi::TextureFormat::D32_Float;
    z.usage = rhi::TextureUsage::DepthTarget;
    z.samples = static_cast<u8>(desc_.msaa);
    z.debug_name = "mce.3d.depth";
    depth_ = device_->create_texture(z);
}

void Forward3D::shutdown() {
    if (!device_) return;
    destroy_targets();
    for (rhi::PipelineHandle* p : {&shadow_pipe_, &mesh_cull_, &mesh_two_sided_, &mesh_blend_, &tonemap_}) {
        if (!p->is_null()) device_->destroy_pipeline(*p);
        *p = {};
    }
    for (rhi::SamplerHandle* s : {&albedo_sampler_, &shadow_sampler_, &hdr_sampler_}) {
        if (!s->is_null()) device_->destroy_sampler(*s);
        *s = {};
    }
    if (!shadow_map_.is_null()) device_->destroy_texture(shadow_map_);
    shadow_map_ = {};
    device_ = nullptr;
    textures_ = nullptr;
}

void Forward3D::record(rhi::CommandBuffer& cmd, const Scene3D& scene, rhi::TextureHandle target) {
    MCE_ASSERT_MSG(device_, "Forward3D::record before init");
    MCE_ASSERT_MSG(scene.built(), "Forward3D::record: scene not built");
    MCE_ASSERT_MSG(!cmd.in_pass(), "Forward3D::record inside a pass");
    stats_ = {};
    const rhi::Extent2D size = scene.view().viewport;
    if (size.width == 0 || size.height == 0) return;
    ensure_targets(size);
    const bool shadows = scene.light().shadows && !scene.shadow_draws().empty();

    // ---- 1. shadow pass
    if (shadows) {
        cmd.push_debug_scope("shadow");
        rhi::RenderPassDesc sp;
        rhi::PassAttachment depth;
        depth.texture = shadow_map_;
        depth.clear.depth = 1.0f;
        sp.depth = depth;
        sp.debug_name = "shadow";
        cmd.begin_pass(sp);
        cmd.bind_pipeline(shadow_pipe_);
        const GpuMesh* bound = nullptr;
        for (const Scene3D::Draw& d : scene.shadow_draws()) {
            if (d.mesh->vertices.is_null()) continue;
            if (d.mesh != bound) {
                cmd.bind_vertex_buffer(d.mesh->vertices);
                cmd.bind_index_buffer(d.mesh->indices, d.mesh->index_format);
                bound = d.mesh;
            }
            const GpuSubmesh& sm = d.mesh->submeshes[d.submesh];
            const ShadowU su{scene.light_view_proj() * d.model};
            cmd.set_uniforms(rhi::bank::kObject, rhi::as_bytes(su));
            cmd.draw(sm.index_count, 1, sm.first_index, 0, static_cast<i32>(sm.base_vertex));
            ++stats_.shadow_draws;
        }
        cmd.end_pass();
        cmd.pop_debug_scope();
    }

    // ---- 2. main forward pass
    cmd.push_debug_scope("forward");
    const Fog& fog = scene.fog();
    rhi::PassAttachment color;
    color.texture = desc_.msaa > 1 ? hdr_msaa_ : hdr_;
    color.clear.color[0] = fog.color.x;
    color.clear.color[1] = fog.color.y;
    color.clear.color[2] = fog.color.z;
    color.clear.color[3] = 1.0f;
    rhi::PassAttachment depth;
    depth.texture = depth_;
    depth.clear.depth = 1.0f;
    depth.store = rhi::StoreOp::DontCare;
    rhi::RenderPassDesc mp;
    mp.colors = Span<const rhi::PassAttachment>(&color, 1);
    mp.depth = depth;
    mp.debug_name = "forward";
    cmd.begin_pass(mp);

    const View3D& view = scene.view();
    FrameU fu;
    fu.view_proj = view.view_proj;
    fu.camera_pos = vec4(view.position, 1.0f);
    fu.params = {scene.exposure(), desc_.encode_srgb_output ? 1.0f : 0.0f, 0.0f, 0.0f};
    cmd.set_uniforms(rhi::bank::kFrame, rhi::as_bytes(fu));

    const DirectionalLight& light = scene.light();
    PassU pu;
    vec3 dir = normalize(light.direction);
    if (!(length_sq(dir) > 0.0f)) dir = {0, -1, 0};
    pu.light_dir = vec4(dir, 0.0f);
    pu.light_color = vec4(light.color * light.intensity, 0.0f);
    pu.sky = vec4(light.ambient_sky, 0.0f);
    pu.ground = vec4(light.ambient_ground, 0.0f);
    pu.fog = vec4(fog.color, fog.density);
    pu.shadow = scene.light_view_proj();
    const aabb3& sb = scene.scene_bounds();
    const f32 radius = length(sb.extents());
    const f32 world_texel = 2.0f * radius / f32(desc_.shadow_size);
    pu.shadow_params = {1.0f / f32(desc_.shadow_size), world_texel * 1.5f, 0.0015f, shadows ? 1.0f : 0.0f};
    cmd.set_uniforms(rhi::bank::kPass, rhi::as_bytes(pu));
    cmd.bind_texture(rhi::bank::kShadowMap, shadow_map_, shadow_sampler_);

    rhi::PipelineHandle bound_pipe;
    const GpuMesh* bound_mesh = nullptr;
    u32 bound_tex = ~0u - 1;
    const Span<const Scene3D::Draw> draws = scene.draws();
    for (u32 i = 0; i < draws.size(); ++i) {
        const Scene3D::Draw& d = draws[i];
        if (d.mesh->vertices.is_null()) continue;
        const GpuSubmesh& sm = d.mesh->submeshes[d.submesh];
        const asset::MaterialChunk& mat = sm.material;
        const rhi::PipelineHandle pipe = i >= scene.opaque_count()                         ? mesh_blend_
                                         : (mat.flags & asset::MaterialFlags::DoubleSided) ? mesh_two_sided_
                                                                                           : mesh_cull_;
        if (!(pipe == bound_pipe)) {
            cmd.bind_pipeline(pipe);
            bound_pipe = pipe;
            ++stats_.pipeline_binds;
        }
        if (d.mesh != bound_mesh) {
            cmd.bind_vertex_buffer(d.mesh->vertices);
            cmd.bind_index_buffer(d.mesh->indices, d.mesh->index_format);
            bound_mesh = d.mesh;
        }
        if (sm.albedo_key != bound_tex) {
            const rhi::TextureHandle tex = sm.albedo_key == ~0u ? textures_->white() : textures_->get(sm.albedo_key);
            cmd.bind_texture(rhi::bank::kAlbedo, tex, albedo_sampler_);
            bound_tex = sm.albedo_key;
            ++stats_.texture_binds;
        }
        const MeshInstance& inst = scene.instances()[d.instance];
        ObjectU ou;
        ou.model = d.model;
        ou.normal = d.normal;
        ou.uv_rect = sm.uv_rect;
        ou.base_color = {mat.base_color[0] * inst.tint.x, mat.base_color[1] * inst.tint.y, mat.base_color[2] * inst.tint.z,
                         mat.base_color[3] * inst.tint.w};
        ou.emissive = {mat.emissive[0], mat.emissive[1], mat.emissive[2], 0.0f};
        ou.params = {(mat.flags & asset::MaterialFlags::AlphaMask) ? mat.alpha_cutoff : 0.0f,
                     (mat.flags & asset::MaterialFlags::Unlit) ? 1.0f : 0.0f, mat.roughness, mat.metallic};
        cmd.set_uniforms(rhi::bank::kObject, rhi::as_bytes(ou));
        cmd.draw(sm.index_count, 1, sm.first_index, 0, static_cast<i32>(sm.base_vertex));
        ++stats_.main_draws;
    }
    cmd.end_pass();
    cmd.pop_debug_scope();

    // ---- 3. resolve
    if (desc_.msaa > 1) cmd.resolve(hdr_msaa_, hdr_);

    // ---- 4. tonemap
    rhi::PassAttachment out;
    out.texture = target;
    out.load = rhi::LoadOp::DontCare;
    rhi::RenderPassDesc tp;
    tp.colors = Span<const rhi::PassAttachment>(&out, 1);
    tp.debug_name = "tonemap";
    cmd.begin_pass(tp);
    cmd.bind_pipeline(tonemap_);
    cmd.set_uniforms(rhi::bank::kFrame, rhi::as_bytes(fu));
    cmd.bind_texture(rhi::bank::kAlbedo, hdr_, hdr_sampler_);
    cmd.draw_non_indexed(3);
    cmd.end_pass();
}

}  // namespace mce::render
