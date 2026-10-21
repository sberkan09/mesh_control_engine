// M-WORKFLOW 7: a project's .mcdefaults cook to CHNK_CVARS, the "name value" lines console::set_defaults reads;
// mistakes are cook errors naming the file.

#include <doctest/doctest.h>

#include <string>

#include "cook_test_util.h"
#include "mce/asset/asset_system.h"
#include "mce/cook/cook.h"

using namespace mce;
using namespace cook_test;

namespace {

std::string defaults_diags(const cook::CookReport& r) {
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

TEST_CASE("convar defaults: .mcdefaults cooks to CHNK_CVARS as name value lines") {
    const std::string root = fresh_dir("defaults_cook");
    write_file(root + "/assets/project.mcdefaults", R"({ "format": "mce-defaults", "version": 1,
        "guid": "5703e000-0000-4000-8000-0000000000d1",
        "convars": { "ui.reference_height": 720, "access.captions": true, "audio.music_db": -6.5, "game.title": "Under tow",
            "a.min": -2147483648, "a.big": 2147483648 } })");
    cook::CookService svc;
    const cook::CookReport r = svc.cook(StringView(root.c_str()), cook::CookOptions{});
    INFO(defaults_diags(r));
    REQUIRE(r.ok());

    asset::AssetSystem assets;
    const std::string cooked = root + "/cooked";
    const StringView roots[] = {StringView(cooked.c_str())};
    asset::MountOptions game;
    game.allow_code = true;  // the game's own pack
    REQUIRE(assets.mount(Span<const StringView>(roots, 1), game));
    const asset::AssetKey key = assets.find(asset::parse_guid("5703e000-0000-4000-8000-0000000000d1").value());
    REQUIRE(key != asset::kInvalidAssetKey);
    CHECK(assets.info(key).type == asset::ChunkType::ConvarDefaults);
    auto h = assets.load_sync<asset::ConvarDefaultsChunk>(key);
    REQUIRE(h);
    const Span<const byte> bytes = assets.resolve(h.value());
    const std::string text(reinterpret_cast<const char*>(bytes.data()), bytes.size());
    CHECK(text == "ui.reference_height 720\naccess.captions true\naudio.music_db -6.5\ngame.title Under tow\n"
                  "a.min -2147483648\na.big 2147483648\n");
}

TEST_CASE("convar defaults: authoring mistakes are cook errors") {
    const std::string long_text =
        R"({ "format": "mce-defaults", "version": 1, "guid": "5703e000-0000-4000-8000-0000000000d1",
            "convars": { "a.b": ")" + std::string(129, 'x') + R"(" } })";
    const char* const bad[] = {
        // a name with a space
        R"({ "format": "mce-defaults", "version": 1, "guid": "5703e000-0000-4000-8000-0000000000d1",
            "convars": { "ui scale": 2 } })",
        // an object as a value
        R"({ "format": "mce-defaults", "version": 1, "guid": "5703e000-0000-4000-8000-0000000000d1",
            "convars": { "a.b": { "x": 1 } } })",
        // a wrong format
        R"({ "format": "mce-config", "version": 1, "guid": "5703e000-0000-4000-8000-0000000000d1", "convars": {} })",
        // a newline in text
        R"({ "format": "mce-defaults", "version": 1, "guid": "5703e000-0000-4000-8000-0000000000d1",
            "convars": { "a.b": "one\ntwo" } })",
        // text longer than the convar default buffer, and padded text the loader would trim
        long_text.c_str(),
        R"({ "format": "mce-defaults", "version": 1, "guid": "5703e000-0000-4000-8000-0000000000d1",
            "convars": { "a.b": " padded" } })",
    };
    for (const char* text : bad) {
        const std::string root = fresh_dir("defaults_bad");
        write_file(root + "/assets/project.mcdefaults", text);
        cook::CookService svc;
        const cook::CookReport r = svc.cook(StringView(root.c_str()), cook::CookOptions{});
        CHECK_FALSE(r.ok());
        CHECK(defaults_diags(r).find("project.mcdefaults") != std::string::npos);
    }
}

TEST_CASE("convar defaults: two files in a project are a cook error naming both") {
    const std::string root = fresh_dir("defaults_two");
    write_file(root + "/assets/a.mcdefaults", R"({ "format": "mce-defaults", "version": 1,
        "guid": "5703e000-0000-4000-8000-0000000000d1", "convars": { "a.b": 1 } })");
    write_file(root + "/assets/b.mcdefaults", R"({ "format": "mce-defaults", "version": 1,
        "guid": "5703e000-0000-4000-8000-0000000000d2", "convars": { "a.c": 2 } })");
    cook::CookService svc;
    const cook::CookReport r = svc.cook(StringView(root.c_str()), cook::CookOptions{});
    CHECK_FALSE(r.ok());
    const std::string d = defaults_diags(r);
    CHECK(d.find("one convar-defaults file") != std::string::npos);
    CHECK(d.find("a.mcdefaults") != std::string::npos);
    CHECK(d.find("b.mcdefaults") != std::string::npos);
}

TEST_CASE("convar defaults: a project without one cooks no such chunk (old packs behave as before)") {
    const std::string root = fresh_dir("defaults_none");
    write_file(root + "/assets/x.mcconfig", R"({ "format": "mce-config", "version": 1,
        "guid": "5703e000-0000-4000-8000-0000000000d3", "values": {} })");
    cook::CookService svc;
    const cook::CookReport r = svc.cook(StringView(root.c_str()), cook::CookOptions{});
    INFO(defaults_diags(r));
    REQUIRE(r.ok());
    asset::AssetSystem assets;
    const std::string cooked = root + "/cooked";
    const StringView roots[] = {StringView(cooked.c_str())};
    REQUIRE(assets.mount(Span<const StringView>(roots, 1)));
    for (asset::AssetKey k = 0; k < assets.asset_count(); ++k) CHECK(assets.info(k).type != asset::ChunkType::ConvarDefaults);
}

TEST_CASE("convar defaults: a mod's pack cannot set convar defaults") {
    const std::string root = fresh_dir("defaults_mod");
    write_file(root + "/assets/project.mcdefaults", R"({ "format": "mce-defaults", "version": 1,
        "guid": "5703e000-0000-4000-8000-0000000000d4", "convars": { "access_log": "/tmp/x" } })");
    cook::CookService svc;
    REQUIRE(svc.cook(StringView(root.c_str()), cook::CookOptions{}).ok());
    asset::AssetSystem assets;
    const std::string cooked = root + "/cooked";
    const StringView roots[] = {StringView(cooked.c_str())};
    REQUIRE(assets.mount(Span<const StringView>(roots, 1)));  // default options: data only
    CHECK(assets.find(asset::parse_guid("5703e000-0000-4000-8000-0000000000d4").value()) == asset::kInvalidAssetKey);
}
