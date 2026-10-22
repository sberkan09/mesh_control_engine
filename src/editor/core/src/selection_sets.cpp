#include "mce/editor/selection_sets.h"

namespace mce::editor {

void SelectionSets::save(u32 slot, Span<const EntityId> ids) {
    if (slot >= kSlots) return;
    Array<EntityId> set;
    for (const EntityId id : ids) {
        if (id == kNoEntity) continue;
        bool seen = false;
        for (const EntityId s : set) seen = seen || s == id;
        if (!seen) set.push_back(id);
    }
    sets_[slot] = static_cast<Array<EntityId>&&>(set);
}

u32 SelectionSets::count() const {
    u32 n = 0;
    for (const Array<EntityId>& s : sets_) n += s.empty() ? 0u : 1u;
    return n;
}

void SelectionSets::clear(u32 slot) {
    if (slot < kSlots) sets_[slot].clear();
}

void SelectionSets::clear_all() {
    for (Array<EntityId>& s : sets_) s.clear();
}

}  // namespace mce::editor
