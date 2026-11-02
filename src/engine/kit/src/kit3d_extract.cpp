#include <algorithm>
#include <atomic>
#include "mce/kit/kit3d_extract.h"

#include <cmath>
#include <cstdlib>
#include <cstring>

#include "mce/asset/formats.h"
#include "mce/ecs/hierarchy.h"
#include "mce/ecs/origin.h"
#include "mce/ecs/transform.h"
#include "mce/kit/chunk_lod3d.h"
#include "mce/kit/firefly.h"
#include "mce/kit/leaf_fall.h"
#include "mce/kit/dust.h"
#include "mce/kit/flock.h"
#include "mce/kit/butterfly.h"
#include "mce/kit/dusk_bat.h"
#include "mce/kit/pollen.h"
#include "mce/kit/precipitation.h"
#include "mce/kit/aurora.h"
#include "mce/kit/cinematic.h"
#include "mce/kit/meteor.h"
#include "mce/kit/rainbow.h"
#include "mce/kit/season.h"
#include "mce/kit/render_textures.h"
#include "mce/kit/cloud_layer.h"
#include "mce/kit/day_cycle.h"
#include "mce/kit/ground_mist.h"
#include "mce/kit/moon.h"
#include "mce/kit/world_clock.h"
#include "mce/kit/rain_ripples.h"
#include "mce/kit/snow_cover.h"
#include "mce/kit/weather.h"
#include "mce/kit/water.h"
#include "mce/kit/water_ice.h"
#include "mce/kit/wet_surfaces.h"
#include "mce/kit/wind_direction.h"
#include "mce/kit/wind_gust.h"
#include "mce/kit/wind_sway.h"
#include "mce/convar.h"
#include "mce/log.h"
#include "mce/refl/property_path.h"
#include "mce/render/accessibility.h"
#include "mce/scene/streaming.h"
#include "mce/string_id.h"
#include "kit3d_internal.h"

