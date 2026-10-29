#pragma once
// GLSL 450 of the 3D frame (Forward3D, ADR-0024). Uniform blocks follow the fixed bank table
// (rhi-renderer.md §3.3): FrameU 0, PassU 1, ObjectU 3; std430 Clusters 4, LightPool 5,
// LightIndices 6, JointPalette 7 (skinned vertex shaders); samplers albedo 6, normal 7, ORM 8,
// shadow cascades 9, sky 10, emissive 13; image-based lighting (M-VISUAL 2): pre-filtered sky 12,
// BRDF LUT 14, SH9 irradiance 15; screen-space ambient occlusion (M-VISUAL 3) 11; the irradiance
// volume (M-VISUAL 6) 5.
// The C++ mirrors of the blocks live in forward3d.cpp and must match member for member.

#include "shaders_cvd.h"

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
    vec4 p_ao;             // x: screen-space ambient occlusion on (M-VISUAL 3), y: contact shadows on (u_ao.g, M-VISUAL2 3)
    vec4 p_local;          // x: local-light shadows on (M-VISUAL 4), y: u_shadow layer of cascade 0
    vec4 p_gi;             // x: irradiance volume on (M-VISUAL 6), y: intensity, z: normal bias, w: fade distance
    vec4 p_gi_min;         // xyz: volume box
    vec4 p_gi_max;
    vec4 p_gi_counts;      // xyz: probes per axis
    vec4 p_vol;            // volumetric fog (M-VISUAL 7): x on, y slices, z max distance, w log2(1 + z / kVolD0)
    vec4 p_vol_grid;       // x, y: froxel columns and rows, z, w: atlas tiles across and down (a tile per slice)
    vec4 p_vol_medium;     // x: density, y: height falloff, z: base height, w: anisotropy g
    vec4 p_vol_albedo;     // rgb: scattering albedo, w: sun scale
    vec4 p_vol_scale;      // x: local light scale, y: ambient scale
    vec4 p_decals;         // x: decals to apply (M-VISUAL 9)
    vec4 p_ssr;            // x: screen-space reflections on (M-VISUAL2 1), y: max roughness, z: roughness fade, w: intensity
    vec4 p_sky_rot;        // M-VISUAL3 4: x cos, y sin of the sky texture's yaw about +Y (y = 0: no rotation)
    vec4 p_wet;            // M-WETSURFACE 5b: x wetness (0 = off), y puddle coverage, z 1 / puddle cell (m), w darken
    vec4 p_wet2;           // xy: the floating origin's xz mod 256 m, z: wet roughness, w: puddles on (0 on Low)
    vec4 p_snow;           // M-SNOWCOVER 6b: x cover (0 = off), y coverage, z 1 / noise cell (m; 0 = no noise, Low), w wind bias
    vec4 p_ripple;         // M-RAINFX 7b: x strength (0 = off), y density, z ring width (>= 0.01), w speed-scaled time mod 60 s
    vec4 p_snow2;          // M-WINDDIR 8c: xy the snow wind direction (xz unit vector the wind blows toward), zw 0
    vec4 p_cloud;          // M-CLOUDS 9b: x coverage (0 = off), y density, z 1 / noise cell (1/m), w plane height (m)
    vec4 p_cloud2;         // xy: the drift in noise cells mod 64 (wind_dir * speed * time), zw 0
    vec4 p_night;          // M-NIGHTSKY 14d: x moon cos size, y moon intensity (0 = no moon), z phase, w night fade
    vec4 p_night2;         // x star density (0 = no stars), y star brightness, z twinkle, w star time mod 60 s
    vec4 p_moon;           // xyz the unit direction toward the moon, w cos(2 pi phase)
    vec4 p_moon2;          // rgb the moon's tint, w sin(2 pi phase)
    vec4 p_water;          // M-WATER 15c: x level (local y), y wave count (0 = flat), z steepness, w half extent (m)
    vec4 p_water2;         // rgb the shallow tint, w depth scale (m)
    vec4 p_water3;         // rgb the deep tint, w reflectivity
    vec4 p_water4;         // x roughness, y foam amount, z foam width (m), w 1 = the opaque depth is bound (u_water_depth)
    vec4 p_water5;         // xy the two detail ripples' phases (rad), z detail slope (0 = none), w 0
    vec4 p_wave[8];        // per Gerstner wave: xy the wave vector k * dir on xz (rad/m), z amplitude (m), w phase (rad)
    vec4 p_aurora;         // M-AURORA 16c: x level (0 = off), y base height, z height span (sines), w time mod 64 s
    vec4 p_aurora2;        // xy the arc centre (xz unit vector), z cos of the arc's half-width, w curtain count (1..6)
    vec4 p_aurora3;        // rgb the low (base) colour, w 0
    vec4 p_aurora4;        // rgb the high (top) colour, w speed (0..4)
    vec4 p_free[12];       // unused, 0: PassU is 96 vec4 = 1536 bytes, a whole number of 256-byte slots (forward3d.cpp)
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
    vec3 dt = d;  // the direction in the texture's frame: yaw by -rotation, once, here (the gradient ignores it)
    if (p_sky_rot.y != 0.0) dt = vec3(p_sky_rot.x * d.x - p_sky_rot.y * d.z, d.y, p_sky_rot.y * d.x + p_sky_rot.x * d.z);
    vec3 tex = textureLod(u_sky, vec3(equirect_uv(dt), 0.0), min(lod, p_sky_ground.w)).rgb * p_sun.z;
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
    vec4 o_skin;       // x: first joint in the palette, -1 = rigid; instanced draws: y first visible slot, z time
    vec4 o_inst;       // instanced draws (M-VISUAL 9): x wind amplitude, y wind frequency, zw wind direction (xz)
    vec4 o_coat;       // M-VISUAL2 4: x clear coat, y its alpha (roughness²), z anisotropy, w its rotation (radians)
    vec4 o_sheen;      // rgb sheen color, w its alpha (roughness², at least 0.0078)
    vec4 o_ext;        // x: any of the lobes above on, y: index of refraction (0 = 1.5), z: transmission
    vec4 o_pom;        // M-VISUAL3 2: x parallax height scale (texture repeats; 0 = off), y/z min/max march steps, w fade-out view depth (m)
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
layout(location = 3) flat out vec4 v_tint;  // M-VISUAL3 6: per-copy colour variation (vec4(1) except batches)
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
    v_tint = vec4(1.0);
    gl_Position = u_view_proj * w;
}
)";

// Automatic instancing (M-VISUAL3 5a): a run of draws of one rigid submesh with one material reads its
// per-draw matrices from a transient storage block at bank 13 (Forward3D uploads it per pass); slot
// o_skin.y + gl_InstanceID is this copy's. kMeshVS's math with o_model / o_normal read per instance, so a
// run's pixels equal the single draws' bit for bit. Feeds kMeshFS and kAoPrepassFS (ObjectU carries the rest).
constexpr const char* kDrawInstancedVS = "#version 450 core\n" MCE_GLSL_FRAME_U MCE_GLSL_OBJECT_U R"(
struct DrawInst { mat4 model; mat4 normal; };
layout(std430, binding = 13) readonly buffer DrawInstances { DrawInst d_inst[]; };
layout(location = 0) in vec4 a_pos;
layout(location = 1) in vec2 a_oct;
layout(location = 2) in vec2 a_uv;
layout(location = 0) out vec3 v_world;
layout(location = 1) out vec3 v_normal;
layout(location = 2) out vec2 v_uv;
layout(location = 3) flat out vec4 v_tint;  // M-VISUAL3 6: per-copy colour variation (vec4(1) except batches)
out gl_PerVertex { vec4 gl_Position; };
vec3 oct_decode(vec2 e) {
    vec3 n = vec3(e, 1.0 - abs(e.x) - abs(e.y));
    if (n.z < 0.0) n.xy = (1.0 - abs(n.yx)) * vec2(n.x >= 0.0 ? 1.0 : -1.0, n.y >= 0.0 ? 1.0 : -1.0);
    return normalize(n);
}
void main() {
    uint k = uint(o_skin.y) + uint(gl_InstanceID);
    vec4 w = d_inst[k].model * vec4(a_pos.xyz, 1.0);
    v_world = w.xyz;
    v_normal = mat3(d_inst[k].normal) * oct_decode(a_oct);
    v_uv = o_uv_rect.xy + a_uv * o_uv_rect.zw;
    v_tint = vec4(1.0);
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
layout(location = 3) flat out vec4 v_tint;  // M-VISUAL3 6: per-copy colour variation (vec4(1) except batches)
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
    v_tint = vec4(1.0);
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

// The sun's cascades, the Forward+ lists and the local-light shadows (the lit passes and the
// volumetric fog's injection read them alike).
#define MCE_GLSL_LIGHTS R"(
layout(binding = 9) uniform sampler2DArrayShadow u_shadow;
layout(std430, binding = 4) readonly buffer Clusters { uvec2 c_ranges[]; };
struct Light { vec4 pos_radius; vec4 color; vec4 spot; vec4 shadow; };  // shadow: x first record (-1 = none), y records
layout(std430, binding = 5) readonly buffer LightPool { Light l_lights[]; };
// Local-light shadow records (M-VISUAL 4): one per atlas tile; rect = uv origin (xy) and size (zw) of
// the tile, info.x = u_shadow layer, info.y = world texel size per meter of distance.
struct LocalShadow { mat4 vp; vec4 rect; vec4 info; };
layout(std430, binding = 11) readonly buffer LocalShadows { LocalShadow l_shadows[]; };
layout(std430, binding = 6) readonly buffer LightIndices { uint l_indices[]; };

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
        for (int x = -1; x <= 1; ++x) sum += texture(u_shadow, vec4(c.xy + vec2(x, y) * ts, float(i) + p_local.y, min(c.z, 1.0)));
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

// ---- local-light shadows: a spot's tile, or the point light's cube face along the major axis
// (+X -X +Y -Y +Z -Z), with the sun's normal offset and PCF 3x3 clamped inside the tile.
float local_shadow(vec4 shadow, vec3 light_pos, vec3 world, vec3 n, vec3 l) {
    vec3 d = world - light_pos;
    int idx = int(shadow.x);
    if (shadow.y > 1.5) {
        vec3 a = abs(d);
        idx += a.x >= a.y && a.x >= a.z ? (d.x > 0.0 ? 0 : 1) : (a.y >= a.z ? (d.y > 0.0 ? 2 : 3) : (d.z > 0.0 ? 4 : 5));
    }
    LocalShadow s = l_shadows[idx];
    float texel = length(d) * s.info.y;
    float NoL = clamp(dot(n, l), 0.0, 1.0);
    vec3 p = world + n * (texel * (1.0 + 2.0 * sqrt(1.0 - NoL * NoL))) + l * texel;
    vec4 c = s.vp * vec4(p, 1.0);
    if (c.w <= 0.0) return 1.0;
    vec3 q = c.xyz / c.w * 0.5 + 0.5;
    if (any(lessThan(q.xy, vec2(0.0))) || any(greaterThan(q.xy, vec2(1.0)))) return 1.0;
    vec2 ts = 1.0 / vec2(textureSize(u_shadow, 0).xy);
    vec2 uv = s.rect.xy + q.xy * s.rect.zw;
    vec2 lo = s.rect.xy + ts, hi = s.rect.xy + s.rect.zw - ts;  // the bilinear 2x2 footprint stays in the tile
    float sum = 0.0;
    for (int y = -1; y <= 1; ++y)
        for (int x = -1; x <= 1; ++x) sum += texture(u_shadow, vec4(clamp(uv + vec2(x, y) * ts, lo, hi), s.info.x, min(q.z, 1.0)));
    return sum * (1.0 / 9.0);
}
)"

// Volumetric fog (M-VISUAL 7): the froxel grid's depth slices (log-spaced from the camera to
// p_vol.z) and its integrated light, an atlas of one tile per slice (rgb: the light scattered toward
// the camera up to the slice's far end, a: the transmittance there).
#define MCE_GLSL_VOLUME R"(
layout(binding = 4) uniform sampler2D u_vol;
const float kVolD0 = 1.0;  // meters: the slices thicken past about this depth
float vol_slice_of(float depth) { return log2(1.0 + max(depth, 0.0) / kVolD0) / p_vol.w * p_vol.y; }
float vol_depth_of(float s) { return kVolD0 * (exp2(s / p_vol.y * p_vol.w) - 1.0); }
vec2 vol_atlas_uv(vec2 uv, int slice) {
    int cols = int(p_vol_grid.z);
    vec2 g = p_vol_grid.xy;
    vec2 p = clamp(uv * g, vec2(0.5), g - 0.5);  // bilinear taps stay inside the slice's tile
    return (vec2(slice % cols, slice / cols) * g + p) / (g * p_vol_grid.zw);
}
// rgb: in-scattered light to add, a: transmittance to multiply, for a point at view depth `depth`
// seen through screen uv (0..1, the full-screen passes' convention).
vec4 vol_fog(vec2 uv, float depth) {
    float s = min(vol_slice_of(depth), p_vol.y);
    int k = int(floor(s));
    vec4 a = k > 0 ? textureLod(u_vol, vol_atlas_uv(uv, k - 1), 0.0) : vec4(0.0, 0.0, 0.0, 1.0);
    vec4 b = k < int(p_vol.y) ? textureLod(u_vol, vol_atlas_uv(uv, k), 0.0) : a;
    return mix(a, b, s - float(k));
}
)"

