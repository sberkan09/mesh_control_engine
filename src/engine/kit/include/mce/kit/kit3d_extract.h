#pragma once
// Kit3DExtract — the render extract for the kit3d components (kit3d.h), the 3D twin of
// SpriteExtract: reads MeshRef / Animator / PointLight3D / SpotLight3D / ReflectionProbe3D /
// Camera3DComponent +
// LocalTransform (WorldTransform when the hierarchy wrote one), the Environment3D resource (or
// component) and StaticInstances3D, and fills a Scene3D in ECS iteration order.
//
//   kit::Kit3DExtract extract;
//   extract.init(renderer.meshes(), &assets);
//   render::Camera3D cam;
//   if (extract.camera(world, alpha, cam)) {     // the primary Camera3DComponent
//       render::Scene3D scene(renderer.meshes(), mem::frame());
//       scene.begin(cam, viewport);
//       extract.extract(world, scene, alpha);     // environment, meshes, lights, instances
//       scene.build();
//       renderer.record_3d(cmd, scene);
//   }
//
// Meshes (and the textures their materials name) are loaded on first sight, one reference held
// per GUID, and uploaded through the host's render::AssetUploadSink; preload() loads every mesh
// and texture of the mounted content up front (players do, so the first frames do not hitch). A
// mesh GUID that is not mounted is skipped with one warning.
//
// Animator poses live in the extract until its next extract() call, so record the scene first.
//
// Interpolation: `alpha` is the app loop's fraction between the last tick and the next. Cameras,
// and MeshRefs with `interpolate` set, are drawn between the transforms they had at the last two
// ticks the extract saw (a render-side cache: the simulation never sees it).

#include "mce/asset/asset_system.h"
#include "mce/ecs/world.h"
#include "mce/kit/kit3d.h"
#include "mce/render/quality.h"
#include "mce/render/scene3d.h"

namespace mce::kit {

struct Kit3DStats {
    u32 meshes = 0;          // MeshRefs submitted
    u32 instances = 0;       // StaticInstances3D copies submitted
    u32 cells_visible = 0;
    u32 point_lights = 0;
    u32 spot_lights = 0;
    u32 reflection_probes = 0;
    u32 animated = 0;
    u32 missing = 0;         // MeshRefs whose mesh is not resident
};

class Kit3DExtract {
public:
    Kit3DExtract() = default;
    Kit3DExtract(const Kit3DExtract&) = delete;
    Kit3DExtract& operator=(const Kit3DExtract&) = delete;
    ~Kit3DExtract() { shutdown(); }

    // `meshes` is the store Scene3D draws from (renderer.meshes(), or a CPU-only store in tests).
    // `assets` may be null: then only meshes bound with bind_mesh() draw.
    void init(const render::MeshStore& meshes, asset::AssetSystem* assets);
    void shutdown();
    // Forgets every cached mesh / texture binding and transform (after remounting a different pack).
    void reset(asset::AssetSystem* assets);

    // Loads every texture, then every mesh, of the mounted content (handles held until shutdown).
    ResultV preload();
    // Draws `guid` as mesh-store key `key` without the asset system (tests, hosts that upload
    // their own meshes).
    void bind_mesh(asset::Guid guid, u32 key);
    // The mesh-store key for a mesh GUID (loaded on first sight), or ~0u when it cannot be drawn.
    u32 mesh_key(asset::Guid guid);
    // The texture-store key for a texture GUID (loaded on first sight), or ~0u.
    u32 texture_key(asset::Guid guid);
    // A MeshRef.material (.mcmat) as a render override (loaded on first sight, its maps too), or
    // null when it cannot be read. Stable until reset() / shutdown().
    const render::MaterialOverride* material(asset::Guid guid);

