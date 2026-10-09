#pragma once
// Solstice Reach gameplay (docs/design/solstice-reach.md): the Keeper walks the island and aligns
// seven heliostats; each alignment moves the time of day. Everything that is "the game" and
// nothing that is the host: the ECS world, World3D (the terrain collider, the authored box
// colliders, the character, the swept camera boom), the InputState snapshot. No window, renderer
// or device, so the same code runs windowed, headless and in play-in-editor.
//
// Systems (deterministic, tick-driven):
//   Startup              sr_startup   terrain + box colliders, the character, the scatter cache, progress
//   FixedUpdate_Pre      sr_flow      title / playing / paused / finale / dawn, the hour, journal timers
//   FixedUpdate_Pre      sr_keeper    camera-relative walk, sprint, jump, interact, shards; or the bot
//   FixedUpdate_Physics  sr_physics   World3D::step, then the camera boom (camera_sweep)
//   FixedUpdate_Post     sr_mirrors   dish rotations follow the alignment progress

#include "components.h"
#include "mce/ecs/world.h"
#include "mce/gameplay.h"
#include "mce/physics3d/world3d.h"

namespace solstice_reach {

using mce::u64;
using mce::u8;
using mce::math::quat;

enum class Mode : mce::u32 { Title, Playing, Paused, Finale, Dawn };

// The hour each count of aligned mirrors moves the island to (design §2.3); index 7 is the finale,
// 8 the dawn the finale ends in.
inline constexpr f32 kHourKeys[9] = {17.5f, 18.67f, 19.5f, 20.25f, 22.0f, 26.0f, 28.5f, 29.83f, 30.5f};
inline constexpr f32 kHoursPerSecond = 1.0f / 8.0f;
inline constexpr f32 kFinaleSeconds = 25.0f;
inline constexpr f32 kAlignSeconds = 2.5f;
inline constexpr f32 kInteractRange = 2.8f;
inline constexpr u32 kMirrors = 7;
inline constexpr const char* kJournal[9] = {
    "THE LAST LONG DAY. SEVEN MIRRORS, ONE EVENING. BEGIN AT THE WATER.",
    "THE WINDMILL STILL TURNS. SOMEONE OILED IT. SOMEONE KEEPS COMING BACK.",
    "LIGHT GOES FROM MIRROR TO MIRROR THE WAY A WORD GOES FROM MOUTH TO MOUTH.",
    "THE ORDER MEASURED THE YEAR BY WHAT THE SUN TOUCHED LAST.",
    "COUNT THE LAMPS ON THE STAIR. TWENTY-FOUR. ONE FOR EVERY HOUR THEY COULD NOT SEE THE SUN.",
    "I USED TO THINK THEY BUILT THIS TO SEE FURTHER. THEY BUILT IT TO KEEP FROM FORGETTING.",
    "ONE MORE. THE VAULT HAS BEEN WAITING A YEAR. SO HAVE I.",
    "IT KEEPS THE LIGHT. THAT'S ALL IT EVER DID. THAT IS ENOUGH.",
    "MORNING. THE MIRRORS CAN REST. SO CAN I.",
};
// Where the seventh beam ends: the Vault's oculus (the Vault sits on the summit plateau).
inline constexpr vec3 kVaultOculus{-6.0f, 63.0f, -20.0f};

// One scatter instance (the host draws these; ScatterCell groups them for culling).
struct ScatterInstance {
    vec3 pos;
    f32 scale;
    quat rot;
    u32 mesh;  // index into ScatterCache::meshes
    u32 pad_ = 0;
};
struct ScatterCell {
    vec3 center;
    f32 radius;
    u32 first = 0, count = 0;
};
struct ScatterCache {
    mce::Array<mce::asset::Guid> meshes;
    mce::Array<ScatterInstance> instances;
    mce::Array<ScatterCell> cells;
};

struct Physics {
    mce::physics::World3D world;
};

struct ReachState {
    Mode mode = Mode::Playing;
    u64 ticks = 0;
    mce::Entity keeper;    // the KeeperSpawn entity (carries Keeper at run time)
    mce::Entity progress;  // the Progress entity
    // Camera (sim state: movement is camera-relative; the host interpolates with prev_*).
    f32 cam_yaw = 0.0f;
    f32 cam_pitch = 0.3f;
    f32 prev_cam_yaw = 0.0f;
    f32 boom = 3.8f;
    f32 prev_boom = 3.8f;
    // Feedback for the host.
    f32 journal_timer = 0.0f;
    u32 journal_line = 0;
    u32 near_mirror = 0;      // 1 + Heliostat::index of the plate the Keeper stands on, else 0
    f32 flare = 0.0f;         // 1 right after an alignment, decays (the dish flash)
    f32 finale_t = 0.0f;      // seconds into the finale
    f32 chime = 0.0f;         // > 0 for one tick when a shard was taken (the host plays a sound)
    u32 shards_total = 0;
    u32 save_requested = 0;   // the host writes the autosave and clears it
    u32 autopilot = 0;        // the bot plays (attract mode, headless)
    u32 waypoint_cursor = 0;
    f32 bot_stuck = 0.0f;     // seconds the bot has stood still while wanting to move
    u32 serial = 0;
    u32 started = 0;          // sr_startup ran
};

// Registers components, systems and the resources (ReachState, Physics, ScatterCache). The host
// installs input (InputState) first.
mce::ResultV install(mce::World& world);

// world.state_hash folded with the physics world and the run state.
u64 hash_game(mce::World& world, u64 seed);

// The time of day (hours, 17.5 .. 30.5), from Progress.
f32 current_hour(mce::World& world);

// A fresh run in place (the title's NEW GAME): Progress, every Heliostat and Sunshard back to
// their authored state, the Keeper back on the spawn, the mode to Playing.
void reset_run(mce::World& world);

}  // namespace solstice_reach
