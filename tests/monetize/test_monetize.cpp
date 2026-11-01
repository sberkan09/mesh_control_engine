// M-MONETIZE step 1: the platform-neutral purchase and ad services (mce/monetize/monetize.h).
// A scripted provider stands in for the store and the ad network; the answers it posts must enter
// the sim only at tick starts, keep entitlements across a relaunch, finish consumables only after
// the game grants them, pause for fullscreen ads, and replay to the same state hash.

#include <doctest/doctest.h>

#include <filesystem>
#include <thread>

#include "mce/ecs/save.h"
#include "mce/monetize/monetize.h"

using namespace mce;
using namespace mce::monetize;

namespace {

struct Call {
    enum Kind : u8 { Query, Purchase, Restore, Finish, Load, Show, Banner, HideBanner } kind;
    u16 index = kNoIndex;
    u32 request = 0;
    FixedString<128> text;
    BannerEdge edge = BannerEdge::Bottom;
    SafeInsets insets;
};

// Records every call; answers only when the test says so.
class ScriptedStore final : public IStoreProvider {
public:
    const char* name() const override { return "scripted"; }
    void start(const Catalog&, Sink& s) override { sink = &s; }
    void stop() override { sink = nullptr; }
    void query_products(u32 request) override { calls.push_back(Call{Call::Query, kNoIndex, request, {}, {}, {}}); }
    void purchase(u16 product, u32 request) override { calls.push_back(Call{Call::Purchase, product, request, {}, {}, {}}); }
    void restore(u32 request) override { calls.push_back(Call{Call::Restore, kNoIndex, request, {}, {}, {}}); }
    void finish(StringView transaction) override { calls.push_back(Call{Call::Finish, kNoIndex, 0, transaction, {}, {}}); }

    void answer(StoreEventKind k, u16 product, u32 request, StringView transaction = {}, StringView price = {}) {
        StoreAnswer a;
        a.kind = k;
        a.product = product;
        a.request = request;
        a.transaction = transaction;
        a.price = price;
        sink->post(a);
    }
    u32 count(Call::Kind k) const {
        u32 n = 0;
        for (const Call& c : calls) n += c.kind == k ? 1u : 0u;
        return n;
    }

    Sink* sink = nullptr;
    Array<Call> calls;
};

class ScriptedAds final : public IAdProvider {
public:
    const char* name() const override { return "scripted"; }
    void start(const Catalog&, Sink& s) override { sink = &s; }
    void stop() override { sink = nullptr; }
    void load(u16 placement, u32 request) override { calls.push_back(Call{Call::Load, placement, request, {}, {}, {}}); }
    void show(u16 placement, u32 request) override { calls.push_back(Call{Call::Show, placement, request, {}, {}, {}}); }
    void show_banner(u16 placement, u32 request, BannerEdge edge, const SafeInsets& insets) override {
        calls.push_back(Call{Call::Banner, placement, request, {}, edge, insets});
    }
    void hide_banner() override { calls.push_back(Call{Call::HideBanner, kNoIndex, 0, {}, {}, {}}); }

    void answer(AdEventKind k, u16 placement, u32 request, u32 reward = 0) {
        AdAnswer a;
        a.kind = k;
        a.placement = placement;
        a.request = request;
        a.reward_amount = reward;
        sink->post(a);
    }