// The forward material shader. kTerrainFS is the same with MCE_TERRAIN: the surface comes from the
// splat layers (terrain_surface) instead of the material's maps.
#define MCE_GLSL_MESH_FS R"(
layout(binding = 6) uniform sampler2DArray u_albedo;
layout(binding = 7) uniform sampler2DArray u_normal_map;
layout(binding = 8) uniform sampler2DArray u_orm;
layout(binding = 13) uniform sampler2DArray u_emissive;
layout(binding = 11) uniform sampler2D u_ao;  // half-resolution SSAO, r = ambient visibility, g = the sun's contact visibility
layout(binding = 3) uniform sampler2D u_ssr;  // half-resolution SSR: rgb reflected light, a confidence
layout(location = 0) in vec3 v_world;
layout(location = 1) in vec3 v_normal;
layout(location = 2) in vec2 v_uv;
layout(location = 3) flat in vec4 v_tint;
layout(location = 0) out vec4 o_color;

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
// ---- M-VISUAL2 4 lobes (Filament's forms): anisotropic GGX along the UV tangent frame, the Charlie
// sheen with Neubelt's visibility (no energy compensation of the base), and a clear coat: a second
// GGX layer of F0 0.04 over the geometric normal that takes its Fresnel share from the base.
vec3 g_tangent = vec3(1.0, 0.0, 0.0), g_bitangent = vec3(0.0, 0.0, 1.0), g_coat_n = vec3(0.0, 1.0, 0.0);  // set by main() when o_ext.x is on
float D_GGX_Aniso(float NoH, float ToH, float BoH, float at, float ab) {
    float a2 = at * ab;
    vec3 d = vec3(ab * ToH, at * BoH, a2 * NoH);
    float b2 = a2 / max(dot(d, d), 1e-12);
    return a2 * b2 * b2 / PI;
}
float V_SmithGGXCorrelated_Aniso(float at, float ab, float ToV, float BoV, float ToL, float BoL, float NoV, float NoL) {
    float lv = NoL * length(vec3(at * ToV, ab * BoV, NoV));
    float ll = NoV * length(vec3(at * ToL, ab * BoL, NoL));
    return 0.5 / max(lv + ll, 1e-5);
}
float D_Charlie(float NoH, float a) {
    float inv = 1.0 / a;
    float sin2 = max(1.0 - NoH * NoH, 0.0078125);
    return (2.0 + inv) * pow(sin2, inv * 0.5) / (2.0 * PI);
}
float V_Neubelt(float NoV, float NoL) { return clamp(1.0 / (4.0 * (NoL + NoV - NoL * NoV)), 0.0, 1.0); }
vec3 direct_ext(vec3 n, vec3 v, vec3 l, vec3 diffuse, vec3 f0, float a, vec3 e) {
    vec3 h = normalize(v + l);
    float NoL = clamp(dot(n, l), 0.0, 1.0);
    float NoV = max(dot(n, v), 1e-4);
    float NoH = clamp(dot(n, h), 0.0, 1.0);
    float VoH = clamp(dot(v, h), 0.0, 1.0);
    vec3 c = vec3(0.0);
    if (NoL > 0.0) {
        vec3 F = F_Schlick(f0, VoH);
        vec3 spec;
        if (o_coat.z != 0.0) {
            float at = max(a * (1.0 + o_coat.z), 0.002), ab = max(a * (1.0 - o_coat.z), 0.002);
            spec = D_GGX_Aniso(NoH, dot(g_tangent, h), dot(g_bitangent, h), at, ab) *
                   V_SmithGGXCorrelated_Aniso(at, ab, dot(g_tangent, v), dot(g_bitangent, v), dot(g_tangent, l), dot(g_bitangent, l), NoV, NoL) * F;
        } else {
            spec = D_GGX(NoH, a) * V_SmithGGXCorrelated(NoV, NoL, a) * F;
        }
        c = diffuse * (1.0 - F) / PI + spec;
        if (o_sheen.r + o_sheen.g + o_sheen.b > 0.0) c += o_sheen.rgb * D_Charlie(NoH, o_sheen.w) * V_Neubelt(NoV, NoL);
        c *= PI * NoL;
    }
    if (o_coat.x > 0.0) {
        float cNoL = clamp(dot(g_coat_n, l), 0.0, 1.0);
        float Fc = F_Schlick(vec3(0.04), VoH).x * o_coat.x;
        c *= 1.0 - Fc;
        if (cNoL > 0.0) {
            float cNoV = max(dot(g_coat_n, v), 1e-4), cNoH = clamp(dot(g_coat_n, h), 0.0, 1.0);
            c += vec3(D_GGX(cNoH, o_coat.y) * V_SmithGGXCorrelated(cNoV, cNoL, o_coat.y) * Fc * PI * cNoL);
        }
    }
    return c * e;
}
vec3 shade(vec3 n, vec3 v, vec3 l, vec3 diffuse, vec3 f0, float a, vec3 e) {
    return o_ext.x > 0.5 ? direct_ext(n, v, l, diffuse, f0, a, e) : direct(n, v, l, diffuse, f0, a, e);
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
// Normal-map decode (M-CONTENT 1b). BC5 samples as (x, y, 0, 1): z is rebuilt from x and y. Only a texel with
// b == 0.0 takes that path; RGBA8 maps (b >= 128 in practice) return t.xyz * 2 - 1 with the same bits as before. (That holds for blue > 0; a blue == 0 texel,
// RGBA8 or BC5, gets its z rebuilt.)
vec3 unpack_normal(vec4 t) {
    vec3 n = t.xyz * 2.0 - 1.0;
    if (t.b == 0.0) n.z = sqrt(max(1.0 - dot(n.xy, n.xy), 0.0));
    return n;
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

// ---- the irradiance volume (M-VISUAL 6): 4 texels per probe, the probe's light at the tetrahedron
// directions kGiTetra (each the sphere integral of L * (1 + w.d) / 4pi). E(n) / pi = sum V_k (1/4 + 3/2 d_k.n)
// is the L1 irradiance of the probe's view; the 8 probes around a point blend by trilinear weight
// times a smooth backface term (a probe behind the surface counts little).
layout(binding = 5) uniform sampler2D u_gi;
const vec3 kGiTetra[4] = vec3[4](vec3(0.57735027, 0.57735027, 0.57735027), vec3(0.57735027, -0.57735027, -0.57735027),
                                 vec3(-0.57735027, 0.57735027, -0.57735027), vec3(-0.57735027, -0.57735027, 0.57735027));
vec3 gi_probe(ivec3 c, vec3 n) {
    ivec2 t = ivec2(c.x * 4, c.y + int(p_gi_counts.y) * c.z);
    vec3 e = vec3(0.0);
    for (int k = 0; k < 4; ++k) e += texelFetch(u_gi, t + ivec2(k, 0), 0).rgb * (0.25 + 1.5 * dot(kGiTetra[k], n));
    return max(e, vec3(0.0));
}
// rgb: irradiance / pi at p along n; a: how much of it applies (1 inside the box, 0 past the fade).
vec4 gi_irradiance(vec3 p, vec3 n) {
    vec3 bmin = p_gi_min.xyz, bmax = p_gi_max.xyz;
    vec3 inside = min(p - bmin, bmax - p);
    float fade = clamp(1.0 + min(min(inside.x, inside.y), inside.z) / max(p_gi.w, 1e-3), 0.0, 1.0);
    if (fade <= 0.0) return vec4(0.0);
    ivec3 counts = ivec3(p_gi_counts.xyz);
    vec3 cell = (bmax - bmin) / vec3(counts - 1);
    vec3 g = (clamp(p + n * p_gi.z, bmin, bmax) - bmin) / cell;
    ivec3 base = clamp(ivec3(floor(g)), ivec3(0), counts - 2);
    vec3 f = clamp(g - vec3(base), 0.0, 1.0);
    vec3 sum = vec3(0.0);
    float wsum = 0.0;
    for (int i = 0; i < 8; ++i) {
        ivec3 o = ivec3(i & 1, (i >> 1) & 1, i >> 2);
        vec3 tw = mix(1.0 - f, f, vec3(o));
        float w = tw.x * tw.y * tw.z;
        vec3 to = bmin + vec3(base + o) * cell - p;
        float d2 = dot(to, to);
        float b = d2 > 1e-8 ? (dot(to * inversesqrt(d2), n) + 1.0) * 0.5 : 1.0;
        w *= b * b + 0.05;
        sum += gi_probe(base + o, n) * w;
        wsum += w;
    }
    return vec4(sum / max(wsum, 1e-6), fade);
}

uint cluster_of(float depth) {
    uvec3 g = uvec3(u_cluster.xyz);
    uint tx = min(uint(gl_FragCoord.x * u_viewport.z * u_cluster.x), g.x - 1u);
    uint ty = min(uint(gl_FragCoord.y * u_viewport.w * u_cluster.y), g.y - 1u);
    int s = int(floor(log(max(depth, 1e-4)) * u_cluster_z.x + u_cluster_z.y));
    uint tz = uint(clamp(s, 0, int(g.z) - 1));
    return tx + g.x * (ty + g.y * tz);
}

// ---- projected decals (M-VISUAL 9): up to p_decals.x boxes in submission order, each blending its
// color over the surface's base color and its roughness / metallic over the surface's, before lighting.
// A smooth decal also fills the surface's normal-map bumps (`fill`: a puddle over paving is a flat
// mirror; without it a low roughness lights each bump's rim as a dotted pattern).
struct DecalRec {
    mat4 inv_world;  // world -> the unit box
    vec4 uv_rect;
    vec4 color;      // rgb, a: opacity
    vec4 params;     // x: roughness, y: metallic, z: material weight, w: emissive
    vec4 fade;       // x: angle fade (cos), y: edge fade (of the half height), z: texture slot (-1 = none)
    vec4 axis;       // xyz: the projection's up axis in world space (local +Y)
};
layout(std430, binding = 15) readonly buffer Decals { DecalRec d_decals[]; };
layout(binding = 0) uniform sampler2DArray u_decal0;
layout(binding = 1) uniform sampler2DArray u_decal1;
layout(binding = 2) uniform sampler2DArray u_decal2;
vec4 decal_texture(int slot, vec2 uv, vec2 gx, vec2 gy) {
    if (slot == 0) return textureGrad(u_decal0, vec3(uv, 0.0), gx, gy);
    if (slot == 1) return textureGrad(u_decal1, vec3(uv, 0.0), gx, gy);
    return textureGrad(u_decal2, vec3(uv, 0.0), gx, gy);
}
// `ng`: the geometric normal; derivatives come from outside the loop (its branches diverge).
void apply_decals(vec3 p, vec3 ng, inout vec3 albedo, inout float rough, inout float metallic, inout vec3 emit, inout float fill) {
    vec3 dpx = dFdx(p), dpy = dFdy(p);
    int count = int(p_decals.x);
    for (int i = 0; i < count; ++i) {
        vec3 l = (d_decals[i].inv_world * vec4(p, 1.0)).xyz;
        if (any(greaterThan(abs(l), vec3(0.5)))) continue;
        vec4 fade = d_decals[i].fade;
        float a = d_decals[i].color.a * clamp((dot(ng, d_decals[i].axis.xyz) - fade.x) * 10.0, 0.0, 1.0);
        if (fade.y > 0.0) a *= clamp((0.5 - abs(l.y)) / (0.5 * fade.y), 0.0, 1.0);
        if (a <= 0.0) continue;
        vec4 r = d_decals[i].uv_rect;
        vec2 uv = r.xy + (l.xz + 0.5) * r.zw;
        vec4 tex = vec4(1.0);
        if (fade.z >= 0.0) {
            mat3 m = mat3(d_decals[i].inv_world);
            tex = decal_texture(int(fade.z), uv, (m * dpx).xz * r.zw, (m * dpy).xz * r.zw);
        }
        a *= tex.a;
        vec3 c = tex.rgb * d_decals[i].color.rgb;
        vec4 q = d_decals[i].params;
        albedo = mix(albedo, c, a);
        rough = mix(rough, q.x, a * q.z);
        metallic = mix(metallic, q.y, a * q.z);
        fill = mix(fill, 1.0, a * q.z * (1.0 - q.x));
        emit += c * (q.w * a);
    }
}

// ---- wet surfaces (M-WETSURFACE 5b), behind p_wet.x > 0. Value noise on integer lattice hashes (no sin():
// its precision differs across GPUs), periodic over 256 m: each octave fits a whole number of cells into that
// span, so the floating origin enters mod 256 m and the puddles stay put across a rebase.
uint wet_hash(uvec2 v) {  // pcg2d (Jarzynski and Olano 2020), one lane
    v = v * 1664525u + 1013904223u;
    v.x += v.y * 1664525u;
    v.y += v.x * 1664525u;
    v ^= v >> 16u;
    v.x += v.y * 1664525u;
    v.y += v.x * 1664525u;
    v ^= v >> 16u;
    return v.x;
}
float wet_lattice(uvec2 c, uint seed) { return float(wet_hash(c + uvec2(seed, 0u)) >> 8u) * (1.0 / 16777216.0); }
// q: meters in 0..256; `cells`: lattice cells across 256 m (a whole number, >= 1).
float wet_value(vec2 q, float cells, uint seed) {
    vec2 g = max(q * (cells / 256.0), vec2(0.0));
    vec2 i = floor(g);
    vec2 f = g - i;
    f = f * f * (3.0 - 2.0 * f);
    uint m = uint(cells);
    uvec2 c0 = uvec2(i) % m;  // g may round up to `cells`: it wraps to cell 0
    uvec2 c1 = (c0 + 1u) % m;
    float a = wet_lattice(c0, seed), b = wet_lattice(uvec2(c1.x, c0.y), seed);
    float c = wet_lattice(uvec2(c0.x, c1.y), seed), d = wet_lattice(c1, seed);
    return mix(mix(a, b, f.x), mix(c, d, f.x), f.y);
}
// Puddle cover 0..1 at world point p: two octaves (1 and 2.03 per puddle cell, each rounded to whole cells per
// 256 m), crevices (low ao) fill first,
// only on up-facing surfaces (`ng`: the geometric normal). The threshold falls as wetness and coverage rise.
float wet_puddle(vec3 p, vec3 ng, float ao) {
    float up = smoothstep(0.9, 0.98, ng.y);
    if (up <= 0.0) return 0.0;
    vec2 q = mod(p.xz + p_wet2.xy, 256.0);
    float c1 = max(floor(256.0 * p_wet.z + 0.5), 1.0), c2 = max(floor(256.0 * 2.03 * p_wet.z + 0.5), 1.0);
    float h = wet_value(q, c1, 0u) * 0.65 + wet_value(q, c2, 0x9E3779B9u) * 0.35 + (1.0 - ao) * 0.25;
    float t = 1.0 - 0.8 * p_wet.y * p_wet.x;
    return smoothstep(t - 0.03, t + 0.03, h) * up;
}
// Darker porous albedo and lower roughness; puddles (standing water) darken the dielectric surface under them once
// more, ask for roughness 0.03 (the lighting's clamp to 0.045 below wins, so they draw at 0.045) and flatten the
// normal map (`fill`). Metals neither darken (their albedo is their F0) nor smooth: water beads on them, and the
// gate holds a wet metal sphere's look within 1%. Returns the puddle cover (the rain ripples' mask).
float apply_wet(vec3 p, vec3 ng, float ao, inout vec3 albedo, inout float rough, float metallic, inout float fill) {
    float w = p_wet.x;
    float pud = (p_wet2.w > 0.5 && p_wet.y > 0.0) ? wet_puddle(p, ng, ao) : 0.0;
    float dielectric = 1.0 - clamp(metallic, 0.0, 1.0);
    float porosity = dielectric * clamp(rough * 1.25, 0.0, 1.0);
    albedo *= mix(1.0, 1.0 - p_wet.w, w * porosity) * mix(1.0, 1.0 - p_wet.w, pud * dielectric);
    rough = mix(rough, min(rough, p_wet2.z), w * dielectric);
    rough = mix(rough, 0.03, pud);
    fill = max(fill, pud);
    return pud;
}

// ---- snow cover (M-SNOWCOVER 6b), behind p_snow.x > 0, after apply_wet: the snow's values replace whatever the
// water did under it, so snow wins over puddles where its mask is high. The mask, at world point p with geometric
// normal ng (the interpolated vertex normal, the puddles' stand-in: a sphere's cap is up-facing and collects snow):
//   up  = smoothstep(0.55, 0.85, ng.y)                     slopes up to ~32 degrees fully, none past ~57
//   c   = clamp(cover * coverage * (1 + wind_bias * dot(ng.xz, wind_dir)), 0, 1)
//         the share of the surface to whiten; the wind blows toward wind_dir (p_snow2.xy, M-WINDDIR 8c; +x by
//         default), so slopes facing that way (lee) take up to 1 + wind_bias times the share and those facing into
//         it (windward) down to 1 - wind_bias; flat ground (ng.xz = 0) is unchanged. With wind_dir (1, 0) the dot
//         is ng.x exactly (ng.x * 1 + ng.z * 0), so the default frame is the pre-8c one bit for bit.
//   noise (p_snow.z > 0): h = the two-octave value noise of the puddles (1 and 2.03 per cell, other seeds),
//         s = c - 0.5, t = 0.5 - s (0.48 + 2.55 s^4) (a fit of the noise's quantiles: about the share c of points
//         has h > t), mask = smoothstep(t - 0.04, t + 0.04, h) * min(4 c, 1) (the factor fades the last
//         percent in from cover 0) * up
//   no noise (Low): mask = c * up, an even layer of the same mean whiteness.
float snow_mask(vec3 p, vec3 ng) {
    float up = smoothstep(0.55, 0.85, ng.y);
    if (up <= 0.0) return 0.0;
    float c = clamp(p_snow.x * p_snow.y * (1.0 + p_snow.w * dot(ng.xz, p_snow2.xy)), 0.0, 1.0);
    if (p_snow.z <= 0.0) return c * up;
    vec2 q = mod(p.xz + p_wet2.xy, 256.0);
    float c1 = max(floor(256.0 * p_snow.z + 0.5), 1.0), c2 = max(floor(256.0 * 2.03 * p_snow.z + 0.5), 1.0);
    float h = wet_value(q, c1, 0x85EBCA6Bu) * 0.65 + wet_value(q, c2, 0xC2B2AE35u) * 0.35;
    float s = c - 0.5;
    float t = 0.5 - s * (0.48 + 2.55 * s * s * s * s);
    return smoothstep(t - 0.04, t + 0.04, h) * min(4.0 * c, 1.0) * up;
}
// Under the mask: albedo toward 0.9 white, roughness toward 0.8, metallic toward 0 (snow on metal is snow), and
// the normal map flattens (`fill`: the layer hides the surface's relief). Snow over a puddle stills it: `pud`, the
// rain ripples' mask, keeps only the uncovered share.
void apply_snow(vec3 p, vec3 ng, inout vec3 albedo, inout float rough, inout float metallic, inout float fill,
                inout float pud) {
    float m = snow_mask(p, ng);
    albedo = mix(albedo, vec3(0.9), m);
    rough = mix(rough, 0.8, m);
    metallic = mix(metallic, 0.0, m);
    fill = max(fill, m);
    pud *= 1.0 - m;
}

// ---- rain ripples (M-RAINFX 7b), behind p_ripple.x > 0 and only where a puddle lies (`pud`, apply_wet's cover
// after the snow): a frame with no wetness, no puddles or no ripples is the ripple-less one bit for bit, and ground
// outside the puddles never changes. Drops land on a grid of 0.5 m cells, 512 across the puddles' 256 m period (the
// same origin p_wet2.xy, so a floating-origin rebase keeps them in place). Cell c holds a drop when its hash is
// below the density; its period is 3..6 quarter seconds and its phase hashed. In each cycle the drop lands at a
// hashed point of its cell (the cycle index wraps with the clock: p_ripple.w is the speed-scaled time mod 60 s,
// which every period divides) and two concentric rings grow from it, radius u * 0.8 and u * 0.44 cells for the
// cycle's age u = 0..1, fading in over the first tenth and out as (1 - u)^2. A ring is the slope of a smooth bump
// (1 - y^2)^2, y = (distance - radius) / (half width), half width ring_width * 0.3 cells: the longest reach,
// 0.9 + 0.8 + 0.3 cells from a cell's corner, stays inside the 3 x 3 cells searched. No derivatives: the branch is
// not uniform.
float ripple_ring(float x, float hw) {
    float y = x / hw;
    return abs(y) < 1.0 ? y * (1.0 - y * y) * 2.598 : 0.0;  // the outward slope, peak 1 at y = 1 / sqrt(3)
}
vec2 ripple_tilt(vec3 p) {
    vec2 g = mod(p.xz + p_wet2.xy, 256.0) * 2.0;  // cells
    vec2 i = floor(g);
    float t4 = p_ripple.w * 4.0;  // quarter seconds, 0..240
    float hw = p_ripple.z * 0.3;
    vec2 tilt = vec2(0.0);
    for (int dy = -1; dy <= 1; ++dy)
        for (int dx = -1; dx <= 1; ++dx) {
            vec2 cell = i + vec2(float(dx), float(dy));
            uvec2 c = uvec2(mod(cell, 512.0));
            uint h = wet_hash(c + uvec2(0x68E31DA4u, 0xB5297A4Du));
            if (float(h & 0xFFFFu) * (1.0 / 65536.0) >= p_ripple.y) continue;
            uint k = 3u + ((h >> 16u) & 3u);
            float tp = t4 / float(k) + float(h >> 18u) * (1.0 / 16384.0);  // cycles
            float cyc = floor(tp);
            float u = tp - cyc;
            uint cy = uint(cyc) % (240u / k);
            uint h2 = wet_hash(uvec2(c.x ^ (cy * 0x27D4EB2Du), c.y + 0x165667B1u));
            vec2 drop = cell + 0.1 + 0.8 * vec2(float(h2 & 0xFFFFu), float(h2 >> 16u)) * (1.0 / 65536.0);
            vec2 d = g - drop;
            float dist = length(d);
            if (dist < 1e-4) continue;
            float a = smoothstep(0.0, 0.1, u) * (1.0 - u) * (1.0 - u);
            float s = ripple_ring(dist - 0.8 * u, hw) + 0.5 * ripple_ring(dist - 0.44 * u, hw);
            tilt += d * (a * s / dist);
        }
    return tilt;
}
// The rings tilt the water's normal by up to ~0.4 rad per ring and roughen it a little at their crests.
void apply_ripples(vec3 p, float pud, inout vec3 n, inout float rough) {
    vec2 t = ripple_tilt(p) * (p_ripple.x * pud);
    n = normalize(n + vec3(t.x, 0.0, t.y) * 0.4);
    rough += 0.04 * min(length(t), 1.0);
}

#ifdef MCE_TERRAIN
// ---- splat-layered terrain (M-TERRAIN 2): the layers' maps are layers of u_albedo / u_normal_map /
// u_orm, the weights two RGBA layers of u_emissive (no emissive on a terrain), the parameters TerrainU.
layout(std140, binding = 2) uniform TerrainU {
    mat4 t_to_local;     // world -> terrain space (the layers tile in its xz metres)
    vec4 t_size;         // xy: size along x and z, z: splat samples per side, w: layer count
    vec4 t_flags;        // x: triplanar allowed, y: height blend allowed, z: height blend depth
    vec4 t_color[8];     // rgb: base color, a: albedo map
    vec4 t_params[8];    // x: 1 / tile, y: roughness, z: metallic, w: height strength
    vec4 t_maps[8];      // x: normal map, y: ORM map, z: triplanar, w: occlusion in ORM.r
};
// One layer's map: projected along y, or along x, y and z blended by `bw` (zero weights skip their fetch).
// Explicit gradients: the layer loop branches per fragment.
vec4 terrain_fetch(sampler2DArray t, vec3 p, vec3 dx, vec3 dy, float layer, float s, vec3 bw) {
    vec4 c = vec4(0.0);
    if (bw.y > 0.0) c += textureGrad(t, vec3(p.xz * s, layer), dx.xz * s, dy.xz * s) * bw.y;
    if (bw.x > 0.0) c += textureGrad(t, vec3(p.zy * s, layer), dx.zy * s, dy.zy * s) * bw.x;
    if (bw.z > 0.0) c += textureGrad(t, vec3(p.xy * s, layer), dx.xy * s, dy.xy * s) * bw.z;
    return c;
}
// A layer's normal map through each projection, reoriented onto the surface normal ("whiteout" blend).
vec3 terrain_normal(vec3 p, vec3 dx, vec3 dy, float layer, float s, vec3 bw, vec3 n) {
    vec3 r = vec3(0.0);
    if (bw.y > 0.0) {
        vec3 t = unpack_normal(textureGrad(u_normal_map, vec3(p.xz * s, layer), dx.xz * s, dy.xz * s));
        r += vec3(t.x + n.x, abs(t.z) * n.y, n.z - t.y) * bw.y;
    }
    if (bw.x > 0.0) {
        vec3 t = unpack_normal(textureGrad(u_normal_map, vec3(p.zy * s, layer), dx.zy * s, dy.zy * s));
        r += vec3(abs(t.z) * n.x, n.y - t.y, t.x + n.z) * bw.x;
    }
    if (bw.z > 0.0) {
        vec3 t = unpack_normal(textureGrad(u_normal_map, vec3(p.xy * s, layer), dx.xy * s, dy.xy * s));
        r += vec3(t.x + n.x, n.y - t.y, abs(t.z) * n.z) * bw.z;
    }
    return r;
}
void terrain_surface(vec3 world, vec3 n, out vec3 albedo, out vec3 nrm, out float rough, out float metallic, out float ao) {
    vec3 p = (t_to_local * vec4(world, 1.0)).xyz;
    vec3 dx = dFdx(p), dy = dFdy(p);
    float res = t_size.z;
    vec2 suv = (p.xz / t_size.xy + 0.5) * ((res - 1.0) / res) + 0.5 / res;  // sample centres span the extent
    vec4 s0 = textureLod(u_emissive, vec3(suv, 0.0), 0.0), s1 = textureLod(u_emissive, vec3(suv, 1.0), 0.0);
    float w[8] = float[8](s0.x, s0.y, s0.z, s0.w, s1.x, s1.y, s1.z, s1.w);
    int count = int(t_size.w);
    vec3 tri = pow(abs(n), vec3(4.0));
    tri /= max(tri.x + tri.y + tri.z, 1e-5);
    tri = mix(vec3(0.0, 1.0, 0.0), tri, step(0.02, 1.0 - tri.y));  // nearly flat: the top projection alone
    vec3 bws[8];
    vec4 col[8];
    float sum = 0.0;
    for (int i = 0; i < count; ++i) {
        sum += w[i];
        bws[i] = (t_flags.x > 0.5 && t_maps[i].z > 0.5) ? tri : vec3(0.0, 1.0, 0.0);
        col[i] = vec4(1.0);
        if (w[i] > 0.002 && t_color[i].a > 0.5) col[i] = terrain_fetch(u_albedo, p, dx, dy, float(i), t_params[i].x, bws[i]);
        col[i].rgb *= t_color[i].rgb;
    }
    for (int i = 0; i < count; ++i) w[i] /= max(sum, 1e-5);
    if (t_flags.y > 0.5) {  // height blend: the layer that rises highest above its weight wins at the seam
        float top = -1e9;
        float h[8];
        for (int i = 0; i < count; ++i) {
            h[i] = w[i] > 0.002 ? w[i] + dot(col[i].rgb, vec3(0.2126, 0.7152, 0.0722)) * t_params[i].w * t_flags.z : -1e9;
            top = max(top, h[i]);
        }
        sum = 0.0;
        for (int i = 0; i < count; ++i) {
            w[i] = max(h[i] - (top - t_flags.z), 0.0);
            sum += w[i];
        }
        for (int i = 0; i < count; ++i) w[i] /= max(sum, 1e-5);
    }
    albedo = vec3(0.0);
    nrm = vec3(0.0);
    rough = 0.0;
    metallic = 0.0;
    ao = 0.0;
    for (int i = 0; i < count; ++i) {
        if (w[i] <= 0.002) continue;
        float wi = w[i];
        albedo += col[i].rgb * wi;
        float r = t_params[i].y, m = t_params[i].z, o = 1.0;
        if (t_maps[i].y > 0.5) {
            vec3 orm = terrain_fetch(u_orm, p, dx, dy, float(i), t_params[i].x, bws[i]).rgb;
            if (t_maps[i].w > 0.5) o = orm.r;
            r *= orm.g;
            m *= orm.b;
        }
        rough += r * wi;
        metallic += m * wi;
        ao += o * wi;
        nrm += (t_maps[i].x > 0.5 ? normalize(terrain_normal(p, dx, dy, float(i), t_params[i].x, bws[i], n)) : n) * wi;
    }
    nrm = dot(nrm, nrm) > 1e-8 ? normalize(nrm) : n;
}
#endif

// A material map at the surface's uv. Where parallax moved it, the uv steps from march layer to march layer, so
// its own screen derivatives (what texture() takes) are noise: the maps there take the mesh uv's footprint, and
// filter it anisotropically here rather than in the sampler. The sampler's anisotropic filter (max_aniso 4) is
// implementation-defined: on the grazing, high-contrast surfaces parallax is for, llvmpipe and an RTX 3070 differed
// by 1.8% of full scale (a 4x vs 16x setting moves llvmpipe as far). Up to 4 trilinear taps along the footprint's
// major axis, at the level of its width over the tap count: the same shape, the same result on every GPU.
// Elsewhere (no parallax, or faded out) texture() as ever; both are read so the implicit-derivative one stays in
// uniform control flow.
vec2 g_uv_dx, g_uv_dy;
bool g_pom_moved = false;
vec4 map_aniso(sampler2DArray s, vec2 at, vec2 dx, vec2 dy) {  // dx, dy: the uv's screen gradients
    vec2 size = vec2(textureSize(s, 0).xy);
    float lx = length(dx * size), ly = length(dy * size);
    vec2 major = lx > ly ? dx : dy;
    float lmaj = max(lx, ly), lmin = max(min(lx, ly), 1e-6);
    float taps = clamp(ceil(lmaj / lmin), 1.0, 4.0);
    float lod = max(0.0, log2(lmaj / taps));
    vec4 sum = vec4(0.0);
    for (int i = 0; i < 4; ++i) {
        if (float(i) >= taps) break;
        sum += textureLod(s, vec3(at + major * ((float(i) + 0.5) / taps - 0.5), 0.0), lod);
    }
    return sum / taps;
}
#define MCE_MAP(s, at) (o_pom.x > 0.0 ? mix(texture(s, vec3(at, 0.0)), map_aniso(s, at, g_uv_dx, g_uv_dy), bvec4(g_pom_moved)) : texture(s, vec3(at, 0.0)))

#ifndef MCE_TERRAIN
// Parallax occlusion mapping (M-VISUAL3 2): the height map is layer 1 of u_normal_map (white = high). The
// view ray is marched through o_pom.y..z layers of the height field in the surface's uv frame, then the last
// two layers are intersected linearly. The tangent frame solves the uv Jacobian from screen derivatives, so
// anisotropic uv scales and mirrored uvs shift correctly. No self-shadowing, no depth offset.
vec2 parallax_uv(vec3 p, vec3 n, vec2 uv, float view_depth) {
    float scale = o_pom.x * (1.0 - smoothstep(0.75 * o_pom.w, o_pom.w, view_depth));
    if (scale <= 0.0) return uv;
    vec3 dp1 = dFdx(p), dp2 = dFdy(p);
    vec2 duv1 = dFdx(uv), duv2 = dFdy(uv);
    float det = duv1.x * duv2.y - duv1.y * duv2.x;
    if (abs(det) < 1e-20) return uv;
    vec3 t = (dp1 * duv2.y - dp2 * duv1.y) / det;  // d p / d u
    vec3 b = (dp2 * duv1.x - dp1 * duv2.x) / det;  // d p / d v
    float tt = dot(t, t), bb = dot(b, b), tb = dot(t, b);
    float gram = tt * bb - tb * tb;
    if (gram < 1e-20) return uv;
    vec3 v = normalize(u_camera_pos.xyz - p);
    float vn = dot(v, n);
    if (vn <= 0.0) return uv;
    vec3 vt = v - n * vn;
    float vu = dot(vt, t), vv = dot(vt, b);
    vec2 dir = vec2(vu * bb - vv * tb, vv * tt - vu * tb) / gram;  // uv displacement per meter along the view's tangential part
    float depth_m = scale * 0.5 * (sqrt(tt) + sqrt(bb));              // the deepest point, in meters
    vec2 total = dir * depth_m / max(vn, 0.05);
    float layers = floor(mix(o_pom.z, o_pom.y, vn) + 0.5);
    vec2 step_uv = total / layers;
    float step_d = 1.0 / layers;
    // The height field at one isotropic level (the footprint's longer axis): textureGrad here took the sampler's
    // anisotropic filter, which is the driver's own (see map_aniso), and costs taps in a loop of up to 64.
    vec2 hsize = vec2(textureSize(u_normal_map, 0).xy);
    float h_lod = max(0.0, log2(max(length(duv1 * hsize), length(duv2 * hsize))));
    vec2 cur = uv, prev = uv;
    float cur_d = 0.0, prev_d = 0.0;
    float tex_d = 1.0 - textureLod(u_normal_map, vec3(cur, 1.0), h_lod).r;
    float prev_tex_d = tex_d;
    for (int i = 0; i < 64; ++i) {
        if (cur_d >= tex_d || float(i) >= layers) break;
        prev = cur;
        prev_d = cur_d;
        prev_tex_d = tex_d;
        cur -= step_uv;
        cur_d += step_d;
        tex_d = 1.0 - textureLod(u_normal_map, vec3(cur, 1.0), h_lod).r;
    }
    float after = tex_d - cur_d, before = prev_tex_d - prev_d;
    float w = (after - before) != 0.0 ? clamp(after / (after - before), 0.0, 1.0) : 0.0;
    g_pom_moved = true;
    return mix(cur, prev, w);
}
#endif

void main() {
    vec3 n = normalize(v_normal);
    if (!gl_FrontFacing) n = -n;
    vec2 uv = v_uv;
    g_uv_dx = dFdx(v_uv);
    g_uv_dy = dFdy(v_uv);
#ifdef MCE_TERRAIN
    vec4 base = vec4(1.0);
    vec3 t_n;
    float t_rough, t_metallic, t_ao;
    terrain_surface(v_world, n, base.rgb, t_n, t_rough, t_metallic, t_ao);
    base *= o_base_color;
#else
    if (o_pom.x > 0.0) uv = parallax_uv(v_world, n, v_uv, -(u_view * vec4(v_world, 1.0)).z);  // uniform branch: it takes derivatives
    vec4 base = MCE_MAP(u_albedo, uv) * o_base_color * v_tint;
    if (o_params.x > 0.0 && base.a < o_params.x) discard;  // alpha mask
#endif
    vec3 color;
    vec3 decal_emit = vec3(0.0);
    if (o_params.y > 0.5) {
        if (p_decals.x > 0.5) {
            float r0 = 1.0, m0 = 0.0, f0 = 0.0;
            apply_decals(v_world, n, base.rgb, r0, m0, decal_emit, f0);
        }
        color = base.rgb;  // unlit
    } else {
#ifdef MCE_TERRAIN
        float rough = t_rough, metallic = t_metallic, ao = t_ao;
#else
        float rough = o_params.z, metallic = o_params.w, ao = 1.0;
#endif
        if (o_maps.y > 0.5) {
            vec3 orm = MCE_MAP(u_orm, uv).rgb;
            if (o_maps.z > 0.5) ao = orm.r;
            if (o_maps.w < 0.5) {
                rough *= orm.g;
                metallic *= orm.b;
            }
        }
        float fill = 0.0, pud = 0.0;
        if (p_decals.x > 0.5) apply_decals(v_world, n, base.rgb, rough, metallic, decal_emit, fill);
        if (p_wet.x > 0.0) pud = apply_wet(v_world, n, ao, base.rgb, rough, metallic, fill);  // n: still the geometric normal
        if (p_snow.x > 0.0) apply_snow(v_world, n, base.rgb, rough, metallic, fill, pud);  // after the water: snow wins
#ifdef MCE_TERRAIN
        n = fill > 0.0 ? normalize(mix(t_n, n, fill)) : t_n;
#else
        if (o_maps.x > 0.5) {  // uniform branch: perturb_normal takes derivatives
            vec3 nb = perturb_normal(n, v_world, v_uv, unpack_normal(MCE_MAP(u_normal_map, uv)));
            n = normalize(mix(nb, n, fill));
        }
#endif
        if (p_ripple.x > 0.0 && pud > 0.0) apply_ripples(v_world, pud, n, rough);  // on the water the snow left
        rough = clamp(rough, 0.045, 1.0);
        metallic = clamp(metallic, 0.0, 1.0);
        float a = rough * rough;
        vec3 diffuse = base.rgb * (1.0 - metallic);
        float trans = o_ext.w > 0.5 ? o_ext.z * (1.0 - metallic) : 0.0;  // transmission takes the diffuse's place
        diffuse *= 1.0 - trans;
        float ior_f0 = o_ext.y > 0.0 ? ((o_ext.y - 1.0) / (o_ext.y + 1.0)) * ((o_ext.y - 1.0) / (o_ext.y + 1.0)) : 0.04;
        vec3 f0 = mix(vec3(ior_f0), base.rgb, metallic);
        vec3 v = normalize(u_camera_pos.xyz - v_world);
        if (o_ext.x > 0.5) {  // uniform branch: the tangent frame takes derivatives
            g_coat_n = normalize(v_normal) * (gl_FrontFacing ? 1.0 : -1.0);
            vec3 dp1 = dFdx(v_world), dp2 = dFdy(v_world);
            vec2 duv1 = dFdx(v_uv), duv2 = dFdy(v_uv);
            vec3 t = cross(dp2, n) * duv1.x + cross(n, dp1) * duv2.x;
            t -= n * dot(n, t);
            t = dot(t, t) > 1e-20 ? normalize(t) : normalize(abs(n.y) < 0.99 ? cross(vec3(0.0, 1.0, 0.0), n) : cross(vec3(1.0, 0.0, 0.0), n));
            vec3 b = cross(n, t);
            g_tangent = t * cos(o_coat.w) + b * sin(o_coat.w);
            g_bitangent = cross(n, g_tangent);
        }
        float depth = -(u_view * vec4(v_world, 1.0)).z;

        // The sun, shadowed.
        vec3 l = -p_light_dir.xyz;
        float sh = dot(n, l) > 0.0 ? shadow_term(v_world, normalize(v_normal) * (gl_FrontFacing ? 1.0 : -1.0), l, depth) : 0.0;
        if (p_ao.y > 0.5 && o_ext.w < 0.5 && sh > 0.0) {  // contact shadows: what the cascades are too coarse to see
            vec4 c = u_view_proj * vec4(v_world, 1.0);
            sh *= textureLod(u_ao, c.xy / c.w * 0.5 + 0.5, 0.0).g;
        }
        color = shade(n, v, l, diffuse, f0, a, p_light_color.rgb) * sh;

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
            if (L.shadow.x >= 0.0 && p_local.x > 0.5) {
                att *= local_shadow(L.shadow, L.pos_radius.xyz, v_world, normalize(v_normal) * (gl_FrontFacing ? 1.0 : -1.0), ld);
                if (att <= 0.0) continue;
            }
            color += shade(n, v, ld, diffuse, f0, a, L.color.rgb * att);
        }

        // Ambient: hemisphere diffuse + sky reflections through the analytic split-sum, or with
        // image-based lighting the SH9 sky irradiance + the pre-filtered sky through the BRDF LUT.
        float NoV = max(dot(n, v), 1e-4);
        vec3 r = reflect(-v, n);
        if (o_ext.x > 0.5 && o_coat.z != 0.0) {  // anisotropy bends the reflection toward the stretch (Filament)
            vec3 dir = o_coat.z >= 0.0 ? g_bitangent : g_tangent;
            vec3 an = cross(cross(dir, v), dir);
            r = reflect(-v, normalize(mix(n, an, abs(o_coat.z) * clamp(5.0 * rough, 0.0, 1.0))));
        }
        float horizon = clamp(1.0 + dot(r, normalize(v_normal)), 0.0, 1.0);
        vec3 amb_diffuse = p_ibl.x > 0.5 ? sh_irradiance(n) * diffuse * p_ibl.y : mix(p_amb_ground.rgb, p_amb_sky.rgb, n.y * 0.5 + 0.5) * diffuse;
        vec3 amb_spec, spec_brdf;
        // The irradiance volume: its light in place of the sky's, and the sky's reflections dimmed by how
        // much of the sky's irradiance reaches this point (rooms do not mirror the open sky).
        float gi_w = 0.0, sky_vis = 1.0;
        if (p_gi.x > 0.5) {
            vec4 gi = gi_irradiance(v_world, n);
            gi_w = gi.a;
            vec3 irr = gi.rgb * p_gi.y;
            vec3 sky_irr = p_ibl.x > 0.5 ? sh_irradiance(n) * p_ibl.y : mix(p_amb_ground.rgb, p_amb_sky.rgb, n.y * 0.5 + 0.5);
            amb_diffuse = mix(amb_diffuse, irr * diffuse, gi_w);
            const vec3 kLuma = vec3(0.2126, 0.7152, 0.0722);
            sky_vis = mix(1.0, clamp(dot(irr, kLuma) / max(dot(sky_irr, kLuma), 1e-4), 0.0, 1.0), gi_w);
        }
        if (p_ibl.x > 0.5) {
            vec2 ab = textureLod(u_brdf_lut, vec2(NoV, rough), 0.0).rg;
            vec3 env = prefiltered_sky(r, rough) * sky_vis;
            if (p_probes.x > 0.5) env = probe_specular(v_world, r, rough, env);
            spec_brdf = f0 * ab.x + ab.y;
            amb_spec = env * spec_brdf * p_sky_horizon.w * horizon * horizon;
        } else {
            spec_brdf = env_brdf(f0, rough, NoV);
            amb_spec = sky_radiance(r, rough * 8.0) * sky_vis * spec_brdf * p_sky_horizon.w * horizon * horizon;
        }
        if (p_ssr.x > 0.5) {  // what the screen saw along r, over the probes, by confidence and roughness
            vec4 c = u_view_proj * vec4(v_world, 1.0);
            vec4 s = textureLod(u_ssr, c.xy / c.w * 0.5 + 0.5, 0.0);
            float w = s.a * clamp((p_ssr.y - rough) / p_ssr.z, 0.0, 1.0);
            amb_spec = mix(amb_spec, s.rgb * spec_brdf * p_ssr.w, w);
        }
        if (p_ao.x > 0.5 && o_ext.w < 0.5) {  // SSAO darkens the ambient only; the screen position as the full-screen passes see it
            vec4 c = u_view_proj * vec4(v_world, 1.0);
            ao *= textureLod(u_ao, c.xy / c.w * 0.5 + 0.5, 0.0).r;
        }
        // The sky's flat ambient is darkened away from the sun; the volume's light is not (it saw the shadows).
        float sun_side = mix(mix(0.55, 1.0, sh * clamp(dot(n, l), 0.0, 1.0)), 1.0, gi_w);
        if (o_ext.x > 0.5) {
            if (o_sheen.r + o_sheen.g + o_sheen.b > 0.0) {  // the sheen under the ambient: the irradiance times an approximate albedo
                vec3 irr = p_ibl.x > 0.5 ? sh_irradiance(n) * p_ibl.y : mix(p_amb_ground.rgb, p_amb_sky.rgb, n.y * 0.5 + 0.5);
                if (p_gi.x > 0.5) irr = mix(irr, gi_irradiance(v_world, n).rgb * p_gi.y, gi_w);
                amb_diffuse += irr * o_sheen.rgb * mix(0.1, 0.5, pow(1.0 - NoV, 2.0)) * (1.0 - o_sheen.w * 0.5);
            }
            if (o_coat.x > 0.0) {  // the coat reflects the sky (not the probes or SSR); the base below loses its share
                float cNoV = max(dot(g_coat_n, v), 1e-4);
                float Fc = (0.04 + 0.96 * pow(1.0 - cNoV, 5.0)) * o_coat.x;
                vec3 rc = reflect(-v, g_coat_n);
                float c_rough = sqrt(o_coat.y);
                vec3 env = p_ibl.x > 0.5 ? prefiltered_sky(rc, c_rough) : sky_radiance(rc, c_rough * 8.0);
                amb_diffuse *= 1.0 - Fc;  // the direct light took its share in direct_ext
                amb_spec = amb_spec * (1.0 - Fc) + env * sky_vis * Fc * p_sky_horizon.w;
            }
        }
        color += (amb_diffuse * sun_side + amb_spec) * ao;
        if (trans > 0.0) {  // thin-walled refraction: the opaque frame (bank kAo), shifted by the view normal, tinted
            vec4 c = u_view_proj * vec4(v_world, 1.0);
            float ior = o_ext.y > 0.0 ? o_ext.y : 1.5;
            vec2 shift = (mat3(u_view) * n).xy * (1.0 - 1.0 / ior) * 0.1;
            vec3 bg = textureLod(u_ao, clamp(c.xy / c.w * 0.5 + 0.5 - shift, 0.0, 1.0), 0.0).rgb;
            vec3 F = f0 + (1.0 - f0) * pow(1.0 - NoV, 5.0);
            color += bg * base.rgb * trans * (1.0 - F);
        }
    }
    vec3 emissive = o_emissive.rgb;
    if (o_emissive.w > 0.5) emissive *= MCE_MAP(u_emissive, uv).rgb;
    color += emissive + decal_emit;
    if (p_vol.x > 0.5) {  // volumetric fog in place of the exp² fog
        vec4 c = u_view_proj * vec4(v_world, 1.0);
        vec4 f = vol_fog(c.xy / c.w * 0.5 + 0.5, -(u_view * vec4(v_world, 1.0)).z);
        color = color * f.a + f.rgb;
    } else {
        float dist = length(v_world - u_camera_pos.xyz);
        float fog = 1.0 - exp(-(p_fog.w * dist) * (p_fog.w * dist));
        color = mix(color, p_fog.rgb, clamp(fog, 0.0, 1.0));
    }
    o_color = vec4(color, base.a);
}
)"
constexpr const char* kMeshFS = "#version 450 core\n" MCE_GLSL_FRAME_U MCE_GLSL_PASS_U MCE_GLSL_OBJECT_U MCE_GLSL_IBL MCE_GLSL_LIGHTS MCE_GLSL_VOLUME MCE_GLSL_MESH_FS;
constexpr const char* kTerrainFS = "#version 450 core\n#define MCE_TERRAIN 1\n" MCE_GLSL_FRAME_U MCE_GLSL_PASS_U MCE_GLSL_OBJECT_U MCE_GLSL_IBL MCE_GLSL_LIGHTS MCE_GLSL_VOLUME MCE_GLSL_MESH_FS;

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

