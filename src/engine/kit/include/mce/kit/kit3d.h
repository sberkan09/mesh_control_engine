#pragma once
// Starter-kit 3D components (M-KIT3D, issue #129): what a 3D scene authored in the editor or over
// MCP is made of, alongside LocalTransform. Plain reflected structs with the field layouts
// SolsticeReach validated; the stock player draws them (kit::Kit3DExtract, kit3d_extract.h) and
// kit::install_kit3d runs their systems. Data-only header (core + refl + math + asset refs), so
// game modules and the cook include it without the renderer.
//
// Conventions: right-handed, Y up, metres. An entity's LocalTransform (WorldTransform when the
// hierarchy wrote one) places it; cameras look down their local -Z.
//
// Layouts are padding-free (World::state_hash hashes component bytes): asset refs and 8-byte
// fields first, then 4-byte fields, with explicit pad fields to a multiple of 8.
//
//   kit::install_kit3d(world, &assets);   // components, Environment3D, Physics3D, the systems

#include "mce/asset/handle.h"
#include "mce/containers/array.h"
#include "mce/fixed_string.h"
#include "mce/math/math.h"
#include "mce/refl/macros.h"

namespace mce {
class World;
}

namespace mce::asset {
class AssetSystem;
}

namespace mce::kit {

// A placed model: `mesh` drawn at the entity's transform.
struct MeshRef {
    asset::AssetRef<asset::Mesh> mesh;
    math::vec4 tint{1.0f, 1.0f, 1.0f, 1.0f};       // multiplies the base color (alpha needs a blended material)
    math::vec3 emissive_tint{1.0f, 1.0f, 1.0f};    // multiplies the emissive color; > 1 feeds bloom
    f32 cull_radius = 0.0f;                        // 0: the mesh bounds; else a sphere culled first (scaled)
    u32 cast_shadows = 1;
    u32 interpolate = 0;  // 1: drawn between the last two ticks' transforms (things the game moves)
    u32 skinning = 1;     // skinned meshes: 0 draws the bind pose rigidly
    u32 visible = 1;
};
MCE_COMPONENT(MeshRef, mesh, tint, emissive_tint, (cull_radius, Range{0, 1000}, Help{"0 = mesh bounds"}), cast_shadows,
              (interpolate, Help{"1: smooth between ticks (moving objects)"}), skinning, visible)

// A point light at the entity's position plus `offset` (in the entity's space).
struct PointLight3D {
    math::Color color{1.0f, 0.7f, 0.4f, 1.0f};  // linear
    math::vec3 offset;
    f32 intensity = 3.0f;
    f32 radius = 7.0f;   // no light past this distance
    u32 enabled = 1;
};
MCE_COMPONENT(PointLight3D, color, offset, (intensity, Range{0, 100}), (radius, Range{0.1f, 200}), enabled)

// A spot light: a point light lit only inside a cone along `direction` (entity space). Full
// intensity inside `inner_deg`, none past `outer_deg` (half-angles).
struct SpotLight3D {
    math::Color color{1.0f, 0.95f, 0.85f, 1.0f};
    math::vec3 offset;
    f32 intensity = 6.0f;
    math::vec3 direction{0.0f, -1.0f, 0.0f};
    f32 radius = 12.0f;
    f32 outer_deg = 30.0f;
    f32 inner_deg = 20.0f;
    u32 enabled = 1;
};
MCE_COMPONENT(SpotLight3D, color, offset, (intensity, Range{0, 100}), direction, (radius, Range{0.1f, 200}),
              (outer_deg, Range{1, 89}), (inner_deg, Range{0, 89}), enabled)

// The 3D view a player renders from: the first `primary` camera in ECS order, at its entity's
// transform (looking down local -Z). Drawn between the last two ticks' transforms, so a camera the
// game moves every tick is smooth at any frame rate. A scene with one plays on the 3D path.
struct Camera3DComponent {
    f32 fov_deg = 60.0f;  // vertical
    f32 near_z = 0.1f;
    f32 far_z = 1000.0f;
    u32 primary = 1;
};
MCE_COMPONENT(Camera3DComponent, (fov_deg, Range{5, 170}), (near_z, Range{0.001f, 100}), (far_z, Range{1, 100000}), primary)

// Plays a node / skeleton clip of the entity's MeshRef mesh by name, cross-fading over `fade`
// seconds when `clip` changes. kit3d advances `time` every tick (deterministic); a game may also
// write `time` itself. An empty or unknown clip shows the rest pose.
struct Animator {
    FixedString<32> clip;
    u64 playing = 0;     // runtime: StringId of the clip `time` belongs to
    u64 previous = 0;    // runtime: the clip fading out
    f32 speed = 1.0f;
    f32 phase = 0.0f;    // seconds added when posing (desynchronises a row of banners)
    f32 fade = 0.2f;     // cross-fade seconds (0 = cut)
    f32 time = 0.0f;     // seconds into `clip` (loops)
    f32 previous_time = 0.0f;
    f32 blend = 1.0f;    // runtime: 0..1 weight of `clip` against `previous`
};
MCE_COMPONENT(Animator, clip, (playing, ReadOnly, Transient), (previous, ReadOnly, Transient), (speed, Range{-8, 8}),
              phase, (fade, Range{0, 5}), time, (previous_time, ReadOnly, Transient), (blend, ReadOnly, Transient))

// The world's light, sky, fog, post chain and shadows (the renderer's DirectionalLight, Sky, Fog,
// PostFx and ShadowSettings, flattened for reflection). It lives in two places:
//   - as a component a scene authors (the first one in ECS order): the editor viewport draws it,
//     and install_kit3d's Startup copies it into the resource;
//   - as the world resource of the same type the game drives (a time of day) and the player draws.
struct Environment3D {
    asset::AssetRef<asset::Texture> sky_texture;    // equirect map; null = the gradient alone
    math::vec3 sun_direction{-0.4f, -1.0f, -0.3f};  // the way the light travels
    f32 sun_intensity = 2.0f;
    math::vec3 sun_color{1.0f, 0.96f, 0.9f};        // linear
    u32 sun_shadows = 1;
    math::vec3 ambient_sky{0.35f, 0.42f, 0.55f};
    f32 exposure = 1.0f;
    math::vec3 ambient_ground{0.20f, 0.18f, 0.16f};
    f32 fog_density = 0.0f;  // exp² fog; 0 = off
    math::vec3 fog_color{0.62f, 0.72f, 0.85f};
    u32 sky_enabled = 1;     // 0: clear to the fog color
    math::vec3 sky_zenith{0.16f, 0.34f, 0.72f};
    f32 sky_sun_size = 0.9994f;  // cos of the sun disk's angular radius
    math::vec3 sky_horizon{0.62f, 0.74f, 0.88f};
    f32 sky_sun_intensity = 12.0f;
    math::vec3 sky_ground{0.22f, 0.21f, 0.20f};
    f32 sky_texture_intensity = 1.0f;
    math::vec4 shadow_splits{0.05f, 0.25f, 1.0f, 1.0f};  // cascade far ends as fractions of shadow_distance
    u32 sky_texture_additive = 0;  // 1: gradient + texture (a star field over a dusk sky)
    f32 sky_specular_ambient = 0.5f;
    u32 bloom = 1;
    f32 bloom_intensity = 0.05f;
    f32 bloom_threshold = 1.0f;
    u32 fxaa = 1;
    u32 shadow_cascades = 3;
    f32 shadow_distance = 60.0f;
    f32 shadow_blend = 0.1f;
    u32 pad_ = 0;
};
MCE_COMPONENT(Environment3D, (sky_texture, Help{"equirect sky map; empty = gradient"}), sun_direction,
              (sun_intensity, Range{0, 50}), sun_color, sun_shadows, ambient_sky, (exposure, Range{0.01f, 16}), ambient_ground,
              (fog_density, Range{0, 0.2f}), fog_color, sky_enabled, sky_zenith, (sky_sun_size, Range{0.99f, 1}), sky_horizon,
              (sky_sun_intensity, Range{0, 100}), sky_ground, (sky_texture_intensity, Range{0, 10}), shadow_splits,
              sky_texture_additive, (sky_specular_ambient, Range{0, 2}), bloom, (bloom_intensity, Range{0, 1}),
              (bloom_threshold, Range{0, 10}), fxaa, (shadow_cascades, Range{1, 4}), (shadow_distance, Range{1, 2000}),
              (shadow_blend, Range{0, 0.5f}))

// ---- many static copies of a few meshes (grass, rocks, trees) -----------------------------------

// One copy. `max_distance` > 0 hides it past that camera distance; past `thin_distance` (> 0) only
// every other copy of its cell draws.
struct StaticInstance3D {
    math::vec3 position;
    f32 scale = 1.0f;
    math::quat rotation;
    u32 mesh = 0;              // index into StaticInstances3D::meshes
    u32 cast_shadows = 1;
    f32 max_distance = 0.0f;
    f32 thin_distance = 0.0f;
};

// A run of instances culled as one sphere (build_cells makes them).
struct StaticCell3D {
    math::vec3 center;
    f32 radius = 0.0f;
    u32 first = 0, count = 0;
};

// World resource: copies a game places at Startup (a seeded scatter, a forest), drawn by the
// extract cell by cell. Not part of World::state_hash (resources are not), so place them
// deterministically if the sim reads them.
struct StaticInstances3D {
    Array<asset::Guid> meshes;
    Array<StaticInstance3D> instances;
    Array<StaticCell3D> cells;

    // Index of `mesh` in `meshes`, appended when new (~0u for a null GUID).
    u32 mesh_slot(asset::Guid mesh);
    // Sorts `instances` into square cells of `cell_size` metres (stable) and rebuilds `cells`;
    // `max_extent` is the largest radius one instance reaches around its position.
    void build_cells(f32 cell_size = 16.0f, f32 max_extent = 6.0f);
};

// Registers the kit3d components, sets the Environment3D and StaticInstances3D resources (kept if
// already set) and adds the systems:
//   Startup           "kit3d_environment"  the scene's Environment3D component, if any, into the resource
//   FixedUpdate_Post  "kit3d_animators"    Animator time and cross-fades
// Idempotent. `assets` may be null.
void install_kit3d(World& world, asset::AssetSystem* assets);

// True when the world has an entity with a Camera3DComponent (the player then draws in 3D).
bool has_camera_3d(World& world);

}  // namespace mce::kit