    Sink* sink = nullptr;
    Array<Call> calls;
};

Catalog make_catalog() {
    Catalog c;
    ProductInfo remove_ads;
    remove_ads.id = "remove_ads";
    remove_ads.type = ProductType::NonConsumable;
    remove_ads.ios_id = "com.test.remove_ads";
    CHECK(c.add_product(remove_ads));
    ProductInfo gems;
    gems.id = "gems_100";
    gems.type = ProductType::Consumable;
    CHECK(c.add_product(gems));
    AdPlacementInfo cont;
    cont.id = "continue";
    cont.format = AdFormat::Rewarded;
    CHECK(c.add_ad(cont));
    AdPlacementInfo level_end;
    level_end.id = "level_end";
    level_end.format = AdFormat::Interstitial;
    CHECK(c.add_ad(level_end));
    AdPlacementInfo menu;
    menu.id = "menu";
    menu.format = AdFormat::Banner;
    CHECK(c.add_ad(menu));
    return c;
}

void fresh_save_root(const char* name) {
    const std::filesystem::path p = std::filesystem::path(MCE_TEST_TMP_DIR) / name;
    std::error_code ec;
    std::filesystem::remove_all(p, ec);
    std::filesystem::create_directories(p, ec);
    save::set_root(StringView(p.string().c_str()));
}

// What a game keeps: hashed by World::state_hash, so the replay test sees every reaction.
struct Wallet {
    u32 gems = 0;
    u32 rewards = 0;
    u32 purchases = 0;
    u32 ads_closed = 0;
};

Array<StoreEvent> g_store_seen;
Array<AdEvent> g_ad_seen;

// The game: grants gems, then finishes the transaction; counts rewards and closes.
void game_post(Frame& f) {
    Store& store = f.resource<Store>();
    for (const StoreEvent& e : f.events().read<StoreEvent>()) {
        g_store_seen.push_back(e);
        if (e.kind != StoreEventKind::Purchased) continue;
        f.query<Wallet&>().each([&](Wallet& w) {
            ++w.purchases;
            if (e.product == store.find("gems_100")) w.gems += 100;
        });
        if (e.product == store.find("gems_100")) store.finish(e.transaction);
    }
    for (const AdEvent& e : f.events().read<AdEvent>()) {
        g_ad_seen.push_back(e);
        f.query<Wallet&>().each([&](Wallet& w) {
            if (e.kind == AdEventKind::Rewarded) w.rewards += e.reward_amount;
            if (e.kind == AdEventKind::Closed) ++w.ads_closed;
        });
    }
}

struct Game {
    World world;
    Service service;
    u64 tick = 0;

    void start(IStoreProvider* store, IAdProvider* ads, const ServiceOptions& opt = {}) {
        REQUIRE(service.init(make_catalog(), store, ads, opt));
        service.install(world);
        world.add_system(Phase::FixedUpdate_Post, "game_post", &game_post);
        world.spawn_with(Wallet{});
        g_store_seen.clear();
        g_ad_seen.clear();
    }
    void step() { world.tick(1.0 / 60, ++tick); }
    Store& store() { return world.resource<Store>(); }
    Ads& ads() { return world.resource<Ads>(); }
    Wallet wallet() {
        Wallet out;
        world.query<Wallet&>().each([&](Wallet& w) { out = w; });
        return out;
    }
};

}  // namespace

TEST_CASE("catalog: encode / decode round trip and rejects bad payloads") {
    const Catalog c = make_catalog();
    Array<byte> bytes;
    encode_catalog(c, bytes);
    Catalog back;
    REQUIRE(decode_catalog(bytes.span(), back));
    REQUIRE(back.products().size() == 2);
    CHECK(back.products()[0].id.view() == "remove_ads");
    CHECK(back.products()[0].ios_id.view() == "com.test.remove_ads");
    CHECK(back.products()[1].type == ProductType::Consumable);
    REQUIRE(back.ads().size() == 3);
    CHECK(back.ads()[2].format == AdFormat::Banner);
    CHECK(back.find_product("nope") == kNoIndex);

    for (usize cut = 0; cut < bytes.size(); ++cut) {
        Catalog t;
        CHECK_FALSE(decode_catalog(Span<const byte>(bytes.data(), cut), t));
    }
    Catalog dup;
    CHECK_FALSE(dup.add_product(ProductInfo{}));  // empty id
    ProductInfo p;
    p.id = "x";
    CHECK(dup.add_product(p));
    CHECK_FALSE(dup.add_product(p));
}

