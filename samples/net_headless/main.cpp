// A server and its clients in one process (mce_net, ADR-0035): the smallest complete multiplayer
// loop the module supports, and its CI check.
//
//   server world: eight drifters circle the origin; a player entity appears for each client that
//                 sends JOIN and moves by that client's inputs, and every 2 s a gust only the server
//                 knows about pushes every player back. Every tick it sends each client its own snapshot,
//                 delta-coded against the one the client acknowledged (M-NET3 step 1; -delta 0: one
//                 full broadcast).
//   client world: sends JOIN, then one input per tick; mirrors the snapshots with
//                 ReplicationClient from a FixedUpdate_Pre system. Its own player is predicted
//                 (M-NET2 step 3): drawn at once from its inputs, reconciled with the server's acks,
//                 the gusts smoothed in (-predict 0: drawn from snapshots like everything else).
//                 Every half second it fires from the origin at drifter 1 where it draws it, and
//                 the server checks the shot against that moment (M-NET2 step 4, lag compensation)
//                 and, for comparison, against its present.
//
// Two networks, one game loop (both are net::Transport):
//   UDP (default)  one client over loopback UDP (net::Host); -latency/-jitter/-loss set
//                  Host::set_conditions on both ends, on the wall clock
//   -sim 1         -clients N over net::SimNetwork (M-NET2 step 1): a virtual clock that moves
//                  1/60 s per tick, -latency/-jitter/-loss on every link, -seed for the rolls.
//                  Nothing depends on the wall clock, so the printed hash is the same on every run
//                  and in every build (scripts/ci/determinism.py, local_ci.sh's determinism stage).
//   -sim 1 -relay 1  the same through a relay (M-NET2 step 6): the server hosts a room on a
//                  net::RelayServer and the clients join it by its code, so every message takes two
//                  hops (-latency applies to each). The game code is the same; only its Transport
//                  is a net::RelayClient.
//
// After -ticks N it lets the last snapshot land and checks that every client's mirror equals the
// server's replicated state bit for bit and that each player moved. Exit code 0 on success.
//
//   sample_net_headless -ticks 300
//   sample_net_headless -sim 1 -clients 4 -latency 75 -jitter 20 -loss 0.05 -ticks 600
//   sample_net_headless -sim 1 -relay 1 -clients 3 -latency 40 -jitter 10 -loss 0.05 -ticks 600
//   sample_net_headless -sim 1 -clients 3 -spectators 2 -latency 75 -ticks 600
//   sample_net_headless -sim 1 -relay 1 -clients 3 -latency 40 -ticks 600 -host_leaves 300
//
// Host migration (M-NET3 step 3, -relay 1 -host_leaves T): the room is made with host migration and
// the host leaves at tick T. The relay names the lowest member the new host; that client's mirror
// becomes the server world (ReplicationServer::adopt, plus the drifters' orbits and its own player,
// which the snapshots do not carry), and the others send JOIN with their player's id to get it
// back. At the end every remaining client must mirror the new host bit for bit.
//
// Spectators (M-NET3 step 2, -sim 1 without -relay): -spectators N more clients send WATCH instead
// of JOIN. They get no player and send no input, only acknowledgements; the server marks them
// set_sees_all and sends them per-client snapshots like the players, and they must mirror
// everything too.

#include <algorithm>
#include <chrono>
#include <cmath>
#include <cstdio>
#include <cstring>
#include <thread>

#include "mce/core.h"
#include "mce/ecs/transform.h"
#include "mce/hash.h"
#include "mce/net/net.h"

using namespace mce;

