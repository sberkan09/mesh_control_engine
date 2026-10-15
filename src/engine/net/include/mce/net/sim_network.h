#pragma once
// mce::net::SimNetwork — an in-memory network on a virtual clock (M-NET2 step 1). Its endpoints
// are Transports (transport.h), so the same game code that runs over Host runs here, and every
// message is delayed, jittered and lost by LinkConditions with a seeded generator. Nothing touches
// a socket or the wall clock: the same calls with the same seed give the same deliveries, so a
// server and several clients in one process give the same state hashes on every run and in every
// build (the headless multi-client runs in local_ci.sh).
//
//   net::SimNetwork network(seed);
//   network.set_default_conditions({.latency_ms = 75, .jitter_ms = 20, .loss = 0.05f});
//   net::SimEndpoint& server = network.add_endpoint(8, 3);
//   net::SimEndpoint& client = network.add_endpoint(1, 3);
//   const net::PeerId to_server = client.connect(server);
//   for each tick: poll and send on each endpoint, tick the worlds, then network.advance(16);
//
// Semantics follow Host: connect() completes after a round trip (Connected on both sides), the
// channel's Delivery decides loss and order (transport.h LinkConditions), a Sequenced message
// older than one already delivered on its channel is dropped. disconnect() is reported Disconnected
// locally at once and on the other side when the notice arrives, after the peer's queued messages.

#include "mce/containers/hash_map.h"
#include "mce/memory/allocator.h"
#include "mce/net/transport.h"

namespace mce::net {

class SimNetwork;

class SimEndpoint final : public Transport {
public:
    // Starts connecting to `server`. kInvalidPeer when every local peer slot is in use.
    PeerId connect(SimEndpoint& server, u32 user_data = 0);

    bool poll(Event& out) override;
    bool send(PeerId peer, u8 channel, Span<const u8> data, Delivery delivery) override;
    void broadcast(u8 channel, Span<const u8> data, Delivery delivery) override;
    void flush() override {}  // sends go into the network at once; SimNetwork::advance delivers
    void disconnect(PeerId peer, u32 user_data = 0) override;

    bool is_connected(PeerId peer) const override;
    PeerStats stats(PeerId peer) const override;
    u8 channel_count() const override { return channels_; }
    u32 index() const { return index_; }

    // Internal: SimNetwork builds endpoints (add_endpoint).
    SimEndpoint(SimNetwork& network, u32 index, u32 max_peers, u8 channels);

protected:
    u32 peer_slots() const override { return static_cast<u32>(peers_.size()); }

private:
    friend class SimNetwork;
    enum class PeerState : u8 { Free, Connecting, Connected };
    struct Peer {
        PeerState state = PeerState::Free;
        u32 remote_endpoint = 0;
        PeerId remote_peer = kInvalidPeer;
        Array<u32> send_seq;  // per channel: the last Sequenced number sent
        Array<u32> recv_seq;  // per channel: the newest Sequenced number delivered
        Array<u64> last_due;  // per channel: ordered deliveries never overtake this
    };
    struct Queued {
        Event event;
        Array<u8> bytes;
    };

    PeerId free_slot() const;
    void reset_peer(PeerId peer);
    void push_event(EventKind kind, PeerId peer, u8 channel, u32 user_data, Array<u8>&& bytes);

    SimNetwork* network_;
    u32 index_;
    u8 channels_;
    Array<Peer> peers_;
    Array<Queued> inbox_;
    usize inbox_head_ = 0;
    Array<u8> current_;  // backs the last Received event
};

class SimNetwork {
public:
    explicit SimNetwork(u64 seed = 1);
    ~SimNetwork();
    SimNetwork(const SimNetwork&) = delete;
    SimNetwork& operator=(const SimNetwork&) = delete;

    // A new endpoint with up to `max_peers` connections of `channels` channels each. It lives as
    // long as the network.
    SimEndpoint& add_endpoint(u32 max_peers = 32, u8 channels = 2);
    SimEndpoint& endpoint(u32 index) { return *endpoints_[index]; }
    u32 endpoint_count() const { return static_cast<u32>(endpoints_.size()); }

    // Conditions for every link without its own.
    void set_default_conditions(const LinkConditions& c) { default_ = c; }
    // Conditions for messages from `from` to `to` (one direction; call twice for both).
    void set_conditions(const SimEndpoint& from, const SimEndpoint& to, const LinkConditions& c);
    const LinkConditions& conditions(u32 from, u32 to) const;

    // Moves the clock forward and hands every message due by then to its endpoint, in arrival
    // order (ties in send order). Endpoints' poll() then returns them.
    void advance(u32 ms);
    u64 now_ms() const { return now_ms_; }
    // Messages on the way.
    usize in_flight() const { return flight_.size(); }

private:
    friend class SimEndpoint;
    enum class Kind : u8 { Connect, Accept, Refuse, Disconnect, Data };
    struct Message {
        u64 due_ms = 0;
        u64 order = 0;
        Kind kind = Kind::Data;
        u32 from_endpoint = 0;
        PeerId from_peer = kInvalidPeer;
        u32 to_endpoint = 0;
        PeerId to_peer = kInvalidPeer;
        u8 channel = 0;
        Delivery delivery = Delivery::Reliable;
        u32 seq = 0;
        u32 user_data = 0;
        Array<u8> bytes;
    };

    // Rolls the conditions and queues `m`. `ordered_after`: the sender peer's per-channel floor
    // (null for unordered), raised to this message's arrival. False when the message was lost.
    bool post(Message&& m, u64* ordered_after);
    void deliver(Message& m);

    Array<mem::UniquePtr<SimEndpoint>> endpoints_;
    HashMap<u64, LinkConditions> links_;
    LinkConditions default_;
    Array<Message> flight_;
    u64 rng_;
    u64 now_ms_ = 0;
    u64 next_order_ = 0;
};

}  // namespace mce::net
