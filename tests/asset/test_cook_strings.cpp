// M-MOBILE step 11a: assets/loc/<locale>.csv cooks to one CHNK_STR per locale, the chunk
// loc::decode_table reads; CSV mistakes are cook errors with a line, and keys a locale lacks
// against the base table are cook warnings.

#include <doctest/doctest.h>

#include "cook_test_util.h"
#include "mce/asset/asset_system.h"
#include "mce/cook/cook.h"
#include "mce/loc/loc.h"

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

constexpr const char* kEnGuid = "10c00000-0000-4000-8000-000000000001";
constexpr const char* kTrGuid = "10c00000-0000-4000-8000-000000000002";

constexpr const char* kEn =
    "key,text,note\n"
    "menu.play,Play,the title button\n"
    "menu.greeting,\"Hello, {name}!\",\n"
    "score.coins#one,{count} coin,\n"
    "score.coins#other,{count} coins,\n"
    "# a comment row\n"
    "\n"
    "quote,\"She said \"\"hi\"\"\nand left\",\n";

constexpr const char* kTr =
    "\xEF\xBB\xBF"  // a BOM, as spreadsheet exports write it
    "key,text\r\n"
    "menu.play,Oyna\r\n"
    "menu.greeting,\"Merhaba, {name}!\"\r\n"
    "score.coins#other,{count} alt\xC4\xB1n\r\n";

loc::StringTable load(asset::AssetSystem& assets, const char* guid) {
    loc::StringTable t;
    const asset::AssetKey key = assets.find(asset::parse_guid(guid).value());
    REQUIRE(key != asset::kInvalidAssetKey);
    CHECK(assets.info(key).type == asset::ChunkType::StringTable);
    auto h = assets.load_sync<asset::StringTable>(key);
    REQUIRE(h);
    REQUIRE(loc::decode_table(assets.resolve(h.value()), t));
    return t;
}

cook::CookReport cook_csv(const std::string& name, const std::string& file, const std::string& csv, const char* settings = "{}") {
    const std::string root = fresh_dir(name);
    write_file(root + "/assets/loc/" + file, csv);
    write_file(root + "/assets/loc/" + file + ".mceasset", sidecar(kEnGuid, "strings", settings));
    cook::CookService svc;
    return svc.cook(StringView(root.c_str()), cook::CookOptions{});
}

}  // namespace

TEST_CASE("strings: assets/loc/<locale>.csv cooks to one CHNK_STR per locale") {
    const std::string root = fresh_dir("strings_cook");
    write_file(root + "/assets/loc/en.csv", kEn);
    write_file(root + "/assets/loc/en.csv.mceasset", sidecar(kEnGuid, "strings"));
    write_file(root + "/assets/loc/tr.csv", kTr);
    write_file(root + "/assets/loc/tr.csv.mceasset", sidecar(kTrGuid, "strings"));
    cook::CookService svc;
    const cook::CookReport r = svc.cook(StringView(root.c_str()), cook::CookOptions{});
    const std::string diags = diags_of(r);
    INFO(diags);
    REQUIRE(r.ok());
    // Turkish lacks "quote" (and score.coins#one, which is fine: Turkish uses #other for 1 too).
    CHECK(diags.find("tr.csv") != std::string::npos);
    CHECK(diags.find("1 key missing here, shown from the base table (en): quote") != std::string::npos);
    CHECK(diags.find("en.csv") == std::string::npos);

    asset::AssetSystem assets;
    const std::string cooked = root + "/cooked";
    const StringView roots[] = {StringView(cooked.c_str())};
    REQUIRE(assets.mount(Span<const StringView>(roots, 1)));
    const loc::StringTable en = load(assets, kEnGuid);
    const loc::StringTable tr = load(assets, kTrGuid);
    CHECK(en.locale() == "en");
    CHECK(tr.locale() == "tr");
    StringView s;
    REQUIRE(en.find("menu.greeting", s));
    CHECK(s == "Hello, {name}!");
    REQUIRE(en.find("score.coins", loc::PluralForm::One, s));
    CHECK(s == "{count} coin");
    REQUIRE(en.find("quote", s));
    CHECK(s == "She said \"hi\"\nand left");
    CHECK_FALSE(en.has_key("#"));
    REQUIRE(tr.find("menu.play", s));
    CHECK(s == "Oyna");  // no BOM, no \r
    REQUIRE(tr.find("score.coins", s));
    CHECK(s == "{count} alt\xC4\xB1n");

    // EN <-> TR through the resource the player installs.
    loc::Localization l10n;
    l10n.add_table(en);
    l10n.add_table(tr);
    const loc::LocaleTag device[] = {"tr-TR"};
    l10n.set_preferred(device);
    CHECK(l10n.locale() == "tr");
    CHECK(l10n.text("menu.play") == "Oyna");
    CHECK(l10n.text("quote") == "She said \"hi\"\nand left");  // falls back to English
    REQUIRE(l10n.set_locale("en"));
    CHECK(l10n.text("menu.play") == "Play");
}

