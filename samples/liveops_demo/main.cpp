// M-LIVEOPS's running demo (SBA-25): a headless level runner wired to every live service against
// local stand-ins, so achievements, scores, cloud saves, remote config and analytics run in
// scripts/ci/local_ci.sh with no vendor account behind them.
//
// The sample's assets hold the game services catalog (game.mcservices: "first_win", an incremental
// "win_5", a lower_is_better "best_time" leaderboard and a "progress" cloud save) and the remote
// config defaults (remote.mcconfig: level_ticks 200). The game is one system:
//
//   - a level lasts level_ticks (remote config) and always ends in a win: first_win unlocks, win_5
//     progresses, the level's time goes to best_time and a level_end analytics event is sent;
//   - after each win the "progress" save is written and uploaded (cloud_save);
//   - with -restore 1 (a reinstall) it asks for the cloud's "progress" once signed in and continues
//     from it on CloudLoaded.
//
// The platform is LocalGameServices (a folder), the config server FileConfigFetcher (a cooked pack)
// and analytics go to a JSON lines file, all behind the consent gate the player would set.
//
//   sample_liveops_demo -ticks 1850                                # nothing persisted, no platform
//   sample_liveops_demo -save_dir D -platform_dir P                # achievements, scores, cloud save
//   sample_liveops_demo -save_dir D2 -platform_dir P -restore 1    # reinstall: progress from the cloud
//   sample_liveops_demo -save_dir D -server S.mce                  # fetch: level_ticks changes next launch
//   sample_liveops_demo -save_dir D -offline 1                     # airplane mode: cached or bundled values
//   sample_liveops_demo -save_dir D -consent denied                # no analytics event leaves
//   sample_liveops_demo -record run.rec / -replay run.rec          # same hash, no provider on replay
//
// Prints every game services event with its tick, then a summary line ending in "hash <16 hex>"
// (scripts/ci/determinism.py).

#include <cstdio>
#include <cstring>

#include "mce/asset/asset_system.h"
#include "mce/core.h"
#include "mce/ecs/save.h"
#include "mce/liveops/game_services.h"
#include "mce/liveops/liveops.h"
#include "mce/liveops/remote_config.h"

using namespace mce;
using namespace mce::liveops;

