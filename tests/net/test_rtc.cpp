// M-NET3 step 4: room members connect directly over WebRTC data channels, signalled through the
// relay; a pair that cannot stays on the relay. The relay link is SimNetwork; the direct links are
// real WebRTC over this machine's interfaces (loopback ICE), so these tests wait on the clock.
// Without WebRTC in the build (MCE_WITH_RTC off) only the relay-fallback cases run.

#include <doctest/doctest.h>

#include <chrono>
#include <cstring>
#include <thread>

#include "mce/net/net.h"

using namespace mce;
using namespace mce::net;

namespace {

struct Member {
    SimEndpoint* link = nullptr;
    RtcHost room;
    Array<Array<u8>> got[3];  // per channel, in arrival order
    u32 connected = 0;
    u32 disconnected = 0;
};

struct Lobby {
    SimNetwork network{11};
    SimEndpoint* relay_end = nullptr;
    RelayServer relay;
    Member members[4];
    u32 count = 0;

    explicit Lobby(u32 n, bool direct_last = true) : count(n) {
        LinkConditions c;
        c.latency_ms = 5;
        network.set_default_conditions(c);
        relay_end = &network.add_endpoint(16, 2);
        relay.init();
        relay.add_transport(*relay_end);
        for (u32 i = 0; i < n; ++i) {
            members[i].link = &network.add_endpoint(1, 2);
            RtcDesc d;
            d.channels = 3;
            d.direct = direct_last || i + 1 < n;
            d.connect_timeout_ms = 8000;
            members[i].room.start(*members[i].link, members[i].link->connect(*relay_end), d);
        }
    }
    void step() {
        relay.update();
        for (u32 i = 0; i < count; ++i) {
            Member& m = members[i];
            for (Event ev; m.room.poll(ev);) {
                if (ev.kind == EventKind::Connected) ++m.connected;
                if (ev.kind == EventKind::Disconnected) ++m.disconnected;
                if (ev.kind != EventKind::Received) continue;
                Array<u8> bytes;
                bytes.resize(ev.data.size());
                if (!ev.data.empty()) std::memcpy(bytes.data(), ev.data.data(), ev.data.size());
                m.got[ev.channel].push_back(std::move(bytes));
            }
            m.room.flush();
        }
        network.advance(1);
        std::this_thread::sleep_for(std::chrono::milliseconds(1));
    }
    template <class F>
    bool run_until(F&& done, f64 seconds = 10.0) {
        const auto start = std::chrono::steady_clock::now();
        while (!done()) {
            step();
            if (std::chrono::duration<f64>(std::chrono::steady_clock::now() - start).count() > seconds) return false;
        }
        return true;
    }
    void meet() {
        REQUIRE(members[0].room.room().create_room(4));
        REQUIRE(run_until([&] { return members[0].room.room().state() == RelayClient::State::InRoom; }));
        for (u32 i = 1; i < count; ++i) REQUIRE(members[i].room.room().join_room(members[0].room.room().room_code()));
        REQUIRE(run_until([&] {
            for (u32 i = 0; i < count; ++i)
                if (members[i].connected != count - 1) return false;
            return true;
        }));
    }
    // Lets the last signalling (late candidates, the other side's switch) cross the relay.
    void settle() {
        for (int i = 0; i < 300; ++i) step();
    }
    bool all_direct() const {
        for (u32 i = 0; i < count; ++i)
            for (u32 j = 0; j < count; ++j)
                if (i != j && members[i].room.path(j) != RtcPath::Direct) return false;
        return true;
    }
};

Span<const u8> as_bytes(const u32& v) { return Span<const u8>(reinterpret_cast<const u8*>(&v), sizeof(v)); }

u32 value(const Array<u8>& b) {
    u32 v = 0;
    if (b.size() == sizeof(v)) std::memcpy(&v, b.data(), sizeof(v));
    return v;
}

}  // namespace

