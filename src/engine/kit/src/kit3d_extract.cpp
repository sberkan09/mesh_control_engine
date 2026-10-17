#include "mce/kit/kit3d_extract.h"

#include <cmath>
#include <cstdlib>

#include "mce/asset/formats.h"
#include "mce/ecs/hierarchy.h"
#include "mce/ecs/origin.h"
#include "mce/ecs/transform.h"
#include "mce/kit/cinematic.h"
#include "mce/kit/render_textures.h"
#include "mce/convar.h"
#include "mce/log.h"
#include "mce/refl/property_path.h"
#include "mce/render/accessibility.h"
#include "mce/string_id.h"
#include "kit3d_internal.h"

namespace mce::kit {

using namespace mce::math;

namespace {

// The entity's pose: WorldTransform when the hierarchy wrote one, else LocalTransform.
bool pose_of(World& world, Entity e, vec3& pos, quat& rot, vec3& scale) {
    if (const WorldTransform* w = world.try_get<WorldTransform>(e)) {
        pos = w->position;
        rot = w->rotation;
        scale = w->scale;
        return true;
    }
    if (const LocalTransform* l = world.try_get<LocalTransform>(e)) {
        pos = l->position;
        rot = l->rotation;
        scale = l->scale;
        return true;
    }
    return false;
}

u32 find_clip_id(const render::GpuMesh& mesh, u64 id) {
    for (u32 i = 0; i < mesh.clips.size(); ++i)
        if (StringId(mesh.clips[i].name.view()).value == id) return i;
    return ~0u;
}


// MCP render_frame / render_sheet per-shot control (M-VISUAL3 3c): the tool hands the player these as flags.
MCE_CVAR(cv_env_override, "r.env_override", "",
         "Environment3D field overrides, 'field=value;field=a,b,c;...' (numbers, 0/1 for flags, comma lanes for vectors and colors); empty = none",
         CVarFlags::None);
MCE_CVAR(cv_dof_focus, "r.dof_focus", 0.0f, "Depth of field: meters to the sharpest plane, overriding the environment and the camera (0 = off)",
         CVarFlags::None);
MCE_CVAR(cv_exposure_scale, "r.exposure_scale", 1.0f, "Multiplies the scene's exposure (1 = unchanged)", CVarFlags::None);

// The parsed r.env_override, rebuilt only when the string changes.
struct EnvOverride {
    refl::PropertyRef ref;
    f32 lanes[4] = {};
    u32 count = 0;
};
constexpr u32 kMaxEnvOverrides = 32;

struct EnvOverrides {
    FixedString<1024> text;
    EnvOverride items[kMaxEnvOverrides];
    u32 count = 0;
};

void parse_env_overrides(StringView text, EnvOverrides& out) {
    out.count = 0;
    const refl::TypeRecord& type = refl::type_of<Environment3D>();
    usize pos = 0;
    while (pos < text.size() && out.count < kMaxEnvOverrides) {
        usize end = pos;
        while (end < text.size() && text[end] != ';') ++end;
        StringView item = text.substr(pos, end - pos);
        pos = end + 1;
        usize eq = 0;
        while (eq < item.size() && item[eq] != '=') ++eq;
        if (eq == 0 || eq >= item.size()) continue;
        Result<refl::PropertyRef> ref = refl::resolve_path(type, item.substr(0, eq));
        if (!ref) {
            MCE_LOG_WARN("r.env_override: no Environment3D field '{}'", item.substr(0, eq));
            continue;
        }
        EnvOverride o;
        o.ref = ref.value();
        char buf[256];
        const StringView rest = item.substr(eq + 1);
        const usize n = rest.size() < sizeof(buf) - 1 ? rest.size() : sizeof(buf) - 1;
        for (usize i = 0; i < n; ++i) buf[i] = rest[i];
        buf[n] = 0;
        const char* q = buf;
        while (o.count < 4 && *q) {
            char* next = nullptr;
            o.lanes[o.count++] = std::strtof(q, &next);
            if (next == q) break;
            q = *next == ',' ? next + 1 : next;
        }
        if (o.count > 0) out.items[out.count++] = o;
    }
}

// Writes the override into env. A lane count that does not match the field is skipped (the MCP tool validates it).
void apply_env_override(Environment3D& env, const EnvOverride& o) {
    using refl::FieldKind;
    void* base = &env;
    u8* at = static_cast<u8*>(base) + o.ref.offset;
    const f32 v = o.lanes[0];
    switch (o.ref.kind) {
        case FieldKind::Bool: *reinterpret_cast<bool*>(at) = v != 0.0f; break;
        case FieldKind::F32: *reinterpret_cast<f32*>(at) = v; break;
        case FieldKind::U32: *reinterpret_cast<u32*>(at) = v < 0.0f ? 0u : static_cast<u32>(v); break;
        case FieldKind::I32: *reinterpret_cast<i32*>(at) = static_cast<i32>(v); break;
        case FieldKind::Vec3:
        case FieldKind::Color:
        case FieldKind::Vec4: {
            const u32 want = o.ref.kind == FieldKind::Vec3 ? 3u : 4u;
            if (o.count < want - (o.ref.kind == FieldKind::Color ? 1u : 0u)) break;
            f32* p = reinterpret_cast<f32*>(at);
            for (u32 i = 0; i < want && i < o.count; ++i) p[i] = o.lanes[i];
            break;
        }
        default: break;
    }
}

}  // namespace

void Kit3DExtract::init(const render::MeshStore& meshes, asset::AssetSystem* assets) {
    meshes_ = &meshes;
    writable_ = nullptr;
    assets_ = assets;
}

void Kit3DExtract::init(render::MeshStore& meshes, asset::AssetSystem* assets) {
    meshes_ = &meshes;
    writable_ = &meshes;
    assets_ = assets;
}

void Kit3DExtract::release_terrain(TerrainEntry& t) {
    if (writable_)
        for (u32 k : t.keys)
            if (k != ~0u) writable_->release(k);
    t.keys.clear();
    t.used.clear();
    if (textures_)
        for (u32 k : {t.shading.albedo_key, t.shading.normal_key, t.shading.orm_key, t.shading.splat_key})
            if (k != ~0u) textures_->release(k);
    t.shading = render::TerrainShading{};
    if (assets_ && t.handle.generation != 0) assets_->release(t.handle);
    t.handle = {};
}

void Kit3DExtract::release_terrains() {
    for (auto& t : terrains_) release_terrain(*t);
    terrains_.clear();
}

// The splat layers' look: each layer's .mcmat (its colors and maps), the maps packed into one texture
// array per kind and the weights into a 2-layer array, all in the texture store.
void Kit3DExtract::build_terrain_shading(TerrainEntry& t, const asset::TerrainView& view) {
    render::TerrainShading& sh = t.shading;
    sh = render::TerrainShading{};
    if (view.layers.empty() || !textures_) return;
    sh.layer_count = static_cast<u32>(view.layers.size());
    sh.size = {view.header->size[0], view.header->size[1]};
    sh.splat_resolution = view.splat_resolution();
    asset::TextureView maps[3][render::TerrainShading::kMaxLayers] = {};
    const asset::TextureView* ptrs[3][render::TerrainShading::kMaxLayers] = {};
    static constexpr asset::MaterialSlot kSlots[3] = {asset::MaterialSlot::BaseColor, asset::MaterialSlot::Normal,
                                                      asset::MaterialSlot::MetallicRoughness};
    for (u32 i = 0; i < sh.layer_count; ++i) {
        const asset::TerrainLayer& src = view.layers[i];
        render::TerrainShading::Layer& l = sh.layers[i];
        l.tile = src.tile;
        l.height_strength = src.height_strength;
        l.triplanar = (src.flags & asset::TerrainLayerFlags::kTriplanar) != 0;
        const render::MaterialOverride* m = material(src.material_guid());
        if (!m) continue;
        l.base_color = {m->params.base_color[0], m->params.base_color[1], m->params.base_color[2], 1.0f};
        l.roughness = m->params.roughness;
        l.metallic = m->params.metallic;
        l.occlusion_in_orm = (m->params.flags & asset::MaterialFlags::OcclusionInMR) != 0;
        for (u32 k = 0; k < 3; ++k) {
            const asset::Guid g = m->params.texture(kSlots[k]);
            if (texture_key(g) == ~0u) continue;
            for (const TextureEntry& te : texture_entries_) {
                if (!(te.guid == g) || te.handle.generation == 0) continue;
                const Result<asset::TextureView> tv =
                    asset::TextureView::read(assets_->resolve(asset::AssetHandle<asset::Texture>{te.handle.index, te.handle.generation}));
                if (tv) {
                    maps[k][i] = tv.value();
                    ptrs[k][i] = &maps[k][i];
                }
            }
        }
    }
    static constexpr const char* kNames[3] = {"terrain.albedo", "terrain.normal", "terrain.orm"};
    u32* keys[3] = {&sh.albedo_key, &sh.normal_key, &sh.orm_key};
    for (u32 k = 0; k < 3; ++k) {
        render::LayerArray arr;
        if (!render::compose_layer_array(Span<const asset::TextureView* const>(ptrs[k], sh.layer_count), arr, kNames[k])) continue;
        const u32 key = textures_->allocate_key();
        if (!textures_->upload(key, 1, arr.upload)) {
            MCE_LOG_WARN("kit3d: terrain {} layer maps failed to upload", asset::to_string(t.guid).view());
            continue;
        }
        *keys[k] = key;
        for (u32 i = 0; i < sh.layer_count; ++i) {
            const bool present = (arr.present & (1u << i)) != 0;
            if (k == 0) sh.layers[i].albedo_map = present;
            else if (k == 1) sh.layers[i].normal_map = present;
            else sh.layers[i].orm_map = present;
        }
    }
    render::TextureMipUpload mip;
    const u32 splat = textures_->allocate_key();
    if (!textures_->upload(splat, 1, render::splat_upload(view, mip))) {
        MCE_LOG_WARN("kit3d: terrain {} splat map failed to upload", asset::to_string(t.guid).view());
        sh.layer_count = 0;  // fall back to the plain material
        return;
    }
    sh.splat_key = splat;
}

void Kit3DExtract::release_handles() {
    if (textures_)
        for (const MaterialEntry& m : material_entries_)
            if (m.parallax_key != ~0u) textures_->release(m.parallax_key);
    if (assets_) {
        for (const MeshEntry& m : mesh_entries_)
            if (m.handle.generation != 0) assets_->release(m.handle);
        for (const TextureEntry& t : texture_entries_)
            if (t.handle.generation != 0) assets_->release(t.handle);
        for (const MaterialEntry& m : material_entries_)
            if (m.handle.generation != 0) assets_->release(m.handle);
        for (const asset::AnyHandle& h : preloaded_) assets_->release(h);
    }
    mesh_entries_.clear();
    texture_entries_.clear();
    material_entries_.clear();
    preloaded_.clear();
    tracked_.clear();
}

void Kit3DExtract::shutdown() {
    release_static_merges();
    release_terrains();
    release_handles();
    meshes_ = nullptr;
    writable_ = nullptr;
    textures_ = nullptr;
    assets_ = nullptr;
}

void Kit3DExtract::reset(asset::AssetSystem* assets) {
    release_static_merges();
    release_terrains();
    release_handles();
    foliage_.clear();
    terrain_foliage_.clear();
    bursts_.clear();
    assets_ = assets;
}

ResultV Kit3DExtract::preload() {
    if (!assets_) return ok();
    // Textures first: a mesh's materials resolve their maps by key when it uploads.
    for (asset::AssetKey k = 0; k < assets_->asset_count(); ++k) {
        if (assets_->info(k).type != asset::ChunkType::Texture) continue;
        MCE_TRY_ASSIGN(asset::AssetHandle<asset::Texture> h, assets_->load_sync<asset::Texture>(k));
        preloaded_.push_back(asset::AnyHandle(h));
    }
    for (asset::AssetKey k = 0; k < assets_->asset_count(); ++k) {
        if (assets_->info(k).type != asset::ChunkType::Mesh) continue;
        MCE_TRY_ASSIGN(asset::AssetHandle<asset::Mesh> h, assets_->load_sync<asset::Mesh>(k));
        preloaded_.push_back(asset::AnyHandle(h));
    }
    return ok();
}

void Kit3DExtract::bind_mesh(asset::Guid guid, u32 key) {
    for (MeshEntry& m : mesh_entries_)
        if (m.guid == guid) {
            m.key = key;
            return;
        }
    MeshEntry e;
    e.guid = guid;
    e.key = key;
    mesh_entries_.push_back(e);
}

u32 Kit3DExtract::texture_key(asset::Guid guid) {
    if (guid == asset::Guid{} || !assets_) return ~0u;
    for (const TextureEntry& t : texture_entries_)
        if (t.guid == guid) return t.key;
    TextureEntry e;
    e.guid = guid;
    const asset::AssetKey key = assets_->find(guid);
    if (key != asset::kInvalidAssetKey) {
        if (Result<asset::AssetHandle<asset::Texture>> h = assets_->load_sync<asset::Texture>(key)) {
            e.handle = asset::AnyHandle(h.value());
            e.key = key;
        } else {
            MCE_LOG_WARN("kit3d: texture {} failed to load", asset::to_string(guid).view());
        }
    }
    texture_entries_.push_back(e);
    return e.key;
}

// M-VISUAL3 2: the normal map (layer 0) and the height map (layer 1) as one 2-layer array under a new
// texture key, which replaces the material's normal_key (the shader reads layer 1 for the march). Without a
// texture store, or when the two textures do not match (size, mips, format, color space) or the height does
// not load, the material draws without parallax: `ext`'s height is cleared so the renderer sees none.
// The array is built once, when the material first resolves, and lives until reset()/shutdown(): a texture hot
// reload or a texture store swap does not rebuild it (follow-up if the editor needs it).
u32 Kit3DExtract::build_parallax_array(render::MaterialOverride& m, asset::Guid material_guid) {
    const asset::Guid layer_guid[2] = {m.params.texture(asset::MaterialSlot::Normal), m.ext.height()};
    asset::TextureView views[2] = {};
    const asset::TextureView* ptrs[2] = {};
    if (textures_) {
        for (u32 i = 0; i < 2; ++i) {
            if (layer_guid[i] == asset::Guid{} || texture_key(layer_guid[i]) == ~0u) continue;
            for (const TextureEntry& te : texture_entries_) {
                if (!(te.guid == layer_guid[i]) || te.handle.generation == 0) continue;
                const Result<asset::TextureView> tv =
                    asset::TextureView::read(assets_->resolve(asset::AssetHandle<asset::Texture>{te.handle.index, te.handle.generation}));
                if (tv) {
                    views[i] = tv.value();
                    ptrs[i] = &views[i];
                }
                break;
            }
        }
        // Height is linear data: an sRGB one (and, with no normal map, the array's format) would bend the depth.
        if (ptrs[1] && (views[1].header->flags & asset::TextureFlags::Srgb)) ptrs[1] = nullptr;
    }
    render::LayerArray arr;
    u32 key = ~0u;
    const bool composed = ptrs[1] && render::compose_layer_array(Span<const asset::TextureView* const>(ptrs, 2), arr, "material.parallax", true);
    // Layer 1 (the height) must have made it in; a normal map that did not match leaves layer 0 flat instead.
    if (composed && (arr.present & 2u)) {
        key = textures_->allocate_key();
        if (!textures_->upload(key, 1, arr.upload)) key = ~0u;
    }
    if (key == ~0u) {
        MCE_LOG_WARN("kit3d: material {} draws without parallax: its height map {} is missing, not resident, imported as sRGB, or does "
                     "not match the normal map {} (same size, mips and format, both linear)",
                     asset::to_string(material_guid).view(), asset::to_string(layer_guid[1]).view(),
                     asset::to_string(layer_guid[0]).view());
        m.ext.height_scale = 0.0f;
        return ~0u;
    }
    m.normal_key = key;
    m.parallax = true;
    m.parallax_normal = (arr.present & 1u) != 0;
    return key;
}

const render::MaterialOverride* Kit3DExtract::material(asset::Guid guid) {
    if (guid == asset::Guid{} || !assets_) return nullptr;
    for (const MaterialEntry& m : material_entries_)
        if (m.guid == guid) return m.value.get();
    MaterialEntry e;
    e.guid = guid;
    if (Result<asset::AssetHandle<asset::Material>> h = assets_->load_sync<asset::Material>(guid)) {
        e.handle = asset::AnyHandle(h.value());
        if (const Result<const asset::MaterialChunk*> chunk = asset::MaterialChunk::read(assets_->resolve(h.value()))) {
            e.value = mem::make_unique<render::MaterialOverride>(mem::heap());
            render::MaterialOverride& m = *e.value;
            m.params = *chunk.value();
            m.ext = asset::MaterialExt::of_material(assets_->resolve(h.value()));
            m.albedo_key = texture_key(m.params.texture(asset::MaterialSlot::BaseColor));
            m.normal_key = texture_key(m.params.texture(asset::MaterialSlot::Normal));
            m.orm_key = texture_key(m.params.texture(asset::MaterialSlot::MetallicRoughness));
            m.emissive_key = texture_key(m.params.texture(asset::MaterialSlot::Emissive));
            // M-CONTENT 1b: a BC4 texture holds one channel (R). In a slot that reads more than R it draws red/grey, so say so once.
            static constexpr asset::MaterialSlot kWide[] = {asset::MaterialSlot::BaseColor, asset::MaterialSlot::MetallicRoughness,
                                                            asset::MaterialSlot::Emissive, asset::MaterialSlot::Normal};
            for (const asset::MaterialSlot slot : kWide) {
                const asset::Guid tg = m.params.texture(slot);
                if (tg == asset::Guid{}) continue;
                for (const TextureEntry& te : texture_entries_) {
                    if (!(te.guid == tg) || te.handle.generation == 0) continue;
                    const Result<asset::TextureView> tv =
                        asset::TextureView::read(assets_->resolve(asset::AssetHandle<asset::Texture>{te.handle.index, te.handle.generation}));
                    if (tv && tv.value().header->format == asset::TextureFormat::BC4)
                        MCE_LOG_WARN("kit3d: material {} binds texture {} cooked as BC4 (one channel) to a slot that reads more than R; "
                                     "set \"compress\": \"bc7\" (colour, ORM) or \"bc5\" (normal) on it",
                                     asset::to_string(guid).view(), asset::to_string(tg).view());
                    break;
                }
            }
            if (m.ext.parallax()) e.parallax_key = build_parallax_array(m, e.guid);
        } else {
            MCE_LOG_WARN("kit3d: material {} does not read", asset::to_string(guid).view());
        }
    } else {
        MCE_LOG_WARN("kit3d: material {} failed to load", asset::to_string(guid).view());
    }
    material_entries_.push_back(static_cast<MaterialEntry&&>(e));
    return material_entries_.back().value.get();
}

u32 Kit3DExtract::mesh_key(asset::Guid guid) {
    if (guid == asset::Guid{}) return ~0u;
    for (MeshEntry& m : mesh_entries_) {
        if (!(m.guid == guid)) continue;
        if (m.key != ~0u && meshes_ && !meshes_->get(m.key) && !m.warned) {
            m.warned = true;
            MCE_LOG_WARN("kit3d: mesh {} is not resident (no upload sink?)", asset::to_string(guid).view());
        }
        return m.key;
    }
    MeshEntry e;
    e.guid = guid;
    if (assets_) {
        const asset::AssetKey key = assets_->find(guid);
        if (key == asset::kInvalidAssetKey) {
            MCE_LOG_WARN("kit3d: mesh {} is not in the mounted content", asset::to_string(guid).view());
        } else if (Result<asset::AssetHandle<asset::Mesh>> h = assets_->load_sync<asset::Mesh>(key)) {
            e.handle = asset::AnyHandle(h.value());
            e.key = key;
            // Its material maps: a mesh holds texture keys, the textures must be resident to sample.
            if (const Result<asset::MeshView> view = asset::MeshView::read(assets_->resolve(h.value())))
                for (const asset::MeshSubmesh& sm : view.value().submeshes)
                    for (u32 s = 0; s < static_cast<u32>(asset::MaterialSlot::Count); ++s)
                        (void)texture_key(sm.material.texture(static_cast<asset::MaterialSlot>(s)));
        } else {
            MCE_LOG_WARN("kit3d: mesh {} failed to load", asset::to_string(guid).view());
        }
    }
    mesh_entries_.push_back(e);
    return e.key;
}

// ---- terrain (M-TERRAIN) ----------------------------------------------------------------------

namespace {
constexpr u32 kTerrainEvictFrames = 300;  // a node mesh not drawn for this many extracts is released
}

void Kit3DExtract::extract_terrains(World& world, render::Scene3D& scene) {
    world.query<const Terrain3D&>().each([&](Entity e, const Terrain3D& t) {
        if (!t.enabled || t.terrain.guid == asset::Guid{} || !assets_) return;
        vec3 pos, scale;
        quat rot;
        if (!pose_of(world, e, pos, rot, scale)) return;
        TerrainEntry* entry = nullptr;
        for (auto& te : terrains_)
            if (te->entity == e) entry = te.get();
        if (!entry) {
            terrains_.push_back(mem::make_unique<TerrainEntry>(mem::heap()));
            entry = terrains_.back().get();
            entry->entity = e;
        }
        entry->stamp = frame_stamp_;
        // (Re)build the tree when the asset, its cooked generation (hot reload) or the UV tiling changed.
        const asset::AssetKey key = assets_->find(t.terrain.guid);
        if (key == asset::kInvalidAssetKey) {
            if (!(entry->guid == t.terrain.guid)) MCE_LOG_WARN("kit3d: terrain {} is not in the mounted content", asset::to_string(t.terrain.guid).view());
            entry->guid = t.terrain.guid;
            return;
        }
        const u32 generation = assets_->generation(key);
        if (!(entry->guid == t.terrain.guid) || entry->generation != generation || entry->uv_tile != t.uv_tile || entry->lod.empty()) {
            release_terrain(*entry);
            entry->lod = render::TerrainLod{};
            entry->guid = t.terrain.guid;
            entry->generation = generation;
            entry->uv_tile = t.uv_tile;
            entry->drawn = false;
            if (Result<asset::AssetHandle<asset::Terrain>> h = assets_->load_sync<asset::Terrain>(key)) {
                entry->handle = asset::AnyHandle(h.value());
                entry->generation = assets_->generation(key);
                const Result<asset::TerrainView> view = asset::TerrainView::read(assets_->resolve(h.value()));
                if (!view || !entry->lod.init(view.value())) {
                    MCE_LOG_WARN("kit3d: terrain {} does not read", asset::to_string(t.terrain.guid).view());
                    entry->lod = render::TerrainLod{};
                } else {
                    build_terrain_shading(*entry, view.value());
                }
            } else {
                MCE_LOG_WARN("kit3d: terrain {} failed to load", asset::to_string(t.terrain.guid).view());
            }
            entry->keys.resize(entry->lod.node_count(), ~0u);
            entry->used.resize(entry->lod.node_count(), 0u);
        }
        if (entry->lod.empty()) return;

        render::TerrainSelectParams params;
        params.world = trs(pos, rot, scale);
        params.pixel_error = t.pixel_error > 0.0f ? t.pixel_error : quality_.terrain_pixel_error;
        params.shadow_reach = bake_mode_ ? 1e30f : (t.cast_shadows ? scene.shadow_settings().distance : 0.0f);
        terrain_nodes_.clear();
        entry->lod.select(scene.view(), params, terrain_nodes_);
        entry->world_prev = entry->drawn ? entry->world_cur : params.world;
        entry->world_cur = params.world;
        entry->drawn = true;
        const render::MaterialOverride* mat = material(t.material.guid);
        entry->shading.triplanar = quality_.terrain_triplanar;
        entry->shading.height_blend = quality_.terrain_height_blend;
        const render::TerrainShading* shading = entry->shading.layer_count > 0 ? &entry->shading : nullptr;
        for (u32 n : terrain_nodes_) {
            if (entry->keys[n] == ~0u) {
                if (!writable_) {
                    ++stats_.missing;
                    continue;
                }
                entry->lod.build_mesh(n, t.uv_tile, terrain_blob_);
                const u32 k = writable_->allocate_key();
                if (!writable_->upload(k, 1, terrain_blob_.span())) {
                    ++stats_.missing;
                    continue;
                }
                entry->keys[n] = k;
                ++stats_.terrain_uploads;
            }
            entry->used[n] = frame_stamp_;
            render::MeshInstance inst;
            inst.mesh = entry->keys[n];
            inst.world = entry->world_cur;
            inst.prev_world = &entry->world_prev;
            inst.tint = t.tint;
            inst.cast_shadows = t.cast_shadows != 0;
            inst.material = mat;
            inst.terrain = shading;
            scene.add(inst);
            ++stats_.terrain_nodes;
            stats_.terrain_triangles += entry->lod.node_triangles(n);
        }
        ++stats_.terrains;
    });
    // Drop terrains whose entity is gone, and (every 64th extract) node meshes not drawn lately.
    for (usize i = terrains_.size(); i-- > 0;) {
        TerrainEntry& te = *terrains_[i];
        if (te.stamp != frame_stamp_) {
            release_terrain(te);
            terrains_.erase(i);
            continue;
        }
        const bool sweep = (frame_stamp_ & 63u) == 0 && writable_;
        const u32 root = te.lod.empty() ? ~0u : te.lod.root();
        for (u32 n = 0; n < te.keys.size(); ++n) {
            if (te.keys[n] == ~0u) continue;
            if (sweep && n != root && frame_stamp_ - te.used[n] > kTerrainEvictFrames) {
                writable_->release(te.keys[n]);
                te.keys[n] = ~0u;
                continue;
            }
            ++stats_.terrain_resident;
        }
    }
}

// ---- interpolation ----------------------------------------------------------------------------

void Kit3DExtract::track(Entity e, const vec3& pos, const quat& rot, const vec3& scale, u64 tick) {
    if (e.index >= tracked_.size()) tracked_.resize(e.index + 1);
    Tracked& t = tracked_[e.index];
    if (t.generation != e.generation || t.tick == ~u64(0)) {
        t.generation = e.generation;
        t.tick = tick;
        t.prev_pos = t.cur_pos = pos;
        t.prev_rot = t.cur_rot = rot;
        t.prev_scale = t.cur_scale = scale;
        return;
    }
    if (t.tick == tick) {  // same tick: the newest pose wins (a teleport inside the frame)
        t.cur_pos = pos;
        t.cur_rot = rot;
        t.cur_scale = scale;
        return;
    }
    t.tick = tick;
    t.prev_pos = t.cur_pos;
    t.prev_rot = t.cur_rot;
    t.prev_scale = t.cur_scale;
    t.cur_pos = pos;
    t.cur_rot = rot;
    t.cur_scale = scale;
}

void Kit3DExtract::interpolated(Entity e, f32 alpha, vec3& pos, quat& rot, vec3& scale) const {
    if (e.index >= tracked_.size()) return;
    const Tracked& t = tracked_[e.index];
    if (t.generation != e.generation) return;
    const f32 a = clamp(alpha, 0.0f, 1.0f);
    pos = lerp(t.prev_pos, t.cur_pos, a);
    rot = slerp(t.prev_rot, t.cur_rot, a);
    scale = lerp(t.prev_scale, t.cur_scale, a);
}

// ---- camera and environment -----------------------------------------------------------------

void Kit3DExtract::sync_origin(World& world) {
    const WorldOrigin* o = world_origin(world);
    if (!o || o->serial == origin_serial_) return;
    const vec3 d{f32(o->cell[0] - origin_cells_[0]) * o->snap, f32(o->cell[1] - origin_cells_[1]) * o->snap,
                 f32(o->cell[2] - origin_cells_[2]) * o->snap};
    origin_serial_ = o->serial;
    for (int i = 0; i < 3; ++i) origin_cells_[i] = o->cell[i];
    if (d.x == 0.0f && d.y == 0.0f && d.z == 0.0f) return;
    for (Tracked& t : tracked_) {
        t.prev_pos = t.prev_pos - d;
        t.cur_pos = t.cur_pos - d;
    }
    for (Array<FoliageEntry>* list : {&foliage_, &terrain_foliage_})
        for (FoliageEntry& f : *list) {
            for (mat4& m : f.transforms) {
                m.c[3][0] -= d.x;
                m.c[3][1] -= d.y;
                m.c[3][2] -= d.z;
            }
            f.rebased = true;
            ++f.version;  // re-upload the moved copies
        }
    for (auto& t : terrains_) t->drawn = false;  // no previous world for this frame (a camera cut)
    shift_static_merges(d);
    // A camera cut for the temporal history: no draw gets last frame's world.
    ++frame_stamp_;
    origin_pending_ = origin_pending_ + d;
}

bool Kit3DExtract::camera(World& world, f32 alpha, render::Camera3D& out, Camera3DComponent* component) {
    sync_origin(world);
    out = render::Camera3D{};
    bool found = false;
    world.query<const Camera3DComponent&>().each([&](Entity e, const Camera3DComponent& c) {
        if (found || !c.primary) return;
        found = view_of(world, e, c, alpha, out);
        if (found && component) *component = c;
    });
    return found;
}

bool Kit3DExtract::camera_of(World& world, Entity e, f32 alpha, render::Camera3D& out) {
    sync_origin(world);
    const Camera3DComponent* c = world.alive(e) ? world.try_get<Camera3DComponent>(e) : nullptr;
    return c && view_of(world, e, *c, alpha, out);
}

bool Kit3DExtract::view_of(World& world, Entity e, const Camera3DComponent& c, f32 alpha, render::Camera3D& out) {
    if (!base_view_of(world, e, c, alpha, out)) return false;
    // M-CINEMA: a blend from another camera's view, then this camera's shake.
    if (const CameraBlend3D* b = world.try_get<CameraBlend3D>(e); b && !(b->from == e) && world.alive(b->from)) {
        const f32 w = camera_blend_weight(*b);
        const Camera3DComponent* fc = world.try_get<Camera3DComponent>(b->from);
        render::Camera3D from;
        if (w < 1.0f && fc && base_view_of(world, b->from, *fc, alpha, from)) {
            out.position = lerp(from.position, out.position, w);
            out.orientation = normalize(slerp(from.orientation, out.orientation, w));
            out.fov_y = lerp(from.fov_y, out.fov_y, w);
        }
    }
    if (const CameraShake3D* s = world.try_get<CameraShake3D>(e); s && s->trauma > 0.0f) {
        // Reduced motion (M-ACCESS) scales the view's shake, never the component.
        const f32 motion = render::accessibility_settings().motion_scale;
        const vec3 r = s->offset_rotation * motion;
        out.position = out.position + rotate(out.orientation, s->offset * motion);
        out.orientation = normalize(out.orientation * quat_from_axis_angle({0.0f, 1.0f, 0.0f}, r.y) *
                                    quat_from_axis_angle({1.0f, 0.0f, 0.0f}, r.x) * quat_from_axis_angle({0.0f, 0.0f, 1.0f}, r.z));
    }
    return true;
}

bool Kit3DExtract::base_view_of(World& world, Entity e, const Camera3DComponent& c, f32 alpha, render::Camera3D& out) {
    vec3 pos, scale;
    quat rot;
    if (!pose_of(world, e, pos, rot, scale)) return false;
    track(e, pos, rot, scale, world.current_tick());
    interpolated(e, alpha, pos, rot, scale);
    out.position = pos;
    out.orientation = normalize(rot);
    out.fov_y = radians(clamp(c.fov_deg, 1.0f, 179.0f));
    out.near_z = max(c.near_z, 1e-4f);
    out.far_z = max(out.near_z * 2.0f, c.far_z);
    return true;
}

void Kit3DExtract::apply_environment(World& world, render::Scene3D& scene) {
    Environment3D env;
    if (const Environment3D* r = world.try_resource<Environment3D>()) {
        env = *r;
    } else {
        bool found = false;
        world.query<const Environment3D&>().each([&](const Environment3D& c) {
            if (found) return;
            found = true;
            env = c;
        });
    }
    {
        static EnvOverrides parsed;
        const StringView want = cv_env_override.get_string();
        if (want != parsed.text.view()) {
            parsed.text.assign(want);
            parse_env_overrides(want, parsed);
        }
        for (u32 i = 0; i < parsed.count; ++i) apply_env_override(env, parsed.items[i]);
    }
    render::DirectionalLight sun;
    sun.direction = env.time_of_day >= 0.0f ? render::sun_direction_at(env.time_of_day) : env.sun_direction;
    sun.color = env.sun_color;
    sun.intensity = env.sun_intensity;
    sun.ambient_sky = env.ambient_sky;
    sun.ambient_ground = env.ambient_ground;
    sun.shadows = env.sun_shadows != 0;
    scene.set_light(sun);
    render::Sky sky;
    sky.zenith = env.sky_zenith;
    sky.horizon = env.sky_horizon;
    sky.ground = env.sky_ground;
    sky.sun_size = env.sky_sun_size;
    sky.sun_intensity = env.sky_sun_intensity;
    sky.texture = texture_key(env.sky_texture.guid);
    sky.texture_intensity = env.sky_texture_intensity;
    sky.texture_additive = env.sky_texture_additive != 0;
    sky.specular_ambient = env.sky_specular_ambient;
    sky.enabled = env.sky_enabled != 0;
    sky.ibl = env.sky_ibl != 0;
    sky.ibl_texture = texture_key(env.ibl_texture.guid);
    sky.ibl_texture_intensity = env.ibl_texture_intensity;
    sky.rotation = env.sky_rotation_deg * (3.14159265358979f / 180.0f);
    scene.set_sky(sky);
    render::Fog fog;
    fog.color = env.fog_color;
    fog.density = env.fog_density;
    scene.set_fog(fog);
    render::Atmosphere atmosphere;
    atmosphere.enabled = env.atmosphere != 0;
    atmosphere.intensity = env.atmosphere_intensity;
    atmosphere.mie = env.atmosphere_haze;
    scene.set_atmosphere(atmosphere);
    render::Volumetrics vol;
    vol.enabled = env.volumetric_fog != 0;
    vol.density = env.volumetric_density;
    vol.height_falloff = env.volumetric_height_falloff;
    vol.anisotropy = env.volumetric_anisotropy;
    vol.albedo = env.volumetric_albedo;
    vol.max_distance = env.volumetric_distance;
    render::apply_quality(quality_, vol);
    scene.set_volumetrics(vol);
    scene.set_exposure(env.exposure * cv_exposure_scale.get_float());
    render::PostFx post;
    post.bloom = env.bloom != 0 && quality_.bloom;
    post.bloom_intensity = env.bloom_intensity;
    post.bloom_threshold = env.bloom_threshold;
    post.fxaa = env.fxaa != 0 && quality_.fxaa;
    post.taa = env.fxaa != 0 && quality_.taa;  // the environment's anti-aliasing switch covers both
    post.auto_exposure.enabled = env.auto_exposure != 0;
    post.auto_exposure.compensation = env.exposure_compensation;
    post.auto_exposure.speed_up = env.auto_exposure_speed_up;
    post.auto_exposure.speed_down = env.auto_exposure_speed_down;
    post.auto_exposure.dt = frame_dt_;
    const u32 lut = texture_key(env.color_grade.guid);
    post.lut_key = lut == ~0u ? 0u : lut;  // texture_key's "none" is ~0u, PostFx's is 0
    post.lut_strength = env.color_grade_strength;
    post.dof.enabled = env.depth_of_field != 0 && quality_.dof_motion_blur;
    post.dof.focus_distance = env.dof_focus_distance;
    post.dof.focus_range = env.dof_focus_range;
    post.dof.blur_range = env.dof_blur_range;
    post.dof.max_radius = env.dof_max_radius;
    post.motion_blur.enabled = env.motion_blur != 0 && quality_.dof_motion_blur;
    post.motion_blur.intensity = env.motion_blur_intensity;
    post.upscaler = env.upscaler != 0 ? render::Upscaler::Spatial : render::Upscaler::Bilinear;
    post.sharpen = env.sharpen;
    post.lens.vignette = env.vignette;
    post.lens.vignette_smoothness = env.vignette_smoothness;
    post.lens.grain = env.film_grain;
    post.lens.grain_size = env.grain_size;
    post.lens.chromatic_aberration = env.chromatic_aberration;
    post.lens.dirt_intensity = env.lens_dirt_intensity;
    const u32 dirt = texture_key(env.lens_dirt.guid);
    post.lens.dirt_key = dirt == ~0u ? 0u : dirt;
    post.tonemapper = env.tonemapper == 1 ? render::Tonemapper::AgX
                    : env.tonemapper == 2 ? render::Tonemapper::PbrNeutral : render::Tonemapper::AcesFit;
    // The primary camera's own focus wins over the environment's.
    bool focused = false;
    world.query<const Camera3DComponent&>().each([&](const Camera3DComponent& c) {
        if (focused || !c.primary) return;
        focused = true;
        if (c.focus_distance > 0.0f) post.dof.focus_distance = c.focus_distance;
    });
    if (cv_dof_focus.get_float() > 0.0f) {
        post.dof.enabled = quality_.dof_motion_blur;
        post.dof.focus_distance = cv_dof_focus.get_float();
    }
    render::apply_accessibility(render::accessibility_settings(), post);  // M-ACCESS: never in the state
    scene.set_post(post);
    render::AmbientOcclusion ao;
    ao.enabled = env.ssao != 0;
    ao.radius = env.ssao_radius;
    ao.intensity = env.ssao_intensity;
    ao.power = env.ssao_power;
    scene.set_ambient_occlusion(ao);
    render::ScreenSpaceReflections ssr;
    ssr.enabled = env.ssr != 0;
    ssr.max_distance = env.ssr_max_distance;
    ssr.thickness = env.ssr_thickness;
    ssr.max_roughness = env.ssr_max_roughness;
    ssr.roughness_fade = env.ssr_roughness_fade;
    ssr.intensity = env.ssr_intensity;
    ssr.steps = env.ssr_steps;
    ssr.edge_fade = env.ssr_edge_fade;
    render::apply_quality(quality_, ssr);
    scene.set_reflections(ssr);
    render::ContactShadows contact;
    contact.enabled = env.contact_shadows != 0;
    contact.length = env.contact_length;
    contact.thickness = env.contact_thickness;
    contact.intensity = env.contact_intensity;
    contact.steps = env.contact_steps;
    render::apply_quality(quality_, contact);
    scene.set_contact_shadows(contact);
    render::ParallaxSettings parallax;
    render::apply_quality(quality_, parallax);
    scene.set_parallax(parallax);
    render::ShadowSettings shadows;
    shadows.cascades = clamp(env.shadow_cascades, 1u, min(render::kMaxCascades, quality_.shadow_cascades));
    shadows.resolution = quality_.shadow_size;
    shadows.distance = env.shadow_distance;
    shadows.splits[0] = env.shadow_splits.x;
    shadows.splits[1] = env.shadow_splits.y;
    shadows.splits[2] = env.shadow_splits.z;
    shadows.splits[3] = env.shadow_splits.w;
    shadows.blend = env.shadow_blend;
    scene.set_shadows(shadows);
}

// ---- drawables ----------------------------------------------------------------------------------

void Kit3DExtract::extract(World& world, render::Scene3D& scene, f32 alpha) {
    sync_origin(world);
    if (!extra_view_) {
        scene.set_origin_shift(origin_pending_);
        origin_pending_ = {};
    }
    stats_ = {};
    probe_entities_.clear();
    gi_entity_ = {};
    apply_environment(world, scene);
    const render::View3D& view = scene.view();
    const u64 tick = world.current_tick();
    // Poses go to poses_ (the scene keeps spans into it until the next extract); offsets are
    // turned into spans once every MeshRef is in, as poses_ may grow meanwhile.
    poses_.clear();
    pending_.clear();
    pending_entity_.clear();
    pose_first_.clear();
    // TAA velocity: this frame's world matrices go to worlds_[parity]; last frame's are in the other.
    if (!extra_view_) {
        world_parity_ ^= 1u;
        ++frame_stamp_;
    }

    // The irradiance volume first: while it captures at runtime, meshes and lights near it stay in the
    // scene off-screen too, so its probes see the same room whichever way the camera looks.
    bool gi_keep = false;
    aabb3 gi_reach{};
    world.query<const GiVolume3D&>().each([&](Entity e, const GiVolume3D& g) {
        if (!g.enabled || scene.gi_volume().enabled) return;
        vec3 pos, scale;
        quat rot;
        if (!pose_of(world, e, pos, rot, scale)) return;
        render::GiVolume gv;
        gv.enabled = true;
        const vec3 c = pos + rotate(rot, g.offset), h = max(g.half_extents, vec3{0.01f, 0.01f, 0.01f});
        gv.bounds = {c - h, c + h};
        gv.counts[0] = g.probes_x;
        gv.counts[1] = g.probes_y;
        gv.counts[2] = g.probes_z;
        gv.intensity = g.intensity;
        gv.normal_bias = g.normal_bias;
        gv.fade_distance = g.fade_distance;
        gv.hysteresis = g.hysteresis;
        gv.probes_per_frame = g.probes_per_frame;
        gv.version = g.version;
        if (bake_mode_) {
            gv.hysteresis = 0.0f;
            gv.probes_per_frame = render::Forward3D::kGiMaxBatchProbes;
        } else {
            gv.baked = texture_key(g.baked.guid);
            render::apply_quality(quality_, gv);
        }
        scene.set_gi_volume(gv);
        gi_entity_ = e;
        if (gv.updates) {
            const f32 margin = length(h);
            gi_keep = true;
            gi_reach = {gv.bounds.min - vec3{margin, margin, margin}, gv.bounds.max + vec3{margin, margin, margin}};
        }
    });
    auto in_view = [&](vec3 c, f32 r) {
        if (render::sphere_visible(view, c, r)) return true;
        if (!gi_keep) return false;
        const vec3 q = min(max(c, gi_reach.min), gi_reach.max);
        return length_sq(c - q) <= r * r;
    };

    // Screens (M-VIEWS 3): one material per ShowRenderTexture, kept until the next extract; the
    // array never grows past what is reserved here, so the scene's pointers stay put.
    screen_materials_.clear();
    if (render_textures_) {
        usize screens = 0;
        world.query<const ShowRenderTexture&>().each([&](const ShowRenderTexture&) { ++screens; });
        screen_materials_.reserve(screens);
    }
    // StaticMerge3D (M-VISUAL3 5b): merged once per frame, the later views of the frame reuse it.
    if (!extra_view_) update_static_merges(world);
    const Ragdoll3DPoses* ragdolls = world.try_resource<Ragdoll3DPoses>();
    world.query<const MeshRef&>().each([&](Entity e, const MeshRef& m) {
        if (!m.visible) return;
        if (merge_stamp_ != 0 && e.index < merged_stamp_.size() && merged_stamp_[e.index] == merge_stamp_) return;  // drawn merged
        vec3 pos, scale;
        quat rot;
        if (!pose_of(world, e, pos, rot, scale)) return;
        if (m.interpolate) {
            track(e, pos, rot, scale, tick);
            interpolated(e, alpha, pos, rot, scale);
        }
        if (!bake_mode_ && m.cull_radius > 0.0f && !in_view(pos, m.cull_radius * max(scale.x, max(scale.y, scale.z))))
            return;
        const u32 key = mesh_key(m.mesh.guid);
        const render::GpuMesh* mesh = key != ~0u && meshes_ ? meshes_->get(key) : nullptr;
        if (!mesh) {
            ++stats_.missing;
            return;
        }
        render::MeshInstance inst;
        inst.mesh = key;
        inst.world = trs(pos, rot, scale);
        if (mesh->lod_levels > 1 && lod_states_.size() <= e.index) lod_states_.resize(e.index + 1, 0);
        if (world_stamp_.size() <= e.index) {
            world_stamp_.resize(e.index + 1, ~0u);  // never drawn: ~0u + 1 matches no stamp
            prev_ok_.resize(e.index + 1, 0);
            for (Array<mat4>& w : worlds_) w.resize(e.index + 1, mat4::identity());
        }
        inst.tint = m.tint;
        inst.emissive_tint = m.emissive_tint;
        inst.cast_shadows = m.cast_shadows != 0;
        inst.skinning = m.skinning != 0;
        inst.material = material(m.material.guid);
        if (render_textures_ && screen_materials_.size() < screen_materials_.capacity()) {
            const ShowRenderTexture* show = world.try_get<ShowRenderTexture>(e);
            const u32 rt = show ? render_textures_->texture_key(show->source) : ~0u;
            if (rt != ~0u && !mesh->submeshes.empty()) {
                render::MaterialOverride o;
                if (inst.material) {
                    o = *inst.material;
                } else {
                    const render::GpuSubmesh& s0 = mesh->submeshes[0];
                    o.params = s0.material;
                    o.ext = s0.material_ext;
                    o.albedo_key = s0.albedo_key;
                    o.normal_key = s0.normal_key;
                    o.orm_key = s0.orm_key;
                    o.emissive_key = s0.emissive_key;
                }
                if (show->slot == 1) {
                    o.emissive_key = rt;
                    for (f32& c : o.params.emissive) c = show->intensity;
                } else {
                    o.albedo_key = rt;
                }
                screen_materials_.push_back(o);
                inst.material = &screen_materials_.back();
            }
        }
        if (!mesh->nodes.empty()) {
            const Animator* a = world.try_get<Animator>(e);
            const usize first = poses_.size();
            poses_.resize(first + mesh->nodes.size());
            const Span<mat4> pose(poses_.data() + first, mesh->nodes.size());
            bool posed = a && extended_pose(world, e, *mesh, *a, pose);
            if (!posed && a && a->playing != 0) {
                render::ClipSample samples[3];
                const usize n = anim_samples(*a, *mesh, [&](u64 id) { return find_clip_id(*mesh, id); }, samples);
                if (n > 0) pose_samples(*mesh, samples, n, pose);
                posed = n > 0;
            }
            const bool ragdoll = ragdolls && ragdolls->poses.find(e);
            if (!posed && !ragdoll) {
                poses_.resize(first);
            } else {
                if (!posed)
                    for (usize i = 0; i < pose.size(); ++i) pose[i] = mesh->rest_pose[i];
                if (ragdoll) apply_ragdoll_pose(world, e, pose);
                pose_first_.push_back(static_cast<u32>(first));
                pose_first_.push_back(static_cast<u32>(mesh->nodes.size()));
                pose_first_.push_back(static_cast<u32>(pending_.size()));
                ++stats_.animated;
            }
        }
        pending_.push_back(inst);
        pending_entity_.push_back(e.index);
        ++stats_.meshes;
    });
    for (usize i = 0; i + 2 < pose_first_.size(); i += 3)
        pending_[pose_first_[i + 2]].pose = Span<const mat4>(poses_.data() + pose_first_[i], pose_first_[i + 1]);
    for (usize i = 0; i < pending_.size(); ++i) {
        // The level the scene picked last frame (hysteresis), now that lod_states_ no longer grows.
        render::MeshInstance& inst = pending_[i];
        const u32 ent = pending_entity_[i];
        if (meshes_->get(inst.mesh)->lod_levels > 1) inst.lod_state = &lod_states_[ent];
        // Drawn last frame too: its world then (the entity index may have been reused; the TAA
        // neighborhood clip absorbs that one frame).
        // Another view drew it this frame already (extract_view): that draw knew whether last frame's was there.
        const bool prev = world_stamp_[ent] == frame_stamp_ ? prev_ok_[ent] != 0 : world_stamp_[ent] + 1 == frame_stamp_;
        if (prev) inst.prev_world = &worlds_[world_parity_ ^ 1u][ent];
        worlds_[world_parity_][ent] = inst.world;
        world_stamp_[ent] = frame_stamp_;
        prev_ok_[ent] = prev ? 1 : 0;
        scene.add(inst);
    }
    for (const MergedDraw& d : merged_draws_) {
        if (!bake_mode_ && !in_view(d.center, d.radius)) continue;
        render::MeshInstance inst;
        inst.mesh = d.key;
        inst.world = translation(-d.shift);
        inst.cast_shadows = d.cast_shadows;
        inst.lod_state = d.lod_state;
        scene.add(inst);
    }
    extract_terrains(world, scene);

    world.query<const PointLight3D&>().each([&](Entity e, const PointLight3D& l) {
        if (!l.enabled || l.intensity <= 0.0f) return;
        vec3 pos, scale;
        quat rot;
        if (!pose_of(world, e, pos, rot, scale)) return;
        render::PointLight pl;
        pl.position = pos + rotate(rot, l.offset);
        if (!in_view(pl.position, l.radius)) return;
        pl.color = {l.color.r, l.color.g, l.color.b};
        pl.intensity = l.intensity;
        pl.radius = l.radius;
        pl.cast_shadows = l.cast_shadows != 0;
        scene.add_point_light(pl);
        ++stats_.point_lights;
    });
    world.query<const SpotLight3D&>().each([&](Entity e, const SpotLight3D& l) {
        if (!l.enabled || l.intensity <= 0.0f) return;
        vec3 pos, scale;
        quat rot;
        if (!pose_of(world, e, pos, rot, scale)) return;
        render::PointLight pl;
        pl.position = pos + rotate(rot, l.offset);
        if (!in_view(pl.position, l.radius)) return;
        pl.color = {l.color.r, l.color.g, l.color.b};
        pl.intensity = l.intensity;
        pl.radius = l.radius;
        const vec3 d = rotate(rot, l.direction);
        pl.direction = length_sq(d) > 0.0f ? normalize(d) : vec3{0.0f, -1.0f, 0.0f};
        pl.spot_outer = radians(clamp(l.outer_deg, 0.5f, 89.0f));
        pl.spot_inner = radians(clamp(l.inner_deg, 0.0f, l.outer_deg));
        pl.cast_shadows = l.cast_shadows != 0;
        scene.add_point_light(pl);
        ++stats_.spot_lights;
    });
    world.query<const ReflectionProbe3D&>().each([&](Entity e, const ReflectionProbe3D& p) {
        if (!p.enabled) return;
        vec3 pos, scale;
        quat rot;
        if (!pose_of(world, e, pos, rot, scale)) return;
        render::ReflectionProbe rp;
        rp.position = pos + rotate(rot, p.offset);
        const vec3 h = max(p.half_extents, vec3{0.01f, 0.01f, 0.01f});
        rp.bounds = {pos - h, pos + h};
        rp.blend_distance = p.blend_distance;
        rp.intensity = p.intensity;
        rp.version = p.version;
        rp.baked = bake_mode_ ? ~0u : texture_key(p.baked.guid);
        const usize before = scene.reflection_probes().size();
        scene.add_reflection_probe(rp);
        if (scene.reflection_probes().size() > before) probe_entities_.push_back(e);
        ++stats_.reflection_probes;
    });

    // Scale (M-VISUAL 9): foliage batches, decals, GPU particles on the scene's clock.
    if (!bake_mode_ && !extra_view_) time_ += max(frame_dt_, 0.0f);
    scene.set_time(time_, bake_mode_ ? 0.0f : max(frame_dt_, 0.0f));
    world.query<const Foliage3D&>().each([&](Entity e, const Foliage3D& f) {
        if (!f.enabled || f.count == 0) return;
        vec3 pos, scale;
        quat rot;
        if (!pose_of(world, e, pos, rot, scale)) return;
        const u32 key = mesh_key(f.mesh.guid);
        const render::GpuMesh* mesh = key != ~0u && meshes_ ? meshes_->get(key) : nullptr;
        if (!mesh) {
            ++stats_.missing;
            return;
        }
        // The scatter's inputs: a change re-scatters and bumps the batch's version (re-upload).
        u64 h = 1469598103934665603ull;
        auto mix = [&h](const void* p, usize n) {
            for (usize i = 0; i < n; ++i) h = (h ^ static_cast<const u8*>(p)[i]) * 1099511628211ull;
        };
        mix(&pos, sizeof(pos));
        mix(&f.half_extents, sizeof(f.half_extents));
        mix(&f.count, sizeof(f.count));
        mix(&f.seed, sizeof(f.seed));
        mix(&f.scale_min, sizeof(f.scale_min));
        mix(&f.scale_max, sizeof(f.scale_max));
        mix(&f.random_yaw, sizeof(f.random_yaw));
        FoliageEntry* entry = nullptr;
        for (FoliageEntry& fe : foliage_)
            if (fe.entity == e) entry = &fe;
        if (!entry) {
            foliage_.push_back({});
            entry = &foliage_.back();
            entry->entity = e;
        }
        if (entry->rebased) {  // moved with the world: the inputs changed only by the shift
            entry->key = h;
            entry->rebased = false;
        }
        if (entry->transforms.empty() || entry->key != h) {
            render::FoliageScatter sc;
            sc.origin = pos;
            sc.half_extents = f.half_extents;
            sc.count = f.count;
            sc.seed = f.seed;
            sc.scale_min = f.scale_min;
            sc.scale_max = f.scale_max;
            sc.random_yaw = f.random_yaw != 0;
            render::scatter_foliage(sc, entry->transforms);
            entry->key = h;
            ++entry->version;
        }
        entry->stamp = frame_stamp_;
        render::InstanceBatch b;
        b.id = e.index + 1;
        b.version = entry->version;
        b.mesh = key;
        b.transforms = entry->transforms.span();
        b.cast_shadows = f.cast_shadows != 0;
        b.cull_distance = bake_mode_ ? 0.0f : f.cull_distance;
        b.tint = f.tint;
        b.material = material(f.material.guid);
        b.wind_amplitude = f.wind_amplitude;
        b.wind_frequency = f.wind_frequency;
        b.wind_direction = f.wind_direction;
        scene.add_batch(b);
        ++stats_.foliage;
        stats_.foliage_instances += static_cast<u32>(entry->transforms.size());
    });
    // Foliage on a terrain (M-TERRAIN 5): re-scattered when the terrain's pose, its cooked generation
    // (a brush stroke re-bakes it) or the scatter's inputs change.
    world.query<const TerrainFoliage3D&>().each([&](Entity e, const TerrainFoliage3D& f) {
        if (!f.enabled || !assets_ || f.density <= 0.0f || f.max_count == 0) return;
        Entity host = e;
        const Terrain3D* terrain = world.try_get<Terrain3D>(e);
        if (!terrain) {
            const ChildOf* parent = world.try_get<ChildOf>(e);
            host = parent ? parent->parent : Entity{};
            terrain = parent && world.alive(host) ? world.try_get<Terrain3D>(host) : nullptr;
        }
        if (!terrain || !terrain->enabled) return;
        const asset::AssetKey tkey = assets_->find(terrain->terrain.guid);
        if (tkey == asset::kInvalidAssetKey) return;
        vec3 pos, scale;
        quat rot;
        if (!pose_of(world, host, pos, rot, scale)) return;
        const u32 key = mesh_key(f.mesh.guid);
        const render::GpuMesh* mesh = key != ~0u && meshes_ ? meshes_->get(key) : nullptr;
        if (!mesh) {
            ++stats_.missing;
            return;
        }
        u64 h = 1469598103934665603ull;
        auto mix = [&h](const void* p, usize n) {
            for (usize i = 0; i < n; ++i) h = (h ^ static_cast<const u8*>(p)[i]) * 1099511628211ull;
        };
        const u32 generation = assets_->generation(tkey);
        mix(&pos, sizeof(pos));
        mix(&rot, sizeof(rot));
        mix(&scale, sizeof(scale));
        mix(&terrain->terrain.guid, sizeof(terrain->terrain.guid));
        mix(&generation, sizeof(generation));
        mix(&f.layer, sizeof(f.layer));
        mix(&f.density, sizeof(f.density));
        mix(&f.max_count, sizeof(f.max_count));
        mix(&f.seed, sizeof(f.seed));
        mix(&f.scale_min, sizeof(f.scale_min));
        mix(&f.scale_max, sizeof(f.scale_max));
        mix(&f.max_slope_deg, sizeof(f.max_slope_deg));
        mix(&f.random_yaw, sizeof(f.random_yaw));
        FoliageEntry* entry = nullptr;
        for (FoliageEntry& fe : terrain_foliage_)
            if (fe.entity == e) entry = &fe;
        if (!entry) {
            terrain_foliage_.push_back({});
            entry = &terrain_foliage_.back();
            entry->entity = e;
            entry->key = ~h;
        }
        if (entry->rebased) {
            entry->key = h;
            entry->rebased = false;
        }
        if (entry->key != h) {
            entry->transforms.clear();
            if (Result<asset::AssetHandle<asset::Terrain>> th = assets_->load_sync<asset::Terrain>(tkey)) {
                if (const Result<asset::TerrainView> tview = asset::TerrainView::read(assets_->resolve(th.value()))) {
                    render::TerrainScatter sc;
                    sc.layer = f.layer;
                    sc.density = f.density;
                    sc.max_count = f.max_count;
                    sc.seed = f.seed;
                    sc.scale_min = f.scale_min;
                    sc.scale_max = f.scale_max;
                    sc.max_slope_deg = f.max_slope_deg;
                    sc.random_yaw = f.random_yaw != 0;
                    sc.position = pos;
                    sc.rotation = rot;
                    sc.scale = scale;
                    render::scatter_on_terrain(tview.value(), sc, entry->transforms);
                }
                assets_->release(asset::AnyHandle(th.value()));
            }
            entry->key = h;
            ++entry->version;
        }
        entry->stamp = frame_stamp_;
        if (entry->transforms.empty()) return;
        render::InstanceBatch b;
        b.id = (e.index + 1) | 0x80000000u;  // apart from the entity's Foliage3D batch
        b.version = entry->version;
        b.mesh = key;
        b.transforms = entry->transforms.span();
        b.cast_shadows = f.cast_shadows != 0;
        b.cull_distance = bake_mode_ ? 0.0f : f.cull_distance;
        b.tint = f.tint;
        b.material = material(f.material.guid);
        b.wind_amplitude = f.wind_amplitude;
        b.wind_frequency = f.wind_frequency;
        b.wind_direction = f.wind_direction;
        scene.add_batch(b);
        ++stats_.foliage;
        stats_.foliage_instances += static_cast<u32>(entry->transforms.size());
    });
    for (usize i = 0; i < foliage_.size();) {  // gone, disabled or destroyed: its scatter is dropped
        if (foliage_[i].stamp == frame_stamp_) {
            ++i;
            continue;
        }
        foliage_[i] = std::move(foliage_.back());
        foliage_.pop_back();
    }
    for (usize i = 0; i < terrain_foliage_.size();) {
        if (terrain_foliage_[i].stamp == frame_stamp_) {
            ++i;
            continue;
        }
        terrain_foliage_[i] = std::move(terrain_foliage_.back());
        terrain_foliage_.pop_back();
    }
    world.query<const Decal3D&>().each([&](Entity e, const Decal3D& d) {
        if (!d.enabled) return;
        vec3 pos, scale;
        quat rot;
        if (!pose_of(world, e, pos, rot, scale)) return;
        render::Decal rd;
        rd.world = trs(pos, rot, scale) * scaling(max(d.size, vec3{0.001f, 0.001f, 0.001f}));
        rd.texture = texture_key(d.texture.guid);
        rd.uv_rect = d.uv_rect;
        rd.color = d.color;
        rd.roughness = d.roughness;
        rd.metallic = d.metallic;
        rd.material_weight = d.material_weight;
        rd.emissive = d.emissive;
        rd.angle_fade = d.angle_fade;
        rd.edge_fade = d.edge_fade;
        scene.add_decal(rd);
        ++stats_.decals;
    });
    world.query<const ParticleEmitter3D&>().each([&](Entity e, const ParticleEmitter3D& p) {
        vec3 pos, scale;
        quat rot;
        if (!pose_of(world, e, pos, rot, scale)) return;
        BurstEntry* entry = nullptr;
        for (BurstEntry& be : bursts_)
            if (be.entity == e) entry = &be;
        const bool first = entry == nullptr;
        if (first) {
            bursts_.push_back({});
            entry = &bursts_.back();
            entry->entity = e;
        }
        const bool burst = first || entry->burst_version != p.burst_version;
        entry->burst_version = p.burst_version;
        entry->stamp = frame_stamp_;
        render::ParticleEmitter em;
        em.id = e.index + 1;
        em.max_particles = p.max_particles;
        em.seed = p.seed;
        em.position = pos + rotate(rot, p.offset);
        em.box = p.box;
        const vec3 dir = rotate(rot, p.direction);
        em.direction = length_sq(dir) > 0.0f ? normalize(dir) : vec3{0.0f, 1.0f, 0.0f};
        em.spread = radians(clamp(p.spread_deg, 0.0f, 180.0f));
        em.rate = p.rate;
        em.burst = burst ? p.burst : 0u;
        em.emitting = p.emitting != 0;
        em.speed_min = p.speed_min;
        em.speed_max = p.speed_max;
        em.lifetime_min = p.lifetime_min;
        em.lifetime_max = p.lifetime_max;
        em.gravity = p.gravity;
        em.drag = p.drag;
        em.collide = p.collide != 0;
        em.ground_height = p.ground_height;
        em.bounce = p.bounce;
        em.size_start = p.size_start;
        em.size_end = p.size_end;
        em.color_start = p.color_start;
        em.color_end = p.color_end;
        em.additive = p.additive != 0;
        em.texture = texture_key(p.texture.guid);
        scene.add_particle_emitter(em);
        ++stats_.particle_emitters;
    });
    for (usize i = 0; i < bursts_.size();) {
        if (bursts_[i].stamp == frame_stamp_) {
            ++i;
            continue;
        }
        bursts_[i] = bursts_.back();
        bursts_.pop_back();
    }

    // Static copies, cell by cell.
    if (const StaticInstances3D* si = world.try_resource<StaticInstances3D>(); si && !si->instances.empty()) {
        Array<u32>& keys = static_keys_;
        if (static_lods_.size() != si->instances.size()) static_lods_.resize(si->instances.size(), 0);
        keys.resize(si->meshes.size());
        for (u32 i = 0; i < si->meshes.size(); ++i) keys[i] = mesh_key(si->meshes[i]);
        for (const StaticCell3D& c : si->cells) {
            if (!bake_mode_ && !in_view(c.center, c.radius)) continue;
            ++stats_.cells_visible;
            const f32 dist = length(c.center - view.position);
            for (u32 i = c.first; i < c.first + c.count && i < si->instances.size(); ++i) {
                const StaticInstance3D& s = si->instances[i];
                if (s.mesh >= si->meshes.size() || keys[s.mesh] == ~0u) continue;
                if (!bake_mode_ && s.max_distance > 0.0f && dist > s.max_distance) continue;
                if (!bake_mode_ && s.thin_distance > 0.0f && dist > s.thin_distance && (i & 1u)) continue;
                render::MeshInstance inst;
                inst.mesh = keys[s.mesh];
                inst.world = trs(s.position, s.rotation, {s.scale, s.scale, s.scale});
                inst.cast_shadows = s.cast_shadows != 0;
                inst.lod_state = &static_lods_[i];
                scene.add(inst);
                ++stats_.instances;
            }
        }
    }
}

}  // namespace mce::kit
