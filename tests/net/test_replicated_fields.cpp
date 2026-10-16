// M-NET2 step 7: replication as data. Reflected fields flagged Replicated travel in the snapshot;
// entities a scene marks NetReplicated get their NetId with no code.

#include <doctest/doctest.h>

#include <cstring>

#include "mce/net/replication.h"

using namespace mce;
using namespace mce::net;

namespace rftest {

struct Fighter {
    u32 hp = 100;
    Entity target;
    f32 local_glow = 0.5f;  // client-side only: never sent
};
MCE_COMPONENT(Fighter, (hp, Replicated), (target, Replicated), local_glow)

struct Score {
    u32 points = 0;
};
MCE_COMPONENT(Score, (points, Replicated))

struct Cosmetic {  // nothing flagged: not sent at all
    u32 hat = 0;
};
MCE_COMPONENT(Cosmetic, hat)

}  // namespace rftest

using rftest::Cosmetic;
using rftest::Fighter;
using rftest::Score;

namespace {

ApplyResult replicate(ReplicationServer& server, World& sw, u32 tick, ReplicationClient& client, World& cw,
                      ByteWriter* keep = nullptr) {
    ByteWriter w;
    server.write_snapshot(sw, tick, w);
    if (keep) keep->append(w.data());
    const ApplyResult r = client.apply(cw.commands(), cw, w.data());
    cw.sync(Phase::FixedUpdate_Pre);
    return r;
}

}  // namespace

TEST_CASE("net replicated fields: flagged fields reach the client, other fields keep the client's values") {
    World sw, cw;
    ReplicationServer server;
    ReplicationClient client;

    const Entity a = sw.spawn_with(LocalTransform{}, Fighter{80, Entity::null, 2.0f}, Cosmetic{7});
    const Entity b = sw.spawn_with(LocalTransform{}, Score{3});
    sw.try_get_mut<Fighter>(a)->target = b;
    const u32 ida = server.assign(sw, a, 1);
    const u32 idb = server.assign(sw, b, 2);

    ApplyResult r = replicate(server, sw, 1, client, cw);
    REQUIRE(r.ok);
    CHECK(r.spawned == 2);
    CHECK(r.components == 2);
    CHECK(r.skipped_components == 0);
    const Entity ca = client.find(ida), cb = client.find(idb);
    REQUIRE(cw.alive(ca));
    REQUIRE(cw.alive(cb));
    const Fighter* f = cw.try_get<Fighter>(ca);
    REQUIRE(f);
    CHECK(f->hp == 80);
    CHECK(f->target == cb);                      // the server's entity comes back as the client's own
    CHECK(f->local_glow == 0.5f);                // not replicated: the default, not the server's 2.0
    CHECK(cw.try_get<Cosmetic>(ca) == nullptr);  // no Replicated field: not sent
    CHECK(cw.try_get<Score>(cb)->points == 3);

    // The client's own value of an unflagged field survives later snapshots.
    cw.try_get_mut<Fighter>(ca)->local_glow = 9.0f;
    sw.try_get_mut<Fighter>(a)->hp = 55;
    sw.try_get_mut<Score>(b)->points = 4;
    r = replicate(server, sw, 2, client, cw);
    REQUIRE(r.ok);
    CHECK(cw.try_get<Fighter>(ca)->hp == 55);
    CHECK(cw.try_get<Fighter>(ca)->local_glow == 9.0f);
    CHECK(cw.try_get<Score>(cb)->points == 4);

    // A target that is not replicated, or gone, arrives as null.
    const Entity loner = sw.spawn_with(LocalTransform{});
    sw.try_get_mut<Fighter>(a)->target = loner;
    REQUIRE(replicate(server, sw, 3, client, cw).ok);
    CHECK(cw.try_get<Fighter>(ca)->target.is_null());
    sw.try_get_mut<Fighter>(a)->target = b;
    sw.destroy(b);
    r = replicate(server, sw, 4, client, cw);
    REQUIRE(r.ok);
    CHECK(r.despawned == 1);
    CHECK(cw.try_get<Fighter>(ca)->target.is_null());
}

