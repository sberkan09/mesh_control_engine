// M-NET2 step 6: rooms with codes through a relay, over SimNetwork.
// M-NET3 step 3: quick play, the room list and host migration.

#include <doctest/doctest.h>

#include <cstring>

#include "mce/net/net.h"

using namespace mce;
using namespace mce::net;

namespace {

struct Player {
    SimEndpoint* link = nullptr;
    RelayClient room;
    Array<PeerId> connected;
    Array<PeerId> left;
    Array<u8> last;
    PeerId last_from = kInvalidPeer;
    u8 last_channel = 0;
    u32 received = 0;
    Array<i32> log;  // Connected: peer, Disconnected: -1 - peer, host change: 100 + new host
};

void on_host_change(void* user, PeerId host) {
    static_cast<Player*>(user)->log.push_back(100 + static_cast<i32>(host));
}

// A relay on one endpoint and players that each reach it over their own link, 20 ms each way.
struct Lobby {
    SimNetwork network{7};
    SimEndpoint* relay_end = nullptr;
    RelayServer relay;
    Player players[6];
    u32 count = 0;

    explicit Lobby(u32 n, RelayDesc desc = {}) : count(n) {
        LinkConditions c;
        c.latency_ms = 20;
        network.set_default_conditions(c);
        relay_end = &network.add_endpoint(32, 2);
        relay.init(desc);
        relay.add_transport(*relay_end);
        for (u32 i = 0; i < n; ++i) {
            players[i].link = &network.add_endpoint(1, 2);
            RelayClientDesc d;
            d.on_host_change = on_host_change;
            d.user = &players[i];
            players[i].room.start(*players[i].link, players[i].link->connect(*relay_end), d);
        }
    }
    void step(u32 ticks = 1) {
        for (u32 t = 0; t < ticks; ++t) {
            relay.update();
            for (u32 i = 0; i < count; ++i) {
                Player& p = players[i];
                for (Event ev; p.room.poll(ev);) {
                    if (ev.kind == EventKind::Connected) p.connected.push_back(ev.peer);
                    if (ev.kind == EventKind::Disconnected) p.left.push_back(ev.peer);
                    if (ev.kind == EventKind::Connected) p.log.push_back(static_cast<i32>(ev.peer));
                    if (ev.kind == EventKind::Disconnected) p.log.push_back(-1 - static_cast<i32>(ev.peer));
                    if (ev.kind == EventKind::Received) {
                        ++p.received;
                        p.last_from = ev.peer;
                        p.last_channel = ev.channel;
                        p.last.resize(ev.data.size());
                        if (!ev.data.empty()) std::memcpy(p.last.data(), ev.data.data(), ev.data.size());
                    }
                }
            }
            network.advance(10);
        }
    }
    // Player 0 hosts a room for `max` and players 1..joiners join it.
    void meet(u32 max, u32 joiners, const RelayRoomOptions& options = {}) {
        REQUIRE(players[0].room.create_room(max, options));
        step(10);
        REQUIRE(players[0].room.state() == RelayClient::State::InRoom);
        for (u32 i = 1; i <= joiners; ++i) REQUIRE(players[i].room.join_room(players[0].room.room_code()));
        step(10);
    }
};

Span<const u8> text(const char* s) { return Span<const u8>(reinterpret_cast<const u8*>(s), std::strlen(s)); }

bool got(const Player& p, const char* s) {
    return p.last.size() == std::strlen(s) && std::memcmp(p.last.data(), s, p.last.size()) == 0;
}

}  // namespace

