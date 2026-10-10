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