TEST_CASE("net rtc: two members go direct over loopback ICE, Reliable keeps its order across the switch") {
    if (!RtcHost::direct_supported()) {
        MESSAGE("no WebRTC in this build (MCE_WITH_RTC off): skipped");
        return;
    }
    Lobby lobby(2);
    lobby.meet();
    Member& a = lobby.members[0];
    Member& b = lobby.members[1];
    CHECK(a.room.path(1) == RtcPath::Connecting);  // the lower member offers at once
    // Reliable messages from before the switch to after it, one per step: none lost, none reordered.
    u32 sent = 0;
    const bool direct = lobby.run_until([&] {
        a.room.send(1, 0, as_bytes(sent), Delivery::Reliable);
        b.room.send(0, 0, as_bytes(sent), Delivery::Reliable);
        ++sent;
        return lobby.all_direct() && sent > 50;
    });
    REQUIRE(direct);
    for (u32 i = 0; i < 50; ++i, ++sent) {
        a.room.send(1, 0, as_bytes(sent), Delivery::Reliable);
        b.room.send(0, 0, as_bytes(sent), Delivery::Reliable);
        lobby.step();
    }
    REQUIRE(lobby.run_until([&] { return a.got[0].size() == sent && b.got[0].size() == sent; }));
    for (u32 i = 0; i < sent; ++i) {
        CHECK(value(a.got[0][i]) == i);
        CHECK(value(b.got[0][i]) == i);
    }
    CHECK(a.room.rtc_stats().direct_opened == 1);
    CHECK(b.room.rtc_stats().direct_opened == 1);
    CHECK(a.room.rtc_stats().direct_messages_sent > 0);
    CHECK(b.room.rtc_stats().direct_messages_received > 0);
    CHECK(a.room.rtc_stats().fallbacks == 0);

    // Direct now: Sequenced and Unreliable on the unordered channel, and no relay traffic for them.
    lobby.settle();
    const u64 relayed = lobby.relay.stats().forwarded;
    for (u32 i = 1; i <= 20; ++i) {
        a.room.send(1, 1, as_bytes(i), Delivery::Sequenced);
        a.room.send(1, 2, as_bytes(i), Delivery::Unreliable);
        lobby.step();
    }
    REQUIRE(lobby.run_until([&] { return b.got[1].size() >= 15 && b.got[2].size() >= 15; }));
    for (usize i = 1; i < b.got[1].size(); ++i) CHECK(value(b.got[1][i]) > value(b.got[1][i - 1]));
    CHECK(lobby.relay.stats().forwarded == relayed);  // nothing went through the relay
    // A large Reliable message crosses whole.
    Array<u8> big;
    big.resize(50000);
    for (usize i = 0; i < big.size(); ++i) big[i] = static_cast<u8>(i * 7);
    CHECK(b.room.send(0, 0, Span<const u8>(big.data(), big.size()), Delivery::Reliable));
    REQUIRE(lobby.run_until([&] { return a.got[0].size() == sent + 1; }));
    CHECK(a.got[0][sent].size() == big.size());
    CHECK(std::memcmp(a.got[0][sent].data(), big.data(), big.size()) == 0);
}

TEST_CASE("net rtc: three members, every pair direct; broadcasts reach all, and a member leaving closes its pairs") {
    if (!RtcHost::direct_supported()) {
        MESSAGE("no WebRTC in this build (MCE_WITH_RTC off): skipped");
        return;
    }
    Lobby lobby(3);
    lobby.meet();
    REQUIRE(lobby.run_until([&] { return lobby.all_direct(); }));
    lobby.settle();
    const u64 relayed = lobby.relay.stats().forwarded;
    const u32 hello = 42;
    lobby.members[1].room.broadcast(0, as_bytes(hello), Delivery::Reliable);
    REQUIRE(lobby.run_until([&] { return lobby.members[0].got[0].size() == 1 && lobby.members[2].got[0].size() == 1; }));
    CHECK(lobby.relay.stats().forwarded == relayed);
    // Member 2 leaves: the others see it go, and their own pair stays direct.
    lobby.members[2].room.room().leave();
    REQUIRE(lobby.run_until([&] { return lobby.members[0].disconnected == 1 && lobby.members[1].disconnected == 1; }));
    CHECK(lobby.members[0].room.path(2) == RtcPath::Relay);
    CHECK(lobby.members[0].room.path(1) == RtcPath::Direct);
    CHECK(lobby.members[2].room.path(0) == RtcPath::Relay);
    lobby.members[0].room.send(1, 0, as_bytes(hello), Delivery::Reliable);
    REQUIRE(lobby.run_until([&] { return lobby.members[1].got[0].size() == 1; }));
}

