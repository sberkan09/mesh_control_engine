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
//
// Snapshot interpolation (M-NET2 step 2): without it the client shows each snapshot as it lands, so
// jitter and loss make remote entities stutter. With set_interpolation(), apply() only records
// each entity's transform with its server tick, and update() — called every client tick — draws
// them a little in the past (the jitter buffer), blending the two snapshots around that time:
//
//   client.set_interpolation({});                          // adaptive delay, 60 Hz server ticks
//   on a snapshot:   client.apply(cmd, world, bytes);
//   every tick:      client.update(cmd, world, dt);       // writes LocalTransform
//
// The delay adapts to the measured snapshot interval and arrival jitter (or is fixed); the render
// time is steered toward it by speeding up or slowing down at most 10%, never by a jump. When the
// render time passes the newest snapshot, an entity moves on with its last velocity for at most
// max_extrapolation_s, then holds the newest snapshot's transform exactly. Everything runs on the
// client's own tick time, so a run over SimNetwork stays deterministic.
//
// Client-side prediction (M-NET2 step 3, mce/net/prediction.h): the entity the local player
// controls is drawn by the game's Predictor, a round trip ahead of the snapshots. set_predicted()
// tells apply() and update() to leave its LocalTransform alone; latest_transform() gives the
// server's word on it for Predictor::reconcile().

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

struct InterpolationDesc {
    // Server ticks per second: snapshot tick numbers are converted to time with it.
    f64 tick_rate = 60.0;
    // How far behind the server the client draws. 0: adaptive, two snapshot intervals (one may be
    // lost) plus the measured arrival jitter (a decaying peak of how late snapshots run), kept within
    // [min_delay_s, max_delay_s].
    f64 delay_s = 0.0;
    f64 min_delay_s = 0.05;
    f64 max_delay_s = 0.35;
    // Past the newest snapshot an entity moves on with its last velocity this long, then holds.
    f64 max_extrapolation_s = 0.1;
};

struct InterpolationStats {
    f32 delay_s = 0.0f;     // the jitter buffer in use
    f32 jitter_s = 0.0f;    // measured arrival jitter
    f32 interval_s = 0.0f;  // measured time between snapshots
    u32 interpolated = 0;   // entities between two snapshots this update
    u32 extrapolated = 0;   // past the newest snapshot, moving on
    u32 held = 0;           // past the extrapolation limit, or with one snapshot only
    u32 predicted = 0;      // drawn by the game's Predictor (set_predicted), not interpolated
};

struct ApplyResult {
    bool ok = false;  // false: malformed, or older than the last applied snapshot (ignored)
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

    // Turns snapshot interpolation on (see the top of this file). Call before the first apply().
    void set_interpolation(const InterpolationDesc& desc);
    bool interpolating() const { return interpolate_; }
    // Interpolating: moves the client's clock by dt_s and writes every mirrored entity's
    // LocalTransform for the render time. Not interpolating: does nothing.
    InterpolationStats update(Commands& cmd, const World& world, f64 dt_s);
    const InterpolationStats& interpolation_stats() const { return stats_; }

    // The entity with this server id is predicted by the game (prediction.h): apply() and update()
    // no longer write its LocalTransform. Can be set before the entity appears.
    void set_predicted(u32 net_id, bool predicted = true);
    bool is_predicted(u32 net_id) const;
    // The transform the newest applied snapshot gave this entity (null when not replicated yet).
    const LocalTransform* latest_transform(u32 net_id) const;

    // The local entity for a server id (null when not replicated yet).
    Entity find(u32 net_id) const;
    usize size() const { return entities_.size(); }
    u32 last_tick() const { return last_tick_; }
    // Forgets every mirrored entity, destroying them (call it on disconnect); the next snapshot,
    // from any server session, rebuilds the mirror. Predicted ids are forgotten too.
    void reset(Commands& cmd, const World& world);

    void set_hooks(const ReplicationHooks& hooks) { hooks_ = hooks; }

private:
    static constexpr u32 kHistory = 16;
    struct Sample {
        u32 tick = 0;
        LocalTransform transform;
    };
    struct Mirror {
        Entity entity;
        NetId id;
        u32 seen = 0;  // the snapshot generation that last listed it
        bool predicted = false;
        LocalTransform latest;  // from the newest applied snapshot
        // Interpolating: the newest snapshots of this entity, oldest first (a ring).
        Sample history[kHistory];
        u32 head = 0;  // index of the oldest
        u32 count = 0;
        void push(u32 tick, const LocalTransform& t);
        const Sample& at(u32 i) const { return history[(head + i) % kHistory]; }
    };
    struct Entry {
        NetId id;
        LocalTransform transform;
        Span<const u8> extra;
    };
    ReplicationHooks hooks_;
    HashMap<u32, Mirror> entities_;
    Array<Entry> entries_;  // scratch: the snapshot being applied
    Array<u32> predicted_;  // set_predicted ids
    u32 generation_ = 0;
    u32 last_tick_ = 0;
    bool has_tick_ = false;

    void sample_arrival(u32 tick);
    LocalTransform sample(const Mirror& m, f64 render_tick, InterpolationStats& stats) const;

    InterpolationDesc interp_;
    bool interpolate_ = false;
    f64 clock_s_ = 0.0;   // the client's own time, moved by update()
    f64 offset_s_ = 0.0;  // server time minus client time, from the earliest arrivals
    f64 jitter_s_ = 0.0;
    f64 interval_s_ = 0.0;
    bool has_offset_ = false;
    f64 render_s_ = 0.0;  // the server time drawn, steered toward clock + offset - delay
    bool has_render_ = false;
    InterpolationStats stats_;
};

// Snapshot wire format, for tools and tests: "MSN" magic, version, tick, entity count, entries.
inline constexpr u8 kSnapshotMagic[3] = {'M', 'S', 'N'};
inline constexpr u8 kSnapshotVersion = 1;

}  // namespace mce::net
