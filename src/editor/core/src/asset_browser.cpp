#include "mce/editor/asset_browser.h"

#include <algorithm>

#include "mce/ecs/transform.h"
#include "mce/kit/components.h"

namespace mce::editor {

namespace {

bool contains_ci(StringView hay, StringView needle) {
    if (needle.empty()) return true;
    for (usize i = 0; i + needle.size() <= hay.size(); ++i) {
        usize k = 0;
        for (; k < needle.size(); ++k) {
            char a = hay[i + k], b = needle[k];
            if (a >= 'A' && a <= 'Z') a = static_cast<char>(a - 'A' + 'a');
            if (b >= 'A' && b <= 'Z') b = static_cast<char>(b - 'A' + 'a');
            if (a != b) break;
        }
        if (k == needle.size()) return true;
    }
    return false;
}

// The file name without its extension: the name a dropped entity gets.
StringView stem(StringView rel_path) {
    StringView f = asset_file_name(rel_path);
    const usize dot = f.rfind('.');
    return dot == StringView::npos || dot == 0 ? f : f.substr(0, dot);
}

}  // namespace

StringView asset_file_name(StringView rel_path) {
    const usize slash = rel_path.rfind('/');
    return slash == StringView::npos ? rel_path : rel_path.substr(slash + 1);
}

StringView asset_folder(StringView rel_path) {
    const usize slash = rel_path.rfind('/');
    return slash == StringView::npos ? StringView() : rel_path.substr(0, slash);
}

void AssetBrowserModel::refresh(const EditorProject& project) {
    folders_.clear();
    folders_.push_back(cook::String());
    for (const ProjectAsset& a : project.assets()) {
        // The asset's folder and every folder above it.
        for (StringView f = asset_folder(a.path.view()); !f.empty(); f = asset_folder(f)) {
            bool known = false;
            for (const cook::String& k : folders_) known = known || k.view() == f;
            if (!known) folders_.push_back(cook::String(f));
        }
    }
    std::sort(folders_.begin(), folders_.end());
    bool still = false;
    for (const cook::String& k : folders_) still = still || k.view() == folder_.view();
    if (!still) folder_.clear();
}

void AssetBrowserModel::subfolders(StringView parent, Array<StringView>& out) const {
    for (const cook::String& f : folders_)
        if (!f.view().empty() && asset_folder(f.view()) == parent) out.push_back(f.view());
}

void AssetBrowserModel::entries(const EditorProject& project, Array<const ProjectAsset*>& out) const {
    for (const ProjectAsset& a : project.assets()) {
        if (!type_.empty() && type_.view() != StringView(asset_type_name(a.type))) continue;
        if (filter_.empty() ? asset_folder(a.path.view()) != folder_.view() : !contains_ci(a.path.view(), filter_.view()))
            continue;
        out.push_back(&a);
    }
    std::sort(out.begin(), out.end(), [](const ProjectAsset* x, const ProjectAsset* y) { return x->path < y->path; });
}

AssetDrop asset_drop_kind(asset::ChunkType type) {
    switch (type) {
        case asset::ChunkType::Texture: return AssetDrop::Sprite;
        case asset::ChunkType::Prefab: return AssetDrop::PrefabInstance;
        case asset::ChunkType::Scene: return AssetDrop::OpenScene;
        default: return AssetDrop::None;
    }
}

EntityId drop_asset(EditorScene& scene, const ProjectAsset& asset, math::vec2 world, EntityId parent) {
    const AssetDrop kind = asset_drop_kind(asset.type);
    if (kind != AssetDrop::Sprite && kind != AssetDrop::PrefabInstance) return kNoEntity;
    scene.begin_edit(kind == AssetDrop::Sprite ? "Add sprite" : "Instantiate prefab");
    const StringView name = stem(asset.path.view());
    EntityId id = kNoEntity;
    if (kind == AssetDrop::Sprite) {
        id = scene.create_entity(name);
        scene.add_component(id, refl::type_of<kit::Sprite>());
        kit::Sprite s = *scene.get<kit::Sprite>(id);
        s.texture.guid = asset.guid;
        scene.set(id, s);
    } else {
        id = scene.create_prefab_instance(name, asset.guid);
    }
    LocalTransform t = *scene.get<LocalTransform>(id);
    t.position = {world.x, world.y, 0.0f};
    scene.set(id, t);
    if (parent != kNoEntity) scene.set_parent(id, parent);
    scene.end_edit();
    scene.selection().set(id);
    return id;
}

}  // namespace mce::editor
