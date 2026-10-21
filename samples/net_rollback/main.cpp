// Rollback netcode in one process (M-NET2 step 5): N peers, each running the whole game in its own
// World, exchange only their inputs over net::SimNetwork. net::RollbackSession predicts the inputs
// that have not arrived, rolls back and simulates again when a guess was wrong, and compares state
// hashes of confirmed frames to catch a desync.
//
//   the game: every player steers a body around a walled pitch and kicks a shared ball when close
//             to it, so each frame depends on everyone's input. Inputs come from a seeded script
//             that changes direction every 10–40 frames, so remote guesses are often wrong.
//   the check: after -frames N, every peer must have confirmed and hashed a recent frame with the
//             same hash, and none may have seen a desync. -desync_at F makes peer 1 compute
//             something else at frame F; then every peer must report a desync at exactly F.
//
//   sample_net_rollback                                     4 peers, 100 ms ± 20, 5% loss
//   sample_net_rollback -peers 2 -prediction 0              lockstep
//   sample_net_rollback -desync_at 300                      catch an injected desync
//   sample_net_rollback -drift 30 -sync 0                   peer 0's clock runs fast, no clock sync
//
// Spectators (M-NET3 step 2): -spectators N adds watchers fed by the players in turn. They run only
// confirmed frames and must end on the same hash as the players.
//
// Nothing reads the wall clock (SimNetwork's virtual clock moves 1/60 s per frame), so the printed
// hash is the same on every run and build (local_ci.sh's determinism stage).

#include <algorithm>
#include <cmath>
#include <cstdio>
#include <cstring>

#include "mce/core.h"
#include "mce/ecs/transform.h"
#include "mce/hash.h"
#include "mce/net/net.h"

using namespace mce;

namespace {

MCE_CVAR(cv_peers, "peers", 4, "Peers (players), 2..8", CVarFlags::None);
MCE_CVAR(cv_frames, "frames", 900, "Frames to play", CVarFlags::None);
MCE_CVAR(cv_latency, "latency", 100, "One-way latency in ms on every link", CVarFlags::None);
MCE_CVAR(cv_jitter, "jitter", 20, "Extra one-way delay, up to this many ms", CVarFlags::None);
MCE_CVAR(cv_loss, "loss", 0.05f, "Chance a packet is lost (0..1)", CVarFlags::None);
MCE_CVAR(cv_seed, "seed", 1, "Seed of the network rolls and the input scripts", CVarFlags::None);
MCE_CVAR(cv_delay, "delay", 2, "Input delay in frames", CVarFlags::None);
MCE_CVAR(cv_prediction, "prediction", 8, "Frames a peer may run ahead of its inputs (0 = lockstep)", CVarFlags::None);
MCE_CVAR(cv_spectators, "spectators", 1, "Spectators, fed by the players in turn (0..8)", CVarFlags::None);
MCE_CVAR(cv_drift, "drift", 0, "Peer 0 plays an extra frame every this many ticks (0: none)", CVarFlags::None);
MCE_CVAR(cv_sync, "sync", 1, "Clock sync: a peer ahead of the others waits now and then", CVarFlags::None);
MCE_CVAR(cv_desync_at, "desync_at", -1, "Peer 1 computes something else at this frame (desync test)", CVarFlags::None);

constexpr f64 kDt = 1.0 / 60.0;
constexpr f32 kPitch = 8.0f;  // half size of the walled pitch
constexpr u32 kBall = 0xFFu;  // the ball's slot

struct Input {
    i8 dx;
    i8 dy;
    u8 kick;
    u8 pad;
};
static_assert(sizeof(Input) == 4);

// Which body this is: a player index, or kBall. Save order and load targets come from it.
struct Slot {
    u32 index;
};
struct Body {
    math::vec3 velocity;
};

struct Peer;
struct FrameInputs {
    const Input* inputs = nullptr;
    u32 players = 0;
    u32 frame = 0;
    Peer* peer = nullptr;
};

struct Peer {
    u32 index = 0;
    World world;
    Entity bodies[net::kRollbackMaxPlayers + 1];  // players, then the ball
    u32 players = 0;
    net::RollbackSession session;
    net::SimEndpoint* end = nullptr;
    net::PeerId link[net::kRollbackMaxPlayers];  // PeerId of each other player
    net::PeerId watcher[net::kRollbackMaxSpectators];  // a player: the spectators it feeds
    net::PeerId feeder = net::kInvalidPeer;            // a spectator: the player feeding it
    i64 sabotage_frame = -1;

