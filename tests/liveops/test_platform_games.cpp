// M-LIVEOPS step 2: Game Center / Play Games behind PlatformGameServices. The store ids come from
// the catalog for the platform at hand; cloud saves keep their version inside the blob, so a second
// device saving over an older version gets a conflict; another app's blob is never overwritten.

#include <doctest/doctest.h>

#include <map>
#include <string>
#include <vector>

#include "mce/liveops/platform_games.h"

using namespace mce;
using namespace mce::liveops;

namespace {

Span<const byte> as_bytes(const std::string& s) {
    return Span<const byte>(reinterpret_cast<const byte*>(s.data()), s.size());
}

std::string as_string(Span<const byte> b) { return std::string(reinterpret_cast<const char*>(b.data()), b.size()); }

ServicesCatalog catalog() {
    ServicesCatalog c;
    AchievementInfo a;
    a.id = "first_win";
    a.ios_id = "com.studio.first_win";
    a.android_id = "CgkIfirst";
    REQUIRE(c.add_achievement(a));
    a = AchievementInfo{};
    a.id = "win_10";
    a.steps = 10;
    a.ios_id = "com.studio.win_10";  // no Android id
    REQUIRE(c.add_achievement(a));
    LeaderboardInfo l;
    l.id = "best_time";
    l.ios_id = "com.studio.best_time";
    l.android_id = "CgkIbest";
    REQUIRE(c.add_leaderboard(l));
    REQUIRE(c.add_cloud_save("progress"));
    return c;
}

// The platform: one cloud shared by every device, answering at once.
struct Cloud {
    std::map<std::string, std::string> blobs;
    bool offline = false;
    bool write_fails = false;
};

struct FakePlatform final : IPlatformGamesBackend {
    Cloud& cloud;
    bool ios;
    bool can_start = true;
    PlatformGamesSink* sink = nullptr;
    std::vector<std::string> calls;

    FakePlatform(Cloud& c, bool ios_ids) : cloud(c), ios(ios_ids) {}
    const char* name() const override { return ios ? "game_center" : "play_games"; }
    bool ios_ids() const override { return ios; }
    bool start(PlatformGamesSink& s) override {
        sink = &s;
        return can_start;
    }
    void sign_in(bool) override { sink->platform_signed_in(!cloud.offline); }
    void report_achievement(StringView id, u32 steps, u32 total, u16 tag) override {
        calls.push_back("achievement " + std::string(id) + " " + std::to_string(steps) + "/" + std::to_string(total));
        sink->platform_achievement(tag, !cloud.offline);
    }
    void submit_score(StringView id, i64 score, u16 tag) override {
        calls.push_back("score " + std::string(id) + " " + std::to_string(score));
        sink->platform_score(tag, !cloud.offline);
    }
    void read_blob(StringView name, u16 tag) override {
        calls.push_back("read " + std::string(name));
        if (cloud.offline) return sink->platform_blob_read(tag, BlobRead::Failed, {});
        auto it = cloud.blobs.find(std::string(name));
        if (it == cloud.blobs.end()) return sink->platform_blob_read(tag, BlobRead::Missing, {});
        sink->platform_blob_read(tag, BlobRead::Ok, as_bytes(it->second));
    }
    void write_blob(StringView name, Span<const byte> blob, u16 tag) override {
        calls.push_back("write " + std::string(name));
        if (cloud.offline || cloud.write_fails) return sink->platform_blob_written(tag, false);
        cloud.blobs[std::string(name)] = as_string(blob);
        sink->platform_blob_written(tag, true);
    }
};

struct Seen final : GameServicesSink {
    int signed_in_ok = -1;
    std::vector<std::pair<u16, bool>> achievements, scores;
    CloudResult saved = CloudResult::Failed;
    u64 saved_version = 99;
    CloudResult loaded = CloudResult::Failed;
    u64 loaded_version = 99;
    std::string loaded_bytes;
    int answers = 0;

    void signed_in(bool ok) override { signed_in_ok = ok ? 1 : 0; }
    void achievement_sent(u16 a, bool ok) override { achievements.emplace_back(a, ok); }
    void score_sent(u16 l, bool ok) override { scores.emplace_back(l, ok); }
    void cloud_saved(u16, CloudResult r, u64 v) override {
        saved = r;
        saved_version = v;
        ++answers;
    }
    void cloud_loaded(u16, CloudResult r, u64 v, Span<const byte> b) override {
        loaded = r;
        loaded_version = v;
        loaded_bytes = as_string(b);
        ++answers;
    }
};

}  // namespace

TEST_CASE("platform games: a cloud blob carries its version and rejects foreign data") {
    Array<byte> blob;
    encode_cloud_blob(7, as_bytes("level=3"), blob);
    u64 v = 0;
    Span<const byte> bytes;
    REQUIRE(decode_cloud_blob(blob.span(), v, bytes));
    CHECK(v == 7);
    CHECK(as_string(bytes) == "level=3");
    encode_cloud_blob(1, {}, blob);
    REQUIRE(decode_cloud_blob(blob.span(), v, bytes));
    CHECK(v == 1);
    CHECK(bytes.empty());
    CHECK_FALSE(decode_cloud_blob(as_bytes("{\"another\":\"app\"}"), v, bytes));
    CHECK_FALSE(decode_cloud_blob(as_bytes("MCGS"), v, bytes));
    blob[4] = byte{9};  // an unknown format
    CHECK_FALSE(decode_cloud_blob(blob.span(), v, bytes));
}

