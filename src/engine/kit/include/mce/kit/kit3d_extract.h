#pragma once
// Kit3DExtract — the render extract for the kit3d components (kit3d.h), the 3D twin of
// SpriteExtract: reads MeshRef / Animator / PointLight3D / SpotLight3D / ReflectionProbe3D /
// Foliage3D / Decal3D / ParticleEmitter3D / Terrain3D / Camera3DComponent +
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
// Terrain3D (M-TERRAIN): the extract keeps each terrain's LOD tree and uploads a node's mesh into the
// store the first time it is drawn (under MeshStore::allocate_key keys), releasing nodes not drawn for
// a while; this needs the store given to init() as non-const (the player's and the editor's are).
// A terrain with splat layers (M-TERRAIN 2) also packs its layers' maps into texture arrays and its
// weights into a texture, which needs set_texture_store(); without one it draws with Terrain3D.material.
//
// StaticMerge3D (M-VISUAL3 5b): the rigid MeshRef children of an entity carrying one are merged into a few meshes
// per cell and material (render/static_merge.h), uploaded under MeshStore::allocate_key() keys (this needs the
// store given to init() as non-const, as terrains do) and drawn in place of the children; set_static_merge(false)
// draws every child on its own again (the editor viewport, for selection and picking).
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
#include "mce/render/terrain.h"

namespace mce::kit {

class RenderTextures;

struct Kit3DStats {
    u32 meshes = 0;          // MeshRefs submitted
    u32 instances = 0;       // StaticInstances3D copies submitted
    u32 cells_visible = 0;
    u32 point_lights = 0;
    u32 spot_lights = 0;
    u32 reflection_probes = 0;
    u32 animated = 0;
    u32 missing = 0;         // MeshRefs whose mesh is not resident
    u32 foliage = 0;         // Foliage3D batches submitted (M-VISUAL 9)
    u32 foliage_instances = 0;
    u32 decals = 0;          // Decal3Ds submitted (the scene culls them)
    u32 particle_emitters = 0;
    u32 terrains = 0;          // Terrain3Ds drawn (M-TERRAIN)
    u32 terrain_nodes = 0;     // their LOD nodes submitted (the scene culls them per pass)
    u32 terrain_triangles = 0;
    u32 terrain_uploads = 0;   // node meshes built and uploaded this frame
    u32 terrain_resident = 0;  // node meshes held in the store
    u32 merged_groups = 0;     // StaticMerge3D components with at least one merged cell (M-VISUAL3 5b)
    u32 merged_children = 0;   // MeshRef children drawn through them instead of on their own
    u32 merged_cells = 0;      // merged meshes submitted (a cell, or a cell and shadow flag), before culling
};

class Kit3DExtract {
public:
    Kit3DExtract();
    Kit3DExtract(const Kit3DExtract&) = delete;
    Kit3DExtract& operator=(const Kit3DExtract&) = delete;
    ~Kit3DExtract();

    // `meshes` is the store Scene3D draws from (renderer.meshes(), or a CPU-only store in tests).
    // `assets` may be null: then only meshes bound with bind_mesh() draw.
    void init(const render::MeshStore& meshes, asset::AssetSystem* assets);
    // As above, and terrains may upload their node meshes into `meshes`.
    void init(render::MeshStore& meshes, asset::AssetSystem* assets);
    // The store terrains upload their splat-layer arrays into (renderer.textures(); null: none).
    void set_texture_store(render::TextureStore* textures) { textures_ = textures; }
    // M-VIEWS 3: a MeshRef with ShowRenderTexture draws the texture as its base color or emission.
    void set_render_textures(const RenderTextures* rts) { render_textures_ = rts; }
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
    // M-VIEWS: the same frame from another camera, into another scene begun with that camera's
    // view; after extract() for the frame's first view. The frame's clocks and TAA history stay
    // as extract() left them (no second time step); culling and LODs follow this scene's view.
    // Poses move with each call, so record the previous view's scene before extracting the next.
    void extract_view(World& world, render::Scene3D& scene, f32 alpha) {
        extra_view_ = true;
        extract(world, scene, alpha);
        extra_view_ = false;
    }
    // The environment alone (Environment3D resource, else the first component, else defaults).
    void apply_environment(World& world, render::Scene3D& scene);

    const Kit3DStats& stats() const { return stats_; }

    // The quality tier's caps on the scene's look (r.quality, render/quality.h): at most its shadow
    // cascades, its shadow-map resolution, bloom and FXAA only when the tier keeps them. Default:
    // no caps (High's knobs, with up to kMaxCascades), which is what the player passes on High.
    void set_quality(const render::QualitySettings& q) { quality_ = q; }
    // Seconds since the last extracted frame: how far Environment3D::auto_exposure adapts per frame,
    // and the step of the scene's clock (foliage wind, particles: Scene3D::set_time).
    void set_frame_dt(f32 dt) { frame_dt_ = dt; }
    // The scene clock's seconds so far (advanced by the frame dt every extract()).
    f32 time() const { return time_; }

