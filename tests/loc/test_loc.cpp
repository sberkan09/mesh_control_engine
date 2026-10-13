// M-MOBILE step 11a: string tables and the Localization resource (mce/loc). The chunk round-trips
// and rejects damage; the language follows the player's choice, then the device, then the base
// language, and survives a relaunch; plural rules, placeholders and Turkish case mapping.

#include <doctest/doctest.h>

#include <cstring>
#include <filesystem>
#include <string>

#include "mce/ecs/save.h"
#include "mce/loc/loc.h"

using namespace mce;
using namespace mce::loc;

namespace {

std::string str(const Array<char>& a) { return std::string(a.data(), a.size()); }

StringTable table(StringView locale, std::initializer_list<std::pair<const char*, const char*>> rows) {
    StringTable t;
    t.set_locale(locale);
    for (const auto& [k, v] : rows) {
        StringView key(k);
        PluralForm form = PluralForm::Other;
        if (const usize hash = key.find('#'); hash != StringView::npos) {
            REQUIRE(parse_plural_form(key.substr(hash + 1), form));
            key = key.substr(0, hash);
        }
        REQUIRE(t.add(key, v, form));
    }
    return t;
}

Localization en_tr() {
    Localization l;
    l.add_table(table("en", {{"menu.play", "Play"},
                             {"menu.quit", "Quit"},
                             {"menu.greeting", "Hello, {name}!"},
                             {"coins#one", "{count} coin"},
                             {"coins#other", "{count} coins"},
                             {"title", "island"}}));
    l.add_table(table("tr", {{"menu.play", "Oyna"},
                             {"menu.greeting", "Merhaba, {name}!"},
                             {"coins#other", "{count} alt\xC4\xB1n"},
                             {"title", "ada"}}));
    return l;
}

std::string upper(StringView locale, StringView s) {
    Array<char> out;
    to_upper(locale, s, out);
    return str(out);
}

std::string lower(StringView locale, StringView s) {
    Array<char> out;
    to_lower(locale, s, out);
    return str(out);
}

}  // namespace

TEST_CASE("loc: a string table round-trips through its chunk") {
    const StringTable t = table("tr-TR", {{"b.key", "ikinci"}, {"a.key", "\xC3\xA7ok g\xC3\xBCzel"}, {"n#one", "bir"}, {"n#other", "{count}"}});
    Array<byte> bytes;
    encode_table(t, bytes);
    StringTable back;
    REQUIRE(decode_table(bytes.span(), back));
    CHECK(back.locale() == "tr-TR");
    REQUIRE(back.size() == 4);
    CHECK(back.entry(0).key == "a.key");  // sorted
    StringView s;
    REQUIRE(back.find("a.key", s));
    CHECK(s == "\xC3\xA7ok g\xC3\xBCzel");
    REQUIRE(back.find("n", PluralForm::One, s));
    CHECK(s == "bir");
    CHECK(back.has_key("n"));
    CHECK_FALSE(back.has_key("c"));
    CHECK_FALSE(back.find("b.key", PluralForm::One, s));

    SUBCASE("damage is rejected") {
        Array<byte> cut = bytes;
        cut.resize(cut.size() - 1);
        CHECK_FALSE(decode_table(cut.span(), back));
        Array<byte> reserved = bytes;
        reserved[28] = byte{1};  // header reserved word
        CHECK_FALSE(decode_table(reserved.span(), back));
        Array<byte> no_locale = bytes;
        no_locale[0] = byte{0};
        CHECK_FALSE(decode_table(no_locale.span(), back));
        Array<byte> swapped = bytes;  // entries 0 and 1 swapped: no longer sorted
        std::memcpy(swapped.data() + 32, bytes.data() + 52, 20);
        std::memcpy(swapped.data() + 52, bytes.data() + 32, 20);
        CHECK(decode_table(swapped.span(), back).error().code == Errc::InvalidArgument);
    }
    SUBCASE("bad keys and text never enter a table") {
        StringTable u;
        CHECK_FALSE(u.add("", "x"));
        CHECK_FALSE(u.add("a b", "x"));
        CHECK_FALSE(u.add("ok", "\xC3("));        // truncated sequence
        CHECK_FALSE(u.add("ok", "\xC0\xAF"));     // overlong
        CHECK_FALSE(u.add("ok", "\xED\xA0\x80")); // surrogate
        CHECK(u.add("ok", "x"));
        CHECK_FALSE(u.add("ok", "y"));
    }
}

