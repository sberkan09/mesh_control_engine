#pragma once
// Snapshot replication (ADR-0035): the server is authoritative; every send tick it writes one
// full snapshot of the entities tagged with NetId (their LocalTransform plus optional game
// bytes), and each client mirrors it — spawning entities it has not seen, updating the ones it
// has, destroying the ones the snapshot no longer lists. Full snapshots on a Sequenced channel:
// a lost packet is simply superseded by the next one, so there is no ack bookkeeping.
//
//   server, per send tick:            client, for each Received snapshot (a system, or between ticks):
//     ByteWriter w;                     client.apply(frame.commands(), frame.world(), event.data);
//     server.write_snapshot(world, tick, w);
//     host.broadcast(1, w.data(), Delivery::Sequenced);
//
// Structural changes on the client go through Commands, so apply() is legal inside a phase and
// takes effect at that phase's sync point. Entities the client spawns carry NetId and
// LocalTransform; ReplicationHooks::on_spawn adds the rest (a sprite, a mesh) from NetId::kind.

#include "mce/containers/hash_map.h"
#include "mce/ecs/transform.h"
#include "mce/ecs/world.h"
#include "mce/net/bytes.h"

namespace mce::net {

// Marks an entity as replicated. `id` is assigned by the server (ReplicationServer::assign) and
// names the same object on every client; `kind` is the game's own archetype number, which the
// client's on_spawn uses to build the entity's local-only parts.
struct NetId {
    u32 id = 0;
    u32 kind = 0;
};

struct ReplicationHooks {
    void* user = nullptr;
    // Server: extra per-entity bytes after its transform (health, animation state, ...). They
    // travel length-prefixed, so a client without read_extra (or reading less) stays in step.
    void (*write_extra)(void* user, const World& world, Entity e, ByteWriter& out) = nullptr;
    // Client: reads what write_extra wrote. Runs for new and existing entities; `e` may still be
    // pending (spawned this apply), so write through `cmd`, not the World.
    void (*read_extra)(void* user, Commands& cmd, Entity e, ByteReader& in) = nullptr;
    // Client: a replicated entity appeared (its NetId and LocalTransform are already queued).
    void (*on_spawn)(void* user, Commands& cmd, Entity e, const NetId& id) = nullptr;
    // Client: a replicated entity left the snapshot; it is destroyed right after this call.
    void (*on_despawn)(void* user, Commands& cmd, Entity e, const NetId& id) = nullptr;
};

class ReplicationServer {
public:
    // Tags `e` for replication with the next free id; returns the id. Works inside a phase.
    u32 assign(Commands& cmd, Entity e, u32 kind);
    // Outside a phase (setup code).
    u32 assign(World& world, Entity e, u32 kind);

    // Appends one snapshot of every entity with NetId + LocalTransform, in id order.
    void write_snapshot(World& world, u32 tick, ByteWriter& out) const;

    void set_hooks(const ReplicationHooks& hooks) { hooks_ = hooks; }

private:
    ReplicationHooks hooks_;
    u32 next_id_ = 1;
};

struct ApplyResult {
    bool ok = false;      // false: malformed, or older than the last applied snapshot (ignored)
    u32 tick = 0;
    u32 spawned = 0;
    u32 updated = 0;
    u32 despawned = 0;
};

class ReplicationClient {
public:
    // Mirrors one snapshot into the world through `cmd`. The whole snapshot is validated before
    // anything is queued, so a malformed or stale one changes nothing.
    ApplyResult apply(Commands& cmd, const World& world, Span<const u8> snapshot);

    // The local entity for a server id (null when not replicated yet).
    Entity find(u32 net_id) const;
    usize size() const { return entities_.size(); }
    u32 last_tick() const { return last_tick_; }
    // Forgets every mirrored entity, destroying them (call it on disconnect); the next snapshot,
    // from any server session, rebuilds the mirror.
    void reset(Commands& cmd, const World& world);

    void set_hooks(const ReplicationHooks& hooks) { hooks_ = hooks; }

private:
    struct Mirror {
        Entity entity;
        NetId id;
        u32 seen = 0;  // the snapshot generation that last listed it
    };
    struct Entry {
        NetId id;
        LocalTransform transform;
        Span<const u8> extra;
    };
    ReplicationHooks hooks_;
    HashMap<u32, Mirror> entities_;
    Array<Entry> entries_;  // scratch: the snapshot being applied
    u32 generation_ = 0;
    u32 last_tick_ = 0;
    bool has_tick_ = false;
};

// Snapshot wire format, for tools and tests: "MSN" magic, version, tick, entity count, entries.
inline constexpr u8 kSnapshotMagic[3] = {'M', 'S', 'N'};
inline constexpr u8 kSnapshotVersion = 1;

}  // namespace mce::net
