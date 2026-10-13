// M-MONETIZE step 2: the fake store and ad network answer every outcome the real ones can
// (success, cancel, pending, failure, fill, no-fill, reward, close) from Service::update, and the
// -monetize.fake.* convars configure them.

#include <doctest/doctest.h>

#include <filesystem>

#include "mce/convar.h"
#include "mce/ecs/save.h"
#include "mce/monetize/fake.h"

using namespace mce;
using namespace mce::monetize;

namespace {

Catalog catalog() {
    Catalog c;
    ProductInfo p;
    p.id = "remove_ads";
    p.type = ProductType::NonConsumable;
    CHECK(c.add_product(p));
    p.id = "gems";
    p.type = ProductType::Consumable;
    CHECK(c.add_product(p));
    p.id = "level_pack";
    p.type = ProductType::NonConsumable;
    CHECK(c.add_product(p));
    AdPlacementInfo a;
    a.id = "continue";
    a.format = AdFormat::Rewarded;
    CHECK(c.add_ad(a));
    a.id = "level_end";
    a.format = AdFormat::Interstitial;
    CHECK(c.add_ad(a));
    a.id = "menu";
    a.format = AdFormat::Banner;
    CHECK(c.add_ad(a));
    return c;
}

Array<StoreEvent> g_store;
Array<AdEvent> g_ads;

void collect(Frame& f) {
    for (const StoreEvent& e : f.events().read<StoreEvent>()) g_store.push_back(e);
    for (const AdEvent& e : f.events().read<AdEvent>()) g_ads.push_back(e);
}

// A host loop: Service::update every tick, the sim only while no fullscreen ad is up.
struct Host {
    World world;
    Service service;
    u64 tick = 0;
    u32 paused_ticks = 0;

    Host(IStoreProvider* store, IAdProvider* ads, const char* save_dir) {
        const std::filesystem::path p = std::filesystem::path(MCE_TEST_TMP_DIR) / save_dir;
        std::error_code ec;
        std::filesystem::remove_all(p, ec);
        std::filesystem::create_directories(p, ec);
        save::set_root(StringView(p.string().c_str()));
        REQUIRE(service.init(catalog(), store, ads));
        service.install(world);
        world.add_system(Phase::FixedUpdate_Post, "collect", &collect);
        g_store.clear();
        g_ads.clear();
    }
    void run(u32 n) {
        for (u32 i = 0; i < n; ++i) {
            service.update();
            if (service.fullscreen_ad_showing()) {
                ++paused_ticks;
                continue;
            }
            world.tick(1.0 / 60, ++tick);
        }
    }
    Store& store() { return world.resource<Store>(); }
    Ads& ads() { return world.resource<Ads>(); }
    u32 count(StoreEventKind k) const {
        u32 n = 0;
        for (const StoreEvent& e : g_store) n += e.kind == k ? 1u : 0u;
        return n;
    }
    u32 count(AdEventKind k) const {
        u32 n = 0;
        for (const AdEvent& e : g_ads) n += e.kind == k ? 1u : 0u;
        return n;
    }
};

}  // namespace

TEST_CASE("fake store: success, query and restore") {
    FakeConfig cfg;
    cfg.owned = "level_pack, nope";
    cfg.price = "9,99 TL";
    FakeStore store(cfg);
    Host h(&store, nullptr, "fake_success");
    CHECK(h.store().available());

    h.store().query_products();
    h.run(1);  // delay 1: the answers come at the first update and enter that tick
    CHECK(h.count(StoreEventKind::ProductInfo) == 3);
    CHECK(h.count(StoreEventKind::ProductsReady) == 1);
    CHECK(h.store().price(h.store().find("gems")) == StringView("9,99 TL"));

    h.store().purchase("gems");
    h.store().purchase("remove_ads");
    h.run(2);
    CHECK(h.count(StoreEventKind::Purchased) == 2);
    CHECK(h.store().owns("remove_ads"));
    CHECK(h.store().unfinished_count() == 1);
    CHECK(store.unfinished() == 1);  // the service finished remove_ads itself
    for (const StoreEvent& e : g_store)
        if (e.kind == StoreEventKind::Purchased && e.product == h.store().find("gems")) h.store().finish(e.transaction);
    CHECK(store.unfinished() == 0);
    CHECK(h.store().unfinished_count() == 0);

    // Restore: the configured account plus what it bought this run.
    h.store().restore_purchases();
    h.run(1);
    CHECK(h.count(StoreEventKind::Restored) == 2);
    CHECK(h.count(StoreEventKind::RestoreFinished) == 1);
    CHECK(h.store().owns("level_pack"));
}

