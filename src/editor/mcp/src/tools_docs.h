#pragma once
// The docs, prompts and capabilities side of the M-AGENT tools (tools_docs.cpp). Private to
// mce_mcp_core: the server's resources/list, resources/read, resources/templates/list,
// prompts/list and prompts/get call these.

#include "tools.h"

namespace mce::mcp {

void docs_list_resources(Server& server, JsonWriter& w);
void docs_list_templates(JsonWriter& w);
// A mce://docs/... resource: false when `uri` is not one this server serves.
bool docs_read_resource(Server& server, StringView uri, cook::String& text, const char*& mime);

// The folder the shipped docs, samples, example projects or content library live in ("docs",
// "samples", "examples", "library"): installed beside the server, or the source tree's. Empty for
// another group.
cook::String content_root(const Server& server, StringView group);

void prompts_list(JsonWriter& w);
// The prompts/get result; false (with `err`) for an unknown prompt or a missing argument.
bool prompts_get(StringView name, cook::JsonValue args, JsonWriter& w, cook::String& err);

}  // namespace mce::mcp