TEST_CASE("strings: the sidecar can name the locale") {
    const cook::CookReport r = cook_csv("strings_sidecar_locale", "brazil.csv", "key,text\nmenu.play,Jogar\n", R"({ "locale": "pt_br" })");
    INFO(diags_of(r));
    CHECK(r.ok());
}

TEST_CASE("strings: authoring mistakes are cook errors with a line") {
    struct Bad {
        const char* file;
        const char* csv;
        const char* says;
    };
    const Bad bad[] = {
        {"en.csv", "id,value\nmenu.play,Play\n", "header"},
        {"en.csv", "key,text\nmenu play,Play\n", ":2:"},
        {"en.csv", "key,text\nmenu.play,Play\nmenu.play,Again\n", "appears twice"},
        {"en.csv", "key,text\ncoins#several,{count}\n", "plural form"},
        {"en.csv", "key,text\ncoins#one,{count} coin\n", "no 'coins#other'"},
        {"en.csv", "key,text\nmenu.play,\"Play\n", "never closed"},
        {"en.csv", "key,text\nmenu.play,\"Play\"x\n", "closing quote"},
        {"en.csv", "key,text\nmenu.play\n", "a key and a text"},
        {"en.csv", "key,text\nmenu.play,\xC3(\n", "UTF-8"},
        {"strings.csv", "key,text\nmenu.play,Play\n", "after its locale"},
    };
    for (const Bad& b : bad) {
        CAPTURE(b.csv);
        const cook::CookReport r = cook_csv("strings_bad", b.file, b.csv);
        const std::string diags = diags_of(r);
        INFO(diags);
        CHECK_FALSE(r.ok());
        CHECK(diags.find(b.file) != std::string::npos);
        CHECK(diags.find(b.says) != std::string::npos);
    }
}

TEST_CASE("strings: a font with \"strings\": true bakes every glyph the tables use") {
    constexpr const char* kFontGuid = "10c00000-0000-4000-8000-0000000000f0";
    const std::string root = fresh_dir("strings_font");
    const std::string a = root + "/assets";
    write_file(a + "/loc/en.csv", "key,text\nmenu.play,play\ntitle,\xE6\x97\xA5\n");  // 日: not in the font
    write_file(a + "/loc/en.csv.mceasset", sidecar(kEnGuid, "strings"));
    write_file(a + "/loc/tr.csv", "key,text\nmenu.play,oyna\nback,geri\n");
    write_file(a + "/loc/tr.csv.mceasset", sidecar(kTrGuid, "strings"));
    write_file(a + "/fonts/proggy.ttf", read_file(std::string(MCE_TEST_DATA_DIR) + "/fonts/ProggyClean.ttf"));
    write_file(a + "/fonts/proggy.ttf.mceasset", sidecar(kFontGuid, "font", R"({ "size": 13, "ranges": [[48, 57]], "strings": true })"));
    cook::CookService svc;
    const cook::CookReport r = svc.cook(StringView(root.c_str()), cook::CookOptions{});
    const std::string diags = diags_of(r);
    INFO(diags);
    REQUIRE(r.ok());
    CHECK(diags.find("proggy.ttf") != std::string::npos);
    CHECK(diags.find("U+65E5") != std::string::npos);  // the missing glyph is a warning

    asset::AssetSystem assets;
    const std::string cooked = root + "/cooked";
    const StringView roots[] = {StringView(cooked.c_str())};
    REQUIRE(assets.mount(Span<const StringView>(roots, 1)));
    const asset::AssetKey key = assets.find(asset::parse_guid(kFontGuid).value());
    REQUIRE(key != asset::kInvalidAssetKey);
    auto h = assets.load_sync<asset::Font>(key);
    REQUIRE(h);
    auto view = asset::FontView::read(assets.resolve(h.value()));
    REQUIRE(view);
    const asset::FontView& f = view.value();
    for (const char c : std::string("0123456789playoynageri" "PLAYOYNAGER")) {  // the ranges, the texts and their upper case
        CAPTURE(c);
        CHECK(f.find(static_cast<u32>(c)) != nullptr);
    }
    CHECK(f.find('b') == nullptr);  // nothing else
    CHECK(f.find(0x65E5) == nullptr);
    // Turkish upper case of "i" is İ: asked for, and only baked when the font has it.
    const bool has_dotted_capital_i = f.find(0x130) != nullptr;
    CHECK((has_dotted_capital_i || diags.find("U+0130") != std::string::npos));

    // Editing a table re-cooks the font (the tables are its dependencies).
    write_file(a + "/loc/tr.csv", "key,text\nmenu.play,oyna\nback,geri\nnext,ileri\n");
    const cook::CookReport r2 = svc.cook(StringView(root.c_str()), cook::CookOptions{});
    REQUIRE(r2.ok());
    bool font_recooked = false;
    for (const auto& ar : r2.assets)
        if (ar.guid == asset::parse_guid(kFontGuid).value()) font_recooked = ar.status == cook::AssetStatus::Cooked;
    CHECK(font_recooked);
}
