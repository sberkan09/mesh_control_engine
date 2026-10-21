#include <doctest/doctest.h>

#include "mce/net/replication.h"

using namespace mce;
using namespace mce::net;

namespace {

struct Health {
    u32 hp;
};
struct Visual {
    u32 kind;
};

LocalTransform at(f32 x, f32 y) {
    LocalTransform t;
    t.position = {x, y, 0.0f};
    return t;
}

// One snapshot from `server_world`, applied to `client_world` outside a phase.
ApplyResult replicate(ReplicationServer& server, World& server_world, u32 tick, ReplicationClient& client,
                      World& client_world) {
    ByteWriter w;
    server.write_snapshot(server_world, tick, w);
    const ApplyResult r = client.apply(client_world.commands(), client_world, w.data());
    client_world.sync(Phase::FixedUpdate_Pre);
    return r;
}

}  // namespace

TEST_CASE("net replication: spawn, update and despawn mirror the server") {
    World sw, cw;
    ReplicationServer server;
    ReplicationClient client;

    const Entity a = sw.spawn_with(at(1, 2));
    const Entity b = sw.spawn_with(at(3, 4));
    sw.spawn_with(at(9, 9));  // not tagged: never replicated
    const u32 ida = server.assign(sw, a, 10);
    const u32 idb = server.assign(sw, b, 20);
    CHECK(ida != idb);

    ApplyResult r = replicate(server, sw, 1, client, cw);
    CHECK(r.ok);
    CHECK(r.spawned == 2);
    CHECK(cw.entity_count() == 2);
    const Entity ca = client.find(ida);
    REQUIRE(cw.alive(ca));
    CHECK(cw.try_get<LocalTransform>(ca)->position.y == 2.0f);
    CHECK(cw.try_get<NetId>(ca)->kind == 10);

    sw.try_get_mut<LocalTransform>(a)->position.x = 5.0f;
    sw.destroy(b);
    r = replicate(server, sw, 2, client, cw);
    CHECK(r.ok);
    CHECK(r.updated == 1);
    CHECK(r.despawned == 1);
    CHECK(cw.entity_count() == 1);
    CHECK(client.find(ida) == ca);  // same local entity kept
    CHECK(cw.try_get<LocalTransform>(ca)->position.x == 5.0f);
    CHECK(client.find(idb).is_null());
}

TEST_CASE("net replication: stale and malformed snapshots change nothing") {
    World sw, cw;
    ReplicationServer server;
    ReplicationClient client;
    const Entity a = sw.spawn_with(at(1, 0));
    const u32 id = server.assign(sw, a, 0);

    ByteWriter old_snapshot;
    server.write_snapshot(sw, 5, old_snapshot);
    sw.try_get_mut<LocalTransform>(a)->position.x = 2.0f;
    REQUIRE(replicate(server, sw, 6, client, cw).ok);

    // Older tick: ignored.
    CHECK_FALSE(client.apply(cw.commands(), cw, old_snapshot.data()).ok);
    cw.sync(Phase::FixedUpdate_Pre);
    CHECK(cw.try_get<LocalTransform>(client.find(id))->position.x == 2.0f);

    // Every truncation of a valid snapshot is rejected without queuing a command.
    ByteWriter good;
    sw.try_get_mut<LocalTransform>(a)->position.x = 3.0f;
    server.write_snapshot(sw, 7, good);
    for (usize n = 0; n < good.size(); ++n) {
        CHECK_FALSE(client.apply(cw.commands(), cw, good.data().subspan(0, n)).ok);
        CHECK(cw.commands().empty());
    }
    // Trailing garbage too.
    ByteWriter longer;
    longer.append(good.data());
    longer.write_u8(0);
    CHECK_FALSE(client.apply(cw.commands(), cw, longer.data()).ok);
    // Wrong magic.
    const u8 junk[1] = {'X'};
    ByteWriter wrong;
    wrong.append(Span<const u8>(junk, 1));
    wrong.append(good.data().subspan(1));
    CHECK_FALSE(client.apply(cw.commands(), cw, wrong.data()).ok);
    CHECK(cw.commands().empty());

    // The intact one still applies.
    CHECK(client.apply(cw.commands(), cw, good.data()).ok);
    cw.sync(Phase::FixedUpdate_Pre);
    CHECK(cw.try_get<LocalTransform>(client.find(id))->position.x == 3.0f);
}

