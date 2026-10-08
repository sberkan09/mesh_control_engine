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
    scene_.set_assets(project_.mounted());  // prefab instances draw their content
}

Result<void> EditorSession::open_project(StringView root) {
    play_.stop();
    if (preview_) preview_->set_assets(nullptr);
    scene_.set_assets(nullptr);
    Result<void> r = project_.open(root);
    if (r) project_.ensure_cooked();
    watcher_.watch(r ? project_.root() : StringView());
    attach_preview();
    if (!r) return r;
    new_scene("untitled");
    return ok();
}

void EditorSession::new_scene(StringView name) {
    play_.stop();
    scene_.new_scene(name);
    scene_diags_ = cook::DiagList{};
    autosaved_revision_ = ~u64(0);
    autosave_clock_ = -1.0;
}

cook::String EditorSession::resolve(StringView path) const {
    if (is_absolute(path) || !project_.is_open()) return cook::String(path);
    cook::String in_assets = project_.asset_path(path);
    if (cook::fs::exists(in_assets.view()) || !cook::fs::exists(path)) return in_assets;
    return cook::String(path);
}

bool EditorSession::open_scene(StringView path) {
    play_.stop();
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
    autosaved_revision_ = ~u64(0);
    autosave_clock_ = -1.0;
    if (has_recovery()) MCE_LOG_WARN("editor: {} has unsaved changes from an earlier session (autosave)", full.view());
    return true;
}

Result<void> EditorSession::save_scene() {
    if (scene_.path().empty()) return Error(Errc::InvalidArgument, "editor.save", "scene has no file yet");
    const cook::String path(scene_.path());
    MCE_TRY(scene_.save(path.view()));
    discard_autosave();
    MCE_LOG_INFO("editor: saved {}", path.view());
    return ok();
}

Result<void> EditorSession::save_scene_as(StringView path) {
    const cook::String full = resolve(path);
    const cook::String old_autosave = autosave_path();
    // Two files must never share a GUID: a copy of a scene that already lives on disk is a new asset.
    if (!scene_.path().empty() && scene_.path() != full.view()) scene_.assign_new_guid();
    MCE_TRY(scene_.save(full.view()));
    MCE_LOG_INFO("editor: saved {}", full.view());
    if (!old_autosave.empty()) cook::fs::remove(old_autosave.view());
    discard_autosave();
    project_.rescan();
    return ok();
}

const cook::CookReport& EditorSession::cook(bool pack) {
    play_.stop();  // the play world reads the mounted tree the cook replaces
    if (preview_) preview_->set_assets(nullptr);
    scene_.set_assets(nullptr);
    const cook::CookReport& report = project_.cook(pack);
    attach_preview();
    return report;
}

cook::String EditorSession::autosave_path() const {
    const StringView path = scene_.path();
    cook::String rel;
    if (project_.is_open()) {
        if (path.empty()) rel = "untitled.mcscene";
        else if (!project_.relative_asset_path(path, rel)) rel.clear();
        if (!rel.empty()) {
            cook::String out = cook::path_join(cook::path_join(project_.root(), ".mce/autosave").view(), rel.view());
            out += ".autosave";
            return out;
        }
    }
    if (path.empty()) return {};
    cook::String out(path);
    out += ".autosave";
    return out;
}

bool EditorSession::autosave() {
    if (!scene_.dirty() || scene_.revision() == autosaved_revision_) return false;
    const cook::String path = autosave_path();
    if (path.empty()) return false;
    cook::String text;
    scene_.serialize(text);
    if (Result<void> r = cook::fs::write_file_atomic(path.view(), Span<const byte>(reinterpret_cast<const byte*>(text.c_str()), text.size())); !r) {
        MCE_LOG_WARN("editor: autosave to {} failed", path.view());
        return false;
    }
    autosaved_revision_ = scene_.revision();
    MCE_LOG_INFO("editor: autosaved {}", path.view());
    return true;
}

void EditorSession::tick_autosave(f64 now, f64 interval) {
    if (autosave_clock_ < 0.0 || !scene_.dirty()) {
        autosave_clock_ = now;
        return;
    }
    if (now - autosave_clock_ < interval) return;
    autosave();
    autosave_clock_ = now;
}

bool EditorSession::has_recovery() const {
    const cook::String path = autosave_path();
    cook::String saved, on_disk;
    if (path.empty() || !cook::fs::read_text(path.view(), saved)) return false;
    if (scene_.path().empty()) return true;
    return !cook::fs::read_text(scene_.path(), on_disk) || !(saved == on_disk);
}

bool EditorSession::recover() {
    const cook::String path = autosave_path();
    cook::String text;
    if (path.empty() || !cook::fs::read_text(path.view(), text)) return false;
    cook::DiagList diags;
    if (!scene_.recover(text.view(), diags)) {
        log_diags(diags);
        return false;
    }
    autosaved_revision_ = scene_.revision();
    MCE_LOG_INFO("editor: recovered unsaved changes from {}", path.view());
    return true;
}

