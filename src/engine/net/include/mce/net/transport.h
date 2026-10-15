#pragma once
// mce::net::Transport — what game code sends and receives through (M-NET2 step 1). Two
// implementations: Host (real UDP over ENet, host.h) and SimEndpoint (an in-memory network on a
// virtual clock with simulated latency, jitter and loss, sim_network.h). Code written against
// Transport runs unchanged over either, so a multiplayer loop is tested headless and
// deterministically with SimNetwork and shipped over Host.
//
// LinkConditions describe a bad network: SimNetwork applies them to every message on a link, and
// Host::set_conditions applies them to its own outgoing messages (to play a build "at 150 ms").

#include "mce/containers/array.h"
#include "mce/fixed_string.h"
#include "mce/types.h"

namespace mce::net {

// Index of a peer slot on its endpoint. Stable from Connected to Disconnected; a slot is reused by
// a later connection, which reports Connected again.
using PeerId = u32;
inline constexpr PeerId kInvalidPeer = ~PeerId(0);

enum class Delivery : u8 {
    Reliable,    // resent until acknowledged, ordered within the channel (events, chat, RPCs)
    Sequenced,   // unreliable; a packet older than one already delivered is dropped (snapshots)
    Unreliable,  // unreliable and unordered (fire-and-forget)
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
    u32 rtt_ms = 0;  // smoothed round-trip time
    u32 rtt_variance_ms = 0;
    u32 packet_loss = 0;  // estimated loss, in 1/65536ths
};

// One direction of a link. A message is delayed by latency_ms plus a uniform 0..jitter_ms, and
// lost with probability `loss`. Reliable messages are never lost: a lost send is "resent" after a
// round trip, so it arrives late instead. Reliable and Sequenced messages keep their order within a
// channel (jitter cannot overtake them); Unreliable ones may arrive out of order.
struct LinkConditions {
    u32 latency_ms = 0;
    u32 jitter_ms = 0;
    f32 loss = 0.0f;  // 0..1
    bool any() const { return latency_ms != 0 || jitter_ms != 0 || loss > 0.0f; }
};

// Counters an endpoint keeps (messages, not datagrams: protocol overhead is not counted here;
// Host::bytes_sent() has the UDP totals).
struct TransportCounters {
    u64 messages_sent = 0;
    u64 messages_received = 0;
    u64 messages_dropped = 0;  // lost to LinkConditions, or a stale Sequenced message
    u64 payload_bytes_sent = 0;
    u64 payload_bytes_received = 0;
};

class Transport {
public:
    Transport() = default;
    Transport(const Transport&) = delete;
    Transport& operator=(const Transport&) = delete;
    virtual ~Transport() = default;

    // The next event, if one is ready. Typical loop: for (net::Event ev; t.poll(ev);) { ... }
    virtual bool poll(Event& out) = 0;
    // Queues a message. False when the peer is not connected or the channel is out of range.
    virtual bool send(PeerId peer, u8 channel, Span<const u8> data, Delivery delivery) = 0;
    // Queues a message to every connected peer.
    virtual void broadcast(u8 channel, Span<const u8> data, Delivery delivery) = 0;
    // Sends queued messages now.
    virtual void flush() = 0;
    // Graceful: the peer is told; poll() reports Disconnected on both sides.
    virtual void disconnect(PeerId peer, u32 user_data = 0) = 0;

    virtual bool is_connected(PeerId peer) const = 0;
    virtual PeerStats stats(PeerId peer) const = 0;
    virtual u8 channel_count() const = 0;

    const TransportCounters& counters() const { return counters_; }
    u32 connected_count() const;
    // The connected peers, in slot order.
    void connected_peers(Array<PeerId>& out) const;

protected:
    // Slots connected_count / connected_peers walk.
    virtual u32 peer_slots() const = 0;
    TransportCounters counters_;
};

}  // namespace mce::net
