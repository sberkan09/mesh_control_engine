#pragma once
// GLSL 450 of the 3D frame (Forward3D, ADR-0024). Uniform blocks follow the fixed bank table
// (rhi-renderer.md §3.3): FrameU 0, PassU 1, ObjectU 3; std430 Clusters 4, LightPool 5,
// LightIndices 6, JointPalette 7 (skinned vertex shaders); samplers albedo 6, normal 7, ORM 8,
// shadow cascades 9, sky 10, emissive 13; image-based lighting (M-VISUAL 2): pre-filtered sky 12,
// BRDF LUT 14, SH9 irradiance 15.
// The C++ mirrors of the blocks live in forward3d.cpp and must match member for member.

namespace mce::render::shaders3d {

// clang-format off
#define MCE_GLSL_FRAME_U R"(
layout(std140, binding = 0) uniform FrameU {
    mat4 u_view_proj;
    mat4 u_view;
    mat4 u_inv_view_proj;
    vec4 u_camera_pos;   // xyz, w: unused
    vec4 u_params;       // x: exposure, y: encode sRGB, z: bloom intensity, w: bloom on
    vec4 u_viewport;     // width, height, 1/width, 1/height
    vec4 u_cluster;      // grid x, grid y, grid z, point light count
    vec4 u_cluster_z;    // slice = floor(log(depth) * x + y); z: near, w: far
};
)"

#define MCE_GLSL_PASS_U R"(
layout(std140, binding = 1) uniform PassU {
    vec4 p_light_dir;      // xyz: the way sunlight travels; w: cascade count (0 = no shadows)
    vec4 p_light_color;    // rgb: color * intensity
    vec4 p_amb_sky;
    vec4 p_amb_ground;
    vec4 p_fog;            // rgb, w: exp2 density
    mat4 p_cascade[4];
    vec4 p_cascade_far;    // view depth where each cascade ends
    vec4 p_cascade_texel;  // world size of a shadow texel per cascade
    vec4 p_cascade_blend;  // cross-fade zone length per cascade (meters of view depth)
    vec4 p_sky_zenith;     // rgb, w: sky enabled
    vec4 p_sky_horizon;    // rgb, w: specular ambient scale
    vec4 p_sky_ground;     // rgb, w: sky texture max lod (-1 = gradient)
    vec4 p_sun;            // x: cos of the disk radius, y: disk intensity, z: sky texture intensity, w: texture is additive
    vec4 p_ibl;            // x: image-based lighting on, y: diffuse scale, z: pre-filtered layers,
                           // w: bake roughness (bake passes) / 1 = an IBL source other than the visible sky (lit passes)
    vec4 p_probes;         // x: reflection probe count (image-based lighting only)
    vec4 p_probe_pos[4];   // capture position, w: intensity
    vec4 p_probe_min[4];   // box, w: blend distance
    vec4 p_probe_max[4];   // box, w: first u_env layer (sharp capture, then the pre-filtered layers)
};
layout(binding = 10) uniform sampler2DArray u_sky;
const float PI = 3.14159265359;
vec2 equirect_uv(vec3 d) { return vec2(atan(d.z, d.x) * 0.15915494 + 0.5, acos(clamp(d.y, -1.0, 1.0)) * 0.31830989); }
vec3 sky_gradient(vec3 d) {
    float h = d.y;
    if (h >= 0.0) return mix(p_sky_horizon.rgb, p_sky_zenith.rgb, sqrt(h));
    return mix(p_sky_horizon.rgb, p_sky_ground.rgb, clamp(-h * 5.0, 0.0, 1.0));
}
// Sky radiance along d; `lod` blurs the texture path (roughness), the gradient is smooth already.
vec3 sky_radiance(vec3 d, float lod) {
    if (p_sky_ground.w < 0.0) return sky_gradient(d);
    vec3 tex = textureLod(u_sky, vec3(equirect_uv(d), 0.0), min(lod, p_sky_ground.w)).rgb * p_sun.z;
    return p_sun.w > 0.5 ? sky_gradient(d) + tex : tex;  // additive: a star field over a gradient
}
)"

#define MCE_GLSL_OBJECT_U R"(
layout(std140, binding = 3) uniform ObjectU {
    mat4 o_model;
    mat4 o_normal;
    vec4 o_uv_rect;
    vec4 o_base_color;
    vec4 o_emissive;   // rgb factor, w: emissive map
    vec4 o_params;     // x: alpha cutoff (0 = no mask), y: unlit, z: roughness, w: metallic
    vec4 o_maps;       // x: normal map, y: ORM map, z: occlusion in ORM.r, w: occlusion-only map
    mat4 o_dequant;    // skinned draws: quantized -> bind space (o_model is then the instance's world)
    vec4 o_skin;       // x: first joint in the palette, -1 = rigid
};
)"
// clang-format on

// Vertex streams of CHNK_MESH (asset::MeshVertex): snorm16 position, octahedral snorm16 normal,
// unorm16 UV — the GPU unpacks them, the shader applies the per-submesh dequantization.
constexpr const char* kMeshVS = "#version 450 core\n" MCE_GLSL_FRAME_U MCE_GLSL_OBJECT_U R"(
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

// Skinning (FR-R3D-060, optional): the joint palette (model-space joint matrices, Scene3D) and
// the skin matrix of a vertex's 4 influences. Joint indices arrive as unorm8 (x255 to index).
#define MCE_GLSL_SKIN R"(
layout(location = 3) in vec4 a_joints;
layout(location = 4) in vec4 a_weights;
layout(std430, binding = 7) readonly buffer JointPalette { mat4 j_palette[]; };
mat4 skin_matrix(float first) {
    if (first < 0.0 || dot(a_weights, vec4(1.0)) <= 0.0) return mat4(1.0);
    int base = int(first);
    ivec4 j = ivec4(a_joints * 255.0 + 0.5);
    return j_palette[base + j.x] * a_weights.x + j_palette[base + j.y] * a_weights.y +
           j_palette[base + j.z] * a_weights.z + j_palette[base + j.w] * a_weights.w;
}
)"

