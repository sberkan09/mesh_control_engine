#pragma once
// EditorSession — the open project + the open scene (editor-tools.md §2.2): the file-level
// commands the menu bar, the cook panel and the selftests share (new/open/save/save-as/cook).
// ImGui-free, so the whole open -> edit -> save -> cook loop runs headless in CI.
//
// Paths: scene paths are absolute (or relative to the working directory); `scene_rel` helpers
// take paths relative to the project's assets/ folder, which is how the catalog lists them.

#include "mce/editor/editor_scene.h"
#include "mce/editor/project.h"

namespace mce::editor {

class PreviewRenderer;

class EditorSession {
public:
    EditorProject& project() { return project_; }
    const EditorProject& project() const { return project_; }
    EditorScene& scene() { return scene_; }
    const EditorScene& scene() const { return scene_; }

    // The renderer that draws the mounted content; detached and re-attached around every cook
    // (the old asset system dies inside the cook). Null: no preview.
    void set_preview(PreviewRenderer* preview);

    // Opens `root` (must hold assets/), cooks it when nothing is cooked yet, starts an empty scene.
    Result<void> open_project(StringView root);

    void new_scene(StringView name);
    // `path` absolute, or relative to assets/ when a project is open and the file exists there.
    // On failure the current scene stays open and scene_diags() says why.
    bool open_scene(StringView path);
    Result<void> save_scene();                       // to path(); fails when the scene was never saved
    Result<void> save_scene_as(StringView path);     // same path resolution as open_scene
    const cook::DiagList& scene_diags() const { return scene_diags_; }

    // Cooks the project (pack: also writes <root>/assets.mce) and remounts the preview.
    const cook::CookReport& cook(bool pack = false);

    // Every scene in the catalog, by path.
    void list_scenes(Array<const ProjectAsset*>& out) const;
    // Absolute path of a scene path given relative to assets/ (or `path` unchanged).
    cook::String resolve(StringView path) const;

private:
    void attach_preview();

    EditorProject project_;
    EditorScene scene_;
    PreviewRenderer* preview_ = nullptr;
    cook::DiagList scene_diags_;
};

}  // namespace mce::editor
