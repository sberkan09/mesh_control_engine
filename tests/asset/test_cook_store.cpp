// M-MONETIZE step 1: a project's .mcstore catalog cooks to CHNK_STORE, the chunk
// monetize::decode_catalog reads; mistakes are cook errors naming the file.

#include <doctest/doctest.h>

#include "cook_test_util.h"
#include "mce/asset/asset_system.h"
#include "mce/cook/cook.h"
#include "mce/monetize/catalog.h"

using namespace mce;
using namespace cook_test;

namespace {

std::string diags_of(const cook::CookReport& r) {
    std::string s;
    for (const auto& d : r.diags.all()) {
        cook::String line;
        cook::format_diagnostic(d, line);
        s += line.c_str();
        s += "\n";
    }
    return s;
}

cook::CookReport cook_one(const std::string& name, const std::string& mcstore) {
    const std::string root = fresh_dir(name);
    write_file(root + "/assets/store.mcstore", mcstore);
    cook::CookService svc;
    return svc.cook(StringView(root.c_str()), cook::CookOptions{});
}

constexpr const char* kGuid = "5703e000-0000-4000-8000-000000000001";

}  // namespace

TEST_CASE("store catalog: .mcstore cooks to CHNK_STORE and decodes") {
    const std::string root = fresh_dir("store_cook");
    write_file(root + "/assets/store.mcstore", R"({ "format": "mce-store", "version": 1,
        "guid": "5703e000-0000-4000-8000-000000000001",
        "products": [
          { "id": "remove_ads", "type": "non_consumable", "ios": "com.studio.game.remove_ads", "android": "remove_ads",
            "removes_ads": true },
          { "id": "gems_100", "type": "consumable", "android": "gems_100" } ],
        "ads": [
          { "id": "continue", "format": "rewarded" },
          { "id": "level_end", "format": "interstitial", "ios": "ca-app-pub-1/2", "android": "ca-app-pub-1/3" },
          { "id": "menu", "format": "banner" } ],
        "admob": { "ios": "ca-app-pub-1111~2222", "android": "ca-app-pub-1111~3333" } })");
    cook::CookService svc;
    const cook::CookReport r = svc.cook(StringView(root.c_str()), cook::CookOptions{});
    INFO(diags_of(r));
    REQUIRE(r.ok());

    asset::AssetSystem assets;
    const std::string cooked = root + "/cooked";
    const StringView roots[] = {StringView(cooked.c_str())};
    REQUIRE(assets.mount(Span<const StringView>(roots, 1)));
    const asset::AssetKey key = assets.find(asset::parse_guid(kGuid).value());
    REQUIRE(key != asset::kInvalidAssetKey);
    CHECK(assets.info(key).type == asset::ChunkType::StoreCatalog);
    auto h = assets.load_sync<asset::StoreCatalog>(key);
    REQUIRE(h);

    monetize::Catalog c;
    REQUIRE(monetize::decode_catalog(assets.resolve(h.value()), c));
    REQUIRE(c.products().size() == 2);
    CHECK(c.products()[0].id.view() == "remove_ads");
    CHECK(c.products()[0].type == monetize::ProductType::NonConsumable);
    CHECK(c.products()[0].ios_id.view() == "com.studio.game.remove_ads");
    CHECK(c.products()[0].removes_ads);
    CHECK(c.products()[1].type == monetize::ProductType::Consumable);
    CHECK_FALSE(c.products()[1].removes_ads);
    CHECK(c.products()[1].ios_id.empty());
    REQUIRE(c.ads().size() == 3);
    CHECK(c.find_ad("level_end") == 1);
    CHECK(c.ads()[1].format == monetize::AdFormat::Interstitial);
    CHECK(c.ads()[1].android_unit.view() == "ca-app-pub-1/3");
    CHECK(c.ads()[2].format == monetize::AdFormat::Banner);
}

TEST_CASE("store catalog: authoring mistakes are cook errors") {
    const char* const bad[] = {
        // unknown product type
        R"({ "format": "mce-store", "version": 1, "guid": "5703e000-0000-4000-8000-000000000001",
            "products": [ { "id": "gems", "type": "subscription" } ] })",
        // unknown ad format
        R"({ "format": "mce-store", "version": 1, "guid": "5703e000-0000-4000-8000-000000000001",
            "ads": [ { "id": "x", "format": "popup" } ] })",
        // two products with one id
        R"({ "format": "mce-store", "version": 1, "guid": "5703e000-0000-4000-8000-000000000001",
            "products": [ { "id": "gems", "type": "consumable" }, { "id": "gems", "type": "consumable" } ] })",
        // no id
        R"({ "format": "mce-store", "version": 1, "guid": "5703e000-0000-4000-8000-000000000001",
            "products": [ { "type": "consumable" } ] })",
        // an id with a space
        R"({ "format": "mce-store", "version": 1, "guid": "5703e000-0000-4000-8000-000000000001",
            "products": [ { "id": "100 gems", "type": "consumable" } ] })",
        // typo'd key
        R"({ "format": "mce-store", "version": 1, "guid": "5703e000-0000-4000-8000-000000000001",
            "products": [ { "id": "gems", "type": "consumable", "andriod": "gems" } ] })",
        // a consumable cannot remove ads
        R"({ "format": "mce-store", "version": 1, "guid": "5703e000-0000-4000-8000-000000000001",
            "products": [ { "id": "gems", "type": "consumable", "removes_ads": true } ] })",
        // removes_ads is not a bool
        R"({ "format": "mce-store", "version": 1, "guid": "5703e000-0000-4000-8000-000000000001",
            "products": [ { "id": "no_ads", "type": "non_consumable", "removes_ads": "yes" } ] })",
        // an ad unit id where the AdMob app id goes
        R"({ "format": "mce-store", "version": 1, "guid": "5703e000-0000-4000-8000-000000000001",
            "admob": { "android": "ca-app-pub-1111/3333" } })",
        // admob is not an object
        R"({ "format": "mce-store", "version": 1, "guid": "5703e000-0000-4000-8000-000000000001",
            "admob": "ca-app-pub-1111~3333" })",
        // a newer version
        R"({ "format": "mce-store", "version": 2, "guid": "5703e000-0000-4000-8000-000000000001" })",
    };
    for (u32 i = 0; i < sizeof(bad) / sizeof(bad[0]); ++i) {
        CAPTURE(i);
        const cook::CookReport r = cook_one("store_bad", bad[i]);
        CHECK_FALSE(r.ok());
        CHECK(diags_of(r).find("store.mcstore") != std::string::npos);
    }
}
