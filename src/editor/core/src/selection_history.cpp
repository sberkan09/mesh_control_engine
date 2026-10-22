#include "mce/editor/selection_history.h"

namespace mce::editor {

void SelectionHistory::visit(EntityId id) {
    if (id == kNoEntity || id == current()) return;
    while (items_.size() > static_cast<usize>(pos_ + 1)) items_.pop_back();  // drop the entries ahead
    items_.push_back(id);
    if (items_.size() > kCap) items_.erase(0);
    pos_ = static_cast<i32>(items_.size()) - 1;
}

}  // namespace mce::editor