// Automatic instancing (M-VISUAL3 5a): per copy the premultiplied cascade or tile MVP the CPU computes for
// a single draw (not view_proj x model on the GPU), slot s_skin.y + gl_InstanceID, so the depth is the same.
constexpr const char* kShadowInstancedVS = R"(#version 450 core
layout(std140, binding = 3) uniform ShadowU { mat4 s_mvp; mat4 s_dequant; vec4 s_skin; };
layout(std430, binding = 13) readonly buffer ShadowInstances { mat4 d_mvp[]; };
layout(location = 0) in vec4 a_pos;
out gl_PerVertex { vec4 gl_Position; };
void main() { gl_Position = d_mvp[uint(s_skin.y) + uint(gl_InstanceID)] * vec4(a_pos.xyz, 1.0); }
)";

constexpr const char* kShadowFS = R"(#version 450 core
void main() {}
)";

// ---- temporal anti-aliasing (M-VISUAL 5) -----------------------------------------------------------
// Velocity prepass: the opaques again into a full-size RGBA16F target with their own depth; rg = this
// pixel's motion in uv since last frame (current minus previous, both unjittered), b = 1 where a mesh
// was drawn (the TAA resolve reprojects the rest as sky at infinity).
#define MCE_GLSL_VELOCITY_U R"(
layout(std140, binding = 1) uniform VelPassU { mat4 vp_jittered; mat4 vp_current; mat4 vp_previous; };
layout(std140, binding = 3) uniform VelObjU { mat4 m_current; mat4 m_previous; mat4 m_dequant; vec4 m_skin; };
layout(location = 0) in vec4 a_pos;
layout(location = 0) out vec4 v_current;
layout(location = 1) out vec4 v_previous;
out gl_PerVertex { vec4 gl_Position; };
)"
constexpr const char* kVelocityVS = "#version 450 core\n" MCE_GLSL_VELOCITY_U R"(
void main() {
    vec4 p = vec4(a_pos.xyz, 1.0);
    vec4 w = m_current * p;
    gl_Position = vp_jittered * w;
    v_current = vp_current * w;
    v_previous = vp_previous * (m_previous * p);
}
)";
// Skinned twin: m_skin.x = this frame's first joint, y = last frame's (-1 = rigid).
constexpr const char* kSkinnedVelocityVS = "#version 450 core\n" MCE_GLSL_VELOCITY_U MCE_GLSL_SKIN R"(
void main() {
    vec4 p = m_dequant * vec4(a_pos.xyz, 1.0);
    vec4 w = m_current * (skin_matrix(m_skin.x) * p);
    gl_Position = vp_jittered * w;
    v_current = vp_current * w;
    v_previous = vp_previous * (m_previous * (skin_matrix(m_skin.y) * p));
}
)";
// Automatic instancing (M-VISUAL3 5a): the model of each copy at slot m_skin.z + gl_InstanceID. Runs never
// hold a moving draw, so last frame's model is this frame's.
constexpr const char* kVelocityInstancedVS = "#version 450 core\n" MCE_GLSL_VELOCITY_U R"(
layout(std430, binding = 13) readonly buffer VelocityInstances { mat4 d_model[]; };
void main() {
    vec4 p = vec4(a_pos.xyz, 1.0);
    mat4 m = d_model[uint(m_skin.z) + uint(gl_InstanceID)];
    vec4 w = m * p;
    gl_Position = vp_jittered * w;
    v_current = vp_current * w;
    v_previous = vp_previous * (m * p);
}
)";
#undef MCE_GLSL_VELOCITY_U
constexpr const char* kVelocityFS = R"(#version 450 core
layout(location = 0) in vec4 v_current;
layout(location = 1) in vec4 v_previous;
layout(location = 0) out vec4 o_velocity;
void main() {
    // Behind last frame's camera: no history to reuse (a motion that throws the lookup off screen).
    // a: view depth, for depth of field (M-VISUAL 8).
    if (v_previous.w <= 1e-4) { o_velocity = vec4(16.0, 16.0, 1.0, v_current.w); return; }
    o_velocity = vec4((v_current.xy / v_current.w - v_previous.xy / v_previous.w) * 0.5, 1.0, v_current.w);
}
)";

// TAA resolve: this frame's sample under each output pixel (point, at the jittered position), the
// history reprojected by the velocity (5-tap Catmull-Rom), clipped to the 3x3 neighborhood's variance
// box in YCoCg over Karis-weighted colors, blended by `blend`. When the output is larger than the
// input (temporal upscaling), the new sample's weight falls off with its distance to the pixel.
constexpr const char* kTaaFS = R"(#version 450 core
layout(std140, binding = 1) uniform TaaU {
    mat4 t_reproject;  // last frame's view-proj * inverse(this frame's), both unjittered: the sky
    vec4 t_in;         // input texel size (xy) and size (zw)
    vec4 t_out;        // output texel size (xy) and size (zw)
    vec4 t_jitter;     // xy: this frame's jitter in uv, z: history valid, w: blend
};
layout(binding = 6) uniform sampler2D u_current;   // HDR, input size, point sampled
layout(binding = 7) uniform sampler2D u_history;   // last output, linear
layout(binding = 8) uniform sampler2D u_velocity;  // input size, point sampled
layout(location = 0) in vec2 v_uv;
layout(location = 0) out vec4 o_color;
vec3 tm(vec3 c) { return c / (1.0 + max(c.r, max(c.g, c.b))); }
vec3 itm(vec3 c) { return c / max(1.0 - max(c.r, max(c.g, c.b)), 1e-4); }
vec3 to_ycocg(vec3 c) { return vec3(dot(c, vec3(0.25, 0.5, 0.25)), dot(c, vec3(0.5, 0.0, -0.5)), dot(c, vec3(-0.25, 0.5, -0.25))); }
vec3 to_rgb(vec3 c) { return vec3(c.x + c.y - c.z, c.x + c.z, c.x - c.y - c.z); }
vec3 history(vec2 uv) {  // Catmull-Rom from 5 bilinear taps (the corners dropped)
    vec2 pos = uv * t_out.zw;
    vec2 c = floor(pos - 0.5) + 0.5;
    vec2 f = pos - c;
    vec2 w0 = f * (-0.5 + f * (1.0 - 0.5 * f));
    vec2 w1 = 1.0 + f * f * (-2.5 + 1.5 * f);
    vec2 w2 = f * (0.5 + f * (2.0 - 1.5 * f));
    vec2 w3 = f * f * (-0.5 + 0.5 * f);
    vec2 w12 = w1 + w2;
    vec2 t12 = (c + w2 / w12) * t_out.xy, t0 = (c - 1.0) * t_out.xy, t3 = (c + 2.0) * t_out.xy;
    vec3 r = textureLod(u_history, vec2(t12.x, t0.y), 0.0).rgb * (w12.x * w0.y) +
             textureLod(u_history, vec2(t0.x, t12.y), 0.0).rgb * (w0.x * w12.y) +
             textureLod(u_history, t12, 0.0).rgb * (w12.x * w12.y) +
             textureLod(u_history, vec2(t3.x, t12.y), 0.0).rgb * (w3.x * w12.y) +
             textureLod(u_history, vec2(t12.x, t3.y), 0.0).rgb * (w12.x * w3.y);
    float w = w12.x * w0.y + w0.x * w12.y + w12.x * w12.y + w3.x * w12.y + w12.x * w3.y;
    return max(r / w, vec3(0.0));
}
void main() {
    vec2 uv = v_uv;
    // The input texel this pixel's point was rasterized in (the projection was shifted by the jitter).
    vec2 cuv = (floor((uv + t_jitter.xy) * t_in.zw) + 0.5) * t_in.xy;
    vec3 current = to_ycocg(tm(max(textureLod(u_current, cuv, 0.0).rgb, vec3(0.0))));
    vec3 m1 = vec3(0.0), m2 = vec3(0.0), lo = vec3(1e9), hi = vec3(-1e9);
    for (int y = -1; y <= 1; ++y)
        for (int x = -1; x <= 1; ++x) {
            vec3 s = to_ycocg(tm(max(textureLod(u_current, cuv + vec2(x, y) * t_in.xy, 0.0).rgb, vec3(0.0))));
            m1 += s;
            m2 += s * s;
            lo = min(lo, s);
            hi = max(hi, s);
        }
    vec3 mu = m1 * (1.0 / 9.0);
    vec3 sigma = sqrt(max(m2 * (1.0 / 9.0) - mu * mu, vec3(0.0)));
    lo = max(lo, mu - 1.25 * sigma);
    hi = min(hi, mu + 1.25 * sigma);

    vec4 vel = textureLod(u_velocity, cuv, 0.0);
    vec2 prev;
    if (vel.b > 0.5) {
        prev = uv - vel.xy;
    } else {
        vec4 p = t_reproject * vec4(uv * 2.0 - 1.0, 1.0, 1.0);
        prev = p.xy / p.w * 0.5 + 0.5;
    }
    float alpha = t_jitter.w;
    if (t_out.z > t_in.z + 0.5) {  // upscaling: trust the new sample by how close it landed
        vec2 d = (cuv - t_jitter.xy - uv) * t_out.zw;
        alpha = clamp(alpha * 2.0 * exp(-1.5 * dot(d, d)), 0.02, 1.0);
    }
    if (t_jitter.z < 0.5 || any(lessThan(prev, vec2(0.0))) || any(greaterThan(prev, vec2(1.0)))) alpha = 1.0;
    vec3 h = to_ycocg(tm(history(prev)));
    vec3 centre = 0.5 * (lo + hi), extent = 0.5 * (hi - lo) + 1e-5;
    vec3 off = h - centre;
    vec3 a = abs(off / extent);
    float m = max(a.x, max(a.y, a.z));
    if (m > 1.0) h = centre + off / m;
    o_color = vec4(itm(max(to_rgb(mix(h, current, alpha)), vec3(0.0))), 1.0);
}
)";

