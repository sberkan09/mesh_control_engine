#include "mce/editor/editor_session.h"

#include <cstdio>
#include <cstring>

#include "mce/cook/cook.h"
#include "mce/cook/envmap.h"
#include "mce/cook/fs.h"
#include "mce/cook/json.h"
#include "mce/ecs/hierarchy.h"
#include "mce/ecs/transform.h"
#include "mce/kit/kit3d.h"
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

// The scene file's stem ("untitled" before its first save): the folder of its bakes.
StringView scene_stem(StringView path) {
    StringView stem = path.empty() ? StringView("untitled") : path;
    if (const usize slash = stem.find_last_of("/\\"); slash != StringView::npos) stem = stem.substr(slash + 1);
    if (const usize dot = stem.find('.'); dot != StringView::npos) stem = stem.substr(0, dot);
    return stem;
}

// Writes a baked environment (.mcenv, envmap.h) at `abs` and its "envmap" sidecar, keeping the
// sidecar's GUID when the file was baked before. `guid` receives it.
Result<void> write_bake(const cook::String& abs, u32 width, u32 height, u32 layers, Span<const u8> rgba, asset::Guid& guid) {
    cook::String sidecar_abs = abs;
    sidecar_abs += ".mceasset";
    guid = {};
    cook::String sidecar;
    if (cook::fs::read_text(sidecar_abs.view(), sidecar)) {  // a re-bake keeps the GUID
        cook::JsonDoc doc;
        cook::JsonError err;
        if (doc.parse(sidecar.view(), err)) {
            if (auto g = asset::parse_guid(doc.root().get("guid").as_string())) guid = g.value();
        }
    }
    const usize slash = abs.view().rfind('/');
    if (slash != StringView::npos) MCE_TRY(cook::fs::create_directories(abs.view().substr(0, slash)));
    if (guid == asset::Guid{}) {
        guid = cook::generate_guid();
        cook::String text("{ \"guid\": \"");
        text += asset::to_string(guid).view();
        text += "\", \"importer\": \"envmap\", \"importer_version\": 1, \"settings\": {} }\n";
        MCE_TRY(cook::fs::write_file_atomic(sidecar_abs.view(), Span<const byte>(reinterpret_cast<const byte*>(text.view().data()), text.size())));
    }
    Array<byte> file;
    cook::write_envmap(width, height, layers, rgba, file);
    return cook::fs::write_file_atomic(abs.view(), file.span());
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
    scene_.set_prefab_apply([](void* user, asset::Guid prefab, const DocComponent& value, u64 fields) {
        return static_cast<EditorSession*>(user)->apply_to_prefab(prefab, value, fields);
    }, this);
}

Result<u32> EditorSession::bake_reflection_probes() {
    constexpr const char* kWhere = "editor.bake_probes";
    static_assert(render::Forward3D::kProbeRgbmRange == cook::kEnvRgbmRange, "the renderer and .mcenv agree on RGBM");
    if (!project_.is_open()) return Error(Errc::InvalidArgument, kWhere, "no project is open");
    if (!preview_) return Error(Errc::InvalidArgument, kWhere, "no preview renderer");
    Array<ProbeBake> bakes;
    MCE_TRY(preview_->bake_probes(scene_.world(), bakes));

    // assets/probes/<scene file stem>/probe_<id>.mcenv: stable per probe, so re-bakes overwrite.
    const StringView stem = scene_stem(scene_.path());
    struct Written {
        EntityId id;
        asset::Guid guid;
    };
    Array<Written> written;
    Array<cook::String> changed;
    const rhi::Extent2D es = render::Forward3D::probe_export_size();
    for (const ProbeBake& b : bakes) {
        const EntityId id = scene_.entity_for(b.entity);
        if (id == kNoEntity || !scene_.get<kit::ReflectionProbe3D>(id)) continue;  // a prefab ghost
        cook::String rel("probes/");
        rel += stem;
        rel += "/probe_";
        char num[16];
        std::snprintf(num, sizeof(num), "%u", id);
        rel += StringView(num);
        rel += ".mcenv";
        asset::Guid guid;
        MCE_TRY(write_bake(project_.asset_path(rel.view()), es.width, es.height / render::Forward3D::kProbeLayers,
                           render::Forward3D::kProbeLayers, b.rgba.span(), guid));
        changed.push_back(rel);
        written.push_back({id, guid});
    }
    if (written.empty()) return u32(0);
    watcher_.poll(0.0, 0.0);  // the writes are ours: re-baseline the watcher
    const HotReloadResult r = hot_reload(changed.span());
    if (r.failed > 0) return Error(Errc::ImportFailed, kWhere, "a baked probe did not cook (see the log)");

    scene_.begin_edit("Bake reflection probes");
    for (const Written& w : written) {
        kit::ReflectionProbe3D p = *scene_.get<kit::ReflectionProbe3D>(w.id);
        p.baked.guid = w.guid;
        scene_.set(w.id, p);
    }
    scene_.end_edit();
    MCE_LOG_INFO("editor: baked {} reflection probe(s) into assets/probes/{}/", written.size(), stem);
    return static_cast<u32>(written.size());
}

