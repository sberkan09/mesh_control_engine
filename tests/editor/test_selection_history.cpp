// M-SELHISTORY 1: Back / Forward through the selected entities.

#include <doctest/doctest.h>

#include "mce/editor/selection_history.h"

using namespace mce;
using namespace mce::editor;

namespace {

bool any(EntityId) { return true; }

}  // namespace

TEST_CASE("SelectionHistory visit, back and forward") {
    SelectionHistory h;
    CHECK_FALSE(h.can_back());
    CHECK_FALSE(h.can_forward());
    h.visit(kNoEntity);  // nothing selected is not an entry
    CHECK(h.size() == 0);
    h.visit(10);
    h.visit(10);  // the same again is not a new entry
    h.visit(20);
    h.visit(30);
    CHECK(h.size() == 3);
    CHECK(h.current() == 30);
    CHECK(h.can_back());
    CHECK_FALSE(h.can_forward());

    CHECK(h.back(any) == 20);
    CHECK(h.back(any) == 10);
    CHECK(h.back(any) == kNoEntity);  // at the start: unchanged
    CHECK(h.current() == 10);
    CHECK(h.forward(any) == 20);
    CHECK(h.can_forward());

    // The shell sees the selection change to the entry it stepped to and visits it: no new entry.
    h.visit(20);
    CHECK(h.size() == 3);
    CHECK(h.forward(any) == 30);
    CHECK(h.forward(any) == kNoEntity);
}

TEST_CASE("SelectionHistory selecting while stepped back drops the entries ahead") {
    SelectionHistory h;
    h.visit(1);
    h.visit(2);
    h.visit(3);
    CHECK(h.back(any) == 2);
    h.visit(9);
    CHECK(h.size() == 3);  // 1, 2, 9
    CHECK_FALSE(h.can_forward());
    CHECK(h.back(any) == 2);
    CHECK(h.back(any) == 1);
}

TEST_CASE("SelectionHistory skips entries that are gone and keeps a cap") {
    SelectionHistory h;
    h.visit(1);
    h.visit(2);
    h.visit(3);
    h.visit(4);
    const auto alive = [](EntityId id) { return id != 2 && id != 3; };
    CHECK(h.back(alive) == 1);  // 3 and 2 are gone
    CHECK(h.forward(alive) == 4);
    CHECK(h.back([](EntityId) { return false; }) == kNoEntity);
    CHECK(h.current() == 4);

    SelectionHistory big;
    for (EntityId i = 1; i <= SelectionHistory::kCap + 10; ++i) big.visit(i);
    CHECK(big.size() == SelectionHistory::kCap);
    CHECK(big.current() == SelectionHistory::kCap + 10);
    big.clear();
    CHECK(big.size() == 0);
    CHECK(big.current() == kNoEntity);
}