// ---- screen-space ambient occlusion (M-VISUAL 3), all at half resolution --------------------------
// Prepass: the opaques' view-space normal (xyz) and view depth (w; 0 = nothing drawn), over kMeshVS
// or kSkinnedMeshVS, honouring alpha masks.
constexpr const char* kAoPrepassFS = "#version 450 core\n" MCE_GLSL_FRAME_U MCE_GLSL_OBJECT_U R"(
layout(binding = 6) uniform sampler2DArray u_albedo;
layout(location = 0) in vec3 v_world;
layout(location = 1) in vec3 v_normal;
layout(location = 2) in vec2 v_uv;
layout(location = 0) out vec4 o_gbuf;
void main() {
    if (o_params.x > 0.0 && texture(u_albedo, vec3(v_uv, 0.0)).a * o_base_color.a < o_params.x) discard;
    vec3 n = normalize(v_normal);
    if (!gl_FrontFacing) n = -n;
    o_gbuf = vec4(normalize(mat3(u_view) * n), -(u_view * vec4(v_world, 1.0)).z);
}
)";

#define MCE_GLSL_AO_U R"(
layout(std140, binding = 1) uniform AoU {
    mat4 a_proj;
    vec4 a_params;  // x: radius (m), y: intensity, z: power, w: bias (cosine)
    vec4 a_step;    // xy: blur step in uv (0 for the occlusion pass)
};
)"

// Alchemy-style obscurance (McGuire et al. 2011) in a cosine form: 12 samples on a 3-turn spiral of
// the projected radius, rotated per pixel by a 4x4 Bayer pattern the blur then removes. Each
// sample in reach occludes by the cosine between the normal and the direction to it.
constexpr const char* kAoFS = "#version 450 core\n" MCE_GLSL_AO_U R"(
layout(binding = 6) uniform sampler2D u_gbuf;
layout(location = 0) in vec2 v_uv;
layout(location = 0) out vec4 o_ao;
vec3 view_pos(vec2 uv, float z) {
    vec2 ndc = uv * 2.0 - 1.0;
    return vec3((ndc.x + a_proj[2][0]) * z / a_proj[0][0], (ndc.y + a_proj[2][1]) * z / a_proj[1][1], -z);
}
void main() {
    vec4 g = textureLod(u_gbuf, v_uv, 0.0);
    if (g.w <= 0.0) {
        o_ao = vec4(1.0);
        return;
    }
    vec3 p = view_pos(v_uv, g.w);
    vec3 n = normalize(g.xyz);
    float r = a_params.x;
    vec2 ruv = 0.5 * abs(vec2(a_proj[0][0], a_proj[1][1])) * r / g.w;
    const float bayer[16] = float[16](0.0, 8.0, 2.0, 10.0, 12.0, 4.0, 14.0, 6.0, 3.0, 11.0, 1.0, 9.0, 15.0, 7.0, 13.0, 5.0);
    ivec2 q = ivec2(gl_FragCoord.xy) & 3;
    float rot = bayer[q.x * 4 + q.y] * (6.2831853 / 16.0);
    vec2 size = vec2(textureSize(u_gbuf, 0));
    const int N = 12;
    float sum = 0.0;
    for (int i = 0; i < N; ++i) {
        float t = (float(i) + 0.5) / float(N);
        float ang = t * (6.2831853 * 3.0) + rot;
        // Snapped to the texel centre the depth belongs to: off-centre, a tilted plane occludes itself.
        vec2 uv = (floor((v_uv + vec2(cos(ang), sin(ang)) * ruv * t) * size) + 0.5) / size;
        vec4 s = textureLod(u_gbuf, uv, 0.0);
        if (s.w <= 0.0) continue;
        vec3 v = view_pos(uv, s.w) - p;
        float vv = dot(v, v);
        if (vv >= r * r || vv <= 1e-8) continue;
        float cosine = dot(v, n) * inversesqrt(vv);
        sum += max(cosine - a_params.w, 0.0) * (1.0 - vv / (r * r));
    }
    float vis = clamp(1.0 - a_params.y * 2.0 * sum / float(N), 0.0, 1.0);
    o_ao = vec4(pow(vis, a_params.z));
}
)";

// Depth-aware separable blur: 9 Gaussian taps along a_step, each weighted down by its view-depth
// difference from the centre, so occlusion does not bleed across silhouettes.
constexpr const char* kAoBlurFS = "#version 450 core\n" MCE_GLSL_AO_U R"(
layout(binding = 6) uniform sampler2D u_ao_src;
layout(binding = 7) uniform sampler2D u_gbuf;
layout(location = 0) in vec2 v_uv;
layout(location = 0) out vec4 o_ao;
void main() {
    float z = textureLod(u_gbuf, v_uv, 0.0).w;
    float c = textureLod(u_ao_src, v_uv, 0.0).r;
    if (z <= 0.0) {
        o_ao = vec4(c);
        return;
    }
    const float w[5] = float[5](0.2270270, 0.1945946, 0.1216216, 0.0540541, 0.0162162);
    float sum = c * w[0], wsum = w[0];
    for (int i = 1; i <= 4; ++i) {
        for (int sgn = -1; sgn <= 1; sgn += 2) {
            vec2 uv = v_uv + a_step.xy * float(i * sgn);
            float zi = textureLod(u_gbuf, uv, 0.0).w;
            float wd = w[i] * max(0.0, 1.0 - abs(zi - z) / (0.05 * z));
            sum += textureLod(u_ao_src, uv, 0.0).r * wd;
            wsum += wd;
        }
    }
    o_ao = vec4(sum / wsum);
}
)";

// ---- screen-space reflections (M-VISUAL2 1), at half resolution ----------------------------------
// Each pixel of the SSAO prepass marches its mirror ray in screen space (uv and 1/z interpolated, so
// the steps are perspective-correct), jittered per pixel by a 4x4 Bayer pattern, until it passes less
// than `thickness` (plus the step's own depth travel) behind the depth buffer; 4 bisections refine the hit. The hit's view position is
// reprojected into last frame, whose lit HDR color is read there. a = confidence: 0 for a miss, the
// sky or a point last frame did not see, fading toward the screen's edges, the ray's
// end and rays that turn toward the camera.
constexpr const char* kSsrFS = R"(#version 450 core
layout(std140, binding = 1) uniform SsrU {
    mat4 r_proj;       // this frame's projection, unjittered
    mat4 r_reproject;  // view space -> last frame's clip space
    vec4 r_params;     // x: max distance (m), y: thickness (m), z: steps, w: edge fade (uv)
    vec4 r_misc;       // x: last frame valid
};
layout(binding = 6) uniform sampler2D u_gbuf;  // view normal (xyz), view depth (w; 0 = nothing drawn)
layout(binding = 7) uniform sampler2D u_prev;  // last frame's HDR color
layout(location = 0) in vec2 v_uv;
layout(location = 0) out vec4 o_ssr;
vec3 view_pos(vec2 uv, float z) {
    vec2 ndc = uv * 2.0 - 1.0;
    return vec3((ndc.x + r_proj[2][0]) * z / r_proj[0][0], (ndc.y + r_proj[2][1]) * z / r_proj[1][1], -z);
}
vec2 project(vec3 p) {
    vec4 c = r_proj * vec4(p, 1.0);
    return c.xy / c.w * 0.5 + 0.5;
}
void main() {
    o_ssr = vec4(0.0);
    vec4 g = textureLod(u_gbuf, v_uv, 0.0);
    if (r_misc.x < 0.5 || g.w <= 0.0) return;
    vec3 p = view_pos(v_uv, g.w);
    vec3 n = normalize(g.xyz);
    vec3 d = reflect(normalize(p), n);
    float facing = 1.0 - smoothstep(0.0, 0.6, d.z);  // toward the camera: what it hits is mostly unseen
    if (facing <= 0.0) return;
    float len = r_params.x;
    const float kNear = 0.05;
    if (d.z > 0.0 && p.z + d.z * len > -kNear) len = (-kNear - p.z) / d.z;  // stop before the camera
    vec3 e = p + d * len;
    vec2 uv0 = v_uv, uv1 = project(e);
    float iz0 = 1.0 / g.w, iz1 = 1.0 / -e.z;
    vec2 du = uv1 - uv0;
    float tmax = 1.0;  // where the segment leaves the screen
    if (du.x > 0.0) tmax = min(tmax, (1.0 - uv0.x) / du.x);
    else if (du.x < 0.0) tmax = min(tmax, -uv0.x / du.x);
    if (du.y > 0.0) tmax = min(tmax, (1.0 - uv0.y) / du.y);
    else if (du.y < 0.0) tmax = min(tmax, -uv0.y / du.y);
    const float bayer[16] = float[16](0.0, 8.0, 2.0, 10.0, 12.0, 4.0, 14.0, 6.0, 3.0, 11.0, 1.0, 9.0, 15.0, 7.0, 13.0, 5.0);
    ivec2 q = ivec2(gl_FragCoord.xy) & 3;
    float jitter = (bayer[q.x * 4 + q.y] + 0.5) / 16.0;
    int steps = int(r_params.z);
    float lo = 0.0, t = 0.0, prev_rz = g.w;
    bool hit = false;
    for (int i = 0; i < 128 && i < steps; ++i) {
        t = tmax * (float(i) + jitter) / float(steps);
        float rz = 1.0 / mix(iz0, iz1, t);
        vec4 sg = textureLod(u_gbuf, mix(uv0, uv1, t), 0.0);
        float sz = sg.w, dz = rz - sz;
        // Behind the depth buffer by less than the thickness plus this step's own depth travel, at a
        // surface facing the ray (one the ray leaves, such as its own start at a grazing angle, is not a hit).
        if (sz > 0.0 && dz > 0.0 && dz < r_params.y + abs(rz - prev_rz) && dot(sg.xyz, d) < 0.0) {
            hit = true;
            break;
        }
        lo = t;
        prev_rz = rz;
    }
    if (!hit) return;
    float hi = t;
    for (int k = 0; k < 4; ++k) {
        float m = 0.5 * (lo + hi);
        float sz = textureLod(u_gbuf, mix(uv0, uv1, m), 0.0).w;
        if (sz > 0.0 && 1.0 / mix(iz0, iz1, m) > sz) hi = m;
        else lo = m;
    }
    vec2 huv = mix(uv0, uv1, hi);
    vec4 hg = textureLod(u_gbuf, huv, 0.0);
    if (hg.w <= 0.0 || dot(hg.xyz, d) >= 0.0) return;  // refined onto a surface the ray leaves
    vec4 c = r_reproject * vec4(view_pos(huv, hg.w), 1.0);
    if (c.w <= 1e-4) return;
    vec2 puv = c.xy / c.w * 0.5 + 0.5;
    if (any(lessThan(puv, vec2(0.0))) || any(greaterThan(puv, vec2(1.0)))) return;
    vec2 edge = min(min(huv, 1.0 - huv), min(puv, 1.0 - puv));
    float fade = r_params.w > 0.0 ? clamp(min(edge.x, edge.y) / r_params.w, 0.0, 1.0) : 1.0;
    fade *= facing * (1.0 - smoothstep(0.75, 1.0, hi));
    o_ssr = vec4(max(textureLod(u_prev, puv, 0.0).rgb, vec3(0.0)), fade);
}
)";

// ---- contact shadows (M-VISUAL2 3), at half resolution ------------------------------------------
// Each pixel of the SSAO prepass marches c_params.x meters toward the sun from just off its surface,
// jittered per pixel by a 4x4 Bayer pattern. A sample that passes behind the depth buffer by more than
// half a texel's footprint and less than the thickness (plus a texel's) occludes; the occlusion fades toward the screen's
// edges and the ray's far end. Out: r = the SSAO visibility (or 1), g = the sun's contact visibility.
constexpr const char* kContactFS = R"(#version 450 core
layout(std140, binding = 1) uniform ContactU {
    mat4 c_proj;     // this frame's projection, unjittered
    vec4 c_dir;      // xyz: toward the sun, view space
    vec4 c_params;   // x: length (m), y: thickness (m), z: steps, w: intensity
    vec4 c_misc;     // x: copy u_ao_src.r
};
layout(binding = 6) uniform sampler2D u_gbuf;    // view normal (xyz), view depth (w; 0 = nothing drawn)
layout(binding = 7) uniform sampler2D u_ao_src;  // the blurred SSAO
layout(location = 0) in vec2 v_uv;
layout(location = 0) out vec4 o_contact;
vec3 view_pos(vec2 uv, float z) {
    vec2 ndc = uv * 2.0 - 1.0;
    return vec3((ndc.x + c_proj[2][0]) * z / c_proj[0][0], (ndc.y + c_proj[2][1]) * z / c_proj[1][1], -z);
}
void main() {
    float ao = c_misc.x > 0.5 ? textureLod(u_ao_src, v_uv, 0.0).r : 1.0;
    o_contact = vec4(ao, 1.0, 0.0, 1.0);
    vec4 g = textureLod(u_gbuf, v_uv, 0.0);
    if (g.w <= 0.0) return;
    vec3 n = normalize(g.xyz), l = c_dir.xyz;
    if (dot(n, l) <= 0.0) return;  // facing away: the sun's own N·L leaves it dark
    vec2 size = vec2(textureSize(u_gbuf, 0));
    // The bias grows with distance: a half-size texel covers more of the surface there.
    float texel = 2.0 * g.w / (abs(c_proj[1][1]) * size.y);
    vec3 p = view_pos(v_uv, g.w) + n * texel;
    float bias = 0.5 * texel, thickness = c_params.y + texel;
    const float bayer[16] = float[16](0.0, 8.0, 2.0, 10.0, 12.0, 4.0, 14.0, 6.0, 3.0, 11.0, 1.0, 9.0, 15.0, 7.0, 13.0, 5.0);
    ivec2 q = ivec2(gl_FragCoord.xy) & 3;
    float jitter = (bayer[q.x * 4 + q.y] + 0.5) / 16.0;
    int steps = int(c_params.z);
    float occ = 0.0;
    for (int i = 0; i < 64 && i < steps; ++i) {
        float t = (float(i) + jitter) / float(steps);
        vec3 s = p + l * (c_params.x * t);
        if (s.z >= -0.05) break;  // past the camera
        vec4 c = c_proj * vec4(s, 1.0);
        vec2 uv = c.xy / c.w * 0.5 + 0.5;
        if (any(lessThan(uv, vec2(0.0))) || any(greaterThan(uv, vec2(1.0)))) break;
        // Snapped to the texel centre the depth belongs to, as the SSAO does.
        float sz = textureLod(u_gbuf, (floor(uv * size) + 0.5) / size, 0.0).w;
        float dz = -s.z - sz;
        if (sz > 0.0 && dz > bias && dz < thickness) {
            vec2 edge = min(uv, 1.0 - uv);
            occ = clamp(min(edge.x, edge.y) / 0.05, 0.0, 1.0) * (1.0 - smoothstep(0.6, 1.0, t));
            break;
        }
    }
    o_contact.g = 1.0 - occ * c_params.w;
}
)";

// A plain copy of the opaque frame for transmissive draws to refract (M-VISUAL2 4, single-sample targets).
constexpr const char* kFrameCopyFS = R"(#version 450 core
layout(binding = 6) uniform sampler2D u_src;
layout(location = 0) in vec2 v_uv;
layout(location = 0) out vec4 o;
void main() { o = textureLod(u_src, v_uv, 0.0); }
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

// The sky's layers over sky_radiance(), shared by the sky pass (kSkyFS) and the water's reflection (kWaterFS, M-WATER
// 15c): the cloud layer, the stars and the moon, and the aurora (M-AURORA 16c). Functions of a direction (and, for the
// night layer and the aurora, the angular size of a pixel along it), so the water reflects them through the same code
// rather than a copy.
#define MCE_GLSL_SKY_LAYERS \
    R"(
