#include "mce/editor/recent_list.h"

namespace mce::editor {
namespace {

// The index of `path` in `items`, or -1.
i32 find(const Array<cook::String>& items, StringView path) {
    for (usize i = 0; i < items.size(); ++i)
        if (items[i].view() == path) return static_cast<i32>(i);
    return -1;
}

}  // namespace

void RecentList::touch(StringView path) {
    if (path.empty()) return;
    remove(path);
    items_.insert(0, cook::String(path));
    while (items_.size() > cap_) items_.pop_back();
}

bool RecentList::remove(StringView path) {
    const i32 at = find(items_, path);
    if (at < 0) return false;
    items_.erase(static_cast<usize>(at));
    return true;
}

cook::String RecentList::serialize() const {
    cook::String out;
    for (const cook::String& s : items_) {
        out += s.view();
        out += '\n';
    }
    return out;
}

void RecentList::parse(StringView text) {
    items_.clear();
    usize pos = 0;
    while (pos <= text.size() && items_.size() < cap_) {
        usize end = pos;
        while (end < text.size() && text[end] != '\n') ++end;
        usize len = end - pos;
        if (len > 0 && text[pos + len - 1] == '\r') --len;
        const StringView line(text.data() + pos, len);
        if (!line.empty() && find(items_, line) < 0) {
            items_.push_back(cook::String(line));
        }
        pos = end + 1;
    }
}

}  // namespace mce::editor
