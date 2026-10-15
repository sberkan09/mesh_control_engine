// M-NET2 step 1: SimNetwork (latency, jitter and loss on a virtual clock), Host::set_conditions,
// and NetStatsMeter.

#include <doctest/doctest.h>

#include <chrono>
#include <cstring>

#include "mce/ecs/dev_stats.h"
#include "mce/net/net.h"

using namespace mce;
using namespace mce::net;

namespace {

struct Pair {
    SimNetwork network;
    SimEndpoint* server = nullptr;
    SimEndpoint* client = nullptr;
    PeerId to_server = kInvalidPeer;
    PeerId to_client = kInvalidPeer;

    explicit Pair(const LinkConditions& c, u64 seed = 1) : network(seed) {
        network.set_default_conditions(c);
        server = &network.add_endpoint(4, 2);
        client = &network.add_endpoint(1, 2);
        to_server = client->connect(*server, 7);
        // A round trip completes the handshake.
        for (u32 i = 0; i < 2 * (c.latency_ms + c.jitter_ms) + 64 * (c.latency_ms * 2 + 1) + 2; ++i) {
            network.advance(1);
            for (Event ev; server->poll(ev);)
                if (ev.kind == EventKind::Connected) to_client = ev.peer;
            for (Event ev; client->poll(ev);) {
            }
            if (to_client != kInvalidPeer && client->is_connected(to_server)) break;
        }
    }
};

Span<const u8> bytes_of(const u32& v) { return Span<const u8>(reinterpret_cast<const u8*>(&v), sizeof v); }

u32 value_of(const Event& ev) {
    u32 v = 0;
    if (ev.data.size() == sizeof v) std::memcpy(&v, ev.data.data(), sizeof v);
    return v;
}

}  // namespace

TEST_CASE("sim network: messages arrive after the link's latency, and the handshake takes a round trip") {
    LinkConditions c;
    c.latency_ms = 40;
    SimNetwork network;
    network.set_default_conditions(c);
    SimEndpoint& server = network.add_endpoint(4, 2);
    SimEndpoint& client = network.add_endpoint(1, 2);
    const PeerId to_server = client.connect(server, 99);
    REQUIRE(to_server != kInvalidPeer);
    CHECK_FALSE(client.is_connected(to_server));

    Event ev;
    network.advance(39);
    CHECK_FALSE(server.poll(ev));
    network.advance(1);
    REQUIRE(server.poll(ev));
    CHECK(ev.kind == EventKind::Connected);
    CHECK(ev.user_data == 99);
    const PeerId to_client = ev.peer;
    CHECK(server.is_connected(to_client));
    network.advance(39);
    CHECK_FALSE(client.poll(ev));
    network.advance(1);
    REQUIRE(client.poll(ev));
    CHECK(ev.kind == EventKind::Connected);
    CHECK(client.is_connected(to_server));
    CHECK(network.now_ms() == 80);

    const u32 hello = 0xC0FFEE;
    CHECK(client.send(to_server, 1, bytes_of(hello), Delivery::Reliable));
    CHECK_FALSE(client.send(to_server, 5, bytes_of(hello), Delivery::Reliable));  // no channel 5
    network.advance(39);
    CHECK_FALSE(server.poll(ev));
    network.advance(1);
    REQUIRE(server.poll(ev));
    CHECK(ev.kind == EventKind::Received);
    CHECK(ev.channel == 1);
    CHECK(value_of(ev) == hello);
    CHECK(server.counters().messages_received == 1);
    CHECK(client.counters().messages_sent == 1);

    const PeerStats s = client.stats(to_server);
    CHECK(s.rtt_ms == 80);
    CHECK(s.packet_loss == 0);
}

TEST_CASE("sim network: jitter reorders unreliable messages but never reliable or sequenced ones") {
    LinkConditions c;
    c.latency_ms = 20;
    c.jitter_ms = 30;
    Pair p(c);
    REQUIRE(p.to_client != kInvalidPeer);

    for (u32 i = 1; i <= 200; ++i) {
        p.client->send(p.to_server, 0, bytes_of(i), Delivery::Reliable);
        p.client->send(p.to_server, 1, bytes_of(i), i % 2 ? Delivery::Sequenced : Delivery::Unreliable);
        p.network.advance(1);
    }
    for (u32 i = 0; i < 100; ++i) p.network.advance(1);

    u32 last_reliable = 0, last_sequenced = 0, reliable = 0, sequenced = 0, unreliable = 0;
    bool reliable_in_order = true, sequenced_in_order = true;
    Array<u32> unreliable_order;
    for (Event ev; p.server->poll(ev);) {
        if (ev.kind != EventKind::Received) continue;
        const u32 v = value_of(ev);
        if (ev.channel == 0) {
            reliable_in_order = reliable_in_order && v == last_reliable + 1;
            last_reliable = v;
            ++reliable;
        } else if (v % 2) {
            sequenced_in_order = sequenced_in_order && v > last_sequenced;
            last_sequenced = v;
            ++sequenced;
        } else {
            unreliable_order.push_back(v);
            ++unreliable;
        }
    }
    CHECK(reliable == 200);
    CHECK(reliable_in_order);
    CHECK(sequenced_in_order);
    CHECK(sequenced == 100);  // no loss: jitter cannot overtake on an ordered channel, so none is stale
    CHECK(unreliable == 100);
    bool reordered = false;
    for (usize i = 1; i < unreliable_order.size(); ++i)
        reordered = reordered || unreliable_order[i] < unreliable_order[i - 1];
    CHECK(reordered);  // 30 ms of jitter over 2 ms spacing
}

