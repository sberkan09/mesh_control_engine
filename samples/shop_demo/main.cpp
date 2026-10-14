// M-MONETIZE's running demo (SBA-21): a headless runner game with a shop, against the fake store
// and ad network, so purchases and ads run in scripts/ci/local_ci.sh with no account behind them.
//
// The store catalog is the sample's assets/store.mcstore, cooked into the dev pack: a "remove_ads"
// non-consumable (removes_ads), a "gems_100" consumable, a rewarded "continue" and a "level_end"
// interstitial. The game is one system:
//
//   - a level lasts 300 ticks; between levels an interstitial shows, unless ads are removed;
//   - in level 2 the runner dies once and a rewarded "continue" ad revives it (with ads removed the
//     continue is free: the game's own alternative to the rewarded ad);
//   - in level 1 it buys gems_100 (granted, then finished), in level 3 remove_ads.
//
// A fullscreen ad pauses the sim exactly like mce_player does: the host keeps calling
// Service::update but skips World::tick until the ad closes.
//
//   sample_shop_demo -ticks 1800                          # fresh install, nothing persisted
//   sample_shop_demo -save_dir D                          # entitlements in D/entitlements.mcesave
//   sample_shop_demo -save_dir D -provider none           # relaunch offline: remove_ads still owned
//   sample_shop_demo -monetize.fake.purchase cancel       # the player backs out of every purchase
//   sample_shop_demo -monetize.fake.owned remove_ads -restore 1   # reinstall, then restore
//   sample_shop_demo -record run.rec / -replay run.rec    # same hash, no provider on replay
//
// Prints every store and ad event with its tick, then a summary line ending in "hash <16 hex>"
// (scripts/ci/determinism.py).

#include <cstdio>

#include "mce/asset/asset_system.h"
#include "mce/core.h"
#include "mce/ecs/save.h"
#include "mce/monetize/catalog.h"
#include "mce/monetize/fake.h"
#include "mce/monetize/monetize.h"

using namespace mce;
using namespace mce::monetize;

