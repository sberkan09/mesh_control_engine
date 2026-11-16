// M-FUZZ 3c: the WebSocket transport reads bytes a stranger sends. A raw TCP client sends the host a handshake and
// frames with damage (the server side), and a raw TCP server answers the host's client with a damaged handshake and
// frames (the client side). A connection may be refused or dropped; the host must not crash, hang or grow without bound.
// Loopback sockets, POSIX only.

#include <doctest/doctest.h>

#if defined(__unix__) || defined(__APPLE__)

#include <arpa/inet.h>
#include <csignal>
#include <fcntl.h>
#include <netinet/in.h>
#include <sys/socket.h>
#include <unistd.h>

#include <chrono>
#include <cstdio>
#include <cstdlib>
#include <cstring>
#include <string>
#include <thread>
#include <vector>

#include "mce/log.h"
#include "mce/net/websocket.h"
#include "mutator.h"
#include "test_support.h"

using namespace mce;
using namespace mce::net;

namespace {

u32 iterations() {
    const char* e = std::getenv("MCE_FUZZ_ITERS");
    return e ? static_cast<u32>(std::strtoul(e, nullptr, 10)) : 150u;
}

void put(std::string& s, std::initializer_list<unsigned> bytes) {
    for (unsigned b : bytes) s.push_back(static_cast<char>(b));
}

// One client frame (masked with a fixed key) or server frame (unmasked).
std::string frame(bool client, unsigned op, const std::string& payload, bool fin = true) {
    std::string f;
    put(f, {(fin ? 0x80u : 0u) | op});
    const unsigned mask = client ? 0x80u : 0u;
    if (payload.size() < 126) {
        put(f, {mask | static_cast<unsigned>(payload.size())});
    } else if (payload.size() < 65536) {
        put(f, {mask | 126u, static_cast<unsigned>(payload.size() >> 8), static_cast<unsigned>(payload.size() & 0xFF)});
    } else {
        put(f, {mask | 127u, 0, 0, 0, 0, static_cast<unsigned>(payload.size() >> 24), static_cast<unsigned>((payload.size() >> 16) & 0xFF),
                static_cast<unsigned>((payload.size() >> 8) & 0xFF), static_cast<unsigned>(payload.size() & 0xFF)});
    }
    const unsigned char key[4] = {0x12, 0x34, 0x56, 0x78};
    if (client) f.append(reinterpret_cast<const char*>(key), 4);
    for (size_t i = 0; i < payload.size(); ++i) f.push_back(static_cast<char>(client ? (payload[i] ^ key[i & 3]) : payload[i]));
    return f;
}

std::string frames_for(bool client) {
    std::string out;
    out += frame(client, 2, std::string("\x00" "hello", 6));
    out += frame(client, 9, "ping");
    out += frame(client, 2, std::string("\x01", 1) + std::string(300, 'x'));
    out += frame(client, 2, std::string("\x02" "part one"), false);
    out += frame(client, 0, " part two", true);
    out += frame(client, 10, "pong");
    out += frame(client, 8, std::string("\x03\xE8" "bye", 5));
    return out;
}

const char* kRequest =
    "GET /?mce_user=7 HTTP/1.1\r\nHost: 127.0.0.1\r\nUpgrade: websocket\r\nConnection: Upgrade\r\n"
    "Sec-WebSocket-Key: dGhlIHNhbXBsZSBub25jZQ==\r\nSec-WebSocket-Version: 13\r\n\r\n";

int connect_to(u16 port) {
    const int s = ::socket(AF_INET, SOCK_STREAM, 0);
    sockaddr_in a{};
    a.sin_family = AF_INET;
    a.sin_port = htons(port);
    ::inet_pton(AF_INET, "127.0.0.1", &a.sin_addr);
    if (::connect(s, reinterpret_cast<sockaddr*>(&a), sizeof(a)) != 0) {
        ::close(s);
        return -1;
    }
    return s;
}

void send_all(int s, const char* d, size_t n) {
    ::signal(SIGPIPE, SIG_IGN);
    while (n) {
        const ssize_t w = ::send(s, d, n, 0);
        if (w <= 0) return;
        d += w;
        n -= static_cast<size_t>(w);
    }
}

void pump(WebSocketHost& h, int rounds) {
    for (int i = 0; i < rounds; ++i) {
        for (Event ev; h.poll(ev);) {
        }
        h.flush();
        std::this_thread::sleep_for(std::chrono::milliseconds(1));
    }
}

}  // namespace

