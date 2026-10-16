#pragma once
// The co-op kit (M-VIEWS step 5): several people on one machine, from scene data.
//
//   CoopGame       one per scene (the first wins): the player prefab, the actions that join, leave and
//                  drive, and how many players there can be.
//   PlayerSpawn    a spawn point; player 1..4 is for that player, 0 for whoever comes.
//   LocalPlayer    the entity of a joined player (player 1..4). The kit puts it there: the prefab carries a
//                  LocalPlayer and the kit overrides its player and LocalTransform.position. A LocalPlayer
//                  already in the scene counts as joined.
//   PlayerCamera   a camera that follows its player and is enabled while that player is in (pair it with
//                  CameraView.split = the same number: the split-screen cells follow who is in).
//   SharedCamera2D a 2D camera that frames every player: the box around them plus a margin, never
//                  smaller than min_size.
//
// Players are numbered 1..4 as in the input scripts (P2/Move) and CameraView.split; player 1 is input slot 0
// (the plain action names, the map stack), player N is slot N - 1. How a player joins. The input map of
// player N (a .mcinput map with "player": N, or `map_player_flags` in code) defines the actions
// that CoopGame names (Join, Leave, Move, Jump by default); the kit reads them per slot from the tick's
// InputState (`player_action_id`), so joins, leaves and movement are all inputs: they record, replay
// and hash like any other. A gamepad takes a slot by tapping South or Start when the host calls
// InputSystem::set_join_slots(0, coop_join_slots(world)) (mce_player does); after that the player's
// actions read that pad. `auto_join` players join at the first tick with no button (1: the keyboard).
// Join presses a free slot; Leave frees it (the entity is destroyed). A game that wants more than the
// stock drive reads `LocalPlayer::slot` and the slot's actions itself.
//
// Drive: a LocalPlayer with `drive` on and a CharacterController3D (use_input off) gets the player's Move
// as its ground velocity (x right, y forward = -z; the controller's speed) and Jump as its jump; one with a
// non-static RigidBody2D gets Move * speed as its linear velocity (a top-down game: gravity scale 0).
//
// All of it runs in the simulation (FixedUpdate_Pre and _Post) and uses only InputState and the
// world, so a run hashes the same whatever the window shows. kit::install_stock installs it.

#include "mce/asset/handle.h"
#include "mce/ecs/entity.h"
#include "mce/fixed_string.h"
#include "mce/math/math.h"
#include "mce/refl/macros.h"

namespace mce {
class World;
}

namespace mce::asset {
class AssetSystem;
}

namespace mce::kit {

struct CoopGame {
    asset::AssetRef<asset::Prefab> player;  // spawned per joining player; null: a bare entity with a LocalPlayer
    FixedString<32> join_action;            // empty: "Join"
    FixedString<32> leave_action;           // empty: "Leave"
    FixedString<32> move_action;            // empty: "Move"
    FixedString<32> jump_action;            // empty: "Jump"
    u32 max_players = 4;                    // 1..4
    u32 auto_join = 1;                      // slots 0..auto_join-1 join at once (no button)
    bool tint_players = true;               // colour each player's Sprite red, blue, green, yellow
    bool enabled = true;
    bool started = false;  // engine-written: the auto_join players have joined
    u8 pad_[5] = {};  // explicit: the state hash reads the component's bytes
};
MCE_COMPONENT(CoopGame, (player, Help{"the prefab spawned for each player; it needs a LocalPlayer and a LocalTransform"}),
              (join_action, Help{"the button action that joins a free slot; empty: Join"}),
              (leave_action, Help{"the button action that leaves; empty: Leave"}),
              (move_action, Help{"the Axis2D action that drives the player's CharacterController3D; empty: Move"}),
              (jump_action, Help{"the button action that jumps; empty: Jump"}), (max_players, Range{1, 4}),
              (auto_join, Range{0, 4}, Help{"how many of the first players join without a button (1: the keyboard player)"}),
              (tint_players, Help{"colour each player's Sprite: red, blue, green, yellow"}), enabled,
              (started, ReadOnly, Transient))

struct PlayerSpawn {
    u32 player = 0;  // 1..4: only that player spawns here; 0: any player
};
MCE_COMPONENT(PlayerSpawn, (player, Range{0, 4}, Help{"1-4: only that player spawns here; 0: any player"}))

struct LocalPlayer {
    u32 player = 1;     // 1..4 (input slot = player - 1; the split-screen cell is the same number)
    bool drive = true;  // the kit feeds Move and Jump to the entity's CharacterController3D or RigidBody2D
    u8 pad_[3] = {};
    f32 speed = 5.0f;   // m/s at full Move for a RigidBody2D (a CharacterController3D has its own speed)
};
MCE_COMPONENT(LocalPlayer, (player, Range{1, 4}, Help{"the player's number; the kit sets it when it spawns the player"}),
              (drive, Help{"feed this player's Move and Jump to the entity's body"}),
              (speed, Range{0, 100}, Help{"m/s at full Move for a RigidBody2D"}))

struct PlayerCamera {
    u32 player = 1;                      // follows this player (1..4)
    math::vec3 offset{0.0f, 10.0f, 8.0f};  // from the player to the camera
    f32 smoothing = 0.0f;                // 1/s; 0 = snap to the player
};
MCE_COMPONENT(PlayerCamera, (player, Range{1, 4}, Help{"follows this player; give the CameraView the same number in split"}),
              (offset, Help{"from the player to the camera; the camera's own rotation is yours"}),
              (smoothing, Range{0, 60}, Help{"1/s; 0 = snap"}))

struct SharedCamera2D {
    f32 margin = 2.0f;      // metres around the players' box
    f32 min_width = 8.0f;   // never frames less than this
    f32 min_height = 6.0f;
    f32 smoothing = 0.0f;   // 1/s; 0 = snap
};
MCE_COMPONENT(SharedCamera2D, (margin, Range{0, 100}, Help{"metres around the box of all players"}),
              (min_width, Range{0.1f, 1000}), (min_height, Range{0.1f, 1000}), (smoothing, Range{0, 60}, Help{"1/s; 0 = snap"}))

// Registers the components and the systems "kit_coop" (FixedUpdate_Pre, before the 3D character
// controllers) and "kit_coop_cameras" (FixedUpdate_Post). `assets` loads the player prefab; null: bare
// entities only. Idempotent.
void install_coop(World& world, asset::AssetSystem* assets);

// Bit n set: input slot n (player n + 1) has a LocalPlayer.
u32 coop_joined_mask(World& world);
// The LocalPlayer entity of input slot `slot` (player = slot + 1), or a null entity.
Entity coop_player(World& world, u32 slot);
// For the host: the number of slots pads may join (CoopGame::max_players of the first enabled CoopGame),
// 0 when the scene has none. Pass (0, coop_join_slots(world)) to InputSystem::set_join_slots.
u32 coop_join_slots(World& world);

}  // namespace mce::kit
