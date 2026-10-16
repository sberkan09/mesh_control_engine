#include "mce/editor/project.h"

#include "mce/asset/file_io.h"
#include "mce/cook/fs.h"
#include "mce/log.h"
#include "mce/platform/platform.h"

namespace mce::editor {

namespace {

constexpr const char* kStage = "editor.project";

bool is_dir(const cook::String& p) { return cook::fs::is_directory(p.view()); }
bool is_file(const cook::String& p) { return cook::fs::exists(p.view()) && !cook::fs::is_directory(p.view()); }

}  // namespace

const char* asset_type_name(asset::ChunkType type) {
    switch (type) {
        case asset::Texture::chunk: return asset::Texture::name;
        case asset::Mesh::chunk: return asset::Mesh::name;
        case asset::Material::chunk: return asset::Material::name;
        case asset::Shader::chunk: return asset::Shader::name;
        case asset::Sfx::chunk: return asset::Sfx::name;
        case asset::Font::chunk: return asset::Font::name;
        case asset::Atlas::chunk: return asset::Atlas::name;
        case asset::Scene::chunk: return asset::Scene::name;
        case asset::InputMap::chunk: return asset::InputMap::name;
        case asset::Prefab::chunk: return asset::Prefab::name;
        case asset::Tilemap::chunk: return asset::Tilemap::name;
        case asset::ParticlePreset::chunk: return asset::ParticlePreset::name;
        case asset::Sequence::chunk: return asset::Sequence::name;
        case asset::Timeline::chunk: return asset::Timeline::name;
        case asset::SpriteSheet::chunk: return asset::SpriteSheet::name;
        case asset::AnimGraph::chunk: return asset::AnimGraph::name;
        case asset::UiDocument::chunk: return asset::UiDocument::name;
        case asset::UiTheme::chunk: return asset::UiTheme::name;
        case asset::Terrain::chunk: return asset::Terrain::name;
        case asset::Video::chunk: return asset::Video::name;
        case asset::BehaviorTree::chunk: return asset::BehaviorTree::name;
        case asset::Script::chunk: return asset::Script::name;
        case asset::StringTable::chunk: return asset::StringTable::name;
        case asset::StoreCatalog::chunk: return asset::StoreCatalog::name;
        case asset::GameServicesCatalogChunk::chunk: return asset::GameServicesCatalogChunk::name;
        case asset::RemoteConfigChunk::chunk: return asset::RemoteConfigChunk::name;
        default: return "";
    }
}

EditorProject::EditorProject() = default;
EditorProject::~EditorProject() = default;

Result<void> EditorProject::open(StringView root) {
    // Absolute, so every path joined to it (save-as, asset_path, resolve) means one place whatever
    // the caller passed: a relative root once made save-as write under assets/<root>/assets/.
    cook::String r = cook::fs::absolute(root);
    while (r.size() > 1 && (r.view().back() == '/' || r.view().back() == '\\')) r = cook::String(r.view().substr(0, r.size() - 1));
    if (!is_dir(cook::path_join(r.view(), "assets"))) return Error(Errc::FileNotFound, kStage, root);
    close();
    root_ = r;
    rescan();
    MCE_LOG_INFO("editor: project {} ({} assets)", root_.view(), assets_.size());
    return ok();
}

void EditorProject::close() {
    root_.clear();
    assets_.clear();
    scan_diags_ = cook::DiagList{};
    report_ = cook::CookReport{};
    mounted_.reset();
    cooked_ = false;
    ++generation_;
}

cook::String EditorProject::asset_path(StringView rel) const {
    return cook::path_join(cook::path_join(root_.view(), "assets").view(), rel);
}

bool EditorProject::relative_asset_path(StringView path, cook::String& rel) const {
    const cook::String base = cook::path_join(root_.view(), "assets");
    if (path.size() <= base.size() + 1 || path.substr(0, base.size()) != base.view()) return false;
    const char sep = path[base.size()];
    if (sep != '/' && sep != '\\') return false;
    rel = cook::String(path.substr(base.size() + 1));
    return true;
}

void EditorProject::rescan() {
    assets_.clear();
    scan_diags_ = cook::DiagList{};
    if (!is_open()) return;
    cook::AssetRegistry reg;
    reg.scan(root_.view(), service_.importers(), false, scan_diags_);
    for (usize i = 0; i < reg.size(); ++i) {
        const cook::AssetRecord& r = reg.at(i);
        ProjectAsset a;
        a.guid = r.guid;
        a.path = r.source_path;
        a.type = r.importer ? r.importer->type() : asset::ChunkType::Manifest;
        assets_.push_back(std::move(a));
    }
}

Result<void> EditorProject::move_asset(asset::Guid guid, StringView new_rel_path) {
    const ProjectAsset* a = find(guid);
    if (!is_open() || !a) return Error(Errc::NotFound, kStage, asset::to_string(guid).view());
    if (new_rel_path.empty() || new_rel_path.find("..") != StringView::npos || new_rel_path[0] == '/')
        return Error(Errc::InvalidArgument, kStage, new_rel_path);
    if (!(cook::lower_extension(new_rel_path) == cook::lower_extension(a->path.view())))
        return Error(Errc::InvalidArgument, kStage, "an asset keeps its extension when renamed");
    cook::AssetRegistry reg;
    cook::DiagList diags;
    reg.scan(root_.view(), service_.importers(), false, diags);
    const cook::String target = asset_path(new_rel_path);
    const usize slash = target.view().rfind('/');
    if (slash != StringView::npos) MCE_TRY(cook::fs::create_directories(target.view().substr(0, slash)));
    Result<void> r = reg.move(guid, new_rel_path);
    rescan();
    return r;
}

const ProjectAsset* EditorProject::find(asset::Guid g) const {
    for (const ProjectAsset& a : assets_)
        if (a.guid == g) return &a;
    return nullptr;
}

const cook::CookReport& EditorProject::cook(bool pack, bool adopt) {
    if (!is_open()) return report_;
    rescan();
    cook::CookOptions opt;
    opt.pack = pack;
    opt.adopt = adopt;
    report_ = service_.cook(root_.view(), opt);
    if (adopt) rescan();  // the catalog picks up the sidecars the cook just wrote
    cooked_ = true;
    log_report();
    remount();
    return report_;
}

void EditorProject::log_report() const {
    MCE_LOG_INFO("editor: cook {}: {} cooked, {} up to date, {} failed, {} error(s)", root_.view(), report_.cooked,
                 report_.up_to_date, report_.failed, report_.diags.error_count());
    for (const cook::Diagnostic& d : report_.diags.all()) {
        cook::String line;
        cook::format_diagnostic(d, line);
        if (d.severity == cook::Severity::Error) MCE_LOG_ERROR("{}", line.view());
        else MCE_LOG_WARN("{}", line.view());
    }
}

// Mounts the cooked tree fresh: assets that failed this pass still serve their previous output.
void EditorProject::remount() {
    const cook::String cooked = cook::path_join(root_.view(), "cooked");
    // A mod project mounts its game after its own tree: the mod's assets win, the game's fill in.
    bases_.clear();
    base_views_.clear();
    base_assets_.clear();
    if (cook::is_mod_project(root_.view())) {
        cook::ModProject mod;
        cook::DiagList diags;  // the cook reported these already
        if (cook::read_mod_project(root_.view(), mod, diags) && cook::read_base_assets(mod.base.span(), base_assets_, diags))
            bases_ = std::move(mod.base);
    }
    Array<StringView> roots;
    roots.push_back(cooked.view());
    for (const cook::String& b : bases_) {
        base_views_.push_back(b.view());
        roots.push_back(b.view());
    }
    if (is_file(cook::path_join(cooked.view(), "manifest.mce"))) {
        auto fresh = mem::make_unique<asset::AssetSystem>(mem::heap());
        if (Result<void> r = fresh->mount(roots.span()); r) {
            mounted_ = std::move(fresh);
        } else {
            FixedString<512> msg;
            format_error(r.error(), msg);
            MCE_LOG_ERROR("editor: cannot mount {}: {}", cooked.view(), msg.view());
        }
    }
    ++generation_;
}

HotReloadResult EditorProject::reload(BeforeRemountFn before_remount, void* user) {
    HotReloadResult out;
    if (!is_open()) return out;
    const f64 t0 = platform::now_seconds();
    rescan();
    report_ = service_.cook(root_.view(), cook::CookOptions{});
    cooked_ = true;
    log_report();
    out.failed = report_.failed;

    // Swap in place only when the mount already knows every cooked asset and nothing went away.
    bool same_set = mounted_.get() != nullptr;
    for (const cook::AssetReport& a : report_.assets)
        if (same_set && a.output_hash != 0 && mounted_->find(a.guid) == asset::kInvalidAssetKey) same_set = false;
    for (asset::AssetKey k = 0; same_set && k < mounted_->asset_count(); ++k) {
        bool listed = base_assets_.contains(mounted_->info(k).guid);  // a mod's game asset: not the cook's
        for (const cook::AssetReport& a : report_.assets) listed = listed || a.guid == mounted_->info(k).guid;
        same_set = listed;
    }
    if (!same_set) {
        if (before_remount) before_remount(user);
        remount();
        out.remounted = true;
    } else {
        const cook::String cooked = cook::path_join(root_.view(), "cooked");
        for (const cook::AssetReport& a : report_.assets) {
            if (a.status != cook::AssetStatus::Cooked) continue;
            const FixedString<40> hex = asset::to_hex(a.guid);
            cook::String path = cooked;
            cook::appendf(path, "/%.2s/%s/%016llx.mce", hex.c_str(), hex.c_str(), static_cast<unsigned long long>(a.output_hash));
            Array<byte> bytes;
            Result<void> r = asset::read_file(path.view(), bytes) ? mounted_->commit_reload(a.guid, bytes.span())
                                                                  : Result<void>(Error(Errc::FileNotFound, kStage, path.view()));
            if (r) {
                ++out.reloaded;
                MCE_LOG_INFO("editor: hot reload {}", a.path.view());
            } else {
                ++out.failed;
                FixedString<512> msg;
                format_error(r.error(), msg);
                MCE_LOG_ERROR("editor: hot reload of {} failed: {}", a.path.view(), msg.view());
            }
        }
    }
    out.seconds = platform::now_seconds() - t0;
    return out;
}

void EditorProject::ensure_cooked() {
    if (is_open() && !mounted_) cook(false);
}

}  // namespace mce::editor
