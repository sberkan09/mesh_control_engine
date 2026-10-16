#pragma once
// A whole arena match in one process over SimNetwork (deterministic): a relay, a server that makes a
// listed "arena" room on it, and up to four bot clients that find it by quick play. CI and the demo
// run this; the windowed sample draws one of its clients.

#include "client.h"
#include "server.h"

namespace arena {

struct SessionDesc {
    u32 clients = 4;
    u32 latency_ms = 150;
    u32 jitter_ms = 20;
    f32 loss = 0.03f;
    u64 seed = 1;
};

inline bool same_bits(f32 a, f32 b) {
    u32 x = 0, y = 0;
    std::memcpy(&x, &a, sizeof x);
    std::memcpy(&y, &b, sizeof y);
    return x == y;
}
inline bool same_bits(const LocalTransform& a, const LocalTransform& b) {
    const f32 fa[10] = {a.position.x, a.position.y, a.position.z, a.rotation.x, a.rotation.y,
                        a.rotation.z, a.rotation.w, a.scale.x,    a.scale.y,    a.scale.z};
    const f32 fb[10] = {b.position.x, b.position.y, b.position.z, b.rotation.x, b.rotation.y,
                        b.rotation.z, b.rotation.w, b.scale.x,    b.scale.y,    b.scale.z};
    for (int i = 0; i < 10; ++i)
        if (!same_bits(fa[i], fb[i])) return false;
    return true;
}

class Session {
public:
    static constexpr const char* kMode = "arena";

    SessionDesc desc;
    Server server;
    Client clients[kMaxPlayers];
    net::RelayServer relay;
    u64 session_tick = 0;
    // A person plays client 0 (windowed local play): its hands are these, not a bot's.
    bool human = false;
    Input human_input;
    bool human_fire = false;

    // Builds the match; the arena is loaded from `assets` (scene `scene_path`).
    Result<void> start(const SessionDesc& d, asset::AssetSystem& assets, StringView scene_path) {
        desc = d;
        desc.clients = d.clients < 1 ? 1 : (d.clients > kMaxPlayers ? kMaxPlayers : d.clients);
        net::LinkConditions link;
        link.latency_ms = d.latency_ms;
        link.jitter_ms = d.jitter_ms;
        link.loss = d.loss;
        network_ = mem::make_unique<net::SimNetwork>(mem::heap(), d.seed);
        network_->set_default_conditions(link);
        relay_end_ = &network_->add_endpoint(kMaxPlayers + 2, 2);
        relay.init();
        relay.add_transport(*relay_end_);

        net::RelayClientDesc room_desc;
        room_desc.channels = kChannels;
        server_end_ = &network_->add_endpoint(1, 2);
        server_room_.start(*server_end_, server_end_->connect(*relay_end_), room_desc);
        net::RelayRoomOptions options;
        options.listed = true;
        options.mode = kMode;
        server_room_.create_room(desc.clients + 1, options);
        MCE_TRY(server.start(server_room_, assets, scene_path));

        for (u32 c = 0; c < desc.clients; ++c) {
            net::SimEndpoint& end = network_->add_endpoint(1, 2);
            rooms_[c].start(end, end.connect(*relay_end_), room_desc);
            clients[c].start(rooms_[c], net::kInvalidPeer, &rooms_[c]);
        }
        return ok();
    }

    net::RelayClient& server_room() { return server_room_; }
    net::RelayClient& room(u32 c) { return rooms_[c]; }
    net::SimNetwork& network() { return *network_; }

    // One 1/60 s tick of the match: the relay, the server, every bot, then the network moves on.
    void tick() {
        const u64 t = session_tick;
        relay.update();
        // The players look for a game once the server's room is open.
        if (server_room_.state() == net::RelayClient::State::InRoom)
            for (u32 c = 0; c < desc.clients; ++c)
                if (rooms_[c].state() == net::RelayClient::State::Idle) rooms_[c].quick_play(kMode, desc.clients + 1);
        server.step();
        for (u32 c = 0; c < desc.clients; ++c) {
            bool fire = false;
            Input in = bot_input(clients[c], c, t, fire);
            if (human && c == 0) {
                in = human_input;
                fire = human_fire;
            }
            clients[c].step(in, fire && (human || t > 60));
        }
        advance();
    }

    // The server stops simulating and repeats its newest snapshot until every client has it (a lost
    // one is superseded by the next) and draws it. Up to ten virtual seconds.
    bool settle() {
        const u32 final_tick = static_cast<u32>(server.tick);
        for (u32 i = 0; i < 600; ++i) {
            if (caught_up(final_tick)) return true;
            relay.update();
            server.receive();
            server.send_snapshots();
            for (u32 c = 0; c < desc.clients; ++c) clients[c].step(Input{}, false, false);
            advance();
        }
        return caught_up(final_tick);
    }