TEST_CASE("net relay: a host makes a room, others join with its code, and members are peers") {
    Lobby lobby(3);
    // Requests made before the link is up wait for it.
    lobby.meet(4, 2);
    Player& host = lobby.players[0];
    Player& a = lobby.players[1];
    Player& b = lobby.players[2];
    CHECK(host.room.is_host());
    CHECK(host.room.member_id() == 0);
    CHECK(host.room.room_code().size() == kRelayCodeLength);
    for (char ch : host.room.room_code()) CHECK(std::strchr(kRelayCodeAlphabet, ch) != nullptr);
    CHECK(a.room.member_id() == 1);
    CHECK(b.room.member_id() == 2);
    CHECK(a.room.room_code() == host.room.room_code());
    // Everybody sees everybody else connect.
    CHECK(host.connected.size() == 2);
    CHECK(a.connected.size() == 2);  // the host, then b
    CHECK(b.connected.size() == 2);  // the host and a, already there
    CHECK(host.room.connected_count() == 2);
    u32 members = 0;
    REQUIRE(lobby.relay.room_members(host.room.room_code(), members));
    CHECK(members == 3);

    // To one member, and to everybody.
    REQUIRE(a.room.send(0, 1, text("to the host"), Delivery::Reliable));
    lobby.step(5);
    CHECK(got(host, "to the host"));
    CHECK(host.last_from == 1);
    CHECK(host.last_channel == 1);
    CHECK(b.received == 0);
    host.room.broadcast(0, text("everyone"), Delivery::Sequenced);
    lobby.step(5);
    CHECK(got(a, "everyone"));
    CHECK(got(b, "everyone"));
    CHECK(a.last_from == 0);
    CHECK(host.room.counters().messages_sent == 2);  // one per member reached
    CHECK(lobby.relay.stats().forwarded == 3);
    CHECK_FALSE(a.room.send(5, 0, text("nobody"), Delivery::Reliable));  // not a member
    CHECK_FALSE(a.room.send(0, 2, text("bad channel"), Delivery::Reliable));

    // A member leaving is a Disconnected for the others; its id is free for the next to join.
    b.room.leave();
    lobby.step(5);
    REQUIRE(host.left.size() == 1);
    CHECK(host.left[0] == 2);
    CHECK(a.left.size() == 1);
    CHECK(b.room.state() == RelayClient::State::Idle);
    REQUIRE(b.room.join_room(host.room.room_code()));
    lobby.step(5);
    CHECK(b.room.member_id() == 2);

    // The host leaving closes the room for everybody.
    host.room.leave();
    lobby.step(5);
    CHECK(a.room.state() == RelayClient::State::Closed);
    CHECK(b.room.state() == RelayClient::State::Closed);
    CHECK_FALSE(a.room.is_connected(0));
    CHECK(lobby.relay.stats().rooms == 0);
    CHECK_FALSE(lobby.relay.room_members(host.room.room_code(), members));
}

TEST_CASE("net relay: codes are case-insensitive, and wrong codes, full rooms and double joins are refused") {
    Lobby lobby(4);
    lobby.meet(2, 0);
    Player& host = lobby.players[0];
    FixedString<8> lower_code;
    for (char ch : host.room.room_code()) {
        const char l[2] = {ch >= 'A' && ch <= 'Z' ? static_cast<char>(ch - 'A' + 'a') : ch, 0};
        lower_code.append(l);
    }
    REQUIRE(lobby.players[1].room.join_room(lower_code.view()));
    lobby.step(5);
    CHECK(lobby.players[1].room.state() == RelayClient::State::InRoom);

    REQUIRE(lobby.players[2].room.join_room(host.room.room_code()));  // a room for 2 is full
    lobby.step(5);
    CHECK(lobby.players[2].room.state() == RelayClient::State::Refused);
    CHECK(lobby.players[2].room.refusal() == RelayRefusal::RoomFull);

    // A code no room has (one character changed), and text that is no code at all.
    FixedString<8> wrong(host.room.room_code());
    char other[7];
    std::memcpy(other, host.room.room_code().data(), 6);
    other[0] = other[0] == 'A' ? 'B' : 'A';
    other[6] = 0;
    wrong.assign(StringView(other, 6));
    REQUIRE(lobby.players[3].room.join_room(wrong.view()));
    lobby.step(5);
    CHECK(lobby.players[3].room.refusal() == RelayRefusal::NoSuchRoom);
    CHECK_FALSE(lobby.players[3].room.join_room("0OI1"));  // too short, and letters a code never has
    CHECK(lobby.players[3].room.refusal() == RelayRefusal::NoSuchRoom);

    // Already in a room: the client says no without asking.
    CHECK_FALSE(host.room.create_room(3));
    CHECK_FALSE(lobby.players[1].room.join_room(host.room.room_code()));
    CHECK(lobby.relay.stats().refusals == 2);
}