TEST_CASE("sim network: loss drops sequenced and unreliable messages at its rate and delays reliable ones") {
    LinkConditions c;
    c.latency_ms = 10;
    c.loss = 0.25f;
    Pair p(c, 7);
    REQUIRE(p.to_client != kInvalidPeer);

    constexpr u32 kSent = 4000;
    for (u32 i = 1; i <= kSent; ++i) {
        p.client->send(p.to_server, 0, bytes_of(i), Delivery::Reliable);
        p.client->send(p.to_server, 1, bytes_of(i), Delivery::Unreliable);
    }
    p.network.advance(2000);
    u32 reliable = 0, unreliable = 0, last = 0;
    bool in_order = true;
    for (Event ev; p.server->poll(ev);) {
        if (ev.kind != EventKind::Received) continue;
        if (ev.channel == 0) {
            in_order = in_order && value_of(ev) == last + 1;
            last = value_of(ev);
            ++reliable;
        } else {
            ++unreliable;
        }
    }
    CHECK(reliable == kSent);
    CHECK(in_order);
    const f64 kept = static_cast<f64>(unreliable) / kSent;
    CHECK(kept > 0.72);
    CHECK(kept < 0.78);
    CHECK(p.client->counters().messages_dropped == kSent - unreliable);
    CHECK(p.client->stats(p.to_server).packet_loss == static_cast<u32>(0.25f * 65536.0f));
}

TEST_CASE("sim network: the same seed gives the same deliveries; another seed does not") {
    auto trace = [](u64 seed) {
        LinkConditions c;
        c.latency_ms = 30;
        c.jitter_ms = 25;
        c.loss = 0.1f;
        Pair p(c, seed);
        Array<u32> out;
        for (u32 i = 1; i <= 300; ++i) {
            p.client->send(p.to_server, 1, bytes_of(i), Delivery::Unreliable);
            p.server->send(p.to_client, 1, bytes_of(i + 1000), Delivery::Sequenced);
            p.network.advance(3);
            for (Event ev; p.server->poll(ev);)
                if (ev.kind == EventKind::Received) out.push_back(value_of(ev));
            for (Event ev; p.client->poll(ev);)
                if (ev.kind == EventKind::Received) out.push_back(value_of(ev));
            out.push_back(0);  // a tick boundary
        }
        return out;
    };
    const Array<u32> a = trace(5), b = trace(5), other = trace(6);
    REQUIRE(a.size() == b.size());
    bool same = true;
    for (usize i = 0; i < a.size(); ++i) same = same && a[i] == b[i];
    CHECK(same);
    bool differs = a.size() != other.size();
    for (usize i = 0; !differs && i < a.size(); ++i) differs = a[i] != other[i];
    CHECK(differs);
}

TEST_CASE("sim network: a full server refuses, and a disconnect reaches the other side after its messages") {
    LinkConditions c;
    c.latency_ms = 5;
    SimNetwork network;
    network.set_default_conditions(c);
    SimEndpoint& server = network.add_endpoint(1, 1);
    SimEndpoint& a = network.add_endpoint(1, 1);
    SimEndpoint& b = network.add_endpoint(1, 1);
    const PeerId a_to_server = a.connect(server);
    network.advance(1);
    const PeerId b_to_server = b.connect(server);
    network.advance(20);
    CHECK(a.is_connected(a_to_server));
    CHECK_FALSE(b.is_connected(b_to_server));
    bool refused = false;
    for (Event ev; b.poll(ev);) refused = refused || ev.kind == EventKind::Disconnected;
    CHECK(refused);
    CHECK(server.connected_count() == 1);
    for (Event ev; server.poll(ev);) {
    }
    for (Event ev; a.poll(ev);) {
    }

    const u32 last_words = 77;
    a.send(a_to_server, 0, bytes_of(last_words), Delivery::Reliable);
    a.disconnect(a_to_server, 3);
    Event ev;
    REQUIRE(a.poll(ev));  // locally at once
    CHECK(ev.kind == EventKind::Disconnected);
    CHECK(ev.user_data == 3);
    CHECK_FALSE(a.is_connected(a_to_server));
    CHECK_FALSE(a.send(a_to_server, 0, bytes_of(last_words), Delivery::Reliable));
    network.advance(10);
    REQUIRE(server.poll(ev));
    CHECK(ev.kind == EventKind::Received);
    CHECK(value_of(ev) == 77);
    REQUIRE(server.poll(ev));
    CHECK(ev.kind == EventKind::Disconnected);
    CHECK(ev.user_data == 3);
    CHECK(server.connected_count() == 0);
}

