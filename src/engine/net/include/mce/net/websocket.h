#pragma once
// mce::net::WebSocketHost — a Transport over WebSocket (RFC 6455), M-NET2 step 6. Browsers cannot
// open UDP sockets, so a web build reaches a server or the relay (relay.h) over WebSocket instead
// of Host's ENet. Like Host, the same class is the server (listen(), many peers) and the client
// (open_client() + connect()). Native builds speak the protocol over plain TCP (no TLS: put a TLS
// proxy in front for wss://); a web build's client uses the browser's WebSocket (emscripten's
// websocket API), and listen() is not available there.
//
// Every message is one binary WebSocket message: [u8 channel][payload]. TCP delivers everything
// reliably and in order, so Sequenced and Unreliable are sent like Reliable (nothing is dropped,
// and a slow link queues instead). Pings measure the round trip for stats() and find dead peers.
//
// Threading: single-threaded; poll() does all the socket work, nothing runs in the background.

#include "mce/memory/allocator.h"
#include "mce/net/transport.h"

namespace mce::net {

struct WebSocketDesc {
    // listen(): the address to bind ("" = every interface, "127.0.0.1" = loopback only).
    FixedString<64> bind_address;
    // listen(): the TCP port; 0 picks a free one (bound_port() reports it).
    u16 port = 0;
    u32 max_peers = 32;
    u8 channels = 2;  // 1..255
    // A message larger than this closes the connection (1009, "too big").
    u32 max_message = 1u << 20;
    // A peer that does not finish its handshake, or answers no ping, is dropped after this long.
    u32 timeout_ms = 5000;
    u32 ping_interval_ms = 1000;
};

class WebSocketHost final : public Transport {
public:
    WebSocketHost();
    ~WebSocketHost() override;

    // Server: binds desc.bind_address:desc.port. False (and logged) when the port is taken, or in a
    // web build.
    bool listen(const WebSocketDesc& desc);
    // Client: an endpoint for outgoing connections (max_peers = 1 unless given).
    bool open_client(const WebSocketDesc& desc = client_desc());
    // Starts connecting to ws://host:port/path. Reported Connected by poll() once the handshake
    // completes, Disconnected if it fails. kInvalidPeer when every slot is in use or host does not
    // resolve. user_data reaches the server's Connected event (as in Host::connect).
    PeerId connect(StringView host, u16 port, u32 user_data = 0, StringView path = "/");

    bool poll(Event& out) override;
    bool send(PeerId peer, u8 channel, Span<const u8> data, Delivery delivery) override;
    void broadcast(u8 channel, Span<const u8> data, Delivery delivery) override;
    void flush() override;
    // Sends a close frame carrying user_data and reports Disconnected here at once; the other side
    // reports it when the frame arrives.
    void disconnect(PeerId peer, u32 user_data = 0) override;
    // Closes every connection and the listening socket. The destructor does this too.
    void close();

    bool is_open() const { return open_; }
    u16 bound_port() const { return port_; }
    bool is_connected(PeerId peer) const override;
    PeerStats stats(PeerId peer) const override;
    u8 channel_count() const override { return channels_; }
    // Bytes queued for a peer that the socket has not taken yet.
    usize pending_bytes(PeerId peer) const;

    static WebSocketDesc client_desc() {
        WebSocketDesc d;
        d.max_peers = 1;
        return d;
    }

protected:
    u32 peer_slots() const override;

private:
    struct Impl;
    mem::UniquePtr<Impl> impl_;
    bool open_ = false;
    u16 port_ = 0;
    u8 channels_ = 0;
};

// The Sec-WebSocket-Accept value for a Sec-WebSocket-Key (RFC 6455 §4.2.2): base64(SHA-1(key +
// the protocol GUID)). 28 characters.
FixedString<32> websocket_accept_key(StringView key);

}  // namespace mce::net