Result<void> EditorSession::bake_gi_volume() {
    constexpr const char* kWhere = "editor.bake_gi";
    static_assert(render::Forward3D::kGiRgbmRange == cook::kEnvRgbmRange, "the renderer and .mcenv agree on RGBM");
    if (!project_.is_open()) return Error(Errc::InvalidArgument, kWhere, "no project is open");
    if (!preview_) return Error(Errc::InvalidArgument, kWhere, "no preview renderer");
    GiBake bake;
    MCE_TRY(preview_->bake_gi(scene_.world(), bake));
    const EntityId id = scene_.entity_for(bake.entity);
    if (id == kNoEntity || !scene_.get<kit::GiVolume3D>(id))
        return Error(Errc::Unsupported, kWhere, "the volume belongs to a prefab instance: bake it in its prefab");

    // assets/gi/<scene file stem>/gi_<id>.mcenv: one RGBM layer of the volume's texels.
    cook::String rel("gi/");
    rel += scene_stem(scene_.path());
    rel += "/gi_";
    char num[16];
    std::snprintf(num, sizeof(num), "%u", id);
    rel += StringView(num);
    rel += ".mcenv";
    asset::Guid guid;
    MCE_TRY(write_bake(project_.asset_path(rel.view()), bake.width, bake.height, 1, bake.rgba.span(), guid));
    watcher_.poll(0.0, 0.0);  // the write is ours: re-baseline the watcher
    const HotReloadResult r = hot_reload(Span<const cook::String>(&rel, 1));
    if (r.failed > 0) return Error(Errc::ImportFailed, kWhere, "the baked volume did not cook (see the log)");

    scene_.begin_edit("Bake GI volume");
    kit::GiVolume3D g = *scene_.get<kit::GiVolume3D>(id);
    g.baked.guid = guid;
    scene_.set(id, g);
    scene_.end_edit();
    MCE_LOG_INFO("editor: baked the GI volume into assets/{}", rel.view());
    return ok();
}

Result<void> EditorSession::apply_to_prefab(asset::Guid prefab, const DocComponent& value, u64 fields) {
    const ProjectAsset* a = project_.is_open() ? project_.find(prefab) : nullptr;
    if (!a) return Error(Errc::NotFound, "editor.prefab_apply", "the prefab is not in the open project");
    const cook::String rel(a->path);
    const cook::String abs = project_.asset_path(rel.view());
    cook::String text;
    if (!cook::fs::read_text(abs.view(), text)) return Error(Errc::IoError, "editor.prefab_apply", abs.view());
    SceneDocument doc;
    cook::DiagList diags;
    if (!doc.parse(text.view(), abs.view(), diags) || doc.kind() != DocKind::Prefab || doc.size() == 0) {
        log_diags(diags);
        return Error(Errc::ParseError, "editor.prefab_apply", abs.view());
    }
    // The prefab root takes the values; a root that is itself an instance overrides them.
    const EntityId root = doc.at(0).id;
    DocComponent* dst = doc.add_component(root, *value.type);
    copy_fields(*value.type, fields, value.data(), dst->data());
    if (!doc.at(0).prefab.is_null()) dst->overrides |= fields;
    cook::String out;
    doc.serialize(out);
    MCE_TRY(write_asset(rel.view(), out.view()));  // re-cooks the prefab and refreshes the ghosts
    MCE_LOG_INFO("editor: applied {} to prefab {}", value.type->name, rel.view());
    return ok();
}

