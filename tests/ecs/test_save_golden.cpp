// Save and replay format pins (M-SAVECOMPAT): a v1 payload frozen as bytes. Players keep their save files across
// game updates, so a change to the layout must either keep these bytes loading or bump the format version; a
// silent layout change fails here instead of on a player's disk.

#include <doctest/doctest.h>

#include <cstdio>
#include <string>

#include "mce/ecs/save.h"
#include "mce/ecs/transform.h"

using namespace mce;

namespace savegolden {

struct GoldenRunner {
    math::vec3 checkpoint;
    f32 best_time = 0.0f;
};
MCE_COMPONENT(GoldenRunner, (checkpoint, Save), (best_time, Save))

struct GoldenCoins {
    u32 count = 0;
};
MCE_COMPONENT(GoldenCoins, (count, Save))

}  // namespace savegolden

using savegolden::GoldenCoins;
using savegolden::GoldenRunner;

namespace {

// Frozen output of write_save for the world below at kSaveFormatVersion 1.
const u8 kGoldenSave[] = {
    0x4d,0x43,0x53,0x56,0x01,0x00,0x00,0x00,0x02,0x00,0x00,0x00,0x02,0x00,0x00,0x00,
    0x11,0x11,0x00,0x00,0x00,0x00,0x00,0x00,0x22,0x22,0x00,0x00,0x00,0x00,0x00,0x00,
    0x58,0x02,0x00,0x00,0x00,0x00,0x00,0x00,0x00,0x00,0x00,0x00,0x00,0x00,0x29,0x40,
    0x63,0x00,0x00,0x00,0x00,0x00,0x00,0x00,0x11,0x86,0x9b,0x0e,0xd2,0x03,0x8d,0x22,
    0x07,0x00,0x00,0x00,0x01,0x00,0x00,0x00,0x4e,0x3c,0xe3,0x51,0xfe,0xef,0x15,0xc5,
    0x02,0x00,0x00,0x00,0x9f,0x82,0x8d,0xaf,0x46,0x12,0xf3,0xc8,0x0e,0x0c,0x00,0x00,
    0x00,0x00,0x00,0x80,0x3f,0x00,0x00,0x00,0x40,0x00,0x00,0x40,0x40,0x81,0x93,0x7c,
    0xb6,0x27,0x41,0x54,0x6d,0x09,0x04,0x00,0x00,0x00,0x00,0x00,0x90,0x40,0x09,0x00,
    0x00,0x00,0x01,0x00,0x00,0x00,0x56,0x22,0x59,0x1e,0xf0,0x90,0x1a,0xdb,0x01,0x00,
    0x00,0x00,0x74,0xa2,0x79,0x44,0x8e,0xe2,0xe5,0xb1,0x07,0x04,0x00,0x00,0x00,0x2a,
    0x00,0x00,0x00,
};

// Frozen output of write_replay for kReplayInfo / kReplayStream at kReplayFormatVersion 1.
const u8 kGoldenReplay[] = {
    0x4d,0x43,0x52,0x50,0x01,0x00,0x00,0x00,0x03,0x00,0x00,0x00,0x00,0x00,0x00,0x00,
    0x11,0x11,0x00,0x00,0x00,0x00,0x00,0x00,0x22,0x22,0x00,0x00,0x00,0x00,0x00,0x00,
    0x0a,0x00,0x00,0x00,0x00,0x00,0x00,0x00,0x58,0x02,0x00,0x00,0x00,0x00,0x00,0x00,
    0xaa,0xaa,0x00,0x00,0x00,0x00,0x00,0x00,0xbb,0xbb,0x00,0x00,0x00,0x00,0x00,0x00,
    0x0a,0x00,0x00,0x00,0x00,0x00,0x00,0x00,0xd8,0x84,0x9f,0x71,0x7d,0x1a,0xbe,0xc6,
    0x01,0x02,0x03,0x04,0x05,0x06,0x07,0x08,0x09,0x0a,
};

const u8 kReplayStream[] = {1, 2, 3, 4, 5, 6, 7, 8, 9, 10};

save::ReplayInfo replay_info() {
    save::ReplayInfo i;
    i.module_version = 3;
    i.scene = asset::Guid{0x1111, 0x2222};
    i.start_tick = 10;
    i.tick_count = 600;
    i.start_hash = 0xAAAA;
    i.final_hash = 0xBBBB;
    return i;
}

save::SaveInfo save_info() { return save::SaveInfo{2, asset::Guid{0x1111, 0x2222}, 600, 12.5}; }

void build_world(World& w) {
    w.register_components<GoldenRunner, GoldenCoins, SaveKey>();
    w.spawn_with(SaveKey{7}, GoldenRunner{{1.0f, 2.0f, 3.0f}, 4.5f});
    w.spawn_with(SaveKey{9}, GoldenCoins{42});
}

std::string hex(const Array<byte>& b) {
    std::string s;
    char tmp[8];
    for (usize i = 0; i < b.size(); ++i) {
        std::snprintf(tmp, sizeof(tmp), "0x%02x,", static_cast<unsigned>(u8(b[i])));
        s += tmp;
        if (i % 16 == 15) s += '\n';
    }
    return s;
}

}  // namespace

