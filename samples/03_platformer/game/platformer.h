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
//
// The game's state outside components is the GameState resource; the host hands sound clips in
// through the Sounds resource before install.

#include "mce/audio/audio.h"
#include "mce/ecs/world.h"
#include "mce/math/math.h"
#include "mce/physics/world2d.h"
#include "mce/result.h"

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

// Registers the game's systems and the GameState resource, then spawns the level. The host
// installs the engine runtime first — input (InputState resource), physics2d and, for sound, the
// audio bridge — and may set a Sounds resource (absent: silent). This is the game-module entry
// point's install (game/module.cpp): no state outside the World.
mce::ResultV install(mce::World& world);

// Order-total hash of the gameplay state on top of World::state_hash (determinism harness).
// Inline: a host that loads the game as a module calls it without linking the game.
inline u64 hash_game(mce::World& world, u64 seed) {
    const GameState& gs = world.resource<GameState>();
    mce::Hasher64 h(world.state_hash(seed));
    h.u64v(world.resource<mce::physics::World2D>().hash_state(seed));
    h.u64v(gs.level_ticks);
    h.u32v(gs.coins);
    h.u32v(gs.deaths);
    h.u32v(gs.stomps);
    h.u32v(gs.flash_ticks);
    h.u32v(gs.paused ? 1u : 0u);
    h.u32v(gs.won ? 1u : 0u);
    h.u64v(gs.won_at);
    return h.value();
}

}  // namespace platformer
