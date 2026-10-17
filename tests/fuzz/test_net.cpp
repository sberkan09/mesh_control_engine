// M-FUZZ 3: what a peer on the network can send. A packet is data from someone else's machine: it may be
// stale, truncated or built to confuse, and the receiver must answer with "false" or a counter, never a crash,
// an out-of-bounds read or an assert. Corpora are packets the engine's own writers made.
//
//   replication: full and delta snapshots (ReplicationClient::apply), and the acknowledgement a client sends
//   rollback:    the packets of a two-player session (RollbackSession::receive)
//   relay:       the request and data messages a RelayServer reads from a connected client

#include <doctest/doctest.h>

#include <cstdio>
#include <cstdlib>
#include <cstring>
#include <vector>

#include "mce/log.h"
#include "mce/net/net.h"
#include "mutator.h"
#include "test_support.h"

using namespace mce;
using namespace mce::net;

namespace {

u32 iterations() {
    const char* e = std::getenv("MCE_FUZZ_ITERS");
    return e ? static_cast<u32>(std::strtoul(e, nullptr, 10)) : 400u;
}

Span<const byte> as_bytes(Span<const u8> s) { return Span<const byte>(reinterpret_cast<const byte*>(s.data()), s.size()); }

// One mutant of `good`, as bytes a net function takes.
Span<const u8> mutant(fuzz::Mutator& m, Span<const u8> good, std::vector<std::uint64_t>& storage) {
    const usize n = m.mutate(as_bytes(good), storage);
    return Span<const u8>(reinterpret_cast<const u8*>(storage.data()), n);
}

// ---- replication ----------------------------------------------------------------------------------------

struct Extra {
    u32 seen = 0;
    static void write(void*, const World&, Entity, ByteWriter& out) { out.write_u32(0xC0FFEE); }
    static void read(void* user, Commands&, Entity, ByteReader& in) {
        static_cast<Extra*>(user)->seen += in.read_u32() != 0;
    }
};

LocalTransform at(f32 x, f32 y) {
    LocalTransform t;
    t.position = {x, y, 0.0f};
    return t;
}

}  // namespace

TEST_CASE("fuzz: replication snapshots and acknowledgements survive mutation") {
    log::set_level(log::Level::Off);
    Extra extra;
    ReplicationHooks hooks;
    hooks.user = &extra;
    hooks.write_extra = &Extra::write;
    hooks.read_extra = &Extra::read;

    World sw;
    ReplicationServer server;
    server.set_hooks(hooks);
    Entity ents[6];
    for (u32 i = 0; i < 6; ++i) {
        ents[i] = sw.spawn_with(at(static_cast<f32>(i), 2.f));
        server.assign(sw, ents[i], i);
    }
    // A full snapshot, then a delta against it (the client acknowledges the first).
    constexpr PeerId kPeer = 1;
    ByteWriter full, delta, ack;
    server.write_snapshot_for(sw, 1, kPeer, full);
    {
        World cw;
        ReplicationClient client;
        client.set_hooks(hooks);
        REQUIRE(client.apply(cw.commands(), cw, full.data()).ok);
        cw.sync(Phase::FixedUpdate_Pre);
        client.write_ack(ack);
        ByteReader r(ack.data());
        REQUIRE(server.read_ack(kPeer, r));
    }
    sw.try_get_mut<LocalTransform>(ents[1])->position.x = 40.f;
    sw.destroy(ents[3]);
    server.write_snapshot_for(sw, 2, kPeer, delta);
    REQUIRE(!full.data().empty());
    REQUIRE(!delta.data().empty());

    std::vector<std::uint64_t> storage;
    fuzz::Mutator m(0x4E7);
    u64 accepted = 0, acks = 0;
    const u32 n = iterations();
    for (u32 it = 0; it < n; ++it) {
        INFO("replay: replication iteration " << it);
        World cw;
        ReplicationClient client;
        client.set_hooks(hooks);
        if (it % 2 == 1) {  // a client that already has the baseline, so a delta applies
            REQUIRE(client.apply(cw.commands(), cw, full.data()).ok);
            cw.sync(Phase::FixedUpdate_Pre);
        }
        const Span<const u8> bytes = mutant(m, (it % 2 ? delta : full).data(), storage);
        const ApplyResult r = client.apply(cw.commands(), cw, bytes);
        cw.sync(Phase::FixedUpdate_Pre);
        accepted += r.ok;
        // What the game does next: look the entities up and read them.
        for (u32 id = 0; id < 8; ++id) {
            const Entity e = client.find(id);
            if (cw.alive(e)) (void)cw.try_get<LocalTransform>(e);
        }
        (void)client.content_hash();

        // The same for the acknowledgement the server reads from a client.
        ByteReader ar(mutant(m, ack.data(), storage));
        acks += server.read_ack(kPeer, ar);
    }
    std::printf("fuzz: replication: %llu snapshots applied, %llu acks read of %u\n",
                static_cast<unsigned long long>(accepted), static_cast<unsigned long long>(acks), n);
    CHECK(accepted > 5);
}

