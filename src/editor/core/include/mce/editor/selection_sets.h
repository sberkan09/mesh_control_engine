#pragma once
// SelectionSets — nine numbered slots that each remember a set of selected entities (M-SELSETS), like Maya's
// quick selection sets. The shell stores the current selection with Ctrl+Shift+1..9 and selects the stored
// entities again with Alt+1..9. recall() drops the entities the caller says are gone (a deleted entity). The sets
// belong to one open scene (entity ids mean nothing in another). Headless, tested in CI.

#include "mce/containers/array.h"
#include "mce/editor/scene_document.h"

namespace mce::editor {

class SelectionSets {
public:
    static constexpr u32 kSlots = 9;

    // Stores `ids` (kNoEntity and repeats skipped, order kept) in `slot`; an empty set clears the slot.
    // Slots past kSlots are ignored.
    void save(u32 slot, Span<const EntityId> ids);

    // The entities of `slot` for which alive(id) is true, in stored order; empty for an unset slot.
    template <class Alive>
    Array<EntityId> recall(u32 slot, Alive&& alive) const {
        Array<EntityId> out;
        if (slot >= kSlots) return out;
        for (const EntityId id : sets_[slot])
            if (alive(id)) out.push_back(id);
        return out;
    }

    bool has(u32 slot) const { return slot < kSlots && !sets_[slot].empty(); }
    u32 count() const;                       // set slots
    usize size(u32 slot) const { return slot < kSlots ? sets_[slot].size() : 0; }
    void clear(u32 slot);
    void clear_all();

private:
    Array<EntityId> sets_[kSlots];
};

}  // namespace mce::editor