// ---- the cloud layer (M-CLOUDS 9b), behind p_cloud.x > 0 (coverage; 0 = the cloudless frame bit for bit). The view
// ray meets a plane p_cloud.w meters above the camera (up-going rays only), camera-relative: the camera's xz and the
// floating origin never enter, only the drift p_cloud2.xy (noise cells, wrapped on the CPU to the 64-cell period).
// Three octaves of value noise on integer lattice hashes (pcg2d, the puddle and snow noise's family; no sin()), at
// 1, 2 and 4 times the cell rate, each a whole lattice per period, so the pattern is seamless and precision-stable.
uint cloud_hash(uvec2 v) {  // pcg2d (Jarzynski and Olano 2020), one lane
    v = v * 1664525u + 1013904223u;
    v.x += v.y * 1664525u;
    v.y += v.x * 1664525u;
    v ^= v >> 16u;
    v.x += v.y * 1664525u;
    v.y += v.x * 1664525u;
    v ^= v >> 16u;
    return v.x;
}
float cloud_lattice(uvec2 c, uint seed) { return float(cloud_hash(c + uvec2(seed, 0u)) >> 8u) * (1.0 / 16777216.0); }
// g: noise coordinates in 0..m cells (m: lattice cells per period).
float cloud_value(vec2 g, uint m, uint seed) {
    vec2 i = floor(g);
    vec2 f = g - i;
    f = f * f * (3.0 - 2.0 * f);
    uvec2 c0 = uvec2(max(i, vec2(0.0))) % m;  // g may round up to m: it wraps to cell 0
    uvec2 c1 = (c0 + 1u) % m;
    float a = cloud_lattice(c0, seed), b = cloud_lattice(uvec2(c1.x, c0.y), seed);
    float c = cloud_lattice(uvec2(c0.x, c1.y), seed), e = cloud_lattice(c1, seed);
    return mix(mix(a, b, f.x), mix(c, e, f.x), f.y);
}
// The cloud over sky color `c` along view direction d (y > 0): returns c with the cloud composited, its opacity a.
//   q    = d.xz * height / d.y * (1 / cell) - drift, mod 64 cells
//   n    = 0.55 v(q) + 0.30 v(2q) + 0.15 v(4q)                       (0..1, mean 0.5)
//   t    = 0.78 - 0.6 coverage                                        (coverage 1 covers the whole sky)
//   mask = smoothstep(t, t + 0.12, n), thick = clamp((n - t) / 0.3, 0, 1) * density
//   a    = mask * (0.55 + 0.45 density) * smoothstep(0.03, 0.25, d.y)  (fades into the horizon haze)
// Lit by the sun (brighter on the sun side, a silver lining on thin edges near the disk, a darker base under a thick
// deck) and the sky ambient, both from the frame's light, so night and dusk follow; the sun disk behind dims by a.
vec3 apply_clouds(vec3 c, vec3 d, vec3 sun, float cs) {
    if (d.y <= 0.03) return c;
    vec2 q = mod(d.xz * (p_cloud.w / d.y) * p_cloud.z - p_cloud2.xy, 64.0);
    float n = cloud_value(q, 64u, 0x27D4EB2Fu) * 0.55 + cloud_value(q * 2.0, 128u, 0x165667B1u) * 0.30 +
              cloud_value(q * 4.0, 256u, 0xD3A2646Cu) * 0.15;
    float t = 0.78 - 0.6 * p_cloud.x;
    float mask = smoothstep(t, t + 0.12, n);
    if (mask <= 0.0) return c;
    float thick = clamp((n - t) / 0.3, 0.0, 1.0) * p_cloud.y;
    float a = mask * (0.55 + 0.45 * p_cloud.y) * smoothstep(0.03, 0.25, d.y);
    float sun_up = smoothstep(-0.1, 0.1, sun.y);
    float side = 0.55 + 0.45 * cs;
    float lining = pow(max(cs, 0.0), 24.0) * 1.5 * (1.0 - thick);
    vec3 direct = p_light_color.rgb * sun_up * (side * (1.0 - 0.65 * thick) + lining);
    vec3 ambient = p_amb_sky.rgb * (1.0 - 0.4 * thick);
    vec3 cloud = 0.9 * (ambient + 0.5 * direct);
    return mix(c, cloud, a);
}
// ---- the night sky (M-NIGHTSKY 14d), behind p_night.y > 0 || p_night2.x > 0 (moon intensity, star density; both 0 =
// the frame without it bit for bit). A function of the view direction only (no camera position, no floating origin)
// and of the twinkle clock p_night2.w (wrapped on the CPU to 60 s). Up-going rays only, scaled by the night fade.
// Stars: the direction is cut into an integer cell grid on the six cube faces (kStarCells cells per face edge; no
// sin()), and each cell is hashed with cloud_hash: a star when its 24-bit hash is under the density times the cell's
// solid angle relative to a face-centre cell ((1 + |c|^2)^-1.5, c the cell centre in -1..1 face coordinates; the
// gnomonic cells shrink about 5x toward the cube corners, so this keeps the stars per steradian even), its position
// (inset to the middle 70% of the cell), a magnitude (u^5: a few bright, many faint), a slight warm-to-cool colour,
// a twinkle phase and rate. The point is a smoothstep blob 1.5 pixels in radius (the pixel footprint comes from the
// neighbouring pixel's ray), never wider than 0.45 of a cell, so it neither aliases nor reaches past the 2 x 2
// cells searched (a star sits at least 0.15 cells inside its cell); where the cells shrink under 4 pixels the layer
// fades out instead of crawling. A star's blob does not cross a cube face's edge (the neighbour face is not searched):
// at most its outer part is cut, and only where a cell is under ten pixels.
const float kStarCells = 32.0;
uint star_face(vec3 d, out vec2 uv) {  // the cube face (0..5: +x -x +y -y +z -z) and its -1..1 coordinates
    vec3 a = abs(d);
    if (a.x >= a.y && a.x >= a.z) { uv = d.yz / a.x; return d.x > 0.0 ? 0u : 1u; }
    if (a.y >= a.z) { uv = d.xz / a.y; return d.y > 0.0 ? 2u : 3u; }
    uv = d.xy / a.z;
    return d.z > 0.0 ? 4u : 5u;
}
vec3 star_face_dir(uint face, vec2 uv) {  // star_face's inverse (unnormalized)
    float s = (face & 1u) == 0u ? 1.0 : -1.0;
    return face < 2u ? vec3(s, uv) : face < 4u ? vec3(uv.x, s, uv.y) : vec3(uv, s);
}
// The stars along d (y > 0), pix: radians per pixel. Twinkle: brightness * (1 - twinkle * 0.6 * w), w a smoothed
// triangle wave of fract(t * rate + phase) with rate (30 + k) / 60 Hz, k 0..63 from the hash, so 60 s is a whole
// number of cycles for every star and the CPU's wrap is seamless.
vec3 night_stars(vec3 d, float pix) {
    vec2 uv;
    uint face = star_face(d, uv);
    vec2 g = (uv * 0.5 + 0.5) * kStarCells;
    vec2 i0 = clamp(floor(g), vec2(0.0), vec2(kStarCells - 1.0));
    vec2 step_to = mix(vec2(-1.0), vec2(1.0), step(vec2(0.5), g - i0));  // toward the nearer neighbours
    float cell = 2.0 / (kStarCells * (1.0 + dot(uv, uv)));  // a cell's angular size here (radians, conservative)
    float lod = smoothstep(2.0, 4.0, cell / pix);
    if (lod <= 0.0) return vec3(0.0);
    float rad = min(1.5 * pix, 0.45 * cell);
    vec3 sum = vec3(0.0);
    for (int k = 0; k < 4; ++k) {
        vec2 ic = i0 + vec2(float(k & 1), float(k >> 1)) * step_to;
        if (any(lessThan(ic, vec2(0.0))) || any(greaterThan(ic, vec2(kStarCells - 1.0)))) continue;  // a face edge
        uvec2 cc = uvec2(ic) + uvec2(face * 64u, 0u);
        uint h0 = cloud_hash(cc + uvec2(0x9E3779B9u, 0u));
        vec2 cuv = (ic + 0.5) * (2.0 / kStarCells) - 1.0;
        if (float(h0 >> 8u) * (1.0 / 16777216.0) >= p_night2.x * pow(1.0 + dot(cuv, cuv), -1.5)) continue;
        uint h1 = cloud_hash(uvec2(h0, cc.y ^ 0x85EBCA6Bu));
        vec2 at = (ic + 0.15 + 0.7 * vec2(float(h1 & 0xFFFu), float((h1 >> 12u) & 0xFFFu)) * (1.0 / 4096.0)) /
                  kStarCells * 2.0 - 1.0;
        float dist = length(d - normalize(star_face_dir(face, at)));
        if (dist >= rad) continue;
        uint h2 = cloud_hash(uvec2(h1, h0));
        float u = float(h2 & 0xFFFu) * (1.0 / 4096.0);
        float mag = 0.06 + 0.94 * u * u * u * u * u;
        vec3 col = mix(vec3(1.0, 0.8, 0.62), vec3(0.72, 0.84, 1.0), float((h2 >> 12u) & 0x3Fu) * (1.0 / 63.0));
        col = mix(vec3(1.0), col, 0.45);
        float x = fract(p_night2.w * (float(30u + (h2 >> 26u)) * (1.0 / 60.0)) +
                        float((h2 >> 18u) & 0xFFu) * (1.0 / 256.0));
        float tri = abs(2.0 * x - 1.0);
        float tw = 1.0 - p_night2.z * 0.6 * tri * tri * (3.0 - 2.0 * tri);
        sum += col * (mag * tw * (1.0 - smoothstep(0.0, rad, dist)));
    }
    return sum * (1.5 * p_night2.y * lod);
}
// The moon and the stars over sky colour c along d. Moon: a disk of cos size p_night.x around p_moon.xyz, shaded as a
// sphere facing the camera: (q.x, q.y) the disk coordinates (1 at the rim) on the basis t (the moon's daily motion,
// the sun's default arc's axis kMoonArc x moon) and b = moon x t, the normal (q, sqrt(1 - |q|^2)) toward the viewer,
// and the sun at the elongation 2 pi phase ahead of the moon along t: s = (sin, 0, -cos) on (t, b, toward the
// viewer) (p_moon2.w, p_moon.w), so phase 0.25 lights the +t half, 0.5 all of it. A soft terminator, a slight limb
// darkening, a faint earthshine on the dark side, a limb 1.5 pixels soft, and a faint additive halo scaled by the
// lit fraction (1 - cos) / 2. The disk hides the stars behind it. The moon is cut at the horizon, the stars fade
// into its haze; both are scaled by the night fade p_night.w.
const vec3 kMoonArc = vec3(0.0, 0.5, 0.8660254);  // render::sun_direction_at's default arc axis (noon elevation 60)
// pix: radians per pixel along d (the sky pass: the neighbouring pixel's ray, apply_night; the water: the reflected
// ray's screen derivative).
vec3 apply_night_at(vec3 c, vec3 d, float pix) {
    if (d.y <= 0.0) return c;
    vec3 stars = p_night2.x > 0.0 ? night_stars(d, pix) * smoothstep(0.0, 0.25, d.y) : vec3(0.0);
    vec3 moon = vec3(0.0);
    float cover = 0.0;
    if (p_night.y > 0.0) {
        vec3 m = p_moon.xyz;
        float cm = dot(d, m);
        float sr = sqrt(1.0 - p_night.x * p_night.x);  // sin of the disk's angular radius
        float hz = smoothstep(0.0, 0.02, d.y);
        float lit_share = 0.5 - 0.5 * p_moon.w;
        float a2 = (1.0 - cm) / (1.0 - p_night.x);  // ~ (angle / radius)^2
        moon = p_moon2.rgb * (p_night.y * 0.02 * lit_share * exp2(-0.4 * a2) * hz);
        if (cm > 0.0 && a2 < 1.5) {
            vec3 t = cross(kMoonArc, m);
            t = dot(t, t) > 1e-8 ? normalize(t) : normalize(cross(m, vec3(1.0, 0.0, 0.0)));
            vec3 b = cross(m, t);
            vec2 q = vec2(dot(d, t), dot(d, b)) / sr;
            float rho2 = dot(q, q);
            float e = clamp(1.5 * pix / sr, 0.02, 0.5);
            cover = (1.0 - smoothstep(1.0 - e, 1.0, sqrt(rho2))) * hz;
            if (cover > 0.0) {
                float z = sqrt(max(1.0 - rho2, 0.0));
                float lit = smoothstep(-0.08, 0.12, q.x * p_moon2.w - z * p_moon.w);
                moon += p_moon2.rgb * (p_night.y * cover * (lit * (0.8 + 0.2 * z) + 0.005));
            }
        }
    }
    return c + (stars * (1.0 - cover) + moon) * p_night.w;
}
// ---- the aurora (M-AURORA 16c), behind p_aurora.x > 0 (level, with the kit's latitude gate and night fade folded in;
// 0 = the frame without it bit for bit). A function of the view direction only (no camera position, no floating
// origin, no parallax) and of the clock p_aurora.w (wrapped on the CPU to 64 s, every rate a whole number of cycles
// per period). Up-going rays only; additive emission over the night sky, under the clouds and the fog.
//   u      the arc parameter 0..1: the azimuth of d from the arc centre p_aurora2.xy (xz; counter-clockwise from above,
//          as the kit's azimuth_deg: u grows with the azimuth), over the arc's width 2 acos(p_aurora2.z); 0 outside.
//   edges  aurora_curtain_height: lower = min(base (1 + 0.2 sin(2 pi (T1 + 0.9 u))), 0.95), upper = min(lower + span
//          (1 + 0.2 sin(2 pi (T3 + 1.3 u + 0.25))), 1), sines of the elevation; Tk the fractional turn of k * quarters
//          cycles per 64 s (quarters = the speed in quarters, as the kit quantises it).
//   h01    (d.y - lower) / (upper - lower), sampled 4 times over the pixel's height (a short march in elevation, not in
//          distance), each smoothstep(0, 0.08, h01) exp(-2.5 h01) times the band colour mix(low, high,
//          smoothstep(0.55, 1, h01)) (aurora_band_color), averaged: the sharp lower edge does not alias.
//   rays   aurora_curtain_intensity's striations: 16 cells per curtain across the arc, a cell's value (0.4 + 0.6 r)
//          (0.85 + 0.15 sin(2 pi (T5 + r))), r the cell's pcg2d hash (cloud_hash(cell, 7), the kit's aurora_hash),
//          box-filtered over the pixel's width in cells and faded to their mean as a cell shrinks under about a
//          pixel, so the rays neither alias nor crawl under a turn; times the end fade over 0.5 - 0.3 (width - 10) /
//          170 of the arc at each end and the pulse 0.9 + 0.1 sin(2 pi T2).
//   total  c + level * kAuroraGain * the above, faded into the horizon haze by smoothstep(0, 0.25, d.y) as the stars.
// The constants below are the kit's (mce/kit/aurora.h), copied as a block: a change there goes here too, and
// test_render3d_aurora_gl compares the drawn edges and band colours with the kit's helpers at probe points.
const float kAuroraPeriod = 64.0;        // kit::kAuroraPeriodSeconds
const float kAuroraSway = 0.2;           // both edges sway by up to 20 percent
const float kAuroraSwayLowU = 0.9;       // lower edge: phase offset 0.9 u, 1 cycle per quarter of speed per period
const float kAuroraSwayHighU = 1.3;      // upper edge: phase offset 1.3 u + 0.25, 3 cycles
const float kAuroraSwayHighOff = 0.25;
const float kAuroraLowerMax = 0.95;      // the lower edge's cap
const uint kAuroraCyclesLow = 1u, kAuroraCyclesHigh = 3u, kAuroraCyclesShimmer = 5u, kAuroraCyclesPulse = 2u;
const float kAuroraEdge = 0.08;          // the lower edge's smoothstep, a share of the curtain's height
const float kAuroraFall = 2.5;           // exp(-2.5 h01) toward the top
const uint kAuroraRayCells = 16u;        // ray cells per curtain
const uint kAuroraRaySeed = 7u;          // aurora_hash(cell, 7)
const float kAuroraRayMin = 0.4;         // a ray's brightness 0.4 + 0.6 r
const float kAuroraShimmer = 0.15;       // 0.85 + 0.15 sin(2 pi (T5 + r))
const float kAuroraPulse = 0.1;          // 0.9 + 0.1 sin(2 pi T2)
const float kAuroraBandLow = 0.55;       // the colour band: smoothstep(0.55, 1, h01) from low to high
// Shader only (not in the kit): the emission at level 1 and full intensity, and the mean of a ray cell's value (what
// the rays fade to under a pixel).
const float kAuroraGain = 0.6;
const float kAuroraRayMean = 0.595;
float aurora_turns(uint cycles) {  // the fractional turn of cycles * quarters cycles per period at p_aurora.w
    float quarters = floor(p_aurora4.w * 4.0 + 0.5);
    return fract(p_aurora.w * (float(cycles) * quarters / kAuroraPeriod));
}
float aurora_sin_turns(float turns, float offset) { return sin(6.2831853 * fract(turns + offset)); }
float aurora_ray(uint cell, float shimmer) {
    float r = float(cloud_hash(uvec2(cell, kAuroraRaySeed)) >> 8u) * (1.0 / 16777216.0);
    return (kAuroraRayMin + (1.0 - kAuroraRayMin) * r) *
           (1.0 - kAuroraShimmer + kAuroraShimmer * aurora_sin_turns(shimmer, r));
}
// The aurora over sky colour c along d; pix: radians per pixel along d (the sky pass: the neighbouring pixel's ray,
// apply_aurora; the water: the reflected ray's screen derivative).
vec3 aurora_layer(vec3 c, vec3 d, float pix) {
    if (d.y <= 0.0) return c;
    float hl = length(d.xz);  // cos of the elevation
    if (hl < 1e-4) return c;
    vec2 e = d.xz / hl;
    vec2 m = p_aurora2.xy;
    float half_w = acos(clamp(p_aurora2.z, 0.0, 0.9962));  // the CPU keeps the width in 10..180 degrees
    float u = 0.5 + atan(m.y * e.x - m.x * e.y, dot(m, e)) / (2.0 * half_w);
    if (u <= 0.0 || u >= 1.0) return c;
    float sway_lo = aurora_sin_turns(aurora_turns(kAuroraCyclesLow), kAuroraSwayLowU * u);
    float sway_hi = aurora_sin_turns(aurora_turns(kAuroraCyclesHigh), kAuroraSwayHighU * u + kAuroraSwayHighOff);
    float lower = min(p_aurora.y * (1.0 + kAuroraSway * sway_lo), kAuroraLowerMax);
    float upper = min(lower + p_aurora.z * (1.0 + kAuroraSway * sway_hi), 1.0);
    float pixy = pix * hl;  // the pixel's height in sines of the elevation
    if (d.y + 0.5 * pixy <= lower || d.y - 0.5 * pixy > upper) return c;
    float span = upper - lower;
    vec3 band = vec3(0.0);
    for (int k = 0; k < 4; ++k) {
        float h = (d.y + pixy * (float(k) * 0.25 - 0.375) - lower) / span;
        if (h <= 0.0 || h > 1.0) continue;
        band += mix(p_aurora3.rgb, p_aurora4.rgb, smoothstep(kAuroraBandLow, 1.0, h)) *
                (smoothstep(0.0, kAuroraEdge, h) * exp(-kAuroraFall * h));
    }
    float width = clamp(degrees(2.0 * half_w), 10.0, 180.0);
    float fade_len = 0.5 - 0.3 * ((width - 10.0) / 170.0);
    float ends = smoothstep(0.0, fade_len, u) * smoothstep(0.0, fade_len, 1.0 - u);
    uint cells = kAuroraRayCells * uint(clamp(p_aurora2.w, 1.0, 6.0));
    float g = u * float(cells);
    uint i0 = min(uint(g), cells - 1u);
    float fx = g - float(i0);
    float w = pix * float(cells) / (2.0 * half_w * hl);  // the pixel's width in ray cells
    float hw = 0.5 * min(w, 1.0);
    float shimmer = aurora_turns(kAuroraCyclesShimmer);
    float rays = aurora_ray(i0, shimmer);
    if (fx < hw && i0 > 0u)  // the footprint reaches into the cell before (after): its share of the box
        rays = mix(rays, aurora_ray(i0 - 1u, shimmer), (hw - fx) / (2.0 * hw));
    else if (fx > 1.0 - hw && i0 + 1u < cells)
        rays = mix(rays, aurora_ray(i0 + 1u, shimmer), (fx - 1.0 + hw) / (2.0 * hw));
    rays = mix(rays, kAuroraRayMean, smoothstep(0.5, 1.5, w));
    float pulse = 1.0 - kAuroraPulse + kAuroraPulse * aurora_sin_turns(aurora_turns(kAuroraCyclesPulse), 0.0);
    return c + band * (0.25 * p_aurora.x * kAuroraGain * ends * rays * pulse * smoothstep(0.0, 0.25, d.y));
}
)"

constexpr const char* kSkyFS = "#version 450 core\n" MCE_GLSL_FRAME_U MCE_GLSL_PASS_U MCE_GLSL_VOLUME R"(
layout(location = 0) in vec2 v_ndc;
layout(location = 0) out vec4 o_color;
)" MCE_GLSL_SKY_LAYERS R"(
vec3 apply_night(vec3 c, vec3 d) {
    if (d.y <= 0.0) return c;
    vec4 w1 = u_inv_view_proj * vec4(v_ndc + vec2(2.0 * u_viewport.z, 0.0), 1.0, 1.0);
    float pix = max(length(normalize(w1.xyz / w1.w - u_camera_pos.xyz) - d), 1e-6);  // radians per pixel
    return apply_night_at(c, d, pix);
}
vec3 apply_aurora(vec3 c, vec3 d) {  // aurora_layer with the sky pass's pixel footprint, as apply_night
    if (d.y <= 0.0) return c;
    vec4 w1 = u_inv_view_proj * vec4(v_ndc + vec2(2.0 * u_viewport.z, 0.0), 1.0, 1.0);
    float pix = max(length(normalize(w1.xyz / w1.w - u_camera_pos.xyz) - d), 1e-6);  // radians per pixel
    return aurora_layer(c, d, pix);
}
void main() {
    vec4 w = u_inv_view_proj * vec4(v_ndc, 1.0, 1.0);
    vec3 d = normalize(w.xyz / w.w - u_camera_pos.xyz);
    vec3 c = sky_radiance(d, 0.0);
    vec3 sun = -p_light_dir.xyz;
    float cs = dot(d, sun);
    float edge = 1.0 - p_sun.x;
    c += p_light_color.rgb * p_sun.y * smoothstep(p_sun.x - edge * 0.25, p_sun.x + edge * 0.25, cs);
    if (p_night.y > 0.0 || p_night2.x > 0.0) c = apply_night(c, d);  // under the clouds and the fog, as the sun is
    if (p_aurora.x > 0.0) c = apply_aurora(c, d);  // in front of the stars, under the clouds and the fog (M-AURORA 16c)
    if (p_cloud.x > 0.0) c = apply_clouds(c, d, sun, cs);  // over the disk: a cloud in front of the sun dims it
    if (p_vol.x > 0.5) {  // the volumetric fog's whole depth in front of the sky
        vec4 f = vol_fog(v_ndc * 0.5 + 0.5, 1e30);
        c = c * f.a + f.rgb;
    } else if (p_fog.w > 0.0) {
        c = mix(p_fog.rgb, c, smoothstep(0.0, 0.35, d.y));  // haze toward the horizon
    }
    o_color = vec4(c, 1.0);
}
)";

// ---- the water plane (M-WATER 15c), drawn by the main pass after the opaques and before the sky and the blended
// draws, only while Scene3D::water() is on (p_water* and p_wave are zero otherwise and nothing reads them). A flat,
// camera-following square of kWaterGrid x kWaterGrid cells at the level p_water.x (floating-origin local), p_water.w
// meters in half size, its centre snapped to whole cells so the vertices never swim; depth-tested and depth-written.
constexpr const char* kWaterVS = "#version 450 core\n" MCE_GLSL_FRAME_U MCE_GLSL_PASS_U R"(
layout(location = 0) out vec3 v_world;
out gl_PerVertex { vec4 gl_Position; };
const int kWaterGrid = 16;  // Forward3D draws 6 * kWaterGrid^2 vertices
void main() {
    const ivec2 kCorner[6] = ivec2[6](ivec2(0, 0), ivec2(1, 0), ivec2(1, 1), ivec2(0, 0), ivec2(1, 1), ivec2(0, 1));
    int q = gl_VertexID / 6;
    ivec2 c = ivec2(q % kWaterGrid, q / kWaterGrid) + kCorner[gl_VertexID % 6];
    float cell = 2.0 * p_water.w / float(kWaterGrid);
    vec2 centre = floor(u_camera_pos.xz / cell + 0.5) * cell;
    vec2 xz = centre + (vec2(c) - 0.5 * float(kWaterGrid)) * cell;
    v_world = vec3(xz.x, p_water.x, xz.y);
    gl_Position = u_view_proj * vec4(v_world, 1.0);
}
)";

