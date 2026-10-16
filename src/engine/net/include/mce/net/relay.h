#pragma once
// Rooms through a relay (M-NET2 step 6). Players behind home routers cannot usually accept
// connections, and a browser cannot open one at all, so everybody connects out to a relay server
// instead, and the relay passes messages between the members of a room. The host makes a room and
// gets a short code ("K7TQ2M") to share; the others join with the code. Nothing needs a port
// forwarded, and a web build (WebSocketHost) can play with a native one (Host) in the same room.
//
//   RelayServer   the relay: rooms, codes and forwarding, over any number of Transports (the
//                 mce_relay tool listens on UDP and WebSocket at once)
//   RelayClient   a Transport whose peers are the other members of a room. The game code written
//                 against Transport (replication, prediction, rollback) runs over it unchanged:
//
//   net::Host link;  link.open_client();
//   net::RelayClient room;  room.start(link, link.connect("relay.example.com", 7777));
//   room.create_room(4);                 // the host; room.room_code() once state() is InRoom
//   room.join_room("K7TQ2M");            // everybody else
//   for (net::Event ev; room.poll(ev);)  // Connected / Received / Disconnected per member
//
// Member ids are PeerIds: the host starts as 0, joiners get the lowest free id. A host that leaves
// closes the room, unless the room was made with host migration. The relay sees every message but
// never reads one; it is not a game server and has no say over the game.
//
// Matchmaking and host migration (M-NET3 step 3):
//
//   room.quick_play("duel", 2);          // joins the fullest listed "duel" room for 2, or makes one
//   room.list_rooms("duel");             // rooms() fills in when the answer arrives (rooms_received())
//   room.create_room(4, {.listed = true, .host_migration = true, .mode = "arena"});
//
// With host migration, a host that leaves hands the room to the lowest member still in it. Every
// member's RelayClientDesc::on_host_change runs (inside poll(), before the old host's Disconnected),
// so the game can turn the new host's client into the server (ReplicationServer::adopt rebuilds it
// from the replication mirror) and point the others at it. Quick-play rooms are listed and migrate.
//
// Delivery over the relay is the link's: Reliable messages go on the link's channel 0, Sequenced
// and Unreliable on its channel 1 (so the link needs 2 channels). A message travels two hops, so
// its latency is the sum of both members' trips to the relay.

#include "mce/containers/hash_map.h"
#include "mce/net/transport.h"

namespace mce::net {

inline constexpr u8 kRelayVersion = 2;
inline constexpr u32 kRelayMaxMembers = 8;
inline constexpr u32 kRelayCodeLength = 6;
inline constexpr u32 kRelayModeLength = 16;  // a game mode name: printable ASCII, at most this long
inline constexpr u32 kRelayMaxListed = 32;   // rooms in one list_rooms answer, at most
// No 0/O or 1/I: codes are read aloud and typed on phones.
inline constexpr char kRelayCodeAlphabet[] = "ABCDEFGHJKLMNPQRSTUVWXYZ23456789";

enum class RelayRefusal : u8 {
    None = 0,
    NoSuchRoom = 1,
    RoomFull = 2,
    AlreadyInRoom = 3,
    BadRequest = 4,
    TooManyRooms = 5,
};
StringView relay_refusal_name(RelayRefusal r);

struct RelayDesc {
    u32 max_rooms = 256;
    u32 max_message = 64 * 1024;  // a larger DATA message is dropped
    u64 seed = 1;                 // room codes are drawn from it (repeatable in tests)
};

struct RelayStats {
    u32 clients = 0;  // connected now
    u32 rooms = 0;    // open now
    u64 rooms_created = 0;
    u64 joins = 0;
    u64 quick_plays = 0;      // of the creates and joins, the ones made by quick play
    u64 host_migrations = 0;  // hosts that left a room that went on
    u64 refusals = 0;
    u64 forwarded = 0;  // messages passed from one member to another (a broadcast counts each)
    u64 forwarded_bytes = 0;
    u64 dropped = 0;  // malformed, too large, or for nobody
};

class RelayServer {
public:
    void init(const RelayDesc& desc = {});
    // Serves the clients that connect over `transport` (which must outlive the server). Returns its
    // index. Any mix works: a Host on UDP for native players, a WebSocketHost for browsers.
    u32 add_transport(Transport& transport);
    // Polls every transport, handles requests, forwards messages and flushes. Call it in a loop.
    void update();

    const RelayStats& stats() const { return stats_; }
    // The room's members, or false when no room has this code.
    bool room_members(StringView code, u32& members) const;

private:
    struct Client {
        u32 transport = 0;
        PeerId peer = kInvalidPeer;
        u64 room = 0;  // packed code, 0 = none
        u8 member = 0;
    };
    struct Room {
        u64 code = 0;
        u8 max_members = 0;
        u8 host = 0;
        bool listed = false;
        bool migrate = false;
        FixedString<kRelayModeLength + 1> mode;
        u64 members[kRelayMaxMembers] = {};  // client keys, 0 = free
        u32 count() const;
    };

    void receive(u64 client_key, Span<const u8> data);
    void create(u64 client_key, Span<const u8> data);
    void join(u64 client_key, Span<const u8> data);
    void quick_play(u64 client_key, Span<const u8> data);
    void list(u64 client_key, Span<const u8> data);
    void open_room(u64 client_key, u8 max_members, bool listed, bool migrate, StringView mode);
    void enter(u64 client_key, Room& room);
    void leave(u64 client_key, bool notify_self);
    void forward(u64 client_key, Span<const u8> data);
    void refuse(u64 client_key, RelayRefusal reason);
    void send_to(u64 client_key, u8 channel, Span<const u8> data, Delivery delivery);
    u64 new_code();

