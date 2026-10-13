// M-MONETIZE step 5: a "remove ads" entitlement. Once a removes_ads product is owned no ad request
// reaches the network (the gate), a banner on screen goes away, and a relaunch with it owned never
// starts the ad network at all.

#include <doctest/doctest.h>

#include <filesystem>

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
    p.removes_ads = true;
    CHECK(c.add_product(p));
    p = ProductInfo{};
    p.id = "level_pack";  // owned, but does not remove ads
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

// The fake network, counting every call the service makes.
class CountingAds final : public IAdProvider {
public:
    const char* name() const override { return "counting"; }
    void start(const Catalog& c, Sink& s) override {
        ++starts;
        inner.start(c, s);
    }
    void stop() override { inner.stop(); }
    void update() override { inner.update(); }
    void load(u16 p, u32 r) override {
        ++requests;
        inner.load(p, r);
    }
    void show(u16 p, u32 r) override {
        ++requests;
        inner.show(p, r);
    }
    void show_banner(u16 p, u32 r, BannerEdge e, const SafeInsets& i) override {
        ++requests;
        inner.show_banner(p, r, e, i);
    }
    void hide_banner() override {
        ++hides;
        inner.hide_banner();
    }
    FakeAds inner;
    u32 starts = 0, requests = 0, hides = 0;
};

struct Host {
    World world;
    Service service;
    u64 tick = 0;

    Host(IStoreProvider* store, IAdProvider* ads, const char* dir, bool fresh) {
        const std::filesystem::path p = std::filesystem::path(MCE_TEST_TMP_DIR) / dir;
        std::error_code ec;
        if (fresh) std::filesystem::remove_all(p, ec);
        std::filesystem::create_directories(p, ec);
        save::set_root(StringView(p.string().c_str()));
        REQUIRE(service.init(catalog(), store, ads));
        service.install(world);
    }
    void run(u32 n) {
        for (u32 i = 0; i < n; ++i) {
            service.update();
            if (service.fullscreen_ad_showing()) continue;
            world.tick(1.0 / 60, ++tick);
        }
    }
    Store& store() { return world.resource<Store>(); }
    Ads& ads() { return world.resource<Ads>(); }
};

}  // namespace

TEST_CASE("remove ads: once owned, no ad request reaches the network and the banner goes") {
    FakeConfig cfg;
    cfg.ad_updates = 2;
    FakeStore store(cfg);
    CountingAds ads;
    Host h(&store, &ads, "remove_ads_live", true);
    CHECK(ads.starts == 1);
    CHECK_FALSE(h.ads().removed());

    CHECK(h.ads().show_banner("menu") != 0);
    CHECK(h.ads().load("level_end") != 0);
    h.run(2);
    CHECK(h.ads().ready("level_end"));
    CHECK(ads.requests == 2);
    CHECK(ads.inner.banner() == 2);  // the menu banner is up

    // Another non-consumable does not remove ads.
    h.store().purchase("level_pack");
    h.run(2);
    REQUIRE(h.store().owns("level_pack"));
    CHECK_FALSE(h.ads().removed());

    h.store().purchase("remove_ads");
    h.run(2);
    REQUIRE(h.store().owns("remove_ads"));
    CHECK(h.ads().removed());
    CHECK(ads.hides == 1);
    CHECK(ads.inner.banner() == kNoIndex);
    CHECK_FALSE(h.ads().ready("level_end"));  // loaded before, but never shown now

    const u32 before = ads.requests;
    CHECK(h.ads().load("continue") == 0);
    CHECK(h.ads().show("level_end") == 0);
    CHECK(h.ads().show("continue") == 0);
    CHECK(h.ads().show_banner("menu") == 0);
    h.run(10);
    CHECK(ads.requests == before);  // the gate: no ad requests with "remove ads"
    CHECK_FALSE(h.service.fullscreen_ad_showing());
}

TEST_CASE("remove ads: a relaunch with it owned never starts the ad network") {
    {
        FakeStore store;
        CountingAds ads;
        Host h(&store, &ads, "remove_ads_relaunch", true);
        h.store().purchase("remove_ads");
        h.run(2);
        REQUIRE(h.store().owns("remove_ads"));
    }
    FakeStore store;
    CountingAds ads;
    Host h(&store, &ads, "remove_ads_relaunch", false);
    CHECK(h.store().owns("remove_ads"));
    CHECK(h.ads().removed());
    CHECK(ads.starts == 0);  // no consent form, no SDK
    CHECK(h.ads().show_banner("menu") == 0);
    CHECK(h.ads().load("level_end") == 0);
    h.run(5);
    CHECK(ads.requests == 0);
    h.service.shutdown();
}

TEST_CASE("remove ads: the catalog carries the flag, only on non-consumables") {
    const Catalog c = catalog();
    Array<byte> bytes;
    encode_catalog(c, bytes);
    Catalog back;
    REQUIRE(decode_catalog(bytes.span(), back));
    CHECK(back.products()[0].removes_ads);
    CHECK_FALSE(back.products()[1].removes_ads);

    // A consumable that removes ads (it is never owned) does not decode.
    Catalog bad;
    ProductInfo p;
    p.id = "gems";
    p.type = ProductType::Consumable;
    p.removes_ads = true;
    CHECK(bad.add_product(p));
    encode_catalog(bad, bytes);
    Catalog t;
    CHECK_FALSE(decode_catalog(bytes.span(), t));
}
