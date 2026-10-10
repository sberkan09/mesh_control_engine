#pragma once
// mce::net::Host — one UDP endpoint with connected peers (ADR-0035). The same class is the
// server (listen() on a port, many peers) and the client (open_client() + connect(), one peer).
// Transport is ENet: per-channel reliable ordered, unreliable sequenced and unreliable unordered
// delivery, fragmentation, RTT and timeouts. No ENet type crosses this header.
//
// Threading: single-threaded. Call poll() from the game loop (a FixedUpdate_Pre system, or the
// host loop between ticks); nothing runs in the background.
//
// Determinism (FR-RUN-040): the network is input, not simulation. Received bytes enter the World
// the way input does — applied by a system at a fixed point in the tick — and the server's
// simulation never waits on a socket.

#include "mce/containers/array.h"
#include "mce/fixed_string.h"
#include "mce/types.h"

namespace mce::net {

// Index of a peer slot on its host. Stable from Connected to Disconnected; a slot is reused by a
// later connection, which reports Connected again.
using PeerId = u32;
inline constexpr PeerId kInvalidPeer = ~PeerId(0);

enum class Delivery : u8 {
    Reliable,    // resent until acknowledged, ordered within the channel (events, chat, RPCs)
    Sequenced,   // unreliable; a packet older than one already delivered is dropped (snapshots)
    Unreliable,  // unreliable and unordered (fire-and-forget)
};

struct HostDesc {
    // listen(): the address to bind ("" = every interface, "127.0.0.1" = loopback only).
    FixedString<64> bind_address;
    // listen(): the UDP port; 0 picks a free one (bound_port() reports it).
    u16 port = 0;
    // Connected peers at most (a client needs 1).
    u32 max_peers = 32;
    // Channels per connection (1..255). Channel 0 is conventionally reliable game messages.
    u8 channels = 2;
    // Bytes/second, 0 = unlimited (ENet throttles to these).
    u32 incoming_bandwidth = 0;
    u32 outgoing_bandwidth = 0;
    // A peer that stops answering is reported Disconnected after this long.
    u32 timeout_ms = 5000;
};

enum class EventKind : u8 { Connected, Disconnected, Received };

struct Event {
    EventKind kind = EventKind::Received;
    PeerId peer = kInvalidPeer;
    u8 channel = 0;
    // Received: the message, valid until the next poll() or close().
    Span<const u8> data;
    // Connected / Disconnected: the u32 the other side passed to connect() / disconnect().
    u32 user_data = 0;
};

struct PeerStats {
    u32 rtt_ms = 0;          // smoothed round-trip time
    u32 rtt_variance_ms = 0;
    u32 packet_loss = 0;     // ENet's estimate, in 1/65536ths
};

class Host {
public:
    Host();
    ~Host();
    Host(const Host&) = delete;
    Host& operator=(const Host&) = delete;

    // Server: binds desc.bind_address:desc.port. False (and logged) when the port is taken or the
    // address does not resolve.
    bool listen(const HostDesc& desc);
    // Client: an unbound endpoint for outgoing connections (max_peers = 1 unless given).
    bool open_client(const HostDesc& desc = client_desc());
    // Starts connecting to host:port (a name or a dotted address). The peer is reported Connected
    // by poll() once the handshake completes, or Disconnected if it does not. kInvalidPeer when
    // the name does not resolve or every peer slot is in use.
    PeerId connect(StringView host, u16 port, u32 user_data = 0);

    // Sends pending traffic and returns the next event, waiting up to timeout_ms for one.
    // False when there is none (or the host is closed). Typical loop:
    //     for (net::Event ev; host.poll(ev);) { ... }
    bool poll(Event& out, u32 timeout_ms = 0);

    // Queues a message; it goes out on the next poll() or flush(). False when the peer is not
    // connected, the channel is out of range, or the message cannot be allocated.
    bool send(PeerId peer, u8 channel, Span<const u8> data, Delivery delivery);
    // Queues a message to every connected peer.
    void broadcast(u8 channel, Span<const u8> data, Delivery delivery);
    // Sends queued messages now, without receiving.
    void flush();

    // Graceful: queued reliable messages go first, then the peer is told; poll() reports
    // Disconnected when it acknowledges (or times out).
    void disconnect(PeerId peer, u32 user_data = 0);
    // Immediate: the peer is told once, unreliably, and forgotten (no Disconnected event).
    void disconnect_now(PeerId peer, u32 user_data = 0);

    // Tells every peer and releases the socket. The destructor does this too.
    void close();

    bool is_open() const { return host_ != nullptr; }
    u16 bound_port() const { return port_; }
    bool is_connected(PeerId peer) const;
    u32 connected_count() const;
    // The connected peers, in slot order.
    void connected_peers(Array<PeerId>& out) const;
    PeerStats stats(PeerId peer) const;
    // UDP payload bytes since listen()/open_client(), every peer and protocol overhead included.
    u64 bytes_sent() const;
    u64 bytes_received() const;
    u8 channel_count() const { return channels_; }

    static HostDesc client_desc() {
        HostDesc d;
        d.max_peers = 1;
        return d;
    }

private:
    bool create(const HostDesc& desc, bool bind);
    void release_received();

    void* host_ = nullptr;      // the ENet host
    void* received_ = nullptr;  // the ENet packet backing the last Received event
    Array<u8> connected_;       // per peer slot: 1 while connected
    u16 port_ = 0;
    u8 channels_ = 0;
    u32 timeout_ms_ = 5000;
};

}  // namespace mce::net
