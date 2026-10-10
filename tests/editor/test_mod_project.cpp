// A mod project in the editor (ADR-0034): the cooked tree mounts with the game's pack behind it,
// so the mod's scenes resolve the game's assets, and a hot reload that changes nothing still swaps
// in place instead of remounting over the game's assets.

#include <doctest/doctest.h>

#include <filesystem>
#include <fstream>
#include <string>

#include "cook_test_util.h"
#include "mce/cook/cook.h"
#include "mce/editor/project.h"

using namespace mce;
using namespace mce::editor;

namespace {

constexpr const char* kCrate = "5ce4e000-0000-4000-8000-000000000001";  // samples/scene_demo's crate texture

}  // namespace

TEST_CASE("mod project: the editor mounts the game's pack behind the mod") {
    const std::string dir = cook_test::fresh_dir("editor_mod_project");
    std::filesystem::create_directories(dir + "/game");
    std::filesystem::copy(std::string(MCE_SOURCE_DIR) + "/samples/scene_demo/assets", dir + "/game/assets",
                          std::filesystem::copy_options::recursive);
    cook::CookService svc;
    cook::CookOptions opt;
    opt.pack = true;
    REQUIRE(svc.cook((dir + "/game").c_str(), opt).ok());

    const std::string mod = dir + "/mod";
    cook_test::write_file(mod + "/mod.json",
                          R"({ "format": "mce-mod", "version": 1, "id": "crates", "base": "../game/assets.mce" })");
    cook_test::write_file(mod + "/assets/scenes/crates.mcscene",
                          std::string(R"({ "format": "mce-scene", "version": 1, "guid": "5ce4e000-0000-4000-8000-0000000000a1",
  "name": "crates", "entities": [ { "name": "Crate", "components": { "LocalTransform": {}, "Sprite": { "texture": "guid:)") +
                              kCrate + R"(" } } } ] })");

    EditorProject p;
    REQUIRE(p.open(StringView(mod.data(), mod.size())));
    REQUIRE(p.cook().ok());
    REQUIRE(p.mounted());
    REQUIRE(p.base_roots().size() == 1);
    CHECK(p.base_roots()[0].ends_with("game/assets.mce"));
    const asset::Guid crate = asset::parse_guid(kCrate).value();
    CHECK(p.mounted()->find(crate) != asset::kInvalidAssetKey);
    auto tex = p.mounted()->load_sync<asset::Texture>(crate);
    REQUIRE(tex);
    p.mounted()->release(tex.value());

    const HotReloadResult r = p.reload();
    CHECK_FALSE(r.remounted);
    CHECK(r.failed == 0);
}