TEST_CASE("net replicated fields: a component laid out differently on the client is skipped, not misread") {
    World sw, cw;
    ReplicationServer server;
    ReplicationClient client;
    const Entity a = sw.spawn_with(LocalTransform{}, Score{12});
    const u32 id = server.assign(sw, a, 0);

    ByteWriter w;
    server.write_snapshot(sw, 1, w);
    // Find the Score block's layout word and change it, as a build with other fields would.
    const refl::TypeRecord* type = refl::find_type(refl::type_id<Score>());
    REQUIRE(type);
    const u32 layout = replicated_layout(*type);
    CHECK(layout != 0);
    Array<u8> bytes;
    bytes.resize(w.data().size());
    std::memcpy(bytes.data(), w.data().data(), bytes.size());
    u8 word[4];
    std::memcpy(word, &layout, 4);
    bool patched = false;
    for (usize i = 0; i + 4 <= bytes.size() && !patched; ++i)
        if (std::memcmp(bytes.data() + i, word, 4) == 0) {
            bytes[i] ^= 0x5A;
            patched = true;
        }
    REQUIRE(patched);

    const ApplyResult r = client.apply(cw.commands(), cw, Span<const u8>(bytes.data(), bytes.size()));
    cw.sync(Phase::FixedUpdate_Pre);
    REQUIRE(r.ok);  // the rest of the snapshot still applies
    CHECK(r.spawned == 1);
    CHECK(r.components == 0);
    CHECK(r.skipped_components == 1);
    REQUIRE(cw.alive(client.find(id)));
    CHECK(cw.try_get<Score>(client.find(id)) == nullptr);

    // Types without Replicated fields have no layout.
    const refl::TypeRecord* cosmetic = refl::find_type(refl::type_id<Cosmetic>());
    REQUIRE(cosmetic);
    CHECK(replicated_layout(*cosmetic) == 0);
}

TEST_CASE("net replicated fields: NetReplicated entities get ids from assign_tagged, and NetOwner travels") {
    World sw, cw;
    ReplicationServer server;
    ReplicationClient client;
    const Entity a = sw.spawn_with(LocalTransform{}, NetReplicated{4}, NetOwner{2});
    const Entity b = sw.spawn_with(LocalTransform{}, NetReplicated{5});
    sw.spawn_with(LocalTransform{});  // unmarked: stays local

    CHECK(server.assign_tagged(sw) == 2);
    CHECK(server.assign_tagged(sw) == 0);  // already assigned
    REQUIRE(sw.try_get<NetId>(a));
    CHECK(sw.try_get<NetId>(a)->kind == 4);
    CHECK(sw.try_get<NetId>(b)->kind == 5);
    CHECK(sw.try_get<NetId>(a)->id != sw.try_get<NetId>(b)->id);

    // Through Commands, inside a phase: an entity spawned later is picked up.
    const Entity c = sw.spawn_with(LocalTransform{}, NetReplicated{6});
    CHECK(server.assign_tagged(sw.commands(), sw) == 1);
    sw.sync(Phase::FixedUpdate_Pre);
    REQUIRE(sw.try_get<NetId>(c));
    CHECK(sw.try_get<NetId>(c)->kind == 6);

    const ApplyResult r = replicate(server, sw, 1, client, cw);
    REQUIRE(r.ok);
    CHECK(r.spawned == 3);
    const Entity ca = client.find(sw.try_get<NetId>(a)->id);
    REQUIRE(cw.alive(ca));
    REQUIRE(cw.try_get<NetOwner>(ca));
    CHECK(cw.try_get<NetOwner>(ca)->peer == 2);
    CHECK(cw.try_get<NetOwner>(client.find(sw.try_get<NetId>(b)->id)) == nullptr);
}
