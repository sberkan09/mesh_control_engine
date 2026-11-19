// M-RECENT 1: the most-recently-used scene list behind File > Open Recent.

#include <doctest/doctest.h>

#include <algorithm>
#include <random>
#include <string>
#include <vector>

#include "mce/editor/recent_list.h"

using namespace mce;
using namespace mce::editor;

namespace {

// The list as "a,b,c", newest first.
std::string joined(const RecentList& r) {
    std::string s;
    for (const cook::String& p : r.items()) {
        if (!s.empty()) s += ",";
        s += std::string(p.view().data(), p.view().size());
    }
    return s;
}

}  // namespace

TEST_CASE("RecentList touch: newest first, no duplicates, capped") {
    RecentList r(3);
    r.touch("a.mcscene");
    r.touch("b.mcscene");
    r.touch("c.mcscene");
    CHECK(joined(r) == "c.mcscene,b.mcscene,a.mcscene");
    r.touch("a.mcscene");  // already listed: moves to the front
    CHECK(joined(r) == "a.mcscene,c.mcscene,b.mcscene");
    r.touch("d.mcscene");  // over the cap: the oldest goes
    CHECK(joined(r) == "d.mcscene,a.mcscene,c.mcscene");
    r.touch("");
    CHECK(r.items().size() == 3);
}

TEST_CASE("RecentList remove drops one path") {
    RecentList r;
    r.touch("a");
    r.touch("b");
    CHECK(r.remove("a"));
    CHECK_FALSE(r.remove("a"));
    CHECK(joined(r) == "b");
}

TEST_CASE("RecentList serialize and parse round-trip; parse skips blanks, CR and repeats, honours the cap") {
    RecentList r;
    r.touch("one");
    r.touch("two");
    r.touch("three");
    const cook::String text = r.serialize();
    CHECK(std::string(text.view().data(), text.view().size()) == "three\ntwo\none\n");
    RecentList back;
    back.parse(text.view());
    CHECK(joined(back) == "three,two,one");

    RecentList messy(2);
    messy.parse("x\r\n\r\nx\ny\nz\n");
    CHECK(joined(messy) == "x,y");

    RecentList none;
    none.parse("");
    CHECK(none.items().empty());
}

// M-RECENT2: random touch, remove, clear and save-and-load sequences against a plain vector.
TEST_CASE("RecentList fuzz: touch, remove, clear and a save-load round trip match a plain model") {
    static const char* const kPaths[] = {"a.mcscene", "b.mcscene", "scenes/c.mcscene", "d e.mcscene", "x", "", "y"};
    for (u32 seed = 1; seed <= 200; ++seed) {
        CAPTURE(seed);
        std::mt19937 rng(seed);
        const u32 cap = static_cast<u32>(rng() % 6);  // 0 is a legal cap: nothing is kept
        RecentList r(cap);
        std::vector<std::string> model;
        for (u32 step = 0; step < 40; ++step) {
            const std::string path = kPaths[rng() % 7];
            switch (rng() % 6) {
                case 0:
                    r.clear();
                    model.clear();
                    break;
                case 1: {
                    const auto it = std::find(model.begin(), model.end(), path);
                    const bool had = it != model.end();
                    if (had) model.erase(it);
                    CHECK(r.remove(StringView(path.data(), path.size())) == had);
                    break;
                }
                case 2: {  // save, then load into a list of the same cap
                    const cook::String text = r.serialize();
                    RecentList back(cap);
                    back.parse(text.view());
                    CHECK(joined(back) == joined(r));
                    break;
                }
                default:
                    r.touch(StringView(path.data(), path.size()));
                    if (!path.empty()) {
                        model.erase(std::remove(model.begin(), model.end(), path), model.end());
                        model.insert(model.begin(), path);
                        if (model.size() > cap) model.resize(cap);
                    }
                    break;
            }
            std::string want;
            for (const std::string& m : model) {
                if (!want.empty()) want += ',';
                want += m;
            }
            CHECK(joined(r) == want);
            CHECK(r.items().size() <= cap);
        }
    }
}