    // StaticMerge3D on (the default) or off. Off releases the merged meshes at once and draws the children one by one.
    void set_static_merge(bool on);
    bool static_merge() const { return static_merge_; }

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
    bool base_view_of(World& world, Entity e, const Camera3DComponent& c, f32 alpha, render::Camera3D& out);  // no blend or shake
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
        u32 parallax_key = ~0u;                          // the [normal, height] array this material owns (M-VISUAL3 2)
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
    u32 build_parallax_array(render::MaterialOverride& m, asset::Guid material_guid);
    void release_handles();
    void release_terrains();
    void extract_terrains(World& world, render::Scene3D& scene);
    struct TerrainEntry;
    void release_terrain(TerrainEntry& t);
    void build_terrain_shading(TerrainEntry& t, const asset::TerrainView& view);
    // static_merge3d.cpp
    struct MergeState;
    void update_static_merges(World& world);
    void release_static_merges();
    bool read_mesh_view(asset::Guid guid, asset::MeshView& out);

    render::QualitySettings quality_{.shadow_cascades = render::kMaxCascades};
    f32 frame_dt_ = 1.0f / 60.0f;
    const render::MeshStore* meshes_ = nullptr;
    render::MeshStore* writable_ = nullptr;  // the same store, when init() got it non-const
    render::TextureStore* textures_ = nullptr;
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
    Array<u8> prev_ok_;            // per entity index: this frame's draw had last frame's world (worlds_)
    bool extra_view_ = false;      // extract_view(): a later view of the frame
    const RenderTextures* render_textures_ = nullptr;
    Array<render::MaterialOverride> screen_materials_;  // this extract's ShowRenderTexture materials (reserved up front: stable)
    u32 world_parity_ = 0;
    u32 frame_stamp_ = 0;
    Kit3DStats stats_;
    bool bake_mode_ = false;
    f32 time_ = 0.0f;
    struct FoliageEntry {  // a Foliage3D's scatter, kept while its inputs stay the same
        Entity entity;
        u64 key = 0;
        u32 version = 0;
        u32 stamp = 0;     // the extract that last saw it
        Array<math::mat4> transforms;       // kind 0 (Foliage3D::mesh)
        Array<math::mat4> more[3];          // kinds 1..3 (Foliage3D::mesh_1..3), M-VISUAL3 6
    };
    Array<FoliageEntry> foliage_;
    Array<FoliageEntry> terrain_foliage_;  // TerrainFoliage3D scatters (M-TERRAIN 5)
    struct BurstEntry {    // a ParticleEmitter3D's last burst_version
        Entity entity;
        u32 burst_version = 0;
        u32 stamp = 0;
    };
    Array<BurstEntry> bursts_;
    struct TerrainEntry {  // a Terrain3D's LOD tree and its resident node meshes
        Entity entity;
        asset::Guid guid;
        asset::AnyHandle handle;
        u32 generation = 0;   // of the asset the tree was built from
        f32 uv_tile = 0.0f;
        u32 stamp = 0;        // the extract that last saw it
        render::TerrainLod lod;
        Array<u32> keys;      // per node: its mesh-store key, ~0u = not resident
        Array<u32> used;      // per node: the extract that last drew it
        math::mat4 world_prev = math::mat4::identity();  // last drawn frame's world (TAA velocity)
        math::mat4 world_cur = math::mat4::identity();
        bool drawn = false;
        render::TerrainShading shading;  // layer_count 0: drawn with Terrain3D.material
    };
    Array<mem::UniquePtr<TerrainEntry>> terrains_;
    Array<u32> terrain_nodes_;  // scratch: this frame's selection
    Array<byte> terrain_blob_;  // scratch: a node's mesh payload
    Array<Entity> probe_entities_;
    Entity gi_entity_;
    // StaticMerge3D (M-VISUAL3 5b)
    struct MergedDraw {  // a merged mesh to submit this frame
        u32 key = ~0u;
        math::vec3 center;  // its bounds, as a sphere to cull
        f32 radius = 0.0f;
        bool cast_shadows = true;
        u8* lod_state = nullptr;
    };
    mem::UniquePtr<MergeState> merge_;
    Array<MergedDraw> merged_draws_;
    Array<u32> merged_stamp_;  // per entity index: == merge_stamp_ while its MeshRef is drawn through a merged mesh
    u32 merge_stamp_ = 0;
    bool static_merge_ = true;
};

}  // namespace mce::kit
