#pragma once
// The arena server (M-NET3 step 5): authoritative movement and shots over any net::Transport (a UDP
// Host, a SimNetwork endpoint, or a relay room it is the host of). It loads the arena from the scene
// in the cooked pack, gives every NetReplicated entity its NetId, and keeps hit boxes of the last
// half second so a shot is checked against what its client was drawing (lag compensation).

#include "arena.h"
#include "mce/asset/asset_system.h"
#include "mce/hash.h"
#include "mce/scene/scene.h"

namespace arena {

// The server's side of one player (not replicated).
struct ArenaPlayer {
    net::PeerId peer = net::kInvalidPeer;
    net::InputReceiver<Input> inputs;
    u32 fire_ready = 0;  // the tick the next shot is allowed
    u32 respawn_at = 0;  // the tick a dead player comes back (0: alive)
    u32 slot = 0;        // the spawn pad it started at
};

struct ServerStats {
    u32 joins = 0;
    u32 shots = 0;
    u32 hits_rewound = 0;  // shots that hit a player where its client drew it
    u32 hits_present = 0;  // the same shots against where the server has it now
    u32 too_old = 0;       // views older than the rewind window (checked at its edge)
    u32 refused = 0;       // shots while dead or inside the fire delay
    u32 kills = 0;
    u64 bytes_sent = 0;  // snapshot bytes written, and what full broadcasts would have cost
    u64 bytes_full = 0;
};

class Server {
public:
    World world;
    net::ReplicationServer replication;
    net::LagCompensator lag;
    Map map;
    ServerStats stats;
    u64 tick = 0;
    u64 snapshot_hash = 0;  // content hash of the newest snapshot (clients get the same entities)

    // Loads the arena scene and starts serving over `transport` (which must outlive the server).
    Result<void> start(net::Transport& transport, asset::AssetSystem& assets, StringView scene_path) {
        net_ = &transport;
        MCE_TRY_ASSIGN(const asset::Guid guid, scene::find_scene(assets, scene_path));
        MCE_TRY(scene::load(world, assets, guid));
        // Walls take part in shots (they stop a ray), so they get hit boxes the size of their scale.
        Array<Entity> walls;
        world.query<const NetReplicated>().each([&](Entity e, const NetReplicated& r) {
            if (r.kind == kKindWall) walls.push_back(e);
        });
        for (const Entity e : walls) world.add(e, net::HitBox{{0.5f, 0.5f, 0.5f}, {}});
        replication.assign_tagged(world);
        lag.init(net::LagCompensationDesc{60.0, kRewindSeconds});
        map.rebuild(world);
        net::ReplicationHooks hooks;
        hooks.write_extra = [](void*, const World& w, Entity e, net::ByteWriter& out) {
            if (const ArenaPlayer* p = w.try_get<ArenaPlayer>(e)) out.write_u32(p->inputs.last_seq());  // the ack
        };
        replication.set_hooks(hooks);
        return ok();
    }

    // Network in, simulate one tick, snapshots out.
    void step() {
        receive();
        respawn();
        world.tick(kDt, ++tick);
        lag.record(world, static_cast<u32>(tick));
        if (tick % kSnapshotEvery == 0) send_snapshots();
    }

    // Everything the network delivered, without simulating (the last snapshots of a run).
    void receive() {
        for (net::Event ev; net_->poll(ev);) {
            if (ev.kind == net::EventKind::Disconnected) {
                if (const Entity e = player_of(ev.peer); !e.is_null()) world.destroy(e);
                replication.forget_client(ev.peer);
            } else if (ev.kind == net::EventKind::Received) {
                net::ByteReader r(ev.data);
                const u8 msg = r.read_u8();
                if (!r.ok()) continue;
                if (msg == kMsgJoin) join(ev.peer);
                else if (msg == kMsgAck) replication.read_ack(ev.peer, r);
                else if (msg == kMsgInput) input(ev.peer, ev.data.subspan(1));
                else if (msg == kMsgShot) shot(ev.peer, r);
            }
        }
    }

    // Writes (and sends) each player's snapshot of the current state.
    void send_snapshots() {
        net::ByteWriter w;
        const u32 t = static_cast<u32>(tick);
        snapshot_hash = replication.write_snapshot(world, t, w);
        const usize full = w.size();
        world.query<const ArenaPlayer>().each([&](const ArenaPlayer& p) {
            w.clear();
            replication.write_snapshot_for(world, t, p.peer, w);
            net_->send(p.peer, kChannelSnapshot, w.data(), net::Delivery::Sequenced);
            stats.bytes_sent += w.size();
            stats.bytes_full += full;
        });
        net_->flush();
    }