// ---- rollback -------------------------------------------------------------------------------------------

namespace {

struct Game {
    u64 state = 1;
    u32 frames = 0;
    static void save(void* user, ByteWriter& out) {
        const Game& g = *static_cast<Game*>(user);
        out.write_u64(g.state);
        out.write_u32(g.frames);
    }
    static void load(void* user, Span<const u8> bytes) {
        Game& g = *static_cast<Game*>(user);
        ByteReader r(bytes);
        g.state = r.read_u64();
        g.frames = r.read_u32();
    }
    static void advance(void* user, u32, const u8* inputs, u32 input_size, u32 players) {
        Game& g = *static_cast<Game*>(user);
        for (u32 p = 0; p < players; ++p) g.state = g.state * 1099511628211ull + inputs[p * input_size] + 1;
        ++g.frames;
    }
};

struct Pair {
    Game games[2];
    RollbackSession s[2];
    explicit Pair() {
        for (u32 p = 0; p < 2; ++p) {
            RollbackDesc d;
            d.players = 2;
            d.local_player = p;
            d.input_size = 1;
            RollbackCallbacks cb;
            cb.user = &games[p];
            cb.save = &Game::save;
            cb.load = &Game::load;
            cb.advance = &Game::advance;
            REQUIRE(s[p].init(d, cb));
        }
    }
    // One tick: both add an input, advance, and exchange packets (instantly). `taken` collects 0 -> 1 packets.
    void tick(u32 f, std::vector<std::vector<u8>>* taken) {
        for (u32 p = 0; p < 2; ++p) {
            const u8 in = static_cast<u8>((f / (3 + p)) % 5);
            s[p].add_local_input(Span<const u8>(&in, 1));
            s[p].advance();
        }
        for (u32 p = 0; p < 2; ++p) {
            ByteWriter w;
            if (!s[p].write_packet(1 - p, w)) continue;
            if (p == 0 && taken) taken->emplace_back(w.data().begin(), w.data().end());
            s[1 - p].receive(w.data());
        }
    }
};

}  // namespace

TEST_CASE("fuzz: rollback packets survive mutation") {
    log::set_level(log::Level::Off);
    std::vector<std::vector<u8>> corpus;
    {
        Pair pair;
        for (u32 f = 0; f < 40; ++f) pair.tick(f, &corpus);
    }
    REQUIRE(corpus.size() > 10);
    std::vector<std::uint64_t> storage;
    fuzz::Mutator m(0xB011);
    u64 accepted = 0;
    const u32 n = iterations();
    for (u32 it = 0; it < n; ++it) {
        INFO("replay: rollback iteration " << it);
        // A session that has run `k` frames receives a mutant of a later (or earlier) packet, then goes on.
        Pair pair;
        const u32 k = 4 + it % 24;
        for (u32 f = 0; f < k; ++f) pair.tick(f, nullptr);
        const std::vector<u8>& good = corpus[(it * 7) % corpus.size()];
        const Span<const u8> bytes = mutant(m, Span<const u8>(good.data(), good.size()), storage);
        accepted += pair.s[1].receive(bytes);
        for (u32 f = k; f < k + 12; ++f) pair.tick(f, nullptr);
        (void)pair.s[1].stats();
        (void)pair.s[1].desynced();
    }
    std::printf("fuzz: rollback: %llu of %u mutated packets accepted\n", static_cast<unsigned long long>(accepted), n);
    CHECK(accepted > 5);
}