TEST_CASE("store: answers enter at the next tick start, consumables finish only after the grant") {
    fresh_save_root("monetize_consumable");
    ScriptedStore store;
    ScriptedAds ads;
    Game g;
    g.start(&store, &ads);
    CHECK(g.store().available());

    const u32 q = g.store().query_products();
    REQUIRE(store.count(Call::Query) == 1);
    store.answer(StoreEventKind::ProductInfo, 1, q, {}, "$0.99");
    store.answer(StoreEventKind::ProductsReady, kNoIndex, q);
    CHECK(g.store().price(1).empty());  // nothing enters between ticks
    g.step();
    CHECK(g.store().price(1) == StringView("$0.99"));
    REQUIRE(g_store_seen.size() == 2);
    CHECK(g_store_seen[1].kind == StoreEventKind::ProductsReady);
    CHECK(g_store_seen[1].request == q);

    const u32 req = g.store().purchase("gems_100");
    REQUIRE(req != 0);
    REQUIRE(store.count(Call::Purchase) == 1);
    CHECK(store.calls[1].index == 1);
    // The store answers from its own thread.
    std::thread t([&] { store.answer(StoreEventKind::Purchased, 1, req, "GPA.1234"); });
    t.join();
    // The game grants and finishes in the same tick it reads the event (FixedUpdate_Post).
    CHECK(store.count(Call::Finish) == 0);
    g.step();
    CHECK(g.wallet().gems == 100);
    CHECK(g.store().unfinished_count() == 0);
    REQUIRE(store.count(Call::Finish) == 1);
    CHECK(store.calls.back().text.view() == "GPA.1234");
    CHECK_FALSE(g.store().owns("gems_100"));  // consumables are never entitlements

    // Unknown products are rejected without a store call.
    CHECK(g.store().purchase("unknown") == 0);
    CHECK(store.count(Call::Purchase) == 1);
}

TEST_CASE("store: a non-consumable stays owned after a relaunch, and restore brings it back") {
    fresh_save_root("monetize_entitlements");
    {
        ScriptedStore store;
        Game g;
        g.start(&store, nullptr);
        const u32 req = g.store().purchase("remove_ads");
        store.answer(StoreEventKind::Cancelled, 0, req);
        g.step();
        CHECK_FALSE(g.store().owns("remove_ads"));
        const u32 again = g.store().purchase("remove_ads");
        store.answer(StoreEventKind::Purchased, 0, again, "T-1");
        CHECK(store.count(Call::Finish) == 0);
        g.step();
        CHECK(g.store().owns("remove_ads"));
        CHECK(store.count(Call::Finish) == 1);  // the service finishes non-consumables itself
        CHECK(g.store().purchase("remove_ads") == 0);  // already owned: no second charge
    }
    {
        ScriptedStore store;  // relaunch: no network answer at all
        Game g;
        g.start(&store, nullptr);
        CHECK(g.store().owns("remove_ads"));
    }
    fresh_save_root("monetize_restore");  // a reinstall: no local entitlements
    {
        ScriptedStore store;
        Game g;
        g.start(&store, nullptr);
        CHECK_FALSE(g.store().owns("remove_ads"));
        const u32 req = g.store().restore_purchases();
        store.answer(StoreEventKind::Restored, 0, req, "T-1");
        store.answer(StoreEventKind::Restored, 1, req, "T-2");  // a consumable: stores never restore these
        store.answer(StoreEventKind::RestoreFinished, kNoIndex, req);
        g.step();
        CHECK(g.store().owns("remove_ads"));
        REQUIRE(g_store_seen.size() == 2);
        CHECK(g_store_seen[0].kind == StoreEventKind::Restored);
        CHECK(g_store_seen[1].kind == StoreEventKind::RestoreFinished);
    }
}

