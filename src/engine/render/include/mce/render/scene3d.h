#pragma once
// The 3D frame: camera, sun, fog, mesh instances -> culled, sorted draw lists (rhi-renderer.md
// §2.5, §3.4 steps 2-3, §3.5 "MVP — 3D diorama frame").
//
// Scene3D is rebuilt every frame from extract (it holds no scene state across frames; its arrays
// live in the allocator it is given, normally the frame arena):
//
//   Scene3D scene(renderer.meshes(), mem::frame());
//   scene.begin(camera, viewport);
//   scene.set_light(sun);
//   scene.set_fog(fog);
//   for (...) scene.add({mesh_key, world});   // + .pose for animated / skinned meshes
//   scene.add_point_light(lamp);        // Forward+ (M5a): any number, clustered at build()
//   scene.build();                      // frustum cull, 64-bit key radix sort, cascades, clusters
//   renderer.record_3d(cmd, scene);     // CSM, forward+ PBR HDR pass + sky, resolve, post chain
//
// Conventions (binding): right-handed, Y up, column vectors, GL clip z in [-1, 1]. Draw order is a
// function of (instances in submission order, camera) only — ties keep submission order.

#include "mce/containers/array.h"
#include "mce/math/math.h"
#include "mce/render/accessibility.h"
#include "mce/render/atmosphere.h"
#include "mce/render/clusters.h"
#include "mce/render/mesh.h"
#include "mce/rhi/types.h"

namespace mce::render {

struct Camera3D {
    math::vec3 position;
    math::quat orientation;  // looks down local -Z, +Y up
    f32 fov_y = math::radians(45.0f);
    f32 near_z = 0.1f;
    f32 far_z = 1000.0f;
};

// A camera on a sphere around `target`: yaw about +Y (0 = looking down -Z), pitch down from the
// horizon (positive looks down), `distance` meters away.
Camera3D orbit_camera(math::vec3 target, f32 yaw, f32 pitch, f32 distance, f32 fov_y = math::radians(45.0f));

struct DirectionalLight {
    math::vec3 direction{-0.4f, -1.0f, -0.3f};  // the way the light travels (normalized on use)
    math::vec3 color{1.0f, 0.96f, 0.9f};        // linear
    f32 intensity = 2.0f;
    math::vec3 ambient_sky{0.35f, 0.42f, 0.55f};
    math::vec3 ambient_ground{0.20f, 0.18f, 0.16f};
    bool shadows = true;
};

struct Fog {
    math::vec3 color{0.62f, 0.72f, 0.85f};  // linear; also the clear color of the main pass
    f32 density = 0.0f;                     // exp² fog: f = 1 - exp(-(density * distance)²); 0 = off
};

// The sky behind the scene (FR-R3D-080): a gradient dome with a sun disk, or an equirect texture.
// It also lights the scene's specular ambient (reflections sample it along R).
struct Sky {
    math::vec3 zenith{0.16f, 0.34f, 0.72f};   // linear radiance straight up
    math::vec3 horizon{0.62f, 0.74f, 0.88f};
    math::vec3 ground{0.22f, 0.21f, 0.20f};   // below the horizon
    f32 sun_size = 0.9994f;                    // cos of the sun disk's angular radius (~2°)
    f32 sun_intensity = 12.0f;                 // disk radiance = light color * light intensity * this
    u32 texture = ~0u;                         // TextureStore key of an equirect map; ~0u = gradient
    f32 texture_intensity = 1.0f;
    bool texture_additive = false;             // true: gradient + texture * intensity (a star field over a dusk gradient)
    f32 specular_ambient = 0.5f;               // scale of sky reflections on surfaces
    bool enabled = true;                       // false: clear to the fog color (the M4 look)
    // Image-based lighting (M-VISUAL 2): the sky lights the scene through an SH9 irradiance (in place
    // of DirectionalLight's two-color hemisphere) and a GGX pre-filtered copy with a BRDF LUT (in
    // place of the analytic split-sum). Re-baked when the sky changes; off = the frame without it.
    bool ibl = false;
    f32 diffuse_ambient = 1.0f;                // scale of the SH9 sky irradiance (ibl only)
    // TextureStore key of an equirect map the IBL bakes from instead of the visible sky (an HDR-ish
    // environment for the lighting while the dome shows something else); ~0u = the sky itself.
    u32 ibl_texture = ~0u;
    f32 ibl_texture_intensity = 1.0f;
};

// Volumetric fog (M-VISUAL 7): a froxel grid over the view (screen tiles × depth slices to
// `max_distance`) of a height fog medium, lit by the sun through its shadow cascades (light shafts),
// by the point and spot lights of the Forward+ clusters (with their local shadows) and by the
// ambient, integrated front to back and applied to every surface and the sky in place of the exp²
// Fog. Beyond `max_distance` the medium adds nothing more. Off draws the frame without it.
struct Volumetrics {
    bool enabled = false;
    f32 density = 0.03f;                 // extinction per meter at `base_height` and below
    f32 height_falloff = 0.15f;          // density × exp(-falloff · (y - base_height)) above it; 0 = uniform
    f32 base_height = 0.0f;
    math::vec3 albedo{0.9f, 0.9f, 0.9f}; // scattering / extinction (the fog's color under white light)
    f32 anisotropy = 0.5f;               // Henyey-Greenstein g: > 0 glows toward the lights
    f32 sun_intensity = 1.0f;            // scales of the light the fog scatters
    f32 local_intensity = 1.0f;
    f32 ambient_intensity = 1.0f;
    f32 max_distance = 48.0f;            // meters of view depth the grid covers
    u32 tile = 16;                       // screen pixels per froxel column (4..64)
    u32 slices = 64;                     // depth slices (8..128)
};

// The post chain (FR-R3D-090): bloom -> ACES tonemap (exposure) -> FXAA. Toggleable per frame.
struct PostFx {
    bool bloom = true;
    f32 bloom_intensity = 0.05f;   // fraction of the blurred image added back
    f32 bloom_threshold = 1.0f;    // soft bright-pass knee start (exposed scene units); 0 = no threshold
    bool fxaa = true;
    // Temporal anti-aliasing (M-VISUAL 5): a jittered projection, a velocity prepass and a history
    // resolve with neighborhood clipping; replaces FXAA (which is then skipped). With
    // Forward3DDesc::render_scale < 1 it also upsamples to the full output (temporal upscaling).
    bool taa = false;
    f32 taa_blend = 0.1f;  // weight of the new frame in the history

