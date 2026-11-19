// M-SELHISTORY 1: Back / Forward through the selected entities.

#include <doctest/doctest.h>

#include <random>
#include <set>
#include <vector>

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

// M-SELHISTORY2: random visit, back, forward and clear against a plain vector and position.
TEST_CASE("SelectionHistory fuzz: visit, back, forward and clear match a plain model") {
    for (u32 seed = 1; seed <= 200; ++seed) {
        CAPTURE(seed);
        std::mt19937 rng(seed);
        SelectionHistory h;
        std::vector<EntityId> items;
        int pos = -1;
        std::set<EntityId> gone;                               // ids the caller says are deleted
        const u32 steps = 40 + static_cast<u32>(rng() % 200);  // long enough to pass the cap of 64
        for (u32 step = 0; step < steps; ++step) {
            const auto alive = [&](EntityId id) { return gone.count(id) == 0; };
            switch (rng() % 7) {
                case 0:
                    h.back(alive);
                    for (int i = pos - 1; i >= 0; --i)
                        if (alive(items[static_cast<usize>(i)])) {
                            pos = i;
                            break;
                        }
                    break;
                case 1:
                    h.forward(alive);
                    for (int i = pos + 1; i < static_cast<int>(items.size()); ++i)
                        if (alive(items[static_cast<usize>(i)])) {
                            pos = i;
                            break;
                        }
                    break;
                case 2:
                    gone.insert(static_cast<EntityId>(1 + rng() % 12));
                    break;
                case 3:
                    if (rng() % 10 == 0) {
                        h.clear();
                        items.clear();
                        pos = -1;
                    }
                    break;
                default: {
                    const EntityId id = rng() % 9 == 0 ? kNoEntity : static_cast<EntityId>(1 + rng() % 12);
                    h.visit(id);
                    const EntityId cur = pos < 0 ? kNoEntity : items[static_cast<usize>(pos)];
                    if (id == kNoEntity || id == cur) break;
                    items.resize(static_cast<usize>(pos) + 1);
                    items.push_back(id);
                    if (items.size() > SelectionHistory::kCap) items.erase(items.begin());
                    pos = static_cast<int>(items.size()) - 1;
                    break;
                }
            }
            REQUIRE(h.size() == items.size());
            CHECK(h.current() == (pos < 0 ? kNoEntity : items[static_cast<usize>(pos)]));
            CHECK(h.can_back() == (pos > 0));
            CHECK(h.can_forward() == (pos >= 0 && pos + 1 < static_cast<int>(items.size())));
            CHECK(h.size() <= SelectionHistory::kCap);
        }
    }
}