namespace {

MCE_CVAR(cv_ticks, "ticks", 300, "Ticks to simulate", CVarFlags::None);
MCE_CVAR(cv_sim, "sim", false, "Run over the simulated network (deterministic) instead of loopback UDP",
         CVarFlags::None);
MCE_CVAR(cv_clients, "clients", 3, "-sim: clients that join", CVarFlags::None);
MCE_CVAR(cv_latency, "latency", 0, "One-way latency in ms on every link", CVarFlags::None);
MCE_CVAR(cv_jitter, "jitter", 0, "Extra one-way delay, up to this many ms", CVarFlags::None);
MCE_CVAR(cv_loss, "loss", 0.0f, "Chance a Sequenced/Unreliable message is lost (0..1)", CVarFlags::None);
MCE_CVAR(cv_seed, "seed", 1, "Seed of the latency, jitter and loss rolls", CVarFlags::None);
MCE_CVAR(cv_interp, "interp", true, "Clients draw snapshots interpolated through a jitter buffer (M-NET2 step 2)",
         CVarFlags::None);
MCE_CVAR(cv_predict, "predict", true, "Clients predict their own player and reconcile it (M-NET2 step 3)",
         CVarFlags::None);
MCE_CVAR(cv_delta, "delta", true,
         "Per-client snapshots coded against each client's acknowledged one (M-NET3 step 1); 0: one broadcast",
         CVarFlags::None);
MCE_CVAR(cv_spectators, "spectators", 0, "-sim 1: clients that only watch (no player, no input)", CVarFlags::None);
MCE_CVAR(cv_host_leaves, "host_leaves", 0,
         "-relay 1: the host leaves at this tick and client 0 takes over (M-NET3 step 3 host migration); 0: never",
         CVarFlags::None);
MCE_CVAR(cv_relay, "relay", false, "-sim: everybody meets in a room on a relay instead of connecting to the server (M-NET2 step 6)",
         CVarFlags::None);

constexpr f64 kDt = 1.0 / 60.0;
constexpr u8 kChannels = 3;
constexpr u8 kChannelControl = 0;   // reliable: JOIN
constexpr u8 kChannelSnapshot = 1;  // sequenced: server -> client snapshots
constexpr u8 kChannelInput = 2;     // sequenced: client -> server inputs
constexpr u8 kMsgJoin = 1;
constexpr u8 kMsgInput = 2;
constexpr u8 kMsgWelcome = 3;  // server -> client: the net id of your player
constexpr u8 kMsgShot = 4;     // client -> server: a ray from the origin and the tick the client drew
constexpr u8 kMsgAck = 5;      // client -> server: the newest snapshot applied (delta snapshots)
constexpr u8 kMsgWatch = 6;    // client -> server: a spectator, instead of JOIN
constexpr u64 kShotTicks = 30;
constexpr f32 kShotRange = 20.0f;
constexpr u64 kGustTicks = 120;
constexpr f32 kGust = 0.25f;
constexpr u32 kKindDrifter = 1;
constexpr u32 kKindPlayer = 2;
constexpr u32 kMaxClients = 8;

struct Drifter {
    f32 radius;
    f32 phase;
};
// What a client sends each tick, and the state the client predicts. The step is shared: the
// server runs it for each input it receives, the client for each input it presses and again when
// it replays unacknowledged inputs after a snapshot.
struct PlayerInput {
    f32 move_x;
};
struct PlayerState {
    LocalTransform transform;
};
void step_player(void*, PlayerState& s, const PlayerInput& in, f32 dt) {
    const f32 move = std::isfinite(in.move_x) ? std::fmax(-1.0f, std::fmin(1.0f, in.move_x)) : 0.0f;
    s.transform.position.x += move * dt;
}

struct Player {
    net::PeerId peer;
    f32 move_x;  // the last input
    net::InputReceiver<PlayerInput> inputs;
};

// Mean |second difference| of a path sampled once per tick: 0 for steady motion, large for stutter.
struct Motion {
    math::vec3 p0, p1;
    u32 samples = 0;
    f64 jerk = 0.0;
    u32 counted = 0;
    void add(math::vec3 p) {
        if (samples >= 2) {
            const math::vec3 d = p - p1 - (p1 - p0);
            jerk += std::sqrt(static_cast<f64>(d.x * d.x + d.y * d.y + d.z * d.z));
            ++counted;
        }
        p0 = p1;
        p1 = p;
        ++samples;
    }
    f64 mean() const { return counted ? jerk / counted : 0.0; }
};

// ---- server -----------------------------------------------------------------------------------

struct Server {
    World own_world;
    World* active_world = &own_world;  // a host that took over runs in its client world (adopt)
    World& world() { return *active_world; }
    net::Transport* net = nullptr;
    net::ReplicationServer replication;
    net::ByteWriter snapshot;
    u64 tick = 0;

    // Hit boxes of the last second, so a shot is checked against what its client was drawing.
    // The rewind covers the slowest link CI runs (150 ms ± 40 plus the jitter buffer).
    net::LagCompensator lag;
    u32 shots = 0, hits_rewound = 0, hits_present = 0;
    u32 too_old = 0;  // arrived after the rewind window (a reliable shot lost and resent): checked late
    Array<net::PeerId> watchers;  // spectators: snapshots, no player

    void start(net::Transport& transport) {
        net = &transport;
        lag.init(net::LagCompensationDesc{60.0, 1.0});
        for (u32 i = 0; i < 8; ++i) {
            const Entity e = world().spawn_with(LocalTransform{}, Drifter{1.0f + f32(i), f32(i) * 0.7f},
                                              net::HitBox{{0.15f, 0.15f, 0.15f}, {}});
            replication.assign(world(), e, kKindDrifter);
        }
        add_drift();
        set_hooks();
    }
    void add_drift() {
        world().add_system(Phase::FixedUpdate_Post, "drift", [](Frame& f) {
            f.query<Drifter, LocalTransform>().each([&](Drifter& dr, LocalTransform& t) {
                dr.phase += f.dt;
                t.position = {dr.radius * std::cos(dr.phase), dr.radius * std::sin(dr.phase), 0.0f};
            });
        });
    }
    void set_hooks() {
        net::ReplicationHooks hooks;
        hooks.write_extra = [](void*, const World& w, Entity e, net::ByteWriter& out) {
            if (const Player* p = w.try_get<Player>(e)) out.write_u32(p->inputs.last_seq());  // the ack
        };
        replication.set_hooks(hooks);
    }
    // Host migration (M-NET3 step 3): this client's mirror becomes the server (after Client).
    void adopt(struct Client& heir);
    u32 rebound = 0;  // players that found their entity again on the new host
    u64 replication_start = 0;  // the tick it took over at

    Entity player_of(net::PeerId peer) {
        Entity found = Entity::null;
        world().query<const Player>().each([&](Entity e, const Player& p) {
            if (p.peer == peer) found = e;
        });
        return found;
    }