TEST_CASE("net stats meter: rates over a window, published as the overlay's net line") {
    LinkConditions c;
    c.latency_ms = 50;
    c.jitter_ms = 10;
    Pair p(c);
    REQUIRE(p.to_client != kInvalidPeer);
    NetStatsMeter meter(1.0);
    u8 payload[100] = {};
    for (u32 ms = 0; ms < 1000; ms += 10) {
        p.server->send(p.to_client, 1, Span<const u8>(payload, 100), Delivery::Unreliable);
        p.network.advance(10);
        for (Event ev; p.client->poll(ev);) {
        }
        meter.update(*p.server, 0.010);
    }
    const NetRates& r = meter.rates();
    CHECK(r.peers == 1);
    CHECK(r.rtt_ms == 110);
    CHECK(r.rtt_variance_ms == 10);
    CHECK(r.up_bytes_per_s == doctest::Approx(10000.0).epsilon(0.02));
    CHECK(r.up_messages_per_s == doctest::Approx(100.0).epsilon(0.02));

    World world;
    meter.publish(world);
    const DevStatsLines* lines = world.try_resource<DevStatsLines>();
    REQUIRE(lines != nullptr);
    REQUIRE(lines->count == 1);
    CHECK(lines->lines[0].key.view() == "net");
    CHECK(lines->lines[0].text.view().starts_with("NET 1 peer  RTT 110 ms"));
    meter.publish(world);  // replaces, does not add
    CHECK(lines->count == 1);
}

TEST_CASE("net host: set_conditions holds outgoing messages back and drops unreliable ones") {
    HostDesc sd;
    sd.bind_address = "127.0.0.1";
    sd.max_peers = 2;
    sd.channels = 2;
    Host server;
    REQUIRE(server.listen(sd));
    Host client;
    REQUIRE(client.open_client());
    const PeerId to_server = client.connect("127.0.0.1", server.bound_port());
    REQUIRE(to_server != kInvalidPeer);
    const auto deadline = std::chrono::steady_clock::now() + std::chrono::seconds(3);
    while (!client.is_connected(to_server) && std::chrono::steady_clock::now() < deadline) {
        for (Event ev; server.poll(ev, 1);) {
        }
        for (Event ev; client.poll(ev, 1);) {
        }
    }
    REQUIRE(client.is_connected(to_server));

    LinkConditions c;
    c.latency_ms = 30;
    c.loss = 1.0f;  // every unreliable message is lost; reliable ones still arrive
    client.set_conditions(c, 3);
    const u32 v = 5;
    const auto sent_at = std::chrono::steady_clock::now();
    REQUIRE(client.send(to_server, 0, bytes_of(v), Delivery::Reliable));
    REQUIRE(client.send(to_server, 1, bytes_of(v), Delivery::Sequenced));
    CHECK(client.delayed_count() == 1);
    CHECK(client.counters().messages_dropped == 1);

    bool got_reliable = false, got_sequenced = false;
    auto arrived_at = sent_at;
    const auto until = std::chrono::steady_clock::now() + std::chrono::seconds(3);
    while (!got_reliable && std::chrono::steady_clock::now() < until) {
        for (Event ev; client.poll(ev, 1);) {
        }
        for (Event ev; server.poll(ev, 1);) {
            if (ev.kind != EventKind::Received) continue;
            if (ev.channel == 0) {
                got_reliable = true;
                arrived_at = std::chrono::steady_clock::now();
            } else {
                got_sequenced = true;
            }
        }
    }
    CHECK(got_reliable);
    CHECK_FALSE(got_sequenced);
    CHECK(std::chrono::duration_cast<std::chrono::milliseconds>(arrived_at - sent_at).count() >= 30);
    CHECK(client.delayed_count() == 0);
}