TEST_CASE("net relay: the host can remove a member, a lost link closes the room, and room limits hold") {
    RelayDesc desc;
    desc.max_rooms = 1;
    Lobby lobby(3, desc);
    lobby.meet(3, 2);
    Player& host = lobby.players[0];
    host.room.disconnect(1);
    lobby.step(5);
    CHECK(lobby.players[1].room.state() == RelayClient::State::Closed);
    CHECK(lobby.players[2].left.size() == 1);
    CHECK(host.room.connected_count() == 1);

    // One room at most: player 1 cannot make a second.
    REQUIRE(lobby.players[1].room.create_room(2));
    lobby.step(5);
    CHECK(lobby.players[1].room.refusal() == RelayRefusal::TooManyRooms);

    // The host's link drops: the relay closes the room for the rest.
    host.link->disconnect(0);
    lobby.step(10);
    CHECK(host.room.state() == RelayClient::State::Closed);
    CHECK(lobby.players[2].room.state() == RelayClient::State::Closed);
    CHECK(lobby.relay.stats().rooms == 0);
    CHECK(lobby.relay.stats().clients == 2);
}

TEST_CASE("net relay: snapshot replication runs unchanged through a room") {
    Lobby lobby(2);
    lobby.meet(2, 1);
    World server_world, client_world;
    ReplicationServer server;
    ReplicationClient client;
    LocalTransform t;
    t.position = {1.0f, 2.0f, 3.0f};
    const Entity e = server_world.spawn_with(t);
    const u32 id = server.assign(server_world, e, 5);
    for (u32 tick = 1; tick <= 20; ++tick) {
        server_world.try_get_mut<LocalTransform>(e)->position.x = static_cast<f32>(tick);
        ByteWriter w;
        server.write_snapshot(server_world, tick, w);
        lobby.players[0].room.broadcast(1, w.data(), Delivery::Sequenced);
        lobby.relay.update();
        for (Event ev; lobby.players[1].room.poll(ev);) {
            if (ev.kind != EventKind::Received) continue;
            client.apply(client_world.commands(), client_world, ev.data);
            client_world.sync(Phase::FixedUpdate_Pre);
        }
        for (Event ev; lobby.players[0].room.poll(ev);) {
        }
        lobby.network.advance(16);
    }
    lobby.step(10);
    for (Event ev; lobby.players[1].room.poll(ev);) {
    }
    const Entity seen = client.find(id);
    REQUIRE(client_world.alive(seen));
    CHECK(client_world.try_get<LocalTransform>(seen)->position.x > 15.0f);
}

