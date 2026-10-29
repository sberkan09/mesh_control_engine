#pragma once
// Weapons, health and damage (M-OPENWORLD C2): hitscan guns, projectile weapons, things that can be
// hurt and die, with a ragdoll for the dead. kit::install_stock installs it.
//
//   Health3D      on anything that can take damage: a character, a car, a crate
//   Weapon3D      fires from its entity: hitscan rays or engine-stepped projectiles
//   Projectile3D  a shot in flight (spawned by a Weapon3D, or placed)
//
// Weapon3D. The shot leaves from the entity's position plus `muzzle` along `direction` (entity space,
// forward is -Z): put the weapon on the camera or a child of the character at eye height to shoot
// where the camera looks. It fires when `fire` is set (the engine clears it), or with `use_input` on
// the Fire action (held with `automatic`, else pressed), at most `rate` shots a second. `pellets` rays
// fan out over `spread_deg` (a cone, from a seed in the component, so a replay shoots the same shots).
// With `magazine` > 0 the gun holds that many rounds, `reserve` spare ones refill it after
// `reload_time` seconds (the Reload action with `use_input`, `reload` set, or automatically when
// empty); `infinite_ammo` never touches `reserve`. A hitscan ray reaches `range` metres; a Projectile
// weapon spawns a Projectile3D with `projectile_mesh` flying at `projectile_speed` (m/s), pulled down by
// `projectile_gravity` x gravity, gone after `projectile_life` seconds or on its first hit, with an
// explosion of `splash_radius` if that is > 0 (damage falls off linearly to the edge).
// The shooter (`owner`, or the weapon's own entity; its parents never get hit) is credited for the
// damage; an owner's Health3D `team` stops hits on its team unless `friendly_fire` (team 0 is nobody's).
//
// Health3D. Damage is reduced by `armor` (0..1), taken from `current`; at 0 the entity dies: `dead`,
// Died3D, and with `ragdoll_on_death` its Ragdoll3D goes limp with the hit's impulse (its NavAgent3D
// stops and its CharacterController3D stops listening to the input). A single hit of at least
// `stagger_damage` (0 = off) staggers a living entity with a Ragdoll3D for `stagger_time` seconds.
// `regen` hp/s starts `regen_delay` seconds after the last hit. With `destroy_on_death` the entity is
// destroyed `linger` seconds after it died. `invulnerable` takes hits (events fire) but loses nothing.
//
// apply_damage() is the one entry for any other source (a fall, a crash, an explosion, a script): call
// it from a FixedUpdate_Pre system or between ticks.
//
// Events (producer FixedUpdate_Pre): WeaponFired3D (every shot or pellet, for tracers and muzzle flashes),
// WeaponHit3D (a shot landed on something, hurt or not: impact marks), Damage3D (a Health3D lost health, or
// would have), Died3D.
//
// Systems: "kit3d_weapons", "kit3d_projectiles" and "kit3d_health" (FixedUpdate_Pre, after "kit3d_bodies").

#include "mce/asset/handle.h"
#include "mce/ecs/entity.h"
#include "mce/math/math.h"
#include "mce/refl/macros.h"

namespace mce {
class World;
}

namespace mce::asset {
class AssetSystem;
}

