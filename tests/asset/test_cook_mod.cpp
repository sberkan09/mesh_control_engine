// Mod projects (ADR-0034): a project with a mod.json cooks against the game's pack — references
// into it resolve, overrides keep their type — and its pack carries the ModInfo chunk that makes it
// loadable from a mods folder.

#include <doctest/doctest.h>

#include "cook_test_util.h"
#include "mce/asset/asset_system.h"
#include "mce/asset/mods.h"
#include "mce/cook/cook.h"

using namespace mce;
using namespace mce::asset;
using namespace cook_test;
using cook::CookOptions;
using cook::CookReport;
using cook::CookService;

namespace {

constexpr const char* kTexGuid = "7a000000-0000-4000-8000-000000000001";
constexpr const char* kMatGuid = "7a000000-0000-4000-8000-000000000002";
constexpr const char* kModMatGuid = "7b000000-0000-4000-8000-000000000001";

Guid G(const char* s) { return parse_guid(s).value(); }

std::string diag_text(const CookReport& r) {
    std::string out;
    for (const cook::Diagnostic& d : r.diags.all()) {
        cook::String line;
        cook::format_diagnostic(d, line);
        out += line.c_str();
        out += '\n';
    }
    return out;
}

std::string png(u8 shade) { return make_png(2, 2, std::vector<u8>(16, shade)); }

void texture(const std::string& root, const std::string& rel, const char* guid, u8 shade) {
    write_file(root + "/assets/" + rel, png(shade));
    write_file(root + "/assets/" + rel + ".mceasset", std::string("{ \"guid\": \"") + guid +
                                                          "\", \"importer\": \"texture\", \"importer_version\": 1, "
                                                          "\"settings\": { \"srgb\": true, \"mips\": false } }");
}

void material(const std::string& root, const std::string& rel, const char* guid, const char* tex) {
    write_file(root + "/assets/" + rel, std::string("{ \"format\": \"mce-material\", \"version\": 1, \"guid\": \"") + guid +
                                            "\", \"base_color\": [1, 1, 1, 1], \"textures\": { \"base_color\": \"guid:" + tex +
                                            "\" } }");
}

// The game: a texture and a material over it, cooked into assets.mce.
std::string make_game(const std::string& dir) {
    const std::string root = dir + "/game";
    texture(root, "textures/wall.png", kTexGuid, 0x22);
    material(root, "materials/wall.mcmat", kMatGuid, kTexGuid);
    CookService svc;
    CookOptions opt;
    opt.pack = true;
    const CookReport r = svc.cook(root.c_str(), opt);
    INFO(diag_text(r));
    REQUIRE(r.ok());
    CHECK_FALSE(r.mod);
    return root + "/assets.mce";
}

void mod_json(const std::string& root, const std::string& extra = "\"base\": \"../game/assets.mce\"") {
    write_file(root + "/mod.json", "{ \"format\": \"mce-mod\", \"version\": 1, \"id\": \"retex\", \"name\": \"Retexture\", "
                                   "\"mod_version\": \"1.1\", \"game\": \"probe\", " +
                                       extra + " }");
}

}  // namespace

