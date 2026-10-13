// M-MONETIZE step 3: the platform store provider. Off a phone mce_platform's store is "none", so every
// request is refused with its closing answer; the answers a phone's store sends (Play Billing,
// StoreKit 2) are mapped to the catalog by the platform id and finished the way the product needs.

#include <doctest/doctest.h>

#include "mce/monetize/platform_store.h"
#include "mce/platform/store.h"

using namespace mce;
using namespace mce::monetize;

namespace {

Catalog catalog() {
    Catalog c;
    ProductInfo p;
    p.id = "remove_ads";
    p.android_id = "com.example.remove_ads";
    p.ios_id = "com.example.ios.remove_ads";
    p.type = ProductType::NonConsumable;
    CHECK(c.add_product(p));
    p = ProductInfo{};
    p.id = "gems";  // no store ids: the catalog id is used
    p.type = ProductType::Consumable;
    CHECK(c.add_product(p));
    return c;
}

struct Collect final : Sink {
    Array<StoreAnswer> store;
    void post(const StoreAnswer& a) override { store.push_back(a); }
    void post(const AdAnswer&) override {}
};

platform::store::Answer answer(platform::store::AnswerKind k, u32 request, StringView id, StringView transaction = {}) {
    platform::store::Answer a;
    a.kind = k;
    a.request = request;
    a.id = id;
    a.transaction = transaction;
    return a;
}

}  // namespace

TEST_CASE("platform store: off a phone there is no store and every request gets its closing answer") {
    CHECK(platform::store::backend() == platform::store::Backend::None);
    const Catalog c = catalog();
    Collect sink;
    PlatformStore store;
    CHECK_FALSE(store.available());
    CHECK(StringView(store.name()) == "none");
    store.start(c, sink);
    store.query_products(1);
    store.purchase(0, 2);
    store.restore(3);
    REQUIRE(sink.store.size() == 3);
    CHECK(sink.store[0].kind == StoreEventKind::ProductsReady);
    CHECK(sink.store[0].request == 1);
    CHECK(sink.store[1].kind == StoreEventKind::Failed);
    CHECK(sink.store[1].product == 0);
    CHECK(sink.store[1].request == 2);
    CHECK(sink.store[2].kind == StoreEventKind::RestoreFinished);
    CHECK(sink.store[2].request == 3);
    store.stop();
}

TEST_CASE("platform store: answers map to the catalog by the store's product id") {
    const Catalog c = catalog();
    Collect sink;
    PlatformStore store;
    store.start(c, sink);
    using K = platform::store::AnswerKind;
    // Off a phone the backend is none, which uses the Android ids (the same rule as Play's).
    CHECK(store.store_id(0) == "com.example.remove_ads");
    CHECK(store.store_id(1) == "gems");
    CHECK(store.store_id(7).empty());

    platform::store::Answer info = answer(K::ProductInfo, 4, "com.example.remove_ads");
    info.price = "₺49,99";
    store.answer(info);
    store.answer(answer(K::ProductInfo, 4, "gems"));
    store.answer(answer(K::ProductInfo, 4, "remove_ads"));  // the game's id is not the store's: ignored
    store.answer(answer(K::ProductsReady, 4, {}));
    REQUIRE(sink.store.size() == 3);
    CHECK(sink.store[0].kind == StoreEventKind::ProductInfo);
    CHECK(sink.store[0].product == 0);
    CHECK(sink.store[0].price.view() == "₺49,99");
    CHECK(sink.store[1].product == 1);
    CHECK(sink.store[2].kind == StoreEventKind::ProductsReady);
    CHECK(sink.store[2].product == kNoIndex);

    // A refusal for an id the catalog does not know still answers the request.
    store.answer(answer(K::Failed, 5, "unknown"));
    REQUIRE(sink.store.size() == 4);
    CHECK(sink.store[3].kind == StoreEventKind::Failed);
    CHECK(sink.store[3].product == kNoIndex);
    CHECK(sink.store[3].request == 5);
    store.stop();
}

TEST_CASE("platform store: long Play purchase tokens survive, consumables are never restored") {
    const Catalog c = catalog();
    Collect sink;
    PlatformStore store;
    store.start(c, sink);
    using K = platform::store::AnswerKind;
    FixedString<512> token;
    for (int i = 0; i < 30; ++i) token.append("abcdefghi.");  // 300 bytes, like a real token
    store.answer(answer(K::Purchased, 6, "gems", token.view()));
    store.answer(answer(K::Purchased, 0, "gems", token.view()));  // the start-up query sees it again: dropped
    store.answer(answer(K::Restored, 7, "gems", "other"));       // a consumable is not restorable
    store.answer(answer(K::Restored, 7, "com.example.remove_ads", "owned-1"));
    REQUIRE(sink.store.size() == 2);
    CHECK(sink.store[0].kind == StoreEventKind::Purchased);
    CHECK(sink.store[0].product == 1);
    CHECK(sink.store[0].transaction.view() == token.view());
    CHECK_FALSE(sink.store[0].transaction.truncated());
    CHECK(sink.store[1].kind == StoreEventKind::Restored);
    CHECK(sink.store[1].product == 0);

    FixedString<600> too_long;
    for (int i = 0; i < 60; ++i) too_long.append("0123456789");
    store.answer(answer(K::Purchased, 8, "gems", too_long.view()));  // cannot be finished later: refused
    CHECK(sink.store.size() == 2);

    // Finishing a reported transaction and an unknown one: neither reaches a store here, neither crashes.
    store.finish(token.view());
    store.finish("never-reported");
    store.stop();
}