    // M-VISUAL 8. Auto exposure: a log-luminance histogram of the frame (compute) drops the darkest
    // `low_percent` and brightest 1 - `high_percent`, averages the rest and moves the exposure toward
    // key / average over `dt` (fast when brightening, slower when darkening). It multiplies
    // Scene3D::exposure(); the first frame snaps. Needs compute.
    struct AutoExposure {
        bool enabled = false;
        f32 key = 0.18f;            // the average luminance the exposure aims for (middle grey)
        f32 compensation = 0.0f;    // EV added to the target (+1 = twice as bright)
        f32 min_exposure = 0.03f;   // clamps of the auto factor
        f32 max_exposure = 32.0f;
        f32 low_percent = 0.5f;     // histogram share ignored at the dark end (0..1)
        f32 high_percent = 0.95f;   // ... and above this share at the bright end
        f32 speed_up = 3.0f;        // adaptation rates per second (exponential, in EV)
        f32 speed_down = 1.0f;
        f32 dt = 1.0f / 60.0f;      // seconds since the last frame; set it every frame
    } auto_exposure;
    // Color grading on the tonemapped (display-space) frame: a 256 × 16 strip LUT (16³; see
    // make_identity_lut), as Post2D's. The TextureStore key; 0 (or a key the store
    // does not hold) = off.
    u32 lut_key = 0;
    f32 lut_strength = 1.0f;  // 0 = ungraded, 1 = fully graded
    // Color-vision correction (M-ACCESS) in the tonemap pass, after the grade; set from the
    // player's settings by apply_accessibility, never by the game.
    ColorVision color_vision = ColorVision::None;
    f32 color_vision_strength = 1.0f;
    // Depth of field: a gather over a disk whose radius follows each pixel's circle of confusion: 0
    // within `focus_range` meters of `focus_distance`, growing to `max_radius` (a fraction of the view
    // height) over `blur_range` meters on either side. Reads the velocity prepass's depth.
    struct DepthOfField {
        bool enabled = false;
        f32 focus_distance = 8.0f;
        f32 focus_range = 1.0f;
        f32 blur_range = 6.0f;
        f32 max_radius = 0.012f;
    } dof;
    // Per-object motion blur: each pixel smears along its velocity from the velocity prepass (the
    // objects' own motion and the camera's), `intensity` of a frame's motion (a shutter fraction),
    // at most `max_length` of the view height.
    struct MotionBlur {
        bool enabled = false;
        f32 intensity = 0.5f;
        f32 max_length = 0.05f;
        u32 samples = 12;  // 4..32 taps along the velocity
    } motion_blur;
    // The velocity prepass runs for TAA and motion blur (both need the previous transforms) and for
    // depth of field (its depth).
    bool needs_velocity() const { return taa || motion_blur.enabled || dof.enabled; }
};

// Screen-space ambient occlusion (M-VISUAL 3): a half-resolution view-normal + depth prepass of the
// opaques, Alchemy SSAO over it, a depth-aware separable blur; the result darkens the ambient terms
// only (hemisphere or image-based, diffuse and specular), never the sun or point lights. Off draws
// the frame without it.
struct AmbientOcclusion {
    bool enabled = false;
    f32 radius = 0.6f;     // world meters the samples reach
    f32 intensity = 1.0f;  // scales the occlusion before the power
    f32 power = 1.5f;      // contrast of the result
    f32 bias = 0.02f;      // ignores occluders this close to the tangent plane (cosine threshold)
};

// Cascaded shadow maps for the sun (FR-R3D-040).
struct ShadowSettings {
    u32 cascades = 3;                          // 1..kMaxCascades
    f32 distance = 60.0f;                      // shadowed view depth (meters), clamped to the far plane
    f32 splits[4] = {0.05f, 0.25f, 1.0f, 1.0f};  // cascade far ends as fractions of `distance`
    f32 blend = 0.1f;                          // fraction of each cascade cross-faded into the next
    u32 resolution = 2048;                     // texels per cascade side: must match Forward3DDesc::shadow_size
};

inline constexpr u32 kMaxCascades = 4;

// Local-light shadows (M-VISUAL 4): spot lights get one atlas tile, point lights six (the faces of a
// cube, each a 90°+ perspective view). The atlas is `layers` extra layers of the cascade array (same
// size, same PCF), cut into `tile`-sized squares. Lights asking for shadows (PointLight::cast_shadows)
// are ranked by screen importance (brightness × projected size) and granted tiles in that order up
// to `max_lights`; the rest stay unshadowed that frame (Scene3D::Stats::local_shadows_dropped).
struct LocalShadowSettings {
    bool enabled = true;
    u32 tile = 512;       // texels per tile side: divides ShadowSettings::resolution
    u32 layers = 1;       // atlas layers: must match Forward3DDesc::local_shadow_layers
    u32 max_lights = 16;
};

// One rendered view of the atlas: a spot light, or one cube face of a point light (+X -X +Y -Y +Z -Z).
struct LocalShadowTile {
    math::mat4 view_proj;  // world -> light clip space (GL convention, perspective)
    rhi::Rect2D rect;      // texels in the layer, top-left origin (RenderPassDesc::viewport)
    u32 layer = 0;         // atlas layer (0 = the first one after the cascades)
    u32 light = 0;         // into Scene3D::point_lights()
    f32 texel_scale = 0.0f;  // world size of one texel per meter of distance from the light
    u32 first_caster = 0;  // into Scene3D::local_shadow_casters()
    u32 caster_count = 0;
};

// A placed reflection probe (M-VISUAL 2): the scene seen from `position`, captured into a GGX
// pre-filtered environment and box-projected onto the surfaces inside `bounds` (parallax-correct
// reflections in rooms and corridors). Specular only; needs Sky::ibl. Captured when it first
// appears and again when its position, bounds or version (or the sky) change: bump `version`
// after the scene around it changed.
struct ReflectionProbe {
    math::vec3 position{0.0f, 0.0f, 0.0f};
    math::aabb3 bounds{{-5.0f, -5.0f, -5.0f}, {5.0f, 5.0f, 5.0f}};  // world space
    f32 blend_distance = 1.0f;  // fade in from the box faces (meters)
    f32 intensity = 1.0f;
    u32 version = 0;
    // TextureStore key of the probe's editor bake (an RGBM RGBA8 array of Forward3D::kProbeLayers
    // layers, kIblEnvWidth x kIblEnvWidth/2, cooked from .mcenv): loaded in place of a runtime
    // capture. ~0u (or not resident) = captured at runtime.
    u32 baked = ~0u;
};

inline constexpr u32 kMaxReflectionProbes = 4;

// A global-illumination irradiance-probe volume (M-VISUAL 6, DDGI-style without ray tracing): a grid
// of counts[0] × counts[1] × counts[2] probes spanning `bounds` (corners included), each holding the
// diffuse light arriving from every direction (an L1 lobe, stored as four tetrahedral samples). A
// probe sees the scene through six small cube faces lit like the main pass: the sun (shadowed by its
// own fit over the volume), the point and spot lights, emissive surfaces, the sky behind them and the
// volume itself, so each update adds a bounce. `probes_per_frame` probes (x-major, wrapping) are
// re-captured every frame and blended in by `hysteresis`, so moving lights and a turning sun carry
// over; the first frame (or a new `version`, bounds or counts) starts a fill of every probe, spread
// over frames by Forward3D's per-frame capture budget, and the volume lights the frame once full.
// Inside the box the probes replace the sky's diffuse ambient and dim its reflections indoors; over
// `fade_distance` outside it the sky's ambient returns. Diffuse only: reflections stay with the sky
// and the reflection probes. Probes buried in walls see their backfaces and darken what is near them,
// so keep the grid's planes off the walls.
struct GiVolume {
    bool enabled = false;
    math::aabb3 bounds{{-8.0f, 0.0f, -8.0f}, {8.0f, 4.0f, 8.0f}};  // world space
    u32 counts[3] = {8, 4, 8};    // probes per axis, 2..kMaxGiProbesPerAxis
    f32 intensity = 1.0f;
    f32 normal_bias = 0.25f;      // meters the lookup moves off the surface along its normal
    f32 fade_distance = 1.0f;     // meters outside the box over which the volume fades to the sky
    f32 hysteresis = 0.85f;       // weight of a probe's old value when it is re-captured
    u32 probes_per_frame = 16;    // rolling updates (0 = frozen after the fill)
    u32 version = 0;              // bump to refill every probe at once
    // TextureStore key of the editor's bake (RGBM RGBA8, gi_volume_size() × 1 layer, cooked from
    // .mcenv): loaded in place of the fill. ~0u (or not resident) = filled at runtime.
    u32 baked = ~0u;
    // False (the Low tier): never captured at runtime. With a bake the volume holds the bake; without
    // one the scene draws without global illumination.
    bool updates = true;
};

inline constexpr u32 kMaxGiProbesPerAxis = 32;
// The volume texture's size: 4 texels per probe along x (probe x-major), one row per (y, z) pair.
inline rhi::Extent2D gi_volume_size(const GiVolume& gi) { return {gi.counts[0] * 4, gi.counts[1] * gi.counts[2]}; }

struct Cascade {
    math::mat4 view_proj;   // world -> light clip space
    f32 near_depth = 0.0f;  // view-depth range this cascade covers
    f32 far_depth = 0.0f;
    f32 texel_world = 0.0f; // world size of one shadow texel (normal-offset bias scale)
    f32 radius = 0.0f;      // bounding-sphere radius of the frustum slice
    f32 depth_range = 0.0f; // light-space depth extent of the ortho box (meters)
    u32 first_caster = 0;   // into Scene3D::cascade_casters()
    u32 caster_count = 0;
};

// A material that replaces every submesh's own (a .mcmat on a MeshRef): its parameters and the
// texture-store keys of its maps (~0u = none).
struct MaterialOverride {
    asset::MaterialChunk params{};
    u32 albedo_key = ~0u;
    u32 normal_key = ~0u;
    u32 orm_key = ~0u;
    u32 emissive_key = ~0u;
};

// A splat-layered terrain's look (M-TERRAIN 2): up to kMaxLayers materials blended by a weight map.
// Forward3D draws an instance carrying one with the terrain shader: each layer's maps come from one
// layer of the texture arrays below (TextureStore keys; ~0u = no layer has that map), tiled in the
// terrain's own xz metres, and the weights from the 2-layer RGBA8 `splat_key` array spanning the
// terrain's extent. Kit3DExtract builds it from a .mcterrain's layers (render::compose_layer_array).
struct TerrainShading {
    static constexpr u32 kMaxLayers = 8;
    struct Layer {
        math::vec4 base_color{1.0f, 1.0f, 1.0f, 1.0f};  // the material's (multiplies its albedo map)
        f32 roughness = 1.0f;
        f32 metallic = 0.0f;
        f32 tile = 4.0f;             // metres per repeat
        f32 height_strength = 1.0f;  // height blend: how far its albedo brightness lifts it
        bool triplanar = false;
        bool albedo_map = false;     // the arrays below hold this layer's map
        bool normal_map = false;
        bool orm_map = false;
        bool occlusion_in_orm = false;
    };
    Layer layers[kMaxLayers];
    u32 layer_count = 0;
    math::vec2 size{1.0f, 1.0f};  // the terrain's extent in metres along x and z
    u32 splat_resolution = 0;     // weight samples per side
    u32 albedo_key = ~0u, normal_key = ~0u, orm_key = ~0u, splat_key = ~0u;
    bool triplanar = true;     // the quality tier lets triplanar layers project along x and z
    bool height_blend = true;  // the quality tier blends by height (else by weight alone)
};

struct MeshInstance {
    u32 mesh = ~0u;                          // MeshStore key
    math::mat4 world = math::mat4::identity();
    Span<const math::mat4> pose;             // model-space node matrices (pose_nodes); empty = rest pose
    math::vec4 tint{1.0f, 1.0f, 1.0f, 1.0f}; // multiplies the material's base color (not its emissive)
    bool cast_shadows = true;
    // Skinned meshes only (FR-R3D-060): true deforms the skinned submeshes by `pose` (their
    // joints' node matrices); false draws them rigidly in their bind pose under `world` — the
    // game picks per instance whether a character uses its skeleton. Ignored for meshes without
    // skins.
    bool skinning = true;
    // Multiplies the material's emissive color: recolors glowing FX (tracers, flashes, team lights)
    // on one shared mesh; values above 1 push it into bloom.
    math::vec3 emissive_tint{1.0f, 1.0f, 1.0f};
    // Meshes with LOD levels (#158): the level drawn last frame, held by the caller per instance so
    // the choice has hysteresis (null: none, the level follows the screen size exactly). build()
    // writes the new level back.
    u8* lod_state = nullptr;
    // Replaces the material of every submesh (null: each submesh draws its own). The caller keeps
    // it alive until the scene is drawn.
    const MaterialOverride* material = nullptr;
    // TAA velocity (M-VISUAL 5): last frame's world matrix and node pose of a moving or animated
    // instance (null / empty = the same as this frame's: only the camera moved it). Read only when
    // PostFx::taa or PostFx::motion_blur is on.
    const math::mat4* prev_world = nullptr;
    Span<const math::mat4> prev_pose;
    // A terrain node (M-TERRAIN 2): drawn with the splat-layer shader instead of its material; `world`
    // must be the terrain's own transform (its space is what the layers tile in). Kept alive by the
    // caller until the scene is drawn.
    const TerrainShading* terrain = nullptr;
};

// The material a submesh of `inst` draws with, and its map keys (the override's when it has one).
inline const asset::MaterialChunk& material_of(const MeshInstance& inst, const GpuSubmesh& sm) {
    return inst.material ? inst.material->params : sm.material;
}
// A submesh's UV rectangle with the material's repeat (.mcmat uv_scale) folded in.
inline math::vec4 tiled_uv_rect(const math::vec4& uv_rect, const asset::MaterialChunk& mat) {
    f32 s[2];
    mat.uv_scale(s);
    return {uv_rect.x * s[0], uv_rect.y * s[1], uv_rect.z * s[0], uv_rect.w * s[1]};
}
inline u32 albedo_key_of(const MeshInstance& inst, const GpuSubmesh& sm) { return inst.material ? inst.material->albedo_key : sm.albedo_key; }
inline u32 normal_key_of(const MeshInstance& inst, const GpuSubmesh& sm) { return inst.material ? inst.material->normal_key : sm.normal_key; }
inline u32 orm_key_of(const MeshInstance& inst, const GpuSubmesh& sm) { return inst.material ? inst.material->orm_key : sm.orm_key; }
inline u32 emissive_key_of(const MeshInstance& inst, const GpuSubmesh& sm) {
    return inst.material ? inst.material->emissive_key : sm.emissive_key;
}

// ---- scale (M-VISUAL 9) -------------------------------------------------------------------------

// Many copies of one rigid mesh (foliage, rocks, debris): world matrices the GPU culls and draws by
// itself. Forward3D keeps the matrices in a GPU buffer under `id` (uploaded again when `version`, the
// count or the mesh changes) and every frame a compute pass culls each instance against the view and
// each sun cascade (and, for the view, against a depth pyramid of the frame's other opaques), picks
// its LOD level and writes the visible lists and indirect draw commands the passes then draw. Opaque
// and alpha-masked materials only (a blended one draws as opaque); skinned meshes are skipped. Batches
// take part in the sun's cascades, the SSAO and velocity prepasses and the
// main pass, not in local-light shadows, reflection probe or GI captures.
struct InstanceBatch {
    u32 id = 0;            // stable per batch (unique within the scene)
    u32 version = 0;       // bump when `transforms` changed
    u32 mesh = ~0u;        // MeshStore key
    // World matrices, alive until the scene is drawn. Read every frame only on the CPU path
    // (InstanceCulling::gpu off or no compute); the GPU path reads them when (re)uploading.
    Span<const math::mat4> transforms;
    // World bounds of every instance; an empty box (min > max, the default) is computed by add().
    math::aabb3 bounds{{1.0f, 1.0f, 1.0f}, {-1.0f, -1.0f, -1.0f}};
    bool cast_shadows = true;
    f32 cull_distance = 0.0f;  // meters from the camera beyond which an instance is culled; 0 = the far plane
    math::vec4 tint{1.0f, 1.0f, 1.0f, 1.0f};
    const MaterialOverride* material = nullptr;  // as MeshInstance::material
    // Foliage wind: a vertex sways sideways by amplitude × h² meters, h its height in meters above
    // the instance's origin, at `frequency` Hz with a phase that varies over the ground (Scene3D's clock).
    f32 wind_amplitude = 0.0f;
    f32 wind_frequency = 0.5f;
    math::vec2 wind_direction{1.0f, 0.0f};  // xz, normalized on use
};

struct InstanceCulling {
    // Compute culling and indirect draws (needs compute); false culls the batches on the CPU against
    // the frustum only (one instanced draw per visible list), which draws the same frame.
    bool gpu = true;
    // GPU only: the view's instances are also tested against a depth pyramid of the frame's other
    // opaque draws (rigid, not alpha-masked), rendered at 1 / `occluder_divisor` of the frame size.
    bool occlusion = true;
    u32 occluder_divisor = 2;
};

// A projected decal (M-VISUAL 9): the unit box [-0.5, 0.5]³ under `world`, projecting its texture
// along the box's local -Y onto every opaque surface inside it (the texture's u along local +X, v
// along +Z). It changes the surface's base color, roughness and metallic before lighting (so decals
// are lit, shadowed and fogged with the surface) and can glow. The main pass applies up to kMaxDecals
// visible decals in submission order (later ones over earlier ones), from up to kMaxDecalTextures
// distinct textures per frame; decals past either limit are dropped (Scene3D::Stats).
struct Decal {
    math::mat4 world = math::mat4::identity();
    u32 texture = ~0u;                      // TextureStore key: rgb color, a coverage; ~0u = plain `color`
    math::vec4 uv_rect{0.0f, 0.0f, 1.0f, 1.0f};  // offset.xy, scale.xy of the texture's region
    math::vec4 color{1.0f, 1.0f, 1.0f, 1.0f};    // linear rgb multiplies the texture; a: opacity
    f32 roughness = 0.7f;
    f32 metallic = 0.0f;
    f32 material_weight = 1.0f;  // how far roughness and metallic follow the decal (0 = keep the surface's)
    f32 emissive = 0.0f;         // emitted = texture × color × this (glowing decals)
    // Fades: where the surface faces away from the projection (its normal against local +Y) between
    // cos = `angle_fade` + 0.1 and `angle_fade`, and toward the box's top and bottom over `edge_fade` of
    // its half height.
    f32 angle_fade = 0.2f;
    f32 edge_fade = 0.2f;
};

inline constexpr u32 kMaxDecals = 64;
inline constexpr u32 kMaxDecalTextures = 3;

// GPU particles (M-VISUAL 9): a ring of `max_particles` simulated in compute, drawn as camera-facing
// sprites after the blended draws (additive or alpha blended, unsorted, unlit, depth tested). Forward3D
// keeps the particles under `id` across frames; an emitter not added for a frame (or a new
// `max_particles` or `seed`) starts over. New particles spawn at `rate` per second of Scene3D's clock
// plus `burst` this frame, inside the box around `position`, moving at a random speed in the cone of
// half-angle `spread` around `direction`; they fall by `gravity`, slow by `drag`, can bounce off the
// plane y = `ground_height`, and grow and fade from the start to the end values over their life.
struct ParticleEmitter {
    u32 id = 0;
    u32 max_particles = 1024;  // the oldest particle is reused when the ring is full
    u32 seed = 1;
    math::vec3 position{0.0f, 0.0f, 0.0f};
    math::vec3 box{0.0f, 0.0f, 0.0f};  // spawn volume half extents
    math::vec3 direction{0.0f, 1.0f, 0.0f};
    f32 spread = 0.4f;                 // radians
    f32 rate = 64.0f;                  // particles per second (0 = bursts only)
    u32 burst = 0;
    bool emitting = true;              // false: no new particles, the live ones play out
    f32 speed_min = 1.0f, speed_max = 3.0f;
    f32 lifetime_min = 1.0f, lifetime_max = 2.0f;
    math::vec3 gravity{0.0f, -9.81f, 0.0f};
    f32 drag = 0.0f;                   // per second: velocity × e^(-drag · dt)
    bool collide = false;              // bounce off y = ground_height
    f32 ground_height = 0.0f;
    f32 bounce = 0.3f;                 // the vertical speed kept by a bounce
    f32 size_start = 0.2f, size_end = 0.05f;          // sprite width in meters
    math::vec4 color_start{4.0f, 2.0f, 0.6f, 1.0f};   // linear (HDR allowed), a: opacity
    math::vec4 color_end{1.0f, 0.2f, 0.05f, 0.0f};
    bool additive = true;              // false: alpha blended
    u32 texture = ~0u;                 // TextureStore key of the sprite (a: coverage); ~0u = a soft round dot
};

inline constexpr u32 kMaxParticlesPerEmitter = 1u << 18;

// Screen-size LOD selection (#158): an instance draws the coarsest level whose geometric error,
// projected at its distance, stays under `pixel_error` pixels. Going coarser needs the error under
// pixel_error * (1 - hysteresis), so an instance at the boundary does not flicker. Shadow passes
// draw `shadow_bias` levels coarser than the main view.
struct LodSettings {
    bool enabled = true;
    f32 pixel_error = 1.0f;
    f32 hysteresis = 0.25f;
    u32 shadow_bias = 1;
    u32 force_level = ~0u;  // debugging (r.lod_force): every instance at this level, clamped
};

struct View3D {
    math::mat4 view;
    math::mat4 proj;
    math::mat4 view_proj;
    math::vec4 planes[6];  // Gribb-Hartmann, normalized, inside = dot(n, p) + d >= 0
    math::vec3 position;
    math::vec3 forward;
    f32 near_z = 0.1f;
    f32 far_z = 1000.0f;
    rhi::Extent2D viewport;
};

View3D compute_view_3d(const Camera3D& cam, rhi::Extent2D viewport);
// Sphere vs the view's frustum planes (conservative).
bool sphere_visible(const View3D& v, math::vec3 center, f32 radius);

class Scene3D {
public:
    struct Draw {
        u32 instance;
        u32 submesh;
        const GpuMesh* mesh;
        math::mat4 model;   // world * node * dequant: quantized vertex -> world (skinned: world)
        math::mat4 normal;  // inverse-transpose of world * node (upper 3x3 used; skinned: of world)
        math::mat4 dequant; // skinned draws: quantized vertex -> bind space (identity otherwise)
        u32 palette;        // skinned draws: first joint matrix in joint_palette(); ~0u = rigid
        u32 lod;            // the level drawn, and its index range (GpuMesh::lod)
        u32 first_index;
        u32 index_count;
        u32 base_vertex;
        math::mat4 prev_model;  // last frame's `model` (TAA velocity; = model when nothing moved)
        u32 prev_palette;       // skinned draws: last frame's joints in joint_palette() (= palette when unposed)
        f32 view_depth;     // positive distance along the view axis
        math::vec3 center;  // world bounding sphere
        f32 radius;
    };
    struct Stats {
        u32 instances = 0;
        u32 submeshes = 0;       // considered for the main view
        u32 visible = 0;         // survived frustum culling
        u32 shadow_casters = 0;
        u32 triangles = 0;       // visible
        u32 skinned_instances = 0;
        u32 joints = 0;          // joint matrices in the palette
        u32 point_lights = 0;
        u32 probes_dropped = 0;  // add_reflection_probe beyond kMaxReflectionProbes
        u32 cascade_casters[kMaxCascades] = {};
        u32 local_shadow_lights = 0;    // lights granted shadows this frame
        u32 local_shadow_tiles = 0;
        u32 local_shadow_casters = 0;   // caster draws over all tiles
        u32 local_shadows_dropped = 0;  // lights that asked but did not fit the budget
        u32 lod_instances[asset::kMaxMeshLods] = {};  // instances at each level (meshes with levels)
        u32 lod_triangles[asset::kMaxMeshLods] = {};  // visible main-pass triangles at each level
        u32 batches = 0;                // M-VISUAL 9: instance batches added
        u32 batch_instances = 0;        // ... their instances, before culling
        u32 decals = 0;                 // visible decals the main pass applies
        u32 decals_culled = 0;          // outside the view
        u32 decals_dropped = 0;         // visible, past kMaxDecals or kMaxDecalTextures
        u32 particle_emitters = 0;
        f32 cluster_ms = 0.0f;   // CPU time of the Forward+ light setup + CPU assignment, if it ran (not deterministic)
        f32 build_ms = 0.0f;     // CPU time of build() in total (not deterministic)
    };

