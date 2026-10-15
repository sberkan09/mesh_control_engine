#include "mce/kit/kit3d_extract.h"

#include <cmath>

#include "mce/asset/formats.h"
#include "mce/ecs/hierarchy.h"
#include "mce/ecs/transform.h"
#include "mce/log.h"
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
    release_handles();
    meshes_ = nullptr;
    assets_ = nullptr;
}

void Kit3DExtract::reset(asset::AssetSystem* assets) {
    release_handles();
    foliage_.clear();
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
            m.albedo_key = texture_key(m.params.texture(asset::MaterialSlot::BaseColor));
            m.normal_key = texture_key(m.params.texture(asset::MaterialSlot::Normal));
            m.orm_key = texture_key(m.params.texture(asset::MaterialSlot::MetallicRoughness));
            m.emissive_key = texture_key(m.params.texture(asset::MaterialSlot::Emissive));
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
    world.query<const Camera3DComponent&>().each([&](Entity e, const Camera3DComponent& c) {
        if (found || !c.primary) return;
        found = view_of(world, e, c, alpha, out);
        if (found && component) *component = c;
    });
    return found;
}

bool Kit3DExtract::camera_of(World& world, Entity e, f32 alpha, render::Camera3D& out) {
    const Camera3DComponent* c = world.alive(e) ? world.try_get<Camera3DComponent>(e) : nullptr;
    return c && view_of(world, e, *c, alpha, out);
}

bool Kit3DExtract::view_of(World& world, Entity e, const Camera3DComponent& c, f32 alpha, render::Camera3D& out) {
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
    scene.set_exposure(env.exposure);
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
    scene.set_post(post);
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
    world_parity_ ^= 1u;
    ++frame_stamp_;

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

    world.query<const MeshRef&>().each([&](Entity e, const MeshRef& m) {
        if (!m.visible) return;
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
            for (Array<mat4>& w : worlds_) w.resize(e.index + 1, mat4::identity());
        }
        inst.tint = m.tint;
        inst.emissive_tint = m.emissive_tint;
        inst.cast_shadows = m.cast_shadows != 0;
        inst.skinning = m.skinning != 0;
        inst.material = material(m.material.guid);
        if (const Animator* a = world.try_get<Animator>(e); a && a->playing != 0 && !mesh->nodes.empty()) {
            render::ClipSample samples[3];
            const usize n = anim_samples(*a, *mesh, [&](u64 id) { return find_clip_id(*mesh, id); }, samples);
            if (n > 0) {
                const usize first = poses_.size();
                poses_.resize(first + mesh->nodes.size());
                pose_samples(*mesh, samples, n, Span<mat4>(poses_.data() + first, mesh->nodes.size()));
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
        if (world_stamp_[ent] + 1 == frame_stamp_) inst.prev_world = &worlds_[world_parity_ ^ 1u][ent];
        worlds_[world_parity_][ent] = inst.world;
        world_stamp_[ent] = frame_stamp_;
        scene.add(inst);
    }

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
    if (!bake_mode_) time_ += max(frame_dt_, 0.0f);
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
    for (usize i = 0; i < foliage_.size();) {  // gone, disabled or destroyed: its scatter is dropped
        if (foliage_[i].stamp == frame_stamp_) {
            ++i;
            continue;
        }
        foliage_[i] = std::move(foliage_.back());
        foliage_.pop_back();
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
