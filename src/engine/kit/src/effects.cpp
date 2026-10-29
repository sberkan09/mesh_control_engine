#include "mce/kit/effects.h"

#include <cmath>
#include <utility>

#include "mce/asset/asset_system.h"
#include "mce/asset/formats.h"
#include "mce/audio/components.h"
#include "mce/ecs/hierarchy.h"
#include "mce/ecs/transform.h"
#include "mce/ecs/world.h"
#include "mce/input/actions.h"
#include "mce/kit/bodies3d.h"
#include "mce/kit/cinematic.h"
#include "mce/kit/components.h"
#include "mce/kit/kit3d.h"
#include "mce/kit/particles.h"
#include "mce/kit/pathfinding.h"
#include "mce/kit/vehicles3d.h"
#include "mce/log.h"
#include "mce/physics/components.h"
#include "mce/render/camera2d.h"

namespace mce::kit {

// Padding-free: World::state_hash hashes component bytes.
static_assert(sizeof(SfxTrigger) == 48);
static_assert(sizeof(ParticleBurst) == 12);
static_assert(sizeof(ShakeTrigger) == 12);
static_assert(sizeof(CameraShake2D) == 40);
static_assert(sizeof(CameraFollow2D) == 60);
static_assert(sizeof(CameraFollow3D) == 120);
static_assert(sizeof(PhysicsEvents) == 56);
static_assert(sizeof(NavGridSource) == 40);

namespace {

struct EffectsConfig {
    asset::AssetSystem* assets = nullptr;
};

// The entity's world position: WorldTransform when the hierarchy wrote one, else LocalTransform.
bool position_of(World& w, Entity e, math::vec3& out) {
    if (e.is_null() || !w.alive(e)) return false;
    if (const WorldTransform* wt = w.try_get<WorldTransform>(e)) {
        out = wt->position;
        return true;
    }
    if (const LocalTransform* lt = w.try_get<LocalTransform>(e)) {
        out = lt->position;
        return true;
    }
    return false;
}

// The entities a tick's 2D physics events name (sorted, unique), for the FireOn conditions.
struct Named {
    Array<Entity> trigger;
    Array<Entity> contact;
    Array<Entity> landed;  // characters that touched down (Landed3D)
    Array<Entity> skid;    // vehicles a wheel of which started to skid (Skid3D)
    Array<Entity> lap;     // vehicles whose driver completed a lap (LapCompleted3D)
};

void add_unique(Array<Entity>& list, Entity e) {
    for (const Entity x : list)
        if (x == e) return;
    list.push_back(e);
}

bool named(const Array<Entity>& list, Entity e) {
    for (const Entity x : list)
        if (x == e) return true;
    return false;
}

// Whether a trigger component fires this tick; clears `fire`.
bool should_fire(FireOn on, bool& fire, u32 fired, Entity e, const Named& n) {
    bool go = fire;
    fire = false;
    switch (on) {
    case FireOn::Manual: break;
    case FireOn::Start: go = go || fired == 0; break;
    case FireOn::TriggerEnter: go = go || named(n.trigger, e); break;
    case FireOn::ContactBegin: go = go || named(n.contact, e); break;
    case FireOn::Landed: go = go || named(n.landed, e); break;
    case FireOn::Skid: go = go || named(n.skid, e); break;
    case FireOn::Lap: go = go || named(n.lap, e); break;
    }
    return go;
}

// A value in [-1, 1] from (entity, play count): jitter that replays identically.
f32 jitter(Entity e, u32 n) {
    u32 h = e.index * 0x9E3779B1u ^ (n + 0x7F4A7C15u) * 0x85EBCA77u;
    h ^= h >> 15;
    h *= 0x2C1B3C6Du;
    h ^= h >> 12;
    return static_cast<f32>(h & 0xFFFFu) / 32767.5f - 1.0f;
}

void record_physics_events(Frame& f, Named& n) {
    World& w = f.world();
    auto events = f.events();
    if (w.event_queue<physics::TriggerEnter>()) {
        for (const physics::TriggerEnter& ev : events.read<physics::TriggerEnter>()) {
            add_unique(n.trigger, ev.sensor);
            add_unique(n.trigger, ev.other);
            for (const auto& [self, other] : {std::pair{ev.sensor, ev.other}, std::pair{ev.other, ev.sensor}})
                if (PhysicsEvents* p = w.try_get_mut<PhysicsEvents>(self)) {
                    p->last_trigger = other;
                    ++p->trigger_enters;
                    ++p->overlapping;
                }
        }
        for (const physics::TriggerExit& ev : events.read<physics::TriggerExit>())
            for (const auto& [self, other] : {std::pair{ev.sensor, ev.other}, std::pair{ev.other, ev.sensor}})
                if (PhysicsEvents* p = w.try_get_mut<PhysicsEvents>(self)) {
                    p->last_trigger = other;
                    ++p->trigger_exits;
                    if (p->overlapping > 0) --p->overlapping;
                }
        for (const physics::ContactBegin2D& ev : events.read<physics::ContactBegin2D>()) {
            add_unique(n.contact, ev.a);
            add_unique(n.contact, ev.b);
            for (const bool is_a : {true, false})
                if (PhysicsEvents* p = w.try_get_mut<PhysicsEvents>(is_a ? ev.a : ev.b)) {
                    p->last_contact = is_a ? ev.b : ev.a;
                    p->contact_point = ev.point;
                    p->contact_normal = is_a ? ev.normal : ev.normal * -1.0f;
                    p->contact_speed = ev.approach_speed;
                    ++p->contact_begins;
                    ++p->touching;
                }
        }
        for (const physics::ContactEnd2D& ev : events.read<physics::ContactEnd2D>())
            for (const Entity self : {ev.a, ev.b})
                if (PhysicsEvents* p = w.try_get_mut<PhysicsEvents>(self)) {
                    ++p->contact_ends;
                    if (p->touching > 0) --p->touching;
                }
    }
    if (w.event_queue<Skid3D>())
        for (const Skid3D& ev : events.read<Skid3D>()) add_unique(n.skid, ev.vehicle);
    if (w.event_queue<LapCompleted3D>())
        for (const LapCompleted3D& ev : events.read<LapCompleted3D>()) add_unique(n.lap, ev.vehicle);
    if (w.event_queue<Landed3D>())
        for (const Landed3D& ev : events.read<Landed3D>()) add_unique(n.landed, ev.entity);
    if (w.event_queue<ContactBegin3D>())
        for (const ContactBegin3D& ev : events.read<ContactBegin3D>()) {
            add_unique(n.contact, ev.a);
            add_unique(n.contact, ev.b);
            for (const bool is_a : {true, false})
                if (PhysicsEvents* p = w.try_get_mut<PhysicsEvents>(is_a ? ev.a : ev.b)) {
                    p->last_contact = is_a ? ev.b : ev.a;
                    p->contact_speed = ev.approach_speed;
                    ++p->contact_begins;
                }
        }
    if (w.event_queue<TriggerEnter3D>()) {
        for (const TriggerEnter3D& ev : events.read<TriggerEnter3D>()) {
            add_unique(n.trigger, ev.trigger);
            add_unique(n.trigger, ev.other);
            for (const auto& [self, other] : {std::pair{ev.trigger, ev.other}, std::pair{ev.other, ev.trigger}})
                if (PhysicsEvents* p = w.try_get_mut<PhysicsEvents>(self)) {
                    p->last_trigger = other;
                    ++p->trigger_enters;
                    ++p->overlapping;
                }
        }
        for (const TriggerExit3D& ev : events.read<TriggerExit3D>())
            for (const auto& [self, other] : {std::pair{ev.trigger, ev.other}, std::pair{ev.other, ev.trigger}})
                if (PhysicsEvents* p = w.alive(self) ? w.try_get_mut<PhysicsEvents>(self) : nullptr) {
                    p->last_trigger = other;
                    ++p->trigger_exits;
                    if (p->overlapping > 0) --p->overlapping;
                }
    }
}

void kit_effects(Frame& f) {
    World& w = f.world();
    // Registered to run on held ticks for UiFlow::keep_audio: a pause screen that keeps the sound going lets the
    // Sfx triggers fire (and nothing else); any other pause holds the whole system as before.
    bool sfx_only = false;
    if (w.paused()) {
        const audio::PauseMix* pause = w.try_resource<audio::PauseMix>();
        if (!pause || !pause->keep) return;
        sfx_only = true;
    }
    Named n;
    if (!sfx_only) record_physics_events(f, n);

    const bool audio = w.event_queue<audio::PlaySfx>() != nullptr;
    f.query<SfxTrigger>().each([&](Entity e, SfxTrigger& s) {
        if (!should_fire(s.on, s.fire, s.fired, e, n)) return;
        if (audio && s.clip.guid != asset::Guid{}) {
            audio::PlaySfx ev;
            ev.clip = s.clip.guid;
            ev.gain_db = s.gain_db + s.gain_jitter_db * jitter(e, s.fired);
            ev.pan = s.pan;
            ev.bus = static_cast<audio::BusId>(s.bus);
            f.events().push(ev);
        }
        ++s.fired;
    });
    if (sfx_only) return;

    ParticleSystem* particles = w.try_resource<ParticleSystem>();
    f.query<ParticleBurst>().each([&](Entity e, ParticleBurst& b) {
        if (!should_fire(b.on, b.fire, b.fired, e, n)) return;
        if (particles) particles->burst(e, b.count);
        ++b.fired;
    });

    f32 trauma = 0.0f;
    f.query<ShakeTrigger>().each([&](Entity e, ShakeTrigger& s) {
        if (!should_fire(s.on, s.fire, s.fired, e, n)) return;
        trauma += s.trauma;
        ++s.fired;
    });
    f.query<CameraShake2D>().each([&](CameraShake2D& s) {
        s.trauma = math::clamp(s.trauma + trauma, 0.0f, 1.0f);
        s.time += f.dt;
        s.trauma = math::max(0.0f, s.trauma - s.decay_per_second * f.dt);
        const render::ShakeOffset o = render::shake_offset(s.seed, s.time, s.trauma, s.max_translation, s.max_rotation, s.frequency_hz);
        s.offset = o.translation;
        s.offset_rotation = o.rotation;
    });
    f.query<CameraShake3D>().each([&](CameraShake3D& s) {
        s.trauma = math::clamp(s.trauma + trauma, 0.0f, 1.0f);
        s.time += f.dt;
        s.trauma = math::max(0.0f, s.trauma - s.decay_per_second * f.dt);
        // Two noise streams: one moves the view (x, y) and rolls it, the other pitches and yaws it.
        const render::ShakeOffset a = render::shake_offset(s.seed, s.time, s.trauma, s.max_translation, s.max_rotation, s.frequency_hz);
        const render::ShakeOffset b = render::shake_offset(s.seed * 2654435761u + 1u, s.time, s.trauma, s.max_rotation, 0.0f, s.frequency_hz);
        s.offset = {a.translation.x, a.translation.y, 0.0f};
        s.offset_rotation = {b.translation.y, b.translation.x, a.rotation};
    });
}

// The fraction of the remaining distance a smoothed follow covers this tick (1 = snap).
f32 follow_blend(f32 smoothing, f32 dt) { return smoothing <= 0.0f ? 1.0f : 1.0f - std::exp(-smoothing * dt); }

void kit_camera_follow(Frame& f) {
    World& w = f.world();
    f.query<CameraFollow2D, LocalTransform>().each([&](CameraFollow2D& c, LocalTransform& t) {
        math::vec3 p3;
        if (!position_of(w, c.target, p3)) return;
        const math::vec2 target{p3.x, p3.y};
        const math::vec2 velocity = c.primed && f.dt > 0.0f ? (target - c.last_target) * (1.0f / f.dt) : math::vec2{};
        c.last_target = target;
        const math::vec2 goal = target + c.offset + velocity * c.look_ahead;
        math::vec2 cam{t.position.x, t.position.y};
        if (!c.primed) {
            cam = goal;  // the first tick starts on the target
            c.primed = true;
        } else {
            // Dead zone: the camera only takes up the part of the move beyond the zone's edge.
            math::vec2 desired = cam;
            const math::vec2 d = goal - cam;
            if (std::fabs(d.x) > c.dead_zone.x) desired.x += d.x - (d.x > 0.0f ? c.dead_zone.x : -c.dead_zone.x);
            if (std::fabs(d.y) > c.dead_zone.y) desired.y += d.y - (d.y > 0.0f ? c.dead_zone.y : -c.dead_zone.y);
            cam = cam + (desired - cam) * follow_blend(c.smoothing, f.dt);
        }
        if (c.use_bounds) {
            cam.x = math::clamp(cam.x, c.bounds_min.x, math::max(c.bounds_min.x, c.bounds_max.x));
            cam.y = math::clamp(cam.y, c.bounds_min.y, math::max(c.bounds_min.y, c.bounds_max.y));
        }
        t.position.x = cam.x;
        t.position.y = cam.y;
    });

    const input::InputState* in = w.try_resource<input::InputState>();
    Physics3D* phys = w.try_resource<Physics3D>();
    f.query<CameraFollow3D, LocalTransform>().each([&](CameraFollow3D& c, LocalTransform& t) {
        math::vec3 p;
        if (!position_of(w, c.target, p)) return;
        math::vec3 foe;
        const bool locked = position_of(w, c.lock_on, foe);
        if (locked) {
            // Behind the target on the line from the locked entity: the target in front, it beyond.
            const f32 dx = p.x - foe.x, dz = p.z - foe.z;
            if (dx * dx + dz * dz > 1e-8f) {
                f32 turn = std::atan2(dx, dz) - c.yaw;
                turn -= math::kTau * std::floor((turn + math::kPi) / math::kTau);  // the short way round
                const f32 most = c.lock_on_rate * f.dt;
                c.yaw += c.lock_on_rate > 0.0f ? math::clamp(turn, -most, most) : turn;
            }
        }
        if (in && c.orbit_speed > 0.0f) {
            if (!locked) c.yaw -= in->look.x * c.orbit_speed * f.dt;
            c.pitch += in->look.y * c.orbit_speed * f.dt;
        }
        // Chase: ease the yaw behind where the target is going, unless the player has just used the Look input.
        c.chase_idle = in && c.orbit_speed > 0.0f && std::fabs(in->look.x) > 0.1f ? 0.0f : c.chase_idle + f.dt;
        if (c.chase && !locked && c.primed && f.dt > 0.0f && c.chase_idle >= c.chase_hold) {
            const f32 vx = (p.x + c.offset.x - c.last_goal.x) / f.dt, vz = (p.z + c.offset.z - c.last_goal.z) / f.dt;
            const Vehicle3D* car = w.try_get<Vehicle3D>(c.target);
            const bool drives = car ? car->speed > 0.0f : c.chase == 2;
            if (drives && vx * vx + vz * vz >= c.chase_min_speed * c.chase_min_speed) {
                f32 turn = std::atan2(-vx, -vz) - c.yaw;
                turn -= math::kTau * std::floor((turn + math::kPi) / math::kTau);  // the short way round
                c.yaw += turn * follow_blend(c.chase_rate, f.dt);
            }
        }
        c.pitch = math::clamp(c.pitch, c.min_pitch, math::max(c.min_pitch, c.max_pitch));
        const math::vec3 raw = p + c.offset;
        math::vec3 goal = raw;
        if (c.primed && c.look_ahead > 0.0f) {
            const math::vec3 v = (raw - c.last_goal) * (1.0f / f.dt);
            goal = goal + math::vec3{v.x, 0.0f, v.z} * c.look_ahead;
        }
        c.last_goal = raw;
        const bool fresh = !c.primed;
        if (!c.primed || c.first_person) {
            c.pivot = goal;
            c.primed = true;
        } else {
            c.pivot = c.pivot + (goal - c.pivot) * follow_blend(c.smoothing, f.dt);
        }
        // Yaw about +Y, then pitch down about the camera's X: forward (-Z) points at the pivot.
        const math::quat q = math::normalize(math::quat_from_axis_angle({0.0f, 1.0f, 0.0f}, c.yaw) *
                                             math::quat_from_axis_angle({1.0f, 0.0f, 0.0f}, -c.pitch));
        t.rotation = q;
        if (c.first_person) {
            c.arm = 0.0f;
            t.position = c.pivot;
            return;
        }
        // The spring arm: a wall pulls the camera in at once; it eases back out at return_speed.
        f32 reach = c.distance;
        const math::vec3 back = math::rotate(q, math::vec3{0.0f, 0.0f, 1.0f});
        if (c.collide && phys)
            if (const auto hit = phys->world.camera_sweep(c.pivot, c.pivot + back * c.distance, c.collision_radius))
                reach = math::clamp(math::dot(hit->position - c.pivot, back), 0.0f, c.distance);
        if (fresh || !c.collide || reach <= c.arm || c.return_speed <= 0.0f) c.arm = reach;
        else c.arm = math::min(reach, c.arm + c.return_speed * f.dt);
        t.position = c.pivot + back * c.arm;
    });
}

// ---- nav grid

bool load_tilemap_grid(World& w, const EffectsConfig& cfg, Entity layer_entity, NavGrid& out) {
    const TilemapLayer* layer = w.alive(layer_entity) ? w.try_get<TilemapLayer>(layer_entity) : nullptr;
    if (!layer) {
        MCE_LOG_WARN("kit: NavGridSource.tilemap is not a TilemapLayer entity; using the box grid");
        return false;
    }
    if (!cfg.assets || layer->map.guid == asset::Guid{}) return false;
    const asset::AssetKey key = cfg.assets->find(layer->map.guid);
    if (key == asset::kInvalidAssetKey) return false;
    Result<asset::AssetHandle<asset::Tilemap>> h = cfg.assets->load_sync<asset::Tilemap>(key);
    if (!h) return false;
    Result<asset::TilemapView> view = asset::TilemapView::read(cfg.assets->resolve(h.value()));
    math::vec3 at;
    position_of(w, layer_entity, at);
    const bool ok = static_cast<bool>(view);
    if (ok) out = nav_grid_from_tilemap(view.value(), {at.x, at.y});
    cfg.assets->release(asset::AnyHandle(h.value()));
    return ok;
}

void build_from_source(World& w, const EffectsConfig& cfg, NavGridSource& s) {
    NavGrid grid;
    if (s.tilemap.is_null() || !load_tilemap_grid(w, cfg, s.tilemap, grid))
        grid = NavGrid(math::max(1u, s.width), math::max(1u, s.height), math::max(0.05f, s.cell_size), s.origin);
    if (s.colliders) mark_static_colliders(grid, w, s.inflate);
    u32 blocked = 0;
    for (const u8 c : grid.costs()) blocked += c == NavGrid::kBlocked ? 1u : 0u;
    s.blocked = blocked;
    w.set_resource(static_cast<NavGrid&&>(grid));
}

// No NavGridSource: a grid over the static colliders, the agents and their targets.
void build_auto(World& w) {
    math::aabb2 box{{1e30f, 1e30f}, {-1e30f, -1e30f}};
    bool any_agent = false;
    w.query<const PathAgent, const LocalTransform>().each([&](const PathAgent& a, const LocalTransform& t) {
        any_agent = true;
        for (const math::vec2 p : {t.position.xy(), a.target}) {
            box.min.x = math::min(box.min.x, p.x);
            box.min.y = math::min(box.min.y, p.y);
            box.max.x = math::max(box.max.x, p.x);
            box.max.y = math::max(box.max.y, p.y);
        }
    });
    if (!any_agent) return;
    const math::aabb2 walls = static_collider_bounds(w);
    if (walls.min.x <= walls.max.x) {
        box.min = {math::min(box.min.x, walls.min.x), math::min(box.min.y, walls.min.y)};
        box.max = {math::max(box.max.x, walls.max.x), math::max(box.max.y, walls.max.y)};
    }
    constexpr f32 kMargin = 4.0f;
    constexpr u32 kMaxCells = 512;
    const math::vec2 origin = box.min - math::vec2(kMargin);
    const math::vec2 size = box.max - box.min + math::vec2(2.0f * kMargin);
    const f32 cell = math::max(0.5f, math::max(size.x, size.y) / static_cast<f32>(kMaxCells));
    NavGrid grid(static_cast<u32>(std::ceil(size.x / cell)), static_cast<u32>(std::ceil(size.y / cell)), cell, origin);
    mark_static_colliders(grid, w, 0.25f);
    w.set_resource(static_cast<NavGrid&&>(grid));
}

void kit_nav_grid(Frame& f) {
    World& w = f.world();
    const EffectsConfig& cfg = w.resource<EffectsConfig>();
    bool has_source = false;
    f.query<NavGridSource>().each([&](NavGridSource& s) {
        if (has_source) return;
        has_source = true;
        if (s.rebuild || !w.try_resource<NavGrid>()) build_from_source(w, cfg, s);
        s.rebuild = false;
    });
    if (!has_source && !w.try_resource<NavGrid>()) build_auto(w);
}

}  // namespace

void install_effects(World& world, asset::AssetSystem* assets) {
    if (EffectsConfig* cfg = world.try_resource<EffectsConfig>()) {
        cfg->assets = assets;
        return;
    }
    world.set_resource(EffectsConfig{assets});
    world.register_components<SfxTrigger, ParticleBurst, ShakeTrigger, CameraShake2D, CameraFollow2D, CameraFollow3D,
                              PhysicsEvents, NavGridSource>();
    world.add_system(Phase::FixedUpdate_Pre, "kit_nav_grid", &kit_nav_grid);
    install_path_agents(world);
    world.add_system(Phase::FixedUpdate_Post, "kit_effects", &kit_effects);
    world.add_system(Phase::FixedUpdate_Post, "kit_camera_follow", &kit_camera_follow);
    world.run_during_cutscenes(Phase::FixedUpdate_Post, "kit_effects");
    world.run_when_held(Phase::FixedUpdate_Post, "kit_effects");
    world.run_during_cutscenes(Phase::FixedUpdate_Post, "kit_camera_follow");
}

}  // namespace mce::kit
