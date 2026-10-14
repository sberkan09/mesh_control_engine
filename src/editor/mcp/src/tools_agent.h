#pragma once
// The M-AGENT tools (ROADMAP M-AGENT: human-free development), one function per tool. Their table
// is agent_tool_table() in tools_agent.cpp; tool_table() lists them after the MVP tools.

#include "tools.h"

namespace mce::mcp {

// tools_render.cpp (SBA-48): frames from the real player, contact sheets, golden diffs.
bool render_frame(ToolCall& c);
bool render_sheet(ToolCall& c);
bool image_diff(ToolCall& c);

}  // namespace mce::mcp
