// M-NET3 step 1: per-client snapshots, delta-coded against the client's acknowledged one, with
// interest management (NetRelevance).

#include <doctest/doctest.h>

#include <cstring>

#include "mce/net/replication.h"

using namespace mce;
using namespace mce::net;

namespace dtest {

struct Armor {
    u32 points = 0;
};
MCE_COMPONENT(Armor, (points, Replicated))

}  // namespace dtest

using dtest::Armor;

namespace {

constexpr PeerId kPeer = 3;

LocalTransform at(f32 x, f32 y) {
    LocalTransform t;
    t.position = {x, y, 0.0f};
    return t;
}

struct Session {
    World sw, cw;
    ReplicationServer server;
    ReplicationClient client;
    ByteWriter last;  // the newest snapshot written
    u64 hash = 0;

    // One send tick: write the client's snapshot, optionally apply it and acknowledge.
    ApplyResult send(u32 tick, bool deliver = true, bool ack = true) {
        last.clear();
        hash = server.write_snapshot_for(sw, tick, kPeer, last);
        if (!deliver) return {};
        const ApplyResult r = client.apply(cw.commands(), cw, last.data());
        cw.sync(Phase::FixedUpdate_Pre);
        if (ack) acknowledge();
        return r;
    }
    void acknowledge() {
        ByteWriter a;
        client.write_ack(a);
        ByteReader in(a.data());
        REQUIRE(server.read_ack(kPeer, in));
    }
    // Every entity on the client matches the server's, bit for bit.
    bool mirrors() {
        bool ok = true;
        sw.query<const NetId, const LocalTransform>().each([&](const NetId& id, const LocalTransform& t) {
            const Entity e = client.find(id.id);
            const LocalTransform* m = cw.alive(e) ? cw.try_get<LocalTransform>(e) : nullptr;
            ok = ok && m && m->position.x == t.position.x && m->position.y == t.position.y &&
                 m->position.z == t.position.z;
        });
        return ok;
    }
};

}  // namespace

TEST_CASE("net delta: after the first acknowledgement snapshots are deltas, far smaller, mirroring exactly") {
    Session s;
    Entity mover = Entity::null;
    for (u32 i = 0; i < 50; ++i) {
        const Entity e = s.sw.spawn_with(at(f32(i), 0), Armor{i});
        s.server.assign(s.sw, e, i % 4);
        if (i == 7) mover = e;
    }
    ApplyResult r = s.send(1);
    REQUIRE(r.ok);
    CHECK_FALSE(r.delta);
    CHECK(r.spawned == 50);
    const usize full_bytes = s.last.size();
    CHECK(s.client.content_hash() == s.hash);

    // Nothing moved: two bytes per entity plus the header.
    r = s.send(2);
    REQUIRE(r.ok);
    CHECK(r.delta);
    CHECK(r.updated == 50);
    CHECK(s.last.size() < 30 + 2 * 50);
    CHECK(s.client.content_hash() == s.hash);

    // One entity moves along x and its armor changes: only those bytes travel.
    s.sw.try_get_mut<LocalTransform>(mover)->position.x = 99.5f;
    s.sw.try_get_mut<Armor>(mover)->points = 1000;
    r = s.send(3);
    REQUIRE(r.ok);
    CHECK(s.last.size() < 30 + 2 * 50 + 40);
    CHECK(s.mirrors());
    CHECK(s.cw.try_get<Armor>(s.client.find(s.sw.try_get<NetId>(mover)->id))->points == 1000);

    const ReplicationServer::ClientStats* st = s.server.client_stats(kPeer);
    REQUIRE(st);
    CHECK(st->snapshots == 3);
    CHECK(st->full == 1);
    CHECK(st->entities == 50);
    CHECK(st->bytes < full_bytes * 3 / 2);

    // Spawns and despawns through deltas.
    s.sw.destroy(mover);
    const Entity late = s.sw.spawn_with(at(-4, 2), Armor{5});
    const u32 late_id = s.server.assign(s.sw, late, 9);
    r = s.send(4);
    REQUIRE(r.ok);
    CHECK(r.delta);
    CHECK(r.spawned == 1);
    CHECK(r.despawned == 1);
    CHECK(s.cw.try_get<NetId>(s.client.find(late_id))->kind == 9);
    CHECK(s.mirrors());
    CHECK(s.client.size() == 50);
}