TEST_CASE("fuzz: a WebSocket server survives damaged handshakes and frames from a raw client") {
    log::set_level(log::Level::Off);
    WebSocketDesc d;
    d.bind_address = "127.0.0.1";
    d.max_peers = 8;
    d.max_message = 4096;
    d.timeout_ms = 200;
    WebSocketHost server;
    REQUIRE(server.listen(d));
    const u16 port = server.bound_port();

    const std::string good = std::string(kRequest) + frames_for(true);
    const usize head = std::strlen(kRequest);
    std::vector<std::uint64_t> storage;
    fuzz::Mutator m(0x5EBC0DE);
    u32 reached = 0;
    const u32 n = iterations();
    for (u32 it = 0; it < n; ++it) {
        INFO("replay: websocket server iteration " << it);
        // Half the mutants keep the handshake intact so the frame reader is what is tested.
        const bool frames_only = it % 2 == 0;
        std::string bytes;
        if (frames_only) {
            const std::string tail = good.substr(head);
            const usize len = m.mutate(Span<const byte>(reinterpret_cast<const byte*>(tail.data()), tail.size()), storage);
            bytes = std::string(kRequest) + std::string(reinterpret_cast<const char*>(storage.data()), len);
        } else {
            const usize len = m.mutate(Span<const byte>(reinterpret_cast<const byte*>(good.data()), good.size()), storage);
            bytes.assign(reinterpret_cast<const char*>(storage.data()), len);
        }
        const int s = connect_to(port);
        if (s < 0) {
            pump(server, 5);
            continue;
        }
        ++reached;
        send_all(s, bytes.data(), bytes.size());
        pump(server, 4);
        ::close(s);
        pump(server, 3);
    }
    pump(server, 20);
    std::printf("fuzz: websocket server: %u of %u connections made\n", reached, n);
    CHECK(reached > n / 2);
}

TEST_CASE("fuzz: a WebSocket client survives a damaged handshake answer and frames from a raw server") {
    log::set_level(log::Level::Off);
    const int ls = ::socket(AF_INET, SOCK_STREAM, 0);
    int one = 1;
    ::setsockopt(ls, SOL_SOCKET, SO_REUSEADDR, &one, sizeof(one));
    sockaddr_in a{};
    a.sin_family = AF_INET;
    a.sin_port = 0;
    ::inet_pton(AF_INET, "127.0.0.1", &a.sin_addr);
    REQUIRE(::bind(ls, reinterpret_cast<sockaddr*>(&a), sizeof(a)) == 0);
    REQUIRE(::listen(ls, 8) == 0);
    socklen_t al = sizeof(a);
    ::getsockname(ls, reinterpret_cast<sockaddr*>(&a), &al);
    const u16 port = ntohs(a.sin_port);
    ::fcntl(ls, F_SETFL, ::fcntl(ls, F_GETFL) | O_NONBLOCK);

    WebSocketDesc cd;
    cd.max_message = 4096;
    cd.timeout_ms = 200;
    WebSocketHost client;
    REQUIRE(client.open_client(cd));

    // The raw server reads the client's request and answers with the right accept key (websocket_accept_key is public),
    // so half of the mutants reach the frame reader behind the handshake.
    std::vector<std::uint64_t> storage;
    fuzz::Mutator m(0xC11E27);
    const u32 n = iterations();
    u32 served = 0;
    for (u32 it = 0; it < n; ++it) {
        INFO("replay: websocket client iteration " << it);
        const PeerId peer = client.connect("127.0.0.1", port, 0, "/");
        if (peer == kInvalidPeer) {
            pump(client, 5);
            continue;
        }
        int s = -1;
        for (int tries = 0; tries < 200 && s < 0; ++tries) {
            s = ::accept(ls, nullptr, nullptr);
            if (s < 0) pump(client, 1);
        }
        if (s < 0) {
            pump(client, 5);
            continue;
        }
        ++served;
        // The request: read until the blank line (the client flushes it from poll()).
        std::string request;
        ::fcntl(s, F_SETFL, ::fcntl(s, F_GETFL) | O_NONBLOCK);
        for (int tries = 0; tries < 200 && request.find("\r\n\r\n") == std::string::npos; ++tries) {
            char buf[1024];
            const ssize_t r = ::recv(s, buf, sizeof(buf), 0);
            if (r > 0) request.append(buf, static_cast<size_t>(r));
            else pump(client, 1);
        }
        std::string key;
        if (const size_t k = request.find("Sec-WebSocket-Key: "); k != std::string::npos) {
            const size_t e = request.find("\r\n", k);
            key = request.substr(k + 19, e - (k + 19));
        }
        const FixedString<32> accept = websocket_accept_key(StringView(key.c_str(), key.size()));
        const std::string real_answer = std::string("HTTP/1.1 101 Switching Protocols\r\nUpgrade: websocket\r\nConnection: Upgrade\r\nSec-WebSocket-Accept: ") +
                                        accept.c_str() + "\r\n\r\n";
        std::string bytes;
        if (it % 2 == 0) {  // a sound handshake, damaged frames
            const std::string tail = frames_for(false);
            const usize len = m.mutate(Span<const byte>(reinterpret_cast<const byte*>(tail.data()), tail.size()), storage);
            bytes = real_answer + std::string(reinterpret_cast<const char*>(storage.data()), len);
        } else {  // everything damaged
            const std::string whole = real_answer + frames_for(false);
            const usize len = m.mutate(Span<const byte>(reinterpret_cast<const byte*>(whole.data()), whole.size()), storage);
            bytes.assign(reinterpret_cast<const char*>(storage.data()), len);
        }
        send_all(s, bytes.data(), bytes.size());
        pump(client, 4);
        ::close(s);
        pump(client, 3);
    }
    pump(client, 20);
    ::close(ls);
    std::printf("fuzz: websocket client: %u of %u connections served\n", served, n);
    CHECK(served > n / 2);
}

#else

TEST_CASE("fuzz: websocket (POSIX sockets only)") {}

#endif