// ---- relay ----------------------------------------------------------------------------------------------

namespace {

const u8 kCreateMsg[] = {0x01, kRelayVersion, 4, 3, 4, 'd', 'u', 'e', 'l'};  // op, version, max, flags, len, mode
const u8 kQuickMsg[] = {0x06, kRelayVersion, 2, 4, 'd', 'u', 'e', 'l'};
const u8 kListMsg[] = {0x07, kRelayVersion, 0};
const u8 kLeaveMsg[] = {0x03};
const u8 kKickMsg[] = {0x05, 1};
const u8 kDataMsg[] = {0x04, 0xFF, 0, 0, 'h', 'e', 'l', 'l', 'o'};  // op, to, channel, delivery, payload

}  // namespace

TEST_CASE("fuzz: relay messages survive mutation") {
    log::set_level(log::Level::Off);
    std::vector<std::uint64_t> storage;
    fuzz::Mutator m(0x7E1A);
    const std::vector<std::vector<u8>> corpus = {
        std::vector<u8>(kCreateMsg, kCreateMsg + sizeof kCreateMsg), std::vector<u8>(kQuickMsg, kQuickMsg + sizeof kQuickMsg),
        std::vector<u8>(kListMsg, kListMsg + sizeof kListMsg),       std::vector<u8>(kLeaveMsg, kLeaveMsg + sizeof kLeaveMsg),
        std::vector<u8>(kKickMsg, kKickMsg + sizeof kKickMsg),       std::vector<u8>(kDataMsg, kDataMsg + sizeof kDataMsg),
    };
    u64 forwarded = 0, dropped = 0;
    const u32 n = iterations();
    for (u32 it = 0; it < n; ++it) {
        INFO("replay: relay iteration " << it);
        SimNetwork network(it + 1);
        SimEndpoint& relay_end = network.add_endpoint(8, 2);
        RelayServer relay;
        relay.init();
        relay.add_transport(relay_end);
        SimEndpoint& a = network.add_endpoint(1, 2);
        SimEndpoint& b = network.add_endpoint(1, 2);
        std::vector<u8> created;  // the newest message the relay sent a
        const PeerId pa = a.connect(relay_end);
        const PeerId pb = b.connect(relay_end);
        const auto pump = [&] {
            for (u32 i = 0; i < 4; ++i) {
                network.advance(10);
                relay.update();
                for (SimEndpoint* e : {&a, &b})
                    for (Event ev; e->poll(ev);)
                        if (e == &a && ev.kind == EventKind::Received) created.assign(ev.data.begin(), ev.data.end());
            }
        };
        pump();
        const auto send = [&](SimEndpoint& e, PeerId p, Span<const u8> bytes) {
            e.send(p, 0, bytes, Delivery::Reliable);
            pump();
        };
        // a opens a room, b joins it by the code and asks for the list; then both send mutants.
        send(a, pa, Span<const u8>(kCreateMsg, sizeof kCreateMsg));
        if (created.size() == 8 && created[0] == 0x81) {  // Created: member, code[6]
            u8 join[2 + kRelayCodeLength] = {0x02, kRelayVersion};
            std::memcpy(join + 2, created.data() + 2, kRelayCodeLength);
            send(b, pb, Span<const u8>(join, sizeof join));
        }
        send(b, pb, Span<const u8>(kListMsg, sizeof kListMsg));
        send(a, pa, mutant(m, Span<const u8>(corpus[it % corpus.size()].data(), corpus[it % corpus.size()].size()), storage));
        send(b, pb, mutant(m, Span<const u8>(corpus[(it / 2) % corpus.size()].data(), corpus[(it / 2) % corpus.size()].size()), storage));
        send(a, pa, Span<const u8>(kDataMsg, sizeof kDataMsg));
        forwarded += relay.stats().forwarded;
        dropped += relay.stats().dropped;
        CHECK(relay.stats().rooms <= 256);
    }
    std::printf("fuzz: relay: %llu forwarded, %llu dropped over %u runs\n", static_cast<unsigned long long>(forwarded),
                static_cast<unsigned long long>(dropped), n);
    CHECK(dropped > 10);
    CHECK(forwarded > 10);  // the room was real: the honest data message reached b
}