TEST_CASE("net delta: lost snapshots and late acknowledgements still decode against what the client has") {
    Session s;
    const Entity a = s.sw.spawn_with(at(0, 0));
    s.server.assign(s.sw, a, 0);
    REQUIRE(s.send(1).ok);
    for (u32 tick = 2; tick < 40; ++tick) {
        s.sw.try_get_mut<LocalTransform>(a)->position.x = f32(tick);
        // Every third snapshot is lost; acknowledgements only go back every fourth tick.
        const bool deliver = tick % 3 != 0;
        const ApplyResult r = s.send(tick, deliver, tick % 4 == 0);
        if (deliver) {
            REQUIRE(r.ok);
            CHECK(s.mirrors());
        }
    }
    // With no acknowledgement for longer than the server keeps, the next one is full again.
    for (u32 tick = 40; tick < 40 + ReplicationServer::kBaselines + 1; ++tick) s.send(tick, false);
    const ApplyResult r = s.send(100);
    REQUIRE(r.ok);
    CHECK_FALSE(r.delta);
}

TEST_CASE("net delta: a client that lost its history asks for a full snapshot") {
    Session s;
    const Entity a = s.sw.spawn_with(at(1, 1));
    s.server.assign(s.sw, a, 0);
    REQUIRE(s.send(1).ok);
    REQUIRE(s.send(2).delta);

    s.client.reset(s.cw.commands(), s.cw);
    s.cw.sync(Phase::FixedUpdate_Pre);
    ApplyResult r = s.send(3, true, false);
    CHECK_FALSE(r.ok);
    CHECK(r.missing_baseline);
    CHECK(s.cw.commands().empty());
    s.acknowledge();  // the reset client acknowledges nothing: tick 0
    r = s.send(4);
    REQUIRE(r.ok);
    CHECK_FALSE(r.delta);
    CHECK(s.mirrors());

    s.server.forget_client(kPeer);
    CHECK(s.server.client_stats(kPeer) == nullptr);
    CHECK_FALSE(s.send(5).delta);
}

TEST_CASE("net delta: the same tick written twice is told apart by its hash") {
    Session s;
    const Entity a = s.sw.spawn_with(at(1, 0));
    s.server.assign(s.sw, a, 0);
    REQUIRE(s.send(1).ok);
    REQUIRE(s.send(2).ok);  // the client acknowledges this tick-2 snapshot
    // The server's state changes after its last tick (a late input) and it writes tick 2 again,
    // which the client never receives.
    s.sw.try_get_mut<LocalTransform>(a)->position.x = 7.0f;
    s.send(2, false);
    s.sw.try_get_mut<LocalTransform>(a)->position.x = 8.0f;
    const ApplyResult r = s.send(3);
    REQUIRE(r.ok);
    CHECK(r.delta);
    CHECK(s.mirrors());
}

TEST_CASE("net delta: every truncation of a delta is rejected without queuing a command") {
    Session s;
    for (u32 i = 0; i < 4; ++i) s.server.assign(s.sw, s.sw.spawn_with(at(f32(i), 0), Armor{i}), i);
    REQUIRE(s.send(1).ok);
    s.sw.query<LocalTransform>().each([](LocalTransform& t) { t.position.y += 1.0f; });
    ByteWriter delta;
    s.server.write_snapshot_for(s.sw, 2, kPeer, delta);
    for (usize n = 0; n < delta.size(); ++n) {
        CHECK_FALSE(s.client.apply(s.cw.commands(), s.cw, delta.data().subspan(0, n)).ok);
        CHECK(s.cw.commands().empty());
    }
    ByteWriter longer;
    longer.append(delta.data());
    longer.write_u8(0);
    CHECK_FALSE(s.client.apply(s.cw.commands(), s.cw, longer.data()).ok);
    // A flipped payload byte fails the content hash.
    Array<u8> bent;
    bent.resize(delta.size());
    std::memcpy(bent.data(), delta.data().data(), bent.size());
    bent[bent.size() - 1] ^= 0x10;
    CHECK_FALSE(s.client.apply(s.cw.commands(), s.cw, Span<const u8>(bent.data(), bent.size())).ok);
    CHECK(s.cw.commands().empty());
    CHECK(s.client.apply(s.cw.commands(), s.cw, delta.data()).ok);
}

