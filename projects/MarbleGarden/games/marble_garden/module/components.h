#pragma once
// MarbleGarden components: plain reflected data (MCE_COMPONENT). Scenes author them, the cook
// (marble_garden_cook links types.cpp) validates them, and the systems in marble_garden.cpp run on them. Keep
// this header data-only (core + refl), so the cook can include it without the game's code.
//
//   Marble          RigidBody3D (Dynamic, Sphere) + MarbleController: the Move action rolls it,
//                   relative to the primary 3D camera's facing (camera looking along -Z: Move = (x, -z)).
//   Gem N           Trigger3D (Sphere) + Gem: spins and bobs; the marble touching it collects it.
//   Pond            Trigger3D (Box) + Hazard: the marble entering it goes back to its respawn point.
//   Game            GardenGame: the 90 s timer, the gem count, the HUD labels, the chime, win / lose.
//   Title/Win/Lose  MenuScreen: Confirm (Enter / Space / click / South) changes to next_scene.
//   Result text     ResultLabel: shows the last round's result (gems, time) in its TextLabel.

#include "mce/ecs/entity.h"
#include "mce/fixed_string.h"
#include "mce/math/math.h"
#include "mce/refl/macros.h"
#include "mce/types.h"

namespace marble_garden {

using mce::f32;
using mce::u32;
using mce::u8;

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

// Rolls a dynamic RigidBody3D sphere by the Move action, relative to the primary 3D camera.
struct MarbleController {
    mce::Entity respawn;             // where the pond (or a fall) puts it back; null = where it started
    mce::math::vec3 spawn_position;  // engine-written on the first tick
    f32 acceleration = 14.0f;        // m/s^2 at full Move input
    f32 max_speed = 7.0f;            // m/s: no push past this along the input direction
    f32 brake = 3.0f;                // m/s^2 of slowing when there is no input (on top of damping)
    f32 kill_y = -6.0f;              // falling below this height also respawns
    u32 respawns = 0;                // engine-written: times it went back to the respawn point
    u32 respawn_cooldown = 0;        // engine-written: ticks before the next respawn may fire
    bool started = false;            // engine-written
    u8 pad_[3] = {};
};
MCE_COMPONENT(MarbleController, (respawn, Help{"respawn point entity (e.g. 'Start'); null = start position"}),
              (spawn_position, ReadOnly, Transient), (acceleration, Range{0, 100}, Help{"m/s^2 at full Move input"}),
              (max_speed, Range{0, 50}, Help{"m/s"}), (brake, Range{0, 50}, Help{"m/s^2 slowing with no input"}),
              (kill_y, Help{"below this height the marble respawns"}), (respawns, ReadOnly, Transient),
              (respawn_cooldown, ReadOnly, Transient), (started, ReadOnly, Transient))

// A collectible gem: put it on an entity with a Trigger3D (Sphere). It spins about +Y and bobs.
struct Gem {
    f32 spin_dps = 90.0f;    // degrees per second about +Y
    f32 bob_height = 0.15f;  // metres
    f32 bob_period = 2.0f;   // seconds
    f32 base_y = 0.0f;       // engine-written
    f32 time = 0.0f;         // engine-written
    u32 value = 1;
    bool started = false;    // engine-written
    u8 pad_[3] = {};
};
MCE_COMPONENT(Gem, (spin_dps, Range{-720, 720}), (bob_height, Range{0, 5}), (bob_period, Range{0.1f, 20}),
              (base_y, ReadOnly, Transient), (time, ReadOnly, Transient), value, (started, ReadOnly, Transient))

// A hazard volume: put it on an entity with a Trigger3D. A marble entering it respawns.
struct Hazard {
    u32 hits = 0;  // engine-written
    u32 pad_ = 0;
};
MCE_COMPONENT(Hazard, (hits, ReadOnly, Transient))

// The round: a countdown, gems to collect, HUD labels and the scene changes at the end.
struct GardenGame {
    mce::Entity hud_timer;   // a TextLabel showing the time left
    mce::Entity hud_gems;    // a TextLabel showing gems collected
    mce::Entity chime;       // an SfxTrigger fired on every gem
    mce::Entity fall_sound;  // an SfxTrigger fired when the marble respawns (optional)
    mce::FixedString<64> win_scene{"scenes/win.mcscene"};
    mce::FixedString<64> lose_scene{"scenes/lose.mcscene"};
    f32 time_limit = 90.0f;  // seconds
    f32 time_left = 90.0f;   // engine-written (starts at time_limit)
    f32 end_delay = 1.0f;    // seconds between the last gem / time out and the scene change
    f32 end_timer = 0.0f;    // engine-written
    u32 gems_total = 5;      // 0 = count the scene's Gem entities at start
    u32 gems_collected = 0;  // engine-written
    u32 falls = 0;           // engine-written: pond / fall respawns this round
    u32 state = 0;           // engine-written: 0 playing, 1 won, 2 lost
    bool started = false;    // engine-written
    u8 pad_[3] = {};
    u32 pad2_ = 0;
};
MCE_COMPONENT(GardenGame, (hud_timer, Help{"TextLabel entity for the time left"}),
              (hud_gems, Help{"TextLabel entity for the gem count"}), (chime, Help{"SfxTrigger entity fired per gem"}),
              (fall_sound, Help{"SfxTrigger entity fired on a respawn (optional)"}),
              (win_scene, Help{"scene path loaded when every gem is collected"}),
              (lose_scene, Help{"scene path loaded when the time runs out"}), (time_limit, Range{1, 3600}),
              (time_left, ReadOnly, Transient), (end_delay, Range{0, 10}), (end_timer, ReadOnly, Transient),
              (gems_total, Help{"0 = count the Gem entities"}), (gems_collected, ReadOnly, Transient),
              (falls, ReadOnly, Transient), (state, ReadOnly, Transient, Help{"0 playing, 1 won, 2 lost"}),
              (started, ReadOnly, Transient))

// A menu screen (title, win, lose): Confirm, Jump or Fire changes to next_scene after `min_time`.
struct MenuScreen {
    mce::FixedString<64> next_scene{"scenes/garden.mcscene"};
    f32 min_time = 0.3f;  // seconds before input is accepted (so a held key does not skip a screen)
    f32 time = 0.0f;      // engine-written
    u32 done = 0;         // engine-written
    u32 pad_ = 0;
};
MCE_COMPONENT(MenuScreen, (next_scene, Help{"scene path loaded on Confirm"}), (min_time, Range{0, 10}),
              (time, ReadOnly, Transient), (done, ReadOnly, Transient))

// On a TextLabel entity of the win or lose screen: writes the last round's result into the text.
struct ResultLabel {
    u32 show_time = 1;  // 1: "5 / 5 gems in 42.3 s"; 0: gems only
    u32 pad_ = 0;
};
MCE_COMPONENT(ResultLabel, show_time)

}  // namespace marble_garden
