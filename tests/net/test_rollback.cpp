// M-NET2 step 5: deterministic rollback / lockstep with desync detection.

#include <doctest/doctest.h>

#include <cstring>

#include "mce/hash.h"
#include "mce/net/net.h"

using namespace mce;
using namespace mce::net;

namespace {

// A toy game whose state depends on every input of every frame, in order.
struct Game {
    u64 state = 1;
    u32 frames = 0;
    i64 sabotage_frame = -1;  // this peer computes something else at that frame
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
    static void advance(void* user, u32 frame, const u8* inputs, u32 input_size, u32 players) {
        Game& g = *static_cast<Game*>(user);
        for (u32 p = 0; p < players; ++p) g.state = g.state * 1099511628211ull + inputs[p * input_size] + 1;
        if (static_cast<i64>(frame) == g.sabotage_frame) g.state ^= 1;
        ++g.frames;
    }
};

// Player p's input at frame f: held for a while, then changed (so predictions sometimes fail).
u8 script(u32 p, u32 f) { return static_cast<u8>(((f / (7 + 3 * p)) * (p + 3)) % 5); }

struct Packet {
    u32 to;
    u32 due;
    Array<u8> bytes;
};

struct Net {
    u32 latency = 6;  // ticks
    u32 jitter = 0;
    u32 loss_pct = 0;
    u64 rng = 0x9E3779B97F4A7C15ull;
    Array<Packet> flight;
    u32 roll(u32 n) {
        rng ^= rng << 13;
        rng ^= rng >> 7;
        rng ^= rng << 17;
        return n ? static_cast<u32>(rng % n) : 0;
    }
};

struct Peers {
    u32 count;
    Game games[kRollbackMaxPlayers];
    RollbackSession sessions[kRollbackMaxPlayers];
    Net net;
    u32 tick = 0;

    explicit Peers(u32 n, u32 max_prediction = 8, u32 delay = 2) : count(n) {
        for (u32 p = 0; p < n; ++p) {
            RollbackDesc d;
            d.players = n;
            d.local_player = p;
            d.input_size = 1;
            d.input_delay = delay;
            d.max_prediction = max_prediction;
            REQUIRE(sessions[p].init(d, {&games[p], Game::save, Game::load, Game::advance}));
        }
    }
    void step() {
        for (u32 p = 0; p < count; ++p) {
            RollbackSession& s = sessions[p];
            const u8 in = script(p, s.frame() + s.desc().input_delay);
            s.add_local_input(Span<const u8>(&in, 1));
            s.advance();
            for (u32 q = 0; q < count; ++q) {
                if (q == p) continue;
                ByteWriter w;
                if (!s.write_packet(q, w)) continue;
                if (net.roll(100) < net.loss_pct) continue;
                Packet pk;
                pk.to = q;
                pk.due = tick + net.latency + net.roll(net.jitter + 1);
                pk.bytes.resize(w.size());
                std::memcpy(pk.bytes.data(), w.data().data(), w.size());
                net.flight.push_back(std::move(pk));
            }
        }
        ++tick;
        usize keep = 0;
        for (usize i = 0; i < net.flight.size(); ++i) {
            if (net.flight[i].due <= tick) {
                CHECK(sessions[net.flight[i].to].receive(
                    Span<const u8>(net.flight[i].bytes.data(), net.flight[i].bytes.size())));
            } else {
                if (keep != i) net.flight[keep] = std::move(net.flight[i]);
                ++keep;
            }
        }
        net.flight.resize(keep);
    }
    // Every peer's hash of every frame they both confirmed agrees; returns how many were compared.
    u32 agree() const {
        u32 compared = 0;
        i64 last = sessions[0].hashed_frame();
        for (u32 p = 1; p < count; ++p) last = std::min(last, sessions[p].hashed_frame());
        for (i64 f = std::max<i64>(0, last - 200); f <= last; ++f) {
            u64 h0 = 0;
            if (!sessions[0].confirmed_hash(static_cast<u32>(f), h0)) continue;
            for (u32 p = 1; p < count; ++p) {
                u64 h = 0;
                REQUIRE(sessions[p].confirmed_hash(static_cast<u32>(f), h));
                CHECK(h == h0);
            }
            ++compared;
        }
        return compared;
    }
};

// The game run in one place with every input known: the truth a confirmed frame must match.
u64 reference_hash(u32 players, u32 delay, u32 after_frame) {
    Game g;
    for (u32 f = 0; f <= after_frame; ++f) {
        u8 inputs[kRollbackMaxPlayers];
        for (u32 p = 0; p < players; ++p) inputs[p] = f < delay ? 0 : script(p, f);
        Game::advance(&g, f, inputs, 1, players);
    }
    ByteWriter w;
    Game::save(&g, w);
    Hasher64 h;
    h.bytes(w.data().data(), w.size());
    return h.value();
}

}  // namespace

