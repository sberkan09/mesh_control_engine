#include "mce/editor/editor_session.h"

#include "mce/cook/fs.h"
#include "mce/editor/preview_renderer.h"
#include "mce/log.h"

namespace mce::editor {

namespace {

bool is_absolute(StringView p) {
    return !p.empty() && (p[0] == '/' || p[0] == '\\' || (p.size() > 1 && p[1] == ':'));
}

void log_diags(const cook::DiagList& diags) {
    for (const cook::Diagnostic& d : diags.all()) {
        cook::String line;
        cook::format_diagnostic(d, line);
        if (d.severity == cook::Severity::Error) MCE_LOG_ERROR("{}", line.view());
        else MCE_LOG_WARN("{}", line.view());
    }
}

}  // namespace

void EditorSession::set_preview(PreviewRenderer* preview) {
    if (preview_) preview_->set_assets(nullptr);
    preview_ = preview;
    attach_preview();
}

void EditorSession::attach_preview() {
    if (preview_) preview_->set_assets(project_.mounted());
}

Result<void> EditorSession::open_project(StringView root) {
    if (preview_) preview_->set_assets(nullptr);
    Result<void> r = project_.open(root);
    if (r) project_.ensure_cooked();
    attach_preview();
    if (!r) return r;
    new_scene("untitled");
    return ok();
}

void EditorSession::new_scene(StringView name) {
    scene_.new_scene(name);
    scene_diags_ = cook::DiagList{};
}

cook::String EditorSession::resolve(StringView path) const {
    if (is_absolute(path) || !project_.is_open()) return cook::String(path);
    cook::String in_assets = project_.asset_path(path);
    if (cook::fs::exists(in_assets.view()) || !cook::fs::exists(path)) return in_assets;
    return cook::String(path);
}

bool EditorSession::open_scene(StringView path) {
    cook::DiagList diags;
    const cook::String full = resolve(path);
    const bool ok_open = static_cast<bool>(scene_.open(full.view(), diags));
    scene_diags_ = std::move(diags);
    if (!ok_open) {
        MCE_LOG_ERROR("editor: cannot open scene {}", full.view());
        log_diags(scene_diags_);
        return false;
    }
    log_diags(scene_diags_);
    MCE_LOG_INFO("editor: opened {} ({} entities)", full.view(), scene_.doc().size());
    return true;
}

Result<void> EditorSession::save_scene() {
    if (scene_.path().empty()) return Error(Errc::InvalidArgument, "editor.save", "scene has no file yet");
    const cook::String path(scene_.path());
    MCE_TRY(scene_.save(path.view()));
    MCE_LOG_INFO("editor: saved {}", path.view());
    return ok();
}

Result<void> EditorSession::save_scene_as(StringView path) {
    const cook::String full = resolve(path);
    // Two files must never share a GUID: a copy of a scene that already lives on disk is a new asset.
    if (!scene_.path().empty() && scene_.path() != full.view()) scene_.assign_new_guid();
    MCE_TRY(scene_.save(full.view()));
    MCE_LOG_INFO("editor: saved {}", full.view());
    project_.rescan();
    return ok();
}

const cook::CookReport& EditorSession::cook(bool pack) {
    if (preview_) preview_->set_assets(nullptr);
    const cook::CookReport& report = project_.cook(pack);
    attach_preview();
    return report;
}

void EditorSession::list_scenes(Array<const ProjectAsset*>& out) const {
    out.clear();
    for (const ProjectAsset& a : project_.assets())
        if (a.type == asset::ChunkType::Scene) out.push_back(&a);
}

}  // namespace mce::editor