TEST_CASE("net delta: NetRelevance sends an entity only to clients with an owned entity close by") {
    Session s;
    const Entity me = s.sw.spawn_with(at(0, 0), NetOwner{kPeer}, NetRelevance{1.0f});
    const Entity other = s.sw.spawn_with(at(50, 0), NetOwner{kPeer + 1}, NetRelevance{10.0f});
    const Entity sign = s.sw.spawn_with(at(500, 0));  // no NetRelevance: everybody gets it
    const u32 my_id = s.server.assign(s.sw, me, 0);
    const u32 other_id = s.server.assign(s.sw, other, 0);
    const u32 sign_id = s.server.assign(s.sw, sign, 1);

    REQUIRE(s.send(1).ok);
    CHECK(s.cw.alive(s.client.find(my_id)));  // ours, though our own radius is small
    CHECK(s.client.find(other_id).is_null());
    CHECK(s.cw.alive(s.client.find(sign_id)));
    CHECK(s.server.client_stats(kPeer)->entities == 2);

    s.sw.try_get_mut<LocalTransform>(other)->position.x = 6.0f;  // inside its 10 m of us
    ApplyResult r = s.send(2);
    REQUIRE(r.ok);
    CHECK(r.spawned == 1);
    CHECK(s.cw.alive(s.client.find(other_id)));

    s.sw.try_get_mut<LocalTransform>(me)->position.x = -20.0f;  // we walk away
    r = s.send(3);
    REQUIRE(r.ok);
    CHECK(r.despawned == 1);
    CHECK(s.client.find(other_id).is_null());

    // The broadcast snapshot still lists everything.
    ByteWriter all;
    s.server.write_snapshot(s.sw, 4, all);
    ReplicationClient everyone;
    World w;
    CHECK(everyone.apply(w.commands(), w, all.data()).spawned == 3);
}

TEST_CASE("net delta: a spectator sees what its camera sees, or everything") {
    Session s;  // kPeer owns nothing here: a spectator
    const Entity a = s.sw.spawn_with(at(0, 0), NetOwner{kPeer + 1}, NetRelevance{10.0f});
    const Entity b = s.sw.spawn_with(at(100, 0), NetOwner{kPeer + 2}, NetRelevance{10.0f});
    const Entity sign = s.sw.spawn_with(at(500, 0));
    const u32 a_id = s.server.assign(s.sw, a, 0);
    const u32 b_id = s.server.assign(s.sw, b, 0);
    s.server.assign(s.sw, sign, 1);

    REQUIRE(s.send(1).ok);
    CHECK(s.server.client_stats(kPeer)->entities == 1);  // owning nothing, it sees only the sign

    const math::vec3 camera{98.0f, 0.0f, 0.0f};
    s.server.set_view(kPeer, &camera);
    REQUIRE(s.send(2).ok);
    CHECK(s.client.find(a_id).is_null());
    CHECK(s.cw.alive(s.client.find(b_id)));

    s.server.set_sees_all(kPeer, true);
    REQUIRE(s.send(3).ok);
    CHECK(s.server.client_stats(kPeer)->entities == 3);
    CHECK(s.mirrors());

    s.server.set_sees_all(kPeer, false);
    s.server.set_view(kPeer, nullptr);
    const ApplyResult r = s.send(4);
    REQUIRE(r.ok);
    CHECK(r.despawned == 2);
    s.server.forget_client(kPeer);
    CHECK(s.server.client_stats(kPeer) == nullptr);
}
