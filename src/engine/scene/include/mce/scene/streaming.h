#pragma once
// Scene streaming (FR-ENT-120): opt-in chunks of a world that load and unload around the player.
//
// A game that never places a SceneChunk is untouched: the pump finds no chunks and returns. To
// stream, author the world as one small base scene plus one scene per chunk, then
//
//   - put a SceneChunk on an entity of the base scene for each chunk scene: its LocalTransform
//     position is the centre of the chunk's box (half_extents), `scene` names the chunk scene;
//   - put a StreamingFocus on whatever the world streams around (the player, the camera).
//
// Each tick, before World::tick, the host calls pump_streaming: a chunk whose box comes within
// load_distance of any enabled focus is loaded and instantiated; one that is farther than
// unload_distance from every focus has the entities it spawned destroyed. The gap between the two
// distances keeps a player on a border from thrashing. `pinned` keeps a chunk loaded wherever the
// focus is (a hub, a boss room the game is about to enter); `disabled` keeps it unloaded. A game
// module drives both from its systems, and reads SceneChunkStatus to know whether a chunk is in.
//
// Chunk scenes are authored in world coordinates (the chunk entity's transform only places its
// box). The entities a chunk spawns belong to it: unloading destroys them, so state that must
// outlive a chunk lives on base-scene entities or in resources. Save games do not track chunks:
// a SaveKey inside a chunk scene is not supported yet.
//
// mce_player, play-in-editor and the MCP headless run all install and pump streaming, so a
// scene with chunks streams in every host with no game code at all.
//
//   scene::install_streaming(world, assets);   // the SceneStreaming resource
//   scene::pump_streaming(world, scene::PumpMode::Settle);   // after the base scene: load what the
//                                                            // start position needs, now
//   per tick: scene::pump_streaming(world); world.tick(...);
//
// Floating origin (M-OPENWORLD A2): a FloatingOrigin3D on one base-scene entity makes the same pump rebase
// the world (mce/ecs/origin.h) whenever the first enabled StreamingFocus gets farther than `threshold`
// metres from the origin, by the focus position rounded to whole `snap` cells. Chunks keep being authored
// in absolute coordinates: a chunk loaded after a rebase is moved by the origin as it spawns. No
// FloatingOrigin3D: no rebase, ever.
//
// Async (the default in windowed hosts) reads chunk files on the asset loader thread and spreads
// instantiation over ticks (max_instantiations_per_pump), so when a chunk appears depends on disk
// speed. Determinism runs (mce_player -headless, the MCP headless run) set async off: loads then
// complete inside the pump that requested them and the world hash stays a function of the ticks.

#include "mce/asset/asset_system.h"
#include "mce/ecs/world.h"
#include "mce/math/math.h"
#include "mce/refl/macros.h"