    // Prints the match, and says whether the gates hold: every mirror equals the server's bit for
    // bit, the replicated fields too, and shots rewound at least as often as they hit at the present.
    bool report(std::FILE* out) {
        u32 replicated = 0;
        server.world.query<const net::NetId>().each([&](const net::NetId&) { ++replicated; });
        const ServerStats& s = server.stats;
        std::fprintf(out, "net_arena (sim): %u clients, latency %u ms, jitter %u ms, loss %.3f, seed %llu, %llu ticks\n",
                     desc.clients, desc.latency_ms, desc.jitter_ms, static_cast<double>(desc.loss),
                     static_cast<unsigned long long>(desc.seed), static_cast<unsigned long long>(server.tick));
        const net::RelayStats& rs = relay.stats();
        const bool matched_by_quick_play = rs.quick_plays == desc.clients && rs.rooms == 1;
        std::fprintf(out, "  relay: room %.*s (%u members), %llu quick plays, %llu messages forwarded -> %s\n",
                     static_cast<int>(server_room_.room_code().size()), server_room_.room_code().data(),
                     server_room_.connected_count() + 1, static_cast<unsigned long long>(rs.quick_plays),
                     static_cast<unsigned long long>(rs.forwarded), matched_by_quick_play ? "OK" : "FAILED");
        bool ok = matched_by_quick_play && server.players() == desc.clients;
        for (u32 c = 0; c < desc.clients; ++c) {
            Client& client = clients[c];
            u32 matched = 0, fields = 0;
            server.world.query<const net::NetId, const LocalTransform>().each(
                [&](Entity e, const net::NetId& id, const LocalTransform& t) {
                    const Entity mirror = client.replication.find(id.id);
                    const LocalTransform* mt = client.world.alive(mirror) ? client.world.try_get<LocalTransform>(mirror) : nullptr;
                    if (mt && same_bits(*mt, t)) ++matched;
                    const ArenaHealth* sh = server.world.try_get<ArenaHealth>(e);
                    const ArenaHealth* ch = client.world.alive(mirror) ? client.world.try_get<ArenaHealth>(mirror) : nullptr;
                    if (sh && ch && sh->hp == ch->hp && sh->frags == ch->frags && sh->deaths == ch->deaths && sh->shots == ch->shots)
                        ++fields;
                });
            const ArenaHealth* h = client.health();
            const bool client_ok = client.connected && client.own_id != 0 && matched == replicated &&
                                   client.world.entity_count() == replicated && fields == desc.clients;
            std::fprintf(out, "  client %u: %u snapshots (%u deltas), %u/%u entities and %u/%u health records match, %u shots fired, "
                              "%u frags, %u deaths, %u corrections -> %s\n",
                         c, client.snapshots, client.deltas, matched, replicated, fields, desc.clients, client.shots_sent,
                         h ? h->frags : 0, h ? h->deaths : 0, client.predictor.stats().corrections, client_ok ? "OK" : "FAILED");
            ok = ok && client_ok;
        }
        const bool shots_ok = s.shots > 0 && s.hits_rewound > 0 && s.hits_rewound >= s.hits_present;
        std::fprintf(out,
                     "  shots: %u (%u refused, %u older than the rewind), hit where the shooter drew the target %u, hit at the "
                     "server's present %u, %u kills -> %s\n",
                     s.shots, s.refused, s.too_old, s.hits_rewound, s.hits_present, s.kills, shots_ok ? "OK" : "FAILED");
        std::fprintf(out, "  snapshots: %llu bytes sent to players, %llu as full broadcasts (%.0f%%)\n",
                     static_cast<unsigned long long>(s.bytes_sent), static_cast<unsigned long long>(s.bytes_full),
                     s.bytes_full ? 100.0 * static_cast<double>(s.bytes_sent) / static_cast<double>(s.bytes_full) : 0.0);
        return ok && shots_ok;
    }

    // The run's fingerprint: every world, what the clients' links did to the traffic, and the shot log.
    u64 fingerprint() {
        Hasher64 h;
        h.u64v(server.world.state_hash());
        for (u32 c = 0; c < desc.clients; ++c) {
            h.u64v(clients[c].world.state_hash());
            h.u64v(clients[c].predictor.stats().corrections);
            h.u64v(clients[c].net->counters().messages_received);
            h.u64v(clients[c].net->counters().messages_dropped);
            h.u64v(clients[c].shots_sent);
        }
        h.u64v(server.stats.shots);
        h.u64v(server.stats.hits_rewound);
        h.u64v(server.stats.hits_present);
        h.u64v(server.stats.kills);
        h.u64v(server.stats.bytes_sent);
        h.u64v(relay.stats().forwarded);
        return h.value();
    }

private:
    void advance() {
        const u64 next_ms = ++session_tick * 1000 / 60;
        network_->advance(static_cast<u32>(next_ms - clock_ms_));
        clock_ms_ = next_ms;
    }

    bool caught_up(u32 final_tick) {
        for (u32 c = 0; c < desc.clients; ++c) {
            const Client& k = clients[c];
            if (k.replication.last_tick() != final_tick || k.applied_hash != server.snapshot_hash) return false;
            if (k.predictor.stats().pending != 0 || k.predictor.stats().offset != 0.0f) return false;
            const net::InterpolationStats& is = k.replication.interpolation_stats();
            if (is.interpolated != 0 || is.extrapolated != 0 || is.held + is.predicted != k.replication.size()) return false;
        }
        return true;
    }

    mem::UniquePtr<net::SimNetwork> network_;
    net::SimEndpoint* relay_end_ = nullptr;
    net::SimEndpoint* server_end_ = nullptr;
    net::RelayClient server_room_;
    net::RelayClient rooms_[kMaxPlayers];
    u64 clock_ms_ = 0;
};

}  // namespace arena
