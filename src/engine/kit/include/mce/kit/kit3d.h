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
#include "mce/physics3d/world3d.h"
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
    asset::AssetRef<asset::Material> material;     // a .mcmat drawn on every submesh instead of the mesh's own
};
MCE_COMPONENT(MeshRef, mesh, tint, emissive_tint, (cull_radius, Range{0, 1000}, Help{"0 = mesh bounds"}), cast_shadows,
              (interpolate, Help{"1: smooth between ticks (moving objects)"}), skinning, visible,
              (material, Help{"a .mcmat replacing every submesh's material; null = the mesh's own"}))

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

// A reflection probe (M-VISUAL 2): the scene seen from the entity's position plus `offset`, used
// for the specular reflections of surfaces inside the world-axis box of `half_extents` around the
// entity (box-projected, so a room's walls line up in its reflections). Keep the room's floor and
// walls inside the box by more than `blend_distance`, over which the probe fades to the sky.
// Needs Environment3D::sky_ibl. Captured when placed or moved; bump `version` to re-capture after
// the room itself changed. With `baked` (the editor's bake, a cooked .mcenv) the bake is loaded
// instead and nothing is captured at runtime. At most four per scene.
struct ReflectionProbe3D {
    math::vec3 half_extents{5.0f, 3.0f, 5.0f};
    math::vec3 offset;
    f32 blend_distance = 1.0f;
    f32 intensity = 1.0f;
    u32 version = 0;
    u32 enabled = 1;
    asset::AssetRef<asset::Texture> baked;
};
MCE_COMPONENT(ReflectionProbe3D, half_extents, (offset, Help{"capture point, entity space"}),
              (blend_distance, Range{0, 20}), (intensity, Range{0, 4}),
              (version, Help{"bump to re-capture"}), enabled,
              (baked, Help{"the editor's bake (.mcenv); empty = captured at runtime"}))

// A global-illumination irradiance volume (M-VISUAL 6): a probes_x × probes_y × probes_z grid of
// probes over the world-axis box of `half_extents` around the entity (centered at `offset`), lighting
// the surfaces inside with the light the scene bounces: the sun, point and spot lights, emissive
// surfaces and the sky, as seen from each probe (render::GiVolume). The first enabled one in the scene
// is used. `probes_per_frame` probes are re-captured every frame and blended in by `hysteresis`, so
// moving lights carry over; bump `version` to refill them all at once. With `baked` (the editor's
// bake, a cooked .mcenv) the volume starts from the bake, and the Low quality tier uses the bake only
// (no bake there: no global illumination). Keep the grid's planes off the walls: a probe buried in a
// wall darkens what is near it.
struct GiVolume3D {
    math::vec3 half_extents{8.0f, 2.0f, 8.0f};
    math::vec3 offset;
    u32 probes_x = 8;
    u32 probes_y = 4;
    u32 probes_z = 8;
    f32 intensity = 1.0f;
    f32 normal_bias = 0.25f;
    f32 fade_distance = 1.0f;
    f32 hysteresis = 0.85f;
    u32 probes_per_frame = 16;
    u32 version = 0;
    u32 enabled = 1;
    asset::AssetRef<asset::Texture> baked;
};
MCE_COMPONENT(GiVolume3D, half_extents, (offset, Help{"box center, entity space"}), (probes_x, Range{2, 32}),
              (probes_y, Range{2, 32}), (probes_z, Range{2, 32}), (intensity, Range{0, 4}),
              (normal_bias, Range{0, 2}, Help{"meters a lookup moves off the surface"}),
              (fade_distance, Range{0, 20}, Help{"meters outside the box over which the sky's ambient returns"}),
              (hysteresis, Range{0, 0.99f}, Help{"weight of a probe's old light when it is re-captured"}),
              (probes_per_frame, Range{0, 1024}, Help{"rolling updates; 0 = filled once"}),
              (version, Help{"bump to refill every probe"}), enabled,
              (baked, Help{"the editor's bake (.mcenv); empty = filled at runtime"}))

// The 3D view a player renders from: the first `primary` camera in ECS order, at its entity's
// transform (looking down local -Z). Drawn between the last two ticks' transforms, so a camera the
// game moves every tick is smooth at any frame rate. A scene with one plays on the 3D path.
// `mouse_look` asks the player for relative mouse mode (InputState::look keeps turning past the
// window edge); the player releases it while paused, while the console is open and when unfocused.
struct Camera3DComponent {
    f32 fov_deg = 60.0f;  // vertical
    f32 near_z = 0.1f;
    f32 far_z = 1000.0f;
    u32 primary = 1;
    u32 mouse_look = 0;
    u32 pad_ = 0;
};
MCE_COMPONENT(Camera3DComponent, (fov_deg, Range{5, 170}), (near_z, Range{0.001f, 100}), (far_z, Range{1, 100000}), primary,
              (mouse_look, Help{"1: capture the mouse for look (released while paused or in the console)"}))