// The skinned twin of kMeshVS (SkinnedMeshVertex streams): bind-space vertex -> skin -> world.
// Rigid submeshes of a skinned mesh (o_skin.x < 0) take the identity skin and the rigid model.
constexpr const char* kSkinnedMeshVS = "#version 450 core\n" MCE_GLSL_FRAME_U MCE_GLSL_OBJECT_U MCE_GLSL_SKIN R"(
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
    mat4 s = skin_matrix(o_skin.x);
    vec4 w = o_model * (s * (o_dequant * vec4(a_pos.xyz, 1.0)));
    v_world = w.xyz;
    v_normal = mat3(o_normal) * (mat3(s) * oct_decode(a_oct));
    v_uv = o_uv_rect.xy + a_uv * o_uv_rect.zw;
    gl_Position = u_view_proj * w;
}
)";

// Image-based lighting (M-VISUAL 2): the sky pre-filtered for GGX into equirect layers of rising
// roughness ((i + 1) / layers; roughness 0 reads the sky itself), the split-sum BRDF LUT and the SH9
// irradiance, stored as 9 texels already convolved with the cosine lobe and divided by pi.
#define MCE_GLSL_IBL R"(
layout(binding = 12) uniform sampler2DArray u_env;
layout(binding = 14) uniform sampler2D u_brdf_lut;
layout(binding = 15) uniform sampler2D u_env_sh;
vec3 sh_irradiance(vec3 n) {
    vec3 e = texelFetch(u_env_sh, ivec2(0, 0), 0).rgb * 0.282095;
    e += texelFetch(u_env_sh, ivec2(1, 0), 0).rgb * (0.488603 * n.y);
    e += texelFetch(u_env_sh, ivec2(2, 0), 0).rgb * (0.488603 * n.z);
    e += texelFetch(u_env_sh, ivec2(3, 0), 0).rgb * (0.488603 * n.x);
    e += texelFetch(u_env_sh, ivec2(4, 0), 0).rgb * (1.092548 * n.x * n.y);
    e += texelFetch(u_env_sh, ivec2(5, 0), 0).rgb * (1.092548 * n.y * n.z);
    e += texelFetch(u_env_sh, ivec2(6, 0), 0).rgb * (0.315392 * (3.0 * n.z * n.z - 1.0));
    e += texelFetch(u_env_sh, ivec2(7, 0), 0).rgb * (1.092548 * n.x * n.z);
    e += texelFetch(u_env_sh, ivec2(8, 0), 0).rgb * (0.546274 * (n.x * n.x - n.y * n.y));
    return max(e, vec3(0.0));
}
// A probe's environment along d: its sharp capture at roughness 0, then layers (i + 1) / layers.
vec3 probe_env(float base, vec3 d, float rough) {
    vec2 uv = equirect_uv(d);
    float lv = rough * p_ibl.z;
    float i0 = floor(lv), i1 = min(i0 + 1.0, p_ibl.z);
    return mix(textureLod(u_env, vec3(uv, base + i0), 0.0).rgb, textureLod(u_env, vec3(uv, base + i1), 0.0).rgb, lv - i0);
}
// The sky's pre-filtered specular replaced, inside a probe's box, by the probe's box-projected one.
vec3 probe_specular(vec3 p, vec3 r, float rough, vec3 sky) {
    float best = 0.0;
    vec3 chosen = sky;
    for (int i = 0; i < int(p_probes.x); ++i) {
        vec3 bmin = p_probe_min[i].xyz, bmax = p_probe_max[i].xyz;
        vec3 inside = min(p - bmin, bmax - p);
        float edge = min(min(inside.x, inside.y), inside.z);
        if (edge <= 0.0) continue;
        float fade = clamp(edge / max(p_probe_min[i].w, 1e-3), 0.0, 1.0);
        if (fade <= best) continue;
        vec3 tf = max((bmax - p) / r, (bmin - p) / r);  // exit distances along r per slab
        float t = min(min(tf.x, tf.y), tf.z);
        vec3 d = normalize(p + r * t - p_probe_pos[i].xyz);  // parallax: the box hit as seen from the capture point
        chosen = probe_env(p_probe_max[i].w, d, rough) * p_probe_pos[i].w;
        best = fade;
    }
    return mix(sky, chosen, best);
}
// The sky slot is laid out like a probe's (layer 0 sharp, then 0.2 ... 1.0). Below roughness 0.2 the
// visible sky is read itself (full texture detail) unless the IBL has its own source.
vec3 prefiltered_sky(vec3 r, float rough) {
    float lv = rough * p_ibl.z;
    if (lv >= 1.0 || p_ibl.w > 0.5) return probe_env(0.0, r, rough);
    return mix(sky_radiance(r, rough * 8.0), textureLod(u_env, vec3(equirect_uv(r), 1.0), 0.0).rgb, lv);
}
)"

constexpr const char* kMeshFS = "#version 450 core\n" MCE_GLSL_FRAME_U MCE_GLSL_PASS_U MCE_GLSL_OBJECT_U MCE_GLSL_IBL R"(
layout(binding = 6) uniform sampler2DArray u_albedo;
layout(binding = 7) uniform sampler2DArray u_normal_map;
layout(binding = 8) uniform sampler2DArray u_orm;
layout(binding = 9) uniform sampler2DArrayShadow u_shadow;
layout(binding = 13) uniform sampler2DArray u_emissive;
layout(std430, binding = 4) readonly buffer Clusters { uvec2 c_ranges[]; };
struct Light { vec4 pos_radius; vec4 color; vec4 spot; };
layout(std430, binding = 5) readonly buffer LightPool { Light l_lights[]; };
layout(std430, binding = 6) readonly buffer LightIndices { uint l_indices[]; };
layout(location = 0) in vec3 v_world;
layout(location = 1) in vec3 v_normal;
layout(location = 2) in vec2 v_uv;
layout(location = 0) out vec4 o_color;

