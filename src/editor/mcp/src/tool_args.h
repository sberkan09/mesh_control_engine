#pragma once
// Argument helpers and small utilities shared by the tool files (tools.cpp, tools_agent_*.cpp).
// Private to mce_mcp_core. Each helper appends to c.error and returns false on a bad argument.

#include "mce/editor/editor_session.h"
#include "mce/input/script.h"
#include "mce/log.h"
#include "mce/mcp/headless_run.h"
#include "tools.h"

namespace mce::mcp {

bool fail(ToolCall& c, StringView msg);
cook::JsonValue arg(const ToolCall& c, const char* name);
bool arg_missing(ToolCall& c, const char* name, const char* expected);
bool arg_string(ToolCall& c, const char* name, StringView& out, bool required);
bool arg_uint(ToolCall& c, const char* name, u64& out, u64 lo, u64 hi, bool required);
bool arg_bool(ToolCall& c, const char* name, bool& out);
editor::EditorScene& scene(ToolCall& c);
// An entity argument: its id (an integer) or its unique name (a string).
bool resolve_entity(ToolCall& c, const char* arg_name, const cook::JsonValue& v, editor::EntityId& out);
bool arg_entity(ToolCall& c, const char* name, editor::EntityId& out);
bool need_project(ToolCall& c);
// A .mcscene path under assets/ ("scenes/level.mcscene"): `abs` receives the file's path.
bool scene_path(Server& server, StringView rel, cook::String& abs, cook::String& err);
void append_diags(cook::String& out, const cook::DiagList& diags, usize max);
const char* level_name(log::Level l);
// Console lines logged after `since`, oldest first.
void read_log(u64 since, Array<log::ConsoleLine>& out);
void write_log_lines(JsonWriter& w, Span<const log::ConsoleLine> lines, usize max);
// "script_errors": [{script, error}] of a headless run whose scene scripts failed; nothing when none did.
void write_script_errors(JsonWriter& w, const HeadlessRun& run);
void progress(Server& server, cook::JsonValue token, u64 done, u64 total);
// run_headless's `input` argument (a script string or an array of segments).
bool arg_input(ToolCall& c, input::InputScript& script);
// The `convars` argument of run_headless and render_frame: {name: value} as console text pairs.
struct ConvarArgs {
    Array<cook::String> names, values;
};
bool arg_convars(ToolCall& c, ConvarArgs& out);
// The same for a value that is not a top-level argument (a .mctest file's "input").
bool input_from(ToolCall& c, const cook::JsonValue& v, input::InputScript& script);

}  // namespace mce::mcp
