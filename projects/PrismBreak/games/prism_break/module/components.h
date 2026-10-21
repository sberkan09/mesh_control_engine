#pragma once
// Prism Break components: plain reflected data (MCE_COMPONENT). Scenes author them, the cook
// (prism_break_cook links types.cpp) validates them, and the systems in prism_break.cpp run on
// them. Keep this header data-only (core + refl + asset refs), so the cook can include it without
// the game's code.
//
// The game is a brick breaker: a paddle at the bottom, a ball, a wall of bricks. Prism bricks
// split the ball in two when they break; steel bricks never break.
//
// Layouts are padding-free (World::state_hash hashes component bytes): asset refs first, then
// 4-byte fields, with explicit pad_ fields to keep sizes a multiple of 8.

#include "mce/asset/handle.h"
#include "mce/math/math.h"
#include "mce/refl/macros.h"
#include "mce/types.h"

namespace prism_break {

using mce::f32;
using mce::i32;
using mce::u32;
using mce::math::vec2;

// The level's rules and sounds. One per scene (the first one in ECS order is used).
struct Rules {
    mce::asset::AssetRef<mce::asset::Sfx> brick_sound;
    mce::asset::AssetRef<mce::asset::Sfx> paddle_sound;
    mce::asset::AssetRef<mce::asset::Sfx> lose_sound;
    mce::asset::AssetRef<mce::asset::Sfx> win_sound;
    u32 lives = 3;
    f32 kill_y = -8.0f;  // a ball below this height is lost
};
MCE_COMPONENT(Rules, brick_sound, paddle_sound, lose_sound, win_sound, (lives, Range{1, 9}),
              (kill_y, Help{"a ball falling below this height is lost"}))

// The paddle: a kinematic RigidBody2D with a box Collider2D. Moves along x with the Move action,
// clamped to [min_x, max_x] (its centre). Where the ball lands on it sets the bounce angle: the
// edge sends it out at `bounce_angle` degrees from vertical.
struct Paddle {
    f32 speed = 16.0f;
    f32 bounce_angle = 60.0f;
    f32 min_x = -9.0f;
    f32 max_x = 9.0f;
    u32 autopilot = 0;  // 1: the paddle follows the ball and launches it (attract mode, headless tests)
    u32 pad_ = 0;
};
MCE_COMPONENT(Paddle, (speed, Range{0, 60}), (bounce_angle, Range{5, 85}), min_x, max_x,
              (autopilot, Help{"1: the paddle plays itself (attract mode)"}))

// A ball: no physics body; it sweeps a circle through the world each tick and reflects off what it
// hits. It rides the paddle until launched (Jump or Fire). Speed grows by `speedup` per brick.
struct Ball {
    f32 speed = 9.0f;
    f32 max_speed = 15.0f;
    f32 speedup = 0.12f;
    f32 radius = 0.22f;
    vec2 vel;          // runtime
    u32 launched = 0;  // runtime: 0 while it rides the paddle
    u32 active = 1;    // runtime: 0 once lost (hidden until the next serve)
    u32 spawned = 0;   // runtime: 1 for a ball split off a prism brick
    u32 pad_ = 0;
};
MCE_COMPONENT(Ball, (speed, Range{1, 40}), (max_speed, Range{1, 60}), (speedup, Range{0, 2}), (radius, Range{0.05f, 2}),
              (vel, ReadOnly, Transient), (launched, ReadOnly, Transient), (active, ReadOnly, Transient),
              (spawned, ReadOnly, Transient))

enum class BrickKind : u32 { Normal, Prism, Steel };
MCE_ENUM(BrickKind, Normal, Prism, Steel)

// A brick: a static RigidBody2D with a box Collider2D and a Sprite (tinted by its hit points).
// Prism bricks split the ball that breaks them; steel bricks bounce the ball and never break.
struct Brick {
    BrickKind kind = BrickKind::Normal;
    u32 max_hp = 1;
    u32 points = 10;
    u32 hp = 0;     // runtime
    u32 alive = 1;  // runtime
    u32 pad_ = 0;
};
MCE_COMPONENT(Brick, kind, (max_hp, Range{1, 5}), (points, Range{0, 1000}), (hp, ReadOnly, Transient),
              (alive, ReadOnly, Transient))

// On a TextLabel: the game writes the run's status into it. Status: score, lives and bricks left;
// Banner: the serve hint, then the result.
enum class HudKind : u32 { Status, Banner };
MCE_ENUM(HudKind, Status, Banner)
struct HudText {
    HudKind kind = HudKind::Status;
};
MCE_COMPONENT(HudText, kind)

}  // namespace prism_break
