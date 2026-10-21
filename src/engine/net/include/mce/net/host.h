#pragma once
// mce::net::Host — one UDP endpoint with connected peers (ADR-0035). The same class is the
// server (listen() on a port, many peers) and the client (open_client() + connect(), one peer).
// Transport is ENet: per-channel reliable ordered, unreliable sequenced and unreliable unordered
// delivery, fragmentation, RTT and timeouts. No ENet type crosses this header.
//
// Threading: single-threaded. Call poll() from the game loop (a FixedUpdate_Pre system, or the
// host loop between ticks); nothing runs in the background.
//
// Host implements Transport (transport.h): game code written against Transport also runs over
// SimNetwork in tests.
//
// Determinism (FR-RUN-040): the network is input, not simulation. Received bytes enter the World
// the way input does — applied by a system at a fixed point in the tick — and the server's
// simulation never waits on a socket.

#include "mce/net/transport.h"

namespace mce::net {

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

class Host final : public Transport {
public:
    Host();
    ~Host() override;
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
    bool poll(Event& out, u32 timeout_ms);
    bool poll(Event& out) override { return poll(out, 0); }

    // Queues a message; it goes out on the next poll() or flush(). False when the peer is not
    // connected, the channel is out of range, or the message cannot be allocated.
    bool send(PeerId peer, u8 channel, Span<const u8> data, Delivery delivery) override;
    // Queues a message to every connected peer.
    void broadcast(u8 channel, Span<const u8> data, Delivery delivery) override;
    // Sends queued messages now, without receiving (with conditions set: the ones that are due).
    void flush() override;

    // Graceful: queued reliable messages go first, then the peer is told; poll() reports
    // Disconnected when it acknowledges (or times out).
    void disconnect(PeerId peer, u32 user_data = 0) override;
    // Immediate: the peer is told once, unreliably, and forgotten (no Disconnected event).
    void disconnect_now(PeerId peer, u32 user_data = 0);

    // Tells every peer and releases the socket. The destructor does this too.
    void close();

    // Simulated bad network on this host's outgoing messages (M-NET2 step 1): each one waits
    // latency + jitter before it reaches ENet, and Sequenced / Unreliable ones are dropped at
    // `loss`. Set it on the server or a client (or both, for the round trip). Wall-clock based, for
    // playing a build; tests use SimNetwork. `seed` makes the loss and jitter rolls repeatable.
    void set_conditions(const LinkConditions& conditions, u64 seed = 1);
    const LinkConditions& conditions() const { return conditions_; }
    // Messages held back by the conditions, not yet handed to ENet.
    usize delayed_count() const { return delayed_.size(); }

    bool is_open() const { return host_ != nullptr; }
    u16 bound_port() const { return port_; }
    bool is_connected(PeerId peer) const override;
    PeerStats stats(PeerId peer) const override;
    // UDP payload bytes since listen()/open_client(), every peer and protocol overhead included.
    u64 bytes_sent() const;
    u64 bytes_received() const;
    u8 channel_count() const override { return channels_; }

    static HostDesc client_desc() {
        HostDesc d;
        d.max_peers = 1;
        return d;
    }

protected:
    u32 peer_slots() const override { return static_cast<u32>(connected_.size()); }

private:
    struct Delayed {
        u64 due_us = 0;
        u64 order = 0;
        PeerId peer = kInvalidPeer;
        u8 channel = 0;
        Delivery delivery = Delivery::Reliable;
        Array<u8> bytes;
    };

    bool create(const HostDesc& desc, bool bind);
    void release_received();
    bool send_now(PeerId peer, u8 channel, Span<const u8> data, Delivery delivery);
    void release_delayed();

    void* host_ = nullptr;      // the ENet host
    void* received_ = nullptr;  // the ENet packet backing the last Received event
    Array<u8> connected_;       // per peer slot: 1 while connected
    u16 port_ = 0;
    u8 channels_ = 0;
    u32 timeout_ms_ = 5000;
    LinkConditions conditions_;
    u64 rng_ = 1;
    u64 next_order_ = 0;
    Array<Delayed> delayed_;
    Array<u64> last_due_us_;  // per peer slot x channel: ordered deliveries never overtake
};

}  // namespace mce::net
