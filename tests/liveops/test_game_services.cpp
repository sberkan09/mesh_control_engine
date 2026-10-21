// M-LIVEOPS step 2: game services. The catalog chunk round-trips and rejects damage; achievements
// and scores count on this device at once and wait in a saved outbox until the platform takes them;
// cloud saves are versioned and a two-device conflict is settled by the policy or the game; replays
// hold with no provider and no file writes.

#include <doctest/doctest.h>

#include <filesystem>
#include <fstream>
#include <sstream>
#include <string>

#include "mce/ecs/save.h"
#include "mce/liveops/game_services.h"

using namespace mce;
using namespace mce::liveops;
namespace fs = std::filesystem;

namespace {

fs::path fresh(const char* name) {
    const fs::path p = fs::path(MCE_TEST_TMP_DIR) / name;
    std::error_code ec;
    fs::remove_all(p, ec);
    fs::create_directories(p, ec);
    return p;
}

std::string read_text(const fs::path& p) {
    std::ifstream f(p, std::ios::binary);
    std::stringstream ss;
    ss << f.rdbuf();
    return ss.str();
}

void use_device(const fs::path& root) { save::set_root(StringView(root.string().c_str())); }

void write_save(const char* name, const std::string& text) {
    REQUIRE(save::write(name, Span<const byte>(reinterpret_cast<const byte*>(text.data()), text.size())));
}

std::string read_save(const char* name) {
    Array<byte> b;
    if (!save::exists(name) || !save::read(name, b)) return "<none>";
    return std::string(reinterpret_cast<const char*>(b.data()), b.size());
}

ServicesCatalog catalog() {
    ServicesCatalog c;
    AchievementInfo a;
    a.id = "first_win";
    a.ios_id = "com.studio.first_win";
    a.android_id = "CgkIabc";
    a.steam_id = "ACH_FIRST_WIN";
    REQUIRE(c.add_achievement(a));
    a = AchievementInfo{};
    a.id = "win_10";
    a.steps = 10;
    REQUIRE(c.add_achievement(a));
    LeaderboardInfo l;
    l.id = "best_time";
    l.lower_is_better = true;
    REQUIRE(c.add_leaderboard(l));
    l = LeaderboardInfo{};
    l.id = "score";
    REQUIRE(c.add_leaderboard(l));
    REQUIRE(c.add_cloud_save("progress"));
    return c;
}

// What the game saw, in the world (so it is in the state hash).
struct GsSeen {
    u32 signed_in = 0;
    u32 sent = 0;
    u32 saved = 0;
    u32 loaded = 0;
    u32 current = 0;
    u32 conflicts = 0;
    u32 failed = 0;
    u32 pad_ = 0;
    i64 last_value = 0;
};

void game(Frame& f) {
    f.query<GsSeen&>().each([&](GsSeen& s) {
        for (const GameServicesEvent& e : f.events().read<GameServicesEvent>()) {
            s.last_value = e.value;
            switch (e.kind) {
                case GameServicesEventKind::SignedIn:
                    ++s.signed_in;
                    break;
                case GameServicesEventKind::AchievementSent:
                case GameServicesEventKind::ScoreSent:
                    ++s.sent;
                    break;
                case GameServicesEventKind::CloudSaved:
                    ++s.saved;
                    break;
                case GameServicesEventKind::CloudLoaded:
                    ++s.loaded;
                    break;
                case GameServicesEventKind::CloudCurrent:
                    ++s.current;
                    break;
                case GameServicesEventKind::CloudConflict:
                    ++s.conflicts;
                    break;
                case GameServicesEventKind::Failed:
                    ++s.failed;
                    break;
                default:
                    break;
            }
        }
    });
}

struct Device {
    World world;
    GameServicesService service;
    u64 tick = 0;
    void start(IGameServicesProvider* p, const GameServicesOptions& opt = {}) {
        service.init(catalog(), p, opt);
        service.install(world);
        world.add_system(Phase::FixedUpdate_Post, "game", &game);
        world.spawn_with(GsSeen{});
    }
    void step(int n = 1) {
        for (int i = 0; i < n; ++i) {
            service.update();
            world.tick(1.0 / 60, ++tick);
        }
    }
    GsSeen seen() {
        GsSeen out;
        world.query<GsSeen&>().each([&](GsSeen& s) { out = s; });
        return out;
    }
    GameServices& gs() { return world.resource<GameServices>(); }
};

}  // namespace