TEST_CASE("net rtc: a member without direct connections keeps its pairs on the relay, and everything still crosses") {
    Lobby lobby(3, /*direct_last=*/false);  // member 2 turned direct off
    lobby.meet();
    Member& a = lobby.members[0];
    Member& c = lobby.members[2];
    const bool rtc = RtcHost::direct_supported();
    REQUIRE(lobby.run_until([&] {
        return a.room.path(2) == RtcPath::Relay && lobby.members[1].room.path(2) == RtcPath::Relay &&
               (!rtc || a.room.path(1) == RtcPath::Direct);
    }));
    if (rtc) CHECK(a.room.rtc_stats().fallbacks == 1);  // offered to 2, refused
    CHECK(c.room.rtc_stats().direct_opened == 0);
    // A broadcast goes direct to member 1 and through the relay to member 2.
    for (u32 i = 1; i <= 5; ++i) {
        a.room.broadcast(0, as_bytes(i), Delivery::Reliable);
        a.room.broadcast(1, as_bytes(i), Delivery::Sequenced);
    }
    REQUIRE(lobby.run_until([&] { return c.got[0].size() == 5 && lobby.members[1].got[0].size() == 5; }));
    for (u32 i = 0; i < 5; ++i) CHECK(value(c.got[0][i]) == i + 1);
    CHECK(c.room.rtc_stats().relay_messages_received >= 5);
    c.room.send(0, 2, as_bytes(7u), Delivery::Unreliable);
    REQUIRE(lobby.run_until([&] { return a.got[2].size() == 1; }));
    CHECK(value(a.got[2][0]) == 7);
}

TEST_CASE("net rtc: snapshot replication runs unchanged over direct links") {
    if (!RtcHost::direct_supported()) {
        MESSAGE("no WebRTC in this build (MCE_WITH_RTC off): skipped");
        return;
    }
    Lobby lobby(2);
    lobby.meet();
    REQUIRE(lobby.run_until([&] { return lobby.all_direct(); }));
    World server_world, client_world;
    ReplicationServer server;
    ReplicationClient client;
    LocalTransform t;
    t.position = {1.0f, 2.0f, 3.0f};
    const Entity e = server_world.spawn_with(t);
    const u32 id = server.assign(server_world, e, 5);
    auto apply = [&] {
        for (const Array<u8>& m : lobby.members[1].got[1]) {
            client.apply(client_world.commands(), client_world, Span<const u8>(m.data(), m.size()));
            client_world.sync(Phase::FixedUpdate_Pre);
        }
        lobby.members[1].got[1].clear();
    };
    lobby.settle();
    const u64 relayed = lobby.relay.stats().forwarded;
    for (u32 tick = 1; tick <= 30; ++tick) {
        server_world.try_get_mut<LocalTransform>(e)->position.x = static_cast<f32>(tick);
        ByteWriter w;
        server.write_snapshot(server_world, tick, w);
        lobby.members[0].room.broadcast(1, w.data(), Delivery::Sequenced);
        lobby.step();
        apply();
    }
    REQUIRE(lobby.run_until([&] {
        apply();
        const Entity seen = client.find(id);
        return client_world.alive(seen) && client_world.try_get<LocalTransform>(seen)->position.x > 25.0f;
    }));
    CHECK(lobby.relay.stats().forwarded == relayed);
}
