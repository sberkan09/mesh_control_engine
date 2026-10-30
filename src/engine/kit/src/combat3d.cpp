#include "mce/kit/combat3d.h"

#include <cmath>

#include "kit3d_internal.h"
#include "mce/containers/array.h"
#include "mce/ecs/hierarchy.h"
#include "mce/ecs/transform.h"
#include "mce/ecs/world.h"
#include "mce/input/actions.h"
#include "mce/kit/bodies3d.h"
#include "mce/kit/kit3d.h"
#include "mce/kit/nav3d.h"
#include "mce/kit/ragdolls3d.h"
#include "mce/kit/seats3d.h"
#include "mce/string_id.h"

namespace mce::kit {

// Padding-free: World::state_hash hashes component bytes.
static_assert(sizeof(Health3D) == 64);
static_assert(sizeof(Weapon3D) == 176);
static_assert(sizeof(Projectile3D) == 44);

namespace {

struct Combat3DState {
    u8 installed = 1;
};

constexpr StringId kFire{"Fire"};
constexpr StringId kReload{"Reload"};
constexpr f32 kTwoPi = 6.283185307179586f;
constexpr f32 kDeg = 0.017453292519943295f;

u32 next_random(u32& s) {
    s ^= s << 13;
    s ^= s >> 17;
    s ^= s << 5;
    return s;
}
f32 unit_random(u32& s) { return static_cast<f32>(next_random(s) >> 8) * (1.0f / 16777216.0f); }

// The first of e and its parents that can be hurt.
Entity health_holder(World& w, Entity e) {
    for (u32 i = 0; i < 8 && !e.is_null() && w.alive(e); ++i) {
        if (w.has<Health3D>(e)) return e;
        const ChildOf* c = w.try_get<ChildOf>(e);
        e = c ? c->parent : Entity{};
    }
    return Entity{};
}

u8 team_of(World& w, Entity e) {
    const Health3D* h = e.is_null() ? nullptr : w.try_get<Health3D>(health_holder(w, e));
    return h ? h->team : u8{0};
}

// e and the entities it hangs under.
usize with_parents(World& w, Entity e, Entity* out, usize n, usize cap);

// The skip list of a shooter: e and its parents, and the car it sits in.
usize shooter_skip(World& w, Entity owner, Entity* out, usize cap);

usize with_parents(World& w, Entity e, Entity* out, usize n, usize cap) {
    for (u32 i = 0; i < 8 && !e.is_null() && w.alive(e) && n < cap; ++i) {
        out[n++] = e;
        const ChildOf* c = w.try_get<ChildOf>(e);
        e = c ? c->parent : Entity{};
    }
    return n;
}

usize shooter_skip(World& w, Entity owner, Entity* out, usize cap) {
    usize n = with_parents(w, owner, out, 0, cap);
    const Rider3D* r = w.alive(owner) ? w.try_get<Rider3D>(owner) : nullptr;
    if (r && r->seated) n = with_parents(w, r->vehicle, out, n, cap);
    return n;
}

math::vec3 any_perpendicular(math::vec3 d) {
    const math::vec3 axis = std::fabs(d.y) < 0.9f ? math::vec3{0.0f, 1.0f, 0.0f} : math::vec3{1.0f, 0.0f, 0.0f};
    return math::normalize(math::cross(d, axis));
}

math::vec3 spread(math::vec3 d, f32 half_angle, u32& seed) {
    if (half_angle <= 0.0f) return d;
    const f32 a = half_angle * std::sqrt(unit_random(seed)), phi = kTwoPi * unit_random(seed);
    const math::vec3 u = any_perpendicular(d), v = math::cross(d, u);
    return math::normalize(d * std::cos(a) + (u * std::cos(phi) + v * std::sin(phi)) * std::sin(a));
}

void push_impulse(World& w, Physics3D& phys, Entity e, math::vec3 impulse) {
    if (math::length_sq(impulse) <= 0.0f) return;
    const RigidBody3D* b = w.try_get<RigidBody3D>(e);
    if (b && b->type == BodyType3DKind::Dynamic) phys.world.apply_impulse(e, impulse);
}

// A shot landed on `hit`: the event, the damage, the push.
void land(Frame& f, Physics3D& phys, Entity weapon, Entity owner, u8 team, bool friendly_fire, Entity hit, math::vec3 point,
          math::vec3 normal, math::vec3 dir, f32 damage, f32 impulse) {
    World& w = f.world();
    f.events().push(WeaponHit3D{weapon, owner, hit, point, normal});
    push_impulse(w, phys, hit, dir * impulse);
    const Entity holder = health_holder(w, hit);
    if (holder.is_null()) return;
    if (team != 0 && !friendly_fire && w.try_get<Health3D>(holder)->team == team) return;
    DamageInfo d;
    d.amount = damage;
    d.instigator = owner;
    d.point = point;
    d.direction = dir;
    d.impulse = impulse;
    apply_damage(w, holder, d);
}

void shoot(Frame& f, Physics3D& phys, Entity e, Weapon3D& wp, math::vec3 pos, math::quat rot) {
    World& w = f.world();
    const Entity owner = wp.owner.is_null() || !w.alive(wp.owner) ? e : wp.owner;
    const u8 team = team_of(w, owner);
    const math::vec3 origin = pos + math::rotate(rot, wp.muzzle);
    const math::vec3 aim = math::normalize(math::rotate(rot, wp.direction));
    Entity skip[16];
    const usize n_skip = with_parents(w, e, skip, shooter_skip(w, owner, skip, 16), 16);
    if (wp.seed == 0) wp.seed = (e.index * 2654435761u) | 1u;
    for (u32 i = 0; i < math::max<u32>(wp.pellets, 1u); ++i) {
        const math::vec3 dir = spread(aim, wp.spread_deg * kDeg, wp.seed);
        if (wp.kind == WeaponKind3D::Projectile) {
            Projectile3D p;
            p.owner = owner;
            p.velocity = dir * wp.projectile_speed;
            p.damage = wp.damage;
            p.life = wp.projectile_life;
            p.gravity_scale = wp.projectile_gravity;
            p.splash_radius = wp.splash_radius;
            p.impulse = wp.impulse;
            p.mask = wp.mask;
            p.team = team;
            p.friendly_fire = wp.friendly_fire;
            LocalTransform t;
            t.position = origin;
            t.rotation = math::quat::identity();
            t.scale = {wp.projectile_scale, wp.projectile_scale, wp.projectile_scale};
            if (wp.projectile_mesh.guid.is_null()) {
                f.commands().spawn(t, p);
            } else {
                MeshRef m;
                m.mesh = wp.projectile_mesh;
                m.interpolate = 1;
                f.commands().spawn(t, p, m);
            }
            f.events().push(WeaponFired3D{e, origin, origin + dir, Entity{}});
            continue;
        }
        physics::QueryFilter3D filter;
        filter.layer_mask = wp.mask;
        filter.include_characters = true;
        filter.exclude = Span<const Entity>(skip, n_skip);
        const std::optional<physics::RayHit3D> h = phys.world.raycast(origin, dir, wp.range, filter);
        const math::vec3 end = h ? h->point : origin + dir * wp.range;
        f.events().push(WeaponFired3D{e, origin, end, h ? h->entity : Entity{}});
        if (!h) continue;
        wp.last_hit = h->entity;
        wp.last_point = h->point;
        land(f, phys, e, owner, team, wp.friendly_fire != 0, h->entity, h->point, h->normal, dir, wp.damage, wp.impulse);
    }
}

void finish_reload(Weapon3D& wp) {
    const u32 room = wp.magazine > wp.ammo ? wp.magazine - wp.ammo : 0u;
    const u32 take = wp.infinite_ammo ? room : math::min(room, wp.reserve);
    wp.ammo = static_cast<u16>(wp.ammo + take);
    if (!wp.infinite_ammo) wp.reserve -= take;
    wp.reloading = 0;
    wp.reload_left = 0.0f;
}

// FixedUpdate_Pre, after the bodies: weapons take their input and shoot.
void kit3d_weapons(Frame& f) {
    World& w = f.world();
    Physics3D& phys = f.resource<Physics3D>();
    const input::InputState* in = w.try_resource<input::InputState>();
    f.query<Weapon3D, LocalTransform>().each([&](Entity e, Weapon3D& wp, LocalTransform& t) {
        if (!wp.primed) {
            wp.primed = 1;
            if (wp.ammo == 0) wp.ammo = wp.magazine;
        }
        wp.cooldown = math::max(wp.cooldown - f.dt, -f.dt);  // a little credit carries the fractional tick, none banks shots
        if (wp.reloading) {
            wp.reload_left -= f.dt;
            if (wp.reload_left <= 0.0f) finish_reload(wp);
        }
        bool want_fire = wp.fire != 0, want_reload = wp.reload != 0;
        if (wp.use_input && in) {
            if (in->has_action(kFire)) want_fire = want_fire || (wp.automatic ? in->held(kFire) : in->pressed(kFire));
            if (in->has_action(kReload)) want_reload = want_reload || in->pressed(kReload);
        }
        wp.fire = wp.reload = 0;
        const bool can_refill = wp.magazine > 0 && wp.ammo < wp.magazine && (wp.infinite_ammo || wp.reserve > 0);
        if (!wp.reloading && can_refill && (want_reload || wp.ammo == 0)) {
            wp.reloading = 1;
            wp.reload_left = wp.reload_time;
            if (wp.reload_time <= 0.0f) finish_reload(wp);
        }
        if (!want_fire || wp.reloading || wp.cooldown > 0.0f) return;
        if (wp.magazine > 0) {
            if (wp.ammo == 0) return;
            --wp.ammo;
        }
        wp.cooldown += 1.0f / wp.rate;
        ++wp.shots;
        math::vec3 pos, scale;
        math::quat rot;
        // A weapon carried by a character can take its aim from another entity (the camera); if that is gone it uses its own pose.
        const LocalTransform* from = !wp.aim_from.is_null() && w.alive(wp.aim_from) ? w.try_get<LocalTransform>(wp.aim_from) : nullptr;
        if (from)
            world_pose(w, wp.aim_from, *from, pos, rot, scale);
        else
            world_pose(w, e, t, pos, rot, scale);
        shoot(f, phys, e, wp, pos, rot);
    });
}

// FixedUpdate_Pre, after the weapons: projectiles fly, hit and explode.
void kit3d_projectiles(Frame& f) {
    World& w = f.world();
    Physics3D& phys = f.resource<Physics3D>();
    const math::vec3 gravity = phys.world.config().gravity;
    f.query<Projectile3D, LocalTransform>().each([&](Entity e, Projectile3D& p, LocalTransform& t) {
        p.life -= f.dt;
        p.velocity += gravity * (p.gravity_scale * f.dt);
        const math::vec3 step = p.velocity * f.dt;
        const f32 len = math::length(step);
        if (len <= 1e-6f) {
            if (p.life <= 0.0f) f.commands().destroy(e);
            return;
        }
        const math::vec3 dir = step * (1.0f / len);
        Entity skip[16];
        const usize n_skip = shooter_skip(w, p.owner, skip, 16);
        physics::QueryFilter3D filter;
        filter.layer_mask = p.mask;
        filter.include_characters = true;
        filter.exclude = Span<const Entity>(skip, n_skip);
        const std::optional<physics::RayHit3D> h = phys.world.raycast(t.position, dir, len, filter);
        if (!h) {
            t.position += step;
            if (p.life <= 0.0f) f.commands().destroy(e);
            return;
        }
        land(f, phys, e, p.owner, p.team, p.friendly_fire != 0, h->entity, h->point, h->normal, dir, p.damage, p.impulse);
        if (p.splash_radius > 0.0f) {
            Entity near[32];
            const u32 n = math::min(phys.world.overlap_sphere(h->point, p.splash_radius, Span<Entity>(near, 32), filter), 32u);
            Entity done[33];  // each Health3D is hurt once per explosion
            usize n_done = 0;
            done[n_done++] = health_holder(w, h->entity);
            for (u32 i = 0; i < n; ++i) {
                const Entity holder = health_holder(w, near[i]);
                bool seen = false;
                if (!holder.is_null())
                    for (usize k = 0; k < n_done; ++k) seen = seen || done[k] == holder;
                if (seen || near[i] == h->entity) continue;
                if (!holder.is_null()) done[n_done++] = holder;
                math::vec3 at = h->point, scale;
                math::quat rot;
                if (const LocalTransform* nt = w.try_get<LocalTransform>(near[i])) world_pose(w, near[i], *nt, at, rot, scale);
                const f32 d = math::length(at - h->point);
                const f32 falloff = 1.0f - math::clamp(d / p.splash_radius, 0.0f, 1.0f);
                if (falloff <= 0.0f) continue;
                const math::vec3 away = d > 1e-4f ? (at - h->point) * (1.0f / d) : dir;
                land(f, phys, e, p.owner, p.team, p.friendly_fire != 0, near[i], h->point, h->normal, away, p.damage * falloff,
                     p.impulse * falloff);
            }
        }
        f.commands().destroy(e);
    });
}

// FixedUpdate_Pre, after the projectiles: regeneration, staggers, the dead's linger.
void kit3d_health(Frame& f) {
    World& w = f.world();
    f.query<Health3D>().each([&](Entity e, Health3D& h) {
        h.since_hit = math::min(h.since_hit + f.dt, 1.0e6f);
        h.current = math::min(h.current, h.max);
        if (h.dead) {
            h.dead_time += f.dt;
            if (h.destroy_on_death && h.dead_time >= h.linger) f.commands().destroy(e);
            return;
        }
        if (h.regen > 0.0f && h.since_hit >= h.regen_delay) h.current = math::min(h.current + h.regen * f.dt, h.max);
        if (h.stagger_left > 0.0f) {
            h.stagger_left -= f.dt;
            if (h.stagger_left <= 0.0f) {
                h.stagger_left = 0.0f;
                if (Ragdoll3D* r = w.try_get_mut<Ragdoll3D>(e)) r->active = 0;
            }
        }
    });
}

}  // namespace

bool apply_damage(World& w, Entity target, const DamageInfo& info) {
    const Entity e = health_holder(w, target);
    if (e.is_null()) return false;
    Health3D& h = *w.try_get_mut<Health3D>(e);
    if (h.dead) return false;
    const f32 amount = math::max(info.amount, 0.0f) * (1.0f - math::clamp(h.armor, 0.0f, 1.0f));
    const math::vec3 dir = math::length_sq(info.direction) > 0.0f ? math::normalize(info.direction) : math::vec3{};
    EventAccess(w).push(Damage3D{e, info.instigator, amount, info.point, dir});
    ++h.hits;
    h.since_hit = 0.0f;
    h.last_attacker = info.instigator;
    if (h.invulnerable) return true;
    h.current -= amount;
    Ragdoll3D* ragdoll = w.try_get_mut<Ragdoll3D>(e);
    if (h.current <= 0.0f) {
        h.current = 0.0f;
        h.dead = 1;
        h.dead_time = 0.0f;
        h.stagger_left = 0.0f;
        if (CharacterController3D* c = w.try_get_mut<CharacterController3D>(e)) {
            c->use_input = false;
            c->move = {};
        }
        if (NavAgent3D* a = w.try_get_mut<NavAgent3D>(e)) a->move_to = 0;
        if (h.ragdoll_on_death && ragdoll) {
            ragdoll->impulse = dir * info.impulse;
            ragdoll->active = 1;
        }
        EventAccess(w).push(Died3D{e, info.instigator});
    } else if (h.stagger_damage > 0.0f && amount >= h.stagger_damage && ragdoll && !ragdoll->active) {
        ragdoll->impulse = dir * info.impulse;
        ragdoll->active = 1;
        h.stagger_left = math::max(h.stagger_time, 0.01f);
    }
    return true;
}

void install_combat3d(World& world, asset::AssetSystem* assets) {
    install_bodies3d(world, assets);
    install_ragdolls3d(world, assets);
    if (world.try_resource<Combat3DState>()) return;
    world.set_resource(Combat3DState{});
    world.register_components<Health3D, Weapon3D, Projectile3D>();
    world.register_event<WeaponFired3D>(Phase::FixedUpdate_Pre);
    world.register_event<WeaponHit3D>(Phase::FixedUpdate_Pre);
    world.register_event<Damage3D>(Phase::FixedUpdate_Pre);
    world.register_event<Died3D>(Phase::FixedUpdate_Pre);
    world.add_system(Phase::FixedUpdate_Pre, "kit3d_weapons", &kit3d_weapons, "kit3d_ragdolls");
    world.add_system(Phase::FixedUpdate_Pre, "kit3d_projectiles", &kit3d_projectiles, "kit3d_weapons");
    world.add_system(Phase::FixedUpdate_Pre, "kit3d_health", &kit3d_health, "kit3d_projectiles");
}

}  // namespace mce::kit
