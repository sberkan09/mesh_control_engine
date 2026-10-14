// M-LIVEOPS step 2: a project's .mcservices game services catalog cooks to CHNK_GSVC, the chunk
// liveops::decode_services_catalog reads; mistakes are cook errors naming the file.

#include <doctest/doctest.h>

#include "cook_test_util.h"
#include "mce/asset/asset_system.h"
#include "mce/cook/cook.h"
#include "mce/liveops/game_services.h"

using namespace mce;
using namespace cook_test;

namespace {

std::string services_diags(const cook::CookReport& r) {
    std::string s;
    for (const auto& d : r.diags.all()) {
        cook::String line;
        cook::format_diagnostic(d, line);
        s += line.c_str();
        s += "\n";
    }
    return s;
}

}  // namespace

TEST_CASE("game services: .mcservices cooks to CHNK_GSVC and decodes") {
    const std::string root = fresh_dir("services_cook");
    write_file(root + "/assets/game.mcservices", R"({ "format": "mce-services", "version": 1,
        "guid": "5703e000-0000-4000-8000-0000000000d1",
        "achievements": [ { "id": "first_win", "ios": "com.studio.first_win", "android": "CgkIfirst" },
                          { "id": "win_10", "steps": 10 } ],
        "leaderboards": [ { "id": "best_time", "order": "lower_is_better", "android": "CgkIbest" },
                          { "id": "score" } ],
        "cloud_saves": [ "progress", "settings" ] })");
    cook::CookService svc;
    const cook::CookReport r = svc.cook(StringView(root.c_str()), cook::CookOptions{});
    INFO(services_diags(r));
    REQUIRE(r.ok());

    asset::AssetSystem assets;
    const std::string cooked = root + "/cooked";
    const StringView roots[] = {StringView(cooked.c_str())};
    REQUIRE(assets.mount(Span<const StringView>(roots, 1)));
    const asset::AssetKey key = assets.find(asset::parse_guid("5703e000-0000-4000-8000-0000000000d1").value());
    REQUIRE(key != asset::kInvalidAssetKey);
    CHECK(assets.info(key).type == asset::ChunkType::GameServicesCatalog);
    auto h = assets.load_sync<asset::GameServicesCatalogChunk>(key);
    REQUIRE(h);
    liveops::ServicesCatalog c;
    REQUIRE(liveops::decode_services_catalog(assets.resolve(h.value()), c));
    REQUIRE(c.achievements().size() == 2);
    CHECK(c.achievements()[0].ios_id.view() == "com.studio.first_win");
    CHECK(c.achievements()[0].steps == 1);
    CHECK(c.achievements()[1].steps == 10);
    REQUIRE(c.leaderboards().size() == 2);
    CHECK(c.leaderboards()[0].lower_is_better);
    CHECK(c.leaderboards()[0].android_id.view() == "CgkIbest");
    CHECK_FALSE(c.leaderboards()[1].lower_is_better);
    CHECK(c.find_cloud_save("settings") == 1);
}

TEST_CASE("game services: authoring mistakes are cook errors") {
    const char* const bad[] = {
        // a duplicate achievement id
        R"({ "format": "mce-services", "version": 1, "guid": "5703e000-0000-4000-8000-0000000000d1",
            "achievements": [ { "id": "a" }, { "id": "a" } ] })",
        // zero steps
        R"({ "format": "mce-services", "version": 1, "guid": "5703e000-0000-4000-8000-0000000000d1",
            "achievements": [ { "id": "a", "steps": 0 } ] })",
        // an id with a space
        R"({ "format": "mce-services", "version": 1, "guid": "5703e000-0000-4000-8000-0000000000d1",
            "leaderboards": [ { "id": "best time" } ] })",
        // an unknown order
        R"({ "format": "mce-services", "version": 1, "guid": "5703e000-0000-4000-8000-0000000000d1",
            "leaderboards": [ { "id": "t", "order": "ascending" } ] })",
        // a typo'd key
        R"({ "format": "mce-services", "version": 1, "guid": "5703e000-0000-4000-8000-0000000000d1",
            "achievements": [ { "id": "a", "step": 3 } ] })",
        // a cloud save that is not a name
        R"({ "format": "mce-services", "version": 1, "guid": "5703e000-0000-4000-8000-0000000000d1",
            "cloud_saves": [ "my/progress" ] })",
        // a cloud save listed twice
        R"({ "format": "mce-services", "version": 1, "guid": "5703e000-0000-4000-8000-0000000000d1",
            "cloud_saves": [ "p", "p" ] })",
        // a newer version
        R"({ "format": "mce-services", "version": 2, "guid": "5703e000-0000-4000-8000-0000000000d1" })",
    };
    for (u32 i = 0; i < sizeof(bad) / sizeof(bad[0]); ++i) {
        CAPTURE(i);
        const std::string root = fresh_dir("services_bad");
        write_file(root + "/assets/game.mcservices", bad[i]);
        cook::CookService svc;
        const cook::CookReport r = svc.cook(StringView(root.c_str()), cook::CookOptions{});
        CHECK_FALSE(r.ok());
        CHECK(services_diags(r).find("game.mcservices") != std::string::npos);
    }
}