// The water's surface. Normals only (the plane stays flat):
//   waves   p_water.y Gerstner waves (forward3d.cpp water_params expands the authoring with the kit's tables:
//           kit::water_wave_set), each a wave vector K, amplitude A and phase (the floating origin and the wrapped
//           clock folded in on the CPU in f64), Q = min(1, steepness / (count k A)) as the kit computes it; the
//           horizontal displacement is undone by the kit's two fixed-point steps and the normal is taken at the
//           source point, so it is kit::water_normal's. Each wave fades to flat where its wavelength covers fewer
//           than 8 pixels (gone under 3): no sparkle toward the horizon, no aliasing of the short waves; the faded
//           slope variance roughens the glint instead. Two fixed small ripples (kWaterDetailK, slope p_water5.z)
//           add detail near the camera and fade the same way.
//   sky     the reflected ray (kept above the horizon) through sky_radiance at the roughness' lod and the sky's own
//           layers (MCE_GLSL_SKY_LAYERS: stars and moon, clouds) and its exp2 horizon haze, not a copy of them; the
//           sun disk is replaced by the glint. Schlick fresnel, F0 0.02, scaled by the reflectivity.
//   glint   GGX from the frame's directional light (the sun, or the moon while the kit hands it the light).
//   body    the shallow-to-deep tint by the optical depth along the view ray to the opaque surface behind the water
//           (u_water_depth: the half-size prepass's view depth, p_water4.w 1), else (and with nothing behind) deep,
//           with a shallow hint fading out with distance; lit by the hemisphere ambient and the light.
//   foam    within p_water4.z meters of the opaque surface behind (along the view ray: the shore line on a steep
//           shore), stripes moving in and the wave crests break it up; needs the depth read.
//   fog     last, as kMeshFS does (volumetric or exp2), so mist and fog hide the water with distance.
constexpr const char* kWaterFS = "#version 450 core\n" MCE_GLSL_FRAME_U MCE_GLSL_PASS_U MCE_GLSL_VOLUME R"(
layout(location = 0) in vec3 v_world;
layout(location = 0) out vec4 o_color;
layout(binding = 6) uniform sampler2D u_water_depth;  // half size; a: the opaques' view depth (0 = nothing drawn)
)" MCE_GLSL_SKY_LAYERS R"(
// The detail ripples' wave vectors (rad/m on xz): forward3d.cpp kWaterDetail (directions and wavelengths) folds the
// origin into their phases with these exact values, so a change here goes there too.
const vec2 kWaterDetailK[2] = vec2[2](vec2(0.8, 0.6) * (6.2831853 / 1.7), vec2(-0.6, 0.8) * (6.2831853 / 1.1));
vec3 water_sky(vec3 r, float pix, float lod) {
    if (p_sky_zenith.w < 0.5) return p_fog.rgb;  // no sky: the frame's clear colour is behind everything
    vec3 c = sky_radiance(r, lod);
    vec3 sun = -p_light_dir.xyz;
    if (p_night.y > 0.0 || p_night2.x > 0.0) c = apply_night_at(c, r, pix);
    if (p_aurora.x > 0.0) c = aurora_layer(c, r, pix);  // the curtains on the reflected ray (M-AURORA 16c)
    if (p_cloud.x > 0.0) c = apply_clouds(c, r, sun, dot(r, sun));
    if (p_vol.x <= 0.5 && p_fog.w > 0.0) c = mix(p_fog.rgb, c, smoothstep(0.0, 0.35, r.y));
    return c;
}
// The view depth of the opaque surface behind view depth zw at screen uv: the half-size texel under the pixel, or
// where that one holds a nearer object (its half-size edge), the nearest surface behind among the 2 x 2 texels
// around; 0 when there is none.
float water_floor_depth(vec2 uv, float zw) {
    ivec2 size = textureSize(u_water_depth, 0);
    float z0 = texelFetch(u_water_depth, clamp(ivec2(uv * vec2(size)), ivec2(0), size - 1), 0).a;
    if (z0 > zw) return z0;
    ivec2 base = ivec2(floor(uv * vec2(size) - 0.5));
    float best = 0.0;
    for (int k = 0; k < 4; ++k) {
        ivec2 t = clamp(base + ivec2(k & 1, k >> 1), ivec2(0), size - 1);
        float z = texelFetch(u_water_depth, t, 0).a;
        if (z > zw && (best == 0.0 || z < best)) best = z;
    }
    return best;
}
void main() {
    vec3 p = v_world;
    float foot = max(length(dFdx(p)), length(dFdy(p)));  // meters per pixel on the plane
    vec3 to_cam = u_camera_pos.xyz - p;
    float dist = length(to_cam);
    vec3 v = to_cam / max(dist, 1e-6);
    int count = int(p_water.y);
    float fade[8];
    float q[8];
    float lost = 0.0;  // slope variance of what the fades removed
    for (int i = 0; i < count; ++i) {
        float k = length(p_wave[i].xy);
        float ka = k * p_wave[i].z * float(count);
        q[i] = ka > p_water.z ? p_water.z / ka : 1.0;
        fade[i] = smoothstep(3.0, 8.0, 6.2831853 / (k * max(foot, 1e-6)));
        float s = k * p_wave[i].z * (1.0 - fade[i]);
        lost += s * s;
    }
    vec2 u = p.xz;
    for (int it = 0; it < 2; ++it) {
        vec2 dsp = vec2(0.0);
        for (int i = 0; i < count; ++i) {
            vec2 K = p_wave[i].xy;
            dsp += K * (q[i] * p_wave[i].z * fade[i] / length(K) * cos(dot(K, u) + p_wave[i].w));
        }
        u = p.xz - dsp;
    }
    vec3 nrm = vec3(0.0, 1.0, 0.0);
    for (int i = 0; i < count; ++i) {
        vec2 K = p_wave[i].xy;
        float th = dot(K, u) + p_wave[i].w;
        float a = p_wave[i].z * fade[i];
        nrm.xz -= K * (a * cos(th));
        nrm.y -= q[i] * length(K) * a * sin(th);
    }
    float fold = 1.0 - nrm.y;  // 0 flat .. the steepness at a fully folded crest
    for (int j = 0; j < 2; ++j) {
        vec2 K = kWaterDetailK[j];
        float f = smoothstep(3.0, 8.0, 6.2831853 / (length(K) * max(foot, 1e-6)));
        nrm.xz -= normalize(K) * (p_water5.z * f * cos(dot(K, p.xz) + p_water5[j]));
        float s = p_water5.z * (1.0 - f);
        lost += s * s;
    }
    nrm.y = max(nrm.y, 1e-4);
    vec3 n = normalize(nrm);
    vec3 r = reflect(-v, n);
    r = normalize(vec3(r.x, max(r.y, 0.002), r.z));  // a ray tipped under the horizon reads the horizon
    float pix = max(max(length(dFdx(r)), length(dFdy(r))), 1e-6);  // radians per pixel along r (the stars' footprint)
    float NoV = clamp(dot(n, v), 1e-4, 1.0);
    float F = (0.02 + 0.98 * pow(1.0 - NoV, 5.0)) * p_water3.w;
    float rough = p_water4.x;
    vec3 sky = water_sky(r, pix, rough * 8.0);
    // the glint
    vec3 l = -p_light_dir.xyz;
    vec3 h = normalize(l + v);
    float NoL = max(dot(n, l), 0.0), NoH = max(dot(n, h), 0.0), LoH = max(dot(l, h), 0.0);
    float alpha = max(rough * rough, 0.02);
    float a2 = alpha * alpha + lost;
    float dd = NoH * NoH * (a2 - 1.0) + 1.0;
    float D = a2 / (PI * dd * dd);
    float Fh = (0.02 + 0.98 * pow(1.0 - LoH, 5.0)) * p_water3.w;
    vec3 glint = p_light_color.rgb * (D * Fh * 0.25 / max(LoH * LoH, 0.1) * NoL * smoothstep(-0.02, 0.05, l.y));
    // the body: the tint by the optical depth to the opaque surface behind
    float zw = -(u_view * vec4(p, 1.0)).z;
    float path = -1.0;  // meters along the view ray to the opaque surface behind; -1 = none (or not read)
    if (p_water4.w > 0.5) {
        vec4 cp = u_view_proj * vec4(p, 1.0);
        float zo = water_floor_depth(cp.xy / cp.w * 0.5 + 0.5, zw);
        if (zo > 0.0) path = dist * (zo / zw - 1.0);
    }
    float t = path >= 0.0 ? 1.0 - exp(-3.0 * path / p_water2.w) : 1.0 - 0.3 * exp(-dist / (10.0 * p_water2.w));
    vec3 light = mix(p_amb_ground.rgb, p_amb_sky.rgb, 0.75) + p_light_color.rgb * (max(l.y, 0.0) / PI);
    vec3 body = mix(p_water2.rgb, p_water3.rgb, t) * light;
    vec3 color = body * (1.0 - F) + sky * F + glint;
    // the shore foam
    if (p_water4.y > 0.0 && path >= 0.0 && path < p_water4.z) {
        float band = 1.0 - smoothstep(0.0, p_water4.z, path);  // 1 at the shore line, 0 at the band's edge
        float crest = clamp(fold / max(p_water.z, 1e-3), 0.0, 1.0);
        float stripe = 0.5 + 0.5 * sin(6.2831853 * path / max(0.35 * p_water4.z, 0.02) + p_water5.x);
        float foam = p_water4.y * smoothstep(0.35, 0.75, band * (0.55 + 0.45 * stripe + 0.35 * crest));
        color = mix(color, vec3(0.9) * light, foam);
    }
    if (p_vol.x > 0.5) {  // as kMeshFS
        vec4 c = u_view_proj * vec4(p, 1.0);
        vec4 f = vol_fog(c.xy / c.w * 0.5 + 0.5, zw);
        color = color * f.a + f.rgb;
    } else {
        float fog = 1.0 - exp(-(p_fog.w * dist) * (p_fog.w * dist));
        color = mix(color, p_fog.rgb, clamp(fog, 0.0, 1.0));
    }
    o_color = vec4(color, 1.0);
}
)";
#undef MCE_GLSL_SKY_LAYERS

// ---- post chain. PostU rides bank 1 for the post passes.
// Auto exposure (M-VISUAL 8): Forward3D::exposure_buffer(), bank kExposure.
#define MCE_GLSL_EXPOSURE(qual) "layout(std430, binding = 12) " qual " buffer Exposure { uint e_bins[64]; float e_exposure; float e_average; uint e_frames; uint e_pad; };\n"

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
constexpr const char* kBloomDownFS = "#version 450 core\n" MCE_GLSL_POST_U MCE_GLSL_EXPOSURE("readonly") R"(
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
        col *= q_params.w * (q_texel.z > 0.5 ? e_exposure : 1.0);  // exposure: the bloom chain works in exposed units
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
// Then the color-vision correction (M-ACCESS) on the graded frame.
// u_cluster here: x > 0.5 = auto exposure (times e_exposure), y = color-grade strength (0 = off),
// z = color-vision mode (0 = off), w = its strength.
constexpr const char* kTonemapFS = "#version 450 core\n" MCE_GLSL_FRAME_U MCE_GLSL_EXPOSURE("readonly") MCE_GLSL_CVD R"(
layout(std140, binding = 1) uniform FilmU {
    vec4 f_lens;   // x vignette, y vignette smoothness, z grain, w grain cell size (pixels)
    vec4 f_lens2;  // x chromatic aberration, y lens dirt intensity, z tonemapper (0 ACES, 1 AgX, 2 Neutral), w grain seed
};
layout(binding = 6) uniform sampler2D u_hdr;
layout(binding = 7) uniform sampler2D u_bloom;
layout(binding = 8) uniform sampler2DArray u_lut;  // 256 × 16 strip (16³), display space
layout(binding = 9) uniform sampler2DArray u_dirt;  // lens dirt mask (linear rgb)
layout(location = 0) in vec2 v_uv;
layout(location = 0) out vec4 o_color;
vec3 aces(vec3 x) { return clamp((x * (2.51 * x + 0.03)) / (x * (2.43 * x + 0.59) + 0.14), 0.0, 1.0); }
// AgX base (Troy Sobotka's; this is the formulation three.js ships): Rec.2020 inset, log2 over 16.5 stops,
// a sigmoid fit, outset, and back to linear sRGB.
vec3 agx_contrast(vec3 x) {
    vec3 x2 = x * x, x4 = x2 * x2;
    return 15.5 * x4 * x2 - 40.14 * x4 * x + 31.96 * x4 - 6.868 * x2 * x + 0.4298 * x2 + 0.1191 * x - 0.00232;
}
vec3 agx(vec3 c) {
    const mat3 to2020 = mat3(0.6274, 0.0691, 0.0164, 0.3293, 0.9195, 0.0880, 0.0433, 0.0113, 0.8956);
    const mat3 from2020 = mat3(1.6605, -0.1246, -0.0182, -0.5876, 1.1329, -0.1006, -0.0728, -0.0083, 1.1187);
    // Column-major: each row of the matrix as a color transform sums to 1 (inset) / each column does (outset).
    const mat3 inset = mat3(0.856627153315983, 0.137318972929847, 0.11189821299995,
                            0.0951212405381588, 0.761241990602591, 0.0767994186031903,
                            0.0482516061458583, 0.101439036467562, 0.811302368396859);
    const mat3 outset = mat3(1.1271005818144368, -0.1413297634984383, -0.14132976349843826,
                             -0.11060664309660323, 1.157823702216272, -0.11060664309660294,
                             -0.016493938717834573, -0.016493938717834257, 1.2519364065950405);
    c = inset * (to2020 * c);
    c = clamp((log2(max(c, 1e-10)) + 12.47393) / 16.5, 0.0, 1.0);
    c = outset * agx_contrast(c);
    return clamp(from2020 * pow(max(c, 0.0), vec3(2.2)), 0.0, 1.0);
}
// Khronos PBR Neutral: leaves in-gamut colors alone up to a knee, then rolls the peak off and desaturates.
vec3 neutral(vec3 c) {
    c = max(c, 0.0);
    const float start = 0.8 - 0.04, desat = 0.15;
    float x = min(c.r, min(c.g, c.b));
    c -= x < 0.08 ? x - 6.25 * x * x : 0.04;
    float peak = max(c.r, max(c.g, c.b));
    if (peak < start) return c;
    const float d = 1.0 - start;
    float new_peak = 1.0 - d * d / (peak + d - start);
    c *= new_peak / peak;
    return mix(c, vec3(new_peak), 1.0 - 1.0 / (desat * (peak - new_peak) + 1.0));
}
vec3 linear_to_srgb(vec3 c) { return mix(c * 12.92, 1.055 * pow(c, vec3(1.0 / 2.4)) - 0.055, step(0.0031308, c)); }
vec3 srgb_to_linear(vec3 c) { return mix(c / 12.92, pow((c + 0.055) / 1.055, vec3(2.4)), step(0.04045, c)); }
vec3 grade(vec3 c) {  // as the 2D post pass (renderer.cpp)
    c = clamp(c, 0.0, 1.0) * 15.0;
    float b0 = floor(c.b), b1 = min(b0 + 1.0, 15.0);
    vec2 rg = vec2((c.r + 0.5) / 256.0, (c.g + 0.5) / 16.0);
    vec3 s0 = textureLod(u_lut, vec3(rg + vec2(b0 / 16.0, 0.0), 0.0), 0.0).rgb;
    vec3 s1 = textureLod(u_lut, vec3(rg + vec2(b1 / 16.0, 0.0), 0.0), 0.0).rgb;
    return mix(s0, s1, c.b - b0);
}
float grain_hash(vec2 cell, float seed) {
    uvec3 q = uvec3(uvec2(cell + 4096.0), uint(seed));
    q = q * 1664525u + 1013904223u;
    q.x += q.y * q.z;
    q.y += q.z * q.x;
    q.z += q.x * q.y;
    q ^= q >> 16u;
    q.x += q.y * q.z;
    return float(q.x >> 8u) * (1.0 / 16777216.0);
}
void main() {
    float exposure = u_params.x * (u_cluster.x > 0.5 ? e_exposure : 1.0);
    vec3 c;
    vec3 bloom = vec3(0.0);
    if (f_lens2.x > 0.0) {  // chromatic aberration: the red and blue taps slide along the radius (1 = 0.5 % of the width at the edge)
        vec2 shift = (v_uv - 0.5) * (f_lens2.x * 0.01);
        c = vec3(texture(u_hdr, v_uv + shift).r, texture(u_hdr, v_uv).g, texture(u_hdr, v_uv - shift).b) * exposure;
        if (u_params.w > 0.5)
            bloom = vec3(textureLod(u_bloom, v_uv + shift, 0.0).r, textureLod(u_bloom, v_uv, 0.0).g, textureLod(u_bloom, v_uv - shift, 0.0).b) * u_params.z;
    } else {
        c = texture(u_hdr, v_uv).rgb * exposure;
        if (u_params.w > 0.5) bloom = textureLod(u_bloom, v_uv, 0.0).rgb * u_params.z;
    }
    c += bloom;
    if (f_lens2.y > 0.0) c += bloom * textureLod(u_dirt, vec3(v_uv, 0.0), 0.0).rgb * f_lens2.y;  // bloom lights the dirt
    if (f_lens.x > 0.0) {  // vignette in linear light: 0 at the centre, 1 at the corners
        float r = length(v_uv - 0.5) * 1.41421356;
        c *= 1.0 - f_lens.x * smoothstep(1.0 - f_lens.y, 1.0, r);
    }
    if (f_lens2.z > 1.5) c = neutral(c);
    else if (f_lens2.z > 0.5) c = agx(c);
    else c = aces(c * 0.6);
    c = u_params.y > 0.5 ? linear_to_srgb(c) : c;
    if (u_cluster.y > 0.0) {  // the LUT is authored on display (sRGB) values
        vec3 d = u_params.y > 0.5 ? c : linear_to_srgb(c);
        d = mix(d, grade(d), u_cluster.y);
        c = u_params.y > 0.5 ? d : srgb_to_linear(d);
    }
    if (u_cluster.z > 0.5) {  // on linear values
        vec3 l = u_params.y > 0.5 ? srgb_to_linear(c) : c;
        l = cvd_correct(int(u_cluster.z + 0.5), u_cluster.w, l);
        c = u_params.y > 0.5 ? linear_to_srgb(l) : l;
    }
    float luma = dot(c, vec3(0.299, 0.587, 0.114));  // FXAA reads this: taken before the grain, which it would chase
    if (f_lens.z > 0.0) {  // animated grain in display space, strongest in the mid tones
        // On an sRGB target (u_params.y < 0.5) the hardware encodes on write and c is still linear: grain there
        // is added to the encoded value so GL and SDL GPU agree.
        vec3 d = u_params.y > 0.5 ? c : linear_to_srgb(clamp(c, 0.0, 1.0));
        float dl = dot(d, vec3(0.299, 0.587, 0.114));
        // The cell in integers, half-pixel cell sizes: with floats, (x + 0.5) / 1.5 lands on a cell edge every third
        // pixel and the driver's rounding picked the cell (rows and columns of grain differed between GPUs).
        uvec2 px = uvec2(v_uv * u_viewport.xy);
        uint half_cells = uint(max(f_lens.w, 1.0) * 2.0 + 0.5);
        float n = grain_hash(vec2((px * 2u + 1u) / half_cells), f_lens2.w) - 0.5;
        d += n * f_lens.z * 0.3 * (4.0 * dl * (1.0 - dl));
        c = u_params.y > 0.5 ? d : srgb_to_linear(clamp(d, 0.0, 1.0));
    }
    o_color = vec4(c, luma);
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
// ---- spatial upsample and sharpening (M-VISUAL2 2), on the display-space frame -------------------
// Edge-adaptive upsample: the 4x4 source texels around the output pixel weighted by a Lanczos-2
// approximation, (25/16 (2/5 d² - 1)² - 9/16) (w d² - 1)², over offsets rotated into the local edge's
// frame (the luma gradient of the four nearest texels, bilinearly weighted) and squeezed across the
// edge and stretched along it by how strong the edge is; the window tightens with it too. The result
// is clamped to the four nearest texels, so the negative lobes cannot ring. q_texel: xy 1 / source size,
// zw source size.
constexpr const char* kUpscaleFS = "#version 450 core\n" MCE_GLSL_POST_U R"(
float luma(vec3 c) { return dot(c, vec3(0.299, 0.587, 0.114)); }
void main() {
    vec2 pp = v_uv * q_texel.zw - 0.5;
    vec2 fp = floor(pp), f = pp - fp;
    ivec2 base = ivec2(fp), hi = ivec2(q_texel.zw) - 1;
    vec3 c[16];
    float l[16];
    for (int j = 0; j < 4; ++j)
        for (int i = 0; i < 4; ++i) {
            vec3 t = texelFetch(u_src, clamp(base + ivec2(i - 1, j - 1), ivec2(0), hi), 0).rgb;
            c[j * 4 + i] = t;
            l[j * 4 + i] = luma(t);
        }
    // The edge: central-difference luma gradients at the four nearest texels, bilinearly weighted.
    vec2 dir = vec2(0.0);
    float len = 0.0;
    for (int b = 1; b <= 2; ++b)
        for (int a = 1; a <= 2; ++a) {
            float w = (a == 1 ? 1.0 - f.x : f.x) * (b == 1 ? 1.0 - f.y : f.y);
            vec2 g = vec2(l[b * 4 + a + 1] - l[b * 4 + a - 1], l[(b + 1) * 4 + a] - l[(b - 1) * 4 + a]);
            float span = max(max(abs(l[b * 4 + a + 1] - l[b * 4 + a]), abs(l[b * 4 + a] - l[b * 4 + a - 1])),
                             max(abs(l[(b + 1) * 4 + a] - l[b * 4 + a]), abs(l[b * 4 + a] - l[(b - 1) * 4 + a])));
            dir += g * w;
            len += w * clamp(length(g) * 0.5 / max(span, 1e-4), 0.0, 1.0);  // 1 on a clean step, 0 on noise
        }
    float dl = length(dir);
    dir = dl > 1e-5 ? dir / dl : vec2(1.0, 0.0);
    len *= len;
    float stretch = 1.0 / max(abs(dir.x), abs(dir.y));  // 1 on an axis, sqrt(2) on a diagonal
    vec2 scale = vec2(1.0 - 0.5 * len, 1.0 + (stretch - 1.0) * len);  // across, along the edge
    float lobe = 0.5 - 0.29 * len;
    float clip = 1.0 / lobe;
    vec3 sum = vec3(0.0);
    float wsum = 0.0;
    for (int j = 0; j < 4; ++j)
        for (int i = 0; i < 4; ++i) {
            vec2 o = vec2(i - 1, j - 1) - f;
            vec2 v = vec2(dot(o, dir), dot(o, vec2(-dir.y, dir.x))) * scale;
            float d2 = min(dot(v, v), clip);
            float a = 0.4 * d2 - 1.0;
            float wb = 25.0 / 16.0 * a * a - 9.0 / 16.0;
            float wl = lobe * d2 - 1.0;
            float w = wb * wl * wl;
            sum += c[j * 4 + i] * w;
            wsum += w;
        }
    vec3 lo = min(min(c[5], c[6]), min(c[9], c[10])), hiC = max(max(c[5], c[6]), max(c[9], c[10]));
    o_color = vec4(clamp(sum / wsum, lo, hiC), 1.0);
}
)";