TEST_CASE("game services: the catalog chunk round-trips and rejects damage") {
    const ServicesCatalog c = catalog();
    Array<byte> bytes;
    encode_services_catalog(c, bytes);
    ServicesCatalog back;
    REQUIRE(decode_services_catalog(bytes.span(), back));
    REQUIRE(back.achievements().size() == 2);
    CHECK(back.achievements()[0].android_id.view() == "CgkIabc");
    CHECK(back.achievements()[0].steam_id.view() == "ACH_FIRST_WIN");
    CHECK(back.achievements()[1].steps == 10);
    CHECK(back.leaderboards()[0].lower_is_better);
    CHECK_FALSE(back.leaderboards()[1].lower_is_better);
    CHECK(back.find_cloud_save("progress") == 0);
    for (usize cut = 0; cut < bytes.size(); ++cut) {
        ServicesCatalog t;
        CHECK_FALSE(decode_services_catalog(Span<const byte>(bytes.data(), cut), t));
    }
    Array<byte> bad = bytes;
    bad[20] = byte{1};  // header reserved
    ServicesCatalog t;
    CHECK_FALSE(decode_services_catalog(bad.span(), t));

    ServicesCatalog dup;
    AchievementInfo a;
    a.id = "x";
    CHECK(dup.add_achievement(a));
    CHECK_FALSE(dup.add_achievement(a));
    a.id = "y";
    a.steps = 0;
    CHECK_FALSE(dup.add_achievement(a));
    CHECK_FALSE(dup.add_cloud_save(""));
}

TEST_CASE("game services: offline first, sent from the outbox on a later launch") {
    const fs::path root = fresh("gs_outbox");
    const fs::path platform = root / "platform";
    use_device(root / "device");
    LocalGameServices local(StringView(platform.string().c_str()));
    local.set_offline(true);
    {
        Device d;
        GameServicesOptions opt;
        opt.retry_updates = 5;
        d.start(&local, opt);
        d.step(3);
        CHECK_FALSE(d.gs().signed_in());
        CHECK(d.gs().unlock("first_win"));
        CHECK_FALSE(d.gs().unlock("first_win"));  // already
        CHECK_FALSE(d.gs().unlock("nope"));
        CHECK(d.gs().add_progress("win_10", 4));
        CHECK(d.gs().add_progress("win_10", 40));  // caps at 10
        CHECK_FALSE(d.gs().add_progress("win_10", 1));
        CHECK(d.gs().unlocked("win_10"));
        CHECK(d.gs().submit_score("best_time", 4200));
        CHECK(d.gs().submit_score("best_time", 5000));  // worse: kept 4200
        CHECK(d.gs().best_score("best_time", -1) == 4200);
        CHECK(d.gs().best_score("score", -1) == -1);
        CHECK(d.gs().pending() == 3);
        CHECK(d.gs().cloud_save("progress") != 0);
        d.step(20);  // retries keep failing
        CHECK(d.gs().pending() == 3);
        CHECK(d.seen().failed == 1);  // the cloud save: not signed in
        CHECK_FALSE(fs::exists(platform / "game_services.jsonl"));
    }
    local.set_offline(false);
    {
        Device d;
        d.start(&local);
        CHECK(d.gs().unlocked("first_win"));  // from the saved state
        CHECK(d.gs().progress("win_10") == 10);
        CHECK(d.gs().best_score("best_time", -1) == 4200);
        CHECK(d.gs().pending() == 3);
        d.step(10);
        CHECK(d.gs().signed_in());
        CHECK(d.gs().pending() == 0);
        CHECK(d.seen().sent == 3);
    }
    const std::string log = read_text(platform / "game_services.jsonl");
    CHECK(log.find("{\"achievement\":\"first_win\",\"steps\":1}\n") != std::string::npos);
    CHECK(log.find("{\"achievement\":\"win_10\",\"steps\":10}\n") != std::string::npos);
    CHECK(log.find("{\"leaderboard\":\"best_time\",\"score\":4200}\n") != std::string::npos);
    {
        Device d;
        d.start(&local);
        d.step(5);
        CHECK(d.gs().pending() == 0);  // nothing is sent twice
        CHECK(d.seen().sent == 0);
        CHECK(d.gs().submit_score("best_time", 3900));
        d.step(2);
        CHECK(d.seen().sent == 1);
        CHECK(d.seen().last_value == 3900);
    }
}

TEST_CASE("game services: a failed sign-in is retried silently") {
    const fs::path root = fresh("gs_signin");
    use_device(root / "device");
    LocalGameServices local(StringView((root / "platform").string().c_str()));
    local.set_offline(true);
    Device d;
    GameServicesOptions opt;
    opt.retry_updates = 4;
    d.start(&local, opt);
    d.step(2);
    CHECK_FALSE(d.gs().signed_in());
    local.set_offline(false);
    d.step(6);
    CHECK(d.gs().signed_in());
    CHECK(d.seen().signed_in == 1);
}