TEST_CASE("cook mod: references into the base pack resolve; overrides keep the type; the pack is a mod") {
    const std::string dir = fresh_dir("cook_mod");
    const std::string game_pack = make_game(dir);

    const std::string root = dir + "/mod";
    mod_json(root);
    texture(root, "textures/wall_hd.png", kTexGuid, 0x99);                 // overrides the game's texture
    material(root, "materials/glow.mcmat", kModMatGuid, kTexGuid);          // new, over a base GUID
    CookService svc;
    CookOptions opt;
    opt.pack = true;
    const CookReport r = svc.cook(root.c_str(), opt);
    INFO(diag_text(r));
    REQUIRE(r.ok());
    CHECK(r.mod);
    CHECK(r.base_assets == 2);
    CHECK(r.overrides == 1);

    Result<ModEntry> m = read_mod_pack(root + "/assets.mce");
    REQUIRE(m);
    CHECK(m.value().id.view() == "retex");
    CHECK(m.value().name.view() == "Retexture");
    CHECK(m.value().version.view() == "1.1");
    CHECK(m.value().game.view() == "probe");
    CHECK(m.value().asset_count == 2);

    // Mounted like the player does: the mod first. The game's material now draws the mod's texture.
    AssetSystem as;
    const std::string mod_pack = root + "/assets.mce";
    const StringView roots[] = {mod_pack, game_pack};
    REQUIRE(as.mount(Span<const StringView>(roots, 2)));
    CHECK(as.asset_count() == 3);
    auto t = as.load_sync<Texture>(G(kTexGuid));
    REQUIRE(t);
    auto tv = TextureView::read(as.resolve(t.value()));
    REQUIRE(tv);
    CHECK(static_cast<u8>(tv.value().pixels(0)[0]) == 0x99);
    as.release(t.value());
    auto mat = as.load_sync<Material>(G(kModMatGuid));
    REQUIRE(mat);
    as.release(mat.value());

    // Determinism holds for mod packs too.
    const std::string first = read_file(mod_pack);
    CookOptions force = opt;
    force.force = true;
    REQUIRE(svc.cook(root.c_str(), force).ok());
    CHECK(read_file(mod_pack) == first);
}

TEST_CASE("cook mod: a type-changing override, a missing base and a bad mod.json are errors") {
    const std::string dir = fresh_dir("cook_mod_errors");
    make_game(dir);
    CookService svc;

    {  // the game's texture GUID reused for a material
        const std::string root = dir + "/retype";
        mod_json(root);
        texture(root, "textures/t.png", "7c000000-0000-4000-8000-000000000001", 1);
        material(root, "materials/m.mcmat", kTexGuid, "7c000000-0000-4000-8000-000000000001");
        const CookReport r = svc.cook(root.c_str(), CookOptions{});
        CHECK_FALSE(r.ok());
        CHECK(diag_text(r).find("overrides base asset") != std::string::npos);
    }
    {  // a reference into the base with the wrong type
        const std::string root = dir + "/wrongref";
        mod_json(root);
        material(root, "materials/m.mcmat", "7c000000-0000-4000-8000-000000000002", kMatGuid);  // a material as a texture
        const CookReport r = svc.cook(root.c_str(), CookOptions{});
        CHECK_FALSE(r.ok());
        CHECK(diag_text(r).find("in the base pack, expected Texture") != std::string::npos);
    }
    {  // without the base, the same reference is missing
        const std::string root = dir + "/nobase";
        mod_json(root, "\"base\": \"../nowhere/assets.mce\"");
        material(root, "materials/m.mcmat", "7c000000-0000-4000-8000-000000000003", kTexGuid);
        const CookReport r = svc.cook(root.c_str(), CookOptions{});
        CHECK_FALSE(r.ok());
        CHECK(diag_text(r).find("cannot read the base pack") != std::string::npos);
        CHECK(diag_text(r).find("missing reference") != std::string::npos);
    }
    {  // --base on the command line stands in for mod.json's
        const std::string root = dir + "/cli_base";
        write_file(root + "/mod.json", R"({ "format": "mce-mod", "version": 1, "id": "cli" })");
        material(root, "materials/m.mcmat", "7c000000-0000-4000-8000-000000000004", kTexGuid);
        CookOptions opt;
        opt.base_packs.push_back(cook::String((dir + "/game/assets.mce").c_str()));
        const CookReport r = svc.cook(root.c_str(), opt);
        INFO(diag_text(r));
        CHECK(r.ok());
    }
    {  // a bad id and a wrong format name file:line:col
        const std::string root = dir + "/badjson";
        write_file(root + "/mod.json", "{ \"format\": \"mce-mod\", \"version\": 1,\n  \"id\": \"Bad Id\" }");
        texture(root, "textures/t.png", "7c000000-0000-4000-8000-000000000005", 1);
        const CookReport r = svc.cook(root.c_str(), CookOptions{});
        CHECK_FALSE(r.ok());
        CHECK(diag_text(r).find("mod.json:2:") != std::string::npos);
    }
}
