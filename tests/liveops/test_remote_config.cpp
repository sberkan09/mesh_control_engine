// M-LIVEOPS step 3: remote config. The chunk round-trips and rejects damage; fetched values never
// change a running game until it applies them (at a tick start) or the next launch reads the
// cache; the defaults are the schema; a fetcher that sends waits for consent; replays hold.

#include <doctest/doctest.h>

#include <filesystem>
#include <string>

#include "mce/ecs/save.h"
#include "mce/liveops/consent.h"
#include "mce/liveops/remote_config.h"

using namespace mce;
using namespace mce::liveops;

namespace {

void fresh_save_root(const char* name) {
    const std::filesystem::path p = std::filesystem::path(MCE_TEST_TMP_DIR) / name;
    std::error_code ec;
    std::filesystem::remove_all(p, ec);
    std::filesystem::create_directories(p, ec);
    save::set_root(StringView(p.string().c_str()));
}

ConfigValue make(const char* key, ConfigType t, i64 i = 0, f64 f = 0.0, const char* s = "") {
    ConfigValue v;
    v.key = key;
    v.type = t;
    v.i = i;
    v.f = f;
    v.s = s;
    return v;
}

ConfigValues defaults() {
    ConfigValues d;
    d.add(make("every", ConfigType::Int, 3));
    d.add(make("bonus", ConfigType::Float, 0, 1.5));
    d.add(make("winter", ConfigType::Bool, 0));
    d.add(make("motd", ConfigType::String, 0, 0, "Welcome"));
    return d;
}

ConfigValues server_values() {
    ConfigValues v;
    v.add(make("every", ConfigType::Int, 5));
    v.add(make("winter", ConfigType::Bool, 1));
    v.add(make("motd", ConfigType::Int, 7));                // wrong type: ignored
    v.add(make("unknown", ConfigType::String, 0, 0, "x"));  // not a default: ignored
    return v;
}

struct ScriptedFetcher final : IConfigFetcher {
    bool consent_needed = true;
    bool answer_now = true;
    bool fail = false;
    ConfigValues values = server_values();
    ConfigFetchSink* pending = nullptr;
    u32 calls = 0;
    const char* name() const override { return "scripted"; }
    bool needs_consent() const override { return consent_needed; }
    void fetch(ConfigFetchSink& sink) override {
        ++calls;
        if (!answer_now)
            pending = &sink;
        else if (fail)
            sink.fetch_failed();
        else
            sink.fetched(values);
    }
};

// The game: reads the config every tick into a component, so the state hash sees every value.
struct Seen {
    i64 every = 0;
    f64 bonus = 0;
    u8 winter = 0;
    u8 pad_[3] = {};
    u32 version = 0;
};

void game(Frame& f) {
    const RemoteConfig& rc = f.resource<RemoteConfig>();
    f.query<Seen&>().each([&](Seen& s) {
        s.every = rc.get<i64>("every", -1);
        s.bonus = rc.get<f64>("bonus", -1.0);
        s.winter = rc.get<bool>("winter", false) ? 1 : 0;
        s.version = rc.version();
    });
}

struct Game {
    World world;
    RemoteConfigService service;
    u64 tick = 0;
    void start(IConfigFetcher* fetcher) {
        service.init(defaults(), fetcher);
        service.install(world);
        world.add_system(Phase::FixedUpdate_Post, "game", &game);
        world.spawn_with(Seen{});
    }
    void step() {
        service.update();
        world.tick(1.0 / 60, ++tick);
    }
    Seen seen() {
        Seen out;
        world.query<Seen&>().each([&](Seen& s) { out = s; });
        return out;
    }
    RemoteConfig& rc() { return world.resource<RemoteConfig>(); }
};

}  // namespace

TEST_CASE("remote config: the chunk round-trips and rejects damage") {
    const ConfigValues d = defaults();
    Array<byte> bytes;
    encode_config(d, bytes);
    ConfigValues back;
    REQUIRE(decode_config(bytes.span(), back));
    REQUIRE(back.size() == 4);
    CHECK(back.find("bonus")->f == 1.5);
    CHECK(back.find("motd")->s.view() == "Welcome");
    CHECK(hash_config(back) == hash_config(d));
    for (usize cut = 0; cut < bytes.size(); ++cut) {
        ConfigValues t;
        CHECK_FALSE(decode_config(Span<const byte>(bytes.data(), cut), t));
    }
    ConfigValues dup;
    CHECK(dup.add(make("a", ConfigType::Int)));
    CHECK_FALSE(dup.add(make("a", ConfigType::Float)));
    CHECK_FALSE(dup.add(make("", ConfigType::Int)));
}