    Scene3D(const MeshStore& meshes, mem::IAllocator& alloc);

    void begin(const Camera3D& camera, rhi::Extent2D viewport);
    void set_light(const DirectionalLight& light) { light_ = light; }
    void set_fog(const Fog& fog) { fog_ = fog; }
    void set_exposure(f32 exposure) { exposure_ = exposure; }
    void set_sky(const Sky& sky) { sky_ = sky; }
    void set_post(const PostFx& post) { post_ = post; }
    void set_ambient_occlusion(const AmbientOcclusion& ao) { ao_ = ao; }
    void set_shadows(const ShadowSettings& shadows) { shadow_settings_ = shadows; }
    void set_local_shadows(const LocalShadowSettings& local) { local_shadows_ = local; }
    void set_cluster_grid(const ClusterGridDesc& grid) { cluster_grid_ = grid; }
    void set_lod(const LodSettings& lod) { lod_ = lod; }
    // The physically based sky (M-VISUAL 7): with Atmosphere::enabled, build() tints the light and
    // sets its ambient from it (see Atmosphere) and Forward3D draws its sky in place of Sky's.
    void set_atmosphere(const Atmosphere& atmosphere) { atmosphere_ = atmosphere; }
    const Atmosphere& atmosphere() const { return atmosphere_; }
    // The unit vector toward the sun the atmosphere is computed for (quantized by
    // Atmosphere::rebake_degrees) and the sun's irradiance above the air (π · color · intensity as
    // set, before the tint). Valid after build() with the atmosphere enabled.
    math::vec3 atmosphere_sun() const { return atmosphere_sun_; }
    math::vec3 sun_irradiance() const { return sun_irradiance_; }
    void set_volumetrics(const Volumetrics& volumetrics) { volumetrics_ = volumetrics; }
    const Volumetrics& volumetrics() const { return volumetrics_; }
    const LodSettings& lod() const { return lod_; }
    void add_point_light(const PointLight& light);
    // Up to kMaxReflectionProbes; later ones are dropped (counted in stats().probes_dropped).
    void add_reflection_probe(const ReflectionProbe& probe);
    // The scene's irradiance volume (one at most); counts are clamped to 2..kMaxGiProbesPerAxis.
    void set_gi_volume(const GiVolume& gi);
    const GiVolume& gi_volume() const { return gi_; }
    // Instances whose mesh key is not resident are skipped (counted nowhere).
    void add(const MeshInstance& instance);
    // M-VISUAL 9. Batches whose mesh is not resident or skinned, or that have no transforms, are skipped.
    void add_batch(const InstanceBatch& batch);
    void set_instance_culling(const InstanceCulling& culling) { culling_ = culling; }
    const InstanceCulling& instance_culling() const { return culling_; }
    void add_decal(const Decal& decal);
    void add_particle_emitter(const ParticleEmitter& emitter);
    // The scene's clock: seconds (the wind's phase) and the step since the last frame (particles).
    void set_time(f32 seconds, f32 dt) {
        time_ = seconds;
        dt_ = dt;
    }
    f32 time() const { return time_; }
    f32 dt() const { return dt_; }
    // Culls, builds the shadow fit and sorts. After build() the accessors below are valid.
    void build();

