// M-NET2 step 6: WebSocketHost over loopback TCP.

#include <doctest/doctest.h>

#include <chrono>
#include <cstring>
#include <thread>

#include "mce/net/websocket.h"

using namespace mce;
using namespace mce::net;

namespace {

struct Seen {
    PeerId connected = kInvalidPeer;
    u32 connect_data = ~0u;
    bool disconnected = false;
    u32 disconnect_data = ~0u;
    u32 received = 0;
    u8 channel = 0xFF;
    Array<u8> last;
};

void take(Seen& s, const Event& ev) {
    if (ev.kind == EventKind::Connected) {
        s.connected = ev.peer;
        s.connect_data = ev.user_data;
    } else if (ev.kind == EventKind::Disconnected) {
        s.disconnected = true;
        s.disconnect_data = ev.user_data;
    } else {
        ++s.received;
        s.channel = ev.channel;
        s.last.resize(ev.data.size());
        if (!ev.data.empty()) std::memcpy(s.last.data(), ev.data.data(), ev.data.size());
    }
}

// Services both ends until `done` holds or ~3 s pass.
template <class Done>
bool pump(WebSocketHost& a, Seen& sa, WebSocketHost& b, Seen& sb, Done&& done) {
    const auto deadline = std::chrono::steady_clock::now() + std::chrono::seconds(3);
    while (std::chrono::steady_clock::now() < deadline) {
        for (Event ev; a.poll(ev);) take(sa, ev);
        for (Event ev; b.poll(ev);) take(sb, ev);
        a.flush();
        b.flush();
        if (done()) return true;
        std::this_thread::sleep_for(std::chrono::milliseconds(1));
    }
    return false;
}

WebSocketDesc loopback(u32 max_peers = 4) {
    WebSocketDesc d;
    d.bind_address = "127.0.0.1";
    d.max_peers = max_peers;
    d.channels = 3;
    return d;
}

Array<u8> pattern(usize n) {
    Array<u8> out;
    out.resize(n);
    for (usize i = 0; i < n; ++i) out[i] = static_cast<u8>(i * 7 + 3);
    return out;
}

bool same(const Array<u8>& a, const Array<u8>& b) {
    return a.size() == b.size() && (a.empty() || std::memcmp(a.data(), b.data(), a.size()) == 0);
}

}  // namespace

TEST_CASE("net websocket: the handshake's accept key matches RFC 6455's example") {
    CHECK(websocket_accept_key("dGhlIHNhbXBsZSBub25jZQ==") == StringView("s3pPLMBiTxaQ9kYGzzhZRbK+xOo="));
}

TEST_CASE("net websocket: a client connects over loopback, messages of every length cross, then it disconnects") {
    WebSocketHost server;
    REQUIRE(server.listen(loopback()));
    REQUIRE(server.bound_port() != 0);
    WebSocketHost client;
    WebSocketDesc cd = WebSocketHost::client_desc();
    cd.channels = 3;
    REQUIRE(client.open_client(cd));
    const PeerId to_server = client.connect("127.0.0.1", server.bound_port(), 42, "/game");
    REQUIRE(to_server != kInvalidPeer);

    Seen s, c;
    REQUIRE(pump(server, s, client, c, [&] { return s.connected != kInvalidPeer && c.connected == to_server; }));
    CHECK(s.connect_data == 42);
    CHECK(server.is_connected(s.connected));
    CHECK(client.is_connected(to_server));

    // 7-bit, 16-bit and 64-bit frame lengths, both ways (client frames are masked, server's not).
    for (const usize n : {usize(0), usize(5), usize(125), usize(126), usize(300), usize(65535), usize(70000)}) {
        const Array<u8> msg = pattern(n);
        const u32 before_s = s.received, before_c = c.received;
        REQUIRE(client.send(to_server, 2, Span<const u8>(msg.data(), msg.size()), Delivery::Reliable));
        REQUIRE(server.send(s.connected, 1, Span<const u8>(msg.data(), msg.size()), Delivery::Unreliable));
        REQUIRE(pump(server, s, client, c, [&] { return s.received > before_s && c.received > before_c; }));
        CHECK(same(s.last, msg));
        CHECK(s.channel == 2);
        CHECK(same(c.last, msg));
        CHECK(c.channel == 1);
    }
    CHECK_FALSE(client.send(to_server, 3, Span<const u8>(), Delivery::Reliable));  // no such channel
    CHECK(client.counters().messages_sent == 7);
    CHECK(server.counters().messages_received == 7);

    CHECK(client.is_connected(to_server));

    client.disconnect(to_server, 7);
    CHECK_FALSE(client.is_connected(to_server));
    REQUIRE(pump(server, s, client, c, [&] { return s.disconnected && c.disconnected; }));
    CHECK(s.disconnect_data == 7);
    CHECK(c.disconnect_data == 7);
    CHECK_FALSE(server.is_connected(s.connected));
}