namespace {

MCE_CVAR(cv_ticks, "ticks", 1850, "Simulation ticks to run", CVarFlags::None);
MCE_CVAR(cv_pack, "pack", "", "Cooked pack holding the catalog and config defaults (default: the sample's dev pack)",
         CVarFlags::None);
MCE_CVAR(cv_save_dir, "save_dir", "", "This device's saves: progress, service state, config cache (empty: none)",
         CVarFlags::None);
MCE_CVAR(cv_platform_dir, "platform_dir", "",
         "The local game services platform folder, shared by \"devices\" (empty: no platform)", CVarFlags::None);
MCE_CVAR(cv_server, "server", "", "A cooked pack standing in for the remote config server (empty: none)",
         CVarFlags::None);
MCE_CVAR(cv_offline, "offline", false, "Airplane mode: no config server, every platform call fails", CVarFlags::None);
MCE_CVAR(cv_consent, "consent", "granted", "The player's consent: granted, denied or unknown", CVarFlags::None);
MCE_CVAR(cv_restore, "restore", false, "Load the cloud's progress once signed in (a reinstall)", CVarFlags::None);
MCE_CVAR(cv_record, "record", "", "Write what entered the sim to this file", CVarFlags::None);
MCE_CVAR(cv_replay, "replay", "", "Replay a -record file instead of the providers", CVarFlags::None);

constexpr u32 kProgressMagic = 0x47504C4C;  // "LLPG"

// The game's state: one entity, hashed by World::state_hash.
struct Run {
    u32 level = 1;
    u32 level_tick = 0;
    u32 level_ticks = 0;  // this level's length (remote config, read at its start)
    u32 wins = 0;
    i64 best_time = 0;  // ms, 0 before any
    u32 restored = 0;   // CloudLoaded progress taken
    u32 restore_request = 0;
    u32 cloud_saved = 0;
    u32 cloud_failed = 0;
    u32 conflicts = 0;
    u32 pad_ = 0;
};

u64 g_tick = 0;
bool g_persist = false;  // -save_dir given (and not replaying): the game writes its progress save

struct Progress {
    u32 magic;
    u32 level;
    u32 wins;
    u32 reserved;
};

void write_progress(const Run& r) {
    if (!g_persist) return;
    const Progress p{kProgressMagic, r.level, r.wins, 0};
    if (Result<void> res = save::write("progress", Span<const byte>(reinterpret_cast<const byte*>(&p), sizeof(p)));
        !res)
        MCE_LOG_ERROR("liveops_demo: the progress save could not be written");
}

bool read_progress(Run& r) {
    Array<byte> bytes;
    if (!save::exists("progress") || !save::read("progress", bytes) || bytes.size() != sizeof(Progress)) return false;
    Progress p;
    std::memcpy(&p, bytes.data(), sizeof(p));
    if (p.magic != kProgressMagic) return false;
    r.level = p.level;
    r.wins = p.wins;
    r.level_tick = 0;
    return true;
}

const char* item_name(const GameServices& gs, const GameServicesEvent& e) {
    const ServicesCatalog& c = gs.catalog();
    switch (e.kind) {
        case GameServicesEventKind::AchievementSent:
            return e.index < c.achievements().size() ? c.achievements()[e.index].id.c_str() : "-";
        case GameServicesEventKind::ScoreSent:
            return e.index < c.leaderboards().size() ? c.leaderboards()[e.index].id.c_str() : "-";
        case GameServicesEventKind::CloudSaved:
        case GameServicesEventKind::CloudLoaded:
        case GameServicesEventKind::CloudCurrent:
        case GameServicesEventKind::CloudConflict:
        case GameServicesEventKind::Failed:
            return e.index < c.cloud_saves().size() ? c.cloud_saves()[e.index].c_str() : "-";
        default:
            return "-";
    }
}

void on_services_event(Run& r, GameServices& gs, const GameServicesEvent& e) {
    std::printf("tick %llu: services %s %s value %lld\n", static_cast<unsigned long long>(g_tick),
                game_services_event_name(e.kind), item_name(gs, e), static_cast<long long>(e.value));
    switch (e.kind) {
        case GameServicesEventKind::SignedIn:
            if (cv_restore.get_bool() && r.restore_request == 0) {
                r.restore_request = gs.cloud_load("progress");
                std::printf("tick %llu: cloud_load progress -> request %u\n", static_cast<unsigned long long>(g_tick),
                            r.restore_request);
            }
            break;
        case GameServicesEventKind::CloudLoaded:
            if (read_progress(r)) {
                ++r.restored;
                r.level_ticks = 0;  // the restored level starts over
                std::printf("tick %llu: progress restored: level %u wins %u\n", static_cast<unsigned long long>(g_tick),
                            r.level, r.wins);
            }
            break;
        case GameServicesEventKind::CloudSaved:
            ++r.cloud_saved;
            break;
        case GameServicesEventKind::CloudConflict:
            ++r.conflicts;
            gs.resolve_conflict("progress", true);  // this device played on: keep its progress
            break;
        case GameServicesEventKind::Failed:
            ++r.cloud_failed;
            break;
        default:
            break;
    }
}

void liveops_game(Frame& f) {
    GameServices& gs = f.resource<GameServices>();
    const RemoteConfig& rc = f.resource<RemoteConfig>();
    g_tick = f.tick;
    f.query<Run&>().each([&](Run& r) {
        for (const GameServicesEvent& e : f.events().read<GameServicesEvent>()) on_services_event(r, gs, e);
        if (r.level_ticks == 0) {
            const i64 len = rc.get_int("level_ticks", 200);
            r.level_ticks = len < 1 ? 1u : static_cast<u32>(len);
        }
        if (++r.level_tick < r.level_ticks) return;

        // A win. The time is the level's length plus a little that varies per level.
        const i64 time_ms = static_cast<i64>(r.level_ticks) * 1000 / 60 + static_cast<i64>((r.level * 37) % 500);
        ++r.wins;
        gs.unlock("first_win");
        gs.add_progress("win_5", 1);
        gs.submit_score("best_time", time_ms);
        if (r.best_time == 0 || time_ms < r.best_time) r.best_time = time_ms;
        analytics::event("level_end", analytics::Params().add("level", r.level).add("time_ms", time_ms));
        std::printf("tick %llu: level %u won in %lld ms\n", static_cast<unsigned long long>(g_tick), r.level,
                    static_cast<long long>(time_ms));
        ++r.level;
        r.level_tick = 0;
        r.level_ticks = 0;
        write_progress(r);
        gs.cloud_save("progress");
    });
}

template <class Chunk>
Result<void> load_first(asset::AssetSystem& assets, asset::ChunkType type, Array<byte>& out) {
    for (asset::AssetKey k = 0; k < assets.asset_count(); ++k) {
        if (assets.info(k).type != type) continue;
        MCE_TRY_ASSIGN(const asset::AssetHandle<Chunk> h, assets.load_sync<Chunk>(k));
        const Span<const byte> bytes = assets.resolve(h);
        for (const byte b : bytes) out.push_back(b);
        assets.release(asset::AnyHandle(h));
        return ok();
    }
    return Error(Errc::NotFound, "liveops_demo: the pack lacks a chunk it needs");
}

Result<void> load_pack(asset::AssetSystem& assets, ServicesCatalog& catalog, ConfigValues& defaults) {
    FixedString<1024> pack(cv_pack.get_string());
#if defined(MCE_SAMPLE_DEV_PACK)
    if (pack.empty()) pack.assign(MCE_SAMPLE_DEV_PACK);
#endif
    const StringView roots[1] = {pack.view()};
    MCE_TRY(assets.mount(Span<const StringView>(roots, 1)));
    Array<byte> bytes;
    MCE_TRY(load_first<asset::GameServicesCatalogChunk>(assets, asset::ChunkType::GameServicesCatalog, bytes));
    MCE_TRY(decode_services_catalog(bytes.span(), catalog));
    bytes.clear();
    MCE_TRY(load_first<asset::RemoteConfigChunk>(assets, asset::ChunkType::RemoteConfig, bytes));
    return decode_config(bytes.span(), defaults);
}

bool read_file(const char* path, Array<byte>& out) {
    std::FILE* f = std::fopen(path, "rb");
    if (!f) return false;
    byte buf[4096];
    usize n = 0;
    while ((n = std::fread(buf, 1, sizeof(buf), f)) > 0)
        for (usize i = 0; i < n; ++i) out.push_back(buf[i]);
    std::fclose(f);
    return true;
}

// A recording is the game services stream's size, that stream, then the remote config stream.
bool write_recording(const char* path, Span<const byte> services, Span<const byte> config) {
    std::FILE* f = std::fopen(path, "wb");
    if (!f) return false;
    const u32 n = static_cast<u32>(services.size());
    bool ok = std::fwrite(&n, sizeof(n), 1, f) == 1;
    ok = ok && std::fwrite(services.data(), 1, services.size(), f) == services.size();
    ok = ok && std::fwrite(config.data(), 1, config.size(), f) == config.size();
    return std::fclose(f) == 0 && ok;
}

int fail(const Error& e) {
    FixedString<512> msg;
    format_error(e, msg);
    MCE_LOG_ERROR("liveops_demo: {}", msg.view());
    return 1;
}

int run() {
    asset::AssetSystem assets;
    ServicesCatalog catalog;
    ConfigValues defaults;
    if (Result<void> r = load_pack(assets, catalog, defaults); !r) return fail(r.error());

    const StringView consent_arg = cv_consent.get_string();
    if (consent_arg == "granted")
        set_consent(Consent::Granted);
    else if (consent_arg == "denied")
        set_consent(Consent::Denied);
    else if (consent_arg == "unknown")
        set_consent(Consent::Unknown);
    else {
        MCE_LOG_ERROR("liveops_demo: -consent wants granted, denied or unknown");
        return 1;
    }

    const FixedString<1024> replay_path(cv_replay.get_string());
    const bool replaying = !replay_path.empty();
    const bool offline = cv_offline.get_bool();
    const FixedString<512> save_dir(cv_save_dir.get_string());
    g_persist = !save_dir.empty() && !replaying;

    GameServicesOptions gs_opt;
    RemoteConfigOptions rc_opt;
    if (g_persist) {
        save::set_root(save_dir.view());
    } else {
        gs_opt.state_save.clear();  // a fresh install every run (determinism.py runs it three times)
        rc_opt.cache_save.clear();
    }

    // Analytics: a JSON lines file in the save directory, sent only while consent is granted.
    FixedString<512> analytics_path;
    if (g_persist) {
        analytics_path.assign(save_dir.view());
        analytics_path.append("/analytics.jsonl");
    }
    analytics::FileBackend analytics_file(analytics_path.view());
    Service liveops;
    ServiceDesc d;
    d.analytics = analytics_path.empty() ? nullptr : &analytics_file;
    liveops.init(d);

    // The platform: a folder every "device" of the player shares.
    const FixedString<512> platform_dir(cv_platform_dir.get_string());
    LocalGameServices platform(platform_dir.view());
    platform.set_offline(offline);
    IGameServicesProvider* provider = platform_dir.empty() || replaying ? nullptr : &platform;
    FileConfigFetcher server(cv_server.get_string());
    IConfigFetcher* fetcher = cv_server.get_string().empty() || offline || replaying ? nullptr : &server;

    World world;
    RemoteConfigService remote_config;
    GameServicesService services;
    remote_config.init(defaults, fetcher, rc_opt);
    services.init(catalog, provider, gs_opt);
    remote_config.install(world);  // both pumps run before the game
    services.install(world);
    world.add_system(Phase::FixedUpdate_Post, "liveops_game", &liveops_game);

    Run start;
    if (g_persist && read_progress(start))
        std::printf("liveops_demo: progress at launch: level %u wins %u\n", start.level, start.wins);
    world.spawn_with(Run(start));

    GameServicesRecorder gs_recorder;
    ConfigRecorder rc_recorder;
    Array<byte> replay_bytes;
    if (replaying && !read_file(replay_path.c_str(), replay_bytes)) {
        MCE_LOG_ERROR("liveops_demo: cannot read {}", replay_path.view());
        return 1;
    }
    u32 gs_size = 0;
    if (replaying) {
        if (replay_bytes.size() >= sizeof(gs_size)) std::memcpy(&gs_size, replay_bytes.data(), sizeof(gs_size));
        if (replay_bytes.size() < sizeof(gs_size) || gs_size > replay_bytes.size() - sizeof(gs_size)) {
            MCE_LOG_ERROR("liveops_demo: {} is not a recording", replay_path.view());
            return 1;
        }
    }
    const Span<const byte> all = replay_bytes.span();
    const usize gs_at = replaying ? sizeof(gs_size) : 0;
    GameServicesReplayer gs_replayer(replaying ? all.subspan(gs_at, gs_size) : Span<const byte>());
    ConfigReplayer rc_replayer(replaying ? all.subspan(gs_at + gs_size) : Span<const byte>());
    if (replaying) {
        services.set_replayer(&gs_replayer);
        remote_config.set_replayer(&rc_replayer);
    } else if (!cv_record.get_string().empty()) {
        services.set_recorder(&gs_recorder);
        remote_config.set_recorder(&rc_recorder);
    }

    const RemoteConfig& rc = world.resource<RemoteConfig>();
    std::printf("liveops_demo: level_ticks %lld motd \"%.*s\" | consent %s\n",
                static_cast<long long>(rc.get_int("level_ticks", 200)),
                static_cast<int>(rc.get_string("motd", "").size()), rc.get_string("motd", "").data(),
                consent_name(consent()));

    const u64 ticks = static_cast<u64>(cv_ticks.get_int());
    for (u64 sim = 1; sim <= ticks; ++sim) {
        liveops.update();
        if (!replaying) {
            remote_config.update();
            services.update();
        }
        world.tick(1.0 / 60.0, sim);
    }

    int rc_exit = 0;
    if (replaying && (gs_replayer.corrupt() || !gs_replayer.done() || rc_replayer.corrupt() || !rc_replayer.done())) {
        MCE_LOG_ERROR("liveops_demo: the replay did not match this run");
        rc_exit = 1;
    }
    if (!cv_record.get_string().empty() && !replaying) {
        const FixedString<1024> path(cv_record.get_string());
        if (!write_recording(path.c_str(), gs_recorder.bytes(), rc_recorder.bytes())) {
            MCE_LOG_ERROR("liveops_demo: cannot write {}", path.view());
            rc_exit = 1;
        }
    }

    Run r;
    world.query<Run&>().each([&](Run& run) { r = run; });
    const GameServices& gs = world.resource<GameServices>();
    liveops.shutdown();  // what consent allows goes out before the summary
    const analytics::Stats st = analytics::stats();
    std::printf(
        "liveops_demo: %llu ticks | level %u wins %u best_time %lld | first_win %s win_5 %u/5 | restored %u"
        " cloud_saved %u failed %u conflicts %u | pending %u | fetched_waiting %s | analytics sent %llu dropped %llu\n",
        static_cast<unsigned long long>(ticks), r.level, r.wins, static_cast<long long>(r.best_time),
        gs.unlocked("first_win") ? "yes" : "no", gs.progress("win_5"), r.restored, r.cloud_saved, r.cloud_failed,
        r.conflicts, gs.pending(), remote_config.fetched_waiting() ? "yes" : "no",
        static_cast<unsigned long long>(st.sent), static_cast<unsigned long long>(st.dropped));
    std::printf("liveops_demo: hash %016llx\n", static_cast<unsigned long long>(world.state_hash()));
    services.shutdown();
    return rc_exit;
}

}  // namespace

int main(int argc, char** argv) {
    log::init({.app_name = "liveops_demo", .level = log::Level::Info});
    console::parse_command_line(argc, argv);
    const int rc = run();
    log::shutdown();
    return rc;
}