TEST_CASE("loc: locale tags normalize") {
    LocaleTag t;
    REQUIRE(normalize_locale("tr_TR.UTF-8", t));
    CHECK(t.view() == "tr-TR");
    REQUIRE(normalize_locale("PT-br", t));
    CHECK(t.view() == "pt-BR");
    REQUIRE(normalize_locale("zh-hant-tw", t));
    CHECK(t.view() == "zh-Hant-TW");
    REQUIRE(normalize_locale("sr_RS@latin", t));
    CHECK(t.view() == "sr-RS");
    CHECK_FALSE(normalize_locale("C", t));
    CHECK_FALSE(normalize_locale("POSIX", t));
    CHECK_FALSE(normalize_locale("", t));
    CHECK(language_of("pt-BR") == "pt");
    CHECK(language_of("en") == "en");
}

TEST_CASE("loc: the fallback chain") {
    const LocaleTag available[] = {"en", "pt-PT", "tr"};
    Array<LocaleTag> chain;
    auto joined = [&] {
        std::string s;
        for (const LocaleTag& t : chain) s += std::string(t.view()) + " ";
        return s;
    };
    {
        const LocaleTag pref[] = {"tr-TR", "de"};
        fallback_chain(pref, available, "en", chain);
        CHECK(joined() == "tr en ");
    }
    {
        const LocaleTag pref[] = {"pt-BR"};  // another region of the language
        fallback_chain(pref, available, "en", chain);
        CHECK(joined() == "pt-PT en ");
    }
    {
        fallback_chain({}, available, "en", chain);
        CHECK(joined() == "en ");
    }
    {
        fallback_chain({}, available, "fr", chain);  // no base table: the first there is
        CHECK(joined() == "en ");
    }
}

TEST_CASE("loc: EN <-> TR with the device, the choice and the base language") {
    Localization l = en_tr();
    CHECK(l.locale() == "en");  // no device languages yet: the base
    CHECK(l.text("menu.play") == "Play");

    const u32 g0 = l.generation();
    const LocaleTag device[] = {"tr-TR", "en-US"};
    l.set_preferred(device);
    CHECK(l.generation() != g0);
    CHECK(l.locale() == "tr");
    CHECK(l.text("menu.play") == "Oyna");
    CHECK(l.text("menu.quit") == "Quit");      // not translated: the English text
    CHECK(l.text("menu.missing") == "menu.missing");  // nowhere: the key itself

    Array<char> buf;
    const Arg name[] = {{"name", "Ada"}};
    l.format("menu.greeting", name, buf);
    CHECK(str(buf) == "Merhaba, Ada!");

    REQUIRE(l.set_locale("en-GB"));  // the language menu; served by "en"
    CHECK(l.locale() == "en");
    CHECK(l.user_locale() == "en-GB");
    buf.clear();
    l.format("menu.greeting", name, buf);
    CHECK(str(buf) == "Hello, Ada!");
    CHECK_FALSE(l.set_locale("de"));  // no German table: nothing changes
    CHECK(l.locale() == "en");
    REQUIRE(l.set_locale(""));        // back to the device
    CHECK(l.locale() == "tr");
}

TEST_CASE("loc: plural forms by locale") {
    CHECK(plural_form("en", 1) == PluralForm::One);
    CHECK(plural_form("en", 0) == PluralForm::Other);
    CHECK(plural_form("en", 2) == PluralForm::Other);
    CHECK(plural_form("tr", 1) == PluralForm::One);
    CHECK(plural_form("tr", 5) == PluralForm::Other);
    CHECK(plural_form("fr", 0) == PluralForm::One);
    CHECK(plural_form("pt-BR", 0) == PluralForm::One);
    CHECK(plural_form("pt-PT", 0) == PluralForm::Other);
    CHECK(plural_form("ja", 1) == PluralForm::Other);
    CHECK(plural_form("ru", 21) == PluralForm::One);
    CHECK(plural_form("ru", 11) == PluralForm::Many);
    CHECK(plural_form("ru", 3) == PluralForm::Few);
    CHECK(plural_form("ru", 13) == PluralForm::Many);
    CHECK(plural_form("pl", 22) == PluralForm::Few);
    CHECK(plural_form("pl", 21) == PluralForm::Many);
    CHECK(plural_form("cs", 3) == PluralForm::Few);
    CHECK(plural_form("ar", 0) == PluralForm::Zero);
    CHECK(plural_form("ar", 2) == PluralForm::Two);
    CHECK(plural_form("ar", 105) == PluralForm::Few);
    CHECK(plural_form("ar", 111) == PluralForm::Many);
    CHECK(plural_form("en", -1) == PluralForm::One);

    Localization l = en_tr();
    Array<char> buf;
    l.plural("coins", 1, {}, buf);
    CHECK(str(buf) == "1 coin");
    buf.clear();
    l.plural("coins", 5, {}, buf);
    CHECK(str(buf) == "5 coins");
    REQUIRE(l.set_locale("tr"));
    buf.clear();
    l.plural("coins", 1, {}, buf);  // Turkish has no #one in this table: #other
    CHECK(str(buf) == "1 alt\xC4\xB1n");
    buf.clear();
    l.plural("coins", -12, {}, buf);
    CHECK(str(buf) == "-12 alt\xC4\xB1n");
}