TEST_CASE("net websocket: the server closing tells the client, and a full server turns a client away") {
    WebSocketHost server;
    REQUIRE(server.listen(loopback(1)));
    WebSocketHost a, b;
    REQUIRE(a.open_client());
    REQUIRE(b.open_client());
    const PeerId pa = a.connect("127.0.0.1", server.bound_port());
    Seen s, ca;
    REQUIRE(pump(server, s, a, ca, [&] { return s.connected != kInvalidPeer && ca.connected == pa; }));

    const PeerId pb = b.connect("127.0.0.1", server.bound_port());
    REQUIRE(pb != kInvalidPeer);
    Seen cb;
    REQUIRE(pump(server, s, b, cb, [&] { return cb.disconnected; }));  // 503: never connected
    CHECK(cb.connected == kInvalidPeer);

    server.disconnect(s.connected, 9);
    REQUIRE(pump(server, s, a, ca, [&] { return ca.disconnected; }));
    CHECK(ca.disconnect_data == 9);
}

TEST_CASE("net websocket: a message over the limit closes the connection; nobody listening fails the connect") {
    WebSocketDesc small = loopback();
    small.max_message = 1024;
    WebSocketHost server;
    REQUIRE(server.listen(small));
    WebSocketHost client;
    REQUIRE(client.open_client());
    const PeerId p = client.connect("127.0.0.1", server.bound_port());
    Seen s, c;
    REQUIRE(pump(server, s, client, c, [&] { return s.connected != kInvalidPeer && c.connected == p; }));
    const Array<u8> big = pattern(4000);
    REQUIRE(client.send(p, 0, Span<const u8>(big.data(), big.size()), Delivery::Reliable));
    REQUIRE(pump(server, s, client, c, [&] { return s.disconnected && c.disconnected; }));
    CHECK(s.received == 0);

    // A port nobody listens on (the server's, once closed).
    const u16 port = server.bound_port();
    server.close();
    WebSocketHost lonely;
    REQUIRE(lonely.open_client());
    const PeerId q = lonely.connect("127.0.0.1", port);
    if (q != kInvalidPeer) {
        Seen l, unused;
        WebSocketHost idle;
        REQUIRE(idle.open_client());
        REQUIRE(pump(lonely, l, idle, unused, [&] { return l.disconnected; }));
        CHECK(l.connected == kInvalidPeer);
    }
}

TEST_CASE("net websocket: pending_bytes counts what a send queued until flush hands it to the socket") {
    WebSocketHost idle;
    CHECK(idle.pending_bytes(0) == 0);  // not open: nothing is pending

    WebSocketHost server;
    REQUIRE(server.listen(loopback()));
    WebSocketHost client;
    WebSocketDesc cd = WebSocketHost::client_desc();
    cd.channels = 3;
    REQUIRE(client.open_client(cd));
    const PeerId to_server = client.connect("127.0.0.1", server.bound_port());
    REQUIRE(to_server != kInvalidPeer);
    Seen s, c;
    REQUIRE(pump(server, s, client, c, [&] { return s.connected != kInvalidPeer && c.connected == to_server; }));

    CHECK(client.pending_bytes(to_server) == 0);
    CHECK(client.pending_bytes(1000) == 0);  // no such peer slot

    const Array<u8> msg = pattern(200);
    REQUIRE(client.send(to_server, 1, Span<const u8>(msg.data(), msg.size()), Delivery::Reliable));
    CHECK(client.pending_bytes(to_server) >= msg.size());  // framed, so a little more than the payload
    const usize one = client.pending_bytes(to_server);
    REQUIRE(client.send(to_server, 1, Span<const u8>(msg.data(), msg.size()), Delivery::Reliable));
    CHECK(client.pending_bytes(to_server) == 2 * one);

    REQUIRE(pump(server, s, client, c, [&] { return s.received == 2; }));
    CHECK(client.pending_bytes(to_server) == 0);  // all of it reached the socket
}