namespace mce::kit {

using namespace mce::math;

namespace {

// Grows a per-entity array to `n`, by at least half its capacity: resizing to e.index + 1 for each new entity copied
// the whole array every time (24k entities arriving at once: seconds).
template <class T>
void grow_to(Array<T>& a, usize n, const T& fill = T{}) {
    const usize c = a.capacity() + a.capacity() / 2;
    if (n > a.capacity()) a.reserve(n > c ? n : c);
    a.resize(n, fill);
}


// Foliage tint variation and wind shaping on a batch (M-VISUAL3 6): the fields as the renderer reads them.
template <class F>
void foliage_look(const F& f, render::InstanceBatch& b) {
    b.tint_variation = f.tint_variation;
    b.wind_ext = {max(f.wind_height, 0.0f), clamp(f.wind_gust, 0.0f, 1.0f), max(f.wind_flutter, 0.0f), 0.0f};
}

// Wind sway (M-WINDSWAY 1): the batch's wind fields scaled by Weather.wind. Batch fields only, after the cached
// transforms, so a wind change never re-scatters (entry->version is untouched). Called only when both exist.
void apply_wind_sway(f32 wind, const WindSway& s, render::InstanceBatch& b) {
    b.wind_amplitude *= 1.0f + (s.amplitude_gain - 1.0f) * wind;
    b.wind_frequency *= 1.0f + (s.frequency_gain - 1.0f) * wind;
    b.wind_ext.y = clamp(b.wind_ext.y + s.gust_gain * wind, 0.0f, 1.0f);
    b.wind_ext.z *= 1.0f + (s.flutter_gain - 1.0f) * wind;
}

// Season (M-SEASONS 19b): the first Season at the first WorldClock's day and hour (day 0, hour 0 with no clock). A flag
// is false for no Season, enabled 0, strength 0 and the pure-summer case where the helpers return exact identities, so
// every consumer skips its code path and frames stay bit-identical. Render-side copies only; nothing is written back.
// Density thinning is not applied: see docs/milestones/M-SEASONS.md (the terrain scatter is row-major, so a prefix of
// the cached copies is not an unbiased thinning).
struct SeasonFrame {
    bool tint_on = false;
    bool snow_on = false;
    math::vec3 tint{1.0f, 1.0f, 1.0f};
    SeasonSnow snow;
};

SeasonFrame read_season(World& world) {
    SeasonFrame out;
    const Season* season = nullptr;
    world.query<const Season&>().each([&](const Season& c) {
        if (!season) season = &c;
    });
    if (!season) return out;
    u32 day = 0;
    f32 hour = 0.0f;
    bool have_clock = false;
    world.query<const WorldClock&>().each([&](const WorldClock& c) {
        if (have_clock) return;
        have_clock = true;
        day = c.day;
        hour = c.hour;
    });
    const f32 phase = season_phase(*season, day, hour);
    out.tint = season_foliage_tint(*season, phase);
    out.tint_on = out.tint.x != 1.0f || out.tint.y != 1.0f || out.tint.z != 1.0f;
    out.snow = season_snow_bias(*season, phase);
    out.snow_on = out.snow.cover_floor > 0.0f || out.snow.lee_shift > 0.0f;
    return out;
}

// Leaf fall (M-LEAFFALL 21b): the id of the one render-side emitter the extract adds. ParticleEmitter3D emitters use
// `e.index + 1`, and an entity index is a row of the world's location table, so it stays far below 0xFFFF0001 (that would
// be 4.29 billion live entities, 64 GB of locations alone); the reserved value cannot collide with one. The foliage batch
// ids live in Forward3D's separate batch id space (batch_gpu_), not the emitter one (emitters_gpu_). A test pins it.
constexpr u32 kLeafFallEmitterId = 0xFFFF0001u;

// Fireflies (M-FIREFLIES 23b): the id of the second render-side emitter (same collision argument as the leaf id).
constexpr u32 kFireflyEmitterId = 0xFFFF0002u;

// Pollen (M-POLLEN 24b): the id of the third render-side emitter (same collision argument as the leaf id).
constexpr u32 kPollenEmitterId = 0xFFFF0003u;

// Wind-blown dust (M-DUSTWIND 25b): the id of the fourth render-side emitter (same collision argument as the leaf id).
constexpr u32 kDustEmitterId = 0xFFFF0004u;
// Bird flocks (M-BIRDFLOCK 26b): the id of the fifth render-side emitter (same collision argument as the leaf id).
constexpr u32 kFlockEmitterId = 0xFFFF0005u;
// Summer butterflies (M-BUTTERFLY 27b): the id of the sixth render-side emitter (same collision argument as the leaf id).
constexpr u32 kButterflyEmitterId = 0xFFFF0006u;
// Dusk bats (M-BAT 30b): the id of the seventh render-side emitter (same collision argument as the leaf id).
constexpr u32 kDuskBatEmitterId = 0xFFFF0007u;

// M-PARTICLEQ 28b: the share of rate and budget a render-side weather emitter keeps (QualitySettings::particle_scale),
// clamped to [0, 1]; NaN reads as 1.
f32 particle_scale_of(const render::QualitySettings& q) {
    const f32 s = q.particle_scale;
    if (!(s == s)) return 1.0f;
    return std::clamp(s, 0.0f, 1.0f);
}

// The emitter budget under a particle scale: unchanged at 1, else floor(budget * scale) and at least 1 (0.5 halves).
u32 scaled_budget(u32 budget, f32 scale) {
    if (scale >= 1.0f) return budget;
    return std::max(static_cast<u32>(static_cast<f32>(budget) * scale), 1u);
}

// Water ice (M-FREEZE 20b): the first WaterIce at the first Season's phase (day 0, hour 0 with no WorldClock; no Season is
// the manual floor only). Edits the local render::WaterSurface only, before apply_quality; nothing is written back. Not
// called when the amount is 0, so the carrier stays bit-identical.
void apply_water_ice(const WaterIce& ice, const WaterIceLook& look, render::WaterSurface& s) {
    s.amplitude *= look.amplitude_scale;
    s.steepness *= look.steepness_scale;
    s.speed_scale *= look.speed_scale;
    const math::vec3 c = water_ice_color(ice);
    const f32 m = look.ice_mix;
    s.shallow_color = math::vec3{s.shallow_color.x + (c.x - s.shallow_color.x) * m, s.shallow_color.y + (c.y - s.shallow_color.y) * m,
                                 s.shallow_color.z + (c.z - s.shallow_color.z) * m};
    s.deep_color = math::vec3{s.deep_color.x + (c.x - s.deep_color.x) * m, s.deep_color.y + (c.y - s.deep_color.y) * m,
                              s.deep_color.z + (c.z - s.deep_color.z) * m};
    s.depth_scale_m += (look.depth_scale_m - s.depth_scale_m) * m;
    s.roughness = std::max(s.roughness, look.roughness_floor);
    s.reflectivity *= look.reflectivity_scale;
    s.foam_amount *= look.foam_scale;
}

// The season tint on a batch, after foliage_look and apply_wind_sway: rgb only, alpha and tint_variation untouched.
void apply_season_tint(const math::vec3& t, render::InstanceBatch& b) {
    b.tint.x *= t.x;
    b.tint.y *= t.y;
    b.tint.z *= t.z;
}

// Snow lee bias under a gust (M-WINDGUST 12b): clamp(bias * (1 + strength * gust_q / 2^24), 0, 1) with the snow bit; with
// no gust, `enabled` 0, the bit clear or a zero level it returns `bias` itself (the same f32). Same clamps as
// effective_wind (NaN strength counts as the default 0.6).
f32 gusted_snow_bias(f32 bias, const WindGust* gust) {
    if (!gust || !gust->enabled || (gust->affects & kGustSnow) == 0 || gust->gust_q == 0) return bias;
    const f64 strength = std::isnan(gust->strength) ? 0.6 : std::clamp(static_cast<f64>(gust->strength), 0.0, 3.0);
    const f64 level = static_cast<f64>(std::min<u32>(gust->gust_q, 1u << 24)) / 16777216.0;
    const f64 v = static_cast<f64>(bias) * (1.0 + strength * level);
    if (std::isnan(v)) return bias;
    return static_cast<f32>(std::clamp(v, 0.0, 1.0));
}

// Wind direction (M-WINDDIR 8d): the batch's authored compass rotated by the scene wind's angle relative to +x, so a
// batch authored along +x follows the scene wind and one authored along another axis keeps its offset. `wind` is the
// unit vector (cos a, sin a) from wind_direction_vector. Batch field only, after the cached transforms (the scatter is
// never touched). Called only when both a WindDirection and a Weather exist.
void apply_wind_direction(const math::vec2& wind, render::InstanceBatch& b) {
    const math::vec2 d = b.wind_direction;
    b.wind_direction = math::vec2{d.x * wind.x - d.y * wind.y, d.x * wind.y + d.y * wind.x};
}

// The kind (0..3) a scattered copy belongs to: a hash of its origin and the seed against the cumulative weights.
// Integer arithmetic only, so the split is the same on every machine and in every run.
u32 foliage_kind(const math::mat4& m, u32 seed, const f32 cumulative[4], u32 live_mask) {
    u32 x, y, z;
    std::memcpy(&x, &m.c[3][0], 4);
    std::memcpy(&y, &m.c[3][1], 4);
    std::memcpy(&z, &m.c[3][2], 4);
    u32 h = seed * 0x9e3779b9u + 0x7f4a7c15u;
    for (const u32 v : {x, y, z}) {
        h ^= v;
        h *= 0x85ebca6bu;
        h ^= h >> 13;
        h *= 0xc2b2ae35u;
        h ^= h >> 16;
    }
    const f32 pick = static_cast<f32>(h >> 8) * (1.0f / 16777216.0f) * cumulative[3];
    for (u32 k = 0; k < 3; ++k)
        if (pick < cumulative[k]) return k;
    for (u32 k = 3; k > 0; --k)  // rounding put the pick past the sum: the last kind that has copies
        if (live_mask >> k & 1u) return k;
    return 0;
}


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

// The environment the frame uses: the Environment3D resource, else the first component, else defaults, then the r.env
// overrides. Shared by apply_environment and the firefly feed (which needs the same time of day).
Environment3D resolve_environment(World& world) {
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
    return env;
}

}  // namespace

void Kit3DExtract::init(const render::MeshStore& meshes, asset::AssetSystem* assets) {
    release_bolt_mesh();
    meshes_ = &meshes;
    writable_ = nullptr;
    assets_ = assets;
}

void Kit3DExtract::init(render::MeshStore& meshes, asset::AssetSystem* assets) {
    release_bolt_mesh();
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
    release_bolt_mesh();
    release_static_merges();
    release_terrains();
    release_handles();
    meshes_ = nullptr;
    writable_ = nullptr;
    textures_ = nullptr;
    assets_ = nullptr;
}

void Kit3DExtract::note_built(const render::Scene3D& built) {
    stats_.decals_dropped = built.stats().decals_dropped;
    if (stats_.decals_dropped == 0 || decal_warnings_ > 0) return;
    ++decal_warnings_;
    MCE_LOG_WARN("kit3d: {} visible Decal3D(s) dropped: a frame draws at most {} decals from {} distinct textures. Pack the "
                 "decal textures into one atlas and give each decal its uv_rect, or reduce the visible count",
                 stats_.decals_dropped, render::kMaxDecals, render::kMaxDecalTextures);
}

void Kit3DExtract::reset(asset::AssetSystem* assets) {
    release_bolt_mesh();
    release_static_merges();
    release_terrains();
    release_handles();
    foliage_.clear();
    terrain_foliage_.clear();
    bursts_.clear();
    decal_warnings_ = 0;
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
    if (e.index >= tracked_.size()) grow_to(tracked_, e.index + 1);
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
            const auto shift = [&](Array<mat4>& list_of) {
                for (mat4& m : list_of) {
                    m.c[3][0] -= d.x;
                    m.c[3][1] -= d.y;
                    m.c[3][2] -= d.z;
                }
            };
            shift(f.transforms);
            for (Array<mat4>& more : f.more) shift(more);
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
    const Environment3D env = resolve_environment(world);
    render::DirectionalLight sun;
    sun.direction = env.time_of_day >= 0.0f ? render::sun_direction_at(env.time_of_day) : env.sun_direction;
    sun.color = env.sun_color;
    sun.intensity = env.sun_intensity;
    sun.ambient_sky = env.ambient_sky;
    sun.ambient_ground = env.ambient_ground;
    sun.shadows = env.sun_shadows != 0;
    const Moon* moon = nullptr;  // the first Moon (14b light hand-off, 14e night sky)
    u32 moon_day = 0;
    bool moon_holds_light = false;
    const f32 sun_elevation_natural = -sun.direction.y;  // the sun's own elevation, before any hand-off
    const render::DirectionalLight sun_natural = sun;     // the sun itself: what the atmosphere's sky follows
    {  // moonlight (M-NIGHTSKY 14b): hand the one directional slot to the moon at night; local copy only, never Environment3D
        world.query<const Moon&>().each([&](const Moon& c) {
            if (!moon) moon = &c;
        });
        if (moon && moon->enabled && env.time_of_day >= 0.0f) {
            const Weather* moon_weather = nullptr;
            world.query<const Weather&>().each([&](const Weather& c) {
                if (!moon_weather) moon_weather = &c;
            });
            u32& day = moon_day;
            bool have_clock = false;
            world.query<const WorldClock&>().each([&](const WorldClock& c) {
                if (have_clock) return;
                have_clock = true;
                day = c.day;
            });
            const f32 level = moon_level(*moon, day, env.time_of_day, moon_weather);
            if (level > 0.0f) {
                const f32 sun_elevation = -sun.direction.y;
                const f32 t = clamp((sun_elevation + 0.05f) / 0.15f, 0.0f, 1.0f);
                const f32 sw = t * t * (3.0f - 2.0f * t);
                const f32 mw = (1.0f - sw) * level;
                const f32 moon_intensity = clamp(moon->light_intensity, 0.0f, 2.0f);
                if (mw > 0.0f && sw * sun.intensity < mw * moon_intensity) {
                    const f32 phase = moon_phase(*moon, day, env.time_of_day);
                    sun.direction = moon_direction(*moon, env.time_of_day, phase);
                    sun.color = moon->light_color;
                    sun.intensity = moon_intensity * mw;
                    sun.shadows = moon->light_shadows != 0;
                    moon_holds_light = true;
                    const f32 lift = mw * 0.02f;  // a small cool ambient lift so shadowed ground is not pure black
                    sun.ambient_sky = sun.ambient_sky + moon->light_color * lift;
                    sun.ambient_ground = sun.ambient_ground + moon->light_color * lift;
                }
            }
        }
    }
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
    // The sky pass draws its sun disk at -light.direction; while the moon holds the light that disk would sit on the
    // phased moon, so the local Sky copy drops it (M-NIGHTSKY 14e). Never Environment3D; unchanged otherwise.
    if (moon_holds_light) sky.sun_intensity = 0.0f;
    // An enabled DayCycle writing the environment (with a clock hour) owns the ambient: its keys are authored, the
    // atmosphere's single-scattering sky is dim in the shade and black soon after sunset.
    bool day_cycle_owns_ambient = false;
    world.query<const DayCycle&>().each([&](const DayCycle& c) { day_cycle_owns_ambient = day_cycle_owns_ambient || c.enabled != 0; });
    day_cycle_owns_ambient = day_cycle_owns_ambient && env.time_of_day >= 0.0f;
    const bool atmosphere_on = env.atmosphere != 0;
    render::Fog fog;
    fog.color = env.fog_color;
    fog.density = env.fog_density;
    // The atmosphere's sun is the light's direction, and at night the moon holds the light: it would paint a daylight
    // sky around the moon. With a DayCycle the sky then follows the sun itself (Atmosphere::sky_sun_direction), so the
    // hand-off changes nothing in the sky: it darkens with the sun, continuously. Without one the atmosphere steps
    // aside while the moon holds the light and the sky clears to the fog colour (the stars and the moon draw over it).
    const bool atmosphere_yields_to_moon = moon_holds_light && atmosphere_on && !day_cycle_owns_ambient;
    if (atmosphere_yields_to_moon) sky.enabled = false;
    render::Atmosphere atmosphere;
    atmosphere.enabled = atmosphere_on && !atmosphere_yields_to_moon;
    atmosphere.intensity = env.atmosphere_intensity;
    atmosphere.mie = env.atmosphere_haze;
    if (day_cycle_owns_ambient) atmosphere.ambient = false;  // the atmosphere still draws the sky and tints the sun
    if (atmosphere_on && day_cycle_owns_ambient && moon_holds_light) {
        atmosphere.sky_sun_direction = sun_natural.direction;
        atmosphere.sky_sun_light = sun_natural.color * sun_natural.intensity;
    }
    if (atmosphere_on) sky.texture_additive = false;  // Forward3D reads it under the atmosphere: only the floor sets it
    if (atmosphere_on && day_cycle_owns_ambient) {
        // The twilight floor: the single-scattering sky goes black within a few degrees under the horizon while the
        // DayCycle's dusk lasts hours, so a gradient in the (DayCycle-driven) fog colour fades in under the
        // atmosphere's sky as the sun sets: the horizon in the fog colour, the zenith darker. By night it is the
        // pre-atmosphere night sky (the fog colour) behind the stars. Stepped in 1/64 so the IBL re-bakes rarely.
        const f32 t = clamp((sun_elevation_natural + 0.1f) / 0.15f, 0.0f, 1.0f);  // 0 at -0.1, 1 at +0.05
        const f32 floor_w = std::round((1.0f - t * t * (3.0f - 2.0f * t)) * 64.0f) / 64.0f;
        if (floor_w > 0.0f) {
            sky.texture_additive = true;
            sky.horizon = env.fog_color * floor_w;
            sky.ground = env.fog_color * floor_w;
            sky.zenith = env.fog_color * (0.15f * floor_w);
        }
    }
    if (atmosphere_on && day_cycle_owns_ambient && sky.ibl) {
        // The sky's image-based diffuse at the DayCycle's level: the IBL keeps the sky's colour and direction, the
        // authored ambient sets how bright the shade is (the atmosphere's own sky is about a third of it by day).
        const render::DirectionalLight& sky_sun = moon_holds_light ? sun_natural : sun;
        const math::vec3 toward = render::atmosphere_quantized_sun(atmosphere, -math::normalize(sky_sun.direction));
        const math::vec3 above = sky_sun.color * sky_sun.intensity * 3.14159265f;
        math::vec3 sky_up = render::atmosphere_sky_ambient(atmosphere, toward) * above;
        if (sky.texture_additive) sky_up = sky_up + (sky.zenith + sky.horizon) * 0.5f;  // the floor, roughly
        const auto luma = [](math::vec3 c) { return 0.2126f * c.x + 0.7152f * c.y + 0.0722f * c.z; };
        sky.diffuse_ambient = clamp(luma(env.ambient_sky) / max(luma(sky_up), 1e-4f), 0.0f, 32.0f);
    }
    scene.set_sky(sky);
    scene.set_atmosphere(atmosphere);
    render::Volumetrics vol;
    vol.enabled = env.volumetric_fog != 0;
    vol.density = env.volumetric_density;
    vol.height_falloff = env.volumetric_height_falloff;
    vol.anisotropy = env.volumetric_anisotropy;
    vol.albedo = env.volumetric_albedo;
    vol.max_distance = env.volumetric_distance;
    {  // morning mist (M-GROUNDMIST 13b): the level feeds the local fog or volumetrics, never Environment3D
        const GroundMist* mist = nullptr;
        world.query<const GroundMist&>().each([&](const GroundMist& c) {
            if (!mist) mist = &c;
        });
        f32 level = 0.0f;
        if (mist && mist->enabled) {
            const Weather* mist_weather = nullptr;
            world.query<const Weather&>().each([&](const Weather& c) {
                if (!mist_weather) mist_weather = &c;
            });
            level = mist_level(*mist, env.time_of_day, mist_weather);
        }
        if (level > 0.0f) {
            const render::Volumetrics plain = vol;
            if (env.volumetric_fog != 0) {  // volumetric path; kept only if the tier leaves the volumetrics on
                const f32 falloff = 1.0f / clamp(mist->thickness_m, 0.5f, 200.0f);
                vol.density = min(1.0f, vol.density + level * clamp(mist->density, 0.0f, 1.0f));
                vol.height_falloff = lerp(vol.height_falloff, max(vol.height_falloff, falloff), level);
                vol.base_height = clamp(mist->base_height_m, -1000.0f, 1000.0f);
                vol.albedo = lerp(vol.albedo, mist->color, level * 0.5f);
            }
            render::apply_quality(quality_, vol);
            if (!vol.enabled) {  // exp2 path (volumetric fog off, or dropped by the tier): exactly one path per frame
                vol = plain;
                render::apply_quality(quality_, vol);
                fog.density += level * clamp(mist->exp2_density, 0.0f, 0.2f);
                fog.color = lerp(fog.color, mist->color, level);
            }
        } else {
            render::apply_quality(quality_, vol);
        }
    }
    scene.set_fog(fog);
    scene.set_volumetrics(vol);
    f32 cloud_cover = 0.0f;  // filled by the clouds block below, read by the night sky and the aurora
    {  // wet surfaces (M-WETSURFACE 5c): only a scene with both a Weather and a WetSurfaces gets wet
        const Weather* weather = nullptr;
        const WetSurfaces* wetc = nullptr;
        world.query<const Weather&>().each([&](const Weather& c) {
            if (!weather) weather = &c;
        });
        world.query<const WetSurfaces&>().each([&](const WetSurfaces& c) {
            if (!wetc) wetc = &c;
        });
        render::WetSurfaces wet;  // dry by default
        if (weather && wetc) {
            wet.wetness = weather->wetness * wetc->strength;
            wet.darken = wetc->darken;
            wet.roughness = wetc->wet_roughness;
            wet.puddles = wetc->puddles;
            wet.puddle_scale = wetc->puddle_scale_m;
            wet.puddles_enabled = true;
        }
        scene.set_wet_surfaces(render::apply_quality(quality_, wet));
    }
    {  // snow cover (M-SNOWCOVER 6c): only a scene with both a Weather and a SnowCover can whiten
        bool has_weather = false;
        const SnowCover* snowc = nullptr;
        world.query<const Weather&>().each([&](const Weather&) { has_weather = true; });
        world.query<const SnowCover&>().each([&](const SnowCover& c) {
            if (!snowc) snowc = &c;
        });
        const WindGust* snow_gust = nullptr;  // read only: the first WindGust raises the lee bias (M-WINDGUST 12b)
        world.query<const WindGust&>().each([&](const WindGust& c) {
            if (!snow_gust) snow_gust = &c;
        });
        const WindDirection* snow_wd = nullptr;
        world.query<const WindDirection&>().each([&](const WindDirection& c) {
            if (!snow_wd) snow_wd = &c;
        });
        render::SnowCover snow;  // bare by default (cover 0)
        if (has_weather && snowc) {
            snow.cover = snowc->cover * snowc->strength;
            snow.coverage = snowc->coverage;
            snow.noise_scale = snowc->noise_scale_m;
            snow.wind_bias = gusted_snow_bias(snowc->wind_bias, snow_gust);
            if (snow_wd) snow.wind_dir = wind_direction_vector(*snow_wd);  // else the default +x (M-WINDDIR 8d)
            snow.noise_enabled = true;
            const SeasonFrame season = read_season(world);  // M-SEASONS 19b: a drawn floor, the simulated cover is not raised
            if (season.snow_on) {
                snow.cover = std::max(snow.cover, season.snow.cover_floor * snowc->strength);
                snow.wind_bias = std::clamp(snow.wind_bias + season.snow.lee_shift, 0.0f, 1.0f);
            }
        }
        scene.set_snow_cover(render::apply_quality(quality_, snow));
    }
    {  // rain ripples (M-RAINFX 7c): only a scene with Weather, WetSurfaces and RainRipples, while it rains
        const Weather* weather = nullptr;
        bool has_wet = false;
        const RainRipples* ripc = nullptr;
        world.query<const Weather&>().each([&](const Weather& c) {
            if (!weather) weather = &c;
        });
        world.query<const WetSurfaces&>().each([&](const WetSurfaces&) { has_wet = true; });
        world.query<const RainRipples&>().each([&](const RainRipples& c) {
            if (!ripc) ripc = &c;
        });
        render::Ripples ripples;  // strength 0 by default (off)
        if (weather && has_wet && ripc && weather->intensity > 0.0f) {
            const f32 inten = weather->intensity < 1.0f ? weather->intensity : 1.0f;
            ripples.strength = ripc->strength * inten;
            ripples.density = ripc->density;
            ripples.speed = ripc->speed;
            ripples.ring_width = ripc->ring_width;
            ripples.enabled = true;
        }
        scene.set_ripples(render::apply_quality(quality_, ripples));
    }
    {  // clouds (M-CLOUDS 9c): only a scene with both a Weather and a CloudLayer; the clock is Scene3D::time()
        const Weather* weather = nullptr;
        const CloudLayer* cloudc = nullptr;
        const WindDirection* cloud_wd = nullptr;
        world.query<const Weather&>().each([&](const Weather& c) {
            if (!weather) weather = &c;
        });
        world.query<const CloudLayer&>().each([&](const CloudLayer& c) {
            if (!cloudc) cloudc = &c;
        });
        world.query<const WindDirection&>().each([&](const WindDirection& c) {
            if (!cloud_wd) cloud_wd = &c;
        });
        render::Clouds clouds;  // coverage 0 by default (off)
        if (weather && cloudc) {
            f32 kind = 0.0f;  // Clear 0, Overcast / Rain / Storm 1, Snow 0.9
            switch (weather->current) {
                case WeatherKind::Clear: kind = 0.0f; break;
                case WeatherKind::Snow: kind = 0.9f; break;
                default: kind = 1.0f; break;
            }
            f32 inten = weather->intensity;  // NaN falls to 0, the rest clamps to 0..1
            inten = inten > 0.0f ? (inten < 1.0f ? inten : 1.0f) : 0.0f;
            const f32 blend = kind * inten;
            clouds.coverage = (cloudc->fair_coverage + (cloudc->overcast_coverage - cloudc->fair_coverage) * blend) *
                              cloudc->strength;
            clouds.scale_m = cloudc->scale_m;
            clouds.height_m = cloudc->height_m;
            clouds.speed = weather->wind * cloudc->speed_scale;
            if (cloud_wd) clouds.wind_dir = wind_direction_vector(*cloud_wd);  // else the default +x
            clouds.enabled = true;
        }
        const render::Clouds shown = render::apply_quality(quality_, clouds);
        scene.set_clouds(shown);
        cloud_cover = shown.coverage;
    }
    const f32 dark = [&] {  // 0 by day, 1 at night: shared by the night sky and the aurora
        const f32 t = clamp((sun_elevation_natural + 0.1f) / 0.2f, 0.0f, 1.0f);
        return 1.0f - t * t * (3.0f - 2.0f * t);
    }();
    {  // night sky (M-NIGHTSKY 14e): moon, stars and their fade from the Moon, the clock, the clouds and the fog
        render::NightSky night;  // density and intensity 0: off
        if (moon && moon->enabled && env.time_of_day >= 0.0f && env.sky_texture.guid == asset::Guid{}) {
            const f32 phase = moon_phase(*moon, moon_day, env.time_of_day);
            night.moon_dir = -moon_direction(*moon, env.time_of_day, phase);  // toward the moon
            night.moon_phase = phase;
            night.moon_cos_size = moon->disk_size;
            night.moon_intensity = moon->disk_intensity;
            night.moon_color = moon->light_color;
            night.star_density = moon->star_density;
            night.star_brightness = moon->star_brightness;
            night.star_twinkle = moon->star_twinkle;
            // the value set_time() gets later in this extract (scene.time() would still be last frame's)
            night.star_time = !bake_mode_ && !extra_view_ ? time_ + max(frame_dt_, 0.0f) : time_;
            const f32 clear = 1.0f - clamp(cloud_cover, 0.0f, 1.0f);
            const f32 f = clamp(fog.density / 0.05f, 0.0f, 1.0f);  // fog is the local copy (weather and mist included)
            const f32 unfogged = 1.0f - f * f * (3.0f - 2.0f * f);
            night.night_fade = dark * clear * unfogged;
        }
        scene.set_night_sky(render::apply_quality(quality_, night));
    }
    {  // aurora (M-AURORA 16b): the first Aurora; without one, when disabled, with a closed gate or no night (level 0),
       // under an equirect sky texture or with non-finite inputs the carrier is the default one (level 0, off), written
       // every frame so a stale one never survives. Only the carrier is written.
        render::AuroraSky shown;  // level 0 by default (off)
        const Aurora* aurora = nullptr;
        world.query<const Aurora&>().each([&](const Aurora& c) {
            if (!aurora) aurora = &c;
        });
        if (aurora && aurora->enabled && env.sky_texture.guid == asset::Guid{} && std::isfinite(aurora->azimuth_deg) &&
            std::isfinite(aurora->width_deg)) {
            // no clock (time_of_day < 0): no night fade, so night_only 1 shows nothing and night_only 0 shows it
            const f32 night_fade = env.time_of_day >= 0.0f ? dark : 0.0f;
            const f32 level = aurora_level(*aurora, night_fade, cloud_cover);  // cloud_cover is post-apply_quality
            if (level > 0.0f && std::isfinite(level)) {
                using detail::aurora_clamp;
                render::AuroraSky a;
                a.level = level;
                a.base_height = aurora_clamp(aurora->base_height, 0.18f, 0.02f, 0.6f);
                a.height_span = aurora_clamp(aurora->height_span, 0.35f, 0.05f, 0.8f);
                // arc centre as an xz unit vector: azimuth_deg 0 = +z, counter-clockwise seen from above, like
                // WindDirection (90 = +x), so (x, z) = (sin, cos)
                const f64 az = static_cast<f64>(aurora->azimuth_deg) * (3.14159265358979323846 / 180.0);
                a.azimuth = math::vec2{static_cast<f32>(std::sin(az)), static_cast<f32>(std::cos(az))};
                // cosine of half the angular width (width_deg clamped to its Range 10..180)
                const f64 half = static_cast<f64>(aurora_clamp(aurora->width_deg, 70.0f, 10.0f, 180.0f)) * 0.5 *
                                 (3.14159265358979323846 / 180.0);
                a.half_width_cos = static_cast<f32>(std::cos(half));
                a.curtain_count = static_cast<i32>(std::clamp<u32>(aurora->curtain_count, 1u, 6u));
                a.speed = aurora_clamp(aurora->speed, 1.0f, 0.0f, 4.0f);
                a.low_color = aurora->low_color;
                a.high_color = aurora->high_color;
                // the value set_time() gets later in this extract (scene.time() would still be last frame's), wrapped in f64
                const f32 frame_time = !bake_mode_ && !extra_view_ ? time_ + max(frame_dt_, 0.0f) : time_;
                a.time = aurora_wrap_time(static_cast<f64>(frame_time));
                a.enabled = true;
                shown = render::apply_quality(quality_, a);
            }
        }
        scene.set_aurora(shown);
    }
    {  // rainbow (M-RAINBOW 17b): the first Rainbow; without one, when disabled, with a closed gate (night, rain, dry
       // ground, thick cloud: level 0), under an equirect sky texture or with non-finite inputs the carrier is the default
       // one (level 0, off), written every frame so a stale one never survives. Only the carrier is written.
        render::RainbowSky shown;  // level 0 by default (off)
        const Rainbow* rainbow = nullptr;
        world.query<const Rainbow&>().each([&](const Rainbow& c) {
            if (!rainbow) rainbow = &c;
        });
        const Weather* rain_weather = nullptr;
        world.query<const Weather&>().each([&](const Weather& c) {
            if (!rain_weather) rain_weather = &c;
        });
        if (rainbow && rainbow->enabled && rain_weather && env.sky_texture.guid == asset::Guid{}) {
            // Sun.direction is the way the light travels (from the sun toward the scene), so the direction from the
            // camera toward the sun is its negation and the direction toward the anti-solar point is the light direction
            // itself. The elevation is asin of the sun's height: -direction.y of the unit light direction (the sine, which
            // sun_elevation_natural is for a unit direction), in degrees. The moonlight hand-off above is not applied.
            const math::vec3 light = env.time_of_day >= 0.0f ? render::sun_direction_at(env.time_of_day) : env.sun_direction;
            const f32 len = std::sqrt(light.x * light.x + light.y * light.y + light.z * light.z);
            const f32 wetness = rain_weather->wetness;  // the ground wetness the Weather system keeps (0 = dry, no bow)
            const bool raining = rain_weather->current == WeatherKind::Rain || rain_weather->current == WeatherKind::Storm;
            const f32 rain = raining ? rain_weather->intensity : 0.0f;
            if (std::isfinite(len) && len > 1e-6f && std::isfinite(wetness) && std::isfinite(rain) && std::isfinite(cloud_cover)) {
                const math::vec3 anti = light * (1.0f / len);  // unit, camera toward the anti-solar point
                const f32 sine = std::clamp(-anti.y, -1.0f, 1.0f);
                const f32 elevation_deg = static_cast<f32>(std::asin(static_cast<f64>(sine)) * (180.0 / 3.14159265358979323846));
                const f32 level = rainbow_level(*rainbow, elevation_deg, wetness, rain, cloud_cover);  // post-apply_quality cover
                if (level > 0.0f && std::isfinite(level)) {
                    using detail::rainbow_clamp;
                    render::RainbowSky r;
                    r.level = level;
                    r.band_width = rainbow_clamp(rainbow->band_width, 1.0f, 0.5f, 2.0f);
                    r.secondary = rainbow_clamp(rainbow->secondary, 1.0f, 0.0f, 1.0f);
                    r.anti_solar = anti;
                    r.enabled = true;
                    shown = render::apply_quality(quality_, r);
                }
            }
        }
        scene.set_rainbow(shown);
    }
    {  // meteor (M-METEOR 18b): the first Meteor; without one, when disabled, with a closed gate (day, thick cloud, rain:
       // level 0) or under an equirect sky texture the carrier is the default one (level 0), written every frame
       // because Scene3D::begin does not reset it. Only the carrier is written; no Moon is needed.
        render::MeteorSky shown;  // level 0 by default (off)
        const Meteor* meteor = nullptr;
        world.query<const Meteor&>().each([&](const Meteor& c) {
            if (!meteor) meteor = &c;
        });
        if (meteor && meteor->enabled && env.sky_texture.guid == asset::Guid{}) {
            const Weather* meteor_weather = nullptr;
            world.query<const Weather&>().each([&](const Weather& c) {
                if (!meteor_weather) meteor_weather = &c;
            });
            const bool raining =
                meteor_weather && (meteor_weather->current == WeatherKind::Rain || meteor_weather->current == WeatherKind::Storm);
            const f32 rain = raining ? meteor_weather->intensity : 0.0f;
            // no clock (time_of_day < 0): no night fade, so night_only 1 shows nothing and night_only 0 shows it
            const f32 night_fade = env.time_of_day >= 0.0f ? dark : 0.0f;
            const f32 level = meteor_level(*meteor, night_fade, cloud_cover, rain);  // cloud_cover is post-apply_quality
            if (level > 0.0f && std::isfinite(level)) {
                render::MeteorSky m;
                m.level = level;
                m.rate = detail::meteor_rate(*meteor);
                m.brightness = detail::meteor_brightness(*meteor);
                m.length = detail::meteor_length(*meteor);
                m.speed = detail::meteor_speed(*meteor);
                m.seed = detail::meteor_seed(*meteor);
                // the value set_time() gets later in this extract (scene.time() would still be last frame's), wrapped in f64
                const f32 frame_time = !bake_mode_ && !extra_view_ ? time_ + max(frame_dt_, 0.0f) : time_;
                m.time = meteor_wrap_time(static_cast<f64>(frame_time));
                m.enabled = true;
                shown = render::apply_quality(quality_, m);
            }
        }
        scene.set_meteor(shown);
    }
    {  // water (M-WATER 15b): the first Water; nothing is set without one, when disabled or with a non-finite level
        const Water* water = nullptr;
        const WindDirection* water_wd = nullptr;
        world.query<const Water&>().each([&](const Water& c) {
            if (!water) water = &c;
        });
        if (water && water->enabled && std::isfinite(water->level_y)) {
            world.query<const WindDirection&>().each([&](const WindDirection& c) {
                if (!water_wd) water_wd = &c;
            });
            render::WaterSurface surf;
            surf.level_y = water->level_y;
            surf.half_extent = water->half_extent;
            surf.wave_count = static_cast<i32>(water->wave_count);
            surf.amplitude = water->amplitude;
            surf.wavelength = water->wavelength;
            surf.steepness = water->steepness;
            // main direction in degrees (0 = +x, counter-clockwise, as water_wave_set expects): the WindDirection heading
            // when following, else the authored one
            const f64 deg = water_wd && water->wind_follow > 0.0f ? static_cast<f64>(water_wd->angle_q) / 65536.0
                                                                   : static_cast<f64>(water->wind_dir_deg);
            const f64 rad = deg * (3.14159265358979323846 / 180.0);
            surf.wind_dir = math::vec2{static_cast<f32>(std::cos(rad)), static_cast<f32>(std::sin(rad))};
            // the value set_time() gets later in this extract (scene.time() would still be last frame's), wrapped in f64
            const f32 frame_time = !bake_mode_ && !extra_view_ ? time_ + max(frame_dt_, 0.0f) : time_;
            surf.time = water_wrap_time(static_cast<f64>(frame_time));
            surf.speed_scale = water->speed_scale;
            surf.shallow_color = water->shallow_color;
            surf.deep_color = water->deep_color;
            surf.depth_scale_m = water->depth_scale_m;
            surf.reflectivity = water->reflectivity;
            surf.roughness = water->roughness;
            surf.foam_amount = water->foam_amount;
            surf.foam_width_m = water->foam_width_m;
            surf.enabled = true;
            {  // M-FREEZE 20b: the freeze look on the local carrier, before the quality tier; skipped at an amount of 0
                const WaterIce* ice = nullptr;
                const Season* ice_season = nullptr;
                world.query<const WaterIce&>().each([&](const WaterIce& c) {
                    if (!ice) ice = &c;
                });
                if (ice) {
                    world.query<const Season&>().each([&](const Season& c) {
                        if (!ice_season) ice_season = &c;
                    });
                    u32 day = 0;
                    f32 hour = 0.0f;
                    bool have_clock = false;
                    world.query<const WorldClock&>().each([&](const WorldClock& c) {
                        if (have_clock) return;
                        have_clock = true;
                        day = c.day;
                        hour = c.hour;
                    });
                    const f32 phase = ice_season ? season_phase(*ice_season, day, hour) : 0.0f;
                    const f32 amount = water_ice_amount(*ice, ice_season, phase);
                    if (amount > 0.0f) apply_water_ice(*ice, water_ice_look(*ice, amount), surf);
                }
            }
            scene.set_water(render::apply_quality(quality_, surf));
        }
    }
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
    post.auto_exposure.min_exposure = env.auto_exposure_min;
    post.auto_exposure.max_exposure = env.auto_exposure_max;
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
    // Why a scene with MeshRefs draws none (a hand-written scene that renders empty): counted per reason, said once.
    u32 refs = 0, hidden = 0, no_mesh = 0, no_pose = 0, culled = 0, merged = 0, not_loaded = 0;
    world.query<const MeshRef&>().each([&](Entity e, const MeshRef& m) {
        ++refs;
        if (!m.visible) {
            ++hidden;
            return;
        }
        if (merge_stamp_ != 0 && e.index < merged_stamp_.size() && merged_stamp_[e.index] == merge_stamp_) {  // drawn merged
            ++merged;
            return;
        }
        vec3 pos, scale;
        quat rot;
        if (!pose_of(world, e, pos, rot, scale)) {
            ++no_pose;
            return;
        }
        if (m.interpolate) {
            track(e, pos, rot, scale, tick);
            interpolated(e, alpha, pos, rot, scale);
        }
        if (!bake_mode_ && m.cull_radius > 0.0f && !in_view(pos, m.cull_radius * max(scale.x, max(scale.y, scale.z)))) {
            ++culled;
            return;
        }
        const u32 key = mesh_key(m.mesh.guid);
        const render::GpuMesh* mesh = key != ~0u && meshes_ ? meshes_->get(key) : nullptr;
        if (!mesh) {
            ++stats_.missing;
            if (m.mesh.guid == asset::Guid{}) ++no_mesh;
            else ++not_loaded;
            return;
        }
        render::MeshInstance inst;
        inst.mesh = key;
        inst.world = trs(pos, rot, scale);
        if (mesh->lod_levels > 1 && lod_states_.size() <= e.index) grow_to(lod_states_, e.index + 1, u8(0));
        if (world_stamp_.size() <= e.index) {
            grow_to(world_stamp_, e.index + 1, ~0u);  // never drawn: ~0u + 1 matches no stamp
            grow_to(prev_ok_, e.index + 1, u8(0));
            for (Array<mat4>& w : worlds_) grow_to(w, e.index + 1, mat4::identity());
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
    // Only for causes a scene's author can fix (no mesh set, no Transform, a mesh that did not load): hidden or culled
    // meshes are ordinary (a cutscene start, a minimap view).
    if (refs > 0 && stats_.meshes == 0 && merged == 0 && !bake_mode_ && !extra_view_ && no_mesh + no_pose + not_loaded > 0) {
        static std::atomic<bool> said{false};  // once per process: a scene that stays empty would repeat it every frame
        if (!said.exchange(true)) {
            MCE_LOG_WARN("kit3d: {} MeshRef entities and none drawn: {} with no mesh set, {} with no Transform, {} whose mesh is not "
                         "loaded (see the 'mesh ... not in the mounted content / not resident / failed to load' lines), {} hidden, {} culled",
                         refs, no_mesh, no_pose, not_loaded, hidden, culled);
        }
    }
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
    // ChunkLod3D (M-OPENWORLD A4): a streamed area that is not loaded draws its proxy, or its impostor far away.
    if (!bake_mode_ && world.find_component(kTypeHash<ChunkLod3D>) != ecs::kInvalidComponent) {
        world.query<const ChunkLod3D&, const scene::SceneChunk&>().each(
            [&](Entity e, const ChunkLod3D& c, const scene::SceneChunk& chunk) {
                if (!c.enabled) return;
                const scene::SceneChunkStatus* st = world.try_get<scene::SceneChunkStatus>(e);
                if (st && st->state == scene::ChunkState::Loaded) return;  // the area's own entities draw
                vec3 pos, scale;
                quat rot;
                if (!pose_of(world, e, pos, rot, scale)) return;
                const f32 dist = scene::distance_to_box(view.position, pos, chunk.half_extents);
                if (c.max_distance > 0.0f && dist > c.max_distance) return;
                const bool far = !c.impostor.guid.is_null() && dist > c.impostor_distance;
                const asset::Guid guid = far ? c.impostor.guid : c.proxy.guid;
                if (guid.is_null()) return;
                if (!in_view(pos, length(chunk.half_extents))) return;
                const u32 key = mesh_key(guid);
                if (key == ~0u || !meshes_ || !meshes_->get(key)) {
                    ++stats_.missing;
                    return;
                }
                render::MeshInstance inst;
                inst.mesh = key;
                inst.world = trs(pos, rot, scale);
                inst.cast_shadows = c.cast_shadows != 0;
                scene.add(inst);
                ++(far ? stats_.chunk_impostors : stats_.chunk_proxies);
            });
    }
    extract_terrains(world, scene);
    extract_lightning_bolt(world, scene);

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
    const Weather* sway_weather = nullptr;  // wind sway: only a scene with both a Weather and a WindSway (first of each)
    const WindSway* sway = nullptr;
    world.query<const Weather&>().each([&](const Weather& c) {
        if (!sway_weather) sway_weather = &c;
    });
    world.query<const WindSway&>().each([&](const WindSway& c) {
        if (!sway) sway = &c;
    });
    const WindGust* sway_gust = nullptr;  // read only: the first WindGust gusts the sway (M-WINDGUST 12b)
    world.query<const WindGust&>().each([&](const WindGust& c) {
        if (!sway_gust) sway_gust = &c;
    });
    const f32 sway_wind = sway_weather && sway ? effective_wind(*sway_weather, sway_gust, kGustSway) : 0.0f;
    const WindDirection* wind_dir = nullptr;  // wind direction: only a scene with both a Weather and a WindDirection
    world.query<const WindDirection&>().each([&](const WindDirection& c) {
        if (!wind_dir) wind_dir = &c;
    });
    const bool turn_wind = sway_weather && wind_dir;
    const SeasonFrame season = read_season(world);  // M-SEASONS 19b
    const math::vec2 scene_wind = turn_wind ? wind_direction_vector(*wind_dir) : math::vec2{1.0f, 0.0f};
    world.query<const Foliage3D&>().each([&](Entity e, const Foliage3D& f) {
        if (!f.enabled || f.count == 0) return;
        vec3 pos, scale;
        quat rot;
        if (!pose_of(world, e, pos, rot, scale)) return;
        // Up to four kinds (mesh, mesh_1..3) share the scatter; a kind whose mesh is unbound or not resident, or whose weight
        // is 0, gets no copies.
        const asset::Guid kind_guid[4] = {f.mesh.guid, f.mesh_1.guid, f.mesh_2.guid, f.mesh_3.guid};
        const auto finite_weight = [](f32 w) { return std::isfinite(w) ? clamp(w, 0.0f, 1.0e6f) : 0.0f; };
        const f32 kind_weight[4] = {finite_weight(f.mesh_weights.x), finite_weight(f.mesh_weights.y), finite_weight(f.mesh_weights.z),
                                    finite_weight(f.mesh_weights.w)};
        u32 kind_key[4];
        f32 cumulative[4];
        u32 resolved = 0;
        for (u32 k = 0; k < 4; ++k) {
            kind_key[k] = mesh_key(kind_guid[k]);
            const bool live = kind_key[k] != ~0u && meshes_ && meshes_->get(kind_key[k]) && kind_weight[k] > 0.0f;
            if (live) resolved |= 1u << k;
            cumulative[k] = (k ? cumulative[k - 1] : 0.0f) + (live ? kind_weight[k] : 0.0f);
        }
        if (!resolved) {
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
        mix(&resolved, sizeof(resolved));  // which kinds exist, and their shares, split the scatter
        mix(cumulative, sizeof(cumulative));
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
        if (entry->version == 0 || entry->key != h) {  // a new entry, or the inputs changed
            render::FoliageScatter sc;
            sc.origin = pos;
            sc.half_extents = f.half_extents;
            sc.count = f.count;
            sc.seed = f.seed;
            sc.scale_min = f.scale_min;
            sc.scale_max = f.scale_max;
            sc.random_yaw = f.random_yaw != 0;
            if (resolved == 1u) {  // one kind: the scatter as it always was
                render::scatter_foliage(sc, entry->transforms);
                for (Array<math::mat4>& m : entry->more) m.clear();
            } else {
                Array<math::mat4> all(mem::heap());
                render::scatter_foliage(sc, all);
                entry->transforms.clear();
                for (Array<math::mat4>& m : entry->more) m.clear();
                for (const math::mat4& m : all) {
                    const u32 kind = foliage_kind(m, f.seed, cumulative, resolved);
                    (kind == 0 ? entry->transforms : entry->more[kind - 1]).push_back(m);
                }
            }
            entry->key = h;
            ++entry->version;
        }
        entry->stamp = frame_stamp_;
        for (u32 k = 0; k < 4; ++k) {
            if (!(resolved >> k & 1u)) continue;
            const Array<math::mat4>& copies = k == 0 ? entry->transforms : entry->more[k - 1];
            if (copies.empty()) continue;
            render::InstanceBatch b;
            // Entity indices stay far below 2^29 (a world holds millions at most); a larger one would collide with another kind's id.
            MCE_ASSERT_MSG(e.index < (1u << 29) - 1, "Foliage3D batch id: entity index needs bit 29");
            b.id = (e.index + 1) | (k << 29);  // disjoint per kind, and from the TerrainFoliage3D ids (bit 31)
            b.version = entry->version;
            b.mesh = kind_key[k];
            b.transforms = copies.span();
            b.cast_shadows = f.cast_shadows != 0;
            b.cull_distance = bake_mode_ ? 0.0f : f.cull_distance;
            b.tint = f.tint;
            b.material = material(f.material.guid);
            b.wind_amplitude = f.wind_amplitude;
            b.wind_frequency = f.wind_frequency;
            b.wind_direction = f.wind_direction;
            foliage_look(f, b);
            if (sway_weather && sway) apply_wind_sway(sway_wind, *sway, b);
            if (season.tint_on) apply_season_tint(season.tint, b);
            if (turn_wind) apply_wind_direction(scene_wind, b);
            scene.add_batch(b);
            ++stats_.foliage;
            stats_.foliage_instances += static_cast<u32>(copies.size());
        }
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
        foliage_look(f, b);
        if (sway_weather && sway) apply_wind_sway(sway_wind, *sway, b);
        if (season.tint_on) apply_season_tint(season.tint, b);
        if (turn_wind) apply_wind_direction(scene_wind, b);
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
        // M-PARTICLEUSER 29b: the weather rain and snow emitter (an entity that also carries Precipitation3D) keeps
        // particle_scale of its rate and budget, render-side only; every other ParticleEmitter3D is untouched, and
        // at scale 1 the emitter is bit-identical. Scale 0 switches it off.
        if (world.has<Precipitation3D>(e)) {
            const f32 pscale = particle_scale_of(quality_);
            if (pscale <= 0.0f) {
                em.emitting = false;
                em.rate = 0.0f;
            } else {
                em.rate = p.rate * pscale;
                em.max_particles = scaled_budget(p.max_particles, pscale);
            }
        }
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
    {  // leaf fall (M-LEAFFALL 21b): one render-side emitter around the camera; nothing is written, nothing added at amount 0
        const LeafFall* leaf = nullptr;
        world.query<const LeafFall&>().each([&](const LeafFall& c) {
            if (!leaf) leaf = &c;
        });
        if (leaf) {
            const Season* leaf_season = nullptr;
            world.query<const Season&>().each([&](const Season& c) {
                if (!leaf_season) leaf_season = &c;
            });
            u32 day = 0;
            f32 hour = 0.0f;
            bool have_clock = false;
            world.query<const WorldClock&>().each([&](const WorldClock& c) {
                if (have_clock) return;
                have_clock = true;
                day = c.day;
                hour = c.hour;
            });
            const f32 phase = leaf_season ? season_phase(*leaf_season, day, hour) : 0.0f;
            const f32 amount = leaf_fall_amount(*leaf, leaf_season, phase);
            if (amount > 0.0f) {
                const Weather* leaf_weather = nullptr;
                world.query<const Weather&>().each([&](const Weather& c) {
                    if (!leaf_weather) leaf_weather = &c;
                });
                const WindGust* leaf_gust = nullptr;
                world.query<const WindGust&>().each([&](const WindGust& c) {
                    if (!leaf_gust) leaf_gust = &c;
                });
                const WindDirection* leaf_wd = nullptr;
                world.query<const WindDirection&>().each([&](const WindDirection& c) {
                    if (!leaf_wd) leaf_wd = &c;
                });
                // No Weather: calm. The gust sway bit: leaves move like the foliage. No WindDirection (or no Weather): +x.
                const f32 wind = leaf_weather ? effective_wind(*leaf_weather, leaf_gust, kGustSway) : 0.0f;
                const math::vec2 wdir = leaf_weather && leaf_wd ? wind_direction_vector(*leaf_wd) : math::vec2{1.0f, 0.0f};
                const LeafFallLook look = leaf_fall_look(*leaf, amount, wind, wdir, season_foliage_tint(leaf_season, phase));
                const f32 pscale = particle_scale_of(quality_);
                if (look.active && pscale > 0.0f) {
                    render::ParticleEmitter em;
                    em.id = kLeafFallEmitterId;
                    em.max_particles = std::clamp(leaf->max_particles, 64u, 8192u);
                    em.max_particles = scaled_budget(em.max_particles, pscale);
                    em.seed = leaf->seed;
                    em.position = scene.view().position + math::vec3{0.0f, look.height, 0.0f};
                    em.box = math::vec3{look.box, 0.0f, look.box};
                    em.direction = look.direction;
                    em.spread = look.spread;
                    em.rate = look.rate * pscale;
                    em.burst = 0;
                    em.emitting = true;
                    em.speed_min = look.speed_min;
                    em.speed_max = look.speed_max;
                    em.lifetime_min = look.lifetime_min;
                    em.lifetime_max = look.lifetime_max;
                    em.gravity = look.gravity;
                    em.drag = look.drag;
                    em.collide = false;
                    em.size_start = look.size;
                    em.size_end = look.size;
                    em.color_start = look.color_start;
                    em.color_end = look.color_end;
                    em.additive = false;
                    scene.add_particle_emitter(em);
                    ++stats_.particle_emitters;
                }
            }
        }
    }
    {  // fireflies (M-FIREFLIES 23b): one additive render-side emitter around the camera; nothing is written, nothing added at amount 0
        const Firefly* ff = nullptr;
        world.query<const Firefly&>().each([&](const Firefly& c) {
            if (!ff) ff = &c;
        });
        if (ff) {
            const Season* ff_season = nullptr;
            world.query<const Season&>().each([&](const Season& c) {
                if (!ff_season) ff_season = &c;
            });
            u32 day = 0;
            f32 hour = 0.0f;
            bool have_clock = false;
            world.query<const WorldClock&>().each([&](const WorldClock& c) {
                if (have_clock) return;
                have_clock = true;
                day = c.day;
                hour = c.hour;
            });
            const f32 phase = ff_season ? season_phase(*ff_season, day, hour) : 0.0f;
            const Weather* ff_weather = nullptr;
            world.query<const Weather&>().each([&](const Weather& c) {
                if (!ff_weather) ff_weather = &c;
            });
            const bool raining =
                ff_weather && (ff_weather->current == WeatherKind::Rain || ff_weather->current == WeatherKind::Storm);
            const f32 rain = raining ? ff_weather->intensity : 0.0f;
            // the night fade the meteor block uses (apply_environment's `dark`); no clock (time_of_day < 0): 0
            const Environment3D ff_env = resolve_environment(world);
            f32 night_fade = 0.0f;
            if (ff_env.time_of_day >= 0.0f) {
                const f32 elev = -render::sun_direction_at(ff_env.time_of_day).y;
                const f32 t = clamp((elev + 0.1f) / 0.2f, 0.0f, 1.0f);
                night_fade = 1.0f - t * t * (3.0f - 2.0f * t);
            }
            const f32 cloud = scene.clouds().coverage;  // post-apply_quality, as the meteor block reads it
            const f32 amount = firefly_amount(*ff, ff_season, phase, night_fade, cloud, rain);
            const FireflyLook look = firefly_look(*ff, amount);
            const f32 pscale = particle_scale_of(quality_);
            if (look.active && pscale > 0.0f) {
                render::ParticleEmitter em;
                em.id = kFireflyEmitterId;
                em.max_particles = std::clamp(ff->max_particles, 16u, 4096u);
                em.max_particles = scaled_budget(em.max_particles, pscale);
                em.seed = ff->seed;
                em.position = scene.view().position + math::vec3{0.0f, look.height, 0.0f};
                em.box = math::vec3{look.box, 0.0f, look.box};
                em.direction = look.direction;
                em.spread = look.spread;
                em.rate = look.rate * pscale;
                em.burst = 0;
                em.emitting = true;
                em.speed_min = look.speed_min;
                em.speed_max = look.speed_max;
                em.lifetime_min = look.lifetime_min;
                em.lifetime_max = look.lifetime_max;
                em.gravity = look.gravity;
                em.drag = look.drag;
                em.collide = false;
                em.size_start = look.size;
                em.size_end = look.size;
                em.color_start = look.color_start;
                em.color_end = look.color_end;
                em.additive = true;
                scene.add_particle_emitter(em);
                ++stats_.particle_emitters;
            }
        }
    }
    {  // pollen (M-POLLEN 24b): one alpha-blended render-side emitter around the camera; nothing is written, nothing added at amount 0
        const Pollen* po = nullptr;
        world.query<const Pollen&>().each([&](const Pollen& c) {
            if (!po) po = &c;
        });
        if (po) {
            const Season* po_season = nullptr;
            world.query<const Season&>().each([&](const Season& c) {
                if (!po_season) po_season = &c;
            });
            u32 day = 0;
            f32 hour = 0.0f;
            bool have_clock = false;
            world.query<const WorldClock&>().each([&](const WorldClock& c) {
                if (have_clock) return;
                have_clock = true;
                day = c.day;
                hour = c.hour;
            });
            const f32 phase = po_season ? season_phase(*po_season, day, hour) : 0.0f;
            const Weather* po_weather = nullptr;
            world.query<const Weather&>().each([&](const Weather& c) {
                if (!po_weather) po_weather = &c;
            });
            const bool raining =
                po_weather && (po_weather->current == WeatherKind::Rain || po_weather->current == WeatherKind::Storm);
            const f32 rain = raining ? po_weather->intensity : 0.0f;
            // the night fade as the firefly block derives it; no clock (time_of_day < 0): 0
            const Environment3D po_env = resolve_environment(world);
            f32 night_fade = 0.0f;
            if (po_env.time_of_day >= 0.0f) {
                const f32 elev = -render::sun_direction_at(po_env.time_of_day).y;
                const f32 t = clamp((elev + 0.1f) / 0.2f, 0.0f, 1.0f);
                night_fade = 1.0f - t * t * (3.0f - 2.0f * t);
            }
            const f32 amount = pollen_amount(*po, po_season, phase, night_fade, rain);
            if (amount > 0.0f) {
                const WindGust* po_gust = nullptr;
                world.query<const WindGust&>().each([&](const WindGust& c) {
                    if (!po_gust) po_gust = &c;
                });
                const WindDirection* po_wd = nullptr;
                world.query<const WindDirection&>().each([&](const WindDirection& c) {
                    if (!po_wd) po_wd = &c;
                });
                // As the leaf block: no Weather is calm, the gust sway bit, no WindDirection (or no Weather) is +x.
                const f32 wind = po_weather ? effective_wind(*po_weather, po_gust, kGustSway) : 0.0f;
                const math::vec2 wdir = po_weather && po_wd ? wind_direction_vector(*po_wd) : math::vec2{1.0f, 0.0f};
                const PollenLook look = pollen_look(*po, amount, wind, wdir);
                const f32 pscale = particle_scale_of(quality_);
                if (look.active && pscale > 0.0f) {
                    render::ParticleEmitter em;
                    em.id = kPollenEmitterId;
                    em.max_particles = std::clamp(po->max_particles, 16u, 4096u);
                    em.max_particles = scaled_budget(em.max_particles, pscale);
                    em.seed = po->seed;
                    em.position = scene.view().position + math::vec3{0.0f, look.height, 0.0f};
                    em.box = math::vec3{look.box, 0.0f, look.box};
                    em.direction = look.direction;
                    em.spread = look.spread;
                    em.rate = look.rate * pscale;
                    em.burst = 0;
                    em.emitting = true;
                    em.speed_min = look.speed_min;
                    em.speed_max = look.speed_max;
                    em.lifetime_min = look.lifetime_min;
                    em.lifetime_max = look.lifetime_max;
                    em.gravity = look.gravity;
                    em.drag = look.drag;
                    em.collide = false;
                    em.size_start = look.size;
                    em.size_end = look.size;
                    em.color_start = look.color_start;
                    em.color_end = look.color_end;
                    em.additive = false;
                    scene.add_particle_emitter(em);
                    ++stats_.particle_emitters;
                }
            }
        }
    }
    {  // dust (M-DUSTWIND 25b): one alpha-blended render-side emitter around the camera; nothing is written, nothing added at amount 0
        const Dust* du = nullptr;
        world.query<const Dust&>().each([&](const Dust& c) {
            if (!du) du = &c;
        });
        if (du && du->enabled != 0 && du->strength > 0.0f) {
            const Weather* du_weather = nullptr;
            world.query<const Weather&>().each([&](const Weather& c) {
                if (!du_weather) du_weather = &c;
            });
            const bool raining =
                du_weather && (du_weather->current == WeatherKind::Rain || du_weather->current == WeatherKind::Storm);
            const f32 rain = raining ? du_weather->intensity : 0.0f;
            const WindGust* du_gust = nullptr;
            world.query<const WindGust&>().each([&](const WindGust& c) {
                if (!du_gust) du_gust = &c;
            });
            // As the leaf block: no Weather is calm, the gust sway bit.
            const f32 wind = du_weather ? effective_wind(*du_weather, du_gust, kGustSway) : 0.0f;
            const f32 amount = dust_amount(*du, wind, rain);
            if (amount > 0.0f) {
                const WindDirection* du_wd = nullptr;
                world.query<const WindDirection&>().each([&](const WindDirection& c) {
                    if (!du_wd) du_wd = &c;
                });
                const math::vec2 wdir = du_weather && du_wd ? wind_direction_vector(*du_wd) : math::vec2{1.0f, 0.0f};
                const DustLook look = dust_look(*du, amount, wind, wdir);
                const f32 pscale = particle_scale_of(quality_);
                if (look.active && pscale > 0.0f) {
                    render::ParticleEmitter em;
                    em.id = kDustEmitterId;
                    em.max_particles = std::clamp(du->max_particles, 16u, 4096u);
                    em.max_particles = scaled_budget(em.max_particles, pscale);
                    em.seed = du->seed;
                    em.position = scene.view().position + math::vec3{0.0f, look.height, 0.0f};
                    em.box = math::vec3{look.box, 0.0f, look.box};
                    em.direction = look.direction;
                    em.spread = look.spread;
                    em.rate = look.rate * pscale;
                    em.burst = 0;
                    em.emitting = true;
                    em.speed_min = look.speed_min;
                    em.speed_max = look.speed_max;
                    em.lifetime_min = look.lifetime_min;
                    em.lifetime_max = look.lifetime_max;
                    em.gravity = look.gravity;
                    em.drag = look.drag;
                    em.collide = false;
                    em.size_start = look.size;
                    em.size_end = look.size;
                    em.color_start = look.color_start;
                    em.color_end = look.color_end;
                    em.additive = false;
                    scene.add_particle_emitter(em);
                    ++stats_.particle_emitters;
                }
            }
        }
    }
    {  // flock (M-BIRDFLOCK 26b): one alpha-blended render-side emitter around the camera; nothing is written, nothing added at amount 0
        const Flock* fl = nullptr;
        world.query<const Flock&>().each([&](const Flock& c) {
            if (!fl) fl = &c;
        });
        if (fl && fl->enabled != 0 && fl->strength > 0.0f) {
            const Season* fl_season = nullptr;
            world.query<const Season&>().each([&](const Season& c) {
                if (!fl_season) fl_season = &c;
            });
            u32 day = 0;
            f32 hour = 0.0f;
            bool have_clock = false;
            world.query<const WorldClock&>().each([&](const WorldClock& c) {
                if (have_clock) return;
                have_clock = true;
                day = c.day;
                hour = c.hour;
            });
            const f32 phase = fl_season ? season_phase(*fl_season, day, hour) : 0.0f;
            const Weather* fl_weather = nullptr;
            world.query<const Weather&>().each([&](const Weather& c) {
                if (!fl_weather) fl_weather = &c;
            });
            const bool raining =
                fl_weather && (fl_weather->current == WeatherKind::Rain || fl_weather->current == WeatherKind::Storm);
            const f32 rain = raining ? fl_weather->intensity : 0.0f;
            // The sun height as the sun block resolves it: the day clock when time_of_day >= 0, else the environment's fixed
            // sun_direction (its y, normalised). The twilight gate uses |y|, so the sign convention does not matter.
            const Environment3D fl_env = resolve_environment(world);
            f32 sun_y = 1.0f;
            if (fl_env.time_of_day >= 0.0f) {
                sun_y = render::sun_direction_at(fl_env.time_of_day).y;
            } else if (length(fl_env.sun_direction) > 0.0f) {
                sun_y = normalize(fl_env.sun_direction).y;
            }
            const f32 amount = flock_amount(*fl, fl_season, phase, sun_y, rain);
            if (amount > 0.0f) {
                const FlockLook look = flock_look(*fl, amount);
                const f32 pscale = particle_scale_of(quality_);
                if (look.active && pscale > 0.0f) {
                    render::ParticleEmitter em;
                    em.id = kFlockEmitterId;
                    em.max_particles = std::clamp(fl->max_particles, 16u, 1024u);
                    em.max_particles = scaled_budget(em.max_particles, pscale);
                    em.seed = fl->seed;
                    em.position = scene.view().position + math::vec3{0.0f, look.height, 0.0f};
                    em.box = math::vec3{look.box, 0.0f, look.box};
                    em.direction = look.direction;
                    em.spread = look.spread;
                    em.rate = look.rate * pscale;
                    em.burst = 0;
                    em.emitting = true;
                    em.speed_min = look.speed_min;
                    em.speed_max = look.speed_max;
                    em.lifetime_min = look.lifetime_min;
                    em.lifetime_max = look.lifetime_max;
                    em.gravity = look.gravity;
                    em.drag = look.drag;
                    em.collide = false;
                    em.size_start = look.size;
                    em.size_end = look.size;
                    em.color_start = look.color_start;
                    em.color_end = look.color_end;
                    em.additive = false;
                    scene.add_particle_emitter(em);
                    ++stats_.particle_emitters;
                }
            }
        }
    }
    {  // butterflies (M-BUTTERFLY 27b): one alpha-blended render-side emitter around the camera; nothing is written, nothing added at amount 0
        const Butterfly* bf = nullptr;
        world.query<const Butterfly&>().each([&](const Butterfly& c) {
            if (!bf) bf = &c;
        });
        if (bf && bf->enabled != 0 && bf->strength > 0.0f) {
            const Season* bf_season = nullptr;
            world.query<const Season&>().each([&](const Season& c) {
                if (!bf_season) bf_season = &c;
            });
            u32 day = 0;
            f32 hour = 0.0f;
            bool have_clock = false;
            world.query<const WorldClock&>().each([&](const WorldClock& c) {
                if (have_clock) return;
                have_clock = true;
                day = c.day;
                hour = c.hour;
            });
            const f32 phase = bf_season ? season_phase(*bf_season, day, hour) : 0.0f;
            const Weather* bf_weather = nullptr;
            world.query<const Weather&>().each([&](const Weather& c) {
                if (!bf_weather) bf_weather = &c;
            });
            const bool raining =
                bf_weather && (bf_weather->current == WeatherKind::Rain || bf_weather->current == WeatherKind::Storm);
            const f32 rain = raining ? bf_weather->intensity : 0.0f;
            // the night fade as the pollen block derives it; no clock (time_of_day < 0): 0
            const Environment3D bf_env = resolve_environment(world);
            f32 night_fade = 0.0f;
            if (bf_env.time_of_day >= 0.0f) {
                const f32 elev = -render::sun_direction_at(bf_env.time_of_day).y;
                const f32 t = clamp((elev + 0.1f) / 0.2f, 0.0f, 1.0f);
                night_fade = 1.0f - t * t * (3.0f - 2.0f * t);
            }
            const WindGust* bf_gust = nullptr;
            world.query<const WindGust&>().each([&](const WindGust& c) {
                if (!bf_gust) bf_gust = &c;
            });
            // As the dust block: no Weather is calm, the gust sway bit.
            const f32 wind = bf_weather ? effective_wind(*bf_weather, bf_gust, kGustSway) : 0.0f;
            const f32 amount = butterfly_amount(*bf, bf_season, phase, night_fade, wind, rain);
            if (amount > 0.0f) {
                const ButterflyLook look = butterfly_look(*bf, amount);
                const f32 pscale = particle_scale_of(quality_);
                if (look.active && pscale > 0.0f) {
                    render::ParticleEmitter em;
                    em.id = kButterflyEmitterId;
                    em.max_particles = std::clamp(bf->max_particles, 8u, 512u);
                    em.max_particles = scaled_budget(em.max_particles, pscale);
                    em.seed = bf->seed;
                    em.position = scene.view().position;
                    em.box = math::vec3{look.box, look.height, look.box};
                    em.direction = look.direction;
                    em.spread = look.spread;
                    em.rate = look.rate * pscale;
                    em.burst = 0;
                    em.emitting = true;
                    em.speed_min = look.speed_min;
                    em.speed_max = look.speed_max;
                    em.lifetime_min = look.lifetime_min;
                    em.lifetime_max = look.lifetime_max;
                    em.gravity = look.gravity;
                    em.drag = look.drag;
                    em.collide = false;
                    em.size_start = look.size;
                    em.size_end = look.size;
                    em.color_start = look.color_start;
                    em.color_end = look.color_end;
                    em.additive = false;
                    scene.add_particle_emitter(em);
                    ++stats_.particle_emitters;
                }
            }
        }
    }
    {  // dusk bats (M-BAT 30b): one alpha-blended render-side emitter around the camera; nothing is written, nothing added at amount 0
        const DuskBat* bat = nullptr;
        world.query<const DuskBat&>().each([&](const DuskBat& c) {
            if (!bat) bat = &c;
        });
        if (bat && bat->enabled != 0 && bat->strength > 0.0f) {
            const Season* bat_season = nullptr;
            world.query<const Season&>().each([&](const Season& c) {
                if (!bat_season) bat_season = &c;
            });
            u32 day = 0;
            f32 hour = 0.0f;
            bool have_clock = false;
            world.query<const WorldClock&>().each([&](const WorldClock& c) {
                if (have_clock) return;
                have_clock = true;
                day = c.day;
                hour = c.hour;
            });
            const f32 phase = bat_season ? season_phase(*bat_season, day, hour) : 0.0f;
            const Weather* bat_weather = nullptr;
            world.query<const Weather&>().each([&](const Weather& c) {
                if (!bat_weather) bat_weather = &c;
            });
            const bool raining =
                bat_weather && (bat_weather->current == WeatherKind::Rain || bat_weather->current == WeatherKind::Storm);
            const f32 rain = raining ? bat_weather->intensity : 0.0f;
            // the night fade as the pollen block derives it; no clock (time_of_day < 0): 0
            const Environment3D bat_env = resolve_environment(world);
            f32 night_fade = 0.0f;
            if (bat_env.time_of_day >= 0.0f) {
                const f32 elev = -render::sun_direction_at(bat_env.time_of_day).y;
                const f32 t = clamp((elev + 0.1f) / 0.2f, 0.0f, 1.0f);
                night_fade = 1.0f - t * t * (3.0f - 2.0f * t);
            }
            const WindGust* bat_gust = nullptr;
            world.query<const WindGust&>().each([&](const WindGust& c) {
                if (!bat_gust) bat_gust = &c;
            });
            // As the dust block: no Weather is calm, the gust sway bit.
            const f32 wind = bat_weather ? effective_wind(*bat_weather, bat_gust, kGustSway) : 0.0f;
            const f32 amount = dusk_bat_amount(*bat, bat_season, phase, night_fade, wind, rain);
            if (amount > 0.0f) {
                const DuskBatLook look = dusk_bat_look(*bat, amount);
                const f32 pscale = particle_scale_of(quality_);
                if (look.active && pscale > 0.0f) {
                    render::ParticleEmitter em;
                    em.id = kDuskBatEmitterId;
                    em.max_particles = std::clamp(bat->max_particles, 8u, 512u);
                    em.max_particles = scaled_budget(em.max_particles, pscale);
                    em.seed = bat->seed;
                    em.position = scene.view().position;
                    em.box = math::vec3{look.box, look.height, look.box};
                    em.direction = look.direction;
                    em.spread = look.spread;
                    em.rate = look.rate * pscale;
                    em.burst = 0;
                    em.emitting = true;
                    em.speed_min = look.speed_min;
                    em.speed_max = look.speed_max;
                    em.lifetime_min = look.lifetime_min;
                    em.lifetime_max = look.lifetime_max;
                    em.gravity = look.gravity;
                    em.drag = look.drag;
                    em.collide = false;
                    em.size_start = look.size;
                    em.size_end = look.size;
                    em.color_start = look.color_start;
                    em.color_end = look.color_end;
                    em.additive = false;
                    scene.add_particle_emitter(em);
                    ++stats_.particle_emitters;
                }
            }
        }
    }
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
