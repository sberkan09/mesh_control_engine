#pragma once
// Ember Step gameplay (03_platformer; ROADMAP M3 demo, full game in M6): everything that is "the
// game" and nothing that is the host. It sees the ECS, physics2d, the InputState snapshot and
// audio's PlaySfx event — no window, no renderer, no device — so the same code runs windowed,
// headless under the determinism harness, and (once the game-module loader lands) from game.dll.
//
// Systems (all deterministic; tick-driven, no wall clock):
//   FixedUpdate_Pre   pf_prev      remember last tick's poses (render interpolation)
//   FixedUpdate_Pre   pf_state     menu / pause / results flow, level loading, level clock
//   FixedUpdate_Pre   pf_movers    moving platforms follow their path (kinematic velocity)
//   FixedUpdate_Pre   pf_crumble   crumbling blocks shake, give way and grow back
//   FixedUpdate_Pre   pf_player    run / jump (coyote time, jump buffer, variable height),
//                                  wall slide + wall jump, dash
//   FixedUpdate_Pre   pf_enemies   enemy FSM: Patrol -> Chase -> Return
//   FixedUpdate_Post  pf_triggers  pickups, hazards, goal (sensor TriggerEnter events)
//   FixedUpdate_Post  pf_combat    stomp or get hurt on enemy contact
//
// The game's state outside components is the GameState resource; the host hands sound clips in
// through the Sounds resource before install. Best times live on a Records component (Save
// fields) on an entity with SaveKey 1, so the host persists them with save::save_game.
//
// Flow: Menu (level select) -> Playing <-> Paused -> Results -> next level / retry / menu. Menus
// read Move.y edges and Confirm; Pause toggles. Install starts Playing level 1 (headless runs and
// the attract bot); an interactive host sets GameState::mode = Mode::Menu right after.

#include "mce/audio/audio.h"
#include "mce/ecs/save.h"
#include "mce/ecs/world.h"
#include "mce/math/math.h"
#include "mce/physics/world2d.h"
#include "mce/result.h"

#include "level.h"

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
enum class Sprite : u32 { Player, Enemy, Coin, Spike, Flag, Platform, Crate, Crumble, Count };
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
    i32 wall = 0;         // -1 / +1: pressed against a wall on that side (airborne), else 0
    u32 wall_coyote = 0;  // ticks left in which a wall jump still counts
    u32 wall_lock = 0;    // ticks after a wall jump during which steering is ignored
    u32 sliding = 0;      // 1 while wall-sliding (animation)
    u32 dash_ticks = 0;   // ticks left of the current dash
    i32 dash_dir = 1;
    u32 dash_ready = 1;   // one dash per air time; ground or a wall recharges it
    u32 dashes = 0;
    i32 last_wall = 1;    // side of the wall last touched (the wall jump kicks away from it)
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

// Kinematic platform: position(t) = origin + extent * sin(a) + extent_cos * cos(a), a = tau * t /
// period + phase. A zero extent_cos is a back-and-forth line; extent ⟂ extent_cos is an orbit.
struct Mover {
    vec2 origin;
    vec2 extent;
    f32 period = 4.0f;
    f32 phase = 0.0f;
    vec2 extent_cos;
};

// A dynamic box the solver owns (M3.5): stacks, gets pushed, rides platforms.
struct Crate {
    u32 id = 0;
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

// X tiles: Solid -> (player lands) Shaking -> Gone (body disabled) -> Solid again.
enum class CrumbleState : u32 { Solid, Shaking, Gone };
struct Crumble {
    CrumbleState state = CrumbleState::Solid;
    u32 timer = 0;
};

// Everything a level spawned (destroyed when another level loads).
struct LevelTag {
    u32 level = 0;
};

// Best times per level in ticks (0 = not finished yet) and how many levels are open.
inline constexpr u32 kRecordsSaveKey = 1;
struct Records {
    u32 best0 = 0, best1 = 0, best2 = 0, best3 = 0, best4 = 0;
    u32 unlocked = 1;
    u32 best(u32 level) const {
        const u32 b[] = {best0, best1, best2, best3, best4};
        return level < 5 ? b[level] : 0;
    }
    void set_best(u32 level, u32 ticks) {
        u32* b[] = {&best0, &best1, &best2, &best3, &best4};
        if (level < 5) *b[level] = ticks;
    }
};
MCE_COMPONENT(Records, (best0, Save), (best1, Save), (best2, Save), (best3, Save), (best4, Save), (unlocked, Save))

// ---- the game's world resource ------------------------------------------------------------------

struct Sounds {  // registered by the host; null handles play nothing
    mce::audio::ClipHandle jump, pickup, hurt, stomp, win;
};

// Effects the host draws (particles, flashes); the game appends, the host drains every tick.
enum class Fx : u32 { Jump, Land, Dash, WallJump, Coin, Stomp, Hurt, Crumble, Win };
struct FxRequest {
    Fx kind = Fx::Jump;
    i32 dir = 0;
    vec2 pos;
};

enum class Mode : u32 { Menu, Playing, Paused, Results };
inline constexpr u32 kResultsDelayTicks = 90;  // win celebration before the results screen

struct GameState {
    Mode mode = Mode::Playing;
    u32 level = 0;         // index into kLevels
    u32 level_serial = 0;  // bumps on every (re)load; the host rebuilds its tilemap on change
    u32 selection = 0;     // highlighted menu entry
    i32 load_request = -1; // level to (re)load at the start of the next tick, -1 none
    f32 nav_prev = 0.0f;   // Move.y last tick (menu edges)
    bool new_best = false; // the finished run beat the stored time
    u32 results_timer = 0; // ticks until the results screen after a win
    u32 records_serial = 0; // bumps when Records changed (the host saves then)
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
    mce::Entity records;
    Sounds sounds;
    FxRequest fx[16];
    u32 fx_count = 0;
};

// Entries of the current menu: Menu = open levels, Paused = Resume / Restart / Menu, Results =
// [Next] / Retry / Menu (no Next after the last level), Playing = none.
inline u32 menu_options(const GameState& gs, const Records* rec) {
    switch (gs.mode) {
        case Mode::Menu: {
            const u32 open = rec ? rec->unlocked : 1u;
            return open < 1 ? 1u : open > kLevelCount ? kLevelCount : open;
        }
        case Mode::Paused: return 3;
        case Mode::Results: return gs.level + 1 < kLevelCount ? 3u : 2u;
        case Mode::Playing: break;
    }
    return 0;
}

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
    h.u32v(static_cast<u32>(gs.mode));
    h.u32v(gs.level);
    h.u32v(gs.level_serial);
    return h.value();
}

}  // namespace platformer
