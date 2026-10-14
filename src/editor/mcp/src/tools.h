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