namespace {

MCE_CVAR(cv_ticks, "ticks", 1800, "Simulation ticks to run (a fullscreen ad pauses the sim; paused ticks do not count)",
         CVarFlags::None);
MCE_CVAR(cv_pack, "pack", "", "Cooked pack holding the store catalog (default: the sample's dev pack)",
         CVarFlags::None);
MCE_CVAR(cv_provider, "provider", "fake", "Store and ad network: fake (the -monetize.fake.* convars) or none (offline)",
         CVarFlags::None);
MCE_CVAR(cv_save_dir, "save_dir", "", "Where the entitlements are saved (empty: not persisted)", CVarFlags::None);
MCE_CVAR(cv_restore, "restore", false, "Restore purchases at the first tick (a reinstall)", CVarFlags::None);
MCE_CVAR(cv_shop, "shop", true, "Buy gems_100 in level 1 and remove_ads in level 3", CVarFlags::None);
MCE_CVAR(cv_record, "record", "", "Write what entered the sim to this file", CVarFlags::None);
MCE_CVAR(cv_replay, "replay", "", "Replay a -record file instead of the providers", CVarFlags::None);

constexpr u32 kLevelTicks = 300;
constexpr u32 kDeathLevel = 2;
constexpr u32 kDeathTick = 150;

// The game's state: one entity, hashed by World::state_hash with everything the shop changed.
struct Run {
    u32 level = 1;
    u32 level_tick = 0;
    u32 coins = 0;
    u32 gems = 0;
    u32 deaths = 0;
    u32 continues = 0;      // revived by a rewarded ad (or for free with ads removed)
    u32 restarts = 0;       // the continue ad was closed early or did not fill: the level starts over
    u32 interstitials = 0;  // level_end ads that closed
    u32 interstitials_skipped = 0;
    u32 purchases = 0;
    u32 restored = 0;
    u32 cancels = 0;
    u32 failures = 0;
    u32 continue_request = 0;  // the rewarded ad being shown for a continue
    u8 rewarded = 0;
    u8 ad_free = 0;  // Ads::removed, as the game last saw it
    u8 pad_[2] = {};
};

u64 g_tick = 0;  // the sim tick being run (log lines)

const char* product_name(const Store& store, u16 i) {
    return i < store.products().size() ? store.products()[i].id.c_str() : "-";
}

void request_log(const char* what, const char* id, u32 request) {
    std::printf("tick %llu: %s %s -> request %u\n", static_cast<unsigned long long>(g_tick), what, id, request);
}

void on_store_event(Run& r, Store& store, const StoreEvent& e) {
    std::printf("tick %llu: store %s %s request %u\n", static_cast<unsigned long long>(g_tick),
                store_event_name(e.kind), product_name(store, e.product), e.request);
    switch (e.kind) {
        case StoreEventKind::Purchased:
            ++r.purchases;
            if (e.product == store.find("gems_100")) {
                r.gems += 100;                // grant first ...
                store.finish(e.transaction);  // ... then let the store forget the transaction
            }
            break;
        case StoreEventKind::Restored:
            ++r.restored;
            break;
        case StoreEventKind::Cancelled:
            ++r.cancels;
            break;
        case StoreEventKind::Failed:
            ++r.failures;
            break;
        default:
            break;
    }
}

void on_ad_event(Run& r, Ads& ads, const AdEvent& e) {
    const char* name = e.placement < ads.placements().size() ? ads.placements()[e.placement].id.c_str() : "-";
    std::printf("tick %llu: ad %s %s request %u\n", static_cast<unsigned long long>(g_tick), ad_event_name(e.kind),
                name, e.request);
    if (e.request == r.continue_request && r.continue_request != 0) {
        if (e.kind == AdEventKind::Rewarded) r.rewarded = 1;
        if (e.kind == AdEventKind::Closed || e.kind == AdEventKind::NoFill || e.kind == AdEventKind::Failed) {
            if (r.rewarded) {
                ++r.continues;  // revived where it fell
            } else {
                ++r.restarts;
                r.level_tick = 0;
            }
            r.continue_request = 0;
            r.rewarded = 0;
        }
        return;
    }
    if (e.kind == AdEventKind::Closed && e.placement == ads.find("level_end")) ++r.interstitials;
}

void shop_game(Frame& f) {
    Store& store = f.resource<Store>();
    Ads& ads = f.resource<Ads>();
    g_tick = f.tick;
    f.query<Run&>().each([&](Run& r) {
        for (const StoreEvent& e : f.events().read<StoreEvent>()) on_store_event(r, store, e);
        for (const AdEvent& e : f.events().read<AdEvent>()) on_ad_event(r, ads, e);
        r.ad_free = ads.removed() ? 1 : 0;

        if (f.tick == 1) {
            request_log("query", "products", store.query_products());
            if (cv_restore.get_bool()) request_log("restore", "purchases", store.restore_purchases());
        }
        if (r.continue_request != 0) return;  // waiting on the continue ad: the runner lies still

        ++r.level_tick;
        if (r.level_tick % 20 == 0) ++r.coins;
        if (cv_shop.get_bool() && r.level == 1 && r.level_tick == 100 && r.purchases == 0)
            request_log("buy", "gems_100", store.purchase("gems_100"));
        if (cv_shop.get_bool() && r.level == 3 && r.level_tick == 10 && !store.owns("remove_ads"))
            request_log("buy", "remove_ads", store.purchase("remove_ads"));

        if (r.level == kDeathLevel && r.level_tick == kDeathTick && r.deaths == 0) {
            ++r.deaths;
            if (ads.removed()) {
                ++r.continues;  // no ads to watch: the continue is on the house
            } else {
                r.continue_request = ads.show("continue");
                request_log("show", "continue", r.continue_request);
                if (r.continue_request == 0) {  // no ad network: the level starts over
                    ++r.restarts;
                    r.level_tick = 0;
                }
            }
            return;
        }
        if (r.level_tick == kLevelTicks) {
            ++r.level;
            r.level_tick = 0;
            if (ads.removed()) {
                ++r.interstitials_skipped;
            } else {
                request_log("show", "level_end", ads.show("level_end"));
            }
        }
    });
}

Result<void> load_catalog(asset::AssetSystem& assets, Catalog& out) {
    FixedString<1024> pack(cv_pack.get_string());
#if defined(MCE_SAMPLE_DEV_PACK)
    if (pack.empty()) pack.assign(MCE_SAMPLE_DEV_PACK);
#endif
    const StringView roots[1] = {pack.view()};
    MCE_TRY(assets.mount(Span<const StringView>(roots, 1)));
    for (asset::AssetKey k = 0; k < assets.asset_count(); ++k) {
        if (assets.info(k).type != asset::ChunkType::StoreCatalog) continue;
        MCE_TRY_ASSIGN(const asset::AssetHandle<asset::StoreCatalog> h, assets.load_sync<asset::StoreCatalog>(k));
        const Result<void> r = decode_catalog(assets.resolve(h), out);
        assets.release(asset::AnyHandle(h));
        return r;
    }
    return Error(Errc::NotFound, "shop_demo: the pack has no store catalog", pack.view());
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

bool write_file(const char* path, Span<const byte> bytes) {
    std::FILE* f = std::fopen(path, "wb");
    if (!f) return false;
    const bool ok = std::fwrite(bytes.data(), 1, bytes.size(), f) == bytes.size();
    return std::fclose(f) == 0 && ok;
}

int fail(const Error& e) {
    FixedString<512> msg;
    format_error(e, msg);
    MCE_LOG_ERROR("shop_demo: {}", msg.view());
    return 1;
}

int run() {
    asset::AssetSystem assets;
    Catalog catalog;
    if (Result<void> r = load_catalog(assets, catalog); !r) {
        return fail(r.error());
    }

    ServiceOptions opt;
    if (cv_save_dir.get_string().empty()) {
        opt.entitlements_save.clear();  // a fresh install every run (determinism.py runs it three times)
    } else {
        save::set_root(cv_save_dir.get_string());
    }

    FakeConfig cfg;
    if (!fake_config_from_convars(cfg)) {
        MCE_LOG_ERROR("shop_demo: a -monetize.fake.* value is not one it knows");
        return 1;
    }
    FakeStore fake_store(cfg);
    FakeAds fake_ads(cfg);
    const StringView provider = cv_provider.get_string();
    if (provider != "fake" && provider != "none") {
        MCE_LOG_ERROR("shop_demo: -provider wants fake or none");
        return 1;
    }
    const bool fake = provider == "fake";

    World world;
    Service service;
    if (Result<void> r = service.init(catalog, fake ? &fake_store : nullptr, fake ? &fake_ads : nullptr, opt); !r) {
        return fail(r.error());
    }
    service.install(world);  // before the game: the pump runs first
    world.add_system(Phase::FixedUpdate_Post, "shop_game", &shop_game);
    world.spawn_with(Run{});

    Recorder recorder;
    Array<byte> replay_bytes;
    const FixedString<1024> replay_path(cv_replay.get_string());
    if (!replay_path.empty() && !read_file(replay_path.c_str(), replay_bytes)) {
        MCE_LOG_ERROR("shop_demo: cannot read {}", replay_path.view());
        return 1;
    }
    Replayer replayer(replay_bytes.span());
    if (!replay_path.empty())
        service.set_replayer(&replayer);
    else if (!cv_record.get_string().empty())
        service.set_recorder(&recorder);

    const Store& store = world.resource<Store>();
    for (const ProductInfo& p : store.products())
        if (store.owns(p.id.view())) std::printf("shop_demo: owns %s at launch\n", p.id.c_str());
    if (world.resource<Ads>().removed())
        std::printf("shop_demo: ads removed at launch: the ad network is not started\n");

    const u64 ticks = static_cast<u64>(cv_ticks.get_int());
    const u64 app_cap = ticks * 4 + 10000;  // a stuck ad must not hang CI
    u64 sim = 0;
    u64 paused = 0;
    for (u64 app = 0; sim < ticks && app < app_cap; ++app) {
        if (replay_path.empty()) service.update();  // also while paused: the ad has to be able to close
        if (service.fullscreen_ad_showing()) {
            ++paused;
            continue;
        }
        world.tick(1.0 / 60.0, ++sim);
    }

    int rc = sim == ticks ? 0 : 1;
    if (rc != 0) MCE_LOG_ERROR("shop_demo: a fullscreen ad never closed ({} of {} ticks)", sim, ticks);
    if (!replay_path.empty() && (replayer.corrupt() || !replayer.done())) {
        MCE_LOG_ERROR("shop_demo: the replay did not match this run");
        rc = 1;
    }
    if (!cv_record.get_string().empty() && replay_path.empty()) {
        const FixedString<1024> path(cv_record.get_string());
        if (!write_file(path.c_str(), recorder.bytes())) {
            MCE_LOG_ERROR("shop_demo: cannot write {}", path.view());
            rc = 1;
        }
    }

    Run r;
    world.query<Run&>().each([&](Run& run) { r = run; });
    std::printf(
        "shop_demo: %llu ticks (%llu paused by ads) | level %u coins %u gems %u | deaths %u continues %u restarts %u"
        " | interstitials %u skipped %u | purchases %u restored %u cancels %u failures %u | unfinished %u\n",
        static_cast<unsigned long long>(sim), static_cast<unsigned long long>(paused), r.level, r.coins, r.gems,
        r.deaths, r.continues, r.restarts, r.interstitials, r.interstitials_skipped, r.purchases, r.restored, r.cancels,
        r.failures, store.unfinished_count());
    for (const ProductInfo& p : store.products())
        if (store.owns(p.id.view())) std::printf("shop_demo: owns %s\n", p.id.c_str());
    std::printf("shop_demo: hash %016llx\n", static_cast<unsigned long long>(world.state_hash()));
    service.shutdown();
    return rc;
}

}  // namespace

int main(int argc, char** argv) {
    log::init({.app_name = "shop_demo", .level = log::Level::Info});
    console::parse_command_line(argc, argv);
    const int rc = run();
    log::shutdown();
    return rc;
}