// ---- shadows: cascade by view depth, PCF 3x3 over the hardware 2x2 compare, cross-faded
float sample_cascade(int i, vec3 world, vec3 n, vec3 l) {
    // Normal offset growing toward grazing light (where acne lives) + a receiver push toward the light.
    float texel = p_cascade_texel[i];
    float NoL = clamp(dot(n, l), 0.0, 1.0);
    vec3 p = world + n * (texel * (1.0 + 2.0 * sqrt(1.0 - NoL * NoL))) + l * texel;
    vec3 c = (p_cascade[i] * vec4(p, 1.0)).xyz * 0.5 + 0.5;
    if (any(lessThan(c.xy, vec2(0.0))) || any(greaterThan(c.xy, vec2(1.0)))) return 1.0;
    vec2 ts = 1.0 / vec2(textureSize(u_shadow, 0).xy);
    float sum = 0.0;
    for (int y = -1; y <= 1; ++y)
        for (int x = -1; x <= 1; ++x) sum += texture(u_shadow, vec4(c.xy + vec2(x, y) * ts, float(i), min(c.z, 1.0)));
    return sum * (1.0 / 9.0);
}
float shadow_term(vec3 world, vec3 n, vec3 l, float depth) {
    int count = int(p_light_dir.w);
    for (int i = 0; i < count; ++i) {
        float far_i = p_cascade_far[i];
        if (depth >= far_i) continue;
        float s = sample_cascade(i, world, n, l);
        float zone = p_cascade_blend[i];
        float t = zone > 0.0 ? clamp((depth - (far_i - zone)) / zone, 0.0, 1.0) : 0.0;
        if (t > 0.0) s = mix(s, i + 1 < count ? sample_cascade(i + 1, world, n, l) : 1.0, t);
        return s;
    }
    return 1.0;
}

// ---- Cook-Torrance GGX (Karis 2013 / Filament): D_GGX, V_SmithGGXCorrelated, F_Schlick
float D_GGX(float NoH, float a) {
    float a2 = a * a;
    float f = (NoH * a2 - NoH) * NoH + 1.0;
    return a2 / (PI * f * f);
}
float V_SmithGGXCorrelated(float NoV, float NoL, float a) {
    float a2 = a * a;
    float gv = NoL * sqrt(NoV * NoV * (1.0 - a2) + a2);
    float gl = NoV * sqrt(NoL * NoL * (1.0 - a2) + a2);
    return 0.5 / max(gv + gl, 1e-5);
}
vec3 F_Schlick(vec3 f0, float VoH) {
    float f = pow(1.0 - VoH, 5.0);
    return f + f0 * (1.0 - f);
}
// Radiance reflected toward v from a light of radiance-scale `e` along l. The engine convention
// (ADR-0024): a white Lambert surface facing a light of intensity I reflects I, so the BRDF is
// scaled by PI.
vec3 direct(vec3 n, vec3 v, vec3 l, vec3 diffuse, vec3 f0, float a, vec3 e) {
    float NoL = clamp(dot(n, l), 0.0, 1.0);
    if (NoL <= 0.0) return vec3(0.0);
    vec3 h = normalize(v + l);
    float NoV = max(dot(n, v), 1e-4);
    float NoH = clamp(dot(n, h), 0.0, 1.0);
    float VoH = clamp(dot(v, h), 0.0, 1.0);
    vec3 F = F_Schlick(f0, VoH);
    vec3 spec = D_GGX(NoH, a) * V_SmithGGXCorrelated(NoV, NoL, a) * F;
    return (diffuse * (1.0 - F) / PI + spec) * (PI * NoL) * e;
}
// Split-sum environment BRDF, analytic fit (Karis, "Physically Based Shading on Mobile").
vec3 env_brdf(vec3 f0, float rough, float NoV) {
    const vec4 c0 = vec4(-1.0, -0.0275, -0.572, 0.022);
    const vec4 c1 = vec4(1.0, 0.0425, 1.04, -0.04);
    vec4 r = rough * c0 + c1;
    float a004 = min(r.x * r.x, exp2(-9.28 * NoV)) * r.x + r.y;
    vec2 ab = vec2(-1.04, 1.04) * a004 + r.zw;
    return f0 * ab.x + ab.y;
}
// Normal mapping without stored tangents: the cotangent frame from screen derivatives (Schüler).
vec3 perturb_normal(vec3 n, vec3 p, vec2 uv, vec3 ts) {
    vec3 dp1 = dFdx(p), dp2 = dFdy(p);
    vec2 duv1 = dFdx(uv), duv2 = dFdy(uv);
    vec3 dp2perp = cross(dp2, n), dp1perp = cross(n, dp1);
    vec3 t = dp2perp * duv1.x + dp1perp * duv2.x;
    vec3 b = dp2perp * duv1.y + dp1perp * duv2.y;
    float m = max(dot(t, t), dot(b, b));
    if (m <= 1e-20) return n;
    float s = inversesqrt(m);
    // glTF normal maps are +Y up in the image while UV v grows downward: B points along -v.
    return normalize(mat3(t * s, -b * s, n) * ts);
}

uint cluster_of(float depth) {
    uvec3 g = uvec3(u_cluster.xyz);
    uint tx = min(uint(gl_FragCoord.x * u_viewport.z * u_cluster.x), g.x - 1u);
    uint ty = min(uint(gl_FragCoord.y * u_viewport.w * u_cluster.y), g.y - 1u);
    int s = int(floor(log(max(depth, 1e-4)) * u_cluster_z.x + u_cluster_z.y));
    uint tz = uint(clamp(s, 0, int(g.z) - 1));
    return tx + g.x * (ty + g.y * tz);
}

