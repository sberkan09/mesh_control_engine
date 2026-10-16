#pragma once
// Direct connections between room members over WebRTC data channels (M-NET3 step 4). Through the
// relay every message pays two hops; RtcHost is a room (a RelayClient inside it) that also tries a
// WebRTC connection to every other member, signalled through the relay. A pair that connects stops
// paying the relay hop; a pair that cannot (strict NATs, no WebRTC in this build, a member that
// turned it off) stays on the relay. Game code sees one Transport either way:
//
//   net::Host link;  link.open_client();
//   net::RtcHost room;  room.start(link, link.connect("relay.example.com", 7777));
//   room.room().quick_play("arena", 4);        // rooms as with RelayClient
//   for (net::Event ev; room.poll(ev);) ...    // Connected / Received / Disconnected per member
//   room.path(peer);                           // Relay, Connecting or Direct
//
// Native builds use libdatachannel (ICE, DTLS, SCTP) where MCE_WITH_RTC is on (Linux with OpenSSL);
// a web build uses the browser's RTCPeerConnection, so a browser and a native player connect
// directly too. Elsewhere every pair stays on the relay.
//
// Each pair has two channels: Reliable messages go on an ordered, reliable one; Sequenced and
// Unreliable on an unordered one without retransmits (Sequenced messages carry a number, and one
// older than a message already delivered is dropped, whichever way each came). When a pair goes
// direct, the switch is marked on the relay, so Reliable messages keep their order across it. A
// direct connection that drops later falls back to the relay; Reliable messages still in flight on
// it are lost then (rare: the network changed under it).
//
// The lower member id offers. Without an ICE server only host candidates are gathered, which is
// enough on one machine or a LAN; across the internet give it a STUN server (RtcDesc::ice_server).

#include "mce/memory/allocator.h"
#include "mce/net/relay.h"
#include "mce/net/transport.h"

namespace mce::net {

class RtcLinks;
struct RtcLinkEvent;

enum class RtcPath : u8 {
    Relay,       // through the relay (not tried, refused, failed or timed out)
    Connecting,  // through the relay while a direct connection is being made
    Direct,      // over a WebRTC data channel
};
StringView rtc_path_name(RtcPath p);

struct RtcDesc {
    u8 channels = 2;               // the game's channels (1..255)
    bool direct = true;            // try WebRTC; false: this member stays on the relay with everyone
    StringView ice_server;         // "stun:stun.example.com:3478", or empty for host candidates only
    u32 connect_timeout_ms = 10000;  // a pair not direct by then stays on the relay
    // As RelayClientDesc: the room's new host, from inside poll().
    void (*on_host_change)(void* user, PeerId new_host) = nullptr;
    void* user = nullptr;
};

struct RtcStats {
    u32 direct_opened = 0;  // pairs that went direct
    u32 fallbacks = 0;      // pairs that tried and stayed on (or went back to) the relay
    u64 direct_messages_sent = 0;
    u64 relay_messages_sent = 0;  // a relay broadcast counts once per member
    u64 direct_messages_received = 0;
    u64 relay_messages_received = 0;
};

class RtcHost final : public Transport {
public:
    RtcHost();
    ~RtcHost() override;

    // Whether this build can make direct connections at all.
    static bool direct_supported();

    // As RelayClient::start: `link` reaches the relay as peer `relay` and must outlive this host.
    void start(Transport& link, PeerId relay, const RtcDesc& desc = {});
    // The room: create_room, join_room, quick_play, list_rooms, leave, state, member_id, is_host...
    // Poll and send through the RtcHost, not through it.
    RelayClient& room() { return room_; }
    const RelayClient& room() const { return room_; }

    RtcPath path(PeerId peer) const;
    const RtcStats& rtc_stats() const { return stats_; }

    bool poll(Event& out) override;
    bool send(PeerId peer, u8 channel, Span<const u8> data, Delivery delivery) override;
    void broadcast(u8 channel, Span<const u8> data, Delivery delivery) override;
    void flush() override;
    // As RelayClient: the host removes a member; anyone else disconnecting the host leaves.
    void disconnect(PeerId peer, u32 user_data = 0) override;

    bool is_connected(PeerId peer) const override { return room_.is_connected(peer); }
    // The relay's round trip (the direct one is not measured here).
    PeerStats stats(PeerId peer) const override { return room_.stats(peer); }
    u8 channel_count() const override { return channels_; }

protected:
    u32 peer_slots() const override { return kRelayMaxMembers; }

private:
    struct Queued {
        Event event;
        Array<u8> bytes;
    };
    struct Pair {
        RtcPath path = RtcPath::Relay;
        u32 link = 0;           // RtcLinks id, 0 = none
        u64 started_us = 0;
        bool sent_switch = false;  // our Reliable messages go direct
        bool got_switch = false;   // theirs come direct, in order after the relay's
        Array<Queued> held;        // their direct Reliable messages before their switch arrived
    };

    void handle_relay(const Event& ev);
    void handle_signal(PeerId peer, Span<const u8> body);
    void handle_link(RtcLinkEvent& ev);
    void deliver(PeerId peer, u8 channel, Span<const u8> payload, bool sequenced, u16 seq);
    void push(EventKind kind, PeerId peer, u8 channel, Span<const u8> data);
    void offer(PeerId peer);
    void drop_link(PeerId peer, bool fallback);
    void signal(PeerId peer, u8 type, Span<const u8> a, Span<const u8> b = {});
    u16 next_seq(u8 channel, Delivery delivery);
    void frame(u8 channel, Span<const u8> data, Delivery delivery, u16 seq, bool direct);
    bool send_direct(Pair& pair, Delivery delivery);
    void check_timeouts();
    PeerId peer_of(u32 link) const;
    RtcLinks& links();

    RelayClient room_;
    RtcDesc desc_;
    FixedString<256> ice_server_;
    u8 channels_ = 2;
    mem::UniquePtr<RtcLinks> links_;
    Pair pairs_[kRelayMaxMembers];
    Array<u16> send_seq_;  // per channel, for Sequenced messages
    Array<u16> recv_seq_;  // per member and channel: the newest delivered
    Array<u8> recv_have_;
    RtcStats stats_;
    Array<Queued> inbox_;
    usize inbox_head_ = 0;
    Array<u8> current_;
    Array<u8> scratch_;
};

}  // namespace mce::net
