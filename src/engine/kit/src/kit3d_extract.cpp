#include "mce/kit/kit3d_extract.h"

#include <cmath>

#include "mce/asset/formats.h"
#include "mce/ecs/hierarchy.h"
#include "mce/ecs/transform.h"
#include "mce/log.h"
#include "mce/string_id.h"

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

}  // namespace

void Kit3DExtract::init(const render::MeshStore& meshes, asset::AssetSystem* assets) {
    meshes_ = &meshes;
    assets_ = assets;
}

void Kit3DExtract::release_handles() {
    if (assets_) {
        for (const MeshEntry& m : mesh_entries_)
            if (m.handle.generation != 0) assets_->release(m.handle);
        for (const TextureEntry& t : texture_entries_)
            if (t.handle.generation != 0) assets_->release(t.handle);
        for (const asset::AnyHandle& h : preloaded_) assets_->release(h);
    }
    mesh_entries_.clear();
    texture_entries_.clear();
    preloaded_.clear();
    tracked_.clear();
}

void Kit3DExtract::shutdown() {
    release_handles();
    meshes_ = nullptr;
    assets_ = nullptr;
}

void Kit3DExtract::reset(asset::AssetSystem* assets) {
    release_handles();
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

bool Kit3DExtract::camera(World& world, f32 alpha, render::Camera3D& out, Camera3DComponent* component) {
    out = render::Camera3D{};
    bool found = false;
    const u64 tick = world.current_tick();
    world.query<const Camera3DComponent&>().each([&](Entity e, const Camera3DComponent& c) {
        if (found || !c.primary) return;
        vec3 pos, scale;
        quat rot;
        if (!pose_of(world, e, pos, rot, scale)) return;
        found = true;
        if (component) *component = c;
        track(e, pos, rot, scale, tick);
        interpolated(e, alpha, pos, rot, scale);
        out.position = pos;
        out.orientation = normalize(rot);
        out.fov_y = radians(clamp(c.fov_deg, 1.0f, 179.0f));
        out.near_z = max(c.near_z, 1e-4f);
        out.far_z = max(c.far_z, out.near_z * 2.0f);
    });
    return found;
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
    render::DirectionalLight sun;
    sun.direction = env.sun_direction;
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
    scene.set_sky(sky);
    render::Fog fog;
    fog.color = env.fog_color;
    fog.density = env.fog_density;
    scene.set_fog(fog);
    scene.set_exposure(env.exposure);
    render::PostFx post;
    post.bloom = env.bloom != 0;
    post.bloom_intensity = env.bloom_intensity;
    post.bloom_threshold = env.bloom_threshold;
    post.fxaa = env.fxaa != 0;
    scene.set_post(post);
    render::ShadowSettings shadows;
    shadows.cascades = clamp(env.shadow_cascades, 1u, render::kMaxCascades);
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
    stats_ = {};
    apply_environment(world, scene);
    const render::View3D& view = scene.view();
    const u64 tick = world.current_tick();
    // Poses go to poses_ (the scene keeps spans into it until the next extract); offsets are
    // turned into spans once every MeshRef is in, as poses_ may grow meanwhile.
    poses_.clear();
    pending_.clear();
    pending_entity_.clear();
    pose_first_.clear();

    world.query<const MeshRef&>().each([&](Entity e, const MeshRef& m) {
        if (!m.visible) return;
        vec3 pos, scale;
        quat rot;
        if (!pose_of(world, e, pos, rot, scale)) return;
        if (m.interpolate) {
            track(e, pos, rot, scale, tick);
            interpolated(e, alpha, pos, rot, scale);
        }
        if (m.cull_radius > 0.0f &&
            !render::sphere_visible(view, pos, m.cull_radius * max(scale.x, max(scale.y, scale.z))))
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
        inst.tint = m.tint;
        inst.emissive_tint = m.emissive_tint;
        inst.cast_shadows = m.cast_shadows != 0;
        inst.skinning = m.skinning != 0;
        if (const Animator* a = world.try_get<Animator>(e); a && a->playing != 0 && !mesh->nodes.empty()) {
            const u32 clip = find_clip_id(*mesh, a->playing);
            if (clip != ~0u) {
                const usize first = poses_.size();
                poses_.resize(first + mesh->nodes.size());
                const Span<mat4> out(poses_.data() + first, mesh->nodes.size());
                const u32 from = a->previous != 0 && a->blend < 1.0f ? find_clip_id(*mesh, a->previous) : ~0u;
                if (from != ~0u) {
                    const render::ClipSample samples[2] = {{from, a->previous_time + a->phase, 1.0f - a->blend},
                                                           {clip, a->time + a->phase, a->blend}};
                    render::pose_nodes_blend(*mesh, Span<const render::ClipSample>(samples, 2), out);
                } else {
                    render::pose_nodes(*mesh, clip, a->time + a->phase, out);
                }
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
        if (meshes_->get(inst.mesh)->lod_levels > 1) inst.lod_state = &lod_states_[pending_entity_[i]];
        scene.add(inst);
    }

    world.query<const PointLight3D&>().each([&](Entity e, const PointLight3D& l) {
        if (!l.enabled || l.intensity <= 0.0f) return;
        vec3 pos, scale;
        quat rot;
        if (!pose_of(world, e, pos, rot, scale)) return;
        render::PointLight pl;
        pl.position = pos + rotate(rot, l.offset);
        if (!render::sphere_visible(view, pl.position, l.radius)) return;
        pl.color = {l.color.r, l.color.g, l.color.b};
        pl.intensity = l.intensity;
        pl.radius = l.radius;
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
        if (!render::sphere_visible(view, pl.position, l.radius)) return;
        pl.color = {l.color.r, l.color.g, l.color.b};
        pl.intensity = l.intensity;
        pl.radius = l.radius;
        const vec3 d = rotate(rot, l.direction);
        pl.direction = length_sq(d) > 0.0f ? normalize(d) : vec3{0.0f, -1.0f, 0.0f};
        pl.spot_outer = radians(clamp(l.outer_deg, 0.5f, 89.0f));
        pl.spot_inner = radians(clamp(l.inner_deg, 0.0f, l.outer_deg));
        scene.add_point_light(pl);
        ++stats_.spot_lights;
    });

    // Static copies, cell by cell.
    if (const StaticInstances3D* si = world.try_resource<StaticInstances3D>(); si && !si->instances.empty()) {
        Array<u32>& keys = static_keys_;
        if (static_lods_.size() != si->instances.size()) static_lods_.resize(si->instances.size(), 0);
        keys.resize(si->meshes.size());
        for (u32 i = 0; i < si->meshes.size(); ++i) keys[i] = mesh_key(si->meshes[i]);
        for (const StaticCell3D& c : si->cells) {
            if (!render::sphere_visible(view, c.center, c.radius)) continue;
            ++stats_.cells_visible;
            const f32 dist = length(c.center - view.position);
            for (u32 i = c.first; i < c.first + c.count && i < si->instances.size(); ++i) {
                const StaticInstance3D& s = si->instances[i];
                if (s.mesh >= si->meshes.size() || keys[s.mesh] == ~0u) continue;
                if (s.max_distance > 0.0f && dist > s.max_distance) continue;
                if (s.thin_distance > 0.0f && dist > s.thin_distance && (i & 1u)) continue;
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