    void receive() {
        for (net::Event ev; net->poll(ev);) {
            if (ev.kind == net::EventKind::Disconnected) {
                if (const Entity e = player_of(ev.peer); !e.is_null()) world().destroy(e);
                replication.forget_client(ev.peer);
                for (usize i = 0; i < watchers.size(); ++i)
                    if (watchers[i] == ev.peer) {
                        watchers.erase(i);
                        break;
                    }
            } else if (ev.kind == net::EventKind::Received) {
                net::ByteReader r(ev.data);
                const u8 msg = r.read_u8();
                if (msg == kMsgJoin && r.ok() && player_of(ev.peer).is_null()) {
                    // After a host migration a player names the entity it had: it gets it back.
                    const u32 had = r.remaining() == 4 ? r.read_u32() : 0;
                    Entity e = Entity::null;
                    u32 id = 0;
                    world().query<const net::NetId>().each([&](Entity it, const net::NetId& n) {
                        if (had && n.id == had && n.kind == kKindPlayer && !world().has<Player>(it)) e = it;
                    });
                    if (!e.is_null()) {
                        world().add(e, Player{ev.peer, 0.0f, {}});
                        id = had;
                        ++rebound;
                    } else {
                        e = world().spawn_with(LocalTransform{}, Player{ev.peer, 0.0f, {}});
                        id = replication.assign(world(), e, kKindPlayer);
                    }
                    net::ByteWriter welcome;
                    welcome.write_u8(kMsgWelcome);
                    welcome.write_u32(id);
                    net->send(ev.peer, kChannelControl, welcome.data(), net::Delivery::Reliable);
                } else if (msg == kMsgWatch && r.ok() && player_of(ev.peer).is_null()) {
                    if (std::find(watchers.begin(), watchers.end(), ev.peer) == watchers.end()) {
                        watchers.push_back(ev.peer);
                        replication.set_sees_all(ev.peer, true);  // a caster: every entity, whatever its relevance
                    }
                } else if (msg == kMsgAck && r.ok()) {
                    replication.read_ack(ev.peer, r);
                } else if (msg == kMsgInput && r.ok()) {
                    const Entity e = player_of(ev.peer);
                    if (e.is_null()) continue;
                    Player& p = *world().try_get_mut<Player>(e);
                    PlayerState s{*world().try_get<LocalTransform>(e)};
                    p.inputs.receive(ev.data.subspan(1), [&](const PlayerInput& in) {
                        step_player(nullptr, s, in, static_cast<f32>(kDt));
                        if (std::isfinite(in.move_x)) p.move_x = std::fmax(-1.0f, std::fmin(1.0f, in.move_x));
                    });
                    world().try_get_mut<LocalTransform>(e)->position = s.transform.position;
                } else if (msg == kMsgShot) {
                    const u64 view_bits = r.read_u64();
                    math::vec3 dir;
                    dir.x = r.read_f32();
                    dir.y = r.read_f32();
                    dir.z = r.read_f32();
                    if (!r.ok()) continue;
                    f64 view = 0.0;
                    std::memcpy(&view, &view_bits, sizeof view);
                    dir = math::normalize(dir);
                    ++shots;
                    if (lag.clamp_view(view) > view) ++too_old;
                    net::RewindHit hit;
                    if (lag.raycast(view, {}, dir, kShotRange, hit) && hit.net_id == 1) ++hits_rewound;
                    if (lag.raycast(static_cast<f64>(tick), {}, dir, kShotRange, hit) && hit.net_id == 1)
                        ++hits_present;
                }
            }
        }
    }

    // Content hash of the newest snapshot: every client gets the same entities here (nothing has
    // NetRelevance), so per-client snapshots share it.
    u64 snapshot_hash = 0;
    u64 bytes_sent = 0, bytes_full = 0;  // snapshot bytes written, and what full broadcasts would cost
    void broadcast_snapshot() {
        snapshot.clear();
        snapshot_hash = replication.write_snapshot(world(), static_cast<u32>(tick), snapshot);
        if (!cv_delta.get_bool()) {
            net->broadcast(kChannelSnapshot, snapshot.data(), net::Delivery::Sequenced);
            net->flush();
            return;
        }
        const usize full = snapshot.size();
        auto send_to = [&](net::PeerId peer) {
            snapshot.clear();
            replication.write_snapshot_for(world(), static_cast<u32>(tick), peer, snapshot);
            net->send(peer, kChannelSnapshot, snapshot.data(), net::Delivery::Sequenced);
            bytes_sent += snapshot.size();
            bytes_full += full;
        };
        world().query<const Player>().each([&](const Player& p) {
            if (p.peer != net::kInvalidPeer) send_to(p.peer);  // not the host's own player
        });
        for (net::PeerId peer : watchers) send_to(peer);
        net->flush();
    }

    // Network in, simulate one tick, snapshot out.
    void step() {
        receive();
        if ((tick + 1) % kGustTicks == 0) {
            // Something the clients cannot predict: their reconcile smooths it in.
            world().query<const Player, LocalTransform>().each(
                [](const Player& p, LocalTransform& t) { t.position.x -= p.move_x > 0.0f ? kGust : -kGust; });
        }
        // A host that took over plays its own player here, with no network in between.
        world().query<const Player, LocalTransform>().each([](const Player& p, LocalTransform& t) {
            if (p.peer != net::kInvalidPeer) return;
            PlayerState st{t};
            step_player(nullptr, st, PlayerInput{p.move_x}, static_cast<f32>(kDt));
            t.position = st.transform.position;
        });
        world().tick(kDt, ++tick);
        lag.record(world(), static_cast<u32>(tick));
        track();
        broadcast_snapshot();
    }

