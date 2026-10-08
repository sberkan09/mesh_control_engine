#pragma once
// Component values <-> JSON for the MCP tools (editor-mcp.md §4). The spelling is the .mcscene one
// (mce/cook/scene_text.h: the same walker reads it, the same writer emits it), with one difference:
// entity references are "$<EntityId>" (the document id the tools use everywhere), not the file
// index. Values are applied to a caller-owned copy; the caller commits it through EditorScene.

#include "mce/editor/scene_document.h"
#include "mce/mcp/json_writer.h"

namespace mce::mcp {

// The non-default, non-Transient fields of `value` as one JSON object.
void write_component(const refl::TypeRecord& type, const void* value, cook::String& out);

// Applies an object of fields onto `value` (omitted fields keep their current value). On failure
// returns false with the reason in `err`; `value` may then be partially written.
bool apply_patch(const refl::TypeRecord& type, cook::JsonValue patch, void* value, const editor::SceneDocument& doc,
                 cook::String& err);

// Sets the property at a refl property path ("shape.half", "position.x", "waypoints[2]") to `v`.
bool apply_path(const refl::TypeRecord& type, StringView path, cook::JsonValue v, void* value,
                const editor::SceneDocument& doc, cook::String& err);

// {"name", "size", "fields": [{"name", "kind", ...}]}: what refl_describe_component returns.
void describe_type(JsonWriter& w, const refl::TypeRecord& type);

}  // namespace mce::mcp
