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
//
// Replication as data (M-NET2 step 7, mce/ecs/replicated.h): an entity a scene marks NetReplicated
// gets its NetId from assign_tagged(), with no code. Every reflected component field flagged
// Replicated (MCE_COMPONENT(Health, (hp, Replicated))) travels in the snapshot after the game's
// extra bytes, and the client writes those fields into its mirror (adding the component when
// missing, leaving the other fields alone). Entity fields travel as NetIds and come back as the
// client's own entities (null when the target is not replicated). A component whose replicated
// fields differ between server and client builds is skipped and counted, never misread.
//
// Per-client snapshots (M-NET3 step 1): write_snapshot_for() writes one client's snapshot. It lists
// only the entities relevant to that client (NetRelevance, mce/ecs/replicated.h) and codes each one
// against the newest snapshot the client acknowledged, so an entity that did not change costs two
// bytes and a moving one only its changed transform floats. The client acknowledges with write_ack()
// in a message it already sends (its inputs); the server passes it to read_ack(). apply() takes both
// forms. Until an acknowledgement arrives, or when it is too old, the client gets a full snapshot.
//
//   server, per send tick, per client:   rep.write_snapshot_for(world, tick, peer, w); host.send(peer, 1, w.data(), Sequenced);
//   server, on a client message:          rep.read_ack(peer, reader);
//   client, in each input message:        client.write_ack(w);
//   server, on disconnect:                rep.forget_client(peer);

#include "mce/containers/hash_map.h"
#include "mce/ecs/replicated.h"
#include "mce/ecs/transform.h"
#include "mce/ecs/world.h"
#include "mce/memory/allocator.h"
#include "mce/net/bytes.h"
#include "mce/net/transport.h"
#include "mce/refl/type.h"

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
    // Assigns a NetId to every entity marked NetReplicated that has none yet (kind from the
    // component), in entity order; returns how many. Call after loading a scene, or every tick to
    // pick up entities spawned with NetReplicated. Outside a phase / inside one through `cmd`.
    u32 assign_tagged(World& world);
    u32 assign_tagged(Commands& cmd, World& world);

    // Appends one snapshot of every entity with NetId + LocalTransform, in id order. Returns its
    // content hash: the client's content_hash() after applying it.
    u64 write_snapshot(World& world, u32 tick, ByteWriter& out) const;

    // Per client (see the top of this file): the entities relevant to `peer`, delta-coded against
    // the newest snapshot it acknowledged, or in full. Returns the content hash, as above.
    u64 write_snapshot_for(World& world, u32 tick, PeerId peer, ByteWriter& out);
    // Reads what ReplicationClient::write_ack wrote. False (and nothing changes) when malformed.
    bool read_ack(PeerId peer, ByteReader& in);
    // Drops the client's history: its next snapshot is full. Call on disconnect.
    void forget_client(PeerId peer);
    struct ClientStats {
        u32 snapshots = 0;  // written by write_snapshot_for
        u32 full = 0;       // of those, full (no usable acknowledgement)
        u32 entities = 0;   // relevant entities in the newest one
        u64 bytes = 0;      // total written
    };
    // Null for a peer write_snapshot_for has not written to.
    const ClientStats* client_stats(PeerId peer) const;
    // Snapshots kept per client (and applied ones kept by the client) to code against: an
    // acknowledgement older than this many sends gets a full snapshot.
    static constexpr u32 kBaselines = 32;

    void set_hooks(const ReplicationHooks& hooks) { hooks_ = hooks; }

    // The canonical entry bytes of one snapshot and where each entity's entry starts (by id).
    struct Canonical {
        struct Ref {
            u32 id = 0;
            u32 offset = 0;
            u32 size = 0;
        };
        u32 tick = 0;
        u64 hash = 0;
        Array<u8> bytes;
        Array<Ref> refs;  // in id order
        const Ref* find(u32 id) const;
    };

private:
    struct Client {
        PeerId peer = kInvalidPeer;
        Canonical sent[kBaselines];  // a ring of the newest snapshots written to it
        u32 head = 0;                // next slot to write
        u32 count = 0;
        i32 baseline = -1;           // slot of the newest acknowledged one still kept
        ClientStats stats;
    };
    Client* client(PeerId peer);
    void build(World& world, u32 tick, PeerId peer, Canonical& out) const;

    ReplicationHooks hooks_;
    u32 next_id_ = 1;
    Array<mem::UniquePtr<Client>> clients_;
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
    u32 components = 0;          // components whose Replicated fields were written
    u32 skipped_components = 0;  // unknown here, or with different Replicated fields
    bool delta = false;              // the snapshot was coded against an earlier one
    bool missing_baseline = false;   // ...that this client no longer has (or never applied): ignored
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
    // The server tick (fractional) the last update() drew; without interpolation, the newest
    // applied snapshot's tick.
    f64 render_tick() const;
    // The server tick whose state this entity shows: the render tick while it is interpolated or
    // extrapolated, the snapshot's own tick while it is held. Send it with a shot at the entity for
    // lag compensation (lag_compensation.h). The newest tick when not interpolating; 0 when unknown.
    f64 drawn_tick(u32 net_id) const;

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
    // Of the newest applied snapshot: what the server's write_snapshot(_for) returned for it.
    u64 content_hash() const { return content_hash_; }
    // The acknowledgement of the newest applied snapshot, for the server's read_ack (12 bytes).
    void write_ack(ByteWriter& out) const;
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
        f64 drawn_tick = 0.0;   // the server tick update() drew it at
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
        Span<const u8> fields;
    };
    void apply_fields(Commands& cmd, const World& world, Entity e, Span<const u8> fields, ApplyResult& result);
    ReplicationServer::Canonical history_[ReplicationServer::kBaselines];  // applied snapshots, a ring
    u32 history_head_ = 0;
    u32 history_count_ = 0;
    ReplicationServer::Canonical canon_;  // the snapshot being applied
    u64 content_hash_ = 0;
    Array<u8> scratch_;  // one component's bytes while its fields are written
    ReplicationHooks hooks_;
    HashMap<u32, Mirror> entities_;
    Array<Entry> entries_;  // scratch: the snapshot being applied
    Array<u32> predicted_;  // set_predicted ids
    u32 generation_ = 0;
    u32 last_tick_ = 0;
    bool has_tick_ = false;

    void sample_arrival(u32 tick);
    LocalTransform sample(const Mirror& m, f64 render_tick, InterpolationStats& stats, f64& drawn_tick) const;

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
// Version 2 (M-NET2 step 7) adds each entry's Replicated fields after its extra bytes.
inline constexpr u8 kSnapshotMagic[3] = {'M', 'S', 'N'};
inline constexpr u8 kSnapshotVersion = 2;
// Delta snapshots (M-NET3 step 1): "MSD" magic, version, tick, content hash, baseline tick and
// hash, entity count, then per entity its id gap, a change mask and what changed.
inline constexpr u8 kDeltaMagic[3] = {'M', 'S', 'D'};
inline constexpr u8 kDeltaVersion = 1;

// The Replicated fields of a reflected type, hashed by name, kind and size (0: it has none). Server
// and client compare it so a component built differently on each side is skipped, not misread.
u32 replicated_layout(const refl::TypeRecord& type);

}  // namespace mce::net