Result<asset::Guid> EditorSession::create_prefab_from(EntityId root, StringView rel_path, bool replace) {
    constexpr const char* kWhere = "editor.prefab_create";
    if (!project_.is_open()) return Error(Errc::InvalidArgument, kWhere, "no project is open");
    const DocEntity* top = scene_.doc().find(root);
    if (!top) return Error(Errc::NotFound, kWhere, "no such entity");
    if (rel_path.size() < 10 || rel_path.substr(rel_path.size() - 9) != ".mcprefab")
        return Error(Errc::InvalidArgument, kWhere, "a prefab path ends in .mcprefab");
    if (cook::fs::exists(project_.asset_path(rel_path).view())) return Error(Errc::AlreadyExists, kWhere, rel_path);

    // The subtree in document order, root first.
    Array<EntityId> ids;
    ids.push_back(root);
    for (const DocEntity& e : scene_.doc().entities())
        if (e.id != root && scene_.is_ancestor(root, e.id)) ids.push_back(e.id);
    SceneDocument prefab;
    const cook::String name(top->name.view());
    prefab.reset(name.view());
    prefab.set_kind(DocKind::Prefab);
    prefab.set_guid(cook::generate_guid());
    for (EntityId id : ids) {
        const DocEntity& e = *scene_.doc().find(id);
        prefab.insert(e.name.view(), -1, e.id);  // same ids: internal references keep resolving
        prefab.find(e.id)->prefab = e.prefab;
        for (const DocComponent& c : e.components) {
            if (id == root && c.type == &refl::type_of<ChildOf>()) continue;  // the instance keeps the parent
            DocComponent* dst = prefab.add_component(e.id, *c.type);
            std::memcpy(dst->data(), c.data(), c.type->size);
            dst->overrides = c.overrides;
            visit_entity_refs(*c.type, dst->data(), [](Entity* f, void* user) {
                const Array<EntityId>& inside = *static_cast<const Array<EntityId>*>(user);
                bool keep = false;
                for (EntityId i : inside) keep |= i == doc_entity_id(*f);
                if (!keep) *f = Entity::null;
            }, &ids);
        }
    }
    // The root's placement is the instance's own: the prefab keeps an identity transform there.
    if (DocComponent* t = prefab.find(root)->find(refl::type_of<LocalTransform>())) {
        const LocalTransform identity{};
        std::memcpy(t->data(), &identity, sizeof(identity));
    }
    cook::String text;
    prefab.serialize(text);
    MCE_TRY(write_asset(rel_path, text.view()));
    const asset::Guid guid = prefab.guid();
    if (!project_.find(guid)) return Error(Errc::ImportFailed, kWhere, "the new prefab did not cook (see the log)");
    MCE_LOG_INFO("editor: saved {} entities as prefab {}", ids.size(), rel_path);
    if (!replace) return guid;

    const LocalTransform placement = scene_.get<LocalTransform>(root) ? *scene_.get<LocalTransform>(root) : LocalTransform{};
    const EntityId parent = scene_.parent_of(root);
    const i32 index = scene_.doc().index_of(root);
    scene_.begin_edit("Replace with prefab");
    scene_.destroy_entity(root);
    const EntityId inst = scene_.create_prefab_instance(name.view(), guid, index);
    scene_.set(inst, placement);
    if (parent != kNoEntity) scene_.set_parent(inst, parent);
    scene_.end_edit();
    return guid;
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

Result<void> EditorSession::save_scene_as(StringView path, asset::Guid keep) {
    const cook::String full = resolve(path);
    const cook::String old_autosave = autosave_path();
    // Two files must never share a GUID: a copy of a scene that already lives on disk is a new asset,
    // unless the caller overwrites an existing file and hands over that file's GUID.
    if (!keep.is_null()) scene_.set_guid(keep);
    else if (!scene_.path().empty() && scene_.path() != full.view()) scene_.assign_new_guid();
    MCE_TRY(scene_.save(full.view()));
    MCE_LOG_INFO("editor: saved {}", full.view());
    if (!old_autosave.empty()) cook::fs::remove(old_autosave.view());
    discard_autosave();
    project_.rescan();
    return ok();
}

const cook::CookReport& EditorSession::cook(bool pack, bool adopt) {
    play_.stop();  // the play world reads the mounted tree the cook replaces
    if (preview_) preview_->set_assets(nullptr);
    scene_.set_assets(nullptr);
    const cook::CookReport& report = project_.cook(pack, adopt);
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
    ++reload_count_;
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