TEST_CASE("net relay: quick play fills the fullest open room of the mode, then opens another") {
    Lobby lobby(5);
    lobby.step(5);
    REQUIRE(lobby.players[0].room.quick_play("duel", 2));
    lobby.step(10);
    CHECK(lobby.players[0].room.is_host());
    REQUIRE(lobby.players[1].room.quick_play("duel", 2));  // the open duel
    lobby.step(10);
    CHECK(lobby.players[1].room.room_code() == lobby.players[0].room.room_code());
    CHECK(lobby.players[1].room.member_id() == 1);
    CHECK(lobby.players[1].room.host_id() == 0);
    REQUIRE(lobby.players[2].room.quick_play("duel", 2));  // full: a new room
    REQUIRE(lobby.players[3].room.quick_play("brawl", 4));  // another mode
    lobby.step(10);
    CHECK(lobby.players[2].room.is_host());
    CHECK(lobby.players[2].room.room_code() != lobby.players[0].room.room_code());
    CHECK(lobby.players[3].room.is_host());
    CHECK(lobby.relay.stats().quick_plays == 4);

    // A private room never shows up, and is never quick-played into.
    Lobby other(2);
    other.meet(4, 0);
    REQUIRE(other.players[1].room.quick_play("", 4));
    other.step(10);
    CHECK(other.players[1].room.is_host());

    // The list: listed rooms only, by mode or all.
    Player& looker = lobby.players[4];
    REQUIRE(looker.room.list_rooms("duel"));
    lobby.step(10);
    REQUIRE(looker.room.rooms_received() == 1);
    REQUIRE(looker.room.rooms().size() == 2);
    u32 members = 0;
    for (const RelayRoomInfo& r : looker.room.rooms()) {
        CHECK(r.mode.view() == "duel");
        CHECK(r.max_members == 2);
        members += r.members;
    }
    CHECK(members == 3);
    REQUIRE(looker.room.list_rooms());
    lobby.step(10);
    CHECK(looker.room.rooms().size() == 3);
    CHECK(looker.room.state() == RelayClient::State::Idle);  // listing joins nothing
    CHECK_FALSE(looker.room.list_rooms("a mode name far too long"));
    CHECK_FALSE(looker.room.quick_play("bad\x01mode", 2));
}

TEST_CASE("net relay: with host migration the lowest member takes over and the room goes on") {
    Lobby lobby(5);
    RelayRoomOptions options;
    options.host_migration = true;
    lobby.meet(4, 3, options);
    for (u32 i = 0; i < 4; ++i) lobby.players[i].log.clear();
    lobby.players[0].room.leave();
    lobby.step(10);
    CHECK(lobby.relay.stats().host_migrations == 1);
    for (u32 i = 1; i < 4; ++i) {
        const Player& p = lobby.players[i];
        CHECK(p.room.state() == RelayClient::State::InRoom);
        CHECK(p.room.host_id() == 1);
        // The new host is announced before the old one's Disconnected.
        REQUIRE(p.log.size() == 2);
        CHECK(p.log[0] == 101);
        CHECK(p.log[1] == -1);
    }
    CHECK(lobby.players[1].room.is_host());
    CHECK_FALSE(lobby.players[2].room.is_host());

    // Messages flow to the new host, and it can remove a member.
    lobby.players[2].room.send(1, 0, text("still here"), Delivery::Reliable);
    lobby.step(5);
    CHECK(got(lobby.players[1], "still here"));
    lobby.players[1].room.disconnect(3);
    lobby.step(5);
    CHECK(lobby.players[3].room.state() == RelayClient::State::Closed);
    lobby.players[2].room.disconnect(3);  // not the host: nothing to remove

    // A newcomer takes the lowest free slot, the old host's.
    REQUIRE(lobby.players[4].room.join_room(lobby.players[1].room.room_code()));
    lobby.step(10);
    CHECK(lobby.players[4].room.member_id() == 0);
    CHECK(lobby.players[4].room.host_id() == 1);
    CHECK_FALSE(lobby.players[4].room.is_host());

    // Without migration the room closes as before.
    Lobby plain(2);
    plain.meet(2, 1);
    plain.players[0].room.leave();
    plain.step(10);
    CHECK(plain.players[1].room.state() == RelayClient::State::Closed);
}

TEST_CASE("relay: relay_refusal_name says why a request was refused") {
    CHECK(relay_refusal_name(RelayRefusal::None) == StringView("none"));
    CHECK(relay_refusal_name(RelayRefusal::NoSuchRoom) == StringView("no such room"));
    CHECK(relay_refusal_name(RelayRefusal::RoomFull) == StringView("room full"));
    CHECK(relay_refusal_name(RelayRefusal::AlreadyInRoom) == StringView("already in a room"));
    CHECK(relay_refusal_name(RelayRefusal::BadRequest) == StringView("bad request"));
    CHECK(relay_refusal_name(RelayRefusal::TooManyRooms) == StringView("too many rooms"));
    CHECK(relay_refusal_name(static_cast<RelayRefusal>(200)) == StringView("unknown"));
}
