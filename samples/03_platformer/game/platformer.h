#pragma once
// 03_platformer gameplay (ROADMAP M3 demo): everything that is "the game" and nothing that is the
// host. It sees the ECS, physics2d, the InputState snapshot and audio's PlaySfx event — no window,
// no renderer, no device — so the same code runs windowed, headless under the determinism harness,
// and (once the game-module loader lands) from game.dll.
//
// Systems (all deterministic; tick-driven, no wall clock):
//   FixedUpdate_Pre   pf_prev      remember last tick's poses (render interpolation)
//   FixedUpdate_Pre   pf_state     pause toggle, level clock, win restart
//   FixedUpdate_Pre   pf_movers    moving platforms follow their path (kinematic velocity)
//   FixedUpdate_Pre   pf_player    run / jump (coyote time, jump buffer, variable height)
//   FixedUpdate_Pre   pf_enemies   enemy FSM: Patrol -> Chase -> Return
//   FixedUpdate_Post  pf_triggers  pickups, hazards, goal (sensor TriggerEnter events)
//   FixedUpdate_Post  pf_combat    stomp or get hurt on enemy contact

#include "mce/audio/audio.h"
#include "mce/ecs/world.h"
#include "mce/math/math.h"

namespace platformer {

using mce::f32;
using mce::i32;
using mce::u32;
using mce::u64;
using mce::math::vec2;

// ---- collision layers -------------------------------------------------------------------------
enum Layer : mce::u8 { Layer_World = 0, Layer_Player = 1, Layer_Enemy = 2, Layer_Sensor = 3 };

// ---- components (trivially copyable, padding-free: World::state_hash hashes their bytes) ------

// What the host draws for an entity. `sprite` is a game-defined art id (see Sprite).
enum class Sprite : u32 { Player, Enemy, Coin, Spike, Flag, Platform, Count };
struct Look {
    Sprite sprite = Sprite::Player;
    i32 order = 0;
    vec2 size{1.0f, 1.0f};  // metres
};

// Position at the end of the previous tick; render lerps from here to LocalTransform.
struct PrevPos {
    vec2 p;
};

struct Player {
    vec2 vel;
    vec2 spawn;
    u32 coyote = 0;       // ticks left in which a jump still counts as grounded
    u32 jump_buffer = 0;  // ticks left in which a early jump press is remembered
    i32 facing = 1;
    u32 invuln = 0;       // ticks of post-respawn grace
    u32 grounded = 0;     // 1 when the last move ended on ground
    u32 running = 0;      // 1 when moving horizontally on ground (animation)
};

enum class EnemyState : u32 { Patrol, Chase, Return };
struct Enemy {
    vec2 home;
    f32 vy = 0.0f;
    f32 dir = 1.0f;
    EnemyState state = EnemyState::Patrol;
    u32 state_ticks = 0;
    f32 patrol_half = 2.5f;  // patrol range around home.x
    u32 seen = 0;            // ticks since the player was last seen (chase memory)
};

// Kinematic platform: position(t) = origin + extent * sin(tau * t / period).
struct Mover {
    vec2 origin;
    vec2 extent;
    f32 period = 4.0f;
    f32 phase = 0.0f;
};

struct Pickup {
    u32 value = 1;
};
struct Hazard {
    u32 damage = 1;
};
struct Goal {
    u32 reached = 0;
};

// ---- the game's world resource ------------------------------------------------------------------

struct Sounds {  // registered by the host; null handles play nothing
    mce::audio::ClipHandle jump, pickup, hurt, stomp, win;
};

struct GameState {
    u64 level_ticks = 0;  // advances only while playing (not paused, not won)
    u32 coins = 0;
    u32 coins_total = 0;
    u32 deaths = 0;
    u32 stomps = 0;
    u32 flash_ticks = 0;  // pickup flash countdown (render reads it)
    f32 shake = 0.0f;     // trauma requested this tick (host feeds its ScreenShake, then zeroes it)
    bool paused = false;
    bool won = false;
    u64 won_at = 0;
    mce::Entity player;
    vec2 level_size;  // metres
    Sounds sounds;
};

struct Config {
    const char* const* level = nullptr;  // rows, top to bottom; null = the built-in level
    u32 rows = 0;
};

// Registers components, events, the physics module, gameplay systems and the GameState resource,
// then spawns the level. `sounds` may be all-null (silent). The audio bridge must be installed
// by the host (audio::install) for PlaySfx events to make sound.
void install(mce::World& world, const Config& cfg = {}, const Sounds& sounds = {});

// The legend-mapped level as tiles for the host's tilemap: width*height, row 0 = bottom.
// 0 empty, 1 ground top, 2 ground fill, 3 one-way plank.
struct TileGrid {
    u32 width = 0, height = 0;
    mce::Array<u32> tiles;
};
void level_tiles(const Config& cfg, TileGrid& out);

// Order-total hash of the gameplay state on top of World::state_hash (determinism harness).
u64 hash_game(mce::World& world, u64 seed);

}  // namespace platformer