    u32 players() {
        u32 n = 0;
        world.query<const ArenaPlayer>().each([&](const ArenaPlayer&) { ++n; });
        return n;
    }
    Entity player_of(net::PeerId peer) {
        Entity found = Entity::null;
        world.query<const ArenaPlayer>().each([&](Entity e, const ArenaPlayer& p) {
            if (p.peer == peer) found = e;
        });
        return found;
    }
    Entity entity_of(u32 net_id) {
        Entity found = Entity::null;
        world.query<const net::NetId>().each([&](Entity e, const net::NetId& n) {
            if (n.id == net_id) found = e;
        });
        return found;
    }

private:
    void join(net::PeerId peer) {
        if (!player_of(peer).is_null() || players() >= kMaxPlayers) return;
        const u32 slot = stats.joins++;
        LocalTransform t;
        const math::vec2 pad = map.pad(slot);
        t.position = {pad.x, pad.y, 0.0f};
        ArenaPlayer p;
        p.peer = peer;
        p.slot = slot;
        const Entity e = world.spawn_with(t, p, ArenaHealth{}, NetOwner{peer}, net::HitBox{{kRadius, kRadius, 0.5f}, {}});
        const u32 id = replication.assign(world, e, kKindPlayer);
        net::ByteWriter welcome;
        welcome.write_u8(kMsgWelcome);
        welcome.write_u32(id);
        net_->send(peer, kChannelControl, welcome.data(), net::Delivery::Reliable);
    }

    void input(net::PeerId peer, Span<const u8> packet) {
        const Entity e = player_of(peer);
        if (e.is_null()) return;
        ArenaPlayer& p = *world.try_get_mut<ArenaPlayer>(e);
        const bool alive = world.try_get<ArenaHealth>(e)->hp > 0;
        State s{*world.try_get<LocalTransform>(e)};
        p.inputs.receive(packet, [&](const Input& in) {
            Input i = in;
            if (!alive) i.move_x = i.move_y = 0.0f;  // the dead stand where they fell (the client predicts the same)
            step_player(&map, s, i, static_cast<f32>(kDt));
        });
        *world.try_get_mut<LocalTransform>(e) = s.transform;
    }

    void shot(net::PeerId peer, net::ByteReader& r) {
        const u64 view_bits = r.read_u64();
        const f32 aim = r.read_f32();
        const Entity e = player_of(peer);
        if (!r.ok() || e.is_null() || !std::isfinite(aim)) return;
        ArenaPlayer& p = *world.try_get_mut<ArenaPlayer>(e);
        ArenaHealth& me = *world.try_get_mut<ArenaHealth>(e);
        ++stats.shots;
        if (me.hp == 0 || tick + 3 < p.fire_ready) {  // 3 ticks of slack: jitter bunches shots up
            ++stats.refused;
            return;
        }
        p.fire_ready = static_cast<u32>(tick) + kFireTicks;
        ++me.shots;  // clients draw a beam from the shooter when this goes up
        f64 view = 0.0;
        std::memcpy(&view, &view_bits, sizeof view);
        if (!std::isfinite(view)) view = static_cast<f64>(tick);
        if (lag.clamp_view(view) > view) ++stats.too_old;
        const net::NetId shooter = *world.try_get<net::NetId>(e);
        const math::vec3 origin = world.try_get<LocalTransform>(e)->position;
        const math::vec3 dir{std::cos(aim), std::sin(aim), 0.0f};
        net::RewindHit hit;
        if (lag.raycast(view, origin, dir, kShotRange, hit, shooter.id)) {
            const Entity target = entity_of(hit.net_id);
            if (!target.is_null() && world.has<ArenaPlayer>(target)) {
                ++stats.hits_rewound;
                damage(e, target);
            }
        }
        if (lag.raycast(static_cast<f64>(tick), origin, dir, kShotRange, hit, shooter.id)) {
            const Entity target = entity_of(hit.net_id);
            if (!target.is_null() && world.has<ArenaPlayer>(target)) ++stats.hits_present;
        }
    }

    void damage(Entity shooter, Entity target) {
        ArenaHealth& h = *world.try_get_mut<ArenaHealth>(target);
        if (h.hp == 0) return;
        if (h.hp > kShotDamage) {
            h.hp -= kShotDamage;
            return;
        }
        h.hp = 0;
        ++h.deaths;
        ++world.try_get_mut<ArenaHealth>(shooter)->frags;
        world.try_get_mut<ArenaPlayer>(target)->respawn_at = static_cast<u32>(tick) + kRespawnTicks;
        ++stats.kills;
    }

    // A dead player comes back at the next pad.
    void respawn() {
        Array<Entity> back;
        world.query<const ArenaPlayer, const ArenaHealth>().each([&](Entity e, const ArenaPlayer& p, const ArenaHealth& h) {
            if (h.hp == 0 && tick + 1 >= p.respawn_at) back.push_back(e);
        });
        for (const Entity e : back) {
            ArenaHealth& h = *world.try_get_mut<ArenaHealth>(e);
            h.hp = kMaxHealth;
            const ArenaPlayer& p = *world.try_get<ArenaPlayer>(e);
            const math::vec2 pad = map.pad(p.slot + h.deaths);
            LocalTransform& t = *world.try_get_mut<LocalTransform>(e);
            t.position = {pad.x, pad.y, 0.0f};
        }
    }

    net::Transport* net_ = nullptr;
};

}  // namespace arena