    bool built() const { return built_; }
    const MeshStore& meshes() const { return *meshes_; }
    const View3D& view() const { return view_; }
    const DirectionalLight& light() const { return light_; }
    const Fog& fog() const { return fog_; }
    f32 exposure() const { return exposure_; }
    const Sky& sky() const { return sky_; }
    const PostFx& post() const { return post_; }
    const AmbientOcclusion& ambient_occlusion() const { return ao_; }
    const ShadowSettings& shadow_settings() const { return shadow_settings_; }
    const LocalShadowSettings& local_shadow_settings() const { return local_shadows_; }
    Span<const PointLight> point_lights() const { return point_lights_.span(); }
    Span<const ReflectionProbe> reflection_probes() const { return probes_.span(); }
    // Every opaque submesh of every instance at its base level, unculled: what probe captures draw.
    // Empty when the scene has neither reflection probes nor an enabled GI volume.
    Span<const Draw> probe_draws() const { return probe_draws_.span(); }
    // The Forward+ lists, assigned on the CPU on first use (Forward3D's GPU path never asks).
    const ClusterLights& clusters() const;
    // The light pool, grid constants and culled light list only: what the GPU assignment reads.
    const ClusterLights& cluster_setup() const { return clusters_; }
    // Sun cascades (M5a): empty when the light casts no shadows or the scene is empty.
    Span<const Cascade> cascades() const { return Span<const Cascade>(cascades_, cascade_count_); }
    // Per-cascade caster lists: indices into shadow_draws(), cascade by cascade.
    Span<const u32> cascade_casters() const { return cascade_casters_.span(); }
    // Local-light shadow tiles granted this frame, light by light (a point light's six faces in a
    // row), and their caster lists (indices into shadow_draws()).
    Span<const LocalShadowTile> local_shadow_tiles() const { return local_tiles_.span(); }
    Span<const u32> local_shadow_casters() const { return local_casters_.span(); }
    Span<const MeshInstance> instances() const { return instances_.span(); }
    // Main-pass draws: opaque front-to-back (by material within equal depth bands is not
    // attempted: the key's material field groups texture binds), then blended back-to-front.
    Span<const Draw> draws() const { return draws_.span(); }
    u32 opaque_count() const { return opaque_count_; }
    // Every submesh of every instance (shadows come from off-screen casters too).
    Span<const Draw> shadow_draws() const { return shadow_draws_.span(); }
    Span<const InstanceBatch> batches() const { return batches_.span(); }
    // The decals the main pass applies (visible, within the limits), in submission order, and the
    // texture keys they sample: Decal::texture of decals()[i] is decal_textures()[decal_slots()[i]]
    // (slot ~0u = untextured).
    Span<const Decal> decals() const { return decals_.span(); }
    Span<const u32> decal_slots() const { return decal_slots_.span(); }
    Span<const u32> decal_textures() const { return Span<const u32>(decal_textures_, decal_texture_count_); }
    Span<const ParticleEmitter> particle_emitters() const { return emitters_.span(); }
    // Joint matrices (model space) of every skinned instance, skin by skin: Draw::palette indexes
    // it. Uploaded once per frame as the skinned shaders' storage buffer.
    Span<const math::mat4> joint_palette() const { return joint_palette_.span(); }
    // World-space bounds of all instances and the light's view-projection fitted to them (the M4
    // single-map fit; kept as the cascade fallback and in the diorama's determinism hash).
    const math::aabb3& scene_bounds() const { return scene_bounds_; }
    const math::mat4& light_view_proj() const { return light_view_proj_; }
    const Stats& stats() const { return stats_; }

private:
    mutable Stats stats_;
    void build_cascades();
    void build_local_shadows();
    u32 select_lod(const MeshInstance& inst, const GpuMesh& mesh) const;