void main() {
    vec4 base = texture(u_albedo, vec3(v_uv, 0.0)) * o_base_color;
    if (o_params.x > 0.0 && base.a < o_params.x) discard;  // alpha mask
    vec3 n = normalize(v_normal);
    if (!gl_FrontFacing) n = -n;
    vec3 color;
    if (o_params.y > 0.5) {
        color = base.rgb;  // unlit
    } else {
        float rough = o_params.z, metallic = o_params.w, ao = 1.0;
        if (o_maps.y > 0.5) {
            vec3 orm = texture(u_orm, vec3(v_uv, 0.0)).rgb;
            if (o_maps.z > 0.5) ao = orm.r;
            if (o_maps.w < 0.5) {
                rough *= orm.g;
                metallic *= orm.b;
            }
        }
        if (o_maps.x > 0.5) n = perturb_normal(n, v_world, v_uv, texture(u_normal_map, vec3(v_uv, 0.0)).xyz * 2.0 - 1.0);
        rough = clamp(rough, 0.045, 1.0);
        metallic = clamp(metallic, 0.0, 1.0);
        float a = rough * rough;
        vec3 diffuse = base.rgb * (1.0 - metallic);
        vec3 f0 = mix(vec3(0.04), base.rgb, metallic);
        vec3 v = normalize(u_camera_pos.xyz - v_world);
        float depth = -(u_view * vec4(v_world, 1.0)).z;

        // The sun, shadowed.
        vec3 l = -p_light_dir.xyz;
        float sh = dot(n, l) > 0.0 ? shadow_term(v_world, normalize(v_normal) * (gl_FrontFacing ? 1.0 : -1.0), l, depth) : 0.0;
        color = direct(n, v, l, diffuse, f0, a, p_light_color.rgb) * sh;

        // Forward+ point and spot lights of this fragment's cluster.
        uvec2 range = c_ranges[cluster_of(depth)];
        for (uint i = 0u; i < range.y; ++i) {
            Light L = l_lights[l_indices[range.x + i]];
            vec3 dl = L.pos_radius.xyz - v_world;
            float d2 = dot(dl, dl);
            float f = d2 * L.color.w;
            float win = clamp(1.0 - f * f, 0.0, 1.0);
            float att = win * win / max(d2, 0.01);  // inverse square, windowed to 0 at the radius
            vec3 ld = dl * inversesqrt(d2);
            float cone = clamp(dot(-ld, L.spot.xyz) + L.spot.w, 0.0, 1.0);  // point lights: 0·x + 1
            att *= cone * cone;
            if (att <= 0.0) continue;
            color += direct(n, v, ld, diffuse, f0, a, L.color.rgb * att);
        }

        // Ambient: hemisphere diffuse + sky reflections through the analytic split-sum, or with
        // image-based lighting the SH9 sky irradiance + the pre-filtered sky through the BRDF LUT.
        float NoV = max(dot(n, v), 1e-4);
        vec3 r = reflect(-v, n);
        float horizon = clamp(1.0 + dot(r, normalize(v_normal)), 0.0, 1.0);
        vec3 amb_diffuse, amb_spec;
        if (p_ibl.x > 0.5) {
            amb_diffuse = sh_irradiance(n) * diffuse * p_ibl.y;
            vec2 ab = textureLod(u_brdf_lut, vec2(NoV, rough), 0.0).rg;
            vec3 env = prefiltered_sky(r, rough);
            if (p_probes.x > 0.5) env = probe_specular(v_world, r, rough, env);
            amb_spec = env * (f0 * ab.x + ab.y) * p_sky_horizon.w * horizon * horizon;
        } else {
            amb_diffuse = mix(p_amb_ground.rgb, p_amb_sky.rgb, n.y * 0.5 + 0.5) * diffuse;
            amb_spec = sky_radiance(r, rough * 8.0) * env_brdf(f0, rough, NoV) * p_sky_horizon.w * horizon * horizon;
        }
        color += (amb_diffuse * mix(0.55, 1.0, sh * clamp(dot(n, l), 0.0, 1.0)) + amb_spec) * ao;
    }
    vec3 emissive = o_emissive.rgb;
    if (o_emissive.w > 0.5) emissive *= texture(u_emissive, vec3(v_uv, 0.0)).rgb;
    color += emissive;
    float dist = length(v_world - u_camera_pos.xyz);
    float fog = 1.0 - exp(-(p_fog.w * dist) * (p_fog.w * dist));
    color = mix(color, p_fog.rgb, clamp(fog, 0.0, 1.0));
    o_color = vec4(color, base.a);
}
)";

constexpr const char* kShadowVS = R"(#version 450 core
layout(std140, binding = 3) uniform ShadowU { mat4 s_mvp; };
layout(location = 0) in vec4 a_pos;
out gl_PerVertex { vec4 gl_Position; };
void main() { gl_Position = s_mvp * vec4(a_pos.xyz, 1.0); }
)";

constexpr const char* kSkinnedShadowVS = "#version 450 core\n" R"(
layout(std140, binding = 3) uniform ShadowU { mat4 s_mvp; mat4 s_dequant; vec4 s_skin; };
layout(location = 0) in vec4 a_pos;
)" MCE_GLSL_SKIN R"(
out gl_PerVertex { vec4 gl_Position; };
void main() { gl_Position = s_mvp * (skin_matrix(s_skin.x) * (s_dequant * vec4(a_pos.xyz, 1.0))); }
)";

constexpr const char* kShadowFS = R"(#version 450 core
void main() {}
)";

constexpr const char* kFullscreenVS = R"(#version 450 core
layout(location = 0) out vec2 v_uv;
out gl_PerVertex { vec4 gl_Position; };
void main() {
    vec2 p = vec2((gl_VertexID << 1) & 2, gl_VertexID & 2);
    v_uv = p;
    gl_Position = vec4(p * 2.0 - 1.0, 0.0, 1.0);
}
)";

// The sky: a full-screen triangle at the far plane (depth test LEQUAL against the cleared 1.0),
// so it only fills pixels no geometry covered.
constexpr const char* kSkyVS = R"(#version 450 core
layout(location = 0) out vec2 v_ndc;
out gl_PerVertex { vec4 gl_Position; };
void main() {
    vec2 p = vec2((gl_VertexID << 1) & 2, gl_VertexID & 2) * 2.0 - 1.0;
    v_ndc = p;
    gl_Position = vec4(p, 1.0, 1.0);
}
)";

