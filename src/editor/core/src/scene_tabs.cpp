#include "mce/editor/scene_tabs.h"

#include "mce/cook/fs.h"

namespace mce::editor {

int SceneTabs::find(StringView path) const {
    for (u32 i = 0; i < tabs_.size(); ++i)
        if (tabs_[i].path.view() == path) return static_cast<int>(i);
    return -1;
}

bool SceneTabs::dirty(const EditorSession& session, u32 index) const {
    if (index >= tabs_.size()) return false;
    if (static_cast<int>(index) == active_) return session.scene().dirty();
    return tabs_[index].parked_dirty;
}

bool SceneTabs::any_dirty(const EditorSession& session) const {
    if (session.scene().dirty()) return true;
    for (u32 i = 0; i < tabs_.size(); ++i)
        if (dirty(session, i)) return true;
    return false;
}

void SceneTabs::sync(const EditorSession& session) {
    const StringView path = session.scene().path();
    if (path.empty()) {
        active_ = -1;
        return;
    }
    int at = find(path);
    if (at < 0) {
        Tab t;
        t.path = cook::String(path);
        tabs_.push_back(static_cast<Tab&&>(t));
        at = static_cast<int>(tabs_.size()) - 1;
    }
    active_ = at;
    tabs_[static_cast<u32>(at)].parked_dirty = false;
}

bool SceneTabs::park(EditorSession& session, cook::String& err) {
    const EditorScene& scene = session.scene();
    if (!scene.dirty()) {
        if (active_ >= 0) tabs_[static_cast<u32>(active_)].parked_dirty = false;
        return true;
    }
    if (scene.path().empty() || active_ < 0) {
        err += "the open scene has unsaved changes and no file yet: save it first";
        return false;
    }
    if (!session.autosave() && !session.has_recovery()) {
        err += "could not park the unsaved changes";
        return false;
    }
    tabs_[static_cast<u32>(active_)].parked_dirty = true;
    return true;
}

bool SceneTabs::open(EditorSession& session, StringView path, cook::String& err) {
    sync(session);
    const cook::String full = session.resolve(path);
    if (active_ >= 0 && tabs_[static_cast<u32>(active_)].path.view() == full.view()) return true;
    if (!park(session, err)) return false;
    const int parked = active_;
    const int known = find(full.view());
    const bool restore = known >= 0 && tabs_[static_cast<u32>(known)].parked_dirty;
    if (!session.open_scene(full.view())) {
        err += "the scene does not open";
        if (parked >= 0) tabs_[static_cast<u32>(parked)].parked_dirty = session.scene().dirty() || tabs_[static_cast<u32>(parked)].parked_dirty;
        return false;
    }
    sync(session);
    if (restore) {
        if (session.recover()) tabs_[static_cast<u32>(active_)].parked_dirty = false;
        else err += "the parked changes could not be restored";
    }
    return true;
}

bool SceneTabs::activate(EditorSession& session, u32 index, cook::String& err) {
    if (index >= tabs_.size()) {
        err += "no such tab";
        return false;
    }
    const cook::String path = tabs_[index].path;
    return open(session, path.view(), err);
}

bool SceneTabs::close(EditorSession& session, u32 index, bool discard, cook::String& err) {
    sync(session);
    if (index >= tabs_.size()) {
        err += "no such tab";
        return false;
    }
    const bool is_active = static_cast<int>(index) == active_;
    if (is_active && session.scene().dirty() && !discard) {
        err += "the scene has unsaved changes";
        return false;
    }
    const cook::String path = tabs_[index].path;
    if (!is_active) {
        if (discard && tabs_[index].parked_dirty) {
            const cook::String autosave = session.autosave_path_for(path.view());
            if (!autosave.empty()) cook::fs::remove(autosave.view());
        }
        tabs_.erase(index);
        if (active_ > static_cast<int>(index)) --active_;
        return true;
    }
    if (discard) session.discard_autosave();
    tabs_.erase(index);
    if (tabs_.empty()) {
        session.new_scene("untitled");
        active_ = -1;
        return true;
    }
    const u32 next = index > 0 ? index - 1 : 0;
    active_ = -1;
    const cook::String next_path = tabs_[next].path;
    const bool restore = tabs_[next].parked_dirty;
    if (!session.open_scene(next_path.view())) {
        err += "the neighbouring scene does not open";
        return false;
    }
    sync(session);
    if (restore && session.recover()) tabs_[static_cast<u32>(active_)].parked_dirty = false;
    return true;
}

}  // namespace mce::editor
