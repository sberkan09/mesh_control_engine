// M-LIVEOPS step 3: a project's .mcconfig remote config defaults cook to CHNK_RCFG, the chunk
// liveops::decode_config reads; mistakes are cook errors naming the file.

#include <doctest/doctest.h>

#include "cook_test_util.h"
#include "importer_common.h"
#include "mce/asset/asset_system.h"
#include "mce/cook/cook.h"
#include "mce/cook/json.h"
#include "mce/liveops/remote_config.h"

using namespace mce;
using namespace cook_test;

namespace {

std::string config_diags(const cook::CookReport& r) {
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

TEST_CASE("remote config: .mcconfig cooks to CHNK_RCFG and decodes with its types") {
    const std::string root = fresh_dir("config_cook");
    write_file(root + "/assets/remote.mcconfig", R"({ "format": "mce-config", "version": 1,
        "guid": "5703e000-0000-4000-8000-0000000000c1",
        "values": { "ad_every_levels": 3, "gem_bonus": 1.5, "winter_event": false, "motd": "Welcome",
                    "drop_rate": { "type": "float", "value": 1 }, "big": -9007199254740992 } })");
    cook::CookService svc;
    const cook::CookReport r = svc.cook(StringView(root.c_str()), cook::CookOptions{});
    INFO(config_diags(r));
    REQUIRE(r.ok());

    asset::AssetSystem assets;
    const std::string cooked = root + "/cooked";
    const StringView roots[] = {StringView(cooked.c_str())};
    REQUIRE(assets.mount(Span<const StringView>(roots, 1)));
    const asset::AssetKey key = assets.find(asset::parse_guid("5703e000-0000-4000-8000-0000000000c1").value());
    REQUIRE(key != asset::kInvalidAssetKey);
    CHECK(assets.info(key).type == asset::ChunkType::RemoteConfig);
    auto h = assets.load_sync<asset::RemoteConfigChunk>(key);
    REQUIRE(h);
    liveops::ConfigValues v;
    REQUIRE(liveops::decode_config(assets.resolve(h.value()), v));
    REQUIRE(v.size() == 6);
    REQUIRE(v.find("ad_every_levels"));
    CHECK(v.find("ad_every_levels")->type == liveops::ConfigType::Int);
    CHECK(v.find("ad_every_levels")->i == 3);
    CHECK(v.find("gem_bonus")->type == liveops::ConfigType::Float);
    CHECK(v.find("gem_bonus")->f == 1.5);
    CHECK(v.find("winter_event")->type == liveops::ConfigType::Bool);
    CHECK(v.find("winter_event")->i == 0);
    CHECK(v.find("motd")->s.view() == "Welcome");
    CHECK(v.find("drop_rate")->type == liveops::ConfigType::Float);
    CHECK(v.find("drop_rate")->f == 1.0);
    CHECK(v.find("big")->i == -9007199254740992ll);
}

TEST_CASE("remote config: authoring mistakes are cook errors") {
    const char* const bad[] = {
        // a key with a space
        R"({ "format": "mce-config", "version": 1, "guid": "5703e000-0000-4000-8000-0000000000c1",
            "values": { "ad every": 3 } })",
        // an unknown type
        R"({ "format": "mce-config", "version": 1, "guid": "5703e000-0000-4000-8000-0000000000c1",
            "values": { "x": { "type": "double", "value": 1 } } })",
        // a value that is not its type
        R"({ "format": "mce-config", "version": 1, "guid": "5703e000-0000-4000-8000-0000000000c1",
            "values": { "x": { "type": "int", "value": 1.5 } } })",
        // an array value
        R"({ "format": "mce-config", "version": 1, "guid": "5703e000-0000-4000-8000-0000000000c1",
            "values": { "x": [1, 2] } })",
        // a typo'd key in the typed form
        R"({ "format": "mce-config", "version": 1, "guid": "5703e000-0000-4000-8000-0000000000c1",
            "values": { "x": { "type": "int", "valeu": 1 } } })",
        // an int JSON cannot hold exactly
        R"({ "format": "mce-config", "version": 1, "guid": "5703e000-0000-4000-8000-0000000000c1",
            "values": { "x": 90071992547409930 } })",
        // values is not an object
        R"({ "format": "mce-config", "version": 1, "guid": "5703e000-0000-4000-8000-0000000000c1", "values": [] })",
        // a newer version
        R"({ "format": "mce-config", "version": 2, "guid": "5703e000-0000-4000-8000-0000000000c1" })",
    };
    for (u32 i = 0; i < sizeof(bad) / sizeof(bad[0]); ++i) {
        CAPTURE(i);
        const std::string root = fresh_dir("config_bad");
        write_file(root + "/assets/remote.mcconfig", bad[i]);
        cook::CookService svc;
        const cook::CookReport r = svc.cook(StringView(root.c_str()), cook::CookOptions{});
        CHECK_FALSE(r.ok());
        CHECK(config_diags(r).find("remote.mcconfig") != std::string::npos);
    }
}

// M-FORMATVER 1: the one version check every asset text importer shares.
TEST_CASE("check_format_version accepts 1..current, refuses 0, newer and non-integers, and says what it read") {
    auto check = [](const char* text, u32 current, std::string* msg = nullptr) {
        cook::JsonDoc doc;
        cook::JsonError err;
        REQUIRE(doc.parse(text, err));
        cook::DiagList diags;
        const bool ok = cook::detail::check_format_version(doc.root(), current, "thing", "a.mcthing", "stage", diags);
        if (msg != nullptr) {
            msg->clear();
            for (const auto& d : diags.all()) {
                cook::String line;
                cook::format_diagnostic(d, line);
                *msg += line.c_str();
            }
        }
        CHECK(ok == diags.all().empty());
        return ok;
    };
    CHECK(check(R"({"version": 1})", 1));
    CHECK(check(R"({"version": 1})", 2));  // an older file still reads after a bump
    CHECK(check(R"({"version": 2})", 2));
    std::string msg;
    CHECK_FALSE(check(R"({"version": 2})", 1, &msg));  // newer than this cook
    CHECK(msg.find("unsupported thing version (this cook reads version 1)") != std::string::npos);
    CHECK(msg.find("a.mcthing") != std::string::npos);
    CHECK_FALSE(check(R"({"version": 0})", 2));
    CHECK_FALSE(check(R"({"version": 1.5})", 2));
    CHECK_FALSE(check(R"({"version": "1"})", 2));
    CHECK_FALSE(check(R"({})", 2));
}