constexpr const char* kSkyFS = "#version 450 core\n" MCE_GLSL_FRAME_U MCE_GLSL_PASS_U R"(
layout(location = 0) in vec2 v_ndc;
layout(location = 0) out vec4 o_color;
void main() {
    vec4 w = u_inv_view_proj * vec4(v_ndc, 1.0, 1.0);
    vec3 d = normalize(w.xyz / w.w - u_camera_pos.xyz);
    vec3 c = sky_radiance(d, 0.0);
    vec3 sun = -p_light_dir.xyz;
    float cs = dot(d, sun);
    float edge = 1.0 - p_sun.x;
    c += p_light_color.rgb * p_sun.y * smoothstep(p_sun.x - edge * 0.25, p_sun.x + edge * 0.25, cs);
    if (p_fog.w > 0.0) c = mix(p_fog.rgb, c, smoothstep(0.0, 0.35, d.y));  // haze toward the horizon
    o_color = vec4(c, 1.0);
}
)";

// ---- post chain. PostU rides bank 1 for the post passes.
#define MCE_GLSL_POST_U R"(
layout(std140, binding = 1) uniform PostU {
    vec4 q_texel;   // xy: 1 / source size
    vec4 q_params;  // x: threshold, y: knee, z: first pass (bright-pass + Karis average), w: exposure
};
layout(binding = 6) uniform sampler2D u_src;
layout(location = 0) in vec2 v_uv;
layout(location = 0) out vec4 o_color;
vec3 tap(vec2 o) { return textureLod(u_src, v_uv + o * q_texel.xy, 0.0).rgb; }
)"

// 13-tap downsample (Jimenez, "Next Generation Post Processing in Call of Duty: AW"). The first
// pass exposes, soft-thresholds and Karis-averages each 2x2 group so single hot pixels can't flicker.
constexpr const char* kBloomDownFS = "#version 450 core\n" MCE_GLSL_POST_U R"(
float karis(vec3 c) { return 1.0 / (1.0 + dot(c, vec3(0.2126, 0.7152, 0.0722))); }
void main() {
    vec3 a = tap(vec2(-2, 2)), b = tap(vec2(0, 2)), c = tap(vec2(2, 2));
    vec3 d = tap(vec2(-2, 0)), e = tap(vec2(0, 0)), f = tap(vec2(2, 0));
    vec3 g = tap(vec2(-2, -2)), h = tap(vec2(0, -2)), i = tap(vec2(2, -2));
    vec3 j = tap(vec2(-1, 1)), k = tap(vec2(1, 1)), l = tap(vec2(-1, -1)), m = tap(vec2(1, -1));
    vec3 g0 = (j + k + l + m) * 0.25, g1 = (a + b + d + e) * 0.25, g2 = (b + c + e + f) * 0.25;
    vec3 g3 = (d + e + g + h) * 0.25, g4 = (e + f + h + i) * 0.25;
    vec3 col;
    if (q_params.z > 0.5) {
        float w0 = 0.5 * karis(g0), w1 = 0.125 * karis(g1), w2 = 0.125 * karis(g2), w3 = 0.125 * karis(g3), w4 = 0.125 * karis(g4);
        col = (g0 * w0 + g1 * w1 + g2 * w2 + g3 * w3 + g4 * w4) / (w0 + w1 + w2 + w3 + w4);
        col *= q_params.w;  // exposure: the bloom chain works in exposed units
        if (q_params.x > 0.0) {
            float br = max(col.r, max(col.g, col.b));
            float knee = q_params.y;
            float soft = clamp(br - q_params.x + knee, 0.0, 2.0 * knee);
            soft = soft * soft / (4.0 * knee + 1e-4);
            col *= max(soft, br - q_params.x) / max(br, 1e-4);
        }
    } else {
        col = g0 * 0.5 + (g1 + g2 + g3 + g4) * 0.125;
    }
    o_color = vec4(max(col, vec3(0.0)), 1.0);
}
)";

// 3x3 tent upsample, added onto the next larger level (the pipeline blends additively).
constexpr const char* kBloomUpFS = "#version 450 core\n" MCE_GLSL_POST_U R"(
void main() {
    vec3 s = tap(vec2(-1, 1)) + tap(vec2(1, 1)) + tap(vec2(-1, -1)) + tap(vec2(1, -1));
    s += (tap(vec2(0, 1)) + tap(vec2(-1, 0)) + tap(vec2(1, 0)) + tap(vec2(0, -1))) * 2.0;
    s += tap(vec2(0, 0)) * 4.0;
    o_color = vec4(s * (1.0 / 16.0), 1.0);
}
)";

// Exposure + bloom, ACES filmic fit (Narkowicz 2015, with its 0.6 input pre-scale so exposure 1
// maps scene-referred albedo-range values to mid tones), then sRGB encode. Alpha carries the
// encoded luma for FXAA.
constexpr const char* kTonemapFS = "#version 450 core\n" MCE_GLSL_FRAME_U R"(
layout(binding = 6) uniform sampler2D u_hdr;
layout(binding = 7) uniform sampler2D u_bloom;
layout(location = 0) in vec2 v_uv;
layout(location = 0) out vec4 o_color;
vec3 aces(vec3 x) { return clamp((x * (2.51 * x + 0.03)) / (x * (2.43 * x + 0.59) + 0.14), 0.0, 1.0); }
vec3 linear_to_srgb(vec3 c) { return mix(c * 12.92, 1.055 * pow(c, vec3(1.0 / 2.4)) - 0.055, step(0.0031308, c)); }
void main() {
    vec3 c = texture(u_hdr, v_uv).rgb * u_params.x;
    if (u_params.w > 0.5) c += textureLod(u_bloom, v_uv, 0.0).rgb * u_params.z;
    c = aces(c * 0.6);
    c = u_params.y > 0.5 ? linear_to_srgb(c) : c;
    o_color = vec4(c, dot(c, vec3(0.299, 0.587, 0.114)));
}
)";

