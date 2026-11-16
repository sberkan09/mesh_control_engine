// M-RECENT 1: the most-recently-used scene list behind File > Open Recent.

#include <doctest/doctest.h>

#include <string>

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