TEST_CASE("fake store: cancel, failure and pending") {
    FakeConfig cfg;
    cfg.purchase = FakePurchase::Cancel;
    FakeStore store(cfg);
    Host h(&store, nullptr, "fake_outcomes");
    h.store().purchase("remove_ads");
    h.run(1);
    CHECK(h.count(StoreEventKind::Cancelled) == 1);
    CHECK_FALSE(h.store().owns("remove_ads"));

    cfg.purchase = FakePurchase::Failure;
    store.set_config(cfg);
    h.store().purchase("remove_ads");
    h.run(1);
    CHECK(h.count(StoreEventKind::Failed) == 1);
    CHECK_FALSE(h.store().owns("remove_ads"));

    cfg.purchase = FakePurchase::Pending;
    cfg.pending_updates = 10;
    store.set_config(cfg);
    h.store().purchase("remove_ads");
    h.run(1);
    CHECK(h.count(StoreEventKind::Pending) == 1);
    CHECK_FALSE(h.store().owns("remove_ads"));
    h.run(9);
    CHECK(h.count(StoreEventKind::Purchased) == 0);
    h.run(1);
    CHECK(h.count(StoreEventKind::Purchased) == 1);
    CHECK(h.store().owns("remove_ads"));
}

TEST_CASE("fake ads: fill pauses the host until close, rewards only when watched") {
    FakeConfig cfg;
    cfg.ad_updates = 20;
    cfg.reward = 3;
    FakeAds ads(cfg);
    Host h(nullptr, &ads, "fake_ads");
    CHECK(h.ads().available());

    h.ads().load("continue");
    h.run(1);
    CHECK(h.count(AdEventKind::Loaded) == 1);
    CHECK(h.ads().ready("continue"));

    h.ads().show("continue");
    h.run(1);
    CHECK(h.service.fullscreen_ad_showing());
    h.run(30);
    CHECK_FALSE(h.service.fullscreen_ad_showing());
    CHECK(h.paused_ticks == 20);
    CHECK(h.count(AdEventKind::Shown) == 1);
    REQUIRE(h.count(AdEventKind::Rewarded) == 1);
    for (const AdEvent& e : g_ads)
        if (e.kind == AdEventKind::Rewarded) CHECK(e.reward_amount == 3);
    CHECK(h.count(AdEventKind::Closed) == 1);

    cfg.rewarded_completes = false;  // closed early
    ads.set_config(cfg);
    h.ads().show("continue");
    h.ads().show("level_end");  // interstitials never reward
    h.run(60);
    CHECK(h.count(AdEventKind::Rewarded) == 1);
    CHECK(h.count(AdEventKind::Closed) == 3);

    h.service.set_safe_insets(SafeInsets{0, 0, 0, 48});
    h.ads().show_banner("menu");
    h.run(1);
    CHECK(ads.banner() == h.ads().find("menu"));
    CHECK(ads.banner_insets().bottom == 48);
    CHECK_FALSE(h.service.fullscreen_ad_showing());  // a banner never pauses
    h.ads().hide_banner();
    h.run(1);
    CHECK(ads.banner() == kNoIndex);
    CHECK(h.count(AdEventKind::Closed) == 4);
}

TEST_CASE("fake ads: no fill and failure") {
    FakeConfig cfg;
    cfg.fill = FakeFill::NoFill;
    FakeAds ads(cfg);
    Host h(nullptr, &ads, "fake_nofill");
    h.ads().show("level_end");
    h.ads().show_banner("menu");
    h.run(1);
    CHECK(h.count(AdEventKind::NoFill) == 2);
    CHECK(h.paused_ticks == 0);

    cfg.fill = FakeFill::Failure;
    ads.set_config(cfg);
    h.ads().load("continue");
    h.run(1);
    CHECK(h.count(AdEventKind::Failed) == 1);
    CHECK_FALSE(h.ads().ready("continue"));
}

TEST_CASE("fake config: the -monetize.fake.* convars") {
    FakeConfig cfg;
    CHECK(fake_config_from_convars(cfg));  // defaults
    CHECK(cfg.purchase == FakePurchase::Success);
    CHECK(cfg.fill == FakeFill::Fill);

    CHECK(console::execute("monetize.fake.purchase pending"));
    CHECK(console::execute("monetize.fake.fill nofill"));
    CHECK(console::execute("monetize.fake.close close"));
    CHECK(console::execute("monetize.fake.reward 5"));
    CHECK(console::execute("monetize.fake.owned remove_ads"));
    REQUIRE(fake_config_from_convars(cfg));
    CHECK(cfg.purchase == FakePurchase::Pending);
    CHECK(cfg.fill == FakeFill::NoFill);
    CHECK_FALSE(cfg.rewarded_completes);
    CHECK(cfg.reward == 5);
    CHECK(cfg.owned.view() == "remove_ads");

    CHECK(console::execute("monetize.fake.purchase maybe"));
    CHECK_FALSE(fake_config_from_convars(cfg));
    for (const char* name : {"monetize.fake.purchase", "monetize.fake.fill", "monetize.fake.close", "monetize.fake.reward",
                             "monetize.fake.owned"})
        ConVar::find(StringId(StringView(name)))->reset();
}
