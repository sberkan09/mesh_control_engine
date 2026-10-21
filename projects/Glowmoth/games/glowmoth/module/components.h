#pragma once
// Glowmoth components: plain reflected data (MCE_COMPONENT). Scenes author them, the cook
// (glowmoth_cook links types.cpp) validates them, and the systems in glowmoth.cpp run on them. Keep
// this header data-only (core + refl + asset refs), so the cook can include it without the game's code.
//
// Layouts are padding-free (World::state_hash hashes component bytes): asset refs first, then
// 4-byte fields, with explicit pad_ fields to keep sizes a multiple of 8.

#include "mce/asset/handle.h"
#include "mce/fixed_string.h"
#include "mce/math/math.h"
#include "mce/refl/macros.h"
#include "mce/types.h"

namespace glowmoth {

using mce::f32;
using mce::i32;
using mce::u32;
using mce::math::vec2;

// Turns the entity about +Z (degrees per second).
struct Spin {
    f32 degrees_per_second = 90.0f;
};
MCE_COMPONENT(Spin, degrees_per_second)

// Moves the entity up and down around where the scene placed it.
struct Bob {
    f32 height = 0.5f;  // meters
    f32 period = 2.0f;  // seconds
    f32 base_y = 0.0f;  // set on the first tick
    f32 time = 0.0f;
    bool started = false;
};
MCE_COMPONENT(Bob, height, period, base_y, time, started)

// The moth: run, a jump with coyote time and a jump buffer, `air_flaps` extra flaps in the air,
// and a slow glide while Jump is held on the way down. Needs a kinematic RigidBody2D, a box
// Collider2D and CharacterController2D. The Sprite (with an AnimatedSprite) faces the run direction.
struct MothController {
    mce::asset::AssetRef<mce::asset::Sfx> jump_sound;
    mce::asset::AssetRef<mce::asset::Sfx> flap_sound;
    mce::asset::AssetRef<mce::asset::Sfx> hurt_sound;
    f32 run_speed = 6.0f;
    f32 jump_speed = 11.0f;
    f32 flap_speed = 8.5f;
    f32 gravity = 30.0f;
    f32 max_fall = 14.0f;
    f32 glide_fall = 2.5f;  // fall speed while Jump is held
    f32 kill_y = -4.0f;     // falling below this sends the moth back to the start
    u32 air_flaps = 1;
    // Runtime state.
    vec2 vel;
    vec2 spawn;
    u32 started = 0;
    u32 coyote = 0;
    u32 jump_buffer = 0;
    i32 facing = 1;
    u32 invuln = 0;
    u32 grounded = 0;
    u32 flaps_left = 0;
    u32 pad_ = 0;
};
MCE_COMPONENT(MothController, jump_sound, flap_sound, hurt_sound, (run_speed, Range{0, 30}), (jump_speed, Range{0, 40}),
              (flap_speed, Range{0, 40}), (gravity, Range{0, 200}), (max_fall, Range{0, 80}), (glide_fall, Range{0, 80}),
              (kill_y, Help{"falling below this height sends the moth back to the start"}),
              (air_flaps, Range{0, 5}, Help{"extra jumps in the air"}), (vel, ReadOnly, Transient),
              (spawn, ReadOnly, Transient), (started, ReadOnly, Transient), (grounded, ReadOnly, Transient),
              (invuln, ReadOnly, Transient), (flaps_left, ReadOnly, Transient))

// A lantern: a sensor the moth lights by touching it. The Sprite switches to `lit_texture`.
struct Lantern {
    mce::asset::AssetRef<mce::asset::Texture> lit_texture;
    mce::asset::AssetRef<mce::asset::Sfx> sound;
    u32 lit = 0;
    u32 pad_ = 0;
};
MCE_COMPONENT(Lantern, lit_texture, sound, (lit, ReadOnly, Transient))

// The moon gate at the end of a level: shut until every lantern is lit, then touching it goes to
// `next_scene` (a source path under assets/).
struct MoonGate {
    mce::asset::AssetRef<mce::asset::Texture> open_texture;
    mce::asset::AssetRef<mce::asset::Sfx> open_sound;
    mce::asset::AssetRef<mce::asset::Sfx> enter_sound;
    mce::FixedString<64> next_scene;
    u32 open = 0;
    u32 entered = 0;
    u32 timer = 0;
    u32 pad_ = 0;
};
MCE_COMPONENT(MoonGate, open_texture, open_sound, enter_sound, (next_scene, Help{"scene to load when the moth enters"}),
              (open, ReadOnly, Transient), (entered, ReadOnly, Transient))

// A bat: flies x = origin.x + extent.x * sin(a), y = origin.y + extent.y * cos(2a), a = tau * t / period
// + phase: across between the flowers, swooping down at both ends. Touching it
// costs a hit and sends the moth back to the level start. No physics body: the game tests the
// box `half` against the moth.
struct Bat {
    vec2 extent{3.0f, 0.0f};
    vec2 half{0.4f, 0.3f};
    f32 period = 4.0f;
    f32 phase = 0.0f;
    vec2 origin;  // runtime: captured on the first tick
    u32 started = 0;
    u32 pad_ = 0;
};
MCE_COMPONENT(Bat, extent, half, (period, Range{0.1f, 60}), phase, (origin, ReadOnly, Transient),
              (started, ReadOnly, Transient))

// On a TextLabel: the game writes the run's status into it. Pair it with Parallax {0, 0}.
enum class HudKind : u32 { Status, Banner };
MCE_ENUM(HudKind, Status, Banner)
struct HudText {
    HudKind kind = HudKind::Status;
};
MCE_COMPONENT(HudText, kind)

// A title, win or lose screen: Jump (or Confirm) after `min_ticks` goes to `next_scene`.
// `reset_run` clears the hits (the title starts a fresh run).
struct MenuScreen {
    mce::asset::AssetRef<mce::asset::Sfx> sound;  // played when the screen moves on
    mce::FixedString<64> next_scene;
    u32 min_ticks = 30;
    u32 reset_run = 0;
};
MCE_COMPONENT(MenuScreen, sound, (next_scene, Help{"scene to load on Jump"}), min_ticks, reset_run)

}  // namespace glowmoth