TEST_CASE("net rollback: two peers 6 ticks apart roll back on wrong guesses and stay in sync") {
    Peers peers(2);
    for (u32 i = 0; i < 600; ++i) peers.step();
    for (u32 p = 0; p < 2; ++p) {
        const RollbackStats& s = peers.sessions[p].stats();
        CHECK(s.rollbacks > 10);
        CHECK(s.max_depth <= 8);
        CHECK(s.desyncs == 0);
        CHECK(s.hashes_compared > 100);
        CHECK_FALSE(peers.sessions[p].desynced());
        CHECK(peers.sessions[p].frame() > 550);  // hardly ever waited
    }
    CHECK(peers.agree() > 100);
    // A confirmed frame equals the game run in one place with every input known.
    const u32 f = static_cast<u32>(peers.sessions[0].hashed_frame());
    u64 h = 0;
    REQUIRE(peers.sessions[0].confirmed_hash(f, h));
    CHECK(h == reference_hash(2, 2, f));
}

TEST_CASE("net rollback: four peers over a jittery, lossy link stay in sync") {
    Peers peers(4);
    peers.net.latency = 4;
    peers.net.jitter = 3;
    peers.net.loss_pct = 10;
    for (u32 i = 0; i < 900; ++i) peers.step();
    for (u32 p = 0; p < 4; ++p) {
        CHECK_FALSE(peers.sessions[p].desynced());
        CHECK(peers.sessions[p].stats().rollbacks > 0);
    }
    CHECK(peers.agree() > 100);
    const u32 f = static_cast<u32>(peers.sessions[2].hashed_frame());
    u64 h = 0;
    REQUIRE(peers.sessions[2].confirmed_hash(f, h));
    CHECK(h == reference_hash(4, 2, f));
}

TEST_CASE("net rollback: lockstep waits instead of guessing") {
    Peers peers(2, 0);
    for (u32 i = 0; i < 300; ++i) peers.step();
    for (u32 p = 0; p < 2; ++p) {
        const RollbackStats& s = peers.sessions[p].stats();
        CHECK(s.rollbacks == 0);
        CHECK(s.predicted == 0);
        CHECK(s.stalls > 50);  // a 6-tick trip against a 2-frame input delay
    }
    CHECK(peers.agree() > 10);
}

TEST_CASE("net rollback: a desync is reported at the frame it happened, on both sides") {
    Peers peers(2);
    peers.games[1].sabotage_frame = 100;
    for (u32 i = 0; i < 300; ++i) peers.step();
    for (u32 p = 0; p < 2; ++p) {
        CHECK(peers.sessions[p].desynced());
        CHECK(peers.sessions[p].desync_frame() == 100);
        CHECK(peers.sessions[p].stats().desyncs > 0);
    }
}

TEST_CASE("net rollback: malformed packets and bad setups are refused") {
    RollbackSession s;
    Game g;
    RollbackDesc d;
    d.input_size = 1;
    d.players = 9;
    CHECK_FALSE(s.init(d, {&g, Game::save, Game::load, Game::advance}));
    d.players = 2;
    CHECK_FALSE(s.init(d, {&g, nullptr, Game::load, Game::advance}));
    REQUIRE(s.init(d, {&g, Game::save, Game::load, Game::advance}));

    RollbackSession other;
    Game g2;
    d.local_player = 1;
    REQUIRE(other.init(d, {&g2, Game::save, Game::load, Game::advance}));
    const u8 in = 3;
    other.add_local_input(Span<const u8>(&in, 1));
    other.advance();
    ByteWriter w;
    REQUIRE(other.write_packet(0, w));
    CHECK(s.receive(w.data()));
    CHECK_FALSE(other.receive(w.data()));   // its own packet
    CHECK_FALSE(other.write_packet(1, w));  // to itself
    Array<u8> bad;
    bad.resize(w.size());
    std::memcpy(bad.data(), w.data().data(), w.size());
    bad[0] = 'X';
    CHECK_FALSE(s.receive(Span<const u8>(bad.data(), bad.size())));
    bad[0] = 'M';
    CHECK_FALSE(s.receive(Span<const u8>(bad.data(), bad.size() - 1)));  // truncated
}