// FXAA (Lottes' FXAA 3.11 "quality" algorithm: local contrast test, edge direction, end-of-edge
// search, sub-pixel blend) over the tonemapped image with luma in alpha.
constexpr const char* kFxaaFS = "#version 450 core\n" MCE_GLSL_FRAME_U R"(
layout(binding = 6) uniform sampler2D u_ldr;
layout(location = 0) in vec2 v_uv;
layout(location = 0) out vec4 o_color;
float luma(vec2 uv) { return textureLod(u_ldr, uv, 0.0).a; }
const float kSteps[12] = float[](1.0, 1.0, 1.0, 1.0, 1.0, 1.5, 2.0, 2.0, 2.0, 2.0, 4.0, 8.0);
void main() {
    vec2 rcp = u_viewport.zw;
    vec2 uv = v_uv;
    vec4 center = textureLod(u_ldr, uv, 0.0);
    float lM = center.a;
    float lS = textureLodOffset(u_ldr, uv, 0.0, ivec2(0, -1)).a;
    float lN = textureLodOffset(u_ldr, uv, 0.0, ivec2(0, 1)).a;
    float lW = textureLodOffset(u_ldr, uv, 0.0, ivec2(-1, 0)).a;
    float lE = textureLodOffset(u_ldr, uv, 0.0, ivec2(1, 0)).a;
    float lmax = max(lM, max(max(lN, lS), max(lE, lW)));
    float lmin = min(lM, min(min(lN, lS), min(lE, lW)));
    float range = lmax - lmin;
    if (range < max(0.0833, lmax * 0.166)) {
        o_color = vec4(center.rgb, 1.0);
        return;
    }
    float lSW = textureLodOffset(u_ldr, uv, 0.0, ivec2(-1, -1)).a;
    float lNE = textureLodOffset(u_ldr, uv, 0.0, ivec2(1, 1)).a;
    float lNW = textureLodOffset(u_ldr, uv, 0.0, ivec2(-1, 1)).a;
    float lSE = textureLodOffset(u_ldr, uv, 0.0, ivec2(1, -1)).a;
    float lNS = lN + lS, lWE = lW + lE;
    float avg = (2.0 * (lNS + lWE) + (lNW + lNE + lSW + lSE)) / 12.0;
    float sub = clamp(abs(avg - lM) / range, 0.0, 1.0);
    sub = smoothstep(0.0, 1.0, sub);
    float sub_offset = sub * sub * 0.75;
    float edge_h = abs(-2.0 * lW + lNW + lSW) + 2.0 * abs(-2.0 * lM + lN + lS) + abs(-2.0 * lE + lNE + lSE);
    float edge_v = abs(-2.0 * lS + lSW + lSE) + 2.0 * abs(-2.0 * lM + lW + lE) + abs(-2.0 * lN + lNW + lNE);
    bool horz = edge_h >= edge_v;
    float step_len = horz ? rcp.y : rcp.x;
    float l1 = horz ? lS : lW, l2 = horz ? lN : lE;
    float g1 = l1 - lM, g2 = l2 - lM;
    bool steep1 = abs(g1) >= abs(g2);
    float grad = 0.25 * max(abs(g1), abs(g2));
    float local_avg;
    if (steep1) {
        step_len = -step_len;
        local_avg = 0.5 * (l1 + lM);
    } else {
        local_avg = 0.5 * (l2 + lM);
    }
    vec2 cuv = uv;
    if (horz) cuv.y += step_len * 0.5; else cuv.x += step_len * 0.5;
    vec2 off = horz ? vec2(rcp.x, 0.0) : vec2(0.0, rcp.y);
    vec2 uv1 = cuv - off, uv2 = cuv + off;
    float e1 = luma(uv1) - local_avg, e2 = luma(uv2) - local_avg;
    bool r1 = abs(e1) >= grad, r2 = abs(e2) >= grad;
    if (!r1) uv1 -= off;
    if (!r2) uv2 += off;
    for (int i = 2; i < 12 && !(r1 && r2); ++i) {
        if (!r1) {
            e1 = luma(uv1) - local_avg;
            r1 = abs(e1) >= grad;
            if (!r1) uv1 -= off * kSteps[i];
        }
        if (!r2) {
            e2 = luma(uv2) - local_avg;
            r2 = abs(e2) >= grad;
            if (!r2) uv2 += off * kSteps[i];
        }
    }
    float d1 = horz ? uv.x - uv1.x : uv.y - uv1.y;
    float d2 = horz ? uv2.x - uv.x : uv2.y - uv.y;
    bool dir1 = d1 < d2;
    float dmin = min(d1, d2);
    float thickness = d1 + d2;
    float px_offset = -dmin / thickness + 0.5;
    bool center_smaller = lM < local_avg;
    bool correct = ((dir1 ? e1 : e2) < 0.0) != center_smaller;
    float final_offset = max(correct ? px_offset : 0.0, sub_offset);
    vec2 fuv = uv;
    if (horz) fuv.y += final_offset * step_len; else fuv.x += final_offset * step_len;
    o_color = vec4(textureLod(u_ldr, fuv, 0.0).rgb, 1.0);
}
)";

// ---- image-based lighting bakes (M-VISUAL 2): full-screen passes, so every backend runs them -------

#define MCE_GLSL_IBL_SAMPLING R"(
vec2 hammersley(uint i, uint n) { return vec2(float(i) / float(n), float(bitfieldReverse(i)) * 2.3283064365386963e-10); }
// A GGX half vector around n for roughness^2 `a`.
vec3 importance_ggx(vec2 xi, vec3 n, float a) {
    float phi = 2.0 * PI * xi.x;
    float cos_t = sqrt((1.0 - xi.y) / (1.0 + (a * a - 1.0) * xi.y));
    float sin_t = sqrt(1.0 - cos_t * cos_t);
    vec3 up = abs(n.y) < 0.999 ? vec3(0.0, 1.0, 0.0) : vec3(1.0, 0.0, 0.0);
    vec3 t = normalize(cross(up, n));
    vec3 b = cross(n, t);
    return normalize(t * (sin_t * cos(phi)) + b * (sin_t * sin(phi)) + n * cos_t);
}
vec3 equirect_dir(vec2 uv) {
    float phi = (uv.x - 0.5) * 2.0 * PI, theta = uv.y * PI;
    return vec3(sin(theta) * cos(phi), cos(theta), sin(theta) * sin(phi));
}
)"