    void start(u32 me, u32 count) {
        index = me;
        players = count;
        for (u32 p = 0; p < count; ++p) {
            LocalTransform t;
            const f32 a = static_cast<f32>(p) * 6.2831853f / static_cast<f32>(count);
            t.position = {std::cos(a) * 5.0f, std::sin(a) * 5.0f, 0.0f};
            bodies[p] = world.spawn_with(t, Slot{p}, Body{});
        }
        bodies[count] = world.spawn_with(LocalTransform{}, Slot{kBall}, Body{});
        world.set_resource<FrameInputs>(FrameInputs{});
        world.add_system(Phase::FixedUpdate_Post, "play", [](Frame& f) {
            const FrameInputs& in = f.resource<FrameInputs>();
            const f32 dt = static_cast<f32>(f.dt);
            Entity ball = Entity::null;
            f.query<const Slot>().each([&](Entity e, const Slot& s) {
                if (s.index == kBall) ball = e;
            });
            LocalTransform* bt = f.world().try_get_mut<LocalTransform>(ball);
            Body* bb = f.world().try_get_mut<Body>(ball);
            f.query<const Slot, LocalTransform, Body>().each([&](const Slot& s, LocalTransform& t, Body& b) {
                if (s.index == kBall || s.index >= in.players) return;
                const Input& i = in.inputs[s.index];
                b.velocity = {static_cast<f32>(i.dx) * 3.0f, static_cast<f32>(i.dy) * 3.0f, 0.0f};
                t.position = t.position + b.velocity * dt;
                t.position.x = std::fmin(kPitch, std::fmax(-kPitch, t.position.x));
                t.position.y = std::fmin(kPitch, std::fmax(-kPitch, t.position.y));
                const math::vec3 to_ball = bt->position - t.position;
                if (i.kick && math::length(to_ball) < 1.0f)
                    bb->velocity = bb->velocity + math::normalize(to_ball) * 6.0f;
            });
            // The ball rolls, slows and bounces off the walls.
            bt->position = bt->position + bb->velocity * dt;
            bb->velocity = bb->velocity * 0.99f;
            if (std::fabs(bt->position.x) > kPitch) {
                bt->position.x = std::copysign(kPitch, bt->position.x);
                bb->velocity.x = -bb->velocity.x;
            }
            if (std::fabs(bt->position.y) > kPitch) {
                bt->position.y = std::copysign(kPitch, bt->position.y);
                bb->velocity.y = -bb->velocity.y;
            }
            if (in.peer && static_cast<i64>(in.frame) == in.peer->sabotage_frame) bt->position.x += 0.001f;
        });
    }

    // RollbackSession callbacks: the state is every body's transform and velocity, in slot order.
    static void save(void* user, net::ByteWriter& out) {
        Peer& p = *static_cast<Peer*>(user);
        for (u32 i = 0; i <= p.players; ++i) {
            const LocalTransform& t = *p.world.try_get<LocalTransform>(p.bodies[i]);
            const Body& b = *p.world.try_get<Body>(p.bodies[i]);
            const f32 v[13] = {t.position.x, t.position.y, t.position.z, t.rotation.x, t.rotation.y,
                               t.rotation.z, t.rotation.w, t.scale.x,    t.scale.y,    t.scale.z,
                               b.velocity.x, b.velocity.y, b.velocity.z};
            for (f32 x : v) out.write_f32(x);
        }
    }
    static void load(void* user, Span<const u8> bytes) {
        Peer& p = *static_cast<Peer*>(user);
        net::ByteReader r(bytes);
        for (u32 i = 0; i <= p.players; ++i) {
            LocalTransform& t = *p.world.try_get_mut<LocalTransform>(p.bodies[i]);
            Body& b = *p.world.try_get_mut<Body>(p.bodies[i]);
            f32 v[13];
            for (f32& x : v) x = r.read_f32();
            t.position = {v[0], v[1], v[2]};
            t.rotation = {v[3], v[4], v[5], v[6]};
            t.scale = {v[7], v[8], v[9]};
            b.velocity = {v[10], v[11], v[12]};
        }
    }
    static void advance(void* user, u32 frame, const u8* inputs, u32 input_size, u32 count) {
        Peer& p = *static_cast<Peer*>(user);
        (void)input_size;
        Input in[net::kRollbackMaxPlayers];
        std::memcpy(in, inputs, sizeof(Input) * count);
        p.world.resource<FrameInputs>() = FrameInputs{in, count, frame, &p};
        p.world.tick(kDt, frame + 1);
    }
};

// Player p's input at frame f: a direction held for 10–40 frames, kicking now and then.
Input script(u32 seed, u32 p, u32 f) {
    u32 start = 0;
    u64 h = 0;
    for (u32 segment = 0;; ++segment) {
        Hasher64 hs;
        hs.u32v(seed);
        hs.u32v(p);
        hs.u32v(segment);
        h = hs.value();
        const u32 len = 10 + static_cast<u32>(h % 31);
        if (f < start + len) break;
        start += len;
    }
    Input in{};
    in.dx = static_cast<i8>(static_cast<i32>((h >> 8) % 3) - 1);
    in.dy = static_cast<i8>(static_cast<i32>((h >> 16) % 3) - 1);
    in.kick = (f % 20) < 3 ? 1 : 0;
    return in;
}

}  // namespace