TEST_CASE("store: a consumable the store redelivers at launch is granted, then finished") {
    fresh_save_root("monetize_unfinished");
    ScriptedStore store;
    Game g;
    g.start(&store, nullptr);
    // Redelivered at launch (the game crashed before granting last time): no request id.
    store.answer(StoreEventKind::Purchased, 1, 0, "GPA.old");
    g.step();
    CHECK(g.wallet().gems == 100);
    CHECK(store.count(Call::Finish) == 1);
}

TEST_CASE("ads: fullscreen state flips on the provider's word, rewards and closes enter at ticks") {
    fresh_save_root("monetize_ads");
    ScriptedAds ads;
    Game g;
    g.start(nullptr, &ads);
    CHECK_FALSE(g.store().available());
    CHECK(g.ads().available());

    CHECK(g.ads().show("menu") == 0);         // a banner is not fullscreen
    CHECK(g.ads().show_banner("continue") == 0);  // and the other way round
    g.service.set_safe_insets(SafeInsets{0, 88, 0, 34});
    const u32 b = g.ads().show_banner("menu", BannerEdge::Bottom);
    REQUIRE(b != 0);
    REQUIRE(ads.calls.size() == 1);
    CHECK(ads.calls[0].kind == Call::Banner);
    CHECK(ads.calls[0].insets.bottom == 34);
    CHECK(ads.calls[0].insets.top == 88);

    const u32 l = g.ads().load("continue");
    ads.answer(AdEventKind::Loaded, 0, l);
    g.step();
    CHECK(g.ads().ready("continue"));
    const u32 s = g.ads().show("continue");
    CHECK_FALSE(g.ads().ready("continue"));
    ads.answer(AdEventKind::Shown, 0, s);
    CHECK(g.service.fullscreen_ad_showing());  // at once: the host pauses before the next tick
    ads.answer(AdEventKind::Rewarded, 0, s, 5);
    ads.answer(AdEventKind::Closed, 0, s);
    CHECK_FALSE(g.service.fullscreen_ad_showing());
    g.step();
    CHECK(g.wallet().rewards == 5);
    CHECK(g.wallet().ads_closed == 1);
    REQUIRE(g_ad_seen.size() == 4);
    CHECK(g_ad_seen[1].kind == AdEventKind::Shown);
    CHECK(g_ad_seen[1].format == AdFormat::Rewarded);
    CHECK(g_ad_seen[2].request == s);

    // A banner's Shown never pauses.
    ads.answer(AdEventKind::Shown, 2, b);
    CHECK_FALSE(g.service.fullscreen_ad_showing());
}

TEST_CASE("null providers: purchases fail, restore finds nothing, ads never fill") {
    fresh_save_root("monetize_null");
    Game g;
    g.start(nullptr, nullptr);
    CHECK_FALSE(g.store().available());
    CHECK_FALSE(g.ads().available());
    const u32 p = g.store().purchase("gems_100");
    const u32 r = g.store().restore_purchases();
    const u32 a = g.ads().show("level_end");
    g.step();
    REQUIRE(g_store_seen.size() == 2);
    CHECK(g_store_seen[0].kind == StoreEventKind::Failed);
    CHECK(g_store_seen[0].request == p);
    CHECK(g_store_seen[1].kind == StoreEventKind::RestoreFinished);
    CHECK(g_store_seen[1].request == r);
    REQUIRE(g_ad_seen.size() == 1);
    CHECK(g_ad_seen[0].kind == AdEventKind::NoFill);
    CHECK(g_ad_seen[0].request == a);
    CHECK(g.wallet().gems == 0);
}

