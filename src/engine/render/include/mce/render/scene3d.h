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
    // PostFx::taa is on.
    const math::mat4* prev_world = nullptr;
    Span<const math::mat4> prev_pose;
};

// The material a submesh of `inst` draws with, and its map keys (the override's when it has one).
inline const asset::MaterialChunk& material_of(const MeshInstance& inst, const GpuSubmesh& sm) {
    return inst.material ? inst.material->params : sm.material;
}
inline u32 albedo_key_of(const MeshInstance& inst, const GpuSubmesh& sm) { return inst.material ? inst.material->albedo_key : sm.albedo_key; }
inline u32 normal_key_of(const MeshInstance& inst, const GpuSubmesh& sm) { return inst.material ? inst.material->normal_key : sm.normal_key; }
inline u32 orm_key_of(const MeshInstance& inst, const GpuSubmesh& sm) { return inst.material ? inst.material->orm_key : sm.orm_key; }
inline u32 emissive_key_of(const MeshInstance& inst, const GpuSubmesh& sm) {
    return inst.material ? inst.material->emissive_key : sm.emissive_key;
}

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
    // Culls, builds the shadow fit and sorts. After build() the accessors below are valid.
    void build();

    bool built() const { return built_; }
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
    u32 opaque_count_ = 0;
    math::aabb3 scene_bounds_{};
    math::mat4 light_view_proj_ = math::mat4::identity();
    bool built_ = false;
};

// World-space AABB of a local AABB under `m` (Arvo).
math::aabb3 transform_aabb(const math::mat4& m, const math::aabb3& b);

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