// Plays a node / skeleton clip of the entity's MeshRef mesh by name, cross-fading over `fade`
// seconds when `clip` changes. kit3d advances `time` every tick (deterministic); a game may also
// write `time` itself. An empty or unknown clip shows the rest pose. With `loop` off the clip plays
// once, holds its last pose and sets `finished`; `restart` plays it again from the start.
//
// With `graph` (an .mcanim AnimGraph, M-DATA-REACH 7) the graph drives the Animator: `state` is the
// state playing, `params` hold the graph's parameters in the order it lists them (the game, a
// sequence or MCP writes them), transitions fire on parameter conditions or when a one-shot state
// finishes and cross-fade over their own fade, a blend state mixes two neighbouring clips by a
// parameter (`clip` and `blend_clip`, sampled at the same phase, weight `blend_weight`), and the
// state's events raise AnimEvent3D (anim3d.h) as the playhead crosses them; `restart` returns to
// the start state. Clip lengths come from the mesh, read through install_kit3d's asset system;
// without it clips loop forever and one-shots never finish.
struct Animator {
    FixedString<32> clip;
    FixedString<32> state;                    // runtime: the graph state playing
    asset::AssetRef<asset::AnimGraph> graph;  // optional state graph (.mcanim)
    u64 playing = 0;                          // runtime: StringId of the clip `time` belongs to
    u64 previous = 0;                         // runtime: the clip fading out
    u64 blend_clip = 0;                       // runtime: a blend state's second clip
    math::vec4 params;                        // the graph's parameters, in its order
    FixedString<24> last_event;               // runtime: the last AnimEvent3D's name
    f32 speed = 1.0f;
    f32 phase = 0.0f;                         // seconds added when posing (desynchronises a row of banners)
    f32 fade = 0.2f;                          // cross-fade seconds (0 = cut); a graph transition sets its own
    f32 time = 0.0f;                          // seconds into `clip`
    f32 previous_time = 0.0f;
    f32 blend = 1.0f;                         // runtime: 0..1 weight of `clip` against `previous`
    f32 blend_weight = 0.0f;                  // runtime: 0..1 weight of `blend_clip` against `clip`
    u32 events_fired = 0;                     // runtime
    bool loop = true;                         // without a graph: false plays `clip` once
    bool finished = false;                    // runtime: a one-shot clip or state reached its end
    bool restart = false;                     // set to replay from the start (the engine clears it)
    u8 pad_ = 0;
    u32 pad2_ = 0;
};
MCE_COMPONENT(Animator, clip, (graph, Help{"an AnimGraph (.mcanim) that drives clip, fades and events"}),
              (params, Help{"the graph's parameters, in the order it lists them"}), (state, ReadOnly, Transient),
              (playing, ReadOnly, Transient), (previous, ReadOnly, Transient), (blend_clip, ReadOnly, Transient),
              (last_event, ReadOnly, Transient), (speed, Range{-8, 8}), phase, (fade, Range{0, 5}), time,
              (previous_time, ReadOnly, Transient), (blend, ReadOnly, Transient), (blend_weight, ReadOnly, Transient),
              (events_fired, ReadOnly, Transient), (loop, Help{"without a graph: off plays the clip once"}),
              (finished, ReadOnly, Transient), (restart, Help{"replay from the start (the engine clears it)"}))

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
    u32 sky_ibl = 0;  // 1: the sky lights the scene (image-based lighting, M-VISUAL 2)
    f32 ibl_texture_intensity = 1.0f;
    asset::AssetRef<asset::Texture> ibl_texture;  // equirect the IBL bakes from; null = the visible sky
    // M-VISUAL 7. The physically based sky: it replaces the gradient and the sky texture, tints the sun
    // (sun_color is then the light above the air) and sets the ambient from the sky and the lit ground.
    u32 atmosphere = 0;
    f32 atmosphere_intensity = 1.0f;
    f32 atmosphere_haze = 1.0f;     // aerosols: whiter skies, a wider glow around the sun
    f32 time_of_day = -1.0f;        // hours 0..24 placing the sun on a simple day's arc; -1 = sun_direction
    // Volumetric fog in place of the exp² fog: a height fog lit by the sun (shafts), the lights and the ambient.
    u32 volumetric_fog = 0;
    f32 volumetric_density = 0.03f;          // extinction per meter at y = 0 and below
    f32 volumetric_height_falloff = 0.15f;   // density × exp(-falloff · y) above 0
    f32 volumetric_anisotropy = 0.5f;        // > 0 glows toward the lights
    math::vec3 volumetric_albedo{0.9f, 0.9f, 0.9f};
    f32 volumetric_distance = 48.0f;         // meters the froxel grid covers
};
MCE_COMPONENT(Environment3D, (sky_texture, Help{"equirect sky map; empty = gradient"}), sun_direction,
              (sun_intensity, Range{0, 50}), sun_color, sun_shadows, ambient_sky, (exposure, Range{0.01f, 16}), ambient_ground,
              (fog_density, Range{0, 0.2f}), fog_color, sky_enabled, sky_zenith, (sky_sun_size, Range{0.99f, 1}), sky_horizon,
              (sky_sun_intensity, Range{0, 100}), sky_ground, (sky_texture_intensity, Range{0, 10}), shadow_splits,
              sky_texture_additive, (sky_specular_ambient, Range{0, 2}), bloom, (bloom_intensity, Range{0, 1}),
              (bloom_threshold, Range{0, 10}), fxaa, (shadow_cascades, Range{1, 4}), (shadow_distance, Range{1, 2000}),
              (shadow_blend, Range{0, 0.5f}), (sky_ibl, Help{"1: SH9 sky irradiance + pre-filtered sky reflections"}),
              (ibl_texture_intensity, Range{0, 10}),
              (ibl_texture, Help{"equirect the image-based lighting bakes from; empty = the visible sky"}),
              (atmosphere, Help{"1: physically based sky (tints the sun, sets the ambient)"}), (atmosphere_intensity, Range{0, 10}),
              (atmosphere_haze, Range{0, 10}), (time_of_day, Range{-1, 24}, Help{"hours placing the sun; -1 = sun_direction"}),
              (volumetric_fog, Help{"1: froxel volumetric fog lit by the sun and lights (replaces the exp² fog)"}),
              (volumetric_density, Range{0, 1}), (volumetric_height_falloff, Range{0, 2}), (volumetric_anisotropy, Range{-0.9f, 0.9f}),
              volumetric_albedo, (volumetric_distance, Range{4, 500}))