namespace mce::scene {

// One streamable chunk: an entity of the base scene (never of a chunk scene).
struct SceneChunk {
    asset::AssetRef<asset::Scene> scene;
    math::vec3 half_extents{16.0f, 16.0f, 16.0f};  // the chunk's box around this entity's position
    f32 load_distance = 8.0f;     // load when a focus comes this close to the box (0 = inside it)
    f32 unload_distance = 16.0f;  // unload when every focus is farther than this (>= load_distance)
    bool pinned = false;          // stay loaded regardless of distance
    bool disabled = false;        // stay unloaded regardless of distance (wins over pinned)
    u8 pad_[2] = {};              // explicit, so state_hash never reads indeterminate padding
};
MCE_COMPONENT(SceneChunk,
    (scene, Help{"the chunk's scene, authored in world coordinates"}),
    (half_extents, Help{"half size of the chunk's box, centred on this entity's position"}),
    (load_distance, Range{0, 10000}, Help{"load when a StreamingFocus comes this close to the box"}),
    (unload_distance, Range{0, 10000}, Help{"unload when every StreamingFocus is farther than this"}),
    (pinned, Help{"keep loaded wherever the focus is"}),
    (disabled, Help{"keep unloaded wherever the focus is"}))

// What the world streams around: the player, the camera. Several are allowed (split screen).
struct StreamingFocus {
    bool enabled = true;
};
MCE_COMPONENT(StreamingFocus, enabled)

// Turns the world rebase on for this scene (one per world; the first one found counts). Layout: padding-free.
struct FloatingOrigin3D {
    f32 threshold = 1024.0f;  // rebase when the focus is farther than this from the origin (at least `snap`)
    f32 snap = 256.0f;        // shifts are whole multiples of this (a power of two keeps textures seamless)
    u32 vertical = 0;         // 1: rebase on y too (space, very deep or high worlds)
    u32 enabled = 1;
};
static_assert(sizeof(FloatingOrigin3D) == 16);
MCE_COMPONENT(FloatingOrigin3D,
    (threshold, Range{16, 100000}, Help{"rebase when the StreamingFocus is this far from the origin"}),
    (snap, Range{1, 4096}, Help{"shift step in metres; a power of two keeps world-anchored textures seamless"}),
    (vertical, Help{"1: rebase on the vertical axis too"}),
    enabled)

enum class ChunkState : u8 { Unloaded, Loading, Loaded, Failed };
MCE_ENUM(ChunkState, Unloaded, Loading, Loaded, Failed)

// Written by the pump on every SceneChunk entity; read-only for games.
struct SceneChunkStatus {
    ChunkState state = ChunkState::Unloaded;
    u8 pad_[3] = {};       // explicit, so state_hash never reads indeterminate padding
    u32 entity_count = 0;  // entities the chunk spawned (loaded chunks)
};
MCE_COMPONENT(SceneChunkStatus, (state, ReadOnly), (entity_count, ReadOnly))

struct StreamingConfig {
    bool async = true;                     // false: load inside the pump that asks (determinism)
    u32 max_instantiations_per_pump = 1;   // chunks instantiated per pump (async mode)
    u32 max_loads_in_flight = 4;           // async reads queued at once
};

struct StreamingStats {
    u32 chunks = 0;
    u32 loaded = 0;
    u32 loading = 0;
    u32 failed = 0;
    u32 streamed_entities = 0;  // alive entities spawned by loaded chunks
    u64 total_loads = 0;
    u64 total_unloads = 0;
};

enum class PumpMode : u8 {
    Budgeted,  // per tick: the config's async / budget rules
    Settle,    // load everything wanted now, synchronously, unbudgeted (start, teleports, loading screens)
};

// The streaming state: a World resource (install_streaming). Movable; holds the asset system by
// pointer, so the AssetSystem must outlive the World.
class SceneStreaming {
public:
    SceneStreaming(asset::AssetSystem* assets, StreamingConfig config);
    ~SceneStreaming();
    SceneStreaming(SceneStreaming&& o) noexcept;
    SceneStreaming& operator=(SceneStreaming&& o) noexcept;
    SceneStreaming(const SceneStreaming&) = delete;
    SceneStreaming& operator=(const SceneStreaming&) = delete;

    void pump(World& world, PumpMode mode);
    // Destroys every streamed entity and drops pending loads (the chunks go back to Unloaded).
    void unload_all(World& world);

    StreamingConfig& config() { return config_; }
    const StreamingStats& stats() const { return stats_; }
    // Entities spawned by `chunk` (a SceneChunk entity) while it is loaded; empty otherwise.
    Span<const Entity> entities(Entity chunk) const;
    void set_assets(asset::AssetSystem* assets) { assets_ = assets; }

private:
    struct Record {
        Entity chunk;
        asset::Guid scene;
        ChunkState state = ChunkState::Unloaded;
        asset::AsyncTicket ticket;
        u64 order = 0;  // request order: ready chunks instantiate first-come, first-served
        Array<Entity> spawned;
    };

    Record* find(Entity chunk);
    const Record* find(Entity chunk) const;
    void unload(World& world, Record& r);
    void cancel(Record& r);
    void instantiate(World& world, Record& r, asset::AnyHandle handle);
    void fail(Record& r, Errc code);

    asset::AssetSystem* assets_ = nullptr;
    StreamingConfig config_;
    StreamingStats stats_;
    Array<Record> records_;
    u64 next_order_ = 1;
};

// Adds the SceneStreaming resource (once; a second call updates the asset system and config).
// Registers nothing: the components arrive with the first scene that uses them.
void install_streaming(World& world, asset::AssetSystem& assets, StreamingConfig config = {});
// Host call, once per tick outside phases (structural: it spawns and destroys). No-op when
// streaming is not installed.
void pump_streaming(World& world, PumpMode mode = PumpMode::Budgeted);

// The rebase step of pump_streaming, for hosts that do not stream: rebases when a FloatingOrigin3D asks
// for it (true when it did). Outside phases.
bool update_floating_origin(World& world);

// Distance from `p` to the box `centre` +- `half` (0 inside).
f32 distance_to_box(const math::vec3& p, const math::vec3& centre, const math::vec3& half);

}  // namespace mce::scene