TEST_CASE("game services: cloud saves move between devices and conflicts are settled") {
    const fs::path root = fresh("gs_cloud");
    LocalGameServices cloud(StringView((root / "platform").string().c_str()));

    // Device A uploads its progress (version 1).
    use_device(root / "a");
    {
        Device a;
        a.start(&cloud);
        a.step(2);
        write_save("progress", "level 3");
        a.gs().cloud_save("progress");
        a.step(2);
        CHECK(a.seen().saved == 1);
        CHECK(a.seen().last_value == 1);
    }

    // Device B, a fresh install, downloads it, plays on and uploads version 2.
    use_device(root / "b");
    {
        Device b;
        b.start(&cloud);
        b.step(2);
        b.gs().cloud_load("progress");
        b.step(2);
        CHECK(b.seen().loaded == 1);
        CHECK(read_save("progress") == "level 3");
        b.gs().cloud_load("progress");
        b.step(2);
        CHECK(b.seen().current == 1);  // nothing newer
        write_save("progress", "level 5 (b)");
        b.gs().cloud_save("progress");
        b.step(2);
        CHECK(b.seen().saved == 1);
        CHECK(b.seen().last_value == 2);
    }

    // Device A played offline meanwhile: its upload conflicts; the game keeps the cloud's.
    use_device(root / "a");
    {
        Device a;
        a.start(&cloud);
        a.step(2);
        write_save("progress", "level 4 (a)");
        a.gs().cloud_save("progress");
        a.step(3);
        CHECK(a.seen().conflicts == 1);
        CHECK(a.seen().last_value == 2);
        CHECK(read_save("progress") == "level 4 (a)");  // nothing changed yet
        CHECK(a.gs().resolve_conflict("progress", false));
        CHECK_FALSE(a.gs().resolve_conflict("progress", false));
        a.step(1);
        CHECK(a.seen().loaded == 1);
        CHECK(read_save("progress") == "level 5 (b)");
    }

    // Device B played on too; with PreferDevice its save goes over the cloud's without asking.
    use_device(root / "b");
    write_save("progress", "level 6 (b)");
    use_device(root / "a");
    write_save("progress", "level 7 (a)");
    {
        Device a;
        a.start(&cloud);
        a.step(2);
        a.gs().cloud_save("progress");
        a.step(2);
        CHECK(a.seen().saved == 1);  // A was in sync at version 2
        CHECK(a.seen().last_value == 3);
    }
    use_device(root / "b");
    {
        Device b;
        GameServicesOptions opt;
        opt.conflict = ConflictPolicy::PreferDevice;
        b.start(&cloud, opt);
        b.step(2);
        b.gs().cloud_load("progress");
        b.step(4);
        CHECK(b.seen().conflicts == 0);
        CHECK(b.seen().saved == 1);
        CHECK(b.seen().last_value == 4);
        CHECK(read_save("progress") == "level 6 (b)");
    }
    use_device(root / "a");
    {
        Device a;
        GameServicesOptions opt;
        opt.conflict = ConflictPolicy::PreferCloud;
        a.start(&cloud, opt);
        a.step(2);
        write_save("progress", "level 8 (a)");
        a.gs().cloud_load("progress");
        a.step(2);
        CHECK(a.seen().loaded == 1);
        CHECK(read_save("progress") == "level 6 (b)");
    }
}

TEST_CASE("game services: replays hold with no provider and no file writes") {
    const fs::path root = fresh("gs_replay");
    use_device(root / "live");
    LocalGameServices local(StringView((root / "platform").string().c_str()));
    GameServicesRecorder rec;
    u64 live_hash = 0;
    auto drive = [](Device& d, int i) {
        if (i == 3) d.gs().unlock("first_win");
        if (i == 5) d.gs().submit_score("score", 77);
        if (i == 8) d.gs().cloud_load("progress");
    };
    {
        Device d;
        d.start(&local);
        d.service.set_recorder(&rec);
        for (int i = 0; i < 20; ++i) {
            drive(d, i);
            d.step();
        }
        CHECK(d.seen().sent == 2);
        CHECK(d.seen().current == 1);
        live_hash = d.world.state_hash();
    }
    use_device(root / "replay");
    {
        Device d;
        d.start(nullptr);
        GameServicesReplayer rp(rec.bytes());
        d.service.set_replayer(&rp);
        for (int i = 0; i < 20; ++i) {
            drive(d, i);
            d.step();
        }
        CHECK(rp.done());
        CHECK_FALSE(rp.corrupt());
        CHECK(d.seen().sent == 2);
        CHECK(d.world.state_hash() == live_hash);
        CHECK_FALSE(save::exists("game_services"));
    }
}
