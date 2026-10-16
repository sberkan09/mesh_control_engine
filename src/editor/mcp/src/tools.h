#pragma once
// The MCP tool table and resources (editor-mcp.md §4). Private to mce_mcp_core.

#include "mce/mcp/server.h"

namespace mce::mcp {

// A content item a tool returns besides its JSON text (MCP "image" content: base64 data + MIME type).
struct ToolContent {
    cook::String mime;    // "image/png"
    cook::String base64;  // the encoded bytes
};

struct ToolCall {
    Server& server;
    cook::JsonValue args;            // the "arguments" object (invalid when absent)
    cook::JsonValue progress_token;  // params._meta.progressToken (invalid when absent)
    JsonWriter& out;                 // the structured result: one object
    cook::String& error;             // set when the tool fails
    Array<ToolContent>* content = nullptr;  // extra content items after the text (images); null: none taken
};

// Returns false (with c.error set) when the call failed: the client sees isError.
using ToolFn = bool (*)(ToolCall& c);

struct ToolDef {
    const char* name;
    const char* description;
    const char* input_schema;  // JSON Schema object, verbatim
    ToolFn fn;
};

// Every tool: the core table (tools.cpp) followed by each group's table. A group lives in its own
// file and adds one line to tool_table(), so groups never edit each other's tables.
Span<const ToolDef> tool_table();
// tools_agent.cpp: the M-AGENT tools (render_frame, render_sheet, image_diff, ...).
Span<const ToolDef> agent_tool_table();
const ToolDef* find_tool(StringView name);

// asset_tools.cpp: asset_schema / asset_create / asset_write / asset_read / tilemap_paint.
Span<const ToolDef> asset_tool_table();
// The mce://assets/schema resource: every type asset_schema describes.
void write_asset_schemas(JsonWriter& w);
// The shared pieces of asset_tools.cpp: the asset named by 'path' or 'guid', a path under assets/
// as the tools accept it, "guid:<uuid>", and cook-validate-then-write of a JSON source asset.
bool asset_tool_arg(ToolCall& c, const editor::ProjectAsset*& out);
bool asset_rel_valid(StringView rel);
cook::String asset_guid_ref(asset::Guid g);
bool asset_validate_and_write(ToolCall& c, StringView rel, asset::Guid guid, StringView text);
// tools_terrain.cpp (M-TERRAIN 6): terrain_sculpt, terrain_paint, terrain_import, terrain_generate.
Span<const ToolDef> terrain_tool_table();
// tools_ui.cpp (M-UI-DATA step 5): ui_list, ui_create, ui_widget_add / set / remove, ui_preview.
Span<const ToolDef> ui_tool_table();
// The UI document named by 'document' (a .mcui path or "guid:<uuid>"), read and checked.
bool ui_document_arg(ToolCall& c, const editor::ProjectAsset*& out);
// tools_render.cpp: ui_preview's capture (a player run with -ui_preview).
bool ui_preview(ToolCall& c);
// tools_timeline.cpp (M-CINEMA step 6): timeline_create, timeline_add_track, timeline_add_key.
Span<const ToolDef> timeline_tool_table();
// tools_script.cpp (M-SCRIPT 4): script_check; empty where the build has no scripting.
Span<const ToolDef> script_tool_table();
// tools_skeleton2d.cpp (M-ANIM2D step 5): skel2d_create, skel2d_add_bone, skel2d_add_slot, skel2d_add_ik, skel2d_key, skel2d_preview.
Span<const ToolDef> skeleton2d_tool_table();
// prefab_tools.cpp: prefab_create_from / prefab_apply / prefab_revert.
Span<const ToolDef> prefab_tool_table();
// project_tools.cpp: project_get / project_set / scene_settings_get / scene_settings_set.
Span<const ToolDef> project_tool_table();

// Argument helpers the groups share (tools.cpp): an entity by id or unique name, a component type
// by name. False with c.error set.
bool tool_arg_entity(ToolCall& c, const char* name, editor::EntityId& out);
bool tool_arg_component(ToolCall& c, const char* name, const refl::TypeRecord*& out);

void list_resources(JsonWriter& w);
// Writes the resources/read result; false when `uri` names no resource.
bool read_resource(Server& server, StringView uri, JsonWriter& w);

}  // namespace mce::mcp

namespace mce::mcp {

// scene_open's work (also --scene at startup): `rel` is a path under assets/.
bool open_scene_file(Server& server, StringView rel, cook::String& err);

}  // namespace mce::mcp