    Motion motion;  // drifter 1 as the server simulates it
    void track() {
        world().query<const net::NetId, const LocalTransform>().each(
            [&](const net::NetId& id, const LocalTransform& t) {
                if (id.id == 1) motion.add(t.position);
            });
    }
};

// ---- client -----------------------------------------------------------------------------------

struct Client {
    World world;
    net::Transport* net = nullptr;
    net::ReplicationClient replication;
    net::PeerId server = net::kInvalidPeer;
    f32 move = 1.0f;  // the input this client holds
    bool connected = false;
    bool watch = false;  // a spectator
    // Host migration: the room's new host is this client (promoted), or another one (rejoin).
    net::RelayClient* room = nullptr;
    bool promoted = false;
    bool adopted = false;  // the server runs in this world now; the client is done
    bool rejoin = false;
    static void on_host_change(void* user, net::PeerId host) {
        Client& c = *static_cast<Client*>(user);
        c.server = host;  // the old host's Disconnected that follows is not ours to act on
        if (c.room && host == c.room->member_id()) c.promoted = true;
        else c.rejoin = true;
    }
    u32 own_id = 0;  // our player's net id, from WELCOME
    net::Predictor<PlayerInput, PlayerState> predictor;
    u64 applied_hash = 0;  // of the newest snapshot applied
    u32 ack = 0;           // from the newest snapshot of our player
    bool has_ack = false;
    u32 snapshots = 0;
    u32 deltas = 0;  // of those, delta-coded
    u64 tick = 0;
    // Received this frame, applied by the net_apply system inside the tick.
    net::ByteWriter pending;
    bool has_pending = false;

    void start(net::Transport& transport, net::PeerId to_server, f32 hold) {
        net = &transport;
        server = to_server;
        move = hold;
        world.set_resource<Client*>(this);
        if (cv_interp.get_bool()) replication.set_interpolation(net::InterpolationDesc{});
        predictor.init(net::PredictionDesc{}, step_player);
        net::ReplicationHooks hooks;
        hooks.user = this;
        hooks.read_extra = [](void* user, Commands&, Entity e, net::ByteReader& in) {
            Client& c = *static_cast<Client*>(user);
            if (c.own_id == 0 || e != c.replication.find(c.own_id)) return;
            const u32 seq = in.read_u32();
            if (in.ok()) {
                c.ack = seq;
                c.has_ack = true;
            }
        };
        replication.set_hooks(hooks);
        world.add_system(Phase::FixedUpdate_Pre, "net_apply", [](Frame& f) {
            Client& c = *f.resource<Client*>();
            if (c.adopted) return;
            if (c.has_pending) {
                const net::ApplyResult r = c.replication.apply(f.commands(), f.world(), c.pending.data());
                if (r.ok) {
                    ++c.snapshots;
                    c.applied_hash = c.replication.content_hash();
                    if (r.delta) ++c.deltas;
                }
                c.has_pending = false;
            }
            c.predict(f);
            c.replication.update(f.commands(), f.world(), f.dt);  // interpolating: draws the render time
        });
    }

    // Our player: start predicting when it first appears, reconcile on each snapshot of it, draw
    // the prediction (snapshots no longer move it, set_predicted).
    void predict(Frame& f) {
        const LocalTransform* server_t = own_id ? replication.latest_transform(own_id) : nullptr;
        if (!server_t) return;
        if (!predictor.active()) {
            predictor.reset(PlayerState{*server_t});
        } else if (has_ack) {
            predictor.reconcile(ack, PlayerState{*server_t});
        }
        has_ack = false;
        predictor.smooth(f.dt);
        if (cv_predict.get_bool()) f.commands().set(replication.find(own_id), predictor.displayed());
    }

    // Fires from the origin at drifter 1 where this client draws it, with the tick it drew.
    u32 shots = 0;
    void fire() {
        const Entity e = replication.find(1);
        if (!connected || !world.alive(e)) return;
        const math::vec3 dir = math::normalize(world.try_get<LocalTransform>(e)->position);
        const f64 view = replication.drawn_tick(1);
        u64 view_bits = 0;
        std::memcpy(&view_bits, &view, sizeof view_bits);
        net::ByteWriter shot;
        shot.write_u8(kMsgShot);
        shot.write_u64(view_bits);
        shot.write_f32(dir.x);
        shot.write_f32(dir.y);
        shot.write_f32(dir.z);
        net->send(server, kChannelControl, shot.data(), net::Delivery::Reliable);
        ++shots;
    }

    void send_inputs() {
        if (cv_delta.get_bool() && replication.last_tick() != 0) {
            net::ByteWriter msg;
            msg.write_u8(kMsgAck);
            replication.write_ack(msg);
            net->send(server, kChannelInput, msg.data(), net::Delivery::Sequenced);
        }
        net::ByteWriter packet;
        packet.write_u8(kMsgInput);
        if (predictor.write_inputs(packet)) net->send(server, kChannelInput, packet.data(), net::Delivery::Sequenced);
    }

    void receive() {
        for (net::Event ev; net->poll(ev);) {
            if (ev.peer != server) continue;  // through a relay the other clients are peers too
            if (ev.kind == net::EventKind::Connected) {
                connected = true;
                const u8 join[1] = {watch ? kMsgWatch : kMsgJoin};
                net->send(server, kChannelControl, Span<const u8>(join, 1), net::Delivery::Reliable);
            } else if (ev.kind == net::EventKind::Disconnected) {
                connected = false;
                own_id = 0;
                predictor.stop();
                replication.reset(world.commands(), world);
            } else if (ev.kind == net::EventKind::Received && ev.channel == kChannelControl) {
                net::ByteReader r(ev.data);
                if (r.read_u8() == kMsgWelcome) {
                    const u32 id = r.read_u32();
                    if (r.ok()) {
                        own_id = id;
                        if (cv_predict.get_bool()) replication.set_predicted(id);
                    }
                }
            } else if (ev.kind == net::EventKind::Received && ev.channel == kChannelSnapshot) {
                pending.clear();  // only the newest snapshot of the frame matters
                pending.append(ev.data);
                has_pending = true;
            }
        }
    }

