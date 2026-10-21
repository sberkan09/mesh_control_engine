#pragma once
// Behavior trees (M-AI 1, ADR-0045): game AI as data. A .mcbt asset (asset::BtView) holds the
// tree and the blackboard keys it reads and writes; an AiBrain component runs it on its entity and
// a Blackboard component holds the key values.
//
//   kit::install_ai(world, &assets);                          // registry + "ai_brains" (install_stock does it)
//   world.resource<kit::BtRegistry>().register_task("swing", &swing);  // the only code hook
//   ... an entity with AiBrain{tree = guid of guard.mcbt}
//
// "ai_brains" is one FixedUpdate_Pre system, ahead of the path and navmesh agents so a move the
// tree asks for starts the same tick. Every tick an active brain advances its clock by dt and ticks
// its tree from the root:
//   - a node returns Running, Success or Failure; a Running node is resumed on the next tick (a
//     sequence carries on at its running child, a selector too: composites keep their place);
//   - a node that was not running enters fresh: its subtree's results clear and its timers start;
//   - when the root finishes, AiBrain::result takes its result and the tree starts again from the
//     root on the next tick, so a tree never runs twice in one tick;
//   - a node that stops running because a parent ended it (a timeout, a parallel that finished, a
//     restart) is halted: a running task hears it once with BtTaskCall::halting set.
// Timed nodes count the ticks they ran, like sequences: Wait 0.5 s completes on its 30th tick at
// 60 Hz (within a thousandth of a tick), and Timeout 1 s fails its child on the 60th. Repeat and
// Retry start their child again on the next tick, never the same one, so no tree loops in a tick.
// Everything a brain knows is in its two components, so a tree is deterministic, in the state hash
// and replays, and a save resumes it on the same node.
//
// Guard observers (M-AI 2, asset::BtAbort): a guard with "abort" re-checks its condition every tick.
// "self" stops its running child the tick the condition fails (the guard fails); "lower", on a
// selector's child, stops the selector's running later branch the tick it holds, and the selector
// runs the guard's branch instead; "both" does the two. The halted tasks hear it as any halt.
//
// Stock tasks (M-AI 2): install_ai registers these names, so a tree reaches the kit with no code
// (a game may register its own function under a name to replace one):
//   move_to     walks to a vec3 key or an entity key (chased while it moves) with the entity's
//               NavAgent3D (navmesh) or PathAgent (grid); Running until it arrives (or comes
//               within `radius`), Failure on no path or no agent; a halt stops the agent
//   look_at     turns the entity to a vec3 or entity key: about +Z with a PathAgent, Sprite or
//               AnimatedSprite (the 2D plane, +X forward), else about +Y (-Z forward); Success
//   animate     plays `clip` on the entity's AnimatedSprite, or on its Animator without a graph;
//               with `finish`, Running until the clip finishes (a Once clip); Failure without one
//   play_sound  an audio::PlaySfx of `sound` at `gain_db` (needs audio installed); Success
//   spawn       instantiates `prefab` at a vec3 or entity key, or at the entity, plus `offset`
//   raise       a BtSignal and a kit::SeqSignal named `event` (sequences' wait_event sees it)
//   wait_event  Running until a BtSignal or SeqSignal named `event` is raised (readable from the
//               tick after it was raised), then Success
//   log         writes `text` to the log at info level, with the entity and the tree; Success
//   make_noise  a kit::Noise of `loudness` metres at the entity (perception.h), in its Stimulus's
//               (else its Perceiver's) team; Success
//   find_point  a point query (M-AI 4): `rings` x `count` points around `around` (or the entity)
//               out to `radius`, kept on the navmesh (3D, when there is one) or on walkable grid
//               cells (2D), optionally only those `visible` or `hidden` from `from`; the one nearest
//               the entity ("near"), farthest from `from` ("far") or nearest to it ("close") goes to
//               `store`; Failure when no point passes
//
// Utility (M-AI 4): a utility node scores each of its score children every tick (`base` plus, per
// consideration, `weight` x a curve of a blackboard key) and runs the best; while one runs, another
// takes over only by beating its score by more than `hysteresis`, so the choice does not flicker. A
// child whose score is 0 or less is not picked. A token node takes a token of the AiTokenPool it
// names when it starts (failing when the pool is full) and gives it back when its child finishes or
// is halted, so at most `capacity` brains run that branch at once.