// ---- static colliders in the 3D physics world --------------------------------------------------

// A static box in Physics3D's World3D, built at Startup from the entity's transform: `half`
// (scaled by the entity's scale) around the entity's position plus `offset` (entity space),
// tilted by `pitch_deg` about its local X (ramps) after the entity's rotation.
struct BoxCollider3D {
    math::vec3 half{0.5f, 0.5f, 0.5f};
    f32 pitch_deg = 0.0f;
    math::vec3 offset;
    f32 friction = 0.6f;
    u32 layer = 0;  // collision layer 0..15
    u32 pad_ = 0;
};
MCE_COMPONENT(BoxCollider3D, half, (pitch_deg, Range{-90, 90}), offset, (friction, Range{0, 2}), (layer, Range{0, 15}))

// A static triangle-mesh collider from a mesh asset (its rest pose), built at Startup under the
// entity's transform. Use a simple collision mesh, not the drawn one, for large levels.
struct MeshCollider3D {
    asset::AssetRef<asset::Mesh> mesh;
    f32 friction = 0.6f;
    u32 layer = 0;
};
MCE_COMPONENT(MeshCollider3D, mesh, (friction, Range{0, 2}), (layer, Range{0, 15}))

// World resource: the 3D physics world the colliders above live in. Games add their own bodies
// and characters to `world` (Entity ids of their choosing) and query it. With `auto_step` the kit
// steps it every FixedUpdate_Physics; a game that steps it itself (only while playing) sets 0.
struct Physics3D {
    physics::World3D world{physics::PhysicsConfig3D{}};
    u32 auto_step = 1;
    u32 colliders = 0;  // bodies the kit built at Startup
};

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

// Registers the kit3d components, sets the Environment3D, StaticInstances3D and Physics3D
// resources (each kept if already set: a game sets Physics3D first for its own PhysicsConfig3D)
// and adds the systems:
//   Startup              "kit3d_environment"  the scene's Environment3D component, if any, into the resource
//   Startup              "kit3d_colliders"    BoxCollider3D / MeshCollider3D bodies (after the environment)
//   FixedUpdate_Physics  "kit3d_physics"      Physics3D::world.step when auto_step
//   FixedUpdate_Post     "kit3d_animators"    Animator time, cross-fades, graphs and events (anim3d.h)
//   FixedUpdate_Post     "kit3d_attach"       AttachToBone poses (after the animators)
// Idempotent. `assets` (for MeshCollider3D, Animator graphs and clip lengths, AttachToBone) may be
// null: mesh colliders are then skipped, graphs do not run and clips loop.
void install_kit3d(World& world, asset::AssetSystem* assets);

// Model-space triangles of a cooked mesh payload (CHNK_MESH) in its rest pose, every submesh
// dequantized and placed by its node; positions are multiplied by `transform` (scale, pose).
// False when the payload does not read. For colliders and tools; skinned submeshes use the bind pose.
bool mesh_triangles(Span<const byte> mesh_blob, const math::mat4& transform, Array<math::vec3>& vertices, Array<u32>& indices);

// True when the world has an entity with a Camera3DComponent (the player then draws in 3D).
bool has_camera_3d(World& world);

}  // namespace mce::kit