namespace mce::kit {

struct Health3D {
    f32 max = 100.0f;
    f32 current = 100.0f;
    f32 armor = 0.0f;               // 0..1 of every hit it absorbs
    f32 regen = 0.0f;               // hp/s
    f32 regen_delay = 3.0f;         // s after the last hit before regen starts
    u8 team = 0;                    // 0: nobody's team; a Weapon with an owner on the same team hurts nothing here
    u8 invulnerable = 0;
    u8 dead = 0;                    // engine-written
    u8 ragdoll_on_death = 0;        // 1: the Ragdoll3D goes limp when it dies
    u8 destroy_on_death = 0;        // 1: destroyed `linger` s after dying
    u8 bonus_mark = 0;              // engine-written: bit 0 = Equipment's, bit 1 = StatusEffects' armor/max bonus sits on this value (M-GAMEPLAYFIX)
    u8 pad_[2] = {};
    f32 linger = 3.0f;              // s a dead entity stays before destroy_on_death takes it
    f32 stagger_damage = 0.0f;      // one hit this big staggers (a Ragdoll3D goes limp for stagger_time); 0 = off
    f32 stagger_time = 1.0f;        // s
    f32 since_hit = 0.0f;           // engine-written: s since the last hit
    f32 stagger_left = 0.0f;        // engine-written
    f32 dead_time = 0.0f;           // engine-written: s since it died
    Entity last_attacker;           // engine-written: who hit it last
    u32 hits = 0;                   // engine-written
};
MCE_COMPONENT(Health3D, (max, Range{1, 1000000}), (current, Range{0, 1000000}), (armor, Range{0, 1}, Help{"the fraction of every hit absorbed"}),
              (regen, Range{0, 10000}, Help{"hp/s"}), (regen_delay, Range{0, 600}, Help{"s after the last hit"}),
              (team, Help{"0: nobody's; a weapon whose owner shares the team does not hurt it"}), invulnerable,
              (dead, ReadOnly, Transient), (ragdoll_on_death, Help{"1: its Ragdoll3D goes limp when it dies"}),
              (destroy_on_death, Help{"1: destroyed `linger` seconds after it dies"}), (linger, Range{0, 600}),
              (stagger_damage, Range{0, 1000000}, Help{"one hit this big staggers it; 0 = off"}), (stagger_time, Range{0, 60}),
              (since_hit, ReadOnly, Transient), (stagger_left, ReadOnly, Transient), (dead_time, ReadOnly, Transient),
              (last_attacker, ReadOnly, Transient), (hits, ReadOnly, Transient), (bonus_mark, ReadOnly, Transient))

enum class WeaponKind3D : u8 { Hitscan, Projectile };
MCE_ENUM(WeaponKind3D, Hitscan, Projectile)

struct Weapon3D {
    WeaponKind3D kind = WeaponKind3D::Hitscan;
    u8 use_input = 0;               // 1: the Fire (and Reload) actions drive it
    u8 automatic = 1;               // 1: held Fire keeps shooting; 0: one shot per press
    u8 fire = 0;                    // set to shoot (the engine clears it)
    u8 reload = 0;                  // set to reload (the engine clears it)
    u8 friendly_fire = 0;           // 1: hits the owner's team too
    u8 infinite_ammo = 0;           // 1: reloads never use `reserve`
    u8 reloading = 0;               // engine-written
    Entity owner;                   // credited for the damage and never hit; null = the weapon's own entity
    Entity aim_from;                // the shot leaves from (and points along) this entity's pose; null or gone = the weapon's own
    asset::AssetRef<asset::Mesh> projectile_mesh;  // Projectile: the model that flies
    u16 pellets = 1;                // rays or projectiles per shot
    u16 mask = 0xFFFF;              // physics layers it hits
    u16 magazine = 0;               // rounds it holds; 0 = never runs dry
    u16 ammo = 0;                   // rounds in it (filled to `magazine` on the first tick when 0)
    u32 reserve = 0;                // spare rounds
    f32 damage = 20.0f;             // per pellet
    f32 range = 100.0f;             // m, hitscan
    f32 rate = 5.0f;                // shots/s
    f32 spread_deg = 0.0f;          // cone half-angle
    f32 impulse = 0.0f;             // N*s a hit gives a dynamic body
    f32 reload_time = 1.5f;         // s
    f32 projectile_speed = 60.0f;   // m/s
    f32 projectile_gravity = 1.0f;  // x the world's gravity
    f32 projectile_life = 5.0f;     // s
    f32 splash_radius = 0.0f;       // m: > 0 explodes where the projectile lands
    f32 projectile_scale = 0.2f;    // the model's scale
    math::vec3 muzzle;              // aim entity's space: where the shot leaves
    math::vec3 direction{0.0f, 0.0f, -1.0f};  // entity space
    f32 cooldown = 0.0f;            // engine-written: s until the next shot
    f32 reload_left = 0.0f;         // engine-written
    u32 shots = 0;                  // engine-written: shots fired
    u32 seed = 0;                   // engine-written: the spread's random state (0: seeded on the first shot)
    Entity last_hit;                // engine-written: what the last ray hit
    math::vec3 last_point;          // engine-written
    u8 primed = 0;                  // engine-written
    u8 pad_[3] = {};
    f32 bonus_applied = 0.0f;       // engine-written: what the wielder's worn items' "damage" stats have added to `damage` (M-GAMEPLAYFIX)
    u32 pad2_ = 0;                  // explicit tail padding: state_hash hashes the raw bytes
};
MCE_COMPONENT(Weapon3D, kind, (use_input, Help{"1: Fire (held with `automatic`) and Reload drive it"}), automatic,
              (fire, Help{"set to shoot (the engine clears it)"}), (reload, Help{"set to reload (the engine clears it)"}), friendly_fire,
              infinite_ammo, (reloading, ReadOnly, Transient), (owner, Help{"credited and never hit; null = the weapon's own entity"}),
              (aim_from, Help{"the shot starts at and points along this entity (a camera); null or gone = the weapon's own pose"}),
              projectile_mesh, (pellets, Range{1, 64}), (mask, Help{"physics layers it hits"}), (magazine, Help{"rounds it holds; 0 = never dry"}),
              ammo, reserve, (damage, Range{0, 1000000}), (range, Range{0.1f, 10000}), (rate, Range{0.01f, 1000}, Help{"shots/s"}),
              (spread_deg, Range{0, 90}), (impulse, Range{0, 1000000}, Help{"N*s a hit gives a dynamic body"}), (reload_time, Range{0, 60}),
              (projectile_speed, Range{0.1f, 10000}), (projectile_gravity, Range{0, 10}), (projectile_life, Range{0.01f, 600}),
              (splash_radius, Range{0, 1000}, Help{"m: > 0 explodes where the projectile lands"}), (projectile_scale, Range{0.01f, 100}),
              (muzzle, Help{"entity space"}), (direction, Help{"entity space; forward is -Z"}), (cooldown, ReadOnly, Transient),
              (reload_left, ReadOnly, Transient), (shots, ReadOnly, Transient), (seed, ReadOnly, Transient), (last_hit, ReadOnly, Transient),
              (last_point, ReadOnly, Transient), (primed, ReadOnly, Transient), (bonus_applied, ReadOnly, Transient))

// A shot in flight. The engine steps it (a ray along each tick's move, gravity on its velocity).
struct Projectile3D {
    Entity owner;                   // credited; never hit
    math::vec3 velocity;            // m/s
    f32 damage = 20.0f;
    f32 life = 5.0f;                // s left
    f32 gravity_scale = 1.0f;
    f32 splash_radius = 0.0f;
    f32 impulse = 0.0f;
    u16 mask = 0xFFFF;
    u8 team = 0;                    // the owner's team
    u8 friendly_fire = 0;
};
MCE_COMPONENT(Projectile3D, (owner, Help{"credited and never hit"}), (velocity, Help{"m/s"}), (damage, Range{0, 1000000}), (life, Range{0, 600}),
              (gravity_scale, Range{0, 10}), (splash_radius, Range{0, 1000}), (impulse, Range{0, 1000000}), mask, team, friendly_fire)

// Events (producer FixedUpdate_Pre).
struct WeaponFired3D {
    Entity weapon;
    math::vec3 origin;
    math::vec3 end;         // where the ray ended (its hit, or `range` out); a projectile: the muzzle plus a metre
    Entity hit;             // null: nothing; a projectile: null
};
struct WeaponHit3D {
    Entity weapon;          // the Weapon3D's entity, or the Projectile3D's
    Entity owner;
    Entity target;          // what was hit (whether or not it has a Health3D)
    math::vec3 point;
    math::vec3 normal;
};
struct Damage3D {
    Entity target;
    Entity instigator;
    f32 amount = 0.0f;      // health lost (after armor)
    math::vec3 point;
    math::vec3 direction;   // unit, from the attacker towards the target
};
struct Died3D {
    Entity entity;
    Entity killer;
};
MCE_EVENT(WeaponFired3D, weapon, origin, end, hit)
MCE_EVENT(WeaponHit3D, weapon, owner, target, point, normal)
MCE_EVENT(Damage3D, target, instigator, amount, point, direction)
MCE_EVENT(Died3D, entity, killer)

struct DamageInfo {
    f32 amount = 0.0f;
    Entity instigator;
    math::vec3 point;
    math::vec3 direction;   // from the attacker towards the target; zero is fine
    f32 impulse = 0.0f;     // N*s given to the target's ragdoll when it dies or staggers
};

// Hurts `target` (or the first of its parents with a Health3D). False when nothing there can be hurt.
// Pushes Damage3D, and Died3D when the hit kills. From a FixedUpdate_Pre system or between ticks.
bool apply_damage(World& world, Entity target, const DamageInfo& info);

// Registers the components and events and adds the systems (after install_bodies3d and install_ragdolls3d,
// which it calls). Idempotent.
void install_combat3d(World& world, asset::AssetSystem* assets);

}  // namespace mce::kit
