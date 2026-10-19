#pragma once
// SceneTabs — several scene files open in one editor (M-SCENETABS). The session still holds ONE open scene; a tab
// that is not active is parked: its file path stays in the list, and when it had unsaved changes they sit in the
// autosave file the session already writes (EditorSession::autosave), so a switch never loses work and a crash
// recovers it. Coming back re-reads the file and, for a parked-dirty tab, recovers the autosave on top (dirty, a
// fresh undo history). Headless, tested in CI; the Scenes panel draws it.

#include "mce/cook/string.h"
#include "mce/editor/editor_session.h"

namespace mce::editor {

class SceneTabs {
public:
    struct Tab {
        cook::String path;  // the scene file as the session reports it (resolved); never empty
        bool parked_dirty = false;  // unsaved changes wait in the autosave file
    };

    // The tabs in the order they were opened. The open scene is `active()` once it has a file.
    Span<const Tab> tabs() const { return Span<const Tab>(tabs_.data(), tabs_.size()); }
    // Index of the tab the session's scene is, or -1 (an untitled scene has no tab).
    int active() const { return active_; }
    // True when the tab has unsaved changes (the active one asks the session's scene).
    bool dirty(const EditorSession& session, u32 index) const;
    bool any_dirty(const EditorSession& session) const;

    // Brings the list in line with the session after something else opened or saved a scene (File > Open, Save
    // As, a rename): the session's scene becomes the active tab, added when it has a file and was not listed.
    void sync(const EditorSession& session);

    // Opens `path` (as for EditorSession::open_scene) in its tab: parks the open scene first, then re-reads the
    // file and restores parked changes. False with `err` set when the open scene is dirty and untitled (it has no
    // file to park beside: the caller asks first) or when the file does not open (the open scene stays).
    bool open(EditorSession& session, StringView path, cook::String& err);
    // Switches to tab `index`; the same as open() on its path.
    bool activate(EditorSession& session, u32 index, cook::String& err);
    // Closes tab `index`. A dirty tab keeps its parked changes unless `discard`; closing the active tab switches
    // to the previous tab, or starts an untitled scene when it was the last. False when the active scene is dirty
    // and closing it would lose it without `discard`.
    bool close(EditorSession& session, u32 index, bool discard, cook::String& err);

private:
    int find(StringView path) const;
    bool park(EditorSession& session, cook::String& err);

    Array<Tab> tabs_;
    int active_ = -1;
};

}  // namespace mce::editor