TEST_CASE("net replication: hooks carry extra state and build local parts") {
    World sw, cw;
    ReplicationServer server;
    ReplicationClient client;

    struct Counts {
        u32 spawned = 0;
        u32 despawned = 0;
    } counts;

    ReplicationHooks sh;
    sh.write_extra = [](void*, const World& w, Entity e, ByteWriter& out) {
        const Health* h = w.try_get<Health>(e);
        out.write_varu32(h ? h->hp : 0);
    };
    server.set_hooks(sh);

    ReplicationHooks ch;
    ch.user = &counts;
    ch.read_extra = [](void*, Commands& cmd, Entity e, ByteReader& in) { cmd.add(e, Health{in.read_varu32()}); };
    ch.on_spawn = [](void* user, Commands& cmd, Entity e, const NetId& id) {
        ++static_cast<Counts*>(user)->spawned;
        cmd.add(e, Visual{id.kind});
    };
    ch.on_despawn = [](void* user, Commands&, Entity, const NetId&) { ++static_cast<Counts*>(user)->despawned; };
    client.set_hooks(ch);

    const Entity a = sw.spawn_with(at(0, 0), Health{100});
    const u32 id = server.assign(sw, a, 3);
    REQUIRE(replicate(server, sw, 1, client, cw).ok);
    const Entity ca = client.find(id);
    CHECK(cw.try_get<Health>(ca)->hp == 100);
    CHECK(cw.try_get<Visual>(ca)->kind == 3);

    sw.try_get_mut<Health>(a)->hp = 40;
    REQUIRE(replicate(server, sw, 2, client, cw).ok);
    CHECK(cw.try_get<Health>(ca)->hp == 40);
    CHECK(counts.spawned == 1);

    // A client-side destroy is repaired by the next snapshot.
    cw.destroy(ca);
    REQUIRE(replicate(server, sw, 3, client, cw).spawned == 1);
    CHECK(cw.alive(client.find(id)));
    CHECK(counts.spawned == 2);

    // reset() clears the mirror (a disconnect).
    client.reset(cw.commands(), cw);
    cw.sync(Phase::FixedUpdate_Pre);
    CHECK(cw.entity_count() == 0);
    CHECK(client.size() == 0);
    CHECK(counts.despawned == 1);
}

TEST_CASE("net replication: apply inside a system takes effect at the phase's sync point") {
    World sw, cw;
    ReplicationServer server;
    const Entity a = sw.spawn_with(at(7, 0));
    server.assign(sw, a, 1);
    static ByteWriter snapshot;
    snapshot.clear();
    server.write_snapshot(sw, 1, snapshot);

    static ReplicationClient client;
    cw.add_system(Phase::FixedUpdate_Pre, "net_apply", [](Frame& f) {
        CHECK(client.apply(f.commands(), f.world(), snapshot.data()).ok);
    });
    cw.tick(1.0 / 60.0, 1);
    CHECK(cw.entity_count() == 1);
    CHECK(cw.try_get<LocalTransform>(client.find(1))->position.x == 7.0f);
}

TEST_CASE("net replication: adopt turns a client's mirror into the server world") {
    World sw, heir_world, other_world;
    ReplicationServer server;
    ReplicationClient heir, other;
    heir.set_interpolation({});
    Entity es[3];
    for (u32 i = 0; i < 3; ++i) {
        LocalTransform t;
        t.position = {static_cast<f32>(i), 0.0f, 0.0f};
        es[i] = sw.spawn_with(t);
        server.assign(sw, es[i], 7);
    }
    for (u32 tick = 1; tick <= 10; ++tick) {
        for (u32 i = 0; i < 3; ++i) sw.try_get_mut<LocalTransform>(es[i])->position.y = static_cast<f32>(tick);
        ByteWriter w;
        server.write_snapshot(sw, tick, w);
        REQUIRE(heir.apply(heir_world.commands(), heir_world, w.data()).ok);
        heir_world.sync(Phase::FixedUpdate_Pre);
        REQUIRE(other.apply(other_world.commands(), other_world, w.data()).ok);
        other_world.sync(Phase::FixedUpdate_Pre);
        heir.update(heir_world.commands(), heir_world, 1.0 / 60.0);  // interpolated: behind the snapshot
        heir_world.sync(Phase::FixedUpdate_Pre);
    }

    ReplicationServer next;
    CHECK(next.adopt(heir_world, heir) == 10);
    CHECK(heir.size() == 0);
    u32 at_newest = 0;
    heir_world.query<const NetId, const LocalTransform>().each([&](const NetId& id, const LocalTransform& t) {
        if (id.kind == 7 && t.position.y == 10.0f) ++at_newest;
    });
    CHECK(at_newest == 3);  // the snapshot's transform, not the interpolated one
    const Entity spawned = heir_world.spawn_with(LocalTransform{});
    CHECK(next.assign(heir_world, spawned, 8) == 4);  // new ids after the adopted ones

    // The other client goes on mirroring the new server, with no reset.
    heir_world.query<const NetId, LocalTransform>().each([](const NetId&, LocalTransform& t) { t.position.z = 5.0f; });
    ByteWriter w;
    next.write_snapshot(heir_world, 11, w);
    const ApplyResult r = other.apply(other_world.commands(), other_world, w.data());
    other_world.sync(Phase::FixedUpdate_Pre);
    REQUIRE(r.ok);
    CHECK(r.spawned == 1);
    CHECK(r.updated == 3);
    for (u32 i = 0; i < 3; ++i) {
        const Entity m = other.find(i + 1);
        REQUIRE(other_world.alive(m));
        CHECK(other_world.try_get<LocalTransform>(m)->position.z == 5.0f);
    }
}
