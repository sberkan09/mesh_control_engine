#pragma once
// Asset file operations the Assets panel offers (M-ASSETNEW): create a starter file of a JSON source kind, duplicate an
// asset under a fresh GUID, delete an asset with its sidecar. A new or copied native document is cooked in memory
// before it is written (validate_and_save_source), so a failure writes nothing. Every call rescans the catalog:
// `ProjectAsset*` pointers are invalid afterwards. Headless, tested in CI.

#include "mce/cook/string.h"
#include "mce/editor/asset_kinds.h"
#include "mce/editor/project.h"

namespace mce::editor {

// A file path under assets/ as the operations accept it: forward slashes, no "." / ".." segments, no drive or leading
// slash, and a name that ends in an extension.
bool asset_path_valid(StringView rel);

// Creates assets/<rel> from the starter object of the kind its extension names (asset_kinds()). Fails when the path
// is invalid, the extension names no kind, or a file is there already. `out` is the new asset's GUID.
bool create_asset(EditorProject& project, StringView rel, asset::Guid& out, cook::String& err);

// Copies the asset to assets/<new_rel> (the same extension) under a fresh GUID; a source with a sidecar is copied
// with its sidecar. References inside the copy to the old GUID are repointed to the new one.
bool duplicate_asset(EditorProject& project, asset::Guid source, StringView new_rel, asset::Guid& out, cook::String& err);

// Deletes the asset file and its sidecar. Other assets that name it by GUID keep a dangling reference, which the cook
// reports: references_to lists them first.
bool delete_asset(EditorProject& project, asset::Guid guid, cook::String& err);

// The paths (relative to assets/) of the other assets whose file text names `guid`.
void references_to(const EditorProject& project, asset::Guid guid, Array<cook::String>& out);

}  // namespace mce::editor