    RelayDesc desc_;
    Array<Transport*> transports_;
    HashMap<u64, Client> clients_;
    HashMap<u64, Room> rooms_;
    u64 rng_ = 1;
    RelayStats stats_;
    Array<u8> scratch_;
};

struct RelayClientDesc {
    u8 channels = 2;  // the game's channels (1..255), carried inside the relay's messages
    // Host migration: the room's new host, from inside poll() (and before the old host's
    // Disconnected). new_host == member_id(): this client is the host now.
    void (*on_host_change)(void* user, PeerId new_host) = nullptr;
    void* user = nullptr;
};

struct RelayRoomOptions {
    bool listed = false;          // shown by list_rooms and found by quick play
    bool host_migration = false;  // a leaving host hands the room on instead of closing it
    StringView mode;              // the game mode, for listing and quick play (kRelayModeLength at most)
};

struct RelayRoomInfo {
    FixedString<kRelayCodeLength + 1> code;
    FixedString<kRelayModeLength + 1> mode;
    u32 members = 0;
    u32 max_members = 0;
};

class RelayClient final : public Transport {
public:
    enum class State : u8 {
        Idle,        // start() not called, or left
        Requesting,  // a create or join is on its way
        InRoom,
        Refused,     // refusal() says why; create_room / join_room may be called again
        Closed,      // the host left, or the link to the relay went down
    };

    // `link` reaches the relay as peer `relay` (connected or still connecting) and must outlive
    // this client. From here on the client polls the link; game code polls the client instead.
    void start(Transport& link, PeerId relay, const RelayClientDesc& desc = {});
    // Requests a room for up to max_members (2..8) with this client as its host. Sent as soon as the
    // link is connected. False when already in a room or a request is on its way.
    bool create_room(u32 max_members = 4, const RelayRoomOptions& options = {});
    // Joins the fullest listed room of this mode and size that has space, or makes one (listed, with
    // host migration) with this client as its host. Same states as create_room / join_room.
    bool quick_play(StringView mode, u32 max_members);
    // Asks for the listed rooms (of one mode, or all when empty), in any state; rooms() holds the
    // answer once rooms_received() has gone up. False: the mode is too long.
    bool list_rooms(StringView mode = {});
    const Array<RelayRoomInfo>& rooms() const { return rooms_; }
    u32 rooms_received() const { return rooms_received_; }
    // Requests to join the room with this code (case-insensitive).
    bool join_room(StringView code);
    // Leaves the room (the host's leaving closes it). Disconnected is reported for every member.
    void leave();

    State state() const { return state_; }
    RelayRefusal refusal() const { return refusal_; }
    StringView room_code() const { return code_.view(); }
    PeerId member_id() const { return member_; }  // kInvalidPeer outside a room
    PeerId host_id() const { return state_ == State::InRoom ? host_ : kInvalidPeer; }
    bool is_host() const { return state_ == State::InRoom && member_ == host_; }

    bool poll(Event& out) override;
    // To one member; kInvalidPeer is not a member. False outside a room or for an absent member.
    bool send(PeerId peer, u8 channel, Span<const u8> data, Delivery delivery) override;
    // To every other member, in one message to the relay.
    void broadcast(u8 channel, Span<const u8> data, Delivery delivery) override;
    void flush() override;
    // The host removes a member from the room; anyone else disconnecting the host leaves.
    void disconnect(PeerId peer, u32 user_data = 0) override;

    bool is_connected(PeerId peer) const override;
    // The link's round trip to the relay (a member is two of those away).
    PeerStats stats(PeerId peer) const override;
    u8 channel_count() const override { return channels_; }

protected:
    u32 peer_slots() const override { return kRelayMaxMembers; }

private:
    struct Queued {
        Event event;
        Array<u8> bytes;
        bool host_change = false;  // not an event: on_host_change(event.peer) when dequeued
    };
    void handle(Span<const u8> data);
    void push(EventKind kind, PeerId peer, u8 channel, const u8* data, usize size);
    void member_left(PeerId peer);
    void close_room(State next);
    bool send_request();
    bool request_room(u8 op, u32 max_members, u8 flags, StringView mode);
    bool data(u8 to, u8 channel, Span<const u8> payload, Delivery delivery);

    Transport* link_ = nullptr;
    PeerId relay_ = kInvalidPeer;
    u8 channels_ = 2;
    State state_ = State::Idle;
    RelayRefusal refusal_ = RelayRefusal::None;
    FixedString<8> code_;
    PeerId member_ = kInvalidPeer;
    PeerId host_ = 0;
    RelayClientDesc desc_;
    Array<RelayRoomInfo> rooms_;
    u32 rooms_received_ = 0;
    Array<u8> list_request_;  // a list request waiting for the link
    bool present_[kRelayMaxMembers] = {};
    Array<u8> request_;  // a create or join waiting for the link
    bool request_sent_ = false;
    Array<Queued> inbox_;
    usize inbox_head_ = 0;
    Array<u8> current_;
    Array<u8> scratch_;
};

}  // namespace mce::net
