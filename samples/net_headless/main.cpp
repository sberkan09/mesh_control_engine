// Server and client in one process over loopback UDP (mce_net, ADR-0035): the smallest complete
// multiplayer loop the module supports, and its CI check.
//
//   server world: eight drifters circle the origin; a player entity appears for each client that
//                 sends JOIN and moves by that client's inputs. Every tick it broadcasts a snapshot.
//   client world: sends JOIN, then one input per tick; mirrors the snapshots with
//                 ReplicationClient from a FixedUpdate_Pre system.
//
// After -ticks N it lets the last snapshot land and checks that the client's mirror equals the
// server's replicated state bit for bit and that the player moved. Exit code 0 on success.
//
//   sample_net_headless -ticks 300

#include <chrono>
#include <cmath>
#include <cstdio>
#include <cstring>

#include "mce/core.h"
#include "mce/ecs/transform.h"
#include "mce/net/net.h"

using namespace mce;

namespace {

MCE_CVAR(cv_ticks, "ticks", 300, "Ticks to simulate", CVarFlags::None);

constexpr f64 kDt = 1.0 / 60.0;
constexpr u8 kChannelControl = 0;   // reliable: JOIN
constexpr u8 kChannelSnapshot = 1;  // sequenced: server -> client snapshots
constexpr u8 kChannelInput = 2;     // sequenced: client -> server inputs
constexpr u8 kMsgJoin = 1;
constexpr u8 kMsgInput = 2;
constexpr u32 kKindDrifter = 1;
constexpr u32 kKindPlayer = 2;

struct Drifter {
    f32 radius;
    f32 phase;
};
struct Player {
    net::PeerId peer;
    f32 move_x;
};

// ---- server -----------------------------------------------------------------------------------

struct Server {
    World world;
    net::Host host;
    net::ReplicationServer replication;
    net::ByteWriter snapshot;
    u64 tick = 0;

    bool start() {
        net::HostDesc d;
        d.bind_address = "127.0.0.1";
        d.channels = 3;
        d.max_peers = 4;
        if (!host.listen(d)) return false;
        for (u32 i = 0; i < 8; ++i) {
            const Entity e = world.spawn_with(LocalTransform{}, Drifter{1.0f + f32(i), f32(i) * 0.7f});
            replication.assign(world, e, kKindDrifter);
        }
        world.add_system(Phase::FixedUpdate_Post, "drift", [](Frame& f) {
            f.query<Drifter, LocalTransform>().each([&](Drifter& dr, LocalTransform& t) {
                dr.phase += f.dt;
                t.position = {dr.radius * std::cos(dr.phase), dr.radius * std::sin(dr.phase), 0.0f};
            });
            f.query<const Player, LocalTransform>().each(
                [&](const Player& p, LocalTransform& t) { t.position.x += p.move_x * f.dt; });
        });
        return true;
    }

    Entity player_of(net::PeerId peer) {
        Entity found = Entity::null;
        world.query<const Player>().each([&](Entity e, const Player& p) {
            if (p.peer == peer) found = e;
        });
        return found;
    }

    // Network in, simulate one tick, snapshot out.
    void step() {
        for (net::Event ev; host.poll(ev);) {
            if (ev.kind == net::EventKind::Disconnected) {
                if (const Entity e = player_of(ev.peer); !e.is_null()) world.destroy(e);
            } else if (ev.kind == net::EventKind::Received) {
                net::ByteReader r(ev.data);
                const u8 msg = r.read_u8();
                if (msg == kMsgJoin && r.ok() && player_of(ev.peer).is_null()) {
                    const Entity e = world.spawn_with(LocalTransform{}, Player{ev.peer, 0.0f});
                    replication.assign(world, e, kKindPlayer);
                } else if (msg == kMsgInput) {
                    const f32 move = r.read_f32();
                    const Entity e = player_of(ev.peer);
                    if (r.ok() && !e.is_null() && std::isfinite(move))
                        world.try_get_mut<Player>(e)->move_x = std::fmax(-1.0f, std::fmin(1.0f, move));
                }
            }
        }
        world.tick(kDt, ++tick);
        snapshot.clear();
        replication.write_snapshot(world, static_cast<u32>(tick), snapshot);
        host.broadcast(kChannelSnapshot, snapshot.data(), net::Delivery::Sequenced);
        host.flush();
    }
};

// ---- client -----------------------------------------------------------------------------------

struct Client {
    World world;
    net::Host host;
    net::ReplicationClient replication;
    net::PeerId server = net::kInvalidPeer;
    bool connected = false;
    u32 snapshots = 0;
    u64 tick = 0;
    // Received this frame, applied by the net_apply system inside the tick.
    net::ByteWriter pending;
    bool has_pending = false;

