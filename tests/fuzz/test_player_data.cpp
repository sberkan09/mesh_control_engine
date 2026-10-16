// M-FUZZ 3: the files a player's disk and a bug report hand to the engine: saves, replays, input recordings
// and input scripts. Corpora are written by the engine's own writers, so the mutants start from valid data.
//
// A save and a replay carry an xxh64 of their body: a byte flipped in the body is refused at the hash, so most
// mutants would never reach the parser behind it. The test therefore mutates the body and re-seals the header
// (size and hash), as a damaged-then-repaired or hand-edited file would be. It also mutates the whole file
// unsealed, which exercises the header checks.

#include <doctest/doctest.h>

#include <cstdio>
#include <cstdlib>
#include <cstring>
#include <string>
#include <vector>

#include "mce/asset/xxhash.h"
#include "mce/ecs/save.h"
#include "mce/ecs/transform.h"
#include "mce/input/replay.h"
#include "mce/input/script.h"
#include "mce/log.h"
#include "mutator.h"
#include "test_support.h"

using namespace mce;

namespace fuzzdata {

struct Runner {
    math::vec3 checkpoint;
    f32 best_time = 0.0f;
    u32 laps = 0;
    i64 score = 0;
    f64 distance = 0.0;
    bool alive = true;
    u8 lives = 3;
};
MCE_COMPONENT(Runner, (checkpoint, Save), (best_time, Save), (laps, Save), (score, Save), (distance, Save), (alive, Save),
              (lives, Save))

struct Coins {
    u32 count = 0;
};
MCE_COMPONENT(Coins, (count, Save))

}  // namespace fuzzdata

using fuzzdata::Coins;
using fuzzdata::Runner;

namespace {

constexpr usize kSaveHeader = 64, kSaveSizeAt = 48, kSaveHashAt = 56;
constexpr usize kReplayHeader = 80, kReplaySizeAt = 64, kReplayHashAt = 72;
// MCE_FUZZ_ITERS overrides the count for a long run (the default keeps the test in the task gate).
u32 iterations() {
    const char* e = std::getenv("MCE_FUZZ_ITERS");
    return e ? static_cast<u32>(std::strtoul(e, nullptr, 10)) : 600u;
}
const u32 kIterations = iterations();

template <class T>
void put_at(std::vector<byte>& b, usize at, T v) {
    std::memcpy(b.data() + at, &v, sizeof v);
}

// Mutates the body behind a `header`-byte header and writes the size and hash the header should carry.
std::vector<byte> mutate_sealed(fuzz::Mutator& m, Span<const byte> good, usize header, usize size_at, usize hash_at,
                                std::vector<std::uint64_t>& storage) {
    const Span<const byte> body(good.data() + header, good.size() - header);
    const usize n = m.mutate(body, storage);
    const auto* p = reinterpret_cast<const byte*>(storage.data());
    std::vector<byte> out(good.begin(), good.begin() + static_cast<std::ptrdiff_t>(header));
    out.insert(out.end(), p, p + n);
    put_at<u64>(out, size_at, n);
    put_at<u64>(out, hash_at, asset::xxh64(p, n));
    return out;
}

void make_world(World& w) {
    w.register_components<Runner, Coins, SaveKey, LocalTransform>();
    for (u32 i = 1; i <= 12; ++i) {
        Runner r;
        r.checkpoint = {static_cast<f32>(i), 2.f, -3.f};
        r.best_time = 10.f + static_cast<f32>(i);
        r.laps = i;
        r.score = static_cast<i64>(i) * -1000;
        r.distance = i * 1.5;
        r.alive = (i % 2) == 0;
        r.lives = static_cast<u8>(i);
        w.spawn_with(SaveKey{i}, r, Coins{i * 7});
    }
}

}  // namespace

TEST_CASE("fuzz: saves survive mutated bodies (re-sealed) and mutated files") {
    log::set_level(log::Level::Off);
    World src;
    make_world(src);
    save::SaveInfo info;
    info.module_version = 4;
    info.scene = asset::Guid{0x1234, 0x5678};
    info.tick = 900;
    info.playtime = 15.0;
    Array<byte> good;
    save::write_save(src, info, good);
    REQUIRE(good.size() > kSaveHeader);

    {  // The sealing helper reproduces a valid file from an unchanged body.
        std::vector<byte> same(good.begin(), good.end());
        put_at<u64>(same, kSaveSizeAt, good.size() - kSaveHeader);
        put_at<u64>(same, kSaveHashAt, asset::xxh64(good.data() + kSaveHeader, good.size() - kSaveHeader));
        World w;
        make_world(w);
        REQUIRE(save::read_save(w, Span<const byte>(same.data(), same.size())));
    }

    u64 accepted = 0, refused = 0;
    std::vector<std::uint64_t> storage;
    fuzz::Mutator m(0x5A5E);
    for (u32 it = 0; it < kIterations; ++it) {
        INFO("replay: save iteration " << it);
        World w;  // a fresh "base world" each time: the apply pass must leave it consistent
        w.register_components<Runner, Coins, SaveKey, LocalTransform>();
        if (it % 3 == 0) make_world(w);
        std::vector<byte> bytes;
        if (it % 4 == 3) {  // the whole file, unsealed
            const usize n = m.mutate(good.span(), storage);
            const auto* p = reinterpret_cast<const byte*>(storage.data());
            bytes.assign(p, p + n);
        } else {
            bytes = mutate_sealed(m, good.span(), kSaveHeader, kSaveSizeAt, kSaveHashAt, storage);
        }
        save::SaveInfo got;
        auto r = save::read_save(w, Span<const byte>(bytes.data(), bytes.size()), &got, 4);
        (r ? accepted : refused)++;
        if (r) {  // what the game does next: read the world it was handed
            w.query<const SaveKey&>().each([&](Entity, const SaveKey& k) { (void)k.key; });
            (void)w.state_hash();
        }
    }
    std::printf("fuzz: saves: %llu accepted, %llu refused\n", static_cast<unsigned long long>(accepted),
                static_cast<unsigned long long>(refused));
    CHECK(accepted > 50);  // the re-sealed mutants must get past the hash and exercise the parser
    CHECK(refused > 50);
}

