// Save games (FR-RUN-065) and replays (FR-RUN-070): the [Save] reflection dump keyed by SaveKey,
// atomic files with .bak rotation, version/corruption diagnostics, and an input-stream replay
// that reproduces the live run's final state hash.

#include <doctest/doctest.h>

#include <cstring>
#include <filesystem>
#include <string>

#include "mce/ecs/save.h"
#include "mce/ecs/transform.h"
#include "mce/input/replay.h"

using namespace mce;

namespace savetest {

struct Runner {
    math::vec3 checkpoint;
    f32 best_time = 0.0f;
    u32 frames_cached = 0;  // derived, never saved
};
MCE_COMPONENT(Runner, (checkpoint, Save), (best_time, Save), frames_cached)

struct Coins {
    u32 count = 0;
};
MCE_COMPONENT(Coins, (count, Save))

}  // namespace savetest

using savetest::Coins;
using savetest::Runner;

namespace {

std::string fresh_root(const char* name) {
    const std::filesystem::path p = std::filesystem::path(MCE_TEST_TMP_DIR) / name;
    std::filesystem::remove_all(p);
    return p.generic_string();
}

}  // namespace

TEST_CASE("save: [Save] fields round-trip by SaveKey; unsaved fields keep live values") {
    World a;
    a.register_components<Runner, Coins, SaveKey, LocalTransform>();
    Entity p = a.spawn_with(SaveKey{1}, Runner{{3, 4, 0}, 12.5f, 77}, Coins{9});
    a.spawn_with(SaveKey{2}, Coins{40});
    a.spawn_with(Coins{1000});  // no SaveKey: not persisted
    (void)p;
    save::SaveInfo info;
    info.module_version = 3;
    info.scene = asset::Guid{0x1234, 0x5678};
    info.tick = 600;
    info.playtime = 10.0;
    Array<byte> bytes;
    save::write_save(a, info, bytes);

    World b;  // the "base world": key 1 exists with defaults, key 2 does not
    b.register_components<Runner, Coins, SaveKey, LocalTransform>();
    Entity q = b.spawn_with(SaveKey{1}, Runner{{0, 0, 0}, 0.0f, 5});
    save::SaveInfo got;
    auto st = save::read_save(b, bytes.span(), &got, 3);
    REQUIRE(st);
    CHECK(st.value().entities == 2);
    CHECK(st.value().spawned == 1);
    CHECK_FALSE(st.value().module_version_mismatch);
    CHECK(got.tick == 600);
    CHECK(got.scene == info.scene);
    const Runner* r = b.try_get<Runner>(q);
    REQUIRE(r);
    CHECK(r->best_time == 12.5f);
    CHECK(r->checkpoint.y == 4.0f);
    CHECK(r->frames_cached == 5);  // not a [Save] field
    CHECK(b.try_get<Coins>(q)->count == 9);  // added (absent in the base world), then written
    CHECK(b.query<const Coins&>().with<SaveKey>().count() == 2);
    CHECK(b.query<const Coins&>().count() == 2);  // the unkeyed entity was never saved
}

TEST_CASE("save: other module versions load with defaults and counts, never fail") {
    World a;
    a.register_components<Runner, Coins, SaveKey>();
    a.spawn_with(SaveKey{1}, Runner{{1, 1, 1}, 5.0f, 0}, Coins{3});
    Array<byte> bytes;
    save::write_save(a, save::SaveInfo{7, {}, 0, 0.0}, bytes);

    World b;  // this "module" does not know Coins
    b.register_components<Runner, SaveKey>();
    auto st = save::read_save(b, bytes.span(), nullptr, 8);
    REQUIRE(st);
    CHECK(st.value().module_version_mismatch);
    CHECK(st.value().unknown_components == 1);
    CHECK(st.value().spawned == 1);
}

TEST_CASE("save: corrupt, truncated and newer-format payloads are diagnosable errors") {
    World w;
    w.register_components<Runner, SaveKey>();
    w.spawn_with(SaveKey{1}, Runner{});
    Array<byte> good;
    save::write_save(w, {}, good);

    Array<byte> bad = good;
    bad[bad.size() - 1] = byte(u8(bad[bad.size() - 1]) ^ 0x5A);
    CHECK(save::read_save(w, bad.span()).error().code == Errc::CrcMismatch);
    CHECK(save::read_save(w, Span<const byte>(good.data(), good.size() - 3)).error().code == Errc::Truncated);
    Array<byte> newer = good;
    const u16 v = save::kSaveFormatVersion + 1;
    std::memcpy(newer.data() + 4, &v, sizeof(v));
    CHECK(save::read_save(w, newer.span()).error().code == Errc::VersionTooNew);
    Array<byte> magic = good;
    magic[0] = byte{0};
    CHECK(save::read_save(w, magic.span()).error().code == Errc::BadMagic);
}