    bool start(u16 port) {
        if (!host.open_client([] {
                net::HostDesc d = net::Host::client_desc();
                d.channels = 3;
                return d;
            }()))
            return false;
        server = host.connect("127.0.0.1", port);
        world.set_resource<Client*>(this);
        world.add_system(Phase::FixedUpdate_Pre, "net_apply", [](Frame& f) {
            Client& c = *f.resource<Client*>();
            if (!c.has_pending) return;
            if (c.replication.apply(f.commands(), f.world(), c.pending.data()).ok) ++c.snapshots;
            c.has_pending = false;
        });
        return server != net::kInvalidPeer;
    }

    void receive() {
        for (net::Event ev; host.poll(ev);) {
            if (ev.kind == net::EventKind::Connected) {
                connected = true;
                const u8 join[1] = {kMsgJoin};
                host.send(server, kChannelControl, Span<const u8>(join, 1), net::Delivery::Reliable);
            } else if (ev.kind == net::EventKind::Disconnected) {
                connected = false;
                replication.reset(world.commands(), world);
            } else if (ev.kind == net::EventKind::Received && ev.channel == kChannelSnapshot) {
                pending.clear();  // only the newest snapshot of the frame matters
                pending.append(ev.data);
                has_pending = true;
            }
        }
    }

    void step() {
        receive();
        if (connected) {
            net::ByteWriter input;
            input.write_u8(kMsgInput);
            input.write_f32(1.0f);  // hold "right"
            host.send(server, kChannelInput, input.data(), net::Delivery::Sequenced);
        }
        world.tick(kDt, ++tick);
        host.flush();
    }
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

int run() {
    Server server;
    if (!server.start()) return 2;
    Client client;
    if (!client.start(server.host.bound_port())) return 2;

    const u64 ticks = static_cast<u64>(cv_ticks.get_int());
    for (u64 i = 0; i < ticks; ++i) {
        server.step();
        client.step();
    }

    // Let the last snapshot land (loopback: a few ms), then compare.
    const u32 final_tick = static_cast<u32>(server.tick);
    const auto deadline = std::chrono::steady_clock::now() + std::chrono::seconds(3);
    while (client.replication.last_tick() != final_tick && std::chrono::steady_clock::now() < deadline) {
        for (net::Event ev; server.host.poll(ev, 1);) {
        }
        client.step();
    }

    u32 checked = 0, mismatched = 0;
    f32 player_x = 0.0f;
    server.world.query<const net::NetId, const LocalTransform>().each(
        [&](const net::NetId& id, const LocalTransform& t) {
            const Entity mirror = client.replication.find(id.id);
            const LocalTransform* mt = client.world.alive(mirror) ? client.world.try_get<LocalTransform>(mirror) : nullptr;
            ++checked;
            if (!mt || !same_bits(*mt, t)) ++mismatched;
            if (id.kind == kKindPlayer && mt) player_x = mt->position.x;
        });
    const bool player_moved = player_x > 0.5f;
    const bool ok = client.connected && mismatched == 0 && checked == 9 && client.world.entity_count() == checked &&
                    player_moved;
    std::printf("net_headless: %u ticks, %u snapshots applied, %u/%u entities match, player x %.3f, "
                "server sent %llu bytes -> %s\n",
                final_tick, client.snapshots, checked - mismatched, checked, static_cast<double>(player_x),
                static_cast<unsigned long long>(server.host.bytes_sent()), ok ? "OK" : "FAILED");

    // Graceful goodbye: the server sees the disconnect and removes the player.
    client.host.disconnect(client.server);
    const auto bye = std::chrono::steady_clock::now() + std::chrono::seconds(2);
    while (server.host.connected_count() > 0 && std::chrono::steady_clock::now() < bye) {
        for (net::Event ev; client.host.poll(ev, 1);) {
        }
        server.step();
    }
    return ok ? 0 : 1;
}

}  // namespace

int main(int argc, char** argv) {
    log::init({.app_name = "net_headless", .level = log::Level::Info});
    console::parse_command_line(argc, argv);
    const int rc = run();
    log::shutdown();
    return rc;
}