int main(int argc, char** argv) {
    log::init({.app_name = "net_rollback", .level = log::Level::Warn});
    console::parse_command_line(argc, argv);
    const u32 count = static_cast<u32>(std::clamp(cv_peers.get_int(), 2, static_cast<i32>(net::kRollbackMaxPlayers)));
    const u32 frames = static_cast<u32>(std::max(cv_frames.get_int(), 60));
    const u32 seed = static_cast<u32>(cv_seed.get_int());
    const u32 watchers =
        static_cast<u32>(std::clamp(cv_spectators.get_int(), 0, static_cast<i32>(net::kRollbackMaxSpectators)));
    const u32 drift = static_cast<u32>(std::max(0, cv_drift.get_int()));

    net::SimNetwork network(seed);
    net::LinkConditions link;
    link.latency_ms = static_cast<u32>(std::max(0, cv_latency.get_int()));
    link.jitter_ms = static_cast<u32>(std::max(0, cv_jitter.get_int()));
    link.loss = std::fmin(1.0f, std::fmax(0.0f, cv_loss.get_float()));
    network.set_default_conditions(link);

    // The players, then the spectators.
    static Peer peers[net::kRollbackMaxPlayers + net::kRollbackMaxSpectators];
    const u32 total = count + watchers;
    for (u32 p = 0; p < total; ++p) {
        Peer& peer = peers[p];
        const bool spectator = p >= count;
        peer.start(p, count);
        if (p == 1) peer.sabotage_frame = cv_desync_at.get_int();
        net::RollbackDesc d;
        d.players = count;
        d.local_player = spectator ? p - count : p;
        d.spectator = spectator;
        d.time_sync = cv_sync.get_int() != 0;
        d.input_size = sizeof(Input);
        d.input_delay = static_cast<u32>(std::clamp(cv_delay.get_int(), 0, 30));
        d.max_prediction = static_cast<u32>(std::clamp(cv_prediction.get_int(), 0, 30));
        if (!peer.session.init(d, {&peer, Peer::save, Peer::load, Peer::advance})) return 2;
        peer.end = &network.add_endpoint(total, 1);
        for (net::PeerId& l : peer.link) l = net::kInvalidPeer;
        for (net::PeerId& l : peer.watcher) l = net::kInvalidPeer;
    }
    // A full mesh: the lower index connects, carrying its index so the other side knows who it is.
    for (u32 p = 0; p < count; ++p)
        for (u32 q = p + 1; q < count; ++q) peers[p].link[q] = peers[p].end->connect(*peers[q].end, p);
    // Spectator s connects to player s % count, saying which spectator it is.
    constexpr u64 kWatch = 100;
    for (u32 s = 0; s < watchers; ++s)
        peers[count + s].feeder = peers[count + s].end->connect(*peers[s % count].end, kWatch + s);

    u64 clock_ms = 0;
    auto tick = [&](u64 t, bool play) {
        for (u32 p = 0; p < total; ++p) {
            Peer& peer = peers[p];
            for (net::Event ev; peer.end->poll(ev);) {
                if (ev.kind == net::EventKind::Connected && ev.user_data < count && ev.user_data != p &&
                    peer.link[ev.user_data] == net::kInvalidPeer) {
                    peer.link[ev.user_data] = ev.peer;
                } else if (ev.kind == net::EventKind::Connected && ev.user_data >= kWatch &&
                           ev.user_data < kWatch + net::kRollbackMaxSpectators) {
                    peer.watcher[ev.user_data - kWatch] = ev.peer;
                } else if (ev.kind == net::EventKind::Received) {
                    peer.session.receive(ev.data);
                }
            }
            if (p >= count) {  // a spectator: run what is confirmed, tell the feeder what it has
                peer.session.advance();
                net::ByteWriter w;
                if (peer.end->is_connected(peer.feeder) && peer.session.write_packet(p % count, w))
                    peer.end->send(peer.feeder, 0, w.data(), net::Delivery::Unreliable);
                continue;
            }
            // -drift: peer 0's clock runs fast, so now and then it plays two frames in one tick.
            const u32 runs = play ? (p == 0 && drift && t % drift == 0 ? 2u : 1u) : 0u;
            for (u32 r = 0; r < runs; ++r) {
                const Input in = script(seed, p, peer.session.frame() + peer.session.desc().input_delay);
                peer.session.add_local_input(Span<const u8>(reinterpret_cast<const u8*>(&in), sizeof in));
                peer.session.advance();
            }
            for (u32 s = 0; s < net::kRollbackMaxSpectators; ++s) {
                net::ByteWriter w;
                if (peer.end->is_connected(peer.watcher[s]) && peer.session.write_spectator_packet(s, w))
                    peer.end->send(peer.watcher[s], 0, w.data(), net::Delivery::Unreliable);
            }
            for (u32 q = 0; q < count; ++q) {
                if (q == p || !peer.end->is_connected(peer.link[q])) continue;
                net::ByteWriter w;
                if (peer.session.write_packet(q, w))
                    peer.end->send(peer.link[q], 0, w.data(), net::Delivery::Unreliable);
            }
        }
        const u64 next_ms = t * 1000 / 60;
        network.advance(static_cast<u32>(next_ms - clock_ms));
        clock_ms = next_ms;
    };

    u64 t = 0;
    for (u32 i = 0; i < frames; ++i) tick(++t, true);
    // Keep exchanging (no new frames) until every peer has confirmed and hashed the same recent frame.
    u32 target = frames;
    for (u32 p = 0; p < count; ++p) target = std::min(target, peers[p].session.frame());
    target = target > 2 ? target - 2 : 0;
    auto hashed = [&] {
        for (u32 p = 0; p < total; ++p)
            if (peers[p].session.hashed_frame() < static_cast<i64>(target)) return false;
        return true;
    };
    for (u32 i = 0; i < 600 && !hashed(); ++i) tick(++t, true);

    std::printf(
        "net_rollback: %u peers, %u spectators, latency %u ms, jitter %u ms, loss %.3f, delay %u, prediction %u, "
        "drift %u, sync %s, seed %u\n",
        count, watchers, link.latency_ms, link.jitter_ms, static_cast<double>(link.loss),
        peers[0].session.desc().input_delay, peers[0].session.desc().max_prediction, drift,
        peers[0].session.desc().time_sync ? "on" : "off", seed);
    bool agree = true;
    u64 h0 = 0;
    const bool have0 = peers[0].session.confirmed_hash(target, h0);
    Hasher64 run;
    for (u32 p = 0; p < total; ++p) {
        const net::RollbackSession& s = peers[p].session;
        const net::RollbackStats& st = s.stats();
        u64 h = 0;
        const bool have = s.confirmed_hash(target, h);
        agree = agree && have && have0 && h == h0;
        if (p >= count) {
            std::printf("  spectator %u (fed by peer %u): frame %u, %u waits, %u hashes compared, desync %s\n",
                        p - count, (p - count) % count, s.frame(), st.stalls, st.hashes_compared,
                        s.desynced() ? "at frame" : "none");
        } else {
            std::printf(
                "  peer %u: frame %u, confirmed %lld, %u rollbacks (%u frames again, deepest %u), %u stalls, "
                "%u sync waits, %u hashes compared, desync %s\n",
                p, s.frame(), static_cast<long long>(s.confirmed_frame()), st.rollbacks, st.resimulated, st.max_depth,
                st.stalls, st.sync_waits, st.hashes_compared, s.desynced() ? "at frame" : "none");
        }
        if (s.desynced()) std::printf("    first desync at frame %lld\n", static_cast<long long>(s.desync_frame()));
        run.u64v(h);
        run.u32v(st.rollbacks);
        run.u32v(st.resimulated);
        run.u32v(st.stalls);
        run.u32v(st.sync_waits);
        run.u64v(static_cast<u64>(s.desync_frame()));
    }

    bool ok = false;
    const i64 injected = cv_desync_at.get_int();
    if (injected >= 0) {
        // Every peer must have caught it, at exactly the injected frame.
        ok = true;
        // Spectators compare with the player feeding them, which computed the same: none of them sees it.
        for (u32 p = 0; p < count; ++p) ok = ok && peers[p].session.desync_frame() == injected;
        std::printf("  injected desync at frame %lld: %s\n", static_cast<long long>(injected),
                    ok ? "caught by every peer" : "MISSED");
    } else {
        ok = agree;
        for (u32 p = 0; p < total; ++p) ok = ok && !peers[p].session.desynced();
        std::printf("  frame %u state hash on every peer and spectator: %016llx (%s)\n", target,
                    static_cast<unsigned long long>(h0), agree ? "all agree" : "DIFFER");
    }
    std::printf("net_rollback: %s, hash %016llx\n", ok ? "OK" : "FAILED", static_cast<unsigned long long>(run.value()));
    log::shutdown();
    return ok ? 0 : 1;
}
