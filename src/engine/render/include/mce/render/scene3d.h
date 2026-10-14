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

// The post chain (FR-R3D-090): bloom -> ACES tonemap (exposure) -> FXAA. Toggleable per frame.
struct PostFx {
    bool bloom = true;
    f32 bloom_intensity = 0.05f;   // fraction of the blurred image added back
    f32 bloom_threshold = 1.0f;    // soft bright-pass knee start (exposed scene units); 0 = no threshold
    bool fxaa = true;
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
};

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
    void set_cluster_grid(const ClusterGridDesc& grid) { cluster_grid_ = grid; }
    void set_lod(const LodSettings& lod) { lod_ = lod; }
    const LodSettings& lod() const { return lod_; }
    void add_point_light(const PointLight& light);
    // Up to kMaxReflectionProbes; later ones are dropped (counted in stats().probes_dropped).
    void add_reflection_probe(const ReflectionProbe& probe);
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
    Span<const PointLight> point_lights() const { return point_lights_.span(); }
    Span<const ReflectionProbe> reflection_probes() const { return probes_.span(); }
    // Every opaque submesh of every instance at its base level, unculled: what probe captures draw.
    // Empty when the scene has no reflection probes.
    Span<const Draw> probe_draws() const { return probe_draws_.span(); }
    // The Forward+ lists, assigned on the CPU on first use (Forward3D's GPU path never asks).
    const ClusterLights& clusters() const;
    // The light pool, grid constants and culled light list only: what the GPU assignment reads.
    const ClusterLights& cluster_setup() const { return clusters_; }
    // Sun cascades (M5a): empty when the light casts no shadows or the scene is empty.
    Span<const Cascade> cascades() const { return Span<const Cascade>(cascades_, cascade_count_); }
    // Per-cascade caster lists: indices into shadow_draws(), cascade by cascade.
    Span<const u32> cascade_casters() const { return cascade_casters_.span(); }
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
    ClusterGridDesc cluster_grid_;
    LodSettings lod_;
    Array<PointLight> point_lights_;
    mutable ClusterLights clusters_;  // assigned lazily by clusters()
    Cascade cascades_[kMaxCascades];
    u32 cascade_count_ = 0;
    Array<u32> cascade_casters_;
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