TEST_CASE("fuzz: replays and input recordings survive mutation") {
    log::set_level(log::Level::Off);
    // A recording of a script that touches every field the recorder writes.
    input::InputScript script;
    REQUIRE(script.parse("1-30 Move=1,0 Jump\n10 Fire\n20-60 P2/Move=-1,0.5 Look=0.25,0.25\n"
                         "40 Touch=0.5,0.9\n50 Mouse=640,360 Point\n55-80 Confirm Cancel Pause\n"));
    input::InputRecorder rec;
    for (u64 t = 1; t <= 90; ++t) rec.record(script.evaluate(t));
    const Span<const byte> stream = rec.bytes();
    REQUIRE(!stream.empty());

    save::ReplayInfo info;
    info.module_version = 2;
    info.scene = asset::Guid{9, 10};
    info.start_tick = 1;
    info.tick_count = rec.ticks();
    info.start_hash = 0xABCD;
    info.final_hash = 0xEF01;
    Array<byte> good;
    save::write_replay(info, stream, good);
    save::ReplayInfo back;
    REQUIRE(save::read_replay(good.span(), back));

    u64 accepted = 0, decoded = 0;
    std::vector<std::uint64_t> storage;
    fuzz::Mutator m(0x2E91A7);
    for (u32 it = 0; it < kIterations; ++it) {
        INFO("replay: replay iteration " << it);
        std::vector<byte> bytes;
        if (it % 4 == 3) {
            const usize n = m.mutate(good.span(), storage);
            const auto* p = reinterpret_cast<const byte*>(storage.data());
            bytes.assign(p, p + n);
        } else {
            bytes = mutate_sealed(m, good.span(), kReplayHeader, kReplaySizeAt, kReplayHashAt, storage);
        }
        save::ReplayInfo got;
        auto r = save::read_replay(Span<const byte>(bytes.data(), bytes.size()), got);
        if (!r) continue;
        ++accepted;
        // The stream is what the replayer decodes tick by tick, whatever it holds.
        input::InputReplayer rp(r.value());
        input::InputState s;
        u32 guard = 0;
        while (!rp.done() && guard++ < 100000) {
            if (!rp.next(s)) break;
            ++decoded;
            (void)s.action(StringId("Move"));
            (void)s.text();
            (void)s.touch_points();
        }
    }
    // And the raw stream, with no replay header around it (a recording kept on its own).
    for (u32 it = 0; it < kIterations; ++it) {
        INFO("replay: recording iteration " << it);
        const usize n = m.mutate(stream, storage);
        const auto* p = reinterpret_cast<const byte*>(storage.data());
        input::InputReplayer rp(Span<const byte>(p, n));
        input::InputState s;
        u32 guard = 0;
        while (!rp.done() && guard++ < 100000) {
            if (!rp.next(s)) break;
            ++decoded;
            (void)s.action(StringId("Jump"));
            (void)s.text();
            (void)s.touch_points();
        }
    }
    std::printf("fuzz: replays: %llu accepted, %llu snapshots decoded\n", static_cast<unsigned long long>(accepted),
                static_cast<unsigned long long>(decoded));
    CHECK(accepted > 50);
    CHECK(decoded > 1000);
}

TEST_CASE("fuzz: input scripts parse or refuse, and evaluate whatever they hold") {
    log::set_level(log::Level::Off);
    const std::string good =
        "# a script with every form\n"
        "10-70 Move=1,0 Jump\n"
        "100 Fire\n"
        "120- Move=-1,0\n"
        "130 Touch=0.5,0.9\n"
        "140 Mouse=640,360 Point\n"
        "150-160 Quality=0 Sprint=true Crouch=false\n"
        "170 P2/Move=0.5,0.5 P3/Jump P4/Fire\n"
        "180-190 Look=-0.25,0.75 Confirm=on Cancel=off\n";
    input::InputScript base;
    REQUIRE(base.parse(good));
    std::vector<std::uint64_t> storage;
    fuzz::Mutator m(0x77EE);
    u64 accepted = 0;
    const Span<const byte> bytes(reinterpret_cast<const byte*>(good.data()), good.size());
    for (u32 it = 0; it < kIterations * 2; ++it) {
        INFO("replay: script iteration " << it);
        const usize n = m.mutate(bytes, storage);
        const std::string text(reinterpret_cast<const char*>(storage.data()), n);
        input::InputScript s;
        if (!s.parse(text)) continue;
        ++accepted;
        for (u64 t : {u64(1), u64(10), u64(11), u64(130), u64(131), u64(1000), ~u64(0) - 1}) (void)s.evaluate(t);
        (void)s.last_tick();
        for (const auto& seg : s.segments()) CHECK(seg.from <= seg.to);
    }
    std::printf("fuzz: scripts: %llu parsed\n", static_cast<unsigned long long>(accepted));
    CHECK(accepted > 20);
}
