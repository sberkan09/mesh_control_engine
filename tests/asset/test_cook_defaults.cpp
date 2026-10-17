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
    REQUIRE(assets.mount(Span<const StringView>(roots, 1)));
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
