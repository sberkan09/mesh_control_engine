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

// Every tool: the MVP table (tools.cpp) followed by the M-AGENT tools (tools_agent.cpp).
Span<const ToolDef> tool_table();
Span<const ToolDef> agent_tool_table();
const ToolDef* find_tool(StringView name);

void list_resources(JsonWriter& w);
// Writes the resources/read result; false when `uri` names no resource.
bool read_resource(Server& server, StringView uri, JsonWriter& w);

}  // namespace mce::mcp

namespace mce::mcp {

// scene_open's work (also --scene at startup): `rel` is a path under assets/.
bool open_scene_file(Server& server, StringView rel, cook::String& err);

}  // namespace mce::mcp