TEST_CASE("replay: store and ad events replay to the same state hash with no provider") {
    fresh_save_root("monetize_replay");
    Recorder rec;
    u64 live_hash = 0;
    u32 live_finishes = 0;
    {
        ScriptedStore store;
        ScriptedAds ads;
        Game g;
        g.start(&store, &ads);
        g.service.set_recorder(&rec);
        for (u32 i = 0; i < 120; ++i) {
            if (i == 10) store.answer(StoreEventKind::Purchased, 1, g.store().purchase("gems_100"), "A");
            if (i == 30) {
                const u32 s = g.ads().show("level_end");
                ads.answer(AdEventKind::Shown, 1, s);
                ads.answer(AdEventKind::Closed, 1, s);
            }
            if (i == 50) {
                const u32 s = g.ads().show("continue");
                ads.answer(AdEventKind::Shown, 0, s);
                ads.answer(AdEventKind::Rewarded, 0, s, 2);
                ads.answer(AdEventKind::Closed, 0, s);
            }
            if (i == 70) store.answer(StoreEventKind::Purchased, 0, g.store().purchase("remove_ads"), "B");
            g.step();
        }
        live_hash = g.world.state_hash();
        live_finishes = store.count(Call::Finish);
        CHECK(g.wallet().gems == 100);
        CHECK(g.wallet().rewards == 2);
        CHECK(g.wallet().ads_closed == 2);
    }
    CHECK(live_finishes == 2);

    fresh_save_root("monetize_replay_2");  // the replay must not need the live run's files
    {
        ScriptedStore store;
        ScriptedAds ads;
        Game g;
        g.start(&store, &ads);
        Replayer rp(rec.bytes());
        g.service.set_replayer(&rp);
        for (u32 i = 0; i < 120; ++i) {
            if (i == 10) g.store().purchase("gems_100");
            if (i == 30) g.ads().show("level_end");
            if (i == 50) g.ads().show("continue");
            if (i == 70) g.store().purchase("remove_ads");
            g.step();
        }
        CHECK(rp.done());
        CHECK_FALSE(rp.corrupt());
        CHECK(g.world.state_hash() == live_hash);
        CHECK(g.store().owns("remove_ads"));
        CHECK(store.calls.empty());  // no provider was asked anything
        CHECK(ads.calls.empty());
        CHECK_FALSE(save::exists("entitlements"));  // and nothing was persisted
    }
}

TEST_CASE("monetize: product types, ad formats and event kinds have stable names, and the names parse back") {
    for (ProductType t : {ProductType::Consumable, ProductType::NonConsumable}) {
        ProductType back = t == ProductType::Consumable ? ProductType::NonConsumable : ProductType::Consumable;
        REQUIRE(parse_product_type(StringView(product_type_name(t)), back));
        CHECK(back == t);
    }
    for (AdFormat f : {AdFormat::Rewarded, AdFormat::Interstitial, AdFormat::Banner}) {
        AdFormat back = f == AdFormat::Banner ? AdFormat::Rewarded : AdFormat::Banner;
        REQUIRE(parse_ad_format(StringView(ad_format_name(f)), back));
        CHECK(back == f);
    }
    // A bad name is refused and leaves the output alone.
    ProductType pt = ProductType::NonConsumable;
    CHECK_FALSE(parse_product_type("Consumable", pt));  // case matters
    CHECK_FALSE(parse_product_type("", pt));
    CHECK(pt == ProductType::NonConsumable);
    AdFormat af = AdFormat::Banner;
    CHECK_FALSE(parse_ad_format("video", af));
    CHECK(af == AdFormat::Banner);

    CHECK(StringView(store_event_name(StoreEventKind::Purchased)) == StringView("Purchased"));
    CHECK(StringView(store_event_name(StoreEventKind::RestoreFinished)) == StringView("RestoreFinished"));
    CHECK(StringView(ad_event_name(AdEventKind::Rewarded)) == StringView("Rewarded"));
    CHECK(StringView(ad_event_name(AdEventKind::NoFill)) == StringView("NoFill"));
}

TEST_CASE("monetize: the null providers report themselves unavailable") {
    CHECK(StringView(null_store().name()) == StringView("none"));
    CHECK(StringView(null_ads().name()) == StringView("none"));
    CHECK_FALSE(null_store().available());
    CHECK_FALSE(null_ads().available());
}
