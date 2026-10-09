#pragma once
// Ashen Court gameplay (ROADMAP M7): a third-person melee slice in a ruined court. Everything
// that is "the game" and nothing that is the host: the ECS world, World3D (characters, the
// court's colliders, the swept camera boom), the InputState snapshot and audio's PlaySfx event.
// No window, renderer or device, so the same code runs windowed and headless under the
// determinism harness.
//
// Systems (deterministic; tick-driven):
//   FixedUpdate_Pre      ac_flow       title / play / pause / dead / victory states, restarts
//   FixedUpdate_Pre      ac_player     camera-relative run + jump, sword swings (AttackEvent)
//   FixedUpdate_Pre      ac_senses     each enemy's distance / sight / reach of the player
//   FixedUpdate_Pre      ac_husk_fsm, ac_wisp_fsm, ac_brute_fsm   fsm.h tables (FR-ENT-090)
//   FixedUpdate_Pre      ac_enemies    per-state movement and attacks
//   FixedUpdate_Pre      ac_orbs       wisp projectiles fly and hit
//   FixedUpdate_Physics  ac_physics    World3D::step, then the camera boom (camera_sweep)
//   FixedUpdate_Post     ac_combat     AttackEvent -> health, stagger, deaths (Damaged / Died)
//   FixedUpdate_Post     ac_pickups    ember hearts heal on touch
//
// Combat is events, not callbacks: attackers push AttackEvent; ac_combat resolves them against
// Health and pushes Damaged / Died for feedback (sound, HUD, particles) — FR-ENT-030 shape.

#include "mce/audio/components.h"
#include "mce/ecs/world.h"
#include "mce/input/actions.h"
#include "mce/kit/fsm.h"
#include "mce/math/math.h"
#include "mce/physics3d/world3d.h"
#include "mce/result.h"