// Contrast-adaptive sharpening (AMD CAS's filter): the pixel minus a share of its four neighbors, the
// share largest where the neighborhood's contrast is low and falling to none where it would clip.
// q_texel: xy the source's texel size, q_params.x sharpness (0..1).
constexpr const char* kSharpenFS = "#version 450 core\n" MCE_GLSL_POST_U R"(
void main() {
    vec3 e = tap(vec2(0.0));
    vec3 b = tap(vec2(0.0, -1.0)), d = tap(vec2(-1.0, 0.0)), fr = tap(vec2(1.0, 0.0)), h = tap(vec2(0.0, 1.0));
    vec3 mn = min(e, min(min(b, d), min(fr, h))), mx = max(e, max(max(b, d), max(fr, h)));
    vec3 amp = sqrt(clamp(min(mn, 1.0 - mx) / max(mx, vec3(1e-4)), 0.0, 1.0));
    vec3 w = amp * (-1.0 / mix(8.0, 5.0, clamp(q_params.x, 0.0, 1.0)));
    o_color = vec4(clamp((e + (b + d + fr + h) * w) / (1.0 + 4.0 * w), 0.0, 1.0), 1.0);
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

// A probe's environment slot as one RGBM RGBA8 strip (the editor's bake reads it back): the slot's
// layers stacked along v, layer l at v in [l, l + 1) / layers.
constexpr const char* kProbeEncodeFS = R"(#version 450 core
layout(std140, binding = 1) uniform ExportU {
    vec4 e_params;  // x: first layer, y: layers, z: RGBM range
};
layout(binding = 6) uniform sampler2DArray u_env_src;
layout(location = 0) in vec2 v_uv;
layout(location = 0) out vec4 o_color;
void main() {
    float lv = v_uv.y * e_params.y;
    float layer = min(floor(lv), e_params.y - 1.0);
    vec3 c = max(textureLod(u_env_src, vec3(v_uv.x, lv - layer, e_params.x + layer), 0.0).rgb, vec3(0.0));
    float m = clamp(max(max(c.r, c.g), max(c.b, 1e-6)) / e_params.z, 1.0 / 255.0, 1.0);
    m = ceil(m * 255.0) / 255.0;
    o_color = vec4(min(c / (m * e_params.z), vec3(1.0)), m);
}
)";

// One layer of a baked probe (RGBM RGBA8 array, cooked from .mcenv) back into its IBL slot.
constexpr const char* kProbeDecodeFS = R"(#version 450 core
layout(std140, binding = 1) uniform DecodeU {
    vec4 d_params;  // x: source layer, z: RGBM range
};
layout(binding = 6) uniform sampler2DArray u_baked;
layout(location = 0) in vec2 v_uv;
layout(location = 0) out vec4 o_color;
void main() {
    vec4 t = textureLod(u_baked, vec3(v_uv, d_params.x), 0.0);
    o_color = vec4(t.rgb * t.a * d_params.z, 1.0);
}
)";

// The irradiance volume's update (M-VISUAL 6): the cube faces of a batch of probes (6 layers per
// probe, row-major from the batch's first row) into their 4 texels each: V_k = the sphere integral of
// L * (1 + w.d_k) / 4pi over the faces' texels (solid angle 1 / (1 + u² + v²)^1.5). Drawn over the
// batch's rows only, alpha = the new value's weight (alpha blending keeps the rest of the old one).
constexpr const char* kGiProjectFS = R"(#version 450 core
layout(std140, binding = 1) uniform GiProjectU {
    vec4 g_size;    // x: texels per row, y: rows in the pass, z: face size
    vec4 g_params;  // x: probes per row, y: weight of the new value, z: the pass's first face probe
};
layout(binding = 6) uniform sampler2DArray u_faces;
layout(location = 0) in vec2 v_uv;
layout(location = 0) out vec4 o_color;
const vec3 kTetra[4] = vec3[4](vec3(0.57735027, 0.57735027, 0.57735027), vec3(0.57735027, -0.57735027, -0.57735027),
                               vec3(-0.57735027, 0.57735027, -0.57735027), vec3(-0.57735027, -0.57735027, 0.57735027));
// The capture's faces (Forward3D probe_face_views): +x -x +y -y +z -z, each looking down -Z of
// look_at(pos, pos + kFwd, kUp), so the face's ndc x runs along cross(kFwd, kUp) and ndc y along kUp.
const vec3 kFwd[6] = vec3[6](vec3(1, 0, 0), vec3(-1, 0, 0), vec3(0, 1, 0), vec3(0, -1, 0), vec3(0, 0, 1), vec3(0, 0, -1));
const vec3 kUp[6] = vec3[6](vec3(0, 1, 0), vec3(0, 1, 0), vec3(0, 0, -1), vec3(0, 0, 1), vec3(0, 1, 0), vec3(0, 1, 0));
void main() {
    int x = min(int(v_uv.x * g_size.x), int(g_size.x) - 1);
    int row = min(int(v_uv.y * g_size.y), int(g_size.y) - 1);
    int k = x & 3;
    int layer = (int(g_params.z) + row * int(g_params.x) + (x >> 2)) * 6;
    int res = int(g_size.z);
    vec3 d_k = kTetra[k];
    vec3 sum = vec3(0.0);
    float wsum = 0.0;
    for (int f = 0; f < 6; ++f) {
        vec3 right = cross(kFwd[f], kUp[f]);
        for (int j = 0; j < res; ++j) {
            for (int i = 0; i < res; ++i) {
                vec2 ndc = (vec2(i, j) + 0.5) * (2.0 / float(res)) - 1.0;
                vec3 d = kFwd[f] + right * ndc.x + kUp[f] * ndc.y;
                float len2 = dot(d, d);
                float w = inversesqrt(len2) / len2;
                d *= inversesqrt(len2);
                sum += texelFetch(u_faces, ivec3(i, j, layer + f), 0).rgb * (w * (1.0 + dot(d, d_k)));
                wsum += w;
            }
        }
    }
    o_color = vec4(sum / wsum, g_params.y);
}
)";

// The irradiance volume as RGBM RGBA8 (the editor's bake reads it back), texel for texel.
constexpr const char* kGiEncodeFS = R"(#version 450 core
layout(std140, binding = 1) uniform ExportU {
    vec4 e_params;  // z: RGBM range
};
layout(binding = 6) uniform sampler2D u_gi_src;
layout(location = 0) in vec2 v_uv;
layout(location = 0) out vec4 o_color;
void main() {
    vec3 c = max(textureLod(u_gi_src, v_uv, 0.0).rgb, vec3(0.0));
    float m = clamp(max(max(c.r, c.g), max(c.b, 1e-6)) / e_params.z, 1.0 / 255.0, 1.0);
    m = ceil(m * 255.0) / 255.0;
    o_color = vec4(min(c / (m * e_params.z), vec3(1.0)), m);
}
)";

// ---- volumetric fog (M-VISUAL 7). Both passes draw the whole atlas (a tile of froxel columns ×
// rows per depth slice); FrameU and PassU are the main pass's.
// Injection: each froxel's medium at its center, rgb = the light it scatters toward the camera per
// meter (σs · in-scattered radiance), a = its extinction σt per meter.
constexpr const char* kVolInjectFS = "#version 450 core\n" MCE_GLSL_FRAME_U MCE_GLSL_PASS_U MCE_GLSL_LIGHTS MCE_GLSL_VOLUME R"(
layout(location = 0) in vec2 v_uv;
layout(location = 0) out vec4 o_color;
float hg(float g, float c) { return (1.0 - g * g) / (4.0 * PI * pow(max(1.0 + g * g - 2.0 * g * c, 1e-4), 1.5)); }
void main() {
    vec2 atlas = p_vol_grid.xy * p_vol_grid.zw;
    vec2 cell = min(floor(v_uv * atlas), atlas - 1.0);
    vec2 tile = floor(cell / p_vol_grid.xy);
    int slice = int(tile.x) + int(tile.y) * int(p_vol_grid.z);
    if (slice >= int(p_vol.y)) {
        o_color = vec4(0.0);
        return;
    }
    vec2 uv = (cell - tile * p_vol_grid.xy + 0.5) / p_vol_grid.xy;
    float depth = vol_depth_of(float(slice) + 0.5);
    vec4 w = u_inv_view_proj * vec4(uv * 2.0 - 1.0, 1.0, 1.0);
    vec3 ray = w.xyz / w.w - u_camera_pos.xyz;
    vec3 fwd = -vec3(u_view[0][2], u_view[1][2], u_view[2][2]);
    vec3 p = u_camera_pos.xyz + ray * (depth / dot(ray, fwd));
    vec3 rd = normalize(ray);
    float sigma_t = p_vol_medium.x * (p_vol_medium.y > 0.0 ? exp(-p_vol_medium.y * max(p.y - p_vol_medium.z, 0.0)) : 1.0);
    float g = p_vol_medium.w;
    // The sun through its cascades (shafts where something casts a shadow). The engine's light
    // scale I is irradiance / π, so the scattered radiance is σs · π · I · phase.
    vec3 l = -p_light_dir.xyz;
    vec3 lin = p_light_color.rgb * (PI * hg(g, dot(l, rd)) * shadow_term(p, l, l, depth) * p_vol_albedo.w);
    // Point and spot lights of the froxel's cluster, with their shadows.
    uvec3 grid = uvec3(u_cluster.xyz);
    uint tx = min(uint(uv.x * u_cluster.x), grid.x - 1u), ty = min(uint(uv.y * u_cluster.y), grid.y - 1u);
    uint tz = uint(clamp(int(floor(log(max(depth, 1e-4)) * u_cluster_z.x + u_cluster_z.y)), 0, int(grid.z) - 1));
    uvec2 range = c_ranges[tx + grid.x * (ty + grid.y * tz)];
    for (uint i = 0u; i < range.y; ++i) {
        Light L = l_lights[l_indices[range.x + i]];
        vec3 dl = L.pos_radius.xyz - p;
        float d2 = dot(dl, dl);
        float f = d2 * L.color.w;
        float win = clamp(1.0 - f * f, 0.0, 1.0);
        float att = win * win / max(d2, 0.01);
        vec3 ld = dl * inversesqrt(d2);
        float cone = clamp(dot(-ld, L.spot.xyz) + L.spot.w, 0.0, 1.0);
        att *= cone * cone;
        if (att <= 0.0) continue;
        if (L.shadow.x >= 0.0 && p_local.x > 0.5) att *= local_shadow(L.shadow, L.pos_radius.xyz, p, ld, ld);
        lin += L.color.rgb * (PI * att * hg(g, dot(ld, rd)) * p_vol_scale.x);
    }
    // The ambient, isotropic: the sphere's mean radiance, between the sky's and the ground's.
    lin += (p_amb_sky.rgb + p_amb_ground.rgb) * (0.5 * p_vol_scale.y);
    o_color = vec4(p_vol_albedo.rgb * sigma_t * lin, sigma_t);
}
)";

// Integration front to back along each froxel column, up to this froxel's slice (energy-conserving
// over each slice's thickness, Hillaire 2015): rgb = scattered light reaching the camera, a = the
// transmittance, both at the slice's far end.
constexpr const char* kVolIntegrateFS = "#version 450 core\n" MCE_GLSL_FRAME_U MCE_GLSL_PASS_U R"(
layout(binding = 6) uniform sampler2D u_media;
layout(location = 0) in vec2 v_uv;
layout(location = 0) out vec4 o_color;
const float kVolD0 = 1.0;
float vol_depth_of(float s) { return kVolD0 * (exp2(s / p_vol.y * p_vol.w) - 1.0); }
void main() {
    vec2 atlas = p_vol_grid.xy * p_vol_grid.zw;
    vec2 cell = min(floor(v_uv * atlas), atlas - 1.0);
    vec2 tile = floor(cell / p_vol_grid.xy);
    int slice = int(tile.x) + int(tile.y) * int(p_vol_grid.z);
    if (slice >= int(p_vol.y)) {
        o_color = vec4(0.0, 0.0, 0.0, 1.0);
        return;
    }
    ivec2 xy = ivec2(cell - tile * p_vol_grid.xy);
    vec2 uv = (vec2(xy) + 0.5) / p_vol_grid.xy;
    vec4 w = u_inv_view_proj * vec4(uv * 2.0 - 1.0, 1.0, 1.0);
    vec3 ray = w.xyz / w.w - u_camera_pos.xyz;
    vec3 fwd = -vec3(u_view[0][2], u_view[1][2], u_view[2][2]);
    float stretch = length(ray) / dot(ray, fwd);  // meters along the ray per meter of view depth
    int cols = int(p_vol_grid.z);
    vec3 light = vec3(0.0);
    float trans = 1.0;
    for (int k = 0; k <= slice; ++k) {
        vec4 m = texelFetch(u_media, ivec2(k % cols, k / cols) * ivec2(p_vol_grid.xy) + xy, 0);
        float len = (vol_depth_of(float(k + 1)) - vol_depth_of(float(k))) * stretch;
        float t = exp(-m.a * len);
        vec3 s = m.a > 1e-7 ? m.rgb * (1.0 - t) / m.a : m.rgb * len;
        light += trans * s;
        trans *= t;
    }
    o_color = vec4(light, trans);
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

// ---- scale (M-VISUAL 9) ---------------------------------------------------------------------------
// Instance batches: per-instance world matrices (bank 13) and the visible lists the culling wrote
// (bank 14); a draw's instance gl_InstanceID reads slot o_skin.y + gl_InstanceID (or its pass's
// own block's). Normals go through the matrix's cofactor (any scale). Wind sways a vertex sideways
// by amplitude × h², h its height above the instance origin.
#define MCE_GLSL_INSTANCES R"(
layout(std430, binding = 13) readonly buffer Instances { mat4 i_world[]; };
layout(std430, binding = 14) readonly buffer Visible { uint i_visible[]; };
mat4 instance_world(float first) { return i_world[i_visible[uint(first) + uint(gl_InstanceID)]]; }
mat3 cofactor3(mat4 m) { return mat3(cross(m[1].xyz, m[2].xyz), cross(m[2].xyz, m[0].xyz), cross(m[0].xyz, m[1].xyz)); }
uint pcg_hash(uint v) {
    uint s = v * 747796405u + 2891336453u;
    uint g = ((s >> ((s >> 28u) + 4u)) ^ s) * 277803737u;
    return (g >> 22u) ^ g;
}
float hash01(vec3 p) {  // [0, 1) from the float bits of p: the same input gives the same value on every device
    uvec3 u = floatBitsToUint(p);
    return float(pcg_hash(u.x ^ pcg_hash(u.y ^ pcg_hash(u.z))) >> 8) * (1.0 / 16777216.0);
}
// ext (M-VISUAL3 6): x wind_height (profile (h / H)^2 when > 0), y gust, z flutter; all 0 = amplitude * h^2 as before.
// `key` is the vertex's own a_pos: the flutter hashes it with the instance origin (bit-exact inputs, not the computed position).
vec3 wind_offset(mat4 w, vec3 p, float t, vec4 wind, vec3 ext, vec3 key) {
    if (wind.x == 0.0) return vec3(0.0);
    float h = max(p.y - w[3].y, 0.0);
    float f = 6.2831853 * wind.y * t + dot(w[3].xz, vec2(0.37, 0.71));
    if (ext == vec3(0.0)) return vec3(wind.z, 0.0, wind.w) * (wind.x * h * h * (sin(f) + 0.35 * sin(2.3 * f + 1.7)));
    float hh = max(ext.x, 1e-2);  // a tiny wind height must not blow the profile up
    float r = ext.x > 0.0 ? h / hh : h;
    float gust = 1.0;
    if (ext.y > 0.0) {  // slow gusts running along the wind at 3 m/s
        float s = dot(w[3].xz, wind.zw) - 3.0 * t;
        gust = 1.0 + ext.y * (0.6 * sin(0.35 * s) + 0.4 * sin(0.83 * s + 1.3));
    }
    vec3 o = vec3(wind.z, 0.0, wind.w) * (wind.x * (ext.x > 0.0 ? r * r : h * h) * gust * (sin(f) + 0.35 * sin(2.3 * f + 1.7)));
    if (ext.z > 0.0) {  // leaf flutter: 4 to 6 Hz, a phase per vertex
        float ph = 6.2831853 * (4.0 + 2.0 * hash01(key.yzx + w[3].xyz)) * t + 6.2831853 * hash01(key + w[3].zxy);
        o += ext.z * r * vec3(sin(ph), 0.5 * sin(1.31 * ph + 0.7), cos(0.93 * ph));
    }
    return o;
}
vec3 oct_decode(vec2 e) {
    vec3 n = vec3(e, 1.0 - abs(e.x) - abs(e.y));
    if (n.z < 0.0) n.xy = (1.0 - abs(n.yx)) * vec2(n.x >= 0.0 ? 1.0 : -1.0, n.y >= 0.0 ? 1.0 : -1.0);
    return normalize(n);
}
)"

// The instanced twin of kMeshVS: o_model is the submesh's node * dequant, the instance's matrix goes
// on top. Feeds kMeshFS and kAoPrepassFS.
constexpr const char* kInstancedMeshVS = "#version 450 core\n" MCE_GLSL_FRAME_U MCE_GLSL_OBJECT_U MCE_GLSL_INSTANCES R"(
layout(location = 0) in vec4 a_pos;
layout(location = 1) in vec2 a_oct;
layout(location = 2) in vec2 a_uv;
layout(location = 0) out vec3 v_world;
layout(location = 1) out vec3 v_normal;
layout(location = 2) out vec2 v_uv;
layout(location = 3) flat out vec4 v_tint;
out gl_PerVertex { vec4 gl_Position; };
void main() {
    mat4 w = instance_world(o_skin.y);
    vec4 p = w * (o_model * vec4(a_pos.xyz, 1.0));
    // M-VISUAL3 6: a batch draw leaves o_dequant (identity for rigid draws, unread here) to carry the variation (column 0)
    // and the wind extension (column 1: height, gust, flutter), so ObjectU keeps its size.
    p.xyz += wind_offset(w, p.xyz, o_skin.z, o_inst, o_dequant[1].xyz, a_pos.xyz);
    vec4 tv = o_dequant[0];
    if (o_params.x > 0.0) tv.w = 1.0;  // alpha-masked: the depth passes mask by the base alpha alone, so the colour variation leaves alpha out
    v_tint = tv == vec4(1.0) ? vec4(1.0) : mix(vec4(1.0), tv, hash01(w[3].xyz));
    v_world = p.xyz;
    v_normal = cofactor3(w) * (mat3(o_normal) * oct_decode(a_oct));
    v_uv = o_uv_rect.xy + a_uv * o_uv_rect.zw;
    gl_Position = u_view_proj * p;
}
)";

// Shadow and velocity passes of a batch: their own per-draw block, alpha-masked like the main pass.
#define MCE_GLSL_INST_PASS_U R"(
layout(std140, binding = 3) uniform InstDrawU {
    mat4 s_view_proj;  // shadows: the cascade's; velocity: unused (VelPassU)
    mat4 s_local;      // node * dequant
    vec4 s_info;       // x: first visible slot, y: time, z: previous time (velocity), w: alpha cutoff (0 = none)
    vec4 s_wind;       // as ObjectU::o_inst
    vec4 s_uv_rect;
    vec4 s_base;       // x: base color alpha, yzw: wind height, gust, flutter (M-VISUAL3 6)
};
layout(binding = 6) uniform sampler2DArray u_albedo;
)"
constexpr const char* kInstancedShadowVS = "#version 450 core\n" MCE_GLSL_INST_PASS_U MCE_GLSL_INSTANCES R"(
layout(location = 0) in vec4 a_pos;
layout(location = 2) in vec2 a_uv;
layout(location = 0) out vec2 v_uv;
out gl_PerVertex { vec4 gl_Position; };
void main() {
    mat4 w = instance_world(s_info.x);
    vec4 p = w * (s_local * vec4(a_pos.xyz, 1.0));
    p.xyz += wind_offset(w, p.xyz, s_info.y, s_wind, s_base.yzw, a_pos.xyz);
    v_uv = s_uv_rect.xy + a_uv * s_uv_rect.zw;
    gl_Position = s_view_proj * p;
}
)";
constexpr const char* kInstancedShadowFS = "#version 450 core\n" MCE_GLSL_INST_PASS_U R"(
layout(location = 0) in vec2 v_uv;
void main() {
    if (s_info.w > 0.0 && texture(u_albedo, vec3(v_uv, 0.0)).a * s_base.x < s_info.w) discard;
}
)";
constexpr const char* kInstancedVelocityVS = "#version 450 core\n" MCE_GLSL_INST_PASS_U MCE_GLSL_INSTANCES R"(
layout(std140, binding = 1) uniform VelPassU { mat4 vp_jittered; mat4 vp_current; mat4 vp_previous; };
layout(location = 0) in vec4 a_pos;
layout(location = 2) in vec2 a_uv;
layout(location = 0) out vec4 v_current;
layout(location = 1) out vec4 v_previous;
layout(location = 2) out vec2 v_uv;
out gl_PerVertex { vec4 gl_Position; };
void main() {
    mat4 w = instance_world(s_info.x);
    vec4 base = w * (s_local * vec4(a_pos.xyz, 1.0));
    vec4 p = base, q = base;
    p.xyz += wind_offset(w, base.xyz, s_info.y, s_wind, s_base.yzw, a_pos.xyz);
    q.xyz += wind_offset(w, base.xyz, s_info.z, s_wind, s_base.yzw, a_pos.xyz);
    v_uv = s_uv_rect.xy + a_uv * s_uv_rect.zw;
    gl_Position = vp_jittered * p;
    v_current = vp_current * p;
    v_previous = vp_previous * q;
}
)";
constexpr const char* kInstancedVelocityFS = "#version 450 core\n" MCE_GLSL_INST_PASS_U R"(
layout(location = 0) in vec4 v_current;
layout(location = 1) in vec4 v_previous;
layout(location = 2) in vec2 v_uv;
layout(location = 0) out vec4 o_velocity;
void main() {
    if (s_info.w > 0.0 && texture(u_albedo, vec3(v_uv, 0.0)).a * s_base.x < s_info.w) discard;
    if (v_previous.w <= 1e-4) { o_velocity = vec4(16.0, 16.0, 1.0, v_current.w); return; }
    o_velocity = vec4((v_current.xy / v_current.w - v_previous.xy / v_previous.w) * 0.5, 1.0, v_current.w);
}
)";
#undef MCE_GLSL_INST_PASS_U