// One layer of the pre-filtered sky: GGX-weighted sky radiance around each texel's direction
// (N = V = R), with filtered importance sampling reading blurrier sky mips for sparser samples.
constexpr const char* kIblPrefilterFS = "#version 450 core\n" MCE_GLSL_PASS_U MCE_GLSL_IBL_SAMPLING R"(
layout(location = 0) in vec2 v_uv;
layout(location = 0) out vec4 o_color;
const uint kSamples = 64u;
void main() {
    vec3 n = equirect_dir(v_uv);
    if (p_ibl.w <= 0.0) {  // the sharp layer: the source itself
        o_color = vec4(sky_radiance(n, 0.0), 1.0);
        return;
    }
    float a = p_ibl.w * p_ibl.w;
    // Solid angle of one texel of the sky texture's top mip (2:1 equirect, p_sky_ground.w = max lod).
    float texel_sa = 4.0 * PI / exp2(2.0 * max(p_sky_ground.w, 0.0) - 1.0);
    vec3 sum = vec3(0.0);
    float weight = 0.0;
    for (uint i = 0u; i < kSamples; ++i) {
        vec3 h = importance_ggx(hammersley(i, kSamples), n, a);
        vec3 l = 2.0 * dot(n, h) * h - n;
        float nol = dot(n, l);
        if (nol <= 0.0) continue;
        float noh = max(dot(n, h), 0.0);
        float dd = noh * noh * (a * a - 1.0) + 1.0;
        float pdf = a * a / (PI * dd * dd) * 0.25;  // D * NoH / (4 VoH) with V = N
        float lod = 0.5 * log2(1.0 / (float(kSamples) * pdf + 1e-6) / texel_sa) + 1.0;
        sum += sky_radiance(l, max(lod, 0.0)) * nol;
        weight += nol;
    }
    o_color = vec4(sum / max(weight, 1e-4), 1.0);
}
)";

// SH9 of the sky: texel k of a 9×1 target integrates radiance × Y_k over the sphere (64×32 grid),
// then applies the cosine-lobe band factors (pi, 2pi/3, pi/4) and the 1/pi of a Lambert BRDF.
constexpr const char* kIblShFS = "#version 450 core\n" MCE_GLSL_PASS_U MCE_GLSL_IBL_SAMPLING R"(
layout(location = 0) in vec2 v_uv;  // unread; declared so the interface matches fullscreen.vert
layout(location = 0) out vec4 o_color;
float sh_basis(int k, vec3 d) {
    if (k == 0) return 0.282095;
    if (k == 1) return 0.488603 * d.y;
    if (k == 2) return 0.488603 * d.z;
    if (k == 3) return 0.488603 * d.x;
    if (k == 4) return 1.092548 * d.x * d.y;
    if (k == 5) return 1.092548 * d.y * d.z;
    if (k == 6) return 0.315392 * (3.0 * d.z * d.z - 1.0);
    if (k == 7) return 1.092548 * d.x * d.z;
    return 0.546274 * (d.x * d.x - d.y * d.y);
}
void main() {
    int k = int(gl_FragCoord.x);
    float lod = max(p_sky_ground.w - 6.0, 0.0);  // the mip nearest the 64-wide grid
    vec3 sum = vec3(0.0);
    for (int j = 0; j < 32; ++j) {
        for (int i = 0; i < 64; ++i) {
            vec2 uv = vec2((float(i) + 0.5) / 64.0, (float(j) + 0.5) / 32.0);
            vec3 d = equirect_dir(uv);
            float d_omega = sin(uv.y * PI) * (PI / 32.0) * (2.0 * PI / 64.0);
            sum += sky_radiance(d, lod) * sh_basis(k, d) * d_omega;
        }
    }
    float band = k == 0 ? 1.0 : (k < 4 ? 2.0 / 3.0 : 0.25);
    o_color = vec4(sum * band, 1.0);
}
)";

// A probe's sharp capture: the six faces (90° views of a 2D array, `q_face_vp` as rendered) into
// one equirect layer.
constexpr const char* kProbeReprojectFS = R"(#version 450 core
layout(std140, binding = 1) uniform ProbeU {
    mat4 q_face_vp[6];
    vec4 q_pos;  // xyz: capture position
};
layout(binding = 6) uniform sampler2DArray u_faces;
layout(location = 0) in vec2 v_uv;
layout(location = 0) out vec4 o_color;
const float PI = 3.14159265359;
)" MCE_GLSL_IBL_SAMPLING R"(
void main() {
    vec3 d = equirect_dir(v_uv);
    vec3 c = vec3(0.0);
    for (int f = 0; f < 6; ++f) {
        vec4 clip = q_face_vp[f] * vec4(q_pos.xyz + d, 1.0);
        if (clip.w <= 0.0) continue;
        vec2 ndc = clip.xy / clip.w;
        if (abs(ndc.x) > 1.0 || abs(ndc.y) > 1.0) continue;
        c = textureLod(u_faces, vec3(ndc * 0.5 + 0.5, float(f)), 0.0).rgb;
        break;
    }
    o_color = vec4(c, 1.0);
}
)";

