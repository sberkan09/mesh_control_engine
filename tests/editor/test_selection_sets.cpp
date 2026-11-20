// M-SELSETS 1: numbered selection sets.

#include <doctest/doctest.h>

#include <algorithm>
#include <random>
#include <vector>

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

TEST_CASE("SelectionSets fuzz: slots follow a plain model through random saves, clears and recalls") {
    for (u32 seed = 1; seed <= 200; ++seed) {
        CAPTURE(seed);
        std::mt19937 rng(seed);
        SelectionSets sets;
        std::vector<EntityId> model[SelectionSets::kSlots];
        for (u32 step = 0; step < 40; ++step) {
            const u32 slot = static_cast<u32>(rng() % (SelectionSets::kSlots + 2));  // two past the end
            const u32 op = static_cast<u32>(rng() % 4);
            if (op == 0 || op == 1) {
                std::vector<EntityId> ids;
                for (u32 i = 0, n = static_cast<u32>(rng() % 6); i < n; ++i)
                    ids.push_back(rng() % 6 == 0 ? kNoEntity : static_cast<EntityId>(1 + rng() % 5));
                sets.save(slot, Span<const EntityId>(ids.data(), ids.size()));
                if (slot < SelectionSets::kSlots) {
                    model[slot].clear();
                    for (EntityId id : ids)
                        if (id != kNoEntity &&
                            std::find(model[slot].begin(), model[slot].end(), id) == model[slot].end())
                            model[slot].push_back(id);
                }
            } else if (op == 2) {
                sets.clear(slot);
                if (slot < SelectionSets::kSlots) model[slot].clear();
            }
            const EntityId dead = static_cast<EntityId>(1 + rng() % 5);
            u32 set_slots = 0;
            for (u32 i = 0; i < SelectionSets::kSlots; ++i) {
                set_slots += model[i].empty() ? 0u : 1u;
                CHECK(sets.has(i) == !model[i].empty());
                CHECK(sets.size(i) == model[i].size());
                const Array<EntityId> got = sets.recall(i, [&](EntityId id) { return id != dead; });
                std::vector<EntityId> want;
                for (EntityId id : model[i])
                    if (id != dead) want.push_back(id);
                REQUIRE(got.size() == want.size());
                for (usize j = 0; j < want.size(); ++j) CHECK(got[j] == want[j]);
            }
            CHECK(sets.count() == set_slots);
        }
    }
}
