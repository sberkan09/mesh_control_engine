#include <doctest/doctest.h>

#include <chrono>

#include "mce/net/host.h"

using namespace mce;
using namespace mce::net;

namespace {

// Services both hosts until `done` holds or ~3 s pass (loopback normally takes a few ms).
template <class OnEvent, class Done>
bool pump(Host& a, Host& b, OnEvent&& on_event, Done&& done) {
    const auto deadline = std::chrono::steady_clock::now() + std::chrono::seconds(3);
    while (std::chrono::steady_clock::now() < deadline) {
        for (Event ev; a.poll(ev, 1);) on_event(0, ev);
        for (Event ev; b.poll(ev, 1);) on_event(1, ev);
        if (done()) return true;
    }
    return false;
}

HostDesc loopback_server() {
    HostDesc d;
    d.bind_address = "127.0.0.1";
    d.port = 0;  // any free port
    d.max_peers = 4;
    d.channels = 2;
    return d;
}

}  // namespace

TEST_CASE("net host: a client connects over loopback, both sides exchange messages, then disconnect") {
    Host server;
    REQUIRE(server.listen(loopback_server()));
    REQUIRE(server.bound_port() != 0);

    Host client;
    REQUIRE(client.open_client());
    const PeerId to_server = client.connect("127.0.0.1", server.bound_port(), 42);
    REQUIRE(to_server != kInvalidPeer);

    PeerId client_on_server = kInvalidPeer;
    bool client_connected = false;
    u32 hello_data = 0;
    REQUIRE(pump(
        server, client,
        [&](int side, const Event& ev) {
            if (ev.kind != EventKind::Connected) return;
            if (side == 0) {
                client_on_server = ev.peer;
                hello_data = ev.user_data;
            } else {
                client_connected = true;
            }
        },
        [&] { return client_on_server != kInvalidPeer && client_connected; }));
    CHECK(hello_data == 42);
    CHECK(server.connected_count() == 1);
    CHECK(server.is_connected(client_on_server));
    CHECK(client.is_connected(to_server));

    // Reliable both ways, on different channels.
    const u8 ping[4] = {'p', 'i', 'n', 'g'};
    CHECK(client.send(to_server, 0, Span<const u8>(ping, 4), Delivery::Reliable));
    CHECK_FALSE(client.send(to_server, 7, Span<const u8>(ping, 4), Delivery::Reliable));  // no channel 7
    bool got_ping = false, got_pong = false;
    u8 pong_channel = 0xFF;
    REQUIRE(pump(
        server, client,
        [&](int side, const Event& ev) {
            if (ev.kind != EventKind::Received) return;
            if (side == 0 && ev.data.size() == 4 && ev.data[0] == 'p') {
                got_ping = true;
                const u8 pong[2] = {'o', 'k'};
                server.send(ev.peer, 1, Span<const u8>(pong, 2), Delivery::Reliable);
            } else if (side == 1 && ev.data.size() == 2) {
                got_pong = true;
                pong_channel = ev.channel;
            }
        },
        [&] { return got_ping && got_pong; }));
    CHECK(pong_channel == 1);

    // Many reliable messages arrive complete and in order.
    for (u32 i = 0; i < 100; ++i) {
        const u8 b[1] = {static_cast<u8>(i)};
        server.broadcast(0, Span<const u8>(b, 1), Delivery::Reliable);
    }
    u32 next = 0;
    bool ordered = true;
    REQUIRE(pump(
        server, client,
        [&](int side, const Event& ev) {
            if (side == 1 && ev.kind == EventKind::Received && ev.data.size() == 1) {
                ordered = ordered && ev.data[0] == next;
                ++next;
            }
        },
        [&] { return next == 100; }));
    CHECK(ordered);
    CHECK(client.stats(to_server).rtt_ms < 1000);
    CHECK(client.bytes_sent() > 0);
    CHECK(server.bytes_received() > 0);

    // Graceful disconnect is reported on both sides.
    client.disconnect(to_server, 7);
    bool server_saw = false, client_saw = false;
    u32 bye_data = 0;
    REQUIRE(pump(
        server, client,
        [&](int side, const Event& ev) {
            if (ev.kind != EventKind::Disconnected) return;
            if (side == 0) {
                server_saw = true;
                bye_data = ev.user_data;
            } else {
                client_saw = true;
            }
        },
        [&] { return server_saw && client_saw; }));
    CHECK(bye_data == 7);
    CHECK(server.connected_count() == 0);
    CHECK_FALSE(client.send(to_server, 0, Span<const u8>(ping, 4), Delivery::Reliable));
}

