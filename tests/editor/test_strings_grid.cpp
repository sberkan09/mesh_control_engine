// M-STRINGSED 1: the strings grid over a project's locale tables. A load / edit / save round trip keeps comment rows
// and extra columns, an edit changes only its row, a missing key shows as missing and set_text adds the row, and a
// table the cook refuses is not written.

#include <doctest/doctest.h>

#include <filesystem>
#include <fstream>
#include <string>

#include "cook_test_util.h"
#include "mce/cook/fs.h"
#include "mce/editor/strings_grid.h"

using namespace mce;
using namespace mce::editor;

namespace {

constexpr const char* kEn = "key,text,note\n# menu strings\nmenu.play,Play,button\nmenu.greeting,\"Hello, {name}!\",\nscore.coins#one,{count} coin,\nscore.coins#other,{count} coins,\n";
constexpr const char* kTr = "key,text\nmenu.play,Oyna\nscore.coins#other,{count} jeton\n";

struct Fixture {
    std::string root;
    EditorProject project;
    StringsGrid grid;

    explicit Fixture(const char* name) {
        root = cook_test::fresh_dir(name);
        std::filesystem::create_directories(root + "/assets/loc");
        std::ofstream(root + "/assets/loc/en.csv") << kEn;
        std::ofstream(root + "/assets/loc/tr.csv") << kTr;
        std::ofstream(root + "/assets/loc/en.csv.mceasset") << R"({ "guid": "5ce4e000-0000-4000-8000-0000000000e1", "importer": "strings" })";
        std::ofstream(root + "/assets/loc/tr.csv.mceasset") << R"({ "guid": "5ce4e000-0000-4000-8000-0000000000e2", "importer": "strings" })";
        REQUIRE(project.open(root));
        cook::String err;
        REQUIRE(grid.load(project, err));
    }
    std::string read(const char* rel) {
        cook::String t;
        REQUIRE(cook::fs::read_text(project.asset_path(rel).view(), t));
        return std::string(t.c_str());
    }
    u32 key(const char* k) {
        for (u32 i = 0; i < grid.keys().size(); ++i)
            if (grid.keys()[i].view() == k) return i;
        FAIL("no such key");
        return 0;
    }
};

}  // namespace

TEST_CASE("StringsGrid: locales, the union of keys and the missing cells") {
    Fixture f("strings_grid_load");
    REQUIRE(f.grid.locales().size() == 2);
    CHECK(f.grid.locales()[0].tag.view() == "en");  // the base first
    CHECK(f.grid.locales()[1].tag.view() == "tr");
    REQUIRE(f.grid.keys().size() == 4);
    CHECK(f.grid.keys()[0].view() == "menu.play");
    CHECK(f.grid.text(0, f.key("menu.greeting")) == "Hello, {name}!");
    CHECK(f.grid.text(1, f.key("menu.play")) == "Oyna");
    CHECK_FALSE(f.grid.has(1, f.key("menu.greeting")));
    CHECK(f.grid.missing(0) == 0);
    CHECK(f.grid.missing(1) == 2);  // the greeting and #one
    CHECK_FALSE(f.grid.dirty());
}

TEST_CASE("StringsGrid: an edit changes only its row; comments and extra columns survive; a new cell adds its row") {
    Fixture f("strings_grid_edit");
    f.grid.set_text(0, f.key("menu.play"), "Start");
    f.grid.set_text(1, f.key("menu.greeting"), "Merhaba, {name}!");
    CHECK(f.grid.dirty());
    CHECK(f.grid.missing(1) == 1);
    cook::String err;
    const bool saved = f.grid.save(f.project, err);
    if (!saved) MESSAGE(std::string(err.view()).substr(0, 600));
    REQUIRE(saved);
    CHECK_FALSE(f.grid.dirty());

    const std::string en = f.read("loc/en.csv");
    CHECK(en.find("# menu strings") != std::string::npos);
    CHECK(en.find("menu.play,Start,button") != std::string::npos);  // the note column kept
    CHECK(en.find("\"Hello, {name}!\"") != std::string::npos);      // quoting kept where needed
    const std::string tr = f.read("loc/tr.csv");
    CHECK(tr.find("menu.greeting,\"Merhaba, {name}!\"") != std::string::npos);

    // A fresh load sees the saved values.
    StringsGrid again;
    REQUIRE(again.load(f.project, err));
    CHECK(again.text(0, 0) == "Start");
    CHECK(again.missing(1) == 1);
}

TEST_CASE("StringsGrid: add and remove a key; a table the cook refuses is not written") {
    Fixture f("strings_grid_keys");
    CHECK(f.grid.add_key("menu.quit"));
    CHECK_FALSE(f.grid.add_key("menu.quit"));
    CHECK_FALSE(f.grid.add_key("  "));
    const u32 quit = f.key("menu.quit");
    f.grid.set_text(0, quit, "Quit");
    f.grid.remove_key(f.key("menu.greeting"));
    cook::String err;
    const bool saved = f.grid.save(f.project, err);
    if (!saved) MESSAGE(std::string(err.view()).substr(0, 600));
    REQUIRE(saved);
    const std::string en = f.read("loc/en.csv");
    CHECK(en.find("menu.quit,Quit") != std::string::npos);
    CHECK(en.find("menu.greeting") == std::string::npos);

    const std::string before = f.read("loc/en.csv");
    CHECK(f.grid.add_key("bad key!"));  // a key with a space and '!' is not allowed
    f.grid.set_text(0, f.key("bad key!"), "x");
    err = cook::String();
    CHECK_FALSE(f.grid.save(f.project, err));
    CHECK(err.view().find("nothing was written") != StringView::npos);
    CHECK(f.read("loc/en.csv") == before);
}