    // The primary Camera3DComponent's view (interpolated by `alpha`). False when the scene has none
    // (`out` is then a default camera at the origin). `component` (optional) receives the camera's
    // component (mouse_look).
    bool camera(World& world, f32 alpha, render::Camera3D& out, Camera3DComponent* component = nullptr);
    // The view of one entity's Camera3DComponent, primary or not (a preview from another camera).
    bool camera_of(World& world, Entity e, f32 alpha, render::Camera3D& out);
    // The environment, then every kit3d drawable, into a scene begun with scene.begin().
    void extract(World& world, render::Scene3D& scene, f32 alpha);
    // The environment alone (Environment3D resource, else the first component, else defaults).
    void apply_environment(World& world, render::Scene3D& scene);

    const Kit3DStats& stats() const { return stats_; }

    // The quality tier's caps on the scene's look (r.quality, render/quality.h): at most its shadow
    // cascades, its shadow-map resolution, bloom and FXAA only when the tier keeps them. Default:
    // no caps (High's knobs, with up to kMaxCascades), which is what the player passes on High.
    void set_quality(const render::QualitySettings& q) { quality_ = q; }

    // Reflection probe and GI volume bakes (M-VISUAL 2 and 6, the editor's bakes): every MeshRef and
    // static instance is submitted whatever the view (no culling, no distance thinning), probes and
    // the volume ignore their existing bakes, and the volume replaces each probe it re-captures
    // (no hysteresis, kGiMaxBatchProbes a frame) whatever the quality tier.
    void set_bake_mode(bool on) { bake_mode_ = on; }
    // The entities of the probes the last extract() added to the scene, in slot order.
    Span<const Entity> probe_entities() const { return probe_entities_.span(); }
    // The GiVolume3D the last extract() gave the scene (null entity: none).
    Entity gi_entity() const { return gi_entity_; }

private:
    bool view_of(World& world, Entity e, const Camera3DComponent& c, f32 alpha, render::Camera3D& out);
    struct MeshEntry {
        asset::Guid guid;
        asset::AnyHandle handle;
        u32 key = ~0u;
        bool warned = false;
    };
    struct TextureEntry {
        asset::Guid guid;
        asset::AnyHandle handle;
        u32 key = ~0u;
    };
    struct MaterialEntry {
        asset::Guid guid;
        asset::AnyHandle handle;
        mem::UniquePtr<render::MaterialOverride> value;  // null: failed
    };
    struct Tracked {  // per entity index: the transforms at the last two ticks seen
        u32 generation = 0;
        u64 tick = ~u64(0);
        math::vec3 prev_pos, cur_pos;
        math::quat prev_rot, cur_rot;
        math::vec3 prev_scale{1, 1, 1}, cur_scale{1, 1, 1};
    };
    void track(Entity e, const math::vec3& pos, const math::quat& rot, const math::vec3& scale, u64 tick);
    void interpolated(Entity e, f32 alpha, math::vec3& pos, math::quat& rot, math::vec3& scale) const;
    void release_handles();

    render::QualitySettings quality_{.shadow_cascades = render::kMaxCascades};
    const render::MeshStore* meshes_ = nullptr;
    asset::AssetSystem* assets_ = nullptr;
    Array<MeshEntry> mesh_entries_;
    Array<TextureEntry> texture_entries_;
    Array<MaterialEntry> material_entries_;
    Array<asset::AnyHandle> preloaded_;
    Array<Tracked> tracked_;
    Array<math::mat4> poses_;              // this frame's Animator poses (Scene3D holds spans into it)
    Array<render::MeshInstance> pending_;  // this frame's MeshRefs, added once their poses are final
    Array<u32> pose_first_;                // (first pose, node count, pending index) per animated MeshRef
    Array<u32> static_keys_;
    Array<u32> pending_entity_;  // entity index of each pending_ instance
    Array<u8> lod_states_;   // per entity index: the LOD level last drawn (Scene3D hysteresis)
    Array<u8> static_lods_;  // per StaticInstances3D copy
    Array<math::mat4> worlds_[2];  // per entity index: the world drawn this frame and last (TAA velocity)
    Array<u32> world_stamp_;       // per entity index: the extract that last drew it
    u32 world_parity_ = 0;
    u32 frame_stamp_ = 0;
    Kit3DStats stats_;
    bool bake_mode_ = false;
    Array<Entity> probe_entities_;
    Entity gi_entity_;
};

}  // namespace mce::kit
