// A server and its clients in one process (mce_net, ADR-0035): the smallest complete multiplayer
// loop the module supports, and its CI check.
//
//   server world: eight drifters circle the origin; a player entity appears for each client that
//                 sends JOIN and moves by that client's inputs, and every 2 s a gust only the server
//                 knows about pushes every player back. Every tick it broadcasts a snapshot.
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
//
// After -ticks N it lets the last snapshot land and checks that every client's mirror equals the
// server's replicated state bit for bit and that each player moved. Exit code 0 on success.
//
//   sample_net_headless -ticks 300
//   sample_net_headless -sim 1 -clients 4 -latency 75 -jitter 20 -loss 0.05 -ticks 600

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

constexpr f64 kDt = 1.0 / 60.0;
constexpr u8 kChannels = 3;
constexpr u8 kChannelControl = 0;   // reliable: JOIN
constexpr u8 kChannelSnapshot = 1;  // sequenced: server -> client snapshots
constexpr u8 kChannelInput = 2;     // sequenced: client -> server inputs
constexpr u8 kMsgJoin = 1;
constexpr u8 kMsgInput = 2;
constexpr u8 kMsgWelcome = 3;  // server -> client: the net id of your player
constexpr u8 kMsgShot = 4;     // client -> server: a ray from the origin and the tick the client drew
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
    World world;
    net::Transport* net = nullptr;
    net::ReplicationServer replication;
    net::ByteWriter snapshot;
    u64 tick = 0;

    // Hit boxes of the last second, so a shot is checked against what its client was drawing.
    // The rewind covers the slowest link CI runs (150 ms ± 40 plus the jitter buffer).
    net::LagCompensator lag;
    u32 shots = 0, hits_rewound = 0, hits_present = 0;
    u32 too_old = 0;  // arrived after the rewind window (a reliable shot lost and resent): checked late

    void start(net::Transport& transport) {
        net = &transport;
        lag.init(net::LagCompensationDesc{60.0, 1.0});
        for (u32 i = 0; i < 8; ++i) {
            const Entity e = world.spawn_with(LocalTransform{}, Drifter{1.0f + f32(i), f32(i) * 0.7f},
                                              net::HitBox{{0.15f, 0.15f, 0.15f}, {}});
            replication.assign(world, e, kKindDrifter);
        }
        world.add_system(Phase::FixedUpdate_Post, "drift", [](Frame& f) {
            f.query<Drifter, LocalTransform>().each([&](Drifter& dr, LocalTransform& t) {
                dr.phase += f.dt;
                t.position = {dr.radius * std::cos(dr.phase), dr.radius * std::sin(dr.phase), 0.0f};
            });
        });
        net::ReplicationHooks hooks;
        hooks.write_extra = [](void*, const World& w, Entity e, net::ByteWriter& out) {
            if (const Player* p = w.try_get<Player>(e)) out.write_u32(p->inputs.last_seq());  // the ack
        };
        replication.set_hooks(hooks);
    }

    Entity player_of(net::PeerId peer) {
        Entity found = Entity::null;
        world.query<const Player>().each([&](Entity e, const Player& p) {
            if (p.peer == peer) found = e;
        });
        return found;
    }

    void receive() {
        for (net::Event ev; net->poll(ev);) {
            if (ev.kind == net::EventKind::Disconnected) {
                if (const Entity e = player_of(ev.peer); !e.is_null()) world.destroy(e);
            } else if (ev.kind == net::EventKind::Received) {
                net::ByteReader r(ev.data);
                const u8 msg = r.read_u8();
                if (msg == kMsgJoin && r.ok() && player_of(ev.peer).is_null()) {
                    const Entity e = world.spawn_with(LocalTransform{}, Player{ev.peer, 0.0f, {}});
                    net::ByteWriter welcome;
                    welcome.write_u8(kMsgWelcome);
                    welcome.write_u32(replication.assign(world, e, kKindPlayer));
                    net->send(ev.peer, kChannelControl, welcome.data(), net::Delivery::Reliable);
                } else if (msg == kMsgInput && r.ok()) {
                    const Entity e = player_of(ev.peer);
                    if (e.is_null()) continue;
                    Player& p = *world.try_get_mut<Player>(e);
                    PlayerState s{*world.try_get<LocalTransform>(e)};
                    p.inputs.receive(ev.data.subspan(1), [&](const PlayerInput& in) {
                        step_player(nullptr, s, in, static_cast<f32>(kDt));
                        if (std::isfinite(in.move_x)) p.move_x = std::fmax(-1.0f, std::fmin(1.0f, in.move_x));
                    });
                    world.try_get_mut<LocalTransform>(e)->position = s.transform.position;
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

    u64 snapshot_hash = 0;  // of the newest snapshot sent
    void broadcast_snapshot() {
        snapshot.clear();
        replication.write_snapshot(world, static_cast<u32>(tick), snapshot);
        Hasher64 h;
        h.bytes(snapshot.data().data(), snapshot.size());
        snapshot_hash = h.value();
        net->broadcast(kChannelSnapshot, snapshot.data(), net::Delivery::Sequenced);
        net->flush();
    }

    // Network in, simulate one tick, snapshot out.
    void step() {
        receive();
        if ((tick + 1) % kGustTicks == 0) {
            // Something the clients cannot predict: their reconcile smooths it in.
            world.query<const Player, LocalTransform>().each(
                [](const Player& p, LocalTransform& t) { t.position.x -= p.move_x > 0.0f ? kGust : -kGust; });
        }
        world.tick(kDt, ++tick);
        lag.record(world, static_cast<u32>(tick));
        track();
        broadcast_snapshot();
    }

    Motion motion;  // drifter 1 as the server simulates it
    void track() {
        world.query<const net::NetId, const LocalTransform>().each([&](const net::NetId& id, const LocalTransform& t) {
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
    u32 own_id = 0;  // our player's net id, from WELCOME
    net::Predictor<PlayerInput, PlayerState> predictor;
    u64 applied_hash = 0;  // of the newest snapshot applied
    u32 ack = 0;           // from the newest snapshot of our player
    bool has_ack = false;
    u32 snapshots = 0;
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
            if (c.has_pending) {
                if (c.replication.apply(f.commands(), f.world(), c.pending.data()).ok) {
                    ++c.snapshots;
                    Hasher64 h;
                    h.bytes(c.pending.data().data(), c.pending.size());
                    c.applied_hash = h.value();
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
        net::ByteWriter packet;
        packet.write_u8(kMsgInput);
        if (predictor.write_inputs(packet)) net->send(server, kChannelInput, packet.data(), net::Delivery::Sequenced);
    }

    void receive() {
        for (net::Event ev; net->poll(ev);) {
            if (ev.kind == net::EventKind::Connected) {
                connected = true;
                const u8 join[1] = {kMsgJoin};
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
        receive();
        if (connected && predictor.active()) {
            if (input) predictor.push(PlayerInput{move});
            send_inputs();
        }
        world.tick(kDt, ++tick);
        if (input && tick > 120 && tick % kShotTicks == 0) fire();
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
bool check(Server& server, Client* clients, u32 count) {
    u32 replicated = 0;
    server.world.query<const net::NetId>().each([&](const net::NetId&) { ++replicated; });
    bool ok = replicated == 8 + count;
    for (u32 c = 0; c < count; ++c) {
        Client& client = clients[c];
        u32 matched = 0;
        server.world.query<const net::NetId, const LocalTransform>().each(
            [&](const net::NetId& id, const LocalTransform& t) {
                const Entity mirror = client.replication.find(id.id);
                const LocalTransform* mt =
                    client.world.alive(mirror) ? client.world.try_get<LocalTransform>(mirror) : nullptr;
                if (mt && same_bits(*mt, t)) ++matched;
            });
        // This client's own player: the one whose server-side peer sent its inputs.
        f32 player_x = 0.0f;
        server.world.query<const Player, const LocalTransform>().each([&](const Player& p, const LocalTransform& t) {
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
    const u32 count = static_cast<u32>(std::min(std::max(cv_clients.get_int(), 1), static_cast<i32>(kMaxClients)));
    const net::LinkConditions link = conditions_from_cvars();
    net::SimNetwork network(static_cast<u64>(cv_seed.get_int()));
    network.set_default_conditions(link);
    net::SimEndpoint& server_end = network.add_endpoint(kMaxClients, kChannels);

    Server server;
    server.start(server_end);
    Client clients[kMaxClients];
    for (u32 c = 0; c < count; ++c) {
        net::SimEndpoint& end = network.add_endpoint(1, kChannels);
        clients[c].start(end, end.connect(server_end), hold_for(c));
    }

    net::NetStatsMeter meter;
    u64 clock_ms = 0;
    // One tick of the whole session: server, clients, then the network moves 1/60 s.
    auto advance = [&](u64 tick) {
        const u64 next_ms = tick * 1000 / 60;
        network.advance(static_cast<u32>(next_ms - clock_ms));
        meter.update(server_end, static_cast<f64>(next_ms - clock_ms) / 1000.0);
        clock_ms = next_ms;
    };
    const u64 ticks = static_cast<u64>(std::max(cv_ticks.get_int(), 1));
    u64 session_tick = 0;
    for (u64 i = 0; i < ticks; ++i) {
        server.step();
        for (u32 c = 0; c < count; ++c) clients[c].step();
        advance(++session_tick);
    }

    // The server stops simulating and repeats its last snapshot until every client has it (a lost
    // one is superseded by the next) and, interpolating, draws it: past the jitter buffer and the
    // extrapolation limit every entity holds the newest snapshot. Up to 10 virtual seconds.
    const u32 final_tick = static_cast<u32>(server.tick);
    auto all_caught_up = [&] {
        for (u32 c = 0; c < count; ++c)
            if (!caught_up(clients[c], final_tick, server.snapshot_hash)) return false;
        return true;
    };
    for (u32 i = 0; i < 600 && !all_caught_up(); ++i) {
        server.receive();
        server.broadcast_snapshot();
        for (u32 c = 0; c < count; ++c) clients[c].step(false);  // resend unacknowledged inputs
        advance(++session_tick);
    }

    std::printf("net_headless (sim): %u clients, latency %u ms, jitter %u ms, loss %.3f, seed %d, %u ticks\n", count,
                link.latency_ms, link.jitter_ms, static_cast<double>(link.loss), cv_seed.get_int(), final_tick);
    const bool ok = check(server, clients, count);
    FixedString<160> stats;
    meter.format(stats);
    std::printf("  server %s\n", stats.c_str());
    const net::InterpolationStats& is = clients[0].replication.interpolation_stats();
    std::printf("  drifter 1 jerk: server %.5f, client 0 %.5f (%s, buffer %.0f ms, jitter %.1f ms)\n",
                server.motion.mean(), clients[0].motion.mean(), cv_interp.get_bool() ? "interpolated" : "raw",
                static_cast<double>(is.delay_s) * 1000.0, static_cast<double>(is.jitter_s) * 1000.0);
    const net::PredictionStats& ps = clients[0].predictor.stats();
    std::printf("  client 0 player: drawn %.0f ms behind its input (%s), %u corrections, %u snaps\n",
                clients[0].mean_input_lag_ms(), cv_predict.get_bool() ? "predicted" : "from snapshots", ps.corrections,
                ps.snaps);

    // The run's fingerprint: every world, the motion each client drew, and what the network did to
    // the traffic.
    Hasher64 h;
    h.u64v(server.world.state_hash());
    for (u32 c = 0; c < count; ++c) {
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
    h.u64v(server.hits_rewound);
    h.u64v(server.hits_present);
    h.u64v(server.too_old);
    h.u64v(server_end.counters().messages_received);
    h.u64v(server_end.counters().messages_dropped);
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