namespace court {

using mce::f32;
using mce::i32;
using mce::u32;
using mce::u64;
using mce::u8;
using mce::math::vec3;

// ---- the kit (assets/models, one per gen_assets.py model) -------------------------------------
enum class Model : u32 {
    Floor, Wall, Column, BrokenColumn, Arch, Dais, Brazier, Rubble,
    Hero, Sword, Husk, Wisp, Brute, Orb, Heart, Count
};
inline constexpr const char* kModelPaths[] = {
    "models/court_floor.glb", "models/wall_segment.glb", "models/column.glb", "models/broken_column.glb",
    "models/arch.glb",        "models/dais.glb",         "models/brazier.glb", "models/rubble.glb",
    "models/hero.glb",        "models/sword.glb",        "models/husk.glb",    "models/wisp.glb",
    "models/brute.glb",       "models/orb.glb",          "models/ember_heart.glb",
};
static_assert(sizeof(kModelPaths) / sizeof(kModelPaths[0]) == static_cast<u32>(Model::Count));

// The level's stated triangle budget (M7: ~300k), checked against the placed instances at load.
inline constexpr u32 kLevelTriangleBudget = 300000;

// One placed static piece of the court (the host renders these; physics gets the colliders).
struct Placement {
    Model model = Model::Floor;
    vec3 position;
    f32 yaw = 0.0f;
    vec3 scale{1.0f, 1.0f, 1.0f};
};
struct Light {
    vec3 position;
    vec3 color;
    f32 intensity = 1.0f;
    f32 radius = 8.0f;
};
struct Layout {
    mce::Array<Placement> statics;
    mce::Array<Light> braziers;  // a point light above each brazier
};
// The court: walls, colonnade, arches, dais, braziers, rubble. Pure data, deterministic.
void build_layout(Layout& out);

// ---- components (trivially copyable, padding-free: World::state_hash hashes their bytes) --------
enum class Kind : u32 { Player, Husk, Wisp, Brute };

// A character on World3D: capsule centre `pos`, facing `yaw` (0 = +Z, CCW from above).
struct Actor {
    Kind kind = Kind::Player;
    f32 yaw = 0.0f;
    vec3 pos;
    vec3 prev_pos;  // last tick (render interpolation)
    f32 prev_yaw = 0.0f;
    f32 vy = 0.0f;
    vec3 home;
    u32 grounded = 0;
    u32 pad_ = 0;
};

struct Health {
    i32 hp = 100;
    i32 max = 100;
    u32 invuln = 0;  // ticks of post-hit grace
    u32 flash = 0;   // ticks of hit flash (render)
};

struct PlayerCtl {
    u32 swing = 0;      // ticks into the current swing (0 = not swinging)
    u32 cooldown = 0;
    u32 swings = 0;
    u32 hits = 0;       // swings that connected
};

// Shared senses, refreshed every tick before the FSMs run (the guards read them).
struct Senses {
    f32 distance = 1e9f;  // to the player (horizontal)
    u32 sees = 0;         // within sight range with a clear line
    u32 lost = 0;         // beyond the give-up range
    u32 in_reach = 0;     // within this kind's attack reach
    u32 bumped = 0;       // a charge ran into a wall this tick (brute)
    u32 struck = 0;       // took a hit this tick (stagger)
};

enum class HuskState : u8 { Idle, Chase, WindUp, Recover, Stagger, Count };
struct HuskBrain {
    HuskState state = HuskState::Idle;
    u8 pad_[3] = {};
    f32 state_time = 0.0f;
};
enum class WispState : u8 { Idle, Hover, Cast, Cooldown, Stagger, Count };
struct WispBrain {
    WispState state = WispState::Idle;
    u8 pad_[3] = {};
    f32 state_time = 0.0f;
    f32 orbit = 1.0f;  // strafe direction
};
enum class BruteState : u8 { Idle, Chase, WindUp, Charge, Stunned, Recover, Count };
struct BruteBrain {
    BruteState state = BruteState::Idle;
    u8 pad_[3] = {};
    f32 state_time = 0.0f;
    vec3 charge_dir;
    u32 charge_hit = 0;  // the player was already hit by this charge
};

struct Orb {
    vec3 pos;
    vec3 prev_pos;
    vec3 vel;
    u32 life = 0;
    u32 pad_ = 0;
};

struct Heart {
    vec3 pos;
    u32 heal = 35;
    u32 pad_ = 0;
};

// ---- events -----------------------------------------------------------------------------------------
// A melee or contact attack: everything in the cone (or sphere, when cos_arc <= -1) takes damage.
struct AttackEvent {
    mce::Entity source;
    vec3 origin;
    vec3 dir;          // horizontal unit
    f32 range = 2.0f;
    f32 cos_arc = 0.3f;
    i32 damage = 10;
    u32 hits_player = 0;  // 1: enemies attacking the player; 0: the player attacking enemies
};
struct Damaged {
    mce::Entity target;
    Kind kind;
    i32 amount;
    vec3 pos;
};
struct Died {
    mce::Entity entity;
    Kind kind;
    vec3 pos;
};

// ---- the game's state --------------------------------------------------------------------------------
enum class Mode : u32 { Title, Playing, Paused, Dead, Victory };

struct Sounds {  // registered by the host; null handles play nothing
    mce::audio::ClipHandle swing, hit, hurt, death, cast, heal, roar, step;
};
inline constexpr mce::u16 kAttenuationCombat = 1;  // the host registers this curve

struct CourtState {
    Mode mode = Mode::Playing;
    u64 ticks = 0;          // ticks in Playing
    u32 serial = 0;         // bumps on every (re)start
    u32 kills = 0;
    u32 enemies = 0;        // alive
    u32 enemies_total = 0;
    u32 deaths = 0;         // player deaths
    u32 damage_taken = 0;
    mce::Entity player;
    // Camera: yaw/pitch are sim state (movement is camera-relative); the boom length comes from
    // the sweep. pivot = player + head offset.
    f32 cam_yaw = 0.0f;  // the boom points +Z: behind a player facing into the court
    f32 cam_pitch = 0.32f;
    f32 boom = 5.5f;
    f32 prev_boom = 5.5f;
    f32 prev_cam_yaw = 0.0f;
    u32 camera_hits = 0;    // ticks the sweep pulled the boom in
    f32 closest_boom = 5.5f;
    u32 autopilot = 0;      // the bot drives the camera (trails the heading)
    u32 settle = 0;         // ticks the sim waits after a restart (the new cast lands at the sync)
    f32 shake = 0.0f;       // camera shake amount, decays over ~0.3 s
    Sounds sounds;
};

inline constexpr f32 kBoomLength = 5.5f;
inline constexpr f32 kBoomRadius = 0.25f;
inline constexpr vec3 kHeadOffset{0.0f, 0.7f, 0.0f};
inline constexpr mce::u32 kSwingTicks = 20;
inline constexpr mce::u32 kSwingHitTick = 7;

// World3D lives in the World as a resource.
struct Physics {
    mce::physics::World3D world;
};

// Registers components, events, the systems and the resources (CourtState, Physics), builds the
// court's colliders and spawns the cast. The host installs input (InputState) and, for sound, the
// audio bridge first; a Sounds resource set before install is picked up.
mce::ResultV install(mce::World& world);

// The attract-mode / headless bot: a pure function of the world (deterministic). It hunts the
// nearest enemy, swings when in reach, sidesteps a brute's wind-up and fetches a heart when hurt.
mce::input::InputState autopilot(mce::World& world, u64 tick);

// Order-total hash of the gameplay state (determinism harness): ECS + World3D + CourtState.
u64 hash_game(mce::World& world, u64 seed);

}  // namespace court