#include "mce/asset/asset_system.h"
#include "mce/asset/formats.h"
#include "mce/ecs/world.h"
#include "mce/math/math.h"
#include "mce/refl/macros.h"

namespace mce::kit {

enum class BtStatus : u8 { Idle, Running, Success, Failure };
MCE_ENUM(BtStatus, Idle, Running, Success, Failure)

// The values of a tree's keys. Bool, Int and Float keys share `numbers` (bools as 0 / 1), each type
// at the slot the cook gave the key; `set` has a bit per slot (numbers 0-15, vectors 16-23,
// entities 24-31). An AiBrain whose tree has keys adds one when its entity has none (on the
// brain's first tick; the tree starts on the next) and writes the keys' defaults that are not set
// yet when it starts. Read and write keys by name with bb_* below.
struct Blackboard {
    f32 numbers[asset::kBtNumbers] = {};
    math::vec3 vectors[asset::kBtVectors];
    Entity entities[asset::kBtEntities];
    u32 set = 0;
    u32 pad_ = 0;
};
MCE_COMPONENT(Blackboard, (numbers, Save), (vectors, Save), (entities, Save), (set, Save, Help{"a bit per slot: numbers 0-15, vectors 16-23, entities 24-31"}))

// Runs a .mcbt behavior tree (kit::install_ai). Set `restart` to start again from scratch (the
// engine clears it); clear `active` to pause the tree where it is. The fields after `restart` are
// runtime state with the Save flag (give the entity a SaveKey): `nodes` holds each node's status
// (bits 0-1, a BtStatus) and a composite's current child (bits 2-7), `slots` the timers and
// counters of the tree's timed nodes and the memory of its tasks.
struct AiBrain {
    asset::AssetRef<asset::BehaviorTree> tree;
    bool active = true;
    bool restart = false;
    bool started = false;              // runtime: the defaults are written and the tree runs
    BtStatus result = BtStatus::Idle;  // runtime: how the root last finished
    u32 runs = 0;                      // runtime: how many times the root finished
    f32 time = 0.0f;                   // runtime: seconds the brain has run
    u32 tree_hash = 0;                 // runtime: the tree the state belongs to (a new tree resets it)
    u8 nodes[asset::kBtMaxNodes] = {};
    f32 slots[asset::kBtMaxSlots] = {};
};
MCE_COMPONENT(AiBrain, tree, (active, Save, Help{"cleared: the tree pauses where it is"}),
              (restart, Help{"start the tree again from scratch (the engine clears it)"}), (started, Save, ReadOnly),
              (result, Save, ReadOnly), (runs, Save, ReadOnly), (time, Save, ReadOnly), (tree_hash, Save, ReadOnly),
              (nodes, Save, ReadOnly), (slots, Save, ReadOnly))

// Tokens for token nodes (M-AI 4): at most `capacity` (up to kMaxHolders) brains hold one at a
// time. A token node finds its pool on the entity its key names (a target that limits its own
// attackers), else the first pool with its name in ECS order (a squad-wide limit). Holders that
// were destroyed are dropped when a brain next asks.
struct AiTokenPool {
    static constexpr u32 kMaxHolders = 8;
    FixedString<24> name;
    u32 capacity = 1;
    u32 held = 0;                 // engine-written
    Entity holders[kMaxHolders];  // engine-written
};
MCE_COMPONENT(AiTokenPool, (name, Help{"what token nodes ask for: \"attack\""}), (capacity, Range{0, 8}, Help{"brains holding a token at once"}),
              (held, Save, ReadOnly), (holders, Save, ReadOnly))

// An emit node: `id` is the signal, `entity` the brain's. Readable from the next tick.
struct BtSignal {
    StringId id;
    Entity entity;
};

struct BtTaskCall;
using BtTaskFn = BtStatus (*)(BtTaskCall& call);

class BtRegistry {
public:
    BtRegistry() = default;
    explicit BtRegistry(asset::AssetSystem* assets) : assets_(assets) {}
    void set_assets(asset::AssetSystem* assets) { assets_ = assets; }
    asset::AssetSystem* assets() const { return assets_; }

    struct Def {
        asset::Guid guid;
        asset::AssetKey key = asset::kInvalidAssetKey;
        u32 generation = 0;
        u32 hash = 0;  // of the cooked payload: AiBrain::tree_hash
        FixedString<32> name;
        u32 slot_count = 0;
        Array<asset::BtKey> keys;
        Array<asset::BtNode> nodes;
        Array<asset::BtConsideration> considerations;
        Array<char> strings;
        StringView string(u32 offset) const;
        // The key named `name`, or asset::kBtNoKey.
        u16 find_key(StringId key_name) const;
    };

