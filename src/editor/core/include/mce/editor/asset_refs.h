#pragma once
// AssetRefs — who names whom (M-ASSETREFS): one pass over the project's text assets that finds every GUID written in
// them (a "guid:<uuid>" reference or a bare uuid) and keeps, per asset, the assets that name it. The Assets panel asks
// it "Used by" for the context menu and "Unused" for its filter. Files over 8 MB and binary imports are not read, and
// a scene's start scene, a script that loads by path, or anything outside assets/ is not seen, so "unused" means
// "no asset file names it", not "safe to delete". Headless, tested in CI.

#include "mce/cook/string.h"
#include "mce/containers/flat_map.h"
#include "mce/editor/project.h"

namespace mce::editor {

class AssetRefs {
public:
    // Reads every asset of the project; replaces the previous result.
    void build(const EditorProject& project);

    // The paths (relative to assets/) of the other assets that name `guid`, in catalog order.
    void used_by(asset::Guid guid, Array<cook::String>& out) const;
    u32 use_count(asset::Guid guid) const;
    // The assets nothing names. Scenes are left out (the project names its start scene outside assets/, and a scene is
    // opened, not referenced).
    void unused(Array<asset::Guid>& out) const;

private:
    struct Node {
        cook::String path;
        asset::ChunkType type = asset::ChunkType::Manifest;
        Array<u32> users;  // indices into nodes_
    };
    Array<Node> nodes_;
    FlatMap<asset::Guid, u32> index_;
};

}  // namespace mce::editor