TEST_CASE("loc: placeholders") {
    Array<char> buf;
    const Arg args[] = {{"name", "Ada"}, {"n", "3"}};
    format("{name} has {n} {{keys}} and {missing}", args, buf);
    CHECK(str(buf) == "Ada has 3 {keys} and {missing}");
    buf.clear();
    format("open { brace", args, buf);
    CHECK(str(buf) == "open { brace");
    buf.clear();
    format("}}", args, buf);
    CHECK(str(buf) == "}");
}

TEST_CASE("loc: Turkish-aware case mapping") {
    CHECK(upper("tr", "istanbul") == "\xC4\xB0STANBUL");          // İSTANBUL
    CHECK(upper("en", "istanbul") == "ISTANBUL");
    CHECK(upper("az-AZ", "i") == "\xC4\xB0");
    CHECK(lower("tr", "ISPARTA") == "\xC4\xB1sparta");            // ısparta
    CHECK(lower("en", "ISPARTA") == "isparta");
    CHECK(lower("tr", "\xC4\xB0ZM\xC4\xB0R") == "izmir");         // İZMİR
    CHECK(lower("en", "\xC4\xB0") == "i\xCC\x87");                // İ -> i + combining dot
    CHECK(upper("tr", "\xC4\xB1\xC4\x9F\xC5\x9F\xC3\xB6\xC3\xBC\xC3\xA7") ==
          "I\xC4\x9E\xC5\x9E\xC3\x96\xC3\x9C\xC3\x87");             // ığşöüç -> IĞŞÖÜÇ
    CHECK(lower("tr", "I\xC4\x9E\xC5\x9E\xC3\x96\xC3\x9C\xC3\x87") ==
          "\xC4\xB1\xC4\x9F\xC5\x9F\xC3\xB6\xC3\xBC\xC3\xA7");
    CHECK(upper("de", "stra\xC3\x9F" "e") == "STRASSE");
    CHECK(upper("ru", "\xD0\xBC\xD0\xB8\xD1\x80") == "\xD0\x9C\xD0\x98\xD0\xA0");  // мир -> МИР
    CHECK(lower("el", "\xCE\xA9") == "\xCF\x89");                 // Ω -> ω
    CHECK(upper("pl", "\xC5\x82\xC3\xB3\xC5\xBA") == "\xC5\x81\xC3\x93\xC5\xB9");  // łóź -> ŁÓŹ
    CHECK(upper("en", "a\xFF" "b") == "A\xFF" "B");               // a malformed byte passes through

    Localization l = en_tr();
    REQUIRE(l.set_locale("tr"));
    Array<char> buf;
    l.upper(l.text("title"), buf);
    CHECK(str(buf) == "ADA");
}

TEST_CASE("loc: the player's choice survives a relaunch") {
    const std::filesystem::path dir = std::filesystem::path(MCE_TEST_TMP_DIR) / "loc_choice";
    std::filesystem::remove_all(dir);
    std::filesystem::create_directories(dir);
    save::set_root(dir.string().c_str());

    const LocaleTag device[] = {"en-US"};
    {
        Localization l = en_tr();
        l.set_preferred(device);
        REQUIRE(l.load_choice());  // no save yet: not an error
        CHECK(l.locale() == "en");
        l.persist_choice();
        REQUIRE(l.set_locale("tr"));
    }
    {
        Localization l = en_tr();
        l.set_preferred(device);
        REQUIRE(l.load_choice());
        CHECK(l.user_locale() == "tr");
        CHECK(l.locale() == "tr");
        CHECK(l.text("menu.play") == "Oyna");
        l.persist_choice();
        REQUIRE(l.set_locale(""));  // follow the device again
    }
    {
        Localization l = en_tr();
        l.set_preferred(device);
        REQUIRE(l.load_choice());
        CHECK(l.user_locale().empty());
        CHECK(l.locale() == "en");
    }
}
