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
//   for (...) scene.add({mesh_key, world});
//   scene.build();                      // frustum cull (sphere vs planes), 64-bit key radix sort
//   renderer.record_3d(cmd, scene);     // shadow pass, forward HDR pass, resolve, tonemap
//
// Conventions (binding): right-handed, Y up, column vectors, GL clip z in [-1, 1]. Draw order is a
// function of (instances in submission order, camera) only — ties keep submission order.

#include "mce/containers/array.h"
#include "mce/math/math.h"
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

struct MeshInstance {
    u32 mesh = ~0u;                          // MeshStore key
    math::mat4 world = math::mat4::identity();
    Span<const math::mat4> pose;             // model-space node matrices (pose_nodes); empty = rest pose
    math::vec4 tint{1.0f, 1.0f, 1.0f, 1.0f}; // multiplies the material's base color
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
        math::mat4 model;   // world * node * dequant: quantized vertex -> world
        math::mat4 normal;  // inverse-transpose of world * node (upper 3x3 used)
        f32 view_depth;     // positive distance along the view axis
    };
    struct Stats {
        u32 instances = 0;
        u32 submeshes = 0;       // considered for the main view
        u32 visible = 0;         // survived frustum culling
        u32 shadow_casters = 0;
        u32 triangles = 0;       // visible
    };

    Scene3D(const MeshStore& meshes, mem::IAllocator& alloc);

    void begin(const Camera3D& camera, rhi::Extent2D viewport);
    void set_light(const DirectionalLight& light) { light_ = light; }
    void set_fog(const Fog& fog) { fog_ = fog; }
    void set_exposure(f32 exposure) { exposure_ = exposure; }
    // Instances whose mesh key is not resident are skipped (counted nowhere).
    void add(const MeshInstance& instance);
    // Culls, builds the shadow fit and sorts. After build() the accessors below are valid.
    void build();

    bool built() const { return built_; }
    const View3D& view() const { return view_; }
    const DirectionalLight& light() const { return light_; }
    const Fog& fog() const { return fog_; }
    f32 exposure() const { return exposure_; }
    Span<const MeshInstance> instances() const { return instances_.span(); }
    // Main-pass draws: opaque front-to-back (by material within equal depth bands is not
    // attempted: the key's material field groups texture binds), then blended back-to-front.
    Span<const Draw> draws() const { return draws_.span(); }
    u32 opaque_count() const { return opaque_count_; }
    // Every submesh of every instance (shadows come from off-screen casters too).
    Span<const Draw> shadow_draws() const { return shadow_draws_.span(); }
    // World-space bounds of all instances and the light's view-projection fitted to them.
    const math::aabb3& scene_bounds() const { return scene_bounds_; }
    const math::mat4& light_view_proj() const { return light_view_proj_; }
    const Stats& stats() const { return stats_; }

private:
    const MeshStore* meshes_;
    mem::IAllocator* alloc_;
    View3D view_{};
    DirectionalLight light_;
    Fog fog_;
    f32 exposure_ = 1.0f;
    Array<MeshInstance> instances_;
    Array<Draw> draws_;
    Array<Draw> shadow_draws_;
    u32 opaque_count_ = 0;
    math::aabb3 scene_bounds_{};
    math::mat4 light_view_proj_ = math::mat4::identity();
    Stats stats_;
    bool built_ = false;
};

// World-space AABB of a local AABB under `m` (Arvo).
math::aabb3 transform_aabb(const math::mat4& m, const math::aabb3& b);

}  // namespace mce::render