TEST_CASE("save golden: write_save still produces the frozen v1 bytes") {
    World w;
    build_world(w);
    Array<byte> out;
    save::write_save(w, save_info(), out);
    INFO("actual bytes:\n" << hex(out));
    REQUIRE(out.size() == sizeof(kGoldenSave));
    CHECK(std::memcmp(out.data(), kGoldenSave, sizeof(kGoldenSave)) == 0);
}

TEST_CASE("save golden: the frozen v1 save loads into a fresh world with its values") {
    World w;
    w.register_components<GoldenRunner, GoldenCoins, SaveKey>();
    save::SaveInfo got;
    auto st = save::read_save(w, Span<const byte>(reinterpret_cast<const byte*>(kGoldenSave), sizeof(kGoldenSave)), &got, 2);
    REQUIRE(st);
    CHECK(st.value().entities == 2);
    CHECK(st.value().spawned == 2);
    CHECK(st.value().unknown_components == 0);
    CHECK(st.value().unknown_fields == 0);
    CHECK(st.value().mismatched_fields == 0);
    CHECK_FALSE(st.value().module_version_mismatch);
    CHECK(got.tick == 600);
    CHECK(got.playtime == 12.5);
    CHECK(got.scene == asset::Guid{0x1111, 0x2222});

    bool saw_runner = false, saw_coins = false;
    w.query<const SaveKey&>().each([&](Entity e, const SaveKey& k) {
        if (k.key == 7) {
            const GoldenRunner* r = w.try_get<GoldenRunner>(e);
            REQUIRE(r);
            CHECK(r->checkpoint.x == 1.0f);
            CHECK(r->checkpoint.y == 2.0f);
            CHECK(r->checkpoint.z == 3.0f);
            CHECK(r->best_time == 4.5f);
            saw_runner = true;
        } else if (k.key == 9) {
            const GoldenCoins* c = w.try_get<GoldenCoins>(e);
            REQUIRE(c);
            CHECK(c->count == 42);
            saw_coins = true;
        }
    });
    CHECK(saw_runner);
    CHECK(saw_coins);
}

TEST_CASE("save golden: write_replay still produces the frozen v1 bytes, and they read back") {
    Array<byte> out;
    save::write_replay(replay_info(), Span<const byte>(reinterpret_cast<const byte*>(kReplayStream), sizeof(kReplayStream)), out);
    INFO("actual bytes:\n" << hex(out));
    REQUIRE(out.size() == sizeof(kGoldenReplay));
    CHECK(std::memcmp(out.data(), kGoldenReplay, sizeof(kGoldenReplay)) == 0);

    save::ReplayInfo got;
    auto stream = save::read_replay(Span<const byte>(reinterpret_cast<const byte*>(kGoldenReplay), sizeof(kGoldenReplay)), got);
    REQUIRE(stream);
    CHECK(got.module_version == 3);
    CHECK(got.start_tick == 10);
    CHECK(got.tick_count == 600);
    CHECK(got.start_hash == 0xAAAA);
    CHECK(got.final_hash == 0xBBBB);
    REQUIRE(stream.value().size() == sizeof(kReplayStream));
    CHECK(std::memcmp(stream.value().data(), kReplayStream, sizeof(kReplayStream)) == 0);
}