TEST_CASE("net host: invalid uses fail cleanly") {
    Host h;
    CHECK_FALSE(h.is_open());
    Event ev;
    CHECK_FALSE(h.poll(ev));
    CHECK(h.connect("127.0.0.1", 1) == kInvalidPeer);
    const u8 msg[1] = {0};
    CHECK_FALSE(h.send(0, 0, Span<const u8>(msg, 1), Delivery::Reliable));

    HostDesc bad;
    bad.channels = 0;
    CHECK_FALSE(h.listen(bad));

    // Two servers cannot share a port.
    Host a;
    REQUIRE(a.listen([] {
        HostDesc d;
        d.bind_address = "127.0.0.1";
        return d;
    }()));
    HostDesc same;
    same.bind_address = "127.0.0.1";
    same.port = a.bound_port();
    Host b;
    CHECK_FALSE(b.listen(same));
    CHECK_FALSE(b.is_open());

    // close() is idempotent; a closed host reopens.
    a.close();
    a.close();
    CHECK_FALSE(a.is_open());
    CHECK(a.open_client());
}

TEST_CASE("net host: a name that does not resolve and a full client are refused") {
    Host h;
    HostDesc d = loopback_server();
    d.bind_address = "no such host";
    CHECK_FALSE(h.listen(d));
    CHECK_FALSE(h.is_open());

    Host server;
    REQUIRE(server.listen(loopback_server()));
    Host client;
    REQUIRE(client.open_client());  // one peer slot
    CHECK(client.connect("no such host", server.bound_port()) == kInvalidPeer);
    REQUIRE(client.connect("127.0.0.1", server.bound_port()) != kInvalidPeer);
    CHECK(client.connect("127.0.0.1", server.bound_port()) == kInvalidPeer);  // the slot is taken
}

TEST_CASE("net host: sequenced and unreliable messages, an unreliable broadcast, flush and disconnect_now") {
    Host server;
    REQUIRE(server.listen(loopback_server()));
    Host client;
    REQUIRE(client.open_client());
    const PeerId to_server = client.connect("127.0.0.1", server.bound_port());
    PeerId on_server = kInvalidPeer;
    bool client_up = false;
    REQUIRE(pump(
        server, client,
        [&](int side, const Event& ev) {
            if (ev.kind != EventKind::Connected) return;
            if (side == 0)
                on_server = ev.peer;
            else
                client_up = true;
        },
        [&] { return on_server != kInvalidPeer && client_up; }));

    // On a quiet loopback link none of these is lost or reordered.
    const u8 a[1] = {'s'}, b[1] = {'u'}, c[1] = {'b'};
    CHECK(client.send(to_server, 0, Span<const u8>(a, 1), Delivery::Sequenced));
    CHECK(client.send(to_server, 1, Span<const u8>(b, 1), Delivery::Unreliable));
    client.flush();
    u32 sequenced = 0, unreliable = 0;
    REQUIRE(pump(
        server, client,
        [&](int side, const Event& ev) {
            if (side != 0 || ev.kind != EventKind::Received || ev.data.size() != 1) return;
            if (ev.data[0] == 's') ++sequenced;
            if (ev.data[0] == 'u') ++unreliable;
        },
        [&] { return sequenced == 1 && unreliable == 1; }));

    server.broadcast(1, Span<const u8>(c, 1), Delivery::Unreliable);
    u32 broadcast_seen = 0;
    REQUIRE(pump(
        server, client,
        [&](int side, const Event& ev) {
            if (side == 1 && ev.kind == EventKind::Received && ev.data.size() == 1 && ev.data[0] == 'b')
                ++broadcast_seen;
        },
        [&] { return broadcast_seen == 1; }));

    // disconnect_now forgets the peer at once, with no Disconnected event for the caller; the other
    // side still learns of it.
    server.disconnect_now(on_server, 5);
    CHECK_FALSE(server.is_connected(on_server));
    bool server_event = false, client_saw = false;
    u32 bye = 0;
    pump(
        server, client,
        [&](int side, const Event& ev) {
            if (ev.kind != EventKind::Disconnected) return;
            if (side == 0)
                server_event = true;
            else {
                client_saw = true;
                bye = ev.user_data;
            }
        },
        [&] { return client_saw; });
    CHECK(client_saw);
    CHECK(bye == 5);
    CHECK_FALSE(server_event);
}

TEST_CASE("net host: simulated latency holds a message back until it is due, then flush hands it to ENet") {
    Host server;
    REQUIRE(server.listen(loopback_server()));
    Host client;
    REQUIRE(client.open_client());
    const PeerId to_server = client.connect("127.0.0.1", server.bound_port());
    bool up = false;
    REQUIRE(pump(
        server, client,
        [&](int side, const Event& ev) {
            if (side == 1 && ev.kind == EventKind::Connected) up = true;
        },
        [&] { return up; }));

    LinkConditions slow;
    slow.latency_ms = 40;
    client.set_conditions(slow);
    CHECK(client.conditions().latency_ms == 40);
    const u8 m[1] = {'x'};
    CHECK(client.send(to_server, 0, Span<const u8>(m, 1), Delivery::Reliable));
    CHECK(client.delayed_count() == 1);  // held, not sent
    client.flush();
    CHECK(client.delayed_count() == 1);  // not due yet

    u32 got = 0;
    REQUIRE(pump(
        server, client,
        [&](int side, const Event& ev) {
            if (side == 0 && ev.kind == EventKind::Received) ++got;
        },
        [&] { return got == 1; }));
    CHECK(client.delayed_count() == 0);
}