    // The tree a brain runs: its cooked asset (copied out once, re-read on hot reload), or one
    // registered with define. Null when it does not resolve (warned once).
    const Def* resolve(const AiBrain& b);
    // Registers (or replaces) the tree run for `guid` from a CHNK_BT payload, ahead of the asset
    // system: tools preview unsaved edits through it, tests skip the cook.
    Result<void> define(asset::Guid guid, Span<const byte> payload);

    // The task table (stable names, part of a game module's contract).
    void register_task(StringView name, BtTaskFn fn);
    BtTaskFn task(StringId id) const;
    // The task ids the resolved trees name that have no registered function, as an Errc::NotFound
    // naming the first.
    Result<void> validate_tasks() const;

    // A task node whose name has no function fails; it is logged once per name.
    void warn_missing(StringId id, StringView name);

private:
    Result<void> copy(Def& out, asset::Guid guid, Span<const byte> payload) const;
    asset::AssetSystem* assets_ = nullptr;
    Array<Def> defs_;
    Array<asset::Guid> warned_;
    struct Task {
        StringId id;
        BtTaskFn fn;
    };
    Array<Task> tasks_;
    Array<StringId> warned_tasks_;
};

// What a task node's function gets. `memory` is the node's own slot (zero when it enters);
// `entering` is set on the tick the node starts, `halting` when a parent ends it while it runs
// (the result is then ignored). The node's arguments are `key` (a blackboard key index into
// `tree.keys`, or asset::kBtNoKey), `value`, `vector` and `arg` (a string; empty when none).
struct BtTaskCall {
    Frame& frame;
    Entity entity;
    Blackboard* blackboard;  // null when the tree has no keys
    const BtRegistry::Def& tree;
    const asset::BtNode& node;
    f32& memory;
    bool entering = false;
    bool halting = false;

    u16 key() const { return node.key; }
    f32 value() const { return node.a; }
    math::vec3 vector() const { return {node.v[0], node.v[1], node.v[2]}; }
    StringView arg() const { return node.arg == asset::kBtNoString ? StringView() : tree.string(node.arg); }
};

// The place a task's key names: a set Vec3 key's value, or a live Entity key's world position.
// False when the node has no key, the key is unset or of another type, or the entity is gone;
// `target` (when given) gets the entity of an Entity key.
bool bt_key_position(const BtTaskCall& call, math::vec3& out, Entity* target = nullptr);
// The same for any of the node's keys (`key2`, `key3`).
bool bt_key_position(const BtTaskCall& call, u16 key, math::vec3& out, Entity* target = nullptr);

// A utility node's children's scores as the runner computes them (the debugger's view); `out` gets
// one per child. False when `node` is not a utility node of `e`'s tree.
bool bt_utility_scores(World& w, Entity e, u32 node, Array<f32>& out);

// Registers AiBrain, Blackboard, AiTokenPool and BtSignal, installs perception (kit/perception.h), sets the BtRegistry resource and adds "ai_brains"
// to FixedUpdate_Pre, with the stock tasks registered. Idempotent (re-points the assets).
void install_ai(World& world, asset::AssetSystem* assets);

// The system itself (exposed for tests and custom pipelines).
void ai_brains(Frame& f);

// A node's status in `b` (the debugger's view): Idle when it has not run since its parent entered.
BtStatus bt_node_status(const AiBrain& b, u32 node);

// Blackboard keys of `e` by name, through its brain's tree. The setters fail when `e` has no
// resolvable brain or Blackboard, the tree has no such key, or the key is of another type (a
// number setter writes Bool, Int and Float keys; bools as 0 / 1). The getters fail on an unset key
// too, and bb_entity on an entity that is no longer alive.
bool bb_set_number(World& w, Entity e, StringView key, f32 value);
bool bb_set_vector(World& w, Entity e, StringView key, math::vec3 value);
bool bb_set_entity(World& w, Entity e, StringView key, Entity value);
bool bb_clear(World& w, Entity e, StringView key);
bool bb_number(World& w, Entity e, StringView key, f32& out);
bool bb_vector(World& w, Entity e, StringView key, math::vec3& out);
bool bb_entity(World& w, Entity e, StringView key, Entity& out);

}  // namespace mce::kit