// Occluders for the batches' occlusion test: the frame's rigid opaques, their view depth (meters along
// the view axis) into an RGBA32F target cleared far.
constexpr const char* kOccluderVS = R"(#version 450 core
layout(std140, binding = 3) uniform OccU { mat4 c_mvp; mat4 c_mv; };
layout(location = 0) in vec4 a_pos;
layout(location = 0) out float v_depth;
out gl_PerVertex { vec4 gl_Position; };
void main() {
    vec4 p = vec4(a_pos.xyz, 1.0);
    v_depth = -(c_mv * p).z;
    gl_Position = c_mvp * p;
}
)";
constexpr const char* kOccluderFS = R"(#version 450 core
layout(location = 0) in float v_depth;
layout(location = 0) out vec4 o_depth;
void main() { o_depth = vec4(v_depth); }
)";

// One level of the depth pyramid: each texel the farthest of the kHizRatio² (clamped) texels under it.
constexpr const char* kHizReduceCS = R"(#version 450 core
layout(local_size_x = 8, local_size_y = 8) in;
layout(std140, binding = 1) uniform HizU { vec4 h_size; };  // xy: source size, zw: this level's size
layout(binding = 6) uniform sampler2D u_src;
layout(rgba32f, binding = 0) uniform writeonly image2D u_dst;
void main() {
    ivec2 d = ivec2(gl_GlobalInvocationID.xy);
    ivec2 dst = ivec2(h_size.zw), src = ivec2(h_size.xy);
    if (d.x >= dst.x || d.y >= dst.y) return;
    ivec2 lo = d * 4, hi = min(lo + 4, src);
    float m = 0.0;
    for (int y = lo.y; y < hi.y; ++y)
        for (int x = lo.x; x < hi.x; ++x) m = max(m, texelFetch(u_src, ivec2(x, y), 0).r);
    imageStore(u_dst, d, vec4(m));
}
)";

// The indirect commands start each frame from their template (index ranges, zero instances).
constexpr const char* kInstanceArgsInitCS = R"(#version 450 core
layout(local_size_x = 64) in;
layout(std140, binding = 1) uniform InitU { uvec4 n_count; };  // x: uints to copy
layout(std430, binding = 13) readonly buffer Template { uint t_words[]; };
layout(std430, binding = 15) writeonly buffer Args { uint a_words[]; };
void main() {
    uint i = gl_GlobalInvocationID.x;
    if (i < n_count.x) a_words[i] = t_words[i];
}
)";

// Culls one batch for one view: the frustum, the cull distance, the depth pyramid (view only), then the
// LOD level; a visible instance takes the next slot of its level's list and counts into the level's
// commands (one per submesh).
constexpr const char* kInstanceCullCS = R"(#version 450 core
layout(local_size_x = 64) in;
layout(std140, binding = 1) uniform CullU {
    mat4 c_view_proj;  // the view's (the depth pyramid's projection)
    vec4 c_planes[6];
    vec4 c_eye;        // xyz: the main camera (LOD, cull distance), w: cull distance (0 = none)
    vec4 c_forward;    // xyz: the main view axis, w: near plane
    vec4 c_sphere;     // the mesh's bounding sphere: center, radius
    vec4 c_batch;      // x: instances, y: first visible slot, z: first command, w: submeshes
    vec4 c_lod;        // x: levels, y: pixels per world unit at 1 m (proj[1][1] * height / 2), z: level bias, w: forced level (-1 = none)
    vec4 c_lod_error;  // error of levels 0..3 (0 when LOD is off)
    vec4 c_lod_px;     // x: pixel error
    vec4 c_hiz;        // x: on, y, z: level 0 size, w: levels
};
layout(std430, binding = 13) readonly buffer Instances { mat4 i_world[]; };
layout(std430, binding = 14) writeonly buffer Visible { uint v_slots[]; };
layout(std430, binding = 15) buffer Args { uint a_words[]; };
layout(binding = 0) uniform sampler2D u_hiz0;
layout(binding = 1) uniform sampler2D u_hiz1;
layout(binding = 2) uniform sampler2D u_hiz2;
layout(binding = 3) uniform sampler2D u_hiz3;
layout(binding = 4) uniform sampler2D u_hiz4;
float hiz_fetch(int level, ivec2 p) {
    if (level == 0) return texelFetch(u_hiz0, p, 0).r;
    if (level == 1) return texelFetch(u_hiz1, p, 0).r;
    if (level == 2) return texelFetch(u_hiz2, p, 0).r;
    if (level == 3) return texelFetch(u_hiz3, p, 0).r;
    return texelFetch(u_hiz4, p, 0).r;
}
// True when the sphere lies behind the pyramid's farthest occluder over its screen rectangle.
bool occluded(vec3 c, float r) {
    float near_depth = dot(c - c_eye.xyz, c_forward.xyz) - r;
    if (near_depth <= c_forward.w) return false;
    vec2 lo = vec2(1e30), hi = vec2(-1e30);
    for (int i = 0; i < 8; ++i) {
        vec3 k = c + r * vec3((i & 1) != 0 ? 1.0 : -1.0, (i & 2) != 0 ? 1.0 : -1.0, (i & 4) != 0 ? 1.0 : -1.0);
        vec4 q = c_view_proj * vec4(k, 1.0);
        if (q.w <= 1e-4) return false;
        vec2 ndc = q.xy / q.w;
        lo = min(lo, ndc);
        hi = max(hi, ndc);
    }
    vec2 size = c_hiz.yz;
    lo = clamp(lo * 0.5 + 0.5, 0.0, 1.0) * size;
    hi = clamp(hi * 0.5 + 0.5, 0.0, 1.0) * size;
    float extent = max(hi.x - lo.x, hi.y - lo.y);
    int level = 0;
    float span = 1.0;
    while (span < extent && level < int(c_hiz.w) - 1) {
        span *= 4.0;
        ++level;
    }
    if (span < extent) return false;  // larger than the coarsest level's texels
    ivec2 lsize = max(ivec2(ceil(size / span)), ivec2(1));
    ivec2 a = clamp(ivec2(floor(lo / span)), ivec2(0), lsize - 1);
    ivec2 b = clamp(ivec2(floor(hi / span)), ivec2(0), lsize - 1);
    float far_depth = max(max(hiz_fetch(level, a), hiz_fetch(level, ivec2(b.x, a.y))),
                          max(hiz_fetch(level, ivec2(a.x, b.y)), hiz_fetch(level, b)));
    return near_depth > far_depth;
}
void main() {
    uint i = gl_GlobalInvocationID.x;
    if (i >= uint(c_batch.x)) return;
    mat4 w = i_world[i];
    float scale = max(max(length(w[0].xyz), length(w[1].xyz)), length(w[2].xyz));
    vec3 c = (w * vec4(c_sphere.xyz, 1.0)).xyz;
    float r = c_sphere.w * scale;
    for (int p = 0; p < 6; ++p)
        if (dot(c_planes[p].xyz, c) + c_planes[p].w < -r) return;
    if (c_eye.w > 0.0 && length(c - c_eye.xyz) - r > c_eye.w) return;
    if (c_hiz.x > 0.5 && occluded(c, r)) return;
    uint levels = uint(c_lod.x);
    uint level = 0u;
    if (c_lod.w >= 0.0) {
        level = min(uint(c_lod.w), levels - 1u);
    } else if (levels > 1u) {
        float px = c_lod.y / max(length(c - c_eye.xyz) - r, c_forward.w);
        for (uint l = 1u; l < levels; ++l)
            if (c_lod_error[l] * scale * px <= c_lod_px.x) level = l;
    }
    level = min(level + uint(c_lod.z), levels - 1u);
    uint subs = uint(c_batch.w), n = uint(c_batch.x);
    uint cmd = uint(c_batch.z) + level * subs;
    uint slot = atomicAdd(a_words[cmd * 5u + 1u], 1u);
    for (uint s = 1u; s < subs; ++s) atomicAdd(a_words[(cmd + s) * 5u + 1u], 1u);
    v_slots[uint(c_batch.y) + level * n + slot] = i;
}
)";

// GPU particles: two vec4 per particle (position, age; velocity, lifetime), lifetime 0 = never spawned.
// The particles in the ring range [first, first + count) are (re)spawned, every other live one moves.
#define MCE_GLSL_PARTICLES(qual) "layout(std430, binding = 13) " qual " buffer Particles { vec4 p_data[]; };\n"
constexpr const char* kParticleSimCS = "#version 450 core\n" MCE_GLSL_PARTICLES("") R"(
layout(local_size_x = 64) in;
layout(std140, binding = 1) uniform SimU {
    vec4 s_spawn;    // x: first ring slot, y: count, z: capacity, w: seed (uint bits)
    vec4 s_step;     // x: dt, y: particles spawned before this frame (uint bits; the random stream), z: drag, w: bounce
    vec4 s_origin;   // xyz: position, w: spread (radians)
    vec4 s_box;      // xyz: spawn half extents, w: collide (> 0.5)
    vec4 s_dir;      // xyz: direction (normalized), w: ground height
    vec4 s_ranges;   // speed min, max, lifetime min, max
    vec4 s_gravity;  // xyz
};
uint hash(uint x) {
    x ^= x >> 16; x *= 0x7feb352du;
    x ^= x >> 15; x *= 0x846ca68bu;
    x ^= x >> 16;
    return x;
}
float rnd(inout uint state) {
    state = hash(state + 0x9e3779b9u);
    return float(state >> 8) * (1.0 / 16777216.0);
}
void main() {
    uint i = gl_GlobalInvocationID.x;
    uint cap = uint(s_spawn.z);
    if (i >= cap) return;
    uint k = (i + cap - uint(s_spawn.x)) % cap;
    float dt = s_step.x;
    if (k < uint(s_spawn.y)) {
        uint state = hash(floatBitsToUint(s_spawn.w) * 0x27d4eb2du ^ hash(floatBitsToUint(s_step.y) + k));
        vec3 p = s_origin.xyz + s_box.xyz * (vec3(rnd(state), rnd(state), rnd(state)) * 2.0 - 1.0);
        vec3 d = s_dir.xyz;
        vec3 t = normalize(abs(d.y) < 0.99 ? cross(d, vec3(0.0, 1.0, 0.0)) : cross(d, vec3(1.0, 0.0, 0.0)));
        vec3 b = cross(d, t);
        float ct = mix(cos(s_origin.w), 1.0, rnd(state));
        float st = sqrt(max(1.0 - ct * ct, 0.0));
        float ph = 6.2831853 * rnd(state);
        vec3 dir = d * ct + (t * cos(ph) + b * sin(ph)) * st;
        float speed = mix(s_ranges.x, s_ranges.y, rnd(state));
        float life = mix(s_ranges.z, s_ranges.w, rnd(state));
        p_data[i * 2u] = vec4(p, 0.0);
        p_data[i * 2u + 1u] = vec4(dir * speed, max(life, 1e-3));
        return;
    }
    vec4 a = p_data[i * 2u], v = p_data[i * 2u + 1u];
    if (v.w <= 0.0 || a.w >= v.w) return;
    v.xyz = (v.xyz + s_gravity.xyz * dt) * exp(-s_step.z * dt);
    a.xyz += v.xyz * dt;
    if (s_box.w > 0.5 && a.y < s_dir.w && v.y < 0.0) {
        a.y = s_dir.w;
        v.y = -v.y * s_step.w;
        v.xz *= 0.8;
    }
    a.w += dt;
    p_data[i * 2u] = a;
    p_data[i * 2u + 1u] = v;
}
)";
// Camera-facing sprites: six vertices per particle (gl_InstanceID), dead ones off screen.
constexpr const char* kParticleVS = "#version 450 core\n" MCE_GLSL_FRAME_U MCE_GLSL_PARTICLES("readonly") R"(
layout(std140, binding = 3) uniform SpriteU {
    vec4 q_size;    // x: start, y: end width (meters)
    vec4 q_start;   // color at birth
    vec4 q_end;     // color at death
    vec4 q_info;    // x: textured
};
layout(location = 0) out vec2 v_uv;
layout(location = 1) out vec4 v_color;
out gl_PerVertex { vec4 gl_Position; };
void main() {
    uint i = uint(gl_InstanceID);
    vec4 a = p_data[i * 2u], v = p_data[i * 2u + 1u];
    if (v.w <= 0.0 || a.w >= v.w) {
        gl_Position = vec4(0.0, 0.0, 2.0, 1.0);  // outside the clip volume
        v_uv = vec2(0.0);
        v_color = vec4(0.0);
        return;
    }
    float t = clamp(a.w / v.w, 0.0, 1.0);
    const vec2 kCorner[6] = vec2[6](vec2(-1, -1), vec2(1, -1), vec2(1, 1), vec2(-1, -1), vec2(1, 1), vec2(-1, 1));
    vec2 c = kCorner[gl_VertexID % 6];
    vec3 right = vec3(u_view[0][0], u_view[1][0], u_view[2][0]);
    vec3 up = vec3(u_view[0][1], u_view[1][1], u_view[2][1]);
    float half_size = 0.5 * mix(q_size.x, q_size.y, t);
    vec3 p = a.xyz + (right * c.x + up * c.y) * half_size;
    v_uv = c * 0.5 + 0.5;
    v_color = mix(q_start, q_end, t);
    gl_Position = u_view_proj * vec4(p, 1.0);
}
)";
constexpr const char* kParticleFS = R"(#version 450 core
layout(std140, binding = 3) uniform SpriteU { vec4 q_size; vec4 q_start; vec4 q_end; vec4 q_info; };
layout(binding = 6) uniform sampler2DArray u_sprite;
layout(location = 0) in vec2 v_uv;
layout(location = 1) in vec4 v_color;
layout(location = 0) out vec4 o_color;
void main() {
    float cover;
    vec3 tint = vec3(1.0);
    if (q_info.x > 0.5) {
        vec4 s = texture(u_sprite, vec3(v_uv, 0.0));
        cover = s.a;
        tint = s.rgb;
    } else {
        float d = length(v_uv * 2.0 - 1.0);
        cover = clamp(1.0 - d, 0.0, 1.0);
        cover *= cover;
    }
    float a = cover * v_color.a;
    o_color = vec4(tint * v_color.rgb * a, a);  // premultiplied (the additive pipeline ignores a)
}
)";
#undef MCE_GLSL_PARTICLES
#undef MCE_GLSL_INSTANCES

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

// ---- post effects (M-VISUAL 8) --------------------------------------------------------------------

// Auto exposure: a 64-bin log2-luminance histogram of every e_src.z-th texel (bin 0 = darker than the
// range), then a single-group reduction to the adapted exposure. Both read ExpU.
#define MCE_GLSL_EXPOSURE_U R"(
layout(std140, binding = 1) uniform ExpU {
    vec4 e_src;     // source width, height, texel step, -
    vec4 e_range;   // min log2 luminance, bins per log2 unit, adaptation rate brightening, darkening (per s)
    vec4 e_target;  // key, compensation (EV), low share, high share
    vec4 e_adapt;   // dt, snap (> 0.5), min exposure, max exposure
};
)"
constexpr const char* kExposureHistCS = "#version 450 core\n" MCE_GLSL_EXPOSURE_U MCE_GLSL_EXPOSURE("") R"(
layout(local_size_x = 16, local_size_y = 16) in;
layout(binding = 6) uniform sampler2D u_src;
shared uint s_bins[64];
void main() {
    uint li = gl_LocalInvocationIndex;
    if (li < 64u) s_bins[li] = 0u;
    barrier();
    ivec2 p = ivec2(gl_GlobalInvocationID.xy) * int(e_src.z);
    if (p.x < int(e_src.x) && p.y < int(e_src.y)) {
        float l = dot(texelFetch(u_src, p, 0).rgb, vec3(0.2126, 0.7152, 0.0722));
        uint bin = 0u;
        if (l > exp2(e_range.x)) bin = uint(clamp((log2(l) - e_range.x) * e_range.y, 0.0, 62.0)) + 1u;
        atomicAdd(s_bins[bin], 1u);
    }
    barrier();
    if (li < 64u && s_bins[li] > 0u) atomicAdd(e_bins[li], s_bins[li]);
}
)";
constexpr const char* kExposureReduceCS = "#version 450 core\n" MCE_GLSL_EXPOSURE_U MCE_GLSL_EXPOSURE("") R"(
layout(local_size_x = 64) in;
shared uint s_bins[64];
void main() {
    uint i = gl_LocalInvocationIndex;
    s_bins[i] = e_bins[i];
    e_bins[i] = 0u;  // empty for the next frame's histogram
    barrier();
    if (i != 0u) return;
    float total = 0.0;
    for (int k = 0; k < 64; ++k) total += float(s_bins[k]);
    // The mean log2 luminance of the share between low and high (the extremes dropped).
    float lo = total * e_target.z, hi = total * e_target.w, acc = 0.0, sum = 0.0, w = 0.0;
    for (int k = 0; k < 64; ++k) {
        float n = float(s_bins[k]);
        float a = max(min(acc + n, hi) - max(acc, lo), 0.0);
        acc += n;
        float lg = k == 0 ? e_range.x - 1.0 : e_range.x + (float(k) - 0.5) / e_range.y;
        sum += a * lg;
        w += a;
    }
    float avg = w > 0.0 ? sum / w : log2(e_target.x);
    float target = log2(clamp(e_target.x * exp2(e_target.y - avg), e_adapt.z, e_adapt.w));
    float cur = (e_frames == 0u || e_adapt.y > 0.5) ? target : log2(max(e_exposure, 1e-6));
    float d = target - cur;
    float rate = d < 0.0 ? e_range.z : e_range.w;  // falling exposure = a brighter scene
    e_exposure = exp2(cur + d * (1.0 - exp(-e_adapt.x * rate)));
    e_average = exp2(avg);
    e_frames += 1u;
}
)";
#undef MCE_GLSL_EXPOSURE_U
#undef MCE_GLSL_EXPOSURE

// Depth of field: a 32-tap disk gather (Vogel spiral) out to the largest circle of confusion. Each tap
// counts where its own circle reaches this pixel; a tap behind this pixel counts only as far as this
// pixel's own circle, so a blurred background does not spill over a sharp foreground.
constexpr const char* kDofFS = R"(#version 450 core
layout(std140, binding = 1) uniform DofU {
    vec4 d_texel;  // output texel size (xy), size (zw)
    vec4 d_focus;  // focus distance, in-focus half range, blur range (m), max radius (output px)
};
layout(binding = 6) uniform sampler2D u_src;
layout(binding = 7) uniform sampler2D u_vel;  // b: covered, a: view depth
layout(location = 0) in vec2 v_uv;
layout(location = 0) out vec4 o_color;
float depth_at(vec2 uv) { vec4 v = textureLod(u_vel, uv, 0.0); return v.b > 0.5 ? v.a : 1e5; }
float coc_of(float z) { return clamp((abs(z - d_focus.x) - d_focus.y) / max(d_focus.z, 1e-3), 0.0, 1.0) * d_focus.w; }
void main() {
    float z0 = depth_at(v_uv), c0 = coc_of(z0);
    vec3 sum = texelFetch(u_src, ivec2(gl_FragCoord.xy), 0).rgb;  // exact: a sharp pixel passes through
    float wsum = 1.0;
    const int N = 32;
    for (int i = 0; i < N; ++i) {
        float r = sqrt((float(i) + 0.5) / float(N)) * d_focus.w;
        float a = float(i) * 2.39996323;
        vec2 uv = v_uv + vec2(cos(a), sin(a)) * r * d_texel.xy;
        float z = depth_at(uv), c = coc_of(z);
        if (z > z0) c = min(c, c0);
        float w = clamp(c - r, 0.0, 1.0);
        sum += textureLod(u_src, uv, 0.0).rgb * w;
        wsum += w;
    }
    o_color = vec4(sum / wsum, 1.0);
}
)";

// Motion blur: m_params.z taps along the longest velocity in this pixel's neighborhood (the velocity
// prepass's for the objects, the camera's reprojection for the sky), centered on it. A tap counts where
// its own blur or this pixel's reaches the other, so a moving object smears over the still background
// beside it and the background shows through its trailing edge (after McGuire et al. 2012, no depth).
constexpr const char* kMotionBlurFS = R"(#version 450 core
layout(std140, binding = 1) uniform MbU {
    mat4 m_reproject;  // last frame's view-proj * inverse(this frame's), both unjittered
    vec4 m_texel;      // output texel size (xy), size (zw)
    vec4 m_params;     // intensity, max length (uv of the height), taps, -
};
layout(binding = 6) uniform sampler2D u_src;
layout(binding = 7) uniform sampler2D u_vel;
layout(location = 0) in vec2 v_uv;
layout(location = 0) out vec4 o_color;
vec2 velocity(vec2 uv) {  // in output pixels, scaled and clamped
    vec4 v = textureLod(u_vel, uv, 0.0);
    vec2 vel;
    if (v.b > 0.5) {
        vel = v.x > 8.0 ? vec2(0.0) : v.xy;
    } else {
        vec4 p = m_reproject * vec4(uv * 2.0 - 1.0, 1.0, 1.0);
        vel = uv - (p.xy / p.w * 0.5 + 0.5);
    }
    vel *= m_params.x * m_texel.zw;
    float len = length(vel), cap = m_params.y * m_texel.w;
    return len > cap ? vel * (cap / len) : vel;
}
void main() {
    vec3 c = texelFetch(u_src, ivec2(gl_FragCoord.xy), 0).rgb;  // exact: a still pixel passes through
    vec2 vc = velocity(v_uv), vmax = vc;
    float r = m_params.y * m_texel.w;  // the neighborhood: as far as the longest blur reaches
    for (int i = 0; i < 8; ++i) {
        float a = float(i) * 0.785398;
        for (int k = 1; k <= 2; ++k) {
            vec2 v = velocity(v_uv + vec2(cos(a), sin(a)) * (r * 0.5 * float(k)) * m_texel.xy);
            if (dot(v, v) > dot(vmax, vmax)) vmax = v;
        }
    }
    float lmax = length(vmax), lc = length(vc);
    if (lmax < 0.5) { o_color = vec4(c, 1.0); return; }  // under half a pixel: untouched
    int n = int(m_params.z);
    vec3 sum = c;
    float wsum = 1.0;
    for (int i = 0; i < n; ++i) {
        float t = (float(i) + 0.5) / float(n) - 0.5;
        vec2 uv = v_uv + vmax * t * m_texel.xy;
        float dist = abs(t) * lmax;
        float li = length(velocity(uv));
        float w = max(clamp(0.5 * li - dist + 1.0, 0.0, 1.0), clamp(0.5 * lc - dist + 1.0, 0.0, 1.0));
        sum += textureLod(u_src, uv, 0.0).rgb * w;
        wsum += w;
    }
    o_color = vec4(sum / wsum, 1.0);
}
)";

}  // namespace mce::render::shaders3d
