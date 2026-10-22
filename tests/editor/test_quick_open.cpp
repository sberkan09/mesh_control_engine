// M-QUICKOPEN 1: the fuzzy search behind Ctrl+P. Every query letter must appear in order (case ignored); a word start
// or a run of letters ranks higher, then the shorter label, then the earlier item.

#include <doctest/doctest.h>

#include <string_view>

#include "mce/editor/quick_open.h"

using namespace mce;
using namespace mce::editor;

namespace {

QuickOpen sample() {
    QuickOpen q;
    q.add(QuickOpenKind::Asset, "sprites/hero.png", 1);
    q.add(QuickOpenKind::Asset, "sprites/hero_shadow.png", 2);
    q.add(QuickOpenKind::Panel, "Hierarchy", 3);
    q.add(QuickOpenKind::Command, "File/Open Scene", 4);
    q.add(QuickOpenKind::Entity, "Hero", 5);
    return q;
}

}  // namespace

TEST_CASE("QuickOpen score: every letter in order, case ignored, word starts and runs rank higher") {
    CHECK(QuickOpen::score("xyz", "hero") == -1);
    CHECK(QuickOpen::score("oreh", "hero") == -1);
    CHECK(QuickOpen::score("heroes", "hero") == -1);
    CHECK(QuickOpen::score("HERO", "hero") >= 0);
    CHECK(QuickOpen::score("", "anything") == QuickOpen::score("", "else"));
    // "bf" starts two words in the first label and sits mid-word in the second.
    CHECK(QuickOpen::score("bf", "fx/blue_flame.mcseq") > QuickOpen::score("bf", "abfoo"));
    // A prefix run beats the same letters spread out.
    CHECK(QuickOpen::score("pla", "Play") > QuickOpen::score("pla", "Prefab Library Asset"));
    // Equal matches: the shorter label wins.
    CHECK(QuickOpen::score("ab", "ab") > QuickOpen::score("ab", "ab_long"));
}

TEST_CASE("QuickOpen search: best first, limited, ties keep the earlier item") {
    const QuickOpen q = sample();
    Array<u32> out;

    q.search("hero", 10, out);
    REQUIRE(out.size() == 3);  // "Hierarchy" and "File/Open Scene" lack the letters in order
    CHECK(out[0] == 4);        // the entity "Hero": prefix and shortest
    CHECK(out[1] == 0);        // sprites/hero.png
    CHECK(out[2] == 1);        // sprites/hero_shadow.png
    CHECK(q.items()[out[0]].kind == QuickOpenKind::Entity);
    CHECK(q.items()[out[0]].id == 5);

    q.search("hero", 2, out);
    REQUIRE(out.size() == 2);
    CHECK(out[0] == 4);
    CHECK(out[1] == 0);

    q.search("", 3, out);  // an empty query lists the first items in order
    REQUIRE(out.size() == 3);
    CHECK(out[0] == 0);
    CHECK(out[1] == 1);
    CHECK(out[2] == 2);

    q.search("zzz", 5, out);
    CHECK(out.empty());
    q.search("hero", 0, out);
    CHECK(out.empty());

    QuickOpen twins;
    twins.add(QuickOpenKind::Asset, "Alpha", 10);
    twins.add(QuickOpenKind::Asset, "Alpha", 11);
    twins.search("alp", 5, out);
    REQUIRE(out.size() == 2);
    CHECK(out[0] == 0);
    CHECK(out[1] == 1);
    twins.search("alp", 1, out);
    REQUIRE(out.size() == 1);
    CHECK(out[0] == 0);
}

TEST_CASE("QuickOpen: clear empties the list; kind names") {
    QuickOpen q = sample();
    CHECK(q.items().size() == 5);
    q.clear();
    CHECK(q.items().empty());
    CHECK(std::string_view(quick_open_kind_name(QuickOpenKind::Asset)) == "Asset");
    CHECK(std::string_view(quick_open_kind_name(QuickOpenKind::Entity)) == "Entity");
    CHECK(std::string_view(quick_open_kind_name(QuickOpenKind::Panel)) == "Panel");
    CHECK(std::string_view(quick_open_kind_name(QuickOpenKind::Command)) == "Command");
}
