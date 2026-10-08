#include "mce/editor/project.h"

#include "mce/cook/fs.h"
#include "mce/log.h"

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
        default: return "";
    }
}

EditorProject::EditorProject() = default;
EditorProject::~EditorProject() = default;

Result<void> EditorProject::open(StringView root) {
    cook::String r(root);
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

const ProjectAsset* EditorProject::find(asset::Guid g) const {
    for (const ProjectAsset& a : assets_)
        if (a.guid == g) return &a;
    return nullptr;
}

const cook::CookReport& EditorProject::cook(bool pack) {
    if (!is_open()) return report_;
    rescan();
    cook::CookOptions opt;
    opt.pack = pack;
    report_ = service_.cook(root_.view(), opt);
    cooked_ = true;
    MCE_LOG_INFO("editor: cook {}: {} cooked, {} up to date, {} failed, {} error(s)", root_.view(), report_.cooked,
                 report_.up_to_date, report_.failed, report_.diags.error_count());
    for (const cook::Diagnostic& d : report_.diags.all()) {
        cook::String line;
        cook::format_diagnostic(d, line);
        if (d.severity == cook::Severity::Error) MCE_LOG_ERROR("{}", line.view());
        else MCE_LOG_WARN("{}", line.view());
    }
    // Mount the cooked tree fresh: assets that failed this pass still serve their previous output.
    const cook::String cooked = cook::path_join(root_.view(), "cooked");
    if (is_file(cook::path_join(cooked.view(), "manifest.mce"))) {
        auto fresh = mem::make_unique<asset::AssetSystem>(mem::heap());
        const StringView roots[1] = {cooked.view()};
        if (Result<void> r = fresh->mount(Span<const StringView>(roots, 1)); r) {
            mounted_ = std::move(fresh);
        } else {
            FixedString<512> msg;
            format_error(r.error(), msg);
            MCE_LOG_ERROR("editor: cannot mount {}: {}", cooked.view(), msg.view());
        }
    }
    ++generation_;
    return report_;
}

void EditorProject::ensure_cooked() {
    if (is_open() && !mounted_) cook(false);
}

}  // namespace mce::editor
