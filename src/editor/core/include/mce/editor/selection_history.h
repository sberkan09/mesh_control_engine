#pragma once
// SelectionHistory — Back / Forward through the entities you selected (M-SELHISTORY), like a browser's history.
// The shell calls visit() with the selection's anchor whenever it changes; Alt+Left and Alt+Right step through the
// list. Selecting something new while stepped back drops the entries ahead (the browser rule). back() and forward()
// skip entries the caller says are gone (a deleted entity). Headless, tested in CI.

#include "mce/containers/array.h"
#include "mce/editor/scene_document.h"

namespace mce::editor {

class SelectionHistory {
public:
    static constexpr u32 kCap = 64;

    // `id` was just selected: becomes the current entry (nothing when it already is, or is kNoEntity); the entries
    // ahead of the current one are dropped, and the oldest go beyond kCap.
    void visit(EntityId id);

    bool can_back() const { return pos_ > 0; }
    bool can_forward() const { return pos_ >= 0 && static_cast<usize>(pos_) + 1 < items_.size(); }

    // Steps to the nearest earlier (later) entry for which alive(id) is true and returns it; kNoEntity (position
    // unchanged) when there is none.
    template <class Alive>
    EntityId back(Alive&& alive) {
        for (i32 i = pos_ - 1; i >= 0; --i)
            if (alive(items_[static_cast<usize>(i)])) {
                pos_ = i;
                return items_[static_cast<usize>(i)];
            }
        return kNoEntity;
    }
    template <class Alive>
    EntityId forward(Alive&& alive) {
        for (usize i = (static_cast<usize>(pos_) + 1); i < items_.size(); ++i)
            if (alive(items_[i])) {
                pos_ = static_cast<i32>(i);
                return items_[i];
            }
        return kNoEntity;
    }

    // The entry the history stands on (kNoEntity when empty).
    EntityId current() const { return pos_ < 0 ? kNoEntity : items_[static_cast<usize>(pos_)]; }
    usize size() const { return items_.size(); }
    void clear() {
        items_.clear();
        pos_ = -1;
    }

private:
    Array<EntityId> items_;
    i32 pos_ = -1;
};

}  // namespace mce::editor