void EditorSession::discard_autosave() {
    const cook::String path = autosave_path();
    if (!path.empty()) cook::fs::remove(path.view());
    autosaved_revision_ = ~u64(0);
}

Result<void> EditorSession::rename_asset(asset::Guid guid, StringView new_rel_path) {
    const ProjectAsset* a = project_.find(guid);
    if (!a) return Error(Errc::NotFound, "editor.rename", asset::to_string(guid).view());
    const cook::String old_abs = project_.asset_path(a->path.view());
    const bool is_open_scene = !scene_.path().empty() && scene_.path() == old_abs.view();
    MCE_TRY(project_.move_asset(guid, new_rel_path));
    if (is_open_scene) scene_.set_path(project_.asset_path(new_rel_path).view());
    watcher_.poll(0.0, 0.0);  // the move is ours: re-baseline the watcher
    hot_reload();
    MCE_LOG_INFO("editor: renamed {} -> {}", old_abs.view(), new_rel_path);
    return ok();
}

Result<HotReloadResult> EditorSession::write_asset(StringView rel_path, StringView text) {
    if (!project_.is_open()) return Error(Errc::InvalidArgument, "editor.write_asset", "no project is open");
    if (rel_path.empty() || rel_path[0] == '/' || rel_path.find("..") != StringView::npos)
        return Error(Errc::InvalidArgument, "editor.write_asset", rel_path);
    const cook::String abs = project_.asset_path(rel_path);
    const usize slash = abs.view().rfind('/');
    if (slash != StringView::npos) MCE_TRY(cook::fs::create_directories(abs.view().substr(0, slash)));
    MCE_TRY(cook::fs::write_file_atomic(abs.view(), Span<const byte>(reinterpret_cast<const byte*>(text.data()), text.size())));
    watcher_.poll(0.0, 0.0);  // the write is ours: re-baseline the watcher
    const cook::String rel(rel_path);
    return hot_reload(Span<const cook::String>(&rel, 1));
}

bool EditorSession::tick_hot_reload(f64 now, f64 interval) {
    Array<cook::String> changed;
    if (!project_.is_open() || !watcher_.poll(now, interval, &changed)) return false;
    hot_reload(changed.span());
    return true;
}

HotReloadResult EditorSession::hot_reload(Span<const cook::String> changed) {
    if (!project_.is_open()) return {};
    struct Ctx {
        EditorSession* self;
    } ctx{this};
    last_reload_ = project_.reload([](void* user) {
        EditorSession& s = *static_cast<Ctx*>(user)->self;
        s.play_.stop();  // both hold the mount that is about to be replaced
        if (s.preview_) s.preview_->set_assets(nullptr);
        s.scene_.set_assets(nullptr);
    }, &ctx);
    if (last_reload_.remounted) attach_preview();
    else if (last_reload_.reloaded > 0) scene_.refresh_prefabs();  // a prefab may have been swapped

    // The open scene, changed on disk by something other than this editor's own save.
    cook::String rel;
    if (!scene_.path().empty() && project_.relative_asset_path(scene_.path(), rel)) {
        bool hit = false;
        for (const cook::String& c : changed) hit = hit || c.view() == rel.view();
        cook::String on_disk, mine;
        if (hit && cook::fs::read_text(scene_.path(), on_disk)) {
            scene_.serialize(mine);
            if (!(on_disk == mine)) {
                if (scene_.dirty()) {
                    MCE_LOG_WARN("editor: {} changed on disk; keeping your unsaved edits (save overwrites it)", rel.view());
                } else {
                    // One undoable step (Ctrl+Z brings back what the other tool replaced).
                    play_.stop();
                    cook::DiagList diags;
                    if (scene_.reload_text(on_disk.view(), scene_.path(), diags)) {
                        scene_.mark_clean();
                        last_reload_.scene_reloaded = true;
                    } else {
                        log_diags(diags);
                    }
                }
            }
        }
    }
    MCE_LOG_INFO("editor: hot reload: {} swapped, {} failed{}{} in {:.0f} ms", last_reload_.reloaded, last_reload_.failed,
                 last_reload_.remounted ? ", remounted" : "", last_reload_.scene_reloaded ? ", scene re-read" : "",
                 last_reload_.seconds * 1000.0);
    return last_reload_;
}

bool EditorSession::enter_play() {
    play_diags_ = cook::DiagList{};
    project_.ensure_cooked();
    if (Result<void> r = play_.enter(project_, scene_.doc(), scene_.path(), play_diags_); !r) {
        MCE_LOG_ERROR("editor: cannot play {}", scene_.doc().name());
        log_diags(play_diags_);
        return false;
    }
    return true;
}

void EditorSession::list_scenes(Array<const ProjectAsset*>& out) const {
    out.clear();
    for (const ProjectAsset& a : project_.assets())
        if (a.type == asset::ChunkType::Scene) out.push_back(&a);
}

}  // namespace mce::editor
