#pragma once
// The asset browser's model (ROADMAP M6, editor-tools.md §2.5, ADR-0030 §7): the project catalog
// as a folder tree, the entries of one folder filtered by name and type, and what dropping an
// asset into the scene makes. Headless (tested in CI); the ImGui panel draws it and turns
// drag-drop and the rename field into calls here and on EditorSession::rename_asset.

#include "mce/editor/editor_scene.h"
#include "mce/editor/project.h"
#include "mce/math/math.h"

namespace mce::editor {

class AssetBrowserModel {
public:
    // Rebuilds the folder list from the catalog (call after a rescan); keeps the current folder
    // when it still exists.
    void refresh(const EditorProject& project);
    // Every folder under assets/ that holds an asset (or a folder that does), sorted; "" = root.
    Span<const cook::String> folders() const { return folders_.span(); }
    // Direct subfolders of `parent` (full paths), sorted.
    void subfolders(StringView parent, Array<StringView>& out) const;

    StringView folder() const { return folder_.view(); }
    void set_folder(StringView folder) { folder_ = cook::String(folder); }
    // Case-insensitive name filter; non-empty searches every folder, not just the current one.
    void set_filter(StringView text) { filter_ = cook::String(text); }
    StringView filter() const { return filter_.view(); }
    // Only this type ("Texture", "Prefab", ...); empty: every type.
    void set_type(StringView type) { type_ = cook::String(type); }

    // The current folder's assets (or the whole catalog's matches), sorted by path.
    void entries(const EditorProject& project, Array<const ProjectAsset*>& out) const;

private:
    Array<cook::String> folders_;
    cook::String folder_, filter_, type_;
};

// The file name of a catalog path ("textures/crate.png" -> "crate.png") and its folder.
StringView asset_file_name(StringView rel_path);
StringView asset_folder(StringView rel_path);

// What dropping an asset on the viewport or the hierarchy does.
enum class AssetDrop : u8 {
    None,
    Sprite,          // a texture: a new entity with a Sprite showing it
    PrefabInstance,  // a prefab: a new instance
    OpenScene,       // a scene: opens it (the shell asks about unsaved changes first)
};
AssetDrop asset_drop_kind(asset::ChunkType type);

// Creates what a Sprite / PrefabInstance drop makes at `world` (scene space), under `parent` when
// given, named after the file, selected; one undo entry. kNoEntity for other kinds.
EntityId drop_asset(EditorScene& scene, const ProjectAsset& asset, math::vec2 world, EntityId parent = kNoEntity);

}  // namespace mce::editor
