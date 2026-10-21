#include "mce/editor/hot_reload.h"

#include <algorithm>

#include "mce/cook/fs.h"

namespace mce::editor {

void AssetWatcher::watch(StringView project_root) {
    root_ = cook::String(project_root);
    entries_.clear();
    last_ = -1.0;
    if (!root_.empty()) scan(entries_);
}

void AssetWatcher::scan(Array<Entry>& out) const {
    out.clear();
    const cook::String assets = cook::path_join(root_.view(), "assets");
    Array<cook::String> files;
    if (!cook::fs::list_files(assets.view(), files)) return;
    for (cook::String& f : files) {
        Entry e;
        if (!cook::fs::stat(cook::path_join(assets.view(), f.view()).view(), e.size, e.mtime)) continue;
        e.path = std::move(f);
        out.push_back(std::move(e));
    }
}

bool AssetWatcher::poll(f64 now, f64 interval, Array<cook::String>* changed) {
    if (changed) changed->clear();
    if (root_.empty()) return false;
    if (interval > 0.0 && last_ >= 0.0 && now - last_ < interval) return false;
    last_ = now;
    Array<Entry> fresh;
    scan(fresh);
    // Both lists are sorted by path: one merge walk finds additions, removals and edits.
    bool any = false;
    usize i = 0, j = 0;
    const auto note = [&](const cook::String& p) {
        any = true;
        if (changed) changed->push_back(p);
    };
    while (i < entries_.size() || j < fresh.size()) {
        if (j == fresh.size() || (i < entries_.size() && entries_[i].path.view() < fresh[j].path.view())) {
            note(entries_[i++].path);  // removed
        } else if (i == entries_.size() || fresh[j].path.view() < entries_[i].path.view()) {
            note(fresh[j++].path);  // added
        } else {
            if (entries_[i].size != fresh[j].size || entries_[i].mtime != fresh[j].mtime) note(fresh[j].path);
            ++i;
            ++j;
        }
    }
    entries_ = std::move(fresh);
    return any;
}

}  // namespace mce::editor
