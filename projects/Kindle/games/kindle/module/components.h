#pragma once
// Kindle components: plain reflected data (MCE_COMPONENT). Scenes author them, the cook
// (kindle_cook links types.cpp) validates them, and the systems in kindle.cpp run on them. Keep
// this header data-only (core + refl + asset refs), so the cook can include it without the game's
// code.
//
// Most of these are Ember Step's mechanics (samples/03_platformer/game) recast as authored
// components: the player controller, coins, hazards, the goal flag, crumbling blocks and moving
// platforms. Ember Step spawns them from ASCII levels in code; Kindle's level is a scene made in
// the editor, so every tunable is a field the inspector (or the MCP server) can set.
//
// The new mechanic is the colour switch: a ColorSwitch sensor flips which SwitchBlock colour is
// solid. Blocks of the active colour collide and draw opaque; the others are ghosts the player
// walks through.
//
// Layouts are padding-free (World::state_hash hashes component bytes): asset refs first, then
// 4-byte fields, with explicit pad_ fields to keep sizes a multiple of 8.

#include "mce/asset/handle.h"
#include "mce/math/math.h"
#include "mce/refl/macros.h"
#include "mce/types.h"

namespace kindle {

using mce::f32;
using mce::i32;
using mce::u32;
using mce::math::vec2;

// ---- the new mechanic ------------------------------------------------------------------------

enum class BlockColor : u32 { Red, Blue };
MCE_ENUM(BlockColor, Red, Blue)

// A block that is solid only while its colour is the active one. Needs RigidBody2D + Collider2D
// (static) and a Sprite; the game disables the body and fades the sprite to `ghost_alpha` while the
// colour is off. A block that turns solid while the player stands inside it waits until they leave.
struct SwitchBlock {
    BlockColor color = BlockColor::Red;
    f32 ghost_alpha = 0.22f;
    u32 solid = 1;  // runtime: 1 while the body is enabled
    u32 pad_ = 0;
};
MCE_COMPONENT(SwitchBlock, color, (ghost_alpha, Range{0, 1}, Help{"sprite alpha while the colour is off"}),
              (solid, ReadOnly, Transient))

// A switch: touching it (a sensor TriggerEnter from the player) flips the active colour. Its Sprite
// is tinted with the active colour. `start_color` on the first switch in ECS order sets the colour
// the level starts with.
struct ColorSwitch {
    mce::asset::AssetRef<mce::asset::Sfx> sound;
    BlockColor start_color = BlockColor::Red;
    u32 presses = 0;  // runtime: how often this switch was hit
};
MCE_COMPONENT(ColorSwitch, (sound, Help{"played on every toggle"}), start_color, (presses, ReadOnly, Transient))

// ---- Ember Step's kit, as authored components -----------------------------------------------

// The platformer controller (Ember Step's pf_player): run, variable-height jump with coyote time
// and a jump buffer, wall slide and wall jump, and an air dash (the Fire action). Needs a kinematic
// RigidBody2D, a box Collider2D and CharacterController2D. The Sprite shows the frame textures.
struct PlayerController {
    mce::asset::AssetRef<mce::asset::Texture> idle;
    mce::asset::AssetRef<mce::asset::Texture> run_a;
    mce::asset::AssetRef<mce::asset::Texture> run_b;
    mce::asset::AssetRef<mce::asset::Texture> jump;
    mce::asset::AssetRef<mce::asset::Texture> dash;
    mce::asset::AssetRef<mce::asset::Sfx> jump_sound;
    mce::asset::AssetRef<mce::asset::Sfx> hurt_sound;
    // Tuning (metres, seconds).
    f32 run_speed = 7.0f;
    f32 jump_speed = 13.0f;
    f32 gravity = 34.0f;
    f32 max_fall = 20.0f;
    f32 dash_speed = 18.0f;
    f32 kill_y = -3.0f;  // falling below this respawns
    // Runtime state (not authored).
    vec2 vel;
    vec2 spawn;
    u32 started = 0;
    u32 coyote = 0;
    u32 jump_buffer = 0;
    i32 facing = 1;
    u32 invuln = 0;
    u32 grounded = 0;
    i32 wall = 0;
    u32 wall_coyote = 0;
    u32 wall_lock = 0;
    i32 last_wall = 1;
    u32 dash_ticks = 0;
    i32 dash_dir = 1;
    u32 dash_ready = 1;
    u32 anim_ticks = 0;
};
MCE_COMPONENT(PlayerController, idle, run_a, run_b, jump, dash, jump_sound, hurt_sound, (run_speed, Range{0, 30}),
              (jump_speed, Range{0, 40}), (gravity, Range{0, 200}), (max_fall, Range{0, 80}), (dash_speed, Range{0, 60}),
              (kill_y, Help{"falling below this height respawns the player"}), (vel, ReadOnly, Transient),
              (spawn, ReadOnly, Transient), (started, ReadOnly, Transient), (grounded, ReadOnly, Transient))

// A pickup (Ember Step's coin): a sensor that adds `value` and disappears.
struct Coin {
    mce::asset::AssetRef<mce::asset::Sfx> sound;
    u32 value = 1;
    u32 pad_ = 0;
};
MCE_COMPONENT(Coin, sound, value)

// Touching it sends the player back to their spawn (Ember Step's spikes).
struct Hazard {
    u32 damage = 1;
};
MCE_COMPONENT(Hazard, damage)

// The goal flag: reaching it finishes the level.
struct Goal {
    mce::asset::AssetRef<mce::asset::Sfx> sound;
    u32 reached = 0;
    u32 pad_ = 0;
};
MCE_COMPONENT(Goal, sound, (reached, ReadOnly, Transient))

// Ember Step's crumbling block: Solid -> (the player lands on it) Shaking -> Gone -> Solid again.
enum class CrumbleState : u32 { Solid, Shaking, Gone };
MCE_ENUM(CrumbleState, Solid, Shaking, Gone)
struct Crumble {
    u32 shake_ticks = 30;
    u32 gone_ticks = 150;
    CrumbleState state = CrumbleState::Solid;
    u32 timer = 0;
};
MCE_COMPONENT(Crumble, shake_ticks, gone_ticks, (state, ReadOnly, Transient), (timer, ReadOnly, Transient))

// Ember Step's moving platform (a kinematic body): position(t) = origin + extent * sin(tau * t /
// period + phase), where origin is where the scene placed it.
struct Mover {
    vec2 extent{2.0f, 0.0f};
    f32 period = 4.0f;
    f32 phase = 0.0f;
    vec2 origin;  // runtime: captured on the first tick
    u32 started = 0;
    u32 pad_ = 0;
};
MCE_COMPONENT(Mover, extent, (period, Range{0.1f, 60}), phase, (origin, ReadOnly, Transient), (started, ReadOnly, Transient))

// On the camera: follows the player, with the camera centre clamped to [min, max].
struct CameraFollow {
    vec2 offset{0.0f, 1.5f};
    vec2 min{-1000.0f, -1000.0f};
    vec2 max{1000.0f, 1000.0f};
    f32 smoothing = 0.15f;  // fraction of the gap closed per tick (1 = locked)
    u32 pad_ = 0;
};
MCE_COMPONENT(CameraFollow, offset, min, max, (smoothing, Range{0.01f, 1}))

// On a TextLabel: the game writes the run's status into it. Status: coins and the active colour;
// Banner: the goal text, then the clear time. Pair it with Parallax {0, 0} to pin it to the screen.
enum class HudKind : u32 { Status, Banner };
MCE_ENUM(HudKind, Status, Banner)
struct HudText {
    HudKind kind = HudKind::Status;
};
MCE_COMPONENT(HudText, kind)

}  // namespace kindle