    const MeshStore* meshes_;
    mem::IAllocator* alloc_;
    View3D view_{};
    DirectionalLight light_;
    Fog fog_;
    f32 exposure_ = 1.0f;
    Sky sky_;
    PostFx post_;
    AmbientOcclusion ao_;
    ShadowSettings shadow_settings_;
    LocalShadowSettings local_shadows_;
    ClusterGridDesc cluster_grid_;
    LodSettings lod_;
    GiVolume gi_;
    Atmosphere atmosphere_;
    math::vec3 atmosphere_sun_{0.0f, 1.0f, 0.0f};
    math::vec3 sun_irradiance_;
    Volumetrics volumetrics_;
    Array<PointLight> point_lights_;
    mutable ClusterLights clusters_;  // assigned lazily by clusters()
    Cascade cascades_[kMaxCascades];
    u32 cascade_count_ = 0;
    Array<u32> cascade_casters_;
    Array<LocalShadowTile> local_tiles_;
    Array<u32> local_casters_;
    Array<MeshInstance> instances_;
    Array<Draw> draws_;
    Array<Draw> shadow_draws_;
    Array<Draw> probe_draws_;
    Array<ReflectionProbe> probes_;
    Array<math::mat4> joint_palette_;
    Array<InstanceBatch> batches_;
    InstanceCulling culling_;
    Array<Decal> decals_;
    Array<u32> decal_slots_;
    u32 decal_textures_[kMaxDecalTextures] = {};
    u32 decal_texture_count_ = 0;
    Array<ParticleEmitter> emitters_;
    f32 time_ = 0.0f;
    f32 dt_ = 1.0f / 60.0f;
    u32 opaque_count_ = 0;
    math::aabb3 scene_bounds_{};
    math::mat4 light_view_proj_ = math::mat4::identity();
    bool built_ = false;
};

// World-space AABB of a local AABB under `m` (Arvo).
math::aabb3 transform_aabb(const math::mat4& m, const math::aabb3& b);

// The six normalized frustum planes of a view-projection (Gribb-Hartmann, inside = dot(n, p) + d >= 0).
void frustum_planes(const math::mat4& view_proj, math::vec4 out[6]);

// The batch's culling of one view, as the GPU does it (M-VISUAL 9): instance i is visible when its
// bounding sphere (the mesh's bounds' sphere under its matrix, radius × the largest axis scale) is
// inside `planes` and within the cull distance of `eye`; its LOD level follows Scene3D's LodSettings
// from `lod_view` (the main view, also for the cascades) plus `level_bias`, without hysteresis.
// Appends (level, instance) pairs, in instance order, to `out`.
struct BatchVisible {
    u32 level;
    u32 instance;
};
void cull_batch(const InstanceBatch& batch, const GpuMesh& mesh, const math::vec4 planes[6], const View3D& lod_view,
                const LodSettings& lod, u32 level_bias, Array<BatchVisible>& out);
// The LOD level of a bounding sphere for `view` (the batch path's select_lod: no hysteresis).
u32 batch_lod(const GpuMesh& mesh, math::vec3 center, f32 radius, f32 scale, const View3D& view, const LodSettings& lod);
// A batch's wind for the instanced shaders: amplitude, frequency, normalized direction xz ((1, 0) when zero).
math::vec4 batch_wind(const InstanceBatch& b);

// Instanced foliage (M-VISUAL 9): `count` world matrices scattered over the rectangle of half extents
// `half_extents` (x, z) around `origin`, each turned by a random yaw and scaled uniformly between
// `scale_min` and `scale_max`, on a jittered grid so no two land on top of each other. Deterministic in
// `seed`. Writes into `out` (resized to count).
struct FoliageScatter {
    math::vec3 origin{0.0f, 0.0f, 0.0f};
    math::vec2 half_extents{5.0f, 5.0f};
    u32 count = 100;
    u32 seed = 1;
    f32 scale_min = 0.8f, scale_max = 1.2f;
    bool random_yaw = true;
};
void scatter_foliage(const FoliageScatter& scatter, Array<math::mat4>& out);

// The minimal bounding sphere of the view-frustum slice [near_depth, far_depth] (centre on the
// view axis, `center_depth` in front of the eye). Depends only on the slice and the projection,
// so it does not change as the camera turns: the cascade fit's stability (no shimmer).
void frustum_slice_sphere(const View3D& view, f32 near_depth, f32 far_depth, f32& center_depth, f32& radius);

// Fits cascade `out` for view-depth range [near_depth, far_depth]: a light-space orthographic box
// around the slice's bounding sphere, its origin snapped to whole shadow texels, its depth range
// stretched back towards the light to include every caster in `scene_bounds`.
void fit_cascade(const View3D& view, math::vec3 light_dir, f32 near_depth, f32 far_depth, u32 resolution,
                 const math::aabb3& scene_bounds, Cascade& out);

}  // namespace mce::render