TEST_CASE("remote config: a fetch waits for apply_fetched or the next launch, never mid-run") {
    fresh_save_root("rc_apply");
    set_consent(Consent::Granted);
    ScriptedFetcher fetcher;
    {
        Game g;
        g.start(&fetcher);
        g.step();
        CHECK(fetcher.calls == 1);
        g.step();
        CHECK(g.rc().fetched_waiting());
        for (int i = 0; i < 5; ++i) g.step();
        CHECK(g.seen().every == 3);  // still the defaults
        CHECK(g.seen().winter == 0);

        g.rc().apply_fetched();
        g.step();  // applied at this tick's start
        CHECK(g.seen().every == 5);
        CHECK(g.seen().winter == 1);
        CHECK(g.seen().bonus == 1.5);                        // not fetched: the default
        CHECK(g.rc().get_string("motd", "?") == "Welcome");  // fetched with the wrong type: ignored
        CHECK(g.rc().get_string("unknown", "?") == "?");     // not a default: ignored
        CHECK(g.seen().version == 1);
        CHECK_FALSE(g.rc().fetched_waiting());
    }
    {
        // The next launch starts with the fetched values, offline.
        Game g;
        g.start(nullptr);
        g.step();
        CHECK(g.seen().every == 5);
        CHECK(g.seen().version == 0);
    }
    set_consent(Consent::Unknown);
}

TEST_CASE("remote config: a fetcher that sends waits for consent; a failure keeps the values") {
    fresh_save_root("rc_consent");
    set_consent(Consent::Unknown);
    ScriptedFetcher fetcher;
    Game g;
    g.start(&fetcher);
    for (int i = 0; i < 3; ++i) g.step();
    CHECK(fetcher.calls == 0);
    set_consent(Consent::Denied);
    g.step();
    CHECK(fetcher.calls == 0);
    set_consent(Consent::Granted);
    g.step();
    CHECK(fetcher.calls == 1);

    ScriptedFetcher failing;
    failing.fail = true;
    failing.consent_needed = false;
    fresh_save_root("rc_fail");
    set_consent(Consent::Unknown);
    Game h;
    h.start(&failing);
    h.step();
    h.step();
    CHECK(failing.calls == 1);  // no consent needed (a static file)
    CHECK_FALSE(h.rc().fetched_waiting());
    CHECK_FALSE(save::exists("remote_config"));
    h.rc().apply_fetched();
    h.step();
    CHECK(h.seen().every == 3);

    // A late answer, and a fetch the game asks for again.
    ScriptedFetcher late;
    late.consent_needed = false;
    late.answer_now = false;
    Game k;
    k.start(&late);
    k.step();
    REQUIRE(late.pending != nullptr);
    k.rc().fetch();  // one in flight: not asked twice
    k.step();
    CHECK(late.calls == 1);
    late.pending->fetched(late.values);
    k.step();
    CHECK(k.rc().fetched_waiting());
    k.rc().fetch();
    k.step();
    CHECK(late.calls == 2);
}

TEST_CASE("remote config: a recorded run replays to the same hash with no fetcher") {
    fresh_save_root("rc_replay");
    set_consent(Consent::Granted);
    ConfigRecorder rec;
    u64 live_hash = 0;
    {
        ScriptedFetcher fetcher;
        Game g;
        g.start(&fetcher);
        g.service.set_recorder(&rec);
        for (int i = 0; i < 60; ++i) {
            if (i == 20) g.rc().apply_fetched();
            g.step();
        }
        CHECK(g.seen().every == 5);
        live_hash = g.world.state_hash();
    }
    fresh_save_root("rc_replay_2");  // a fresh install: the defaults only
    {
        ScriptedFetcher fetcher;
        Game g;
        g.start(&fetcher);
        ConfigReplayer rp(rec.bytes());
        g.service.set_replayer(&rp);
        for (int i = 0; i < 60; ++i) {
            if (i == 20) g.rc().apply_fetched();
            g.step();
        }
        CHECK(rp.done());
        CHECK_FALSE(rp.corrupt());
        CHECK(fetcher.calls == 0);
        CHECK(g.world.state_hash() == live_hash);
        CHECK_FALSE(save::exists("remote_config"));
    }
    set_consent(Consent::Unknown);
}
