#pragma once
// The MCP tool table and resources (editor-mcp.md §4). Private to mce_mcp_core.

#include "mce/mcp/server.h"

namespace mce::mcp {

struct ToolCall {
    Server& server;
    cook::JsonValue args;            // the "arguments" object (invalid when absent)
    cook::JsonValue progress_token;  // params._meta.progressToken (invalid when absent)
    JsonWriter& out;                 // the structured result: one object
    cook::String& error;             // set when the tool fails
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
const ToolDef* find_tool(StringView name);

// asset_tools.cpp: asset_schema / asset_create / asset_write / asset_read / tilemap_paint.
Span<const ToolDef> asset_tool_table();
// The mce://assets/schema resource: every type asset_schema describes.
void write_asset_schemas(JsonWriter& w);

void list_resources(JsonWriter& w);
// Writes the resources/read result; false when `uri` names no resource.
bool read_resource(Server& server, StringView uri, JsonWriter& w);

}  // namespace mce::mcp

namespace mce::mcp {

// scene_open's work (also --scene at startup): `rel` is a path under assets/.
bool open_scene_file(Server& server, StringView rel, cook::String& err);

}  // namespace mce::mcp