// A probe's pre-filtered layers, GGX-convolved from its sharp capture (no mips to read: many samples).
constexpr const char* kProbePrefilterFS = R"(#version 450 core
layout(std140, binding = 1) uniform PrefilterU {
    vec4 f_params;  // y: samples, w: roughness
};
layout(binding = 6) uniform sampler2D u_src;
layout(location = 0) in vec2 v_uv;
layout(location = 0) out vec4 o_color;
const float PI = 3.14159265359;
vec2 equirect_uv(vec3 d) { return vec2(atan(d.z, d.x) * 0.15915494 + 0.5, acos(clamp(d.y, -1.0, 1.0)) * 0.31830989); }
)" MCE_GLSL_IBL_SAMPLING R"(
void main() {
    vec3 n = equirect_dir(v_uv);
    float a = f_params.w * f_params.w;
    uint count = uint(f_params.y);
    vec3 sum = vec3(0.0);
    float weight = 0.0;
    for (uint i = 0u; i < count; ++i) {
        vec3 h = importance_ggx(hammersley(i, count), n, a);
        vec3 l = 2.0 * dot(n, h) * h - n;
        float nol = dot(n, l);
        if (nol <= 0.0) continue;
        sum += textureLod(u_src, equirect_uv(l), 0.0).rgb * nol;
        weight += nol;
    }
    o_color = vec4(sum / max(weight, 1e-4), 1.0);
}
)";

// The split-sum BRDF LUT (Karis 2013): x = NoV, y = roughness; rg = scale and bias of F0.
constexpr const char* kIblBrdfFS = "#version 450 core\nconst float PI = 3.14159265359;\n" MCE_GLSL_IBL_SAMPLING R"(
layout(location = 0) in vec2 v_uv;
layout(location = 0) out vec4 o_color;
const uint kSamples = 256u;
void main() {
    float nov = max(v_uv.x, 1e-3), rough = v_uv.y;
    float a = rough * rough;
    float k = a * 0.5;  // Smith-Schlick k for image-based lighting
    vec3 v = vec3(sqrt(1.0 - nov * nov), 0.0, nov);
    vec2 ab = vec2(0.0);
    for (uint i = 0u; i < kSamples; ++i) {
        vec3 h = importance_ggx(hammersley(i, kSamples), vec3(0.0, 0.0, 1.0), a);
        vec3 l = 2.0 * dot(v, h) * h - v;
        float nol = clamp(l.z, 0.0, 1.0), noh = clamp(h.z, 0.0, 1.0), voh = clamp(dot(v, h), 0.0, 1.0);
        if (nol <= 0.0) continue;
        float g = (nol / (nol * (1.0 - k) + k)) * (nov / (nov * (1.0 - k) + k));
        float g_vis = g * voh / max(noh * nov, 1e-6);
        float fc = pow(1.0 - voh, 5.0);
        ab += vec2((1.0 - fc) * g_vis, fc * g_vis);
    }
    o_color = vec4(ab / float(kSamples), 0.0, 1.0);
}
)";

#undef MCE_GLSL_IBL
#undef MCE_GLSL_IBL_SAMPLING
#undef MCE_GLSL_FRAME_U
#undef MCE_GLSL_PASS_U
// Forward+ light assignment on the GPU (M-VISUAL 1): the twin of ClusterLights::assign(). One
// invocation per cluster tests every culled light against the cluster's view-space AABB, in light
// order, so each cluster's list matches the CPU's order. The radius test is inflated by 1e-4 so the
// lists are a superset of the CPU's: an extra light is one whose window is 0 at every pixel of the
// cluster, and the fragment shader skips att <= 0, so the frame stays identical.
constexpr const char* kClusterBuildCS = R"(#version 450 core
layout(local_size_x = 64) in;
layout(std140, binding = 1) uniform ClusterBuildU {
    uvec4 u_grid;    // grid x, y, z, culled light count
    uvec4 u_layout;  // x: index capacity, y: tile-y offset in b_bounds, z: slice-edge offset
};
struct CullLight { vec4 center_radius; uvec4 light; };
layout(std430, binding = 4) writeonly buffer Clusters { uvec2 c_ranges[]; };
layout(std430, binding = 6) writeonly buffer LightIndices { uint l_indices[]; };
layout(std430, binding = 8) readonly buffer CullLights { CullLight c_lights[]; };
layout(std430, binding = 9) readonly buffer Bounds { float b_bounds[]; };
layout(std430, binding = 10) buffer Counter { uint n_total; uint n_dropped; };

float dist_sq_range(float v, float lo, float hi) {
    float d = v < lo ? lo - v : (v > hi ? v - hi : 0.0);
    return d * d;
}

void main() {
    uint gx = u_grid.x, gy = u_grid.y;
    uint c = gl_GlobalInvocationID.x;
    if (c >= gx * gy * u_grid.z) return;
    uint tx = c % gx, ty = (c / gx) % gy, s = c / (gx * gy);
    vec2 bx = vec2(b_bounds[(s * gx + tx) * 2u], b_bounds[(s * gx + tx) * 2u + 1u]);
    vec2 by = vec2(b_bounds[u_layout.y + (s * gy + ty) * 2u], b_bounds[u_layout.y + (s * gy + ty) * 2u + 1u]);
    vec2 bz = vec2(-b_bounds[u_layout.z + s + 1u], -b_bounds[u_layout.z + s]);
    uint n = 0u;
    for (uint i = 0u; i < u_grid.w; ++i) {
        vec4 l = c_lights[i].center_radius;
        float d2 = dist_sq_range(l.z, bz.x, bz.y) + dist_sq_range(l.y, by.x, by.y) + dist_sq_range(l.x, bx.x, bx.y);
        if (d2 <= l.w * l.w * 1.0001) ++n;
    }
    uint offset = n > 0u ? atomicAdd(n_total, n) : 0u;
    uint keep = offset >= u_layout.x ? 0u : min(n, u_layout.x - offset);
    if (keep < n) atomicAdd(n_dropped, n - keep);
    c_ranges[c] = uvec2(offset, keep);
    uint k = 0u;
    for (uint i = 0u; i < u_grid.w && k < keep; ++i) {
        vec4 l = c_lights[i].center_radius;
        float d2 = dist_sq_range(l.z, bz.x, bz.y) + dist_sq_range(l.y, by.x, by.y) + dist_sq_range(l.x, bx.x, bx.y);
        if (d2 <= l.w * l.w * 1.0001) l_indices[offset + k++] = c_lights[i].light.x;
    }
}
)";

#undef MCE_GLSL_OBJECT_U
#undef MCE_GLSL_POST_U
#undef MCE_GLSL_SKIN

}  // namespace mce::render::shaders3d
