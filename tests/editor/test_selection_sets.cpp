// M-SELSETS 1: numbered selection sets.

#include <doctest/doctest.h>

#include "mce/editor/selection_sets.h"

using namespace mce;
using namespace mce::editor;

TEST_CASE("SelectionSets save and recall") {
    SelectionSets s;
    CHECK(s.count() == 0);
    CHECK_FALSE(s.has(0));
    const EntityId a[] = {5, 7, kNoEntity, 5, 9};
    s.save(0, Span<const EntityId>(a, 5));
    CHECK(s.has(0));
    CHECK(s.count() == 1);
    CHECK(s.size(0) == 3);  // 5, 7, 9: the null id and the repeat are dropped
    const Array<EntityId> all = s.recall(0, [](EntityId) { return true; });
    REQUIRE(all.size() == 3);
    CHECK(all[0] == 5);
    CHECK(all[1] == 7);
    CHECK(all[2] == 9);
    const Array<EntityId> some = s.recall(0, [](EntityId e) { return e != 7; });  // 7 was deleted
    REQUIRE(some.size() == 2);
    CHECK(some[1] == 9);
    CHECK(s.recall(3, [](EntityId) { return true; }).empty());  // unset slot
}

TEST_CASE("SelectionSets clear and bounds") {
    SelectionSets s;
    const EntityId a[] = {1};
    s.save(8, Span<const EntityId>(a, 1));
    s.save(9, Span<const EntityId>(a, 1));  // past the last slot: ignored
    CHECK(s.count() == 1);
    CHECK_FALSE(s.has(9));
    s.save(8, Span<const EntityId>());  // an empty set clears the slot
    CHECK(s.count() == 0);
    s.save(2, Span<const EntityId>(a, 1));
    s.save(4, Span<const EntityId>(a, 1));
    s.clear(2);
    CHECK(s.count() == 1);
    s.clear_all();
    CHECK(s.count() == 0);
}