TEST_CASE("platform games: achievements and scores go out under the platform's own ids") {
    const ServicesCatalog c = catalog();
    Cloud cloud;
    for (const bool ios : {true, false}) {
        FakePlatform platform(cloud, ios);
        PlatformGameServices provider(platform);
        Seen seen;
        provider.start(c, seen);
        provider.sign_in(false);
        CHECK(seen.signed_in_ok == 1);
        provider.report_achievement(0, 1);
        provider.report_achievement(1, 14);  // capped at the achievement's 10 steps
        provider.submit_score(0, 4200);
        if (ios) {
            REQUIRE(platform.calls.size() == 3);
            CHECK(platform.calls[0] == "achievement com.studio.first_win 1/1");
            CHECK(platform.calls[1] == "achievement com.studio.win_10 10/10");
            CHECK(platform.calls[2] == "score com.studio.best_time 4200");
            CHECK(seen.achievements == std::vector<std::pair<u16, bool>>{{0, true}, {1, true}});
        } else {
            // win_10 has no Android id: it fails here (and stays in the outbox) without a platform call.
            REQUIRE(platform.calls.size() == 2);
            CHECK(platform.calls[0] == "achievement CgkIfirst 1/1");
            CHECK(platform.calls[1] == "score CgkIbest 4200");
            CHECK(seen.achievements == std::vector<std::pair<u16, bool>>{{0, true}, {1, false}});
        }
        CHECK(seen.scores == std::vector<std::pair<u16, bool>>{{0, true}});
    }
}

TEST_CASE("platform games: cloud saves are versioned across devices") {
    const ServicesCatalog c = catalog();
    Cloud cloud;
    FakePlatform phone_a(cloud, true), phone_b(cloud, true);
    PlatformGameServices a(phone_a), b(phone_b);
    Seen seen_a, seen_b;
    a.start(c, seen_a);
    b.start(c, seen_b);

    // Nothing in the cloud yet: version 0, no bytes.
    a.cloud_load(0);
    CHECK(seen_a.loaded == CloudResult::Ok);
    CHECK(seen_a.loaded_version == 0);
    CHECK(seen_a.loaded_bytes.empty());

    a.cloud_save(0, 0, as_bytes("a1"));
    CHECK(seen_a.saved == CloudResult::Ok);
    CHECK(seen_a.saved_version == 1);
    a.cloud_save(0, 1, as_bytes("a2"));
    CHECK(seen_a.saved_version == 2);

    // B still builds on version 0: a conflict naming the cloud's version, and the cloud is unchanged.
    b.cloud_save(0, 0, as_bytes("b1"));
    CHECK(seen_b.saved == CloudResult::Conflict);
    CHECK(seen_b.saved_version == 2);
    b.cloud_load(0);
    CHECK(seen_b.loaded == CloudResult::Ok);
    CHECK(seen_b.loaded_version == 2);
    CHECK(seen_b.loaded_bytes == "a2");
    b.cloud_save(0, 2, as_bytes("b3"));
    CHECK(seen_b.saved == CloudResult::Ok);
    CHECK(seen_b.saved_version == 3);
    a.cloud_load(0);
    CHECK(seen_a.loaded_bytes == "b3");

    // Offline or a failed write: Failed, nothing changes.
    cloud.offline = true;
    a.cloud_save(0, 3, as_bytes("x"));
    CHECK(seen_a.saved == CloudResult::Failed);
    a.cloud_load(0);
    CHECK(seen_a.loaded == CloudResult::Failed);
    cloud.offline = false;
    cloud.write_fails = true;
    a.cloud_save(0, 3, as_bytes("x"));
    CHECK(seen_a.saved == CloudResult::Failed);
    cloud.write_fails = false;
    b.cloud_load(0);
    CHECK(seen_b.loaded_bytes == "b3");
}

TEST_CASE("platform games: another app's saved game is never overwritten") {
    const ServicesCatalog c = catalog();
    Cloud cloud;
    cloud.blobs["progress"] = "{\"saved by\":\"an older build\"}";
    FakePlatform phone(cloud, false);
    PlatformGameServices p(phone);
    Seen seen;
    p.start(c, seen);
    p.cloud_load(0);
    CHECK(seen.loaded == CloudResult::Failed);
    p.cloud_save(0, 0, as_bytes("mine"));
    CHECK(seen.saved == CloudResult::Failed);
    CHECK(cloud.blobs["progress"] == "{\"saved by\":\"an older build\"}");
    for (const std::string& call : phone.calls) CHECK(call.rfind("write", 0) != 0);
}

TEST_CASE("platform games: a platform that does not start fails every call without reaching it") {
    const ServicesCatalog c = catalog();
    Cloud cloud;
    FakePlatform phone(cloud, true);
    phone.can_start = false;
    PlatformGameServices p(phone);
    Seen seen;
    p.start(c, seen);
    p.sign_in(true);
    CHECK(seen.signed_in_ok == 0);
    p.report_achievement(0, 1);
    p.submit_score(0, 1);
    p.cloud_save(0, 0, as_bytes("x"));
    p.cloud_load(0);
    CHECK(seen.achievements == std::vector<std::pair<u16, bool>>{{0, false}});
    CHECK(seen.scores == std::vector<std::pair<u16, bool>>{{0, false}});
    CHECK(seen.saved == CloudResult::Failed);
    CHECK(seen.loaded == CloudResult::Failed);
    CHECK(phone.calls.empty());
}