TEST_CASE("save: files are written atomically with one .bak and errors carry the path") {
    save::set_root(fresh_root("saves"));
    CHECK_FALSE(save::exists("slot1"));
    World w;
    w.register_components<Runner, SaveKey>();
    const Entity e = w.spawn_with(SaveKey{1}, Runner{{0, 0, 0}, 30.0f, 0});
    REQUIRE(save::save_game(w, "slot1", {}));
    w.set(e, Runner{{0, 0, 0}, 25.0f, 0});
    REQUIRE(save::save_game(w, "slot1", {}));
    CHECK(save::exists("slot1"));
    const std::string main = save::path_of("slot1").c_str();
    CHECK(std::filesystem::exists(main + ".bak"));
    CHECK_FALSE(std::filesystem::exists(main + ".tmp"));

    World fresh;
    fresh.register_components<Runner, SaveKey>();
    REQUIRE(save::load_game(fresh, "slot1"));
    CHECK(fresh.query<const Runner&>().count() == 1);
    fresh.query<const Runner&>().each([](const Runner& r) { CHECK(r.best_time == 25.0f); });

    // A newer-format save: code + path, not a crash.
    Array<byte> bytes;
    REQUIRE(save::read("slot1", bytes));
    const u16 v = save::kSaveFormatVersion + 1;
    std::memcpy(bytes.data() + 4, &v, sizeof(v));
    REQUIRE(save::write("slot1", bytes.span()));
    auto r = save::load_game(fresh, "slot1");
    REQUIRE_FALSE(r);
    CHECK(r.error().code == Errc::VersionTooNew);
    CHECK(std::string(r.error().path.c_str()).find("slot1.mcesave") != std::string::npos);

    // A missing main file falls back to the .bak.
    std::filesystem::remove(main);
    CHECK(save::load_game(fresh, "slot1"));
    CHECK(save::load_game(fresh, "nope").error().code == Errc::FileNotFound);
}

namespace {

// A tiny deterministic "game": input moves a runner; jump presses count coins.
void build_game(World& w) {
    w.register_components<Runner, Coins, SaveKey, LocalTransform>();
    w.set_resource(input::InputState{});
    w.spawn_with(SaveKey{1}, LocalTransform{}, Coins{0});
    w.add_system(Phase::FixedUpdate_Pre, "move", [](Frame& f) {
        const input::InputState& in = f.resource<input::InputState>();
        f.query<LocalTransform&, Coins&>().each([&](LocalTransform& t, Coins& c) {
            t.position.x += in.move.x * f.dt * 3.0f;
            t.position.y += in.move.y * f.dt * 3.0f;
            if (in.jump_pressed) ++c.count;
        });
    });
}

input::InputState synth(u64 tick) {
    input::InputState s;
    s.tick = tick;
    s.move = {static_cast<f32>((tick / 17) % 3) - 1.0f, (tick % 50) < 25 ? 1.0f : -0.5f};
    s.jump_pressed = tick % 37 == 0;
    return s;
}

}  // namespace

TEST_CASE("replay: the recorded input stream reproduces the live run's state hash") {
    constexpr u64 kTicks = 3600;  // 60 s at 60 Hz
    World live;
    build_game(live);
    input::InputRecorder rec;
    save::ReplayInfo info;
    info.start_hash = live.state_hash();
    for (u64 t = 0; t < kTicks; ++t) {
        const input::InputState s = synth(t);
        rec.record(s);
        live.resource<input::InputState>() = s;
        live.tick(1.0 / 60, t);
    }
    info.tick_count = kTicks;
    info.final_hash = live.state_hash();
    Array<byte> file;
    save::write_replay(info, rec.bytes(), file);

    save::ReplayInfo got;
    auto stream = save::read_replay(file.span(), got);
    REQUIRE(stream);
    CHECK(got.tick_count == kTicks);
    World replay;
    build_game(replay);
    CHECK(replay.state_hash() == got.start_hash);
    input::InputReplayer rp(stream.value());
    u64 t = 0;
    input::InputState s;
    while (rp.next(s)) {
        replay.resource<input::InputState>() = s;
        replay.tick(1.0 / 60, t++);
    }
    CHECK(t == kTicks);
    CHECK(replay.state_hash() == got.final_hash);

    // Corruption is caught before a single tick runs.
    Array<byte> bad = file;
    bad[bad.size() / 2] = byte(u8(bad[bad.size() / 2]) ^ 0x01);
    save::ReplayInfo ignored;
    CHECK(save::read_replay(bad.span(), ignored).error().code == Errc::CrcMismatch);
}