    // One tick holding `move` (input: false keeps resending the unacknowledged inputs only).
    void step(bool input = true) {
        if (adopted) return;
        receive();
        if (rejoin && connected && own_id) {
            // The new host: ask for our player back.
            net::ByteWriter join;
            join.write_u8(kMsgJoin);
            join.write_u32(own_id);
            net->send(server, kChannelControl, join.data(), net::Delivery::Reliable);
            rejoin = false;
        }
        if (connected && (predictor.active() || watch)) {
            if (input && !watch) predictor.push(PlayerInput{move});
            send_inputs();  // a spectator: only the acknowledgement
        }
        world.tick(kDt, ++tick);
        if (input && !watch && tick > 120 && tick % kShotTicks == 0) fire();
        track();
        net->flush();
    }

    // How smooth drifter 1 looks here: the mean size of its per-tick change in velocity.
    Motion motion;
    // How far behind its inputs our player is drawn, in seconds (0 when predicting).
    f64 input_lag_s = 0.0;
    u32 lag_samples = 0;
    void track() {
        const Entity e = replication.find(1);
        const LocalTransform* t = world.alive(e) ? world.try_get<LocalTransform>(e) : nullptr;
        if (t) motion.add(t->position);
        const Entity own = own_id ? replication.find(own_id) : Entity::null;
        const LocalTransform* ot = world.alive(own) ? world.try_get<LocalTransform>(own) : nullptr;
        if (ot && predictor.active() && tick > 120) {
            input_lag_s += std::fabs(static_cast<f64>(predictor.state().transform.position.x - ot->position.x) /
                                     static_cast<f64>(move));
            ++lag_samples;
        }
    }
    f64 mean_input_lag_ms() const { return lag_samples ? input_lag_s / lag_samples * 1000.0 : 0.0; }
};

void Server::adopt(Client& heir) {
    net = heir.net;
    active_world = &heir.world;
    heir.adopted = true;
    tick = replication.adopt(world(), heir.replication);
    replication_start = tick;
    lag.init(net::LagCompensationDesc{60.0, 1.0});
    add_drift();
    set_hooks();
    // What the snapshots did not carry: the drifters' orbits and hit boxes, and our own player.
    Array<Entity> drifters;
    Entity own = Entity::null;
    world().query<const net::NetId>().each([&](Entity e, const net::NetId& id) {
        if (id.kind == kKindDrifter) drifters.push_back(e);
        if (id.id == heir.own_id) own = e;
    });
    for (Entity e : drifters) {
        const math::vec3 p = world().try_get<LocalTransform>(e)->position;
        world().add(e, Drifter{std::sqrt(p.x * p.x + p.y * p.y), std::atan2(p.y, p.x)});
        world().add(e, net::HitBox{{0.15f, 0.15f, 0.15f}, {}});
    }
    if (!own.is_null()) world().add(own, Player{net::kInvalidPeer, heir.move, {}});
}

// Bit-identical, field by field (NaN-safe, padding-free).
bool same_bits(f32 a, f32 b) {
    u32 x = 0, y = 0;
    std::memcpy(&x, &a, sizeof x);
    std::memcpy(&y, &b, sizeof y);
    return x == y;
}
bool same_bits(const LocalTransform& a, const LocalTransform& b) {
    const f32 fa[10] = {a.position.x, a.position.y, a.position.z, a.rotation.x, a.rotation.y,
                        a.rotation.z, a.rotation.w, a.scale.x,    a.scale.y,    a.scale.z};
    const f32 fb[10] = {b.position.x, b.position.y, b.position.z, b.rotation.x, b.rotation.y,
                        b.rotation.z, b.rotation.w, b.scale.x,    b.scale.y,    b.scale.z};
    for (int i = 0; i < 10; ++i)
        if (!same_bits(fa[i], fb[i])) return false;
    return true;
}

// The client has the server's last snapshot (the same bytes: a late input can change the
// server's state after its last tick), every input acknowledged and, interpolating or
// predicting, draws it exactly.
bool caught_up(const Client& c, u32 final_tick, u64 snapshot_hash) {
    if (c.replication.last_tick() != final_tick || c.applied_hash != snapshot_hash) return false;
    if (c.predictor.stats().pending != 0 || c.predictor.stats().offset != 0.0f) return false;
    if (!c.replication.interpolating()) return true;
    const net::InterpolationStats& s = c.replication.interpolation_stats();
    return s.interpolated == 0 && s.extrapolated == 0 && s.held + s.predicted == c.replication.size();
}

// Every client mirrors the server bit for bit, and each player moved the way its client held.
// `heir`: the client that became the host (not checked as a client), or count when none did.
bool check(Server& server, Client* clients, u32 count, u32 spectators = 0, u32 heir = ~0u) {
    u32 replicated = 0;
    server.world().query<const net::NetId>().each([&](const net::NetId&) { ++replicated; });
    bool ok = replicated == 8 + count;
    for (u32 c = count; c < count + spectators; ++c) {
        Client& watcher = clients[c];
        u32 matched = 0;
        server.world().query<const net::NetId, const LocalTransform>().each(
            [&](const net::NetId& id, const LocalTransform& t) {
                const Entity mirror = watcher.replication.find(id.id);
                const LocalTransform* mt =
                    watcher.world.alive(mirror) ? watcher.world.try_get<LocalTransform>(mirror) : nullptr;
                if (mt && same_bits(*mt, t)) ++matched;
            });
        const bool watcher_ok = watcher.connected && watcher.own_id == 0 && matched == replicated &&
                                watcher.world.entity_count() == replicated;
        std::printf("  spectator %u: %u snapshots applied (%u deltas), %u/%u entities match -> %s\n", c - count,
                    watcher.snapshots, watcher.deltas, matched, replicated, watcher_ok ? "OK" : "FAILED");
        ok = ok && watcher_ok;
    }
    for (u32 c = 0; c < count; ++c) {
        if (c == heir) continue;
        Client& client = clients[c];
        u32 matched = 0;
        server.world().query<const net::NetId, const LocalTransform>().each(
            [&](const net::NetId& id, const LocalTransform& t) {
                const Entity mirror = client.replication.find(id.id);
                const LocalTransform* mt =
                    client.world.alive(mirror) ? client.world.try_get<LocalTransform>(mirror) : nullptr;
                if (mt && same_bits(*mt, t)) ++matched;
            });
        // This client's own player: the one whose server-side peer sent its inputs.
        f32 player_x = 0.0f;
        server.world().query<const Player, const LocalTransform>().each([&](const Player& p, const LocalTransform& t) {
            if (std::fabs(p.move_x - client.move) < 1e-6f) player_x = t.position.x;
        });
        const bool moved = client.move > 0.0f ? player_x > 0.5f : player_x < -0.5f;
        const bool client_ok =
            client.connected && matched == replicated && client.world.entity_count() == replicated && moved;
        std::printf("  client %u: %u snapshots applied, %u/%u entities match, player x %.3f -> %s\n", c,
                    client.snapshots, matched, replicated, static_cast<double>(player_x), client_ok ? "OK" : "FAILED");
        ok = ok && client_ok;
    }
    // Every shot at drifter 1 hits when checked against the moment its client drew; against the
    // server's present most miss.
    const bool shots_ok = server.shots > 0 && server.hits_rewound >= server.shots - server.too_old;
    std::printf(
        "  shots at drifter 1: %u (%u older than the rewind), hit when rewound %u, hit at the server's present %u -> "
        "%s\n",
        server.shots, server.too_old, server.hits_rewound, server.hits_present, shots_ok ? "OK" : "FAILED");
    return ok && shots_ok;
}

f32 hold_for(u32 client) {
    static const f32 kHolds[kMaxClients] = {1.0f, -1.0f, 0.5f, -0.5f, 0.75f, -0.75f, 0.25f, -0.25f};
    return kHolds[client % kMaxClients];
}

net::LinkConditions conditions_from_cvars() {
    net::LinkConditions c;
    c.latency_ms = static_cast<u32>(std::max(0, cv_latency.get_int()));
    c.jitter_ms = static_cast<u32>(std::max(0, cv_jitter.get_int()));
    c.loss = std::fmin(1.0f, std::fmax(0.0f, cv_loss.get_float()));
    return c;
}

// ---- over SimNetwork (deterministic) ------------------------------------------------------------

int run_sim() {
    const bool relayed = cv_relay.get_bool();
    // A room holds the host and up to 7 others.
    const i32 most = relayed ? static_cast<i32>(net::kRelayMaxMembers - 1) : static_cast<i32>(kMaxClients);
    const u32 count = static_cast<u32>(std::min(std::max(cv_clients.get_int(), 1), most));
    // Spectators take the client slots the players leave (direct only).
    const u32 spectators =
        relayed ? 0u : static_cast<u32>(std::clamp(cv_spectators.get_int(), 0, static_cast<i32>(kMaxClients - count)));
    const u32 total = count + spectators;
    const net::LinkConditions link = conditions_from_cvars();
    net::SimNetwork network(static_cast<u64>(cv_seed.get_int()));
    network.set_default_conditions(link);

    // Direct: the clients connect to the server's endpoint. Relayed: everybody connects to the
    // relay's, the server makes a room and the clients join it once its code is known.
    net::SimEndpoint* relay_end = relayed ? &network.add_endpoint(kMaxClients + 1, 2) : nullptr;
    net::RelayServer relay;
    relay.init();
    if (relay_end) relay.add_transport(*relay_end);
    net::RelayClient rooms[kMaxClients + 1];
    net::RelayClientDesc room_desc;
    room_desc.channels = kChannels;
    room_desc.on_host_change = Client::on_host_change;
    const u64 host_leaves = relayed ? static_cast<u64>(std::max(cv_host_leaves.get_int(), 0)) : 0;

    net::SimEndpoint& server_end = network.add_endpoint(relayed ? 1 : kMaxClients, relayed ? 2 : kChannels);
    Server server;
    if (relayed) {
        rooms[kMaxClients].start(server_end, server_end.connect(*relay_end), room_desc);
        net::RelayRoomOptions options;
        options.host_migration = host_leaves > 0;
        rooms[kMaxClients].create_room(count + 1, options);
        server.start(rooms[kMaxClients]);
    } else {
        server.start(server_end);
    }
    Client clients[kMaxClients];
    for (u32 c = 0; c < total; ++c) {
        clients[c].watch = c >= count;
        net::SimEndpoint& end = network.add_endpoint(1, relayed ? 2 : kChannels);
        if (relayed) {
            net::RelayClientDesc d = room_desc;
            d.user = &clients[c];
            rooms[c].start(end, end.connect(*relay_end), d);
            clients[c].start(rooms[c], 0, hold_for(c));  // the host is member 0
            clients[c].room = &rooms[c];
        } else {
            clients[c].start(end, end.connect(server_end), hold_for(c));
        }
    }
    auto run_relay = [&] {
        if (!relayed) return;
        relay.update();
        const net::RelayClient& host = rooms[kMaxClients];
        if (host.state() != net::RelayClient::State::InRoom) return;
        for (u32 c = 0; c < count; ++c)
            if (rooms[c].state() == net::RelayClient::State::Idle) rooms[c].join_room(host.room_code());
    };

    // Host migration: the host leaves; the client the relay names takes over in its own world.
    Server heir;
    Server* active = &server;
    u32 heir_index = ~0u;
    u64 left_at = 0;
    auto migrate = [&](u64 tick) {
        if (host_leaves && tick == host_leaves && active == &server) {
            rooms[kMaxClients].leave();
            rooms[kMaxClients].flush();
            active = nullptr;  // no server until a client takes over
            left_at = tick;
        }
        if (active) return;
        for (u32 c = 0; c < count; ++c) {
            if (!clients[c].promoted) continue;
            heir.adopt(clients[c]);
            active = &heir;
            heir_index = c;
            return;
        }
    };

    net::NetStatsMeter meter;
    u64 clock_ms = 0;
    // One tick of the whole session: server, clients, then the network moves 1/60 s.
    auto advance = [&](u64 tick) {
        const u64 next_ms = tick * 1000 / 60;
        network.advance(static_cast<u32>(next_ms - clock_ms));
        meter.update(*(active ? active : &server)->net, static_cast<f64>(next_ms - clock_ms) / 1000.0);
        clock_ms = next_ms;
    };
    const u64 ticks = static_cast<u64>(std::max(cv_ticks.get_int(), 1));
    u64 session_tick = 0;
    for (u64 i = 0; i < ticks; ++i) {
        run_relay();
        migrate(i + 1);
        if (active) active->step();
        for (u32 c = 0; c < total; ++c) clients[c].step();
        advance(++session_tick);
    }
    if (!active) {
        std::printf("net_headless: no client took over from the host\n");
        return 1;
    }
    Server& srv = *active;

    // The server stops simulating and repeats its last snapshot until every client has it (a lost
    // one is superseded by the next) and, interpolating, draws it: past the jitter buffer and the
    // extrapolation limit every entity holds the newest snapshot. Up to 10 virtual seconds.
    const u32 final_tick = static_cast<u32>(srv.tick);
    auto all_caught_up = [&] {
        for (u32 c = 0; c < total; ++c)
            if (c != heir_index && !caught_up(clients[c], final_tick, srv.snapshot_hash)) return false;
        return true;
    };
    for (u32 i = 0; i < 600 && !all_caught_up(); ++i) {
        run_relay();
        srv.receive();
        srv.broadcast_snapshot();
        for (u32 c = 0; c < total; ++c) clients[c].step(false);  // resend unacknowledged inputs
        advance(++session_tick);
    }

    if (spectators) std::printf("net_headless: %u spectators watch\n", spectators);
    std::printf("net_headless (sim%s): %u clients, latency %u ms, jitter %u ms, loss %.3f, seed %d, %u ticks\n",
                relayed ? ", through a relay" : "", count, link.latency_ms, link.jitter_ms, static_cast<double>(link.loss),
                cv_seed.get_int(), final_tick);
    const u32 host_room = heir_index != ~0u ? heir_index : kMaxClients;
    if (relayed) {
        const net::RelayStats& rs = relay.stats();
        std::printf("  relay: room %.*s, %u members, %llu messages forwarded (%llu bytes), %llu dropped\n",
                    static_cast<int>(rooms[host_room].room_code().size()), rooms[host_room].room_code().data(),
                    rooms[host_room].connected_count() + 1, static_cast<unsigned long long>(rs.forwarded),
                    static_cast<unsigned long long>(rs.forwarded_bytes), static_cast<unsigned long long>(rs.dropped));
    }
    if (heir_index != ~0u) {
        std::printf("  host migration: the host left at tick %llu, client %u (member %u) took over at tick %u, %u players "
                    "rebound, relay counted %llu migration\n",
                    static_cast<unsigned long long>(left_at), heir_index, rooms[heir_index].member_id(),
                    static_cast<u32>(heir.replication_start), heir.rebound,
                    static_cast<unsigned long long>(relay.stats().host_migrations));
    }
    const bool ok = check(srv, clients, count, spectators, heir_index) && (!host_leaves || heir.rebound == count - 1);
    FixedString<160> stats;
    meter.format(stats);
    std::printf("  server %s\n", stats.c_str());
    const u32 shown = heir_index == 0 ? 1 : 0;  // the first client still a client
    const net::InterpolationStats& is = clients[shown].replication.interpolation_stats();
    std::printf("  drifter 1 jerk: server %.5f, client %u %.5f (%s, buffer %.0f ms, jitter %.1f ms)\n",
                srv.motion.mean(), shown, clients[shown].motion.mean(), cv_interp.get_bool() ? "interpolated" : "raw",
                static_cast<double>(is.delay_s) * 1000.0, static_cast<double>(is.jitter_s) * 1000.0);
    if (cv_delta.get_bool()) {
        u32 deltas = 0, snapshots = 0;
        for (u32 c = 0; c < total; ++c) {
            deltas += clients[c].deltas;
            snapshots += clients[c].snapshots;
        }
        std::printf("  snapshots: %u of %u applied were deltas; %llu bytes sent, %llu as full broadcasts (%.0f%%)\n",
                    deltas, snapshots, static_cast<unsigned long long>(srv.bytes_sent),
                    static_cast<unsigned long long>(srv.bytes_full),
                    srv.bytes_full ? 100.0 * static_cast<double>(srv.bytes_sent) / static_cast<double>(srv.bytes_full)
                                      : 0.0);
    }
    const net::PredictionStats& ps = clients[shown].predictor.stats();
    std::printf("  client %u player: drawn %.0f ms behind its input (%s), %u corrections, %u snaps\n", shown,
                clients[shown].mean_input_lag_ms(), cv_predict.get_bool() ? "predicted" : "from snapshots", ps.corrections,
                ps.snaps);

    // The run's fingerprint: every world, the motion each client drew, and what the network did to
    // the traffic.
    Hasher64 h;
    h.u64v(srv.world().state_hash());
    for (u32 c = 0; c < total; ++c) {
        h.u64v(clients[c].world.state_hash());
        u64 jerk_bits = 0;  // the path the client drew, not just where it ended
        std::memcpy(&jerk_bits, &clients[c].motion.jerk, sizeof jerk_bits);
        h.u64v(jerk_bits);
        u64 lag_bits = 0;
        std::memcpy(&lag_bits, &clients[c].input_lag_s, sizeof lag_bits);
        h.u64v(lag_bits);
        h.u64v(clients[c].predictor.stats().corrections);
        h.u64v(clients[c].net->counters().messages_received);
        h.u64v(clients[c].net->counters().messages_dropped);
    }
    h.u64v(srv.hits_rewound);
    h.u64v(srv.hits_present);
    h.u64v(srv.too_old);
    h.u64v(server_end.counters().messages_received);
    h.u64v(server_end.counters().messages_dropped);
    if (relayed) h.u64v(relay.stats().forwarded);
    std::printf("net_headless: %s, hash %016llx\n", ok ? "OK" : "FAILED", static_cast<unsigned long long>(h.value()));
    return ok ? 0 : 1;
}

// ---- over loopback UDP ----------------------------------------------------------------------------

int run_udp() {
    net::HostDesc sd;
    sd.bind_address = "127.0.0.1";
    sd.channels = kChannels;
    sd.max_peers = 4;
    net::Host server_host;
    if (!server_host.listen(sd)) return 2;
    net::HostDesc cd = net::Host::client_desc();
    cd.channels = kChannels;
    net::Host client_host;
    if (!client_host.open_client(cd)) return 2;
    const net::PeerId to_server = client_host.connect("127.0.0.1", server_host.bound_port());
    if (to_server == net::kInvalidPeer) return 2;
    const net::LinkConditions link = conditions_from_cvars();
    if (link.any()) {
        server_host.set_conditions(link, static_cast<u64>(cv_seed.get_int()));
        client_host.set_conditions(link, static_cast<u64>(cv_seed.get_int()) + 1);
    }

    Server server;
    server.start(server_host);
    Client client;
    client.start(client_host, to_server, 1.0f);

    // With simulated latency the loop runs at 60 Hz of wall time, as a game would; without, as fast
    // as it can.
    const u64 ticks = static_cast<u64>(cv_ticks.get_int());
    const auto t0 = std::chrono::steady_clock::now();
    for (u64 i = 0; i < ticks; ++i) {
        server.step();
        client.step();
        if (link.any()) std::this_thread::sleep_until(t0 + std::chrono::microseconds((i + 1) * 1000000 / 60));
    }

    // Let the last snapshot land (loopback: a few ms, plus any simulated latency), then compare.
    const u32 final_tick = static_cast<u32>(server.tick);
    const auto deadline = std::chrono::steady_clock::now() + std::chrono::seconds(3 + link.latency_ms / 250);
    while (!caught_up(client, final_tick, server.snapshot_hash) && std::chrono::steady_clock::now() < deadline) {
        server.receive();
        server.broadcast_snapshot();
        for (net::Event ev; server_host.poll(ev, 1);) {
        }
        client.step(false);
    }

    std::printf("net_headless (udp): %u ticks, server sent %llu bytes\n", final_tick,
                static_cast<unsigned long long>(server_host.bytes_sent()));
    const bool ok = check(server, &client, 1);
    std::printf("net_headless: %s\n", ok ? "OK" : "FAILED");

    // Graceful goodbye: the server sees the disconnect and removes the player.
    client_host.disconnect(to_server);
    const auto bye = std::chrono::steady_clock::now() + std::chrono::seconds(2);
    while (server_host.connected_count() > 0 && std::chrono::steady_clock::now() < bye) {
        for (net::Event ev; client_host.poll(ev, 1);) {
        }
        server.step();
    }
    return ok ? 0 : 1;
}

}  // namespace

int main(int argc, char** argv) {
    log::init({.app_name = "net_headless", .level = log::Level::Info});
    console::parse_command_line(argc, argv);
    const int rc = cv_sim.get_bool() ? run_sim() : run_udp();
    log::shutdown();
    return rc;
}