TEST_CASE("net websocket: a bad description or a full client is refused, and a closed host answers with zeros") {
    WebSocketHost h;
    WebSocketDesc d = loopback();
    d.channels = 0;
    CHECK_FALSE(h.listen(d));
    d = loopback();
    d.max_peers = 0;
    CHECK_FALSE(h.listen(d));

    // Closed: no stats, a broadcast and a connect do nothing.
    CHECK(h.stats(0).rtt_ms == 0);
    const Array<u8> msg = pattern(8);
    h.broadcast(0, Span<const u8>(msg.data(), msg.size()), Delivery::Reliable);
    CHECK(h.connect("127.0.0.1", 1) == kInvalidPeer);

    // A client built with one slot has none left for a second connection.
    WebSocketHost server;
    REQUIRE(server.listen(loopback()));
    WebSocketHost client;
    WebSocketDesc cd = WebSocketHost::client_desc();
    cd.max_peers = 1;
    REQUIRE(client.open_client(cd));
    REQUIRE(client.connect("127.0.0.1", server.bound_port()) != kInvalidPeer);
    CHECK(client.connect("127.0.0.1", server.bound_port()) == kInvalidPeer);
}

TEST_CASE("net websocket: broadcast reaches every connected client, and pings keep a live link up and measure it") {
    WebSocketDesc sd = loopback(3);
    sd.ping_interval_ms = 5;
    WebSocketHost server;
    REQUIRE(server.listen(sd));
    WebSocketHost a, b;
    WebSocketDesc cd = WebSocketHost::client_desc();
    cd.channels = 3;
    cd.ping_interval_ms = 5;
    REQUIRE(a.open_client(cd));
    REQUIRE(b.open_client(cd));
    const PeerId pa = a.connect("127.0.0.1", server.bound_port());
    const PeerId pb = b.connect("127.0.0.1", server.bound_port());
    Seen s, ca, cb;
    const auto both_up = [&] { return ca.connected == pa && cb.connected == pb; };
    const auto run = [&](auto&& done) {
        const auto deadline = std::chrono::steady_clock::now() + std::chrono::seconds(3);
        while (std::chrono::steady_clock::now() < deadline) {
            for (Event ev; server.poll(ev);) take(s, ev);
            for (Event ev; a.poll(ev);) take(ca, ev);
            for (Event ev; b.poll(ev);) take(cb, ev);
            server.flush();
            a.flush();
            b.flush();
            if (done()) return true;
            std::this_thread::sleep_for(std::chrono::milliseconds(1));
        }
        return false;
    };
    REQUIRE(run(both_up));

    const Array<u8> msg = pattern(33);
    server.broadcast(2, Span<const u8>(msg.data(), msg.size()), Delivery::Reliable);
    REQUIRE(run([&] { return ca.received == 1 && cb.received == 1; }));
    CHECK(ca.channel == 2);
    CHECK(same(ca.last, msg));
    CHECK(same(cb.last, msg));

    // Let several ping intervals pass: pongs come back, nobody is dropped, and stats stay readable.
    const auto until = std::chrono::steady_clock::now() + std::chrono::milliseconds(120);
    run([&] { return std::chrono::steady_clock::now() > until; });
    CHECK_FALSE(ca.disconnected);
    CHECK_FALSE(cb.disconnected);
    CHECK_FALSE(s.disconnected);
    CHECK(server.stats(s.connected).rtt_ms < 1000);
    CHECK(a.stats(pa).rtt_ms < 1000);
    CHECK(a.stats(1000).rtt_ms == 0);  // no such peer
}

TEST_CASE("net websocket: a peer that answers no ping is dropped after the timeout") {
    WebSocketDesc sd = loopback();
    sd.ping_interval_ms = 5;
    sd.timeout_ms = 80;
    WebSocketHost server;
    REQUIRE(server.listen(sd));
    WebSocketHost silent;
    REQUIRE(silent.open_client());
    const PeerId p = silent.connect("127.0.0.1", server.bound_port());
    Seen s, c;
    REQUIRE(pump(server, s, silent, c, [&] { return s.connected != kInvalidPeer && c.connected == p; }));

    // From here only the server runs; the client never reads its pings, so no pong comes back.
    const auto deadline = std::chrono::steady_clock::now() + std::chrono::seconds(3);
    while (!s.disconnected && std::chrono::steady_clock::now() < deadline) {
        for (Event ev; server.poll(ev);) take(s, ev);
        server.flush();
        std::this_thread::sleep_for(std::chrono::milliseconds(2));
    }
    CHECK(s.disconnected);
}
